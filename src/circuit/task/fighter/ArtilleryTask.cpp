/*
 * ArtilleryTask.cpp
 *
 *  Created on: Jan 6, 2016
 *      Author: rlcevg
 */

#include "task/fighter/ArtilleryTask.h"
#include "task/fighter/AttackTask.h"
#include "task/TaskManager.h"
#include "map/ThreatMap.h"
#include "module/MilitaryManager.h"
#include "setup/SetupManager.h"
#include "terrain/TerrainManager.h"
#include "terrain/path/PathFinder.h"
#include "terrain/path/QueryPathSingle.h"
#include "terrain/path/QueryPathMulti.h"
#include "unit/action/FightAction.h"
#include "unit/action/MoveAction.h"
#include "unit/ally/AllyUnit.h"
#include "unit/enemy/EnemyUnit.h"
#include "unit/CircuitUnit.h"
#include "CircuitAI.h"
#include "util/Utils.h"

#include "spring/SpringCallback.h"
#include "spring/SpringMap.h"

#include "AISCommands.h"

#include <limits>
#include <tuple>

namespace circuit {

using namespace springai;

static bool IsNearAny(const AIFloat3& pos, const F3Vec& points, float radius)
{
	const float sqRadius = SQUARE(radius);
	for (const AIFloat3& p : points) {
		if (pos.SqDistance2D(p) <= sqRadius) {
			return true;
		}
	}
	return false;
}

// Scratch shared by all artillery tasks: only used within one synchronous call (path queries copy their goals)
F3Vec CArtilleryTask::safePositions;
F3Vec CArtilleryTask::firePositions;
std::vector<CEnemyInfo*> CArtilleryTask::candidates;
std::vector<CArtilleryTask::SFrontEnemy> CArtilleryTask::frontEnemies;
F3Vec CArtilleryTask::frontGoals;
F3Vec CArtilleryTask::crowd;
std::vector<CEnemyInfo*> CArtilleryTask::ignoredQuarries;
std::vector<CArtilleryTask::SEscort> CArtilleryTask::escorts;
std::vector<CArtilleryTask::SDanger> CArtilleryTask::dangers;
std::vector<float> CArtilleryTask::coverGrid;
std::vector<float> CArtilleryTask::fleeCoverGrid;
std::vector<char> CArtilleryTask::cellAdded;

CArtilleryTask::CArtilleryTask(ITaskManager* mgr, bool isAntiHeavy)
		: IFighterTask(mgr, FightType::ARTY, 1.f)
		, isAntiHeavy(isAntiHeavy)
{
	position = manager->GetCircuit()->GetSetupManager()->GetBasePos();
	// Anti-heavy: no firing spot it allows (IsFireSpotAllowed) = shoot from where it stands, never walk in to one
	isHoldAttack = isAntiHeavy;
}

CArtilleryTask::~CArtilleryTask()
{
}

bool CArtilleryTask::CanAssignTo(CCircuitUnit* unit) const
{
	CCircuitDef* cdef = unit->GetCircuitDef();
	return units.empty()
			&& (isAntiHeavy ? manager->GetCircuit()->GetMilitaryManager()->IsAhArty(cdef) : cdef->IsRoleArty());
}

void CArtilleryTask::AssignTo(CCircuitUnit* unit)
{
	IFighterTask::AssignTo(unit);

	unit->GetUnit()->SetMoveState(0);

	int squareSize = manager->GetCircuit()->GetPathfinder()->GetSquareSize();
	CCircuitDef* cdef = unit->GetCircuitDef();
	ITravelAction* travelAction;
	if (cdef->IsAttrSiege()) {
		travelAction = new CFightAction(unit, squareSize);
	} else {
		travelAction = new CMoveAction(unit, squareSize);
	}
	unit->PushTravelAct(travelAction);
	travelAction->StateWait();
	unit->SetAllowedToJump(cdef->IsAbleToJump() && cdef->IsAttrJump());
}

void CArtilleryTask::RemoveAssignee(CCircuitUnit* unit)
{
	IFighterTask::RemoveAssignee(unit);
	if (units.empty()) {
		manager->AbortTask(this);
	}

	unit->GetUnit()->SetMoveState(1);
}

void CArtilleryTask::Start(CCircuitUnit* unit)
{
	if (!CheckFlee(unit)) {  // a (re)started task must not walk towards an enemy first
		Execute(unit, false);
	}
}

void CArtilleryTask::Update()
{
	const int frame = manager->GetCircuit()->GetLastFrame();
	const bool isTick = (++updCount % 4 == 0);
	for (CCircuitUnit* unit : units) {
		if (CheckFlee(unit)) {  // every update: run from approaching enemies before anything else
			shotFrame = -1;
			isScooting = false;  // the flee has its own path
			continue;
		}
		if (UpdateScoot(unit, frame)) {
			continue;
		}
		if (isTick || unit->IsForceUpdate(frame)) {
			Execute(unit, true);
		}
	}
}

void CArtilleryTask::OnUnitIdle(CCircuitUnit* unit)
{
	IFighterTask::OnUnitIdle(unit);  // a coward leaves for its retreat
	if (units.find(unit) == units.end()) {
		return;
	}
	// Anti-heavy mode: our own Stops (StopAttack, HoldHere) and a finished attack make the unit idle. Keep the task
	// (route, safety walk, flee timer) and decide again on the next Update, not from within the engine's idle event
	if (isAntiHeavy) {
		unit->ForceUpdate(manager->GetCircuit()->GetLastFrame());
		return;
	}
	RemoveAssignee(unit);
}

const CMilitaryManager::SArtilleryCfg& CArtilleryTask::GetCfg() const
{
	return manager->GetCircuit()->GetMilitaryManager()->GetArtilleryCfg();
}

float CArtilleryTask::GetMaxPathThreat(CCircuitUnit* unit) const
{
	// max_path_threat x own power (pwr_mod, not thr_mod: thr_mod shapes how scary a type looks as an enemy
	// and is often ~0 for artillery)
	const float maxPathThreat = GetCfg().maxPathThreat;
	return (maxPathThreat >= 0.f)
			? std::max(maxPathThreat * unit->GetCircuitDef()->GetPower(), THREAT_MIN)
			: std::numeric_limits<float>::max();
}

float CArtilleryTask::GetFleeMargin(CCircuitUnit* unit) const
{
	return unit->GetCircuitDef()->GetArtyFleeMargin(GetCfg().fleeMargin);
}

/*
 * Shooting and walking to firing spots
 */

void CArtilleryTask::Execute(CCircuitUnit* unit, bool isUpdating)
{
	if (unit->Blocker() != nullptr) {
		return;  // Do not interrupt current action
	}

	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	const AIFloat3& pos = unit->GetPos(frame);
	const CMilitaryManager::SArtilleryCfg& artyCfg = GetCfg();
	const bool isNeedCover = (artyCfg.allyThreat >= 0.f);
	CEnemyInfo* bestTarget = FindTarget(unit, pos);

	if (bestTarget != nullptr) {
		// ally_threat: don't stay behind shooting once the escort has moved on
		if (isNeedCover && !HasCover(unit, pos)) {
			SetTarget(nullptr);
			StopAttack(unit);
			if (!IsQueryReady(unit)) {
				return;
			}
			if (isAntiHeavy) {
				WalkAntiHeavy(unit, GetWalkRange(unit), isUpdating);  // a covered spot nearby, else the army
			} else {
				FallbackNoSpot(unit, isUpdating);
			}
			return;
		}
		const bool wasAttacking = isAttacking;
		isAttacking = true;
		attackPos = bestTarget->GetPos();
		if (isAntiHeavy) {
			// The attack task's firing spots: the cheapest allowed spot (threat, walk) at ~90% of our range, re-picked
			// every 3 s and when the heavy moves a tile, so the unit backs off as it closes in; none: from here
			SetTarget(bestTarget);  // also for the other anti-heavy units' overkill check (GetAhReadyDamage)
			if (!wasAttacking) {
				unit->ClearTarget();  // our own Stop, scoot or flee came in between: order at once
			}
			Attack(frame, true);
			return;
		}
		unit->GetTravelAct()->StateWait();
		TRY_UNIT(circuit, unit,
			unit->GetUnit()->Attack(bestTarget->GetUnit(), UNIT_COMMAND_OPTION_RIGHT_MOUSE_KEY, frame + FRAMES_PER_SEC * 60);
			unit->CmdSetTarget(bestTarget);
		)
		return;
	}
	SetTarget(nullptr);
	StopAttack(unit);

	if (!IsQueryReady(unit)) {
		return;
	}

	const float range = GetWalkRange(unit);
	if (isAntiHeavy) {
		WalkAntiHeavy(unit, range, isUpdating);
		return;
	}

	if (enemyPositions.empty()) {
		FallbackNoTarget(unit, isUpdating);
		return;
	}

	// Walking to a firing spot: cheap checks on every Execute, a full search and path query every 6 s
	if (IsFireRouteValid(unit, range, enemyPositions)) {
		return;
	}

	// Goals: any cell within range of a building (old), or only covered firing cells (ally_threat)
	if (!isNeedCover) {
		RunFireQuery(unit, range, enemyPositions, TARGET, isUpdating);
		return;
	}
	const float goalRange = FillCoveredSpots(unit, range, enemyPositions);
	if (firePositions.empty()) {
		FallbackNoSpot(unit, isUpdating);
		return;
	}
	RunFireQuery(unit, goalRange, firePositions, TARGET, isUpdating);
}

void CArtilleryTask::RunFireQuery(CCircuitUnit* unit, float goalRange, const F3Vec& goals, Goal goal, bool isUpdating)
{
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	CPathFinder* pathfinder = circuit->GetPathfinder();
	std::shared_ptr<IPathQuery> query = pathfinder->CreatePathMultiQuery(
			unit, circuit->GetThreatMap(), frame,
			unit->GetPos(frame), goalRange, goals, nullptr, GetMaxPathThreat(unit));
	pathQueries[unit] = query;
	query->HoldTask(this);

	pathfinder->RunQuery(query, [this, isUpdating, goal](const IPathQuery* query) {
		if (this->IsQueryAlive(query)) {
			this->ApplyTargetPath(static_cast<const CQueryPathMulti*>(query), isUpdating, goal);
		}
	});
}

float CArtilleryTask::GetWalkRange(CCircuitUnit* unit) const
{
	return std::max(unit->GetCircuitDef()->GetMaxRange(), (float)manager->GetCircuit()->GetThreatMap()->GetSquareSize());
}

void CArtilleryTask::WalkAntiHeavy(CCircuitUnit* unit, float range, bool isUpdating)
{
	// Heavies first: a covered spot by one, from where the unit shoots it once its weapon is ready and the heavy in
	// LOS. Else the front: a covered spot in range of an enemy near our army, rather than the base, which nearly always
	// has cover. Else behind the army
	FillSafeSpots(unit);  // where the army is
	const F3Vec& front = FillFrontGoals(range);
	if (IsFireRouteValid(unit, range, (FRONT == routeGoal) ? front : enemyPositions)) {
		return;
	}
	std::optional<bool> isHereSpot;  // both steps may ask whether here is a firing spot: worked out once
	if (SeekFireSpot(unit, range, enemyPositions, TARGET, isHereSpot, isUpdating)
		|| SeekFireSpot(unit, range, front, FRONT, isHereSpot, isUpdating))
	{
		return;
	}
	FallbackStay(unit, isUpdating);
}

bool CArtilleryTask::SeekFireSpot(CCircuitUnit* unit, float range, const F3Vec& goals, Goal goal,
		std::optional<bool>& isHereSpot, bool isUpdating)
{
	if (goals.empty()) {
		return false;
	}
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	const AIFloat3& pos = unit->GetPos(frame);

	// Already at one: hold (a route there would be a point or two, finish at once and make the unit idle). The walk
	// goes 2 cells inside range (FillCoveredSpots), so an enemy must move that far back before it walks again
	if (IsNearAny(pos, goals, range)) {
		if (!isHereSpot.has_value()) {
			isHereSpot = IsFireSpotValid(unit, pos, GetNetThreatAt(unit, pos));
		}
		if (*isHereSpot) {
			HoldHere(unit);
			return true;
		}
	}

	// The search sweeps cells around every escort: after one that found nothing, not again for 6 s (the re-plan's pace)
	if (frame < searchFrame[goal]) {
		return false;
	}
	if (GetCfg().allyThreat < 0.f) {
		RunFireQuery(unit, range, goals, goal, isUpdating);  // cover off: any cell in range
		return true;
	}
	const float goalRange = FillCoveredSpots(unit, range, goals);
	if (firePositions.empty()) {
		searchFrame[goal] = frame + FRAMES_PER_SEC * 6;
		return false;
	}
	RunFireQuery(unit, goalRange, firePositions, goal, isUpdating);
	return true;
}

const F3Vec& CArtilleryTask::FillFrontGoals(float range)
{
	frontGoals.clear();
	const CMilitaryManager::SArtilleryCfg& artyCfg = GetCfg();
	if (artyCfg.allyThreat < 0.f) {
		return frontGoals;  // cover off: no front step, it would walk to any enemy
	}
	const float reach = range + artyCfg.allyRadius;
	bool isLos = false;
	for (const SFrontEnemy& enemy : frontEnemies) {
		if ((isLos && !enemy.isLos) || !IsNearAny(enemy.pos, safePositions, reach)) {
			continue;
		}
		if (enemy.isLos && !isLos) {
			frontGoals.clear();  // the first one in LOS: radar dots no longer count
			isLos = true;
		}
		frontGoals.push_back(enemy.pos);
	}
	return frontGoals;
}

CEnemyInfo* CArtilleryTask::FindTarget(CCircuitUnit* unit, const AIFloat3& pos)
{
	if (isAntiHeavy) {
		return FindHeavyTarget(unit, pos);
	}
	// Known enemy buildings only. Standing safe: return the best one in range, else fill enemyPositions with
	// those out of range (to walk to). Standing in threat: never shoot, only fill enemyPositions
	CCircuitAI* circuit = manager->GetCircuit();
	CThreatMap* threatMap = circuit->GetThreatMap();
	CCircuitDef* cdef = unit->GetCircuitDef();
	const bool notAW = !cdef->HasAntiWater();
	const int canTargetCat = cdef->GetTargetCategory();
	const int noChaseCat = cdef->GetNoChaseCategory();
	const float sqRange = SQUARE(cdef->GetMaxRange());

	enemyPositions.clear();
	threatMap->SetThreatType(unit);
	const bool isPosSafe = (threatMap->GetThreatAt(pos) <= THREAT_MIN) || cdef->IsPlane();

	// Трубка 15, прицел 120, бац, бац …и мимо!
	// In range: any building before a shut pop-up turret (it takes a third of the damage); then builders (nano
	// towers, factories), armed buildings by power, unarmed ones, no-chase ones; the nearest last
	const int frame = circuit->GetLastFrame();
	using Rank = std::tuple<bool, int, float, float>;
	Rank bestRank;
	CEnemyInfo* bestTarget = nullptr;
	for (auto& kv : circuit->GetEnemyInfos()) {
		CEnemyInfo* enemy = kv.second;
		if (!enemy->IsInRadarOrLOS() || (notAW && (enemy->GetPos().y < -SQUARE_SIZE * 5))) {
			continue;
		}
		CCircuitDef* edef = enemy->GetCircuitDef();
		if ((edef == nullptr) || edef->IsMobile() || (isPosSafe && edef->IsAttrSiege())) {
			continue;
		}
		const int targetCat = edef->GetCategory();
		if ((targetCat & canTargetCat) == 0) {
			continue;
		}

		const float sqDist = pos.SqDistance2D(enemy->GetPos());
		if (sqDist < sqRange) {
			if (!isPosSafe) {
				continue;
			}
			// builders (all unarmed) 0, armed 1, unarmed 2, no-chase 3
			const float power = edef->GetPower();
			const int tier = edef->IsEnemyRoleAny(CCircuitDef::RoleMask::BUILDER) ? 0
					: (power > 0.f) ? 1 : ((targetCat & noChaseCat) == 0) ? 2 : 3;
			const Rank rank(IsPopupShut(enemy, frame), tier, -power, sqDist);
			if ((bestTarget == nullptr) || (rank < bestRank)) {
				bestRank = rank;
				bestTarget = enemy;
			}
			continue;
		}

		if ((targetCat & noChaseCat) == 0) {
			enemyPositions.push_back(enemy->GetPos());
		}
	}

	if (bestTarget != nullptr) {
		position = bestTarget->GetPos();
	}
	return bestTarget;
}

CEnemyInfo* CArtilleryTask::FindHeavyTarget(CCircuitUnit* unit, const AIFloat3& pos)
{
	// The heavies and commanders anti-heavy units hunt (CCircuitDef::IsQuarry), mobile or not. Shoots only where it
	// stands is fine (IsThreatOk), the threat of the heavies it may shoot now aside (IsQuarryIgnored): the best one in
	// range, in LOS for a long reload, that the other anti-heavy units' ready shots won't kill. enemyPositions: all of
	// them, in range too (one we can't shoot from here may be shot from a covered spot nearby), mobile or not, to walk
	// to; the route query takes the nearest firing spot. frontEnemies: every enemy it can shoot, aircraft aside
	CCircuitAI* circuit = manager->GetCircuit();
	CMilitaryManager* militaryMgr = circuit->GetMilitaryManager();
	CCircuitDef* cdef = unit->GetCircuitDef();
	const bool notAW = !cdef->HasAntiWater();
	const int canTargetCat = cdef->GetTargetCategory();
	const int noChaseCat = cdef->GetNoChaseCategory();
	const float sqRange = SQUARE(cdef->GetMaxRange());
	const bool isSeenOnly = cdef->IsLongReload();  // fires at what it sees, not at a radar blip
	const float shotDamage = cdef->GetShotDamage();

	enemyPositions.clear();
	candidates.clear();
	ignoredQuarries.clear();
	frontEnemies.clear();
	for (auto& kv : circuit->GetEnemyInfos()) {
		CEnemyInfo* enemy = kv.second;
		if (enemy->IsHidden() || !enemy->IsInRadarOrLOS() || (notAW && (enemy->GetPos().y < -SQUARE_SIZE * 5))) {
			continue;
		}
		CCircuitDef* edef = enemy->GetCircuitDef();
		if ((edef != nullptr) && !edef->IsAbleToFly()
			&& ((edef->GetCategory() & canTargetCat) != 0) && ((edef->GetCategory() & noChaseCat) == 0))
		{
			frontEnemies.push_back({enemy->GetPos(), enemy->IsInLOS()});  // any enemy it can shoot: FillFrontGoals
		}
		if (!cdef->IsQuarry(edef)) {
			continue;
		}
		if (IsQuarryIgnored(unit, enemy)) {
			ignoredQuarries.push_back(enemy);
		}
		if ((pos.SqDistance2D(enemy->GetPos()) < sqRange) && (!isSeenOnly || enemy->IsInLOS())) {
			candidates.push_back(enemy);
		}
		// A long reload fires only at what it sees: a static heavy on radar alone stays unseen however long the unit
		// waits in range, so it is left to the front step (a radar dot there)
		if (((edef->GetCategory() & noChaseCat) == 0) && (!isSeenOnly || enemy->IsInLOS() || edef->IsMobile())) {
			enemyPositions.push_back(enemy->GetPos());
		}
	}
	if (!cdef->IsPlane() && !IsThreatOk(unit, pos, GetNetThreatAt(unit, pos), false)) {
		return nullptr;
	}

	// Mobile first, then one this shot kills, commanders (builders), the strongest, the nearest
	using Rank = std::tuple<bool, bool, bool, float, float>;
	Rank bestRank;
	CEnemyInfo* bestTarget = nullptr;
	for (CEnemyInfo* enemy : candidates) {
		const float health = enemy->GetHealth() + enemy->GetShieldPower();
		if (militaryMgr->GetAhReadyDamage(enemy, this) > 1.1f * health) {
			continue;  // the others' ready shots kill it
		}
		CCircuitDef* edef = enemy->GetCircuitDef();
		const Rank rank(!edef->IsMobile(), health > shotDamage, !edef->IsEnemyRoleAny(CCircuitDef::RoleMask::BUILDER),
				-edef->GetPower(), pos.SqDistance2D(enemy->GetPos()));
		if ((bestTarget == nullptr) || (rank < bestRank)) {
			bestRank = rank;
			bestTarget = enemy;
		}
	}
	if (bestTarget != nullptr) {
		position = bestTarget->GetPos();
	}
	return bestTarget;
}

bool CArtilleryTask::IsPopupShut(CEnemyInfo* enemy, int frame) const
{
	CCircuitDef* edef = enemy->GetCircuitDef();
	if (!edef->IsPopup() || enemy->IsBeingBuilt()) {
		return false;  // a nanoframe is never armoured
	}
	// Desolator, Lucifer: armoured only while switched off (lowpower stalls them, doesn't close them); readable
	// only in LOS, else open (activateWhenBuilt)
	if (edef->IsPopupSwitch()) {
		return enemy->IsInLOS() && !enemy->GetUnit()->IsActivated();
	}
	// Faraday, Gauss, Hacksaw: open while it has a target. They close 2-5 s after the last one, under a unit's
	// re-pick period (every 4th update, ~4 s), so no hold is needed
	return !IsPopupTargetNear(enemy, frame);
}

bool CArtilleryTask::IsPopupTargetNear(CEnemyInfo* enemy, int frame) const
{
	CCircuitAI* circuit = manager->GetCircuit();
	CCircuitDef* edef = enemy->GetCircuitDef();
	const float margin = SQUARE_SIZE * 8;  // it opens a little before a target is in range
	const AIFloat3& ePos = enemy->GetPos();
	const float radius = edef->GetMaxRange() + margin;
	COOAICallback* clb = circuit->GetCallback();
	if (!clb->IsFriendlyUnitsIn(ePos, radius, false)) {
		return false;
	}
	for (int fId : clb->GetFriendlyUnitIdsIn(ePos, radius, false)) {
		if (fId == -1) {
			continue;
		}
		CAllyUnit* ally = circuit->GetFriendlyUnit(fId);
		if ((ally == nullptr) || (ally->GetCircuitDef() == nullptr)) {
			return true;  // not listed yet: assume the turret is open for it
		}
		const AIFloat3& aPos = ally->GetPos(frame);
		const CCircuitDef::RangeType rt = ally->GetCircuitDef()->IsAbleToFly() ? CCircuitDef::RangeType::AIR
				: (aPos.y < -SQUARE_SIZE * 5) ? CCircuitDef::RangeType::WATER : CCircuitDef::RangeType::LAND;
		const float range = edef->GetMaxRange(rt);  // 0: can't hit that layer
		if ((range > 0.f) && (ePos.SqDistance2D(aPos) < SQUARE(range + margin))) {
			return true;
		}
	}
	return false;
}

void CArtilleryTask::ApplyTargetPath(const CQueryPathMulti* query, bool isUpdating, Goal goal)
{
	const std::shared_ptr<PathInfo>& pPath = query->GetPathInfo();
	CCircuitUnit* unit = query->GetUnit();

	if (!pPath->posPath.empty()
		&& IsThreatOk(unit, pPath->posPath.back(),  // this unit's threat layer
				manager->GetCircuit()->GetThreatMap()->GetThreatAt(unit, pPath->posPath.back()), true))
	{
		position = pPath->posPath.back();
		unit->GetTravelAct()->SetPath(pPath);
		unit->GetTravelAct()->StateActivate();
		firePath = pPath;
		routeGoal = goal;
		replanFrame = manager->GetCircuit()->GetLastFrame() + FRAMES_PER_SEC * 6;
	} else if (isAntiHeavy) {
		// No route under the cap: not that kind of goal for 6 s, decide again now
		const int frame = manager->GetCircuit()->GetLastFrame();
		searchFrame[goal] = frame + FRAMES_PER_SEC * 6;
		unit->ForceUpdate(frame);
	} else {
		FallbackNoSpot(unit, isUpdating);
	}
}

bool CArtilleryTask::IsFireRouteValid(CCircuitUnit* unit, float range, const F3Vec& goals)
{
	// Until the next full re-plan, keep walking while the firing spot still is one (in range of a known building,
	// IsFireSpotValid) and the waypoints left stay under max_path_threat. goals: what it was planned for
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	ITravelAction* travelAct = unit->GetTravelAct();
	if ((firePath == nullptr) || (frame >= replanFrame) || (travelAct->GetPath() != firePath)
		|| !(travelAct->IsActive() || travelAct->IsFinished()))  // finished: the last waypoint is ordered, still walking
	{
		return false;
	}
	const F3Vec& route = firePath->posPath;
	const AIFloat3& spot = route.back();

	const float reach = range + 2 * circuit->GetPathfinder()->GetSquareSize();  // with the pathfinder's slack
	if (!IsNearAny(spot, goals, reach) || !IsFireSpotValid(unit, spot, circuit->GetThreatMap()->GetThreatAt(unit, spot))) {
		return false;
	}

	// The waypoints left stay under the cap. Not for a cloaked unit: the pathfinder plans its route on the
	// decloak layer, which GetThreatAt doesn't read (IsCloakRoute)
	const AIFloat3& pos = unit->GetPos(frame);
	const float maxPathThreat = GetMaxPathThreat(unit);
	if ((maxPathThreat < std::numeric_limits<float>::max()) && !IsCloakRoute(unit)) {
		// From the waypoint nearest to us
		size_t next = 0;
		float minSqDist = std::numeric_limits<float>::max();
		for (size_t i = 0; i < route.size(); ++i) {
			const float sqDist = pos.SqDistance2D(route[i]);
			if (sqDist < minSqDist) {
				minSqDist = sqDist;
				next = i;
			}
		}
		CThreatMap* threatMap = circuit->GetThreatMap();
		for (size_t i = next; i < route.size(); ++i) {
			if (threatMap->GetThreatAt(unit, route[i]) > maxPathThreat) {
				return false;
			}
		}
	}
	return true;
}

bool CArtilleryTask::IsFireSpotValid(CCircuitUnit* unit, const AIFloat3& spot, float threat) const
{
	// Threat-free (anti-heavy: or IsThreatOk); with ally_threat on, also covered and not where the flee would fire on
	// arrival. FillCoveredSpots applies the same rules to many cells at once with grids
	CCircuitAI* circuit = manager->GetCircuit();
	if (!IsThreatOk(unit, spot, threat, true)) {
		return false;
	}
	if (GetCfg().allyThreat < 0.f) {
		return true;  // any spot (old behaviour)
	}
	if (!HasCover(unit, spot)) {
		return false;
	}
	const float fleeMargin = GetFleeMargin(unit);
	if (fleeMargin < 0.f) {
		return true;
	}
	CollectDangers(unit, fleeMargin + 2 * circuit->GetPathfinder()->GetSquareSize(), spot, spot);
	float dangerThreat;
	return (FindDanger(spot, dangerThreat) == nullptr) || HasCover(unit, spot, dangerThreat, true);
}

float CArtilleryTask::FillCoveredSpots(CCircuitUnit* unit, float range, const F3Vec& goals)
{
	// firePositions: cells (one per pathfinder square, 64 elmos) within range of a building in goals
	// that have ally_threat cover, no threat, can be reached and won't set off the flee on arrival.
	// Returns the goal range for the path query: the pathfinder accepts any square within one square of a cell centre.
	// The work is bounded to the area around our mobile escorts: no cell elsewhere can be covered
	firePositions.clear();

	CCircuitAI* circuit = manager->GetCircuit();
	const CMilitaryManager::SArtilleryCfg& artyCfg = GetCfg();
	const int cellSize = circuit->GetPathfinder()->GetSquareSize();
	const float goalRange = cellSize;
	const int mapWidth = CTerrainManager::GetTerrainWidth() / cellSize + 1;
	const int mapHeight = CTerrainManager::GetTerrainHeight() / cellSize + 1;

	// Firing cell centres stay (range - 2 cells) from the building, so any spot the pathfinder may stop at
	// (up to ~1.5 cells off) still reaches it
	const float fireRange = std::max(range - 2 * cellSize, goalRange);
	const float sqFireRange = SQUARE(fireRange);
	const int fireRad = int(fireRange / cellSize) + 1;

	// Escorts: mobile ones cover a firing spot; against the flee static defences count too, as in CheckFlee.
	// Anti-heavy: a cell with threat is fine where that cover outweighs it, up to the route cap (IsThreatOk)
	const bool isCoverNeeded = (artyCfg.allyThreat > 0.f);
	const float fleeMargin = GetFleeMargin(unit);
	const bool isFleeOn = (fleeMargin >= 0.f);
	const bool isThreatCover = isAntiHeavy && (artyCfg.allyThreat >= 0.f);
	const float maxCellThreat = isThreatCover ? GetMaxPathThreat(unit) : THREAT_MIN;
	const bool isCrowdCheck = isAntiHeavy && CollectCrowd(unit);
	if (isCoverNeeded || isFleeOn || isThreatCover) {
		CollectEscorts(unit, isFleeOn);
	} else {
		escorts.clear();
	}

	// Area, in cells, inclusive: around the mobile escorts; ally_threat 0 needs no cover, so the whole map
	const float allyRadius = artyCfg.allyRadius;
	const float sqAllyRadius = SQUARE(allyRadius);
	const int allyRad = int(allyRadius / cellSize) + 1;
	int x0 = 0, z0 = 0, x1 = mapWidth - 1, z1 = mapHeight - 1;
	if (isCoverNeeded) {
		x0 = mapWidth; z0 = mapHeight; x1 = -1; z1 = -1;
		for (const SEscort& escort : escorts) {
			if (escort.isMobile) {
				const int ax = int(escort.pos.x) / cellSize;
				const int az = int(escort.pos.z) / cellSize;
				x0 = std::min(x0, ax - allyRad);  x1 = std::max(x1, ax + allyRad);
				z0 = std::min(z0, az - allyRad);  z1 = std::max(z1, az + allyRad);
			}
		}
		x0 = std::max(x0, 0);  x1 = std::min(x1, mapWidth - 1);
		z0 = std::max(z0, 0);  z1 = std::min(z1, mapHeight - 1);
		if ((x0 > x1) || (z0 > z1)) {
			return goalRange;  // no mobile escort
		}
	}
	const int width = x1 - x0 + 1;
	const int height = z1 - z0 + 1;
	auto cellIndex = [x0, z0, width](int x, int z) { return (z - z0) * width + (x - x0); };
	auto cellCentre = [cellSize](int x, int z) {
		return AIFloat3((x + 0.5f) * cellSize, 0.f, (z + 0.5f) * cellSize);
	};

	// Flee zones over the area. Reach + 2 cells: the unit may stop up to ~1.5 cells from a cell centre
	if (isFleeOn) {
		CollectDangers(unit, fleeMargin + 2 * cellSize,
				AIFloat3(x0 * cellSize, 0.f, z0 * cellSize), AIFloat3((x1 + 1) * cellSize, 0.f, (z1 + 1) * cellSize));
	} else {
		dangers.clear();
	}
	const bool isFleeCheck = !dangers.empty();
	const bool isFleeGrid = isFleeCheck || isThreatCover;

	// Escort power per cell, by cell centre
	if (isCoverNeeded || isFleeGrid) {
		coverGrid.assign(width * height, 0.f);
		fleeCoverGrid.assign(isFleeGrid ? width * height : 0, 0.f);
		for (const SEscort& escort : escorts) {
			const int ax = int(escort.pos.x) / cellSize;
			const int az = int(escort.pos.z) / cellSize;
			for (int z = std::max(az - allyRad, z0); z <= std::min(az + allyRad, z1); ++z) {
				for (int x = std::max(ax - allyRad, x0); x <= std::min(ax + allyRad, x1); ++x) {
					if (cellCentre(x, z).SqDistance2D(escort.pos) > sqAllyRadius) {
						continue;
					}
					const int index = cellIndex(x, z);
					if (escort.isMobile) {
						coverGrid[index] += escort.power;
					}
					if (isFleeGrid) {
						fleeCoverGrid[index] += escort.power;
					}
				}
			}
		}
	}

	CThreatMap* threatMap = circuit->GetThreatMap();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	STerrainMapArea* area = unit->GetArea();
	const float sqReach = SQUARE(fireRange + allyRadius + cellSize);  // a building this far from every escort has no covered cell
	cellAdded.assign(width * height, 0);
	for (const AIFloat3& target : goals) {
		if (isCoverNeeded) {
			bool isNearEscort = false;
			for (const SEscort& escort : escorts) {
				if (escort.isMobile && (escort.pos.SqDistance2D(target) <= sqReach)) {
					isNearEscort = true;
					break;
				}
			}
			if (!isNearEscort) {
				continue;
			}
		}
		const int tx = int(target.x) / cellSize;
		const int tz = int(target.z) / cellSize;
		for (int z = std::max(tz - fireRad, z0); z <= std::min(tz + fireRad, z1); ++z) {
			for (int x = std::max(tx - fireRad, x0); x <= std::min(tx + fireRad, x1); ++x) {
				const int index = cellIndex(x, z);
				if (cellAdded[index] || (isCoverNeeded && (coverGrid[index] < artyCfg.allyThreat))) {
					continue;
				}
				AIFloat3 cellPos = cellCentre(x, z);
				if (cellPos.SqDistance2D(target) > sqFireRange) {
					continue;
				}
				cellAdded[index] = 1;  // checked once, whatever the outcome
				if (isCrowdCheck && IsNearAny(cellPos, crowd, CROWD_RADIUS)) {
					continue;  // another anti-heavy stands or walks there: one shell would hit both
				}
				// A spot inside a flee zone without the cover to hold there: walk in, flee, walk back
				float dangerThreat;
				if (isFleeCheck && (FindDanger(cellPos, dangerThreat) != nullptr)
					&& (fleeCoverGrid[index] < artyCfg.allyThreat + dangerThreat))
				{
					continue;
				}
				CTerrainManager::CorrectPosition(cellPos);
				// Only threat-free (anti-heavy: covered, within the route cap) cells the unit can reach: otherwise the
				// nearest goal may be one that gets rejected afterwards (threat), or the search may sweep the whole map
				// (unreachable)
				const float cellThreat = threatMap->GetThreatAt(unit, cellPos);
				const bool isThreatOk = (cellThreat <= THREAT_MIN)
						|| (isThreatCover && (cellThreat <= maxCellThreat)
							&& (fleeCoverGrid[index] - cellThreat >= artyCfg.allyThreat));
				if (isThreatOk && terrainMgr->CanMoveToPos(area, cellPos)) {
					firePositions.push_back(cellPos);
				}
			}
		}
	}
	return goalRange;
}

/*
 * Nothing to shoot or walk to
 */

void CArtilleryTask::FallbackNoTarget(CCircuitUnit* unit, bool isUpdating)
{
	if (GetCfg().isNoScout) {
		FallbackStay(unit, isUpdating);
	} else {
		FallbackScout(unit, isUpdating);
	}
}

void CArtilleryTask::FallbackNoSpot(CCircuitUnit* unit, bool isUpdating)
{
	if (GetCfg().isNoScout) {
		FallbackStay(unit, isUpdating);
	} else {
		FallbackBasePos(unit, isUpdating);
	}
}

void CArtilleryTask::FallbackStay(CCircuitUnit* unit, bool isUpdating)
{
	// Stay put while the spot is threat-free (anti-heavy: or IsThreatOk) and (ally_threat) still covered, otherwise
	// move to safety. Anti-heavy: hold once within ARMY_HOLD_RADIUS of the army spot (safePositions, from
	// WalkAntiHeavy), covered or not (the flee guards it, and a re-plan would only give the same short path), not
	// anywhere covered such as the base. Re-checked every few updates by Execute
	// Still walking to a safety spot: get there first, else at the edge of cover it stops, drifts out and walks again
	ITravelAction* travelAct = unit->GetTravelAct();
	if ((safePath != nullptr) && (travelAct->GetPath() == safePath) && travelAct->IsActive()) {
		return;
	}
	CCircuitAI* circuit = manager->GetCircuit();
	const AIFloat3& pos = unit->GetPos(circuit->GetLastFrame());
	const bool isStay = isAntiHeavy ? IsNearAny(pos, safePositions, ARMY_HOLD_RADIUS)
			: (IsThreatOk(unit, pos, circuit->GetThreatMap()->GetThreatAt(unit, pos), false)
				&& ((GetCfg().allyThreat < 0.f) || HasCover(unit, pos)));
	if (isStay) {
		HoldHere(unit);
	} else {
		FallbackSafePos(unit, isUpdating);
	}
}

void CArtilleryTask::HoldHere(CCircuitUnit* unit)
{
	ITravelAction* travelAct = unit->GetTravelAct();
	// Finished: the last waypoint is ordered and the unit may still be walking (the action list ends a finishing
	// travel at once, so its state is HALT by now, not FINISH)
	if (travelAct->IsActive() || travelAct->IsFinished()) {
		// Walking somewhere that is no longer wanted: the engine still holds queued waypoints, stop here
		CCircuitAI* circuit = manager->GetCircuit();
		TRY_UNIT(circuit, unit,
			unit->GetUnit()->Stop();
		)
	}
	travelAct->StateWait();
}

void CArtilleryTask::StopAttack(CCircuitUnit* unit)
{
	// The engine keeps an attack order (60 s) after we stop wanting it: it would follow a heavy that walks away,
	// whatever the move state, and fire at it once only radar sees it
	if (!isAttacking) {
		return;
	}
	isAttacking = false;
	CCircuitAI* circuit = manager->GetCircuit();
	TRY_UNIT(circuit, unit,
		unit->GetUnit()->Stop();
	)
}

void CArtilleryTask::ApplyPathOrHold(CCircuitUnit* unit, const std::shared_ptr<PathInfo>& pPath)
{
	// A path of 1-2 points means we are already there: a move order would finish at once, make the unit idle
	// and restart the task, so hold instead
	if (pPath->posPath.size() > 2) {
		position = pPath->posPath.back();
		unit->GetTravelAct()->SetPath(pPath);
		unit->GetTravelAct()->StateActivate();
	} else {
		HoldHere(unit);
	}
}

void CArtilleryTask::FillSafeSpots(CCircuitUnit* unit)
{
	CCircuitAI* circuit = manager->GetCircuit();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();

	// Anti-heavy: its slot behind the army (GetArmySpot). Else: attack/defend group leaders, else a defended mex
	// cluster, else the base
	if (isAntiHeavy) {
		AIFloat3 armySpot;
		if (GetArmySpot(unit, armySpot)) {
			safePositions.assign(1, armySpot);
			return;
		}
	}
	circuit->GetMilitaryManager()->FillSafePos(unit, safePositions);

	// Stand behind the leader, on the side towards our base
	const float followDist = GetCfg().followDist;
	if (followDist <= 0.f) {
		return;
	}
	const AIFloat3& basePos = circuit->GetSetupManager()->GetBasePos();
	STerrainMapArea* area = unit->GetArea();
	for (AIFloat3& safePos : safePositions) {
		AIFloat3 dir = basePos - safePos;
		dir.y = 0.f;
		const float len = dir.Length2D();
		if (len <= followDist) {
			continue;
		}
		AIFloat3 backPos = safePos + dir / len * followDist;
		CTerrainManager::CorrectPosition(backPos);
		if (terrainMgr->CanMoveToPos(area, backPos)) {
			safePos = backPos;
		}
	}
}

bool CArtilleryTask::GetArmySpot(CCircuitUnit* unit, AIFloat3& outPos)
{
	CCircuitAI* circuit = manager->GetCircuit();
	CMilitaryManager* militaryMgr = circuit->GetMilitaryManager();
	const int frame = circuit->GetLastFrame();
	const AIFloat3& pos = unit->GetPos(frame);
	const AIFloat3& basePos = circuit->GetSetupManager()->GetBasePos();
	const float sqBaseRange = SQUARE(militaryMgr->GetBaseDefRange());
	const float speed = std::max(unit->GetCircuitDef()->GetSpeed(), 1.f);  // elmos per second

	// The group: power over walking time, the current one ARMY_KEEP times more (no switching over small swings)
	const IFighterTask* group = nullptr;
	AIFloat3 groupPos;
	float bestScore = 0.f;
	for (IFighterTask* task : militaryMgr->GetTasks(FightType::ATTACK)) {
		AIFloat3 leaderPos;
		float range, power;
		if (!static_cast<CAttackTask*>(task)->GetCoverDisc(frame, leaderPos, range, power)
			|| (leaderPos.SqDistance2D(basePos) < sqBaseRange))
		{
			continue;  // fleeing, or still at our base
		}
		const float walkSec = pos.distance2D(leaderPos) / speed;
		const float score = power / (1.f + walkSec / ARMY_WALK_SEC) * ((task == armyGroup) ? ARMY_KEEP : 1.f);
		if (score > bestScore) {
			bestScore = score;
			group = task;
			groupPos = leaderPos;
		}
	}
	armyGroup = group;
	if (group == nullptr) {
		return false;
	}

	// Its slot: the n-th of the artillery-mode anti-heavies following that group (by unit id) stands on alternate
	// sides of the point follow_dist back towards our base, (n + 1) / 2 slot gaps out
	int n = 0;
	for (IFighterTask* task : militaryMgr->GetTasks(FightType::ARTY)) {
		const CArtilleryTask* other = static_cast<const CArtilleryTask*>(task);
		if ((other != this) && (other->armyGroup == group) && !other->units.empty()
			&& ((*other->units.begin())->GetId() < unit->GetId()))
		{
			++n;
		}
	}
	AIFloat3 back = basePos - groupPos;
	back.y = 0.f;
	const float len = back.Length2D();
	if (len < 1.f) {
		outPos = groupPos;
		return true;
	}
	back /= len;
	const AIFloat3 side(-back.z, 0.f, back.x);
	const int k = (n % 2 == 1) ? (n + 1) / 2 : -(n / 2);  // 0, 1, -1, 2, -2, ...
	const AIFloat3 straight = groupPos + back * std::max(0.f, std::min(GetCfg().followDist, len));
	STerrainMapArea* area = unit->GetArea();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	const AIFloat3 tries[] = {straight + side * (k * SLOT_GAP), straight, groupPos};  // its slot, straight back, the leader
	for (const AIFloat3& p : tries) {
		AIFloat3 spot = p;
		CTerrainManager::CorrectPosition(spot);
		if (terrainMgr->CanMoveToPos(area, spot)) {
			outPos = spot;
			return true;
		}
	}
	return false;
}

void CArtilleryTask::FallbackSafePos(CCircuitUnit* unit, bool isUpdating)
{
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	FillSafeSpots(unit);

	CPathFinder* pathfinder = circuit->GetPathfinder();
	std::shared_ptr<IPathQuery> query = pathfinder->CreatePathMultiQuery(
			unit, circuit->GetThreatMap(), frame,
			unit->GetPos(frame), isAntiHeavy ? DEFAULT_SLACK : DEFAULT_SLACK * 4, safePositions);  // anti-heavy: its own slot
	pathQueries[unit] = query;
	query->HoldTask(this);

	pathfinder->RunQuery(query, [this, isUpdating](const IPathQuery* query) {
		if (this->IsQueryAlive(query)) {
			this->ApplySafePos(static_cast<const CQueryPathMulti*>(query), isUpdating);
		}
	});
}

void CArtilleryTask::ApplySafePos(const CQueryPathMulti* query, bool isUpdating)
{
	const std::shared_ptr<PathInfo>& pPath = query->GetPathInfo();
	CCircuitUnit* unit = query->GetUnit();
	if (pPath->posPath.empty()) {
		FallbackBasePos(unit, isUpdating);
	} else {
		safePath = pPath;  // FallbackStay walks it to the end
		ApplyPathOrHold(unit, pPath);
	}
}

void CArtilleryTask::FallbackBasePos(CCircuitUnit* unit, bool isUpdating)
{
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	position = circuit->GetSetupManager()->GetBasePos();

	CPathFinder* pathfinder = circuit->GetPathfinder();
	std::shared_ptr<IPathQuery> query = pathfinder->CreatePathSingleQuery(
			unit, circuit->GetThreatMap(), frame,
			unit->GetPos(frame), position, DEFAULT_SLACK * 4);
	pathQueries[unit] = query;
	query->HoldTask(this);

	pathfinder->RunQuery(query, [this, isUpdating](const IPathQuery* query) {
		if (this->IsQueryAlive(query)) {
			this->ApplyBasePos(static_cast<const CQueryPathSingle*>(query), isUpdating);
		}
	});
}

void CArtilleryTask::ApplyBasePos(const CQueryPathSingle* query, bool isUpdating)
{
	const std::shared_ptr<PathInfo>& pPath = query->GetPathInfo();
	CCircuitUnit* unit = query->GetUnit();

	if (GetCfg().isNoScout) {
		ApplyPathOrHold(unit, pPath);  // never scout: no path means stay put
	} else if (!pPath->posPath.empty()) {
		unit->GetTravelAct()->SetPath(pPath);
		unit->GetTravelAct()->StateActivate();
	} else {
		FallbackScout(unit, isUpdating);
	}
}

/*
 * Old fallbacks (no_scout false): scout enemy mexes, else fight-move to a random spot
 */

void CArtilleryTask::FallbackScout(CCircuitUnit* unit, bool isUpdating)
{
	CCircuitAI* circuit = manager->GetCircuit();
	CThreatMap* threatMap = circuit->GetThreatMap();
	const int frame = circuit->GetLastFrame();
	const AIFloat3& pos = unit->GetPos(frame);
	const AIFloat3& threatPos = unit->GetTravelAct()->IsActive() ? position : pos;
	const bool proceed = isUpdating && (threatMap->GetThreatAt(unit, threatPos) < THREAT_MIN);
	if (!proceed) {
		position = circuit->GetMilitaryManager()->GetScoutPosition(unit);
	}

	if (!utils::is_valid(position) || !circuit->GetTerrainManager()->CanMoveToPos(unit->GetArea(), position)) {
		Fallback(unit, proceed);
		return;
	}

	CPathFinder* pathfinder = circuit->GetPathfinder();
	std::shared_ptr<IPathQuery> query = pathfinder->CreatePathSingleQuery(
			unit, circuit->GetThreatMap(), frame,
			pos, position, DEFAULT_SLACK * 4);
	pathQueries[unit] = query;
	query->HoldTask(this);

	pathfinder->RunQuery(query, [this](const IPathQuery* query) {
		if (this->IsQueryAlive(query)) {
			this->ApplyScoutPath(static_cast<const CQueryPathSingle*>(query));
		}
	});
}

void CArtilleryTask::ApplyScoutPath(const CQueryPathSingle* query)
{
	const std::shared_ptr<PathInfo>& pPath = query->GetPathInfo();
	CCircuitUnit* unit = query->GetUnit();

	const bool proceed = pPath->path.size() > 2;
	if (proceed) {
		unit->GetTravelAct()->SetPath(pPath);
		unit->GetTravelAct()->StateActivate();
	} else {
		Fallback(unit, proceed);
	}
}

void CArtilleryTask::Fallback(CCircuitUnit* unit, bool proceed)
{
	if (proceed) {
		return;
	}

	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();

	float x = rand() % terrainMgr->GetTerrainWidth();
	float z = rand() % terrainMgr->GetTerrainHeight();
	position = AIFloat3(x, circuit->GetMap()->GetElevationAt(x, z), z);
	position = terrainMgr->GetMovePosition(unit->GetArea(), position);
	TRY_UNIT(circuit, unit,
		unit->CmdFightTo(position, UNIT_COMMAND_OPTION_RIGHT_MOUSE_KEY, frame + FRAMES_PER_SEC * 60);
	)
	unit->GetTravelAct()->StateWait();
}

/*
 * Early flee (artillery_flee_margin)
 */

bool CArtilleryTask::CheckFlee(CCircuitUnit* unit)
{
	// Flee when an enemy that can hit us comes within its weapon range + margin, unless (ally_threat) our cover
	// here outweighs the enemies that set it off. Returns true while fleeing
	const float fleeMargin = GetFleeMargin(unit);
	if ((fleeMargin < 0.f) || (unit->Blocker() != nullptr)) {
		isFleeing = false;
		return false;
	}

	const int frame = manager->GetCircuit()->GetLastFrame();
	const AIFloat3& pos = unit->GetPos(frame);
	CollectDangers(unit, fleeMargin, pos, pos);
	float dangerThreat;
	const SDanger* danger = FindDanger(pos, dangerThreat);
	// Cover net of those enemies: an escort they outweigh doesn't hold the artillery in place
	if ((danger == nullptr) || ((GetCfg().allyThreat >= 0.f) && HasCover(unit, pos, dangerThreat, true))) {
		if (isFleeing && isAntiHeavy) {
			manager->GetCircuit()->GetMilitaryManager()->NoteAh(unit->GetCircuitDef(), CMilitaryManager::AhNote::FLEE_END);
			searchFrame[FRONT] = std::max(searchFrame[FRONT], frame + FRAMES_PER_SEC * 5);  // settle before the front
		}
		isFleeing = false;
		return false;
	}

	// On the way: a flee planned in the last 2 s, or a flee path still being planned (also after a moment of
	// cover). Any other path still being planned (Execute's: it may lead towards the enemy) is replaced at once
	SetTarget(nullptr);  // not attacking: no ready shot for the others' overkill check
	StopAttack(unit);
	const bool isRecentFlee = (frame < fleeFrame);
	if ((isFleeing && isRecentFlee) || ((isFleeing || isRecentFlee) && !IsQueryReady(unit))) {
		return true;
	}
	isFleeing = true;
	fleeFrame = frame + FRAMES_PER_SEC * 2;  // re-plan the flee every 2 s while the danger stays
	const AIFloat3 dangerPos = danger->pos;
	Flee(unit, dangerPos);
	return true;
}

void CArtilleryTask::CollectDangers(CCircuitUnit* unit, float margin, const AIFloat3& minPos,
		const AIFloat3& maxPos) const
{
	dangers.clear();
	for (auto& kv : manager->GetCircuit()->GetEnemyInfos()) {
		CEnemyInfo* enemy = kv.second;
		float eRange;
		if (!IsGroundDanger(enemy, true, eRange)) {  // gunships count for artillery
			continue;
		}
		const AIFloat3& ePos = enemy->GetPos();
		const float sqReach = SQUARE(eRange + margin);
		// Distance to the rectangle (0 inside)
		const float dx = std::max(std::max(minPos.x - ePos.x, ePos.x - maxPos.x), 0.f);
		const float dz = std::max(std::max(minPos.z - ePos.z, ePos.z - maxPos.z), 0.f);
		if ((SQUARE(dx) + SQUARE(dz) <= sqReach) && !IsQuarryIgnored(unit, enemy)) {
			dangers.push_back({ePos, sqReach, enemy->GetThreat()});
		}
	}
}

const CArtilleryTask::SDanger* CArtilleryTask::FindDanger(const AIFloat3& pos, float& outThreat)
{
	const SDanger* nearest = nullptr;
	float minSqDist = std::numeric_limits<float>::max();
	outThreat = 0.f;
	for (const SDanger& danger : dangers) {
		const float sqDist = pos.SqDistance2D(danger.pos);
		if (sqDist <= danger.sqReach) {
			outThreat += danger.threat;
			if (sqDist < minSqDist) {
				minSqDist = sqDist;
				nearest = &danger;
			}
		}
	}
	return nearest;
}

void CArtilleryTask::Flee(CCircuitUnit* unit, const AIFloat3& dangerPos)
{
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	const AIFloat3& pos = unit->GetPos(frame);

	// Candidates: safety spots (behind groups, else defended mex cluster, else base) and the base
	FillSafeSpots(unit);
	const AIFloat3& basePos = circuit->GetSetupManager()->GetBasePos();
	if (circuit->GetTerrainManager()->CanMoveToPos(unit->GetArea(), basePos)) {
		safePositions.push_back(basePos);
	}
	const AIFloat3* best = PickFleeSpot(pos, dangerPos, safePositions);
	if (best == nullptr) {
		FallbackBasePos(unit, true);
		return;
	}
	position = *best;

	CPathFinder* pathfinder = circuit->GetPathfinder();
	std::shared_ptr<IPathQuery> query = pathfinder->CreatePathSingleQuery(
			unit, circuit->GetThreatMap(), frame,
			pos, position, DEFAULT_SLACK * 4, nullptr, GetMaxPathThreat(unit));
	pathQueries[unit] = query;
	query->HoldTask(this);

	pathfinder->RunQuery(query, [this](const IPathQuery* query) {
		if (this->IsQueryAlive(query)) {
			this->ApplyFleePath(static_cast<const CQueryPathSingle*>(query));
		}
	});
}

void CArtilleryTask::ApplyFleePath(const CQueryPathSingle* query)
{
	const std::shared_ptr<PathInfo>& pPath = query->GetPathInfo();
	CCircuitUnit* unit = query->GetUnit();
	if (pPath->posPath.empty()) {
		FallbackBasePos(unit, true);  // no path under the threat cap: any path home
	} else {
		ApplyPathOrHold(unit, pPath);  // already there: nowhere better to go
	}
}

/*
 * Anti-heavy mode
 */

CMilitaryManager::AhNote CArtilleryTask::GetAhActivity() const
{
	if (isFleeing) {
		return CMilitaryManager::AhNote::FLEE;
	}
	if (isAttacking || isScooting || (shotFrame >= 0)) {
		return CMilitaryManager::AhNote::ENGAGE;  // shooting, or scooting after a shot
	}
	if (!units.empty() && (firePath != nullptr)) {
		ITravelAction* travelAct = (*units.begin())->GetTravelAct();
		if ((travelAct->GetPath() == firePath) && travelAct->IsActive() && (TARGET == routeGoal)) {
			return CMilitaryManager::AhNote::HUNT;  // walking to a firing spot
		}
	}
	return CMilitaryManager::AhNote::PARK;  // holding or following the army
}

bool CArtilleryTask::IsQuarryIgnored(CCircuitUnit* unit, CEnemyInfo* enemy) const
{
	if (!isAntiHeavy) {
		return false;
	}
	CCircuitDef* cdef = unit->GetCircuitDef();
	if (!cdef->IsQuarry(enemy->GetCircuitDef())) {
		return false;
	}
	const int frame = manager->GetCircuit()->GetLastFrame();
	if (!unit->HasWeapon() || !unit->IsWeaponReady(frame)) {
		return false;  // reloading: step back like from any other enemy
	}
	CCircuitDef* edef = enemy->GetCircuitDef();
	if (!edef->IsAttacker() || (edef->HasAntiAir() && !edef->HasAntiLand() && !edef->HasAntiWater())) {
		return true;  // it can't shoot back
	}
	const float range = cdef->GetMaxRange();
	const float eRange = std::max(edef->GetMaxRange(CCircuitDef::RangeType::LAND),
			edef->GetMaxRange(CCircuitDef::RangeType::WATER));
	if (eRange < range) {
		return true;  // we out-range it
	}
	return unit->GetPos(frame).SqDistance2D(enemy->GetPos()) <= SQUARE(range);  // it out-ranges us: once in our range
}

float CArtilleryTask::GetNetThreatAt(CCircuitUnit* unit, const AIFloat3& spot) const
{
	CThreatMap* threatMap = manager->GetCircuit()->GetThreatMap();
	float threat = threatMap->GetThreatAt(unit, spot);
	for (CEnemyInfo* enemy : ignoredQuarries) {
		threat -= threatMap->GetEnemyThreatAt(unit, enemy, spot);
	}
	return threat;
}

bool CArtilleryTask::IsFireSpotAllowed(const AIFloat3& p) const
{
	// Called by Attack right after FindHeavyTarget (same Execute), so ignoredQuarries is this unit's
	CCircuitUnit* unit = *units.begin();
	return IsFireSpotValid(unit, p, GetNetThreatAt(unit, p));
}

bool CArtilleryTask::IsThreatOk(CCircuitUnit* unit, const AIFloat3& spot, float threat, bool isWalk) const
{
	if (threat <= THREAT_MIN) {
		return true;
	}
	if (!isAntiHeavy || (GetCfg().allyThreat < 0.f) || (isWalk && (threat > GetMaxPathThreat(unit)))) {
		return false;
	}
	return HasCover(unit, spot, threat, true);
}

bool CArtilleryTask::UpdateScoot(CCircuitUnit* unit, int frame)
{
	if (!isAntiHeavy || (manager->GetCircuit()->GetMilitaryManager()->GetAntiHeavyCfg().artyScoot <= 0.f)) {
		return false;
	}
	const bool isReady = unit->HasWeapon() && unit->IsWeaponReady(frame);
	const bool isShot = isAttacking && wasReady && !isReady;
	wasReady = isReady;
	if (!isScooting) {
		// A shot: the weapon stopped being ready while our attack order was live (not an attack given while still
		// reloading). Let its burst finish (Merlin's rockets), then step back
		if (shotFrame < 0) {
			if (!isShot) {
				return false;
			}
			shotFrame = frame;
		}
		if (frame < shotFrame + unit->GetCircuitDef()->GetBurstFrames()) {
			return true;
		}
		shotFrame = -1;
		StartScoot(unit, frame);
		return isScooting;
	}
	// Turn back once walking back to where it fired from takes as long as the reload it has left
	const float speed = unit->GetCircuitDef()->GetSpeed() / FRAMES_PER_SEC;  // elmos per frame
	const float backDist = unit->GetPos(frame).distance2D(scootFrom);
	if (unit->IsWeaponReady(frame) || ((speed > 0.f) && (backDist / speed >= unit->GetWeaponReloadLeft(frame)))) {
		isScooting = false;
		unit->ForceUpdate(frame);  // Execute now
		return false;
	}
	return true;
}

void CArtilleryTask::StartScoot(CCircuitUnit* unit, int frame)
{
	// Straight away from the target, else up to 90 degrees to either side: the first spot the unit can reach that is
	// no hotter than here (back can lead into flanks or defences behind us). None: no scoot, Execute decides
	CCircuitAI* circuit = manager->GetCircuit();
	CThreatMap* threatMap = circuit->GetThreatMap();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	const float dist = circuit->GetMilitaryManager()->GetAntiHeavyCfg().artyScoot;
	const AIFloat3& pos = unit->GetPos(frame);

	// No time to get there and back before the weapon is ready (short reload, slow unit): it would only lose its spot
	const float speed = unit->GetCircuitDef()->GetSpeed() / FRAMES_PER_SEC;
	if ((speed <= 0.f) || (2.f * dist / speed >= unit->GetWeaponReloadLeft(frame))) {
		return;
	}
	AIFloat3 dir = pos - attackPos;
	dir.y = 0.f;
	if (dir.Length2D() < 1.f) {
		dir = circuit->GetSetupManager()->GetBasePos() - pos;  // on top of it: towards our base
		dir.y = 0.f;
	}
	const float len = dir.Length2D();
	if (len < 1.f) {
		return;
	}
	dir /= len;

	const float hereThreat = threatMap->GetThreatAt(unit, pos);
	static constexpr float ANGLES[] = {0.f, 0.25f, -0.25f, 0.5f, -0.5f};  // x pi
	AIFloat3 dest;
	bool isFound = false;
	for (float a : ANGLES) {
		const float rad = a * static_cast<float>(M_PI);
		const float c = cosf(rad), sn = sinf(rad);
		AIFloat3 spot = pos + AIFloat3(dir.x * c - dir.z * sn, 0.f, dir.x * sn + dir.z * c) * dist;
		CTerrainManager::CorrectPosition(spot);
		if (terrainMgr->CanMoveToPos(unit->GetArea(), spot) && (threatMap->GetThreatAt(unit, spot) <= hereThreat)) {
			dest = spot;
			isFound = true;
			break;
		}
	}
	if (!isFound) {
		return;
	}

	SetTarget(nullptr);  // not attacking: no ready shot for the others' overkill check
	isAttacking = false;  // the move replaces the attack order
	isScooting = true;
	scootFrom = pos;
	circuit->GetMilitaryManager()->NoteAh(unit->GetCircuitDef(), CMilitaryManager::AhNote::SCOOT);
	unit->GetTravelAct()->StateWait();
	TRY_UNIT(circuit, unit,
		unit->CmdMoveTo(dest, UNIT_COMMAND_OPTION_RIGHT_MOUSE_KEY, frame + FRAMES_PER_SEC * 60);
		unit->CmdWantedSpeed(NO_SPEED_LIMIT);
	)
}

/*
 * Cover
 */

bool CArtilleryTask::IsEscort(CCircuitUnit* unit, CAllyUnit* ally, bool isFlee) const
{
	// Allied armed units that can fight ground or water units: mobile ones but not artillery (main role, so
	// artillery can't escort itself) nor anti-heavy units with artillery behaviour (IsAhArty), planes, anti-air only
	// units or mobile bombs (no ground/water weapon).
	// For the flee check, static defences that can hit ground count too (firing spots look for a mobile escort)
	CCircuitDef* adef = ally->GetCircuitDef();
	if ((ally->GetId() == unit->GetId()) || (adef == nullptr) || !adef->IsAttacker() || adef->IsPlane()) {
		return false;
	}
	if (!adef->IsMobile()) {
		return isFlee && IsStaticDefence(adef);
	}
	return (adef->GetMainRole() != ROLE_TYPE(ARTY)) && (adef->HasAntiLand() || adef->HasAntiWater())
			&& !manager->GetCircuit()->GetMilitaryManager()->IsAhArty(adef);
}

bool CArtilleryTask::CollectCrowd(CCircuitUnit* unit)
{
	// The other artillery-mode anti-heavies: where each stands, and the end of the firing route it walks
	crowd.clear();
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	for (IFighterTask* task : circuit->GetMilitaryManager()->GetTasks(FightType::ARTY)) {
		CArtilleryTask* other = static_cast<CArtilleryTask*>(task);
		if ((other == this) || !other->isAntiHeavy || other->units.empty()) {
			continue;
		}
		CCircuitUnit* ally = *other->units.begin();
		crowd.push_back(ally->GetPos(frame));
		ITravelAction* travelAct = ally->GetTravelAct();
		if ((other->firePath != nullptr) && (travelAct->GetPath() == other->firePath) && travelAct->IsActive()) {
			crowd.push_back(other->firePath->posPath.back());
		}
	}
	return !crowd.empty();
}

void CArtilleryTask::CollectEscorts(CCircuitUnit* unit, bool isFlee)
{
	// Escorts (IsEscort) with their power (pwr_mod, not thr_mod) at full health: no per-unit health reads,
	// this runs over every friendly unit
	escorts.clear();
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	circuit->UpdateFriendlyUnits();  // drop units that died since the last refresh
	for (auto& kv : circuit->GetFriendlyUnits()) {
		CAllyUnit* ally = kv.second;
		if (!IsEscort(unit, ally, isFlee)) {
			continue;
		}
		const AIFloat3& allyPos = ally->GetPos(frame);
		if (utils::is_valid(allyPos)) {
			CCircuitDef* adef = ally->GetCircuitDef();
			escorts.push_back({allyPos, adef->GetPower(), adef->IsMobile()});
		}
	}
}

bool CArtilleryTask::HasCover(CCircuitUnit* unit, const AIFloat3& spot, float enemyThreat, bool isFlee) const
{
	// Escort power near spot, net of enemyThreat, reaches ally_threat. One pass that stops as soon as it does.
	// Slack: firing spots are picked by cell centre, and the unit may stop up to ~1.5 cells away, so without
	// slack it could arrive, find no cover and turn back
	CCircuitAI* circuit = manager->GetCircuit();
	const CMilitaryManager::SArtilleryCfg& artyCfg = GetCfg();
	const float need = artyCfg.allyThreat + enemyThreat;
	if (need <= 0.f) {
		return true;
	}
	const float sqRadius = SQUARE(artyCfg.allyRadius + circuit->GetPathfinder()->GetSquareSize() * 2);
	const int frame = circuit->GetLastFrame();
	circuit->UpdateFriendlyUnits();  // drop units that died since the last refresh
	float cover = 0.f;
	for (auto& kv : circuit->GetFriendlyUnits()) {
		CAllyUnit* ally = kv.second;
		if (!IsEscort(unit, ally, isFlee)) {
			continue;
		}
		const AIFloat3& allyPos = ally->GetPos(frame);
		if (utils::is_valid(allyPos) && (allyPos.SqDistance2D(spot) <= sqRadius)) {
			cover += ally->GetCircuitDef()->GetPower();
			if (cover >= need) {
				return true;
			}
		}
	}
	return false;
}

} // namespace circuit

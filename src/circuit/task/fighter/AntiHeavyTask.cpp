/*
 * AntiHeavyTask.cpp
 *
 *  Created on: Jun 30, 2016
 *      Author: rlcevg
 */

#include "task/fighter/AntiHeavyTask.h"
#include "task/fighter/AttackTask.h"
#include "task/TaskManager.h"
#include "map/ThreatMap.h"
#include "module/AllyCover.h"
#include "module/MilitaryManager.h"
#include "setup/SetupManager.h"
#include "terrain/TerrainManager.h"
#include "terrain/path/PathFinder.h"
#include "terrain/path/QueryPathSingle.h"
#include "terrain/path/QueryPathMulti.h"
#include "unit/action/DGunAction.h"
#include "unit/action/FightAction.h"
#include "unit/action/MoveAction.h"
#include "unit/ally/AllyUnit.h"
#include "unit/enemy/EnemyUnit.h"
#include "unit/CircuitUnit.h"
#include "CircuitAI.h"
#include "util/Utils.h"

#include "spring/SpringMap.h"

#include "AISCommands.h"

#include <limits>
#include <tuple>

namespace circuit {

using namespace springai;

static constexpr float PARK_CELL = 512.f;  // towers within one cell this size are one tower group
static constexpr int PARK_CHECK_FRAMES = FRAMES_PER_SEC * 5;  // the park spot is looked at again this often
static constexpr int PARK_HOLD_FRAMES = FRAMES_PER_SEC * 30;  // a park spot is kept at least this long; no walk-in after a flee
static constexpr float PARK_SWITCH = 1.3f;  // after that, another spot must score this much better
static constexpr float PARK_WALK_SEC = 60.f;  // a spot this many seconds' walk away scores half
static constexpr float FIRE_HOLD_RANGE = 1.25f;  // x weapon range: a mobile heavy this near, parkers hold fire
static constexpr int FIRE_HOLD_FRAMES = FRAMES_PER_SEC * 5;  // ... until this long after

std::vector<CAntiHeavyTask::SParkSpot> CAntiHeavyTask::parkSpots;

CAntiHeavyTask::CAntiHeavyTask(ITaskManager* mgr, float powerMod)
		: ISquadTask(mgr, FightType::AH, powerMod)
{
	CCircuitAI* circuit = manager->GetCircuit();
	float x = rand() % circuit->GetTerrainManager()->GetTerrainWidth();
	float z = rand() % circuit->GetTerrainManager()->GetTerrainHeight();
	position = AIFloat3(x, circuit->GetMap()->GetElevationAt(x, z), z);
	crowdRadius = GetCfg().spacing;  // firing spots: the two units stand apart
	isHoldAttack = true;  // the heavy, not whatever chaff comes into range (and it fires on hold fire)
}

CAntiHeavyTask::~CAntiHeavyTask()
{
}

bool CAntiHeavyTask::CanAssignTo(CCircuitUnit* unit) const
{
	if (!unit->GetCircuitDef()->IsRoleAH() ||
		(unit->GetCircuitDef() != leader->GetCircuitDef()))
	{
		return false;
	}
	if (!IsRoomFor(1)) {
		return false;  // full: antiheavy_max_group
	}
	const int frame = manager->GetCircuit()->GetLastFrame();
	if (leader->GetPos(frame).SqDistance2D(unit->GetPos(frame)) > SQUARE(1000.f)) {
		return false;
	}
	return true;
}

bool CAntiHeavyTask::IsRoomFor(size_t count) const
{
	const int typeMax = (leader != nullptr) ? leader->GetCircuitDef()->GetAhMaxGroup() : 0;
	const int maxGroup = (typeMax > 0) ? typeMax : GetCfg().maxGroup;
	return (maxGroup <= 0) || (units.size() + count <= (size_t)maxGroup);
}

void CAntiHeavyTask::AssignTo(CCircuitUnit* unit)
{
	ISquadTask::AssignTo(unit);
	CCircuitDef* cdef = unit->GetCircuitDef();
	highestRange = std::max(highestRange, cdef->GetLosRadius());
	highestRange = std::max(highestRange, cdef->GetJumpRange());

	CCircuitAI* circuit = manager->GetCircuit();
	if (unit->GetDGunAct() != nullptr) {
		unit->GetDGunAct()->StateWait();
	}

	// Ground units walk on a plain move: a fight-move stops a Lance to shoot chaff on its way, or as it runs
	int squareSize = circuit->GetPathfinder()->GetSquareSize();
	ITravelAction* travelAction;
	if (cdef->IsAttrSiege() && cdef->IsAbleToFly()) {
		travelAction = new CFightAction(unit, squareSize);
	} else {
		travelAction = new CMoveAction(unit, squareSize);
	}
	unit->PushTravelAct(travelAction);
	travelAction->StateWait();
	unit->SetAllowedToJump(cdef->IsAbleToJump() && cdef->IsAttrJump());
	if (isParkHold || isFireHold) {
		ApplyHolds(unit);
	}
}

void CAntiHeavyTask::RemoveAssignee(CCircuitUnit* unit)
{
	ISquadTask::RemoveAssignee(unit);
	if (isParkHold || isFireHold) {
		TRY_UNIT(manager->GetCircuit(), unit,
			unit->GetUnit()->SetMoveState(ISquadTask::GetMoveState(unit));
			unit->GetUnit()->SetFireState(unit->GetCircuitDef()->GetFireState());
		)
	}
	if (leader == nullptr) {
		manager->AbortTask(this);
	} else {
		highestRange = std::max(highestRange, leader->GetCircuitDef()->GetLosRadius());
		highestRange = std::max(highestRange, leader->GetCircuitDef()->GetJumpRange());
	}
}

void CAntiHeavyTask::Stop(bool done)
{
	if (isParkHold || isFireHold) {
		isParkHold = isFireHold = false;
		for (CCircuitUnit* unit : units) {
			ApplyHolds(unit);  // their own states back (scooting units: ISquadTask::Stop)
		}
	}
	ISquadTask::Stop(done);
}

void CAntiHeavyTask::Start(CCircuitUnit* unit)
{
	if ((State::REGROUP == state) || (State::ENGAGE == state)) {
		return;
	}
	if (!pPath->posPath.empty()) {
		unit->GetTravelAct()->SetPath(pPath);
		unit->GetTravelAct()->StateActivate();
	}
}

void CAntiHeavyTask::Update()
{
	++updCount;

	/*
	 * Drop task once the hunt is over (antiheavy.keep_sec; mines: once no heavy is known, as before)
	 */
	if (updCount % 32 == 1) {
		CCircuitAI* circuit = manager->GetCircuit();
		const bool isGone = ((leader != nullptr) && leader->GetCircuitDef()->IsRoleMine())
				? (circuit->GetEnemyManager()->GetEnemyCost(ROLE_TYPE(HEAVY)) < 1.f)
				: !circuit->GetMilitaryManager()->IsAhHunting();
		if (isGone) {
			manager->AbortTask(this);
			return;
		}
	}

	/*
	 * Flee from approaching enemies before anything else (every update). No merges: anti-heavies are best spread out,
	 * and a merge would pull a unit off its spot
	 */
	if (CheckFlee()) {
		return;
	}

	/*
	 * Regroup if required
	 */
	bool wasRegroup = (State::REGROUP == state);
	bool mustRegroup = IsMustRegroup();
	if (leader == nullptr) {  // all units released as stuck: task aborted
		return;
	}
	if (State::REGROUP == state) {
		if (mustRegroup) {
			CCircuitAI* circuit = manager->GetCircuit();
			int frame = circuit->GetLastFrame() + FRAMES_PER_SEC * 60;
			for (CCircuitUnit* unit : units) {
				TRY_UNIT(circuit, unit,
					unit->GetUnit()->Fight(groupPos, UNIT_COMMAND_OPTION_RIGHT_MOUSE_KEY, frame);
				)
				unit->GetTravelAct()->StateWait();
			}
		}
		return;
	}

	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	UpdateScoots(frame);  // every update: step out of reach right after a shot

	bool isExecute = (updCount % 2 == 0) && (frame >= lastTouched + FRAMES_PER_SEC);
	if (!isExecute) {
		for (CCircuitUnit* unit : units) {
			isExecute |= unit->IsForceUpdate(frame);
		}
		if (!isExecute) {
			if (wasRegroup && !pPath->posPath.empty()) {
				ActivePath();
			}
			return;
		}
	}
	lastTouched = frame;
	CollectDangers();  // for every spot check of this update (firing spots too)

	/*
	 * Update target
	 */
	if ((GetFleeMargin() < 0.f) && IsReloading(frame)) {
		if (IsQueryReady(leader)) {
			FallbackAttackSafe();  // no flee: old blind pull-back to the attack groups
		}
		return;
	}

	// A scooting leader stands past the stand-off distance FindTarget engages within: keep the fight (and the target)
	// until it's back, else the whole squad would be routed in again while it reloads
	const bool isLeaderScoot = (State::ENGAGE == state) && (target != nullptr) && IsScooting(leader);
	const bool isTargetsFound = isLeaderScoot || FindTarget();

	state = (target != nullptr) ? State::ENGAGE : State::ROAM;
	isParking = !isTargetsFound && IsParker() && utils::is_valid(park.pos);
	if (isTargetsFound) {
		parkCheckFrame = 0;  // back from a fight or a walk-in: the park spot is looked at again at once
	}
	UpdateHolds(frame);
	if (target != nullptr) {
		position = target->GetPos();
		AttackTarget(frame);
		return;
	}

	if (!IsQueryReady(leader)) {
		return;
	}

	if (!isTargetsFound) {  // enemyPositions.empty()
		FallbackTargetEmpty();
		return;
	}

	// Walk in to the nearest heavy; routes are planned again every few seconds, not on every update
	if ((frame < routeFrame + PARK_CHECK_FRAMES) && !pPath->posPath.empty()) {
		return;
	}
	routeFrame = frame;
	QueryPathMulti(enemyPositions, circuit->GetPathfinder()->GetSquareSize(),
			[this](const CQueryPathMulti* query) { ApplyTargetPath(query); });
}

void CAntiHeavyTask::OnUnitIdle(CCircuitUnit* unit)
{
	ISquadTask::OnUnitIdle(unit);
	if (units.empty() || (State::DISENGAGE == state) || (units.find(unit) == units.end())) {
		return;  // fleeing: CheckFlee re-plans; or the unit left (retreat)
	}

	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	if (isParking) {
		SpreadOut(unit, frame);
		return;
	}

	const float maxDist = std::max<float>(lowestRange, circuit->GetPathfinder()->GetSquareSize());
	if (position.SqDistance2D(leader->GetPos(frame)) < SQUARE(maxDist)) {
		CTerrainManager* terrainMgr = circuit->GetTerrainManager();
		float x = rand() % terrainMgr->GetTerrainWidth();
		float z = rand() % terrainMgr->GetTerrainHeight();
		position = AIFloat3(x, circuit->GetMap()->GetElevationAt(x, z), z);
		position = terrainMgr->GetMovePosition(leader->GetArea(), position);
	}
	Start(unit);  // NOTE: Not sure if it has effect
}

void CAntiHeavyTask::OnUnitDamaged(CCircuitUnit* unit, CEnemyInfo* attacker)
{
	CCircuitAI* circuit = manager->GetCircuit();
	const AIFloat3 pos = unit->GetPos(circuit->GetLastFrame());  // a copy: the unit may leave the task below
	ISquadTask::OnUnitDamaged(unit, attacker);
	if (unit->GetTask() != this) {  // sent home for repair: park spots near here score lower for a while
		CMilitaryManager* militaryMgr = circuit->GetMilitaryManager();
		militaryMgr->NoteAhRetreat(pos);
		militaryMgr->NoteAh(unit->GetCircuitDef(), CMilitaryManager::AhNote::RETREAT);
		return;
	}
	if ((attacker != nullptr) && (unit->GetDGunAct() != nullptr)) {
		unit->GetDGunAct()->StateActivate();
	}
}

CMilitaryManager::AhNote CAntiHeavyTask::GetActivity() const
{
	return (State::DISENGAGE == state) ? CMilitaryManager::AhNote::FLEE
		: (State::ENGAGE == state) ? CMilitaryManager::AhNote::ENGAGE
		: isParking ? CMilitaryManager::AhNote::PARK
		: CMilitaryManager::AhNote::HUNT;
}

const CMilitaryManager::SAntiHeavyCfg& CAntiHeavyTask::GetCfg() const
{
	return manager->GetCircuit()->GetMilitaryManager()->GetAntiHeavyCfg();
}

bool CAntiHeavyTask::IsCloakRoute(CCircuitUnit* unit) const
{
	// Without cloak_route the cloak is only extra safety: a decloak (firing, splash, a close enemy, low energy)
	// must not leave the unit on a route planned past enemy weapons. Mines (Skuttle) live by sneaking up: always
	CCircuitDef* cdef = unit->GetCircuitDef();
	return (cdef->IsAttrCloakRoute() || cdef->IsRoleMine()) && IFighterTask::IsCloakRoute(unit);
}

float CAntiHeavyTask::GetPathThreatMod() const
{
	if ((leader != nullptr) && leader->GetCircuitDef()->IsAbleToFly()) {
		return -1.f;  // aircraft: the cap was never tuned for the air threat layer and their small strength
	}
	return GetCfg().pathThreat;
}

float CAntiHeavyTask::GetFleeMargin() const
{
	if (leader == nullptr) {
		return -1.f;
	}
	CCircuitDef* cdef = leader->GetCircuitDef();
	if (cdef->IsRoleMine() || cdef->IsAbleToFly()) {
		return -1.f;  // mines hide (cloak) instead; aircraft don't walk the ground paths the flee plans
	}
	return cdef->GetAhFleeMargin(GetCfg().fleeMargin);
}

float CAntiHeavyTask::GetPickMargin(const CEnemyInfo* enemy) const
{
	const float margin = GetFleeMargin();
	return std::max(margin, GetDangerStart(enemy) + 0.5f * margin);
}

float CAntiHeavyTask::GetDangerStart(const CEnemyInfo* enemy) const
{
	// Half the flee margin, or one second of its walking if that is more: a fast heavy closes in before the next check
	CCircuitDef* edef = enemy->GetCircuitDef();
	return std::max(0.5f * GetFleeMargin(), (edef != nullptr) ? edef->GetSpeed() : 0.f);
}

bool CAntiHeavyTask::IsReloading(int frame) const
{
	CCircuitDef* cdef = leader->GetCircuitDef();
	return !cdef->IsRoleMine() && (cdef->GetReloadTime() >= FRAMES_PER_SEC * 5) && !leader->IsWeaponReady(frame);
}

bool CAntiHeavyTask::IsLongReload(const CCircuitDef* cdef)
{
	return cdef->GetReloadTime() >= FRAMES_PER_SEC * 5;
}

float CAntiHeavyTask::GetScootMargin(CCircuitUnit* unit) const
{
	CCircuitDef* cdef = unit->GetCircuitDef();
	if (cdef->IsRoleMine() || cdef->IsAbleToFly()) {
		return -1.f;
	}
	const float fleeMargin = GetFleeMargin();
	if (cdef->IsReloadScoot()) {
		return cdef->GetReloadScootMargin((fleeMargin < 0.f) ? DEFAULT_SCOOT_MARGIN : fleeMargin);
	}
	return fleeMargin;  // flee off (< 0): no scoot, the old leader pull-back instead
}

bool CAntiHeavyTask::IsUnitReloading(CCircuitUnit* unit, int frame) const
{
	CCircuitDef* cdef = unit->GetCircuitDef();
	if (cdef->IsReloadScoot()) {
		return ISquadTask::IsUnitReloading(unit, frame);
	}
	return (cdef->GetReloadTime() >= FRAMES_PER_SEC * 5) && unit->HasWeapon() && !unit->IsWeaponReady(frame);
}

void CAntiHeavyTask::OnScootStart(CCircuitUnit* unit)
{
	manager->GetCircuit()->GetMilitaryManager()->NoteAh(unit->GetCircuitDef(), CMilitaryManager::AhNote::SCOOT);
}

AIFloat3 CAntiHeavyTask::GetScootAnchor(CCircuitUnit* unit, const AIFloat3& uPos) const
{
	const bool isNearPark = utils::is_valid(park.pos)
			&& (park.pos.SqDistance2D(uPos) <= SQUARE(unit->GetCircuitDef()->GetMaxRange()));
	return isNearPark ? park.pos : uPos;
}

int CAntiHeavyTask::GetMoveState(CCircuitUnit* unit) const
{
	return isParkHold ? 0 : ISquadTask::GetMoveState(unit);
}

void CAntiHeavyTask::AttackTarget(int frame)
{
	CCircuitDef* cdef = leader->GetCircuitDef();
	if (cdef->IsRoleMine() || cdef->IsAbleToFly()) {
		float minePower = 0.f;
		for (CCircuitUnit* unit : units) {
			AttackDirect(unit, frame, minePower);  // they never scoot
		}
		return;
	}
	for (CCircuitUnit* unit : units) {
		if (unit->GetDGunAct() != nullptr) {
			unit->GetDGunAct()->StateActivate();
		}
	}
	Attack(frame, true);  // firing spots (IFighterTask): spread out, outside the pick zones
}

bool CAntiHeavyTask::AttackDirect(CCircuitUnit* unit, int frame, float& minePower)
{
	if (unit->Blocker() != nullptr) {
		return false;
	}
	if (unit->GetDGunAct() != nullptr) {
		unit->GetDGunAct()->StateActivate();
	}
	unit->GetTravelAct()->StateWait();

	if (unit->GetCircuitDef()->IsRoleMine()) {
		const bool isAttack = (target->GetThreat() > minePower);
		minePower += unit->GetCircuitDef()->GetPower();
		if (!isAttack) {
			return false;
		}
	}
	if (target->GetUnit()->IsCloaked()) {
		unit->Attack(target->GetPos(), target, frame + FRAMES_PER_SEC * 60);
	} else {
		unit->Attack(target, frame + FRAMES_PER_SEC * 60);
	}
	return true;
}

bool CAntiHeavyTask::IsQuarry(CEnemyInfo* enemy) const
{
	return IsQuarryFor(leader->GetCircuitDef(), enemy);
}

bool CAntiHeavyTask::IsQuarryFor(const CCircuitDef* cdef, CEnemyInfo* enemy)
{
	CCircuitDef* edef = enemy->GetCircuitDef();
	if ((edef == nullptr) || !edef->IsEnemyRoleAny(CCircuitDef::RoleMask::HEAVY | CCircuitDef::RoleMask::COMM)) {
		return false;
	}
	return !(cdef->IsAttrAntiStat() && edef->IsMobile())
		&& ((edef->GetCategory() & cdef->GetTargetCategory()) != 0)
		&& !(edef->IsAbleToFly() && !cdef->HasAntiAir());
}

bool CAntiHeavyTask::IsFleeIgnored(CEnemyInfo* enemy) const
{
	if (!IsQuarry(enemy)) {
		return false;
	}
	// A heavy is what we hunt. Only one that out-ranges a long-reload weapon is a danger, while that weapon isn't
	// ready or the heavy is out of its reach
	CCircuitDef* cdef = leader->GetCircuitDef();
	const float range = cdef->GetMaxRange();
	float eRange;
	if (!IsLongReload(cdef) || !IsGroundDangerDef(enemy->GetCircuitDef(), true, eRange) || (eRange < range)) {
		return true;
	}
	const int frame = manager->GetCircuit()->GetLastFrame();
	return !IsReloading(frame) && (leader->GetPos(frame).SqDistance2D(enemy->GetPos()) <= SQUARE(range));
}

bool CAntiHeavyTask::IsCoverOn() const
{
	return manager->GetCircuit()->GetMilitaryManager()->GetAllyCover()->IsOn();
}

float CAntiHeavyTask::GetCoverAt(const AIFloat3& pos) const
{
	return manager->GetCircuit()->GetMilitaryManager()->GetAllyCover()->GetCoverAt(pos, {nullptr, nullptr, 1.f}).GetSum();
}

float CAntiHeavyTask::GetFleeThreat(CEnemyInfo* enemy) const
{
	return (!IsCoverOn() || (GetCoverAt(enemy->GetPos()) > 0.f)) ? enemy->GetThreat() : std::numeric_limits<float>::max();
}

bool CAntiHeavyTask::IsFleeCovered(const AIFloat3& pos, float enemyThreat, float allyThreat, float allyRadius,
		CCircuitDef::RoleT ownRole) const
{
	if (!IsCoverOn()) {
		return ISquadTask::IsFleeCovered(pos, enemyThreat, allyThreat, allyRadius, ownRole);  // our units around pos
	}
	return GetCoverAt(pos) - enemyThreat >= allyThreat;
}

bool CAntiHeavyTask::CheckFlee()
{
	// antiheavy.flee_margin (M). Per danger (FindFleeDanger): the flee starts at its range + GetDangerStart and ends a
	// quarter of M further out; cover (IsFleeCovered, flee_ally_threat) calls it off
	const CMilitaryManager::SAntiHeavyCfg& ahCfg = GetCfg();
	const float margin = GetFleeMargin();
	const bool isFlee = UpdateFlee((margin < 0.f) ? -1.f : 0.f, 0.25f * margin, true,
			ahCfg.fleeAllyThreat, ahCfg.fleeAllyRadius, ROLE_TYPE(AH));
	if (isFlee && !wasFleeing) {
		manager->GetCircuit()->GetMilitaryManager()->NoteAh(leader->GetCircuitDef(), CMilitaryManager::AhNote::FLEE_START);
	}
	wasFleeing = isFlee;
	return isFlee;
}

void CAntiHeavyTask::FleeFrom(const AIFloat3& dangerPos)
{
	// Parkers run to the nearest safe park spot further from the danger, and stay there; others as before
	CollectDangers();
	const AIFloat3* goal = nullptr;
	if (IsParker()) {
		FillParkSpots(parkSpots);
		urgentPositions.clear();
		for (const SParkSpot& spot : parkSpots) {
			urgentPositions.push_back(spot.pos);
		}
		goal = PickFleeSpot(leader->GetPos(manager->GetCircuit()->GetLastFrame()), dangerPos, urgentPositions);
		ClearPark();
		if (goal != nullptr) {
			SetPark(parkSpots[goal - urgentPositions.data()]);
		}
	}
	if (goal == nullptr) {
		goal = FindFleeGoal(dangerPos);
	}
	if (goal == nullptr) {
		FallbackBasePos(true);
		return;
	}
	QueryPath(leader, *goal, FALLBACK_PATH_RANGE, true, [this](const CQueryPathSingle* query) { ApplyFleePath(query); });
}

void CAntiHeavyTask::ApplyFleePath(const CQueryPathSingle* query)
{
	pPath = query->GetPathInfo();

	if (!pPath->posPath.empty()) {
		position = pPath->posPath.back();
		ActivePath();
		state = State::DISENGAGE;
	} else {
		FallbackBasePos(true);  // no path under the cap: base, then (last resort) through any threat
	}
}

void CAntiHeavyTask::OnFleeEnd()
{
	walkInFrame = manager->GetCircuit()->GetLastFrame() + PARK_HOLD_FRAMES;  // the way back in leads to the same danger
	if (utils::is_valid(park.pos)) {
		return;  // keep walking to our park spot: it is safe, and covered
	}
	// Out of reach: don't walk the rest of the way to the safety spot
	pPath = std::make_shared<PathInfo>();  // else OnUnitIdle -> Start() would resume the flee path after the Stop
	CCircuitAI* circuit = manager->GetCircuit();
	for (CCircuitUnit* unit : units) {
		if (unit->Blocker() != nullptr) {
			continue;  // busy with its own action (jump, ...)
		}
		unit->GetTravelAct()->StateWait();
		TRY_UNIT(circuit, unit,
			unit->GetUnit()->Stop();
		)
	}
}

float CAntiHeavyTask::GetStandOff(CEnemyInfo* enemy, float engageRange) const
{
	// The threat map draws an enemy's threat out to its range (threat-map squares) on each layer; one more
	// square of slack. Our layer as CThreatMap::SetThreatType picks it
	CCircuitDef* cdef = leader->GetCircuitDef();
	const CEnemyUnit* data = enemy->GetData();
	int range;
	if (cdef->IsAbleToFly()) {
		range = data->GetRange(CCircuitDef::ThreatType::AIR);
	} else if (cdef->IsAmphibious()) {
		range = std::max(data->GetRange(CCircuitDef::ThreatType::LAND), data->GetRange(CCircuitDef::ThreatType::WATER));
	} else {
		range = data->GetRange(CCircuitDef::ThreatType::LAND);
	}
	if (range <= 0) {
		return engageRange;  // can't hit our layer
	}
	return std::max(engageRange, (float)((range + 1) * manager->GetCircuit()->GetThreatMap()->GetSquareSize()));
}

bool CAntiHeavyTask::ProbeStandOff(const AIFloat3& ePos, const AIFloat3& pos, float standOff, AIFloat3& outPos) const
{
	// The threat map draws a moving enemy further out than its stored range (speed slack), and cells are coarse:
	// read the threat rather than trust the estimate
	CCircuitAI* circuit = manager->GetCircuit();
	CThreatMap* threatMap = circuit->GetThreatMap();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	STerrainMapArea* area = leader->GetArea();
	const float step = threatMap->GetSquareSize();
	const float cap = GetPathThreatCap(leader, pos, {GetPointTowards(ePos, pos, standOff)});
	for (int i = 0; i <= 6; ++i) {
		const AIFloat3 p = GetPointTowards(ePos, pos, standOff + i * step);
		if (terrainMgr->CanMoveToPos(area, p) && (GetTaskThreatAt(leader, p) <= cap)) {
			outPos = p;
			return true;
		}
		if (p.SqDistance2D(pos) < 1.f) {
			break;  // reached the squad: no point further out on this line
		}
	}
	return false;
}

AIFloat3 CAntiHeavyTask::GetPointTowards(const AIFloat3& ePos, const AIFloat3& pos, float dist)
{
	AIFloat3 dir = pos - ePos;
	dir.y = 0.f;
	const float len = dir.Length2D();
	if (len <= dist) {
		return pos;
	}
	AIFloat3 p = ePos + dir * (dist / len);
	CTerrainManager::CorrectPosition(p);
	return p;
}

void CAntiHeavyTask::CollectDangers()
{
	dangers.clear();
	if (GetFleeMargin() < 0.f) {
		return;
	}
	for (auto& kv : manager->GetCircuit()->GetEnemyInfos()) {
		CEnemyInfo* enemy = kv.second;
		float eRange;
		if (!IsGroundDanger(enemy, true, eRange) || IsFleeIgnored(enemy)) {
			continue;  // as FindFleeDanger
		}
		dangers.push_back({enemy->GetPos(), SQUARE(eRange + GetPickMargin(enemy)), GetFleeThreat(enemy)});
	}
}

bool CAntiHeavyTask::IsSpotSafe(const AIFloat3& spot) const
{
	float threat = 0.f;
	bool isReached = false;
	for (const SDanger& danger : dangers) {
		if (spot.SqDistance2D(danger.pos) <= danger.sqReach) {
			threat += danger.threat;
			isReached = true;
		}
	}
	if (!isReached) {
		return true;
	}
	const CMilitaryManager::SAntiHeavyCfg& ahCfg = GetCfg();
	return (ahCfg.fleeAllyThreat >= 0.f)
		&& IsFleeCovered(spot, threat, ahCfg.fleeAllyThreat, ahCfg.fleeAllyRadius, ROLE_TYPE(AH));
}

float CAntiHeavyTask::GetEscapeSafeThreat() const
{
	return IsSniper() ? GetSafeThreat() : -1.f;
}

float CAntiHeavyTask::GetAllowedThreatAt(const AIFloat3& pos, float strength) const
{
	// As the route cap (GetPathThreatCap), for this strength
	const float capMod = GetPathThreatMod();
	if (capMod < 0.f) {
		return std::numeric_limits<float>::max();
	}
	CCircuitAI* circuit = manager->GetCircuit();
	const AIFloat3& basePos = circuit->GetSetupManager()->GetBasePos();
	const float bonus = circuit->GetMilitaryManager()->GetBaseThrBonus(basePos.SqDistance2D(pos));
	return strength * bonus / GetThrDistModAt(pos) * capMod;
}

bool CAntiHeavyTask::GetSightLifts(const CEnemyInfo* enemy, float& outAimLift, float& outTargetLift) const
{
	return CAttackTask::GetSightLiftsFor(manager->GetCircuit()->GetMilitaryManager(), leader->GetCircuitDef(), enemy,
			outAimLift, outTargetLift);
}

bool CAntiHeavyTask::IsFireSpotAllowed(const AIFloat3& p) const
{
	return IsSpotSafe(p);
}

bool CAntiHeavyTask::FindTarget()
{
	CCircuitAI* circuit = manager->GetCircuit();
	CMap* map = circuit->GetMap();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	CThreatMap* threatMap = circuit->GetThreatMap();
	const int frame = circuit->GetLastFrame();
	const AIFloat3& pos = leader->GetPos(frame);
	STerrainMapArea* area = leader->GetArea();
	CCircuitDef* cdef = leader->GetCircuitDef();
	const float maxPower = attackPower * powerMod * GetCountPowerMod();  // divided by the thr_mod.distance curve at each enemy's spot
	const float weaponRange = cdef->GetMaxRange();
	const float minEngage = threatMap->GetSquareSize() * 2.0f;
	// Mines and aircraft engage within LOS / jump range (highestRange), as before
	const bool isOldEngage = cdef->IsRoleMine() || cdef->IsAbleToFly();
	// Stand-off routing goes with the route cap; without it (path_threat < 0, aircraft) the old goals: the heavies
	const bool isStandOff = (GetPathThreatMod() >= 0.f);
	const float pathSlack = isStandOff ? 2 * circuit->GetPathfinder()->GetSquareSize() : 0.f;  // routes end within this of a stand-off point
	const bool isFleeOn = (GetFleeMargin() >= 0.f);
	bool isRadarOnly = false;

	// Best first: mobile, the target we have, one other squads shoot at too (focus fire), nearest
	using Rank = std::tuple<bool, bool, bool, float>;
	Rank bestRank(true, true, true, std::numeric_limits<float>::max());
	CEnemyInfo* bestTarget = nullptr;
	enemyPositions.clear();
	threatMap->SetThreatType(leader);  // shared threat-map state: set on every pick
	for (auto& kv : circuit->GetEnemyInfos()) {
		CEnemyInfo* enemy = kv.second;
		// The cheap role test first: most enemies aren't heavies, and the rest reads maps and the engine
		if (enemy->IsHidden() || !IsQuarry(enemy)) {
			continue;
		}
		const AIFloat3& ePos = enemy->GetPos();
		const float sqDist = pos.SqDistance2D(ePos);
		// A long-reload weapon fires at what we see, not at a radar blip
		const bool isSeen = !IsLongReload(cdef) || enemy->IsInLOS();
		isRadarOnly |= !isSeen && (sqDist <= SQUARE(weaponRange));
		// Already within weapon range (pulled from a fight; or out-ranging it): attack from here, no walk in, so its
		// escorts don't rule it out; the flee still guards the squad. The current target keeps it a bit further out,
		// so a heavy at the edge of range isn't dropped and picked again every check
		const float reach = (enemy == target) ? weaponRange * 1.2f : weaponRange;
		const bool isInReach = isSeen && (sqDist <= SQUARE(reach));
		if ((!isInReach && (frame < walkInFrame))  // just fled: no walk back in for a while
			|| (ePos.y - map->GetElevationAt(ePos.x, ePos.z) > weaponRange)
			|| (!isStandOff && !terrainMgr->CanMoveToPos(area, ePos)))
		{
			continue;
		}
		// Its escorts against our power. Not for one that can't shoot back where we stand (the spot checks below)
		float eRange;
		const bool isOutranged = !IsGroundDangerDef(enemy->GetCircuitDef(), true, eRange) || (eRange < weaponRange);
		if (!isInReach && !isOutranged
			&& (maxPower / GetThrDistModAt(ePos) <= GetTaskThreatExcl(leader, ePos, enemy->GetThreat())))
		{
			continue;
		}

		// Routes end at the stand-off point, just outside the heavy's own threat, so the route cap only meets
		// its escorts' threat; the squad attacks from there. Within 0.9x weapon range of one we see; one we don't, to
		// where we would see it (LOS), or not at all if we wouldn't see it from outside its threat
		const float engageRange = isOldEngage ? std::max(highestRange, minEngage)
				: std::max(isSeen ? weaponRange * 0.9f : cdef->GetLosRadius(), minEngage);
		float standOff = isStandOff ? GetStandOff(enemy, engageRange) : engageRange;
		AIFloat3 standPos = GetPointTowards(ePos, pos, standOff);
		const bool isFar = !isInReach && (sqDist > SQUARE(standOff + pathSlack));
		if (!isSeen && (!isFar || (standOff > engageRange))) {
			continue;
		}
		if (isStandOff && isFar) {
			if (!ProbeStandOff(ePos, pos, standOff, standPos)) {
				continue;  // no point near its ring under the route cap: the route there would fail
			}
			standOff = ePos.distance2D(standPos);  // engage once there
		}
		// Flee on: skip heavies we'd have to attack from inside a pick zone (escorts, riots, ...), else the squad walks
		// in, flees, and walks back in. Where the attack ends up (weapon range) and where the route ends
		if (isFleeOn && !isInReach
			&& (!IsSpotSafe(GetPointTowards(ePos, pos, weaponRange * 0.9f)) || !IsSpotSafe(standPos)))
		{
			continue;
		}

		if (isFar) {
			enemyPositions.push_back(isStandOff ? standPos : ePos);
			continue;
		}
		// Shots the other squads have ready at it would kill it: theirs
		const float ready = GetOthersReadyDamage(enemy);
		if (ready > 1.1f * (enemy->GetHealth() + enemy->GetShieldPower())) {
			continue;
		}
		const Rank rank(!enemy->GetCircuitDef()->IsMobile(), enemy != target, ready <= 0.f, sqDist);
		if (rank < bestRank) {
			bestRank = rank;
			bestTarget = enemy;
		}
	}

	CMilitaryManager* militaryMgr = circuit->GetMilitaryManager();
	if (isRadarOnly) {
		militaryMgr->NoteAh(cdef, CMilitaryManager::AhNote::RADAR_ONLY);
	}
	if ((target != nullptr) && (bestTarget != target)) {
		militaryMgr->NoteAh(cdef, CMilitaryManager::AhNote::DROP);
	}
	SetTarget(bestTarget);
	return (bestTarget != nullptr) || !enemyPositions.empty();
}

float CAntiHeavyTask::GetOthersReadyDamage(CEnemyInfo* enemy) const
{
	const int frame = manager->GetCircuit()->GetLastFrame();
	float damage = 0.f;
	for (IFighterTask* task : enemy->GetTasks()) {
		if ((task == this) || (task->GetFightType() != FightType::AH)) {
			continue;
		}
		for (CCircuitUnit* unit : task->GetAssignees()) {
			if (unit->IsWeaponReady(frame)) {
				damage += unit->GetCircuitDef()->GetShotDamage();
			}
		}
	}
	return damage;
}

void CAntiHeavyTask::ApplyTargetPath(const CQueryPathMulti* query)
{
	pPath = query->GetPathInfo();

	if (!pPath->posPath.empty()) {
		position = pPath->posPath.back();
		ActivePath();
	} else {
		FallbackTargetEmpty();
	}
}

void CAntiHeavyTask::FallbackTargetEmpty()
{
	CCircuitAI* circuit = manager->GetCircuit();
	if (!leader->GetCircuitDef()->IsRoleMine() && !circuit->GetMilitaryManager()->IsAhHunting()) {
		manager->AbortTask(this);
	} else if (IsParker()) {
		FallbackPark();
	} else {
		FallbackAttackSafe();
	}
}

bool CAntiHeavyTask::IsSniper() const
{
	CCircuitDef* cdef = leader->GetCircuitDef();
	return !cdef->IsRoleMine() && !cdef->IsAbleToFly() && (cdef->GetMaxRange() > cdef->GetLosRadius());
}

bool CAntiHeavyTask::IsParker() const
{
	return (leader != nullptr) && IsSniper() && IsCoverOn();
}

void CAntiHeavyTask::FillParkSpots(std::vector<SParkSpot>& outSpots)
{
	outSpots.clear();
	CCircuitAI* circuit = manager->GetCircuit();
	CMilitaryManager* militaryMgr = circuit->GetMilitaryManager();
	const int frame = circuit->GetLastFrame();
	const AIFloat3& basePos = circuit->GetSetupManager()->GetBasePos();

	// Tower groups: the armed statics of each PARK_CELL cell, around their power-weighted centre
	struct STowers {
		AIFloat3 sum = ZeroVector;  // positions x power
		float power = 0.f;
		float range = 0.f;
	};
	std::map<int, STowers> cells;
	const int cellsX = circuit->GetTerrainManager()->GetTerrainWidth() / PARK_CELL + 1;
	for (auto& kv : circuit->GetFriendlyUnits()) {
		CAllyUnit* ally = kv.second;
		CCircuitDef* adef = ally->GetCircuitDef();
		if ((adef == nullptr) || adef->IsMobile() || !adef->IsAttacker() || !IsStaticDefence(adef)) {
			continue;
		}
		const AIFloat3& p = ally->GetPos(frame);
		const float power = utils::is_valid(p) ? GetCoverPower(ally) : 0.f;
		if (power <= 0.f) {
			continue;  // nanoframe
		}
		STowers& towers = cells[int(p.z / PARK_CELL) * cellsX + int(p.x / PARK_CELL)];
		towers.sum += p * power;
		towers.power += power;
		towers.range = std::max(towers.range, adef->GetMaxRange(CCircuitDef::RangeType::LAND));
	}
	for (auto& kv : cells) {
		const STowers& towers = kv.second;
		AddParkSpot(GetPointTowards(towers.sum / towers.power, basePos, towers.range / 3.f), nullptr, outSpots);
	}

	// Behind our attack groups, where artillery follows them
	const float followDist = militaryMgr->GetArtilleryCfg().followDist;
	for (IFighterTask* task : militaryMgr->GetTasks(FightType::ATTACK)) {
		AIFloat3 groupPos;
		float range, power;
		if (static_cast<CAttackTask*>(task)->GetCoverDisc(frame, groupPos, range, power)) {
			AddParkSpot(GetPointTowards(groupPos, basePos, followDist), task, outSpots);
		}
	}
	std::sort(outSpots.begin(), outSpots.end(), [](const SParkSpot& a, const SParkSpot& b) { return a.score > b.score; });
}

void CAntiHeavyTask::AddParkSpot(const AIFloat3& pos, const IFighterTask* group, std::vector<SParkSpot>& outSpots)
{
	CCircuitAI* circuit = manager->GetCircuit();
	CMilitaryManager* militaryMgr = circuit->GetMilitaryManager();
	const AIFloat3& leaderPos = leader->GetPos(circuit->GetLastFrame());
	if (!circuit->GetTerrainManager()->CanMoveToPos(leader->GetArea(), pos)
		|| (GetTaskThreatAt(leader, pos) > GetPathThreatCap(leader, leaderPos, {pos}))
		|| !IsSpotSafe(pos))
	{
		return;
	}
	const float claimRadius = std::max(PARK_CELL / 2.f, 2.f * GetCfg().spacing);
	for (IFighterTask* task : militaryMgr->GetTasks(FightType::AH)) {
		const AIFloat3& claim = static_cast<CAntiHeavyTask*>(task)->GetParkPos();
		if ((task != this) && utils::is_valid(claim) && (claim.SqDistance2D(pos) < SQUARE(claimRadius))) {
			return;  // another squad parks there: spread out
		}
	}
	const float cover = GetCoverAt(pos);
	if (cover <= 0.f) {
		return;
	}

	// Heavy pressure: recent sightings (weighted down over keep_sec), full within our weapon range of the spot
	CCircuitDef* cdef = leader->GetCircuitDef();
	const float range = cdef->GetMaxRange();
	const float keepFrames = GetCfg().keepFrames;
	float pressure = 0.f;
	for (const CMilitaryManager::SHeavySeen& seen : militaryMgr->GetHeavySightings()) {
		pressure += std::max(0.f, 1.f - seen.age / keepFrames) * std::min(1.f, range / std::max(pos.distance2D(seen.pos), 1.f));
	}
	const float walkSec = leaderPos.distance2D(pos) / std::max(cdef->GetSpeed(), 1.f);
	float score = cover * pressure / (1.f + walkSec / PARK_WALK_SEC);
	if (militaryMgr->IsNearAhRetreat(pos, PARK_CELL / 2.f)) {
		score *= 0.5f;  // one of ours was hit near there lately
	}
	outSpots.push_back({pos, group, cover, score});
}

void CAntiHeavyTask::SetPark(const SParkSpot& spot)
{
	park = spot;
	parkFrame = manager->GetCircuit()->GetLastFrame();
}

void CAntiHeavyTask::UpdatePark()
{
	// Keep the spot for PARK_HOLD_FRAMES, then until another scores PARK_SWITCH times more. Leave it once it is gone
	// (unsafe, claimed, its group gone) or has lost half its cover
	FillParkSpots(parkSpots);
	const SParkSpot* current = nullptr;
	for (const SParkSpot& spot : parkSpots) {
		const bool isSame = (park.group != nullptr) ? (spot.group == park.group)
				: ((spot.group == nullptr) && (spot.pos.SqDistance2D(park.pos) < SQUARE(PARK_CELL / 2.f)));
		if (isSame) {
			current = &spot;
			break;
		}
	}
	const int frame = manager->GetCircuit()->GetLastFrame();
	if (utils::is_valid(park.pos) && (current != nullptr) && (current->cover >= 0.5f * park.cover)
		&& ((frame < parkFrame + PARK_HOLD_FRAMES) || (parkSpots.front().score < PARK_SWITCH * current->score)))
	{
		park.pos = current->pos;  // a group's spot moves with it
		park.score = current->score;
		return;
	}
	ClearPark();
	if (!parkSpots.empty()) {
		SetPark(parkSpots.front());
	}
}

void CAntiHeavyTask::FallbackPark()
{
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	const AIFloat3 oldPos = park.pos;
	const bool isCheck = (frame >= parkCheckFrame);
	if (isCheck) {
		parkCheckFrame = frame + PARK_CHECK_FRAMES;
		UpdatePark();
	}
	if (!utils::is_valid(park.pos)) {
		FallbackAttackSafe();  // nowhere covered: the old way, to the army
		return;
	}
	// A new route when the spot moved, else on a check while the squad isn't there
	const float spacing = GetCfg().spacing;
	const bool isMoved = !utils::is_valid(oldPos) || (oldPos.SqDistance2D(park.pos) > SQUARE(spacing));
	if (isMoved || (isCheck && (leader->GetPos(frame).SqDistance2D(park.pos) > SQUARE(2.f * spacing)))) {
		QueryPath(leader, park.pos, FALLBACK_PATH_RANGE, true, [this](const CQueryPathSingle* query) { ApplyParkPath(query); });
	}
}

void CAntiHeavyTask::ApplyParkPath(const CQueryPathSingle* query)
{
	pPath = query->GetPathInfo();

	if (!pPath->posPath.empty()) {
		position = pPath->posPath.back();
		ActivePath();
	} else {
		ClearPark();  // no way there under the cap
		FallbackAttackSafe();
	}
}

void CAntiHeavyTask::SpreadOut(CCircuitUnit* unit, int frame)
{
	// Side by side, facing away from our base: one shot can't hit both. Left alone 1-2 spacings from the leader
	const AIFloat3& leaderPos = leader->GetPos(frame);
	const AIFloat3& uPos = unit->GetPos(frame);
	const float spacing = GetCfg().spacing;
	const float sqDist = uPos.SqDistance2D(leaderPos);
	if ((unit == leader) || ((sqDist >= SQUARE(spacing)) && (sqDist <= SQUARE(2.f * spacing)))) {
		return;
	}
	CCircuitAI* circuit = manager->GetCircuit();
	AIFloat3 dir = circuit->GetSetupManager()->GetBasePos() - leaderPos;
	dir.y = 0.f;
	const float len = dir.Length2D();
	dir = (len > 1.f) ? dir / len : RgtVector;
	AIFloat3 pos = leaderPos + AIFloat3(-dir.z, 0.f, dir.x) * spacing;
	CTerrainManager::CorrectPosition(pos);
	pos = circuit->GetTerrainManager()->GetMovePosition(unit->GetArea(), pos);
	if (pos.SqDistance2D(uPos) < SQUARE(SQUARE_SIZE * 4)) {
		return;  // nowhere better (the next idle would ask again)
	}
	unit->GetTravelAct()->StateWait();
	TRY_UNIT(circuit, unit,
		unit->CmdMoveTo(pos, UNIT_COMMAND_OPTION_RIGHT_MOUSE_KEY, frame + FRAMES_PER_SEC * 60);
	)
}

void CAntiHeavyTask::UpdateHolds(int frame)
{
	// Hold position while parked at towers (the tower spot is the cover); parkers hold fire while a mobile heavy is
	// near, so the long reload isn't spent on chaff (the attack on a target still fires)
	const bool isParker = IsParker();
	const float range = leader->GetCircuitDef()->GetMaxRange();
	if (isParker && manager->GetCircuit()->GetMilitaryManager()->IsQuarryWithin(leader, FIRE_HOLD_RANGE * range, true)) {
		fireHoldFrame = frame;
	}
	const bool isMoveHold = isParking && (park.group == nullptr);
	const bool isFireHoldNow = isParker && (frame < fireHoldFrame + FIRE_HOLD_FRAMES);
	if ((isMoveHold == isParkHold) && (isFireHoldNow == isFireHold)) {
		return;
	}
	isParkHold = isMoveHold;
	isFireHold = isFireHoldNow;
	for (CCircuitUnit* unit : units) {
		ApplyHolds(unit);
	}
}

void CAntiHeavyTask::ApplyHolds(CCircuitUnit* unit)
{
	if (!IsScooting(unit)) {
		SetScootHold(unit, false);  // GetMoveState; a scooting unit holds position until its scoot ends
	}
	TRY_UNIT(manager->GetCircuit(), unit,
		unit->GetUnit()->SetFireState(isFireHold ? CCircuitDef::FireType::HOLD : unit->GetCircuitDef()->GetFireState());
	)
}

void CAntiHeavyTask::FallbackAttackSafe()
{
	if (leader->GetCircuitDef()->IsRoleMine()) {
		FallbackBasePos(true);
		return;
	}

	CCircuitAI* circuit = manager->GetCircuit();
	circuit->GetMilitaryManager()->FillAttackSafePos(leader, urgentPositions);
	if (urgentPositions.empty()) {
		FallbackStaticSafe();
		return;
	}

	QueryPathMulti(urgentPositions, FALLBACK_PATH_RANGE, [this](const CQueryPathMulti* query) { ApplyAttackSafe(query); });
}

void CAntiHeavyTask::ApplyAttackSafe(const CQueryPathMulti* query)
{
	pPath = query->GetPathInfo();

	if (!pPath->posPath.empty()) {
		position = pPath->posPath.back();
		ActivePath();
	} else {
		FallbackStaticSafe();
	}
}

void CAntiHeavyTask::FallbackStaticSafe()
{
	CCircuitAI* circuit = manager->GetCircuit();
	circuit->GetMilitaryManager()->FillStaticSafePos(leader, urgentPositions);
	if (urgentPositions.empty()) {
		FallbackBasePos(true);
		return;
	}

	QueryPathMulti(urgentPositions, FALLBACK_PATH_RANGE, [this](const CQueryPathMulti* query) { ApplyStaticSafe(query); });
}

void CAntiHeavyTask::ApplyStaticSafe(const CQueryPathMulti* query)
{
	pPath = query->GetPathInfo();

	if (!pPath->posPath.empty()) {
		position = pPath->posPath.back();
		ActivePath();
	} else {
		FallbackBasePos(true);
	}
}

void CAntiHeavyTask::FallbackBasePos(bool isCapped)
{
	CCircuitAI* circuit = manager->GetCircuit();
	position = circuit->GetSetupManager()->GetBasePos();
	QueryPath(leader, position, FALLBACK_PATH_RANGE, isCapped,
			[this, isCapped](const CQueryPathSingle* query) { ApplyBasePos(query, isCapped); });
}

void CAntiHeavyTask::ApplyBasePos(const CQueryPathSingle* query, bool isCapped)
{
	pPath = query->GetPathInfo();

	if (!pPath->posPath.empty()) {
		position = pPath->posPath.back();
		ActivePath();
	} else if (isCapped && (GetPathThreatMod() >= 0.f)) {
		FallbackBasePos(false);  // getting home matters more than the cap: last try through any threat
	} else {
		FallbackCommPos();
	}
}

void CAntiHeavyTask::FallbackCommPos()
{
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	CCircuitUnit* commander = circuit->GetSetupManager()->GetCommander();
	if ((commander != nullptr) &&
		circuit->GetTerrainManager()->CanMoveToPos(leader->GetArea(), commander->GetPos(frame)))
	{
		for (CCircuitUnit* unit : units) {
			unit->Guard(commander, frame + FRAMES_PER_SEC * 60);
			unit->GetTravelAct()->StateWait();
		}
		return;
	}

	position = circuit->GetSetupManager()->GetBasePos();
	for (CCircuitUnit* unit : units) {
		TRY_UNIT(circuit, unit,
			unit->GetUnit()->Fight(position, UNIT_COMMAND_OPTION_RIGHT_MOUSE_KEY, frame + FRAMES_PER_SEC * 60);
		)
		unit->GetTravelAct()->StateWait();
	}
}

} // namespace circuit

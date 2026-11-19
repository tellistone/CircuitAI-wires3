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
static constexpr int CHECK_FRAMES = FRAMES_PER_SEC * 5;  // idle: the park spot (or way to the army) is checked this often
static constexpr int PARK_HOLD_FRAMES = FRAMES_PER_SEC * 30;  // a park spot is kept at least this long
static constexpr float PARK_SWITCH = 1.3f;  // after that, another spot must score this much better
static constexpr float PARK_WALK_SEC = 60.f;  // a spot this many seconds' walk away scores half
static constexpr int PARK_SLOTS = 5;  // spots behind one attack group: straight back, and two to each side
static constexpr int ROUTE_FRAMES = FRAMES_PER_SEC * 5;  // a walk-in route is planned again at most this often
static constexpr int WALK_IN_PAUSE_FRAMES = FRAMES_PER_SEC * 30;  // no walk-in this long after a flee
static constexpr int WALK_IN_RETRY_FRAMES = FRAMES_PER_SEC * 15;  // ... nor after no route to any heavy was found
static constexpr int LOS_GRACE_FRAMES = FRAMES_PER_SEC * 3;  // a long-reload squad keeps a target this long out of LOS
static constexpr int HELD_MIN_FRAMES = FRAMES_PER_SEC * 10;  // a target the whole squad holds off from is skipped this
static constexpr int HELD_MAX_FRAMES = FRAMES_PER_SEC * 60;  // long, doubling each time again, up to this

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
		UpdateHolds(false);  // no attack: fire at will
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
		UpdateHolds(false);
		if (IsQueryReady(leader)) {
			FallbackAttackSafe();  // no flee: old blind pull-back to the attack groups
		}
		return;
	}

	// A scooting leader stands past the stand-off distance FindTarget engages within: keep the fight (and the target)
	// until it's back, else the whole squad would be routed in again while it reloads. A long-reload weapon fires only
	// at what we see: a target just out of LOS is kept a moment with no new orders, rather than dropped and picked again
	const bool isEngaged = (State::ENGAGE == state) && (target != nullptr);
	const bool isLosGrace = isEngaged && leader->GetCircuitDef()->IsLongReload() && !target->IsInLOS()
			&& (frame < targetSeenFrame + LOS_GRACE_FRAMES);
	const bool isTargetsFound = (isEngaged && IsScooting(leader)) || isLosGrace || FindTarget();

	state = (target != nullptr) ? State::ENGAGE : State::ROAM;
	if ((target != nullptr) && target->IsInLOS()) {
		targetSeenFrame = frame;
	}
	isParking = !isTargetsFound && IsParker() && utils::is_valid(park.pos);
	if (isTargetsFound) {
		checkFrame = 0;  // back from a fight or a walk-in: the park spot (or the way to the army) is looked at at once
	}
	UpdateHolds((target != nullptr) && !isLosGrace);  // hold fire only while attacking it
	if (target != nullptr) {
		position = target->GetPos();
		if (isLosGrace) {
			StopAttacks();  // stand and wait: no shot at its radar blip
		} else {
			AttackTarget(frame);
		}
		return;
	}
	StopAttacks();  // a dropped target: else a parked squad (no new orders) would go on shooting or chasing it

	if (!IsQueryReady(leader)) {
		return;
	}

	if (!isTargetsFound) {  // enemyPositions.empty()
		FallbackTargetEmpty();
		return;
	}

	// Walk in to the nearest heavy; routes are planned again every few seconds, not on every update
	if ((frame < routeFrame + ROUTE_FRAMES) && !pPath->posPath.empty()) {
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

	if (isParking) {
		SpreadOut(unit, manager->GetCircuit()->GetLastFrame());
	} else {
		Start(unit);  // back on the squad's path. position stays: it marks the walk-in goal (FindTarget)
	}
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
	return GetDangerStart(enemy) + 0.5f * GetFleeMargin();  // at least the flee margin: the start is at least half of it
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
	return !cdef->IsRoleMine() && cdef->IsLongReload() && !leader->IsWeaponReady(frame);
}

float CAntiHeavyTask::GetHuntReach(const CCircuitDef* cdef)
{
	return cdef->GetMaxRange() + cdef->GetSpeed() * 30.f;
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
	return cdef->IsLongReload() && unit->HasWeapon() && !unit->IsWeaponReady(frame);
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
	for (CCircuitUnit* unit : units) {
		if (unit->GetDGunAct() != nullptr) {
			unit->GetDGunAct()->StateActivate();
		}
	}
	CCircuitDef* cdef = leader->GetCircuitDef();
	if (!cdef->IsRoleMine() && !cdef->IsAbleToFly()) {
		Attack(frame, true);  // firing spots (IFighterTask): spread out, safe (IsSpotSafe)
		// Every unit holds out of range (no firing spot passes): skip it for a while rather than the squad standing still,
		// as attack groups' held bans. Again soon after: twice as long. A unit in range attacks from where it stands
		const AIFloat3& tPos = target->GetPos();
		auto isInRange = [&](CCircuitUnit* u) {
			return u->GetPos(frame).SqDistance2D(tPos) <= SQUARE(u->GetCircuitDef()->GetMaxRange());
		};
		if (IsAllHolding() && std::none_of(units.begin(), units.end(), isInRange)) {
			for (auto it = helds.begin(); it != helds.end();) {  // a skip that ended a while ago is forgotten
				it = (frame >= it->second.until + HELD_MAX_FRAMES) ? helds.erase(it) : std::next(it);
			}
			SHeld& held = helds[target->GetId()];
			held.frames = (held.frames > 0) ? std::min(2 * held.frames, HELD_MAX_FRAMES) : HELD_MIN_FRAMES;
			held.until = frame + held.frames;
		}
		return;
	}
	float minePower = 0.f;
	for (CCircuitUnit* unit : units) {
		AttackDirect(unit, frame, minePower);  // they never scoot
	}
}

void CAntiHeavyTask::AttackDirect(CCircuitUnit* unit, int frame, float& minePower)
{
	if (unit->Blocker() != nullptr) {
		return;
	}
	unit->GetTravelAct()->StateWait();

	if (unit->GetCircuitDef()->IsRoleMine()) {
		const bool isAttack = (target->GetThreat() > minePower);
		minePower += unit->GetCircuitDef()->GetPower();
		if (!isAttack) {
			return;
		}
	}
	if (target->GetUnit()->IsCloaked()) {
		unit->Attack(target->GetPos(), target, frame + FRAMES_PER_SEC * 60);
	} else {
		unit->Attack(target, frame + FRAMES_PER_SEC * 60);
	}
}

bool CAntiHeavyTask::IsQuarry(CEnemyInfo* enemy) const
{
	return leader->GetCircuitDef()->IsQuarry(enemy->GetCircuitDef());
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
	if (!cdef->IsLongReload() || !IsGroundDangerDef(enemy->GetCircuitDef(), true, eRange) || (eRange < range)) {
		return true;
	}
	const int frame = manager->GetCircuit()->GetLastFrame();
	return !IsReloading(frame) && (leader->GetPos(frame).SqDistance2D(enemy->GetPos()) <= SQUARE(range));
}

bool CAntiHeavyTask::IsCoverOn() const
{
	return manager->GetCircuit()->GetMilitaryManager()->GetAllyCover()->IsOn();
}

bool CAntiHeavyTask::IsCoverMapAt(const AIFloat3& pos) const
{
	return IsCoverOn() && !manager->GetCircuit()->GetThreatMap()->IsWaterAt(pos);
}

float CAntiHeavyTask::GetCoverAt(const AIFloat3& pos) const
{
	return manager->GetCircuit()->GetMilitaryManager()->GetAllyCover()->GetCoverAt(pos, {nullptr, nullptr, 1.f}).GetSum();
}

float CAntiHeavyTask::GetFleeThreat(CEnemyInfo* enemy) const
{
	const AIFloat3& ePos = enemy->GetPos();
	return (!IsCoverMapAt(ePos) || (GetCoverAt(ePos) > 0.f)) ? enemy->GetThreat() : std::numeric_limits<float>::max();
}

bool CAntiHeavyTask::IsFleeCovered(const AIFloat3& pos, float enemyThreat, float allyThreat, float allyRadius,
		CCircuitDef::RoleT ownRole) const
{
	if (!IsCoverMapAt(pos)) {
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
	return UpdateFlee((margin < 0.f) ? -1.f : 0.f, 0.25f * margin, true, ahCfg.fleeAllyThreat, ahCfg.fleeAllyRadius,
			ROLE_TYPE(AH));
}

void CAntiHeavyTask::FleeFrom(const AIFloat3& dangerPos)
{
	// Towards the army (FindFleeGoal: the nearest group leader further from the danger, else a defended mex cluster or
	// our base), stopping once out of reach (OnFleeEnd). The park spot we fled from is dropped: a new one after the flee
	ClearPark();
	const AIFloat3* goal = FindFleeGoal(dangerPos);
	if (goal == nullptr) {
		FallbackBasePos(true);
		return;
	}
	QueryPath(leader, *goal, FALLBACK_PATH_RANGE, true, [this](const CQueryPathSingle* query) { ApplyFleePath(query); });
}

void CAntiHeavyTask::ApplyFleePath(const CQueryPathSingle* query)
{
	if (TakePath(query->GetPathInfo())) {
		state = State::DISENGAGE;
	} else {
		FallbackBasePos(true);  // no path under the cap: base, then (last resort) through any threat
	}
}

void CAntiHeavyTask::OnFleeEnd()
{
	CCircuitAI* circuit = manager->GetCircuit();
	circuit->GetMilitaryManager()->NoteAh(leader->GetCircuitDef(), CMilitaryManager::AhNote::FLEE_END);
	walkInFrame = circuit->GetLastFrame() + WALK_IN_PAUSE_FRAMES;  // the way back in leads to the same danger
	// Out of reach: stop here, near the front, rather than walk the rest of the way back
	pPath = std::make_shared<PathInfo>();  // else OnUnitIdle -> Start() would resume the flee path after the Stop
	for (CCircuitUnit* unit : units) {
		if (unit->Blocker() == nullptr) {  // else busy with its own action (jump, ...)
			StopUnit(unit);
		}
	}
}

void CAntiHeavyTask::StopUnit(CCircuitUnit* unit)
{
	unit->GetTravelAct()->StateWait();
	TRY_UNIT(manager->GetCircuit(), unit,
		unit->GetUnit()->Stop();
	)
	unit->ClearTarget();  // IFighterTask::Attack orders it again once there is a target
}

void CAntiHeavyTask::StopAttacks()
{
	// Once per drop: a stopped unit has no target left. Scooting units keep stepping back (UpdateScoots ends them)
	for (CCircuitUnit* unit : units) {
		if ((unit->GetTarget() != nullptr) && (unit->Blocker() == nullptr) && !IsScooting(unit)) {
			StopUnit(unit);
		}
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
	const float margin = GetFleeMargin();
	if (margin < 0.f) {
		return;
	}
	for (auto& kv : manager->GetCircuit()->GetEnemyInfos()) {
		CEnemyInfo* enemy = kv.second;
		float eRange;
		if (!IsGroundDanger(enemy, true, eRange) || IsFleeIgnored(enemy)) {
			continue;  // as FindFleeDanger
		}
		dangers.push_back({enemy->GetPos(), SQUARE(eRange + GetPickMargin(enemy)),
				SQUARE(eRange + GetDangerStart(enemy) + 0.25f * margin), GetFleeThreat(enemy)});
	}
}

bool CAntiHeavyTask::IsSpotSafe(const AIFloat3& spot, CEnemyInfo* excl, bool isKept) const
{
	// Its threat is taken off as the map drew it there (CThreatMap::GetEnemyThreatAt), so no other threat is hidden
	const float exclThreat = (excl != nullptr)
			? manager->GetCircuit()->GetThreatMap()->GetEnemyThreatAt(leader, excl, spot) : 0.f;
	const AIFloat3& leaderPos = leader->GetPos(manager->GetCircuit()->GetLastFrame());
	if (GetTaskThreatExcl(leader, spot, exclThreat) > GetPathThreatAllowance(leader, leaderPos, {spot})) {
		return false;
	}
	float threat = 0.f;
	bool isReached = false;
	for (const SDanger& danger : dangers) {
		if (spot.SqDistance2D(danger.pos) <= (isKept ? danger.sqKeep : danger.sqPick)) {
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

bool CAntiHeavyTask::GetSightLifts(const CEnemyInfo* enemy, float& outAimLift, float& outTargetLift) const
{
	return CAttackTask::GetSightLiftsFor(manager->GetCircuit()->GetMilitaryManager(), leader->GetCircuitDef(), enemy,
			outAimLift, outTargetLift);
}

bool CAntiHeavyTask::IsFireSpotAllowed(const AIFloat3& p) const
{
	return IsSpotSafe(p, target, true);  // the squad is committed to it: flee end zones
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
	const float weaponRange = cdef->GetMaxRange();
	const float minEngage = threatMap->GetSquareSize() * 2.0f;
	// Mines and aircraft engage within LOS / jump range (highestRange), as before
	const bool isOldEngage = cdef->IsRoleMine() || cdef->IsAbleToFly();
	// Stand-off routing goes with the route cap; without it (path_threat < 0, aircraft) the old goals: the heavies
	const bool isStandOff = (GetPathThreatMod() >= 0.f);
	const float pathSlack = isStandOff ? 2 * circuit->GetPathfinder()->GetSquareSize() : 0.f;  // routes end within this of a stand-off point
	// Snipers walk in only to heavies they reach within 30 s (they park meanwhile); the others hunt across the map
	const float sqHuntReach = IsSniper() ? SQUARE(GetHuntReach(cdef)) : std::numeric_limits<float>::max();
	// The current target and the heavy the squad walks in to now (its route ends at that heavy's ring) are judged
	// against the flee's end zones, not the pick zones, so a small shift in their escorts doesn't turn the squad back
	const bool isWalkingIn = (State::ROAM == state) && !isParking && !pPath->posPath.empty();
	const float goalSlack = pathSlack + 2 * threatMap->GetSquareSize();
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
		if (enemy->IsHidden() || !IsQuarry(enemy) || IsHeld(enemy, frame)) {
			continue;  // held off from it lately (AttackTarget)
		}
		const AIFloat3& ePos = enemy->GetPos();
		const float sqDist = pos.SqDistance2D(ePos);
		// A long-reload weapon fires at what we see, not at a radar blip
		const bool isSeen = !cdef->IsLongReload() || enemy->IsInLOS();
		isRadarOnly |= !isSeen && (sqDist <= SQUARE(weaponRange));
		// Already within weapon range (pulled from a fight; or out-ranging it): attack from here, no walk in, so its
		// escorts don't rule it out; the flee still guards the squad. The current target keeps it a bit further out,
		// so a heavy at the edge of range isn't dropped and picked again every check
		const float reach = (enemy == target) ? weaponRange * 1.2f : weaponRange;
		const bool isInReach = isSeen && (sqDist <= SQUARE(reach));
		// No walk in for a while after a flee, nor (snipers) to a heavy beyond GetHuntReach
		if ((!isInReach && ((frame < walkInFrame) || (sqDist > sqHuntReach)))
			|| (ePos.y - map->GetElevationAt(ePos.x, ePos.z) > weaponRange)
			|| (!isStandOff && !terrainMgr->CanMoveToPos(area, ePos)))
		{
			continue;
		}
		// Mines and aircraft (no spot test below): its escorts against our power, as before. Not for one that can't
		// shoot back where we stand
		if (isOldEngage && !isInReach) {
			float eRange;
			const bool isOutranged = !IsGroundDangerDef(enemy->GetCircuitDef(), true, eRange) || (eRange < weaponRange);
			const float maxPower = attackPower * powerMod * GetCountPowerMod() / GetThrDistModAt(ePos);
			if (!isOutranged && (maxPower <= GetTaskThreatExcl(leader, ePos, enemy->GetThreat()))) {
				continue;
			}
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
		// Skip heavies we'd have to attack from a spot that isn't safe (escorts, riots, turrets), else the squad walks
		// in, flees, and walks back in; firing spots pass the same test. Where the attack ends up (0.9x weapon range)
		// and where the route ends. Mines and aircraft attack directly, as before
		const bool isKept = (enemy == target)
				|| (isWalkingIn && (position.SqDistance2D(ePos) <= SQUARE(standOff + goalSlack)));
		if (!isOldEngage && !isInReach
			&& (!IsSpotSafe(GetPointTowards(ePos, pos, weaponRange * 0.9f), enemy, isKept)
				|| !IsSpotSafe(standPos, enemy, isKept)))
		{
			continue;
		}

		if (isFar) {
			enemyPositions.push_back(isStandOff ? standPos : ePos);
			continue;
		}
		// Shots the other squads have ready at it would kill it: theirs
		const float ready = circuit->GetMilitaryManager()->GetAhReadyDamage(enemy, this);
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

bool CAntiHeavyTask::IsHeld(CEnemyInfo* enemy, int frame) const
{
	auto it = helds.find(enemy->GetId());
	return (it != helds.end()) && (frame < it->second.until);
}

bool CAntiHeavyTask::TakePath(const std::shared_ptr<PathInfo>& path)
{
	pPath = path;
	if (pPath->posPath.empty()) {
		return false;
	}
	position = pPath->posPath.back();
	ActivePath();
	return true;
}

void CAntiHeavyTask::ApplyTargetPath(const CQueryPathMulti* query)
{
	if (!TakePath(query->GetPathInfo())) {
		// No way to any heavy under the cap: don't search the same routes again on every re-plan
		walkInFrame = std::max(walkInFrame, manager->GetCircuit()->GetLastFrame() + WALK_IN_RETRY_FRAMES);
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
	} else if (IsCheckDue()) {
		FallbackAttackSafe();  // to the army
	}
}

bool CAntiHeavyTask::IsCheckDue()
{
	// Every CHECK_FRAMES, or at once with no route (a failed one, or stopped after a flee)
	const int frame = manager->GetCircuit()->GetLastFrame();
	if ((frame < checkFrame) && !pPath->posPath.empty()) {
		return false;
	}
	checkFrame = frame + CHECK_FRAMES;
	return true;
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
	// dist back from pos towards our base, but not onto our factory (AddParkSpot skips spots there)
	auto backFrom = [&basePos](const AIFloat3& pos, float dist) {
		return GetPointTowards(pos, basePos, std::max(0.f, std::min(dist, pos.distance2D(basePos) - PARK_CELL / 2.f)));
	};

	// Behind our attack groups, where artillery follows them: follow_dist back, and side slots a claim apart, so a few
	// squads can wait behind one group. The front: towers count only when no group offers a spot
	const float followDist = militaryMgr->GetArtilleryCfg().followDist;
	const float slotGap = 1.1f * GetClaimRadius();
	for (IFighterTask* task : militaryMgr->GetTasks(FightType::ATTACK)) {
		AIFloat3 groupPos;
		float range, power;
		if (!static_cast<CAttackTask*>(task)->GetCoverDisc(frame, groupPos, range, power)) {
			continue;
		}
		const AIFloat3 back = backFrom(groupPos, followDist);
		AIFloat3 side(basePos.z - groupPos.z, 0.f, groupPos.x - basePos.x);  // across the line to our base
		const float len = side.Length2D();
		side = (len > 1.f) ? side / len : RgtVector;
		for (int slot = 0; slot < PARK_SLOTS; ++slot) {
			const int k = (slot % 2 == 1) ? (slot + 1) / 2 : -(slot / 2);  // 0, 1, -1, 2, -2
			AIFloat3 p = back + side * (k * slotGap);
			CTerrainManager::CorrectPosition(p);
			AddParkSpot(p, task, slot, outSpots);
		}
	}

	// Tower groups: the armed statics of each PARK_CELL cell, around their power-weighted centre
	if (outSpots.empty()) {
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
			AddParkSpot(backFrom(towers.sum / towers.power, towers.range / 3.f), nullptr, 0, outSpots);
		}
	}
	std::sort(outSpots.begin(), outSpots.end(), [](const SParkSpot& a, const SParkSpot& b) { return a.score > b.score; });
}

float CAntiHeavyTask::GetClaimRadius() const
{
	return std::max(PARK_CELL / 2.f, 2.f * GetCfg().spacing);
}

void CAntiHeavyTask::AddParkSpot(const AIFloat3& pos, const IFighterTask* group, int slot,
		std::vector<SParkSpot>& outSpots)
{
	CCircuitAI* circuit = manager->GetCircuit();
	CMilitaryManager* militaryMgr = circuit->GetMilitaryManager();
	const CMilitaryManager::SAntiHeavyCfg& ahCfg = GetCfg();
	if ((pos.SqDistance2D(circuit->GetSetupManager()->GetBasePos()) < SQUARE(PARK_CELL / 2.f))  // on our factory
		|| !circuit->GetTerrainManager()->CanMoveToPos(leader->GetArea(), pos))
	{
		return;
	}
	const float claimRadius = GetClaimRadius();
	for (IFighterTask* task : militaryMgr->GetTasks(FightType::AH)) {
		const AIFloat3& claim = static_cast<CAntiHeavyTask*>(task)->GetParkPos();
		if ((task != this) && utils::is_valid(claim) && (claim.SqDistance2D(pos) < SQUARE(claimRadius))) {
			return;  // another squad parks there: spread out
		}
	}
	const float cover = GetCoverAt(pos);
	if ((cover <= 0.f) || !IsSpotSafe(pos, nullptr)) {
		return;
	}

	// Heavy pressure: recent sightings (weighted down over keep_sec), full within our weapon range of the spot
	CCircuitDef* cdef = leader->GetCircuitDef();
	const float range = cdef->GetMaxRange();
	const float keepFrames = ahCfg.keepFrames;
	float pressure = 0.f;
	for (const CMilitaryManager::SHeavySeen& seen : militaryMgr->GetHeavySightings()) {
		pressure += std::max(0.f, 1.f - seen.age / keepFrames) * std::min(1.f, range / std::max(pos.distance2D(seen.pos), 1.f));
	}
	// Cover past 3x what calls off a flee adds nothing: the pressure decides between well-covered spots
	const float usedCover = (ahCfg.fleeAllyThreat > 0.f) ? std::min(cover, 3.f * ahCfg.fleeAllyThreat) : cover;
	const AIFloat3& leaderPos = leader->GetPos(circuit->GetLastFrame());
	const float walkSec = leaderPos.distance2D(pos) / std::max(cdef->GetSpeed(), 1.f);
	float score = usedCover * pressure / (1.f + walkSec / PARK_WALK_SEC);
	if (militaryMgr->IsNearAhRetreat(pos, PARK_CELL / 2.f)) {
		score *= 0.5f;  // one of ours was hit near there lately
	}
	outSpots.push_back({pos, group, slot, cover, score});
}

void CAntiHeavyTask::SetPark(const SParkSpot& spot)
{
	park = spot;
	parkFrame = manager->GetCircuit()->GetLastFrame();
}

void CAntiHeavyTask::UpdatePark()
{
	// Keep the spot for PARK_HOLD_FRAMES, then until another scores PARK_SWITCH times more; leave it once it has lost
	// half its cover. A spot behind an attack group is kept while the group lives, also off the list for a while (the
	// group falls back, an enemy comes near: the flee guards the squad, and drops the spot); a tower spot once it's gone
	FillParkSpots(parkSpots);
	const SParkSpot* current = nullptr;
	for (const SParkSpot& spot : parkSpots) {
		const bool isSame = (park.group != nullptr) ? ((spot.group == park.group) && (spot.slot == park.slot))
				: ((spot.group == nullptr) && (spot.pos.SqDistance2D(park.pos) < SQUARE(PARK_CELL / 2.f)));
		if (isSame) {
			current = &spot;
			break;
		}
	}
	CMilitaryManager* militaryMgr = manager->GetCircuit()->GetMilitaryManager();
	const std::set<IFighterTask*>& groups = militaryMgr->GetTasks(FightType::ATTACK);
	const bool isGroupAlive = (park.group != nullptr)
			&& std::any_of(groups.begin(), groups.end(), [this](const IFighterTask* g) { return g == park.group; });
	const int frame = manager->GetCircuit()->GetLastFrame();
	const float score = (current != nullptr) ? current->score : park.score;
	if (utils::is_valid(park.pos)
		&& ((current != nullptr) ? (current->cover >= 0.5f * park.cover) : isGroupAlive)
		&& ((frame < parkFrame + PARK_HOLD_FRAMES) || parkSpots.empty() || (parkSpots.front().score < PARK_SWITCH * score)))
	{
		if (current != nullptr) {
			park.pos = current->pos;  // a group's spot moves with it
			park.score = current->score;
		}
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
	const bool isCheck = IsCheckDue();
	if (isCheck) {
		UpdatePark();
	}
	if (!utils::is_valid(park.pos)) {
		if (isCheck) {
			FallbackAttackSafe();  // nowhere covered: the old way, to the army
		}
		return;
	}
	// On a check, a new route while the squad isn't there (a new spot, or its group moved on): small moves of the
	// group's leader don't walk the squad around
	if (isCheck && (leader->GetPos(frame).SqDistance2D(park.pos) > SQUARE(GetClaimRadius()))) {
		QueryPath(leader, park.pos, FALLBACK_PATH_RANGE, true, [this](const CQueryPathSingle* query) { ApplyParkPath(query); });
	}
}

void CAntiHeavyTask::ApplyParkPath(const CQueryPathSingle* query)
{
	if (!TakePath(query->GetPathInfo())) {
		ClearPark();  // no way there under the cap
		FallbackAttackSafe();
	}
}

void CAntiHeavyTask::SpreadOut(CCircuitUnit* unit, int frame)
{
	// Side by side, facing away from our base: one shot can't hit two. The n-th other unit (in squad order) stands on
	// alternate sides, (n + 1) / 2 spacings from the leader; left alone between that and one spacing further out
	const float spacing = GetCfg().spacing;
	if ((unit == leader) || (spacing <= 0.f)) {
		return;
	}
	int n = 1;
	for (CCircuitUnit* other : units) {
		if (other == unit) {
			break;
		}
		n += (other != leader) ? 1 : 0;
	}
	const float dist = spacing * ((n + 1) / 2);
	const AIFloat3& leaderPos = leader->GetPos(frame);
	const AIFloat3& uPos = unit->GetPos(frame);
	const float sqDist = uPos.SqDistance2D(leaderPos);
	if ((sqDist >= SQUARE(dist)) && (sqDist <= SQUARE(dist + spacing))) {
		return;
	}
	CCircuitAI* circuit = manager->GetCircuit();
	AIFloat3 dir = circuit->GetSetupManager()->GetBasePos() - leaderPos;
	dir.y = 0.f;
	const float len = dir.Length2D();
	dir = (len > 1.f) ? dir / len : RgtVector;
	AIFloat3 pos = leaderPos + AIFloat3(-dir.z, 0.f, dir.x) * ((n % 2 == 1) ? dist : -dist);
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

void CAntiHeavyTask::UpdateHolds(bool isAttack)
{
	// Parked: hold position (the spot is the cover). Parkers hold fire only while they attack a heavy, so the long
	// reload isn't spent on chaff (the attack on it still fires); fire at will otherwise
	const bool isFireHoldNow = IsParker() && isAttack;
	if ((isParking == isParkHold) && (isFireHoldNow == isFireHold)) {
		return;
	}
	isParkHold = isParking;
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
	if (!TakePath(query->GetPathInfo())) {
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
	if (!TakePath(query->GetPathInfo())) {
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
	if (TakePath(query->GetPathInfo())) {
		return;
	}
	if (isCapped && (GetPathThreatMod() >= 0.f)) {
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

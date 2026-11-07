/*
 * AttackTask.cpp
 *
 *  Created on: Jan 28, 2015
 *      Author: rlcevg
 */

#include "task/fighter/AttackTask.h"
#include "task/TaskManager.h"
#include "map/InfluenceMap.h"
#include "map/ThreatMap.h"
#include "module/FactoryManager.h"
#include "module/MilitaryManager.h"
#include "setup/SetupManager.h"
#include "terrain/TerrainManager.h"
#include "terrain/path/PathFinder.h"
#include "terrain/path/QueryPathSingle.h"
#include "terrain/path/QueryPathMulti.h"
#include "unit/action/FightAction.h"
#include "unit/action/MoveAction.h"
#include "unit/action/SupportAction.h"
#include "unit/enemy/EnemyUnit.h"
#include "unit/CircuitUnit.h"
#include "CircuitAI.h"
#include "util/Utils.h"

#include "spring/SpringMap.h"

#include <algorithm>
#include <cmath>

#include "AISCommands.h"

namespace circuit {

using namespace springai;

// Routes per target decision: each one whose approach to the target it reaches is unsafe drops that target
// and routes again to the rest
static constexpr int MAX_TARGET_ROUTES = 3;
// A target dropped for an unsafe route approach isn't picked again for this long
static constexpr int REJECTED_TARGET_FRAMES = FRAMES_PER_SEC * 6;
// Goals of one target route
static constexpr size_t MAX_TARGET_GOALS = 16;
// For this long after an escape, targets are picked (and their approach checked) at CAUTIOUS_POWER x our strength
static constexpr int CAUTIOUS_FRAMES = FRAMES_PER_SEC * 4;
static constexpr float CAUTIOUS_POWER = 0.75f;
// Stickiness: another target must be at least this much nearer (by distance) to replace the current one
static constexpr float STICKY_DIST = 0.8f;
// A target or front route is kept for this long, instead of planned again at every decision, while nothing changed
static constexpr int ROUTE_REUSE_FRAMES = FRAMES_PER_SEC * 3;
// Within this of the flee destination the group counts as arrived (and the destination must be farther than this)
static constexpr float FLEE_ARRIVE_DIST = 400.f;
// quota.attack_static_cells: a route to a firing cell is kept this long, while its target keeps its anchor
// (buildings don't move; a parked mobile that moves ends it at once: IsSameAnchor)
static constexpr int CELL_ROUTE_REUSE_FRAMES = FRAMES_PER_SEC * 6;
// Cell targets (buildings, parked mobiles) checked for firing cells per target decision; beyond that: discs
static constexpr int MAX_CELL_CHECKS = 32;
// Parked mobiles get cells only if no faster than this x our slowest unit: faster ones leave before a walk arrives
static constexpr float PARKED_SPEED_MOD = 1.25f;
// quota.attack_static_cells.siege_bonus: for a slow mobile type whose weapon range is at least this x our fire range
static constexpr float SIEGE_RANGE_MOD = 1.2f;
// Firing cells' line-of-sight test aims at half the target's height, within these (elmos)
static constexpr float CELL_TARGET_LIFT_MIN = SQUARE_SIZE;
static constexpr float CELL_TARGET_LIFT_MAX = 32.f;
// A cell target with no safe firing cell is banned for REJECTED_TARGET_FRAMES << level, level up to this
static constexpr int CELL_BAN_MAX_LEVEL = 2;
// ... and the ban and its level are dropped this long after the first ban in a row, or when the threat at the target
// changes by more than CELL_BAN_THREAT_CHANGE (fraction)
static constexpr int CELL_BAN_RESET_FRAMES = FRAMES_PER_SEC * 60;
static constexpr float CELL_BAN_THREAT_CHANGE = 0.25f;

// Route ids for ReuseRoute/KeepRoute that aren't enemy ids
static constexpr ICoreUnit::Id FRONT_ROUTE_ID = -1;  // FallbackFrontPos
static constexpr ICoreUnit::Id REINFORCE_ROUTE_ID = -2;  // to the group we reinforce, or the rally host's meeting point
static constexpr ICoreUnit::Id MEET_ROUTE_ID = -3;  // rally host to its meeting point
// Routes to a group or a meeting point (ApplyGroupRoute) are kept this long, and while their goal (a moving group)
// drifts less than this share of the route's length: a far group's route barely changes when its goal moves a little
static constexpr int GROUP_ROUTE_REUSE_FRAMES = FRAMES_PER_SEC * 6;
static constexpr float GROUP_ROUTE_SLACK = 0.1f;
// Rally: after an empty route to the host (or its fight), hold this long before routing again
static constexpr int RALLY_ROUTE_RETRY_FRAMES = FRAMES_PER_SEC * 6;
// Rally host: arrived at the meeting point within this; also how close its route there has to get
static constexpr float MEET_ARRIVE_DIST = 200.f;

namespace {
// Power-weighted median weapon range against one target type, of the non-support units that can hit it (or of all
// units, if none can): the group's fire range for firing cells (quota.attack_static_cells)
float WeightedMedianRange(const std::set<CCircuitUnit*>& units, CCircuitDef::RangeType rt)
{
	static std::vector<std::pair<float, float>> ranges;  // range, power; NOTE: micro-opt
	ranges.clear();
	float total = 0.f;
	for (int pass = 0; (pass < 2) && ranges.empty(); ++pass) {  // 0: shooters of this type; 1 (none): everyone
		for (CCircuitUnit* unit : units) {
			CCircuitDef* cdef = unit->GetCircuitDef();
			const float range = cdef->GetMaxRange(rt);
			if ((pass == 0) && (cdef->IsRoleSupport() || (range <= 0.f))) {
				continue;
			}
			const float power = std::max(cdef->GetPower(), 1e-3f);
			ranges.push_back({range, power});
			total += power;
		}
	}
	std::sort(ranges.begin(), ranges.end());
	float sum = 0.f;
	for (const auto& r : ranges) {
		sum += r.second;
		if (sum >= total * 0.5f) {
			return r.first;
		}
	}
	return ranges.empty() ? 0.f : ranges.back().first;
}
} // namespace

CAttackTask::CAttackTask(ITaskManager* mgr, float minPower, float powerMod)
		: ISquadTask(mgr, FightType::ATTACK, powerMod)
		, minPower(minPower)
		, reinforce(nullptr)
		, reinforceFails(0)
		, reinforceRetryFrame(-1)
		, rallyRouteFrame(-1)
		, meetFailPos(-RgtVector)
		, isMeetWalk(false)
		, isMeetArrived(false)
		, isJoinerNear(false)
		, meetPos(-RgtVector)
		, meetArrivePos(-RgtVector)
		, fleeFrame(0)
		, fleePathFrame(0)
		, fleeQueryId(-1)
		, fleeDest(-RgtVector)
		, isAtFleeDest(false)
		, fleeTargetId(-1)
		, isReplanForced(false)
		, corneredFrame(-1)
		, corneredPos(-RgtVector)
		, isCorneredHold(false)
		, fireRangeLand(0.f)
		, fireRangeWater(0.f)
		, targetRouteTries(0)
		, scaleRefSqDist(0.f)
		, anchorTargetId(-1)
		, pickSqDist(0.f)
		, lastRoute({nullptr, -RgtVector, -1, 0, ROUTE_REUSE_FRAMES, AIFloat3(-RgtVector), -1, 0.f, -1})
		, isHolding(false)
		, stepFrame(0)
		, hitPos(-RgtVector)
		, hitRadius(0.f)
		, hitFrame(-1)
{
	attackRepeat = FRAMES_PER_SEC * 2;  // same ~2 s tempo as the decisions
	CCircuitAI* circuit = manager->GetCircuit();
	float x = rand() % circuit->GetTerrainManager()->GetTerrainWidth();
	float z = rand() % circuit->GetTerrainManager()->GetTerrainHeight();
	position = AIFloat3(x, circuit->GetMap()->GetElevationAt(x, z), z);
}

CAttackTask::~CAttackTask()
{
}

CMilitaryManager* CAttackTask::GetMilitaryMgr() const
{
	return static_cast<CMilitaryManager*>(manager);
}

bool CAttackTask::CanAssignTo(CCircuitUnit* unit) const
{
	assert(leader != nullptr);

	const int frame = manager->GetCircuit()->GetLastFrame();
	const float mergeDist = GetMilitaryMgr()->GetAttackMerge().distance;  // quota.attack_merge.distance
	if (leader->GetPos(frame).SqDistance2D(unit->GetPos(frame)) > SQUARE(mergeDist)) {
		return false;
	}
	return IsCompatible(unit);
}

bool CAttackTask::IsCompatible(CCircuitUnit* unit) const
{
	assert(leader != nullptr);

	CCircuitDef* ldef = leader->GetCircuitDef();
	CCircuitDef* udef = unit->GetCircuitDef();
	const float slower = std::min(ldef->GetSpeed(), udef->GetSpeed());
	const float faster = std::max(ldef->GetSpeed(), udef->GetSpeed());
	if (slower * GetMilitaryMgr()->GetAttackMerge().speedRatio < faster) {  // quota.attack_merge.speed_ratio
		return false;
	}

	// The joining group's leader must not move worse than ours (smaller terrain area). Otherwise it
	// would take the lead (ISquadTask::FindLeader) and drag the group back to it, or, if kept out of
	// the lead, get stranded on routes only our leader can take.
	const STerrainMapArea* ourArea = leader->GetArea();
	const STerrainMapArea* newArea = unit->GetArea();
	if ((newArea != nullptr) && ((ourArea == nullptr) || (newArea->percentOfMap < ourArea->percentOfMap))) {
		return false;
	}
	if (udef->IsAmphibious() && (ldef->IsAmphibious() || ldef->IsLander() || ldef->IsFloater())) {
		return true;
	}
	return (ldef->IsAbleToFly() && udef->IsAbleToFly())
		|| (ldef->IsLander() && udef->IsLander())
		|| (ldef->IsFloater() && udef->IsFloater());
}

void CAttackTask::AssignTo(CCircuitUnit* unit)
{
	ISquadTask::AssignTo(unit);
	CCircuitDef* cdef = unit->GetCircuitDef();
	UpdateHighestRange();

	if (cdef->IsRoleSupport()) {
		unit->PushBack(new CSupportAction(unit));
	}

	int squareSize = manager->GetCircuit()->GetPathfinder()->GetSquareSize();
	ITravelAction* travelAction;
	if (cdef->IsAttrSiege()) {
		travelAction = new CFightAction(unit, squareSize);
	} else {
		travelAction = new CMoveAction(unit, squareSize);
	}
	unit->PushTravelAct(travelAction);
	travelAction->StateWait();
	unit->SetAllowedToJump(cdef->IsAbleToJump() && cdef->IsAttrJump());
	lastRoute.path = nullptr;  // the group changed: next decision plans anew (speed, strength)
}

void CAttackTask::RemoveAssignee(CCircuitUnit* unit)
{
	ISquadTask::RemoveAssignee(unit);
	if (leader == nullptr) {
		manager->AbortTask(this);
		return;
	}
	UpdateHighestRange();
	lastRoute.path = nullptr;  // the group changed: next decision plans anew (speed, strength)
	// Under quota.attack after a loss the group stays together (it used to break up and walk home
	// to gather); Update() looks for a stronger group to reinforce while it is not engaged.
}

void CAttackTask::Merge(ISquadTask* task)
{
	ISquadTask::Merge(task);
	UpdateHighestRange();
	lastRoute.path = nullptr;  // the group changed: next decision plans anew (speed, strength)
	// The newcomers usually start a regroup. A leader standing where the last regroup began counts as stuck
	// (IsMustRegroup), which would drop the leader of a group that holds still while taking in several groups
	prevGroupPos = -RgtVector;
}

void CAttackTask::UpdateHighestRange()
{
	highestRange = 0.f;
	for (CCircuitUnit* unit : units) {
		CCircuitDef* cdef = unit->GetCircuitDef();
		highestRange = std::max(highestRange, std::max(cdef->GetMaxRange(), cdef->GetLosRadius()));
	}

	fireRangeLand = WeightedMedianRange(units, CCircuitDef::RangeType::LAND);
	fireRangeWater = WeightedMedianRange(units, CCircuitDef::RangeType::WATER);
}

void CAttackTask::Start(CCircuitUnit* unit)
{
	if ((State::REGROUP == state) || (State::ENGAGE == state)) {
		return;
	}
	if (!pPath->posPath.empty()) {
		unit->GetTravelAct()->SetPath(pPath, lowestSpeed);
		unit->GetTravelAct()->StateActivate();
	}
}

void CAttackTask::Update()
{
	++updCount;

	/*
	 * Merge into another group if possible
	 */
	ISquadTask* task = GetAttackMergeTask();
	if (task != nullptr) {
		task->Merge(this);
		units.clear();
		// TODO: Deal with cowards?
		manager->AbortTask(this);
		return;
	}
	AbsorbWeakerTasks();

	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	UpdateScoots(frame);  // reload_scoot: step out of reach right after a shot (before the escape: Cornered engages too)
	if (UpdateEscape(frame)) {
		return;
	}

	/*
	 * Regroup if required
	 */
	// A rally host walking to its meeting point doesn't stop to gather: its joiners walk in on their own routes
	if (isMeetWalk && (State::REGROUP == state)) {
		state = State::ROAM;
	}
	const bool wasRegroup = (State::REGROUP == state);
	const bool mustRegroup = !isMeetWalk && IsMustRegroup();
	if (leader == nullptr) {  // all units released as stuck: task aborted
		return;
	}
	if (State::REGROUP == state) {
		if (mustRegroup) {
			const int timeout = frame + FRAMES_PER_SEC * 60;
			for (CCircuitUnit* unit : units) {
				unit->Gather(groupPos, timeout);
				unit->GetTravelAct()->StateWait();
			}
		}
		return;
	}

	/*
	 * Decide every 2 updates (~2 s), or sooner when a unit asks for it (re-plan, sudden threat)
	 */
	const bool isDecision = (updCount % 2 == 0);
	bool isForced = false;  // a unit asked for a re-plan (unsafe path ahead, sudden threat)
	if (isDecision) {
		isReplanForced = false;  // a forced re-plan is allowed again
		ISquadTask::Update();
		if (leader == nullptr) {  // task aborted
			return;
		}
		// Only looked at, not cleared: this decision may plan no route (one still being worked out, or engaging),
		// so the request still re-plans at the next update; but this decision doesn't reuse a route either
		for (CCircuitUnit* unit : units) {
			isForced |= unit->IsForceUpdatePending(frame);
		}
	} else {
		for (CCircuitUnit* unit : units) {
			isForced |= unit->IsForceUpdate(frame);
		}
		if (!isForced) {
			if (wasRegroup && !pPath->posPath.empty()) {
				ActivePath(lowestSpeed);
			}
			return;
		}
	}
	// A normal decision that no unit asked to re-plan may keep a fresh route to the same goal (ReuseRoute)
	Decide(frame, isDecision && !isForced);
}

void CAttackTask::Decide(int frame, bool isReuse)
{
	isHolding = false;  // re-set by HoldPosition if this decision finds nothing to go to

	/*
	 * Reinforce: a group under quota.attack walks to the nearest stronger group instead of hunting alone.
	 * A small group that isn't fighting looks for one every decision (~2 s), unless it gave up recently.
	 */
	// quota.attack_merge.rally: the host meets its joiners (UpdateMeet below); every compatible group that can reach
	// it reinforces it, whatever its size (no retry cooldown)
	CMilitaryManager* militaryMgr = GetMilitaryMgr();
	CAttackTask* rallyHost = militaryMgr->GetRallyHost();
	const bool isHost = (rallyHost == this);
	const bool isJoiner = !isHost && militaryMgr->IsRallyJoiner(this);
	isMeetWalk = false;  // re-set by UpdateMeet
	if (!isHost) {
		ResetMeet();
	}
	if (!isJoiner) {
		meetFailPos = -RgtVector;
	}
	if (isHost) {
		reinforce = nullptr;
	} else if (isJoiner) {
		if (State::ENGAGE != state) {
			reinforce = rallyHost;
		}
	} else if (attackPower >= minPower) {
		reinforce = nullptr;  // an army itself (grown by merges)
	} else if ((reinforce == nullptr) && (State::ENGAGE != state) && (frame >= reinforceRetryFrame)) {
		reinforce = militaryMgr->FindReinforceSquad(leader, attackPower, this);
	}
	const AIFloat3& startPos = leader->GetPos(frame);
	if ((reinforce != nullptr) && (GetReinforceSquad() != nullptr)) {
		// Fight back on the way: a target that passes the usual checks (FindTarget) and is already within range gets
		// attacked; otherwise keep walking
		FindTarget();
		if ((target != nullptr) && (position.SqDistance2D(startPos) < SQUARE(highestRange))) {  // GetRangePos
			state = State::ENGAGE;
			Engage(frame);
			return;
		}
		ClearScaleAnchor();  // walking to a group, not into an accepted fight
		Reinforce(frame, isReuse);
		return;
	}  // else: nothing to join, carry on as a small attack group
	if (isHost && UpdateMeet(frame, isReuse)) {
		return;
	}

	// A route is still being worked out: keep the current pick until it comes back, unless the target is
	// already within range (then engage as usual)
	if (!IsQueryReady(leader) && (target != nullptr)
		&& (GetRangePos(target, frame).SqDistance2D(startPos) >= SQUARE(highestRange)))
	{
		return;
	}
	// Also on enemy-held ground (influence < 0): FindTarget judges threat at the target and along the approach line.
	// With nothing to go for there, FallbackFrontPos holds nearby instead of walking back.
	FindTarget();

	state = State::ROAM;
	if ((target != nullptr) && (position.SqDistance2D(startPos) < SQUARE(highestRange))) {
		state = State::ENGAGE;
		Engage(frame);
		return;
	}
	if (!IsQueryReady(leader)) {
		return;
	}
	if (target == nullptr) {
		FallbackFrontPos(isReuse);
		return;
	}
	if (isReuse && ReuseRoute(target->GetId(), position, frame)) {  // position: GetRangePos (FindTarget)
		return;
	}
	// Route-aware pick: one capped route to all candidates; whichever it reaches first becomes the target,
	// if the route's approach to it is safe
	targetRouteTries = 0;
	RequestTargetPath(routeGoals);
}

/*
 * Group strength and the threat it tolerates
 */

float CAttackTask::GetSafeThreat() const
{
	return std::max(GetStrengthSum(), THREAT_MIN) * powerMod;  // the thr_mod.distance curve is applied per judged spot
}

float CAttackTask::GetPickStrength(int frame) const
{
	// Current-health strength (the measure the escape uses, so we don't pick fights we'd flee from at once)
	return GetSafeThreat() * ((frame < fleeFrame + CAUTIOUS_FRAMES) ? CAUTIOUS_POWER : 1.f);
}

float CAttackTask::GetKeepStrength(const AIFloat3& targetPos, float strength) const
{
	return IsInBaseRadius(targetPos) ? strength : strength * GetMilitaryMgr()->GetAttackKeepThreat();
}

float CAttackTask::GetAllowedThreatAt(const AIFloat3& pos, float strength, float refSqDist) const
{
	CCircuitAI* circuit = manager->GetCircuit();
	const AIFloat3& basePos = circuit->GetSetupManager()->GetBasePos();
	const float bonus = GetMilitaryMgr()->GetBaseThrBonus(basePos.SqDistance2D(pos));
	return strength * bonus / GetThrDistModAt(pos) / std::max(GetNearBaseScaleAt(pos, refSqDist), 1e-3f);
}

float CAttackTask::GetAllowedThreatAt(const AIFloat3& pos, float strength) const
{
	return GetAllowedThreatAt(pos, strength, scaleRefSqDist);
}

float CAttackTask::GetNearBaseScaleAt(const AIFloat3& pos, float refSqDist) const
{
	// Threat at spots nearer our base than the group was when it last picked a target counts less
	// (quadratic, as FindTarget always did). Fixed at pick time, so a group closing in on an enemy
	// between it and our base keeps judging that fight the way it accepted it.
	if (refSqDist <= 0.f) {
		return 1.f;
	}
	const AIFloat3& basePos = manager->GetCircuit()->GetSetupManager()->GetBasePos();
	// Floor (quota.thr_mod.scale_min): caps the near-base boost, so groups near base still flee a far bigger army
	const float scale = std::min(pos.SqDistance2D(basePos) / refSqDist, 1.f);
	return std::max(scale, GetMilitaryMgr()->GetThrScaleMin());
}

void CAttackTask::SetScaleAnchor(const CEnemyInfo* enemy, float refSqDist)
{
	// Keep the scale only for a fight towards home (target nearer our base than the reference):
	// an outward fight never relied on it, so stragglers behind the group get no discount
	const AIFloat3& basePos = manager->GetCircuit()->GetSetupManager()->GetBasePos();
	if ((refSqDist > 0.f) && (enemy->GetPos().SqDistance2D(basePos) < refSqDist)) {
		scaleRefSqDist = refSqDist;
		anchorTargetId = enemy->GetId();
	} else {
		ClearScaleAnchor();
	}
}

void CAttackTask::ClearScaleAnchor()
{
	scaleRefSqDist = 0.f;
	anchorTargetId = -1;
}

bool CAttackTask::IsInBaseRadius(const AIFloat3& pos) const
{
	CCircuitAI* circuit = manager->GetCircuit();
	const float baseRange = GetMilitaryMgr()->GetBaseDefRange();
	const AIFloat3& basePos = circuit->GetSetupManager()->GetBasePos();
	return (baseRange > 0.f) && (pos.SqDistance2D(basePos) < SQUARE(baseRange));
}

template<typename AllowedAt>
bool CAttackTask::IsLineSafe(const AIFloat3& from, const AIFloat3& to, float maxReach, AllowedAt allowedAt) const
{
	// Walk from 'from' towards 'to', out to maxReach (or to 'to' if closer), one threat square at a time
	CThreatMap* threatMap = manager->GetCircuit()->GetThreatMap();
	AIFloat3 dir = to - from;
	dir.y = 0.f;
	const float dist = dir.Length2D();
	if (dist < 1.f) {
		return true;
	}
	dir /= dist;
	const float reach = std::min(maxReach, dist);
	const float step = threatMap->GetSquareSize();
	for (float d = step; d <= reach; d += step) {
		AIFloat3 p = from + dir * d;
		CTerrainManager::CorrectPosition(p);
		const float threat = threatMap->GetThreatAt(leader, p);
		if ((threat > 0.f) && (threat >= allowedAt(p))) {  // allowances are above 0: no threat is always safe
			return false;
		}
	}
	return true;
}

/*
 * Merging and reinforcing
 */

ISquadTask* CAttackTask::GetAttackMergeTask()
{
	// Groups under quota.attack look for a group to join every quota.attack_merge.join_interval[0] (4) updates,
	// others every join_interval[1] (32).
	// Unlike other squads, an attack group may merge while its leader stands on enemy-held ground;
	// the straight line between the leaders must still be safe by the threat allowance (IsSafeMergeLine).
	// Not while fighting: walking off to another group would leave a fight we accepted (absorbing skips us too)
	// During a rally (quota.attack_merge.rally) the host and its joiners check at the small-group interval.
	// The rally is looked up only on updates where either interval falls
	CMilitaryManager* militaryMgr = GetMilitaryMgr();
	const CMilitaryManager::SAttackMerge& cfg = militaryMgr->GetAttackMerge();  // quota.attack_merge.join_interval
	const bool isSmallTick = (updCount % cfg.joinSmall == 1 % cfg.joinSmall);
	const bool isArmyTick = (updCount % cfg.joinArmy == 1 % cfg.joinArmy);
	if ((!isSmallTick && !isArmyTick) || IsFighting()) {
		return nullptr;
	}
	const bool isFast = (attackPower < minPower) || (militaryMgr->GetRallyHost() == this)
			|| militaryMgr->IsRallyJoiner(this);
	return (isFast ? isSmallTick : isArmyTick) ? CheckMergeTask() : nullptr;
}

bool CAttackTask::IsSafeMergeLine(const ISquadTask* host) const
{
	// We walk the straight line to the host's leader, judged by our allowance (as our escape judges it);
	// within quota.attack_merge.support_radius (400) of the host's leader it stands beside us, so both groups'
	// strength counts there
	const CAttackTask* squad = static_cast<const CAttackTask*>(host);  // merging stays within one fight type
	const int frame = manager->GetCircuit()->GetLastFrame();
	const AIFloat3& from = leader->GetPos(frame);
	const AIFloat3& to = squad->GetLeaderPos(frame);
	// At most quota.attack_merge.travel_sec (30 s) of straight travel: with a long merge distance a slow group could
	// otherwise still be walking when the regroup's 60 s limit drops far-away units as stuck
	const CMilitaryManager::SAttackMerge& cfg = GetMilitaryMgr()->GetAttackMerge();
	if (from.SqDistance2D(to) >= SQUARE(cfg.travelSec * lowestSpeed)) {
		return false;
	}
	const float own = GetSafeThreat();
	const float together = own + squad->GetSafeThreat();
	const float sqSupport = SQUARE(cfg.supportRadius);
	return IsLineSafe(from, to, std::numeric_limits<float>::max(), [&](const AIFloat3& p) {
		return GetAllowedThreatAt(p, (p.SqDistance2D(to) < sqSupport) ? together : own);
	});
}

void CAttackTask::AbsorbWeakerTasks()
{
	// A group at or above quota.attack pulls in attack groups under quota.attack near it, every
	// quota.attack_merge.absorb_interval (8) updates.
	// Same rules as joining (CanAssignTo: speed ratio, leaders within the merge distance, movement type,
	// never a worse mover than our leader; reachable; IsSafeMergeLine incl. its travel limit), seen from the
	// small group's side.
	// The rally host (quota.attack_merge.rally) absorbs any compatible group, armies too, whatever its own size
	// A rally host with a joiner within the merge distance checks every 2 updates, so the merge fires while it waits
	const bool isHost = (GetMilitaryMgr()->GetRallyHost() == this);
	const int absorbInterval = (isHost && isJoinerNear) ? 2 : GetMilitaryMgr()->GetAttackMerge().absorb;
	if ((!isHost && (attackPower < minPower)) || (updCount % absorbInterval != 5 % absorbInterval)) {
		return;
	}

	CCircuitAI* circuit = manager->GetCircuit();
	const AIFloat3& pos = leader->GetPos(circuit->GetLastFrame());
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();

	static std::vector<CAttackTask*> weaklings;  // NOTE: micro-opt
	for (IFighterTask* t : GetMilitaryMgr()->GetTasks(fightType)) {
		CAttackTask* candy = static_cast<CAttackTask*>(t);
		CCircuitUnit* candyLeader = candy->GetLeader();
		// Cheap checks first; the small group walks to us, so the line is judged from its side
		if ((candy == this) || candy->IsDead() || (candyLeader == nullptr)
			|| (!isHost && (candy->GetAttackPower() >= minPower))
			|| !CanAssignTo(candyLeader)
			|| candy->IsFighting()  // let it finish its fight (it doesn't join either)
			|| !terrainMgr->CanMoveToPos(candyLeader->GetArea(), pos)
			|| !candy->IsSafeMergeLine(this))
		{
			continue;
		}
		weaklings.push_back(candy);
	}

	// Merge after the loop: aborting a task removes it from the set being iterated
	for (CAttackTask* candy : weaklings) {
		Merge(candy);  // CanAssignTo ruled out units that would take the lead
		candy->units.clear();
		manager->AbortTask(candy);
	}
	weaklings.clear();
}

CAttackTask* CAttackTask::GetReinforceSquad()
{
	CMilitaryManager* militaryMgr = GetMilitaryMgr();
	// NOTE: the army may be deleted already, do not dereference before lookup
	const std::set<IFighterTask*>& tasks = militaryMgr->GetTasks(fightType);
	if ((tasks.find(static_cast<IFighterTask*>(reinforce)) != tasks.end()) && !reinforce->IsDead()
		&& (reinforce->GetLeader() != nullptr)
		&& ((reinforce->GetAttackPower() > attackPower) || (reinforce == militaryMgr->GetRallyHost())))
	{
		return reinforce;  // a rally host is kept whatever its size
	}
	// Group gone or no longer stronger: pick the nearest other stronger one
	reinforce = militaryMgr->FindReinforceSquad(leader, attackPower, this);
	return reinforce;
}

void CAttackTask::Reinforce(const int frame, bool isReuse)
{
	SetTarget(nullptr);
	state = State::ROAM;

	if (!IsQueryReady(leader)) {
		return;
	}
	// Threat-aware path to the group's leader, capped like every other route (GetPathThreatCap: the common
	// allowance). It only has to get within reinforce_range (800), so the hot zone around a fighting group doesn't block
	// it; merging takes over inside the merge distance (CheckMergeTask from our side, AbsorbWeakerTasks from
	// an army's side). Range: quota.attack_merge.reinforce_range. Rally host: PickRallyGoal
	const AIFloat3& startPos = leader->GetPos(frame);
	const bool isRally = (reinforce == GetMilitaryMgr()->GetRallyHost());
	AIFloat3 goal = reinforce->GetLeaderPos(frame);  // validated by GetReinforceSquad this update
	bool isToMeet = false;
	if (isRally && !PickRallyGoal(frame, goal, isToMeet)) {
		position = startPos;
		GatherAt(startPos);  // nothing safe near the host's fight and we are close already: stand here
		return;
	}
	position = goal;
	if (isReuse && ReuseRoute(REINFORCE_ROUTE_ID, goal, frame)) {
		return;
	}
	if (isRally && !isToMeet && (frame < rallyRouteFrame)) {
		HoldPosition();  // the last route to the host came back empty: a new one would likely fail too
		return;
	}
	// Never stop farther out than we can merge from (the merge distance, or travel_sec at our slowest speed), with
	// a margin for the host moving: otherwise a slow group would park short of the host and never merge
	const CMilitaryManager::SAttackMerge& cfg = GetMilitaryMgr()->GetAttackMerge();
	const float mergeReach = std::min(cfg.distance, cfg.travelSec * lowestSpeed);
	const float pathRange = std::min(cfg.reinforceRange, mergeReach * 0.8f);
	const float slack = GROUP_ROUTE_SLACK * startPos.distance2D(goal);
	RunGroupRoute(frame, goal, pathRange, [this, goal, isToMeet, slack](const CQueryPathSingle* query) {
		this->ApplyReinforcePath(query, goal, isToMeet, slack);
	});
}

bool CAttackTask::PickRallyGoal(int frame, AIFloat3& inOutGoal, bool& outIsToMeet) const
{
	// Reinforcing the rally host (quota.attack_merge.rally), one goal rule:
	// - the host fights: the safe edge of its fight (FindFightEdge), to add our strength without walking into it alone;
	// - the host meets us and we are farther than the merge distance: its meeting point, so both sides converge
	//   instead of us chasing it (unless our last route to that same point came back empty);
	// - otherwise: the host itself (inOutGoal as given; inside the merge distance, so the merge always fires).
	// false: nothing safe near the fight and we are close already, stand still
	outIsToMeet = false;
	if (State::ENGAGE == reinforce->state) {
		const AIFloat3 hostPos = inOutGoal;
		return FindFightEdge(frame, hostPos, inOutGoal);
	}
	const float mergeDist = GetMilitaryMgr()->GetAttackMerge().distance;
	AIFloat3 meet;
	if ((leader->GetPos(frame).SqDistance2D(inOutGoal) > SQUARE(mergeDist)) && reinforce->GetMeetPoint(frame, meet)
		&& (meet.SqDistance2D(meetFailPos) > SQUARE(1.f)))
	{
		inOutGoal = meet;
		outIsToMeet = true;
	}
	return true;
}

void CAttackTask::ApplyReinforcePath(const CQueryPathSingle* query, const AIFloat3& goal, bool isToMeet, float slack)
{
	if (ApplyGroupRoute(query, REINFORCE_ROUTE_ID, goal, slack)) {
		reinforceFails = 0;
		if (isToMeet) {
			meetFailPos = -RgtVector;
		}
		return;
	}
	const int frame = manager->GetCircuit()->GetLastFrame();
	if ((reinforce != nullptr) && (reinforce == GetMilitaryMgr()->GetRallyHost())) {
		// Rally: never hunt alone. No route to the meeting point: route to the host itself from the next decision
		// (until the host picks another point); no route to the host either: hold, and retry after a while
		if (isToMeet) {
			meetFailPos = goal;
		} else {
			rallyRouteFrame = frame + RALLY_ROUTE_RETRY_FRAMES;
		}
		HoldPosition();
		return;
	}
	const CMilitaryManager::SAttackMerge& cfg = GetMilitaryMgr()->GetAttackMerge();
	if (++reinforceFails >= cfg.reinforceFails) {
		// No safe route quota.attack_merge.reinforce_fails times in a row (10: ~20 s at a decision every ~2 s):
		// act as a small attack group for reinforce_retry (10 s), then look again
		// (merge checks and absorbing still apply meanwhile)
		reinforce = nullptr;
		reinforceFails = 0;
		reinforceRetryFrame = frame + cfg.reinforceRetry;
	}
}

void CAttackTask::RunGroupRoute(int frame, const AIFloat3& goal, float range,
		std::function<void (const CQueryPathSingle*)>&& onComplete)
{
	// A capped route (reinforce costs) from our leader to within range of goal
	CCircuitAI* circuit = manager->GetCircuit();
	CPathFinder* pathfinder = circuit->GetPathfinder();
	const AIFloat3& startPos = leader->GetPos(frame);
	std::shared_ptr<IPathQuery> query = pathfinder->CreatePathSingleQuery(
			leader, circuit->GetThreatMap(), frame,
			startPos, goal, std::max(range, static_cast<float>(pathfinder->GetSquareSize())), nullptr,
			GetPathThreatCap(startPos, {goal}));
	SetRouteThreat(query.get(), AttackRoute::REINFORCE);
	RunRoute(query, [onComplete = std::move(onComplete)](const IPathQuery* query) {
		onComplete(static_cast<const CQueryPathSingle*>(query));
	});
}

bool CAttackTask::ApplyGroupRoute(const CQueryPathSingle* query, ICoreUnit::Id routeId, const AIFloat3& goal,
		float slack)
{
	// Walk the route and keep it for ReuseRoute (GROUP_ROUTE_REUSE_FRAMES, the goal may drift by slack); false: empty
	pPath = query->GetPathInfo();
	if (pPath->path.empty()) {
		return false;
	}
	if (pPath->path.size() > 2) {
		ActivePath(lowestSpeed);
	}
	KeepRoute(routeId, goal);
	lastRoute.reuseFrames = GROUP_ROUTE_REUSE_FRAMES;
	lastRoute.goalSlack = slack;
	return true;
}

bool CAttackTask::FindFightEdge(int frame, const AIFloat3& hostPos, AIFloat3& outPos) const
{
	// The rally host is fighting: its position stepped back towards us until the threat there is under our own
	// allowance (as our route judges it), so we join the fight from its safe side.
	// Nothing safe on the steps: from farther away than one step past them, walk to that point (the edge is looked
	// for again on the way); closer: false, the caller stands still (a route to our own spot would be a one-node
	// path that leaves the last move order running)
	constexpr float STEP = 200.f;
	constexpr int MAX_STEPS = 12;
	constexpr float FAR_DIST = (MAX_STEPS + 2) * STEP;  // 2800: a 400 gap past the last step checked
	const AIFloat3& ownPos = leader->GetPos(frame);
	const float dist = ownPos.distance2D(hostPos);
	if (dist < 1.f) {
		outPos = hostPos;
		return true;
	}
	CCircuitAI* circuit = manager->GetCircuit();
	CThreatMap* threatMap = circuit->GetThreatMap();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	const AIFloat3 dir = (ownPos - hostPos) / dist;
	const float own = GetSafeThreat();
	for (int k = 0; (k <= MAX_STEPS) && (k * STEP < dist); ++k) {
		AIFloat3 p = hostPos + dir * (k * STEP);
		CTerrainManager::CorrectPosition(p);
		if (!terrainMgr->CanMoveToPos(leader->GetArea(), p)) {
			continue;
		}
		const float threat = threatMap->GetThreatAt(leader, p);
		if ((threat <= 0.f) || (threat < GetAllowedThreatAt(p, own))) {
			outPos = p;
			return true;
		}
	}
	if (dist > FAR_DIST) {
		outPos = hostPos + dir * FAR_DIST;
		CTerrainManager::CorrectPosition(outPos);
		return true;
	}
	return false;
}

/*
 * Rally host: meeting the groups that reinforce it (quota.attack_merge.rally)
 */

bool CAttackTask::GetMeetPoint(int frame, AIFloat3& outPos) const
{
	if (!utils::is_valid(meetPos) || (State::ENGAGE == state) || (frame < fleeFrame)) {
		return false;  // engaged or fleeing: joiners go to the host itself
	}
	outPos = meetPos;
	return true;
}

void CAttackTask::ResetMeet()
{
	isMeetArrived = false;
	isJoinerNear = false;
	meetPos = -RgtVector;
	meetTracks.clear();
}

bool CAttackTask::UpdateMeet(int frame, bool isReuse)
{
	// We meet while a significant joiner is on its way: until it merges, stalls (TrackJoiner) or drops out of
	// JOINER_MAX_SEC. Small joiners still come and merge, they just don't hold us
	static std::vector<CAttackTask*> significant;  // NOTE: micro-opt
	CollectSignificantJoiners(frame, significant);
	if (significant.empty()) {
		isMeetArrived = false;
		meetPos = -RgtVector;
		return false;
	}

	// Fight back on the way, as reinforcing groups do; the meeting goes on after the fight (GetMeetPoint gives
	// joiners no point while we engage)
	const AIFloat3& pos = leader->GetPos(frame);
	FindTarget();
	if ((target != nullptr) && (target->GetPos().SqDistance2D(pos) < SQUARE(highestRange))) {
		state = State::ENGAGE;
		isMeetArrived = false;
		Engage(frame);
		return true;
	}
	ClearScaleAnchor();
	SetTarget(nullptr);
	state = State::ROAM;

	meetPos = FindMeetPoint(frame, significant);
	position = meetPos;
	if (IsAtMeetPoint(pos)) {
		// Wait at the point (regrouping allowed). No quiet-spot search: the point was judged safe, and the joiners
		// aim at it
		GatherAt(meetPos);
		return true;
	}
	isMeetWalk = true;
	if ((isReuse && ReuseRoute(MEET_ROUTE_ID, meetPos, frame)) || !IsQueryReady(leader)) {
		return true;
	}
	const AIFloat3 goal = meetPos;
	RunGroupRoute(frame, goal, MEET_ARRIVE_DIST, [this, goal](const CQueryPathSingle* query) {
		if (!this->ApplyGroupRoute(query, MEET_ROUTE_ID, goal, 0.f)) {
			this->HoldPosition();  // no safe route there: wait here, the joiners still come to us
		}
	});
	return true;
}

void CAttackTask::CollectSignificantJoiners(int frame, std::vector<CAttackTask*>& outSignificant)
{
	// Joiners: groups reinforcing us. Significant: within JOINER_MAX_SEC of travel, still closing in (TrackJoiner),
	// with SIGNIFICANT_SHARE of our strength (KEEP_SHARE once counted). Also sets isJoinerNear (fast absorbing)
	constexpr float SIGNIFICANT_SHARE = 0.25f;
	constexpr float KEEP_SHARE = 0.20f;
	constexpr float JOINER_MAX_SEC = 60.f;
	CMilitaryManager* militaryMgr = GetMilitaryMgr();
	const AIFloat3& pos = leader->GetPos(frame);
	const float strength = GetRallyStrength();
	const float sqMergeDist = SQUARE(militaryMgr->GetAttackMerge().distance);
	outSignificant.clear();
	isJoinerNear = false;
	for (IFighterTask* t : militaryMgr->GetTasks(FightType::ATTACK)) {
		CAttackTask* j = static_cast<CAttackTask*>(t);
		if ((j == this) || j->IsDead() || (j->GetLeader() == nullptr) || (j->reinforce != this)) {
			continue;
		}
		const AIFloat3& jPos = j->GetLeaderPos(frame);
		const float sqDist = jPos.SqDistance2D(pos);
		isJoinerNear |= (sqDist <= sqMergeDist);
		const float sec = sqrtf(sqDist) / std::max(j->lowestSpeed, 1.f);
		if (sec > JOINER_MAX_SEC) {
			continue;
		}
		bool isStalled;
		SMeetTrack& track = TrackJoiner(j, sec, frame, isStalled);
		const float share = track.isSignificant ? KEEP_SHARE : SIGNIFICANT_SHARE;
		track.isSignificant = !isStalled && (j->GetRallyStrength() >= share * strength);
		if (track.isSignificant) {
			outSignificant.push_back(j);
		}
	}
	// Forget joiners not looked at this decision
	meetTracks.erase(std::remove_if(meetTracks.begin(), meetTracks.end(), [frame](const SMeetTrack& m) {
		return m.seenFrame != frame;
	}), meetTracks.end());
}

bool CAttackTask::IsAtMeetPoint(const AIFloat3& pos)
{
	// Arrived within MEET_ARRIVE_DIST of the point; still there until pushed farther than MEET_LEAVE_DIST away
	// (escape) or the point moved MEET_MOVED_DIST since: then walk there again by a route
	constexpr float MEET_LEAVE_DIST = 600.f;
	constexpr float MEET_MOVED_DIST = 400.f;
	const float sqMeetDist = meetPos.SqDistance2D(pos);
	if (isMeetArrived && ((sqMeetDist > SQUARE(MEET_LEAVE_DIST))
		|| (meetPos.SqDistance2D(meetArrivePos) > SQUARE(MEET_MOVED_DIST))))
	{
		isMeetArrived = false;
	}
	if (!isMeetArrived && (sqMeetDist < SQUARE(MEET_ARRIVE_DIST))) {
		isMeetArrived = true;
		meetArrivePos = meetPos;
	}
	return isMeetArrived;
}

CAttackTask::SMeetTrack& CAttackTask::TrackJoiner(const CAttackTask* joiner, float gapSec, int frame, bool& outIsStalled)
{
	// A joiner whose gap to us (its travel time) hasn't shrunk by MEET_PROGRESS_SEC within MEET_STALL_FRAMES is
	// stalled: stuck behind a wall of threat, fighting on the way, or with no route. While stalled its best gap
	// follows it up, so MEET_PROGRESS_SEC of fresh progress counts it again. A track not looked at for a while
	// (it was too far) starts afresh
	constexpr float MEET_PROGRESS_SEC = 5.f;
	constexpr int MEET_STALL_FRAMES = FRAMES_PER_SEC * 20;
	constexpr int MEET_TRACK_GAP_FRAMES = FRAMES_PER_SEC * 6;
	auto it = std::find_if(meetTracks.begin(), meetTracks.end(), [joiner](const SMeetTrack& m) {
		return m.task == joiner;
	});
	if ((it == meetTracks.end()) || (frame - it->seenFrame > MEET_TRACK_GAP_FRAMES)) {
		const SMeetTrack fresh = {joiner, gapSec, frame, frame, false};
		outIsStalled = false;
		if (it == meetTracks.end()) {
			meetTracks.push_back(fresh);
			return meetTracks.back();
		}
		*it = fresh;
		return *it;
	}
	it->seenFrame = frame;
	if (State::ENGAGE == state) {
		it->bestFrame = frame;  // we fight, joiners wait at its edge (FindFightEdge): no stall clock meanwhile
	}
	if (gapSec <= it->bestSec - MEET_PROGRESS_SEC) {
		it->bestSec = gapSec;
		it->bestFrame = frame;
	}
	outIsStalled = (frame - it->bestFrame > MEET_STALL_FRAMES);
	if (outIsStalled) {
		it->bestSec = std::max(it->bestSec, gapSec);
	}
	return *it;
}

AIFloat3 CAttackTask::FindMeetPoint(int frame, const std::vector<CAttackTask*>& joiners) const
{
	constexpr float MAX_PULL_BACK = 1500.f;  // never more than this much nearer our base than we are
	constexpr float STICKY_DIST = 400.f;
	constexpr float STICKY_SEC = 5.f;
	constexpr int STEPS = 5;

	const AIFloat3& hostPos = leader->GetPos(frame);
	CCircuitAI* circuit = manager->GetCircuit();
	const AIFloat3& basePos = circuit->GetSetupManager()->GetBasePos();

	// Joiners' strength-weighted centroid and their slowest speed
	AIFloat3 centroid = ZeroVector;
	float weight = 0.f;
	float joinSpeed = std::numeric_limits<float>::max();
	for (CAttackTask* j : joiners) {
		const float s = std::max(j->GetRallyStrength(), 1e-3f);
		centroid += j->GetLeaderPos(frame) * s;
		weight += s;
		joinSpeed = std::min(joinSpeed, j->lowestSpeed);
	}
	centroid /= weight;
	const float hostSpeed = std::max(lowestSpeed, 1.f);
	joinSpeed = std::max(joinSpeed, 1.f);
	// Slower side's travel time to p: what the meeting waits for
	auto meetSec = [&](const AIFloat3& p) {
		return std::max(p.distance2D(hostPos) / hostSpeed, p.distance2D(centroid) / joinSpeed);
	};

	AIFloat3 target;
	if (IsInBaseRadius(GetMilitaryMgr()->GetRallyBlobPos())) {
		target = basePos;  // the blob is at our base: meet there
	} else {
		// Both sides arrive together
		target = hostPos + (centroid - hostPos) * (hostSpeed / (hostSpeed + joinSpeed));
		// Not more than MAX_PULL_BACK nearer our base than we are: if it is, the first spot on host -> target that is
		// exactly that far back, i.e. the smaller t with |(host - base) + t (target - host)| = r (2D)
		const float r = hostPos.distance2D(basePos) - MAX_PULL_BACK;
		if (target.SqDistance2D(basePos) < SQUARE(r)) {  // so r > 0, and the host itself is beyond r
			const float vx = hostPos.x - basePos.x, vz = hostPos.z - basePos.z;
			const float dx = target.x - hostPos.x, dz = target.z - hostPos.z;
			const float a = dx * dx + dz * dz;
			const float b = vx * dx + vz * dz;
			const float c = vx * vx + vz * vz - r * r;
			const float t = (-b - sqrtf(std::max(b * b - a * c, 0.f))) / a;
			target = hostPos + (target - hostPos) * t;
		}
	}

	// Step back towards the host until the spot is safe and reachable for everyone (the host's own spot always is)
	AIFloat3 best = hostPos;
	for (int k = 0; k < STEPS; ++k) {
		AIFloat3 p = hostPos + (target - hostPos) * (1.f - static_cast<float>(k) / STEPS);
		CTerrainManager::CorrectPosition(p);
		if (IsMeetSpotOk(p, frame, joiners)) {
			best = p;
			break;
		}
	}

	// Sticky: keep the current point while it is still fine, unless the new one is far off and saves real time
	if (utils::is_valid(meetPos) && IsMeetSpotOk(meetPos, frame, joiners)
		&& ((best.SqDistance2D(meetPos) <= SQUARE(STICKY_DIST)) || (meetSec(meetPos) - meetSec(best) <= STICKY_SEC)))
	{
		return meetPos;
	}
	return best;
}

bool CAttackTask::IsMeetSpotOk(const AIFloat3& pos, int frame, const std::vector<CAttackTask*>& joiners) const
{
	CTerrainManager* terrainMgr = manager->GetCircuit()->GetTerrainManager();
	if (!terrainMgr->CanMoveToPos(leader->GetArea(), pos)) {
		return false;
	}
	for (CAttackTask* j : joiners) {
		if (!terrainMgr->CanMoveToPos(j->GetLeader()->GetArea(), pos)) {
			return false;
		}
	}
	const float own = GetSafeThreat();
	const AIFloat3& hostPos = leader->GetPos(frame);
	const float threat = manager->GetCircuit()->GetThreatMap()->GetThreatAt(leader, pos);
	if ((threat > 0.f) && (threat >= GetAllowedThreatAt(pos, own))) {
		return false;
	}
	return IsLineSafe(hostPos, pos, std::numeric_limits<float>::max(), [&](const AIFloat3& p) {
		return GetAllowedThreatAt(p, own);
	});
}

/*
 * Escape
 */

bool CAttackTask::UpdateEscape(int frame)
{
	// Checked on every update (not just every decision), also while engaging or reinforcing.
	// Not while defending: a fight with a target inside the base defence radius is held (no flee, no pull-back)
	const bool isDanger = IsInDanger(frame, GetMilitaryMgr()->GetAttackFleeShare());
	const bool isHoldGround = IsHoldingGround(frame) || (isDanger && isCorneredHold);
	isAtFleeDest = false;
	const bool isFleeing = isDanger && !isHoldGround && Flee(frame);
	// Already at the flee destination (stronger group, haven, base): no cornered rule, carry on as usual
	// (pull-back and normal decisions)
	if (isDanger && !isFleeing && !isAtFleeDest) {
		// Cornered (holding ground, or nowhere better to run): don't stand and take it.
		// Moving to a quieter spot ends ENGAGE, so remember that this group is holding ground.
		isCorneredHold = isHoldGround;
		Cornered(frame);
		return true;
	}
	corneredPos = -RgtVector;
	isCorneredHold = false;
	if (!isFleeing && !isHoldGround) {
		PullBackEndangered(frame);
	}
	if (isFleeing || (frame < fleeFrame)) {
		// keep running along the escape path for a moment instead of turning straight back;
		// still send units with empty shields to retreat (normally done at each decision)
		if (updCount % 2 == 0) {
			ISquadTask::Update();
		}
		return true;
	}
	// One forced re-plan per normal decision interval (~2 s): if the new route is still unsafe ahead
	// (route caps are looser than per-spot allowances), wait for the next normal decision
	if (isPathAheadUnsafe && !isReplanForced && (frame >= lastTouched + FRAMES_PER_SEC)) {
		lastTouched = frame;
		isReplanForced = true;
		for (CCircuitUnit* unit : units) {
			unit->ForceUpdate(frame);  // re-plan now instead of walking into it
		}
	}
	return false;
}

bool CAttackTask::IsHoldingGround(int frame) const
{
	// Only inside the base defence radius (where thr_mod.base_bonus applies): out in the field the enemy
	// is nearer our base than we are in about half of all fights, and those should still be escapable
	return (State::ENGAGE == state) && (target != nullptr) && IsInBaseRadius(target->GetPos());
}

bool CAttackTask::Flee(int frame)
{
	const AIFloat3 dest = FindFleeDest(frame);
	if (leader->GetPos(frame).SqDistance2D(dest) < SQUARE(FLEE_ARRIVE_DIST)) {
		isAtFleeDest = true;
		fleeFrame = std::min(fleeFrame, frame);  // stop running, but keep the cautious window after it
		return false;  // already there: running gets us nowhere
	}

	if (target != nullptr) {
		fleeTargetId = target->GetId();  // kept for the next pick (FindTarget)
	}
	SetTarget(nullptr);
	state = State::ROAM;
	isHolding = false;
	ClearScaleAnchor();  // running: judge danger at full value, also on the way home
	const bool wasFleeing = (frame < fleeFrame + FRAMES_PER_SEC);  // grace: still in the same escape
	fleeFrame = frame + FRAMES_PER_SEC * 2;
	if (wasFleeing && (frame < fleePathFrame + FRAMES_PER_SEC)) {
		return true;  // escape path is fresh
	}
	// Don't replace an escape query that hasn't come back yet (a slow shared pathfinder would otherwise
	// never deliver a way out); give it up to 5 s
	if (wasFleeing && IsFleeQueryPending() && (frame < fleePathFrame + FRAMES_PER_SEC * 5)) {
		return true;
	}
	fleePathFrame = frame;
	fleeDest = dest;

	// No straight move first: it could run through the enemy. Units keep their current orders until the
	// threat-aware path arrives (the straight move is only the fallback when no path is found).
	CCircuitAI* circuit = manager->GetCircuit();
	CPathFinder* pathfinder = circuit->GetPathfinder();
	std::shared_ptr<IPathQuery> query = pathfinder->CreatePathSingleQuery(
			leader, circuit->GetThreatMap(), frame,
			leader->GetPos(frame), dest, pathfinder->GetSquareSize());  // threat as cost, no cap: least-threat way out
	SetRouteThreat(query.get(), AttackRoute::FLEE);
	fleeQueryId = query->GetId();
	RunRoute(query, [this](const IPathQuery* query) {  // supersedes any pending target/reinforce path
		this->fleeQueryId = -1;
		this->ApplyFleePath(static_cast<const CQueryPathSingle*>(query));
	});
	return true;
}

AIFloat3 CAttackTask::FindFleeDest(int frame) const
{
	CCircuitAI* circuit = manager->GetCircuit();
	CMilitaryManager* militaryMgr = GetMilitaryMgr();
	CThreatMap* threatMap = circuit->GetThreatMap();

	// The nearest stronger attack group (fight beside it, then merge), if the two groups together can stand there:
	// it may be in this same fight, or past the enemy. Not the rally host by preference: it may be far away
	CAttackTask* squad = militaryMgr->FindReinforceSquad(leader, attackPower, this, false);
	if (squad != nullptr) {
		const AIFloat3& squadPos = squad->GetLeaderPos(frame);
		const float together = GetSafeThreat() + squad->GetSafeThreat();
		if (threatMap->GetThreatAt(leader, squadPos) < GetAllowedThreatAt(squadPos, together)) {
			return squadPos;
		}
	}

	// The nearest defence point of a metal cluster we hold (FillFriendlyClusterPos) where we can stand alone,
	// judged at full value like the escape itself (no near-base discount from the fight we're leaving), not farther
	// from our base than we are (never run forward), and not right here (we'd count as arrived and stop running)
	static F3Vec clusterPositions;  // NOTE: micro-opt
	militaryMgr->FillFriendlyClusterPos(leader, clusterPositions);
	const AIFloat3& pos = leader->GetPos(frame);
	const AIFloat3& basePos = circuit->GetSetupManager()->GetBasePos();
	const float sqBaseDist = pos.SqDistance2D(basePos);
	const float strength = GetSafeThreat();
	AIFloat3 dest = -RgtVector;
	float minSqDist = std::numeric_limits<float>::max();
	for (const AIFloat3& p : clusterPositions) {
		const float sqDist = pos.SqDistance2D(p);
		if ((sqDist >= SQUARE(FLEE_ARRIVE_DIST)) && (sqDist < minSqDist) && (p.SqDistance2D(basePos) <= sqBaseDist)
			&& (threatMap->GetThreatAt(leader, p) < GetAllowedThreatAt(p, strength, 0.f)))
		{
			minSqDist = sqDist;
			dest = p;
		}
	}
	if (utils::is_valid(dest)) {
		return dest;
	}

	dest = circuit->GetFactoryManager()->GetClosestHaven(leader);
	return utils::is_valid(dest) ? dest : basePos;
}

bool CAttackTask::IsFleeQueryPending() const
{
	if (fleeQueryId < 0) {
		return false;
	}
	auto it = pathQueries.find(leader);
	return (it != pathQueries.end()) && (it->second != nullptr) && (it->second->GetId() == fleeQueryId)
			&& !it->second->IsLost();
}

void CAttackTask::ApplyFleePath(const CQueryPathSingle* query)
{
	if (manager->GetCircuit()->GetLastFrame() >= fleeFrame) {
		return;  // the escape is over (re-engaged): a late result must not drag the group home
	}
	pPath = query->GetPathInfo();
	if (pPath->posPath.empty()) {
		// No way out found: fall back to a straight move to the destination (Flee re-queries within 1 s)
		MoveGroupTo(fleeDest);
		return;
	}
	ActivePath();
}

void CAttackTask::Cornered(int frame)
{
	if (frame < corneredFrame + FRAMES_PER_SEC) {
		return;  // keep the current move / attack for a second
	}
	corneredFrame = frame;
	isHolding = false;
	CCircuitAI* circuit = manager->GetCircuit();
	CThreatMap* threatMap = circuit->GetThreatMap();
	const AIFloat3& pos = leader->GetPos(frame);

	AIFloat3 quietPos;
	const float quietThreat = FindQuietSpot(pos, quietPos);
	// Already heading somewhere: switch only to a clearly (20%) quieter spot
	const bool isMoving = utils::is_valid(corneredPos) && (corneredPos.SqDistance2D(pos) > SQUARE(200.f));
	const float refThreat = threatMap->GetThreatAt(leader, isMoving ? corneredPos : pos);
	if ((quietPos.SqDistance2D(pos) > SQUARE(200.f)) && (quietThreat < refThreat * 0.8f)) {
		corneredPos = quietPos;
		SetTarget(nullptr);
		state = State::ROAM;
		CPathFinder* pathfinder = circuit->GetPathfinder();
		std::shared_ptr<IPathQuery> query = pathfinder->CreatePathSingleQuery(
				leader, threatMap, frame, pos, quietPos, pathfinder->GetSquareSize());  // threat as cost, no cap
		SetRouteThreat(query.get(), AttackRoute::CORNERED);
		RunRoute(query, [this](const IPathQuery* query) {
			this->ApplyCorneredPath(static_cast<const CQueryPathSingle*>(query));
		});
		return;
	}
	if (isMoving) {
		return;  // still on the way to a spot that is quiet enough
	}
	corneredPos = -RgtVector;

	// Nowhere quieter: fight from here
	CEnemyInfo* enemy = ((target != nullptr) && (target->GetPos().SqDistance2D(pos) < SQUARE(highestRange)))
			? target : FindNearestHittable(frame);
	if (enemy != nullptr) {
		SetTarget(enemy);
		position = enemy->GetPos();
		state = State::ENGAGE;
		Engage(frame);
	}
}

void CAttackTask::ApplyCorneredPath(const CQueryPathSingle* query)
{
	pPath = query->GetPathInfo();
	if (pPath->posPath.empty()) {
		MoveGroupTo(corneredPos);
		return;
	}
	ActivePath();
}

CEnemyInfo* CAttackTask::FindNearestHittable(int frame) const
{
	CCircuitAI* circuit = manager->GetCircuit();
	CCircuitDef* cdef = leader->GetCircuitDef();
	const AIFloat3& pos = leader->GetPos(frame);
	const int canTargetCat = cdef->GetTargetCategory();
	const bool notAA = !cdef->HasAntiAir();
	const float maxSqDist = SQUARE(std::max(highestRange, cdef->GetMaxRange()) * 1.5f);
	CEnemyInfo* best = nullptr;
	float minSqDist = maxSqDist;
	for (auto& kv : circuit->GetEnemyInfos()) {
		CEnemyInfo* enemy = kv.second;
		if (enemy->IsHidden()) {
			continue;
		}
		CCircuitDef* edef = enemy->GetCircuitDef();
		if ((edef != nullptr)
			&& (((edef->GetCategory() & canTargetCat) == 0) || (edef->IsAbleToFly() && notAA)))
		{
			continue;
		}
		const float sqDist = enemy->GetPos().SqDistance2D(pos);
		if (sqDist < minSqDist) {
			minSqDist = sqDist;
			best = enemy;
		}
	}
	return best;
}

/*
 * Targets and routes
 */

void CAttackTask::FindTarget()
{
	CCircuitAI* circuit = manager->GetCircuit();
	CMilitaryManager* militaryMgr = GetMilitaryMgr();
	CMap* map = circuit->GetMap();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	CThreatMap* threatMap = circuit->GetThreatMap();
	const AIFloat3& basePos = circuit->GetSetupManager()->GetBasePos();
	const int frame = circuit->GetLastFrame();
	const AIFloat3& pos = leader->GetPos(frame);
	STerrainMapArea* area = leader->GetArea();
	CCircuitDef* cdef = leader->GetCircuitDef();
	const bool isAntiStatic = cdef->IsAttrAntiStat();
	const bool notAW = !cdef->HasAntiWater();
	const bool notAA = !cdef->HasAntiAir();
	const int canTargetCat = cdef->GetTargetCategory();
	const int noChaseCat = cdef->GetNoChaseCategory();
	const float weaponRange = cdef->GetMaxRange();
	const float sqMaxSpeed = SQUARE(highestSpeed * 0.8f / FRAMES_PER_SEC);
	const float sqHighestRange = SQUARE(highestRange);
	// Divided by the thr_mod.distance curve at each judged spot
	const float maxPower = GetPickStrength(frame);
	// The current target is judged against a looser allowance (quota.attack_keep_threat x) at its spot and on
	// the in-range approach line, so small swings in threat or in our health don't drop it and pick it again at
	// the next decision. Not on its route (cap, approach check): the escape and the path-ahead check judge our
	// units at the usual allowance, so a route into looser ground would end in a flee. Not inside the base
	// defence radius either, where an engaged group holds ground without escaping.
	// After an escape the target was let go (other groups may take it while we run), but it is still ours at this
	// pick. During the cautious window that is 0.75 x 1.34 ~ the usual allowance, so we don't walk straight back
	// into what we ran from
	const float keepPower = maxPower * militaryMgr->GetAttackKeepThreat();
	const ICoreUnit::Id prevTargetId = (target != nullptr) ? target->GetId() : fleeTargetId;
	fleeTargetId = -1;
	auto powerFor = [&](const CEnemyInfo* e) {
		return ((e->GetId() == prevTargetId) && !IsInBaseRadius(e->GetPos())) ? keepPower : maxPower;
	};

	// Near-base scale reference: the enemy we already went for keeps the anchor from when it was first picked
	// (otherwise the discount would fade as we close in, and the escape would flee the fight we accepted);
	// every other enemy is judged from where we stand now.
	// No discount at all while we stand on enemy-held ground: the enemy outweighs us here, judge at full value
	const float sqOBDist = pos.SqDistance2D(basePos);  // Own to Base distance
	const bool isEnemyGround = IsOnEnemyGround();
	const ICoreUnit::Id prevAnchorId = (scaleRefSqDist > 0.f) ? anchorTargetId : -1;
	const float prevRef = scaleRefSqDist;
	auto refFor = [&](const CEnemyInfo* e) {
		return isEnemyGround ? 0.f : (((prevAnchorId >= 0) && (e->GetId() == prevAnchorId)) ? prevRef : sqOBDist);
	};
	pickSqDist = isEnemyGround ? 0.f : sqOBDist;  // 0: ApplyTargetPath keeps no anchor either

	SetTarget(nullptr);  // make adequate enemy->GetTasks().size()
	threatMap->SetThreatType(leader);
	UpdateCellBans(frame);

	// The current target's planned firing cell (quota.attack_static_cells), while still under its allowance: the side
	// we fight it from. It is judged there when in range, and it is its only route goal while it is kept (below)
	const CEnemyInfo* prevEnemy = (prevTargetId >= 0) ? circuit->GetEnemyInfo(prevTargetId) : nullptr;
	AIFloat3 plannedCell;
	const bool hasPlanned = (prevEnemy != nullptr)
			&& GetPlannedCell(prevTargetId, powerFor(prevEnemy), refFor(prevEnemy), plannedCell);

	// Contested clusters (threat there blocks our mex expansion) that this group can take: enemies there count as
	// quota.contested.bonus times closer. Judged at the full allowance from where we stand (no near-base anchor)
	static std::vector<CMilitaryManager::SContestedSpot> contested;  // NOTE: micro-opt
	militaryMgr->GetTakeableContested(leader, highestRange,
			[this, maxPower](const AIFloat3& p) { return GetAllowedThreatAt(p, maxPower, 0.f); }, contested);
	// Off while any candidate is already within range: don't walk off from a fight for a contested target
	bool isContestedOn = !contested.empty();

	// Targets whose route approach was unsafe are skipped for a while (else stickiness picks them again)
	for (auto it = rejectedUntil.begin(); it != rejectedUntil.end();) {
		it = (it->second.until <= frame) ? rejectedUntil.erase(it) : std::next(it);
	}

	// Can our leader's weapons hit this type, and may we chase it
	auto isTargetableType = [&](const CEnemyInfo* enemy) {
		const CCircuitDef* edef = enemy->GetCircuitDef();
		return (edef == nullptr)
			|| !((isAntiStatic && edef->IsMobile())
				|| ((edef->GetCategory() & canTargetCat) == 0)
				|| ((edef->GetCategory() & noChaseCat) != 0)
				|| (edef->IsAbleToFly() && notAA));
	};
	// ... and at its height (under water, high up); the elevation is an engine call, so this goes last
	auto isTargetableHeight = [&](const CEnemyInfo* enemy) {
		const AIFloat3& ePos = enemy->GetPos();
		CCircuitDef* edef = enemy->GetCircuitDef();
		if (edef == nullptr) {
			return !notAW || (ePos.y >= -SQUARE_SIZE * 5);
		}
		const float elevation = map->GetElevationAt(ePos.x, ePos.z);
		return (!notAW || edef->IsYTargetable(elevation, ePos.y)) && (ePos.y - elevation <= weaponRange);
	};
	auto distModFor = [](const CEnemyInfo* enemy) {
		const CCircuitDef* edef = enemy->GetCircuitDef();
		return (edef != nullptr) ? edef->GetTargetDistMod() : 1.f;
	};

	// quota.attack_static_cells: a target that gets firing cells beyond range (a building or a parked mobile)
	const CMilitaryManager::SAttackStaticCells& cellCfg = militaryMgr->GetAttackStaticCells();
	struct SCandidate {
		float key;  // rawKey x target_dist_mod^2, / quota.contested.bonus^2 in a contested cluster: the ranking
		float rawKey;  // squared distance x near-base scale: about what the route sees
		CEnemyInfo* enemy;
		bool hasAnchor;  // GetCellAnchor: anchor, epoch
		AIFloat3 anchor;
		int epoch;
		// Where range and the approach line are measured from, and a disc goal's spot: a radar-only parked
		// mobile's anchor (its radar position wobbles by the engine's radar error), else its position (GetRangePos)
		AIFloat3 rangePos;
	};
	static std::vector<SCandidate> candidates;  // NOTE: micro-opt
	candidates.clear();
	for (auto& kv : circuit->GetEnemyInfos()) {
		CEnemyInfo* enemy = kv.second;
		// Cheap checks first
		if (enemy->IsHidden() || (enemy->GetTasks().size() > 2)
			|| IsRejected(enemy->GetId(), -1, -1, frame)
			|| !isTargetableType(enemy))
		{
			continue;
		}
		const AIFloat3& ePos = enemy->GetPos();
		const AIFloat3& eVel = enemy->GetVel();
		if ((eVel.SqLength2D() >= sqMaxSpeed) && (eVel.dot2D(pos - ePos) < 0)) {
			continue;  // runs away faster than we chase
		}
		if ((threatMap->GetThreatAt(ePos) >= GetAllowedThreatAt(ePos, powerFor(enemy), refFor(enemy)))
			|| !terrainMgr->CanMobileReachAt(area, ePos, highestRange)
			|| !isTargetableHeight(enemy))
		{
			continue;
		}
		SCandidate c = {0.f, 0.f, enemy, false, ePos, -1, ePos};
		c.hasAnchor = cellCfg.isOn && GetCellAnchor(enemy, frame, c.anchor, c.epoch);
		if (c.hasAnchor && !enemy->IsInLOS()) {
			c.rangePos = c.anchor;
		}
		// Ranked by Own to Enemy distance x near-base scale, times the type's target_dist_mod (behaviour.json)
		// so unwanted types count as farther. From the range position: a radar-only parked mobile's wobble would
		// shift its rank
		const float scale = std::min(c.rangePos.SqDistance2D(basePos) / sqOBDist, 1.f);
		const float sqOEDist = pos.SqDistance2D(c.rangePos);
		c.rawKey = sqOEDist * scale;
		c.key = c.rawKey * SQUARE(distModFor(enemy));
		// quota.attack_static_cells.siege_bonus: a slow mobile type that outranges us on the ground (artillery) ranks
		// closer. By type, parked or not: it doesn't come and go with each hop, which would swap targets. Only while
		// in view or just out of it (IsRecentlySeen), not at a spot it was last seen on minutes ago
		const CCircuitDef* edef = enemy->GetCircuitDef();
		if (cellCfg.isOn && (cellCfg.siegeBonus > 1.f) && (edef != nullptr) && edef->IsMobile()
			&& edef->HasAntiLand() && (fireRangeLand > 0.f)
			&& (edef->GetMaxRange(CCircuitDef::RangeType::LAND) >= SIEGE_RANGE_MOD * fireRangeLand)
			&& (edef->GetSpeed() <= lowestSpeed * PARKED_SPEED_MOD)
			&& enemy->IsRecentlySeen(frame, cellCfg.parkedFrames))
		{
			c.key /= SQUARE(cellCfg.siegeBonus);
		}
		candidates.push_back(c);
		if (sqOEDist < sqHighestRange) {
			isContestedOn = false;
		}
	}
	// Enemies in a contested cluster we can take count quota.contested.bonus times closer
	if (isContestedOn) {
		const float contestedMod = 1.f / SQUARE(militaryMgr->GetContested().bonus);
		for (SCandidate& c : candidates) {
			if (CMilitaryManager::IsInContested(contested, c.enemy->GetPos())) {
				c.key *= contestedMod;
			}
		}
	}

	// Nearest candidates first. Keep the nearest MAX_TARGET_GOALS targets as goals of one route query (Update); the
	// nearest is the target until the route says which one we can actually reach under the threat cap, with a safe approach
	std::sort(candidates.begin(), candidates.end(),
			[](const SCandidate& a, const SCandidate& b) { return a.key < b.key; });
	routeGoals.clear();
	const SCandidate* best = nullptr;
	const SCandidate* prev = nullptr;  // the current target, if still a candidate
	// quota.attack_static_cells: a cell target beyond range is routed to its safe firing cells (one goal per cell)
	static std::vector<SFiringCell> cells;  // NOTE: micro-opt
	int cellChecks = 0;
	bool isPlannedGoal = false;  // the current target's goals are its planned cell (no other side of it)
	size_t targetCount = 0;  // goals are counted by target: a cell target may bring several cells
	for (const SCandidate& c : candidates) {
		const ICoreUnit::Id id = c.enemy->GetId();
		const AIFloat3& rangePos = c.rangePos;  // a radar-only parked mobile's spot (GetRangePos)
		const bool isPrev = (id == prevTargetId);
		const bool isInRange = (rangePos.SqDistance2D(pos) < sqHighestRange);
		// A target within range is engaged at once, without a route: the straight line from it towards us is
		// its approach check. One beyond range gets no line check: its route's approach is checked instead
		// (ApplyTargetPath / IsApproachSafe)
		if (isInRange) {
			const float power = powerFor(c.enemy);
			const float ref = refFor(c.enemy);
			auto allowedAt = [&](const AIFloat3& p) { return GetAllowedThreatAt(p, power, ref); };
			// The current target with a planned firing cell: judged on the side we plan to fight from, as its route
			// was (the line from it to the cell, and our walk there). Else, or that side went hot: the line towards us
			const bool isPlannedSide = isPrev && hasPlanned
					&& IsLineSafe(rangePos, plannedCell, highestRange, allowedAt)
					&& IsLineSafe(pos, plannedCell, std::numeric_limits<float>::max(), allowedAt);
			if (!isPlannedSide && !IsLineSafe(rangePos, pos, highestRange, allowedAt)) {
				continue;
			}
		}
		// The route goes to whichever goal it reaches first, i.e. about the really nearest. So leave out a
		// candidate really nearer than the best but ranked behind it by target_dist_mod or quota.contested: it would
		// take the route (kept if it is our current target, for stickiness)
		if ((best != nullptr) && (c.rawKey < best->rawKey) && !isPrev) {
			continue;
		}
		// Cell target beyond range (quota.attack_static_cells): its firing cells, one goal each. Only in addition to
		// the check at its own spot above, so never more aggressive than without: refused only when every place we
		// could shoot it from is too hot
		cells.clear();
		if (c.hasAnchor && !isInRange && (cellChecks < MAX_CELL_CHECKS)) {
			if (IsCellBanned(id, frame)) {
				continue;
			}
			++cellChecks;
			const bool isPlanned = isPrev && hasPlanned;
			if (!CollectFiringCells(c.enemy, c.anchor, c.epoch, maxPower, refFor(c.enemy),
					isPlanned ? &plannedCell : nullptr, frame, cells))
			{
				continue;
			}
			isPlannedGoal |= isPlanned;
		}
		if (best == nullptr) {
			best = &c;
		}
		if (isPrev) {
			prev = &c;
		}
		if (cells.empty()) {
			routeGoals.push_back({id, rangePos, c.key});
		}
		for (const SFiringCell& cell : cells) {
			routeGoals.push_back({id, c.anchor, c.key, cell.pos, cell.sector, c.epoch});
		}
		if (++targetCount >= MAX_TARGET_GOALS) {
			break;
		}
	}

	// Stickiness: keep the current target unless another is clearly nearer (STICKY_DIST by distance), so
	// near-equal targets don't flip back and forth between decisions
	const float sqSticky = SQUARE(STICKY_DIST);
	const bool isSticky = (prev != nullptr) && (best->key >= prev->key * sqSticky);
	// The current target was kept above although really nearer than the best; if stickiness won't keep it
	// either, leave it out too (all of its goals), or it would take the route
	if ((prev != nullptr) && !isSticky && (prev != best) && (prev->rawKey < best->rawKey)) {
		const ICoreUnit::Id prevId = prev->enemy->GetId();
		routeGoals.erase(std::remove_if(routeGoals.begin(), routeGoals.end(),
				[prevId](const STargetGoal& g) { return g.id == prevId; }), routeGoals.end());
	}

	// The full screened list: if the route's approach to the pick is rejected, the next route goes to these
	screenedGoals = routeGoals;

	// Stickiness: the route then only gets the kept target (all of its goals, or only its planned side if it has
	// one), so it can't swap to a near-equal one either. No other goal could be clearly nearer than it: every
	// screened one ranks at or behind the best, and sticky means the best isn't clearly nearer. If the planned side's
	// approach is rejected, the next route goes to its other sides (screenedGoals)
	if (isSticky) {
		best = prev;
		const ICoreUnit::Id prevId = prev->enemy->GetId();
		const int plannedSector = isPlannedGoal ? lastRoute.sector : -1;
		routeGoals.erase(std::remove_if(routeGoals.begin(), routeGoals.end(), [prevId, plannedSector](const STargetGoal& g) {
			return (g.id != prevId) || ((plannedSector >= 0) && (g.sector != plannedSector));
		}), routeGoals.end());
	}
	// A planned side holds only while its target stays ours
	if ((best == nullptr) || (best->enemy->GetId() != lastRoute.targetId)) {
		lastRoute.sector = -1;
	}

	if (best != nullptr) {
		SetTarget(best->enemy);
		position = GetRangePos(target, frame);
		SetScaleAnchor(best->enemy, refFor(best->enemy));
	} else {
		ClearScaleAnchor();  // no fight accepted: no near-base discount
	}
	// Return: target, startPos=leader->pos, endPos=position
}

bool CAttackTask::IsOnEnemyGround() const
{
	CCircuitAI* circuit = manager->GetCircuit();
	return circuit->GetInflMap()->GetInfluenceAt(leader->GetPos(circuit->GetLastFrame())) < -INFL_EPS;
}

bool CAttackTask::GetCellAnchor(const CEnemyInfo* enemy, int frame, AIFloat3& outPos, int& outEpoch) const
{
	const CCircuitDef* edef = enemy->GetCircuitDef();
	if (edef == nullptr) {
		return false;
	}
	if (!edef->IsMobile()) {
		outPos = enemy->GetPos();  // an identified building has no radar error
		outEpoch = 0;  // it never moves (its type, and so this anchor, is only known once seen in LOS)
		return true;
	}
	const int parkedFrames = GetMilitaryMgr()->GetAttackStaticCells().parkedFrames;
	return (parkedFrames > 0) && (edef->GetSpeed() <= lowestSpeed * PARKED_SPEED_MOD)
		&& !enemy->IsBeingBuilt()  // a nanoframe sits in its factory, which is the cell target there
		&& enemy->GetParkedSpot(frame, parkedFrames, outPos, outEpoch);
}

bool CAttackTask::IsSameAnchor(const CEnemyInfo* enemy, int epoch, int frame) const
{
	AIFloat3 pos;
	int now;
	return (enemy != nullptr) && GetCellAnchor(enemy, frame, pos, now) && (now == epoch);
}

AIFloat3 CAttackTask::GetRangePos(const CEnemyInfo* enemy, int frame) const
{
	AIFloat3 anchor;
	int epoch;
	if (!enemy->IsInLOS() && GetMilitaryMgr()->GetAttackStaticCells().isOn && GetCellAnchor(enemy, frame, anchor, epoch)) {
		return anchor;
	}
	return enemy->GetPos();
}

bool CAttackTask::CollectFiringCells(const CEnemyInfo* enemy, const AIFloat3& anchor, int epoch, float power,
		float ref, const AIFloat3* planned, int frame, std::vector<SFiringCell>& outCells)
{
	// Judged at the pick power, as their route's approach will be (not the keep allowance). A parked mobile: near
	// sectors only
	const bool isMobile = enemy->GetCircuitDef()->IsMobile();
	if (!FillFiringCells(enemy, anchor, power, ref, isMobile, outCells) && (planned == nullptr)) {
		BanCell(enemy, epoch, frame);
		return false;
	}
	// Sides whose walk in was unsafe stay out for a while (RejectGoal); all of them: so does the target. The planned
	// cell replaces its sector's fresh one, so losses or a re-route don't move the spot we walk to
	const ICoreUnit::Id id = enemy->GetId();
	const int plannedSector = (planned != nullptr) ? lastRoute.sector : -1;
	const bool hadCells = !outCells.empty();
	outCells.erase(std::remove_if(outCells.begin(), outCells.end(), [&](const SFiringCell& cell) {
		return (cell.sector == plannedSector) || IsRejected(id, cell.sector, epoch, frame);
	}), outCells.end());
	if (planned != nullptr) {
		outCells.push_back({*planned, plannedSector});
	}
	return !hadCells || !outCells.empty();
}

bool CAttackTask::FillFiringCells(const CEnemyInfo* enemy, const AIFloat3& anchor, float power, float ref,
		bool nearOnly, std::vector<SFiringCell>& outCells) const
{
	// Points on two rings around the anchor, the outer first (the inner only if the outer yields no cell), and on
	// each the sectors facing us first (the far ones only if fewer than 2 near sectors yield a cell, and never if
	// nearOnly). A point is a cell if we can reach it, its threat is below the allowance (the pick's power and
	// reference) and it sees the target. Each sector keeps its best-scoring point; cells scoring over score_slack
	// worse than the best one are left out. Call after threatMap->SetThreatType(leader)
	outCells.clear();
	const CMilitaryManager::SAttackStaticCells& cfg = GetMilitaryMgr()->GetAttackStaticCells();
	CCircuitAI* circuit = manager->GetCircuit();
	CThreatMap* threatMap = circuit->GetThreatMap();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	const AIFloat3& tPos = anchor;
	const AIFloat3& lPos = leader->GetPos(circuit->GetLastFrame());
	const float fireRange = (tPos.y < -SQUARE_SIZE * 5) ? fireRangeWater : fireRangeLand;
	if (fireRange < SQUARE_SIZE * 4) {
		return true;  // melee group: as close as it gets (a disc)
	}
	const AIFloat3 toUs = lPos - tPos;
	const float bearingUs = (toUs.Length2D() > 1.f) ? atan2f(toUs.z, toUs.x) : 0.f;
	auto pointAt = [&tPos](float angle, float radius) {
		AIFloat3 p(tPos.x + radius * cosf(angle), tPos.y, tPos.z + radius * sinf(angle));
		CTerrainManager::CorrectPosition(p);
		return p;
	};
	// Fast path: undefended (no threat at it, nor at the outer ring point facing us): a disc
	if ((threatMap->GetThreatAt(tPos) <= THREAT_MIN)
		&& (threatMap->GetThreatAt(pointAt(bearingUs, fireRange * cfg.rings[0])) <= THREAT_MIN))
	{
		return true;
	}

	// Sectors facing us: the sector's middle within 90 degrees of our bearing
	const int sectors = cfg.sectors;
	const int angles = cfg.angles;
	const float twoPi = 2.f * static_cast<float>(M_PI);
	auto sectorOf = [sectors, angles](int i) { return i * sectors / angles; };
	static std::vector<char> isNear;  // by sector; NOTE: micro-opt
	isNear.resize(sectors);
	for (int sec = 0; sec < sectors; ++sec) {
		isNear[sec] = (utils::angle_diff((sec + 0.5f) * twoPi / sectors, bearingUs) <= static_cast<float>(M_PI) * 0.5f);
	}

	const int sqSize = circuit->GetPathfinder()->GetSquareSize();
	const int maxX = terrainMgr->GetSectorXSize() - 1;
	const int maxZ = terrainMgr->GetSectorZSize() - 1;
	auto toPathXY = [sqSize, maxX, maxZ](const AIFloat3& p) {
		return int2(utils::clamp(int(p.x) / sqSize, 0, maxX), utils::clamp(int(p.z) / sqSize, 0, maxZ));
	};
	const int2 targetXY = toPathXY(tPos);
	// Aimed from cell_aim_height above a cell at half the target's height (a tall strider shows over a low ridge)
	const float targetLift = utils::clamp(enemy->GetCircuitDef()->GetHeight() * 0.5f,
			CELL_TARGET_LIFT_MIN, CELL_TARGET_LIFT_MAX);
	const NSMicroPather::TestFunc hitTest = ISquadTask::MakeHitTest(terrainMgr, cfg.aimHeight, targetLift);
	STerrainMapArea* area = leader->GetArea();
	const float strength = std::max(power, THREAT_MIN);
	constexpr float NO_SCORE = std::numeric_limits<float>::max();
	struct SBest {
		AIFloat3 pos;
		float score;  // NO_SCORE: no cell in this sector
	};
	static std::vector<SBest> best;  // by sector; NOTE: micro-opt

	bool isReachable = false;  // some point is reachable (none: an island, plateau or far shore)
	bool isThreatOk = false;  // some reachable point is below the allowance (no line of sight is not a ban)
	for (const float ringMod : cfg.rings) {
		const float radius = fireRange * ringMod;
		best.assign(sectors, {AIFloat3(), NO_SCORE});
		int nearCells = 0;  // near sectors with a cell
		// The near sectors' points first, the far ones' only if fewer than 2 near sectors yield a cell
		for (const bool isNearPass : {true, false}) {
			if (!isNearPass && (nearOnly || (nearCells >= 2))) {
				break;
			}
			for (int i = 0; i < angles; ++i) {
				const int sec = sectorOf(i);
				if (isNear[sec] != isNearPass) {
					continue;
				}
				const AIFloat3 p = pointAt(i * twoPi / angles, radius);
				if (!terrainMgr->CanMoveToPos(area, p)) {
					continue;
				}
				isReachable = true;
				const float threat = threatMap->GetThreatAt(p);
				if (threat >= GetAllowedThreatAt(p, power, ref)) {
					continue;
				}
				isThreatOk = true;
				if (!hitTest(toPathXY(p), targetXY)) {  // the costly test last
					continue;
				}
				const float score = cfg.threatWeight * std::max(threat, 0.f) / strength + p.distance2D(lPos) / fireRange;
				SBest& b = best[sec];
				if (score < b.score) {
					nearCells += (isNearPass && (b.score == NO_SCORE)) ? 1 : 0;
					b = {p, score};
				}
			}
		}
		float bestScore = NO_SCORE;
		for (const SBest& b : best) {
			bestScore = std::min(bestScore, b.score);
		}
		if (bestScore < NO_SCORE) {
			for (int sec = 0; sec < sectors; ++sec) {
				if (best[sec].score <= bestScore + cfg.scoreSlack) {
					outCells.push_back({best[sec].pos, sec});
				}
			}
			return true;
		}
	}
	// No cell. Nothing reachable (a disc route may still reach ground in range, e.g. a shore) or no line of sight (a
	// pessimistic test): a disc. Too hot everywhere we can reach: ban
	return !isReachable || isThreatOk;
}

bool CAttackTask::GetPlannedCell(ICoreUnit::Id targetId, float strength, float refSqDist, AIFloat3& outCell) const
{
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	if (!lastRoute.IsCell() || (lastRoute.targetId != targetId)
		|| !IsSameAnchor(circuit->GetEnemyInfo(targetId), lastRoute.epoch, frame)  // moved, or out of view
		|| IsRejected(targetId, lastRoute.sector, lastRoute.epoch, frame)
		|| (circuit->GetThreatMap()->GetThreatAt(leader, lastRoute.cell)
			>= GetAllowedThreatAt(lastRoute.cell, strength, refSqDist)))
	{
		return false;
	}
	outCell = lastRoute.cell;
	return true;
}

bool CAttackTask::GetFireArcBearing(float& outBearing) const
{
	if (target == nullptr) {
		return false;
	}
	const float strength = GetKeepStrength(target->GetPos(), GetPickStrength(manager->GetCircuit()->GetLastFrame()));
	AIFloat3 cell;
	if (!GetPlannedCell(target->GetId(), strength, scaleRefSqDist, cell)) {
		return false;
	}
	const AIFloat3& tPos = lastRoute.goal;  // a cell route's goal is its anchor (KeepRoute)
	outBearing = atan2f(cell.z - tPos.z, cell.x - tPos.x);
	return true;
}

bool CAttackTask::IsCellBanned(ICoreUnit::Id id, int frame) const
{
	auto it = cellBans.find(id);
	return (it != cellBans.end()) && (frame < it->second.until);
}

void CAttackTask::BanCell(const CEnemyInfo* enemy, int epoch, int frame)
{
	// At its range position: a radar-only parked mobile's wobble would change the threat read and drop the ban
	const float threat = manager->GetCircuit()->GetThreatMap()->GetThreatAt(GetRangePos(enemy, frame));
	auto it = cellBans.find(enemy->GetId());
	if (it == cellBans.end()) {
		cellBans[enemy->GetId()] = {frame + REJECTED_TARGET_FRAMES, frame, threat, 0, epoch};
		return;
	}
	SCellBan& ban = it->second;  // banned again with nothing changed: longer
	ban.level = std::min(ban.level + 1, CELL_BAN_MAX_LEVEL);
	ban.until = frame + (REJECTED_TARGET_FRAMES << ban.level);
	ban.threat = threat;
}

void CAttackTask::UpdateCellBans(int frame)
{
	CCircuitAI* circuit = manager->GetCircuit();
	CThreatMap* threatMap = circuit->GetThreatMap();
	for (auto it = cellBans.begin(); it != cellBans.end();) {
		const SCellBan& ban = it->second;
		const CEnemyInfo* enemy = circuit->GetEnemyInfo(it->first);
		bool isDrop = (frame >= ban.since + CELL_BAN_RESET_FRAMES) || !IsSameAnchor(enemy, ban.epoch, frame);
		if (!isDrop) {
			const float threat = threatMap->GetThreatAt(GetRangePos(enemy, frame));
			isDrop = fabsf(threat - ban.threat) > CELL_BAN_THREAT_CHANGE * std::max(ban.threat, THREAT_MIN);
		}
		it = isDrop ? cellBans.erase(it) : std::next(it);
	}
}

void CAttackTask::RequestTargetPath(const std::vector<STargetGoal>& goals)
{
	CCircuitAI* circuit = manager->GetCircuit();
	CPathFinder* pathfinder = circuit->GetPathfinder();
	const int frame = circuit->GetLastFrame();
	const AIFloat3& startPos = leader->GetPos(frame);
	const float eps = pathfinder->GetSquareSize();
	const float pathRange = std::max(highestRange - eps, eps);

	// A firing cell's goal: the cell is its node (search heuristic, threat cap); its end nodes are a wedge of its
	// sector around the target, out to our range, so the route stops about where we engage, on the cell's side
	const bool isWedge = std::any_of(goals.begin(), goals.end(), [](const STargetGoal& g) { return g.IsCell(); });
	const float halfAngle = static_cast<float>(M_PI) / GetMilitaryMgr()->GetAttackStaticCells().sectors;
	F3Vec goalPos;
	std::vector<SGoalShape> shapes;  // none if no goal is a wedge
	goalPos.reserve(goals.size());
	for (const STargetGoal& g : goals) {
		goalPos.push_back(g.IsCell() ? g.cell : g.pos);
		if (isWedge) {
			shapes.push_back(g.IsCell() ? SGoalShape::Wedge(g.pos, pathRange, g.cell, halfAngle) : SGoalShape::Disc());
		}
	}
	std::shared_ptr<IPathQuery> query = pathfinder->CreatePathMultiQuery(
			leader, circuit->GetThreatMap(), frame,
			startPos, pathRange, goalPos, GetHitTest(), GetPathThreatCap(startPos, goalPos));
	if (isWedge) {
		static_cast<CQueryPathMulti*>(query.get())->SetGoalShapes(std::move(shapes));
	}
	SetRouteThreat(query.get(), AttackRoute::TARGET);
	// The route reports back later; a later FindTarget may have rewritten the candidates by then
	RunRoute(query, [this, goals](const IPathQuery* query) {
		this->ApplyTargetPath(static_cast<const CQueryPathMulti*>(query), goals);
	});
}

void CAttackTask::ApplyTargetPath(const CQueryPathMulti* query, const std::vector<STargetGoal>& goals)
{
	const std::shared_ptr<PathInfo> path = query->GetPathInfo();  // a copy: routing again may release the query
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();

	if (path->posPath.empty()) {
		// Firing cells' wedges may be cut off where a disc around the target is not: route once more with those
		// targets as discs (as without quota.attack_static_cells), counted against MAX_TARGET_ROUTES
		if (std::any_of(goals.begin(), goals.end(), [](const STargetGoal& g) { return g.IsCell(); })
			&& (++targetRouteTries < MAX_TARGET_ROUTES))
		{
			std::vector<STargetGoal> discs;
			for (const STargetGoal& g : goals) {
				// A target's goals are next to each other (FindTarget); a repeated disc would be harmless anyway
				if ((circuit->GetEnemyInfo(g.id) != nullptr) && (discs.empty() || (discs.back().id != g.id))) {
					discs.push_back({g.id, g.pos, g.key});
				}
			}
			if (!discs.empty()) {
				RequestTargetPath(discs);
				return;
			}
		}
		pPath = path;
		// No candidate reachable within our threat cap: drop the target and head for the front
		SetTarget(nullptr);
		ClearScaleAnchor();
		FallbackFrontPos();
		return;
	}

	// A cell route only if the route's end lies in the cell's wedge: one that ends outside every goal (cut short,
	// target corrected) gets the disc's capped target-to-end check, not an uncapped line to a far cell.
	// A copy: routing again may release the query, and the goals with it
	STargetGoal hit{-1, AIFloat3(), 0.f};
	CEnemyInfo* reached = FindReachedGoal(query, goals, hit) ? circuit->GetEnemyInfo(hit.id) : nullptr;
	const AIFloat3* cell = hit.IsCell() ? &hit.cell : nullptr;
	// A parked mobile that moved (or went out of view) while the route was searched: its wedges no longer stand
	// around it, so the route is judged as a disc
	if ((reached != nullptr) && hit.IsCell() && !IsSameAnchor(reached, hit.epoch, frame)) {
		hit.sector = -1;
		cell = nullptr;
	}
	if (reached != nullptr) {
		// Its approach on this route must be safe (same strength as picking), judged with the near-base reference
		// it would get: its own if it is the anchored target, else the one from where the group stood at pick time.
		// If not, route again to the other candidates, up to MAX_TARGET_ROUTES routes per decision
		const bool isAnchored = (reached->GetId() == anchorTargetId);
		const AIFloat3& basePos = circuit->GetSetupManager()->GetBasePos();
		const AIFloat3 rangePos = GetRangePos(reached, frame);  // a radar-only parked mobile's spot
		const float ref = isAnchored ? scaleRefSqDist
				: ((rangePos.SqDistance2D(basePos) < pickSqDist) ? pickSqDist : 0.f);
		if (!IsApproachSafe(path->posPath, rangePos, GetPickStrength(frame), ref, cell)) {
			RejectGoal(hit, frame);
			// Not our target any more, while the next route runs (nor counted against its group limit)
			SetTarget(nullptr);
			// Next route: every screened candidate of the last decision, not just this route's goals (when the
			// current target was kept, that was only it), minus rejected (targets and sides) and dead
			std::vector<STargetGoal> rest;
			for (const STargetGoal& g : screenedGoals) {
				if (!IsRejected(g.id, g.sector, g.epoch, frame) && (circuit->GetEnemyInfo(g.id) != nullptr)) {
					rest.push_back(g);
				}
			}
			if ((++targetRouteTries < MAX_TARGET_ROUTES) && !rest.empty()) {
				RequestTargetPath(rest);
				return;
			}
			ClearScaleAnchor();
			FallbackFrontPos();
			return;
		}
		if (!isAnchored) {
			SetScaleAnchor(reached, pickSqDist);  // the route led to another candidate than the pick
		}
		SetTarget(reached);
		position = rangePos;
	}
	pPath = path;
	ActivePath(lowestSpeed);
	if (reached != nullptr) {
		// With its cell: the side we'll fight it from; the goal is then its anchor (the wedges' centre). A disc: where
		// range is measured from (Decide reuses it with the same)
		KeepRoute(reached->GetId(), (cell != nullptr) ? hit.pos : position, cell, hit.sector, hit.epoch);
	}
}

bool CAttackTask::FindReachedGoal(const CQueryPathMulti* query, const std::vector<STargetGoal>& goals,
		STargetGoal& outGoal) const
{
	CCircuitAI* circuit = manager->GetCircuit();
	const AIFloat3& endPos = query->GetPathInfo()->posPath.back();
	const float sqSize = circuit->GetPathfinder()->GetSquareSize();
	const float sqInRange = SQUARE(query->GetMaxRange() + sqSize);
	const std::vector<SGoalShape>& shapes = query->GetGoalShapes();  // parallel to goals; empty: all discs
	const STargetGoal* wedgeHit = nullptr;
	const STargetGoal* discHit = nullptr;
	const STargetGoal* nearest = nullptr;
	float wedgeSqDist = std::numeric_limits<float>::max();
	float nearestSqDist = std::numeric_limits<float>::max();
	for (size_t i = 0; i < goals.size(); ++i) {
		const STargetGoal& g = goals[i];
		if (circuit->GetEnemyInfo(g.id) == nullptr) {  // may have died since
			continue;
		}
		const float sqDist = (g.IsCell() ? g.cell : g.pos).SqDistance2D(endPos);  // to the goal's node
		if (g.IsCell()) {
			// Inside its wedge, widened by a square (as the disc's range); right at the target counts too
			const bool isIn = (i < shapes.size())
					&& ((g.pos.SqDistance2D(endPos) < SQUARE(sqSize)) || shapes[i].Contains(endPos, sqSize));
			if (isIn && (sqDist < wedgeSqDist)) {
				wedgeSqDist = sqDist;
				wedgeHit = &g;
			}
		} else if ((discHit == nullptr) && (sqDist <= sqInRange)) {
			discHit = &g;
		}
		if (sqDist < nearestSqDist) {
			nearestSqDist = sqDist;
			nearest = &g;
		}
	}
	const STargetGoal* found = (wedgeHit != nullptr) ? wedgeHit : (discHit != nullptr) ? discHit : nearest;
	if (found == nullptr) {
		return false;
	}
	outGoal = *found;
	if (found != wedgeHit) {
		outGoal.sector = -1;  // a disc route (nearest: maybe a cell goal whose wedge it didn't reach)
	}
	return true;
}

void CAttackTask::RejectGoal(const STargetGoal& g, int frame)
{
	const int until = frame + REJECTED_TARGET_FRAMES;
	if (g.IsCell()) {
		rejectedUntil[{g.id, g.sector}] = {until, g.epoch};
		const bool hasOtherSide = std::any_of(screenedGoals.begin(), screenedGoals.end(), [&](const STargetGoal& o) {
			return (o.id == g.id) && o.IsCell() && !IsRejected(o.id, o.sector, o.epoch, frame);
		});
		if (hasOtherSide) {
			return;
		}
	}
	rejectedUntil[{g.id, -1}] = {until, -1};
}

bool CAttackTask::IsRejected(ICoreUnit::Id id, int sector, int epoch, int frame) const
{
	auto isOn = [&](int sec) {
		auto it = rejectedUntil.find({id, sec});
		// A side rejected under another anchor epoch is of a spot the target has left
		return (it != rejectedUntil.end()) && (frame < it->second.until) && ((sec < 0) || (it->second.epoch == epoch));
	};
	return isOn(-1) || ((sector >= 0) && isOn(sector));
}

bool CAttackTask::IsApproachSafe(const F3Vec& path, const AIFloat3& targetPos, float strength, float refSqDist,
		const AIFloat3* cell) const
{
	// The ground we'd fight from, judged spot by spot with the common allowance (no support)
	auto allowedAt = [&](const AIFloat3& p) { return GetAllowedThreatAt(p, strength, refSqDist); };
	if (cell != nullptr) {
		// 1. a firing cell (quota.attack_static_cells): the cell itself was judged when picked; the line from where the
		//    route ends to it is the walk in (the route's last stretch below lies behind the end, not ahead of it)
		if (!IsLineSafe(path.back(), *cell, std::numeric_limits<float>::max(), allowedAt)) {
			return false;
		}
	} else if (!IsLineSafe(targetPos, path.back(), highestRange, allowedAt)) {
		// 1. the line from the target to where the route ends, out to our range: the firing spots lie on it. Capped:
		//    a route cut short (or a target that moved) ends far away, and the ground in between is the route's job
		return false;
	}
	// 2. the route's last stretch, as long as our range, walked back from its end
	float left = highestRange;
	for (size_t i = path.size() - 1; (i > 0) && (left > 0.f); --i) {
		if (!IsLineSafe(path[i], path[i - 1], left, allowedAt)) {
			return false;
		}
		left -= path[i].distance2D(path[i - 1]);
	}
	return true;
}

void CAttackTask::FallbackFrontPos(bool isReuse)
{
	CCircuitAI* circuit = manager->GetCircuit();
	GetMilitaryMgr()->FillFrontPos(leader, urgentPositions);
	if (urgentPositions.empty()) {
		HoldPosition();
		return;
	}

	const int frame = circuit->GetLastFrame();
	const AIFloat3& startPos = leader->GetPos(frame);
	if (IsOnEnemyGround()) {
		// The front is our own cluster on friendly ground. Already past it (farther from our base than its
		// nearest point): routing there walks the group back, and it would bounce on the border, so hold at
		// the quietest spot nearby. Not past it (an enemy-held patch on the way): keep going to the front.
		const AIFloat3& basePos = circuit->GetSetupManager()->GetBasePos();
		float sqFrontDist = std::numeric_limits<float>::max();
		for (const AIFloat3& p : urgentPositions) {
			sqFrontDist = std::min(sqFrontDist, p.SqDistance2D(basePos));
		}
		if (startPos.SqDistance2D(basePos) > sqFrontDist) {
			HoldPosition();
			return;
		}
	}
	const AIFloat3 goal = urgentPositions.front();  // urgentPositions is shared by all fighter tasks
	if (isReuse && ReuseRoute(FRONT_ROUTE_ID, goal, frame)) {
		return;
	}
	const float pathRange = DEFAULT_SLACK * 4;

	CPathFinder* pathfinder = circuit->GetPathfinder();
	std::shared_ptr<IPathQuery> query = pathfinder->CreatePathMultiQuery(
			leader, circuit->GetThreatMap(), frame,
			startPos, pathRange, urgentPositions, nullptr, GetPathThreatCap(startPos, urgentPositions));
	SetRouteThreat(query.get(), AttackRoute::FRONT);
	RunRoute(query, [this, goal](const IPathQuery* query) {
		this->ApplyFrontPos(static_cast<const CQueryPathMulti*>(query), goal);
	});
}

void CAttackTask::ApplyFrontPos(const CQueryPathMulti* query, const AIFloat3& goal)
{
	pPath = query->GetPathInfo();

	if (pPath->path.empty()) {
		HoldPosition();
	} else if (pPath->path.size() > 2) {
		ActivePath();
		KeepRoute(FRONT_ROUTE_ID, goal);
	}
}

float CAttackTask::GetPathThreatCap(const AIFloat3& startPos, const F3Vec& goals) const
{
	// One cap for the whole route (the pathfinder takes a single value): the common allowance with the
	// strictest thr_mod.distance value of start and goals, but the loosest base_bonus and near-base scale,
	// so a target that passed picking thanks to them stays reachable. Spots on the way that are hotter than
	// their own allowance are caught by the path-ahead check and the escape; threat is also a route cost,
	// so the route avoids them where it can.
	CMilitaryManager* militaryMgr = GetMilitaryMgr();
	const AIFloat3& basePos = manager->GetCircuit()->GetSetupManager()->GetBasePos();
	float curve = GetThrDistModAt(startPos);
	float bonus = militaryMgr->GetBaseThrBonus(basePos.SqDistance2D(startPos));
	float scale = GetNearBaseScaleAt(startPos, scaleRefSqDist);
	for (const AIFloat3& g : goals) {
		curve = std::max(curve, GetThrDistModAt(g));
		bonus = std::max(bonus, militaryMgr->GetBaseThrBonus(basePos.SqDistance2D(g)));
		scale = std::min(scale, GetNearBaseScaleAt(g, scaleRefSqDist));
	}
	return GetSafeThreat() * bonus / curve / std::max(scale, 1e-3f);
}

void CAttackTask::SetRouteThreat(IPathQuery* query, AttackRoute route) const
{
	CMilitaryManager* militaryMgr = GetMilitaryMgr();
	const float strength = GetSafeThreat();
	if (militaryMgr->IsAttackRouteDynamic(route)) {
		// Threat is weighed against the allowance at each cell (what the escape and the path-ahead check use):
		// light threat costs less than the flat 2x, threat at the allowance the same, heavier threat more
		// (quadratic). Big groups walk through what small ones go around.
		// The allowance depends only on distance from our base (base bonus, distance curve, near-base scale): sample
		// it here, on this thread, at half-cell steps out to the map diagonal; the route runs on a worker thread
		CPathFinder* pathfinder = manager->GetCircuit()->GetPathfinder();
		const AIFloat3& basePos = manager->GetCircuit()->GetSetupManager()->GetBasePos();
		const float step = std::max(pathfinder->GetSquareSize() * 0.5f, 1.f);
		const float width = CTerrainManager::GetTerrainWidth();
		const float height = CTerrainManager::GetTerrainHeight();
		const int count = static_cast<int>(sqrtf(width * width + height * height) / step) + 2;
		std::vector<float> allowByDist(count);
		for (int i = 0; i < count; ++i) {
			allowByDist[i] = GetAllowedThreatAt(basePos + AIFloat3((i + 0.5f) * step, 0.f, 0.f), strength);
		}
		// Escapes: capped, so a weak group under heavy fire doesn't trade a short run through the worst of it
		// for a much longer one through moderate threat (more time under fire, and a wider search)
		const bool isEscape = (route == AttackRoute::FLEE) || (route == AttackRoute::CORNERED);
		query->SetThreatAllowance(std::move(allowByDist), step, basePos, pathfinder->GetPathMapXSize(),
				pathfinder->GetSquareSize(), isEscape ? militaryMgr->GetAttackFleePathMaxWeight() : 0.f);
	}

	// Capped routes (target, front, reinforce) ignore light threat: at or below quota.attack_path_ignore_threat x
	// the group's current strength it costs nothing, so the route goes straight through it instead of around.
	// Never above half the lowest per-spot allowance anywhere (strength x the lowest base bonus / the attack
	// thr_mod.distance curve's highest value; near-base scale <= 1), or the path-ahead check and the escape would
	// flag ground the route treats as free
	const bool isCapped = (route == AttackRoute::TARGET) || (route == AttackRoute::FRONT)
			|| (route == AttackRoute::REINFORCE);
	const float frac = militaryMgr->GetAttackPathIgnoreThreat();
	if (isCapped && (frac > 0.f)) {
		const float maxFrac = 0.5f * militaryMgr->GetBaseThrBonusMin() / militaryMgr->GetThrDistModMax(fightType);
		query->SetThreatFloor(strength * std::min(frac, maxFrac));
	}
}

void CAttackTask::RunRoute(const std::shared_ptr<IPathQuery>& query, std::function<void (const IPathQuery*)>&& onComplete)
{
	pathQueries[leader] = query;  // supersedes any pending route of this group
	query->HoldTask(this);
	manager->GetCircuit()->GetPathfinder()->RunQuery(query,
			[this, onComplete = std::move(onComplete)](const IPathQuery* query) {
		if (this->IsQueryAlive(query)) {
			onComplete(query);
		}
	});
}

void CAttackTask::KeepRoute(ICoreUnit::Id targetId, const AIFloat3& goal, const AIFloat3* cell, int sector, int epoch)
{
	lastRoute = {pPath, goal, targetId, manager->GetCircuit()->GetLastFrame(),
			(cell != nullptr) ? CELL_ROUTE_REUSE_FRAMES : ROUTE_REUSE_FRAMES,
			(cell != nullptr) ? *cell : AIFloat3(-RgtVector), (cell != nullptr) ? sector : -1, 0.f,
			(cell != nullptr) ? epoch : -1};
}

bool CAttackTask::ReuseRoute(ICoreUnit::Id targetId, const AIFloat3& goal, int frame)
{
	// Same goal: the same target (or front) that moved less than one path square, or less than the route's goalSlack
	// (routes to groups). The units pulled back from danger would wait for a new route, so not while any unit is in danger
	const float sqMoved = SQUARE(std::max(static_cast<float>(manager->GetCircuit()->GetPathfinder()->GetSquareSize()),
			lastRoute.goalSlack));
	// A cell route instead holds while its target keeps the route's anchor (GetPlannedCell below): a radar-only
	// parked mobile's position wobbles by more than a square, its anchor doesn't
	if ((lastRoute.path == nullptr) || lastRoute.path->posPath.empty() || (lastRoute.targetId != targetId)
		|| (frame >= lastRoute.frame + lastRoute.reuseFrames)
		|| (!lastRoute.IsCell() && (goal.SqDistance2D(lastRoute.goal) >= sqMoved))
		|| isPathAheadUnsafe || !endangered.empty()
		|| !leader->GetTravelAct()->IsActive() || (leader->GetTravelAct()->GetPath() != lastRoute.path))
	{
		return false;
	}
	// A target route's approach is judged again (a few threat lookups, far cheaper than a new route): the group may
	// have lost strength, or threat moved onto the ground it would fight from. Same reference as ApplyTargetPath:
	// FindTarget just anchored this target, or cleared the anchor (0)
	if (targetId >= 0) {
		const float strength = GetPickStrength(frame);
		// A firing cell must still be below the current target's allowance (quota.attack_keep_threat)
		AIFloat3 cell;
		if (lastRoute.IsCell() && !GetPlannedCell(targetId, GetKeepStrength(goal, strength), scaleRefSqDist, cell)) {
			return false;
		}
		if (!IsApproachSafe(lastRoute.path->posPath, goal, strength, scaleRefSqDist, lastRoute.IsCell() ? &cell : nullptr)) {
			return false;  // the new route's ApplyTargetPath rejects it, or finds a safe approach
		}
	}
	pPath = lastRoute.path;  // Start() and regrouping re-send pPath
	return true;
}

/*
 * Moving and holding
 */

float CAttackTask::GetGroupRadius() const
{
	// quota.attack_group_radius: [<x the group's highest weapon/LOS range>, <min elmos>]
	const std::pair<float, float>& r = GetMilitaryMgr()->GetAttackGroupRadius();
	return std::max(highestRange * r.first, r.second);
}

bool CAttackTask::IsOutOfReach(CCircuitUnit* unit, int frame) const
{
	// A straggler: farther from the leader than the group radius (still walking in after a merge, or left behind),
	// unless it already has the target within its weapon range
	const AIFloat3& pos = unit->GetPos(frame);
	if ((target != nullptr) && (pos.SqDistance2D(target->GetPos()) < SQUARE(unit->GetCircuitDef()->GetMaxRange()))) {
		return false;
	}
	return pos.SqDistance2D(leader->GetPos(frame)) > SQUARE(GetGroupRadius());
}

bool CAttackTask::IsFighting() const
{
	// Engaged and some unit has the target within its own weapon range. ENGAGE alone starts at highestRange,
	// which includes LOS radius: far too wide to keep groups from merging.
	if ((State::ENGAGE != state) || (target == nullptr)) {
		return false;
	}
	const int frame = manager->GetCircuit()->GetLastFrame();
	const AIFloat3& tPos = target->GetPos();
	for (CCircuitUnit* unit : units) {
		if (unit->GetPos(frame).SqDistance2D(tPos) < SQUARE(unit->GetCircuitDef()->GetMaxRange())) {
			return true;
		}
	}
	return false;
}

void CAttackTask::Engage(int frame)
{
	Attack(frame);
	// Stragglers (IsOutOfReach) got no attack order. One without an active path (e.g. it merged in while we were
	// engaged: it took the leader's waiting travel state) would idle where it is: walk it to the leader, along the
	// line the merge check judged safe, rather than straight to a firing spot next to the enemy
	const AIFloat3& leaderPos = leader->GetPos(frame);
	for (CCircuitUnit* unit : units) {
		if ((unit->Blocker() == nullptr) && !unit->GetTravelAct()->IsActive() && !IsScooting(unit)
			&& IsOutOfReach(unit, frame))
		{
			MoveUnitTo(unit, leaderPos, frame);
		}
	}
}

void CAttackTask::HoldPosition()
{
	AIFloat3 bestPos;
	FindQuietSpot(leader->GetPos(manager->GetCircuit()->GetLastFrame()), bestPos);
	MoveGroupTo(bestPos);
	isHolding = true;
}

float CAttackTask::FindQuietSpot(const AIFloat3& pos, AIFloat3& outPos)
{
	// Quietest reachable spot within 2x our range: stay put when it is as quiet as anywhere around
	// (no creeping), and keep away from where we were last hit while holding (attackers we can't see
	// or reach leave no threat on the map)
	CCircuitAI* circuit = manager->GetCircuit();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	CThreatMap* threatMap = circuit->GetThreatMap();
	const int frame = circuit->GetLastFrame();
	STerrainMapArea* area = leader->GetArea();
	const bool isAvoidHit = (frame < hitFrame + FRAMES_PER_SEC * 30);
	auto isHitZone = [&](const AIFloat3& p) {
		return isAvoidHit && (p.SqDistance2D(hitPos) < SQUARE(hitRadius));
	};

	AIFloat3 bestPos = pos;
	float bestThreat = isHitZone(pos) ? std::numeric_limits<float>::max() : threatMap->GetThreatAt(leader, pos);
	float bestSqDist = 0.f;
	const float maxDist = std::max(highestRange * 2.f, 400.f);
	for (int ring = 1; ring <= 3; ++ring) {
		const float dist = maxDist * ring / 3.f;
		for (int a = 0; a < 8; ++a) {
			const float angle = a * static_cast<float>(M_PI) / 4.f;
			AIFloat3 p(pos.x + dist * cosf(angle), pos.y, pos.z + dist * sinf(angle));
			CTerrainManager::CorrectPosition(p);
			if (isHitZone(p) || !terrainMgr->CanMoveToPos(area, p)) {
				continue;
			}
			const float threat = threatMap->GetThreatAt(leader, p);
			const float sqDist = p.SqDistance2D(pos);
			// Clearly quieter wins; equally quiet: the nearer one (so a quiet spot keeps us where we are)
			if ((threat < bestThreat - THREAT_MIN * 0.1f)
				|| ((threat < bestThreat + THREAT_MIN * 0.1f) && (sqDist < bestSqDist)))
			{
				bestPos = p;
				bestThreat = threat;
				bestSqDist = sqDist;
			}
		}
	}
	outPos = bestPos;
	return (bestThreat < std::numeric_limits<float>::max()) ? bestThreat : threatMap->GetThreatAt(leader, pos);
}

void CAttackTask::GatherAt(const AIFloat3& pos)
{
	// Wait around pos: only units farther than GATHER_RADIUS walk there, the rest stand where they are, spread
	// out as they came in (not packed onto one spot for area damage, and no orders resent every decision).
	// A fixed radius, not the group radius: that grows with range, and artillery would wait far off.
	// Holding: a unit hit by something we didn't go for steps out of its range (OnUnitDamaged)
	constexpr float GATHER_RADIUS = 600.f;
	const int frame = manager->GetCircuit()->GetLastFrame();
	const float sqRadius = SQUARE(GATHER_RADIUS);
	pPath = std::make_shared<PathInfo>();
	for (CCircuitUnit* unit : units) {
		if (unit->Blocker() != nullptr) {
			continue;
		}
		unit->GetTravelAct()->StateWait();
		if (unit->GetPos(frame).SqDistance2D(pos) > sqRadius) {
			MoveUnitTo(unit, pos, frame);
		}
	}
	isHolding = true;
}

void CAttackTask::MoveGroupTo(const AIFloat3& pos)
{
	const int frame = manager->GetCircuit()->GetLastFrame();
	pPath = std::make_shared<PathInfo>();
	for (CCircuitUnit* unit : units) {
		if (unit->Blocker() != nullptr) {
			continue;
		}
		unit->GetTravelAct()->StateWait();
		MoveUnitTo(unit, pos, frame);
	}
}

void CAttackTask::MoveUnitTo(CCircuitUnit* unit, const AIFloat3& pos, int frame)
{
	TRY_UNIT(manager->GetCircuit(), unit,
		unit->CmdMoveTo(pos, UNIT_COMMAND_OPTION_RIGHT_MOUSE_KEY, frame + FRAMES_PER_SEC * 60);
		unit->CmdWantedSpeed(NO_SPEED_LIMIT);
	)
}

void CAttackTask::OnUnitIdle(CCircuitUnit* unit)
{
	ISquadTask::OnUnitIdle(unit);
	if (units.empty()) {
		return;
	}

	CCircuitAI* circuit = manager->GetCircuit();
	const float maxDist = std::max<float>(lowestRange, circuit->GetPathfinder()->GetSquareSize());
	if (position.SqDistance2D(leader->GetPos(circuit->GetLastFrame())) < SQUARE(maxDist)) {
		CTerrainManager* terrainMgr = circuit->GetTerrainManager();
		float x = rand() % terrainMgr->GetTerrainWidth();
		float z = rand() % terrainMgr->GetTerrainHeight();
		position = AIFloat3(x, circuit->GetMap()->GetElevationAt(x, z), z);
		position = terrainMgr->GetMovePosition(leader->GetArea(), position);
	}

	if (units.find(unit) != units.end()) {
		Start(unit);  // NOTE: Not sure if it has effect
	}
}

void CAttackTask::OnUnitDamaged(CCircuitUnit* unit, CEnemyInfo* attacker)
{
	IFighterTask::OnUnitDamaged(unit, attacker);  // may send the unit to retreat
	if (!isHolding || (leader == nullptr) || (units.find(unit) == units.end())) {
		return;
	}
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	if (frame < stepFrame + FRAMES_PER_SEC) {
		return;
	}
	stepFrame = frame;

	// Hit while holding by something we didn't go for (not visible, or no safe route to it):
	// step out of its range, away from it; unknown attacker: avoid this spot and pick the next quietest
	const AIFloat3& uPos = unit->GetPos(frame);
	if ((attacker != nullptr) && !attacker->IsHidden()) {
		const AIFloat3& aPos = attacker->GetPos();
		CCircuitDef* edef = attacker->GetCircuitDef();
		const float range = (edef != nullptr) ? edef->GetMaxRange() : uPos.distance2D(aPos);
		hitPos = aPos;
		hitRadius = range + 200.f;
		hitFrame = frame;
		AIFloat3 dir = leader->GetPos(frame) - aPos;
		dir.y = 0.f;
		const float len = dir.Length2D();
		if (len > 1.f) {
			AIFloat3 p = aPos + dir / len * hitRadius;
			CTerrainManager::CorrectPosition(p);
			if (circuit->GetTerrainManager()->CanMoveToPos(leader->GetArea(), p)) {
				MoveGroupTo(p);
				return;
			}
		}
	} else {
		hitPos = uPos;
		hitRadius = std::max(highestRange, 300.f);
		hitFrame = frame;
	}
	HoldPosition();
}

} // namespace circuit

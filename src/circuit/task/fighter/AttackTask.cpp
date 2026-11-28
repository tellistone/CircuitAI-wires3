/*
 * AttackTask.cpp
 *
 *  Created on: Jan 28, 2015
 *      Author: rlcevg
 */

#include "task/fighter/AttackTask.h"
#include "task/TaskManager.h"
#include "task/IdleTask.h"
#include "task/RetreatTask.h"
#include "map/InfluenceMap.h"
#include "map/ThreatMap.h"
#include "module/AllyCover.h"
#include "module/BuilderManager.h"
#include "module/FactoryManager.h"
#include "module/Frontline.h"
#include "module/MilitaryManager.h"
#include "module/Protect.h"
#include "setup/SetupManager.h"
#include "terrain/TerrainManager.h"
#include "terrain/path/PathFinder.h"
#include "terrain/path/QueryPathSingle.h"
#include "terrain/path/QueryPathMulti.h"
#include "unit/action/FightAction.h"
#include "unit/action/MoveAction.h"
#include "unit/enemy/EnemyManager.h"
#include "unit/enemy/EnemyUnit.h"
#include "unit/CircuitUnit.h"
#include "CircuitAI.h"
#include "util/Utils.h"

#include "spring/SpringMap.h"

#include "Log.h"

#include <algorithm>
#include <cmath>

#include "AISCommands.h"

namespace circuit {

using namespace springai;

// Routes per target decision: each one whose approach to the target it reaches is unsafe drops that target
// and routes again to the rest
static constexpr int MAX_TARGET_ROUTES = 3;
// A target dropped for an unsafe route approach, or that an empty route couldn't reach, isn't picked again for this long
static constexpr int REJECTED_TARGET_FRAMES = FRAMES_PER_SEC * 6;
// Goals of one target route
static constexpr size_t MAX_TARGET_GOALS = 16;
// For this long after an escape, targets are picked (and their approach checked) at CAUTIOUS_POWER x our strength
static constexpr int CAUTIOUS_FRAMES = FRAMES_PER_SEC * 4;
static constexpr float CAUTIOUS_POWER = 0.75f;
// The cover log (quota.attack_ally_cover): an escape this soon after engaging a target picked only with cover counts,
// and so does one with the multiplier at the leader under this share of what it was when the fight was engaged
static constexpr int COVER_ESCAPE_FRAMES = FRAMES_PER_SEC * 5;
static constexpr float COVER_LOSS_SHARE = 0.75f;
// The current target stays in contact out to this x its reach (IsInContact), and keeps its exemptions from
// quota.raid_attack_factor (base radius, contact) out to this x the radius, so an enemy stepping back and forth across
// either line doesn't flip the pick
static constexpr float TARGET_KEEP = 1.25f;
// ApproachInLine: the line walks in to this x our fire range short of the pick (as the firing spots' ring), and keeps
// its path while the target's end moves less than APPROACH_KEEP
static constexpr float APPROACH_STAND = 0.9f;
static constexpr float APPROACH_KEEP = 128.f;
// Contact by an enemy's own range is capped at highestRange + this x our fire range (the firing-spot ring,
// IFighterTask's arc spots): a unit in contact walks at most about highestRange to its spot, as a group whose leader
// is within highestRange does. Long-range artillery farther out is routed to (siege, firing cells) as before
static constexpr float CONTACT_SPOT_MOD = 0.9f;
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

// quota.attack_balance (CAttackBalance). The base core, this x the base defence radius: fights inside it are never judged
// by the balance (the full radius often reaches our front, where the escape holds every fight: IsHoldingGround)
static constexpr float BALANCE_BASE_CORE = 0.5f;
// Candidates the balance checks per decision (plus the own spot); past it the rest are skipped unchecked: at least
// MAX_TARGET_GOALS + 1 of the nearest failed, the ground ahead is the enemy's
static constexpr int MAX_BALANCE_CHECKS = 32;
// A candidate is skipped while the line from the leader to its F passes this near a holding avoid zone's spot
static constexpr float AVOID_ZONE_RADIUS = 600.f;
// The skip line: at most one per group this often (flag and zone events always log)
static constexpr int BALANCE_LOG_FRAMES = FRAMES_PER_SEC * 10;

// Route ids for ReuseRoute/KeepRoute that aren't enemy ids
static constexpr ICoreUnit::Id FRONT_ROUTE_ID = -1;  // FallbackFrontPos
// quota.attack_merge.idle_merge: an army walking to a group breaks the walk off only for a target ranked under this
// share of the group's distance, so the two distances don't trade places back and forth as both groups move
static constexpr float MERGE_BREAK_OFF_SHARE = 0.5f;
// Keep walking to the group we reinforce while it has at least this share of our power, though it may rank below us
// now: one loss would otherwise turn two similar groups around (both walking to each other still merges them)
static constexpr float REINFORCE_KEEP_SHARE = 0.8f;
static constexpr ICoreUnit::Id REINFORCE_ROUTE_ID = -2;  // to the group we reinforce (or the rally host's fight edge)
static constexpr ICoreUnit::Id PROTECT_ROUTE_ID = -3;  // quota.protect: to the commander's fight (WalkToProtect)
// Routes to a group (ApplyGroupRoute) are kept this long, and while their goal (a moving group)
// drifts less than this share of the route's length: a far group's route barely changes when its goal moves a little
static constexpr int GROUP_ROUTE_REUSE_FRAMES = FRAMES_PER_SEC * 6;
static constexpr float GROUP_ROUTE_SLACK = 0.1f;
// Rally: after an empty route to the host (or its fight), act as a normal attack group this long before routing again
static constexpr int RALLY_ROUTE_RETRY_FRAMES = FRAMES_PER_SEC * 6;
// Rally joiners near the host's fight (FindFightEdge): steps of the edge search back from the host towards us, and the
// distance beyond which a joiner with no safe step still walks in to that far point (a 400 gap past the last step).
// For RALLY_EDGE_MEMORY_FRAMES after a search found nothing near enough, that distance is RALLY_EDGE_HYSTERESIS more,
// so a joiner acting as a normal group that drifts out a little isn't called back in at once
static constexpr float RALLY_EDGE_STEP = 200.f;
static constexpr int RALLY_EDGE_STEPS = 12;
static constexpr float RALLY_EDGE_FAR_DIST = (RALLY_EDGE_STEPS + 2) * RALLY_EDGE_STEP;  // 2800
static constexpr float RALLY_EDGE_HYSTERESIS = 800.f;
static constexpr int RALLY_EDGE_MEMORY_FRAMES = FRAMES_PER_SEC * 20;
// Rally host: counts as fighting this long after its last Engage, so a target blinking at the edge of its range
// doesn't swap its joiners' goal (fight edge or the host) every decision
static constexpr int RALLY_FIGHT_MEMORY_FRAMES = FRAMES_PER_SEC * 6;
// Holding moves to another spot (or switches spots) only for one with at most this share of the threat: two spots
// can't each beat the other, so small changes in threat don't walk and turn the line every few seconds
static constexpr float HOLD_SWITCH_SHARE = 0.8f;
// A hold spot this near is reached straight (no route query); a block standing facing slides there
static constexpr float HOLD_NEAR = DEFAULT_SLACK * 4;
// Escorts (quota.escort). escortModeFrame before the mode's first setting: it is set at once, with no hold
static constexpr int ESCORT_MODE_UNSET = -2;
// A threat to the core: an armed mobile enemy within its weapon range + this many seconds of its travel of a core unit
static constexpr float ESCORT_THREAT_SEC = 2.f;
// Unarmed escorts (shields) stand on a ring this wide round the core's centre (the first on it), and are sent back to
// their point again once this share of their shield radius (at least the ring) from it, or on repeats
static constexpr float ESCORT_SHIELD_RING = 80.f;
static constexpr float ESCORT_SHIELD_SLACK = 0.5f;

namespace {
// Power-weighted median weapon range against one target type, of the units that can hit it (or of all units, if none
// can), support units left out while they are escorts (isNoSupport): the group's fire range for firing cells
// (quota.attack_static_cells), escorts' stay discs and their reach (quota.escort)
float WeightedMedianRange(const std::set<CCircuitUnit*>& units, CCircuitDef::RangeType rt, bool isNoSupport)
{
	static std::vector<std::pair<float, float>> ranges;  // range, power; NOTE: micro-opt
	ranges.clear();
	float total = 0.f;
	for (int pass = 0; (pass < 2) && ranges.empty(); ++pass) {  // 0: shooters of this type; 1 (none): everyone
		for (CCircuitUnit* unit : units) {
			CCircuitDef* cdef = unit->GetCircuitDef();
			const float range = unit->GetFireRange(rt);  // a dynamic commander: its fitted weapons'
			if ((pass == 0) && ((isNoSupport && cdef->IsRoleSupport()) || (range <= 0.f))) {
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
		, isEscortMode(false)
		, escortModeFrame(ESCORT_MODE_UNSET)
		, minPower(minPower)
		, isPastFloor(false)
		, isOwnGroundOnly(false)
		, reinforce(nullptr)
		, reinforceFails(0)
		, reinforceRetryFrame(-1)
		, isDecisionReuse(false)
		, rallyRouteFrame(-1)
		, rallyEdgeFailFrame(-RALLY_EDGE_MEMORY_FRAMES)
		, engageFrame(-RALLY_FIGHT_MEMORY_FRAMES)
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
		, targetSqKey(0.f)
		, contactSpread(0.f)
		, contactFrame(-1)
		, lastRoute({nullptr, -RgtVector, -1, 0, ROUTE_REUSE_FRAMES, AIFloat3(-RgtVector), -1, 0.f, -1})
		, isOutnumbered(false)
		, ownForce(0.f)
		, ownForceFrame(-1)
		, balanceLogFrame(-BALANCE_LOG_FRAMES)
		, blockSince(-1)
		, balanceTargetId(-1)
		, balanceFightId(-1)
		, isHolding(false)
		, holdPos(-RgtVector)
		, stepFrame(0)
		, hitPos(-RgtVector)
		, hitRadius(0.f)
		, hitFrame(-1)
{
	attackRepeat = FRAMES_PER_SEC * 2;  // same ~2 s tempo as the decisions
	isNewcomerOn = true;  // newcomers never make the group stop for them (ISquadTask::IsNewcomer)
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
	// quota.attack_merge.speed_ratio between the leaders. Support units always attach to a group, whatever the speeds:
	// no speed rule when either side has only support units (a unit leading a group joins with that whole group:
	// GetSpeedMix). The group then walks at its slowest unit's pace; FindReinforceSquad prefers hosts they keep up with
	if (IsMoveCompatible(unit, GetMilitaryMgr()->GetAttackMerge().speedRatio)) {
		return true;
	}
	return !(GetSpeedMix().HasCore() && GetSpeedMix(unit).HasCore())
		&& IsMoveCompatible(unit, std::numeric_limits<float>::max());
}

void CAttackTask::AssignTo(CCircuitUnit* unit)
{
	if (unit == comm) {
		// Our commander comes from the builder manager: IUnitTask::AssignTo takes it out of our own idle task only
		manager->GetCircuit()->GetBuilderManager()->GetIdleTask()->RemoveAssignee(unit);
	}
	ISquadTask::AssignTo(unit);
	CCircuitDef* cdef = unit->GetCircuitDef();
	UpdateHighestRange();
	UpdateEscortMode(manager->GetCircuit()->GetLastFrame());

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
	const bool isComm = (unit == comm);
	const CommEnd reason = commEnd;
	if (isComm) {
		comm = nullptr;
		commEnd = CommEnd::NONE;
	}
	ISquadTask::RemoveAssignee(unit);
	if (isComm) {
		// By whatever took it (a flee, a retreat, its death, our end), or "taken": anything else (e.g. a player order)
		HandCommBack(unit, (reason != CommEnd::NONE) ? reason : CommEnd::TAKEN);
	}
	if (leader == nullptr) {
		manager->AbortTask(this);
		return;
	}
	if (isComm) {
		GetMilitaryMgr()->PromoteCommAttack(this);  // the units that merged in go on as a normal attack group
	}
	UpdateHighestRange();
	UpdateEscortMode(manager->GetCircuit()->GetLastFrame());
	lastRoute.path = nullptr;  // the group changed: next decision plans anew (speed, strength)
	// Under quota.attack after a loss the group stays together (it used to break up and walk home
	// to gather); Update() looks for a stronger group to reinforce while it is not engaged.
}

void CAttackTask::Merge(ISquadTask* task)
{
	ISquadTask::Merge(task);
	UpdateHighestRange();
	UpdateEscortMode(manager->GetCircuit()->GetLastFrame());
	lastRoute.path = nullptr;  // the group changed: next decision plans anew (speed, strength)
	// Both groups' cover discs are stale (the merged one's units are in our strength now): collect them again, or a
	// decision later this update would count those units twice
	GetAllyCover()->Invalidate();
	// The newcomers don't count for a regroup for a while (ISquadTask::IsNewcomer), but one may still follow. A leader
	// standing where the last regroup began counts as stuck (IsMustRegroup): start that check afresh
	prevGroupPos = -RgtVector;
}

void CAttackTask::UpdateHighestRange()
{
	highestRange = 0.f;
	for (CCircuitUnit* unit : units) {
		CCircuitDef* cdef = unit->GetCircuitDef();
		highestRange = std::max(highestRange, std::max(unit->GetFireRange(), cdef->GetLosRadius()));
	}

	fireRangeLand = WeightedMedianRange(units, CCircuitDef::RangeType::LAND, isEscortMode);
	fireRangeWater = WeightedMedianRange(units, CCircuitDef::RangeType::WATER, isEscortMode);
}

void CAttackTask::Start(CCircuitUnit* unit)
{
	if ((State::REGROUP == state) || (State::ENGAGE == state)) {
		return;
	}
	if (!pPath->posPath.empty()) {
		unit->GetTravelAct()->SetPath(pPath, GetPathSpeed(unit, manager->GetCircuit()->GetLastFrame(), lowestSpeed));
		unit->GetTravelAct()->StateActivate();
	}
}

void CAttackTask::Update()
{
	++updCount;

	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	// A commander attack: the attack's end first; it never joins another group, absorbs one or escapes as a group
	const bool isComm = (comm != nullptr);
	if (isComm && UpdateComm(frame)) {
		return;
	}

	/*
	 * Merge into another group if possible
	 */
	ISquadTask* task = isComm ? nullptr : GetAttackMergeTask();
	if (task != nullptr) {
		task->Merge(this);
		units.clear();
		// TODO: Deal with cowards?
		manager->AbortTask(this);
		return;
	}
	if (!isComm) {
		AbsorbWeakerTasks();
	}
	if (ReleaseUnarmed()) {  // task aborted
		return;
	}

	UpdateEscortMode(frame);  // its hold runs out between joins and losses too
	UpdateScoots(frame);  // reload_scoot: step out of reach right after a shot (before the escape: Cornered engages too)
	if (!isComm && UpdateEscape(frame)) {
		return;
	}

	/*
	 * Regroup if required
	 */
	const bool wasRegroup = (State::REGROUP == state);
	const bool mustRegroup = IsMustRegroup();
	if (leader == nullptr) {  // all units released as stuck: task aborted
		return;
	}
	if (State::REGROUP == state) {
		if (mustRegroup) {
			Regroup(frame);
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
		// Only looked at, not cleared: this decision may plan no route (one still being worked out, or engaging),
		// so the request still re-plans at the next update; but this decision doesn't reuse a route either
		for (CCircuitUnit* unit : units) {
			isForced |= unit->IsForceUpdatePending(frame);
		}
	} else {
		for (CCircuitUnit* unit : units) {
			isForced |= unit->IsForceUpdate(frame);
		}
	}
	// The regroup just ended: walk the route on (a decision may reuse it), unless a unit asked for a new one
	if (wasRegroup && !isForced && !pPath->posPath.empty()) {
		ActivePath(lowestSpeed);
	}
	// The target died while we fought it (~CEnemyInfo: ClearTarget leaves ENGAGE with no target; every decision that
	// finds nothing sets ROAM): pick the next one now rather than at the next decision, so the units don't stand idle
	const bool isTargetLost = (State::ENGAGE == state) && (target == nullptr);
	// Walking in line to the target (ApproachInLine): the fight starts as soon as it is in contact
	const bool isContact = IsApproaching() && IsInContact(target, false, frame);
	if (!isDecision && !isForced && !isTargetLost && !isContact) {
		return;
	}
	// A normal decision that no unit asked to re-plan may keep a fresh route to the same goal (ReuseRoute)
	Decide(frame, isDecision && !isForced && !isContact);
}

void CAttackTask::Decide(int frame, bool isReuse)
{
	isHolding = false;  // re-set by HoldPosition if this decision finds nothing to go to
	isDecisionReuse = isReuse;  // the target route's callbacks fall back with the same (FallbackFrontPos)
	// quota.attack_balance log: blocked since the last decision
	if (blockSince >= 0) {
		GetBalance()->NoteBlocked(this, leader->GetId(), frame - blockSince);
		blockSince = -1;
	}

	/*
	 * Reinforce: a group under quota.attack walks to the nearest group ranked above it (CMilitaryManager::IsRankedAbove)
	 * instead of hunting alone. A small group that isn't fighting looks for one every decision (~2 s), unless it gave up
	 * recently. An army picks a target first, and walks to such a group when that group is nearer than the target or it
	 * has none (SeekMerge).
	 */
	// quota.attack_merge.rally: every compatible group that can reach the host reinforces it, whatever its size (no
	// retry cooldown); the host carries on as usual and never waits for them
	CMilitaryManager* militaryMgr = GetMilitaryMgr();
	// A commander attack walks to no group (reinforce, rally, idle_merge) and commits to no protect
	const bool isComm = (comm != nullptr);
	// quota.protect: committed to the commander's fight, or gathering for a raid incident's, the group neither walks to
	// merge nor joins a rally
	const bool isGathering = !isComm && IsGathering();
	const bool isProtect = !isComm && (IsProtecting() || isGathering);
	CAttackTask* rallyHost = (isProtect || isComm) ? nullptr : militaryMgr->GetRallyHost();
	const bool isHost = (rallyHost == this);
	const bool isJoiner = !isComm && !isHost && !isProtect && militaryMgr->IsRallyJoiner(this);
	const bool isFreeArmy = !isComm && !isHost && !isJoiner && !isProtect && IsArmy();  // an army outside the rally
	if (isHost || isProtect || isComm) {
		reinforce = nullptr;
	} else if (isJoiner) {
		if (frame < rallyRouteFrame) {
			// Our last route to the host came back empty: act as a normal attack group meanwhile, never wait for a route
			reinforce = nullptr;
		} else if (State::ENGAGE != state) {
			reinforce = rallyHost;
		}
	} else if (isFreeArmy) {
		// An army itself (grown by merges): it picks targets first; reinforce is the group it walks to (SeekMerge),
		// kept through fights on the way until it goes for a target
	} else if ((reinforce == nullptr) && (State::ENGAGE != state) && (frame >= reinforceRetryFrame)) {
		reinforce = militaryMgr->FindReinforceSquad(leader, attackPower, this);
	}
	if (!isFreeArmy && (reinforce != nullptr) && (GetReinforceSquad() != nullptr)) {
		AIFloat3 goal = reinforce->GetLeaderPos(frame);  // validated by GetReinforceSquad
		SStrength routeStrength = GetSafeThreat();  // joint with the host when the route is capped for both groups
		if ((reinforce != rallyHost) || PickRallyGoal(frame, goal, routeStrength)) {
			SetOutnumbered(false, frame, "walk to a host");  // quota.attack_balance: combining, exempt
			// Fight back on the way; otherwise keep walking
			if (EngageInRange(frame, true)) {
				return;
			}
			Reinforce(frame, isReuse, goal, routeStrength);
			return;
		}
		// The rally host's fight has no spot near us where we can stand, alone or beside it: act as a normal attack
		// group for a while (below), as after an empty route to the host, rather than walk into the fight or stand still.
		// No route into it (a doomed search, or one the escape would turn back from)
		rallyRouteFrame = frame + RALLY_ROUTE_RETRY_FRAMES;
		rallyEdgeFailFrame = frame;
		reinforce = nullptr;
	}  // else: nothing to join, carry on as a small attack group

	// quota.attack_balance: outnumbered where we stand? Before the wait for a pending route, so a group whose route is
	// still being worked out doesn't keep walking into an army (the flag going on drops that route)
	UpdateOutnumbered(frame);
	if (isOutnumbered) {
		NoteBlocked(frame);
	}

	// quota.protect, gathering for a raid incident: fight back on the way, else walk to where it waits (no hunt: the
	// group counts towards the go only once it is there)
	if (isGathering) {
		if (!EngageInRange(frame, true)) {
			WalkToProtect(frame, isReuse);
		}
		return;
	}

	// A route is still being worked out: keep the current pick until it comes back, unless the target is
	// already within range or in contact (then engage as usual)
	if (!IsQueryReady(leader) && (target != nullptr)
		&& !IsInLeaderRange(target, GetRangePos(target, frame), true, frame)
		&& !IsInContact(target, true, frame))
	{
		return;
	}
	// Also on enemy-held ground (influence < 0): FindTarget judges threat at the target and along the approach line.
	// With nothing to go for there, FallbackFrontPos holds nearby instead of walking back.
	if (EngageInRange(frame, false)) {
		return;  // a fight here, maybe on the way to merge: the walk goes on after it
	}
	state = State::ROAM;
	if (!IsQueryReady(leader)) {
		return;
	}
	if (target == nullptr) {
		if (isComm) {
			EndComm(isCommKeepFail ? CommEnd::KEEP : CommEnd::NO_TARGET);  // back to its jobs at the next update
		} else if (isProtect) {
			WalkToProtect(frame, isReuse);
		} else {
			FallbackFrontPos(isReuse);
		}
		return;
	}
	// The group we'd merge with is nearer: merge, then hunt together (outnumbered: only one nearer our base)
	if (!isProtect && !isComm && SeekMerge(frame, isReuse, targetSqKey, IsFlagged())) {
		return;
	}
	if (isReuse && ReuseRoute(target->GetId(), position, frame)) {  // position: GetRangePos (FindTarget)
		reinforce = nullptr;  // a hunt ends an army's walk to merge
		return;
	}
	// Route-aware pick: one capped route to all candidates; whichever it reaches first becomes the target,
	// if the route's approach to it is safe
	targetRouteTries = 0;
	RequestTargetPath(routeGoals);
}

bool CAttackTask::EngageInRange(int frame, bool isWalking)
{
	// A target that passes the usual checks (FindTarget) and whose range spot (GetRangePos: position) is already
	// within our range (IsInLeaderRange), or that is in contact: engage it. FindTarget's own verdict, so the two never
	// disagree.
	// Walking to a group the pick is used only in range: with no enemy we could hit within the contact bound, skip
	// FindTarget, it couldn't engage. The bound is over 1.25x highestRange + our fire range, so it also covers a range
	// spot (at most a fire range in front of its target) within highestRange
	if (isWalking && (FindNearestHittable(frame, GetContactBound(frame)) == nullptr)) {
		SetTarget(nullptr);
		fleeTargetId = -1;  // as FindTarget: the target we fled from is ours only at the next pick
		return false;
	}
	if (!FindTarget()) {
		return false;
	}
	if (ApproachInLine(frame)) {
		return true;  // in sight, not yet in contact: the line walks in, the fight starts at contact (Update)
	}
	state = State::ENGAGE;
	NoteCoverEngage(frame);  // the cover log
	NoteBalanceEngage(frame);  // quota.attack_balance: the outcome log
	Engage(frame);
	return true;
}

bool CAttackTask::ApproachInLine(int frame)
{
	// The pick is within range of the leader (highestRange: sight as well as weapons) but not in contact: the line
	// walks straight at it, to APPROACH_STAND x our fire range short of it, rather than every unit walking to its own
	// firing spot from here, bunching on the way. The fight starts at contact (IsInContact at its plain reach, kept
	// at 1.25x once engaged), checked every update. Only from roaming: a fight under way never forms the line again
	// (a kill, a target stepping back). False: engage at once as before (no line, already in contact or about there,
	// or no straight walk there)
	if ((State::ENGAGE == state) || (target == nullptr) || !IsStandAllowed(frame)
		|| IsInContact(target, false, frame))
	{
		return false;
	}
	const float stand = APPROACH_STAND * GetGroundFireRange();
	const AIFloat3& leaderPos = leader->GetPos(frame);
	AIFloat3 dir = leaderPos - position;  // position: the pick's range spot (FindTarget)
	dir.y = 0.f;
	const float dist = dir.Length2D();
	if ((stand < SQUARE_SIZE * 4) || (dist < stand + DEFAULT_SLACK)) {
		return false;
	}
	AIFloat3 end = position + dir * (stand / dist);
	CTerrainManager::CorrectPosition(end);
	CTerrainManager* terrainMgr = manager->GetCircuit()->GetTerrainManager();
	if (!terrainMgr->CanMoveToPos(leader->GetArea(), end)
		|| !terrainMgr->IsWalkClear(leader->GetArea(), leaderPos, end, -1.f))  // < 0: every square on the way
	{
		return false;
	}
	// Already walking in to about there: no new path, so the line doesn't lay out again at every decision
	if (IsApproaching() && (approachPath->posPath.back().SqDistance2D(end) < SQUARE(APPROACH_KEEP))) {
		return true;
	}
	state = State::ROAM;
	approachPath = std::make_shared<PathInfo>();
	approachPath->posPath = {leaderPos, end};
	pPath = approachPath;
	EndLineFacing();  // the path may end within FACE_KEEP of where the line faces from: it walks all the same
	ActivePath(lowestSpeed);
	return true;
}

bool CAttackTask::GetFireFrontCentre(AIFloat3& outPos) const
{
	if (leader == nullptr) {
		return false;
	}
	outPos = leader->GetPos(manager->GetCircuit()->GetLastFrame());
	return true;
}

bool CAttackTask::IsApproaching() const
{
	return (State::ROAM == state) && (target != nullptr) && (approachPath != nullptr) && (pPath == approachPath);
}

/*
 * Group strength and the threat it tolerates
 */

float CAttackTask::GetJointSafeThreat(const CAttackTask* squad) const
{
	// Both groups' safe threat with the group size bonus of the joint count, not each group's own
	const float own = GetSafeThreat() / GetCountPowerMod();
	const float other = squad->GetSafeThreat() / squad->GetCountPowerMod();
	return (own + other) * GetCountPowerMod(units.size() + squad->GetAssignees().size());
}

float CAttackTask::GetPickStrength(int frame) const
{
	// Current-health strength (the measure the escape uses, so we don't pick fights we'd flee from at once).
	// Armed escorts count at the share of the group's fire range they reach from their stay discs (GetEscortReach):
	// the group doesn't take fights its core would end up fighting alone. The escape counts them in full
	float reachShare = 1.f;
	if (isEscortMode) {
		float sum = 0.f;
		float reached = 0.f;
		for (const SUnitStrength& s : GetUnitStrengths()) {
			sum += s.power;
			CCircuitDef* cdef = s.unit->GetCircuitDef();
			reached += (cdef->IsRoleSupport() && IsArmedUnit(cdef)) ? s.power * GetEscortReach(cdef) : s.power;
		}
		reachShare = (sum > 0.f) ? reached / sum : 1.f;
	}
	return GetSafeThreat() * reachShare * ((frame < fleeFrame + CAUTIOUS_FRAMES) ? CAUTIOUS_POWER : 1.f);
}

float CAttackTask::GetKeepStrength(const AIFloat3& targetPos, float strength) const
{
	return IsInBaseRadius(targetPos) ? strength : strength * GetMilitaryMgr()->GetAttackKeepThreat();
}

float CAttackTask::GetRaidFactor() const
{
	if (!GetMilitaryMgr()->IsRaidAttackFactor()) {
		return 1.f;
	}
	float sum = 0.f;
	float modSum = 0.f;
	for (const SUnitStrength& s : GetUnitStrengths()) {
		sum += s.power;
		modSum += s.power * s.unit->GetCircuitDef()->GetRaidThrMod();
	}
	return (sum > 0.f) ? std::min(modSum / sum, 1.f) : 1.f;
}

bool CAttackTask::IsRaidFactorTarget(const CCircuitDef* edef)
{
	// Main role or bound to one (cloaked_raider), or a later entry of its role list (Halberd: assault, raider)
	return (edef != nullptr) && (edef->IsRoleRaider() || edef->IsRoleScout()
			|| edef->IsEnemyRoleAny(CCircuitDef::RoleMask::RAIDER | CCircuitDef::RoleMask::SCOUT));
}

float CAttackTask::GetBaseAllowedThreatAt(const AIFloat3& pos, float strength, float refSqDist) const
{
	if (comm != nullptr) {
		return std::numeric_limits<float>::max();  // a commander attack: the commander's cover test judges (comm)
	}
	CCircuitAI* circuit = manager->GetCircuit();
	const AIFloat3& basePos = circuit->GetSetupManager()->GetBasePos();
	const float bonus = GetMilitaryMgr()->GetBaseThrBonus(basePos.SqDistance2D(pos));
	return strength * bonus / GetThrDistModAt(pos) / std::max(GetNearBaseScaleAt(pos, refSqDist), 1e-3f);
}

float CAttackTask::GetAllowedThreatAt(const AIFloat3& pos, const SStrength& strength, float refSqDist,
		float threat, int* outCoverSource) const
{
	// Cover multiplies whatever strength the caller passes (pick, keep, cautious, raid factor, joint), so the keep
	// and cautious margins scale with it. Linear in strength: the routes' threat / cover mod matches it exactly.
	// At most 2x: under the base allowance a spot passes, at 2x it or more it fails, either way without cover.
	// quota.protect: a committed group judges the commander's fight with the strength present (CProtect::GetBoost),
	// which has the other committed groups there, the commander and the turrets in it already: no cover there
	const float boost = GetMilitaryMgr()->GetProtect()->GetBoost(this, pos);
	const float allowed = GetBaseAllowedThreatAt(pos, strength.value * boost, refSqDist);
	if (boost > 1.f) {
		return allowed;
	}
	const bool isKnown = (threat >= 0.f);
	if (!GetAllyCover()->IsOn() || (isKnown && ((threat < allowed) || (threat >= allowed * 2.f)))) {
		return allowed;
	}
	int source;
	const float mod = GetCoverMod(pos, strength.partner, &source);
	if (mod <= 1.f) {
		return allowed;
	}
	if ((outCoverSource != nullptr) && (*outCoverSource < 0) && isKnown && (threat < allowed * mod)) {
		*outCoverSource = source;  // the cover log: this spot passes only with cover
	}
	return allowed * mod;
}

float CAttackTask::GetAllowedThreatAt(const AIFloat3& pos, float strength) const
{
	return GetAllowedThreatAt(pos, strength, scaleRefSqDist);
}

bool CAttackTask::IsDangerAt(CCircuitUnit* unit, const AIFloat3& p, const SStrength& strength, float refSqDist,
		int* outCoverSource) const
{
	const float threat = GetTaskThreatAt(unit, p);
	return (threat > 0.f) && (threat >= GetAllowedThreatAt(p, strength, refSqDist, threat, outCoverSource));
}

/*
 * Allied cover (quota.attack_ally_cover)
 */

CAllyCover* CAttackTask::GetAllyCover() const
{
	return GetMilitaryMgr()->GetAllyCover();
}

float CAttackTask::GetCoverMod(const AIFloat3& p, const CAttackTask* partner, int* outSource) const
{
	// A flying group: allied ground defences and groups don't guard it
	if (outSource != nullptr) {
		*outSource = static_cast<int>(CAllyCover::Source::DEFENCES);
	}
	if ((leader == nullptr) || leader->GetCircuitDef()->IsAbleToFly()) {
		return 1.f;
	}
	const float base = (partner != nullptr) ? GetJointSafeThreat(partner) : GetSafeThreat();
	CAllyCover::Source source;
	const float mod = GetAllyCover()->GetMod(p, {this, partner, std::max(base, 1e-3f)}, source);
	if (outSource != nullptr) {
		*outSource = static_cast<int>(source);
	}
	return mod;
}

void CAttackTask::NoteCoverPick(ICoreUnit::Id targetId, int source)
{
	coverLog.pickId = (source >= 0) ? targetId : -1;
	coverLog.pickSource = source;
	coverLog.pickEngageFrame = -1;
}

void CAttackTask::NoteCoverEngage(int frame)
{
	CAllyCover* allyCover = GetAllyCover();
	if (!allyCover->IsOn()) {
		return;
	}
	const ICoreUnit::Id id = target->GetId();
	if ((id == coverLog.pickId) && (coverLog.pickEngageFrame < 0)) {
		coverLog.pickEngageFrame = frame;
		allyCover->NoteEngagedPick(coverLog.pickSource);  // a fight cover licensed, counted once it is fought
	}
	if (id != coverLog.engageId) {
		coverLog.engageId = id;
		coverLog.engagePos = leader->GetPos(frame);
		coverLog.engageMod = GetCoverMod(coverLog.engagePos, nullptr);
	}
}

void CAttackTask::NoteCoverEscape(int frame)
{
	CAllyCover* allyCover = GetAllyCover();
	if (!allyCover->IsOn() || (target == nullptr)) {
		return;
	}
	const ICoreUnit::Id id = target->GetId();
	if ((id == coverLog.pickId) && (coverLog.pickEngageFrame >= 0)
		&& (frame - coverLog.pickEngageFrame < COVER_ESCAPE_FRAMES))
	{
		allyCover->NoteQuickEscape(coverLog.pickSource);
	}
	const float lostMod = COVER_LOSS_SHARE * coverLog.engageMod;
	if ((id == coverLog.engageId) && (coverLog.engageMod > 1.f) && (GetCoverMod(leader->GetPos(frame), nullptr) < lostMod)) {
		// Where we engaged from: cover fell there too (it left us), or it still stands and we walked out of it
		allyCover->NoteCoverLossEscape(GetCoverMod(coverLog.engagePos, nullptr) < lostMod);
	}
	coverLog.engageId = -1;  // the next fight is a new one
}

void CAttackTask::NoteCoverLoss(CCircuitUnit* unit, int frame)
{
	// At or over the escape's allowance without cover, under it with: only cover kept the unit there (the escape's own
	// check, IsInDanger). Not while holding ground near base, which has no escape
	CAllyCover* allyCover = GetAllyCover();
	if (!allyCover->IsOn()) {
		return;
	}
	int source = -1;  // set only where the spot passes thanks to cover
	if ((leader != nullptr) && !IsHoldingGround(frame)) {
		const AIFloat3& pos = unit->GetPos(frame);
		GetAllowedThreatAt(pos, GetEscapeSafeThreat(), scaleRefSqDist, GetTaskThreatAt(unit, pos), &source);
	}
	allyCover->NoteLoss(unit->GetCircuitDef()->GetCostM(), source);
}

bool CAttackTask::GetCoverDisc(int frame, AIFloat3& outPos, float& outRange, float& outPower)
{
	if (IsDead() || (leader == nullptr) || units.empty() || IsFleeing(frame)) {
		return false;  // a fleeing group is dropped on its first fleeing frame: it leaves whoever it covered
	}
	UpdateContactUnits(frame);  // contactSpread: its units' spread around the leader (stragglers aside)
	outPos = leader->GetPos(frame);
	outRange = GetGroundFireRange() + contactSpread;
	outPower = GetPickStrength(frame);  // what it would trust itself with: lower just after an escape
	return (outRange > 0.f) && (outPower > 0.f);
}

void CAttackTask::SetRouteCover(IPathQuery* query, const CAttackTask* partner) const
{
	CCircuitAI* circuit = manager->GetCircuit();
	CAllyCover* allyCover = GetAllyCover();
	if (!allyCover->IsOn() || leader->GetCircuitDef()->IsAbleToFly() || IsCloakRoute(leader)) {
		return;
	}
	const float* threat = query->GetThreatArray();
	if ((threat == nullptr) || (circuit->GetPathfinder()->GetPathMapXSize() != circuit->GetThreatMap()->GetThreatMapWidth())) {
		return;  // the route's grid is the threat map's (sectors); never expected to differ
	}
	// Weighed against our own strength, as the route's allowance, floor and path-ahead check are (also on a joint
	// route, whose cap alone is joint): the partner's disc is still left out
	query->SetThreatArray(allyCover->MakeRouteLayer(threat, {this, partner, std::max(GetSafeThreat(), 1e-3f)}));
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

bool CAttackTask::IsInBaseRadius(const AIFloat3& pos, float scale) const
{
	CCircuitAI* circuit = manager->GetCircuit();
	const float baseRange = GetMilitaryMgr()->GetBaseDefRange();
	const AIFloat3& basePos = circuit->GetSetupManager()->GetBasePos();
	return (baseRange > 0.f) && (pos.SqDistance2D(basePos) < SQUARE(baseRange * scale));
}

/*
 * Force balance (quota.attack_balance)
 */

CAttackBalance* CAttackTask::GetBalance() const
{
	return GetMilitaryMgr()->GetAttackBalance();
}

bool CAttackTask::IsBalanceOn() const
{
	// Not for a commander attack: its own filter judges its fights (CBuilderManager::IsCommTargetOk)
	return GetBalance()->IsOn() && (leader != nullptr) && (comm == nullptr) && !leader->GetCircuitDef()->IsAbleToFly()
		&& !IsCloakRoute(leader);
}

bool CAttackTask::IsBalanceLive() const
{
	return !GetBalance()->IsLogOnly() && IsBalanceOn();
}

bool CAttackTask::IsInBaseCore(const AIFloat3& pos) const
{
	return IsInBaseRadius(pos, BALANCE_BASE_CORE);
}

void CAttackTask::GetForceAt(const AIFloat3& F, CAttackBalance::SForce& outForce)
{
	GetBalance()->GetForceAt(F, this, leader->GetCircuitDef()->IsAmphibious(), outForce);
	outForce.own = GetOwnForce(manager->GetCircuit()->GetLastFrame());
}

float CAttackTask::GetOwnForce(int frame)
{
	// The same at every F of a decision: our units are timed to the leader, not to F
	if (ownForceFrame == frame) {
		return ownForce;
	}
	ownForceFrame = frame;
	CAttackBalance* balance = GetBalance();
	const AIFloat3& leaderPos = leader->GetPos(frame);
	const bool isWater = manager->GetCircuit()->GetThreatMap()->IsWaterAt(leaderPos);
	const bool isAmph = leader->GetCircuitDef()->IsAmphibious();
	float sum = 0.f;
	for (const SUnitStrength& s : GetUnitStrengths()) {
		const CCircuitDef* cdef = s.unit->GetCircuitDef();
		const float range = CAttackBalance::GetRangeAt(cdef, isWater, isAmph);
		sum += s.power * balance->GetMobileWeight(s.unit->GetPos(frame).distance2D(leaderPos), range, cdef->GetSpeed());
	}
	ownForce = sum * powerMod;
	return ownForce;
}

AIFloat3 CAttackTask::GetFirePos(const AIFloat3& rangePos, int frame) const
{
	// One continuous rule, so F never jumps as the group closes in
	const AIFloat3& pos = leader->GetPos(frame);
	const float dist = rangePos.distance2D(pos);
	if (dist < 1.f) {
		return rangePos;
	}
	AIFloat3 F = rangePos + (pos - rangePos) * (std::min(GetGroundFireRange(), dist) / dist);
	F.y = rangePos.y;
	return F;
}

void CAttackTask::UpdateOutnumbered(int frame)
{
	if (!IsBalanceOn()) {
		SetOutnumbered(false, frame, "cloaked or flying");  // the leader cloaked (or the balance is off: never set)
		return;
	}
	const AIFloat3& pos = leader->GetPos(frame);
	// Exempt, and each clears the flag: an army walking to merge (SeekMerge; smaller groups walking to a host, rally
	// joiners included, cleared it in Decide's reinforce branch), a group at our base or heading home to a fight there
	if (reinforce != nullptr) {
		SetOutnumbered(false, frame, "walk to a host");
		return;
	}
	if (IsInBaseCore(pos) || ((target != nullptr) && IsInBaseCore(target->GetPos()))) {
		SetOutnumbered(false, frame, "base core");
		return;
	}
	const bool isEngaged = (State::ENGAGE == state);
	if (isEngaged && !isOutnumbered) {
		return;  // the fight we accepted: the escape judges it
	}
	CAttackBalance* balance = GetBalance();
	GetForceAt(pos, spotForce);
	if (isOutnumbered) {
		// Held between the lines: cleared only at the pick line. While engaged this is all, so a flagged group finishes
		// a fight it is in
		const bool isClear = !balance->IsFail(spotForce, CAttackBalance::Line::PICK);
		balance->NoteCheck(CAttackBalance::Line::OWN, !isClear);
		if (isClear) {
			SetOutnumbered(false, frame, "pick line", &spotForce);
		}
		return;
	}
	const bool isFail = balance->IsFail(spotForce, CAttackBalance::Line::OWN);
	balance->NoteCheck(CAttackBalance::Line::OWN, isFail);
	if (isFail) {
		SetOutnumbered(true, frame, "keep line", &spotForce);
		SetAvoidZone(pos, frame, "outnumbered here");
	}
}

void CAttackTask::SetOutnumbered(bool value, int frame, const char* reason, const CAttackBalance::SForce* force)
{
	if (isOutnumbered == value) {
		return;
	}
	isOutnumbered = value;
	GetBalance()->NoteFlag(value);
	const AIFloat3& pos = leader->GetPos(frame);
	const std::string forceText = (force == nullptr) ? std::string()
			: utils::string_format(": E %.1f, own %.1f, allied %.1f, turrets %.1f, E/A %.2f", force->enemy, force->own,
				force->allied, force->cover, force->GetRatio());
	manager->GetCircuit()->LOG("BALANCE%s #%i (%i units) outnumbered %s (%s) at (%.0f, %.0f)%s",
			GetBalance()->IsLogOnly() ? " would be" : "", leader->GetId(), static_cast<int>(units.size()), value ? "on" : "off",
			reason, pos.x, pos.z, forceText.c_str());
	if (value && IsBalanceLive()) {
		// Never walk the route this decision was still waiting for into the army: drop it (its reply is ignored)
		pathQueries.erase(leader);
	}
}

bool CAttackTask::IsBalanceSkip(const CEnemyInfo* enemy, const AIFloat3& F, bool isHeld, bool isFirst, int& ioChecks,
		int frame)
{
	CAttackBalance* balance = GetBalance();
	const ICoreUnit::Id id = enemy->GetId();
	if (IsInAvoidZone(F, frame)) {
		balance->NoteCheck(CAttackBalance::Line::ZONE, true, id);
		if (isFirst && (frame >= balanceLogFrame + BALANCE_LOG_FRAMES)) {
			CAttackBalance::SForce force;
			GetForceAt(F, force);
			LogBalanceSkip("avoid zone", enemy, isHeld, F, force, nullptr, frame);
		}
		return true;
	}
	if (ioChecks >= MAX_BALANCE_CHECKS) {
		return true;
	}
	++ioChecks;
	CAttackBalance::SForce force;
	GetForceAt(F, force);
	const CAttackBalance::Line line = isHeld ? CAttackBalance::Line::KEEP : CAttackBalance::Line::PICK;
	const bool isSkip = balance->IsFail(force, line);
	balance->NoteCheck(line, isSkip, id);
	if (!isSkip) {
		return false;
	}
	if (isHeld) {
		SetAvoidZone(F, frame, "the target we held dropped");
	}
	if (isFirst) {
		LogBalanceSkip(isHeld ? "keep" : "pick", enemy, isHeld, F, force, nullptr, frame);
	}
	return true;
}

void CAttackTask::SetAvoidZone(const AIFloat3& spot, int frame, const char* reason)
{
	// A free slot, else the older zone
	SAvoidZone* zone = &avoidZones[0];
	for (SAvoidZone& z : avoidZones) {
		if (z.setFrame < 0) {
			zone = &z;
			break;
		}
		if (z.setFrame < zone->setFrame) {
			zone = &z;
		}
	}
	const bool isReplace = (zone->setFrame >= 0);
	*zone = {spot, frame, -1};
	GetBalance()->NoteZone(true);
	manager->GetCircuit()->LOG("BALANCE%s #%i avoid zone %s at (%.0f, %.0f): %s", GetBalance()->IsLogOnly() ? " would" : "",
			leader->GetId(), isReplace ? "replaced" : "set", spot.x, spot.z, reason);
}

bool CAttackTask::IsInAvoidZone(const AIFloat3& F, int frame)
{
	const AIFloat3& pos = leader->GetPos(frame);
	const float sqRadius = SQUARE(AVOID_ZONE_RADIUS);
	bool isIn = false;
	for (SAvoidZone& z : avoidZones) {
		if (z.setFrame < 0) {
			continue;
		}
		// From inside the zone every line starts in it: only one heading toward the spot crosses it (one leading away
		// is the way out). From outside: the line passes within the radius
		const bool isCrossing = (pos.SqDistance2D(z.spot) < sqRadius)
				? ((F.x - pos.x) * (z.spot.x - pos.x) + (F.z - pos.z) * (z.spot.z - pos.z) > 0.f)
				: (utils::sq_dist_to_segment_2d(z.spot, pos, F) < sqRadius);
		if (!isCrossing) {
			continue;
		}
		if (z.testFrame != frame) {
			// As the own-spot check would see it with our group standing there: the same two lines as the flag (set at
			// the keep line, lifted at the pick line), so the zone holds while the army stays, and lifts as soon as it
			// leaves or allies arrive
			z.testFrame = frame;
			CAttackBalance::SForce force;
			GetForceAt(z.spot, force);
			if (!GetBalance()->IsFail(force, CAttackBalance::Line::PICK)) {
				z.setFrame = -1;
				GetBalance()->NoteZone(false);
				manager->GetCircuit()->LOG("BALANCE%s #%i avoid zone lifted at (%.0f, %.0f): E %.1f, A %.1f, E/A %.2f",
						GetBalance()->IsLogOnly() ? " would be" : "", leader->GetId(), z.spot.x, z.spot.z, force.enemy,
						force.GetOurs(), force.GetRatio());
				continue;
			}
		}
		isIn = true;
	}
	return isIn;
}

void CAttackTask::LogBalanceSkip(const char* line, const CEnemyInfo* enemy, bool isHeld, const AIFloat3& F,
		const CAttackBalance::SForce& force, const char* fallback, int frame)
{
	if (frame < balanceLogFrame + BALANCE_LOG_FRAMES) {
		return;
	}
	balanceLogFrame = frame;
	const CCircuitDef* edef = (enemy != nullptr) ? enemy->GetCircuitDef() : nullptr;
	const AIFloat3& ePos = (enemy != nullptr) ? enemy->GetPos() : F;
	manager->GetCircuit()->LOG("BALANCE%s skip #%i (%i units) %s line: %s%s at (%.0f, %.0f), F (%.0f, %.0f): E %.1f,"
			" own %.1f, allied %.1f, turrets %.1f, E/A %.2f, blips %.0f%%%s%s",
			GetBalance()->IsLogOnly() ? " would" : "", leader->GetId(), static_cast<int>(units.size()), line,
			(enemy == nullptr) ? "own spot" : (edef != nullptr) ? edef->GetDef()->GetName() : "unknown",
			isHeld ? " (current)" : "",
			ePos.x, ePos.z, F.x, F.z, force.enemy, force.own, force.allied, force.cover, force.GetRatio(),
			100.f * force.GetBlipShare(), (fallback != nullptr) ? ", fall-back: " : "", (fallback != nullptr) ? fallback : "");
}

void CAttackTask::NoteBalanceEngage(int frame)
{
	// A new target engaged (contact and base-core fights too, for the log only); one fight at a time
	if ((target->GetId() == balanceTargetId) || !IsBalanceOn()) {
		return;
	}
	balanceTargetId = target->GetId();
	CAttackBalance* balance = GetBalance();
	if (balance->IsFightOpen(balanceFightId)) {
		return;
	}
	CAttackBalance::SForce force;
	GetForceAt(GetFirePos(GetRangePos(target, frame), frame), force);
	balanceFightId = balance->OpenFight(units, force.GetRatio(), force.GetBlipShare());
}

void CAttackTask::Stop(bool done)
{
	if (comm != nullptr) {
		// Ended with the commander in it: IUnitTask::Stop would hand it to our own idle task
		CCircuitUnit* unit = comm;
		const CommEnd reason = (commEnd != CommEnd::NONE) ? commEnd : CommEnd::STOPPED;
		comm = nullptr;
		commEnd = CommEnd::NONE;
		units.erase(unit);
		pathQueries.erase(unit);
		unit->Clear();
		HandCommBack(unit, reason);
	}
	// quota.attack_balance log: wiped out or merged, the blocked time up to now still counts
	if (blockSince >= 0) {
		const int frame = manager->GetCircuit()->GetLastFrame();
		GetBalance()->NoteBlocked(this, (leader != nullptr) ? leader->GetId() : -1, frame - blockSince);
		blockSince = -1;
	}
	ISquadTask::Stop(done);
}

/*
 * Commander attack
 */
const char* CommEndName(CommEnd reason)
{
	static const char* names[] = {"none", "kill", "no target", "keep", "no route", "leash", "time", "flee", "retreat",
			"disarm", "morph", "hide", "death", "taken", "stopped", "off"};
	static_assert(sizeof(names) / sizeof(names[0]) == static_cast<size_t>(CommEnd::OFF) + 1, "a name per CommEnd");
	return names[static_cast<size_t>(reason)];
}

CAttackTask* CAttackTask::GetCommAttackOf(CCircuitUnit* unit)
{
	IUnitTask* task = unit->GetTask();
	if ((task == nullptr) || (task->GetType() != IUnitTask::Type::FIGHTER)
		|| (static_cast<IFighterTask*>(task)->GetFightType() != IFighterTask::FightType::ATTACK))
	{
		return nullptr;
	}
	CAttackTask* attack = static_cast<CAttackTask*>(task);
	return (attack->GetComm() == unit) ? attack : nullptr;
}

bool CAttackTask::UpdateComm(int frame)
{
	// An end set since the last update (no target, no route, a failed keep), else the commander's limits: leash, time,
	// hide, disarm, morph (CBuilderManager::GetCommAttackEnd)
	if (commEnd == CommEnd::NONE) {
		commEnd = manager->GetCircuit()->GetBuilderManager()->GetCommAttackEnd(comm, this);
		if (commEnd == CommEnd::NONE) {
			return false;
		}
	}
	RemoveAssignee(comm);  // to the builder idle task; aborts the task if the commander was alone
	return true;
}

void CAttackTask::HandCommBack(CCircuitUnit* unit, CommEnd reason)
{
	CBuilderManager* builderMgr = manager->GetCircuit()->GetBuilderManager();
	manager->GetIdleTask()->RemoveAssignee(unit);  // IUnitTask::RemoveAssignee put it in our idle task
	builderMgr->GetIdleTask()->AssignTo(unit);
	builderMgr->SetWorkerJump(unit);  // as its jobs set it: the home fight (CCombatTask) sets none of its own
	builderMgr->OnCommAttackEnd(unit, this, reason);
}

void CAttackTask::OnCommDamaged(CCircuitUnit* unit)
{
	// The builder retreat, as for its jobs (not the military one); not out of a duel it is winning, their D-gun taken
	// as ready: both walk at about the same speed, a turn away is shot in the back
	MarkStrengthStale();  // its health changed: GetUnitStrengths reads again
	CCircuitAI* circuit = manager->GetCircuit();
	CBuilderManager* builderMgr = circuit->GetBuilderManager();
	if (!builderMgr->IsWorkerRetreat(unit)) {
		return;
	}
	const bool isDisarmed = unit->IsDisarmed(circuit->GetLastFrame());
	if (!isDisarmed && builderMgr->IsCommDuelHeld(unit, true)) {
		return;
	}
	EndComm(isDisarmed ? CommEnd::DISARM : CommEnd::RETREAT);
	manager->AssignTask(unit, builderMgr->EnqueueRetreat());  // RemoveAssignee hands it over first
}

bool CAttackTask::IsCommTargetOk(CEnemyInfo* enemy, bool isCurrent)
{
	const bool isOk = manager->GetCircuit()->GetBuilderManager()->IsCommTargetOk(comm, enemy, isCurrent);
	if (!isOk && isCurrent) {
		isCommKeepFail = true;  // the end's reason, if nothing else passes
	}
	return isOk;
}

void CAttackTask::NoteCommReject(ICoreUnit::Id id)
{
	if (comm != nullptr) {
		manager->GetCircuit()->GetBuilderManager()->BanCommTarget(comm, id, false);
	}
}

bool CAttackTask::IsCommRouteOk(CEnemyInfo* enemy, const F3Vec& path)
{
	const bool isKeep = (enemy->GetId() == commRouteId);
	if (!manager->GetCircuit()->GetBuilderManager()->IsCommRouteOk(comm, enemy, path, isKeep)) {
		return false;
	}
	commRouteId = enemy->GetId();
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
	// A group walking to another (reinforce: a small group, an army merging (SeekMerge), a rally joiner) checks every
	// update, so the merge fires as soon as it gets there, before a group on the move walks out of reach again.
	// During a rally (quota.attack_merge.rally) the host and joiners not walking check at the small-group interval.
	// The rally is looked up only on updates where either interval falls
	if (IsFighting()) {
		return nullptr;
	}
	if (reinforce != nullptr) {
		return CheckMergeTask();
	}
	CMilitaryManager* militaryMgr = GetMilitaryMgr();
	const CMilitaryManager::SAttackMerge& cfg = militaryMgr->GetAttackMerge();  // quota.attack_merge.join_interval
	const bool isSmallTick = (updCount % cfg.joinSmall == 1 % cfg.joinSmall);
	const bool isArmyTick = (updCount % cfg.joinArmy == 1 % cfg.joinArmy);
	if (!isSmallTick && !isArmyTick) {
		return nullptr;
	}
	const bool isFast = !IsArmy() || (militaryMgr->GetRallyHost() == this) || militaryMgr->IsRallyJoiner(this);
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
	// Our line stands with its front row at a route's end, the leader up to its depth behind: that much more
	if (from.SqDistance2D(to) >= SQUARE(cfg.travelSec * lowestSpeed + GetLineDepth())) {
		return false;
	}
	const float own = GetSafeThreat();
	const SStrength together = GetJointStrength(squad);  // near the host: its cover is in it, not counted again
	const float sqSupport = SQUARE(cfg.supportRadius);
	return IsLineSafe(from, to, std::numeric_limits<float>::max(), [&](const AIFloat3& p, float threat) {
		return GetAllowedThreatAt(p, (p.SqDistance2D(to) < sqSupport) ? together : SStrength(own), scaleRefSqDist, threat);
	});
}

void CAttackTask::AbsorbWeakerTasks()
{
	// Every quota.attack_merge.absorb_interval (8) updates, a group pulls in groups near it: an army those under
	// quota.attack, any group those walking to it (below).
	// Same rules as joining (CanAssignTo: speed ratio unless a side has only support units, leaders within the merge
	// distance, movement type, never a worse mover than our leader; reachable; IsSafeMergeLine incl. its travel limit),
	// seen from the small group's side.
	// The rally host (quota.attack_merge.rally) absorbs any compatible group, armies too, whatever its own size
	// A rally host checks every 2 updates, so a joiner that arrives merges at once (the host doesn't wait for it)
	// Any group walking to us (reinforce: ours) is absorbed too, whatever either size: a walker that kept us as its
	// group though we now rank below it (REINFORCE_KEEP_SHARE) can't join us from its side (CheckMergeTask)
	const bool isHost = (GetMilitaryMgr()->GetRallyHost() == this);
	const int absorbInterval = isHost ? 2 : GetMilitaryMgr()->GetAttackMerge().absorb;
	if (updCount % absorbInterval != 5 % absorbInterval) {
		return;
	}
	const bool isArmy = IsArmy();

	CCircuitAI* circuit = manager->GetCircuit();
	const AIFloat3& pos = leader->GetPos(circuit->GetLastFrame());
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();

	// quota.protect: a group committed to the commander's fight is absorbed only by another committed one (Merge hands
	// the commit over); a committed group absorbs any
	const bool isProtect = IsProtecting();

	static std::vector<CAttackTask*> weaklings;  // NOTE: micro-opt
	for (IFighterTask* t : GetMilitaryMgr()->GetTasks(fightType)) {
		CAttackTask* candy = static_cast<CAttackTask*>(t);
		CCircuitUnit* candyLeader = candy->GetLeader();
		// Cheap checks first; the small group walks to us, so the line is judged from its side
		if ((candy == this) || candy->IsDead() || (candyLeader == nullptr)
			|| (!isHost && (candy->reinforce != this) && (!isArmy || candy->IsArmy()))
			|| (!isProtect && candy->IsProtecting())
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
		Merge(candy);  // CanAssignTo ruled out worse movers; a core unit takes the lead from support units (FindLeader)
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
	// Still one we may join and can reach (CanReinforce): our leader may have changed, or the group moved to another area
	if ((tasks.find(static_cast<IFighterTask*>(reinforce)) != tasks.end()) && !reinforce->IsDead()
		&& (reinforce->GetLeader() != nullptr)
		&& ((reinforce->GetAttackPower() >= REINFORCE_KEEP_SHARE * attackPower)
			|| (reinforce == militaryMgr->GetRallyHost()))
		&& militaryMgr->CanReinforce(reinforce, leader))
	{
		return reinforce;  // a rally host is kept whatever its size
	}
	// Group gone or now clearly weaker: pick the nearest one ranked above us
	reinforce = militaryMgr->FindReinforceSquad(leader, attackPower, this);
	return reinforce;
}

void CAttackTask::Reinforce(const int frame, bool isReuse, const AIFloat3& goal, const SStrength& routeStrength)
{
	SetTarget(nullptr);
	ClearScaleAnchor();  // walking to a group, not into an accepted fight
	state = State::ROAM;

	if (!IsQueryReady(leader)) {
		return;
	}
	// Threat-aware path to the group's leader (rally host: PickRallyGoal), capped like every other route
	// (GetPathThreatCap: the common allowance, for routeStrength). It only has to get within reinforce_range (800), so
	// the hot zone around a fighting group doesn't block it; merging takes over inside the merge distance
	// (CheckMergeTask from our side, AbsorbWeakerTasks from an army's side). Range: quota.attack_merge.reinforce_range
	const AIFloat3& startPos = leader->GetPos(frame);
	position = goal;
	if (isReuse && ReuseRoute(REINFORCE_ROUTE_ID, goal, frame)) {
		return;
	}
	// Never stop farther out than we can merge from (the merge distance, or travel_sec at our slowest speed), with
	// a margin for the host moving: otherwise a slow group would park short of the host and never merge
	const CMilitaryManager::SAttackMerge& cfg = GetMilitaryMgr()->GetAttackMerge();
	const float mergeReach = std::min(cfg.distance, cfg.travelSec * lowestSpeed);
	const float pathRange = std::min(cfg.reinforceRange, mergeReach * 0.8f);
	const float slack = GROUP_ROUTE_SLACK * startPos.distance2D(goal);
	RunGroupRoute(frame, goal, pathRange, routeStrength, [this, goal, slack](const CQueryPathSingle* query) {
		this->ApplyReinforcePath(query, goal, slack);
	});
}

bool CAttackTask::PickRallyGoal(int frame, AIFloat3& inOutGoal, SStrength& inOutStrength) const
{
	// Reinforcing the rally host (quota.attack_merge.rally), one goal rule:
	// - the host fights: the safe edge of its fight (FindFightEdge), to add our strength without walking into it alone,
	//   else beside the host if the two groups together can stand there;
	// - otherwise: the host itself (inOutGoal as given; inside the merge distance, so the merge always fires).
	// inOutStrength: what the route's cap allows for (in: ours; both groups' for the spot beside a fighting host, whose
	// cover is then left out of the route).
	// false: nowhere to stand near the fight and we are close already, the caller acts as a normal group for a while
	// (FindTarget still picks targets at the fight's edge that pass its own approach checks)
	if (reinforce->IsRallyFighting(frame)) {
		const AIFloat3 hostPos = inOutGoal;
		const bool isRecentFail = (frame < rallyEdgeFailFrame + RALLY_EDGE_MEMORY_FRAMES);
		const float farDist = RALLY_EDGE_FAR_DIST + (isRecentFail ? RALLY_EDGE_HYSTERESIS : 0.f);
		return FindFightEdge(frame, hostPos, farDist, inOutGoal, inOutStrength);
	}
	return true;
}

void CAttackTask::ApplyReinforcePath(const CQueryPathSingle* query, const AIFloat3& goal, float slack)
{
	const bool isRally = (reinforce != nullptr) && (reinforce == GetMilitaryMgr()->GetRallyHost());
	if (ApplyGroupRoute(query, REINFORCE_ROUTE_ID, goal, slack)) {
		reinforceFails = 0;
		if (!isRally && (pPath->path.size() <= 2)) {
			// Already beside the group: a short route isn't walked (ApplyGroupRoute), so stop here rather than carry
			// on with older orders (a front route) out of merge distance; in line where we can
			if (!StandInLine(manager->GetCircuit()->GetLastFrame())) {
				GatherAt(goal);
			}
		}
		return;
	}
	const int frame = manager->GetCircuit()->GetLastFrame();
	if (isRally) {
		// Rally: no route to the host (or its fight). Never wait for one: act as a normal attack group (hunt, front)
		// from the next decision, and route to the host again after a while (Decide)
		rallyRouteFrame = frame + RALLY_ROUTE_RETRY_FRAMES;
		return;
	}
	// An army can fight on its own: it gives up at its first empty route (a failed route searches all the ground it
	// may cross, the costliest kind), and hunts or goes to the front from the next decision. A small group gives up after
	// quota.attack_merge.reinforce_fails empty routes in a row (10: ~20 s at a decision every ~2 s) and acts as a small
	// attack group. Either way it looks again after reinforce_retry (10 s); merge checks and absorbing still apply meanwhile
	const CMilitaryManager::SAttackMerge& cfg = GetMilitaryMgr()->GetAttackMerge();
	if (IsArmy() || (++reinforceFails >= cfg.reinforceFails)) {
		reinforce = nullptr;
		reinforceFails = 0;
		reinforceRetryFrame = frame + cfg.reinforceRetry;
	}
}

/*
 * Commander protect (quota.protect, CProtect)
 */

void CAttackTask::WalkToProtect(int frame, bool isReuse)
{
	// Committed with nothing to fight in reach: walk to the commander's fight (a raid incident's gathering post in its
	// no-go). Its safe edge as rally joiners find the host's (FindFightEdge: in the protect's present radius the spots
	// are judged with the strength present and ours); none: straight to the commander, the go was judged for all of
	// us together. The route cap carries the same boost at the goal
	SetTarget(nullptr);
	ClearScaleAnchor();  // walking to the fight, not into an accepted one
	state = State::ROAM;
	if (!IsQueryReady(leader)) {
		return;
	}
	const CProtect* protect = GetMilitaryMgr()->GetProtect();
	const AIFloat3 gather = protect->GetGather(this);
	const AIFloat3 hostPos = utils::is_valid(gather) ? gather : protect->GetGoal(this);
	AIFloat3 goal = hostPos;
	SStrength edgeStrength = GetSafeThreat();
	if (!FindFightEdge(frame, hostPos, RALLY_EDGE_FAR_DIST, goal, edgeStrength)) {
		goal = hostPos;
	}
	const SStrength routeStrength(GetSafeThreat() * protect->GetBoost(this, goal));
	position = goal;
	if (isReuse && ReuseRoute(PROTECT_ROUTE_ID, goal, frame)) {
		return;
	}
	const AIFloat3& startPos = leader->GetPos(frame);
	const float slack = GROUP_ROUTE_SLACK * startPos.distance2D(goal);
	RunGroupRoute(frame, goal, DEFAULT_SLACK * 2, routeStrength, [this, goal, slack](const CQueryPathSingle* query) {
		if (!this->ApplyGroupRoute(query, PROTECT_ROUTE_ID, goal, slack)) {
			this->GatherAt(goal);  // no capped route: straight there, the commander can't wait
		}
	});
}

/*
 * Merging armies (quota.attack_merge.idle_merge): an army walks to another group and merges, unless it has a nearer target
 */

bool CAttackTask::SeekMerge(int frame, bool isReuse, float sqTargetKey, bool isBackOnly)
{
	// Armies only: a smaller group reinforces already (Decide), the rally host and its joiners have their own rules.
	// An army's reinforce is the group it walks to: kept through fights on the way, ended by a hunt (a target route
	// accepted), dropped here when idle merging is off or after an empty route to a group (the retry wait)
	CMilitaryManager* militaryMgr = GetMilitaryMgr();
	if (!IsArmy() || (militaryMgr->GetRallyHost() == this) || militaryMgr->IsRallyJoiner(this)) {
		return false;
	}
	if (!militaryMgr->IsAttackIdleMerge() || (frame < reinforceRetryFrame)) {
		reinforce = nullptr;
		return false;
	}
	// The group we walk to, kept while it has REINFORCE_KEEP_SHARE of our power, else the nearest one ranked above us.
	// None: we are the top of our class, we hunt or go to the front as before (the others come to us)
	const bool wasWalking = (reinforce != nullptr);
	CAttackTask* host = GetReinforceSquad();
	if (host == nullptr) {
		return false;
	}
	// Outnumbered (quota.attack_balance): never toward the enemy. GetReinforceSquad has stored the host already: clear
	// it, or we would count as walking to a host, which is exempt and clears the flag (the retry wait is untouched)
	if (isBackOnly) {
		const AIFloat3& basePos = manager->GetCircuit()->GetSetupManager()->GetBasePos();
		if (host->GetLeaderPos(frame).SqDistance2D(basePos) >= leader->GetPos(frame).SqDistance2D(basePos)) {
			reinforce = nullptr;
			return false;
		}
	}
	// The group competes with the target on the target ranking's scale (FindTarget: a raid near our base, a commander
	// or a contested cluster counts as nearer, a scout as farther)
	const float sqHostDist = leader->GetPos(frame).SqDistance2D(host->GetLeaderPos(frame));
	const float share = wasWalking ? MERGE_BREAK_OFF_SHARE : 1.f;
	if (sqTargetKey < SQUARE(share) * sqHostDist) {
		if (!wasWalking) {
			reinforce = nullptr;  // hunting; a walk broken off ends when the target's route is accepted
		}
		return false;
	}
	Reinforce(frame, isReuse, host->GetLeaderPos(frame), GetSafeThreat());
	return true;
}

float CAttackTask::GetLineDepth() const
{
	const CFormation* line = GetFormation();  // nullptr: the line is off
	return (line != nullptr) ? line->GetDepth() : 0.f;
}

float CAttackTask::GetLeaderLineOffset() const
{
	const CFormation* line = GetFormation();  // nullptr: the line is off
	return (line != nullptr) ? line->GetLeaderOffset() : 0.f;
}

void CAttackTask::RunGroupRoute(int frame, const AIFloat3& goal, float range, const SStrength& strength,
		std::function<void (const CQueryPathSingle*)>&& onComplete)
{
	// A capped route (reinforce costs, the cap for strength) from our leader to within range of goal. In line, the
	// front row stands at the route's end and the leader up to the line's depth behind it, so the route ends that much
	// closer: the leader gets within range (merge reach) rather than parking short of it
	CCircuitAI* circuit = manager->GetCircuit();
	CPathFinder* pathfinder = circuit->GetPathfinder();
	const AIFloat3& startPos = leader->GetPos(frame);
	const float goalRange = std::max(range - GetLineDepth(), static_cast<float>(pathfinder->GetSquareSize()));
	std::shared_ptr<IPathQuery> query = pathfinder->CreatePathSingleQuery(
			leader, circuit->GetThreatMap(), frame,
			startPos, goal, goalRange, nullptr,
			GetPathThreatCap(startPos, {goal}, strength.value));
	SetRouteThreat(query.get(), AttackRoute::REINFORCE, strength.partner);
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

bool CAttackTask::FindFightEdge(int frame, const AIFloat3& hostPos, float farDist, AIFloat3& outPos,
		SStrength& outStrength) const
{
	// The rally host is fighting: its position stepped back towards us until the threat there is under our own
	// allowance (as our route judges it), so we join the fight from its safe side.
	// Nothing safe on the steps: from farther away than farDist, walk to the point that far out (the edge is looked
	// for again on the way); closer: beside the host, if the two groups together can stand there (the flee's rule,
	// FindFleeDest: on the way in our escape picks the same spot), with a route capped for both; else false
	const AIFloat3& ownPos = leader->GetPos(frame);
	const float dist = ownPos.distance2D(hostPos);
	if (dist < 1.f) {
		return true;  // outPos: the host (as given)
	}
	CCircuitAI* circuit = manager->GetCircuit();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	const AIFloat3 dir = (ownPos - hostPos) / dist;
	const float own = GetSafeThreat();
	for (int k = 0; (k <= RALLY_EDGE_STEPS) && (k * RALLY_EDGE_STEP < dist); ++k) {
		AIFloat3 p = hostPos + dir * (k * RALLY_EDGE_STEP);
		CTerrainManager::CorrectPosition(p);
		if (!terrainMgr->CanMoveToPos(leader->GetArea(), p)) {
			continue;
		}
		if (!IsDangerAt(leader, p, own)) {
			outPos = p;
			return true;
		}
	}
	if (dist > farDist) {
		outPos = hostPos + dir * RALLY_EDGE_FAR_DIST;
		CTerrainManager::CorrectPosition(outPos);
		return true;
	}
	if (reinforce == nullptr) {
		return false;  // no host group to stand beside (quota.protect walks to the commander)
	}
	const SStrength joint = GetJointStrength(reinforce);
	if (!IsDangerAt(leader, hostPos, joint, scaleRefSqDist)) {
		outStrength = joint;
		return true;  // outPos: the host (as given)
	}
	return false;
}

/*
 * Rally host (quota.attack_merge.rally): it never waits for its joiners, they come to it
 */

bool CAttackTask::IsRallyFighting(int frame) const
{
	return (State::ENGAGE == state) || (frame - engageFrame < RALLY_FIGHT_MEMORY_FRAMES);
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
	// The escape would flee from a fight the pick still accepts (it judges the target's spot, the escape each unit's).
	// For a turret just seen, the flee grace (quota.attack_flee_grace) re-spots first. If the group flees anyway from a
	// target standing still, it passes on it (6, 12, 24 s), or the next pick sends it straight back (a moving one may
	// follow us, and is then the one to fight). Not while running already, holding ground or cornered
	const bool isMismatch = isDanger && !isHoldGround && (frame >= fleeFrame) && IsFightOn(frame);
	if (IsInFleeGrace(frame, isMismatch, GetMilitaryMgr()->GetAttackFleeGrace(), GetMilitaryMgr()->GetAttackFleeShare())) {
		if (frame == graceFrame) {
			attackFrame = frame - attackRepeat;  // orders again now, not at the 3 s repeat: spots avoid the new danger
		}
		Engage(frame);
		PullBackEndangered(frame, true);  // those without a fresh spot (stragglers, no spot range)
		return true;
	}
	CEnemyInfo* fledTarget = (isMismatch && !IsMoving(target)) ? target : nullptr;  // Flee lets it go
	const bool isFleeing = isDanger && !isHoldGround && Flee(frame);
	if (isFleeing && (fledTarget != nullptr)) {
		Ban(heldBans, fledTarget, -1, frame);
	}
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
		// keep running along the escape path for a moment instead of turning straight back
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

bool CAttackTask::IsFormationFlee(int frame) const
{
	// Fleeing, or cornered and moving to a quieter spot: the line runs too, so light units don't bunch for splash
	return (frame < fleeFrame) || utils::is_valid(corneredPos);
}

bool CAttackTask::IsHoldingGround(int frame) const
{
	// Only inside the base defence radius (where thr_mod.base_bonus applies): out in the field the enemy
	// is nearer our base than we are in about half of all fights, and those should still be escapable
	return (State::ENGAGE == state) && (target != nullptr) && IsInBaseRadius(target->GetPos());
}

bool CAttackTask::IsFightOn(int frame) const
{
	if ((State::ENGAGE != state) || (target == nullptr)) {
		return false;
	}
	// As FindTarget judges the current target (no raid factor: an engaged target is mostly in contact, exempt)
	const AIFloat3 rangePos = GetRangePos(target, frame);
	return !IsDangerAt(leader, rangePos, GetKeepStrength(rangePos, GetPickStrength(frame)), scaleRefSqDist);
}

bool CAttackTask::Flee(int frame)
{
	const AIFloat3 dest = FindFleeDest(frame);
	if (leader->GetPos(frame).SqDistance2D(dest) < SQUARE(FLEE_ARRIVE_DIST)) {
		isAtFleeDest = true;
		fleeFrame = std::min(fleeFrame, frame);  // stop running, but keep the cautious window after it
		return false;  // already there: running gets us nowhere
	}

	const bool wasFleeing = (frame < fleeFrame + FRAMES_PER_SEC);  // grace: still in the same escape
	if (!wasFleeing) {
		NoteCoverEscape(frame);  // the cover log
	}
	if (balanceFightId >= 0) {
		GetBalance()->NoteFled(balanceFightId);  // quota.attack_balance: the outcome log
	}
	if (target != nullptr) {
		fleeTargetId = target->GetId();  // kept for the next pick (FindTarget)
	}
	SetTarget(nullptr);
	state = State::ROAM;
	isHolding = false;
	ClearScaleAnchor();  // running: judge danger at full value, also on the way home
	GetMilitaryMgr()->GetFrontline()->ReleaseHold(this, FRAMES_PER_SEC * 30);  // quota.hold_choke: driven off it
	fleeFrame = frame + FRAMES_PER_SEC * 2;
	if (!wasFleeing) {
		GetAllyCover()->Invalidate();  // a running group covers no one: from this frame on, not the next
	}
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

	// The nearest stronger attack group (fight beside it, then merge), if the two groups together can stand there:
	// it may be in this same fight, or past the enemy. Not the rally host by preference: it may be far away
	CAttackTask* squad = militaryMgr->FindReinforceSquad(leader, attackPower, this, false);
	if (squad != nullptr) {
		const AIFloat3& squadPos = squad->GetLeaderPos(frame);
		if (!IsDangerAt(leader, squadPos, GetJointStrength(squad), scaleRefSqDist)) {
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
			&& !IsDangerAt(leader, p, strength, 0.f))
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
	const float refThreat = GetTaskThreatAt(leader, isMoving ? corneredPos : pos);
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
			? target : FindNearestHittable(frame, std::max(highestRange, leader->GetFireRange()) * 1.5f);
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

CEnemyInfo* CAttackTask::FindNearestHittable(int frame, float radius) const
{
	CCircuitAI* circuit = manager->GetCircuit();
	CCircuitDef* cdef = leader->GetCircuitDef();
	const AIFloat3& pos = leader->GetPos(frame);
	const int canTargetCat = cdef->GetTargetCategory();
	const bool notAA = !cdef->HasAntiAir();
	const float maxSqDist = SQUARE(radius);
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
 * Contact
 */

void CAttackTask::UpdateContactUnits(int frame)
{
	if (contactFrame == frame) {
		return;
	}
	contactFrame = frame;
	contactUnits.clear();
	float sqSpread = 0.f;
	const AIFloat3& leaderPos = leader->GetPos(frame);
	const float sqGroupRadius = SQUARE(GetGroupRadius());
	for (CCircuitUnit* unit : units) {
		const AIFloat3& pos = unit->GetPos(frame);
		const float sqDist = pos.SqDistance2D(leaderPos);
		if (sqDist > sqGroupRadius) {
			continue;  // a straggler (IsOutOfReach): it walks to the leader, it doesn't pull the group into a fight
		}
		contactUnits.push_back({pos, unit->GetCircuitDef()->IsHighTrajectory()});
		sqSpread = std::max(sqSpread, sqDist);
	}
	contactSpread = sqrtf(sqSpread);
}

float CAttackTask::GetContactReach(const CEnemyInfo* enemy) const
{
	// Our fire range: the group's power-weighted median, as elsewhere (one long-range unit doesn't widen it).
	// The enemy's own range: it can hit us from there (an outranging skirmisher or artillery shelling us)
	float reach = GetGroundFireRange();
	CCircuitDef* edef = enemy->GetCircuitDef();
	float eRange;
	if ((edef != nullptr) && IsGroundDangerDef(edef, false, eRange)) {  // mobile, armed, not flying or anti-air only
		reach = std::max(reach, std::min(eRange, GetContactCap()));
	}
	return reach + DEFAULT_SLACK;
}

float CAttackTask::GetContactCap() const
{
	return highestRange + CONTACT_SPOT_MOD * GetGroundFireRange();
}

bool CAttackTask::IsInContact(const CEnemyInfo* enemy, bool isCurrent, int frame, float* outSqDist)
{
	const float reach = GetContactReach(enemy) * (isCurrent ? TARGET_KEEP : 1.f);
	return IsSeenByUnits(enemy, enemy->GetPos(), reach, isCurrent, frame, outSqDist);
}

float CAttackTask::GetContactBound(int frame)
{
	UpdateContactUnits(frame);
	return contactSpread + (GetContactCap() + DEFAULT_SLACK) * TARGET_KEEP;  // the cap is above our fire range
}

bool CAttackTask::IsSeenByUnits(const CEnemyInfo* enemy, const AIFloat3& pos, float range, bool isCurrent, int frame,
		float* outSqDist)
{
	UpdateContactUnits(frame);
	float minSqDist = std::numeric_limits<float>::max();
	bool isSeen = false;
	if (pos.SqDistance2D(leader->GetPos(frame)) <= SQUARE(contactSpread + range)) {  // else beyond every unit we count
		float aimLift = 0.f, targetLift = 0.f;
		const bool isSight = GetSightLifts(enemy, aimLift, targetLift) && !(isCurrent && (State::ENGAGE == state));
		CTerrainManager* terrainMgr = manager->GetCircuit()->GetTerrainManager();
		const float sqRange = SQUARE(range);
		for (const SContactUnit& u : contactUnits) {
			const float sqDist = pos.SqDistance2D(u.pos);
			minSqDist = std::min(minSqDist, sqDist);
			isSeen = isSeen || ((sqDist < sqRange) && (!isSight || u.isHighTrajectory
				|| terrainMgr->IsLineClear(u.pos, pos, aimLift, targetLift, false)));
		}
	}
	if (outSqDist != nullptr) {
		*outSqDist = minSqDist;
	}
	return isSeen;
}

bool CAttackTask::IsInLeaderRange(const CEnemyInfo* enemy, const AIFloat3& rangePos, bool isCurrent, int frame)
{
	return (rangePos.SqDistance2D(leader->GetPos(frame)) < SQUARE(highestRange))
		&& IsSeenByUnits(enemy, rangePos, highestRange, isCurrent, frame);
}

/*
 * Targets and routes
 */

bool CAttackTask::FindTarget()
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
	const float weaponRange = leader->GetFireRange();
	const float sqMaxSpeed = SQUARE(highestSpeed * CHASE_SPEED_MOD / FRAMES_PER_SEC);
	// A runner that outranges us is dropped at a lower speed: we would only catch it inside its range.
	// Our range: the group's weighted median weapon range (as IsSiegeTarget), not highestRange (that includes LOS)
	const float sqOutrangeSpeed = SQUARE(highestSpeed * OUTRANGE_CHASE_SPEED_MOD / FRAMES_PER_SEC);
	const float groundRange = GetGroundFireRange();
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
	// quota.attack_balance: its keep line is only for the target we still hold, never the one we fled from
	const ICoreUnit::Id heldTargetId = (target != nullptr) ? target->GetId() : -1;
	const ICoreUnit::Id prevTargetId = (target != nullptr) ? target->GetId() : fleeTargetId;
	fleeTargetId = -1;
	// quota.raid_attack_factor: an enemy raider or scout is judged with our strength x our units' raid_thr_mod
	// (skirmishers: about 4x the strength to go after raiders in the field). At full strength inside the base
	// defence radius (raiding our base) and in contact (IsInContact: already fighting us; skipped, the
	// group would walk off on Move orders, which Zero-K's skirmish micro ignores, and the escape judges at full
	// strength, so it would neither fight nor flee). The current target keeps both out to TARGET_KEEP x
	const float raidFactor = GetRaidFactor();
	// While engaged, every enemy in contact is part of the fight we accepted: judged at the keep allowance as the
	// current target is, so a kill doesn't drop the rest of the fight (the next pick has no current target). A fight
	// is still only started at the usual allowance (roaming)
	const bool isEngaged = (State::ENGAGE == state);
	auto powerFor = [&](const CEnemyInfo* e, bool isContact) {
		const AIFloat3& ePos = e->GetPos();
		const bool isPrev = (e->GetId() == prevTargetId);
		const bool isBase = IsInBaseRadius(ePos);
		const float power = ((isPrev || (isContact && isEngaged)) && !isBase) ? keepPower : maxPower;
		if ((raidFactor >= 1.f) || !IsRaidFactorTarget(e->GetCircuitDef())) {
			return power;
		}
		const bool isFree = isContact || (isPrev ? IsInBaseRadius(ePos, TARGET_KEEP) : isBase);
		return isFree ? power : power * raidFactor;
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
	threatMap->SetThreatType(leader);  // shared threat-map state: set on every pick
	UpdateCellBans(frame);

	// The current target's planned firing cell (quota.attack_static_cells), while still under its allowance: the side
	// we fight it from. It is judged there when in range, and it is its only route goal while it is kept (below)
	const CEnemyInfo* prevEnemy = (prevTargetId >= 0) ? circuit->GetEnemyInfo(prevTargetId) : nullptr;
	AIFloat3 plannedCell;
	const bool hasPlanned = (prevEnemy != nullptr)
			&& GetPlannedCell(prevTargetId, powerFor(prevEnemy, IsInContact(prevEnemy, true, frame)), refFor(prevEnemy),
				plannedCell);

	// Contested clusters (threat there blocks our mex expansion) that this group can take: enemies there count as
	// quota.contested.bonus times closer. Judged at the full allowance from where we stand (no near-base anchor)
	static std::vector<CMilitaryManager::SContestedSpot> contested;  // NOTE: micro-opt
	militaryMgr->GetTakeableContested(leader, highestRange,
			[this, maxPower](const AIFloat3& p, float threat) { return GetAllowedThreatAt(p, maxPower, 0.f, threat); },
			contested);
	// Off while any candidate is already within range or in contact: don't walk off from a fight for a contested target
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
	const CMilitaryManager::SAttackCells& cellCfg = militaryMgr->GetAttackCells();
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
		bool isContact;  // IsInContact: while engaged, ranked ahead of every enemy not in contact
		bool isInRange;  // IsInLeaderRange, or in contact: engaged at once, no route
		int coverSource;  // the cover log: passed its own spot or approach line only with cover, by source (-1: not)
		bool isZone;  // quota.protect: a zone enemy while committed (CProtect::IsZoneEnemy)
	};
	static std::vector<SCandidate> candidates;  // NOTE: micro-opt
	// quota.attack_own_ground_time: early on, a target we'd walk to must be on our ground (in the loop).
	// The same under the quota.attack floor (UpdatePastFloor) and while the enemy army outweighs ours
	// (quota.attack_threat_floor), at any time
	CInfluenceMap* inflMap = circuit->GetInflMap();
	UpdatePastFloor();
	isOwnGroundOnly = (comm == nullptr)  // a commander attack: its own filter (leash, radius)
			&& ((frame < militaryMgr->GetAttackOwnGroundFrame()) || !isPastFloor || militaryMgr->IsAttackThreatFloor());
	// quota.protect: committed to the commander's fight, only the enemies in its zone (and those already fighting us).
	// They pass the own-ground rule, other groups' claims, the 3-group cap and the force balance; those that hit the
	// commander lately rank 3x nearer
	const CProtect* protect = militaryMgr->GetProtect();
	const bool isProtect = IsProtecting();
	isCommKeepFail = false;
	candidates.clear();
	for (auto& kv : circuit->GetEnemyInfos()) {
		CEnemyInfo* enemy = kv.second;
		const bool isZone = isProtect && protect->IsZoneEnemy(this, enemy);
		// Cheap checks first
		if (enemy->IsHidden() || ((enemy->GetTasks().size() > 2) && !isZone) || !isTargetableType(enemy)) {
			continue;
		}
		const bool isPrev = (enemy->GetId() == prevTargetId);
		// A commander attack: only what the commander's filter passes (kind, radius and leash, budget, cover, duel)
		if ((comm != nullptr) && !IsCommTargetOk(enemy, isPrev)) {
			continue;
		}
		float contactSqDist;
		const bool isContact = IsInContact(enemy, isPrev, frame, &contactSqDist);
		if (isProtect && !isZone && !isContact) {
			continue;
		}
		// In contact: already fighting us. It needs no route, so a rejected route doesn't matter, and another group's
		// claim doesn't take it: a claim stops us heading for an enemy, not shooting one we reach (the current target
		// stays in contact out to TARGET_KEEP x, so an enemy at the edge isn't picked and dropped by turns). Else not
		// one another raid or attack group already targets from clearly nearer (IsClaimedByNearer); a raid's claim
		// doesn't take our current target: a passing raid would make us re-route away and back.
		// In the fight (in contact while engaged), a runner still within reach is shot at as it goes, and a kiter's step
		// back doesn't flip the pick; a roaming group doesn't chase a raider or scout that pokes it and runs.
		// IsRunningAway: runs away faster than we chase (or nearly as fast, and outranges us)
		if ((!isContact && (IsRejected(enemy->GetId(), -1, -1, frame)
				|| (!isZone && IsClaimedByNearer(enemy, pos, 0.f, /*isRaidClaim*/ !isPrev, /*isAttackClaim*/ true, frame))))
			|| (!(isContact && isEngaged) && IsRunningAway(enemy, pos, sqMaxSpeed, sqOutrangeSpeed, groundRange)))
		{
			continue;
		}
		const AIFloat3& ePos = enemy->GetPos();
		int spotSource = -1;  // the cover log
		if (IsDangerAt(leader, ePos, powerFor(enemy, isContact), refFor(enemy), &spotSource)
			|| !terrainMgr->CanMobileReachAt(area, ePos, highestRange)
			|| !isTargetableHeight(enemy))
		{
			continue;
		}
		SCandidate c = {0.f, 0.f, enemy, false, ePos, -1, ePos, isContact, false, spotSource, isZone};
		c.hasAnchor = GetCellAnchor(enemy, frame, c.anchor, c.epoch);
		if (c.hasAnchor && !enemy->IsInLOS()) {
			c.rangePos = c.anchor;
		}
		// Ranked by Own to Enemy distance x near-base scale, times the type's target_dist_mod (behaviour.json)
		// so unwanted types count as farther. From the range position: a radar-only parked mobile's wobble would
		// shift its rank. In contact: from our nearest unit (while engaged, contact enemies rank among themselves, ahead of
		// the rest)
		const float scale = std::min(c.rangePos.SqDistance2D(basePos) / sqOBDist, 1.f);
		const float sqOEDist = pos.SqDistance2D(c.rangePos);
		// quota.attack_own_ground_time and the floors: our territory (IsAllyTerritory: our buildings and land
		// builders outweigh the enemy's buildings; mobiles on neither side count, else a group would license its own
		// surroundings and creep forward, and the target would veto itself).
		// A target in contact (within our fire range of one of our units, or its range reaches one; not LOS: a group
		// would hop forward one LOS radius at a time) is always fair game, so fights at the edge aren't dropped. The
		// current target stays ours until the enemy outweighs us there by half a building, so it doesn't flicker on
		// the border as our constructors move or enemy buildings come into view
		if (isOwnGroundOnly && !isContact && !isZone
			&& !inflMap->IsAllyTerritory(c.rangePos, isPrev ? -1.f : INFL_EPS))
		{
			continue;
		}
		c.isInRange = isContact || IsInLeaderRange(enemy, c.rangePos, isPrev, frame);
		c.rawKey = (isContact ? contactSqDist : sqOEDist) * scale;
		c.key = c.rawKey * SQUARE(distModFor(enemy));
		if (IsSiegeTarget(enemy, frame)) {  // quota.attack_static_cells.siege_bonus
			c.key /= SQUARE(cellCfg.siegeBonus);
		}
		if (isZone && protect->IsRecentAttacker(enemy->GetId(), frame)) {
			c.key /= 9.f;  // quota.protect: the commander's attackers first
		}
		candidates.push_back(c);
		if (c.isInRange) {
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
	// nearest is the target until the route says which one we can actually reach under the threat cap, with a safe approach.
	// While engaged, enemies in contact come first: finish the fight we're in before walking off to another target (a
	// preferred type, a contested cluster), which would also put the line back on mid-fight. Roaming, target_dist_mod
	// ranks them as any other (a scout poking the front row doesn't pull the group off its walk)
	std::sort(candidates.begin(), candidates.end(), [isEngaged](const SCandidate& a, const SCandidate& b) {
		return (isEngaged && (a.isContact != b.isContact)) ? a.isContact : (a.key < b.key);
	});
	routeGoals.clear();
	const SCandidate* best = nullptr;
	const SCandidate* prev = nullptr;  // the current target, if still a candidate
	// quota.attack_static_cells: a cell target beyond range is routed to its safe firing cells (one goal per cell)
	static std::vector<SFiringCell> cells;  // NOTE: micro-opt
	int cellChecks = 0;
	// quota.attack_balance: every candidate out of contact and the base core is judged by the force balance at its F.
	// Outnumbered where we stand (and not log-only): only targets in contact or inside the base core. In-range targets
	// don't count: the in-range test uses highestRange, which includes sight
	const bool isBalanceOn = IsBalanceOn();
	const bool isBalanceLive = isBalanceOn && !GetBalance()->IsLogOnly();
	const bool isFlagged = isBalanceLive && isOutnumbered;
	int balanceChecks = 0;
	// The balance skipped a candidate (log-only: would have), and whether any candidate passed it or wasn't judged:
	// skips and no pass, the group is blocked (log-only: would be)
	bool isBalanceSkip = false;
	bool isBalancePass = false;
	bool isPlannedGoal = false;  // the current target's goals are its planned cell (no other side of it)
	size_t targetCount = 0;  // goals are counted by target: a cell target may bring several cells
	for (SCandidate& c : candidates) {
		const ICoreUnit::Id id = c.enemy->GetId();
		if (IsBanned(heldBans, id, frame)) {
			continue;  // the whole group held off it last time (no safe firing spot): something else for a while
		}
		const AIFloat3& rangePos = c.rangePos;  // a radar-only parked mobile's spot (GetRangePos)
		const bool isPrev = (id == prevTargetId);
		const bool isInRange = c.isInRange;
		// quota.attack_balance: judged unless in contact or inside the base core. Its free skips come first, before
		// the approach line and the firing cells: flagged, only unjudged targets; past the check cap, the rest
		const bool isJudged = isBalanceOn && !c.isContact && !IsInBaseCore(rangePos)
				&& !c.isZone;  // quota.protect: judged centrally
		if (isJudged && isBalanceLive && (isFlagged || (balanceChecks >= MAX_BALANCE_CHECKS))) {
			isBalanceSkip = true;
			continue;
		}
		// A target within range (or in contact) is engaged at once, without a route: the straight line from it
		// towards the leader (its first highestRange: the ground our units will stand on to fire) is its approach
		// check. One beyond range gets no line check: its route's approach is checked instead
		// (ApplyTargetPath / IsApproachSafe)
		if (isInRange) {
			const float power = powerFor(c.enemy, c.isContact);
			const float ref = refFor(c.enemy);
			int lineSource = -1;  // the cover log
			auto allowedAt = [&](const AIFloat3& p, float threat) {
				return GetAllowedThreatAt(p, power, ref, threat, &lineSource);
			};
			// The current target with a planned firing cell: judged on the side we plan to fight from, as its route
			// was (the line from it to the cell, and our walk there). Else, or that side went hot: the line towards us
			const bool isPlannedSide = isPrev && hasPlanned
					&& IsLineSafe(rangePos, plannedCell, highestRange, allowedAt)
					&& IsLineSafe(pos, plannedCell, std::numeric_limits<float>::max(), allowedAt);
			if (!isPlannedSide) {
				lineSource = -1;  // the side that decides
				if (!IsLineSafe(rangePos, pos, highestRange, allowedAt)) {
					continue;
				}
			}
			if (c.coverSource < 0) {
				c.coverSource = lineSource;  // the cover log: its approach line passed only with cover
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
			if (IsBanned(cellBans, id, frame)) {
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
		if (isJudged) {
			// F: where we'd fire from, its first firing cell for a cell target. The keep line only for the target we hold
			const AIFloat3 firePos = cells.empty() ? GetFirePos(rangePos, frame) : cells.front().pos;
			if (IsBalanceSkip(c.enemy, firePos, id == heldTargetId, best == nullptr, balanceChecks, frame)) {
				isBalanceSkip = true;
				if (isBalanceLive) {
					continue;
				}
			} else {
				isBalancePass = true;
			}
		} else {
			isBalancePass = true;
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
	// near-equal targets don't flip back and forth between decisions. While engaged, a current target out of contact
	// doesn't win over a best in contact (the fight ranks first: a best out of contact means none passed). Roaming, as
	// any other target
	const float sqSticky = SQUARE(STICKY_DIST);
	const bool isSticky = (prev != nullptr) && (!isEngaged || prev->isContact || !best->isContact)
			&& (best->key >= prev->key * sqSticky);
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

	if (isBalanceSkip && !isBalancePass) {
		NoteBlocked(frame);  // quota.attack_balance: blocked time (log-only: what would have been)
	}
	if (best == nullptr) {
		ClearScaleAnchor();  // no fight accepted: no near-base discount
		return false;
	}
	if (best->enemy->GetId() != prevTargetId) {
		NoteCoverPick(best->enemy->GetId(), best->coverSource);  // the cover log
	}
	SetTarget(best->enemy);
	position = best->rangePos;  // GetRangePos
	targetSqKey = best->key;
	SetScaleAnchor(best->enemy, refFor(best->enemy));
	return best->isInRange;  // target, startPos=leader->pos, endPos=position
}

void CAttackTask::UpdatePastFloor()
{
	// Hysteresis: a group that loses a unit or two just past the floor keeps going; a beaten one falls back to
	// targets on our ground. Power is the units' def power (not health), so it changes only as units join or die
	if (attackPower >= minPower) {
		isPastFloor = true;
	} else if (attackPower < minPower * GetMilitaryMgr()->GetAttackFloorHold()) {
		isPastFloor = false;
	}
}

bool CAttackTask::IsOnEnemyGround() const
{
	CCircuitAI* circuit = manager->GetCircuit();
	return circuit->GetInflMap()->GetInfluenceAt(leader->GetPos(circuit->GetLastFrame())) < -INFL_EPS;
}

void CAttackTask::RequestTargetPath(const std::vector<STargetGoal>& goals)
{
	CCircuitAI* circuit = manager->GetCircuit();
	CPathFinder* pathfinder = circuit->GetPathfinder();
	const int frame = circuit->GetLastFrame();
	const AIFloat3& startPos = leader->GetPos(frame);
	const float eps = pathfinder->GetSquareSize();
	// In line, the front row stands at the route's end and the leader its row's offset behind it: the route ends that
	// much closer, so the leader gets within range and engages (EngageInRange). A leader in the front row, or no
	// line: as before
	const float pathRange = std::max(highestRange - eps - GetLeaderLineOffset(), eps);

	// A firing cell's goal: the cell is its node (search heuristic, threat cap); its end nodes are a wedge of its
	// sector around the target, out to our range, so the route stops about where we engage, on the cell's side
	const bool isWedge = std::any_of(goals.begin(), goals.end(), [](const STargetGoal& g) { return g.IsCell(); });
	const float halfAngle = static_cast<float>(M_PI) / GetMilitaryMgr()->GetAttackCells().sectors;
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
		// No candidate reachable within our threat cap: drop the target and head for the front. Its targets are
		// rejected as for an unsafe approach: else the next decision routes to them again, the costliest search there
		// is (all that is reachable under the cap), e.g. to a target on a plateau with no way up
		for (const STargetGoal& g : goals) {
			rejectedUntil[{g.id, -1}] = {frame + REJECTED_TARGET_FRAMES, -1};
			NoteCommReject(g.id);
		}
		SetTarget(nullptr);
		ClearScaleAnchor();
		FallbackFrontPos(isDecisionReuse);
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
		int approachSource = -1;  // the cover log
		// A commander attack: its time budget and its cover along this route (no threat cap: comm)
		const bool isSafe = (comm != nullptr) ? IsCommRouteOk(reached, path->posPath)
				: IsApproachSafe(path->posPath, rangePos, GetPickStrength(frame), ref, cell, &approachSource);
		if (!isSafe) {
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
			FallbackFrontPos(isDecisionReuse);
			return;
		}
		if ((approachSource >= 0) && (reached->GetId() != coverLog.pickId)) {
			NoteCoverPick(reached->GetId(), approachSource);  // the cover log: its route's approach passed only with cover
		}
		if (!isAnchored) {
			SetScaleAnchor(reached, pickSqDist);  // the route led to another candidate than the pick
		}
		SetTarget(reached);
		position = rangePos;
		reinforce = nullptr;  // a hunt ends an army's walk to merge
	}
	pPath = path;
	// A line standing facing (a hold, the end of a walk) keeps facing on a path that ends nearby (FACE_KEEP) and
	// doesn't follow it: a target route is always walked, else a target just out of range is never engaged
	EndLineFacing();
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
	NoteCommReject(g.id);  // a commander attack: the commander leaves it alone for a while
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
		const AIFloat3* cell, int* outCoverSource) const
{
	// The ground we'd fight from, judged spot by spot with the common allowance (no support)
	auto allowedAt = [&](const AIFloat3& p, float threat) {
		return GetAllowedThreatAt(p, strength, refSqDist, threat, outCoverSource);
	};
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
	if (comm != nullptr) {
		// A commander attack never holds or heads for the front: no target it can reach, back to its jobs
		EndComm(CommEnd::NO_ROUTE);
		return;
	}
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	// quota.attack_balance: outnumbered where we stand, so never toward the enemy (the reason logged with the own spot)
	const bool isFlagged = IsFlagged();
	auto logFallback = [&](const char* fallback) {
		if (isFlagged) {
			LogBalanceSkip("own spot", nullptr, false, leader->GetPos(frame), spotForce, fallback, frame);
		}
	};
	// Nothing to attack: first walk to merge with another group (quota.attack_merge.idle_merge). Outnumbered: only one
	// nearer our base, combining is the best answer to being outnumbered
	if (SeekMerge(frame, isReuse, std::numeric_limits<float>::max(), isFlagged)) {
		logFallback("merge");
		return;
	}
	GetMilitaryMgr()->FillFrontPos(leader, urgentPositions);
	const AIFloat3& startPos = leader->GetPos(frame);
	const AIFloat3& basePos = circuit->GetSetupManager()->GetBasePos();
	const float sqOwnDist = startPos.SqDistance2D(basePos);
	if (IsBalanceLive()) {
		// quota.attack_balance: never back across a spot where we were outnumbered (a holding avoid zone, as for
		// targets), or the front walk leads into the army we just backed away from. Outnumbered: also only the front's
		// points nearer our base than we are (without the enemy-ground hold below)
		urgentPositions.erase(std::remove_if(urgentPositions.begin(), urgentPositions.end(), [&](const AIFloat3& p) {
			return (isFlagged && (p.SqDistance2D(basePos) >= sqOwnDist)) || IsInAvoidZone(p, frame);
		}), urgentPositions.end());
	}
	if (urgentPositions.empty()) {  // no front, or none of it we may walk to: hold here
		logFallback("hold");
		HoldPosition();
		return;
	}

	if (isFlagged) {
		logFallback("front");
	} else if (!isOwnGroundOnly && IsOnEnemyGround()) {
		// Restricted to our ground (isOwnGroundOnly): nothing out here may be picked, so walk back to the front
		// (a floor can switch on mid-push); there is no border to bounce on.
		// The front is our own cluster on friendly ground. Already past it (farther from our base than its
		// nearest point): routing there walks the group back, and it would bounce on the border, so hold at
		// the quietest spot nearby. Not past it (an enemy-held patch on the way): keep going to the front.
		float sqFrontDist = std::numeric_limits<float>::max();
		for (const AIFloat3& p : urgentPositions) {
			sqFrontDist = std::min(sqFrontDist, p.SqDistance2D(basePos));
		}
		if (sqOwnDist > sqFrontDist) {
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
	// quota.protect: a route into the commander's fight is capped for the strength present, as its goals are judged
	const CProtect* protect = GetMilitaryMgr()->GetProtect();
	float boost = 1.f;
	for (const AIFloat3& g : goals) {
		boost = std::max(boost, protect->GetBoost(this, g));
	}
	return GetPathThreatCap(startPos, goals, GetSafeThreat() * boost);
}

float CAttackTask::GetPathThreatCap(const AIFloat3& startPos, const F3Vec& goals, float strength) const
{
	if (comm != nullptr) {
		return std::numeric_limits<float>::max();  // a commander attack: IsCommRouteOk judges the route it finds
	}
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
	return strength * bonus / curve / std::max(scale, 1e-3f);
}

void CAttackTask::SetRouteThreat(IPathQuery* query, AttackRoute route, const CAttackTask* partner) const
{
	// First: the allowance and the floor below read the query's threat layer, which cover may replace
	SetRouteCover(query, partner);

	CMilitaryManager* militaryMgr = GetMilitaryMgr();
	const float strength = GetSafeThreat();
	const bool isCapped = (route == AttackRoute::TARGET) || (route == AttackRoute::FRONT)
			|| (route == AttackRoute::REINFORCE);
	// Dynamic: threat is weighed against the allowance at each cell (what the escape and the path-ahead check use):
	// light threat costs less than the flat 2x, threat at the allowance the same, heavier threat more (quadratic). Big
	// groups walk through what small ones go around. Escapes: capped, so a weak group under heavy fire doesn't trade a
	// short run through the worst of it for a much longer one through moderate threat (more time under fire, and a
	// wider search).
	// Capped routes (target, front, reinforce) also keep clear of ground just under the escape's line where they can
	// (quota.danger_margin): a route skirting a known turret there tips over once units fan out off the path or take a
	// few hits
	const bool isEscape = (route == AttackRoute::FLEE) || (route == AttackRoute::CORNERED);
	SetRouteAllowance(query, strength, militaryMgr->IsAttackRouteDynamic(route), /*isMargin*/ isCapped,
			isEscape ? militaryMgr->GetAttackFleePathMaxWeight() : 0.f);

	// Capped routes ignore light threat: at or below quota.attack_path_ignore_threat x the group's current strength it
	// costs nothing, so the route goes straight through it instead of around. Never above half the lowest per-spot
	// allowance anywhere (strength x the lowest base bonus / the attack thr_mod.distance curve's highest value;
	// near-base scale <= 1), or the path-ahead check and the escape would flag ground the route treats as free
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
	if ((target != nullptr) && (pos.SqDistance2D(target->GetPos()) < SQUARE(unit->GetFireRange()))) {
		return false;
	}
	return pos.SqDistance2D(leader->GetPos(frame)) > SQUARE(GetGroupRadius());
}

bool CAttackTask::IsGuardPullable(const CCircuitUnit* unit, bool isFromGroup) const
{
	// A lone unit's task just ends when it leaves. A squad of 2+ would get weaker: only for isFromGroup, and only while
	// it keeps quota.attack power without the unit. Never a fighting group, nor the rally host: every compatible group
	// is walking to it, nor a group quota.protect committed
	CMilitaryManager* militaryMgr = static_cast<CMilitaryManager*>(manager);
	if ((comm != nullptr) || IsFighting() || (militaryMgr->GetRallyHost() == this) || IsProtecting()) {
		return false;  // nor a commander attack (off the group list)
	}
	return (units.size() == 1) || (isFromGroup && (attackPower - unit->GetCircuitDef()->GetPower() >= minPower));
}

void CAttackTask::Engage(int frame)
{
	engageFrame = frame;  // IsRallyFighting
	Attack(frame);
	// Every unit holds off the target (no safe firing spot) while out of its range: the next pick passes on it
	// (6, 12, 24 s, as cell bans) instead of the group standing still. Not while banned: no level-up from a re-check
	if ((target != nullptr) && IsAllHolding() && !IsBanned(heldBans, target->GetId(), frame)) {
		Ban(heldBans, target, -1, frame);
	}
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

void CAttackTask::HoldPosition(bool isWalk)
{
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	const AIFloat3& pos = leader->GetPos(frame);
	AIFloat3 bestPos;
	float hereThreat;
	const float bestThreat = FindQuietSpot(pos, bestPos, &hereThreat);
	isHolding = true;
	if (isWalk && utils::is_valid(holdPos)) {
		if (!IsQueryReady(leader)) {
			return;  // its path is on the way
		}
		// Walking to the spot, or standing there in line: keep it unless another one is clearly quieter. Nothing is
		// sent again (a unit pulled back from danger rejoins with the next path, or a regroup)
		if (IsHoldKept() && (bestThreat >= GetTaskThreatAt(leader, holdPos) * HOLD_SWITCH_SHARE)) {
			pPath = holdPath;  // Start() and regrouping re-send pPath
			return;
		}
	}
	holdPos = -RgtVector;
	holdPath = nullptr;
	if (!isWalk) {
		MoveGroupTo(bestPos);  // hit: away at once (a danger step)
		return;
	}
	// Stay unless a spot is clearly quieter (in line where we can). Then go there: to a spot nearby straight (no route
	// query, as the straight move had none), in line where we can, else along a route
	if ((bestPos.SqDistance2D(pos) < 1.f) || (bestThreat >= hereThreat * HOLD_SWITCH_SHARE)) {
		if (!StandInLine(frame)) {
			MoveGroupTo(pos);
		}
		return;
	}
	if (bestPos.SqDistance2D(pos) < SQUARE(HOLD_NEAR)) {
		if (!WalkLineTo(bestPos, frame)) {
			MoveGroupTo(bestPos);
		}
		return;
	}
	holdPos = bestPos;
	CPathFinder* pathfinder = circuit->GetPathfinder();
	std::shared_ptr<IPathQuery> query = pathfinder->CreatePathSingleQuery(
			leader, circuit->GetThreatMap(), frame, pos, bestPos, pathfinder->GetSquareSize(), nullptr,
			GetPathThreatCap(pos, {bestPos}));
	SetRouteThreat(query.get(), AttackRoute::FRONT);
	RunRoute(query, [this](const IPathQuery* query) {
		this->ApplyHoldPath(static_cast<const CQueryPathSingle*>(query));
	});
}

void CAttackTask::ApplyHoldPath(const CQueryPathSingle* query)
{
	pPath = query->GetPathInfo();
	if (pPath->posPath.size() > 2) {
		holdPath = pPath;  // to holdPos
		EndLineFacing();  // a walk elsewhere, even if the route ends near where the line faces from (FACE_KEEP)
		ActivePath();  // the line walks it, and stands in line at its end
		return;
	}
	// No path (or a trivial one): straight there, in line where we can
	const AIFloat3 spot = holdPos;
	if (!WalkLineTo(spot, manager->GetCircuit()->GetLastFrame())) {
		MoveGroupTo(spot);  // as before
		holdPos = -RgtVector;
	}
}

float CAttackTask::FindQuietSpot(const AIFloat3& pos, AIFloat3& outPos, float* outHereThreat)
{
	// Quietest reachable spot within 2x our range: stay put when it is as quiet as anywhere around
	// (no creeping), and keep away from where we were last hit while holding (attackers we can't see
	// or reach leave no threat on the map)
	CCircuitAI* circuit = manager->GetCircuit();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	const int frame = circuit->GetLastFrame();
	STerrainMapArea* area = leader->GetArea();
	const bool isAvoidHit = (frame < hitFrame + FRAMES_PER_SEC * 30);
	auto isHitZone = [&](const AIFloat3& p) {
		return isAvoidHit && (p.SqDistance2D(hitPos) < SQUARE(hitRadius));
	};

	AIFloat3 bestPos = pos;
	float bestThreat = isHitZone(pos) ? std::numeric_limits<float>::max() : GetTaskThreatAt(leader, pos);
	if (outHereThreat != nullptr) {
		*outHereThreat = bestThreat;  // max: we stand where we were hit, any spot outside it beats here
	}
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
			const float threat = GetTaskThreatAt(leader, p);
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
	return (bestThreat < std::numeric_limits<float>::max()) ? bestThreat : GetTaskThreatAt(leader, pos);
}

void CAttackTask::Regroup(int frame)
{
	// In line (formation on, enough units): the line stops and the units walk to their slots in it, so laggards come
	// up into their rows instead of everyone packing onto the leader. Where no line fits, the units follow the short
	// path to its end (the leader's spot), as a gather would take them.
	// The path is made once per regroup; each check sends it again to units taken off it (pulled back from danger
	// and safe again, or skipped by Start), not to those still in danger
	if (IsStandAllowed(frame)) {
		// This regroup's path (one that began at groupFrame), still walked: a route that came back since replaces it
		const bool isKept = (regroupPath != nullptr) && (regroupFrame == groupFrame)
				&& (leader->GetTravelAct()->GetPath() == regroupPath);
		if (!isKept) {
			regroupPath = MakeStandPath(groupPos);
			regroupFrame = groupFrame;
		}
		SendStandPath(regroupPath, isKept);
		return;
	}
	const int timeout = frame + FRAMES_PER_SEC * 60;
	for (CCircuitUnit* unit : units) {
		unit->Gather(groupPos, timeout);
		unit->GetTravelAct()->StateWait();
	}
}

bool CAttackTask::IsStandAllowed(int frame) const
{
	// Formation on, and enough units that take a slot (aircraft, support and no_formation units don't): with fewer no
	// line forms, and the short stand path would only pack every unit onto one spot
	const CMilitaryManager::SFormationCfg& cfg = GetMilitaryMgr()->GetFormationCfg();
	if (!cfg.isEnable || !IsFormationAllowed(frame) || ((int)units.size() < cfg.minUnits)) {
		return false;
	}
	int count = 0;
	for (CCircuitUnit* unit : units) {
		if (CFormation::IsSlotUnit(unit->GetCircuitDef()) && (++count >= cfg.minUnits)) {
			return true;
		}
	}
	return false;
}

bool CAttackTask::IsOnPath(CCircuitUnit* unit, const std::shared_ptr<PathInfo>& path)
{
	ITravelAction* travelAct = unit->GetTravelAct();
	return (path != nullptr) && (travelAct->GetPath() == path) && !travelAct->IsWait();
}

bool CAttackTask::IsHoldKept() const
{
	// Not once its travel finished: a line that switched off while standing (no room for 2 columns) has its units
	// walk to the stand path's end and stop there, and it forms again only on a path walked anew
	ITravelAction* travelAct = leader->GetTravelAct();
	return (holdPath != nullptr) && (travelAct->GetPath() == holdPath) && travelAct->IsActive();
}

void CAttackTask::SendStandPath(const std::shared_ptr<PathInfo>& path, bool isKept)
{
	// Not to units still in danger (PullBackEndangered sent them away); with isKept (a regroup's path sent again) not
	// to units already on it (walking it, or at its end), so a stand sends no orders again
	for (CCircuitUnit* unit : units) {
		if ((isKept && IsOnPath(unit, path))
			|| (std::find(endangered.begin(), endangered.end(), unit) != endangered.end()))
		{
			continue;
		}
		ITravelAction* travelAct = unit->GetTravelAct();
		travelAct->SetPath(path);  // full speed when not steered, as a gather
		travelAct->StateActivate();
	}
}

bool CAttackTask::StandInLine(int frame)
{
	// Holding or waiting (beside the group we reinforce): the line stands where it is, as a
	// regroup's does, and faces the threat; units out of line walk to their slots. Kept while the leader walks it
	// (IsHoldKept): later calls send nothing (a unit pulled back from danger rejoins with the next path). Where no
	// line fits, the units follow the short path to its end, and the next call stands again. False: no line to stand
	// in (IsStandAllowed), the caller moves as before
	if (!IsStandAllowed(frame)) {
		return false;
	}
	isHolding = true;
	if (!IsHoldKept()) {
		holdPos = leader->GetPos(frame);  // a hold keeps this stand while no spot is clearly quieter than here
		holdPath = MakeStandPath(holdPos);
		SendStandPath(holdPath, false);
	}
	pPath = holdPath;  // Start() and regrouping re-send pPath
	return true;
}

bool CAttackTask::WalkLineTo(const AIFloat3& pos, int frame)
{
	// A hold's walk to pos, where it stands. Standing facing with pos nearby, the block slides there as it stands,
	// its front still to the threat (SlideLine); else the line walks the straight 2-point path there and faces anew.
	// False: no line (IsStandAllowed), the caller moves as before
	if (!IsStandAllowed(frame)) {
		return false;
	}
	const AIFloat3& leaderPos = leader->GetPos(frame);
	holdPos = pos;
	holdPath = std::make_shared<PathInfo>();
	const bool isNear = (pos.SqDistance2D(leaderPos) < SQUARE(HOLD_NEAR));
	if (!isNear || !SlideLine(pos - leaderPos, holdPath->posPath)) {
		EndLineFacing();  // the path may end within FACE_KEEP of where the line faces from: it walks all the same
		holdPath->posPath = {leaderPos, pos};
	}
	pPath = holdPath;
	SendStandPath(holdPath, false);  // as a stand: not to units still in danger
	return true;
}

std::shared_ptr<PathInfo> CAttackTask::MakeStandPath(const AIFloat3& pos) const
{
	// The line stands at the path's end, its rows back along the leader's track (else along the path's last stretch)
	std::shared_ptr<PathInfo> path = std::make_shared<PathInfo>();
	const CFormation* line = GetFormation();  // nullptr: the line is off
	if (line != nullptr) {
		line->GetStandPath(path->posPath);  // on: it stops where it stands
		return path;
	}
	// Off (often just after a fight, with no track to go by): the front row at pos, facing the nearest strong
	// enemy group (the most threat per distance), else away from our base. Standing, the line still turns to a clear
	// threat around it (facing)
	CCircuitAI* circuit = manager->GetCircuit();
	const CEnemyManager::SEnemyGroup* best = nullptr;
	float bestScore = 0.f;
	for (const CEnemyManager::SEnemyGroup& group : circuit->GetEnemyManager()->GetEnemyGroups()) {
		if (group.units.empty() || (group.threat <= 0.f)) {
			continue;
		}
		const float score = group.threat / std::max(pos.distance2D(group.pos), float(DEFAULT_SLACK * 4));
		if (score > bestScore) {
			bestScore = score;
			best = &group;
		}
	}
	AIFloat3 dir = (best != nullptr) ? (best->pos - pos) : (pos - circuit->GetSetupManager()->GetBasePos());
	dir.y = 0.f;
	if (dir.SqLength2D() >= 1.f) {
		// A first stretch of DEFAULT_SLACK towards pos gives the line its way (BuildLine, with no track)
		AIFloat3 back = pos - dir.Normalize2D() * DEFAULT_SLACK;
		CTerrainManager::CorrectPosition(back);
		path->posPath.push_back(back);
	}
	path->posPath.push_back(pos);
	return path;
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

/*
 * Escorts (quota.escort)
 */

bool CAttackTask::IsEscortUnit(CCircuitUnit* unit) const
{
	// Shields always (IFighterTask's default); armed support units in escort mode
	CCircuitDef* cdef = unit->GetCircuitDef();
	return cdef->IsRoleSupport() && (isEscortMode || !IsArmedUnit(cdef));
}

void CAttackTask::UpdateEscortMode(int frame)
{
	// Core: the non-support units, newcomers too. Power: the def power attackPower sums, of armed units only (shields
	// don't fight: a few of them mustn't push a small core under the share)
	const CMilitaryManager::SEscortCfg& cfg = GetMilitaryMgr()->GetEscortCfg();
	float power = 0.f;
	float corePower = 0.f;
	int coreCount = 0;
	for (CCircuitUnit* unit : units) {
		CCircuitDef* cdef = unit->GetCircuitDef();
		const float armedPower = IsArmedUnit(cdef) ? cdef->GetPower() : 0.f;
		power += armedPower;
		if (!cdef->IsRoleSupport()) {
			corePower += armedPower;
			++coreCount;
		}
	}
	const float share = (power > 0.f) ? (corePower / power) : ((coreCount > 0) ? 1.f : 0.f);
	const bool isWanted = (coreCount > 0) && (share >= (isEscortMode ? cfg.coreShareOff : cfg.coreShareOn));
	const bool isFirst = (ESCORT_MODE_UNSET == escortModeFrame);
	if (isWanted == isEscortMode) {
		escortModeFrame = -1;
		return;
	}
	// Each change holds mode_hold_sec first, so a group near a threshold doesn't flip with every loss and join.
	// No core: off at once
	if (!isFirst && (coreCount > 0)) {
		if (escortModeFrame < 0) {
			escortModeFrame = frame;
			return;
		}
		if (frame < escortModeFrame + cfg.modeHoldFrames) {
			return;
		}
	}
	isEscortMode = isWanted;
	escortModeFrame = -1;
	UpdateHighestRange();  // the group's fire range leaves escorts out
	lastRoute.path = nullptr;  // the pick strength changed: next decision plans anew
	const int supportCount = static_cast<int>(units.size()) - coreCount;
	if ((supportCount > 0) && (leader != nullptr)) {
		manager->GetCircuit()->LOG("ESCORT #%i (%i units, %i support) mode %s: %s, core share %.2f", leader->GetId(),
				static_cast<int>(units.size()), supportCount, isEscortMode ? "on" : "off",
				(coreCount == 0) ? "no core" : isEscortMode ? "core holds on" : "core thin", share);
	}
}

AIFloat3 CAttackTask::GetCoreCentre(int frame, std::vector<AIFloat3>* outPositions) const
{
	const AIFloat3& leaderPos = leader->GetPos(frame);
	const float sqGroupRadius = SQUARE(GetGroupRadius());
	AIFloat3 sum(ZeroVector);
	int count = 0;
	for (CCircuitUnit* unit : units) {
		if (unit->GetCircuitDef()->IsRoleSupport()) {
			continue;
		}
		const AIFloat3& pos = unit->GetPos(frame);
		if (pos.SqDistance2D(leaderPos) > sqGroupRadius) {
			continue;  // a straggler walking in
		}
		sum += pos;
		++count;
		if (outPositions != nullptr) {
			outPositions->push_back(pos);
		}
	}
	if (count == 0) {
		return leaderPos;
	}
	AIFloat3 centre = sum / count;
	CTerrainManager::CorrectPosition(centre);
	return centre;
}

float CAttackTask::GetStayRadius(const CCircuitDef* cdef) const
{
	// No direction: the disc doesn't swing across the group as it turns. An escort that outranges the group still
	// stands behind the core: firing spots lie at about 0.9x its range from the target, on the side facing it
	const CMilitaryManager::SEscortCfg& cfg = GetMilitaryMgr()->GetEscortCfg();
	const float range = std::max(cdef->GetMaxRange(CCircuitDef::RangeType::LAND),
			cdef->GetMaxRange(CCircuitDef::RangeType::WATER));
	return cfg.stayRadius + std::min(cfg.offsetCap, std::max(0.f, range - GetGroundFireRange()));
}

float CAttackTask::GetEscortReach(const CCircuitDef* cdef) const
{
	const float fireRange = GetGroundFireRange();
	if (fireRange <= 0.f) {
		return 1.f;
	}
	const float range = std::max(cdef->GetMaxRange(CCircuitDef::RangeType::LAND),
			cdef->GetMaxRange(CCircuitDef::RangeType::WATER));
	return std::min(1.f, (range + GetMilitaryMgr()->GetEscortCfg().stayRadius) / fireRange);
}

bool CAttackTask::ReleaseUnarmed()
{
	if (units.empty()) {
		return false;
	}
	for (CCircuitUnit* unit : units) {
		if (!IsUnarmedSupport(unit->GetCircuitDef())) {
			return false;
		}
	}
	manager->GetCircuit()->LOG("ESCORT #%i (%i units) no armed unit left: back to finding a group", leader->GetId(),
			static_cast<int>(units.size()));
	static std::vector<CCircuitUnit*> released;  // NOTE: micro-opt
	released.assign(units.begin(), units.end());
	ReleaseStuck(released);  // to the idle pool (a new CSupportTask); the task aborts with its last unit
	return true;
}

void CAttackTask::OrderEscorts(std::vector<SFireClaim>& claims, std::vector<SOrder>& orders, int frame, bool isRepeat,
		int timeout)
{
	// Escorts in the claims (not busy, not stragglers: Engage walks those to the leader), and the widest reach of the
	// armed ones (their candidate enemies)
	static std::vector<size_t> escorts;  // NOTE: micro-opt
	static std::vector<CEnemyInfo*> currents;  // their targets: compared only, kept if still among the candidates
	escorts.clear();
	currents.clear();
	int shieldCount = 0;
	float maxReach = 0.f;
	const CMilitaryManager::SEscortCfg& cfg = GetMilitaryMgr()->GetEscortCfg();
	for (size_t i = 0; i < claims.size(); ++i) {
		CCircuitUnit* unit = claims[i].unit;
		if (!IsEscortUnit(unit) || (unit->Blocker() != nullptr) || IsScooting(unit) || IsOutOfReach(unit, frame)) {
			continue;
		}
		escorts.push_back(i);
		CCircuitDef* cdef = unit->GetCircuitDef();
		if (IsArmedUnit(cdef)) {
			maxReach = std::max(maxReach, GetStayRadius(cdef) * cfg.keepMod + unit->GetFireRange());
			if (unit->GetTarget() != nullptr) {
				currents.push_back(unit->GetTarget());
			}
		} else {
			++shieldCount;
		}
	}
	if (escorts.empty()) {
		return;
	}
	CCircuitAI* circuit = manager->GetCircuit();
	static std::vector<AIFloat3> corePositions;  // NOTE: micro-opt
	corePositions.clear();
	spotDiscPos = GetCoreCentre(frame, &corePositions);  // every escort's disc centre (OrderInDisc, HoldInDisc)
	const AIFloat3 centre = spotDiscPos;

	// Candidates, once for all escorts: enemies we see within the widest reach of the centre (buildings only as the
	// group's target or a kept one). A threat to the core is an armed mobile enemy within its weapon range +
	// ESCORT_THREAT_SEC of its travel of a core unit; threats first, nearest to the core first
	struct SCandidate {
		CEnemyInfo* enemy;
		float sqCoreDist;  // to the nearest core unit
		bool isThreat;
	};
	static std::vector<SCandidate> cands;  // NOTE: micro-opt
	cands.clear();
	if (maxReach > 0.f) {
		for (auto it = spotDiscBans.begin(); it != spotDiscBans.end();) {
			it = (it->second <= frame) ? spotDiscBans.erase(it) : std::next(it);
		}
		const float sqMaxReach = SQUARE(maxReach + DEFAULT_SLACK);
		for (auto& kv : circuit->GetEnemyInfos()) {
			CEnemyInfo* enemy = kv.second;
			if (enemy->IsHidden() || (enemy->GetPos().SqDistance2D(centre) > sqMaxReach)) {
				continue;
			}
			CCircuitDef* edef = enemy->GetCircuitDef();
			if ((edef != nullptr) && !edef->IsMobile() && (enemy != target)
				&& (std::find(currents.begin(), currents.end(), enemy) == currents.end()))
			{
				continue;
			}
			float sqCoreDist = std::numeric_limits<float>::max();
			for (const AIFloat3& p : corePositions) {
				sqCoreDist = std::min(sqCoreDist, p.SqDistance2D(enemy->GetPos()));
			}
			float eRange;
			const bool isThreat = (edef != nullptr) && IsGroundDanger(enemy, false, eRange)
					&& (sqCoreDist <= SQUARE(eRange + edef->GetSpeed() * ESCORT_THREAT_SEC));
			cands.push_back({enemy, sqCoreDist, isThreat});
		}
		std::stable_sort(cands.begin(), cands.end(), [](const SCandidate& a, const SCandidate& b) {
			return a.isThreat && (!b.isThreat || (a.sqCoreDist < b.sqCoreDist));
		});
	}

	int shieldIdx = 0;
	for (size_t i : escorts) {
		SFireClaim& claim = claims[i];
		CCircuitUnit* unit = claim.unit;
		CCircuitDef* cdef = unit->GetCircuitDef();
		const AIFloat3& uPos = unit->GetPos(frame);
		const bool wasWalking = unit->GetTravelAct()->IsActive();
		unit->GetTravelAct()->StateWait();

		// Unarmed (shields): never an attack order. Each to its own point round the core's centre (the first on it, the
		// rest on a small ring), so they don't pile up on one spot
		if (!IsArmedUnit(cdef)) {
			AIFloat3 pos = centre;
			if ((shieldIdx > 0) && (shieldCount > 1)) {
				const float angle = 2.f * static_cast<float>(M_PI) * (shieldIdx - 1) / (shieldCount - 1);
				pos.x += ESCORT_SHIELD_RING * cosf(angle);
				pos.z += ESCORT_SHIELD_RING * sinf(angle);
				CTerrainManager::CorrectPosition(pos);
			}
			++shieldIdx;
			fireSpots.erase(unit);
			claim.pos = pos;
			const float slack = std::max(cdef->GetShieldRadius() * ESCORT_SHIELD_SLACK, ESCORT_SHIELD_RING);
			if (isRepeat || wasWalking || (uPos.SqDistance2D(pos) > SQUARE(slack))) {
				unit->ClearTarget();
				MoveUnitTo(unit, pos, frame);
			}
			continue;
		}

		const float discRadius = GetStayRadius(cdef);
		const float keepRadius = discRadius * cfg.keepMod;
		const float sqKeepMod = SQUARE(cfg.keepMod);
		const float sqSpeed = SQUARE(cdef->GetSpeed() * CHASE_SPEED_MOD / FRAMES_PER_SEC);
		const float sqOutrangeSpeed = SQUARE(cdef->GetSpeed() * OUTRANGE_CHASE_SPEED_MOD / FRAMES_PER_SEC);
		const float ourRange = std::max(unit->GetFireRange(CCircuitDef::RangeType::LAND),
				unit->GetFireRange(CCircuitDef::RangeType::WATER));
		const bool isEmpOnly = (cdef->GetShotDamage() <= 0.f);  // paralyzers only (Venom): a stunned enemy waits
		// Artillery (high trajectory: Firewalker, Emissary, Tremor) doesn't shell enemies within its splash of a core
		// unit: its shells scatter. Direct-fire splash (riots) still covers the core
		const float sqSplash = cdef->IsHighTrajectory() ? SQUARE(cdef->GetAoe()) : 0.f;
		const int canTargetCat = cdef->GetTargetCategory();
		const bool notAA = !cdef->HasAntiAir();
		CEnemyInfo* current = unit->GetTarget();  // compared only, until found among the candidates (alive, seen)
		auto isOk = [&](const SCandidate& c, float r) {
			CEnemyInfo* enemy = c.enemy;
			CCircuitDef* edef = enemy->GetCircuitDef();
			if ((edef != nullptr) && (((edef->GetCategory() & canTargetCat) == 0) || (edef->IsAbleToFly() && notAA))) {
				return false;
			}
			return !(isEmpOnly && enemy->IsParalyzed())
				&& (c.sqCoreDist >= sqSplash)
				&& !IsDiscBanned(enemy, cdef)
				&& !IsRunningAway(enemy, centre, sqSpeed, sqOutrangeSpeed, ourRange)
				&& IsDiscReach(unit, enemy, r);
		};
		// The two best picks it can reach: threats to the core by distance (the current one's divided by keep_mod², and
		// judged from its disc grown by keep_mod), then the current target if it is no threat (grown disc), then the
		// group's target. The group's target never holds an escort back from a threat to the core
		struct SPick {
			CEnemyInfo* enemy = nullptr;
			float radius = 0.f;
			int tier = 3;
			float key = 0.f;
			bool IsBefore(const SPick& o) const { return (tier < o.tier) || ((tier == o.tier) && (key < o.key)); }
		};
		SPick best, next;
		for (const SCandidate& c : cands) {
			const bool isCurrent = (c.enemy == current);
			SPick p;
			p.enemy = c.enemy;
			p.radius = isCurrent ? keepRadius : discRadius;
			if (c.isThreat) {
				p.tier = 0;
				p.key = isCurrent ? (c.sqCoreDist / sqKeepMod) : c.sqCoreDist;
			} else if (isCurrent) {
				p.tier = 1;
			} else if (c.enemy == target) {
				p.tier = 2;
			} else {
				continue;
			}
			if (!p.IsBefore(next) || !isOk(c, p.radius)) {
				continue;
			}
			if (p.IsBefore(best)) {
				next = best;
				best = p;
			} else {
				next = p;
			}
		}

		if (best.enemy == nullptr) {  // nothing to hit from the disc: hold, sent again only when something changed
			if (isRepeat || wasWalking || (current != nullptr) || (uPos.SqDistance2D(centre) > SQUARE(discRadius))) {
				HoldInDisc(claim, discRadius, frame, timeout);
			} else {
				claim.pos = uPos;
			}
			continue;
		}
		// Nothing changed (the same pick, its spot still in the disc and clear of kite discs, no repeat due): its order
		// stands
		AIFloat3 keptPos;
		if (!isRepeat && !wasWalking && (best.enemy == current) && GetKeptFireSpot(unit, false, current, keptPos)
			&& (keptPos.SqDistance2D(centre) <= SQUARE(best.radius)) && !IsKiteRespot(unit, frame))
		{
			claim.pos = keptPos;
			continue;
		}
		// Ordered in the pass with the others, longest range first (OrderOnTarget, OrderInDisc)
		orders.push_back({i, GetSpotRange(unit, best.enemy), best.enemy, best.radius, next.enemy, next.radius});
	}
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

	// Holding or waiting, only a unit walking our path is started again (Start sends the stand path then): one pulled
	// back from danger would walk straight back into the slot that endangered it, and one at the path's end would
	// get the same move every second; they rejoin with the next path, or a regroup
	if ((units.find(unit) != units.end()) && !IsIdleInLine(unit)
		&& (!isHolding || unit->GetTravelAct()->IsActive()))
	{
		Start(unit);  // NOTE: Not sure if it has effect
	}
}

void CAttackTask::OnUnitDestroyed(CCircuitUnit* unit, CEnemyInfo* attacker)
{
	if (unit == comm) {
		EndComm(unit->IsMorphing() ? CommEnd::MORPH : CommEnd::DEATH);  // a finished morph replaces the unit
	}
	NoteCoverLoss(unit, manager->GetCircuit()->GetLastFrame());  // the cover log, while the unit is still ours
	ISquadTask::OnUnitDestroyed(unit, attacker);
}

void CAttackTask::OnUnitDamaged(CCircuitUnit* unit, CEnemyInfo* attacker)
{
	if (unit == comm) {
		OnCommDamaged(unit);  // the builder retreat; a commander attack never holds
		return;
	}
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
	// A regroup would send the units straight back (Regroup, or the walk resumed at its end): it ends here, and the
	// next decision plans afresh
	if (State::REGROUP == state) {
		state = State::ROAM;
	}

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
	HoldPosition(false);  // hit: away at once, no waiting for a path
}

} // namespace circuit

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
// quota.attack_static_cells: a route to a static target's firing cell is kept this long (buildings don't move)
static constexpr int STATIC_ROUTE_REUSE_FRAMES = FRAMES_PER_SEC * 6;
// Static targets checked for firing cells per target decision; beyond that they are routed to as discs
static constexpr int MAX_STATIC_CHECKS = 32;
// A static target with no safe firing cell is banned for REJECTED_TARGET_FRAMES << level, level up to this
static constexpr int STATIC_BAN_MAX_LEVEL = 2;
// ... and the ban and its level are dropped this long after the first ban in a row, or when the threat at the target
// changes by more than STATIC_BAN_THREAT_CHANGE (fraction)
static constexpr int STATIC_BAN_RESET_FRAMES = FRAMES_PER_SEC * 60;
static constexpr float STATIC_BAN_THREAT_CHANGE = 0.25f;
// A planned firing cell guides the engagement (in-range check, firing-spot arc) for this long after its route
static constexpr int PLANNED_CELL_FRAMES = FRAMES_PER_SEC * 30;

CAttackTask::CAttackTask(ITaskManager* mgr, float minPower, float powerMod)
		: ISquadTask(mgr, FightType::ATTACK, powerMod)
		, minPower(minPower)
		, reinforce(nullptr)
		, reinforceFails(0)
		, reinforceRetryFrame(-1)
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
		, plannedTargetId(-1)
		, plannedCell(-RgtVector)
		, plannedFrame(0)
		, fireRangeLand(0.f)
		, fireRangeWater(0.f)
		, targetRouteTries(0)
		, scaleRefSqDist(0.f)
		, anchorTargetId(-1)
		, pickSqDist(0.f)
		, lastRoute({nullptr, -RgtVector, -1, 0, ROUTE_REUSE_FRAMES, AIFloat3(-RgtVector), false})
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

	// Group fire range (quota.attack_static_cells): power-weighted median of the weapon ranges of the non-support
	// units that can hit this target type (all units if there are none)
	auto medianRange = [this](CCircuitDef::RangeType rt) {
		static std::vector<std::pair<float, float>> ranges;  // range, power; NOTE: micro-opt
		ranges.clear();
		float total = 0.f;
		for (int pass = 0; (pass < 2) && ranges.empty(); ++pass) {
			for (CCircuitUnit* unit : units) {
				CCircuitDef* cdef = unit->GetCircuitDef();
				const float range = cdef->GetMaxRange(rt);
				// pass 0: shooters of this target type, not support; pass 1 (none): everyone
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
	};
	fireRangeLand = medianRange(CCircuitDef::RangeType::LAND);
	fireRangeWater = medianRange(CCircuitDef::RangeType::WATER);
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
	if (UpdateEscape(frame)) {
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
	if (attackPower >= minPower) {
		reinforce = nullptr;  // an army itself (grown by merges)
	} else if ((reinforce == nullptr) && (State::ENGAGE != state) && (frame >= reinforceRetryFrame)) {
		reinforce = GetMilitaryMgr()->FindReinforceSquad(leader, attackPower, this);
	}
	const AIFloat3& startPos = leader->GetPos(frame);
	if ((reinforce != nullptr) && (GetReinforceSquad() != nullptr)) {
		// Fight back on the way: a target that passes the usual checks (FindTarget) and is already within range gets
		// attacked; otherwise keep walking
		FindTarget();
		if ((target != nullptr) && (target->GetPos().SqDistance2D(startPos) < SQUARE(highestRange))) {
			state = State::ENGAGE;
			Engage(frame);
			return;
		}
		ClearScaleAnchor();  // walking to a group, not into an accepted fight
		Reinforce(frame);
		return;
	}  // else: nothing to join, carry on as a small attack group

	// A route is still being worked out: keep the current pick until it comes back, unless the target is
	// already within range (then engage as usual)
	if (!IsQueryReady(leader) && (target != nullptr)
		&& (target->GetPos().SqDistance2D(startPos) >= SQUARE(highestRange)))
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
	if (isReuse && ReuseRoute(target->GetId(), target->GetPos(), frame)) {
		return;
	}
	// Route-aware pick: one capped route to all candidates; whichever it reaches first becomes the target,
	// if the route's approach to it is safe
	targetRouteTries = 0;
	rejectedCells.clear();
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
	const CMilitaryManager::SAttackMerge& cfg = GetMilitaryMgr()->GetAttackMerge();
	const int interval = (attackPower < minPower) ? cfg.joinSmall : cfg.joinArmy;  // quota.attack_merge.join_interval
	return ((updCount % interval == 1 % interval) && !IsFighting()) ? CheckMergeTask() : nullptr;
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
	const int absorbInterval = GetMilitaryMgr()->GetAttackMerge().absorb;
	if ((attackPower < minPower) || (updCount % absorbInterval != 5 % absorbInterval)) {
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
			|| (candy->GetAttackPower() >= minPower)
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
		&& (reinforce->GetLeader() != nullptr) && (reinforce->GetAttackPower() > attackPower))
	{
		return reinforce;
	}
	// Group gone or no longer stronger: pick the nearest other stronger one
	reinforce = militaryMgr->FindReinforceSquad(leader, attackPower, this);
	return reinforce;
}

void CAttackTask::Reinforce(const int frame)
{
	SetTarget(nullptr);
	state = State::ROAM;

	if (!IsQueryReady(leader)) {
		return;
	}
	// Threat-aware path to the group's leader, capped like every other route (GetPathThreatCap: the common
	// allowance). It only has to get within reinforce_range (800), so the hot zone around a fighting group doesn't block
	// it; merging takes over inside the merge distance (CheckMergeTask from our side, AbsorbWeakerTasks from
	// an army's side). Range: quota.attack_merge.reinforce_range.
	CCircuitAI* circuit = manager->GetCircuit();
	CPathFinder* pathfinder = circuit->GetPathfinder();
	const AIFloat3& startPos = leader->GetPos(frame);
	position = reinforce->GetLeaderPos(frame);  // validated by GetReinforceSquad this update
	// Never stop farther out than we can merge from (the merge distance, or travel_sec at our slowest speed), with
	// a margin for the host moving: otherwise a slow group would park short of the host and never merge
	const CMilitaryManager::SAttackMerge& cfg = GetMilitaryMgr()->GetAttackMerge();
	const float mergeReach = std::min(cfg.distance, cfg.travelSec * lowestSpeed);
	const float pathRange = std::max(std::min(cfg.reinforceRange, mergeReach * 0.8f),
			static_cast<float>(pathfinder->GetSquareSize()));

	std::shared_ptr<IPathQuery> query = pathfinder->CreatePathSingleQuery(
			leader, circuit->GetThreatMap(), frame,
			startPos, position, pathRange, nullptr, GetPathThreatCap(startPos, {position}));
	SetRouteThreat(query.get(), AttackRoute::REINFORCE);
	RunRoute(query, [this](const IPathQuery* query) {
		this->ApplyReinforcePath(static_cast<const CQueryPathSingle*>(query));
	});
}

void CAttackTask::ApplyReinforcePath(const CQueryPathSingle* query)
{
	pPath = query->GetPathInfo();

	if (!pPath->path.empty()) {
		reinforceFails = 0;
		if (pPath->path.size() > 2) {
			ActivePath(lowestSpeed);
		}
		return;
	}
	const CMilitaryManager::SAttackMerge& cfg = GetMilitaryMgr()->GetAttackMerge();
	if (++reinforceFails >= cfg.reinforceFails) {
		// No safe route quota.attack_merge.reinforce_fails times in a row (10: ~20 s at a decision every ~2 s):
		// act as a small attack group for reinforce_retry (10 s), then look again
		// (merge checks and absorbing still apply meanwhile)
		reinforce = nullptr;
		reinforceFails = 0;
		reinforceRetryFrame = manager->GetCircuit()->GetLastFrame() + cfg.reinforceRetry;
	}
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
	// it may be in this same fight, or past the enemy
	CAttackTask* squad = militaryMgr->FindReinforceSquad(leader, attackPower, this);
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
	UpdateStaticBans(frame);

	// Contested clusters (threat there blocks our mex expansion) that this group can take: enemies there count as
	// quota.contested.bonus times closer. Judged at the full allowance from where we stand (no near-base anchor)
	static std::vector<CMilitaryManager::SContestedSpot> contested;  // NOTE: micro-opt
	militaryMgr->GetTakeableContested(leader, highestRange,
			[this, maxPower](const AIFloat3& p) { return GetAllowedThreatAt(p, maxPower, 0.f); }, contested);
	// Off while any candidate is already within range: don't walk off from a fight for a contested target
	bool isContestedOn = !contested.empty();

	// Targets whose route approach was unsafe are skipped for a while (else stickiness picks them again)
	for (auto it = rejectedUntil.begin(); it != rejectedUntil.end();) {
		it = (it->second <= frame) ? rejectedUntil.erase(it) : std::next(it);
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

	struct SCandidate {
		float key;  // rawKey x target_dist_mod^2, / quota.contested.bonus^2 in a contested cluster: the ranking
		float rawKey;  // squared distance x near-base scale: about what the route sees
		CEnemyInfo* enemy;
	};
	static std::vector<SCandidate> candidates;  // NOTE: micro-opt
	candidates.clear();
	for (auto& kv : circuit->GetEnemyInfos()) {
		CEnemyInfo* enemy = kv.second;
		// Cheap checks first
		if (enemy->IsHidden() || (enemy->GetTasks().size() > 2)
			|| (rejectedUntil.find(enemy->GetId()) != rejectedUntil.end())
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
		// Ranked by Own to Enemy distance x near-base scale, times the type's target_dist_mod (behaviour.json)
		// so unwanted types count as farther
		const float scale = std::min(ePos.SqDistance2D(basePos) / sqOBDist, 1.f);
		const float sqOEDist = pos.SqDistance2D(ePos);
		const float rawKey = sqOEDist * scale;
		candidates.push_back({rawKey * SQUARE(distModFor(enemy)), rawKey, enemy});
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
	// quota.attack_static_cells: a static target beyond range is routed to its safe firing cells (one goal per cell)
	const CMilitaryManager::SAttackStaticCells& cellCfg = militaryMgr->GetAttackStaticCells();
	NSMicroPather::TestFunc cellHitTest;  // made on first use
	static std::vector<SFiringCell> cells;  // NOTE: micro-opt
	int staticChecks = 0;
	size_t targetCount = 0;  // goals are counted by target: a static target may bring several cells
	for (const SCandidate& c : candidates) {
		const AIFloat3& ePos = c.enemy->GetPos();
		const bool isPrev = (c.enemy->GetId() == prevTargetId);
		const bool isInRange = (ePos.SqDistance2D(pos) < sqHighestRange);
		// A target within range is engaged at once, without a route: the straight line from it towards us is
		// its approach check. One beyond range gets no line check: its route's approach is checked instead
		// (ApplyTargetPath / IsApproachSafe)
		if (isInRange) {
			const float power = powerFor(c.enemy);
			const float ref = refFor(c.enemy);
			auto allowedAt = [&](const AIFloat3& p) { return GetAllowedThreatAt(p, power, ref); };
			// The current target with a planned firing cell: judged on the side we plan to fight from, as its route
			// was (the line from it to the cell, and our walk there). Else, or that side went hot: the line towards us
			AIFloat3 cell;
			const bool isPlannedSide = isPrev && GetPlannedCell(c.enemy, power, ref, cell)
					&& IsLineSafe(ePos, cell, highestRange, allowedAt)
					&& IsLineSafe(pos, cell, std::numeric_limits<float>::max(), allowedAt);
			if (!isPlannedSide && !IsLineSafe(ePos, pos, highestRange, allowedAt)) {
				continue;
			}
		}
		// The route goes to whichever goal it reaches first, i.e. about the really nearest. So leave out a
		// candidate really nearer than the best but ranked behind it by target_dist_mod or quota.contested: it would
		// take the route (kept if it is our current target, for stickiness)
		if ((best != nullptr) && (c.rawKey < best->rawKey) && !isPrev) {
			continue;
		}
		// Static target beyond range: its firing cells. Only in addition to the check at its own spot above, so
		// never more aggressive than without; refused only when every place we could shoot from is too hot
		CellResult cellResult = CellResult::DISC;
		const CCircuitDef* edef = c.enemy->GetCircuitDef();
		if (cellCfg.isOn && !isInRange && (edef != nullptr) && !edef->IsMobile() && (staticChecks < MAX_STATIC_CHECKS)) {
			if (IsStaticBanned(c.enemy->GetId(), frame)) {
				continue;
			}
			++staticChecks;
			if (cellHitTest == nullptr) {
				cellHitTest = ISquadTask::MakeHitTest(terrainMgr, cellCfg.aimHeight);
			}
			cellResult = FillFiringCells(c.enemy, powerFor(c.enemy), refFor(c.enemy), cellHitTest, cells);
			if (CellResult::NONE == cellResult) {
				BanStatic(c.enemy, frame);
				continue;
			}
		}
		if (best == nullptr) {
			best = &c;
		}
		if (isPrev) {
			prev = &c;
		}
		if (CellResult::CELLS == cellResult) {
			for (const SFiringCell& cell : cells) {
				routeGoals.push_back({c.enemy->GetId(), ePos, c.key, cell.pos, cell.sector});
			}
		} else {
			routeGoals.push_back({c.enemy->GetId(), ePos, c.key});
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

	// Stickiness: the route then only gets the kept target (all of its goals), so it can't swap to a near-equal one
	// either. No other goal could be clearly nearer than it: every screened one ranks at or behind the best, and
	// sticky means the best isn't clearly nearer
	if (isSticky) {
		best = prev;
		const ICoreUnit::Id prevId = prev->enemy->GetId();
		routeGoals.erase(std::remove_if(routeGoals.begin(), routeGoals.end(),
				[prevId](const STargetGoal& g) { return g.id != prevId; }), routeGoals.end());
	}

	if (best != nullptr) {
		SetTarget(best->enemy);
		position = target->GetPos();
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

CAttackTask::CellResult CAttackTask::FillFiringCells(const CEnemyInfo* enemy, float power, float ref,
		const NSMicroPather::TestFunc& hitTest, std::vector<SFiringCell>& outCells) const
{
	// Points on rings around the target (the outer ring first, the inner only if the outer yields nothing), the
	// sectors facing us first (the far ones only if fewer than 2 near sectors yield a cell). A point is a cell if
	// we can reach it, its threat is below the allowance (the pick's power and reference), and it sees the target.
	// Best point per sector by score; cells scoring more than score_slack worse than the best are left out.
	// Call after threatMap->SetThreatType(leader)
	outCells.clear();
	const CMilitaryManager::SAttackStaticCells& cfg = GetMilitaryMgr()->GetAttackStaticCells();
	CCircuitAI* circuit = manager->GetCircuit();
	CThreatMap* threatMap = circuit->GetThreatMap();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	const int frame = circuit->GetLastFrame();
	const AIFloat3& tPos = enemy->GetPos();
	const AIFloat3& lPos = leader->GetPos(frame);
	const float fireRange = (tPos.y < -SQUARE_SIZE * 5) ? fireRangeWater : fireRangeLand;
	if (fireRange < SQUARE_SIZE * 4) {
		return CellResult::DISC;  // melee group: as close as it gets
	}

	AIFloat3 toUs = lPos - tPos;
	const float bearingUs = (toUs.Length2D() > 1.f) ? atan2f(toUs.z, toUs.x) : 0.f;
	auto pointAt = [&](float angle, float radius) {
		AIFloat3 p(tPos.x + radius * cosf(angle), tPos.y, tPos.z + radius * sinf(angle));
		CTerrainManager::CorrectPosition(p);
		return p;
	};
	// Fast path: undefended (no threat at it, nor at the outer ring point facing us)
	if ((threatMap->GetThreatAt(tPos) <= THREAT_MIN)
		&& (threatMap->GetThreatAt(pointAt(bearingUs, fireRange * cfg.rings[0])) <= THREAT_MIN))
	{
		return CellResult::DISC;
	}

	const int sqSize = circuit->GetPathfinder()->GetSquareSize();
	const int maxX = terrainMgr->GetSectorXSize() - 1;
	const int maxZ = terrainMgr->GetSectorZSize() - 1;
	auto toPathXY = [sqSize, maxX, maxZ](const AIFloat3& p) {
		return int2(utils::clamp(int(p.x) / sqSize, 0, maxX), utils::clamp(int(p.z) / sqSize, 0, maxZ));
	};
	const int2 targetXY = toPathXY(tPos);
	STerrainMapArea* area = leader->GetArea();
	const float strength = std::max(power, THREAT_MIN);
	const int sectors = cfg.sectors;
	const int angles = cfg.angles;
	const float sectorArc = 2.f * static_cast<float>(M_PI) / sectors;

	struct SBest {
		AIFloat3 pos;
		float score;
		bool isSet;
	};
	static std::vector<SBest> best;  // by sector; NOTE: micro-opt
	static std::vector<char> isNear;  // by sector: its middle within 90 degrees of our bearing
	isNear.resize(sectors);
	for (int sec = 0; sec < sectors; ++sec) {
		float diff = fabsf((sec + 0.5f) * sectorArc - bearingUs);
		diff = fmodf(diff, 2.f * static_cast<float>(M_PI));
		diff = std::min(diff, 2.f * static_cast<float>(M_PI) - diff);
		isNear[sec] = (diff <= static_cast<float>(M_PI) * 0.5f);
	}

	bool isReachable = false;  // some point is reachable (none: an island, plateau or far shore; route as before)
	bool isThreatOk = false;  // some reachable point is below the allowance (a "no line of sight" miss, not a ban)
	for (int ring = 0; ring < 2; ++ring) {
		const float radius = fireRange * cfg.rings[ring];
		best.assign(sectors, {AIFloat3(), 0.f, false});
		for (int pass = 0; pass < 2; ++pass) {  // 0: near sectors, 1: far sectors
			if (pass == 1) {
				int nearCells = 0;
				for (int sec = 0; sec < sectors; ++sec) {
					nearCells += (isNear[sec] && best[sec].isSet) ? 1 : 0;
				}
				if (nearCells >= 2) {
					break;
				}
			}
			for (int i = 0; i < angles; ++i) {
				const int sec = i * sectors / angles;
				if ((isNear[sec] != 0) != (pass == 0)) {
					continue;
				}
				const AIFloat3 p = pointAt(i * 2.f * static_cast<float>(M_PI) / angles, radius);
				if (!terrainMgr->CanMoveToPos(area, p)) {
					continue;
				}
				isReachable = true;
				const float threat = threatMap->GetThreatAt(p);
				if (threat >= GetAllowedThreatAt(p, power, ref)) {
					continue;
				}
				isThreatOk = true;
				if (!hitTest(toPathXY(p), targetXY)) {
					continue;
				}
				const float score = cfg.threatWeight * std::max(threat, 0.f) / strength + p.distance2D(lPos) / fireRange;
				if (!best[sec].isSet || (score < best[sec].score)) {
					best[sec] = {p, score, true};
				}
			}
		}
		float bestScore = std::numeric_limits<float>::max();
		for (const SBest& b : best) {
			if (b.isSet) {
				bestScore = std::min(bestScore, b.score);
			}
		}
		for (int sec = 0; sec < sectors; ++sec) {
			if (best[sec].isSet && (best[sec].score <= bestScore + cfg.scoreSlack)) {
				outCells.push_back({best[sec].pos, sec});
			}
		}
		if (!outCells.empty()) {
			return CellResult::CELLS;
		}
	}
	// No cell: no point reachable (the disc route may still reach ground in range, e.g. a shore), or below the
	// allowance somewhere but no line of sight (a pessimistic test): route to it as before. Too hot everywhere we
	// can reach: ban
	return (!isReachable || isThreatOk) ? CellResult::DISC : CellResult::NONE;
}

bool CAttackTask::GetPlannedCell(const CEnemyInfo* enemy, float strength, float refSqDist, AIFloat3& outCell) const
{
	CCircuitAI* circuit = manager->GetCircuit();
	if ((enemy == nullptr) || (enemy->GetId() != plannedTargetId)
		|| (circuit->GetLastFrame() >= plannedFrame + PLANNED_CELL_FRAMES)
		|| (circuit->GetThreatMap()->GetThreatAt(leader, plannedCell) >= GetAllowedThreatAt(plannedCell, strength, refSqDist)))
	{
		return false;
	}
	outCell = plannedCell;
	return true;
}

bool CAttackTask::GetFireArcBearing(float& outBearing) const
{
	if (target == nullptr) {
		return false;
	}
	const AIFloat3& tPos = target->GetPos();
	const float keep = IsInBaseRadius(tPos) ? 1.f : GetMilitaryMgr()->GetAttackKeepThreat();
	AIFloat3 cell;
	if (!GetPlannedCell(target, GetPickStrength(manager->GetCircuit()->GetLastFrame()) * keep, scaleRefSqDist, cell)) {
		return false;
	}
	const AIFloat3 d = cell - tPos;
	if (d.Length2D() < 1.f) {
		return false;
	}
	outBearing = atan2f(d.z, d.x);
	return true;
}

bool CAttackTask::IsStaticBanned(ICoreUnit::Id id, int frame) const
{
	auto it = staticBans.find(id);
	return (it != staticBans.end()) && (frame < it->second.until);
}

void CAttackTask::BanStatic(const CEnemyInfo* enemy, int frame)
{
	const float threat = manager->GetCircuit()->GetThreatMap()->GetThreatAt(enemy->GetPos());
	auto it = staticBans.find(enemy->GetId());
	if (it == staticBans.end()) {
		staticBans[enemy->GetId()] = {frame + REJECTED_TARGET_FRAMES, frame, threat, 0};
		return;
	}
	SStaticBan& ban = it->second;  // banned again with nothing changed: longer
	ban.level = std::min(ban.level + 1, STATIC_BAN_MAX_LEVEL);
	ban.until = frame + (REJECTED_TARGET_FRAMES << ban.level);
	ban.threat = threat;
}

void CAttackTask::UpdateStaticBans(int frame)
{
	CCircuitAI* circuit = manager->GetCircuit();
	CThreatMap* threatMap = circuit->GetThreatMap();
	for (auto it = staticBans.begin(); it != staticBans.end();) {
		const SStaticBan& ban = it->second;
		const CEnemyInfo* enemy = circuit->GetEnemyInfo(it->first);
		bool isDrop = (enemy == nullptr) || (frame >= ban.since + STATIC_BAN_RESET_FRAMES);
		if (!isDrop) {
			const float threat = threatMap->GetThreatAt(enemy->GetPos());
			isDrop = fabsf(threat - ban.threat) > STATIC_BAN_THREAT_CHANGE * std::max(ban.threat, THREAT_MIN);
		}
		it = isDrop ? staticBans.erase(it) : std::next(it);
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
	F3Vec goalPos;
	goalPos.reserve(goals.size());
	bool isWedge = false;
	for (const STargetGoal& g : goals) {
		goalPos.push_back((g.sector >= 0) ? g.cell : g.pos);
		isWedge |= (g.sector >= 0);
	}
	std::vector<CQueryPathMulti::SGoalShape> shapes;
	if (isWedge) {
		const float halfAngle = static_cast<float>(M_PI) / GetMilitaryMgr()->GetAttackStaticCells().sectors;
		shapes.reserve(goals.size());
		for (const STargetGoal& g : goals) {
			if (g.sector >= 0) {
				shapes.push_back({g.pos, pathRange, atan2f(g.cell.z - g.pos.z, g.cell.x - g.pos.x), halfAngle});
			} else {
				shapes.push_back({g.pos, 0.f, 0.f, -1.f});
			}
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

	if (path->posPath.empty()) {
		// Firing cells' wedges may be cut off where a disc around the target is not: route once more with those
		// targets as discs (as without quota.attack_static_cells), counted against MAX_TARGET_ROUTES
		const bool isWedge = std::any_of(goals.begin(), goals.end(), [](const STargetGoal& g) { return g.sector >= 0; });
		if (isWedge && (++targetRouteTries < MAX_TARGET_ROUTES)) {
			std::vector<STargetGoal> discs;
			for (const STargetGoal& g : goals) {
				if ((circuit->GetEnemyInfo(g.id) == nullptr) || std::any_of(discs.begin(), discs.end(),
					[&g](const STargetGoal& d) { return d.id == g.id; }))
				{
					continue;
				}
				discs.push_back({g.id, g.pos, g.key});
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

	// The goal the route reached: a firing cell's wedge that holds the route's end (the one whose cell is nearest the
	// end), else the first disc in range of the end (nearest first in the list, as FindTarget sorted it); if none
	// holds it (moved, corrected), the goal it ends nearest to
	const AIFloat3& endPos = path->posPath.back();
	const float sqSize = circuit->GetPathfinder()->GetSquareSize();
	const float reach = query->GetMaxRange() + sqSize;
	const float sqInRange = SQUARE(reach);
	const float cosHalf = cosf(static_cast<float>(M_PI) / GetMilitaryMgr()->GetAttackStaticCells().sectors);
	const STargetGoal* wedgeHit = nullptr;
	const STargetGoal* discHit = nullptr;
	const STargetGoal* nearest = nullptr;
	float minCellSqDist = std::numeric_limits<float>::max();
	float minSqDist = std::numeric_limits<float>::max();
	for (const STargetGoal& g : goals) {
		if (circuit->GetEnemyInfo(g.id) == nullptr) {  // may have died since
			continue;
		}
		if (g.sector >= 0) {
			AIFloat3 toEnd = endPos - g.pos;
			AIFloat3 toCell = g.cell - g.pos;
			const float endLen = toEnd.Length2D();
			const float cellLen = toCell.Length2D();
			const bool isIn = (endLen <= reach) && ((endLen < sqSize) || (cellLen < 1.f)
					|| (toEnd.dot2D(toCell) >= endLen * cellLen * cosHalf));
			const float sqCellDist = g.cell.SqDistance2D(endPos);
			if (isIn && (sqCellDist < minCellSqDist)) {
				minCellSqDist = sqCellDist;
				wedgeHit = &g;
			}
			if (sqCellDist < minSqDist) {
				minSqDist = sqCellDist;
				nearest = &g;
			}
		} else {
			const float sqDist = g.pos.SqDistance2D(endPos);
			if ((discHit == nullptr) && (sqDist <= sqInRange)) {
				discHit = &g;
			}
			if (sqDist < minSqDist) {
				minSqDist = sqDist;
				nearest = &g;
			}
		}
	}
	const STargetGoal* hit = (wedgeHit != nullptr) ? wedgeHit : (discHit != nullptr) ? discHit : nearest;
	CEnemyInfo* reached = (hit != nullptr) ? circuit->GetEnemyInfo(hit->id) : nullptr;
	// A cell route only if the route's end lies in the cell's wedge: a route that ends outside every goal (cut
	// short, target corrected) gets today's capped target-to-end check instead of an uncapped line to a far cell.
	// Copied: routing again may release the query, and the goals with it
	const bool isCell = (hit != nullptr) && (hit == wedgeHit);
	const int cellSector = isCell ? hit->sector : -1;
	const AIFloat3 cellPos = isCell ? hit->cell : AIFloat3(-RgtVector);
	const AIFloat3* cell = isCell ? &cellPos : nullptr;
	if (reached != nullptr) {
		// Its approach on this route must be safe (same strength as picking), judged with the near-base reference
		// it would get: its own if it is the anchored target, else the one from where the group stood at pick time.
		// If not, route again to the other candidates, up to MAX_TARGET_ROUTES routes per decision
		const int frame = circuit->GetLastFrame();
		const bool isAnchored = (reached->GetId() == anchorTargetId);
		const AIFloat3& basePos = circuit->GetSetupManager()->GetBasePos();
		const float ref = isAnchored ? scaleRefSqDist
				: ((reached->GetPos().SqDistance2D(basePos) < pickSqDist) ? pickSqDist : 0.f);
		if (!IsApproachSafe(path->posPath, reached->GetPos(), GetPickStrength(frame), ref, cell)) {
			const ICoreUnit::Id reachedId = reached->GetId();
			// A firing cell: drop only that side for this decision; the whole target only once it has no other cell
			if (isCell) {
				rejectedCells.push_back({reachedId, cellSector});
			}
			auto isCellRejected = [this](const STargetGoal& g) {
				return (g.sector >= 0) && std::any_of(rejectedCells.begin(), rejectedCells.end(),
						[&g](const std::pair<ICoreUnit::Id, int>& rc) { return (rc.first == g.id) && (rc.second == g.sector); });
			};
			const bool hasOtherCell = isCell && std::any_of(screenedGoals.begin(), screenedGoals.end(),
					[&](const STargetGoal& g) { return (g.id == reachedId) && (g.sector >= 0) && !isCellRejected(g); });
			if (!hasOtherCell) {
				rejectedUntil[reachedId] = frame + REJECTED_TARGET_FRAMES;
			}
			// Not our target any more, while the next route runs (nor counted against its group limit)
			SetTarget(nullptr);
			// Next route: every screened candidate of the last decision, not just this route's goals (when the
			// current target was kept, that was only it), minus rejected (targets and cells) and dead
			std::vector<STargetGoal> rest;
			for (const STargetGoal& g : screenedGoals) {
				auto it = rejectedUntil.find(g.id);
				if (((it == rejectedUntil.end()) || (it->second <= frame)) && !isCellRejected(g)
					&& (circuit->GetEnemyInfo(g.id) != nullptr))
				{
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
		position = reached->GetPos();
		// The side we'll fight it from (or none: a disc route, fight from wherever we arrive)
		plannedTargetId = isCell ? reached->GetId() : -1;
		plannedCell = cellPos;
		plannedFrame = frame;
	}
	pPath = path;
	ActivePath(lowestSpeed);
	if (reached != nullptr) {
		KeepRoute(reached->GetId(), reached->GetPos(), cell);
	}
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
	if (isReuse && ReuseRoute(-1, goal, frame)) {
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
		KeepRoute(-1, goal);
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

void CAttackTask::KeepRoute(ICoreUnit::Id targetId, const AIFloat3& goal, const AIFloat3* cell)
{
	lastRoute = {pPath, goal, targetId, manager->GetCircuit()->GetLastFrame(),
			(cell != nullptr) ? STATIC_ROUTE_REUSE_FRAMES : ROUTE_REUSE_FRAMES,
			(cell != nullptr) ? *cell : AIFloat3(-RgtVector), cell != nullptr};
}

bool CAttackTask::ReuseRoute(ICoreUnit::Id targetId, const AIFloat3& goal, int frame)
{
	// Same goal: the same target (or front) that moved less than one path square. The units pulled back from
	// danger would wait for a new route, so not while any unit is in danger
	const float sqMoved = SQUARE(manager->GetCircuit()->GetPathfinder()->GetSquareSize());
	if ((lastRoute.path == nullptr) || lastRoute.path->posPath.empty() || (lastRoute.targetId != targetId)
		|| (frame >= lastRoute.frame + lastRoute.reuseFrames)
		|| (goal.SqDistance2D(lastRoute.goal) >= sqMoved)
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
		const AIFloat3* cell = lastRoute.isCell ? &lastRoute.cell : nullptr;
		if (!IsApproachSafe(lastRoute.path->posPath, goal, strength, scaleRefSqDist, cell)) {
			return false;  // the new route's ApplyTargetPath rejects it, or finds a safe approach
		}
		// A firing cell must still be below the allowance (the current target's, quota.attack_keep_threat)
		if ((cell != nullptr) && (manager->GetCircuit()->GetThreatMap()->GetThreatAt(leader, *cell)
			>= GetAllowedThreatAt(*cell, strength * (IsInBaseRadius(goal) ? 1.f : GetMilitaryMgr()->GetAttackKeepThreat()),
					scaleRefSqDist)))
		{
			return false;
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
		if ((unit->Blocker() == nullptr) && !unit->GetTravelAct()->IsActive() && IsOutOfReach(unit, frame)) {
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

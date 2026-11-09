/*
 * RaidTask.cpp
 *
 *  Created on: Jan 6, 2016
 *      Author: rlcevg
 */

#include "task/fighter/RaidTask.h"
#include "task/TaskManager.h"
#include "map/InfluenceMap.h"
#include "map/ThreatMap.h"
#include "map/MapManager.h"
#include "module/FactoryManager.h"
#include "module/MilitaryManager.h"
#include "setup/SetupManager.h"
#include "terrain/TerrainManager.h"
#include "terrain/path/PathFinder.h"
#include "terrain/path/QueryPathSingle.h"
#include "terrain/path/QueryPathMulti.h"
#include "unit/action/MoveAction.h"
#include "unit/action/FightAction.h"
#include "unit/enemy/EnemyUnit.h"
#include "unit/CircuitUnit.h"
#include "CircuitAI.h"
#include "util/Utils.h"

#include "spring/SpringMap.h"

#include <algorithm>

#include "AISCommands.h"

#define MOVING_SPEED	0.5f  // elmos per frame: a target faster than this counts as moving (no firing spots)
// quota.raid_rally joiners
#define CHASE_TRAVEL_SEC	30.f  // chase a bigger group only within this many seconds of straight travel...
#define CHASE_KEEP_SEC	36.f  // ...and keep chasing it out to this (no flip-flop at the edge)
#define CHASE_PATH_RANGE	300.f  // the chase route ends this near the group's leader (merging needs 1000)
#define CHASE_ROUTE_FRAMES	(FRAMES_PER_SEC * 6)  // a chase route is kept this long...
#define CHASE_ROUTE_SLACK	0.1f  // ...while the chased leader moved less than this share of the way...
#define CHASE_ROUTE_SLACK_MIN	200.f  // ...or than this many elmos (near the end 10% is a few elmos)
#define CHASE_FAILS	3  // empty chase routes in a row before...
#define CHASE_RETRY_FRAMES	(FRAMES_PER_SEC * 15)  // ...raiding normally for this long
#define CHASE_NEAR_MOVE	100.f  // beside the chased group: the Fight order is re-sent once its leader moved this far...
#define CHASE_NEAR_FRAMES	(FRAMES_PER_SEC * 3)  // ...or this long after the last one

namespace circuit {

using namespace springai;

CRaidTask::CRaidTask(ITaskManager* mgr, float maxPower, float powerMod)
		: ISquadTask(mgr, FightType::RAID, powerMod)
		, chaseTask(nullptr)
		, chaseFails(0)
		, chaseRetryFrame(-FRAMES_PER_SEC * 60)
		, isChaseSkip(false)
		, chaseRouteFrame(-FRAMES_PER_SEC * 60)
		, chaseGoal(-RgtVector)
		, isChaseDest(false)
		, nearFightFrame(-FRAMES_PER_SEC * 60)
		, nearFightPos(-RgtVector)
		, maxPower(maxPower)
		, fleeFrame(0)
		, fleePathFrame(0)
		, fleeQueryId(-1)
		, fleeDest(-RgtVector)
		, isFightPending(false)
		, isContestedRoute(false)
		, contestedFailFrame(-FRAMES_PER_SEC * 60)
		, isContestedDest(false)
		, raidSpot(-RgtVector)
		, raidSpotPos(-RgtVector)
		, raidSpotFailFrame(-FRAMES_PER_SEC * 60)
		, roamPathFrame(-FRAMES_PER_SEC * 60)
		, roamGoal(-RgtVector)
{
	CCircuitAI* circuit = manager->GetCircuit();
	float x = rand() % circuit->GetTerrainManager()->GetTerrainWidth();
	float z = rand() % circuit->GetTerrainManager()->GetTerrainHeight();
	position = AIFloat3(x, circuit->GetMap()->GetElevationAt(x, z), z);
}

CRaidTask::~CRaidTask()
{
}

CMilitaryManager* CRaidTask::GetMilitaryMgr() const
{
	return static_cast<CMilitaryManager*>(manager);
}

bool CRaidTask::CanAssignTo(CCircuitUnit* unit) const
{
	// Distance first: during a raid rally every group checks every other one every 2 updates, and most are far
	const int frame = manager->GetCircuit()->GetLastFrame();
	if (leader->GetPos(frame).SqDistance2D(unit->GetPos(frame)) > SQUARE(1000.f)) {
		return false;
	}
	CCircuitDef* udef = unit->GetCircuitDef();
	if (!udef->IsRoleRaider()) {
		return false;
	}
	// Our type only; during a raid rally (quota.raid_rally) any raider that moves like us. A group of our type that
	// is mixed since a rally must still have all its speeds within the ratio of ours
	if ((udef != leader->GetCircuitDef())
		? ((GetMilitaryMgr()->GetRaidRallyHost() == nullptr) || !IsRallyCompatible(unit))
		: !IsSpeedSpreadOk(unit))
	{
		return false;
	}
	return HasRoom();
}

bool CRaidTask::IsRallyCompatible(CCircuitUnit* unit) const
{
	const CCircuitDef* udef = unit->GetCircuitDef();
	const float speedRatio = GetMilitaryMgr()->GetAttackMerge().speedRatio;  // quota.attack_merge.speed_ratio
	if (!udef->IsRoleRaider() || udef->IsAbleToFly() || !IsMoveCompatible(unit, speedRatio)) {
		return false;
	}
	return IsSpeedSpreadOk(unit);
}

bool CRaidTask::IsSpeedSpreadOk(CCircuitUnit* unit) const
{
	// Leading a raid group: all its units' speeds and ours within the ratio, not only the leaders' (merges in a chain
	// would otherwise widen the spread a step at a time). Groups of one type always pass
	const CCircuitDef* udef = unit->GetCircuitDef();
	const float speedRatio = GetMilitaryMgr()->GetAttackMerge().speedRatio;  // quota.attack_merge.speed_ratio
	float slowest = udef->GetSpeed();
	float fastest = slowest;
	IUnitTask* task = unit->GetTask();
	if ((task != nullptr) && (task->GetType() == IUnitTask::Type::FIGHTER)
		&& (static_cast<IFighterTask*>(task)->GetFightType() == FightType::RAID))
	{
		const CRaidTask* group = static_cast<const CRaidTask*>(task);
		if (group->GetLeader() == unit) {
			slowest = group->lowestSpeed;
			fastest = group->highestSpeed;
		}
	}
	return std::max(fastest, highestSpeed) <= std::min(slowest, lowestSpeed) * speedRatio;
}

bool CRaidTask::HasRoom() const
{
	if (attackPower <= maxPower) {
		return true;
	}
	// A raid rally lets the group grow until its strength reaches the rally's goal, the measure the rally ends on
	const float goal = GetMilitaryMgr()->GetRaidRallyGoal();  // 0 without a rally
	return GetRallyStrength() * GetCountPowerMod() < goal;
}

bool CRaidTask::IsInRally() const
{
	const CMilitaryManager* militaryMgr = GetMilitaryMgr();
	const CRaidTask* host = militaryMgr->GetRaidRallyHost();
	return (host != nullptr) && ((host == this) || militaryMgr->IsRaidRallyJoiner(this));
}

ISquadTask* CRaidTask::GetRaidMergeTask()
{
	// GetMergeTask runs on updates where updCount % 32 == 1, which are among these
	if ((updCount % 2 == 1) && IsInRally()) {
		return CheckMergeTask();
	}
	return GetMergeTask();
}

bool CRaidTask::IsSafeMergeLine(const ISquadTask* host) const
{
	if (!IsInRally()) {
		return ISquadTask::IsSafeMergeLine(host);
	}
	const int frame = manager->GetCircuit()->GetLastFrame();
	const float safeThreat = GetSafeThreat();
	return IsLineSafe(leader->GetPos(frame), host->GetLeaderPos(frame), std::numeric_limits<float>::max(),
			[this, safeThreat](const AIFloat3& p) { return GetSafeThreatAt(p, safeThreat); });
}

float CRaidTask::GetPathThreatMod() const
{
	return IsCautious(manager->GetCircuit()->GetLastFrame()) ? 0.75f : 1.f;
}

void CRaidTask::AssignTo(CCircuitUnit* unit)
{
	ISquadTask::AssignTo(unit);
	CCircuitDef* cdef = unit->GetCircuitDef();
	highestRange = std::max(highestRange, cdef->GetLosRadius());
	highestRange = std::max(highestRange, cdef->GetJumpRange());

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
}

void CRaidTask::RemoveAssignee(CCircuitUnit* unit)
{
	ISquadTask::RemoveAssignee(unit);
	if (leader == nullptr) {
		manager->AbortTask(this);
	} else {
		highestRange = std::max(highestRange, leader->GetCircuitDef()->GetLosRadius());
		highestRange = std::max(highestRange, leader->GetCircuitDef()->GetJumpRange());
	}
}

void CRaidTask::Start(CCircuitUnit* unit)
{
	if ((State::REGROUP == state) || (State::ENGAGE == state)) {
		return;
	}
	if (!pPath->posPath.empty()) {
		unit->GetTravelAct()->SetPath(pPath);
		unit->GetTravelAct()->StateActivate();
	}
}

void CRaidTask::Update()
{
	++updCount;

	/*
	 * Merge tasks if possible
	 */
	ISquadTask* task = GetRaidMergeTask();
	if (task != nullptr) {
		task->Merge(this);
		units.clear();
		manager->AbortTask(this);
		return;
	}

	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();

	/*
	 * Escape: checked on every update (not just every decision), also while engaging or regrouping
	 */
	const bool isInDanger = IsInDanger(frame, static_cast<CMilitaryManager*>(manager)->GetRaidFleeShare());
	if (isInDanger && Flee(frame)) {
		return;
	}
	UpdateScoots(frame);  // reload_scoot: step out of reach right after a shot
	if (isFightPending) {
		SendPendingFight(frame);
	}
	PullBackEndangered(frame);
	if (frame < fleeFrame) {
		return;  // keep running along the escape path for a moment instead of turning straight back
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
			const int timeout = frame + FRAMES_PER_SEC * 60;
			for (CCircuitUnit* unit : units) {
				const AIFloat3& pos = utils::get_radial_pos(groupPos, SQUARE_SIZE * 8);
				TRY_UNIT(circuit, unit,
					unit->CmdMoveTo(groupPos, UNIT_COMMAND_OPTION_RIGHT_MOUSE_KEY, timeout);
					unit->GetUnit()->PatrolTo(pos, UNIT_COMMAND_OPTION_RIGHT_MOUSE_KEY | UNIT_COMMAND_OPTION_SHIFT_KEY, timeout);
				)
				unit->GetTravelAct()->StateWait();
			}
		}
		return;
	}

	// Decide about once a second (was every 2nd update and at least 1s), sooner when the path ahead turned dangerous
	bool isExecute = (frame >= lastTouched + FRAMES_PER_SEC)
		|| (isPathAheadUnsafe && (frame >= lastTouched + FRAMES_PER_SEC / 2));
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

	/*
	 * Update target
	 */
	const bool isTargetsFound = FindTarget();
	// Raid rally joiner: with no target in range (FindTarget picks only those) and no enemy at our base to defend,
	// head for the bigger group
	if ((target == nullptr) && urgentPositions.empty()) {
		const CRaidTask* chase = UpdateChase(frame);
		if (chase != nullptr) {
			state = State::ROAM;
			Chase(frame, chase);
			return;
		}
	}
	nearFightPos = -RgtVector;  // not staying beside a chased group: other orders follow

	state = State::ROAM;
	if (target != nullptr) {
		state = State::ENGAGE;
		position = target->GetPos();
		if (leader->GetCircuitDef()->IsAbleToFly()) {
			if (target->GetUnit()->IsCloaked()) {
				for (CCircuitUnit* unit : units) {
					if (unit->Blocker() != nullptr) {
						continue;  // Do not interrupt current action
					}
					unit->GetTravelAct()->StateWait();

					const AIFloat3& pos = target->GetPos();
					TRY_UNIT(circuit, unit,
						unit->CmdAttackGround(pos, UNIT_COMMAND_OPTION_RIGHT_MOUSE_KEY, frame + FRAMES_PER_SEC * 60);
					)
				}
			} else {
				for (CCircuitUnit* unit : units) {
					if (unit->Blocker() != nullptr) {
						continue;  // Do not interrupt current action
					}
					unit->GetTravelAct()->StateWait();

					TRY_UNIT(circuit, unit,
						unit->GetUnit()->Attack(target->GetUnit(), UNIT_COMMAND_OPTION_RIGHT_MOUSE_KEY, frame + FRAMES_PER_SEC * 60);
						unit->CmdSetTarget(target);
					)
				}
			}
		} else {
			const AIFloat3& tVel = target->GetVel();
			if (tVel.SqLength2D() > SQUARE(MOVING_SPEED)) {
				// Moving target (fleeing builder etc.): a firing spot goes stale at once, chase it directly
				AttackDirect(frame);
			} else {
				// Firing spot per unit (in range, close, low threat), plain move there then Attack: keeps FindTarget's priority
				Attack(frame, true);
				if (IsAllHolding()) {
					// Every ranged unit holds (no safe spot in range) while the target is out of reach: the group would
					// stand still for as long as FindTarget keeps choosing it. Pass on it for a while instead.
					heldTargets[target->GetId()] = frame;
				}
			}
		}
		return;
	}

	if (!IsQueryReady(leader)) {
		return;
	}

	if (!isTargetsFound) {  // urgentPositions.empty() && enemyPositions.empty()
		FallbackRaid();
		return;
	}

	CCircuitDef* cdef = leader->GetCircuitDef();
	CThreatMap* threatMap = circuit->GetThreatMap();
	const AIFloat3& startPos = leader->GetPos(frame);
	const float pathRange = std::max(std::min(cdef->GetMaxRange(), cdef->GetLosRadius()), (float)threatMap->GetSquareSize());

	// Uniform cap along the path: the strictest thr_mod.distance value of start and goals
	const F3Vec& goals = !urgentPositions.empty() ? urgentPositions : enemyPositions;
	float pathMod = GetThrDistModAt(startPos);
	for (const AIFloat3& g : goals) {
		pathMod = std::max(pathMod, GetThrDistModAt(g));
	}
	CPathFinder* pathfinder = circuit->GetPathfinder();
	std::shared_ptr<IPathQuery> query = pathfinder->CreatePathMultiQuery(
			leader, threatMap, frame,
			startPos, pathRange, goals, GetHitTest(), GetSafeThreat() / pathMod);
	pathQueries[leader] = query;
	query->HoldTask(this);

	pathfinder->RunQuery(query, [this](const IPathQuery* query) {
		if (this->IsQueryAlive(query)) {
			this->ApplyTargetPath(static_cast<const CQueryPathMulti*>(query));
		}
	});
}

void CRaidTask::OnUnitIdle(CCircuitUnit* unit)
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
		isContestedDest = false;
		raidSpot = -RgtVector;  // arrived: quota.raider_spot (if on) picks the next spot instead of this random point
	}

	if (units.find(unit) != units.end()) {
		Start(unit);  // NOTE: Not sure if it has effect
	}
}

bool CRaidTask::FindTarget()
{
	CCircuitAI* circuit = manager->GetCircuit();
	CMap* map = circuit->GetMap();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	CThreatMap* threatMap = circuit->GetThreatMap();
	CInfluenceMap* inflMap = circuit->GetInflMap();
	const AIFloat3& pos = leader->GetPos(circuit->GetLastFrame());
	STerrainMapArea* area = leader->GetArea();
	CCircuitDef* cdef = leader->GetCircuitDef();
	const bool isAntiStatic = cdef->IsAttrAntiStat();
	const bool notAW = !cdef->HasAntiWater();
	const bool notAA = !cdef->HasAntiAir();
	const float sqMaxSpeed = SQUARE(highestSpeed * CHASE_SPEED_MOD / FRAMES_PER_SEC);
	// A runner that outranges us is dropped at a lower speed, near base too: we would only catch it inside its range
	const float sqOutrangeSpeed = SQUARE(highestSpeed * OUTRANGE_CHASE_SPEED_MOD / FRAMES_PER_SEC);
	// Current health, same measure as the escape check; for 4s after an escape demand a 25% margin,
	// so the group doesn't pick a target at the edge of the danger it just left and bounce back and forth
	const float maxPower = GetSafeThreat() * (IsCautious(circuit->GetLastFrame()) ? 0.75f : 1.f);
	const float weaponRange = cdef->GetMaxRange();
	const int canTargetCat = cdef->GetTargetCategory();
	const int noChaseCat = cdef->GetNoChaseCategory();
	const float range = std::max(leader->GetUnit()->GetMaxRange() + threatMap->GetSquareSize() * 2,
								 cdef->GetLosRadius());
	float minSqDist = SQUARE(range);
	float maxThreat = 0.f;
	float minPower = maxPower;

	const AIFloat3& basePos = circuit->GetSetupManager()->GetBasePos();
	const float baseRange = circuit->GetMilitaryManager()->GetBaseDefRange();
	const float sqBaseRange = SQUARE(baseRange);
	const bool isDefender = basePos.SqDistance2D(pos) < sqBaseRange;

	SetTarget(nullptr);  // make adequate enemy->GetTasks().size()
	CMilitaryManager* militaryMgr = circuit->GetMilitaryManager();
	CEnemyInfo* bestTarget = nullptr;
	CEnemyInfo* worstTarget = nullptr;
	urgentPositions.clear();
	enemyPositions.clear();
	threatMap->SetThreatType(leader);  // shared threat-map state: set on every pick
	// Contested clusters (threat there blocks our mex expansion) that this group can take: an enemy there counts
	// quota.contested.bonus times closer as a route goal
	static std::vector<CMilitaryManager::SContestedSpot> contested;  // NOTE: micro-opt
	militaryMgr->GetTakeableContested(leader, highestRange,
			[this, maxPower](const AIFloat3& p) { return GetSafeThreatAt(p, maxPower); }, contested);
	const float contestedBonus = militaryMgr->GetContested().bonus;
	F3Vec contestedPositions;
	float sqContestedDist = std::numeric_limits<float>::max();
	float sqOtherDist = std::numeric_limits<float>::max();
	const CCircuitAI::EnemyInfos& enemies = circuit->GetEnemyInfos();
	for (auto& kv : enemies) {
		CEnemyInfo* enemy = kv.second;
		if (enemy->IsHidden() || (enemy->GetTasks().size() > 2)) {
			continue;
		}
		auto itHeld = heldTargets.find(enemy->GetId());
		if (itHeld != heldTargets.end()) {
			if (circuit->GetLastFrame() < itHeld->second + FRAMES_PER_SEC * 10) {
				continue;  // no safe firing spot last time: pass on it for 10 s
			}
			heldTargets.erase(itHeld);
		}

		const AIFloat3& ePos = enemy->GetPos();
		const bool isEnemyUrgent = isDefender && (inflMap->GetAllyDefendInflAt(ePos) > INFL_EPS);
		if ((!isEnemyUrgent && !urgentPositions.empty())
			|| !terrainMgr->CanMobileReachAt(area, ePos, highestRange))
		{
			continue;
		}

		const float sqEBDist = basePos.SqDistance2D(ePos);
		const float distMod = GetThrDistModAt(ePos);  // thr_mod.distance curve at the enemy's spot
		float checkPower = maxPower / distMod;
		float sqCheckSpeed = sqMaxSpeed;
		if (sqEBDist < sqBaseRange) {
			checkPower *= circuit->GetMilitaryManager()->GetBaseThrBonus(sqEBDist);  // quota.thr_mod.base_bonus near base
			sqCheckSpeed *= 2.f;
		}
		const float power = GetTaskThreatAt(leader, ePos);
		if (checkPower <= power) {
			continue;
		}
		// The near-base doubling (sqCheckSpeed) is not for a runner that outranges us
		if (IsRunningAway(enemy, pos, sqCheckSpeed, sqOutrangeSpeed, highestRange)) {
			continue;
		}

		int targetCat;
		float defThreat;
		bool isBuilder;
		CCircuitDef* edef = enemy->GetCircuitDef();
		if (edef != nullptr) {
			if (isAntiStatic && edef->IsMobile()) {
				continue;
			}
			targetCat = edef->GetCategory();
			if (((targetCat & canTargetCat) == 0)
				|| (edef->IsAbleToFly() && notAA))
			{
				continue;
			}
			float elevation = map->GetElevationAt(ePos.x, ePos.z);
			if ((notAW && !edef->IsYTargetable(elevation, ePos.y))
				|| (ePos.y - elevation > weaponRange))
			{
				continue;
			}
			defThreat = edef->GetPower();
			isBuilder = militaryMgr->IsRaidPreferred(edef);  // configurable preferred target
		} else {
			if (notAW && (ePos.y < -SQUARE_SIZE * 5)) {
				continue;
			}
			targetCat = UNKNOWN_CATEGORY;
			defThreat = enemy->GetThreat();
			isBuilder = false;
		}

		float sqDist = pos.SqDistance2D(ePos);
		if ((minPower / distMod > power) && (minSqDist > sqDist)) {
			if (enemy->IsInRadarOrLOS()) {
				if (((targetCat & noChaseCat) == 0) && !enemy->IsBeingBuilt()) {
					if (isBuilder) {
						bestTarget = enemy;
						minSqDist = sqDist;
						maxThreat = std::numeric_limits<float>::max();
					} else if (maxThreat <= defThreat) {
						bestTarget = enemy;
//						minSqDist = sqDist;
						maxThreat = defThreat;
					}
//					minPower = power;
				} else if (bestTarget == nullptr) {
					worstTarget = enemy;
				}
			}
			continue;
		}

		if (isEnemyUrgent) {
			urgentPositions.push_back(ePos);
		} else {
			enemyPositions.push_back(ePos);
			if (CMilitaryManager::IsInContested(contested, ePos)) {
				contestedPositions.push_back(ePos);
				sqContestedDist = std::min(sqContestedDist, sqDist);
			} else {
				sqOtherDist = std::min(sqOtherDist, sqDist);
			}
		}
	}
	if (bestTarget == nullptr) {
		bestTarget = worstTarget;
	}
	// The route goes to the nearest goal it reaches: route only to contested enemies when the nearest of them,
	// counted quota.contested.bonus times closer, is at least as near as the nearest other enemy.
	// Not for 10 s after such a route came back empty: then all goals again
	isContestedRoute = urgentPositions.empty() && !contestedPositions.empty()
			&& (sqContestedDist <= sqOtherDist * SQUARE(contestedBonus))
			&& (circuit->GetLastFrame() >= contestedFailFrame + FRAMES_PER_SEC * 10);
	if (isContestedRoute) {
		enemyPositions.swap(contestedPositions);
	}

	if (bestTarget != nullptr) {
		SetTarget(bestTarget);
		return true;
	}

	if (urgentPositions.empty() && enemyPositions.empty()) {
		return false;
	}

	return true;
	// Return: target, startPos=leader->pos, urgentPositions and enemyPositions
}

void CRaidTask::ApplyTargetPath(const CQueryPathMulti* query)
{
	pPath = query->GetPathInfo();

	if (!pPath->posPath.empty()) {
		position = pPath->posPath.back();
		ActivePath();
	} else {
		if (isContestedRoute) {
			contestedFailFrame = manager->GetCircuit()->GetLastFrame();
		}
		// Not to a contested cluster: the capped route just failed, and the roam path has no cap
		FallbackRaid(false);
	}
}

void CRaidTask::FallbackRaid(bool isContestedAllowed)
{
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	CThreatMap* threatMap = circuit->GetThreatMap();
	const AIFloat3& pos = leader->GetPos(frame);
	const AIFloat3& threatPos = leader->GetTravelAct()->IsActive() ? position : pos;
	const float safeThreat = GetSafeThreat();
	// Nothing to hunt: go to the nearest contested cluster we can take (its threat blocks our mex expansion),
	// unless we're already there; else the usual mex-spot roaming
	CMilitaryManager* militaryMgr = circuit->GetMilitaryManager();
	static std::vector<CMilitaryManager::SContestedSpot> contested;  // NOTE: micro-opt
	contested.clear();
	threatMap->SetThreatType(leader);  // shared threat-map state: set on every pick
	if (isContestedAllowed) {
		const float cautionMod = IsCautious(frame) ? 0.75f : 1.f;  // same as FindTarget
		militaryMgr->GetTakeableContested(leader, highestRange,
				[this, safeThreat, cautionMod](const AIFloat3& p) { return GetSafeThreatAt(p, safeThreat * cautionMod); },
				contested);
	}
	const CMilitaryManager::SContestedSpot* bestContested = nullptr;
	float sqBestDist = std::numeric_limits<float>::max();
	for (const CMilitaryManager::SContestedSpot& c : contested) {
		const float sqDist = pos.SqDistance2D(c.pos);
		if ((sqDist > c.sqRadius) && (sqDist < sqBestDist)) {
			sqBestDist = sqDist;
			bestContested = &c;
		}
	}
	// A contested destination from before is dropped too when contested ones aren't allowed now, and so is a raid
	// rally chase goal (where a group's leader was)
	const bool mustRepick = (!isContestedAllowed && isContestedDest) || isChaseDest;
	isChaseDest = false;
	isContestedDest = (bestContested != nullptr);
	const bool isDanger = (safeThreat / GetThrDistModAt(threatPos) <= GetTaskThreatAt(leader, threatPos));
	// quota.raider_spot: roam between scored spots instead of the shared mex list
	bool isSpotDest = false;
	bool isSpotDone = false;
	if ((bestContested == nullptr) && militaryMgr->IsRaidSpotOn()) {
		isSpotDest = UpdateRaidSpot(safeThreat, mustRepick, isDanger, isSpotDone);
	} else {
		raidSpot = -RgtVector;
	}
	if (bestContested != nullptr) {
		position = terrainMgr->GetMovePosition(leader->GetArea(), bestContested->pos);
	} else if (isSpotDest) {
		// heading to a scored spot (UpdateRaidSpot)
	} else if (mustRepick || isDanger || isSpotDone) {
		// Try a few candidates and keep the first one that passes the same check. Taking the next spot
		// unchecked made raids hop to a new (often equally dangerous, far = strict curve) spot every update.
		AIFloat3 candidate = -RgtVector;
		for (int i = 0; i < 8; ++i) {
			candidate = militaryMgr->GetRaidPosition(leader);
			if (!utils::is_valid(candidate)
				|| (safeThreat / GetThrDistModAt(candidate) > GetTaskThreatAt(leader, candidate)))
			{
				break;
			}
			candidate = -RgtVector;  // all tried spots dangerous: fall through to a random point
		}
		position = candidate;
	}

	if (!utils::is_valid(position)) {
		float x = rand() % terrainMgr->GetTerrainWidth();
		float z = rand() % terrainMgr->GetTerrainHeight();
		position = AIFloat3(x, circuit->GetMap()->GetElevationAt(x, z), z);
		position = terrainMgr->GetMovePosition(leader->GetArea(), position);
	}

	// Still walking the roam route to this same destination, planned under 3 s ago, with no danger at the
	// destination, on the stretch ahead or to any unit: keep it instead of planning the same route again
	if (!isDanger && !isPathAheadUnsafe && endangered.empty() && (position.SqDistance2D(roamGoal) < 1.f)
		&& (frame < roamPathFrame + FRAMES_PER_SEC * 3) && (roamPath != nullptr)
		&& leader->GetTravelAct()->IsActive() && (leader->GetTravelAct()->GetPath() == roamPath))
	{
		pPath = roamPath;  // a failed target route may have replaced it, and Start() and regrouping re-send pPath
		return;
	}
	roamGoal = position;
	roamPathFrame = frame;
	roamPath = nullptr;  // set again when this route arrives: an older route must not pass for it

	CPathFinder* pathfinder = circuit->GetPathfinder();
	std::shared_ptr<IPathQuery> query = pathfinder->CreatePathSingleQuery(
			leader, threatMap, frame,
			pos, position, pathfinder->GetSquareSize());
	pathQueries[leader] = query;
	query->HoldTask(this);

	pathfinder->RunQuery(query, [this](const IPathQuery* query) {
		if (this->IsQueryAlive(query)) {
			this->ApplyRaidPath(static_cast<const CQueryPathSingle*>(query));
		}
	});
}

bool CRaidTask::UpdateRaidSpot(float safeThreat, bool mustRepick, bool isDanger, bool& isSpotDone)
{
	CCircuitAI* circuit = manager->GetCircuit();
	CMilitaryManager* militaryMgr = circuit->GetMilitaryManager();
	const int frame = circuit->GetLastFrame();
	isSpotDone = false;
	const bool isOwnDest = utils::is_valid(raidSpot) && (position.SqDistance2D(raidSpotPos) < 1.f);
	if (isOwnDest) {
		if (isDanger) {
			militaryMgr->BlockRaidSpot(raidSpot, FRAMES_PER_SEC * 10);  // don't re-pick it straight away
		} else if (!mustRepick && !circuit->GetMapManager()->IsInLOS(raidSpot)) {
			return true;  // keep heading to it
		}
	} else if (!mustRepick && !isDanger && (frame < raidSpotFailFrame + FRAMES_PER_SEC * 5)) {
		return false;  // no spot qualified a moment ago: keep the current destination, score again in a while
	}

	const AIFloat3 spot = militaryMgr->PickRaidSpot(leader, safeThreat);
	raidSpotPos = utils::is_valid(spot)
			? circuit->GetTerrainManager()->GetMovePosition(leader->GetArea(), spot) : AIFloat3(-RgtVector);
	if (utils::is_valid(raidSpotPos)) {
		raidSpot = spot;
		position = raidSpotPos;
		return true;
	}
	raidSpot = -RgtVector;
	raidSpotFailFrame = frame;
	isSpotDone = isOwnDest;  // the spot we were heading to is done and there's no next one: move on the old way
	return false;
}

void CRaidTask::ApplyRaidPath(const CQueryPathSingle* query)
{
	pPath = query->GetPathInfo();

	if (pPath->path.size() > 2) {
//		position = path.back();
		ActivePath();
		roamPath = pPath;  // FallbackRaid may keep walking it for a while
		return;
	}

	// Short path: plain Fight order, sent from Update(). Path callbacks can run during an allied AI's turn,
	// and orders sent from there go out under the wrong team and are dropped.
	isFightPending = true;
}

/*
 * Raid rally joiner (quota.raid_rally)
 */

CRaidTask* CRaidTask::UpdateChase(int frame)
{
	CMilitaryManager* militaryMgr = GetMilitaryMgr();
	if (!militaryMgr->IsRaidRallyJoiner(this)) {
		chaseTask = nullptr;
		chaseFails = 0;
		return nullptr;
	}
	if (frame < chaseRetryFrame) {
		chaseTask = nullptr;
		return nullptr;  // CHASE_FAILS empty routes in a row: raid normally for a while
	}
	if (isChaseSkip) {
		isChaseSkip = false;
		return nullptr;  // the last chase route came back empty: raid normally this time, never wait
	}
	// Keep the group we chase while it is still bigger and in reach (it may be gone: look it up first)
	const std::set<IFighterTask*>& tasks = militaryMgr->GetTasks(fightType);
	auto it = tasks.find(const_cast<CRaidTask*>(chaseTask));
	const bool isKeep = (it != tasks.end()) && IsChaseable(chaseTask, frame, SQUARE(CHASE_KEEP_SEC * lowestSpeed));
	if (!isKeep) {
		chaseTask = FindChaseTask(frame);
	}
	if ((chaseTask != nullptr) && utils::is_valid(raidSpot)) {
		militaryMgr->ReleaseRaidSpot(leader);  // no longer heading for our raid spot: free it for other groups
		raidSpot = -RgtVector;
	}
	return const_cast<CRaidTask*>(chaseTask);
}

const CRaidTask* CRaidTask::FindChaseTask(int frame) const
{
	// The nearest bigger one within CHASE_TRAVEL_SEC
	const AIFloat3& pos = leader->GetPos(frame);
	const CRaidTask* best = nullptr;
	float minSqDist = SQUARE(CHASE_TRAVEL_SEC * lowestSpeed);
	for (IFighterTask* t : GetMilitaryMgr()->GetTasks(fightType)) {
		const CRaidTask* group = static_cast<const CRaidTask*>(t);
		if ((group == this) || (group->GetLeader() == nullptr)) {
			continue;
		}
		const float sqDist = pos.SqDistance2D(group->GetLeader()->GetPos(frame));
		if ((sqDist < minSqDist) && IsChaseable(group, frame, minSqDist)) {
			minSqDist = sqDist;
			best = group;
		}
	}
	return best;
}

bool CRaidTask::IsChaseable(const CRaidTask* group, int frame, float sqMaxDist) const
{
	CCircuitUnit* gLeader = group->GetLeader();
	// Bigger than us, with room for us (it may grow up to the rally's goal; a full group would only be escorted)
	if (group->IsDead() || (gLeader == nullptr) || !group->HasRoom()
		|| !CMilitaryManager::IsRankedAbove(group->GetAttackPower(), gLeader, attackPower, leader))
	{
		return false;
	}
	const AIFloat3& gPos = gLeader->GetPos(frame);
	return (leader->GetPos(frame).SqDistance2D(gPos) < sqMaxDist) && group->IsRallyCompatible(leader)
			&& manager->GetCircuit()->GetTerrainManager()->CanMoveToPos(leader->GetArea(), gPos);
}

void CRaidTask::Chase(int frame, const CRaidTask* group)
{
	isContestedDest = false;
	isFightPending = false;
	const AIFloat3& startPos = leader->GetPos(frame);
	const AIFloat3 goal = group->GetLeader()->GetPos(frame);
	position = goal;
	isChaseDest = true;  // FallbackRaid picks a new destination once we raid again
	if (startPos.SqDistance2D(goal) < SQUARE(CHASE_PATH_RANGE)) {
		// Beside it already (merging waits for a safe line): stay with it. A Fight order every second would make the
		// units stutter and turn: re-send it only once the leader moved on, or now and then (other orders since)
		if ((goal.SqDistance2D(nearFightPos) > SQUARE(CHASE_NEAR_MOVE)) || (frame >= nearFightFrame + CHASE_NEAR_FRAMES)) {
			SendPendingFight(frame);
			nearFightPos = goal;
			nearFightFrame = frame;
		}
		return;
	}
	nearFightPos = -RgtVector;  // walking a route again: the next near Fight goes out at once
	if (!IsQueryReady(leader)) {
		return;
	}
	// Still walking a fresh route to where the group's leader is now (it moved less than CHASE_ROUTE_SLACK of the way)
	if ((chaseRoute != nullptr) && (frame < chaseRouteFrame + CHASE_ROUTE_FRAMES)
		&& (goal.SqDistance2D(chaseGoal)
			< SQUARE(std::max(CHASE_ROUTE_SLACK * startPos.distance2D(chaseGoal), CHASE_ROUTE_SLACK_MIN)))
		&& leader->GetTravelAct()->IsActive() && (leader->GetTravelAct()->GetPath() == chaseRoute))
	{
		pPath = chaseRoute;  // Start() and regrouping re-send pPath
		return;
	}
	chaseRouteFrame = frame;
	chaseGoal = goal;
	chaseRoute = nullptr;  // set again when this route arrives
	// Capped like target routes (GetPathThreatCap: our strength, distance curve, base bonus; caution: GetPathThreatMod)
	QueryPath(leader, goal, CHASE_PATH_RANGE, true, [this](const CQueryPathSingle* query) {
		this->ApplyChasePath(query);
	});
}

void CRaidTask::ApplyChasePath(const CQueryPathSingle* query)
{
	pPath = query->GetPathInfo();
	if (!pPath->posPath.empty()) {
		chaseFails = 0;
		chaseRoute = pPath;
		ActivePath();
		return;
	}
	// No safe route: raid normally at once (the next update decides), never wait for one
	const int frame = manager->GetCircuit()->GetLastFrame();
	isChaseSkip = true;
	lastTouched = frame - FRAMES_PER_SEC;
	if (++chaseFails >= CHASE_FAILS) {
		chaseFails = 0;
		chaseRetryFrame = frame + CHASE_RETRY_FRAMES;
	}
}

void CRaidTask::AttackDirect(int frame)
{
	// Same re-order rule as IFighterTask::Attack, without firing spots
	const int targetTile = manager->GetCircuit()->GetInflMap()->Pos2Index(target->GetPos());
	const bool isRepeatAttack = (frame >= attackFrame + attackRepeat);
	attackFrame = isRepeatAttack ? frame : attackFrame;
	for (CCircuitUnit* unit : units) {
		if ((unit->Blocker() != nullptr) || IsScooting(unit)) {
			continue;
		}
		unit->GetTravelAct()->StateWait();
		fireSpots.erase(unit);
		if (isRepeatAttack || (unit->GetTarget() != target) || (unit->GetTargetTile() != targetTile)) {
			unit->Attack(target, targetTile, frame + FRAMES_PER_SEC * 60);
		}
	}
}

bool CRaidTask::IsAllHolding() const
{
	// HOLD (IFighterTask::Attack): ranged unit ordered on this target without a firing spot
	const int frame = manager->GetCircuit()->GetLastFrame();
	int ranged = 0;
	for (CCircuitUnit* unit : units) {
		const float range = GetFireRange(unit);
		if (unit->GetCircuitDef()->IsAttrMelee() || (range < SQUARE_SIZE * 4)) {
			continue;
		}
		++ranged;
		if ((unit->GetTarget() != target) || (fireSpots.find(unit) != fireSpots.end())
			|| (unit->GetPos(frame).SqDistance2D(target->GetPos()) <= SQUARE(range)))
		{
			return false;  // has a spot, or can already shoot it
		}
	}
	return ranged > 0;
}

void CRaidTask::SendPendingFight(int frame)
{
	isFightPending = false;
	CCircuitAI* circuit = manager->GetCircuit();
	for (CCircuitUnit* unit : units) {
		TRY_UNIT(circuit, unit,
			unit->GetUnit()->Fight(position, UNIT_COMMAND_OPTION_RIGHT_MOUSE_KEY, frame + FRAMES_PER_SEC * 60);
		)
		unit->GetTravelAct()->StateWait();
	}
}

float CRaidTask::GetSafeThreatAt(const AIFloat3& pos, float safeThreat) const
{
	// Same allowances as FindTarget: near-base bonus and the distance curve at this spot
	CCircuitAI* circuit = manager->GetCircuit();
	const AIFloat3& basePos = circuit->GetSetupManager()->GetBasePos();
	const float sqDist = basePos.SqDistance2D(pos);
	return safeThreat * circuit->GetMilitaryManager()->GetBaseThrBonus(sqDist) / GetThrDistModAt(pos);
}

bool CRaidTask::Flee(int frame)
{
	CCircuitAI* circuit = manager->GetCircuit();
	AIFloat3 dest = circuit->GetFactoryManager()->GetClosestHaven(leader);
	if (!utils::is_valid(dest)) {
		dest = circuit->GetSetupManager()->GetBasePos();
	}
	if (leader->GetPos(frame).SqDistance2D(dest) < SQUARE(400.f)) {
		// End the "keep running" hold, but keep the 4 s cautious window of the last escape
		fleeFrame = std::min(fleeFrame, frame);
		return false;  // danger at home: running gets us nowhere
	}

	SetTarget(nullptr);
	state = State::ROAM;
	isFightPending = false;  // the escape replaces any pending roam order
	nearFightPos = -RgtVector;  // and any Fight order beside a chased group
	const bool wasFleeing = (frame < fleeFrame + FRAMES_PER_SEC);  // grace: still in the same escape
	fleeFrame = frame + FRAMES_PER_SEC * 2;
	if (wasFleeing && (frame < fleePathFrame + FRAMES_PER_SEC)) {
		return true;  // escape path is fresh
	}
	if (wasFleeing && IsFleeQueryPending() && (frame < fleePathFrame + FRAMES_PER_SEC * 5)) {
		return true;  // a slow pathfinder: don't cancel the pending way out with a new request (gives up after 5 s)
	}
	fleePathFrame = frame;
	lastTouched = frame;

	// No straight move home while the path is computed: a straight line can run through the threat,
	// and a one-reading danger blip would send the whole group home. Units keep their orders until the
	// least-threat path arrives; only if no path is found do they take the straight move (ApplyFleePath).
	fleeDest = dest;

	CPathFinder* pathfinder = circuit->GetPathfinder();
	std::shared_ptr<IPathQuery> query = pathfinder->CreatePathSingleQuery(
			leader, circuit->GetThreatMap(), frame,
			leader->GetPos(frame), dest, pathfinder->GetSquareSize());  // threat as cost, no cap: least-threat way out
	pathQueries[leader] = query;  // supersedes any pending target/raid path
	fleeQueryId = query->GetId();
	query->HoldTask(this);

	pathfinder->RunQuery(query, [this](const IPathQuery* query) {
		if (this->IsQueryAlive(query)) {
			this->fleeQueryId = -1;
			this->ApplyFleePath(static_cast<const CQueryPathSingle*>(query));
		}
	});
	return true;
}

bool CRaidTask::IsFleeQueryPending() const
{
	if (fleeQueryId < 0) {
		return false;
	}
	const auto it = pathQueries.find(leader);
	return (it != pathQueries.end()) && (it->second != nullptr) && (it->second->GetId() == fleeQueryId)
			&& !it->second->IsLost();  // dropped by a pathfinder handover: its callback never comes
}

void CRaidTask::ApplyFleePath(const CQueryPathSingle* query)
{
	// Keep the raid destination in 'position': once the escape ends, FallbackRaid re-checks it
	// (and picks another if it is dangerous) instead of the group walking all the way home
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	if (frame >= fleeFrame) {
		return;  // late result: the escape is over and the group may have engaged again, don't drag it home
	}
	pPath = query->GetPathInfo();
	if (pPath->posPath.empty()) {
		// No way out found: fall back to the straight move to the haven (or base)
		for (CCircuitUnit* unit : units) {
			if (unit->Blocker() != nullptr) {
				continue;
			}
			unit->GetTravelAct()->StateWait();
			TRY_UNIT(circuit, unit,
				unit->CmdMoveTo(fleeDest, UNIT_COMMAND_OPTION_RIGHT_MOUSE_KEY, frame + FRAMES_PER_SEC * 60);
				unit->CmdWantedSpeed(NO_SPEED_LIMIT);
			)
		}
		return;
	}
	ActivePath();
}

} // namespace circuit

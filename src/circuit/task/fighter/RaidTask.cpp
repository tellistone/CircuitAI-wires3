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

namespace circuit {

using namespace springai;

CRaidTask::CRaidTask(ITaskManager* mgr, float maxPower, float powerMod)
		: ISquadTask(mgr, FightType::RAID, powerMod)
		, maxPower(maxPower)
		, fleeFrame(0)
		, fleePathFrame(0)
		, pullFrame(0)
{
	CCircuitAI* circuit = manager->GetCircuit();
	float x = rand() % circuit->GetTerrainManager()->GetTerrainWidth();
	float z = rand() % circuit->GetTerrainManager()->GetTerrainHeight();
	position = AIFloat3(x, circuit->GetMap()->GetElevationAt(x, z), z);
}

CRaidTask::~CRaidTask()
{
}

bool CRaidTask::CanAssignTo(CCircuitUnit* unit) const
{
	if (!unit->GetCircuitDef()->IsRoleRaider() ||
		(unit->GetCircuitDef() != leader->GetCircuitDef()))
	{
		return false;
	}
	if (attackPower > maxPower) {
		return false;
	}
	const int frame = manager->GetCircuit()->GetLastFrame();
	if (leader->GetPos(frame).SqDistance2D(unit->GetPos(frame)) > SQUARE(1000.f)) {
		return false;
	}
	return true;
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
	ISquadTask* task = GetMergeTask();
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
	bool isPathUnsafe = false;
	if (IsInDanger(frame, isPathUnsafe) && Flee(frame)) {
		return;
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
		|| (isPathUnsafe && (frame >= lastTouched + FRAMES_PER_SEC / 2));
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
			// Direct Attack keeps FindTarget's priority (e.g. turret over nearby solars); a fight-move would shoot whatever comes first
			Attack(frame, true);
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

	CPathFinder* pathfinder = circuit->GetPathfinder();
	std::shared_ptr<IPathQuery> query = pathfinder->CreatePathMultiQuery(
			leader, threatMap, frame,
			startPos, pathRange, !urgentPositions.empty() ? urgentPositions : enemyPositions, GetHitTest(), GetSafeThreat());
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
	const float maxSpeed = SQUARE(highestSpeed * 0.8f / FRAMES_PER_SEC);
	// Current health, same measure as the escape check; for 4s after an escape demand a 25% margin,
	// so the group doesn't pick a target at the edge of the danger it just left and bounce back and forth
	const bool isCautious = (circuit->GetLastFrame() < fleeFrame + FRAMES_PER_SEC * 4);
	const float maxPower = GetSafeThreat() * (isCautious ? 0.75f : 1.f);
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
	threatMap->SetThreatType(leader);
	const CCircuitAI::EnemyInfos& enemies = circuit->GetEnemyInfos();
	for (auto& kv : enemies) {
		CEnemyInfo* enemy = kv.second;
		if (enemy->IsHidden() || (enemy->GetTasks().size() > 2)) {
			continue;
		}

		const AIFloat3& ePos = enemy->GetPos();
		const bool isEnemyUrgent = isDefender && (inflMap->GetAllyDefendInflAt(ePos) > INFL_EPS);
		if ((!isEnemyUrgent && !urgentPositions.empty())
			|| !terrainMgr->CanMobileReachAt(area, ePos, highestRange))
		{
			continue;
		}

		const float sqEBDist = basePos.SqDistance2D(ePos);
		float checkPower = maxPower;
		float checkSpeed = maxSpeed;
		if (sqEBDist < sqBaseRange) {
			checkPower *= 2.0f - 1.0f / baseRange * sqrtf(sqEBDist);  // 200% near base
			checkSpeed *= 2.f;
		}
		const float power = threatMap->GetThreatAt(ePos);
		if (checkPower <= power) {
			continue;
		}
		const AIFloat3& eVel = enemy->GetVel();
		if ((eVel.SqLength2D() >= checkSpeed) && (eVel.dot2D(pos - ePos) < 0)) {
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
		if ((minPower > power) && (minSqDist > sqDist)) {
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
		}
	}
	if (bestTarget == nullptr) {
		bestTarget = worstTarget;
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
		FallbackRaid();
	}
}

void CRaidTask::FallbackRaid()
{
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	CThreatMap* threatMap = circuit->GetThreatMap();
	const AIFloat3& pos = leader->GetPos(frame);
	const AIFloat3& threatPos = leader->GetTravelAct()->IsActive() ? position : pos;
	if (GetSafeThreat() <= threatMap->GetThreatAt(leader, threatPos)) {
		position = circuit->GetMilitaryManager()->GetRaidPosition(leader);
	}

	if (!utils::is_valid(position)) {
		float x = rand() % terrainMgr->GetTerrainWidth();
		float z = rand() % terrainMgr->GetTerrainHeight();
		position = AIFloat3(x, circuit->GetMap()->GetElevationAt(x, z), z);
		position = terrainMgr->GetMovePosition(leader->GetArea(), position);
	}

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

void CRaidTask::ApplyRaidPath(const CQueryPathSingle* query)
{
	pPath = query->GetPathInfo();

	if (pPath->path.size() > 2) {
//		position = path.back();
		ActivePath();
		return;
	}

	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	for (CCircuitUnit* unit : units) {
		TRY_UNIT(circuit, unit,
			unit->GetUnit()->Fight(position, UNIT_COMMAND_OPTION_RIGHT_MOUSE_KEY, frame + FRAMES_PER_SEC * 60);
		)
		unit->GetTravelAct()->StateWait();
	}
}

float CRaidTask::GetSafeThreat() const
{
	// Like attackPower (def power incl. pwr_mod) but scaled by current health, times the task's powerMod
	float power = 0.f;
	for (CCircuitUnit* unit : units) {
		power += GetUnitPower(unit);
	}
	return std::max(power, THREAT_MIN) * powerMod;
}

float CRaidTask::GetUnitPower(CCircuitUnit* unit) const
{
	return unit->GetCircuitDef()->GetPower() * sqrtf(std::max(unit->GetHealthPercent(), 0.f));
}

float CRaidTask::GetSafeThreatAt(const AIFloat3& pos, float safeThreat) const
{
	// Same allowance as FindTarget near our base: up to 200% at the base centre
	CCircuitAI* circuit = manager->GetCircuit();
	const AIFloat3& basePos = circuit->GetSetupManager()->GetBasePos();
	const float baseRange = circuit->GetMilitaryManager()->GetBaseDefRange();
	const float sqDist = basePos.SqDistance2D(pos);
	return (sqDist < SQUARE(baseRange)) ? safeThreat * (2.0f - sqrtf(sqDist) / baseRange) : safeThreat;
}

bool CRaidTask::IsInDanger(int frame, bool& isPathUnsafe)
{
	isPathUnsafe = false;
	endangered.clear();
	CThreatMap* threatMap = manager->GetCircuit()->GetThreatMap();
	const float safeThreat = GetSafeThreat();
	float totalPower = 0.f;
	float dangerPower = 0.f;
	for (CCircuitUnit* unit : units) {
		const float power = GetUnitPower(unit);
		totalPower += power;
		if (unit->GetUnit()->IsCloaked()) {
			continue;  // unseen: enemies in range can't shoot it
		}
		const AIFloat3& pos = unit->GetPos(frame);
		if (threatMap->GetThreatAt(unit, pos) >= GetSafeThreatAt(pos, safeThreat)) {
			endangered.push_back(unit);
			dangerPower += power;
		}
	}
	// Flee only when the units in danger hold at least quota.raid_flee_share of the group's current strength
	const float fleeShare = static_cast<CMilitaryManager*>(manager)->GetRaidFleeShare();
	if (!endangered.empty() && (dangerPower >= fleeShare * totalPower)) {
		return true;
	}

	if (!leader->GetTravelAct()->IsActive() || leader->GetUnit()->IsCloaked()) {
		return false;
	}
	const std::shared_ptr<PathInfo>& path = leader->GetTravelAct()->GetPath();
	if (path == nullptr) {
		return false;
	}
	// The stretch of path the group covers in the next ~2s
	const AIFloat3& pos = leader->GetPos(frame);
	const float lookAhead = std::max(leader->GetCircuitDef()->GetSpeed() * 2.f, 200.f);
	const float sqLookAhead = SQUARE(lookAhead);
	for (const AIFloat3& p : path->posPath) {
		if ((p.SqDistance2D(pos) < sqLookAhead) && (threatMap->GetThreatAt(leader, p) >= GetSafeThreatAt(p, safeThreat))) {
			isPathUnsafe = true;
			break;
		}
	}
	return false;
}

void CRaidTask::PullBackEndangered(int frame)
{
	// Units in danger that didn't make the whole group flee: send them back to the leader,
	// unless the leader is in danger too (then there is no safer spot nearby)
	if (endangered.empty() || (frame < pullFrame + FRAMES_PER_SEC)
		|| (std::find(endangered.begin(), endangered.end(), leader) != endangered.end()))
	{
		return;
	}
	pullFrame = frame;
	CCircuitAI* circuit = manager->GetCircuit();
	const AIFloat3& leaderPos = leader->GetPos(frame);
	for (CCircuitUnit* unit : endangered) {
		if (unit->Blocker() != nullptr) {
			continue;
		}
		unit->GetTravelAct()->StateWait();  // re-activated with the group's next path
		TRY_UNIT(circuit, unit,
			unit->CmdMoveTo(leaderPos, UNIT_COMMAND_OPTION_RIGHT_MOUSE_KEY, frame + FRAMES_PER_SEC * 60);
			unit->CmdWantedSpeed(NO_SPEED_LIMIT);
		)
	}
}

bool CRaidTask::Flee(int frame)
{
	CCircuitAI* circuit = manager->GetCircuit();
	AIFloat3 dest = circuit->GetFactoryManager()->GetClosestHaven(leader);
	if (!utils::is_valid(dest)) {
		dest = circuit->GetSetupManager()->GetBasePos();
	}
	if (leader->GetPos(frame).SqDistance2D(dest) < SQUARE(400.f)) {
		fleeFrame = 0;
		return false;  // danger at home: running gets us nowhere
	}

	SetTarget(nullptr);
	state = State::ROAM;
	const bool wasFleeing = (frame < fleeFrame + FRAMES_PER_SEC);  // grace: still in the same escape
	fleeFrame = frame + FRAMES_PER_SEC * 2;
	if (wasFleeing && (frame < fleePathFrame + FRAMES_PER_SEC)) {
		return true;  // escape path is fresh
	}
	fleePathFrame = frame;
	lastTouched = frame;

	// Break off at once; the threat-aware path replaces this when it is ready
	if (!wasFleeing) {
		for (CCircuitUnit* unit : units) {
			if (unit->Blocker() != nullptr) {
				continue;
			}
			unit->GetTravelAct()->StateWait();
			TRY_UNIT(circuit, unit,
				unit->CmdMoveTo(dest, UNIT_COMMAND_OPTION_RIGHT_MOUSE_KEY, frame + FRAMES_PER_SEC * 60);
				unit->CmdWantedSpeed(NO_SPEED_LIMIT);
			)
		}
	}

	CPathFinder* pathfinder = circuit->GetPathfinder();
	std::shared_ptr<IPathQuery> query = pathfinder->CreatePathSingleQuery(
			leader, circuit->GetThreatMap(), frame,
			leader->GetPos(frame), dest, pathfinder->GetSquareSize());  // threat as cost, no cap: least-threat way out
	pathQueries[leader] = query;  // supersedes any pending target/raid path
	query->HoldTask(this);

	pathfinder->RunQuery(query, [this](const IPathQuery* query) {
		if (this->IsQueryAlive(query)) {
			this->ApplyFleePath(static_cast<const CQueryPathSingle*>(query));
		}
	});
	return true;
}

void CRaidTask::ApplyFleePath(const CQueryPathSingle* query)
{
	// Keep the raid destination in 'position': once the escape ends, FallbackRaid re-checks it
	// (and picks another if it is dangerous) instead of the group walking all the way home
	pPath = query->GetPathInfo();
	if (pPath->posPath.empty()) {
		return;  // keep the straight move from Flee()
	}
	ActivePath();
}

} // namespace circuit

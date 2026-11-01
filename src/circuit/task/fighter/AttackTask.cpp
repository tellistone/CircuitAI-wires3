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
#include "module/MilitaryManager.h"
#include "setup/SetupManager.h"
#include "terrain/TerrainManager.h"
#include "terrain/path/PathFinder.h"
#include "terrain/path/QueryPathSingle.h"
#include "terrain/path/QueryPathMulti.h"
#include "terrain/path/QueryLineMap.h"
#include "unit/action/FightAction.h"
#include "unit/action/MoveAction.h"
#include "unit/action/SupportAction.h"
#include "unit/enemy/EnemyUnit.h"
#include "unit/CircuitUnit.h"
#include "CircuitAI.h"
#include "util/Utils.h"

#include "spring/SpringMap.h"

#include "AISCommands.h"

namespace circuit {

using namespace springai;

CAttackTask::CAttackTask(ITaskManager* mgr, float minPower, float powerMod)
		: ISquadTask(mgr, FightType::ATTACK, powerMod)
		, minPower(minPower)
		, reinforce(nullptr)
		, isReinforceAbandoned(false)
{
	CCircuitAI* circuit = manager->GetCircuit();
	float x = rand() % circuit->GetTerrainManager()->GetTerrainWidth();
	float z = rand() % circuit->GetTerrainManager()->GetTerrainHeight();
	position = AIFloat3(x, circuit->GetMap()->GetElevationAt(x, z), z);
}

CAttackTask::~CAttackTask()
{
}

bool CAttackTask::CanAssignTo(CCircuitUnit* unit) const
{
	assert(leader != nullptr);

	const int frame = manager->GetCircuit()->GetLastFrame();
	if (leader->GetPos(frame).SqDistance2D(unit->GetPos(frame)) > SQUARE(1000.f)) {
		return false;
	}
	return IsCompatible(unit);
}

bool CAttackTask::IsCompatible(CCircuitUnit* unit) const
{
	assert(leader != nullptr);

	float speedLeader = leader->GetCircuitDef()->GetSpeed();
	float speedUnit = unit->GetCircuitDef()->GetSpeed();
	if (speedLeader > speedUnit) {
		std::swap(speedLeader, speedUnit);
	}
	if (speedLeader * 1.5f < speedUnit) {
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
	if (unit->GetCircuitDef()->IsAmphibious()
		&& (leader->GetCircuitDef()->IsAmphibious()
			|| leader->GetCircuitDef()->IsLander()
			|| leader->GetCircuitDef()->IsFloater()))
	{
		return true;
	}
	if ((leader->GetCircuitDef()->IsAbleToFly() && unit->GetCircuitDef()->IsAbleToFly())
		|| (leader->GetCircuitDef()->IsLander() && unit->GetCircuitDef()->IsLander())
		|| (leader->GetCircuitDef()->IsFloater() && unit->GetCircuitDef()->IsFloater()))
	{
		return true;
	}
	return false;
}

void CAttackTask::AssignTo(CCircuitUnit* unit)
{
	ISquadTask::AssignTo(unit);
	CCircuitDef* cdef = unit->GetCircuitDef();
	highestRange = std::max(highestRange, cdef->GetLosRadius());

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
}

void CAttackTask::RemoveAssignee(CCircuitUnit* unit)
{
	ISquadTask::RemoveAssignee(unit);
	if (leader == nullptr) {
		manager->AbortTask(this);
		return;
	}
	highestRange = std::max(highestRange, leader->GetCircuitDef()->GetLosRadius());
	// Under quota.attack after a loss the group stays together (it used to break up and walk home
	// to gather) and tries to reinforce the nearest army; with none it carries on as a small group.
	if ((attackPower < minPower) && (reinforce == nullptr) && !isReinforceAbandoned) {
		reinforce = static_cast<CMilitaryManager*>(manager)->FindReinforceSquad(leader);
	}
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
	 * Merge tasks if possible
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

	/*
	 * Regroup if required
	 */
	bool wasRegroup = (State::REGROUP == state);
	bool mustRegroup = IsMustRegroup();
	if (State::REGROUP == state) {
		if (mustRegroup) {
			CCircuitAI* circuit = manager->GetCircuit();
			int frame = circuit->GetLastFrame() + FRAMES_PER_SEC * 60;
			for (CCircuitUnit* unit : units) {
				unit->Gather(groupPos, frame);
				unit->GetTravelAct()->StateWait();
			}
		}
		return;
	}

	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	bool isExecute = (updCount % 4 == 2);
	if (!isExecute) {
		for (CCircuitUnit* unit : units) {
			isExecute |= unit->IsForceUpdate(frame);
		}
		if (!isExecute) {
			if (wasRegroup && !pPath->posPath.empty()) {
				ActivePath(lowestSpeed);
			}
			return;
		}
	} else {
		ISquadTask::Update();
		if (leader == nullptr) {  // task aborted
			return;
		}
	}

	/*
	 * Reinforce: a group under quota.attack walks to its army instead of hunting targets alone
	 */
	if (reinforce != nullptr) {
		if (attackPower >= minPower) {
			reinforce = nullptr;  // grown into an army itself (merges on the way)
		} else if (GetReinforceSquad() != nullptr) {
			Reinforce(frame);
			return;
		}  // else: no army left to join, carry on as a small attack group
	}

	const AIFloat3& startPos = leader->GetPos(frame);
	if (circuit->GetInflMap()->GetInfluenceAt(startPos) < -INFL_EPS) {
		SetTarget(nullptr);
	} else {
		FindTarget();
	}

	state = State::ROAM;
	if (target != nullptr) {
		if (position.SqDistance2D(startPos) < SQUARE(highestRange)) {
			state = State::ENGAGE;
			Attack(frame);
			return;
		}
	}

	if (!IsQueryReady(leader)) {
		return;
	}

	if (target == nullptr) {
		FallbackFrontPos();
		return;
	}

	const AIFloat3& endPos = position;
	CPathFinder* pathfinder = circuit->GetPathfinder();
	const float eps = pathfinder->GetSquareSize();
	const float pathRange = std::max(highestRange - eps, eps);

	std::shared_ptr<IPathQuery> query = pathfinder->CreatePathSingleQuery(
			leader, circuit->GetThreatMap(), frame,
			startPos, endPos, pathRange, GetHitTest(), attackPower);
	pathQueries[leader] = query;
	query->HoldTask(this);

	pathfinder->RunQuery(query, [this](const IPathQuery* query) {
		if (this->IsQueryAlive(query)) {
			this->ApplyTargetPath(static_cast<const CQueryPathSingle*>(query));
		}
	});
}

ISquadTask* CAttackTask::GetAttackMergeTask()
{
	// Groups under quota.attack look for a group to join every 4 updates, others every 32.
	// Unlike other squads, an attack group may merge while its leader stands on enemy-held ground;
	// the straight line between the leaders must still be safe (CheckMergeTask).
	const int interval = (attackPower < minPower) ? 4 : 32;
	return (updCount % interval == 1) ? CheckMergeTask() : nullptr;
}

void CAttackTask::AbsorbWeakerTasks()
{
	// A group at or above quota.attack pulls in attack groups under quota.attack near it, every 8 updates.
	// Same rules as joining (CanAssignTo: speed within 1.5x, leaders within 1000 elmos, movement type,
	// never a worse mover than our leader; reachable; safe straight line), seen from the small group's side,
	// but at most 30 s of travel so absorbed units arrive well inside the 60 s regroup "stuck" limit.
	if ((attackPower < minPower) || (updCount % 8 != 5)) {
		return;
	}

	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	const AIFloat3& pos = leader->GetPos(frame);
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	CPathFinder* pathfinder = circuit->GetPathfinder();

	static std::vector<CAttackTask*> weaklings;  // NOTE: micro-opt
	const std::set<IFighterTask*>& tasks = static_cast<CMilitaryManager*>(manager)->GetTasks(fightType);
	for (IFighterTask* t : tasks) {
		CAttackTask* candy = static_cast<CAttackTask*>(t);
		CCircuitUnit* candyLeader = candy->GetLeader();
		if ((candy == this) || candy->IsDead() || (candyLeader == nullptr)
			|| (candy->GetAttackPower() >= minPower)
			|| !CanAssignTo(candyLeader))
		{
			continue;
		}
		const AIFloat3& candyPos = candyLeader->GetPos(frame);
		if (!terrainMgr->CanMoveToPos(candyLeader->GetArea(), pos)) {
			continue;
		}
		if (candyPos.SqDistance2D(pos) >= SQUARE(MAX_TRAVEL_SEC / 2 * candy->lowestSpeed)) {
			continue;
		}
		std::shared_ptr<CQueryLineMap> query = std::static_pointer_cast<CQueryLineMap>(
				pathfinder->CreateLineMapQuery(candyLeader, circuit->GetThreatMap(), frame));
		if (!query->IsSafeLine(candyPos, pos)) {
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
	CMilitaryManager* militaryMgr = static_cast<CMilitaryManager*>(manager);
	// NOTE: the army may be deleted already, do not dereference before lookup
	const std::set<IFighterTask*>& tasks = militaryMgr->GetTasks(fightType);
	if ((tasks.find(static_cast<IFighterTask*>(reinforce)) != tasks.end()) && !reinforce->IsDead()
		&& (reinforce->GetLeader() != nullptr) && (reinforce->GetAttackPower() >= minPower))
	{
		return reinforce;
	}
	// Army gone or shrunk: pick the nearest other one (excludes this task, its power is under quota.attack)
	reinforce = militaryMgr->FindReinforceSquad(leader);
	return reinforce;
}

void CAttackTask::Reinforce(const int frame)
{
	SetTarget(nullptr);
	state = State::ROAM;

	if (!IsQueryReady(leader)) {
		return;
	}

	// Threat-aware path to the army's leader; cells with threat above our attackPower are blocked
	// (same cap as an army's target path). No safe route: give up reinforcing (ApplyReinforcePath).
	// Merging takes over within 1000 elmos (CheckMergeTask every 4 updates from our side,
	// AbsorbWeakerTasks every 8 from the army's side).
	CCircuitAI* circuit = manager->GetCircuit();
	const AIFloat3& startPos = leader->GetPos(frame);
	position = reinforce->GetLeaderPos(frame);  // validated by GetReinforceSquad this update
	const float pathRange = DEFAULT_SLACK * 4;

	CPathFinder* pathfinder = circuit->GetPathfinder();
	std::shared_ptr<IPathQuery> query = pathfinder->CreatePathSingleQuery(
			leader, circuit->GetThreatMap(), frame,
			startPos, position, pathRange, nullptr, attackPower);
	pathQueries[leader] = query;
	query->HoldTask(this);

	pathfinder->RunQuery(query, [this](const IPathQuery* query) {
		if (this->IsQueryAlive(query)) {
			this->ApplyReinforcePath(static_cast<const CQueryPathSingle*>(query));
		}
	});
}

void CAttackTask::ApplyReinforcePath(const CQueryPathSingle* query)
{
	pPath = query->GetPathInfo();

	if (!pPath->path.empty()) {
		if (pPath->path.size() > 2) {
			ActivePath(lowestSpeed);
		}
	} else {
		// No safe route to the army: abandon reinforcing for good and act as a small attack group
		// from the next decision on (merge checks and absorbing still apply)
		reinforce = nullptr;
		isReinforceAbandoned = true;
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

	if (units.find(unit) != units.end()) {
		Start(unit);  // NOTE: Not sure if it has effect
	}
}

void CAttackTask::FindTarget()
{
	CCircuitAI* circuit = manager->GetCircuit();
	CMap* map = circuit->GetMap();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	CThreatMap* threatMap = circuit->GetThreatMap();
	const AIFloat3& basePos = circuit->GetSetupManager()->GetBasePos();
	const AIFloat3& pos = leader->GetPos(circuit->GetLastFrame());
	STerrainMapArea* area = leader->GetArea();
	CCircuitDef* cdef = leader->GetCircuitDef();
	const bool isAntiStatic = cdef->IsAttrAntiStat();
	const bool notAW = !cdef->HasAntiWater();
	const bool notAA = !cdef->HasAntiAir();
	const float maxSpeed = SQUARE(highestSpeed * 0.8f / FRAMES_PER_SEC);
	const float maxPower = attackPower * powerMod;  // divided by the thr_mod.distance curve at each enemy's spot
	const float weaponRange = cdef->GetMaxRange();
	const int canTargetCat = cdef->GetTargetCategory();
	const int noChaseCat = cdef->GetNoChaseCategory();

	CEnemyInfo* bestTarget = nullptr;
	const float sqOBDist = pos.SqDistance2D(basePos);  // Own to Base distance
	float minSqDist = std::numeric_limits<float>::max();

	SetTarget(nullptr);  // make adequate enemy->GetTasks().size()
	threatMap->SetThreatType(leader);
	const CCircuitAI::EnemyInfos& enemies = circuit->GetEnemyInfos();
	for (auto& kv : enemies) {
		CEnemyInfo* enemy = kv.second;
		if (enemy->IsHidden() || (enemy->GetTasks().size() > 2)) {
			continue;
		}
		const AIFloat3& ePos = enemy->GetPos();
		const float sqBEDist = ePos.SqDistance2D(basePos);  // Base to Enemy distance
		const float scale = std::min(sqBEDist / sqOBDist, 1.f);
		if ((maxPower / GetThrDistModAt(ePos) <= threatMap->GetThreatAt(ePos) * scale)
			|| !terrainMgr->CanMobileReachAt(area, ePos, highestRange))
		{
			continue;
		}
		const AIFloat3& eVel = enemy->GetVel();
		if ((eVel.SqLength2D() >= maxSpeed) && (eVel.dot2D(pos - ePos) < 0)) {
			continue;
		}

		CCircuitDef* edef = enemy->GetCircuitDef();
		if (edef != nullptr) {
			if ((isAntiStatic && edef->IsMobile())
				|| ((edef->GetCategory() & canTargetCat) == 0)
				|| ((edef->GetCategory() & noChaseCat) != 0)
				|| (edef->IsAbleToFly() && notAA))
			{
				continue;
			}
			float elevation = map->GetElevationAt(ePos.x, ePos.z);
			if ((notAW && !edef->IsYTargetable(elevation, ePos.y))
				|| (ePos.y - elevation > weaponRange)
				/*|| enemy->IsBeingBuilt()*/)
			{
				continue;
			}
		} else {
			if (notAW && (ePos.y < -SQUARE_SIZE * 5)) {
				continue;
			}
		}

		const float sqOEDist = pos.SqDistance2D(ePos) * scale;  // Own to Enemy distance
		if (minSqDist > sqOEDist) {
			minSqDist = sqOEDist;
			bestTarget = enemy;
		}
	}

	if (bestTarget != nullptr) {
		SetTarget(bestTarget);
		position = target->GetPos();
	}
	// Return: target, startPos=leader->pos, endPos=position
}

void CAttackTask::ApplyTargetPath(const CQueryPathSingle* query)
{
	pPath = query->GetPathInfo();

	if (!pPath->posPath.empty()) {
		ActivePath(lowestSpeed);
	} else {
		Fallback();
	}
}

void CAttackTask::FallbackFrontPos()
{
	CCircuitAI* circuit = manager->GetCircuit();
	circuit->GetMilitaryManager()->FillFrontPos(leader, urgentPositions);
	if (urgentPositions.empty()) {
		FallbackBasePos();
		return;
	}

	const int frame = circuit->GetLastFrame();
	const AIFloat3& startPos = leader->GetPos(frame);
	const float pathRange = DEFAULT_SLACK * 4;

	CPathFinder* pathfinder = circuit->GetPathfinder();
	std::shared_ptr<IPathQuery> query = pathfinder->CreatePathMultiQuery(
			leader, circuit->GetThreatMap(), frame,
			startPos, pathRange, urgentPositions);
	pathQueries[leader] = query;
	query->HoldTask(this);

	pathfinder->RunQuery(query, [this](const IPathQuery* query) {
		if (this->IsQueryAlive(query)) {
			this->ApplyFrontPos(static_cast<const CQueryPathMulti*>(query));
		}
	});
}

void CAttackTask::ApplyFrontPos(const CQueryPathMulti* query)
{
	pPath = query->GetPathInfo();

	if (!pPath->path.empty()) {
		if (pPath->path.size() > 2) {
			ActivePath();
		}
	} else {
		FallbackBasePos();
	}
}

void CAttackTask::FallbackBasePos()
{
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	CSetupManager* setupMgr = circuit->GetSetupManager();

	const AIFloat3& startPos = leader->GetPos(frame);
	const AIFloat3& endPos = setupMgr->GetBasePos();
	const float pathRange = DEFAULT_SLACK * 4;

	CPathFinder* pathfinder = circuit->GetPathfinder();
	std::shared_ptr<IPathQuery> query = pathfinder->CreatePathSingleQuery(
			leader, circuit->GetThreatMap(), frame,
			startPos, endPos, pathRange);
	pathQueries[leader] = query;
	query->HoldTask(this);

	pathfinder->RunQuery(query, [this](const IPathQuery* query) {
		if (this->IsQueryAlive(query)) {
			this->ApplyBasePos(static_cast<const CQueryPathSingle*>(query));
		}
	});
}

void CAttackTask::ApplyBasePos(const CQueryPathSingle* query)
{
	pPath = query->GetPathInfo();

	if (!pPath->path.empty()) {
		if (pPath->path.size() > 2) {
			ActivePath();
		}
	} else {
		Fallback();
	}
}

void CAttackTask::Fallback()
{
	// should never happen
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	for (CCircuitUnit* unit : units) {
		TRY_UNIT(circuit, unit,
			unit->GetUnit()->Fight(position, UNIT_COMMAND_OPTION_RIGHT_MOUSE_KEY, frame + FRAMES_PER_SEC * 60);
			unit->CmdWantedSpeed(lowestSpeed);
		)
		unit->GetTravelAct()->StateWait();
	}
}

} // namespace circuit

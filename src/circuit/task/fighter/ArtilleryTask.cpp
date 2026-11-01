/*
 * ArtilleryTask.cpp
 *
 *  Created on: Jan 6, 2016
 *      Author: rlcevg
 */

#include "task/fighter/ArtilleryTask.h"
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
#include "unit/enemy/EnemyUnit.h"
#include "unit/CircuitUnit.h"
#include "CircuitAI.h"
#include "util/Utils.h"

#include "spring/SpringMap.h"

#include "AISCommands.h"

namespace circuit {

using namespace springai;

CArtilleryTask::CArtilleryTask(ITaskManager* mgr)
		: IFighterTask(mgr, FightType::ARTY, 1.f)
{
	position = manager->GetCircuit()->GetSetupManager()->GetBasePos();
}

CArtilleryTask::~CArtilleryTask()
{
}

bool CArtilleryTask::CanAssignTo(CCircuitUnit* unit) const
{
	return units.empty() && unit->GetCircuitDef()->IsRoleArty();
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
	Execute(unit, false);
}

void CArtilleryTask::Update()
{
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();

	if (++updCount % 4 == 0) {
		for (CCircuitUnit* unit : units) {
			Execute(unit, true);
		}
	} else {
		for (CCircuitUnit* unit : units) {
			if (unit->IsForceUpdate(frame)) {
				Execute(unit, true);
			}
		}
	}
}

void CArtilleryTask::Execute(CCircuitUnit* unit, bool isUpdating)
{
	if (unit->Blocker() != nullptr) {
		return;  // Do not interrupt current action
	}

	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	const AIFloat3& pos = unit->GetPos(frame);
	CEnemyInfo* bestTarget = FindTarget(unit, pos);

	// ally_threat: don't stay behind shooting once the escort has moved on
	const bool isNeedCover = (circuit->GetMilitaryManager()->GetArtilleryCfg().allyThreat >= 0.f);
	if ((bestTarget != nullptr) && isNeedCover && !HasCover(unit, pos)) {
		if (IsQueryReady(unit)) {
			FallbackNoSpot(unit, isUpdating);
		}
		return;
	}

	if (bestTarget != nullptr) {
		unit->GetTravelAct()->StateWait();

		TRY_UNIT(circuit, unit,
			unit->GetUnit()->Attack(bestTarget->GetUnit(), UNIT_COMMAND_OPTION_RIGHT_MOUSE_KEY, frame + FRAMES_PER_SEC * 60);
			unit->CmdSetTarget(bestTarget);
		)
		return;
	}

	if (!IsQueryReady(unit)) {
		return;
	}

	if (enemyPositions.empty()) {
		FallbackNoTarget(unit, isUpdating);
		return;
	}

	CCircuitDef* cdef = unit->GetCircuitDef();
	CThreatMap* threatMap = circuit->GetThreatMap();
	const float range = std::max(cdef->GetMaxRange()/* - threatMap->GetSquareSize()*/, (float)threatMap->GetSquareSize());

	// artillery block of behaviour.json: firing spots need allied cover, paths may not cross heavy threat
	const CMilitaryManager::SArtilleryCfg& artyCfg = circuit->GetMilitaryManager()->GetArtilleryCfg();
	const float maxThreat = (artyCfg.maxPathThreat >= 0.f)
			? std::max(artyCfg.maxPathThreat * cdef->GetThreat(), THREAT_MIN)  // same formula as enemy threat on the map
			: std::numeric_limits<float>::max();

	// Goals: either any cell within range of a building (old), or only covered firing cells
	const F3Vec* goals = &enemyPositions;
	float goalRange = range;
	if (artyCfg.allyThreat >= 0.f) {
		goalRange = FillCoveredSpots(unit, range);
		if (firePositions.empty()) {
			FallbackNoSpot(unit, isUpdating);
			return;
		}
		goals = &firePositions;
	}

	CPathFinder* pathfinder = circuit->GetPathfinder();
	std::shared_ptr<IPathQuery> query = pathfinder->CreatePathMultiQuery(
			unit, threatMap, frame,
			pos, goalRange, *goals, nullptr, maxThreat);
	pathQueries[unit] = query;
	query->HoldTask(this);

	pathfinder->RunQuery(query, [this, isUpdating](const IPathQuery* query) {
		if (this->IsQueryAlive(query)) {
			this->ApplyTargetPath(static_cast<const CQueryPathMulti*>(query), isUpdating);
		}
	});
}

void CArtilleryTask::OnUnitIdle(CCircuitUnit* unit)
{
	IFighterTask::OnUnitIdle(unit);
	if (units.find(unit) != units.end()) {
		RemoveAssignee(unit);
	}
}

CEnemyInfo* CArtilleryTask::FindTarget(CCircuitUnit* unit, const AIFloat3& pos)
{
	CCircuitAI* circuit = manager->GetCircuit();
	CThreatMap* threatMap = circuit->GetThreatMap();
	CCircuitDef* cdef = unit->GetCircuitDef();
	const bool notAW = !cdef->HasAntiWater();
	const int canTargetCat = cdef->GetTargetCategory();
	const int noChaseCat = cdef->GetNoChaseCategory();
	float range = cdef->GetMaxRange();
	float minSqDist = SQUARE(range);

	enemyPositions.clear();
	threatMap->SetThreatType(unit);
	bool isPosSafe = (threatMap->GetThreatAt(pos) <= THREAT_MIN) || cdef->IsPlane();

	const CCircuitAI::EnemyInfos& enemies = circuit->GetEnemyInfos();
	if (isPosSafe) {
		// Трубка 15, прицел 120, бац, бац …и мимо!
		float maxThreat = .0f;
		CEnemyInfo* bestTarget = nullptr;
		CEnemyInfo* mediumTarget = nullptr;
		CEnemyInfo* worstTarget = nullptr;
		for (auto& kv : enemies) {
			CEnemyInfo* enemy = kv.second;
			if (!enemy->IsInRadarOrLOS() ||
				(notAW && (enemy->GetPos().y < -SQUARE_SIZE * 5)))
			{
				continue;
			}

			CCircuitDef* edef = enemy->GetCircuitDef();
			if ((edef == nullptr) || edef->IsMobile() || edef->IsAttrSiege()) {
				continue;
			}
			int targetCat = edef->GetCategory();
			if ((targetCat & canTargetCat) == 0) {
				continue;
			}

			const float sqDist = pos.SqDistance2D(enemy->GetPos());
			if (sqDist < minSqDist) {
				if (edef->IsEnemyRoleAny(CCircuitDef::RoleMask::BUILDER)) {
					bestTarget = enemy;
					minSqDist = sqDist;
					maxThreat = std::numeric_limits<float>::max();
				} else if (edef->GetPower() > maxThreat) {
					bestTarget = enemy;
					minSqDist = sqDist;
					maxThreat = edef->GetPower();
				} else if (bestTarget == nullptr) {
					if ((targetCat & noChaseCat) == 0) {
						mediumTarget = enemy;
					} else if (mediumTarget == nullptr) {
						worstTarget = enemy;
					}
				}
				continue;
			}

			if ((targetCat & noChaseCat) != 0) {
				continue;
			}
//			if (sqDist < SQUARE(2000.f)) {  // maxSqDist
				enemyPositions.push_back(enemy->GetPos());
//			}
		}
		if (bestTarget == nullptr) {
			bestTarget = (mediumTarget != nullptr) ? mediumTarget : worstTarget;
		}
		if (bestTarget != nullptr) {
			position = bestTarget->GetPos();
			return bestTarget;
		}
	} else {
		// Avoid closest units and choose safe position
		for (auto& kv : enemies) {
			CEnemyInfo* enemy = kv.second;
			if (!enemy->IsInRadarOrLOS() ||
				(notAW && (enemy->GetPos().y < -SQUARE_SIZE * 5)))
			{
				continue;
			}

			CCircuitDef* edef = enemy->GetCircuitDef();
			if ((edef == nullptr) || edef->IsMobile()) {
				continue;
			}
			int targetCat = edef->GetCategory();
			if (((targetCat & canTargetCat) == 0)
				|| ((targetCat & noChaseCat) != 0))
			{
				continue;
			}

			const float sqDist = pos.SqDistance2D(enemy->GetPos());
			if (sqDist < minSqDist) {
				continue;
			}

//			if (sqDist < SQUARE(2000.f)) {  // maxSqDist
				enemyPositions.push_back(enemy->GetPos());
//			}
		}
	}

	if (enemyPositions.empty()) {
		return nullptr;
	}

	return nullptr;
	// Return: <target>, startPos=pos, enemyPositions
}

void CArtilleryTask::ApplyTargetPath(const CQueryPathMulti* query, bool isUpdating)
{
	const std::shared_ptr<PathInfo>& pPath = query->GetPathInfo();
	CCircuitUnit* unit = query->GetUnit();

	if (!pPath->posPath.empty()
		&& (manager->GetCircuit()->GetThreatMap()->GetThreatAt(unit, pPath->posPath.back()) <= THREAT_MIN))  // this unit's threat layer
	{
		position = pPath->posPath.back();
		unit->GetTravelAct()->SetPath(pPath);
		unit->GetTravelAct()->StateActivate();
	} else {
		FallbackNoSpot(unit, isUpdating);
	}
}

void CArtilleryTask::FallbackBasePos(CCircuitUnit* unit, bool isUpdating)
{
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	CSetupManager* setupMgr = circuit->GetSetupManager();

	const AIFloat3& startPos = unit->GetPos(frame);
	position = setupMgr->GetBasePos();
	const float pathRange = DEFAULT_SLACK * 4;  // pathfinder->GetSquareSize()

	CPathFinder* pathfinder = circuit->GetPathfinder();
	std::shared_ptr<IPathQuery> query = pathfinder->CreatePathSingleQuery(
			unit, circuit->GetThreatMap(), frame,
			startPos, position, pathRange);
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

	if (!pPath->posPath.empty()) {
		unit->GetTravelAct()->SetPath(pPath);
		unit->GetTravelAct()->StateActivate();
	} else if (manager->GetCircuit()->GetMilitaryManager()->GetArtilleryCfg().isNoScout) {
		unit->GetTravelAct()->StateWait();  // stay put rather than scout
	} else {
		FallbackScout(unit, isUpdating);
	}
}

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

	const float pathRange = DEFAULT_SLACK * 4;  // pathfinder->GetSquareSize()

	CPathFinder* pathfinder = circuit->GetPathfinder();
	std::shared_ptr<IPathQuery> query = pathfinder->CreatePathSingleQuery(
			unit, circuit->GetThreatMap(), frame,
			pos, position, pathRange);
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
		unit->GetUnit()->Fight(position, UNIT_COMMAND_OPTION_RIGHT_MOUSE_KEY, frame + FRAMES_PER_SEC * 60);
	)
	unit->GetTravelAct()->StateWait();
}

void CArtilleryTask::FallbackNoTarget(CCircuitUnit* unit, bool isUpdating)
{
	if (manager->GetCircuit()->GetMilitaryManager()->GetArtilleryCfg().isNoScout) {
		FallbackSafePos(unit, isUpdating);
	} else {
		FallbackScout(unit, isUpdating);
	}
}

void CArtilleryTask::FallbackSafePos(CCircuitUnit* unit, bool isUpdating)
{
	CCircuitAI* circuit = manager->GetCircuit();
	CMilitaryManager* militaryMgr = circuit->GetMilitaryManager();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	const int frame = circuit->GetLastFrame();

	// Nearest attack/defend group leaders, else a defended mex cluster, else the base
	militaryMgr->FillSafePos(unit, safePositions);

	// Stand behind the leader, on the side towards our base
	const float followDist = militaryMgr->GetArtilleryCfg().followDist;
	if (followDist > 0.f) {
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

	const AIFloat3& startPos = unit->GetPos(frame);
	const float pathRange = DEFAULT_SLACK * 4;

	CPathFinder* pathfinder = circuit->GetPathfinder();
	std::shared_ptr<IPathQuery> query = pathfinder->CreatePathMultiQuery(
			unit, circuit->GetThreatMap(), frame,
			startPos, pathRange, safePositions);
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

	if (!pPath->posPath.empty()) {
		position = pPath->posPath.back();
		unit->GetTravelAct()->SetPath(pPath);
		unit->GetTravelAct()->StateActivate();
	} else {
		FallbackBasePos(unit, isUpdating);
	}
}

void CArtilleryTask::FallbackNoSpot(CCircuitUnit* unit, bool isUpdating)
{
	if (manager->GetCircuit()->GetMilitaryManager()->GetArtilleryCfg().isNoScout) {
		FallbackSafePos(unit, isUpdating);
	} else {
		FallbackBasePos(unit, isUpdating);
	}
}

float CArtilleryTask::FillCoveredSpots(CCircuitUnit* unit, float range)
{
	firePositions.clear();

	CCircuitAI* circuit = manager->GetCircuit();
	const CMilitaryManager::SArtilleryCfg& artyCfg = circuit->GetMilitaryManager()->GetArtilleryCfg();
	CThreatMap* threatMap = circuit->GetThreatMap();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	STerrainMapArea* area = unit->GetArea();

	// Grid of candidate firing cells; the pathfinder accepts any spot within half a cell of a centre
	const int cellSize = circuit->GetPathfinder()->GetSquareSize() * 4;
	const float goalRange = cellSize * 0.5f;
	const int width = CTerrainManager::GetTerrainWidth() / cellSize + 1;
	const int height = CTerrainManager::GetTerrainHeight() / cellSize + 1;
	coverGrid.assign(width * height, 0.f);

	// Cover: threat of escorts near the cell
	const float allyRadius = artyCfg.allyRadius;
	const float sqAllyRadius = SQUARE(allyRadius);
	const int cellRad = int(allyRadius / cellSize) + 1;
	bool isAnyCover = false;
	CollectEscorts(unit);
	for (const auto& escort : escorts) {
		const AIFloat3& allyPos = escort.first;
		const float allyThreat = escort.second;
		const int ax = int(allyPos.x) / cellSize;
		const int az = int(allyPos.z) / cellSize;
		for (int z = std::max(az - cellRad, 0); z <= std::min(az + cellRad, height - 1); ++z) {
			for (int x = std::max(ax - cellRad, 0); x <= std::min(ax + cellRad, width - 1); ++x) {
				const AIFloat3 cellPos((x + 0.5f) * cellSize, 0.f, (z + 0.5f) * cellSize);
				if (cellPos.SqDistance2D(allyPos) <= sqAllyRadius) {
					coverGrid[z * width + x] += allyThreat;
					isAnyCover = true;
				}
			}
		}
	}
	if (!isAnyCover && (artyCfg.allyThreat > 0.f)) {
		return goalRange;
	}

	// Covered cells within range of a building: centre at most (range - cell) from it,
	// so any spot the pathfinder may stop at still reaches the building
	const float fireRange = std::max(range - cellSize, goalRange);
	const float sqFireRange = SQUARE(fireRange);
	const int fireRad = int(fireRange / cellSize) + 1;
	std::vector<char>& isAdded = cellAdded;
	isAdded.assign(width * height, 0);
	for (const AIFloat3& target : enemyPositions) {
		const int tx = int(target.x) / cellSize;
		const int tz = int(target.z) / cellSize;
		for (int z = std::max(tz - fireRad, 0); z <= std::min(tz + fireRad, height - 1); ++z) {
			for (int x = std::max(tx - fireRad, 0); x <= std::min(tx + fireRad, width - 1); ++x) {
				const int index = z * width + x;
				if (isAdded[index] || (coverGrid[index] < artyCfg.allyThreat)) {
					continue;
				}
				AIFloat3 cellPos((x + 0.5f) * cellSize, 0.f, (z + 0.5f) * cellSize);
				if (cellPos.SqDistance2D(target) > sqFireRange) {
					continue;
				}
				isAdded[index] = 1;  // checked once, whatever the outcome
				CTerrainManager::CorrectPosition(cellPos);
				// Only threat-free cells the unit can reach: otherwise the nearest goal may be one that
				// gets rejected afterwards (threat), or the search may sweep the whole map (unreachable)
				if ((threatMap->GetThreatAt(unit, cellPos) > THREAT_MIN) || !terrainMgr->CanMoveToPos(area, cellPos)) {
					continue;
				}
				firePositions.push_back(cellPos);
			}
		}
	}
	return goalRange;
}

void CArtilleryTask::CollectEscorts(CCircuitUnit* unit)
{
	// Allied mobile armed units. Artillery (main role) and aircraft don't count, so artillery can't escort itself
	escorts.clear();
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	circuit->UpdateFriendlyUnits();  // drop units that died since the last refresh
	for (auto& kv : circuit->GetFriendlyUnits()) {
		CAllyUnit* ally = kv.second;
		CCircuitDef* adef = ally->GetCircuitDef();
		if ((ally->GetId() == unit->GetId()) || (adef == nullptr) || !adef->IsMobile() || !adef->IsAttacker() || adef->IsPlane()
			|| (adef->GetMainRole() == ROLE_TYPE(ARTY)))
		{
			continue;
		}
		const AIFloat3& allyPos = ally->GetPos(frame);
		if (utils::is_valid(allyPos)) {
			escorts.emplace_back(allyPos, adef->GetThreat());
		}
	}
}

bool CArtilleryTask::HasCover(CCircuitUnit* unit, const AIFloat3& spot)
{
	CCircuitAI* circuit = manager->GetCircuit();
	const CMilitaryManager::SArtilleryCfg& artyCfg = circuit->GetMilitaryManager()->GetArtilleryCfg();
	// One grid cell of slack: firing spots are picked by cell centre, and the unit may stop up to
	// half a cell away, so without slack it could arrive, find no cover and turn back
	const float slack = circuit->GetPathfinder()->GetSquareSize() * 4;
	const float sqAllyRadius = SQUARE(artyCfg.allyRadius + slack);
	CollectEscorts(unit);
	float cover = 0.f;
	for (const auto& escort : escorts) {
		if (escort.first.SqDistance2D(spot) <= sqAllyRadius) {
			cover += escort.second;
		}
	}
	return cover >= artyCfg.allyThreat;
}

} // namespace circuit

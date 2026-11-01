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

	const bool isTick = (++updCount % 4 == 0);
	for (CCircuitUnit* unit : units) {
		if (CheckFlee(unit)) {  // every update: run from approaching enemies before anything else
			continue;
		}
		if (isTick || unit->IsForceUpdate(frame)) {
			Execute(unit, true);
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
			// Own power (pwr_mod), not threat: thr_mod shapes how scary a type looks as an enemy and is often ~0 for artillery
			? std::max(artyCfg.maxPathThreat * cdef->GetPower(), THREAT_MIN)
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

	const bool isNoScout = manager->GetCircuit()->GetMilitaryManager()->GetArtilleryCfg().isNoScout;
	if ((pPath->posPath.size() > 2) || (!isNoScout && !pPath->posPath.empty())) {
		unit->GetTravelAct()->SetPath(pPath);
		unit->GetTravelAct()->StateActivate();
	} else if (isNoScout) {
		HoldHere(unit);  // stay put rather than scout
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
		FallbackStay(unit, isUpdating);
	} else {
		FallbackScout(unit, isUpdating);
	}
}

void CArtilleryTask::FallbackStay(CCircuitUnit* unit, bool isUpdating)
{
	// Stay put while the spot is threat-free and (ally_threat) still covered, otherwise move to safety.
	// Re-checked every few updates by Execute
	CCircuitAI* circuit = manager->GetCircuit();
	const AIFloat3& pos = unit->GetPos(circuit->GetLastFrame());
	const bool isSafe = (circuit->GetThreatMap()->GetThreatAt(unit, pos) <= THREAT_MIN);
	const bool isCovered = (circuit->GetMilitaryManager()->GetArtilleryCfg().allyThreat < 0.f) || HasCover(unit, pos);
	if (!isSafe || !isCovered) {
		FallbackSafePos(unit, isUpdating);
		return;
	}
	HoldHere(unit);
}

void CArtilleryTask::HoldHere(CCircuitUnit* unit)
{
	if (unit->GetTravelAct()->IsActive()) {
		// Walking somewhere that is no longer wanted: the engine still holds queued waypoints, stop here
		CCircuitAI* circuit = manager->GetCircuit();
		TRY_UNIT(circuit, unit,
			unit->GetUnit()->Stop();
		)
	}
	unit->GetTravelAct()->StateWait();
}

void CArtilleryTask::FallbackSafePos(CCircuitUnit* unit, bool isUpdating)
{
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();

	FillSafeSpots(unit);

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

void CArtilleryTask::FillSafeSpots(CCircuitUnit* unit)
{
	CCircuitAI* circuit = manager->GetCircuit();
	CMilitaryManager* militaryMgr = circuit->GetMilitaryManager();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();

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
}

void CArtilleryTask::ApplySafePos(const CQueryPathMulti* query, bool isUpdating)
{
	const std::shared_ptr<PathInfo>& pPath = query->GetPathInfo();
	CCircuitUnit* unit = query->GetUnit();

	if (pPath->posPath.size() > 2) {
		position = pPath->posPath.back();
		unit->GetTravelAct()->SetPath(pPath);
		unit->GetTravelAct()->StateActivate();
	} else if (!pPath->posPath.empty()) {
		// Already there: a move order would finish at once, make the unit idle and restart the task
		HoldHere(unit);
	} else {
		FallbackBasePos(unit, isUpdating);
	}
}

void CArtilleryTask::FallbackNoSpot(CCircuitUnit* unit, bool isUpdating)
{
	if (manager->GetCircuit()->GetMilitaryManager()->GetArtilleryCfg().isNoScout) {
		FallbackStay(unit, isUpdating);
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

	// Grid of candidate firing cells, one per pathfinder square (64 elmos);
	// the pathfinder accepts any square within one square of a cell centre
	const int cellSize = circuit->GetPathfinder()->GetSquareSize();
	const float goalRange = cellSize;
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

	// Covered cells within range of a building: centre at most (range - 2 cells) from it,
	// so any spot the pathfinder may stop at (up to ~1.5 cells off) still reaches the building
	const float fireRange = std::max(range - 2 * cellSize, goalRange);
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

bool CArtilleryTask::CheckFlee(CCircuitUnit* unit)
{
	// artillery_flee_margin: flee when a known armed mobile enemy (or unidentified radar blip) is within
	// its weapon range + margin, unless (ally_threat) our cover is here. Returns true while fleeing
	CCircuitAI* circuit = manager->GetCircuit();
	const CMilitaryManager::SArtilleryCfg& artyCfg = circuit->GetMilitaryManager()->GetArtilleryCfg();
	const float fleeMargin = unit->GetCircuitDef()->GetArtyFleeMargin(artyCfg.fleeMargin);  // per unit type, else the block's
	if ((fleeMargin < 0.f) || (unit->Blocker() != nullptr)) {
		isFleeing = false;
		return false;
	}

	const int frame = circuit->GetLastFrame();
	const AIFloat3& pos = unit->GetPos(frame);
	CEnemyInfo* danger = nullptr;
	float minSqDist = std::numeric_limits<float>::max();
	for (auto& kv : circuit->GetEnemyInfos()) {
		CEnemyInfo* enemy = kv.second;
		// Only enemies we see now (not last-known ghosts) that can act
		if (enemy->IsHidden() || !enemy->IsInRadarOrLOS()
			|| enemy->IsBeingBuilt() || enemy->IsParalyzed() || enemy->IsDisarmed())
		{
			continue;
		}
		CCircuitDef* edef = enemy->GetCircuitDef();
		if (edef == nullptr) {
			// Unidentified radar blip: only a moving one (a parked building blip is no danger)
			if (enemy->GetVel().SqLength2D() < 0.01f) {
				continue;
			}
		} else if (!edef->IsMobile() || !edef->IsAttacker() || edef->IsPlane()) {
			continue;
		}
		const float reach = ((edef != nullptr) ? edef->GetMaxRange() : 0.f) + fleeMargin;
		const float sqDist = pos.SqDistance2D(enemy->GetPos());
		if ((sqDist <= SQUARE(reach)) && (sqDist < minSqDist)) {
			minSqDist = sqDist;
			danger = enemy;
		}
	}
	if ((danger == nullptr) || ((artyCfg.allyThreat >= 0.f) && HasCover(unit, pos))) {
		isFleeing = false;
		return false;
	}

	if ((isFleeing && (frame < fleeFrame)) || !IsQueryReady(unit)) {
		return true;  // already on the way
	}
	isFleeing = true;
	fleeFrame = frame + FRAMES_PER_SEC * 2;  // re-plan the flee every 2 s while the danger stays
	Flee(unit, danger->GetPos());
	return true;
}

void CArtilleryTask::Flee(CCircuitUnit* unit, const AIFloat3& dangerPos)
{
	CCircuitAI* circuit = manager->GetCircuit();
	CMilitaryManager* militaryMgr = circuit->GetMilitaryManager();
	const int frame = circuit->GetLastFrame();
	const AIFloat3& pos = unit->GetPos(frame);

	// Candidates: safety spots (behind groups, else defended mex cluster, else base) and the base
	FillSafeSpots(unit);
	const AIFloat3& basePos = circuit->GetSetupManager()->GetBasePos();
	if (circuit->GetTerrainManager()->CanMoveToPos(unit->GetArea(), basePos)) {
		safePositions.push_back(basePos);
	}

	// Nearest spot that is further from the danger than we are, else the one furthest from it
	const float sqDangerDist = pos.SqDistance2D(dangerPos);
	const AIFloat3* best = nullptr;
	float bestSqDist = std::numeric_limits<float>::max();
	for (const AIFloat3& spot : safePositions) {
		const float sqDist = pos.SqDistance2D(spot);
		if ((spot.SqDistance2D(dangerPos) > sqDangerDist) && (sqDist < bestSqDist)) {
			bestSqDist = sqDist;
			best = &spot;
		}
	}
	if (best == nullptr) {
		float maxSqDist = -1.f;
		for (const AIFloat3& spot : safePositions) {
			const float sqDist = spot.SqDistance2D(dangerPos);
			if (sqDist > maxSqDist) {
				maxSqDist = sqDist;
				best = &spot;
			}
		}
	}
	if (best == nullptr) {
		FallbackBasePos(unit, true);
		return;
	}
	position = *best;

	const CMilitaryManager::SArtilleryCfg& artyCfg = militaryMgr->GetArtilleryCfg();
	const float maxThreat = (artyCfg.maxPathThreat >= 0.f)
			? std::max(artyCfg.maxPathThreat * unit->GetCircuitDef()->GetPower(), THREAT_MIN)
			: std::numeric_limits<float>::max();
	const float pathRange = DEFAULT_SLACK * 4;

	CPathFinder* pathfinder = circuit->GetPathfinder();
	std::shared_ptr<IPathQuery> query = pathfinder->CreatePathSingleQuery(
			unit, circuit->GetThreatMap(), frame,
			pos, position, pathRange, nullptr, maxThreat);
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

	if (pPath->posPath.size() > 2) {
		unit->GetTravelAct()->SetPath(pPath);
		unit->GetTravelAct()->StateActivate();
	} else if (!pPath->posPath.empty()) {
		HoldHere(unit);  // already there; nowhere better to go
	} else {
		FallbackBasePos(unit, true);  // no path under the threat cap: any path home
	}
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
			escorts.emplace_back(allyPos, adef->GetPower());  // ally strength: pwr_mod, not thr_mod
		}
	}
}

bool CArtilleryTask::HasCover(CCircuitUnit* unit, const AIFloat3& spot)
{
	CCircuitAI* circuit = manager->GetCircuit();
	const CMilitaryManager::SArtilleryCfg& artyCfg = circuit->GetMilitaryManager()->GetArtilleryCfg();
	// Slack: firing spots are picked by cell centre, and the unit may stop up to ~1.5 cells
	// away, so without slack it could arrive, find no cover and turn back
	const float slack = circuit->GetPathfinder()->GetSquareSize() * 2;
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

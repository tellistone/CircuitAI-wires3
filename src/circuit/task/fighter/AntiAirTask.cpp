/*
 * AntiAirTask.cpp
 *
 *  Created on: Jan 6, 2016
 *      Author: rlcevg
 */

#include "task/fighter/AntiAirTask.h"
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
#include "unit/action/FightAction.h"
#include "unit/action/MoveAction.h"
#include "unit/enemy/EnemyUnit.h"
#include "unit/CircuitUnit.h"
#include "CircuitAI.h"
#include "util/Utils.h"

#include "spring/SpringMap.h"

#include "AISCommands.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace circuit {

using namespace springai;

static constexpr float UNKNOWN_AIR_HEIGHT = 50.f;  // an unidentified blip this far above ground counts as a plane
static constexpr float BLIP_POS_ERROR = 96.f;  // engine default radar error size (ZK keeps it): sideways shift of a blip

// Ground (or water surface) a radar blip stands on, as high as it can be: the blip is shifted sideways by up
// to BLIP_POS_ERROR, so take the highest ground around it, else a ground unit near a cliff reads as flying
static float GetBlipGroundHeight(CMap* map, const AIFloat3& p)
{
	const float maxX = CTerrainManager::GetTerrainWidth() - 1.f;
	const float maxZ = CTerrainManager::GetTerrainHeight() - 1.f;
	const float offsets[][2] = {{0.f, 0.f}, {1.f, 0.f}, {-1.f, 0.f}, {0.f, 1.f}, {0.f, -1.f},
								{0.7f, 0.7f}, {0.7f, -0.7f}, {-0.7f, 0.7f}, {-0.7f, -0.7f}};
	float height = 0.f;  // water surface: a ship or hover floats at 0 over a deep sea floor
	for (const auto& o : offsets) {
		const float x = std::min(std::max(p.x + o[0] * BLIP_POS_ERROR, 0.f), maxX);
		const float z = std::min(std::max(p.z + o[1] * BLIP_POS_ERROR, 0.f), maxZ);
		height = std::max(height, map->GetElevationAt(x, z));
	}
	return height;
}

static const int SPLIT_MERGE_COOLDOWN = FRAMES_PER_SEC * 30;  // a unit split off a blocked regroup doesn't merge for this long

CAntiAirTask::CAntiAirTask(ITaskManager* mgr, float powerMod)
		: ISquadTask(mgr, FightType::AA, powerMod)
{
	isFireSpotChase = false;  // fight from the firing spot, never let the engine chase a plane past it
	fireSpotReach = 2.f;  // stragglers keep following the squad path
	CCircuitAI* circuit = manager->GetCircuit();
	float x = rand() % circuit->GetTerrainManager()->GetTerrainWidth();
	float z = rand() % circuit->GetTerrainManager()->GetTerrainHeight();
	position = AIFloat3(x, circuit->GetMap()->GetElevationAt(x, z), z);
	approachPos = position;
}

CAntiAirTask::~CAntiAirTask()
{
}

bool CAntiAirTask::CanAssignTo(CCircuitUnit* unit) const
{
	if (!unit->GetCircuitDef()->IsRoleAA() ||
		(unit->GetCircuitDef() != leader->GetCircuitDef()))
	{
		return false;
	}
	const int frame = manager->GetCircuit()->GetLastFrame();
	if (frame < mergeFrame) {
		return false;  // split off a blocked regroup not long ago
	}
	if (leader->GetPos(frame).SqDistance2D(unit->GetPos(frame)) > SQUARE(1000.f)) {
		return false;
	}
	return true;
}

void CAntiAirTask::AssignTo(CCircuitUnit* unit)
{
	ISquadTask::AssignTo(unit);

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

void CAntiAirTask::RemoveAssignee(CCircuitUnit* unit)
{
	holdPending.erase(unit);
	splitPending.erase(unit);
	ISquadTask::RemoveAssignee(unit);
	if (leader == nullptr) {
		manager->AbortTask(this);
	}
}

void CAntiAirTask::Start(CCircuitUnit* unit)
{
	if ((State::REGROUP == state) || (State::ENGAGE == state)) {
		return;
	}
	if (!pPath->posPath.empty()) {
		unit->GetTravelAct()->SetPath(pPath);
		unit->GetTravelAct()->StateActivate();
	}
}

void CAntiAirTask::Update()
{
	++updCount;

	/*
	 * Check safety
	 */
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();

	FlushHolds();
	if (FlushSplits()) {
		return;  // task aborted: its last units left
	}

	if (CheckFlee()) {  // every update: run from approaching ground enemies before anything else
		return;
	}

	if (State::DISENGAGE == state) {
		if (updCount % 32 == 1) {
			const float maxDist = std::max<float>(lowestRange, circuit->GetPathfinder()->GetSquareSize());
			if (position.SqDistance2D(leader->GetPos(frame)) < SQUARE(maxDist)) {
				state = State::ROAM;
			} else {
				if (IsQueryReady(leader)) {
					FallbackDisengage();
				}
				return;
			}
		} else {
			return;
		}
	}

	/*
	 * Merge tasks if possible
	 */
	ISquadTask* task = (frame >= mergeFrame) ? GetMergeTask() : nullptr;
	if (task != nullptr) {
		task->Merge(this);
		units.clear();
		manager->AbortTask(this);
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
			// Half of IsMustRegroup's "far from the group" distance: anything it may call far gets a path back
			// (it counts only units that can move, so its distance can be smaller than one from units.size())
			const float sqNearDist = SQUARE(0.5f * std::max<float>(SQUARE_SIZE * 8 * units.size(), highestRange));
			for (CCircuitUnit* unit : units) {
				if ((unit == leader) || (groupPos.SqDistance2D(unit->GetPos(frame)) <= sqNearDist)) {
					Hold(unit);  // at the gathering point (the leader's spot) already
				} else if (IsQueryReady(unit)) {
					RegroupUnit(unit);
				}
			}
		}
		return;
	}

	bool isExecute = (updCount % 4 == 2);
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

	/*
	 * Update target
	 */
	FindTarget();

	const AIFloat3& startPos = leader->GetPos(frame);
	state = State::ROAM;
	if (target != nullptr) {
		const float sqRange = SQUARE(GetAirRange());
		if (position.SqDistance2D(startPos) < sqRange) {
			state = State::ENGAGE;
			retargetMove = GetAirRange() * 0.25f;  // a plane changes tile nearly every update: re-order only when it moved this far
			Attack(frame);
			return;
		}
	}

	if (!IsQueryReady(leader)) {
		return;
	}

	if (target == nullptr) {
		FallbackSafePos();
		return;
	}

	CPathFinder* pathfinder = circuit->GetPathfinder();
	std::shared_ptr<IPathQuery> query = pathfinder->CreatePathSingleQuery(
			leader, circuit->GetThreatMap(), frame,
			startPos, approachPos, pathfinder->GetSquareSize(), nullptr, GetPathThreatCap(leader, startPos, {approachPos}));
	pathQueries[leader] = query;
	query->HoldTask(this);

	pathfinder->RunQuery(query, [this](const IPathQuery* query) {
		if (this->IsQueryAlive(query)) {
			this->ApplyTargetPath(static_cast<const CQueryPathSingle*>(query));
		}
	});
}

void CAntiAirTask::OnUnitIdle(CCircuitUnit* unit)
{
	ISquadTask::OnUnitIdle(unit);
	if ((leader == nullptr) || (State::DISENGAGE == state)) {
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

void CAntiAirTask::OnUnitDamaged(CCircuitUnit* unit, CEnemyInfo* attacker)
{
	ISquadTask::OnUnitDamaged(unit, attacker);

	if ((leader == nullptr) || (State::DISENGAGE == state) ||
		((attacker != nullptr) && (attacker->GetCircuitDef() != nullptr) && attacker->GetCircuitDef()->IsAbleToFly()))
	{
		return;
	}

	if (!IsQueryReady(unit)) {
		return;
	}

	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	const AIFloat3& startPos = leader->GetPos(frame);
	circuit->GetMilitaryManager()->FillSafePos(leader, urgentPositions);
	const float pathRange = DEFAULT_SLACK * 4;

	CPathFinder* pathfinder = circuit->GetPathfinder();
	std::shared_ptr<IPathQuery> query = pathfinder->CreatePathMultiQuery(
			leader, circuit->GetThreatMap(), frame,
			startPos, pathRange, urgentPositions, nullptr, GetPathThreatCap(leader, startPos, urgentPositions));
	pathQueries[leader] = query;
	query->HoldTask(this);

	pathfinder->RunQuery(query, [this](const IPathQuery* query) {
		if (this->IsQueryAlive(query)) {
			this->ApplyDamagedPath(static_cast<const CQueryPathMulti*>(query));
		}
	});
}

void CAntiAirTask::FindTarget()
{
	CCircuitAI* circuit = manager->GetCircuit();
	CMilitaryManager* militaryMgr = circuit->GetMilitaryManager();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	CThreatMap* threatMap = circuit->GetThreatMap();
	CInfluenceMap* inflMap = circuit->GetInflMap();
	CMap* map = circuit->GetMap();
	const int frame = circuit->GetLastFrame();
	const AIFloat3& pos = leader->GetPos(frame);
	STerrainMapArea* area = leader->GetArea();
	CCircuitDef* cdef = leader->GetCircuitDef();
	const int canTargetCat = cdef->GetTargetCategory();
	const int noChaseCat = cdef->GetNoChaseCategory();
	const float maxPower = GetSafeThreat();  // health-scaled, same as firing spots; judged with GetAllowedThreatAt

	// antiair block of behaviour.json
	const CMilitaryManager::SAntiAirCfg& aaCfg = militaryMgr->GetAntiAirCfg();
	const float airRange = GetAirRange();
	const float sqRange = SQUARE(airRange);
	const bool isLeash = (aaCfg.leash >= 0.f);
	const float sqLeash = SQUARE(aaCfg.leash);
	const bool isFireSpot = (aaCfg.fireSpot >= 0.f);
	const float fireDist = airRange * aaCfg.fireSpot;
	// Home points: our attack and defend group leaders, and the base
	homePoints.clear();
	if (isLeash) {
		const std::array<IFighterTask::FightType, 2> types = {IFighterTask::FightType::ATTACK, IFighterTask::FightType::DEFEND};
		for (IFighterTask::FightType type : types) {
			for (IFighterTask* task : militaryMgr->GetTasks(type)) {
				homePoints.push_back(static_cast<ISquadTask*>(task)->GetLeaderPos(frame));
			}
		}
		homePoints.push_back(circuit->GetSetupManager()->GetBasePos());
	}

	CEnemyInfo* bestTarget = nullptr;
	float minSqDist = std::numeric_limits<float>::max();

	threatMap->SetThreatType(leader);
	const CCircuitAI::EnemyInfos& enemies = circuit->GetEnemyInfos();
	for (auto& kv : enemies) {
		CEnemyInfo* enemy = kv.second;
		if (enemy->IsHidden()) {
			continue;
		}
		const AIFloat3& ePos = enemy->GetPos();

		CCircuitDef* edef = enemy->GetCircuitDef();
		if (edef != nullptr) {
			if (((edef->GetCategory() & canTargetCat) == 0)
				|| ((edef->GetCategory() & noChaseCat) != 0))
			{
				continue;
			}
		} else if (ePos.y - GetBlipGroundHeight(map, ePos) < UNKNOWN_AIR_HEIGHT) {
			// Unknown radar blip: skips the category check above, so only one flying high enough
			// is taken for a plane; a ground raider's dot in our mex field is not an AA target
			continue;
		}

		const float sqDist = pos.SqDistance2D(ePos);
		if (sqDist >= minSqDist) {
			continue;
		}

		// Threat where the AA would stand to shoot, not under the aircraft
		AIFloat3 checkPos = ePos;
		bool isCoverSpot = true;  // checkPos may be judged for building cover
		if (isFireSpot) {
			if (sqDist <= sqRange) {
				checkPos = pos;
			} else {
				checkPos = ePos + (pos - ePos).Normalize2D() * fireDist;
				CTerrainManager::CorrectPosition(checkPos);
				if (!terrainMgr->CanMoveToPos(area, checkPos)) {
					checkPos = ePos;  // can't stand there: judge and approach as before
					isCoverSpot = false;  // but don't let cover under the plane stand in for the spot
				}
			}
		}

		// Leash: aircraft already in range is always fair game; otherwise it must be near home
		if (isLeash && (sqDist > sqRange)) {
			bool isNearHome = false;
			if (aaCfg.isOwnGround) {
				// Ground our buildings and static defences cover is ours, judged where the AA would stand.
				// The plain influence map counts armed units only, so a mex field or the back of the base
				// with no army nearby reads as neutral.
				isNearHome = isCoverSpot && (inflMap->GetAllyDefendInflAt(checkPos) > INFL_EPS);
				if (!isNearHome) {
					// Else our armed influence under the plane, without the squad's own share (else AA in enemy
					// land makes its surroundings "ours" and creeps forward one plane at a time), must be real
					// and beat the enemy's without the plane's own share. The plane's share is at most its
					// threat, and none for a plane with no ground range (a pure fighter adds no influence),
					// so it is taken off the enemy side only when it has one, clamped, never added to ours.
					// Without the range check a fighter's threat would erase a real enemy army under it.
					float allyInfl = inflMap->GetAllyInflAt(ePos);
					for (CCircuitUnit* u : units) {
						allyInfl -= inflMap->GetMobileArmedInflAt(u, ePos);
					}
					const float planeShare = (enemy->GetData()->GetRange(CCircuitDef::ThreatType::LAND) > 0) ? enemy->GetThreat() : 0.f;
					const float enemyInfl = std::max(0.f, inflMap->GetEnemyInflAt(ePos) - planeShare);
					isNearHome = (allyInfl > INFL_EPS) && (allyInfl - enemyInfl > INFL_EPS);
				}
			}
			for (const AIFloat3& hp : homePoints) {
				if (isNearHome) {
					break;
				}
				isNearHome = (hp.SqDistance2D(ePos) <= sqLeash);
			}
			if (!isNearHome) {
				continue;
			}
		}

		// Take off only the plane's own threat at checkPos, as the threat map drew it: none outside
		// its ground range (a pure fighter has none), so it can't erase real ground threat there
		float planeThreat = 0.f;
		const int planeRange = enemy->GetData()->GetRange(CCircuitDef::ThreatType::LAND);  // threat map cells
		if (planeRange > 0) {
			const float cells = checkPos.distance2D(ePos) / threatMap->GetSquareSize();
			if (cells <= planeRange) {
				const bool isConst = (edef != nullptr) && edef->IsAlwaysHit();
				planeThreat = isConst ? enemy->GetThreat() : enemy->GetThreat() * (1.f - 0.5f * cells / planeRange);
			}
		}
		const float threat = std::max(0.f, threatMap->GetThreatAt(checkPos) - planeThreat);
		if ((threat >= GetAllowedThreatAt(checkPos, maxPower)) ||
			!terrainMgr->CanMoveToPos(area, ePos))
		{
			continue;
		}

		minSqDist = sqDist;
		bestTarget = enemy;
		approachPos = checkPos;
	}

	SetTarget(bestTarget);
	if (bestTarget != nullptr) {
		position = target->GetPos();
	}
	// Return: target, startPos=leader->pos, endPos=position
}

void CAntiAirTask::FallbackDisengage()
{
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	const AIFloat3& startPos = leader->GetPos(frame);

	CPathFinder* pathfinder = circuit->GetPathfinder();
	std::shared_ptr<IPathQuery> query = pathfinder->CreatePathSingleQuery(
			leader, circuit->GetThreatMap(), frame,
			startPos, position, pathfinder->GetSquareSize(), nullptr, GetPathThreatCap(leader, startPos, {position}));
	pathQueries[leader] = query;
	query->HoldTask(this);

	pathfinder->RunQuery(query, [this](const IPathQuery* query) {
		if (this->IsQueryAlive(query)) {
			this->ApplyDisengagePath(static_cast<const CQueryPathSingle*>(query));
		}
	});
}

void CAntiAirTask::ApplyDisengagePath(const CQueryPathSingle* query)
{
	pPath = query->GetPathInfo();

	if (!pPath->path.empty()) {
		if (pPath->path.size() > 2) {
			ActivePath();
		}
		return;
	}

	// No path to the disengage spot: look for any safe spot along a threat-aware path
	state = State::ROAM;
	FallbackSafePos();
}

void CAntiAirTask::ApplyTargetPath(const CQueryPathSingle* query)
{
	pPath = query->GetPathInfo();

	if (!pPath->posPath.empty()) {
		ActivePath();
	} else {
		FallbackSafePos();  // was: fight-move straight at the target
	}
}

void CAntiAirTask::FallbackSafePos()
{
	CCircuitAI* circuit = manager->GetCircuit();
	circuit->GetMilitaryManager()->FillSafePos(leader, urgentPositions);
	if (urgentPositions.empty()) {
		FallbackCommPos();
		return;
	}

	const int frame = circuit->GetLastFrame();
	const AIFloat3& startPos = leader->GetPos(frame);
	const float pathRange = DEFAULT_SLACK * 4;

	CPathFinder* pathfinder = circuit->GetPathfinder();
	std::shared_ptr<IPathQuery> query = pathfinder->CreatePathMultiQuery(
			leader, circuit->GetThreatMap(), frame,
			startPos, pathRange, urgentPositions, nullptr, GetPathThreatCap(leader, startPos, urgentPositions));
	pathQueries[leader] = query;
	query->HoldTask(this);

	pathfinder->RunQuery(query, [this](const IPathQuery* query) {
		if (this->IsQueryAlive(query)) {
			this->ApplySafePos(static_cast<const CQueryPathMulti*>(query));
		}
	});
}

void CAntiAirTask::ApplySafePos(const CQueryPathMulti* query)
{
	pPath = query->GetPathInfo();

	if (!pPath->posPath.empty()) {
		position = pPath->posPath.back();
		ActivePath();
	} else {
		FallbackCommPos();
	}
}

void CAntiAirTask::FallbackCommPos()
{
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	CCircuitUnit* commander = circuit->GetSetupManager()->GetCommander();
	if ((commander == nullptr) ||
		!circuit->GetTerrainManager()->CanMoveToPos(leader->GetArea(), commander->GetPos(frame)))
	{
		HoldAll();
		return;
	}

	// Go to the commander along a threat-aware path (was: plain Guard order)
	const AIFloat3& startPos = leader->GetPos(frame);
	const float pathRange = DEFAULT_SLACK * 4;

	CPathFinder* pathfinder = circuit->GetPathfinder();
	std::shared_ptr<IPathQuery> query = pathfinder->CreatePathSingleQuery(
			leader, circuit->GetThreatMap(), frame,
			startPos, commander->GetPos(frame), pathRange, nullptr, GetPathThreatCap(leader, startPos, {commander->GetPos(frame)}));
	pathQueries[leader] = query;
	query->HoldTask(this);

	pathfinder->RunQuery(query, [this](const IPathQuery* query) {
		if (this->IsQueryAlive(query)) {
			this->ApplyFallbackPath(static_cast<const CQueryPathSingle*>(query), false, true);
		}
	});
}

void CAntiAirTask::FallbackBasePos(bool isCapped)
{
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	const AIFloat3& startPos = leader->GetPos(frame);
	const AIFloat3& basePos = circuit->GetSetupManager()->GetBasePos();
	const float pathRange = DEFAULT_SLACK * 4;

	CPathFinder* pathfinder = circuit->GetPathfinder();
	std::shared_ptr<IPathQuery> query = pathfinder->CreatePathSingleQuery(
			leader, circuit->GetThreatMap(), frame,
			startPos, basePos, pathRange, nullptr,
			isCapped ? GetPathThreatCap(leader, startPos, {basePos}) : std::numeric_limits<float>::max());
	pathQueries[leader] = query;
	query->HoldTask(this);

	pathfinder->RunQuery(query, [this, isCapped](const IPathQuery* query) {
		if (this->IsQueryAlive(query)) {
			this->ApplyFallbackPath(static_cast<const CQueryPathSingle*>(query), true, isCapped);
		}
	});
}

void CAntiAirTask::ApplyFallbackPath(const CQueryPathSingle* query, bool isDisengage, bool isCapped)
{
	pPath = query->GetPathInfo();

	if (!pPath->posPath.empty()) {
		position = pPath->posPath.back();
		ActivePath();
		if (isDisengage) {
			state = State::DISENGAGE;
		}
	} else if (isDisengage && isCapped) {
		FallbackBasePos(false);  // getting away matters more than the cap: last try through any threat
	} else {
		HoldAll();
	}
}

void CAntiAirTask::RegroupUnit(CCircuitUnit* unit)
{
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	const AIFloat3& startPos = unit->GetPos(frame);

	CPathFinder* pathfinder = circuit->GetPathfinder();
	std::shared_ptr<IPathQuery> query = pathfinder->CreatePathSingleQuery(
			unit, circuit->GetThreatMap(), frame,
			startPos, groupPos, DEFAULT_SLACK, nullptr, GetPathThreatCap(unit, startPos, {groupPos}));
	pathQueries[unit] = query;
	query->HoldTask(this);

	pathfinder->RunQuery(query, [this](const IPathQuery* query) {
		if (this->IsQueryAlive(query)) {
			this->ApplyRegroupPath(static_cast<const CQueryPathSingle*>(query));
		}
	});
}

void CAntiAirTask::ApplyRegroupPath(const CQueryPathSingle* query)
{
	CCircuitUnit* unit = query->GetUnit();
	if ((State::REGROUP != state) || (units.find(unit) == units.end())) {
		return;  // regroup ended while the path was computed
	}

	const std::shared_ptr<PathInfo>& path = query->GetPathInfo();
	if (!path->posPath.empty()) {
		unit->GetTravelAct()->SetPath(path);
		unit->GetTravelAct()->StateActivate();
	} else {
		// Can't rejoin without crossing threat: it would hold far away and keep the whole squad
		// in REGROUP (no targets) until the stuck-unit rule drops it. Give it its own task instead.
		Hold(unit);
		splitPending.insert(unit);
	}
}

bool CAntiAirTask::FlushSplits()
{
	if (splitPending.empty()) {
		return false;
	}
	CMilitaryManager* militaryMgr = manager->GetCircuit()->GetMilitaryManager();
	std::vector<CCircuitUnit*> leaving;
	for (CCircuitUnit* unit : splitPending) {
		if ((unit != leader) && (units.find(unit) != units.end())) {
			leaving.push_back(unit);
		}
	}
	splitPending.clear();
	const int noMergeUntil = manager->GetCircuit()->GetLastFrame() + SPLIT_MERGE_COOLDOWN;
	for (CCircuitUnit* unit : leaving) {
		CAntiAirTask* task = static_cast<CAntiAirTask*>(militaryMgr->EnqueueTask(IFighterTask::FightType::AA));
		task->mergeFrame = noMergeUntil;  // else it merges straight back (straight safe line) and splits again
		manager->AssignTask(unit, task);
	}
	return units.empty() || (leader == nullptr);
}

void CAntiAirTask::Hold(CCircuitUnit* unit)
{
	// No threat-aware way to go: stay put (weapons still fire at what comes in range).
	// The Stop order itself goes out from Update() (FlushHolds), with the squad's other orders.
	unit->GetTravelAct()->StateWait();
	holdPending.insert(unit);
}

void CAntiAirTask::FlushHolds()
{
	if (holdPending.empty()) {
		return;
	}
	CCircuitAI* circuit = manager->GetCircuit();
	for (CCircuitUnit* unit : holdPending) {
		if (units.find(unit) == units.end() || unit->GetTravelAct()->IsActive()) {
			continue;  // left the task, or got a path since
		}
		TRY_UNIT(circuit, unit,
			unit->GetUnit()->Stop();
		)
	}
	holdPending.clear();
}

float CAntiAirTask::GetAirRange() const
{
	// lowestRange is the longest range over all weapon kinds; AA targets planes, so use the air range
	// (squads hold one unit type, so the leader's is everyone's)
	const float airRange = (leader != nullptr) ? leader->GetCircuitDef()->GetMaxRange(CCircuitDef::RangeType::AIR) : 0.f;
	return (airRange > 0.f) ? airRange : lowestRange;
}

float CAntiAirTask::GetSafeThreat() const
{
	float power = 0.f;
	for (CCircuitUnit* unit : units) {
		power += unit->GetCircuitDef()->GetPower() * sqrtf(std::max(unit->GetHealthPercent(), 0.f));
	}
	return std::max(power, THREAT_MIN) * powerMod;
}

float CAntiAirTask::GetPathThreatCap(CCircuitUnit* unit, const AIFloat3& startPos, const F3Vec& goals) const
{
	// The pathfinder skips cells hotter than this. One value for the whole route: the squad's strength with
	// the strictest thr_mod.distance and the loosest base_bonus of start and goals (as attack groups do).
	// Never below the threat where the unit stands, so a squad already in danger can still walk out
	// through ground no hotter than here.
	if (unit->GetUnit()->IsCloaked()) {
		return std::numeric_limits<float>::max();  // the pathfinder uses the decloak-range array, a different scale
	}
	CCircuitAI* circuit = manager->GetCircuit();
	CMilitaryManager* militaryMgr = circuit->GetMilitaryManager();
	const AIFloat3& basePos = circuit->GetSetupManager()->GetBasePos();
	float curve = GetThrDistModAt(startPos);
	float bonus = militaryMgr->GetBaseThrBonus(basePos.SqDistance2D(startPos));
	for (const AIFloat3& g : goals) {
		curve = std::max(curve, GetThrDistModAt(g));
		bonus = std::max(bonus, militaryMgr->GetBaseThrBonus(basePos.SqDistance2D(g)));
	}
	const float cap = GetSafeThreat() * bonus / curve;
	return std::max(cap, circuit->GetThreatMap()->GetThreatAt(unit, startPos) + THREAT_MIN);
}

float CAntiAirTask::GetAllowedThreatAt(const AIFloat3& pos, float strength) const
{
	CCircuitAI* circuit = manager->GetCircuit();
	const AIFloat3& basePos = circuit->GetSetupManager()->GetBasePos();
	const float bonus = circuit->GetMilitaryManager()->GetBaseThrBonus(basePos.SqDistance2D(pos));
	return strength * bonus / GetThrDistModAt(pos);
}

void CAntiAirTask::HoldAll()
{
	pPath = std::make_shared<PathInfo>();  // else OnUnitIdle -> Start() would resume the old path after the Stop
	for (CCircuitUnit* unit : units) {
		Hold(unit);
	}
}

bool CAntiAirTask::CheckFlee()
{
	// antiair.flee_margin: flee when an armed mobile ground enemy we see now (or a moving low radar blip)
	// is within its weapon range + margin of any squad unit, unless (flee_cover) our other armed units hold
	// the ground at the leader. Returns true while fleeing
	CCircuitAI* circuit = manager->GetCircuit();
	const CMilitaryManager::SAntiAirCfg& aaCfg = circuit->GetMilitaryManager()->GetAntiAirCfg();
	CEnemyInfo* danger = nullptr;
	const int frame = circuit->GetLastFrame();

	// antiair.group_escape: next to an attack group, follow its call instead of judging alone. While it doesn't
	// escape the squad counts as covered (no own flee, so it doesn't run off and come back at the front); when
	// it escapes, run to the same spot
	CAttackTask* group = (leader != nullptr) ? FindEscortedGroup(frame) : nullptr;
	if (group != nullptr) {
		const AIFloat3& dest = group->GetFleeDest();
		if (group->IsEscaping(frame) && utils::is_valid(dest)) {
			if ((isFleeing && (frame < fleeFrame)) || !IsQueryReady(leader)) {
				return true;  // already on the way
			}
			isFleeing = true;
			fleeFrame = frame + FRAMES_PER_SEC * 2;  // re-plan every 2 s: the group may pick another spot
			FleeTo(dest);
			return true;
		}
	} else if ((aaCfg.fleeMargin >= 0.f) && (leader != nullptr)) {
		CMap* map = circuit->GetMap();
		float minSqDist = std::numeric_limits<float>::max();
		for (auto& kv : circuit->GetEnemyInfos()) {
			CEnemyInfo* enemy = kv.second;
			// Only enemies we see now (not last-known ghosts) that can act
			if (enemy->IsHidden() || !enemy->IsInRadarOrLOS()
				|| enemy->IsBeingBuilt() || enemy->IsParalyzed() || enemy->IsDisarmed())
			{
				continue;
			}
			const AIFloat3& ePos = enemy->GetPos();
			CCircuitDef* edef = enemy->GetCircuitDef();
			if (edef == nullptr) {
				// Unidentified radar blip: only a moving one on the ground (a high one is a plane, our target)
				if ((enemy->GetVel().SqLength2D() < 0.01f)
					|| (ePos.y - GetBlipGroundHeight(map, ePos) >= UNKNOWN_AIR_HEIGHT))
				{
					continue;
				}
			} else if (!edef->IsMobile() || !edef->IsAttacker() || edef->IsAbleToFly()
				|| !(edef->HasAntiLand() || edef->HasAntiWater()))
			{
				continue;  // enemy AA can't shoot the squad: no reason to run from it
			}
			// Its reach against the squad: ground (or water) weapon range, not its anti-air range
			const float eRange = (edef != nullptr)
					? std::max(edef->GetMaxRange(CCircuitDef::RangeType::LAND), edef->GetMaxRange(CCircuitDef::RangeType::WATER))
					: 0.f;
			const float sqReach = SQUARE(eRange + aaCfg.fleeMargin);
			for (CCircuitUnit* unit : units) {
				const float sqDist = unit->GetPos(frame).SqDistance2D(ePos);
				if ((sqDist <= sqReach) && (sqDist < minSqDist)) {
					minSqDist = sqDist;
					danger = enemy;
				}
			}
		}
		if ((danger != nullptr) && aaCfg.isFleeCover && IsFleeCovered(leader->GetPos(frame))) {
			danger = nullptr;
		}
	}

	if (danger == nullptr) {
		if (isFleeing) {
			isFleeing = false;
			pathQueries.erase(leader);  // drop a flee path still in flight: it must not send the squad off afterwards
			if (State::DISENGAGE == state) {
				state = State::ROAM;  // danger gone: back to normal work on this update
			}
		}
		return false;
	}

	if ((isFleeing && (frame < fleeFrame)) || !IsQueryReady(leader)) {
		return true;  // already on the way
	}
	isFleeing = true;
	fleeFrame = frame + FRAMES_PER_SEC * 2;  // re-plan the flee every 2 s while the danger stays
	Flee(danger->GetPos());
	return true;
}

bool CAntiAirTask::IsFleeCovered(const AIFloat3& pos) const
{
	// Our armed influence at pos without the squad's own share beats the enemy's: an army or defences stand here
	CInfluenceMap* inflMap = manager->GetCircuit()->GetInflMap();
	float allyInfl = inflMap->GetAllyInflAt(pos);
	for (CCircuitUnit* u : units) {
		allyInfl -= inflMap->GetMobileArmedInflAt(u, pos);
	}
	return (allyInfl > INFL_EPS) && (allyInfl > inflMap->GetEnemyInflAt(pos));
}

void CAntiAirTask::Flee(const AIFloat3& dangerPos)
{
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	const AIFloat3& startPos = leader->GetPos(frame);

	// Candidates: safe spots (attack/defend group leaders, else a defended mex cluster, else base) and the base
	circuit->GetMilitaryManager()->FillSafePos(leader, fleeSpots);
	const AIFloat3& basePos = circuit->GetSetupManager()->GetBasePos();
	if (circuit->GetTerrainManager()->CanMoveToPos(leader->GetArea(), basePos)) {
		fleeSpots.push_back(basePos);
	}

	// Nearest spot further from the danger than we are, else the one furthest from it
	const float sqDangerDist = startPos.SqDistance2D(dangerPos);
	const AIFloat3* best = nullptr;
	float bestSqDist = std::numeric_limits<float>::max();
	for (const AIFloat3& spot : fleeSpots) {
		const float sqDist = startPos.SqDistance2D(spot);
		if ((spot.SqDistance2D(dangerPos) > sqDangerDist) && (sqDist < bestSqDist)) {
			bestSqDist = sqDist;
			best = &spot;
		}
	}
	if (best == nullptr) {
		float maxSqDist = -1.f;
		for (const AIFloat3& spot : fleeSpots) {
			const float sqDist = spot.SqDistance2D(dangerPos);
			if (sqDist > maxSqDist) {
				maxSqDist = sqDist;
				best = &spot;
			}
		}
	}
	if (best == nullptr) {
		FallbackBasePos(true);
		return;
	}
	FleeTo(*best);
}

CAttackTask* CAntiAirTask::FindEscortedGroup(int frame) const
{
	const CMilitaryManager* militaryMgr = manager->GetCircuit()->GetMilitaryManager();
	const float reach = militaryMgr->GetAntiAirCfg().groupEscape;
	if (reach < 0.f) {
		return nullptr;
	}
	const AIFloat3& pos = leader->GetPos(frame);
	CAttackTask* best = nullptr;
	float minSqDist = SQUARE(reach);
	for (IFighterTask* task : militaryMgr->GetTasks(IFighterTask::FightType::ATTACK)) {
		CAttackTask* attack = static_cast<CAttackTask*>(task);
		if (attack->GetLeader() == nullptr) {
			continue;
		}
		const float sqDist = pos.SqDistance2D(attack->GetLeaderPos(frame));
		if (sqDist <= minSqDist) {
			minSqDist = sqDist;
			best = attack;
		}
	}
	return best;
}

void CAntiAirTask::FleeTo(const AIFloat3& goal)
{
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	const AIFloat3& startPos = leader->GetPos(frame);
	const float pathRange = DEFAULT_SLACK * 4;

	CPathFinder* pathfinder = circuit->GetPathfinder();
	std::shared_ptr<IPathQuery> query = pathfinder->CreatePathSingleQuery(
			leader, circuit->GetThreatMap(), frame,
			startPos, goal, pathRange, nullptr, GetPathThreatCap(leader, startPos, {goal}));
	pathQueries[leader] = query;
	query->HoldTask(this);

	pathfinder->RunQuery(query, [this](const IPathQuery* query) {
		if (this->IsQueryAlive(query)) {
			this->ApplyFleePath(static_cast<const CQueryPathSingle*>(query));
		}
	});
}

void CAntiAirTask::ApplyFleePath(const CQueryPathSingle* query)
{
	pPath = query->GetPathInfo();

	if (pPath->posPath.size() > 2) {
		position = pPath->posPath.back();
		ActivePath();
		state = State::DISENGAGE;
	} else if (!pPath->posPath.empty()) {
		HoldAll();  // already there; nowhere better to go
		state = State::DISENGAGE;
	} else {
		FallbackBasePos(true);  // no path under the cap: base, then (last resort) through any threat
	}
}

void CAntiAirTask::ApplyDamagedPath(const CQueryPathMulti* query)
{
	pPath = query->GetPathInfo();

	if (!pPath->posPath.empty()) {
		position = pPath->posPath.back();
		ActivePath();
		state = State::DISENGAGE;
	} else {
		FallbackBasePos(true);  // was: fight-move straight to base
	}
}

} // namespace circuit

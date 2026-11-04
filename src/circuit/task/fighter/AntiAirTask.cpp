/*
 * AntiAirTask.cpp
 *
 *  Created on: Jan 6, 2016
 *      Author: rlcevg
 */

#include "task/fighter/AntiAirTask.h"
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
#include <utility>

namespace circuit {

using namespace springai;

static constexpr float UNKNOWN_AIR_HEIGHT = 50.f;  // an unidentified blip this far above ground counts as a plane
static constexpr float BLIP_POS_ERROR = 96.f;  // engine default radar error size (ZK keeps it): sideways shift of a blip
static constexpr float FALLBACK_PATH_RANGE = DEFAULT_SLACK * 4;  // goal tolerance of safety, base, commander and flee paths
static const int SPLIT_MERGE_COOLDOWN = FRAMES_PER_SEC * 30;  // a unit split off a blocked regroup doesn't merge for this long
static const int FLEE_REPLAN_FRAMES = FRAMES_PER_SEC * 2;  // re-plan the flee this often while the danger stays

// Is an unidentified radar blip at p high enough above the ground (or water surface) to be a plane? The blip is
// shifted sideways by up to BLIP_POS_ERROR, so it must clear the highest ground around it, else a ground unit
// near a cliff reads as flying. Each read can only make the answer "no", so stop at the first that does: a blip
// near sea level needs no read, most ground blips one
static bool IsHighBlip(CMap* map, const AIFloat3& p)
{
	if (p.y < UNKNOWN_AIR_HEIGHT) {
		return false;  // not above the water surface: a ship or hover floats at 0 over a deep sea floor
	}
	const float maxX = CTerrainManager::GetTerrainWidth() - 1.f;
	const float maxZ = CTerrainManager::GetTerrainHeight() - 1.f;
	const float offsets[][2] = {{0.f, 0.f}, {1.f, 0.f}, {-1.f, 0.f}, {0.f, 1.f}, {0.f, -1.f},
								{0.7f, 0.7f}, {0.7f, -0.7f}, {-0.7f, 0.7f}, {-0.7f, -0.7f}};
	for (const auto& o : offsets) {
		const float x = std::min(std::max(p.x + o[0] * BLIP_POS_ERROR, 0.f), maxX);
		const float z = std::min(std::max(p.z + o[1] * BLIP_POS_ERROR, 0.f), maxZ);
		if (p.y - map->GetElevationAt(x, z) < UNKNOWN_AIR_HEIGHT) {
			return false;
		}
	}
	return true;
}

// Nearest spot further from the danger than pos is, else the one furthest from the danger; nullptr if none
static const AIFloat3* PickFleeSpot(const AIFloat3& pos, const AIFloat3& dangerPos, const F3Vec& spots)
{
	const float sqDangerDist = pos.SqDistance2D(dangerPos);
	const AIFloat3* best = nullptr;
	float bestSqDist = std::numeric_limits<float>::max();
	for (const AIFloat3& spot : spots) {
		const float sqDist = pos.SqDistance2D(spot);
		if ((spot.SqDistance2D(dangerPos) > sqDangerDist) && (sqDist < bestSqDist)) {
			bestSqDist = sqDist;
			best = &spot;
		}
	}
	if (best != nullptr) {
		return best;
	}
	float maxSqDist = -1.f;
	for (const AIFloat3& spot : spots) {
		const float sqDist = spot.SqDistance2D(dangerPos);
		if (sqDist > maxSqDist) {
			maxSqDist = sqDist;
			best = &spot;
		}
	}
	return best;
}

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

	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();

	FlushHolds();
	if (FlushSplits()) {
		return;  // task aborted: its last units left
	}

	if (CheckFlee()) {  // every update: run from approaching ground enemies before anything else
		return;
	}

	// Disengaging: keep walking until the spot is reached (checked every update, so the squad hunts again on
	// arrival), re-planning the path every 32 updates
	if (State::DISENGAGE == state) {
		const float maxDist = std::max<float>(lowestRange, circuit->GetPathfinder()->GetSquareSize());
		if (position.SqDistance2D(leader->GetPos(frame)) >= SQUARE(maxDist)) {
			if ((updCount % 32 == 1) && IsQueryReady(leader)) {
				FallbackDisengage();
			}
			return;
		}
		state = State::ROAM;
	}

	// Merge into a nearby squad of the same type
	ISquadTask* task = (frame >= mergeFrame) ? GetMergeTask() : nullptr;
	if (task != nullptr) {
		task->Merge(this);
		units.clear();
		manager->AbortTask(this);
		return;
	}

	// Regroup if required
	const bool wasRegroup = (State::REGROUP == state);
	const bool mustRegroup = IsMustRegroup();
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

	// Pick a target every 4th update, or sooner when a unit asks for it
	const bool isExecute = (updCount % 4 == 2)
			|| std::any_of(units.begin(), units.end(), [frame](CCircuitUnit* u) { return u->IsForceUpdate(frame); });
	if (!isExecute) {
		if (wasRegroup && !pPath->posPath.empty()) {
			ActivePath();
		}
		return;
	}

	FindTarget();

	state = State::ROAM;
	if ((target != nullptr) && (position.SqDistance2D(leader->GetPos(frame)) < SQUARE(GetAirRange()))) {
		state = State::ENGAGE;
		retargetMove = GetAirRange() * 0.25f;  // a plane changes tile nearly every update: re-order only when it moved this far
		Attack(frame);
		return;
	}

	if (!IsQueryReady(leader)) {
		return;
	}
	if (target == nullptr) {
		FallbackSafePos();
		return;
	}
	QueryPath(leader, approachPos, circuit->GetPathfinder()->GetSquareSize(), true,
			[this](const CQueryPathSingle* query) { ApplyTargetPath(query); });
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

	// The path is the leader's: one in flight (from an earlier hit, or a flee) is not replaced on every hit
	if (!IsQueryReady(leader)) {
		return;
	}
	manager->GetCircuit()->GetMilitaryManager()->FillSafePos(leader, urgentPositions);
	QueryPathMulti(urgentPositions, [this](const CQueryPathMulti* query) { ApplyDamagedPath(query); });
}

void CAntiAirTask::FindTarget()
{
	// The nearest aircraft that passes the leash and whose firing spot is safe enough. Candidates are judged
	// nearest first, so the costly checks stop at the first that passes
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	const AIFloat3& pos = leader->GetPos(frame);
	const int canTargetCat = leader->GetCircuitDef()->GetTargetCategory();
	const int noChaseCat = leader->GetCircuitDef()->GetNoChaseCategory();

	circuit->GetThreatMap()->SetThreatType(leader);  // shared threat-map state: set on every pick

	targetCandidates.clear();
	for (auto& kv : circuit->GetEnemyInfos()) {
		CEnemyInfo* enemy = kv.second;
		if (enemy->IsHidden()) {
			continue;
		}
		CCircuitDef* edef = enemy->GetCircuitDef();
		if ((edef != nullptr)
			&& (((edef->GetCategory() & canTargetCat) == 0) || ((edef->GetCategory() & noChaseCat) != 0)))
		{
			continue;
		}
		targetCandidates.emplace_back(pos.SqDistance2D(enemy->GetPos()), enemy);
	}
	// Equally near candidates in enemy id order (the order of GetEnemyInfos): the first wins, as before the sort
	std::sort(targetCandidates.begin(), targetCandidates.end(), [](const auto& a, const auto& b) {
		return (a.first < b.first) || ((a.first == b.first) && (a.second->GetId() < b.second->GetId()));
	});

	const CMilitaryManager::SAntiAirCfg& aaCfg = circuit->GetMilitaryManager()->GetAntiAirCfg();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	CThreatMap* threatMap = circuit->GetThreatMap();
	CMap* map = circuit->GetMap();
	STerrainMapArea* area = leader->GetArea();
	const float sqRange = SQUARE(GetAirRange());
	const bool isLeash = (aaCfg.leash >= 0.f);
	bool isPrepared = false;  // squad strength and home points wait for a candidate past the cheap checks
	float maxPower = 0.f;
	CEnemyInfo* bestTarget = nullptr;
	for (const auto& [sqDist, enemy] : targetCandidates) {
		const AIFloat3& ePos = enemy->GetPos();
		// Unknown radar blip: skipped the category check, so only one flying high enough is taken for
		// a plane; a ground raider's dot in our mex field is not an AA target
		if ((enemy->GetCircuitDef() == nullptr) && !IsHighBlip(map, ePos)) {
			continue;
		}
		if (!terrainMgr->CanMoveToPos(area, ePos)) {
			continue;
		}
		if (!isPrepared) {
			isPrepared = true;
			maxPower = GetSafeThreat();  // health-scaled, same as firing spots; judged with GetAllowedThreatAt
			if (isLeash) {
				FillHomePoints();
			}
		}
		bool isCoverSpot;  // checkPos may be judged for building cover
		const AIFloat3 checkPos = GetFireSpot(pos, ePos, sqDist, isCoverSpot);
		// Leash: aircraft already in range is always fair game; otherwise it must be near home
		if (isLeash && (sqDist > sqRange) && !IsNearHome(enemy, checkPos, isCoverSpot)) {
			continue;
		}
		// Threat where the AA would stand to shoot, without the plane's own share
		const float threat = std::max(0.f, threatMap->GetThreatAt(checkPos) - GetPlaneThreatAt(enemy, checkPos));
		if (threat >= GetAllowedThreatAt(checkPos, maxPower)) {
			continue;
		}
		bestTarget = enemy;
		approachPos = checkPos;
		break;
	}

	SetTarget(bestTarget);
	if (bestTarget != nullptr) {
		position = target->GetPos();
	}
}

void CAntiAirTask::FillHomePoints()
{
	// Leash anchors: our attack and defend group leaders, and the base
	CCircuitAI* circuit = manager->GetCircuit();
	CMilitaryManager* militaryMgr = circuit->GetMilitaryManager();
	const int frame = circuit->GetLastFrame();
	homePoints.clear();
	const std::array<IFighterTask::FightType, 2> types = {IFighterTask::FightType::ATTACK, IFighterTask::FightType::DEFEND};
	for (IFighterTask::FightType type : types) {
		for (IFighterTask* task : militaryMgr->GetTasks(type)) {
			homePoints.push_back(static_cast<ISquadTask*>(task)->GetLeaderPos(frame));
		}
	}
	homePoints.push_back(circuit->GetSetupManager()->GetBasePos());
}

AIFloat3 CAntiAirTask::GetFireSpot(const AIFloat3& pos, const AIFloat3& ePos, float sqDist, bool& isCoverSpot) const
{
	// antiair.fire_spot: where the squad would stand to shoot the aircraft at ePos. In range already: here.
	// Else fire_spot x air range short of it, towards the squad; where the squad can't stand, under the plane.
	isCoverSpot = true;
	CCircuitAI* circuit = manager->GetCircuit();
	const CMilitaryManager::SAntiAirCfg& aaCfg = circuit->GetMilitaryManager()->GetAntiAirCfg();
	if (aaCfg.fireSpot < 0.f) {
		return ePos;  // old check under the aircraft
	}
	const float airRange = GetAirRange();
	if (sqDist <= SQUARE(airRange)) {
		return pos;
	}
	AIFloat3 spot = ePos + (pos - ePos).Normalize2D() * (airRange * aaCfg.fireSpot);
	CTerrainManager::CorrectPosition(spot);
	if (!circuit->GetTerrainManager()->CanMoveToPos(leader->GetArea(), spot)) {
		isCoverSpot = false;  // judge and approach under the plane, but its building cover doesn't count
		return ePos;
	}
	return spot;
}

bool CAntiAirTask::IsNearHome(CEnemyInfo* enemy, const AIFloat3& checkPos, bool isCoverSpot) const
{
	// antiair.leash: the aircraft is within leash of a home point (FillHomePoints), or (own_ground) over our ground
	const CMilitaryManager::SAntiAirCfg& aaCfg = manager->GetCircuit()->GetMilitaryManager()->GetAntiAirCfg();
	const AIFloat3& ePos = enemy->GetPos();
	const float sqLeash = SQUARE(aaCfg.leash);
	for (const AIFloat3& hp : homePoints) {
		if (hp.SqDistance2D(ePos) <= sqLeash) {
			return true;
		}
	}
	if (!aaCfg.isOwnGround) {
		return false;
	}

	// Ground our buildings and static defences cover is ours, judged where the AA would stand.
	// The plain influence map counts armed units only, so a mex field or the back of the base
	// with no army nearby reads as neutral.
	CInfluenceMap* inflMap = manager->GetCircuit()->GetInflMap();
	if (isCoverSpot && (inflMap->GetAllyDefendInflAt(checkPos) > INFL_EPS)) {
		return true;
	}
	// Else our armed influence under the plane, without the squad's own share (else AA in enemy land makes
	// its surroundings "ours" and creeps forward one plane at a time), must be real and beat the enemy's
	// without the plane's own share. The plane's share is at most its threat, and none for a plane with no
	// ground range (a pure fighter adds no influence), so it is taken off the enemy side only when it has one,
	// clamped, never added to ours. Without the range check a fighter's threat would erase a real enemy army.
	float allyInfl = inflMap->GetAllyInflAt(ePos);
	for (CCircuitUnit* u : units) {
		allyInfl -= inflMap->GetMobileArmedInflAt(u, ePos);
	}
	const float planeShare = (enemy->GetData()->GetRange(CCircuitDef::ThreatType::LAND) > 0) ? enemy->GetThreat() : 0.f;
	const float enemyInfl = std::max(0.f, inflMap->GetEnemyInflAt(ePos) - planeShare);
	return (allyInfl > INFL_EPS) && (allyInfl - enemyInfl > INFL_EPS);
}

float CAntiAirTask::GetPlaneThreatAt(CEnemyInfo* enemy, const AIFloat3& pos) const
{
	// The plane's own threat at pos, as the threat map drew it: none outside its ground range (a pure fighter
	// has none), so taking it off can't erase real ground threat there
	const int planeRange = enemy->GetData()->GetRange(CCircuitDef::ThreatType::LAND);  // threat map cells
	if (planeRange <= 0) {
		return 0.f;
	}
	const float cells = pos.distance2D(enemy->GetPos()) / manager->GetCircuit()->GetThreatMap()->GetSquareSize();
	if (cells > planeRange) {
		return 0.f;
	}
	CCircuitDef* edef = enemy->GetCircuitDef();
	const bool isConst = (edef != nullptr) && edef->IsAlwaysHit();
	return isConst ? enemy->GetThreat() : enemy->GetThreat() * (1.f - 0.5f * cells / planeRange);
}

void CAntiAirTask::QueryPath(CCircuitUnit* unit, const AIFloat3& goal, float range, bool isCapped,
		std::function<void (const CQueryPathSingle*)>&& apply)
{
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	const AIFloat3& startPos = unit->GetPos(frame);
	const float maxThreat = isCapped ? GetPathThreatCap(unit, startPos, {goal}) : std::numeric_limits<float>::max();

	CPathFinder* pathfinder = circuit->GetPathfinder();
	std::shared_ptr<IPathQuery> query = pathfinder->CreatePathSingleQuery(
			unit, circuit->GetThreatMap(), frame, startPos, goal, range, nullptr, maxThreat);
	pathQueries[unit] = query;
	query->HoldTask(this);

	pathfinder->RunQuery(query, [this, apply = std::move(apply)](const IPathQuery* query) {
		if (this->IsQueryAlive(query)) {
			apply(static_cast<const CQueryPathSingle*>(query));
		}
	});
}

void CAntiAirTask::QueryPathMulti(const F3Vec& goals, std::function<void (const CQueryPathMulti*)>&& apply)
{
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	const AIFloat3& startPos = leader->GetPos(frame);

	CPathFinder* pathfinder = circuit->GetPathfinder();
	std::shared_ptr<IPathQuery> query = pathfinder->CreatePathMultiQuery(
			leader, circuit->GetThreatMap(), frame, startPos, FALLBACK_PATH_RANGE, goals, nullptr,
			GetPathThreatCap(leader, startPos, goals));
	pathQueries[leader] = query;
	query->HoldTask(this);

	pathfinder->RunQuery(query, [this, apply = std::move(apply)](const IPathQuery* query) {
		if (this->IsQueryAlive(query)) {
			apply(static_cast<const CQueryPathMulti*>(query));
		}
	});
}

void CAntiAirTask::FallbackDisengage()
{
	QueryPath(leader, position, manager->GetCircuit()->GetPathfinder()->GetSquareSize(), true,
			[this](const CQueryPathSingle* query) { ApplyDisengagePath(query); });
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
		FallbackSafePos();
	}
}

void CAntiAirTask::FallbackSafePos()
{
	manager->GetCircuit()->GetMilitaryManager()->FillSafePos(leader, urgentPositions);
	if (urgentPositions.empty()) {
		FallbackCommPos();
		return;
	}
	QueryPathMulti(urgentPositions, [this](const CQueryPathMulti* query) { ApplySafePos(query); });
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
	// Go to the commander along a threat-aware path
	CCircuitAI* circuit = manager->GetCircuit();
	CCircuitUnit* commander = circuit->GetSetupManager()->GetCommander();
	if ((commander == nullptr) ||
		!circuit->GetTerrainManager()->CanMoveToPos(leader->GetArea(), commander->GetPos(circuit->GetLastFrame())))
	{
		HoldAll();
		return;
	}
	QueryPath(leader, commander->GetPos(circuit->GetLastFrame()), FALLBACK_PATH_RANGE, true,
			[this](const CQueryPathSingle* query) { ApplyFallbackPath(query, false, true); });
}

void CAntiAirTask::FallbackBasePos(bool isCapped)
{
	QueryPath(leader, manager->GetCircuit()->GetSetupManager()->GetBasePos(), FALLBACK_PATH_RANGE, isCapped,
			[this, isCapped](const CQueryPathSingle* query) { ApplyFallbackPath(query, true, isCapped); });
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
	QueryPath(unit, groupPos, DEFAULT_SLACK, true,
			[this](const CQueryPathSingle* query) { ApplyRegroupPath(query); });
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
	return std::max(GetStrengthSum(), THREAT_MIN) * powerMod;
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
	// antiair.flee_margin: flee from ground enemies in reach (FindFleeDanger), unless (flee_ally_threat)
	// our units next to the leader outweigh them by enough. Returns true while fleeing
	CCircuitAI* circuit = manager->GetCircuit();
	const CMilitaryManager::SAntiAirCfg& aaCfg = circuit->GetMilitaryManager()->GetAntiAirCfg();
	const int frame = circuit->GetLastFrame();
	float dangerThreat = 0.f;
	CEnemyInfo* danger = ((aaCfg.fleeMargin >= 0.f) && (leader != nullptr)) ? FindFleeDanger(dangerThreat) : nullptr;
	if ((danger != nullptr) && (aaCfg.fleeAllyThreat >= 0.f) && IsFleeCovered(leader->GetPos(frame), dangerThreat)) {
		danger = nullptr;
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
	fleeFrame = frame + FLEE_REPLAN_FRAMES;
	Flee(danger->GetPos());
	return true;
}

CEnemyInfo* CAntiAirTask::FindFleeDanger(float& dangerThreat) const
{
	// The nearest enemy we see now (not a last-known ghost) that can act and shoot the squad, within its ground
	// (or water) weapon range + flee_margin of any squad unit (for a mobile bomb, its blast radius). Enemy AA and
	// aircraft don't count; an unidentified blip counts while it moves and sits low. dangerThreat: the threat of
	// every such enemy, each counted once
	CCircuitAI* circuit = manager->GetCircuit();
	const float fleeMargin = circuit->GetMilitaryManager()->GetAntiAirCfg().fleeMargin;
	const int frame = circuit->GetLastFrame();
	const AIFloat3& leaderPos = leader->GetPos(frame);
	float squadRadius = 0.f;  // no unit is further than this from the leader
	for (CCircuitUnit* unit : units) {
		squadRadius = std::max(squadRadius, leaderPos.distance2D(unit->GetPos(frame)));
	}

	CEnemyInfo* danger = nullptr;
	float minSqDist = std::numeric_limits<float>::max();
	dangerThreat = 0.f;
	for (auto& kv : circuit->GetEnemyInfos()) {
		CEnemyInfo* enemy = kv.second;
		if (enemy->IsHidden() || !enemy->IsInRadarOrLOS()
			|| enemy->IsBeingBuilt() || enemy->IsParalyzed() || enemy->IsDisarmed())
		{
			continue;
		}
		CCircuitDef* edef = enemy->GetCircuitDef();
		float range = 0.f;  // unidentified blip: the margin only
		if (edef == nullptr) {
			if (enemy->GetVel().SqLength2D() < 0.01f) {
				continue;  // a parked blip is no danger
			}
		} else if (!edef->IsMobile() || !edef->IsAttacker() || edef->IsAbleToFly()
			|| (edef->HasAntiAir() && !edef->HasAntiLand() && !edef->HasAntiWater()))
		{
			// Enemy AA can't shoot the squad: no reason to run from it. A mobile bomb (no weapon, only its
			// death explosion) has no anti-land flag either, but it is a danger
			continue;
		} else {
			range = std::max(edef->GetMaxRange(CCircuitDef::RangeType::LAND), edef->GetMaxRange(CCircuitDef::RangeType::WATER));
		}
		const AIFloat3& ePos = enemy->GetPos();
		const float reach = range + fleeMargin;
		if (leaderPos.SqDistance2D(ePos) > SQUARE(reach + squadRadius)) {
			continue;  // out of reach of every unit
		}
		float sqDist = std::numeric_limits<float>::max();  // to the nearest squad unit
		for (CCircuitUnit* unit : units) {
			sqDist = std::min(sqDist, unit->GetPos(frame).SqDistance2D(ePos));
		}
		if ((sqDist > SQUARE(reach)) || ((edef == nullptr) && IsHighBlip(circuit->GetMap(), ePos))) {
			continue;  // out of reach, or a high blip: a plane, our target
		}
		dangerThreat += enemy->GetThreat();
		if (sqDist < minSqDist) {
			minSqDist = sqDist;
			danger = enemy;
		}
	}
	return danger;
}

bool CAntiAirTask::IsFleeCovered(const AIFloat3& pos, float enemyThreat) const
{
	// Net escort power next to the squad: our and allied armed units within flee_ally_radius (as
	// CArtilleryTask::HasCover; mobile ones but not AA (main role) or aircraft, so AA squads can't cover each
	// other, plus static defences with a ground/water threat range, so not static AA or superweapons; each
	// unit's power with its pwr_mod, scaled by sqrt(health %) as squad strength is) minus enemyThreat (the
	// enemies that set off the flee) must reach flee_ally_threat. When the army leaves or dies, or the enemy
	// outgrows it, the cover goes.
	CCircuitAI* circuit = manager->GetCircuit();
	const CMilitaryManager::SAntiAirCfg& aaCfg = circuit->GetMilitaryManager()->GetAntiAirCfg();
	const int frame = circuit->GetLastFrame();
	const float sqRadius = SQUARE(aaCfg.fleeAllyRadius);
	circuit->UpdateFriendlyUnits();  // drop units that died since the last refresh
	float cover = 0.f;
	for (auto& kv : circuit->GetFriendlyUnits()) {
		CAllyUnit* ally = kv.second;
		CCircuitDef* adef = ally->GetCircuitDef();
		if ((adef == nullptr) || !adef->IsAttacker() || adef->IsAbleToFly()
			|| (adef->IsMobile() ? (adef->GetMainRole() == ROLE_TYPE(AA)) : !IsStaticDefence(adef)))
		{
			continue;
		}
		const AIFloat3& allyPos = ally->GetPos(frame);
		if (utils::is_valid(allyPos) && (allyPos.SqDistance2D(pos) <= sqRadius)) {
			cover += GetCoverPower(ally);
			if (cover - enemyThreat >= aaCfg.fleeAllyThreat) {
				return true;
			}
		}
	}
	return false;
}

void CAntiAirTask::Flee(const AIFloat3& dangerPos)
{
	// Candidates: safe spots (attack/defend group leaders, else a defended mex cluster, else base) and the base
	CCircuitAI* circuit = manager->GetCircuit();
	circuit->GetMilitaryManager()->FillSafePos(leader, fleeSpots);
	const AIFloat3& basePos = circuit->GetSetupManager()->GetBasePos();
	if (circuit->GetTerrainManager()->CanMoveToPos(leader->GetArea(), basePos)) {
		fleeSpots.push_back(basePos);
	}

	const AIFloat3* goal = PickFleeSpot(leader->GetPos(circuit->GetLastFrame()), dangerPos, fleeSpots);
	if (goal == nullptr) {
		FallbackBasePos(true);
		return;
	}
	QueryPath(leader, *goal, FALLBACK_PATH_RANGE, true, [this](const CQueryPathSingle* query) { ApplyFleePath(query); });
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
		FallbackBasePos(true);
	}
}

} // namespace circuit

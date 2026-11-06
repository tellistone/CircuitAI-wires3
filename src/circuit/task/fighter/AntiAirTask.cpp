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

	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();

	FlushHolds();
	if (FlushSplits()) {
		return;  // task aborted: its last units left
	}

	if (CheckFlee()) {  // every update: run from approaching ground enemies before anything else
		return;
	}
	UpdateScoots(frame);  // reload_scoot: step out of reach right after a shot

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

	// A scooting leader may stand beyond air range of the target: keep the fight until it's back, else the squad
	// would be routed in again while it reloads
	const bool isLeaderScoot = (State::ENGAGE == state) && (target != nullptr) && IsScooting(leader);
	if (!isLeaderScoot) {
		FindTarget();
	}

	state = State::ROAM;
	if ((target != nullptr)
		&& (isLeaderScoot || (position.SqDistance2D(leader->GetPos(frame)) < SQUARE(GetAirRange()))))
	{
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
	QueryPathMulti(urgentPositions, FALLBACK_PATH_RANGE, [this](const CQueryPathMulti* query) { ApplyDamagedPath(query); });
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
	QueryPathMulti(urgentPositions, FALLBACK_PATH_RANGE, [this](const CQueryPathMulti* query) { ApplySafePos(query); });
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
	CEnemyInfo* danger = ((aaCfg.fleeMargin >= 0.f) && (leader != nullptr)) ? FindFleeDanger(aaCfg.fleeMargin, false, dangerThreat) : nullptr;
	if ((danger != nullptr) && (aaCfg.fleeAllyThreat >= 0.f) && IsFleeCovered(leader->GetPos(frame), dangerThreat,
			aaCfg.fleeAllyThreat, aaCfg.fleeAllyRadius, ROLE_TYPE(AA)))
	{
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

void CAntiAirTask::Flee(const AIFloat3& dangerPos)
{
	// Candidates: safe spots (attack/defend group leaders, else a defended mex cluster, else base) and the base
	const AIFloat3* goal = FindFleeGoal(dangerPos);
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

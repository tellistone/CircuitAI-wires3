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

	/*
	 * Check safety
	 */
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();

	FlushHolds();
	if (FlushSplits()) {
		return;  // task aborted: its last units left
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
	if (State::REGROUP == state) {
		if (mustRegroup) {
			// Same "far from the group" distance as IsMustRegroup: only those need a path back
			const float sqNearDist = SQUARE(std::max<float>(SQUARE_SIZE * 8 * units.size(), highestRange));
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
		}

		const float sqDist = pos.SqDistance2D(ePos);
		if (sqDist >= minSqDist) {
			continue;
		}

		// Leash: aircraft already in range is always fair game; otherwise it must be near home
		if (isLeash && (sqDist > sqRange)) {
			bool isNearHome = false;
			if (aaCfg.isOwnGround) {
				// Our influence there without the squad's own share: else AA in enemy land makes its
				// surroundings "ours" and creeps forward one plane at a time
				float infl = inflMap->GetInfluenceAt(ePos);
				for (CCircuitUnit* u : units) {
					infl -= inflMap->GetMobileArmedInflAt(u, ePos);
				}
				isNearHome = (infl > INFL_EPS);
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

		// Threat where the AA would stand to shoot, not under the aircraft
		AIFloat3 checkPos = ePos;
		if (isFireSpot) {
			if (sqDist <= sqRange) {
				checkPos = pos;
			} else {
				checkPos = ePos + (pos - ePos).Normalize2D() * fireDist;
				CTerrainManager::CorrectPosition(checkPos);
				if (!terrainMgr->CanMoveToPos(area, checkPos)) {
					checkPos = ePos;  // can't stand there: judge and approach as before
				}
			}
		}
		const float threat = std::max(0.f, threatMap->GetThreatAt(checkPos) - enemy->GetThreat());
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

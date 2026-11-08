/*
 * AntiHeavyTask.cpp
 *
 *  Created on: Jun 30, 2016
 *      Author: rlcevg
 */

#include "task/fighter/AntiHeavyTask.h"
#include "task/TaskManager.h"
#include "map/ThreatMap.h"
#include "module/MilitaryManager.h"
#include "setup/SetupManager.h"
#include "terrain/TerrainManager.h"
#include "terrain/path/PathFinder.h"
#include "terrain/path/QueryPathSingle.h"
#include "terrain/path/QueryPathMulti.h"
#include "unit/action/DGunAction.h"
#include "unit/action/FightAction.h"
#include "unit/action/MoveAction.h"
#include "unit/enemy/EnemyUnit.h"
#include "unit/CircuitUnit.h"
#include "CircuitAI.h"
#include "util/Utils.h"

#include "spring/SpringMap.h"

#include "AISCommands.h"

#include <limits>

namespace circuit {

using namespace springai;

std::vector<CAntiHeavyTask::SDanger> CAntiHeavyTask::dangers;

CAntiHeavyTask::CAntiHeavyTask(ITaskManager* mgr, float powerMod)
		: ISquadTask(mgr, FightType::AH, powerMod)
{
	CCircuitAI* circuit = manager->GetCircuit();
	float x = rand() % circuit->GetTerrainManager()->GetTerrainWidth();
	float z = rand() % circuit->GetTerrainManager()->GetTerrainHeight();
	position = AIFloat3(x, circuit->GetMap()->GetElevationAt(x, z), z);
}

CAntiHeavyTask::~CAntiHeavyTask()
{
}

bool CAntiHeavyTask::CanAssignTo(CCircuitUnit* unit) const
{
	if (!unit->GetCircuitDef()->IsRoleAH() ||
		(unit->GetCircuitDef() != leader->GetCircuitDef()))
	{
		return false;
	}
	if (!IsRoomFor(1)) {
		return false;  // full: antiheavy_max_group
	}
	const int frame = manager->GetCircuit()->GetLastFrame();
	if (leader->GetPos(frame).SqDistance2D(unit->GetPos(frame)) > SQUARE(1000.f)) {
		return false;
	}
	return true;
}

bool CAntiHeavyTask::IsRoomFor(size_t count) const
{
	const int maxGroup = (leader != nullptr) ? leader->GetCircuitDef()->GetAhMaxGroup() : 0;
	return (maxGroup <= 0) || (units.size() + count <= (size_t)maxGroup);
}

void CAntiHeavyTask::AssignTo(CCircuitUnit* unit)
{
	ISquadTask::AssignTo(unit);
	CCircuitDef* cdef = unit->GetCircuitDef();
	highestRange = std::max(highestRange, cdef->GetLosRadius());
	highestRange = std::max(highestRange, cdef->GetJumpRange());

	CCircuitAI* circuit = manager->GetCircuit();
	if (cdef->IsAbleToCloak() && !cdef->IsOpenFire()) {
		TRY_UNIT(circuit, unit,
			unit->GetUnit()->SetFireState(CCircuitDef::FireType::RETURN);
		)
	}

	if (unit->GetDGunAct() != nullptr) {
		unit->GetDGunAct()->StateWait();
	}

	int squareSize = circuit->GetPathfinder()->GetSquareSize();
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

void CAntiHeavyTask::RemoveAssignee(CCircuitUnit* unit)
{
	ISquadTask::RemoveAssignee(unit);
	if (leader == nullptr) {
		manager->AbortTask(this);
	} else {
		highestRange = std::max(highestRange, leader->GetCircuitDef()->GetLosRadius());
		highestRange = std::max(highestRange, leader->GetCircuitDef()->GetJumpRange());
	}

	CCircuitDef* cdef = unit->GetCircuitDef();
	if (cdef->IsAbleToCloak()) {
		TRY_UNIT(manager->GetCircuit(), unit,
			unit->GetUnit()->SetFireState(cdef->GetFireState());
		)
	}
}

void CAntiHeavyTask::Start(CCircuitUnit* unit)
{
	if ((State::REGROUP == state) || (State::ENGAGE == state)) {
		return;
	}
	if (!pPath->posPath.empty()) {
		unit->GetTravelAct()->SetPath(pPath);
		unit->GetTravelAct()->StateActivate();
	}
}

void CAntiHeavyTask::Update()
{
	++updCount;

	/*
	 * Drop task once no enemy heavy has been seen for a while (mines: once none is known, as before)
	 */
	if (updCount % 32 == 1) {
		CCircuitAI* circuit = manager->GetCircuit();
		const bool isGone = ((leader != nullptr) && leader->GetCircuitDef()->IsRoleMine())
				? (circuit->GetEnemyManager()->GetEnemyCost(ROLE_TYPE(HEAVY)) < 1.f)
				: !circuit->GetMilitaryManager()->IsEnemyHeavySeen(CMilitaryManager::AH_KEEP_FRAMES);
		if (isGone) {
			manager->AbortTask(this);
			return;
		}
	}

	/*
	 * Flee from approaching enemies before anything else (every update)
	 */
	if (CheckFlee()) {
		return;
	}

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
			CCircuitAI* circuit = manager->GetCircuit();
			int frame = circuit->GetLastFrame() + FRAMES_PER_SEC * 60;
			for (CCircuitUnit* unit : units) {
				TRY_UNIT(circuit, unit,
					unit->GetUnit()->Fight(groupPos, UNIT_COMMAND_OPTION_RIGHT_MOUSE_KEY, frame);
				)
				unit->GetTravelAct()->StateWait();
			}
		}
		return;
	}

	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	UpdateScoots(frame);  // every update: step out of reach right after a shot

	bool isExecute = (updCount % 2 == 0) && (frame >= lastTouched + FRAMES_PER_SEC);
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
	if ((GetFleeMargin() < 0.f) && IsReloading(frame)) {
		if (IsQueryReady(leader)) {
			FallbackAttackSafe();  // no flee: old blind pull-back to the attack groups
		}
		return;
	}

	// A scooting leader stands past the stand-off distance FindTarget engages within: keep the fight (and the target)
	// until it's back, else the whole squad would be routed in again while it reloads
	const bool isLeaderScoot = (State::ENGAGE == state) && (target != nullptr) && IsScooting(leader);
	const bool isTargetsFound = isLeaderScoot || FindTarget();

	state = State::ROAM;
	if (target != nullptr) {
		state = State::ENGAGE;
		position = target->GetPos();
		float power = 0.f;
		for (CCircuitUnit* unit : units) {
			if (!IsScooting(unit)) {  // a scooting unit comes back once its weapon is ready
				AttackTarget(unit, frame, power);
			}
		}
		return;
	}

	if (!IsQueryReady(leader)) {
		return;
	}

	if (!isTargetsFound) {  // enemyPositions.empty()
		FallbackTargetEmpty();
		return;
	}

	QueryPathMulti(enemyPositions, circuit->GetPathfinder()->GetSquareSize(),
			[this](const CQueryPathMulti* query) { ApplyTargetPath(query); });
}

void CAntiHeavyTask::OnUnitIdle(CCircuitUnit* unit)
{
	ISquadTask::OnUnitIdle(unit);
	if (units.empty() || (State::DISENGAGE == state)) {
		return;  // fleeing: CheckFlee re-plans
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

void CAntiHeavyTask::OnUnitDamaged(CCircuitUnit* unit, CEnemyInfo* attacker)
{
	ISquadTask::OnUnitDamaged(unit, attacker);

	if (attacker != nullptr) {
		if (unit->GetCircuitDef()->IsAbleToCloak()) {
			TRY_UNIT(manager->GetCircuit(), unit,
				unit->GetUnit()->SetFireState(CCircuitDef::FireType::OPEN);
			)
		}
		if (unit->GetDGunAct() != nullptr) {
			unit->GetDGunAct()->StateActivate();
		}
	}
}

const CMilitaryManager::SAntiHeavyCfg& CAntiHeavyTask::GetCfg() const
{
	return manager->GetCircuit()->GetMilitaryManager()->GetAntiHeavyCfg();
}

float CAntiHeavyTask::GetPathThreatMod() const
{
	if ((leader != nullptr) && leader->GetCircuitDef()->IsAbleToFly()) {
		return -1.f;  // aircraft: the cap was never tuned for the air threat layer and their small strength
	}
	return GetCfg().pathThreat;
}

float CAntiHeavyTask::GetFleeMargin() const
{
	if (leader == nullptr) {
		return -1.f;
	}
	CCircuitDef* cdef = leader->GetCircuitDef();
	if (cdef->IsRoleMine() || cdef->IsAbleToFly()) {
		return -1.f;  // mines hide (cloak) instead; aircraft don't walk the ground paths the flee plans
	}
	return cdef->GetAhFleeMargin(GetCfg().fleeMargin);
}

bool CAntiHeavyTask::IsReloading(int frame) const
{
	CCircuitDef* cdef = leader->GetCircuitDef();
	return !cdef->IsRoleMine() && (cdef->GetReloadTime() >= FRAMES_PER_SEC * 5) && !leader->IsWeaponReady(frame);
}

float CAntiHeavyTask::GetScootMargin(CCircuitUnit* unit) const
{
	CCircuitDef* cdef = unit->GetCircuitDef();
	if (cdef->IsRoleMine() || cdef->IsAbleToFly()) {
		return -1.f;
	}
	const float fleeMargin = GetFleeMargin();
	if (cdef->IsReloadScoot()) {
		return cdef->GetReloadScootMargin((fleeMargin < 0.f) ? DEFAULT_SCOOT_MARGIN : fleeMargin);
	}
	return fleeMargin;  // flee off (< 0): no scoot, the old leader pull-back instead
}

bool CAntiHeavyTask::IsUnitReloading(CCircuitUnit* unit, int frame) const
{
	CCircuitDef* cdef = unit->GetCircuitDef();
	if (cdef->IsReloadScoot()) {
		return ISquadTask::IsUnitReloading(unit, frame);
	}
	return (cdef->GetReloadTime() >= FRAMES_PER_SEC * 5) && unit->HasWeapon() && !unit->IsWeaponReady(frame);
}

void CAntiHeavyTask::OnScootEnd(CCircuitUnit* unit, int frame, bool isEngaged)
{
	if (!isEngaged) {
		ISquadTask::OnScootEnd(unit, frame, isEngaged);  // back on the squad's path (else it waits for the next one)
		return;
	}
	float minePower = 0.f;  // mines never scoot
	AttackTarget(unit, frame, minePower);
}

bool CAntiHeavyTask::AttackTarget(CCircuitUnit* unit, int frame, float& minePower)
{
	if (unit->Blocker() != nullptr) {
		return false;
	}
	if (unit->GetDGunAct() != nullptr) {
		unit->GetDGunAct()->StateActivate();
	}
	unit->GetTravelAct()->StateWait();

	if (unit->GetCircuitDef()->IsRoleMine()) {
		const bool isAttack = (target->GetThreat() > minePower);
		minePower += unit->GetCircuitDef()->GetPower();
		if (!isAttack) {
			return false;
		}
	}
	if (target->GetUnit()->IsCloaked()) {
		unit->Attack(target->GetPos(), target, frame + FRAMES_PER_SEC * 60);
	} else {
		unit->Attack(target, frame + FRAMES_PER_SEC * 60);
	}
	return true;
}

bool CAntiHeavyTask::IsQuarry(CEnemyInfo* enemy) const
{
	CCircuitDef* edef = enemy->GetCircuitDef();
	if ((edef == nullptr) || !edef->IsEnemyRoleAny(CCircuitDef::RoleMask::HEAVY | CCircuitDef::RoleMask::COMM)) {
		return false;
	}
	CCircuitDef* cdef = leader->GetCircuitDef();
	return !(cdef->IsAttrAntiStat() && edef->IsMobile())
		&& ((edef->GetCategory() & cdef->GetTargetCategory()) != 0)
		&& !(edef->IsAbleToFly() && !cdef->HasAntiAir());
}

bool CAntiHeavyTask::IsFleeIgnored(CEnemyInfo* enemy) const
{
	return (enemy == target) || IsQuarry(enemy);  // reloading units step away from the target on their own (Scoot)
}

bool CAntiHeavyTask::CheckFlee()
{
	// antiheavy.flee_margin: from enemies in reach (IsFleeIgnored: not the heavies it hunts; gunships count), unless
	// (flee_ally_threat) escorts next to the leader outweigh them by enough. Once fleeing, until the danger is a bit
	// further than what set it off
	const CMilitaryManager::SAntiHeavyCfg& ahCfg = GetCfg();
	const float fleeMargin = GetFleeMargin();
	return UpdateFlee(fleeMargin, fleeMargin + std::max(0.25f * fleeMargin, 50.f), true,
			ahCfg.fleeAllyThreat, ahCfg.fleeAllyRadius, ROLE_TYPE(AH));
}

void CAntiHeavyTask::FleeFrom(const AIFloat3& dangerPos)
{
	const AIFloat3* goal = FindFleeGoal(dangerPos);
	if (goal == nullptr) {
		FallbackBasePos(true);
		return;
	}
	QueryPath(leader, *goal, FALLBACK_PATH_RANGE, true, [this](const CQueryPathSingle* query) { ApplyFleePath(query); });
}

void CAntiHeavyTask::ApplyFleePath(const CQueryPathSingle* query)
{
	pPath = query->GetPathInfo();

	if (!pPath->posPath.empty()) {
		position = pPath->posPath.back();
		ActivePath();
		state = State::DISENGAGE;
	} else {
		FallbackBasePos(true);  // no path under the cap: base, then (last resort) through any threat
	}
}

void CAntiHeavyTask::OnFleeEnd()
{
	// Out of reach: don't walk the rest of the way to the safety spot
	pPath = std::make_shared<PathInfo>();  // else OnUnitIdle -> Start() would resume the flee path after the Stop
	CCircuitAI* circuit = manager->GetCircuit();
	for (CCircuitUnit* unit : units) {
		if (unit->Blocker() != nullptr) {
			continue;  // busy with its own action (jump, ...)
		}
		unit->GetTravelAct()->StateWait();
		TRY_UNIT(circuit, unit,
			unit->GetUnit()->Stop();
		)
	}
}

float CAntiHeavyTask::GetStandOff(CEnemyInfo* enemy, float engageRange) const
{
	// The threat map draws an enemy's threat out to its range (threat-map squares) on each layer; one more
	// square of slack. Our layer as CThreatMap::SetThreatType picks it
	CCircuitDef* cdef = leader->GetCircuitDef();
	const CEnemyUnit* data = enemy->GetData();
	int range;
	if (cdef->IsAbleToFly()) {
		range = data->GetRange(CCircuitDef::ThreatType::AIR);
	} else if (cdef->IsAmphibious()) {
		range = std::max(data->GetRange(CCircuitDef::ThreatType::LAND), data->GetRange(CCircuitDef::ThreatType::WATER));
	} else {
		range = data->GetRange(CCircuitDef::ThreatType::LAND);
	}
	if (range <= 0) {
		return engageRange;  // can't hit our layer
	}
	return std::max(engageRange, (float)((range + 1) * manager->GetCircuit()->GetThreatMap()->GetSquareSize()));
}

bool CAntiHeavyTask::ProbeStandOff(const AIFloat3& ePos, const AIFloat3& pos, float standOff, AIFloat3& outPos) const
{
	// The threat map draws a moving enemy further out than its stored range (speed slack), and cells are coarse:
	// read the threat rather than trust the estimate
	CCircuitAI* circuit = manager->GetCircuit();
	CThreatMap* threatMap = circuit->GetThreatMap();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	STerrainMapArea* area = leader->GetArea();
	const float step = threatMap->GetSquareSize();
	const float cap = GetPathThreatCap(leader, pos, {GetPointTowards(ePos, pos, standOff)});
	for (int i = 0; i <= 6; ++i) {
		const AIFloat3 p = GetPointTowards(ePos, pos, standOff + i * step);
		if (terrainMgr->CanMoveToPos(area, p) && (GetTaskThreatAt(leader, p) <= cap)) {
			outPos = p;
			return true;
		}
		if (p.SqDistance2D(pos) < 1.f) {
			break;  // reached the squad: no point further out on this line
		}
	}
	return false;
}

AIFloat3 CAntiHeavyTask::GetPointTowards(const AIFloat3& ePos, const AIFloat3& pos, float dist)
{
	AIFloat3 dir = pos - ePos;
	dir.y = 0.f;
	const float len = dir.Length2D();
	if (len <= dist) {
		return pos;
	}
	AIFloat3 p = ePos + dir * (dist / len);
	CTerrainManager::CorrectPosition(p);
	return p;
}

void CAntiHeavyTask::CollectDangers(float margin)
{
	dangers.clear();
	for (auto& kv : manager->GetCircuit()->GetEnemyInfos()) {
		CEnemyInfo* enemy = kv.second;
		float eRange;
		if (!IsGroundDanger(enemy, true, eRange) || IsFleeIgnored(enemy)) {
			continue;  // as FindFleeDanger
		}
		dangers.push_back({enemy->GetPos(), SQUARE(eRange + margin), enemy->GetThreat()});
	}
}

bool CAntiHeavyTask::IsSpotSafe(const AIFloat3& spot) const
{
	float threat = 0.f;
	bool isReached = false;
	for (const SDanger& danger : dangers) {
		if (spot.SqDistance2D(danger.pos) <= danger.sqReach) {
			threat += danger.threat;
			isReached = true;
		}
	}
	if (!isReached) {
		return true;
	}
	const CMilitaryManager::SAntiHeavyCfg& ahCfg = GetCfg();
	return (ahCfg.fleeAllyThreat >= 0.f)
		&& IsFleeCovered(spot, threat, ahCfg.fleeAllyThreat, ahCfg.fleeAllyRadius, ROLE_TYPE(AH));
}

bool CAntiHeavyTask::FindTarget()
{
	CCircuitAI* circuit = manager->GetCircuit();
	CMap* map = circuit->GetMap();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	CThreatMap* threatMap = circuit->GetThreatMap();
	const AIFloat3& pos = leader->GetPos(circuit->GetLastFrame());
	STerrainMapArea* area = leader->GetArea();
	CCircuitDef* cdef = leader->GetCircuitDef();
	const float maxPower = attackPower * powerMod * GetCountPowerMod();  // divided by the thr_mod.distance curve at each enemy's spot
	const float weaponRange = cdef->GetMaxRange();
	const float engageRange = std::max(highestRange, threatMap->GetSquareSize() * 2.0f);  // old: LOS / jump range
	// Stand-off routing goes with the route cap; without it (path_threat < 0, aircraft) the old goals: the heavies
	const bool isStandOff = (GetPathThreatMod() >= 0.f);
	const float pathSlack = isStandOff ? 2 * circuit->GetPathfinder()->GetSquareSize() : 0.f;  // routes end within this of a stand-off point
	// Flee on: skip heavies we'd have to attack from inside a flee zone (escorts, riots, ...), else the squad walks
	// in, flees, and walks back in. Without cover it waits for the army
	const float fleeMargin = GetFleeMargin();
	bool isDangersReady = false;
	float minSqDist = std::numeric_limits<float>::max();

	CEnemyInfo* bestTarget = nullptr;
	enemyPositions.clear();
	threatMap->SetThreatType(leader);  // shared threat-map state: set on every pick
	const CCircuitAI::EnemyInfos& enemies = circuit->GetEnemyInfos();
	for (auto& kv : enemies) {
		CEnemyInfo* enemy = kv.second;
		if (enemy->IsHidden()) {
			continue;
		}
		// The cheap role test first: most enemies aren't heavies, and the rest reads maps and the engine
		if (!IsQuarry(enemy)) {
			continue;
		}
		const AIFloat3& ePos = enemy->GetPos();
		if ((ePos.y - map->GetElevationAt(ePos.x, ePos.z) > weaponRange)
			|| (maxPower / GetThrDistModAt(ePos) <= GetTaskThreatExcl(leader, ePos, enemy->GetThreat()))
			|| !terrainMgr->CanMoveToPos(area, ePos))
		{
			continue;
		}

		// Routes end at the stand-off point, just outside the heavy's own threat, so the route cap only meets
		// its escorts' threat (what the test above judges); the squad attacks from there
		float standOff = isStandOff ? GetStandOff(enemy, engageRange) : engageRange;
		AIFloat3 standPos = GetPointTowards(ePos, pos, standOff);
		const float sqDist = pos.SqDistance2D(ePos);
		const bool isFar = (sqDist > SQUARE(standOff + pathSlack));
		if (isStandOff && isFar) {
			if (!ProbeStandOff(ePos, pos, standOff, standPos)) {
				continue;  // no point near its ring under the route cap: the route there would fail
			}
			standOff = ePos.distance2D(standPos);  // engage once there
		}
		if (fleeMargin >= 0.f) {
			if (!isDangersReady) {
				isDangersReady = true;
				CollectDangers(fleeMargin);
			}
			// Where the attack ends up (weapon range) and where the route ends
			if (!IsSpotSafe(GetPointTowards(ePos, pos, weaponRange * 0.9f)) || !IsSpotSafe(standPos)) {
				continue;
			}
		}

		if (sqDist <= SQUARE(standOff + pathSlack)) {
			if (sqDist < minSqDist) {
				minSqDist = sqDist;
				bestTarget = enemy;
			}
		} else {
			enemyPositions.push_back(isStandOff ? standPos : ePos);
		}
	}

	SetTarget(bestTarget);
	return (bestTarget != nullptr) || !enemyPositions.empty();
}

void CAntiHeavyTask::ApplyTargetPath(const CQueryPathMulti* query)
{
	pPath = query->GetPathInfo();

	if (!pPath->posPath.empty()) {
		position = pPath->posPath.back();
		ActivePath();
	} else {
		FallbackTargetEmpty();
	}
}

void CAntiHeavyTask::FallbackTargetEmpty()
{
	CCircuitAI* circuit = manager->GetCircuit();
	if (!leader->GetCircuitDef()->IsRoleMine() &&
		!circuit->GetMilitaryManager()->IsEnemyHeavySeen(CMilitaryManager::AH_KEEP_FRAMES))
	{
		manager->AbortTask(this);
	} else {
		FallbackAttackSafe();
	}
}

void CAntiHeavyTask::FallbackAttackSafe()
{
	if (leader->GetCircuitDef()->IsRoleMine()) {
		FallbackBasePos(true);
		return;
	}

	CCircuitAI* circuit = manager->GetCircuit();
	circuit->GetMilitaryManager()->FillAttackSafePos(leader, urgentPositions);
	if (urgentPositions.empty()) {
		FallbackStaticSafe();
		return;
	}

	QueryPathMulti(urgentPositions, FALLBACK_PATH_RANGE, [this](const CQueryPathMulti* query) { ApplyAttackSafe(query); });
}

void CAntiHeavyTask::ApplyAttackSafe(const CQueryPathMulti* query)
{
	pPath = query->GetPathInfo();

	if (!pPath->posPath.empty()) {
		position = pPath->posPath.back();
		ActivePath();
	} else {
		FallbackStaticSafe();
	}
}

void CAntiHeavyTask::FallbackStaticSafe()
{
	CCircuitAI* circuit = manager->GetCircuit();
	circuit->GetMilitaryManager()->FillStaticSafePos(leader, urgentPositions);
	if (urgentPositions.empty()) {
		FallbackBasePos(true);
		return;
	}

	QueryPathMulti(urgentPositions, FALLBACK_PATH_RANGE, [this](const CQueryPathMulti* query) { ApplyStaticSafe(query); });
}

void CAntiHeavyTask::ApplyStaticSafe(const CQueryPathMulti* query)
{
	pPath = query->GetPathInfo();

	if (!pPath->posPath.empty()) {
		position = pPath->posPath.back();
		ActivePath();
	} else {
		FallbackBasePos(true);
	}
}

void CAntiHeavyTask::FallbackBasePos(bool isCapped)
{
	CCircuitAI* circuit = manager->GetCircuit();
	position = circuit->GetSetupManager()->GetBasePos();
	QueryPath(leader, position, FALLBACK_PATH_RANGE, isCapped,
			[this, isCapped](const CQueryPathSingle* query) { ApplyBasePos(query, isCapped); });
}

void CAntiHeavyTask::ApplyBasePos(const CQueryPathSingle* query, bool isCapped)
{
	pPath = query->GetPathInfo();

	if (!pPath->posPath.empty()) {
		position = pPath->posPath.back();
		ActivePath();
	} else if (isCapped && (GetPathThreatMod() >= 0.f)) {
		FallbackBasePos(false);  // getting home matters more than the cap: last try through any threat
	} else {
		FallbackCommPos();
	}
}

void CAntiHeavyTask::FallbackCommPos()
{
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	CCircuitUnit* commander = circuit->GetSetupManager()->GetCommander();
	if ((commander != nullptr) &&
		circuit->GetTerrainManager()->CanMoveToPos(leader->GetArea(), commander->GetPos(frame)))
	{
		for (CCircuitUnit* unit : units) {
			unit->Guard(commander, frame + FRAMES_PER_SEC * 60);
			unit->GetTravelAct()->StateWait();
		}
		return;
	}

	position = circuit->GetSetupManager()->GetBasePos();
	for (CCircuitUnit* unit : units) {
		TRY_UNIT(circuit, unit,
			unit->GetUnit()->Fight(position, UNIT_COMMAND_OPTION_RIGHT_MOUSE_KEY, frame + FRAMES_PER_SEC * 60);
		)
		unit->GetTravelAct()->StateWait();
	}
}

} // namespace circuit

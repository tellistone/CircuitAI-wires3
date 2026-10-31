/*
 * ScoutTask.cpp
 *
 *  Created on: Jan 28, 2015
 *      Author: rlcevg
 */

#include "task/fighter/ScoutTask.h"
#include "task/TaskManager.h"
#include "map/ThreatMap.h"
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

#include <limits>

#include "spring/SpringMap.h"

#include "AISCommands.h"
//#include "Drawer.h"  // TraceRay

namespace circuit {

using namespace springai;

CScoutTask::CScoutTask(ITaskManager* mgr, float powerMod)
		: IFighterTask(mgr, FightType::SCOUT, powerMod)
{
}

CScoutTask::~CScoutTask()
{
}

bool CScoutTask::CanAssignTo(CCircuitUnit* unit) const
{
	return units.empty() && unit->GetCircuitDef()->IsRoleScout();
}

void CScoutTask::AssignTo(CCircuitUnit* unit)
{
	IFighterTask::AssignTo(unit);

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

void CScoutTask::RemoveAssignee(CCircuitUnit* unit)
{
	IFighterTask::RemoveAssignee(unit);
	if (units.empty()) {
		manager->AbortTask(this);
	}
}

void CScoutTask::Start(CCircuitUnit* unit)
{
	Execute(unit, false);
}

void CScoutTask::Update()
{
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();

	// Re-decide on every update (was every 2nd update and at most once per second).
	// Floor at the threat map refresh rate: re-deciding faster would only reuse the same threat data.
	if (frame >= lastTouched + THREAT_UPDATE_RATE) {
		lastTouched = frame;
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

void CScoutTask::OnUnitIdle(CCircuitUnit* unit)
{
	IFighterTask::OnUnitIdle(unit);
	if (units.find(unit) != units.end()) {
		RemoveAssignee(unit);
	}
}

void CScoutTask::OnRearmStart(CCircuitUnit* unit)
{
	SetTarget(nullptr);
}

void CScoutTask::Execute(CCircuitUnit* unit, bool isUpdating)
{
	if (unit->Blocker() != nullptr) {
		return;  // Do not interrupt current action
	}

	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	const AIFloat3& pos = unit->GetPos(frame);

	const bool isTargetsFound = FindTarget(unit, pos);

	if (target != nullptr) {
		position = target->GetPos();
		CThreatMap* threatMap = circuit->GetThreatMap();
		const float fireRange = std::max(unit->GetCircuitDef()->GetMaxRange(), (float)threatMap->GetSquareSize());
		// Only shoot from where we stand if it is in range and safe; otherwise approach along a threat-aware path
		if ((pos.SqDistance2D(position) <= SQUARE(fireRange))
			&& (threatMap->GetThreatAt(unit, pos) < GetSafeThreat(unit)))
		{
			const bool wasTravel = unit->GetTravelAct()->IsActive();
			unit->GetTravelAct()->StateWait();
			// Re-issue only when something changed: avoids command spam at the faster update rate
			if (wasTravel || (lastAttackId != target->GetId()) || (frame >= lastAttackFrame + FRAMES_PER_SEC)) {
				lastAttackId = target->GetId();
				lastAttackFrame = frame;
				AttackTarget(unit, pos, frame);
			}
			return;
		}
		lastAttackId = -1;
		if (!IsQueryReady(unit) || !NeedRepath(unit, target->GetId())) {
			return;
		}
		ApproachTarget(unit, pos, fireRange, isUpdating);
		return;
	}

	lastAttackId = -1;
	if (!IsQueryReady(unit) || !NeedRepath(unit, -1)) {
		return;
	}

	if (!isTargetsFound) {  // enemyPositions.empty()
		FallbackScout(unit, isUpdating);
		return;
	}

	CCircuitDef* cdef = unit->GetCircuitDef();
	CThreatMap* threatMap = circuit->GetThreatMap();
	const float range = std::max(unit->GetUnit()->GetMaxRange() + threatMap->GetSquareSize() * 2,
								 /*unit->IsUnderWater(frame) ? cdef->GetSonarRadius() : */cdef->GetLosRadius());

	CPathFinder* pathfinder = circuit->GetPathfinder();
	std::shared_ptr<IPathQuery> query = pathfinder->CreatePathMultiQuery(
			unit, threatMap, frame,
			pos, range * 0.5f, enemyPositions, nullptr, GetPathThreatCap(unit));
	pathQueries[unit] = query;
	query->HoldTask(this);

	pathfinder->RunQuery(query, [this, isUpdating](const IPathQuery* query) {
		if (this->IsQueryAlive(query)) {
			this->ApplyTargetPath(static_cast<const CQueryPathMulti*>(query), isUpdating);
		}
	});
}

bool CScoutTask::FindTarget(CCircuitUnit* unit, const AIFloat3& pos)
{
	CCircuitAI* circuit = manager->GetCircuit();
	CMap* map = circuit->GetMap();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	CThreatMap* threatMap = circuit->GetThreatMap();
	STerrainMapArea* area = unit->GetArea();
	CCircuitDef* cdef = unit->GetCircuitDef();
	const bool isAntiStatic = cdef->IsAttrAntiStat();
	const bool notAW = !cdef->HasAntiWater();
	const bool notAA = !cdef->HasAntiAir();
	const float speed = SQUARE(cdef->GetSpeed() * 0.8f / FRAMES_PER_SEC);
	const float maxPower = GetSafeThreat(unit);  // incl. distance-from-home modifier
	const float weaponRange = cdef->GetMaxRange();
	const int canTargetCat = cdef->GetTargetCategory();
	const int noChaseCat = cdef->GetNoChaseCategory();
	const float range = std::max(unit->GetUnit()->GetMaxRange() + threatMap->GetSquareSize() * 2,
								 /*unit->IsUnderWater(frame) ? cdef->GetSonarRadius() : */cdef->GetLosRadius());
	float minSqDist = SQUARE(range);
	float maxThreat = 0.f;
	float minPower = maxPower;

	const int frame = circuit->GetLastFrame();
	for (auto it = badTargets.begin(); it != badTargets.end();) {
		it = (it->second <= frame) ? badTargets.erase(it) : std::next(it);
	}

	SetTarget(nullptr);  // make adequate enemy->GetTasks().size()
	CEnemyInfo* bestTarget = nullptr;
	CEnemyInfo* worstTarget = nullptr;
	enemyPositions.clear();
	threatMap->SetThreatType(unit);
	const CCircuitAI::EnemyInfos& enemies = circuit->GetEnemyInfos();
	for (auto& kv : enemies) {
		CEnemyInfo* enemy = kv.second;
		if (enemy->IsHidden() || (enemy->GetTasks().size() > 2)) {
			continue;
		}
		const bool isBadTarget = (badTargets.find(enemy->GetId()) != badTargets.end());  // no safe approach found recently
		const AIFloat3& ePos = enemy->GetPos();
		const float power = threatMap->GetThreatAt(ePos);
		if ((maxPower <= power)
			|| !terrainMgr->CanMoveToPos(area, ePos)
			|| (enemy->GetVel().SqLength2D() >= speed))
		{
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
			isBuilder = edef->IsEnemyRoleAny(CCircuitDef::RoleMask::BUILDER);
		} else {
			if (notAW && (ePos.y < -SQUARE_SIZE * 5)) {
				continue;
			}
			targetCat = UNKNOWN_CATEGORY;
			defThreat = enemy->GetThreat();
			isBuilder = false;
		}

		float sqDist = pos.SqDistance2D(ePos);
		if (isBadTarget) {
			continue;
		}
		if ((minPower > power) && (minSqDist > sqDist)) {
			if (enemy->IsInRadarOrLOS()) {
//				AIFloat3 dir = enemy->GetPos() - pos;
//				float rayRange = dir.LengthNormalize();
//				CUnit::Id hitUID = circuit->GetDrawer()->TraceRay(pos, dir, rayRange, u, 0);
//				if (hitUID == enemy->GetId()) {
					if (((targetCat & noChaseCat) == 0) && !enemy->IsBeingBuilt()) {
						if (isBuilder) {
							bestTarget = enemy;
							minSqDist = sqDist;
							maxThreat = std::numeric_limits<float>::max();
						} else if (maxThreat <= defThreat) {
							bestTarget = enemy;
							minSqDist = sqDist;
							maxThreat = defThreat;
						}
						minPower = power;
					} else if (bestTarget == nullptr) {
						worstTarget = enemy;
					}
//				}
			}
			continue;
		}
		if (sqDist < SQUARE(1000.f)) {  // maxSqDist
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
	if (enemyPositions.empty()) {
		return false;
	}

	return true;
	// Return: target=bestTarget, startPos=pos, enemyPositions
}

void CScoutTask::ApplyTargetPath(const CQueryPathMulti* query, bool isUpdating)
{
	const std::shared_ptr<PathInfo>& pPath = query->GetPathInfo();
	CCircuitUnit* unit = query->GetUnit();

	if (!pPath->posPath.empty()) {
		position = pPath->posPath.back();
		ActivatePath(unit, pPath, -1, (GetPathThreatCap(unit) < std::numeric_limits<float>::max()));
	} else {
		FallbackScout(unit, isUpdating);
	}
}

void CScoutTask::FallbackScout(CCircuitUnit* unit, bool isUpdating)
{
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	CThreatMap* threatMap = circuit->GetThreatMap();
	const AIFloat3& pos = unit->GetPos(frame);
	const AIFloat3& threatPos = unit->GetTravelAct()->IsActive() ? position : pos;
	const float safeThreat = GetSafeThreat(unit);
	// NOTE: Use max(unit_threat, THREAT_MIN) for no-weapon scouts
	const bool proceed = isUpdating && utils::is_valid(position) && (threatMap->GetThreatAt(unit, threatPos) < safeThreat);
	if (!proceed) {
		if (isUpdating && utils::is_valid(position)) {
			// Destination turned dangerous (e.g. an army sits there): don't re-pick it straight away
			circuit->GetMilitaryManager()->BlockScoutSpot(position, FRAMES_PER_SEC * 10);
		}
		position = PickScoutPosition(unit, safeThreat);
	}

	RunScoutQuery(unit, true, 0);
}

void CScoutTask::RunScoutQuery(CCircuitUnit* unit, bool isCapped, int attempt)
{
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	CThreatMap* threatMap = circuit->GetThreatMap();
	const AIFloat3& pos = unit->GetPos(frame);

	CPathFinder* pathfinder = circuit->GetPathfinder();
	std::shared_ptr<IPathQuery> query = pathfinder->CreatePathSingleQuery(
			unit, threatMap, frame,
			pos, position, pathfinder->GetSquareSize(), nullptr,
			isCapped ? GetPathThreatCap(unit) : std::numeric_limits<float>::max());
	pathQueries[unit] = query;
	query->HoldTask(this);

	pathfinder->RunQuery(query, [this, isCapped, attempt](const IPathQuery* query) {
		if (this->IsQueryAlive(query)) {
			this->ApplyScoutPath(static_cast<const CQueryPathSingle*>(query), isCapped, attempt);
		}
	});
}

void CScoutTask::ApplyScoutPath(const CQueryPathSingle* query, bool isCapped, int attempt)
{
	const std::shared_ptr<PathInfo>& pPath = query->GetPathInfo();
	CCircuitUnit* unit = query->GetUnit();

	if (pPath->path.size() > 2) {
//		position = path.back();
		ActivatePath(unit, pPath, -1, isCapped && (GetPathThreatCap(unit) < std::numeric_limits<float>::max()));
		return;
	}

	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	CThreatMap* threatMap = circuit->GetThreatMap();
	const float safeThreat = GetSafeThreat(unit);

	if (pPath->path.empty() && isCapped && (GetPathThreatCap(unit) < std::numeric_limits<float>::max())) {
		// No route that stays out of danger
		if (attempt < 2) {
			// Try another scouting spot; keep scouts off this one for a while
			circuit->GetMilitaryManager()->BlockScoutSpot(position, FRAMES_PER_SEC * 10);
			position = PickScoutPosition(unit, safeThreat);
			RunScoutQuery(unit, true, attempt + 1);
		} else {
			// Every route crosses some danger, or we are standing in it: least-threat route to a destination
			// that is safe right now, mobile enemies included (spot picking ignores them); else go home
			if (!utils::is_valid(position) || (threatMap->GetThreatAt(unit, position) >= safeThreat)) {
				position = circuit->GetTerrainManager()->GetMovePosition(unit->GetArea(), circuit->GetSetupManager()->GetBasePos());
			}
			RunScoutQuery(unit, false, attempt + 1);
		}
		return;
	}

	// Very short path or no path at all: plain move only into a safe spot
	if (utils::is_valid(position) && (threatMap->GetThreatAt(unit, position) < safeThreat)) {
		TRY_UNIT(circuit, unit,
			unit->CmdMoveTo(position, UNIT_COMMAND_OPTION_RIGHT_MOUSE_KEY, frame + FRAMES_PER_SEC * 60);
		)
	}
	unit->GetTravelAct()->StateWait();
}

float CScoutTask::GetSafeThreat(CCircuitUnit* unit) const
{
	CCircuitAI* circuit = manager->GetCircuit();
	return std::max(circuit->GetThreatMap()->GetUnitThreat(unit), THREAT_MIN) * GetPowerModAt(unit->GetPos(circuit->GetLastFrame()));
}

float CScoutTask::GetPathThreatCap(CCircuitUnit* unit) const
{
	// NOTE: The pathfinder uses the decloak-range array for cloaked units, which is a different scale
	if (unit->GetUnit()->IsCloaked()) {
		return std::numeric_limits<float>::max();
	}
	return GetSafeThreat(unit);
}

void CScoutTask::AttackTarget(CCircuitUnit* unit, const AIFloat3& firePos, int frame)
{
	CCircuitAI* circuit = manager->GetCircuit();
	const int timeout = frame + FRAMES_PER_SEC * 60;
	if (target->GetUnit()->IsCloaked()) {
		TRY_UNIT(circuit, unit,
			unit->CmdAttackGround(position, UNIT_COMMAND_OPTION_RIGHT_MOUSE_KEY, timeout);
		)
	} else if (unit->GetCircuitDef()->IsAttrMelee()) {
		unit->Attack(target, timeout);  // melee units have to close in anyway
	} else {
		TRY_UNIT(circuit, unit,
			unit->GetUnit()->Attack(target->GetUnit(), UNIT_COMMAND_OPTION_RIGHT_MOUSE_KEY, timeout);
			// Once the target is gone, return to the checked firing spot instead of attack-moving onwards
			unit->CmdMoveTo(firePos, UNIT_COMMAND_OPTION_RIGHT_MOUSE_KEY | UNIT_COMMAND_OPTION_SHIFT_KEY, timeout);
			unit->CmdWantedSpeed(NO_SPEED_LIMIT);
			unit->CmdSetTarget(target);
		)
	}
}

void CScoutTask::ApproachTarget(CCircuitUnit* unit, const AIFloat3& pos, float fireRange, bool isUpdating)
{
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	CThreatMap* threatMap = circuit->GetThreatMap();
	CPathFinder* pathfinder = circuit->GetPathfinder();
	// End the path inside weapon range; capped so neither the route nor the firing spot is too dangerous
	const float pathRange = std::max(fireRange * 0.9f, (float)pathfinder->GetSquareSize());
	std::shared_ptr<IPathQuery> query = pathfinder->CreatePathSingleQuery(
			unit, threatMap, frame,
			pos, position, pathRange, nullptr, GetPathThreatCap(unit));
	pathQueries[unit] = query;
	query->HoldTask(this);

	const ICoreUnit::Id targetId = target->GetId();
	pathfinder->RunQuery(query, [this, targetId, isUpdating](const IPathQuery* query) {
		if (this->IsQueryAlive(query)) {
			this->ApplyApproachPath(static_cast<const CQueryPathSingle*>(query), targetId, isUpdating);
		}
	});
}

void CScoutTask::ApplyApproachPath(const CQueryPathSingle* query, ICoreUnit::Id targetId, bool isUpdating)
{
	const std::shared_ptr<PathInfo>& pPath = query->GetPathInfo();
	CCircuitUnit* unit = query->GetUnit();

	if (!pPath->posPath.empty()) {
		ActivatePath(unit, pPath, targetId, (GetPathThreatCap(unit) < std::numeric_limits<float>::max()));
		return;
	}

	// No safe way to get this target: skip it for a while and scout instead
	badTargets[targetId] = manager->GetCircuit()->GetLastFrame() + FRAMES_PER_SEC * 3;
	if ((target != nullptr) && (target->GetId() == targetId)) {
		SetTarget(nullptr);
	}
	FallbackScout(unit, isUpdating);
}

AIFloat3 CScoutTask::PickScoutPosition(CCircuitUnit* unit, float safeThreat)
{
	CCircuitAI* circuit = manager->GetCircuit();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	CThreatMap* threatMap = circuit->GetThreatMap();
	CMilitaryManager* militaryMgr = circuit->GetMilitaryManager();

	// Best-scored mex spot: near the enemy, unseen for longest, not defended by known static enemies
	const AIFloat3 spot = militaryMgr->PickScoutSpot(unit, safeThreat);
	if (utils::is_valid(spot)) {
		return spot;
	}
	// Random safe spot
	for (int i = 0; i < 4; ++i) {
		float x = rand() % terrainMgr->GetTerrainWidth();
		float z = rand() % terrainMgr->GetTerrainHeight();
		AIFloat3 rndPos(x, circuit->GetMap()->GetElevationAt(x, z), z);
		rndPos = terrainMgr->GetMovePosition(unit->GetArea(), rndPos);
		if (utils::is_valid(rndPos) && (threatMap->GetThreatAt(unit, rndPos) < safeThreat)) {
			return rndPos;
		}
	}
	// Nothing safe found: head home
	return terrainMgr->GetMovePosition(unit->GetArea(), circuit->GetSetupManager()->GetBasePos());
}

void CScoutTask::ActivatePath(CCircuitUnit* unit, const std::shared_ptr<PathInfo>& pPath, ICoreUnit::Id pathTargetId, bool isCapped)
{
	CCircuitAI* circuit = manager->GetCircuit();
	ITravelAction* travelAct = unit->GetTravelAct();
	travelAct->SetPath(pPath);
	travelAct->StateActivate();
	lastPathFrame = circuit->GetLastFrame();
	lastPathTargetId = pathTargetId;
	isPathCapped = isCapped;
	// Unit actions are only updated every ACTION_UPDATE_RATE frames (staggered) and move actions
	// at most once per second, so a new path could take ~2s to reach the unit. Issue it now.
	travelAct->ResetUpdateTimer();
	unit->Update(circuit);
}

bool CScoutTask::NeedRepath(CCircuitUnit* unit, ICoreUnit::Id targetId) const
{
	const int frame = manager->GetCircuit()->GetLastFrame();
	return !unit->GetTravelAct()->IsActive()
		|| (targetId != lastPathTargetId)
		|| (frame >= lastPathFrame + FRAMES_PER_SEC)
		// An uncapped (least-threat) path crosses danger on purpose: don't re-plan it on every update
		|| (isPathCapped && IsPathUnsafe(unit));
}

bool CScoutTask::IsPathUnsafe(CCircuitUnit* unit) const
{
	CCircuitAI* circuit = manager->GetCircuit();
	CThreatMap* threatMap = circuit->GetThreatMap();
	const AIFloat3& pos = unit->GetPos(circuit->GetLastFrame());
	const float safeThreat = GetSafeThreat(unit);
	if (threatMap->GetThreatAt(unit, pos) >= safeThreat) {
		return true;
	}
	const std::shared_ptr<PathInfo>& pPath = unit->GetTravelAct()->GetPath();
	if (pPath == nullptr) {
		return false;
	}
	// Check the stretch of path the scout will cover in the next ~2s
	const float lookAhead = std::max(unit->GetCircuitDef()->GetSpeed() * 2.f, 200.f);
	const float sqLookAhead = SQUARE(lookAhead);
	for (const AIFloat3& p : pPath->posPath) {
		if ((p.SqDistance2D(pos) < sqLookAhead) && (threatMap->GetThreatAt(unit, p) >= safeThreat)) {
			return true;
		}
	}
	return false;
}

} // namespace circuit

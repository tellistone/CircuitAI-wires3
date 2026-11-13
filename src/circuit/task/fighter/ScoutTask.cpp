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
#include "Command.h"
//#include "Drawer.h"  // TraceRay

namespace circuit {

using namespace springai;

CScoutTask::CScoutTask(ITaskManager* mgr, float powerMod)
		: IFighterTask(mgr, FightType::SCOUT, powerMod)
{
	isRaidView = true;  // CThreatMap raid layers
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
			// Safety net: a scout with an empty order queue and no travel path is standing still for no reason.
			// The engine sends UnitIdle only once when the queue empties, so re-decide from scratch here.
			bool isStalled = false;
			if ((frame >= lastStallCheck + FRAMES_PER_SEC * 2) && !unit->GetTravelAct()->IsActive() && IsQueryReady(unit)) {
				lastStallCheck = frame;
				auto commands = unit->GetUnit()->GetCurrentCommands();
				isStalled = commands.empty();
				utils::free_clear(commands);
			}
			if (isStalled) {
				position = -RgtVector;  // pick a fresh destination
				lastAttackId = -1;
			}
			Execute(unit, !isStalled);
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
	if (isFleeing) {
		return;  // arrived at the flee spot: wait out the clear time here, then scouting resumes
	}
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

	if (CheckFlee(unit, pos, frame)) {
		return;
	}

	const bool isTargetsFound = FindTarget(unit, pos);

	if (target != nullptr) {
		position = target->GetPos();
		CThreatMap* threatMap = circuit->GetThreatMap();
		const float fireRange = std::max(unit->GetCircuitDef()->GetMaxRange(), (float)threatMap->GetSquareSize());
		const float safeThreat = GetSafeThreat(unit);
		auto isSafeAt = [&](const AIFloat3& p) {
			return threatMap->GetThreatAt(unit, p) < safeThreat / GetThrDistModAt(p);
		};
		auto attackFrom = [&](const AIFloat3& firePos) {
			const bool wasTravel = unit->GetTravelAct()->IsActive();
			unit->GetTravelAct()->StateWait();
			// Re-issue only when something changed: avoids command spam at the faster update rate
			if (wasTravel || (lastAttackId != target->GetId()) || (frame >= lastAttackFrame + FRAMES_PER_SEC)) {
				lastAttackId = target->GetId();
				lastAttackFrame = frame;
				AttackTarget(unit, firePos, frame);
			}
		};
		const float sqDist = pos.SqDistance2D(position);
		// Only shoot from where we stand if it is in range and safe; otherwise approach along a threat-aware path
		if ((sqDist <= SQUARE(fireRange)) && isSafeAt(pos)) {
			attackFrom(pos);
			return;
		}
		// Just out of range: the approach path works on a coarse grid, so it can end (or count as arrived) a little
		// outside real weapon range, and a new path from here would be empty. If we stand safe and the spot inside
		// range towards the target is safe too, let the engine's attack order close the last stretch itself.
		const float closeRange = fireRange + circuit->GetPathfinder()->GetSquareSize() * 2;
		if ((sqDist > SQUARE(fireRange)) && (sqDist <= SQUARE(closeRange)) && isSafeAt(pos)) {
			const float dist = sqrtf(sqDist);
			AIFloat3 firePos = position + (pos - position) * (fireRange * 0.9f / dist);
			firePos.y = circuit->GetMap()->GetElevationAt(firePos.x, firePos.z);
			if (isSafeAt(firePos)) {
				attackFrom(firePos);
				return;
			}
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

	float goalMod = 1.f;
	for (const AIFloat3& ePos : enemyPositions) {
		goalMod = std::max(goalMod, GetThrDistModAt(ePos));
	}
	CPathFinder* pathfinder = circuit->GetPathfinder();
	std::shared_ptr<IPathQuery> query = pathfinder->CreatePathMultiQuery(
			unit, threatMap, frame,
			pos, range * 0.5f, enemyPositions, nullptr, GetPathThreatCap(unit, goalMod));
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
	const float maxPower = GetSafeThreat(unit);  // divided by the thr_mod.distance curve at each enemy's spot
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
	threatMap->SetThreatType(unit);  // shared state for other readers: on the plain layers, as always
	const CCircuitAI::EnemyInfos& enemies = circuit->GetEnemyInfos();
	for (auto& kv : enemies) {
		CEnemyInfo* enemy = kv.second;
		if (enemy->IsHidden() || (enemy->GetTasks().size() > 2)) {
			continue;
		}
		const bool isBadTarget = (badTargets.find(enemy->GetId()) != badTargets.end());  // no safe approach found recently
		const AIFloat3& ePos = enemy->GetPos();
		const float power = threatMap->GetThreatAt(unit, ePos);  // the raid view, as the scout's other reads
		if ((maxPower / GetThrDistModAt(ePos) <= power)
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
	if (isFleeing) {
		return;  // asked before the flee started
	}
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
	const bool proceed = isUpdating && utils::is_valid(position) && (threatMap->GetThreatAt(unit, threatPos) < safeThreat / GetThrDistModAt(threatPos));
	if (!proceed) {
		if (isUpdating && utils::is_valid(position)) {
			// Destination turned dangerous (e.g. an army sits there): don't re-pick it straight away
			circuit->GetMilitaryManager()->BlockScoutSpot(position, FRAMES_PER_SEC * 10);
		}
		position = PickScoutPosition(unit, safeThreat);
		if (!utils::is_valid(position)) {
			HoldScout(unit);
			return;
		}
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
			isCapped ? GetPathThreatCap(unit, GetThrDistModAt(position)) : std::numeric_limits<float>::max());
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
	CCircuitAI* circuit = manager->GetCircuit();
	CThreatMap* threatMap = circuit->GetThreatMap();
	const float safeThreat = GetSafeThreat(unit);
	const bool hasCap = (GetPathThreatCap(unit) < std::numeric_limits<float>::max());
	auto isSafeNow = [&](const AIFloat3& p) {
		return utils::is_valid(p) && (threatMap->GetThreatAt(unit, p) < safeThreat / GetThrDistModAt(p));
	};
	if (isFleeing) {
		return;  // asked before the flee started
	}
	if (pPath->path.size() > 2) {
		ActivatePath(unit, pPath, -1, isCapped && hasCap);
		return;
	}

	const int frame = circuit->GetLastFrame();
	const AIFloat3& pos = unit->GetPos(frame);
	// The pathfinder checks the cap on every cell next to the start: a scout standing in danger gets no capped
	// route to anywhere. That's the scout's spot, not the destination's fault: flee instead of blocking spots
	if (CheckFlee(unit, pos, frame)) {
		return;
	}

	// No capped route to a spot we can reach: danger is in the way
	if (pPath->path.empty() && isCapped && hasCap
		&& utils::is_valid(position) && circuit->GetTerrainManager()->CanMoveToPos(unit->GetArea(), position))
	{
		// Keep scouts off this spot for a while
		circuit->GetMilitaryManager()->BlockScoutSpot(position, FRAMES_PER_SEC * 10);
		if (attempt < 2) {
			// Try another scouting spot
			position = PickScoutPosition(unit, safeThreat);
			if (!utils::is_valid(position)) {
				HoldScout(unit);
				return;
			}
			RunScoutQuery(unit, true, attempt + 1);
		} else if (isSafeNow(position)) {
			// Least-threat route, but only to a destination that is safe right now (spot picking ignores mobile
			// enemies). If it leads into danger after all, the flee takes over
			RunScoutQuery(unit, false, attempt + 1);
		} else {
			HoldScout(unit);
		}
		return;
	}

	// No route at all (e.g. still inside the factory), or the spot is right here: plain move to the spot if it is
	// safe, otherwise wait and pick a fresh spot on the next update. Scouts never head home.
	if (!isSafeNow(position)) {
		if (utils::is_valid(position) && !pPath->path.empty()) {
			// Standing next to a spot that turned dangerous: keep scouts off it for a while
			circuit->GetMilitaryManager()->BlockScoutSpot(position, FRAMES_PER_SEC * 10);
		}
		HoldScout(unit);
		return;
	}
	// NOTE: This runs in a pathfinder callback, which can run inside an allied AI's engine event (shared pathfinder),
	//       where orders are rejected by the server. A one-point path goes through ActivatePath, which handles that.
	std::shared_ptr<PathInfo> pMove = std::make_shared<PathInfo>();
	pMove->posPath.push_back(position);
	ActivatePath(unit, pMove, -1, false);
}

float CScoutTask::GetSafeThreat(CCircuitUnit* unit) const
{
	// The thr_mod.distance curve is applied per judged spot (GetThrDistModAt), not here
	return std::max(manager->GetCircuit()->GetThreatMap()->GetUnitThreat(unit), THREAT_MIN) * powerMod;
}

float CScoutTask::GetPathThreatCap(CCircuitUnit* unit, float goalMod) const
{
	// NOTE: The pathfinder uses the decloak-range array for cloaked units, which is a different scale
	if (IsCloakRoute(unit)) {
		return std::numeric_limits<float>::max();
	}
	// The cap is uniform along the path: use the stricter of start and goal, so a path planned near home
	// doesn't get the bold near-home cap all the way into enemy land (and then trip the look-ahead check)
	CCircuitAI* circuit = manager->GetCircuit();
	return GetSafeThreat(unit) / std::max(GetThrDistModAt(unit->GetPos(circuit->GetLastFrame())), goalMod);
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
			pos, position, pathRange, nullptr, GetPathThreatCap(unit, GetThrDistModAt(position)));
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
	if (isFleeing) {
		return;  // asked before the flee started
	}
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
	for (int i = 0; i < 8; ++i) {
		float x = rand() % terrainMgr->GetTerrainWidth();
		float z = rand() % terrainMgr->GetTerrainHeight();
		AIFloat3 rndPos(x, circuit->GetMap()->GetElevationAt(x, z), z);
		rndPos = terrainMgr->GetMovePosition(unit->GetArea(), rndPos);
		if (utils::is_valid(rndPos) && (threatMap->GetThreatAt(unit, rndPos) < safeThreat / GetThrDistModAt(rndPos))) {
			return rndPos;
		}
	}
	// Nothing safe found: the caller waits and picks again on the next update (scouts never head home)
	return -RgtVector;
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
	// (Path callbacks always run in our own engine event: CPathFinder::DeliverToOwner)
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
	if (threatMap->GetThreatAt(unit, pos) >= safeThreat / GetThrDistModAt(pos)) {
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
		if ((p.SqDistance2D(pos) < sqLookAhead) && (threatMap->GetThreatAt(unit, p) >= safeThreat / GetThrDistModAt(p))) {
			return true;
		}
	}
	return false;
}

bool CScoutTask::IsInDanger(CCircuitUnit* unit, const AIFloat3& p) const
{
	CThreatMap* threatMap = manager->GetCircuit()->GetThreatMap();
	return threatMap->GetThreatAt(unit, p) >= GetSafeThreat(unit) / GetThrDistModAt(p);
}

bool CScoutTask::CheckFlee(CCircuitUnit* unit, const AIFloat3& pos, int frame)
{
	// A cloaked route ignores the plain threat on purpose: cloaked scouts sneak through it
	if (IsCloakRoute(unit) || !IsInDanger(unit, pos)) {
		if (!isFleeing) {
			return false;
		}
		if (safeSinceFrame < 0) {
			safeSinceFrame = frame;
		}
		if (frame < safeSinceFrame + FLEE_CLEAR_FRAMES) {
			return true;  // keep running along the flee path (or wait at its end) until clear for long enough
		}
		// Clear: resume scouting from here with a fresh spot
		isFleeing = false;
		safeSinceFrame = -1;
		fleeDest = -RgtVector;
		position = -RgtVector;
		return false;
	}

	if (!isFleeing) {
		isFleeing = true;
		lastFleeFrame = -1;
		SetTarget(nullptr);
		lastAttackId = -1;
	}
	safeSinceFrame = -1;
	// Re-plan once a second (the danger moves), or straight away when the run ended or its spot turned dangerous
	if ((frame >= lastFleeFrame + FRAMES_PER_SEC)
		|| ((frame >= lastFleeFrame + THREAT_UPDATE_RATE) && IsQueryReady(unit)
			&& (!unit->GetTravelAct()->IsActive() || !utils::is_valid(fleeDest) || IsInDanger(unit, fleeDest))))
	{
		Flee(unit, pos, frame);
	}
	return true;
}

void CScoutTask::Flee(CCircuitUnit* unit, const AIFloat3& pos, int frame)
{
	lastFleeFrame = frame;
	CCircuitAI* circuit = manager->GetCircuit();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	CThreatMap* threatMap = circuit->GetThreatMap();
	STerrainMapArea* area = unit->GetArea();
	const float safeThreat = GetSafeThreat(unit);
	const float width = terrainMgr->GetTerrainWidth();
	const float height = terrainMgr->GetTerrainHeight();

	// Candidate spots on rings around the scout, nearest ring first. Prefer spots well clear of the threat (half the
	// safe level) so the scout doesn't stop right at the edge; else any safe spot; else the least dangerous one
	static F3Vec clearSpots;  // NOTE: micro-opt
	static F3Vec safeSpots;
	clearSpots.clear();
	safeSpots.clear();
	AIFloat3 leastSpot = -RgtVector;
	float leastRatio = std::numeric_limits<float>::max();
	constexpr int DIRS = 16;
	constexpr int MAX_SPOTS = 4;
	int ring = 0;
	for (float radius = 250.f; (radius <= 3000.f) && (clearSpots.size() < MAX_SPOTS); radius += 250.f, ++ring) {
		const float offset = (ring % 2) * 0.5f;  // stagger the rings
		for (int i = 0; i < DIRS; ++i) {
			const float angle = 2.f * static_cast<float>(M_PI) * (i + offset) / DIRS;
			AIFloat3 p(utils::clamp(pos.x + radius * cosf(angle), 1.f, width - 1.f), pos.y,
					   utils::clamp(pos.z + radius * sinf(angle), 1.f, height - 1.f));
			p = terrainMgr->GetMovePosition(area, p);
			if (!utils::is_valid(p) || !terrainMgr->CanMoveToPos(area, p)) {
				continue;
			}
			const float ratio = threatMap->GetThreatAt(unit, p) * GetThrDistModAt(p) / safeThreat;
			if (ratio < 0.5f) {
				clearSpots.push_back(p);
			} else if (ratio < 1.f) {
				if (safeSpots.size() < MAX_SPOTS) {
					safeSpots.push_back(p);
				}
			} else if (ratio < leastRatio) {
				leastRatio = ratio;
				leastSpot = p;
			}
		}
	}
	const F3Vec* spots = !clearSpots.empty() ? &clearSpots : &safeSpots;
	if (spots->empty()) {
		if (!utils::is_valid(leastSpot)) {
			return;  // nowhere to go (e.g. still inside the factory): try again in a second
		}
		safeSpots.push_back(leastSpot);
	}
	fleeDest = spots->front();  // nearest: the straight-move fallback when no route is found

	// Least-threat route (no cap: we stand in danger, every route starts in it) to whichever spot is cheapest to reach
	CPathFinder* pathfinder = circuit->GetPathfinder();
	std::shared_ptr<IPathQuery> query = pathfinder->CreatePathMultiQuery(
			unit, threatMap, frame,
			pos, pathfinder->GetSquareSize(), *spots);
	pathQueries[unit] = query;
	query->HoldTask(this);

	pathfinder->RunQuery(query, [this](const IPathQuery* query) {
		if (this->IsQueryAlive(query)) {
			this->ApplyFleePath(static_cast<const CQueryPathMulti*>(query));
		}
	});
}

void CScoutTask::ApplyFleePath(const CQueryPathMulti* query)
{
	if (!isFleeing) {
		return;  // the flee ended while this was in flight
	}
	const std::shared_ptr<PathInfo>& pPath = query->GetPathInfo();
	CCircuitUnit* unit = query->GetUnit();
	if (!pPath->posPath.empty()) {
		fleeDest = pPath->posPath.back();
		ActivatePath(unit, pPath, -1, false);
	} else if (utils::is_valid(fleeDest)) {
		std::shared_ptr<PathInfo> pMove = std::make_shared<PathInfo>();
		pMove->posPath.push_back(fleeDest);
		ActivatePath(unit, pMove, -1, false);
	}
}

void CScoutTask::HoldScout(CCircuitUnit* unit)
{
	position = -RgtVector;  // forces a fresh pick on the next update
	unit->GetTravelAct()->StateWait();
}

} // namespace circuit

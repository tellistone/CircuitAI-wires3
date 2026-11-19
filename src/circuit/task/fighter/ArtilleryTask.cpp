/*
 * ArtilleryTask.cpp
 *
 *  Created on: Jan 6, 2016
 *      Author: rlcevg
 */

#include "task/fighter/ArtilleryTask.h"
#include "task/fighter/AntiHeavyTask.h"
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

#include <limits>
#include <tuple>

namespace circuit {

using namespace springai;

// Scratch shared by all artillery tasks: only used within one synchronous call (path queries copy their goals)
F3Vec CArtilleryTask::safePositions;
F3Vec CArtilleryTask::firePositions;
std::vector<CArtilleryTask::SEscort> CArtilleryTask::escorts;
std::vector<CArtilleryTask::SDanger> CArtilleryTask::dangers;
std::vector<float> CArtilleryTask::coverGrid;
std::vector<float> CArtilleryTask::fleeCoverGrid;
std::vector<char> CArtilleryTask::cellAdded;

CArtilleryTask::CArtilleryTask(ITaskManager* mgr, bool isAntiHeavy)
		: IFighterTask(mgr, FightType::ARTY, 1.f)
		, isAntiHeavy(isAntiHeavy)
{
	position = manager->GetCircuit()->GetSetupManager()->GetBasePos();
}

CArtilleryTask::~CArtilleryTask()
{
}

bool CArtilleryTask::CanAssignTo(CCircuitUnit* unit) const
{
	CCircuitDef* cdef = unit->GetCircuitDef();
	return units.empty()
			&& (isAntiHeavy ? manager->GetCircuit()->GetMilitaryManager()->IsAhArty(cdef) : cdef->IsRoleArty());
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
	if (!CheckFlee(unit)) {  // a (re)started task must not walk towards an enemy first
		Execute(unit, false);
	}
}

void CArtilleryTask::Update()
{
	const int frame = manager->GetCircuit()->GetLastFrame();
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

void CArtilleryTask::OnUnitIdle(CCircuitUnit* unit)
{
	IFighterTask::OnUnitIdle(unit);
	if (units.find(unit) != units.end()) {
		RemoveAssignee(unit);
	}
}

const CMilitaryManager::SArtilleryCfg& CArtilleryTask::GetCfg() const
{
	return manager->GetCircuit()->GetMilitaryManager()->GetArtilleryCfg();
}

float CArtilleryTask::GetMaxPathThreat(CCircuitUnit* unit) const
{
	// max_path_threat x own power (pwr_mod, not thr_mod: thr_mod shapes how scary a type looks as an enemy
	// and is often ~0 for artillery)
	const float maxPathThreat = GetCfg().maxPathThreat;
	return (maxPathThreat >= 0.f)
			? std::max(maxPathThreat * unit->GetCircuitDef()->GetPower(), THREAT_MIN)
			: std::numeric_limits<float>::max();
}

float CArtilleryTask::GetFleeMargin(CCircuitUnit* unit) const
{
	return unit->GetCircuitDef()->GetArtyFleeMargin(GetCfg().fleeMargin);
}

/*
 * Shooting and walking to firing spots
 */

void CArtilleryTask::Execute(CCircuitUnit* unit, bool isUpdating)
{
	if (unit->Blocker() != nullptr) {
		return;  // Do not interrupt current action
	}

	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	const AIFloat3& pos = unit->GetPos(frame);
	const CMilitaryManager::SArtilleryCfg& artyCfg = GetCfg();
	const bool isNeedCover = (artyCfg.allyThreat >= 0.f);
	CEnemyInfo* bestTarget = FindTarget(unit, pos);

	if (bestTarget != nullptr) {
		// ally_threat: don't stay behind shooting once the escort has moved on
		if (isNeedCover && !HasCover(unit, pos)) {
			SetTarget(nullptr);
			if (IsQueryReady(unit)) {
				FallbackNoSpot(unit, isUpdating);
			}
			return;
		}
		ahActivity = CMilitaryManager::AhNote::ENGAGE;
		if (isAntiHeavy) {
			SetTarget(bestTarget);  // for the other anti-heavy units' overkill check (GetOthersReadyDamage)
		}
		unit->GetTravelAct()->StateWait();
		TRY_UNIT(circuit, unit,
			unit->GetUnit()->Attack(bestTarget->GetUnit(), UNIT_COMMAND_OPTION_RIGHT_MOUSE_KEY, frame + FRAMES_PER_SEC * 60);
			unit->CmdSetTarget(bestTarget);
		)
		return;
	}
	SetTarget(nullptr);

	if (!IsQueryReady(unit)) {
		return;
	}

	if (enemyPositions.empty()) {
		FallbackNoTarget(unit, isUpdating);
		return;
	}

	CThreatMap* threatMap = circuit->GetThreatMap();
	const float range = std::max(unit->GetCircuitDef()->GetMaxRange(), (float)threatMap->GetSquareSize());

	// Walking to a firing spot: cheap checks on every Execute, a full search and path query every 6 s
	if (IsFireRouteValid(unit, range)) {
		ahActivity = CMilitaryManager::AhNote::HUNT;
		return;
	}

	// Goals: any cell within range of a building (old), or only covered firing cells (ally_threat)
	const F3Vec* goals = &enemyPositions;
	float goalRange = range;
	if (isNeedCover) {
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
			pos, goalRange, *goals, nullptr, GetMaxPathThreat(unit));
	pathQueries[unit] = query;
	query->HoldTask(this);

	pathfinder->RunQuery(query, [this, isUpdating](const IPathQuery* query) {
		if (this->IsQueryAlive(query)) {
			this->ApplyTargetPath(static_cast<const CQueryPathMulti*>(query), isUpdating);
		}
	});
}

CEnemyInfo* CArtilleryTask::FindTarget(CCircuitUnit* unit, const AIFloat3& pos)
{
	// Known enemy buildings only (anti-heavy: the heavies and commanders we hunt, mobile or not). Standing safe:
	// return the best one in range, else fill enemyPositions with those out of range (to walk to). Standing in
	// threat: never shoot, only fill enemyPositions
	CCircuitAI* circuit = manager->GetCircuit();
	CThreatMap* threatMap = circuit->GetThreatMap();
	CCircuitDef* cdef = unit->GetCircuitDef();
	const bool notAW = !cdef->HasAntiWater();
	const int canTargetCat = cdef->GetTargetCategory();
	const int noChaseCat = cdef->GetNoChaseCategory();
	const float sqRange = SQUARE(cdef->GetMaxRange());

	enemyPositions.clear();
	threatMap->SetThreatType(unit);
	float posThreat = threatMap->GetThreatAt(pos);
	if (isAntiHeavy && (posThreat > THREAT_MIN)) {
		posThreat -= GetIgnoredThreatAt(unit, pos);  // a heavy we can shoot now doesn't keep us from shooting
	}
	// Anti-heavy: also where our cover outweighs the threat (IsThreatOk), so it shoots behind the army in a fight
	const bool isPosSafe = cdef->IsPlane() || IsThreatOk(unit, pos, posThreat, false);
	// Anti-heavy: a long-reload weapon fires at what we see, not at a radar blip (a blip is still walked to)
	const bool isSeenOnly = isAntiHeavy && CAntiHeavyTask::IsLongReload(cdef);

	// Трубка 15, прицел 120, бац, бац …и мимо!
	// In range: builders first, then the strongest, each only if closer than the current pick; then anything.
	// Anti-heavy: mobile first, then builders (commanders), the strongest, the nearest; not one the other anti-heavy
	// units' ready shots will kill
	using Rank = std::tuple<bool, bool, float, float>;
	Rank bestRank(true, true, 0.f, std::numeric_limits<float>::max());
	float minSqDist = sqRange;
	float maxPower = 0.f;
	CEnemyInfo* bestTarget = nullptr;
	CEnemyInfo* mediumTarget = nullptr;
	CEnemyInfo* worstTarget = nullptr;
	for (auto& kv : circuit->GetEnemyInfos()) {
		CEnemyInfo* enemy = kv.second;
		if (!enemy->IsInRadarOrLOS() || (notAW && (enemy->GetPos().y < -SQUARE_SIZE * 5))) {
			continue;
		}
		CCircuitDef* edef = enemy->GetCircuitDef();
		if (edef == nullptr) {
			continue;
		}
		if (isAntiHeavy) {
			if (enemy->IsHidden() || !CAntiHeavyTask::IsQuarryFor(cdef, enemy)) {
				continue;
			}
		} else if (edef->IsMobile() || (isPosSafe && edef->IsAttrSiege())) {
			continue;
		}
		const int targetCat = edef->GetCategory();
		if ((targetCat & canTargetCat) == 0) {
			continue;
		}

		const float sqDist = pos.SqDistance2D(enemy->GetPos());
		if (sqDist < sqRange) {
			if (!isPosSafe) {
				continue;
			}
			if (isAntiHeavy) {
				if ((isSeenOnly && !enemy->IsInLOS())
					|| (GetOthersReadyDamage(enemy) > 1.1f * (enemy->GetHealth() + enemy->GetShieldPower())))
				{
					continue;
				}
				const Rank rank(!edef->IsMobile(), !edef->IsEnemyRoleAny(CCircuitDef::RoleMask::BUILDER),
						-edef->GetPower(), sqDist);
				if ((bestTarget == nullptr) || (rank < bestRank)) {
					bestRank = rank;
					bestTarget = enemy;
				}
				continue;
			}
			if (sqDist >= minSqDist) {
				continue;
			}
			if (edef->IsEnemyRoleAny(CCircuitDef::RoleMask::BUILDER)) {
				bestTarget = enemy;
				minSqDist = sqDist;
				maxPower = std::numeric_limits<float>::max();
			} else if (edef->GetPower() > maxPower) {
				bestTarget = enemy;
				minSqDist = sqDist;
				maxPower = edef->GetPower();
			} else if (bestTarget == nullptr) {
				if ((targetCat & noChaseCat) == 0) {
					mediumTarget = enemy;
				} else if (mediumTarget == nullptr) {
					worstTarget = enemy;
				}
			}
			continue;
		}

		if ((targetCat & noChaseCat) == 0) {
			enemyPositions.push_back(enemy->GetPos());
		}
	}

	if (bestTarget == nullptr) {
		bestTarget = (mediumTarget != nullptr) ? mediumTarget : worstTarget;
	}
	if (bestTarget != nullptr) {
		position = bestTarget->GetPos();
	}
	return bestTarget;
}

void CArtilleryTask::ApplyTargetPath(const CQueryPathMulti* query, bool isUpdating)
{
	const std::shared_ptr<PathInfo>& pPath = query->GetPathInfo();
	CCircuitUnit* unit = query->GetUnit();

	if (!pPath->posPath.empty()
		&& IsThreatOk(unit, pPath->posPath.back(),  // this unit's threat layer
				manager->GetCircuit()->GetThreatMap()->GetThreatAt(unit, pPath->posPath.back()), true))
	{
		position = pPath->posPath.back();
		unit->GetTravelAct()->SetPath(pPath);
		unit->GetTravelAct()->StateActivate();
		firePath = pPath;
		ahActivity = CMilitaryManager::AhNote::HUNT;
		replanFrame = manager->GetCircuit()->GetLastFrame() + FRAMES_PER_SEC * 6;
	} else {
		FallbackNoSpot(unit, isUpdating);
	}
}

bool CArtilleryTask::IsFireRouteValid(CCircuitUnit* unit, float range)
{
	// Until the next full re-plan, keep walking while the firing spot still is one (in range of a known building,
	// IsFireSpotValid) and the waypoints left stay under max_path_threat. Uses enemyPositions from FindTarget
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	ITravelAction* travelAct = unit->GetTravelAct();
	if ((firePath == nullptr) || (frame >= replanFrame) || (travelAct->GetPath() != firePath)
		|| !(travelAct->IsActive() || travelAct->IsFinished()))  // finished: the last waypoint is ordered, still walking
	{
		return false;
	}
	const F3Vec& route = firePath->posPath;
	const AIFloat3& spot = route.back();

	const float sqReach = SQUARE(range + 2 * circuit->GetPathfinder()->GetSquareSize());  // with the pathfinder's slack
	bool isInRange = false;
	for (const AIFloat3& target : enemyPositions) {
		if (spot.SqDistance2D(target) <= sqReach) {
			isInRange = true;
			break;
		}
	}
	if (!isInRange || !IsFireSpotValid(unit, spot)) {
		return false;
	}

	// The waypoints left stay under the cap. Not for a cloaked unit: the pathfinder plans its route on the
	// decloak layer, which GetThreatAt doesn't read (IsCloakRoute)
	const AIFloat3& pos = unit->GetPos(frame);
	const float maxPathThreat = GetMaxPathThreat(unit);
	if ((maxPathThreat < std::numeric_limits<float>::max()) && !IsCloakRoute(unit)) {
		// From the waypoint nearest to us
		size_t next = 0;
		float minSqDist = std::numeric_limits<float>::max();
		for (size_t i = 0; i < route.size(); ++i) {
			const float sqDist = pos.SqDistance2D(route[i]);
			if (sqDist < minSqDist) {
				minSqDist = sqDist;
				next = i;
			}
		}
		CThreatMap* threatMap = circuit->GetThreatMap();
		for (size_t i = next; i < route.size(); ++i) {
			if (threatMap->GetThreatAt(unit, route[i]) > maxPathThreat) {
				return false;
			}
		}
	}
	return true;
}

bool CArtilleryTask::IsFireSpotValid(CCircuitUnit* unit, const AIFloat3& spot)
{
	// Threat-free (anti-heavy: or IsThreatOk); with ally_threat on, also covered and not where the flee would fire on
	// arrival. FillCoveredSpots applies the same rules to many cells at once with grids
	CCircuitAI* circuit = manager->GetCircuit();
	if (!IsThreatOk(unit, spot, circuit->GetThreatMap()->GetThreatAt(unit, spot), true)) {
		return false;
	}
	if (GetCfg().allyThreat < 0.f) {
		return true;  // any spot (old behaviour)
	}
	if (!HasCover(unit, spot)) {
		return false;
	}
	const float fleeMargin = GetFleeMargin(unit);
	if (fleeMargin < 0.f) {
		return true;
	}
	CollectDangers(unit, fleeMargin + 2 * circuit->GetPathfinder()->GetSquareSize(), spot, spot);
	float dangerThreat;
	return (FindDanger(spot, dangerThreat) == nullptr) || HasCover(unit, spot, dangerThreat, true);
}

float CArtilleryTask::FillCoveredSpots(CCircuitUnit* unit, float range)
{
	// firePositions: cells (one per pathfinder square, 64 elmos) within range of a building in enemyPositions
	// that have ally_threat cover, no threat, can be reached and won't set off the flee on arrival.
	// Returns the goal range for the path query: the pathfinder accepts any square within one square of a cell centre.
	// The work is bounded to the area around our mobile escorts: no cell elsewhere can be covered
	firePositions.clear();

	CCircuitAI* circuit = manager->GetCircuit();
	const CMilitaryManager::SArtilleryCfg& artyCfg = GetCfg();
	const int cellSize = circuit->GetPathfinder()->GetSquareSize();
	const float goalRange = cellSize;
	const int mapWidth = CTerrainManager::GetTerrainWidth() / cellSize + 1;
	const int mapHeight = CTerrainManager::GetTerrainHeight() / cellSize + 1;

	// Firing cell centres stay (range - 2 cells) from the building, so any spot the pathfinder may stop at
	// (up to ~1.5 cells off) still reaches it
	const float fireRange = std::max(range - 2 * cellSize, goalRange);
	const float sqFireRange = SQUARE(fireRange);
	const int fireRad = int(fireRange / cellSize) + 1;

	// Escorts: mobile ones cover a firing spot; against the flee static defences count too, as in CheckFlee.
	// Anti-heavy: a cell with threat is fine where that cover outweighs it, up to the route cap (IsThreatOk)
	const bool isCoverNeeded = (artyCfg.allyThreat > 0.f);
	const float fleeMargin = GetFleeMargin(unit);
	const bool isFleeOn = (fleeMargin >= 0.f);
	const bool isThreatCover = isAntiHeavy && (artyCfg.allyThreat >= 0.f);
	const float maxCellThreat = isThreatCover ? GetMaxPathThreat(unit) : THREAT_MIN;
	if (isCoverNeeded || isFleeOn || isThreatCover) {
		CollectEscorts(unit, isFleeOn);
	} else {
		escorts.clear();
	}

	// Area, in cells, inclusive: around the mobile escorts; ally_threat 0 needs no cover, so the whole map
	const float allyRadius = artyCfg.allyRadius;
	const float sqAllyRadius = SQUARE(allyRadius);
	const int allyRad = int(allyRadius / cellSize) + 1;
	int x0 = 0, z0 = 0, x1 = mapWidth - 1, z1 = mapHeight - 1;
	if (isCoverNeeded) {
		x0 = mapWidth; z0 = mapHeight; x1 = -1; z1 = -1;
		for (const SEscort& escort : escorts) {
			if (escort.isMobile) {
				const int ax = int(escort.pos.x) / cellSize;
				const int az = int(escort.pos.z) / cellSize;
				x0 = std::min(x0, ax - allyRad);  x1 = std::max(x1, ax + allyRad);
				z0 = std::min(z0, az - allyRad);  z1 = std::max(z1, az + allyRad);
			}
		}
		x0 = std::max(x0, 0);  x1 = std::min(x1, mapWidth - 1);
		z0 = std::max(z0, 0);  z1 = std::min(z1, mapHeight - 1);
		if ((x0 > x1) || (z0 > z1)) {
			return goalRange;  // no mobile escort
		}
	}
	const int width = x1 - x0 + 1;
	const int height = z1 - z0 + 1;
	auto cellIndex = [x0, z0, width](int x, int z) { return (z - z0) * width + (x - x0); };
	auto cellCentre = [cellSize](int x, int z) {
		return AIFloat3((x + 0.5f) * cellSize, 0.f, (z + 0.5f) * cellSize);
	};

	// Flee zones over the area. Reach + 2 cells: the unit may stop up to ~1.5 cells from a cell centre
	if (isFleeOn) {
		CollectDangers(unit, fleeMargin + 2 * cellSize,
				AIFloat3(x0 * cellSize, 0.f, z0 * cellSize), AIFloat3((x1 + 1) * cellSize, 0.f, (z1 + 1) * cellSize));
	} else {
		dangers.clear();
	}
	const bool isFleeCheck = !dangers.empty();
	const bool isFleeGrid = isFleeCheck || isThreatCover;

	// Escort power per cell, by cell centre
	if (isCoverNeeded || isFleeGrid) {
		coverGrid.assign(width * height, 0.f);
		fleeCoverGrid.assign(isFleeGrid ? width * height : 0, 0.f);
		for (const SEscort& escort : escorts) {
			const int ax = int(escort.pos.x) / cellSize;
			const int az = int(escort.pos.z) / cellSize;
			for (int z = std::max(az - allyRad, z0); z <= std::min(az + allyRad, z1); ++z) {
				for (int x = std::max(ax - allyRad, x0); x <= std::min(ax + allyRad, x1); ++x) {
					if (cellCentre(x, z).SqDistance2D(escort.pos) > sqAllyRadius) {
						continue;
					}
					const int index = cellIndex(x, z);
					if (escort.isMobile) {
						coverGrid[index] += escort.power;
					}
					if (isFleeGrid) {
						fleeCoverGrid[index] += escort.power;
					}
				}
			}
		}
	}

	CThreatMap* threatMap = circuit->GetThreatMap();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	STerrainMapArea* area = unit->GetArea();
	const float sqReach = SQUARE(fireRange + allyRadius + cellSize);  // a building this far from every escort has no covered cell
	cellAdded.assign(width * height, 0);
	for (const AIFloat3& target : enemyPositions) {
		if (isCoverNeeded) {
			bool isNearEscort = false;
			for (const SEscort& escort : escorts) {
				if (escort.isMobile && (escort.pos.SqDistance2D(target) <= sqReach)) {
					isNearEscort = true;
					break;
				}
			}
			if (!isNearEscort) {
				continue;
			}
		}
		const int tx = int(target.x) / cellSize;
		const int tz = int(target.z) / cellSize;
		for (int z = std::max(tz - fireRad, z0); z <= std::min(tz + fireRad, z1); ++z) {
			for (int x = std::max(tx - fireRad, x0); x <= std::min(tx + fireRad, x1); ++x) {
				const int index = cellIndex(x, z);
				if (cellAdded[index] || (isCoverNeeded && (coverGrid[index] < artyCfg.allyThreat))) {
					continue;
				}
				AIFloat3 cellPos = cellCentre(x, z);
				if (cellPos.SqDistance2D(target) > sqFireRange) {
					continue;
				}
				cellAdded[index] = 1;  // checked once, whatever the outcome
				// A spot inside a flee zone without the cover to hold there: walk in, flee, walk back
				float dangerThreat;
				if (isFleeCheck && (FindDanger(cellPos, dangerThreat) != nullptr)
					&& (fleeCoverGrid[index] < artyCfg.allyThreat + dangerThreat))
				{
					continue;
				}
				CTerrainManager::CorrectPosition(cellPos);
				// Only threat-free (anti-heavy: covered, within the route cap) cells the unit can reach: otherwise the
				// nearest goal may be one that gets rejected afterwards (threat), or the search may sweep the whole map
				// (unreachable)
				const float cellThreat = threatMap->GetThreatAt(unit, cellPos);
				const bool isThreatOk = (cellThreat <= THREAT_MIN)
						|| (isThreatCover && (cellThreat <= maxCellThreat)
							&& (fleeCoverGrid[index] - cellThreat >= artyCfg.allyThreat));
				if (isThreatOk && terrainMgr->CanMoveToPos(area, cellPos)) {
					firePositions.push_back(cellPos);
				}
			}
		}
	}
	return goalRange;
}

/*
 * Nothing to shoot or walk to
 */

void CArtilleryTask::FallbackNoTarget(CCircuitUnit* unit, bool isUpdating)
{
	ahActivity = CMilitaryManager::AhNote::PARK;
	if (GetCfg().isNoScout) {
		FallbackStay(unit, isUpdating);
	} else {
		FallbackScout(unit, isUpdating);
	}
}

void CArtilleryTask::FallbackNoSpot(CCircuitUnit* unit, bool isUpdating)
{
	ahActivity = CMilitaryManager::AhNote::PARK;
	if (GetCfg().isNoScout) {
		FallbackStay(unit, isUpdating);
	} else {
		FallbackBasePos(unit, isUpdating);
	}
}

void CArtilleryTask::FallbackStay(CCircuitUnit* unit, bool isUpdating)
{
	// Stay put while the spot is threat-free (anti-heavy: or IsThreatOk) and (ally_threat) still covered, otherwise
	// move to safety.
	// Re-checked every few updates by Execute
	CCircuitAI* circuit = manager->GetCircuit();
	const AIFloat3& pos = unit->GetPos(circuit->GetLastFrame());
	const bool isSafe = IsThreatOk(unit, pos, circuit->GetThreatMap()->GetThreatAt(unit, pos), false);
	if (isSafe && ((GetCfg().allyThreat < 0.f) || HasCover(unit, pos))) {
		HoldHere(unit);
	} else {
		FallbackSafePos(unit, isUpdating);
	}
}

void CArtilleryTask::HoldHere(CCircuitUnit* unit)
{
	ITravelAction* travelAct = unit->GetTravelAct();
	// Finished: the last waypoint is ordered and the unit may still be walking (the action list ends a finishing
	// travel at once, so its state is HALT by now, not FINISH)
	if (travelAct->IsActive() || travelAct->IsFinished()) {
		// Walking somewhere that is no longer wanted: the engine still holds queued waypoints, stop here
		CCircuitAI* circuit = manager->GetCircuit();
		TRY_UNIT(circuit, unit,
			unit->GetUnit()->Stop();
		)
	}
	travelAct->StateWait();
}

void CArtilleryTask::ApplyPathOrHold(CCircuitUnit* unit, const std::shared_ptr<PathInfo>& pPath)
{
	// A path of 1-2 points means we are already there: a move order would finish at once, make the unit idle
	// and restart the task, so hold instead
	if (pPath->posPath.size() > 2) {
		position = pPath->posPath.back();
		unit->GetTravelAct()->SetPath(pPath);
		unit->GetTravelAct()->StateActivate();
	} else {
		HoldHere(unit);
	}
}

void CArtilleryTask::FillSafeSpots(CCircuitUnit* unit)
{
	CCircuitAI* circuit = manager->GetCircuit();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();

	// Nearest attack/defend group leaders, else a defended mex cluster, else the base
	circuit->GetMilitaryManager()->FillSafePos(unit, safePositions);

	// Stand behind the leader, on the side towards our base
	const float followDist = GetCfg().followDist;
	if (followDist <= 0.f) {
		return;
	}
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

void CArtilleryTask::FallbackSafePos(CCircuitUnit* unit, bool isUpdating)
{
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	FillSafeSpots(unit);

	CPathFinder* pathfinder = circuit->GetPathfinder();
	std::shared_ptr<IPathQuery> query = pathfinder->CreatePathMultiQuery(
			unit, circuit->GetThreatMap(), frame,
			unit->GetPos(frame), DEFAULT_SLACK * 4, safePositions);
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
	if (pPath->posPath.empty()) {
		FallbackBasePos(unit, isUpdating);
	} else {
		ApplyPathOrHold(unit, pPath);
	}
}

void CArtilleryTask::FallbackBasePos(CCircuitUnit* unit, bool isUpdating)
{
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	position = circuit->GetSetupManager()->GetBasePos();

	CPathFinder* pathfinder = circuit->GetPathfinder();
	std::shared_ptr<IPathQuery> query = pathfinder->CreatePathSingleQuery(
			unit, circuit->GetThreatMap(), frame,
			unit->GetPos(frame), position, DEFAULT_SLACK * 4);
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

	if (GetCfg().isNoScout) {
		ApplyPathOrHold(unit, pPath);  // never scout: no path means stay put
	} else if (!pPath->posPath.empty()) {
		unit->GetTravelAct()->SetPath(pPath);
		unit->GetTravelAct()->StateActivate();
	} else {
		FallbackScout(unit, isUpdating);
	}
}

/*
 * Old fallbacks (no_scout false): scout enemy mexes, else fight-move to a random spot
 */

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

	CPathFinder* pathfinder = circuit->GetPathfinder();
	std::shared_ptr<IPathQuery> query = pathfinder->CreatePathSingleQuery(
			unit, circuit->GetThreatMap(), frame,
			pos, position, DEFAULT_SLACK * 4);
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

/*
 * Early flee (artillery_flee_margin)
 */

bool CArtilleryTask::CheckFlee(CCircuitUnit* unit)
{
	// Flee when an enemy that can hit us comes within its weapon range + margin, unless (ally_threat) our cover
	// here outweighs the enemies that set it off. Returns true while fleeing
	const float fleeMargin = GetFleeMargin(unit);
	if ((fleeMargin < 0.f) || (unit->Blocker() != nullptr)) {
		isFleeing = false;
		return false;
	}

	const int frame = manager->GetCircuit()->GetLastFrame();
	const AIFloat3& pos = unit->GetPos(frame);
	CollectDangers(unit, fleeMargin, pos, pos);
	float dangerThreat;
	const SDanger* danger = FindDanger(pos, dangerThreat);
	// Cover net of those enemies: an escort they outweigh doesn't hold the artillery in place
	if ((danger == nullptr) || ((GetCfg().allyThreat >= 0.f) && HasCover(unit, pos, dangerThreat, true))) {
		if (isFleeing && isAntiHeavy) {
			manager->GetCircuit()->GetMilitaryManager()->NoteAh(unit->GetCircuitDef(), CMilitaryManager::AhNote::FLEE_END);
		}
		isFleeing = false;
		return false;
	}

	// On the way: a flee planned in the last 2 s, or a flee path still being planned (also after a moment of
	// cover). Any other path still being planned (Execute's: it may lead towards the enemy) is replaced at once
	ahActivity = CMilitaryManager::AhNote::FLEE;
	SetTarget(nullptr);  // not attacking: no ready shot for the others' overkill check
	const bool isRecentFlee = (frame < fleeFrame);
	if ((isFleeing && isRecentFlee) || ((isFleeing || isRecentFlee) && !IsQueryReady(unit))) {
		return true;
	}
	isFleeing = true;
	fleeFrame = frame + FRAMES_PER_SEC * 2;  // re-plan the flee every 2 s while the danger stays
	const AIFloat3 dangerPos = danger->pos;
	Flee(unit, dangerPos);
	return true;
}

void CArtilleryTask::CollectDangers(CCircuitUnit* unit, float margin, const AIFloat3& minPos, const AIFloat3& maxPos)
{
	dangers.clear();
	for (auto& kv : manager->GetCircuit()->GetEnemyInfos()) {
		CEnemyInfo* enemy = kv.second;
		float eRange;
		if (!IsGroundDanger(enemy, true, eRange)) {  // gunships count for artillery
			continue;
		}
		const AIFloat3& ePos = enemy->GetPos();
		const float sqReach = SQUARE(eRange + margin);
		// Distance to the rectangle (0 inside)
		const float dx = std::max(std::max(minPos.x - ePos.x, ePos.x - maxPos.x), 0.f);
		const float dz = std::max(std::max(minPos.z - ePos.z, ePos.z - maxPos.z), 0.f);
		if ((SQUARE(dx) + SQUARE(dz) <= sqReach) && !IsQuarryIgnored(unit, enemy)) {
			dangers.push_back({ePos, sqReach, enemy->GetThreat()});
		}
	}
}

const CArtilleryTask::SDanger* CArtilleryTask::FindDanger(const AIFloat3& pos, float& outThreat)
{
	const SDanger* nearest = nullptr;
	float minSqDist = std::numeric_limits<float>::max();
	outThreat = 0.f;
	for (const SDanger& danger : dangers) {
		const float sqDist = pos.SqDistance2D(danger.pos);
		if (sqDist <= danger.sqReach) {
			outThreat += danger.threat;
			if (sqDist < minSqDist) {
				minSqDist = sqDist;
				nearest = &danger;
			}
		}
	}
	return nearest;
}

void CArtilleryTask::Flee(CCircuitUnit* unit, const AIFloat3& dangerPos)
{
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	const AIFloat3& pos = unit->GetPos(frame);

	// Candidates: safety spots (behind groups, else defended mex cluster, else base) and the base
	FillSafeSpots(unit);
	const AIFloat3& basePos = circuit->GetSetupManager()->GetBasePos();
	if (circuit->GetTerrainManager()->CanMoveToPos(unit->GetArea(), basePos)) {
		safePositions.push_back(basePos);
	}
	const AIFloat3* best = PickFleeSpot(pos, dangerPos, safePositions);
	if (best == nullptr) {
		FallbackBasePos(unit, true);
		return;
	}
	position = *best;

	CPathFinder* pathfinder = circuit->GetPathfinder();
	std::shared_ptr<IPathQuery> query = pathfinder->CreatePathSingleQuery(
			unit, circuit->GetThreatMap(), frame,
			pos, position, DEFAULT_SLACK * 4, nullptr, GetMaxPathThreat(unit));
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
	if (pPath->posPath.empty()) {
		FallbackBasePos(unit, true);  // no path under the threat cap: any path home
	} else {
		ApplyPathOrHold(unit, pPath);  // already there: nowhere better to go
	}
}

/*
 * Anti-heavy mode
 */

bool CArtilleryTask::IsQuarryIgnored(CCircuitUnit* unit, CEnemyInfo* enemy)
{
	if (!isAntiHeavy) {
		return false;
	}
	CCircuitDef* cdef = unit->GetCircuitDef();
	if (!CAntiHeavyTask::IsQuarryFor(cdef, enemy)) {
		return false;
	}
	const int frame = manager->GetCircuit()->GetLastFrame();
	if (!unit->HasWeapon() || !unit->IsWeaponReady(frame)) {
		return false;  // reloading: step back like from any other enemy
	}
	CCircuitDef* edef = enemy->GetCircuitDef();
	if (!edef->IsAttacker() || (edef->HasAntiAir() && !edef->HasAntiLand() && !edef->HasAntiWater())) {
		return true;  // it can't shoot back
	}
	const float range = cdef->GetMaxRange();
	const float eRange = std::max(edef->GetMaxRange(CCircuitDef::RangeType::LAND),
			edef->GetMaxRange(CCircuitDef::RangeType::WATER));
	if (eRange < range) {
		return true;  // we out-range it
	}
	return unit->GetPos(frame).SqDistance2D(enemy->GetPos()) <= SQUARE(range);  // it out-ranges us: once in our range
}

float CArtilleryTask::GetIgnoredThreatAt(CCircuitUnit* unit, const AIFloat3& pos)
{
	// The threat map draws an enemy's threat out to its range (threat-map squares) on our layer; one more square of
	// slack. Its threat is taken off only where its ring reaches: elsewhere that would hide other threat (turrets)
	CCircuitAI* circuit = manager->GetCircuit();
	const float squareSize = circuit->GetThreatMap()->GetSquareSize();
	const bool isAmph = unit->GetCircuitDef()->IsAmphibious();
	float threat = 0.f;
	for (auto& kv : circuit->GetEnemyInfos()) {
		CEnemyInfo* enemy = kv.second;
		if (enemy->IsHidden() || !enemy->IsInRadarOrLOS() || !IsQuarryIgnored(unit, enemy)) {
			continue;
		}
		const CEnemyUnit* data = enemy->GetData();
		const int range = isAmph
				? std::max(data->GetRange(CCircuitDef::ThreatType::LAND), data->GetRange(CCircuitDef::ThreatType::WATER))
				: data->GetRange(CCircuitDef::ThreatType::LAND);
		if ((range > 0) && (pos.SqDistance2D(enemy->GetPos()) <= SQUARE((range + 1) * squareSize))) {
			threat += enemy->GetThreat();
		}
	}
	return threat;
}

bool CArtilleryTask::IsThreatOk(CCircuitUnit* unit, const AIFloat3& spot, float threat, bool isWalk)
{
	if (threat <= THREAT_MIN) {
		return true;
	}
	if (!isAntiHeavy || (GetCfg().allyThreat < 0.f) || (isWalk && (threat > GetMaxPathThreat(unit)))) {
		return false;
	}
	return HasCover(unit, spot, threat, true);
}

float CArtilleryTask::GetOthersReadyDamage(CEnemyInfo* enemy) const
{
	// Anti-heavy artillery tasks and anti-heavy squads that target it (IFighterTask::SetTarget)
	const int frame = manager->GetCircuit()->GetLastFrame();
	const AIFloat3& ePos = enemy->GetPos();
	float damage = 0.f;
	for (IFighterTask* task : enemy->GetTasks()) {
		if ((task == this) || !((task->GetFightType() == FightType::AH)
			|| ((task->GetFightType() == FightType::ARTY) && static_cast<CArtilleryTask*>(task)->IsAntiHeavy())))
		{
			continue;
		}
		for (CCircuitUnit* unit : task->GetAssignees()) {
			CCircuitDef* udef = unit->GetCircuitDef();
			if (unit->HasWeapon() && unit->IsWeaponReady(frame)
				&& (unit->GetPos(frame).SqDistance2D(ePos) <= SQUARE(udef->GetMaxRange())))
			{
				damage += udef->GetShotDamage();
			}
		}
	}
	return damage;
}

/*
 * Cover
 */

bool CArtilleryTask::IsEscort(CCircuitUnit* unit, CAllyUnit* ally, bool isFlee) const
{
	// Allied armed units that can fight ground or water units: mobile ones but not artillery (main role, so
	// artillery can't escort itself) nor anti-heavy units with artillery behaviour (IsAhArty), planes, anti-air only
	// units or mobile bombs (no ground/water weapon).
	// For the flee check, static defences that can hit ground count too (firing spots look for a mobile escort)
	CCircuitDef* adef = ally->GetCircuitDef();
	if ((ally->GetId() == unit->GetId()) || (adef == nullptr) || !adef->IsAttacker() || adef->IsPlane()) {
		return false;
	}
	if (!adef->IsMobile()) {
		return isFlee && IsStaticDefence(adef);
	}
	return (adef->GetMainRole() != ROLE_TYPE(ARTY)) && (adef->HasAntiLand() || adef->HasAntiWater())
			&& !manager->GetCircuit()->GetMilitaryManager()->IsAhArty(adef);
}

void CArtilleryTask::CollectEscorts(CCircuitUnit* unit, bool isFlee)
{
	// Escorts (IsEscort) with their power (pwr_mod, not thr_mod) at full health: no per-unit health reads,
	// this runs over every friendly unit
	escorts.clear();
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	circuit->UpdateFriendlyUnits();  // drop units that died since the last refresh
	for (auto& kv : circuit->GetFriendlyUnits()) {
		CAllyUnit* ally = kv.second;
		if (!IsEscort(unit, ally, isFlee)) {
			continue;
		}
		const AIFloat3& allyPos = ally->GetPos(frame);
		if (utils::is_valid(allyPos)) {
			CCircuitDef* adef = ally->GetCircuitDef();
			escorts.push_back({allyPos, adef->GetPower(), adef->IsMobile()});
		}
	}
}

bool CArtilleryTask::HasCover(CCircuitUnit* unit, const AIFloat3& spot, float enemyThreat, bool isFlee)
{
	// Escort power near spot, net of enemyThreat, reaches ally_threat. One pass that stops as soon as it does.
	// Slack: firing spots are picked by cell centre, and the unit may stop up to ~1.5 cells away, so without
	// slack it could arrive, find no cover and turn back
	CCircuitAI* circuit = manager->GetCircuit();
	const CMilitaryManager::SArtilleryCfg& artyCfg = GetCfg();
	const float need = artyCfg.allyThreat + enemyThreat;
	if (need <= 0.f) {
		return true;
	}
	const float sqRadius = SQUARE(artyCfg.allyRadius + circuit->GetPathfinder()->GetSquareSize() * 2);
	const int frame = circuit->GetLastFrame();
	circuit->UpdateFriendlyUnits();  // drop units that died since the last refresh
	float cover = 0.f;
	for (auto& kv : circuit->GetFriendlyUnits()) {
		CAllyUnit* ally = kv.second;
		if (!IsEscort(unit, ally, isFlee)) {
			continue;
		}
		const AIFloat3& allyPos = ally->GetPos(frame);
		if (utils::is_valid(allyPos) && (allyPos.SqDistance2D(spot) <= sqRadius)) {
			cover += ally->GetCircuitDef()->GetPower();
			if (cover >= need) {
				return true;
			}
		}
	}
	return false;
}

} // namespace circuit

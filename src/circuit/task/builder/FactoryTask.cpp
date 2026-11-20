/*
 * FactoryTask.cpp
 *
 *  Created on: Jan 30, 2015
 *      Author: rlcevg
 */

#include "task/builder/FactoryTask.h"
#include "task/TaskManager.h"
#include "module/BuilderManager.h"
#include "module/FactoryManager.h"
#include "terrain/TerrainManager.h"
#include "unit/ally/AllyUnit.h"
#include "CircuitAI.h"
#include "util/Scheduler.h"
#include "util/Utils.h"

#include "spring/SpringMap.h"

#include "AISCommands.h"

namespace circuit {

using namespace springai;

static int opposite[] = {
	UNIT_FACING_NORTH,
	UNIT_FACING_WEST,
	UNIT_FACING_SOUTH,
	UNIT_FACING_EAST
};

CBFactoryTask::CBFactoryTask(ITaskManager* mgr, Priority priority,
							 CCircuitDef* buildDef, const AIFloat3& position,
							 float cost, float shake, bool isPlop, int timeout)
		: IBuilderTask(mgr, priority, buildDef, position, Type::BUILDER, BuildType::FACTORY, cost, shake, timeout)
		, isPlop(isPlop)
		, stallFrame(-1)
{
	manager->GetCircuit()->GetFactoryManager()->AddFactory(buildDef);
}

CBFactoryTask::~CBFactoryTask()
{
}

void CBFactoryTask::Start(CCircuitUnit* unit)
{
	if (isPlop) {
		Execute(unit);
	} else {
		IBuilderTask::Start(unit);
	}
}

void CBFactoryTask::Update()
{
	if (!isPlop) {
		IBuilderTask::Update();
		return;
	}

	// A plop is instant once the order starts, so a commander in build range with no frame placed is stuck: its site is
	// blocked, most often by an allied commander standing there with orders of its own (it won't bugger off), or by an
	// ally's factory placed meanwhile. The engine would wait out the order's 60 s timeout; pick another site instead
	if ((target != nullptr) || units.empty() || !utils::is_valid(buildPos)) {
		stallFrame = -1;
		return;
	}
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	CCircuitUnit* unit = *units.begin();
	const float halfSize = std::max(buildDef->GetDef()->GetXSize(), buildDef->GetDef()->GetZSize()) * SQUARE_SIZE / 2;
	const float range = unit->GetCircuitDef()->GetBuildDistance() + halfSize * 1.42f + unit->GetCircuitDef()->GetRadius();
	if (unit->GetPos(frame).SqDistance2D(buildPos) > SQUARE(range)) {
		stallFrame = -1;
		return;
	}
	if (stallFrame < 0) {
		stallFrame = frame;
	} else if (frame - stallFrame >= FRAMES_PER_SEC * 4) {
		ReplaceSite(unit);
	}
}

void CBFactoryTask::OnUnitIdle(CCircuitUnit* unit)
{
	// The order ended without a frame (timed out, or the engine dropped it): don't retry a site that is now blocked
	if (isPlop && (target == nullptr) && utils::is_valid(buildPos)
		&& (IsOccupied(buildPos, facing)
			|| !manager->GetCircuit()->GetMap()->IsPossibleToBuildAt(buildDef->GetDef(), buildPos, facing)))
	{
		ReplaceSite(unit);
		return;
	}
	IBuilderTask::OnUnitIdle(unit);
}

void CBFactoryTask::ReplaceSite(CCircuitUnit* unit)
{
	stallFrame = -1;
	if (utils::is_valid(buildPos)) {
		badSites.push_back(buildPos);
		SetBuildPos(-RgtVector);
	}
	if (badSites.size() > 8) {  // bounded: nothing near fits, take any site again rather than none
		badSites.erase(badSites.begin());
	}
	Execute(unit);  // FindBuildSite: not a bad site, nobody standing in it
}

bool CBFactoryTask::IsOccupied(const AIFloat3& pos, int face) const
{
	CCircuitAI* circuit = manager->GetCircuit();
	UnitDef* def = buildDef->GetDef();
	const float hx = (((face & 1) == 0) ? def->GetXSize() : def->GetZSize()) * SQUARE_SIZE / 2;
	const float hz = (((face & 1) == 0) ? def->GetZSize() : def->GetXSize()) * SQUARE_SIZE / 2;
	const int frame = circuit->GetLastFrame();
	circuit->UpdateFriendlyUnits();
	auto friendlies = circuit->GetCallback()->GetFriendlyUnitsIn(pos, std::max(hx, hz) * 1.42f + 64.f);
	bool isOccupied = false;
	for (springai::Unit* u : friendlies) {
		CAllyUnit* alu = circuit->GetFriendlyUnit(u);
		if ((alu == nullptr) || !alu->GetCircuitDef()->IsMobile()) {
			continue;  // structures are on the blocking map and IsPossibleToBuildAt already
		}
		const AIFloat3& p = alu->GetPos(frame);
		const float r = alu->GetCircuitDef()->GetRadius() + SQUARE_SIZE;
		if ((std::fabs(p.x - pos.x) < hx + r) && (std::fabs(p.z - pos.z) < hz + r)) {
			isOccupied = true;
			break;
		}
	}
	utils::free_clear(friendlies);
	return isOccupied;
}

void CBFactoryTask::Cancel()
{
	IBuilderTask::Cancel();

	if (target == nullptr) {
		manager->GetCircuit()->GetFactoryManager()->DelFactory(buildDef);
	}
}

void CBFactoryTask::FindBuildSite(CCircuitUnit* builder, const AIFloat3& pos, float searchRadius)
{
	CCircuitAI* circuit = manager->GetCircuit();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();

//	facing = UNIT_COMMAND_BUILD_NO_FACING;
	float terWidth = terrainMgr->GetTerrainWidth();
	float terHeight = terrainMgr->GetTerrainHeight();
	if (math::fabs(terWidth - 2 * pos.x) > math::fabs(terHeight - 2 * pos.z)) {
		facing = (2 * pos.x > terWidth) ? UNIT_FACING_WEST : UNIT_FACING_EAST;
	} else {
		facing = (2 * pos.z > terHeight) ? UNIT_FACING_NORTH : UNIT_FACING_SOUTH;
	}

	const float badSqDist = SQUARE(std::max(buildDef->GetDef()->GetXSize(), buildDef->GetDef()->GetZSize()) * SQUARE_SIZE);
	CTerrainManager::TerrainPredicate predicate = [this, terrainMgr, builder, badSqDist](const AIFloat3& p) {
		if (!terrainMgr->CanReachAtSafe(builder, p, builder->GetCircuitDef()->GetBuildDistance())) {
			return false;
		}
		if (!isPlop) {
			return true;
		}
		// Plop sites: not where the order stalled before, and not over a friendly unit (an allied commander in a
		// crowded start box) that would keep the commander waiting for it to move
		for (const AIFloat3& bad : badSites) {
			if (bad.SqDistance2D(p) < badSqDist) {
				return false;
			}
		}
		return !IsOccupied(p, facing);
	};
	CMap* map = circuit->GetMap();
	auto checkFacing = [this, map, terrainMgr, &predicate, &pos, searchRadius]() {
		AIFloat3 bp = terrainMgr->FindBuildSite(buildDef, pos, searchRadius, facing, predicate);
		if (!utils::is_valid(bp)) {
			return false;
		}

		// decides if a factory should face the opposite direction due to bad terrain
		AIFloat3 posOffset = bp;
		const float size = DEFAULT_SLACK;
		switch (facing) {
			default:
			case UNIT_FACING_SOUTH: {  // z++
				posOffset.z += size;
			} break;
			case UNIT_FACING_EAST: {  // x++
				posOffset.x += size;
			} break;
			case UNIT_FACING_NORTH: {  // z--
				posOffset.z -= size;
			} break;
			case UNIT_FACING_WEST: {  // x--
				posOffset.x -= size;
			} break;
		}
		if (map->IsPossibleToBuildAt(buildDef->GetDef(), posOffset, facing)) {
			SetBuildPos(bp);
			return true;
		}
		return false;
	};

	if (checkFacing()) {
		return;
	}
	facing = opposite[facing];
	if (checkFacing()) {
		return;
	}
	++facing %= 4;
	if (checkFacing()) {
		return;
	}
	facing = opposite[facing];
	checkFacing();
}

} // namespace circuit

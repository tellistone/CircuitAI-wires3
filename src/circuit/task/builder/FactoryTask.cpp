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

#include "spring/SpringCallback.h"
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
		, checkFrame(0)
		, blockersFrame(-1)
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

	// Engine: a builder whose footprint has a mobile unit in it asks that unit to bugger off, but a unit busy with its own
	// job (an allied commander building, a unit with nowhere to step in a crowd) stays, and the builder gives up after
	// 30 retries (~15 s); then the plain retries pick the same site again (IsPossibleToBuildAt ignores mobile units).
	// So: a commander in range of a site that stays occupied gets another site
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	if ((target != nullptr) || units.empty() || !utils::is_valid(buildPos) || (frame < checkFrame)) {
		return;
	}
	checkFrame = frame + FRAMES_PER_SEC / 2;
	CCircuitUnit* unit = *units.begin();
	const float range = unit->GetCircuitDef()->GetBuildDistance() + unit->GetCircuitDef()->GetRadius()
			+ std::max(buildDef->GetDef()->GetXSize(), buildDef->GetDef()->GetZSize()) * SQUARE_SIZE / 2 * 1.42f;
	if ((unit->GetPos(frame).SqDistance2D(buildPos) > SQUARE(range)) || !IsOccupied(buildPos, facing, unit)) {
		stallFrame = -1;  // walking there, or the engine's bugger-off has cleared it
	} else if (stallFrame < 0) {
		stallFrame = frame;
	} else if (frame - stallFrame >= FRAMES_PER_SEC * 3) {
		stallFrame = -1;
		SetBuildPos(-RgtVector);
		Execute(unit);  // FindBuildSite skips occupied footprints, this one included
	}
}

bool CBFactoryTask::IsOccupied(const AIFloat3& pos, int face, CCircuitUnit* builder)
{
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	if (blockersFrame != frame) {  // one snapshot per frame: a site search tests hundreds of footprints
		blockersFrame = frame;
		blockers.clear();
		circuit->UpdateFriendlyUnits();
		for (const auto& kv : circuit->GetFriendlyUnits()) {  // ours and allies'
			CAllyUnit* alu = kv.second;
			if (alu->GetCircuitDef()->IsMobile()) {  // structures: blocking map and IsPossibleToBuildAt
				blockers.push_back({alu->GetId(), alu->GetPos(frame), alu->GetCircuitDef()->GetRadius() + SQUARE_SIZE});
			}
		}
	}
	UnitDef* def = buildDef->GetDef();
	const float hx = (((face & 1) == 0) ? def->GetXSize() : def->GetZSize()) * SQUARE_SIZE / 2;
	const float hz = (((face & 1) == 0) ? def->GetZSize() : def->GetXSize()) * SQUARE_SIZE / 2;
	for (const SBlocker& b : blockers) {
		if ((b.id != builder->GetId()) && (std::fabs(b.pos.x - pos.x) < hx + b.radius)
			&& (std::fabs(b.pos.z - pos.z) < hz + b.radius))
		{
			return true;
		}
	}
	return false;
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

	// Plop: not over another friendly unit (an allied commander in a crowded start box) that may never step aside
	bool isAvoidUnits = isPlop;
	CTerrainManager::TerrainPredicate predicate = [this, terrainMgr, builder, &isAvoidUnits](const AIFloat3& p) {
		return terrainMgr->CanReachAtSafe(builder, p, builder->GetCircuitDef()->GetBuildDistance())
				&& !(isAvoidUnits && IsOccupied(p, facing, builder));
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

	const int startFacing = facing;
	auto checkFacings = [this, &checkFacing, startFacing]() {
		facing = startFacing;
		if (checkFacing()) {
			return true;
		}
		facing = opposite[facing];
		if (checkFacing()) {
			return true;
		}
		++facing %= 4;
		if (checkFacing()) {
			return true;
		}
		facing = opposite[facing];
		return checkFacing();
	};

	if (!checkFacings() && isAvoidUnits) {
		isAvoidUnits = false;  // all taken: a site to wait on beats Execute's fallback, a random base and no factory
		checkFacings();
	}
}

} // namespace circuit

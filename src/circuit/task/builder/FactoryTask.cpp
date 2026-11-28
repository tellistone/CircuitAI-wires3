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
#include "Command.h"
#include "Log.h"

namespace circuit {

using namespace springai;

static constexpr int ENGINE_CMD_RECLAIM = 90;  // engine's CMD_RECLAIM

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
		, stallPos(-RgtVector)
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

	// A plop is instant once the order starts, so a commander that stands still for 1.5 s with no frame placed is stuck,
	// whatever the cause: a unit in the footprint that won't step aside, a path that allied commanders block, an order
	// the engine dropped (the AI may never hear it go idle). The engine itself would retry for ~15 s, then go idle, and
	// the plain retries pick the same site again. So: drop the site, remember it, pick another further afield.
	// The plop is the most urgent job of the game: every second without the first factory is lost production
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	if ((target != nullptr) || units.empty() || (frame < checkFrame)) {
		return;
	}
	checkFrame = frame + FRAMES_PER_SEC / 4;
	CCircuitUnit* unit = *units.begin();
	const AIFloat3& pos = unit->GetPos(frame);
	if (unit->IsMorphing() || (stallFrame < 0) || (pos.SqDistance2D(stallPos) > SQUARE(SQUARE_SIZE * 2))) {
		stallPos = pos;  // moving (walking there, or stepping out of its own footprint), or a morph's wait
		stallFrame = frame;
		return;
	}
	if (frame - stallFrame < FRAMES_PER_SEC * 3 / 2) {
		return;
	}
	auto commands = unit->GetUnit()->GetCurrentCommands();
	const int cmdId = commands.empty() ? 0 : commands.front()->GetId();
	utils::free_clear(commands);
	if (cmdId == ENGINE_CMD_RECLAIM) {  // clearing features in the footprint first
		stallFrame = frame;
		return;
	}
	circuit->LOG("PLOP %i:%02i stalled %is at (%.0f, %.0f): site (%.0f, %.0f) %.0f away, %s, order %i",
			frame / FRAMES_PER_SEC / 60, frame / FRAMES_PER_SEC % 60, (frame - stallFrame) / FRAMES_PER_SEC,
			pos.x, pos.z, buildPos.x, buildPos.z, utils::is_valid(buildPos) ? pos.distance2D(buildPos) : -1.f,
			(utils::is_valid(buildPos) && IsOccupied(buildPos, facing, unit)) ? "occupied" : "clear", cmdId);
	ReplaceSite(unit);
}

void CBFactoryTask::OnUnitIdle(CCircuitUnit* unit)
{
	// The order ended with no frame: the site failed (blocked, unreachable). Another site, not the same one twice more
	// and then no commander, which leaves the plop job unassigned and blocks any other first factory
	if (isPlop && (target == nullptr)) {
		ReplaceSite(unit);
	} else {
		IBuilderTask::OnUnitIdle(unit);
	}
}

void CBFactoryTask::ReplaceSite(CCircuitUnit* unit)
{
	if (++buildFails > TASK_RETRIES) {  // boxed in: an ordinary first factory job takes over (it still plops)
		manager->AbortTask(this);
		return;
	}
	stallFrame = -1;
	if (utils::is_valid(buildPos)) {
		if (badSites.size() >= 8) {
			badSites.erase(badSites.begin());
		}
		badSites.push_back(buildPos);
		SetBuildPos(-RgtVector);
	}
	Execute(unit);  // FindBuildSite: not a failed site, nobody standing in it
}

bool CBFactoryTask::IsOccupied(const AIFloat3& pos, int face, CCircuitUnit* builder) const
{
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	UnitDef* def = buildDef->GetDef();
	const float hx = (((face & 1) == 0) ? def->GetXSize() : def->GetZSize()) * SQUARE_SIZE / 2;
	const float hz = (((face & 1) == 0) ? def->GetZSize() : def->GetXSize()) * SQUARE_SIZE / 2;
	// Cheap per call (a site search makes hundreds): the list and each unit's position are refreshed once per frame
	circuit->UpdateFriendlyUnits();
	for (const auto& kv : circuit->GetFriendlyUnits()) {  // ours and allies'
		CAllyUnit* alu = kv.second;
		CCircuitDef* cdef = alu->GetCircuitDef();
		// Structures: blocking map and IsPossibleToBuildAt. Aircraft: not on the ground blocking map
		if (!cdef->IsMobile() || cdef->IsAbleToFly() || (alu->GetId() == builder->GetId())) {
			continue;
		}
		const AIFloat3& p = alu->GetPos(frame);
		const float r = cdef->GetRadius() + SQUARE_SIZE;
		if ((std::fabs(p.x - pos.x) < hx + r) && (std::fabs(p.z - pos.z) < hz + r)) {
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

	// Plop: not over another friendly unit (an allied commander in a crowded start box) that may never step aside, and
	// not near a site that failed before: 2 footprints from each after the first failure, up to 4, so the next site is
	// out of the crowd rather than the one beside the last, and a run of failures can't push it far from the start
	bool isAvoidUnits = isPlop;
	const float badSqDist = SQUARE(std::max(buildDef->GetDef()->GetXSize(), buildDef->GetDef()->GetZSize()) * SQUARE_SIZE
			* std::min<float>(badSites.size() + 1, 4));
	auto isBadSite = [this, badSqDist](const AIFloat3& p) {
		for (const AIFloat3& bad : badSites) {
			if (bad.SqDistance2D(p) < badSqDist) {
				return true;
			}
		}
		return false;
	};
	CTerrainManager::TerrainPredicate predicate = [this, terrainMgr, builder, &isAvoidUnits, &isBadSite](const AIFloat3& p) {
		return terrainMgr->CanReachAtSafe(builder, p, builder->GetCircuitDef()->GetBuildDistance())
				&& !(isAvoidUnits && (isBadSite(p) || IsOccupied(p, facing, builder)));
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
		isAvoidUnits = false;  // all taken or failed: a site to wait on beats Execute's fallback, a random base and no factory
		checkFacings();
	}
}

} // namespace circuit

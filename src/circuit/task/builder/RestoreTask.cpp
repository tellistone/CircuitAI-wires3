/*
 * RestoreTask.cpp
 *
 *  Restore ground the enemy terraformed (trenches, walls) to the map's original height.
 */

#include "task/builder/RestoreTask.h"
#include "task/TaskManager.h"
#include "module/BuilderManager.h"
#include "map/ThreatMap.h"
#include "terrain/path/QueryPathSingle.h"
#include "CircuitAI.h"
#include "util/Utils.h"

namespace circuit {

using namespace springai;

CBRestoreTask::CBRestoreTask(ITaskManager* mgr, Priority priority, int cell)
		: IBuilderTask(mgr, priority, nullptr, mgr->GetCircuit()->GetBuilderManager()->GetRestore().CellPos(cell, 0.5f),
					   Type::BUILDER, BuildType::TERRAFORM, 1.f, 0.f, 0)
		, cell(cell)
		, tag(-1)
		, isAssigned(false)
		, isDanger(false)
{
}

CBRestoreTask::~CBRestoreTask()
{
}

bool CBRestoreTask::CanAssignTo(CCircuitUnit* unit) const
{
	CCircuitAI* circuit = manager->GetCircuit();
	const CBuilderManager::SRestore& restore = circuit->GetBuilderManager()->GetRestore();
	if ((units.find(unit) == units.end()) && (units.size() >= (unsigned)restore.maxBuilders)) {
		return false;
	}
	if (unit->GetPos(circuit->GetLastFrame()).SqDistance2D(position) > SQUARE(restore.range)) {
		return false;
	}
	// The spot here; the way there is checked on the builder's path (ApplyPath)
	return circuit->GetThreatMap()->GetPlainThreatAt(unit, position) <= THREAT_MIN;
}

void CBRestoreTask::AssignTo(CCircuitUnit* unit)
{
	IBuilderTask::AssignTo(unit);
	isAssigned = true;
}

void CBRestoreTask::DropForDanger()
{
	isDanger = true;
	manager->AbortTask(this);
}

void CBRestoreTask::Stop(bool done)
{
	manager->GetCircuit()->GetBuilderManager()->OnRestoreEnd(this, done);
	IBuilderTask::Stop(done);
}

void CBRestoreTask::Execute(CCircuitUnit* unit)
{
	CCircuitAI* circuit = manager->GetCircuit();
	TRY_UNIT(circuit, unit,
		unit->CmdPriority(ClampPriority());
		if (tag < 0) {
			// The first builder makes the terraform: ZK's lasso layout (LuaUI/Widgets/gui_lasso_terraform.lua)
			CBuilderManager* builderMgr = circuit->GetBuilderManager();
			tag = builderMgr->NextRestoreTag();
			const AIFloat3 minPos = builderMgr->GetRestore().CellPos(cell, 0.f);
			const AIFloat3 maxPos = builderMgr->GetRestore().CellPos(cell, 1.f);
			unit->CmdTerraform({
				5.f,  // terraform type: restore
				static_cast<float>(circuit->GetTeamId()),
				position.x, position.z,  // command position
				static_cast<float>(tag),
				1.f,  // loop: an area, not a wall
				0.f,  // height: restore aims at the original height
				4.f,  // points
				1.f,  // constructors
				0.f,  // volume selection: raise and lower
				minPos.x, minPos.z, minPos.x, maxPos.z, maxPos.x, maxPos.z, maxPos.x, minPos.z,
				static_cast<float>(unit->GetId())});
		}
		// ZK's fallback repairs the nearest terraunit of the tag until none is left, then the order ends
		unit->GetUnit()->ExecuteCustomCommand(CMD_RESTORE_GROUND, {position.x, position.y, position.z, static_cast<float>(tag)});
	)
}

void CBRestoreTask::ApplyPath(const CQueryPathSingle* query)
{
	CThreatMap* threatMap = manager->GetCircuit()->GetThreatMap();
	CCircuitUnit* unit = query->GetUnit();
	const F3Vec& path = query->GetPathInfo()->posPath;
	for (size_t i = 1; i < path.size(); ++i) {  // not where the builder stands: it can't help that
		if (threatMap->GetPlainThreatAt(unit, path[i]) > THREAT_MIN) {
			// One builder's unsafe way drops the job for all on it: the cell's back-off stops a re-take every update
			DropForDanger();
			return;
		}
	}
	IBuilderTask::ApplyPath(query);
}

void CBRestoreTask::OnUnitIdle(CCircuitUnit* unit)
{
	// Idle at the cell after the order: no terraunit of the tag is left (or none was needed), for every builder on it
	const float range = unit->GetCircuitDef()->GetBuildDistance()
			+ CBuilderManager::SRestore::CELL * SQUARE_SIZE * 0.71f + 100.f;  // + half the cell's diagonal + terraunit leash
	if ((tag >= 0) && (unit->GetPos(manager->GetCircuit()->GetLastFrame()).SqDistance2D(position) < SQUARE(range))) {
		manager->DoneTask(this);
	} else if (++buildFails <= 2) {  // the engine sometimes drops orders
		Execute(unit);
	} else {
		manager->AbortTask(this);
	}
}

} // namespace circuit

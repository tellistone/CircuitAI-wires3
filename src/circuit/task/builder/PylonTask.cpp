/*
 * PylonTask.cpp
 *
 *  Created on: Jan 30, 2015
 *      Author: rlcevg
 */

#include "task/builder/PylonTask.h"
#include "task/TaskManager.h"
#include "map/ThreatMap.h"
#include "module/EconomyManager.h"
#include "resource/EnergyGrid.h"
#include "resource/GridLink.h"
#include "unit/CircuitUnit.h"
#include "CircuitAI.h"
#include "util/Utils.h"

#include "spring/SpringMap.h"

#include "AISCommands.h"

namespace circuit {

using namespace springai;

CBPylonTask::CBPylonTask(ITaskManager* mgr, Priority priority,
						 CCircuitDef* buildDef, const AIFloat3& position,
						 IGridLink* link, float cost, int timeout)
		: IBuilderTask(mgr, priority, buildDef, position, Type::BUILDER, BuildType::PYLON, cost, 0.f, timeout)
		, link(link)
{
	if (link != nullptr) {
		link->SetBeingBuilt(true);
	}
}

CBPylonTask::~CBPylonTask()
{
}

void CBPylonTask::Finish()
{
	if (link != nullptr) {
		link->SetBeingBuilt(false);
	}
	CEconomyManager* economyMgr = manager->GetCircuit()->GetEconomyManager();
	if (economyMgr->GetEnergyGrid() != nullptr) {
		economyMgr->GetEnergyGrid()->SetDirty();  // the next piece starts from this one
	}
	economyMgr->UpdatePylonTasks();

	IBuilderTask::Finish();
}

void CBPylonTask::Cancel()
{
	if (link != nullptr) {
		link->SetBeingBuilt(false);
	}

	IBuilderTask::Cancel();
}

void CBPylonTask::Execute(CCircuitUnit* unit)
{
	CCircuitAI* circuit = manager->GetCircuit();
	TRY_UNIT(circuit, unit,
		unit->CmdPriority(ClampPriority());
	)

	const int frame = circuit->GetLastFrame();
	if (target != nullptr) {
		TRY_UNIT(circuit, unit,
			unit->GetUnit()->Repair(target->GetUnit(), UNIT_CMD_OPTION, frame + FRAMES_PER_SEC * 60);
		)
		return;
	}
	UnitDef* buildUDef = buildDef->GetDef();
	if (utils::is_valid(buildPos)) {
		if (circuit->GetMap()->IsPossibleToBuildAt(buildUDef, buildPos, facing)) {
			TRY_UNIT(circuit, unit,
				unit->GetUnit()->Build(buildUDef, buildPos, facing, 0, frame + FRAMES_PER_SEC * 60);
			)
		} else {
			// Its site links; one found around it may not, and the next piece would go beside it. Planned again instead
			manager->FallbackTask(unit);
		}
		return;
	}

	circuit->GetThreatMap()->SetThreatType(unit);
	const float searchRadius = circuit->GetEconomyManager()->GetPylonRange() * 0.5f;
	FindBuildSite(unit, position, searchRadius);

	if (utils::is_valid(buildPos)) {
		TRY_UNIT(circuit, unit,
			unit->GetUnit()->Build(buildUDef, buildPos, facing, 0, frame + FRAMES_PER_SEC * 60);
		)
	} else {
		// Fallback to Guard/Assist/Patrol
		manager->FallbackTask(unit);
	}
}

} // namespace circuit

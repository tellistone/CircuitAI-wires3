/*
 * EnergyTask.cpp
 *
 *  Created on: Jan 30, 2015
 *      Author: rlcevg
 */

#include "task/builder/EnergyTask.h"
#include "task/TaskManager.h"
#include "module/EconomyManager.h"
#include "unit/CircuitUnit.h"
#include "spring/SpringMap.h"
#include "CircuitAI.h"
#include "util/Utils.h"

namespace circuit {

using namespace springai;

CBEnergyTask::CBEnergyTask(ITaskManager* mgr, Priority priority,
						   CCircuitDef* buildDef, const AIFloat3& position,
						   float cost, float shake, int timeout)
		: IBuilderTask(mgr, priority, buildDef, position, Type::BUILDER, BuildType::ENERGY, cost, shake, timeout)
		, isStalling(false)
{
}

CBEnergyTask::~CBEnergyTask()
{
}

void CBEnergyTask::Update()
{
	IBuilderTask::Update();
	if (units.empty() || (target == nullptr)) {
		return;
	}

	CCircuitAI* circuit = manager->GetCircuit();
	// Same test as CEconomyManager::UpdateEnergyTasks: high build priority while short of energy
	CEconomyManager* economyMgr = circuit->GetEconomyManager();
	const bool isEnergyStalling = economyMgr->IsEnergyStalling() || economyMgr->IsEnergyStall();
	if (isStalling == isEnergyStalling) {
		return;
	}
	isStalling = isEnergyStalling;
	priority = isEnergyStalling ? IBuilderTask::Priority::HIGH : IBuilderTask::Priority::NORMAL;
	TRY_UNIT(circuit, target,
		target->CmdPriority(ClampPriority());
	)
}

void CBEnergyTask::Execute(CCircuitUnit* unit)
{
	CCircuitAI* circuit = manager->GetCircuit();
	if ((target != nullptr) || !circuit->GetEconomyManager()->IsGeoDef(buildDef)) {
		IBuilderTask::Execute(unit);
		return;
	}

	// Geothermal: must stay exactly on a vent, never search around the position
	if (!utils::is_valid(buildPos) || !circuit->GetMap()->IsPossibleToBuildAt(buildDef->GetDef(), buildPos, facing)) {
		SetBuildPos(-RgtVector);
		const AIFloat3 pos = circuit->GetEconomyManager()->FindGeoSpot(buildDef, position, unit);
		if (!utils::is_valid(pos)) {
			// Fallback to Guard/Assist/Patrol
			manager->FallbackTask(unit);
			return;
		}
		SetBuildPos(pos);
	}
	IBuilderTask::Execute(unit);  // buildPos is a free vent: builds there
}

} // namespace circuit

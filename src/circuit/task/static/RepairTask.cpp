/*
 * RepairTask.cpp
 *
 *  Created on: Mar 30, 2015
 *      Author: rlcevg
 */

#include "task/static/RepairTask.h"
#include "task/TaskManager.h"
#include "task/IdleTask.h"
#include "module/BuilderManager.h"
#include "module/EconomyManager.h"
#include "module/FactoryManager.h"
#include "unit/action/DGunAction.h"
#include "CircuitAI.h"
#include "util/Utils.h"

#include "spring/SpringCallback.h"

#include "AISCommands.h"
#include "Feature.h"

namespace circuit {

using namespace springai;

// Whether a static builder can repair the target from where it stands: its build distance to the target's edge,
// as the engine measures it. The task's position is the target's spot when the job began, not the builder's
static bool IsInReach(CCircuitUnit* unit, CAllyUnit* target, int frame)
{
	const float reach = unit->GetCircuitDef()->GetBuildDistance() + target->GetCircuitDef()->GetRadius();
	return unit->GetPos(frame).SqDistance2D(target->GetPos(frame)) <= SQUARE(reach);
}

CSRepairTask::CSRepairTask(ITaskManager* mgr, Priority priority, CAllyUnit* target, int timeout)
		: IRepairTask(mgr, priority, Type::FACTORY, target, timeout)
{
}

CSRepairTask::~CSRepairTask()
{
}

void CSRepairTask::AssignTo(CCircuitUnit* unit)
{
	IUnitTask::AssignTo(unit);

	CCircuitAI* circuit = manager->GetCircuit();
	ShowAssignee(unit);
	if (!utils::is_valid(position)) {
		position = unit->GetPos(circuit->GetLastFrame());
	}

	if (unit->HasDGun()) {
		unit->PushDGunAct(new CDGunAction(unit, unit->GetDGunRange()));
	}
}

void CSRepairTask::Start(CCircuitUnit* unit)
{
	IRepairTask::Start(unit);
	if (targetId != -1) {
		Execute(unit);
	}
}

void CSRepairTask::Update()
{
	CCircuitAI* circuit = manager->GetCircuit();
	CEconomyManager* economyMgr = circuit->GetEconomyManager();
	if (economyMgr->GetAvgMetalIncome() < savedIncome * 0.6f) {
		manager->AbortTask(this);
	} else if ((++updCount % 4 == 0) && !units.empty()) {
		const int frame = circuit->GetLastFrame();
		CAllyUnit* repTarget = circuit->GetFriendlyUnit(targetId);
		if (repTarget == nullptr) {
			manager->AbortTask(this);
			return;
		}
		// Measured from each nano, not from where the target stood when the job began: a target that left a nano's
		// reach frees that nano. Jobs are taken at 0.9 of the range, so a target on the edge doesn't flip it
		decltype(units) outUnits;
		for (CCircuitUnit* unit : units) {
			if (!IsInReach(unit, repTarget, frame)) {
				outUnits.insert(unit);
			}
		}
		if (outUnits.size() == units.size()) {
			manager->AbortTask(this);
			return;
		}
		for (CCircuitUnit* unit : outUnits) {
			manager->AssignTask(unit, manager->GetIdleTask());
		}
		CCircuitUnit* nano = *units.begin();
		const AIFloat3& nanoPos = nano->GetPos(frame);
		const float radius = nano->GetCircuitDef()->GetBuildDistance();

		IBuilderTask* task = nullptr;
		CAllyUnit* newTarget = nullptr;  // of a new repair job, nullptr for reclaim
		if (repTarget->GetUnit()->IsBeingBuilt()) {
			CFactoryManager* factoryMgr = circuit->GetFactoryManager();
			if (economyMgr->IsMetalEmpty() && !factoryMgr->IsHighPriority(repTarget)) {
				// Check for damaged units
				CBuilderManager* builderMgr = circuit->GetBuilderManager();
				circuit->UpdateFriendlyUnits();
				auto us = circuit->GetCallback()->GetFriendlyUnitsIn(nanoPos, radius * 0.9f);
				for (Unit* u : us) {
					CAllyUnit* candUnit = circuit->GetFriendlyUnit(u);
					if ((candUnit == nullptr) || builderMgr->IsReclaimed(candUnit)) {
						continue;
					}
					if (!u->IsBeingBuilt() && (u->GetHealth() < u->GetMaxHealth())) {
						task = factoryMgr->EnqueueRepair(IBuilderTask::Priority::NORMAL, candUnit);
						newTarget = candUnit;
						break;
					}
				}
				utils::free_clear(us);
				if (task == nullptr) {
					// Reclaim task
					if (circuit->GetCallback()->IsFeaturesIn(nanoPos, radius)) {
						task = factoryMgr->EnqueueReclaim(IBuilderTask::Priority::NORMAL, nanoPos, radius);
					}
				}
			}
		} else if (economyMgr->IsMetalFull()) {
			// Check for units under construction
			CFactoryManager* factoryMgr = circuit->GetFactoryManager();
			CBuilderManager* builderMgr = circuit->GetBuilderManager();
			const float maxCost = MAX_BUILD_SEC * economyMgr->GetAvgMetalIncome() * economyMgr->GetEcoFactor();
			circuit->UpdateFriendlyUnits();
			auto us = circuit->GetCallback()->GetFriendlyUnitsIn(nanoPos, radius * 0.9f);
			for (Unit* u : us) {
				CAllyUnit* candUnit = circuit->GetFriendlyUnit(u);
				if ((candUnit == nullptr) || builderMgr->IsReclaimed(candUnit)) {
					continue;
				}
				bool isHighPrio = factoryMgr->IsHighPriority(candUnit);
				if (u->IsBeingBuilt() && ((candUnit->GetCircuitDef()->GetBuildTime() < maxCost) || isHighPrio)) {
					IBuilderTask::Priority priority = isHighPrio ? IBuilderTask::Priority::HIGH : IBuilderTask::Priority::NORMAL;
					task = factoryMgr->EnqueueRepair(priority, candUnit);
					newTarget = candUnit;
					break;
				}
			}
			utils::free_clear(us);
		}

		if (task != nullptr) {
			decltype(units) tmpUnits = units;
			for (CCircuitUnit* unit : tmpUnits) {
				// Only the nanos that reach the new job (the scan was round the first one); the rest look again
				const bool isReach = (newTarget == nullptr) || IsInReach(unit, newTarget, frame);
				manager->AssignTask(unit, isReach ? static_cast<IUnitTask*>(task) : manager->GetIdleTask());
			}
			manager->AbortTask(this);
		}
	}
}

void CSRepairTask::Finish()
{
	CCircuitAI* circuit = manager->GetCircuit();
	for (CCircuitUnit* unit : units) {
		TRY_UNIT(circuit, unit,
			unit->CmdPriority(0);
			unit->GetUnit()->PatrolTo(unit->GetPos(circuit->GetLastFrame()), UNIT_COMMAND_OPTION_SHIFT_KEY);  // not the target's spot
		)
	}

	IRepairTask::Finish();
}

void CSRepairTask::OnUnitIdle(CCircuitUnit* unit)
{
	// nano.hand_off: a nano handed this job the frame its last one ended can still get that job's late UnitIdle.
	// The target is still a frame: give the order again instead of ending the job
	CCircuitAI* circuit = manager->GetCircuit();
	if (circuit->GetLastFrame() < unit->GetTaskFrame() + 20) {
		CAllyUnit* repTarget = circuit->GetFriendlyUnit(targetId);
		if ((repTarget != nullptr) && repTarget->GetUnit()->IsBeingBuilt()) {
			Execute(unit);
			return;
		}
	}
	manager->DoneTask(this);
}

void CSRepairTask::OnUnitDamaged(CCircuitUnit* unit, CEnemyInfo* attacker)
{
	// TODO: Terraform attacker into dust
}

} // namespace circuit

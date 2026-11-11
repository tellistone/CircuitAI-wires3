/*
 * RepairTask.cpp
 *
 *  Created on: Jan 29, 2015
 *      Author: rlcevg
 */

#include "task/builder/RepairTask.h"
#include "task/RetreatTask.h"
#include "task/TaskManager.h"
#include "module/BuilderManager.h"
#include "module/EconomyManager.h"
#include "unit/action/TravelAction.h"
#include "CircuitAI.h"
#include "util/Utils.h"

namespace circuit {

using namespace springai;

CBRepairTask::CBRepairTask(ITaskManager* mgr, Priority priority, CAllyUnit* target, int timeout)
		: IRepairTask(mgr, priority, Type::BUILDER, target, timeout)
{
}

CBRepairTask::~CBRepairTask()
{
}

void CBRepairTask::Update()
{
	if (units.empty()) {
		// Unit healed on its own while nobody was assigned: drop the job
		CCircuitAI* circuit = manager->GetCircuit();
		CAllyUnit* repTarget = (target != nullptr) ? target : circuit->GetFriendlyUnit(targetId);
		if ((repTarget == nullptr) || (repTarget->GetUnit()->GetHealth() >= repTarget->GetUnit()->GetMaxHealth())) {
			manager->DoneTask(this);
			return;
		}
	}
	if (IsLeashBroken()) {
		return;
	}
	IRepairTask::Update();
}

bool CBRepairTask::IsLeashBroken()
{
	// Builder repair of a mobile unit is NOW (heavy/prio_repair) or HIGH (left from a retreat)
	if (units.empty() || (target == nullptr) || !target->GetCircuitDef()->IsMobile()
		|| ((priority != Priority::NOW) && (priority != Priority::HIGH)))
	{
		chases.clear();
		return false;
	}
	// A retreating unit is heading home to be repaired: keep following it
	if (target->GetTask()->GetType() == IUnitTask::Type::RETREAT) {
		chases.clear();  // the chase check starts afresh once it stops retreating
		return false;
	}
	CCircuitAI* circuit = manager->GetCircuit();
	CBuilderManager* builderMgr = circuit->GetBuilderManager();
	const float leash = builderMgr->GetPrioRepairLeash();
	const int frame = circuit->GetLastFrame();
	const AIFloat3& targetPos = target->GetPos(frame);
	bool isAnyInRange = false;
	bool isAllStalled = true;  // no assignee closes in on the target
	std::vector<CCircuitUnit*> unsafe;
	for (CCircuitUnit* unit : units) {
		// Don't follow the target into a fight the builder can't survive (retreat.repair_safe, same rule as job selection)
		if (builderMgr->IsUnsafePrioRepair(this, unit, targetPos)) {
			unsafe.push_back(unit);
			continue;
		}
		const float buildDist = unit->GetCircuitDef()->GetBuildDistance();
		const float sqDist = targetPos.SqDistance2D(unit->GetPos(frame));
		const float range = buildDist + SQUARE_SIZE * 8;
		if (sqDist < SQUARE(range)) {
			inRange.insert(unit);  // repair has started
			isAnyInRange = true;
			chases[unit].Reset();
			continue;
		}
		if ((leash > 0.f) && (inRange.find(unit) != inRange.end()) && (sqDist > SQUARE(buildDist + leash))) {
			// Target drove off half-repaired: stop chasing it
			builderMgr->OnPrioRepairLeash(targetId);
			manager->AbortTask(this);
			return true;
		}
		if (!builderMgr->IsChaseStalled(chases[unit], targetId, sqrtf(sqDist) - range, frame)) {
			isAllStalled = false;
		}
	}
	if (!unsafe.empty()) {
		// Job selection skips the job while it stays unsafe; it is picked up again once it isn't
		for (CCircuitUnit* unit : unsafe) {
			RemoveAssignee(unit);
		}
		if (units.empty()) {
			return true;
		}
	}
	// Nobody in reach and nobody closing in (target as fast, or moving at its group's pace, or stop-and-go): drop it.
	// Same cooldown as the leash, then a new job is taken if the unit still needs one.
	if (!isAnyInRange && isAllStalled) {
		builderMgr->OnPrioRepairLeash(targetId);
		manager->AbortTask(this);
		return true;
	}
	return false;
}

void CBRepairTask::RemoveAssignee(CCircuitUnit* unit)
{
	inRange.erase(unit);
	chases.erase(unit);
	IRepairTask::RemoveAssignee(unit);
}

void CBRepairTask::Start(CCircuitUnit* unit)
{
	IRepairTask::Start(unit);
	if (targetId != -1) {
		Update(unit);
	}
}

void CBRepairTask::OnUnitIdle(CCircuitUnit* unit)
{
	// An ally's unit (escalation assist) has no target, only its id
	CAllyUnit* repTarget = (target != nullptr) ? target : manager->GetCircuit()->GetFriendlyUnit(targetId);
	if (repTarget == nullptr) {
		manager->DoneTask(this);  // gone or finished out of the list
		return;
	}
	Unit* u = repTarget->GetUnit();
	if (u->GetHealth() < u->GetMaxHealth()) {
		// unit stuck or event order fail
		RemoveAssignee(unit);
	} else {
		// task finished
		manager->DoneTask(this);
	}
}

void CBRepairTask::OnUnitDamaged(CCircuitUnit* unit, CEnemyInfo* attacker)
{
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	CCircuitDef* cdef = unit->GetCircuitDef();
	const float healthPerc = unit->GetHealthPercent();
	// Builder hit on a priority repair: the spot is under fire, call the job off (retreat.repair_hit_exit)
	CBuilderManager* builderMgr = circuit->GetBuilderManager();
	if ((priority == Priority::NOW) && builderMgr->IsRepairHitBuilder()) {
		builderMgr->OnPrioRepairLeash(targetId, builderMgr->GetRepairHitCooldown());
		manager->AbortTask(this);  // the builder goes idle, or retreats below
	}
	if ((healthPerc > cdef->GetRetreat()) && !unit->IsDisarmed(frame)) {
		if ((cdef->IsRoleHeavy() || cdef->IsAttrPrioRepair()) && (healthPerc < 0.9f)) {
			circuit->GetBuilderManager()->EnqueueRepair(IBuilderTask::Priority::NOW, unit);
		}
		return;
	}

	CRetreatTask* task = manager->GetCircuit()->GetBuilderManager()->EnqueueRetreat();
	manager->AssignTask(unit, task);
}

bool CBRepairTask::Reevaluate(CCircuitUnit* unit)
{
	CCircuitAI* circuit = manager->GetCircuit();
	// FIXME: Replace const 1000.0f with build time?
	if ((cost > 1000.0f) && (circuit->GetEconomyManager()->GetAvgMetalIncome() < savedIncome * 0.6f)) {
		manager->AbortTask(this);
		return false;
	}

	CAllyUnit* repTarget = (target != nullptr) ? target : circuit->GetFriendlyUnit(targetId);
	if ((repTarget != nullptr) && (repTarget->GetUnit()->GetHealth() < repTarget->GetUnit()->GetMaxHealth())) {
		buildPos = repTarget->GetPos(circuit->GetLastFrame());
	} else {
		manager->AbortTask(this);
		return false;
	}
	return true;
}

} // namespace circuit

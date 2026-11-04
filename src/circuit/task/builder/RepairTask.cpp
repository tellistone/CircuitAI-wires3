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
		, fleeFrame(-1)
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
		fleeFrame = -1;
		return false;
	}
	// A retreating unit is heading home to be repaired: keep following it
	if (target->GetTask()->GetType() == IUnitTask::Type::RETREAT) {
		fleeFrame = -1;
		return false;
	}
	CCircuitAI* circuit = manager->GetCircuit();
	CBuilderManager* builderMgr = circuit->GetBuilderManager();
	const float leash = builderMgr->GetPrioRepairLeash();
	const int frame = circuit->GetLastFrame();
	const AIFloat3& targetPos = target->GetPos(frame);
	bool isFleeing = true;  // from every assignee
	for (CCircuitUnit* unit : units) {
		const float buildDist = unit->GetCircuitDef()->GetBuildDistance();
		const AIFloat3& pos = unit->GetPos(frame);
		const float sqDist = targetPos.SqDistance2D(pos);
		if (sqDist < SQUARE(buildDist + SQUARE_SIZE * 8)) {
			inRange.insert(unit);  // repair has started
		} else if ((leash > 0.f) && (inRange.find(unit) != inRange.end()) && (sqDist > SQUARE(buildDist + leash))) {
			// Target drove off half-repaired: stop chasing it
			builderMgr->OnPrioRepairLeash(targetId);
			manager->AbortTask(this);
			return true;
		}
		if (isFleeing && !builderMgr->IsRepairTargetFleeing(this, pos, buildDist)) {
			isFleeing = false;
		}
	}
	// Target keeps driving away from all its repairers (as fast or faster than them): don't chase it.
	// Job selection skips it while it moves away, a new job is taken once it stops or turns back.
	if (!isFleeing) {
		fleeFrame = -1;
	} else if (fleeFrame < 0) {
		fleeFrame = frame;
	} else if (frame - fleeFrame >= builderMgr->GetRepairFleeFrames()) {
		manager->AbortTask(this);
		return true;
	}
	return false;
}

void CBRepairTask::RemoveAssignee(CCircuitUnit* unit)
{
	inRange.erase(unit);
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
	Unit* u = target->GetUnit();
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

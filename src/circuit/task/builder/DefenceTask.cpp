/*
 * DefenceTask.cpp
 *
 *  Created on: Jan 30, 2015
 *      Author: rlcevg
 */

#include "task/builder/DefenceTask.h"
#include "task/TaskManager.h"
#include "map/InfluenceMap.h"
#include "module/AATurretPicker.h"
#include "module/BuilderManager.h"
#include "module/MilitaryManager.h"
#include "module/CreepPlanner.h"
#include "unit/CircuitDef.h"
#include "CircuitAI.h"
#include "util/Utils.h"

#include "module/TurretPicker.h"
#include "spring/SpringCallback.h"
#include "spring/SpringMap.h"
#include "util/Defines.h"

#include "Feature.h"
#include "Log.h"
#include "UnitDef.h"

namespace circuit {

using namespace springai;

CBDefenceTask::CBDefenceTask(ITaskManager* mgr, Priority priority,
							 CCircuitDef* buildDef, const AIFloat3& position,
							 float cost, float shake, int timeout)
		: IBuilderTask(mgr, priority, buildDef, position, Type::BUILDER, BuildType::DEFENCE, cost, shake, timeout)
		, isUrgent(false)
		, isOffPoints(false)
		, isPicker(false)
		, isAAPicker(false)
		, isAlly(true)
		, creepLane(-1)
		, normalCost(cost)
		, charged(0.f)
		, spend(0)
{
}

CBDefenceTask::~CBDefenceTask()
{
}

void CBDefenceTask::Update()
{
	// A picker job whose builder got there: dropped if threat has reached the spot or it no longer pays (the enemy
	// building it was for died, say), rather than slid to other ground by the site search. The spot is masked, so the
	// next order looks elsewhere instead of walking back to it
	CMilitaryManager* militaryMgr = manager->GetCircuit()->GetMilitaryManager();
	if (isPicker && (target == nullptr) && !traveled.empty()
		&& !militaryMgr->GetTurretPicker()->IsStillWorth(this, *traveled.begin()))
	{
		militaryMgr->GetSpotScorer()->MaskFailed(buildPos);
		if (IsCreep()) {
			militaryMgr->GetCreepPlanner()->AddFail(creepLane);  // a failed step, not a success
		}
		manager->AbortTask(this);
		return;
	}
	// An aa_turret job whose builder got there: dropped if ground threat has reached the spot (its spot masked)
	if (isAAPicker && (target == nullptr) && !traveled.empty() && (militaryMgr->GetAATurretPicker() != nullptr)
		&& militaryMgr->GetAATurretPicker()->IsThreatened(buildPos))
	{
		manager->GetCircuit()->LOG("AA TURRET dropped on arrival: %s at (%.0f, %.0f), ground threat reached it",
				buildDef->GetDef()->GetName(), buildPos.x, buildPos.z);
		militaryMgr->GetSpotScorer()->MaskFailed(buildPos);
		manager->AbortTask(this);
		return;
	}
	IBuilderTask::Update();
	if (units.empty() || (target == nullptr)) {
		return;
	}

	CCircuitAI* circuit = manager->GetCircuit();
	bool isUnderEnemy = (circuit->GetInflMap()->GetEnemyInflAt(buildPos) > INFL_EPS);
	if (isUrgent == isUnderEnemy) {
		return;
	}
	isUrgent = isUnderEnemy;
	if (isUnderEnemy) {
		priority = IBuilderTask::Priority::HIGH;
		cost *= 8.f;  // attract more builders
	} else {
		priority = IBuilderTask::Priority::NORMAL;
		cost = normalCost;
	}
	TRY_UNIT(circuit, target,
		target->CmdPriority(ClampPriority());
	)
}

void CBDefenceTask::Finish()
{
	// NOTE: No "reclaim turret blockers" job here. It was queued with cost 0, which CBReclaimTask::CanAssignTo
	//       never accepts (0 > build power x helper_sec is false), so no constructor could take it. It only
	//       counted as an untaken reclaim job for 60 s and blocked wreck jobs (CEconomyManager::UpdateReclaimTasks).
	IBuilderTask::Finish();
}

void CBDefenceTask::Cancel()
{
	CMilitaryManager* militaryMgr = manager->GetCircuit()->GetMilitaryManager();
	if (!isOffPoints) {
		militaryMgr->AbortDefence(this);
	}
	if (IsCreep() && (target == nullptr) && (militaryMgr != nullptr)) {
		militaryMgr->GetCreepPlanner()->OnUntaken(creepLane);  // timed out or dropped before it started
	}

	IBuilderTask::Cancel();
}

void CBDefenceTask::SetTarget(CCircuitUnit* unit)
{
	IBuilderTask::SetTarget(unit);
	if (unit == nullptr) {
		return;
	}
	CMilitaryManager* militaryMgr = manager->GetCircuit()->GetMilitaryManager();
	if (isOffPoints) {
		militaryMgr->AddOffPointsTower(unit->GetId());
	}
	if (IsCreep() && (priority == Priority::HIGH)) {
		// HIGH ranks the job for builders looking for work; once begun ZK builds it at its normal priority, unless
		// enemies near raise it (Update), so a turret at the line doesn't take metal before the factory
		ChangePriority(Priority::NORMAL);
	}
	if (charged > 0.f) {
		militaryMgr->AddChargedTower(unit->GetId(), charged);  // porcupine.budget.loss_refund
	}
}

void CBDefenceTask::OnUnitIdle(CCircuitUnit* unit)
{
	// The engine refused the build (IBuilderTask::OnUnitIdle aborts after TASK_RETRIES): a picker's spot is skipped
	// for 5 minutes, so the next order looks elsewhere
	if ((isPicker || isAAPicker) && !IsDead() && (target == nullptr) && (buildFails + 1 > TASK_RETRIES)
		&& utils::is_valid(buildPos))
	{
		manager->GetCircuit()->GetMilitaryManager()->GetSpotScorer()->MaskFailed(buildPos);
	}
	IBuilderTask::OnUnitIdle(unit);
}

void CBDefenceTask::Execute(CCircuitUnit* unit)
{
	// A picker or aa_turret job is built on its exact spot or not at all: the site search would move it up to 1600
	// elmos, onto ground nobody scored
	if (IsDead()) {
		return;
	}
	if ((isPicker || isAAPicker) && (target == nullptr)
		&& (!utils::is_valid(buildPos)
			|| !manager->GetCircuit()->GetMap()->IsPossibleToBuildAt(buildDef->GetDef(), buildPos, facing)))
	{
		if (utils::is_valid(buildPos)) {
			// The engine refuses the spot now (a wreck or a building on it): the next order looks elsewhere
			manager->GetCircuit()->GetMilitaryManager()->GetSpotScorer()->MaskFailed(buildPos);
		}
		manager->AbortTask(this);
		return;
	}
	IBuilderTask::Execute(unit);
}

} // namespace circuit

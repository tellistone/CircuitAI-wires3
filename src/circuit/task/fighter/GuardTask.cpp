/*
 * GuardTask.cpp
 *
 *  Created on: Jan 28, 2015
 *      Author: rlcevg
 */

#include "task/fighter/GuardTask.h"
#include "map/InfluenceMap.h"
#include "module/MilitaryManager.h"
#include "unit/CircuitUnit.h"
#include "CircuitAI.h"
#include "util/Utils.h"

namespace circuit {

using namespace springai;

CFGuardTask::CFGuardTask(ITaskManager* mgr, CCircuitUnit* vip, float maxPower)
		: IFighterTask(mgr, FightType::GUARD, 1.f)
		, vipId(vip->GetId())
		, maxPower(maxPower)
		, isVipRetreat(false)
		, isVipSafe(false)
{
}

CFGuardTask::~CFGuardTask()
{
}

bool CFGuardTask::CanAssignTo(CCircuitUnit* unit) const
{
	// One guard per slot, only while the builder needs one, only configured roles
	if (!units.empty() || !IsActive()) {
		return false;
	}
	return static_cast<CMilitaryManager*>(manager)->IsGuardRole(unit->GetCircuitDef());
}

void CFGuardTask::Start(CCircuitUnit* unit)
{
	CCircuitAI* circuit = manager->GetCircuit();
	CCircuitUnit* vip = circuit->GetTeamUnit(vipId);
	if (vip != nullptr) {
		TRY_UNIT(circuit, unit,
			unit->GetUnit()->Guard(vip->GetUnit());
			unit->CmdWantedSpeed(NO_SPEED_LIMIT);
		)
	} else {
		manager->AbortTask(this);
	}
}

void CFGuardTask::Update()
{
	IFighterTask::Update();

	CCircuitUnit* vip = manager->GetCircuit()->GetTeamUnit(vipId);
	if (vip == nullptr) {
		return;  // builder's destroyed handler removes the slot
	}
	CheckVip(vip);
	if (!IsActive() && !units.empty()) {
		Release();
	}
}

void CFGuardTask::OnUnitIdle(CCircuitUnit* unit)
{
	CCircuitAI* circuit = manager->GetCircuit();
	CCircuitUnit* vip = circuit->GetTeamUnit(vipId);
	if (vip != nullptr) {
		TRY_UNIT(circuit, unit,
			unit->GetUnit()->Guard(vip->GetUnit());
		)
	} else {
		manager->AbortTask(this);
	}
}

void CFGuardTask::CheckVip(CCircuitUnit* vip)
{
	CCircuitAI* circuit = manager->GetCircuit();
	const CMilitaryManager::SGuardCfg& cfg = static_cast<CMilitaryManager*>(manager)->GetGuardCfg();

	// Exit 1: the builder is retreating
	isVipRetreat = (vip->GetTask() != nullptr) && (vip->GetTask()->GetType() == IUnitTask::Type::RETREAT);

	// Exit 2: the builder stands inside the allied threat bubble.
	// The guard's own power is subtracted, so the guard can't make the spot look safe by itself.
	if (cfg.safeAlly <= 0.f) {
		isVipSafe = false;
		return;
	}
	CInfluenceMap* inflMap = circuit->GetInflMap();
	const AIFloat3& pos = vip->GetPos(circuit->GetLastFrame());
	float allyInfl = inflMap->GetAllyInflAt(pos);
	for (CCircuitUnit* unit : units) {
		allyInfl -= unit->GetCircuitDef()->GetPower();
	}
	isVipSafe = (allyInfl >= cfg.safeAlly) && (inflMap->GetEnemyInflAt(pos) <= cfg.safeEnemy);
}

void CFGuardTask::Release()
{
	CMilitaryManager* militaryMgr = static_cast<CMilitaryManager*>(manager);
	decltype(units) tmpUnits = units;
	for (CCircuitUnit* unit : tmpUnits) {
		// IsActive() is false here, so MakeTask won't hand this slot back
		IUnitTask* task = militaryMgr->MakeTask(unit);
		if (task != nullptr) {
			manager->AssignTask(unit, task);
		} else {
			RemoveAssignee(unit);  // falls back to idle task, re-tasked on its next update
		}
	}
}

} // namespace circuit

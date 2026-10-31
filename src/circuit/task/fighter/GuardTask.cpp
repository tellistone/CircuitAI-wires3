/*
 * GuardTask.cpp
 *
 *  Created on: Jan 28, 2015
 *      Author: rlcevg
 */

#include "task/fighter/GuardTask.h"
#include "map/InfluenceMap.h"
#include "module/MilitaryManager.h"
#include "terrain/TerrainManager.h"
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
		, safeSince(-1)
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
	if (!IsActive()) {
		if (!units.empty()) {
			Release();
		}
	} else if (units.empty()) {
		Pull(vip);
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
		safeSince = -1;
		return;
	}
	CInfluenceMap* inflMap = circuit->GetInflMap();
	const AIFloat3& pos = vip->GetPos(circuit->GetLastFrame());
	float allyInfl = inflMap->GetAllyInflAt(pos);
	for (CCircuitUnit* unit : units) {
		allyInfl -= unit->GetCircuitDef()->GetPower();
	}
	const int frame = circuit->GetLastFrame();
	if ((allyInfl >= cfg.safeAlly) && (inflMap->GetEnemyInflAt(pos) <= cfg.safeEnemy)) {
		if (safeSince < 0) {
			safeSince = frame;
		}
		// Debounce: an army walking past shouldn't send the guard away
		isVipSafe = (frame - safeSince >= cfg.safeFrames);
	} else {
		safeSince = -1;
		isVipSafe = false;
	}
}

void CFGuardTask::Pull(CCircuitUnit* vip)
{
	CMilitaryManager* militaryMgr = static_cast<CMilitaryManager*>(manager);
	const CMilitaryManager::SGuardCfg& cfg = militaryMgr->GetGuardCfg();
	if (cfg.pullRange <= 0.f) {
		return;
	}
	CCircuitAI* circuit = manager->GetCircuit();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	const int frame = circuit->GetLastFrame();
	const AIFloat3& pos = vip->GetPos(frame);
	CCircuitUnit* best = nullptr;
	float minSqDist = SQUARE(cfg.pullRange);
	for (CCircuitUnit* unit : militaryMgr->GetArmy()) {
		// Only units the AI controls that are idle or on a raid/defend task. Never:
		//   retreating or player-controlled (not FIGHTER/IDLE), already guarding,
		//   scouting (keeps the scout quota), or in an attack squad (AttackTask disbands below its min power)
		IUnitTask* curTask = unit->GetTask();
		if ((curTask == nullptr)
			|| ((curTask->GetType() != IUnitTask::Type::FIGHTER) && (curTask->GetType() != IUnitTask::Type::IDLE))
			|| !militaryMgr->IsGuardRole(unit->GetCircuitDef()))
		{
			continue;
		}
		if (curTask->GetType() == IUnitTask::Type::FIGHTER) {
			const FightType ft = static_cast<IFighterTask*>(curTask)->GetFightType();
			if ((ft == FightType::GUARD) || (ft == FightType::SCOUT) || (ft == FightType::ATTACK)) {
				continue;
			}
		}
		const float sqDist = pos.SqDistance2D(unit->GetPos(frame));
		if ((sqDist < minSqDist) && terrainMgr->CanReachAt(unit, pos, unit->GetCircuitDef()->GetMaxRange())) {
			minSqDist = sqDist;
			best = unit;
		}
	}
	if (best != nullptr) {
		manager->AssignTask(best, this);
	}
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

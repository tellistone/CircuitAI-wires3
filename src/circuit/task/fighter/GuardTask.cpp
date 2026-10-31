/*
 * GuardTask.cpp
 *
 *  Created on: Jan 28, 2015
 *      Author: rlcevg
 */

#include "task/fighter/GuardTask.h"
#include "map/InfluenceMap.h"
#include "map/ThreatMap.h"
#include "module/MilitaryManager.h"
#include "task/builder/BuilderTask.h"
#include "terrain/TerrainManager.h"
#include "unit/CircuitUnit.h"
#include "CircuitAI.h"
#include "util/Utils.h"

#include "Log.h"

namespace circuit {

using namespace springai;

CFGuardTask::CFGuardTask(ITaskManager* mgr, CCircuitUnit* vip, float maxPower)
		: IFighterTask(mgr, FightType::GUARD, 1.f)
		, vipId(vip->GetId())
		, maxPower(maxPower)
		, isVipRetreat(false)
		, isVipSafe(false)
		, safeSince(-1)
		, cooldownUntil(-1)
		, lastAlly(0.f)
		, lastEnemy(0.f)
		, nextDebugFrame(0)
{
}

CFGuardTask::~CFGuardTask()
{
}

bool CFGuardTask::IsActive() const
{
	return !isVipRetreat && !isVipSafe && (manager->GetCircuit()->GetLastFrame() >= cooldownUntil);
}

bool CFGuardTask::CanAssignTo(CCircuitUnit* unit) const
{
	// One guard per slot, only while the builder needs one, only configured roles
	if (!units.empty() || !IsActive()) {
		return false;
	}
	CMilitaryManager* militaryMgr = static_cast<CMilitaryManager*>(manager);
	if (!militaryMgr->IsGuardRole(unit->GetCircuitDef()) || militaryMgr->IsGuardCapReached(unit->GetCircuitDef())) {
		return false;
	}
	// Don't send a unit (e.g. one just released by another slot's threat exit) to a builder under heavy threat
	CCircuitUnit* vip = manager->GetCircuit()->GetTeamUnit(vipId);
	return (vip == nullptr) || !IsOverThreat(unit, vip->GetPos(manager->GetCircuit()->GetLastFrame()));
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
	const int prevCooldown = cooldownUntil;
	CheckThreat();
	if (!IsActive()) {
		if (!units.empty()) {
			const char* reason = isVipRetreat ? "builder retreating"
					: isVipSafe ? "builder safe"
					: (cooldownUntil != prevCooldown) ? "guard in enemy threat"
					: "cooldown";
			DebugLog(vip, reason);
			Release();
		}
	} else if (units.empty()) {
		Pull(vip);
		if (!units.empty()) {
			DebugLog(vip, "pulled guard");
		}
	}

	const int frame = manager->GetCircuit()->GetLastFrame();
	if (frame >= nextDebugFrame) {
		nextDebugFrame = frame + FRAMES_PER_SEC * 10;
		DebugLog(vip, "status");
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
	lastAlly = allyInfl;
	lastEnemy = inflMap->GetEnemyInflAt(pos);
	bool isSafe = (allyInfl >= cfg.safeAlly) && (lastEnemy <= cfg.safeEnemy);

	// The builder's job destination must be safe too: a builder at base that just took a far mex
	// keeps its slot open, so a guard can join (pull reaches units gathering at base) before it leaves
	if (isSafe) {
		const AIFloat3 dest = GetVipDestination(vip);
		if (utils::is_valid(dest)) {
			float destAlly = inflMap->GetAllyInflAt(dest);
			for (CCircuitUnit* unit : units) {
				destAlly -= unit->GetCircuitDef()->GetPower();
			}
			isSafe = (destAlly >= cfg.safeAlly) && (inflMap->GetEnemyInflAt(dest) <= cfg.safeEnemy);
		}
	}

	if (isSafe) {
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

AIFloat3 CFGuardTask::GetVipDestination(CCircuitUnit* vip) const
{
	IUnitTask* task = vip->GetTask();
	if ((task == nullptr) || (task->GetType() != IUnitTask::Type::BUILDER)) {
		return -RgtVector;
	}
	IBuilderTask* bt = static_cast<IBuilderTask*>(task);
	if (bt->GetTarget() != nullptr) {  // repair, or assisting an existing structure
		return bt->GetTarget()->GetPos(manager->GetCircuit()->GetLastFrame());
	}
	return bt->GetPosition();  // -RgtVector when the job has no location
}

void CFGuardTask::CheckThreat()
{
	// Exit 3: a guard stands in enemy threat. Cancel the guard and keep this builder's slot closed for a while,
	// so the guard (or the next pulled unit) isn't fed into the same fight
	const CMilitaryManager::SGuardCfg& cfg = static_cast<CMilitaryManager*>(manager)->GetGuardCfg();
	if ((cfg.threatCooldown < 0) || units.empty()) {
		return;
	}
	const int frame = manager->GetCircuit()->GetLastFrame();
	for (CCircuitUnit* unit : units) {
		if (IsOverThreat(unit, unit->GetPos(frame))) {
			cooldownUntil = frame + cfg.threatCooldown;
			return;
		}
	}
}

bool CFGuardTask::IsOverThreat(CCircuitUnit* unit, const AIFloat3& pos) const
{
	// Light threat (below threat_ratio x the unit's own threat) doesn't count: the guard holds
	const CMilitaryManager::SGuardCfg& cfg = static_cast<CMilitaryManager*>(manager)->GetGuardCfg();
	if (cfg.threatCooldown < 0) {
		return false;
	}
	CThreatMap* threatMap = manager->GetCircuit()->GetThreatMap();
	const float maxThreat = std::max(THREAT_MIN, cfg.threatRatio * threatMap->GetUnitThreat(unit));
	return threatMap->GetThreatAt(unit, pos) > maxThreat;
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
		const AIFloat3& unitPos = unit->GetPos(frame);
		const float sqDist = pos.SqDistance2D(unitPos);
		if (sqDist >= minSqDist) {
			continue;
		}
		// Don't pull a unit the threat exit would cancel right away or on arrival (avoids a cooldown ping-pong)
		if (IsOverThreat(unit, pos) || IsOverThreat(unit, unitPos)) {
			continue;
		}
		if (!militaryMgr->IsGuardCapReached(unit->GetCircuitDef())  // O(army): checked last, on nearby candidates only
			&& terrainMgr->CanReachAt(unit, pos, unit->GetCircuitDef()->GetMaxRange()))
		{
			minSqDist = sqDist;
			best = unit;
		}
	}
	if (best != nullptr) {
		manager->AssignTask(best, this);
	}
}

void CFGuardTask::DebugLog(CCircuitUnit* vip, const char* event) const
{
	if (!static_cast<CMilitaryManager*>(manager)->GetGuardCfg().isDebug) {
		return;
	}
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	circuit->LOG("GUARD %s | builder %i %s | guard %s | ally %.2f enemy %.2f | safe %i (for %.1fs) retreat %i cooldown %.1fs",
			event, vip->GetId(), vip->GetCircuitDef()->GetDef()->GetName(),
			units.empty() ? "none" : (*units.begin())->GetCircuitDef()->GetDef()->GetName(),
			lastAlly, lastEnemy, isVipSafe, (safeSince < 0) ? 0.f : float(frame - safeSince) / FRAMES_PER_SEC,
			isVipRetreat, std::max(0.f, float(cooldownUntil - frame) / FRAMES_PER_SEC));
}

void CFGuardTask::Release()
{
	CCircuitAI* circuit = manager->GetCircuit();
	CMilitaryManager* militaryMgr = static_cast<CMilitaryManager*>(manager);
	decltype(units) tmpUnits = units;
	for (CCircuitUnit* unit : tmpUnits) {
		// Drop the engine Guard order: some new tasks (e.g. a fresh raid) issue no command in Start,
		// and the unit would keep guarding until that task's first update
		TRY_UNIT(circuit, unit,
			unit->GetUnit()->Stop();
		)
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

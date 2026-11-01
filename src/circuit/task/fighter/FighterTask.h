/*
 * FighterTask.h
 *
 *  Created on: Aug 31, 2015
 *      Author: rlcevg
 */

#ifndef SRC_CIRCUIT_TASK_FIGHTER_FIGHTERTASK_H_
#define SRC_CIRCUIT_TASK_FIGHTER_FIGHTERTASK_H_

#include "task/UnitTask.h"
#include "util/Defines.h"

#include <limits>
#include <map>

namespace circuit {

class CEnemyInfo;

class IFighterTask: public IUnitTask {
public:
	enum class FightType: char {RALLY = 0, GUARD, DEFEND, SCOUT, RAID, ATTACK, BOMB, MELEE, ARTY, AA, AH, SUPPORT, SUPER, _SIZE_};
	using FT = std::underlying_type<FightType>::type;

protected:
	IFighterTask(ITaskManager* mgr, FightType type, float powerMod, int timeout = ASSIGN_TIMEOUT);
public:
	virtual ~IFighterTask();

	virtual void AssignTo(CCircuitUnit* unit) override;
	virtual void RemoveAssignee(CCircuitUnit* unit) override;

	virtual void Update() override;

	virtual void OnUnitIdle(CCircuitUnit* unit) override;
	virtual void OnUnitDamaged(CCircuitUnit* unit, CEnemyInfo* attacker) override;
	virtual void OnUnitDestroyed(CCircuitUnit* unit, CEnemyInfo* attacker) override;

	FightType GetFightType() const { return fightType; }
	const springai::AIFloat3& GetPosition() const { return position; }

	float GetAttackPower() const { return attackPower; }
	CEnemyInfo* GetTarget() const { return target; }
	void ClearTarget() { target = nullptr; }  // Only for ~CEnemyUnit

	const std::set<CCircuitUnit*>& GetShields() const { return shields; }

protected:
	void SetTarget(CEnemyInfo* enemy);
	// The task's thr_mod multiplier from its distance-from-base curve (quota.thr_mod.distance.<task>), read at the spot
	// being judged (an enemy, a destination, a unit's own position): divide a threat allowance by it. 1 if not configured.
	float GetThrDistModAt(const springai::AIFloat3& pos) const;
	// isDirect: Attack the target first instead of fight-moving to it.
	// The engine still lets weapons shoot other units while the ordered target can't be hit (out of range),
	// but switches to the ordered target as soon as it can.
	void Attack(const int frame, const bool isDirect = false);
	// Firing spot for a ranged unit: within its weapon range of the target, trading walking distance against
	// threat and against spots already claimed by other units this round; false = no usable range
	// NONE: no usable range (old orders); HOLD: every spot dangerous but the unit's own spot is safe;
	// RISKY: every spot dangerous, the unit's too: least dangerous spot by the usual score
	enum class FireSpot: char {NONE, SPOT, HOLD, RISKY};
	FireSpot FindFireSpot(CCircuitUnit* unit, const F3Vec& claimed, float safeThreat, springai::AIFloat3& outPos) const;
	// Group strength the task's escape compares threat against; <= 0: no escape, firing spots aren't limited
	virtual float GetEscapeSafeThreat() const { return -1.f; }
	// Units sent to a RISKY firing spot, with the frame: pull-back leaves them alone for 3 s
	// (pointers only compared, never dereferenced)
	std::map<CCircuitUnit*, int> riskySpotFrames;
	// Threat allowed at a spot for this strength (tasks with an escape override it)
	virtual float GetAllowedThreatAt(const springai::AIFloat3& pos, float strength) const {
		return std::numeric_limits<float>::max();
	}

	FightType fightType;
	springai::AIFloat3 position;  // attack/scout position

	float attackPower;
	float powerMod;
	int attackFrame;
	// NOTE: Never assign directly, use SetTarget() to avoid access to a dead target
	CEnemyInfo* target;

	std::set<CCircuitUnit*> cowards;
	std::set<CCircuitUnit*> shields;

	static F3Vec urgentPositions;  // NOTE: micro-opt
	static F3Vec enemyPositions;  // NOTE: micro-opt

#ifdef DEBUG_VIS
public:
	virtual void Log() override;
#endif
};

} // namespace circuit

#endif // SRC_CIRCUIT_TASK_FIGHTER_FIGHTERTASK_H_

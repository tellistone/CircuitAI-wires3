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
	// Ranged units move to a firing spot (FindFireSpot), then Attack the target. isDirect: units without a spot
	// (melee, no usable range) get a direct Attack instead of a fight-move, so the target keeps priority.
	// The engine still lets weapons shoot other units while the ordered target can't be hit.
	void Attack(const int frame, const bool isDirect = false);
	// Firing spot for a ranged unit: within its weapon range of the target, trading walking distance against
	// threat and against spots other units are heading to.
	// NONE: no usable range (old orders); SPOT: a safe spot; HOLD: every spot dangerous but the unit's own
	// spot is safe; RISKY: nowhere safe, the unit's spot neither: the least dangerous spot
	enum class FireSpot: char {NONE, SPOT, HOLD, RISKY};
	float GetFireRange(CCircuitUnit* unit) const;  // max range against the current target's kind
	bool IsInFireBand(CCircuitUnit* unit, const springai::AIFloat3& pos) const;  // 0.6-0.95x range from the target
	bool GetKeptFireSpot(CCircuitUnit* unit, springai::AIFloat3& outPos) const;  // last spot, if still usable
	bool IsOnRiskySpot(CCircuitUnit* unit, int frame) const;  // sent to a RISKY spot within 3 s: no pull-back
	FireSpot FindFireSpot(CCircuitUnit* unit, const std::map<CCircuitUnit*, springai::AIFloat3>& claims,
			float safeThreat, springai::AIFloat3& outPos) const;
	// Group strength the task's escape compares threat against; <= 0: no escape, firing spots aren't limited
	virtual float GetEscapeSafeThreat() const { return -1.f; }
	int attackRepeat = FRAMES_PER_SEC * 3;  // re-send attack orders this often (frames)
	bool isFireSpotChase = true;  // at a firing spot: true = Attack order (engine may chase), false = fight from the spot
	float fireSpotReach = -1.f;  // > 0: units farther than this x their fire range from the target get no attack order and keep travelling
	// Firing spot each unit was last sent to (pointers only compared, never dereferenced)
	struct SFireSpot {
		springai::AIFloat3 pos;
		int frame;  // when it was last ordered
		bool isRisky;
	};
	std::map<CCircuitUnit*, SFireSpot> fireSpots;
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

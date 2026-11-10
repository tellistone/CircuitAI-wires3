/*
 * DefendTask.h
 *
 *  Created on: Feb 12, 2016
 *      Author: rlcevg
 */

#ifndef SRC_CIRCUIT_TASK_FIGHTER_DEFENDTASK_H_
#define SRC_CIRCUIT_TASK_FIGHTER_DEFENDTASK_H_

#include "task/fighter/SquadTask.h"

namespace circuit {

class CDefendTask: public ISquadTask {
public:
	CDefendTask(ITaskManager* mgr, const springai::AIFloat3& position,
				FightType check, FightType promote, float maxPower, float powerMod);
	virtual ~CDefendTask();

	virtual bool CanAssignTo(CCircuitUnit* unit) const override;
	virtual void AssignTo(CCircuitUnit* unit) override;
	virtual void RemoveAssignee(CCircuitUnit* unit) override;

	virtual void Start(CCircuitUnit* unit) override;
	virtual void Update() override;

	void SetPosition(const springai::AIFloat3& pos) { position = pos; }
	void SetMaxPower(float power) { maxPower = power * powerMod; }
	// Also gather up to this metal value of units before promoting (base gather: the enemy army per ally); 0 = none
	void SetMaxCost(float cost) { maxCost = cost * powerMod; }
//	void SetWantedTarget(CEnemyInfo* enemy) { SetTarget(enemy); }

	FightType GetPromote() const { return promote; }

protected:
	float GetMaxPower() const { return maxPower; }
	float GetMaxCost() const { return maxCost; }
	float GetAttackCost() const { return attackCost; }
	bool IsGathered() const { return (attackPower >= maxPower) && (attackCost >= maxCost); }

private:
	virtual void Merge(ISquadTask* task) override;
	bool FindTarget();
	void ApplyTargetPath(const CQueryPathMulti* query);
	void FallbackFrontPos();
	void ApplyFrontPos(const CQueryPathMulti* query);
	void FallbackBasePos();
	void ApplyBasePos(const CQueryPathSingle* query);
	void Fallback();

	FightType check;
	FightType promote;
	float maxPower;
	float maxCost = 0.f;
	float attackCost = 0.f;  // metal value of the units in the group
};

} // namespace circuit

#endif // SRC_CIRCUIT_TASK_FIGHTER_DEFENDTASK_H_

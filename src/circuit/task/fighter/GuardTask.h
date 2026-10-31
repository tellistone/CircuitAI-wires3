/*
 * GuardTask.h
 *
 *  Created on: Jan 28, 2015
 *      Author: rlcevg
 */

#ifndef SRC_CIRCUIT_TASK_FIGHTER_GUARDTASK_H_
#define SRC_CIRCUIT_TASK_FIGHTER_GUARDTASK_H_

#include "task/fighter/FighterTask.h"
#include "unit/CircuitUnit.h"

namespace circuit {

/*
 * Guard slot for an early builder (see behaviour.json "guard").
 * Holds at most one fighter. The slot goes inactive, and releases its guard,
 * while the builder retreats or stands in a safe spot.
 */
class CFGuardTask: public IFighterTask {
public:
	CFGuardTask(ITaskManager* mgr, CCircuitUnit* vip, float maxPower);
	virtual ~CFGuardTask();

	virtual bool CanAssignTo(CCircuitUnit* unit) const override;

	virtual void Start(CCircuitUnit* unit) override;
	virtual void Update() override;

	virtual void OnUnitIdle(CCircuitUnit* unit) override;

	bool IsActive() const { return !isVipRetreat && !isVipSafe; }
	bool IsVipSafe() const { return isVipSafe; }

private:
	void CheckVip(CCircuitUnit* vip);
	void Release();
	void Pull(CCircuitUnit* vip);

	ICoreUnit::Id vipId;
	float maxPower;
	bool isVipRetreat;
	bool isVipSafe;
	int safeSince;  // frame the builder became safe, -1 = not safe
};

} // namespace circuit

#endif // SRC_CIRCUIT_TASK_FIGHTER_GUARDTASK_H_

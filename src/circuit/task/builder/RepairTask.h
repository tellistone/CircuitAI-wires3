/*
 * RepairTask.h
 *
 *  Created on: Jan 29, 2015
 *      Author: rlcevg
 */

#ifndef SRC_CIRCUIT_TASK_REPAIRTASK_H_
#define SRC_CIRCUIT_TASK_REPAIRTASK_H_

#include "task/common/RepairTask.h"

namespace circuit {

class CEconomyManager;

class CBRepairTask: public IRepairTask {
public:
	CBRepairTask(ITaskManager* mgr, Priority priority, CAllyUnit* target, int timeout = 0);
	virtual ~CBRepairTask();

	using IBuilderTask::Update;
	virtual void Update() override;
	virtual void Start(CCircuitUnit* unit) override;
	virtual void RemoveAssignee(CCircuitUnit* unit) override;

	virtual void OnUnitIdle(CCircuitUnit* unit) override;
	virtual void OnUnitDamaged(CCircuitUnit* unit, CEnemyInfo* attacker) override;

private:
	virtual bool Reevaluate(CCircuitUnit* unit) override;
	bool IsLeashBroken();

	std::set<CCircuitUnit*> inRange;  // builders that got within build range of the target
};

} // namespace circuit

#endif // SRC_CIRCUIT_TASK_REPAIRTASK_H_

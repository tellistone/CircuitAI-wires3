/*
 * IdleTask.h
 *
 *  Created on: Jan 13, 2015
 *      Author: rlcevg
 */

#ifndef SRC_CIRCUIT_TASK_IDLETASK_H_
#define SRC_CIRCUIT_TASK_IDLETASK_H_

#include "task/UnitTask.h"

namespace circuit {

class CIdleTask: public IUnitTask {
public:
	CIdleTask(ITaskManager* mgr);
	virtual ~CIdleTask();

	virtual void AssignTo(CCircuitUnit* unit) override;
	virtual void RemoveAssignee(CCircuitUnit* unit) override;
	// Drops a unit it still lists while the unit is in another task (CBuilderManager's COMM ROUTE check): unlike
	// RemoveAssignee, leaves the unit's actions alone
	void Forget(CCircuitUnit* unit) {
		units.erase(unit);
		updateUnits.erase(unit);
	}
	bool IsListed(CCircuitUnit* unit) const { return units.find(unit) != units.end(); }

	virtual void Start(CCircuitUnit* unit) override;
	virtual void Update() override;
	virtual void Stop(bool done) override;

	virtual void OnUnitIdle(CCircuitUnit* unit) override;
	virtual void OnUnitDamaged(CCircuitUnit* unit, CEnemyInfo* attacker) override;
	virtual void OnUnitDestroyed(CCircuitUnit* unit, CEnemyInfo* attacker) override;

private:
	std::set<CCircuitUnit*> updateUnits;
	unsigned int updateSlice;
};

} // namespace circuit

#endif // SRC_CIRCUIT_TASK_IDLETASK_H_

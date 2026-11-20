/*
 * DefenceTask.h
 *
 *  Created on: Jan 30, 2015
 *      Author: rlcevg
 */

#ifndef SRC_CIRCUIT_TASK_BUILDER_DEFENCETASK_H_
#define SRC_CIRCUIT_TASK_BUILDER_DEFENCETASK_H_

#include "task/builder/BuilderTask.h"

namespace circuit {

class CBDefenceTask: public IBuilderTask {
public:
	CBDefenceTask(ITaskManager* mgr, Priority priority,
				  CCircuitDef* buildDef, const springai::AIFloat3& position,
				  float cost, float shake, int timeout);
	virtual ~CBDefenceTask();

	virtual void Update() override;
	virtual void SetTarget(CCircuitUnit* unit) override;
	virtual void OnUnitIdle(CCircuitUnit* unit) override;

	// Picker (CTurretPicker) and cornered (MakeCommDefence) towers: no defence point counted them, so neither their
	// abort nor their tower's death takes a cost off one
	void SetOffPoints() { isOffPoints = true; }
	bool IsOffPoints() const { return isOffPoints; }
	// A picker job: built on its exact spot or not at all, and dropped (its spot masked) on arrival if the spot no
	// longer pays. creepLane: a turret.creep order's lane (its line choke id; -1: none), which keeps the creep's
	// threat rule on arrival. isAlly: the order counted A, and so does the arrival's value
	void SetPicker(int creepLane, bool isAlly) {
		isPicker = isOffPoints = true;
		this->creepLane = creepLane;
		this->isAlly = isAlly;
	}
	bool IsPicker() const { return isPicker; }
	bool IsCreep() const { return creepLane >= 0; }
	int GetCreepLane() const { return creepLane; }
	bool IsAlly() const { return isAlly; }
	// porcupine.budget: what the turret account charged for it (0 = free), and the spender
	void SetCharge(float value, int by) { charged = value; spend = by; }
	float GetCharged() const { return charged; }
	int GetSpend() const { return spend; }

protected:
	virtual void Finish() override;
	virtual void Cancel() override;
	virtual void Execute(CCircuitUnit* unit) override;

	bool isUrgent;
	bool isOffPoints;
	bool isPicker;
	bool isAlly;
	int creepLane;
	float normalCost;
	float charged;
	int spend;
};

} // namespace circuit

#endif // SRC_CIRCUIT_TASK_BUILDER_DEFENCETASK_H_

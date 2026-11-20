/*
 * MexTask.h
 *
 *  Created on: Jan 31, 2015
 *      Author: rlcevg
 */

#ifndef SRC_CIRCUIT_TASK_BUILDER_MEXTASK_H_
#define SRC_CIRCUIT_TASK_BUILDER_MEXTASK_H_

#include "task/builder/BuilderTask.h"

namespace circuit {

class CBMexTask: public IBuilderTask {
public:
	CBMexTask(ITaskManager* mgr, Priority priority,
			  CCircuitDef* buildDef, const springai::AIFloat3& position,
			  float cost, int timeout);
	virtual ~CBMexTask();

	virtual bool CanAssignTo(CCircuitUnit* unit) const override;
	virtual void AssignTo(CCircuitUnit* unit) override;
	virtual void RemoveAssignee(CCircuitUnit* unit) override;
	virtual void Update() override;

	virtual void SetTarget(CCircuitUnit* unit) override;

protected:
	virtual void Cancel() override;

	virtual void Execute(CCircuitUnit* unit) override;

public:
	virtual void OnUnitIdle(CCircuitUnit* unit) override;

private:
	// An allied AI's builder heads for the spot (closed in the shared metal manager, not by us): none of ours should
	bool IsSpotTaken() const;
	// Claim buildPos's spot for the whole ally team (CMetalManager is shared) unless an ally holds it; true = ours
	bool TryReserve();
	// Reopen the spot this job claimed, if any: never one an ally holds
	void Release();

	int reservedSpot = -1;  // metal spot this job closed in the ally team's metal manager; -1 = none
};

} // namespace circuit

#endif // SRC_CIRCUIT_TASK_BUILDER_MEXTASK_H_

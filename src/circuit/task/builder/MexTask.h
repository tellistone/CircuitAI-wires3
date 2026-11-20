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

	virtual void SetTarget(CCircuitUnit* unit) override;

	// The spot is reserved in the ally team's metal manager by someone else (an allied AI's builder heads for it):
	// no builder of ours should walk there
	bool IsSpotTaken() const;

protected:
	virtual void Cancel() override;

	virtual void Execute(CCircuitUnit* unit) override;

public:
	virtual void OnUnitIdle(CCircuitUnit* unit) override;

private:
	// Nearest spot to the job that the ally team has left open and unit can build safely; -1 if none.
	// Needs the threat map's type set for unit
	int FindOpenSpot(CCircuitUnit* unit) const;
	// Close the spot for the whole ally team (CMetalManager is shared), as ours
	void Reserve(int index);
	// Reopen the spot this job reserved, if any: never one an ally holds
	void Release();

	int reservedSpot = -1;  // metal spot this job closed in the ally team's metal manager; -1 = none
};

} // namespace circuit

#endif // SRC_CIRCUIT_TASK_BUILDER_MEXTASK_H_

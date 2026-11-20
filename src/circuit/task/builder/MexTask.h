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

protected:
	virtual void Cancel() override;

	virtual void Execute(CCircuitUnit* unit) override;

public:
	virtual void OnUnitIdle(CCircuitUnit* unit) override;

private:
	// An allied AI's builder claimed the spot (closed in the shared metal manager, not by this job): no builder of
	// ours should go there
	bool IsSpotTaken() const;
	// Claim buildPos's spot for the whole ally team (CMetalManager is shared) unless an ally holds it; true = ours.
	// A claim is ally-wide; CEconomyManager::openSpots is this AI's own reservation, taken when the job is queued
	bool TryClaim();
	// Reopen the spot this job claimed, if any and nothing is placed on it: never one an ally holds
	void Unclaim();

	int claimedSpot = -1;  // metal spot this job closed in the ally team's metal manager; -1 = none
};

} // namespace circuit

#endif // SRC_CIRCUIT_TASK_BUILDER_MEXTASK_H_

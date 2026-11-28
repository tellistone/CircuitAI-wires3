/*
 * FactoryTask.h
 *
 *  Created on: Jan 30, 2015
 *      Author: rlcevg
 */

#ifndef SRC_CIRCUIT_TASK_FACTORYTASK_H_
#define SRC_CIRCUIT_TASK_FACTORYTASK_H_

#include "task/builder/BuilderTask.h"

#include <vector>

namespace circuit {

class CBFactoryTask: public IBuilderTask {
public:
	CBFactoryTask(ITaskManager* mgr, Priority priority,
				  CCircuitDef* buildDef, const springai::AIFloat3& position,
				  float cost, float shake, bool isPlop, int timeout);
	virtual ~CBFactoryTask();

	virtual void Start(CCircuitUnit* unit) override;
	virtual void Update() override;
protected:
	virtual void Cancel() override;

public:
	virtual void OnUnitIdle(CCircuitUnit* unit) override;

private:
	virtual void FindBuildSite(CCircuitUnit* builder, const springai::AIFloat3& pos, float searchRadius) override;

	// A mobile friendly unit, other than builder, stands in the footprint at pos (plop: it may never step aside)
	bool IsOccupied(const springai::AIFloat3& pos, int face, CCircuitUnit* builder) const;
	// Plop: give up the current site as failed and pick another
	void ReplaceSite(CCircuitUnit* unit);

	bool isPlop;
	int stallFrame;  // plop: frame the commander last moved (to stallPos) without a frame placed; -1 = not yet
	int checkFrame;  // plop: next stall check
	springai::AIFloat3 stallPos;
	std::vector<springai::AIFloat3> badSites;  // plop: sites where the order stalled or failed
};

} // namespace circuit

#endif // SRC_CIRCUIT_TASK_FACTORYTASK_H_

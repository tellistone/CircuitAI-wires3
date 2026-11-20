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

	// Plop: a mobile friendly unit (often an allied commander, busy with its own orders) stands in the footprint
	bool IsOccupied(const springai::AIFloat3& pos, int face) const;
	// Plop: give up the current site, if the build order hasn't started on it, and pick another
	void ReplaceSite(CCircuitUnit* unit);

	bool isPlop;
	int stallFrame;  // plop: frame the commander was first seen in build range without a frame placed; -1 = not yet
	std::vector<springai::AIFloat3> badSites;  // plop: sites where the build order stalled
};

} // namespace circuit

#endif // SRC_CIRCUIT_TASK_FACTORYTASK_H_

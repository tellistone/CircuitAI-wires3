/*
 * FactoryTask.h
 *
 *  Created on: Jan 30, 2015
 *      Author: rlcevg
 */

#ifndef SRC_CIRCUIT_TASK_FACTORYTASK_H_
#define SRC_CIRCUIT_TASK_FACTORYTASK_H_

#include "task/builder/BuilderTask.h"

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

private:
	virtual void FindBuildSite(CCircuitUnit* builder, const springai::AIFloat3& pos, float searchRadius) override;

	// A mobile friendly unit, other than builder, stands in the footprint at pos (plop: it may never step aside)
	bool IsOccupied(const springai::AIFloat3& pos, int face, CCircuitUnit* builder) const;

	bool isPlop;
	int stallFrame;  // plop: frame the commander was first seen in range of its site with a unit in it; -1 = no
	int checkFrame;  // plop: next stall check
};

} // namespace circuit

#endif // SRC_CIRCUIT_TASK_FACTORYTASK_H_

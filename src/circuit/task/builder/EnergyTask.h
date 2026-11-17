/*
 * EnergyTask.h
 *
 *  Created on: Jan 30, 2015
 *      Author: rlcevg
 */

#ifndef SRC_CIRCUIT_TASK_BUILDER_ENERGYTASK_H_
#define SRC_CIRCUIT_TASK_BUILDER_ENERGYTASK_H_

#include "task/builder/BuilderTask.h"
#include "resource/EnergyGrid.h"

namespace circuit {

class CBEnergyTask: public IBuilderTask {
public:
	CBEnergyTask(ITaskManager* mgr, Priority priority,
				 CCircuitDef* buildDef, const springai::AIFloat3& position,
				 float cost, float shake, int timeout);
	virtual ~CBEnergyTask();

	virtual void Update() override;
	virtual void Finish() override;
	virtual void Cancel() override;

	// economy.energy.fill: the site must keep these grid links, here and when searched again. With link (a gap of
	// the grid plan), the gap is held as being built while the job lives
	void SetLinkSite(const CEnergyGrid::SLinkSite& site, IGridLink* gap);

protected:
	virtual void Execute(CCircuitUnit* unit) override;
	// A link site: one that keeps its links (never the wide search that could miss them). Else the usual search
	virtual void FindBuildSite(CCircuitUnit* builder, const springai::AIFloat3& pos, float searchRadius) override;

private:
	void ReleaseLink();

	CEnergyGrid::SLinkSite linkSite;
	bool isLinkSite;
	IGridLink* link;
};

} // namespace circuit

#endif // SRC_CIRCUIT_TASK_BUILDER_ENERGYTASK_H_

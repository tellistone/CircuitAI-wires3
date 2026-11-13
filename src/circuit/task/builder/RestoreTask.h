/*
 * RestoreTask.h
 *
 *  Restore ground the enemy terraformed (trenches, walls) to the map's original height: ZK's restore terraform
 *  (CMD_TERRAFORM_INTERNAL type 5) over one square cell, then CMD_RESTORE repairs its terraunits.
 *  CBuilderManager::UpdateRestore finds the cells and queues the jobs.
 */

#ifndef SRC_CIRCUIT_TASK_BUILDER_RESTORETASK_H_
#define SRC_CIRCUIT_TASK_BUILDER_RESTORETASK_H_

#include "task/builder/BuilderTask.h"

#include <map>

namespace circuit {

class CBRestoreTask: public IBuilderTask {
public:
	// cell: index in CBuilderManager's restore grid, the square to restore
	CBRestoreTask(ITaskManager* mgr, Priority priority, int cell);
	virtual ~CBRestoreTask();

	// A builder within restore.range, no threat to it on the cell, up to restore.builders on it
	virtual bool CanAssignTo(CCircuitUnit* unit) const override;
	virtual void AssignTo(CCircuitUnit* unit) override;
	virtual void RemoveAssignee(CCircuitUnit* unit) override;

	virtual void Stop(bool done) override;
protected:
	// Dropped: the builders still on it stop, so none keeps restoring next to what made us drop it
	virtual void Cancel() override;
	virtual void Execute(CCircuitUnit* unit) override;
	// The builder's threat-aware path there (asked again while it walks): any threat on it drops the job
	virtual void ApplyPath(const CQueryPathSingle* query) override;

public:
	virtual void OnUnitIdle(CCircuitUnit* unit) override;

	int GetCell() const { return cell; }
	bool IsAssigned() const { return isAssigned; }

private:
	int cell;
	int tag;  // ZK's terraform command tag, -1 = no terraform made yet
	bool isAssigned;  // a builder has taken the job: its end puts the cell on hold
	std::map<CCircuitUnit*, int> orderFrames;  // frame each builder got its own restore order
};

} // namespace circuit

#endif // SRC_CIRCUIT_TASK_BUILDER_RESTORETASK_H_

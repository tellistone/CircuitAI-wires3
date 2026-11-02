/*
 * SquadTask.h
 *
 *  Created on: Jan 23, 2016
 *      Author: rlcevg
 */

#ifndef SRC_CIRCUIT_TASK_FIGHTER_SQUADTASK_H_
#define SRC_CIRCUIT_TASK_FIGHTER_SQUADTASK_H_

#include "task/fighter/FighterTask.h"
#include "terrain/path/MicroPather.h"

#include <memory>

namespace circuit {

class ISquadTask: public IFighterTask {
protected:
	ISquadTask(ITaskManager* mgr, FightType type, float powerMod);
public:
	virtual ~ISquadTask();

	virtual void AssignTo(CCircuitUnit* unit) override;
	virtual void RemoveAssignee(CCircuitUnit* unit) override;

	virtual void Merge(ISquadTask* task);

	CCircuitUnit* GetLeader() const { return leader; }
	const springai::AIFloat3& GetLeaderPos(int frame) const;

private:
	void FindLeader(decltype(units)::iterator itBegin, decltype(units)::iterator itEnd);

	bool IsMergeSafe() const;

protected:
	// Nearest same-type task this one may join (no ground/influence check, see GetMergeTask)
	ISquadTask* CheckMergeTask();
	// Is the straight line from our leader to the host's leader safe to walk for merging?
	// Default: walkable and no threat at all (CQueryLineMap::IsSafeLine)
	virtual bool IsSafeMergeLine(const ISquadTask* host) const;
	ISquadTask* GetMergeTask();
	// Also releases stuck units (far from the regroup spot after 60 s, or a leader that didn't move between
	// regroups) to the idle pool; leader == nullptr afterwards if none are left
	bool IsMustRegroup();
	void ReleaseStuck(std::vector<CCircuitUnit*>& stuck);
	void ActivePath(float speed = NO_SPEED_LIMIT);
	NSMicroPather::TestFunc GetHitTest() const;

	float lowestRange;
	float highestRange;
	float lowestSpeed;
	float highestSpeed;
	// NOTE: Using unit instead of area directly may save from processing UpdateAreaUsers
	CCircuitUnit* leader;  // slowest, weakest unit, true leader
	springai::AIFloat3 groupPos;
	springai::AIFloat3 prevGroupPos;
	std::shared_ptr<PathInfo> pPath;

	int groupFrame;

#ifdef DEBUG_VIS
public:
	virtual void Log() override;
#endif
};

} // namespace circuit

#endif // SRC_CIRCUIT_TASK_FIGHTER_SQUADTASK_H_

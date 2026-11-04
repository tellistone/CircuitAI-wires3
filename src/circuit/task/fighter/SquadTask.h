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

	/*
	 * Escape check of attack and raid groups. Danger: threat at or above GetAllowedThreatAt for GetEscapeSafeThreat
	 */
	// Fills 'endangered' (units standing in danger; cloaked ones can't be shot) and 'isPathAheadUnsafe' (danger on the
	// leader's path within ~2 s of walking). True: the endangered units hold at least fleeShare of the group's strength
	bool IsInDanger(int frame, float fleeShare);
	// Endangered units that didn't make the whole group flee go to the strength-weighted centre of the safe ones, at
	// most once a second. Also when the leader is the one in danger (the group follows its path, so a stuck leader would
	// keep everyone heading into it). A unit sent to a RISKY firing spot in the last 3 s stays.
	void PullBackEndangered(int frame);
	std::vector<CCircuitUnit*> endangered;  // from the last IsInDanger
	bool isPathAheadUnsafe = false;  // from the last IsInDanger
	int pullFrame = 0;  // last pull-back

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

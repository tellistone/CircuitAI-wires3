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
#include "unit/CircuitDef.h"

#include <functional>
#include <memory>

namespace circuit {

class CMap;
class CQueryPathSingle;
class CQueryPathMulti;

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
	// IsMustRegroup ignores this unit (anti-heavy: stepping out of reach while it reloads)
	virtual bool IsRegroupExempt(CCircuitUnit* unit) const { return false; }
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

	/*
	 * Threat-capped paths and the squad flee (anti-air, anti-heavy)
	 */
	static constexpr float FALLBACK_PATH_RANGE = DEFAULT_SLACK * 4;  // goal tolerance of safety, base, commander and flee paths
	static const int FLEE_REPLAN_FRAMES = FRAMES_PER_SEC * 2;  // re-plan the flee this often while the danger stays
	// Is an unidentified radar blip at p high enough above the ground (or water surface) to be a plane?
	static bool IsHighBlip(CMap* map, const springai::AIFloat3& p);
	float GetSafeThreat() const;  // current squad strength (health-scaled power) x powerMod
	// Multiplier on GetPathThreatCap's squad allowance; < 0 = no cap at all
	virtual float GetPathThreatMod() const { return 1.f; }
	// Pathfinder threat cap for a route from startPos to goals (see .cpp)
	float GetPathThreatCap(CCircuitUnit* unit, const springai::AIFloat3& startPos, const F3Vec& goals) const;
	// Path for unit to goal (isCapped: under GetPathThreatCap, else through any threat); apply runs if still current
	void QueryPath(CCircuitUnit* unit, const springai::AIFloat3& goal, float range, bool isCapped,
			std::function<void (const CQueryPathSingle*)>&& apply);
	// Path for the leader to the nearest of goals within range, under GetPathThreatCap
	void QueryPathMulti(const F3Vec& goals, float range, std::function<void (const CQueryPathMulti*)>&& apply);
	// The nearest enemy we see now that can act and shoot the squad (IsGroundDanger; isGunship: gunships count),
	// within its ground (or water) weapon range + margin of any squad unit; IsFleeIgnored enemies and high radar
	// blips (planes) don't count. dangerThreat: the threat of every such enemy, each counted once
	CEnemyInfo* FindFleeDanger(float margin, bool isGunship, float& dangerThreat) const;
	virtual bool IsFleeIgnored(CEnemyInfo* enemy) const { return false; }  // FindFleeDanger skips it
	// Escorts next to pos (our and allied armed units within allyRadius: mobile ones that aren't aircraft or of
	// ownRole (main role, so squads of one kind can't cover each other), and static defences that can hit ground),
	// minus enemyThreat, reach allyThreat
	bool IsFleeCovered(const springai::AIFloat3& pos, float enemyThreat, float allyThreat, float allyRadius,
			CCircuitDef::RoleT ownRole) const;
	// Flee goal for the leader away from dangerPos: a safe spot (attack/defend group leaders, else a defended mex
	// cluster, else base) or the base (PickFleeSpot); nullptr if none. Fills fleeSpots
	const springai::AIFloat3* FindFleeGoal(const springai::AIFloat3& dangerPos);
	F3Vec fleeSpots;  // FindFleeGoal scratch

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

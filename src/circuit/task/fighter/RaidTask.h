/*
 * RaidTask.h
 *
 *  Created on: Jan 6, 2016
 *      Author: rlcevg
 */

#ifndef SRC_CIRCUIT_TASK_FIGHTER_RAIDTASK_H_
#define SRC_CIRCUIT_TASK_FIGHTER_RAIDTASK_H_

#include "task/fighter/SquadTask.h"
#include "unit/CoreUnit.h"

#include <map>

namespace circuit {

class CMilitaryManager;

class CRaidTask: public ISquadTask {
public:
	CRaidTask(ITaskManager* mgr, float maxPower, float powerMod);
	virtual ~CRaidTask();

	virtual bool CanAssignTo(CCircuitUnit* unit) const override;
	virtual void AssignTo(CCircuitUnit* unit) override;
	virtual void RemoveAssignee(CCircuitUnit* unit) override;

	virtual void Start(CCircuitUnit* unit) override;
	virtual void Update() override;

	virtual void OnUnitIdle(CCircuitUnit* unit) override;

	// quota.raid_rally: a raider whose group moves like ours (IsMoveCompatible at quota.attack_merge.speed_ratio)
	virtual bool IsRallyCompatible(CCircuitUnit* unit) const override;
	// The group led by unit (or the unit alone) and ours together keep all speeds within quota.attack_merge.speed_ratio
	bool IsSpeedSpreadOk(CCircuitUnit* unit) const;

private:
	CMilitaryManager* GetMilitaryMgr() const;
	// May take more units: up to maxPower, and during a raid rally until its strength (with the group size bonus)
	// reaches the rally's goal. Units of other types may join only during a raid rally, and only compatible ones
	bool HasRoom() const;
	bool IsInRally() const;  // a raid rally is on and we are its host or a joiner
	bool IsCautious(int frame) const { return frame < fleeFrame + FRAMES_PER_SEC * 4; }  // just escaped
	// GetMergeTask, but in a rally every 2 updates, and judged by the straight line (IsSafeMergeLine), not influence
	ISquadTask* GetRaidMergeTask();
	// In a rally: the straight line to the other leader under our allowance, as our escape judges it. Else the default
	virtual bool IsSafeMergeLine(const ISquadTask* host) const override;
	// The chase route's cap (the only capped QueryPath of a raid group): x0.75 for 4 s after an escape, as FindTarget
	virtual float GetPathThreatMod() const override;

	/*
	 * Raid rally joiner (quota.raid_rally): chase the nearest bigger compatible raid group within CHASE_TRAVEL_SEC
	 * and merge into it. Fights back only at targets in range on the way. No safe route: raid normally (never wait);
	 * CHASE_FAILS empty routes in a row: raid normally for CHASE_RETRY_FRAMES
	 */
	// The group to chase now (nullptr: raid normally). Keeps the last one while it is still bigger and in reach
	CRaidTask* UpdateChase(int frame);
	bool IsChaseable(const CRaidTask* group, int frame, float sqMaxDist) const;  // bigger, compatible, reachable, near
	const CRaidTask* FindChaseTask(int frame) const;  // the nearest chaseable group within CHASE_TRAVEL_SEC
	void Chase(int frame, const CRaidTask* group);
	void ApplyChasePath(const CQueryPathSingle* query);
	const CRaidTask* chaseTask;  // the group we chase (validated against the task set before use)
	int chaseFails;  // empty chase routes in a row
	int chaseRetryFrame;  // after CHASE_FAILS: raid normally until this frame
	bool isChaseSkip;  // the last chase route came back empty: the next decision raids normally
	int chaseRouteFrame;  // the last chase route: when asked, for which goal (reused CHASE_ROUTE_FRAMES)
	springai::AIFloat3 chaseGoal;
	std::shared_ptr<PathInfo> chaseRoute;  // ...once it arrived (kept only while the leader walks it)
	bool isChaseDest;  // 'position' is a chase goal: FallbackRaid picks a new destination
	int nearFightFrame;  // beside the chased group: the last Fight order to its leader, when and where
	springai::AIFloat3 nearFightPos;  // -RgtVector: none since other orders

	bool FindTarget();
	void ApplyTargetPath(const CQueryPathMulti* query);
	// isContestedAllowed: may head for a contested cluster (not after a failed route to targets: its path is uncapped)
	void FallbackRaid(bool isContestedAllowed = true);
	// quota.raider_spot: keep or pick the scored spot to roam to. true = 'position' is that spot; false = roam the
	// old way, which must pick a new destination now if isSpotDone (the spot we were heading to is done, no next one)
	bool UpdateRaidSpot(float safeThreat, bool mustRepick, bool isDanger, bool& isSpotDone);
	void ApplyRaidPath(const CQueryPathSingle* query);

	// Escape: current group strength vs threat where the units are and just ahead on the path
	float GetSafeThreatAt(const springai::AIFloat3& pos, float safeThreat) const;
	// Firing spots (IFighterTask::Attack) skip spots the escape would call dangerous
	virtual float GetEscapeSafeThreat() const override { return GetSafeThreat(); }
	virtual float GetAllowedThreatAt(const springai::AIFloat3& pos, float strength) const override {
		return GetSafeThreatAt(pos, strength);
	}
	bool Flee(int frame);  // false: nowhere better to go (already home), fight instead
	bool IsFleeQueryPending() const;  // our escape path request is still in flight
	void ApplyFleePath(const CQueryPathSingle* query);

	float maxPower;
	int fleeFrame;  // keep running until this frame
	int fleePathFrame;  // last escape path request
	int fleeQueryId;  // pending escape path request (-1: none), not superseded by the 1 s refresh
	springai::AIFloat3 fleeDest;  // haven (or base) of the current escape: straight-move fallback when no path is found
	bool isFightPending;
	std::map<ICoreUnit::Id, int> heldTargets;  // targets with no safe firing spot in reach, and when: skipped for 10 s
	bool isContestedRoute;  // the pending target route goes only to enemies in contested clusters
	int contestedFailFrame;  // when such a route last came back empty: all goals again for 10 s
	bool isContestedDest;  // 'position' is a contested cluster picked by FallbackRaid
	// quota.raider_spot: the scored spot FallbackRaid roams to, and the move position made from it (-RgtVector = none).
	// Re-picked once the spot is in our LOS, turns dangerous, or 'position' was changed by something else
	springai::AIFloat3 raidSpot;
	springai::AIFloat3 raidSpotPos;
	int raidSpotFailFrame;  // last frame no spot qualified: wait 5 s before scoring all spots again
	int roamPathFrame;  // when FallbackRaid last asked for a roam route
	springai::AIFloat3 roamGoal;  // ...and to where: re-planned only every 3 s while walking it safely
	std::shared_ptr<PathInfo> roamPath;  // the route to roamGoal once it arrived (kept only while the leader walks it)
	void AttackDirect(int frame);  // direct Attack, no firing spots (moving targets)
	bool IsAllHolding() const;  // every unit with spots (melee too) holds without one, target out of their reach
	void SendPendingFight(int frame);  // short roam path: the Fight order sent from Update() (not from the path callback)
};

} // namespace circuit

#endif // SRC_CIRCUIT_TASK_FIGHTER_RAIDTASK_H_

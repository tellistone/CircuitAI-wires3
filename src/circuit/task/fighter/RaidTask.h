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

private:
	bool FindTarget();
	void ApplyTargetPath(const CQueryPathMulti* query);
	// isContestedAllowed: may head for a contested cluster (not after a failed route to targets: its path is uncapped)
	void FallbackRaid(bool isContestedAllowed = true);
	// quota.raider_spot: keep or pick the scored spot to roam to. true = 'position' is that spot; false = roam the
	// old way, which must pick a new destination now if isSpotDone (the spot we were heading to is done, no next one)
	bool UpdateRaidSpot(float safeThreat, bool mustRepick, bool isDanger, bool& isSpotDone);
	void ApplyRaidPath(const CQueryPathSingle* query);

	// Escape: current group strength vs threat where the units are and just ahead on the path
	float GetSafeThreat() const;
	float GetSafeThreatAt(const springai::AIFloat3& pos, float safeThreat) const;
	// Firing spots (IFighterTask::Attack) skip spots the escape would call dangerous
	virtual float GetEscapeSafeThreat() const override { return GetSafeThreat(); }
	virtual float GetAllowedThreatAt(const springai::AIFloat3& pos, float strength) const override {
		return GetSafeThreatAt(pos, strength);
	}
	bool IsInDanger(int frame, bool& isPathUnsafe);  // fills 'endangered'
	float GetUnitPower(CCircuitUnit* unit) const;
	void PullBackEndangered(int frame);
	bool Flee(int frame);  // false: nowhere better to go (already home), fight instead
	bool IsFleeQueryPending() const;  // our escape path request is still in flight
	void ApplyFleePath(const CQueryPathSingle* query);

	float maxPower;
	int fleeFrame;  // keep running until this frame
	int fleePathFrame;  // last escape path request
	int fleeQueryId;  // pending escape path request (-1: none), not superseded by the 1 s refresh
	springai::AIFloat3 fleeDest;  // haven (or base) of the current escape: straight-move fallback when no path is found
	int pullFrame;  // last pull-back of endangered stragglers
	std::vector<CCircuitUnit*> endangered;
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
	bool isPathAheadUnsafe;  // from the last IsInDanger: the stretch of path ahead crosses too much threat
	int roamPathFrame;  // when FallbackRaid last asked for a roam route
	springai::AIFloat3 roamGoal;  // ...and to where: re-planned only every 3 s while walking it safely
	std::shared_ptr<PathInfo> roamPath;  // the route to roamGoal once it arrived (kept only while the leader walks it)
	void AttackDirect(int frame);  // direct Attack, no firing spots (moving targets)
	bool IsAllHolding() const;  // every ranged unit holds without a spot, target out of their reach  // short roam path: Fight order to send from Update() (not from the path callback)
	void SendPendingFight(int frame);  // units standing in danger, from the last IsInDanger
};

} // namespace circuit

#endif // SRC_CIRCUIT_TASK_FIGHTER_RAIDTASK_H_

/*
 * AttackTask.h
 *
 *  Created on: Jan 28, 2015
 *      Author: rlcevg
 */

#ifndef SRC_CIRCUIT_TASK_FIGHTER_ATTACKTASK_H_
#define SRC_CIRCUIT_TASK_FIGHTER_ATTACKTASK_H_

#include "task/fighter/SquadTask.h"
#include "unit/CoreUnit.h"

#include <map>

namespace circuit {

class CAttackTask: public ISquadTask {
public:
	CAttackTask(ITaskManager* mgr, float minPower, float powerMod);
	virtual ~CAttackTask();

	virtual bool CanAssignTo(CCircuitUnit* unit) const override;
	// CanAssignTo without the quota.attack_merge.distance limit: speed ratio, movement type, not a worse mover
	bool IsCompatible(CCircuitUnit* unit) const;
	// Walk to this stronger group and merge into it instead of hunting targets alone (while a safe route exists)
	void SetReinforce(CAttackTask* squad) { reinforce = squad; }
	virtual void AssignTo(CCircuitUnit* unit) override;
	virtual void RemoveAssignee(CCircuitUnit* unit) override;
	virtual void Merge(ISquadTask* task) override;

	virtual void Start(CCircuitUnit* unit) override;
	virtual void Update() override;

	virtual void OnUnitIdle(CCircuitUnit* unit) override;
	virtual void OnUnitDamaged(CCircuitUnit* unit, CEnemyInfo* attacker) override;

private:
	ISquadTask* GetAttackMergeTask();
	// Attack (firing spots), and walk stragglers (IsOutOfReach) to the leader if they have no path
	void Engage(int frame);
	float GetGroupRadius() const;  // quota.attack_group_radius
	// Straggler: farther from the leader than GetGroupRadius and the target out of its weapon range.
	// Gets no firing-spot order (keeps travelling)
	virtual bool IsOutOfReach(CCircuitUnit* unit, int frame) const override;
	bool IsFighting() const;  // ENGAGE with the target within some unit's weapon range: no merging away
	// Straight line from our leader to the host's: threat below our allowance, both groups' strength near the host
	virtual bool IsSafeMergeLine(const ISquadTask* host) const override;
	CAttackTask* GetReinforceSquad();
	void Reinforce(const int frame);
	void ApplyReinforcePath(const CQueryPathSingle* query);
	void AbsorbWeakerTasks();
	void FindTarget();
	// Threat on the line from 'from' towards 'to' (out to maxReach) stays below the allowance for 'strength' at each
	// sampled spot, or for 'supportStrength' within supportRadius of 'to'. Used by the target approach check (from the
	// target to where its route ends) and merging (from our leader to the host's)
	bool IsLineSafe(const springai::AIFloat3& from, const springai::AIFloat3& to, float maxReach,
			float strength, float supportStrength, float supportRadius) const;
	// Fighting a target inside the base defence radius: stand and fight instead of fleeing
	bool IsHoldingGround(int frame) const;
	// Threat cap for target and reinforce paths: never above what the escape tolerates
	float GetPathThreatCap(const springai::AIFloat3& startPos, const F3Vec& goals) const;
	// quota.attack_keep_threat applies to the current target only outside the base defence radius
	bool IsKeepAllowedAt(const springai::AIFloat3& pos) const;
	// quota.attack_path_ignore_threat on a capped route: light threat costs nothing
	void SetPathThreatFloor(IPathQuery* query) const;
	// quota.attack_path_dynamic_cost: threat cost scaled by threat / the group's current strength, per route kind
	void SetPathThreatStrength(IPathQuery* query, int kind) const;
	// Escape, as in CRaidTask: group strength from current health, units standing in danger, flee / pull back
	float GetSafeThreat() const;
	virtual float GetEscapeSafeThreat() const override { return GetSafeThreat(); }
	float GetUnitPower(CCircuitUnit* unit) const;
	// The one threat allowance used everywhere (picking, approach line, escape, path ahead, firing spots,
	// flee target, route caps): strength x base_bonus(spot) / thr_mod.distance(spot) / scale(spot)
	float GetNearBaseScaleAt(const springai::AIFloat3& pos) const;
	virtual float GetAllowedThreatAt(const springai::AIFloat3& pos, float strength) const override;
	bool IsInDanger(int frame, bool& isPathUnsafe);  // fills 'endangered'
	void PullBackEndangered(int frame);
	bool Flee(int frame);  // false: nowhere better to go, fight instead
	void ApplyFleePath(const CQueryPathSingle* query);
	// One capped route to all candidate targets (goals in the order of ids)
	void RequestTargetPath(std::vector<ICoreUnit::Id> ids, F3Vec goals);
	void ApplyTargetPath(const CQueryPathMulti* query, const std::vector<ICoreUnit::Id>& ids);
	// The ground we'd fight the target from, along this route: the line from the target to the route's end,
	// and the route's last stretch as long as our range; every spot below the allowance for 'strength'
	bool IsApproachSafe(const F3Vec& path, const springai::AIFloat3& targetPos, float strength) const;
	// Nothing to go to: hold at the quietest reachable spot within 2x our range (never head home),
	// avoiding the spot we were last hit at while holding; retry next decision
	// highestRange = max over all units of (weapon range, sight radius), rebuilt the same way on join, leave, merge
	void UpdateHighestRange();
	void HoldPosition();
	void MoveGroupTo(const springai::AIFloat3& pos);
	// Quietest reachable spot within 2x our range (24 samples + where we stand), avoiding the last hit area;
	// returns the threat there
	float FindQuietSpot(const springai::AIFloat3& pos, springai::AIFloat3& outPos);
	// In danger but not fleeing (hold-ground): move to the quietest spot nearby by the
	// least-threat route; already there: attack the nearest enemy we can hit. Re-checked every second.
	void Cornered(int frame);
	void ApplyCorneredPath(const CQueryPathSingle* query);
	CEnemyInfo* FindNearestHittable(int frame) const;
	bool IsOnEnemyGround() const;  // influence at the leader < 0
	// No target: route to the front line, or hold nearby when past it on enemy-held ground (or no front / no safe route)
	void FallbackFrontPos();
	void ApplyFrontPos(const CQueryPathMulti* query);

	float minPower;
	CAttackTask* reinforce;  // stronger group to join; validated through CMilitaryManager::GetTasks before use
	int reinforceFails;  // consecutive reinforce paths that came back empty
	int reinforceRetryFrame;  // after giving up, don't look for a group to reinforce before this frame
	int fleeFrame;  // keep running until this frame
	int fleePathFrame;  // last escape path request
	int fleeQueryId;  // id of the escape path query still in flight, -1 = none
	bool IsFleeQueryPending() const;
	int pullFrame;  // last pull-back of endangered stragglers
	std::vector<CCircuitUnit*> endangered;  // units standing in danger, from the last IsInDanger
	std::vector<ICoreUnit::Id> candidateIds;  // FindTarget: screened targets, nearest first (max 16)
	F3Vec candidatePos;  // their positions, goals of the target route
	std::vector<ICoreUnit::Id> screenedIds;  // FindTarget: all screened targets before stickiness trims them (retry goals)
	F3Vec screenedPos;
	bool isHolding;  // HoldPosition is in effect (until the next decision or escape)
	int stepFrame;  // last step back after being hit while holding
	springai::AIFloat3 hitPos;  // where we were hit while holding (avoided by HoldPosition for 30 s)
	float hitRadius;
	int hitFrame;
	float scaleRefSqDist;  // leader-to-base distance^2 when the current target was first picked: reference of the near-base scale
	ICoreUnit::Id anchorTargetId;  // target that scaleRefSqDist belongs to (-1: none)
	int targetRouteTries;  // routes so far in this target decision (MAX_TARGET_ROUTES)
	std::map<ICoreUnit::Id, int> rejectedUntil;  // targets dropped for an unsafe route approach -> skip until frame
	int corneredFrame;  // last cornered check
	bool isReplanForced;  // a forced re-plan (unsafe path ahead) was used since the last normal decision
	springai::AIFloat3 corneredPos;  // spot we're moving to while cornered (-RgtVector: none)
	bool isCorneredHold;  // cornered while holding ground: keep holding (no flee) until the danger ends
	bool isAtFleeDest;
	springai::AIFloat3 fleeDest;  // where the current escape runs to (straight-move fallback when no path)  // flee gave up: already within 400 elmos of where it would run to
	float pickSqDist;  // leader-to-base distance^2 at the last FindTarget
};

} // namespace circuit

#endif // SRC_CIRCUIT_TASK_FIGHTER_ATTACKTASK_H_

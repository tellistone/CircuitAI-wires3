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

#include <functional>
#include <map>

namespace circuit {

class CMilitaryManager;
enum class AttackRoute: int;  // module/MilitaryManager.h

class CAttackTask: public ISquadTask {
public:
	CAttackTask(ITaskManager* mgr, float minPower, float powerMod);
	virtual ~CAttackTask();

	virtual bool CanAssignTo(CCircuitUnit* unit) const override;
	// CanAssignTo without the quota.attack_merge.distance limit: speed ratio, movement type, not a worse mover
	bool IsCompatible(CCircuitUnit* unit) const;
	// Walk to this stronger group and merge into it instead of hunting targets alone (while a safe route exists)
	void SetReinforce(CAttackTask* squad) { reinforce = squad; }
	// Raw current strength (GetStrengthSum: power x sqrt(health share)): quota.attack_merge.rally compares it with blobs
	float GetRallyStrength() const { return GetStrengthSum(); }
	// The rally host's meeting point, while it walks or waits there (not engaged, not fleeing); false: go to the host
	bool GetMeetPoint(int frame, springai::AIFloat3& outPos) const;
	// A guard slot may pull this group's unit (CFGuardTask::Pull): it is alone, not fighting and not the rally host.
	// With quota.attack_merge.base_gather false a new riot walks out alone, so this is how it gets pulled
	bool IsGuardPullable() const;
	virtual void AssignTo(CCircuitUnit* unit) override;
	virtual void RemoveAssignee(CCircuitUnit* unit) override;
	virtual void Merge(ISquadTask* task) override;

	virtual void Start(CCircuitUnit* unit) override;
	// Merging, escape and regrouping on every update; a decision (Decide) every 2 updates, or when a unit asks
	virtual void Update() override;

	virtual void OnUnitIdle(CCircuitUnit* unit) override;
	virtual void OnUnitDamaged(CCircuitUnit* unit, CEnemyInfo* attacker) override;

protected:
	// The planned firing cell's side of the current target (quota.attack_static_cells), while it is still valid
	virtual bool GetFireArcBearing(float& outBearing) const override;

private:
	// A target route goal: one candidate target, or one firing cell of a cell target (quota.attack_static_cells:
	// a building, or a parked mobile)
	struct STargetGoal {
		ICoreUnit::Id id;
		springai::AIFloat3 pos;  // the target's position; for a firing cell, its anchor (GetCellAnchor)
		float key;  // ranking: squared distance x near-base scale x target_dist_mod^2 (/ quota.contested.bonus^2)
		springai::AIFloat3 cell = springai::AIFloat3();  // firing cell (IsCell only)
		int sector = -1;  // >= 0: a firing cell's goal, a wedge of that sector around the target; -1: a disc around it
		int epoch = -1;  // the anchor's epoch the cell was built under (IsCell only)

		bool IsCell() const { return sector >= 0; }
	};
	// A firing cell of a cell target: where the group can shoot it from, under the threat allowance
	struct SFiringCell {
		springai::AIFloat3 pos;
		int sector;
	};
	// A cell target with no safe firing cell: skipped beyond LOS range until 'until' (growing while nothing changes)
	struct SCellBan {
		int until;  // frame
		int since;  // frame of the first ban in a row: the ban and its level reset 60 s after it
		float threat;  // threat at the target's spot at the last ban: a change of more than 25% resets the ban
		int level;  // 0, 1, 2: 6, 12, 24 s
		int epoch;  // the anchor's epoch: dropped when it changes (a parked mobile moved)
	};
	// A rejected side (or whole target, sector -1) of a target, skipped until 'until'
	struct SRejection {
		int until;  // frame
		int epoch;  // a side counts only under the anchor's epoch it was rejected in
	};
	// The last target or front route that was walked, so the next decision can keep it (ReuseRoute)
	struct SRoute {
		std::shared_ptr<PathInfo> path;  // nullptr: none
		springai::AIFloat3 goal;  // the target's position, or the front's first position
		ICoreUnit::Id targetId;  // -1: front route
		int frame;  // when it arrived
		int reuseFrames;  // kept for this long (longer for a static target's firing cell: buildings don't move)
		// The firing cell it leads to and its sector (IsCell only): the side we plan to fight the target from, for as
		// long as it stays our target (GetPlannedCell)
		springai::AIFloat3 cell;
		int sector;  // -1: no cell
		float goalSlack;  // the goal may move this far and the route still counts (routes to groups); else one square
		int epoch;  // the target anchor's epoch (IsCell only): the planned side holds only under it

		bool IsCell() const { return sector >= 0; }
	};

	CMilitaryManager* GetMilitaryMgr() const;

	/*
	 * Group strength and the threat it tolerates
	 */
	// GetSafeThreat (ISquadTask) of this group and squad together, with the group size bonus for both groups' units
	float GetJointSafeThreat(const CAttackTask* squad) const;
	virtual float GetEscapeSafeThreat() const override { return GetSafeThreat(); }
	// Strength for picking a target and checking its approach: lower for a few seconds after an escape
	float GetPickStrength(int frame) const;
	// strength x quota.attack_keep_threat (not inside the base defence radius): what keeps the current target
	float GetKeepStrength(const springai::AIFloat3& targetPos, float strength) const;
	// The one threat allowance used everywhere (picking, approach line, escape, path ahead, firing spots,
	// flee destination, route caps): strength x base_bonus(spot) / thr_mod.distance(spot) / scale(spot).
	// refSqDist: the near-base scale's reference (0: none); the 2-argument form uses the current fight's
	float GetAllowedThreatAt(const springai::AIFloat3& pos, float strength, float refSqDist) const;
	virtual float GetAllowedThreatAt(const springai::AIFloat3& pos, float strength) const override;
	// IFighterTask::IsDangerAt with the near-base reference refSqDist
	using IFighterTask::IsDangerAt;
	bool IsDangerAt(CCircuitUnit* unit, const springai::AIFloat3& p, float strength, float refSqDist) const;
	// Threat at spots nearer our base than refSqDist (squared distance to base) counts less, floored by
	// quota.thr_mod.scale_min
	float GetNearBaseScaleAt(const springai::AIFloat3& pos, float refSqDist) const;
	// The current fight's near-base reference (scaleRefSqDist, anchorTargetId): kept only for a fight towards home,
	// i.e. a target nearer our base than refSqDist
	void SetScaleAnchor(const CEnemyInfo* enemy, float refSqDist);
	void ClearScaleAnchor();
	bool IsInBaseRadius(const springai::AIFloat3& pos) const;  // inside the base defence radius
	// Threat on the line from 'from' towards 'to' (out to maxReach) stays below allowedAt(spot) at each sampled spot
	template<typename AllowedAt>
	bool IsLineSafe(const springai::AIFloat3& from, const springai::AIFloat3& to, float maxReach,
			AllowedAt allowedAt) const;

	/*
	 * Merging and reinforcing
	 */
	ISquadTask* GetAttackMergeTask();
	// Straight line from our leader to the host's: threat below our allowance, both groups' strength near the host
	virtual bool IsSafeMergeLine(const ISquadTask* host) const override;
	void AbsorbWeakerTasks();
	CAttackTask* GetReinforceSquad();
	void Reinforce(const int frame, bool isReuse);
	// quota.attack_merge.idle_merge: an army without a target for a while reinforces the group GetReinforceSquad picks
	// (ranked above us: CMilitaryManager::IsRankedAbove) and merges with it; false: not yet idle long enough, no
	// such group, or none we could route to lately
	bool SeekIdleMerge(int frame, bool isReuse);
	// A fight here (isHunt false) or a target route accepted (true): the idle clock restarts; a hunt also ends an
	// army's walk to merge
	void SetHasTarget(int frame, bool isHunt);
	// Rally: where to walk when reinforcing the host; false: stand still
	bool PickRallyGoal(int frame, springai::AIFloat3& inOutGoal, bool& outIsToMeet) const;
	void ApplyReinforcePath(const CQueryPathSingle* query, const springai::AIFloat3& goal, bool isToMeet, float slack);
	bool FindFightEdge(int frame, const springai::AIFloat3& hostPos, springai::AIFloat3& outPos) const;
	// Routes to a group or a meeting point: capped, reinforce costs, kept with KeepRoute for GROUP_ROUTE_REUSE_FRAMES
	void RunGroupRoute(int frame, const springai::AIFloat3& goal, float range,
			std::function<void (const CQueryPathSingle*)>&& onComplete);
	bool ApplyGroupRoute(const CQueryPathSingle* query, ICoreUnit::Id routeId, const springai::AIFloat3& goal,
			float slack);

	// Rally host: while a significant joiner (>= 25% of our strength, within 60 s, closing in) is on its way, walk to a
	// meeting point with the significant ones and wait there. true: this decision is done
	struct SMeetTrack {  // progress of one joiner towards us: a joiner that stops closing in no longer holds us
		const CAttackTask* task;  // compared only, never dereferenced (may be gone)
		float bestSec;  // smallest gap (travel time at its speed) seen, in steps of MEET_PROGRESS_SEC
		int bestFrame;  // when bestSec was set
		int seenFrame;  // last decision that looked at it
		bool isSignificant;  // counted as significant last decision (it keeps counting down to a lower share)
	};
	// Engaged, or engaged within RALLY_FIGHT_MEMORY_FRAMES: as a rally host, joiners go to the fight edge
	bool IsRallyFighting(int frame) const;
	void ResetMeet();
	bool UpdateMeet(int frame, bool isReuse);
	void CollectSignificantJoiners(int frame, std::vector<CAttackTask*>& outSignificant);
	bool IsAtMeetPoint(const springai::AIFloat3& pos);  // updates isMeetArrived
	// How far behind its line's front row our leader may stand (the line's depth; 0 with no line). A route's end is
	// where the front row stands, but merging and arriving are judged at the leader
	float GetLineDepth() const;
	// Updates the joiner's track; true: it counts as significant (strong enough, not stalled)
	bool TrackJoiner(const CAttackTask* joiner, float gapSec, float ourStrength, int frame);
	// Where the host and its joiners arrive together, stepped back towards the host until safe and reachable
	springai::AIFloat3 FindMeetPoint(int frame, const std::vector<CAttackTask*>& joiners) const;
	bool IsMeetSpotOk(const springai::AIFloat3& pos, int frame, const std::vector<CAttackTask*>& joiners) const;

	/*
	 * Escape: checked on every update. True: this update is done (running, or cornered)
	 */
	bool UpdateEscape(int frame);
	// Fighting a target inside the base defence radius: stand and fight instead of fleeing
	bool IsHoldingGround(int frame) const;
	bool Flee(int frame);  // false: nowhere better to go, fight instead
	// Nearest stronger attack group we can stand beside, else the nearest safe defence point of a cluster we hold,
	// else the closest haven, else base
	springai::AIFloat3 FindFleeDest(int frame) const;
	void ApplyFleePath(const CQueryPathSingle* query);
	bool IsFleeQueryPending() const;
	// In danger but not fleeing (hold-ground): move to the quietest spot nearby by the
	// least-threat route; already there: attack the nearest enemy we can hit. Re-checked every second.
	void Cornered(int frame);
	void ApplyCorneredPath(const CQueryPathSingle* query);
	// Nearest enemy within radius that our leader can hit (category, air)
	CEnemyInfo* FindNearestHittable(int frame, float radius) const;
	// Line formation: also while the escape runs (the flee and the cornered move keep state ROAM and walk as a line)
	virtual bool IsFormationAllowed(int frame) const override { return true; }
	virtual bool IsFormationFlee(int frame) const override;
	virtual float GetFormationRange() const override { return fireRangeLand; }

	/*
	 * Targets and routes
	 */
	// One decision: reinforce a stronger group, or pick a target and engage it or route to it, else go to the
	// front. isReuse: a normal decision no unit asked to re-plan, which may keep a fresh route to the same goal
	void Decide(int frame, bool isReuse);
	// Picks the target and fills routeGoals (and screenedGoals)
	void FindTarget();
	// FindTarget, then engage the target if it is already within range; true: engaged.
	// isWalking: only the in-range result matters, so no FindTarget when no enemy is near
	bool EngageInRange(int frame, bool isWalking);

	/*
	 * Firing cells (quota.attack_static_cells; AttackCells.cpp): where the group shoots a building or a parked
	 * mobile from, beyond range. Every piece of cell state (goals, planned side, rejected sides, bans) records the
	 * anchor epoch it was built under and holds only while the target keeps that anchor (IsSameAnchor)
	 */
	// Where a target's cells are built around, and that spot's epoch. A building: its position, epoch 0. A parked
	// mobile (parked_sec; no faster than 1.25 x our slowest unit, not a nanoframe): the spot it stood still on in
	// LOS, and the frame it settled there. false: no cells (cells off, any other target, unknown type)
	bool GetCellAnchor(const CEnemyInfo* enemy, int frame, springai::AIFloat3& outPos, int& outEpoch) const;
	bool IsSameAnchor(const CEnemyInfo* enemy, int epoch, int frame) const;
	// Where range to a target is measured from: a radar-only parked mobile's anchor (its radar position wobbles by
	// the engine's radar error), else its position
	springai::AIFloat3 GetRangePos(const CEnemyInfo* enemy, int frame) const;
	// siege_bonus applies: a slow mobile type that outranges us on the ground, in view or just out of it
	bool IsSiegeTarget(const CEnemyInfo* enemy, int frame) const;
	// The target's cells around anchor, at most one per sector, judged with the pick's power and near-base
	// reference; nearOnly: the sectors facing us only (parked mobiles). false: every reachable point is above the
	// allowance (ban it). Else outCells; empty: route to it as a disc (undefended, melee group, nothing reachable,
	// or no line of sight)
	bool FillFiringCells(const CEnemyInfo* enemy, const springai::AIFloat3& anchor, float power, float ref,
			bool nearOnly, std::vector<SFiringCell>& outCells) const;
	// The route goals of a cell target beyond range: FillFiringCells minus its rejected sides, with the planned
	// cell (GetPlannedCell) in place of its sector's. false: skip the target (banned now, or every side
	// rejected). Else outCells; empty: route to it as a disc
	bool CollectFiringCells(const CEnemyInfo* enemy, const springai::AIFloat3& anchor, int epoch, float power,
			float ref, const springai::AIFloat3* planned, int frame, std::vector<SFiringCell>& outCells);
	// The cell the last accepted route to this target led to (lastRoute), while the target keeps that route's
	// anchor, its side isn't rejected and it is below the allowance for strength (the caller's: the keep allowance)
	bool GetPlannedCell(ICoreUnit::Id targetId, float strength, float refSqDist, springai::AIFloat3& outCell) const;
	bool IsCellBanned(ICoreUnit::Id id, int frame) const;
	void BanCell(const CEnemyInfo* enemy, int epoch, int frame);
	// Drops bans whose target died or lost its anchor, whose threat changed by more than 25%, or that started over
	// 60 s ago
	void UpdateCellBans(int frame);

	bool IsOnEnemyGround() const;  // influence at the leader < 0
	// One capped route to all candidate targets
	void RequestTargetPath(const std::vector<STargetGoal>& goals);
	void ApplyTargetPath(const CQueryPathMulti* query, const std::vector<STargetGoal>& goals);
	// The goal a route ended at (a copy; false: none alive): a firing cell whose wedge holds the end (its cell nearest
	// the end), else the first disc in range of the end, else (moved, corrected) the goal it ends nearest to, as a
	// disc (sector -1): only a route that ends in the wedge is a cell route
	bool FindReachedGoal(const CQueryPathMulti* query, const std::vector<STargetGoal>& goals,
			STargetGoal& outGoal) const;
	// Unsafe approach to g: drop its firing cell's side for REJECTED_TARGET_FRAMES, or the whole target when it is
	// a disc or the target has no other screened side left
	void RejectGoal(const STargetGoal& g, int frame);
	// sector -1: the whole target; a side (sector >= 0) only if it was rejected under this anchor epoch
	bool IsRejected(ICoreUnit::Id id, int sector, int epoch, int frame) const;
	// The ground we'd fight the target from, along this route: the line from the target to the route's end (or, for
	// a firing cell, from the route's end to the cell), and the route's last stretch as long as our range; every spot
	// below the allowance for 'strength'
	bool IsApproachSafe(const F3Vec& path, const springai::AIFloat3& targetPos, float strength, float refSqDist,
			const springai::AIFloat3* cell = nullptr) const;
	// No target: an army idle long enough walks to merge (SeekIdleMerge); else route to the front line, or hold nearby
	// when past it on enemy-held ground (or no front / no safe route). isReuse: a normal decision no unit asked to
	// re-plan, which may keep a fresh route to the same front
	void FallbackFrontPos(bool isReuse = false);
	void ApplyFrontPos(const CQueryPathMulti* query, const springai::AIFloat3& goal);
	// Threat cap for capped routes (target, front, reinforce): never above what the escape tolerates
	float GetPathThreatCap(const springai::AIFloat3& startPos, const F3Vec& goals) const;
	// Threat cost of a route: quota.attack_path_dynamic_cost, and on capped routes quota.attack_path_ignore_threat
	void SetRouteThreat(IPathQuery* query, AttackRoute route) const;
	void RunRoute(const std::shared_ptr<IPathQuery>& query, std::function<void (const IPathQuery*)>&& onComplete);
	void KeepRoute(ICoreUnit::Id targetId, const springai::AIFloat3& goal, const springai::AIFloat3* cell = nullptr,
			int sector = -1, int epoch = -1);
	// Still walking the last route to this same goal (moved less than one square, or goalSlack), planned under its
	// reuseFrames ago, with no unit in danger, nothing unsafe just ahead and (target) a safe approach still: keep it
	// (pPath) instead of planning it again
	bool ReuseRoute(ICoreUnit::Id targetId, const springai::AIFloat3& goal, int frame);

	/*
	 * Moving and holding
	 */
	// highestRange = max over all units of (weapon range, sight radius), rebuilt the same way on join, leave, merge;
	// also the group fire ranges (fireRangeLand, fireRangeWater)
	void UpdateHighestRange();
	float GetGroupRadius() const;  // quota.attack_group_radius
	// Straggler: farther from the leader than GetGroupRadius and the target out of its weapon range.
	// Gets no firing-spot order (keeps travelling)
	virtual bool IsOutOfReach(CCircuitUnit* unit, int frame) const override;
	bool IsFighting() const;  // ENGAGE with the target within some unit's weapon range: no merging away
	// Attack (firing spots), and walk stragglers (IsOutOfReach) to the leader if they have no path
	void Engage(int frame);
	// Nothing to go to: hold at the quietest reachable spot within 2x our range (never head home),
	// avoiding the spot we were last hit at while holding; retry next decision
	// Walk to the quietest spot nearby (FindQuietSpot) and hold there; isHolding. isWalk: along a planned path, so the
	// line formation takes the group there and it stands in line (a spot within 256 elmos, or no path: straight
	// moves); false: straight moves at once (stepping out of an attacker's range)
	void HoldPosition(bool isWalk = true);
	void ApplyHoldPath(const CQueryPathSingle* query);
	// Quietest reachable spot within 2x our range (24 samples + where we stand), avoiding the last hit area;
	// returns the threat there
	float FindQuietSpot(const springai::AIFloat3& pos, springai::AIFloat3& outPos);
	// Units more than 600 from pos walk there, the rest stand; sets isHolding
	void Regroup(int frame);  // the regroup's move (state REGROUP): in line where it can
	std::shared_ptr<PathInfo> MakeRegroupPath() const;
	void GatherAt(const springai::AIFloat3& pos);
	void MoveGroupTo(const springai::AIFloat3& pos);
	void MoveUnitTo(CCircuitUnit* unit, const springai::AIFloat3& pos, int frame);

	float minPower;
	bool IsArmy() const { return attackPower >= minPower; }  // at or above quota.attack

	// Reinforcing
	CAttackTask* reinforce;  // stronger group to join; validated through CMilitaryManager::GetTasks before use
	int reinforceFails;  // consecutive reinforce paths that came back empty
	int reinforceRetryFrame;  // after giving up, don't look for a group to reinforce before this frame
	int reinforceGiveUps;  // reinforce_fails give-ups in a row: each doubles the retry wait (up to 8x)
	int lastTargetFrame;  // last decision that fought or went for a target (SetHasTarget); SeekIdleMerge's delay
	bool isDecisionReuse;  // the last decision's isReuse, for the fallbacks of its target route
	int rallyRouteFrame;  // rally: our last route to the host came back empty, hold until this frame
	springai::AIFloat3 meetFailPos;  // rally: the host's meeting point our last route to came back empty

	// Rally host meeting its joiners (quota.attack_merge.rally)
	bool isMeetWalk;  // walking to the meeting point this decision: no regroup on the way
	bool isMeetArrived;  // at the meeting point: stand there until pushed away from it, or it moves
	bool isJoinerNear;  // a joiner is within the merge distance: absorb every 2 updates
	int engageFrame;  // last Engage (IsRallyFighting)
	springai::AIFloat3 meetPos;  // -RgtVector: not meeting
	springai::AIFloat3 meetArrivePos;  // the meeting point when we arrived
	std::vector<SMeetTrack> meetTracks;

	// Escape
	int fleeFrame;  // keep running until this frame
	int fleePathFrame;  // last escape path request
	int fleeQueryId;  // id of the escape path query still in flight, -1 = none
	springai::AIFloat3 fleeDest;  // where the current escape runs to (straight-move fallback when no path)
	bool isAtFleeDest;  // flee gave up: already within FLEE_ARRIVE_DIST of where it would run to
	ICoreUnit::Id fleeTargetId;  // target dropped by the last escape: still counts as current at the next pick (-1: none)
	bool isReplanForced;  // a forced re-plan (unsafe path ahead) was used since the last normal decision
	int corneredFrame;  // last cornered check
	springai::AIFloat3 corneredPos;  // spot we're moving to while cornered (-RgtVector: none)
	bool isCorneredHold;  // cornered while holding ground: keep holding (no flee) until the danger ends

	// Targets and routes
	std::vector<STargetGoal> routeGoals;  // FindTarget: goals of the next target route, nearest first (max 16)
	std::vector<STargetGoal> screenedGoals;  // FindTarget: all screened targets before stickiness trims them (retry goals)
	// (target, sector; -1: the whole target) dropped for an unsafe route approach -> skip until frame
	std::map<std::pair<ICoreUnit::Id, int>, SRejection> rejectedUntil;
	std::map<ICoreUnit::Id, SCellBan> cellBans;  // cell targets with no safe firing cell (beyond LOS range only)
	// Power-weighted median of the weapon ranges of the non-support units that can hit land / under-water targets
	float fireRangeLand;
	float fireRangeWater;
	int targetRouteTries;  // routes so far in this target decision (MAX_TARGET_ROUTES)
	float scaleRefSqDist;  // leader-to-base distance^2 when the current target was first picked: reference of the near-base scale
	ICoreUnit::Id anchorTargetId;  // target that scaleRefSqDist belongs to (-1: none)
	float pickSqDist;  // leader-to-base distance^2 at the last FindTarget (0: on enemy-held ground)
	SRoute lastRoute;  // dropped when a unit joins or leaves: a fresh route re-sends the path at the group's new speed

	// Holding
	bool isHolding;  // HoldPosition is in effect (until the next decision or escape)
	springai::AIFloat3 holdPos;  // the spot HoldPosition walks the group to along a path (-RgtVector: none)
	std::shared_ptr<PathInfo> regroupPath;  // the short path a regroup in line stands on (Regroup)
	int regroupFrame = -1;  // groupFrame of the regroup that made it
	int stepFrame;  // last step back after being hit while holding
	springai::AIFloat3 hitPos;  // where we were hit while holding (avoided by HoldPosition for 30 s)
	float hitRadius;
	int hitFrame;
};

} // namespace circuit

#endif // SRC_CIRCUIT_TASK_FIGHTER_ATTACKTASK_H_

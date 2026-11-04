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
	virtual void AssignTo(CCircuitUnit* unit) override;
	virtual void RemoveAssignee(CCircuitUnit* unit) override;
	virtual void Merge(ISquadTask* task) override;

	virtual void Start(CCircuitUnit* unit) override;
	virtual void Update() override;

	virtual void OnUnitIdle(CCircuitUnit* unit) override;
	virtual void OnUnitDamaged(CCircuitUnit* unit, CEnemyInfo* attacker) override;

private:
	// A target route goal: one candidate target
	struct STargetGoal {
		ICoreUnit::Id id;
		springai::AIFloat3 pos;
		float key;  // ranking: squared distance x near-base scale x target_dist_mod^2 (/ quota.contested.bonus^2)
	};
	// The last target or front route that was walked, so the next decision can keep it (ReuseRoute)
	struct SRoute {
		std::shared_ptr<PathInfo> path;  // nullptr: none
		springai::AIFloat3 goal;  // the target's position, or the front's first position
		ICoreUnit::Id targetId;  // -1: front route
		int frame;  // when it arrived
	};

	CMilitaryManager* GetMilitaryMgr() const;

	/*
	 * Group strength and the threat it tolerates
	 */
	// Like attackPower (def power incl. pwr_mod) but scaled by current health, times the task's powerMod
	float GetSafeThreat() const;
	virtual float GetEscapeSafeThreat() const override { return GetSafeThreat(); }
	float GetUnitPower(CCircuitUnit* unit) const;
	// Strength for picking a target and checking its approach: lower for a few seconds after an escape
	float GetPickStrength(int frame) const;
	// The one threat allowance used everywhere (picking, approach line, escape, path ahead, firing spots,
	// flee destination, route caps): strength x base_bonus(spot) / thr_mod.distance(spot) / scale(spot).
	// refSqDist: the near-base scale's reference (0: none); the 2-argument form uses the current fight's
	float GetAllowedThreatAt(const springai::AIFloat3& pos, float strength, float refSqDist) const;
	virtual float GetAllowedThreatAt(const springai::AIFloat3& pos, float strength) const override;
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
	void Reinforce(const int frame);
	void ApplyReinforcePath(const CQueryPathSingle* query);

	/*
	 * Escape: checked on every update. True: this update is done (running, or cornered)
	 */
	bool UpdateEscape(int frame);
	bool IsInDanger(int frame);  // fills 'endangered' and 'isPathAheadUnsafe'
	// Fighting a target inside the base defence radius: stand and fight instead of fleeing
	bool IsHoldingGround(int frame) const;
	void PullBackEndangered(int frame);
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
	CEnemyInfo* FindNearestHittable(int frame) const;

	/*
	 * Targets and routes
	 */
	// Picks the target and fills routeGoals (and screenedGoals)
	void FindTarget();
	bool IsOnEnemyGround() const;  // influence at the leader < 0
	// One capped route to all candidate targets
	void RequestTargetPath(const std::vector<STargetGoal>& goals);
	void ApplyTargetPath(const CQueryPathMulti* query, const std::vector<ICoreUnit::Id>& ids);
	// The ground we'd fight the target from, along this route: the line from the target to the route's end,
	// and the route's last stretch as long as our range; every spot below the allowance for 'strength'
	bool IsApproachSafe(const F3Vec& path, const springai::AIFloat3& targetPos, float strength, float refSqDist) const;
	// No target: route to the front line, or hold nearby when past it on enemy-held ground (or no front / no safe
	// route). isReuse: a normal decision no unit asked to re-plan, which may keep a fresh route to the same front
	void FallbackFrontPos(bool isReuse = false);
	void ApplyFrontPos(const CQueryPathMulti* query, const springai::AIFloat3& goal);
	// Threat cap for capped routes (target, front, reinforce): never above what the escape tolerates
	float GetPathThreatCap(const springai::AIFloat3& startPos, const F3Vec& goals, float strength) const;
	// Threat cost of a route: quota.attack_path_dynamic_cost, and on capped routes quota.attack_path_ignore_threat
	void SetRouteThreat(IPathQuery* query, AttackRoute route, float strength) const;
	void RunRoute(const std::shared_ptr<IPathQuery>& query, std::function<void (const IPathQuery*)>&& onComplete);
	void KeepRoute(ICoreUnit::Id targetId, const springai::AIFloat3& goal);
	// Still walking the last route to this same goal, planned under ROUTE_REUSE_FRAMES ago, with no unit in danger,
	// nothing unsafe just ahead and (target) a safe approach still: keep it (pPath) instead of planning it again
	bool ReuseRoute(ICoreUnit::Id targetId, const springai::AIFloat3& goal, int frame);

	/*
	 * Moving and holding
	 */
	// highestRange = max over all units of (weapon range, sight radius), rebuilt the same way on join, leave, merge
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
	void HoldPosition();
	// Quietest reachable spot within 2x our range (24 samples + where we stand), avoiding the last hit area;
	// returns the threat there
	float FindQuietSpot(const springai::AIFloat3& pos, springai::AIFloat3& outPos);
	void MoveGroupTo(const springai::AIFloat3& pos);
	void MoveUnitTo(CCircuitUnit* unit, const springai::AIFloat3& pos, int frame);

	float minPower;

	// Reinforcing
	CAttackTask* reinforce;  // stronger group to join; validated through CMilitaryManager::GetTasks before use
	int reinforceFails;  // consecutive reinforce paths that came back empty
	int reinforceRetryFrame;  // after giving up, don't look for a group to reinforce before this frame

	// Escape
	int fleeFrame;  // keep running until this frame
	int fleePathFrame;  // last escape path request
	int fleeQueryId;  // id of the escape path query still in flight, -1 = none
	springai::AIFloat3 fleeDest;  // where the current escape runs to (straight-move fallback when no path)
	bool isAtFleeDest;  // flee gave up: already within FLEE_ARRIVE_DIST of where it would run to
	ICoreUnit::Id fleeTargetId;  // target dropped by the last escape: still counts as current at the next pick (-1: none)
	int pullFrame;  // last pull-back of endangered stragglers
	std::vector<CCircuitUnit*> endangered;  // units standing in danger, from the last IsInDanger
	bool isPathAheadUnsafe;  // the leader's path just ahead is above the allowance, from the last IsInDanger
	bool isReplanForced;  // a forced re-plan (unsafe path ahead) was used since the last normal decision
	int corneredFrame;  // last cornered check
	springai::AIFloat3 corneredPos;  // spot we're moving to while cornered (-RgtVector: none)
	bool isCorneredHold;  // cornered while holding ground: keep holding (no flee) until the danger ends

	// Targets and routes
	std::vector<STargetGoal> routeGoals;  // FindTarget: goals of the next target route, nearest first (max 16)
	std::vector<STargetGoal> screenedGoals;  // FindTarget: all screened targets before stickiness trims them (retry goals)
	std::map<ICoreUnit::Id, int> rejectedUntil;  // targets dropped for an unsafe route approach -> skip until frame
	int targetRouteTries;  // routes so far in this target decision (MAX_TARGET_ROUTES)
	float scaleRefSqDist;  // leader-to-base distance^2 when the current target was first picked: reference of the near-base scale
	ICoreUnit::Id anchorTargetId;  // target that scaleRefSqDist belongs to (-1: none)
	float pickSqDist;  // leader-to-base distance^2 at the last FindTarget (0: on enemy-held ground)
	SRoute lastRoute;

	// Holding
	bool isHolding;  // HoldPosition is in effect (until the next decision or escape)
	int stepFrame;  // last step back after being hit while holding
	springai::AIFloat3 hitPos;  // where we were hit while holding (avoided by HoldPosition for 30 s)
	float hitRadius;
	int hitFrame;
};

} // namespace circuit

#endif // SRC_CIRCUIT_TASK_FIGHTER_ATTACKTASK_H_

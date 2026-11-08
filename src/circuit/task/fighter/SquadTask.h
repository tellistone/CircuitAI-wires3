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
class CTerrainManager;

class ISquadTask: public IFighterTask {
protected:
	ISquadTask(ITaskManager* mgr, FightType type, float powerMod);
public:
	virtual ~ISquadTask();

	virtual void AssignTo(CCircuitUnit* unit) override;
	virtual void RemoveAssignee(CCircuitUnit* unit) override;
	// Abort/done moves units to the idle task without RemoveAssignee: give scooting units their move state back first
	virtual void Stop(bool done) override;
	// A destroyed unit needs no move state back: drop its scoot without sending it orders
	virtual void OnUnitDestroyed(CCircuitUnit* unit, CEnemyInfo* attacker) override;

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
	// Can this squad take count more units by merging (CheckMergeTask asks the host)? Default: always
	virtual bool IsRoomFor(size_t count) const { return true; }
	// IsMustRegroup ignores this unit. Default: it is scooting (stepping out of reach while it reloads)
	virtual bool IsRegroupExempt(CCircuitUnit* unit) const { return IsScooting(unit); }
	void ReleaseStuck(std::vector<CCircuitUnit*>& stuck);
	void ActivePath(float speed = NO_SPEED_LIMIT);
	NSMicroPather::TestFunc GetHitTest() const;
	// Line of sight over terrain between two path-map squares (sector max elevation), from aimLift above the start to
	// targetLift above the end
	static NSMicroPather::TestFunc MakeHitTest(CTerrainManager* terrainMgr, float aimLift, float targetLift = SQUARE_SIZE);

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
	 * Shoot and scoot: while engaged, a unit whose weapon is still reloading and that stands in ground threat steps
	 * straight away from the target to just outside the target's ground range + its scoot margin (or a margin further
	 * back, if already past that), and fights again once reloaded enough, or sooner if walking back into range would
	 * take longer than the reload it has left. No scoot when there isn't time to get out and back before the weapon is
	 * ready. The rest of the squad fights on. Scoots exist only while the squad is engaged: they end when it stops.
	 * Call UpdateScoots every update; Attack, the pull-back and the regroup check skip scooting units
	 */
	static constexpr float DEFAULT_SCOOT_MARGIN = 100.f;  // reload_scoot_margin when unset (tasks may override)
	virtual bool IsScooting(CCircuitUnit* unit) const override { return scoots.find(unit) != scoots.end(); }
	// Elmos beyond the target's range a scooting unit steps to; < 0 = this unit never scoots.
	// Default: reload_scoot units (not mines or aircraft): reload_scoot_margin, else DEFAULT_SCOOT_MARGIN
	virtual float GetScootMargin(CCircuitUnit* unit) const;
	// Should this unit (GetScootMargin >= 0) be out of reach now? Default: main weapon below reload_scoot reloaded
	virtual bool IsUnitReloading(CCircuitUnit* unit, int frame) const;
	// A scooting unit is reloaded enough. Default: drop its old target so the next Attack re-orders it, and (engaged)
	// ask for an update now, or (not engaged) Start it on the squad's path
	virtual void OnScootEnd(CCircuitUnit* unit, int frame, bool isEngaged);
	// Every update: scoot reloading units while engaged, and end the scoot of units reloaded enough, or whose walk back
	// into range takes as long as the reload they have left. Not engaged: every scoot ends.
	// A unit outside all ground threat doesn't start a scoot (it fights on). Re-checked once a second while scooting
	void UpdateScoots(int frame);

private:
	struct SScoot {
		int frame;  // last scoot check
		springai::AIFloat3 dest;  // last spot it was sent to
	};
	using Scoots = std::map<CCircuitUnit*, SScoot>;
	// Starts (scoot == nullptr) or renews a scoot
	void Scoot(CCircuitUnit* unit, SScoot* scoot, int frame, float margin);
	// Straight away from the target, just outside its ground range + margin
	springai::AIFloat3 GetScootSpot(CCircuitUnit* unit, const springai::AIFloat3& uPos, float margin) const;
	// Does walking dist take as long as the unit's weapon has left to reload?
	bool IsWalkLongerThanReload(CCircuitUnit* unit, int frame, float dist) const;
	// While scooting a unit is on hold position: the engine's idle auto-attack (weapon range + 200 elmos on
	// maneuver) would otherwise walk it straight back in after its scoot move ends or it stops
	void SetScootHold(CCircuitUnit* unit, bool isHold);
	// Erases the scoot, gives the unit its move state back, then OnScootEnd
	void EndScoot(Scoots::iterator it, int frame, bool isEngaged);
	Scoots scoots;  // reloading units stepping out of reach (on hold position)

protected:
	/*
	 * Threat-capped paths and the squad flee (anti-air, anti-heavy)
	 */
	static constexpr float FALLBACK_PATH_RANGE = DEFAULT_SLACK * 4;  // goal tolerance of safety, base, commander and flee paths
	static const int FLEE_REPLAN_FRAMES = FRAMES_PER_SEC * 2;  // re-plan the flee this often while the danger stays
public:  // also used by the commander flee (CBuilderManager::UpdateCommFlee)
	// Is an unidentified radar blip at p high enough above the ground (or water surface) to be a plane?
	static bool IsHighBlip(CMap* map, const springai::AIFloat3& p);
protected:
	/*
	 * Target picks: runners not worth chasing (attack, raid)
	 */
	static constexpr float CHASE_SPEED_MOD = 0.8f;  // x our top speed: a runner this fast is not chased
	static constexpr float OUTRANGE_CHASE_SPEED_MOD = 0.75f;  // x our top speed: nor this fast, if it outranges us
	static_assert(CHASE_SPEED_MOD >= OUTRANGE_CHASE_SPEED_MOD, "IsRunningAway: sqSpeed >= sqOutrangeSpeed");
	static constexpr float OUTRANGE_MOD = 1.1f;  // outranges us: ground range over ours x this (not by a few elmos)
	// Is the enemy moving away from pos at sqSpeed or more (elmos per frame, squared), or at sqOutrangeSpeed or more
	// when its ground/water weapons (IsGroundDangerDef: not anti-air only) outrange ourRange x OUTRANGE_MOD?
	// sqSpeed must be at least sqOutrangeSpeed. Unknown type (radar blip): not outranging
	static bool IsRunningAway(CEnemyInfo* enemy, const springai::AIFloat3& pos, float sqSpeed, float sqOutrangeSpeed,
			float ourRange);
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
	// The squad flee (anti-air, anti-heavy), every update before anything else: run from the nearest enemy within its
	// range + margin of a squad unit (FindFleeDanger), unless escorts next to the leader cover every enemy in reach
	// (IsFleeCovered; allyThreat < 0 = never covered). While fleeing the reach is range + endMargin (>= margin), so an
	// enemy at the edge can't switch the flee on and off. Flee plans the run, again every FLEE_REPLAN_FRAMES while
	// the danger stays; OnFleeEnd runs once it's gone. margin < 0 = off. True while fleeing
	bool UpdateFlee(float margin, float endMargin, bool isGunship, float allyThreat, float allyRadius,
			CCircuitDef::RoleT ownRole);
	virtual void Flee(const springai::AIFloat3& dangerPos) {}
	virtual void OnFleeEnd() {}
private:
	bool isFleeing = false;
	int fleeReplanFrame = 0;  // UpdateFlee re-plans the flee from this frame
protected:

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

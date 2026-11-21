/*
 * ArtilleryTask.h
 *
 *  Created on: Jan 6, 2016
 *      Author: rlcevg
 */

#ifndef SRC_CIRCUIT_TASK_FIGHTER_ARTILLERYTASK_H_
#define SRC_CIRCUIT_TASK_FIGHTER_ARTILLERYTASK_H_

#include "task/fighter/FighterTask.h"
#include "module/MilitaryManager.h"

#include <memory>
#include <vector>

namespace circuit {

/*
 * Artillery: shoot known enemy buildings from threat-free (and covered) spots, walk to such spots, otherwise stay behind
 * the army; flee from approaching enemies. Anti-heavy mode (isAntiHeavy, CMilitaryManager::IsAhArty: Lance, Merlin, Phantom, Racketeer):
 * the same, with the heavies and commanders anti-heavy units hunt (CCircuitDef::IsQuarry) as targets instead
 * of buildings (FindHeavyTarget), and covered spots with threat are fine too (IsThreatOk), see IsQuarryIgnored;
 * a heavy in range is shot from the attack task's firing spots (IFighterTask::Attack, IsFireSpotAllowed), so the unit
 * keeps to ~90% of its range and backs off from a heavy that closes in; after each shot it steps back (UpdateScoot,
 * antiheavy.artillery_scoot). With no heavy to shoot it walks to a covered firing spot by one, else holds at a covered
 * spot in range of an enemy near our army, the front (or walks to one), else follows the army (WalkAntiHeavy)
 */
class CArtilleryTask: public IFighterTask {
public:
	CArtilleryTask(ITaskManager* mgr, bool isAntiHeavy = false);
	virtual ~CArtilleryTask();

	virtual bool CanAssignTo(CCircuitUnit* unit) const override;
	virtual void AssignTo(CCircuitUnit* unit) override;
	virtual void RemoveAssignee(CCircuitUnit* unit) override;

	virtual void Start(CCircuitUnit* unit) override;
	virtual void Update() override;

	virtual void OnUnitIdle(CCircuitUnit* unit) override;  // zk artillery: a new task; anti-heavy mode: keeps it

	bool IsAntiHeavy() const { return isAntiHeavy; }
	CMilitaryManager::AhNote GetAhActivity() const;  // for the ANTIHEAVY log line

private:
	const CMilitaryManager::SArtilleryCfg& GetCfg() const;  // artillery block of behaviour.json
	float GetMaxPathThreat(CCircuitUnit* unit) const;  // max_path_threat x own power, or no limit
	float GetFleeMargin(CCircuitUnit* unit) const;  // artillery_flee_margin of this unit type, else the block's; < 0 = off

	// Shooting and walking to firing spots
	void Execute(CCircuitUnit* unit, bool isUpdating);
	CEnemyInfo* FindTarget(CCircuitUnit* unit, const springai::AIFloat3& pos);  // in range, else fills enemyPositions
	CEnemyInfo* FindHeavyTarget(CCircuitUnit* unit, const springai::AIFloat3& pos);  // FindTarget, anti-heavy mode
	// A pop-up turret (CCircuitDef::IsPopup) that is shut (armoured); false for anything else. The AI can't read an
	// enemy's armoured state: guessed from its on/off switch, or from our units within its range
	bool IsPopupShut(CEnemyInfo* enemy, int frame) const;
	bool IsPopupTargetNear(CEnemyInfo* enemy, int frame) const;  // an ally unit it can hit within its range + margin
	void StopAttack(CCircuitUnit* unit);  // cancel our attack order, if one is live
	// What a firing route is for: the task's targets (enemyPositions: buildings, anti-heavy: heavies), or (anti-heavy)
	// the front (frontGoals)
	enum Goal: int { TARGET = 0, FRONT = 1 };
	// Path query to the nearest of goals (firing cells, or targets with cover off), under the route cap
	void RunFireQuery(CCircuitUnit* unit, float goalRange, const F3Vec& goals, Goal goal, bool isUpdating);
	void ApplyTargetPath(const CQueryPathMulti* query, bool isUpdating, Goal goal);
	// Walking to a firing spot: keep the route between full re-plans (every 6 s) while it's still good. goals: what
	// the route was planned for (routeGoal)
	bool IsFireRouteValid(CCircuitUnit* unit, float range, const F3Vec& goals);
	// FillCoveredSpots' rules for one spot with this much threat there
	bool IsFireSpotValid(CCircuitUnit* unit, const springai::AIFloat3& spot, float threat) const;
	// ally_threat: fills firePositions (covered, safe, reachable, and not where the flee would fire) in range of goals,
	// returns goal range. Anti-heavy: not within CROWD_RADIUS of another anti-heavy (CollectCrowd)
	float FillCoveredSpots(CCircuitUnit* unit, float range, const F3Vec& goals);
	static constexpr float CROWD_RADIUS = 96.f;
	bool CollectCrowd(CCircuitUnit* unit);  // crowd: the other artillery-mode anti-heavies; false: none

	// Nothing to shoot or walk to
	void FallbackNoTarget(CCircuitUnit* unit, bool isUpdating);  // no known building
	void FallbackNoSpot(CCircuitUnit* unit, bool isUpdating);  // no acceptable firing spot
	// no_scout (anti-heavy: always): hold if safe and covered (anti-heavy: and near the army), else FallbackSafePos
	void FallbackStay(CCircuitUnit* unit, bool isUpdating);
	void HoldHere(CCircuitUnit* unit);  // stop walking (incl. queued engine waypoints) and wait
	void ApplyPathOrHold(CCircuitUnit* unit, const std::shared_ptr<PathInfo>& pPath);  // walk, or hold if already there
	void FillSafeSpots(CCircuitUnit* unit);  // safePositions: FillSafePos pulled back by follow_dist
	void FallbackSafePos(CCircuitUnit* unit, bool isUpdating);  // move to the nearest safety spot
	void ApplySafePos(const CQueryPathMulti* query, bool isUpdating);
	void FallbackBasePos(CCircuitUnit* unit, bool isUpdating);
	void ApplyBasePos(const CQueryPathSingle* query, bool isUpdating);
	// Old fallbacks (no_scout false)
	void FallbackScout(CCircuitUnit* unit, bool isUpdating);
	void ApplyScoutPath(const CQueryPathSingle* query);
	void Fallback(CCircuitUnit* unit, bool proceed);

	// Early flee (artillery_flee_margin)
	struct SDanger {  // flee zone: an enemy that can hit us, and how far that sets off the flee
		springai::AIFloat3 pos;
		float sqReach;  // (weapon range + margin)^2
		float threat;
	};
	bool CheckFlee(CCircuitUnit* unit);  // true while fleeing
	// Fills dangers: flee zones (IsGroundDanger, reach = weapon range + margin) that touch the x/z rectangle;
	// not the heavies IsQuarryIgnored passes
	void CollectDangers(CCircuitUnit* unit, float margin, const springai::AIFloat3& minPos,
			const springai::AIFloat3& maxPos) const;
	// The nearest flee zone in dangers that reaches pos, nullptr if none; outThreat: all of them that reach it
	static const SDanger* FindDanger(const springai::AIFloat3& pos, float& outThreat);
	void Flee(CCircuitUnit* unit, const springai::AIFloat3& dangerPos);
	void ApplyFleePath(const CQueryPathSingle* query);

	float GetWalkRange(CCircuitUnit* unit) const;  // weapon range for firing spots, at least a threat square
	// Anti-heavy mode with no heavy to shoot: keep a route still good; else a covered firing spot by a heavy
	// (enemyPositions), else at the front (FillFrontGoals), held where it stands if it is one; else behind the army
	// (FallbackStay)
	void WalkAntiHeavy(CCircuitUnit* unit, float range, bool isUpdating);
	// Hold here if this is a firing spot (IsFireSpotValid) in range of one of goals, else walk to the nearest covered
	// one (after a search that found nothing, none of that goal for 6 s). false: none
	bool SeekFireSpot(CCircuitUnit* unit, float range, const F3Vec& goals, Goal goal, bool isUpdating);
	// frontGoals: the enemies of frontEnemies within range + ally_radius of where the army is (safePositions), those in
	// LOS if any, else radar dots. Cover off (ally_threat < 0): none
	const F3Vec& FillFrontGoals(float range);

	// Anti-heavy mode. A heavy we hunt is no danger (flee, firing spots) and its threat doesn't make where we stand
	// unsafe to shoot from while our weapon is ready and we out-range it or it is within our range. While we reload it
	// counts like any other enemy, so the unit steps back after a shot
	bool IsQuarryIgnored(CCircuitUnit* unit, CEnemyInfo* enemy) const;
	// Threat at spot without that of the heavies FindHeavyTarget found IsQuarryIgnored (ignoredQuarries)
	float GetNetThreatAt(CCircuitUnit* unit, const springai::AIFloat3& spot) const;
	// Anti-heavy firing spots (IFighterTask::Attack's FindFireSpot): IsFireSpotValid with GetNetThreatAt
	virtual bool IsFireSpotAllowed(const springai::AIFloat3& p) const override;
	virtual bool IsScooting(CCircuitUnit* unit) const override { return isScooting; }
	// A spot with this much threat is fine: threat-free, or (anti-heavy mode) our cover there, net of the threat, still
	// reaches ally_threat, as the flee judges it. isWalk: a spot to walk to, also no more threat than the route cap
	bool IsThreatOk(CCircuitUnit* unit, const springai::AIFloat3& spot, float threat, bool isWalk) const;
	// Shoot and scoot (antiheavy.artillery_scoot): after each shot (once its burst is over) the unit steps that far back,
	// away from the target, threatened or not, and holds there; it turns back in time to be in place when its weapon
	// is ready, then decides as usual. No scoot when there isn't time to get there and back, or every spot back is
	// hotter than here. Every update: true while a shot's burst or a scoot is on (no Execute then)
	bool UpdateScoot(CCircuitUnit* unit, int frame);
	void StartScoot(CCircuitUnit* unit, int frame);

	// Cover (ally_threat, ally_radius)
	struct SEscort {
		springai::AIFloat3 pos;
		float power;
		bool isMobile;  // only mobile escorts cover a firing spot; static defences only count against the flee
	};
	bool IsEscort(CCircuitUnit* unit, CAllyUnit* ally, bool isFlee) const;  // isFlee: static defences count too
	void CollectEscorts(CCircuitUnit* unit, bool isFlee);  // fills escorts
	// Escort power near spot, net of enemyThreat, reaches ally_threat
	bool HasCover(CCircuitUnit* unit, const springai::AIFloat3& spot, float enemyThreat = 0.f, bool isFlee = false) const;

	std::shared_ptr<PathInfo> firePath;  // route to the current firing spot
	Goal routeGoal = TARGET;  // what firePath is for
	int searchFrame[2] = {0, 0};  // anti-heavy: no firing-spot search for that Goal before this frame
	int replanFrame = 0;  // re-plan that route in full after this frame
	bool isFleeing = false;
	int fleeFrame = 0;  // re-plan the flee after this frame
	const bool isAntiHeavy;  // heavies instead of buildings
	bool isAttacking = false;  // our attack order is live (StopAttack)
	springai::AIFloat3 attackPos;  // where the target of our attack order was (scoot direction)
	int shotFrame = -1;  // a shot's first frame, until its scoot starts; -1 = none
	bool wasReady = false;  // the weapon was ready on the last update (a shot = ready, then not)
	bool isScooting = false;
	springai::AIFloat3 scootFrom;  // where the scooting unit fired from
	std::shared_ptr<PathInfo> safePath;  // route to the current safety spot (FallbackStay walks it to the end)

	// Scratch shared by all artillery tasks (micro-opt, as IFighterTask::enemyPositions)
	static F3Vec safePositions;
	static F3Vec firePositions;  // covered firing cells
	static std::vector<CEnemyInfo*> candidates;  // FindHeavyTarget: heavies in range
	struct SFrontEnemy {
		springai::AIFloat3 pos;
		bool isLos;
	};
	static std::vector<SFrontEnemy> frontEnemies;  // FindHeavyTarget: every enemy it can shoot, aircraft aside
	static F3Vec frontGoals;  // FillFrontGoals
	static F3Vec crowd;  // CollectCrowd
	static std::vector<CEnemyInfo*> ignoredQuarries;  // FindHeavyTarget: heavies IsQuarryIgnored, for its Attack
	static std::vector<SEscort> escorts;
	static std::vector<SDanger> dangers;
	static std::vector<float> coverGrid;  // FillCoveredSpots: mobile escort power per cell of the escorts' area
	static std::vector<float> fleeCoverGrid;  // FillCoveredSpots: the same with static defences, against the flee
	static std::vector<char> cellAdded;  // FillCoveredSpots: cell already checked
};

} // namespace circuit

#endif // SRC_CIRCUIT_TASK_FIGHTER_ARTILLERYTASK_H_

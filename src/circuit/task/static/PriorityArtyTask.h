/*
 * PriorityArtyTask.h
 *
 *  Long-range static guns (Big Bertha, Disco Rave Party, Starlight) aimed by a fixed priority list (behaviour.json
 *  "target_tiers", STargetPriority) in place of CSuperTask's enemy-cluster picker.
 */

#ifndef SRC_CIRCUIT_TASK_STATIC_PRIORITYARTYTASK_H_
#define SRC_CIRCUIT_TASK_STATIC_PRIORITYARTYTASK_H_

#include "task/fighter/FighterTask.h"
#include "task/static/ArtyMath.h"
#include "unit/CoreUnit.h"

#include <deque>
#include <unordered_map>
#include <vector>

namespace springai {
	class Weapon;
}

namespace circuit {

struct STargetPriority;

/*
 * The gun runs at hold fire ("fire_state": "hold"), so only our orders aim it, and keeps one target until it's
 * lost or a clearly better one shows up:
 *   - lost: dead, hidden, out of range, a mobile unit out of radar and LOS or submerged, a friendly unit near the
 *     aim point (checked every half second), or failed (below). Re-pick at once
 *   - every 10 s: switch for a higher tier than the one it was picked at; in the same tier, for a finished unit
 *     over a nanoframe; with the same tier and finish, for one that ranks higher and is worth 3x as much
 * Ranking: tier; finished; in LOS; value (cost, or splash value in "splash" tiers; "turn_half_value" weighs it by
 * turning); turning time. A mobile unit more than 10 s of turning away (or moving more than "mobile_move" during
 * the shell's flight) ranks after all others.
 * Shots are counted on every weapon that can target (a gun of several fires them in turn). A target fails after 5
 * shells landed (fired at it in LOS or radar, given their flight time) since this gun last damaged anything, or after
 * max(5, 4 x shots to kill) shots at it out of both (shared by the ally team's guns, cleared when it's seen again:
 * until then it's skipped), or with no shot at all for turning + 1 reload + "shot_grace". A blocked arc is a failure
 * too, unless "high_arc" lets a building take the high arc over it. Failed targets are skipped for 60 s x
 * 2^(failures - 1); a target's failures reset once it takes damage from this gun.
 * An attack order on a static unit never times out (CCommandAI), so it's sent again only once the gun goes idle.
 *
 * "beam" (Starlight): the satellite's beam is on whenever it's undocked and burns wherever it points, also while it
 * turns and after a Stop (which freezes it). The task tracks where it points (turning at "turn_rate", docking and
 * undocking when the gun is disabled), and:
 *   - picks only a target whose way there (both axes turning at once) crosses no friendly unit, nor one at the target
 *     or a beam blocked by terrain. From near straight down (the beam lands on the Starlight, which it can't hurt) it
 *     turns the heading first, then crawls out along one ray, which may cross up to 2000 metal of small friendly
 *     units. A target dropped for friends is skipped for 10 s, with no failure, and the gun looks again at once
 *   - friendly units at the beam now: re-pick without testing the first 300 elmos of the way, else move the beam
 *     sideways the short way, to the side with less friendly cost
 *   - progress: once the beam is on a target, it gets its health / "dps" + 3 s. Still there after that, it's a failure
 *     in LOS or radar, else one blind shot (it's skipped until it's seen again)
 *   - while it turns to its target, it switches only to one no further away, unless that's in the top tier
 */
class CPriorityArtyTask: public IFighterTask {
public:
	CPriorityArtyTask(ITaskManager* mgr);
	virtual ~CPriorityArtyTask();

	virtual bool CanAssignTo(CCircuitUnit* unit) const override;
	virtual void RemoveAssignee(CCircuitUnit* unit) override;

	virtual void Start(CCircuitUnit* unit) override;
	virtual void Update() override;

	virtual void OnUnitIdle(CCircuitUnit* unit) override;

	// An enemy took damage from this task's gun (EnemyDamaged, in our LOS or radar)
	void OnEnemyDamaged(CEnemyInfo* enemy);

	// The main weapon's shell (outShell keeps its defaults for what the weapon doesn't set); false: no main weapon
	static bool ReadShell(const CCircuitDef* cdef, ballistics::SShell& outShell);

private:
	struct SCandidate {
		CEnemyInfo* enemy;
		arty::SRank rank;
		float heading;  // degrees
		float pitch;  // degrees, launch angle
		float flightSec;  // on the arc it's fired on
		bool isMobile;
		bool isHigh;  // high arc
	};
	struct SFailure {
		int count = 0;
		int skipUntil = 0;  // frame
	};
	struct SBuilding {  // for splash value
		ICoreUnit::Id id;
		springai::AIFloat3 pos;
		float cost;
	};
	struct SWeapon {
		springai::Weapon* weapon;  // owner
		int reloadFrames;
		int lastReloadFrame;
	};
	struct SSpot {  // a beam path's spot
		float x, z;
		float radius;  // friendly guard
	};

	void ReadWeapons(CCircuitUnit* unit);
	int CountShots();  // shots since the last call, over all weapons
	bool IsDisabled(CCircuitUnit* unit, int frame) const;  // lowpower, disarmed or stunned
	bool IsLost(int frame);  // the current target; adds a failure where it failed
	// The filters a candidate must pass, but its failure record and the friendly-unit and arc checks
	bool IsTargetable(CEnemyInfo* enemy, bool& outIsMobile) const;
	bool IsInRange(const springai::AIFloat3& enemyPos) const;
	bool IsFriendNear(const springai::AIFloat3& aimPos, int frame) const;  // friendly unit in the aim box
	bool IsArcClear(const springai::AIFloat3& aimPos, bool isHigh) const;
	int GetBlindBudget(CEnemyInfo* enemy) const;  // shots out of LOS and radar
	bool IsBlindSpent(CEnemyInfo* enemy) const;
	// isHigh: on the high arc (the low one's numbers otherwise)
	bool MakeCandidate(CEnemyInfo* enemy, bool isHigh, SCandidate& outCand) const;
	float GetSplashValue(CEnemyInfo* enemy) const;
	void AddFailure(CEnemyInfo* enemy, int frame);
	// Pick the best target; with a current one, switch only for one the switching rule allows. skipLen: beam paths
	// aren't tested for their first this many elmos. false: no target
	bool Pick(CCircuitUnit* unit, int frame, float skipLen = 0.f);
	void SetNewTarget(const SCandidate& cand, int frame);
	void Order(CCircuitUnit* unit);
	void Drop(CCircuitUnit* unit, int frame);  // no target: stop shelling the old one

	// Beam ("beam")
	void UpdateBeam(CCircuitUnit* unit, int frame, int elapsed, bool isDisabled);
	bool StepAim(int frame, int elapsed, bool isDisabled);  // the tracked satellite; true: just undocked
	bool IsAimDone() const;  // pointing where it's ordered to
	bool IsFromDown() const;  // near straight down (or aligning there): the heading turns first, then out along a ray
	float GetSatY() const;  // the satellite's height now
	springai::AIFloat3 GetSpot(float h, float p) const;  // where the beam lands at heading h, pitch p
	// Friendly cost in the beam's guard box at a spot, with the beam at tan(pitch) there; lead: grown this much
	float GetFriendCost(const springai::AIFloat3& spot, float tanP, float lead, int frame) const;
	// Friendly cost on the way from where the beam points now to (toHeading, toPitch) at a target y high, without the
	// first skipLen elmos. From near straight down: along the ray at toHeading
	float GetPathCost(float toHeading, float toPitch, float y, float skipLen, int frame);
	bool IsBeamLost(int frame, float skipLen);  // the current target; adds a failure where it failed
	// A candidate's friendly, terrain and path checks; may add a failure or a skip. outTested: a path was tested
	bool IsBeamClear(const SCandidate& c, int frame, float skipLen, bool& outTested);
	void Dodge(CCircuitUnit* unit, int frame);  // friends at the beam, no target clear: move it sideways
	void SoftSkip(CEnemyInfo* enemy, int frame);  // dropped for friends: skipped for a while, no failure

	const STargetPriority* tp;
	springai::AIFloat3 aimFrom;  // muzzle: the gun's base + 50
	float range;  // on flat ground
	float speed;  // elmo/s
	float gravity;  // elmo/s^2
	float heightMod;
	float aoe;  // the largest of its weapons' (friendly guard)
	float splashRadius;
	float shellDamage;  // per shot, averaged over the weapons (CCircuitDef::GetShotDamage)
	int reloadFrames;  // the shortest
	bool isAntiWater;  // can hit a submerged target
	std::vector<SWeapon> weapons;

	// Target state
	int pickTier;  // tier when picked (sticky for the switching rule)
	bool isTargetMobile;
	bool isTargetHigh;  // fired on the high arc
	int trajectory;  // last set: 0 low, 1 high, -1 not set by us
	int flightFrames;  // to the target
	int checkFrame;  // next 10 s check
	int noShotFrame;  // a shot is due by then
	int lastUpdateFrame;
	int dryShots;  // shells landed since this gun last damaged anything
	std::deque<int> landFrames;  // when shells fired at the target in LOS or radar land, in firing order
	float heading;  // last bearing ordered, degrees (beam: where it points now, tracked)
	float pitch;  // launch angle (beam: from straight down)
	bool isReorder;  // went idle: order again

	// Beam state
	springai::Weapon* aimer;  // the weapon that can target (in weapons)
	float satHeight;  // the satellite's height above the base
	float aimHeading;  // where it turns to
	float aimPitch;
	springai::AIFloat3 aimPoint;  // ... that ground point; -RgtVector: none (frozen by a Stop)
	int readyFrame;  // undocked by then; INT_MAX: disabled
	int arriveFrame;  // the beam reached the target; -1: not yet
	int dwellFrames;  // ... it should be dead this long after
	float crawlCost;  // friendly cost the way to the target may cross (out from the base)
	bool isAligning;  // turning the heading from straight down before the crawl out
	bool isDodging;  // moving off friendly units
	springai::AIFloat3 exitSpot;  // ... where it found them; -RgtVector: none
	int satId;  // the satellite unit; -1: not known yet
	std::vector<SSpot> spots;  // per path test

	std::unordered_map<ICoreUnit::Id, SFailure> failures;
	std::vector<SBuilding> buildings;  // known enemy buildings near our range, per pick
	std::vector<SCandidate> candidates;  // per pick
};

} // namespace circuit

#endif // SRC_CIRCUIT_TASK_STATIC_PRIORITYARTYTASK_H_

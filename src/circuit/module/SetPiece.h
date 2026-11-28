/*
 * SetPiece.h
 *
 *  Created on: Nov 28, 2026
 *      Author: Claude
 */

#ifndef SRC_CIRCUIT_MODULE_SETPIECE_H_
#define SRC_CIRCUIT_MODULE_SETPIECE_H_

#include "unit/CoreUnit.h"
#include "util/Defines.h"

#include "AIFloat3.h"

#include <unordered_map>
#include <vector>

namespace circuit {

class CCircuitAI;
class CCircuitUnit;
class CEnemyInfo;
class IFighterTask;
class ISquadTask;

/*
 * Set piece attack (quota.set_piece): one planned attack, by every group in reach at once, on an enemy force on our
 * ground that no group would take alone. Updated once a second, after the protect. It never orders units: it latches
 * groups, and their tasks ask it what to do (IsCommitted, GetWait, GetGoal, IsTargetMember, GetBoost). One at a time.
 * 1. Scan, while none runs: armed mobile ground enemies seen in the last 10 s and finished turrets, in choke-map
 *    regions we held in the last held_sec (settled ours, or contested with our buildings in it). Blobs
 *    (CEnemyManager::MakeBlobs, at most 8) within 1600 elmos of each other are one target. Skipped: one sharing half
 *    its threat with a target on cooldown, or half its members with an answerable raid incident (quota.raid_defence),
 *    or one some group's own pick would take (its allowance, ally cover included). T: its threat with the count
 *    bonus, plus the enemy turrets outside it whose reach covers its centre.
 * 2. Groups: attack and raid groups that can reach it (gunships if they hit ground), anti-heavy squads, and our
 *    commanders (the commander attack's gates, CBuilderManager::IsSetPieceFree). Each needs its leader within reach
 *    elmos of the target's edge (commanders comm_reach), to be calm (no fight, or one with the target), not
 *    protecting or gathering, to hit half of T, and to get to its staging or hold point within gather_sec (straight
 *    line at its pace). P: their strength with one count bonus for all their units (anti-heavy squads add none).
 *    Start: P > start_ratio x T, and the strongest group's allowance at P / start_ratio passes the centre.
 * 3. Staged, for a parked target (its members' mean velocity under 200 elmos in 5 s; turrets are parked) whose reach
 *    + 200 is within stage_dist, outside the base defence radius: each group walks at its own strength to its staging
 *    point (a safe spot just outside the target's reach + 200 on its side; where it stands if nearer) and waits there.
 *    Go once the staged groups (within 200 elmos of their points) pass the start test alone; abandoned if not
 *    gather_sec after the start. At the go each staged group is released at arrive - its walk (to contact, at its
 *    pace), arrive = now + the longest walk + 1 s, gunships 3 s later; the rest are reinforcements and go at once.
 *    Contact with any group, or a parked target moving 300 elmos for 2 s, releases everyone. Any other target: all go
 *    in at once from the start.
 * 4. Fight: released groups take only members (past the 3-group cap, claims, the own-ground rule and the force
 *    balance) and what they are in contact with. Within R of the target's edge (R + 250 once present; R = max(1000,
 *    its range + 250)) they judge with the strength present (GetBoost: the released calm groups there and itself,
 *    one count bonus; gunships count gunships only); with nothing to take they walk to the fight's safe edge.
 *    Anti-heavy squads take its quarries first, else its chaff, and get no boost.
 * 5. Revert: before its first contact, a group whose pick is an armed enemy outside the target leaves (unless the
 *    outside enemies in its reach are under pass_share of its strength: it fights them in passing), and so does one
 *    the protect takes. It may rejoin once, before the go.
 * 6. End: P / T under abandon_ratio; no member seen for 10 s; no group left; nothing fought or approached for
 *    stall_sec (after the go, or from the start straight in); the target 1500 elmos from its start and off our
 *    ground. Its groups go back to their own decisions, its commanders to their jobs (CommEnd::SET_PIECE), and its
 *    members get a cooldown: cooldown_sec, doubled after each abandon in a row (up to 4x), reset by a kill.
 * Members are kept by id: an armed enemy within 800 elmos of the centre joins, one unseen for 10 s drops out.
 * log_only: scan, start line and label, no commit. SETPIECE log lines, and a minute summary.
 */
class CSetPiece {
public:
	struct SConfig {
		bool isEnabled = false;
		bool isLogOnly = false;
		bool isLabel = true;  // the map label at its target
		float reach = 3000.f;  // a group's leader within this of the target's edge
		float commReach = 1000.f;  // ... a commander's
		float startRatio = 1.1f;
		float abandonRatio = 0.75f;
		float stageDist = 1000.f;  // staged only when the target's range + 200 is within this
		float gatherSec = 45.f;  // a group gets to its point within this; a staged set piece goes within this
		int stallFrames = FRAMES_PER_SEC * 15;
		float passShare = 0.25f;
		int heldFrames = FRAMES_PER_SEC * 60;  // our ground: regions held within this
		int cooldownFrames = FRAMES_PER_SEC * 30;
	};

	CSetPiece(CCircuitAI* circuit, const SConfig& cfg);
	~CSetPiece();

	bool IsEnabled() const { return cfg.isEnabled; }
	bool IsLive() const { return cfg.isEnabled && !cfg.isLogOnly; }  // groups act on it (not log_only)
	void Update();  // once a second
	void ClearLabel();  // also on the AI's release

	// In the running set piece. The usual path (every pick and threat check): one compare
	bool IsCommitted(const IFighterTask* task) const {
		return !commits.empty() && (commits.find(task) != commits.end());
	}
	// Where a committed group waits for its release: its staging point; invalid once it may go, or not committed
	springai::AIFloat3 GetWait(const IFighterTask* task) const;
	// The target's centre for a committed group; invalid otherwise
	springai::AIFloat3 GetGoal(const IFighterTask* task) const;
	// One of the target's members, for a committed group that may go
	bool IsTargetMember(const IFighterTask* task, const CEnemyInfo* enemy) const;
	bool IsMember(ICoreUnit::Id id) const { return members.find(id) != members.end(); }
	// What a released group's own strength is multiplied by where it judges the fight: within R of the target's edge
	// (R + 250 once present), the strength present with itself / its own; elsewhere, or any other task, 1
	float GetBoost(const IFighterTask* task, const springai::AIFloat3& pos) const {
		return isGo ? GetGoBoost(task, pos) : 1.f;
	}
	// A committed commander attack's cover (CBuilderManager's flee): the released groups present and itself, as raw
	// strength; 0 otherwise
	float GetPresentStrength(const IFighterTask* task) const;
	// pos stands in a region we held within held_sec (choke map ready)
	bool IsHeldAt(const springai::AIFloat3& pos) const;
	// Farther than comm_reach from the target's edge: a committed commander's leash
	bool IsBeyondCommReach(const springai::AIFloat3& pos) const;
	void OnTaskEnd(const IFighterTask* task);  // CMilitaryManager::DequeueTask: no pointer outlives its task

private:
	enum class Phase: char {OFF, STAGE, GO};
	enum class EndKind: char {KILL, ABANDON, OTHER};  // the cooldown's: a kill resets it, an abandon doubles the next
	struct SMember {
		float threat;
		float range;  // its ground weapon range
		bool isTurret;
	};
	using Members = std::unordered_map<ICoreUnit::Id, SMember>;
	// The geometry and size of a set of members (MakeShape)
	struct SShape {
		springai::AIFloat3 centre = -RgtVector;  // threat-weighted
		float spread = 0.f;  // its farthest member from the centre: the target's edge
		float range = 0.f;  // its longest weapon range
		float reach = 0.f;  // from the centre: its farthest member's distance + range
		float threat = 0.f;  // its members' summed
		float count = 0.f;  // their count weight
		float danger = 0.f;  // T: threat x count bonus + the turrets outside it that reach its centre
		bool isParked = true;
		float GetRadius() const;  // R, from the edge
		float GetEdgeDist(const springai::AIFloat3& pos) const;
	};
	struct SCommit {
		ISquadTask* task;
		springai::AIFloat3 stage = -RgtVector;  // its staging point; invalid: straight in
		int releaseFrame = -1;  // it may go from then; -1: at the go
		int arriveFrame = -1;  // its first contact with a member
		float lastDist = 0.f;  // its leader's distance to the centre at the last update
		float boost = 1.f;  // GetBoost's value this second
		bool isStaged = false;
		bool isPresent = false;
		bool isReleased = false;
		bool isTimed = false;  // released at its time at the go (staged then)
		bool isRejoined = false;  // reverted once and back: no more rejoins
		bool isGunship = false;
		bool isAntiHeavy = false;
	};
	struct SCandidate {  // a group (or a commander) that may join
		ISquadTask* task;  // nullptr: a commander not yet in a commander attack
		CCircuitUnit* lead;
		float base;  // its strength x powerMod, no count bonus (GetJointSafeThreat's measure)
		size_t count;
		springai::AIFloat3 stage;
		float walkSec;  // to its staging point (staged), else to contact
		float goSec;  // from its staging point to contact (staged), else walkSec
		bool isGunship;
		bool isAntiHeavy;
	};
	struct SCooldown {
		std::unordered_map<ICoreUnit::Id, float> threats;  // its members'
		int until;
	};

	void UpdateHeld(int frame);
	// An armed enemy we may count (seen within 10 s, or a finished turret): its threat and range. false: not one
	bool GetMemberInfo(const CEnemyInfo* enemy, int frame, SMember& out) const;
	SShape MakeShape(const Members& list) const;
	void Scan(int frame);
	bool IsSkipped(const Members& list, float threat) const;  // on cooldown, or a raid incident's
	// The groups that can join a target, and P. false: none, or one would take it alone
	bool CollectCandidates(const Members& list, const SShape& s, bool isStagedPick, int frame,
			std::vector<SCandidate>& out, float& outP) const;
	// task nullptr: lead is a commander
	bool MakeCandidate(ISquadTask* task, CCircuitUnit* lead, const Members& list, const SShape& s, bool isStagedPick,
			int frame, SCandidate& out) const;
	// A safe spot for it just outside the target's reach, on its side (where it stands if nearer); invalid: none
	springai::AIFloat3 FindStage(const ISquadTask* task, CCircuitUnit* lead, const SShape& s, int frame) const;
	float GetHitShare(const CCircuitUnit* lead, const Members& list) const;  // of the members' threat it can hit
	// P over start_ratio x T, and the strongest group's allowance at P / start_ratio passes the centre
	bool IsStrongEnough(float power, const SShape& s, const ISquadTask* strongest) const;
	void Start(const Members& list, const SShape& s, bool isStagedPick, std::vector<SCandidate>& groups, float power,
			int frame);
	void Commit(const SCandidate& c, int frame);
	void UpdateTarget(int frame);  // members, shape; ends when all are gone
	void UpdateGroups(int frame);
	void Go(const char* reason, bool isAll, int frame);
	void Revert(const IFighterTask* task, const char* reason, int frame);
	void End(const char* reason, EndKind kind, int frame);
	// A member reaches its leader, or it reaches a member (with the target engaged)
	bool IsInContact(const ISquadTask* task, int frame) const;
	// Its pick is an armed enemy outside the target and those around it are pass_share of its strength or more
	bool IsPulledAway(const ISquadTask* task, int frame) const;
	float GetGoBoost(const IFighterTask* task, const springai::AIFloat3& pos) const;
	void SetLabel(const char* text);
	void LogMinute();

	CCircuitAI* circuit;
	SConfig cfg;

	Phase phase = Phase::OFF;
	bool isGo = false;  // in a go and live: GetBoost's quick path
	bool isStaged = false;
	bool isNotReadyLogged = false;
	int startFrame = -1;
	int goFrame = -1;
	int arriveFrame = -1;  // planned (at the start, then at the go)
	int stallFrame = -1;  // last fight or approach
	int movedFrame = -1;  // a parked target away from its start since then
	int firstArrive = -1;  // the timed groups' first contacts: the arrival spread
	int lastArrive = -1;
	int timedArrivals = 0;
	float startPower = 0.f;
	int kills = 0;
	Members members;
	std::unordered_map<ICoreUnit::Id, float> seenThreats;  // every member it had: the cooldown's
	SShape shape;
	springai::AIFloat3 startCentre = -RgtVector;
	std::unordered_map<const IFighterTask*, SCommit> commits;
	std::unordered_map<const IFighterTask*, ISquadTask*> rejoins;  // reverted once before the go: may come back
	std::vector<SCooldown> cooldowns;
	int abandons = 0;  // in a row, at most 2: the cooldown's shift
	std::vector<int> heldFrames;  // per choke-map region: last frame we held it
	springai::AIFloat3 labelPos = -RgtVector;
	int nextLogFrame;
	struct SStats {
		int starts = 0;
		int staged = 0;
		int goes = 0;
		int kills = 0;  // ends with a kill
		int abandons = 0;
		int others = 0;
		int arrivals = 0;
		int spreads = 0;  // staged ends with 2+ arrivals
		float spreadSum = 0.f;
		float spreadMax = 0.f;
	} stats;
};

} // namespace circuit

#endif // SRC_CIRCUIT_MODULE_SETPIECE_H_

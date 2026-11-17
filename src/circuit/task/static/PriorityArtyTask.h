/*
 * PriorityArtyTask.h
 *
 *  Long-range static artillery (Big Bertha, Disco Rave Party) aimed by a fixed priority list (behaviour.json
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
 *     aim point (checked every second), or failed (below). Re-pick at once
 *   - every 10 s: switch for a higher tier than the one it was picked at; in the same tier, for a finished unit
 *     over a nanoframe; with the same tier and finish, for one that ranks higher and is worth 3x as much
 * Ranking: tier; finished; in LOS; value (cost, or splash value in "splash" tiers; "turn_half_value" weighs it by
 * turning); turning time. A mobile unit more than 10 s of turning away (or moving more than "mobile_move" during
 * the shell's flight) ranks after all others.
 * Shots are counted on every weapon (a gun of several fires them in turn). A target fails after 5 shells landed
 * (fired at it in LOS or radar, given their flight time) since this gun last damaged anything, or after max(5,
 * 4 x shots to kill) shots at it out of both (shared by the ally team's guns, cleared when it's seen again: until
 * then it's skipped), or with no shot at all for turning + 1 reload + "shot_grace". A blocked arc is a failure too,
 * unless "high_arc" lets a building take the high arc over it. Failed targets are skipped for 60 s x
 * 2^(failures - 1); a target's failures reset once it takes damage from this gun.
 * An attack order on a static unit never times out (CCommandAI), so it's sent again only once the gun goes idle
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
	// Pick the best target; with a current one, switch only for one the switching rule allows
	void Pick(CCircuitUnit* unit, int frame);
	void SetNewTarget(const SCandidate& cand, int frame);
	void Order(CCircuitUnit* unit);
	void Drop(CCircuitUnit* unit, int frame);  // no target: stop shelling the old one

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
	float heading;  // last bearing ordered, degrees
	float pitch;
	bool isReorder;  // went idle: order again

	std::unordered_map<ICoreUnit::Id, SFailure> failures;
	std::vector<SBuilding> buildings;  // known enemy buildings near our range, per pick
	std::vector<SCandidate> candidates;  // per pick
};

} // namespace circuit

#endif // SRC_CIRCUIT_TASK_STATIC_PRIORITYARTYTASK_H_

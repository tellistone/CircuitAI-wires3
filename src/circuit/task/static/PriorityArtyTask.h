/*
 * PriorityArtyTask.h
 *
 *  Long-range static artillery (Big Bertha) aimed by a fixed priority list (behaviour.json "target_tiers",
 *  STargetPriority) in place of CSuperTask's enemy-cluster picker.
 */

#ifndef SRC_CIRCUIT_TASK_STATIC_PRIORITYARTYTASK_H_
#define SRC_CIRCUIT_TASK_STATIC_PRIORITYARTYTASK_H_

#include "task/fighter/FighterTask.h"
#include "task/static/ArtyMath.h"
#include "unit/CoreUnit.h"

#include <unordered_map>
#include <vector>

namespace circuit {

struct STargetPriority;

/*
 * The gun runs at hold fire ("fire_state": "hold"), so only our orders aim it, and keeps one target until it's
 * lost or a clearly better one shows up:
 *   - lost: dead, hidden, out of range, a mobile unit out of radar and LOS, a friendly unit near the aim point,
 *     or failed (below). Re-pick at once
 *   - every 10 s: switch for a higher tier than the one it was picked at; in the same tier, for a finished unit
 *     over a nanoframe; with the same tier and finish, for one that ranks higher and is worth 3x as much
 * Ranking: tier; finished; in LOS; value (cost, or splash value in "splash" tiers); turning time. A mobile unit
 * more than 10 s of turning away ranks after all others.
 * A target fails after 5 shots in a row (it in LOS or radar) that damaged nothing, or after max(5, 4 x shots to
 * kill) shots at it out of both (shared by our guns, cleared when it's seen again: until then it's skipped), or
 * with no shot at all for turning + 1 reload + 10 s. A blocked arc is a failure too. Failed targets are skipped
 * for 60 s x 2^(failures - 1); a target's failures reset once it takes damage from this gun
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
		arty::SRank rank;  // isFar: mobile and more than 10 s of turning away
		float heading;  // degrees
		float pitch;  // degrees, launch angle
		bool isMobile;
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

	void ReadWeapon(CCircuitUnit* unit);
	bool IsDisabled(CCircuitUnit* unit, int frame) const;  // lowpower, disarmed or stunned
	// The filters a candidate must pass, but its failure record and the friendly-unit and arc checks
	bool IsTargetable(CEnemyInfo* enemy, bool& outIsMobile) const;
	bool IsInRange(const springai::AIFloat3& enemyPos) const;
	bool IsFriendNear(const springai::AIFloat3& aimPos, int frame) const;  // friendly unit in the aim box
	bool IsArcClear(const springai::AIFloat3& aimPos) const;
	int GetBlindBudget(CEnemyInfo* enemy) const;  // shots out of LOS and radar
	bool IsBlindSpent(CEnemyInfo* enemy) const;
	bool MakeCandidate(CEnemyInfo* enemy, SCandidate& outCand) const;
	float GetSplashValue(CEnemyInfo* enemy) const;
	void AddFailure(CEnemyInfo* enemy, int frame);
	// Pick the best target; with a current one, switch only for one the switching rule allows
	void Pick(CCircuitUnit* unit, int frame);
	void Order(CCircuitUnit* unit, int frame);
	void Drop(CCircuitUnit* unit, int frame);  // no target: stop shelling the old one

	const STargetPriority* tp;
	springai::AIFloat3 aimFrom;  // muzzle: the gun's base + 50
	float range;  // on flat ground
	float speed;  // elmo/s
	float gravity;  // elmo/s^2
	float heightMod;
	float aoe;
	float shellDamage;
	int reloadFrames;

	// Target state
	int pickTier;  // tier when picked (sticky for the switching rule)
	bool isTargetMobile;
	int checkFrame;  // next 10 s check
	int orderFrame;  // last order
	int noShotFrame;  // a shot is due by then
	int lastUpdateFrame;
	int lastReloadFrame;
	int dryShots;  // shots since this gun last damaged anything
	float heading;  // last bearing ordered, degrees
	float pitch;
	bool isReorder;  // went idle: order again

	std::unordered_map<ICoreUnit::Id, SFailure> failures;
	std::vector<SBuilding> buildings;  // known enemy buildings near our range, per pick
	std::vector<SCandidate> candidates;  // per pick
};

} // namespace circuit

#endif // SRC_CIRCUIT_TASK_STATIC_PRIORITYARTYTASK_H_

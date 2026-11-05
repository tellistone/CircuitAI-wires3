/*
 * AntiHeavyTask.h
 *
 *  Created on: Jun 30, 2016
 *      Author: rlcevg
 */

#ifndef SRC_CIRCUIT_TASK_FIGHTER_ANTIHEAVYTASK_H_
#define SRC_CIRCUIT_TASK_FIGHTER_ANTIHEAVYTASK_H_

#include "task/fighter/SquadTask.h"
#include "module/MilitaryManager.h"

namespace circuit {

class CAntiHeavyTask: public ISquadTask {
public:
	CAntiHeavyTask(ITaskManager* mgr, float powerMod);
	virtual ~CAntiHeavyTask();

	virtual bool CanAssignTo(CCircuitUnit* unit) const override;
	virtual void AssignTo(CCircuitUnit* unit) override;
	virtual void RemoveAssignee(CCircuitUnit* unit) override;

	virtual void Start(CCircuitUnit* unit) override;
	virtual void Update() override;

	virtual void OnUnitIdle(CCircuitUnit* unit) override;
	virtual void OnUnitDamaged(CCircuitUnit* unit, CEnemyInfo* attacker) override;

protected:
	virtual float GetPathThreatMod() const override;  // antiheavy.path_threat; < 0 = no cap (old behaviour, and aircraft)
	// The squad flee doesn't run from the heavies it hunts (IsQuarry); reloading units step away from the target
	// on their own (Scoot)
	virtual bool IsFleeIgnored(CEnemyInfo* enemy) const override;
	virtual bool IsRegroupExempt(CCircuitUnit* unit) const override { return scoots.find(unit) != scoots.end(); }

private:
	const CMilitaryManager::SAntiHeavyCfg& GetCfg() const;  // antiheavy block of behaviour.json
	// The leader type's antiheavy_flee_margin, else the block's; -1 (off) for mines and aircraft
	float GetFleeMargin() const;
	// Flee off: the leader's long-reload weapon (5 s or more) isn't ready (old blind pull-back to the attack groups)
	bool IsReloading(int frame) const;
	// Shoot and scoot (flee on): this unit's long-reload weapon isn't ready. Not mines or units without a weapon
	bool IsUnitReloading(CCircuitUnit* unit, int frame) const;
	// Every update while engaged: a reloading unit within the target's ground range + flee margin moves straight
	// away to just outside it (re-sent once a second while still in reach), one out of reach stops there; a unit
	// whose weapon is ready again attacks at once
	void UpdateScoots(int frame);
	void Scoot(CCircuitUnit* unit, int frame);
	bool AttackTarget(CCircuitUnit* unit, int frame, float& minePower);  // the engage orders for one unit
	// A heavy or commander the squad can shoot (role, target category, anti-static, air)
	bool IsQuarry(CEnemyInfo* enemy) const;

	// Stand-off distance from a heavy: just outside its own threat ring on our threat layer, but never less than
	// the old engage range (LOS / jump range). Routes end there; within it the squad attacks
	float GetStandOff(CEnemyInfo* enemy, float engageRange) const;
	// The point on the line from ePos towards pos, dist from ePos (pos itself if it's nearer)
	static springai::AIFloat3 GetPointTowards(const springai::AIFloat3& ePos, const springai::AIFloat3& pos, float dist);
	// Flee zones of the enemies the flee doesn't ignore, for IsSpotSafe (FindTarget scratch)
	void CollectDangers(float margin);
	// No flee zone reaches spot, or (flee_ally_threat) escorts there cover the enemies that reach it
	bool IsSpotSafe(const springai::AIFloat3& spot) const;
	void StopAll();  // a flee ended: stop walking its path
	// Stand-off point for a heavy at ePos: the first walkable point from standOff outwards (one threat cell at a
	// time, a few cells at most) towards pos whose threat is under the route cap; false if none
	bool ProbeStandOff(const springai::AIFloat3& ePos, const springai::AIFloat3& pos, float standOff,
			springai::AIFloat3& outPos) const;

	bool FindTarget();
	void ApplyTargetPath(const CQueryPathMulti* query);
	void FallbackTargetEmpty();
	void FallbackAttackSafe();
	void ApplyAttackSafe(const CQueryPathMulti* query);
	void FallbackStaticSafe();
	void ApplyStaticSafe(const CQueryPathMulti* query);
	void FallbackBasePos(bool isCapped);
	void ApplyBasePos(const CQueryPathSingle* query, bool isCapped);
	void FallbackCommPos();

	bool CheckFlee();  // antiheavy.flee_margin; true while fleeing
	void Flee(const springai::AIFloat3& dangerPos);
	void ApplyFleePath(const CQueryPathSingle* query);

	bool isFleeing = false;
	int fleeFrame = 0;  // re-plan the flee after this frame
	std::map<CCircuitUnit*, int> scoots;  // reloading units stepping out of reach: frame of their last scoot order

	struct SDanger {
		springai::AIFloat3 pos;
		float sqReach;  // (weapon range + flee margin)^2
		float threat;
	};
	static std::vector<SDanger> dangers;  // CollectDangers scratch, shared by all anti-heavy tasks
};

} // namespace circuit

#endif // SRC_CIRCUIT_TASK_FIGHTER_ANTIHEAVYTASK_H_

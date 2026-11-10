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
	// Cloaked units route like the rest (threat map, path_threat cap) unless their type has the cloak_route attribute
	virtual bool IsCloakRoute(CCircuitUnit* unit) const override;
	virtual float GetPathThreatMod() const override;  // antiheavy.path_threat; < 0 = no cap (old behaviour, and aircraft)
	// The squad flee doesn't run from the heavies it hunts (IsQuarry); reloading units step away from the target
	// on their own (Scoot)
	virtual bool IsFleeIgnored(CEnemyInfo* enemy) const override;
	// antiheavy_max_group of the squad's unit type: a merge may not take it over that size
	virtual bool IsRoomFor(size_t count) const override;
	// Shoot and scoot (ISquadTask). reload_scoot units use their own threshold and margin (default: the flee
	// margin); others scoot while their long-reload weapon (5 s or more) isn't ready, when the flee is on.
	// Never mines or aircraft
	virtual float GetScootMargin(CCircuitUnit* unit) const override;
	virtual bool IsUnitReloading(CCircuitUnit* unit, int frame) const override;
	virtual void OnScootEnd(CCircuitUnit* unit, int frame, bool isEngaged) override;  // attacks again at once

private:
	const CMilitaryManager::SAntiHeavyCfg& GetCfg() const;  // antiheavy block of behaviour.json
	// The leader type's antiheavy_flee_margin, else the block's; -1 (off) for mines and aircraft
	float GetFleeMargin() const;
	// Flee off: the leader's long-reload weapon (5 s or more) isn't ready (old blind pull-back to the attack groups)
	bool IsReloading(int frame) const;
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

	bool CheckFlee();  // antiheavy.flee_margin (UpdateFlee); true while fleeing
	virtual void FleeFrom(const springai::AIFloat3& dangerPos) override;
	virtual void OnFleeEnd() override;  // stop walking the flee path
	void ApplyFleePath(const CQueryPathSingle* query);

	struct SDanger {
		springai::AIFloat3 pos;
		float sqReach;  // (weapon range + flee margin)^2
		float threat;
	};
	static std::vector<SDanger> dangers;  // CollectDangers scratch, shared by all anti-heavy tasks
};

} // namespace circuit

#endif // SRC_CIRCUIT_TASK_FIGHTER_ANTIHEAVYTASK_H_

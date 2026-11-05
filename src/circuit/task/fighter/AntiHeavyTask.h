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
	// The flee doesn't run from the heavies it hunts (IsQuarry) while the weapon is ready; while reloading it
	// runs from everything in reach (shoot and scoot)
	virtual bool IsFleeIgnored(CEnemyInfo* enemy) const override;

private:
	const CMilitaryManager::SAntiHeavyCfg& GetCfg() const;  // antiheavy block of behaviour.json
	// The leader type's antiheavy_flee_margin, else the block's; -1 (off) for mines and aircraft
	float GetFleeMargin() const;
	// A long-reload weapon (5 s or more) that isn't ready on any squad unit: the squad has just fired
	bool IsReloading(int frame) const;
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
	mutable int reloadFrame = -1;  // IsReloading cache
	mutable bool isReloading = false;

	struct SDanger {
		springai::AIFloat3 pos;
		float sqReach;  // (weapon range + flee margin)^2
		float threat;
	};
	static std::vector<SDanger> dangers;  // CollectDangers scratch, shared by all anti-heavy tasks
};

} // namespace circuit

#endif // SRC_CIRCUIT_TASK_FIGHTER_ANTIHEAVYTASK_H_

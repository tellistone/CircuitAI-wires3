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
	virtual float GetPathThreatMod() const override;  // antiheavy.path_threat; < 0 = no cap (old behaviour)
	// The flee doesn't run from the heavies it hunts (IsQuarry) while the weapon is ready; while reloading it
	// runs from everything in reach (shoot and scoot)
	virtual bool IsFleeIgnored(CEnemyInfo* enemy) const override;

private:
	const CMilitaryManager::SAntiHeavyCfg& GetCfg() const;  // antiheavy block of behaviour.json
	// The leader type's antiheavy_flee_margin, else the block's; -1 (off) for mines and aircraft
	float GetFleeMargin() const;
	// A long-reload weapon (5 s or more) that isn't ready: the squad has just fired
	bool IsReloading(int frame) const;
	// A heavy or commander the squad can shoot (role, target category, anti-static, air)
	bool IsQuarry(CEnemyInfo* enemy) const;

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
	// FindTarget: the smallest threat of the far heavies in enemyPositions. Their own cells carry it, so the route
	// to them may cross that much on top of the squad's allowance
	float quarryThreat = 0.f;
};

} // namespace circuit

#endif // SRC_CIRCUIT_TASK_FIGHTER_ANTIHEAVYTASK_H_

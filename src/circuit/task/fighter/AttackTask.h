/*
 * AttackTask.h
 *
 *  Created on: Jan 28, 2015
 *      Author: rlcevg
 */

#ifndef SRC_CIRCUIT_TASK_FIGHTER_ATTACKTASK_H_
#define SRC_CIRCUIT_TASK_FIGHTER_ATTACKTASK_H_

#include "task/fighter/SquadTask.h"

namespace circuit {

class CAttackTask: public ISquadTask {
public:
	CAttackTask(ITaskManager* mgr, float minPower, float powerMod);
	virtual ~CAttackTask();

	virtual bool CanAssignTo(CCircuitUnit* unit) const override;
	// CanAssignTo without the 1000-elmo distance limit: speed, movement type, not a worse mover
	bool IsCompatible(CCircuitUnit* unit) const;
	// Walk to this army and merge into it instead of hunting targets alone
	void SetReinforce(CAttackTask* squad) { reinforce = squad; }
	virtual void AssignTo(CCircuitUnit* unit) override;
	virtual void RemoveAssignee(CCircuitUnit* unit) override;

	virtual void Start(CCircuitUnit* unit) override;
	virtual void Update() override;

	virtual void OnUnitIdle(CCircuitUnit* unit) override;

private:
	ISquadTask* GetAttackMergeTask();
	CAttackTask* GetReinforceSquad();
	void Reinforce(const int frame);
	void ApplyReinforcePath(const CQueryPathSingle* query);
	void ConvertToGather();
	void AbsorbWeakerTasks();
	void FindTarget();
	void ApplyTargetPath(const CQueryPathSingle* query);
	void FallbackFrontPos();
	void ApplyFrontPos(const CQueryPathMulti* query);
	void FallbackBasePos();
	void ApplyBasePos(const CQueryPathSingle* query);
	void Fallback();

	float minPower;
	CAttackTask* reinforce;  // army to join; validated through CMilitaryManager::GetTasks before use
};

} // namespace circuit

#endif // SRC_CIRCUIT_TASK_FIGHTER_ATTACKTASK_H_

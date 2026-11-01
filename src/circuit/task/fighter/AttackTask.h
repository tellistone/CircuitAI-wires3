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
	// Walk to this stronger group and merge into it instead of hunting targets alone (while a safe route exists)
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
	void AbsorbWeakerTasks();
	void FindTarget();
	// Highest threat on the line from the target towards us, out to our range (where we'd shoot from),
	// stays below the allowance at each sampled spot
	bool IsApproachSafe(const springai::AIFloat3& ePos, const springai::AIFloat3& pos, float safeThreat) const;
	// Escape, as in CRaidTask: group strength from current health, units standing in danger, flee / pull back
	float GetSafeThreat() const;
	float GetUnitPower(CCircuitUnit* unit) const;
	float GetSafeThreatAt(const springai::AIFloat3& pos, float safeThreat) const;
	bool IsInDanger(int frame, bool& isPathUnsafe);  // fills 'endangered'
	void PullBackEndangered(int frame);
	bool Flee(int frame);  // false: nowhere better to go, fight instead
	void ApplyFleePath(const CQueryPathSingle* query);
	void ApplyTargetPath(const CQueryPathSingle* query);
	void FallbackFrontPos();
	void ApplyFrontPos(const CQueryPathMulti* query);
	void FallbackBasePos();
	void ApplyBasePos(const CQueryPathSingle* query);
	void Fallback();

	float minPower;
	CAttackTask* reinforce;  // stronger group to join; validated through CMilitaryManager::GetTasks before use
	int reinforceFails;  // consecutive reinforce paths that came back empty
	int reinforceRetryFrame;  // after giving up, don't look for a group to reinforce before this frame
	int fleeFrame;  // keep running until this frame
	int fleePathFrame;  // last escape path request
	int pullFrame;  // last pull-back of endangered stragglers
	std::vector<CCircuitUnit*> endangered;  // units standing in danger, from the last IsInDanger
};

} // namespace circuit

#endif // SRC_CIRCUIT_TASK_FIGHTER_ATTACKTASK_H_

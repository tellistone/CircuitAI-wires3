/*
 * RaidTask.h
 *
 *  Created on: Jan 6, 2016
 *      Author: rlcevg
 */

#ifndef SRC_CIRCUIT_TASK_FIGHTER_RAIDTASK_H_
#define SRC_CIRCUIT_TASK_FIGHTER_RAIDTASK_H_

#include "task/fighter/SquadTask.h"

namespace circuit {

class CRaidTask: public ISquadTask {
public:
	CRaidTask(ITaskManager* mgr, float maxPower, float powerMod);
	virtual ~CRaidTask();

	virtual bool CanAssignTo(CCircuitUnit* unit) const override;
	virtual void AssignTo(CCircuitUnit* unit) override;
	virtual void RemoveAssignee(CCircuitUnit* unit) override;

	virtual void Start(CCircuitUnit* unit) override;
	virtual void Update() override;

	virtual void OnUnitIdle(CCircuitUnit* unit) override;

private:
	bool FindTarget();
	void ApplyTargetPath(const CQueryPathMulti* query);
	void FallbackRaid();
	void ApplyRaidPath(const CQueryPathSingle* query);

	// Escape: current group strength vs threat where the units are and just ahead on the path
	float GetSafeThreat() const;
	float GetSafeThreatAt(const springai::AIFloat3& pos, float safeThreat) const;
	bool IsInDanger(int frame, bool& isPathUnsafe) const;
	bool Flee(int frame);  // false: nowhere better to go (already home), fight instead
	void ApplyFleePath(const CQueryPathSingle* query, const springai::AIFloat3& dest);

	float maxPower;
	int fleeFrame;  // keep running until this frame
	int fleePathFrame;  // last escape path request
};

} // namespace circuit

#endif // SRC_CIRCUIT_TASK_FIGHTER_RAIDTASK_H_

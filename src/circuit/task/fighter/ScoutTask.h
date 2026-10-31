/*
 * ScoutTask.h
 *
 *  Created on: Jan 28, 2015
 *      Author: rlcevg
 */

#ifndef SRC_CIRCUIT_TASK_FIGHTER_SCOUTTASK_H_
#define SRC_CIRCUIT_TASK_FIGHTER_SCOUTTASK_H_

#include "task/fighter/FighterTask.h"
#include "unit/CoreUnit.h"

#include <unordered_map>

namespace circuit {

class CScoutTask: public IFighterTask {
public:
	CScoutTask(ITaskManager* mgr, float powerMod);
	virtual ~CScoutTask();

	virtual bool CanAssignTo(CCircuitUnit* unit) const override;
	virtual void AssignTo(CCircuitUnit* unit) override;
	virtual void RemoveAssignee(CCircuitUnit* unit) override;

	virtual void Start(CCircuitUnit* unit) override;
	virtual void Update() override;

	virtual void OnUnitIdle(CCircuitUnit* unit) override;

	virtual void OnRearmStart(CCircuitUnit* unit) override;

private:
	void Execute(CCircuitUnit* unit, bool isUpdating);
	bool FindTarget(CCircuitUnit* unit, const springai::AIFloat3& pos);
	void ApplyTargetPath(const CQueryPathMulti* query, bool isUpdating);
	void FallbackScout(CCircuitUnit* unit, bool isUpdating);
	void ApplyScoutPath(const CQueryPathSingle* query, bool isCapped, int attempt);

	// Threat-awareness helpers
	float GetSafeThreat(CCircuitUnit* unit) const;  // threat above this is too dangerous for the scout
	float GetPathThreatCap(CCircuitUnit* unit) const;  // hard threat cap for path queries
	void AttackTarget(CCircuitUnit* unit, const springai::AIFloat3& firePos, int frame);
	void ApproachTarget(CCircuitUnit* unit, const springai::AIFloat3& pos, float fireRange, bool isUpdating);
	void ApplyApproachPath(const CQueryPathSingle* query, ICoreUnit::Id targetId, bool isUpdating);
	springai::AIFloat3 PickScoutPosition(CCircuitUnit* unit, float safeThreat);
	void RunScoutQuery(CCircuitUnit* unit, bool isCapped, int attempt);

	std::unordered_map<ICoreUnit::Id, int> badTargets;  // enemy id -> frame until which it is skipped
};

} // namespace circuit

#endif // SRC_CIRCUIT_TASK_FIGHTER_SCOUTTASK_H_

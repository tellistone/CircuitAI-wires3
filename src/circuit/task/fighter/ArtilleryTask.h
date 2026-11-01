/*
 * ArtilleryTask.h
 *
 *  Created on: Jan 6, 2016
 *      Author: rlcevg
 */

#ifndef SRC_CIRCUIT_TASK_FIGHTER_ARTILLERYTASK_H_
#define SRC_CIRCUIT_TASK_FIGHTER_ARTILLERYTASK_H_

#include "task/fighter/FighterTask.h"

namespace circuit {

class CArtilleryTask: public IFighterTask {
public:
	CArtilleryTask(ITaskManager* mgr);
	virtual ~CArtilleryTask();

	virtual bool CanAssignTo(CCircuitUnit* unit) const override;
	virtual void AssignTo(CCircuitUnit* unit) override;
	virtual void RemoveAssignee(CCircuitUnit* unit) override;

	virtual void Start(CCircuitUnit* unit) override;
	virtual void Update() override;

	virtual void OnUnitIdle(CCircuitUnit* unit) override;

private:
	void Execute(CCircuitUnit* unit, bool isUpdating);
	CEnemyInfo* FindTarget(CCircuitUnit* unit, const springai::AIFloat3& pos);
	void ApplyTargetPath(const CQueryPathMulti* query, bool isUpdating);
	void FallbackBasePos(CCircuitUnit* unit, bool isUpdating);
	void ApplyBasePos(const CQueryPathSingle* query, bool isUpdating);
	void FallbackScout(CCircuitUnit* unit, bool isUpdating);
	void ApplyScoutPath(const CQueryPathSingle* query);
	void Fallback(CCircuitUnit* unit, bool proceed);

	// artillery block of behaviour.json
	void FallbackNoTarget(CCircuitUnit* unit, bool isUpdating);  // nothing to walk to
	void FallbackSafePos(CCircuitUnit* unit, bool isUpdating);  // join the nearest group (no_scout)
	void ApplySafePos(const CQueryPathMulti* query, bool isUpdating);
	void FallbackNoSpot(CCircuitUnit* unit, bool isUpdating);  // no acceptable firing spot
	float FillCoveredSpots(CCircuitUnit* unit, float range);  // fills firePositions, returns goal range

	F3Vec safePositions;
	F3Vec firePositions;  // covered firing cells (ally_threat)
	std::vector<float> coverGrid;  // FillCoveredSpots scratch
	std::vector<char> cellAdded;
};

} // namespace circuit

#endif // SRC_CIRCUIT_TASK_FIGHTER_ARTILLERYTASK_H_

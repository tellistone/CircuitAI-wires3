/*
 * RetreatTask.h
 *
 *  Created on: Jan 18, 2015
 *      Author: rlcevg
 */

#ifndef SRC_CIRCUIT_TASK_RETREATTASK_H_
#define SRC_CIRCUIT_TASK_RETREATTASK_H_

#include "task/UnitTask.h"
#include "util/Defines.h"

namespace circuit {

class CQueryCostMap;

class CRetreatTask: public IUnitTask {
public:
	CRetreatTask(ITaskManager* mgr, int timeout = ASSIGN_TIMEOUT);
	virtual ~CRetreatTask();

	virtual void ClearRelease() override;

	virtual void AssignTo(CCircuitUnit* unit) override;
	virtual void RemoveAssignee(CCircuitUnit* unit) override;

	virtual void Start(CCircuitUnit* unit) override;
	virtual void Update() override;
protected:
	virtual void Finish() override;
	virtual void Cancel() override;

public:
	virtual void OnUnitIdle(CCircuitUnit* unit) override;
	virtual void OnUnitDamaged(CCircuitUnit* unit, CEnemyInfo* attacker) override;
	virtual void OnUnitDestroyed(CCircuitUnit* unit, CEnemyInfo* attacker) override;

	void CheckRepairer(CCircuitUnit* newRep);
	void SetRepairer(CCircuitUnit* unit) { repairer = unit; }
	CCircuitUnit* GetRepairer() const { return repairer; }

	// Commander flee (CBuilderManager::UpdateCommFlee): heads for goal (or a cheaper repairer) and doesn't end on
	// full health; the builder manager ends it once the danger is gone
	void SetFlee(const springai::AIFloat3& goal, int frame) { fleeGoal = goal; fleeFrame = frame; }
	void StopFlee(CCircuitUnit* unit);  // carries on as a plain retreat, for repair
	void EndFlee(CCircuitUnit* unit);  // back to work
	bool IsFlee() const { return fleeFrame >= 0; }
	int GetFleeFrame() const { return fleeFrame; }
	const springai::AIFloat3& GetFleeGoal() const { return fleeGoal; }
	bool IsArrived() const { return State::REGROUP == state; }
	void Redirect(CCircuitUnit* unit, const springai::AIFloat3& goal);  // new flee goal, on the way again

private:
	void PushTravelAct(CCircuitUnit* unit);  // move (or fight) home
	void EnqueueRepair(CCircuitUnit* unit);
	springai::AIFloat3 GetHaven(CCircuitUnit* unit) const;  // closest haven, else the base
	// Where the path heads (the repairer, else the flee goal, else the haven) and how close counts as there
	springai::AIFloat3 GetGoal(CCircuitUnit* unit, float& outRange) const;
	void ApplyPath(const CQueryPathSingle* query);
	CCircuitUnit* ValidateNewRepairer(const IPathQuery* query, int newRepId) const;
	void ApplyCostMap(const CQueryCostMap* query, CCircuitUnit* newRep);

	CCircuitUnit* repairer;
	int regroupFrame;  // when unit arrived at haven
	float lastHavenSqDist;  // distance to haven at the last move attempt, -1 = none
	std::shared_ptr<IPathQuery> costQuery;  // owner
	springai::AIFloat3 fleeGoal;
	int fleeFrame;  // flee start, -1 = plain retreat
};

} // namespace circuit

#endif // SRC_CIRCUIT_TASK_RETREATTASK_H_

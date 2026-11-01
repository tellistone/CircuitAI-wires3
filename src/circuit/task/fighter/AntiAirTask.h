/*
 * AntiAirTask.h
 *
 *  Created on: Jan 6, 2016
 *      Author: rlcevg
 */

#ifndef SRC_CIRCUIT_TASK_FIGHTER_ANTIAIRTASK_H_
#define SRC_CIRCUIT_TASK_FIGHTER_ANTIAIRTASK_H_

#include "task/fighter/SquadTask.h"

#include <set>

namespace circuit {

class CAntiAirTask: public ISquadTask {
public:
	CAntiAirTask(ITaskManager* mgr, float powerMod);
	virtual ~CAntiAirTask();

	virtual bool CanAssignTo(CCircuitUnit* unit) const override;
	virtual void AssignTo(CCircuitUnit* unit) override;
	virtual void RemoveAssignee(CCircuitUnit* unit) override;

	virtual void Start(CCircuitUnit* unit) override;
	virtual void Update() override;

	virtual void OnUnitIdle(CCircuitUnit* unit) override;
	virtual void OnUnitDamaged(CCircuitUnit* unit, CEnemyInfo* attacker) override;

protected:
	// Firing spots (IFighterTask::Attack): judge spots against the squad's current strength,
	// with the same allowance as target choice (base bonus and thr_mod.distance at the spot)
	virtual float GetEscapeSafeThreat() const override { return GetSafeThreat(); }
	virtual float GetAllowedThreatAt(const springai::AIFloat3& pos, float strength) const override;

private:
	void FindTarget();
	void FallbackDisengage();
	void ApplyDisengagePath(const CQueryPathSingle* query);
	void ApplyTargetPath(const CQueryPathSingle* query);
	void FallbackSafePos();
	void ApplySafePos(const CQueryPathMulti* query);
	void FallbackCommPos();
	void FallbackBasePos(bool isCapped);
	void ApplyFallbackPath(const CQueryPathSingle* query, bool isDisengage, bool isCapped);
	void RegroupUnit(CCircuitUnit* unit);
	void ApplyRegroupPath(const CQueryPathSingle* query);
	void Hold(CCircuitUnit* unit);
	void HoldAll();

	void ApplyDamagedPath(const CQueryPathMulti* query);

	float GetAirRange() const;  // the squad's range against aircraft
	float GetSafeThreat() const;  // current squad strength (health-scaled power) x powerMod
	// Pathfinder threat cap for a route from startPos to goals (see .cpp)
	float GetPathThreatCap(CCircuitUnit* unit, const springai::AIFloat3& startPos, const F3Vec& goals) const;
	void FlushHolds();
	bool FlushSplits();  // true: the task was aborted (no units left)

	F3Vec homePoints;  // FindTarget scratch: leash anchors
	// Hold orders are queued by path callbacks and sent from Update(), next to the squad's other orders
	std::set<CCircuitUnit*> holdPending;
	std::set<CCircuitUnit*> splitPending;  // can't rejoin the squad safely: leaves for its own task (from Update)
	int mergeFrame = 0;  // no merging (either way) before this frame: set on a task split off a blocked regroup
	springai::AIFloat3 approachPos;  // where to walk to shoot the target: its firing spot (FindTarget)
};

} // namespace circuit

#endif // SRC_CIRCUIT_TASK_FIGHTER_ANTIAIRTASK_H_

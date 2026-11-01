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
	void FallbackBasePos();
	void ApplyFallbackPath(const CQueryPathSingle* query, bool isDisengage);
	void RegroupUnit(CCircuitUnit* unit);
	void ApplyRegroupPath(const CQueryPathSingle* query);
	void Hold(CCircuitUnit* unit);
	void HoldAll();

	void ApplyDamagedPath(const CQueryPathMulti* query);

	float GetSafeThreat() const;  // current squad strength (health-scaled power) x powerMod
	void FlushHolds();

	F3Vec homePoints;  // FindTarget scratch: leash anchors
	// Hold orders are sent from Update(): path callbacks can run during an allied AI's turn,
	// and orders sent from there go out under the wrong team and are dropped
	std::set<CCircuitUnit*> holdPending;
};

} // namespace circuit

#endif // SRC_CIRCUIT_TASK_FIGHTER_ANTIAIRTASK_H_

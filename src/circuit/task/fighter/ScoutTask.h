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

#include <memory>
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
	void ApplyScoutPath(const CQueryPathSingle* query, int attempt);

	// Threat-awareness helpers
	float GetSafeThreat(CCircuitUnit* unit) const;  // threat above this is too dangerous for the scout
	// Hard threat cap for path queries; goalMod = thr_mod.distance multiplier at the goal (the stricter of start and goal is used)
	float GetPathThreatCap(CCircuitUnit* unit, float goalMod = 1.f) const;
	void AttackTarget(CCircuitUnit* unit, const springai::AIFloat3& firePos, int frame);
	void ApproachTarget(CCircuitUnit* unit, const springai::AIFloat3& pos, float fireRange, bool isUpdating);
	void ApplyApproachPath(const CQueryPathSingle* query, ICoreUnit::Id targetId, bool isUpdating);
	// Scout spot, else a random safe spot no nearer our base than the scout, else our frontline cluster; -RgtVector if none
	springai::AIFloat3 PickScoutPosition(CCircuitUnit* unit);
	void RunScoutQuery(CCircuitUnit* unit, int attempt);

	void ActivatePath(CCircuitUnit* unit, const std::shared_ptr<PathInfo>& pPath, ICoreUnit::Id pathTargetId, bool isCapped);
	bool NeedRepath(CCircuitUnit* unit, ICoreUnit::Id targetId) const;
	bool IsPathUnsafe(CCircuitUnit* unit) const;

	// Danger at p: threat x thr_mod.distance(p) / GetSafeThreat; at 1 or more the scout can't take it
	float GetDangerRatio(CCircuitUnit* unit, const springai::AIFloat3& p) const;
	bool IsInDanger(CCircuitUnit* unit, const springai::AIFloat3& p) const { return GetDangerRatio(unit, p) >= 1.f; }
	// Flee: a scout standing in danger runs to the nearest spot well clear of it (least-threat route), and resumes
	// scouting once it has been out of danger for FLEE_CLEAR_FRAMES. Scouts never head home.
	bool CheckFlee(CCircuitUnit* unit, const springai::AIFloat3& pos, int frame);  // true: fleeing, skip scouting
	void Flee(CCircuitUnit* unit, const springai::AIFloat3& pos, int frame);
	bool IsFleeing(int frame) const { return frame < fleeUntil; }
	void HoldScout(CCircuitUnit* unit);  // nowhere safe to scout right now: stop, pick again in a second

	std::unordered_map<ICoreUnit::Id, int> badTargets;
	int lastPathFrame = -1;  // when the current travel path was applied
	ICoreUnit::Id lastPathTargetId = -1;  // target the current travel path leads to (-1: none)
	bool isPathCapped = false;  // current travel path avoids tiles above safe threat
	int lastAttackFrame = -1;
	int lastStallCheck = -1;  // last check for an empty order queue
	ICoreUnit::Id lastAttackId = -1;  // enemy id -> frame until which it is skipped

	static constexpr int FLEE_CLEAR_FRAMES = FRAMES_PER_SEC * 2;  // out of danger this long ends the flee
	static constexpr float FRONT_ARRIVE_DIST = 200.f;  // last-resort front spot: not one we already stand at
	int fleeUntil = -1;  // fleeing until this frame (pushed on while in danger); -1: not fleeing
	int lastFleeFrame = -1;  // last flee path request
	int holdUntil = -1;  // HoldScout: no new scouting pick until this frame
};

} // namespace circuit

#endif // SRC_CIRCUIT_TASK_FIGHTER_SCOUTTASK_H_

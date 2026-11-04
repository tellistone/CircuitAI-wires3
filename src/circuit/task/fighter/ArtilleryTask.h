/*
 * ArtilleryTask.h
 *
 *  Created on: Jan 6, 2016
 *      Author: rlcevg
 */

#ifndef SRC_CIRCUIT_TASK_FIGHTER_ARTILLERYTASK_H_
#define SRC_CIRCUIT_TASK_FIGHTER_ARTILLERYTASK_H_

#include "task/fighter/FighterTask.h"
#include "module/MilitaryManager.h"

#include <memory>
#include <vector>

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
	const CMilitaryManager::SArtilleryCfg& GetCfg() const;  // artillery block of behaviour.json
	float GetMaxPathThreat(CCircuitUnit* unit) const;  // max_path_threat x own power, or no limit

	// Shooting and walking to firing spots
	void Execute(CCircuitUnit* unit, bool isUpdating);
	CEnemyInfo* FindTarget(CCircuitUnit* unit, const springai::AIFloat3& pos);  // in range, else fills enemyPositions
	void ApplyTargetPath(const CQueryPathMulti* query, bool isUpdating);
	float FillCoveredSpots(CCircuitUnit* unit, float range);  // ally_threat: fills firePositions, returns goal range

	// Nothing to shoot or walk to
	void FallbackNoTarget(CCircuitUnit* unit, bool isUpdating);  // no known building
	void FallbackNoSpot(CCircuitUnit* unit, bool isUpdating);  // no acceptable firing spot
	void FallbackStay(CCircuitUnit* unit, bool isUpdating);  // no_scout: hold if safe and covered, else FallbackSafePos
	void HoldHere(CCircuitUnit* unit);  // stop walking (incl. queued engine waypoints) and wait
	void ApplyPathOrHold(CCircuitUnit* unit, const std::shared_ptr<PathInfo>& pPath);  // walk, or hold if already there
	void FillSafeSpots(CCircuitUnit* unit);  // safePositions: FillSafePos pulled back by follow_dist
	void FallbackSafePos(CCircuitUnit* unit, bool isUpdating);  // move to the nearest safety spot
	void ApplySafePos(const CQueryPathMulti* query, bool isUpdating);
	void FallbackBasePos(CCircuitUnit* unit, bool isUpdating);
	void ApplyBasePos(const CQueryPathSingle* query, bool isUpdating);
	// Old fallbacks (no_scout false)
	void FallbackScout(CCircuitUnit* unit, bool isUpdating);
	void ApplyScoutPath(const CQueryPathSingle* query);
	void Fallback(CCircuitUnit* unit, bool proceed);

	// Early flee (artillery_flee_margin)
	bool CheckFlee(CCircuitUnit* unit);  // true while fleeing
	CEnemyInfo* FindDanger(const springai::AIFloat3& pos, float fleeMargin, float& outThreat) const;
	void Flee(CCircuitUnit* unit, const springai::AIFloat3& dangerPos);
	void ApplyFleePath(const CQueryPathSingle* query);

	// Cover (ally_threat, ally_radius)
	static bool IsEscort(CCircuitUnit* unit, CAllyUnit* ally, bool isFlee);  // isFlee: static defences count too
	void CollectEscorts(CCircuitUnit* unit);  // fills escorts (mobile only)
	// Escort power near spot, net of enemyThreat, reaches ally_threat
	bool HasCover(CCircuitUnit* unit, const springai::AIFloat3& spot, float enemyThreat = 0.f, bool isFlee = false);

	bool isFleeing = false;
	int fleeFrame = 0;  // re-plan the flee after this frame

	// Scratch shared by all artillery tasks (micro-opt, as IFighterTask::enemyPositions)
	static F3Vec safePositions;
	static F3Vec firePositions;  // covered firing cells
	static std::vector<std::pair<springai::AIFloat3, float>> escorts;  // position, power
	static std::vector<float> coverGrid;  // FillCoveredSpots: cover per cell of the escorts' area
	static std::vector<char> cellAdded;  // FillCoveredSpots: cell already checked
};

} // namespace circuit

#endif // SRC_CIRCUIT_TASK_FIGHTER_ARTILLERYTASK_H_

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
	float GetFleeMargin(CCircuitUnit* unit) const;  // artillery_flee_margin of this unit type, else the block's; < 0 = off

	// Shooting and walking to firing spots
	void Execute(CCircuitUnit* unit, bool isUpdating);
	CEnemyInfo* FindTarget(CCircuitUnit* unit, const springai::AIFloat3& pos);  // in range, else fills enemyPositions
	void ApplyTargetPath(const CQueryPathMulti* query, bool isUpdating);
	// Walking to a firing spot: keep the route between full re-plans (every 6 s) while it's still good
	bool IsFireRouteValid(CCircuitUnit* unit, float range);
	bool IsFireSpotValid(CCircuitUnit* unit, const springai::AIFloat3& spot);  // FillCoveredSpots' rules for one spot
	// ally_threat: fills firePositions (covered, safe, reachable, and not where the flee would fire), returns goal range
	float FillCoveredSpots(CCircuitUnit* unit, float range);

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
	struct SDanger {  // flee zone: an enemy that can hit us, and how far that sets off the flee
		springai::AIFloat3 pos;
		float sqReach;  // (weapon range + margin)^2
		float threat;
	};
	bool CheckFlee(CCircuitUnit* unit);  // true while fleeing
	// Fills dangers: flee zones (IsGroundDanger, reach = weapon range + margin) that touch the x/z rectangle
	void CollectDangers(float margin, const springai::AIFloat3& minPos, const springai::AIFloat3& maxPos);
	// The nearest flee zone in dangers that reaches pos, nullptr if none; outThreat: all of them that reach it
	static const SDanger* FindDanger(const springai::AIFloat3& pos, float& outThreat);
	void Flee(CCircuitUnit* unit, const springai::AIFloat3& dangerPos);
	void ApplyFleePath(const CQueryPathSingle* query);

	// Cover (ally_threat, ally_radius)
	struct SEscort {
		springai::AIFloat3 pos;
		float power;
		bool isMobile;  // only mobile escorts cover a firing spot; static defences only count against the flee
	};
	static bool IsEscort(CCircuitUnit* unit, CAllyUnit* ally, bool isFlee);  // isFlee: static defences count too
	void CollectEscorts(CCircuitUnit* unit, bool isFlee);  // fills escorts
	// Escort power near spot, net of enemyThreat, reaches ally_threat
	bool HasCover(CCircuitUnit* unit, const springai::AIFloat3& spot, float enemyThreat = 0.f, bool isFlee = false);

	std::shared_ptr<PathInfo> firePath;  // route to the current firing spot
	int replanFrame = 0;  // re-plan that route in full after this frame
	bool isFleeing = false;
	int fleeFrame = 0;  // re-plan the flee after this frame

	// Scratch shared by all artillery tasks (micro-opt, as IFighterTask::enemyPositions)
	static F3Vec safePositions;
	static F3Vec firePositions;  // covered firing cells
	static std::vector<SEscort> escorts;
	static std::vector<SDanger> dangers;
	static std::vector<float> coverGrid;  // FillCoveredSpots: mobile escort power per cell of the escorts' area
	static std::vector<float> fleeCoverGrid;  // FillCoveredSpots: the same with static defences, against the flee
	static std::vector<char> cellAdded;  // FillCoveredSpots: cell already checked
};

} // namespace circuit

#endif // SRC_CIRCUIT_TASK_FIGHTER_ARTILLERYTASK_H_

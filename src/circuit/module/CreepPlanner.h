/*
 * CreepPlanner.h
 *
 *  Created on: Nov 11, 2026
 *      Author: Claude
 */

#ifndef SRC_CIRCUIT_MODULE_CREEPPLANNER_H_
#define SRC_CIRCUIT_MODULE_CREEPPLANNER_H_

#include "unit/CoreUnit.h"

#include "AIFloat3.h"

#include <map>
#include <vector>

namespace circuit {

class CCircuitAI;
class CCircuitUnit;
class IBuilderTask;

/*
 * turret.creep: pushers claim the regions beyond our line for our turrets, region by region, up to just outside the
 * enemy's static range. Once a second (doc/chokepoints.md, "The creep"):
 * - Lanes: the live chokes of our current line (the chokes out of our held ground). A lane's outer region is the side
 *   we don't hold; none if the enemy holds it. Its target: the outer region's choke (no enemy artillery on it) nearest
 *   the enemy's held ground, or the region's centre if it has no other.
 * - A step: from our finished turret that hits ground nearest the target (within one step + its range of the lane's
 *   choke), turret.creep_step x its range toward the target, ending on the target once within a step; cut, then halved
 *   and quartered, out of every known enemy static's range + turret.creep_margin.
 * - A lane rests while a finished defence (ours or an ally's) stands in its outer region or by its target: the
 *   region becomes ours (held ground, 20 s) and the line moves past it. One of ours being built or queued there, or
 *   by the step's end, is helped instead. No step where turret.creep_losses of our turrets died in the outer region in
 *   the last 2 minutes, or after two failed steps in 2 minutes.
 * - Pushers: turret.creep_builders plus one per turret.creep_income metal/s; the commander first (not while hide mode
 *   holds it), then the free mobile ground constructors nearest a lane. Each free one, every turret.creep_interval at
 *   most, takes the nearest lane no other took this second (CBuilderManager::OrderCreepTurret)
 * - Between steps a pusher guards its lane's turret, while the account fills for its next order within two intervals
 *   and metal isn't stalling (MakeHoldTask)
 */
class CCreepPlanner {
public:
	CCreepPlanner(CCircuitAI* circuit);
	virtual ~CCreepPlanner();

	void Update();  // every second

	bool IsPusher(const CCircuitUnit* unit) const;
	// A pusher between jobs: a guard on its lane's turret, or nullptr (it works as any builder)
	IBuilderTask* MakeHoldTask(CCircuitUnit* unit);
	void OnGunLost(const springai::AIFloat3& pos);  // a finished ground-hitting turret of ours died
	void OnDropped(const springai::AIFloat3& pos);  // a creep job dropped on arrival: its lane's step failed

private:
	struct SLane {
		int chokeId;  // the line choke's graph id (kept over rebuilds)
		springai::AIFloat3 end;  // the step's end, or the job to help
		float radius;  // the picker's about end
		ICoreUnit::Id anchorId;  // the turret it steps from
		float gap;  // elmos from that turret to the target
	};
	struct SFail {
		int count = 0;
		int frame = 0;
	};
	enum Skip: int {NOT_LIVE, ENEMY_HELD, FAILED, LOSSES, CLAIMED, NO_ANCHOR, IN_REACH, _SIZE_};

	void UpdatePushers();
	void MakeLanes();
	const SLane* FindLane(int chokeId) const;
	const SLane* NearestLane(const springai::AIFloat3& pos, const std::vector<bool>* isTaken = nullptr) const;
	void AddFail(int chokeId);

	CCircuitAI* circuit;
	std::vector<ICoreUnit::Id> pushers;
	std::map<ICoreUnit::Id, int> nextFrames;  // pusher -> frame of its next step at the earliest
	std::map<ICoreUnit::Id, int> laneOf;  // pusher -> the line choke id it last stepped on
	std::vector<SLane> lanes;  // this second's
	std::vector<std::pair<springai::AIFloat3, int>> losses;  // our ground-hitting turrets lost, when
	std::map<int, SFail> fails;  // line choke id -> its failed steps
	int skips[Skip::_SIZE_];  // this second's, for the log
	int lineSize;
	int logFrame;
};

} // namespace circuit

#endif // SRC_CIRCUIT_MODULE_CREEPPLANNER_H_

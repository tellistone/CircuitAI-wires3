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
 * turret.creep: our builders walk our turrets out along the choke analysis, lane by lane, up to just outside the
 * enemy's static range. Once a second (doc/chokepoints.md, "The creep"):
 * - Lanes: the live chokes of our current line, as CFrontline ranks them (income behind x traffic). The target: the
 *   outer region's choke (no enemy artillery on it) nearest the enemy's held ground beyond it, kept until another is
 *   20% nearer, or its centre if it has none (or is the enemy's last ground); enemy-held ground too: the step stops at
 *   their turrets' range.
 * - A step: from our finished turret that hits ground nearest the target, in the lane's two regions (else anywhere),
 *   turret.creep_step x its range toward the lane's gap until within turret.creep_radius of it, then toward the target,
 *   landing on it once within a step; pulled back to a half, then a quarter, out of every known enemy static's range +
 *   turret.creep_margin. None that doesn't bring the anchor nearer the enemy. Light defs on the way; the unlocked
 *   tiers too at the line (on the target, or pulled back by their range).
 * - Claimed: a finished ground defence (ours or an ally's) within turret.creep_radius of the target, or within half
 *   the step of its end, or an ally's being built there. A creep step of ours under way within turret.creep_radius
 *   of the end: one at a time. Another job of ours being built or queued there: help it (assist only, never a second
 *   one).
 * - Rests: turret.creep_losses of our turrets lost in the outer region or a step of the end, or two failed steps
 *   within 2 minutes, rest a lane 2 minutes, doubling (up to 16 minutes) while it keeps failing within 10 minutes of
 *   its last rest. A step's end under threat (the picker's creep rule) only waits.
 * - Orders: up to turret.creep_builders steps under way at once, plus one per turret.creep_income metal/s. Each step
 *   is a job for a free mobile ground builder (CBuilderManager::OrderCreepTurret): the commander first, while hide
 *   mode doesn't hold it, on the highest-ranked of the three lanes nearest it; else the highest-ranked lane left
 *   takes the free constructor nearest its step. A lane takes an order every turret.creep_interval at most. Between
 *   steps a builder works as any other
 */
class CCreepPlanner {
public:
	CCreepPlanner(CCircuitAI* circuit);
	virtual ~CCreepPlanner();

	void Update();  // every second

	void OnGunLost(const springai::AIFloat3& pos);  // a finished ground-hitting turret of ours died
	// A creep job dropped on arrival: its lane's step failed (the builder's lane, else the nearest to pos)
	void OnDropped(const CCircuitUnit* builder, const springai::AIFloat3& pos);

private:
	struct SLane {
		int chokeId;  // the line choke's graph id (kept over rebuilds)
		springai::AIFloat3 end;  // the step's end, or the job to help
		float radius;  // the picker's about end
		ICoreUnit::Id anchorId;  // the turret it steps from
		bool isHelp;  // a defence of ours is queued or being built at end: assist only
		bool isAtLine;  // on the target or pulled back by enemy range: the unlocked tiers too
	};
	struct SMemo {  // a lane's, kept over seconds
		int fails = 0;
		int lastFail = 0;
		int rests = 0;  // in a row: each doubles the next
		int lastRest = -1;
		int until = 0;  // resting until
		int targetId = -1;  // the choke id it last targeted
		int next = 0;  // no order on it before this frame
	};
	struct SGun {
		ICoreUnit::Id id;
		springai::AIFloat3 pos;
		float range;
		int region;
	};
	enum Skip: int {NOT_LIVE, RESTING, CLAIMED, NO_ANCHOR, NO_PROGRESS, IN_REACH, STEPPING, THREATENED, _SIZE_};

	void Gather();  // this second's guns, creep jobs, enemy reach and enemy ground
	void MakeLanes();
	void OrderSteps(int most);  // to free builders, while fewer than most steps are under way
	// The outer region's exit nearest the enemy (memo.targetId kept until another is 20% nearer), else its centre
	springai::AIFloat3 ChooseTarget(int choke, int outer, SMemo& memo) const;
	const SGun* ChooseAnchor(int inner, int outer, const springai::AIFloat3& target) const;
	// From anchor toward aim, a full step pulled back out of the enemy's reach; false if none (at their line)
	bool MakeStep(const SGun& anchor, const springai::AIFloat3& aim, float fullStep, springai::AIFloat3& outEnd,
			float& outLen) const;
	float EnemySqDist(const springai::AIFloat3& pos, int skipRegion) const;  // to the enemy's ground beyond skipRegion
	bool IsInEnemyReach(const springai::AIFloat3& pos) const;
	// The highest-ranked (first) among the three nearest pos not taken
	const SLane* PickLane(const springai::AIFloat3& pos, const std::vector<bool>* isTaken = nullptr) const;
	void AddFail(int chokeId);
	void Rest(SMemo& m);

	CCircuitAI* circuit;
	std::map<ICoreUnit::Id, int> laneOf;  // builder -> the line choke id of its last creep order (OnDropped)
	std::vector<SLane> lanes;  // this second's, highest rank first
	std::vector<std::pair<springai::AIFloat3, int>> losses;  // our ground-hitting turrets lost, when
	std::map<int, SMemo> memos;  // line choke id -> its rests and target
	// This second's (Gather)
	std::vector<SGun> guns;  // our finished ground-hitting turrets: the anchors
	std::vector<springai::AIFloat3> jobs;  // our creep steps under way (queued or being built)
	std::vector<std::pair<springai::AIFloat3, float>> enemyReach;  // known enemy ones: land range + creep_margin
	std::vector<std::pair<int, springai::AIFloat3>> enemyAt;  // the enemy's held ground (its side if none): region, centre
	int skips[Skip::_SIZE_];  // this second's, for the log
	int lineSize;
	int logFrame;
};

} // namespace circuit

#endif // SRC_CIRCUIT_MODULE_CREEPPLANNER_H_

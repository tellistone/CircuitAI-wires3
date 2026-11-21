/*
 * CreepPlanner.h
 *
 *  Created on: Nov 11, 2026
 *      Author: Claude
 */

#ifndef SRC_CIRCUIT_MODULE_CREEPPLANNER_H_
#define SRC_CIRCUIT_MODULE_CREEPPLANNER_H_

#include "AIFloat3.h"

#include <map>
#include <vector>

namespace circuit {

class CCircuitAI;
class CCircuitUnit;
class IBuilderTask;

/*
 * turret.creep: walks our turrets out along the choke analysis, lane by lane, up to just outside the enemy's static
 * range, as jobs for our builders. Once a second (doc/chokepoints.md, "The creep"):
 * - Lanes: the live chokes of our current line, as CFrontline ranks them (income behind x traffic). The target: the
 *   outer region's choke (no enemy artillery on it) nearest the enemy's held ground beyond it, kept until another is
 *   20% nearer, or its centre if it has none (or is the enemy's last ground); enemy-held ground too: the step stops at
 *   their turrets' range.
 * - A step: from our finished turret that hits ground nearest the target, in the lane's two regions (else anywhere),
 *   turret.creep_step x its range toward the lane's gap until within turret.creep_radius of it, then toward the target,
 *   landing on it once within a step; pulled back to a half, then a quarter, out of every known enemy static's range +
 *   turret.creep_margin. None that doesn't bring the anchor nearer the enemy. Light defs, and each turret.tiers
 *   entry the turret account's spare covers (turret.creep_budget x its dearest def, past a waiting ask), whatever
 *   our mex share; at the line (on the target, or pulled back by their range) the unlocked tiers too, waited for.
 * - Claimed: a finished ground defence (ours or an ally's) within turret.creep_radius of the target, or within half
 *   the step of its end, or an ally's being built there. One step at a time: a lane with a creep job of its own
 *   waits for it. Another job of ours being built or queued at the end: help it, by raising it to NORMAL (never a
 *   second turret beside it; a nanoframe of ours whose job timed out gets a repair job).
 * - Rests: turret.creep_losses of our turrets lost in the outer region or a step of the end, or two failed steps
 *   within 2 minutes, rest a lane 2 minutes, doubling (up to 16 minutes) while it keeps failing within 10 minutes of
 *   its last rest. A step's end under threat (the picker's creep rule) only waits.
 * - Orders: up to turret.creep_builders steps queued or under way at once, plus one per turret.creep_income metal/s,
 *   highest-ranked lane first. Each step is a job queued at HIGH (CBuilderManager::OrderCreepTurret) that any builder
 *   looking for work may take, as it ranks jobs by priority and walk (MakeBuilderTask): nobody building is pulled
 *   off, though one walking to another job may switch to it, as for any job. Once begun, it builds at NORMAL. Only
 *   with a builder that can reach the step within the HIGH travel cap (FindPlanner, whom the spot is searched for),
 *   else the next lane gets it. A job holds its lane and its place in the count until it is built or times out (30 s
 *   with no builder on it; refunded if not started), so one whose builder died stays queued for the next. A lane
 *   takes an order every turret.creep_interval at most, and waits 60 s after its job was cancelled unstarted
 */
class CCreepPlanner {
public:
	CCreepPlanner(CCircuitAI* circuit);
	virtual ~CCreepPlanner();

	void Update();  // every second

	void OnGunLost(const springai::AIFloat3& pos);  // a finished ground-hitting turret of ours died
	void AddFail(int lane);  // a step failed: no spot, or its job dropped on arrival (two in 2 minutes rest the lane)
	void OnUntaken(int lane);  // a creep job of lane cancelled before it started (timed out, dropped): it waits

private:
	struct SLane {
		int chokeId;  // the line choke's graph id (kept over rebuilds)
		springai::AIFloat3 end;  // the step's end, or the job to help
		float radius;  // the picker's about end
		bool isAtLine;  // on the target or pulled back by enemy range: the unlocked tiers too, waited for
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
		springai::AIFloat3 pos;
		float range;
		int region;
	};
	enum Skip: int {NOT_LIVE, RESTING, STEPPING, CLAIMED, HELPED, NO_ANCHOR, NO_PROGRESS, IN_REACH, NO_BUILDER,
			THREATENED, _SIZE_};

	void Gather();  // this second's guns, creep jobs, enemy reach and enemy ground
	void MakeLanes();
	void OrderSteps(int most);  // queues steps while fewer than most are queued or under way
	// Whom a step's spot is searched for: the nearest mobile ground constructor that can reach pos within the HIGH
	// travel cap at its speed, as MakeBuilderTask takes jobs; else the commander (not while hide mode holds it), so
	// a constructor's search isn't held to the commander's flee zones. nullptr: nobody would take it
	CCircuitUnit* FindPlanner(const springai::AIFloat3& pos) const;
	// The outer region's exit nearest the enemy (memo.targetId kept until another is 20% nearer), else its centre
	springai::AIFloat3 ChooseTarget(int choke, int outer, SMemo& memo) const;
	const SGun* ChooseAnchor(int inner, int outer, const springai::AIFloat3& target) const;
	// From anchor toward aim, a full step pulled back out of the enemy's reach; false if none (at their line)
	bool MakeStep(const SGun& anchor, const springai::AIFloat3& aim, float fullStep, springai::AIFloat3& outEnd,
			float& outLen) const;
	float EnemySqDist(const springai::AIFloat3& pos, int skipRegion) const;  // to the enemy's ground beyond skipRegion
	bool IsInEnemyReach(const springai::AIFloat3& pos) const;
	void Rest(SMemo& m);

	CCircuitAI* circuit;
	std::vector<SLane> lanes;  // this second's, highest rank first
	std::vector<std::pair<springai::AIFloat3, int>> losses;  // our ground-hitting turrets lost, when
	std::map<int, SMemo> memos;  // line choke id -> its rests and target
	// This second's (Gather)
	std::vector<SGun> guns;  // our finished ground-hitting turrets: the anchors
	std::vector<int> jobLanes;  // the lanes of our creep jobs, queued or being built
	std::vector<std::pair<springai::AIFloat3, float>> enemyReach;  // known enemy ones: land range + creep_margin
	std::vector<std::pair<int, springai::AIFloat3>> enemyAt;  // the enemy's held ground (its side if none): region, centre
	int skips[Skip::_SIZE_];  // this second's, for the log
	int lineSize;
	int logFrame;
};

} // namespace circuit

#endif // SRC_CIRCUIT_MODULE_CREEPPLANNER_H_

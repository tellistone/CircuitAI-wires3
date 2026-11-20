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
 * turret.creep: pushers step our turrets forward to just outside the enemy's static range, once a second.
 * - Pushers: turret.creep_builders, plus one per turret.creep_income metal/s of income; the commander first (not
 *   while hide mode holds it), then the free mobile ground constructors nearest the best step. Kept while alive.
 * - Steps: from each finished turret of ours that hits ground, toward the known enemy static that hits ground whose
 *   stop line (its land range + turret.creep_margin) is nearest; with none known, toward the nearest live choke of
 *   the enemy line (stop line: the choke). A step is turret.creep_step x the turret's range, cut at the stop line,
 *   and shortened (to a half, then a quarter) out of every other enemy static's reach. None where a defence of ours or
 *   an ally's (queued too) already stands within turret.creep_radius of its end, where turret.creep_losses of our
 *   turrets died within 2 minutes in range of it, or from a turret whose steps failed twice in the last 2 minutes.
 *   Already at the stop line (within a quarter step), a turret makes none: the creep has reached their line there.
 * - Each free pusher, every turret.creep_interval at most, takes the step with the shortest gap to its stop line among
 *   the three nearest it, and orders a picker turret within turret.creep_radius of its end
 *   (CBuilderManager::OrderCreepTurret: creep_budget x the cost in the account, the picker's creep threat rule).
 * - Between steps a pusher guards (repairs, assists) the best step's turret, if the account fills for it within two
 *   intervals and metal isn't stalling, rather than taking a job elsewhere (MakeHoldTask)
 */
class CCreepPlanner {
public:
	CCreepPlanner(CCircuitAI* circuit);
	virtual ~CCreepPlanner();

	void Update();  // every second

	bool IsPusher(const CCircuitUnit* unit) const;
	// A pusher between jobs: a guard on the best step's turret, or nullptr (it works as any builder)
	IBuilderTask* MakeHoldTask(CCircuitUnit* unit);

private:
	struct SGun {
		ICoreUnit::Id id;
		springai::AIFloat3 pos;
		float range;
	};
	struct SStep {
		ICoreUnit::Id fromId;  // the turret it starts from
		springai::AIFloat3 end;
		float gap;  // elmos from the turret to the stop line
	};
	struct SFail {
		int count = 0;
		int frame = 0;
	};

	void UpdatePushers(const springai::AIFloat3& nearPos);
	void MakeSteps(const std::vector<SGun>& ours, const std::vector<SGun>& enemies);
	bool IsBlocked(const SStep& step, float radius, float lossRange) const;

	CCircuitAI* circuit;
	std::vector<ICoreUnit::Id> pushers;
	std::map<ICoreUnit::Id, int> nextFrames;  // pusher -> frame of its next step at the earliest
	std::vector<SStep> steps;  // this second's, shortest gap first
	std::map<ICoreUnit::Id, springai::AIFloat3> turrets;  // our guns last second, for the losses
	std::vector<std::pair<springai::AIFloat3, int>> losses;  // our guns lost, when
	std::map<ICoreUnit::Id, SFail> fails;  // a turret's failed steps
	int logFrame;
};

} // namespace circuit

#endif // SRC_CIRCUIT_MODULE_CREEPPLANNER_H_

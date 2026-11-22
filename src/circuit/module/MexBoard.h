/*
 * MexBoard.h
 *
 *  Created on: Nov 22, 2026
 *      Author: Claude
 */

#ifndef SRC_CIRCUIT_MODULE_MEXBOARD_H_
#define SRC_CIRCUIT_MODULE_MEXBOARD_H_

#include "unit/CoreUnit.h"

#include "AIFloat3.h"

#include <vector>

namespace circuit {

class CCircuitAI;
class CCircuitUnit;
class CQueryCostMap;
class IBuilderTask;

/*
 * economy.mex_board, for the opening (CEconomyManager::IsOpening): builders are matched to mex spots by when each
 * would get there, instead of mex jobs being made one at a time near whichever builder picks. A builder holds one
 * spot: the one it walks to, or while it builds something else, its next. Its arrival at a spot is the time left on its
 * job (and on the energy the mex chain queues after a mex), plus the walk from that job's site (from where it stands
 * between jobs or walking to its spot), on its cost map when that started there, else in a straight line. Pairs go by
 * mex_front's score of the arrival (comm_bias for a commander), lowest first, so no builder takes a spot another
 * reaches sooner; a commander never takes a spot another free builder reaches sooner. A held spot goes to another
 * builder only if it gets there min_gain_sec sooner, and a builder leaves its spot for another only if that scores
 * min_gain_sec better. A builder walking to its spot within builder_jobs.commit_sec of it keeps it, and one fleeing
 * keeps it for hold_sec. A mex job is made only when a builder is matched (no job cap), and no other builder takes it
 * (CBMexTask::CanAssignTo). Only spots the builder's own picker would take (reach, flee zones, threat, a hiding
 * commander's range and territory), and not nearer an ally's base or factory than ours. A job that ended without its
 * mex (unreachable, the build failed) keeps its spot from that builder for 30 s. Matching runs when something changed,
 * at most once a frame: a builder with no spot asks for a job, a builder starts building its spot, a held job ends, a
 * builder dies. When the opening ends, builders keep the job they walk to; the next jobs nobody walks to are dropped
 */
class CMexBoard {
public:
	CMexBoard(CCircuitAI* circuit);

	bool IsActive() const;
	bool IsHolding() const { return !holds.empty(); }
	bool IsHeld(int spot) const;
	// Held for a builder other than unit: unit doesn't take it
	bool IsHeldByOther(const IBuilderTask* task, const CCircuitUnit* unit) const;
	// The mex job unit holds, nullptr = none. A builder between jobs matches first if anything changed. query: the
	// cost map its pick rates on (CBuilderManager::DefaultMakeTask), nullptr = the one it holds
	IBuilderTask* GetMexJob(CCircuitUnit* unit, const CQueryCostMap* query = nullptr);

	void OnBuildStart(CCircuitUnit* unit, const IBuilderTask* task);  // at its site: its next spot comes now
	void OnTaskRemoved(const IBuilderTask* task, bool done);
	void OnBuilderDestroyed(CCircuitUnit* unit);
	// Each frame (CBuilderManager::UpdateBuild): matches if anything changed, and builders that got a spot while
	// between jobs (or lost the one they walked to) pick now, not on the idle pass
	void Update();

private:
	struct SHold {
		CCircuitUnit* unit;
		IBuilderTask* task;
		int spot;
		int fleeFrame;  // its builder began to flee at, -1 = not fleeing
	};
	struct SSkip {
		ICoreUnit::Id unitId;
		int spot;
		int until;  // frame
	};
	SHold* FindHold(const CCircuitUnit* unit);
	void Match(CCircuitUnit* asker, const CQueryCostMap* askerQuery);
	// Where unit's walk to a spot would start, and the seconds before it can (its job's walk and build left). false:
	// on a job that doesn't end in a building (reclaim, repair, a fight, a flee), so no spot for it
	bool GetStart(CCircuitUnit* unit, const SHold* hold, springai::AIFloat3& outFrom, float& outBusy) const;
	std::vector<springai::AIFloat3> GetAllyAnchors() const;  // team games: allied AI bases and allied factories
	void Release(IBuilderTask* task);  // a job no longer held: dropped, and a builder walking to it goes idle
	void End();

	CCircuitAI* circuit;
	bool isOn;
	float minGain;  // seconds
	int holdFrames;
	std::vector<SHold> holds;
	std::vector<SSkip> skips;
	std::vector<CCircuitUnit*> toAssign;  // Update's AssignNext
	int matchFrame = -1;
	bool isDirty = true;
};

} // namespace circuit

#endif // SRC_CIRCUIT_MODULE_MEXBOARD_H_

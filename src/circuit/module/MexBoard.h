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
 * economy.mex_board, for the opening (CEconomyManager::IsOpening): builders are matched to mex spots by when each would
 * get there, instead of mex jobs being made one at a time near whichever builder picks. A builder holds one spot: the
 * one it walks to, or while it builds something else, its next. Its arrival at a spot is the time left on its job (and
 * on the energy the mex chain queues after a mex), plus the walk from that job's site (from where it stands between
 * jobs or walking to its spot), on its cost map when that started there, else in a straight line (a walker's map
 * started where it picked, so a walker measures straight). As the mex search, a builder rates only spots in the
 * mex_front.clusters nearest clusters (CMetalManager::FindNearSpots: its walk stops at threat walls). A builder walking
 * to its spot keeps it until the spot has failed its test for 3 s (a raider passing it doesn't turn the builder): the
 * pick's spot test with threat aside (CEconomyManager::GetMexSpotTest: an enemy or ally may build there meanwhile), the
 * builder's own danger rule (CBuilderManager::GetMexJobTest), and threat there within the builder's own (THREAT_MIN for
 * a constructor). Its job leaves threat at the spot to the board meanwhile (CBMexTask::IsReachable). A fleeing builder
 * holds no spot. The rest go by mex_front's score of the arrival (comm_bias for a commander), lowest first, so no
 * builder takes a spot another reaches sooner; a commander never takes a spot another free builder reaches sooner. A
 * next spot nobody walks to yet may move to another builder on any match, which costs nothing. A mex job is made only
 * when a builder is matched (no job cap), and no other builder takes it (CBMexTask::CanAssignTo). A builder takes a
 * spot only with no threat there, and only if its own picker would (reach, flee zones, threat, a hiding commander's
 * range and territory), and in team games not an ally's (CEconomyManager::GetTestedSpots). A job that ended without its
 * mex (unreachable, the build failed, a retreat from damage) keeps its spot from that builder for 30 s. Matching runs
 * once a second, and at once (at most once a frame) when a builder with no spot asks for a job, a builder starts
 * building its spot, a held job ends or a builder dies. When the opening ends, builders keep the job they walk to; the
 * next jobs nobody walks to are dropped
 */
class CMexBoard {
public:
	CMexBoard(CCircuitAI* circuit);

	bool IsActive() const;
	bool IsHolding() const { return !holds.empty(); }
	bool IsHeld(int spot) const;
	// The builder task is held for, nullptr = none: no other builder takes it, and its own walk there leaves threat at
	// the spot to the board (CBMexTask::CanAssignTo, IsReachable)
	const CCircuitUnit* GetHolder(const IBuilderTask* task) const;
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
		int failFrame;  // its walking builder's test first failed at, -1 = passes
	};
	struct SSkip {
		ICoreUnit::Id unitId;
		int spot;
		int until;  // frame
	};
	SHold* FindHold(const CCircuitUnit* unit);
	const SHold* FindHold(const IBuilderTask* task) const;
	void DropHold(const SHold* hold);  // erases it, and matches again

	// Match, in steps: the spots, each builder's start and pairs, the pick, then the jobs
	struct SMatch;  // MexBoard.cpp
	void Match(CCircuitUnit* asker, const CQueryCostMap* askerQuery);
	void MarkSpots(std::vector<bool>& outIsSpot) const;
	void RateBuilders(CCircuitUnit* asker, const CQueryCostMap* askerQuery, SMatch& m);
	void Pick(SMatch& m) const;
	void Apply(CCircuitUnit* asker, SMatch& m);
	// Where unit's walk to a spot would start, and the seconds before it can (its job's walk and build left). false:
	// on a job that doesn't end in a building (reclaim, repair, a fight, a flee), so no spot for it
	bool GetStart(CCircuitUnit* unit, const SHold* hold, springai::AIFloat3& outFrom, float& outBusy) const;
	void End();

	CCircuitAI* circuit;
	bool isOn;
	std::vector<SHold> holds;
	std::vector<SSkip> skips;
	std::vector<CCircuitUnit*> toAssign;  // Update's AssignNext
	int matchFrame = -1;
	bool isDirty = true;
};

} // namespace circuit

#endif // SRC_CIRCUIT_MODULE_MEXBOARD_H_

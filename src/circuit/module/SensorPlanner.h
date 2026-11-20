/*
 * SensorPlanner.h
 *
 *  Created on: Nov 11, 2026
 *      Author: Claude
 */

#ifndef SRC_CIRCUIT_MODULE_SENSORPLANNER_H_
#define SRC_CIRCUIT_MODULE_SENSORPLANNER_H_

#include "unit/CoreUnit.h"

#include "AIFloat3.h"

#include <cstdint>
#include <functional>
#include <map>
#include <vector>

namespace circuit {

class CCircuitAI;
class CCircuitDef;
class CCircuitUnit;
class IBuilderTask;

/*
 * choke.radar.enable: the radar planner (B), on CChokeMap's traffic grid. The ally team's authority AI keeps the map
 * every 15 s; every AI's builders take their radar spots from it.
 * - Weight: each cell any enemy movement type reaches weighs a floor plus its traffic, each scaled to add up to our
 *   mex income, plus the income of our clusters' mexes in it. Enemy ground (the enemy's half of the map, and the
 *   enemy's held ground on ours, CChokeMap::IsEnemyGround) weighs choke.radar.enemy x our mex income more, spread over its cells.
 * - Coverage: the union of the viewsheds of finished static radars, ours and allies', and of pending radar jobs
 *   (below). Mobile radar is ignored, so coverage doesn't flicker. Sight rays start half the model's height plus its sight emit height up.
 * - Candidates: the highest buildable cell of each 512-elmo block on our half of the map (not on the enemy's held
 *   ground), or on our held ground.
 * - A builder between jobs, with a factory up and no NOW job waiting, builds a radar nearby first (a NOW job, stalls
 *   or not) when its AI's budget holds one: choke.radar.share of its income, at most two radars' worth, starting
 *   with one. So radar follows the builders as they expand, with no walks for it. The spot is one of the candidates
 *   within 600 elmos or the highest buildable cell there: the one with the most uncovered weight in its viewshed
 *   for 1 + risk, if that is choke.radar.min_gain of the total weight (and, when the builder stands in coverage,
 *   its view is at least choke.radar.inside new by weight).
 *   It must be reachable and free of threat, and stand 500 elmos from other radars (closer, ISensorTask drops the
 *   job). Risk is 1 for each radar lost near it, halving every 5 minutes. A job its builder leaves is dropped
 *   after 3 s.
 * - choke.radar.guard: the radar job's next job is that tower, HIGH, at the spot scorer's best spot within
 *   choke.radar.guard_radius covering the radar's approaches (beside the radar if none passes), unless a tower of
 *   ours or an ally's already reaches the radar. It is queued once the radar stands, outside the radar budget and
 *   under the usual stall rule, and dropped if the radar is.
 * - choke.radar.heavy (the Advanced Radar): offered first to a builder between jobs that isn't a commander and
 *   stands within 1500 elmos of a spot choke.radar.heavy_depth from enemy ground, while the ally team's metal income
 *   (ZK's overdrive totals) is at least choke.radar.heavy_income, the type is available (its behaviour.json limit
 *   counts our units and queued jobs, so a lost one is rebuilt) and no heavy of the ally team was lost in the last
 *   90 s. Spots: the candidates within 1500 elmos at that depth, ranked by the uncovered weight in
 *   their range disc; scored by their viewshed for 1 + the radars lost within 1500 elmos, which must reach
 *   choke.radar.heavy_min_gain of the total weight. At most 4 new viewsheds per search, a search per AI every 10 s,
 *   and a log line a minute with the best score when none passes.
 * - choke.radar.chain: a build_chain hub "radar" step goes to the highest free spot within 300 elmos of its offset,
 *   if its view there is choke.radar.min_gain of the total weight, else the step is skipped; a step with
 *   "upgrade": true builds the heavy there instead when the heavy gates and score pass.
 * - Pending: every active radar job of the ally team's AIs (the planner's, a hub's, MakeDefence's), with its own
 *   type's viewshed, until the job ends (10 minutes at most). Coverage counts finished radars and pending jobs, so a
 *   nanoframe whose job was dropped doesn't hold an area dark; any radar, nanoframes too, keeps others 500 elmos off.
 * Viewsheds are cached while their cell is a candidate, a heavy candidate or a pending radar's
 */
class CSensorPlanner {
public:
	CSensorPlanner(CCircuitAI* circuit, CCircuitDef* radarDef, CCircuitDef* heavyDef);
	virtual ~CSensorPlanner();

	void Update();  // every 15 s; the authority's only
	// A builder of this AI between jobs: its radar, queued NOW, or nullptr. isSafe: may it go there (flee zones)
	IBuilderTask* MakeRadarTask(CCircuitUnit* unit, const std::function<bool(const springai::AIFloat3&)>& isSafe);
	// choke.radar.chain: a hub "radar" step of this AI asking for askDef near inOutPos (its offset spot), built by
	// builderDef. The def to build, at inOutPos (moved to the planner's spot), or nullptr to skip it. Planner off: askDef
	CCircuitDef* PlaceChainRadar(CCircuitDef* askDef, bool isUpgrade, CCircuitDef* builderDef,
			springai::AIFloat3& inOutPos);
	// The authority's: an active radar job of any ally AI starts or ends; its view counts as covered meanwhile
	void NotePending(const IBuilderTask* task);
	void EndPending(const IBuilderTask* task);
	CCircuitDef* GetHeavyDef() const { return heavyDef; }

	float GetCoveredShare() const { return (totalWeight > 0.f) ? coveredWeight / totalWeight : 1.f; }
	const std::vector<uint8_t>& GetCovered() const { return isCovered; }  // per traffic cell (choke_overlay)
	const std::vector<springai::AIFloat3>& GetCandidates() const { return candidates; }  // choke_overlay

private:
	struct SRadar {
		springai::AIFloat3 pos;
		std::vector<int> cells;  // its viewshed
		CCircuitDef* cdef;
		bool isBuilt;  // a finished unit: only those cover
	};
	struct SEvent {
		springai::AIFloat3 pos;
		int frame;
		CCircuitDef* cdef;
		const IBuilderTask* task;  // a pending radar's job, nullptr for a loss
	};
	// The authority's: the best spot near unit that isOk passes; its view is covered from now on. -RgtVector if none
	springai::AIFloat3 TakeSpot(CCircuitUnit* unit, const std::function<bool(const springai::AIFloat3&)>& isOk);
	// The authority's: the heavy's best spot near unit, as TakeSpot; outBest: the best score it saw (log)
	springai::AIFloat3 TakeHeavySpot(CCircuitUnit* unit, const std::function<bool(const springai::AIFloat3&)>& isOk,
			float& outBest);
	float ScoreHeavy(const springai::AIFloat3& pos);  // the authority's: its uncovered view for 1 + risk; -1 = no spot
	bool IsDeepReach(const springai::AIFloat3& pos) const;  // the authority's: a deep enough spot within HEAVY_REACH?
	// The authority's: the highest free cell within CHAIN_RADIUS of pos where cdef can be built, or -RgtVector
	springai::AIFloat3 FindChainSpot(CCircuitDef* cdef, const springai::AIFloat3& pos) const;
	bool IsHeavyReady(CCircuitDef* builderDef) const;  // this AI's: may builderDef start a heavy now
	IBuilderTask* MakeGuardTask(const springai::AIFloat3& radarPos);  // detached, or nullptr if none is needed
	void Cover(const std::vector<int>& cells);
	bool IsFree(const springai::AIFloat3& pos) const;  // no radar or pending one within RADAR_NEAR
	void UpdateWeights();
	void UpdateRadars();
	void UpdateCandidates();
	// Of the cells within [x0, x1] x [z0, z1] elmos that isIn passes, the first of the 4 highest where cdef builds
	springai::AIFloat3 FindHighest(CCircuitDef* cdef, float x0, float z0, float x1, float z1,
			const std::function<bool(const springai::AIFloat3&)>& isIn) const;
	const std::vector<int>& GetViewshed(const springai::AIFloat3& pos, CCircuitDef* cdef);  // cached
	bool IsCached(const springai::AIFloat3& pos, CCircuitDef* cdef) const;
	std::vector<int> MakeViewshed(const springai::AIFloat3& pos, CCircuitDef* cdef) const;  // of cdef's radar
	// The uncovered weight cdef's radar sees from pos, and its share of all it sees
	float GetGain(const springai::AIFloat3& pos, CCircuitDef* cdef, float& outShare);
	float GetDiscGain(const springai::AIFloat3& pos, float range) const;  // uncovered weight within range: an upper bound
	float GetRisk(const springai::AIFloat3& pos, float radius) const;  // radars lost within radius of it

	CCircuitAI* circuit;
	CCircuitDef* radarDef;
	CCircuitDef* heavyDef;  // choke.radar.heavy, or nullptr
	std::vector<float> weights;  // per traffic cell
	std::vector<float> enemyDist;  // per traffic cell: elmos to the nearest enemy ground cell
	float totalWeight;
	std::vector<uint8_t> isCovered;
	float coveredWeight;
	std::map<ICoreUnit::Id, SRadar> radars;  // known static radars
	std::vector<SEvent> deaths;  // radars lost
	std::vector<SEvent> pending;  // active radar jobs
	std::vector<springai::AIFloat3> candidates;
	std::vector<springai::AIFloat3> heavyCands;  // the last heavy search's scored candidates
	std::map<std::pair<int, int>, std::vector<int>> viewsheds;  // {cell, def id} -> cells seen from it
	float budget;  // metal this AI may still spend on radar
	int budgetFrame;  // budget last grew then
	int heavySearchFrame;  // a builder of this AI last looked for a heavy spot then
	int heavyLogFrame;  // ... and logged a failed search then
};

} // namespace circuit

#endif // SRC_CIRCUIT_MODULE_SENSORPLANNER_H_

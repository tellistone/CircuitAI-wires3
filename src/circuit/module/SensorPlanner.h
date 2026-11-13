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
 *   mex income, plus the income of our clusters' mexes in it. Enemy ground (the enemy's half of the map, and enemy
 *   territory on ours) weighs choke.radar.enemy x our mex income more, spread over its cells.
 * - Coverage: the union of the viewsheds of known static radars, ours and allies', nanoframes too; a radar a builder
 *   was sent to build counts until a radar stands within 500 elmos of it, or for 90 s. Mobile radar is ignored, so
 *   coverage doesn't flicker. Sight rays start half the model's height plus its sight emit height up.
 * - Candidates: the highest buildable cell of each 512-elmo block on our half of the map (not in enemy territory),
 *   or in our territory.
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
 * Viewsheds are cached while their cell is a candidate or a pending radar's
 */
class CSensorPlanner {
public:
	CSensorPlanner(CCircuitAI* circuit, CCircuitDef* radarDef);
	virtual ~CSensorPlanner();

	void Update();  // every 15 s; the authority's only
	// A builder of this AI between jobs: its radar, queued NOW, or nullptr. isSafe: may it go there (flee zones)
	IBuilderTask* MakeRadarTask(CCircuitUnit* unit, const std::function<bool(const springai::AIFloat3&)>& isSafe);

	float GetCoveredShare() const { return (totalWeight > 0.f) ? coveredWeight / totalWeight : 1.f; }
	const std::vector<uint8_t>& GetCovered() const { return isCovered; }  // per traffic cell (choke_overlay)
	const std::vector<springai::AIFloat3>& GetCandidates() const { return candidates; }  // choke_overlay

private:
	struct SRadar {
		springai::AIFloat3 pos;
		std::vector<int> cells;  // its viewshed
	};
	struct SEvent {
		springai::AIFloat3 pos;
		int frame;
	};
	// The authority's: the best spot near unit that isOk passes; its view is covered from now on. -RgtVector if none
	springai::AIFloat3 TakeSpot(CCircuitUnit* unit, const std::function<bool(const springai::AIFloat3&)>& isOk);
	IBuilderTask* MakeGuardTask(const springai::AIFloat3& radarPos);  // detached, or nullptr if none is needed
	void UpdateWeights();
	void UpdateRadars();
	void UpdateCandidates();
	// Of the cells within [x0, x1] x [z0, z1] elmos that isIn passes, the first buildable of the 4 highest
	springai::AIFloat3 FindHighest(float x0, float z0, float x1, float z1,
			const std::function<bool(const springai::AIFloat3&)>& isIn) const;
	const std::vector<int>& GetViewshed(const springai::AIFloat3& pos);  // cached
	std::vector<int> MakeViewshed(const springai::AIFloat3& pos, CCircuitDef* cdef) const;  // of cdef's radar
	float GetGain(const springai::AIFloat3& pos, float& outShare);  // the uncovered weight it sees, and its share of all
	float GetRisk(const springai::AIFloat3& pos) const;  // radars lost near it

	CCircuitAI* circuit;
	CCircuitDef* radarDef;
	std::vector<float> weights;  // per traffic cell
	float totalWeight;
	std::vector<uint8_t> isCovered;
	float coveredWeight;
	std::map<ICoreUnit::Id, SRadar> radars;  // known static radars
	std::vector<SEvent> deaths;  // radars lost
	std::vector<SEvent> pending;  // radars builders were sent to build
	std::vector<springai::AIFloat3> candidates;
	std::map<int, std::vector<int>> viewsheds;  // cell -> cells seen from it
	float budget;  // metal this AI may still spend on radar
	int budgetFrame;  // budget last grew then
};

} // namespace circuit

#endif // SRC_CIRCUIT_MODULE_SENSORPLANNER_H_

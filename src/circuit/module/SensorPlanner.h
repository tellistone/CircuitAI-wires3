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
 * choke.radar.enable: the radar planner (B), on CChokeMap's traffic grid, run by the ally team's authority AI only.
 * - Weight: each cell any enemy movement type reaches weighs a floor plus its traffic, each scaled to add up to our
 *   mex income, plus the income of our clusters' mexes in it. Enemy ground (the enemy's half of the map, and enemy
 *   territory on ours) weighs choke.radar.enemy x our mex income more, spread over its cells. So radar looks into
 *   enemy ground and at the approaches and our clusters first, then spreads over the rest.
 * - Coverage: the union of the viewsheds of known static radars, ours and allies', nanoframes too. Mobile radar is
 *   ignored, so coverage doesn't flicker as scouts fly past.
 * - Candidates: the highest buildable cell of each 512-elmo block on our half of the map (not in enemy territory),
 *   or in our territory.
 * - Every 15 s: the candidate with the most uncovered weight in its viewshed for its cost x (1 + risk) x (1 + the
 *   distance to our nearest builder / a quarter of the map's diagonal) is queued, if it adds at least
 *   choke.radar.min_gain of the total weight. It is a HIGH job until half the weight is covered, NORMAL after, and
 *   the budget starts with one radar, so the first ones go up in the opening. Risk is the threat there in main
 *   towers, plus 1 for each radar lost near it, halving every 5 minutes; a planned radar that no builder starts
 *   within 90 s, or that dies unfinished, counts as lost. At most one planned radar waits for a builder, and
 *   spending is capped at choke.radar.share of income.
 * - An idle constructor outside coverage gets a radar as a NOW job at the best of the candidates within 600 elmos of
 *   it and the highest buildable cell there, if that adds min_gain and has no threat on it (MakeIdleRadar, on any AI
 *   of the team: the authority's planner picks the spot).
 * A planned radar's view counts as covered until it is built or given up, so no second one goes there.
 * Candidates are tried best upper bound first (the weight in the square around them), at most 16 new viewsheds a
 * call; viewsheds are cached while their cell stays a candidate
 */
class CSensorPlanner {
public:
	CSensorPlanner(CCircuitAI* circuit, CCircuitDef* radarDef);
	virtual ~CSensorPlanner();

	void Update();  // every 15 s
	// An idle constructor of ours: a radar nearby if it stands outside the team's coverage, queued NOW for it
	IBuilderTask* MakeIdleRadar(CCircuitUnit* unit);
	// The authority's: the best candidate within reach of pos that isOk passes, when pos is outside coverage and the
	// candidate adds min_gain; its view counts as covered from now on. -RgtVector if none
	springai::AIFloat3 TakeIdleSpot(const springai::AIFloat3& pos, const std::function<bool(const springai::AIFloat3&)>& isOk);

	float GetCoveredShare() const { return (totalWeight > 0.f) ? coveredWeight / totalWeight : 1.f; }
	const std::vector<uint8_t>& GetCovered() const { return isCovered; }  // per traffic cell (choke_overlay)
	const std::vector<springai::AIFloat3>& GetCandidates() const { return candidates; }  // choke_overlay

private:
	struct SRadar {
		springai::AIFloat3 pos;
		std::vector<int> cells;  // its viewshed
	};
	struct SDeath {
		springai::AIFloat3 pos;
		int frame;
	};
	void AddPending(const springai::AIFloat3& pos);
	void UpdateWeights();
	void UpdateRadars();
	void UpdateCandidates();
	const std::vector<int>& GetViewshed(const springai::AIFloat3& pos);  // a candidate's, cached
	std::vector<int> MakeViewshed(const springai::AIFloat3& pos, float range, float emit) const;
	float GetRisk(const springai::AIFloat3& pos) const;

	CCircuitAI* circuit;
	CCircuitDef* radarDef;
	std::vector<float> weights;  // per traffic cell
	float totalWeight;
	std::vector<uint8_t> isCovered;
	float coveredWeight;
	std::map<ICoreUnit::Id, SRadar> radars;  // known static radars
	std::vector<SDeath> deaths;
	std::vector<SDeath> pending;  // planned radars not built yet, by the frame they were planned
	std::vector<springai::AIFloat3> candidates;
	std::map<int, std::vector<int>> viewsheds;  // candidate's cell -> cells it sees
	springai::AIFloat3 plannedPos;  // the last planned radar's, until its task is gone
	float budget;  // metal planned radars may still spend
	int lastFrame;
};

} // namespace circuit

#endif // SRC_CIRCUIT_MODULE_SENSORPLANNER_H_

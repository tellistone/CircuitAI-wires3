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
#include <map>
#include <vector>

namespace circuit {

class CCircuitAI;
class CCircuitDef;

/*
 * choke.radar.enable: the radar planner (B), on CChokeMap's traffic grid.
 * - Weight: each cell any enemy movement type reaches weighs a floor plus its traffic, each scaled to add up to our
 *   mex income, plus the income of our clusters' mexes in it. So radar goes to the approaches and our clusters
 *   first, then spreads over the rest. The target is choke.radar.coverage of the total weight.
 * - Coverage: the union of the viewsheds of known static radars, ours and allies'. Mobile radar is ignored, so
 *   coverage doesn't flicker as scouts fly past.
 * - Candidates: the highest buildable cell of each 512-elmo block on our side of the map or in our territory.
 * - Every 15 s while under target: the candidate with the most uncovered weight in its viewshed for its cost x
 *   (1 + risk) is queued. Risk is the threat there in main towers, plus 1 for each radar lost near it, halving every
 *   5 minutes. At most one planned radar waits for a builder, and spending is capped at choke.radar.share of income.
 * Candidates are tried best upper bound first (the weight in the square around them) and viewsheds are cached until
 * the terrain changes
 */
class CSensorPlanner {
public:
	CSensorPlanner(CCircuitAI* circuit, CCircuitDef* radarDef);
	virtual ~CSensorPlanner();

	void Update();  // every 15 s

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
	void UpdateWeights();
	void UpdateRadars();
	void UpdateCandidates();
	const std::vector<int>& GetViewshed(const springai::AIFloat3& pos);  // a candidate's, cached
	std::vector<int> MakeViewshed(const springai::AIFloat3& pos, float range, float emit) const;
	float GetRisk(const springai::AIFloat3& pos) const;
	float GetHeight(float x, float z) const;

	CCircuitAI* circuit;
	CCircuitDef* radarDef;
	std::vector<float> weights;  // per traffic cell
	float totalWeight;
	std::vector<uint8_t> isCovered;
	float coveredWeight;
	std::map<ICoreUnit::Id, SRadar> radars;  // known static radars
	std::vector<SDeath> deaths;
	std::vector<springai::AIFloat3> candidates;
	std::map<int, std::vector<int>> viewsheds;  // candidate's cell -> cells it sees
	int heightVersion;  // CTerrainData's when viewsheds were made
	float budget;  // metal planned radars may still spend
	int lastFrame;
};

} // namespace circuit

#endif // SRC_CIRCUIT_MODULE_SENSORPLANNER_H_

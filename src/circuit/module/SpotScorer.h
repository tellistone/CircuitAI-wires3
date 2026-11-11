/*
 * SpotScorer.h
 *
 *  Created on: Nov 11, 2026
 *      Author: Claude
 */

#ifndef SRC_CIRCUIT_MODULE_SPOTSCORER_H_
#define SRC_CIRCUIT_MODULE_SPOTSCORER_H_

#include "AIFloat3.h"

#include <map>
#include <vector>

namespace circuit {

class CCircuitAI;
class CCircuitDef;
struct STerrainMapArea;

/*
 * choke.spot.enable: one scorer for towers at a cluster, towers at a choke, base towers and army hold spots, with
 * KAIK's GetDefensePos terms on CChokeMap's traffic field:
 *   score = cover x (1 + 0.5 x height) x setback / (1 + threat at the spot / the def's own threat)
 *           / (1 + distance to our nearest builder / a quarter of the map's diagonal)
 * cover is the traffic of the targets in range with a clear line (halved for each of our towers already covering
 * one), height is the height above the targets (up to 150 elmos), setback (choke goals) the closeness to
 * choke.setback x range behind the line. Candidates are tried best upper bound first (traffic in range, from the
 * field's summed-area table) and the search stops when no bound can beat the best score
 */
class CSpotScorer {
public:
	enum class GoalType: char {CHOKE, POSITION, HOLD};
	struct SGoal {
		GoalType type = GoalType::POSITION;
		int choke = -1;  // CHOKE, HOLD: choke index in the threat graph
		int innerRegion = -1;  // CHOKE, HOLD: the region on our side of it
		springai::AIFloat3 asset;  // POSITION: what the spot guards (a defence point, the base)
		STerrainMapArea* area = nullptr;  // HOLD: the group's movement area (nullptr: any)
	};
	struct SCandidate {
		springai::AIFloat3 pos;
		float score;
	};

	CSpotScorer(CCircuitAI* circuit);
	virtual ~CSpotScorer();

	// The best spot for def within radius of pos, -RgtVector if none passes
	springai::AIFloat3 FindSpot(CCircuitDef* def, const springai::AIFloat3& pos, float radius, const SGoal& goal);
	// A build failed there: every search skips the spot's traffic cell for 5 minutes
	void MaskFailed(const springai::AIFloat3& pos);
	// Weight of the approach to pos within def's reach that our towers don't cover yet (the porc sweep's order)
	float GetUncovered(CCircuitDef* def, const springai::AIFloat3& pos, float radius);

	const std::vector<SCandidate>& GetLastCandidates() const { return lastCandidates; }  // choke_overlay

private:
	struct STarget {
		springai::AIFloat3 pos;  // on the ground
		float weight;
	};
	struct STower {
		springai::AIFloat3 pos;
		float range;
		float emit;
		bool isHighArc;
	};
	std::vector<STarget> MakeTargets(CCircuitDef* def, const springai::AIFloat3& pos, float radius, const SGoal& goal);
	std::vector<STower> GetTowers(const springai::AIFloat3& pos, float radius) const;  // ours and allies', queued too
	void HalveCovered(std::vector<STarget>& targets, const std::vector<STower>& towers) const;
	float GetLineDist(int choke, const springai::AIFloat3& pos) const;  // to the choke's line
	bool IsBuildable(CCircuitDef* def, const springai::AIFloat3& pos) const;
	bool IsClearLine(const springai::AIFloat3& from, float emit, const springai::AIFloat3& to) const;
	float GetHeight(float x, float z) const;
	bool IsMasked(const springai::AIFloat3& pos) const;

	CCircuitAI* circuit;
	std::map<int, int> failedCells;  // traffic cell -> frame the mask ends
	std::vector<SCandidate> lastCandidates;
};

} // namespace circuit

#endif // SRC_CIRCUIT_MODULE_SPOTSCORER_H_

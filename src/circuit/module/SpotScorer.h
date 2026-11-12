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
 *   score = cover x (1 + choke.spot.height x height) x (1 + choke.spot.enemy x enemy value / the def's cost)
 *           x setback x lean / (1 + threat at the spot / the def's own threat)
 *           / (1 + distance to our nearest builder / a quarter of the map's diagonal)
 * cover is the traffic of the targets in range with a clear line (halved for each of our towers already covering
 * one), height is the height above the targets as a share of choke.spot.height_cap (up to 1), setback (choke goals)
 * the closeness to choke.setback x range behind the line, enemy value the cost of known enemies in range and in a
 * clear line that it can hit, lean (position goals) 1 + choke.spot.forward x (1 + ahead) / 2, where ahead is how
 * much nearer the enemy walks to the spot than to the asset, as a share of the radius (-1 to 1). Hold spots skip
 * the builder term. Candidates stay on the
 * goal's side: a choke's inner region, or the asset's region, or a wall cell no enemy walks. They are tried best upper
 * bound first (the traffic in range times the terms that need no line of sight) and the search stops when no bound
 * can beat the best score
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

	// The best spot for def, -RgtVector if none passes: CHOKE, HOLD within choke.setback[1] + 0.3 ranges of the
	// choke's centre, POSITION within radius of the asset
	springai::AIFloat3 FindSpot(CCircuitDef* def, const SGoal& goal, float radius = 0.f);
	// A build failed there: every search skips the spot's traffic cell for 5 minutes
	void MaskFailed(const springai::AIFloat3& pos);
	// Per position, the weight of its approach within radius + def's range that our towers don't cover yet (the porc
	// sweep's order)
	std::vector<float> GetUncovered(CCircuitDef* def, const std::vector<springai::AIFloat3>& positions, float radius);

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
	std::vector<STarget> MakeTargets(CCircuitDef* def, const springai::AIFloat3& pos, float radius, const SGoal& goal,
			const std::vector<STower>& towers) const;
	std::vector<STower> GetTowers() const;  // ours and allies', queued too
	bool IsClearLine(const springai::AIFloat3& from, float emit, const springai::AIFloat3& to) const;
	bool IsMasked(const springai::AIFloat3& pos) const;

	CCircuitAI* circuit;
	std::map<int, int> failedCells;  // traffic cell -> frame the mask ends
	std::vector<SCandidate> lastCandidates;
};

} // namespace circuit

#endif // SRC_CIRCUIT_MODULE_SPOTSCORER_H_

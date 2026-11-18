/*
 * SpotScorer.h
 *
 *  Created on: Nov 11, 2026
 *      Author: Claude
 */

#ifndef SRC_CIRCUIT_MODULE_SPOTSCORER_H_
#define SRC_CIRCUIT_MODULE_SPOTSCORER_H_

#include "module/TurretValue.h"

#include "AIFloat3.h"

#include <map>
#include <vector>

namespace circuit {

class CCircuitAI;
class CCircuitDef;
struct STerrainMapArea;

/*
 * One scorer for towers at a cluster (choke.spot.enable), towers at a choke, base towers, radar guards and army hold
 * spots, on the turret picker's value model (CTurretValue) with each goal's own targets and factor:
 *   score = (T + E) x (1 + choke.spot.height x height) x (1 + choke.spot.view x V) x factor
 *   factor = setback x lean / (1 + threat at the spot / the def's own threat)
 *            / (1 + distance to our nearest builder / a quarter of the map's diagonal)
 * T is the goal's approach traffic in range with a clear line, in metal as the picker counts it (halved for each
 * tower already covering it that defends and hits it), E choke.spot.enemy x the picker's enemy targets (buildings seen
 * in the last 3 minutes; CTurretPicker::GetEnemyTargets) in range and in a clear line that it can hit, halved so, height the height above those targets as a share of choke.spot.height_cap (up to 1),
 * V the share of the range circle in a clear line of fire, setback (choke goals) the closeness to choke.setback x
 * range behind the line, lean (position goals) 1 + choke.spot.forward x (1 + ahead) / 2, where ahead (-1 to 1) is how
 * far the spot sits, per radius, toward where the asset's approach comes from. Hold spots skip the builder term and
 * E. Candidates stay on the goal's side: the home region (a choke's inner region, the asset's), a region the enemy
 * only walks into through it, or a wall cell no enemy walks. No cost threshold: a goal's tower goes on its best spot.
 * Big cannons have their own targets and terms on the same model (FindGunSpot)
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
	// A tower guarding a building of ours (the base's, a build chain hub's, a radar's): FindSpot's best POSITION spot
	// within radius of it; -RgtVector for AA or a def that isn't a defence (FindSpot values ground targets only)
	springai::AIFloat3 FindGuardSpot(CCircuitDef* def, const springai::AIFloat3& asset, float radius);
	// porcupine.superweapon.spot
	struct SGunConfig {
		float radius = 1000.f;  // elmos around the old site that are searched; 0: off
		float height = 0.5f;  // the most height above the targets adds (all of it at 150 elmos)
	};
	// A big cannon's spot (Big Bertha, the Disco Rave Party) within cfg.radius of centre, -RgtVector if none passes or
	// def isn't a cannon with a low arc (the .cpp has the rules)
	springai::AIFloat3 FindGunSpot(CCircuitDef* def, const springai::AIFloat3& centre, const SGunConfig& cfg);
	// A build failed there (or a turret picker job was dropped): every search skips its 64-elmo cell for 5 minutes
	void MaskFailed(const springai::AIFloat3& pos);
	// Per position, the weight of its approach within radius + def's range that our towers don't cover yet (the porc
	// sweep's order)
	std::vector<float> GetUncovered(CCircuitDef* def, const std::vector<springai::AIFloat3>& positions, float radius);

	const std::vector<SCandidate>& GetLastCandidates() const { return lastCandidates; }  // choke_overlay

	// Shared with CTurretPicker
	struct STower {
		springai::AIFloat3 pos;
		float range;
		float emit;
		bool isHighArc;
		CCircuitDef* def;  // CTurretPicker's filters
	};
	std::vector<STower> GetTowers() const;  // ours and allies', queued too
	bool IsClearLine(const springai::AIFloat3& from, float emit, const springai::AIFloat3& to) const;
	// A target on the ground (in deep water: under it too) that def's weapons reach
	static bool IsHit(const CCircuitDef* def, const springai::AIFloat3& pos) { return IsHitAt(def, pos.y); }
	static bool IsHitAt(const CCircuitDef* def, float elevation);
	static bool IsHit(const CCircuitDef* def, const CTurretValue::SPos& pos) { return IsHitAt(def, pos.y); }
	// CTurretValue's line of fire and V for def (high-arc: any line, V 1)
	CTurretValue::ClearFn MakeClearFn(const CCircuitDef* def) const;
	CTurretValue::ViewFn MakeViewFn(const CCircuitDef* def) const;
	bool IsMasked(const springai::AIFloat3& pos) const;  // MaskFailed's cell, still masked
	// Halves each target once per tower already covering it that defends and hits it (CTurretPicker's rule too)
	void HalveCovered(std::vector<CTurretValue::STarget>& targets, const std::vector<STower>& towers) const;

private:
	// The goal's traffic targets (metal: the raw traffic), halved, best MAX_TARGETS
	std::vector<CTurretValue::STarget> MakeTargets(CCircuitDef* def, const springai::AIFloat3& pos, float radius,
			const SGoal& goal, const std::vector<STower>& towers) const;
	// A builder of ours that can build def gets within build range of the spot, walking its own class's mask from
	// where it stands. Always true with isAnyReach, a builder that can build def and flies, or no builder counted
	std::function<bool (const springai::AIFloat3&)> MakeReachFn(CCircuitDef* def, bool isAnyReach) const;

	CCircuitAI* circuit;
	static int MaskKey(const springai::AIFloat3& pos);  // its MASK_CELL square
	std::map<int, int> failedCells;  // MaskKey -> frame the mask ends
	std::vector<SCandidate> lastCandidates;
};

} // namespace circuit

#endif // SRC_CIRCUIT_MODULE_SPOTSCORER_H_

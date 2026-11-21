/*
 * TurretValue.h
 *
 *  Created on: Nov 11, 2026
 *      Author: Claude
 */

#ifndef SRC_CIRCUIT_MODULE_TURRETVALUE_H_
#define SRC_CIRCUIT_MODULE_TURRETVALUE_H_

#include <functional>
#include <vector>

namespace circuit {

/*
 * CTurretPicker's value of a spot and its search, with no engine calls, so a harness can test them. In metal:
 *   base  = (T + A + E + C) x (1 + turret.height x height) + B
 *   value = base x (1 + turret.front x F) x (1 + turret.view x V) x factor / (1 + walk / travel)
 * T, A and E sum the targets of their kind in range with a clear line of fire; C is the best choke target so; height
 * is how far the spot stands above the mean height of those targets, per heightCap (up to 1); F the spot's
 * enemy building influence over the highest among the candidates; V the share of its range circle's ground, by area,
 * in a clear line of fire from it (a cliff top over the ground below scores high, its foot low); B the caller's own
 * metal for the spot, whatever is in range (CTurretPicker: turret.foremost). Only a spot whose
 * base reaches minBase is worth building; the most valuable of those wins
 */
class CTurretValue {
public:
	struct SPos {
		float x, y, z;  // y: ground height
	};
	// ECON: one of our economy buildings, in A unless a choke the spot counts carries our economy already
	enum class Kind: char {TRAFFIC, ALLY, ENEMY, CHOKE, ECON};
	struct STarget {
		SPos pos;
		float metal;  // weighted, and halved for each tower already covering it
		Kind kind;
		float econPart = 0.f;  // CHOKE: the share of metal that is our economy (turret.choke_econ)
	};
	struct SCandidate {
		SPos pos;
		float enemyInfl;  // F before it is normalised
		float walk;  // elmos from the ordered constructor
		float factor = 1.f;  // the caller's own terms for the spot, > 0 (CSpotScorer: setback, lean, threat, builder)
		float bonus = 0.f;  // B, metal added to the base
	};
	struct STerms {
		float traffic = 0.f, ally = 0.f, enemy = 0.f, choke = 0.f;  // T, A, E, C
		float height = 0.f;  // 0 to 1
		float front = 0.f;  // F, 0 to 1
		float view = 1.f;  // V, 0 to 1 (1 with no ViewFn)
		float bonus = 0.f;  // B
		float base = 0.f;
		float value = 0.f;
	};
	struct SScored {
		SCandidate cand;
		STerms terms;
		bool isWorth;  // base >= minBase
	};
	struct SParams {
		float range;  // the def's
		float heightBonus = 0.5f;  // turret.height
		float heightCap = 150.f;  // elmos above the targets that earn all of heightBonus
		float front = 1.f;  // turret.front
		float view = 1.f;  // turret.view
		float chokeDepth = 0.f;  // turret.choke_depth: C x (1 - this x the choke's distance / range)
		float travel = 1000.f;  // constructor walk that halves a spot's rank
		float minBase = 0.f;  // turret.min_value x the def's cost
		float refineStep = 16.f;  // elmos to each corner of the best few
	};
	// A clear line of fire from a spot to a target (always true for a high-arc def)
	using ClearFn = std::function<bool (const SPos& from, const SPos& to)>;
	// V of a spot, 0 to 1
	using ViewFn = std::function<float (const SPos& from)>;
	// A refined spot: false if it fails the hard rules, else out filled
	using MakeFn = std::function<bool (float x, float z, SCandidate& out)>;
	// The costly hard rules, run only on a candidate about to be tested: false if it fails them; may move it a little
	// (to the build grid). nullptr: none
	using FinishFn = std::function<bool (SCandidate& c)>;

	struct SResult {
		std::vector<SScored> scored;  // the spots tested, worth ones first, by value
		int tested = 0;
		int unfinished = 0;  // failed the costly rules
		float frontNorm = 0.f;  // the highest enemy influence among the candidates
		float bestBase = 0.f;  // the highest base tested, worth or not
		bool IsFound() const { return !scored.empty() && scored.front().isWorth; }
	};

	// viewOf: V, read only for a spot with a base (nullptr: V = 1 everywhere)
	// C of a choke target sqDist away: its metal, less the farther it is in range (turret.choke_depth, 0 to 1)
	static float ChokeWorth(float metal, float sqDist, float range, float depth);
	static STerms Evaluate(const SCandidate& c, const std::vector<STarget>& targets, const SParams& p, float frontNorm,
			const ClearFn& isClear, const ViewFn& viewOf = nullptr);
	// Best upper bound first: the targets in range with no line test, x the full height bonus, x the spot's own front,
	// walk and factor terms, x the full view bonus; stops once no bound beats the best worth value. A spot whose base can't reach minBase isn't
	// tested, nor finished. Then the best 5 are tried refineStep to each corner
	static SResult Search(const std::vector<SCandidate>& candidates, const std::vector<STarget>& targets,
			const SParams& p, const ClearFn& isClear, const MakeFn& makeAt, const FinishFn& finish = nullptr,
			const ViewFn& viewOf = nullptr);
	static float FrontNorm(const std::vector<SCandidate>& candidates);
};

} // namespace circuit

#endif // SRC_CIRCUIT_MODULE_TURRETVALUE_H_

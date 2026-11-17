/*
 * TurretValue.cpp
 *
 *  Created on: Nov 11, 2026
 *      Author: Claude
 */

#include "module/TurretValue.h"

#include <algorithm>

namespace circuit {

#define REFINE_COUNT	5  // best spots that get the corner pass

namespace {

inline float SqDist2D(const CTurretValue::SPos& a, const CTurretValue::SPos& b)
{
	return (a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z);
}

inline float LeanAndWalk(const CTurretValue::SCandidate& c, const CTurretValue::SParams& p, float frontNorm,
		float& outFront)
{
	outFront = (frontNorm > 0.f) ? std::min(c.enemyInfl / frontNorm, 1.f) : 0.f;
	return (1.f + std::max(p.front, 0.f) * outFront) * c.factor / (1.f + c.walk / std::max(p.travel, 1.f));
}

}  // namespace

CTurretValue::STerms CTurretValue::Evaluate(const SCandidate& c, const std::vector<STarget>& targets, const SParams& p,
		float frontNorm, const ClearFn& isClear, const ViewFn& viewOf)
{
	STerms t;
	const float sqRange = p.range * p.range;
	float heightSum = 0.f;
	int count = 0;
	for (const STarget& target : targets) {
		if ((target.metal <= 0.f) || (SqDist2D(c.pos, target.pos) > sqRange) || !isClear(c.pos, target.pos)) {
			continue;
		}
		switch (target.kind) {
			case Kind::TRAFFIC: t.traffic += target.metal; break;
			case Kind::ALLY: t.ally += target.metal; break;
			case Kind::ENEMY: t.enemy += target.metal; break;
			case Kind::CHOKE: t.choke = std::max(t.choke, target.metal); break;  // one choke's worth at most
		}
		heightSum += target.pos.y;
		++count;
	}
	if (count > 0) {
		const float above = c.pos.y - heightSum / count;
		t.height = std::min(std::max(above / std::max(p.heightCap, 1.f), 0.f), 1.f);
	}
	t.base = (t.traffic + t.ally + t.enemy + t.choke) * (1.f + std::max(p.heightBonus, 0.f) * t.height);
	// V ranks a spot, as F does, and never decides its worth; read only where there is a base to rank
	t.view = (viewOf && (t.base > 0.f)) ? std::min(std::max(viewOf(c.pos), 0.f), 1.f) : 1.f;
	t.value = t.base * LeanAndWalk(c, p, frontNorm, t.front) * (1.f + std::max(p.view, 0.f) * t.view);
	return t;
}

float CTurretValue::FrontNorm(const std::vector<SCandidate>& candidates)
{
	float norm = 0.f;
	for (const SCandidate& c : candidates) {
		norm = std::max(norm, c.enemyInfl);
	}
	return norm;
}

CTurretValue::SResult CTurretValue::Search(const std::vector<SCandidate>& candidates,
		const std::vector<STarget>& targets, const SParams& p, const ClearFn& isClear, const MakeFn& makeAt,
		const FinishFn& finish, const ViewFn& viewOf)
{
	SResult result;
	result.frontNorm = FrontNorm(candidates);
	const float sqRange = p.range * p.range;
	const float heightMax = 1.f + std::max(p.heightBonus, 0.f);
	const float viewMax = 1.f + std::max(p.view, 0.f);
	struct SBound {
		size_t idx;
		float bound;
	};
	std::vector<SBound> bounds;
	bounds.reserve(candidates.size());
	for (size_t i = 0; i < candidates.size(); ++i) {
		const SCandidate& c = candidates[i];
		float inRange = 0.f;
		float choke = 0.f;
		for (const STarget& target : targets) {
			if ((target.metal > 0.f) && (SqDist2D(c.pos, target.pos) <= sqRange)) {
				if (target.kind == Kind::CHOKE) {
					choke = std::max(choke, target.metal);
				} else {
					inRange += target.metal;
				}
			}
		}
		const float baseBound = (inRange + choke) * heightMax;
		if ((baseBound <= 0.f) || (baseBound < p.minBase)) {
			continue;  // never worth it, nor shown as tested
		}
		float front;
		bounds.push_back({i, baseBound * LeanAndWalk(c, p, result.frontNorm, front) * viewMax});
	}
	std::stable_sort(bounds.begin(), bounds.end(), [](const SBound& a, const SBound& b) {
		return a.bound > b.bound;
	});

	float bestValue = -1.f;
	auto test = [&](const SCandidate& c) {
		++result.tested;
		const STerms terms = Evaluate(c, targets, p, result.frontNorm, isClear, viewOf);
		const bool isWorth = (terms.base > 0.f) && (terms.base >= p.minBase);
		result.scored.push_back({c, terms, isWorth});
		result.bestBase = std::max(result.bestBase, terms.base);
		if (isWorth && (terms.value > bestValue)) {
			bestValue = terms.value;
		}
	};
	for (const SBound& b : bounds) {
		if (b.bound <= bestValue) {
			break;  // sorted by bound: nothing further can win
		}
		SCandidate c = candidates[b.idx];
		if (finish && !finish(c)) {
			++result.unfinished;
			continue;
		}
		test(c);
	}

	auto order = [](const SScored& a, const SScored& b) {
		return (a.isWorth != b.isWorth) ? a.isWorth : (a.terms.value > b.terms.value);
	};
	std::stable_sort(result.scored.begin(), result.scored.end(), order);
	const size_t refineCount = std::min<size_t>(result.scored.size(), REFINE_COUNT);
	std::vector<SCandidate> seeds;
	for (size_t i = 0; i < refineCount; ++i) {
		seeds.push_back(result.scored[i].cand);
	}
	const float q = p.refineStep;
	for (const SCandidate& seed : seeds) {
		for (const float dx : {-q, q}) {
			for (const float dz : {-q, q}) {
				SCandidate c;
				if (makeAt(seed.pos.x + dx, seed.pos.z + dz, c)) {
					test(c);
				}
			}
		}
	}
	std::stable_sort(result.scored.begin(), result.scored.end(), order);
	return result;
}

} // namespace circuit

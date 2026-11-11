/*
 * SpotScorer.cpp
 *
 *  Created on: Nov 11, 2026
 *      Author: Claude
 */

#include "module/SpotScorer.h"
#include "map/ThreatMap.h"
#include "module/BuilderManager.h"
#include "terrain/ChokeMap.h"
#include "terrain/TerrainData.h"
#include "terrain/TerrainManager.h"
#include "unit/ally/AllyTeam.h"
#include "unit/ally/AllyUnit.h"
#include "unit/CircuitDef.h"
#include "unit/CircuitUnit.h"
#include "CircuitAI.h"
#include "util/Defines.h"
#include "util/Point.h"
#include "util/Utils.h"

#include "spring/SpringMap.h"

#include "Log.h"
#include "UnitDef.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace circuit {

using namespace springai;

#define MASK_FRAMES		(FRAMES_PER_SEC * 300)  // a failed build site is skipped for 5 minutes
#define MAX_TARGETS		32
#define HEIGHT_CAP		150.f  // elmos above the targets that still add to the score
#define TARGET_LIFT		20.f  // sight rays end this high above a target's ground
#define REFINE_COUNT	5  // best coarse candidates that get the half-cell pass
#define MAX_CANDIDATES_SHOWN	200

CSpotScorer::CSpotScorer(CCircuitAI* circuit)
		: circuit(circuit)
{
}

CSpotScorer::~CSpotScorer()
{
}

AIFloat3 CSpotScorer::FindSpot(CCircuitDef* def, const AIFloat3& pos, float radius, const SGoal& goal)
{
	const auto t0 = std::chrono::steady_clock::now();
	lastCandidates.clear();
	const CChokeMap* chokeMap = circuit->GetAllyTeam()->GetChokeMap().get();
	if (!chokeMap->IsReady()) {
		return -RgtVector;
	}
	const CRegionGraph& graph = chokeMap->GetGraph();
	const CTrafficField& traffic = chokeMap->GetTraffic();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	const bool isChokeGoal = (goal.type != GoalType::POSITION);
	const float range = std::max(def->GetMaxRange(), (float)traffic.GetCellSize());
	const float emit = def->GetDef()->GetLosHeight();
	const bool isHighArc = (def->GetDef()->GetHighTrajectoryType() == 1);

	std::vector<STarget> targets = MakeTargets(def, pos, radius, goal);
	if (targets.empty()) {
		return -RgtVector;
	}
	float meanHeight = 0.f;
	for (const STarget& t : targets) {
		meanHeight += t.pos.y;
	}
	meanHeight /= targets.size();

	// Coarse candidates: traffic cell centres within radius, on our side of the choke, off lanes and failed sites
	const int outerRegion = isChokeGoal ? graph.OtherSide(graph.GetChokes()[goal.choke], goal.innerRegion) : -1;
	struct SCoarse {
		AIFloat3 pos;
		float bound;
	};
	std::vector<SCoarse> coarse;
	const int cellSize = traffic.GetCellSize();
	const int x0 = std::max(int(pos.x - radius) / cellSize, 0);
	const int x1 = std::min(int(pos.x + radius) / cellSize, traffic.GetWidth() - 1);
	const int z0 = std::max(int(pos.z - radius) / cellSize, 0);
	const int z1 = std::min(int(pos.z + radius) / cellSize, traffic.GetHeight() - 1);
	for (int z = z0; z <= z1; ++z) {
		for (int x = x0; x <= x1; ++x) {
			const AIFloat3 c((x + 0.5f) * cellSize, 0.f, (z + 0.5f) * cellSize);
			if ((c.SqDistance2D(pos) > SQUARE(radius)) || IsMasked(c)) {
				continue;
			}
			if (isChokeGoal && (graph.GetRegionAt(c.x, c.z) == outerRegion)) {
				continue;
			}
			if ((goal.type != GoalType::HOLD) && chokeMap->IsOnLane(c)) {
				continue;
			}
			// No score beats the weight of the targets in range times the best multiplier (height's 1.5)
			float bound = 0.f;
			for (const STarget& t : targets) {
				if (c.SqDistance2D(t.pos) <= SQUARE(range)) {
					bound += t.weight;
				}
			}
			coarse.push_back({c, 1.5f * bound});
		}
	}
	std::stable_sort(coarse.begin(), coarse.end(), [](const SCoarse& a, const SCoarse& b) {
		return a.bound > b.bound;
	});

	const std::set<CCircuitUnit*>& workers = circuit->GetBuilderManager()->GetWorkers();
	const int frame = circuit->GetLastFrame();
	const float quarterDiag = CTerrainManager::GetTerrainDiagonal() * 0.25f;
	const float ownThreat = std::max(def->GetThreat(), 1.f);
	AIFloat3 assetGround = goal.asset;
	assetGround.y = GetHeight(goal.asset.x, goal.asset.z);
	auto scoreAt = [&](AIFloat3 c) {
		c.y = GetHeight(c.x, c.z);
		if (goal.type == GoalType::HOLD) {
			if ((goal.area != nullptr) && !terrainMgr->CanMoveToPos(goal.area, c)) {
				return -1.f;
			}
		} else if (!IsBuildable(def, c)) {
			return -1.f;
		}
		if (goal.type == GoalType::POSITION) {
			// The asset itself must stay in reach and in sight
			if ((c.SqDistance2D(goal.asset) > SQUARE(0.8f * range)) || (!isHighArc && !IsClearLine(c, emit, assetGround))) {
				return -1.f;
			}
		}
		float cover = 0.f;
		for (const STarget& t : targets) {
			if ((c.SqDistance2D(t.pos) <= SQUARE(range)) && (isHighArc || IsClearLine(c, emit, t.pos))) {
				cover += t.weight;
			}
		}
		const float height = utils::clamp((c.y - meanHeight) / HEIGHT_CAP, 0.f, 1.f);
		float setback = 1.f;
		if (isChokeGoal) {
			const CChokeMap::SConfig& cfg = chokeMap->GetConfig();
			const float d = GetLineDist(goal.choke, c);
			const float lo = cfg.setback[0] * range;
			const float hi = cfg.setback[1] * range;
			if (d < lo) {
				setback = std::max(d / lo, 0.1f);
			} else if (d > hi) {
				setback = std::max(1.f - (d - hi) / (0.5f * range), 0.1f);
			}
		}
		const float threat = circuit->GetThreatMap()->GetAllThreatAt(c);
		float builderDist = 0.f;
		if (!workers.empty()) {
			float minSq = std::numeric_limits<float>::max();
			for (CCircuitUnit* w : workers) {
				minSq = std::min(minSq, w->GetPos(frame).SqDistance2D(c));
			}
			builderDist = std::sqrt(minSq);
		}
		return cover * (1.f + 0.5f * height) * setback / (1.f + threat / ownThreat) / (1.f + builderDist / quarterDiag);
	};

	float bestScore = -1.f;
	AIFloat3 bestPos = -RgtVector;
	std::vector<SCandidate> scored;
	int tested = 0;
	for (const SCoarse& cand : coarse) {
		if (cand.bound <= bestScore) {
			break;  // sorted by bound: nothing further can win
		}
		++tested;
		const float score = scoreAt(cand.pos);
		if (score < 0.f) {
			continue;
		}
		scored.push_back({cand.pos, score});
		if (score > bestScore) {
			bestScore = score;
			bestPos = cand.pos;
		}
	}
	// Half-cell pass around the best few
	std::stable_sort(scored.begin(), scored.end(), [](const SCandidate& a, const SCandidate& b) {
		return a.score > b.score;
	});
	const float q = cellSize * 0.25f;
	const size_t refineCount = std::min<size_t>(scored.size(), REFINE_COUNT);
	for (size_t i = 0; i < refineCount; ++i) {
		for (const AIFloat3& offset : {AIFloat3(-q, 0, -q), AIFloat3(q, 0, -q), AIFloat3(-q, 0, q), AIFloat3(q, 0, q)}) {
			const AIFloat3 c = scored[i].pos + offset;
			if ((c.SqDistance2D(pos) > SQUARE(radius)) || IsMasked(c)
				|| ((goal.type != GoalType::HOLD) && chokeMap->IsOnLane(c)))
			{
				continue;
			}
			++tested;
			const float score = scoreAt(c);
			if (score > bestScore) {
				bestScore = score;
				bestPos = c;
			}
		}
	}
	if (utils::is_valid(bestPos)) {
		bestPos.y = GetHeight(bestPos.x, bestPos.z);
	}
	scored.resize(std::min<size_t>(scored.size(), MAX_CANDIDATES_SHOWN));
	lastCandidates = std::move(scored);

	const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
	circuit->LOG("CHOKE spot %s: %i of %i candidates, %i targets, score %.2f, %.2f ms", def->GetDef()->GetName(),
			tested, (int)coarse.size(), (int)targets.size(), bestScore, ms);
	return bestPos;
}

void CSpotScorer::MaskFailed(const AIFloat3& pos)
{
	const CTrafficField& traffic = circuit->GetAllyTeam()->GetChokeMap()->GetTraffic();
	if (!traffic.IsEmpty()) {
		failedCells[traffic.IndexAt(pos.x, pos.z)] = circuit->GetLastFrame() + MASK_FRAMES;
	}
}

float CSpotScorer::GetUncovered(CCircuitDef* def, const AIFloat3& pos, float radius)
{
	if (!circuit->GetAllyTeam()->GetChokeMap()->IsReady()) {
		return 0.f;
	}
	SGoal goal;
	goal.asset = pos;
	float sum = 0.f;
	for (const STarget& t : MakeTargets(def, pos, radius, goal)) {
		sum += t.weight;
	}
	return sum;
}

/*
 * What a spot should cover, halved for each of our towers that already covers it, best 32 kept:
 * - a choke: its line, and a band one weapon range deep on the enemy's side, weighted by traffic;
 * - a position: its local approach (CTrafficField::GetApproachWeight), plus the asset itself with a floor weight
 *   (the mean target weight, or 1 with no traffic), so a spot near it is picked on height and sight alone
 */
std::vector<CSpotScorer::STarget> CSpotScorer::MakeTargets(CCircuitDef* def, const AIFloat3& pos, float radius,
		const SGoal& goal)
{
	const CChokeMap* chokeMap = circuit->GetAllyTeam()->GetChokeMap().get();
	const CRegionGraph& graph = chokeMap->GetGraph();
	const CTrafficField& traffic = chokeMap->GetTraffic();
	const float range = std::max(def->GetMaxRange(), (float)traffic.GetCellSize());
	const float reach = radius + range;
	const int cellSize = traffic.GetCellSize();
	const int x0 = std::max(int(pos.x - reach) / cellSize, 0);
	const int x1 = std::min(int(pos.x + reach) / cellSize, traffic.GetWidth() - 1);
	const int z0 = std::max(int(pos.z - reach) / cellSize, 0);
	const int z1 = std::min(int(pos.z + reach) / cellSize, traffic.GetHeight() - 1);

	std::vector<STarget> targets;
	const bool isChokeGoal = (goal.type != GoalType::POSITION);
	const int outerRegion = isChokeGoal ? graph.OtherSide(graph.GetChokes()[goal.choke], goal.innerRegion) : -1;
	const int assetCell = traffic.IndexAt(goal.asset.x, goal.asset.z);
	for (int z = z0; z <= z1; ++z) {
		for (int x = x0; x <= x1; ++x) {
			AIFloat3 c((x + 0.5f) * cellSize, 0.f, (z + 0.5f) * cellSize);
			if (c.SqDistance2D(pos) > SQUARE(reach)) {
				continue;
			}
			const int cell = z * traffic.GetWidth() + x;
			float weight = 0.f;
			if (isChokeGoal) {
				const float d = GetLineDist(goal.choke, c);
				if ((d <= cellSize) || ((d <= range) && (graph.GetRegionAt(c.x, c.z) == outerRegion))) {
					weight = traffic.GetAt(cell);
				}
			} else {
				weight = traffic.GetApproachWeight(cell, assetCell, c.distance2D(goal.asset));
			}
			if (weight > 0.f) {
				c.y = GetHeight(c.x, c.z);
				targets.push_back({c, weight});
			}
		}
	}
	auto keepBest = [&targets](size_t count) {
		std::stable_sort(targets.begin(), targets.end(), [](const STarget& a, const STarget& b) {
			return a.weight > b.weight;
		});
		if (targets.size() > count) {
			targets.resize(count);
		}
	};
	keepBest(MAX_TARGETS * 4);  // the line-of-fire tests below only for those that may make the best 32
	HalveCovered(targets, GetTowers(pos, reach));
	keepBest(MAX_TARGETS);
	if (!isChokeGoal) {
		float floor = 1.f;
		if (!targets.empty()) {
			floor = 0.f;
			for (const STarget& t : targets) {
				floor += t.weight;
			}
			floor /= targets.size();
		}
		AIFloat3 asset = goal.asset;
		asset.y = GetHeight(asset.x, asset.z);
		targets.push_back({asset, floor});
	}
	return targets;
}

/*
 * Every tower of our ally team in reach (human allies' too), and our queued tower tasks not yet started (a started
 * one has its nanoframe in the list already), so each counts once from queue to death
 */
std::vector<CSpotScorer::STower> CSpotScorer::GetTowers(const AIFloat3& pos, float radius) const
{
	std::vector<STower> towers;
	const int frame = circuit->GetLastFrame();
	auto add = [&towers, &pos, radius](CCircuitDef* cdef, const AIFloat3& p) {
		if ((cdef != nullptr) && !cdef->IsMobile() && cdef->IsAttacker()
			&& (p.SqDistance2D(pos) <= SQUARE(radius + cdef->GetMaxRange())))
		{
			towers.push_back({p, cdef->GetMaxRange(), cdef->GetDef()->GetLosHeight(),
					cdef->GetDef()->GetHighTrajectoryType() == 1});
		}
	};
	for (const auto& kv : circuit->GetFriendlyUnits()) {
		add(kv.second->GetCircuitDef(), kv.second->GetPos(frame));
	}
	for (const IBuilderTask* task : circuit->GetBuilderManager()->GetTasks(IBuilderTask::BuildType::DEFENCE)) {
		if (task->GetTarget() == nullptr) {
			add(task->GetBuildDef(), task->GetPosition());
		}
	}
	return towers;
}

void CSpotScorer::HalveCovered(std::vector<STarget>& targets, const std::vector<STower>& towers) const
{
	for (STarget& t : targets) {
		for (const STower& tower : towers) {
			if ((tower.pos.SqDistance2D(t.pos) <= SQUARE(tower.range))
				&& (tower.isHighArc || IsClearLine(tower.pos, tower.emit, t.pos)))
			{
				t.weight *= 0.5f;
			}
		}
	}
}

float CSpotScorer::GetLineDist(int choke, const AIFloat3& pos) const
{
	const CChokeMap* chokeMap = circuit->GetAllyTeam()->GetChokeMap().get();
	const SCellMask& mask = chokeMap->GetGraph().GetMask();
	float bestSq = std::numeric_limits<float>::max();
	for (int cell : chokeMap->GetGraph().GetChokes()[choke].line) {
		const float dx = mask.CentreX(cell) - pos.x;
		const float dz = mask.CentreZ(cell) - pos.z;
		bestSq = std::min(bestSq, dx * dx + dz * dz);
	}
	return std::sqrt(bestSq);
}

bool CSpotScorer::IsBuildable(CCircuitDef* def, const AIFloat3& pos) const
{
	return circuit->GetTerrainManager()->CanBeBuiltAt(def, pos)
			&& circuit->GetMap()->IsPossibleToBuildAt(def->GetDef(), pos, UNIT_FACING_SOUTH);
}

/*
 * Terrain blocks both sight and fire: a ray from the spot's ground plus the def's sight emit height to the target's
 * ground plus 20 elmos, on the heightmap, two squares a step
 */
bool CSpotScorer::IsClearLine(const AIFloat3& from, float emit, const AIFloat3& to) const
{
	const float y0 = GetHeight(from.x, from.z) + emit;
	const float y1 = GetHeight(to.x, to.z) + TARGET_LIFT;
	const int steps = int(from.distance2D(to) / (SQUARE_SIZE * 2));
	for (int i = 1; i < steps; ++i) {
		const float f = float(i) / steps;
		const float x = from.x + (to.x - from.x) * f;
		const float z = from.z + (to.z - from.z) * f;
		if (GetHeight(x, z) > y0 + (y1 - y0) * f) {
			return false;
		}
	}
	return true;
}

float CSpotScorer::GetHeight(float x, float z) const
{
	const FloatVec& heightMap = circuit->GetTerrainManager()->GetAreaData()->heightMap;
	const int width = circuit->GetMap()->GetWidth();
	const int height = circuit->GetMap()->GetHeight();
	const int hx = utils::clamp(int(x) / SQUARE_SIZE, 0, width - 1);
	const int hz = utils::clamp(int(z) / SQUARE_SIZE, 0, height - 1);
	return heightMap[hz * width + hx];
}

bool CSpotScorer::IsMasked(const AIFloat3& pos) const
{
	const CTrafficField& traffic = circuit->GetAllyTeam()->GetChokeMap()->GetTraffic();
	auto it = failedCells.find(traffic.IndexAt(pos.x, pos.z));
	return (it != failedCells.end()) && (circuit->GetLastFrame() < it->second);
}

} // namespace circuit

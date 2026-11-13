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
#include "terrain/TerrainManager.h"
#include "unit/ally/AllyUnit.h"
#include "unit/enemy/EnemyUnit.h"
#include "unit/CircuitDef.h"
#include "unit/CircuitUnit.h"
#include "CircuitAI.h"
#include "util/Defines.h"
#include "util/Point.h"
#include "util/Utils.h"

#include "Log.h"
#include "UnitDef.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace circuit {

using namespace springai;

#define MASK_FRAMES		(FRAMES_PER_SEC * 300)  // a failed build site is skipped for 5 minutes
#define MAX_TARGETS		32
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

AIFloat3 CSpotScorer::FindSpot(CCircuitDef* def, const SGoal& goal, float radius)
{
	const auto t0 = std::chrono::steady_clock::now();
	lastCandidates.clear();
	const CChokeMap* chokeMap = circuit->GetChokeMap();
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
	const CRegionGraph::SChoke* choke = isChokeGoal ? &graph.GetChokes()[goal.choke] : nullptr;
	AIFloat3 pos = goal.asset;
	if (isChokeGoal) {
		pos = chokeMap->GetCellPos(choke->centre);
		radius = (chokeMap->GetConfig().setback[1] + 0.3f) * range;
	}

	const std::vector<STarget> targets = MakeTargets(def, pos, radius, goal, GetTowers());
	if (targets.empty()) {
		return -RgtVector;
	}
	float meanHeight = 0.f;
	for (const STarget& t : targets) {
		meanHeight += t.pos.y;
	}
	meanHeight /= targets.size();

	// The terms without a line of sight: setback, the lean toward the enemy, threat and the builder's distance
	const std::set<CCircuitUnit*>& workers = circuit->GetBuilderManager()->GetWorkers();
	const int frame = circuit->GetLastFrame();
	const float quarterDiag = CTerrainManager::GetTerrainDiagonal() * 0.25f;
	const float ownThreat = std::max(def->GetThreat(), 1.f);
	const CChokeMap::SConfig& cfg = chokeMap->GetConfig();
	const float heightBonus = std::max(cfg.spotHeight, 0.f);
	const float heightCap = std::max(cfg.spotHeightCap, 1.f);
	// choke.spot.enemy: known enemies a tower could shoot, each worth its cost per the tower's own. Buildings by their
	// last known position, mobiles while in radar or sight. As threatened spots are skipped, an armed one only counts
	// where the tower outranges it
	std::vector<STarget> enemies;
	if ((goal.type != GoalType::HOLD) && (cfg.spotEnemy > 0.f)) {
		for (const auto& kv : circuit->GetEnemyInfos()) {
			const CEnemyInfo* e = kv.second;
			const CCircuitDef* edef = e->GetCircuitDef();
			if (e->IsHidden() || (edef == nullptr) || (edef->IsMobile() && !e->IsInRadarOrLOS())
				|| !(edef->IsAbleToFly() ? def->HasAntiAir() : def->HasAntiLand())
				|| (e->GetPos().SqDistance2D(pos) > SQUARE(radius + range)))
			{
				continue;
			}
			enemies.push_back({e->GetPos(), cfg.spotEnemy * e->GetCost() / std::max(def->GetCostM(), 1.f)});
		}
	}
	auto enemyAt = [&](const AIFloat3& c, bool isLine) {  // 1 + their worth in range (and in a clear line)
		float value = 1.f;
		for (const STarget& e : enemies) {
			if ((c.SqDistance2D(e.pos) <= SQUARE(range)) && (!isLine || isHighArc || IsClearLine(c, emit, e.pos))) {
				value += e.weight;
			}
		}
		return value;
	};
	// choke.spot.forward: a position's towers lean the way its approach comes from. pull: the targets' weighted offsets
	// from the asset; a spot's ahead (-1 to 1) is its offset along pull, per radius, times how one-sided the approach
	// is (|pull| over the targets' weighted distance), so 0 for an approach from all sides
	const float spotForward = (goal.type == GoalType::POSITION) ? std::max(cfg.spotForward, 0.f) : 0.f;
	AIFloat3 pull = ZeroVector;
	float spread = 0.f;
	for (const STarget& t : targets) {
		pull += (t.pos - pos) * t.weight;
		spread += t.pos.distance2D(pos) * t.weight;
	}
	const float aheadScale = ((spread > 0.f) && (radius > 0.f)) ? 1.f / (radius * spread) : 0.f;
	auto leanAt = [&](const AIFloat3& c) {
		const float ahead = ((c.x - pos.x) * pull.x + (c.z - pos.z) * pull.z) * aheadScale;
		return 1.f + spotForward * 0.5f * (1.f + ahead);
	};
	auto factorAt = [&](const AIFloat3& c) {
		float setback = 1.f;
		if (isChokeGoal) {
			const float d = graph.GetLineDist(*choke, c.x, c.z);
			const float lo = cfg.setback[0] * range;
			const float hi = cfg.setback[1] * range;
			if (d < lo) {
				setback = std::max(d / lo, 0.1f);
			} else if (d > hi) {
				setback = std::max(1.f - (d - hi) / (0.5f * range), 0.1f);
			}
		}
		float builderDist = 0.f;
		if ((goal.type != GoalType::HOLD) && !workers.empty()) {
			float minSq = std::numeric_limits<float>::max();
			for (CCircuitUnit* w : workers) {
				minSq = std::min(minSq, w->GetPos(frame).SqDistance2D(c));
			}
			builderDist = std::sqrt(minSq);
		}
		return setback * leanAt(c) / (1.f + circuit->GetThreatMap()->GetAllThreatAt(c) / ownThreat)
				/ (1.f + builderDist / quarterDiag);
	};
	// On the goal's side: the home region (a choke's inner region, the asset's), a region the enemy only walks into
	// through it (high ground off it), or a wall cell no enemy walks
	const int homeRegion = isChokeGoal ? goal.innerRegion : chokeMap->GetRegionAt(goal.asset);
	const std::vector<bool> isHomeSide = chokeMap->GetShelteredBy(homeRegion);
	auto isCandidate = [&](const AIFloat3& c) {
		const int region = graph.GetRegionAt(c.x, c.z);
		return (c.SqDistance2D(pos) <= SQUARE(radius)) && !IsMasked(c)
				&& ((homeRegion < 0) || (region == CRegionGraph::NO_REGION) || isHomeSide[region])
				&& ((goal.type == GoalType::HOLD) || !chokeMap->IsOnLane(c));
	};
	AIFloat3 assetGround = goal.asset;
	assetGround.y = terrainMgr->GetHeightAt(goal.asset.x, goal.asset.z);
	auto scoreAt = [&](AIFloat3 c, float factor) {
		c.y = terrainMgr->GetHeightAt(c.x, c.z);
		if (goal.type == GoalType::HOLD) {
			if ((goal.area != nullptr) && !terrainMgr->CanMoveToPos(goal.area, c)) {
				return -1.f;
			}
		} else if ((circuit->GetThreatMap()->GetAllThreatAt(c) > THREAT_MIN) || !terrainMgr->CanBuildAt(def, c)) {
			return -1.f;  // a builder only builds out of threat (FindBuildSite): it would move such a tower elsewhere
		}
		// A position's asset itself must stay in reach and in sight
		if ((goal.type == GoalType::POSITION) && ((c.SqDistance2D(goal.asset) > SQUARE(0.8f * range))
			|| (!isHighArc && !IsClearLine(c, emit, assetGround))))
		{
			return -1.f;
		}
		float cover = 0.f;
		for (const STarget& t : targets) {
			if ((c.SqDistance2D(t.pos) <= SQUARE(range)) && (isHighArc || IsClearLine(c, emit, t.pos))) {
				cover += t.weight;
			}
		}
		const float height = utils::clamp((c.y - meanHeight) / heightCap, 0.f, 1.f);
		return cover * (1.f + heightBonus * height) * factor * enemyAt(c, true);
	};

	// Coarse candidates: traffic cell centres. No score beats the targets in range x factor x height's best x the
	// enemies in range
	struct SCoarse {
		AIFloat3 pos;
		float factor;
		float bound;
	};
	std::vector<SCoarse> coarse;
	const int cellSize = traffic.GetCellSize();
	ForEachCellIn(traffic.GetWidth(), traffic.GetHeight(), cellSize, pos.x - radius, pos.z - radius,
			pos.x + radius, pos.z + radius, [&](int x, int z) {
		const AIFloat3 c((x + 0.5f) * cellSize, 0.f, (z + 0.5f) * cellSize);
		if (!isCandidate(c)) {
			return;
		}
		float inRange = 0.f;
		for (const STarget& t : targets) {
			inRange += (c.SqDistance2D(t.pos) <= SQUARE(range)) ? t.weight : 0.f;
		}
		const float factor = (inRange > 0.f) ? factorAt(c) : 0.f;
		const float bound = (factor > 0.f) ? (1.f + heightBonus) * inRange * factor * enemyAt(c, false) : 0.f;
		coarse.push_back({c, factor, bound});
	});
	std::stable_sort(coarse.begin(), coarse.end(), [](const SCoarse& a, const SCoarse& b) {
		return a.bound > b.bound;
	});

	float bestScore = -1.f;
	AIFloat3 bestPos = -RgtVector;
	std::vector<SCandidate> scored;
	int tested = 0;
	for (const SCoarse& cand : coarse) {
		if (cand.bound <= bestScore) {
			break;  // sorted by bound: nothing further can win
		}
		++tested;
		const float score = scoreAt(cand.pos, cand.factor);
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
			if (!isCandidate(c)) {
				continue;
			}
			++tested;
			const float score = scoreAt(c, factorAt(c));
			if (score > bestScore) {
				bestScore = score;
				bestPos = c;
			}
		}
	}
	if (utils::is_valid(bestPos)) {
		bestPos.y = terrainMgr->GetHeightAt(bestPos.x, bestPos.z);
	}
	scored.resize(std::min<size_t>(scored.size(), MAX_CANDIDATES_SHOWN));
	lastCandidates = std::move(scored);

	const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
	const float lean = utils::is_valid(bestPos) ? leanAt(bestPos) : 1.f;
	circuit->LOG("CHOKE spot %s: %i of %i candidates, %i targets, %i enemies, lean %.2f, score %.2f, %.2f ms",
			def->GetDef()->GetName(), tested, (int)coarse.size(), (int)targets.size(), (int)enemies.size(), lean, bestScore,
			ms);
	return bestPos;
}

void CSpotScorer::MaskFailed(const AIFloat3& pos)
{
	const CTrafficField& traffic = circuit->GetChokeMap()->GetTraffic();
	if (!traffic.IsEmpty()) {
		failedCells[traffic.IndexAt(pos.x, pos.z)] = circuit->GetLastFrame() + MASK_FRAMES;
	}
}

std::vector<float> CSpotScorer::GetUncovered(CCircuitDef* def, const std::vector<AIFloat3>& positions, float radius)
{
	std::vector<float> sums(positions.size(), 0.f);
	if (!circuit->GetChokeMap()->IsReady()) {
		return sums;
	}
	const std::vector<STower> towers = GetTowers();
	SGoal goal;
	for (size_t i = 0; i < positions.size(); ++i) {
		goal.asset = positions[i];
		for (const STarget& t : MakeTargets(def, positions[i], radius, goal, towers)) {
			sums[i] += t.weight;
		}
	}
	return sums;
}

/*
 * What a spot should cover, halved for each of our towers that already covers it, best 32 kept:
 * - a choke: its line, and a band one weapon range deep on the enemy's side, weighted by traffic;
 * - a position: its own approach (CTrafficField::GetApproach, walked up to twice the reach), plus the asset itself
 *   with a floor weight (the mean target weight, or 1 with no traffic), so a spot near it is picked on height and
 *   sight alone
 */
std::vector<CSpotScorer::STarget> CSpotScorer::MakeTargets(CCircuitDef* def, const AIFloat3& pos, float radius,
		const SGoal& goal, const std::vector<STower>& towers) const
{
	const CChokeMap* chokeMap = circuit->GetChokeMap();
	const CRegionGraph& graph = chokeMap->GetGraph();
	const CTrafficField& traffic = chokeMap->GetTraffic();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	const float range = std::max(def->GetMaxRange(), (float)traffic.GetCellSize());
	const float reach = radius + range;
	const int cellSize = traffic.GetCellSize();
	const bool isChokeGoal = (goal.type != GoalType::POSITION);
	const CRegionGraph::SChoke* choke = isChokeGoal ? &graph.GetChokes()[goal.choke] : nullptr;
	const int outerRegion = isChokeGoal ? graph.OtherSide(*choke, goal.innerRegion) : -1;

	std::vector<STarget> targets;
	auto add = [&](int x, int z, float weight) {
		AIFloat3 c((x + 0.5f) * cellSize, 0.f, (z + 0.5f) * cellSize);
		if ((weight > 0.f) && (c.SqDistance2D(pos) <= SQUARE(reach))) {
			c.y = terrainMgr->GetHeightAt(c.x, c.z);
			targets.push_back({c, weight});
		}
	};
	if (isChokeGoal) {
		ForEachCellIn(traffic.GetWidth(), traffic.GetHeight(), cellSize, pos.x - reach, pos.z - reach,
				pos.x + reach, pos.z + reach, [&](int x, int z) {
			const float cx = (x + 0.5f) * cellSize;
			const float cz = (z + 0.5f) * cellSize;
			const float d = graph.GetLineDist(*choke, cx, cz);
			if ((d <= cellSize) || ((d <= range) && (graph.GetRegionAt(cx, cz) == outerRegion))) {
				add(x, z, traffic.GetAt(z * traffic.GetWidth() + x));
			}
		});
	} else {
		for (const std::pair<int, float>& cw : traffic.GetApproach(traffic.IndexAt(goal.asset.x, goal.asset.z),
				2.f * reach))
		{
			add(cw.first % traffic.GetWidth(), cw.first / traffic.GetWidth(), cw.second);
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
	for (STarget& t : targets) {
		for (const STower& tower : towers) {
			if ((tower.pos.SqDistance2D(t.pos) <= SQUARE(tower.range))
				&& (tower.isHighArc || IsClearLine(tower.pos, tower.emit, t.pos)))
			{
				t.weight *= 0.5f;
			}
		}
	}
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
		asset.y = terrainMgr->GetHeightAt(asset.x, asset.z);
		targets.push_back({asset, floor});
	}
	return targets;
}

/*
 * Every tower of our ally team (human allies' too), and our queued tower tasks not yet started, chained ones too (a
 * started one has its nanoframe in the list already), so each counts once from queue to death
 */
std::vector<CSpotScorer::STower> CSpotScorer::GetTowers() const
{
	std::vector<STower> towers;
	const int frame = circuit->GetLastFrame();
	auto add = [&towers](CCircuitDef* cdef, const AIFloat3& p) {
		if ((cdef != nullptr) && !cdef->IsMobile() && cdef->IsAttacker()) {
			towers.push_back({p, cdef->GetMaxRange(), cdef->GetDef()->GetLosHeight(),
					cdef->GetDef()->GetHighTrajectoryType() == 1});
		}
	};
	for (const auto& kv : circuit->GetFriendlyUnits()) {
		add(kv.second->GetCircuitDef(), kv.second->GetPos(frame));
	}
	for (IBuilderTask* task : circuit->GetBuilderManager()->GetTasks(IBuilderTask::BuildType::DEFENCE)) {
		for (IBuilderTask* t = task; t != nullptr; t = t->GetNextTask()) {  // a porc chain's waiting towers too
			if (t->GetTarget() == nullptr) {
				add(t->GetBuildDef(), t->GetPosition());
			}
		}
	}
	return towers;
}

/*
 * Terrain blocks both sight and fire: a ray from the spot's ground plus the def's sight emit height to the target's
 * ground plus 20 elmos, on the heightmap, two squares a step
 */
bool CSpotScorer::IsClearLine(const AIFloat3& from, float emit, const AIFloat3& to) const
{
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	const float y0 = terrainMgr->GetHeightAt(from.x, from.z) + emit;
	const float y1 = terrainMgr->GetHeightAt(to.x, to.z) + TARGET_LIFT;
	const int steps = int(from.distance2D(to) / (SQUARE_SIZE * 2));
	for (int i = 1; i < steps; ++i) {
		const float f = float(i) / steps;
		const float x = from.x + (to.x - from.x) * f;
		const float z = from.z + (to.z - from.z) * f;
		if (terrainMgr->GetHeightAt(x, z) > y0 + (y1 - y0) * f) {
			return false;
		}
	}
	return true;
}

bool CSpotScorer::IsMasked(const AIFloat3& pos) const
{
	const CTrafficField& traffic = circuit->GetChokeMap()->GetTraffic();
	auto it = failedCells.find(traffic.IndexAt(pos.x, pos.z));
	return (it != failedCells.end()) && (circuit->GetLastFrame() < it->second);
}

} // namespace circuit

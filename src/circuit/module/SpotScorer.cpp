/*
 * SpotScorer.cpp
 *
 *  Created on: Nov 11, 2026
 *      Author: Claude
 */

#include "module/SpotScorer.h"
#include "map/ThreatMap.h"
#include "module/BuilderManager.h"
#include "module/MilitaryManager.h"
#include "module/TurretPicker.h"
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
#define MASK_CELL		64.f  // elmos: the traffic field's cell, and the turret picker's candidate cell
#define MAX_TARGETS		32
#define TARGET_LIFT		20.f  // sight rays end this high above a target's ground
#define VIEW_RAYS		24  // bearings V samples
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
	const CTurretPicker* picker = circuit->GetMilitaryManager()->GetTurretPicker();

	// The terms without a line of sight: setback, the lean toward the enemy, threat and the builder's distance
	const std::set<CCircuitUnit*>& workers = circuit->GetBuilderManager()->GetWorkers();
	const int frame = circuit->GetLastFrame();
	const float quarterDiag = CTerrainManager::GetTerrainDiagonal() * 0.25f;
	const float ownThreat = std::max(def->GetThreat(), 1.f);
	const CChokeMap::SConfig& cfg = chokeMap->GetConfig();
	// The turret picker's value model (CTurretValue): the approach's traffic in metal, as the picker counts it, and
	// choke.spot.enemy x the cost of known enemies a tower could shoot (E). Buildings by their last known position,
	// mobiles while in radar or sight. As threatened spots are skipped, an armed one only counts where the tower
	// outranges it
	std::vector<CTurretValue::STarget> values;
	for (const STarget& t : targets) {
		const CTurretValue::SPos tp = {t.pos.x, t.pos.y, t.pos.z};
		if ((goal.type == GoalType::HOLD) || IsHit(def, t.pos)) {  // a hold's group guards the choke whatever it hits
			values.push_back({tp, picker->TrafficMetal(t.weight, range), CTurretValue::Kind::TRAFFIC});
		}
	}
	int enemyCount = 0;
	if ((goal.type != GoalType::HOLD) && (cfg.spotEnemy > 0.f)) {
		for (const auto& kv : circuit->GetEnemyInfos()) {
			const CEnemyInfo* e = kv.second;
			const CCircuitDef* edef = e->GetCircuitDef();
			if (e->IsHidden() || (edef == nullptr) || (edef->IsMobile() ? !e->IsInRadarOrLOS() : !picker->IsEnemyBuilding(e, false))
				|| !(edef->IsAbleToFly() ? def->HasAntiAir() : def->HasAntiLand())
				|| (e->GetPos().SqDistance2D(pos) > SQUARE(radius + range)))
			{
				continue;
			}
			const AIFloat3& ep = e->GetPos();
			values.push_back({{ep.x, terrainMgr->GetHeightAt(ep.x, ep.z), ep.z}, cfg.spotEnemy * e->GetCost(),
					CTurretValue::Kind::ENEMY});
			++enemyCount;
		}
	}
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
	// A tower only where a builder of ours that can build it gets within build range, walking its own class's mask
	// from where it stands: else no one takes the job, and it waits 5 minutes, holding up the porc chain behind it,
	// before it is dropped. A builder that flies lifts the test; one of a class no factory builds isn't counted, and
	// with none counted there's no test
	struct SWalker {
		int classIdx;
		int part;
		float range;
	};
	std::vector<SWalker> walkers;
	bool isAnyReach = (goal.type == GoalType::HOLD);  // a hold's group checks its own way there
	for (CCircuitUnit* w : workers) {
		CCircuitDef* wdef = w->GetCircuitDef();
		if (isAnyReach || !wdef->CanBuild(def)) {
			continue;
		}
		if (wdef->IsAbleToFly()) {
			isAnyReach = true;
			continue;
		}
		const int classIdx = chokeMap->GetWalkClass(wdef);
		if (classIdx < 0) {
			continue;
		}
		const std::vector<int>& parts = chokeMap->GetClassParts(classIdx);
		if (parts.empty()) {
			continue;
		}
		const SCellMask& mask = chokeMap->GetClassMask(classIdx);
		const AIFloat3 wpos = w->GetPos(frame);
		const int cell = mask.NearestPassable(mask.IndexAt(wpos.x, wpos.z), 2);
		const SWalker walker = {classIdx, (cell >= 0) ? parts[cell] : -1, wdef->GetBuildDistance() + SQUARE_SIZE * 2};
		if ((walker.part >= 0) && std::none_of(walkers.begin(), walkers.end(), [&walker](const SWalker& o) {
			return (o.classIdx == walker.classIdx) && (o.part == walker.part) && (o.range >= walker.range);
		})) {
			walkers.push_back(walker);
		}
	}
	auto isInReach = [&](const AIFloat3& c) {
		if (isAnyReach || walkers.empty()) {
			return true;
		}
		return std::any_of(walkers.begin(), walkers.end(), [&](const SWalker& walker) {
			const SCellMask& mask = chokeMap->GetClassMask(walker.classIdx);
			const std::vector<int>& parts = chokeMap->GetClassParts(walker.classIdx);
			bool isNear = false;
			ForEachCellIn(mask.width, mask.height, mask.cellSize, c.x - walker.range, c.z - walker.range,
					c.x + walker.range, c.z + walker.range, [&](int x, int z) {
				const int i = z * mask.width + x;
				isNear = isNear || ((parts[i] == walker.part)
						&& (SQUARE(mask.CentreX(i) - c.x) + SQUARE(mask.CentreZ(i) - c.z) <= SQUARE(walker.range)));
			});
			return isNear;
		});
	};
	AIFloat3 assetGround = goal.asset;
	assetGround.y = terrainMgr->GetHeightAt(goal.asset.x, goal.asset.z);
	// The costly rules, only on a candidate about to be scored
	auto isFinished = [&](const CTurretValue::SCandidate& vc) {
		const AIFloat3 c(vc.pos.x, vc.pos.y, vc.pos.z);
		if (goal.type == GoalType::HOLD) {
			if ((goal.area != nullptr) && !terrainMgr->CanMoveToPos(goal.area, c)) {
				return false;
			}
		} else if ((circuit->GetThreatMap()->GetAllThreatAt(c) > THREAT_MIN) || !terrainMgr->CanBuildAt(def, c)
			|| !isInReach(c))
		{
			return false;  // a builder only builds out of threat (FindBuildSite), and only where it gets in range
		}
		// A position's asset itself must stay in reach and in sight
		return (goal.type != GoalType::POSITION) || ((c.SqDistance2D(goal.asset) <= SQUARE(0.8f * range))
			&& (isHighArc || IsClearLine(c, emit, assetGround)));
	};
	auto makeAt = [&](float x, float z, CTurretValue::SCandidate& out) {
		const AIFloat3 c(x, terrainMgr->GetHeightAt(x, z), z);
		if (!isCandidate(c)) {
			return false;
		}
		out.pos = {c.x, c.y, c.z};
		out.enemyInfl = 0.f;
		out.walk = 0.f;
		out.factor = factorAt(c);
		return true;
	};

	// Candidates: the traffic cell centres, best upper bound first (CTurretValue::Search), then the best few a quarter
	// cell to each corner
	const int cellSize = traffic.GetCellSize();
	std::vector<CTurretValue::SCandidate> candidates;
	ForEachCellIn(traffic.GetWidth(), traffic.GetHeight(), cellSize, pos.x - radius, pos.z - radius,
			pos.x + radius, pos.z + radius, [&](int x, int z) {
		CTurretValue::SCandidate c;
		if (makeAt((x + 0.5f) * cellSize, (z + 0.5f) * cellSize, c)) {
			candidates.push_back(c);
		}
	});
	CTurretValue::SParams params;
	params.range = range;
	params.heightBonus = cfg.spotHeight;
	params.heightCap = cfg.spotHeightCap;
	params.front = 0.f;  // the lean is the factor's
	params.view = cfg.spotView;
	params.minBase = 0.f;  // a goal's tower goes up wherever is best
	params.refineStep = cellSize * 0.25f;
	const CTurretValue::SResult result = CTurretValue::Search(candidates, values, params, MakeClearFn(def),
			[&](float x, float z, CTurretValue::SCandidate& out) { return makeAt(x, z, out) && isFinished(out); },
			isFinished, MakeViewFn(def));

	AIFloat3 bestPos = -RgtVector;
	float bestScore = -1.f;
	float bestView = 0.f;
	if (result.IsFound()) {
		const CTurretValue::SScored& best = result.scored.front();
		bestPos = AIFloat3(best.cand.pos.x, terrainMgr->GetHeightAt(best.cand.pos.x, best.cand.pos.z), best.cand.pos.z);
		bestScore = best.terms.value;
		bestView = best.terms.view;
	}
	std::vector<SCandidate> scored;
	for (const CTurretValue::SScored& sc : result.scored) {
		if (sc.isWorth && (scored.size() < MAX_CANDIDATES_SHOWN)) {
			scored.push_back({AIFloat3(sc.cand.pos.x, sc.cand.pos.y, sc.cand.pos.z), sc.terms.value});
		}
	}
	lastCandidates = std::move(scored);

	const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
	const float lean = utils::is_valid(bestPos) ? leanAt(bestPos) : 1.f;
	circuit->LOG("CHOKE spot %s: %i of %i candidates, %i targets, %i enemies, lean %.2f, view %.2f, score %.0f, %.2f ms",
			def->GetDef()->GetName(), result.tested, (int)candidates.size(), (int)targets.size(), enemyCount, lean,
			bestView, bestScore, ms);
	return bestPos;
}

void CSpotScorer::MaskFailed(const AIFloat3& pos)
{
	failedCells[MaskKey(pos)] = circuit->GetLastFrame() + MASK_FRAMES;  // the traffic field may not be built yet
}

int CSpotScorer::MaskKey(const AIFloat3& pos)
{
	return int(pos.z / MASK_CELL) * 65536 + int(pos.x / MASK_CELL);
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
	// As the turret picker halves: only by a tower that defends (CTurretPicker::IsDefence) and hits the target
	const CTurretPicker* picker = circuit->GetMilitaryManager()->GetTurretPicker();
	for (STarget& t : targets) {
		for (const STower& tower : towers) {
			if (picker->IsDefence(tower.def) && IsHit(tower.def, t.pos)
				&& (tower.pos.SqDistance2D(t.pos) <= SQUARE(tower.range))
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
					cdef->GetDef()->GetHighTrajectoryType() == 1, cdef});
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

bool CSpotScorer::IsHit(const CCircuitDef* def, const AIFloat3& pos)
{
	return def->HasAntiLand() || (CTerrainManager::IsDeepWater(pos.y) && def->HasAntiWater());
}

CTurretValue::ClearFn CSpotScorer::MakeClearFn(const CCircuitDef* def) const
{
	if (def->GetDef()->GetHighTrajectoryType() == 1) {
		return [](const CTurretValue::SPos&, const CTurretValue::SPos&) { return true; };  // high-arc: any line
	}
	const float emit = def->GetDef()->GetLosHeight();
	return [this, emit](const CTurretValue::SPos& from, const CTurretValue::SPos& to) {
		return IsClearLine(AIFloat3(from.x, from.y, from.z), emit, AIFloat3(to.x, to.y, to.z));
	};
}

/*
 * V: the share of the range circle's ground, by area, in a clear line of fire from the spot. Along each of VIEW_RAYS
 * bearings, out every 2 squares to the range, a point is in line while it rises above the steepest ground before it,
 * as IsClearLine tests one line; points off the map don't count. Water reads at its surface
 */
CTurretValue::ViewFn CSpotScorer::MakeViewFn(const CCircuitDef* def) const
{
	if (def->GetDef()->GetHighTrajectoryType() == 1) {
		return nullptr;  // high-arc: any line
	}
	const float emit = def->GetDef()->GetLosHeight();
	const float range = std::max(def->GetMaxRange(), 1.f);
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	std::vector<std::pair<float, float>> dirs;
	for (int r = 0; r < VIEW_RAYS; ++r) {
		const float angle = 2.f * static_cast<float>(M_PI) * r / VIEW_RAYS;
		dirs.push_back({std::cos(angle), std::sin(angle)});
	}
	return [terrainMgr, emit, range, dirs](const CTurretValue::SPos& from) {
		const float y0 = terrainMgr->GetHeightAt(from.x, from.z) + emit;
		const float step = SQUARE_SIZE * 2;
		const float maxX = CTerrainManager::GetTerrainWidth();
		const float maxZ = CTerrainManager::GetTerrainHeight();
		float seen = 0.f;
		float total = 0.f;
		for (const auto& [dx, dz] : dirs) {
			float steepest = -std::numeric_limits<float>::max();
			for (float d = step; d <= range; d += step) {
				const float x = from.x + dx * d;
				const float z = from.z + dz * d;
				if ((x < 0.f) || (z < 0.f) || (x > maxX) || (z > maxZ)) {
					break;
				}
				const float ground = terrainMgr->GetHeightAt(x, z);
				total += d;  // area: a point's ring grows with d
				if ((std::max(ground, 0.f) + TARGET_LIFT - y0) / d >= steepest) {
					seen += d;
				}
				steepest = std::max(steepest, (ground - y0) / d);
			}
		}
		return (total > 0.f) ? seen / total : 0.f;
	};
}

bool CSpotScorer::IsMasked(const AIFloat3& pos) const
{
	auto it = failedCells.find(MaskKey(pos));
	return (it != failedCells.end()) && (circuit->GetLastFrame() < it->second);
}

} // namespace circuit

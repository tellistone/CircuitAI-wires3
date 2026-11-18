/*
 * SpotScorer.cpp
 *
 *  Created on: Nov 11, 2026
 *      Author: Claude
 */

#include "module/SpotScorer.h"
#include "map/ThreatMap.h"
#include "map/InfluenceMap.h"
#include "module/BuilderManager.h"
#include "module/EconomyManager.h"
#include "module/MilitaryManager.h"
#include "module/TurretPicker.h"
#include "resource/MetalManager.h"
#include "task/static/PriorityArtyTask.h"
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
// FindGunSpot
#define GUN_CELL		128  // elmos between candidates
#define GUN_TARGETS		48  // the best by metal
#define GUN_FREE_SPOT	0.5f  // a metal spot on the enemy's half with no mex: this x its income x turret.horizon
#define GUN_SPOT_TAKEN	48.f  // an enemy building this near a metal spot is its mex
#define GUN_HEIGHT_CAP	150.f  // elmos above the targets that earn all of spot.height

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

	const std::vector<STower> towers = GetTowers();
	const std::vector<CTurretValue::STarget> targets = MakeTargets(def, pos, radius, goal, towers);
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
	// The turret picker's value model (CTurretValue): the approach's traffic in metal (as the picker counts it, at
	// weight 1), and E, choke.spot.enemy x the picker's enemy targets (known enemy buildings seen in the last 3
	// minutes, a mex at its income x turret.horizon), halved as the traffic is. Mobiles don't count: their routes are
	// the approach, and a spot that freezes once queued mustn't follow a passing raid
	std::vector<CTurretValue::STarget> values;
	for (const CTurretValue::STarget& t : targets) {
		if ((goal.type == GoalType::HOLD) || IsHit(def, t.pos)) {  // a hold's group guards the choke whatever it hits
			values.push_back({t.pos, picker->TrafficMetal(t.metal, range, 1.f), CTurretValue::Kind::TRAFFIC});
		}
	}
	int enemyCount = 0;
	if ((goal.type != GoalType::HOLD) && (cfg.spotEnemy > 0.f)) {
		std::vector<CTurretValue::STarget> enemies = picker->GetEnemyTargets(pos, radius + range);
		HalveCovered(enemies, towers);
		for (CTurretValue::STarget& e : enemies) {
			if (IsHit(def, e.pos)) {
				e.metal *= cfg.spotEnemy;
				values.push_back(e);
				++enemyCount;
			}
		}
	}
	// The turret picker's A (our unarmed buildings in range) and C (line chokes in range), as an order counts them, and
	// its F (the front) below. A hold spot is the army's, not a tower's: none of them
	std::vector<AIFloat3> enemyBuildings;
	if (goal.type != GoalType::HOLD) {
		CTurretPicker::SSpotShare share = picker->GetSpotShare(def, pos, radius);
		values.insert(values.end(), share.targets.begin(), share.targets.end());
		enemyBuildings = std::move(share.enemyBuildings);
	}
	// choke.spot.forward: a position's towers lean the way its approach comes from. pull: the targets' weighted offsets
	// from the asset; a spot's ahead (-1 to 1) is its offset along pull, per radius, times how one-sided the approach
	// is (|pull| over the targets' weighted distance), so 0 for an approach from all sides
	const float spotForward = (goal.type == GoalType::POSITION) ? std::max(cfg.spotForward, 0.f) : 0.f;
	AIFloat3 pull = ZeroVector;
	float spread = 0.f;
	for (const CTurretValue::STarget& t : targets) {
		const AIFloat3 offset(t.pos.x - pos.x, 0.f, t.pos.z - pos.z);
		pull += offset * t.metal;
		spread += offset.Length2D() * t.metal;
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
	// A tower only where a builder of ours that can build it gets within build range: else no one takes the job, and
	// it waits 5 minutes, holding up the porc chain behind it, before it is dropped. A hold's group checks its own way
	const std::function<bool (const AIFloat3&)> isInReach = MakeReachFn(def, goal.type == GoalType::HOLD);
	AIFloat3 assetGround = goal.asset;
	assetGround.y = terrainMgr->GetHeightAt(goal.asset.x, goal.asset.z);
	// The costly rules, only on a candidate about to be scored
	// The costly rules, only on a candidate about to be scored. A tower's spot moves onto its build site: buildable
	// facing south and open on the blocking map (lanes, queued jobs), as the turret picker's (FindSiteAt)
	auto isFinished = [&](CTurretValue::SCandidate& vc) {
		AIFloat3 c(vc.pos.x, vc.pos.y, vc.pos.z);
		if (goal.type == GoalType::HOLD) {
			return (goal.area == nullptr) || terrainMgr->CanMoveToPos(goal.area, c);
		}
		c = terrainMgr->FindSiteAt(def, c, UNIT_FACING_SOUTH);
		if (!utils::is_valid(c) || !isCandidate(c) || (circuit->GetThreatMap()->GetAllThreatAt(c) > THREAT_MIN)
			|| !isInReach(c))
		{
			return false;  // a builder only builds out of threat (FindBuildSite), and only where it gets in range
		}
		// A position's asset itself must stay in reach and in sight
		c.y = terrainMgr->GetHeightAt(c.x, c.z);
		if ((goal.type == GoalType::POSITION) && ((c.SqDistance2D(goal.asset) > SQUARE(0.8f * range))
			|| (!isHighArc && !IsClearLine(c, emit, assetGround))))
		{
			return false;
		}
		vc.pos = {c.x, c.y, c.z};
		vc.enemyInfl = picker->GetEnemyInfl(enemyBuildings, c);
		vc.factor = factorAt(c);
		return true;
	};
	auto makeAt = [&](float x, float z, CTurretValue::SCandidate& out) {
		const AIFloat3 c(x, terrainMgr->GetHeightAt(x, z), z);
		if (!isCandidate(c)) {
			return false;
		}
		out.pos = {c.x, c.y, c.z};
		out.enemyInfl = picker->GetEnemyInfl(enemyBuildings, c);
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
	params.front = (goal.type == GoalType::HOLD) ? 0.f : picker->GetConfig().front;  // turret.front
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

AIFloat3 CSpotScorer::FindGuardSpot(CCircuitDef* def, const AIFloat3& asset, float radius)
{
	if (def->IsRoleAA() || !circuit->GetMilitaryManager()->GetTurretPicker()->IsDefence(def)) {
		return -RgtVector;
	}
	SGoal goal;
	goal.asset = asset;
	return FindSpot(def, goal, radius);
}

/*
 * A big cannon's spot, on the turret picker's value model (CTurretValue) with no front or view term:
 *   value = E x (1 + spot.height x height) / (1 + distance from centre / spot.radius)
 * E sums its targets in reach (the height-adjusted Cannon range) with a clear low shell arc, simulated as
 * CPriorityArtyTask aims: the enemy buildings we know of (seen in the last 3 minutes, a mex at its income x
 * turret.horizon), and the metal spots on the enemy's half with no mex we know of at half a mex's, the best
 * GUN_TARGETS of them by metal; height its height above them as a share of 150 elmos (up to 1). Candidates every
 * GUN_CELL within spot.radius of centre: our territory, off the lanes, out of threat, not masked, buildable
 * (FindSiteAt), where a builder of ours gets in range and (a grid consumer) in pylon reach of a grid with enough energy
 */
AIFloat3 CSpotScorer::FindGunSpot(CCircuitDef* def, const AIFloat3& centre, const SGunConfig& cfg)
{
	const CChokeMap* chokeMap = circuit->GetChokeMap();
	ballistics::SShell shell;
	const float range = def->GetMaxRange(CCircuitDef::RangeType::LAND);
	if ((cfg.radius <= 0.f) || (range <= 0.f) || !chokeMap->IsReady()
		|| (def->GetDef()->GetHighTrajectoryType() == 1)  // high arc only: any spot clears
		|| !CPriorityArtyTask::ReadShell(def, shell) || !shell.isCannon)
	{
		return -RgtVector;
	}
	const auto t0 = std::chrono::steady_clock::now();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	auto groundAt = [terrainMgr](float x, float z) {
		return std::max(terrainMgr->GetHeightAt(x, z), 0.f);  // shells fly over water, and burst on it
	};
	auto isClear = [&](const CTurretValue::SPos& from, const CTurretValue::SPos& to) {
		return ballistics::IsShotClear(from.x, std::max(from.y, 0.f) + ballistics::AIM_FROM_HEIGHT, from.z, to.x,
				std::max(to.y, 0.f) + ballistics::AIM_HEIGHT, to.z, range, shell, groundAt);
	};

	// Candidates
	CInfluenceMap* inflMap = circuit->GetInflMap();
	CThreatMap* threatMap = circuit->GetThreatMap();
	CEconomyManager* economyMgr = circuit->GetEconomyManager();
	const bool isConsumer = economyMgr->IsGridConsumer(def);
	auto isCandidate = [&](const AIFloat3& c) {
		return (c.SqDistance2D(centre) <= SQUARE(cfg.radius)) && !IsMasked(c) && !chokeMap->IsOnLane(c)
			&& inflMap->IsAllyTerritory(c, INFL_EPS) && (threatMap->GetAllThreatAt(c) <= THREAT_MIN);
	};
	auto makeAt = [&](float x, float z, CTurretValue::SCandidate& out) {
		const AIFloat3 c(x, terrainMgr->GetHeightAt(x, z), z);
		if (!isCandidate(c)) {
			return false;
		}
		out = {{c.x, c.y, c.z}, 0.f, c.distance2D(centre)};
		return true;
	};
	const std::function<bool (const AIFloat3&)> isInReach = MakeReachFn(def, false);
	// The costly rules: the spot moves onto its build site (buildable facing south, open on the blocking map)
	auto isFinished = [&](CTurretValue::SCandidate& vc) {
		const AIFloat3 c = terrainMgr->FindSiteAt(def, AIFloat3(vc.pos.x, vc.pos.y, vc.pos.z), UNIT_FACING_SOUTH);
		if (!utils::is_valid(c) || !isCandidate(c) || !isInReach(c)
			|| (isConsumer && !economyMgr->IsGridReachable(def, c)))
		{
			return false;
		}
		vc.pos = {c.x, terrainMgr->GetHeightAt(c.x, c.z), c.z};
		vc.walk = c.distance2D(centre);
		return true;
	};
	std::vector<CTurretValue::SCandidate> candidates;
	float topY = 0.f;
	ForEachCellIn(CTerrainManager::GetTerrainWidth() / GUN_CELL, CTerrainManager::GetTerrainHeight() / GUN_CELL,
			GUN_CELL, centre.x - cfg.radius, centre.z - cfg.radius, centre.x + cfg.radius, centre.z + cfg.radius,
			[&](int x, int z) {
		CTurretValue::SCandidate c;
		if (makeAt((x + 0.5f) * GUN_CELL, (z + 0.5f) * GUN_CELL, c)) {
			candidates.push_back(c);
			topY = std::max(topY, c.pos.y);
		}
	});
	if (candidates.empty()) {
		return -RgtVector;
	}
	// The farthest any target can be: from the highest candidate down to the water
	const float rangeMax = ballistics::CannonRange(range, -(topY + ballistics::AIM_FROM_HEIGHT) * shell.heightMod,
			shell.speed, shell.gravity) * ballistics::AIM_RANGE_MOD;

	// Targets
	const CTurretPicker* picker = circuit->GetMilitaryManager()->GetTurretPicker();
	const float reach = cfg.radius + rangeMax;
	std::vector<CTurretValue::STarget> targets;
	for (const CTurretValue::STarget& t : picker->GetEnemyTargets(centre, reach)) {
		if (IsHit(def, t.pos)) {
			targets.push_back(t);
		}
	}
	const size_t enemyCount = targets.size();
	CMetalManager* metalMgr = circuit->GetMetalManager();
	const CMetalData::Metals& spots = metalMgr->GetSpots();
	const float spotMetal = picker->GetConfig().horizon * GUN_FREE_SPOT;
	for (unsigned i = 0; i < spots.size(); ++i) {
		const AIFloat3& p = spots[i].position;
		if (!metalMgr->IsOpenSpot(i) || (p.SqDistance2D(centre) > SQUARE(reach)) || chokeMap->IsOurHalf(p)
			|| std::any_of(targets.begin(), targets.begin() + enemyCount, [&p](const CTurretValue::STarget& t) {
				return SQUARE(t.pos.x - p.x) + SQUARE(t.pos.z - p.z) < SQUARE(GUN_SPOT_TAKEN);
			}))
		{
			continue;  // ours, out of reach, on our half, or a mex we know of (counted)
		}
		const CTurretValue::SPos sp = {p.x, terrainMgr->GetHeightAt(p.x, p.z), p.z};
		if (IsHit(def, sp)) {
			targets.push_back({sp, spots[i].income * spotMetal, CTurretValue::Kind::ENEMY});
		}
	}
	if (targets.size() > GUN_TARGETS) {
		std::nth_element(targets.begin(), targets.begin() + GUN_TARGETS, targets.end(),
				[](const CTurretValue::STarget& a, const CTurretValue::STarget& b) { return a.metal > b.metal; });
		targets.resize(GUN_TARGETS);
	}
	if (targets.empty()) {
		return -RgtVector;
	}

	CTurretValue::SParams params;
	params.range = rangeMax;  // a bound: isClear tests the real reach
	params.heightBonus = cfg.height;
	params.heightCap = GUN_HEIGHT_CAP;
	params.front = 0.f;
	params.view = 0.f;
	params.travel = cfg.radius;
	params.refineStep = GUN_CELL * 0.25f;
	const CTurretValue::SResult result = CTurretValue::Search(candidates, targets, params, isClear,
			[&](float x, float z, CTurretValue::SCandidate& out) { return makeAt(x, z, out) && isFinished(out); },
			isFinished);

	const bool isFound = result.IsFound();
	const CTurretValue::SScored* best = isFound ? &result.scored.front() : nullptr;
	const AIFloat3 bestPos = isFound ? AIFloat3(best->cand.pos.x, best->cand.pos.y, best->cand.pos.z) : -RgtVector;
	const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
	circuit->LOG("BIG GUN spot %s: %i of %i candidates, %i targets (%i enemies), %s, E %.0f, height %.2f, %.0f up on "
			"the old site, %.2f ms", def->GetDef()->GetName(), result.tested, (int)candidates.size(), (int)targets.size(),
			(int)enemyCount, isFound ? "found" : "none", isFound ? best->terms.enemy : 0.f,
			isFound ? best->terms.height : 0.f, isFound ? bestPos.y - terrainMgr->GetHeightAt(centre.x, centre.z) : 0.f, ms);
	return bestPos;
}

/*
 * A builder that flies lifts the test; one of a class no factory builds isn't counted, and with none counted there's
 * no test
 */
std::function<bool (const AIFloat3&)> CSpotScorer::MakeReachFn(CCircuitDef* def, bool isAnyReach) const
{
	struct SWalker {
		int classIdx;
		int part;
		float range;
	};
	const CChokeMap* chokeMap = circuit->GetChokeMap();
	const int frame = circuit->GetLastFrame();
	std::vector<SWalker> walkers;
	for (CCircuitUnit* w : circuit->GetBuilderManager()->GetWorkers()) {
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
	if (isAnyReach || walkers.empty()) {
		return [](const AIFloat3&) { return true; };
	}
	return [chokeMap, walkers](const AIFloat3& c) {
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
		for (const CTurretValue::STarget& t : MakeTargets(def, positions[i], radius, goal, towers)) {
			sums[i] += t.metal;
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
std::vector<CTurretValue::STarget> CSpotScorer::MakeTargets(CCircuitDef* def, const AIFloat3& pos, float radius,
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

	std::vector<CTurretValue::STarget> targets;  // metal: the raw traffic
	auto add = [&](int x, int z, float weight) {
		const float cx = (x + 0.5f) * cellSize;
		const float cz = (z + 0.5f) * cellSize;
		if ((weight > 0.f) && (SQUARE(cx - pos.x) + SQUARE(cz - pos.z) <= SQUARE(reach))) {
			targets.push_back({{cx, terrainMgr->GetHeightAt(cx, cz), cz}, weight, CTurretValue::Kind::TRAFFIC});
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
		std::stable_sort(targets.begin(), targets.end(),
				[](const CTurretValue::STarget& a, const CTurretValue::STarget& b) { return a.metal > b.metal; });
		if (targets.size() > count) {
			targets.resize(count);
		}
	};
	keepBest(MAX_TARGETS * 4);  // the line-of-fire tests of the halving only for those that may make the best 32
	HalveCovered(targets, towers);
	keepBest(MAX_TARGETS);
	if (!isChokeGoal) {
		float floor = 1.f;
		if (!targets.empty()) {
			floor = 0.f;
			for (const CTurretValue::STarget& t : targets) {
				floor += t.metal;
			}
			floor /= targets.size();
		}
		targets.push_back({{goal.asset.x, terrainMgr->GetHeightAt(goal.asset.x, goal.asset.z), goal.asset.z}, floor,
				CTurretValue::Kind::TRAFFIC});
	}
	return targets;
}

/*
 * Halved for each tower that covers it already, defends (CTurretPicker::IsDefence) and hits it: an AA tower doesn't
 * cover a mex, nor a Claw minefield an approach
 */
void CSpotScorer::HalveCovered(std::vector<CTurretValue::STarget>& targets, const std::vector<STower>& towers) const
{
	const CTurretPicker* picker = circuit->GetMilitaryManager()->GetTurretPicker();
	for (CTurretValue::STarget& t : targets) {
		const AIFloat3 p(t.pos.x, t.pos.y, t.pos.z);
		for (const STower& tower : towers) {
			if (picker->IsDefence(tower.def) && IsHit(tower.def, p) && (tower.pos.SqDistance2D(p) <= SQUARE(tower.range))
				&& (tower.isHighArc || IsClearLine(tower.pos, tower.emit, p)))
			{
				t.metal *= 0.5f;
			}
		}
	}
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
	circuit->GetBuilderManager()->ForEachQueuedTower([&add](IBuilderTask* t) {
		add(t->GetBuildDef(), t->GetPosition());
	});
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

bool CSpotScorer::IsHitAt(const CCircuitDef* def, float elevation)
{
	return def->HasAntiLand() || (CTerrainManager::IsDeepWater(elevation) && def->HasAntiWater());
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

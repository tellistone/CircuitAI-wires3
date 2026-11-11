/*
 * SensorPlanner.cpp
 *
 *  Created on: Nov 11, 2026
 *      Author: Claude
 */

#include "module/SensorPlanner.h"
#include "map/ThreatMap.h"
#include "module/BuilderManager.h"
#include "module/EconomyManager.h"
#include "module/MilitaryManager.h"
#include "resource/MetalManager.h"
#include "terrain/ChokeMap.h"
#include "terrain/TerrainManager.h"
#include "unit/ally/AllyUnit.h"
#include "unit/CircuitDef.h"
#include "unit/CircuitUnit.h"
#include "CircuitAI.h"
#include "util/Point.h"
#include "util/Utils.h"

#include "Log.h"
#include "UnitDef.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <set>

namespace circuit {

using namespace springai;

#define PLAN_SEC		15  // Update's interval
#define BLOCK_SIZE		512  // elmos: one candidate per block
#define BUILD_TRIES		4  // highest cells of a block tried for a build site
#define DEATH_HALF		(FRAMES_PER_SEC * 300)  // a lost radar's penalty halves every 5 minutes
#define DEATH_NEAR		512.f  // elmos: a loss counts against candidates this close
#define TARGET_LIFT		20.f  // sight rays end this high above a cell's ground
#define FRESH_VIEWS		16  // most viewsheds made per Update
#define PLAN_TIMEOUT	(FRAMES_PER_SEC * 90)  // a planned radar no builder starts by then is given up
#define IDLE_REACH		600.f  // elmos: an idle constructor's radar goes this close to it
#define HORIZON_SEC		300.f  // a planned radar's worth: the coverage it adds over this long from now

CSensorPlanner::CSensorPlanner(CCircuitAI* circuit, CCircuitDef* radarDef)
		: circuit(circuit)
		, radarDef(radarDef)
		, totalWeight(0.f)
		, coveredWeight(0.f)
		, plannedPos(-RgtVector)
		, budget((radarDef != nullptr) ? radarDef->GetCostM() : 0.f)  // the first radar goes up at once
		, lastFrame(-1)
{
}

CSensorPlanner::~CSensorPlanner()
{
}

void CSensorPlanner::Update()
{
	CChokeMap* chokeMap = circuit->GetChokeMap();
	if (!chokeMap->GetConfig().isRadar || !chokeMap->IsReady() || chokeMap->GetTraffic().IsEmpty()
		|| (radarDef == nullptr) || (chokeMap->GetAuthority() != circuit))  // one planner for the ally team
	{
		return;
	}
	const auto t0 = std::chrono::steady_clock::now();
	const int frame = circuit->GetLastFrame();
	const float cost = radarDef->GetCostM();
	const float dt = (lastFrame >= 0) ? float(frame - lastFrame) / FRAMES_PER_SEC : PLAN_SEC;
	lastFrame = frame;
	budget = std::min(budget + chokeMap->GetConfig().radarShare * circuit->GetEconomyManager()->GetAvgMetalIncome() * dt,
			cost * 2.f);

	UpdateWeights();
	UpdateRadars();
	CBuilderManager* builderMgr = circuit->GetBuilderManager();
	bool isWaiting = false;
	bool isPlannedQueued = false;
	for (const IBuilderTask* task : builderMgr->GetTasks(IBuilderTask::BuildType::RADAR)) {
		isWaiting |= (task->GetTarget() == nullptr);
		isPlannedQueued |= (task->GetTaskPos().SqDistance2D(plannedPos) < SQUARE(SQUARE_SIZE));
	}
	// The last planned radar's task is gone and no radar stands there: killed as it was built, or never started
	if (utils::is_valid(plannedPos) && !isPlannedQueued) {
		const bool isBuilt = std::any_of(radars.begin(), radars.end(), [this](const std::pair<const ICoreUnit::Id, SRadar>& kv) {
			return kv.second.pos.SqDistance2D(plannedPos) < SQUARE(SQUARE_SIZE * 16);
		});
		if (!isBuilt) {
			deaths.push_back({plannedPos, frame});
		}
		plannedPos = -RgtVector;
	}
	if (!radarDef->IsAvailable(frame) || (budget < cost) || isWaiting) {
		return;  // at most one planned radar waits for a builder
	}

	// Summed-area table of the uncovered weight: a candidate gains at most what its viewshed's square holds
	const CTrafficField& traffic = chokeMap->GetTraffic();
	const int width = traffic.GetWidth();
	const int height = traffic.GetHeight();
	const int size = traffic.GetCellSize();
	std::vector<double> sat((width + 1) * (height + 1), 0.0);
	for (int z = 0; z < height; ++z) {
		for (int x = 0; x < width; ++x) {
			const int cell = z * width + x;
			const float w = isCovered[cell] ? 0.f : weights[cell];
			sat[(z + 1) * (width + 1) + x + 1] = w + sat[z * (width + 1) + x + 1] + sat[(z + 1) * (width + 1) + x]
					- sat[z * (width + 1) + x];
		}
	}
	const float range = radarDef->GetDef()->GetRadarRadius();
	const int r = int(range / size) + 1;
	auto boundAt = [&](const AIFloat3& pos) {
		const int cx = int(pos.x) / size;
		const int cz = int(pos.z) / size;
		const int x0 = std::max(cx - r, 0), x1 = std::min(cx + r + 1, width);
		const int z0 = std::max(cz - r, 0), z1 = std::min(cz + r + 1, height);
		return float(sat[z1 * (width + 1) + x1] - sat[z0 * (width + 1) + x1] - sat[z1 * (width + 1) + x0]
				+ sat[z0 * (width + 1) + x0]);
	};

	UpdateCandidates();
	// Coverage over the next 5 minutes: a radar counts for the share of them it is up, once the builder that gets it
	// up soonest has walked there (as the crow flies) and built it
	const std::set<CCircuitUnit*>& workers = builderMgr->GetWorkers();
	auto uptimeOf = [&](const AIFloat3& pos) {
		float delay = workers.empty() ? 0.f : std::numeric_limits<float>::max();
		for (CCircuitUnit* w : workers) {
			CCircuitDef* wdef = w->GetCircuitDef();
			if (wdef->CanBuild(radarDef)) {
				delay = std::min(delay, w->GetPos(frame).distance2D(pos) / std::max(wdef->GetSpeed(), 1.f)
						+ radarDef->GetBuildTime() / std::max(wdef->GetBuildSpeed(), 1.f));
			}
		}
		return std::max(1.f - delay / HORIZON_SEC, 0.f);
	};
	struct SScored {
		AIFloat3 pos;
		float bound;  // score at most
		float divisor;  // cost x (1 + risk) / uptime
	};
	std::vector<SScored> scored;
	std::set<int> candidateCells;
	for (const AIFloat3& pos : candidates) {
		candidateCells.insert(traffic.IndexAt(pos.x, pos.z));
		const float uptime = uptimeOf(pos);
		if (uptime > 0.f) {
			const float divisor = cost * (1.f + GetRisk(pos)) / uptime;
			scored.push_back({pos, boundAt(pos) / divisor, divisor});
		}
	}
	for (auto it = viewsheds.begin(); it != viewsheds.end();) {  // only today's candidates stay cached
		it = (candidateCells.count(it->first) == 0) ? viewsheds.erase(it) : std::next(it);
	}
	std::stable_sort(scored.begin(), scored.end(), [](const SScored& a, const SScored& b) {  // ties: block order
		return a.bound > b.bound;
	});
	AIFloat3 bestPos = -RgtVector;
	float bestScore = 0.f;
	float bestGain = 0.f;
	int viewCount = 0;
	int freshCount = 0;
	for (const SScored& s : scored) {
		if (s.bound <= bestScore) {
			break;
		}
		if (viewsheds.count(traffic.IndexAt(s.pos.x, s.pos.z)) == 0) {
			if (freshCount >= FRESH_VIEWS) {
				break;  // the best so far; the rest are made on later calls
			}
			++freshCount;
		}
		float gain = 0.f;
		for (int cell : GetViewshed(s.pos)) {
			gain += isCovered[cell] ? 0.f : weights[cell];
		}
		++viewCount;
		if (gain / s.divisor > bestScore) {
			bestScore = gain / s.divisor;
			bestGain = gain;
			bestPos = s.pos;
		}
	}
	const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
	if (!utils::is_valid(bestPos) || (bestGain < chokeMap->GetConfig().radarGain * totalWeight)) {
		return;  // no radar adds enough
	}
	// HIGH until half the weight is covered: a NORMAL job far from our builders waits for them to run out of work
	const bool isUrgent = (GetCoveredShare() < 0.5f);
	builderMgr->EnqueueTask(isUrgent ? IBuilderTask::Priority::HIGH : IBuilderTask::Priority::NORMAL, radarDef, bestPos,
			IBuilderTask::BuildType::RADAR, cost, SQUARE_SIZE * 8.f, true, PLAN_TIMEOUT);
	circuit->LOG("RADAR planned at (%.0f, %.0f)%s: covered %.0f%% +%.0f%%, %i of %i candidates seen, %.2f ms",
			bestPos.x, bestPos.z, isUrgent ? " high" : "", GetCoveredShare() * 100.f, bestGain / totalWeight * 100.f,
			viewCount, (int)candidates.size(), ms);
	plannedPos = bestPos;
	AddPending(bestPos);
	budget -= cost;
}

IBuilderTask* CSensorPlanner::MakeIdleRadar(CCircuitUnit* unit)
{
	const CChokeMap* chokeMap = circuit->GetChokeMap();
	const int frame = circuit->GetLastFrame();
	if (!chokeMap->GetConfig().isRadar || !chokeMap->IsReady() || (radarDef == nullptr)
		|| !radarDef->IsAvailable(frame) || !unit->GetCircuitDef()->CanBuild(radarDef))
	{
		return nullptr;
	}
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	CThreatMap* threatMap = circuit->GetThreatMap();
	const float buildDist = unit->GetCircuitDef()->GetBuildDistance();
	auto isOk = [&](const AIFloat3& spot) {
		return terrainMgr->CanReachAt(unit, spot, buildDist) && (threatMap->GetThreatAt(spot) <= THREAT_MIN);
	};
	CSensorPlanner* planner = chokeMap->GetAuthority()->GetMilitaryManager()->GetSensorPlanner();
	const AIFloat3 spot = planner->TakeIdleSpot(unit->GetPos(frame), isOk);
	if (!utils::is_valid(spot)) {
		return nullptr;
	}
	circuit->LOG("RADAR for idle %s at (%.0f, %.0f)", unit->GetCircuitDef()->GetDef()->GetName(), spot.x, spot.z);
	return circuit->GetBuilderManager()->EnqueueTask(IBuilderTask::Priority::NOW, radarDef, spot,
			IBuilderTask::BuildType::RADAR, radarDef->GetCostM(), SQUARE_SIZE * 8.f, true, PLAN_TIMEOUT);
}

AIFloat3 CSensorPlanner::TakeIdleSpot(const AIFloat3& pos, const std::function<bool(const AIFloat3&)>& isOk)
{
	const CTrafficField& traffic = circuit->GetChokeMap()->GetTraffic();
	if (weights.empty() || (isCovered.size() != weights.size()) || isCovered[traffic.IndexAt(pos.x, pos.z)]) {
		return -RgtVector;  // no plan yet, or it stands in our radar
	}
	// The planner's candidates near it, and the highest buildable cell near it (a block's candidate can be further)
	std::vector<AIFloat3> spots;
	for (const AIFloat3& c : candidates) {
		if ((c.SqDistance2D(pos) <= SQUARE(IDLE_REACH)) && isOk(c)) {
			spots.push_back(c);
		}
	}
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	const int size = traffic.GetCellSize();
	std::vector<std::pair<float, AIFloat3>> cells;
	ForEachCellIn(traffic.GetWidth(), traffic.GetHeight(), size, pos.x - IDLE_REACH, pos.z - IDLE_REACH,
			pos.x + IDLE_REACH, pos.z + IDLE_REACH, [&](int x, int z) {
		AIFloat3 c((x + 0.5f) * size, 0.f, (z + 0.5f) * size);
		if (c.SqDistance2D(pos) <= SQUARE(IDLE_REACH)) {
			c.y = terrainMgr->GetHeightAt(c.x, c.z);
			cells.push_back({c.y, c});
		}
	});
	std::stable_sort(cells.begin(), cells.end(), [](const std::pair<float, AIFloat3>& a, const std::pair<float, AIFloat3>& b) {
		return a.first > b.first;
	});
	for (unsigned i = 0; i < std::min<unsigned>(cells.size(), BUILD_TRIES); ++i) {
		if (terrainMgr->CanBuildAt(radarDef, cells[i].second) && isOk(cells[i].second)) {
			spots.push_back(cells[i].second);
			break;
		}
	}
	AIFloat3 bestPos = -RgtVector;
	float bestScore = 0.f;
	float bestGain = 0.f;
	for (const AIFloat3& c : spots) {
		float gain = 0.f;
		for (int cell : GetViewshed(c)) {
			gain += isCovered[cell] ? 0.f : weights[cell];
		}
		const float score = gain / (1.f + GetRisk(c));
		if (score > bestScore) {
			bestScore = score;
			bestGain = gain;
			bestPos = c;
		}
	}
	if (!utils::is_valid(bestPos) || (bestGain < circuit->GetChokeMap()->GetConfig().radarGain * totalWeight)) {
		return -RgtVector;
	}
	AddPending(bestPos);
	return bestPos;
}

void CSensorPlanner::AddPending(const AIFloat3& pos)
{
	pending.push_back({pos, circuit->GetLastFrame()});
	for (int cell : GetViewshed(pos)) {
		coveredWeight += isCovered[cell] ? 0.f : weights[cell];
		isCovered[cell] = 1;
	}
}

void CSensorPlanner::UpdateWeights()
{
	const CChokeMap* chokeMap = circuit->GetChokeMap();
	const CTrafficField& traffic = chokeMap->GetTraffic();
	const SCellMask& reach = chokeMap->GetReachMask();
	CMetalManager* metalMgr = circuit->GetMetalManager();
	const CMetalData::Metals& spots = metalMgr->GetSpots();
	const int width = traffic.GetWidth();
	const int height = traffic.GetHeight();
	const int size = traffic.GetCellSize();

	// Our mex income; the whole map's before we hold any
	float ourIncome = 0.f;
	float mapIncome = 0.f;
	for (unsigned i = 0; i < spots.size(); ++i) {
		const int cluster = metalMgr->GetCluster(i);
		const bool isOurs = (cluster >= 0) && metalMgr->IsClusterHeld(cluster);
		ourIncome += isOurs ? spots[i].income : 0.f;
		mapIncome += spots[i].income;
	}
	const float income = (ourIncome > 0.f) ? ourIncome : mapIncome;

	weights.assign(width * height, 0.f);
	std::vector<uint8_t> isEnemyGround(width * height, 0);
	int reachCount = 0;
	int enemyCount = 0;
	float trafficSum = 0.f;
	for (int cell = 0; cell < width * height; ++cell) {
		const AIFloat3 pos((cell % width + 0.5f) * size, 0.f, (cell / width + 0.5f) * size);
		if (reach.IsPassable(reach.IndexAt(pos.x, pos.z))) {
			weights[cell] = 1.f;  // marks it reachable
			++reachCount;
			trafficSum += traffic.GetAt(cell);
			// The enemy's half of the map, and enemy territory on ours
			const int region = chokeMap->GetRegionAt(pos);
			isEnemyGround[cell] = !chokeMap->IsOurHalf(pos) || ((region >= 0) && chokeMap->IsEnemyTerritory(region));
			enemyCount += isEnemyGround[cell];
		}
	}
	const float floor = (reachCount > 0) ? income / reachCount : 0.f;
	const float trafficScale = (trafficSum > 0.f) ? income / trafficSum : 0.f;
	const float enemyFloor = (enemyCount > 0) ? chokeMap->GetConfig().radarEnemy * income / enemyCount : 0.f;
	for (int cell = 0; cell < width * height; ++cell) {
		if (weights[cell] > 0.f) {
			weights[cell] = floor + traffic.GetAt(cell) * trafficScale + (isEnemyGround[cell] ? enemyFloor : 0.f);
		}
	}
	for (unsigned i = 0; i < spots.size(); ++i) {
		const int cluster = metalMgr->GetCluster(i);
		if ((cluster >= 0) && metalMgr->IsClusterHeld(cluster)) {
			weights[traffic.IndexAt(spots[i].position.x, spots[i].position.z)] += spots[i].income;
		}
	}
	totalWeight = 0.f;
	for (float w : weights) {
		totalWeight += w;
	}
}

void CSensorPlanner::UpdateRadars()
{
	const CTrafficField& traffic = circuit->GetChokeMap()->GetTraffic();
	const int frame = circuit->GetLastFrame();
	std::map<ICoreUnit::Id, SRadar> seen;
	for (const auto& kv : circuit->GetFriendlyUnits()) {
		CAllyUnit* unit = kv.second;
		CCircuitDef* cdef = unit->GetCircuitDef();
		if (cdef->IsMobile()) {
			continue;
		}
		const float range = cdef->GetDef()->GetRadarRadius();
		if (range <= 0.f) {
			continue;  // a nanoframe counts: no second radar is planned over it
		}
		auto it = radars.find(kv.first);
		if (it != radars.end()) {
			seen[kv.first] = std::move(it->second);
		} else {
			const AIFloat3& pos = unit->GetPos(frame);
			seen[kv.first] = {pos, MakeViewshed(pos, range, cdef->GetDef()->GetLosHeight())};
		}
	}
	// One that is gone was most likely killed: building there again costs more
	for (const auto& kv : radars) {
		if (seen.find(kv.first) == seen.end()) {
			deaths.push_back({kv.second.pos, frame});
		}
	}
	deaths.erase(std::remove_if(deaths.begin(), deaths.end(), [frame](const SDeath& d) {
		return frame - d.frame > DEATH_HALF * 6;
	}), deaths.end());
	radars = std::move(seen);

	isCovered.assign(traffic.GetWidth() * traffic.GetHeight(), 0);
	for (const auto& kv : radars) {
		for (int cell : kv.second.cells) {
			isCovered[cell] = 1;
		}
	}
	// Planned ones count until built (their own view counts then) or given up
	pending.erase(std::remove_if(pending.begin(), pending.end(), [this, frame](const SDeath& p) {
		return (frame - p.frame > PLAN_TIMEOUT) || std::any_of(radars.begin(), radars.end(),
				[&p](const std::pair<const ICoreUnit::Id, SRadar>& kv) {
			return kv.second.pos.SqDistance2D(p.pos) < SQUARE(SQUARE_SIZE * 16);
		});
	}), pending.end());
	for (const SDeath& p : pending) {
		for (int cell : GetViewshed(p.pos)) {
			isCovered[cell] = 1;
		}
	}
	coveredWeight = 0.f;
	for (unsigned cell = 0; cell < weights.size(); ++cell) {
		coveredWeight += isCovered[cell] ? weights[cell] : 0.f;
	}
}

void CSensorPlanner::UpdateCandidates()
{
	const CChokeMap* chokeMap = circuit->GetChokeMap();
	const CTrafficField& traffic = chokeMap->GetTraffic();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	const int size = traffic.GetCellSize();
	const int perBlock = BLOCK_SIZE / size;
	candidates.clear();
	for (int bz = 0; bz < traffic.GetHeight(); bz += perBlock) {
		for (int bx = 0; bx < traffic.GetWidth(); bx += perBlock) {
			// The block's cells on our half of the map (not enemy territory) or in our territory, highest first
			std::vector<std::pair<float, AIFloat3>> cells;
			for (int z = bz; z < std::min(bz + perBlock, traffic.GetHeight()); ++z) {
				for (int x = bx; x < std::min(bx + perBlock, traffic.GetWidth()); ++x) {
					AIFloat3 pos((x + 0.5f) * size, 0.f, (z + 0.5f) * size);
					const int region = chokeMap->GetRegionAt(pos);
					const bool isOk = chokeMap->IsOurHalf(pos)
							? ((region < 0) || !chokeMap->IsEnemyTerritory(region))
							: ((region >= 0) && chokeMap->IsOurTerritory(region));
					if (isOk) {
						pos.y = terrainMgr->GetHeightAt(pos.x, pos.z);
						cells.push_back({pos.y, pos});
					}
				}
			}
			std::sort(cells.begin(), cells.end(), [](const std::pair<float, AIFloat3>& a, const std::pair<float, AIFloat3>& b) {
				return a.first > b.first;
			});
			for (unsigned i = 0; i < std::min<unsigned>(cells.size(), BUILD_TRIES); ++i) {
				if (terrainMgr->CanBuildAt(radarDef, cells[i].second)) {
					candidates.push_back(cells[i].second);
					break;
				}
			}
		}
	}
}

const std::vector<int>& CSensorPlanner::GetViewshed(const AIFloat3& pos)
{
	const CTrafficField& traffic = circuit->GetChokeMap()->GetTraffic();
	const int cell = traffic.IndexAt(pos.x, pos.z);
	auto it = viewsheds.find(cell);
	if (it == viewsheds.end()) {
		it = viewsheds.emplace(cell, MakeViewshed(pos, radarDef->GetDef()->GetRadarRadius(),
				radarDef->GetDef()->GetLosHeight())).first;
	}
	return it->second;
}

/*
 * Rays from the emitter (its ground plus emit) to every cell on the rim of the disc's square, half a cell a step:
 * a cell is seen when no ground nearer on its ray rises above the line to its own ground plus TARGET_LIFT
 */
std::vector<int> CSensorPlanner::MakeViewshed(const AIFloat3& pos, float range, float emit) const
{
	const CTrafficField& traffic = circuit->GetChokeMap()->GetTraffic();
	const int width = traffic.GetWidth();
	const int height = traffic.GetHeight();
	const float size = traffic.GetCellSize();
	const int r = int(range / size) + 1;
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	const float eye = terrainMgr->GetHeightAt(pos.x, pos.z) + emit;
	std::vector<uint8_t> isSeen(width * height, 0);
	std::vector<int> cells;
	auto mark = [&](float x, float z) {
		const int cx = int(x / size);
		const int cz = int(z / size);
		if ((cx >= 0) && (cz >= 0) && (cx < width) && (cz < height) && !isSeen[cz * width + cx]) {
			isSeen[cz * width + cx] = 1;
			cells.push_back(cz * width + cx);
		}
	};
	mark(pos.x, pos.z);
	auto castRay = [&](int dx, int dz) {
		const float len = std::sqrt(float(dx * dx + dz * dz)) * size;
		const int steps = int(len / (size * 0.5f));
		float maxSlope = -std::numeric_limits<float>::max();
		for (int i = 1; i <= steps; ++i) {
			const float dist = len * i / steps;
			if (dist > range) {
				break;
			}
			const float x = pos.x + dx * size * i / steps;
			const float z = pos.z + dz * size * i / steps;
			const float ground = terrainMgr->GetHeightAt(x, z);
			if ((ground + TARGET_LIFT - eye) / dist >= maxSlope) {
				mark(x, z);
			}
			maxSlope = std::max(maxSlope, (ground - eye) / dist);
		}
	};
	for (int d = -r; d <= r; ++d) {
		castRay(d, -r);
		castRay(d, r);
		if ((d > -r) && (d < r)) {
			castRay(-r, d);
			castRay(r, d);
		}
	}
	return cells;
}

float CSensorPlanner::GetRisk(const AIFloat3& pos) const
{
	float risk = circuit->GetThreatMap()->GetAllThreatAt(pos) / std::max(circuit->GetChokeMap()->GetMainDef()->GetThreat(), 1.f);
	const int frame = circuit->GetLastFrame();
	for (const SDeath& d : deaths) {
		if (d.pos.SqDistance2D(pos) < SQUARE(DEATH_NEAR)) {
			risk += std::exp2(-float(frame - d.frame) / DEATH_HALF);
		}
	}
	return risk;
}

} // namespace circuit

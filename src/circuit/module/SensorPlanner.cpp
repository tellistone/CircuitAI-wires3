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
#include "terrain/TerrainData.h"
#include "terrain/TerrainManager.h"
#include "unit/ally/AllyTeam.h"
#include "unit/ally/AllyUnit.h"
#include "unit/CircuitDef.h"
#include "CircuitAI.h"
#include "util/GameAttribute.h"
#include "util/Point.h"
#include "util/Utils.h"

#include "spring/SpringMap.h"

#include "Log.h"
#include "Unit.h"
#include "UnitDef.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace circuit {

using namespace springai;

#define PLAN_SEC		15  // Update's interval
#define BLOCK_SIZE		512  // elmos: one candidate per block
#define BUILD_TRIES		4  // highest cells of a block tried for a build site
#define DEATH_HALF		(FRAMES_PER_SEC * 300)  // a lost radar's penalty halves every 5 minutes
#define DEATH_NEAR		512.f  // elmos: a loss counts against candidates this close
#define TARGET_LIFT		20.f  // sight rays end this high above a cell's ground

CSensorPlanner::CSensorPlanner(CCircuitAI* circuit, CCircuitDef* radarDef)
		: circuit(circuit)
		, radarDef(radarDef)
		, totalWeight(0.f)
		, coveredWeight(0.f)
		, heightVersion(-1)
		, budget(0.f)
		, lastFrame(-1)
{
}

CSensorPlanner::~CSensorPlanner()
{
}

void CSensorPlanner::Update()
{
	CChokeMap* chokeMap = circuit->GetAllyTeam()->GetChokeMap().get();
	if (!chokeMap->GetConfig().isRadar || !chokeMap->IsReady() || chokeMap->GetTraffic().IsEmpty()
		|| (radarDef == nullptr))
	{
		return;
	}
	const auto t0 = std::chrono::steady_clock::now();
	const int frame = circuit->GetLastFrame();
	const int terrainVersion = circuit->GetGameAttribute()->GetTerrainData().GetHeightVersion();
	if (terrainVersion != heightVersion) {
		heightVersion = terrainVersion;
		viewsheds.clear();
		radars.clear();  // re-made below
	}
	const float cost = radarDef->GetCostM();
	const float dt = (lastFrame >= 0) ? float(frame - lastFrame) / FRAMES_PER_SEC : PLAN_SEC;
	lastFrame = frame;
	budget = std::min(budget + chokeMap->GetConfig().radarShare * circuit->GetEconomyManager()->GetAvgMetalIncome() * dt,
			cost * 2.f);

	UpdateWeights();
	UpdateRadars();
	if (!radarDef->IsAvailable(frame) || (coveredWeight >= chokeMap->GetConfig().radarCoverage * totalWeight)
		|| (budget < cost))
	{
		return;
	}
	CBuilderManager* builderMgr = circuit->GetBuilderManager();
	for (const IBuilderTask* task : builderMgr->GetTasks(IBuilderTask::BuildType::RADAR)) {
		if (task->GetTarget() == nullptr) {
			return;  // one waits already
		}
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
	struct SScored {
		AIFloat3 pos;
		float bound;  // score at most
		float divisor;  // cost x (1 + risk)
	};
	std::vector<SScored> scored;
	for (const AIFloat3& pos : candidates) {
		const float divisor = cost * (1.f + GetRisk(pos));
		scored.push_back({pos, boundAt(pos) / divisor, divisor});
	}
	std::stable_sort(scored.begin(), scored.end(), [](const SScored& a, const SScored& b) {  // ties: block order
		return a.bound > b.bound;
	});
	AIFloat3 bestPos = -RgtVector;
	float bestScore = 0.f;
	float bestGain = 0.f;
	int viewCount = 0;
	for (const SScored& s : scored) {
		if (s.bound <= bestScore) {
			break;
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
	if (!utils::is_valid(bestPos)) {
		return;
	}
	builderMgr->EnqueueTask(IBuilderTask::Priority::NORMAL, radarDef, bestPos, IBuilderTask::BuildType::RADAR, cost,
			SQUARE_SIZE * 8.f, true);
	budget -= cost;
	circuit->LOG("RADAR planned at (%.0f, %.0f): covered %.0f%% +%.0f%%, %i of %i candidates seen, %.2f ms",
			bestPos.x, bestPos.z, GetCoveredShare() * 100.f, bestGain / totalWeight * 100.f, viewCount,
			(int)candidates.size(), ms);
}

void CSensorPlanner::UpdateWeights()
{
	const CChokeMap* chokeMap = circuit->GetAllyTeam()->GetChokeMap().get();
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
		const bool isOurs = (cluster >= 0) && (metalMgr->IsClusterQueued(cluster) || metalMgr->IsClusterFinished(cluster));
		ourIncome += isOurs ? spots[i].income : 0.f;
		mapIncome += spots[i].income;
	}
	const float income = (ourIncome > 0.f) ? ourIncome : mapIncome;

	weights.assign(width * height, 0.f);
	int reachCount = 0;
	float trafficSum = 0.f;
	for (int cell = 0; cell < width * height; ++cell) {
		const float x = (cell % width + 0.5f) * size;
		const float z = (cell / width + 0.5f) * size;
		if (reach.IsPassable(reach.IndexAt(x, z))) {
			weights[cell] = 1.f;  // marks it reachable
			++reachCount;
			trafficSum += traffic.GetAt(cell);
		}
	}
	const float floor = (reachCount > 0) ? income / reachCount : 0.f;
	const float trafficScale = (trafficSum > 0.f) ? income / trafficSum : 0.f;
	for (int cell = 0; cell < width * height; ++cell) {
		if (weights[cell] > 0.f) {
			weights[cell] = floor + traffic.GetAt(cell) * trafficScale;
		}
	}
	for (unsigned i = 0; i < spots.size(); ++i) {
		const int cluster = metalMgr->GetCluster(i);
		if ((cluster >= 0) && (metalMgr->IsClusterQueued(cluster) || metalMgr->IsClusterFinished(cluster))) {
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
	const CTrafficField& traffic = circuit->GetAllyTeam()->GetChokeMap()->GetTraffic();
	const int frame = circuit->GetLastFrame();
	std::map<ICoreUnit::Id, SRadar> seen;
	for (const auto& kv : circuit->GetFriendlyUnits()) {
		CAllyUnit* unit = kv.second;
		CCircuitDef* cdef = unit->GetCircuitDef();
		const float range = cdef->GetDef()->GetRadarRadius();
		if (cdef->IsMobile() || (range <= 0.f) || unit->GetUnit()->IsBeingBuilt()) {
			continue;
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
	coveredWeight = 0.f;
	for (unsigned cell = 0; cell < weights.size(); ++cell) {
		coveredWeight += isCovered[cell] ? weights[cell] : 0.f;
	}
}

void CSensorPlanner::UpdateCandidates()
{
	const CChokeMap* chokeMap = circuit->GetAllyTeam()->GetChokeMap().get();
	const CTrafficField& traffic = chokeMap->GetTraffic();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	CMap* map = circuit->GetMap();
	const int size = traffic.GetCellSize();
	const int perBlock = BLOCK_SIZE / size;
	candidates.clear();
	for (int bz = 0; bz < traffic.GetHeight(); bz += perBlock) {
		for (int bx = 0; bx < traffic.GetWidth(); bx += perBlock) {
			// The block's cells on our side or in our territory, highest first
			std::vector<std::pair<float, AIFloat3>> cells;
			for (int z = bz; z < std::min(bz + perBlock, traffic.GetHeight()); ++z) {
				for (int x = bx; x < std::min(bx + perBlock, traffic.GetWidth()); ++x) {
					AIFloat3 pos((x + 0.5f) * size, 0.f, (z + 0.5f) * size);
					const int region = chokeMap->GetRegionAt(pos);
					if ((region >= 0) && (chokeMap->IsOurSide(region) || chokeMap->IsOurTerritory(region))) {
						pos.y = map->GetElevationAt(pos.x, pos.z);
						cells.push_back({GetHeight(pos.x, pos.z), pos});
					}
				}
			}
			std::sort(cells.begin(), cells.end(), [](const std::pair<float, AIFloat3>& a, const std::pair<float, AIFloat3>& b) {
				return a.first > b.first;
			});
			for (unsigned i = 0; i < std::min<unsigned>(cells.size(), BUILD_TRIES); ++i) {
				const AIFloat3& pos = cells[i].second;
				if (terrainMgr->CanBeBuiltAt(radarDef, pos) && map->IsPossibleToBuildAt(radarDef->GetDef(), pos, UNIT_FACING_SOUTH)) {
					candidates.push_back(pos);
					break;
				}
			}
		}
	}
}

const std::vector<int>& CSensorPlanner::GetViewshed(const AIFloat3& pos)
{
	const CTrafficField& traffic = circuit->GetAllyTeam()->GetChokeMap()->GetTraffic();
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
	const CTrafficField& traffic = circuit->GetAllyTeam()->GetChokeMap()->GetTraffic();
	const int width = traffic.GetWidth();
	const int height = traffic.GetHeight();
	const float size = traffic.GetCellSize();
	const int r = int(range / size) + 1;
	const float eye = GetHeight(pos.x, pos.z) + emit;
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
			const float ground = GetHeight(x, z);
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
	CMilitaryManager* militaryMgr = circuit->GetMilitaryManager();
	const std::vector<CCircuitDef*>& defenders = militaryMgr->GetLandDefenders();
	CCircuitDef* mainDef = defenders.empty() ? militaryMgr->GetDefaultPorc() : defenders.front();
	float risk = circuit->GetThreatMap()->GetAllThreatAt(pos) / std::max(mainDef->GetThreat(), 1.f);
	const int frame = circuit->GetLastFrame();
	for (const SDeath& d : deaths) {
		if (d.pos.SqDistance2D(pos) < SQUARE(DEATH_NEAR)) {
			risk += std::exp2(-float(frame - d.frame) / DEATH_HALF);
		}
	}
	return risk;
}

float CSensorPlanner::GetHeight(float x, float z) const
{
	const FloatVec& heightMap = circuit->GetTerrainManager()->GetAreaData()->heightMap;
	const int width = circuit->GetMap()->GetWidth();
	const int height = circuit->GetMap()->GetHeight();
	const int hx = utils::clamp(int(x) / SQUARE_SIZE, 0, width - 1);
	const int hz = utils::clamp(int(z) / SQUARE_SIZE, 0, height - 1);
	return heightMap[hz * width + hx];
}

} // namespace circuit

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
#include "module/FactoryManager.h"
#include "module/MilitaryManager.h"
#include "resource/MetalManager.h"
#include "terrain/ChokeMap.h"
#include "terrain/TerrainManager.h"
#include "unit/ally/AllyUnit.h"
#include "unit/CircuitDef.h"
#include "unit/CircuitUnit.h"
#include "CircuitAI.h"
#include "util/Utils.h"

#include "Log.h"
#include "UnitDef.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <set>

namespace circuit {

using namespace springai;

#define BLOCK_SIZE		512  // elmos: one candidate per block
#define BUILD_TRIES		4  // highest cells tried for a build site
#define DEATH_HALF		(FRAMES_PER_SEC * 300)  // a lost radar's penalty halves every 5 minutes
#define DEATH_NEAR		512.f  // elmos: a loss counts against spots this close
#define TARGET_LIFT		20.f  // sight rays end this high above a cell's ground
#define PENDING_TIME	(FRAMES_PER_SEC * 90)  // a radar a builder was sent to build counts this long until one stands
#define ORPHAN_TIMEOUT	(FRAMES_PER_SEC * 3)  // a radar job its builder left goes then: any builder takes a NOW job
#define REACH			600.f  // elmos: a builder out of coverage builds its radar this close
#define RADAR_NEAR		500.f  // elmos: ISensorTask drops a job this close to a radar of its kind; a pending one ends

CSensorPlanner::CSensorPlanner(CCircuitAI* circuit, CCircuitDef* radarDef)
		: circuit(circuit)
		, radarDef(radarDef)
		, totalWeight(0.f)
		, coveredWeight(0.f)
		, budget((radarDef != nullptr) ? radarDef->GetCostM() : 0.f)  // the first radar goes up in the opening
		, budgetFrame(0)
{
}

CSensorPlanner::~CSensorPlanner()
{
}

void CSensorPlanner::Update()
{
	const CChokeMap* chokeMap = circuit->GetChokeMap();
	if (!chokeMap->GetConfig().isRadar || !chokeMap->IsReady() || (radarDef == nullptr)
		|| (chokeMap->GetAuthority() != circuit))  // one map for the ally team
	{
		return;
	}
	UpdateWeights();
	UpdateRadars();
	UpdateCandidates();
	const CTrafficField& traffic = chokeMap->GetTraffic();
	std::set<int> cached;
	for (const AIFloat3& pos : candidates) {
		cached.insert(traffic.IndexAt(pos.x, pos.z));
	}
	for (const SEvent& p : pending) {
		cached.insert(traffic.IndexAt(p.pos.x, p.pos.z));
	}
	for (auto it = viewsheds.begin(); it != viewsheds.end();) {
		it = (cached.count(it->first) == 0) ? viewsheds.erase(it) : std::next(it);
	}
}

IBuilderTask* CSensorPlanner::MakeRadarTask(CCircuitUnit* unit, const std::function<bool(const AIFloat3&)>& isSafe)
{
	const CChokeMap* chokeMap = circuit->GetChokeMap();
	const int frame = circuit->GetLastFrame();
	if (!chokeMap->GetConfig().isRadar || (radarDef == nullptr) || !radarDef->IsAvailable(frame)
		|| !unit->GetCircuitDef()->CanBuild(radarDef) || (circuit->GetFactoryManager()->GetFactoryCount() == 0))
	{
		return nullptr;  // the factory goes first
	}
	const float cost = radarDef->GetCostM();
	budget = std::min(budget + chokeMap->GetConfig().radarShare * circuit->GetEconomyManager()->GetAvgMetalIncome()
			* (frame - budgetFrame) / FRAMES_PER_SEC, cost * 2.f);
	budgetFrame = frame;
	if (budget < cost) {
		return nullptr;
	}
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	CThreatMap* threatMap = circuit->GetThreatMap();
	const float buildDist = unit->GetCircuitDef()->GetBuildDistance();
	// No threat to the builder (its layer) or to the radar (the surface one, for air builders), and a way there
	auto isOk = [&](const AIFloat3& spot) {
		return (std::max(threatMap->GetThreatAt(spot), threatMap->GetAllThreatAt(spot)) <= THREAT_MIN) && isSafe(spot)
			&& terrainMgr->CanReachAt(unit, spot, buildDist);
	};
	CSensorPlanner* planner = chokeMap->GetAuthority()->GetMilitaryManager()->GetSensorPlanner();
	const AIFloat3 spot = planner->TakeSpot(unit, isOk);
	if (!utils::is_valid(spot)) {
		return nullptr;
	}
	budget -= cost;
	circuit->LOG("RADAR by %s at (%.0f, %.0f): %.0f%% covered with it", unit->GetCircuitDef()->GetDef()->GetName(),
			spot.x, spot.z, planner->GetCoveredShare() * 100.f);
	return circuit->GetBuilderManager()->EnqueueTask(IBuilderTask::Priority::NOW, radarDef, spot,
			IBuilderTask::BuildType::RADAR, cost, SQUARE_SIZE * 8.f, true, ORPHAN_TIMEOUT);
}

AIFloat3 CSensorPlanner::TakeSpot(CCircuitUnit* unit, const std::function<bool(const AIFloat3&)>& isOk)
{
	if (weights.empty()) {
		return -RgtVector;  // no map yet
	}
	const CTrafficField& traffic = circuit->GetChokeMap()->GetTraffic();
	const AIFloat3 pos = unit->GetPos(circuit->GetLastFrame());
	const CChokeMap::SConfig& config = circuit->GetChokeMap()->GetConfig();
	// Standing in our radar, only a radar that sees mostly new ground
	const float minShare = isCovered[traffic.IndexAt(pos.x, pos.z)] ? config.radarInside : 0.f;
	// The candidates near it, and the highest buildable cell near it (a block's candidate can stand further off)
	auto isNear = [&pos](const AIFloat3& c) { return c.SqDistance2D(pos) <= SQUARE(REACH); };
	std::vector<AIFloat3> spots;
	std::copy_if(candidates.begin(), candidates.end(), std::back_inserter(spots), isNear);
	const AIFloat3 local = FindHighest(pos.x - REACH, pos.z - REACH, pos.x + REACH, pos.z + REACH, isNear);
	if (utils::is_valid(local)) {
		spots.push_back(local);
	}
	auto isFree = [this](const AIFloat3& c) {
		return std::none_of(radars.begin(), radars.end(), [&c](const std::pair<const ICoreUnit::Id, SRadar>& kv) {
			return kv.second.pos.SqDistance2D(c) < SQUARE(RADAR_NEAR);
		}) && std::none_of(pending.begin(), pending.end(), [&c](const SEvent& p) {
			return p.pos.SqDistance2D(c) < SQUARE(RADAR_NEAR);
		});
	};
	const float minGain = config.radarGain * totalWeight;
	AIFloat3 bestPos = -RgtVector;
	float bestScore = 0.f;
	for (const AIFloat3& c : spots) {
		float share;
		const float gain = GetGain(c, share);
		if ((gain < minGain) || (share < minShare)) {
			continue;
		}
		const float score = gain / (1.f + GetRisk(c));
		if ((score > bestScore) && isFree(c) && isOk(c)) {  // the costly tests last
			bestScore = score;
			bestPos = c;
		}
	}
	if (!utils::is_valid(bestPos)) {
		return -RgtVector;
	}
	pending.push_back({bestPos, circuit->GetLastFrame()});
	for (int cell : GetViewshed(bestPos)) {
		coveredWeight += isCovered[cell] ? 0.f : weights[cell];
		isCovered[cell] = 1;
	}
	return bestPos;
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
		if (cdef->GetDef()->GetRadarRadius() <= 0.f) {
			continue;  // a nanoframe counts: no second radar goes over it
		}
		auto it = radars.find(kv.first);
		if (it != radars.end()) {
			seen[kv.first] = std::move(it->second);
		} else {
			const AIFloat3& pos = unit->GetPos(frame);
			seen[kv.first] = {pos, MakeViewshed(pos, cdef)};
		}
	}
	// One that is gone was most likely killed: building there again costs more
	for (const auto& kv : radars) {
		if (seen.find(kv.first) == seen.end()) {
			deaths.push_back({kv.second.pos, frame});
		}
	}
	deaths.erase(std::remove_if(deaths.begin(), deaths.end(), [frame](const SEvent& d) {
		return frame - d.frame > DEATH_HALF * 6;
	}), deaths.end());
	radars = std::move(seen);

	isCovered.assign(traffic.GetWidth() * traffic.GetHeight(), 0);
	for (const auto& kv : radars) {
		for (int cell : kv.second.cells) {
			isCovered[cell] = 1;
		}
	}
	// Pending ones count until a radar stands there (its own view counts then), or for PENDING_TIME
	pending.erase(std::remove_if(pending.begin(), pending.end(), [this, frame](const SEvent& p) {
		return (frame - p.frame > PENDING_TIME) || std::any_of(radars.begin(), radars.end(),
				[&p](const std::pair<const ICoreUnit::Id, SRadar>& kv) {
			return kv.second.pos.SqDistance2D(p.pos) < SQUARE(RADAR_NEAR);  // it, or one that ends its job
		});
	}), pending.end());
	for (const SEvent& p : pending) {
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
	// Our half of the map (not enemy territory), or our territory
	auto isOurs = [chokeMap](const AIFloat3& pos) {
		const int region = chokeMap->GetRegionAt(pos);
		return chokeMap->IsOurHalf(pos) ? ((region < 0) || !chokeMap->IsEnemyTerritory(region))
				: ((region >= 0) && chokeMap->IsOurTerritory(region));
	};
	candidates.clear();
	for (int z = 0; z < traffic.GetHeight() * traffic.GetCellSize(); z += BLOCK_SIZE) {
		for (int x = 0; x < traffic.GetWidth() * traffic.GetCellSize(); x += BLOCK_SIZE) {
			const AIFloat3 pos = FindHighest(x, z, x + BLOCK_SIZE - 1, z + BLOCK_SIZE - 1, isOurs);
			if (utils::is_valid(pos)) {
				candidates.push_back(pos);
			}
		}
	}
}

AIFloat3 CSensorPlanner::FindHighest(float x0, float z0, float x1, float z1,
		const std::function<bool(const AIFloat3&)>& isIn) const
{
	const CTrafficField& traffic = circuit->GetChokeMap()->GetTraffic();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	const int size = traffic.GetCellSize();
	std::vector<AIFloat3> cells;
	ForEachCellIn(traffic.GetWidth(), traffic.GetHeight(), size, x0, z0, x1, z1, [&](int x, int z) {
		AIFloat3 c((x + 0.5f) * size, 0.f, (z + 0.5f) * size);
		if (isIn(c)) {
			c.y = terrainMgr->GetHeightAt(c.x, c.z);
			cells.push_back(c);
		}
	});
	const int tries = std::min<int>(cells.size(), BUILD_TRIES);
	std::partial_sort(cells.begin(), cells.begin() + tries, cells.end(), [](const AIFloat3& a, const AIFloat3& b) {
		return a.y > b.y;
	});
	for (int i = 0; i < tries; ++i) {
		if (terrainMgr->CanBuildAt(radarDef, cells[i])) {
			return cells[i];
		}
	}
	return -RgtVector;
}

const std::vector<int>& CSensorPlanner::GetViewshed(const AIFloat3& pos)
{
	const CTrafficField& traffic = circuit->GetChokeMap()->GetTraffic();
	const int cell = traffic.IndexAt(pos.x, pos.z);
	auto it = viewsheds.find(cell);
	if (it == viewsheds.end()) {
		it = viewsheds.emplace(cell, MakeViewshed(pos, radarDef)).first;
	}
	return it->second;
}

/*
 * Rays from the emitter to every cell on the rim of the disc's square, half a cell a step: a cell is seen when no
 * ground nearer on its ray rises above the line to its own ground plus TARGET_LIFT. The engine emits from the
 * unit's middle plus its emit height; the AI reads the sight one only
 */
std::vector<int> CSensorPlanner::MakeViewshed(const AIFloat3& pos, CCircuitDef* cdef) const
{
	const CTrafficField& traffic = circuit->GetChokeMap()->GetTraffic();
	const int width = traffic.GetWidth();
	const int height = traffic.GetHeight();
	const float size = traffic.GetCellSize();
	const float range = cdef->GetDef()->GetRadarRadius();
	const int r = int(range / size) + 1;
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	const float eye = terrainMgr->GetHeightAt(pos.x, pos.z) + cdef->GetHeight() * 0.5f + cdef->GetDef()->GetLosHeight();
	std::vector<int> cells = {traffic.IndexAt(pos.x, pos.z)};
	auto castRay = [&](int dx, int dz) {
		const float len = std::sqrt(float(dx * dx + dz * dz)) * size;
		const int steps = int(len / (size * 0.5f));
		float maxSlope = -std::numeric_limits<float>::max();
		for (int i = 1; i <= steps; ++i) {
			const float dist = len * i / steps;
			const float x = pos.x + dx * size * i / steps;
			const float z = pos.z + dz * size * i / steps;
			if ((dist > range) || (x < 0.f) || (z < 0.f) || (x >= width * size) || (z >= height * size)) {
				break;  // a ray off the map doesn't come back
			}
			const float ground = terrainMgr->GetHeightAt(x, z);
			if ((ground + TARGET_LIFT - eye) / dist >= maxSlope) {
				cells.push_back(traffic.IndexAt(x, z));
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
	std::sort(cells.begin(), cells.end());
	cells.erase(std::unique(cells.begin(), cells.end()), cells.end());
	return cells;
}

float CSensorPlanner::GetGain(const AIFloat3& pos, float& outShare)
{
	float gain = 0.f;
	float seen = 0.f;
	for (int cell : GetViewshed(pos)) {
		gain += isCovered[cell] ? 0.f : weights[cell];
		seen += weights[cell];
	}
	outShare = (seen > 0.f) ? gain / seen : 0.f;
	return gain;
}

float CSensorPlanner::GetRisk(const AIFloat3& pos) const
{
	float risk = 0.f;
	const int frame = circuit->GetLastFrame();
	for (const SEvent& d : deaths) {
		if (d.pos.SqDistance2D(pos) < SQUARE(DEATH_NEAR)) {
			risk += std::exp2(-float(frame - d.frame) / DEATH_HALF);
		}
	}
	return risk;
}

} // namespace circuit

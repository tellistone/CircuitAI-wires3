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
#include "module/SpotScorer.h"
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
#define PENDING_TIME	(FRAMES_PER_SEC * 600)  // a radar job counts this long at most (a job dropped without its end)
#define ORPHAN_TIMEOUT	(FRAMES_PER_SEC * 3)  // a radar job its builder left goes then: any builder takes a NOW job
#define REACH			600.f  // elmos: a builder out of coverage builds its radar this close
#define RADAR_NEAR		500.f  // elmos: ISensorTask drops a job this close to a radar of its kind
#define HEAVY_REACH		1500.f  // elmos: a builder builds the heavy this close; losses this close count against it
#define HEAVY_TRIES		16  // heavy candidates scored by their viewshed, of those with the most uncovered weight
#define HEAVY_FRESH		4  // new heavy viewsheds per search at most (the rest wait for the next)
#define HEAVY_RETRY		(FRAMES_PER_SEC * 10)  // a builder of this AI looks for a heavy spot at most this often
#define HEAVY_REBUILD	(FRAMES_PER_SEC * 90)  // no heavy for this long after the ally team lost one
#define HEAVY_LOG		(FRAMES_PER_SEC * 60)  // a failed heavy search is logged at most this often per AI
#define CHAIN_RADIUS	300.f  // elmos: a hub radar step's spot from its offset

CSensorPlanner::CSensorPlanner(CCircuitAI* circuit, CCircuitDef* radarDef, CCircuitDef* heavyDef)
		: circuit(circuit)
		, radarDef(radarDef)
		, heavyDef(heavyDef)
		, totalWeight(0.f)
		, coveredWeight(0.f)
		, budget((radarDef != nullptr) ? radarDef->GetCostM() : 0.f)  // the first radar goes up in the opening
		, budgetFrame(0)
		, heavySearchFrame(-HEAVY_RETRY)
		, heavyLogFrame(-HEAVY_LOG)
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
	std::set<std::pair<int, int>> cached;
	for (const AIFloat3& pos : candidates) {
		cached.insert({traffic.IndexAt(pos.x, pos.z), radarDef->GetId()});
	}
	for (const AIFloat3& pos : heavyCands) {
		cached.insert({traffic.IndexAt(pos.x, pos.z), heavyDef->GetId()});
	}
	for (const SEvent& p : pending) {
		cached.insert({traffic.IndexAt(p.pos.x, p.pos.z), p.cdef->GetId()});
	}
	for (auto it = viewsheds.begin(); it != viewsheds.end();) {
		it = (cached.count(it->first) == 0) ? viewsheds.erase(it) : std::next(it);
	}
}

IBuilderTask* CSensorPlanner::MakeRadarTask(CCircuitUnit* unit, const std::function<bool(const AIFloat3&)>& isSafe)
{
	const CChokeMap* chokeMap = circuit->GetChokeMap();
	const int frame = circuit->GetLastFrame();
	if (!chokeMap->GetConfig().isRadar || (circuit->GetFactoryManager()->GetFactoryCount() == 0)) {
		return nullptr;  // the factory goes first
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

	// The heavy first: it covers what many small ones would. Not the commander's job, nor a front builder's
	const AIFloat3 pos = unit->GetPos(frame);
	if (!unit->GetCircuitDef()->IsRoleComm() && (frame - heavySearchFrame >= HEAVY_RETRY)
		&& planner->IsDeepReach(pos) && IsHeavyReady(unit->GetCircuitDef()))
	{
		heavySearchFrame = frame;
		float best;
		const AIFloat3 spot = planner->TakeHeavySpot(unit, isOk, best);
		if (utils::is_valid(spot)) {
			circuit->LOG("RADAR HEAVY by %s at (%.0f, %.0f): %.0f%% covered with it",
					unit->GetCircuitDef()->GetDef()->GetName(), spot.x, spot.z, planner->GetCoveredShare() * 100.f);
			// Its guard is the build_chain "radar" hub of its type
			return circuit->GetBuilderManager()->EnqueueTask(IBuilderTask::Priority::NOW, heavyDef, spot,
					IBuilderTask::BuildType::RADAR, heavyDef->GetCostM(), 0.f, true, ORPHAN_TIMEOUT);
		}
		if (frame - heavyLogFrame >= HEAVY_LOG) {
			heavyLogFrame = frame;
			circuit->LOG("RADAR HEAVY none near (%.0f, %.0f): best %.1f%% new, %.0f%% needed", pos.x, pos.z,
					best * 100.f, circuit->GetChokeMap()->GetConfig().heavyGain * 100.f);
		}
	}

	if ((radarDef == nullptr) || !radarDef->IsAvailable(frame) || !unit->GetCircuitDef()->CanBuild(radarDef)) {
		return nullptr;
	}
	const float cost = radarDef->GetCostM();
	budget = std::min(budget + chokeMap->GetConfig().radarShare * circuit->GetEconomyManager()->GetAvgMetalIncome()
			* (frame - budgetFrame) / FRAMES_PER_SEC, cost * 2.f);
	budgetFrame = frame;
	if (budget < cost) {
		return nullptr;
	}
	const AIFloat3 spot = planner->TakeSpot(unit, isOk);
	if (!utils::is_valid(spot)) {
		return nullptr;
	}
	budget -= cost;
	circuit->LOG("RADAR by %s at (%.0f, %.0f): %.0f%% covered with it", unit->GetCircuitDef()->GetDef()->GetName(),
			spot.x, spot.z, planner->GetCoveredShare() * 100.f);
	IBuilderTask* task = circuit->GetBuilderManager()->EnqueueTask(IBuilderTask::Priority::NOW, radarDef, spot,
			IBuilderTask::BuildType::RADAR, cost, 0.f, true, ORPHAN_TIMEOUT);  // no shake: built where it sees most
	task->SetNextTask(MakeGuardTask(spot));  // queued once the radar stands; dropped with it
	return task;
}

const CCircuitDef* CSensorPlanner::GetPlannedDef() const
{
	return circuit->GetChokeMap()->GetConfig().isRadar ? heavyDef : nullptr;
}

bool CSensorPlanner::IsHeavyReady(CCircuitDef* builderDef) const
{
	const int frame = circuit->GetLastFrame();
	if ((heavyDef == nullptr) || !heavyDef->IsAvailable(frame) || !builderDef->CanBuild(heavyDef)
		|| (circuit->GetEconomyManager()->GetTeamMetalIncome() < circuit->GetChokeMap()->GetConfig().heavyIncome))
	{
		return false;
	}
	const CSensorPlanner* planner = circuit->GetChokeMap()->GetAuthority()->GetMilitaryManager()->GetSensorPlanner();
	return std::none_of(planner->deaths.begin(), planner->deaths.end(), [this, frame](const SEvent& d) {
		return (d.cdef == heavyDef) && (frame - d.frame < HEAVY_REBUILD);
	});
}

CCircuitDef* CSensorPlanner::PlaceChainRadar(CCircuitDef* askDef, bool isUpgrade, CCircuitDef* builderDef,
		AIFloat3& inOutPos)
{
	const CChokeMap* chokeMap = circuit->GetChokeMap();
	const CChokeMap::SConfig& config = chokeMap->GetConfig();
	if (!config.isRadar || !config.isRadarChain || !chokeMap->IsReady()) {
		return askDef;
	}
	CSensorPlanner* planner = chokeMap->GetAuthority()->GetMilitaryManager()->GetSensorPlanner();
	if (planner->weights.empty()) {
		return askDef;  // no map yet
	}
	if (isUpgrade && IsHeavyReady(builderDef)) {
		const AIFloat3 spot = planner->FindChainSpot(heavyDef, inOutPos);
		if (utils::is_valid(spot) && (planner->ScoreHeavy(spot) >= config.heavyGain * planner->totalWeight)) {
			circuit->LOG("RADAR CHAIN upgrade at (%.0f, %.0f)", spot.x, spot.z);
			inOutPos = spot;
			return heavyDef;
		}
	}
	AIFloat3 spot = planner->FindChainSpot(askDef, inOutPos);
	if (!utils::is_valid(spot)) {
		spot = inOutPos;
	}
	float share;
	const float gain = planner->GetGain(spot, askDef, share);
	if (gain < config.radarGain * planner->totalWeight) {
		circuit->LOG("RADAR CHAIN skip %s at (%.0f, %.0f): %.1f%% new", askDef->GetDef()->GetName(), spot.x, spot.z,
				(planner->totalWeight > 0.f) ? gain / planner->totalWeight * 100.f : 0.f);
		return nullptr;
	}
	circuit->LOG("RADAR CHAIN place %s at (%.0f, %.0f)", askDef->GetDef()->GetName(), spot.x, spot.z);
	inOutPos = spot;
	return askDef;
}

void CSensorPlanner::NotePending(const IBuilderTask* task)
{
	const CChokeMap* chokeMap = circuit->GetChokeMap();
	CCircuitDef* cdef = task->GetBuildDef();
	if (!chokeMap->GetConfig().isRadar || !chokeMap->IsReady() || (cdef == nullptr)
		|| (cdef->GetDef()->GetRadarRadius() <= 0.f))
	{
		return;
	}
	pending.push_back({task->GetTaskPos(), circuit->GetLastFrame(), cdef, task});
	if (!isCovered.empty()) {  // else UpdateRadars counts it
		Cover(GetViewshed(task->GetTaskPos(), cdef));
	}
}

void CSensorPlanner::EndPending(const IBuilderTask* task)
{
	// Its view stays covered until UpdateRadars: a finished radar covers it, an aborted job frees it then
	pending.erase(std::remove_if(pending.begin(), pending.end(), [task](const SEvent& p) {
		return p.task == task;
	}), pending.end());
}

void CSensorPlanner::Cover(const std::vector<int>& cells)
{
	for (int cell : cells) {
		coveredWeight += isCovered[cell] ? 0.f : weights[cell];
		isCovered[cell] = 1;
	}
}

bool CSensorPlanner::IsFree(const AIFloat3& c) const
{
	return std::none_of(radars.begin(), radars.end(), [&c](const std::pair<const ICoreUnit::Id, SRadar>& kv) {
		return kv.second.pos.SqDistance2D(c) < SQUARE(RADAR_NEAR);
	}) && std::none_of(pending.begin(), pending.end(), [&c](const SEvent& p) {
		return p.pos.SqDistance2D(c) < SQUARE(RADAR_NEAR);
	});
}

AIFloat3 CSensorPlanner::FindChainSpot(CCircuitDef* cdef, const AIFloat3& pos) const
{
	auto isIn = [this, &pos](const AIFloat3& c) {
		return (c.SqDistance2D(pos) <= SQUARE(CHAIN_RADIUS)) && IsFree(c);
	};
	return FindHighest(cdef, pos.x - CHAIN_RADIUS, pos.z - CHAIN_RADIUS, pos.x + CHAIN_RADIUS, pos.z + CHAIN_RADIUS,
			isIn);
}

IBuilderTask* CSensorPlanner::MakeGuardTask(const AIFloat3& radarPos)
{
	const CChokeMap::SConfig& config = circuit->GetChokeMap()->GetConfig();
	CCircuitDef* guardDef = config.radarGuard.empty() ? nullptr : circuit->GetCircuitDef(config.radarGuard.c_str());
	if ((guardDef == nullptr) || !guardDef->IsAvailable(circuit->GetLastFrame())) {
		return nullptr;
	}
	// A tower of ours or an ally's already reaches it
	const int frame = circuit->GetLastFrame();
	for (const auto& kv : circuit->GetFriendlyUnits()) {
		CCircuitDef* cdef = kv.second->GetCircuitDef();
		if (!cdef->IsMobile() && cdef->IsAttacker()
			&& (kv.second->GetPos(frame).SqDistance2D(radarPos) < SQUARE(cdef->GetMaxRange())))
		{
			return nullptr;
		}
	}
	// The spot scorer's best spot covering its approaches, else beside it
	AIFloat3 pos = circuit->GetMilitaryManager()->GetSpotScorer()->FindGuardSpot(guardDef, radarPos,
			config.radarGuardRadius);
	if (!utils::is_valid(pos)) {
		pos = radarPos;
	}
	if (circuit->GetMilitaryManager()->RefuseCrowdedLight(guardDef, pos, "radar guard")) {
		return nullptr;  // turret.crowd_count
	}
	CMilitaryManager::TurretScope scope(circuit->GetMilitaryManager(), CMilitaryManager::TurretSpend::GUARD);
	return circuit->GetBuilderManager()->EnqueueTask(IBuilderTask::Priority::HIGH, guardDef, pos,
			IBuilderTask::BuildType::DEFENCE, guardDef->GetCostM(), 0.f, false);
}

AIFloat3 CSensorPlanner::TakeSpot(CCircuitUnit* unit, const std::function<bool(const AIFloat3&)>& isOk)
{
	if (weights.empty()) {
		return -RgtVector;  // no map yet
	}
	const CTrafficField& traffic = circuit->GetChokeMap()->GetTraffic();
	const int width = traffic.GetWidth();
	const AIFloat3 pos = unit->GetPos(circuit->GetLastFrame());
	const CChokeMap::SConfig& config = circuit->GetChokeMap()->GetConfig();
	const float minGain = config.radarGain * totalWeight;
	// Too little uncovered weight in sight of any spot near it (once the map is covered): no search
	const float span = REACH + radarDef->GetDef()->GetRadarRadius();
	float nearGain = 0.f;
	ForEachCellIn(width, traffic.GetHeight(), traffic.GetCellSize(), pos.x - span, pos.z - span, pos.x + span,
			pos.z + span, [&](int x, int z) {
		nearGain += isCovered[z * width + x] ? 0.f : weights[z * width + x];
	});
	if (nearGain < minGain) {
		return -RgtVector;
	}
	// Standing in our radar, only a radar whose view is choke.radar.inside new
	const float minShare = isCovered[traffic.IndexAt(pos.x, pos.z)] ? config.radarInside : 0.f;
	// The candidates near it, and the highest buildable cell near it (a block's candidate can stand further off)
	auto isNear = [&pos](const AIFloat3& c) { return c.SqDistance2D(pos) <= SQUARE(REACH); };
	std::vector<AIFloat3> spots;
	std::copy_if(candidates.begin(), candidates.end(), std::back_inserter(spots), isNear);
	const AIFloat3 local = FindHighest(radarDef, pos.x - REACH, pos.z - REACH, pos.x + REACH, pos.z + REACH, isNear);
	if (utils::is_valid(local)) {
		spots.push_back(local);
	}
	AIFloat3 bestPos = -RgtVector;
	float bestScore = 0.f;
	for (const AIFloat3& c : spots) {
		float share;
		const float score = GetGain(c, radarDef, share) / (1.f + GetRisk(c, DEATH_NEAR));  // where radars die, see more
		if ((score < minGain) || (share < minShare)) {
			continue;
		}
		if ((score > bestScore) && IsFree(c) && isOk(c)) {  // the costly tests last
			bestScore = score;
			bestPos = c;
		}
	}
	return bestPos;  // its job's NotePending covers its view
}

AIFloat3 CSensorPlanner::TakeHeavySpot(CCircuitUnit* unit, const std::function<bool(const AIFloat3&)>& isOk,
		float& outBest)
{
	outBest = 0.f;
	if (weights.empty() || (heavyDef == nullptr)) {
		return -RgtVector;  // no map yet
	}
	const CTrafficField& traffic = circuit->GetChokeMap()->GetTraffic();
	const CChokeMap::SConfig& config = circuit->GetChokeMap()->GetConfig();
	const AIFloat3 pos = unit->GetPos(circuit->GetLastFrame());
	const float minGain = config.heavyGain * totalWeight;
	const float range = heavyDef->GetDef()->GetRadarRadius();
	// Candidates near it and deep in our ground, ranked by the uncovered weight in their range disc (a viewshed is a
	// part of its disc, so that is an upper bound of its gain)
	std::vector<std::pair<float, AIFloat3>> ranked;
	for (const AIFloat3& c : candidates) {
		if ((c.SqDistance2D(pos) <= SQUARE(HEAVY_REACH))
			&& (enemyDist[traffic.IndexAt(c.x, c.z)] >= config.heavyDepth))
		{
			ranked.push_back({GetDiscGain(c, range), c});
		}
	}
	const int tries = std::min<int>(ranked.size(), HEAVY_TRIES);
	std::partial_sort(ranked.begin(), ranked.begin() + tries, ranked.end(),
			[](const std::pair<float, AIFloat3>& a, const std::pair<float, AIFloat3>& b) {
		return a.first > b.first;
	});
	heavyCands.clear();
	AIFloat3 bestPos = -RgtVector;
	float bestScore = minGain;
	int fresh = 0;
	for (int i = 0; i < tries; ++i) {
		const AIFloat3& c = ranked[i].second;
		if ((i > 0) && (ranked[i].first < bestScore)) {
			break;  // sorted: no later one can pass or beat it (the first is scored for the log)
		}
		if (!IsCached(c, heavyDef) && (++fresh > HEAVY_FRESH)) {
			break;  // the rest next time
		}
		heavyCands.push_back(c);
		const float score = ScoreHeavy(c);
		outBest = std::max(outBest, (totalWeight > 0.f) ? score / totalWeight : 0.f);
		if ((score >= bestScore) && isOk(c)) {
			bestScore = score;
			bestPos = c;
		}
	}
	return bestPos;  // its job's NotePending covers its view
}

float CSensorPlanner::ScoreHeavy(const AIFloat3& pos)
{
	const CTrafficField& traffic = circuit->GetChokeMap()->GetTraffic();
	if ((enemyDist[traffic.IndexAt(pos.x, pos.z)] < circuit->GetChokeMap()->GetConfig().heavyDepth) || !IsFree(pos)
		|| !circuit->GetTerrainManager()->CanBuildAt(heavyDef, pos))
	{
		return -1.f;
	}
	float share;
	return GetGain(pos, heavyDef, share) / (1.f + GetRisk(pos, HEAVY_REACH));
}

bool CSensorPlanner::IsDeepReach(const AIFloat3& pos) const
{
	if (enemyDist.empty()) {
		return false;  // no map yet
	}
	const CTrafficField& traffic = circuit->GetChokeMap()->GetTraffic();
	return enemyDist[traffic.IndexAt(pos.x, pos.z)] + HEAVY_REACH >= circuit->GetChokeMap()->GetConfig().heavyDepth;
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
			// The enemy's half of the map, and the enemy's held ground on ours
			const int region = chokeMap->GetRegionAt(pos);
			isEnemyGround[cell] = !chokeMap->IsOurHalf(pos) || ((region >= 0) && chokeMap->IsEnemyGround(region));
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
	// Elmos to the nearest enemy ground cell: a two-pass chamfer sweep (the heavy stands deep)
	const float diag = size * SQRT_2;
	enemyDist.assign(width * height, std::numeric_limits<float>::max());
	for (int cell = 0; cell < width * height; ++cell) {
		if (isEnemyGround[cell]) {
			enemyDist[cell] = 0.f;
		}
	}
	auto relax = [&](int x, int z, int dx, int dz, float step) {
		const int nx = x + dx, nz = z + dz;
		if ((nx >= 0) && (nx < width) && (nz >= 0) && (nz < height) && (enemyDist[nz * width + nx] < enemyDist[z * width + x] - step)) {
			enemyDist[z * width + x] = enemyDist[nz * width + nx] + step;
		}
	};
	for (int z = 0; z < height; ++z) {
		for (int x = 0; x < width; ++x) {
			relax(x, z, -1, 0, size); relax(x, z, 0, -1, size); relax(x, z, -1, -1, diag); relax(x, z, 1, -1, diag);
		}
	}
	for (int z = height - 1; z >= 0; --z) {
		for (int x = width - 1; x >= 0; --x) {
			relax(x, z, 1, 0, size); relax(x, z, 0, 1, size); relax(x, z, 1, 1, diag); relax(x, z, -1, 1, diag);
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
			continue;
		}
		auto it = radars.find(kv.first);
		if (it != radars.end()) {
			seen[kv.first] = std::move(it->second);
		} else {
			const AIFloat3& pos = unit->GetPos(frame);
			seen[kv.first] = {pos, MakeViewshed(pos, cdef), cdef, false};
		}
		seen[kv.first].isBuilt = !unit->GetUnit()->IsBeingBuilt();  // a nanoframe only keeps others off (IsFree)
	}
	// One that is gone was most likely killed: building there again costs more
	for (const auto& kv : radars) {
		if (seen.find(kv.first) == seen.end()) {
			deaths.push_back({kv.second.pos, frame, kv.second.cdef, nullptr});
		}
	}
	deaths.erase(std::remove_if(deaths.begin(), deaths.end(), [frame](const SEvent& d) {
		return frame - d.frame > DEATH_HALF * 6;
	}), deaths.end());
	radars = std::move(seen);

	isCovered.assign(traffic.GetWidth() * traffic.GetHeight(), 0);
	for (const auto& kv : radars) {
		if (kv.second.isBuilt) {
			for (int cell : kv.second.cells) {
				isCovered[cell] = 1;
			}
		}
	}
	// Active jobs count until they end (EndPending), or for PENDING_TIME
	pending.erase(std::remove_if(pending.begin(), pending.end(), [frame](const SEvent& p) {
		return frame - p.frame > PENDING_TIME;
	}), pending.end());
	for (const SEvent& p : pending) {
		for (int cell : GetViewshed(p.pos, p.cdef)) {
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
	// Our half of the map (not the enemy's held ground), or our held ground
	auto isOurs = [chokeMap](const AIFloat3& pos) {
		const int region = chokeMap->GetRegionAt(pos);
		return chokeMap->IsOurHalf(pos) ? ((region < 0) || !chokeMap->IsEnemyGround(region))
				: ((region >= 0) && chokeMap->IsOurGround(region));
	};
	candidates.clear();
	for (int z = 0; z < traffic.GetHeight() * traffic.GetCellSize(); z += BLOCK_SIZE) {
		for (int x = 0; x < traffic.GetWidth() * traffic.GetCellSize(); x += BLOCK_SIZE) {
			const AIFloat3 pos = FindHighest(radarDef, x, z, x + BLOCK_SIZE - 1, z + BLOCK_SIZE - 1, isOurs);
			if (utils::is_valid(pos)) {
				candidates.push_back(pos);
			}
		}
	}
}

AIFloat3 CSensorPlanner::FindHighest(CCircuitDef* cdef, float x0, float z0, float x1, float z1,
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
		if (terrainMgr->CanBuildAt(cdef, cells[i])) {
			return cells[i];
		}
	}
	return -RgtVector;
}

const std::vector<int>& CSensorPlanner::GetViewshed(const AIFloat3& pos, CCircuitDef* cdef)
{
	const CTrafficField& traffic = circuit->GetChokeMap()->GetTraffic();
	const std::pair<int, int> key(traffic.IndexAt(pos.x, pos.z), cdef->GetId());
	auto it = viewsheds.find(key);
	if (it == viewsheds.end()) {
		it = viewsheds.emplace(key, MakeViewshed(pos, cdef)).first;
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

float CSensorPlanner::GetGain(const AIFloat3& pos, CCircuitDef* cdef, float& outShare)
{
	float gain = 0.f;
	float seen = 0.f;
	for (int cell : GetViewshed(pos, cdef)) {
		gain += isCovered[cell] ? 0.f : weights[cell];
		seen += weights[cell];
	}
	outShare = (seen > 0.f) ? gain / seen : 0.f;
	return gain;
}

float CSensorPlanner::GetDiscGain(const AIFloat3& pos, float range) const
{
	const CTrafficField& traffic = circuit->GetChokeMap()->GetTraffic();
	const int width = traffic.GetWidth();
	const int size = traffic.GetCellSize();
	float gain = 0.f;
	ForEachCellIn(width, traffic.GetHeight(), size, pos.x - range, pos.z - range, pos.x + range, pos.z + range,
			[&](int x, int z) {
		const float dx = (x + 0.5f) * size - pos.x;
		const float dz = (z + 0.5f) * size - pos.z;
		if ((dx * dx + dz * dz <= SQUARE(range + size)) && !isCovered[z * width + x]) {
			gain += weights[z * width + x];
		}
	});
	return gain;
}

bool CSensorPlanner::IsCached(const AIFloat3& pos, CCircuitDef* cdef) const
{
	const CTrafficField& traffic = circuit->GetChokeMap()->GetTraffic();
	return viewsheds.count({traffic.IndexAt(pos.x, pos.z), cdef->GetId()}) > 0;
}

float CSensorPlanner::GetRisk(const AIFloat3& pos, float radius) const
{
	float risk = 0.f;
	const int frame = circuit->GetLastFrame();
	for (const SEvent& d : deaths) {
		if (d.pos.SqDistance2D(pos) < SQUARE(radius)) {
			risk += std::exp2(-float(frame - d.frame) / DEATH_HALF);
		}
	}
	return risk;
}

} // namespace circuit

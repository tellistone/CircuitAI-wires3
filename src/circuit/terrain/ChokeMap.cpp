/*
 * ChokeMap.cpp
 *
 *  Created on: Nov 11, 2026
 *      Author: Claude
 */

#include "terrain/ChokeMap.h"
#include "terrain/ChokeOverlay.h"
#include "terrain/TerrainData.h"
#include "terrain/TerrainManager.h"
#include "module/MilitaryManager.h"
#include "resource/MetalManager.h"
#include "setup/DefenceMatrix.h"
#include "setup/SetupManager.h"
#include "unit/CircuitDef.h"
#include "CircuitAI.h"
#include "util/GameAttribute.h"
#include "util/Scheduler.h"
#include "util/Utils.h"
#include "json/json.h"

#include "spring/SpringMap.h"

#include "Log.h"

#include "MoveData.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>

namespace circuit {

using namespace springai;

#define CHOKE_UPDATE_RATE	(FRAMES_PER_SEC * 2)
#define SCAN_RATE			(FRAMES_PER_SEC * 5)
#define ANALYSIS_RATE		(FRAMES_PER_SEC * 10)
#define MASK_COOLDOWN		(FRAMES_PER_SEC * 30)
#define TRAFFIC_COOLDOWN	(FRAMES_PER_SEC * 60)
#define SOURCE_COOLDOWN		(FRAMES_PER_SEC * 120)
#define SETTLE_FRAMES		(FRAMES_PER_SEC * 20)
#define BLEND_COOLDOWN		(FRAMES_PER_SEC * 30)
#define GRAPH_CELL			32  // elmos: the graph's cells, two slope map pixels
#define TRAFFIC_CELL		64  // elmos: the traffic field's cells

// Everything a build job reads and writes. Filled on the main thread, run on the area worker, applied on the main
// thread: the worker never touches CChokeMap
struct CChokeMap::SBuildJob {
	struct SClassParams {
		int mobileId;
		float maxSlope, minElevation, maxElevation;
		bool canHover, canFloat;
		float jumpRange;
	};
	// Inputs
	bool isMasks = false;
	bool isGraph = false;
	int heightVersion = 0;
	int mapWidth = 0, mapHeight = 0;  // height map squares (8 elmos)
	FloatVec heightMap, slopeMap;
	bool waterIsHarmful = false, waterIsAVoid = false;
	std::vector<SClassParams> classes;
	std::vector<bool> isGating;  // per class
	std::vector<SCellMask> masks;  // the current ones: compared with, or used as they are
	SCellMask prevGraphMask;
	CRegionGraph::SParams graphParams;
	std::vector<int> trafficClasses;  // never naval: ships get no layer
	std::vector<MoveKey> keys;  // per class
	std::vector<std::vector<int>> sourceCells;  // per source group, traffic cells
	std::vector<CTrafficField::SDestination> destinations;
	CTrafficField::SParams trafficParams;
	// Outputs
	bool isMasksChanged = false;
	bool isGraphBuilt = false;
	CRegionGraph graph;
	std::vector<CTrafficField::SType> trafficTypes;
	std::vector<int> doneClasses;  // trafficClasses that got their layer
	float maskMs = 0.f, graphMs = 0.f, trafficMs = 0.f;
};

CChokeMap::CChokeMap(CCircuitAI* circuit)
		: circuit(circuit)
		, isRequested(false)
		, isReady(false)
		, isBuilding(false)
		, isGraphDirty(false)
		, isSourceDirty(false)
		, isOpennessDirty(true)
		, jobSerial(0)
		, version(0)
		, analysisHash(0)
		, mainRange(1.f)
		, laneVersion(0)
		, heightVersion(-1)
		, maskFrame(-MASK_COOLDOWN)
		, trafficFrame(-TRAFFIC_COOLDOWN)
		, sourceFrame(-SOURCE_COOLDOWN)
		, nextScanFrame(0)
		, nextAnalysisFrame(0)
		, isTrafficDirty(false)
		, isOpening(true)
		, enemyAllyCount(1)
		, blendFrame(0)
		, isStartKnown(false)
		, openness(0.5f)
		, opennessBand(Openness::SEMI_OPEN)
		, isOpennessSet(false)
{
	ReadConfig(circuit->GetSetupManager()->GetConfig());
	overlay = std::make_unique<CChokeOverlay>(this);
	circuit->GetScheduler()->RunOnInit(std::make_shared<CGameTask>(&CChokeMap::Init, this));
}

CChokeMap::~CChokeMap()
{
}

void CChokeMap::ReadConfig(const Json::Value& root)
{
	const Json::Value& choke = root["choke"];
	config.isChoke = choke.get("enable", false).asBool();
	const Json::Value& region = choke["region"];
	config.minArea = region.get("min_area", config.minArea).asFloat();
	config.merge = region.get("merge", config.merge).asFloat();
	config.defensible = choke.get("defensible", config.defensible).asFloat();
	config.entrances = choke.get("entrances", config.entrances).asInt();
	config.nearRanges = choke.get("near", config.nearRanges).asFloat();
	const Json::Value& setback = choke["setback"];
	config.setback[0] = setback.get((unsigned)0, config.setback[0]).asFloat();
	config.setback[1] = setback.get((unsigned)1, config.setback[1]).asFloat();
	config.lane = choke.get("lane", config.lane).asFloat();
	config.isExpand = choke.get("expand", false).asBool();
	config.gateShare = choke.get("gate_share", config.gateShare).asFloat();
	config.spread = choke["traffic"].get("spread", config.spread).asFloat();
	const Json::Value& bands = choke["openness"]["bands"];
	for (unsigned i = 0; i < 3; ++i) {
		config.bands[i] = bands.get(i, config.bands[i]).asFloat();
	}
	config.isSpot = choke["spot"].get("enable", false).asBool();
	const Json::Value& radar = choke["radar"];
	config.isRadar = radar.get("enable", false).asBool();
	config.radarGain = radar.get("min_gain", config.radarGain).asFloat();
	config.radarShare = radar.get("share", config.radarShare).asFloat();
	const Json::Value& objective = choke["objective"];
	config.isObjective = objective.get("enable", false).asBool();
	config.objGain = objective.get("gain", config.objGain).asFloat();
	config.objPower = objective.get("power", config.objPower).asFloat();
	config.objRetryFrames = objective.get("retry", config.objRetryFrames / FRAMES_PER_SEC).asFloat() * FRAMES_PER_SEC;
	config.isHold = root["quota"].get("hold_choke", false).asBool();
}

void CChokeMap::Init()
{
	mainRange = std::max(GetMainDef()->GetMaxRange(), (float)GRAPH_CELL);
	InitMoveClasses();
	InitTypeSet();
	InitSources();
	const int width = circuit->GetMap()->GetWidth() * SQUARE_SIZE / TRAFFIC_CELL;
	const int height = circuit->GetMap()->GetHeight() * SQUARE_SIZE / TRAFFIC_CELL;
	traffic.Init(width, height, TRAFFIC_CELL, {config.spread, mainRange * 0.5f});

	updateTask = std::make_shared<CGameTask>(&CChokeMap::Update, this);
	circuit->GetScheduler()->RunTaskEvery(updateTask, CHOKE_UPDATE_RATE, circuit->GetSkirmishAIId() % CHOKE_UPDATE_RATE);

	if (!config.IsAnyOn()) {
		return;  // nothing reads it: built when the overlay first asks
	}
	// Masks and graph on the first frame: the defence points split by region at their init, right after this one.
	// The traffic layers follow on the worker
	isRequested = true;
	QueueAllTraffic();
	std::shared_ptr<SBuildJob> job = MakeJob(true, true, {});
	RunJob(*job);
	ApplyJob(*job);
}

void CChokeMap::SetAuthority(CCircuitAI* authority)
{
	if (updateTask != nullptr) {
		circuit->GetScheduler()->RemoveTask(updateTask);
	}
	circuit = authority;
	// A job in flight finishes on the leaving AI's scheduler, which drops its result: start over on the next update
	isBuilding = false;
	++jobSerial;
	isGraphDirty = true;  // traffic classes stay pending until their layers are in
	if (updateTask != nullptr) {
		circuit->GetScheduler()->RunTaskEvery(updateTask, CHOKE_UPDATE_RATE, circuit->GetSkirmishAIId() % CHOKE_UPDATE_RATE);
	}
	overlay->SetAuthority(authority);
}

void CChokeMap::Request()
{
	if (isRequested) {
		return;
	}
	isRequested = true;
	isGraphDirty = true;
	heightVersion = -1;  // masks too
	QueueAllTraffic();  // the type set's layers, once the graph is in
	ScheduleBuilds();
}

void CChokeMap::Rebuild()
{
	traffic.Init(traffic.GetWidth(), traffic.GetHeight(), TRAFFIC_CELL, {config.spread, mainRange * 0.5f});
	isRequested = true;  // a key set before anything asked for the analysis builds it now
	isGraphDirty = true;
	QueueAllTraffic();
	ScheduleBuilds();
}

void CChokeMap::Reanalyze()
{
	if (isReady) {
		nextAnalysisFrame = circuit->GetLastFrame() + ANALYSIS_RATE;
		UpdateTerritory();
		Analyze();
	}
}

void CChokeMap::Update()
{
	if (!isRequested) {
		return;
	}
	const int frame = circuit->GetLastFrame();
	if (!isStartKnown && factories.empty()) {
		InitSources();  // our start wasn't known at init (fixed start positions)
		if (isStartKnown) {
			UpdateSides();
			if (isReady) {
				MakeDestinations();  // now with our base, and only on our side
			}
			QueueAllTraffic();  // the guessed sources too
			isOpennessDirty = true;
			nextAnalysisFrame = frame;
		}
	}
	if (isReady && (frame >= nextScanFrame)) {
		nextScanFrame = frame + SCAN_RATE;
		ScanEnemies();
		const STypeChange change = UpdateTypeSet();
		isGraphDirty |= change.isGating;
		for (int c : change.joined) {
			QueueTraffic(c);
		}
		for (int c : change.left) {
			traffic.RemoveType(moveClasses[c].key);
			pendingTraffic.erase(std::remove(pendingTraffic.begin(), pendingTraffic.end(), c), pendingTraffic.end());
		}
		if (!change.left.empty()) {
			Reblend();
		}
	}
	ScheduleBuilds();
	if (frame >= nextAnalysisFrame) {
		Reanalyze();
	}
	// Re-blend at most every 30 s, and only when a share moved by more than 0.1
	if (isReady && (frame >= blendFrame + BLEND_COOLDOWN)) {
		bool isMoved = false;
		for (const STypeState& ts : typeStates) {
			isMoved |= ts.isInSet && (std::fabs(ts.share - ts.blendShare) > 0.1f);
		}
		if (isMoved) {
			Reblend();
		}
	}
	overlay->Update();
}

/*
 * Movement classes: every mobile type a factory in the game can build, walkers and jumpers apart
 */
void CChokeMap::InitMoveClasses()
{
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	std::set<CCircuitDef::Id> buildable;
	for (CCircuitDef& cdef : circuit->GetCircuitDefs()) {
		if (!cdef.IsMobile() && !cdef.GetBuildOptions().empty()) {
			buildable.insert(cdef.GetBuildOptions().begin(), cdef.GetBuildOptions().end());
		}
	}
	for (CCircuitDef::Id id : buildable) {
		CCircuitDef* cdef = circuit->GetCircuitDef(id);
		if ((cdef == nullptr) || !cdef->IsMobile() || cdef->IsAbleToFly() || (cdef->GetMobileId() < 0)) {
			continue;
		}
		const STerrainMapMobileType* mt = terrainMgr->GetMobileTypeById(cdef->GetMobileId());
		const MoveKey key = KeyOf(cdef);
		const bool isJumper = (key % 2) == 1;
		auto it = std::find_if(moveClasses.begin(), moveClasses.end(), [key](const SMoveClass& mc) {
			return mc.key == key;
		});
		if (it != moveClasses.end()) {
			it->jumpRange = std::max(it->jumpRange, isJumper ? cdef->GetJumpRange() : 0.f);  // leaky errs safe
			continue;
		}
		SMoveClass mc;
		mc.key = key;
		mc.mobileId = cdef->GetMobileId();
		mc.jumpRange = isJumper ? cdef->GetJumpRange() : 0.f;
		if (!mt->canHover && (mt->maxElevation <= 0.f)) {
			mc.kind = MoveKind::NAVAL;  // ships and subs can't go on land
		} else if (isJumper) {
			mc.kind = MoveKind::JUMPER;
		} else if (mt->maxSlope >= 0.99f) {
			mc.kind = MoveKind::CLIMBER;  // spiders climb nearly every cliff
		} else if (mt->canHover) {
			mc.kind = MoveKind::HOVER;
		} else if (!mt->canFloat && (mt->minElevation < -SQUARE_SIZE * 5)) {
			mc.kind = MoveKind::AMPHIBIOUS;  // as CCircuitDef::IsAmphibious
		} else {
			mc.kind = MoveKind::GROUND;
		}
		mc.role = (mc.kind == MoveKind::NAVAL) ? MoveRole::NAVAL
				: ((mc.kind == MoveKind::GROUND) || (mc.kind == MoveKind::HOVER)) ? MoveRole::GATING
				: MoveRole::LEAK;
		mc.name = (mt->moveData != nullptr) ? mt->moveData->GetName() : utils::int_to_string(mc.mobileId, "type%i");
		if (isJumper) {
			mc.name += "-jump";
		}
		moveClasses.push_back(mc);
	}
	std::sort(moveClasses.begin(), moveClasses.end(), [](const SMoveClass& a, const SMoveClass& b) {
		return a.key < b.key;
	});
	classMasks.resize(moveClasses.size());

	std::ostringstream text;
	text << "CHOKE move classes:";
	for (const SMoveClass& mc : moveClasses) {
		text << " " << mc.name << ((mc.role == MoveRole::GATING) ? "(gating)" : (mc.role == MoveRole::LEAK) ? "(leak)" : "(naval)");
	}
	circuit->LOG("%s", text.str().c_str());
}

std::shared_ptr<CChokeMap::SBuildJob> CChokeMap::MakeJob(bool isMasks, bool isGraph, const std::vector<int>& trafficClasses)
{
	CTerrainData& terrainData = circuit->GetGameAttribute()->GetTerrainData();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	std::shared_ptr<SBuildJob> job = std::make_shared<SBuildJob>();
	job->isMasks = isMasks || classMasks.empty() || classMasks.front().cells.empty();
	job->isGraph = isGraph;
	job->heightVersion = terrainData.GetHeightVersion();
	if (job->isMasks) {
		CMap* map = circuit->GetMap();
		job->mapWidth = map->GetWidth();
		job->mapHeight = map->GetHeight();
		map->GetHeightMap(job->heightMap);
		map->GetSlopeMap(job->slopeMap);
	}
	job->waterIsHarmful = terrainData.waterIsHarmful;
	job->waterIsAVoid = terrainData.waterIsAVoid;
	for (const SMoveClass& mc : moveClasses) {
		const STerrainMapMobileType* mt = terrainMgr->GetMobileTypeById(mc.mobileId);
		job->classes.push_back({mc.mobileId, mt->maxSlope, mt->minElevation, mt->maxElevation, mt->canHover,
				mt->canFloat, mc.jumpRange});
		job->keys.push_back(mc.key);
	}
	for (size_t c = 0; c < moveClasses.size(); ++c) {
		job->isGating.push_back(IsGating(c));
	}
	job->masks = classMasks;
	job->prevGraphMask = graph.GetMask();
	job->graphParams = GetGraphParams();
	job->trafficClasses = trafficClasses;
	job->sourceCells = GetSourceCells();
	job->destinations = destinations;
	job->trafficParams = {config.spread, mainRange * 0.5f};
	return job;
}

void CChokeMap::RunJob(SBuildJob& job)
{
	using clock = std::chrono::steady_clock;
	auto ms = [](clock::time_point t0) {
		return std::chrono::duration<float, std::milli>(clock::now() - t0).count();
	};
	const size_t classCount = job.classes.size();

	/*
	 * Masks: a 32 elmo cell passes for a class when at least 2 of its 4 slope pixels pass the same slope and depth
	 * tests CTerrainData applies per sector
	 */
	if (job.isMasks) {
		const clock::time_point t0 = clock::now();
		const int hmW = job.mapWidth;
		const int smW = job.mapWidth / 2;
		const int smH = job.mapHeight / 2;
		const int width = job.mapWidth / 4;
		const int height = job.mapHeight / 4;
		std::vector<float> minH(smW * smH), maxH(smW * smH);
		for (int sz = 0; sz < smH; ++sz) {
			for (int sx = 0; sx < smW; ++sx) {
				const int h = (sz * 2) * hmW + sx * 2;
				const float a = job.heightMap[h], b = job.heightMap[h + 1];
				const float c = job.heightMap[h + hmW], d = job.heightMap[h + hmW + 1];
				minH[sz * smW + sx] = std::min({a, b, c, d});
				maxH[sz * smW + sx] = std::max({a, b, c, d});
			}
		}
		std::map<int, SCellMask> walkMasks;  // per mobile type: jumpers start from their walking mask
		for (size_t ci = 0; ci < classCount; ++ci) {
			const SBuildJob::SClassParams& cp = job.classes[ci];
			auto it = walkMasks.find(cp.mobileId);
			if (it == walkMasks.end()) {
				auto isPass = [&](int s) {
					const float slope = job.slopeMap[s];
					if (cp.canHover) {
						return (cp.maxElevation >= maxH[s]) && !job.waterIsAVoid && ((maxH[s] <= 0.f) || (cp.maxSlope >= slope));
					}
					if (cp.canFloat && (cp.maxElevation >= maxH[s]) && !job.waterIsHarmful
						&& ((maxH[s] <= 0.f) || (cp.maxSlope >= slope)))
					{
						return true;
					}
					return (cp.maxSlope >= slope) && (cp.minElevation <= minH[s]) && (cp.maxElevation >= maxH[s])
							&& (!job.waterIsHarmful || (minH[s] >= 0.f));
				};
				SCellMask mask;
				mask.Resize(width, height, GRAPH_CELL);
				for (int z = 0; z < height; ++z) {
					for (int x = 0; x < width; ++x) {
						const int s = (z * 2) * smW + x * 2;
						const int count = isPass(s) + isPass(s + 1) + isPass(s + smW) + isPass(s + smW + 1);
						mask.cells[z * width + x] = (count >= 2) ? 1 : 0;
					}
				}
				it = walkMasks.emplace(cp.mobileId, std::move(mask)).first;
			}
			SCellMask mask = it->second;
			if (cp.jumpRange > 0.f) {
				mask = mask.OpenThinWalls(cp.jumpRange);  // walls a jump crosses
			}
			job.isMasksChanged |= (ci >= job.masks.size()) || (job.masks[ci] != mask);
			if (ci < job.masks.size()) {
				job.masks[ci] = std::move(mask);
			} else {
				job.masks.push_back(std::move(mask));
			}
		}
		job.heightMap.clear();
		job.heightMap.shrink_to_fit();
		job.slopeMap.clear();
		job.slopeMap.shrink_to_fit();
		job.maskMs = ms(t0);
	}

	/*
	 * The threat graph, from the union of the gating classes. Rebuilt only when that union changed
	 */
	if (job.isGraph || job.isMasksChanged) {
		const clock::time_point t0 = clock::now();
		const SCellMask& any = job.masks.front();
		SCellMask threatMask;
		threatMask.Resize(any.width, any.height, any.cellSize);
		for (size_t ci = 0; ci < classCount; ++ci) {
			if (job.isGating[ci]) {
				threatMask.Unite(job.masks[ci]);
			}
		}
		if (job.isGraph || (threatMask != job.prevGraphMask)) {
			job.graph.Build(threatMask, job.graphParams);
			job.isGraphBuilt = true;
		}
		job.graphMs = ms(t0);
	}

	/*
	 * Traffic: the first class's layer, and a copy of it for each other class whose mask is the same at 64 elmos.
	 * The rest wait for the next job, so other work on the worker never waits long
	 */
	if (!job.trafficClasses.empty() && !job.destinations.empty()) {
		const clock::time_point t0 = clock::now();
		SCellMask built;  // the coarse mask flooded
		for (int ci : job.trafficClasses) {
			SCellMask coarse = job.masks[ci].Downsample(TRAFFIC_CELL / GRAPH_CELL, 2);
			if (job.trafficTypes.empty()) {
				// A source on a wall for this class (a factory on a slope): its nearest passable cell within 4 cells
				std::vector<CTrafficField::Cells> sources = job.sourceCells;
				for (CTrafficField::Cells& cells : sources) {
					for (int& cell : cells) {
						const int nearest = coarse.NearestPassable(cell, 4);
						cell = (nearest >= 0) ? nearest : cell;
					}
				}
				job.trafficTypes.push_back(CTrafficField::BuildType(job.keys[ci], coarse, sources, job.destinations,
						job.trafficParams));
				built = std::move(coarse);
			} else if (coarse == built) {
				CTrafficField::SType copy = job.trafficTypes.front();
				copy.key = job.keys[ci];
				job.trafficTypes.push_back(std::move(copy));
			} else {
				continue;
			}
			job.doneClasses.push_back(ci);
		}
		job.trafficMs = ms(t0);
	}
}

void CChokeMap::StartJob(const std::shared_ptr<SBuildJob>& job)
{
	isBuilding = true;
	const int serial = ++jobSerial;
	circuit->GetScheduler()->RunParallelTask(std::make_shared<CGameTask>([job]() { RunJob(*job); }),
			std::make_shared<CGameTask>([this, job, serial]() {
				if (serial == jobSerial) {  // else the authority moved on, and so did the state this job was made from
					ApplyJob(*job);
				}
			}));
}

void CChokeMap::ApplyJob(SBuildJob& job)
{
	isBuilding = false;
	const int frame = circuit->GetLastFrame();
	if (job.isMasks) {
		heightVersion = job.heightVersion;
		maskFrame = frame;
		if (job.isMasksChanged) {
			classMasks = std::move(job.masks);
			reachMask = classMasks.front();
			for (const SCellMask& m : classMasks) {
				reachMask.Unite(m);
			}
			isTrafficDirty |= isReady;
			isOpennessDirty = true;
		}
		circuit->LOG("CHOKE masks: %.1f ms%s", job.maskMs, job.isMasksChanged ? ", changed" : "");
	}
	if (job.isGraphBuilt) {
		if (isReady) {
			job.graph.CarryIdsFrom(graph, frame);
		}
		graph = std::move(job.graph);
		pointChokeDist.clear();  // made again for the new chokes
		isOpennessDirty = true;
		UpdateSides();
		MakeLanes();
		UpdateChokeTraffic();
		if (destinations.empty()) {
			MakeDestinations();
		}
		nextAnalysisFrame = frame;
		circuit->LOG("CHOKE graph: %i regions, %i chokes, %.1f ms on %ix%i cells", (int)graph.GetRegions().size(),
				(int)graph.GetChokes().size(), job.graphMs, graph.GetMask().width, graph.GetMask().height);
		overlay->OnGraphBuilt();
	}
	if (!job.trafficTypes.empty()) {
		for (CTrafficField::SType& t : job.trafficTypes) {
			traffic.SetType(std::move(t));
		}
		for (int c : job.doneClasses) {
			pendingTraffic.erase(std::remove(pendingTraffic.begin(), pendingTraffic.end(), c), pendingTraffic.end());
		}
		Reblend();
		circuit->LOG("CHOKE traffic: %i layers, %.1f ms", (int)job.trafficTypes.size(), job.trafficMs);
	}
	isReady = !graph.IsEmpty();
	++version;
	if (frame >= nextAnalysisFrame) {
		Reanalyze();
	}
	ScheduleBuilds();  // the next pending layer
}

void CChokeMap::ScheduleBuilds()
{
	if (!isRequested || isBuilding) {
		return;
	}
	const int frame = circuit->GetLastFrame();
	const int terrainVersion = circuit->GetGameAttribute()->GetTerrainData().GetHeightVersion();
	const bool isMasks = (terrainVersion != heightVersion) && (frame >= maskFrame + MASK_COOLDOWN);
	const bool isGraph = isGraphDirty || graph.IsEmpty();
	const bool isTraffic = isReady && !destinations.empty();
	if (isTraffic && ((isTrafficDirty && (frame >= trafficFrame + TRAFFIC_COOLDOWN))
		|| (isSourceDirty && (frame >= sourceFrame + SOURCE_COOLDOWN))))
	{
		QueueAllTraffic();
		isTrafficDirty = isSourceDirty = false;
		trafficFrame = sourceFrame = frame;
	}
	const std::vector<int> trafficClasses = isTraffic ? pendingTraffic : std::vector<int>();
	if (!isMasks && !isGraph && trafficClasses.empty()) {
		return;
	}
	isGraphDirty = false;  // the job takes it: a change from now on (gating, a key set) dirties it again
	StartJob(MakeJob(isMasks, isGraph, trafficClasses));
}

void CChokeMap::QueueTraffic(int classIdx)
{
	if ((moveClasses[classIdx].role != MoveRole::NAVAL)
		&& (std::find(pendingTraffic.begin(), pendingTraffic.end(), classIdx) == pendingTraffic.end()))
	{
		pendingTraffic.push_back(classIdx);
	}
}

void CChokeMap::QueueAllTraffic()
{
	for (size_t c = 0; c < moveClasses.size(); ++c) {
		if (typeStates[c].isInSet) {
			QueueTraffic(c);
		}
	}
}

/*
 * Lanes: a corridor choke.lane wide along the path through every choke narrower than three lanes, out to one lane
 * length either side of its line. Only with choke.enable, so building placement stays as it was without it
 */
void CChokeMap::MakeLanes()
{
	std::vector<SLane> newLanes;
	const SCellMask& mask = graph.GetMask();
	if (config.isChoke) {
		for (const CRegionGraph::SChoke& choke : graph.GetChokes()) {
			if (choke.width >= 3.f * config.lane) {
				continue;
			}
			// Across the line: from region A's cells beside it to region B's
			float ax = 0.f, az = 0.f, bx = 0.f, bz = 0.f;
			int na = 0, nb = 0;
			for (int c : choke.line) {
				ForEachNeighbour(mask, c, [&](int next, int) {
					const int r = graph.GetRegionAt(next);
					if (r == choke.regionA) {
						ax += mask.CentreX(next); az += mask.CentreZ(next); ++na;
					} else if (r == choke.regionB) {
						bx += mask.CentreX(next); bz += mask.CentreZ(next); ++nb;
					}
				});
			}
			AIFloat3 dir(1.f, 0.f, 0.f);
			if ((na > 0) && (nb > 0)) {
				const AIFloat3 d(bx / nb - ax / na, 0.f, bz / nb - az / na);
				if (d.SqLength2D() > 1.f) {
					dir = d / d.Length2D();
				}
			}
			SLane lane;
			lane.chokeId = choke.id;
			lane.centre = AIFloat3(mask.CentreX(choke.centre), 0.f, mask.CentreZ(choke.centre));
			lane.dir = dir;
			lane.halfLength = config.lane + GRAPH_CELL;
			lane.halfWidth = config.lane * 0.5f;
			newLanes.push_back(lane);
		}
	}
	auto isSame = [](const SLane& a, const SLane& b) {
		return (a.chokeId == b.chokeId) && (a.centre.SqDistance2D(b.centre) < 1.f) && (a.dir.SqDistance2D(b.dir) < 1e-4f)
				&& (a.halfLength == b.halfLength) && (a.halfWidth == b.halfWidth);
	};
	if (!std::equal(lanes.begin(), lanes.end(), newLanes.begin(), newLanes.end(), isSame)) {
		lanes = std::move(newLanes);
		++laneVersion;
	}
}

CRegionGraph::SParams CChokeMap::GetGraphParams() const
{
	CRegionGraph::SParams params;
	params.minArea = config.minArea * SQUARE(mainRange / GRAPH_CELL);  // cells
	params.merge = config.merge;
	params.defensibleWidth = config.defensible * mainRange;
	return params;
}

AIFloat3 CChokeMap::GetCellPos(int cell) const
{
	const SCellMask& mask = graph.GetMask();
	return AIFloat3(mask.CentreX(cell), circuit->GetTerrainManager()->GetHeightAt(mask.CentreX(cell), mask.CentreZ(cell)),
			mask.CentreZ(cell));
}

int CChokeMap::GetRegionAt(const AIFloat3& pos) const
{
	if (graph.IsEmpty()) {
		return CRegionGraph::NO_REGION;
	}
	// A spot on a wall cell (a mex on a ledge): the nearest passable cell's region within 2 cells
	const SCellMask& mask = graph.GetMask();
	const int cell = mask.NearestPassable(mask.IndexAt(pos.x, pos.z), 2);
	return (cell >= 0) ? graph.GetRegionAt(cell) : CRegionGraph::NO_REGION;
}

void CChokeMap::UpdateChokeTraffic()
{
	const SCellMask& mask = graph.GetMask();
	chokeTraffic.clear();
	for (const CRegionGraph::SChoke& choke : graph.GetChokes()) {
		std::set<int> cells;  // each traffic cell once: four graph cells share one
		for (int c : choke.line) {
			cells.insert(traffic.IndexAt(mask.CentreX(c), mask.CentreZ(c)));
		}
		float sum = 0.f;
		for (int t : cells) {
			sum += traffic.GetAt(t);
		}
		chokeTraffic.push_back(sum);
	}
}

bool CChokeMap::IsChokeSettled(int choke, int frame) const
{
	return frame >= graph.GetChokes()[choke].bornFrame + SETTLE_FRAMES;
}

int CChokeMap::FindMoveClass(const std::string& name) const
{
	auto lower = [](std::string s) {
		std::transform(s.begin(), s.end(), s.begin(), ::tolower);
		return s;
	};
	const std::string want = lower(name);
	for (size_t c = 0; c < moveClasses.size(); ++c) {
		if (lower(moveClasses[c].name) == want) {
			return c;
		}
	}
	CCircuitDef* cdef = circuit->GetCircuitDef(name.c_str());
	return (cdef != nullptr) ? ClassOf(cdef) : -1;
}

bool CChokeMap::IsGating(int classIdx) const
{
	const STypeState& ts = typeStates[classIdx];
	return ts.isInSet && ((moveClasses[classIdx].role == MoveRole::GATING) || ts.isPromoted);
}

const CChokeMap::SPointInfo* CChokeMap::GetPointInfo(int cluster, int index) const
{
	if ((cluster < 0) || (cluster >= (int)firstPointOf.size())) {
		return nullptr;
	}
	const size_t i = firstPointOf[cluster] + index;
	return ((i < pointInfos.size()) && (pointInfos[i].cluster == cluster)) ? &pointInfos[i] : nullptr;
}

bool CChokeMap::IsOnLane(const AIFloat3& pos) const
{
	return std::any_of(lanes.begin(), lanes.end(), [&pos](const SLane& lane) { return lane.Contains(pos.x, pos.z); });
}

CCircuitDef* CChokeMap::GetMainDef() const
{
	CMilitaryManager* militaryMgr = circuit->GetMilitaryManager();
	const std::vector<CCircuitDef*>& defenders = circuit->GetTerrainManager()->IsWaterMap()
			? militaryMgr->GetWaterDefenders() : militaryMgr->GetLandDefenders();
	return defenders.empty() ? militaryMgr->GetDefaultPorc() : defenders.front();
}

const char* CChokeMap::GetBandName(Openness band)
{
	switch (band) {
		case Openness::CHOKEPOINTED: return "chokepointed";
		case Openness::SEMI_OPEN: return "semi-open";
		default: return "open";
	}
}

} // namespace circuit

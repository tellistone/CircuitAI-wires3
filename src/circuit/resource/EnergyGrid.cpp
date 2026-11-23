/*
 * EnergyGrid.cpp
 *
 *  Created on: Apr 19, 2015
 *      Author: rlcevg
 */

#include "resource/EnergyGrid.h"
#include "resource/MetalManager.h"
#include "module/BuilderManager.h"
#include "module/EconomyManager.h"
#include "setup/SetupManager.h"
#include "terrain/TerrainManager.h"
#include "CircuitAI.h"
#include "util/Scheduler.h"
#include "util/Utils.h"
#include "json/json.h"
#include "lemon/kruskal.h"

#include <algorithm>
#include <unordered_map>

#include "spring/SpringMap.h"

#include "AISCommands.h"
#include "Log.h"
#ifdef DEBUG_VIS
#include "Figure.h"
#endif

namespace circuit {

using namespace springai;

class CEnergyGrid::SpanningNode : public lemon::MapBase<CEnergyGrid::SpanningGraph::Node, bool> {
public:
	SpanningNode(const std::vector<bool>& lc, const std::vector<CEnergyNode*>& nodes)
		: linkedClusters(lc), nodes(nodes)
	{}
	Value operator[](Key k) const {
		return (*this)[CMetalData::ClusterGraph::id(k)];
	}
	Value operator[](int u) const {
		return linkedClusters[u] && !nodes[u]->IsBeingBuilt();
	}
private:
	const std::vector<bool>& linkedClusters;
	const std::vector<CEnergyNode*>& nodes;
};

class CEnergyGrid::SpanningLink : public lemon::MapBase<CEnergyGrid::SpanningGraph::Edge, bool> {
public:
	SpanningLink(const CEnergyGrid::SpanningTree& st, const std::vector<CEnergyLink>& links)
		: spanningTree(st), links(links)
	{}
	Value operator[](Key k) const {
		return spanningTree.find(k) != spanningTree.end();
	}
private:
	const CEnergyGrid::SpanningTree& spanningTree;
	const std::vector<CEnergyLink>& links;
};

class CEnergyGrid::DetectNode : public lemon::MapBase<CEnergyGrid::SpanningGraph::Node, bool> {
public:
	DetectNode(const CEnergyGrid::SpanningGraph& graph,
			const std::vector<CEnergyNode*>& nodes,
			const std::vector<CEnergyLink>& links, CCircuitAI* c)
		: graph(graph), nodes(nodes), links(links)
	{}
	Value operator[](Key k) const {
		const CEnergyNode* node = nodes[CMetalData::ClusterGraph::id(k)];
		if (node->IsBeingBuilt() || !node->IsValid()) {
			return false;
		}
		if (!node->IsMexed()) {
			return true;
		}
		if (node->IsFinished()) {
			return false;
		}
		CEnergyGrid::SpanningGraph::IncEdgeIt edgeIt(graph, k);
		for (; edgeIt != lemon::INVALID; ++ edgeIt) {
			const CEnergyLink& link = links[CMetalData::ClusterGraph::id(edgeIt)];
			if (!link.IsFinished() && link.IsValid()) {
				return false;
			}
		}
		return true;
	}
private:
	const CEnergyGrid::SpanningGraph& graph;
	const std::vector<CEnergyNode*>& nodes;
	const std::vector<CEnergyLink>& links;
};

class CEnergyGrid::DetectLink : public lemon::MapBase<CEnergyGrid::SpanningGraph::Edge, bool> {
public:
	DetectLink(const std::vector<CEnergyLink>& links) : links(links) {}
	Value operator[](Key k) const {
		const CEnergyLink& link = links[CMetalData::ClusterGraph::id(k)];
		return !link.IsFinished() && !link.IsBeingBuilt() && link.IsValid();
	}
private:
	const std::vector<CEnergyLink>& links;
};

CEnergyGrid::CEnergyGrid(CCircuitAI* circuit)
		: circuit(circuit)
		, markFrame(-1)
		, gridNodesFrame(-FRAMES_PER_SEC)
		, isForceRebuild(false)
		, ownedFilter(nullptr)
		, ownedClusters(nullptr)
		, edgeCosts(nullptr)
		, nodeFilter(nullptr)
		, linkFilter(nullptr)
		, spanningGraph(nullptr)
		, spanningBfs(nullptr)
#ifdef DEBUG_VIS
		, figureGridId(-1)
		, figureInvalidId(-1)
		, figureFinishedId(-1)
		, figureBuildId(-1)
		, figureKruskalId(-1)
		, isVis(false)
		, toggleFrame(-1)
#endif
{
	circuit->GetScheduler()->RunOnInit(std::make_shared<CGameTask>(&CEnergyGrid::Init, this));

	for (CCircuitDef& cdef : circuit->GetCircuitDefs()) {
		const std::map<std::string, std::string>& customParams = cdef.GetDef()->GetCustomParams();
		auto it = customParams.find("pylonrange");
		if (it != customParams.end()) {
			pylonRanges[cdef.GetId()] = utils::string_to_float(it->second) - 1.f;
			cdef.SetIsPylon(true);
		}
		it = customParams.find("income_energy");
		if (it != customParams.end()) {
			energyIncomes[cdef.GetId()] = utils::string_to_float(it->second);
		} else if (customParams.find("windgen") != customParams.end()) {
			energyIncomes[cdef.GetId()] = -1.f;  // varies: its current output
		}
		it = customParams.find("neededlink");
		if (it != customParams.end()) {
			const float needed = utils::string_to_float(it->second);
			if (needed > 0.f) {
				neededLinks[cdef.GetId()] = needed;
			}
		}
	}

	ReadConfig();
}

CEnergyGrid::~CEnergyGrid()
{
	delete ownedFilter;
	delete ownedClusters;
	delete edgeCosts;

	delete nodeFilter;
	delete linkFilter;
	delete spanningGraph;
	delete spanningBfs;

	utils::free_clear(nodes);
}

void CEnergyGrid::ReadConfig()
{
	const Json::Value& root = circuit->GetSetupManager()->GetConfig();
	const std::string& cfgName = circuit->GetSetupManager()->GetConfigName();
	const Json::Value& pylon = root["economy"]["energy"]["pylon"];
	for (const Json::Value& pyl : pylon) {
		CCircuitDef* cdef = circuit->GetCircuitDef(pyl.asCString());
		if (cdef == nullptr) {
			circuit->LOG("CONFIG %s: has unknown UnitDef '%s'", cfgName.c_str(), pyl.asCString());
		} else {
			rangePylons[pylonRanges[cdef->GetId()]] = cdef->GetId();
		}
	}
}

void CEnergyGrid::Init()
{
	CMetalManager* metalMgr = circuit->GetMetalManager();
	const CMetalData::Metals& spots = metalMgr->GetSpots();
	const CMetalData::Clusters& clusters = metalMgr->GetClusters();
	const CMetalData::ClusterGraph& clusterGraph = metalMgr->GetClusterGraph();

	ownedFilter = new OwnedFilter(clusterGraph, false);
	ownedClusters = new OwnedGraph(clusterGraph, *ownedFilter);
	edgeCosts = new CMetalData::ClusterCostMap(clusterGraph);

	nodeFilter = new SpanningNode(linkedClusters, nodes);
	linkFilter = new SpanningLink(spanningTree, links);
	spanningGraph = new SpanningGraph(clusterGraph, *nodeFilter, *linkFilter);
	spanningBfs = new SpanningBFS(*spanningGraph);

	linkedClusters.resize(clusters.size(), false);
	const CMetalData::ClusterCostMap& costs = metalMgr->GetClusterEdgeCosts();

	auto closestSpot = [&spots](const CMetalData::MetalIndices& indices, const AIFloat3& pos) {
		int idxSpot = -1;
		float minDist = std::numeric_limits<float>::max();
		for (int i : indices) {
			float dist = spots[i].position.SqDistance2D(pos);
			if (dist < minDist) {
				minDist = dist;
				idxSpot = i;
			}
		}
		return idxSpot;
	};

	links.reserve(clusterGraph.edgeNum());
	for (int i = 0; i < clusterGraph.edgeNum(); ++i) {
		CMetalData::ClusterGraph::Edge edge = clusterGraph.edgeFromId(i);
		// NOTE: CMetalManager::GetClusterEdgeCosts() is not the same as distance between cluster mexes
		(*edgeCosts)[edge] = costs[edge];
		int idx0 = clusterGraph.id(clusterGraph.u(edge));
		int idx1 = clusterGraph.id(clusterGraph.v(edge));
		int idxSpot0 = closestSpot(clusters[idx0].idxSpots, clusters[idx1].position);
		int idxSpot1 = closestSpot(clusters[idx1].idxSpots, spots[idxSpot0].position);
		links.emplace_back(idx0, spots[idxSpot0].position, idx1, spots[idxSpot1].position);
	}

	nodes.reserve(clusterGraph.nodeNum());
	for (int i = 0; i < clusterGraph.nodeNum(); ++i) {
		nodes.push_back(new CEnergyNode(i, clusters[i], spots));
	}
}

void CEnergyGrid::Update()
{
	if (markFrame /*+ FRAMES_PER_SEC*/ >= circuit->GetLastFrame()) {
		return;
	}
	markFrame = circuit->GetLastFrame();

	circuit->UpdateFriendlyUnits();
	const CAllyTeam::AllyUnits& friendlies = circuit->GetFriendlyUnits();
	static std::vector<CAllyUnit*> tmpMexes;  // NOTE: micro-opt
	static std::vector<CAllyUnit*> tmpPylons;  // NOTE: micro-opt
//	tmpMexes.reserve(friendlies.size());
//	tmpPylons.reserve(friendlies.size());
	for (auto& kv : friendlies) {
		CAllyUnit* unit = kv.second;
		if (unit->GetUnit()->IsBeingBuilt()) {
			continue;
		}
		if (unit->GetCircuitDef()->IsMex()) {
			tmpMexes.push_back(unit);
		}
		if (unit->GetCircuitDef()->IsPylon()) {
			tmpPylons.push_back(unit);
		}
	}

	circuit->GetMetalManager()->MarkAllyMexes(tmpMexes);
	MarkClusters();
	MarkAllyPylons(tmpPylons);
	CheckGrid();
	RebuildTree();

	tmpMexes.clear();
	tmpPylons.clear();
}

IGridLink* CEnergyGrid::GetLinkToBuild(CCircuitDef*& outDef, AIFloat3& outPos)
{
	/*
	 * Detect link to build
	 */
	const AIFloat3& pos = circuit->GetSetupManager()->GetBasePos();
	int index = circuit->GetMetalManager()->FindNearestCluster(pos);
	if (index < 0 || !(*nodeFilter)[index]) {
		return nullptr;
	}

	DetectNode goalNode(*spanningGraph, nodes, links, circuit);
	DetectLink goalLink(links);
	spanningBfs->init();
	spanningBfs->addSource(spanningGraph->nodeFromId(index));
	// NOTE: usually 'start' doesn't examine source node, except this specific version
	auto target = spanningBfs->start(goalNode, goalLink);
	if (target.first != lemon::INVALID) {
		CEnergyNode* node = nodes[spanningGraph->id(target.first)];
		return FindNodeDef(outDef, outPos, node);
	}
	if (target.second != lemon::INVALID) {
		CEnergyLink* link = &links[spanningGraph->id(target.second)];
		return FindLinkDef(outDef, outPos, link);
	}
	return nullptr;
}

CEnergyGrid::SLinkSite CEnergyGrid::MakeLinkSite(float range, const AIFloat3& from, float fromRange,
		const AIFloat3& to, float toRange)
{
	const float gap = from.distance2D(to) - fromRange - toRange;
	return {from, to, fromRange + range, toRange + range, gap <= range * 2.f * 0.85f};
}

AIFloat3 CEnergyGrid::FindLinkSite(CCircuitDef* cdef, const SLinkSite& link, float searchRadius, const Predicate& predicate)
{
	const float dist = link.from.distance2D(link.to);
	AIFloat3 ideal = link.from;
	if (dist >= 1.f) {
		const float step = link.isLast ? (dist + link.fromReach - link.toReach) * 0.5f : link.fromReach * 0.9f;
		ideal += (link.to - link.from) / dist * step;
	}
	CTerrainManager::CorrectPosition(ideal);

	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	const float sqFrom = SQUARE(link.fromReach - SQUARE_SIZE);
	const float sqTo = SQUARE(link.toReach - SQUARE_SIZE);
	// A step out gets a quarter of the reach closer to to: one that can't (water, a cliff) fails, never piles up at from
	const float sqAhead = SQUARE(std::max(dist - link.fromReach * 0.25f, 0.f));
	CTerrainManager::TerrainPredicate isLinked = [&link, sqFrom, sqTo, sqAhead, &predicate](const AIFloat3& p) {
		return (p.SqDistance2D(link.from) < sqFrom) && (p.SqDistance2D(link.to) < (link.isLast ? sqTo : sqAhead))
			&& predicate(p);
	};
	AIFloat3 buildPos = terrainMgr->FindBuildSite(cdef, ideal, searchRadius, UNIT_COMMAND_BUILD_NO_FACING, isLinked);
	if (!utils::is_valid(buildPos) && link.isLast && (dist >= 1.f)) {  // no site touching both: one more step it is
		CTerrainManager::TerrainPredicate isFromLinked = [&link, sqFrom, sqAhead, &predicate](const AIFloat3& p) {
			return (p.SqDistance2D(link.from) < sqFrom) && (p.SqDistance2D(link.to) < sqAhead) && predicate(p);
		};
		buildPos = terrainMgr->FindBuildSite(cdef, ideal, searchRadius, UNIT_COMMAND_BUILD_NO_FACING, isFromLinked);
	}
	return buildPos;
}

IGridLink* CEnergyGrid::FindFillSite(CCircuitDef* cdef, const AIFloat3& pos, float maxDist, const Predicate& predicate,
		SLinkSite& outSite, AIFloat3& outPos)
{
	outPos = -RgtVector;
	const float range = GetPylonRange(cdef->GetId());
	if (range <= 0.f) {
		return nullptr;
	}
	Update();

	const std::vector<SPiece> queued = GetQueuedEnergy();
	unsigned bareCount;
	const std::vector<SFill> mexes = GetMexFills(range, pos, maxDist, queued, bareCount);
	const std::vector<SFill> gaps = GetGapFills(range, pos, maxDist, queued);
	const SFill* fill = TryFills(cdef, mexes.data(), mexes.data() + bareCount, predicate, outPos);  // 1. bare mexes
	if (fill == nullptr) {
		fill = TryFills(cdef, gaps.data(), gaps.data() + gaps.size(), predicate, outPos);  // 2. the plan's gaps
	}
	if (fill == nullptr) {  // 3. poor grids; not a base_only piece on land: after a mex's first, those only fill gaps
		CEconomyManager* economyMgr = circuit->GetEconomyManager();
		fill = TryFills(cdef, mexes.data() + bareCount, mexes.data() + mexes.size(), [&predicate, economyMgr, cdef](const AIFloat3& p) {
			return !economyMgr->IsBaseOnlyAt(cdef, p) && predicate(p);
		}, outPos);
	}
	if (fill == nullptr) {
		return nullptr;
	}
	outSite = fill->site;
	return fill->link;
}

const CEnergyGrid::SFill* CEnergyGrid::TryFills(CCircuitDef* cdef, const SFill* first, const SFill* last,
		const Predicate& predicate, AIFloat3& outPos)
{
	const int frame = circuit->GetLastFrame();
	for (int tries = 0; (first < last) && (tries < 4); ++first) {  // FindBuildSite is the costly part
		const int key = (cdef->GetId() << 20) | ((int(first->site.from.z) / 64) << 10) | (int(first->site.from.x) / 64);
		auto it = fillRetryFrames.find(key);
		if ((it != fillRetryFrames.end()) && (frame < it->second)) {
			continue;  // failed lately: skipped, without using up a try
		}
		++tries;
		outPos = FindLinkSite(cdef, first->site, first->site.fromReach * 0.5f, predicate);
		if (utils::is_valid(outPos)) {
			return first;
		}
		fillRetryFrames[key] = frame + FRAMES_PER_SEC * 30;
	}
	outPos = -RgtVector;
	return nullptr;
}

std::vector<CEnergyGrid::SPiece> CEnergyGrid::GetQueuedEnergy()
{
	std::vector<SPiece> pieces;
	CBuilderManager* builderMgr = circuit->GetBuilderManager();
	for (IBuilderTask::BuildType type : {IBuilderTask::BuildType::ENERGY, IBuilderTask::BuildType::PYLON}) {
		for (const IBuilderTask* t : builderMgr->GetTasks(type)) {
			const CCircuitDef::Id defId = t->GetBuildDef()->GetId();
			if (!IsEnergyDef(defId)) {
				continue;  // an energypylon's reach would cover everything around it
			}
			pieces.push_back(utils::is_valid(t->GetBuildPos())
					? SPiece{t->GetBuildPos(), GetPylonRange(defId)}
					: SPiece{t->GetPosition(), GetPylonRange(defId) + SQUARE_SIZE * 16});  // not placed yet: up to its shake off
		}
	}
	return pieces;
}

int CEnergyGrid::CountLinked(const std::vector<SPiece>& pieces, const AIFloat3& pos, float range)
{
	return std::count_if(pieces.begin(), pieces.end(), [&pos, range](const SPiece& c) {
		return c.pos.SqDistance2D(pos) < SQUARE(range + c.range);
	});
}

std::vector<CEnergyGrid::SFill> CEnergyGrid::GetMexFills(float range, const AIFloat3& pos, float maxDist,
		const std::vector<SPiece>& queued, unsigned& outBareCount)
{
	const int frame = circuit->GetLastFrame();
	const std::vector<SGridNode>& grid = GetGridNodes();

	// Energy of the ally team, being built too: the grids' snapshot lags
	std::vector<SPiece> pieces;
	for (const auto& kv : circuit->GetFriendlyUnits()) {  // fresh: Update
		const CCircuitDef* edef = kv.second->GetCircuitDef();
		if ((edef != nullptr) && IsEnergyDef(edef->GetId()) && (GetPylonRange(edef->GetId()) > 0.f)) {
			pieces.push_back({kv.second->GetPos(frame), GetPylonRange(edef->GetId())});
		}
	}

	std::vector<SFill> mexes;
	for (const auto& kv : circuit->GetTeamUnits()) {
		CCircuitUnit* unit = kv.second;
		if (!unit->GetCircuitDef()->IsMex() || unit->GetUnit()->IsBeingBuilt()) {
			continue;
		}
		const AIFloat3& p = unit->GetPos(frame);
		const float sqDist = p.SqDistance2D(pos);
		const float mexRange = GetPylonRange(unit->GetCircuitDef()->GetId());
		if ((sqDist >= SQUARE(maxDist)) || (CountLinked(queued, p, mexRange) > 0)) {
			continue;
		}
		auto it = gridNodeIndex.find(kv.first);
		const SGridNode* node = (it != gridNodeIndex.end()) ? &grid[it->second] : nullptr;
		if (node == nullptr) {  // not in the snapshot yet (just built, or no gridNumber yet): the grid it overlaps
			for (const SGridNode& n : grid) {
				if ((n.pos.SqDistance2D(p) < SQUARE(n.range + mexRange)) && ((node == nullptr) || (n.capacity > node->capacity))) {
					node = &n;
				}
			}
		}
		const int count = CountLinked(pieces, p, mexRange);
		if ((node == nullptr) && (count > 0)) {
			continue;  // energy beside it, of a grid the snapshot doesn't have yet
		}
		const float perMex = (node != nullptr) ? node->capacity / std::max(node->mexCount, 1) : 0.f;
		mexes.push_back({MakeLinkSite(range, p, mexRange, p, mexRange), nullptr, perMex, count, sqDist});
	}
	std::sort(mexes.begin(), mexes.end());  // so the bare ones (on no grid with energy, none beside) first, nearest first
	outBareCount = std::count_if(mexes.begin(), mexes.end(), [](const SFill& f) {
		return (f.perMex <= 0.f) && (f.pieces == 0);
	});
	return mexes;
}

std::vector<CEnergyGrid::SFill> CEnergyGrid::GetGapFills(float range, const AIFloat3& pos, float maxDist,
		const std::vector<SPiece>& queued)
{
	// Gap ends: a head (the building of the linked part nearest the other part) that grows towards the other head
	std::vector<SFill> gaps;
	auto addGap = [&gaps, &pos, maxDist, range](IGridLink* link, const AIFloat3& p0, float r0, const AIFloat3& p1, float r1) {
		const float sqDist = p0.SqDistance2D(pos);
		if ((sqDist < SQUARE(maxDist)) && (p0.SqDistance2D(p1) > SQUARE(r0 + r1))) {
			gaps.push_back({MakeLinkSite(range, p0, r0, p1, r1), link, 0.f, 0, sqDist});
		}
	};
	// Within a finished cluster. Not held as being built: that hides the cluster, and all its links, from the plan's
	// own linker (GetLinkToBuild); a queued energy job that links to the head holds it instead
	for (unsigned i = 0; i < nodes.size(); ++i) {
		CEnergyNode* node = nodes[i];
		if (!linkedClusters[i] || node->IsBeingBuilt() || !node->IsValid() || node->IsMexed()) {
			continue;
		}
		const CEnergyNode::SPylon& h0 = node->GetSourceHead();
		const CEnergyNode::SPylon& h1 = node->GetTargetHead();
		if (CountLinked(queued, h0.pos, h0.range) == 0) {
			addGap(nullptr, h0.pos, h0.range, h1.pos, h1.range);
		}
	}
	// The plan: links between finished clusters, held as being built by the job
	const CMetalData::ClusterGraph& clusterGraph = circuit->GetMetalManager()->GetClusterGraph();
	for (const CMetalData::ClusterGraph::Edge edge : spanningTree) {
		CEnergyLink& link = links[clusterGraph.id(edge)];
		if (!link.IsFinished() && !link.IsBeingBuilt() && link.IsValid()) {
			const CEnergyLink::SPylon* h0 = link.GetSourceHead();
			const CEnergyLink::SPylon* h1 = link.GetTargetHead();
			addGap(&link, h0->pos, h0->range, h1->pos, h1->range);
			addGap(&link, h1->pos, h1->range, h0->pos, h0->range);  // each end is on its own cluster's grid
		}
	}

	std::sort(gaps.begin(), gaps.end());  // nearest first
	return gaps;
}

float CEnergyGrid::GetPylonRange(CCircuitDef::Id defId)
{
	auto it = pylonRanges.find(defId);
	return (it != pylonRanges.end()) ? it->second : .0f;
}

float CEnergyGrid::GetNeededLink(CCircuitDef::Id defId) const
{
	auto it = neededLinks.find(defId);
	return (it != neededLinks.end()) ? it->second : .0f;
}

bool CEnergyGrid::IsOffGrid(Unit* u, bool isConsumer)
{
	return (!isConsumer && !u->IsActivated()) || u->IsParalyzed()
		|| (u->GetRulesParamFloat("disarmed", 0.f) > 0.f) || (u->GetRulesParamFloat("morphDisable", 0.f) > 0.f);
}

const std::vector<CEnergyGrid::SGridNode>& CEnergyGrid::GetGridNodes()
{
	const int frame = circuit->GetLastFrame();
	if (gridNodesFrame + FRAMES_PER_SEC > frame) {
		return gridNodes;
	}
	gridNodesFrame = frame;
	gridNodes.clear();
	gridNodeIndex.clear();

	// The overdrive gadget puts gridNumber on every grid building, and the grid's capacity (OD_gridMaximum) on its
	// energy buildings and pylons, not on mexes or consumers. All of a grid carry the same value: read it once per grid.
	// Off ones (IsOffGrid) are out of both, with a stale gridNumber: they get their grid from what they overlap.
	// Unfinished ones are left out (consumers' aside, which the game links while being built).
	// By def id: allied units' defs can belong to another AI instance on our ally team
	struct SRaw {
		float income;  // off ones: their energy when back on
		int gameGrid;  // 0 = off
		bool isMex;
	};
	std::vector<SRaw> raws;
	std::vector<int> offs;  // indices of off ones
	std::unordered_map<int, float> capacities;  // by gameGrid
	circuit->UpdateFriendlyUnits();
	for (const auto& kv : circuit->GetFriendlyUnits()) {
		CAllyUnit* unit = kv.second;
		const CCircuitDef* cdef = unit->GetCircuitDef();
		if (cdef == nullptr) {
			continue;
		}
		const float range = GetPylonRange(cdef->GetId());
		if (range <= 0.f) {
			continue;
		}
		Unit* u = unit->GetUnit();
		const bool isConsumer = GetNeededLink(cdef->GetId()) > 0.f;
		SRaw raw = {0.f, 0, cdef->IsMex()};
		if (IsOffGrid(u, isConsumer)) {
			if (u->IsBeingBuilt()) {
				continue;
			}
			auto it = energyIncomes.find(cdef->GetId());
			if (it != energyIncomes.end()) {
				raw.income = (it->second >= 0.f) ? it->second : u->GetRulesParamFloat("current_energyIncome", 0.f);
			}
			offs.push_back(raws.size());
		} else {
			raw.gameGrid = (int)u->GetRulesParamFloat("gridNumber", 0.f);
			if (raw.gameGrid <= 0) {
				continue;
			}
			if (!cdef->IsMex() && !isConsumer && (capacities.find(raw.gameGrid) == capacities.end())) {
				const float capacity = u->GetRulesParamFloat("OD_gridMaximum", -1.f);
				if (capacity >= 0.f) {
					capacities[raw.gameGrid] = capacity;
				}
			}
		}
		gridNodeIndex[unit->GetId()] = gridNodes.size();
		gridNodes.push_back({unit->GetId(), unit->GetPos(frame), range, raw.gameGrid, 0.f, 0});
		raws.push_back(raw);
	}

	// Union-find: on ones by the game's grid, off ones with everything they overlap
	std::vector<int> parent(raws.size());
	for (unsigned i = 0; i < parent.size(); ++i) {
		parent[i] = i;
	}
	std::function<int(int)> find = [&parent, &find](int i) {
		return (parent[i] == i) ? i : (parent[i] = find(parent[i]));
	};
	auto unite = [&parent, &find](int a, int b) {
		a = find(a);
		b = find(b);
		if (a != b) {
			parent[std::max(a, b)] = std::min(a, b);
		}
	};
	std::unordered_map<int, int> firstOfGrid;
	for (unsigned i = 0; i < raws.size(); ++i) {
		if (raws[i].gameGrid > 0) {
			auto it = firstOfGrid.emplace(raws[i].gameGrid, i).first;
			unite(i, it->second);
		}
	}
	for (int i : offs) {
		for (unsigned j = 0; j < gridNodes.size(); ++j) {
			if ((i != (int)j)
				&& (gridNodes[i].pos.SqDistance2D(gridNodes[j].pos) < SQUARE(gridNodes[i].range + gridNodes[j].range)))
			{
				unite(i, j);
			}
		}
	}

	// Per set: capacity = its game grids' capacities + its off ones' energy. Id = its lowest game grid, stable while
	// the game keeps its numbers (the watchdog compares them across snapshots); a set of off ones only: -(first unit id)
	struct SSet {
		int id;
		float capacity;
		int mexCount;
		std::set<int> gameGrids;
	};
	std::unordered_map<int, SSet> sets;
	for (unsigned i = 0; i < raws.size(); ++i) {
		SSet& set = sets.emplace(find(i), SSet{0, 0.f, 0, {}}).first->second;
		set.capacity += raws[i].income;
		set.mexCount += raws[i].isMex ? 1 : 0;
		if (raws[i].gameGrid > 0) {
			set.gameGrids.insert(raws[i].gameGrid);
		}
	}
	for (auto& kv : sets) {
		SSet& set = kv.second;
		for (int g : set.gameGrids) {
			auto it = capacities.find(g);
			if (it != capacities.end()) {
				set.capacity += it->second;
			}
		}
		set.id = set.gameGrids.empty() ? -gridNodes[kv.first].id : *set.gameGrids.begin();
	}
	for (unsigned i = 0; i < gridNodes.size(); ++i) {
		const SSet& set = sets.find(find(i))->second;
		gridNodes[i].grid = set.id;
		gridNodes[i].capacity = set.capacity;
		gridNodes[i].mexCount = set.mexCount;
	}
	return gridNodes;
}

CEnergyNode* CEnergyGrid::FindNodeDef(CCircuitDef*& outDef, AIFloat3& outPos, CEnergyNode* node)
{
	outDef = nullptr;
	outPos = -RgtVector;
	CEconomyManager* economyMgr = circuit->GetEconomyManager();
	const float searchRadius = economyMgr->GetPylonRange() * 0.5f;
	const int frame = circuit->GetLastFrame();
	const float metalIncome = economyMgr->GetAvgMetalIncome();

	const CCircuitDef* pylonDef = economyMgr->GetPylonDef();
	if (node->IsPylonable(economyMgr->GetPylonRange())
		&& pylonDef->IsAvailable(frame)
		&& (metalIncome > pylonDef->GetCostM() * 0.15f))
	{
		outDef = const_cast<CCircuitDef*>(pylonDef);
		outPos = circuit->GetTerrainManager()->FindBuildSite(outDef, node->GetCenterPos(), searchRadius, UNIT_COMMAND_BUILD_NO_FACING);
		return node;
	}

	const CEnergyNode::SPylon& pylon0 = node->GetSourceHead();
	const CEnergyNode::SPylon& pylon1 = node->GetTargetHead();

	decltype(rangePylons) candDefs = rangePylons;
	while (!candDefs.empty()) {
		CCircuitDef::Id defId = -1;
		float range = 0.f;
		auto it = candDefs.rbegin();
		for (; it != candDefs.rend(); ++it) {
			defId = it->second;
			range = it->first;
			const float dist = pylon0.range + pylon1.range + range/* * 1.2f*/;
			if (pylon0.pos.SqDistance2D(pylon1.pos) > SQUARE(dist)) {
				break;
			}
		}

		outDef = circuit->GetCircuitDef(defId);
		if ((outDef == nullptr) || !outDef->IsAvailable(frame)
			|| ((metalIncome < outDef->GetCostM() * 0.1f) && (outDef->GetCostM() > 100.f)))
		{
			candDefs.erase(range);
			continue;
		}

		// A site that links to the head and gets closer to the other end, as the fill's: the open cell nearest a fixed
		// spot could stand beside the head without linking or getting closer, and the next piece would go beside it
		const SLinkSite site = MakeLinkSite(range, pylon0.pos, pylon0.range, pylon1.pos, pylon1.range);
		outPos = FindLinkSite(outDef, site, site.fromReach * 0.5f, [economyMgr, outDef](const AIFloat3& p) {
			return economyMgr->IsAllowedAt(outDef, p);  // economy.energy.base_only: no wind away from the base
		});
		if (utils::is_valid(outPos) && circuit->GetBuilderManager()->IsBuilderInArea(outDef, outPos)) {
			break;
		} else {
			outPos = -RgtVector;
//			candDefs.erase(std::next(it).base());  // http://stackoverflow.com/questions/1830158/how-to-call-erase-with-a-reverse-iterator
			candDefs.erase(range);
		}
	}

	return node;
}

CEnergyLink* CEnergyGrid::FindLinkDef(CCircuitDef*& outDef, AIFloat3& outPos, CEnergyLink* link)
{
	outDef = nullptr;
	outPos = -RgtVector;
	CBuilderManager* builderMgr = circuit->GetBuilderManager();
	CEconomyManager* economyMgr = circuit->GetEconomyManager();

	const CEnergyLink::SPylon* pylon0 = link->GetSourceHead();
	const CEnergyLink::SPylon* pylon1 = link->GetTargetHead();

	const float metalIncome = economyMgr->GetAvgMetalIncome();
	const int frame = circuit->GetLastFrame();
	decltype(rangePylons) candDefs = rangePylons;
	while (!candDefs.empty()) {
		CCircuitDef::Id defId = -1;
		float range = 0.f;
		auto it = candDefs.rbegin();
		for (; it != candDefs.rend(); ++it) {
			defId = it->second;
			range = it->first;
			const float dist = pylon0->range + pylon1->range + range * 1.2f;
			if (pylon0->pos.SqDistance2D(pylon1->pos) > SQUARE(dist)) {
				break;
			}
		}

		outDef = circuit->GetCircuitDef(defId);
		if ((outDef == nullptr) || !outDef->IsAvailable(frame)
			|| ((metalIncome < outDef->GetCostM() * 0.2f) && (outDef->GetCostM() > 100.f)))
		{
			candDefs.erase(range);
			continue;
		}

		// A site that links to the head and gets closer to the other end (as in FindNodeDef)
		const SLinkSite site = MakeLinkSite(range, pylon0->pos, pylon0->range, pylon1->pos, pylon1->range);
		outPos = FindLinkSite(outDef, site, site.fromReach * 0.5f, [economyMgr, outDef](const AIFloat3& p) {
			return economyMgr->IsAllowedAt(outDef, p);  // economy.energy.base_only: no wind away from the base
		});
		if (utils::is_valid(outPos) && builderMgr->IsBuilderInArea(outDef, outPos)) {
			break;
		} else {
			outPos = -RgtVector;
//			candDefs.erase(std::next(it).base());  // http://stackoverflow.com/questions/1830158/how-to-call-erase-with-a-reverse-iterator
			candDefs.erase(range);
		}
	}

	return link;
}

void CEnergyGrid::MarkAllyPylons(const std::vector<CAllyUnit*>& pylons)
{
	decltype(markedPylons) prevUnits = std::move(markedPylons);
	markedPylons.clear();
	auto first1  = pylons.begin();
	auto last1   = pylons.end();
	auto first2  = prevUnits.begin();
	auto last2   = prevUnits.end();
	auto d_first = std::back_inserter(markedPylons);
	auto addPylon = [&d_first, this](CAllyUnit* unit) {
		std::vector<CEnergyNode*> ns;
		AddPylon(unit->GetId(), unit->GetCircuitDef()->GetId(), unit->GetPos(circuit->GetLastFrame()), ns);
		*d_first++ = std::make_pair(unit->GetId(), ns);
	};
	auto delPylon = [this](const std::pair<ICoreUnit::Id, std::vector<CEnergyNode*>>& pylonId) {
		RemovePylon(pylonId);
	};

	// @see std::set_symmetric_difference + std::set_intersection
	while (first1 != last1) {
		if (first2 == last2) {
			addPylon(*first1);  // everything else in first1..last1 is new units
			while (++first1 != last1) {
				addPylon(*first1);
			}
			break;
		}

		if ((*first1)->GetId() < first2->first) {
			addPylon(*first1);  // new unit
			++first1;  // advance pylons
		} else {
			if (first2->first < (*first1)->GetId()) {
				delPylon(*first2);  // dead unit
			} else {
				*d_first++ = *first2;  // old unit
				++first1;  // advance pylons
			}
			++first2;  // advance prevUnits
		}
	}
	while (first2 != last2) {  // everything else in first2..last2 is dead units
		delPylon(*first2++);
	}
}

void CEnergyGrid::AddPylon(const ICoreUnit::Id unitId, const CCircuitDef::Id defId, const AIFloat3& pos,
		std::vector<CEnergyNode*>& outNodes)
{
	CMetalManager* metalMgr = circuit->GetMetalManager();
	const CMetalData::ClusterGraph& clusterGraph = metalMgr->GetClusterGraph();

	// Find edges to which building belongs to
	const float range = pylonRanges[defId];
	CMetalData::ClusterGraph::EdgeIt edgeIt(clusterGraph);
	for (; edgeIt != lemon::INVALID; ++edgeIt) {
		int edgeIdx = clusterGraph.id(edgeIt);
		CEnergyLink& link = links[edgeIdx];
		const AIFloat3& P0 = link.GetSourcePos();
		const AIFloat3& P1 = link.GetTargetPos();

		const AIFloat3 midPos = (P0 + P1) * 0.5f;
		if (midPos.SqDistance2D(pos) < SQUARE(midPos.distance2D(P1) + range)) {
			link.AddPylon(unitId, pos, range);
			linkPylons.insert(edgeIdx);
		}
	}

	CMetalData::IndicesDists indices;
	metalMgr->FindSpotsInRadius(pos, range, indices);
	if (indices.empty()) {
		int index = metalMgr->FindNearestCluster(pos);
		if (index < 0) {
			return;
		}
		nodes[index]->AddPylon(unitId, pos, range);
		outNodes.push_back(nodes[index]);
		linkNodes.insert(nodes[index]);
	} else {
		for (const std::pair<int, float>& p : indices) {
			int index = metalMgr->GetCluster(p.first);
			if (nodes[index]->AddPylon(unitId, pos, range)) {
				outNodes.push_back(nodes[index]);
				linkNodes.insert(nodes[index]);
			}
		}
	}
}

void CEnergyGrid::RemovePylon(const std::pair<ICoreUnit::Id, std::vector<CEnergyNode*>>& pylonId)
{
	for (int i = 0; i < (int)links.size(); ++i) {
		if (links[i].RemovePylon(pylonId.first)) {
			unlinkPylons.insert(i);
		}
	}

	for (CEnergyNode* node : pylonId.second) {
		if (node->RemovePylon(pylonId.first)) {
			linkNodes.insert(node);
		}
	}
}

void CEnergyGrid::CheckGrid()
{
	if (linkPylons.empty() && unlinkPylons.empty()
		&& linkNodes.empty())
	{
		return;
	}

	for (const int edgeIdx : linkPylons) {
		CEnergyLink& link = links[edgeIdx];
		if (link.IsFinished()) {
			continue;
		}
		link.CheckConnection();
	}
	linkPylons.clear();

	for (const int edgeIdx : unlinkPylons) {
		links[edgeIdx].CheckConnection();  // calculates source, target
	}
	unlinkPylons.clear();

	for (CEnergyNode* node : linkNodes) {
		node->CheckConnection();
	}
	linkNodes.clear();
}

void CEnergyGrid::MarkClusters()
{
	const int size = circuit->GetMetalManager()->GetClusters().size();
	for (int index = 0; index < size; ++index) {
		bool isOur = circuit->GetMetalManager()->IsClusterFinished(index);
		if (linkedClusters[index] == isOur) {
			continue;
		}
		(isOur ? linkClusters : unlinkClusters).push_back(index);
		linkedClusters[index] = isOur;
	}
}

void CEnergyGrid::RebuildTree()
{
	if (linkClusters.empty() && unlinkClusters.empty() && !isForceRebuild) {
		return;
	}
	isForceRebuild = false;
	CMetalManager* metalMgr = circuit->GetMetalManager();
	const CMetalData::ClusterGraph& clusterGraph = metalMgr->GetClusterGraph();

	// Remove destroyed edges
	for (int index : unlinkClusters) {
		OwnedGraph::Node node = ownedClusters->nodeFromId(index);
		ownedClusters->disable(node);
	}
	unlinkClusters.clear();

	// Add new edges to Kruskal graph
	for (int index : linkClusters) {
		CMetalData::ClusterGraph::Node node = clusterGraph.nodeFromId(index);
		ownedClusters->enable(node);
		CMetalData::ClusterGraph::IncEdgeIt edgeIt(clusterGraph, node);
		for (; edgeIt != lemon::INVALID; ++edgeIt) {
			int idx0 = clusterGraph.id(clusterGraph.oppositeNode(node, edgeIt));
			if (linkedClusters[idx0]) {
				CEnergyLink& link = links[clusterGraph.id(edgeIt)];
				link.SetSource(idx0);
			}
		}
	}
	linkClusters.clear();

	const CMetalData::ClusterCostMap& costs = metalMgr->GetClusterEdgeCosts();
	for (const CMetalData::ClusterGraph::Edge edge : spanningTree) {
		CEnergyLink& link = links[clusterGraph.id(edge)];
		if (link.IsFinished() || link.IsBeingBuilt()) {
			// Mark used edges as const
			(*edgeCosts)[edge] = costs[edge] * MIN_COSTMOD;
		} else if (!link.IsValid()) {
			(*edgeCosts)[edge] = costs[edge] / MIN_COSTMOD;
		} else {
			(*edgeCosts)[edge] = costs[edge] * link.GetCostMod();
		}
	}

	// Build Kruskal's minimum spanning tree
	spanningTree.clear();
	lemon::kruskal(*ownedClusters, *edgeCosts, std::inserter(spanningTree, spanningTree.end()));
}

#ifdef DEBUG_VIS
void CEnergyGrid::UpdateVis()
{
	if (!isVis) {
		return;
	}

	CMap* map = circuit->GetMap();
	Figure* fig = circuit->GetDrawer()->GetFigure();

	fig->Remove(figureFinishedId);
	fig->Remove(figureBuildId);
	fig->Remove(figureInvalidId);
	fig->Remove(figureGridId);
	// create new figure groups
	figureFinishedId = fig->DrawLine(ZeroVector, ZeroVector, 0.0f, false, FRAMES_PER_SEC * 300, 0);
	figureBuildId    = fig->DrawLine(ZeroVector, ZeroVector, 0.0f, false, FRAMES_PER_SEC * 300, 0);
	figureInvalidId  = fig->DrawLine(ZeroVector, ZeroVector, 0.0f, false, FRAMES_PER_SEC * 300, 0);
	figureGridId     = fig->DrawLine(ZeroVector, ZeroVector, 0.0f, false, FRAMES_PER_SEC * 300, 0);
	for (const CEnergyLink& link : links) {
		int figureId;
		float height = 20.0f;
		if (link.IsFinished()) {
			figureId = figureFinishedId;
			height = 18.0f;
		} else if (link.IsBeingBuilt()) {
			figureId = figureBuildId;
		} else if (!link.IsValid()) {
			figureId = figureInvalidId;
		} else {
			figureId = figureGridId;
			height = 18.0f;
		}
		AIFloat3 pos0 = link.GetSourcePos();
		const AIFloat3 dir = (link.GetTargetPos() - pos0) / 10;
		pos0.y = map->GetElevationAt(pos0.x, pos0.z) + height;
		for (int i = 0; i < 10; ++i) {
			AIFloat3 pos1 = pos0 + dir;
			pos1.y = map->GetElevationAt(pos1.x, pos1.z) + height;
			fig->DrawLine(pos0, pos1, 16.0f, false, FRAMES_PER_SEC * 300, figureId);
			pos0 = pos1;
		}
	}
	fig->SetColor(figureFinishedId, AIColor(0.1f, 0.3f, 1.0f), 255);
	fig->SetColor(figureBuildId,    AIColor(1.0f, 1.0f, 0.0f), 255);
	fig->SetColor(figureInvalidId,  AIColor(1.0f, 0.3f, 0.3f), 255);
	fig->SetColor(figureGridId,     AIColor(0.5f, 0.5f, 0.5f), 255);

	// Draw planned Kruskal
	fig->Remove(figureKruskalId);
	figureKruskalId = fig->DrawLine(ZeroVector, ZeroVector, 0.0f, false, FRAMES_PER_SEC * 300, 0);
	const CMetalData::Clusters& clusters = circuit->GetMetalManager()->GetClusters();
	const CMetalData::ClusterGraph& clusterGraph = circuit->GetMetalManager()->GetClusterGraph();
	for (const CMetalData::ClusterGraph::Edge edge : spanningTree) {
		const AIFloat3& posFrom = clusters[clusterGraph.id(clusterGraph.u(edge))].position;
		const AIFloat3& posTo = clusters[clusterGraph.id(clusterGraph.v(edge))].position;
		AIFloat3 pos0 = posFrom;
		const AIFloat3 dir = (posTo - pos0) / 10;
		pos0.y = map->GetElevationAt(pos0.x, pos0.z) + 19.0f;
		for (int i = 0; i < 10; ++i) {
			AIFloat3 pos1 = pos0 + dir;
			pos1.y = map->GetElevationAt(pos1.x, pos1.z) + 19.0f;
			fig->DrawLine(pos0, pos1, 16.0f, false, FRAMES_PER_SEC * 300, figureKruskalId);
			pos0 = pos1;
		}
	}
	fig->SetColor(figureKruskalId, AIColor(0.0f, 1.0f, 1.0f), 255);

	delete fig;
}

void CEnergyGrid::ToggleVis()
{
	if (toggleFrame >= circuit->GetLastFrame()) {
		return;
	}
	toggleFrame = circuit->GetLastFrame();

	isVis = !isVis;
	if (isVis) {
		UpdateVis();
	} else {
		Figure* fig = circuit->GetDrawer()->GetFigure();
		fig->Remove(figureFinishedId);
		fig->Remove(figureBuildId);
		fig->Remove(figureInvalidId);
		fig->Remove(figureGridId);
		fig->Remove(figureKruskalId);
		delete fig;
	}
}

void CEnergyGrid::DrawNodePylons(const AIFloat3& pos)
{
	int index = circuit->GetMetalManager()->FindNearestCluster(pos);
	for (const auto& kv : nodes[index]->GetPylons()) {
		CAllyUnit* unit = circuit->GetFriendlyUnit(kv.first);
		const AIFloat3& p = unit->GetPos(circuit->GetLastFrame());
		circuit->GetDrawer()->DeletePointsAndLines(p);
	}
	for (const auto& kv : nodes[index]->GetPylons()) {
		CAllyUnit* unit = circuit->GetFriendlyUnit(kv.first);
		const AIFloat3& p = unit->GetPos(circuit->GetLastFrame());
		circuit->GetDrawer()->AddPoint(p, utils::int_to_string(index, "n: %i").c_str());
	}
}

void CEnergyGrid::DrawLinkPylons(const AIFloat3& pos)
{
	float minDist = std::numeric_limits<float>::max();
	int index = -1;
	for (int i = 0; i < (int)links.size(); ++i) {
		float dist = ((links[i].GetSourcePos() + links[i].GetTargetPos()) * 0.5f).SqDistance2D(pos);
		if (dist < minDist) {
			minDist = dist;
			index = i;
		}
	}
	for (const auto& kv : links[index].GetPylons()) {
		const AIFloat3& p = kv.second->pos;
		circuit->GetDrawer()->DeletePointsAndLines(p);
	}
	for (const auto& kv : links[index].GetPylons()) {
		const AIFloat3& p = kv.second->pos;
		circuit->GetDrawer()->AddPoint(p, utils::int_to_string(index, "l: %i").c_str());
	}
}
#endif

} // namespace circuit

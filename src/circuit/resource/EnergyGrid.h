/*
 * EnergyGrid.h
 *
 *  Created on: Apr 19, 2015
 *      Author: rlcevg
 */

#ifndef SRC_CIRCUIT_RESOURCE_ENERGYGRID_H_
#define SRC_CIRCUIT_RESOURCE_ENERGYGRID_H_

#include "resource/EnergyLink.h"
#include "resource/EnergyNode.h"
#include "resource/MetalData.h"
#include "unit/CircuitUnit.h"
#include "unit/CircuitDef.h"
#include "lemon/adaptors.h"
#include "lemon/bfs.h"

#include <functional>
#include <set>
#include <map>
#include <unordered_map>

namespace circuit {

class CCircuitAI;

class CEnergyGrid {
public:
	CEnergyGrid(CCircuitAI* circuit);
	virtual ~CEnergyGrid();

private:
	void ReadConfig();
	void Init();

public:
	void Update();
	void SetForceRebuild(bool value) { isForceRebuild = value; }
	IGridLink* GetLinkToBuild(CCircuitDef*& outDef, springai::AIFloat3& outPos);
	using Predicate = std::function<bool (const springai::AIFloat3& p)>;  // CTerrainManager::TerrainPredicate
	// Where a building links to the grid building at from towards the one at to. Reach = the sum of both link ranges
	struct SLinkSite {
		springai::AIFloat3 from, to;
		float fromReach, toReach;
		bool isLast;  // one piece closes the gap: it touches both; else a step out from from
	};
	// For a piece of link range <range> between grid buildings: isLast while the gap is within one pylon span
	// (0.85 x twice the range, as the grid-link watchdog plans). from == to: beside that one building
	static SLinkSite MakeLinkSite(float range, const springai::AIFloat3& from, float fromRange,
			const springai::AIFloat3& to, float toRange);
	// The open site for cdef nearest the ideal spot (midway through the overlap of both reaches when isLast, else 0.9
	// of from's reach out) that keeps those links (for the last piece, failing that, from's only) and passes
	// predicate; -RgtVector if none
	springai::AIFloat3 FindLinkSite(CCircuitDef* cdef, const SLinkSite& link, float searchRadius, Predicate& predicate);
	// economy.energy.fill: an open gap of the grid plan: a planned link between two finished mex clusters, not being
	// built, or the mexes of one finished cluster that aren't linked yet, with no energy job queued at its end. The gap
	// end nearest pos within maxDist whose link site for cdef (a building with pylonrange) passes predicate: outPos
	// (-RgtVector if none) and outSite. Returns the planned link for the job to hold; nullptr within a cluster or none
	IGridLink* FindGapToFill(CCircuitDef* cdef, const springai::AIFloat3& pos, float maxDist, Predicate& predicate,
			SLinkSite& outSite, springai::AIFloat3& outPos);
	// economy.energy.fill: a site for cdef beside one of our finished mexes within maxDist of pos, and its link.
	// isBare: the nearest mex on no grid with energy (the game's grids; energy of the ally team beside it, nanoframes
	// too, also counts, as the snapshot lags). Else the mex whose grid has the least energy per mex. Never a mex with
	// an energy job queued beside it. -RgtVector if none
	springai::AIFloat3 FindMexToFill(CCircuitDef* cdef, const springai::AIFloat3& pos, float maxDist, bool isBare,
			Predicate& predicate, SLinkSite& outSite);

	float GetPylonRange(CCircuitDef::Id defId);
	// Makes energy (customParams.income_energy or windgen)
	bool IsEnergyDef(CCircuitDef::Id defId) const { return energyIncomes.find(defId) != energyIncomes.end(); }
	// Grid energy a structure needs to work (Zero-K customParams.neededlink), 0 = none
	float GetNeededLink(CCircuitDef::Id defId) const;

	struct SGridNode {
		ICoreUnit::Id id;
		springai::AIFloat3 pos;
		float range;  // link range: two buildings link when closer than the sum of theirs
		int grid;  // its grid: the game's lowest gridNumber in it (off ones joined by overlap), < 0 = off ones only
		float capacity;  // that grid's energy capacity: OD_gridMaximum + its off ones' energy, 0 = none
	};
	// Out of the game's grid for now, as unit_mex_overdrive's status check has it: paralysed, disarmed, morphing,
	// or not active (a solar closes for 4 s when hit; nanoframes aren't active). Consumers count as active.
	// The game leaves such a unit's gridNumber stale
	static bool IsOffGrid(springai::Unit* u, bool isConsumer);
	// Finished grid buildings of our ally team (ours and allies'), grouped as the game does. Ones it took off the grid
	// for now (a closed solar, a stunned or morphing building) still count: each joins the grids it overlaps, which it
	// links again when back on, and adds its energy to their capacity. Cached for 1 s
	const std::vector<SGridNode>& GetGridNodes();

	void SetAuthority(CCircuitAI* authority) { circuit = authority; }

private:
	CEnergyNode* FindNodeDef(CCircuitDef*& outDef, springai::AIFloat3& outPos, CEnergyNode* node);
	CEnergyLink* FindLinkDef(CCircuitDef*& outDef, springai::AIFloat3& outPos, CEnergyLink* link);

	CCircuitAI* circuit;

	int markFrame;
	std::deque<std::pair<ICoreUnit::Id, std::vector<CEnergyNode*>>> markedPylons;  // sorted by insertion
	std::unordered_map<CCircuitDef::Id, float> pylonRanges;
	std::map<float, CCircuitDef::Id> rangePylons;
	std::unordered_map<CCircuitDef::Id, float> neededLinks;
	std::unordered_map<CCircuitDef::Id, float> energyIncomes;  // customParams.income_energy; wind: < 0

	std::vector<SGridNode> gridNodes;
	int gridNodesFrame;

	std::vector<bool> linkedClusters;
	std::set<int> linkPylons, unlinkPylons;
	std::set<CEnergyNode*> linkNodes;
	std::vector<CEnergyLink> links;  // Graph's exterior property
	std::vector<CEnergyNode*> nodes;  // Graph's exterior property

	void MarkAllyPylons(const std::vector<CAllyUnit*>& pylons);
	void AddPylon(const ICoreUnit::Id unitId, const CCircuitDef::Id defId, const springai::AIFloat3& pos,
			std::vector<CEnergyNode*>& outNodes);
	void RemovePylon(const std::pair<ICoreUnit::Id, std::vector<CEnergyNode*>>& pylonId);
	void CheckGrid();

	std::vector<int> linkClusters;
	std::vector<int> unlinkClusters;
	bool isForceRebuild;

	class SpanningNode;
	class SpanningLink;
	class DetectNode;
	class DetectLink;
	using OwnedFilter = CMetalData::ClusterGraph::NodeMap<bool>;
	using OwnedGraph = lemon::FilterNodes<const CMetalData::ClusterGraph, OwnedFilter>;
	using SpanningTree = std::set<CMetalData::ClusterGraph::Edge>;
	using SpanningGraph = lemon::SubGraph<const CMetalData::ClusterGraph, SpanningNode, SpanningLink>;
	using SpanningBFS = lemon::Bfs<SpanningGraph>;

	SpanningTree spanningTree;
	OwnedFilter* ownedFilter;
	OwnedGraph* ownedClusters;
	CMetalData::ClusterCostMap* edgeCosts;

	SpanningNode* nodeFilter;
	SpanningLink* linkFilter;
	SpanningGraph* spanningGraph;
	SpanningBFS* spanningBfs;  // breadth-first search

	void MarkClusters();
	void RebuildTree();

#ifdef DEBUG_VIS
private:
	int figureGridId;
	int figureInvalidId;
	int figureFinishedId;
	int figureBuildId;
	int figureKruskalId;
	bool isVis;
	int toggleFrame;
public:
	bool IsVis() const { return isVis; }
	void UpdateVis();
	void ToggleVis();
	void DrawNodePylons(const springai::AIFloat3& pos);
	void DrawLinkPylons(const springai::AIFloat3& pos);
#endif
};

} // namespace circuit

#endif // SRC_CIRCUIT_RESOURCE_ENERGYGRID_H_

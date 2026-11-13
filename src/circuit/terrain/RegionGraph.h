/*
 * RegionGraph.h
 *
 *  Created on: Nov 11, 2026
 *      Author: Claude
 */

#ifndef SRC_CIRCUIT_TERRAIN_REGIONGRAPH_H_
#define SRC_CIRCUIT_TERRAIN_REGIONGRAPH_H_

#include "terrain/CellMask.h"

#include <vector>

namespace circuit {

/*
 * Regions and the chokes between them, from a mask of passable cells (the method Brood War bots use):
 * - altitude: each passable cell's distance to the nearest wall (a 3-4 chamfer transform, the map edge is a wall);
 * - flood down: cells from highest altitude to lowest. A cell with no labelled neighbour starts a region, a cell
 *   next to one region joins it, a cell next to two or more joins the one it touches most and is a meeting cell of
 *   each pair. Meeting cells of a pair that don't touch are separate chokes;
 * - merge: two regions merge when a choke between them is nearly as wide as the smaller one (choke altitude >=
 *   merge x its highest altitude), and a region under minArea merges across its widest choke. Until nothing changes.
 * Every step breaks ties by cell index, so allied AIs and reruns get the same graph.
 *
 * Regions and chokes are used by index (0..n-1) within one build. Their ids stay the same across rebuilds where
 * the shape stays the same (CarryIdsFrom), for anything kept between builds
 */
class CRegionGraph {
public:
	struct SParams {
		float minArea = 0.f;  // cells: a smaller region merges into a neighbour
		float merge = 0.7f;  // share of the smaller region's highest altitude
		float defensibleWidth = 0.f;  // elmos: towers try to close chokes up to this wide
	};
	struct SRegion {
		int id;
		int area;  // cells
		int centre;  // its highest cell
		std::vector<int> chokes;  // indices of its chokes
	};
	struct SChoke {
		int id;
		int regionA, regionB;  // indices, A < B
		int centre;  // its highest cell, the middle of the gap
		float width;  // elmos, twice the centre's altitude
		std::vector<int> line;  // its meeting cells, ascending
		bool isDefensible;
		int bornFrame;  // first build it was found in (kept by CarryIdsFrom)
	};
	static constexpr int NO_REGION = -1;

	// The result of CutFrom: how many units of capacity separate the sources from the targets, and two cuts that do
	struct SCut {
		float flow = 0.f;
		bool isOpen = false;  // flow over the cap, or a region is both a source and a target: no cut
		std::vector<int> nearSource;  // chokes of the cut nearest the sources, ascending
		std::vector<int> nearTarget;  // chokes of the cut nearest the targets, ascending
	};

public:
	CRegionGraph();

	void Build(const SCellMask& mask, const SParams& params);
	// Ids from the previous build where the shape is the same: a region keeps the id of the old region holding at
	// least half its cells, a choke the id (and bornFrame) of the old choke whose line runs within a cell of at least
	// half of its own. Everything else gets a new id and bornFrame = frame
	void CarryIdsFrom(const CRegionGraph& prev, int frame);

	bool IsEmpty() const { return mask.cells.empty(); }
	const SCellMask& GetMask() const { return mask; }
	int GetRegionAt(int cell) const { return regionOf[cell]; }  // index or NO_REGION (a wall)
	int GetRegionAt(float x, float z) const { return regionOf[mask.IndexAt(x, z)]; }
	const std::vector<SRegion>& GetRegions() const { return regions; }
	const std::vector<SChoke>& GetChokes() const { return chokes; }
	int FindRegion(int id) const;  // index, or -1
	int FindChoke(int id) const;  // index, or -1
	int OtherSide(const SChoke& choke, int region) const {
		return (choke.regionA == region) ? choke.regionB : choke.regionA;
	}
	float GetLineDist(const SChoke& choke, float x, float z) const;  // elmos from (x, z) to its line's nearest cell

	/*
	 * Max-flow from source regions to target regions, one unit of capacity per choke as given (by choke index), and
	 * the two minimum cuts it finds: nearest the sources and nearest the targets. Stops once the flow passes maxFlow
	 * (isOpen, no cuts). Breaks ties by index, so the cuts are always the same
	 */
	SCut CutFrom(const std::vector<int>& sources, const std::vector<int>& targets, const std::vector<float>& capacity,
			float maxFlow) const;
	// Per region, the fewest chokes to cross from a source region, not crossing blocked chokes (by choke index, empty:
	// none); -1 out of reach
	std::vector<int> HopsFrom(const std::vector<int>& sources, const std::vector<bool>& isChokeBlocked = {}) const;
	std::vector<bool> ReachableFrom(const std::vector<int>& sources, const std::vector<bool>& isChokeBlocked) const;

private:
	void ComputeAltitude();
	float GetAltitude(int cell) const;  // elmos to the nearest wall

	SCellMask mask;
	std::vector<int> altitude;  // chamfer units: 3 per cell, 0 on walls
	std::vector<int> regionOf;  // region index per cell, NO_REGION on walls
	std::vector<SRegion> regions;
	std::vector<SChoke> chokes;
	int nextRegionId;
	int nextChokeId;
};

} // namespace circuit

#endif // SRC_CIRCUIT_TERRAIN_REGIONGRAPH_H_

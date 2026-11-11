/*
 * CellMask.h
 *
 *  Created on: Nov 11, 2026
 *      Author: Claude
 */

#ifndef SRC_CIRCUIT_TERRAIN_CELLMASK_H_
#define SRC_CIRCUIT_TERRAIN_CELLMASK_H_

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace circuit {

/*
 * A grid of square cells, each passable (1) or a wall (0), for one or more movement types. Row-major: cell (x, z)
 * is at z * width + x. The map edge counts as a wall. No engine types, so the region graph and the traffic field
 * built from it can be tested on their own
 */
struct SCellMask {
	int width = 0;
	int height = 0;
	int cellSize = 0;  // elmos
	std::vector<uint8_t> cells;

	void Resize(int w, int h, int size) {
		width = w;
		height = h;
		cellSize = size;
		cells.assign(w * h, 0);
	}
	int Size() const { return width * height; }
	bool IsIn(int x, int z) const { return (x >= 0) && (z >= 0) && (x < width) && (z < height); }
	bool IsPassable(int x, int z) const { return IsIn(x, z) && (cells[z * width + x] != 0); }
	bool IsPassable(int index) const { return cells[index] != 0; }
	int IndexAt(float x, float z) const {  // elmos to the cell under them, clamped to the grid
		int cx = int(x) / cellSize;
		int cz = int(z) / cellSize;
		cx = (cx < 0) ? 0 : (cx >= width) ? width - 1 : cx;
		cz = (cz < 0) ? 0 : (cz >= height) ? height - 1 : cz;
		return cz * width + cx;
	}
	float CentreX(int index) const { return (index % width + 0.5f) * cellSize; }
	float CentreZ(int index) const { return (index / width + 0.5f) * cellSize; }
	bool operator==(const SCellMask& o) const {
		return (width == o.width) && (height == o.height) && (cellSize == o.cellSize) && (cells == o.cells);
	}
	bool operator!=(const SCellMask& o) const { return !(*this == o); }

	// Unites another mask of the same size into this one
	void Unite(const SCellMask& o) {
		for (int i = 0; i < Size(); ++i) {
			cells[i] |= o.cells[i];
		}
	}
	// The passable cell nearest index within reach cells (straight-line distance, ties in cell order), -1 if none
	int NearestPassable(int index, int reach) const;
	// A coarser grid, factor x factor cells into one: a coarse cell passes when at least `need` of its cells pass
	SCellMask Downsample(int factor, int need) const;
	// What a jump of `span` elmos crosses: every run of wall cells along a row or a column with passable cells at
	// both ends at most `span` apart passes. Thicker walls, and walls at the map edge, stay
	SCellMask OpenThinWalls(float span) const;
};

/*
 * Calls f(x, z) for every cell of a width x height grid of size-elmo cells under the rectangle from (x0, z0) to
 * (x1, z1) in elmos, clamped to the grid
 */
template <typename F>
inline void ForEachCellIn(int width, int height, int size, float x0, float z0, float x1, float z1, F&& f)
{
	const int cx0 = std::max(int(x0) / size, 0);
	const int cz0 = std::max(int(z0) / size, 0);
	const int cx1 = std::min(int(x1) / size, width - 1);
	const int cz1 = std::min(int(z1) / size, height - 1);
	for (int z = cz0; z <= cz1; ++z) {
		for (int x = cx0; x <= cx1; ++x) {
			f(x, z);
		}
	}
}

/*
 * Walks the passable neighbours of a passable cell: 4 straight ones (cost 2), and the 4 diagonal ones whose two
 * shared straight neighbours both pass (cost 3), so nothing slips diagonally between two wall cells.
 * f(int neighbour, int cost). Order is fixed: same answers on every machine
 */
template <typename F>
inline void ForEachNeighbour(const SCellMask& m, int index, F&& f)
{
	const int x = index % m.width;
	const int z = index / m.width;
	const bool w = m.IsPassable(x - 1, z);
	const bool e = m.IsPassable(x + 1, z);
	const bool n = m.IsPassable(x, z - 1);
	const bool s = m.IsPassable(x, z + 1);
	if (n) { f(index - m.width, 2); }
	if (w) { f(index - 1, 2); }
	if (e) { f(index + 1, 2); }
	if (s) { f(index + m.width, 2); }
	if (n && w && m.IsPassable(x - 1, z - 1)) { f(index - m.width - 1, 3); }
	if (n && e && m.IsPassable(x + 1, z - 1)) { f(index - m.width + 1, 3); }
	if (s && w && m.IsPassable(x - 1, z + 1)) { f(index + m.width - 1, 3); }
	if (s && e && m.IsPassable(x + 1, z + 1)) { f(index + m.width + 1, 3); }
}

/*
 * Walking distance over passable cells from the source cells, in half cells: 2 per straight step and 3 per
 * diagonal one (integer chamfer costs, so every machine gets the same answer). A bucket queue makes it linear.
 * Cells beyond maxDist, walls and unreachable cells get UNREACHED. Sources on walls are skipped
 */
class CDistanceFlood {
public:
	static constexpr int UNREACHED = std::numeric_limits<int>::max();

	// outReached (optional): every cell given a distance, in the order they were settled
	static void Run(const SCellMask& mask, const std::vector<int>& sources, int maxDist, std::vector<int>& outDist,
			std::vector<int>* outReached = nullptr);
	// Run again on the same mask with the last run's dist and reached (both empty the first time): only the cells it
	// reached are reset, so many short floods on a big mask cost what they reach
	static void Rerun(const SCellMask& mask, const std::vector<int>& sources, int maxDist, std::vector<int>& dist,
			std::vector<int>& reached);
	// Elmos per unit of distance
	static float ToElmos(const SCellMask& mask, int dist) { return dist * 0.5f * mask.cellSize; }
	static int FromElmos(const SCellMask& mask, float elmos) { return int(elmos * 2.f / mask.cellSize); }

private:
	// dist: every cell UNREACHED
	static void Flood(const SCellMask& mask, const std::vector<int>& sources, int maxDist, std::vector<int>& dist,
			std::vector<int>* outReached);
};

} // namespace circuit

#endif // SRC_CIRCUIT_TERRAIN_CELLMASK_H_

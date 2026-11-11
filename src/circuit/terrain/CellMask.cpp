/*
 * CellMask.cpp
 *
 *  Created on: Nov 11, 2026
 *      Author: Claude
 */

#include "terrain/CellMask.h"

namespace circuit {

SCellMask SCellMask::Downsample(int factor, int need) const
{
	SCellMask out;
	out.Resize(width / factor, height / factor, cellSize * factor);
	for (int z = 0; z < out.height; ++z) {
		for (int x = 0; x < out.width; ++x) {
			int count = 0;
			for (int dz = 0; dz < factor; ++dz) {
				for (int dx = 0; dx < factor; ++dx) {
					count += cells[(z * factor + dz) * width + x * factor + dx];
				}
			}
			out.cells[z * out.width + x] = (count >= need) ? 1 : 0;
		}
	}
	return out;
}

SCellMask SCellMask::OpenThinWalls(float span) const
{
	SCellMask out = *this;
	const int maxSteps = int(span / cellSize);  // from passable cell to passable cell
	// Along each row (dx = 1) and each column (dz = 1), from its first cell
	auto openLine = [&](int x, int z, int dx, int dz) {
		int lastPass = -1;  // step of the last passable cell
		for (int k = 0; IsIn(x + k * dx, z + k * dz); ++k) {
			if (!IsPassable(x + k * dx, z + k * dz)) {
				continue;
			}
			if ((lastPass >= 0) && (k - lastPass > 1) && (k - lastPass <= maxSteps)) {
				for (int j = lastPass + 1; j < k; ++j) {
					out.cells[(z + j * dz) * width + x + j * dx] = 1;
				}
			}
			lastPass = k;
		}
	};
	for (int z = 0; z < height; ++z) {
		openLine(0, z, 1, 0);
	}
	for (int x = 0; x < width; ++x) {
		openLine(x, 0, 0, 1);
	}
	return out;
}

void CDistanceFlood::Run(const SCellMask& mask, const std::vector<int>& sources, int maxDist, std::vector<int>& outDist,
		std::vector<int>* outReached)
{
	outDist.assign(mask.Size(), UNREACHED);
	if (outReached != nullptr) {
		outReached->clear();
	}
	Flood(mask, sources, maxDist, outDist, outReached);
}

void CDistanceFlood::Rerun(const SCellMask& mask, const std::vector<int>& sources, int maxDist, std::vector<int>& dist,
		std::vector<int>& reached)
{
	if ((int)dist.size() != mask.Size()) {
		dist.assign(mask.Size(), UNREACHED);
	} else {
		for (int cell : reached) {  // every cell a flood gives a distance gets settled, so this is all of them
			dist[cell] = UNREACHED;
		}
	}
	reached.clear();
	Flood(mask, sources, maxDist, dist, &reached);
}

void CDistanceFlood::Flood(const SCellMask& mask, const std::vector<int>& sources, int maxDist, std::vector<int>& outDist,
		std::vector<int>* outReached)
{
	// Edge costs are 2 and 3, so a step from distance d lands in bucket (d + 2) % 4 or (d + 3) % 4, never in d's own
	std::vector<int> buckets[4];
	size_t pending = 0;
	for (int s : sources) {
		if (mask.IsPassable(s) && (outDist[s] != 0)) {
			outDist[s] = 0;
			buckets[0].push_back(s);
			++pending;
		}
	}
	for (int d = 0; pending > 0; ++d) {
		std::vector<int>& bucket = buckets[d & 3];
		for (int cell : bucket) {
			if (outDist[cell] != d) {
				continue;  // reached by a shorter step after it was queued
			}
			if (outReached != nullptr) {
				outReached->push_back(cell);
			}
			ForEachNeighbour(mask, cell, [&](int next, int cost) {
				const int nd = d + cost;
				if ((nd <= maxDist) && (nd < outDist[next])) {
					outDist[next] = nd;
					buckets[nd & 3].push_back(next);
					++pending;
				}
			});
		}
		pending -= bucket.size();
		bucket.clear();
	}
}

} // namespace circuit

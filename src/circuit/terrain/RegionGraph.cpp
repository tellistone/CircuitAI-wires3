/*
 * RegionGraph.cpp
 *
 *  Created on: Nov 11, 2026
 *      Author: Claude
 */

#include "terrain/RegionGraph.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <utility>

namespace circuit {

namespace {

// Meeting cells of one pair of initial regions (a < b)
struct SMeeting {
	int a, b;
	std::vector<int> cells;
	int maxAltitude = 0;  // chamfer units
};

// Initial regions merged so far
class CUnion {
public:
	CUnion(const std::vector<int>& altitudes, const std::vector<int>& areas)
		: parent(altitudes.size()), altitude(altitudes), area(areas)
	{
		for (size_t i = 0; i < parent.size(); ++i) {
			parent[i] = i;
		}
	}
	int Find(int i) {
		while (parent[i] != i) {
			parent[i] = parent[parent[i]];
			i = parent[i];
		}
		return i;
	}
	void Unite(int a, int b) {  // a and b are roots; the lower index stays the root
		if (b < a) {
			std::swap(a, b);
		}
		parent[b] = a;
		altitude[a] = std::max(altitude[a], altitude[b]);
		area[a] += area[b];
	}
	int Altitude(int root) const { return altitude[root]; }
	int Area(int root) const { return area[root]; }
	int Size() const { return parent.size(); }

private:
	std::vector<int> parent;
	std::vector<int> altitude;
	std::vector<int> area;
};

constexpr float INF_CAPACITY = 1e30f;
constexpr float EPS_CAPACITY = 1e-4f;

}  // namespace

CRegionGraph::CRegionGraph()
		: nextRegionId(0)
		, nextChokeId(0)
{
}

void CRegionGraph::Build(const SCellMask& m, const SParams& params)
{
	mask = m;
	regions.clear();
	chokes.clear();
	const int n = mask.Size();
	ComputeAltitude();

	/*
	 * Flood down: highest altitude first. Within one altitude, cells spread out from the regions they touch, ring
	 * by ring in cell order, so two regions meet in the middle of a corridor of even width, not at one end. Cells
	 * left over start new regions, again in cell order
	 */
	int topAltitude = 0;
	for (int a : altitude) {
		topAltitude = std::max(topAltitude, a);
	}
	std::vector<std::vector<int>> byAltitude(topAltitude + 1);
	for (int i = 0; i < n; ++i) {
		if (altitude[i] > 0) {
			byAltitude[altitude[i]].push_back(i);
		}
	}
	std::vector<int> label(n, NO_REGION);
	std::vector<uint8_t> isQueued(n, 0);
	std::vector<int> seedAltitude;
	std::vector<int> seedArea;
	std::vector<SMeeting> meetings;
	std::map<std::pair<int, int>, int> meetingOf;  // pair of initial regions -> index in meetings
	auto labelCell = [&](int cell, int alt) {
		int labels[8];
		int counts[8];
		int k = 0;
		ForEachNeighbour(mask, cell, [&](int next, int) {
			const int l = label[next];
			if (l == NO_REGION) {
				return;
			}
			for (int j = 0; j < k; ++j) {
				if (labels[j] == l) {
					++counts[j];
					return;
				}
			}
			labels[k] = l;
			counts[k++] = 1;
		});
		if (k == 0) {
			label[cell] = seedAltitude.size();
			seedAltitude.push_back(alt);
			seedArea.push_back(1);
			return;
		}
		int best = 0;
		for (int j = 1; j < k; ++j) {
			if ((counts[j] > counts[best]) || ((counts[j] == counts[best]) && (labels[j] < labels[best]))) {
				best = j;
			}
		}
		label[cell] = labels[best];
		++seedArea[labels[best]];
		for (int i = 0; i < k; ++i) {
			for (int j = i + 1; j < k; ++j) {
				const std::pair<int, int> key(std::min(labels[i], labels[j]), std::max(labels[i], labels[j]));
				auto it = meetingOf.find(key);
				if (it == meetingOf.end()) {
					it = meetingOf.emplace(key, meetings.size()).first;
					meetings.push_back({key.first, key.second, {}});
				}
				SMeeting& meeting = meetings[it->second];
				meeting.cells.push_back(cell);
				meeting.maxAltitude = std::max(meeting.maxAltitude, alt);
			}
		}
	};
	std::deque<int> queue;
	auto spread = [&](int alt) {  // labels the queued cells, then their unlabelled neighbours of the same altitude
		while (!queue.empty()) {
			const int cell = queue.front();
			queue.pop_front();
			labelCell(cell, alt);
			ForEachNeighbour(mask, cell, [&](int next, int) {
				if ((altitude[next] == alt) && !isQueued[next]) {
					isQueued[next] = 1;
					queue.push_back(next);
				}
			});
		}
	};
	for (int alt = topAltitude; alt > 0; --alt) {
		for (int cell : byAltitude[alt]) {
			bool isNextToRegion = false;
			ForEachNeighbour(mask, cell, [&](int next, int) {
				isNextToRegion |= (label[next] != NO_REGION);
			});
			if (isNextToRegion) {
				isQueued[cell] = 1;
				queue.push_back(cell);
			}
		}
		spread(alt);
		for (int cell : byAltitude[alt]) {
			if (!isQueued[cell]) {
				isQueued[cell] = 1;
				queue.push_back(cell);
				spread(alt);
			}
		}
	}

	/*
	 * Merge: wide chokes first, then regions too small to hold, until nothing changes
	 */
	CUnion regionSet(seedAltitude, seedArea);
	std::vector<int> wideFirst(meetings.size());
	for (size_t i = 0; i < meetings.size(); ++i) {
		wideFirst[i] = i;
	}
	std::stable_sort(wideFirst.begin(), wideFirst.end(), [&meetings](int l, int r) {
		return meetings[l].maxAltitude > meetings[r].maxAltitude;
	});
	for (bool isChanged = true; isChanged;) {
		isChanged = false;
		for (int mi : wideFirst) {
			const SMeeting& meeting = meetings[mi];
			const int ra = regionSet.Find(meeting.a);
			const int rb = regionSet.Find(meeting.b);
			if ((ra != rb)
				&& (meeting.maxAltitude >= params.merge * std::min(regionSet.Altitude(ra), regionSet.Altitude(rb))))
			{
				regionSet.Unite(ra, rb);
				isChanged = true;
			}
		}
		std::vector<int> smallFirst;
		for (int r = 0; r < regionSet.Size(); ++r) {
			if ((regionSet.Find(r) == r) && (regionSet.Area(r) < params.minArea)) {
				smallFirst.push_back(r);
			}
		}
		std::stable_sort(smallFirst.begin(), smallFirst.end(), [&regionSet](int l, int r) {
			return regionSet.Area(l) < regionSet.Area(r);
		});
		for (int r : smallFirst) {
			if ((regionSet.Find(r) != r) || (regionSet.Area(r) >= params.minArea)) {
				continue;  // merged into another one on this pass
			}
			int other = -1;  // across its first touching meeting in wideFirst: its widest choke
			for (int mi : wideFirst) {
				const int ra = regionSet.Find(meetings[mi].a);
				const int rb = regionSet.Find(meetings[mi].b);
				if ((ra != rb) && ((ra == r) || (rb == r))) {
					other = (ra == r) ? rb : ra;
					break;
				}
			}
			if (other >= 0) {
				regionSet.Unite(r, other);
				isChanged = true;
			}
		}
	}

	/*
	 * Final regions, numbered in cell order
	 */
	std::vector<int> indexOfRoot(regionSet.Size(), NO_REGION);
	regionOf.assign(n, NO_REGION);
	for (int i = 0; i < n; ++i) {
		if (label[i] == NO_REGION) {
			continue;
		}
		const int root = regionSet.Find(label[i]);
		if (indexOfRoot[root] == NO_REGION) {
			indexOfRoot[root] = regions.size();
			regions.push_back({-1, 0, i, {}});
		}
		const int r = indexOfRoot[root];
		regionOf[i] = r;
		SRegion& region = regions[r];
		++region.area;
		if (altitude[i] > altitude[region.centre]) {
			region.centre = i;
		}
	}

	/*
	 * Chokes: meeting cells of each pair of final regions, split where they don't touch
	 */
	std::map<std::pair<int, int>, std::vector<int>> cellsOfPair;
	for (const SMeeting& meeting : meetings) {
		const int a = indexOfRoot[regionSet.Find(meeting.a)];
		const int b = indexOfRoot[regionSet.Find(meeting.b)];
		if (a != b) {
			std::vector<int>& cells = cellsOfPair[std::make_pair(std::min(a, b), std::max(a, b))];
			cells.insert(cells.end(), meeting.cells.begin(), meeting.cells.end());
		}
	}
	std::vector<int> stamp(n, -1);  // which pair's line a cell is on, while splitting that pair
	int pairStamp = 0;
	for (auto& kv : cellsOfPair) {
		std::vector<int>& cells = kv.second;
		std::sort(cells.begin(), cells.end());
		cells.erase(std::unique(cells.begin(), cells.end()), cells.end());
		for (int c : cells) {
			stamp[c] = pairStamp;
		}
		for (int start : cells) {
			if (stamp[start] != pairStamp) {
				continue;  // already in a choke of this pair
			}
			SChoke choke;
			choke.id = -1;
			choke.regionA = kv.first.first;
			choke.regionB = kv.first.second;
			choke.centre = start;
			choke.bornFrame = std::numeric_limits<int>::min() / 2;  // found at the start: no wait (CarryIdsFrom sets new ones)
			std::deque<int> queue = {start};
			stamp[start] = -1;
			while (!queue.empty()) {
				const int c = queue.front();
				queue.pop_front();
				choke.line.push_back(c);
				const int x = c % mask.width;
				const int z = c / mask.width;
				for (int dz = -1; dz <= 1; ++dz) {
					for (int dx = -1; dx <= 1; ++dx) {
						if (mask.IsIn(x + dx, z + dz)) {
							const int next = c + dz * mask.width + dx;
							if (stamp[next] == pairStamp) {
								stamp[next] = -1;
								queue.push_back(next);
							}
						}
					}
				}
			}
			std::sort(choke.line.begin(), choke.line.end());
			for (int c : choke.line) {
				if (altitude[c] > altitude[choke.centre]) {
					choke.centre = c;
				}
			}
			choke.width = 2.f * GetAltitude(choke.centre);
			choke.isDefensible = (choke.width <= params.defensibleWidth);
			regions[choke.regionA].chokes.push_back(chokes.size());
			regions[choke.regionB].chokes.push_back(chokes.size());
			chokes.push_back(std::move(choke));
		}
		++pairStamp;
	}

	// Fresh ids; CarryIdsFrom replaces them where the previous build had the same shape
	for (SRegion& region : regions) {
		region.id = nextRegionId++;
	}
	for (SChoke& choke : chokes) {
		choke.id = nextChokeId++;
	}
}

void CRegionGraph::ComputeAltitude()
{
	// Two-pass 3-4 chamfer distance to the nearest wall. Outside the map counts as a wall
	const int w = mask.width;
	const int h = mask.height;
	altitude.assign(w * h, 0);
	constexpr int FAR = std::numeric_limits<int>::max() / 2;
	for (int i = 0; i < w * h; ++i) {
		altitude[i] = mask.IsPassable(i) ? FAR : 0;
	}
	auto at = [this, w, h](int x, int z) {
		return ((x < 0) || (z < 0) || (x >= w) || (z >= h)) ? 0 : altitude[z * w + x];
	};
	for (int z = 0; z < h; ++z) {
		for (int x = 0; x < w; ++x) {
			int& a = altitude[z * w + x];
			if (a > 0) {
				a = std::min({a, at(x - 1, z) + 3, at(x, z - 1) + 3, at(x - 1, z - 1) + 4, at(x + 1, z - 1) + 4});
			}
		}
	}
	for (int z = h - 1; z >= 0; --z) {
		for (int x = w - 1; x >= 0; --x) {
			int& a = altitude[z * w + x];
			if (a > 0) {
				a = std::min({a, at(x + 1, z) + 3, at(x, z + 1) + 3, at(x + 1, z + 1) + 4, at(x - 1, z + 1) + 4});
			}
		}
	}
}

float CRegionGraph::GetAltitude(int cell) const
{
	// Chamfer units are thirds of a cell, from cell centre to wall centre: half a cell less to the wall's edge
	return (altitude[cell] > 0) ? (altitude[cell] / 3.f - 0.5f) * mask.cellSize : 0.f;
}

float CRegionGraph::GetLineDist(const SChoke& choke, float x, float z) const
{
	float bestSq = std::numeric_limits<float>::max();
	for (int cell : choke.line) {
		const float dx = mask.CentreX(cell) - x;
		const float dz = mask.CentreZ(cell) - z;
		bestSq = std::min(bestSq, dx * dx + dz * dz);
	}
	return std::sqrt(bestSq);
}

int CRegionGraph::FindRegion(int id) const
{
	for (size_t i = 0; i < regions.size(); ++i) {
		if (regions[i].id == id) {
			return i;
		}
	}
	return -1;
}

int CRegionGraph::FindChoke(int id) const
{
	for (size_t i = 0; i < chokes.size(); ++i) {
		if (chokes[i].id == id) {
			return i;
		}
	}
	return -1;
}

void CRegionGraph::CarryIdsFrom(const CRegionGraph& prev, int frame)
{
	// Unmatched regions and chokes are numbered after the previous build's ids, so no id is ever given twice
	std::vector<bool> isRegionCarried(regions.size(), false);
	std::vector<bool> isChokeCarried(chokes.size(), false);
	auto numberTheRest = [&]() {
		nextRegionId = prev.nextRegionId;
		nextChokeId = prev.nextChokeId;
		for (size_t i = 0; i < regions.size(); ++i) {
			if (!isRegionCarried[i]) {
				regions[i].id = nextRegionId++;
			}
		}
		for (size_t i = 0; i < chokes.size(); ++i) {
			if (!isChokeCarried[i]) {
				chokes[i].id = nextChokeId++;
				chokes[i].bornFrame = frame;
			}
		}
	};
	if ((prev.mask.width != mask.width) || (prev.mask.height != mask.height)) {
		numberTheRest();
		return;
	}

	// Greedy matching, best overlap first: (overlap, new index, old index)
	using Match = std::pair<int, std::pair<int, int>>;
	auto assign = [](std::vector<Match>& matches, std::vector<bool>& isNewDone, size_t oldCount,
			const std::function<int (int)>& needOf, const std::function<void (int, int)>& take) {
		std::stable_sort(matches.begin(), matches.end(), [](const Match& l, const Match& r) {
			return l.first > r.first;
		});
		std::vector<bool> isOldUsed(oldCount, false);
		for (const Match& m : matches) {
			const int ni = m.second.first;
			const int oi = m.second.second;
			if (!isNewDone[ni] && !isOldUsed[oi] && (m.first * 2 >= needOf(ni))) {
				isNewDone[ni] = isOldUsed[oi] = true;
				take(ni, oi);
			}
		}
	};

	// Regions: shared cells, counted by runs of the same (new, old) pair along the rows
	std::map<std::pair<int, int>, int> shared;
	std::pair<int, int> runPair(NO_REGION, NO_REGION);
	int runLength = 0;
	for (int i = 0; i <= mask.Size(); ++i) {
		const std::pair<int, int> pair = (i < mask.Size())
				? std::make_pair(regionOf[i], prev.regionOf[i]) : std::make_pair(NO_REGION, NO_REGION);
		if (pair == runPair) {
			++runLength;
			continue;
		}
		if ((runPair.first != NO_REGION) && (runPair.second != NO_REGION)) {
			shared[runPair] += runLength;
		}
		runPair = pair;
		runLength = 1;
	}
	std::vector<Match> matches;
	for (const auto& kv : shared) {
		matches.push_back({kv.second, kv.first});
	}
	assign(matches, isRegionCarried, prev.regions.size(),
		[this](int ni) { return regions[ni].area; },
		[this, &prev](int ni, int oi) { regions[ni].id = prev.regions[oi].id; });

	// Chokes: line cells within a cell of an old line
	std::vector<int> oldLineOf(mask.Size(), -1);
	for (size_t oi = 0; oi < prev.chokes.size(); ++oi) {
		for (int c : prev.chokes[oi].line) {
			oldLineOf[c] = oi;
		}
	}
	std::map<std::pair<int, int>, int> nearCounts;
	for (size_t ni = 0; ni < chokes.size(); ++ni) {
		for (int c : chokes[ni].line) {
			const int x = c % mask.width;
			const int z = c / mask.width;
			int seen[9];
			int k = 0;
			for (int dz = -1; dz <= 1; ++dz) {
				for (int dx = -1; dx <= 1; ++dx) {
					if (!mask.IsIn(x + dx, z + dz)) {
						continue;
					}
					const int oi = oldLineOf[c + dz * mask.width + dx];
					if ((oi >= 0) && (std::find(seen, seen + k, oi) == seen + k)) {
						seen[k++] = oi;
						++nearCounts[std::make_pair((int)ni, oi)];
					}
				}
			}
		}
	}
	matches.clear();
	for (const auto& kv : nearCounts) {
		matches.push_back({kv.second, kv.first});
	}
	assign(matches, isChokeCarried, prev.chokes.size(),
		[this](int ni) { return (int)chokes[ni].line.size(); },
		[this, &prev](int ni, int oi) {
			chokes[ni].id = prev.chokes[oi].id;
			chokes[ni].bornFrame = prev.chokes[oi].bornFrame;
		});
	numberTheRest();
}

CRegionGraph::SCut CRegionGraph::CutFrom(const std::vector<int>& sources, const std::vector<int>& targets,
		const std::vector<float>& capacity, float maxFlow) const
{
	SCut result;
	const int regionCount = regions.size();
	std::vector<bool> isSource(regionCount, false);
	for (int s : sources) {
		isSource[s] = true;
	}
	for (int t : targets) {
		if (isSource[t]) {
			result.isOpen = true;
			result.flow = maxFlow + 1.f;
			return result;
		}
	}
	if (sources.empty() || targets.empty()) {
		return result;
	}

	// Residual graph: every choke is a pair of arcs, each the other's reverse; S and T join the sources and targets
	struct SArc {
		int to;
		float cap;
		int rev;  // index of the reverse arc in adj[to]
	};
	const int S = regionCount;
	const int T = regionCount + 1;
	std::vector<std::vector<SArc>> adj(regionCount + 2);
	auto addArcs = [&adj](int u, int v, float capUV, float capVU) {
		adj[u].push_back({v, capUV, (int)adj[v].size()});
		adj[v].push_back({u, capVU, (int)adj[u].size() - 1});
	};
	for (size_t c = 0; c < chokes.size(); ++c) {
		if (capacity[c] > 0.f) {
			addArcs(chokes[c].regionA, chokes[c].regionB, capacity[c], capacity[c]);
		}
	}
	for (int s : sources) {
		addArcs(S, s, INF_CAPACITY, 0.f);
	}
	for (int t : targets) {
		addArcs(t, T, INF_CAPACITY, 0.f);
	}

	// Edmonds-Karp: shortest augmenting paths, arcs in insertion order
	std::vector<std::pair<int, int>> via(regionCount + 2);  // node -> (previous node, arc index there)
	while (true) {
		std::vector<bool> isSeen(regionCount + 2, false);
		std::deque<int> queue = {S};
		isSeen[S] = true;
		while (!queue.empty() && !isSeen[T]) {
			const int u = queue.front();
			queue.pop_front();
			for (size_t k = 0; k < adj[u].size(); ++k) {
				const SArc& arc = adj[u][k];
				if ((arc.cap > EPS_CAPACITY) && !isSeen[arc.to]) {
					isSeen[arc.to] = true;
					via[arc.to] = std::make_pair(u, (int)k);
					queue.push_back(arc.to);
				}
			}
		}
		if (!isSeen[T]) {
			break;
		}
		float bottleneck = INF_CAPACITY;
		for (int v = T; v != S; v = via[v].first) {
			bottleneck = std::min(bottleneck, adj[via[v].first][via[v].second].cap);
		}
		for (int v = T; v != S; v = via[v].first) {
			SArc& arc = adj[via[v].first][via[v].second];
			arc.cap -= bottleneck;
			adj[v][arc.rev].cap += bottleneck;
		}
		result.flow += bottleneck;
		if (result.flow > maxFlow) {
			result.isOpen = true;
			return result;
		}
	}

	// Source side: reachable from S in the residual graph. Target side: can reach T in it
	std::vector<bool> isSourceSide(regionCount + 2, false);
	std::deque<int> queue = {S};
	isSourceSide[S] = true;
	while (!queue.empty()) {
		const int u = queue.front();
		queue.pop_front();
		for (const SArc& arc : adj[u]) {
			if ((arc.cap > EPS_CAPACITY) && !isSourceSide[arc.to]) {
				isSourceSide[arc.to] = true;
				queue.push_back(arc.to);
			}
		}
	}
	std::vector<bool> isTargetSide(regionCount + 2, false);
	queue = {T};
	isTargetSide[T] = true;
	while (!queue.empty()) {
		const int v = queue.front();
		queue.pop_front();
		for (const SArc& arc : adj[v]) {  // arc v->u; its reverse u->v is what u would use to reach v
			if ((adj[arc.to][arc.rev].cap > EPS_CAPACITY) && !isTargetSide[arc.to]) {
				isTargetSide[arc.to] = true;
				queue.push_back(arc.to);
			}
		}
	}
	for (size_t c = 0; c < chokes.size(); ++c) {
		if (capacity[c] <= 0.f) {
			continue;
		}
		const SChoke& choke = chokes[c];
		if (isSourceSide[choke.regionA] != isSourceSide[choke.regionB]) {
			result.nearSource.push_back(c);
		}
		if (isTargetSide[choke.regionA] != isTargetSide[choke.regionB]) {
			result.nearTarget.push_back(c);
		}
	}
	return result;
}

std::vector<int> CRegionGraph::HopsFrom(const std::vector<int>& sources, const std::vector<bool>& isChokeBlocked) const
{
	std::vector<int> hops(regions.size(), -1);
	std::deque<int> queue;
	for (int s : sources) {
		if (hops[s] < 0) {
			hops[s] = 0;
			queue.push_back(s);
		}
	}
	while (!queue.empty()) {
		const int r = queue.front();
		queue.pop_front();
		for (int c : regions[r].chokes) {
			const int next = OtherSide(chokes[c], r);
			if ((hops[next] < 0) && (isChokeBlocked.empty() || !isChokeBlocked[c])) {
				hops[next] = hops[r] + 1;
				queue.push_back(next);
			}
		}
	}
	return hops;
}

std::vector<bool> CRegionGraph::ReachableFrom(const std::vector<int>& sources, const std::vector<bool>& isChokeBlocked) const
{
	const std::vector<int> hops = HopsFrom(sources, isChokeBlocked);
	std::vector<bool> isReached(hops.size());
	for (size_t r = 0; r < hops.size(); ++r) {
		isReached[r] = (hops[r] >= 0);
	}
	return isReached;
}

} // namespace circuit

/*
 * ChokePockets.cpp
 *
 *  Created on: Nov 11, 2026
 *      Author: Claude
 */

// CChokeMap: map sides and territory, pockets and their cuts, leaks, the current and forward lines, openness, the
// traffic field's destinations, and the choke_overlay save fixture

#include "terrain/ChokeMap.h"
#include "resource/MetalManager.h"
#include "setup/DefenceMatrix.h"
#include "setup/SetupManager.h"
#include "unit/ally/AllyUnit.h"
#include "unit/enemy/EnemyManager.h"
#include "unit/CircuitDef.h"
#include "CircuitAI.h"
#include "util/FileSystem.h"
#include "util/Utils.h"
#include "json/json.h"

#include "spring/SpringCallback.h"
#include "spring/SpringMap.h"

#include "Log.h"

#include "DataDirs.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>

namespace circuit {

using namespace springai;

#define TERRITORY_JOIN	(FRAMES_PER_SEC * 20)
#define TERRITORY_LEAVE	(FRAMES_PER_SEC * 60)
#define MAX_DESTINATIONS	16

static constexpr float NO_LIMIT = std::numeric_limits<float>::max();

void CChokeMap::Reanalyze()
{
	if (isReady) {
		nextAnalysisFrame = circuit->GetLastFrame() + FRAMES_PER_SEC * 10;
		UpdateTerritory();
		Analyze();
	}
}

/*
 * Map sides, from start boxes and enemy factories only, so a passing raid or a sneaky mex never rewires gating
 */
void CChokeMap::UpdateSides()
{
	const size_t count = graph.GetRegions().size();
	isEnemySide.assign(count, false);
	isOurSide.assign(count, false);
	if (graph.IsEmpty()) {
		return;
	}
	const SCellMask& mask = graph.GetMask();
	auto markRect = [this, &mask](const SRect& r, std::vector<bool>& side) {
		const int x0 = std::max(int(r.x0) / mask.cellSize, 0);
		const int z0 = std::max(int(r.z0) / mask.cellSize, 0);
		const int x1 = std::min(int(r.x1) / mask.cellSize, mask.width - 1);
		const int z1 = std::min(int(r.z1) / mask.cellSize, mask.height - 1);
		for (int z = z0; z <= z1; ++z) {
			for (int x = x0; x <= x1; ++x) {
				const int region = graph.GetRegionAt(z * mask.width + x);
				if (region != CRegionGraph::NO_REGION) {
					side[region] = true;
				}
			}
		}
	};
	for (const SRect& r : ourBoxes) {
		markRect(r, isOurSide);
	}
	for (const SSourceGroup& g : sourceGroups) {
		for (const SRect& r : g.boxes) {
			markRect(r, isEnemySide);
		}
		for (const AIFloat3& pos : g.factoryPos) {
			const int region = GetRegionAt(pos);
			if (region != CRegionGraph::NO_REGION) {
				isEnemySide[region] = true;
			}
		}
	}
}

/*
 * Territory: regions holding a side's buildings, joining after 20 s and leaving 60 s after the last one went.
 * Keyed by region id, so it carries over a rebuild
 */
void CChokeMap::UpdateTerritory()
{
	const int frame = circuit->GetLastFrame();
	const std::vector<CRegionGraph::SRegion>& regions = graph.GetRegions();
	std::set<int> ours, theirs;
	for (const auto& kv : circuit->GetFriendlyUnits()) {
		CAllyUnit* unit = kv.second;
		if (!unit->GetCircuitDef()->IsMobile()) {
			const int region = GetRegionAt(unit->GetPos(frame));
			if (region != CRegionGraph::NO_REGION) {
				ours.insert(regions[region].id);
			}
		}
	}
	for (const auto& kv : circuit->GetEnemyManager()->GetEnemyUnits()) {
		const CEnemyUnit* e = kv.second;
		if ((e->GetCircuitDef() != nullptr) && !e->GetCircuitDef()->IsMobile() && !e->IsDead()) {
			const int region = GetRegionAt(e->GetPos());
			if (region != CRegionGraph::NO_REGION) {
				theirs.insert(regions[region].id);
			}
		}
	}
	auto update = [frame, &regions](std::map<int, STerritory>& hold, const std::set<int>& now, std::vector<bool>& out) {
		for (int id : now) {
			STerritory& t = hold[id];
			if (t.since < 0) {
				t.since = frame;
			}
			t.lastSeen = frame;
		}
		for (auto it = hold.begin(); it != hold.end();) {
			it = (frame > it->second.lastSeen + TERRITORY_LEAVE) ? hold.erase(it) : std::next(it);
		}
		out.assign(regions.size(), false);
		for (size_t r = 0; r < regions.size(); ++r) {
			auto it = hold.find(regions[r].id);
			out[r] = (it != hold.end()) && (frame >= it->second.since + TERRITORY_JOIN);
		}
	};
	update(ourHold, ours, isOurTerritory);
	update(enemyHold, theirs, isEnemyTerritory);
}

void CChokeMap::Analyze()
{
	AnalyzePoints();
	UpdateLeaks();
	for (SPointInfo& pi : pointInfos) {
		if (pi.pocket >= 0) {
			pi.isLeaky |= isAirSeen;
			pockets[pi.pocket].isLeaky |= pi.isLeaky;
		}
	}
	UpdateLines();
	if (isOpennessDirty) {
		UpdateOpenness();
	}
	++version;
}

/*
 * Every defence point of every cluster: its entrances from the enemy side, its pocket, its distances to the pocket's
 * chokes, and whether fire can reach its mexes from outside the pocket's cut
 */
void CChokeMap::AnalyzePoints()
{
	pointInfos.clear();
	pockets.clear();
	CDefenceMatrix* defence = circuit->GetAllyTeam()->GetDefenceMatrix().get();
	CMetalManager* metalMgr = circuit->GetMetalManager();
	const CMetalData::Metals& spots = metalMgr->GetSpots();
	const CMetalData::Clusters& clusters = metalMgr->GetClusters();
	if ((defence == nullptr) || (defence->GetClusterCount() != (int)clusters.size()) || graph.IsEmpty()) {
		return;  // before CDefenceMatrix::Init, which calls Reanalyze
	}
	const SCellMask& mask = graph.GetMask();
	const std::vector<CRegionGraph::SChoke>& chokes = graph.GetChokes();
	const std::vector<int> enemySide = RegionsWhere(isEnemySide);
	const std::vector<float> unitCapacity = ChokeCapacity(1.f, 1.f);
	const float nearDist = config.nearRanges * mainRange;

	// The points, their mexes (the cluster's spots nearest each point) and their regions
	std::vector<std::vector<int>> pointMexes;
	for (int k = 0; k < (int)clusters.size(); ++k) {
		const std::vector<CDefenceMatrix::SDefPoint>& points = defence->GetDefPoints(k);
		const size_t first = pointMexes.size();
		for (size_t i = 0; i < points.size(); ++i) {
			SPointInfo pi;
			pi.cluster = k;
			pi.index = i;
			pi.pos = points[i].position;
			pi.income = 0.f;
			pi.region = CRegionGraph::NO_REGION;
			pi.entrances = 0;
			pi.isOpen = false;
			pi.pocket = -1;
			pi.isExposed = false;
			pi.isLeaky = false;
			pointInfos.push_back(pi);
			pointMexes.emplace_back();
		}
		for (int s : clusters[k].idxSpots) {
			int best = -1;
			float bestSq = NO_LIMIT;
			for (size_t i = 0; i < points.size(); ++i) {
				const float sq = points[i].position.SqDistance2D(spots[s].position);
				if (!points[i].isGeo && (sq < bestSq)) {
					bestSq = sq;
					best = i;
				}
			}
			if (best >= 0) {
				pointMexes[first + best].push_back(s);
				pointInfos[first + best].income += spots[s].income;
			}
		}
	}
	for (size_t p = 0; p < pointInfos.size(); ++p) {
		SPointInfo& pi = pointInfos[p];
		std::map<int, int> votes;  // region -> mexes in it
		for (int s : pointMexes[p]) {
			const int region = GetRegionAt(spots[s].position);
			if (region != CRegionGraph::NO_REGION) {
				++votes[region];
			}
		}
		int most = 0;
		for (const auto& kv : votes) {
			if (kv.second > most) {  // ties to the lower region
				most = kv.second;
				pi.region = kv.first;
			}
		}
		if (pi.region == CRegionGraph::NO_REGION) {
			pi.region = GetRegionAt(pi.pos);
		}
	}

	// Walking distances from each point to every choke, up to choke.near. Only change with the graph
	if ((distSerial != graphSerial) || (pointChokeDist.size() != pointInfos.size())) {
		distSerial = graphSerial;
		pointChokeDist.assign(pointInfos.size(), std::vector<float>(chokes.size(), -1.f));
		std::vector<int> dist, reached;
		const int maxDist = CDistanceFlood::FromElmos(mask, nearDist);
		for (size_t p = 0; p < pointInfos.size(); ++p) {
			std::vector<int> sources;
			for (int s : pointMexes[p]) {
				sources.push_back(mask.IndexAt(spots[s].position.x, spots[s].position.z));
			}
			sources.push_back(mask.IndexAt(pointInfos[p].pos.x, pointInfos[p].pos.z));
			CDistanceFlood::Rerun(mask, sources, maxDist, dist, reached);
			for (size_t c = 0; c < chokes.size(); ++c) {
				int best = CDistanceFlood::UNREACHED;
				for (int cell : chokes[c].line) {
					best = std::min(best, dist[cell]);
				}
				if (best != CDistanceFlood::UNREACHED) {
					pointChokeDist[p][c] = CDistanceFlood::ToElmos(mask, best);
				}
			}
		}
	}

	/*
	 * Entrances and cuts. The pocket cut is the enemy-nearest cut when it qualifies (every choke defensible, within
	 * choke.near, outer side not enemy territory), else the cluster-nearest cut
	 */
	auto innerOf = [this, &chokes](int region, const std::vector<int>& cut) {
		std::vector<bool> isBlocked(chokes.size(), false);
		for (int c : cut) {
			isBlocked[c] = true;
		}
		return graph.ReachableFrom({region}, isBlocked);
	};
	auto isQualified = [&](const std::vector<int>& cut, const std::vector<bool>& inner, const std::vector<size_t>& pts) {
		for (int c : cut) {
			const int outer = inner[chokes[c].regionA] ? chokes[c].regionB : chokes[c].regionA;
			const bool isNear = std::any_of(pts.begin(), pts.end(), [&](size_t p) {
				return (pointChokeDist[p][c] >= 0.f) && (pointChokeDist[p][c] <= nearDist);
			});
			if (!chokes[c].isDefensible || !isNear || isEnemyTerritory[outer]) {
				return false;
			}
		}
		return true;
	};
	struct SCandidate {
		std::vector<int> cut;
		std::vector<bool> inner;
		int innerCount;
	};
	std::vector<SCandidate> candidates;
	std::vector<int> pointCandidate(pointInfos.size(), -1);
	for (size_t p = 0; p < pointInfos.size(); ++p) {
		SPointInfo& pi = pointInfos[p];
		if (pi.region == CRegionGraph::NO_REGION) {
			continue;  // nothing walks there: stays on cluster
		}
		if (enemySide.empty() || isEnemySide[pi.region]) {
			pi.isOpen = true;  // the enemy's side, or it isn't known yet
			pi.entrances = config.entrances + 1;
			continue;
		}
		const CRegionGraph::SCut cut = graph.CutFrom(enemySide, {pi.region}, unitCapacity, config.entrances);
		pi.entrances = cut.isOpen ? config.entrances + 1 : (int)std::lround(cut.flow);
		pi.isOpen = cut.isOpen;
		if (pi.isOpen || (pi.entrances == 0)) {
			continue;
		}
		std::vector<bool> inner = innerOf(pi.region, cut.nearSource);
		std::vector<int> chosen = cut.nearSource;
		if (!isQualified(chosen, inner, {p})) {
			chosen = cut.nearTarget;
			inner = innerOf(pi.region, chosen);
		}
		auto it = std::find_if(candidates.begin(), candidates.end(), [&chosen](const SCandidate& c) {
			return c.cut == chosen;
		});
		if (it == candidates.end()) {
			candidates.push_back({chosen, inner, (int)std::count(inner.begin(), inner.end(), true)});
			it = candidates.end() - 1;
		}
		pointCandidate[p] = it - candidates.begin();
	}

	// A point behind another pocket's cut joins it: the pocket with the largest inner set holding the point's region
	// (ties to the lowest choke indices), so nested pockets aren't defended twice
	std::vector<int> order(candidates.size());
	for (size_t i = 0; i < order.size(); ++i) {
		order[i] = i;
	}
	std::sort(order.begin(), order.end(), [&candidates](int a, int b) {
		return (candidates[a].innerCount != candidates[b].innerCount)
				? (candidates[a].innerCount > candidates[b].innerCount) : (candidates[a].cut < candidates[b].cut);
	});
	std::vector<int> pocketOf(candidates.size(), -1);  // candidate -> pocket index
	for (size_t p = 0; p < pointInfos.size(); ++p) {
		if (pointCandidate[p] < 0) {
			continue;
		}
		for (int ci : order) {
			if (!candidates[ci].inner[pointInfos[p].region]) {
				continue;
			}
			if (pocketOf[ci] < 0) {
				pocketOf[ci] = pockets.size();
				SPocket pocket;
				pocket.chokes = candidates[ci].cut;
				for (int c : pocket.chokes) {
					pocket.chokeIds.push_back(chokes[c].id);
					pocket.outer.push_back(candidates[ci].inner[chokes[c].regionA] ? chokes[c].regionB : chokes[c].regionA);
				}
				std::sort(pocket.chokeIds.begin(), pocket.chokeIds.end());
				pocket.isInner = candidates[ci].inner;
				pocket.isQualified = false;
				pocket.isOuterEnemy = false;
				pocket.isLeaky = false;
				pocket.isHeld = false;
				pockets.push_back(pocket);
			}
			pointInfos[p].pocket = pocketOf[ci];
			pockets[pocketOf[ci]].points.push_back(p);
			break;
		}
	}

	// Per pocket: qualification, enemy outside, ours; per point: distances to its pocket's chokes and exposure
	SCellMask shipWater;
	if (!shipCells.empty()) {
		SCellMask naval;
		naval.Resize(mask.width, mask.height, mask.cellSize);
		for (size_t c = 0; c < moveClasses.size(); ++c) {
			if (moveClasses[c].role == MoveRole::NAVAL) {
				naval.Unite(classMasks[c]);
			}
		}
		std::vector<int> dist;
		CDistanceFlood::Run(naval, shipCells, CDistanceFlood::UNREACHED / 2, dist);
		shipWater.Resize(mask.width, mask.height, mask.cellSize);
		for (int i = 0; i < mask.Size(); ++i) {
			shipWater.cells[i] = (dist[i] != CDistanceFlood::UNREACHED);
		}
	}
	const int reach = int(config.exposure / mask.cellSize);
	for (SPocket& pocket : pockets) {
		std::vector<size_t> pts(pocket.points.begin(), pocket.points.end());
		pocket.isQualified = isQualified(pocket.chokes, pocket.isInner, pts);
		for (int outer : pocket.outer) {
			pocket.isOuterEnemy |= isEnemyTerritory[outer];
		}
		for (int p : pocket.points) {
			SPointInfo& pi = pointInfos[p];
			pocket.isHeld |= metalMgr->IsClusterQueued(pi.cluster) || metalMgr->IsClusterFinished(pi.cluster);
			for (int c : pocket.chokes) {
				pi.chokeDist.push_back(pointChokeDist[p][c]);
			}
			// Ground outside the cut, or water enemy ships reach, within choke.exposure of a mex
			std::vector<AIFloat3> assets;
			for (int s : pointMexes[p]) {
				assets.push_back(spots[s].position);
			}
			if (assets.empty()) {
				assets.push_back(pi.pos);
			}
			for (const AIFloat3& a : assets) {
				const int ax = int(a.x) / mask.cellSize, az = int(a.z) / mask.cellSize;
				for (int dz = -reach; (dz <= reach) && !pi.isExposed; ++dz) {
					for (int dx = -reach; dx <= reach; ++dx) {
						if ((dx * dx + dz * dz > reach * reach) || !mask.IsIn(ax + dx, az + dz)) {
							continue;
						}
						const int cell = (az + dz) * mask.width + ax + dx;
						const int region = graph.GetRegionAt(cell);
						if (((region != CRegionGraph::NO_REGION) && !pocket.isInner[region])
							|| (!shipWater.cells.empty() && shipWater.cells[cell]))
						{
							pi.isExposed = true;
							break;
						}
					}
				}
			}
			pi.isLeaky = pi.isExposed;
		}
	}
}

/*
 * Leak types: one flood over the union of every set type's mask, from the enemy side, with every pocket's cut
 * blocked. A pocket region it reaches is leaky
 */
void CChokeMap::UpdateLeaks()
{
	const SCellMask& mask = graph.GetMask();
	isLeakCell.assign(mask.Size(), false);
	if (pockets.empty()) {
		return;
	}
	SCellMask walk;
	walk.Resize(mask.width, mask.height, mask.cellSize);
	for (size_t c = 0; c < moveClasses.size(); ++c) {
		if (typeStates[c].isInSet) {
			walk.Unite(classMasks[c]);
		}
	}
	walk.Unite(threatMask);
	if (walk == threatMask) {
		return;  // no type crosses a cell the gating types can't: every cut holds
	}
	for (const SPocket& pocket : pockets) {
		for (int c : pocket.chokes) {
			for (int cell : graph.GetChokes()[c].line) {
				walk.cells[cell] = 0;
			}
		}
	}
	std::vector<int> seeds;
	for (int i = 0; i < mask.Size(); ++i) {
		const int region = graph.GetRegionAt(i);
		if ((region != CRegionGraph::NO_REGION) && isEnemySide[region]) {
			seeds.push_back(i);
		}
	}
	std::vector<int> dist;
	CDistanceFlood::Run(walk, seeds, CDistanceFlood::UNREACHED / 2, dist);
	std::vector<bool> isLeakRegion(graph.GetRegions().size(), false);
	for (int i = 0; i < mask.Size(); ++i) {
		if (dist[i] != CDistanceFlood::UNREACHED) {
			isLeakCell[i] = true;
			const int region = graph.GetRegionAt(i);
			if (region != CRegionGraph::NO_REGION) {
				isLeakRegion[region] = true;
			}
		}
	}
	for (SPointInfo& pi : pointInfos) {
		if ((pi.pocket >= 0) && isLeakRegion[pi.region]) {
			pi.isLeaky = true;
		}
	}
}

/*
 * The current line: the cut nearest our territory against enemy territory (C). The forward line: the cut nearest our
 * territory against the enemy side, through chokes the enemy may hold (D). Chokes too wide to defend count double.
 * Entrances from us: per region, the cut nearest it from our territory (E)
 */
void CChokeMap::UpdateLines()
{
	const std::vector<float> capacity = ChokeCapacity(1.f, 2.f);
	std::vector<int> ours = RegionsWhere(isOurTerritory);
	if (ours.empty()) {
		ours = RegionsWhere(isOurSide);
	}
	// A region both sides hold counts as ours: the line runs beyond our buildings there
	auto notOurs = [&ours](std::vector<int> regions) {
		regions.erase(std::remove_if(regions.begin(), regions.end(), [&ours](int r) {
			return std::find(ours.begin(), ours.end(), r) != ours.end();
		}), regions.end());
		return regions;
	};
	const std::vector<int> enemySide = notOurs(RegionsWhere(isEnemySide));
	std::vector<int> theirs = notOurs(RegionsWhere(isEnemyTerritory));
	if (theirs.empty()) {
		theirs = enemySide;
	}
	auto lineOf = [this, &capacity, &ours](const std::vector<int>& from) {
		const CRegionGraph::SCut cut = graph.CutFrom(from, ours, capacity, NO_LIMIT);
		return cut.isOpen ? std::vector<int>() : cut.nearTarget;
	};
	currentLine = lineOf(theirs);
	forwardLine = lineOf(enemySide);

	// Entrances from us: found when first asked, as only regions with enemy units are
	entranceSources = ours;
	entranceCapacity = ChokeCapacity(1.f, 1.f);
	entrancesFromUs.assign(graph.GetRegions().size(), std::vector<int>());
	isEntranceKnown.assign(graph.GetRegions().size(), false);
}

const std::vector<int>& CChokeMap::GetEntrancesFromUs(int region) const
{
	static const std::vector<int> none;
	if ((region < 0) || (region >= (int)isEntranceKnown.size())) {
		return none;  // before the first analysis of this graph
	}
	if (!isEntranceKnown[region]) {
		isEntranceKnown[region] = true;
		const CRegionGraph::SCut cut = graph.CutFrom(entranceSources, {region}, entranceCapacity, config.entrances);
		if (!cut.isOpen) {
			entrancesFromUs[region] = cut.nearTarget;
		}
	}
	return entrancesFromUs[region];
}

/*
 * openness = 0.5 x the share of mex income open from the enemy side + 0.5 x the width of the narrowest separation
 * between the sides over 4 main-tower ranges (capped at 1). Bands switch only 0.05 past a boundary
 */
void CChokeMap::UpdateOpenness()
{
	const std::vector<int> enemySide = RegionsWhere(isEnemySide);
	const std::vector<int> ourSide = RegionsWhere(isOurSide);
	if (pointInfos.empty() || enemySide.empty() || ourSide.empty()) {
		return;  // stays dirty until both sides and the defence points are known
	}
	isOpennessDirty = false;
	float total = 0.f, open = 0.f;
	for (const SPointInfo& pi : pointInfos) {
		total += pi.income;
		open += pi.isOpen ? pi.income : 0.f;
	}
	const float openShare = (total > 0.f) ? open / total : 1.f;
	std::vector<float> widths;
	for (const CRegionGraph::SChoke& choke : graph.GetChokes()) {
		widths.push_back(choke.width);
	}
	const float laneLimit = 4.f * mainRange;
	const CRegionGraph::SCut cut = graph.CutFrom(enemySide, ourSide, widths, laneLimit);
	const float lane = cut.isOpen ? 1.f : std::min(cut.flow / laneLimit, 1.f);
	openness = 0.5f * openShare + 0.5f * lane;

	auto bandOf = [this](float value) {
		return (value < config.bands[0]) ? Openness::CHOKEPOINTED
				: (value <= config.bands[1]) ? Openness::SEMI_OPEN : Openness::OPEN;
	};
	const Openness oldBand = opennessBand;
	if (!isOpennessSet) {
		opennessBand = bandOf(openness);
	} else if (bandOf(openness - config.bands[2]) > opennessBand) {
		opennessBand = bandOf(openness - config.bands[2]);
	} else if (bandOf(openness + config.bands[2]) < opennessBand) {
		opennessBand = bandOf(openness + config.bands[2]);
	}
	if (!isOpennessSet || (oldBand != opennessBand)) {
		circuit->LOG("CHOKE openness %.2f (%s: open income %.2f, lane %.2f) on %s", openness,
				GetBandName(opennessBand), openShare, lane, circuit->GetMap()->GetName());
	}
	isOpennessSet = true;
}

/*
 * Traffic destinations, fixed at the first build so the field doesn't churn as we expand: our base, valued like our
 * richest cluster, and every cluster closer to our side than to the enemy's, grouped by region and valued by income.
 * At most 16 groups: the smallest merges into its nearest
 */
void CChokeMap::MakeDestinations()
{
	destinations.clear();
	CMetalManager* metalMgr = circuit->GetMetalManager();
	const CMetalData::Metals& spots = metalMgr->GetSpots();
	const CMetalData::Clusters& clusters = metalMgr->GetClusters();
	auto distTo = [](const AIFloat3& p, const std::vector<SRect>& rects) {
		float best = NO_LIMIT;
		for (const SRect& r : rects) {
			const float dx = std::max({r.x0 - p.x, 0.f, p.x - r.x1});
			const float dz = std::max({r.z0 - p.z, 0.f, p.z - r.z1});
			best = std::min(best, dx * dx + dz * dz);
		}
		return best;
	};
	std::vector<SRect> enemyRects;
	for (const SSourceGroup& g : sourceGroups) {
		enemyRects.insert(enemyRects.end(), g.boxes.begin(), g.boxes.end());
	}
	const bool isSided = !ourBoxes.empty() && !enemyRects.empty();

	struct SGroup {
		int region;
		AIFloat3 pos;
		std::vector<int> cells;  // traffic cells
		float value;
	};
	std::vector<SGroup> groups;
	float richest = 0.f;
	for (const CMetalData::SCluster& cluster : clusters) {
		float income = 0.f;
		for (int s : cluster.idxSpots) {
			income += spots[s].income;
		}
		richest = std::max(richest, income);
		if (isSided && (distTo(cluster.position, ourBoxes) >= distTo(cluster.position, enemyRects))) {
			continue;
		}
		const int region = GetRegionAt(cluster.position);
		auto it = std::find_if(groups.begin(), groups.end(), [region](const SGroup& g) {
			return (region != CRegionGraph::NO_REGION) && (g.region == region);
		});
		if (it == groups.end()) {
			groups.push_back({region, cluster.position, {}, 0.f});
			it = groups.end() - 1;
		}
		for (int s : cluster.idxSpots) {
			it->cells.push_back(traffic.IndexAt(spots[s].position.x, spots[s].position.z));
		}
		it->value += income;
	}
	while (groups.size() > MAX_DESTINATIONS) {
		auto smallest = std::min_element(groups.begin(), groups.end(), [](const SGroup& a, const SGroup& b) {
			return a.value < b.value;
		});
		auto nearest = groups.end();
		float bestSq = NO_LIMIT;
		for (auto it = groups.begin(); it != groups.end(); ++it) {
			const float sq = it->pos.SqDistance2D(smallest->pos);
			if ((it != smallest) && (sq < bestSq)) {
				bestSq = sq;
				nearest = it;
			}
		}
		nearest->cells.insert(nearest->cells.end(), smallest->cells.begin(), smallest->cells.end());
		nearest->value += smallest->value;
		groups.erase(smallest);
	}
	const AIFloat3& basePos = circuit->GetSetupManager()->GetBasePos();
	if (utils::is_valid(basePos)) {
		groups.push_back({CRegionGraph::NO_REGION, basePos, {traffic.IndexAt(basePos.x, basePos.z)}, richest});
	}
	for (SGroup& g : groups) {
		std::sort(g.cells.begin(), g.cells.end());
		g.cells.erase(std::unique(g.cells.begin(), g.cells.end()), g.cells.end());
		destinations.push_back({g.cells, g.value});
	}
}

std::vector<float> CChokeMap::ChokeCapacity(float defensibleCap, float wideCap) const
{
	std::vector<float> capacity;
	for (const CRegionGraph::SChoke& choke : graph.GetChokes()) {
		capacity.push_back(choke.isDefensible ? defensibleCap : wideCap);
	}
	return capacity;
}

std::vector<int> CChokeMap::RegionsWhere(const std::vector<bool>& flags) const
{
	std::vector<int> result;
	for (size_t r = 0; r < flags.size(); ++r) {
		if (flags[r]) {
			result.push_back(r);
		}
	}
	return result;
}

/*
 * choke_overlay save: the graph's chokes and the pocket classes of the defence points, with the tuning values that
 * made them. Written by the opening default type set when nothing has been seen yet
 */
std::string CChokeMap::MakeFixture() const
{
	Json::Value root;
	root["map"] = circuit->GetMap()->GetName();
	root["min_area"] = config.minArea;
	root["merge"] = config.merge;
	root["defensible"] = config.defensible;
	root["main_range"] = mainRange;
	const SCellMask& mask = graph.GetMask();
	Json::Value& chokes = root["chokes"];
	chokes = Json::Value(Json::arrayValue);
	for (const CRegionGraph::SChoke& choke : graph.GetChokes()) {
		Json::Value c;
		c["x"] = int(mask.CentreX(choke.centre));
		c["z"] = int(mask.CentreZ(choke.centre));
		c["width"] = int(choke.width);
		c["defensible"] = choke.isDefensible;
		chokes.append(c);
	}
	Json::Value& points = root["points"];
	points = Json::Value(Json::arrayValue);
	for (const SPointInfo& pi : pointInfos) {
		Json::Value p;
		p["x"] = int(pi.pos.x);
		p["z"] = int(pi.pos.z);
		p["class"] = pi.isOpen ? "open" : (pi.pocket < 0) ? "unreached" : utils::int_to_string(pi.entrances, "gated %i");
		p["leaky"] = pi.isLeaky;
		points.append(p);
	}
	root["openness"] = std::round(openness * 100.f) / 100.f;
	Json::StreamWriterBuilder builder;
	builder["indentation"] = "\t";
	return Json::writeString(builder, root);
}

/*
 * Debug builds: compare with the saved fixture for this map, if there is one, and log what differs
 */
void CChokeMap::CompareFixture() const
{
	std::string path = "choke/" + utils::MakeFileSystemCompatible(circuit->GetMap()->GetName()) + ".json";
	DataDirs* dataDirs = circuit->GetCallback()->GetDataDirs();
	const bool isFound = utils::LocatePath(dataDirs, path);
	delete dataDirs;
	if (!isFound) {
		return;
	}
	std::ifstream file(path);
	Json::Value saved, now;
	Json::CharReaderBuilder builder;
	std::string errors;
	std::istringstream current(MakeFixture());
	if (!Json::parseFromStream(builder, file, &saved, &errors) || !Json::parseFromStream(builder, current, &now, &errors)) {
		circuit->LOG("CHOKE fixture %s: can't read (%s)", path.c_str(), errors.c_str());
		return;
	}
	int differences = 0;
	for (const char* key : {"chokes", "points"}) {
		const Json::Value& a = saved[key];
		const Json::Value& b = now[key];
		for (Json::ArrayIndex i = 0; i < std::max(a.size(), b.size()); ++i) {
			if ((i >= a.size()) || (i >= b.size()) || (a[i] != b[i])) {
				const std::string was = (i < a.size()) ? Json::writeString(Json::StreamWriterBuilder(), a[i]) : "none";
				const std::string is = (i < b.size()) ? Json::writeString(Json::StreamWriterBuilder(), b[i]) : "none";
				circuit->LOG("CHOKE fixture %s[%i]: was %s, now %s", key, (int)i, was.c_str(), is.c_str());
				++differences;
			}
		}
	}
	circuit->LOG("CHOKE fixture %s: %i differences", path.c_str(), differences);
}

} // namespace circuit

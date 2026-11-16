/*
 * ChokePockets.cpp
 *
 *  Created on: Nov 11, 2026
 *      Author: Claude
 */

// CChokeMap: map sides and territory, pockets and their cuts, the current and forward lines, openness, the traffic
// field's destinations, and the choke_overlay save fixture

#include "terrain/ChokeMap.h"
#include "resource/MetalManager.h"
#include "setup/DefenceMatrix.h"
#include "setup/SetupManager.h"
#include "unit/ally/AllyUnit.h"
#include "unit/enemy/EnemyManager.h"
#include "unit/CircuitDef.h"
#include "CircuitAI.h"
#include "util/Utils.h"
#include "json/json.h"

#include "spring/SpringMap.h"

#include "Log.h"

#include <algorithm>
#include <functional>
#include <iterator>
#include <cmath>
#include <limits>

namespace circuit {

using namespace springai;

#define TERRITORY_JOIN	(FRAMES_PER_SEC * 20)
#define OUR_LEAVE		(FRAMES_PER_SEC * 60)  // our territory leaves this long after its last building went
#define ENEMY_LEAVE		(FRAMES_PER_SEC * 120)  // and the enemy's
#define ENEMY_HOLD_COST	150.f  // metal: a cheaper enemy building that isn't armed (a mex, a solar) holds no ground
#define MAX_DESTINATIONS	16

static constexpr float NO_LIMIT = std::numeric_limits<float>::max();

void CChokeMap::SCutCache::Reset(std::vector<int>&& from, std::vector<float>&& cap, float limit, size_t regionCount)
{
	sources = std::move(from);
	capacity = std::move(cap);
	maxFlow = limit;
	cuts.assign(regionCount, CRegionGraph::SCut());
	isKnown.assign(regionCount, false);
}

const CRegionGraph::SCut& CChokeMap::SCutCache::Get(const CRegionGraph& graph, int region)
{
	static const CRegionGraph::SCut none;
	if ((region < 0) || (region >= (int)isKnown.size())) {
		return none;  // before the first analysis of this graph
	}
	if (!isKnown[region]) {
		isKnown[region] = true;
		cuts[region] = graph.CutFrom(sources, {region}, capacity, maxFlow);
	}
	return cuts[region];
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
		ForEachCellIn(mask.width, mask.height, mask.cellSize, r.x0, r.z0, r.x1, r.z1, [&](int x, int z) {
			const int region = graph.GetRegionAt(z * mask.width + x);
			if (region != CRegionGraph::NO_REGION) {
				side[region] = true;
			}
		});
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
	sideCuts.Reset(RegionsWhere(isEnemySide), ChokeCapacity(1.f, 1.f), config.entrances, count);
}

std::vector<bool> CChokeMap::GetShelteredBy(int region) const
{
	std::vector<bool> isSheltered(graph.GetRegions().size(), false);
	if (region < 0) {
		return isSheltered;
	}
	isSheltered[region] = true;
	std::vector<int> enemySide = RegionsWhere(isEnemySide);
	enemySide.erase(std::remove(enemySide.begin(), enemySide.end(), region), enemySide.end());
	if (enemySide.empty()) {
		return isSheltered;
	}
	const std::vector<CRegionGraph::SChoke>& chokes = graph.GetChokes();
	std::vector<bool> isBlocked(chokes.size());
	for (size_t c = 0; c < chokes.size(); ++c) {
		isBlocked[c] = (chokes[c].regionA == region) || (chokes[c].regionB == region);
	}
	const std::vector<bool> isReached = graph.ReachableFrom(enemySide, isBlocked);
	for (size_t r = 0; r < isSheltered.size(); ++r) {
		isSheltered[r] = isSheltered[r] || !isReached[r];
	}
	return isSheltered;
}

/*
 * Territory: regions holding a side's buildings, joining after 20 s and leaving 60 s (ours) or 120 s (the enemy's)
 * after the last one went. Only the enemy's towers and buildings of 150 metal or more count, so a lone mex or solar
 * doesn't flip a pocket or the lines. Keyed by region id, so it carries over a rebuild
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
		const CCircuitDef* edef = e->GetCircuitDef();
		if ((edef != nullptr) && !edef->IsMobile() && !e->IsDead()
			&& (edef->IsAttacker() || (e->GetCost() >= ENEMY_HOLD_COST)))
		{
			const int region = GetRegionAt(e->GetPos());
			if (region != CRegionGraph::NO_REGION) {
				theirs.insert(regions[region].id);
			}
		}
	}
	auto update = [frame, &regions](std::map<int, STerritory>& hold, const std::set<int>& now, int leave,
			std::vector<bool>& out) {
		for (int id : now) {
			STerritory& t = hold[id];
			if (t.since < 0) {
				t.since = frame;
			}
			t.lastSeen = frame;
		}
		for (auto it = hold.begin(); it != hold.end();) {
			it = (frame > it->second.lastSeen + leave) ? hold.erase(it) : std::next(it);
		}
		out.assign(regions.size(), false);
		for (size_t r = 0; r < regions.size(); ++r) {
			auto it = hold.find(regions[r].id);
			out[r] = (it != hold.end()) && (frame >= it->second.since + TERRITORY_JOIN);
		}
	};
	update(ourHold, ours, OUR_LEAVE, isOurTerritory);
	update(enemyHold, theirs, ENEMY_LEAVE, isEnemyTerritory);
}

void CChokeMap::Analyze()
{
	AnalyzePoints();
	UpdateLines();
	if (isOpennessDirty) {
		UpdateOpenness();
	}
	// The version moves only when what the overlay draws from this did
	std::size_t hash = 0;
	auto mix = [&hash](int v) { hash = hash * 1000003u + std::hash<int>()(v); };
	for (const SPointInfo& pi : pointInfos) {
		mix(pi.region); mix(pi.entrances); mix(pi.isOpen); mix(pi.pocket);
	}
	for (const SPocket& pocket : pockets) {
		mix(pocket.isQualified); mix(pocket.isHeld);
		for (int id : pocket.chokeIds) {
			mix(id);
		}
	}
	for (const std::vector<int>* line : {&currentLine, &forwardLine, &enemyLine}) {
		mix(-1);
		for (int c : *line) {
			mix(c);
		}
	}
	mix(static_cast<int>(opennessBand));
	if (hash != analysisHash) {
		analysisHash = hash;
		++version;
	}
}

/*
 * Every defence point of every cluster: its entrances from the enemy side, its pocket, and its distances to the
 * pocket's chokes
 */
void CChokeMap::AnalyzePoints()
{
	pointInfos.clear();
	firstPointOf.clear();
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
	const float nearDist = config.nearRanges * mainRange;

	// The points, their mexes (the cluster's spots nearest each point) and their regions
	std::vector<std::vector<int>> pointMexes;
	for (int k = 0; k < (int)clusters.size(); ++k) {
		const std::vector<CDefenceMatrix::SDefPoint>& points = defence->GetDefPoints(k);
		const size_t first = pointMexes.size();
		firstPointOf.push_back(first);
		for (size_t i = 0; i < points.size(); ++i) {
			pointInfos.push_back({k, (int)i, points[i].position, 0.f, CRegionGraph::NO_REGION, 0, false, -1, {}});
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

	// Walking distances from each point to every choke, up to choke.near. Made again after a graph build
	if (pointChokeDist.size() != pointInfos.size()) {
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
	auto isQualified = [&](const std::vector<int>& cut, const std::vector<bool>& inner, const std::vector<int>& pts) {
		for (int c : cut) {
			const int outer = inner[chokes[c].regionA] ? chokes[c].regionB : chokes[c].regionA;
			const bool isNear = std::any_of(pts.begin(), pts.end(), [&](int p) {
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
	const bool isSided = std::find(isEnemySide.begin(), isEnemySide.end(), true) != isEnemySide.end();
	for (int p = 0; p < (int)pointInfos.size(); ++p) {
		SPointInfo& pi = pointInfos[p];
		if (pi.region == CRegionGraph::NO_REGION) {
			continue;  // nothing walks there: stays on cluster
		}
		if (!isSided || isEnemySide[pi.region]) {
			pi.isOpen = true;  // the enemy's side, or it isn't known yet
			pi.entrances = config.entrances + 1;
			continue;
		}
		const CRegionGraph::SCut& cut = sideCuts.Get(graph, pi.region);
		pi.entrances = cut.isOpen ? config.entrances + 1 : (int)std::lround(cut.flow);
		pi.isOpen = cut.isOpen;
		if (pi.isOpen || (pi.entrances == 0)) {
			continue;
		}
		std::vector<bool> inner = innerOf(pi.region, cut.nearSource);
		const std::vector<int>* chosen = &cut.nearSource;
		if (!isQualified(*chosen, inner, {p})) {
			chosen = &cut.nearTarget;
			inner = innerOf(pi.region, *chosen);
		}
		auto it = std::find_if(candidates.begin(), candidates.end(), [chosen](const SCandidate& c) {
			return c.cut == *chosen;
		});
		if (it == candidates.end()) {
			candidates.push_back({*chosen, inner, (int)std::count(inner.begin(), inner.end(), true)});
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
				}
				std::sort(pocket.chokeIds.begin(), pocket.chokeIds.end());
				pocket.isInner = candidates[ci].inner;
				pocket.isQualified = false;
				pocket.isHeld = false;
				pockets.push_back(pocket);
			}
			pointInfos[p].pocket = pocketOf[ci];
			pockets[pocketOf[ci]].points.push_back(p);
			break;
		}
	}

	// Per pocket: qualification and ours; per point: distances to its pocket's chokes
	for (SPocket& pocket : pockets) {
		pocket.isQualified = isQualified(pocket.chokes, pocket.isInner, pocket.points);
		for (int p : pocket.points) {
			SPointInfo& pi = pointInfos[p];
			pocket.isHeld |= metalMgr->IsClusterHeld(pi.cluster);
			for (int c : pocket.chokes) {
				pi.chokeDist.push_back(pointChokeDist[p][c]);
			}
		}
	}
}

/*
 * The current line: the cut nearest our territory against enemy territory (C). The forward line: the cut nearest our
 * territory against the enemy side, through chokes the enemy may hold (D). Chokes too wide to defend count double.
 * The enemy line: the same cut nearest enemy territory, and the front line the chokes both share, which every
 * cheapest cut between the sides passes. Entrances from us: per region, the cut nearest it from our territory (E)
 */
void CChokeMap::UpdateLines()
{
	const std::vector<float> capacity = ChokeCapacity(1.f, 2.f);
	std::vector<int> ours = GetOurRegions();
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
	// An open cut has no chokes
	CRegionGraph::SCut cut = graph.CutFrom(theirs, ours, capacity, NO_LIMIT);
	currentLine = std::move(cut.nearTarget);
	enemyLine = std::move(cut.nearSource);
	frontLine.clear();
	std::set_intersection(currentLine.begin(), currentLine.end(), enemyLine.begin(), enemyLine.end(),
			std::back_inserter(frontLine));
	forwardLine = graph.CutFrom(enemySide, ours, capacity, NO_LIMIT).nearTarget;
	// Entrances from us: found when first asked, as only regions with enemy units are
	entranceCuts.Reset(std::move(ours), ChokeCapacity(1.f, 1.f), config.entrances, graph.GetRegions().size());
}

const std::vector<int>& CChokeMap::GetEntrancesFromUs(int region) const
{
	return entranceCuts.Get(graph, region).nearTarget;  // empty when open
}

std::vector<int> CChokeMap::GetOurRegions() const
{
	std::vector<int> ours = RegionsWhere(isOurTerritory);
	return ours.empty() ? RegionsWhere(isOurSide) : ours;
}

bool CChokeMap::IsOurHalf(const AIFloat3& pos) const
{
	auto sqDistTo = [&pos](const SRect& r) {
		const float dx = std::max({r.x0 - pos.x, 0.f, pos.x - r.x1});
		const float dz = std::max({r.z0 - pos.z, 0.f, pos.z - r.z1});
		return dx * dx + dz * dz;
	};
	float ourSq = NO_LIMIT, enemySq = NO_LIMIT;
	for (const SRect& r : ourBoxes) {
		ourSq = std::min(ourSq, sqDistTo(r));
	}
	for (const SSourceGroup& g : sourceGroups) {
		for (const SRect& r : g.boxes) {
			enemySq = std::min(enemySq, sqDistTo(r));
		}
		for (const AIFloat3& f : g.factoryPos) {
			enemySq = std::min(enemySq, sqDistTo({f.x, f.z, f.x, f.z}));
		}
	}
	return (ourSq == NO_LIMIT) || (enemySq == NO_LIMIT) || (ourSq < enemySq);
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
 * Traffic destinations, fixed at the first build (and again when the guessed enemy start goes) so the field doesn't
 * churn as we expand: our base, valued like our richest cluster, and every cluster on our half of the map, grouped
 * by region and valued by income. At most 16 groups: the smallest merges into its nearest
 */
void CChokeMap::MakeDestinations()
{
	destinations.clear();
	CMetalManager* metalMgr = circuit->GetMetalManager();
	const CMetalData::Metals& spots = metalMgr->GetSpots();
	const CMetalData::Clusters& clusters = metalMgr->GetClusters();
	struct SGroup {
		int region;
		AIFloat3 pos;
		std::vector<int> cells;  // traffic cells
		float value;
	};
	std::vector<SGroup> groups;
	float richest = 0.f;
	for (const CMetalData::SCluster& cluster : clusters) {
		richest = std::max(richest, cluster.income);
		if (!IsOurHalf(cluster.position)) {
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
		it->value += cluster.income;
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
 * made them, to compare tunings and maps offline
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
		points.append(p);
	}
	root["openness"] = std::round(openness * 100.f) / 100.f;
	Json::StreamWriterBuilder builder;
	builder["indentation"] = "\t";
	return Json::writeString(builder, root);
}

} // namespace circuit

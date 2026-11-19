/*
 * Frontline.cpp
 *
 *  Created on: Nov 11, 2026
 *      Author: Claude
 */

#include "module/Frontline.h"
#include "map/MapManager.h"
#include "module/MilitaryManager.h"
#include "module/SpotScorer.h"
#include "module/TurretPicker.h"
#include "resource/MetalManager.h"
#include "setup/DefenceMatrix.h"
#include "task/fighter/FighterTask.h"
#include "terrain/ChokeMap.h"
#include "terrain/ChokeOverlay.h"
#include "terrain/TerrainManager.h"
#include "unit/ally/AllyTeam.h"
#include "unit/enemy/EnemyManager.h"
#include "unit/enemy/EnemyUnit.h"
#include "unit/CircuitUnit.h"
#include "CircuitAI.h"
#include "util/Utils.h"

#include "Log.h"

#include <algorithm>
#include <cmath>

namespace circuit {

using namespace springai;

#define BUILDER_HALF	(FRAMES_PER_SEC * 60)  // an unseen enemy constructor's value halves this often
#define WANT_MARGIN		1.2f  // a choke wants this much more than the enemy army near it
#define WANT_OTHER		0.5f  // an enemy threatens its region's other line chokes by this share (its nearest in full)
#define SWITCH_SHARE	1.3f  // a slot moves only to a choke this much shorter
#define SAMPLE_COUNT	8  // LOS samples on a choke's line, and as many beyond it on the enemy's side
#define SEEN_MIN		0.25f  // a choke with no defence is undefended once this share of it was seen lately
#define HOLD_RETRY		(FRAMES_PER_SEC * 30)  // a slot with no hold spot is scored again after this
#define HOLD_END		(FRAMES_PER_SEC * 30)  // a hold ends once its choke is this long not short
#define HOLD_LEASE		(FRAMES_PER_SEC * 60)  // a claim lapses when its group hasn't asked for its spot this long

namespace {

bool Contains(const std::vector<int>& list, int value)
{
	return std::find(list.begin(), list.end(), value) != list.end();
}

}  // namespace

CFrontline::CFrontline(CCircuitAI* circuit)
		: circuit(circuit)
{
}

CFrontline::~CFrontline()
{
}

void CFrontline::Update()
{
	const CChokeMap* chokeMap = circuit->GetChokeMap();
	const CChokeMap::SConfig& config = chokeMap->GetConfig();
	// C feeds the turret picker's short-choke caller too
	const bool isShortCaller = circuit->GetMilitaryManager()->GetTurretPicker()->GetConfig().isShortChoke;
	if (!chokeMap->IsReady() || !(config.isHold || isShortCaller || chokeMap->GetOverlay()->IsOn())) {
		holds.clear();
		return;
	}
	// Armed enemies, once for every choke: the ones the held ground counts (CChokeMap::IsCountedEnemy), so the lines
	// and what they're ranked by forget an army at the same time
	const int frame = circuit->GetLastFrame();
	std::vector<SArmed> armed;
	for (const auto& kv : circuit->GetEnemyManager()->GetEnemyUnits()) {
		const CEnemyUnit* enemy = kv.second;
		if (CChokeMap::IsCountedEnemy(enemy, frame) && enemy->GetCircuitDef()->IsAttacker()) {
			const CCircuitDef* edef = enemy->GetCircuitDef();
			armed.push_back({enemy->GetPos(), edef->IsMobile(), edef->GetMaxRange(), enemy->GetThreat(),
					enemy->GetCost()});
		}
	}
	UpdateRanks(armed);
	UpdateFront(armed);
	UpdateHolds();
	Fortify();

	for (auto it = holdBans.begin(); it != holdBans.end();) {
		it = (frame >= it->second) ? holdBans.erase(it) : std::next(it);
	}
}

/*
 * E: enemy-held chokes and the enemy line's. A choke's outer side is the one more chokes away from our held ground; it
 * is enemy-held when that side is the enemy's ground. Enemy towers count when the line or our approach to it (one
 * main-tower range deep) is in their range, enemy mobiles within a main-tower range of the line. Unarmed enemy units
 * share their cost equally between the entrances of their region from our side, counted at those within
 * 2 x choke.near ranges of them. What we saw of a choke: the lower of the shares of its line and of the ground beyond
 * it on the enemy's side (up to a main-tower range out, where its defenders would stand) in our LOS lately
 */
void CFrontline::UpdateRanks(const std::vector<SArmed>& armed)
{
	const CChokeMap* chokeMap = circuit->GetChokeMap();
	const CRegionGraph& graph = chokeMap->GetGraph();
	const std::vector<CRegionGraph::SChoke>& chokes = graph.GetChokes();
	const float mainRange = chokeMap->GetMainRange();
	const int frame = circuit->GetLastFrame();
	const std::vector<int> hops = graph.HopsFrom(chokeMap->GetOurRegions());  // chokes away from our ground
	const SCellMask& mask = graph.GetMask();
	const float mapWidth = mask.width * mask.cellSize, mapHeight = mask.height * mask.cellSize;
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();

	std::vector<float> values(chokes.size(), 0.f);
	const float valueReach = 2.f * chokeMap->GetConfig().nearRanges * mainRange;
	for (const auto& kv : circuit->GetEnemyManager()->GetEnemyUnits()) {
		const CEnemyUnit* enemy = kv.second;
		CCircuitDef* edef = enemy->GetCircuitDef();
		if (enemy->IsHidden() || enemy->IsFake() || (edef == nullptr) || edef->IsAttacker() || edef->IsAbleToFly()) {
			continue;  // gone (seen dead, or its spot seen empty), or not there to take
		}
		const int region = chokeMap->GetRegionAt(enemy->GetPos());
		if (region < 0) {
			continue;
		}
		const std::vector<int>& entrances = chokeMap->GetEntrancesFromUs(region);
		float value = enemy->GetCost();
		if (edef->IsMobile() && !enemy->IsInRadarOrLOS() && (enemy->GetLastSeen() >= 0)) {
			value *= std::exp2(-float(frame - enemy->GetLastSeen()) / BUILDER_HALF);  // a constructor walks on
		}
		for (int c : entrances) {
			if (chokeMap->GetCellPos(chokes[c].centre).SqDistance2D(enemy->GetPos()) <= SQUARE(valueReach)) {
				values[c] += value / entrances.size();
			}
		}
	}

	std::vector<bool> isEnemyLine(chokes.size(), false);
	for (int c : chokeMap->GetEnemyLine()) {
		isEnemyLine[c] = true;
	}
	CMapManager* mapMgr = circuit->GetMapManager();
	ranks.clear();
	for (int c = 0; c < (int)chokes.size(); ++c) {
		const CRegionGraph::SChoke& choke = chokes[c];
		const AIFloat3 centre = chokeMap->GetCellPos(choke.centre);
		SRank rank = {choke.id, 0.f, 0.f, 0.f, values[c], false, false};
		bool isCovered = false;  // by enemy towers
		for (const SArmed& e : armed) {
			const float reach = e.isMobile ? mainRange : e.range + mainRange;
			if (centre.distance2D(e.pos) > reach + choke.width * 0.5f + 64.f) {
				continue;  // can't be in reach of any of its line
			}
			const float dist = graph.GetLineDist(choke, e.pos.x, e.pos.z);
			if (dist <= reach) {
				rank.defence += e.threat;
				rank.defenceCost += e.cost;
				isCovered |= !e.isMobile && (dist <= e.range);
			}
		}
		auto farness = [&hops](int region) {  // out of reach is farthest
			return (hops[region] < 0) ? std::numeric_limits<int>::max() : hops[region];
		};
		const int a = choke.regionA, b = choke.regionB;
		const int outer = (farness(a) == farness(b)) ? -1 : (farness(a) > farness(b)) ? a : b;
		rank.isHeld = isCovered || ((outer >= 0) && chokeMap->IsEnemyGround(outer));
		if (!rank.isHeld && !isEnemyLine[c]) {
			continue;
		}

		// Confidence: samples on its line, and beyond it on the enemy's side (its outer side, else the side that is
		// enemy ground), in our LOS within the last minute: as long as an army seen there counts
		std::vector<AIFloat3> samples;
		const int count = std::min<int>(SAMPLE_COUNT, choke.line.size());
		for (int i = 0; i < count; ++i) {
			samples.push_back(chokeMap->GetCellPos(choke.line[i * choke.line.size() / count]));
		}
		int enemySide = outer;
		if (enemySide < 0) {
			const bool isA = chokeMap->IsEnemyGround(a), isB = chokeMap->IsEnemyGround(b);
			enemySide = (isA == isB) ? -1 : isA ? a : b;
		}
		if ((enemySide >= 0) && (count > 0)) {
			// Across the line (its cells ascend, so the first and last are near its ends), toward the enemy's side
			const AIFloat3 first = chokeMap->GetCellPos(choke.line.front());
			const AIFloat3 last = chokeMap->GetCellPos(choke.line.back());
			const AIFloat3 toward = chokeMap->GetCellPos(graph.GetRegions()[enemySide].centre) - centre;
			AIFloat3 across(last.z - first.z, 0.f, first.x - last.x);
			if (across.SqLength2D() < 1.f) {
				across = toward;
			}
			across.y = 0.f;
			const float length = across.Length2D();
			if (length > 0.f) {
				across = across * ((across.dot2D(toward) < 0.f) ? -1.f / length : 1.f / length);
				for (int i = 0; i < SAMPLE_COUNT / 2; ++i) {
					const int cell = choke.line[(2 * i + 1) * choke.line.size() / SAMPLE_COUNT];
					const AIFloat3 base = chokeMap->GetCellPos(cell);
					for (float depth : {0.5f, 1.f}) {
						AIFloat3 p = base + across * (depth * mainRange);
						if ((p.x >= 0.f) && (p.z >= 0.f) && (p.x < mapWidth) && (p.z < mapHeight)) {
							p.y = terrainMgr->GetHeightAt(p.x, p.z);  // under water only sonar sees it
							samples.push_back(p);
						}
					}
				}
			}
		}
		std::vector<int>& seen = sampleSeen[choke.id];
		seen.resize(samples.size(), -CChokeMap::ARMY_SEEN - 1);
		int fresh = 0, freshBeyond = 0;
		for (size_t i = 0; i < samples.size(); ++i) {
			if (mapMgr->IsInLOS(samples[i])) {
				seen[i] = frame;
			}
			if (frame - seen[i] <= CChokeMap::ARMY_SEEN) {
				if (i < (size_t)count) {
					++fresh;
				} else {
					++freshBeyond;
				}
			}
		}
		const int beyond = (int)samples.size() - count;
		rank.confidence = (count <= 0) ? 0.f
				: (beyond <= 0) ? float(fresh) / count
				: std::min(float(fresh) / count, float(freshBeyond) / beyond);
		rank.isUndefended = (rank.defence <= 0.f) && (rank.confidence >= SEEN_MIN);
		ranks.push_back(rank);
	}
	std::stable_sort(ranks.begin(), ranks.end(), [](const SRank& x, const SRank& y) {
		return x.defence < y.defence;
	});
	for (auto it = sampleSeen.begin(); it != sampleSeen.end();) {  // chokes a rebuild took away
		it = (graph.FindChoke(it->first) < 0) ? sampleSeen.erase(it) : std::next(it);
	}
}

/*
 * C: each of our points behind the current line counts at its nearest line choke. Our finished towers count when their
 * range reaches the line (CTurretPicker::IsDefence: not a Big Bertha); its cover is the share of its line our towers
 * that hit ground reach (CChokeMap::GetCover). An enemy mobile seen in the last minute (as the held ground counts it)
 * counts at the line chokes out into its region: the nearest in full, the others by half (none when no line choke
 * opens into its region)
 */
void CFrontline::UpdateFront(const std::vector<SArmed>& armed)
{
	front.clear();
	const CChokeMap* chokeMap = circuit->GetChokeMap();
	const CRegionGraph& graph = chokeMap->GetGraph();
	const std::vector<CRegionGraph::SChoke>& chokes = graph.GetChokes();
	const std::vector<int>& line = chokeMap->GetCurrentLine();
	if (line.empty()) {
		return;
	}
	CMetalManager* metalMgr = circuit->GetMetalManager();
	const std::vector<bool> behind = ReachableFromUs(line);
	std::vector<float> income(line.size(), 0.f);
	for (const CChokeMap::SPointInfo& pi : chokeMap->GetPointInfos()) {
		if ((pi.region < 0) || !behind[pi.region] || !metalMgr->IsClusterHeld(pi.cluster)) {
			continue;
		}
		size_t nearest = 0;
		float bestSq = std::numeric_limits<float>::max();
		for (size_t i = 0; i < line.size(); ++i) {
			const float sq = chokeMap->GetCellPos(chokes[line[i]].centre).SqDistance2D(pi.pos);
			if (sq < bestSq) {
				bestSq = sq;
				nearest = i;
			}
		}
		income[nearest] += pi.income;
	}

	const int frame = circuit->GetLastFrame();
	const CTurretPicker* picker = circuit->GetMilitaryManager()->GetTurretPicker();
	std::vector<SArmed> towers;  // ours and allies'
	std::vector<CChokeMap::SSideUnit> groundTowers;  // those that hit ground, for cover
	for (const auto& kv : circuit->GetFriendlyUnits()) {
		CCircuitDef* cdef = kv.second->GetCircuitDef();
		if (picker->IsDefence(cdef) && !kv.second->GetUnit()->IsBeingBuilt()) {
			const AIFloat3 pos = kv.second->GetPos(frame);
			towers.push_back({pos, false, cdef->GetMaxRange(), cdef->GetThreat(), 0.f});
			if (cdef->HasAntiLand()) {
				groundTowers.push_back({pos, false, cdef->GetMaxRange(), cdef->GetPower(), 0.f, true});
			}
		}
	}
	// Enemy mobiles in the region outside a line choke: each threatens the nearest of its region's line chokes in full,
	// the rest by half
	std::vector<int> outer(line.size(), -1);
	for (size_t i = 0; i < line.size(); ++i) {
		const CRegionGraph::SChoke& choke = chokes[line[i]];
		outer[i] = behind[choke.regionA] ? (behind[choke.regionB] ? -1 : choke.regionB)
				: (behind[choke.regionB] ? choke.regionA : -1);
	}
	std::vector<float> wanted(line.size(), 0.f), wantedCost(line.size(), 0.f);
	for (const SArmed& e : armed) {
		const int region = e.isMobile ? chokeMap->GetRegionAt(e.pos) : -1;
		if (region < 0) {
			continue;
		}
		std::vector<size_t> touching;  // line chokes out into its region
		size_t nearest = 0;
		float bestDist = std::numeric_limits<float>::max();
		for (size_t i = 0; i < line.size(); ++i) {
			if (outer[i] != region) {
				continue;
			}
			touching.push_back(i);
			const float dist = graph.GetLineDist(chokes[line[i]], e.pos.x, e.pos.z);
			if (dist < bestDist) {
				bestDist = dist;
				nearest = i;
			}
		}
		for (size_t i : touching) {
			const float share = WANT_MARGIN * ((i == nearest) ? 1.f : WANT_OTHER);
			wanted[i] += e.threat * share;
			wantedCost[i] += e.cost * share;
		}
	}
	for (size_t i = 0; i < line.size(); ++i) {
		const CRegionGraph::SChoke& choke = chokes[line[i]];
		const AIFloat3 centre = chokeMap->GetCellPos(choke.centre);
		const int inner = behind[choke.regionA] ? choke.regionA : behind[choke.regionB] ? choke.regionB : -1;
		SFront f = {choke.id, (inner >= 0) ? graph.GetRegions()[inner].id : -1,
				income[i] * chokeMap->GetChokeTraffic(line[i]), wanted[i], wantedCost[i], 0.f,
				chokeMap->GetCover(line[i], groundTowers)};
		for (const SArmed& t : towers) {
			if ((centre.distance2D(t.pos) <= t.range + choke.width * 0.5f + 64.f)
				&& (graph.GetLineDist(choke, t.pos.x, t.pos.z) <= t.range))
			{
				f.towers += t.threat;
			}
		}
		front.push_back(f);
	}
	std::stable_sort(front.begin(), front.end(), [](const SFront& x, const SFront& y) {
		return x.rank > y.rank;
	});
}

/*
 * C: a slot is at the most short choke (SFront::IsShorter) no other slot has. Once claimed, its choke keeps it until
 * 30 s after it was last short (an enemy army walking in and out of range doesn't walk its group away), unless another
 * is 1.3x as short as its choke was in the last 30 s; then it moves to the most short one, or ends if none is short.
 * Its group keeps the claim when it moves
 */
void CFrontline::UpdateHolds()
{
	const CChokeMap* chokeMap = circuit->GetChokeMap();
	const int frame = circuit->GetLastFrame();
	int slotCount = 0;
	if (chokeMap->GetConfig().isHold) {
		switch (chokeMap->GetOpennessBand()) {
			case CChokeMap::Openness::CHOKEPOINTED: slotCount = 2; break;
			case CChokeMap::Openness::SEMI_OPEN: slotCount = 1; break;
			default: break;
		}
	}
	holds.resize(slotCount);
	auto isTaken = [this](int chokeId) {
		return std::any_of(holds.begin(), holds.end(), [chokeId](const SHold& h) { return h.chokeId == chokeId; });
	};
	for (SHold& hold : holds) {
		if ((hold.task != nullptr) && (frame >= hold.askFrame + HOLD_LEASE)) {
			hold.task = nullptr;  // its group went elsewhere
			hold.spot = -RgtVector;
		}
		const SFront* current = FindFront(hold.chokeId);
		if ((current != nullptr) && current->IsShort()) {
			hold.shortFrame = frame;
		}
		if ((current != nullptr) && (current->GetShortfall() > 0.f)) {
			hold.shortfall = current->GetShortfall();
			hold.shortfallFrame = frame;
		}
		const bool isKept = (current != nullptr) && (hold.task != nullptr) && (frame < hold.shortFrame + HOLD_END);
		const SFront* best = nullptr;
		for (const SFront& f : front) {
			if (f.IsShort() && ((f.chokeId == hold.chokeId) || !isTaken(f.chokeId))
				&& ((best == nullptr) || f.IsShorter(*best)))
			{
				best = &f;
			}
		}
		if ((hold.chokeId >= 0) && (best == nullptr) && !isKept) {
			circuit->LOG("CHOKE hold at choke %i ends", hold.chokeId);
			hold = SHold();
			continue;
		}
		if ((best == nullptr) || (best == current)) {
			continue;
		}
		// Its last shortfall for HOLD_END: an army stepping out of its region doesn't make it less short
		auto asHeld = [&](SFront f) {
			if ((frame < hold.shortfallFrame + HOLD_END) && (f.GetShortfall() < hold.shortfall)) {
				f.wanted = f.towers + hold.shortfall;
			}
			return f;
		};
		if (!isKept || best->IsShorter(asHeld(*current), SWITCH_SHARE)) {
			circuit->LOG("CHOKE hold at choke %i (wants %.0f, towers %.0f, cover %.0f%%, rank %.1f)", best->chokeId,
					best->wanted, best->towers, best->cover * 100.f, best->rank);
			hold.chokeId = best->chokeId;
			hold.spot = -RgtVector;  // scored again for its group
			hold.retryFrame = 0;
			hold.shortFrame = frame;
			hold.shortfall = std::max(best->GetShortfall(), 0.f);
			hold.shortfallFrame = frame;
		}
	}
}

/*
 * C with choke.enable: the top-ranked short choke's pocket gets a MakeDefence call at a point of ours behind it, so
 * choke defence (A) may send its towers to that entrance
 */
void CFrontline::Fortify()
{
	const CChokeMap* chokeMap = circuit->GetChokeMap();
	if (!chokeMap->GetConfig().isHold || !chokeMap->GetConfig().isChoke) {
		return;
	}
	CMetalManager* metalMgr = circuit->GetMetalManager();
	const CRegionGraph& graph = chokeMap->GetGraph();
	for (const SFront& f : front) {
		if (!f.IsShort()) {
			continue;
		}
		const int choke = graph.FindChoke(f.chokeId);
		for (const CChokeMap::SPointInfo& pi : chokeMap->GetPointInfos()) {
			if ((pi.pocket >= 0) && Contains(chokeMap->GetPockets()[pi.pocket].chokes, choke)
				&& metalMgr->IsClusterHeld(pi.cluster))
			{
				circuit->GetMilitaryManager()->MakeDefence(pi.cluster, pi.pos);
				return;
			}
		}
	}
}

AIFloat3 CFrontline::GetHoldSpot(CCircuitUnit* leader)
{
	const CChokeMap* chokeMap = circuit->GetChokeMap();
	IUnitTask* unitTask = leader->GetTask();
	if (!chokeMap->GetConfig().isHold || !chokeMap->IsReady() || (unitTask == nullptr)
		|| (unitTask->GetType() != IUnitTask::Type::FIGHTER)
		|| (static_cast<IFighterTask*>(unitTask)->GetFightType() != IFighterTask::FightType::ATTACK))
	{
		return -RgtVector;
	}
	const IFighterTask* task = static_cast<IFighterTask*>(unitTask);
	const int frame = circuit->GetLastFrame();
	auto ban = holdBans.find(task);
	if ((ban != holdBans.end()) && (frame < ban->second)) {
		return -RgtVector;
	}
	auto it = std::find_if(holds.begin(), holds.end(), [task](const SHold& h) { return h.task == task; });
	if (it == holds.end()) {
		// A free slot at a short choke, whose shortfall this group can make up
		it = std::find_if(holds.begin(), holds.end(), [this, task, frame](const SHold& h) {
			const SFront* f = FindFront(h.chokeId);
			return (f != nullptr) && (h.task == nullptr) && (frame >= h.retryFrame) && f->IsShort()
					&& (task->GetAttackPower() >= f->GetShortfall());
		});
		if (it == holds.end()) {
			return -RgtVector;
		}
		it->spot = -RgtVector;  // scored for this group
	}
	SHold& hold = *it;
	if (!utils::is_valid(hold.spot)) {
		hold.spot = FindHoldSpot(hold.chokeId, leader);
		if (!utils::is_valid(hold.spot)) {
			// Nowhere to stand (no traffic near the choke): the slot stays free, and is scored again in a while
			hold.task = nullptr;
			hold.retryFrame = frame + HOLD_RETRY;
			return -RgtVector;
		}
		circuit->LOG("CHOKE hold spot (%.0f, %.0f) at choke %i for task %p", hold.spot.x, hold.spot.z, hold.chokeId,
				(const void*)task);
	}
	hold.task = task;  // claimed once it has a spot
	hold.askFrame = frame;
	return hold.spot;
}

AIFloat3 CFrontline::FindHoldSpot(int chokeId, CCircuitUnit* leader) const
{
	const CChokeMap* chokeMap = circuit->GetChokeMap();
	const CRegionGraph& graph = chokeMap->GetGraph();
	const int choke = graph.FindChoke(chokeId);
	const SFront* f = FindFront(chokeId);
	const int inner = (f != nullptr) ? graph.FindRegion(f->innerRegionId) : -1;
	if ((choke < 0) || (inner < 0)) {
		return -RgtVector;
	}
	CSpotScorer::SGoal goal;
	goal.type = CSpotScorer::GoalType::HOLD;
	goal.choke = choke;
	goal.innerRegion = inner;
	goal.area = leader->GetArea();
	return circuit->GetMilitaryManager()->GetSpotScorer()->FindSpot(leader->GetCircuitDef(), goal);
}

void CFrontline::ReleaseHold(const IFighterTask* task, int banFrames)
{
	for (SHold& hold : holds) {
		if (hold.task == task) {
			hold.task = nullptr;
			hold.spot = -RgtVector;
		}
	}
	if (banFrames > 0) {
		holdBans[task] = circuit->GetLastFrame() + banFrames;
	} else {
		holdBans.erase(task);
	}
}

std::vector<bool> CFrontline::ReachableFromUs(const std::vector<int>& blocked) const
{
	const CChokeMap* chokeMap = circuit->GetChokeMap();
	const CRegionGraph& graph = chokeMap->GetGraph();
	std::vector<bool> isBlocked(graph.GetChokes().size(), false);
	for (int c : blocked) {
		isBlocked[c] = true;
	}
	return graph.ReachableFrom(chokeMap->GetOurRegions(), isBlocked);
}

/*
 * The enemy army outside a choke first: by shortfall where either has one, else by the share of its line uncovered,
 * the higher rank on a tie
 */
bool CFrontline::SFront::IsShorter(const SFront& other, float share) const
{
	const float self = std::max(GetShortfall(), 0.f), theirs = std::max(other.GetShortfall(), 0.f);
	if ((self > 0.f) || (theirs > 0.f)) {
		return (self != theirs) ? (self > share * theirs) : (rank > share * other.rank);
	}
	const float gap = 1.f - cover, otherGap = 1.f - other.cover;
	return (gap != otherGap) ? (gap > share * otherGap) : (rank > share * other.rank);
}

const CFrontline::SFront* CFrontline::FindFront(int chokeId) const
{
	auto it = std::find_if(front.begin(), front.end(), [chokeId](const SFront& f) { return f.chokeId == chokeId; });
	return (it != front.end()) ? &*it : nullptr;
}

} // namespace circuit

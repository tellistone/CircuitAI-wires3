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
#include "resource/MetalManager.h"
#include "setup/DefenceMatrix.h"
#include "task/fighter/FighterTask.h"
#include "terrain/ChokeMap.h"
#include "terrain/ChokeOverlay.h"
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

#define RECENT_FRAMES	(FRAMES_PER_SEC * 180)  // enemies and our LOS samples count this long
#define COMMIT_FRAMES	(FRAMES_PER_SEC * 240)  // an objective lasts at most this long
#define BUILDER_HALF	(FRAMES_PER_SEC * 60)  // an unseen enemy constructor's value halves this often
#define INCOME_SEC		180.f  // an objective's gain: this much of the income it brings behind the line
#define WANT_MARGIN		1.2f  // a choke wants this much more than the enemy army near it
#define SWITCH_SHARE	1.3f  // a slot or objective moves only to one this much better
#define SAMPLE_COUNT	8  // LOS samples on a choke's line
#define HOLD_RETRY		(FRAMES_PER_SEC * 30)  // a slot with no hold spot is scored again after this
#define HOLD_END		(FRAMES_PER_SEC * 30)  // a hold ends once its choke is this long not short
#define HOLD_LEASE		(FRAMES_PER_SEC * 60)  // a claim lapses when its group hasn't asked for its spot this long

namespace {

bool Contains(const std::vector<int>& list, int value)
{
	return std::find(list.begin(), list.end(), value) != list.end();
}

// A choke's defence as far as we know it: what we saw, more for the share of its line we haven't seen lately
float ExpectedDefence(const CFrontline::SRank& rank)
{
	return rank.defence / std::max(rank.confidence, 0.25f);
}

}  // namespace

CFrontline::CFrontline(CCircuitAI* circuit)
		: circuit(circuit)
		, retryFrame(0)
{
}

CFrontline::~CFrontline()
{
}

void CFrontline::Update()
{
	const CChokeMap* chokeMap = circuit->GetChokeMap();
	const CChokeMap::SConfig& config = chokeMap->GetConfig();
	if (!chokeMap->IsReady() || !(config.isHold || config.isObjective || chokeMap->GetOverlay()->IsOn())) {
		holds.clear();
		objective = SObjective();
		return;
	}
	// Armed enemies, once for every choke
	const int frame = circuit->GetLastFrame();
	std::vector<SArmed> armed;
	for (const auto& kv : circuit->GetEnemyManager()->GetEnemyUnits()) {
		const CEnemyUnit* enemy = kv.second;
		CCircuitDef* edef = enemy->GetCircuitDef();
		if (!enemy->IsDead() && !enemy->IsFake() && (edef != nullptr) && edef->IsAttacker()
			&& (!edef->IsMobile() || enemy->IsRecentlySeen(frame, RECENT_FRAMES)))  // towers count until seen dead
		{
			armed.push_back({enemy->GetPos(), edef->IsMobile(), edef->GetMaxRange(), enemy->GetThreat(), enemy->GetCost()});
		}
	}
	UpdateRanks(armed);
	UpdateFront(armed);
	UpdateHolds();
	Fortify();
	UpdateObjective();

	for (auto it = holdBans.begin(); it != holdBans.end();) {
		it = (frame >= it->second) ? holdBans.erase(it) : std::next(it);
	}
}

/*
 * E: a choke's outer side is the one more chokes away from our territory. Enemy towers count when the line or our
 * approach to it (one main-tower range deep) is in their range, enemy mobiles within a main-tower range of the line.
 * Unarmed enemy units share their cost equally between the entrances of their region from our side, counted at
 * those within 2 x choke.near ranges of them
 */
void CFrontline::UpdateRanks(const std::vector<SArmed>& armed)
{
	const CChokeMap* chokeMap = circuit->GetChokeMap();
	const CRegionGraph& graph = chokeMap->GetGraph();
	const std::vector<CRegionGraph::SChoke>& chokes = graph.GetChokes();
	const float mainRange = chokeMap->GetMainRange();
	const int frame = circuit->GetLastFrame();
	const std::vector<int> hops = graph.HopsFrom(chokeMap->GetOurRegions());  // chokes away from our territory

	std::vector<float> values(chokes.size(), 0.f);
	const float valueReach = 2.f * chokeMap->GetConfig().nearRanges * mainRange;
	for (const auto& kv : circuit->GetEnemyManager()->GetEnemyUnits()) {
		const CEnemyUnit* enemy = kv.second;
		CCircuitDef* edef = enemy->GetCircuitDef();
		if (enemy->IsDead() || enemy->IsFake() || (edef == nullptr) || edef->IsAttacker()) {
			continue;
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

	CMapManager* mapMgr = circuit->GetMapManager();
	ranks.clear();
	for (int c = 0; c < (int)chokes.size(); ++c) {
		const CRegionGraph::SChoke& choke = chokes[c];
		const AIFloat3 centre = chokeMap->GetCellPos(choke.centre);
		SRank rank = {choke.id, 0.f, 0.f, 0.f, values[c]};
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
		if (!isCovered && ((outer < 0) || !chokeMap->IsEnemyTerritory(outer))) {
			continue;
		}

		// Confidence: line samples in our LOS within the last 3 minutes
		std::vector<int>& seen = sampleSeen[choke.id];
		const int count = std::min<int>(SAMPLE_COUNT, choke.line.size());
		seen.resize(count, -RECENT_FRAMES - 1);
		int fresh = 0;
		for (int i = 0; i < count; ++i) {
			const int cell = choke.line[i * choke.line.size() / count];
			if (mapMgr->IsInLOS(chokeMap->GetCellPos(cell))) {
				seen[i] = frame;
			}
			fresh += (frame - seen[i] <= RECENT_FRAMES) ? 1 : 0;
		}
		rank.confidence = (count > 0) ? float(fresh) / count : 0.f;
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
 * C: each of our points behind the current line counts at its nearest line choke. Our towers count when their range
 * reaches the line, enemy mobiles within choke.near ranges of its centre
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
	std::vector<SArmed> towers;  // ours and allies'
	for (const auto& kv : circuit->GetFriendlyUnits()) {
		CCircuitDef* cdef = kv.second->GetCircuitDef();
		if (!cdef->IsMobile() && cdef->IsAttacker()) {
			towers.push_back({kv.second->GetPos(frame), false, cdef->GetMaxRange(), cdef->GetThreat(), 0.f});
		}
	}
	const float nearRange = chokeMap->GetConfig().nearRanges * chokeMap->GetMainRange();
	for (size_t i = 0; i < line.size(); ++i) {
		const CRegionGraph::SChoke& choke = chokes[line[i]];
		const AIFloat3 centre = chokeMap->GetCellPos(choke.centre);
		const int inner = behind[choke.regionA] ? choke.regionA : behind[choke.regionB] ? choke.regionB : -1;
		SFront f = {choke.id, (inner >= 0) ? graph.GetRegions()[inner].id : -1,
				income[i] * chokeMap->GetChokeTraffic(line[i]), 0.f, 0.f, 0.f};
		for (const SArmed& e : armed) {
			if (e.isMobile && (centre.SqDistance2D(e.pos) <= SQUARE(nearRange))) {
				f.wanted += e.threat * WANT_MARGIN;
				f.wantedCost += e.cost * WANT_MARGIN;
			}
		}
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
 * C: a slot is at the top-ranked choke whose towers are short of what it wants. Once claimed, its choke keeps it until
 * 30 s after it was last short (an enemy army walking in and out of range doesn't walk its group away), unless another
 * short one ranks 1.3x higher; then it moves to the top short one, or ends if none is. Its group keeps the claim when
 * it moves
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
		if ((current != nullptr) && (current->GetShortfall() > 0.f)) {
			hold.shortFrame = frame;
		}
		const bool isKept = (current != nullptr) && (hold.task != nullptr) && (frame < hold.shortFrame + HOLD_END);
		const SFront* best = nullptr;
		for (const SFront& f : front) {  // highest rank first
			if ((f.GetShortfall() > 0.f) && ((f.chokeId == hold.chokeId) || !isTaken(f.chokeId))) {
				best = &f;
				break;
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
		if (!isKept || (best->rank >= SWITCH_SHARE * current->rank)) {
			circuit->LOG("CHOKE hold at choke %i (rank %.1f, wants %.0f, towers %.0f)", best->chokeId, best->rank,
					best->wanted, best->towers);
			hold.chokeId = best->chokeId;
			hold.spot = -RgtVector;  // scored again for its group
			hold.retryFrame = 0;
			hold.shortFrame = frame;
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
		if (f.GetShortfall() <= 0.f) {
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

/*
 * D: commits until taken (none of its chokes is enemy-held), 4 minutes pass, or our attack power falls below its
 * defence as it is now; switches only to an objective scoring 1.3x higher, within the same 4 minutes
 */
void CFrontline::UpdateObjective()
{
	const CChokeMap* chokeMap = circuit->GetChokeMap();
	if (!chokeMap->GetConfig().isObjective) {
		objective = SObjective();
		return;
	}
	const int frame = circuit->GetLastFrame();
	float armyPower = 0.f;
	SObjective next = MakeObjective(armyPower);
	next.startFrame = frame;
	if (!objective.chokeIds.empty()) {
		float defence = 0.f;
		for (int id : objective.chokeIds) {
			const SRank* rank = FindRank(id);
			defence += (rank != nullptr) ? ExpectedDefence(*rank) : 0.f;
		}
		const bool isTaken = std::none_of(objective.chokeIds.begin(), objective.chokeIds.end(), [this](int id) {
			return FindRank(id) != nullptr;
		});
		const bool isFailed = (armyPower < defence) || (frame >= objective.startFrame + COMMIT_FRAMES);
		if (isTaken || isFailed) {
			circuit->LOG("CHOKE objective %s", isTaken ? "taken" : "failed");
			if (!isTaken) {
				retryFrame = frame + chokeMap->GetConfig().objRetryFrames;
			}
			objective = SObjective();
		} else if (!next.chokeIds.empty() && (next.chokeIds != objective.chokeIds)
			&& (next.score >= SWITCH_SHARE * objective.score))
		{
			next.startFrame = objective.startFrame;  // a switch keeps the commitment's clock: 4 minutes in all
			objective = next;
			circuit->LOG("CHOKE objective switched: %i chokes, score %.2f", (int)objective.chokeIds.size(), objective.score);
		}
		return;
	}
	if ((frame >= retryFrame) && !next.chokeIds.empty()) {
		objective = next;
		circuit->LOG("CHOKE objective: %i chokes, score %.2f, defence %.0f, army %.0f", (int)objective.chokeIds.size(),
				objective.score, objective.defence, armyPower);
	}
}

CFrontline::SObjective CFrontline::MakeObjective(float& outArmyPower) const
{
	SObjective obj;
	outArmyPower = 0.f;
	for (const IFighterTask* task : circuit->GetMilitaryManager()->GetTasks(IFighterTask::FightType::ATTACK)) {
		outArmyPower += task->GetAttackPower();
	}
	const CChokeMap* chokeMap = circuit->GetChokeMap();
	const CChokeMap::SConfig& config = chokeMap->GetConfig();
	const std::vector<int>& forward = chokeMap->GetForwardLine();
	const std::vector<int>& current = chokeMap->GetCurrentLine();
	if (forward.empty() || current.empty()) {
		return obj;
	}
	const std::vector<bool> behindForward = ReachableFromUs(forward);
	const std::vector<bool> behindCurrent = ReachableFromUs(current);
	if ((forward.size() > current.size())
		|| ((forward.size() == current.size())
			&& (GetIncomeIn(behindForward) <= GetIncomeIn(behindCurrent))))
	{
		return obj;
	}

	const float mainCost = chokeMap->GetMainDef()->GetCostM();
	CDefenceMatrix* defence = circuit->GetAllyTeam()->GetDefenceMatrix().get();
	const std::vector<CRegionGraph::SChoke>& chokes = chokeMap->GetGraph().GetChokes();

	// Gain: what the current-line chokes it retires would cost to keep, and the income it brings behind the line
	float gain = 0.f;
	for (int c : current) {
		if (Contains(forward, c)) {
			continue;
		}
		const CDefenceMatrix::SDefPoint* point = defence->GetChokePoint(chokes[c].id);
		gain += ((point != nullptr) && (point->cost > 0.f)) ? point->cost : mainCost;
		const SFront* f = FindFront(chokes[c].id);
		gain += (f != nullptr) ? f->wantedCost : 0.f;
	}
	std::vector<bool> brought(behindForward.size());
	for (size_t r = 0; r < brought.size(); ++r) {
		brought[r] = behindForward[r] && !behindCurrent[r];
	}
	gain += GetIncomeIn(brought) * INCOME_SEC;

	// Cost: the enemy-held forward chokes' defence, and what holding them takes
	float cost = 0.f;
	for (int c : forward) {
		const SRank* rank = FindRank(chokes[c].id);
		if (Contains(current, c) || (rank == nullptr)) {
			continue;
		}
		obj.chokeIds.push_back(chokes[c].id);
		obj.positions.push_back(chokeMap->GetCellPos(chokes[c].centre));
		obj.defence += ExpectedDefence(*rank);
		cost += rank->defenceCost + mainCost * (1.f + chokes[c].width / chokeMap->GetMainRange());
	}
	if (obj.chokeIds.empty() || (gain < config.objGain * cost) || (outArmyPower < config.objPower * obj.defence)) {
		return SObjective();
	}
	obj.score = gain / std::max(cost, 1.f);
	return obj;
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
			return (f != nullptr) && (h.task == nullptr) && (frame >= h.retryFrame) && (f->GetShortfall() > 0.f)
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

float CFrontline::GetIncomeIn(const std::vector<bool>& regions) const
{
	float income = 0.f;
	for (const CChokeMap::SPointInfo& pi : circuit->GetChokeMap()->GetPointInfos()) {
		if ((pi.region >= 0) && regions[pi.region]) {
			income += pi.income;
		}
	}
	return income;
}

const CFrontline::SRank* CFrontline::FindRank(int chokeId) const
{
	auto it = std::find_if(ranks.begin(), ranks.end(), [chokeId](const SRank& r) { return r.chokeId == chokeId; });
	return (it != ranks.end()) ? &*it : nullptr;
}

const CFrontline::SFront* CFrontline::FindFront(int chokeId) const
{
	auto it = std::find_if(front.begin(), front.end(), [chokeId](const SFront& f) { return f.chokeId == chokeId; });
	return (it != front.end()) ? &*it : nullptr;
}

} // namespace circuit

/*
 * ChokeDefence.cpp
 *
 *  Created on: Nov 11, 2026
 *      Author: Claude
 */

#include "module/ChokeDefence.h"
#include "module/BuilderManager.h"
#include "module/MilitaryManager.h"
#include "module/SpotScorer.h"
#include "resource/MetalManager.h"
#include "terrain/TerrainManager.h"
#include "unit/ally/AllyTeam.h"
#include "unit/CircuitDef.h"
#include "CircuitAI.h"
#include "util/Utils.h"

#include "Log.h"

#include <algorithm>

namespace circuit {

using namespace springai;

#define SWITCH_FRAMES	(FRAMES_PER_SEC * 180)  // a pocket's mode lasts at least 3 minutes

namespace {

float CostOf(const CChokeDefence::Towers& towers)
{
	float cost = 0.f;
	for (const CCircuitDef* def : towers) {
		cost += def->GetCostM();
	}
	return cost;
}

// Within a tower range of an entrance the chokes and the cluster are the same place: it stays on its cluster
bool IsNearEntrance(const CChokeMap::SPointInfo& info, float range)
{
	return std::any_of(info.chokeDist.begin(), info.chokeDist.end(), [range](float d) {
		return (d >= 0.f) && (d <= range);
	});
}

}  // namespace

CChokeDefence::CChokeDefence(CCircuitAI* circuit)
		: circuit(circuit)
		, laneVersion(-1)
{
}

CChokeDefence::~CChokeDefence()
{
}

CChokeDefence::SPlan CChokeDefence::Plan(int cluster, const CDefenceMatrix::SDefPoint* point, const Towers& picked,
		float maxCost, bool isSighting)
{
	SPlan plan;
	plan.clusterList = picked;
	CChokeMap* chokeMap = circuit->GetChokeMap();
	if (!chokeMap->GetConfig().isChoke || !chokeMap->IsReady() || point->isGeo || (point->chokeId >= 0) || isSighting) {
		return plan;
	}
	CDefenceMatrix* defence = circuit->GetAllyTeam()->GetDefenceMatrix().get();
	const CChokeMap::SPointInfo* info = chokeMap->GetPointInfo(cluster, point - defence->GetDefPoints(cluster).data());
	const float mainRange = chokeMap->GetMainRange();
	if ((info == nullptr) || (info->pocket < 0) || IsNearEntrance(*info, mainRange)) {
		return plan;
	}
	const CChokeMap::SPocket& pocket = chokeMap->GetPockets()[info->pocket];

	// The cluster keeps its AA and its first other tower, the main one
	CCircuitDef* mainDef = nullptr;
	Towers keep, rest;
	for (CCircuitDef* def : picked) {
		if (!def->IsRoleAA() && (mainDef != nullptr)) {
			rest.push_back(def);
		} else {
			mainDef = def->IsRoleAA() ? mainDef : def;
			keep.push_back(def);
		}
	}
	if (rest.empty()) {
		return plan;  // nothing to send
	}

	// Budget, the entrances' total: what the pocket's held points would spend today beyond what they keep, less what
	// they spent beyond that already (on their clusters, before the pocket defended at its chokes)
	CMetalManager* metalMgr = circuit->GetMetalManager();
	const float perPoint = std::min(maxCost, CostOf(picked));
	const float keepCost = CostOf(keep);
	int pointCount = 0;
	float budget = 0.f;
	for (int p : pocket.points) {
		const CChokeMap::SPointInfo& pi = chokeMap->GetPointInfos()[p];
		if (IsNearEntrance(pi, mainRange)) {
			continue;
		}
		++pointCount;
		if (metalMgr->IsClusterHeld(pi.cluster)) {
			budget += perPoint - std::max(defence->GetDefPoints(pi.cluster)[pi.index].cost, keepCost);
		}
	}
	// A main tower set back s ranges covers 2 x sqrt(1 - s^2) ranges of its entrance's line
	const std::vector<CRegionGraph::SChoke>& chokes = chokeMap->GetGraph().GetChokes();
	const float s = chokeMap->GetConfig().setback[1];
	const float cover = 2.f * mainDef->GetMaxRange() * std::sqrt(std::max(1.f - s * s, 0.1f));
	int needed = 0;
	for (int c : pocket.chokes) {
		needed += std::max((int)std::ceil(chokes[c].width / cover), 1);
	}
	plan.mode = Commit(pocket, pointCount, budget >= needed * mainDef->GetCostM());
	if (plan.mode == Mode::CLUSTER) {
		return plan;
	}

	// The budget split across the entrances by traffic, at least the first tower each: the one with the most room
	// gets this call's towers. Unsettled ones and those enemy artillery covers wait, and while one does, a point
	// with nowhere else to send its towers keeps them
	plan.clusterList = keep;
	float totalTraffic = 0.f;
	for (int c : pocket.chokes) {
		totalTraffic += chokeMap->GetChokeTraffic(c);
	}
	const int frame = circuit->GetLastFrame();
	bool isWaiting = false;
	float bestRoom = 0.f;
	for (int c : pocket.chokes) {
		if (!chokeMap->IsChokeSettled(c, frame) || chokeMap->IsArtilleryNear(c, frame)) {
			isWaiting = true;
			continue;
		}
		const float share = (totalTraffic > 0.f) ? chokeMap->GetChokeTraffic(c) / totalTraffic : 1.f / pocket.chokes.size();
		const float cap = std::max(rest.front()->GetCostM(), budget * share);
		const int inner = pocket.isInner[chokes[c].regionA] ? chokes[c].regionA : chokes[c].regionB;
		CDefenceMatrix::SDefPoint* chokePoint = GetChokePoint(c, inner, mainDef);
		const float room = cap - chokePoint->cost;
		if (room > bestRoom) {  // ties to the lower choke index
			bestRoom = room;
			plan.chokePoint = chokePoint;
			plan.choke = c;
			plan.innerRegion = inner;
			plan.chokeCap = cap;
		}
	}
	if (plan.chokePoint != nullptr) {
		plan.chokeList = rest;
	} else if (isWaiting) {
		plan.clusterList = picked;
	}
	return plan;
}

/*
 * The mode it should have: CHOKES when it qualifies (terrain, the enemy's gating types, territory outside an
 * entrance), its chokes are fewer than its points and its budget once covered its entrances. A new cut is a new key,
 * so a new state. Built towers never move
 */
CChokeDefence::Mode CChokeDefence::Commit(const CChokeMap::SPocket& pocket, int pointCount, bool isAffordable)
{
	CDefenceMatrix* defence = circuit->GetAllyTeam()->GetDefenceMatrix().get();
	const int frame = circuit->GetLastFrame();
	CDefenceMatrix::SPocketState* state = defence->GetPocketState(pocket.chokeIds);
	isAffordable |= (state != nullptr) && state->isAffordable;  // the first time counts
	const Mode wanted = (pocket.isQualified && ((int)pocket.chokes.size() < pointCount) && isAffordable)
			? Mode::CHOKES : Mode::CLUSTER;
	std::string ids;
	for (int id : pocket.chokeIds) {
		ids += utils::int_to_string(id, " %i");
	}
	if (state == nullptr) {
		state = defence->AddPocketState(pocket.chokeIds, {wanted, frame, isAffordable});
		circuit->LOG("CHOKE pocket [%s ] of %i points: %s", ids.c_str(), pointCount, GetModeName(wanted));
	} else {
		state->isAffordable = isAffordable;
		if ((state->mode != wanted) && (frame >= state->commitFrame + SWITCH_FRAMES)) {
			circuit->LOG("CHOKE pocket [%s ]: %s -> %s", ids.c_str(), GetModeName(state->mode), GetModeName(wanted));
			state->mode = wanted;
			state->commitFrame = frame;
		}
	}
	return state->mode;
}

/*
 * A choke's point, made on first ask at the best spot for the main tower behind it (CSpotScorer, the flag of F
 * aside), else towards its inner region's centre as far as the setback band's middle
 */
CDefenceMatrix::SDefPoint* CChokeDefence::GetChokePoint(int choke, int innerRegion, CCircuitDef* mainDef)
{
	CChokeMap* chokeMap = circuit->GetChokeMap();
	CDefenceMatrix* defence = circuit->GetAllyTeam()->GetDefenceMatrix().get();
	const CRegionGraph& graph = chokeMap->GetGraph();
	const int id = graph.GetChokes()[choke].id;
	CDefenceMatrix::SDefPoint* point = defence->GetChokePoint(id);
	if (point != nullptr) {
		return point;
	}
	CSpotScorer::SGoal goal;
	goal.type = CSpotScorer::GoalType::CHOKE;
	goal.choke = choke;
	goal.innerRegion = innerRegion;
	AIFloat3 anchor = circuit->GetMilitaryManager()->GetSpotScorer()->FindSpot(mainDef, goal);
	if (!utils::is_valid(anchor)) {
		const AIFloat3 centre = chokeMap->GetCellPos(graph.GetChokes()[choke].centre);
		const AIFloat3 dir = chokeMap->GetCellPos(graph.GetRegions()[innerRegion].centre) - centre;
		const float len = dir.Length2D();
		const float band = 0.5f * (chokeMap->GetConfig().setback[0] + chokeMap->GetConfig().setback[1]) * mainDef->GetMaxRange();
		anchor = centre;
		if (len > 1.f) {
			anchor = centre + dir * (std::min(len, band) / len);
		}
		CTerrainManager::CorrectPosition(anchor);
	}
	circuit->LOG("CHOKE point for choke %i at (%.0f, %.0f)", id, anchor.x, anchor.z);
	return defence->AddChokePoint(id, anchor);
}

void CChokeDefence::Update()
{
	CChokeMap* chokeMap = circuit->GetChokeMap();
	if (!chokeMap->GetConfig().isChoke || !chokeMap->IsReady()) {
		return;
	}
	if (chokeMap->GetLaneVersion() != laneVersion) {
		laneVersion = chokeMap->GetLaneVersion();
		UpdateLanes();
	}

	// Enemy artillery in range of a choke: its towers not yet started are called off
	CDefenceMatrix* defence = circuit->GetAllyTeam()->GetDefenceMatrix().get();
	const int frame = circuit->GetLastFrame();
	std::vector<IBuilderTask*> calledOff;
	for (IBuilderTask* task : circuit->GetBuilderManager()->GetTasks(IBuilderTask::BuildType::DEFENCE)) {
		if ((task->GetTarget() != nullptr) || (task->GetBuildDef() == nullptr)) {
			continue;
		}
		const CDefenceMatrix::SDefPoint* point = defence->GetDefPoint(task->GetPosition(), task->GetBuildDef()->GetCostM());
		if ((point == nullptr) || (point->chokeId < 0)) {
			continue;
		}
		const int choke = chokeMap->GetGraph().FindChoke(point->chokeId);
		if ((choke >= 0) && chokeMap->IsArtilleryNear(choke, frame)) {
			calledOff.push_back(task);
		}
	}
	for (IBuilderTask* task : calledOff) {
		circuit->LOG("CHOKE artillery near choke: %s called off", task->GetBuildDef()->GetDef()->GetName());
		circuit->GetMilitaryManager()->CallOffDefence(task);
	}
}

bool CChokeDefence::IsPocketDefended(const CChokeMap::SPocket& pocket) const
{
	CDefenceMatrix* defence = circuit->GetAllyTeam()->GetDefenceMatrix().get();
	return std::all_of(pocket.chokeIds.begin(), pocket.chokeIds.end(), [defence](int id) {
		const CDefenceMatrix::SDefPoint* point = defence->GetChokePoint(id);
		return (point != nullptr) && (point->cost > .5f);
	});
}

bool CChokeDefence::IsDefendedAtChokes(int cluster) const
{
	const CChokeMap* chokeMap = circuit->GetChokeMap();
	for (const CChokeMap::SPointInfo& pi : chokeMap->GetPointInfos()) {
		if ((pi.cluster == cluster) && (pi.pocket >= 0) && IsPocketDefended(chokeMap->GetPockets()[pi.pocket])) {
			return true;
		}
	}
	return false;
}

void CChokeDefence::UpdateLanes()
{
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	const SBlockingMap& blockingMap = terrainMgr->GetBlockingMap();
	const int step = SQUARE_SIZE * 2;  // a blocking map cell
	std::vector<int> cells;
	for (const CChokeMap::SLane& lane : circuit->GetChokeMap()->GetLanes()) {
		const float reach = lane.halfLength + lane.halfWidth;
		ForEachCellIn(blockingMap.columns, blockingMap.rows, step, lane.centre.x - reach, lane.centre.z - reach,
				lane.centre.x + reach, lane.centre.z + reach, [&](int x, int z) {
			if (lane.Contains((x + 0.5f) * step, (z + 0.5f) * step)) {
				cells.push_back(z * blockingMap.columns + x);
			}
		});
	}
	terrainMgr->SetLaneCells(std::move(cells));
}

const char* CChokeDefence::GetModeName(Mode mode)
{
	return (mode == Mode::CHOKES) ? "at chokes" : "on cluster";
}

} // namespace circuit

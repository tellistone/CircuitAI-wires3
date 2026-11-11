/*
 * ChokeDefence.cpp
 *
 *  Created on: Nov 11, 2026
 *      Author: Claude
 */

#include "module/ChokeDefence.h"
#include "module/BuilderManager.h"
#include "module/MilitaryManager.h"
#include "terrain/TerrainManager.h"
#include "unit/ally/AllyTeam.h"
#include "unit/CircuitDef.h"
#include "CircuitAI.h"
#include "util/Utils.h"

#include "spring/SpringMap.h"

#include "Log.h"

#include <algorithm>

namespace circuit {

using namespace springai;

#define SWITCH_FRAMES	(FRAMES_PER_SEC * 180)  // a pocket's mode lasts at least 3 minutes

CChokeDefence::CChokeDefence(CCircuitAI* circuit)
		: circuit(circuit)
		, laneVersion(-1)
{
}

CChokeDefence::~CChokeDefence()
{
}

CChokeDefence::SPlan CChokeDefence::Plan(int cluster, const CDefenceMatrix::SDefPoint* point,
		const std::vector<CDefenceMatrix::STower>& picked, float maxCost)
{
	SPlan plan;
	plan.clusterList = picked;
	CChokeMap* chokeMap = circuit->GetAllyTeam()->GetChokeMap().get();
	if (!chokeMap->GetConfig().isChoke || !chokeMap->IsReady() || point->isGeo || (point->chokeId >= 0)) {
		return plan;
	}
	CDefenceMatrix* defence = circuit->GetAllyTeam()->GetDefenceMatrix().get();
	const int index = point - defence->GetDefPoints(cluster).data();
	const CChokeMap::SPointInfo* info = chokeMap->GetPointInfo(cluster, index);
	if ((info == nullptr) || (info->pocket < 0)) {
		return plan;
	}
	const CChokeMap::SPocket& pocket = chokeMap->GetPockets()[info->pocket];
	const float mainRange = chokeMap->GetMainRange();
	// Within a tower range of an entrance the chokes and the cluster are the same place: it stays on its cluster,
	// and spends nothing at the chokes
	auto isNearEntrance = [mainRange](const CChokeMap::SPointInfo& pi) {
		return std::any_of(pi.chokeDist.begin(), pi.chokeDist.end(), [mainRange](float d) {
			return (d >= 0.f) && (d <= mainRange);
		});
	};
	if (isNearEntrance(*info)) {
		return plan;
	}
	int pointCount = 0;
	for (int p : pocket.points) {
		pointCount += isNearEntrance(chokeMap->GetPointInfos()[p]) ? 0 : 1;
	}

	// The lists by mode: AA always on the cluster; in MIXED mode the cluster's first other tower too
	using Towers = std::vector<CDefenceMatrix::STower>;
	auto split = [&picked](Mode mode, Towers& clusterSide, Towers& chokeSide) {
		bool isKeepNext = (mode == Mode::MIXED);
		for (const CDefenceMatrix::STower& tower : picked) {
			if (tower.def->IsRoleAA()) {
				clusterSide.push_back(tower);
			} else if (isKeepNext) {
				clusterSide.push_back(tower);
				isKeepNext = false;
			} else {
				chokeSide.push_back(tower);
			}
		}
	};
	auto costOf = [](const Towers& towers) {
		float cost = 0.f;
		for (const CDefenceMatrix::STower& tower : towers) {
			cost += tower.def->GetCostM();
		}
		return cost;
	};
	const float perPoint = std::min(maxCost, costOf(picked));
	auto poolOf = [&](Mode mode) {
		Towers clusterSide, chokeSide;
		split(mode, clusterSide, chokeSide);
		return pointCount * std::max(perPoint - costOf(clusterSide), 0.f);
	};
	auto mainIt = std::find_if(picked.begin(), picked.end(), [](const CDefenceMatrix::STower& tower) {
		return !tower.def->IsRoleAA();
	});
	if (mainIt == picked.end()) {
		return plan;  // nothing but AA: the cluster keeps it all
	}
	CCircuitDef* mainDef = mainIt->def;
	const bool isAffordable = poolOf(Mode::CHOKES) >= pocket.chokes.size() * mainDef->GetCostM();

	plan.mode = Commit(pocket, pointCount, isAffordable);
	if (plan.mode == Mode::CLUSTER) {
		return plan;
	}
	plan.clusterList.clear();
	Towers chokeSide;
	split(plan.mode, plan.clusterList, chokeSide);
	if (chokeSide.empty()) {
		return plan;
	}

	// The pool split across the entrances by traffic, at least the first tower each; the choke with the most room
	// left gets this call's towers. Not yet settled ones and those enemy artillery covers wait
	const float pool = poolOf(plan.mode);
	float totalTraffic = 0.f;
	for (int c : pocket.chokes) {
		totalTraffic += chokeMap->GetChokeTraffic(c);
	}
	const int frame = circuit->GetLastFrame();
	const std::vector<CRegionGraph::SChoke>& chokes = chokeMap->GetGraph().GetChokes();
	float bestRoom = 0.f;
	for (size_t i = 0; i < pocket.chokes.size(); ++i) {
		const int c = pocket.chokes[i];
		if (!chokeMap->IsChokeSettled(c, frame) || chokeMap->IsArtilleryNear(c, frame)) {
			continue;
		}
		const float share = (totalTraffic > 0.f) ? chokeMap->GetChokeTraffic(c) / totalTraffic : 1.f / pocket.chokes.size();
		const float cap = std::max(chokeSide.front().def->GetCostM(), pool * share);
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
		plan.chokeList = chokeSide;
	}
	return plan;
}

/*
 * Hard events: the pocket's cut changed (a new key, so a new state), it stopped or started qualifying (terrain, the
 * enemy's gating types, territory outside an entrance), it turned leaky, its pool first bought a main tower per
 * entrance, or it was lost. Built towers never move
 */
CChokeDefence::Mode CChokeDefence::Commit(const CChokeMap::SPocket& pocket, int pointCount, bool isAffordable)
{
	CDefenceMatrix* defence = circuit->GetAllyTeam()->GetDefenceMatrix().get();
	const int frame = circuit->GetLastFrame();
	CDefenceMatrix::SPocketState* state = defence->GetPocketState(pocket.chokeIds);
	isAffordable |= (state != nullptr) && state->isAffordable;
	const bool isFewer = (int)pocket.chokes.size() < pointCount;
	const int signature = (pocket.isQualified ? 1 : 0) | (pocket.isLeaky ? 2 : 0) | (pocket.isOuterEnemy ? 4 : 0)
			| (isAffordable ? 8 : 0) | (pocket.isHeld ? 16 : 0) | (isFewer ? 32 : 0);
	const Mode wanted = (!pocket.isQualified || !isFewer || !isAffordable) ? Mode::CLUSTER
			: pocket.isLeaky ? Mode::MIXED : Mode::CHOKES;
	std::string ids;
	for (int id : pocket.chokeIds) {
		ids += utils::int_to_string(id, " %i");
	}
	if (state == nullptr) {
		state = defence->AddPocketState(pocket.chokeIds, {wanted, frame, signature, isAffordable});
		circuit->LOG("CHOKE pocket [%s ] of %i points: %s", ids.c_str(), pointCount, GetModeName(wanted));
	} else {
		state->isAffordable = isAffordable;
		if ((state->signature != signature) && (frame >= state->commitFrame + SWITCH_FRAMES)) {
			if (state->mode != wanted) {
				circuit->LOG("CHOKE pocket [%s ]: %s -> %s", ids.c_str(), GetModeName(state->mode), GetModeName(wanted));
			}
			state->mode = wanted;
			state->signature = signature;
			state->commitFrame = frame;
		}
	}
	return state->mode;
}

CDefenceMatrix::SDefPoint* CChokeDefence::GetChokePoint(int choke, int innerRegion, CCircuitDef* mainDef)
{
	CChokeMap* chokeMap = circuit->GetAllyTeam()->GetChokeMap().get();
	CDefenceMatrix* defence = circuit->GetAllyTeam()->GetDefenceMatrix().get();
	const int id = chokeMap->GetGraph().GetChokes()[choke].id;
	CDefenceMatrix::SDefPoint* point = defence->GetChokePoint(id);
	if (point == nullptr) {
		const AIFloat3 anchor = FindAnchor(choke, innerRegion, mainDef);
		point = defence->AddChokePoint(id, anchor);
		circuit->LOG("CHOKE point for choke %i at (%.0f, %.0f)", id, anchor.x, anchor.z);
	}
	return point;
}

AIFloat3 CChokeDefence::FindAnchor(int choke, int innerRegion, CCircuitDef* mainDef) const
{
	CChokeMap* chokeMap = circuit->GetAllyTeam()->GetChokeMap().get();
	const CRegionGraph& graph = chokeMap->GetGraph();
	const CRegionGraph::SChoke& ch = graph.GetChokes()[choke];
	const SCellMask& mask = graph.GetMask();
	const CTrafficField& traffic = chokeMap->GetTraffic();
	const float range = mainDef->GetMaxRange();
	const float lo = chokeMap->GetConfig().setback[0] * range;
	const float hi = chokeMap->GetConfig().setback[1] * range;
	const AIFloat3 centre = chokeMap->GetCellPos(ch.centre);
	auto lineDist = [&ch, &mask](const AIFloat3& p) {
		float bestSq = std::numeric_limits<float>::max();
		for (int cell : ch.line) {
			bestSq = std::min(bestSq, SQUARE(mask.CentreX(cell) - p.x) + SQUARE(mask.CentreZ(cell) - p.z));
		}
		return std::sqrt(bestSq);
	};

	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	CMap* map = circuit->GetMap();
	const int size = traffic.GetCellSize();
	AIFloat3 best = -RgtVector;
	float bestTraffic = -1.f;
	const int x0 = std::max(int(centre.x - hi) / size, 0);
	const int x1 = std::min(int(centre.x + hi) / size, traffic.GetWidth() - 1);
	const int z0 = std::max(int(centre.z - hi) / size, 0);
	const int z1 = std::min(int(centre.z + hi) / size, traffic.GetHeight() - 1);
	for (int z = z0; z <= z1; ++z) {
		for (int x = x0; x <= x1; ++x) {
			AIFloat3 p((x + 0.5f) * size, 0.f, (z + 0.5f) * size);
			if ((graph.GetRegionAt(p.x, p.z) != innerRegion) || chokeMap->IsOnLane(p)) {
				continue;
			}
			const float d = lineDist(p);
			const float value = traffic.GetAt(z * traffic.GetWidth() + x);
			if ((d < lo) || (d > hi) || (value <= bestTraffic)) {
				continue;
			}
			p.y = map->GetElevationAt(p.x, p.z);
			if (terrainMgr->CanBeBuiltAt(mainDef, p) && map->IsPossibleToBuildAt(mainDef->GetDef(), p, UNIT_FACING_SOUTH)) {
				best = p;
				bestTraffic = value;
			}
		}
	}
	if (!utils::is_valid(best)) {
		// Nothing buildable in the band: towards the region's highest cell, as far as the band's middle
		const AIFloat3 inner = chokeMap->GetCellPos(graph.GetRegions()[innerRegion].centre);
		const AIFloat3 dir = inner - centre;
		const float len = dir.Length2D();
		best = centre;
		if (len > 1.f) {
			best += dir * (std::min(len, 0.5f * (lo + hi)) / len);
		}
		CTerrainManager::CorrectPosition(best);
		best.y = map->GetElevationAt(best.x, best.z);
	}
	return best;
}

void CChokeDefence::Update()
{
	CChokeMap* chokeMap = circuit->GetAllyTeam()->GetChokeMap().get();
	if (!chokeMap->IsReady()) {
		return;
	}
	if (chokeMap->GetLaneVersion() != laneVersion) {
		laneVersion = chokeMap->GetLaneVersion();
		UpdateLanes();
	}

	// Enemy artillery in range of a choke: its towers not yet started are called off, the army holds it instead
	CDefenceMatrix* defence = circuit->GetAllyTeam()->GetDefenceMatrix().get();
	CBuilderManager* builderMgr = circuit->GetBuilderManager();
	const int frame = circuit->GetLastFrame();
	std::vector<IBuilderTask*> cancelled;
	for (IBuilderTask* task : builderMgr->GetTasks(IBuilderTask::BuildType::DEFENCE)) {
		if ((task->GetTarget() != nullptr) || (task->GetBuildDef() == nullptr)) {
			continue;
		}
		const CDefenceMatrix::SDefPoint* point = defence->GetDefPoint(task->GetPosition(), task->GetBuildDef()->GetCostM());
		if ((point == nullptr) || (point->chokeId < 0)) {
			continue;
		}
		const int choke = chokeMap->GetGraph().FindChoke(point->chokeId);
		if ((choke >= 0) && chokeMap->IsArtilleryNear(choke, frame)) {
			cancelled.push_back(task);
		}
	}
	for (IBuilderTask* task : cancelled) {
		circuit->LOG("CHOKE artillery near choke: %s called off", task->GetBuildDef()->GetDef()->GetName());
		circuit->GetMilitaryManager()->CallOffDefence(task);
	}
}

bool CChokeDefence::IsPocketDefended(const CChokeMap::SPocket& pocket) const
{
	CDefenceMatrix* defence = circuit->GetAllyTeam()->GetDefenceMatrix().get();
	return std::any_of(pocket.chokeIds.begin(), pocket.chokeIds.end(), [defence](int id) {
		const CDefenceMatrix::SDefPoint* point = defence->GetChokePoint(id);
		return (point != nullptr) && (point->cost > .5f);
	});
}

bool CChokeDefence::IsDefendedAtChokes(int cluster) const
{
	const CChokeMap* chokeMap = circuit->GetAllyTeam()->GetChokeMap().get();
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
	for (const CChokeMap::SLane& lane : circuit->GetAllyTeam()->GetChokeMap()->GetLanes()) {
		const float reach = lane.halfLength + lane.halfWidth;
		const int x0 = std::max(int(lane.centre.x - reach) / step, 0);
		const int x1 = std::min(int(lane.centre.x + reach) / step, blockingMap.columns - 1);
		const int z0 = std::max(int(lane.centre.z - reach) / step, 0);
		const int z1 = std::min(int(lane.centre.z + reach) / step, blockingMap.rows - 1);
		for (int z = z0; z <= z1; ++z) {
			for (int x = x0; x <= x1; ++x) {
				if (lane.Contains((x + 0.5f) * step, (z + 0.5f) * step)) {
					cells.push_back(z * blockingMap.columns + x);
				}
			}
		}
	}
	terrainMgr->SetLaneCells(std::move(cells));
}

const char* CChokeDefence::GetModeName(Mode mode)
{
	switch (mode) {
		case Mode::CHOKES: return "at chokes";
		case Mode::MIXED: return "mixed";
		default: return "on cluster";
	}
}

} // namespace circuit

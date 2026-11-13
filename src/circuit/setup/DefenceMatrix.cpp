/*
 * DefenceMatrix.cpp
 *
 *  Created on: Sep 20, 2015
 *      Author: rlcevg
 */

#include "setup/DefenceMatrix.h"
#include "module/EconomyManager.h"
#include "module/MilitaryManager.h"
#include "resource/MetalManager.h"
#include "setup/SetupManager.h"
#include "terrain/ChokeMap.h"
#include "terrain/TerrainManager.h"
#include "unit/ally/AllyTeam.h"
#include "CircuitAI.h"
#include "util/math/HierarchCluster.h"
#include "util/math/RagMatrix.h"
#include "util/math/EncloseCircle.h"
#include "util/Scheduler.h"
#include "util/Utils.h"
#include "json/json.h"

#include <algorithm>
#include <limits>

#include "spring/SpringMap.h"

namespace circuit {

using namespace springai;

CDefenceMatrix::CDefenceMatrix(CCircuitAI* circuit)
		: metalManager(nullptr)
{
	circuit->GetScheduler()->RunOnInit(std::make_shared<CGameTask>(&CDefenceMatrix::Init, this, circuit));

	ReadConfig(circuit);
}

CDefenceMatrix::~CDefenceMatrix()
{
}

void CDefenceMatrix::ReadConfig(CCircuitAI* circuit)
{
	const Json::Value& defence = circuit->GetSetupManager()->GetConfig()["defence"];
	const Json::Value& baseRad = defence["base_rad"];
	baseRadMin = baseRad.get((unsigned)0, 1000.f).asFloat();
	baseRadMax = baseRad.get((unsigned)1, 3000.f).asFloat();
	baseRange = utils::clamp(CTerrainManager::GetTerrainDiagonal() * 0.3f, baseRadMin, baseRadMax);
	const Json::Value& commRad = defence["comm_rad"];
	commRadBegin = commRad.get((unsigned)0, 1000.f).asFloat();
	const float commRadEnd = commRad.get((unsigned)1, 300.f).asFloat();
	commRadFraction = (commRadEnd - commRadBegin) / baseRange;
}

void CDefenceMatrix::Init(CCircuitAI* circuit)
{
	metalManager = circuit->GetMetalManager();
	const CMetalData::Metals& spots = metalManager->GetSpots();
	const CMetalData::Clusters& clusters = metalManager->GetClusters();
	clusterInfos.resize(clusters.size());

	const bool isWaterMap = circuit->GetTerrainManager()->IsWaterMap();
	CMilitaryManager* militaryMgr = circuit->GetMilitaryManager();
	const std::vector<CCircuitDef*>& defenders = isWaterMap ? militaryMgr->GetWaterDefenders() : militaryMgr->GetLandDefenders();
	CCircuitDef* rangeDef = defenders.empty() ? militaryMgr->GetDefaultPorc() : defenders.front();

	CMap* map = circuit->GetMap();
	float maxDistance = rangeDef->GetMaxRange() * 0.75f * 2;
	CHierarchCluster clust;
	CEncloseCircle enclose;
	// choke.enable: spots in different regions never share a point
	CChokeMap* chokeMap = circuit->GetChokeMap();
	const bool isRegionSplit = chokeMap->GetConfig().isChoke && chokeMap->IsReady();

	for (unsigned k = 0; k < clusters.size(); ++k) {
		const CMetalData::MetalIndices& idxSpots = clusters[k].idxSpots;
		int nrows = idxSpots.size();
		CRagMatrix distmatrix(nrows);
		std::vector<int> regions;  // isRegionSplit: per spot
		for (int i = 0; isRegionSplit && (i < nrows); ++i) {
			regions.push_back(chokeMap->GetRegionAt(spots[idxSpots[i]].position));
		}
		for (int i = 1; i < nrows; ++i) {
			for (int j = 0; j < i; ++j) {
				distmatrix(i, j) = spots[idxSpots[i]].position.distance2D(spots[idxSpots[j]].position);
				if (isRegionSplit && (regions[i] != regions[j])) {
					distmatrix(i, j) += maxDistance * 100.f;  // a choke between them: never one point
				}
			}
		}

		const CHierarchCluster::Clusters& iclusters = clust.Clusterize(distmatrix, maxDistance);

		DefPoints& defPoints = clusterInfos[k].defPoints;
		unsigned nclusters = iclusters.size();
		defPoints.reserve(nclusters);
		for (unsigned i = 0; i < nclusters; ++i) {
			std::vector<AIFloat3> points;
			points.reserve(iclusters[i].size());
			for (unsigned j = 0; j < iclusters[i].size(); ++j) {
				points.push_back(spots[idxSpots[iclusters[i][j]]].position);
			}
			enclose.MakeCircle(points);
			AIFloat3 pos = enclose.GetCenter();
			pos.y = map->GetElevationAt(pos.x, pos.z);
			defPoints.push_back({pos, .0f});
		}
	}

	if (militaryMgr->IsGeoDefence()) {
		AddGeoPoints(circuit, maxDistance * 0.5f);
	}
	chokeMap->Reanalyze();  // pockets need the points
}

void CDefenceMatrix::AddGeoPoints(CCircuitAI* circuit, float coverRange)
{
	// Same nearest-cluster rule as GetDefPoint and MakeDefence(pos), so the towers built for a vent and their
	// losses are counted on its point. A vent within coverRange of a point (towers merge into one point up to
	// twice that apart) is left to that point
	CMap* map = circuit->GetMap();
	for (const AIFloat3& spot : circuit->GetEconomyManager()->GetGeoSpots()) {
		const int index = metalManager->FindNearestCluster(spot);
		if (index < 0) {
			continue;
		}
		DefPoints& defPoints = clusterInfos[index].defPoints;
		const bool isCovered = std::any_of(defPoints.begin(), defPoints.end(), [&spot, coverRange](const SDefPoint& p) {
			return p.position.SqDistance2D(spot) < SQUARE(coverRange);
		});
		if (!isCovered) {
			AIFloat3 pos = spot;
			pos.y = map->GetElevationAt(pos.x, pos.z);
			geoPoints.push_back({index, (unsigned)defPoints.size()});
			defPoints.push_back({pos, .0f, true});
		}
	}
}

float CDefenceMatrix::SDefPoint::GetSpotSqDist(const AIFloat3& pos) const
{
	float best = std::numeric_limits<float>::max();
	for (const SSpot& spot : spots) {
		if (spot.isFrozen) {
			best = std::min(best, spot.pos.SqDistance2D(pos));
		}
	}
	return best;
}

CDefenceMatrix::SDefPoint* CDefenceMatrix::GetDefPoint(const AIFloat3& pos, float defCost)
{
	// choke.spot.enable: a scored spot can stand a few hundred elmos off its point, nearer another one. A tower on it,
	// or slid a little off it, is its point's
	SDefPoint* spotPoint = nullptr;
	float spotSq = SQUARE(SQUARE_SIZE * 8);  // as IsSpotTaken
	auto checkSpots = [&pos, &spotPoint, &spotSq](SDefPoint& point) {
		const float sq = point.GetSpotSqDist(pos);
		if (sq < spotSq) {
			spotSq = sq;
			spotPoint = &point;
		}
	};
	for (SClusterInfo& info : clusterInfos) {
		std::for_each(info.defPoints.begin(), info.defPoints.end(), checkSpots);
	}
	for (auto& kv : chokePoints) {
		checkSpots(kv.second);
	}
	if (spotPoint != nullptr) {
		return spotPoint;
	}

	int index = metalManager->FindNearestCluster(pos);
	if (index < 0) {
		return nullptr;
	}

	DefPoints& defPoints = clusterInfos[index].defPoints;
	unsigned idx = 0;
	float dist = pos.distance2D(defPoints[idx].position);
	for (unsigned i = 1; i < defPoints.size(); ++i) {
		if (defPoints[i].cost >= defCost) {
			float tmp = pos.distance2D(defPoints[i].position);
			if (tmp < dist) {
				dist = tmp;
				idx = i;
			}
		}
	}
	SDefPoint* point = &defPoints[idx];
	// A vent is off to its cluster's side, so a tower built for it can be nearer another cluster: check them all
	for (const std::pair<int, unsigned>& gp : geoPoints) {
		SDefPoint& geoPoint = clusterInfos[gp.first].defPoints[gp.second];
		if (geoPoint.cost >= defCost) {
			const float tmp = pos.distance2D(geoPoint.position);
			if (tmp < dist) {
				dist = tmp;
				point = &geoPoint;
			}
		}
	}
	// So is a choke point (choke.enable)
	for (auto& kv : chokePoints) {
		if (kv.second.cost >= defCost) {
			const float tmp = pos.distance2D(kv.second.position);
			if (tmp < dist) {
				dist = tmp;
				point = &kv.second;
			}
		}
	}
	return point;
}

CDefenceMatrix::SDefPoint* CDefenceMatrix::GetChokePoint(int chokeId)
{
	auto it = chokePoints.find(chokeId);
	return (it != chokePoints.end()) ? &it->second : nullptr;
}

CDefenceMatrix::SDefPoint* CDefenceMatrix::AddChokePoint(int chokeId, const AIFloat3& anchor)
{
	SDefPoint& point = chokePoints[chokeId];
	point.position = anchor;
	point.cost = 0.f;
	point.chokeId = chokeId;
	return &point;
}

CDefenceMatrix::SPocketState* CDefenceMatrix::GetPocketState(const std::vector<int>& chokeIds)
{
	auto it = pocketStates.find(chokeIds);
	return (it != pocketStates.end()) ? &it->second : nullptr;
}

CDefenceMatrix::SPocketState* CDefenceMatrix::AddPocketState(const std::vector<int>& chokeIds, const SPocketState& state)
{
	return &(pocketStates[chokeIds] = state);
}

void CDefenceMatrix::SetBaseRange(float range)
{
	baseRange = utils::clamp(range, baseRadMin, baseRadMax);
}

} // namespace circuit

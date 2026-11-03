/*
 * MetalManager.h
 *
 *  Created on: Dec 9, 2014
 *      Author: rlcevg
 */

#ifndef SRC_CIRCUIT_METALMANAGER_H_
#define SRC_CIRCUIT_METALMANAGER_H_

#include "resource/MetalData.h"
#include "unit/CircuitUnit.h"
#include "lemon/adaptors.h"
#include "lemon/dijkstra.h"

namespace circuit {

class CCircuitAI;
class CMetalData;
class CScheduler;
class CGameTask;
class CRagMatrix;

class CMetalManager {
public:
	CMetalManager(CCircuitAI* circuit, CMetalData* metalData);
	virtual ~CMetalManager();

private:
	void Init();

public:
	void ParseMetalSpots();

	bool HasMetalSpots() const { return (metalData->IsInitialized() && !metalData->IsEmpty()); }
	bool HasMetalClusters() const { return !metalData->GetClusters().empty(); }
	bool IsClusterizing() const { return metalData->IsClusterizing(); }

	void ClusterizeMetal(CCircuitDef* commDef);
	void SetAuthority(CCircuitAI* authority) { circuit = authority; }

public:
	const CMetalData::Metals& GetSpots() const { return metalData->GetSpots(); }

	const int FindNearestSpot(const springai::AIFloat3& pos) const {
		return metalData->FindNearestSpot(pos);
	}
	const int FindNearestSpot(const springai::AIFloat3& pos, CMetalData::PointPredicate& predicate) const {
		return metalData->FindNearestSpot(pos, predicate);
	}
	void FindSpotsInRadius(const springai::AIFloat3& pos, const float radius, CMetalData::IndicesDists& outIndices) const {
		metalData->FindSpotsInRadius(pos, radius, outIndices);
	}

	const int FindNearestCluster(const springai::AIFloat3& pos) const {
		return metalData->FindNearestCluster(pos);
	}
	const int FindNearestCluster(const springai::AIFloat3& pos, CMetalData::PointPredicate& predicate) const {
		return metalData->FindNearestCluster(pos, predicate);
	}

	const CMetalData::Clusters& GetClusters() const { return metalData->GetClusters(); }
	const CMetalData::ClusterGraph& GetClusterGraph() const { return metalData->GetClusterGraph(); }
	const CMetalData::ClusterCostMap& GetClusterEdgeCosts() const { return metalData->GetClusterEdgeCosts(); }

public:
	void SetOpenSpot(int index, bool value);
	void SetOpenSpot(const springai::AIFloat3& pos, bool value);
	bool IsOpenSpot(int index) const { return metalInfos[index].isOpen; }
	bool IsOpenSpot(const springai::AIFloat3& pos) const;
	void MarkAllyMexes();
	void MarkAllyMexes(const std::vector<CAllyUnit*>& mexes);
	int GetMexCount() const { return markedMexes.size(); }
	bool IsClusterFinished(int index) const {
		return clusterInfos[index].finishedCount >= GetClusters()[index].idxSpots.size();
	}
	bool IsClusterQueued(int index) const {
		return clusterInfos[index].queuedCount >= GetClusters()[index].idxSpots.size();
	}
	bool IsMexInFinished(int index) const;
	int GetCluster(int index) const { return metalInfos[index].clusterId; }

	int GetMexToBuild(const springai::AIFloat3& pos, CMetalData::PointPredicate& predicate);
	// Contested clusters: ones with threat at their centre that the last GetMexToBuild walks ran into
	// (the start cluster, or a neighbour of a cluster the walk crossed). They act as walls: mexes behind
	// them can't be found even if safe. A mark lasts maxAge frames unless the walk crosses it safely.
	const std::vector<int>& GetContestedClusters(int frame, int maxAge);
	float GetContestedThreat(int index) const { return clusterInfos[index].contestedThreat; }

	float GetMinIncome() const { return metalData->GetMinIncome(); }
	float GetAvgIncome() const { return metalData->GetAvgIncome(); }
	float GetMaxIncome() const { return metalData->GetMaxIncome(); }

private:
	CCircuitAI* circuit;
	CMetalData* metalData;

	struct SMetalInfo {
		bool isOpen;
		int clusterId;
	};
	struct SClusterInfo {
		unsigned int queuedCount;
		unsigned int finishedCount;
		int contestedFrame;  // -1 = not contested
		float contestedThreat;  // threat at its centre when marked
	};
	std::vector<SMetalInfo> metalInfos;
	std::vector<SClusterInfo> clusterInfos;

	int markFrame;
	struct SMex {
		ICoreUnit::Id unitId;
		int index;
	};
	std::deque<SMex> markedMexes;  // sorted by insertion
	std::vector<int> contestedIds;  // clusters with contestedFrame >= 0

	void MarkContested(int index, int frame);
	void MarkWalkWalls(int frame);

	class SafeCluster;
	class DetectCluster;
	using ClusterGraph = lemon::FilterNodes<const CMetalData::ClusterGraph, SafeCluster>;
	using ShortPath = lemon::Dijkstra<ClusterGraph, CMetalData::ClusterCostMap>;

	SafeCluster* threatFilter;
	ClusterGraph* filteredGraph;
	ShortPath* shortPath;
};

} // namespace circuit

#endif // SRC_CIRCUIT_METALMANAGER_H_

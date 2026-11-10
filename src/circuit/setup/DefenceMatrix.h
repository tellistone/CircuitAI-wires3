/*
 * DefenceMatrix.h
 *
 *  Created on: Sep 20, 2015
 *      Author: rlcevg
 */

#ifndef SRC_CIRCUIT_SETUP_DEFENCEMATRIX_H_
#define SRC_CIRCUIT_SETUP_DEFENCEMATRIX_H_

#include "AIFloat3.h"

#include <utility>
#include <vector>

namespace circuit {

class CCircuitAI;
class CMetalManager;

class CDefenceMatrix {
public:
	struct SDefPoint {
		springai::AIFloat3 position;
		float cost;
		bool isGeo = false;  // a geothermal vent's own point (porcupine.geo): defended only for that vent
	};
	using DefPoints = std::vector<SDefPoint>;
	struct SClusterInfo {
		DefPoints defPoints;
	};

public:
	CDefenceMatrix(CCircuitAI* circuit);
	virtual ~CDefenceMatrix();

private:
	void ReadConfig(CCircuitAI* circuit);
	void Init(CCircuitAI* circuit);
	// porcupine.geo: each vent that no mex point covers becomes a point of its nearest cluster
	void AddGeoPoints(CCircuitAI* circuit, float coverRange);

public:
	std::vector<SDefPoint>& GetDefPoints(int index) { return clusterInfos[index].defPoints; }
	SDefPoint* GetDefPoint(const springai::AIFloat3& pos, float cost);

	void SetBaseRange(float range);
	float GetBaseRange() const { return baseRange; }
	float GetCommRadBegin() const { return commRadBegin; }
	float GetCommRad(float baseDist) const {
		return commRadFraction * baseDist + commRadBegin;
	}

private:
	CMetalManager* metalManager;
	std::vector<SClusterInfo> clusterInfos;
	std::vector<std::pair<int, unsigned>> geoPoints;  // AddGeoPoints: {cluster, index in its defPoints}

	float baseRadMin;
	float baseRadMax;
	float baseRange;
	float commRadBegin;
	float commRadFraction;
};

} // namespace circuit

#endif // SRC_CIRCUIT_SETUP_DEFENCEMATRIX_H_

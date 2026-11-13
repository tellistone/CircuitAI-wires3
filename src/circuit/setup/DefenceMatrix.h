/*
 * DefenceMatrix.h
 *
 *  Created on: Sep 20, 2015
 *      Author: rlcevg
 */

#ifndef SRC_CIRCUIT_SETUP_DEFENCEMATRIX_H_
#define SRC_CIRCUIT_SETUP_DEFENCEMATRIX_H_

#include "AIFloat3.h"

#include <map>
#include <utility>
#include <vector>

namespace circuit {

class CCircuitAI;
class CCircuitDef;
class CMetalManager;

class CDefenceMatrix {
public:
	// choke.spot.enable: a spot scored for a tower type at a point, frozen once queued (CSpotScorer), so a lost tower of
	// that type is rebuilt there. A tower there that is cancelled before it starts unfreezes it and counts as a
	// failure; once one fails twice within 10 minutes, the point's new towers of that type go back to today's shake
	// until 10 minutes after the last failure
	struct SSpot {
		CCircuitDef* def;
		springai::AIFloat3 pos;
		bool isFrozen = false;
		int failures = 0;  // in a row, each within 10 minutes of the last
		int failFrame = 0;  // the last failure
	};
	struct SDefPoint {
		springai::AIFloat3 position;
		float cost;
		bool isGeo = false;  // a geothermal vent's own point (porcupine.geo): defended only for that vent
		int chokeId = -1;  // choke.enable: a choke point, at its anchor behind that choke (CChokeDefence)
		std::vector<SSpot> spots = {};
		// Squared distance from pos to its nearest frozen spot, float max with none
		float GetSpotSqDist(const springai::AIFloat3& pos) const;
	};
	// choke.enable: how a pocket (CChokeMap::SPocket) is defended (CChokeDefence)
	enum class DefenceMode: char {CLUSTER, CHOKES};
	struct SPocketState {
		DefenceMode mode;
		int commitFrame;  // when it was last decided
		bool isAffordable;  // its budget once covered its entrances: only the first time counts
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
	int GetClusterCount() const { return clusterInfos.size(); }
	// The point a tower at pos belongs to: the one with a frozen spot under it, else the nearest with that cost spent
	SDefPoint* GetDefPoint(const springai::AIFloat3& pos, float cost);

	// choke.enable: choke points by choke id (never removed: towers built there stay), and pocket states by the
	// pocket's sorted choke ids
	SDefPoint* GetChokePoint(int chokeId);  // nullptr if none yet
	SDefPoint* AddChokePoint(int chokeId, const springai::AIFloat3& anchor);
	const std::map<int, SDefPoint>& GetChokePoints() const { return chokePoints; }
	SPocketState* GetPocketState(const std::vector<int>& chokeIds);  // nullptr if not decided yet
	SPocketState* AddPocketState(const std::vector<int>& chokeIds, const SPocketState& state);

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
	std::map<int, SDefPoint> chokePoints;
	std::map<std::vector<int>, SPocketState> pocketStates;

	float baseRadMin;
	float baseRadMax;
	float baseRange;
	float commRadBegin;
	float commRadFraction;
};

} // namespace circuit

#endif // SRC_CIRCUIT_SETUP_DEFENCEMATRIX_H_

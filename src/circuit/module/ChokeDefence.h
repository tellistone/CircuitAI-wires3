/*
 * ChokeDefence.h
 *
 *  Created on: Nov 11, 2026
 *      Author: Claude
 */

#ifndef SRC_CIRCUIT_MODULE_CHOKEDEFENCE_H_
#define SRC_CIRCUIT_MODULE_CHOKEDEFENCE_H_

#include "setup/DefenceMatrix.h"
#include "terrain/ChokeMap.h"

#include <vector>

namespace circuit {

class CCircuitAI;
class CCircuitDef;

/*
 * choke.enable: choke defence (A). A pocket (CChokeMap::SPocket) defends at its 1 to choke.entrances chokes when
 * they are fewer than its defence points, each defensible and near, none with enemy territory outside, and its
 * budget buys enough main towers to cover each entrance's width. Each point keeps its AA and its first tower on its
 * cluster, against raiders and anything the cut doesn't stop; the rest of what the pocket's held points would spend
 * today goes to the entrances, less what they spent on their clusters beyond that. Modes:
 * - CLUSTER: as without choke.enable;
 * - CHOKES: the first tower and the AA on the cluster, the rest at the entrances.
 * A pocket's mode is decided when its first tower is ordered, and switches at most every 3 minutes
 */
class CChokeDefence {
public:
	using Mode = CDefenceMatrix::DefenceMode;
	using Towers = std::vector<CCircuitDef*>;
	// Where MakeDefence's towers for a point go
	struct SPlan {
		Mode mode = Mode::CLUSTER;
		Towers clusterList;  // at the point, capped by maxCost as without choke.enable
		Towers chokeList;  // at chokePoint, walked again while chokeCap has room
		CDefenceMatrix::SDefPoint* chokePoint = nullptr;
		int choke = -1;  // its index in the threat graph
		int innerRegion = -1;  // its region on the pocket's side
		float chokeCap = 0.f;
	};

	CChokeDefence(CCircuitAI* circuit);
	virtual ~CChokeDefence();

	// picked: the towers MakeDefence picks for the point today, in order; maxCost: the point's cap today.
	// isSighting: an enemy is at the cluster already, so the cluster gets them all
	SPlan Plan(int cluster, const CDefenceMatrix::SDefPoint* point, const Towers& picked, float maxCost,
			bool isSighting);
	// Every 2 s: lanes into our blocking map, and unstarted towers called off at chokes enemy artillery covers
	void Update();
	// Towers queued or built at every entrance of the pocket
	bool IsPocketDefended(const CChokeMap::SPocket& pocket) const;
	// One of the cluster's points sits in a pocket defended at its entrances
	bool IsDefendedAtChokes(int cluster) const;

	static const char* GetModeName(Mode mode);

private:
	Mode Commit(const CChokeMap::SPocket& pocket, int pointCount, bool isAffordable);
	CDefenceMatrix::SDefPoint* GetChokePoint(int choke, int innerRegion, CCircuitDef* mainDef);
	void UpdateLanes();  // CChokeMap's lanes into our blocking map

	CCircuitAI* circuit;
	int laneVersion;  // CChokeMap's lanes in our blocking map
};

} // namespace circuit

#endif // SRC_CIRCUIT_MODULE_CHOKEDEFENCE_H_

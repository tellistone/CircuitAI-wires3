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
 * they are fewer than its defence points, each defensible and near, none with enemy territory outside, and the
 * pocket's pool buys a main tower per entrance. The pool is what its points would spend today less what each keeps
 * on its cluster, so the chokes only win by concentrating the same metal. Modes:
 * - CLUSTER: as without choke.enable;
 * - CHOKES: the towers go to the entrances, the AA stays on the cluster;
 * - MIXED (a leaky pocket): the cluster also keeps its first tower.
 * A pocket's mode is decided when its first tower is ordered, and switches only on a hard event, 3 minutes apart
 */
class CChokeDefence {
public:
	using Mode = CDefenceMatrix::DefenceMode;
	// Where MakeDefence's towers for a point go
	struct SPlan {
		Mode mode = Mode::CLUSTER;
		std::vector<CDefenceMatrix::STower> clusterList;  // at the point, capped by maxCost as without choke.enable
		std::vector<CDefenceMatrix::STower> chokeList;  // at chokePoint, walked again while chokeCap has room
		CDefenceMatrix::SDefPoint* chokePoint = nullptr;
		int choke = -1;  // its index in the threat graph
		int innerRegion = -1;  // its region on the pocket's side
		float chokeCap = 0.f;
	};

	CChokeDefence(CCircuitAI* circuit);
	virtual ~CChokeDefence();

	// picked: the towers MakeDefence picks for the point today, in order; maxCost: the point's cap today
	SPlan Plan(int cluster, const CDefenceMatrix::SDefPoint* point, const std::vector<CDefenceMatrix::STower>& picked,
			float maxCost);
	// Every 2 s: lanes into our blocking map, and unstarted towers cancelled at chokes enemy artillery covers
	void Update();
	// Towers at one of the pocket's entrances, queued or built
	bool IsPocketDefended(const CChokeMap::SPocket& pocket) const;
	// One of the cluster's points sits in a pocket defended at its entrances
	bool IsDefendedAtChokes(int cluster) const;

	static const char* GetModeName(Mode mode);

private:
	Mode Commit(const CChokeMap::SPocket& pocket, int pointCount, bool isAffordable);
	CDefenceMatrix::SDefPoint* GetChokePoint(int choke, int innerRegion, CCircuitDef* mainDef);
	// Simple placement: the buildable traffic cell with the most traffic choke.setback x range behind the line
	springai::AIFloat3 FindAnchor(int choke, int innerRegion, CCircuitDef* mainDef) const;
	void UpdateLanes();  // CChokeMap's lanes into our blocking map

	CCircuitAI* circuit;
	int laneVersion;  // CChokeMap's lanes in our blocking map
};

} // namespace circuit

#endif // SRC_CIRCUIT_MODULE_CHOKEDEFENCE_H_

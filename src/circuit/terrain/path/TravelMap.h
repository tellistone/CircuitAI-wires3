/*
 * TravelMap.h
 *
 *  Created on: Nov 27, 2026
 *      Author: Claude
 */

#ifndef SRC_CIRCUIT_TERRAIN_PATH_TRAVELMAP_H_
#define SRC_CIRCUIT_TERRAIN_PATH_TRAVELMAP_H_

#include "util/Defines.h"

#include "AIFloat3.h"

#include <memory>

namespace circuit {

class CCircuitAI;
class CCircuitUnit;
class IPathQuery;

/*
 * Threat-aware travel time from one spot (quota.raid_defence posts, quota.protect wards): a cost map
 * (CPathFinder::CreateCostMapQuery: a raid group's view, squad threat, threat added as cost) made on the path worker
 * for one movement type. The route to a square is the cheapest one, so it bends round threat; its time is that
 * route's length at the group's speed (threat is a price, not a slowdown). The last ready map is kept while the
 * next one is made
 */
class CTravelMap {
public:
	CTravelMap();
	~CTravelMap();

	// A new map from pos for unit's movement type, unless one is on its way: when there is none yet, the last one is
	// maxFrames old, it started more than maxMove away (once it is MOVE_MIN_FRAMES old: a goal that moves every
	// second doesn't remake it every second), or it is for another movement type
	void Refresh(CCircuitAI* circuit, CCircuitUnit* unit, const springai::AIFloat3& pos, int maxFrames, float maxMove);
	// Seconds at speed (elmos per second) from the start to pos; < 0: no map yet, or pos can't be reached
	float GetSec(const springai::AIFloat3& pos, float speed) const;
	bool IsReady() const { return ready != nullptr; }
	int GetMobileId() const { return readyMobileId; }  // the ready map's movement type
	const springai::AIFloat3& GetStart() const { return readyStart; }

private:
	static constexpr int MOVE_MIN_FRAMES = FRAMES_PER_SEC * 2;
	void TakePending();  // a pending map that is done becomes the ready one; a lost one is dropped

	std::shared_ptr<IPathQuery> ready;
	std::shared_ptr<IPathQuery> pending;
	int readyFrame;
	int readyMobileId;
	springai::AIFloat3 readyStart;
	int pendingFrame;
	int pendingMobileId;
	springai::AIFloat3 pendingStart;
	int squareSize;
};

} // namespace circuit

#endif // SRC_CIRCUIT_TERRAIN_PATH_TRAVELMAP_H_

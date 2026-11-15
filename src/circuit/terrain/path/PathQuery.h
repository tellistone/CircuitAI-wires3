/*
 * PathQuery.h
 *
 *  Created on: Apr 22, 2020
 *      Author: rlcevg
 */

#ifndef SRC_CIRCUIT_TERRAIN_PATH_PATHQUERY_H_
#define SRC_CIRCUIT_TERRAIN_PATH_PATHQUERY_H_

#include "terrain/path/PathFinder.h"

namespace circuit {

class CCircuitUnit;
class IUnitTask;

class IPathQuery {
public:
	enum class Type: char {NONE = 0, SINGLE, MULTI, COST, LINE, _SIZE_};
	enum class State: char {NONE = 0, PROCESS, READY, _SIZE_};

protected:
	IPathQuery(const CPathFinder& pathfinder, int id, Type type);
	virtual ~IPathQuery();

public:
	int GetId() const { return id; }
	Type GetType() const { return type; }

	void SetState(State value) { state.store(value); }
	State GetState() const { return state.load(); }
	void SetGeneration(int value) { generation = value; }
	// Still waiting, but its pathfinder changed scheduler since it was run: the result will never come
	bool IsLost() const;

	void Init(const bool* canMoveArray, const float* threatArray,
			  NSMicroPather::CostFunc&& moveFun, NSMicroPather::CostFunc&& threatFun,
			  CCircuitUnit* unit = nullptr);

	const bool* GetCanMoveArray() const { return canMoveArray; }
	const float* GetThreatArray() const { return threatArray; }
	const NSMicroPather::CostFunc& GetMoveFun() const { return moveFun; }
	const NSMicroPather::CostFunc& GetThreatFun() const { return threatFun; }
	// Threat at or below this counts as none: no route cost, and the path is straightened across it.
	// Call after creating the query, before running it. The threat cap (maxThreat) is unchanged
	void SetThreatFloor(float value);
	// Threat cost against the allowance, the threat the group tolerates at a cell. It depends only on distance from
	// basePos: allowByDist[i] is the allowance at i x step elmos (the last entry beyond). Call after creating the query,
	// before running it (SetThreatFloor in either order).
	// isDynamic: the usual 2x threat weight becomes (1 + threat / allowance), so a cell costs threat x (1 + threat /
	// allowance) instead of 2 x threat: cheaper below the allowance, the same at it, steeper above. Units pathed on the
	// cloak array (weight 1x) get half of that. maxWeight caps (1 + threat / allowance) (0 or less = no cap).
	// margin < 1 (quota.danger_margin): a cell costs more from margin x the allowance on, up to marginMod times as much at
	// the allowance and above, so a route keeps clear of ground just under the escape's line where it can (it may still
	// cross it: the cap alone blocks)
	void SetThreatAllowance(std::vector<float>&& allowByDist, float step, const springai::AIFloat3& basePos,
			int pathMapXSize, int squareSize, bool isDynamic, float maxWeight, float margin, float marginMod);
	float GetThreatFloor() const { return threatFloor; }
	const FloatVec& GetHeightMap() const { return heightMap; }

	CCircuitUnit* GetUnit() const { return unit; }

	void HoldTask(IUnitTask* task);  // avoid heap-use-after-free

protected:
	const CPathFinder& pathfinder;  // NOTE: double-check threaded calls
	const FloatVec& heightMap;

	int id;
	Type type;
	std::atomic<State> state;
	int generation = 0;  // CPathFinder::GetGeneration() when run

	const bool* canMoveArray;  // outdate after AREA_UPDATE_RATE
	const float* threatArray;  // outdate after THREAT_UPDATE_RATE
	NSMicroPather::CostFunc moveFun;  // AREA_UPDATE_RATE
	NSMicroPather::CostFunc threatFun;  // THREAT_UPDATE_RATE
	float threatFloor = 0.f;

	CCircuitUnit* unit;  // optional, non-safe

	IUnitTask* taskHolder;
};

} // namespace circuit

#endif // SRC_CIRCUIT_TERRAIN_PATH_PATHQUERY_H_

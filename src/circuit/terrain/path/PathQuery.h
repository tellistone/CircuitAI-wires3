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
	// Dynamic threat cost: the usual 2x threat weight becomes (1 + threat / strength), so a cell costs
	// threat x (1 + threat / strength) instead of 2 x threat: half price for light threat, the same at threat ==
	// strength, steeper above. Units pathed on the cloak array (weight 1x) get half of that. Call after creating
	// the query, before SetThreatFloor (either order works) and before running it
	void SetThreatStrength(float strength);
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

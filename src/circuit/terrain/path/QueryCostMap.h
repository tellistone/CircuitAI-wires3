/*
 * QueryCostMap.h
 *
 *  Created on: Apr 26, 2020
 *      Author: rlcevg
 */

#ifndef SRC_CIRCUIT_TERRAIN_PATH_QUERYCOSTMAP_H_
#define SRC_CIRCUIT_TERRAIN_PATH_QUERYCOSTMAP_H_

#include "terrain/path/PathQuery.h"

namespace circuit {

class CQueryCostMap: public IPathQuery {
public:
	CQueryCostMap(const CPathFinder& pathfinder, int id);
	virtual ~CQueryCostMap();

	// isLength: also each square's route length (GetLength)
	void InitQuery(const springai::AIFloat3& startPos, bool isBottleneck = false, bool isLength = false);

	void Prepare();

	// Process Data
	std::vector<float>& GetCostMapRef() { return costMap; }
	std::vector<float>* GetLenMapPtr() { return isLength ? &lenMap : nullptr; }

	// Input Data
	const springai::AIFloat3& GetStartPos() const { return startPos; }
	bool IsBottleneck() const { return isBottleneck; }

	// Result
	float GetCostAt(const springai::AIFloat3& endPos, int radius) const;
	// Cost of the one square under pos, -1 if not reached
	float GetCostAtSquare(const springai::AIFloat3& pos) const;
	// isLength: the cheapest route's length in elmos to the square under pos; not reached (a building on it), the
	// cheapest of GetCostAt's ring points within radius. -1: none reached
	float GetLength(const springai::AIFloat3& pos, int radius) const;

private:
	std::vector<float> costMap;
	std::vector<float> lenMap;  // isLength: in squares

	springai::AIFloat3 startPos;
	bool isBottleneck = false;
	bool isLength = false;
};

} // namespace circuit

#endif // SRC_CIRCUIT_TERRAIN_PATH_QUERYCOSTMAP_H_

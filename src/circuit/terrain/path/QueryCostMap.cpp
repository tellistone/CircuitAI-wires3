/*
 * QueryCostMap.cpp
 *
 *  Created on: Apr 26, 2020
 *      Author: rlcevg
 */

#include "terrain/path/QueryCostMap.h"

namespace circuit {

using namespace springai;

CQueryCostMap::CQueryCostMap(const CPathFinder& pathfinder, int id)
		: IPathQuery(pathfinder, id, Type::COST)
{
}

CQueryCostMap::~CQueryCostMap()
{
}

void CQueryCostMap::InitQuery(const AIFloat3& startPos, bool isBottleneck, bool isLength)
{
	this->startPos = startPos;
	this->isBottleneck = isBottleneck;
	this->isLength = isLength;
}

void CQueryCostMap::Prepare()
{
	// TODO: Cache to avoid memory allocations
	costMap.resize(pathfinder.GetPathMapXSize() * pathfinder.GetPathMapYSize(), -1.f);
	if (isLength) {
		lenMap.resize(costMap.size(), -1.f);
	}
//	std::fill(costMap.begin(), costMap.end(), -1.f);
}

/*
 * WARNING: endPos must be correct
 */
float CQueryCostMap::GetCostAt(const AIFloat3& endPos, int radius) const
{
	float pathCost = -1.f;
	radius /= pathfinder.GetSquareSize();

	int xm, ym;
	pathfinder.Pos2PathXY(endPos, &xm, &ym);

	auto minCost = pathfinder.IsInPathMap(xm, ym, radius)
	? std::function<float (float, int, int)>([this](float cost, int x, int y) -> float {
		const float costR = costMap[pathfinder.PathXY2PathIndex(x, y)];
		if (cost < 0.f) {
			return costR;
		}
		return (costR < 0.f) ? cost : std::min(cost, costR);
	})
	: std::function<float (float, int, int)>([this](float cost, int x, int y) -> float {
		if (!pathfinder.IsInPathMap(x, y)) {
			return cost;
		}
		const float costR = costMap[pathfinder.PathXY2PathIndex(x, y)];
		if (cost < 0.f) {
			return costR;
		}
		return (costR < 0.f) ? cost : std::min(cost, costR);
	});

	// test 8 points
	pathCost = minCost(pathCost, xm - radius, ym);
	pathCost = minCost(pathCost, xm + radius, ym);
	pathCost = minCost(pathCost, xm, ym - radius);
	pathCost = minCost(pathCost, xm, ym + radius);

	const int r2 = radius * ISQRT_2 + 1;
	pathCost = minCost(pathCost, xm - r2, ym - r2);
	pathCost = minCost(pathCost, xm + r2, ym - r2);
	pathCost = minCost(pathCost, xm - r2, ym + r2);
	pathCost = minCost(pathCost, xm + r2, ym + r2);

	return pathCost;
}

float CQueryCostMap::GetCostAtSquare(const AIFloat3& pos) const
{
	int x, y;
	pathfinder.Pos2PathXY(pos, &x, &y);
	if (!pathfinder.IsInPathMap(x, y)) {
		return -1.f;
	}
	return costMap[pathfinder.PathXY2PathIndex(x, y)];
}

float CQueryCostMap::GetLength(const AIFloat3& pos, int radius) const
{
	if (!isLength) {
		return -1.f;
	}
	int xm, ym;
	pathfinder.Pos2PathXY(pos, &xm, &ym);
	const int r = radius / pathfinder.GetSquareSize();
	const int r2 = r * ISQRT_2 + 1;
	// The square itself first, then GetCostAt's ring points
	const int points[9][2] = {{xm, ym}, {xm - r, ym}, {xm + r, ym}, {xm, ym - r}, {xm, ym + r},
			{xm - r2, ym - r2}, {xm + r2, ym - r2}, {xm - r2, ym + r2}, {xm + r2, ym + r2}};
	float minCost = -1.f;
	float len = -1.f;
	for (int i = 0; i < 9; ++i) {
		if (!pathfinder.IsInPathMap(points[i][0], points[i][1])) {
			continue;
		}
		const int index = pathfinder.PathXY2PathIndex(points[i][0], points[i][1]);
		if ((costMap[index] >= 0.f) && ((minCost < 0.f) || (costMap[index] < minCost))) {
			minCost = costMap[index];
			len = lenMap[index];
		}
		if ((i == 0) && (len >= 0.f)) {
			break;
		}
	}
	return (len >= 0.f) ? len * pathfinder.GetSquareSize() : -1.f;
}

} // namespace circuit

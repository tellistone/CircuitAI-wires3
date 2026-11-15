/*
 * PathQuery.cpp
 *
 *  Created on: Apr 22, 2020
 *      Author: rlcevg
 */

#include "terrain/path/PathQuery.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <cmath>
#include "terrain/path/PathFinder.h"
#include "task/UnitTask.h"

namespace circuit {

IPathQuery::IPathQuery(const CPathFinder& pathfinder, int id, Type type)
		: pathfinder(pathfinder)
		, heightMap(pathfinder.GetHeightMap())
		, id(id)
		, type(type)
		, state(State::NONE)
		, canMoveArray(nullptr)
		, threatArray(nullptr)
		, unit(nullptr)
		, taskHolder(nullptr)
{
}

IPathQuery::~IPathQuery()
{
	if (taskHolder != nullptr) {
		taskHolder->Release();
	}
}

void IPathQuery::SetThreatAllowance(std::vector<float>&& allowByDist, float step, const springai::AIFloat3& basePos,
		int pathMapXSize, int squareSize, bool isDynamic, float maxWeight, float margin, float marginMod)
{
	const float hotLoad = ((margin < 1.f) && (marginMod > 1.f)) ? std::max(margin, 0.f) : std::numeric_limits<float>::max();
	if (allowByDist.empty() || !(step > 0.f) || (pathMapXSize <= 0) || (!isDynamic && (hotLoad > 1.f))) {
		return;  // keep the flat weight
	}
	// Stored as 1 / allowance; shared, as the cost function is copied for each run
	for (float& a : allowByDist) {
		a = 1.f / std::max(a, 1e-3f);
	}
	auto invAllow = std::make_shared<const std::vector<float>>(std::move(allowByDist));
	const float* arr = threatArray;
	const float invStep = 1.f / step;
	const float bx = basePos.x;
	const float bz = basePos.z;
	const float sq = static_cast<float>(squareSize);
	const int last = static_cast<int>(invAllow->size()) - 1;
	const float maxW = (maxWeight > 0.f) ? maxWeight : std::numeric_limits<float>::max();
	threatFun = [fun = std::move(threatFun), arr, invAllow, invStep, bx, bz, sq, pathMapXSize, last, isDynamic, maxW,
			hotLoad, marginMod](int index) {
		const int z = index / pathMapXSize;
		const float dx = ((index - z * pathMapXSize) + 0.5f) * sq - bx;
		const float dz = (z + 0.5f) * sq - bz;
		const int i = std::min(static_cast<int>(sqrtf(dx * dx + dz * dz) * invStep), last);
		const float load = arr[index] * (*invAllow)[i];  // threat / allowance
		const float weight = isDynamic ? 0.5f * std::min(1.f + load, maxW) : 1.f;
		return fun(index) * ((load >= hotLoad) ? weight * marginMod : weight);
	};
}

void IPathQuery::SetThreatFloor(float value)
{
	threatFloor = std::max(value, 0.f);
	if (threatFloor <= 0.f) {
		return;
	}
	const float* arr = threatArray;
	const float floor = threatFloor;
	threatFun = [fun = std::move(threatFun), arr, floor](int index) {
		return (arr[index] <= floor) ? 0.f : fun(index);
	};
}

void IPathQuery::Init(const bool* canMoveArray, const float* threatArray,
		NSMicroPather::CostFunc&& moveFun, NSMicroPather::CostFunc&& threatFun,
		CCircuitUnit* unit)
{
	this->canMoveArray = canMoveArray;
	this->threatArray = threatArray;
	this->moveFun = moveFun;
	this->threatFun = threatFun;
	this->unit = unit;  // optional
}

void IPathQuery::HoldTask(IUnitTask* task)
{
	taskHolder = task;
	taskHolder->AddRef();
}

bool IPathQuery::IsLost() const
{
	return (State::PROCESS == GetState()) && (generation != pathfinder.GetGeneration());
}

} // namespace circuit

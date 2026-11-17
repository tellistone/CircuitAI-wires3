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

void IPathQuery::SetThreatArray(std::shared_ptr<const FloatVec>&& array)
{
	if (array == nullptr) {
		return;
	}
	threatArrayOwner = std::move(array);
	threatArray = threatArrayOwner->data();
	const float* arr = threatArray;  // lives as long as the query, which outlives its runs
	threatFun = [arr](int index) {
		return 2.f * arr[index];
	};
}

void IPathQuery::SetThreatAllowance(std::vector<float>&& allowByDist, float step, const springai::AIFloat3& basePos,
		int pathMapXSize, int squareSize, bool isDynamic, float maxWeight, float margin, float marginMod)
{
	// Margin: the weight rises from 1x at margin x the allowance to marginMod x at the allowance and above. A ramp, not a
	// step, so a small change in threat (or in our strength) doesn't flip a route between two ways
	const bool isMargin = (margin < 1.f) && (marginMod > 1.f);
	const float hotLoad = isMargin ? std::max(margin, 0.f) : 1.f;
	const float rampSlope = isMargin ? (marginMod - 1.f) / (1.f - hotLoad) : 0.f;
	if (allowByDist.empty() || !(step > 0.f) || (pathMapXSize <= 0) || (!isDynamic && !isMargin)) {
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
			hotLoad, rampSlope](int index) {
		const int z = index / pathMapXSize;
		const float dx = ((index - z * pathMapXSize) + 0.5f) * sq - bx;
		const float dz = (z + 0.5f) * sq - bz;
		const int i = std::min(static_cast<int>(sqrtf(dx * dx + dz * dz) * invStep), last);
		const float load = arr[index] * (*invAllow)[i];  // threat / allowance
		const float weight = isDynamic ? 0.5f * std::min(1.f + load, maxW) : 1.f;
		const float ramp = 1.f + rampSlope * std::min(std::max(load - hotLoad, 0.f), 1.f - hotLoad);
		return fun(index) * weight * ramp;
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

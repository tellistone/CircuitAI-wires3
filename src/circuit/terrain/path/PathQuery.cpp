/*
 * PathQuery.cpp
 *
 *  Created on: Apr 22, 2020
 *      Author: rlcevg
 */

#include "terrain/path/PathQuery.h"

#include <algorithm>
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

void IPathQuery::SetThreatStrength(float strength, float maxWeight)
{
	if (!(strength > 0.f)) {
		return;  // no strength known: keep the flat weight
	}
	const float* arr = threatArray;
	const float invStr = 1.f / std::max(strength, 1e-3f);
	if (maxWeight > 0.f) {
		threatFun = [fun = std::move(threatFun), arr, invStr, maxWeight](int index) {
			return fun(index) * 0.5f * std::min(1.f + arr[index] * invStr, maxWeight);
		};
	} else {
		threatFun = [fun = std::move(threatFun), arr, invStr](int index) {
			return fun(index) * 0.5f * (1.f + arr[index] * invStr);
		};
	}
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

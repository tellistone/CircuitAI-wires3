/*
 * TravelAction.cpp
 *
 *  Created on: Feb 16, 2016
 *      Author: rlcevg
 */

#include "unit/action/TravelAction.h"
#include "unit/CircuitUnit.h"
#include "unit/CircuitDef.h"
#include "task/UnitTask.h"
#include "task/fighter/Formation.h"
#include "CircuitAI.h"
#include "util/Utils.h"

#include "AISCommands.h"

namespace circuit {

using namespace springai;

ITravelAction::ITravelAction(CCircuitUnit* owner, Type type, int squareSize, float speed)
		: IUnitAction(owner, type)
		, speed(speed)
		, pathIterator(0)
		, isForce(true)
		, lastFrame(-1)
		, formPos(-RgtVector)
		, formSpeed(NO_SPEED_LIMIT)
{
	CCircuitUnit* unit = static_cast<CCircuitUnit*>(ownerList);
	CCircuitDef* cdef = unit->GetCircuitDef();
	int size = std::max(cdef->GetDef()->GetXSize(), cdef->GetDef()->GetZSize());
	int incMod = std::max(size / 4, 1);
	if (cdef->IsPlane()) {
		incMod *= 8;
	} else if (cdef->IsAbleToFly()) {
		incMod *= 3;
	} else if (cdef->IsTurnLarge()) {
		incMod *= 2;
	}
	increment = incMod * DEFAULT_SLACK / squareSize + 1;
	minSqDist = squareSize * increment;  // / 2;
	minSqDist *= minSqDist;
}

ITravelAction::ITravelAction(CCircuitUnit* owner, Type type, const std::shared_ptr<PathInfo>& pPath,
		int squareSize, float speed)
		: ITravelAction(owner, type, squareSize, speed)
{
	this->pPath = pPath;
}

ITravelAction::~ITravelAction()
{
}

void ITravelAction::OnEnd()
{
	IAction::OnEnd();
	CCircuitUnit* unit = static_cast<CCircuitUnit*>(ownerList);
	unit->GetTask()->OnTravelEnd(unit);  // WARNING: do not clear/delete unit actions
}

void ITravelAction::SetPath(const std::shared_ptr<PathInfo>& pPath, float speed)
{
	pathIterator = 0;
	this->pPath = pPath;
	this->speed = speed;
	isForce = true;
	formPos = -RgtVector;
}

int ITravelAction::CalcSpeedStep(float& stepSpeed)
{
	CCircuitUnit* unit = static_cast<CCircuitUnit*>(ownerList);
	const AIFloat3& pos = unit->GetPos(lastFrame);
	int pathMaxIndex = pPath->posPath.size() - 1;

	int lastStep = pathIterator;
	float sqDistToStep = pos.SqDistance2D(pPath->posPath[pathIterator]);
	int step = std::min(pathIterator + increment, pathMaxIndex);
	float sqNextDistToStep = pos.SqDistance2D(pPath->posPath[step]);
	while ((sqNextDistToStep < sqDistToStep) && (pathIterator < pathMaxIndex)) {
		pathIterator = step;
		sqDistToStep = sqNextDistToStep;
		step = std::min(pathIterator + increment, pathMaxIndex);
		sqNextDistToStep = pos.SqDistance2D(pPath->posPath[step]);
	}

	if (!isForce && (pathIterator == lastStep) && ((int)sqDistToStep > minSqDist)) {
		return -1;
	} else {
		stepSpeed = speed;
	}

	if ((int)sqDistToStep <= minSqDist) {
		pathIterator = step;
		state = (pathIterator == pathMaxIndex) ? State::FINISH : state;
	}

	isForce = false;
	return pathMaxIndex;
}

bool ITravelAction::TryFormationOrder(CCircuitAI* circuit)
{
	CCircuitUnit* unit = static_cast<CCircuitUnit*>(ownerList);
	const CFormation* formation = pPath->posPath.empty() ? nullptr : unit->GetTask()->GetFormation();
	SFormationOrder order;
	if ((formation == nullptr) || !formation->GetOrder(unit, order)) {
		formPos = -RgtVector;  // following the path: the next formation move goes out at once
		return false;
	}
	isForce = true;  // once the formation lets go, path following sends its step at once
	const bool isMove = IsFormationMoveDue(unit->GetPos(lastFrame), order);
	if (!isMove && (std::fabs(order.speed - formSpeed) <= 0.15f * std::fabs(formSpeed))) {
		return true;
	}
	TRY_UNIT(circuit, unit,
		if (isMove) {
			if (IsEqual(Mask::FIGHT)) {
				unit->GetUnit()->Fight(order.pos, UNIT_COMMAND_OPTION_RIGHT_MOUSE_KEY, lastFrame + FRAMES_PER_SEC * 60);
			} else {
				unit->CmdMoveTo(order.pos, UNIT_COMMAND_OPTION_RIGHT_MOUSE_KEY, lastFrame + FRAMES_PER_SEC * 60);
			}
			formPos = order.pos;
		}
		unit->CmdWantedSpeed(order.speed);
		formSpeed = order.speed;
	)
	return true;
}

bool ITravelAction::IsFormationMoveDone(const AIFloat3& pos) const
{
	return utils::is_valid(formPos) && (pos.SqDistance2D(formPos) < SQUARE(DEFAULT_SLACK));
}

bool ITravelAction::IsFormationMoveDue(const AIFloat3& pos, const SFormationOrder& order) const
{
	if (!utils::is_valid(formPos)) {
		return true;
	}
	// Within the order interval + 1 s of the last target: the next one, unless it is the same spot
	CCircuitUnit* unit = static_cast<CCircuitUnit*>(ownerList);
	const float speed = (formSpeed > 0.f) ? formSpeed : unit->GetCircuitDef()->GetSpeed();
	const float reach = std::max<float>(speed * (GetOrderInterval() + FRAMES_PER_SEC) / FRAMES_PER_SEC, DEFAULT_SLACK);
	const float3 toLast = formPos - pos;
	const float sqLast = toLast.SqLength2D();
	if (sqLast < SQUARE(reach)) {
		return order.pos.SqDistance2D(formPos) >= SQUARE(order.slack);
	}
	// The target moved off the way to the last one (a re-plan, a bend, a wider or narrower line) or behind the unit
	const float3 toNew = order.pos - pos;
	const float along = toNew.dot2D(toLast) / std::sqrt(sqLast);
	return (along < -order.slack) || (toNew.SqLength2D() - SQUARE(along) > SQUARE(order.slack));
}

} // namespace circuit

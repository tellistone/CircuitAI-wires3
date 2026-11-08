/*
 * TravelAction.h
 *
 *  Created on: Feb 16, 2016
 *      Author: rlcevg
 */

#ifndef SRC_CIRCUIT_UNIT_ACTION_TRAVELACTION_H_
#define SRC_CIRCUIT_UNIT_ACTION_TRAVELACTION_H_

#include "unit/action/UnitAction.h"
#include "util/Defines.h"

#include <memory>

namespace circuit {

struct SFormationOrder;

class ITravelAction: public IUnitAction {
public:
	ITravelAction(CCircuitUnit* owner, Type type, int squareSize, float speed = NO_SPEED_LIMIT);
	ITravelAction(CCircuitUnit* owner, Type type, const std::shared_ptr<PathInfo>& pPath,
			int squareSize, float speed = NO_SPEED_LIMIT);
	virtual ~ITravelAction();

	virtual void OnEnd() override;

	void SetPath(const std::shared_ptr<PathInfo>& pPath, float speed = NO_SPEED_LIMIT);
	const std::shared_ptr<PathInfo>& GetPath() const { return pPath; }
	void ResetUpdateTimer() { lastFrame = -1; }  // next Update issues commands at once
	// Frames between Updates that send orders: Fight (siege units) every 2 s, Move every second
	int GetOrderInterval() const { return IsEqual(Mask::FIGHT) ? FRAMES_PER_SEC * 2 : FRAMES_PER_SEC; }
	// Stands where its last formation move sent it (none since the last SetPath or path following: false)
	bool IsFormationMoveDone(const springai::AIFloat3& pos) const;

protected:
	// Moves pathIterator on while that brings it closer to pos; returns the squared distance to its point
	float AdvancePathIterator(const springai::AIFloat3& pos);
	int CalcSpeedStep(float& stepSpeed);
	// While the squad's line formation steers the unit, send its formation move instead of path steps (true).
	// Sent only when needed: the first one after a new path, near the last target, or when the target moved off the
	// way to the last one; the wanted speed also when it changes by more than 15%
	bool TryFormationOrder(CCircuitAI* circuit);

	std::shared_ptr<PathInfo> pPath;
	float speed;
	int pathIterator;
	int increment;
	int minSqDist;
	bool isForce;
	int lastFrame;

private:
	bool IsFormationMoveDue(const springai::AIFloat3& pos, const SFormationOrder& order) const;
	springai::AIFloat3 formPos;  // last formation move (-RgtVector: none since SetPath or path following)
	float formSpeed;
};

} // namespace circuit

#endif // SRC_CIRCUIT_UNIT_ACTION_TRAVELACTION_H_

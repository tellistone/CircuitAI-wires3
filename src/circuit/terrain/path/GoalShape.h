/*
 * GoalShape.h
 *
 *  Shape of one goal of a multi-goal route (CQueryPathMulti)
 */

#ifndef SRC_CIRCUIT_TERRAIN_PATH_GOALSHAPE_H_
#define SRC_CIRCUIT_TERRAIN_PATH_GOALSHAPE_H_

#include "AIFloat3.h"

#include <algorithm>
#include <cmath>

namespace circuit {

// Shape of one goal of a multi-goal route (CQueryPathMulti::SetGoalShapes).
// Wedge: its end nodes are the squares within radius of anchor whose direction from it is within halfAngle of the
// direction towards 'towards'; the goal's own position is then only its heuristic node.
// Disc (default): the usual ring of the query's maxRange around the goal's position
struct SGoalShape {
	static SGoalShape Disc() { return SGoalShape(); }
	static SGoalShape Wedge(const springai::AIFloat3& anchor, float radius, const springai::AIFloat3& towards,
			float halfAngle)
	{
		SGoalShape s;
		s.isWedge = true;
		s.anchor = anchor;
		s.radius = radius;
		const float dx = towards.x - anchor.x;
		const float dz = towards.z - anchor.z;
		const float len = sqrtf(dx * dx + dz * dz);
		s.dirX = (len > 1.f) ? dx / len : 1.f;
		s.dirZ = (len > 1.f) ? dz / len : 0.f;
		s.cosHalf = cosf(std::min(halfAngle, static_cast<float>(M_PI)));
		return s;
	}
	// Offset (dx, dz) from the anchor inside the wedge, its radius widened by slack
	bool Contains(float dx, float dz, float slack = 0.f) const {
		const float sqLen = dx * dx + dz * dz;
		return (sqLen <= (radius + slack) * (radius + slack))
			&& (dx * dirX + dz * dirZ >= sqrtf(sqLen) * cosHalf);
	}
	bool Contains(const springai::AIFloat3& p, float slack = 0.f) const {
		return Contains(p.x - anchor.x, p.z - anchor.z, slack);
	}

	bool isWedge = false;
	springai::AIFloat3 anchor;
	float radius = 0.f;
	float dirX = 1.f, dirZ = 0.f;  // unit direction of the wedge's middle
	float cosHalf = 1.f;  // cos(halfAngle)
};

} // namespace circuit

#endif // SRC_CIRCUIT_TERRAIN_PATH_GOALSHAPE_H_

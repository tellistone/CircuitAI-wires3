/*
 * Point.h
 *
 *  Created on: May 13, 2020
 *      Author: rlcevg
 */

#ifndef SRC_CIRCUIT_UTIL_POINT_H_
#define SRC_CIRCUIT_UTIL_POINT_H_

#include "Sim/Misc/GlobalConstants.h"
#include "AIFloat3.h"

namespace utils {

static inline bool is_equal_pos(const springai::AIFloat3& posA, const springai::AIFloat3& posB, const float slack = SQUARE_SIZE * 2)
{
	return (math::fabs(posA.x - posB.x) <= slack) && (math::fabs(posA.z - posB.z) <= slack);
}

static inline springai::AIFloat3 get_near_pos(const springai::AIFloat3& pos, float range)
{
	springai::AIFloat3 offset((float)rand() / RAND_MAX - 0.5f, 0.f, (float)rand() / RAND_MAX - 0.5f);
	return pos + offset * range;
}

static inline springai::AIFloat3 get_radial_pos(const springai::AIFloat3& pos, float radius)
{
	float angle = (float)rand() / RAND_MAX * 2 * M_PI;
	springai::AIFloat3 offset(std::cos(angle), 0.f, std::sin(angle));
	return pos + offset * radius;
}

static inline bool is_valid(const springai::AIFloat3& pos)
{
	return pos.x != -RgtVector.x;
}

// Squared 2D distance from p to the segment a-b
static inline float sq_dist_to_segment_2d(const springai::AIFloat3& p, const springai::AIFloat3& a,
		const springai::AIFloat3& b)
{
	const float dx = b.x - a.x;
	const float dz = b.z - a.z;
	const float sqLen = dx * dx + dz * dz;
	float t = (sqLen > 0.f) ? ((p.x - a.x) * dx + (p.z - a.z) * dz) / sqLen : 0.f;
	t = (t < 0.f) ? 0.f : (t > 1.f) ? 1.f : t;
	const float ex = a.x + t * dx - p.x;
	const float ez = a.z + t * dz - p.z;
	return ex * ex + ez * ez;
}

} // namespace utils

#endif // SRC_CIRCUIT_UTIL_POINT_H_

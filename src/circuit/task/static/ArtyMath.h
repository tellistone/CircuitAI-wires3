/*
 * ArtyMath.h
 *
 *  CPriorityArtyTask's cannon arc maths, ranking and switching rule, with no AI dependencies so an offline harness
 *  can test them. Units: elmos and seconds (v: elmo/s, g: elmo/s^2, g > 0).
 */

#ifndef SRC_CIRCUIT_TASK_STATIC_ARTYMATH_H_
#define SRC_CIRCUIT_TASK_STATIC_ARTYMATH_H_

#include <algorithm>
#include <cmath>

namespace circuit {

namespace ballistics {

// Launch angle (as its tangent) for a shell at speed v under gravity g to a point d away and h higher, on the low
// arc or (isHigh) the high one. false: out of reach
inline bool ArcTan(float d, float h, float v, float g, bool isHigh, float& outTan)
{
	if (d < 1.f) {
		outTan = isHigh ? 1e3f : 0.f;
		return true;
	}
	const float v2 = v * v;
	const float root = v2 * v2 - g * (g * d * d + 2.f * h * v2);
	if (root < 0.f) {
		return false;
	}
	outTan = (v2 + (isHigh ? std::sqrt(root) : -std::sqrt(root))) / (g * d);
	return true;
}

inline bool LowArcTan(float d, float h, float v, float g, float& outTan)
{
	return ArcTan(d, h, v, g, false, outTan);
}

// Seconds in flight over ground distance d at launch angle tan
inline float FlightTime(float d, float tanA, float v)
{
	return d * std::sqrt(1.f + tanA * tanA) / v;  // d / (v cos)
}

// Engine's height-adjusted Cannon range (CCannon::GetRange2D without the downhill boost, so never larger than the
// engine's): range on flat ground, and heightDiff (target minus aim-from point, already x the weapon's heightMod)
inline float CannonRange(float range, float heightDiff, float v, float g)
{
	const float k = 1.f - 4.f * g * heightDiff / (v * v);
	return (k < 0.f) ? 0.f : range * 0.5f * (1.f + std::sqrt(k));
}

// Walk the low arc (isHigh: the high one) from (x0, y0, z0) to (x1, y1, z1) in steps along the ground, stopping
// stopShort before the target. Blocked where the shell is less than clearance above heightAt(x, z) (water: pass the
// surface, 0). false: blocked or out of reach
template<typename HeightAt>
bool IsArcClear(float x0, float y0, float z0, float x1, float y1, float z1, float v, float g,
		float step, float clearance, float stopShort, HeightAt heightAt, bool isHigh = false)
{
	const float dx = x1 - x0;
	const float dz = z1 - z0;
	const float d = std::sqrt(dx * dx + dz * dz);
	float tanA;
	if (!ArcTan(d, y1 - y0, v, g, isHigh, tanA)) {
		return false;
	}
	const float end = d - stopShort;
	if (end <= step) {
		return true;
	}
	const float ux = dx / d;
	const float uz = dz / d;
	const float drop = g * (1.f + tanA * tanA) / (2.f * v * v);  // y(x) = y0 + x tanA - drop x^2
	for (float x = step; x < end; x += step) {
		const float y = y0 + x * (tanA - drop * x);
		if (y - heightAt(x0 + ux * x, z0 + uz * x) < clearance) {
			return false;
		}
	}
	return true;
}

} // namespace ballistics

namespace arty {

// What ranks a target
struct SRank {
	int tier;  // 0 is the top
	float value;  // cost, or splash value
	float turnSec;  // turning time to it
	bool isFar;  // mobile and too far to turn to (or moving too fast to hit): after all others
	bool isFinished;
	bool isVisible;  // in LOS
};

// a before b: not far; tier; finished; in LOS; value (highest); turning time (least)
inline bool IsRankedHigher(const SRank& a, const SRank& b)
{
	if (a.isFar != b.isFar) {
		return !a.isFar;
	}
	if (a.tier != b.tier) {
		return a.tier < b.tier;
	}
	if (a.isFinished != b.isFinished) {
		return a.isFinished;
	}
	if (a.isVisible != b.isVisible) {
		return a.isVisible;
	}
	if (a.value != b.value) {
		return a.value > b.value;
	}
	return a.turnSec < b.turnSec;
}

// cand ranks higher than the current target cur. Switch for a higher tier than pickTier (cur's when it was picked,
// or better since); in that tier, for finished over a nanoframe, else for valueMod x the value. Sight alone never
inline bool IsSwitchWorth(const SRank& cand, const SRank& cur, int pickTier, float valueMod)
{
	if (cand.tier != pickTier) {
		return cand.tier < pickTier;
	}
	if (cand.isFinished != cur.isFinished) {
		return cand.isFinished;
	}
	return cand.value >= valueMod * cur.value;
}

// Turning-weighted value: halved by halfSec of turning (a gun that slows down while it turns); halfSec <= 0: value
inline float TurnWeighted(float value, float turnSec, float halfSec)
{
	return (halfSec > 0.f) ? value / (1.f + turnSec / halfSec) : value;
}

// Turning time from the turret's heading and pitch to a target's (degrees), at yaw and pitch degrees a second.
// isFlip: the gun can fire backwards, so it never turns its heading more than 90. pitchRate <= 0: pitch is free
inline float TurnTime(float fromHeading, float fromPitch, float toHeading, float toPitch, float yawRate,
		float pitchRate, bool isFlip)
{
	float dHeading = std::fmod(std::fabs(toHeading - fromHeading), 360.f);
	if (dHeading > 180.f) {
		dHeading = 360.f - dHeading;
	}
	if (isFlip && (dHeading > 90.f)) {
		dHeading = 180.f - dHeading;
	}
	const float yawSec = dHeading / yawRate;
	return (pitchRate > 0.f) ? std::max(yawSec, std::fabs(toPitch - fromPitch) / pitchRate) : yawSec;
}

// Frames a target is skipped after its count-th failure: skip x 2^(count - 1), doubling up to maxShift times
inline int FailSkipFrames(int count, int skip, int maxShift)
{
	return skip << std::min(std::max(count - 1, 0), maxShift);
}

} // namespace arty

} // namespace circuit

#endif // SRC_CIRCUIT_TASK_STATIC_ARTYMATH_H_

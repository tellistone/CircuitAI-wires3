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

// CPriorityArtyTask's shell, which CSpotScorer::FindGunSpot simulates too. The engine tests a Cannon's line of fire
// (CCannon::HaveFreeLineOfFire) from its aim-from piece, against the ground over the first 15/16 of the distance
constexpr float AIM_FROM_HEIGHT = 112.f;  // elmos above the gun's base a shell starts at (its aim-from piece: Big
                                          // Bertha's query is 113 up, the Disco Rave Party's spindle 112)
constexpr float AIM_HEIGHT = 20.f;  // ... and ends at above the target's base (water: its surface)
constexpr float AIM_RANGE_MOD = 0.97f;  // aimed within this much of the height-adjusted range
constexpr float ARC_SAMPLE_STEP = 16.f;  // elmos between arc samples
constexpr float ARC_MIN_CLEARANCE = 8.f;  // blocked where the shell is less than this above the ground
constexpr float ARC_END_SHORT = 44.f;  // last sample at least this far short of the target
constexpr float ARC_END_SHARE = 1.f / 16.f;  // ... and this share of the distance (the engine's skipped end)

struct SShell {
	float speed = 1050.f;  // elmo/s
	float gravity = 120.f;  // elmo/s^2: its myGravity, else ZK's for every cannon
	float heightMod = 0.8f;  // of its range
	bool isCannon = false;
};

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

// Walk the low arc (isHigh: the high one) from (x0, y0, z0) to (x1, y1, z1) every ARC_SAMPLE_STEP along the ground,
// stopping ARC_END_SHORT or ARC_END_SHARE of the distance (the more) before the target. Blocked where the shell is
// less than ARC_MIN_CLEARANCE above heightAt(x, z) (water: pass the surface, 0). false: blocked or out of reach
template<typename HeightAt>
bool IsArcClear(float x0, float y0, float z0, float x1, float y1, float z1, float v, float g, HeightAt heightAt,
		bool isHigh = false)
{
	const float dx = x1 - x0;
	const float dz = z1 - z0;
	const float d = std::sqrt(dx * dx + dz * dz);
	float tanA;
	if (!ArcTan(d, y1 - y0, v, g, isHigh, tanA)) {
		return false;
	}
	const float end = d - std::max(ARC_END_SHORT, d * ARC_END_SHARE);
	if (end <= ARC_SAMPLE_STEP) {
		return true;
	}
	const float ux = dx / d;
	const float uz = dz / d;
	const float drop = g * (1.f + tanA * tanA) / (2.f * v * v);  // y(x) = y0 + x tanA - drop x^2
	for (float x = ARC_SAMPLE_STEP; x < end; x += ARC_SAMPLE_STEP) {
		const float y = y0 + x * (tanA - drop * x);
		if (y - heightAt(x0 + ux * x, z0 + uz * x) < ARC_MIN_CLEARANCE) {
			return false;
		}
	}
	return true;
}

// A shot from an aim-from point at (x0, y0, z0) to an aim point (x1, y1, z1): in reach (the height-adjusted range,
// heightDiff x heightMod, x AIM_RANGE_MOD) and on a clear low arc
template<typename HeightAt>
bool IsShotClear(float x0, float y0, float z0, float x1, float y1, float z1, float range, const SShell& shell,
		HeightAt heightAt)
{
	const float reach = CannonRange(range, (y1 - y0) * shell.heightMod, shell.speed, shell.gravity) * AIM_RANGE_MOD;
	if ((x1 - x0) * (x1 - x0) + (z1 - z0) * (z1 - z0) > reach * reach) {
		return false;
	}
	return IsArcClear(x0, y0, z0, x1, y1, z1, shell.speed, shell.gravity, heightAt);
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

/*
 * Satellite beam (Starlight): the satellite hangs over the base and turns its heading (0 = +z, 90 = +x) and its pitch
 * (from straight down) at fixed rates. The beam lands drop x tan(pitch) from the base, drop being the satellite's
 * height above the ground there. Degrees, elmos and seconds
 */
namespace beam {

constexpr float DEG_TO_RAD = M_PI / 180.f;
constexpr float RAD_TO_DEG = 180.f / M_PI;

// Ground reach of a beam with 3D range from a satellite drop above the ground
inline float Reach(float range, float drop)
{
	return std::sqrt(std::max(range * range - drop * drop, 0.f));
}

// Pitch from straight down to a point dist away on the ground and drop below the satellite
inline float Pitch(float dist, float drop)
{
	return std::atan2(dist, std::max(drop, 1.f)) * RAD_TO_DEG;
}

// to - from the short way round, in [-180, 180)
inline float HeadingDiff(float from, float to)
{
	const float d = std::fmod(to - from + 180.f, 360.f);
	return ((d < 0.f) ? d + 360.f : d) - 180.f;
}

// One step of at most maxStep towards to (a heading: the short way round); at to once within a step
inline float StepHeading(float from, float to, float maxStep)
{
	const float d = HeadingDiff(from, to);
	return (std::fabs(d) <= maxStep) ? to : from + ((d < 0.f) ? -maxStep : maxStep);
}
inline float StepToward(float from, float to, float maxStep)
{
	return (std::fabs(to - from) <= maxStep) ? to : from + ((to < from) ? -maxStep : maxStep);
}

// The multiple of 90 nearest a heading (a docking satellite turns to it)
inline float NearestCardinal(float heading)
{
	return std::round(heading / 90.f) * 90.f;
}

/*
 * The beam's spots on the ground (dx, dz from the base) as the satellite turns from (h0, p0) to (h1, p1), both axes at
 * once at yawRate and pitchRate, with the satellite drop above the ground. Spots are at most spacing apart (with at
 * most maxSpots of them), the first at (h0, p0) and the last at (h1, p1). visit(dx, dz, pitch, along): along is how
 * far the spot has moved so far
 */
template<typename Visit>
void WalkPath(float h0, float p0, float h1, float p1, float yawRate, float pitchRate, float drop, float spacing,
		int maxSpots, Visit visit)
{
	const float dh = HeadingDiff(h0, h1);
	const float dp = p1 - p0;
	const float tH = std::fabs(dh) / yawRate;
	const float tP = (pitchRate > 0.f) ? std::fabs(dp) / pitchRate : 0.f;
	const float tEnd = std::max(tH, tP);
	const float minDt = tEnd / std::max(maxSpots - 1, 1);
	const float wH = yawRate * DEG_TO_RAD;
	const float wP = pitchRate * DEG_TO_RAD;
	float t = 0.f;
	float along = 0.f;
	float lastX = 0.f, lastZ = 0.f;
	for (int i = 0; i < maxSpots; ++i) {
		const float h = (t >= tH) ? h0 + dh : h0 + std::copysign(yawRate * t, dh);
		const float p = (t >= tP) ? p1 : p0 + std::copysign(pitchRate * t, dp);
		const float tanP = std::tan(p * DEG_TO_RAD);
		const float r = drop * tanP;
		const float x = r * std::sin(h * DEG_TO_RAD);
		const float z = r * std::cos(h * DEG_TO_RAD);
		if (i > 0) {
			along += std::sqrt((x - lastX) * (x - lastX) + (z - lastZ) * (z - lastZ));
		}
		lastX = x;
		lastZ = z;
		visit(x, z, p, along);
		if (t >= tEnd) {
			break;
		}
		// The spot moves r x heading rate across and drop x sec^2 x pitch rate along (a little faster further out)
		const float v = ((t < tH) ? r * wH : 0.f) + ((t < tP) ? drop * (1.f + tanP * tanP) * wP : 0.f);
		t = std::min(t + std::max(0.9f * spacing / std::max(v, 1e-3f), minDt), tEnd);
		if (i == maxSpots - 2) {
			t = tEnd;  // the last spot is the end
		}
	}
}

// Is the straight line from a point (x1, y1, z1) up to the satellite (x0, y0, z0) clear of the ground? Walked back from
// the point in steps along the ground, from stopShort away, until the line is above maxHeight. Blocked where it's less
// than clearance above heightAt(x, z)
template<typename HeightAt>
bool IsLineClear(float x0, float y0, float z0, float x1, float y1, float z1, float step, float clearance,
		float stopShort, float maxHeight, HeightAt heightAt)
{
	const float dx = x0 - x1;
	const float dz = z0 - z1;
	const float d = std::sqrt(dx * dx + dz * dz);
	if (d < 1.f) {
		return true;
	}
	const float ux = dx / d;
	const float uz = dz / d;
	const float rise = (y0 - y1) / d;
	for (float s = stopShort; s < d; s += step) {
		const float y = y1 + s * rise;
		if (y > maxHeight + clearance) {
			break;
		}
		if (y - heightAt(x1 + ux * s, z1 + uz * s) < clearance) {
			return false;
		}
	}
	return true;
}

} // namespace beam

} // namespace circuit

#endif // SRC_CIRCUIT_TASK_STATIC_ARTYMATH_H_

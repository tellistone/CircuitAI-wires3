/*
 * EnemyUnit.cpp
 *
 *  Created on: Aug 30, 2015
 *      Author: rlcevg
 */

#include "unit/enemy/EnemyUnit.h"
#include "task/fighter/FighterTask.h"
#include "terrain/TerrainManager.h"
#include "util/Utils.h"

#include "WeaponMount.h"
#include "WrappWeapon.h"

#include <algorithm>
#include <cmath>

namespace circuit {

using namespace springai;

namespace {
// Height above the ground (the heightmap square, CTerrainManager::GetHeightAt; water at its surface) that reads as
// a jumper in flight: walking units stand on it, give or take a square's slope. A Jack's arc passes it 4 frames in
constexpr float JUMP_AIR_MIN = 20.f;
// How far (x the jump's height, + JUMP_AIR_MIN) the height above the ground may stray from the arc's at that point:
// the ground under the arc isn't the line it is drawn over (a drop of 100 elmos is half a Jack's arc height)
constexpr float JUMP_LIFT_TOL = 0.5f;

// Frames to the landing of a jumper seen at lift elmos above the ground and fall elmos above where it lands, with
// velocity vel (elmos per frame), on the jump gadget's arc (unit_jumpjets.lua): x, z linear from start to goal,
// y = 4 h i (1 - i) above that line, i = 0..1 at one step per frame. The flight is about 2 sqrt((L/2)^2 + h^2) long at
// s elmos per frame, so the horizontal speed v = L s / flight gives L and the step. With u = 1 - i the share left and
// dy the drop from start to goal, vel.y = step (4 h (2u - 1) - dy) and fall = u (dy + 4 h (1 - u)): u solves
// 4 h u^2 - (vel.y / step) u - fall = 0. < 0: not on such an arc: faster than its jump could go (thrown, carried),
// too slow to read, or a height above the ground that doesn't fit the arc at i
float GetJumpFramesLeft(const AIFloat3& vel, float lift, float fall, float s, float h, float range)
{
	const float v = vel.Length2D();
	const float r = std::min(v / std::max(s, 1e-3f), 0.99f);
	const float len = 2.f * r * h / sqrtf(1.f - SQUARE(r));
	if ((v < 0.1f) || (h <= 0.f) || (len > range * 1.25f)) {
		return -1.f;
	}
	const float step = v / std::max(std::min(len, range), 1.f);
	const float w = vel.y / step;
	const float u = utils::clamp((w + sqrtf(std::max(SQUARE(w) + 16.f * h * fall, 0.f))) / (8.f * h), 0.f, 1.f);
	if (std::fabs(lift - 4.f * h * u * (1.f - u)) > JUMP_LIFT_TOL * h + JUMP_AIR_MIN) {
		return -1.f;
	}
	return u / step;
}
}

CEnemyUnit::CEnemyUnit(Id unitId, Unit* unit, CCircuitDef* cdef)
		: ICoreUnit(unitId, unit)
		, knownFrame(-1)
		, lastSeen(-1)
		, data(SEnemyData(cdef,
			0.f,         // shieldPower
			0.f,         // health
			false,       // isBeingBuilt
			false,       // isParalyzed
			false,       // isDisarmed
			ZeroVector,  // pos
			ZeroVector,  // vel
			0.f,         // threat
			{0},         // range
			unitId,      // id
			0.f,         // cost
			SEnemyData::LosMask::NONE))  // losStatus
{
	Init();
}

CEnemyUnit::CEnemyUnit(CCircuitDef* cdef, const AIFloat3& pos)
		: ICoreUnit(-1, nullptr)
		, knownFrame(-1)
		, lastSeen(-1)
		, shield(nullptr)
		, data(SEnemyData(cdef,
			cdef->GetMaxShield(),  // shieldPower
			cdef->GetHealth(),  // health
			false,       // isBeingBuilt
			false,       // isParalyzed
			false,       // isDisarmed
			pos,         // pos
			ZeroVector,  // vel
			0.f,         // threat
			{0},         // range
			-1,          // id
			cdef->GetCostM(),  // cost
			SEnemyData::LosMask::NONE))  // losStatus
{
}

CEnemyUnit::~CEnemyUnit()
{
	delete shield;
}

void CEnemyUnit::SetCircuitDef(CCircuitDef* cdef)
{
	data.cdef = cdef;
	Init();
}

void CEnemyUnit::Init()
{
	if (data.cdef == nullptr) {
		data.cost = 0.f;
		shield = nullptr;
	} else {
		data.cost = data.cdef->GetCostM();
		WeaponMount* wpMnt = data.cdef->GetShieldMount();
		shield = (wpMnt == nullptr) ? nullptr : WrappWeapon::GetInstance(unit->GetSkirmishAIId(), id, wpMnt->GetWeaponMountId());
	}
}

float CEnemyUnit::GetRadius() const
{
	if (data.cdef == nullptr) {
		return 1.f;
	}
	return data.cdef->GetRadius();
}

bool CEnemyUnit::IsAttacker() const
{
	if (data.cdef == nullptr) {  // unknown enemy is a threat
		return true;
	}
	return data.cdef->IsAttacker();
}

float CEnemyUnit::GetDamage() const
{
	if (data.cdef == nullptr) {  // unknown enemy is a threat
		return 0.1f;
	}
	float dmg = data.cdef->GetThrDamage();
	if (dmg < 1e-3f) {
		return .0f;
	}
	if (data.isBeingBuilt || data.isParalyzed || data.isDisarmed) {
		return 1e-3f;
	}
	// TODO: Mind the slow down: dps * WeaponDef->GetReload / Weapon->GetReloadTime;
	return dmg;
}

void CEnemyUnit::UpdateInRadarData(const AIFloat3& p)
{
	data.pos = p;
	CTerrainData::CorrectPosition(data.pos);
	data.vel = unit->GetVel();
	blindShots = 0;  // seen
}

void CEnemyUnit::TrackStill(int frame)
{
	// One path square: wedges, route reuse and cells all work at that grain, and units in a blob are pushed about
	constexpr float STILL_TOL = 64.f;
	// The engine's base radar error (CLosHandler::defBaseRadarErrorSize): a radar contact wobbles about the true spot
	constexpr float RADAR_TOL = 96.f;
	// elmos per frame (6 elmos/s): a spot is only taken while the unit is about stopped
	constexpr float STILL_SPEED = 0.2f;
	const float sqDist = (stillSince < 0) ? 0.f : data.pos.SqDistance2D(stillPos);
	if (data.IsInLOS()) {
		if ((stillSince < 0) || (sqDist > SQUARE(STILL_TOL))) {
			const bool isStopped = data.vel.SqLength2D() < SQUARE(STILL_SPEED);
			stillPos = data.pos;
			stillSince = isStopped ? frame : -1;
		}
	} else if ((stillSince >= 0) && (sqDist > SQUARE(STILL_TOL + RADAR_TOL))) {
		stillSince = -1;  // on radar only and away from the spot: moved
	}
	// A gap in updates (out of radar and LOS) changes nothing by itself: on return the unit is judged against its spot
}

bool CEnemyUnit::GetParkedSpot(int frame, int parkedFrames, AIFloat3& outPos, int& outEpoch) const
{
	const CCircuitDef* cdef = data.cdef;
	if ((stillSince < 0) || (frame - stillSince < parkedFrames)
		|| (cdef == nullptr) || !cdef->IsMobile() || cdef->IsAbleToFly()
		|| IsFake() || IsHidden()
		// Out of view for longer we don't know it's still there; a short gap keeps the spot, so the side and bans
		// built for it don't flip on every flicker
		|| !IsRecentlySeen(frame, parkedFrames))
	{
		return false;
	}
	outPos = stillPos;
	outEpoch = stillSince;
	return true;
}

bool CEnemyUnit::UpdateJump(int frame, CTerrainManager* terrainMgr, bool isLand)
{
	const CCircuitDef* cdef = data.cdef;
	const float lift = data.pos.y - std::max(terrainMgr->GetHeightAt(data.pos.x, data.pos.z), 0.f);
	if (!data.IsInLOS() || (lift <= JUMP_AIR_MIN)) {
		isOffArc = false;
		if (IsInFlight() && data.IsInLOS()) {  // down (early on a building, or late): reload from then, or as guessed
			jumpReadyFrame = std::min(jumpReadyFrame, frame + cdef->GetJumpReload());
			landFrame = -1;
		} else if (IsInFlight() && (frame >= landFrame)) {
			landFrame = -1;  // down unseen, as guessed
		}
	} else if (!isOffArc && (!IsInFlight() || (frame >= landFrame))) {  // took off, or still up past the guess
		// The gadget sets the unit's velocity to its step each frame (MoveCtrl.SetVelocity: the unit's speed)
		const AIFloat3& vel = data.vel;
		const float s = cdef->GetJumpSpeed();
		const float h = cdef->GetJumpHeight();
		const float range = cdef->GetJumpRange();
		auto land = [this, terrainMgr, &vel](float n) {  // the spot n frames on, and the height above it
			data.landPos = data.pos + AIFloat3(vel.x, 0.f, vel.z) * n;
			CTerrainManager::CorrectPosition(data.landPos);
			data.landPos.y = terrainMgr->GetHeightAt(data.landPos.x, data.landPos.z);  // the gadget's goal height
			return data.pos.y - data.landPos.y;
		};
		float n = GetJumpFramesLeft(vel, lift, lift, s, h, range);  // as if it lands level with the ground under it
		if (n >= 0.f) {
			n = GetJumpFramesLeft(vel, lift, land(n), s, h, range);  // then from the height of that spot
		}
		if (n >= 0.f) {
			land(n);
			landFrame = frame + std::max(int(n + 0.5f), 1);
			jumpReadyFrame = landFrame + cdef->GetJumpReload();
		} else {
			// Up, but off a jump's arc: carried by a transport, thrown, or no jump after all. Not taken as one till it's
			// seen down or out of LOS (a transport slowing down to drop it is too slow for its cruise and reads as a
			// jump), and a flight under way ends ready, the safe side
			isOffArc = true;
			if (IsInFlight()) {
				jumpReadyFrame = frame;
				landFrame = -1;
			}
		}
	}
	data.isJumping = isLand && IsInFlight();
	const bool isReady = !IsInFlight() && (frame >= jumpReadyFrame);
	const bool isChanged = (isReady != isJumpReady);
	isJumpReady = isReady;
	return isChanged;
}

void CEnemyUnit::UpdateInLosData()
{
	if (shield != nullptr) {
		data.shieldPower = shield->GetShieldPower();
	}
	data.health = unit->GetHealth();
	data.isParalyzed = unit->IsParalyzed();
	data.isBeingBuilt = unit->IsBeingBuilt();
	data.isDisarmed = unit->GetRulesParamFloat("disarmed", 0) > .0f;
}

CEnemyInfo::CEnemyInfo(CEnemyUnit* data)
		: data(data)
{
}

CEnemyInfo::~CEnemyInfo()
{
	for (IFighterTask* task : tasks) {
		task->ClearTarget();
	}
}

CEnemyFake::CEnemyFake(CCircuitDef* cdef, const AIFloat3& pos, int timeout)
		: CEnemyUnit(cdef, pos)
		, timeout(timeout)
{
}

CEnemyFake::~CEnemyFake()
{
}

} // namespace circuit

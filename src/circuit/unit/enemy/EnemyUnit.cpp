/*
 * EnemyUnit.cpp
 *
 *  Created on: Aug 30, 2015
 *      Author: rlcevg
 */

#include "unit/enemy/EnemyUnit.h"
#include "task/fighter/FighterTask.h"
#include "util/Utils.h"

#include "WeaponMount.h"
#include "WrappWeapon.h"

namespace circuit {

using namespace springai;

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
		|| IsFake() || IsHidden())
	{
		return false;
	}
	// Out of view (lastSeen: the frame it left radar) we don't know it's still there; a short gap (radar edge, a
	// jammer) keeps the spot for parkedFrames, so the side and bans built for it don't flip on every flicker
	if (!IsInRadarOrLOS() && ((lastSeen < 0) || (frame - lastSeen >= parkedFrames))) {
		return false;
	}
	outPos = stillPos;
	outEpoch = stillSince;
	return true;
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

/*
 * FighterTask.cpp
 *
 *  Created on: Aug 31, 2015
 *      Author: rlcevg
 */

#include "task/fighter/FighterTask.h"
#include "task/RetreatTask.h"
#include "map/InfluenceMap.h"
#include "map/ThreatMap.h"
#include "module/BuilderManager.h"
#include "module/MilitaryManager.h"
#include "terrain/TerrainManager.h"
#include "setup/SetupManager.h"
#include "unit/action/TravelAction.h"
#include "unit/action/DGunAction.h"
#include "unit/action/RearmAction.h"
#include "unit/enemy/EnemyUnit.h"
#include "CircuitAI.h"
#include "util/Utils.h"

#include <algorithm>
#include <cmath>

namespace circuit {

using namespace springai;

F3Vec IFighterTask::urgentPositions;  // NOTE: micro-opt
F3Vec IFighterTask::enemyPositions;  // NOTE: micro-opt

IFighterTask::IFighterTask(ITaskManager* mgr, FightType type, float powerMod, int timeout)
		: IUnitTask(mgr, Priority::NORMAL, Type::FIGHTER, timeout)
		, fightType(type)
		, position(-RgtVector)
		, attackPower(.0f)
		, powerMod(powerMod)
		, attackFrame(-1)
		, target(nullptr)
{
}

IFighterTask::~IFighterTask()
{
	if (target != nullptr) {
		target->UnbindTask(this);
	}
}

void IFighterTask::AssignTo(CCircuitUnit* unit)
{
	IUnitTask::AssignTo(unit);

	CCircuitDef* cdef = unit->GetCircuitDef();
	attackPower += cdef->GetPower();
	if (unit->HasShield()) {
		shields.insert(unit);
	}

	if (cdef->IsAttrRearm()) {
		unit->PushBack(new CRearmAction(unit));
	}

	if (unit->HasDGun()) {
		const float range = std::max(unit->GetDGunRange() * 1.1f, cdef->GetLosRadius());
		unit->PushDGunAct(new CDGunAction(unit, range));
	}
}

void IFighterTask::RemoveAssignee(CCircuitUnit* unit)
{
	IUnitTask::RemoveAssignee(unit);

	attackPower -= unit->GetCircuitDef()->GetPower();
	cowards.erase(unit);
	if (unit->HasShield()) {
		shields.erase(unit);
	}
}

void IFighterTask::Update()
{
	CCircuitAI* circuit = manager->GetCircuit();
	CMilitaryManager* militaryMgr = circuit->GetMilitaryManager();
	const float minShield = circuit->GetSetupManager()->GetEmptyShield();
	decltype(units) tmpUnits = shields;
	for (CCircuitUnit* unit : tmpUnits) {
		if (!unit->IsShieldCharged(minShield)) {
			CRetreatTask* task = militaryMgr->EnqueueRetreat();
			manager->AssignTask(unit, task);
		}
	}
}

void IFighterTask::OnUnitIdle(CCircuitUnit* unit)
{
	auto it = cowards.find(unit);
	if (it != cowards.end()) {
		cowards.erase(it);
		CRetreatTask* task = manager->GetCircuit()->GetMilitaryManager()->EnqueueRetreat();
		manager->AssignTask(unit, task);
	} else {
		unit->SetTaskFrame(manager->GetCircuit()->GetLastFrame());
	}
}

void IFighterTask::OnUnitDamaged(CCircuitUnit* unit, CEnemyInfo* attacker)
{
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	CCircuitDef* cdef = unit->GetCircuitDef();
	const float healthPerc = unit->GetHealthPercent();
	if (unit->HasShield()) {
		const float minShield = circuit->GetSetupManager()->GetEmptyShield();
		if ((healthPerc > cdef->GetRetreat()) && unit->IsShieldCharged(minShield)) {
			if ((cdef->IsRoleHeavy() || cdef->IsAttrPrioRepair()) && (healthPerc < 0.9f)) {
				circuit->GetBuilderManager()->EnqueueRepair(IBuilderTask::Priority::NOW, unit);
			}
			return;
		}
	} else if ((healthPerc > cdef->GetRetreat()) && !unit->IsDisarmed(frame)) {
		if ((cdef->IsRoleHeavy() || cdef->IsAttrPrioRepair()) && (healthPerc < 0.9f)) {
			circuit->GetBuilderManager()->EnqueueRepair(IBuilderTask::Priority::NOW, unit);
		}
		return;
	} else if (healthPerc < 0.2f) {  // stuck units workaround: they don't shoot and don't see distant threat
		CRetreatTask* task = circuit->GetMilitaryManager()->EnqueueRetreat();
		manager->AssignTask(unit, task);
		return;
	}

	CThreatMap* threatMap = circuit->GetThreatMap();
	const float range = cdef->GetMaxRange();
	if ((target == nullptr) || !target->IsInLOS()) {
		CRetreatTask* task = circuit->GetMilitaryManager()->EnqueueRetreat();
		manager->AssignTask(unit, task);
		return;
	}
	const AIFloat3& pos = unit->GetPos(frame);
	if ((target->GetPos().SqDistance2D(pos) > SQUARE(range)) ||
		(threatMap->GetThreatAt(unit, pos) * 2 > threatMap->GetUnitThreat(unit)))
	{
		CRetreatTask* task = circuit->GetMilitaryManager()->EnqueueRetreat();
		manager->AssignTask(unit, task);
		return;
	}
	cowards.insert(unit);
}

void IFighterTask::OnUnitDestroyed(CCircuitUnit* unit, CEnemyInfo* attacker)
{
	RemoveAssignee(unit);
}

float IFighterTask::GetThrDistModAt(const AIFloat3& pos) const
{
	return manager->GetCircuit()->GetMilitaryManager()->GetThrDistMod(fightType, pos);
}

void IFighterTask::SetTarget(CEnemyInfo* enemy)
{
	if (target != nullptr) {
		target->UnbindTask(this);
	}
	if (enemy != nullptr) {
		enemy->BindTask(this);
	}
	target = enemy;
}

void IFighterTask::Attack(const int frame, const bool isDirect)
{
	const int targetTile = manager->GetCircuit()->GetInflMap()->Pos2Index(target->GetPos());
	const bool isRepeatAttack = (frame >= attackFrame + FRAMES_PER_SEC * 3);
	attackFrame = isRepeatAttack ? frame : attackFrame;
	std::vector<CCircuitUnit*> orderUnits;
	for (CCircuitUnit* unit : units) {
		if (unit->Blocker() != nullptr) {
			continue;  // Do not interrupt current action
		}
		unit->GetTravelAct()->StateWait();

		if (isRepeatAttack
			|| (unit->GetTarget() != target)
			|| (unit->GetTargetTile() != targetTile))
		{
			orderUnits.push_back(unit);
		}
	}
	if (isDirect) {
		for (CCircuitUnit* unit : orderUnits) {
			unit->Attack(target, targetTile, frame + FRAMES_PER_SEC * 60);
		}
		return;
	}

	// Forget spots of units that left the group
	for (auto it = fireSpots.begin(); it != fireSpots.end();) {
		it = (units.find(it->first) == units.end()) ? fireSpots.erase(it) : std::next(it);
	}
	// Units still heading to a spot that is in range of the target pick first (so nobody takes it
	// from them); then longest range first. Later units avoid spots already taken.
	const AIFloat3& tPos = target->GetPos();
	auto hasValidSpot = [&](CCircuitUnit* u) {
		auto it = fireSpots.find(u);
		return (it != fireSpots.end())
			&& (it->second.SqDistance2D(tPos) <= SQUARE(u->GetCircuitDef()->GetMaxRange()));
	};
	std::sort(orderUnits.begin(), orderUnits.end(), [&](CCircuitUnit* a, CCircuitUnit* b) {
		const bool va = hasValidSpot(a), vb = hasValidSpot(b);
		if (va != vb) {
			return va;
		}
		return a->GetCircuitDef()->GetMaxRange() > b->GetCircuitDef()->GetMaxRange();
	});
	// Units not re-ordered this round keep standing where they are: their spots count as taken
	F3Vec claimed;
	for (CCircuitUnit* unit : units) {
		if (std::find(orderUnits.begin(), orderUnits.end(), unit) == orderUnits.end()) {
			claimed.push_back(unit->GetPos(frame));
		}
	}
	const float safeThreat = GetEscapeSafeThreat();
	for (auto it = riskySpotFrames.begin(); it != riskySpotFrames.end();) {  // forget expired (and dead) units
		it = (frame >= it->second + FRAMES_PER_SEC * 3) ? riskySpotFrames.erase(it) : std::next(it);
	}
	for (CCircuitUnit* unit : orderUnits) {
		AIFloat3 firePos;
		const FireSpot spot = unit->GetCircuitDef()->IsAttrMelee() ? FireSpot::NONE
				: FindFireSpot(unit, claimed, safeThreat, firePos);
		if ((FireSpot::SPOT == spot) || (FireSpot::RISKY == spot)) {
			fireSpots[unit] = firePos;
		} else {
			fireSpots.erase(unit);
		}
		if (FireSpot::SPOT == spot) {
			claimed.push_back(firePos);
			unit->AttackFrom(firePos, target, targetTile, frame + FRAMES_PER_SEC * 60);
		} else if (FireSpot::RISKY == spot) {
			// Nowhere safe to shoot from and not safe here either: the least dangerous spot
			claimed.push_back(firePos);
			riskySpotFrames[unit] = frame;
			unit->AttackFrom(firePos, target, targetTile, frame + FRAMES_PER_SEC * 60);
		} else if (FireSpot::HOLD == spot) {
			// Every spot in range is one the escape calls dangerous: stay put, shoot what comes into range
			claimed.push_back(firePos);
			unit->HoldFire(firePos, target, targetTile, frame + FRAMES_PER_SEC * 60);
		} else {
			unit->Attack(target->GetPos(), target, targetTile, frame + FRAMES_PER_SEC * 60);
		}
	}
}

IFighterTask::FireSpot IFighterTask::FindFireSpot(CCircuitUnit* unit, const F3Vec& claimed, float safeThreat, AIFloat3& outPos) const
{
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	CCircuitDef* cdef = unit->GetCircuitDef();
	const AIFloat3& tPos = target->GetPos();
	CCircuitDef* edef = target->GetCircuitDef();
	const CCircuitDef::RangeType rt = ((edef != nullptr) && edef->IsAbleToFly()) ? CCircuitDef::RangeType::AIR
			: (tPos.y < -SQUARE_SIZE * 5) ? CCircuitDef::RangeType::WATER : CCircuitDef::RangeType::LAND;
	const float range = cdef->GetMaxRange(rt);
	if (range < SQUARE_SIZE * 4) {
		return FireSpot::NONE;
	}
	const float radius = range * 0.9f;
	const AIFloat3& uPos = unit->GetPos(frame);
	AIFloat3 dir = uPos - tPos;
	dir.y = 0.f;
	const float uDist = dir.Length2D();
	const float bearing = (uDist > 1.f) ? atan2f(dir.z, dir.x) : 0.f;

	CThreatMap* threatMap = circuit->GetThreatMap();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	const float strength = std::max(cdef->GetPower() * sqrtf(std::max(unit->GetHealthPercent(), 0.f)), THREAT_MIN);
	const float wThreat = circuit->GetMilitaryManager()->GetAttackSpotThreat();
	const float sqCrowd = SQUARE(80.f);
	// Spots the task's escape would call dangerous (same allowance as its IsInDanger) are skipped
	auto isDanger = [&](const AIFloat3& p, float threat) {
		return (safeThreat > 0.f) && (threat >= GetAllowedThreatAt(p, safeThreat));
	};
	bool isFound = false;
	float bestScore = std::numeric_limits<float>::max();
	AIFloat3 riskyPos = -RgtVector;  // best by score among dangerous spots
	float riskyScore = std::numeric_limits<float>::max();
	auto consider = [&](const AIFloat3& p) {
		const float threat = threatMap->GetThreatAt(unit, p);
		int crowd = 0;
		for (const AIFloat3& c : claimed) {
			if (c.SqDistance2D(p) < sqCrowd) {
				++crowd;
			}
		}
		const float s = wThreat * (threat / strength + 0.5f * crowd) + p.distance2D(uPos) / range;
		if (isDanger(p, threat)) {
			if (s < riskyScore) {
				riskyScore = s;
				riskyPos = p;
			}
			return;
		}
		if (s < bestScore) {
			bestScore = s;
			outPos = p;
			isFound = true;
		}
	};

	if (uDist <= range) {  // already in range: standing still is a candidate
		consider(uPos);
	}
	// The spot it was already heading to, if still in range of the target: the distance term favours it
	auto itSpot = fireSpots.find(unit);
	if ((itSpot != fireSpots.end()) && (itSpot->second.SqDistance2D(tPos) <= SQUARE(range))) {
		consider(itSpot->second);
	}
	// 13 spots on the arc, +-90 degrees around our bearing from the target (never round its far side)
	for (int i = -6; i <= 6; ++i) {
		const float angle = bearing + i * static_cast<float>(M_PI) / 12.f;
		AIFloat3 p(tPos.x + radius * cosf(angle), tPos.y, tPos.z + radius * sinf(angle));
		CTerrainManager::CorrectPosition(p);
		if (!terrainMgr->CanMoveToPos(unit->GetArea(), p)) {
			continue;
		}
		consider(p);
	}
	if (!isFound) {
		// Every spot dangerous: hold if where the unit stands is safe; otherwise the least dangerous spot
		const bool isSafeHere = (safeThreat > 0.f) && !isDanger(uPos, threatMap->GetThreatAt(unit, uPos));
		if (isSafeHere) {
			outPos = uPos;
			return FireSpot::HOLD;
		}
		if (utils::is_valid(riskyPos)) {
			outPos = riskyPos;
			return FireSpot::RISKY;
		}
		return FireSpot::NONE;
	}
	return FireSpot::SPOT;
}

#ifdef DEBUG_VIS
void IFighterTask::Log()
{
	IUnitTask::Log();

	CCircuitAI* circuit = manager->GetCircuit();
	circuit->GetDrawer()->AddPoint(position, "position");
	circuit->LOG("fightType: %i | attackPower: %f | powerMod: %f", fightType, attackPower, powerMod);
	if (target != nullptr) {
		circuit->GetDrawer()->AddPoint(target->GetPos(), "target");
	}
}
#endif

} // namespace circuit

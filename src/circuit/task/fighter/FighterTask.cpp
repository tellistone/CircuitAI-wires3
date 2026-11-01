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
	fireSpots.erase(unit);
	riskySpotFrames.erase(unit);
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
	const bool isRepeatAttack = (frame >= attackFrame + attackRepeat);
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

	// Units still heading to a spot in range of the target pick first, then longest range first.
	// Later units avoid spots already taken.
	std::map<CCircuitUnit*, AIFloat3> keptSpots;
	for (CCircuitUnit* unit : orderUnits) {
		AIFloat3 p;
		if (IsFireSpotKept(unit, p)) {
			keptSpots[unit] = p;
		} else {
			fireSpots.erase(unit);
		}
	}
	std::map<CCircuitUnit*, float> ranges;
	for (CCircuitUnit* unit : orderUnits) {
		ranges[unit] = GetFireRange(unit);
	}
	std::sort(orderUnits.begin(), orderUnits.end(), [&](CCircuitUnit* a, CCircuitUnit* b) {
		const bool va = keptSpots.count(a) > 0, vb = keptSpots.count(b) > 0;
		if (va != vb) {
			return va;
		}
		return ranges[a] > ranges[b];
	});
	// Units not re-ordered this round: the spot they're heading to counts as taken, else where they stand
	F3Vec claimed;
	for (CCircuitUnit* unit : units) {
		if (std::find(orderUnits.begin(), orderUnits.end(), unit) == orderUnits.end()) {
			AIFloat3 p;
			claimed.push_back(IsFireSpotKept(unit, p) ? p : unit->GetPos(frame));
		}
	}
	// Spots still being headed to are taken before anyone picks: others avoid them
	for (const auto& kv : keptSpots) {
		claimed.push_back(kv.second);
	}
	const float safeThreat = GetEscapeSafeThreat();
	for (auto it = riskySpotFrames.begin(); it != riskySpotFrames.end();) {  // forget expired (and dead) units
		it = (frame >= it->second + FRAMES_PER_SEC * 3) ? riskySpotFrames.erase(it) : std::next(it);
	}
	for (CCircuitUnit* unit : orderUnits) {
		auto itKept = keptSpots.find(unit);
		if (itKept != keptSpots.end()) {  // its own claim doesn't count against it
			auto itc = std::find_if(claimed.begin(), claimed.end(), [&](const AIFloat3& c) {
				return (c.x == itKept->second.x) && (c.z == itKept->second.z);
			});
			if (itc != claimed.end()) {
				claimed.erase(itc);
			}
		}
		AIFloat3 firePos;
		const FireSpot spot = unit->GetCircuitDef()->IsAttrMelee() ? FireSpot::NONE
				: FindFireSpot(unit, claimed, safeThreat, firePos);
		if ((FireSpot::SPOT == spot) || (FireSpot::RISKY == spot)) {
			fireSpots[unit] = std::make_pair(firePos, frame);
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

float IFighterTask::GetFireRange(CCircuitUnit* unit) const
{
	const AIFloat3& tPos = target->GetPos();
	CCircuitDef* edef = target->GetCircuitDef();
	const CCircuitDef::RangeType rt = ((edef != nullptr) && edef->IsAbleToFly()) ? CCircuitDef::RangeType::AIR
			: (tPos.y < -SQUARE_SIZE * 5) ? CCircuitDef::RangeType::WATER : CCircuitDef::RangeType::LAND;
	return unit->GetCircuitDef()->GetMaxRange(rt);
}

bool IFighterTask::IsFireSpotKept(CCircuitUnit* unit, AIFloat3& outPos) const
{
	auto it = fireSpots.find(unit);
	if (it == fireSpots.end()) {
		return false;
	}
	const float range = GetFireRange(unit);
	const float sqDist = it->second.first.SqDistance2D(target->GetPos());
	if ((manager->GetCircuit()->GetLastFrame() > it->second.second + FRAMES_PER_SEC * 10)
		|| (sqDist > SQUARE(range * 0.95f)) || (sqDist < SQUARE(range * 0.6f)))
	{
		return false;
	}
	outPos = it->second.first;
	return true;
}

IFighterTask::FireSpot IFighterTask::FindFireSpot(CCircuitUnit* unit, const F3Vec& claimed, float safeThreat, AIFloat3& outPos) const
{
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	CCircuitDef* cdef = unit->GetCircuitDef();
	const AIFloat3& tPos = target->GetPos();
	const float range = GetFireRange(unit);
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
	// The walk there counts too: a straight line from the unit (skipping the square it stands in, so a
	// unit already in danger can still step out) that crosses danger would get it pulled back on the way
	const float step = threatMap->GetSquareSize();
	auto isWalkDanger = [&](const AIFloat3& p) {
		if (safeThreat <= 0.f) {
			return false;
		}
		AIFloat3 dir = p - uPos;
		dir.y = 0.f;
		const float dist = dir.Length2D();
		if (dist <= step) {
			return false;
		}
		dir /= dist;
		for (float d = step; d < dist; d += step) {
			AIFloat3 q = uPos + dir * d;
			CTerrainManager::CorrectPosition(q);
			if (isDanger(q, threatMap->GetThreatAt(unit, q))) {
				return true;
			}
		}
		return false;
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
		if (isDanger(p, threat) || isWalkDanger(p)) {
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

	// Standing still, or the spot it was already heading to, while in range but not point-blank
	// (closer than 0.6x range a fresh arc spot steps it back out): the distance term favours them
	const float sqMinDist = SQUARE(range * 0.6f);
	if ((uDist <= range) && (uDist * uDist >= sqMinDist)) {
		consider(uPos);
	}
	AIFloat3 keptPos;
	if (IsFireSpotKept(unit, keptPos)) {
		consider(keptPos);
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

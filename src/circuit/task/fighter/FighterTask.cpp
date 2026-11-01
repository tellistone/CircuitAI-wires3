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
	// Every unit claims where it's heading: its last spot if still usable, else where it stands.
	// Longest range picks first; each pick replaces that unit's claim.
	std::map<CCircuitUnit*, AIFloat3> claims;
	for (CCircuitUnit* unit : units) {
		AIFloat3 p;
		claims[unit] = GetKeptFireSpot(unit, p) ? p : unit->GetPos(frame);
	}
	std::map<CCircuitUnit*, float> ranges;
	for (CCircuitUnit* unit : orderUnits) {
		ranges[unit] = GetFireRange(unit);
	}
	std::sort(orderUnits.begin(), orderUnits.end(), [&](CCircuitUnit* a, CCircuitUnit* b) {
		return ranges[a] > ranges[b];
	});
	const float safeThreat = GetEscapeSafeThreat();
	const int timeout = frame + FRAMES_PER_SEC * 60;
	for (CCircuitUnit* unit : orderUnits) {
		AIFloat3 firePos;
		const FireSpot spot = unit->GetCircuitDef()->IsAttrMelee() ? FireSpot::NONE
				: FindFireSpot(unit, claims, safeThreat, firePos);
		switch (spot) {
			case FireSpot::SPOT:
			case FireSpot::RISKY: {  // RISKY: nowhere safe, not here either: the least dangerous spot
				claims[unit] = firePos;
				fireSpots[unit] = {firePos, frame, FireSpot::RISKY == spot};
				if (isFireSpotChase) {
					unit->AttackFrom(firePos, target, targetTile, timeout);
				} else {
					unit->FightFrom(firePos, target, targetTile, timeout);
				}
			} break;
			case FireSpot::HOLD: {  // every spot dangerous, here is safe: stay, fight what comes into range
				claims[unit] = firePos;
				fireSpots.erase(unit);
				unit->HoldFire(firePos, target, targetTile, timeout);
			} break;
			default: {  // melee / no usable range
				fireSpots.erase(unit);
				if (isDirect) {
					unit->Attack(target, targetTile, timeout);  // keeps target priority
				} else {
					unit->Attack(target->GetPos(), target, targetTile, timeout);
				}
			} break;
		}
	}
}

namespace {
const float FIRE_RADIUS   = 0.9f;   // arc spots, x range
const float FIRE_BAND_MIN = 0.6f;   // own/kept spot no closer than this, x range (else step back out)
const float FIRE_BAND_MAX = 0.95f;  // own/kept spot no farther than this, x range
const float CROWD_RADIUS  = 80.f;   // claims within this count as crowding a spot
const float CROWD_WEIGHT  = 0.5f;   // score per crowding claim (x attack_spot_threat)
const int   ARC_STEPS     = 6;      // spots each side of our bearing, PI/24 apart (+-45 degrees)
const float ARC_SPAN      = 0.25f;  // half the arc, x PI
const float SUPPORT_RADIUS = 400.f; // own units heading within this of a spot back up a unit there
const int   SPOT_KEEP     = FRAMES_PER_SEC * 10;  // last spot forgotten this long after its order
const int   RISKY_GRACE   = FRAMES_PER_SEC * 3;   // pull-back leaves a unit sent to a RISKY spot alone
}

float IFighterTask::GetFireRange(CCircuitUnit* unit) const
{
	const AIFloat3& tPos = target->GetPos();
	CCircuitDef* edef = target->GetCircuitDef();
	const CCircuitDef::RangeType rt = ((edef != nullptr) && edef->IsAbleToFly()) ? CCircuitDef::RangeType::AIR
			: (tPos.y < -SQUARE_SIZE * 5) ? CCircuitDef::RangeType::WATER : CCircuitDef::RangeType::LAND;
	return unit->GetCircuitDef()->GetMaxRange(rt);
}

bool IFighterTask::IsInFireBand(CCircuitUnit* unit, const AIFloat3& pos) const
{
	const float range = GetFireRange(unit);
	const float sqDist = pos.SqDistance2D(target->GetPos());
	return (sqDist >= SQUARE(range * FIRE_BAND_MIN)) && (sqDist <= SQUARE(range * FIRE_BAND_MAX));
}

bool IFighterTask::GetKeptFireSpot(CCircuitUnit* unit, AIFloat3& outPos) const
{
	auto it = fireSpots.find(unit);
	if ((it == fireSpots.end())
		|| (manager->GetCircuit()->GetLastFrame() > it->second.frame + SPOT_KEEP)
		|| !IsInFireBand(unit, it->second.pos))
	{
		return false;
	}
	outPos = it->second.pos;
	return true;
}

bool IFighterTask::IsOnRiskySpot(CCircuitUnit* unit, int frame) const
{
	auto it = fireSpots.find(unit);
	return (it != fireSpots.end()) && it->second.isRisky && (frame < it->second.frame + RISKY_GRACE);
}

IFighterTask::FireSpot IFighterTask::FindFireSpot(CCircuitUnit* unit, const std::map<CCircuitUnit*, AIFloat3>& claims,
		float safeThreat, AIFloat3& outPos) const
{
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	CCircuitDef* cdef = unit->GetCircuitDef();
	const AIFloat3& tPos = target->GetPos();
	const float range = GetFireRange(unit);
	if (range < SQUARE_SIZE * 4) {
		return FireSpot::NONE;
	}
	const AIFloat3& uPos = unit->GetPos(frame);
	CThreatMap* threatMap = circuit->GetThreatMap();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	const float strength = std::max(cdef->GetPower() * sqrtf(std::max(unit->GetHealthPercent(), 0.f)), THREAT_MIN);
	const float wThreat = circuit->GetMilitaryManager()->GetAttackSpotThreat();
	const float step = threatMap->GetSquareSize();
	const bool hasDangerLine = (safeThreat > 0.f);
	// Local support: the unit stands alone at its spot, so a spot is judged against the strength of our units
	// heading to within SUPPORT_RADIUS of it (itself included), not the whole group's; never more than that
	std::vector<std::pair<AIFloat3, float>> backers;  // (where each unit is heading, its current strength)
	for (const auto& kv : claims) {
		if (kv.first != unit) {
			CCircuitUnit* u = kv.first;
			backers.emplace_back(kv.second,
					u->GetCircuitDef()->GetPower() * sqrtf(std::max(u->GetHealthPercent(), 0.f)) * powerMod);
		}
	}
	const float ownStrength = strength * powerMod;
	auto supportAt = [&](const AIFloat3& p) {
		float s = ownStrength;
		for (const auto& b : backers) {
			if (b.first.SqDistance2D(p) < SQUARE(SUPPORT_RADIUS)) {
				s += b.second;
			}
		}
		return std::min(s, safeThreat);
	};
	// Danger = what the task's escape calls dangerous (same allowance as its IsInDanger), for the local support
	auto isDanger = [&](const AIFloat3& p, float threat) {
		return hasDangerLine && (threat >= GetAllowedThreatAt(p, supportAt(p)));
	};
	const float startThreat = threatMap->GetThreatAt(unit, uPos);
	const bool isSafeHere = hasDangerLine && !isDanger(uPos, startThreat);

	// Straight walk from the unit to p: is it dangerous, and its worst threat. A unit already in danger may
	// step out: the danger it starts in is ignored until the line leaves it, unless it gets hotter than here.
	auto checkWalk = [&](const AIFloat3& p, float& outPeak) {
		outPeak = 0.f;
		AIFloat3 dir = p - uPos;
		dir.y = 0.f;
		const float dist = dir.Length2D();
		if (!hasDangerLine || (dist <= step)) {
			return false;
		}
		dir /= dist;
		bool isLeft = isSafeHere;
		bool isWalkDanger = false;
		for (float d = step; d < dist; d += step) {
			AIFloat3 q = uPos + dir * d;
			CTerrainManager::CorrectPosition(q);
			const float qThreat = threatMap->GetThreatAt(unit, q);
			if (!isDanger(q, qThreat)) {
				isLeft = true;
			} else if (isLeft || (qThreat > startThreat)) {
				isWalkDanger = true;
				outPeak = std::max(outPeak, qThreat);
			}
		}
		return isWalkDanger;
	};

	bool isFound = false;
	float bestScore = std::numeric_limits<float>::max();
	AIFloat3 riskyPos = -RgtVector;  // best among dangerous spots, judged by the worse of walk and spot
	float riskyScore = std::numeric_limits<float>::max();
	auto consider = [&](const AIFloat3& p) {
		int crowd = 0;
		for (const auto& kv : claims) {
			if ((kv.first != unit) && (kv.second.SqDistance2D(p) < SQUARE(CROWD_RADIUS))) {
				++crowd;
			}
		}
		const float threat = threatMap->GetThreatAt(unit, p);
		float walkPeak;
		const bool isWalkDanger = checkWalk(p, walkPeak);
		const bool isSpotDanger = isDanger(p, threat);
		const float worst = std::max(threat, walkPeak);
		const float s = wThreat * (worst / strength + CROWD_WEIGHT * crowd) + p.distance2D(uPos) / range;
		if (isSpotDanger || isWalkDanger) {
			if (s < riskyScore) {
				riskyScore = s;
				riskyPos = p;
			}
		} else if (s < bestScore) {
			bestScore = s;
			outPos = p;
			isFound = true;
		}
	};

	// Where it stands, and its last spot, while in the fire band: the distance term favours them
	if (IsInFireBand(unit, uPos)) {
		consider(uPos);
	}
	AIFloat3 keptPos;
	if (GetKeptFireSpot(unit, keptPos)) {
		consider(keptPos);
	}
	// Spots on an arc around the target, facing us (+-45 degrees: keeps the group together)
	AIFloat3 dir = uPos - tPos;
	const float bearing = (dir.Length2D() > 1.f) ? atan2f(dir.z, dir.x) : 0.f;
	const float radius = range * FIRE_RADIUS;
	for (int i = -ARC_STEPS; i <= ARC_STEPS; ++i) {
		const float angle = bearing + i * static_cast<float>(M_PI) * ARC_SPAN / ARC_STEPS;
		AIFloat3 p(tPos.x + radius * cosf(angle), tPos.y, tPos.z + radius * sinf(angle));
		CTerrainManager::CorrectPosition(p);
		if (terrainMgr->CanMoveToPos(unit->GetArea(), p)) {
			consider(p);
		}
	}

	if (isFound) {
		return FireSpot::SPOT;
	}
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

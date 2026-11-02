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
	meleeRoutes.erase(unit);
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
		auto itr = meleeRoutes.find(unit);
		if (itr != meleeRoutes.end()) {
			itr->second.frame = -FRAMES_PER_SEC * 3600;  // walked its whole route: the next Attack sends it on
		}
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
// Melee spots: a full ring of 16 spots (PI/8 apart) around the enemy
const int   MELEE_STEPS   = 8;      // spots each side of our bearing
const int   MELEE_HOPS    = 4;      // spots queued per route: the chosen one, then on around the ring
const int   MELEE_HOP_SLOTS = 3;    // ring slots between queued spots (3 x PI/8 = 67.5 degrees)
const float MELEE_SEP     = 0.8f;   // spots of melee units on one enemy at least this x the even-spacing chord apart
const float MELEE_SKIP_NEAR = 0.75f;  // ring spots within this x ring radius of the unit are skipped: keep moving
const int   MELEE_ROUTE_KEEP = FRAMES_PER_SEC * 8;  // replan a route this long after its order at the latest
}

void IFighterTask::Attack(const int frame, const bool isDirect)
{
	CCircuitAI* circuit = manager->GetCircuit();
	const int targetTile = circuit->GetInflMap()->Pos2Index(target->GetPos());
	const bool isRepeatAttack = (frame >= attackFrame + attackRepeat);
	attackFrame = isRepeatAttack ? frame : attackFrame;
	const AIFloat3& tPos = target->GetPos();
	const bool isTileMoveOrder = (retargetMove <= 0.f) || (tPos.SqDistance2D(lastOrderTargetPos) > SQUARE(retargetMove));
	const float meleeSpotRange = circuit->GetMilitaryManager()->GetMeleeSpot().spotRange;
	std::vector<CCircuitUnit*> orderUnits;
	std::vector<CCircuitUnit*> meleeUnits;  // quota.melee: judged every call, AttackMelee decides who gets orders
	for (CCircuitUnit* unit : units) {
		if (unit->Blocker() != nullptr) {
			continue;  // Do not interrupt current action
		}
		if (IsOutOfReach(unit, frame)) {
			continue;  // far behind: keep following the squad's path instead of a straight walk to a spot
		}
		unit->GetTravelAct()->StateWait();

		CCircuitDef* cdef = unit->GetCircuitDef();
		if (cdef->IsAttrMelee() && (cdef->GetMeleeSpotRange(meleeSpotRange) > 0.f)) {  // unit's melee_range, else quota
			meleeUnits.push_back(unit);
			continue;
		}
		if (isRepeatAttack
			|| (unit->GetTarget() != target)
			|| ((unit->GetTargetTile() != targetTile) && isTileMoveOrder))
		{
			orderUnits.push_back(unit);
		}
	}
	if (!orderUnits.empty()) {
		lastOrderTargetPos = tPos;
	}
	// Every unit claims where it's heading: its last spot if still usable, else where it stands.
	// Melee units on a route claim the route spot nearest to them.
	// Longest range picks first; each pick replaces that unit's claim.
	std::map<CCircuitUnit*, AIFloat3> claims;
	for (CCircuitUnit* unit : units) {
		const AIFloat3& uPos = unit->GetPos(frame);
		auto itr = meleeRoutes.find(unit);
		if (itr != meleeRoutes.end()) {
			AIFloat3 nearSpot = itr->second.spots.front();
			for (const AIFloat3& sp : itr->second.spots) {
				if (sp.SqDistance2D(uPos) < nearSpot.SqDistance2D(uPos)) {
					nearSpot = sp;
				}
			}
			claims[unit] = nearSpot;
			continue;
		}
		AIFloat3 p;
		claims[unit] = GetKeptFireSpot(unit, p) ? p : uPos;
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
		const FireSpot spot = unit->GetCircuitDef()->IsAttrMelee() ? FireSpot::NONE  // melee spots off for this unit
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
	if (!meleeUnits.empty()) {
		AttackMelee(frame, meleeUnits, isRepeatAttack, claims, safeThreat, timeout);
	}
}

void IFighterTask::AttackMelee(const int frame, const std::vector<CCircuitUnit*>& meleeUnits, const bool isRepeatAttack,
		std::map<CCircuitUnit*, AIFloat3>& claims, float safeThreat, const int timeout)
{
	CCircuitAI* circuit = manager->GetCircuit();
	const CMilitaryManager::SMeleeSpot& cfg = circuit->GetMilitaryManager()->GetMeleeSpot();
	const int cap = cfg.perTarget;
	const AIFloat3& tPos = target->GetPos();

	// Melee units of our other fighter tasks already on each enemy (pointers compared only)
	std::map<CEnemyInfo*, int> counts;
	if (cap > 0) {
		for (auto& kv : circuit->GetTeamUnits()) {
			CCircuitUnit* u = kv.second;
			IUnitTask* task = u->GetTask();
			if ((task == this) || (task == nullptr) || (task->GetType() != IUnitTask::Type::FIGHTER)
				|| (u->GetTarget() == nullptr) || !u->GetCircuitDef()->IsAttrMelee())
			{
				continue;
			}
			++counts[u->GetTarget()];
		}
	}

	// Who stays on the target: units already on it first, then the nearest; past the cap the rest pick
	// another enemy near it (their previous pick first), else stay on the target after all
	std::vector<CCircuitUnit*> order = meleeUnits;
	std::map<CCircuitUnit*, float> sqDists;
	for (CCircuitUnit* unit : order) {
		sqDists[unit] = unit->GetPos(frame).SqDistance2D(tPos);
	}
	std::sort(order.begin(), order.end(), [&](CCircuitUnit* a, CCircuitUnit* b) {
		const bool onA = (a->GetTarget() == target);
		const bool onB = (b->GetTarget() == target);
		return (onA != onB) ? onA : (sqDists[a] < sqDists[b]);
	});
	std::map<CCircuitUnit*, CEnemyInfo*> assigned;
	std::map<CEnemyInfo*, int> taskCounts;  // this task's melee units per enemy
	for (CCircuitUnit* unit : order) {
		CEnemyInfo* enemy = target;
		if ((cap > 0) && (counts[target] >= cap)) {
			CEnemyInfo* alt = FindMeleeAltTarget(unit, counts, cap, cfg.retargetRadius, safeThreat);
			if (alt != nullptr) {
				enemy = alt;
			}
		}
		assigned[unit] = enemy;
		++counts[enemy];
		++taskCounts[enemy];
	}

	// Who gets new orders: no route yet (on repeat, or a new enemy), a new enemy, the route is old, its enemy moved
	// off the ring, or the unit is on the last leg (or went idle: OnUnitIdle expires the route)
	std::vector<CCircuitUnit*> replans;
	for (CCircuitUnit* unit : order) {
		CEnemyInfo* enemy = assigned[unit];
		auto it = meleeRoutes.find(unit);
		bool isReplan;
		if (it == meleeRoutes.end()) {
			isReplan = isRepeatAttack || (unit->GetTarget() != enemy);
		} else {
			const SMeleeRoute& route = it->second;
			const AIFloat3& uPos = unit->GetPos(frame);
			size_t nearIdx = 0;
			for (size_t i = 1; i < route.spots.size(); ++i) {
				if (route.spots[i].SqDistance2D(uPos) < route.spots[nearIdx].SqDistance2D(uPos)) {
					nearIdx = i;
				}
			}
			const bool isLastLeg = (nearIdx + 2 >= route.spots.size())
					&& (route.spots[nearIdx].SqDistance2D(uPos) < SQUARE(std::max(route.radius, SQUARE_SIZE * 4.f)));
			isReplan = (route.enemy != enemy) || (unit->GetTarget() != enemy)
					|| (frame > route.frame + MELEE_ROUTE_KEEP)
					|| (enemy->GetPos().SqDistance2D(route.enemyPos) > SQUARE(route.radius * 0.5f + SQUARE_SIZE * 2))
					|| isLastLeg
					// sent elsewhere meanwhile (escape, regroup): back to the ring on the next repeat
					|| (isRepeatAttack
						&& (uPos.SqDistance2D(enemy->GetPos()) > SQUARE(route.radius * 2.f + SQUARE_SIZE * 8)));
		}
		if (isReplan) {
			replans.push_back(unit);
		}
	}

	CInfluenceMap* inflMap = circuit->GetInflMap();
	CThreatMap* threatMap = circuit->GetThreatMap();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	for (CCircuitUnit* unit : replans) {  // nearest first: they pick their spots first
		CEnemyInfo* enemy = assigned[unit];
		const int tile = inflMap->Pos2Index(enemy->GetPos());
		CCircuitDef* cdef = unit->GetCircuitDef();
		const float spotRange = cdef->GetMeleeSpotRange(cfg.spotRange);  // behaviour.json per unit "melee_range"
		AIFloat3 pos;
		float angle = 0.f;
		FireSpot spot = FireSpot::NONE;
		float radius = 0.f;
		if (spotRange > 0.f) {
			radius = GetFireRange(unit, enemy) * spotRange;
			// Other melee units' spots on this enemy, and how far apart spots must be: 0.8 x the chord between
			// that many units evenly spread around the ring
			std::vector<AIFloat3> meleeClaims;
			for (const auto& kv : assigned) {
				if ((kv.first != unit) && (kv.second == enemy)) {
					meleeClaims.push_back(claims[kv.first]);
				}
			}
			const int n = std::max(taskCounts[enemy], 2);
			const float minSep = 2.f * radius * sinf(static_cast<float>(M_PI) / n) * MELEE_SEP;
			// Already circling this enemy (a route on it, still near the ring): carry on the same way round
			auto itr = meleeRoutes.find(unit);
			const bool isForward = (itr != meleeRoutes.end()) && (itr->second.enemy == enemy)
					&& (unit->GetPos(frame).SqDistance2D(enemy->GetPos()) < SQUARE(radius * 2.f + SQUARE_SIZE * 8));
			spot = FindMeleeSpot(unit, enemy, spotRange, claims, meleeClaims, minSep, isForward, safeThreat, pos, angle);
			if ((FireSpot::SPOT != spot) && isForward) {  // nothing safe ahead: any way round
				spot = FindMeleeSpot(unit, enemy, spotRange, claims, meleeClaims, minSep, false, safeThreat, pos, angle);
			}
		}
		switch (spot) {
			case FireSpot::SPOT:
			case FireSpot::RISKY: {
				SMeleeRoute route;
				route.spots.push_back(pos);
				route.enemyPos = enemy->GetPos();
				route.enemy = enemy;
				route.radius = radius;
				route.frame = frame;
				// Keep moving: on around the ring, the same way for every unit of the task so they stay apart,
				// up to the first spot that can't be reached or is dangerous. Not from a RISKY spot.
				const AIFloat3& ePos = enemy->GetPos();
				for (int k = 1; (k < MELEE_HOPS) && (FireSpot::SPOT == spot); ++k) {
					const float a = angle + k * MELEE_HOP_SLOTS * static_cast<float>(M_PI) / MELEE_STEPS;
					AIFloat3 p(ePos.x + radius * cosf(a), ePos.y, ePos.z + radius * sinf(a));
					CTerrainManager::CorrectPosition(p);
					if (!terrainMgr->CanMoveToPos(unit->GetArea(), p)
						|| ((safeThreat > 0.f) && (threatMap->GetThreatAt(unit, p) >= GetAllowedThreatAt(p, safeThreat))))
					{
						break;
					}
					route.spots.push_back(p);
				}
				claims[unit] = pos;
				fireSpots[unit] = {pos, frame, FireSpot::RISKY == spot};
				unit->MeleeRoute(route.spots, enemy, tile, timeout);
				meleeRoutes[unit] = std::move(route);
			} break;
			case FireSpot::HOLD: {  // every spot dangerous, here is safe: stay, fight what comes into range
				claims[unit] = pos;
				fireSpots.erase(unit);
				meleeRoutes.erase(unit);
				unit->HoldFire(pos, enemy, tile, timeout);
			} break;
			default: {  // no usable range, or melee_range <= 0 for this unit: close in directly
				fireSpots.erase(unit);
				meleeRoutes.erase(unit);
				unit->Attack(enemy, tile, timeout);
			} break;
		}
	}
}

CEnemyInfo* IFighterTask::FindMeleeAltTarget(CCircuitUnit* unit, const std::map<CEnemyInfo*, int>& counts, int cap,
		float radius, float safeThreat) const
{
	CCircuitAI* circuit = manager->GetCircuit();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	CThreatMap* threatMap = circuit->GetThreatMap();
	CCircuitDef* cdef = unit->GetCircuitDef();
	const int canTargetCat = cdef->GetTargetCategory();
	const AIFloat3& tPos = target->GetPos();
	const AIFloat3& uPos = unit->GetPos(circuit->GetLastFrame());
	CEnemyInfo* best = nullptr;
	float bestSqDist = std::numeric_limits<float>::max();
	for (auto& kv : circuit->GetEnemyInfos()) {
		CEnemyInfo* enemy = kv.second;
		if ((enemy == target) || enemy->IsHidden()) {
			continue;
		}
		const AIFloat3& ePos = enemy->GetPos();
		if (ePos.SqDistance2D(tPos) > SQUARE(radius)) {
			continue;
		}
		auto it = counts.find(enemy);
		if ((it != counts.end()) && (it->second >= cap)) {
			continue;
		}
		CCircuitDef* edef = enemy->GetCircuitDef();
		if (edef != nullptr) {
			if (((edef->GetCategory() & canTargetCat) == 0) || (edef->IsAbleToFly() && !cdef->HasAntiAir())) {
				continue;
			}
		} else if (!cdef->HasAntiWater() && (ePos.y < -SQUARE_SIZE * 5)) {
			continue;
		}
		if (!terrainMgr->CanMoveToPos(unit->GetArea(), ePos)
			|| ((safeThreat > 0.f) && (threatMap->GetThreatAt(unit, ePos) >= GetAllowedThreatAt(ePos, safeThreat))))
		{
			continue;
		}
		// Its previous pick wins unless another is twice as near
		const float sqDist = ePos.SqDistance2D(uPos) * ((enemy == unit->GetTarget()) ? 0.25f : 1.f);
		if (sqDist < bestSqDist) {
			bestSqDist = sqDist;
			best = enemy;
		}
	}
	return best;
}


bool IFighterTask::IsOutOfReach(CCircuitUnit* unit, int frame) const
{
	return (fireSpotReach > 0.f)
		&& (unit->GetPos(frame).SqDistance2D(target->GetPos()) > SQUARE(GetFireRange(unit) * fireSpotReach));
}

float IFighterTask::GetFireRange(CCircuitUnit* unit) const
{
	return GetFireRange(unit, target);
}

float IFighterTask::GetFireRange(CCircuitUnit* unit, CEnemyInfo* enemy) const
{
	const AIFloat3& tPos = enemy->GetPos();
	CCircuitDef* edef = enemy->GetCircuitDef();
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
	const float range = GetFireRange(unit);
	if (range < SQUARE_SIZE * 4) {
		return FireSpot::NONE;
	}
	// Spots on an arc around the target, facing us (+-45 degrees: keeps the group together)
	const AIFloat3& uPos = unit->GetPos(manager->GetCircuit()->GetLastFrame());
	AIFloat3 dir = uPos - target->GetPos();
	const float bearing = (dir.Length2D() > 1.f) ? atan2f(dir.z, dir.x) : 0.f;
	const SSpotShape shape = {range * FIRE_RADIUS, bearing, ARC_SPAN, ARC_STEPS, false, 0.f, nullptr, false};
	float angle;
	return SearchSpot(unit, target, range, shape, claims, safeThreat, outPos, angle);
}

IFighterTask::FireSpot IFighterTask::FindMeleeSpot(CCircuitUnit* unit, CEnemyInfo* enemy, float spotRange,
		const std::map<CCircuitUnit*, AIFloat3>& claims, const std::vector<AIFloat3>& meleeClaims,
		float minSep, bool isForward, float safeThreat, AIFloat3& outPos, float& outAngle) const
{
	const float range = GetFireRange(unit, enemy);
	if (range < SQUARE_SIZE * 4) {
		return FireSpot::NONE;
	}
	// A full ring around the enemy, starting from our side (only the half ahead while already circling it)
	const AIFloat3& uPos = unit->GetPos(manager->GetCircuit()->GetLastFrame());
	AIFloat3 dir = uPos - enemy->GetPos();
	const float bearing = (dir.Length2D() > 1.f) ? atan2f(dir.z, dir.x) : 0.f;
	const SSpotShape shape = {range * spotRange, bearing, 1.f, MELEE_STEPS, true, minSep, &meleeClaims, isForward};
	return SearchSpot(unit, enemy, range, shape, claims, safeThreat, outPos, outAngle);
}

IFighterTask::FireSpot IFighterTask::SearchSpot(CCircuitUnit* unit, CEnemyInfo* enemy, float range,
		const SSpotShape& shape, const std::map<CCircuitUnit*, AIFloat3>& claims, float safeThreat,
		AIFloat3& outPos, float& outAngle) const
{
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	CCircuitDef* cdef = unit->GetCircuitDef();
	const AIFloat3& tPos = enemy->GetPos();
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
	float riskyAngle = 0.f;
	auto consider = [&](const AIFloat3& p, float angle) {
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
				riskyAngle = angle;
			}
		} else if (s < bestScore) {
			bestScore = s;
			outPos = p;
			outAngle = angle;
			isFound = true;
		}
	};

	if (!shape.isMelee) {
		// Where it stands, and its last spot, while in the fire band: the distance term favours them
		if (IsInFireBand(unit, uPos)) {
			consider(uPos, 0.f);
		}
		AIFloat3 keptPos;
		if (GetKeptFireSpot(unit, keptPos)) {
			consider(keptPos, 0.f);
		}
	}
	// Spots on an arc (or full ring) around the enemy. Melee: skip spots next to the unit (it must move on),
	// and in the first pass spots too close to other melee units' spots; a second pass without that
	// separation only if the first found no safe spot
	const bool isFullRing = (shape.arcSpan >= 1.f);
	const int iMin = shape.isForward ? 1  // ahead only: keeps circling the same way as its route did
			: isFullRing ? (-shape.arcSteps + 1) : -shape.arcSteps;  // a full ring: -PI and PI are one spot
	const float skipNear = shape.isMelee ? std::max(shape.radius * MELEE_SKIP_NEAR, SQUARE_SIZE * 2.f) : -1.f;
	const bool hasSepPass = shape.isMelee && (shape.meleeClaims != nullptr) && !shape.meleeClaims->empty()
			&& (shape.minSep > 0.f);
	for (int pass = hasSepPass ? 0 : 1; (pass < 2) && !isFound; ++pass) {
		for (int i = iMin; i <= shape.arcSteps; ++i) {
			const float angle = shape.bearing + i * static_cast<float>(M_PI) * shape.arcSpan / shape.arcSteps;
			AIFloat3 p(tPos.x + shape.radius * cosf(angle), tPos.y, tPos.z + shape.radius * sinf(angle));
			CTerrainManager::CorrectPosition(p);
			if ((skipNear > 0.f) && (p.SqDistance2D(uPos) < SQUARE(skipNear))) {
				continue;
			}
			if (pass == 0) {
				bool isClose = false;
				for (const AIFloat3& c : *shape.meleeClaims) {
					if (c.SqDistance2D(p) < SQUARE(shape.minSep)) {
						isClose = true;
						break;
					}
				}
				if (isClose) {
					continue;
				}
			}
			if (terrainMgr->CanMoveToPos(unit->GetArea(), p)) {
				consider(p, angle);
			}
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
		outAngle = riskyAngle;
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

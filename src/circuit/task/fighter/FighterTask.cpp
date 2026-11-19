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
#include "unit/ally/AllyUnit.h"
#include "unit/CircuitDef.h"
#include "CircuitAI.h"
#include "util/Utils.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace circuit {

using namespace springai;

namespace {
const float FIRE_RADIUS   = 0.9f;   // arc spots, x range
const float FIRE_BAND_MIN = 0.6f;   // own/kept spot no closer than this, x range (else step back out)
const float FIRE_BAND_MAX = 0.95f;  // own/kept spot no farther than this, x range
const float CROWD_WEIGHT  = 0.5f;   // score per crowding claim (x attack_spot_threat)
const int   ARC_STEPS     = 6;      // spots each side of our bearing, PI/24 apart (+-45 degrees)
const float ARC_SPAN      = 0.25f;  // half the arc, x PI
const float SUPPORT_RADIUS = 400.f; // own units heading within this of a spot back up a unit there
const int   SPOT_KEEP     = FRAMES_PER_SEC * 10;  // last spot forgotten this long after its order
const int   RISKY_GRACE   = FRAMES_PER_SEC * 3;   // pull-back leaves a unit sent to a RISKY spot alone
const float WALK_CLIMB    = 40.f;   // with lifts (GetUnitSightLifts): a spot this much up or down needs a direct walk
const int   ARC_STEPS_CLOSE = 12;   // melee close spots: spots each side of our bearing, PI/24 apart (+-90 degrees)
const float ARC_SPAN_CLOSE  = 0.5f; // melee close spots: half the arc, x PI (room round the target to spread out)
const float MELEE_STICK   = 100.f;  // a melee unit already on an enemy counts as this much nearer it (AssignMeleeTargets)
const int   ALT_FAIL_SKIP = FRAMES_PER_SEC * 5;  // another target with no safe spot by it is passed over this long
}

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
	if (manager->GetCircuit()->GetMilitaryManager()->IsAreaDamage(cdef)) {
		areaPower += cdef->GetPower();
	}
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

	CCircuitDef* cdef = unit->GetCircuitDef();
	attackPower -= cdef->GetPower();
	if (manager->GetCircuit()->GetMilitaryManager()->IsAreaDamage(cdef)) {
		areaPower -= cdef->GetPower();
	}
	cowards.erase(unit);
	fireSpots.erase(unit);
	meleeAltFails.erase(unit);
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
	strengthFrame = STALE_FRAME;  // its health changed: GetUnitStrengths reads again
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
	const AIFloat3& tPos = target->GetPos();
	const bool isTileMoveOrder = (retargetMove <= 0.f) || (tPos.SqDistance2D(lastOrderTargetPos) > SQUARE(retargetMove));
	// Every unit claims where it's heading: its last spot if still usable, else where it stands.
	// Longest range picks first; each pick replaces that unit's claim.
	std::vector<SFireClaim> claims;
	for (const SUnitStrength& s : GetUnitStrengths()) {
		AIFloat3 keptPos;
		const bool isMelee = s.unit->GetCircuitDef()->IsAttrMelee();  // its spot may be a close one or a ranged one
		const bool isKept = GetKeptFireSpot(s.unit, isMelee, keptPos) || (isMelee && GetKeptFireSpot(s.unit, false, keptPos));
		claims.push_back({s.unit, isKept ? keptPos : s.unit->GetPos(frame), s.power});
	}
	// Melee spread: past the target's cap, melee units go for other enemies near it
	std::map<CCircuitUnit*, CEnemyInfo*> meleeTargets;
	AssignMeleeTargets(claims, frame, meleeTargets);
	struct SOrder {
		size_t claim;  // index in claims
		float range;
		CEnemyInfo* enemy;  // its target: the group's, or another for melee (AssignMeleeTargets)
	};
	std::vector<SOrder> orders;  // units to order
	for (size_t i = 0; i < claims.size(); ++i) {
		CCircuitUnit* unit = claims[i].unit;
		if ((unit->Blocker() != nullptr) || IsScooting(unit)) {
			continue;  // Do not interrupt current action (or a reloading unit's step back)
		}
		if (IsOutOfReach(unit, frame)) {
			continue;  // far behind: keep following the squad's path instead of a straight walk to a spot
		}
		unit->GetTravelAct()->StateWait();

		auto mit = meleeTargets.find(unit);
		CEnemyInfo* enemy = (mit != meleeTargets.end()) ? mit->second : target;
		if (isRepeatAttack
			|| (unit->GetTarget() != enemy)
			|| ((enemy == target) && (unit->GetTargetTile() != targetTile) && isTileMoveOrder))
		{
			orders.push_back({i, GetSpotRange(unit, enemy), enemy});
		}
	}
	if (orders.empty()) {
		return;
	}
	lastOrderTargetPos = tPos;
	std::stable_sort(orders.begin(), orders.end(), [](const SOrder& a, const SOrder& b) { return a.range > b.range; });
	const float safeThreat = GetEscapeSafeThreat();
	float arcBearing;
	const float* arc = GetFireArcBearing(arcBearing) ? &arcBearing : nullptr;  // once for the whole group
	const int timeout = frame + FRAMES_PER_SEC * 60;
	for (const SOrder& order : orders) {
		SFireClaim& claim = claims[order.claim];
		CCircuitUnit* unit = claim.unit;
		AIFloat3 firePos;
		if ((order.enemy != target) && AttackMeleeOther(claims, order.claim, safeThreat, order.enemy, timeout)) {
			continue;
		}  // else no safe spot by that enemy: the group's target after all
		// Melee: a close spot right next to the target, unless its death explosion is a blast for the unit, then a
		// normal firing spot within weapon range as ranged units pick
		const bool isMelee = IsCloseSpot(unit, target);
		FireSpot spot = FindFireSpot(claims, order.claim, safeThreat, arc, isMelee, firePos);
		// Melee with no safe spot next to the target: a safe spot within weapon range (the ranged search) before it
		// holds or walks in anyway. Units without a real gun find nothing new there (their range is as short)
		if (isMelee && ((FireSpot::HOLD == spot) || (FireSpot::RISKY == spot))) {
			AIFloat3 rangedPos;
			if (FindFireSpot(claims, order.claim, safeThreat, arc, false, rangedPos) == FireSpot::SPOT) {
				spot = FireSpot::SPOT;
				firePos = rangedPos;
			}
		}
		switch (spot) {
			case FireSpot::SPOT:
			case FireSpot::RISKY: {  // RISKY: nowhere safe, not here either: the least dangerous spot
				claim.pos = firePos;
				fireSpots[unit] = {firePos, frame, FireSpot::RISKY == spot};
				if (isFireSpotChase) {
					unit->AttackFrom(firePos, target, targetTile, timeout);
				} else {
					unit->FightFrom(firePos, target, targetTile, timeout);
				}
			} break;
			case FireSpot::HOLD: {  // every spot dangerous, here is safe: stay, fight what comes into range
				claim.pos = firePos;
				fireSpots.erase(unit);
				if (isHoldAttack && (firePos.SqDistance2D(tPos) <= SQUARE(order.range))) {
					unit->AttackFrom(firePos, target, targetTile, timeout);  // in range from here; stays here after
				} else {
					unit->HoldFire(firePos, target, targetTile, timeout);
				}
			} break;
			default: {  // no usable range (or melee spots off)
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

bool IFighterTask::AttackMeleeOther(std::vector<SFireClaim>& claims, size_t self, float safeThreat, CEnemyInfo* enemy,
		int timeout)
{
	SFireClaim& claim = claims[self];
	CCircuitUnit* unit = claim.unit;
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	const int tile = circuit->GetInflMap()->Pos2Index(enemy->GetPos());
	// Its firing spot next to that enemy: FindFireSpot and the helpers it calls read 'target', so it is swapped in
	// for the search only (no binding: nothing outside this call sees it). No group arc: that is the target's side
	CEnemyInfo* groupTarget = target;
	target = enemy;
	AIFloat3 firePos;
	const FireSpot spot = FindFireSpot(claims, self, safeThreat, nullptr, true, firePos);
	target = groupTarget;
	if (FireSpot::SPOT != spot) {
		// No safe spot next to it (or none it can stand on): not a valid target for now (AssignMeleeTargets skips
		// it for a while, so the unit isn't re-ordered at it every update)
		meleeAltFails[unit] = {enemy, frame};
		return false;
	}
	claim.pos = firePos;
	fireSpots[unit] = {firePos, frame, false};
	if (isFireSpotChase) {
		unit->AttackFrom(firePos, enemy, tile, timeout);
	} else {
		unit->FightFrom(firePos, enemy, tile, timeout);
	}
	return true;
}

bool IFighterTask::IsOutOfReach(CCircuitUnit* unit, int frame) const
{
	return (fireSpotReach > 0.f)
		&& (unit->GetPos(frame).SqDistance2D(target->GetPos()) > SQUARE(GetFireRange(unit) * fireSpotReach));
}

float IFighterTask::GetFireRange(CCircuitUnit* unit, const CEnemyInfo* enemy) const
{
	const AIFloat3& tPos = enemy->GetPos();
	CCircuitDef* edef = enemy->GetCircuitDef();
	const CCircuitDef::RangeType rt = ((edef != nullptr) && edef->IsAbleToFly()) ? CCircuitDef::RangeType::AIR
			: (tPos.y < -SQUARE_SIZE * 5) ? CCircuitDef::RangeType::WATER : CCircuitDef::RangeType::LAND;
	return unit->GetCircuitDef()->GetMaxRange(rt);
}

float IFighterTask::GetSpotRange(CCircuitUnit* unit, const CEnemyInfo* enemy) const
{
	const float range = GetFireRange(unit, enemy);
	CCircuitDef* cdef = unit->GetCircuitDef();
	if (!IsCloseSpot(unit, enemy)) {
		return range;  // ranged, or melee on a blast: a normal firing spot
	}
	const float gap = manager->GetCircuit()->GetMilitaryManager()->GetAttackMeleeSpot();
	if (gap < 0.f) {
		return 0.f;  // off: no usable range, direct Attack as before
	}
	// Right next to the target: both bodies plus the gap, never beyond the weapon's reach
	CCircuitDef* edef = enemy->GetCircuitDef();
	const float bodies = cdef->GetRadius() + ((edef != nullptr) ? edef->GetRadius() : 0.f);
	return std::min(range, bodies + gap);
}

bool IFighterTask::IsCloseSpot(CCircuitUnit* unit, const CEnemyInfo* enemy) const
{
	return unit->GetCircuitDef()->IsAttrMelee() && !IsMeleeBlast(unit, enemy);
}

bool IFighterTask::IsMeleeBlast(CCircuitUnit* unit, const CEnemyInfo* enemy) const
{
	const float share = manager->GetCircuit()->GetMilitaryManager()->GetAttackMeleeBlast();
	CCircuitDef* cdef = unit->GetCircuitDef();
	CCircuitDef* edef = enemy->GetCircuitDef();
	return (share >= 0.f) && (edef != nullptr) && !cdef->IsDeathBomb() && (edef->GetDeathAoe() > 0.f)
		&& (edef->GetDeathDamage() > 0.f) && (edef->GetDeathDamage() >= share * cdef->GetHealth());
}

int IFighterTask::GetMeleeCap(CCircuitUnit* unit, const CEnemyInfo* enemy) const
{
	const int maxCount = manager->GetCircuit()->GetMilitaryManager()->GetAttackMeleeMax();
	if (maxCount <= 0) {
		return std::numeric_limits<int>::max();
	}
	// Half the close-spot ring (the side facing us, see FindFireSpot), one body plus a little apart
	const float ring = GetSpotRange(unit, enemy) * FIRE_RADIUS;
	const float slot = unit->GetCircuitDef()->GetRadius() * 2.f + SQUARE_SIZE * 2;
	const int room = static_cast<int>(static_cast<float>(M_PI) * ring / std::max(slot, 1.f));
	return std::max(1, std::min(maxCount, room));
}

void IFighterTask::AssignMeleeTargets(const std::vector<SFireClaim>& claims, int frame,
		std::map<CCircuitUnit*, CEnemyInfo*>& outTargets) const
{
	CCircuitAI* circuit = manager->GetCircuit();
	CMilitaryManager* militaryMgr = circuit->GetMilitaryManager();
	if (militaryMgr->GetAttackMeleeMax() <= 0) {
		return;  // off: every melee unit on the group's target
	}
	// Melee units Attack may order, with close spots (not on a blast: those fire from a normal spot), nearest the
	// target first; already on it: a head start, so
	// they keep it rather than swap with one that walked a little closer
	const AIFloat3& tPos = target->GetPos();
	struct SMelee {
		CCircuitUnit* unit;
		float dist;
	};
	std::vector<SMelee> melees;
	for (const SFireClaim& c : claims) {
		CCircuitUnit* unit = c.unit;
		if (!IsCloseSpot(unit, target) || (GetSpotRange(unit) < SQUARE_SIZE * 4)
			|| (unit->Blocker() != nullptr) || IsScooting(unit) || IsOutOfReach(unit, frame))
		{
			continue;
		}
		const float dist = unit->GetPos(frame).distance2D(tPos) - ((unit->GetTarget() == target) ? MELEE_STICK : 0.f);
		melees.push_back({unit, dist});
	}
	if (melees.empty()) {
		return;
	}
	std::stable_sort(melees.begin(), melees.end(), [](const SMelee& a, const SMelee& b) { return a.dist < b.dist; });

	// Other targets: enemies in sight (known type, real position) near the group's target, on the ground
	std::vector<CEnemyInfo*> others;
	const float spread = militaryMgr->GetAttackMeleeSpread();
	bool isOthersRead = false;
	auto readOthers = [&]() {
		isOthersRead = true;
		if (spread <= 0.f) {
			return;
		}
		for (auto& kv : circuit->GetEnemyInfos()) {
			CEnemyInfo* enemy = kv.second;
			CCircuitDef* edef = enemy->GetCircuitDef();
			const AIFloat3& ePos = enemy->GetPos();
			if ((enemy == target) || !enemy->IsInLOS() || enemy->IsHidden() || (edef == nullptr) || edef->IsAbleToFly()
				|| (ePos.y < -SQUARE_SIZE * 5) || (ePos.SqDistance2D(tPos) > SQUARE(spread)))
			{
				continue;
			}
			others.push_back(enemy);
		}
	};

	std::map<const CEnemyInfo*, int> load;  // melee units on each enemy
	for (const SMelee& m : melees) {
		CCircuitUnit* unit = m.unit;
		if (load[target] < GetMeleeCap(unit, target)) {
			++load[target];
			continue;  // the group's target
		}
		if (!isOthersRead) {
			readOthers();
		}
		auto fit = meleeAltFails.find(unit);
		const CEnemyInfo* failed = ((fit != meleeAltFails.end()) && (frame < fit->second.second + ALT_FAIL_SKIP))
				? fit->second.first : nullptr;
		const AIFloat3& uPos = unit->GetPos(frame);
		const int targetCat = unit->GetCircuitDef()->GetTargetCategory();
		CEnemyInfo* best = nullptr;
		float bestDist = std::numeric_limits<float>::max();
		for (CEnemyInfo* enemy : others) {
			const AIFloat3& ePos = enemy->GetPos();
			if ((enemy == failed) || !(targetCat & enemy->GetCircuitDef()->GetCategory()) || IsMeleeBlast(unit, enemy)
				|| (load[enemy] >= GetMeleeCap(unit, enemy)))
			{
				continue;  // no safe spot there lately, not one it can hit, a blast (no melee there), or full
			}
			const float dist = ePos.distance2D(uPos) - ((unit->GetTarget() == enemy) ? MELEE_STICK : 0.f);
			if ((dist < bestDist) && circuit->GetTerrainManager()->CanMoveToPos(unit->GetArea(), ePos)) {
				bestDist = dist;
				best = enemy;
			}
		}
		if (best != nullptr) {
			++load[best];
			outTargets[unit] = best;
		}  // else: no other valid target around, the group's target over its cap as before
	}
}

bool IFighterTask::IsInFireBand(CCircuitUnit* unit, const AIFloat3& pos, bool isClose) const
{
	const float range = isClose ? GetSpotRange(unit) : GetFireRange(unit);
	// Close (melee): no lower bound, closer is fine
	const float bandMin = isClose ? 0.f : FIRE_BAND_MIN;
	const float sqDist = pos.SqDistance2D(target->GetPos());
	return (sqDist >= SQUARE(range * bandMin)) && (sqDist <= SQUARE(range * FIRE_BAND_MAX));
}

bool IFighterTask::GetKeptFireSpot(CCircuitUnit* unit, bool isClose, AIFloat3& outPos) const
{
	auto it = fireSpots.find(unit);
	if ((it == fireSpots.end())
		|| (manager->GetCircuit()->GetLastFrame() > it->second.frame + SPOT_KEEP)
		|| !IsInFireBand(unit, it->second.pos, isClose))
	{
		return false;
	}
	outPos = it->second.pos;
	return true;
}

bool IFighterTask::IsOnFreshSpot(CCircuitUnit* unit, int frame, bool isRiskyOnly) const
{
	auto it = fireSpots.find(unit);
	return (it != fireSpots.end()) && (!isRiskyOnly || it->second.isRisky) && (frame < it->second.frame + RISKY_GRACE);
}

IFighterTask::FireSpot IFighterTask::FindFireSpot(const std::vector<SFireClaim>& claims, size_t self, float safeThreat,
		const float* arcBearing, bool isClose, AIFloat3& outPos) const
{
	CCircuitUnit* unit = claims[self].unit;
	const float range = isClose ? GetSpotRange(unit) : GetFireRange(unit);  // close: right next to the target
	if (range < SQUARE_SIZE * 4) {
		return FireSpot::NONE;
	}
	CCircuitAI* circuit = manager->GetCircuit();
	const AIFloat3& tPos = target->GetPos();
	const AIFloat3& uPos = unit->GetPos(circuit->GetLastFrame());
	CThreatMap* threatMap = circuit->GetThreatMap();
	const float strength = std::max(claims[self].power, THREAT_MIN);
	const float wThreat = circuit->GetMilitaryManager()->GetAttackSpotThreat();
	const float step = threatMap->GetSquareSize();
	const bool hasDangerLine = (safeThreat > 0.f);

	// Local support: the unit stands alone at its spot, so a spot is judged against the strength of our units
	// heading to within SUPPORT_RADIUS of it (itself included), not the whole group's; never more than that
	// The group size bonus (GetCountPowerMod) counts only those units
	auto supportAt = [&](const AIFloat3& p) {
		float s = strength;
		size_t count = 1;
		for (size_t i = 0; i < claims.size(); ++i) {
			if ((i != self) && (claims[i].pos.SqDistance2D(p) < SQUARE(SUPPORT_RADIUS))) {
				s += claims[i].power;
				++count;
			}
		}
		return std::min(s * powerMod * GetCountPowerMod(count), safeThreat);
	};
	// Danger = what the task's escape calls dangerous (same allowance as its IsInDanger), for the local support.
	// The allowance is always above 0, so where there is no threat there is no danger (no need to work it out)
	// share < 1: the margin's stricter line (quota.danger_margin), which a SPOT is looked for under first
	auto isDanger = [&](const AIFloat3& p, float threat, float share = 1.f) {
		return hasDangerLine && (threat > 0.f)
				&& (threat >= share * GetAllowedThreatFor(p, supportAt(p),
						(share > 0.f) ? threat / share : std::numeric_limits<float>::max()));
	};
	const float startThreat = GetTaskThreatAt(unit, uPos);
	const bool isSafeHere = hasDangerLine && !isDanger(uPos, startThreat);

	// Straight walk from the unit to p: is it dangerous, and its worst threat. A unit already in danger may
	// step out: the danger it starts in is ignored until the line leaves it, unless it gets hotter than here.
	// share < 1: judged against the margin's line from the start, so a unit in the margin band may step out of it too
	auto checkWalk = [&](const AIFloat3& p, float& outPeak, float share = 1.f) {
		outPeak = 0.f;
		AIFloat3 dir = p - uPos;
		dir.y = 0.f;
		const float dist = dir.Length2D();
		if (!hasDangerLine || (dist <= step)) {
			return false;
		}
		dir /= dist;
		bool isLeft = !isDanger(uPos, startThreat, share);
		bool isWalkDanger = false;
		for (float d = step; d < dist; d += step) {
			AIFloat3 q = uPos + dir * d;
			CTerrainManager::CorrectPosition(q);
			const float qThreat = GetTaskThreatAt(unit, q);
			if (!isDanger(q, qThreat, share)) {
				isLeft = true;
			} else if (isLeft || (qThreat > startThreat)) {
				isWalkDanger = true;
				outPeak = std::max(outPeak, qThreat);
			}
		}
		return isWalkDanger;
	};

	// Candidate spots. Cost: threat (the worse of spot and walk), crowding by other claims, walking distance
	struct SCandidate {
		AIFloat3 pos;
		float threat;
		int crowd;  // other claims within crowdRadius
		float dist;  // walking distance / range
		float walkPeak = -1.f;  // worst threat of a dangerous walk there, 0: safe walk; < 0: not walked yet
		float cost = 0.f;  // with a safe walk
	};
	auto costOf = [&](const SCandidate& c, float worst) {
		return wThreat * (worst / strength + CROWD_WEIGHT * c.crowd) + c.dist;
	};
	// With lifts (GetUnitSightLifts), only spots with a line of fire to the target, judged from the ground there:
	// stricter than the pick's (sectors' highest points), so a unit at its spot never reads the target as out of sight.
	// And only spots it walks to directly: one up on a plateau seen from below would be reached the long way round, on
	// the engine's path, not a route of ours. Blind or round the long way, the target needs a route (FindTarget).
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	float aimLift, targetLift;
	const bool isSight = GetUnitSightLifts(unit, target, aimLift, targetLift);
	std::vector<SCandidate> cands;
	// Melee close spots sit on a small ring: crowding by a body's width (else every spot on it crowds every other),
	// and a wider arc, so units on one target spread round it rather than queue up on one side
	const float crowdR = isClose ? std::min(crowdRadius, unit->GetCircuitDef()->GetRadius() * 2.f + SQUARE_SIZE * 2)
			: crowdRadius;
	const int arcSteps = isClose ? ARC_STEPS_CLOSE : ARC_STEPS;
	const float arcSpan = isClose ? ARC_SPAN_CLOSE : ARC_SPAN;
	auto add = [&](const AIFloat3& p) {
		if ((isSight && (!terrainMgr->IsLineClear(p, tPos, aimLift, targetLift, true)
			|| !terrainMgr->IsWalkClear(unit->GetArea(), uPos, p, WALK_CLIMB))) || !IsFireSpotAllowed(p))
		{
			return;
		}
		int crowd = 0;
		for (size_t i = 0; i < claims.size(); ++i) {
			if ((i != self) && (claims[i].pos.SqDistance2D(p) < SQUARE(crowdR))) {
				++crowd;
			}
		}
		SCandidate c{p, GetTaskThreatAt(unit, p), crowd, p.distance2D(uPos) / range};
		c.cost = costOf(c, std::max(c.threat, 0.f));
		cands.push_back(c);
	};
	// On the group's planned arc (arcBearing), if it has one
	auto isOnArc = [&](const AIFloat3& p) {
		const AIFloat3 d = p - tPos;
		return (arcBearing == nullptr) || (d.Length2D() < 1.f)
			|| (utils::angle_diff(atan2f(d.z, d.x), *arcBearing) <= static_cast<float>(M_PI) * arcSpan + 1e-3f);
	};
	// Its last spot, and where it stands, while in the fire band (and on the planned arc): the distance term
	// favours them
	AIFloat3 keptPos;
	if (GetKeptFireSpot(unit, isClose, keptPos) && isOnArc(keptPos)) {
		add(keptPos);
	}
	const size_t keptCount = cands.size();  // its last spot: no margin (see SPOT)
	if (IsInFireBand(unit, uPos, isClose) && isOnArc(uPos)) {
		add(uPos);
	}
	// Spots on an arc around the target, facing us or the planned side (+-45 degrees: keeps the group together;
	// melee close spots +-90, see crowdR)
	AIFloat3 dir = uPos - tPos;
	const float bearing = (arcBearing != nullptr) ? *arcBearing : (dir.Length2D() > 1.f) ? atan2f(dir.z, dir.x) : 0.f;
	const float radius = range * FIRE_RADIUS;
	for (int i = -arcSteps; i <= arcSteps; ++i) {
		const float angle = bearing + i * static_cast<float>(M_PI) * arcSpan / arcSteps;
		AIFloat3 p(tPos.x + radius * cosf(angle), tPos.y, tPos.z + radius * sinf(angle));
		CTerrainManager::CorrectPosition(p);
		if (terrainMgr->CanMoveToPos(unit->GetArea(), p)) {
			add(p);
		}
	}

	// SPOT: the cheapest spot with a safe walk there (ties: the earlier candidate). The walk is the costly check,
	// so spots are tried cheapest first and only until one passes. First with a margin under the danger line
	// (quota.danger_margin): a spot just under it tips over as soon as the unit takes a few hits or a turret is seen.
	// Its last spot passes at the line, so threat wavering around the margin doesn't shuttle it between two spots.
	// Then, if none passed, at the line itself as before
	std::vector<size_t> order(cands.size());
	std::iota(order.begin(), order.end(), 0);
	std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return cands[a].cost < cands[b].cost; });
	const float margin = circuit->GetMilitaryManager()->GetDangerMargin();
	if (margin < 1.f) {
		float peak;
		for (size_t i : order) {
			SCandidate& c = cands[i];
			const float share = (i < keptCount) ? 1.f : margin;
			if (!isDanger(c.pos, c.threat, share) && !checkWalk(c.pos, peak, share)) {
				outPos = c.pos;
				return FireSpot::SPOT;
			}
		}
	}
	for (size_t i : order) {
		SCandidate& c = cands[i];
		if (!isDanger(c.pos, c.threat) && !checkWalk(c.pos, c.walkPeak)) {
			outPos = c.pos;
			return FireSpot::SPOT;
		}
	}
	// HOLD: every spot is dangerous, here isn't
	if (isSafeHere) {
		outPos = uPos;
		return FireSpot::HOLD;
	}
	// RISKY: the least dangerous spot, judged by the worse of walk and spot
	const SCandidate* risky = nullptr;
	float riskyCost = std::numeric_limits<float>::max();
	for (SCandidate& c : cands) {
		if (c.walkPeak < 0.f) {
			checkWalk(c.pos, c.walkPeak);
		}
		const float cost = costOf(c, std::max(c.threat, c.walkPeak));
		if (cost < riskyCost) {
			riskyCost = cost;
			risky = &c;
		}
	}
	if (risky != nullptr) {
		outPos = risky->pos;
		return FireSpot::RISKY;
	}
	// No spot in sight of the target that we walk to directly (or none we can stand on): hold rather than a direct
	// Attack, which the engine would turn into a walk round to wherever it can fire from. isHoldAttack (anti-heavy):
	// also with no lifts, so it never walks in to a spot it doesn't allow
	if (isSight || isHoldAttack) {
		outPos = uPos;
		return FireSpot::HOLD;
	}
	return FireSpot::NONE;
}

bool IFighterTask::IsAllHolding() const
{
	// HOLD (IFighterTask::Attack): unit with a spot range (melee too) ordered on this target without a firing spot
	CCircuitAI* circuit = manager->GetCircuit();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	const int frame = circuit->GetLastFrame();
	const AIFloat3& tPos = target->GetPos();
	int ranged = 0;
	for (CCircuitUnit* unit : units) {
		if ((GetSpotRange(unit) < SQUARE_SIZE * 4) || (unit->Blocker() != nullptr) || IsOutOfReach(unit, frame)) {
			continue;  // no spot (direct Attack), busy, or a straggler: Attack gives it no spot
		}
		++ranged;
		if ((unit->GetTarget() != target) || (fireSpots.find(unit) != fireSpots.end()) || IsScooting(unit)) {
			return false;  // has a spot, or steps back as it reloads (it fires)
		}
		// Can already shoot it: weapon range, not spot range (a melee unit with a real gun, Reaver, shoots from well
		// outside its spot), and in sight as firing spots must be (else it holds at a cliff foot, within range)
		const AIFloat3& pos = unit->GetPos(frame);
		float aimLift, targetLift;
		if ((pos.SqDistance2D(tPos) <= SQUARE(GetFireRange(unit)))
			&& (!GetUnitSightLifts(unit, target, aimLift, targetLift)
				|| terrainMgr->IsLineClear(pos, tPos, aimLift, targetLift, true)))
		{
			return false;
		}
	}
	return ranged > 0;
}

bool IFighterTask::GetUnitSightLifts(CCircuitUnit* unit, const CEnemyInfo* enemy, float& outAimLift,
		float& outTargetLift) const
{
	return !unit->GetCircuitDef()->IsHighTrajectory() && GetSightLifts(enemy, outAimLift, outTargetLift);
}

const std::vector<IFighterTask::SUnitStrength>& IFighterTask::GetUnitStrengths() const
{
	// Still valid: read this frame, for the same units
	const int frame = manager->GetCircuit()->GetLastFrame();
	bool isValid = (strengthFrame == frame) && (unitStrengths.size() == units.size());
	auto it = units.begin();
	for (size_t i = 0; isValid && (i < unitStrengths.size()); ++i, ++it) {
		isValid = (unitStrengths[i].unit == *it);
	}
	if (!isValid) {
		strengthFrame = frame;
		strengthSum = 0.f;
		unitStrengths.clear();
		const std::vector<float> shieldShares = GetShieldShares(frame);
		size_t i = 0;
		for (CCircuitUnit* unit : units) {
			CCircuitDef* cdef = unit->GetCircuitDef();
			// Def power has the unit's own full shield; with shields in the group, its share of theirs instead
			const float basePower = shieldShares.empty() ? cdef->GetPower()
					: cdef->GetPwrDamage() * sqrtf(std::max(cdef->GetHealth() + shieldShares[i] * SHIELD_MOD, 0.f));
			const float power = basePower * sqrtf(std::max(unit->GetHealthPercent(), 0.f));
			unitStrengths.push_back({unit, power});
			strengthSum += power;
			++i;
		}
	}
	return unitStrengths;
}

std::vector<float> IFighterTask::GetShieldShares(int frame) const
{
	// Each group shield's full charge shared by the group's armed units inside it, as CThreatMap::AddShields shares
	// an enemy shield: an area shield (Aspis, unarmed) counts for the units it covers, and once. Full, not current:
	// Felon spends its shield to fire, and a strength that dips with every shot would flip the escape; shields
	// below retreat.shield leave the group (Update). Empty: no shields in the group
	std::vector<float> shares;
	if (shields.empty()) {
		return shares;
	}
	shares.resize(units.size(), 0.f);
	std::vector<size_t> inside;
	for (CCircuitUnit* shield : shields) {
		const AIFloat3& sPos = shield->GetPos(frame);
		const float sqRadius = SQUARE(shield->GetCircuitDef()->GetShieldRadius());
		inside.clear();
		size_t i = 0;
		for (CCircuitUnit* unit : units) {
			if (unit->GetCircuitDef()->IsAttacker() && (unit->GetPos(frame).SqDistance2D(sPos) <= sqRadius)) {
				inside.push_back(i);
			}
			++i;
		}
		for (size_t j : inside) {
			shares[j] += shield->GetCircuitDef()->GetMaxShield() / inside.size();
		}
	}
	return shares;
}

float IFighterTask::GetStrengthSum() const
{
	GetUnitStrengths();
	return strengthSum;
}

float IFighterTask::GetCountPowerMod(size_t count) const
{
	return manager->GetCircuit()->GetThreatMap()->GetCountMod(count);
}

bool IFighterTask::IsSquadThreat(CCircuitUnit* unit)
{
	IUnitTask* task = unit->GetTask();
	return (task != nullptr) && (task->GetType() == IUnitTask::Type::FIGHTER)
			&& static_cast<IFighterTask*>(task)->IsSquadThreat();
}

bool IFighterTask::IsCloakRoute(CCircuitUnit* unit) const
{
	// As CPathFinder::FillMapData: cloak doesn't work under water (sonar), there it routes on the usual layer
	return ((unit->GetPos(manager->GetCircuit()->GetLastFrame()).y >= 0.f) || unit->GetCircuitDef()->IsSonarStealth())
		&& unit->GetUnit()->IsCloaked();
}

bool IFighterTask::IsCloakRouteUnit(CCircuitUnit* unit)
{
	IUnitTask* task = unit->GetTask();
	if ((task != nullptr) && (task->GetType() == IUnitTask::Type::FIGHTER)) {
		return static_cast<IFighterTask*>(task)->IsCloakRoute(unit);
	}
	return unit->GetUnit()->IsCloaked();  // no fighter task: CPathFinder checks water first
}

float IFighterTask::GetTaskThreatAt(CCircuitUnit* unit, const AIFloat3& pos) const
{
	CThreatMap* threatMap = manager->GetCircuit()->GetThreatMap();
	return IsSquadThreat() ? threatMap->GetSquadThreatAt(unit, pos) : threatMap->GetThreatAt(unit, pos);
}

float IFighterTask::GetTaskThreatExcl(CCircuitUnit* unit, const AIFloat3& pos, float exclThreat) const
{
	CThreatMap* threatMap = manager->GetCircuit()->GetThreatMap();
	return IsSquadThreat() ? threatMap->GetSquadThreatExcl(unit, pos, exclThreat)
			: std::max(0.f, threatMap->GetThreatAt(unit, pos) - exclThreat);
}

bool IFighterTask::IsDangerAt(CCircuitUnit* unit, const AIFloat3& p, float strength) const
{
	const float threat = GetTaskThreatAt(unit, p);
	return (threat > 0.f) && (threat >= GetAllowedThreatFor(p, strength, threat));
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

float IFighterTask::GetCoverPower(CAllyUnit* ally)
{
	// Power scaled by sqrt(health %), as squad strength is; nanoframes don't count.
	// The caller filters which units count (armed, not aircraft, not its own role)
	springai::Unit* u = ally->GetUnit();
	if (u->IsBeingBuilt()) {
		return 0.f;
	}
	const float maxHealth = u->GetMaxHealth();
	const float healthPerc = (maxHealth > 0.f) ? u->GetHealth() / maxHealth : 0.f;
	return ally->GetCircuitDef()->GetPower() * sqrtf(std::max(healthPerc, 0.f));  // ally strength: pwr_mod, not thr_mod
}

bool IFighterTask::IsStaticDefence(CCircuitDef* cdef)
{
	return (cdef->GetThreatRange(CCircuitDef::ThreatType::LAND) > 0)
			|| (cdef->GetThreatRange(CCircuitDef::ThreatType::WATER) > 0);
}

bool IFighterTask::IsGroundDanger(CEnemyInfo* enemy, bool isGunship, float& outRange)
{
	outRange = 0.f;
	if (enemy->IsHidden() || !enemy->IsInRadarOrLOS()
		|| enemy->IsBeingBuilt() || enemy->IsParalyzed() || enemy->IsDisarmed())
	{
		return false;
	}
	CCircuitDef* edef = enemy->GetCircuitDef();
	if (edef == nullptr) {
		return enemy->GetVel().SqLength2D() >= 0.01f;
	}
	return IsGroundDangerDef(edef, isGunship, outRange);
}

bool IFighterTask::IsGroundDangerDef(CCircuitDef* edef, bool isGunship, float& outRange)
{
	outRange = 0.f;
	// Anti-air only: has an air weapon and no ground or water one. Not "no ground/water weapon": mobile bombs
	// (imp, snitch, skuttle) have no weapon at all, only a death explosion, and must still count
	if (!edef->IsMobile() || !edef->IsAttacker() || (isGunship ? edef->IsPlane() : edef->IsAbleToFly())
		|| (edef->HasAntiAir() && !edef->HasAntiLand() && !edef->HasAntiWater()))
	{
		return false;  // static, unarmed, flying or anti-air only
	}
	outRange = std::max(edef->GetMaxRange(CCircuitDef::RangeType::LAND), edef->GetMaxRange(CCircuitDef::RangeType::WATER));
	return true;
}

const springai::AIFloat3* IFighterTask::PickFleeSpot(const springai::AIFloat3& pos, const springai::AIFloat3& dangerPos,
		const F3Vec& spots)
{
	const float sqDangerDist = pos.SqDistance2D(dangerPos);
	const springai::AIFloat3* nearestAway = nullptr;
	const springai::AIFloat3* furthest = nullptr;
	float minSqDist = std::numeric_limits<float>::max();
	float maxSqDangerDist = -1.f;
	for (const springai::AIFloat3& spot : spots) {
		const float sqSpotDanger = spot.SqDistance2D(dangerPos);
		if (sqSpotDanger > maxSqDangerDist) {
			maxSqDangerDist = sqSpotDanger;
			furthest = &spot;
		}
		const float sqDist = pos.SqDistance2D(spot);
		if ((sqSpotDanger > sqDangerDist) && (sqDist < minSqDist)) {
			minSqDist = sqDist;
			nearestAway = &spot;
		}
	}
	return (nearestAway != nullptr) ? nearestAway : furthest;
}

} // namespace circuit

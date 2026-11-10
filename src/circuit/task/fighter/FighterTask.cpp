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
	struct SOrder {
		size_t claim;  // index in claims
		float range;
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

		if (isRepeatAttack
			|| (unit->GetTarget() != target)
			|| ((unit->GetTargetTile() != targetTile) && isTileMoveOrder))
		{
			orders.push_back({i, GetSpotRange(unit)});
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
		const bool isMelee = unit->GetCircuitDef()->IsAttrMelee();
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
				unit->HoldFire(firePos, target, targetTile, timeout);
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

bool IFighterTask::IsOutOfReach(CCircuitUnit* unit, int frame) const
{
	return (fireSpotReach > 0.f)
		&& (unit->GetPos(frame).SqDistance2D(target->GetPos()) > SQUARE(GetFireRange(unit) * fireSpotReach));
}

float IFighterTask::GetFireRange(CCircuitUnit* unit) const
{
	const AIFloat3& tPos = target->GetPos();
	CCircuitDef* edef = target->GetCircuitDef();
	const CCircuitDef::RangeType rt = ((edef != nullptr) && edef->IsAbleToFly()) ? CCircuitDef::RangeType::AIR
			: (tPos.y < -SQUARE_SIZE * 5) ? CCircuitDef::RangeType::WATER : CCircuitDef::RangeType::LAND;
	return unit->GetCircuitDef()->GetMaxRange(rt);
}

float IFighterTask::GetSpotRange(CCircuitUnit* unit) const
{
	const float range = GetFireRange(unit);
	CCircuitDef* cdef = unit->GetCircuitDef();
	if (!cdef->IsAttrMelee()) {
		return range;
	}
	const float gap = manager->GetCircuit()->GetMilitaryManager()->GetAttackMeleeSpot();
	if (gap < 0.f) {
		return 0.f;  // off: no usable range, direct Attack as before
	}
	// Right next to the target: both bodies plus the gap, never beyond the weapon's reach
	CCircuitDef* edef = target->GetCircuitDef();
	const float bodies = cdef->GetRadius() + ((edef != nullptr) ? edef->GetRadius() : 0.f);
	return std::min(range, bodies + gap);
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

bool IFighterTask::IsOnRiskySpot(CCircuitUnit* unit, int frame) const
{
	auto it = fireSpots.find(unit);
	return (it != fireSpots.end()) && it->second.isRisky && (frame < it->second.frame + RISKY_GRACE);
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
	auto isDanger = [&](const AIFloat3& p, float threat) {
		return hasDangerLine && (threat > 0.f) && (threat >= GetAllowedThreatAt(p, supportAt(p)));
	};
	const float startThreat = GetTaskThreatAt(unit, uPos);
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
			const float qThreat = GetTaskThreatAt(unit, q);
			if (!isDanger(q, qThreat)) {
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
		int crowd;  // other claims within CROWD_RADIUS
		float dist;  // walking distance / range
		float walkPeak = -1.f;  // worst threat of a dangerous walk there, 0: safe walk; < 0: not walked yet
		float cost = 0.f;  // with a safe walk
	};
	auto costOf = [&](const SCandidate& c, float worst) {
		return wThreat * (worst / strength + CROWD_WEIGHT * c.crowd) + c.dist;
	};
	std::vector<SCandidate> cands;
	auto add = [&](const AIFloat3& p) {
		int crowd = 0;
		for (size_t i = 0; i < claims.size(); ++i) {
			if ((i != self) && (claims[i].pos.SqDistance2D(p) < SQUARE(CROWD_RADIUS))) {
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
			|| (utils::angle_diff(atan2f(d.z, d.x), *arcBearing) <= static_cast<float>(M_PI) * ARC_SPAN + 1e-3f);
	};
	// Where it stands, and its last spot, while in the fire band (and on the planned arc): the distance term
	// favours them
	if (IsInFireBand(unit, uPos, isClose) && isOnArc(uPos)) {
		add(uPos);
	}
	AIFloat3 keptPos;
	if (GetKeptFireSpot(unit, isClose, keptPos) && isOnArc(keptPos)) {
		add(keptPos);
	}
	// Spots on an arc around the target, facing us or the planned side (+-45 degrees: keeps the group together)
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	AIFloat3 dir = uPos - tPos;
	const float bearing = (arcBearing != nullptr) ? *arcBearing : (dir.Length2D() > 1.f) ? atan2f(dir.z, dir.x) : 0.f;
	const float radius = range * FIRE_RADIUS;
	for (int i = -ARC_STEPS; i <= ARC_STEPS; ++i) {
		const float angle = bearing + i * static_cast<float>(M_PI) * ARC_SPAN / ARC_STEPS;
		AIFloat3 p(tPos.x + radius * cosf(angle), tPos.y, tPos.z + radius * sinf(angle));
		CTerrainManager::CorrectPosition(p);
		if (terrainMgr->CanMoveToPos(unit->GetArea(), p)) {
			add(p);
		}
	}

	// SPOT: the cheapest spot with a safe walk there (ties: the earlier candidate). The walk is the costly check,
	// so spots are tried cheapest first and only until one passes
	std::vector<size_t> order(cands.size());
	std::iota(order.begin(), order.end(), 0);
	std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return cands[a].cost < cands[b].cost; });
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
	return FireSpot::NONE;
}

bool IFighterTask::IsAllHolding() const
{
	// HOLD (IFighterTask::Attack): unit with a spot range (melee too) ordered on this target without a firing spot
	const int frame = manager->GetCircuit()->GetLastFrame();
	int ranged = 0;
	for (CCircuitUnit* unit : units) {
		if (GetSpotRange(unit) < SQUARE_SIZE * 4) {
			continue;  // direct Attack (no spot)
		}
		++ranged;
		// Weapon range, not spot range: a melee unit with a real gun (Reaver) shoots from well outside its spot
		if ((unit->GetTarget() != target) || (fireSpots.find(unit) != fireSpots.end())
			|| (unit->GetPos(frame).SqDistance2D(target->GetPos()) <= SQUARE(GetFireRange(unit))))
		{
			return false;  // has a spot, or can already shoot it
		}
	}
	return ranged > 0;
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
	return (threat > 0.f) && (threat >= GetAllowedThreatAt(p, strength));
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

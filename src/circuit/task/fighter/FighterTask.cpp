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

#include "Log.h"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "AISCommands.h"

namespace circuit {

using namespace springai;

namespace {
const float FIRE_RADIUS   = 0.9f;   // arc spots, x range
// Escorts (quota.escort, CAttackTask): an enemy with no spot in an escort's disc is skipped by its unit type this long;
// an escort with nothing to hit outside its disc steps back in to this share of the radius from the centre
const int   DISC_BAN      = FRAMES_PER_SEC * 5;
const float DISC_HOLD_IN  = 0.8f;
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
const float MELEE_STICK   = 100.f;  // a melee unit already on an enemy counts this much nearer it (AssignMeleeTargets)
const int   MELEE_BAN     = FRAMES_PER_SEC * 5;  // another target with no safe close spot is passed over this long
const float CROWD_CLOSE   = 1.f;    // melee close spots: cost per crowding claim, x close range of walk (~4 arc steps)
// Kiting (quota.kite): where a unit stands off a kited enemy, x range: the ring of spots on a kited target, and a step
// out of a disc (its disc keeps them past quota.kite x range, so a unit has 0.1x range to give before it is
// re-spotted). A step goes straight away from the enemy first, then KITE_ANGLE either side, then twice that
const float KITE_STAND    = 0.95f;
const float KITE_ANGLE    = static_cast<float>(M_PI) / 6.f;
// A walk out of a kite disc it starts in comes no nearer its centre than this x where it starts (one across the middle
// is no way out; a step at 2 KITE_ANGLE to the side dips about 0.93)
const float KITE_WALK_IN  = 0.9f;
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
	if (unit->HasShield()) {
		shields.erase(unit);
	}
}

void IFighterTask::OnUnitIdle(CCircuitUnit* unit)
{
	auto it = cowards.find(unit);
	if (it != cowards.end()) {
		cowards.erase(it);
		manager->AssignTask(unit, EnqueueRetreat());
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
	// Two separate reasons to retreat: health at or below "retreat" (or disarmed), and shield charge at or below
	// "retreat_shield". Neither one's threshold affects the other
	const bool isHurt = (healthPerc <= cdef->GetRetreat()) || unit->IsDisarmed(frame);
	if (!isHurt && !unit->IsShieldLow()) {
		if ((cdef->IsRoleHeavy() || cdef->IsAttrPrioRepair()) && (healthPerc < 0.9f)) {
			circuit->GetBuilderManager()->EnqueueRepair(IBuilderTask::Priority::NOW, unit);
		}
		return;
	}
	// Retreat now: stuck units workaround (low health: they don't shoot and don't see distant threat), no target
	// in sight, the target out of range, or the threat here over half its own. Else it retreats once idle
	CThreatMap* threatMap = circuit->GetThreatMap();
	const AIFloat3& pos = unit->GetPos(frame);
	if ((isHurt && (healthPerc < 0.2f))
		|| (target == nullptr) || !target->IsInLOS()
		|| (target->GetPos().SqDistance2D(pos) > SQUARE(cdef->GetMaxRange()))
		|| (threatMap->GetThreatAt(unit, pos) * 2 > threatMap->GetUnitThreat(unit)))
	{
		manager->AssignTask(unit, EnqueueRetreat());
		return;
	}
	cowards.insert(unit);
}

CRetreatTask* IFighterTask::EnqueueRetreat()
{
	return manager->GetCircuit()->GetMilitaryManager()->EnqueueRetreat();
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
	// A jumper in flight: where it lands (GetFightPos), so take-off moves the target's tile and re-orders at once
	const int targetTile = manager->GetCircuit()->GetInflMap()->Pos2Index(target->GetFightPos());
	const bool isRepeatAttack = (frame >= attackFrame + attackRepeat);
	attackFrame = isRepeatAttack ? frame : attackFrame;
	const AIFloat3& tPos = target->GetFightPos();
	GatherKited();
	const bool isTileMoveOrder = (retargetMove <= 0.f) || (tPos.SqDistance2D(lastOrderTargetPos) > SQUARE(retargetMove));
	// Every unit claims where it's heading: its last spot if still usable, else where it stands.
	// Longest range picks first; each pick replaces that unit's claim.
	std::vector<SFireClaim> claims;
	for (const SUnitStrength& s : GetUnitStrengths()) {
		AIFloat3 keptPos;
		const bool isClose = IsCloseSpot(s.unit, target);  // its spot may be a close one or a ranged one
		const bool isKept = GetKeptFireSpot(s.unit, isClose, target, keptPos)
				|| (isClose && GetKeptFireSpot(s.unit, false, target, keptPos));
		claims.push_back({s.unit, isKept ? keptPos : s.unit->GetPos(frame), s.power});
	}
	// Melee spread: past the target's cap, melee units go for other enemies near it
	for (auto it = meleeBans.begin(); it != meleeBans.end();) {
		it = (it->second <= frame) ? meleeBans.erase(it) : std::next(it);
	}
	std::map<CCircuitUnit*, CEnemyInfo*> meleeTargets;
	AssignMeleeTargets(claims, frame, meleeTargets);
	std::vector<SOrder> orders;  // units to order
	for (size_t i = 0; i < claims.size(); ++i) {
		CCircuitUnit* unit = claims[i].unit;
		if ((unit->Blocker() != nullptr) || IsScooting(unit)) {
			continue;  // Do not interrupt current action (or a reloading unit's step back)
		}
		if (IsOutOfReach(unit, frame) || IsEscortUnit(unit)) {
			continue;  // far behind: keep following the squad's path instead of a straight walk to a spot; escorts below
		}
		unit->GetTravelAct()->StateWait();

		auto mit = meleeTargets.find(unit);
		CEnemyInfo* enemy = (mit != meleeTargets.end()) ? mit->second : target;
		// Too near a kited enemy, or heading there: re-spotted now, not at the repeat
		if (isRepeatAttack
			|| (unit->GetTarget() != enemy)
			|| ((enemy == target) && (unit->GetTargetTile() != targetTile) && isTileMoveOrder)
			|| IsKiteRespot(unit, frame))
		{
			orders.push_back({i, GetSpotRange(unit, enemy), enemy});
		}
	}
	const int timeout = frame + FRAMES_PER_SEC * 60;
	OrderEscorts(claims, orders, frame, isRepeatAttack, timeout);
	if (!orders.empty()) {
		OrderOnTarget(claims, orders, frame, isDirect, GetEscapeSafeThreat(), timeout);
	}
	kited.clear();  // pointers: only good for this pass
}

void IFighterTask::GatherKited()
{
	kited.clear();
	CCircuitAI* circuit = manager->GetCircuit();
	if (circuit->GetMilitaryManager()->GetKiteRange() <= 0.f) {
		return;
	}
	for (const auto& kv : circuit->GetEnemyInfos()) {
		const CEnemyInfo* enemy = kv.second;
		const CCircuitDef* edef = enemy->GetCircuitDef();
		if ((edef != nullptr) && edef->IsAttrKite() && enemy->IsInLOS() && !enemy->IsHidden()
			&& !enemy->IsBeingBuilt() && !enemy->IsParalyzed() && !enemy->IsDisarmed())
		{
			kited.push_back(enemy);
		}
	}
}

void IFighterTask::GetKiteDiscs(CCircuitUnit* unit, std::vector<SKiteDisc>& outDiscs) const
{
	outDiscs.clear();
	if (kited.empty()) {
		return;
	}
	const CCircuitDef* cdef = unit->GetCircuitDef();
	if (cdef->IsAttrMelee() || cdef->IsAbleToFly() || !IsArmedUnit(cdef)) {
		return;  // melee closes in; aircraft and bombs don't kite
	}
	const float kite = manager->GetCircuit()->GetMilitaryManager()->GetKiteRange();
	for (const CEnemyInfo* enemy : kited) {
		const float range = GetFireRange(unit, enemy);  // 0: can't hit it (anti-air on a ground unit): no disc
		if (range * kite >= SQUARE_SIZE * 4) {
			outDiscs.push_back({enemy->GetFightPos(), range * kite, range * KITE_STAND});
		}
	}
}

const IFighterTask::SKiteDisc* IFighterTask::FindKiteDisc(const std::vector<SKiteDisc>& discs, const AIFloat3& p)
{
	// The one p is deepest in (share of its radius, squared)
	const SKiteDisc* best = nullptr;
	float bestShare = 1.f;
	for (const SKiteDisc& d : discs) {
		const float share = p.SqDistance2D(d.pos) / SQUARE(d.range);
		if (share < bestShare) {
			bestShare = share;
			best = &d;
		}
	}
	return best;
}

bool IFighterTask::IsKiteWalkClear(const std::vector<SKiteDisc>& discs, const AIFloat3& from, const AIFloat3& to)
{
	// The straight walk's nearest point to each disc's centre: outside the disc, or for one it starts in, no nearer
	// than KITE_WALK_IN x where it starts
	const AIFloat3 seg = to - from;
	const float sqLen = seg.SqLength2D();
	for (const SKiteDisc& d : discs) {
		const float t = (sqLen > 1.f) ? utils::clamp(((d.pos.x - from.x) * seg.x + (d.pos.z - from.z) * seg.z) / sqLen, 0.f, 1.f) : 0.f;
		const AIFloat3 closest(from.x + seg.x * t, 0.f, from.z + seg.z * t);
		const float sqFrom = from.SqDistance2D(d.pos);
		const float sqLimit = (sqFrom < SQUARE(d.range)) ? sqFrom * SQUARE(KITE_WALK_IN) : SQUARE(d.range);
		if (closest.SqDistance2D(d.pos) < sqLimit) {
			return false;
		}
	}
	return true;
}

bool IFighterTask::IsKiteRespot(CCircuitUnit* unit, int frame) const
{
	static std::vector<SKiteDisc> discs;  // NOTE: micro-opt
	GetKiteDiscs(unit, discs);
	if (discs.empty()) {
		return false;
	}
	// Where it is heading: the spot it was last sent to while fresh (a step out of a disc too), else where it stands
	const AIFloat3& uPos = unit->GetPos(frame);
	auto it = fireSpots.find(unit);
	const AIFloat3& spot = ((it != fireSpots.end()) && (frame <= it->second.frame + SPOT_KEEP)) ? it->second.pos : uPos;
	return (FindKiteDisc(discs, spot) != nullptr) || !IsKiteWalkClear(discs, uPos, spot);
}

bool IFighterTask::IsKited(const CEnemyInfo* enemy) const
{
	const CCircuitDef* edef = enemy->GetCircuitDef();
	return (edef != nullptr) && edef->IsAttrKite() && (manager->GetCircuit()->GetMilitaryManager()->GetKiteRange() > 0.f);
}

void IFighterTask::OrderOnTarget(std::vector<SFireClaim>& claims, std::vector<SOrder>& orders, int frame,
		bool isDirect, float safeThreat, int timeout)
{
	const int targetTile = manager->GetCircuit()->GetInflMap()->Pos2Index(target->GetFightPos());
	const AIFloat3& tPos = target->GetFightPos();
	if (std::any_of(orders.begin(), orders.end(), [](const SOrder& o) { return o.discRadius <= 0.f; })) {
		lastOrderTargetPos = tPos;  // units ordered on the group's target (escorts alone don't reset its drift)
	}
	std::stable_sort(orders.begin(), orders.end(), [](const SOrder& a, const SOrder& b) { return a.range > b.range; });
	float arcBearing;
	const float* arc = GetFireArcBearing(arcBearing) ? &arcBearing : nullptr;  // once for the whole group
	for (const SOrder& order : orders) {
		SFireClaim& claim = claims[order.claim];
		CCircuitUnit* unit = claim.unit;
		AIFloat3 firePos;
		if (order.discRadius > 0.f) {  // an escort
			if (!OrderInDisc(claims, order, safeThreat, timeout)) {
				HoldInDisc(claim, order.discRadius, frame, timeout);
			}
			continue;
		}
		if ((order.enemy != target) && AttackMeleeOther(claims, order.claim, safeThreat, order.enemy, timeout)) {
			continue;
		}  // else no safe spot by that enemy: the group's target after all
		// Melee: a close spot right next to the target, unless its death explosion is a blast for the unit, then a
		// normal firing spot within weapon range as ranged units pick
		const bool isMelee = IsCloseSpot(unit, target);
		FireSpot spot = FindFireSpot(claims, order.claim, safeThreat, arc, isMelee, target, firePos);
		// Melee with no safe spot next to the target: a safe spot within weapon range (the ranged search) before it
		// holds or walks in anyway. Units without a real gun find nothing new there (their range is as short)
		if (isMelee && ((FireSpot::HOLD == spot) || (FireSpot::RISKY == spot))) {
			AIFloat3 rangedPos;
			if (FindFireSpot(claims, order.claim, safeThreat, arc, false, target, rangedPos) == FireSpot::SPOT) {
				spot = FireSpot::SPOT;
				firePos = rangedPos;
			}
		}
		switch (spot) {
			case FireSpot::SPOT:
			case FireSpot::RISKY:  // RISKY: nowhere safe, not here either: the least dangerous spot
			case FireSpot::KITE: {  // KITE: too near a kited enemy: step out, fight from there (no chase back in)
				claim.pos = firePos;
				fireSpots[unit] = {firePos, frame, FireSpot::RISKY == spot};
				if (isFireSpotChase && (FireSpot::KITE != spot)) {
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
					unit->Attack(target->GetFightPos(), target, targetTile, timeout);
				}
			} break;
		}
	}
}

bool IFighterTask::IsArmedUnit(const CCircuitDef* cdef)
{
	return cdef->IsAttacker() && !cdef->IsDeathBomb();
}

bool IFighterTask::IsUnarmedSupport(const CCircuitDef* cdef)
{
	return cdef->IsRoleSupport() && !IsArmedUnit(cdef);
}

bool IFighterTask::IsEscortUnit(CCircuitUnit* unit) const
{
	return IsUnarmedSupport(unit->GetCircuitDef());
}

bool IFighterTask::IsDiscBanned(const CEnemyInfo* enemy, const CCircuitDef* cdef) const
{
	return spotDiscBans.find(std::make_pair(enemy, static_cast<int>(cdef->GetId()))) != spotDiscBans.end();
}

bool IFighterTask::IsDiscReach(CCircuitUnit* unit, const CEnemyInfo* enemy, float r) const
{
	const float range = GetFireRange(unit, enemy);
	return (range >= SQUARE_SIZE * 4) && (enemy->GetFightPos().SqDistance2D(spotDiscPos) <= SQUARE(r + range * FIRE_RADIUS));
}

bool IFighterTask::OrderInDisc(std::vector<SFireClaim>& claims, const SOrder& order, float safeThreat, int timeout)
{
	SFireClaim& claim = claims[order.claim];
	CCircuitUnit* unit = claim.unit;
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	for (int i = 0; i < 2; ++i) {
		CEnemyInfo* enemy = (i == 0) ? order.enemy : order.alt;
		if (enemy == nullptr) {
			continue;
		}
		// Spots on the arc from the enemy towards the disc's centre (not towards the unit, which may stand at its edge)
		const AIFloat3 toDisc = spotDiscPos - enemy->GetFightPos();
		const float bearing = (toDisc.SqLength2D() > 1.f) ? atan2f(toDisc.z, toDisc.x) : 0.f;
		isSpotDisc = true;
		spotDiscSqRadius = SQUARE((i == 0) ? order.discRadius : order.altRadius);
		AIFloat3 firePos;
		const FireSpot spot = FindFireSpot(claims, order.claim, safeThreat, &bearing, false, enemy, firePos);
		isSpotDisc = false;
		if ((FireSpot::SPOT != spot) && (FireSpot::KITE != spot)) {  // KITE: a step out of a kite disc, fought from too
			spotDiscBans[std::make_pair(enemy, static_cast<int>(unit->GetCircuitDef()->GetId()))] = frame + DISC_BAN;
			continue;
		}
		if (enemy != unit->GetTarget()) {
			CCircuitDef* edef = enemy->GetCircuitDef();
			circuit->LOG("ESCORT %s #%i on %s%s", unit->GetCircuitDef()->GetDef()->GetName(), unit->GetId(),
					(edef != nullptr) ? edef->GetDef()->GetName() : "blip", (enemy == target) ? " (group target)" : "");
		}
		claim.pos = firePos;
		fireSpots[unit] = {firePos, frame, false};
		unit->FightFrom(firePos, enemy, circuit->GetInflMap()->Pos2Index(enemy->GetFightPos()), timeout);
		return true;
	}
	return false;
}

void IFighterTask::HoldInDisc(SFireClaim& claim, float discRadius, int frame, int timeout)
{
	CCircuitUnit* unit = claim.unit;
	CCircuitAI* circuit = manager->GetCircuit();
	if (unit->GetTarget() != nullptr) {
		circuit->LOG("ESCORT %s #%i holds: nothing to hit from its disc", unit->GetCircuitDef()->GetDef()->GetName(),
				unit->GetId());
	}
	fireSpots.erase(unit);
	unit->ClearTarget();
	const AIFloat3& uPos = unit->GetPos(frame);
	const float dist = uPos.distance2D(spotDiscPos);
	if (dist <= discRadius) {
		claim.pos = uPos;
		unit->HoldFire(uPos, nullptr, -1, timeout);
		return;
	}
	AIFloat3 holdPos = spotDiscPos + (uPos - spotDiscPos) * (discRadius * DISC_HOLD_IN / dist);
	CTerrainManager::CorrectPosition(holdPos);
	claim.pos = holdPos;
	TRY_UNIT(circuit, unit,
		unit->CmdMoveTo(holdPos, UNIT_COMMAND_OPTION_RIGHT_MOUSE_KEY, timeout);
		unit->CmdWantedSpeed(NO_SPEED_LIMIT);
	)
}

bool IFighterTask::AttackMeleeOther(std::vector<SFireClaim>& claims, size_t self, float safeThreat, CEnemyInfo* enemy,
		int timeout)
{
	SFireClaim& claim = claims[self];
	CCircuitUnit* unit = claim.unit;
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	AIFloat3 firePos;
	// No group arc: that is the group target's side
	if (FindFireSpot(claims, self, safeThreat, nullptr, true, enemy, firePos) != FireSpot::SPOT) {
		meleeBans[enemy] = frame + MELEE_BAN;  // no safe spot by it: not a valid target for a while, for any of ours
		return false;
	}
	const int tile = circuit->GetInflMap()->Pos2Index(enemy->GetFightPos());
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
		&& (unit->GetPos(frame).SqDistance2D(target->GetFightPos()) > SQUARE(GetFireRange(unit) * fireSpotReach));
}

float IFighterTask::GetFireRange(CCircuitUnit* unit, const CEnemyInfo* enemy) const
{
	const AIFloat3& tPos = enemy->GetFightPos();  // a jumper in flight: by where it lands (water or not)
	CCircuitDef* edef = enemy->GetCircuitDef();
	const CCircuitDef::RangeType rt = ((edef != nullptr) && edef->IsAbleToFly()) ? CCircuitDef::RangeType::AIR
			: (tPos.y < -SQUARE_SIZE * 5) ? CCircuitDef::RangeType::WATER : CCircuitDef::RangeType::LAND;
	return unit->GetCircuitDef()->GetMaxRange(rt);
}

float IFighterTask::GetSpotRange(CCircuitUnit* unit, const CEnemyInfo* enemy) const
{
	if (!unit->GetCircuitDef()->IsAttrMelee()) {
		return GetFireRange(unit, enemy);
	}
	// Melee: a close spot, unless the target is a blast it can fire at from outside (IsCloseSpot), then a normal
	// one. Melee spots off: 0, no usable range (direct Attack, as before)
	const float closeRange = GetCloseRange(unit, enemy);
	return ((closeRange > 0.f) && !IsCloseSpot(unit, enemy)) ? GetFireRange(unit, enemy) : closeRange;
}

float IFighterTask::GetCloseRange(CCircuitUnit* unit, const CEnemyInfo* enemy) const
{
	const float gap = manager->GetCircuit()->GetMilitaryManager()->GetAttackMeleeSpot();
	if (gap < 0.f) {
		return 0.f;  // off: no usable range, direct Attack as before
	}
	// Right next to the target: both bodies plus the gap, never beyond the weapon's reach
	CCircuitDef* cdef = unit->GetCircuitDef();
	CCircuitDef* edef = enemy->GetCircuitDef();
	const float bodies = cdef->GetRadius() + ((edef != nullptr) ? edef->GetRadius() : 0.f);
	return std::min(GetFireRange(unit, enemy), bodies + gap);
}

bool IFighterTask::IsMeleeBlast(CCircuitUnit* unit, const CEnemyInfo* enemy) const
{
	const float share = manager->GetCircuit()->GetMilitaryManager()->GetAttackMeleeBlast();
	CCircuitDef* cdef = unit->GetCircuitDef();
	CCircuitDef* edef = enemy->GetCircuitDef();
	return (share >= 0.f) && (edef != nullptr) && !cdef->IsDeathBomb()
		&& (edef->GetDeathDamage() > 0.f) && (edef->GetDeathDamage() >= share * cdef->GetHealth())
		&& (edef->GetDeathAoe() > GetCloseRange(unit, enemy) * FIRE_RADIUS);  // reaches its close spots
}

bool IFighterTask::IsCloseSpot(CCircuitUnit* unit, const CEnemyInfo* enemy) const
{
	// A blast it can fire at from outside: a normal spot (on the ring at FIRE_RADIUS x its range, see FindFireSpot)
	return unit->GetCircuitDef()->IsAttrMelee() && (GetCloseRange(unit, enemy) > 0.f)
		&& !(IsMeleeBlast(unit, enemy)
			&& (GetFireRange(unit, enemy) * FIRE_RADIUS > enemy->GetCircuitDef()->GetDeathAoe()));
}

int IFighterTask::GetMeleeCap(CCircuitUnit* unit, const CEnemyInfo* enemy) const
{
	const int maxCount = manager->GetCircuit()->GetMilitaryManager()->GetAttackMeleeMax();
	// Bodies round the whole close-spot ring, a little apart
	const float ring = GetCloseRange(unit, enemy) * FIRE_RADIUS;
	const float slot = unit->GetCircuitDef()->GetRadius() * 2.f + SQUARE_SIZE * 2;
	const int room = static_cast<int>(2.f * static_cast<float>(M_PI) * ring / std::max(slot, 1.f));
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
	// Melee units Attack may order that close in on the target, nearest first; already on it: a head start, so they
	// keep it rather than swap with one that walked a little closer
	const AIFloat3& tPos = target->GetPos();
	struct SMelee {
		CCircuitUnit* unit;
		float dist;
	};
	std::vector<SMelee> melees;
	bool isAnyElsewhere = false;  // a unit on another target: it may keep it
	for (const SFireClaim& c : claims) {
		CCircuitUnit* unit = c.unit;
		if (!IsCloseSpot(unit, target) || (unit->Blocker() != nullptr) || IsScooting(unit) || IsOutOfReach(unit, frame)
			|| IsEscortUnit(unit))
		{
			continue;
		}
		const bool isOnTarget = (unit->GetTarget() == target);
		isAnyElsewhere |= !isOnTarget;
		melees.push_back({unit, unit->GetPos(frame).distance2D(tPos) - (isOnTarget ? MELEE_STICK : 0.f)});
	}
	if (melees.empty()
		|| (!isAnyElsewhere && (melees.size() <= static_cast<size_t>(GetMeleeCap(melees[0].unit, target)))))
	{
		return;  // all on the target, within its cap
	}
	std::stable_sort(melees.begin(), melees.end(), [](const SMelee& a, const SMelee& b) { return a.dist < b.dist; });

	// Other targets: enemies in sight (known type, real position) near the group's target, on the ground, not banned
	std::vector<CEnemyInfo*> others;
	const float spread = militaryMgr->GetAttackMeleeSpread();
	for (auto& kv : circuit->GetEnemyInfos()) {
		CEnemyInfo* enemy = kv.second;
		CCircuitDef* edef = enemy->GetCircuitDef();
		const AIFloat3& ePos = enemy->GetPos();
		if ((enemy != target) && enemy->IsInLOS() && !enemy->IsHidden() && (edef != nullptr) && !edef->IsAbleToFly()
			&& (ePos.y >= -SQUARE_SIZE * 5) && (ePos.SqDistance2D(tPos) <= SQUARE(spread))
			&& (meleeBans.find(enemy) == meleeBans.end()))
		{
			others.push_back(enemy);
		}
	}
	// May this unit close in on that other enemy: one it can hit and may chase, not a blast, with room left
	std::map<const CEnemyInfo*, int> load;  // melee units on each enemy
	auto isValid = [&](CCircuitUnit* unit, CEnemyInfo* enemy) {
		CCircuitDef* cdef = unit->GetCircuitDef();
		const int ecat = enemy->GetCircuitDef()->GetCategory();
		return ((cdef->GetTargetCategory() & ecat) != 0) && ((cdef->GetNoChaseCategory() & ecat) == 0)
			&& !IsMeleeBlast(unit, enemy) && (load[enemy] < GetMeleeCap(unit, enemy));
	};
	auto assign = [&](CCircuitUnit* unit, CEnemyInfo* enemy) {
		++load[enemy];
		outTargets[unit] = enemy;
	};

	// Engaged: a unit at its close spot by another enemy keeps it until it dies (no walking off a half-dead kill)
	std::vector<CCircuitUnit*> rest;
	for (const SMelee& m : melees) {
		CCircuitUnit* unit = m.unit;
		CEnemyInfo* cur = nullptr;
		if (unit->GetTarget() != target) {
			auto it = std::find(others.begin(), others.end(), unit->GetTarget());  // only live ones: compared, not read
			cur = (it != others.end()) ? *it : nullptr;
		}
		const auto spot = fireSpots.find(unit);
		const float atSpot = unit->GetCircuitDef()->GetRadius() * 2.f + SQUARE_SIZE * 4;
		if ((cur != nullptr) && (spot != fireSpots.end()) && isValid(unit, cur)
			&& (unit->GetPos(frame).SqDistance2D(spot->second.pos) < SQUARE(atSpot)))
		{
			assign(unit, cur);
		} else {
			rest.push_back(unit);
		}
	}
	// The rest: the group's target up to its cap, nearest first, then each the nearest valid other enemy
	for (CCircuitUnit* unit : rest) {
		if (load[target] < GetMeleeCap(unit, target)) {
			++load[target];
			continue;  // the group's target
		}
		const AIFloat3& uPos = unit->GetPos(frame);
		CEnemyInfo* best = nullptr;
		float bestDist = std::numeric_limits<float>::max();
		for (CEnemyInfo* enemy : others) {
			const float dist = enemy->GetPos().distance2D(uPos) - ((unit->GetTarget() == enemy) ? MELEE_STICK : 0.f);
			if ((dist < bestDist) && isValid(unit, enemy)
				&& circuit->GetTerrainManager()->CanMoveToPos(unit->GetArea(), enemy->GetPos()))
			{
				bestDist = dist;
				best = enemy;
			}
		}
		if (best != nullptr) {
			assign(unit, best);
		}  // else: no other valid target around, the group's target over its cap as before
	}
}

bool IFighterTask::IsInFireBand(CCircuitUnit* unit, const AIFloat3& pos, bool isClose, const CEnemyInfo* enemy) const
{
	const float range = isClose ? GetCloseRange(unit, enemy) : GetFireRange(unit, enemy);
	// Close (melee): no lower bound, closer is fine. Kited: up to the full range (the kite disc is its lower bound)
	const float bandMin = isClose ? 0.f : FIRE_BAND_MIN;
	const float bandMax = (!isClose && IsKited(enemy)) ? 1.f : FIRE_BAND_MAX;
	const float sqDist = pos.SqDistance2D(enemy->GetFightPos());
	return (sqDist >= SQUARE(range * bandMin)) && (sqDist <= SQUARE(range * bandMax));
}

bool IFighterTask::GetKeptFireSpot(CCircuitUnit* unit, bool isClose, const CEnemyInfo* enemy, AIFloat3& outPos) const
{
	auto it = fireSpots.find(unit);
	if ((it == fireSpots.end())
		|| (manager->GetCircuit()->GetLastFrame() > it->second.frame + SPOT_KEEP)
		|| !IsInFireBand(unit, it->second.pos, isClose, enemy))
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
		const float* arcBearing, bool isClose, const CEnemyInfo* enemy, AIFloat3& outPos) const
{
	CCircuitUnit* unit = claims[self].unit;
	const float range = isClose ? GetCloseRange(unit, enemy) : GetFireRange(unit, enemy);  // close: next to it
	if (range < SQUARE_SIZE * 4) {
		return FireSpot::NONE;
	}
	CCircuitAI* circuit = manager->GetCircuit();
	const AIFloat3& tPos = enemy->GetFightPos();  // a jumper in flight: where it lands
	const AIFloat3& uPos = unit->GetPos(circuit->GetLastFrame());
	CThreatMap* threatMap = circuit->GetThreatMap();
	// Kited enemies' keep-out discs: no spot inside one, whatever the target (melee close spots have none)
	std::vector<SKiteDisc> discs;
	if (!isClose) {
		GetKiteDiscs(unit, discs);
	}
	bool isKiteDropped = false;  // a candidate fell in a disc
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
		int crowd;  // other claims within crowdR
		float dist;  // walking distance / range
		float walkPeak = -1.f;  // worst threat of a dangerous walk there, 0: safe walk; < 0: not walked yet
		float cost = 0.f;  // with a safe walk
	};
	// Melee close spots: a crowding claim costs CROWD_CLOSE of walk on its own, not scaled by attack_spot_threat as
	// for ranged spots, whose walk is normalised by a much longer range (there it is worth only a few elmos here)
	const float crowdCost = isClose ? CROWD_CLOSE : wThreat * CROWD_WEIGHT;
	auto costOf = [&](const SCandidate& c, float worst) {
		return wThreat * worst / strength + crowdCost * c.crowd + c.dist;
	};
	// With lifts (GetUnitSightLifts), only spots with a line of fire to the target, judged from the ground there:
	// stricter than the pick's (sectors' highest points), so a unit at its spot never reads the target as out of sight.
	// And only spots it walks to directly: one up on a plateau seen from below would be reached the long way round, on
	// the engine's path, not a route of ours. Blind or round the long way, the target needs a route (FindTarget).
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	float aimLift, targetLift;
	const bool isSight = GetUnitSightLifts(unit, enemy, aimLift, targetLift);
	std::vector<SCandidate> cands;
	// Melee close spots sit on a small ring: crowding by a body's width (else every spot on it crowds every other),
	// and a wider arc, so units on one target spread round it rather than queue up on one side
	const float crowdR = isClose ? std::min(crowdRadius, unit->GetCircuitDef()->GetRadius() * 2.f) : crowdRadius;
	const int arcSteps = isClose ? ARC_STEPS_CLOSE : ARC_STEPS;
	const float arcSpan = isClose ? ARC_SPAN_CLOSE : ARC_SPAN;
	auto add = [&](const AIFloat3& p) {
		if ((isSpotDisc && (p.SqDistance2D(spotDiscPos) > spotDiscSqRadius))
			|| (isSight && (!terrainMgr->IsLineClear(p, tPos, aimLift, targetLift, true)
			|| !terrainMgr->IsWalkClear(unit->GetArea(), uPos, p, WALK_CLIMB))) || !IsFireSpotAllowed(p))
		{
			return;
		}
		if ((FindKiteDisc(discs, p) != nullptr) || !IsKiteWalkClear(discs, uPos, p)) {
			isKiteDropped = true;
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
	if (GetKeptFireSpot(unit, isClose, enemy, keptPos) && isOnArc(keptPos)) {
		add(keptPos);
	}
	const size_t keptCount = cands.size();  // its last spot: no margin (see SPOT)
	if (IsInFireBand(unit, uPos, isClose, enemy) && isOnArc(uPos)) {
		add(uPos);
	}
	// Spots on an arc around the target, facing us or the planned side (+-45 degrees: keeps the group together;
	// melee close spots +-90, see crowdR)
	AIFloat3 dir = uPos - tPos;
	const float bearing = (arcBearing != nullptr) ? *arcBearing : (dir.Length2D() > 1.f) ? atan2f(dir.z, dir.x) : 0.f;
	const float radius = range * ((!isClose && IsKited(enemy)) ? KITE_STAND : FIRE_RADIUS);
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
	// KITE: it stands too near a kited enemy and no spot is clear of them all: a step out, straight away from the one it
	// is deepest in first, then to either side. The first safe one (spot and walk); else, unless here is safe (HOLD),
	// the least dangerous one
	AIFloat3 stepPos;
	float stepWorst = std::numeric_limits<float>::max();  // < 0: safe; max: no step
	if (const SKiteDisc* inDisc = FindKiteDisc(discs, uPos)) {
		AIFloat3 away = uPos - inDisc->pos;
		if (away.SqLength2D() < 1.f) {
			away = uPos - tPos;  // right on it: away from the target
		}
		const float awayBearing = (away.SqLength2D() >= 1.f) ? atan2f(away.z, away.x) : 0.f;
		for (int k : {0, 1, -1, 2, -2}) {
			const float angle = awayBearing + k * KITE_ANGLE;
			AIFloat3 p(inDisc->pos.x + inDisc->step * cosf(angle), inDisc->pos.y, inDisc->pos.z + inDisc->step * sinf(angle));
			CTerrainManager::CorrectPosition(p);
			if (!terrainMgr->CanMoveToPos(unit->GetArea(), p) || !IsFireSpotAllowed(p)
				|| (FindKiteDisc(discs, p) != nullptr) || !IsKiteWalkClear(discs, uPos, p)
				|| !terrainMgr->IsWalkClear(unit->GetArea(), uPos, p, WALK_CLIMB))
			{
				continue;
			}
			const float threat = GetTaskThreatAt(unit, p);
			float peak;
			const bool isWalkDanger = checkWalk(p, peak);
			const float worst = (isWalkDanger || isDanger(p, threat)) ? std::max(threat, peak) : -1.f;
			if (worst < stepWorst) {
				stepWorst = worst;
				stepPos = p;
			}
			if (worst < 0.f) {
				break;
			}
		}
	}
	if (stepWorst < 0.f) {
		outPos = stepPos;
		return FireSpot::KITE;
	}
	// HOLD: every spot is dangerous, here isn't
	if (isSafeHere) {
		outPos = uPos;
		return FireSpot::HOLD;
	}
	if (stepWorst < std::numeric_limits<float>::max()) {
		outPos = stepPos;
		return FireSpot::KITE;
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
	// Kiting too: every spot fell in a kite disc and it stands outside them, so it holds rather than walk in
	if (isSight || isHoldAttack || isKiteDropped) {
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
	const AIFloat3& tPos = target->GetFightPos();
	int ranged = 0;
	for (CCircuitUnit* unit : units) {
		if ((GetSpotRange(unit) < SQUARE_SIZE * 4) || (unit->Blocker() != nullptr) || IsOutOfReach(unit, frame)
			|| IsEscortUnit(unit))
		{
			continue;  // no spot (direct Attack), busy, a straggler: Attack gives it no spot; escorts pick their own
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
	// at or below their retreat_shield retreat when hit (OnUnitDamaged). Empty: no shields in the group
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

bool IFighterTask::IsFighting() const
{
	// Engaged and some unit has the target within its own weapon range. ENGAGE alone starts at highestRange,
	// which includes LOS radius: far too wide to keep groups from merging.
	if ((State::ENGAGE != state) || (target == nullptr)) {
		return false;
	}
	const int frame = manager->GetCircuit()->GetLastFrame();
	const AIFloat3& tPos = target->GetPos();
	for (CCircuitUnit* unit : units) {
		if (unit->GetPos(frame).SqDistance2D(tPos) < SQUARE(unit->GetCircuitDef()->GetMaxRange())) {
			return true;
		}
	}
	return false;
}

} // namespace circuit

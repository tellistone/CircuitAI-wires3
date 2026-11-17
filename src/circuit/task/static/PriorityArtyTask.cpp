/*
 * PriorityArtyTask.cpp
 *
 *  Long-range static artillery (Big Bertha, Disco Rave Party) aimed by a fixed priority list,
 *  @see PriorityArtyTask.h
 */

#include "task/static/PriorityArtyTask.h"
#include "task/TaskManager.h"
#include "module/MilitaryManager.h"
#include "terrain/TerrainManager.h"
#include "unit/ally/AllyUnit.h"
#include "unit/enemy/EnemyUnit.h"
#include "unit/CircuitUnit.h"
#include "unit/CircuitDef.h"
#include "CircuitAI.h"

#include "spring/SpringCallback.h"

#include "AISCommands.h"
#include "Unit.h"
#include "Weapon.h"
#include "WeaponDef.h"
#include "WeaponMount.h"

#include <algorithm>
#include <climits>
#include <cmath>

namespace circuit {

using namespace springai;

#define UPDATE_INTERVAL	FRAMES_PER_SEC  // at most this often (shots, the lost checks)
#define CHECK_INTERVAL	(FRAMES_PER_SEC * 10)  // look for a better target this often
#define IDLE_INTERVAL	(FRAMES_PER_SEC * 3)  // no target: look again this often
#define FAIL_SKIP		(FRAMES_PER_SEC * 60)  // a failed target is skipped this long x 2^(failures - 1)
#define FAIL_SKIP_MAX_SHIFT	5  // ... up to 32 minutes
#define DRY_SHOTS		5  // shells landed at a target in LOS or radar since this gun last damaged anything
#define LAND_MARGIN		FRAMES_PER_SEC  // slack on a shell's estimated landing
#define BLIND_SHOTS_MIN	5  // shots at a target out of both: max(this, BLIND_KILL_MOD x shots to kill)
#define BLIND_KILL_MOD	4
#define FAR_TURN_SEC	10.f  // mobile units more than this many seconds of turning away rank last
#define SWITCH_VALUE	3.f  // same tier and finish: switch only for this many times the value
#define RANGE_MOD		0.97f  // aim within this much of the height-adjusted range
#define MUZZLE_LIFT		50.f  // elmos above the base the shell starts at (the gun's aimposoffset)
#define ARC_STEP		16.f  // elmos between arc samples
#define ARC_CLEARANCE	8.f  // blocked where the shell is less than this above the ground
#define ARC_STOP_SHORT	44.f  // last sample this far short of the target
#define FLY_LIFT		100.f  // unknown radar blip this far above the ground: flying
#define DIVE_DEPTH		(SQUARE_SIZE * 5.f)  // submerged deeper than this: out of reach without a water weapon

CPriorityArtyTask::CPriorityArtyTask(ITaskManager* mgr)
		: IFighterTask(mgr, IFighterTask::FightType::SUPER, 1.f)
		, tp(nullptr)
		, aimFrom(-RgtVector)
		, range(0.f)
		, speed(1050.f)
		, gravity(120.f)
		, heightMod(0.8f)
		, aoe(0.f)
		, splashRadius(0.f)
		, shellDamage(2000.f)
		, reloadFrames(1)
		, isAntiWater(false)
		, pickTier(STargetPriority::NO_TIER)
		, isTargetMobile(false)
		, isTargetHigh(false)
		, trajectory(-1)
		, flightFrames(0)
		, checkFrame(0)
		, noShotFrame(INT_MAX)
		, lastUpdateFrame(0)
		, dryShots(0)
		, heading(0.f)
		, pitch(0.f)
		, isReorder(false)
{
}

CPriorityArtyTask::~CPriorityArtyTask()
{
	for (SWeapon& w : weapons) {
		delete w.weapon;
	}
}

bool CPriorityArtyTask::CanAssignTo(CCircuitUnit* unit) const
{
	return false;
}

void CPriorityArtyTask::RemoveAssignee(CCircuitUnit* unit)
{
	IFighterTask::RemoveAssignee(unit);
	if (units.empty()) {
		manager->AbortTask(this);
	}
}

void CPriorityArtyTask::Start(CCircuitUnit* unit)
{
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	position = unit->GetPos(frame);
	tp = unit->GetCircuitDef()->GetTargetPriority();
	aimFrom = position;
	aimFrom.y += MUZZLE_LIFT;
	heading = 0.f;  // facing south (+z)
	TRY_UNIT(circuit, unit,
		ReadWeapons(unit);
		heading = 90.f * unit->GetUnit()->GetBuildingFacing();  // S, E, N, W
	)
	splashRadius = ((tp != nullptr) && (tp->splashRadius >= 0.f)) ? tp->splashRadius : aoe;
	pitch = 0.f;
	checkFrame = frame;
	noShotFrame = INT_MAX;
	lastUpdateFrame = frame - UPDATE_INTERVAL;
	dryShots = 0;
	isReorder = false;
}

void CPriorityArtyTask::ReadWeapons(CCircuitUnit* unit)
{
	CCircuitDef* cdef = unit->GetCircuitDef();
	range = cdef->GetMaxRange(CCircuitDef::RangeType::LAND);
	aoe = cdef->GetAoe();
	reloadFrames = std::max(cdef->GetReloadTime(), 1);
	if (cdef->GetShotDamage() > 0.f) {
		shellDamage = cdef->GetShotDamage();
	}
	isAntiWater = cdef->HasAntiWater();

	// Arc: the main weapon's
	WeaponMount* mount = cdef->GetWeaponMount();
	if (mount != nullptr) {
		WeaponDef* wd = mount->GetWeaponDef();
		const float projSpeed = wd->GetProjectileSpeed() * FRAMES_PER_SEC;  // elmo/frame
		if (projSpeed > 0.f) {
			speed = projSpeed;
		}
		const float myGravity = wd->GetMyGravity() * SQUARE(FRAMES_PER_SEC);  // elmo/frame^2; 0: map gravity
		if (myGravity > 0.f) {
			gravity = myGravity;
		}
		heightMod = wd->GetHeightMod();
		delete wd;
	}

	// Shots: every real weapon (a gun of several fires them in turn)
	for (SWeapon& w : weapons) {
		delete w.weapon;
	}
	weapons.clear();
	for (Weapon* weapon : unit->GetUnit()->GetWeapons()) {
		WeaponDef* wd = weapon->GetDef();
		const std::map<std::string, std::string>& customParams = wd->GetCustomParams();
		const bool isReal = (customParams.find("fake_weapon") == customParams.end()) && !wd->IsShield()
				&& !wd->IsManualFire();
		delete wd;
		if (isReal) {
			weapons.push_back({weapon, std::max(weapon->GetReloadTime(), 1), weapon->GetReloadFrame()});
		} else {
			delete weapon;
		}
	}
}

// A shot: a weapon's reload frame jumped forward by half its reload or more
int CPriorityArtyTask::CountShots()
{
	int shots = 0;
	for (SWeapon& w : weapons) {
		const int reloadFrame = w.weapon->GetReloadFrame();
		if (reloadFrame - w.lastReloadFrame >= std::max(w.reloadFrames / 2, 1)) {
			++shots;
		}
		w.lastReloadFrame = reloadFrame;
	}
	return shots;
}

void CPriorityArtyTask::Update()
{
	if (units.empty() || (tp == nullptr)) {
		return;
	}
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	const int elapsed = frame - lastUpdateFrame;
	if (elapsed < UPDATE_INTERVAL) {
		return;
	}
	lastUpdateFrame = frame;
	CCircuitUnit* unit = *units.begin();

	int shots = 0;
	TRY_UNIT(circuit, unit,
		shots = CountShots();
	)
	if ((unit->Blocker() != nullptr) || IsDisabled(unit, frame)) {
		if (noShotFrame != INT_MAX) {
			noShotFrame += elapsed;  // the no-shot timer pauses
		}
		return;  // a stun may hold the reload back: not a shot
	}

	if ((shots > 0) && (target != nullptr)) {
		for (int i = 0; i < shots; ++i) {
			if (target->IsInRadarOrLOS()) {
				landFrames.push_back(frame + flightFrames);  // dry once landed with no damage (OnEnemyDamaged)
			} else {
				target->GetData()->AddBlindShot();  // no damage events out of both
			}
		}
		noShotFrame = frame + reloadFrames + static_cast<int>(tp->shotGrace * FRAMES_PER_SEC);
	}
	while (!landFrames.empty() && (landFrames.front() + LAND_MARGIN <= frame)) {
		landFrames.pop_front();
		++dryShots;
	}

	if ((target != nullptr) && IsLost(frame)) {  // re-pick at once
#ifdef DEBUG_LOG
		circuit->LOG("ARTY %i lost target %i (dry %i)", unit->GetId(), target->GetId(), dryShots);
#endif
		SetTarget(nullptr);
		checkFrame = frame;
	}
	if (frame >= checkFrame) {
		Pick(unit, frame);
	}
	if ((target != nullptr) && isReorder) {
		Order(unit);
	}
}

void CPriorityArtyTask::OnUnitIdle(CCircuitUnit* unit)
{
	IFighterTask::OnUnitIdle(unit);
	isReorder = true;
}

void CPriorityArtyTask::OnEnemyDamaged(CEnemyInfo* enemy)
{
	dryShots = 0;
	failures.erase(enemy->GetId());
	// The shells due by now hit something
	const int frame = manager->GetCircuit()->GetLastFrame();
	while (!landFrames.empty() && (landFrames.front() <= frame + LAND_MARGIN)) {
		landFrames.pop_front();
	}
}

bool CPriorityArtyTask::IsDisabled(CCircuitUnit* unit, int frame) const
{
	if (unit->IsDisarmed(frame)) {
		return true;
	}
	Unit* u = unit->GetUnit();
	return u->IsParalyzed() || (u->GetRulesParamFloat("lowpower", 0.f) > 0.f);
}

bool CPriorityArtyTask::IsLost(int frame)
{
	bool isMobile;
	if (!IsTargetable(target, isMobile)) {  // dead, hidden, out of range, out of sight or submerged
		return true;
	}
	if (target->IsInRadarOrLOS() && (dryShots >= DRY_SHOTS)) {
		AddFailure(target, frame);
		return true;
	}
	if (IsBlindSpent(target)) {
		return true;  // skipped until it's seen again
	}
	if (frame > noShotFrame) {
		AddFailure(target, frame);
		return true;
	}
	return IsFriendNear(target->GetPos(), frame);  // no failure: it may clear
}

bool CPriorityArtyTask::IsTargetable(CEnemyInfo* enemy, bool& outIsMobile) const
{
	CEnemyUnit* data = enemy->GetData();
	if (data->IsDead() || data->IsFake() || enemy->IsHidden()) {
		return false;
	}
	const AIFloat3& ePos = enemy->GetPos();
	CCircuitDef* edef = enemy->GetCircuitDef();
	if (edef != nullptr) {
		if (edef->IsAbleToFly()) {
			return false;
		}
		outIsMobile = edef->IsMobile();
	} else {  // a radar blip of unknown type
		const float ground = std::max(manager->GetCircuit()->GetTerrainManager()->GetHeightAt(ePos.x, ePos.z), 0.f);
		if (ePos.y > ground + FLY_LIFT) {
			return false;
		}
		outIsMobile = true;  // attack-unit: the engine follows the blip
	}
	if (outIsMobile && !enemy->IsInRadarOrLOS()) {
		return false;
	}
	if (!isAntiWater && (ePos.y < -DIVE_DEPTH)) {
		return false;  // the engine won't aim at it (ArtilleryTask's test)
	}
	return IsInRange(ePos);
}

bool CPriorityArtyTask::IsInRange(const AIFloat3& enemyPos) const
{
	const float heightDiff = (enemyPos.y - aimFrom.y) * heightMod;
	const float maxRange = ballistics::CannonRange(range, heightDiff, speed, gravity) * RANGE_MOD;
	return aimFrom.SqDistance2D(enemyPos) <= SQUARE(maxRange);
}

/*
 * Friendly units (ours and allies', not planes) in a box around the aim point along the line of fire: the shell
 * scatters mostly along it. Half-sizes: aoe + guardSide x distance across, aoe + guardAlong along
 */
bool CPriorityArtyTask::IsFriendNear(const AIFloat3& aimPos, int frame) const
{
	CCircuitAI* circuit = manager->GetCircuit();
	const float dx = aimPos.x - aimFrom.x;
	const float dz = aimPos.z - aimFrom.z;
	const float dist = std::sqrt(dx * dx + dz * dz);
	if (dist < 1.f) {
		return true;
	}
	const float ux = dx / dist;
	const float uz = dz / dist;
	const float side = aoe + tp->guardSide * dist;
	const float along = aoe + tp->guardAlong;
	const float radius = std::sqrt(SQUARE(side) + SQUARE(along));
	COOAICallback* clb = circuit->GetCallback();
	if (!clb->IsFriendlyUnitsIn(aimPos, radius, false)) {
		return false;  // most often: a count, without filling the id buffer
	}
	for (int fId : clb->GetFriendlyUnitIdsIn(aimPos, radius, false)) {
		if (fId == -1) {
			continue;
		}
		CAllyUnit* ally = circuit->GetFriendlyUnit(fId);
		if (ally == nullptr) {
			return true;  // not listed yet: assume it's in the way
		}
		if ((ally->GetCircuitDef() != nullptr) && ally->GetCircuitDef()->IsPlane()) {
			continue;
		}
		const AIFloat3& p = ally->GetPos(frame);
		const float px = p.x - aimPos.x;
		const float pz = p.z - aimPos.z;
		if ((std::fabs(px * ux + pz * uz) < along) && (std::fabs(px * uz - pz * ux) < side)) {
			return true;
		}
	}
	return false;
}

bool CPriorityArtyTask::IsArcClear(const AIFloat3& aimPos, bool isHigh) const
{
	CTerrainManager* terrainMgr = manager->GetCircuit()->GetTerrainManager();
	auto heightAt = [terrainMgr](float x, float z) {
		return std::max(terrainMgr->GetHeightAt(x, z), 0.f);  // shells fly over water
	};
	return ballistics::IsArcClear(aimFrom.x, aimFrom.y, aimFrom.z, aimPos.x, aimPos.y, aimPos.z, speed, gravity,
			ARC_STEP, ARC_CLEARANCE, ARC_STOP_SHORT, heightAt, isHigh);
}

int CPriorityArtyTask::GetBlindBudget(CEnemyInfo* enemy) const
{
	float health = enemy->GetHealth();
	if ((health <= 0.f) && (enemy->GetCircuitDef() != nullptr)) {
		health = enemy->GetCircuitDef()->GetHealth();
	}
	const int toKill = std::max(static_cast<int>(std::ceil(health / shellDamage)), 1);
	return std::max(BLIND_SHOTS_MIN, BLIND_KILL_MOD * toKill);
}

bool CPriorityArtyTask::IsBlindSpent(CEnemyInfo* enemy) const
{
	if (enemy->IsInRadarOrLOS()) {
		return false;
	}
	const int shots = enemy->GetData()->GetBlindShots();
	return (shots > 0) && (shots >= GetBlindBudget(enemy));
}

bool CPriorityArtyTask::MakeCandidate(CEnemyInfo* enemy, bool isHigh, SCandidate& outCand) const
{
	bool isMobile;
	if (!IsTargetable(enemy, isMobile)) {
		return false;
	}
	const bool isVisible = enemy->IsInLOS();
	const int tier = tp->GetTier(enemy->GetCircuitDef(), isVisible);
	if (tier == STargetPriority::NO_TIER) {
		return false;
	}
	const AIFloat3& ePos = enemy->GetPos();
	const float dx = ePos.x - aimFrom.x;
	const float dz = ePos.z - aimFrom.z;
	const float dist = std::sqrt(dx * dx + dz * dz);
	float tanA;
	if (!ballistics::ArcTan(dist, ePos.y - aimFrom.y, speed, gravity, isHigh, tanA)) {
		return false;
	}
	constexpr float RAD_TO_DEG = 180.f / M_PI;
	const float eHeading = std::atan2(dx, dz) * RAD_TO_DEG;  // 0 = south (+z), 90 = east (+x)
	const float ePitch = std::atan(tanA) * RAD_TO_DEG;
	const float turnSec = arty::TurnTime(heading, pitch, eHeading, ePitch, tp->turnYaw, tp->turnPitch, tp->isTurnFlip);
	const float flightSec = ballistics::FlightTime(dist, tanA, speed);
	bool isFar = false;
	if (isMobile) {
		isFar = (turnSec > FAR_TURN_SEC);
		if ((tp->mobileMove >= 0.f) && !isFar) {  // it would be gone before the shell lands
			const float moveDist = std::sqrt(enemy->GetVel().SqLength2D()) * FRAMES_PER_SEC * (turnSec + flightSec);
			isFar = (moveDist > tp->mobileMove);
		}
	}

	outCand.enemy = enemy;
	outCand.rank.tier = tier;
	outCand.rank.value = enemy->GetCost();  // splash tiers: GetSplashValue, set once all buildings are known
	outCand.rank.turnSec = turnSec;
	outCand.rank.isFar = isFar;
	outCand.rank.isFinished = !enemy->IsBeingBuilt();
	outCand.rank.isVisible = isVisible;
	outCand.heading = eHeading;
	outCand.pitch = ePitch;
	outCand.flightSec = flightSec;
	outCand.isMobile = isMobile;
	outCand.isHigh = isHigh;
	return true;
}

// The target's cost and the cost of known enemy buildings within splash of it
float CPriorityArtyTask::GetSplashValue(CEnemyInfo* enemy) const
{
	const AIFloat3& ePos = enemy->GetPos();
	const ICoreUnit::Id eId = enemy->GetId();
	const float sqRadius = SQUARE(splashRadius);
	float value = enemy->GetCost();
	for (const SBuilding& b : buildings) {
		if ((b.id != eId) && (b.pos.SqDistance2D(ePos) <= sqRadius)) {
			value += b.cost;
		}
	}
	return value;
}

void CPriorityArtyTask::AddFailure(CEnemyInfo* enemy, int frame)
{
	SFailure& f = failures[enemy->GetId()];
	++f.count;
	f.skipUntil = frame + arty::FailSkipFrames(f.count, FAIL_SKIP, FAIL_SKIP_MAX_SHIFT);
}

void CPriorityArtyTask::Pick(CCircuitUnit* unit, int frame)
{
	CCircuitAI* circuit = manager->GetCircuit();
	checkFrame = frame + CHECK_INTERVAL;

	for (auto it = failures.begin(); it != failures.end();) {  // forget the gone
		it = (circuit->GetEnemyInfo(it->first) == nullptr) ? failures.erase(it) : std::next(it);
	}

	SCandidate cur;
	if ((target != nullptr) && !MakeCandidate(target, isTargetHigh, cur)) {
		SetTarget(nullptr);
	}
	const bool hasCur = (target != nullptr);
	if (hasCur) {  // its tier got better since the pick (now in LOS): keep that, so sight never makes a switch
		pickTier = (pickTier == STargetPriority::NO_TIER) ? cur.rank.tier : std::min(pickTier, cur.rank.tier);
	}

	const float sqBuildingRange = SQUARE(range + splashRadius);
	buildings.clear();
	candidates.clear();
	for (const auto& kv : circuit->GetEnemyInfos()) {
		CEnemyInfo* enemy = kv.second;
		if (tp->hasSplash) {
			CCircuitDef* edef = enemy->GetCircuitDef();
			if ((edef != nullptr) && !edef->IsMobile() && !enemy->GetData()->IsDead() && !enemy->IsHidden()
				&& (aimFrom.SqDistance2D(enemy->GetPos()) <= sqBuildingRange))
			{
				buildings.push_back({enemy->GetId(), enemy->GetPos(), enemy->GetCost()});
			}
		}
		if (enemy == target) {
			continue;
		}
		auto it = failures.find(enemy->GetId());
		if ((it != failures.end()) && (it->second.skipUntil > frame)) {
			continue;
		}
		if (IsBlindSpent(enemy)) {
			continue;  // until it's seen again
		}
		SCandidate cand;
		if (MakeCandidate(enemy, false, cand)) {
			candidates.push_back(cand);
		}
	}
	auto setValue = [this](SCandidate& c) {
		if (tp->IsSplash(c.rank.tier)) {
			c.rank.value = GetSplashValue(c.enemy);
		}
		c.rank.value = arty::TurnWeighted(c.rank.value, c.rank.turnSec, tp->turnHalfValue);
	};
	if (hasCur) {
		setValue(cur);
	}
	for (SCandidate& c : candidates) {
		setValue(c);
	}
	std::sort(candidates.begin(), candidates.end(), [](const SCandidate& a, const SCandidate& b) {
		return arty::IsRankedHigher(a.rank, b.rank);
	});

	// The first that ranks above the current target and is worth switching to (with none: the first), with no
	// friendly unit near and a clear arc
	SCandidate* best = nullptr;
	for (SCandidate& c : candidates) {
		if (hasCur) {
			if (!arty::IsRankedHigher(c.rank, cur.rank)) {
				break;  // nor any after it: keep the current target
			}
			if (!arty::IsSwitchWorth(c.rank, cur.rank, pickTier, SWITCH_VALUE)) {
				continue;
			}
		}
		const AIFloat3& ePos = c.enemy->GetPos();
		if (IsFriendNear(ePos, frame)) {
			continue;
		}
		if (!IsArcClear(ePos, false)) {
			// A building may take the high arc over the terrain (a gun with a trajectory toggle)
			SCandidate high;
			if (!tp->isHighArc || c.isMobile || !IsArcClear(ePos, true) || !MakeCandidate(c.enemy, true, high)) {
				AddFailure(c.enemy, frame);  // not checked again within its skip window
#ifdef DEBUG_LOG
				circuit->LOG("ARTY %i arc blocked to %i", unit->GetId(), c.enemy->GetId());
#endif
				continue;
			}
			high.rank = c.rank;  // ranked as it was
			c = high;
		}
		best = &c;
		break;
	}

	if (best == nullptr) {
		if (!hasCur) {
			Drop(unit, frame);
		}
		return;
	}
#ifdef DEBUG_LOG
	circuit->LOG("ARTY %i picks %i tier %i value %.0f turn %.1fs flight %.1fs%s (was %i)", unit->GetId(),
			best->enemy->GetId(), best->rank.tier + 1, best->rank.value, best->rank.turnSec, best->flightSec,
			best->isHigh ? " high" : "", hasCur ? target->GetId() : -1);
#endif
	SetNewTarget(*best, frame);
	Order(unit);
}

void CPriorityArtyTask::SetNewTarget(const SCandidate& cand, int frame)
{
	SetTarget(cand.enemy);
	pickTier = cand.rank.tier;
	isTargetMobile = cand.isMobile;
	isTargetHigh = cand.isHigh;
	flightFrames = static_cast<int>(cand.flightSec * FRAMES_PER_SEC);
	dryShots = 0;
	landFrames.clear();
	noShotFrame = frame + static_cast<int>((cand.rank.turnSec + tp->shotGrace) * FRAMES_PER_SEC) + reloadFrames;
	heading = cand.heading;
	pitch = cand.pitch;
}

void CPriorityArtyTask::Order(CCircuitUnit* unit)
{
	TRY_UNIT(manager->GetCircuit(), unit,
		const int wantTrajectory = isTargetHigh ? 1 : 0;
		if (tp->isHighArc && (trajectory != wantTrajectory)) {  // a gun with a trajectory toggle
			unit->GetUnit()->SetTrajectory(wantTrajectory);
			trajectory = wantTrajectory;
		}
		// No timeout: a static unit's order never times out anyway (CCommandAI)
		if (isTargetMobile) {  // the engine leads it
			unit->GetUnit()->Attack(target->GetUnit(), UNIT_COMMAND_OPTION_RIGHT_MOUSE_KEY);
		} else {  // no engine ground test on attack-ground for these weapons (unit_target_features.lua, avoidGround)
			unit->CmdAttackGround(target->GetPos(), UNIT_COMMAND_OPTION_RIGHT_MOUSE_KEY);
		}
	)
	isReorder = false;
	state = State::ENGAGE;
}

void CPriorityArtyTask::Drop(CCircuitUnit* unit, int frame)
{
	if (State::ENGAGE == state) {
		TRY_UNIT(manager->GetCircuit(), unit,
			unit->GetUnit()->Stop();
		)
		state = State::ROAM;
	}
	SetTarget(nullptr);
	pickTier = STargetPriority::NO_TIER;
	noShotFrame = INT_MAX;
	landFrames.clear();
	checkFrame = frame + IDLE_INTERVAL;
}

} // namespace circuit

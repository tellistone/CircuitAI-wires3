/*
 * PriorityArtyTask.cpp
 *
 *  Long-range static guns (Big Bertha, Disco Rave Party, Starlight) aimed by a fixed priority list,
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
#include "util/Point.h"
#include "CircuitAI.h"

#include "spring/SpringCallback.h"
#include "spring/SpringMap.h"

#include "AISCommands.h"
#include "Unit.h"
#include "Weapon.h"
#include "WeaponDef.h"
#include "WeaponMount.h"

#include <algorithm>
#include <cfloat>
#include <climits>
#include <cmath>

namespace circuit {

using namespace springai;

// At most this often (shots, the lost checks). The military manager comes round to each fight task at most every
// FRAMES_PER_SEC frames, so this keeps the checks within a second
#define UPDATE_INTERVAL	(FRAMES_PER_SEC / 2)
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
// Beam
#define SAT_SPEED		120.f  // elmo/s the satellite rises once undocked and falls once disabled
#define UNDOCK_FRAMES	(FRAMES_PER_SEC * 22 / 10)  // from docked to the beam on
#define DOWN_DIST		75.f  // the beam lands this close to the base: on the Starlight itself (it can't hurt it)
#define DOWN_PITCH		2.f  // degrees: as near straight down as that once the satellite is up
#define ALIGN_HEADING	2.f  // degrees: from near straight down, turn the heading first when it's more than this off
#define ALIGN_DIST		40.f  // ... by aiming at the ground this far from the base (under DOWN_DIST)
#define BEAM_CLEAR		50.f  // friendly guard: the beam's radius + this across
#define UNIT_HEIGHT		60.f  // ... and along, + where the slanted beam meets a unit this tall
#define LEAD_SEC		2.f  // ... a mobile target's box grows by its speed x this
#define BIG_COST		1000.f  // a friendly unit worth this much (or unknown) blocks a beam path
#define CRAWL_COST		2000.f  // out from the base along one ray: may cross this much of smaller friendly units
#define EXIT_LEN		300.f  // friends at the beam: the way out isn't tested for this long
#define PATH_SPACING	120.f  // elmos between a beam path's spots
#define PATH_SPOTS		200  // ... at most
#define PATH_TESTS		8  // per pick
#define DWELL_SEC		3.f  // on a target: its health / dps + this long
#define SOFT_SKIP		(FRAMES_PER_SEC * 10)  // dropped for friends: skipped this long, no failure
#define LINE_STEP		32.f  // elmos between beam line samples (terrain)
#define TAN_MAX			10.f  // the guard's slant: at most this x UNIT_HEIGHT
#define BLOCKED			FLT_MAX  // friendly cost

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
		, aimer(nullptr)
		, satHeight(0.f)
		, aimHeading(0.f)
		, aimPitch(0.f)
		, aimPoint(-RgtVector)
		, readyFrame(INT_MAX)
		, arriveFrame(-1)
		, dwellFrames(0)
		, crawlCost(0.f)
		, isAligning(false)
		, isDodging(false)
		, exitSpot(-RgtVector)
		, satId(-1)
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
	// Beam: just built, docked (it undocks once switched on); the heading is the satellite's, the facing's
	satHeight = 0.f;
	aimHeading = heading;
	aimPitch = 0.f;
	aimPoint = -RgtVector;
	readyFrame = INT_MAX;
	arriveFrame = -1;
	crawlCost = 0.f;
	isAligning = isDodging = false;
	exitSpot = -RgtVector;
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

	// Shots: every real weapon that can target (a gun of several fires them in turn; the Starlight's relays can't)
	for (SWeapon& w : weapons) {
		delete w.weapon;
	}
	weapons.clear();
	aimer = nullptr;
	CCircuitAI* circuit = manager->GetCircuit();
	const int anyCategory = circuit->GetAirCategory() | circuit->GetLandCategory() | circuit->GetWaterCategory();
	std::vector<WeaponMount*> mounts = cdef->GetDef()->GetWeaponMounts();  // in the unit's weapons' order
	std::vector<Weapon*> unitWeapons = unit->GetUnit()->GetWeapons();
	int minReload = INT_MAX;
	for (unsigned i = 0; i < unitWeapons.size(); ++i) {
		Weapon* weapon = unitWeapons[i];
		WeaponDef* wd = weapon->GetDef();
		const std::map<std::string, std::string>& customParams = wd->GetCustomParams();
		const bool isReal = (customParams.find("fake_weapon") == customParams.end()) && !wd->IsShield()
				&& !wd->IsManualFire() && ((i >= mounts.size()) || ((mounts[i]->GetOnlyTargetCategory() & anyCategory) != 0));
		delete wd;
		if (isReal) {
			const int reload = std::max(weapon->GetReloadTime(), 1);
			weapons.push_back({weapon, reload, weapon->GetReloadFrame()});
			minReload = std::min(minReload, reload);
			if (aimer == nullptr) {
				aimer = weapon;
			}
		} else {
			delete weapon;
		}
	}
	for (WeaponMount* m : mounts) {
		delete m;
	}
	if (minReload != INT_MAX) {
		reloadFrames = minReload;
	}
	if ((tp != nullptr) && tp->IsBeam()) {  // the aimer's live range (it follows range upgrades), but no further than the beam's
		range = (aimer != nullptr) ? std::min(aimer->GetRange(), tp->beamRange) : tp->beamRange;
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
	if (tp->IsBeam()) {
		UpdateBeam(unit, frame, elapsed, IsDisabled(unit, frame));
		return;
	}

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
	if ((frame >= checkFrame) && !Pick(unit, frame)) {
		Drop(unit, frame);
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
	if (tp->IsBeam()) {  // from the satellite at its height
		const float maxRange = beam::Reach(range, position.y + tp->beamHeight - enemyPos.y) * RANGE_MOD;
		return position.SqDistance2D(enemyPos) <= SQUARE(maxRange);
	}
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
	if (tp->IsBeam()) {
		return 1;  // one dwell
	}
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
	constexpr float RAD_TO_DEG = 180.f / M_PI;
	const float eHeading = std::atan2(dx, dz) * RAD_TO_DEG;  // 0 = south (+z), 90 = east (+x)
	float ePitch;
	float turnSec;
	float flightSec;
	if (tp->IsBeam()) {  // pitch from straight down, from the satellite at its height
		ePitch = beam::Pitch(dist, position.y + tp->beamHeight - ePos.y);
		turnSec = IsFromDown()  // the heading first, then out along the ray
				? arty::TurnTime(heading, 0.f, eHeading, 0.f, tp->turnYaw, 0.f, false)
					+ ((tp->turnPitch > 0.f) ? std::fabs(ePitch - pitch) / tp->turnPitch : 0.f)
				: arty::TurnTime(heading, pitch, eHeading, ePitch, tp->turnYaw, tp->turnPitch, false);
		flightSec = 0.f;
	} else {
		float tanA;
		if (!ballistics::ArcTan(dist, ePos.y - aimFrom.y, speed, gravity, isHigh, tanA)) {
			return false;
		}
		ePitch = std::atan(tanA) * RAD_TO_DEG;
		turnSec = arty::TurnTime(heading, pitch, eHeading, ePitch, tp->turnYaw, tp->turnPitch, tp->isTurnFlip);
		flightSec = ballistics::FlightTime(dist, tanA, speed);
	}
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

bool CPriorityArtyTask::Pick(CCircuitUnit* unit, int frame, float skipLen)
{
	CCircuitAI* circuit = manager->GetCircuit();
	checkFrame = frame + CHECK_INTERVAL;
	const bool isBeam = tp->IsBeam();
	if (isBeam) {
		if (aimer != nullptr) {
			TRY_UNIT(circuit, unit,
				range = std::min(aimer->GetRange(), tp->beamRange);
			)
		}
		circuit->UpdateFriendlyUnits();  // for the path tests
	}

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
	// friendly unit near and a clear arc (beam: a clear way there)
	SCandidate* best = nullptr;
	int pathTests = 0;
	for (SCandidate& c : candidates) {
		if (hasCur) {
			if (!arty::IsRankedHigher(c.rank, cur.rank)) {
				break;  // nor any after it: keep the current target
			}
			if (!arty::IsSwitchWorth(c.rank, cur.rank, pickTier, SWITCH_VALUE)) {
				continue;
			}
			if (isBeam && (arriveFrame < 0) && (c.rank.tier != 0) && (c.rank.turnSec > cur.rank.turnSec)) {
				continue;  // still turning to the current one: only to one no further, unless it's in the top tier
			}
		}
		if (isBeam) {
			if (pathTests >= PATH_TESTS) {
				break;
			}
			bool isTested = false;
			const bool isClear = IsBeamClear(c, frame, skipLen, isTested);
			pathTests += isTested ? 1 : 0;
			if (!isClear) {
				continue;
			}
			best = &c;
			break;
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
		return hasCur;
	}
#ifdef DEBUG_LOG
	circuit->LOG("ARTY %i picks %i tier %i value %.0f turn %.1fs flight %.1fs%s (was %i)", unit->GetId(),
			best->enemy->GetId(), best->rank.tier + 1, best->rank.value, best->rank.turnSec, best->flightSec,
			best->isHigh ? " high" : "", hasCur ? target->GetId() : -1);
#endif
	SetNewTarget(*best, frame);
	Order(unit);
	return true;
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
	noShotFrame = frame + static_cast<int>((cand.rank.turnSec + tp->shotGrace) * FRAMES_PER_SEC);
	if (tp->IsBeam()) {  // heading and pitch are tracked: turn to it (and get there by noShotFrame)
		arriveFrame = -1;
		crawlCost = IsFromDown() ? CRAWL_COST : 0.f;
		aimPoint = cand.enemy->GetPos();
	} else {
		noShotFrame += reloadFrames;
		heading = cand.heading;
		pitch = cand.pitch;
	}
}

void CPriorityArtyTask::Order(CCircuitUnit* unit)
{
	if (tp->IsBeam()) {
		isDodging = false;
		aimPoint = target->GetPos();
		const float h = std::atan2(aimPoint.x - position.x, aimPoint.z - position.z) * beam::RAD_TO_DEG;
		isAligning = IsFromDown() && (std::fabs(beam::HeadingDiff(heading, h)) > ALIGN_HEADING);
		if (isAligning) {  // near straight down: turn the heading on the Starlight itself, then crawl out along the ray
			aimPoint = position + AIFloat3(std::sin(h * beam::DEG_TO_RAD), 0.f, std::cos(h * beam::DEG_TO_RAD)) * ALIGN_DIST;
			aimPoint.y = std::max(manager->GetCircuit()->GetTerrainManager()->GetHeightAt(aimPoint.x, aimPoint.z), 0.f);
			TRY_UNIT(manager->GetCircuit(), unit,
				unit->CmdAttackGround(aimPoint, UNIT_COMMAND_OPTION_RIGHT_MOUSE_KEY);
			)
			isReorder = false;
			state = State::ENGAGE;
			return;
		}
	}
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
	if ((State::ENGAGE == state) && !isDodging) {  // a dodge goes on to its spot
		TRY_UNIT(manager->GetCircuit(), unit,
			unit->GetUnit()->Stop();
		)
		state = State::ROAM;
		// Beam: a Stop freezes it where it points (unit_starlight_handler.lua, StopAim)
		aimPoint = -RgtVector;
		aimHeading = heading;
		aimPitch = pitch;
	}
	isAligning = false;
	SetTarget(nullptr);
	pickTier = STargetPriority::NO_TIER;
	noShotFrame = INT_MAX;
	landFrames.clear();
	checkFrame = frame + IDLE_INTERVAL;
}

/*
 * Beam
 */
void CPriorityArtyTask::UpdateBeam(CCircuitUnit* unit, int frame, int elapsed, bool isDisabled)
{
	if (satId < 0) {  // made once the Starlight is up (mahlazer.lua)
		TRY_UNIT(manager->GetCircuit(), unit,
			satId = static_cast<int>(unit->GetUnit()->GetRulesParamFloat("has_satellite", -1.f));
		)
	}
	if ((target != nullptr) && isTargetMobile && !isAligning) {
		aimPoint = target->GetPos();  // the engine follows it
	}
	if (StepAim(frame, elapsed, isDisabled) && (target != nullptr)) {  // undocked: it turns back from where it docked
		isReorder = true;  // near straight down again: align first
		arriveFrame = -1;
		crawlCost = IsFromDown() ? CRAWL_COST : 0.f;
		SCandidate c;
		noShotFrame = MakeCandidate(target, false, c)
				? frame + static_cast<int>((c.rank.turnSec + tp->shotGrace) * FRAMES_PER_SEC) : frame;
	}
	if (isDisabled || (frame < readyFrame) || (unit->Blocker() != nullptr)) {  // docked or undocking: the timers pause
		if (noShotFrame != INT_MAX) {
			noShotFrame += elapsed;
		}
		if (arriveFrame >= 0) {
			arriveFrame += elapsed;
		}
		return;
	}

	const bool isAimDone = IsAimDone();
	const AIFloat3 spot = GetSpot(heading, pitch);
	if (utils::is_valid(exitSpot) && (isAimDone || (spot.SqDistance2D(exitSpot) >= SQUARE(EXIT_LEN)))) {
		exitSpot = -RgtVector;  // off the friendly units it found
	}
	if (isDodging && isAimDone) {
		isDodging = false;
	}
	if ((target != nullptr) && !isAligning && (arriveFrame < 0) && isAimDone) {  // on it: it should die within its dwell
		float health = target->GetHealth();
		if ((health <= 0.f) && (target->GetCircuitDef() != nullptr)) {
			health = target->GetCircuitDef()->GetHealth();
		}
		arriveFrame = frame;
		dwellFrames = static_cast<int>((std::max(health, 0.f) / tp->beamDps + DWELL_SEC) * FRAMES_PER_SEC);
		noShotFrame = INT_MAX;
		crawlCost = 0.f;
	}

	// Friendly units at the beam now: off them the short way (not checked again until it's EXIT_LEN away or there)
	if (!isDodging && !utils::is_valid(exitSpot) && (position.SqDistance2D(spot) >= SQUARE(DOWN_DIST))) {
		const float allowed = ((target != nullptr) && (arriveFrame < 0)) ? crawlCost : 0.f;
		if (GetFriendCost(spot, std::tan(pitch * beam::DEG_TO_RAD), 0.f, frame) > allowed) {
#ifdef DEBUG_LOG
			manager->GetCircuit()->LOG("BEAM %i friends at the beam (target %i)", unit->GetId(),
					(target != nullptr) ? target->GetId() : -1);
#endif
			if (target != nullptr) {
				SoftSkip(target, frame);
				SetTarget(nullptr);
			}
			exitSpot = spot;
			if (!Pick(unit, frame, EXIT_LEN)) {
				Dodge(unit, frame);
			}
			return;
		}
	}

	// Still on the way out: the rest of it isn't tested
	const float exitLeft = utils::is_valid(exitSpot) ? std::max(EXIT_LEN - spot.distance2D(exitSpot), 0.f) : 0.f;
	if ((target != nullptr) && IsBeamLost(frame, exitLeft)) {  // re-pick at once
#ifdef DEBUG_LOG
		manager->GetCircuit()->LOG("BEAM %i lost target %i", unit->GetId(), target->GetId());
#endif
		SetTarget(nullptr);
		checkFrame = frame;
	}
	if ((frame >= checkFrame) && !Pick(unit, frame, exitLeft)) {
		if (exitLeft > 0.f) {
			Dodge(unit, frame);  // a Stop would leave it on them
		} else {
			Drop(unit, frame);
		}
	}
	if ((target != nullptr)
		&& (isReorder || (isAligning && (std::fabs(beam::HeadingDiff(heading, aimHeading)) < 0.01f))))
	{
		Order(unit);  // aligned: now the target itself
	}
}

/*
 * Where the satellite points (mahlazer.lua): undocked, it turns towards its aim at "turn_rate". Once disabled the beam
 * goes off, the aim stops (StopAim) and the satellite falls at SAT_SPEED, its heading turning to the nearest 90 on the
 * way down and its pitch to straight down (that turn goes on once docked). Once back on it's ready at once while still
 * falling, else after UNDOCK_FRAMES, and rises at SAT_SPEED while it turns
 */
bool CPriorityArtyTask::StepAim(int frame, int elapsed, bool isDisabled)
{
	const float sec = static_cast<float>(elapsed) / FRAMES_PER_SEC;
	const float yawStep = tp->turnYaw * sec;
	const float pitchStep = (tp->turnPitch > 0.f) ? tp->turnPitch * sec : 180.f;
	if (isDisabled) {
		if (readyFrame != INT_MAX) {  // docking
			readyFrame = INT_MAX;
			aimHeading = heading;
			aimPitch = 0.f;
		}
		if (satHeight > 0.f) {
			heading = beam::StepHeading(heading, beam::NearestCardinal(heading), yawStep);
			satHeight = std::max(satHeight - SAT_SPEED * sec, 0.f);
		}
		pitch = beam::StepToward(pitch, aimPitch, pitchStep);
		return false;
	}
	bool isJustReady = false;
	if (readyFrame == INT_MAX) {
		readyFrame = (satHeight > 0.f) ? frame : frame + UNDOCK_FRAMES;
	}
	if (frame < readyFrame) {
		pitch = beam::StepToward(pitch, aimPitch, pitchStep);
		return false;
	}
	if (frame - elapsed < readyFrame) {
		isJustReady = true;
	}
	satHeight = std::min(satHeight + SAT_SPEED * sec, tp->beamHeight);
	if (utils::is_valid(aimPoint)) {  // the engine aims it from where the satellite is now
		const float dx = aimPoint.x - position.x;
		const float dz = aimPoint.z - position.z;
		aimHeading = std::atan2(dx, dz) * beam::RAD_TO_DEG;
		aimPitch = beam::Pitch(std::sqrt(dx * dx + dz * dz), GetSatY() - aimPoint.y);
	}
	heading = beam::StepHeading(heading, aimHeading, yawStep);
	pitch = beam::StepToward(pitch, aimPitch, pitchStep);
	return isJustReady;
}

bool CPriorityArtyTask::IsFromDown() const
{
	return isAligning || (pitch < DOWN_PITCH);
}

bool CPriorityArtyTask::IsAimDone() const
{
	return (std::fabs(beam::HeadingDiff(heading, aimHeading)) < 0.01f) && (std::fabs(pitch - aimPitch) < 0.01f);
}

float CPriorityArtyTask::GetSatY() const
{
	return position.y + satHeight;
}

AIFloat3 CPriorityArtyTask::GetSpot(float h, float p) const
{
	CTerrainManager* terrainMgr = manager->GetCircuit()->GetTerrainManager();
	const float tanP = std::tan(p * beam::DEG_TO_RAD);
	const AIFloat3 dir(std::sin(h * beam::DEG_TO_RAD), 0.f, std::cos(h * beam::DEG_TO_RAD));
	AIFloat3 spot = position + dir * (std::max(satHeight, 1.f) * tanP);  // on the base's level, then on the ground there
	CTerrainManager::CorrectPosition(spot);
	spot = position + dir * (std::max(GetSatY() - std::max(terrainMgr->GetHeightAt(spot.x, spot.z), 0.f), 1.f) * tanP);
	CTerrainManager::CorrectPosition(spot);
	spot.y = std::max(terrainMgr->GetHeightAt(spot.x, spot.z), 0.f);
	return spot;
}

/*
 * Friendly units (ours and allies', not planes, not the Starlight itself) in a box at the spot along the beam's
 * heading. Half-sizes: the beam's radius + BEAM_CLEAR across, and as much + UNIT_HEIGHT x tan(pitch) along (the slanted
 * beam meets a unit before the spot), both + lead. BLOCKED: one worth BIG_COST or more, or one not listed yet
 */
float CPriorityArtyTask::GetFriendCost(const AIFloat3& spot, float tanP, float lead, int frame) const
{
	CCircuitAI* circuit = manager->GetCircuit();
	const float dx = spot.x - position.x;
	const float dz = spot.z - position.z;
	const float dist = std::sqrt(dx * dx + dz * dz);
	const float ux = (dist > 1.f) ? dx / dist : 0.f;
	const float uz = (dist > 1.f) ? dz / dist : 1.f;
	const float side = aoe + BEAM_CLEAR + lead;
	const float along = side + UNIT_HEIGHT * std::min(tanP, TAN_MAX);
	const float radius = std::sqrt(SQUARE(side) + SQUARE(along));
	COOAICallback* clb = circuit->GetCallback();
	if (!clb->IsFriendlyUnitsIn(spot, radius, false)) {
		return 0.f;
	}
	const ICoreUnit::Id ownId = units.empty() ? -1 : (*units.begin())->GetId();
	float cost = 0.f;
	for (int fId : clb->GetFriendlyUnitIdsIn(spot, radius, false)) {
		if ((fId == -1) || (fId == ownId) || (fId == satId)) {
			continue;  // the beam can't hurt the Starlight, nor its satellite
		}
		CAllyUnit* ally = circuit->GetFriendlyUnit(fId);
		if (ally == nullptr) {
			return BLOCKED;  // not listed yet: assume it's in the way
		}
		CCircuitDef* fdef = ally->GetCircuitDef();
		if ((fdef != nullptr) && fdef->IsPlane()) {
			continue;
		}
		const AIFloat3& p = ally->GetPos(frame);
		const float px = p.x - spot.x;
		const float pz = p.z - spot.z;
		if ((std::fabs(px * ux + pz * uz) < along) && (std::fabs(px * uz - pz * ux) < side)) {
			const float c = (fdef != nullptr) ? fdef->GetCostM() : BIG_COST;
			if (c >= BIG_COST) {
				return BLOCKED;
			}
			cost += c;
		}
	}
	return cost;
}

/*
 * The spots the beam passes on its way (with the satellite at its height), each guarded by a circle of the beam's
 * radius + BEAM_CLEAR + UNIT_HEIGHT x tan(pitch); friendly units in any of them count once. A unit not listed yet is
 * left out (Pick updates the list first). BLOCKED: one worth BIG_COST or more
 */
float CPriorityArtyTask::GetPathCost(float toHeading, float toPitch, float y, float skipLen, int frame)
{
	CCircuitAI* circuit = manager->GetCircuit();
	const float drop = std::max(position.y + tp->beamHeight - y, 1.f);
	const float fromHeading = IsFromDown() ? toHeading : heading;  // near straight down: the heading first
	float minX = FLT_MAX, maxX = -FLT_MAX, minZ = FLT_MAX, maxZ = -FLT_MAX;
	spots.clear();
	beam::WalkPath(fromHeading, pitch, toHeading, toPitch, tp->turnYaw, tp->turnPitch, drop, PATH_SPACING, PATH_SPOTS,
		[&](float dx, float dz, float p, float along) {
			if ((SQUARE(dx) + SQUARE(dz) < SQUARE(DOWN_DIST)) || (along < skipLen)) {
				return;  // on the Starlight itself, or the way out
			}
			const float r = aoe + BEAM_CLEAR + UNIT_HEIGHT * std::min(std::tan(p * beam::DEG_TO_RAD), TAN_MAX);
			const float x = position.x + dx;
			const float z = position.z + dz;
			spots.push_back({x, z, r});
			minX = std::min(minX, x - r);
			maxX = std::max(maxX, x + r);
			minZ = std::min(minZ, z - r);
			maxZ = std::max(maxZ, z + r);
		});
	if (spots.empty()) {
		return 0.f;
	}
	const AIFloat3 center((minX + maxX) * 0.5f, position.y, (minZ + maxZ) * 0.5f);
	const float radius = 0.5f * std::sqrt(SQUARE(maxX - minX) + SQUARE(maxZ - minZ));
	COOAICallback* clb = circuit->GetCallback();
	if (!clb->IsFriendlyUnitsIn(center, radius, false)) {
		return 0.f;
	}
	const ICoreUnit::Id ownId = units.empty() ? -1 : (*units.begin())->GetId();
	float cost = 0.f;
	for (int fId : clb->GetFriendlyUnitIdsIn(center, radius, false)) {
		if ((fId == -1) || (fId == ownId) || (fId == satId)) {
			continue;
		}
		CAllyUnit* ally = circuit->GetFriendlyUnit(fId);
		if (ally == nullptr) {
			continue;
		}
		CCircuitDef* fdef = ally->GetCircuitDef();
		if ((fdef != nullptr) && fdef->IsPlane()) {
			continue;
		}
		const AIFloat3& p = ally->GetPos(frame);
		if ((p.x < minX) || (p.x > maxX) || (p.z < minZ) || (p.z > maxZ)) {
			continue;
		}
		for (const SSpot& s : spots) {
			if (SQUARE(p.x - s.x) + SQUARE(p.z - s.z) < SQUARE(s.radius)) {
				const float c = (fdef != nullptr) ? fdef->GetCostM() : BIG_COST;
				if (c >= BIG_COST) {
					return BLOCKED;
				}
				cost += c;
				break;
			}
		}
	}
	return cost;
}

bool CPriorityArtyTask::IsBeamLost(int frame, float skipLen)
{
	bool isMobile;
	if (!IsTargetable(target, isMobile) || IsBlindSpent(target)) {  // dead, hidden, out of range or sight, submerged
		return true;
	}
	if ((arriveFrame >= 0) && (frame >= arriveFrame + dwellFrames)) {  // still there after its dwell
		if (target->IsInRadarOrLOS()) {
			AddFailure(target, frame);
		} else {
			target->GetData()->AddBlindShot();  // skipped until it's seen again
		}
		return true;
	}
	if (frame > noShotFrame) {  // never got there
		AddFailure(target, frame);
		return true;
	}
	// Friendly units at the target, or (still turning) on the rest of the way there
	const AIFloat3& ePos = target->GetPos();
	const float dx = ePos.x - position.x;
	const float dz = ePos.z - position.z;
	const float ePitch = beam::Pitch(std::sqrt(dx * dx + dz * dz), position.y + tp->beamHeight - ePos.y);
	const float lead = isTargetMobile ? std::sqrt(target->GetVel().SqLength2D()) * FRAMES_PER_SEC * LEAD_SEC : 0.f;
	if ((GetFriendCost(ePos, std::tan(ePitch * beam::DEG_TO_RAD), lead, frame) > 0.f)
		|| ((arriveFrame < 0) && (GetPathCost(std::atan2(dx, dz) * beam::RAD_TO_DEG, ePitch, ePos.y, skipLen, frame) > crawlCost)))
	{
		SoftSkip(target, frame);
		return true;
	}
	return false;
}

bool CPriorityArtyTask::IsBeamClear(const SCandidate& c, int frame, float skipLen, bool& outTested)
{
	outTested = false;
	CCircuitAI* circuit = manager->GetCircuit();
	const AIFloat3& ePos = c.enemy->GetPos();
	const float lead = c.isMobile ? std::sqrt(c.enemy->GetVel().SqLength2D()) * FRAMES_PER_SEC * LEAD_SEC : 0.f;
	if (GetFriendCost(ePos, std::tan(c.pitch * beam::DEG_TO_RAD), lead, frame) > 0.f) {
		SoftSkip(c.enemy, frame);
		return false;
	}
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	auto heightAt = [terrainMgr](float x, float z) {
		return std::max(terrainMgr->GetHeightAt(x, z), 0.f);  // the beam goes through water
	};
	if (!beam::IsLineClear(position.x, position.y + tp->beamHeight, position.z, ePos.x, ePos.y, ePos.z, LINE_STEP,
		ARC_CLEARANCE, ARC_STOP_SHORT, circuit->GetMap()->GetMaxHeight(), heightAt))
	{
		AddFailure(c.enemy, frame);  // not checked again within its skip window
#ifdef DEBUG_LOG
		circuit->LOG("BEAM %i line blocked to %i", (*units.begin())->GetId(), c.enemy->GetId());
#endif
		return false;
	}
	outTested = true;
	const float allowed = IsFromDown() ? CRAWL_COST : 0.f;  // out from the base along one ray
	if (GetPathCost(c.heading, c.pitch, ePos.y, skipLen, frame) > allowed) {
		SoftSkip(c.enemy, frame);
		return false;
	}
	return true;
}

// Friendly units at the beam, and no target with a clear way: turn the heading (the faster move) a step to the side
// with less friendly cost first, out to 90 degrees, until the spot and the way there (but its first EXIT_LEN) are clear
void CPriorityArtyTask::Dodge(CCircuitUnit* unit, int frame)
{
	CCircuitAI* circuit = manager->GetCircuit();
	const AIFloat3 spot = GetSpot(heading, pitch);
	const float tanP = std::tan(pitch * beam::DEG_TO_RAD);
	const float r = std::max(position.distance2D(spot), 1.f);
	const float box = 2.f * (aoe + BEAM_CLEAR) + UNIT_HEIGHT * std::min(tanP, TAN_MAX);
	const float step = std::clamp(box / r * beam::RAD_TO_DEG, 2.f, 10.f);
	const float sign = (GetPathCost(heading + step, pitch, spot.y, 0.f, frame)
			<= GetPathCost(heading - step, pitch, spot.y, 0.f, frame)) ? 1.f : -1.f;
	float toHeading = heading + sign * step;  // none clear: a step to the better side
	for (float mod : {1.f, 2.f, 3.f, 5.f, 8.f, 12.f, 18.f, 27.f, 40.f}) {
		const float off = std::min(mod * step, 90.f);
		bool isFound = false;
		for (float side : {sign, -sign}) {
			const float h = heading + side * off;
			if ((GetFriendCost(GetSpot(h, pitch), tanP, 0.f, frame) <= 0.f)
				&& (GetPathCost(h, pitch, spot.y, EXIT_LEN, frame) <= 0.f))
			{
				toHeading = h;
				isFound = true;
				break;
			}
		}
		if (isFound || (off >= 90.f)) {
			break;
		}
	}
#ifdef DEBUG_LOG
	circuit->LOG("BEAM %i dodges %.1f deg", unit->GetId(), beam::HeadingDiff(heading, toHeading));
#endif
	aimPoint = GetSpot(toHeading, pitch);
	TRY_UNIT(circuit, unit,
		unit->CmdAttackGround(aimPoint, UNIT_COMMAND_OPTION_RIGHT_MOUSE_KEY);
	)
	isDodging = true;
	isAligning = false;
	isReorder = false;
	state = State::ENGAGE;
	checkFrame = frame + IDLE_INTERVAL;
}

void CPriorityArtyTask::SoftSkip(CEnemyInfo* enemy, int frame)
{
	SFailure& f = failures[enemy->GetId()];
	f.skipUntil = std::max(f.skipUntil, frame + SOFT_SKIP);
}

} // namespace circuit

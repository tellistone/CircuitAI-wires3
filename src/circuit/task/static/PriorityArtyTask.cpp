/*
 * PriorityArtyTask.cpp
 *
 *  Long-range static artillery (Big Bertha) aimed by a fixed priority list, @see PriorityArtyTask.h
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
#include "util/Utils.h"

#include "spring/SpringCallback.h"

#include "AISCommands.h"
#include "Damage.h"
#include "Lua.h"
#include "WeaponDef.h"
#include "WeaponMount.h"

#include <algorithm>
#include <climits>
#include <cmath>

namespace circuit {

using namespace springai;

#define CHECK_INTERVAL	(FRAMES_PER_SEC * 10)  // look for a better target this often
#define IDLE_INTERVAL	(FRAMES_PER_SEC * 3)  // no target: look again this often
#define ORDER_TIMEOUT	(FRAMES_PER_SEC * 60)
#define ORDER_REPEAT	(FRAMES_PER_SEC * 30)  // re-send the order before it times out
#define NO_SHOT_GRACE	(FRAMES_PER_SEC * 10)  // a shot is due within turning + 1 reload + this
#define FAIL_SKIP		(FRAMES_PER_SEC * 60)  // a failed target is skipped this long x 2^(failures - 1)
#define FAIL_SKIP_MAX_SHIFT	5  // ... up to 32 minutes
#define DRY_SHOTS		5  // shots at a target in LOS or radar that damaged nothing
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

CPriorityArtyTask::CPriorityArtyTask(ITaskManager* mgr)
		: IFighterTask(mgr, IFighterTask::FightType::SUPER, 1.f)
		, tp(nullptr)
		, aimFrom(-RgtVector)
		, range(0.f)
		, speed(1050.f)
		, gravity(120.f)
		, heightMod(0.8f)
		, aoe(0.f)
		, shellDamage(2000.f)
		, reloadFrames(1)
		, pickTier(STargetPriority::NO_TIER)
		, isTargetMobile(false)
		, checkFrame(0)
		, orderFrame(0)
		, noShotFrame(INT_MAX)
		, lastUpdateFrame(0)
		, lastReloadFrame(0)
		, dryShots(0)
		, heading(0.f)
		, pitch(0.f)
		, isReorder(false)
{
}

CPriorityArtyTask::~CPriorityArtyTask()
{
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
	const int frame = manager->GetCircuit()->GetLastFrame();
	position = unit->GetPos(frame);
	tp = unit->GetCircuitDef()->GetTargetPriority();
	ReadWeapon(unit);
	aimFrom = position;
	aimFrom.y += MUZZLE_LIFT;
	heading = 0.f;  // facing south (+z)
	TRY_UNIT(manager->GetCircuit(), unit,
		heading = 90.f * unit->GetUnit()->GetBuildingFacing();  // S, E, N, W
	)
	pitch = 0.f;
	checkFrame = frame;
	orderFrame = frame;
	noShotFrame = INT_MAX;
	lastUpdateFrame = frame;
	lastReloadFrame = unit->GetWeaponReloadFrame(frame);
	dryShots = 0;
	isReorder = false;
}

void CPriorityArtyTask::ReadWeapon(CCircuitUnit* unit)
{
	CCircuitDef* cdef = unit->GetCircuitDef();
	range = cdef->GetMaxRange(CCircuitDef::RangeType::LAND);
	aoe = cdef->GetAoe();
	reloadFrames = std::max(cdef->GetReloadTime(), 1);
	WeaponMount* mount = cdef->GetWeaponMount();
	if (mount == nullptr) {
		return;
	}
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
	Damage* damage = wd->GetDamage();
	const std::vector<float> damages = damage->GetTypes();
	delete damage;
	if (!damages.empty()) {
		float sum = 0.f;
		for (float d : damages) {
			sum += d;
		}
		if (sum > 0.f) {
			shellDamage = sum / damages.size();
		}
	}
	delete wd;
}

void CPriorityArtyTask::Update()
{
	if (units.empty() || (tp == nullptr)) {
		return;
	}
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	const int elapsed = frame - lastUpdateFrame;
	lastUpdateFrame = frame;
	CCircuitUnit* unit = *units.begin();

	const int reloadFrame = unit->GetWeaponReloadFrame(frame);
	if ((unit->Blocker() != nullptr) || IsDisabled(unit, frame)) {
		if (noShotFrame != INT_MAX) {
			noShotFrame += elapsed;  // the no-shot timer pauses
		}
		lastReloadFrame = reloadFrame;  // a stun may hold the reload back: not a shot
		return;
	}

	// A shot: the reload frame jumped forward by half a reload or more
	if ((reloadFrame - lastReloadFrame >= std::max(reloadFrames / 2, 1)) && (target != nullptr)) {
		if (target->IsInRadarOrLOS()) {
			++dryShots;  // reset by damage (OnEnemyDamaged)
		} else {
			circuit->GetMilitaryManager()->AddBlindShot(target->GetId());  // no damage events out of both
		}
		noShotFrame = frame + reloadFrames + NO_SHOT_GRACE;
	}
	lastReloadFrame = reloadFrame;

	// Lost target: re-pick at once
	if (target != nullptr) {
		bool isMobile;
		bool isLost = !IsTargetable(target, isMobile);  // dead, hidden, out of range or out of sight
		if (!isLost) {
			if (target->IsInRadarOrLOS()) {
				if (dryShots >= DRY_SHOTS) {
					AddFailure(target, frame);
					isLost = true;
				}
			} else if (IsBlindSpent(target)) {
				isLost = true;  // skipped until it's seen again
			}
		}
		if (!isLost && (frame > noShotFrame)) {
			AddFailure(target, frame);
			isLost = true;
		}
		if (isLost) {
#ifdef DEBUG_LOG
			circuit->LOG("ARTY %i lost target %i (dry %i)", unit->GetId(), target->GetId(), dryShots);
#endif
			SetTarget(nullptr);
			checkFrame = frame;
		}
	}

	if (frame >= checkFrame) {
		if ((target != nullptr) && IsFriendNear(target->GetPos(), frame)) {
			SetTarget(nullptr);
		}
		Pick(unit, frame);
		return;
	}

	if ((target != nullptr) && (isReorder || (frame >= orderFrame + ORDER_REPEAT))) {
		Order(unit, frame);
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
}

bool CPriorityArtyTask::IsDisabled(CCircuitUnit* unit, int frame) const
{
	if (unit->IsDisarmed(frame)) {
		return true;
	}
	Unit* u = unit->GetUnit();
	return u->IsParalyzed() || (u->GetRulesParamFloat("lowpower", 0.f) > 0.f);
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
	const std::vector<int> friendIds = circuit->GetCallback()->GetFriendlyUnitIdsIn(aimPos,
			std::sqrt(SQUARE(side) + SQUARE(along)), false);
	for (int fId : friendIds) {
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

bool CPriorityArtyTask::IsArcClear(const AIFloat3& aimPos) const
{
	CTerrainManager* terrainMgr = manager->GetCircuit()->GetTerrainManager();
	auto heightAt = [terrainMgr](float x, float z) {
		return std::max(terrainMgr->GetHeightAt(x, z), 0.f);  // shells fly over water
	};
	return ballistics::IsArcClear(aimFrom.x, aimFrom.y, aimFrom.z, aimPos.x, aimPos.y, aimPos.z, speed, gravity,
			ARC_STEP, ARC_CLEARANCE, ARC_STOP_SHORT, heightAt);
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
	const int shots = manager->GetCircuit()->GetMilitaryManager()->GetBlindShots(enemy->GetId());
	return (shots > 0) && (shots >= GetBlindBudget(enemy));
}

bool CPriorityArtyTask::MakeCandidate(CEnemyInfo* enemy, SCandidate& outCand) const
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
	float tanA;
	if (!ballistics::LowArcTan(std::sqrt(dx * dx + dz * dz), ePos.y - aimFrom.y, speed, gravity, tanA)) {
		return false;
	}
	constexpr float RAD_TO_DEG = 180.f / M_PI;
	const float eHeading = std::atan2(dx, dz) * RAD_TO_DEG;  // 0 = south (+z), 90 = east (+x)
	const float ePitch = std::atan(tanA) * RAD_TO_DEG;
	float dHeading = std::fmod(std::fabs(eHeading - heading), 360.f);
	if (dHeading > 180.f) {
		dHeading = 360.f - dHeading;
	}
	const float turnSec = std::max(dHeading / tp->turnYaw, std::fabs(ePitch - pitch) / tp->turnPitch);

	outCand.enemy = enemy;
	outCand.rank.tier = tier;
	outCand.rank.value = enemy->GetCost();  // splash tiers: GetSplashValue, set once all buildings are known
	outCand.rank.turnSec = turnSec;
	outCand.rank.isFar = isMobile && (turnSec > FAR_TURN_SEC);
	outCand.rank.isFinished = !enemy->IsBeingBuilt();
	outCand.rank.isVisible = isVisible;
	outCand.heading = eHeading;
	outCand.pitch = ePitch;
	outCand.isMobile = isMobile;
	return true;
}

// The target's cost and the cost of known enemy buildings within splash of it
float CPriorityArtyTask::GetSplashValue(CEnemyInfo* enemy) const
{
	const AIFloat3& ePos = enemy->GetPos();
	const ICoreUnit::Id eId = enemy->GetId();
	const float sqAoe = SQUARE(aoe);
	float value = enemy->GetCost();
	for (const SBuilding& b : buildings) {
		if ((b.id != eId) && (b.pos.SqDistance2D(ePos) <= sqAoe)) {
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
	if ((target != nullptr) && !MakeCandidate(target, cur)) {
		SetTarget(nullptr);
	}
	const bool hasCur = (target != nullptr);
	if (hasCur) {  // its tier got better since the pick (now in LOS): keep that, so sight never makes a switch
		pickTier = (pickTier == STargetPriority::NO_TIER) ? cur.rank.tier : std::min(pickTier, cur.rank.tier);
	}

	const bool isSplash = std::find(tp->isSplash.begin(), tp->isSplash.end(), true) != tp->isSplash.end();
	const float sqBuildingRange = SQUARE(range + aoe);
	buildings.clear();
	candidates.clear();
	for (const auto& kv : circuit->GetEnemyInfos()) {
		CEnemyInfo* enemy = kv.second;
		if (isSplash) {
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
		if (MakeCandidate(enemy, cand)) {
			candidates.push_back(cand);
		}
	}
	if (hasCur) {
		candidates.push_back(cur);
	}
	for (SCandidate& c : candidates) {
		if ((c.rank.tier < (int)tp->isSplash.size()) && tp->isSplash[c.rank.tier]) {
			c.rank.value = GetSplashValue(c.enemy);
		}
	}
	std::sort(candidates.begin(), candidates.end(), [](const SCandidate& a, const SCandidate& b) {
		return arty::IsRankedHigher(a.rank, b.rank);
	});

	// The first with no friendly unit near and a clear arc. The current target passed both already
	const SCandidate* best = nullptr;
	const SCandidate* curCand = nullptr;
	for (const SCandidate& c : candidates) {
		if (c.enemy == target) {
			curCand = &c;
			best = &c;
			break;
		}
		const AIFloat3& ePos = c.enemy->GetPos();
		if (IsFriendNear(ePos, frame)) {
			continue;
		}
		if (!IsArcClear(ePos)) {
			AddFailure(c.enemy, frame);  // not checked again within its skip window
#ifdef DEBUG_LOG
			circuit->LOG("ARTY %i arc blocked to %i", unit->GetId(), c.enemy->GetId());
#endif
			continue;
		}
		best = &c;
		break;
	}

	if (best == nullptr) {
		Drop(unit, frame);
		return;
	}
	if (best->enemy == target) {
		if (isReorder || (frame >= orderFrame + ORDER_REPEAT)) {
			Order(unit, frame);
		}
		return;
	}
	if (hasCur) {
		if (curCand == nullptr) {  // cur ranks below best: never reached in the walk
			for (const SCandidate& c : candidates) {
				if (c.enemy == target) {
					curCand = &c;
					break;
				}
			}
		}
		if ((curCand != nullptr) && !arty::IsSwitchWorth(best->rank, curCand->rank, pickTier, SWITCH_VALUE)) {
			if (isReorder || (frame >= orderFrame + ORDER_REPEAT)) {
				Order(unit, frame);
			}
			return;
		}
	}

#ifdef DEBUG_LOG
	circuit->LOG("ARTY %i picks %i tier %i value %.0f turn %.1fs (was %i)", unit->GetId(), best->enemy->GetId(),
			best->rank.tier + 1, best->rank.value, best->rank.turnSec, hasCur ? target->GetId() : -1);
#endif
	SetTarget(best->enemy);
	pickTier = best->rank.tier;
	isTargetMobile = best->isMobile;
	dryShots = 0;
	noShotFrame = frame + static_cast<int>(best->rank.turnSec * FRAMES_PER_SEC) + reloadFrames + NO_SHOT_GRACE;
	heading = best->heading;
	pitch = best->pitch;
	Order(unit, frame);
}

void CPriorityArtyTask::Order(CCircuitUnit* unit, int frame)
{
	CCircuitAI* circuit = manager->GetCircuit();
	const AIFloat3& tPos = target->GetPos();

	std::string cmd("ai_super_fire:");
	cmd += utils::int_to_string(unit->GetId()) + "/" + utils::int_to_string(tPos.x) + "/" + utils::int_to_string(tPos.z);
	circuit->GetLua()->CallRules(cmd.c_str(), cmd.size());

	TRY_UNIT(circuit, unit,
		if (isTargetMobile) {  // the engine leads it
			unit->GetUnit()->Attack(target->GetUnit(), UNIT_COMMAND_OPTION_RIGHT_MOUSE_KEY, frame + ORDER_TIMEOUT);
		} else {  // no engine ground test on attack-ground for this weapon (unit_target_features.lua)
			unit->CmdAttackGround(tPos, UNIT_COMMAND_OPTION_RIGHT_MOUSE_KEY, frame + ORDER_TIMEOUT);
		}
	)
	orderFrame = frame;
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
	checkFrame = frame + IDLE_INTERVAL;
}

} // namespace circuit

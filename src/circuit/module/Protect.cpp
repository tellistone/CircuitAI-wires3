/*
 * Protect.cpp
 *
 *  Created on: Nov 21, 2026
 *      Author: Claude
 */

#include "module/Protect.h"
#include "module/AttackBalance.h"
#include "module/BuilderManager.h"
#include "module/MilitaryManager.h"
#include "setup/SetupManager.h"
#include "task/fighter/DefendTask.h"
#include "task/fighter/SquadTask.h"
#include "terrain/TerrainManager.h"
#include "unit/CircuitUnit.h"
#include "unit/ally/AllyUnit.h"
#include "unit/enemy/EnemyUnit.h"
#include "CircuitAI.h"
#include "util/Utils.h"

#include "Log.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace circuit {

using namespace springai;

static constexpr int MIN_STATE_FRAMES = FRAMES_PER_SEC * 5;  // a go or no-go lasts at least this long
static constexpr int ATTACKER_FRAMES = FRAMES_PER_SEC * 5;  // an enemy that hit the commander is ranked first this long
static constexpr float COMMIT_MOD = 1.25f;  // groups are committed until groups + commander + static cover reach this x danger
static constexpr float COMMIT_MARGIN = 0.25f;  // ... or the go line + this, if more (a go_ratio over 1.25 can still go)
static constexpr float KEEP_REACH_MOD = 1.5f;  // a committed group is dropped past this x respond_sec of travel
static constexpr float CORNERED_MOD = 0.6f;  // the go line x this once the commander is cornered or paralysed
static constexpr float AIR_ON_SHARE = 0.6f;  // an air danger: gunships over this share of the commander's danger list
static constexpr float AIR_OFF_SHARE = 0.4f;  // ... until under this
static constexpr float PRESENT_SHARE = 0.5f;  // a group is present within this x zone of the commander
static constexpr float PRESENT_KEEP_SHARE = 0.625f;  // ... and stays present within this x zone
static constexpr size_t MAX_TURRET_SPOTS = 6;  // turret spots weighed as the meeting point per update
static constexpr float TURRET_SPOT_GAP = 150.f;  // turrets closer than this to a weighed spot count as that spot
static constexpr float AT_SPOT_DIST = DEFAULT_SLACK * 4;  // the commander stands at the meeting point within this
static constexpr float MEET_BACK_DIST = 100.f;  // the meeting point is this far behind its turret, away from the danger
static constexpr float MAX_TURRET_RANGE = 2000.f;  // static anti-air beyond this reach is no cover (IsStaticDefence's line)
static constexpr float ENGAGED_SLACK = 200.f;  // a group with a target out of the zone within its range + this is busy
static constexpr float PLANE_PASS_DIST = 300.f;  // a plane heads for the commander when its track passes this close
static constexpr int PLANE_HOLD_FRAMES = FRAMES_PER_SEC * 5;  // ... and counts this long after it last did
// The groups a go of each kind takes: ground (attack, raid and gathering) or air (anti-air, flying ones too)
static const std::vector<IFighterTask::FightType> GROUND_TYPES = {IFighterTask::FightType::ATTACK,
		IFighterTask::FightType::RAID, IFighterTask::FightType::DEFEND};
static const std::vector<IFighterTask::FightType> AIR_TYPES = {IFighterTask::FightType::AA};

// Drops the entries stamped more than maxFrames ago
static void EraseOlder(std::unordered_map<ICoreUnit::Id, int>& stamps, int frame, int maxFrames)
{
	for (auto it = stamps.begin(); it != stamps.end();) {
		it = (frame - it->second > maxFrames) ? stamps.erase(it) : std::next(it);
	}
}

// The track ahead (straight on at vel) passes within PLANE_PASS_DIST of the point toPos away
static bool IsHeadingFor(const AIFloat3& toPos, const AIFloat3& vel)
{
	const float sqSpeed = vel.SqLength2D();
	const float t = (sqSpeed > 1e-3f) ? std::max((toPos.x * vel.x + toPos.z * vel.z) / sqSpeed, 0.f) : 0.f;
	return SQUARE(toPos.x - vel.x * t) + SQUARE(toPos.z - vel.z * t) <= SQUARE(PLANE_PASS_DIST);
}

CProtect::CProtect(CCircuitAI* circuit, const SConfig& cfg)
		: circuit(circuit)
		, cfg(cfg)
		, state(State::OFF)
		, startFrame(-1)
		, stateFrame(-1)
		, lastDangerFrame(-1)
		, damagedFrame(-1)
		, isCornered(false)
		, commPos(-RgtVector)
		, dangerPos(-RgtVector)
		, isAirDanger(false)
		, isAirGo(false)
		, airShare(0.f)
		, peakAirShare(0.f)
		, goCount(0)
		, dangerRange(0.f)
		, planeDanger(0.f)
		, planeCount(0)
		, own(0.f)
		, danger(0.f)
		, staticCover(0.f)
		, commCover(0.f)
		, committedSum(0.f)
		, presentSum(0.f)
		, ratio(0.f)
		, deathSec(std::numeric_limits<float>::max())
		, prevHealth(-1.f)
		, prevHealthFrame(-1)
		, dps(0.f)
		, turretPos(-RgtVector)
{
}

CProtect::~CProtect()
{
}

void CProtect::OnCommDamaged(CCircuitUnit* comm, CEnemyInfo* attacker)
{
	if (!cfg.isEnabled || (comm != circuit->GetSetupManager()->GetCommander())) {
		return;
	}
	const int frame = circuit->GetLastFrame();
	damagedFrame = frame;
	if (attacker != nullptr) {
		attackers[attacker->GetId()] = frame;
	}
}

bool CProtect::IsZoneEnemy(const CEnemyInfo* enemy) const
{
	if (state != State::GO) {
		return false;
	}
	const CCircuitDef* edef = enemy->GetCircuitDef();
	if (edef == nullptr) {
		if (enemy->GetVel().SqLength2D() < 0.01f) {
			return false;  // a parked radar dot (a mex, a wall): only moving ones, as the commander's danger list
		}
	} else if (!edef->IsMobile() || !edef->IsAttacker()) {
		return false;  // mobile armed enemies only
	}
	return enemy->GetPos().SqDistance2D(commPos) <= SQUARE(cfg.zone);
}

bool CProtect::IsRecentAttacker(ICoreUnit::Id id, int frame) const
{
	auto it = attackers.find(id);
	return (it != attackers.end()) && (frame - it->second <= ATTACKER_FRAMES);
}

bool CProtect::IsAtMeetingPoint(const AIFloat3& pos) const
{
	return utils::is_valid(turretPos) && (pos.SqDistance2D(turretPos) < SQUARE(AT_SPOT_DIST));
}

float CProtect::GetGoBoost(const IFighterTask* task, const AIFloat3& pos) const
{
	auto it = committed.find(task);
	if ((it == committed.end()) || !IsInFight(pos, it->second.isPresent)) {
		return 1.f;
	}
	// The strength there now (a group still on its way isn't), and itself wherever it is
	const float self = std::max(it->second.strength, 1e-3f);
	return std::max((presentSum + (it->second.isPresent ? 0.f : self)) / self, 1.f);
}

void CProtect::Transfer(const IFighterTask* from, const IFighterTask* to)
{
	auto it = committed.find(from);
	if (it == committed.end()) {
		return;
	}
	const SCommit commit = it->second;
	committed.erase(it);
	committed.emplace(to, commit);  // already committed: kept (its strength is read again at the next update)
}

void CProtect::OnTaskEnd(const IFighterTask* task)
{
	auto it = committed.find(task);
	if (it == committed.end()) {
		return;
	}
	committedSum -= it->second.strength;
	if (it->second.isPresent) {
		presentSum -= it->second.strength;
	}
	committed.erase(it);
}

void CProtect::Update()
{
	if (!cfg.isEnabled) {
		return;
	}
	const int frame = circuit->GetLastFrame();
	EraseOlder(attackers, frame, ATTACKER_FRAMES);
	CCircuitUnit* comm = circuit->GetSetupManager()->GetCommander();
	CBuilderManager::SCommView view;
	if ((comm == nullptr) || comm->IsDead() || !circuit->GetBuilderManager()->GetCommView(comm, cfg.zone, view)) {
		if (state != State::OFF) {
			End("no commander", frame);
		}
		return;
	}
	commPos = comm->GetPos(frame);
	UpdateDeathTime(comm, frame);
	CCircuitDef* commDef = comm->GetCircuitDef();
	UpdatePlanes(frame);
	if (planeDanger > 0.f) {
		CollectTurrets(frame);  // for the bubble; else once the protect is on, below
	}

	// 1. Anchor: the seen danger beats the commander, or it took damage since the last update, or planes heading for
	// it beat its anti-air bubble (its own power if it hits air, and the anti-air turrets covering it)
	const bool isDamaged = (damagedFrame >= 0) && (frame - damagedFrame <= FRAMES_PER_SEC);
	const bool isPlaneThreat = (planeDanger > 0.f)
			&& (planeDanger > (commDef->HasAntiAir() ? view.own : 0.f) + GetStaticCover(commPos, true, 0.f));  // bombs land at the plane's spot
	const bool isThreat = (view.danger > view.own) || isDamaged || isPlaneThreat;
	if (isThreat) {
		lastDangerFrame = frame;
	}
	const bool isStart = (state == State::OFF);
	if (isStart) {
		if (!isThreat) {
			return;
		}
		Start(frame);
	} else if (frame - lastDangerFrame > cfg.keepFrames) {
		End("clear", frame);
		return;
	}
	if (planeDanger <= 0.f) {
		CollectTurrets(frame);
	}
	if (utils::is_valid(view.dangerPos)) {
		dangerPos = view.dangerPos;
		dangerRange = view.dangerRange;
	}
	// The air share: gunships of the commander's list and the planes heading for it, of both. Kept while both are
	// empty: gunships out of sight would otherwise let a ground go start
	const float listDanger = view.danger + planeDanger;
	if (listDanger > 0.f) {
		airShare = (view.airDanger + planeDanger) / listDanger;
		isAirDanger = (airShare > (isAirDanger ? AIR_OFF_SHARE : AIR_ON_SHARE));
		peakAirShare = std::max(peakAirShare, airShare);
	}
	own = (isAirDanger && !commDef->HasAntiAir()) ? 0.f : view.own;
	isCornered |= view.isCornered || comm->GetUnit()->IsParalyzed();

	// 2. Danger: the enemy force that could shoot the commander soon (the force balance's, statics included), or its
	// own list (gunships on their way, moving radar dots), whichever is more. The planes heading for it count in air
	// mode only: ground groups can't shoot them, so a few bombers with a ground push would sink a ground go
	CAttackBalance::SForce force;
	circuit->GetMilitaryManager()->GetAttackBalance()->GetForceAt(commPos, nullptr, commDef->IsAmphibious(), force);
	danger = std::max(force.enemy, view.danger + (isAirDanger ? planeDanger : 0.f));
	if (isStart) {
		circuit->LOG("PROTECT start: commander power %.1f, danger %.1f (force %.1f, its own list %.1f, bombers %i: %.1f),"
				" aircraft %.0f%%%s at (%.0f, %.0f)", own, danger, force.enemy, view.danger, planeCount, planeDanger,
				airShare * 100.f, isDamaged ? ", damaged" : "", commPos.x, commPos.z);
	}

	// 3. and 4. Turret point and commit
	UpdateTurretPoint(comm, frame);
	commCover = GetStaticCover(commPos, isAirDanger);
	const float goLine = cfg.goRatio * (isCornered ? CORNERED_MOD : 1.f);
	UpdateCommitted(frame, danger * std::max(COMMIT_MOD, goLine + COMMIT_MARGIN));
	ratio = (danger > 0.f) ? GetSum() / danger : std::numeric_limits<float>::max();

	// Go or no-go, each for MIN_STATE_FRAMES at least. A go needs a group: with none there is nothing to send, and a
	// no-go costs nothing (a poke with no danger would otherwise go at once and hold off a real no-go). One kind per
	// go: when the danger turns to the other kind, the go ends at once (ground groups can't shoot gunships, anti-air
	// can't hold ground), and the timer stays, as the air flag's own band damps the switch
	if ((state == State::GO) && (isAirDanger != isAirGo)) {
		const int goFrame = stateFrame;
		SetState(State::NOGO, isAirDanger ? "nogo (air)" : "nogo (ground)", frame);
		stateFrame = goFrame;
	} else if (frame - stateFrame >= MIN_STATE_FRAMES) {
		if ((state == State::NOGO) && (ratio >= goLine) && !committed.empty()) {
			SetState(State::GO, "go", frame);
		} else if ((state == State::GO) && (ratio < goLine / circuit->GetMilitaryManager()->GetAttackKeepThreat())) {
			SetState(State::NOGO, "nogo", frame);
		}
	}
}

void CProtect::Start(int frame)
{
	state = State::NOGO;
	startFrame = frame;
	stateFrame = -MIN_STATE_FRAMES;  // the first decision is at once
	isCornered = false;
	isAirDanger = false;
	airShare = peakAirShare = 0.f;
	goCount = 0;
	committed.clear();
	turretPos = dangerPos = -RgtVector;
	dangerRange = 0.f;
	isAirGo = false;
}

void CProtect::End(const char* reason, int frame)
{
	// One summary per protect, gone or not: an air-heavy rush never goes, so its share shows only here
	circuit->LOG("PROTECT end (%s) after %.0f s: %i go%s, aircraft up to %.0f%%", reason,
			(frame - startFrame) / float(FRAMES_PER_SEC), goCount, (goCount == 1) ? "" : "s", peakAirShare * 100.f);
	state = State::OFF;
	committed.clear();
	committedSum = presentSum = 0.f;
	turretPos = -RgtVector;
	isCornered = false;
	prevHealth = -1.f;
	dps = 0.f;
	deathSec = std::numeric_limits<float>::max();
}

void CProtect::SetState(State value, const char* reason, int frame)
{
	circuit->LOG("PROTECT %s: ratio %.2f (groups %.1f + commander %.1f + static %.1f vs danger %.1f), %i groups "
			"(present %.1f), aircraft %.0f%%%s%s%s, death in %.0f s", reason, std::min(ratio, 99.f), committedSum, own,
			staticCover, danger, (int)committed.size(), presentSum, airShare * 100.f, isAirDanger ? " (air danger)" : "",
			isCornered ? ", cornered" : "", utils::is_valid(turretPos) ? ", at turrets" : "", std::min(deathSec, 999.f));
	if (value == State::NOGO) {
		committed.clear();  // no group is sent: they go back to what they would do
	} else {
		++goCount;
		isAirGo = isAirDanger;
	}
	state = value;
	stateFrame = frame;
}

void CProtect::UpdateDeathTime(CCircuitUnit* comm, int frame)
{
	// Its damage rate, smoothed over about two updates; regeneration and repair lower it
	const float health = comm->GetUnit()->GetHealth();
	if ((prevHealth >= 0.f) && (frame > prevHealthFrame)) {
		const float rate = std::max(prevHealth - health, 0.f) * FRAMES_PER_SEC / (frame - prevHealthFrame);
		dps = 0.5f * (dps + rate);
	}
	prevHealth = health;
	prevHealthFrame = frame;
	deathSec = (dps > 1.f) ? health / dps : std::numeric_limits<float>::max();
}

void CProtect::UpdatePlanes(int frame)
{
	// Seen enemy planes with a ground or water weapon (bombers; gunships are in the commander's list) within the zone,
	// heading for the commander (the track ahead passes within PLANE_PASS_DIST) or that hit it in the last 5 s, and
	// held PLANE_HOLD_FRAMES after either, so a bomber turning or lining up doesn't drop in and out each second. At
	// their threat before thr_mod: bombers' thr_mod (0.01..0.1) keeps the threat map's readers from shying away from
	// planes overhead, and would hide them here
	planeDanger = 0.f;
	planeCount = 0;
	EraseOlder(planes, frame, PLANE_HOLD_FRAMES);
	const float sqZone = SQUARE(cfg.zone);
	for (auto& kv : circuit->GetEnemyInfos()) {
		const CEnemyInfo* enemy = kv.second;
		const CCircuitDef* edef = enemy->GetCircuitDef();
		if ((edef == nullptr) || !edef->IsPlane() || !(edef->HasAntiLand() || edef->HasAntiWater())
			|| enemy->IsHidden() || !enemy->IsInRadarOrLOS() || enemy->IsBeingBuilt() || enemy->IsParalyzed()
			|| enemy->IsDisarmed())
		{
			continue;
		}
		const AIFloat3 toComm = commPos - enemy->GetPos();
		if (toComm.SqLength2D() > sqZone) {
			continue;
		}
		if (IsRecentAttacker(enemy->GetId(), frame) || IsHeadingFor(toComm, enemy->GetVel())) {
			planes[enemy->GetId()] = frame;
		} else if (planes.find(enemy->GetId()) == planes.end()) {
			continue;
		}
		planeDanger += edef->GetRawThreat();
		++planeCount;
	}
}

/*
 * Static cover: our and allied finished turrets that hit what the danger is and reach the defence of a spot
 */

void CProtect::CollectTurrets(int frame)
{
	turrets.clear();
	const float sqReach = SQUARE(cfg.zone + 600.f);
	for (auto& kv : circuit->GetFriendlyUnits()) {
		CAllyUnit* ally = kv.second;
		CCircuitDef* adef = ally->GetCircuitDef();
		if ((adef == nullptr) || adef->IsMobile() || !adef->IsAttacker()) {
			continue;
		}
		// Against ground: a defence by the threat map's rule (no static AA, no superweapon or long-range artillery).
		// Against aircraft: anti-air within the same reach. Both kinds: the plane bubble is checked before the flag
		const bool isDefence = IFighterTask::IsStaticDefence(adef);
		const bool isAntiAir = adef->HasAntiAir() && (adef->GetMaxRange(CCircuitDef::RangeType::AIR) <= MAX_TURRET_RANGE);
		if (!isDefence && !isAntiAir) {
			continue;
		}
		const AIFloat3& allyPos = ally->GetPos(frame);
		if (!utils::is_valid(allyPos) || (allyPos.SqDistance2D(commPos) > sqReach)) {
			continue;
		}
		const float power = IFighterTask::GetCoverPower(ally);  // 0 for a nanoframe
		if (power > 0.f) {
			turrets.push_back({allyPos, power, adef, isDefence, isAntiAir});
		}
	}
}

float CProtect::GetStaticCover(const AIFloat3& pos, bool isAir, float range) const
{
	// A turret counts when it hits the danger's kind (aircraft: anti-air only) and its range covers the spot plus half
	// the attacker's range, where an enemy attacking the commander there stands on average
	const bool isWater = circuit->GetTerrainManager()->IsDeepWaterAt(pos);
	const CCircuitDef::RangeType rangeType = isAir ? CCircuitDef::RangeType::AIR
			: (isWater ? CCircuitDef::RangeType::WATER : CCircuitDef::RangeType::LAND);
	float cover = 0.f;
	for (const STurret& t : turrets) {
		const bool isHit = isAir ? t.isAntiAir : (t.isDefence && (isWater ? t.def->HasAntiWater() : t.def->HasAntiLand()));
		if (!isHit) {
			continue;
		}
		const float reach = t.def->GetMaxRange(rangeType) - 0.5f * range;
		if ((reach > 0.f) && (t.pos.SqDistance2D(pos) <= SQUARE(reach))) {
			cover += t.power;
		}
	}
	return cover;
}

AIFloat3 CProtect::GetBehind(CCircuitUnit* comm, const AIFloat3& turret) const
{
	// Behind the turret line rather than in it: the turrets take the first shots
	if (!utils::is_valid(dangerPos)) {
		return turret;
	}
	AIFloat3 dir = turret - dangerPos;
	dir.y = 0.f;
	const float dist = dir.Length2D();
	if (dist < 1.f) {
		return turret;
	}
	AIFloat3 spot = turret + dir * (MEET_BACK_DIST / dist);
	CTerrainManager::CorrectPosition(spot);
	return circuit->GetTerrainManager()->CanMoveToPos(comm->GetArea(), spot) ? spot : turret;
}

bool CProtect::IsTurretSpotValid(CCircuitUnit* comm, const AIFloat3& spot, float cover) const
{
	// Strong enough with the commander. Once it stands there, that is all: the danger follows it into the turrets.
	// On the way: reached before its time to death, away from the danger (PickFleeSpot's rule) and a safe flee spot
	if (cover + own < danger) {
		return false;
	}
	const float sqDist = commPos.SqDistance2D(spot);
	if (sqDist < SQUARE(AT_SPOT_DIST)) {
		return true;
	}
	const float speed = std::max(comm->GetCircuitDef()->GetSpeed(), 1.f);
	if (sqrtf(sqDist) / speed >= deathSec) {
		return false;
	}
	if (utils::is_valid(dangerPos) && (spot.SqDistance2D(dangerPos) <= commPos.SqDistance2D(dangerPos))) {
		return false;
	}
	return circuit->GetBuilderManager()->IsCommFleeSpot(comm, spot);
}

bool CProtect::IsTurretSpotHeld(CCircuitUnit* comm, const AIFloat3& spot, float cover) const
{
	// Chasers closing in on the turrets don't drop it: that is where the turrets shoot them
	return (cover + own >= danger / circuit->GetMilitaryManager()->GetAttackKeepThreat())
			&& circuit->GetTerrainManager()->CanMoveToPos(comm->GetArea(), spot);
}

void CProtect::UpdateTurretPoint(CCircuitUnit* comm, int frame)
{
	// Kept while it holds (IsTurretSpotHeld), replaced only when it fails: the spot behind the nearest turret group
	// near the commander that passes IsTurretSpotValid, with the cover where the commander would stand. The turrets
	// are collected in Update
	if (utils::is_valid(turretPos)) {
		const float cover = GetStaticCover(turretPos, isAirDanger);
		if (IsTurretSpotHeld(comm, turretPos, cover)) {
			staticCover = cover;
			return;
		}
		turretPos = -RgtVector;
	}
	staticCover = GetStaticCover(commPos, isAirDanger);  // turrets that already cover the commander where it stands
	if (turrets.empty()) {
		return;
	}
	std::sort(turrets.begin(), turrets.end(), [this](const STurret& a, const STurret& b) {
		return a.pos.SqDistance2D(commPos) < b.pos.SqDistance2D(commPos);
	});
	static F3Vec spots;  // NOTE: micro-opt
	spots.clear();
	for (const STurret& t : turrets) {
		if ((isAirDanger ? t.isAntiAir : t.isDefence) && std::none_of(spots.begin(), spots.end(), [&t](const AIFloat3& s) {
			return s.SqDistance2D(t.pos) < SQUARE(TURRET_SPOT_GAP);
		})) {
			spots.push_back(t.pos);
			if (spots.size() >= MAX_TURRET_SPOTS) {
				break;
			}
		}
	}
	for (const AIFloat3& s : spots) {  // the nearest first
		const AIFloat3 meet = GetBehind(comm, s);
		const float cover = GetStaticCover(meet, isAirDanger);
		if (IsTurretSpotValid(comm, meet, cover)) {
			turretPos = meet;
			staticCover = cover;
			return;
		}
	}
}

/*
 * Commit
 */

bool CProtect::IsInFight(const AIFloat3& pos, bool wasPresent) const
{
	const float radius = cfg.zone * (wasPresent ? PRESENT_KEEP_SHARE : PRESENT_SHARE);
	return pos.SqDistance2D(commPos) <= SQUARE(radius);
}

float CProtect::GetTravelSec(const ISquadTask* task, int frame) const
{
	return task->GetLeader()->GetPos(frame).distance2D(commPos) / std::max(task->GetLowestSpeed(), 1.f);
}

void CProtect::CollectCandidates(int frame, std::vector<SCandidate>& out) const
{
	// Groups of the danger's kind (GROUND_TYPES: attack, raid and gathering attack- or raid-bound ones, no flying
	// leader; AIR_TYPES: anti-air, flying ones too) that reach the commander in time and aren't fleeing or busy
	// fighting outside the zone
	out.clear();
	CMilitaryManager* militaryMgr = circuit->GetMilitaryManager();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	const float maxSec = std::min(cfg.respondSec, deathSec);
	const float sqZone = SQUARE(cfg.zone);
	for (IFighterTask::FightType type : isAirDanger ? AIR_TYPES : GROUND_TYPES) {
		for (IFighterTask* t : militaryMgr->GetTasks(type)) {
			ISquadTask* g = static_cast<ISquadTask*>(t);  // attack, raid, defend and anti-air tasks are squads
			CCircuitUnit* leader = g->GetLeader();
			if (g->IsDead() || (leader == nullptr) || (!isAirDanger && leader->GetCircuitDef()->IsAbleToFly())
				|| g->IsFleeing(frame)
				|| (committed.find(g) != committed.end()))
			{
				continue;
			}
			if (type == IFighterTask::FightType::DEFEND) {
				// quota.solo_hunt hunters too: on go each leaves as a one-unit attack group (CDefendTask::Update)
				const CDefendTask* dt = static_cast<CDefendTask*>(g);
				const IFighterTask::FightType promote = dt->GetPromote();
				if ((promote != IFighterTask::FightType::ATTACK) && (promote != IFighterTask::FightType::RAID)
					&& !dt->IsSolo())
				{
					continue;
				}
			}
			const float travelSec = GetTravelSec(g, frame);
			if ((travelSec > maxSec) || !terrainMgr->CanMoveToPos(leader->GetArea(), commPos)) {
				continue;
			}
			const AIFloat3& pos = leader->GetPos(frame);
			const CEnemyInfo* target = g->GetTarget();
			if ((target != nullptr) && (target->GetPos().SqDistance2D(commPos) > sqZone)
				&& (target->GetPos().SqDistance2D(pos) < SQUARE(g->GetHighestRange() + ENGAGED_SLACK)))
			{
				continue;
			}
			out.push_back({g, travelSec});
		}
	}
	// The soonest there first: a fast group from farther out beats a slow one nearby
	std::sort(out.begin(), out.end(), [](const SCandidate& a, const SCandidate& b) { return a.travelSec < b.travelSec; });
}

void CProtect::Refresh(const ISquadTask* task, int frame, SCommit& commit)
{
	// Its strength now. A fleeing group isn't present (no boost to the others), but still counts for keeping a go: a
	// short flee on the way in doesn't release every group. A go starts only with groups that aren't fleeing
	// (CollectCandidates skips them)
	commit.strength = task->GetRallyStrength();
	commit.isPresent = !task->IsFleeing(frame) && IsInFight(task->GetLeader()->GetPos(frame), commit.isPresent);
	committedSum += commit.strength;
	if (commit.isPresent) {
		presentSum += commit.strength;
	}
}

void CProtect::UpdateCommitted(int frame, float need)
{
	// Go: the committed groups stay (latched), new ones join while short. Dead ones drop out, and those gone out of
	// reach (a flee to a haven, our base or a far cluster; past KEEP_REACH_MOD x respond_sec of travel, so a group near
	// the join line doesn't flap). No-go: picked afresh. The go is judged with all of them; the boost only with those
	// present (GetGoBoost). New groups are of the danger's kind (CollectCandidates)
	if (state != State::GO) {
		committed.clear();
	}
	committedSum = 0.f;
	presentSum = own + commCover;
	for (auto it = committed.begin(); it != committed.end();) {
		const ISquadTask* g = static_cast<const ISquadTask*>(it->first);  // an ended task is gone already (OnTaskEnd)
		if (g->IsDead() || (g->GetLeader() == nullptr) || (GetTravelSec(g, frame) > cfg.respondSec * KEEP_REACH_MOD)) {
			it = committed.erase(it);
			continue;
		}
		Refresh(g, frame, it->second);
		++it;
	}
	if (GetSum() >= need) {
		return;
	}
	static std::vector<SCandidate> candidates;  // NOTE: micro-opt
	CollectCandidates(frame, candidates);
	for (const SCandidate& c : candidates) {
		Refresh(c.task, frame, committed[c.task]);
		if (GetSum() >= need) {
			break;
		}
	}
}

} // namespace circuit

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
		, dangerRange(0.f)
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
	for (auto it = attackers.begin(); it != attackers.end();) {
		it = (frame - it->second > ATTACKER_FRAMES) ? attackers.erase(it) : std::next(it);
	}
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

	// 1. Anchor: the seen danger beats the commander, or it took damage since the last update
	const bool isDamaged = (damagedFrame >= 0) && (frame - damagedFrame <= FRAMES_PER_SEC);
	const bool isThreat = (view.danger > view.own) || isDamaged;
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
	if (utils::is_valid(view.dangerPos)) {
		dangerPos = view.dangerPos;
		dangerRange = view.dangerRange;
	}
	if (view.danger > 0.f) {
		const float airShare = view.airDanger / view.danger;
		isAirDanger = (airShare > (isAirDanger ? AIR_OFF_SHARE : AIR_ON_SHARE));
	}
	if (isAirDanger && (state == State::GO)) {
		SetState(State::NOGO, "nogo (air)", frame);  // at once: the committed ground groups can't shoot gunships
	}
	CCircuitDef* commDef = comm->GetCircuitDef();
	own = (isAirDanger && !commDef->HasAntiAir()) ? 0.f : view.own;
	isCornered |= view.isCornered || comm->GetUnit()->IsParalyzed();

	// 2. Danger: the enemy force that could shoot the commander soon (the force balance's, statics included), or its
	// own list (gunships on their way, moving radar dots), whichever is more
	CAttackBalance::SForce force;
	circuit->GetMilitaryManager()->GetAttackBalance()->GetForceAt(commPos, nullptr, commDef->IsAmphibious(), force);
	danger = std::max(force.enemy, view.danger);
	if (isStart) {
		circuit->LOG("PROTECT start: commander power %.1f, danger %.1f (force %.1f, its own list %.1f)%s at (%.0f, %.0f)",
				own, danger, force.enemy, view.danger, isDamaged ? ", damaged" : "", commPos.x, commPos.z);
	}

	// 3. and 4. Turret point and commit
	UpdateTurretPoint(comm, frame);
	commCover = GetStaticCover(commPos);
	const float goLine = cfg.goRatio * (isCornered ? CORNERED_MOD : 1.f);
	UpdateCommitted(comm, frame, danger * std::max(COMMIT_MOD, goLine + COMMIT_MARGIN));
	ratio = (danger > 0.f) ? GetSum() / danger : std::numeric_limits<float>::max();

	// Go or no-go, each for MIN_STATE_FRAMES at least. A go needs a group: with none there is nothing to send, and a
	// no-go costs nothing (a poke with no danger would otherwise go at once and hold off a real no-go)
	if (frame - stateFrame >= MIN_STATE_FRAMES) {
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
	committed.clear();
	turretPos = dangerPos = -RgtVector;
	dangerRange = 0.f;
}

void CProtect::End(const char* reason, int frame)
{
	circuit->LOG("PROTECT end (%s) after %.0f s", reason, (frame - startFrame) / float(FRAMES_PER_SEC));
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
			"(present %.1f)%s%s%s, death in %.0f s", reason, std::min(ratio, 99.f), committedSum, own, staticCover,
			danger, (int)committed.size(), presentSum, isCornered ? ", cornered" : "", isAirDanger ? ", air" : "",
			utils::is_valid(turretPos) ? ", at turrets" : "", std::min(deathSec, 999.f));
	if (value == State::NOGO) {
		committed.clear();  // no group is sent: they go back to what they would do
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
		// Against gunships: anti-air within the same reach
		const bool isDefence = IFighterTask::IsStaticDefence(adef);
		const bool isAntiAir = adef->HasAntiAir() && (adef->GetMaxRange(CCircuitDef::RangeType::AIR) <= MAX_TURRET_RANGE);
		if (!(isAirDanger ? isAntiAir : isDefence)) {
			continue;
		}
		const AIFloat3& allyPos = ally->GetPos(frame);
		if (!utils::is_valid(allyPos) || (allyPos.SqDistance2D(commPos) > sqReach)) {
			continue;
		}
		const float power = IFighterTask::GetCoverPower(ally);  // 0 for a nanoframe
		if (power > 0.f) {
			turrets.push_back({allyPos, power, adef});
		}
	}
}

float CProtect::GetStaticCover(const AIFloat3& pos) const
{
	// A turret counts when it hits the danger's kind (gunships: anti-air only) and its range covers the spot plus half
	// the danger's range, where an enemy attacking the commander there stands on average
	const bool isWater = circuit->GetTerrainManager()->IsDeepWaterAt(pos);
	const CCircuitDef::RangeType rangeType = isAirDanger ? CCircuitDef::RangeType::AIR
			: (isWater ? CCircuitDef::RangeType::WATER : CCircuitDef::RangeType::LAND);
	float cover = 0.f;
	for (const STurret& t : turrets) {
		const bool isHit = isAirDanger ? t.def->HasAntiAir() : (isWater ? t.def->HasAntiWater() : t.def->HasAntiLand());
		if (!isHit) {
			continue;
		}
		const float reach = t.def->GetMaxRange(rangeType) - 0.5f * dangerRange;
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
	// near the commander that passes IsTurretSpotValid, with the cover where the commander would stand
	CollectTurrets(frame);
	if (utils::is_valid(turretPos)) {
		const float cover = GetStaticCover(turretPos);
		if (IsTurretSpotHeld(comm, turretPos, cover)) {
			staticCover = cover;
			return;
		}
		turretPos = -RgtVector;
	}
	staticCover = GetStaticCover(commPos);  // turrets that already cover the commander where it stands
	if (turrets.empty()) {
		return;
	}
	std::sort(turrets.begin(), turrets.end(), [this](const STurret& a, const STurret& b) {
		return a.pos.SqDistance2D(commPos) < b.pos.SqDistance2D(commPos);
	});
	static F3Vec spots;  // NOTE: micro-opt
	spots.clear();
	for (const STurret& t : turrets) {
		if (std::none_of(spots.begin(), spots.end(), [&t](const AIFloat3& s) {
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
		const float cover = GetStaticCover(meet);
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

void CProtect::CollectCandidates(CCircuitUnit* comm, int frame, std::vector<SCandidate>& out) const
{
	// Attack, raid and gathering (attack- or raid-bound) groups that reach the commander in time and aren't fleeing or
	// busy fighting outside the zone
	out.clear();
	CMilitaryManager* militaryMgr = circuit->GetMilitaryManager();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	const float maxSec = std::min(cfg.respondSec, deathSec);
	const float sqZone = SQUARE(cfg.zone);
	for (IFighterTask::FightType type : {IFighterTask::FightType::ATTACK, IFighterTask::FightType::RAID,
		IFighterTask::FightType::DEFEND})
	{
		for (IFighterTask* t : militaryMgr->GetTasks(type)) {
			ISquadTask* g = static_cast<ISquadTask*>(t);  // attack, raid and defend tasks are squads
			CCircuitUnit* leader = g->GetLeader();
			if (g->IsDead() || (leader == nullptr) || leader->GetCircuitDef()->IsAbleToFly() || g->IsFleeing(frame)
				|| (committed.find(g) != committed.end()))
			{
				continue;
			}
			if (type == IFighterTask::FightType::DEFEND) {
				const IFighterTask::FightType promote = static_cast<CDefendTask*>(g)->GetPromote();
				if ((promote != IFighterTask::FightType::ATTACK) && (promote != IFighterTask::FightType::RAID)) {
					continue;
				}
			}
			const AIFloat3& pos = leader->GetPos(frame);
			const float sqDist = pos.SqDistance2D(commPos);
			const float travelSec = sqrtf(sqDist) / std::max(g->GetLowestSpeed(), 1.f);
			if ((travelSec > maxSec) || !terrainMgr->CanMoveToPos(leader->GetArea(), commPos)) {
				continue;
			}
			const CEnemyInfo* target = g->GetTarget();
			if ((target != nullptr) && (target->GetPos().SqDistance2D(commPos) > sqZone)
				&& (target->GetPos().SqDistance2D(pos) < SQUARE(g->GetHighestRange() + ENGAGED_SLACK)))
			{
				continue;
			}
			out.push_back({g, sqDist});
		}
	}
	std::sort(out.begin(), out.end(), [](const SCandidate& a, const SCandidate& b) { return a.sqDist < b.sqDist; });
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

void CProtect::UpdateCommitted(CCircuitUnit* comm, int frame, float need)
{
	// Go: the committed groups stay (latched), dead ones drop out, new ones join while short. No-go: picked afresh.
	// The go is judged with all of them; the boost only with those present (GetGoBoost). An air danger takes no new
	// group: anti-air units are in anti-air tasks, not these
	if (state != State::GO) {
		committed.clear();
	}
	committedSum = 0.f;
	presentSum = own + commCover;
	for (auto it = committed.begin(); it != committed.end();) {
		const ISquadTask* g = static_cast<const ISquadTask*>(it->first);  // an ended task is gone already (OnTaskEnd)
		if (g->IsDead() || (g->GetLeader() == nullptr)) {
			it = committed.erase(it);
			continue;
		}
		Refresh(g, frame, it->second);
		++it;
	}
	if (isAirDanger || (GetSum() >= need)) {
		return;
	}
	static std::vector<SCandidate> candidates;  // NOTE: micro-opt
	CollectCandidates(comm, frame, candidates);
	for (const SCandidate& c : candidates) {
		Refresh(c.task, frame, committed[c.task]);
		if (GetSum() >= need) {
			break;
		}
	}
}

} // namespace circuit

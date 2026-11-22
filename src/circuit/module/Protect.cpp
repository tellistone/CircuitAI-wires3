/*
 * Protect.cpp
 *
 *  Created on: Nov 21, 2026
 *      Author: Claude
 */

#include "module/Protect.h"
#include "map/ThreatMap.h"
#include "module/BuilderManager.h"
#include "module/MilitaryManager.h"
#include "setup/SetupManager.h"
#include "task/RetreatTask.h"
#include "task/fighter/DefendTask.h"
#include "task/fighter/SquadTask.h"
#include "terrain/TerrainManager.h"
#include "unit/CircuitUnit.h"
#include "unit/ally/AllyUnit.h"
#include "unit/enemy/EnemyManager.h"
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
static constexpr float CORNERED_MOD = 0.6f;  // the go line x this once the commander is cornered or paralysed
static constexpr size_t MAX_TURRET_SPOTS = 6;  // turret spots weighed as the meeting point per update
static constexpr float TURRET_SPOT_GAP = 150.f;  // turrets closer than this to a weighed spot count as that spot
static constexpr float AT_SPOT_DIST = DEFAULT_SLACK * 4;  // the commander stands at a meeting point (as COMM_ESCAPE_MIN)
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
		, committedSum(0.f)
		, ratio(0.f)
		, deathSec(std::numeric_limits<float>::max())
		, prevHealth(-1.f)
		, prevHealthFrame(-1)
		, dps(0.f)
		, turretPos(-RgtVector)
		, meetPos(-RgtVector)
		, fleeMeetPos(-RgtVector)
		, meetTask(nullptr)
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
	if ((edef != nullptr) && (!edef->IsMobile() || !edef->IsAttacker())) {
		return false;  // mobile armed enemies only (a blip counts: it moves at the commander)
	}
	return enemy->GetPos().SqDistance2D(commPos) <= SQUARE(cfg.zone);
}

bool CProtect::IsRecentAttacker(ICoreUnit::Id id, int frame) const
{
	auto it = attackers.find(id);
	return (it != attackers.end()) && (frame - it->second <= ATTACKER_FRAMES);
}

float CProtect::GetJointMod(const IFighterTask* task) const
{
	if (state != State::GO) {
		return 1.f;
	}
	auto it = committed.find(task);
	if (it == committed.end()) {
		return 1.f;
	}
	return std::max((committedSum + own + staticCover) / std::max(it->second, 1e-3f), 1.f);
}

float CProtect::GetBoost(const IFighterTask* task, const AIFloat3& pos, float range) const
{
	if (state != State::GO) {
		return 1.f;  // the usual path: one compare
	}
	const float sqRange = SQUARE(range);
	if ((pos.SqDistance2D(commPos) > sqRange) && (!utils::is_valid(turretPos) || (pos.SqDistance2D(turretPos) > sqRange))) {
		return 1.f;
	}
	return GetJointMod(task);
}

void CProtect::Transfer(const IFighterTask* from, const IFighterTask* to)
{
	auto it = committed.find(from);
	if (it == committed.end()) {
		return;
	}
	const float strength = it->second;
	committed.erase(it);
	committed.emplace(to, strength);  // already committed: kept (its strength is read again at the next update)
	released.erase(to);
	if (meetTask == from) {
		meetTask = static_cast<const ISquadTask*>(to);
	}
}

void CProtect::OnTaskEnd(const IFighterTask* task)
{
	auto it = committed.find(task);
	if (it != committed.end()) {
		committedSum -= it->second;
		committed.erase(it);
	}
	released.erase(task);
	if (meetTask == task) {
		meetTask = nullptr;
	}
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
	own = view.own;
	UpdateDeathTime(comm, frame);

	// 1. Anchor: the seen danger beats the commander, or it took damage since the last update
	const bool isDamaged = (damagedFrame >= 0) && (frame - damagedFrame <= FRAMES_PER_SEC);
	const bool isThreat = (view.danger > own) || isDamaged;
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
		isAirDanger = view.isAir;
	}
	isCornered |= view.isCornered || comm->GetUnit()->IsParalyzed();

	// 2. Danger: the summed blobs centred in the zone (with the enemy count, as squads judge), or the commander's own
	// list (gunships and moving blips too), whichever is more
	danger = std::max(GetBlobDanger(frame), view.danger);
	if (isStart) {
		circuit->LOG("PROTECT start: commander power %.1f, danger %.1f (its own list %.1f)%s at (%.0f, %.0f)", own, danger,
				view.danger, isDamaged ? ", damaged" : "", commPos.x, commPos.z);
	}

	// 3. and 4. Turret point and commit
	UpdateTurretPoint(comm, frame);
	UpdateCommitted(comm, frame);
	const float sum = committedSum + own + staticCover;
	ratio = (danger > 0.f) ? sum / danger : std::numeric_limits<float>::max();

	// Go or no-go, each for MIN_STATE_FRAMES at least
	const float goLine = cfg.goRatio * (isCornered ? CORNERED_MOD : 1.f);
	if (frame - stateFrame >= MIN_STATE_FRAMES) {
		if ((state == State::NOGO) && (ratio >= goLine)) {
			SetState(State::GO, "go", frame);
		} else if ((state == State::GO) && (ratio < goLine / circuit->GetMilitaryManager()->GetAttackKeepThreat())) {
			SetState(State::NOGO, "nogo", frame);
		}
	}
	UpdateMeetingPoint(comm, frame);
}

void CProtect::Start(int frame)
{
	state = State::NOGO;
	startFrame = frame;
	stateFrame = -MIN_STATE_FRAMES;  // the first decision is at once
	isCornered = false;
	committed.clear();
	released.clear();
	turretPos = meetPos = fleeMeetPos = dangerPos = -RgtVector;
	dangerRange = 0.f;
	isAirDanger = false;
	meetTask = nullptr;
}

void CProtect::End(const char* reason, int frame)
{
	circuit->LOG("PROTECT end (%s) after %.0f s", reason, (frame - startFrame) / float(FRAMES_PER_SEC));
	state = State::OFF;
	committed.clear();
	released.clear();
	turretPos = meetPos = fleeMeetPos = -RgtVector;
	meetTask = nullptr;
	isCornered = false;
	prevHealth = -1.f;
	dps = 0.f;
	deathSec = std::numeric_limits<float>::max();
}

void CProtect::SetState(State value, const char* reason, int frame)
{
	circuit->LOG("PROTECT %s: ratio %.2f (groups %.1f + commander %.1f + static %.1f vs danger %.1f), %i groups%s%s, "
			"death in %.0f s", reason, ratio, committedSum, own, staticCover, danger, (int)committed.size(),
			isCornered ? ", cornered" : "", utils::is_valid(turretPos) ? ", at turrets" : "",
			std::min(deathSec, 999.f));
	if (value == State::NOGO) {
		// No group is sent: those committed run to the meeting point when they flee
		for (const auto& kv : committed) {
			released.insert(kv.first);
		}
		committed.clear();
		meetTask = nullptr;
	} else {
		for (const auto& kv : committed) {
			released.erase(kv.first);
		}
	}
	state = value;
	stateFrame = frame;
}

bool CProtect::IsTaskAlive(const IFighterTask* task) const
{
	CMilitaryManager* militaryMgr = circuit->GetMilitaryManager();
	IFighterTask* t = const_cast<IFighterTask*>(task);  // only looked up
	for (IFighterTask::FightType type : {IFighterTask::FightType::ATTACK, IFighterTask::FightType::RAID,
		IFighterTask::FightType::DEFEND})
	{
		const std::set<IFighterTask*>& tasks = militaryMgr->GetTasks(type);
		if (tasks.find(t) != tasks.end()) {
			return !t->IsDead() && (static_cast<const ISquadTask*>(task)->GetLeader() != nullptr);
		}
	}
	return false;
}

float CProtect::GetBlobDanger(int frame) const
{
	// The attack rally's seen time, so the per-frame blob cache is shared with it
	const int seenFrames = circuit->GetMilitaryManager()->GetAttackRallySeenFrames();
	const float sqZone = SQUARE(cfg.zone);
	float threat = 0.f;
	float count = 0.f;
	for (const CEnemyManager::SEnemyBlob& b : circuit->GetEnemyManager()->GetBlobs(frame, seenFrames)) {
		if (b.pos.SqDistance2D(commPos) <= sqZone) {
			threat += b.threat;
			count += b.count;
		}
	}
	return threat * circuit->GetThreatMap()->GetCountMod(count);
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

void CProtect::UpdateTurretPoint(CCircuitUnit* comm, int frame)
{
	// Kept while it still holds, replaced only when it fails: the nearest of the turret spots near the commander
	CollectTurrets(frame);
	if (utils::is_valid(turretPos)) {
		const float cover = GetStaticCover(turretPos);
		if (IsTurretSpotValid(comm, turretPos, cover)) {
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
		const float cover = GetStaticCover(s);
		if (IsTurretSpotValid(comm, s, cover)) {
			turretPos = s;
			staticCover = cover;
			return;
		}
	}
}

/*
 * Commit
 */

void CProtect::CollectCandidates(CCircuitUnit* comm, int frame, std::vector<SCandidate>& out) const
{
	// Attack, raid and gathering (attack- or raid-bound) groups that reach the commander in time and aren't busy
	// fighting outside the zone
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
			if (g->IsDead() || (leader == nullptr) || leader->GetCircuitDef()->IsAbleToFly()
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
			out.push_back({g, sqDist, travelSec});
		}
	}
	std::sort(out.begin(), out.end(), [](const SCandidate& a, const SCandidate& b) { return a.sqDist < b.sqDist; });
}

void CProtect::UpdateCommitted(CCircuitUnit* comm, int frame)
{
	// Go: the committed groups stay (latched), dead ones drop out, new ones join while short. No-go: picked afresh
	if (state != State::GO) {
		committed.clear();
	}
	committedSum = 0.f;
	for (auto it = committed.begin(); it != committed.end();) {
		if (!IsTaskAlive(it->first)) {
			it = committed.erase(it);
			continue;
		}
		const ISquadTask* g = static_cast<const ISquadTask*>(it->first);
		it->second = g->GetRallyStrength() * g->GetCountPowerMod();
		committedSum += it->second;
		++it;
	}
	const float need = danger * COMMIT_MOD;
	if (committedSum + own + staticCover >= need) {
		return;
	}
	static std::vector<SCandidate> candidates;  // NOTE: micro-opt
	CollectCandidates(comm, frame, candidates);
	for (const SCandidate& c : candidates) {
		const float strength = c.task->GetRallyStrength() * c.task->GetCountPowerMod();
		committed[c.task] = strength;
		if (state == State::GO) {
			released.erase(c.task);  // no-go: picked again, it still runs to the meeting point until the go
		}
		committedSum += strength;
		if (committedSum + own + staticCover >= need) {
			break;
		}
	}
}

void CProtect::UpdateMeetingPoint(CCircuitUnit* comm, int frame)
{
	// The commander's: the turret point, else on a go the nearest committed group (kept while it holds), which must be
	// away from the danger and a safe flee spot, or the commander stands at it already. On a no-go the commander flees
	// as it would (its flee goal already prefers our groups), and the groups let go run to that goal. A health retreat
	// keeps its own goal: none here
	fleeMeetPos = -RgtVector;
	IUnitTask* task = comm->GetTask();
	const bool isRetreat = (task != nullptr) && (task->GetType() == IUnitTask::Type::RETREAT);
	if (isRetreat && !static_cast<CRetreatTask*>(task)->IsFlee()) {
		meetPos = -RgtVector;
		meetTask = nullptr;
		return;
	}
	if (utils::is_valid(turretPos)) {
		meetPos = turretPos;
		meetTask = nullptr;
		return;
	}
	if (state != State::GO) {
		meetPos = -RgtVector;
		meetTask = nullptr;
		if (isRetreat) {
			fleeMeetPos = static_cast<CRetreatTask*>(task)->GetFleeGoal();
		}
		return;
	}
	CBuilderManager* builderMgr = circuit->GetBuilderManager();
	auto isValid = [&](const AIFloat3& p) {
		if (commPos.SqDistance2D(p) < SQUARE(AT_SPOT_DIST)) {
			return true;
		}
		if (utils::is_valid(dangerPos) && (p.SqDistance2D(dangerPos) <= commPos.SqDistance2D(dangerPos))) {
			return false;
		}
		return builderMgr->IsCommFleeSpot(comm, p);
	};
	if ((meetTask != nullptr) && (committed.find(meetTask) != committed.end())) {
		const AIFloat3& p = meetTask->GetLeader()->GetPos(frame);
		if (isValid(p)) {
			meetPos = p;
			return;
		}
	}
	meetTask = nullptr;
	meetPos = -RgtVector;
	float minSqDist = std::numeric_limits<float>::max();
	for (const auto& kv : committed) {
		const ISquadTask* g = static_cast<const ISquadTask*>(kv.first);
		const AIFloat3& p = g->GetLeader()->GetPos(frame);
		const float sqDist = commPos.SqDistance2D(p);
		if ((sqDist < minSqDist) && isValid(p)) {
			minSqDist = sqDist;
			meetTask = g;
			meetPos = p;
		}
	}
}

} // namespace circuit

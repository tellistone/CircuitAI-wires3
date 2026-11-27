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
#include <cstdio>
#include <limits>
#include <set>

namespace circuit {

using namespace springai;

static constexpr int MIN_STATE_FRAMES = FRAMES_PER_SEC * 5;  // a go or no-go lasts at least this long
static constexpr int ATTACKER_FRAMES = FRAMES_PER_SEC * 5;  // an enemy that hit a commander is ranked first this long
static constexpr float COMMIT_MOD = 1.25f;  // groups are committed until groups + members + static cover reach this x danger
static constexpr float COMMIT_MARGIN = 0.25f;  // ... or the go line + this, if more (a go_ratio over 1.25 can still go)
static constexpr float KEEP_REACH_MOD = 1.5f;  // a committed group is dropped past this x respond_sec of travel
static constexpr float CORNERED_MOD = 0.6f;  // the go line x this once the commander is cornered or paralysed
static constexpr float PRESENT_SHARE = 0.5f;  // a group is present within this x zone of the goal
static constexpr float PRESENT_KEEP_SHARE = 0.625f;  // ... and stays present within this x zone
static constexpr size_t MAX_TURRET_SPOTS = 6;  // turret spots weighed as the meeting point per update
static constexpr float TURRET_SPOT_GAP = 150.f;  // turrets closer than this to a weighed spot count as that spot
static constexpr float AT_SPOT_DIST = DEFAULT_SLACK * 4;  // the commander stands at the meeting point within this
static constexpr float MEET_BACK_DIST = 100.f;  // the meeting point is this far behind its turret, away from the danger
static constexpr float MAX_TURRET_RANGE = 2000.f;  // static anti-air beyond this reach is no cover (IsStaticDefence's line)
static constexpr float ENGAGED_SLACK = 200.f;  // a group with a target out of the zone within its range + this is busy
static constexpr float PLANE_PASS_DIST = 300.f;  // a plane heads for the goal when its track passes this close
static constexpr int PLANE_HOLD_FRAMES = FRAMES_PER_SEC * 5;  // ... and counts this long after it last did
static constexpr float WARD_JOIN_DIST = 600.f;  // members closer than this are one ward
static constexpr float WARD_LEAVE_DIST = 800.f;  // ... and stay one until this far apart
static constexpr float SCORE_SEC = 3.f;  // the pick's score: ward value / (travel sec + this)
static constexpr float MOVE_SCORE_MOD = 2.f;  // a committed group not yet present moves to a ward scoring it this x higher
// The groups each side takes: ground (attack, raid and gathering) or air (anti-air, flying ones too)
static const std::vector<IFighterTask::FightType> GROUND_TYPES = {IFighterTask::FightType::ATTACK,
		IFighterTask::FightType::RAID, IFighterTask::FightType::DEFEND};
static const std::vector<IFighterTask::FightType> AIR_TYPES = {IFighterTask::FightType::AA};
// The tasks that are wards when they hold a unit with the protect attribute
static const std::vector<IFighterTask::FightType> WARD_TYPES = {IFighterTask::FightType::AH,
		IFighterTask::FightType::ARTY};

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

// A protected unit or group this update
struct CProtect::SMember {
	const void* key;  // the commander unit, or the task
	CCircuitUnit* comm;  // a commander, else nullptr
	CCircuitUnit* lead;  // the commander, the squad's leader or the artillery unit
	AIFloat3 pos;
	float value;  // metal cost
	float own;  // power: the commander's (its view), the squad's rally strength, the artillery's attack power
	bool isAntiAir;
	bool isDamaged;  // a commander hit in the last second
	CBuilderManager::SCommView view;  // commanders only
	int ward;  // AssignWards
};

CProtect::CProtect(CCircuitAI* circuit, const SConfig& cfg)
		: circuit(circuit)
		, cfg(cfg)
		, nextWardId(1)
		, isAnyGo(false)
		, turretFrame(-1)
{
}

CProtect::~CProtect()
{
}

void CProtect::OnCommDamaged(CCircuitUnit* comm, CEnemyInfo* attacker)
{
	if (!cfg.isEnabled || !comm->GetCircuitDef()->IsRoleComm()) {
		return;
	}
	const int frame = circuit->GetLastFrame();
	damaged[comm->GetId()] = frame;
	if (attacker != nullptr) {
		attackers[attacker->GetId()] = frame;
	}
}

const CProtect::SWard* CProtect::FindWard(const IFighterTask* task, Side* outSide) const
{
	auto it = committed.find(task);
	if (it == committed.end()) {
		return nullptr;
	}
	auto wt = wards.find(it->second.ward);
	if (wt == wards.end()) {
		return nullptr;
	}
	if (outSide != nullptr) {
		*outSide = it->second.side;
	}
	return &wt->second;
}

bool CProtect::IsActing(const IFighterTask* task) const
{
	Side side;
	const SWard* w = FindWard(task, &side);
	return (w != nullptr) && (w->sides[side].state == State::GO);
}

bool CProtect::IsZoneEnemy(const IFighterTask* task, const CEnemyInfo* enemy) const
{
	const SWard* w = FindWard(task);
	if ((w == nullptr) || (w->sides[GROUND].state != State::GO)) {
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
	return enemy->GetPos().SqDistance2D(w->goal) <= SQUARE(cfg.zone);
}

bool CProtect::IsRecentAttacker(ICoreUnit::Id id, int frame) const
{
	auto it = attackers.find(id);
	return (it != attackers.end()) && (frame - it->second <= ATTACKER_FRAMES);
}

AIFloat3 CProtect::GetGoal(const IFighterTask* task) const
{
	const SWard* w = FindWard(task);
	return (w != nullptr) ? w->goal : AIFloat3(-RgtVector);
}

AIFloat3 CProtect::GetMeetingPoint(const CCircuitUnit* comm) const
{
	auto it = memberWard.find(comm);
	if (it == memberWard.end()) {
		return -RgtVector;
	}
	auto wt = wards.find(it->second);
	return ((wt != wards.end()) && wt->second.isOn) ? wt->second.turretPos : AIFloat3(-RgtVector);
}

bool CProtect::IsAtMeetingPoint(const CCircuitUnit* comm, const AIFloat3& pos) const
{
	const AIFloat3 meet = GetMeetingPoint(comm);
	return utils::is_valid(meet) && (pos.SqDistance2D(meet) < SQUARE(AT_SPOT_DIST));
}

float CProtect::GetGoBoost(const IFighterTask* task, const AIFloat3& pos) const
{
	auto it = committed.find(task);
	if (it == committed.end()) {
		return 1.f;
	}
	auto wt = wards.find(it->second.ward);
	if ((wt == wards.end()) || (wt->second.sides[it->second.side].state != State::GO)
		|| !IsInFight(wt->second, pos, it->second.isPresent))
	{
		return 1.f;
	}
	// The strength of its ward's side there now (a group still on its way isn't), and itself wherever it is
	const float self = std::max(it->second.strength, 1e-3f);
	return std::max((wt->second.sides[it->second.side].presentSum + (it->second.isPresent ? 0.f : self)) / self, 1.f);
}

void CProtect::Transfer(const IFighterTask* from, const IFighterTask* to)
{
	auto it = committed.find(from);
	if (it != committed.end()) {
		const SCommit commit = it->second;
		committed.erase(it);
		committed.emplace(to, commit);  // already committed: kept (its strength is read again at the next update)
	}
	auto mt = memberWard.find(from);  // a merged protected squad: its ward goes on with the one it merged into
	if (mt != memberWard.end()) {
		const int id = mt->second;
		memberWard.erase(mt);
		memberWard.emplace(to, id);
	}
}

void CProtect::OnTaskEnd(const IFighterTask* task)
{
	memberWard.erase(task);
	auto it = committed.find(task);
	if (it == committed.end()) {
		return;
	}
	auto wt = wards.find(it->second.ward);
	if (wt != wards.end()) {
		SSide& s = wt->second.sides[it->second.side];
		s.committedSum -= it->second.strength;
		if (it->second.isPresent) {
			s.presentSum -= it->second.strength;
		}
		--s.count;
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
	EraseOlder(damaged, frame, FRAMES_PER_SEC);

	// 0. Wards: the members, clustered
	static std::vector<SMember> members;  // NOTE: micro-opt
	CollectMembers(frame, members);
	AssignWards(members);
	goals.clear();
	std::map<int, std::vector<const SMember*>> byWard;
	for (const SMember& m : members) {
		goals.push_back(m.pos);
		byWard[m.ward].push_back(&m);
	}

	// 1-3. Each ward: anchor, danger by side, turret point
	CollectPlanes();
	for (auto& kv : byWard) {
		UpdateWard(wards[kv.first], kv.second, frame);
	}

	// 4. The pick over the wards that are on. Air first: its go answers the gunships, which the ground side weighs
	// otherwise. Only a go's groups move, so in a no-go only the anti-air turrets and the members answer them
	static std::vector<SWard*> on;  // NOTE: micro-opt
	on.clear();
	for (auto& kv : wards) {
		if (kv.second.isOn) {
			on.push_back(&kv.second);
		}
	}
	if (!on.empty()) {
		Pick(AIR, frame, on);
		for (SWard* w : on) {
			const SSide& air = w->sides[AIR];
			w->sides[GROUND].spill = GetGunshipSpill(*w, air.own + air.cover
					+ ((air.state == State::GO) ? air.committedSum : 0.f));
		}
		Pick(GROUND, frame, on);
	}
	isAnyGo = std::any_of(on.begin(), on.end(), [](const SWard* w) { return w->IsGo(); });
}

void CProtect::CollectMembers(int frame, std::vector<SMember>& out)
{
	// Our commanders with the builder flee (their danger view), and anti-heavy squads and artillery tasks holding a
	// unit with the protect attribute (not flying ones: the danger list is of enemies that hit ground)
	out.clear();
	CBuilderManager* builderMgr = circuit->GetBuilderManager();
	for (auto& kv : circuit->GetTeamUnits()) {
		CCircuitUnit* unit = kv.second;
		CCircuitDef* cdef = unit->GetCircuitDef();
		if (!cdef->IsRoleComm() || unit->IsDead()) {
			continue;
		}
		SMember m;
		if (!builderMgr->GetCommView(unit, cfg.zone, m.view)) {
			continue;
		}
		m.key = unit;
		m.comm = m.lead = unit;
		m.pos = unit->GetPos(frame);
		m.value = cdef->GetCostM();
		m.own = m.view.own;
		m.isAntiAir = cdef->HasAntiAir();
		m.isDamaged = damaged.find(unit->GetId()) != damaged.end();
		out.push_back(m);
	}
	CMilitaryManager* militaryMgr = circuit->GetMilitaryManager();
	for (IFighterTask::FightType type : WARD_TYPES) {
		for (IFighterTask* t : militaryMgr->GetTasks(type)) {
			const std::set<CCircuitUnit*>& units = t->GetAssignees();
			if (units.empty() || std::none_of(units.begin(), units.end(), [](CCircuitUnit* u) {
				return u->GetCircuitDef()->IsAttrProtect();
			})) {
				continue;
			}
			const bool isSquad = (type == IFighterTask::FightType::AH);  // artillery tasks hold one unit
			CCircuitUnit* lead = isSquad ? static_cast<ISquadTask*>(t)->GetLeader() : *units.begin();
			if ((lead == nullptr) || lead->GetCircuitDef()->IsAbleToFly() || t->IsDead()) {
				continue;
			}
			SMember m;
			m.key = t;
			m.comm = nullptr;
			m.lead = lead;
			m.pos = lead->GetPos(frame);
			m.value = m.own = 0.f;
			m.isAntiAir = false;
			for (CCircuitUnit* u : units) {
				m.value += u->GetCircuitDef()->GetCostM();
				m.own += IFighterTask::GetCoverPower(u);  // health-scaled power, as squad strength is
				m.isAntiAir |= u->GetCircuitDef()->HasAntiAir();
			}
			m.isDamaged = false;  // a hit isn't a rush for a group (far artillery): only the force balance starts it
			out.push_back(m);
		}
	}
}

void CProtect::AssignWards(std::vector<SMember>& members)
{
	// Members within WARD_JOIN_DIST are one ward; members of one ward stay so until WARD_LEAVE_DIST apart. A cluster
	// keeps a ward id among its members', else gets a new one. A ward left with no member ends
	const size_t n = members.size();
	std::vector<int> prev(n);
	std::vector<size_t> root(n);
	for (size_t i = 0; i < n; ++i) {
		auto it = memberWard.find(members[i].key);
		prev[i] = (it != memberWard.end()) ? it->second : -1;
		root[i] = i;
	}
	auto find = [&root](size_t i) {
		while (root[i] != i) {
			root[i] = root[root[i]];
			i = root[i];
		}
		return i;
	};
	for (size_t i = 0; i < n; ++i) {
		for (size_t j = i + 1; j < n; ++j) {
			const float sqDist = members[i].pos.SqDistance2D(members[j].pos);
			const bool isSame = (prev[i] >= 0) && (prev[i] == prev[j]);
			if ((sqDist < SQUARE(WARD_JOIN_DIST)) || (isSame && (sqDist < SQUARE(WARD_LEAVE_DIST)))) {
				root[find(i)] = find(j);
			}
		}
	}
	std::map<size_t, std::vector<size_t>> clusterOf;
	for (size_t i = 0; i < n; ++i) {
		clusterOf[find(i)].push_back(i);
	}
	// The order ids are kept in: clusters with a commander first (a ward split from its commander starts afresh, not
	// the commander's protect), then the larger
	std::vector<std::pair<int, const std::vector<size_t>*>> clusters;
	for (auto& kv : clusterOf) {
		const int comms = std::count_if(kv.second.begin(), kv.second.end(), [&members](size_t i) {
			return members[i].comm != nullptr;
		});
		clusters.emplace_back((comms > 0) ? 1 : 0, &kv.second);
	}
	std::stable_sort(clusters.begin(), clusters.end(), [](const auto& a, const auto& b) {
		return (a.first > b.first) || ((a.first == b.first) && (a.second->size() > b.second->size()));
	});
	// A cluster keeps the id of its members' ward in a go first, then on, then the oldest: a threatened commander
	// walking past its idle artillery keeps its protect
	auto getRank = [this](int id) {
		const SWard& w = wards.at(id);
		return w.isOn ? (w.IsGo() ? 2 : 1) : 0;
	};
	std::set<int> taken;
	for (const auto& kc : clusters) {
		const std::vector<size_t>* c = kc.second;
		int id = -1;
		for (size_t i : *c) {
			if ((prev[i] < 0) || (taken.find(prev[i]) != taken.end()) || (wards.find(prev[i]) == wards.end())) {
				continue;
			}
			if ((id < 0) || (getRank(prev[i]) > getRank(id)) || ((getRank(prev[i]) == getRank(id)) && (prev[i] < id))) {
				id = prev[i];
			}
		}
		if (id < 0) {
			id = nextWardId++;
			wards[id].id = id;
		}
		taken.insert(id);
		for (size_t i : *c) {
			members[i].ward = id;
		}
	}
	memberWard.clear();
	for (const SMember& m : members) {
		memberWard[m.key] = m.ward;
	}
	const int frame = circuit->GetLastFrame();
	for (auto it = wards.begin(); it != wards.end();) {
		if (taken.find(it->first) != taken.end()) {
			++it;
			continue;
		}
		if (it->second.isOn) {
			End(it->second, "gone", frame);
		}
		it = wards.erase(it);
	}
}

// A ward's members summed this update
struct CProtect::SSums {
	const SMember* lead = nullptr;  // the costliest commander, else the costliest member
	const SMember* listFrom = nullptr;  // the commander with the largest danger list
	bool hasGroup = false;
	bool isCommThreat = false;  // a commander's list beats it
	bool isDamaged = false;  // a commander hit in the last second
	float own = 0.f;
	float airOwn = 0.f;  // those that hit air
};

void CProtect::SumMembers(SWard& w, const std::vector<const SMember*>& members, SSums& out) const
{
	out = SSums();
	w.value = 0.f;
	for (const SMember* m : members) {
		w.value += m->value;
		out.own += m->own;
		if (m->isAntiAir) {
			out.airOwn += m->own;
		}
		if (m->comm != nullptr) {
			if ((out.lead == nullptr) || (out.lead->comm == nullptr) || (m->value > out.lead->value)) {
				out.lead = m;
			}
			if ((out.listFrom == nullptr) || (m->view.danger > out.listFrom->view.danger)) {
				out.listFrom = m;
			}
			out.isCommThreat |= (m->view.danger > m->view.own);
			out.isDamaged |= m->isDamaged;
		} else {
			out.hasGroup = true;
			if ((out.lead == nullptr) || ((out.lead->comm == nullptr) && (m->value > out.lead->value))) {
				out.lead = m;
			}
		}
	}
	w.comm = out.lead->comm;
	w.goal = out.lead->pos;
	w.name = out.lead->lead->GetCircuitDef()->GetDef()->GetName();
	w.memberCount = members.size();
}

void CProtect::UpdateWard(SWard& w, const std::vector<const SMember*>& members, int frame)
{
	SSums sums;
	SumMembers(w, members, sums);
	UpdateDeathTime(w, frame);
	UpdatePlanes(w, frame);
	if (w.planeDanger > 0.f) {
		CollectTurrets(frame);  // for the bubble
	}

	// 1. Anchor: a commander's seen danger beats it, or it took damage since the last update; a group member is
	// outnumbered at the goal by the force balance's own line (attack_balance.ratio; our army standing by holds it);
	// or planes heading for the goal beat its anti-air bubble (its anti-air members' power, and the anti-air turrets
	// covering it)
	CAttackBalance* balance = circuit->GetMilitaryManager()->GetAttackBalance();
	CAttackBalance::SForce force;
	bool isForce = false;
	auto getForce = [&]() {
		if (!isForce) {
			balance->GetForceAt(w.goal, nullptr, sums.lead->lead->GetCircuitDef()->IsAmphibious(), force);
			isForce = true;
		}
	};
	bool isOutnumbered = false;
	if (sums.hasGroup) {
		getForce();
		const float line = balance->IsOn() ? balance->GetRatioLine() : 1.f;
		isOutnumbered = (force.enemy > 0.f) && (force.GetRatio() >= line);
	}
	const bool isPlaneThreat = (w.planeDanger > 0.f)
			&& (w.planeDanger > sums.airOwn + GetStaticCover(w.goal, true, 0.f));  // bombs land at the plane's spot
	const bool isThreat = sums.isCommThreat || sums.isDamaged || isOutnumbered || isPlaneThreat;
	if (isThreat) {
		w.lastDangerFrame = frame;
	}
	const bool isStart = !w.isOn;
	if (isStart) {
		if (!isThreat) {
			return;
		}
		Start(w, frame);
	} else if (frame - w.lastDangerFrame > cfg.keepFrames) {
		End(w, "clear", frame);
		return;
	}
	CollectTurrets(frame);
	getForce();
	if ((sums.listFrom != nullptr) && utils::is_valid(sums.listFrom->view.dangerPos)) {
		w.dangerPos = sums.listFrom->view.dangerPos;
		w.dangerRange = sums.listFrom->view.dangerRange;
	}
	if (w.comm != nullptr) {
		w.isCornered |= sums.lead->view.isCornered || w.comm->GetUnit()->IsParalyzed();
	}
	UpdateSides(w, sums, force.enemy - force.air, force.air, isStart, isOutnumbered);
	w.goLine = cfg.goRatio * (w.isCornered ? CORNERED_MOD : 1.f);
}

void CProtect::UpdateSides(SWard& w, const SSums& sums, float forceGround, float forceAir, bool isStart,
		bool isOutnumbered)
{
	// 2. Danger by side. Ground: its commanders' list without its gunships, or the force balance's enemy force at the
	// goal (statics included) without its aircraft, whichever is more. Air: the list's gunships (a group ward: the
	// force's aircraft) and the planes heading for it
	SSide& ground = w.sides[GROUND];
	SSide& air = w.sides[AIR];
	const float listDanger = (sums.listFrom != nullptr) ? sums.listFrom->view.danger : 0.f;
	const float listAir = (sums.listFrom != nullptr) ? sums.listFrom->view.airDanger : 0.f;
	const float listGround = std::max(listDanger - listAir, 0.f);
	ground.danger = std::max(listGround, forceGround);
	w.gunshipDanger = (sums.listFrom != nullptr) ? listAir : forceAir;
	air.danger = w.gunshipDanger + w.planeDanger;
	if (ground.danger + air.danger > 0.f) {  // kept while both are empty (logged)
		w.airShare = air.danger / (ground.danger + air.danger);
		w.peakAirShare = std::max(w.peakAirShare, w.airShare);
	}
	ground.own = sums.own;
	air.own = sums.airOwn;
	if (isStart) {
		circuit->LOG("PROTECT start [%s]: power %.1f, ground %.1f (force %.1f, list %.1f), air %.1f (gunships %.1f, "
				"bombers %i: %.1f)%s%s at (%.0f, %.0f)", GetLabel(w).c_str(), sums.own, ground.danger, forceGround,
				listGround, air.danger, w.gunshipDanger, w.planeCount, w.planeDanger, sums.isDamaged ? ", damaged" : "",
				isOutnumbered ? ", outnumbered" : "", w.goal.x, w.goal.z);
	}

	// 3. Turret point (a commander ward)
	ground.commCover = GetStaticCover(w.goal, false, w.dangerRange);
	air.commCover = GetStaticCover(w.goal, true, w.dangerRange);
	if (w.comm != nullptr) {
		UpdateTurretPoint(w);
	} else {
		w.turretPos = -RgtVector;
		ground.cover = ground.commCover;
		air.cover = air.commCover;
	}
}

float CProtect::GetGunshipSpill(const SWard& w, float airAnswer) const
{
	// The gunships among the air part that airAnswer leaves unanswered. Bombers don't spill: they make one pass, and
	// ground groups can't shoot them
	const float airDanger = w.sides[AIR].danger;
	return (airDanger > 0.f) ? std::max(airDanger - airAnswer, 0.f) * w.gunshipDanger / airDanger : 0.f;
}

void CProtect::ResetWard(SWard& w)
{
	for (SSide& s : w.sides) {
		s = SSide();
		s.stateFrame = -MIN_STATE_FRAMES;  // the first decision is at once
	}
	w.isCornered = false;
	w.airShare = w.peakAirShare = 0.f;
	w.turretPos = w.dangerPos = -RgtVector;
	w.dangerRange = 0.f;
}

void CProtect::Start(SWard& w, int frame)
{
	w.isOn = true;
	w.startFrame = frame;
	ResetWard(w);
}

void CProtect::End(SWard& w, const char* reason, int frame)
{
	// One summary per protect, gone or not
	circuit->LOG("PROTECT end (%s) [%s] after %.0f s: goes %i ground, %i air; aircraft up to %.0f%%", reason,
			GetLabel(w).c_str(), (frame - w.startFrame) / float(FRAMES_PER_SEC), w.sides[GROUND].goCount,
			w.sides[AIR].goCount, w.peakAirShare * 100.f);
	w.isOn = false;
	for (auto it = committed.begin(); it != committed.end();) {
		it = (it->second.ward == w.id) ? committed.erase(it) : std::next(it);
	}
	ResetWard(w);
}

std::string CProtect::GetLabel(const SWard& w) const
{
	char buf[128];
	if (w.memberCount > 1) {
		snprintf(buf, sizeof(buf), "ward %i: %s +%i, %.0f m", w.id, w.name, w.memberCount - 1, w.value);
	} else {
		snprintf(buf, sizeof(buf), "ward %i: %s, %.0f m", w.id, w.name, w.value);
	}
	return buf;
}

void CProtect::SetState(SWard& w, Side side, State value, int frame)
{
	SSide& s = w.sides[side];
	char spill[48] = "";
	if (side == GROUND) {
		snprintf(spill, sizeof(spill), " + unanswered gunships %.1f", s.spill);
	}
	circuit->LOG("PROTECT %s (%s) [%s]: ratio %.2f (groups %.1f + members %.1f + static %.1f vs danger %.1f%s), %i "
			"groups (present %.1f), aircraft %.0f%%%s%s, death in %.0f s", (value == State::GO) ? "go" : "nogo",
			(side == AIR) ? "air" : "ground", GetLabel(w).c_str(), std::min(s.ratio, 99.f), s.committedSum, s.own,
			s.cover, s.danger, spill, s.count, s.presentSum, w.airShare * 100.f, w.isCornered ? ", cornered" : "",
			utils::is_valid(w.turretPos) ? ", at turrets" : "", std::min(w.deathSec, 999.f));
	if (value == State::GO) {
		++s.goCount;
	}  // a no-go's groups are inert at once (IsActing, GetGoBoost) and dropped at the next update
	s.state = value;
	s.stateFrame = frame;
}

void CProtect::UpdateDeathTime(SWard& w, int frame)
{
	// The lead commander's damage rate, smoothed over about two updates; regeneration and repair lower it. A group
	// ward has none: a squad losing a unit would read as a sudden drop, just when help is needed
	if (w.comm == nullptr) {
		w.healthId = -1;
		w.prevHealth = -1.f;
		w.dps = 0.f;
		w.deathSec = std::numeric_limits<float>::max();
		return;
	}
	if (w.healthId != w.comm->GetId()) {
		w.healthId = w.comm->GetId();
		w.prevHealth = -1.f;
		w.dps = 0.f;
	}
	const float health = w.comm->GetUnit()->GetHealth();
	if ((w.prevHealth >= 0.f) && (frame > w.prevHealthFrame)) {
		const float rate = std::max(w.prevHealth - health, 0.f) * FRAMES_PER_SEC / (frame - w.prevHealthFrame);
		w.dps = 0.5f * (w.dps + rate);
	}
	w.prevHealth = health;
	w.prevHealthFrame = frame;
	w.deathSec = (w.dps > 1.f) ? health / w.dps : std::numeric_limits<float>::max();
}

void CProtect::CollectPlanes()
{
	// Seen enemy planes with a ground or water weapon (bombers; gunships are in the commander's list) that can act
	enemyPlanes.clear();
	for (auto& kv : circuit->GetEnemyInfos()) {
		const CEnemyInfo* enemy = kv.second;
		const CCircuitDef* edef = enemy->GetCircuitDef();
		if ((edef != nullptr) && edef->IsPlane() && (edef->HasAntiLand() || edef->HasAntiWater())
			&& !enemy->IsHidden() && enemy->IsInRadarOrLOS() && !enemy->IsBeingBuilt() && !enemy->IsParalyzed()
			&& !enemy->IsDisarmed())
		{
			enemyPlanes.push_back(enemy);
		}
	}
}

void CProtect::UpdatePlanes(SWard& w, int frame)
{
	// Those planes within the zone, heading for the goal (the track ahead passes within PLANE_PASS_DIST) or that hit
	// a commander in the last 5 s, and held PLANE_HOLD_FRAMES after either, so a bomber turning or lining up doesn't
	// drop in and out each second. At their threat before thr_mod: bombers' thr_mod (0.01..0.1) keeps the threat
	// map's readers from shying away from planes overhead, and would hide them here
	w.planeDanger = 0.f;
	w.planeCount = 0;
	EraseOlder(w.planes, frame, PLANE_HOLD_FRAMES);
	const float sqZone = SQUARE(cfg.zone);
	for (const CEnemyInfo* enemy : enemyPlanes) {
		const AIFloat3 toGoal = w.goal - enemy->GetPos();
		if (toGoal.SqLength2D() > sqZone) {
			continue;
		}
		if (IsRecentAttacker(enemy->GetId(), frame) || IsHeadingFor(toGoal, enemy->GetVel())) {
			w.planes[enemy->GetId()] = frame;
		} else if (w.planes.find(enemy->GetId()) == w.planes.end()) {
			continue;
		}
		w.planeDanger += enemy->GetCircuitDef()->GetRawThreat();
		++w.planeCount;
	}
}

/*
 * Static cover: our and allied finished turrets that hit what the danger is and reach the defence of a spot
 */

void CProtect::CollectTurrets(int frame)
{
	// Once per update, around every member
	if (turretFrame == frame) {
		return;
	}
	turretFrame = frame;
	turrets.clear();
	const float sqReach = SQUARE(cfg.zone + 600.f);
	for (auto& kv : circuit->GetFriendlyUnits()) {
		CAllyUnit* ally = kv.second;
		CCircuitDef* adef = ally->GetCircuitDef();
		if ((adef == nullptr) || adef->IsMobile() || !adef->IsAttacker()) {
			continue;
		}
		// Against ground: a defence by the threat map's rule (no static AA, no superweapon or long-range artillery).
		// Against aircraft: anti-air within the same reach. Both kinds: the plane bubble is checked before the start
		const bool isDefence = IFighterTask::IsStaticDefence(adef);
		const bool isAntiAir = adef->HasAntiAir() && (adef->GetMaxRange(CCircuitDef::RangeType::AIR) <= MAX_TURRET_RANGE);
		if (!isDefence && !isAntiAir) {
			continue;
		}
		const AIFloat3& allyPos = ally->GetPos(frame);
		if (!utils::is_valid(allyPos) || std::none_of(goals.begin(), goals.end(), [&allyPos, sqReach](const AIFloat3& g) {
			return allyPos.SqDistance2D(g) <= sqReach;
		})) {
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
	// A turret counts when it hits the side's kind (aircraft: anti-air only) and its range covers the spot plus half
	// the attacker's range, where an enemy attacking there stands on average
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

AIFloat3 CProtect::GetBehind(const SWard& w, const AIFloat3& turret) const
{
	// Behind the turret line rather than in it: the turrets take the first shots
	if (!utils::is_valid(w.dangerPos)) {
		return turret;
	}
	AIFloat3 dir = turret - w.dangerPos;
	dir.y = 0.f;
	const float dist = dir.Length2D();
	if (dist < 1.f) {
		return turret;
	}
	AIFloat3 spot = turret + dir * (MEET_BACK_DIST / dist);
	CTerrainManager::CorrectPosition(spot);
	return circuit->GetTerrainManager()->CanMoveToPos(w.comm->GetArea(), spot) ? spot : turret;
}

float CProtect::GetSpotRatio(const SWard& w, const AIFloat3& pos) const
{
	// The turrets covering pos and the members against the danger. With a ground part: ground turrets and their
	// power against it plus the gunships that the anti-air turrets there leave unanswered (the ground side's spill).
	// Air only: anti-air turrets and the anti-air power against it. Nothing to hold: 0
	const SSide& ground = w.sides[GROUND];
	const SSide& air = w.sides[AIR];
	const float airHold = air.own + GetStaticCover(pos, true, w.dangerRange);
	if (ground.danger > 0.f) {
		return (ground.own + GetStaticCover(pos, false, w.dangerRange)) / (ground.danger + GetGunshipSpill(w, airHold));
	}
	return (air.danger > 0.f) ? airHold / air.danger : 0.f;
}

void CProtect::SetCover(SWard& w, const AIFloat3& pos)
{
	w.sides[GROUND].cover = GetStaticCover(pos, false, w.dangerRange);
	w.sides[AIR].cover = GetStaticCover(pos, true, w.dangerRange);
}

bool CProtect::IsTurretSpotValid(const SWard& w, const AIFloat3& spot) const
{
	// Strong enough with the members. Once the lead commander stands there, that is all: the danger follows it into
	// the turrets. On the way: reached before its time to death, away from the danger (PickFleeSpot's rule) and a
	// safe flee spot
	if (GetSpotRatio(w, spot) < 1.f) {
		return false;
	}
	const float sqDist = w.goal.SqDistance2D(spot);
	if (sqDist < SQUARE(AT_SPOT_DIST)) {
		return true;
	}
	const float speed = std::max(w.comm->GetCircuitDef()->GetSpeed(), 1.f);
	if (sqrtf(sqDist) / speed >= w.deathSec) {
		return false;
	}
	if (utils::is_valid(w.dangerPos) && (spot.SqDistance2D(w.dangerPos) <= w.goal.SqDistance2D(w.dangerPos))) {
		return false;
	}
	return circuit->GetBuilderManager()->IsCommFleeSpot(w.comm, spot);
}

bool CProtect::IsTurretSpotHeld(const SWard& w, const AIFloat3& spot) const
{
	// Chasers closing in on the turrets don't drop it: that is where the turrets shoot them
	return (GetSpotRatio(w, spot) >= 1.f / circuit->GetMilitaryManager()->GetAttackKeepThreat())
			&& circuit->GetTerrainManager()->CanMoveToPos(w.comm->GetArea(), spot);
}

void CProtect::UpdateTurretPoint(SWard& w)
{
	// Kept while it holds (IsTurretSpotHeld), replaced only when it fails: the spot behind the nearest turret group
	// near the goal that passes IsTurretSpotValid, with the cover where the commander would stand
	if (utils::is_valid(w.turretPos)) {
		if (IsTurretSpotHeld(w, w.turretPos)) {
			SetCover(w, w.turretPos);
			return;
		}
		w.turretPos = -RgtVector;
	}
	for (SSide& s : w.sides) {
		s.cover = s.commCover;  // turrets that already cover the goal
	}
	if (turrets.empty()) {
		return;
	}
	const AIFloat3 goal = w.goal;
	std::sort(turrets.begin(), turrets.end(), [&goal](const STurret& a, const STurret& b) {
		return a.pos.SqDistance2D(goal) < b.pos.SqDistance2D(goal);
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
		const AIFloat3 meet = GetBehind(w, s);
		if (IsTurretSpotValid(w, meet)) {
			w.turretPos = meet;
			SetCover(w, meet);
			return;
		}
	}
}

/*
 * The pick
 */

bool CProtect::IsInFight(const SWard& w, const AIFloat3& pos, bool wasPresent) const
{
	const float radius = cfg.zone * (wasPresent ? PRESENT_KEEP_SHARE : PRESENT_SHARE);
	return pos.SqDistance2D(w.goal) <= SQUARE(radius);
}

float CProtect::GetTravelSec(const ISquadTask* task, const AIFloat3& goal, int frame) const
{
	return task->GetLeader()->GetPos(frame).distance2D(goal) / std::max(task->GetLowestSpeed(), 1.f);
}

float CProtect::GetReachSec(const ISquadTask* task, const SWard& w, int frame) const
{
	const float travelSec = GetTravelSec(task, w.goal, frame);
	return ((travelSec <= std::min(cfg.respondSec, w.deathSec))
			&& circuit->GetTerrainManager()->CanMoveToPos(task->GetLeader()->GetArea(), w.goal)) ? travelSec : -1.f;
}

void CProtect::CollectCandidates(bool isAir, int frame, const std::vector<SWard*>& on, std::vector<SCandidate>& out) const
{
	// Groups of the side's kind (GROUND_TYPES: attack, raid and gathering attack- or raid-bound ones, no flying
	// leader; AIR_TYPES: anti-air, flying ones too), not committed or fleeing, with the wards each can answer: reached
	// in time (respond_sec, or the ward's time to death if shorter), and not busy fighting outside its zone
	out.clear();
	CMilitaryManager* militaryMgr = circuit->GetMilitaryManager();
	const float sqZone = SQUARE(cfg.zone);
	for (IFighterTask::FightType type : isAir ? AIR_TYPES : GROUND_TYPES) {
		for (IFighterTask* t : militaryMgr->GetTasks(type)) {
			ISquadTask* g = static_cast<ISquadTask*>(t);  // attack, raid, defend and anti-air tasks are squads
			CCircuitUnit* leader = g->GetLeader();
			if (g->IsDead() || (leader == nullptr) || (!isAir && leader->GetCircuitDef()->IsAbleToFly())
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
			const AIFloat3& pos = leader->GetPos(frame);
			const CEnemyInfo* target = g->GetTarget();
			const float sqEngaged = SQUARE(g->GetHighestRange() + ENGAGED_SLACK);
			SCandidate c;
			c.task = g;
			c.travelSec.assign(on.size(), -1.f);
			bool isAny = false;
			for (size_t i = 0; i < on.size(); ++i) {
				const SWard* w = on[i];
				const float travelSec = GetReachSec(g, *w, frame);
				if (travelSec < 0.f) {
					continue;
				}
				if ((target != nullptr) && (target->GetPos().SqDistance2D(w->goal) > sqZone)
					&& (target->GetPos().SqDistance2D(pos) < sqEngaged))
				{
					continue;
				}
				c.travelSec[i] = travelSec;
				isAny = true;
			}
			if (isAny) {
				out.push_back(std::move(c));
			}
		}
	}
}

void CProtect::Refresh(const ISquadTask* task, int frame, SCommit& commit, SWard& w)
{
	// Its strength now. A fleeing group isn't present (no boost to the others), but still counts for keeping a go: a
	// short flee on the way in doesn't release every group. A go starts only with groups that aren't fleeing
	// (CollectCandidates skips them)
	SSide& s = w.sides[commit.side];
	commit.strength = task->GetRallyStrength();
	commit.isPresent = !task->IsFleeing(frame) && IsInFight(w, task->GetLeader()->GetPos(frame), commit.isPresent);
	s.committedSum += commit.strength;
	if (commit.isPresent) {
		s.presentSum += commit.strength;
	}
	++s.count;
}

void CProtect::Fill(Side side, State state, int frame, const std::vector<SWard*>& on, std::vector<SCandidate>& pool,
		const SWard* only)
{
	// The best pair first, until no ward in state is short of its need or no group is left: the soonest there for one
	// ward, the most valuable for groups that could answer several
	while (true) {
		SCandidate* best = nullptr;
		SWard* bestWard = nullptr;
		float bestScore = -1.f;
		for (SCandidate& c : pool) {
			if (c.ward != nullptr) {
				continue;
			}
			for (size_t i = 0; i < on.size(); ++i) {
				SWard* w = on[i];
				const SSide& s = w->sides[side];
				if ((s.state != state) || w->isReleased || ((only != nullptr) && (w != only)) || (c.travelSec[i] < 0.f)
					|| (s.GetSum() >= s.need))
				{
					continue;
				}
				const float score = w->value / (c.travelSec[i] + SCORE_SEC);
				if (score > bestScore) {
					best = &c;
					bestWard = w;
					bestScore = score;
				}
			}
		}
		if (best == nullptr) {
			return;
		}
		best->ward = bestWard;
		SCommit& commit = committed[best->task];
		commit = SCommit();
		commit.ward = bestWard->id;
		commit.side = side;
		Refresh(best->task, frame, commit, *bestWard);
	}
}

void CProtect::Pick(Side side, int frame, const std::vector<SWard*>& on)
{
	// 1. Each ward's need: its part and the spill, the spill at most as much again (the unanswered gunships shoot the
	// ground groups too, but gunships alone, or far past the ground part, pull no ground groups in). Its committed
	// groups: in a go they stay (latched), unless dead or gone out of reach (a flee to a haven, our base or a far
	// cluster; past KEEP_REACH_MOD x respond_sec of travel, so a group near the join line doesn't flap); in a no-go
	// they are picked afresh. The go is judged with all of them; the boost only with those present (GetGoBoost)
	for (SWard* w : on) {
		SSide& s = w->sides[side];
		s.need = (s.danger + std::min(s.spill, s.danger)) * std::max(COMMIT_MOD, w->goLine + COMMIT_MARGIN);
		s.ClearSums();
		w->isReleased = false;
	}
	const float maxReachSec = cfg.respondSec * KEEP_REACH_MOD;
	for (auto it = committed.begin(); it != committed.end();) {
		SCommit& c = it->second;
		if (c.side != side) {
			++it;
			continue;
		}
		auto wt = wards.find(c.ward);
		const ISquadTask* g = static_cast<const ISquadTask*>(it->first);  // an ended task is gone already (OnTaskEnd)
		if ((wt == wards.end()) || !wt->second.isOn || (wt->second.sides[side].state != State::GO) || g->IsDead()
			|| (g->GetLeader() == nullptr) || (GetTravelSec(g, wt->second.goal, frame) > maxReachSec))
		{
			it = committed.erase(it);
			continue;
		}
		Refresh(g, frame, c, wt->second);
		++it;
	}

	// 2. The fill: wards in a go first (their fight is on), then the others
	static std::vector<SCandidate> pool;  // NOTE: micro-opt
	CollectCandidates(side == AIR, frame, on, pool);
	Fill(side, State::GO, frame, on, pool);
	Fill(side, State::NOGO, frame, on, pool);
	if (on.size() > 1) {
		// 3. A ward in a no-go still under its go line hands its groups back, the lowest value first, and the others
		// fill again: two wards splitting the same groups would otherwise both stay out. Then those handed back try
		// again alone, the highest value first, from the groups left: one may go where the other couldn't
		static std::vector<SWard*> released;  // NOTE: micro-opt
		released.clear();
		while (true) {
			SWard* worst = nullptr;
			for (SWard* w : on) {
				const SSide& s = w->sides[side];
				if ((s.state == State::NOGO) && !w->isReleased && (s.count > 0) && (s.GetRatio() < w->goLine)
					&& ((worst == nullptr) || (w->value < worst->value)))
				{
					worst = w;
				}
			}
			if (worst == nullptr) {
				break;
			}
			Release(*worst, side, pool);
			released.push_back(worst);
			Fill(side, State::GO, frame, on, pool);
			Fill(side, State::NOGO, frame, on, pool);
		}
		std::sort(released.begin(), released.end(), [](const SWard* a, const SWard* b) { return a->value > b->value; });
		for (SWard* w : released) {
			w->isReleased = false;
			Fill(side, State::NOGO, frame, on, pool, w);
			if (w->sides[side].GetRatio() < w->goLine) {
				Release(*w, side, pool);
			}
		}

		// 4. Moves, after the fill so a hand-back never erases a moved group
		MoveGroups(side, frame, on);
	}

	// 5. Go or no-go, each for MIN_STATE_FRAMES at least. A go needs a group: with none there is nothing to send, and
	// a no-go costs nothing (a poke with no danger would otherwise go at once and hold off a real no-go)
	const float keepThreat = circuit->GetMilitaryManager()->GetAttackKeepThreat();
	for (SWard* w : on) {
		SSide& s = w->sides[side];
		s.ratio = s.GetRatio();
		if (frame - s.stateFrame < MIN_STATE_FRAMES) {
			continue;
		}
		if ((s.state == State::NOGO) && (s.ratio >= w->goLine) && (s.count > 0)) {
			SetState(*w, side, State::GO, frame);
		} else if ((s.state == State::GO) && (s.ratio < w->goLine / keepThreat)) {
			SetState(*w, side, State::NOGO, frame);
		}
	}
}

void CProtect::Release(SWard& w, Side side, std::vector<SCandidate>& pool)
{
	w.isReleased = true;
	for (auto it = committed.begin(); it != committed.end();) {
		const bool isOurs = (it->second.ward == w.id) && (it->second.side == side);
		it = isOurs ? committed.erase(it) : std::next(it);
	}
	for (SCandidate& c : pool) {
		if (c.ward == &w) {
			c.ward = nullptr;
		}
	}
	w.sides[side].ClearSums();
}

void CProtect::MoveGroups(Side side, int frame, const std::vector<SWard*>& on)
{
	// A group latched to a go and not yet present moves, once, to a ward short of its need that scores it
	// MOVE_SCORE_MOD x higher (a commander rushed after the groups were latched to a Lance group) and is in a go, or
	// reaches its go line with it, while the go it leaves stays above its keep line without it
	const float keepThreat = circuit->GetMilitaryManager()->GetAttackKeepThreat();
	for (auto& kv : committed) {
		SCommit& c = kv.second;
		if ((c.side != side) || c.isPresent || c.isMoved) {
			continue;
		}
		SWard& from = wards.at(c.ward);
		SSide& fs = from.sides[side];
		const float judged = fs.danger + fs.spill;
		if ((fs.state != State::GO)
			|| ((judged > 0.f) && ((fs.GetSum() - c.strength) / judged < from.goLine / keepThreat)))
		{
			continue;
		}
		const ISquadTask* g = static_cast<const ISquadTask*>(kv.first);
		float bestScore = MOVE_SCORE_MOD * from.value / (GetTravelSec(g, from.goal, frame) + SCORE_SEC);
		SWard* best = nullptr;
		for (SWard* w : on) {
			// Short of its need, and in a go or lifted to its go line by the group: else the group would stop acting,
			// come back next second and move again
			const SSide& s = w->sides[side];
			const float judged = s.danger + s.spill;
			if ((w == &from) || (s.GetSum() >= s.need) || ((s.state != State::GO)
				&& ((judged <= 0.f) || ((s.GetSum() + c.strength) / judged < w->goLine))))
			{
				continue;
			}
			const float travelSec = GetReachSec(g, *w, frame);
			if (travelSec < 0.f) {
				continue;
			}
			const float score = w->value / (travelSec + SCORE_SEC);
			if (score >= bestScore) {
				best = w;
				bestScore = score;
			}
		}
		if (best == nullptr) {
			continue;
		}
		circuit->LOG("PROTECT move (%s): a group of %.1f from [%s] to [%s]", (side == AIR) ? "air" : "ground",
				c.strength, GetLabel(from).c_str(), GetLabel(*best).c_str());
		fs.committedSum -= c.strength;
		--fs.count;
		SSide& bs = best->sides[side];
		bs.committedSum += c.strength;
		++bs.count;
		c.ward = best->id;
		c.isMoved = true;
	}
}

} // namespace circuit

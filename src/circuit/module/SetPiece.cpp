/*
 * SetPiece.cpp
 *
 *  Created on: Nov 28, 2026
 *      Author: Claude
 */

#include "module/SetPiece.h"
#include "module/BuilderManager.h"
#include "module/MilitaryManager.h"
#include "module/RaidDefence.h"
#include "module/TurretPicker.h"
#include "map/ThreatMap.h"
#include "setup/SetupManager.h"
#include "task/fighter/AttackTask.h"
#include "task/fighter/RaidTask.h"
#include "task/fighter/SquadTask.h"
#include "terrain/ChokeMap.h"
#include "terrain/TerrainManager.h"
#include "unit/CircuitUnit.h"
#include "unit/enemy/EnemyManager.h"
#include "unit/enemy/EnemyUnit.h"
#include "CircuitAI.h"
#include "util/Utils.h"

#include "Drawer.h"
#include "Log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>

namespace circuit {

using namespace springai;

static constexpr int SEEN_FRAMES = FRAMES_PER_SEC * 10;  // a mobile counts seen this recently; members drop out after
static constexpr size_t MAX_BLOBS = 8;
static constexpr float MERGE_DIST = 1600.f;  // blobs with seeds this close are one target
static constexpr float JOIN_DIST = 800.f;  // an armed enemy this near the centre joins the target
static constexpr float SHARE = 0.5f;  // the same target as a cooldown (or a raid incident) from this share on; half of T
static constexpr float STAGE_MARGIN = 200.f;  // staging points stand this far outside the target's reach
static constexpr float STAGE_REACH = 2000.f;  // staging points stand within this past the target's reach
static constexpr float STAGE_WALK_MOD = 1.5f;  // a staged set piece not gone by this x the slowest walk ...
static constexpr int STAGE_SLACK = FRAMES_PER_SEC * 10;  // ... + this is abandoned (paths run past the straight line)
static constexpr float STAGED_DIST = 200.f;  // a group this near its staging point has staged
static constexpr float PARKED_DIST = 200.f;  // a target that moves less than this ...
static constexpr int PARKED_FRAMES = FRAMES_PER_SEC * 5;  // ... in this long is parked
static constexpr int MOVED_FRAMES = FRAMES_PER_SEC * 2;  // a parked target on the move this long releases everyone
static constexpr float LEFT_DIST = 1500.f;  // it ends once this far from its start and off our ground
static constexpr float MIN_RADIUS = 1000.f;  // R from the target's edge: at least this ...
static constexpr float RADIUS_MARGIN = 250.f;  // ... and its range + this; a present group keeps R + this
static constexpr int ARRIVE_SLACK = FRAMES_PER_SEC;  // the planned arrival: after the longest walk + this
static constexpr int GUNSHIP_LAG = FRAMES_PER_SEC * 3;  // gunships arrive this long after the ground wave
static constexpr int LATE_FRAMES = FRAMES_PER_SEC * 5;  // an arrival this late is logged as late
static constexpr float CONTACT_SLACK = 100.f;  // contact: within the longer range of the two + this
static constexpr float PICK_REACH = 200.f;  // the outside enemies around a group: within its range + this
static constexpr float APPROACH_DIST = 16.f;  // a released group this much nearer than a second ago approaches

// The commander attack a committed task is, while its commander is in it
static CAttackTask* GetCommAttack(ISquadTask* task)
{
	if (task->GetFightType() != IFighterTask::FightType::ATTACK) {
		return nullptr;
	}
	CAttackTask* attack = static_cast<CAttackTask*>(task);
	return (attack->GetComm() != nullptr) ? attack : nullptr;
}

// Its strength x powerMod without its count bonus (CAttackTask::GetJointSafeThreat's measure)
static float GetBase(const ISquadTask* task)
{
	return task->GetSafeThreat() / task->GetCountPowerMod();
}

// A rally's host (quota.attack_merge.rally, quota.raid_rally): every compatible group walks to it
static bool IsRallyHost(const CMilitaryManager* militaryMgr, const ISquadTask* task)
{
	return (task == militaryMgr->GetRallyHost()) || (task == militaryMgr->GetRaidRallyHost());
}

// Its next decision now, not at its next update
static void ForceUpdate(ISquadTask* task, int frame)
{
	for (CCircuitUnit* unit : task->GetAssignees()) {
		unit->ForceUpdate(frame);
	}
}

float CSetPiece::SShape::GetRadius() const
{
	return std::max(MIN_RADIUS, range + RADIUS_MARGIN);
}

float CSetPiece::SShape::GetEdgeDist(const AIFloat3& pos) const
{
	return std::max(pos.distance2D(centre) - spread, 0.f);
}

float CSetPiece::SShape::GetWalkSec(const AIFloat3& pos, float range, float speed) const
{
	return std::max(GetEdgeDist(pos) - range, 0.f) / std::max(speed, 1.f);
}

CSetPiece::CSetPiece(CCircuitAI* circuit, const SConfig& cfg)
		: circuit(circuit)
		, cfg(cfg)
		, nextLogFrame(FRAMES_PER_SEC * 60)
{
}

CSetPiece::~CSetPiece()
{
}

void CSetPiece::Update()
{
	const int frame = circuit->GetLastFrame();
	UpdateHeld(frame);
	if (phase == Phase::OFF) {
		Scan(frame);
	} else {
		UpdateTarget(frame);
		if (phase != Phase::OFF) {
			UpdateGroups(frame);
		}
	}
	if (frame >= nextLogFrame) {
		nextLogFrame = frame + FRAMES_PER_SEC * 60;
		LogMinute();
	}
}

AIFloat3 CSetPiece::GetWait(const IFighterTask* task) const
{
	auto it = commits.find(task);
	if (it == commits.end()) {
		return -RgtVector;
	}
	const bool isWaiting = (phase == Phase::STAGE) || (circuit->GetLastFrame() < it->second.releaseFrame);
	return isWaiting ? it->second.stage : AIFloat3(-RgtVector);
}

AIFloat3 CSetPiece::GetGoal(const IFighterTask* task) const
{
	return Has(task) ? shape.centre : AIFloat3(-RgtVector);
}

bool CSetPiece::IsTargetMember(const IFighterTask* task, const CEnemyInfo* enemy) const
{
	return IsMember(enemy->GetId()) && Has(task) && !utils::is_valid(GetWait(task));
}

float CSetPiece::GetPresentStrength(const IFighterTask* task) const
{
	// Raw strength (GetRallyStrength), as the commander's flee weighs its cover
	auto it = commits.find(task);
	if ((phase != Phase::GO) || (it == commits.end())) {
		return 0.f;
	}
	float sum = it->second.isPresent ? 0.f : it->second.task->GetRallyStrength();
	for (const auto& kv : commits) {
		if (kv.second.isPresent) {
			sum += kv.second.task->GetRallyStrength();
		}
	}
	return sum;
}

bool CSetPiece::IsHeldAt(const AIFloat3& pos) const
{
	const CChokeMap* chokeMap = circuit->GetChokeMap();
	if (!chokeMap->IsReady()) {
		return false;
	}
	const int region = chokeMap->GetRegionAt(pos);
	return (region >= 0) && (region < static_cast<int>(heldFrames.size()))
			&& (circuit->GetLastFrame() - heldFrames[region] <= cfg.heldFrames);
}

bool CSetPiece::IsNearTarget(const AIFloat3& pos) const
{
	return !commits.empty() && (pos.SqDistance2D(shape.centre) < SQUARE(MERGE_DIST));
}

bool CSetPiece::IsBeyondCommReach(const AIFloat3& pos) const
{
	return (phase != Phase::OFF) && (shape.GetEdgeDist(pos) > cfg.commReach);
}

void CSetPiece::OnTaskEnd(const IFighterTask* task)
{
	commits.erase(task);
	rejoins.erase(task);
}

void CSetPiece::UpdateHeld(int frame)
{
	// Our ground: choke-map regions settled ours, or contested with our buildings in them, within held_sec. The map is
	// built on request (as the raid defence asks for it)
	CChokeMap* chokeMap = circuit->GetChokeMap();
	chokeMap->Request();
	if (!chokeMap->IsReady()) {
		if (!isNotReadyLogged) {
			isNotReadyLogged = true;
			circuit->LOG("SETPIECE waits: the choke map isn't ready (no scan until it is)");
		}
		return;
	}
	const size_t count = chokeMap->GetGraph().GetRegions().size();
	if (heldFrames.size() != count) {
		heldFrames.assign(count, -cfg.heldFrames - 1);  // a new graph: held from now on
	}
	for (size_t r = 0; r < count; ++r) {
		if (chokeMap->IsOursOrContested(r)) {
			heldFrames[r] = frame;
		}
	}
}

bool CSetPiece::GetMemberInfo(const CEnemyInfo* enemy, int frame, SMember& out) const
{
	// Armed ground mobiles of known type seen within SEEN_FRAMES, as the rallies' blobs (aircraft are anti-air's), and
	// finished turrets (CTurretPicker::IsDefence; a building stays known until seen dead)
	const CCircuitDef* edef = enemy->GetCircuitDef();
	if ((edef == nullptr) || enemy->IsHidden() || enemy->IsBeingBuilt() || (enemy->GetThreat() <= 0.f)
		|| edef->IsAbleToFly())
	{
		return false;
	}
	const bool isTurret = !edef->IsMobile();
	if (isTurret ? !circuit->GetMilitaryManager()->GetTurretPicker()->IsDefence(edef)
		: (!(edef->HasAntiLand() || edef->HasAntiWater()) || !enemy->IsRecentlySeen(frame, SEEN_FRAMES)))
	{
		return false;
	}
	out.threat = enemy->GetThreat();
	out.range = edef->GetMaxRange(CCircuitDef::RangeType::LAND);
	out.isTurret = isTurret;
	return true;
}

CSetPiece::SShape CSetPiece::MakeShape(const Members& list, int frame) const
{
	SShape s;
	AIFloat3 sum = ZeroVector;
	AIFloat3 vel = ZeroVector;
	float weight = 0.f;
	for (const auto& kv : list) {
		const CEnemyInfo* e = circuit->GetEnemyInfo(kv.first);
		if (e == nullptr) {
			continue;
		}
		const float w = std::max(kv.second.threat, 1e-3f);
		sum += e->GetPos() * w;
		vel += e->GetVel() * w;  // turrets stand still
		weight += w;
		s.threat += kv.second.threat;
		s.count += e->GetData()->GetCountWeight();
		s.range = std::max(s.range, kv.second.range);
	}
	if (weight <= 0.f) {
		return s;
	}
	s.centre = sum / weight;
	for (const auto& kv : list) {
		const CEnemyInfo* e = circuit->GetEnemyInfo(kv.first);
		if (e != nullptr) {
			const float dist = e->GetPos().distance2D(s.centre);
			s.spread = std::max(s.spread, dist);
			s.reach = std::max(s.reach, dist + kv.second.range);
		}
	}
	// Parked: at its members' mean velocity (elmos a frame) the centre moves under PARKED_DIST in PARKED_FRAMES
	s.isParked = ((vel / weight).Length2D() * PARKED_FRAMES < PARKED_DIST);
	// T: sized as the rallies size a blob, and the armed enemies outside it that join its fight: turrets that reach
	// its centre, mobiles within MERGE_DIST (an army parked just off our ground). The outside turrets near its reach
	// are kept for the staging points
	s.danger = s.threat * circuit->GetThreatMap()->GetCountMod(s.count);
	for (const auto& kv : circuit->GetEnemyInfos()) {
		SMember m;
		if ((list.find(kv.first) != list.end()) || !GetMemberInfo(kv.second, frame, m)) {
			continue;
		}
		const AIFloat3& ePos = kv.second->GetPos();
		const float dist = ePos.distance2D(s.centre);
		if (dist < (m.isTurret ? m.range : MERGE_DIST)) {
			s.danger += m.threat;
		}
		if (m.isTurret && (dist < s.reach + m.range + STAGE_REACH)) {
			s.turrets.emplace_back(ePos, m.range);
		}
	}
	return s;
}

void CSetPiece::Scan(int frame)
{
	if (heldFrames.empty()) {
		return;  // the choke map isn't ready
	}
	for (auto it = cooldowns.begin(); it != cooldowns.end();) {
		it = (frame >= it->until) ? cooldowns.erase(it) : std::next(it);
	}

	// Candidates on our ground as blobs; blobs whose seeds are within MERGE_DIST are one target
	static std::vector<CEnemyManager::SBlobMember> units;  // NOTE: micro-opt
	static std::vector<CEnemyManager::SEnemyBlob> blobs;  // NOTE: micro-opt
	static std::vector<std::pair<ICoreUnit::Id, SMember>> infos;  // NOTE: micro-opt
	units.clear();
	infos.clear();
	for (const auto& kv : circuit->GetEnemyInfos()) {
		SMember m;
		if (!GetMemberInfo(kv.second, frame, m) || !IsHeldAt(kv.second->GetPos())) {
			continue;
		}
		CEnemyManager::SBlobMember u;
		u.pos = kv.second->GetPos();
		u.threat = m.threat;
		u.count = kv.second->GetData()->GetCountWeight();
		u.id = infos.size();
		units.push_back(u);
		infos.emplace_back(kv.first, m);
	}
	if (units.empty()) {
		return;
	}
	CEnemyManager::MakeBlobs(units, MAX_BLOBS, blobs);
	std::vector<int> root(blobs.size());
	std::iota(root.begin(), root.end(), 0);
	auto find = [&root](int i) {
		while (root[i] != i) {
			i = root[i] = root[root[i]];
		}
		return i;
	};
	for (size_t i = 0; i < blobs.size(); ++i) {
		for (size_t j = i + 1; j < blobs.size(); ++j) {
			if (blobs[i].pos.SqDistance2D(blobs[j].pos) < SQUARE(MERGE_DIST)) {
				root[find(j)] = find(i);
			}
		}
	}
	std::vector<Members> targets(blobs.size());
	for (const CEnemyManager::SBlobMember& u : units) {
		if (u.blob >= 0) {
			targets[find(u.blob)].insert(infos[u.id]);
		}
	}

	// The hottest target that passes starts (one set piece at a time)
	std::vector<std::pair<const Members*, SShape>> picks;
	for (const Members& list : targets) {
		if (!list.empty()) {
			picks.emplace_back(&list, MakeShape(list, frame));
		}
	}
	std::sort(picks.begin(), picks.end(), [](const auto& a, const auto& b) { return a.second.danger > b.second.danger; });
	const AIFloat3& basePos = circuit->GetSetupManager()->GetBasePos();
	const float baseRange = circuit->GetMilitaryManager()->GetBaseDefRange();
	const float maxPower = GetMaxPower();
	std::vector<SCandidate> groups;
	for (const auto& p : picks) {
		const Members& list = *p.first;
		const SShape& s = p.second;
		// Too small to bother, or more than all of us together could start on (before the candidate pass)
		if (!utils::is_valid(s.centre) || (s.danger < cfg.minThreat) || (cfg.startRatio * s.danger >= maxPower)
			|| IsSkipped(list, s.threat))
		{
			continue;
		}
		const bool isStagedPick = s.isParked && (s.range + STAGE_MARGIN <= cfg.stageDist)
				&& (s.centre.SqDistance2D(basePos) > SQUARE(baseRange));
		float power;
		if (!CollectCandidates(list, s, isStagedPick, frame, groups, power)) {
			continue;
		}
		const SCandidate* strongest = nullptr;
		for (const SCandidate& c : groups) {
			if ((c.task != nullptr) && !c.isAntiHeavy && ((strongest == nullptr) || (c.base > strongest->base))) {
				strongest = &c;
			}
		}
		if (IsStrongEnough(power, s, (strongest != nullptr) ? strongest->task : nullptr)) {
			Start(list, s, isStagedPick, groups, power, frame);
			return;
		}
	}
}

float CSetPiece::GetMaxPower() const
{
	// Every group of ours (anti-heavy squads add none) and commander, with one count bonus: P never passes it
	float base = 0.f;
	size_t count = 0;
	for (IFighterTask::FightType type : {IFighterTask::FightType::ATTACK, IFighterTask::FightType::RAID}) {
		for (IFighterTask* t : circuit->GetMilitaryManager()->GetTasks(type)) {
			const ISquadTask* task = static_cast<ISquadTask*>(t);
			base += GetBase(task);
			count += task->GetAssignees().size();
		}
	}
	for (const auto& kv : circuit->GetTeamUnits()) {
		if (kv.second->GetCircuitDef()->IsRoleComm()) {
			base += IFighterTask::GetCoverPower(kv.second);
			++count;
		}
	}
	return base * circuit->GetThreatMap()->GetCountMod(count);
}

bool CSetPiece::IsSkipped(const Members& list, float threat) const
{
	// The same target as one on cooldown: half its threat among that one's members
	for (const SCooldown& c : cooldowns) {
		float shared = 0.f;
		for (const auto& kv : list) {
			if (c.threats.find(kv.first) != c.threats.end()) {
				shared += kv.second.threat;
			}
		}
		if (shared >= SHARE * threat) {
			return true;
		}
	}
	// A raid the raid defence answers: half its members in an answerable incident
	const CRaidDefence* raidDefence = circuit->GetMilitaryManager()->GetRaidDefence();
	if (!raidDefence->IsOn()) {
		return false;
	}
	for (const CRaidDefence::SIncident& inc : raidDefence->GetIncidents()) {
		if (!inc.isAnswerable) {
			continue;
		}
		const size_t shared = std::count_if(list.begin(), list.end(), [&inc](const Members::value_type& kv) {
			return std::binary_search(inc.members.begin(), inc.members.end(), kv.first);
		});
		if (shared >= SHARE * list.size()) {
			return true;
		}
	}
	return false;
}

bool CSetPiece::CollectCandidates(const Members& list, const SShape& s, bool isStagedPick, int frame,
		std::vector<SCandidate>& out, float& outP) const
{
	out.clear();
	CMilitaryManager* militaryMgr = circuit->GetMilitaryManager();
	for (IFighterTask::FightType type : {IFighterTask::FightType::ATTACK, IFighterTask::FightType::RAID,
		IFighterTask::FightType::AH})
	{
		for (IFighterTask* t : militaryMgr->GetTasks(type)) {
			ISquadTask* task = static_cast<ISquadTask*>(t);
			SCandidate c;
			if (!task->IsDead() && MakeCandidate(task, task->GetLeader(), list, s, isStagedPick, frame, c)) {
				out.push_back(c);
			}
		}
	}
	// Our commanders, through the commander attack's gates
	CBuilderManager* builderMgr = circuit->GetBuilderManager();
	for (const auto& kv : circuit->GetTeamUnits()) {
		CCircuitUnit* unit = kv.second;
		SCandidate c;
		if (unit->GetCircuitDef()->IsRoleComm() && !unit->IsDead()
			&& MakeCandidate(nullptr, unit, list, s, isStagedPick, frame, c) && builderMgr->IsSetPieceFree(unit))
		{
			out.push_back(c);
		}
	}
	// Some group's own pick would take it: not a set piece. P: one count bonus for all their units
	float base = 0.f;
	size_t count = 0;
	bool isAnyGroup = false;
	for (const SCandidate& c : out) {
		if (c.isAntiHeavy) {
			continue;
		}
		base += c.base;
		count += c.count;
		if (c.task != nullptr) {
			if (!c.task->IsRejectedAt(s.centre, c.task->GetSafeThreat())) {
				return false;
			}
			isAnyGroup = true;
		}
	}
	outP = base * circuit->GetThreatMap()->GetCountMod(count);
	return isAnyGroup;
}

bool CSetPiece::MakeCandidate(ISquadTask* task, CCircuitUnit* lead, const Members& list, const SShape& s,
		bool isStagedPick, int frame, SCandidate& out) const
{
	if (lead == nullptr) {
		return false;
	}
	const CCircuitDef* cdef = lead->GetCircuitDef();
	const bool isComm = (task == nullptr);
	const bool isAntiHeavy = !isComm && (task->GetFightType() == IFighterTask::FightType::AH);
	const bool isGunship = cdef->IsAbleToFly();
	const AIFloat3& pos = lead->GetPos(frame);
	// Gunships that hit ground; anti-heavy squads that walk (not bombers, not mines); in reach, and able to get there
	if ((isGunship && (isAntiHeavy || !cdef->HasAntiLand())) || (isAntiHeavy && cdef->IsRoleMine())
		|| (s.GetEdgeDist(pos) > (isComm ? cfg.commReach : cfg.reach))
		|| (!isGunship && !circuit->GetTerrainManager()->CanMobileReachAt(lead->GetArea(), s.centre, cdef->GetMaxRange())))
	{
		return false;
	}
	// Calm: not fleeing, nor fighting anything but the target; not the protect's, nor a rally's host
	if (!isComm) {
		const CEnemyInfo* target = task->GetTarget();
		if (task->IsFleeing(frame) || task->IsProtecting() || task->IsGathering()
			|| IsRallyHost(circuit->GetMilitaryManager(), task)
			|| (task->IsFighting() && (list.find(target->GetId()) == list.end())))
		{
			return false;
		}
	}
	// It hits half of T (anti-heavy squads: any of it)
	const float hitShare = GetHitShare(lead, list);
	if (isAntiHeavy ? (hitShare <= 0.f) : (hitShare < SHARE)) {
		return false;
	}
	// Its point within gather_sec at its pace: its staging point (a commander's within comm_reach too: its leash), else
	// contact (its range off the target's edge)
	const float speed = std::max(isComm ? cdef->GetSpeed() : task->GetLowestSpeed(), 1.f);
	const float range = cdef->GetMaxRange();
	out.stage = -RgtVector;
	if (isStagedPick) {
		out.stage = FindStage(task, lead, isComm ? range : task->GetHighestRange(), s, frame);
		if (!utils::is_valid(out.stage) || (isComm && (s.GetEdgeDist(out.stage) > cfg.commReach))) {
			return false;
		}
		out.walkSec = pos.distance2D(out.stage) / speed;
		out.goSec = s.GetWalkSec(out.stage, range, speed);
	} else {
		out.walkSec = out.goSec = s.GetWalkSec(pos, range, speed);
	}
	if (out.walkSec > cfg.gatherSec) {
		return false;
	}
	out.task = task;
	out.lead = lead;
	out.base = isComm ? IFighterTask::GetCoverPower(lead) : GetBase(task);
	out.count = isComm ? 1 : task->GetAssignees().size();
	out.isGunship = isGunship;
	out.isAntiHeavy = isAntiHeavy;
	return true;
}

AIFloat3 CSetPiece::FindStage(const ISquadTask* task, CCircuitUnit* lead, float range, const SShape& s,
		int frame) const
{
	// On its side, STAGE_MARGIN outside both the target's reach and its own range of the target's edge (it opens no
	// fight from there), else up to ~60 degrees round either way: a spot it can stand on, out of every outside enemy
	// turret's range + STAGE_MARGIN, and safe for its own strength (a commander: no allowance of its own)
	const AIFloat3& pos = lead->GetPos(frame);
	const float dist = std::max(pos.distance2D(s.centre), 1.f);
	const float stageDist = std::max(s.reach, s.spread + range) + STAGE_MARGIN;
	const float dx = (pos.x - s.centre.x) / dist;
	const float dz = (pos.z - s.centre.z) / dist;
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	for (float angle : {0.f, 0.5f, -0.5f, 1.f, -1.f}) {
		const float c = cosf(angle);
		const float sn = sinf(angle);
		AIFloat3 p(s.centre.x + (dx * c - dz * sn) * stageDist, s.centre.y, s.centre.z + (dx * sn + dz * c) * stageDist);
		CTerrainManager::CorrectPosition(p);
		const bool isCovered = std::any_of(s.turrets.begin(), s.turrets.end(), [&p](const auto& t) {
			return p.SqDistance2D(t.first) < SQUARE(t.second + STAGE_MARGIN);
		});
		if (!isCovered && terrainMgr->CanMoveToPos(lead->GetArea(), p)
			&& ((task == nullptr) || !task->IsRejectedAt(p, task->GetSafeThreat())))
		{
			return p;
		}
	}
	return -RgtVector;
}

float CSetPiece::GetHitShare(const CCircuitUnit* lead, const Members& list) const
{
	// As the picks' type test: its target category, anti-static units only buildings, under water only anti-water
	const CCircuitDef* cdef = lead->GetCircuitDef();
	float hit = 0.f;
	float all = 0.f;
	for (const auto& kv : list) {
		const CEnemyInfo* e = circuit->GetEnemyInfo(kv.first);
		if (e == nullptr) {
			continue;
		}
		const CCircuitDef* edef = e->GetCircuitDef();
		all += kv.second.threat;
		if (((edef->GetCategory() & cdef->GetTargetCategory()) != 0) && !(cdef->IsAttrAntiStat() && edef->IsMobile())
			&& (cdef->HasAntiWater() || (e->GetPos().y >= -SQUARE_SIZE * 5)))
		{
			hit += kv.second.threat;
		}
	}
	return (all > 0.f) ? hit / all : 0.f;
}

bool CSetPiece::IsStrongEnough(float power, const SShape& s, const ISquadTask* strongest) const
{
	// Power over T, and the threat at the centre under what that power allows there (the strongest group's measure)
	return (strongest != nullptr) && (power > cfg.startRatio * s.danger)
			&& !strongest->IsRejectedAt(s.centre, power / cfg.startRatio);
}

void CSetPiece::Start(const Members& list, const SShape& s, bool isStagedPick, std::vector<SCandidate>& groups,
		float power, int frame)
{
	members = list;
	shape = s;
	startCentre = s.centre;
	isStaged = isStagedPick;
	phase = isStaged ? Phase::STAGE : Phase::GO;
	startFrame = frame;
	goFrame = isStaged ? -1 : frame;
	stallFrame = frame;
	movedFrame = -1;
	firstArrive = lastArrive = -1;
	timedArrivals = kills = 0;
	startPower = power;
	seenThreats.clear();
	for (const auto& kv : members) {
		seenThreats[kv.first] = kv.second.threat;
	}
	float walkSec = 0.f;
	float goSec = 0.f;
	for (const SCandidate& c : groups) {
		walkSec = std::max(walkSec, c.walkSec);
		goSec = std::max(goSec, c.goSec);
	}
	const float etaSec = (isStaged ? walkSec + goSec : walkSec) + float(ARRIVE_SLACK) / FRAMES_PER_SEC;
	arriveFrame = frame + static_cast<int>(etaSec * FRAMES_PER_SEC);
	stageEndFrame = frame + static_cast<int>(STAGE_WALK_MOD * walkSec * FRAMES_PER_SEC) + STAGE_SLACK;
	++stats.starts;
	if (isStaged) {
		++stats.staged;
	}
	circuit->LOG("SETPIECE start%s: %s, P %.1f vs T %.1f, %i groups, %i members (range %.0f, %s) at (%.0f, %.0f),"
			" arrival in %.0f s", cfg.isLogOnly ? " (log only)" : "", isStaged ? "staged" : "straight in", power, s.danger,
			static_cast<int>(groups.size()), static_cast<int>(members.size()), s.range, s.isParked ? "parked" : "moving",
			s.centre.x, s.centre.z, etaSec);
	if (IsLive()) {
		for (const SCandidate& c : groups) {
			Commit(c, frame);
		}
	}
	SetLabel(power, etaSec);
}

void CSetPiece::Commit(const SCandidate& c, int frame)
{
	ISquadTask* task = c.task;
	if (task == nullptr) {  // a commander: a commander attack of its own, with no target yet
		task = circuit->GetBuilderManager()->StartSetPieceAttack(c.lead);
		if (task == nullptr) {
			return;
		}
	}
	SCommit& commit = commits[task];
	commit = SCommit();
	commit.task = task;
	commit.stage = isStaged ? c.stage : AIFloat3(-RgtVector);
	commit.lastDist = c.lead->GetPos(frame).distance2D(shape.centre);
	commit.isGunship = c.isGunship;
	commit.isAntiHeavy = c.isAntiHeavy;
	if (isStaged) {
		ForceUpdate(task, frame);  // its next decision is the set piece's: to its staging point
	} else {
		commit.releaseFrame = frame;
		Release(commit, frame);
	}
}

void CSetPiece::UpdateTarget(int frame)
{
	// Members: a destroyed one leaves the enemy list at once (a kill); one unseen for SEEN_FRAMES drops out
	for (auto it = members.begin(); it != members.end();) {
		const CEnemyInfo* e = circuit->GetEnemyInfo(it->first);
		if (e == nullptr) {
			++kills;
			it = members.erase(it);
		} else if (!GetMemberInfo(e, frame, it->second)) {
			it = members.erase(it);
		} else {
			++it;
		}
	}
	if (members.empty()) {
		if (kills > 0) {
			End("cleared", EndKind::KILL, frame);
		} else {
			End("lost", EndKind::OTHER, frame);
		}
		return;
	}
	// Joiners: armed enemies near the centre
	for (const auto& kv : circuit->GetEnemyInfos()) {
		SMember m;
		if (!IsMember(kv.first) && (kv.second->GetPos().SqDistance2D(shape.centre) < SQUARE(JOIN_DIST))
			&& GetMemberInfo(kv.second, frame, m))
		{
			members.emplace(kv.first, m);
		}
	}
	for (const auto& kv : members) {
		seenThreats[kv.first] = kv.second.threat;
	}
	shape = MakeShape(members, frame);
}

void CSetPiece::UpdateGroups(int frame)
{
	if (cfg.isLogOnly) {  // nothing committed: the label stays gather_sec
		if (frame - startFrame >= static_cast<int>(cfg.gatherSec * FRAMES_PER_SEC)) {
			End("log only", EndKind::OTHER, frame);
		}
		return;
	}
	// Each group: the protect took it, or its pick pulls it away (before its first contact); its contact, staging,
	// release and presence
	const float radius = shape.GetRadius();
	static std::vector<std::pair<const IFighterTask*, const char*>> gone;  // NOTE: micro-opt
	gone.clear();
	bool isContact = false;
	bool isApproach = false;
	for (auto& kv : commits) {
		SCommit& c = kv.second;
		ISquadTask* task = c.task;
		CCircuitUnit* lead = task->GetLeader();
		if (lead == nullptr) {
			continue;  // aborting
		}
		if (task->IsProtecting() || task->IsGathering()) {
			gone.emplace_back(task, "protect");
			continue;
		}
		const bool isTouch = IsInContact(task, frame);
		if ((c.arriveFrame < 0) && !isTouch && !c.isAntiHeavy && IsPulledAway(task, frame)) {
			gone.emplace_back(task, "pick");
			continue;
		}
		const bool isWaiting = (phase == Phase::STAGE) || (frame < c.releaseFrame);
		if (!isWaiting && !c.isReleased) {
			Release(c, frame);  // a timed one, at its time
		}
		if (isTouch && !isWaiting && (c.arriveFrame < 0)) {
			c.arriveFrame = frame;
			++stats.arrivals;
			if (c.isTimed) {
				firstArrive = (firstArrive < 0) ? frame : firstArrive;
				lastArrive = frame;
				++timedArrivals;
			}
			circuit->LOG("SETPIECE arrive%s: a group of %i (%.1f) after %.1f s, %+.1f s from the plan",
					(c.isTimed && (frame > arriveFrame + LATE_FRAMES)) ? " late" : "",
					static_cast<int>(task->GetAssignees().size()), task->GetRallyStrength(),
					float(frame - startFrame) / FRAMES_PER_SEC, float(frame - arriveFrame) / FRAMES_PER_SEC);
		}
		const AIFloat3& pos = lead->GetPos(frame);
		const float dist = pos.distance2D(shape.centre);
		c.isStaged = utils::is_valid(c.stage) && (pos.SqDistance2D(c.stage) < SQUARE(STAGED_DIST));
		c.isPresent = !isWaiting && !task->IsFleeing(frame)
				&& (shape.GetEdgeDist(pos) <= radius + (c.isPresent ? RADIUS_MARGIN : 0.f));
		isContact |= isTouch;
		isApproach |= !isWaiting && (isTouch || (dist < c.lastDist - APPROACH_DIST));
		c.lastDist = dist;
	}
	for (const auto& g : gone) {
		Revert(g.first, g.second, frame);
	}
	if (commits.empty()) {
		End("no groups", EndKind::OTHER, frame);
		return;
	}
	if (UpdateBoosts(frame) < cfg.abandonRatio * shape.danger) {
		End("abandon", EndKind::ABANDON, frame);
		return;
	}

	// Contact releases everyone still waiting, and so does a parked target on the move for MOVED_FRAMES
	const char* releaseReason = isContact ? "contact" : nullptr;
	if (isStaged && !shape.isParked) {
		if (movedFrame < 0) {
			movedFrame = frame;
		} else if ((frame - movedFrame >= MOVED_FRAMES) && (releaseReason == nullptr)) {
			releaseReason = "moved";
		}
	} else {
		movedFrame = -1;
	}

	if (phase == Phase::STAGE) {
		// A group reverted once may come back, before the go
		for (auto it = rejoins.begin(); it != rejoins.end();) {
			ISquadTask* task = it->second;
			SCandidate c;
			if (!MakeCandidate(task, task->GetLeader(), members, shape, true, frame, c)) {
				++it;
				continue;
			}
			Commit(c, frame);
			commits[task].isRejoined = true;
			circuit->LOG("SETPIECE rejoin: a group of %i (%.1f)", static_cast<int>(task->GetAssignees().size()),
					task->GetRallyStrength());
			it = rejoins.erase(it);
		}
		if (releaseReason != nullptr) {
			Go(releaseReason, true, frame);
			return;
		}
		// The go: the staged groups alone pass the start test
		CThreatMap* threatMap = circuit->GetThreatMap();
		float stagedBase = 0.f;
		size_t stagedCount = 0;
		const ISquadTask* strongest = nullptr;
		for (const auto& kv : commits) {
			const SCommit& c = kv.second;
			if (!c.isStaged || c.isAntiHeavy || c.task->IsFleeing(frame)) {
				continue;
			}
			stagedBase += GetBase(c.task);
			stagedCount += c.task->GetAssignees().size();
			if ((GetCommAttack(c.task) == nullptr) && ((strongest == nullptr) || (GetBase(c.task) > GetBase(strongest)))) {
				strongest = c.task;
			}
		}
		if (IsStrongEnough(stagedBase * threatMap->GetCountMod(stagedCount), shape, strongest)) {
			Go("ready", false, frame);
		} else if (frame >= stageEndFrame) {
			End("no go", EndKind::ABANDON, frame);
		}
		return;
	}

	if (releaseReason != nullptr) {
		for (auto& kv : commits) {
			SCommit& c = kv.second;
			if (!c.isReleased) {
				c.releaseFrame = frame;
				Release(c, frame);
			}
		}
		UpdateBoosts(frame);
	}
	// The fight: some group in contact or walking in, else a stall (a failed plan, as an abandon)
	if (isContact || isApproach) {
		stallFrame = frame;
	} else if (frame - stallFrame >= cfg.stallFrames) {
		End("stall", EndKind::ABANDON, frame);
		return;
	}
	if ((shape.centre.SqDistance2D(startCentre) > SQUARE(LEFT_DIST)) && !IsHeldAt(shape.centre)) {
		End("left", EndKind::OTHER, frame);
	}
}

float CSetPiece::UpdateBoosts(int frame)
{
	// P: every committed group (anti-heavy squads add none), one stepping back too: its escape is brief, and an abandon
	// on that dip would rout the rest. The wave: the released groups present and, from the go, the timed ones on their
	// way, so the first one released (the farthest) doesn't judge the target alone; gunships count gunships only
	auto isWave = [frame](const SCommit& c) {
		return c.isPresent || (c.isTimed && !c.task->IsFleeing(frame));
	};
	CThreatMap* threatMap = circuit->GetThreatMap();
	float base = 0.f, waveBase = 0.f, airBase = 0.f;
	size_t count = 0, waveCount = 0, airCount = 0;
	for (const auto& kv : commits) {
		const SCommit& c = kv.second;
		if (c.isAntiHeavy) {
			continue;
		}
		const float b = GetBase(c.task);
		const size_t n = c.task->GetAssignees().size();
		base += b;
		count += n;
		if (isWave(c)) {
			waveBase += b;
			waveCount += n;
			if (c.isGunship) {
				airBase += b;
				airCount += n;
			}
		}
	}
	for (auto& kv : commits) {
		SCommit& c = kv.second;
		c.boost = 1.f;
		if (c.isAntiHeavy || !c.isReleased) {
			continue;
		}
		float sum = c.isGunship ? airBase : waveBase;
		size_t n = c.isGunship ? airCount : waveCount;
		if (!isWave(c)) {  // always with itself
			sum += GetBase(c.task);
			n += c.task->GetAssignees().size();
		}
		c.boost = std::max(sum * threatMap->GetCountMod(n) / std::max(c.task->GetSafeThreat(), 1e-3f), 1.f);
	}
	return base * threatMap->GetCountMod(count);
}

void CSetPiece::Release(SCommit& c, int frame)
{
	c.isReleased = true;
	ForceUpdate(c.task, frame);
}

void CSetPiece::Go(const char* reason, bool isAll, int frame)
{
	// Each staged group's walk to contact (straight, at its pace): all arrive after the longest + 1 s, gunships 3 s
	// after the ground wave. The rest (all of them on isAll) go at once
	auto walkFrames = [this, frame](const SCommit& c) {
		CCircuitUnit* lead = c.task->GetLeader();
		const float range = lead->GetCircuitDef()->GetMaxRange();
		return static_cast<int>(shape.GetWalkSec(lead->GetPos(frame), range, c.task->GetLowestSpeed()) * FRAMES_PER_SEC);
	};
	int longest = -1;
	int longestAir = -1;
	int staged = 0;
	for (const auto& kv : commits) {
		const SCommit& c = kv.second;
		if ((isAll || c.isStaged) && (c.task->GetLeader() != nullptr)) {
			int& slot = c.isGunship ? longestAir : longest;
			slot = std::max(slot, walkFrames(c));
			staged += c.isStaged ? 1 : 0;
		}
	}
	arriveFrame = frame + std::max(longest, (longest < 0) ? longestAir : 0) + ARRIVE_SLACK;
	const int airArrive = (longest < 0) ? arriveFrame : (arriveFrame + GUNSHIP_LAG);
	for (auto& kv : commits) {
		SCommit& c = kv.second;
		c.isTimed = !isAll && c.isStaged && (c.task->GetLeader() != nullptr);
		c.releaseFrame = c.isTimed ? std::max((c.isGunship ? airArrive : arriveFrame) - walkFrames(c), frame) : frame;
		if (c.releaseFrame <= frame) {
			Release(c, frame);
		}
	}
	UpdateBoosts(frame);  // the wave counts from now
	phase = Phase::GO;
	goFrame = stallFrame = frame;
	++stats.goes;
	const float etaSec = float(arriveFrame - frame) / FRAMES_PER_SEC;
	circuit->LOG("SETPIECE go (%s) after %.1f s: %i staged of %i groups, T %.1f, arrival in %.1f s", reason,
			float(frame - startFrame) / FRAMES_PER_SEC, staged, static_cast<int>(commits.size()), shape.danger, etaSec);
	SetLabel(startPower, etaSec);
}

void CSetPiece::Revert(const IFighterTask* task, const char* reason, int frame)
{
	auto it = commits.find(task);
	if (it == commits.end()) {
		return;
	}
	ISquadTask* group = it->second.task;
	circuit->LOG("SETPIECE revert (%s): a group of %i (%.1f)", reason, static_cast<int>(group->GetAssignees().size()),
			group->GetRallyStrength());
	CAttackTask* attack = GetCommAttack(group);
	if (attack != nullptr) {
		attack->EndComm(CommEnd::SET_PIECE);  // the commander back to its jobs
	} else if ((phase == Phase::STAGE) && !it->second.isRejoined) {
		rejoins[task] = group;
	}
	commits.erase(it);
	ForceUpdate(group, frame);
}

void CSetPiece::End(const char* reason, EndKind kind, int frame)
{
	// Its members on cooldown: cooldown_sec, doubled after each abandon (no go, stall) in a row (up to 4x), reset by a
	// kill
	if (kind == EndKind::KILL) {
		abandons = 0;
	}
	const int cooldown = cfg.cooldownFrames << abandons;
	if (kind == EndKind::ABANDON) {
		abandons = std::min(abandons + 1, 2);
	}
	cooldowns.push_back({std::move(seenThreats), frame + cooldown});
	seenThreats.clear();
	++((kind == EndKind::KILL) ? stats.kills : (kind == EndKind::ABANDON) ? stats.abandons : stats.others);
	const float spread = float(lastArrive - firstArrive) / FRAMES_PER_SEC;
	if (timedArrivals > 1) {
		++stats.spreads;
		stats.spreadSum += spread;
		stats.spreadMax = std::max(stats.spreadMax, spread);
	}
	char spreadText[48] = "";
	if (isStaged && (timedArrivals > 1)) {
		snprintf(spreadText, sizeof(spreadText), ", arrival spread %.1f s", spread);
	}
	circuit->LOG("SETPIECE end (%s) after %.1f s: %s, %i groups left, %i members left (T %.1f), %i killed%s; cooldown"
			" %.0f s", reason, float(frame - startFrame) / FRAMES_PER_SEC, (goFrame >= 0) ? "went" : "no go",
			static_cast<int>(commits.size()), static_cast<int>(members.size()), shape.danger, kills, spreadText,
			float(cooldown) / FRAMES_PER_SEC);
	// Its groups decide again at once; its commanders go back to their jobs
	for (const auto& kv : commits) {
		CAttackTask* attack = GetCommAttack(kv.second.task);
		if (attack != nullptr) {
			attack->EndComm(CommEnd::SET_PIECE);
		}
		ForceUpdate(kv.second.task, frame);
	}
	commits.clear();
	rejoins.clear();
	members.clear();
	phase = Phase::OFF;
	ClearLabel();
}

bool CSetPiece::IsInContact(const ISquadTask* task, int frame) const
{
	// Fighting a member, or a member's weapons reach the leader (+ slack). Not our reach of it: a waiting group that
	// outranges the target stages beyond its own range (FindStage), and opens no fight
	const CEnemyInfo* target = task->GetTarget();
	if ((target != nullptr) && IsMember(target->GetId()) && task->IsFighting()) {
		return true;
	}
	const AIFloat3& pos = task->GetLeader()->GetPos(frame);
	for (const auto& kv : members) {
		const CEnemyInfo* e = circuit->GetEnemyInfo(kv.first);
		if ((e != nullptr) && (pos.SqDistance2D(e->GetPos()) < SQUARE(kv.second.range + CONTACT_SLACK))) {
			return true;
		}
	}
	return false;
}

bool CSetPiece::IsPulledAway(const ISquadTask* task, int frame) const
{
	// Its pick: an armed enemy outside the target. In passing while the armed enemies outside it around the group (its
	// range + PICK_REACH of the leader; the pick itself wherever it is) stay under pass_share of its strength
	const CEnemyInfo* target = task->GetTarget();
	if ((target == nullptr) || IsMember(target->GetId()) || (target->GetThreat() <= 0.f)) {
		return false;
	}
	CCircuitUnit* lead = task->GetLeader();
	const AIFloat3& pos = lead->GetPos(frame);
	const float sqReach = SQUARE(lead->GetCircuitDef()->GetMaxRange() + PICK_REACH);
	float outside = (pos.SqDistance2D(target->GetPos()) < sqReach) ? 0.f : target->GetThreat();
	for (const auto& kv : circuit->GetEnemyInfos()) {
		const CEnemyInfo* e = kv.second;
		if (!e->IsHidden() && (e->GetThreat() > 0.f) && !IsMember(kv.first) && (pos.SqDistance2D(e->GetPos()) < sqReach)) {
			outside += e->GetThreat();
		}
	}
	return outside >= cfg.passShare * GetBase(task);
}

float CSetPiece::GetGoBoost(const IFighterTask* task, const AIFloat3& pos) const
{
	auto it = commits.find(task);
	if ((it == commits.end()) || (it->second.boost <= 1.f)) {
		return 1.f;
	}
	const float radius = shape.GetRadius() + (it->second.isPresent ? RADIUS_MARGIN : 0.f);
	return (shape.GetEdgeDist(pos) <= radius) ? it->second.boost : 1.f;
}

void CSetPiece::SetLabel(float power, float etaSec)
{
	ClearLabel();
	if (cfg.isLabel) {
		char text[128];
		snprintf(text, sizeof(text), "Set piece attack. Power: %.0f. Threat: %.0f. Estimated arrival: %.0f s", power,
				shape.danger, etaSec);
		labelPos = shape.centre;
		circuit->GetDrawer()->AddPoint(labelPos, text);
	}
}

void CSetPiece::ClearLabel()
{
	if (utils::is_valid(labelPos)) {
		circuit->GetDrawer()->DeletePointsAndLines(labelPos);
		labelPos = -RgtVector;
	}
}

void CSetPiece::LogMinute()
{
	if ((stats.starts == 0) && (phase == Phase::OFF)) {
		return;
	}
	circuit->LOG("SETPIECE minute: %i starts (%i staged), %i goes; ends: %i cleared, %i abandoned, %i other; %i"
			" arrivals; staged spread %.1f s average, %.1f s max over %i%s", stats.starts, stats.staged, stats.goes,
			stats.kills, stats.abandons, stats.others, stats.arrivals,
			(stats.spreads > 0) ? stats.spreadSum / stats.spreads : 0.f, stats.spreadMax, stats.spreads,
			(phase != Phase::OFF) ? "; one running" : "");
	stats = SStats();
}

} // namespace circuit

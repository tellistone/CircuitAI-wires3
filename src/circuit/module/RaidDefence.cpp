/*
 * RaidDefence.cpp
 *
 *  Created on: Nov 27, 2026
 *      Author: Claude
 */

#include "module/RaidDefence.h"
#include "module/BuilderManager.h"
#include "module/EconomyManager.h"
#include "module/FactoryManager.h"
#include "module/Frontline.h"
#include "module/MilitaryManager.h"
#include "module/Protect.h"
#include "map/InfluenceMap.h"
#include "map/MapManager.h"
#include "map/ThreatMap.h"
#include "resource/MetalManager.h"
#include "task/fighter/SquadTask.h"
#include "terrain/ChokeMap.h"
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
#include <iterator>
#include <limits>

namespace circuit {

using namespace springai;

// A counted enemy this second
struct CRaidDefence::SCounted {
	CEnemyManager::SBlobMember m;  // threat x raid_thr_mod and the raid view's count weight, faded
	AIFloat3 vel;
	bool isComm;
};

CRaidDefence::CRaidDefence(CCircuitAI* circuit, const SConfig& cfg)
		: circuit(circuit)
		, cfg(cfg)
		, isOver(!cfg.isEnabled)
		, threatT(0.f)
		, threatB(0.f)
		, threatBmax(0.f)
		, oneRaider(0.f)
		, needD(0.f)
		, nextIncidentId(1)
		, coverFrame(-COVER_FRAMES)
		, stateLogFrame(-STATE_LOG_FRAMES)
		, lostMetal(0.f)
		, defendKills(0.f)
		, raidKills(0.f)
		, shareSum(0.f)
		, shareSamples(0)
{
}

CRaidDefence::~CRaidDefence()
{
}

bool CRaidDefence::IsRole(const CCircuitDef* cdef) const
{
	const CCircuitDef::RoleT mainRole = cdef->GetMainRole();
	const CCircuitDef::RoleM mask = CCircuitDef::GetMask(mainRole) | CCircuitDef::GetMask(circuit->GetBindedRole(mainRole));
	return !cdef->IsAbleToFly() && ((cfg.roleMask & mask) != 0);
}

bool CRaidDefence::IsOurGround(const AIFloat3& pos) const
{
	AIFloat3 p = pos;
	CTerrainManager::CorrectPosition(p);
	if (circuit->GetInflMap()->IsAllyTerritory(p, INFL_EPS)) {
		return true;
	}
	const CChokeMap* chokeMap = circuit->GetChokeMap();
	if (!chokeMap->IsReady()) {
		return false;
	}
	const int region = chokeMap->GetRegionAt(p);
	return (region != CRegionGraph::NO_REGION) && chokeMap->IsOurGround(region);
}

void CRaidDefence::Update()
{
	if (!IsOn()) {
		return;
	}
	const int frame = circuit->GetLastFrame();
	CheckWindow(frame);
	if (isOver) {
		return;
	}
	circuit->GetChokeMap()->Request();  // held ground and pockets: builds the choke map if nothing else asked for it
	for (auto it = attackers.begin(); it != attackers.end();) {
		it = (frame - it->second > ATTACKER_FRAMES) ? attackers.erase(it) : std::next(it);
	}

	UpdateDefenderAreas(frame);
	if (frame - coverFrame >= COVER_FRAMES) {
		coverFrame = frame;
		UpdateCover(frame);
	}
	CollectAssets(frame);

	static std::vector<SCounted> all;  // NOTE: micro-opt
	static std::vector<SCounted> ground;  // NOTE: micro-opt
	CollectEnemies(frame, all, ground);
	UpdateBmax(all);
	UpdateIncidents(frame, ground);

	threatT = threatB = 0.f;
	for (const SIncident& inc : incidents) {
		if (inc.isAnswerable) {
			threatT += inc.threat;
			threatB = std::max(threatB, inc.threat);
		}
	}
	needD = std::min(cfg.margin * threatT, cfg.maxShare * GetRaidStrength());

	// The share of our raiders defending, for the minute line
	unsigned int raiders = 0;
	unsigned int defending = 0;
	for (CCircuitUnit* unit : circuit->GetMilitaryManager()->GetArmy()) {
		if (!IsRole(unit->GetCircuitDef())) {
			continue;
		}
		++raiders;
		const IUnitTask* task = unit->GetTask();
		if ((task != nullptr) && (task->GetType() == IUnitTask::Type::FIGHTER)
			&& (static_cast<const IFighterTask*>(task)->GetFightType() == IFighterTask::FightType::RAID_DEFEND))
		{
			++defending;
		}
	}
	if (raiders > 0) {
		shareSum += (float)defending / raiders;
		++shareSamples;
	}

	LogState(frame);
}

void CRaidDefence::CheckWindow(int frame)
{
	// One-off, as quota.solo_hunt's switch: once the ally team's finished mexes reach mex_share of the map's spots
	CMetalManager* metalMgr = circuit->GetMetalManager();
	metalMgr->MarkAllyMexes();
	const int spotCount = metalMgr->GetSpots().size();
	const float share = (spotCount > 0) ? (float)metalMgr->GetMexCount() / spotCount : 1.f;
	if (share < cfg.mexShare) {
		return;
	}
	isOver = true;
	incidents.clear();
	threatT = threatB = needD = 0.f;
	circuit->LOG("RAIDDEF over at %i s: ally mexes %i / %i = %.2f, mex_share %.2f", frame / FRAMES_PER_SEC,
			metalMgr->GetMexCount(), spotCount, share, cfg.mexShare);
}

void CRaidDefence::UpdateDefenderAreas(int frame)
{
	defenderAreas.clear();
	for (CCircuitUnit* unit : circuit->GetMilitaryManager()->GetArmy()) {
		const CCircuitDef* cdef = unit->GetCircuitDef();
		if (!IsRole(cdef) || (unit->GetArea() == nullptr)) {
			continue;
		}
		auto it = std::find_if(defenderAreas.begin(), defenderAreas.end(), [unit](const SArea& a) {
			return a.area == unit->GetArea();
		});
		if (it == defenderAreas.end()) {
			defenderAreas.push_back({unit->GetArea(), cdef->GetMaxRange()});
		} else {
			it->range = std::max(it->range, cdef->GetMaxRange());
		}
	}
}

/*
 * Measure
 */

void CRaidDefence::CollectEnemies(int frame, std::vector<SCounted>& outAll, std::vector<SCounted>& outGround)
{
	outAll.clear();
	outGround.clear();
	CThreatMap* threatMap = circuit->GetThreatMap();
	static std::vector<SCounted> comms;  // NOTE: micro-opt
	static std::vector<bool> isCommGround;  // NOTE: micro-opt
	comms.clear();
	isCommGround.clear();
	for (const auto& kv : circuit->GetMapManager()->GetHostileUnits()) {
		const CEnemyUnit* e = kv.second;
		const CCircuitDef* edef = e->GetCircuitDef();
		// GetBlobs' surface fighters: known, mobile, not flying, armed against land or water, finished, not gone
		if ((edef == nullptr) || !edef->IsMobile() || edef->IsAbleToFly()
			|| !(edef->HasAntiLand() || edef->HasAntiWater())
			|| e->IsHidden() || e->IsBeingBuilt() || (e->GetThreat() <= 0.f)
			|| !e->IsRecentlySeen(frame, SEEN_FRAMES))
		{
			continue;
		}
		const bool isComm = edef->IsRoleComm();
		if (!isComm && !edef->IsEnemyRoleAny(cfg.enemyRoleMask)) {
			continue;
		}
		const float threat = e->GetThreat() * edef->GetRaidThrMod();
		if (!isComm) {
			oneRaider = std::max(oneRaider, threat * threatMap->GetCountMod(e->GetRaidCountWeight()));
		}
		const int unseen = e->IsInRadarOrLOS() ? 0 : (frame - e->GetLastSeen());
		const float fade = std::exp2(-unseen / FADE_FRAMES);
		SCounted c;
		c.m.pos = e->GetPos();
		c.m.threat = threat * fade;
		c.m.count = e->GetRaidCountWeight() * fade;
		c.m.id = e->GetId();
		c.vel = e->GetVel();
		c.isComm = isComm;
		// On our ground, and kept so until GROUND_KEEP_FRAMES off it
		if (IsOurGround(c.m.pos)) {
			groundFrames[c.m.id] = frame;
		}
		auto it = groundFrames.find(c.m.id);
		const bool isGround = (it != groundFrames.end()) && (frame - it->second <= GROUND_KEEP_FRAMES);
		if (isComm) {
			comms.push_back(c);
			isCommGround.push_back(isGround);
			continue;
		}
		outAll.push_back(c);
		if (isGround) {
			outGround.push_back(c);
		}
	}
	for (auto it = groundFrames.begin(); it != groundFrames.end();) {
		it = (frame - it->second > GROUND_KEEP_FRAMES) ? groundFrames.erase(it) : std::next(it);
	}
	// An enemy commander walking with raiders is part of the raid; alone it isn't one
	auto isNearRaider = [](const SCounted& comm, const std::vector<SCounted>& raiders) {
		return std::any_of(raiders.begin(), raiders.end(), [&comm](const SCounted& r) {
			return !r.isComm && (r.m.pos.SqDistance2D(comm.m.pos) < SQUARE(COMM_JOIN_DIST));
		});
	};
	for (size_t i = 0; i < comms.size(); ++i) {
		if (isNearRaider(comms[i], outAll)) {
			outAll.push_back(comms[i]);
		}
		if (isCommGround[i] && isNearRaider(comms[i], outGround)) {
			outGround.push_back(comms[i]);
		}
	}
}

void CRaidDefence::UpdateBmax(std::vector<SCounted>& all)
{
	// The biggest raider blob anywhere, as a peak that halves every BMAX_HALF_FRAMES: the bar a cluster's turrets
	// must clear. Never below one raider, so a cluster with a single small turret isn't defended before any is seen
	static std::vector<CEnemyManager::SBlobMember> members;  // NOTE: micro-opt
	static std::vector<CEnemyManager::SEnemyBlob> blobs;  // NOTE: micro-opt
	members.clear();
	for (const SCounted& c : all) {
		members.push_back(c.m);
	}
	CEnemyManager::MakeBlobs(members, members.size(), blobs);
	CThreatMap* threatMap = circuit->GetThreatMap();
	float biggest = 0.f;
	for (const CEnemyManager::SEnemyBlob& b : blobs) {
		biggest = std::max(biggest, b.threat * threatMap->GetCountMod(b.count));
	}
	threatBmax = std::max({threatBmax * std::exp2(-FRAMES_PER_SEC / BMAX_HALF_FRAMES), biggest, oneRaider});
}

void CRaidDefence::UpdateIncidents(int frame, std::vector<SCounted>& ground)
{
	// The blobs on our ground. Each keeps the id of the incident a second ago it shares at least half of that one's
	// members with (the most shared first, hottest blob first), else it is a new incident
	static std::vector<CEnemyManager::SBlobMember> members;  // NOTE: micro-opt
	static std::vector<CEnemyManager::SEnemyBlob> blobs;  // NOTE: micro-opt
	members.clear();
	std::unordered_map<ICoreUnit::Id, AIFloat3> vels;
	for (const SCounted& c : ground) {
		members.push_back(c.m);
		vels[c.m.id] = c.vel;
	}
	CEnemyManager::MakeBlobs(members, members.size(), blobs);

	CThreatMap* threatMap = circuit->GetThreatMap();
	std::vector<SIncident> next(blobs.size());
	for (size_t i = 0; i < blobs.size(); ++i) {
		next[i].pos = blobs[i].pos;
		next[i].vel = ZeroVector;
		next[i].threat = blobs[i].threat * threatMap->GetCountMod(blobs[i].count);
	}
	for (const CEnemyManager::SBlobMember& m : members) {
		next[m.blob].members.push_back(m.id);
		next[m.blob].vel += vels[m.id];
	}
	std::sort(next.begin(), next.end(), [](const SIncident& a, const SIncident& b) { return a.threat > b.threat; });

	std::vector<bool> isTaken(incidents.size(), false);
	for (SIncident& inc : next) {
		std::sort(inc.members.begin(), inc.members.end());
		inc.vel /= std::max<size_t>(inc.members.size(), 1);
		int best = -1;
		size_t bestShared = 0;
		for (size_t j = 0; j < incidents.size(); ++j) {
			if (isTaken[j]) {
				continue;
			}
			const std::vector<ICoreUnit::Id>& prev = incidents[j].members;
			std::vector<ICoreUnit::Id> shared;
			std::set_intersection(inc.members.begin(), inc.members.end(), prev.begin(), prev.end(),
					std::back_inserter(shared));
			if ((shared.size() * 2 >= prev.size()) && (shared.size() > bestShared)) {
				best = j;
				bestShared = shared.size();
			}
		}
		if (best >= 0) {
			isTaken[best] = true;
			inc.id = incidents[best].id;
			inc.openFrame = incidents[best].openFrame;
		} else {
			inc.id = nextIncidentId++;
			inc.openFrame = frame;
		}
		inc.isAnswerable = CanOpenIncident(inc);
	}
	incidents.swap(next);
}

bool CRaidDefence::CanOpenIncident(const SIncident& incident) const
{
	// One test for sizing and response: some area our raiders move in reaches it (we have none yet: any), and an
	// undefended asset of ours is within protect's zone of it. A raid on a defended cluster, or on an ally's base,
	// doesn't raise the reserve
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	if (!defenderAreas.empty() && std::none_of(defenderAreas.begin(), defenderAreas.end(), [&](const SArea& a) {
		return terrainMgr->CanMobileReachAt(a.area, incident.pos, a.range);
	})) {
		return false;
	}
	return FindUndefended(incident.pos, circuit->GetMilitaryManager()->GetProtect()->GetZone()) != nullptr;
}

const CRaidDefence::SAsset* CRaidDefence::FindUndefended(const AIFloat3& pos, float radius) const
{
	const SAsset* best = nullptr;
	float minSqDist = SQUARE(radius);
	for (const SAsset& a : assets) {
		const float sqDist = a.pos.SqDistance2D(pos);
		if (!a.isDefended && (sqDist <= minSqDist)) {
			minSqDist = sqDist;
			best = &a;
		}
	}
	return best;
}

float CRaidDefence::GetRaidStrength() const
{
	float sum = 0.f;
	CMilitaryManager* militaryMgr = circuit->GetMilitaryManager();
	for (IFighterTask::FightType type : {IFighterTask::FightType::RAID, IFighterTask::FightType::RAID_DEFEND}) {
		for (const IFighterTask* t : militaryMgr->GetTasks(type)) {
			const ISquadTask* g = static_cast<const ISquadTask*>(t);
			if (!g->IsDead() && (g->GetLeader() != nullptr)) {
				sum += g->GetSafeThreat();
			}
		}
	}
	return sum;
}

/*
 * Assets and static cover
 */

float CRaidDefence::GetCoverAt(const AIFloat3& pos, std::vector<ICoreUnit::Id>* outTurrets) const
{
	float cover = 0.f;
	for (const STurret& t : turrets) {
		if (t.pos.SqDistance2D(pos) <= SQUARE(t.range)) {
			cover += t.power;
			if (outTurrets != nullptr) {
				outTurrets->push_back(t.id);
			}
		}
	}
	return cover;
}

bool CRaidDefence::IsCoverKept(const std::vector<ICoreUnit::Id>& ids) const
{
	return std::all_of(ids.begin(), ids.end(), [this](ICoreUnit::Id id) {
		return circuit->GetFriendlyUnit(id) != nullptr;
	});
}

void CRaidDefence::UpdateCover(int frame)
{
	// Our and allied finished ground turrets (the threat map's static defences that hit land)
	turrets.clear();
	for (auto& kv : circuit->GetFriendlyUnits()) {
		CAllyUnit* ally = kv.second;
		CCircuitDef* adef = ally->GetCircuitDef();
		if ((adef == nullptr) || adef->IsMobile() || !IFighterTask::IsStaticDefence(adef) || !adef->HasAntiLand()) {
			continue;
		}
		const float power = IFighterTask::GetCoverPower(ally);  // 0 for a nanoframe
		if (power > 0.f) {
			turrets.push_back({ally->GetId(), ally->GetPos(frame), adef->GetMaxRange(CCircuitDef::RangeType::LAND), power});
		}
	}

	// A cluster: the cover at its weakest mex of ours. Defended at Bmax, kept down to DEFENDED_KEEP x Bmax while
	// none of the turrets it was found defended with is gone. No cover: undefended whatever Bmax is
	CMetalManager* metalMgr = circuit->GetMetalManager();
	const CMetalData::Clusters& metalClusters = metalMgr->GetClusters();
	clusters.resize(metalClusters.size());
	std::vector<float> weakest(metalClusters.size(), std::numeric_limits<float>::max());
	std::vector<std::vector<ICoreUnit::Id>> covering(metalClusters.size());  // the turrets covering any of its mexes
	for (auto& kv : circuit->GetTeamUnits()) {
		CCircuitUnit* unit = kv.second;
		if (!unit->GetCircuitDef()->IsMex() || unit->GetUnit()->IsBeingBuilt()) {
			continue;
		}
		const AIFloat3& pos = unit->GetPos(frame);
		const int spot = metalMgr->FindNearestSpot(pos);
		if (spot < 0) {
			continue;
		}
		const int c = metalMgr->GetCluster(spot);
		weakest[c] = std::min(weakest[c], GetCoverAt(pos, &covering[c]));
	}
	auto judge = [this](bool& isDefended, std::vector<ICoreUnit::Id>& kept, float cover,
			std::vector<ICoreUnit::Id>& covering)
	{
		if ((cover <= 0.f) || (isDefended && ((cover < DEFENDED_KEEP * threatBmax) || !IsCoverKept(kept)))) {
			isDefended = false;
		} else if (!isDefended && (cover >= threatBmax)) {
			isDefended = true;
			kept.swap(covering);
		}
	};
	for (size_t c = 0; c < clusters.size(); ++c) {
		SCluster& cl = clusters[c];
		cl.hasMex = (weakest[c] != std::numeric_limits<float>::max());
		cl.cover = cl.hasMex ? weakest[c] : 0.f;
		if (!cl.hasMex) {
			cl.isDefended = false;
			continue;
		}
		judge(cl.isDefended, cl.turrets, cl.cover, covering[c]);
	}

	// Factories the same, at their own spot
	std::map<ICoreUnit::Id, SFactoryCover> covers;
	for (CCircuitUnit* factory : circuit->GetFactoryManager()->GetFactoryUnits()) {
		SFactoryCover fc;
		auto it = factoryCovers.find(factory->GetId());
		if (it != factoryCovers.end()) {
			fc = it->second;
		}
		std::vector<ICoreUnit::Id> ids;
		const float cover = GetCoverAt(factory->GetPos(frame), &ids);
		judge(fc.isDefended, fc.turrets, cover, ids);
		covers[factory->GetId()] = fc;
	}
	factoryCovers.swap(covers);
}

int CRaidDefence::GetPocketAt(const AIFloat3& pos) const
{
	// The pocket behind the cut nearest it, as the choke map nests them: the largest inner side holding its region
	const CChokeMap* chokeMap = circuit->GetChokeMap();
	if (!chokeMap->IsReady()) {
		return -1;
	}
	const int region = chokeMap->GetRegionAt(pos);
	if (region == CRegionGraph::NO_REGION) {
		return -1;
	}
	const std::vector<CChokeMap::SPocket>& pockets = chokeMap->GetPockets();
	int best = -1;
	int bestInner = 0;
	for (size_t p = 0; p < pockets.size(); ++p) {
		const std::vector<bool>& isInner = pockets[p].isInner;
		if ((region >= (int)isInner.size()) || !isInner[region]) {
			continue;
		}
		const int inner = std::count(isInner.begin(), isInner.end(), true);
		if (inner > bestInner) {
			best = p;
			bestInner = inner;
		}
	}
	return best;
}

bool CRaidDefence::IsPocketHeld(int pocket) const
{
	// quota.hold_choke: an attack group holds every entrance of the pocket
	if (pocket < 0) {
		return false;
	}
	const std::vector<CFrontline::SHold>& holds = circuit->GetMilitaryManager()->GetFrontline()->GetHolds();
	const std::vector<int>& chokeIds = circuit->GetChokeMap()->GetPockets()[pocket].chokeIds;
	return !chokeIds.empty() && std::all_of(chokeIds.begin(), chokeIds.end(), [&holds](int chokeId) {
		return std::any_of(holds.begin(), holds.end(), [chokeId](const CFrontline::SHold& h) {
			return (h.chokeId == chokeId) && (h.task != nullptr);
		});
	});
}

void CRaidDefence::CollectAssets(int frame)
{
	// Our finished mexes, constructors (the commander too) and factories, each second. Allies' aren't ours to defend
	assets.clear();
	CMetalManager* metalMgr = circuit->GetMetalManager();
	const CMetalData::Metals& spots = metalMgr->GetSpots();
	const CMetalData::Clusters& metalClusters = metalMgr->GetClusters();
	std::vector<int> clusterPockets(metalClusters.size(), -2);  // -2: not looked up yet
	auto getClusterPocket = [&](int c) {
		if (clusterPockets[c] == -2) {
			clusterPockets[c] = GetPocketAt(metalClusters[c].position);
		}
		return clusterPockets[c];
	};
	auto isClusterDefended = [this](int c) {
		return (c >= 0) && (c < (int)clusters.size()) && clusters[c].isDefended;
	};
	for (auto& kv : circuit->GetTeamUnits()) {
		CCircuitUnit* unit = kv.second;
		const CCircuitDef* cdef = unit->GetCircuitDef();
		if (!cdef->IsMex() || unit->GetUnit()->IsBeingBuilt()) {
			continue;
		}
		const AIFloat3& pos = unit->GetPos(frame);
		const int spot = metalMgr->FindNearestSpot(pos);
		if (spot < 0) {
			continue;
		}
		SAsset a;
		a.kind = SAsset::Kind::MEX;
		a.unitId = unit->GetId();
		a.pos = pos;
		a.value = cdef->GetCostM() * COST_SHARE + spots[spot].income * INCOME_SEC;
		a.cluster = metalMgr->GetCluster(spot);
		a.pocket = getClusterPocket(a.cluster);
		a.isDefended = isClusterDefended(a.cluster) || IsPocketHeld(a.pocket);
		assets.push_back(a);
	}
	for (CCircuitUnit* factory : circuit->GetFactoryManager()->GetFactoryUnits()) {
		SAsset a;
		a.kind = SAsset::Kind::FACTORY;
		a.unitId = factory->GetId();
		a.pos = factory->GetPos(frame);
		a.value = factory->GetCircuitDef()->GetCostM() * COST_SHARE;
		a.cluster = -1;
		a.pocket = GetPocketAt(a.pos);
		auto it = factoryCovers.find(a.unitId);
		a.isDefended = ((it != factoryCovers.end()) && it->second.isDefended) || IsPocketHeld(a.pocket);
		assets.push_back(a);
	}
	for (CCircuitUnit* worker : circuit->GetBuilderManager()->GetWorkers()) {
		const CCircuitDef* cdef = worker->GetCircuitDef();
		if (cdef->IsAbleToFly() || worker->IsDead()) {
			continue;  // flying builders dodge raiders on their own
		}
		SAsset a;
		a.kind = SAsset::Kind::BUILDER;
		a.unitId = worker->GetId();
		a.pos = worker->GetPos(frame);
		a.value = cdef->GetCostM() * COST_SHARE;
		const int c = metalMgr->FindNearestCluster(a.pos);
		a.cluster = ((c >= 0) && (a.pos.SqDistance2D(metalClusters[c].position) <= SQUARE(metalClusters[c].radius + 200.f)))
				? c : -1;
		a.pocket = GetPocketAt(a.pos);
		a.isDefended = isClusterDefended(a.cluster) || IsPocketHeld(a.pocket);
		assets.push_back(a);
	}
}

/*
 * Events
 */

void CRaidDefence::OnUnitDamaged(CCircuitUnit* unit, CEnemyInfo* attacker)
{
	if (!IsOn() || (attacker == nullptr)) {
		return;
	}
	const CCircuitDef* cdef = unit->GetCircuitDef();
	if (cdef->IsMex() || cdef->IsBuilder()) {  // mexes, constructors and the commander, factories
		attackers[attacker->GetId()] = circuit->GetLastFrame();
	}
}

bool CRaidDefence::IsRecentAttacker(ICoreUnit::Id id, int frame) const
{
	auto it = attackers.find(id);
	return (it != attackers.end()) && (frame - it->second <= ATTACKER_FRAMES);
}

void CRaidDefence::OnUnitDestroyed(CCircuitUnit* unit, CEnemyInfo* attacker)
{
	// Metal of our mexes, constructors and factories lost to the counted enemy roles
	if (!IsOn() || (attacker == nullptr)) {
		return;
	}
	const CCircuitDef* edef = attacker->GetCircuitDef();
	const CCircuitDef* cdef = unit->GetCircuitDef();
	const bool isAsset = cdef->IsMex() || cdef->IsBuilder();
	if (isAsset && (edef != nullptr) && edef->IsEnemyRoleAny(cfg.enemyRoleMask)) {
		lostMetal += cdef->GetCostM();
	}
}

void CRaidDefence::OnEnemyKilled(const CEnemyInfo* enemy, int attackerId)
{
	// Enemy raiders killed by defence groups, and what raid groups kill off our ground
	if (!IsOn() || (attackerId < 0)) {
		return;
	}
	CCircuitUnit* unit = circuit->GetTeamUnit(attackerId);
	const IUnitTask* task = (unit != nullptr) ? unit->GetTask() : nullptr;
	if ((task == nullptr) || (task->GetType() != IUnitTask::Type::FIGHTER)) {
		return;
	}
	const IFighterTask::FightType type = static_cast<const IFighterTask*>(task)->GetFightType();
	const CCircuitDef* edef = enemy->GetCircuitDef();
	if ((type == IFighterTask::FightType::RAID_DEFEND) && (edef != nullptr) && edef->IsEnemyRoleAny(cfg.enemyRoleMask)) {
		defendKills += enemy->GetCost();
	} else if ((type == IFighterTask::FightType::RAID) && !IsOurGround(enemy->GetPos())) {
		raidKills += enemy->GetCost();
	}
}

/*
 * Logs
 */

void CRaidDefence::LogState(int frame)
{
	// Every STATE_LOG_FRAMES while there is something to show
	if ((frame - stateLogFrame < STATE_LOG_FRAMES) || ((needD <= 0.f) && incidents.empty())) {
		return;
	}
	stateLogFrame = frame;
	int defended = 0;
	int undefended = 0;
	for (const SCluster& cl : clusters) {
		if (cl.hasMex) {
			++(cl.isDefended ? defended : undefended);
		}
	}
	std::string list;
	for (const SIncident& inc : incidents) {
		char entry[96];
		snprintf(entry, sizeof(entry), "%s#%i %.1f x%i at (%.0f, %.0f)%s", list.empty() ? "" : ", ", inc.id, inc.threat,
				(int)inc.members.size(), inc.pos.x, inc.pos.z, inc.isAnswerable ? "" : " (no asset)");
		list += entry;
	}
	circuit->LOG("RAIDDEF state: T %.1f, B %.1f (c %.2f), Bmax %.1f, one raider %.1f, D %.1f; clusters %i defended, "
			"%i not; incidents: %s", threatT, threatB, (threatT > 0.f) ? threatB / threatT : 0.f, threatBmax, oneRaider,
			needD, defended, undefended, list.empty() ? "none" : list.c_str());
}

void CRaidDefence::Log()
{
	if (!IsEnabled() || ((lostMetal <= 0.f) && (defendKills <= 0.f) && (raidKills <= 0.f) && (shareSamples == 0))) {
		return;
	}
	circuit->LOG("RAIDDEF minute: lost to raiders %.0f m, raiders killed by defenders %.0f m, killed by raid groups off "
			"our ground %.0f m, raiders defending %.0f%%%s", lostMetal, defendKills, raidKills,
			(shareSamples > 0) ? shareSum / shareSamples * 100.f : 0.f, isOver ? " (over)" : "");
	lostMetal = defendKills = raidKills = shareSum = 0.f;
	shareSamples = 0;
}

} // namespace circuit

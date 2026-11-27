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
#include "module/SpotScorer.h"
#include "resource/MetalManager.h"
#include "setup/SetupManager.h"
#include "task/builder/BuilderTask.h"
#include "task/fighter/DefendTask.h"
#include "task/fighter/RaidDefendTask.h"
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
#include <numeric>

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
		, nextSlotId(1)
		, postFrame(-POST_FRAMES)
		, pendingPostCount(-1)
		, pendingPostFrame(-1)
		, splitFrame(-SPLIT_FRAMES)
		, splitD(0.f)
		, splitWarm(false)
		, reserveR(0.f)
		, shortFrame(-1)
		, surplusFrame(-1)
		, isWaveOn(false)
		, warmFrame(-1)
		, isWarm(false)
		, warmSlotId(-1)
		, isNogo(false)
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

	// An incident whose protect ward has been in a no-go for NOGO_HALF_FRAMES counts at half its threat
	const CProtect* protect = circuit->GetMilitaryManager()->GetProtect();
	threatT = threatB = 0.f;
	isNogo = false;
	for (const SIncident& inc : incidents) {
		if (!inc.isAnswerable) {
			continue;
		}
		const CProtect::SIncidentState state = protect->GetIncidentState(inc.id);
		isNogo |= state.isOn && !state.isGo;
		const float threat = inc.threat * ((state.nogoFrames >= NOGO_HALF_FRAMES) ? 0.5f : 1.f);
		threatT += threat;
		threatB = std::max(threatB, threat);
	}
	const float cap = cfg.maxShare * GetRaidStrength();
	needD = std::min(cfg.margin * threatT, cap);
	UpdateWarm(frame, cap);

	CCircuitUnit* rep = FindRepUnit();
	UpdateReserve(frame, rep);
	UpdateResponse(frame, rep);

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
	ReleaseAll(frame);
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
			const SIncident& prev = incidents[best];
			inc.id = prev.id;
			inc.openFrame = prev.openFrame;
			inc.goalId = prev.goalId;
			inc.candId = prev.candId;
			inc.candFrame = prev.candFrame;
			inc.concededId = prev.concededId;
			inc.concededCluster = prev.concededCluster;
			inc.gather = prev.gather;
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
 * Reserve
 */

void CRaidDefence::UpdateWarm(int frame, float cap)
{
	// Once a wave ends (T back to 0), one post_min group waits at the biggest post for WARM_FRAMES, so the next wave
	// doesn't meet an empty reserve
	if (threatT > 0.f) {
		isWaveOn = true;
		warmFrame = -1;
	} else if (isWaveOn) {
		isWaveOn = false;
		warmFrame = frame;
	}
	isWarm = (warmFrame >= 0) && (frame - warmFrame < WARM_FRAMES);
	if (isWarm) {
		needD = std::min(std::max(needD, cfg.postMin * oneRaider), cap);
	}
}

void CRaidDefence::UpdateReserve(int frame, CCircuitUnit* rep)
{
	// Where it waits and how much, then recruit or release towards D
	if ((rep != nullptr) && (frame - postFrame >= POST_FRAMES)) {
		postFrame = frame;
		RefreshPosts(frame, rep);
	}
	// Every SPLIT_FRAMES, and at once when a wave starts or ends
	const bool isSplit = (frame - splitFrame >= SPLIT_FRAMES) || ((needD > 0.f) != (splitD > 0.f)) || (isWarm != splitWarm);
	if (isSplit) {
		splitFrame = frame;
		splitD = needD;
		splitWarm = isWarm;
		Split(frame, rep);
	}
	UpdateNeeds(frame);
	for (SSlot& slot : slots) {
		slot.map.Refresh(circuit, rep, slot.pos, POST_FRAMES, slot.isEscort ? MAP_MOVE_DIST : 1.f);
	}
	CountHave(frame);
	if (isSplit) {
		Rebalance(frame);
	}
	for (auto it = releasedFrames.begin(); it != releasedFrames.end();) {
		it = (frame - it->second > RELEASE_IMMUNE_FRAMES) ? releasedFrames.erase(it) : std::next(it);
	}
	if (reserveR < RECRUIT_SHORT * needD) {
		shortFrame = (shortFrame < 0) ? frame : shortFrame;
		if (frame - shortFrame >= TIER2_FRAMES) {
			Recruit(frame);
		}
	} else {
		shortFrame = -1;
	}
	if (reserveR > needD / RELEASE_RATIO) {
		surplusFrame = (surplusFrame < 0) ? frame : surplusFrame;
		if (frame - surplusFrame >= RELEASE_FRAMES) {
			Release(frame);
		}
	} else {
		surplusFrame = -1;
	}
}

CCircuitUnit* CRaidDefence::FindRepUnit() const
{
	// Posts are placed and travel maps made for one movement type: the most common among our raiders of the roles
	std::map<int, std::pair<int, CCircuitUnit*>> byMobile;  // mobile id -> count, its first unit
	for (CCircuitUnit* unit : circuit->GetMilitaryManager()->GetArmy()) {
		const CCircuitDef* cdef = unit->GetCircuitDef();
		if (!IsRole(cdef) || (unit->GetArea() == nullptr)) {
			continue;
		}
		std::pair<int, CCircuitUnit*>& entry = byMobile[cdef->GetMobileId()];
		entry.second = (entry.second == nullptr) ? unit : entry.second;
		++entry.first;
	}
	CCircuitUnit* rep = nullptr;
	int maxCount = 0;
	for (const auto& kv : byMobile) {
		if (kv.second.first > maxCount) {
			maxCount = kv.second.first;
			rep = kv.second.second;
		}
	}
	return rep;
}

void CRaidDefence::RefreshPosts(int frame, CCircuitUnit* rep)
{
	// Sites: our undefended mex clusters (their mexes' value, at the cluster's centre) and factories that rep reaches
	struct SSite {
		AIFloat3 pos;
		float value;
		int pocket;
	};
	std::vector<SSite> sites;
	std::map<int, size_t> clusterSites;  // cluster -> index in sites
	const CMetalData::Clusters& metalClusters = circuit->GetMetalManager()->GetClusters();
	for (const SAsset& a : assets) {
		if (a.isDefended || (a.kind == SAsset::Kind::BUILDER)) {
			continue;
		}
		if (a.kind == SAsset::Kind::MEX) {
			auto it = clusterSites.find(a.cluster);
			if (it != clusterSites.end()) {
				sites[it->second].value += a.value;
				continue;
			}
			clusterSites[a.cluster] = sites.size();
			sites.push_back({metalClusters[a.cluster].position, a.value, a.pocket});
		} else {
			sites.push_back({a.pos, a.value, a.pocket});
		}
	}
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	const float range = rep->GetCircuitDef()->GetMaxRange();
	sites.erase(std::remove_if(sites.begin(), sites.end(), [terrainMgr, rep, range](const SSite& s) {
		return !terrainMgr->CanMobileReachAt(rep->GetArea(), s.pos, range);
	}), sites.end());
	auto getCentre = [](const std::vector<const SSite*>& group, float& outValue) {
		AIFloat3 sum = ZeroVector;
		outValue = 0.f;
		for (const SSite* s : group) {
			sum += s->pos * s->value;
			outValue += s->value;
		}
		return (outValue > 0.f) ? AIFloat3(sum / outValue) : group.front()->pos;
	};

	// A post per pocket
	std::vector<SSlot> next;
	std::map<int, std::vector<const SSite*>> pocketSites;
	std::vector<const SSite*> openSites;
	for (const SSite& s : sites) {
		if (s.pocket >= 0) {
			pocketSites[s.pocket].push_back(&s);
		} else {
			openSites.push_back(&s);
		}
	}
	for (const auto& kv : pocketSites) {
		SSlot post;
		post.pocketKey = circuit->GetChokeMap()->GetPockets()[kv.first].chokeIds;
		post.pos = GetPocketPost(kv.first, getCentre(kv.second, post.value), rep);
		next.push_back(std::move(post));
	}

	// Open ground: each site, the most valuable first, joins the nearest open post within POST_JOIN_SEC by its travel
	// map (the old ones, then those opened before it), else opens one
	std::vector<SSlot> open;
	for (const SSlot& s : slots) {
		if (!s.isEscort && s.pocketKey.empty()) {
			open.push_back(s);
		}
	}
	std::vector<std::vector<const SSite*>> openGroups(open.size());
	std::sort(openSites.begin(), openSites.end(), [](const SSite* a, const SSite* b) { return a->value > b->value; });
	const int mobileId = rep->GetCircuitDef()->GetMobileId();
	const float speed = rep->GetCircuitDef()->GetSpeed();
	for (const SSite* site : openSites) {
		int best = -1;
		float minSec = POST_JOIN_SEC;
		for (size_t i = 0; i < open.size(); ++i) {
			const float sec = GetSlotSec(open[i], mobileId, site->pos, speed);
			if (sec <= minSec) {
				minSec = sec;
				best = i;
			}
		}
		if (best < 0) {
			best = open.size();
			open.emplace_back();
			open.back().pos = site->pos;
			openGroups.emplace_back();
		}
		openGroups[best].push_back(site);
	}
	for (size_t i = 0; i < open.size(); ++i) {
		if (!openGroups[i].empty()) {  // nothing left there: the post goes
			open[i].pos = terrainMgr->GetMovePosition(rep->GetArea(), getCentre(openGroups[i], open[i].value));
			next.push_back(std::move(open[i]));
		}
	}

	// A new post count waits POST_COUNT_FRAMES: meanwhile the posts stay, each site's value with the nearest
	const size_t postCount = std::count_if(slots.begin(), slots.end(), [](const SSlot& s) { return !s.isEscort; });
	if ((next.size() != postCount) && (postCount > 0)) {
		if ((int)next.size() != pendingPostCount) {
			pendingPostCount = next.size();
			pendingPostFrame = frame;
		}
		if (frame - pendingPostFrame < POST_COUNT_FRAMES) {
			for (SSlot& s : slots) {
				s.value = s.isEscort ? s.value : 0.f;
			}
			for (const SSite& site : sites) {
				SSlot* nearest = nullptr;
				for (SSlot& s : slots) {
					if (!s.isEscort && ((nearest == nullptr)
						|| (s.pos.SqDistance2D(site.pos) < nearest->pos.SqDistance2D(site.pos))))
					{
						nearest = &s;
					}
				}
				nearest->value += site.value;
			}
			return;
		}
	}
	pendingPostCount = -1;

	// The same post as before (its pocket, or the open post itself) keeps its id, its map, and its spot unless that
	// moved beyond POST_MOVE_DIST
	for (SSlot& post : next) {
		auto it = std::find_if(slots.begin(), slots.end(), [&post](const SSlot& s) {
			return !s.isEscort && ((post.id >= 0) ? (s.id == post.id) : (!post.pocketKey.empty() && (s.pocketKey == post.pocketKey)));
		});
		if (it == slots.end()) {
			post.id = nextSlotId++;
			continue;
		}
		post.id = it->id;
		post.map = it->map;
		if (post.pos.SqDistance2D(it->pos) <= SQUARE(POST_MOVE_DIST)) {
			post.pos = it->pos;
		}
	}
	std::vector<SSlot> escorts;
	for (SSlot& s : slots) {
		if (s.isEscort) {
			escorts.push_back(std::move(s));
		}
	}
	slots.swap(next);
	for (SSlot& e : escorts) {
		slots.push_back(std::move(e));
	}
}

AIFloat3 CRaidDefence::GetPocketPost(int pocket, const AIFloat3& centre, CCircuitUnit* rep) const
{
	// A pocket with 1 or 2 entrances, all defensible: the hold spot at its busiest one (as CFrontline's holds), if no
	// threat reaches it. Else the centre of its assets
	CChokeMap* chokeMap = circuit->GetChokeMap();
	const CChokeMap::SPocket& p = chokeMap->GetPockets()[pocket];
	if (p.isQualified && !p.chokes.empty() && (p.chokes.size() <= 2)) {
		int best = p.chokes.front();
		for (int c : p.chokes) {
			best = (chokeMap->GetChokeTraffic(c) > chokeMap->GetChokeTraffic(best)) ? c : best;
		}
		const CRegionGraph::SChoke& choke = chokeMap->GetGraph().GetChokes()[best];
		const bool isInnerA = (choke.regionA < (int)p.isInner.size()) && p.isInner[choke.regionA];
		CSpotScorer::SGoal goal;
		goal.type = CSpotScorer::GoalType::HOLD;
		goal.choke = best;
		goal.innerRegion = isInnerA ? choke.regionA : choke.regionB;
		goal.area = rep->GetArea();
		const AIFloat3 spot = circuit->GetMilitaryManager()->GetSpotScorer()->FindSpot(rep->GetCircuitDef(), goal);
		if (utils::is_valid(spot) && (circuit->GetThreatMap()->GetThreatAt(rep, spot) < THREAT_MIN)) {
			return spot;
		}
	}
	return circuit->GetTerrainManager()->GetMovePosition(rep->GetArea(), centre);
}

void CRaidDefence::Split(int frame, CCircuitUnit* rep)
{
	// The posts, the most valuable first: H = min(D, margin x B) by value, each share at least post_min x oneRaider.
	// The warm floor: all of it at the biggest post
	const float minShare = cfg.postMin * oneRaider;
	std::vector<SSlot*> posts;
	for (SSlot& s : slots) {
		s.weight = 0.f;
		if (!s.isEscort && (s.value > 0.f)) {
			posts.push_back(&s);
		}
	}
	std::sort(posts.begin(), posts.end(), [](const SSlot* a, const SSlot* b) { return a->value > b->value; });
	warmSlotId = -1;
	float postShare = 0.f;
	size_t keep = posts.size();
	if (isWarm && !posts.empty()) {
		posts.front()->weight = 1.f;
		warmSlotId = posts.front()->id;
		postShare = needD;
		keep = 1;
	} else if (!posts.empty()) {
		postShare = std::min(needD, cfg.margin * threatB);
		// The smallest post folds into the nearest kept one while there are too many, or its share is under the minimum
		std::vector<float> values(posts.size());
		for (size_t i = 0; i < posts.size(); ++i) {
			values[i] = posts[i]->value;
		}
		const float total = std::accumulate(values.begin(), values.end(), 0.f);
		while ((keep > 1) && ((keep > MAX_SLOTS) || (postShare * values[keep - 1] < minShare * total))) {
			--keep;
			const AIFloat3& pos = posts[keep]->pos;
			size_t nearest = 0;
			for (size_t i = 1; i < keep; ++i) {
				nearest = (posts[i]->pos.SqDistance2D(pos) < posts[nearest]->pos.SqDistance2D(pos)) ? i : nearest;
			}
			values[nearest] += values[keep];
		}
		for (size_t i = 0; i < keep; ++i) {
			posts[i]->weight = values[i] / total;
		}
	}

	// Escorts from the rest, up to half of it (all of D without a post), while the slots last: exposed constructors
	// outside defended clusters, those with an escort already first, then the nearest to a post by its travel map.
	// Each needs post_min x oneRaider less its riot guards there. What is left goes to the posts (UpdateNeeds)
	float budget = posts.empty() ? needD : (needD - postShare) * 0.5f;
	struct SCand {
		const SAsset* asset;
		float need;
		bool isOpen;
		float sec;
	};
	std::vector<SCand> cands;
	if ((rep != nullptr) && (budget > 0.f)) {
		CTerrainManager* terrainMgr = circuit->GetTerrainManager();
		const int mobileId = rep->GetCircuitDef()->GetMobileId();
		const float speed = std::max(rep->GetCircuitDef()->GetSpeed(), 1.f);
		const AIFloat3& basePos = circuit->GetSetupManager()->GetBasePos();
		for (const SAsset& a : assets) {
			CCircuitUnit* builder = ((a.kind == SAsset::Kind::BUILDER) && !a.isDefended) ? circuit->GetTeamUnit(a.unitId) : nullptr;
			if ((builder == nullptr) || !IsExposed(builder, frame) || !terrainMgr->CanMoveToPos(rep->GetArea(), a.pos)) {
				continue;
			}
			const float need = minShare - GetGuardStrength(builder, frame);
			if (need <= 0.f) {
				continue;  // its riot guards cover it
			}
			float sec = (keep > 0) ? std::numeric_limits<float>::max() : basePos.distance2D(a.pos) / speed;
			for (size_t i = 0; i < keep; ++i) {
				sec = std::min(sec, GetSlotSec(*posts[i], mobileId, a.pos, speed));
			}
			const bool isOpen = std::any_of(slots.begin(), slots.end(), [&a](const SSlot& s) {
				return s.isEscort && (s.builderId == a.unitId);
			});
			cands.push_back({&a, need, isOpen, sec});
		}
	}
	std::sort(cands.begin(), cands.end(), [](const SCand& a, const SCand& b) {
		return (a.isOpen != b.isOpen) ? a.isOpen : (a.sec < b.sec);
	});
	std::vector<SSlot> escorts;
	size_t count = keep;
	for (const SCand& c : cands) {
		if ((count >= MAX_SLOTS) || (c.need > budget)) {
			break;
		}
		auto it = std::find_if(slots.begin(), slots.end(), [&c](const SSlot& s) {
			return s.isEscort && (s.builderId == c.asset->unitId);
		});
		SSlot escort;
		if (it != slots.end()) {
			escort = std::move(*it);
		} else {
			escort.id = nextSlotId++;
			escort.isEscort = true;
			escort.builderId = c.asset->unitId;
		}
		escort.pos = c.asset->pos;
		escort.value = c.asset->value;
		escort.need = c.need;
		escorts.push_back(std::move(escort));
		budget -= c.need;
		++count;
	}
	slots.erase(std::remove_if(slots.begin(), slots.end(), [](const SSlot& s) { return s.isEscort; }), slots.end());
	for (SSlot& e : escorts) {
		slots.push_back(std::move(e));
	}
}

bool CRaidDefence::IsExposed(CCircuitUnit* builder, int frame) const
{
	// guard.summon's test: on ground that isn't safe, or its job is; not while it retreats
	const IUnitTask* task = builder->GetTask();
	if ((task == nullptr) || (task->GetType() == IUnitTask::Type::RETREAT)) {
		return false;
	}
	CMilitaryManager* militaryMgr = circuit->GetMilitaryManager();
	if (!militaryMgr->IsSafeGround(builder->GetPos(frame))) {
		return true;
	}
	if (task->GetType() != IUnitTask::Type::BUILDER) {
		return false;
	}
	const IBuilderTask* job = static_cast<const IBuilderTask*>(task);
	const AIFloat3 dest = (job->GetTarget() != nullptr) ? job->GetTarget()->GetPos(frame) : job->GetPosition();
	return utils::is_valid(dest) && !militaryMgr->IsSafeGround(dest);
}

float CRaidDefence::GetGuardStrength(CCircuitUnit* builder, int frame) const
{
	const IFighterTask* guard = circuit->GetMilitaryManager()->GetGuardTask(builder);
	if (guard == nullptr) {
		return 0.f;
	}
	const AIFloat3& pos = builder->GetPos(frame);
	float sum = 0.f;
	for (CCircuitUnit* unit : guard->GetAssignees()) {
		if (unit->GetPos(frame).SqDistance2D(pos) <= SQUARE(GUARD_NEAR_DIST)) {
			sum += IFighterTask::GetCoverPower(unit);
		}
	}
	return sum;
}

void CRaidDefence::UpdateNeeds(int frame)
{
	// Escorts follow their constructors (one that is gone needs nothing until the next split closes it); the posts
	// share D less the escorts' needs
	float escortSum = 0.f;
	for (SSlot& s : slots) {
		if (!s.isEscort) {
			continue;
		}
		CCircuitUnit* builder = circuit->GetTeamUnit(s.builderId);
		if (builder == nullptr) {
			s.need = 0.f;
			continue;
		}
		s.pos = builder->GetPos(frame);
		escortSum += s.need;
	}
	const float pool = std::max(needD - escortSum, 0.f);
	for (SSlot& s : slots) {
		if (!s.isEscort) {
			s.need = pool * s.weight;
		}
	}
}

void CRaidDefence::CountHave(int frame)
{
	for (SSlot& s : slots) {
		s.have = 0.f;
	}
	reserveR = 0.f;
	CMilitaryManager* militaryMgr = circuit->GetMilitaryManager();
	std::vector<CRaidDefendTask*> slotless;
	for (IFighterTask* t : militaryMgr->GetTasks(IFighterTask::FightType::RAID_DEFEND)) {
		CRaidDefendTask* group = static_cast<CRaidDefendTask*>(t);
		if (group->IsDead() || (group->GetLeader() == nullptr)) {
			continue;
		}
		const float strength = group->GetSafeThreat();
		reserveR += strength;
		if (group->IsHold() && (!isNogo || (strength >= cfg.postMin * oneRaider))) {
			group->SetHold(false);  // a held recruit's group is big enough, or no raid is in a no-go any more
		}
		SSlot* slot = FindSlotToEdit(group->GetSlot());
		if (slot != nullptr) {
			slot->have += strength;
		} else {
			slotless.push_back(group);
		}
	}
	// A parked solo hunter (quota.solo_hunt) holds the post nearest its parking spot
	for (IFighterTask* t : militaryMgr->GetTasks(IFighterTask::FightType::DEFEND)) {
		const CDefendTask* hunter = static_cast<const CDefendTask*>(t);
		CCircuitUnit* leader = hunter->GetLeader();
		if (!hunter->IsSolo() || hunter->IsDead() || (leader == nullptr) || (hunter->GetTarget() != nullptr)) {
			continue;
		}
		const AIFloat3 spot = militaryMgr->GetSoloParkSpot(hunter);
		if (!utils::is_valid(spot) || (leader->GetPos(frame).SqDistance2D(spot) > SQUARE(HUNTER_PARK_DIST))) {
			continue;
		}
		SSlot* post = nullptr;
		for (SSlot& s : slots) {
			if (!s.isEscort && ((post == nullptr) || (s.pos.SqDistance2D(spot) < post->pos.SqDistance2D(spot)))) {
				post = &s;
			}
		}
		if (post == nullptr) {
			break;
		}
		const float strength = hunter->GetSafeThreat();
		post->have += strength;
		reserveR += strength;
	}
	// A group without a slot takes the nearest one short of strength, else the nearest post
	auto isShort = [](const SSlot& s) { return s.have < s.need; };
	auto isPost = [](const SSlot& s) { return !s.isEscort; };
	for (CRaidDefendTask* group : slotless) {
		CCircuitUnit* leader = group->GetLeader();
		const AIFloat3& pos = leader->GetPos(frame);
		const int mobileId = leader->GetCircuitDef()->GetMobileId();
		SSlot* slot = FindNearestSlot(leader->GetArea(), mobileId, pos, group->GetLowestSpeed(), isShort);
		if (slot == nullptr) {
			slot = FindNearestSlot(leader->GetArea(), mobileId, pos, group->GetLowestSpeed(), isPost);
		}
		if (slot != nullptr) {
			group->SetSlot(slot->id);
			slot->have += group->GetSafeThreat();
		}
	}
}

void CRaidDefence::Rebalance(int frame)
{
	// A calm group whose post keeps its need without it goes to the nearest slot short of strength. Escorts and held
	// recruits stay
	auto isShort = [](const SSlot& s) { return s.have < s.need; };
	for (IFighterTask* t : circuit->GetMilitaryManager()->GetTasks(IFighterTask::FightType::RAID_DEFEND)) {
		CRaidDefendTask* group = static_cast<CRaidDefendTask*>(t);
		CCircuitUnit* leader = group->GetLeader();
		SSlot* from = FindSlotToEdit(group->GetSlot());
		if ((leader == nullptr) || (from == nullptr) || from->isEscort || group->IsHold() || !IsCalm(group, frame)) {
			continue;
		}
		const float strength = group->GetSafeThreat();
		if (from->have - strength < from->need) {
			continue;
		}
		SSlot* to = FindNearestSlot(leader->GetArea(), leader->GetCircuitDef()->GetMobileId(), leader->GetPos(frame),
				group->GetLowestSpeed(), isShort);
		if (to == nullptr) {
			continue;
		}
		group->SetSlot(to->id);
		from->have -= strength;
		to->have += strength;
		circuit->LOG("RAIDDEF move: a group of %i (%.1f) from slot #%i to slot #%i", (int)group->GetAssignees().size(),
				strength, from->id, to->id);
	}
}

void CRaidDefence::Recruit(int frame)
{
	// Tier 2: calm raid groups on our ground, or within respond_sec of a slot short of strength by its travel map,
	// nearest first. A whole group while it is at most WHOLE_GROUP_MOD x the shortfall, else its units nearest the slot
	CMilitaryManager* militaryMgr = circuit->GetMilitaryManager();
	const float respondSec = militaryMgr->GetProtect()->GetRespondSec();
	auto isShort = [](const SSlot& s) { return s.have < s.need; };
	struct SCand {
		CRaidTask* group;
		SSlot* slot;
		float sec;
	};
	std::vector<SCand> cands;
	for (IFighterTask* t : militaryMgr->GetTasks(IFighterTask::FightType::RAID)) {
		CRaidTask* group = static_cast<CRaidTask*>(t);
		CCircuitUnit* leader = group->GetLeader();
		if (group->IsDead() || (leader == nullptr) || !IsCalm(group, frame)
			|| (militaryMgr->GetRaidRallyHost() == group) || militaryMgr->IsRaidRallyJoiner(group))
		{
			continue;
		}
		const std::set<CCircuitUnit*>& units = group->GetAssignees();
		if (std::any_of(units.begin(), units.end(), [this, militaryMgr, frame](CCircuitUnit* u) {
			return !IsRole(u->GetCircuitDef()) || militaryMgr->IsScouting(u) || IsReleased(u, frame);
		})) {
			continue;
		}
		const AIFloat3& pos = leader->GetPos(frame);
		float sec;
		SSlot* slot = FindNearestSlot(leader->GetArea(), leader->GetCircuitDef()->GetMobileId(), pos,
				group->GetLowestSpeed(), isShort, &sec);
		if ((slot != nullptr) && ((sec <= respondSec) || IsOurGround(pos))) {
			cands.push_back({group, slot, sec});
		}
	}
	std::sort(cands.begin(), cands.end(), [](const SCand& a, const SCand& b) { return a.sec < b.sec; });
	for (const SCand& c : cands) {
		const float shortfall = needD - reserveR;
		if (shortfall <= 0.f) {
			break;
		}
		if (!isShort(*c.slot)) {
			continue;  // a nearer group filled it
		}
		const float strength = c.group->GetSafeThreat();
		std::vector<CCircuitUnit*> taken(c.group->GetAssignees().begin(), c.group->GetAssignees().end());
		const size_t total = taken.size();
		float takenStrength = strength;
		if (strength > WHOLE_GROUP_MOD * shortfall) {
			const AIFloat3& slotPos = c.slot->pos;
			std::sort(taken.begin(), taken.end(), [frame, &slotPos](CCircuitUnit* a, CCircuitUnit* b) {
				return a->GetPos(frame).SqDistance2D(slotPos) < b->GetPos(frame).SqDistance2D(slotPos);
			});
			// A unit's share of the group's strength: its own (power x sqrt(health share)) in the group's measure
			const float mod = strength / std::max(c.group->GetRallyStrength(), THREAT_MIN);
			takenStrength = 0.f;
			size_t n = 0;
			while ((n < taken.size()) && (takenStrength < shortfall)) {
				takenStrength += IFighterTask::GetCoverPower(taken[n++]) * mod;
			}
			taken.resize(n);
		}
		CRaidDefendTask* defenders = NewGroup(*c.slot);
		for (CCircuitUnit* unit : taken) {
			militaryMgr->AssignTask(unit, defenders);  // the last unit leaving ends the raid group
		}
		reserveR += takenStrength;
		c.slot->have += takenStrength;
		circuit->LOG("RAIDDEF recruit: %i of %i units of a raid group (%.1f) to slot #%i, R %.1f / D %.1f",
				(int)taken.size(), (int)total, takenStrength, c.slot->id, reserveR, needD);
	}
}

IFighterTask* CRaidDefence::MakeTask(CCircuitUnit* unit)
{
	// Tier 1: a new raider of the roles starts a group at the slot short the most that it reaches. While a raid
	// incident is in a no-go: at the post nearest it, held there until its group is big enough (CountHave), so
	// recruits don't walk one by one into the raid
	const CCircuitDef* cdef = unit->GetCircuitDef();
	if (!IsOn() || !IsRole(cdef) || !IFighterTask::IsArmedUnit(cdef) || (reserveR >= RECRUIT_SHORT * needD)) {
		return nullptr;
	}
	SSlot* best = nullptr;
	if (isNogo) {
		best = FindNearestSlot(unit->GetArea(), cdef->GetMobileId(), unit->GetPos(circuit->GetLastFrame()),
				cdef->GetSpeed(), [](const SSlot& s) { return !s.isEscort; });
	} else {
		CTerrainManager* terrainMgr = circuit->GetTerrainManager();
		float maxShort = 0.f;
		for (SSlot& s : slots) {
			if ((s.need - s.have > maxShort) && terrainMgr->CanMoveToPos(unit->GetArea(), s.pos)) {
				maxShort = s.need - s.have;
				best = &s;
			}
		}
	}
	if (best == nullptr) {
		return nullptr;
	}
	const float strength = IFighterTask::GetCoverPower(unit);  // counted until the next second
	best->have += strength;
	reserveR += strength;
	circuit->LOG("RAIDDEF recruit: a new %s to slot #%i%s, R %.1f / D %.1f", cdef->GetDef()->GetName(), best->id,
			isNogo ? " (held)" : "", reserveR, needD);
	CRaidDefendTask* group = NewGroup(*best);
	group->SetHold(isNogo);
	return group;
}

void CRaidDefence::Release(int frame)
{
	// Calm groups back to raiding while R stays at D or above: those without a slot first, then those at slots that
	// need nothing, then by the slot's value, the warm floor's post last
	struct SCand {
		CRaidDefendTask* group;
		int rank;
		float value;
	};
	std::vector<SCand> cands;
	for (IFighterTask* t : circuit->GetMilitaryManager()->GetTasks(IFighterTask::FightType::RAID_DEFEND)) {
		CRaidDefendTask* group = static_cast<CRaidDefendTask*>(t);
		if (group->IsDead() || (group->GetLeader() == nullptr) || !IsCalm(group, frame)) {
			continue;
		}
		const SSlot* slot = FindSlot(group->GetSlot());
		const int rank = (slot == nullptr) ? 0 : (slot->need <= 0.f) ? 1 : (slot->id == warmSlotId) ? 3 : 2;
		cands.push_back({group, rank, (slot != nullptr) ? slot->value : 0.f});
	}
	std::sort(cands.begin(), cands.end(), [](const SCand& a, const SCand& b) {
		return (a.rank != b.rank) ? (a.rank < b.rank) : (a.value < b.value);
	});
	for (const SCand& c : cands) {
		const float strength = c.group->GetSafeThreat();
		if ((needD > 0.f) && (reserveR - strength < needD)) {
			continue;
		}
		SSlot* slot = FindSlotToEdit(c.group->GetSlot());
		if (slot != nullptr) {
			slot->have -= strength;
		}
		reserveR -= strength;
		ReleaseGroup(c.group, frame);
	}
}

void CRaidDefence::ReleaseAll(int frame)
{
	const std::set<IFighterTask*>& tasks = circuit->GetMilitaryManager()->GetTasks(IFighterTask::FightType::RAID_DEFEND);
	const std::vector<IFighterTask*> groups(tasks.begin(), tasks.end());  // a group ends as its last unit leaves
	for (IFighterTask* t : groups) {
		ReleaseGroup(static_cast<CRaidDefendTask*>(t), frame);
	}
	slots.clear();
	reserveR = 0.f;
}

void CRaidDefence::ReleaseGroup(CRaidDefendTask* group, int frame)
{
	// A new raid group of its units, which can't be recruited again for RELEASE_IMMUNE_FRAMES
	const std::vector<CCircuitUnit*> units(group->GetAssignees().begin(), group->GetAssignees().end());
	if (group->IsDead() || units.empty()) {
		return;
	}
	circuit->LOG("RAIDDEF release: a group of %i (%.1f) from slot #%i, R %.1f / D %.1f", (int)units.size(),
			group->GetSafeThreat(), group->GetSlot(), reserveR, needD);
	CMilitaryManager* militaryMgr = circuit->GetMilitaryManager();
	IFighterTask* raid = militaryMgr->EnqueueTask(IFighterTask::FightType::RAID);
	for (CCircuitUnit* unit : units) {
		releasedFrames[unit->GetId()] = frame;
		militaryMgr->AssignTask(unit, raid);  // the last unit leaving ends the defence group
	}
}

CRaidDefendTask* CRaidDefence::NewGroup(const SSlot& slot)
{
	CRaidDefendTask* group = static_cast<CRaidDefendTask*>(
			circuit->GetMilitaryManager()->EnqueueTask(IFighterTask::FightType::RAID_DEFEND));
	group->SetSlot(slot.id);
	return group;
}

CRaidDefence::SSlot* CRaidDefence::FindNearestSlot(STerrainMapArea* area, int mobileId, const AIFloat3& pos,
		float speed, const std::function<bool (const SSlot&)>& isOk, float* outSec)
{
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	SSlot* best = nullptr;
	float minSec = std::numeric_limits<float>::max();
	for (SSlot& slot : slots) {
		if (!isOk(slot) || !terrainMgr->CanMoveToPos(area, slot.pos)) {
			continue;
		}
		const float sec = GetSlotSec(slot, mobileId, pos, speed);
		if (sec < minSec) {
			minSec = sec;
			best = &slot;
		}
	}
	if (outSec != nullptr) {
		*outSec = minSec;
	}
	return best;
}

const CRaidDefence::SSlot* CRaidDefence::FindSlot(int id) const
{
	auto it = std::find_if(slots.begin(), slots.end(), [id](const SSlot& s) { return s.id == id; });
	return (it != slots.end()) ? &*it : nullptr;
}

CRaidDefence::SSlot* CRaidDefence::FindSlotToEdit(int id)
{
	return const_cast<SSlot*>(static_cast<const CRaidDefence*>(this)->FindSlot(id));
}

bool CRaidDefence::HasRoom(int slotId) const
{
	const SSlot* slot = FindSlot(slotId);
	return (slot != nullptr) && (slot->have < slot->need);
}

float CRaidDefence::GetSlotSec(const SSlot& slot, int mobileId, const AIFloat3& pos, float speed) const
{
	if (slot.map.IsReady() && (slot.map.GetMobileId() == mobileId)) {
		const float sec = slot.map.GetSec(pos, speed);
		if (sec >= 0.f) {
			return sec;
		}
	}
	return slot.pos.distance2D(pos) / std::max(speed, 1.f);
}

float CRaidDefence::GetSlotSec(int slotId, int mobileId, const AIFloat3& pos, float speed) const
{
	const SSlot* slot = FindSlot(slotId);
	return (slot != nullptr) ? GetSlotSec(*slot, mobileId, pos, speed) : -1.f;
}

bool CRaidDefence::IsReleased(const CCircuitUnit* unit, int frame) const
{
	auto it = releasedFrames.find(unit->GetId());
	return (it != releasedFrames.end()) && (frame - it->second < RELEASE_IMMUNE_FRAMES);
}

bool CRaidDefence::IsCalm(const CRaidTask* group, int frame)
{
	return (group->GetTarget() == nullptr) && !group->IsFleeing(frame) && !group->IsProtecting() && !group->IsGathering();
}

bool CRaidDefence::CanLend(const CRaidDefendTask* group, const AIFloat3& goal, ICoreUnit::Id goalId) const
{
	// Not a held recruit. An escort to its own constructor; to anything else only while the constructor has no threat
	// and the goal is within escort_leash_sec of it (its slot's travel map)
	if (group->IsHold()) {
		return false;
	}
	const SSlot* slot = FindSlot(group->GetSlot());
	if ((slot == nullptr) || !slot->isEscort || (slot->builderId == goalId)) {
		return true;
	}
	CCircuitUnit* leader = group->GetLeader();
	CCircuitUnit* builder = circuit->GetTeamUnit(slot->builderId);
	if ((leader == nullptr) || (builder == nullptr)) {
		return true;  // a constructor gone: the escort guards nothing
	}
	if (circuit->GetThreatMap()->GetThreatAt(leader, builder->GetPos(circuit->GetLastFrame())) >= THREAT_MIN) {
		return false;
	}
	return GetSlotSec(*slot, leader->GetCircuitDef()->GetMobileId(), goal, group->GetLowestSpeed())
			<= cfg.escortLeashSec;
}

float CRaidDefence::GetPostBonus(const AIFloat3& pos) const
{
	const CMilitaryManager* militaryMgr = circuit->GetMilitaryManager();
	float bonus = 1.f;
	for (const SSlot& s : slots) {
		if (!s.isEscort) {
			bonus = std::max(bonus, militaryMgr->GetBaseThrBonus(s.pos.SqDistance2D(pos)));
		}
	}
	return bonus;
}

void CRaidDefence::FillFleeSpots(F3Vec& out) const
{
	if (!IsOn()) {
		return;
	}
	for (const SSlot& s : slots) {
		if (!s.isEscort && (s.have > 0.f)) {
			out.push_back(s.pos);
		}
	}
	for (const SIncident& inc : incidents) {
		if (inc.isAnswerable && utils::is_valid(inc.gather)) {
			out.push_back(inc.gather);
		}
	}
}

/*
 * Response
 */

void CRaidDefence::UpdateResponse(int frame, CCircuitUnit* rep)
{
	// Each answerable incident's goal (conceded once protect finds it late) and gathering post, sent to protect
	CProtect* protect = circuit->GetMilitaryManager()->GetProtect();
	std::vector<CProtect::SIncident> list;
	for (SIncident& inc : incidents) {
		if (!inc.isAnswerable) {
			continue;
		}
		const SAsset* goal = FindAsset(inc.goalId);
		if ((goal != nullptr) && (goal->kind != SAsset::Kind::BUILDER) && protect->GetIncidentState(inc.id).isLate) {
			// Concede and ambush: no group gets there before it dies, so meet the raid at its next asset
			inc.concededId = goal->unitId;
			inc.concededCluster = goal->cluster;
			inc.goalId = -1;
			circuit->LOG("RAIDDEF concede: incident #%i gives up %i (cluster %i)", inc.id, goal->unitId, goal->cluster);
		}
		UpdateGoal(inc, frame);
		goal = FindAsset(inc.goalId);
		if (goal == nullptr) {
			continue;
		}
		inc.gather = GetGather(inc, goal->pos, rep);
		CCircuitUnit* unit = circuit->GetTeamUnit(goal->unitId);
		const char* name = (unit != nullptr) ? unit->GetCircuitDef()->GetDef()->GetName() : "?";
		list.push_back({inc.id, goal->unitId, goal->pos, inc.gather, name, goal->value, inc.threat});
	}
	protect->SetIncidents(list);
}

void CRaidDefence::UpdateGoal(SIncident& inc, int frame)
{
	// The pick at once when there is no goal (or it is gone or defended), else only once another asset has been the
	// better pick for GOAL_SWITCH_FRAMES: a weaving blob doesn't drag the response back and forth
	const SAsset* pick = PickGoal(inc);
	if ((pick == nullptr) && (inc.concededId >= 0)) {
		inc.concededId = inc.concededCluster = -1;  // nothing left outside what it conceded: that lapses
		pick = PickGoal(inc);
	}
	if (pick == nullptr) {
		inc.goalId = inc.candId = -1;
		return;
	}
	if ((FindAsset(inc.goalId) == nullptr) || (pick->unitId == inc.goalId)) {
		inc.goalId = pick->unitId;
		inc.candId = -1;
		return;
	}
	if (inc.candId != pick->unitId) {
		inc.candId = pick->unitId;
		inc.candFrame = frame;
	} else if (frame - inc.candFrame >= GOAL_SWITCH_FRAMES) {
		inc.goalId = pick->unitId;
		inc.candId = -1;
	}
}

const CRaidDefence::SAsset* CRaidDefence::PickGoal(const SIncident& inc) const
{
	// Within protect's zone of the blob, outside what it conceded: the first asset within GOAL_TRACK_DIST of its track
	// ahead, else the nearest
	const float sqZone = SQUARE(circuit->GetMilitaryManager()->GetProtect()->GetZone());
	const float sqSpeed = inc.vel.SqLength2D();
	const bool isMoving = (sqSpeed >= SQUARE(GOAL_TRACK_SPEED));
	const SAsset* ahead = nullptr;
	const SAsset* nearest = nullptr;
	float minAhead = std::numeric_limits<float>::max();
	float minSqDist = std::numeric_limits<float>::max();
	for (const SAsset& a : assets) {
		const bool isConceded = (a.unitId == inc.concededId)
				|| ((inc.concededCluster >= 0) && (a.cluster == inc.concededCluster));
		const AIFloat3 to = a.pos - inc.pos;
		const float sqDist = to.SqLength2D();
		if (a.isDefended || isConceded || (sqDist > sqZone)) {
			continue;
		}
		if (sqDist < minSqDist) {
			minSqDist = sqDist;
			nearest = &a;
		}
		if (!isMoving) {
			continue;
		}
		const float t = (to.x * inc.vel.x + to.z * inc.vel.z) / sqSpeed;  // frames along the track
		const float sqOff = SQUARE(to.x - inc.vel.x * t) + SQUARE(to.z - inc.vel.z * t);
		if ((t >= 0.f) && (sqOff <= SQUARE(GOAL_TRACK_DIST)) && (t < minAhead)) {
			minAhead = t;
			ahead = &a;
		}
	}
	return (ahead != nullptr) ? ahead : nearest;
}

const CRaidDefence::SAsset* CRaidDefence::FindAsset(ICoreUnit::Id unitId) const
{
	if (unitId < 0) {
		return nullptr;
	}
	auto it = std::find_if(assets.begin(), assets.end(), [unitId](const SAsset& a) { return a.unitId == unitId; });
	return ((it != assets.end()) && !it->isDefended) ? &*it : nullptr;
}

AIFloat3 CRaidDefence::GetGather(const SIncident& inc, const AIFloat3& goal, CCircuitUnit* rep) const
{
	// Where committed groups meet in a no-go, out of the raid's threat (rep's view) and within protect's zone of the
	// goal: the last one while it holds, else the nearest post, else the first such spot stepping from the goal away
	// from the blob; else none
	if (rep == nullptr) {
		return -RgtVector;
	}
	CThreatMap* threatMap = circuit->GetThreatMap();
	const float zone = circuit->GetMilitaryManager()->GetProtect()->GetZone();
	auto isSafe = [&](const AIFloat3& p) {
		return (p.SqDistance2D(goal) <= SQUARE(zone)) && (threatMap->GetThreatAt(rep, p) < THREAT_MIN);
	};
	if (utils::is_valid(inc.gather) && isSafe(inc.gather)) {
		return inc.gather;
	}
	const SSlot* best = nullptr;
	for (const SSlot& s : slots) {
		if (!s.isEscort && isSafe(s.pos)
			&& ((best == nullptr) || (s.pos.SqDistance2D(goal) < best->pos.SqDistance2D(goal))))
		{
			best = &s;
		}
	}
	if (best != nullptr) {
		return best->pos;
	}
	AIFloat3 dir = goal - inc.pos;
	dir.y = 0.f;
	const float len = dir.Length2D();
	if (len < 1.f) {
		return -RgtVector;
	}
	dir /= len;
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	for (float d = GATHER_STEP; d <= zone; d += GATHER_STEP) {
		AIFloat3 p = goal + dir * d;
		CTerrainManager::CorrectPosition(p);
		if (terrainMgr->CanMoveToPos(rep->GetArea(), p) && isSafe(p)) {
			return terrainMgr->GetMovePosition(rep->GetArea(), p);
		}
	}
	return -RgtVector;
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
	if ((frame - stateLogFrame < STATE_LOG_FRAMES) || ((needD <= 0.f) && (reserveR <= 0.f) && incidents.empty())) {
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
		char entry[128];
		char goal[48] = "";
		if (inc.isAnswerable && (inc.goalId >= 0)) {
			snprintf(goal, sizeof(goal), " on %i%s", inc.goalId, utils::is_valid(inc.gather) ? " (gathering post)" : "");
		}
		snprintf(entry, sizeof(entry), "%s#%i %.1f x%i at (%.0f, %.0f)%s%s", list.empty() ? "" : ", ", inc.id,
				inc.threat, (int)inc.members.size(), inc.pos.x, inc.pos.z, inc.isAnswerable ? "" : " (no asset)", goal);
		list += entry;
	}
	std::string slotList;
	for (const SSlot& slot : slots) {
		char entry[96];
		if (slot.isEscort) {
			snprintf(entry, sizeof(entry), "%sescort #%i of %i %.1f/%.1f", slotList.empty() ? "" : ", ", slot.id,
					slot.builderId, slot.have, slot.need);
		} else {
			snprintf(entry, sizeof(entry), "%spost #%i (%.0f, %.0f) %.1f/%.1f%s", slotList.empty() ? "" : ", ", slot.id,
					slot.pos.x, slot.pos.z, slot.have, slot.need, (slot.id == warmSlotId) ? " warm" : "");
		}
		slotList += entry;
	}
	circuit->LOG("RAIDDEF state: T %.1f, B %.1f (c %.2f), Bmax %.1f, one raider %.1f, D %.1f%s, R %.1f; clusters %i "
			"defended, %i not; slots (have/need): %s; incidents: %s", threatT, threatB,
			(threatT > 0.f) ? threatB / threatT : 0.f, threatBmax, oneRaider, needD, isWarm ? " (warm)" : "", reserveR,
			defended, undefended, slotList.empty() ? "none" : slotList.c_str(), list.empty() ? "none" : list.c_str());
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

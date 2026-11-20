/*
 * CreepPlanner.cpp
 *
 *  Created on: Nov 11, 2026
 *      Author: Claude
 */

#include "module/CreepPlanner.h"
#include "module/BuilderManager.h"
#include "module/EconomyManager.h"
#include "module/MilitaryManager.h"
#include "module/TurretPicker.h"
#include "spring/SpringMap.h"
#include "terrain/ChokeMap.h"
#include "terrain/TerrainManager.h"
#include "unit/ally/AllyUnit.h"
#include "unit/enemy/EnemyUnit.h"
#include "unit/CircuitDef.h"
#include "unit/CircuitUnit.h"
#include "CircuitAI.h"
#include "util/Utils.h"

#include "Log.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace circuit {

using namespace springai;

static constexpr int MEMORY_FRAMES = FRAMES_PER_SEC * 120;  // losses and failed steps count this long
static constexpr int LOG_FRAMES = FRAMES_PER_SEC * 60;

CCreepPlanner::CCreepPlanner(CCircuitAI* circuit)
		: circuit(circuit)
		, skips{}
		, lineSize(0)
		, logFrame(0)
{
}

CCreepPlanner::~CCreepPlanner()
{
}

void CCreepPlanner::Update()
{
	const CTurretPicker::SConfig& cfg = circuit->GetMilitaryManager()->GetTurretPicker()->GetConfig();
	if (!cfg.isCreep) {
		pushers.clear();
		lanes.clear();
		return;
	}
	const int frame = circuit->GetLastFrame();
	losses.erase(std::remove_if(losses.begin(), losses.end(), [frame](const std::pair<AIFloat3, int>& l) {
		return frame - l.second > MEMORY_FRAMES;
	}), losses.end());
	for (auto it = fails.begin(); it != fails.end();) {
		it = (frame - it->second.frame > MEMORY_FRAMES) ? fails.erase(it) : std::next(it);
	}

	MakeLanes();
	UpdatePushers();

	CBuilderManager* builderMgr = circuit->GetBuilderManager();
	std::vector<bool> isTaken(lanes.size(), false);
	for (ICoreUnit::Id id : pushers) {
		CCircuitUnit* unit = circuit->GetTeamUnit(id);
		auto it = nextFrames.find(id);
		if ((unit == nullptr) || ((it != nextFrames.end()) && (frame < it->second)) || !builderMgr->IsTurretFree(unit)) {
			continue;
		}
		const SLane* lane = NearestLane(unit->GetPos(frame), &isTaken);
		if (lane == nullptr) {
			break;
		}
		const int result = builderMgr->OrderCreepTurret(unit, lane->end, lane->radius);
		if (result == 0) {
			continue;  // stall or budget: again next second
		}
		for (size_t i = 0; i < lanes.size(); ++i) {  // and every lane stepping to the same ground
			isTaken[i] = isTaken[i] || (lanes[i].end.SqDistance2D(lane->end) <= SQUARE(cfg.creepRadius));
		}
		nextFrames[id] = frame + int(cfg.creepInterval * FRAMES_PER_SEC);
		laneOf[id] = lane->chokeId;
		if (result == -1) {
			AddFail(lane->chokeId);
		}
	}

	if (frame >= logFrame) {
		logFrame = frame + LOG_FRAMES;
		circuit->LOG("CREEP %i pushers, %i lanes of a line of %i; skipped: %i not live, %i enemy-held, %i failed, "
				"%i losses, %i claimed, %i no turret near, %i in enemy reach; %i lost lately",
				(int)pushers.size(), (int)lanes.size(), lineSize, skips[NOT_LIVE], skips[ENEMY_HELD], skips[FAILED],
				skips[LOSSES], skips[CLAIMED], skips[NO_ANCHOR], skips[IN_REACH], (int)losses.size());
	}
}

bool CCreepPlanner::IsPusher(const CCircuitUnit* unit) const
{
	return std::find(pushers.begin(), pushers.end(), unit->GetId()) != pushers.end();
}

IBuilderTask* CCreepPlanner::MakeHoldTask(CCircuitUnit* unit)
{
	if (lanes.empty() || !IsPusher(unit)) {
		return nullptr;
	}
	CBuilderManager* builderMgr = circuit->GetBuilderManager();
	const CTurretPicker::SConfig& cfg = circuit->GetMilitaryManager()->GetTurretPicker()->GetConfig();
	const int frame = circuit->GetLastFrame();
	auto it = laneOf.find(unit->GetId());
	const SLane* lane = (it != laneOf.end()) ? FindLane(it->second) : nullptr;
	if (lane == nullptr) {
		lane = NearestLane(unit->GetPos(frame));
	}
	// Worth holding only while the account fills for this order soon (the cost the order itself asks)
	const float cost = builderMgr->GetCreepCost(unit, lane->end);
	if ((cost < 0.f) || builderMgr->IsMetalStalling()
		|| (circuit->GetMilitaryManager()->GetTurretWait(cost) > 2.f * cfg.creepInterval))
	{
		return nullptr;
	}
	CCircuitUnit* anchor = circuit->GetTeamUnit(lane->anchorId);
	if ((anchor == nullptr) || !builderMgr->IsTurretJobSafe(unit, anchor->GetPos(frame))) {
		return nullptr;
	}
	return builderMgr->EnqueueGuard(IBuilderTask::Priority::LOW, anchor, int(cfg.creepInterval * FRAMES_PER_SEC));
}

void CCreepPlanner::OnGunLost(const AIFloat3& pos)
{
	losses.push_back({pos, circuit->GetLastFrame()});
}

void CCreepPlanner::OnDropped(const AIFloat3& pos)
{
	const SLane* lane = NearestLane(pos);
	const float radius = circuit->GetMilitaryManager()->GetTurretPicker()->GetConfig().creepRadius;
	if ((lane != nullptr) && (lane->end.SqDistance2D(pos) <= SQUARE(2.f * radius))) {
		AddFail(lane->chokeId);
	}
}

void CCreepPlanner::UpdatePushers()
{
	const CTurretPicker::SConfig& cfg = circuit->GetMilitaryManager()->GetTurretPicker()->GetConfig();
	CBuilderManager* builderMgr = circuit->GetBuilderManager();
	// Half a pusher of hysteresis on income, so it doesn't flip at a multiple of creep_income
	const float income = circuit->GetEconomyManager()->GetAvgMetalIncome();
	auto wantAt = [&](float slack) {
		return std::max(cfg.creepBuilders, 0) + ((cfg.creepIncome > 0.f) ? int(income / cfg.creepIncome + slack) : 0);
	};
	const int have = (int)pushers.size();
	const int want = std::max(wantAt(0.f), std::min(have, wantAt(0.5f)));

	const std::set<CCircuitUnit*>& workers = builderMgr->GetWorkers();
	CCircuitUnit* comm = nullptr;
	if (!builderMgr->IsCommHiding()) {
		for (CCircuitUnit* u : workers) {
			if (u->GetCircuitDef()->IsRoleComm()) {
				comm = u;
				break;
			}
		}
	}
	// Gone, not a builder any more, or a commander gone into hiding
	pushers.erase(std::remove_if(pushers.begin(), pushers.end(), [&](ICoreUnit::Id id) {
		CCircuitUnit* u = circuit->GetTeamUnit(id);
		return (u == nullptr) || (workers.find(u) == workers.end()) || (u->GetCircuitDef()->IsRoleComm() && (u != comm));
	}), pushers.end());
	if ((comm != nullptr) && (want > 0) && !IsPusher(comm)) {
		pushers.insert(pushers.begin(), comm->GetId());  // the commander first
	}
	const int frame = circuit->GetLastFrame();
	while (!lanes.empty() && ((int)pushers.size() < want)) {
		CCircuitUnit* best = nullptr;
		float bestSqDist = std::numeric_limits<float>::max();
		for (CCircuitUnit* u : workers) {
			const CCircuitDef* cdef = u->GetCircuitDef();
			if (!cdef->IsMobile() || cdef->IsAbleToFly() || cdef->IsRoleComm() || IsPusher(u) || !builderMgr->IsTurretFree(u)) {
				continue;
			}
			const AIFloat3& pos = u->GetPos(frame);
			const float sqDist = NearestLane(pos)->end.SqDistance2D(pos);
			if (sqDist < bestSqDist) {
				bestSqDist = sqDist;
				best = u;
			}
		}
		if (best == nullptr) {
			break;
		}
		pushers.push_back(best->GetId());
	}
	if ((int)pushers.size() > want) {
		pushers.resize(want);
	}
	for (auto it = nextFrames.begin(); it != nextFrames.end();) {
		it = (std::find(pushers.begin(), pushers.end(), it->first) == pushers.end()) ? nextFrames.erase(it) : std::next(it);
	}
	for (auto it = laneOf.begin(); it != laneOf.end();) {
		it = (std::find(pushers.begin(), pushers.end(), it->first) == pushers.end()) ? laneOf.erase(it) : std::next(it);
	}
}

void CCreepPlanner::MakeLanes()
{
	lanes.clear();
	std::fill(std::begin(skips), std::end(skips), 0);
	lineSize = 0;
	CChokeMap* chokeMap = circuit->GetChokeMap();
	if (!chokeMap->IsReady()) {
		chokeMap->Request();
		return;
	}
	CTurretPicker* picker = circuit->GetMilitaryManager()->GetTurretPicker();
	const CTurretPicker::SConfig& cfg = picker->GetConfig();
	const int frame = circuit->GetLastFrame();
	const std::vector<int>& line = chokeMap->GetCurrentLine();
	lineSize = (int)line.size();
	if (line.empty()) {
		return;
	}
	const CRegionGraph& graph = chokeMap->GetGraph();
	const std::vector<CRegionGraph::SRegion>& regions = graph.GetRegions();
	const std::vector<CRegionGraph::SChoke>& chokes = graph.GetChokes();

	// Our finished guns (the anchors); every defence of ours and allies' (claims), unfinished ones of ours to help
	struct SGun {
		ICoreUnit::Id id;
		AIFloat3 pos;
		float range;
	};
	std::vector<SGun> guns;
	std::vector<AIFloat3> finished, helpable, foreign;  // defences: finished; ours unfinished or queued; allies' unfinished
	for (const auto& kv : circuit->GetFriendlyUnits()) {
		const CCircuitDef* cdef = kv.second->GetCircuitDef();
		if (!picker->IsDefence(cdef)) {
			continue;
		}
		const AIFloat3& pos = kv.second->GetPos(frame);
		const bool isOurs = (circuit->GetTeamUnit(kv.first) != nullptr);
		if (!kv.second->GetUnit()->IsBeingBuilt()) {
			finished.push_back(pos);
			if (isOurs && cdef->HasAntiLand()) {
				guns.push_back({kv.first, pos, cdef->GetMaxRange(CCircuitDef::RangeType::LAND)});
			}
		} else {
			(isOurs ? helpable : foreign).push_back(pos);
		}
	}
	for (const IBuilderTask* t : circuit->GetBuilderManager()->GetTasks(IBuilderTask::BuildType::DEFENCE)) {
		if ((t->GetTarget() == nullptr) && picker->IsDefence(t->GetBuildDef())) {
			helpable.push_back(t->GetPosition());
		}
	}
	// The enemy's known guns, at their land range + the margin
	std::vector<std::pair<AIFloat3, float>> enemyReach;
	for (const auto& kv : circuit->GetEnemyInfos()) {
		const CCircuitDef* edef = kv.second->GetCircuitDef();
		if (picker->IsEnemyBuilding(kv.second, false) && picker->IsGroundGun(edef)) {
			enemyReach.push_back({kv.second->GetPos(), edef->GetMaxRange(CCircuitDef::RangeType::LAND) + cfg.creepMargin});
		}
	}
	auto isInEnemyReach = [&enemyReach](const AIFloat3& p) {
		return std::any_of(enemyReach.begin(), enemyReach.end(), [&p](const std::pair<AIFloat3, float>& e) {
			return e.first.SqDistance2D(p) <= SQUARE(e.second);
		});
	};
	// Where the enemy is: its held ground's region centres (its side of the map while it holds none known)
	std::vector<AIFloat3> enemyAt;
	for (size_t r = 0; r < regions.size(); ++r) {
		if (chokeMap->IsEnemyGround(r)) {
			enemyAt.push_back(chokeMap->GetCellPos(regions[r].centre));
		}
	}
	for (size_t r = 0; enemyAt.empty() && (r < regions.size()); ++r) {
		if (chokeMap->IsEnemySide(r)) {
			enemyAt.push_back(chokeMap->GetCellPos(regions[r].centre));
		}
	}
	auto enemyDist = [&enemyAt](const AIFloat3& p) {
		float best = std::numeric_limits<float>::max();
		for (const AIFloat3& e : enemyAt) {
			best = std::min(best, p.SqDistance2D(e));
		}
		return best;
	};
	auto anyWithin = [](const std::vector<AIFloat3>& list, const AIFloat3& p, float radius) -> const AIFloat3* {
		for (const AIFloat3& q : list) {
			if (q.SqDistance2D(p) <= SQUARE(radius)) {
				return &q;
			}
		}
		return nullptr;
	};

	for (int c : line) {
		const CRegionGraph::SChoke& ch = chokes[c];
		if (!chokeMap->IsChokeLive(c, frame)) {
			++skips[NOT_LIVE];
			continue;
		}
		const int outer = chokeMap->IsOurGround(ch.regionA) ? ch.regionB
				: chokeMap->IsOurGround(ch.regionB) ? ch.regionA : CRegionGraph::NO_REGION;
		if ((outer == CRegionGraph::NO_REGION) || chokeMap->IsEnemyGround(outer)) {
			++skips[ENEMY_HELD];
			continue;
		}
		auto fit = fails.find(ch.id);
		if ((fit != fails.end()) && (fit->second.count >= 2)) {
			++skips[FAILED];
			continue;
		}
		if ((cfg.creepLosses > 0) && (std::count_if(losses.begin(), losses.end(), [&](const std::pair<AIFloat3, int>& l) {
				return chokeMap->GetRegionAt(l.first) == outer;
			}) >= cfg.creepLosses))
		{
			++skips[LOSSES];
			continue;
		}
		// The target: the outer region's way on toward the enemy, else its centre
		AIFloat3 target = chokeMap->GetCellPos(regions[outer].centre);
		float targetDist = std::numeric_limits<float>::max();
		for (int k : regions[outer].chokes) {
			if ((k == c) || chokeMap->IsArtilleryNear(k, frame)) {
				continue;
			}
			const AIFloat3 p = chokeMap->GetCellPos(chokes[k].centre);
			const float d = enemyDist(p);
			if (d < targetDist) {
				targetDist = d;
				target = p;
			}
		}
		// Claimed: a finished defence in the outer region or by the target. Being claimed: help ours, wait on allies'
		auto inClaim = [&](const AIFloat3& p) {
			return (chokeMap->GetRegionAt(p) == outer) || (p.SqDistance2D(target) <= SQUARE(cfg.creepRadius));
		};
		if (std::any_of(finished.begin(), finished.end(), inClaim) || std::any_of(foreign.begin(), foreign.end(), inClaim)) {
			++skips[CLAIMED];
			continue;
		}
		auto helpIt = std::find_if(helpable.begin(), helpable.end(), inClaim);
		// The anchor: our gun nearest the target, if it stands by this lane
		const SGun* anchor = nullptr;
		for (const SGun& g : guns) {
			if ((anchor == nullptr) || (g.pos.SqDistance2D(target) < anchor->pos.SqDistance2D(target))) {
				anchor = &g;
			}
		}
		const float fullStep = (anchor != nullptr) ? cfg.creepStep * anchor->range : 0.f;
		if ((anchor == nullptr) || (fullStep <= 0.f)
			|| (graph.GetLineDist(ch, anchor->pos.x, anchor->pos.z) > anchor->range + fullStep))
		{
			++skips[NO_ANCHOR];
			continue;
		}
		const float gap = anchor->pos.distance2D(target);
		if (helpIt != helpable.end()) {
			lanes.push_back({ch.id, *helpIt, cfg.creepRadius, anchor->id, gap});
			continue;
		}
		// The step: onto the target once within one, else a step toward it; out of the enemy's static reach
		AIFloat3 dir = target - anchor->pos;
		dir.y = 0.f;
		dir.SafeNormalize2D();
		const float stepLen = std::min(fullStep, gap);
		bool isFound = false;
		AIFloat3 end;
		for (float f : {1.f, 0.5f, 0.25f}) {
			end = anchor->pos + dir * (stepLen * f);
			CTerrainManager::CorrectPosition(end);
			if (!isInEnemyReach(end)) {
				isFound = true;
				break;
			}
		}
		if (!isFound) {
			++skips[IN_REACH];
			continue;
		}
		end.y = circuit->GetMap()->GetElevationAt(end.x, end.z);
		// Short of the target, the picker stays within half the step of its end: ahead of the turret behind it
		const float radius = (stepLen >= gap) ? cfg.creepRadius : std::min(cfg.creepRadius, 0.5f * stepLen);
		if (const AIFloat3* job = anyWithin(helpable, end, cfg.creepRadius)) {
			lanes.push_back({ch.id, *job, cfg.creepRadius, anchor->id, gap});  // a step already queued: help it
		} else if (anyWithin(finished, end, radius) || anyWithin(foreign, end, cfg.creepRadius)) {
			++skips[CLAIMED];
		} else {
			lanes.push_back({ch.id, end, radius, anchor->id, gap});
		}
	}
}

const CCreepPlanner::SLane* CCreepPlanner::FindLane(int chokeId) const
{
	auto it = std::find_if(lanes.begin(), lanes.end(), [chokeId](const SLane& l) { return l.chokeId == chokeId; });
	return (it != lanes.end()) ? &*it : nullptr;
}

const CCreepPlanner::SLane* CCreepPlanner::NearestLane(const AIFloat3& pos, const std::vector<bool>* isTaken) const
{
	const SLane* best = nullptr;
	float bestSqDist = std::numeric_limits<float>::max();
	for (size_t i = 0; i < lanes.size(); ++i) {
		const float sqDist = lanes[i].end.SqDistance2D(pos);
		if (((isTaken == nullptr) || !(*isTaken)[i]) && (sqDist < bestSqDist)) {
			bestSqDist = sqDist;
			best = &lanes[i];
		}
	}
	return best;
}

void CCreepPlanner::AddFail(int chokeId)
{
	SFail& f = fails[chokeId];
	++f.count;
	f.frame = circuit->GetLastFrame();
}

} // namespace circuit

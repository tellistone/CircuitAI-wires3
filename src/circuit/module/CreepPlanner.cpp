/*
 * CreepPlanner.cpp
 *
 *  Created on: Nov 11, 2026
 *      Author: Claude
 */

#include "module/CreepPlanner.h"
#include "module/BuilderManager.h"
#include "module/EconomyManager.h"
#include "module/Frontline.h"
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

static constexpr int MEMORY_FRAMES = FRAMES_PER_SEC * 120;  // losses count this long, and a lane's first rest
static constexpr int REST_RESET_FRAMES = FRAMES_PER_SEC * 600;  // a lane's rests stop doubling after this long
static constexpr int MAX_DOUBLINGS = 3;  // a rest is up to 2^3 = 8 x MEMORY_FRAMES (16 minutes)
static constexpr int LOG_FRAMES = FRAMES_PER_SEC * 60;
static constexpr int NEAREST_LANES = 3;  // a pusher takes the highest-ranked among this many lanes nearest it

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
	for (auto it = rests.begin(); it != rests.end();) {
		const SRest& r = it->second;
		const bool isStale = (frame >= r.until) && (frame - std::max(r.lastFail, r.lastRest) > REST_RESET_FRAMES);
		it = isStale ? rests.erase(it) : std::next(it);
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
		const SLane* lane = PickLane(unit->GetPos(frame), &isTaken);
		if (lane == nullptr) {
			break;
		}
		const int result = builderMgr->OrderCreepTurret(unit, lane->end, lane->radius, lane->isHelp);
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
		circuit->LOG("CREEP %i pushers, %i lanes of a line of %i; skipped: %i not live, %i resting, %i claimed, "
				"%i no turret, %i no progress, %i in enemy reach; %i lost lately",
				(int)pushers.size(), (int)lanes.size(), lineSize, skips[NOT_LIVE], skips[RESTING], skips[CLAIMED],
				skips[NO_ANCHOR], skips[NO_PROGRESS], skips[IN_REACH], (int)losses.size());
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
		lane = PickLane(unit->GetPos(frame));
	}
	if (lane->isHelp) {
		return nullptr;  // the next order helps the job there
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

void CCreepPlanner::OnDropped(const CCircuitUnit* builder, const AIFloat3& pos)
{
	auto it = (builder != nullptr) ? laneOf.find(builder->GetId()) : laneOf.end();
	if (it != laneOf.end()) {
		AddFail(it->second);
		return;
	}
	const SLane* lane = PickLane(pos);
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
			const float sqDist = PickLane(pos)->end.SqDistance2D(pos);
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
	const std::vector<CFrontline::SFront>& front = circuit->GetMilitaryManager()->GetFrontline()->GetFront();
	lineSize = (int)front.size();
	if (front.empty()) {
		return;
	}
	CTurretPicker* picker = circuit->GetMilitaryManager()->GetTurretPicker();
	const CTurretPicker::SConfig& cfg = picker->GetConfig();
	const int frame = circuit->GetLastFrame();
	const CRegionGraph& graph = chokeMap->GetGraph();
	const std::vector<CRegionGraph::SRegion>& regions = graph.GetRegions();
	const std::vector<CRegionGraph::SChoke>& chokes = graph.GetChokes();

	// Our finished guns (the anchors) with their regions; every finished defence, ours or allies' (claims)
	struct SGun {
		ICoreUnit::Id id;
		AIFloat3 pos;
		float range;
		int region;
	};
	std::vector<SGun> guns;
	std::vector<AIFloat3> finished;
	for (const auto& kv : circuit->GetFriendlyUnits()) {
		const CCircuitDef* cdef = kv.second->GetCircuitDef();
		if (!picker->IsDefence(cdef) || kv.second->GetUnit()->IsBeingBuilt()) {
			continue;
		}
		const AIFloat3& pos = kv.second->GetPos(frame);
		finished.push_back(pos);
		if (cdef->HasAntiLand() && (circuit->GetTeamUnit(kv.first) != nullptr)) {
			guns.push_back({kv.first, pos, cdef->GetMaxRange(CCircuitDef::RangeType::LAND), chokeMap->GetRegionAt(pos)});
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
	auto enemySqDist = [&enemyAt](const AIFloat3& p) {
		float best = std::numeric_limits<float>::max();
		for (const AIFloat3& e : enemyAt) {
			best = std::min(best, p.SqDistance2D(e));
		}
		return best;
	};
	auto isNear = [](const std::vector<AIFloat3>& list, const AIFloat3& p, float radius) {
		return std::any_of(list.begin(), list.end(), [&](const AIFloat3& q) { return q.SqDistance2D(p) <= SQUARE(radius); });
	};
	auto isGround = [](const CCircuitDef* d) { return d->HasAntiLand(); };

	for (const CFrontline::SFront& f : front) {  // highest rank first
		const int c = graph.FindChoke(f.chokeId);
		const int inner = graph.FindRegion(f.innerRegionId);
		if ((c < 0) || (inner < 0) || !chokeMap->IsChokeLive(c, frame)) {
			++skips[NOT_LIVE];  // or from before a rebuild
			continue;
		}
		auto rit = rests.find(f.chokeId);
		if ((rit != rests.end()) && (frame < rit->second.until)) {
			++skips[RESTING];
			continue;
		}
		const CRegionGraph::SChoke& ch = chokes[c];
		const int outer = graph.OtherSide(ch, inner);
		const AIFloat3 gapPos = chokeMap->GetCellPos(ch.centre);
		// The target: the outer region's way on toward the enemy, else its centre; the gap itself when the enemy holds
		// the outer region (the step stops at their range)
		AIFloat3 target = gapPos;
		if (!chokeMap->IsEnemyGround(outer)) {
			target = chokeMap->GetCellPos(regions[outer].centre);
			float targetSqDist = std::numeric_limits<float>::max();
			for (int k : regions[outer].chokes) {
				if ((k == c) || chokeMap->IsArtilleryNear(k, frame)) {
					continue;
				}
				const AIFloat3 p = chokeMap->GetCellPos(chokes[k].centre);
				const float d = enemySqDist(p);
				if (d < targetSqDist) {
					targetSqDist = d;
					target = p;
				}
			}
		}
		if (isNear(finished, target, cfg.creepRadius)) {
			++skips[CLAIMED];
			continue;
		}
		// The anchor: our gun nearest the target in the lane's two regions, else anywhere
		const SGun* anchor = nullptr;
		bool isInLane = false;
		for (const SGun& g : guns) {
			const bool isIn = (g.region == inner) || (g.region == outer);
			if ((isIn && !isInLane) || ((isIn == isInLane)
				&& ((anchor == nullptr) || (g.pos.SqDistance2D(target) < anchor->pos.SqDistance2D(target)))))
			{
				anchor = &g;
				isInLane = isIn;
			}
		}
		const float fullStep = (anchor != nullptr) ? cfg.creepStep * anchor->range : 0.f;
		if (fullStep <= 0.f) {
			++skips[NO_ANCHOR];
			continue;
		}
		if (enemySqDist(target) >= enemySqDist(anchor->pos)) {
			++skips[NO_PROGRESS];  // a flank or a back door: it doesn't bring us nearer the enemy
			continue;
		}
		// Through the gap the graph found, then on to the target
		const bool isPastGap = (anchor->region == outer) || (anchor->pos.SqDistance2D(gapPos) <= SQUARE(cfg.creepRadius));
		const AIFloat3 aim = isPastGap ? target : gapPos;
		const float dist = anchor->pos.distance2D(aim);
		const float stepLen = std::min(fullStep, dist);
		const bool isOnTarget = isPastGap && (stepLen >= dist);
		// Pulled back out of the enemy's static reach; a step shorter than a quarter of a full one is at their line
		AIFloat3 dir = aim - anchor->pos;
		dir.y = 0.f;
		dir.SafeNormalize2D();
		bool isFound = false;
		AIFloat3 end;
		float len = 0.f;
		for (float share : {1.f, 0.5f, 0.25f}) {
			len = stepLen * share;
			if ((len < 0.25f * fullStep) && !(isOnTarget && (share == 1.f))) {
				break;
			}
			end = anchor->pos + dir * len;
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
		// Our turrets lost in the outer region or a step of the end, since the lane's last rest
		const int since = (rit != rests.end()) ? rit->second.lastRest : -1;
		if ((cfg.creepLosses > 0) && (std::count_if(losses.begin(), losses.end(), [&](const std::pair<AIFloat3, int>& l) {
				return (l.second > since)
					&& ((chokeMap->GetRegionAt(l.first) == outer) || (l.first.SqDistance2D(end) <= SQUARE(fullStep)));
			}) >= cfg.creepLosses))
		{
			Rest(rests[f.chokeId]);
			++skips[RESTING];
			continue;
		}
		// The picker stays within half the step of its end, so the turret lands ahead of the one behind it. A finished
		// defence there takes the step; one being built or queued is helped
		const bool isAtTarget = isOnTarget && (len >= stepLen);  // not pulled back: the radius claims use about the target
		const float radius = isAtTarget ? cfg.creepRadius : std::min(cfg.creepRadius, 0.5f * len);
		AIFloat3 jobPos;
		if (isNear(finished, end, radius)) {
			++skips[CLAIMED];
		} else if (circuit->GetBuilderManager()->HasTurretDefence(end, radius, isGround, jobPos)) {
			lanes.push_back({f.chokeId, jobPos, radius, anchor->id, true});
		} else {
			lanes.push_back({f.chokeId, end, radius, anchor->id, false});
		}
	}
}

const CCreepPlanner::SLane* CCreepPlanner::FindLane(int chokeId) const
{
	auto it = std::find_if(lanes.begin(), lanes.end(), [chokeId](const SLane& l) { return l.chokeId == chokeId; });
	return (it != lanes.end()) ? &*it : nullptr;
}

const CCreepPlanner::SLane* CCreepPlanner::PickLane(const AIFloat3& pos, const std::vector<bool>* isTaken) const
{
	// The three nearest not taken; of them the first (lanes are in rank order)
	std::vector<std::pair<float, size_t>> near3;
	for (size_t i = 0; i < lanes.size(); ++i) {
		if ((isTaken == nullptr) || !(*isTaken)[i]) {
			near3.push_back({lanes[i].end.SqDistance2D(pos), i});
		}
	}
	if (near3.empty()) {
		return nullptr;
	}
	const size_t n = std::min<size_t>(near3.size(), NEAREST_LANES);
	std::partial_sort(near3.begin(), near3.begin() + n, near3.end());
	size_t best = near3[0].second;
	for (size_t j = 1; j < n; ++j) {
		best = std::min(best, near3[j].second);
	}
	return &lanes[best];
}

void CCreepPlanner::AddFail(int chokeId)
{
	SRest& r = rests[chokeId];
	++r.fails;
	r.lastFail = circuit->GetLastFrame();
	if (r.fails >= 2) {
		Rest(r);
	}
}

void CCreepPlanner::Rest(SRest& r)
{
	// 2 minutes, doubling while the lane keeps failing within 10 minutes of its last rest
	const int frame = circuit->GetLastFrame();
	if ((r.lastRest < 0) || (frame - r.lastRest > REST_RESET_FRAMES)) {
		r.rests = 0;
	}
	r.until = frame + (MEMORY_FRAMES << std::min(r.rests, MAX_DOUBLINGS));
	++r.rests;
	r.lastRest = frame;
	r.fails = 0;
}

} // namespace circuit

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

static constexpr int MEMORY_FRAMES = FRAMES_PER_SEC * 120;  // losses and failures count this long; a lane's first rest
static constexpr int REST_RESET_FRAMES = FRAMES_PER_SEC * 600;  // a lane's rests stop doubling after this long
static constexpr int MAX_DOUBLINGS = 3;  // a rest is up to 2^3 = 8 x MEMORY_FRAMES (16 minutes)
static constexpr int LOG_FRAMES = FRAMES_PER_SEC * 60;
static constexpr int NEAREST_LANES = 3;  // a pusher takes the highest-ranked among this many lanes nearest it
static constexpr float KEEP_TARGET = 1.2f;  // a lane keeps its target until another is this much nearer the enemy

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
	const CTurretPicker* picker = circuit->GetMilitaryManager()->GetTurretPicker();
	const CTurretPicker::SConfig& cfg = picker->GetConfig();
	if (!cfg.isCreep) {
		pushers.clear();
		lanes.clear();
		return;
	}
	const int frame = circuit->GetLastFrame();
	losses.erase(std::remove_if(losses.begin(), losses.end(), [frame](const std::pair<AIFloat3, int>& l) {
		return frame - l.second > MEMORY_FRAMES;
	}), losses.end());

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
		// A step's end under threat (the picker's creep rule) waits: not a failed step
		const SLane* lane = PickLane(unit->GetPos(frame), &isTaken);
		while ((lane != nullptr) && !lane->isHelp && picker->IsCreepThreatened(unit, lane->end)) {
			++skips[THREATENED];
			isTaken[lane - lanes.data()] = true;
			lane = PickLane(unit->GetPos(frame), &isTaken);
		}
		if (lane == nullptr) {
			break;
		}
		const int result = builderMgr->OrderCreepTurret(unit, lane->end, lane->radius, lane->isHelp, !lane->isAtLine);
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
		circuit->LOG("CREEP %i pushers, %i lanes of a line of %i; this second skipped: %i not live, %i resting, "
				"%i claimed, %i no turret, %i no progress, %i in enemy reach, %i ends under threat; %i lost lately",
				(int)pushers.size(), (int)lanes.size(), lineSize, skips[NOT_LIVE], skips[RESTING], skips[CLAIMED],
				skips[NO_ANCHOR], skips[NO_PROGRESS], skips[IN_REACH], skips[THREATENED], (int)losses.size());
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
	const CTurretPicker* picker = circuit->GetMilitaryManager()->GetTurretPicker();
	const CTurretPicker::SConfig& cfg = picker->GetConfig();
	const int frame = circuit->GetLastFrame();
	auto it = laneOf.find(unit->GetId());
	const SLane* lane = (it != laneOf.end()) ? FindLane(it->second) : nullptr;
	if (lane == nullptr) {
		lane = PickLane(unit->GetPos(frame));
	}
	// Not for a job to help (the next order does), nor while the step's end is under threat (an army parked off the
	// front: work elsewhere meanwhile). Worth it only while the account fills for this order soon
	if (lane->isHelp || builderMgr->IsMetalStalling() || picker->IsCreepThreatened(unit, lane->end)) {
		return nullptr;
	}
	const float cost = builderMgr->GetCreepCost(unit, lane->end, !lane->isAtLine);
	if ((cost < 0.f) || (circuit->GetMilitaryManager()->GetTurretWait(cost) > 2.f * cfg.creepInterval)) {
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
	auto laneSqDist = [this, frame](CCircuitUnit* u) {
		const AIFloat3& pos = u->GetPos(frame);
		return PickLane(pos)->end.SqDistance2D(pos);
	};
	// The free mobile ground constructor nearest a lane, not a pusher yet
	auto nearestFree = [&](float& outSqDist) {
		CCircuitUnit* best = nullptr;
		outSqDist = std::numeric_limits<float>::max();
		for (CCircuitUnit* u : workers) {
			const CCircuitDef* cdef = u->GetCircuitDef();
			if (!cdef->IsMobile() || cdef->IsAbleToFly() || cdef->IsRoleComm() || IsPusher(u) || !builderMgr->IsTurretFree(u)) {
				continue;
			}
			const float sqDist = laneSqDist(u);
			if (sqDist < outSqDist) {
				outSqDist = sqDist;
				best = u;
			}
		}
		return best;
	};
	float sqDist;
	while (!lanes.empty() && ((int)pushers.size() < want)) {
		CCircuitUnit* best = nearestFree(sqDist);
		if (best == nullptr) {
			break;
		}
		pushers.push_back(best->GetId());
	}
	if ((int)pushers.size() > want) {
		pushers.resize(want);
	}
	// A free constructor pusher gives way to a free constructor at under half its distance from a lane, one a second:
	// the front moves, and a pusher wanders off between steps
	if (!lanes.empty()) {
		auto farthest = pushers.end();
		float farSqDist = 0.f;
		for (auto it = pushers.begin(); it != pushers.end(); ++it) {
			CCircuitUnit* u = circuit->GetTeamUnit(*it);
			if (u->GetCircuitDef()->IsRoleComm() || !builderMgr->IsTurretFree(u)) {
				continue;
			}
			const float d = laneSqDist(u);
			if (d > farSqDist) {
				farSqDist = d;
				farthest = it;
			}
		}
		CCircuitUnit* best = (farthest != pushers.end()) ? nearestFree(sqDist) : nullptr;
		if ((best != nullptr) && (sqDist < 0.25f * farSqDist)) {
			*farthest = best->GetId();
		}
	}
	for (auto it = nextFrames.begin(); it != nextFrames.end();) {
		it = (std::find(pushers.begin(), pushers.end(), it->first) == pushers.end()) ? nextFrames.erase(it) : std::next(it);
	}
	for (auto it = laneOf.begin(); it != laneOf.end();) {
		it = (std::find(pushers.begin(), pushers.end(), it->first) == pushers.end()) ? laneOf.erase(it) : std::next(it);
	}
}


void CCreepPlanner::Gather()
{
	CTurretPicker* picker = circuit->GetMilitaryManager()->GetTurretPicker();
	const float margin = picker->GetConfig().creepMargin;
	CChokeMap* chokeMap = circuit->GetChokeMap();
	const std::vector<CRegionGraph::SRegion>& regions = chokeMap->GetGraph().GetRegions();
	const int frame = circuit->GetLastFrame();
	guns.clear();
	for (const auto& kv : circuit->GetTeamUnits()) {
		CCircuitUnit* unit = kv.second;
		const CCircuitDef* cdef = unit->GetCircuitDef();
		if (picker->IsGroundGun(cdef) && !unit->GetUnit()->IsBeingBuilt()) {
			const AIFloat3& pos = unit->GetPos(frame);
			guns.push_back({kv.first, pos, cdef->GetMaxRange(CCircuitDef::RangeType::LAND), chokeMap->GetRegionAt(pos)});
		}
	}
	enemyReach.clear();
	for (const auto& kv : circuit->GetEnemyInfos()) {
		const CCircuitDef* edef = kv.second->GetCircuitDef();
		if (picker->IsEnemyBuilding(kv.second, false) && picker->IsGroundGun(edef)) {
			enemyReach.push_back({kv.second->GetPos(), edef->GetMaxRange(CCircuitDef::RangeType::LAND) + margin});
		}
	}
	enemyAt.clear();
	for (int pass = 0; (pass < 2) && enemyAt.empty(); ++pass) {  // its held ground, else its side of the map
		for (int r = 0; r < (int)regions.size(); ++r) {
			if ((pass == 0) ? chokeMap->IsEnemyGround(r) : chokeMap->IsEnemySide(r)) {
				enemyAt.push_back({r, chokeMap->GetCellPos(regions[r].centre)});
			}
		}
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
	// Lanes off the line, their rests over and stale, are forgotten
	const int frame = circuit->GetLastFrame();
	for (auto it = memos.begin(); it != memos.end();) {
		const SMemo& m = it->second;
		const bool isOn = std::any_of(front.begin(), front.end(), [&](const CFrontline::SFront& f) {
			return f.chokeId == it->first;
		});
		const bool isStale = (frame >= m.until) && (frame - std::max(m.lastFail, m.lastRest) > REST_RESET_FRAMES);
		it = (!isOn && isStale) ? memos.erase(it) : std::next(it);
	}
	if (front.empty()) {
		return;
	}
	Gather();
	if (guns.empty()) {
		skips[NO_ANCHOR] = lineSize;  // the creep starts from a first turret another caller builds
		return;
	}
	const CTurretPicker::SConfig& cfg = circuit->GetMilitaryManager()->GetTurretPicker()->GetConfig();
	const CRegionGraph& graph = chokeMap->GetGraph();
	const std::vector<CRegionGraph::SChoke>& chokes = graph.GetChokes();
	CBuilderManager* builderMgr = circuit->GetBuilderManager();
	using DF = CBuilderManager::DefenceFound;
	auto isGround = [](const CCircuitDef* d) { return d->HasAntiLand(); };
	AIFloat3 defencePos;

	for (const CFrontline::SFront& f : front) {  // highest rank first
		const int c = graph.FindChoke(f.chokeId);
		const int inner = graph.FindRegion(f.innerRegionId);
		if ((c < 0) || (inner < 0) || ((chokes[c].regionA != inner) && (chokes[c].regionB != inner))
			|| !chokeMap->IsChokeLive(c, frame))
		{
			++skips[NOT_LIVE];  // or stale from before a rebuild
			continue;
		}
		SMemo& memo = memos[f.chokeId];
		if (frame < memo.until) {
			++skips[RESTING];
			continue;
		}
		const int outer = graph.OtherSide(chokes[c], inner);
		const AIFloat3 target = ChooseTarget(c, outer, memo);
		if (builderMgr->FindTurretDefence(target, cfg.creepRadius, isGround, defencePos) == DF::FINISHED) {
			++skips[CLAIMED];
			continue;
		}
		const SGun* anchor = ChooseAnchor(inner, outer, target);
		const float fullStep = cfg.creepStep * anchor->range;
		if (fullStep <= 0.f) {
			++skips[NO_ANCHOR];
			continue;
		}
		if (EnemySqDist(target, outer) >= EnemySqDist(anchor->pos, outer)) {
			++skips[NO_PROGRESS];  // a flank or a back door: it doesn't bring us nearer the enemy
			continue;
		}
		// Through the gap the graph found, then on to the target, landing on it once within a step
		const AIFloat3 gapPos = chokeMap->GetCellPos(chokes[c].centre);
		const bool isPastGap = (anchor->region == outer) || (anchor->pos.SqDistance2D(gapPos) <= SQUARE(cfg.creepRadius));
		const AIFloat3 aim = isPastGap ? target : gapPos;
		AIFloat3 end;
		float len;
		if (!MakeStep(*anchor, aim, fullStep, end, len)) {
			++skips[IN_REACH];
			continue;
		}
		const float dist = anchor->pos.distance2D(aim);
		const bool isAtTarget = isPastGap && (len >= dist);
		const bool isPulledBack = (len < std::min(fullStep, dist));
		// Our turrets lost in the outer region or a step of the end, since the lane's last rest
		if ((cfg.creepLosses > 0) && (std::count_if(losses.begin(), losses.end(), [&](const std::pair<AIFloat3, int>& l) {
				return (l.second > memo.lastRest)
					&& ((chokeMap->GetRegionAt(l.first) == outer) || (l.first.SqDistance2D(end) <= SQUARE(fullStep)));
			}) >= cfg.creepLosses))
		{
			Rest(memo);
			++skips[RESTING];
			continue;
		}
		// The picker stays within half the step of its end, so the turret lands ahead of the one behind it (on the
		// target: creep_radius, as its claim). A finished ground defence there, or an ally's being built, takes the
		// step; one of ours being built or queued is helped
		const float radius = isAtTarget ? cfg.creepRadius : std::min(cfg.creepRadius, 0.5f * len);
		const bool isAtLine = isAtTarget || isPulledBack;
		switch (builderMgr->FindTurretDefence(end, radius, isGround, defencePos)) {
			case DF::FINISHED:
			case DF::ALLY_PENDING: {
				++skips[CLAIMED];
			} break;
			case DF::OURS_PENDING: {
				lanes.push_back({f.chokeId, defencePos, radius, anchor->id, true, isAtLine});
			} break;
			case DF::NONE: {
				lanes.push_back({f.chokeId, end, radius, anchor->id, false, isAtLine});
			} break;
		}
	}
}

AIFloat3 CCreepPlanner::ChooseTarget(int choke, int outer, SMemo& memo) const
{
	CChokeMap* chokeMap = circuit->GetChokeMap();
	const CRegionGraph& graph = chokeMap->GetGraph();
	const std::vector<CRegionGraph::SChoke>& chokes = graph.GetChokes();
	const CRegionGraph::SRegion& region = graph.GetRegions()[outer];
	const int frame = circuit->GetLastFrame();
	// The enemy's last ground: its heart, not an exit
	const bool isBeyond = std::any_of(enemyAt.begin(), enemyAt.end(), [outer](const std::pair<int, AIFloat3>& e) {
		return e.first != outer;
	});
	AIFloat3 target = chokeMap->GetCellPos(region.centre);
	int targetId = -1;
	float bestSqDist = std::numeric_limits<float>::max();
	for (int k : region.chokes) {
		if (!isBeyond || (k == choke) || chokeMap->IsArtilleryNear(k, frame)) {
			continue;
		}
		const AIFloat3 p = chokeMap->GetCellPos(chokes[k].centre);
		const float d = EnemySqDist(p, outer) / ((chokes[k].id == memo.targetId) ? SQUARE(KEEP_TARGET) : 1.f);
		if (d < bestSqDist) {
			bestSqDist = d;
			target = p;
			targetId = chokes[k].id;
		}
	}
	memo.targetId = targetId;
	return target;
}

const CCreepPlanner::SGun* CCreepPlanner::ChooseAnchor(int inner, int outer, const AIFloat3& target) const
{
	// Our gun nearest the target in the lane's two regions, else anywhere (guns isn't empty)
	auto nearest = [&](bool isInLane) {
		const SGun* best = nullptr;
		for (const SGun& g : guns) {
			if ((!isInLane || (g.region == inner) || (g.region == outer))
				&& ((best == nullptr) || (g.pos.SqDistance2D(target) < best->pos.SqDistance2D(target))))
			{
				best = &g;
			}
		}
		return best;
	};
	const SGun* anchor = nearest(true);
	return (anchor != nullptr) ? anchor : nearest(false);
}

bool CCreepPlanner::MakeStep(const SGun& anchor, const AIFloat3& aim, float fullStep, AIFloat3& outEnd,
		float& outLen) const
{
	// A full step, or onto aim if nearer; pulled back to a half, then a quarter (no shorter than a quarter of a full
	// step), until out of every known enemy static's reach
	AIFloat3 dir = aim - anchor.pos;
	dir.y = 0.f;
	dir.SafeNormalize2D();
	const float stepLen = std::min(fullStep, anchor.pos.distance2D(aim));
	for (float share : {1.f, 0.5f, 0.25f}) {
		const float len = stepLen * share;
		if ((share < 1.f) && (len < 0.25f * fullStep)) {
			break;
		}
		AIFloat3 end = anchor.pos + dir * len;
		CTerrainManager::CorrectPosition(end);
		if (!IsInEnemyReach(end)) {
			end.y = circuit->GetMap()->GetElevationAt(end.x, end.z);
			outEnd = end;
			outLen = len;
			return len > 0.f;
		}
	}
	return false;
}

float CCreepPlanner::EnemySqDist(const AIFloat3& pos, int skipRegion) const
{
	// The outer region itself only when it is all the enemy holds
	float beyond = std::numeric_limits<float>::max(), own = beyond;
	for (const auto& e : enemyAt) {
		float& m = (e.first == skipRegion) ? own : beyond;
		m = std::min(m, pos.SqDistance2D(e.second));
	}
	return (beyond < std::numeric_limits<float>::max()) ? beyond : own;
}

bool CCreepPlanner::IsInEnemyReach(const AIFloat3& pos) const
{
	return std::any_of(enemyReach.begin(), enemyReach.end(), [&pos](const std::pair<AIFloat3, float>& e) {
		return e.first.SqDistance2D(pos) <= SQUARE(e.second);
	});
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
	// Two within 2 minutes rest the lane
	const int frame = circuit->GetLastFrame();
	SMemo& m = memos[chokeId];
	m.fails = (frame - m.lastFail > MEMORY_FRAMES) ? 1 : m.fails + 1;
	m.lastFail = frame;
	if (m.fails >= 2) {
		Rest(m);
	}
}

void CCreepPlanner::Rest(SMemo& m)
{
	// 2 minutes, doubling while the lane keeps failing within 10 minutes of its last rest
	const int frame = circuit->GetLastFrame();
	if ((m.lastRest < 0) || (frame - m.lastRest > REST_RESET_FRAMES)) {
		m.rests = 0;
	}
	m.until = frame + (MEMORY_FRAMES << std::min(m.rests, MAX_DOUBLINGS));
	++m.rests;
	m.lastRest = frame;
	m.fails = 0;
}

} // namespace circuit

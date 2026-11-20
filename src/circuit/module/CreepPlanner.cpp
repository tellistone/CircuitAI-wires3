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
#include "task/builder/DefenceTask.h"
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
static constexpr int NEAREST_LANES = 3;  // the commander takes the highest-ranked among this many lanes nearest it
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
	const CTurretPicker::SConfig& cfg = circuit->GetMilitaryManager()->GetTurretPicker()->GetConfig();
	if (!cfg.isCreep) {
		lanes.clear();
		return;
	}
	const int frame = circuit->GetLastFrame();
	losses.erase(std::remove_if(losses.begin(), losses.end(), [frame](const std::pair<AIFloat3, int>& l) {
		return frame - l.second > MEMORY_FRAMES;
	}), losses.end());
	for (auto it = laneOf.begin(); it != laneOf.end();) {  // builders gone
		it = (circuit->GetTeamUnit(it->first) == nullptr) ? laneOf.erase(it) : std::next(it);
	}

	MakeLanes();
	// Steps under way at once: turret.creep_builders, plus one per turret.creep_income metal/s
	const float income = circuit->GetEconomyManager()->GetAvgMetalIncome();
	const int most = std::max(cfg.creepBuilders, 0) + ((cfg.creepIncome > 0.f) ? int(income / cfg.creepIncome) : 0);
	OrderSteps(most);

	if (frame >= logFrame) {
		logFrame = frame + LOG_FRAMES;
		circuit->LOG("CREEP %i of %i steps under way, %i lanes of a line of %i; this second skipped: %i not live, "
				"%i resting, %i claimed, %i no turret, %i no progress, %i in enemy reach, %i stepping, %i ends under "
				"threat; %i lost lately", (int)jobs.size(), most, (int)lanes.size(), lineSize, skips[NOT_LIVE],
				skips[RESTING], skips[CLAIMED], skips[NO_ANCHOR], skips[NO_PROGRESS], skips[IN_REACH], skips[STEPPING],
				skips[THREATENED], (int)losses.size());
	}
}

void CCreepPlanner::OrderSteps(int most)
{
	const CTurretPicker* picker = circuit->GetMilitaryManager()->GetTurretPicker();
	const CTurretPicker::SConfig& cfg = picker->GetConfig();
	CBuilderManager* builderMgr = circuit->GetBuilderManager();
	int busy = (int)jobs.size();
	if (lanes.empty() || (busy >= most)) {
		return;
	}
	const int frame = circuit->GetLastFrame();
	std::vector<bool> isTaken(lanes.size());
	for (size_t i = 0; i < lanes.size(); ++i) {
		isTaken[i] = (frame < memos[lanes[i].chokeId].next);  // ordered on within an interval
	}
	// Each step is a job for a free mobile ground builder: the commander first (not while hide mode holds it). Between
	// steps a builder works as any other
	std::vector<CCircuitUnit*> free;
	for (CCircuitUnit* u : builderMgr->GetWorkers()) {
		const CCircuitDef* cdef = u->GetCircuitDef();
		if (cdef->IsMobile() && !cdef->IsAbleToFly() && builderMgr->IsTurretFree(u)
			&& !(cdef->IsRoleComm() && builderMgr->IsCommHiding()))
		{
			free.push_back(u);
		}
	}
	std::stable_partition(free.begin(), free.end(), [](CCircuitUnit* u) { return u->GetCircuitDef()->IsRoleComm(); });

	while ((busy < most) && !free.empty()) {
		// The commander takes the highest-ranked of the three lanes nearest it; else the highest-ranked lane left
		// takes the builder nearest its step
		auto who = free.begin();
		const SLane* lane = nullptr;
		if ((*who)->GetCircuitDef()->IsRoleComm()) {
			lane = PickLane((*who)->GetPos(frame), &isTaken);
		} else {
			auto it = std::find(isTaken.begin(), isTaken.end(), false);
			if (it != isTaken.end()) {
				lane = &lanes[it - isTaken.begin()];
				who = std::min_element(free.begin(), free.end(), [lane, frame](CCircuitUnit* a, CCircuitUnit* b) {
					return a->GetPos(frame).SqDistance2D(lane->end) < b->GetPos(frame).SqDistance2D(lane->end);
				});
			}
		}
		if (lane == nullptr) {
			break;
		}
		CCircuitUnit* unit = *who;
		// A step's end under threat (the picker's creep rule) waits: not a failed step
		if (!lane->isHelp && picker->IsCreepThreatened(unit, lane->end)) {
			++skips[THREATENED];
			isTaken[lane - lanes.data()] = true;
			continue;
		}
		const int result = builderMgr->OrderCreepTurret(unit, lane->end, lane->radius, lane->isHelp, !lane->isAtLine);
		if (result == 0) {
			break;  // stall or budget: again next second
		}
		for (size_t i = 0; i < lanes.size(); ++i) {  // and every lane stepping to the same ground
			isTaken[i] = isTaken[i] || (lanes[i].end.SqDistance2D(lane->end) <= SQUARE(cfg.creepRadius));
		}
		memos[lane->chokeId].next = frame + int(cfg.creepInterval * FRAMES_PER_SEC);
		if (result == 1) {
			laneOf[unit->GetId()] = lane->chokeId;
			free.erase(who);
			++busy;
		} else if (result == -1) {
			AddFail(lane->chokeId);
		}
	}
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
	jobs.clear();
	for (const IBuilderTask* t : circuit->GetBuilderManager()->GetTasks(IBuilderTask::BuildType::DEFENCE)) {
		if (static_cast<const CBDefenceTask*>(t)->IsCreep()) {
			jobs.push_back(t->GetPosition());
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
	jobs.clear();
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
		// One step at a time: ours already under way there
		if (std::any_of(jobs.begin(), jobs.end(), [&](const AIFloat3& p) {
				return p.SqDistance2D(end) <= SQUARE(cfg.creepRadius);
			}))
		{
			++skips[STEPPING];
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

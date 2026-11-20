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
#include "setup/SetupManager.h"
#include "spring/SpringMap.h"
#include "terrain/ChokeMap.h"
#include "terrain/TerrainManager.h"
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
static constexpr int NEAREST_STEPS = 3;  // a pusher takes the shortest gap among this many steps nearest it

CCreepPlanner::CCreepPlanner(CCircuitAI* circuit)
		: circuit(circuit)
		, logFrame(0)
{
}

CCreepPlanner::~CCreepPlanner()
{
}

void CCreepPlanner::Update()
{
	CTurretPicker* picker = circuit->GetMilitaryManager()->GetTurretPicker();
	const CTurretPicker::SConfig& cfg = picker->GetConfig();
	if (!cfg.isCreep) {
		pushers.clear();
		steps.clear();
		return;
	}
	const int frame = circuit->GetLastFrame();
	auto isGun = [picker](const CCircuitDef* d) { return picker->IsDefence(d) && d->HasAntiLand(); };

	// Our finished guns, and the ones lost since last second
	std::vector<SGun> ours;
	std::map<ICoreUnit::Id, AIFloat3> nowTurrets;
	for (const auto& kv : circuit->GetTeamUnits()) {
		CCircuitUnit* unit = kv.second;
		const CCircuitDef* cdef = unit->GetCircuitDef();
		if (isGun(cdef) && !unit->GetUnit()->IsBeingBuilt()) {
			const AIFloat3& pos = unit->GetPos(frame);
			ours.push_back({unit->GetId(), pos, cdef->GetMaxRange(CCircuitDef::RangeType::LAND)});
			nowTurrets[unit->GetId()] = pos;
		}
	}
	for (const auto& kv : turrets) {
		if (nowTurrets.find(kv.first) == nowTurrets.end()) {
			losses.push_back({kv.second, frame});
		}
	}
	turrets.swap(nowTurrets);
	losses.erase(std::remove_if(losses.begin(), losses.end(), [frame](const std::pair<AIFloat3, int>& l) {
		return frame - l.second > MEMORY_FRAMES;
	}), losses.end());
	for (auto it = fails.begin(); it != fails.end();) {
		it = (frame - it->second.frame > MEMORY_FRAMES) ? fails.erase(it) : std::next(it);
	}

	// The enemy's known guns (until seen dead)
	std::vector<SGun> enemies;
	for (const auto& kv : circuit->GetEnemyInfos()) {
		const CCircuitDef* edef = kv.second->GetCircuitDef();
		if (picker->IsEnemyBuilding(kv.second, false) && isGun(edef)) {
			enemies.push_back({kv.first, kv.second->GetPos(), edef->GetMaxRange(CCircuitDef::RangeType::LAND)});
		}
	}

	MakeSteps(ours, enemies);
	UpdatePushers(steps.empty() ? circuit->GetSetupManager()->GetBasePos() : steps.front().end);

	CBuilderManager* builderMgr = circuit->GetBuilderManager();
	std::vector<bool> isTaken(steps.size(), false);
	for (ICoreUnit::Id id : pushers) {
		CCircuitUnit* unit = circuit->GetTeamUnit(id);
		auto it = nextFrames.find(id);
		if ((unit == nullptr) || ((it != nextFrames.end()) && (frame < it->second)) || !builderMgr->IsTurretFree(unit)) {
			continue;
		}
		// The shortest gap among the steps nearest it that no other pusher took this second
		const AIFloat3& pos = unit->GetPos(frame);
		std::vector<int> close;
		for (int i = 0; i < (int)steps.size(); ++i) {
			if (!isTaken[i]) {
				close.push_back(i);
			}
		}
		std::sort(close.begin(), close.end(), [&](int a, int b) {
			return steps[a].end.SqDistance2D(pos) < steps[b].end.SqDistance2D(pos);
		});
		close.resize(std::min<size_t>(close.size(), NEAREST_STEPS));
		if (close.empty()) {
			break;
		}
		const int best = *std::min_element(close.begin(), close.end(), [this](int a, int b) {
			return steps[a].gap < steps[b].gap;
		});
		const int result = builderMgr->OrderCreepTurret(unit, steps[best].end);
		if (result == 0) {
			continue;  // stall or budget: again next second
		}
		isTaken[best] = true;
		nextFrames[id] = frame + int(cfg.creepInterval * FRAMES_PER_SEC);
		if (result < 0) {
			SFail& f = fails[steps[best].fromId];
			++f.count;
			f.frame = frame;
		}
	}

	if (frame >= logFrame) {
		logFrame = frame + LOG_FRAMES;
		if (steps.empty()) {
			circuit->LOG("CREEP %i pushers, no step (%i guns of ours, %i of the enemy known, %i lost lately)",
					(int)pushers.size(), (int)ours.size(), (int)enemies.size(), (int)losses.size());
		} else {
			const SStep& s = steps.front();
			circuit->LOG("CREEP %i pushers, %i steps; the shortest gap %.0f, to (%.0f, %.0f); %i lost lately",
					(int)pushers.size(), (int)steps.size(), s.gap, s.end.x, s.end.z, (int)losses.size());
		}
	}
}

bool CCreepPlanner::IsPusher(const CCircuitUnit* unit) const
{
	return std::find(pushers.begin(), pushers.end(), unit->GetId()) != pushers.end();
}

IBuilderTask* CCreepPlanner::MakeHoldTask(CCircuitUnit* unit)
{
	if (steps.empty() || !IsPusher(unit) || circuit->GetEconomyManager()->IsStalling()) {
		return nullptr;
	}
	const CTurretPicker::SConfig& cfg = circuit->GetMilitaryManager()->GetTurretPicker()->GetConfig();
	// Worth holding only while the account fills for a light turret soon
	float cost = 0.f;
	for (const CCircuitDef* d : cfg.light) {
		cost = std::max(cost, d->GetCostM());
	}
	if (circuit->GetMilitaryManager()->GetTurretWait(cost * utils::clamp(cfg.creepBudget, 0.f, 1.f))
		> 2.f * cfg.creepInterval)
	{
		return nullptr;
	}
	const int frame = circuit->GetLastFrame();
	const AIFloat3& pos = unit->GetPos(frame);
	const SStep* best = nullptr;
	for (const SStep& s : steps) {
		if ((best == nullptr) || (s.end.SqDistance2D(pos) < best->end.SqDistance2D(pos))) {
			best = &s;
		}
	}
	CCircuitUnit* from = circuit->GetTeamUnit(best->fromId);
	CBuilderManager* builderMgr = circuit->GetBuilderManager();
	if ((from == nullptr) || !builderMgr->IsTurretJobSafe(unit, from->GetPos(frame))) {
		return nullptr;
	}
	return builderMgr->EnqueueGuard(IBuilderTask::Priority::LOW, from, int(cfg.creepInterval * FRAMES_PER_SEC));
}

void CCreepPlanner::UpdatePushers(const AIFloat3& nearPos)
{
	const CTurretPicker::SConfig& cfg = circuit->GetMilitaryManager()->GetTurretPicker()->GetConfig();
	CBuilderManager* builderMgr = circuit->GetBuilderManager();
	const float income = circuit->GetEconomyManager()->GetAvgMetalIncome();
	const int want = std::max(cfg.creepBuilders, 0) + ((cfg.creepIncome > 0.f) ? int(income / cfg.creepIncome) : 0);

	const std::set<CCircuitUnit*>& workers = builderMgr->GetWorkers();
	auto isWorker = [&workers](CCircuitUnit* u) { return (u != nullptr) && (workers.find(u) != workers.end()); };
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
		return !isWorker(u) || (u->GetCircuitDef()->IsRoleComm() && (u != comm));
	}), pushers.end());
	if ((comm != nullptr) && (want > 0) && !IsPusher(comm)) {
		pushers.insert(pushers.begin(), comm->GetId());  // the commander first
	}
	const int frame = circuit->GetLastFrame();
	while ((int)pushers.size() < want) {
		CCircuitUnit* best = nullptr;
		float bestSqDist = std::numeric_limits<float>::max();
		for (CCircuitUnit* u : workers) {
			const CCircuitDef* cdef = u->GetCircuitDef();
			if (!cdef->IsMobile() || cdef->IsAbleToFly() || cdef->IsRoleComm() || IsPusher(u) || !builderMgr->IsTurretFree(u)) {
				continue;
			}
			const float sqDist = u->GetPos(frame).SqDistance2D(nearPos);
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
}

void CCreepPlanner::MakeSteps(const std::vector<SGun>& ours, const std::vector<SGun>& enemies)
{
	steps.clear();
	if (ours.empty()) {
		return;
	}
	const CTurretPicker::SConfig& cfg = circuit->GetMilitaryManager()->GetTurretPicker()->GetConfig();
	const int frame = circuit->GetLastFrame();
	// Where steps head, and how far short of it they stop: the enemy's guns at their range + the margin; with none
	// known, the live chokes of the enemy line, at the choke
	struct SGoal {
		AIFloat3 pos;
		float stop;
	};
	std::vector<SGoal> goals;
	for (const SGun& e : enemies) {
		goals.push_back({e.pos, e.range + cfg.creepMargin});
	}
	if (goals.empty()) {
		CChokeMap* chokeMap = circuit->GetChokeMap();
		if (chokeMap->IsReady()) {
			for (int c : chokeMap->GetEnemyLine()) {
				if (chokeMap->IsChokeLive(c, frame)) {
					goals.push_back({chokeMap->GetCellPos(chokeMap->GetGraph().GetChokes()[c].centre), 0.f});
				}
			}
		} else {
			chokeMap->Request();
		}
	}
	if (goals.empty()) {
		return;
	}
	auto isInEnemyReach = [&](const AIFloat3& p) {
		return std::any_of(enemies.begin(), enemies.end(), [&](const SGun& e) {
			return e.pos.SqDistance2D(p) <= SQUARE(e.range + cfg.creepMargin);
		});
	};

	for (const SGun& t : ours) {
		auto fit = fails.find(t.id);
		if ((fit != fails.end()) && (fit->second.count >= 2)) {
			continue;
		}
		const SGoal* goal = nullptr;
		float gap = std::numeric_limits<float>::max();
		for (const SGoal& g : goals) {
			const float d = t.pos.distance2D(g.pos) - g.stop;
			if (d < gap) {
				gap = d;
				goal = &g;
			}
		}
		const float fullStep = cfg.creepStep * t.range;
		if ((goal == nullptr) || (fullStep <= 0.f) || (gap <= 0.25f * fullStep)) {
			continue;  // at their line already
		}
		AIFloat3 dir = goal->pos - t.pos;
		dir.y = 0.f;
		dir.SafeNormalize2D();
		const float stepLen = std::min(fullStep, gap);
		bool isFound = false;
		SStep step = {t.id, t.pos, gap};
		for (float f : {1.f, 0.5f, 0.25f}) {  // out of every enemy gun's reach
			AIFloat3 end = t.pos + dir * (stepLen * f);
			CTerrainManager::CorrectPosition(end);
			if (!isInEnemyReach(end)) {
				end.y = circuit->GetMap()->GetElevationAt(end.x, end.z);
				step.end = end;
				isFound = true;
				break;
			}
		}
		if (isFound && !IsBlocked(step, std::min(cfg.creepRadius, 0.5f * step.end.distance2D(t.pos)), t.range)) {
			steps.push_back(step);
		}
	}
	std::sort(steps.begin(), steps.end(), [](const SStep& a, const SStep& b) { return a.gap < b.gap; });
}

bool CCreepPlanner::IsBlocked(const SStep& step, float radius, float lossRange) const
{
	const CTurretPicker::SConfig& cfg = circuit->GetMilitaryManager()->GetTurretPicker()->GetConfig();
	// Our turrets lost lately within a turret's range of its end: the enemy holds it (artillery, a raid that returns)
	if (cfg.creepLosses > 0) {
		const float sqReach = SQUARE(lossRange);
		const int count = std::count_if(losses.begin(), losses.end(), [&](const std::pair<AIFloat3, int>& l) {
			return l.first.SqDistance2D(step.end) <= sqReach;
		});
		if (count >= cfg.creepLosses) {
			return true;
		}
	}
	// Taken: a ground defence of ours or an ally's (queued too) already stands there
	AIFloat3 defencePos;
	auto isGround = [](const CCircuitDef* d) { return d->HasAntiLand(); };
	return circuit->GetBuilderManager()->HasTurretDefence(step.end, radius, isGround, defencePos);
}

} // namespace circuit

/*
 * MexBoard.cpp
 *
 *  Created on: Nov 22, 2026
 *      Author: Claude
 */

#include "module/MexBoard.h"
#include "module/BuilderManager.h"
#include "module/EconomyManager.h"
#include "resource/MetalManager.h"
#include "setup/SetupManager.h"
#include "task/IdleTask.h"
#include "task/builder/BuildChain.h"
#include "terrain/TerrainManager.h"
#include "terrain/path/PathFinder.h"
#include "terrain/path/QueryCostMap.h"
#include "unit/action/TravelAction.h"
#include "unit/ally/AllyUnit.h"
#include "CircuitAI.h"
#include "util/GameAttribute.h"
#include "util/Utils.h"
#include "json/json.h"

#include <algorithm>
#include <limits>

namespace circuit {

using namespace springai;

static constexpr int SKIP_FRAMES = FRAMES_PER_SEC * 30;  // a spot whose job failed for a builder waits this long for it

CMexBoard::CMexBoard(CCircuitAI* circuit)
		: circuit(circuit)
{
	// "mex_board": false = off; missing = on with the defaults
	const Json::Value& board = circuit->GetSetupManager()->GetConfig()["economy"]["mex_board"];
	const Json::Value& cfg = board.isObject() ? board : Json::Value::nullSingleton();
	isOn = board.isObject() || board.isNull() || (board.isBool() && board.asBool());
	minGain = std::max(cfg.get("min_gain_sec", 3.f).asFloat(), 0.f);
	holdFrames = std::max(cfg.get("hold_sec", 20.f).asFloat(), 0.f) * FRAMES_PER_SEC;
}

bool CMexBoard::IsActive() const
{
	return isOn && circuit->GetEconomyManager()->IsOpening();
}

bool CMexBoard::IsHeld(int spot) const
{
	return std::any_of(holds.begin(), holds.end(), [spot](const SHold& h) { return h.spot == spot; });
}

bool CMexBoard::IsHeldByOther(const IBuilderTask* task, const CCircuitUnit* unit) const
{
	auto it = std::find_if(holds.begin(), holds.end(), [task](const SHold& h) { return h.task == task; });
	return (it != holds.end()) && (it->unit != unit);
}

CMexBoard::SHold* CMexBoard::FindHold(const CCircuitUnit* unit)
{
	auto it = std::find_if(holds.begin(), holds.end(), [unit](const SHold& h) { return h.unit == unit; });
	return (it != holds.end()) ? &*it : nullptr;
}

IBuilderTask* CMexBoard::GetJob(CCircuitUnit* unit, const CQueryCostMap* query)
{
	if (!IsActive()) {
		return nullptr;
	}
	SHold* hold = FindHold(unit);
	isDirty |= (hold == nullptr);  // it needs a spot
	// Not for a walking builder (an urgent pick, IBuilderTask::Reevaluate): the match may move the job it walks to
	if (isDirty && (matchFrame != circuit->GetLastFrame())
		&& (unit->GetTask() == circuit->GetBuilderManager()->GetIdleTask()))
	{
		Match(unit, query);
		hold = FindHold(unit);
	}
	return (hold != nullptr) ? hold->task : nullptr;
}

void CMexBoard::OnBuildStart(CCircuitUnit* unit, const IBuilderTask* task)
{
	auto it = std::find_if(holds.begin(), holds.end(), [unit, task](const SHold& h) {
		return (h.unit == unit) && (h.task == task);
	});
	if (it != holds.end()) {
		holds.erase(it);
		isDirty = true;
	}
}

void CMexBoard::OnTaskRemoved(const IBuilderTask* task, bool done)
{
	auto it = std::find_if(holds.begin(), holds.end(), [task](const SHold& h) { return h.task == task; });
	if (it == holds.end()) {
		return;
	}
	if (!done) {  // unreachable, the build failed, a flee from damage: not that spot again soon
		skips.push_back({it->unit->GetId(), it->spot, circuit->GetLastFrame() + SKIP_FRAMES});
	}
	holds.erase(it);
	isDirty = true;
}

void CMexBoard::OnBuilderDestroyed(CCircuitUnit* unit)
{
	toAssign.erase(std::remove(toAssign.begin(), toAssign.end(), unit), toAssign.end());
	auto it = std::find_if(holds.begin(), holds.end(), [unit](const SHold& h) { return h.unit == unit; });
	if (it != holds.end()) {  // its next spot (the one it walked to ended with it)
		IBuilderTask* task = it->task;
		holds.erase(it);
		Release(task);
	}
	isDirty = true;
}

void CMexBoard::Release(IBuilderTask* task)
{
	circuit->GetBuilderManager()->AbortTask(task);  // its spot reopens (CBMexTask::Cancel)
}

void CMexBoard::End()
{
	std::vector<SHold> old;
	old.swap(holds);
	for (const SHold& h : old) {
		if (h.unit->GetTask() != h.task) {
			Release(h.task);
		}
	}
	toAssign.clear();
	skips.clear();
}

void CMexBoard::Update()
{
	if (!IsActive()) {
		if (!holds.empty()) {
			End();
		}
		return;
	}
	if (isDirty && (matchFrame != circuit->GetLastFrame())) {
		Match(nullptr, nullptr);
	}
	std::vector<CCircuitUnit*> units;
	units.swap(toAssign);
	CBuilderManager* builderMgr = circuit->GetBuilderManager();
	for (CCircuitUnit* unit : units) {
		builderMgr->AssignNext(unit);  // between jobs only
	}
}

bool CMexBoard::GetStart(CCircuitUnit* unit, const SHold* hold, AIFloat3& outFrom, float& outBusy) const
{
	const AIFloat3& pos = unit->GetPos(circuit->GetLastFrame());
	outFrom = pos;
	outBusy = 0.f;
	const IUnitTask* task = unit->GetTask();
	if ((task->GetType() == IUnitTask::Type::IDLE) || (task->GetType() == IUnitTask::Type::WAIT)) {
		return true;
	}
	if (task->GetType() != IUnitTask::Type::BUILDER) {
		return false;  // a flee, a fight, player control
	}
	const IBuilderTask* taskB = static_cast<const IBuilderTask*>(task);
	if ((taskB->GetBuildType() == IBuilderTask::BuildType::PATROL) || ((hold != nullptr) && (hold->task == taskB))) {
		return true;  // nothing to do, or walking to its spot: it may take another from where it stands
	}
	if ((taskB->GetBuildType() >= IBuilderTask::BuildType::REPAIR) || (taskB->GetBuildDef() == nullptr)) {
		return false;
	}
	// A building: the walk left, the build left, and the energy the mex chain queues beside a mex (ExecuteChain)
	const float speed = std::max(unit->GetCircuitDef()->GetSpeed(), 1.f);
	const float buildSpeed = std::max(unit->GetBuildSpeed(), 1.f);
	outFrom = taskB->GetPosition();
	if ((unit->GetTravelAct() != nullptr) && !unit->GetTravelAct()->IsFinished()) {
		outBusy += pos.distance2D(outFrom) / speed;
	}
	const CCircuitUnit* target = taskB->GetTarget();
	const float progress = (target != nullptr) ? target->GetUnit()->GetBuildProgress() : 0.f;
	outBusy += taskB->GetCost() * (1.f - progress) / std::max(taskB->GetBuildPower(), buildSpeed);
	if (taskB->GetBuildType() == IBuilderTask::BuildType::MEX) {
		CEconomyManager* economyMgr = circuit->GetEconomyManager();
		const SBuildChain* chain = circuit->GetBuilderManager()->GetBuildChain(taskB->GetBuildType(), taskB->GetBuildDef());
		float make;
		const CCircuitDef* energyDef = ((chain != nullptr) && (chain->energy > economyMgr->GetAvgEnergyIncome()))
				? economyMgr->GetLowEnergy(outFrom, make, true) : nullptr;
		outBusy += (energyDef != nullptr) ? energyDef->GetCostM() / buildSpeed : 0.f;
	}
	return true;
}

std::vector<AIFloat3> CMexBoard::GetAllyAnchors() const
{
	std::vector<AIFloat3> anchors;
	if (circuit->GetAllyTeam()->GetSize() <= 1) {
		return anchors;
	}
	for (CCircuitAI* ai : circuit->GetGameAttribute()->GetCircuits()) {
		if ((ai != circuit) && ai->IsInitialized() && (ai->GetAllyTeamId() == circuit->GetAllyTeamId())
			&& utils::is_valid(ai->GetSetupManager()->GetBasePos()))
		{
			anchors.push_back(ai->GetSetupManager()->GetBasePos());
		}
	}
	const int frame = circuit->GetLastFrame();
	circuit->UpdateFriendlyUnits();
	for (const auto& kv : circuit->GetFriendlyUnits()) {
		const CCircuitDef* cdef = kv.second->GetCircuitDef();
		if ((cdef != nullptr) && !cdef->IsMobile() && cdef->IsBuilder() && (circuit->GetTeamUnit(kv.first) == nullptr)) {
			anchors.push_back(kv.second->GetPos(frame));
		}
	}
	return anchors;
}

void CMexBoard::Match(CCircuitUnit* asker, const CQueryCostMap* askerQuery)
{
	const int frame = circuit->GetLastFrame();
	matchFrame = frame;
	isDirty = false;
	skips.erase(std::remove_if(skips.begin(), skips.end(), [frame](const SSkip& s) { return s.until <= frame; }),
			skips.end());

	CBuilderManager* builderMgr = circuit->GetBuilderManager();
	CEconomyManager* economyMgr = circuit->GetEconomyManager();
	CMetalManager* metalMgr = circuit->GetMetalManager();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	CPathFinder* pathfinder = circuit->GetPathfinder();
	const CMetalData::Metals& metalSpots = metalMgr->GetSpots();
	constexpr float INF = std::numeric_limits<float>::max();

	// Spots: open ones some builder could take, and held ones; not nearer an ally's base or factory than our base
	std::vector<int> spots;
	std::vector<int> entryOf(metalSpots.size(), -1);
	auto addSpot = [&spots, &entryOf](int spot) {
		if (entryOf[spot] < 0) {
			entryOf[spot] = spots.size();
			spots.push_back(spot);
		}
		return entryOf[spot];
	};
	{
		const std::function<bool (int spot)> isOpen = economyMgr->GetMexSpotTest(nullptr, true);
		const std::vector<AIFloat3> anchors = GetAllyAnchors();
		const AIFloat3& basePos = circuit->GetSetupManager()->GetBasePos();
		for (unsigned i = 0; i < metalSpots.size(); ++i) {
			const AIFloat3& pos = metalSpots[i].position;
			const float sqOurs = basePos.SqDistance2D(pos);
			if (isOpen(i) && std::none_of(anchors.begin(), anchors.end(), [&pos, sqOurs](const AIFloat3& a) {
					return a.SqDistance2D(pos) < sqOurs;
				}))
			{
				addSpot(i);
			}
		}
	}

	// Builders, and where each would start from. A fixed one keeps its spot: fleeing up to hold_sec, or walking to it
	// within commit_sec of it
	struct SBuilder {
		CCircuitUnit* unit;
		AIFloat3 from;
		float busy;  // seconds
		const CQueryCostMap* query;  // started at from, else nullptr: straight line
		int hold;  // entry of the spot it holds, -1 = none
		float holdArrival, holdKey;  // at that spot, INF if it no longer takes it
		bool isFixed;
	};
	std::vector<SBuilder> builders;
	for (CCircuitUnit* unit : builderMgr->GetWorkers()) {
		SHold* hold = FindHold(unit);
		const IUnitTask* task = unit->GetTask();
		const AIFloat3& pos = unit->GetPos(frame);
		if (hold != nullptr) {
			const bool isFlee = (task->GetType() == IUnitTask::Type::RETREAT);
			hold->fleeFrame = !isFlee ? -1 : (hold->fleeFrame < 0) ? frame : hold->fleeFrame;
			const float speed = std::max(unit->GetCircuitDef()->GetSpeed(), 1.f);
			if (isFlee ? (frame - hold->fleeFrame <= holdFrames)
				: ((task == hold->task)
					&& (pos.distance2D(hold->task->GetPosition()) <= builderMgr->jobCommitSec * speed)))
			{
				builders.push_back({unit, pos, 0.f, nullptr, addSpot(hold->spot), 0.f, 0.f, true});
				continue;
			}
		}
		SBuilder b = {unit, pos, 0.f, nullptr, -1, INF, INF, false};
		if (!GetStart(unit, hold, b.from, b.busy)) {
			continue;
		}
		const int buildDist = std::max<int>(unit->GetCircuitDef()->GetBuildDistance(), pathfinder->GetSquareSize());
		const auto it = builderMgr->costQueries.find(unit);
		const IPathQuery* query = ((unit == asker) && (askerQuery != nullptr)) ? askerQuery
				: ((it != builderMgr->costQueries.end()) && (it->second->GetState() == IPathQuery::State::READY))
					? it->second.get() : nullptr;
		const CQueryCostMap* costMap = static_cast<const CQueryCostMap*>(query);
		b.query = ((costMap != nullptr) && (costMap->GetStartPos().SqDistance2D(b.from) < SQUARE(buildDist)))
				? costMap : nullptr;
		b.hold = (hold != nullptr) ? entryOf[hold->spot] : -1;
		builders.push_back(b);
	}

	// Every builder and spot its own picker would take: arrival, and the score pairs go by
	struct SPair {
		int b, s;  // builder, spot entry
		float arrival, key;
	};
	std::vector<SPair> pairs;
	const CMetalManager::ClusterScore scores[2] = {economyMgr->GetMexScore(false), economyMgr->GetMexScore(true)};
	for (int bi = 0; bi < (int)builders.size(); ++bi) {
		SBuilder& b = builders[bi];
		if (b.isFixed) {
			continue;
		}
		const std::function<bool (const AIFloat3& pos)> isJobOk = builderMgr->GetMexJobTest(b.unit);  // sets its threat type
		if (!isJobOk) {
			continue;  // a commander off mex jobs
		}
		const CCircuitDef* cdef = b.unit->GetCircuitDef();
		const float speed = std::max(cdef->GetSpeed(), 1.f);
		const float costSpeed = speed / pathfinder->GetSquareSize() * COST_BASE;
		const int buildDist = std::max<int>(cdef->GetBuildDistance(), pathfinder->GetSquareSize());
		const CMetalManager::ClusterScore& score = scores[cdef->IsRoleComm() ? 1 : 0];
		const ICoreUnit::Id unitId = b.unit->GetId();
		for (int e = 0; e < (int)spots.size(); ++e) {
			const int spot = spots[e];
			const AIFloat3& pos = metalSpots[spot].position;
			if (std::any_of(skips.begin(), skips.end(), [unitId, spot](const SSkip& s) {
					return (s.unitId == unitId) && (s.spot == spot);
				})
				|| !terrainMgr->CanReachAtSafe(b.unit, pos, cdef->GetBuildDistance()) || !isJobOk(pos))
			{
				continue;
			}
			float walk = b.from.distance2D(pos) / speed;
			if ((b.query != nullptr) && (b.from.SqDistance2D(pos) >= SQUARE(buildDist))) {
				const float cost = b.query->GetCostAt(pos, buildDist);
				if (cost < 0.f) {
					continue;  // buildings block the way
				}
				walk = cost / costSpeed;
			}
			const float arrival = b.busy + walk;
			const int cluster = metalMgr->GetCluster(spot);
			const float key = (score && (cluster >= 0)) ? score(cluster, arrival) : arrival;
			if (e == b.hold) {
				b.holdArrival = arrival;
				b.holdKey = key;
			}
			pairs.push_back({bi, e, arrival, key});
		}
	}
	std::sort(pairs.begin(), pairs.end(), [](const SPair& a, const SPair& b) { return a.key < b.key; });
	std::vector<std::vector<int>> bySpot(spots.size());  // pair indices
	for (int i = 0; i < (int)pairs.size(); ++i) {
		bySpot[pairs[i].s].push_back(i);
	}

	// Lowest score first. A spot's holder, while still free, keeps it unless this one gets there min_gain sooner; a
	// builder, while its own spot is still free, keeps it unless this one scores min_gain better. The second pass takes
	// what the first left because a holder it waited for went elsewhere
	std::vector<int> matchOf(builders.size(), -1);
	std::vector<int> takenBy(spots.size(), -1);
	std::vector<int> heldBy(spots.size(), -1);
	for (int bi = 0; bi < (int)builders.size(); ++bi) {
		const int e = builders[bi].hold;
		if (e >= 0) {
			heldBy[e] = bi;
			if (builders[bi].isFixed) {
				matchOf[bi] = e;
				takenBy[e] = bi;
			}
		}
	}
	for (int pass = 0; pass < 2; ++pass) {
		for (const SPair& p : pairs) {
			if ((matchOf[p.b] >= 0) || (takenBy[p.s] >= 0)) {
				continue;
			}
			const SBuilder& b = builders[p.b];
			const int v = heldBy[p.s];
			if ((v >= 0) && (v != p.b) && (matchOf[v] < 0) && (p.arrival + minGain > builders[v].holdArrival)) {
				continue;
			}
			if ((b.hold >= 0) && (b.hold != p.s) && (takenBy[b.hold] < 0) && (p.key + minGain > b.holdKey)) {
				continue;
			}
			// The commander's lean orders its own picks; it never takes a spot another free builder reaches sooner (but
			// keeps its own as any builder does)
			if (b.unit->GetCircuitDef()->IsRoleComm() && (p.s != b.hold)
				&& std::any_of(bySpot[p.s].begin(), bySpot[p.s].end(), [&](int i) {
					return (pairs[i].b != p.b) && (matchOf[pairs[i].b] < 0) && (pairs[i].arrival < p.arrival);
				}))
			{
				continue;
			}
			matchOf[p.b] = p.s;
			takenBy[p.s] = p.b;
		}
	}

	// Apply. A spot held before keeps its job, for its new builder if it moved (one walking to it leaves it); one
	// nobody holds now gives its job up; a newly held one gets a job
	std::vector<SHold> kept;
	std::vector<IBuilderTask*> released;
	std::vector<CCircuitUnit*> bumped;
	for (SHold& h : holds) {
		const int e = entryOf[h.spot];
		const int bi = (e >= 0) ? takenBy[e] : -1;
		CCircuitUnit* unit = (bi >= 0) ? builders[bi].unit : nullptr;
		if (unit != h.unit) {
			if (h.unit->GetTask() == h.task) {
				bumped.push_back(h.unit);
			}
			if (unit == nullptr) {
				released.push_back(h.task);
				continue;
			}
			h.unit = unit;
			h.fleeFrame = -1;
		}
		takenBy[e] = -1;  // has its job
		kept.push_back(h);
	}
	holds.swap(kept);
	for (CCircuitUnit* unit : bumped) {
		unit->GetTask()->RemoveAssignee(unit);  // to idle
		CBuilderManager::SOpeningStats* stats = builderMgr->GetOpeningStats(unit);
		if (stats != nullptr) {
			++stats->moves;
		}
	}
	for (IBuilderTask* task : released) {
		Release(task);
	}
	for (int e = 0; e < (int)spots.size(); ++e) {
		if (takenBy[e] >= 0) {
			holds.push_back({builders[takenBy[e]].unit, economyMgr->EnqueueMex(spots[e]), spots[e], -1});
		}
	}
	// Between jobs with a spot, or off the one it walked to: picks on the next update (the asker picks now)
	CIdleTask* idleTask = builderMgr->GetIdleTask();
	auto addAssign = [this, asker](CCircuitUnit* unit) {
		if ((unit != asker) && (std::find(toAssign.begin(), toAssign.end(), unit) == toAssign.end())) {
			toAssign.push_back(unit);
		}
	};
	for (const SHold& h : holds) {
		if (h.unit->GetTask() == idleTask) {
			addAssign(h.unit);
		}
	}
	for (CCircuitUnit* unit : bumped) {
		addAssign(unit);
	}
}

} // namespace circuit

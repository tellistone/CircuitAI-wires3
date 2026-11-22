/*
 * MexBoard.cpp
 *
 *  Created on: Nov 22, 2026
 *      Author: Claude
 */

#include "module/MexBoard.h"
#include "map/ThreatMap.h"
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
#include "CircuitAI.h"
#include "util/Utils.h"
#include "json/json.h"

#include <algorithm>

namespace circuit {

using namespace springai;

static constexpr int SKIP_FRAMES = FRAMES_PER_SEC * 30;  // a spot whose job failed for a builder waits this long for it
static constexpr int FAIL_FRAMES = FRAMES_PER_SEC * 3;  // a walker keeps its spot this long after it fails the test

CMexBoard::CMexBoard(CCircuitAI* circuit)
		: circuit(circuit)
{
	// "mex_board": false = off; missing = on with the defaults
	const Json::Value& board = circuit->GetSetupManager()->GetConfig()["economy"]["mex_board"];
	const Json::Value& cfg = board.isObject() ? board : Json::Value::nullSingleton();
	isOn = board.isObject() || board.isNull() || (board.isBool() && board.asBool());
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
	return std::any_of(holds.begin(), holds.end(), [task, unit](const SHold& h) {
		return (h.task == task) && (h.unit != unit);
	});
}

CMexBoard::SHold* CMexBoard::FindHold(const CCircuitUnit* unit)
{
	auto it = std::find_if(holds.begin(), holds.end(), [unit](const SHold& h) { return h.unit == unit; });
	return (it != holds.end()) ? &*it : nullptr;
}

CMexBoard::SHold* CMexBoard::FindHold(const IBuilderTask* task)
{
	auto it = std::find_if(holds.begin(), holds.end(), [task](const SHold& h) { return h.task == task; });
	return (it != holds.end()) ? &*it : nullptr;
}

void CMexBoard::DropHold(const SHold* hold)
{
	holds.erase(holds.begin() + (hold - holds.data()));
	isDirty = true;
}

IBuilderTask* CMexBoard::GetMexJob(CCircuitUnit* unit, const CQueryCostMap* query)
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
	const SHold* hold = FindHold(task);
	if ((hold != nullptr) && (hold->unit == unit)) {
		DropHold(hold);
	}
}

void CMexBoard::OnTaskRemoved(const IBuilderTask* task, bool done)
{
	const SHold* hold = FindHold(task);
	if (hold == nullptr) {
		return;
	}
	if (!done) {  // unreachable, the build failed, a flee from damage: not that spot again soon
		skips.push_back({hold->unit->GetId(), hold->spot, circuit->GetLastFrame() + SKIP_FRAMES});
	}
	DropHold(hold);
}

void CMexBoard::OnBuilderDestroyed(CCircuitUnit* unit)
{
	toAssign.erase(std::remove(toAssign.begin(), toAssign.end(), unit), toAssign.end());
	const SHold* hold = FindHold(unit);
	if (hold != nullptr) {  // its next spot (the one it walked to ended with it)
		IBuilderTask* task = hold->task;
		DropHold(hold);
		circuit->GetBuilderManager()->AbortTask(task);  // its spot reopens (CBMexTask::Cancel)
	}
	isDirty = true;
}

void CMexBoard::End()
{
	std::vector<SHold> old;
	old.swap(holds);
	CBuilderManager* builderMgr = circuit->GetBuilderManager();
	for (const SHold& h : old) {
		if (h.unit->GetTask() != h.task) {
			builderMgr->AbortTask(h.task);
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
	// Also once a second: a spot can turn unsafe, open or close with no builder event
	const int frame = circuit->GetLastFrame();
	if ((isDirty || (frame - matchFrame >= FRAMES_PER_SEC)) && (matchFrame != frame)) {
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
		const SBuildChain* chain = circuit->GetBuilderManager()->GetBuildChain(taskB->GetBuildType(), taskB->GetBuildDef());
		const CCircuitDef* energyDef = circuit->GetEconomyManager()->GetChainEnergy(chain, outFrom);
		outBusy += (energyDef != nullptr) ? energyDef->GetCostM() / buildSpeed : 0.f;
	}
	return true;
}

// One match's working set, by spot id
struct CMexBoard::SMatch {
	struct SBuilder {
		CCircuitUnit* unit;
		AIFloat3 from;  // where its walk to a spot would start
		float busy;  // seconds before it can
		int hold;  // the spot it holds, -1 = none
		bool isFixed;  // keeps it: walking to it, or fleeing
	};
	struct SPair {
		int b, spot;  // builder, spot
		float arrival, key;
	};
	std::vector<bool> isSpot;  // one some builder could take, or one held
	std::vector<SBuilder> builders;
	std::vector<SPair> pairs;
	std::vector<int> takenBy;  // builder, -1 = none
};

void CMexBoard::Match(CCircuitUnit* asker, const CQueryCostMap* askerQuery)
{
	const int frame = circuit->GetLastFrame();
	matchFrame = frame;
	isDirty = false;
	skips.erase(std::remove_if(skips.begin(), skips.end(), [frame](const SSkip& s) { return s.until <= frame; }),
			skips.end());

	SMatch m;
	MarkSpots(m.isSpot);
	RateBuilders(asker, askerQuery, m);
	Pick(m);
	Apply(asker, m);
}

void CMexBoard::MarkSpots(std::vector<bool>& outIsSpot) const
{
	// Open ones some builder could take (not an ally's), and held ones
	CEconomyManager* economyMgr = circuit->GetEconomyManager();
	outIsSpot.assign(circuit->GetMetalManager()->GetSpots().size(), false);
	for (int i : economyMgr->GetTestedSpots()) {
		outIsSpot[i] = economyMgr->IsAllyOpenSpot(i) || IsHeld(i);
	}
}

void CMexBoard::RateBuilders(CCircuitUnit* asker, const CQueryCostMap* askerQuery, SMatch& m)
{
	// Each builder, and every spot its own picker would take: the arrival, and the score pairs go by
	const int frame = circuit->GetLastFrame();
	CBuilderManager* builderMgr = circuit->GetBuilderManager();
	CEconomyManager* economyMgr = circuit->GetEconomyManager();
	CMetalManager* metalMgr = circuit->GetMetalManager();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	CThreatMap* threatMap = circuit->GetThreatMap();
	CPathFinder* pathfinder = circuit->GetPathfinder();
	const CMetalData::Metals& metalSpots = metalMgr->GetSpots();
	const CMetalManager::ClusterScore scores[2] = {economyMgr->GetMexScore(false), economyMgr->GetMexScore(true)};
	std::vector<int> nearSpots;
	for (CCircuitUnit* unit : builderMgr->GetWorkers()) {
		SHold* hold = FindHold(unit);
		const IUnitTask* task = unit->GetTask();
		const AIFloat3& pos = unit->GetPos(frame);
		SMatch::SBuilder b = {unit, pos, 0.f, (hold != nullptr) ? hold->spot : -1, false};
		if (hold != nullptr) {
			const bool isFlee = (task->GetType() == IUnitTask::Type::RETREAT);
			hold->fleeFrame = !isFlee ? -1 : (hold->fleeFrame < 0) ? frame : hold->fleeFrame;
			if (task != hold->task) {
				hold->failFrame = -1;  // counts only while it walks there
			}
			if (isFlee && (frame - hold->fleeFrame <= holdFrames)) {
				b.isFixed = true;
				m.builders.push_back(b);
				continue;
			}
		}
		if (!GetStart(unit, hold, b.from, b.busy)) {
			continue;
		}
		const std::function<bool (const AIFloat3& pos)> isJobOk = builderMgr->GetMexJobTest(unit);  // sets its threat type
		if (!isJobOk) {
			continue;  // a commander off mex jobs
		}
		const CCircuitDef* cdef = unit->GetCircuitDef();
		const ICoreUnit::Id unitId = unit->GetId();
		// Keeping a spot: its own danger rule decides; taking one also wants no threat there (as CanReachAtSafe)
		auto isKeepOk = [&](int spot) {
			const AIFloat3& spotPos = metalSpots[spot].position;
			return m.isSpot[spot]
				&& std::none_of(skips.begin(), skips.end(), [unitId, spot](const SSkip& s) {
					return (s.unitId == unitId) && (s.spot == spot);
				})
				&& terrainMgr->CanReachAt(unit, spotPos, cdef->GetBuildDistance()) && isJobOk(spotPos);
		};
		CMetalData::PointPredicate isPairOk = [&](int spot) {
			return (threatMap->GetThreatAt(metalSpots[spot].position) <= THREAT_MIN) && isKeepOk(spot);
		};
		// A walker keeps its spot until that has failed the test for FAIL_FRAMES (a raider passing it doesn't turn it)
		if ((hold != nullptr) && (task == hold->task)) {
			hold->failFrame = isKeepOk(hold->spot) ? -1 : (hold->failFrame < 0) ? frame : hold->failFrame;
			if ((hold->failFrame < 0) || (frame - hold->failFrame < FAIL_FRAMES)) {
				b.isFixed = true;
				m.builders.push_back(b);
				continue;
			}
		}

		const float speed = std::max(cdef->GetSpeed(), 1.f);
		const float costSpeed = speed / pathfinder->GetSquareSize() * COST_BASE;
		const int buildDist = std::max<int>(cdef->GetBuildDistance(), pathfinder->GetSquareSize());
		// Its cost map, if that started where its walk would (else a straight line)
		const auto it = builderMgr->costQueries.find(unit);
		const IPathQuery* query = ((unit == asker) && (askerQuery != nullptr)) ? askerQuery
				: ((it != builderMgr->costQueries.end()) && (it->second->GetState() == IPathQuery::State::READY))
					? it->second.get() : nullptr;
		const CQueryCostMap* costMap = static_cast<const CQueryCostMap*>(query);
		if ((costMap != nullptr) && (costMap->GetStartPos().SqDistance2D(b.from) >= SQUARE(buildDist))) {
			costMap = nullptr;
		}
		// As the mex search: only the mex_front.clusters nearest clusters with such a spot, on its walk around threat
		// walls (the front lean would pull a builder across the map)
		nearSpots.clear();
		metalMgr->FindNearSpots(b.from, isPairOk, economyMgr->GetMexFrontClusters(), nearSpots);
		const CMetalManager::ClusterScore& score = scores[cdef->IsRoleComm() ? 1 : 0];
		const int bi = m.builders.size();
		for (int spot : nearSpots) {
			const AIFloat3& spotPos = metalSpots[spot].position;
			float walk = b.from.distance2D(spotPos) / speed;
			if ((costMap != nullptr) && (b.from.SqDistance2D(spotPos) >= SQUARE(buildDist))) {
				const float cost = costMap->GetCostAt(spotPos, buildDist);
				if (cost < 0.f) {
					continue;  // buildings block the way
				}
				walk = cost / costSpeed;
			}
			const float arrival = b.busy + walk;
			const int cluster = metalMgr->GetCluster(spot);
			const float key = (score && (cluster >= 0)) ? score(cluster, arrival) : arrival;
			m.pairs.push_back({bi, spot, arrival, key});
		}
		m.builders.push_back(b);
	}
}

void CMexBoard::Pick(SMatch& m) const
{
	// Builders that keep their spots first, then lowest score first. A commander never takes a spot another free
	// builder reaches sooner; the second pass gives it what the first left because that builder went elsewhere
	const std::vector<SMatch::SBuilder>& builders = m.builders;
	std::vector<SMatch::SPair>& pairs = m.pairs;
	std::sort(pairs.begin(), pairs.end(), [](const SMatch::SPair& a, const SMatch::SPair& b) { return a.key < b.key; });
	std::vector<std::vector<int>> bySpot(m.isSpot.size());  // pair indices
	for (int i = 0; i < (int)pairs.size(); ++i) {
		bySpot[pairs[i].spot].push_back(i);
	}
	std::vector<bool> isMatched(builders.size(), false);
	m.takenBy.assign(m.isSpot.size(), -1);
	for (int bi = 0; bi < (int)builders.size(); ++bi) {
		if (builders[bi].isFixed) {
			isMatched[bi] = true;
			m.takenBy[builders[bi].hold] = bi;
		}
	}
	for (int pass = 0; pass < 2; ++pass) {
		for (const SMatch::SPair& p : pairs) {
			if (isMatched[p.b] || (m.takenBy[p.spot] >= 0)) {
				continue;
			}
			if (builders[p.b].unit->GetCircuitDef()->IsRoleComm()
				&& std::any_of(bySpot[p.spot].begin(), bySpot[p.spot].end(), [&](int i) {
					return (pairs[i].b != p.b) && !isMatched[pairs[i].b] && (pairs[i].arrival < p.arrival);
				}))
			{
				continue;
			}
			isMatched[p.b] = true;
			m.takenBy[p.spot] = p.b;
		}
	}
}

void CMexBoard::Apply(CCircuitUnit* asker, SMatch& m)
{
	// A spot held before keeps its job, for its new builder if it moved (one walking to it leaves it); one nobody holds
	// now gives its job up; a newly held one gets a job
	CBuilderManager* builderMgr = circuit->GetBuilderManager();
	CEconomyManager* economyMgr = circuit->GetEconomyManager();
	std::vector<SHold> kept;
	std::vector<IBuilderTask*> released;
	std::vector<CCircuitUnit*> bumped;
	for (SHold& h : holds) {
		const int bi = m.takenBy[h.spot];
		CCircuitUnit* unit = (bi >= 0) ? m.builders[bi].unit : nullptr;
		if (unit != h.unit) {
			if (h.unit->GetTask() == h.task) {
				bumped.push_back(h.unit);
			}
			if (unit == nullptr) {
				released.push_back(h.task);
				continue;
			}
			h.unit = unit;
			h.fleeFrame = h.failFrame = -1;
		}
		m.takenBy[h.spot] = -1;  // has its job
		kept.push_back(h);
	}
	holds.swap(kept);
	for (CCircuitUnit* unit : bumped) {
		unit->GetTask()->RemoveAssignee(unit);  // to idle
	}
	for (IBuilderTask* task : released) {
		builderMgr->AbortTask(task);  // its spot reopens (CBMexTask::Cancel)
	}
	for (int spot = 0; spot < (int)m.takenBy.size(); ++spot) {
		if (m.takenBy[spot] >= 0) {
			holds.push_back({m.builders[m.takenBy[spot]].unit, economyMgr->EnqueueMex(spot), spot, -1, -1});
		}
	}
	// economy.opening.log: a walk the board ended, and which way its builder's next spot lies
	for (CCircuitUnit* unit : bumped) {
		CBuilderManager::SOpeningStats* stats = builderMgr->GetOpeningStats(unit);
		const SHold* next = FindHold(unit);
		if (stats != nullptr) {
			++stats->moves;
			if (next != nullptr) {
				++(builderMgr->IsJobForward(unit, next->task->GetPosition()) ? stats->forward : stats->back);
			}
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

/*
 * SupportTask.cpp
 *
 *  Created on: Jul 3, 2016
 *      Author: rlcevg
 */

#include "task/fighter/SupportTask.h"
#include "task/fighter/AttackTask.h"
#include "task/fighter/SquadTask.h"
#include "task/TaskManager.h"
#include "map/ThreatMap.h"
#include "module/MilitaryManager.h"
#include "setup/SetupManager.h"
#include "terrain/TerrainManager.h"
#include "terrain/path/PathFinder.h"
#include "terrain/path/QueryPathMulti.h"
#include "unit/action/MoveAction.h"
#include "unit/CircuitUnit.h"
#include "CircuitAI.h"
#include "util/Utils.h"

#include "AISCommands.h"

namespace circuit {

using namespace springai;

// Updates between host picks and route requests
static constexpr int WALK_UPDATES = 4;
// The route's threat cap: this x the host's power (it walks alone, towards where the host fights), never below the
// threat where it stands
static constexpr float WALK_THREAT_MOD = 0.5f;
// Another host replaces the current one only with this much more power per shield
static constexpr float HOST_SWITCH_MOD = 1.25f;

CSupportTask::CSupportTask(ITaskManager* mgr)
		: IFighterTask(mgr, FightType::SUPPORT, 1.f)
		, host(nullptr)
		, isHolding(false)
{
	const AIFloat3& pos = manager->GetCircuit()->GetSetupManager()->GetBasePos();
	position = utils::get_radial_pos(pos, SQUARE_SIZE * 32);
}

CSupportTask::~CSupportTask()
{
}

void CSupportTask::AssignTo(CCircuitUnit* unit)
{
	IFighterTask::AssignTo(unit);
	ITravelAction* travelAction = new CMoveAction(unit, manager->GetCircuit()->GetPathfinder()->GetSquareSize());
	unit->PushTravelAct(travelAction);
	travelAction->StateWait();
}

void CSupportTask::RemoveAssignee(CCircuitUnit* unit)
{
	IFighterTask::RemoveAssignee(unit);
	if (units.empty()) {
		manager->AbortTask(this);
	}
}

void CSupportTask::Start(CCircuitUnit* unit)
{
	updCount = 0;  // pick a host at the next update
}

void CSupportTask::Update()
{
	if (updCount++ % WALK_UPDATES != 0) {
		return;
	}
	CCircuitUnit* unit = *units.begin();
	if (unit->Blocker() != nullptr) {
		return;  // Do not interrupt current action
	}
	const int frame = manager->GetCircuit()->GetLastFrame();
	ISquadTask* squad = FindHost(unit, frame);
	if (squad == nullptr) {
		host = nullptr;
		HoldNearBase(unit);
		return;
	}
	host = squad;
	isHolding = false;
	const float joinDist = static_cast<CMilitaryManager*>(manager)->GetAttackMerge().distance;
	if (unit->GetPos(frame).SqDistance2D(squad->GetLeaderPos(frame)) < SQUARE(joinDist)) {
		manager->AssignTask(unit, squad);  // this task aborts with its only unit
		return;
	}
	WalkTo(unit, squad, frame);
}

ISquadTask* CSupportTask::FindHost(CCircuitUnit* unit, int frame) const
{
	CMilitaryManager* militaryMgr = static_cast<CMilitaryManager*>(manager);
	CTerrainManager* terrainMgr = manager->GetCircuit()->GetTerrainManager();
	const float speedMin = militaryMgr->GetEscortCfg().speedMin;
	const float speedRatio = militaryMgr->GetAttackMerge().speedRatio;
	// Shields walking to each group, this one left out
	std::map<const ISquadTask*, int> walkers;
	for (IFighterTask* t : militaryMgr->GetTasks(FightType::SUPPORT)) {
		const CSupportTask* walker = static_cast<const CSupportTask*>(t);
		if ((walker != this) && (walker->GetHost() != nullptr)) {
			++walkers[walker->GetHost()];
		}
	}
	// Power per shield already there: power / (1 + shields), for groups with an armed unit that it may join
	auto scoreOf = [&](ISquadTask* squad, bool isAttack) {
		CCircuitUnit* leader = squad->GetLeader();
		if ((leader == nullptr) || squad->IsDead()
			|| !terrainMgr->CanMoveToPos(unit->GetArea(), leader->GetPos(frame)))
		{
			return -1.f;
		}
		const bool isOk = isAttack ? static_cast<CAttackTask*>(squad)->IsCompatible(unit)
				: (squad->IsMoveCompatible(unit, speedRatio)
					&& squad->IsSpeedMixOk(ISquadTask::GetSpeedMix(unit), speedMin));
		if (!isOk) {
			return -1.f;
		}
		bool isArmed = false;
		int shields = 0;
		for (CCircuitUnit* ass : squad->GetAssignees()) {
			CCircuitDef* cdef = ass->GetCircuitDef();
			if (cdef->IsAttacker()) {
				isArmed = true;
			} else if (cdef->IsRoleSupport()) {
				++shields;
			}
		}
		if (!isArmed) {
			return -1.f;
		}
		auto it = walkers.find(squad);
		shields += (it != walkers.end()) ? it->second : 0;
		return squad->GetAttackPower() / (1 + shields);
	};
	// Attack groups first; gathering groups only when no attack group will do
	for (FightType ft : {FightType::ATTACK, FightType::DEFEND}) {
		ISquadTask* best = nullptr;
		float bestScore = 0.f;
		float hostScore = -1.f;
		for (IFighterTask* t : militaryMgr->GetTasks(ft)) {
			ISquadTask* squad = static_cast<ISquadTask*>(t);
			const float score = scoreOf(squad, FightType::ATTACK == ft);
			if (squad == host) {
				hostScore = score;
			}
			if (score > bestScore) {
				bestScore = score;
				best = squad;
			}
		}
		if ((hostScore > 0.f) && (bestScore <= hostScore * HOST_SWITCH_MOD)) {
			return host;  // still good: don't turn round for a small gain
		}
		if (best != nullptr) {
			return best;
		}
	}
	return nullptr;
}

void CSupportTask::WalkTo(CCircuitUnit* unit, ISquadTask* squad, int frame)
{
	if (!IsQueryReady(unit)) {
		return;
	}
	CCircuitAI* circuit = manager->GetCircuit();
	CPathFinder* pathfinder = circuit->GetPathfinder();
	const AIFloat3& startPos = unit->GetPos(frame);
	// Capped: it walks alone, round danger, towards the host (and the danger the host stands in, at its power)
	const float maxThreat = std::max(squad->GetAttackPower() * WALK_THREAT_MOD,
			GetTaskThreatAt(unit, startPos) + THREAT_MIN);
	std::shared_ptr<IPathQuery> query = pathfinder->CreatePathMultiQuery(
			unit, circuit->GetThreatMap(), frame,
			startPos, pathfinder->GetSquareSize(), {squad->GetLeaderPos(frame)}, nullptr, maxThreat, true);
	pathQueries[unit] = query;
	query->HoldTask(this);

	pathfinder->RunQuery(query, [this](const IPathQuery* query) {
		if (this->IsQueryAlive(query)) {
			this->ApplyPath(static_cast<const CQueryPathMulti*>(query));
		}
	});
}

void CSupportTask::ApplyPath(const CQueryPathMulti* query)
{
	const std::shared_ptr<PathInfo>& pPath = query->GetPathInfo();
	CCircuitUnit* unit = query->GetUnit();
	if (pPath->posPath.empty()) {
		host = nullptr;  // no safe way there: wait at base, another pick at the next round
		HoldNearBase(unit);
		return;
	}
	unit->GetTravelAct()->SetPath(pPath);
	unit->GetTravelAct()->StateActivate();
}

void CSupportTask::HoldNearBase(CCircuitUnit* unit)
{
	if (isHolding) {
		return;
	}
	isHolding = true;
	CCircuitAI* circuit = manager->GetCircuit();
	AIFloat3 pos = position;
	CTerrainManager::CorrectPosition(pos);
	pos = circuit->GetTerrainManager()->FindBuildSite(unit->GetCircuitDef(), pos, 300.0f, UNIT_COMMAND_BUILD_NO_FACING);
	unit->GetTravelAct()->StateWait();
	TRY_UNIT(circuit, unit,
		unit->CmdMoveTo(pos, UNIT_COMMAND_OPTION_RIGHT_MOUSE_KEY, circuit->GetLastFrame() + FRAMES_PER_SEC * 60);
		unit->CmdWantedSpeed(NO_SPEED_LIMIT);
	)
}

} // namespace circuit

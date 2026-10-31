/*
 * EscortTask.cpp
 *
 *  Created on: Oct 31, 2026
 *      Author: Claude
 */

#include "task/builder/EscortTask.h"
#include "task/fighter/SquadTask.h"
#include "task/TaskManager.h"
#include "module/BuilderManager.h"
#include "module/MilitaryManager.h"
#include "setup/SetupManager.h"
#include "terrain/TerrainManager.h"
#include "unit/action/TravelAction.h"
#include "CircuitAI.h"
#include "util/Utils.h"

#include "AISCommands.h"

namespace circuit {

using namespace springai;

CBEscortTask::CBEscortTask(ITaskManager* mgr, ISquadTask* squad)
		: IBuilderTask(mgr, Priority::NORMAL, nullptr, -RgtVector,
					   Type::BUILDER, BuildType::GUARD, 0.f, 0.f, 0)
		, squad(squad)
		, fightType(static_cast<int>(squad->GetFightType()))
		, orderPos(-RgtVector)
		, isFightOrder(false)
{
}

CBEscortTask::~CBEscortTask()
{
}

bool CBEscortTask::CanAssignTo(CCircuitUnit* unit) const
{
	return units.empty();
}

void CBEscortTask::AssignTo(CCircuitUnit* unit)
{
	IBuilderTask::AssignTo(unit);

	static_cast<CBuilderManager*>(manager)->DelBuildPower(unit);
}

void CBEscortTask::RemoveAssignee(CCircuitUnit* unit)
{
	IBuilderTask::RemoveAssignee(unit);
	if (units.empty()) {
		manager->AbortTask(this);
	}

	static_cast<CBuilderManager*>(manager)->AddBuildPower(unit);
}

void CBEscortTask::Start(CCircuitUnit* unit)
{
	// Orders are given directly, not by travel action
	if (unit->GetTravelAct() != nullptr) {
		unit->GetTravelAct()->StateFinish();
	}
	Follow(unit, true);
}

void CBEscortTask::Update()
{
	if (!IsSquadAlive()) {
		manager->AbortTask(this);
		return;
	}
	auto assignees = units;  // Follow may abort the task
	for (CCircuitUnit* unit : assignees) {
		Follow(unit, false);
		if (isDead) {
			break;
		}
	}
}

void CBEscortTask::Stop(bool done)
{
	for (CCircuitUnit* unit : units) {
		static_cast<CBuilderManager*>(manager)->AddBuildPower(unit);
	}

	IBuilderTask::Stop(done);
}

void CBEscortTask::OnUnitIdle(CCircuitUnit* unit)
{
	if (!IsSquadAlive()) {
		manager->AbortTask(this);
		return;
	}
	Follow(unit, true);
}

bool CBEscortTask::IsSquadAlive() const
{
	CMilitaryManager* militaryMgr = manager->GetCircuit()->GetMilitaryManager();
	// NOTE: squad may be deleted already, do not dereference before lookup
	const std::set<IFighterTask*>& tasks = militaryMgr->GetTasks(static_cast<IFighterTask::FightType>(fightType));
	return (tasks.find(static_cast<IFighterTask*>(squad)) != tasks.end())
		&& !squad->IsDead() && (squad->GetLeader() != nullptr);
}

void CBEscortTask::Follow(CCircuitUnit* unit, bool isForce)
{
	CCircuitAI* circuit = manager->GetCircuit();
	const CBuilderManager::SEscort& escort = static_cast<CBuilderManager*>(manager)->GetEscortCfg();
	const int frame = circuit->GetLastFrame();

	CCircuitUnit* leader = squad->GetLeader();
	const AIFloat3& leaderPos = leader->GetPos(frame);
	const AIFloat3& basePos = circuit->GetSetupManager()->GetBasePos();

	// Stay a bit behind the group, on the side of the base
	AIFloat3 dir = basePos - leaderPos;
	dir.y = 0.f;
	const float baseDist = dir.Length2D();
	AIFloat3 pos = leaderPos;
	if (baseDist > 1.f) {
		pos += dir * (std::min(escort.behind, baseDist) / baseDist);
	}
	CTerrainManager::CorrectPosition(pos);
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	const float buildDist = unit->GetCircuitDef()->GetBuildDistance();
	if (!terrainMgr->CanReachAt(unit, pos, buildDist)) {
		pos = leaderPos;
		if (!terrainMgr->CanReachAt(unit, pos, buildDist)) {
			manager->AbortTask(this);  // group went where builder can't follow (water, cliffs)
			return;
		}
	}

	// Group falls back if it moves towards the base, unless it is fighting
	const AIFloat3 vel = leader->GetUnit()->GetVel();
	const bool isFallback = (State::ENGAGE != squad->GetState())
			&& (vel.SqLength2D() > SQUARE(0.5f))
			&& (vel.x * dir.x + vel.z * dir.z > 0.f);
	const float sqDist = unit->GetPos(frame).SqDistance2D(pos);
	const bool isLagging = sqDist > SQUARE(escort.follow);
	const bool isFight = !isFallback && !isLagging;

	// Re-issuing the order would interrupt current repair: only do it if it changed noticeably
	if (!isForce && (isFight == isFightOrder) && utils::is_valid(orderPos)
		&& (orderPos.SqDistance2D(pos) < SQUARE(SQUARE_SIZE * 24)))
	{
		return;
	}
	orderPos = pos;
	isFightOrder = isFight;

	if (!isFight && (sqDist < SQUARE(SQUARE_SIZE * 8))) {
		return;  // already there, avoid move-idle loop
	}
	TRY_UNIT(circuit, unit,
		if (isFight) {
			// Patrol never finishes and repairs/reclaims along the way like fight
			unit->GetUnit()->PatrolTo(pos);
		} else {
			unit->CmdMoveTo(pos, UNIT_COMMAND_OPTION_RIGHT_MOUSE_KEY, frame + FRAMES_PER_SEC * 60);
		}
		unit->CmdWantedSpeed(NO_SPEED_LIMIT);
	)
}

} // namespace circuit

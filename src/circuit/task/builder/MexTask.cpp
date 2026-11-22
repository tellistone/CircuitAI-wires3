/*
 * MexTask.cpp
 *
 *  Created on: Jan 31, 2015
 *      Author: rlcevg
 */

#include "task/builder/MexTask.h"
#include "task/fighter/GuardTask.h"
#include "task/TaskManager.h"
#include "map/ThreatMap.h"
#include "module/EconomyManager.h"
#include "module/BuilderManager.h"
#include "module/MexBoard.h"
#include "module/MilitaryManager.h"
#include "resource/MetalManager.h"
#include "terrain/TerrainManager.h"
#include "CircuitAI.h"
#include "util/Utils.h"

#include "spring/SpringCallback.h"
#include "spring/SpringMap.h"

#include "AISCommands.h"

namespace circuit {

using namespace springai;

CBMexTask::CBMexTask(ITaskManager* mgr, Priority priority,
					 CCircuitDef* buildDef, const AIFloat3& position,
					 float cost, int timeout)
		: IBuilderTask(mgr, priority, buildDef, position, Type::BUILDER, BuildType::MEX, cost, 0.f, timeout)
{
}

CBMexTask::~CBMexTask()
{
}

bool CBMexTask::CanAssignTo(CCircuitUnit* unit) const
{
	CCircuitAI* circuit = manager->GetCircuit();
	const CCircuitUnit* holder = circuit->GetBuilderManager()->GetMexBoard()->GetHolder(this);  // economy.mex_board
	if (!IBuilderTask::CanAssignTo(unit) || IsSpotTaken()  // an allied AI's builder heads there already
		|| ((holder != nullptr) && (holder != unit)))
	{
		return false;
	}
	CMilitaryManager* militaryMgr = circuit->GetMilitaryManager();
	// guard.summon: the guard is called as the builder heads off safe ground, never waited for
	if (unit->GetCircuitDef()->IsAttacker() || militaryMgr->GetGuardCfg().isSummon) {
		return true;
	}
	// TODO: Naked expansion on big maps
	int cluster = circuit->GetMetalManager()->FindNearestCluster(GetPosition());
	if ((cluster < 0) || militaryMgr->HasDefence(cluster)) {
		return true;
	}
	CFGuardTask* guard = static_cast<CFGuardTask*>(militaryMgr->GetGuardTask(unit));
	// Guarded, or guard released because the builder is in a safe spot (next idle guard-role unit refills the slot once it leaves)
	return ((guard != nullptr) && (!guard->GetAssignees().empty() || guard->IsVipSafe()))
			|| (circuit->GetLastFrame() > circuit->GetEconomyManager()->GetMexEscortFrame());
}

bool CBMexTask::IsSpotTaken() const
{
	return (target == nullptr) && (claimedSpot < 0) && utils::is_valid(buildPos)
			&& !manager->GetCircuit()->GetMetalManager()->IsOpenSpot(buildPos);
}

void CBMexTask::AssignTo(CCircuitUnit* unit)
{
	IBuilderTask::AssignTo(unit);
	// Claim the spot for the whole ally team as soon as a builder heads for it, not on arrival (Execute): allied AIs
	// share the metal manager, so their searches skip it and they don't all walk to the same spot
	TryClaim();
}

void CBMexTask::RemoveAssignee(CCircuitUnit* unit)
{
	IBuilderTask::RemoveAssignee(unit);
	// Nobody walks there any more (retreat, a nearer job): let an ally have the spot.
	// The next builder of ours to take the job claims it again
	if (units.empty()) {
		Unclaim();
	}
}

bool CBMexTask::IsReachable(CCircuitUnit* unit, const AIFloat3& endPos)
{
	// Its mex board walk: the board judges threat at the spot (CMexBoard::RateBuilders), so a raider passing it
	// doesn't end the job
	CCircuitAI* circuit = manager->GetCircuit();
	if (circuit->GetBuilderManager()->GetMexBoard()->GetHolder(this) == unit) {
		return circuit->GetTerrainManager()->CanReachAt(unit, endPos, unit->GetCircuitDef()->GetBuildDistance());
	}
	return IBuilderTask::IsReachable(unit, endPos);
}

void CBMexTask::Update()
{
	if (units.empty() && IsSpotTaken()) {  // an allied AI's builder heads there: free the job slot now, not on timeout
		manager->AbortTask(this);
		return;
	}
	IBuilderTask::Update();
}

bool CBMexTask::TryClaim()
{
	if ((target != nullptr) || !utils::is_valid(buildPos)) {
		return false;
	}
	CMetalManager* metalMgr = manager->GetCircuit()->GetMetalManager();
	const int index = metalMgr->FindNearestSpot(buildPos);
	if ((index < 0) || ((index != claimedSpot) && !metalMgr->IsOpenSpot(index))) {
		return false;
	}
	metalMgr->SetOpenSpot(index, false);
	claimedSpot = index;
	return true;
}

void CBMexTask::Unclaim()
{
	// Never once a frame is placed: it holds the spot, and the mex destroyed handler reopens it
	if ((claimedSpot >= 0) && (target == nullptr)) {
		manager->GetCircuit()->GetMetalManager()->SetOpenSpot(claimedSpot, true);
		claimedSpot = -1;
	}
}

void CBMexTask::Cancel()
{
	if ((target == nullptr) && utils::is_valid(buildPos)) {
		CCircuitAI* circuit = manager->GetCircuit();
		int index = circuit->GetMetalManager()->FindNearestSpot(buildPos);
		circuit->GetEconomyManager()->SetOpenSpot(index, true);
		SetBuildPos(-RgtVector);
	}
	Unclaim();  // only our own claim: a spot an ally heads for stays closed
}

// guard.summon: no escort gate, so builders take undefended clusters at once. The cluster's preventive towers are
// booked as its first mex starts, not once every spot is queued (the mex chain's rule below 10 m/s): a builder is
// there, and the tower is its next job
static void BookClusterDefence(CCircuitAI* circuit, int index, const AIFloat3& pos)
{
	CMilitaryManager* militaryMgr = circuit->GetMilitaryManager();
	const int cluster = circuit->GetMetalManager()->GetCluster(index);
	if (militaryMgr->GetGuardCfg().isSummon && (cluster >= 0) && !militaryMgr->HasDefence(cluster)) {
		militaryMgr->MakeDefence(cluster, pos, nullptr, true);
	}
}

void CBMexTask::Execute(CCircuitUnit* unit)
{
	CCircuitAI* circuit = manager->GetCircuit();
	TRY_UNIT(circuit, unit,
		unit->CmdPriority(ClampPriority());
	)

	const int frame = circuit->GetLastFrame();
	if (target != nullptr) {
		TRY_UNIT(circuit, unit,
			unit->GetUnit()->Repair(target->GetUnit(), UNIT_CMD_OPTION, frame + FRAMES_PER_SEC * 60);
		)
		return;
	}
	CMetalManager* metalMgr = circuit->GetMetalManager();
	CEconomyManager* economyMgr = circuit->GetEconomyManager();
	UnitDef* buildUDef = buildDef->GetDef();
	if (utils::is_valid(buildPos)) {
		int index = metalMgr->FindNearestSpot(buildPos);
		const CCircuitDef* escortDef = GetEscortDef(unit);
		if ((index >= 0) && (escortDef != nullptr) && economyMgr->IsEnemyMexAt(buildPos)) {
			// An enemy mex we know of holds it: walk up into the turret's range, where OnUnitIdle queues it
			const float range = escortDef->GetMaxRange();
			AIFloat3 pos = buildPos - (buildPos - unit->GetPos(frame)).Normalize2D() * range * 0.9f;
			CTerrainManager::CorrectPosition(pos);
			TRY_UNIT(circuit, unit,
				unit->CmdMoveTo(pos, UNIT_CMD_OPTION, frame + FRAMES_PER_SEC * 60);
			)
			return;
		}
		if (index >= 0) {
			if (circuit->GetMap()->IsPossibleToBuildAt(buildUDef, buildPos, facing)) {
				if (TryClaim()) {  // ours, or still open
					TRY_UNIT(circuit, unit,
						unit->GetUnit()->Build(buildUDef, buildPos, facing, 0, frame + FRAMES_PER_SEC * 60);
					)
					BookClusterDefence(circuit, index, buildPos);
					return;
				} else {
					economyMgr->SetOpenSpot(index, true);
				}
			} else {
				Unclaim();
				economyMgr->SetOpenSpot(index, true);
			}
		}
	}

	// NOTE: Unsafe fallback expansion (mex can be behind enemy lines)
	const CMetalData::Metals& spots = metalMgr->GetSpots();
	CMap* map = circuit->GetMap();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	CCircuitDef* mexDef = buildDef;
	circuit->GetThreatMap()->SetThreatType(unit);
	CMetalData::PointPredicate predicate = [&spots, economyMgr, map, mexDef, terrainMgr, unit](const int index) {
		return (economyMgr->IsAllyOpenSpot(index)
				&& terrainMgr->CanBeBuiltAtSafe(mexDef, spots[index].position)  // hostile environment
				&& terrainMgr->CanReachAtSafe(unit, spots[index].position, unit->GetCircuitDef()->GetBuildDistance())
				&& map->IsPossibleToBuildAt(mexDef->GetDef(), spots[index].position, UNIT_COMMAND_BUILD_NO_FACING));
	};
	int index = metalMgr->FindNearestSpot(position, predicate);

	if (index >= 0) {
		SetBuildPos(spots[index].position);
		economyMgr->SetOpenSpot(index, false);
		TryClaim();  // for the ally team too, or allied builders walk here meanwhile
		TRY_UNIT(circuit, unit,
			unit->GetUnit()->Build(buildUDef, buildPos, facing, 0, frame + FRAMES_PER_SEC * 60);
		)
		BookClusterDefence(circuit, index, buildPos);
	} else {
//		buildPos = -RgtVector;
		// Fallback to Guard/Assist/Patrol
		manager->FallbackTask(unit);
	}
}

CCircuitDef* CBMexTask::GetEscortDef(CCircuitUnit* unit) const
{
	CCircuitAI* circuit = manager->GetCircuit();
	CCircuitDef* def = circuit->GetMilitaryManager()->GetDefaultPorc();
	return ((def != nullptr) && def->IsAvailable(circuit->GetLastFrame()) && unit->GetCircuitDef()->CanBuild(def))
			? def : nullptr;
}

void CBMexTask::OnUnitIdle(CCircuitUnit* unit)
{
	/*
	 * Idle by an enemy mex on the spot (seen on the way, or walked up to from Execute): a turret against it first.
	 * The enemy mex is the one we know of, so a builder that stepped back out of its sight still finds it
	 */
	CCircuitAI* circuit = manager->GetCircuit();
	CCircuitDef* def = GetEscortDef(unit);
	const AIFloat3& pos = unit->GetPos(circuit->GetLastFrame());
	if ((def != nullptr) && (buildPos.SqDistance2D(pos) < SQUARE(def->GetMaxRange() + 200.0f))  // 200 elmos
		&& circuit->GetEconomyManager()->IsEnemyMexAt(buildPos))
	{
		// A turret job of ours that reaches the enemy mex: help it
		CBuilderManager* builderMgr = circuit->GetBuilderManager();
		IBuilderTask* task = nullptr;
		for (IBuilderTask* t : builderMgr->GetTasks(IBuilderTask::BuildType::DEFENCE)) {
			const CCircuitDef* tdef = t->GetBuildDef();
			if ((tdef != nullptr) && tdef->IsAttacker() && t->CanAssignTo(unit)
				&& (buildPos.SqDistance2D(t->GetTaskPos()) < SQUARE(tdef->GetMaxRange())))
			{
				task = t;
				break;
			}
		}
		// Else a new one, unless a friendly turret reaches it already or the account (porcupine.budget) is short:
		// then the spot reopens, and the mex search passes it over until that changes
		CMilitaryManager* militaryMgr = circuit->GetMilitaryManager();
		if ((task == nullptr)
			&& (circuit->GetEconomyManager()->IsEnemyMexCovered(buildPos) || !militaryMgr->CanAffordTurret(def->GetCostM())))
		{
			manager->AbortTask(this);
			return;
		}
		if (task == nullptr) {
			AIFloat3 newPos = buildPos - (buildPos - pos).Normalize2D() * def->GetMaxRange() * 0.9f;
			CTerrainManager::CorrectPosition(newPos);
			CMilitaryManager::TurretScope scope(militaryMgr, CMilitaryManager::TurretSpend::ESCORT);
			task = builderMgr->EnqueueTask(IBuilderTask::Priority::HIGH, def, newPos, IBuilderTask::BuildType::DEFENCE);
		}
		// TODO: Before BuildTask assign MoveTask(task->GetTaskPos())
		manager->AssignTask(unit, task);
		return;
	}

	IBuilderTask::OnUnitIdle(unit);
}

} // namespace circuit

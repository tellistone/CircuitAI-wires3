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
	if (!IBuilderTask::CanAssignTo(unit) || IsSpotTaken()) {  // an allied AI's builder heads there already
		return false;
	}
	if (unit->GetCircuitDef()->IsAttacker()) {
		return true;
	}
	// TODO: Naked expansion on big maps
	CCircuitAI* circuit = manager->GetCircuit();
	CMilitaryManager* militaryMgr = circuit->GetMilitaryManager();
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
	if ((target != nullptr) || (reservedSpot >= 0) || !utils::is_valid(buildPos)) {
		return false;
	}
	CMetalManager* metalMgr = manager->GetCircuit()->GetMetalManager();
	const int index = metalMgr->FindNearestSpot(buildPos);
	return (index >= 0) && !metalMgr->IsOpenSpot(index);
}

void CBMexTask::AssignTo(CCircuitUnit* unit)
{
	IBuilderTask::AssignTo(unit);

	// Claim the spot for the whole ally team as soon as a builder heads for it, not on arrival (Execute): allied AIs
	// share the metal manager, so their searches skip it and they don't all walk to the same spot
	if ((target != nullptr) || (reservedSpot >= 0) || !utils::is_valid(buildPos)) {
		return;
	}
	CMetalManager* metalMgr = manager->GetCircuit()->GetMetalManager();
	const int index = metalMgr->FindNearestSpot(buildPos);
	if ((index >= 0) && metalMgr->IsOpenSpot(index)) {  // CanAssignTo rejects a taken spot; else Execute picks another
		Reserve(index);
	}
}

void CBMexTask::RemoveAssignee(CCircuitUnit* unit)
{
	IBuilderTask::RemoveAssignee(unit);

	// Nobody walks there any more (retreat, a nearer job) and nothing is placed yet: let an ally have the spot.
	// The next builder of ours to take the job claims it again
	if (units.empty() && (target == nullptr)) {
		Release();
	}
}

void CBMexTask::SetTarget(CCircuitUnit* unit)
{
	IBuilderTask::SetTarget(unit);
	if (unit != nullptr) {
		reservedSpot = -1;  // the frame holds the spot now; the mex destroyed handler reopens it
	}
}

void CBMexTask::Reserve(int index)
{
	if (reservedSpot == index) {
		return;
	}
	Release();
	manager->GetCircuit()->GetMetalManager()->SetOpenSpot(index, false);
	reservedSpot = index;
}

void CBMexTask::Release()
{
	if (reservedSpot < 0) {
		return;
	}
	manager->GetCircuit()->GetMetalManager()->SetOpenSpot(reservedSpot, true);
	reservedSpot = -1;
}

int CBMexTask::FindOpenSpot(CCircuitUnit* unit) const
{
	CCircuitAI* circuit = manager->GetCircuit();
	CMetalManager* metalMgr = circuit->GetMetalManager();
	CEconomyManager* economyMgr = circuit->GetEconomyManager();
	const CMetalData::Metals& spots = metalMgr->GetSpots();
	CMap* map = circuit->GetMap();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	CCircuitDef* mexDef = buildDef;
	CMetalData::PointPredicate predicate = [&spots, economyMgr, map, mexDef, terrainMgr, unit](const int index) {
		return (economyMgr->IsAllyOpenSpot(index)
				&& terrainMgr->CanBeBuiltAtSafe(mexDef, spots[index].position)  // hostile environment
				&& terrainMgr->CanReachAtSafe(unit, spots[index].position, unit->GetCircuitDef()->GetBuildDistance())
				&& map->IsPossibleToBuildAt(mexDef->GetDef(), spots[index].position, UNIT_COMMAND_BUILD_NO_FACING));
	};
	return metalMgr->FindNearestSpot(position, predicate);
}

void CBMexTask::Cancel()
{
	if ((target == nullptr) && utils::is_valid(buildPos)) {
		CCircuitAI* circuit = manager->GetCircuit();
		int index = circuit->GetMetalManager()->FindNearestSpot(buildPos);
		circuit->GetEconomyManager()->SetOpenSpot(index, true);
		SetBuildPos(-RgtVector);
	}
	Release();  // only our own claim: a spot an ally heads for stays closed
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
		if (index >= 0) {
			if (circuit->GetMap()->IsPossibleToBuildAt(buildUDef, buildPos, facing)) {
				if ((reservedSpot == index) || metalMgr->IsOpenSpot(index)) {
					Reserve(index);
					TRY_UNIT(circuit, unit,
						unit->GetUnit()->Build(buildUDef, buildPos, facing, 0, frame + FRAMES_PER_SEC * 60);
					)
					return;
				} else {
					economyMgr->SetOpenSpot(index, true);
				}
			} else {
				Release();
				economyMgr->SetOpenSpot(index, true);
			}
		}
	}

	// NOTE: Unsafe fallback expansion (mex can be behind enemy lines)
	circuit->GetThreatMap()->SetThreatType(unit);
	int index = FindOpenSpot(unit);

	if (index >= 0) {
		SetBuildPos(metalMgr->GetSpots()[index].position);
		economyMgr->SetOpenSpot(index, false);
		Reserve(index);  // for the ally team too, or allied builders walk here meanwhile
		TRY_UNIT(circuit, unit,
			unit->GetUnit()->Build(buildUDef, buildPos, facing, 0, frame + FRAMES_PER_SEC * 60);
		)
	} else {
//		buildPos = -RgtVector;
		// Fallback to Guard/Assist/Patrol
		manager->FallbackTask(unit);
	}
}

void CBMexTask::OnUnitIdle(CCircuitUnit* unit)
{
	/*
	 * Check if unit is idle because of enemy mex ahead and build turret if so.
	 */
	CCircuitAI* circuit = manager->GetCircuit();
	CCircuitDef* def = circuit->GetMilitaryManager()->GetDefaultPorc();
	if ((def == nullptr) || !def->IsAvailable(circuit->GetLastFrame())) {
		IBuilderTask::OnUnitIdle(unit);
		return;
	}

	const float range = def->GetMaxRange();
	const float testRange = range + 200.0f;  // 200 elmos
	const AIFloat3& pos = unit->GetPos(circuit->GetLastFrame());
	if (buildPos.SqDistance2D(pos) < SQUARE(testRange)) {
		CCircuitDef::Id mexDefId = circuit->GetEconomyManager()->GetMexDef()->GetId();
		COOAICallback* clb = circuit->GetCallback();
		// TODO: Use internal CCircuitAI::GetEnemyUnits?
		auto enemies = clb->GetEnemyUnitIdsIn(buildPos, SQUARE_SIZE);
		bool blocked = false;
		for (int enemyId : enemies) {
			if (enemyId == -1) {
				continue;
			}
			CCircuitDef::Id enemyDefId = clb->Unit_GetDefId(enemyId);
			if (enemyDefId == mexDefId) {
				blocked = true;
				break;
			}
		}
		if (blocked) {
			CBuilderManager* builderMgr = circuit->GetBuilderManager();
			IBuilderTask* task = nullptr;
			const float qdist = SQUARE(200.0f);  // 200 elmos
			// TODO: Push tasks into bgi::rtree
			for (IBuilderTask* t : builderMgr->GetTasks(IBuilderTask::BuildType::DEFENCE)) {
				if (pos.SqDistance2D(t->GetTaskPos()) < qdist) {
					task = t;
					break;
				}
			}
			if (task == nullptr) {
				AIFloat3 newPos = buildPos - (buildPos - pos).Normalize2D() * range * 0.9f;
				CTerrainManager::CorrectPosition(newPos);
				CMilitaryManager::TurretScope scope(circuit->GetMilitaryManager(), CMilitaryManager::TurretSpend::ESCORT);
				task = builderMgr->EnqueueTask(IBuilderTask::Priority::HIGH, def, newPos, IBuilderTask::BuildType::DEFENCE);
			}
			// TODO: Before BuildTask assign MoveTask(task->GetTaskPos())
			manager->AssignTask(unit, task);
			return;
		}
	}

	IBuilderTask::OnUnitIdle(unit);
}

} // namespace circuit

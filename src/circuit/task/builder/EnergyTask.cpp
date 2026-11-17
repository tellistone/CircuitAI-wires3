/*
 * EnergyTask.cpp
 *
 *  Created on: Jan 30, 2015
 *      Author: rlcevg
 */

#include "task/builder/EnergyTask.h"
#include "task/TaskManager.h"
#include "module/EconomyManager.h"
#include "resource/GridLink.h"
#include "terrain/TerrainManager.h"
#include "unit/CircuitUnit.h"
#include "spring/SpringMap.h"
#include "CircuitAI.h"
#include "util/Utils.h"

namespace circuit {

using namespace springai;

CBEnergyTask::CBEnergyTask(ITaskManager* mgr, Priority priority,
						   CCircuitDef* buildDef, const AIFloat3& position,
						   float cost, float shake, int timeout)
		: IBuilderTask(mgr, priority, buildDef, position, Type::BUILDER, BuildType::ENERGY, cost, shake, timeout)
		, isLinkSite(false)
		, link(nullptr)
{
}

CBEnergyTask::~CBEnergyTask()
{
}

void CBEnergyTask::Update()
{
	IBuilderTask::Update();
	if (units.empty() || (target == nullptr)) {
		return;
	}

	// Same rule as when the job was queued (CEconomyManager::GetEnergyPriority). Nothing else changes an energy
	// job's priority; ChangePriority only sends ZK a command when its build priority changes
	const Priority wanted = manager->GetCircuit()->GetEconomyManager()->GetEnergyPriority(buildDef);
	if (wanted != priority) {
		ChangePriority(wanted);
	}
}

void CBEnergyTask::Finish()
{
	ReleaseLink();
	IBuilderTask::Finish();
}

void CBEnergyTask::Cancel()
{
	ReleaseLink();
	IBuilderTask::Cancel();
}

void CBEnergyTask::SetLinkSite(const CEnergyGrid::SLinkSite& site, IGridLink* gap)
{
	ReleaseLink();
	linkSite = site;
	isLinkSite = true;
	link = gap;
	if (link != nullptr) {
		link->SetBeingBuilt(true);
	}
}

void CBEnergyTask::ReleaseLink()
{
	if (link != nullptr) {
		link->SetBeingBuilt(false);
		link = nullptr;
	}
}

void CBEnergyTask::Execute(CCircuitUnit* unit)
{
	CCircuitAI* circuit = manager->GetCircuit();
	if ((target == nullptr) && isLinkSite
		&& (!utils::is_valid(buildPos) || !circuit->GetMap()->IsPossibleToBuildAt(buildDef->GetDef(), buildPos, facing)))
	{
		FindBuildSite(unit, position, 0.f);  // its site was taken: another that keeps the links
		if (!utils::is_valid(buildPos)) {
			manager->FallbackTask(unit);  // drops the job (and the gap it held): it is planned afresh
			return;
		}
	}
	if ((target != nullptr) || !circuit->GetEconomyManager()->IsGeoDef(buildDef)) {
		IBuilderTask::Execute(unit);
		return;
	}

	// Geothermal: must stay exactly on a vent, never search around the position
	if (!utils::is_valid(buildPos) || !circuit->GetMap()->IsPossibleToBuildAt(buildDef->GetDef(), buildPos, facing)) {
		SetBuildPos(-RgtVector);
		const AIFloat3 pos = circuit->GetEconomyManager()->FindGeoSpot(buildDef, position, unit);
		if (!utils::is_valid(pos)) {
			// Fallback to Guard/Assist/Patrol
			manager->FallbackTask(unit);
			return;
		}
		SetBuildPos(pos);
	}
	IBuilderTask::Execute(unit);  // buildPos is a free vent: builds there
}

void CBEnergyTask::FindBuildSite(CCircuitUnit* builder, const AIFloat3& pos, float searchRadius)
{
	if (!isLinkSite) {
		IBuilderTask::FindBuildSite(builder, pos, searchRadius);
		return;
	}
	CCircuitAI* circuit = manager->GetCircuit();
	CEconomyManager* economyMgr = circuit->GetEconomyManager();
	CEnergyGrid* energyGrid = economyMgr->GetEnergyGrid();
	energyGrid->SetAuthority(circuit);
	SetBuildPos(-RgtVector);  // its old site's blocker off
	SetBuildPos(energyGrid->FindLinkSite(buildDef, linkSite, linkSite.fromReach * 0.5f,
										 economyMgr->GetFillPredicate(buildDef, builder)));
}

} // namespace circuit

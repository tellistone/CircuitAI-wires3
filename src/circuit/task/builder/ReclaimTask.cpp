/*
 * ReclaimTask.cpp
 *
 *  Created on: Jan 31, 2015
 *      Author: rlcevg
 */

#include "task/builder/ReclaimTask.h"
#include "task/TaskManager.h"
#include "module/EconomyManager.h"
#include "terrain/TerrainManager.h"
#include "unit/action/TravelAction.h"
#include "CircuitAI.h"
#include "util/Utils.h"

#include "spring/SpringCallback.h"

#include "AISCommands.h"

namespace circuit {

using namespace springai;

CBReclaimTask::CBReclaimTask(ITaskManager* mgr, Priority priority,
							 const AIFloat3& position,
							 float cost, int timeout, float radius, bool isMetal)
		: IReclaimTask(mgr, priority, Type::BUILDER, position, cost, timeout, radius, isMetal)
{
}

CBReclaimTask::CBReclaimTask(ITaskManager* mgr, Priority priority,
							 CCircuitUnit* target,
							 int timeout)
		: IReclaimTask(mgr, priority, Type::BUILDER, target, timeout)
{
}

CBReclaimTask::~CBReclaimTask()
{
}

void CBReclaimTask::AssignTo(CCircuitUnit* unit)
{
	IBuilderTask::AssignTo(unit);

	lastTouched = manager->GetCircuit()->GetLastFrame();
}

bool CBReclaimTask::CanAssignTo(CCircuitUnit* unit) const
{
	// Takes helpers while its metal is above <helper_sec> seconds of the build power already on it
	return unit->GetCircuitDef()->IsAbleToReclaim()
			&& (cost > buildPower * manager->GetCircuit()->GetEconomyManager()->GetReclaimHelperSec());
}

bool CBReclaimTask::Reevaluate(CCircuitUnit* unit)
{
	if (!isMetal) {
		return true;
	}

	CCircuitAI* circuit = manager->GetCircuit();
	// NOTE: Full metal storage doesn't abort a running job, it only stops new ones (CEconomyManager::UpdateReclaimTasks)
	if (!units.empty()) {
		/*
		 * Update reclaim position
		 */
		const int frame = circuit->GetLastFrame();
		const AIFloat3& pos = unit->GetPos(frame);
		auto enemies = circuit->GetCallback()->GetEnemyUnitsIn(pos, 500.0f);
		if (!enemies.empty()) {
			for (Unit* enemy : enemies) {
				if ((enemy != nullptr) && enemy->IsBeingBuilt()) {
					TRY_UNIT(circuit, unit,
						unit->GetUnit()->ReclaimUnit(enemy, UNIT_CMD_OPTION, frame + FRAMES_PER_SEC * 60);
					)
					utils::free_clear(enemies);
					return false;
				}
			}
			utils::free_clear(enemies);
		}

		// Best-ranked safe reclaimable feature nearby
		CEconomyManager* economyMgr = circuit->GetEconomyManager();
		const auto features = economyMgr->GetReclaimFeatures(circuit->GetCallback()->GetFeatureIdsIn(pos, 500.0f));
		const auto best = economyMgr->FindReclaimFeature(pos, features,
				[economyMgr, unit](const CEconomyManager::SReclaimFeature& f) { return economyMgr->IsReclaimSafe(unit, f.pos); });
		if (best != nullptr) {
			position = best->pos;
		}
	}

	return true;
}

} // namespace circuit

/*
 * RetreatTask.cpp
 *
 *  Created on: Jan 18, 2015
 *      Author: rlcevg
 */

#include "task/RetreatTask.h"
#include "task/TaskManager.h"
#include "map/ThreatMap.h"
#include "module/BuilderManager.h"
#include "module/FactoryManager.h"
#include "setup/SetupManager.h"
#include "terrain/path/PathFinder.h"
#include "terrain/path/QueryPathSingle.h"
#include "terrain/path/QueryCostMap.h"
#include "terrain/TerrainManager.h"
#include "unit/action/DGunAction.h"
#include "unit/action/MoveAction.h"
#include "unit/action/FightAction.h"
#include "CircuitAI.h"
#include "util/Utils.h"

#include "AISCommands.h"

namespace circuit {

using namespace springai;

CRetreatTask::CRetreatTask(ITaskManager* mgr, int timeout)
		: IUnitTask(mgr, Priority::NORMAL, Type::RETREAT, timeout)
		, repairer(nullptr)
		, regroupFrame(-1)
		, lastHavenSqDist(-1.f)
		, fleeGoal(-RgtVector)
		, fleeFrame(-1)
{
}

CRetreatTask::~CRetreatTask()
{
}

void CRetreatTask::ClearRelease()
{
	costQuery = nullptr;
	IUnitTask::ClearRelease();
}

void CRetreatTask::AssignTo(CCircuitUnit* unit)
{
	IUnitTask::AssignTo(unit);

	if (unit->HasDGun()) {
		unit->PushDGunAct(new CDGunAction(unit, unit->GetDGunRange() * 0.8f));
	}

	CCircuitAI* circuit = manager->GetCircuit();
	CCircuitDef* cdef = unit->GetCircuitDef();
	TRY_UNIT(circuit, unit,
		unit->GetUnit()->SetFireState(cdef->IsAttrRetHold() ? CCircuitDef::FireType::HOLD : CCircuitDef::FireType::OPEN);
	)
	if (cdef->IsAttrBoost()) {
		unit->SetTaskFrame(circuit->GetLastFrame());  // avoid UnitIdle on find_pad
		const int frame = circuit->GetLastFrame() + FRAMES_PER_SEC * 60;
		TRY_UNIT(circuit, unit,
			if (cdef->IsPlane()) {
				unit->CmdFindPad(frame);
			}
			unit->CmdManualFire(UNIT_COMMAND_OPTION_ALT_KEY, frame);
		)
		return;
	}

	PushTravelAct(unit);

	// A capturer sent home to hide ("capture_hide") at full health needs no repair. A fleeing commander gets none
	// either: a builder would walk into the danger to reach it. StopFlee queues it once the flee turns into a retreat
	if (!IsFlee() && !(unit->IsCaptureHide() && (unit->GetHealthPercent() > 0.98f))) {
		EnqueueRepair(unit);
	}
}

void CRetreatTask::PushTravelAct(CCircuitUnit* unit)
{
	CCircuitDef* cdef = unit->GetCircuitDef();
	const int squareSize = manager->GetCircuit()->GetPathfinder()->GetSquareSize();
	if (cdef->IsAttrRetFight()) {
		unit->PushTravelAct(new CFightAction(unit, squareSize));
	} else {
		unit->PushTravelAct(new CMoveAction(unit, squareSize));
	}
	unit->SetAllowedToJump(cdef->IsAbleToJump() && !cdef->IsAttrNoJump());
}

void CRetreatTask::EnqueueRepair(CCircuitUnit* unit)
{
	// Mobile repair; planes repair on air pads
	if (!unit->GetCircuitDef()->IsPlane()) {
		manager->GetCircuit()->GetBuilderManager()->EnqueueRepair(IBuilderTask::Priority::HIGH, unit);
	}
}

AIFloat3 CRetreatTask::GetHaven(CCircuitUnit* unit) const
{
	CCircuitAI* circuit = manager->GetCircuit();
	const AIFloat3 haven = circuit->GetFactoryManager()->GetClosestHaven(unit);
	return utils::is_valid(haven) ? haven : circuit->GetSetupManager()->GetBasePos();
}

float CRetreatTask::GetHavenRange() const
{
	// Well inside a caretaker's build range
	CCircuitAI* circuit = manager->GetCircuit();
	return circuit->GetFactoryManager()->GetAssistDef()->GetBuildDistance() * 0.6f + circuit->GetPathfinder()->GetSquareSize();
}

void CRetreatTask::RemoveAssignee(CCircuitUnit* unit)
{
	IUnitTask::RemoveAssignee(unit);
	if (units.empty()) {
		manager->DoneTask(this);
	}

	TRY_UNIT(manager->GetCircuit(), unit,
		unit->GetUnit()->SetFireState(unit->GetCircuitDef()->GetFireState());
	)
}

void CRetreatTask::Start(CCircuitUnit* unit)
{
	if ((unit->GetTravelAct() == nullptr) || unit->GetTravelAct()->IsFinished()) {
		return;
	}

	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	CPathFinder* pathfinder = circuit->GetPathfinder();
	const AIFloat3& startPos = unit->GetPos(frame);
	AIFloat3 endPos;
	float range;

	if (repairer != nullptr) {
		endPos = repairer->GetPos(frame);
		range = pathfinder->GetSquareSize();
	} else if (IsFlee()) {
		endPos = fleeGoal;
		range = pathfinder->GetSquareSize() * 2;
	} else {
		endPos = GetHaven(unit);
		range = GetHavenRange();
	}

	if (unit->GetTravelAct()->GetPath() == nullptr) {
		std::shared_ptr<PathInfo> pPath = std::make_shared<PathInfo>();
		pPath->posPath.push_back(endPos);
		unit->GetTravelAct()->SetPath(pPath);
	}

	if (!IsQueryReady(unit)) {
		return;
	}

//	const float minThreat = circuit->GetThreatMap()->GetUnitThreat(unit) * 0.125f;
	std::shared_ptr<IPathQuery> query = pathfinder->CreatePathSingleQuery(
			unit, circuit->GetThreatMap(), frame,
			startPos, endPos, range/*, nullptr, minThreat*/);
	pathQueries[unit] = query;
	query->HoldTask(this);

	pathfinder->RunQuery(query, [this](const IPathQuery* query) {
		if (this->IsQueryAlive(query)) {
			this->ApplyPath(static_cast<const CQueryPathSingle*>(query));
		}
	});
}

void CRetreatTask::Update()
{
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	bool isExecute = (++updCount % 2 == 0);
	const int waitTimeout = circuit->GetSetupManager()->GetRetreatWait();
	auto assignees = units;
	for (CCircuitUnit* unit : assignees) {
		if (IsFlee()) {  // ended by CBuilderManager::UpdateCommFlee, not by health
			if (unit->IsForceUpdate(frame) || isExecute) {
				Start(unit);
			}
			continue;
		}
		const float healthPerc = unit->GetHealthPercent();
		bool isRepaired = unit->HasShield()
				? (healthPerc > 0.98f) && unit->IsShieldCharged(circuit->GetSetupManager()->GetFullShield())
				: healthPerc > 0.98f;
		// Waited at haven too long without a repairer: return to duty.
		// A builder working on it restarts the wait, so the timeout never cuts a repair short.
		bool isWaitOver = false;
		if ((waitTimeout > 0) && (State::REGROUP == state) && (repairer == nullptr) && !unit->GetCircuitDef()->IsPlane()) {
			if (circuit->GetBuilderManager()->IsRepairAssigned(unit->GetId())) {
				regroupFrame = frame;
			} else {
				isWaitOver = (frame - regroupFrame >= waitTimeout);
			}
		}

		// "capture_hide": stays at the haven while its captured units are worth too much to risk it
		if ((isRepaired || isWaitOver) && !unit->IsDisarmed(frame) && !unit->IsCaptureHide()) {
			RemoveAssignee(unit);
			circuit->GetBuilderManager()->OnRetreatEnd(unit);
		} else if (unit->IsForceUpdate(frame) || isExecute) {
			Start(unit);
		}
	}
}

void CRetreatTask::Finish()
{
	Cancel();
}

void CRetreatTask::Cancel()
{
	if (repairer != nullptr) {
		IUnitTask* repairerTask = repairer->GetTask();
		repairerTask->GetManager()->AbortTask(repairerTask);
	}
}

void CRetreatTask::OnUnitIdle(CCircuitUnit* unit)
{
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();

	CCircuitDef* cdef = unit->GetCircuitDef();
	if (cdef->IsPlane()) {
		// NOTE: unit considered idle after boost and find_pad
		if (State::REGROUP == state) {
			state = State::ROAM;
			return;
		}
		if (unit->GetTravelAct() != nullptr) {
			unit->GetTravelAct()->StateFinish();
		}

		unit->SetTaskFrame(frame);  // avoid UnitIdle on find_pad
		TRY_UNIT(circuit, unit,
			unit->CmdFindPad(frame + FRAMES_PER_SEC * 60);
		)
		state = State::REGROUP;
		return;
	}

	const bool isFleeGoal = (repairer == nullptr) && IsFlee();
	const AIFloat3 haven = (repairer != nullptr) ? repairer->GetPos(frame) : isFleeGoal ? fleeGoal : GetHaven(unit);

	// Stop well inside the repairer's build range (same margin as the retreat path), so the wait spot
	// can't end up on the edge or outside of it
	const float repairDist = (repairer != nullptr)
			? repairer->GetCircuitDef()->GetBuildDistance()
			: isFleeGoal ? (float)(circuit->GetPathfinder()->GetSquareSize() * 2)
			: circuit->GetFactoryManager()->GetAssistDef()->GetBuildDistance();
	const float maxDist = repairDist * 0.6f + cdef->GetRadius();  // big units can't get their center any closer
	const AIFloat3& unitPos = unit->GetPos(frame);
	const float sqDist = unitPos.SqDistance2D(haven);
	// Crowded haven: accept a spot still in full range once the last move got it no closer
	const bool isStuckInRange = (sqDist <= SQUARE(repairDist)) && (lastHavenSqDist >= 0.f)
			&& (sqDist > lastHavenSqDist - SQUARE(SQUARE_SIZE * 2));
	lastHavenSqDist = sqDist;
	if ((sqDist > maxDist * maxDist) && !isStuckInRange) {
		// TODO: push MoveAction into unit? to avoid enemy fire
		TRY_UNIT(circuit, unit,
			unit->CmdMoveTo(haven, UNIT_COMMAND_OPTION_RIGHT_MOUSE_KEY, frame + FRAMES_PER_SEC * 1);
		)
		// TODO: Add fail counter?
	} else {
		// TODO: push WaitAction into unit
		// Patrol on the spot: never goes idle and still returns fire, but holds still so repairers
		// (nano towers, builders) keep it in range instead of losing it on a back-and-forth loop
		TRY_UNIT(circuit, unit,
//			unit->CmdPriority(0);
			unit->GetUnit()->PatrolTo(unitPos);
		)

		if (unit->GetTravelAct() != nullptr) {
			unit->GetTravelAct()->StateFinish();
		}
		if (State::REGROUP != state) {
			regroupFrame = frame;
		}
		state = State::REGROUP;
		lastHavenSqDist = -1.f;
	}
}

void CRetreatTask::OnUnitDamaged(CCircuitUnit* unit, CEnemyInfo* attacker)
{
	if (unit->IsCaptureHide()) {
		EnqueueRepair(unit);  // skipped on AssignTo while it was unhurt
	}
	if (State::REGROUP != state) {
		return;
	}
	state = State::ROAM;

	if (unit->GetTravelAct() == nullptr) {
		PushTravelAct(unit);  // IsAttrBoost units don't get one on AssignTo
	}
	unit->GetTravelAct()->StateActivate();

	Start(unit);
}

void CRetreatTask::Redirect(CCircuitUnit* unit, const AIFloat3& goal)
{
	fleeGoal = goal;
	state = State::ROAM;
	lastHavenSqDist = -1.f;
	if (unit->GetTravelAct() != nullptr) {
		unit->GetTravelAct()->StateActivate();
	}
	Start(unit);
}

void CRetreatTask::StopFlee(CCircuitUnit* unit)
{
	fleeFrame = -1;
	EnqueueRepair(unit);  // skipped while fleeing
}

void CRetreatTask::EndFlee(CCircuitUnit* unit)
{
	fleeFrame = -1;
	RemoveAssignee(unit);
	manager->GetCircuit()->GetBuilderManager()->OnRetreatEnd(unit);
}

void CRetreatTask::OnUnitDestroyed(CCircuitUnit* unit, CEnemyInfo* attacker)
{
	RemoveAssignee(unit);
}

void CRetreatTask::CheckRepairer(CCircuitUnit* newRep)
{
	if (IsFlee()) {
		return;  // a commander flee heads for its flee goal: a builder on an older repair job doesn't redirect it
	}
	CCircuitUnit* unit = *units.begin();

	if ((costQuery != nullptr) && (costQuery->GetState() != IPathQuery::State::READY)) {  // not ready
		return;
	}

	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	const AIFloat3& startPos = unit->GetPos(frame);

	CPathFinder* pathfinder = circuit->GetPathfinder();
	costQuery = pathfinder->CreateCostMapQuery(
			unit, circuit->GetThreatMap(), frame, startPos);
	costQuery->HoldTask(this);

	CCircuitUnit::Id newRepId = newRep->GetId();
	pathfinder->RunQuery(costQuery, [this, newRepId](const IPathQuery* query) {
		CCircuitUnit* newRep = this->ValidateNewRepairer(query, newRepId);
		if (newRep != nullptr) {
			this->ApplyCostMap(static_cast<const CQueryCostMap*>(query), newRep);
		}
	});
}

void CRetreatTask::ApplyPath(const CQueryPathSingle* query)
{
	const std::shared_ptr<PathInfo>& pPath = query->GetPathInfo();
	CCircuitUnit* unit = query->GetUnit();

	if (pPath->posPath.empty()) {
		pPath->posPath.push_back(query->GetEndPos());
	}
	unit->GetTravelAct()->SetPath(pPath);
}

CCircuitUnit* CRetreatTask::ValidateNewRepairer(const IPathQuery* query, int newRepId) const
{
	if (isDead || (costQuery == nullptr) || (costQuery->GetId() != query->GetId())) {
		return nullptr;
	}
	CCircuitUnit* newRep = manager->GetCircuit()->GetTeamUnit(newRepId);
	if (newRep == nullptr) {
		return nullptr;
	}
	if (newRep->GetTask()->GetType() != IUnitTask::Type::BUILDER) {
		return nullptr;
	}
	IBuilderTask* taskB = static_cast<IBuilderTask*>(newRep->GetTask());
	if ((taskB->GetBuildType() != IBuilderTask::BuildType::REPAIR) || (taskB->GetTarget() != query->GetUnit())) {
		return nullptr;
	}
	return newRep;
}

void CRetreatTask::ApplyCostMap(const CQueryCostMap* query, CCircuitUnit* newRep)
{
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	CPathFinder* pathfinder = circuit->GetPathfinder();
	CCircuitUnit* unit = query->GetUnit();
	AIFloat3 endPos;
	float range;

	bool isRepairer = (repairer != nullptr);
	if (isRepairer) {
		endPos = repairer->GetPos(frame);
		range = pathfinder->GetSquareSize();
	} else {
		endPos = GetHaven(unit);
		range = GetHavenRange();
	}

	float prevCost = query->GetCostAt(endPos, range);
	if (isRepairer && repairer->GetCircuitDef()->IsMobile()) {
		prevCost /= 2;
	}

	endPos = unit->GetPos(frame);
	float nextCost = query->GetCostAt(endPos, range);
	if (unit->GetCircuitDef()->IsMobile()) {
		nextCost /= 2;
	}

	if (prevCost > nextCost) {
		SetRepairer(newRep);
	}
}

} // namespace circuit

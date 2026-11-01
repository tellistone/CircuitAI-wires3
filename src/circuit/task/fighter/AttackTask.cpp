/*
 * AttackTask.cpp
 *
 *  Created on: Jan 28, 2015
 *      Author: rlcevg
 */

#include "task/fighter/AttackTask.h"
#include "task/TaskManager.h"
#include "map/InfluenceMap.h"
#include "map/ThreatMap.h"
#include "module/FactoryManager.h"
#include "module/MilitaryManager.h"
#include "setup/SetupManager.h"
#include "terrain/TerrainManager.h"
#include "terrain/path/PathFinder.h"
#include "terrain/path/QueryPathSingle.h"
#include "terrain/path/QueryPathMulti.h"
#include "terrain/path/QueryLineMap.h"
#include "unit/action/FightAction.h"
#include "unit/action/MoveAction.h"
#include "unit/action/SupportAction.h"
#include "unit/enemy/EnemyUnit.h"
#include "unit/CircuitUnit.h"
#include "CircuitAI.h"
#include "util/Utils.h"

#include "spring/SpringMap.h"

#include <algorithm>
#include <cmath>

#include "AISCommands.h"

namespace circuit {

using namespace springai;

CAttackTask::CAttackTask(ITaskManager* mgr, float minPower, float powerMod)
		: ISquadTask(mgr, FightType::ATTACK, powerMod)
		, minPower(minPower)
		, reinforce(nullptr)
		, reinforceFails(0)
		, reinforceRetryFrame(-1)
		, fleeFrame(0)
		, fleePathFrame(0)
		, pullFrame(0)
		, isHolding(false)
		, stepFrame(0)
		, hitPos(-RgtVector)
		, hitRadius(0.f)
		, hitFrame(-1)
		, scaleRefSqDist(0.f)
		, anchorTargetId(-1)
		, corneredFrame(-1)
		, corneredPos(-RgtVector)
		, isCorneredHold(false)
		, fleeGiveUpDest(-RgtVector)
		, pickSqDist(0.f)
{
	CCircuitAI* circuit = manager->GetCircuit();
	float x = rand() % circuit->GetTerrainManager()->GetTerrainWidth();
	float z = rand() % circuit->GetTerrainManager()->GetTerrainHeight();
	position = AIFloat3(x, circuit->GetMap()->GetElevationAt(x, z), z);
}

CAttackTask::~CAttackTask()
{
}

bool CAttackTask::CanAssignTo(CCircuitUnit* unit) const
{
	assert(leader != nullptr);

	const int frame = manager->GetCircuit()->GetLastFrame();
	if (leader->GetPos(frame).SqDistance2D(unit->GetPos(frame)) > SQUARE(1000.f)) {
		return false;
	}
	return IsCompatible(unit);
}

bool CAttackTask::IsCompatible(CCircuitUnit* unit) const
{
	assert(leader != nullptr);

	float speedLeader = leader->GetCircuitDef()->GetSpeed();
	float speedUnit = unit->GetCircuitDef()->GetSpeed();
	if (speedLeader > speedUnit) {
		std::swap(speedLeader, speedUnit);
	}
	if (speedLeader * 1.5f < speedUnit) {
		return false;
	}

	// The joining group's leader must not move worse than ours (smaller terrain area). Otherwise it
	// would take the lead (ISquadTask::FindLeader) and drag the group back to it, or, if kept out of
	// the lead, get stranded on routes only our leader can take.
	const STerrainMapArea* ourArea = leader->GetArea();
	const STerrainMapArea* newArea = unit->GetArea();
	if ((newArea != nullptr) && ((ourArea == nullptr) || (newArea->percentOfMap < ourArea->percentOfMap))) {
		return false;
	}
	if (unit->GetCircuitDef()->IsAmphibious()
		&& (leader->GetCircuitDef()->IsAmphibious()
			|| leader->GetCircuitDef()->IsLander()
			|| leader->GetCircuitDef()->IsFloater()))
	{
		return true;
	}
	if ((leader->GetCircuitDef()->IsAbleToFly() && unit->GetCircuitDef()->IsAbleToFly())
		|| (leader->GetCircuitDef()->IsLander() && unit->GetCircuitDef()->IsLander())
		|| (leader->GetCircuitDef()->IsFloater() && unit->GetCircuitDef()->IsFloater()))
	{
		return true;
	}
	return false;
}

void CAttackTask::AssignTo(CCircuitUnit* unit)
{
	ISquadTask::AssignTo(unit);
	CCircuitDef* cdef = unit->GetCircuitDef();
	highestRange = std::max(highestRange, cdef->GetLosRadius());

	if (cdef->IsRoleSupport()) {
		unit->PushBack(new CSupportAction(unit));
	}

	int squareSize = manager->GetCircuit()->GetPathfinder()->GetSquareSize();
	ITravelAction* travelAction;
	if (cdef->IsAttrSiege()) {
		travelAction = new CFightAction(unit, squareSize);
	} else {
		travelAction = new CMoveAction(unit, squareSize);
	}
	unit->PushTravelAct(travelAction);
	travelAction->StateWait();
	unit->SetAllowedToJump(cdef->IsAbleToJump() && cdef->IsAttrJump());
}

void CAttackTask::RemoveAssignee(CCircuitUnit* unit)
{
	ISquadTask::RemoveAssignee(unit);
	if (leader == nullptr) {
		manager->AbortTask(this);
		return;
	}
	highestRange = std::max(highestRange, leader->GetCircuitDef()->GetLosRadius());
	// Under quota.attack after a loss the group stays together (it used to break up and walk home
	// to gather); Update() looks for a stronger group to reinforce while it is not engaged.
}

void CAttackTask::Start(CCircuitUnit* unit)
{
	if ((State::REGROUP == state) || (State::ENGAGE == state)) {
		return;
	}
	if (!pPath->posPath.empty()) {
		unit->GetTravelAct()->SetPath(pPath, lowestSpeed);
		unit->GetTravelAct()->StateActivate();
	}
}

void CAttackTask::Update()
{
	++updCount;

	/*
	 * Merge tasks if possible
	 */
	ISquadTask* task = GetAttackMergeTask();
	if (task != nullptr) {
		task->Merge(this);
		units.clear();
		// TODO: Deal with cowards?
		manager->AbortTask(this);
		return;
	}
	AbsorbWeakerTasks();

	/*
	 * Escape: checked on every update (not just every decision), also while engaging or reinforcing
	 */
	{
		const int frame = manager->GetCircuit()->GetLastFrame();
		bool isPathUnsafe = false;
		// Not while defending: a fight with a target inside the base defence radius is held (no flee, no pull-back)
		const bool isDanger = IsInDanger(frame, isPathUnsafe);
		const bool isHolding = IsHoldingGround(frame) || (isDanger && isCorneredHold);
		const bool isFleeing = isDanger && !isHolding && Flee(frame);
		if (isDanger && !isFleeing) {
			// Cornered (holding ground, or nowhere better to run): don't stand and take it.
			// Moving to a quieter spot ends ENGAGE, so remember that this group is holding ground.
			isCorneredHold = isHolding;
			Cornered(frame);
			return;
		}
		corneredPos = -RgtVector;
		isCorneredHold = false;
		fleeGiveUpDest = -RgtVector;
		if (!isFleeing && !isHolding) {
			PullBackEndangered(frame);
		}
		if (isFleeing || (frame < fleeFrame)) {
			// keep running along the escape path for a moment instead of turning straight back;
			// still send units with empty shields to retreat (normally done at each decision below)
			if (updCount % 4 == 2) {
				ISquadTask::Update();
			}
			return;
		}
		if (isPathUnsafe && (frame >= lastTouched + FRAMES_PER_SEC)) {
			lastTouched = frame;  // at most one forced re-plan per second
			for (CCircuitUnit* unit : units) {
				unit->ForceUpdate(frame);  // re-plan now instead of walking into it
			}
		}
	}

	/*
	 * Regroup if required
	 */
	bool wasRegroup = (State::REGROUP == state);
	bool mustRegroup = IsMustRegroup();
	if (State::REGROUP == state) {
		if (mustRegroup) {
			CCircuitAI* circuit = manager->GetCircuit();
			int frame = circuit->GetLastFrame() + FRAMES_PER_SEC * 60;
			for (CCircuitUnit* unit : units) {
				unit->Gather(groupPos, frame);
				unit->GetTravelAct()->StateWait();
			}
		}
		return;
	}

	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	bool isExecute = (updCount % 4 == 2);
	if (!isExecute) {
		for (CCircuitUnit* unit : units) {
			isExecute |= unit->IsForceUpdate(frame);
		}
		if (!isExecute) {
			if (wasRegroup && !pPath->posPath.empty()) {
				ActivePath(lowestSpeed);
			}
			return;
		}
	} else {
		ISquadTask::Update();
		if (leader == nullptr) {  // task aborted
			return;
		}
	}
	isHolding = false;  // re-set by HoldPosition if this decision finds nothing to go to

	/*
	 * Reinforce: a group under quota.attack walks to the nearest stronger group instead of hunting alone.
	 * A small group that isn't fighting looks for one every decision (~4 s), unless it gave up recently.
	 */
	if (attackPower >= minPower) {
		reinforce = nullptr;  // an army itself (grown by merges)
	} else if ((reinforce == nullptr) && (State::ENGAGE != state) && (frame >= reinforceRetryFrame)) {
		reinforce = static_cast<CMilitaryManager*>(manager)->FindReinforceSquad(leader, attackPower, this);
	}
	if ((reinforce != nullptr) && (GetReinforceSquad() != nullptr)) {
		// Fight back on the way: a target that passes the usual checks (FindTarget: threat at its spot
		// below our power) and is already within range gets attacked; otherwise keep walking.
		FindTarget();
		if ((target != nullptr) && (target->GetPos().SqDistance2D(leader->GetPos(frame)) < SQUARE(highestRange))) {
			state = State::ENGAGE;
			Attack(frame);
			return;
		}
		scaleRefSqDist = 0.f;  // walking to a group, not into an accepted fight
		Reinforce(frame);
		return;
	}  // else: nothing to join, carry on as a small attack group

	const AIFloat3& startPos = leader->GetPos(frame);
	if (circuit->GetInflMap()->GetInfluenceAt(startPos) < -INFL_EPS) {
		SetTarget(nullptr);
		scaleRefSqDist = 0.f;
	} else {
		FindTarget();
	}

	state = State::ROAM;
	if (target != nullptr) {
		if (position.SqDistance2D(startPos) < SQUARE(highestRange)) {
			state = State::ENGAGE;
			Attack(frame);
			return;
		}
	}

	if (!IsQueryReady(leader)) {
		return;
	}

	if (target == nullptr) {
		FallbackFrontPos();
		return;
	}

	// Route-aware pick: one capped route to all safe candidates; whichever it reaches first becomes the target
	CPathFinder* pathfinder = circuit->GetPathfinder();
	const float eps = pathfinder->GetSquareSize();
	const float pathRange = std::max(highestRange - eps, eps);

	std::shared_ptr<IPathQuery> query = pathfinder->CreatePathMultiQuery(
			leader, circuit->GetThreatMap(), frame,
			startPos, pathRange, candidatePos, GetHitTest(), GetPathThreatCap(startPos, candidatePos));
	pathQueries[leader] = query;
	query->HoldTask(this);

	// The route reports back later; a later FindTarget may have rewritten the candidates by then
	pathfinder->RunQuery(query, [this, ids = candidateIds](const IPathQuery* query) {
		if (this->IsQueryAlive(query)) {
			this->ApplyTargetPath(static_cast<const CQueryPathMulti*>(query), ids);
		}
	});
}

ISquadTask* CAttackTask::GetAttackMergeTask()
{
	// Groups under quota.attack look for a group to join every 4 updates, others every 32.
	// Unlike other squads, an attack group may merge while its leader stands on enemy-held ground;
	// the straight line between the leaders must still be safe (CheckMergeTask).
	const int interval = (attackPower < minPower) ? 4 : 32;
	return (updCount % interval == 1) ? CheckMergeTask() : nullptr;
}

void CAttackTask::AbsorbWeakerTasks()
{
	// A group at or above quota.attack pulls in attack groups under quota.attack near it, every 8 updates.
	// Same rules as joining (CanAssignTo: speed within 1.5x, leaders within 1000 elmos, movement type,
	// never a worse mover than our leader; reachable; safe straight line), seen from the small group's side,
	// but at most 30 s of travel so absorbed units arrive well inside the 60 s regroup "stuck" limit.
	if ((attackPower < minPower) || (updCount % 8 != 5)) {
		return;
	}

	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	const AIFloat3& pos = leader->GetPos(frame);
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	CPathFinder* pathfinder = circuit->GetPathfinder();

	static std::vector<CAttackTask*> weaklings;  // NOTE: micro-opt
	const std::set<IFighterTask*>& tasks = static_cast<CMilitaryManager*>(manager)->GetTasks(fightType);
	for (IFighterTask* t : tasks) {
		CAttackTask* candy = static_cast<CAttackTask*>(t);
		CCircuitUnit* candyLeader = candy->GetLeader();
		if ((candy == this) || candy->IsDead() || (candyLeader == nullptr)
			|| (candy->GetAttackPower() >= minPower)
			|| !CanAssignTo(candyLeader))
		{
			continue;
		}
		const AIFloat3& candyPos = candyLeader->GetPos(frame);
		if (!terrainMgr->CanMoveToPos(candyLeader->GetArea(), pos)) {
			continue;
		}
		if (candyPos.SqDistance2D(pos) >= SQUARE(MAX_TRAVEL_SEC / 2 * candy->lowestSpeed)) {
			continue;
		}
		std::shared_ptr<CQueryLineMap> query = std::static_pointer_cast<CQueryLineMap>(
				pathfinder->CreateLineMapQuery(candyLeader, circuit->GetThreatMap(), frame));
		if (!query->IsSafeLine(candyPos, pos)) {
			continue;
		}
		weaklings.push_back(candy);
	}

	// Merge after the loop: aborting a task removes it from the set being iterated
	for (CAttackTask* candy : weaklings) {
		Merge(candy);  // CanAssignTo ruled out units that would take the lead
		candy->units.clear();
		manager->AbortTask(candy);
	}
	weaklings.clear();
}

CAttackTask* CAttackTask::GetReinforceSquad()
{
	CMilitaryManager* militaryMgr = static_cast<CMilitaryManager*>(manager);
	// NOTE: the army may be deleted already, do not dereference before lookup
	const std::set<IFighterTask*>& tasks = militaryMgr->GetTasks(fightType);
	if ((tasks.find(static_cast<IFighterTask*>(reinforce)) != tasks.end()) && !reinforce->IsDead()
		&& (reinforce->GetLeader() != nullptr) && (reinforce->GetAttackPower() > attackPower))
	{
		return reinforce;
	}
	// Group gone or no longer stronger: pick the nearest other stronger one
	reinforce = militaryMgr->FindReinforceSquad(leader, attackPower, this);
	return reinforce;
}

void CAttackTask::Reinforce(const int frame)
{
	SetTarget(nullptr);
	state = State::ROAM;

	if (!IsQueryReady(leader)) {
		return;
	}
	// Threat-aware path to the group's leader; cells with threat above attackPower (no thr_mod: a
	// reinforcing group is meant to avoid fights), or GetPathThreatCap if lower, are blocked. It only has to get within 800 elmos, so the hot zone around
	// a fighting group doesn't block it; merging takes over inside 1000 elmos (CheckMergeTask every
	// 4 updates from our side, AbsorbWeakerTasks every 8 from an army's side).
	CCircuitAI* circuit = manager->GetCircuit();
	const AIFloat3& startPos = leader->GetPos(frame);
	position = reinforce->GetLeaderPos(frame);  // validated by GetReinforceSquad this update
	const float pathRange = 800.f;

	CPathFinder* pathfinder = circuit->GetPathfinder();
	std::shared_ptr<IPathQuery> query = pathfinder->CreatePathSingleQuery(
			leader, circuit->GetThreatMap(), frame,
			startPos, position, pathRange, nullptr, std::min(attackPower, GetPathThreatCap(startPos, {position})));
	pathQueries[leader] = query;
	query->HoldTask(this);

	pathfinder->RunQuery(query, [this](const IPathQuery* query) {
		if (this->IsQueryAlive(query)) {
			this->ApplyReinforcePath(static_cast<const CQueryPathSingle*>(query));
		}
	});
}

void CAttackTask::ApplyReinforcePath(const CQueryPathSingle* query)
{
	pPath = query->GetPathInfo();

	if (!pPath->path.empty()) {
		reinforceFails = 0;
		if (pPath->path.size() > 2) {
			ActivePath(lowestSpeed);
		}
	} else if (++reinforceFails >= 3) {
		// No safe route 3 times in a row (~12 s): act as a small attack group for 30 s, then look again
		// (merge checks and absorbing still apply meanwhile)
		reinforce = nullptr;
		reinforceFails = 0;
		reinforceRetryFrame = manager->GetCircuit()->GetLastFrame() + FRAMES_PER_SEC * 30;
	}
}

void CAttackTask::OnUnitIdle(CCircuitUnit* unit)
{
	ISquadTask::OnUnitIdle(unit);
	if (units.empty()) {
		return;
	}

	CCircuitAI* circuit = manager->GetCircuit();
	const float maxDist = std::max<float>(lowestRange, circuit->GetPathfinder()->GetSquareSize());
	if (position.SqDistance2D(leader->GetPos(circuit->GetLastFrame())) < SQUARE(maxDist)) {
		CTerrainManager* terrainMgr = circuit->GetTerrainManager();
		float x = rand() % terrainMgr->GetTerrainWidth();
		float z = rand() % terrainMgr->GetTerrainHeight();
		position = AIFloat3(x, circuit->GetMap()->GetElevationAt(x, z), z);
		position = terrainMgr->GetMovePosition(leader->GetArea(), position);
	}

	if (units.find(unit) != units.end()) {
		Start(unit);  // NOTE: Not sure if it has effect
	}
}

void CAttackTask::FindTarget()
{
	CCircuitAI* circuit = manager->GetCircuit();
	CMap* map = circuit->GetMap();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	CThreatMap* threatMap = circuit->GetThreatMap();
	const AIFloat3& basePos = circuit->GetSetupManager()->GetBasePos();
	const AIFloat3& pos = leader->GetPos(circuit->GetLastFrame());
	STerrainMapArea* area = leader->GetArea();
	CCircuitDef* cdef = leader->GetCircuitDef();
	const bool isAntiStatic = cdef->IsAttrAntiStat();
	const bool notAW = !cdef->HasAntiWater();
	const bool notAA = !cdef->HasAntiAir();
	const float maxSpeed = SQUARE(highestSpeed * 0.8f / FRAMES_PER_SEC);
	// Current-health strength (same measure the escape uses, so we don't pick fights we'd flee from at once),
	// a bit lower for 4 s after an escape; divided by the thr_mod.distance curve at each judged spot
	const int frame = circuit->GetLastFrame();
	const bool isCautious = (frame < fleeFrame + FRAMES_PER_SEC * 4);
	const float maxPower = GetSafeThreat() * (isCautious ? 0.75f : 1.f);
	const float weaponRange = cdef->GetMaxRange();
	const int canTargetCat = cdef->GetTargetCategory();
	const int noChaseCat = cdef->GetNoChaseCategory();

	const float sqOBDist = pos.SqDistance2D(basePos);  // Own to Base distance
	// Near-base scale reference: the enemy we already went for keeps the anchor from when it was first picked
	// (otherwise the discount would fade as we close in, and the escape would flee the fight we accepted);
	// every other enemy is judged from where we stand now
	const ICoreUnit::Id prevAnchorId = (scaleRefSqDist > 0.f) ? anchorTargetId : -1;
	const float prevRef = scaleRefSqDist;
	auto anchorFor = [&](CEnemyInfo* e) {
		return ((prevAnchorId >= 0) && (e->GetId() == prevAnchorId)) ? prevRef : sqOBDist;
	};
	pickSqDist = sqOBDist;
	std::vector<std::pair<float, CEnemyInfo*>> candidates;  // (scaled distance, enemy)

	SetTarget(nullptr);  // make adequate enemy->GetTasks().size()
	threatMap->SetThreatType(leader);
	const CCircuitAI::EnemyInfos& enemies = circuit->GetEnemyInfos();
	for (auto& kv : enemies) {
		CEnemyInfo* enemy = kv.second;
		if (enemy->IsHidden() || (enemy->GetTasks().size() > 2)) {
			continue;
		}
		const AIFloat3& ePos = enemy->GetPos();
		const float sqBEDist = ePos.SqDistance2D(basePos);  // Base to Enemy distance
		const float scale = std::min(sqBEDist / sqOBDist, 1.f);  // ordering
		scaleRefSqDist = anchorFor(enemy);
		if ((threatMap->GetThreatAt(ePos) >= GetAllowedThreatAt(ePos, maxPower))
			|| !terrainMgr->CanMobileReachAt(area, ePos, highestRange))
		{
			continue;
		}
		const AIFloat3& eVel = enemy->GetVel();
		if ((eVel.SqLength2D() >= maxSpeed) && (eVel.dot2D(pos - ePos) < 0)) {
			continue;
		}

		CCircuitDef* edef = enemy->GetCircuitDef();
		if (edef != nullptr) {
			if ((isAntiStatic && edef->IsMobile())
				|| ((edef->GetCategory() & canTargetCat) == 0)
				|| ((edef->GetCategory() & noChaseCat) != 0)
				|| (edef->IsAbleToFly() && notAA))
			{
				continue;
			}
			float elevation = map->GetElevationAt(ePos.x, ePos.z);
			if ((notAW && !edef->IsYTargetable(elevation, ePos.y))
				|| (ePos.y - elevation > weaponRange)
				/*|| enemy->IsBeingBuilt()*/)
			{
				continue;
			}
		} else {
			if (notAW && (ePos.y < -SQUARE_SIZE * 5)) {
				continue;
			}
		}

		const float sqOEDist = pos.SqDistance2D(ePos) * scale;  // Own to Enemy distance
		candidates.emplace_back(sqOEDist, enemy);
	}

	// Nearest candidate whose approach is also safe: the target's own cell can be quiet while the
	// spot we'd shoot it from sits under a big enemy group's guns
	std::sort(candidates.begin(), candidates.end(),
			[](const std::pair<float, CEnemyInfo*>& a, const std::pair<float, CEnemyInfo*>& b) { return a.first < b.first; });
	// Keep the nearest 16 safe ones as goals of one route query (Update); the nearest is the target until
	// the route says which one we can actually reach under the threat cap
	candidateIds.clear();
	candidatePos.clear();
	CEnemyInfo* bestTarget = nullptr;
	for (const auto& c : candidates) {
		scaleRefSqDist = anchorFor(c.second);
		if (IsApproachSafe(c.second->GetPos(), pos, maxPower)) {
			if (bestTarget == nullptr) {
				bestTarget = c.second;
			}
			candidateIds.push_back(c.second->GetId());
			candidatePos.push_back(c.second->GetPos());
			if (candidateIds.size() >= 16) {
				break;
			}
		}
	}

	if (bestTarget != nullptr) {
		SetTarget(bestTarget);
		position = target->GetPos();
		// Keep the scale only for a fight towards home (target nearer our base than the reference):
		// an outward fight never relied on it, so stragglers behind the group get no discount
		const float ref = anchorFor(bestTarget);
		const bool isHomeward = bestTarget->GetPos().SqDistance2D(basePos) < ref;
		scaleRefSqDist = isHomeward ? ref : 0.f;
		anchorTargetId = isHomeward ? bestTarget->GetId() : -1;
	} else {
		scaleRefSqDist = 0.f;  // no fight accepted: no near-base discount
		anchorTargetId = -1;
	}
	// Return: target, startPos=leader->pos, endPos=position
}

bool CAttackTask::IsApproachSafe(const AIFloat3& ePos, const AIFloat3& pos, float safeThreat) const
{
	// Walk from the target towards us, out to our range (or to us if closer), one threat square at a time,
	// judging each spot with the common allowance.
	// Expects threatMap->SetThreatType(leader) already done (FindTarget).
	CThreatMap* threatMap = manager->GetCircuit()->GetThreatMap();
	AIFloat3 dir = pos - ePos;
	dir.y = 0.f;
	const float dist = dir.Length2D();
	if (dist < 1.f) {
		return true;
	}
	dir /= dist;
	const float reach = std::min(highestRange, dist);
	const float step = threatMap->GetSquareSize();
	for (float d = step; d <= reach; d += step) {
		AIFloat3 p = ePos + dir * d;
		CTerrainManager::CorrectPosition(p);
		if (threatMap->GetThreatAt(p) >= GetAllowedThreatAt(p, safeThreat)) {
			return false;
		}
	}
	return true;
}

bool CAttackTask::IsHoldingGround(int frame) const
{
	if ((State::ENGAGE != state) || (target == nullptr)) {
		return false;
	}
	// Only inside the base defence radius (where thr_mod.base_bonus applies): out in the field the enemy
	// is nearer our base than we are in about half of all fights, and those should still be escapable
	CCircuitAI* circuit = manager->GetCircuit();
	const float baseRange = circuit->GetMilitaryManager()->GetBaseDefRange();
	const AIFloat3& basePos = circuit->GetSetupManager()->GetBasePos();
	return (baseRange > 0.f) && (target->GetPos().SqDistance2D(basePos) < SQUARE(baseRange));
}

float CAttackTask::GetPathThreatCap(const AIFloat3& startPos, const F3Vec& goals) const
{
	// One cap for the whole route (the pathfinder takes a single value): the common allowance with the
	// strictest thr_mod.distance value of start and goals, but the loosest base_bonus and near-base scale,
	// so a target that passed picking thanks to them stays reachable. Spots on the way that are hotter than
	// their own allowance are caught by the path-ahead check and the escape; threat is also a route cost,
	// so the route avoids them where it can.
	CCircuitAI* circuit = manager->GetCircuit();
	CMilitaryManager* militaryMgr = circuit->GetMilitaryManager();
	const AIFloat3& basePos = circuit->GetSetupManager()->GetBasePos();
	float curve = GetThrDistModAt(startPos);
	float bonus = militaryMgr->GetBaseThrBonus(basePos.SqDistance2D(startPos));
	float scale = GetNearBaseScaleAt(startPos);
	for (const AIFloat3& g : goals) {
		curve = std::max(curve, GetThrDistModAt(g));
		bonus = std::max(bonus, militaryMgr->GetBaseThrBonus(basePos.SqDistance2D(g)));
		scale = std::min(scale, GetNearBaseScaleAt(g));
	}
	return GetSafeThreat() * bonus / curve / std::max(scale, 1e-3f);
}

float CAttackTask::GetSafeThreat() const
{
	// Like attackPower (def power incl. pwr_mod) but scaled by current health, times the task's powerMod
	float power = 0.f;
	for (CCircuitUnit* unit : units) {
		power += GetUnitPower(unit);
	}
	return std::max(power, THREAT_MIN) * powerMod;  // the thr_mod.distance curve is applied per judged spot
}

float CAttackTask::GetUnitPower(CCircuitUnit* unit) const
{
	return unit->GetCircuitDef()->GetPower() * sqrtf(std::max(unit->GetHealthPercent(), 0.f));
}

float CAttackTask::GetNearBaseScaleAt(const AIFloat3& pos) const
{
	// Threat at spots nearer our base than the group was when it last picked a target counts less
	// (quadratic, as FindTarget always did). Fixed at pick time, so a group closing in on an enemy
	// between it and our base keeps judging that fight the way it accepted it.
	if (scaleRefSqDist <= 0.f) {
		return 1.f;
	}
	CCircuitAI* circuit = manager->GetCircuit();
	const AIFloat3& basePos = circuit->GetSetupManager()->GetBasePos();
	// Floor (quota.thr_mod.scale_min): caps the near-base boost, so groups near base still flee a far bigger army
	const float scale = std::min(pos.SqDistance2D(basePos) / scaleRefSqDist, 1.f);
	return std::max(scale, circuit->GetMilitaryManager()->GetThrScaleMin());
}

float CAttackTask::GetAllowedThreatAt(const AIFloat3& pos, float strength) const
{
	CCircuitAI* circuit = manager->GetCircuit();
	const AIFloat3& basePos = circuit->GetSetupManager()->GetBasePos();
	const float bonus = circuit->GetMilitaryManager()->GetBaseThrBonus(basePos.SqDistance2D(pos));
	return strength * bonus / GetThrDistModAt(pos) / std::max(GetNearBaseScaleAt(pos), 1e-3f);
}

bool CAttackTask::IsInDanger(int frame, bool& isPathUnsafe)
{
	isPathUnsafe = false;
	endangered.clear();
	CThreatMap* threatMap = manager->GetCircuit()->GetThreatMap();
	const float safeThreat = GetSafeThreat();
	float totalPower = 0.f;
	float dangerPower = 0.f;
	for (CCircuitUnit* unit : units) {
		const float power = GetUnitPower(unit);
		totalPower += power;
		if (unit->GetUnit()->IsCloaked()) {
			continue;  // unseen: enemies in range can't shoot it
		}
		const AIFloat3& pos = unit->GetPos(frame);
		if (threatMap->GetThreatAt(unit, pos) >= GetAllowedThreatAt(pos, safeThreat)) {
			endangered.push_back(unit);
			dangerPower += power;
		}
	}
	// Flee only when the units in danger hold at least quota.attack_flee_share of the group's current strength
	const float fleeShare = static_cast<CMilitaryManager*>(manager)->GetAttackFleeShare();
	if (!endangered.empty() && (dangerPower >= fleeShare * totalPower)) {
		return true;
	}

	if (!leader->GetTravelAct()->IsActive() || leader->GetUnit()->IsCloaked()) {
		return false;
	}
	const std::shared_ptr<PathInfo>& path = leader->GetTravelAct()->GetPath();
	if (path == nullptr) {
		return false;
	}
	// The stretch of path the group covers in the next ~2s
	const AIFloat3& pos = leader->GetPos(frame);
	const float lookAhead = std::max(leader->GetCircuitDef()->GetSpeed() * 2.f, 200.f);
	const float sqLookAhead = SQUARE(lookAhead);
	for (const AIFloat3& p : path->posPath) {
		if ((p.SqDistance2D(pos) < sqLookAhead) && (threatMap->GetThreatAt(leader, p) >= GetAllowedThreatAt(p, safeThreat))) {
			isPathUnsafe = true;
			break;
		}
	}
	return false;
}

void CAttackTask::PullBackEndangered(int frame)
{
	// Units in danger that didn't make the whole group flee: send them to the strength-weighted centre
	// of the units that are safe
	if (endangered.empty() || (frame < pullFrame + FRAMES_PER_SEC)) {
		return;
	}
	AIFloat3 safePos(ZeroVector);
	float safePower = 0.f;
	for (CCircuitUnit* unit : units) {
		if (std::find(endangered.begin(), endangered.end(), unit) != endangered.end()) {
			continue;
		}
		const float power = std::max(GetUnitPower(unit), 1e-3f);
		safePos += unit->GetPos(frame) * power;
		safePower += power;
	}
	if (safePower <= 0.f) {
		return;
	}
	safePos /= safePower;
	CTerrainManager::CorrectPosition(safePos);
	pullFrame = frame;

	CCircuitAI* circuit = manager->GetCircuit();
	for (CCircuitUnit* unit : endangered) {
		if (unit->Blocker() != nullptr) {
			continue;
		}
		auto it = riskySpotFrames.find(unit);
		if ((it != riskySpotFrames.end()) && (frame < it->second + FRAMES_PER_SEC * 3)) {
			continue;  // just sent to the least dangerous firing spot: don't bounce it back
		}
		unit->GetTravelAct()->StateWait();  // re-activated with the group's next path
		TRY_UNIT(circuit, unit,
			unit->CmdMoveTo(safePos, UNIT_COMMAND_OPTION_RIGHT_MOUSE_KEY, frame + FRAMES_PER_SEC * 60);
			unit->CmdWantedSpeed(NO_SPEED_LIMIT);
		)
	}
}

bool CAttackTask::Flee(int frame)
{
	// Run to the nearest stronger attack group (fight beside it, then merge), else the closest haven, else base
	CCircuitAI* circuit = manager->GetCircuit();
	AIFloat3 dest = -RgtVector;
	CAttackTask* squad = static_cast<CMilitaryManager*>(manager)->FindReinforceSquad(leader, attackPower, this);
	if (squad != nullptr) {
		// Only if the two groups together can stand there: it may be in this same fight, or past the enemy
		const AIFloat3& squadPos = squad->GetLeaderPos(frame);
		const float together = GetSafeThreat() + squad->GetSafeThreat();
		if (circuit->GetThreatMap()->GetThreatAt(leader, squadPos) < GetAllowedThreatAt(squadPos, together)) {
			dest = squadPos;
		}
	}
	if (!utils::is_valid(dest)) {
		dest = circuit->GetFactoryManager()->GetClosestHaven(leader);
	}
	if (!utils::is_valid(dest)) {
		dest = circuit->GetSetupManager()->GetBasePos();
	}
	if (leader->GetPos(frame).SqDistance2D(dest) < SQUARE(400.f)) {
		fleeGiveUpDest = dest;  // the cornered rule stays within 400 elmos of it
		fleeFrame = std::min(fleeFrame, frame);  // stop running, but keep the cautious window after it
		return false;  // already there: running gets us nowhere
	}

	SetTarget(nullptr);
	state = State::ROAM;
	isHolding = false;
	scaleRefSqDist = 0.f;  // running: judge danger at full value, also on the way home
	fleeGiveUpDest = -RgtVector;
	const bool wasFleeing = (frame < fleeFrame + FRAMES_PER_SEC);  // grace: still in the same escape
	fleeFrame = frame + FRAMES_PER_SEC * 2;
	if (wasFleeing && (frame < fleePathFrame + FRAMES_PER_SEC)) {
		return true;  // escape path is fresh
	}
	fleePathFrame = frame;

	// Break off at once; the threat-aware path replaces this when it is ready
	if (!wasFleeing) {
		for (CCircuitUnit* unit : units) {
			if (unit->Blocker() != nullptr) {
				continue;
			}
			unit->GetTravelAct()->StateWait();
			TRY_UNIT(circuit, unit,
				unit->CmdMoveTo(dest, UNIT_COMMAND_OPTION_RIGHT_MOUSE_KEY, frame + FRAMES_PER_SEC * 60);
				unit->CmdWantedSpeed(NO_SPEED_LIMIT);
			)
		}
	}

	CPathFinder* pathfinder = circuit->GetPathfinder();
	std::shared_ptr<IPathQuery> query = pathfinder->CreatePathSingleQuery(
			leader, circuit->GetThreatMap(), frame,
			leader->GetPos(frame), dest, pathfinder->GetSquareSize());  // threat as cost, no cap: least-threat way out
	pathQueries[leader] = query;  // supersedes any pending target/reinforce path
	query->HoldTask(this);

	pathfinder->RunQuery(query, [this](const IPathQuery* query) {
		if (this->IsQueryAlive(query)) {
			this->ApplyFleePath(static_cast<const CQueryPathSingle*>(query));
		}
	});
	return true;
}

void CAttackTask::ApplyFleePath(const CQueryPathSingle* query)
{
	pPath = query->GetPathInfo();
	if (pPath->posPath.empty()) {
		return;  // keep the straight move from Flee()
	}
	ActivePath();
}

void CAttackTask::ApplyTargetPath(const CQueryPathMulti* query, const std::vector<ICoreUnit::Id>& ids)
{
	pPath = query->GetPathInfo();

	if (pPath->posPath.empty()) {
		// No candidate reachable within our threat cap: drop the target and head for the front
		SetTarget(nullptr);
		scaleRefSqDist = 0.f;
		FallbackFrontPos();
		return;
	}

	// The route reached the candidate it ends within range of (nearest first in the list, as FindTarget
	// sorted it); if none is within range (moved, corrected), the one it ends nearest to
	const AIFloat3& endPos = pPath->posPath.back();
	const F3Vec& goals = query->GetTargets();
	CCircuitAI* circuit = manager->GetCircuit();
	const float sqInRange = SQUARE(query->GetMaxRange() + circuit->GetPathfinder()->GetSquareSize());
	CEnemyInfo* reached = nullptr;
	CEnemyInfo* nearest = nullptr;
	float minSqDist = std::numeric_limits<float>::max();
	for (size_t i = 0; (i < goals.size()) && (i < ids.size()); ++i) {
		CEnemyInfo* enemy = circuit->GetEnemyInfo(ids[i]);  // may have died since
		if (enemy == nullptr) {
			continue;
		}
		const float sqDist = goals[i].SqDistance2D(endPos);
		if (sqDist <= sqInRange) {
			reached = enemy;
			break;
		}
		if (sqDist < minSqDist) {
			minSqDist = sqDist;
			nearest = enemy;
		}
	}
	if (reached == nullptr) {
		reached = nearest;
	}
	if (reached != nullptr) {
		if (reached->GetId() != anchorTargetId) {
			// The route led to another candidate: it was judged from where the group stood at pick time
			const AIFloat3& basePos = circuit->GetSetupManager()->GetBasePos();
			const bool isHomeward = reached->GetPos().SqDistance2D(basePos) < pickSqDist;
			scaleRefSqDist = isHomeward ? pickSqDist : 0.f;
			anchorTargetId = isHomeward ? reached->GetId() : -1;
		}
		SetTarget(reached);
		position = reached->GetPos();
	}
	ActivePath(lowestSpeed);
}

void CAttackTask::FallbackFrontPos()
{
	CCircuitAI* circuit = manager->GetCircuit();
	circuit->GetMilitaryManager()->FillFrontPos(leader, urgentPositions);
	if (urgentPositions.empty()) {
		HoldPosition();
		return;
	}

	const int frame = circuit->GetLastFrame();
	const AIFloat3& startPos = leader->GetPos(frame);
	const float pathRange = DEFAULT_SLACK * 4;

	CPathFinder* pathfinder = circuit->GetPathfinder();
	std::shared_ptr<IPathQuery> query = pathfinder->CreatePathMultiQuery(
			leader, circuit->GetThreatMap(), frame,
			startPos, pathRange, urgentPositions, nullptr, GetPathThreatCap(startPos, urgentPositions));
	pathQueries[leader] = query;
	query->HoldTask(this);

	pathfinder->RunQuery(query, [this](const IPathQuery* query) {
		if (this->IsQueryAlive(query)) {
			this->ApplyFrontPos(static_cast<const CQueryPathMulti*>(query));
		}
	});
}

void CAttackTask::ApplyFrontPos(const CQueryPathMulti* query)
{
	pPath = query->GetPathInfo();

	if (!pPath->path.empty()) {
		if (pPath->path.size() > 2) {
			ActivePath();
		}
	} else {
		HoldPosition();
	}
}

void CAttackTask::HoldPosition()
{
	// Never head home from the front with nothing to do: hold at the quietest spot nearby
	AIFloat3 bestPos;
	FindQuietSpot(leader->GetPos(manager->GetCircuit()->GetLastFrame()), bestPos, -RgtVector, 0.f);
	MoveGroupTo(bestPos);
	isHolding = true;
}

float CAttackTask::FindQuietSpot(const AIFloat3& pos, AIFloat3& outPos, const AIFloat3& anchor, float anchorRadius)
{
	// Quietest reachable spot within 2x our range: stay put when it is as quiet as anywhere around
	// (no creeping), and keep away from where we were last hit while holding (attackers we can't see
	// or reach leave no threat on the map)
	CCircuitAI* circuit = manager->GetCircuit();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	CThreatMap* threatMap = circuit->GetThreatMap();
	const int frame = circuit->GetLastFrame();
	STerrainMapArea* area = leader->GetArea();
	const bool isAvoidHit = (frame < hitFrame + FRAMES_PER_SEC * 30);
	auto isHitZone = [&](const AIFloat3& p) {
		return isAvoidHit && (p.SqDistance2D(hitPos) < SQUARE(hitRadius));
	};

	AIFloat3 bestPos = pos;
	float bestThreat = isHitZone(pos) ? std::numeric_limits<float>::max() : threatMap->GetThreatAt(leader, pos);
	float bestSqDist = 0.f;
	const float maxDist = std::max(highestRange * 2.f, 400.f);
	for (int ring = 1; ring <= 3; ++ring) {
		const float dist = maxDist * ring / 3.f;
		for (int a = 0; a < 8; ++a) {
			const float angle = a * static_cast<float>(M_PI) / 4.f;
			AIFloat3 p(pos.x + dist * cosf(angle), pos.y, pos.z + dist * sinf(angle));
			CTerrainManager::CorrectPosition(p);
			if (isHitZone(p) || !terrainMgr->CanMoveToPos(area, p)
				|| (utils::is_valid(anchor) && (p.SqDistance2D(anchor) > SQUARE(anchorRadius))))
			{
				continue;
			}
			const float threat = threatMap->GetThreatAt(leader, p);
			const float sqDist = p.SqDistance2D(pos);
			// Clearly quieter wins; equally quiet: the nearer one (so a quiet spot keeps us where we are)
			if ((threat < bestThreat - THREAT_MIN * 0.1f)
				|| ((threat < bestThreat + THREAT_MIN * 0.1f) && (sqDist < bestSqDist)))
			{
				bestPos = p;
				bestThreat = threat;
				bestSqDist = sqDist;
			}
		}
	}
	outPos = bestPos;
	return (bestThreat < std::numeric_limits<float>::max()) ? bestThreat : threatMap->GetThreatAt(leader, pos);
}

void CAttackTask::Cornered(int frame)
{
	if (frame < corneredFrame + FRAMES_PER_SEC) {
		return;  // keep the current move / attack for a second
	}
	corneredFrame = frame;
	isHolding = false;
	CCircuitAI* circuit = manager->GetCircuit();
	CThreatMap* threatMap = circuit->GetThreatMap();
	const AIFloat3& pos = leader->GetPos(frame);
	const float hereThreat = threatMap->GetThreatAt(leader, pos);

	AIFloat3 quietPos;
	// Flee gave up next to a haven / stronger group: stay within 400 elmos of it, or the next update
	// would flee back to it (holding ground: no flee, so no limit)
	const AIFloat3 anchor = isCorneredHold ? AIFloat3(-RgtVector) : fleeGiveUpDest;
	const float quietThreat = FindQuietSpot(pos, quietPos, anchor, 400.f);
	// Already heading somewhere: switch only to a clearly (20%) quieter spot
	const bool isMoving = utils::is_valid(corneredPos) && (corneredPos.SqDistance2D(pos) > SQUARE(200.f));
	const float refThreat = isMoving ? threatMap->GetThreatAt(leader, corneredPos) : hereThreat;
	if ((quietPos.SqDistance2D(pos) > SQUARE(200.f)) && (quietThreat < refThreat * 0.8f)) {
		corneredPos = quietPos;
		SetTarget(nullptr);
		state = State::ROAM;
		CPathFinder* pathfinder = circuit->GetPathfinder();
		std::shared_ptr<IPathQuery> query = pathfinder->CreatePathSingleQuery(
				leader, threatMap, frame, pos, quietPos, pathfinder->GetSquareSize());  // threat as cost, no cap
		pathQueries[leader] = query;
		query->HoldTask(this);
		pathfinder->RunQuery(query, [this](const IPathQuery* query) {
			if (this->IsQueryAlive(query)) {
				this->ApplyCorneredPath(static_cast<const CQueryPathSingle*>(query));
			}
		});
		return;
	}
	if (isMoving) {
		return;  // still on the way to a spot that is quiet enough
	}
	corneredPos = -RgtVector;

	// Nowhere quieter: fight from here
	CEnemyInfo* enemy = ((target != nullptr) && (target->GetPos().SqDistance2D(pos) < SQUARE(highestRange)))
			? target : FindNearestHittable(frame);
	if (enemy != nullptr) {
		SetTarget(enemy);
		position = enemy->GetPos();
		state = State::ENGAGE;
		Attack(frame);
	}
}

void CAttackTask::ApplyCorneredPath(const CQueryPathSingle* query)
{
	pPath = query->GetPathInfo();
	if (pPath->posPath.empty()) {
		MoveGroupTo(corneredPos);
		return;
	}
	ActivePath();
}

CEnemyInfo* CAttackTask::FindNearestHittable(int frame) const
{
	CCircuitAI* circuit = manager->GetCircuit();
	CCircuitDef* cdef = leader->GetCircuitDef();
	const AIFloat3& pos = leader->GetPos(frame);
	const int canTargetCat = cdef->GetTargetCategory();
	const bool notAA = !cdef->HasAntiAir();
	const float maxSqDist = SQUARE(std::max(highestRange, cdef->GetMaxRange()) * 1.5f);
	CEnemyInfo* best = nullptr;
	float minSqDist = maxSqDist;
	for (auto& kv : circuit->GetEnemyInfos()) {
		CEnemyInfo* enemy = kv.second;
		if (enemy->IsHidden()) {
			continue;
		}
		CCircuitDef* edef = enemy->GetCircuitDef();
		if ((edef != nullptr)
			&& (((edef->GetCategory() & canTargetCat) == 0) || (edef->IsAbleToFly() && notAA)))
		{
			continue;
		}
		const float sqDist = enemy->GetPos().SqDistance2D(pos);
		if (sqDist < minSqDist) {
			minSqDist = sqDist;
			best = enemy;
		}
	}
	return best;
}

void CAttackTask::MoveGroupTo(const AIFloat3& pos)
{
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	pPath = std::make_shared<PathInfo>();
	for (CCircuitUnit* unit : units) {
		if (unit->Blocker() != nullptr) {
			continue;
		}
		unit->GetTravelAct()->StateWait();
		TRY_UNIT(circuit, unit,
			unit->CmdMoveTo(pos, UNIT_COMMAND_OPTION_RIGHT_MOUSE_KEY, frame + FRAMES_PER_SEC * 60);
			unit->CmdWantedSpeed(NO_SPEED_LIMIT);
		)
	}
}

void CAttackTask::OnUnitDamaged(CCircuitUnit* unit, CEnemyInfo* attacker)
{
	IFighterTask::OnUnitDamaged(unit, attacker);  // may send the unit to retreat
	if (!isHolding || (leader == nullptr) || (units.find(unit) == units.end())) {
		return;
	}
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	if (frame < stepFrame + FRAMES_PER_SEC) {
		return;
	}
	stepFrame = frame;

	// Hit while holding by something we didn't go for (not visible, or no safe route to it):
	// step out of its range, away from it; unknown attacker: avoid this spot and pick the next quietest
	const AIFloat3& uPos = unit->GetPos(frame);
	if ((attacker != nullptr) && !attacker->IsHidden()) {
		const AIFloat3& aPos = attacker->GetPos();
		CCircuitDef* edef = attacker->GetCircuitDef();
		const float range = (edef != nullptr) ? edef->GetMaxRange() : uPos.distance2D(aPos);
		hitPos = aPos;
		hitRadius = range + 200.f;
		hitFrame = frame;
		AIFloat3 dir = leader->GetPos(frame) - aPos;
		dir.y = 0.f;
		const float len = dir.Length2D();
		if (len > 1.f) {
			AIFloat3 p = aPos + dir / len * hitRadius;
			CTerrainManager::CorrectPosition(p);
			if (circuit->GetTerrainManager()->CanMoveToPos(leader->GetArea(), p)) {
				MoveGroupTo(p);
				return;
			}
		}
	} else {
		hitPos = uPos;
		hitRadius = std::max(highestRange, 300.f);
		hitFrame = frame;
	}
	HoldPosition();
}

} // namespace circuit

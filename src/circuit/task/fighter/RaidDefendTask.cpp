/*
 * RaidDefendTask.cpp
 *
 *  Created on: Nov 27, 2026
 *      Author: Claude
 */

#include "task/fighter/RaidDefendTask.h"
#include "map/ThreatMap.h"
#include "module/MilitaryManager.h"
#include "module/RaidDefence.h"
#include "terrain/TerrainManager.h"
#include "terrain/path/PathFinder.h"
#include "terrain/path/QueryPathSingle.h"
#include "unit/action/TravelAction.h"
#include "unit/enemy/EnemyUnit.h"
#include "unit/CircuitUnit.h"
#include "CircuitAI.h"
#include "util/Utils.h"

#include "AISCommands.h"

namespace circuit {

using namespace springai;

CRaidDefendTask::CRaidDefendTask(ITaskManager* mgr, float powerMod)
		: CRaidTask(mgr, FightType::RAID_DEFEND, 0.f, powerMod)
		, slotId(-1)
		, isParked(false)
		, parkPos(-RgtVector)
		, parkVipId(-1)
		, parkFrame(-FRAMES_PER_SEC * 60)
		, parkCount(0)
{
}

CRaidDefendTask::~CRaidDefendTask()
{
}

bool CRaidDefendTask::CanAssignTo(CCircuitUnit* unit) const
{
	const int frame = manager->GetCircuit()->GetLastFrame();
	if (leader->GetPos(frame).SqDistance2D(unit->GetPos(frame)) > SQUARE(1000.f)) {
		return false;
	}
	CMilitaryManager* militaryMgr = GetMilitaryMgr();
	if (!militaryMgr->GetRaidDefence()->IsRole(unit->GetCircuitDef()) || militaryMgr->IsScouting(unit)
		|| !IsMoveCompatible(unit, militaryMgr->GetAttackMerge().speedRatio) || !IsSpeedSpreadOk(unit))
	{
		return false;
	}
	const IUnitTask* task = unit->GetTask();
	if ((task != nullptr) && (task->GetType() == IUnitTask::Type::FIGHTER)
		&& (static_cast<const IFighterTask*>(task)->GetFightType() == FightType::RAID_DEFEND))
	{
		// A merge: two groups of one slot become one, whatever its room
		const CRaidDefendTask* group = static_cast<const CRaidDefendTask*>(task);
		return (group->GetLeader() == unit) && (group->GetSlot() == slotId);
	}
	return HasRoom();
}

void CRaidDefendTask::Start(CCircuitUnit* unit)
{
	if (isParked && (State::ROAM == state)) {
		ParkUnit(unit, manager->GetCircuit()->GetLastFrame());  // the group waits: so does it, no old route
		return;
	}
	CRaidTask::Start(unit);
}

void CRaidDefendTask::OnUnitIdle(CCircuitUnit* unit)
{
	ISquadTask::OnUnitIdle(unit);  // not the raid's: no random destination
	if (units.find(unit) != units.end()) {
		Start(unit);
	}
}

void CRaidDefendTask::SetSlot(int id)
{
	if (id == slotId) {
		return;
	}
	slotId = id;
	isParked = false;
	// Decide at the next update, not a second later
	lastTouched = std::min(lastTouched, manager->GetCircuit()->GetLastFrame() - FRAMES_PER_SEC);
}

bool CRaidDefendTask::HasRoom() const
{
	return GetMilitaryMgr()->GetRaidDefence()->HasRoom(slotId);
}

bool CRaidDefendTask::IsTargetAllowed(const CEnemyInfo* enemy, bool isZone, bool isCurrent) const
{
	if (isZone) {
		return true;  // quota.protect: committed to its ward's fight
	}
	const AIFloat3& ePos = enemy->GetPos();
	const float contact = leader->GetCircuitDef()->GetMaxRange() + CONTACT_SLACK;
	if (leader->GetPos(manager->GetCircuit()->GetLastFrame()).SqDistance2D(ePos) <= SQUARE(contact)) {
		return true;  // in contact: it fights back wherever it is
	}
	const CRaidDefence* raidDefence = GetMilitaryMgr()->GetRaidDefence();
	const float reactSec = raidDefence->GetConfig().reactSec;
	const float sec = raidDefence->GetSlotSec(slotId, leader->GetCircuitDef()->GetMobileId(), ePos, lowestSpeed);
	if ((sec < 0.f) || (sec > (isCurrent ? TARGET_KEEP * reactSec : reactSec))) {
		return false;
	}
	// The current target on any ground: a hurt raider running home isn't let go at the territory line
	return isCurrent || raidDefence->IsOurGround(ePos);
}

bool CRaidDefendTask::IsPreferredTarget(const CEnemyInfo* enemy, const CCircuitDef* edef) const
{
	return CRaidTask::IsPreferredTarget(enemy, edef)
			|| GetMilitaryMgr()->GetRaidDefence()->IsRecentAttacker(enemy->GetId(), manager->GetCircuit()->GetLastFrame());
}

float CRaidDefendTask::GetThrDistModAt(const AIFloat3& pos) const
{
	return GetMilitaryMgr()->GetThrDistMod(FightType::RAID, pos);  // the raid's curve (quota.thr_mod.distance.raid)
}

void CRaidDefendTask::OnNoTarget(bool isContestedAllowed)
{
	if (IsProtecting()) {
		isParked = false;
		FallbackRaid(false);  // quota.protect: committed with nothing to hunt, head for the ward's goal as a raid does
		return;
	}
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	const AIFloat3& pos = leader->GetPos(frame);
	const CRaidDefence::SSlot* slot = GetMilitaryMgr()->GetRaidDefence()->FindSlot(slotId);
	CCircuitUnit* vip = ((slot != nullptr) && slot->isEscort) ? circuit->GetTeamUnit(slot->builderId) : nullptr;
	// No slot yet: wait where it is (CRaidDefence gives it one, or releases it, within a second)
	AIFloat3 dest = (vip != nullptr) ? vip->GetPos(frame) : (slot != nullptr) ? slot->pos : pos;
	dest = circuit->GetTerrainManager()->GetMovePosition(leader->GetArea(), dest);
	if (pos.SqDistance2D(dest) <= SQUARE(PARK_RADIUS)) {
		Park(frame, dest, vip);
	} else {
		WalkTo(frame, dest);
	}
}

void CRaidDefendTask::Park(int frame, const AIFloat3& dest, CCircuitUnit* vip)
{
	// Orders again only when something changed: the spot or the constructor, a fight or an escape since, a merge,
	// or now and then
	const ICoreUnit::Id vipId = (vip != nullptr) ? vip->GetId() : -1;
	if (isParked && (vipId == parkVipId) && (dest.SqDistance2D(parkPos) < SQUARE(REPATH_DIST))
		&& (engageFrame < parkFrame) && (fleeFrame <= parkFrame) && (units.size() == parkCount)
		&& (frame < parkFrame + PARK_ORDER_FRAMES))
	{
		return;
	}
	isParked = true;
	parkPos = dest;
	parkVipId = vipId;
	parkFrame = frame;
	parkCount = units.size();
	position = dest;
	isFightPending = false;
	roamPath = nullptr;
	pPath = std::make_shared<PathInfo>();  // nothing to walk: a regroup or Start doesn't send an old route again
	for (CCircuitUnit* unit : units) {
		if ((unit->Blocker() == nullptr) && !IsScooting(unit)) {
			ParkUnit(unit, frame);
		}
	}
}

void CRaidDefendTask::ParkUnit(CCircuitUnit* unit, int frame)
{
	CCircuitAI* circuit = manager->GetCircuit();
	CCircuitUnit* vip = (parkVipId >= 0) ? circuit->GetTeamUnit(parkVipId) : nullptr;
	unit->GetTravelAct()->StateWait();
	TRY_UNIT(circuit, unit,
		if (vip != nullptr) {
			unit->GetUnit()->Guard(vip->GetUnit());  // follows the constructor, shoots what hits it
		} else {
			unit->CmdMoveTo(parkPos, UNIT_COMMAND_OPTION_RIGHT_MOUSE_KEY, frame + FRAMES_PER_SEC * 60);
			unit->GetUnit()->PatrolTo(utils::get_radial_pos(parkPos, SQUARE_SIZE * 8),
					UNIT_COMMAND_OPTION_RIGHT_MOUSE_KEY | UNIT_COMMAND_OPTION_SHIFT_KEY, frame + FRAMES_PER_SEC * 60);
		}
	)
}

void CRaidDefendTask::WalkTo(int frame, const AIFloat3& dest)
{
	isParked = false;
	position = dest;
	// The route asked for a moment ago, or the one it walks with nothing unsafe on it, still goes (nearly) here
	const bool isSameGoal = utils::is_valid(roamGoal) && (dest.SqDistance2D(roamGoal) < SQUARE(REPATH_DIST));
	const bool isWalking = (roamPath != nullptr) && leader->GetTravelAct()->IsActive()
			&& (leader->GetTravelAct()->GetPath() == roamPath);
	if (isSameGoal && ((isWalking && !isPathAheadUnsafe && endangered.empty()) || (frame < roamPathFrame + ROUTE_FRAMES))) {
		if (isWalking) {
			pPath = roamPath;  // a failed target route may have replaced it, and Start() and regroups re-send pPath
		}
		return;
	}
	roamGoal = dest;
	roamPathFrame = frame;
	roamPath = nullptr;  // set again when this route arrives (ApplyRaidPath)

	// The raid's roam route: threat as a cost, the group's strength as the allowance
	CCircuitAI* circuit = manager->GetCircuit();
	CPathFinder* pathfinder = circuit->GetPathfinder();
	std::shared_ptr<IPathQuery> query = pathfinder->CreatePathSingleQuery(
			leader, circuit->GetThreatMap(), frame,
			leader->GetPos(frame), dest, pathfinder->GetSquareSize());
	SetRouteAllowance(query.get(), GetSafeThreat(), /*isDynamic*/ false, /*isMargin*/ true);
	pathQueries[leader] = query;
	query->HoldTask(this);

	pathfinder->RunQuery(query, [this](const IPathQuery* query) {
		if (this->IsQueryAlive(query)) {
			this->ApplyRaidPath(static_cast<const CQueryPathSingle*>(query));
		}
	});
}

AIFloat3 CRaidDefendTask::GetFleeHaven() const
{
	// The nearest post nearer than the raid's haven with no more threat there: the group stays by our assets
	const AIFloat3 haven = CRaidTask::GetFleeHaven();
	const AIFloat3& pos = leader->GetPos(manager->GetCircuit()->GetLastFrame());
	const float havenThreat = GetTaskThreatAt(leader, haven);
	AIFloat3 best = haven;
	float minSqDist = pos.SqDistance2D(haven);
	for (const CRaidDefence::SSlot& slot : GetMilitaryMgr()->GetRaidDefence()->GetSlots()) {
		const float sqDist = pos.SqDistance2D(slot.pos);
		if (slot.isEscort || (sqDist < SQUARE(FLEE_HOME_DIST)) || (sqDist >= minSqDist)
			|| (GetTaskThreatAt(leader, slot.pos) > havenThreat))
		{
			continue;
		}
		best = slot.pos;
		minSqDist = sqDist;
	}
	return best;
}

} // namespace circuit

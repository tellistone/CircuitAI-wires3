/*
 * RaidDefendTask.cpp
 *
 *  Created on: Nov 27, 2026
 *      Author: Claude
 */

#include "task/fighter/RaidDefendTask.h"
#include "module/MilitaryManager.h"
#include "module/Protect.h"
#include "module/RaidDefence.h"
#include "unit/enemy/EnemyUnit.h"
#include "unit/CircuitUnit.h"
#include "CircuitAI.h"
#include "util/Utils.h"

namespace circuit {

using namespace springai;

CRaidDefendTask::CRaidDefendTask(ITaskManager* mgr, float powerMod)
		: CRaidTask(mgr, FightType::RAID_DEFEND, 0.f, powerMod)
		, slotId(-1)
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
		// A merge: two groups of one slot, or committed to the same protect ward (gathering), become one, whatever its room
		const CRaidDefendTask* group = static_cast<const CRaidDefendTask*>(task);
		return (group->GetLeader() == unit)
				&& ((group->GetSlot() == slotId) || militaryMgr->GetProtect()->IsSameCommit(this, group));
	}
	return HasRoom();
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

ISquadTask* CRaidDefendTask::GetRaidMergeTask()
{
	// A new recruit starts alone: while waiting, merge often (there are few defence groups)
	return isParked ? GetMergeTask(MERGE_PARK_UPDATES) : GetMergeTask();
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

void CRaidDefendTask::OnNoTarget(bool isContestedAllowed)
{
	// quota.protect: committed with nothing to hunt, head for the ward's goal as a raid does, or wait where protect
	// says (FallbackRaid); else wait at the post (no post yet: where it is; CRaidDefence gives it one, or releases it,
	// within a second)
	if (IsProtecting() || IsGathering()) {
		FallbackRaid(false);
		return;
	}
	const int frame = manager->GetCircuit()->GetLastFrame();
	const CRaidDefence::SSlot* slot = GetMilitaryMgr()->GetRaidDefence()->FindSlot(slotId);
	WaitAt(frame, (slot != nullptr) ? slot->pos : leader->GetPos(frame));
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
		if ((sqDist < SQUARE(FLEE_HOME_DIST)) || (sqDist >= minSqDist)
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

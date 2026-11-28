/*
 * RaidDefendTask.h
 *
 *  Created on: Nov 27, 2026
 *      Author: Claude
 */

#ifndef SRC_CIRCUIT_TASK_FIGHTER_RAIDDEFENDTASK_H_
#define SRC_CIRCUIT_TASK_FIGHTER_RAIDDEFENDTASK_H_

#include "task/fighter/RaidTask.h"

namespace circuit {

/*
 * A raid group that defends our ground (quota.raid_defence, CRaidDefence). CRaidDefence gives it a slot: a post by
 * our undefended mexes and factories. Everything else is the raid group's own: target pick and fight check, escape,
 * flee grace, scoots, regroup, firing spots, routes under its strength. Its hooks (CRaidTask):
 * - merges only with defence groups of the same post or the same protect commit, checked every MERGE_PARK_UPDATES
 *   updates while it waits; never in a raid rally, never chases a bigger group
 * - new targets: on our ground within react_sec of its post (threat-aware travel, CRaidDefence::GetSlotSec), or in
 *   contact; its current target is kept out to TARGET_KEEP x react_sec on any ground; any in its protect ward's zone
 * - preferred targets: the raid's, and enemies that hit our mexes, constructors or factories in the last 5 s
 * - home ground: its own distance curve (quota.thr_mod.distance.raid_defend, the raid curve if unset)
 * - no contested-cluster routes, no roaming: with nothing to hunt it waits at its post (CRaidTask::WaitAt).
 *   Committed to a raid incident in its no-go: the same where protect says (its gathering post); in its go: the
 *   raid's way to the goal
 * - its escape runs to the nearest post with no more threat than the raid's haven
 */
class CRaidDefendTask: public CRaidTask {
public:
	CRaidDefendTask(ITaskManager* mgr, float powerMod);
	virtual ~CRaidDefendTask();

	// Merges only (CheckMergeTask asks with the other group's leader): it leads a defence group of the same slot
	// that moves like ours
	virtual bool CanAssignTo(CCircuitUnit* unit) const override;
	virtual void OnUnitIdle(CCircuitUnit* unit) override;
	virtual bool IsRallyCompatible(CCircuitUnit* unit) const override { return false; }  // never in a raid rally

	void SetSlot(int id);  // CRaidDefence: where it waits from now on
	int GetSlot() const { return slotId; }  // -1: none

protected:
	virtual bool HasRoom() const override;  // its slot holds less than it needs
	virtual ISquadTask* GetRaidMergeTask() override;
	virtual bool IsChaseAllowed() const override { return false; }
	virtual bool IsTargetAllowed(const CEnemyInfo* enemy, bool isZone, bool isCurrent) const override;
	virtual bool IsPreferredTarget(const CEnemyInfo* enemy, const CCircuitDef* edef) const override;
	virtual bool IsContestedAllowed() const override { return false; }
	virtual void OnNoTarget(bool isContestedAllowed) override;
	virtual springai::AIFloat3 GetFleeHaven() const override;

private:
	static constexpr float CONTACT_SLACK = 100.f;  // in contact: within the leader's range + this
	static constexpr float TARGET_KEEP = 1.5f;  // the current target is kept out to this x react_sec
	static constexpr float FLEE_HOME_DIST = 400.f;  // a haven this near is no escape (CRaidTask::Flee)
	static constexpr int MERGE_PARK_UPDATES = 4;  // while it waits, merges are checked this often (new recruits)

	int slotId;
};

} // namespace circuit

#endif // SRC_CIRCUIT_TASK_FIGHTER_RAIDDEFENDTASK_H_

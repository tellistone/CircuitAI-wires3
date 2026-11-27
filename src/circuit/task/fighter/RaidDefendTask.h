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
 * our undefended mexes and factories, or an escort beside an exposed constructor. Everything else is the raid
 * group's own: target pick and fight check, escape, flee grace, scoots, regroup, firing spots, routes under its
 * strength. Its hooks (CRaidTask):
 * - merges only with defence groups of the same slot or the same protect commit; never in a raid rally, never chases
 *   a bigger group
 * - new targets: on our ground within react_sec of its slot (threat-aware travel, CRaidDefence::GetSlotSec), or in
 *   contact; its current target is kept out to TARGET_KEEP x react_sec on any ground; any in its protect ward's zone
 * - preferred targets: the raid's, and enemies that hit our mexes, constructors or factories in the last 5 s
 * - no contested-cluster routes, no roaming: with nothing to hunt it walks to its slot on a threat-aware route (the
 *   raid's roam route, same allowance) and waits there in a small patrol, or guarding the constructor. No path
 *   queries while it waits. Committed to a raid incident in its no-go: the same at the gathering post (quota.protect);
 *   in its go: the raid's way to the goal
 * - its escape runs to the nearest post with no more threat than the raid's haven
 */
class CRaidDefendTask: public CRaidTask {
public:
	CRaidDefendTask(ITaskManager* mgr, float powerMod);
	virtual ~CRaidDefendTask();

	// Merges only (CheckMergeTask asks with the other group's leader): it leads a defence group of the same slot
	// that moves like ours
	virtual bool CanAssignTo(CCircuitUnit* unit) const override;
	virtual void Start(CCircuitUnit* unit) override;
	virtual void OnUnitIdle(CCircuitUnit* unit) override;
	virtual bool IsRallyCompatible(CCircuitUnit* unit) const override { return false; }  // never in a raid rally

	void SetSlot(int id);  // CRaidDefence: where it waits from now on
	int GetSlot() const { return slotId; }  // -1: none
	// CRaidDefence: a recruit held at its post while a raid incident is in a no-go: not lent to protect, not moved
	void SetHold(bool value) { isHold = value; }
	bool IsHold() const { return isHold; }

protected:
	virtual bool HasRoom() const override;  // its slot holds less than it needs
	virtual ISquadTask* GetRaidMergeTask() override { return GetMergeTask(); }
	virtual bool IsChaseAllowed() const override { return false; }
	virtual bool IsTargetAllowed(const CEnemyInfo* enemy, bool isZone, bool isCurrent) const override;
	virtual bool IsPreferredTarget(const CEnemyInfo* enemy, const CCircuitDef* edef) const override;
	virtual bool IsContestedAllowed() const override { return false; }
	virtual void OnNoTarget(bool isContestedAllowed) override;
	virtual springai::AIFloat3 GetFleeHaven() const override;
	virtual float GetThrDistModAt(const springai::AIFloat3& pos) const override;

private:
	static constexpr float PARK_RADIUS = 300.f;  // the leader this near its slot: wait there
	static constexpr float REPATH_DIST = 200.f;  // a new route, or new wait orders, once the slot moved this far
	static constexpr int PARK_ORDER_FRAMES = FRAMES_PER_SEC * 10;  // wait orders sent again at least this often
	static constexpr int ROUTE_FRAMES = FRAMES_PER_SEC * 3;  // a route to the same spot asked for at most this often
	static constexpr float CONTACT_SLACK = 100.f;  // in contact: within the leader's range + this
	static constexpr float TARGET_KEEP = 1.5f;  // the current target is kept out to this x react_sec
	static constexpr float FLEE_HOME_DIST = 400.f;  // a haven this near is no escape (CRaidTask::Flee)

	void Park(int frame, const springai::AIFloat3& dest, CCircuitUnit* vip);
	void ParkUnit(CCircuitUnit* unit, int frame);
	void WalkTo(int frame, const springai::AIFloat3& dest);

	int slotId;
	bool isHold;
	bool isParked;
	springai::AIFloat3 parkPos;
	ICoreUnit::Id parkVipId;  // escort: the constructor its units guard; -1: a post
	int parkFrame;  // the last wait orders
	size_t parkCount;  // units then: after a merge the newcomers get them too
};

} // namespace circuit

#endif // SRC_CIRCUIT_TASK_FIGHTER_RAIDDEFENDTASK_H_

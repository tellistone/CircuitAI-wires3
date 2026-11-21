/*
 * SupportTask.h
 *
 *  Created on: Jul 3, 2016
 *      Author: rlcevg
 */

#ifndef SRC_CIRCUIT_TASK_FIGHTER_SUPPORTTASK_H_
#define SRC_CIRCUIT_TASK_FIGHTER_SUPPORTTASK_H_

#include "task/fighter/FighterTask.h"

namespace circuit {

class ISquadTask;
class CQueryPathMulti;

/*
 * An unarmed support unit (Aspis, Funnelweb) on its way to a group (armed ones are attack units: CAttackTask).
 * It picks a host it keeps up with (quota.attack_merge.speed_ratio with the leader, quota.escort.speed_min with the
 * core's slowest unit): the attack group with the most power for the shields already with it or walking to it, else a
 * gathering (defend) group; else any attack group of its kind of mover (a slow one slows that group down rather than
 * idle at base); else it waits near base. It walks there with Move orders along a threat-capped route (standing where
 * it is while there is none), and joins within quota.attack_merge.distance. In the group it is an escort: a formation
 * slot while walking, a point round the core's centre in fights (never attack orders)
 */
class CSupportTask: public IFighterTask {
public:
	CSupportTask(ITaskManager* mgr);
	virtual ~CSupportTask();

	virtual void AssignTo(CCircuitUnit* unit) override;
	virtual void RemoveAssignee(CCircuitUnit* unit) override;

	virtual void Start(CCircuitUnit* unit) override;
	virtual void Update() override;

	// The group it walks to (nullptr: none, waits near base). Compared only: it may be gone
	const ISquadTask* GetHost() const { return host; }

private:
	// Best host for the unit now, or nullptr. Its current host is kept unless another is clearly better
	ISquadTask* FindHost(CCircuitUnit* unit, int frame) const;
	void WalkTo(CCircuitUnit* unit, ISquadTask* squad, int frame);
	void ApplyPath(const CQueryPathMulti* query);
	void HoldNearBase(CCircuitUnit* unit);

	ISquadTask* host;
	bool isHolding;  // waiting near base (sent there already)
};

} // namespace circuit

#endif // SRC_CIRCUIT_TASK_FIGHTER_SUPPORTTASK_H_

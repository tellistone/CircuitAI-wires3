/*
 * EscortTask.h
 *
 *  Created on: Oct 31, 2026
 *      Author: Claude
 */

#ifndef SRC_CIRCUIT_TASK_BUILDER_ESCORTTASK_H_
#define SRC_CIRCUIT_TASK_BUILDER_ESCORTTASK_H_

#include "task/builder/BuilderTask.h"

namespace circuit {

class ISquadTask;

/*
 * Builder that follows an attack group: patrols (auto repair/reclaim) behind it
 * while the group holds or advances, plain moves while it falls back or when lagging.
 */
class CBEscortTask: public IBuilderTask {
public:
	CBEscortTask(ITaskManager* mgr, ISquadTask* squad);
	virtual ~CBEscortTask();

	virtual bool CanAssignTo(CCircuitUnit* unit) const override;
	virtual void AssignTo(CCircuitUnit* unit) override;
	virtual void RemoveAssignee(CCircuitUnit* unit) override;

	virtual void Start(CCircuitUnit* unit) override;
	virtual void Update() override;
	virtual void Stop(bool done) override;

public:
	virtual void OnUnitIdle(CCircuitUnit* unit) override;

	ISquadTask* GetSquad() const { return squad; }

private:
	bool IsSquadAlive() const;
	void Follow(CCircuitUnit* unit, bool isForce);

	ISquadTask* squad;  // validated against MilitaryManager tasks before use
	int fightType;
	springai::AIFloat3 orderPos;
	bool isFightOrder;
};

} // namespace circuit

#endif // SRC_CIRCUIT_TASK_BUILDER_ESCORTTASK_H_

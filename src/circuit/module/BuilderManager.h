/*
 * BuilderManager.h
 *
 *  Created on: Dec 1, 2014
 *      Author: rlcevg
 */

#ifndef SRC_CIRCUIT_MODULE_BUILDERMANAGER_H_
#define SRC_CIRCUIT_MODULE_BUILDERMANAGER_H_

#include "module/UnitModule.h"
#include "task/builder/BuilderTask.h"
#include "terrain/TerrainData.h"
#include "unit/CircuitUnit.h"

#include <map>
#include <set>
#include <vector>
#include <unordered_set>

namespace springai {
	class AIFloat3;
}

namespace circuit {

class IGridLink;
class CQueryCostMap;
class CRetreatTask;
class CBRepairTask;
class CBReclaimTask;
class CCombatTask;

struct SBuildChain;

class CBuilderManager: public IUnitModule {
public:
	friend class CBuilderScript;

	CBuilderManager(CCircuitAI* circuit);
	virtual ~CBuilderManager();

private:
	void ReadConfig();
	void Init();
public:
	void Release();

	virtual int UnitCreated(CCircuitUnit* unit, CCircuitUnit* builder) override;
	virtual int UnitFinished(CCircuitUnit* unit) override;
	virtual int UnitIdle(CCircuitUnit* unit) override;
	virtual int UnitDamaged(CCircuitUnit* unit, CEnemyInfo* attacker) override;
	virtual int UnitDestroyed(CCircuitUnit* unit, CEnemyInfo* attacker) override;

	CCircuitDef* GetTerraDef() const { return terraDef; }

	unsigned int GetWorkerCount() const { return workers.size(); }
	float GetJobSwitch() const { return jobSwitch; }
	void AddBuildPower(CCircuitUnit* unit);
	void DelBuildPower(CCircuitUnit* unit);
	float GetBuildPower() const { return buildPower; }
	float GetWorkerCost() const { return workerCost; }  // metal value of mobile constructors, commanders excluded
	bool CanEnqueueTask(const unsigned mod = 8) const { return buildTasksCount < workers.size() * mod; }
	const std::set<IBuilderTask*>& GetTasks(IBuilderTask::BuildType type) const;
	void ActivateTask(IBuilderTask* task);

	IBuilderTask* EnqueueTask(IBuilderTask::Priority priority,
							  CCircuitDef* buildDef,
							  const springai::AIFloat3& position,
							  IBuilderTask::BuildType type,
							  float cost,
							  float shake = SQUARE_SIZE * 32,  // Alter/randomize position by offset
							  bool isActive = true,  // Should task go to general queue or remain detached?
							  int timeout = ASSIGN_TIMEOUT);
	IBuilderTask* EnqueueTask(IBuilderTask::Priority priority,
							  CCircuitDef* buildDef,
							  const springai::AIFloat3& position,
							  IBuilderTask::BuildType type,
							  float shake = SQUARE_SIZE * 32,
							  bool isActive = true,
							  int timeout = ASSIGN_TIMEOUT);
	IBuilderTask* EnqueueFactory(IBuilderTask::Priority priority,
								 CCircuitDef* buildDef,
								 const springai::AIFloat3& position,
								 float shake = SQUARE_SIZE * 32,
								 bool isPlop = false,
								 bool isActive = true,
								 int timeout = ASSIGN_TIMEOUT);
	IBuilderTask* EnqueuePylon(IBuilderTask::Priority priority,
							   CCircuitDef* buildDef,
							   const springai::AIFloat3& position,
							   IGridLink* link,
							   float cost,
							   bool isActive = true,
							   int timeout = ASSIGN_TIMEOUT);
	IBuilderTask* EnqueueRepair(IBuilderTask::Priority priority,
								CCircuitUnit* target,
								int timeout = ASSIGN_TIMEOUT);
	bool IsUnsafePrioRepair(const IBuilderTask* task, CCircuitUnit* unit, const springai::AIFloat3& pos) const;
	bool IsRepairAssigned(ICoreUnit::Id unitId) const;
	float GetPrioRepairLeash() const { return prioRepairLeash; }
	void OnPrioRepairLeash(ICoreUnit::Id unitId);  // target drove off mid-repair: hold off new priority jobs
	// Mobile, not retreating repair target out of builder's reach (builder at pos) and moving away from it
	// faster than repair_flee share of its own max speed
	bool IsRepairTargetFleeing(const IBuilderTask* task, CCircuitUnit* builder, const springai::AIFloat3& pos,
							   const springai::AIFloat3* targetVel = nullptr) const;  // targetVel: already read by caller
	// Builder closing in on a moving repair target: checkpoint of its gap beyond build range
	struct SChaseCheck {
		ICoreUnit::Id targetId = -1;
		float gap = 0.f;
		int frame = -1;
		void Reset() { targetId = -1; frame = -1; }
	};
	// gap <= 0 = in reach. True once the gap has not shrunk by repair_chase[1] for repair_chase[0] sec
	bool IsChaseStalled(SChaseCheck& chase, ICoreUnit::Id targetId, float gap, int frame) const;
	// Engine's own repair (picked while patrolling) of a mobile unit that the builder doesn't close in on
	bool IsEngineChaseStalled(CCircuitUnit* unit, SChaseCheck& chase) const;
	int GetPrioRepairCooldown() const { return prioRepairCooldown; }
	void OnRetreatEnd(CCircuitUnit* unit);  // unit left retreat: drop its leftover retreat (HIGH) repair job

	struct SEscort {
		float minPower;  // min attack power of a group to get escort builder, 0 = disabled
		float share;  // max share of workers on escort duty
		float behind;  // distance behind group leader (towards base)
		float follow;  // distance from escort point at which builder catches up along threat-aware path
		float range;  // max distance from a builder to the group when assigning
		bool isComm;  // allow commander to escort
		int maxPriority;  // highest priority of a job a builder can be taken off (NOW never)
	};
	const SEscort& GetEscortCfg() const { return escort; }
private:
	void PullRepairer(IBuilderTask* task, CCircuitUnit* target);
	bool IsPrioRepairFull(const CCircuitUnit* unit) const;
public:
	IBuilderTask* EnqueueReclaim(IBuilderTask::Priority priority,
								 const springai::AIFloat3& position,
								 float cost,
								 int timeout,
								 float radius = .0f,
								 bool isMetal = true);
	IBuilderTask* EnqueueReclaim(IBuilderTask::Priority priority,
								 CCircuitUnit* target,
								 int timeout = ASSIGN_TIMEOUT);
	IBuilderTask* EnqueuePatrol(IBuilderTask::Priority priority,
								const springai::AIFloat3& position,
								float cost,
								int timeout);
	IBuilderTask* EnqueueTerraform(IBuilderTask::Priority priority,
								   CCircuitUnit* target,
								   const springai::AIFloat3& position = -RgtVector,
								   float cost = 1.0f,
								   bool isActive = true,
								   int timeout = ASSIGN_TIMEOUT);
	IBuilderTask* EnqueueGuard(IBuilderTask::Priority priority,
							   CCircuitUnit* target,
							   int timeout = ASSIGN_TIMEOUT);
	IUnitTask* EnqueueWait(int timeout);
	CRetreatTask* EnqueueRetreat();
	CCombatTask* EnqueueCombat(float powerMod);

private:
	IBuilderTask* AddTask(IBuilderTask::Priority priority,
						  CCircuitDef* buildDef,
						  const springai::AIFloat3& position,
						  IBuilderTask::BuildType type,
						  float cost,
						  float shake,
						  bool isActive,
						  int timeout);
	void DequeueTask(IUnitTask* task, bool done = false);

public:
	bool IsBuilderInArea(CCircuitDef* buildDef, const springai::AIFloat3& position) const;  // Check if build-area has proper builder

	virtual IUnitTask* MakeTask(CCircuitUnit* unit) override;
	virtual void AbortTask(IUnitTask* task) override;
	virtual void DoneTask(IUnitTask* task) override;
	virtual void FallbackTask(CCircuitUnit* unit) override;

	SBuildChain* GetBuildChain(IBuilderTask::BuildType buildType, CCircuitDef* cdef);

	bool IsReclaimed(CAllyUnit* unit) const { return reclaimedUnits.find(unit) != reclaimedUnits.end(); }

	/*
	 * Commander flee (always on) and hide (commander.json "hide")
	 */
	// DefaultMakeTask sends this armed builder to fight at home (IBuilderTask::Reevaluate re-plans on the same test)
	bool IsHomeFight(CCircuitUnit* unit);
	// An uncovered danger (recently seen ones included) within the wide (stop-fleeing) line: no morph now
	bool IsCommInDanger(CCircuitUnit* unit);

private:
	IUnitTask* DefaultMakeTask(CCircuitUnit* unit);
	// How a job ranks for a builder: travel cost x invWeight, lowest wins; beyond travelSec only as a fallback
	struct SJobRank {
		float invWeight;  // 1 / economy.builder_jobs.weight of the priority
		float travelSec;  // economy.builder_jobs.travel of the priority; max = no cap
	};
	// By the job's priority, except mex drops to NORMAL during an energy stall
	const SJobRank& GetJobRank(const IBuilderTask* task, bool isEnergyStalling) const;
	IBuilderTask* MakeCommTask(CCircuitUnit* unit, const CQueryCostMap* query, float sqMaxBaseRange);

	struct SCommDanger {
		springai::AIFloat3 pos;
		float range;  // its ground/water weapon range, 0 for a blip
		float threat;
		bool isAir;  // a gunship
		bool isSeen;  // in radar or LOS now; else gone from both within COMM_DANGER_MEMORY (jobs and morph only)
	};
	// One commander's view of the dangers, once per frame (GetCommState)
	struct SCommState {
		int frame = -1;
		float wideMargin = 0.f;  // flee_margin + max(25%, 50): where a flee stops
		float allyThreat = 0.f;  // hide.flee_ally_threat, < 0 = never covered
		int nearCount = 0;  // seen dangers whose flee zone (range + flee_margin) reaches it
		float nearThreat = 0.f;
		int wideCount = 0;  // seen dangers within range + wideMargin
		float wideThreat = 0.f;
		int allCount = 0;  // as wideCount, recently seen ones included
		float allThreat = 0.f;
		springai::AIFloat3 dangerPos;  // nearest seen danger within the wide line
		bool isAir = false;  // it is a gunship
		float cover = 0.f;  // the commander and allies within flee_ally_radius (power x sqrt(health %)), when allCount > 0
		int escapeFrame = -1;  // HasCommEscape evaluated at
		bool isEscape = false;
		springai::AIFloat3 escapeGoal;
	};
	static float GetCommWideMargin(float margin) { return margin + std::max(0.25f * margin, 50.f); }
	float GetCommFleeMargin(CCircuitUnit* unit) const;  // hide.flee_margin, < 0 = no flee
	void UpdateCommDangers();
	// Outside every flee zone (range + margin); isSeenOnly: dangers in radar or LOS now only
	bool IsCommSafe(const springai::AIFloat3& pos, float margin, bool isSeenOnly);
	// Cover power of ground-capable armed allies and static defences within radius of pos, skip left out
	float GetCommCover(const springai::AIFloat3& pos, float radius, CCircuitUnit* skip) const;
	// nullptr: not a commander, or its flee is off
	SCommState* GetCommState(CCircuitUnit* unit);
	// Cover beats threat x mod by hide.flee_ally_threat
	static bool IsCommCovered(const SCommState* state, float threat, float mod) {
		return (state->allyThreat >= 0.f) && (state->cover - threat * mod >= state->allyThreat);
	}
	// A safe flee spot not at its feet exists (state->escapeGoal)
	bool HasCommEscape(CCircuitUnit* unit, SCommState* state);
	// An uncovered seen danger within the wide line and nowhere better to flee to
	bool IsCommCornered(CCircuitUnit* unit, SCommState* state);
	// A job at pos: outside the flee zones (wide line, recently seen dangers included), or covered 1.25x there
	bool IsCommJobSafe(CCircuitUnit* unit, const springai::AIFloat3& pos, float margin);
	// Cornered commander: a static defence on its far side from the danger; nullptr = none to build (or metal
	// stalling), or one already on the go (the current task)
	IBuilderTask* MakeCommDefence(CCircuitUnit* unit, const springai::AIFloat3& dangerPos, bool isAir);
	// Hide mode: allied strength (the commander's own left out) at pos at least matches the enemy's, on our ground
	bool IsCommTerritory(CCircuitUnit* unit, const springai::AIFloat3& pos) const;
	// A flee spot outside every seen flee zone, not at its feet; false = none (cornered)
	bool FindCommFleeGoal(CCircuitUnit* unit, const springai::AIFloat3& dangerPos, bool isAir, float margin,
						  springai::AIFloat3& outGoal);
	void UpdateCommFlee();
	IBuilderTask* MakeBuilderTask(CCircuitUnit* unit, const CQueryCostMap* query);
	IBuilderTask* CreateBuilderTask(const springai::AIFloat3& position, CCircuitUnit* unit);

	void AddBuildList(CCircuitUnit* unit);
	void RemoveBuildList(CCircuitUnit* unit);

	void Watchdog();
	void UpdateIdle();
	void UpdateBuild();
	void UpdateEscort();

	Handlers2 createdHandler;
	Handlers1 finishedHandler;
	Handlers1 idleHandler;
	EHandlers damagedHandler;
	EHandlers destroyedHandler;

	std::map<CAllyUnit*, IBuilderTask*> unfinishedUnits;
	std::map<ICoreUnit::Id, CBRepairTask*> repairedUnits;
	std::map<CAllyUnit*, CBReclaimTask*> reclaimedUnits;
	std::vector<std::set<IBuilderTask*>> buildTasks;  // UnitDef based tasks
	unsigned int buildTasksCount;
	float buildPower;
	float workerCost;
	std::vector<IUnitTask*> buildUpdates;  // owner
	unsigned int buildIterator;

	std::set<CCircuitUnit*> workers;
	std::set<CCircuitUnit*> comms;  // commanders among workers
	std::vector<SCommDanger> commDangers;
	int commDangerFrame;  // frame commDangers was built at
	std::map<CCircuitUnit*, SCommState> commStates;
	F3Vec commFleeSpots;  // FindCommFleeGoal scratch
	float commThrMod;  // quota.thr_mod.comm: undone for enemy commanders' threat in the flee
	bool isCommHide;  // hide by threat, latched once triggered
	bool isCommHideAir;  // enemy air now (live): hide, and only our buildings' ground counts as territory
	float prioRepairRange;  // range to pull busy workers onto NOW repair, 0 = disabled
	float prioRepairShare;  // max share of workers that can be on NOW repair at once
	SJobRank jobRanks[4];  // LOW, NORMAL, HIGH, NOW
	float jobSwitch;  // walking builder switches to a same-type job nearer than this share of its current one; 0 = off
	bool isPrioRepairSafe;  // apply builder threat check to NOW repair of mobile units
	bool isPrioRepairSafeStatic;  // same for static units
	float prioRepairLeash;  // cancel started priority repair if target gets this far beyond build range, 0 = off
	int prioRepairCooldown;  // frames without new priority repair job after leash break
	float repairFleeShare;  // target moving away faster than this share of its max speed counts as fleeing, 0 = off
	int repairChaseFrames;  // frames a builder may chase a repair target without closing in, 0 = off
	float repairChaseClose;  // elmos the gap must shrink by within repairChaseFrames
	std::map<ICoreUnit::Id, int> leashUntil;  // unitId: frame until which priority repair is held off
	SEscort escort;
	std::set<IUnitTask*> escortTasks;
	std::map<CCircuitUnit*, std::shared_ptr<IPathQuery>> costQueries;  // IPathQuery owner

	CCircuitDef* terraDef;
	std::unordered_map<IBuilderTask::BT, std::unordered_map<CCircuitDef*, SBuildChain*>> buildChains;  // owner
	struct SSuper {
		float minIncome;  // metal per second
		float maxTime;  // seconds
	} super;

public:
	void UpdateAreaUsers();
private:
	std::unordered_set<STerrainMapMobileType::Id> workerMobileTypes;
	std::unordered_set<CCircuitDef*> workerDefs;
	std::map<STerrainMapArea*, std::map<CCircuitDef*, int>> buildAreas;  // area <=> worker types

	virtual void Load(std::istream& is) override;
	virtual void Save(std::ostream& os) const override;
};

} // namespace circuit

#endif // SRC_CIRCUIT_MODULE_BUILDERMANAGER_H_

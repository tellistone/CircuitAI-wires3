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
	bool IsRepairFleeOn() const { return repairFleeShare > 0.f; }
	int GetRepairFleeFrames() const { return repairFleeFrames; }
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
	// Flee zones the commander should keep out of: visible ground dangers at their weapon range + margin
	bool IsCommSafe(const springai::AIFloat3& pos, float margin);
	// DefaultMakeTask sends this armed builder to fight at home (IBuilderTask::Reevaluate re-plans on the same test)
	bool IsHomeFight(CCircuitUnit* unit);
	// In a flee zone at the wide (stop-fleeing) line: no morph now
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
	};
	static float GetCommWideMargin(float margin) { return margin + std::max(0.25f * margin, 50.f); }
	float GetCommFleeMargin(CCircuitUnit* unit) const;  // hide.flee_margin, < 0 = no flee
	void UpdateCommDangers();
	// Number of flee zones (range + margin) reaching pos; outThreat their threat, outPos/outIsAir the nearest one
	int GetCommDanger(const springai::AIFloat3& pos, float margin, float& outThreat,
					  springai::AIFloat3* outPos = nullptr, bool* outIsAir = nullptr);
	// The commander itself and ground-capable allies within radius of pos, minus enemyThreat, reach allyThreat
	bool IsCommCovered(const springai::AIFloat3& pos, float enemyThreat, float allyThreat, float radius) const;
	// Fight at home only if no flee zone (wide line) reaches the commander, or it is covered there
	bool IsCommCombatSafe(CCircuitUnit* unit, float margin);
	// Hide mode: allied strength (the commander's own left out) at pos at least matches the enemy's, on our ground
	bool IsCommTerritory(CCircuitUnit* unit, const springai::AIFloat3& pos) const;
	springai::AIFloat3 FindCommFleeGoal(CCircuitUnit* unit, const springai::AIFloat3& dangerPos, bool isAir,
										float margin);
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
	int repairFleeFrames;  // frames a target may keep fleeing from all its repairers before the job is dropped
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

/*
 * BuilderManager.h
 *
 *  Created on: Dec 1, 2014
 *      Author: rlcevg
 */

#ifndef SRC_CIRCUIT_MODULE_BUILDERMANAGER_H_
#define SRC_CIRCUIT_MODULE_BUILDERMANAGER_H_

#include "module/UnitModule.h"
#include "setup/SetupManager.h"
#include "task/builder/BuilderTask.h"
#include "terrain/TerrainData.h"
#include "unit/CircuitUnit.h"

#include <array>
#include <map>
#include <set>
#include <vector>
#include <unordered_map>
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
class CBRestoreTask;
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
	void OnPrioRepairLeash(ICoreUnit::Id unitId, int cooldown = -1);  // -1 = repair_leash cooldown
	bool IsRepairHitBuilder() const { return isRepairHitBuilder; }
	int GetRepairHitCooldown() const { return repairHitCooldown; }  // target drove off mid-repair: hold off new priority jobs
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

	// Restore terraformed ground (economy.json "restore"), every 2 s: a cell whose height is off the map's start by
	// <depth> or more on <share> of its samples gets a CBRestoreTask at <priority>, nearest builder first, while a
	// builder is within <range> and the cell is safe: no known enemy on it or next to it, no threat on it. A job
	// whose cell turns unsafe, or whose builder's path there crosses threat, is dropped and its builders stop. Any
	// drop of a job a builder took (also a builder hit or killed) backs its cell off (cooldown, doubling).
	// Cells with an allied building are skipped (levelled pads); an ally's terraform gives its cell up
	struct SRestore {
		static constexpr int CELL = 16;  // cell side in heightmap squares (128 elmos): one terraform segment each
		struct SCell {
			CBRestoreTask* task = nullptr;
			int until = -1;  // frame the cell is held off until
			int tries = 0;  // jobs done while it stays dug; reset once it is back and not held
			int backoff = 0;  // last hold after a drop, frames (0 = none since the last finished job)
			int enemyFrame = -1;  // frame a known enemy was seen on or next to it
			int buildingFrame = -1;  // frame an allied building was seen on it
			bool isDug = false;
		};
		float range = 0.f;  // builder to cell, 0 = off
		float depth;
		float share;
		int maxTasks;
		int maxBuilders;  // per job
		int maxTries;
		int cooldown;  // frames
		IBuilderTask::Priority priority;  // of the jobs (and ZK's build priority they send)
		int cellsX = 0;
		int cellsZ = 0;
		std::vector<SCell> grid;  // cellsX * cellsZ
		int dugCount = 0;  // dug cells at the last scan
		const SAreaData* scanned = nullptr;  // heightmap of the last scan (TerrainData swaps it on change)
		int nextTag = 1 << 20;  // ZK terraform tags: a float, below 2^24
		int CellOf(const springai::AIFloat3& pos) const;
		springai::AIFloat3 CellPos(int cell, float offset) const;  // offset 0 = min corner, 0.5 = centre, 1 = max
	};
	const SRestore& GetRestore() const { return restore; }
	int NextRestoreTag() { return restore.nextTag++; }
	void OnRestoreEnd(CBRestoreTask* task, bool done);
private:
	void UpdateRestore();
	void ScanRestore();
	SRestore restore;
public:

	// Escalation mode (MilitaryManager::IsEscalation), every escalation.interval: an allied superweapon
	// (escalation.super) or singu being built gets our builders at NOW (our own job on it raised, an ally's frame
	// assisted); with none of a kind being built, and no job of ours for one, we start one at NOW after a random
	// escalation.delay. Off: the jobs raised drop back to NORMAL
	void UpdateEscalation(bool isOn);
	// Escalation has live superweapon work: a frame that progressed in the last escalation.stale s or has our builders,
	// a job of ours not placed yet (with builders, or younger than escalation.stale), or an ally's claim. Factories pause
	// only then: not for a start timer, a stuck frame (ZK frames don't decay), nor for singus alone
	bool IsEscalationBusy() const { return isEscalationBusy; }
	bool IsEscalationTask(const IBuilderTask* task) const {
		return escalationTasks.find(const_cast<IBuilderTask*>(task)) != escalationTasks.end();
	}
private:
	void PromoteEscalation(IBuilderTask* task);
	IBuilderTask* AssistEscalationFrame(CAllyUnit* frame);
	IBuilderTask* StartEscalation(int kind);
	// Our build power on escalation jobs (singu jobs only with isSingu) is at its share of all of it, unit's added:
	// escalation.builder_share (all of it while metal is near full), and singu_share of builder_share for singus
	bool IsEscalationFull(CCircuitUnit* unit, bool isSingu) const;
	bool IsEscalationSingu(const IBuilderTask* task) const;
	static constexpr int ESC_CLAIM_FRAMES = FRAMES_PER_SEC * 30;  // ally-team claim, refreshed every update
	static constexpr int ESC_STUCK_STALES = 3;  // a frame without progress this many escalation.stale stops blocking a start
	std::set<IBuilderTask*> escalationTasks;  // raised to NOW by escalation mode
	// Per kind (0 superweapon, 1 singu): when we start our own (-1 = no timer); kept through a short off spell
	struct SEscProject {
		int startFrame = -1;
		bool isLogged = false;  // "nothing to start" logged since the timer ran out
		bool isFrontStale = false;  // the last front job went stale: the next one is placed as the rest
		int staleCount = 0;  // jobs dropped stale in a row: the next start waits 2^count x as long (up to 2 min)
	};
	std::array<SEscProject, ESC_KINDS> escProjects;
	// Own unplaced escalation jobs without builders: since when. Until escalation.stale they are live (claim,
	// factory pause); past it one escalation started is dropped, one of the normal paths only blocks a new start
	std::map<IBuilderTask*, int> escIdleSince;
	// Escalation frames (unit id): build progress, and when it last went up. Kept through an off spell
	struct SEscFrame {
		float progress;
		int riseFrame;
	};
	std::map<int, SEscFrame> escFrames;
	// Jobs escalation started itself (the only ones a stale wait drops; kept through an off spell)
	std::set<IBuilderTask*> escStarted;
	bool isEscalationBusy = false;
public:
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
	// By the job's priority, except mex drops to NORMAL while energy is worse off than metal (IsEnergyStalling).
	// Not on the energy latch alone: then metal is as short, and a mex still helps
	const SJobRank& GetJobRank(const IBuilderTask* task, bool isEnergyStalling) const;
	IBuilderTask* MakeCommTask(CCircuitUnit* unit, const CQueryCostMap* query, float sqMaxBaseRange);

	struct SCommDanger {
		springai::AIFloat3 pos;
		float range;  // its ground/water weapon range, 0 for a blip
		float threat;
		bool isAir;  // a gunship
		bool isSeen;  // in radar or LOS now; else gone from both within COMM_DANGER_MEMORY (jobs and morph only)
	};
	// A spot a builder other than a commander fled from: every such builder's jobs keep off it until endFrame
	struct SFleeScar {
		springai::AIFloat3 pos;
		float range;  // the danger's weapon range
		float threat;  // what reached the builder as it fled
		int endFrame;
	};
	// Dangers in reach of a spot and their summed threat
	struct SCommZone {
		int count = 0;
		float threat = 0.f;
		void Add(float t) { ++count; threat += t; }
	};
	// Flee settings: a commander's from commander.json hide, other builders' from retreat.builder_flee and their own
	// builder_flee_margin
	struct SFleeCfg {
		float margin = -1.f;  // flee_margin: a flee starts at a danger's range + this; < 0 = off
		float allyThreat = 0.f;  // flee_ally_threat
		float allyRadius = 500.f;  // flee_ally_radius
	};
	// A ground builder with the flee on: its flee settings, and its view of the dangers (GetCommState)
	struct SCommState {
		const CSetupManager::SCommInfo::SHide* hide = nullptr;  // commanders only: hide mode and morph
		ICoreUnit::Id id = -1;
		SFleeCfg cfg;
		float wideMargin = 0.f;  // margin + max(25%, 50): where a flee stops
		int frame = -1;  // read at; reused for COMM_STATE_FRAMES
		SCommZone nearZone;  // seen dangers whose flee zone (range + flee_margin) reaches it
		SCommZone wideZone;  // seen dangers within range + wideMargin
		SCommZone allZone;  // as wideZone, recently seen ones included
		// Nearest seen danger within the wide line, kept from the last refresh that saw one: a flee whose goal turns
		// unsafe while the danger near it has gone from sight still picks a new spot away from where it was. Also
		// what a cornered builder's defence must hit
		springai::AIFloat3 dangerPos;
		float dangerRange = 0.f;  // its weapon range
		bool isAir = false;  // it is a gunship
		float own = 0.f;  // its own cover power (power x sqrt(health %))
		float cover = 0.f;  // it and allies within flee_ally_radius, when allZone has any (stops once it covers all)
		int escapeFrame = -1;  // HasCommEscape evaluated at
		bool isEscape = false;
		springai::AIFloat3 escapeGoal;
		const IBuilderTask* cornerTask = nullptr;  // other builders: the defence it was cornered into (MakeCommDefence)
		bool IsComm() const { return hide != nullptr; }
		// Cover beats threat x mod by flee_ally_threat (< 0 = never covered)
		bool IsCovered(float threat, float mod) const {
			return (cfg.allyThreat >= 0.f) && (cover - threat * mod >= cfg.allyThreat);
		}
		// Some danger in the zone, and not covered
		bool IsExposed(const SCommZone& zone, float mod) const {
			return (zone.count > 0) && !IsCovered(zone.threat, mod);
		}
	};
	// An ally that covers a commander: a ground-capable armed mobile or a static defence, with its cover power
	struct SCommCover {
		ICoreUnit::Id id;
		springai::AIFloat3 pos;
		float power;
	};
	static float GetCommWideMargin(float margin) { return margin + std::max(0.25f * margin, 50.f); }
	void AddComm(CCircuitUnit* unit);  // a state for a ground builder with the flee on
	void UpdateCommDangers();
	// Outside every seen flee zone (range + margin)
	bool IsCommSafe(const springai::AIFloat3& pos, float margin);
	void UpdateCommCovers();
	// Cover power within radius of pos, skipId left out; stops summing once it reaches enough
	float GetCommCover(const springai::AIFloat3& pos, float radius, ICoreUnit::Id skipId, float enough);
	// nullptr: a flying builder, or its flee is off
	SCommState* GetCommState(CCircuitUnit* unit);
	// A safe flee spot not at its feet exists (state->escapeGoal)
	bool HasCommEscape(CCircuitUnit* unit, SCommState* state);
	// An uncovered seen danger within the wide line and nowhere better to flee to
	bool IsCommCornered(CCircuitUnit* unit, SCommState* state);
	// Other builders: on the defence they were cornered into (cornerTask), which they finish unless metal stalls
	bool IsFinishingDefence(CCircuitUnit* unit, const SCommState* state) const;
	// Metal is short and builders pull more than income: their new metal jobs wait (MakeBuilderTask)
	bool IsMetalStalling() const;
	// A job at pos: always with no state (flee off); else outside the flee zones (wide line, recently seen dangers
	// included), or covered 1.25x there. Other builders: a fight-back task is measured at the start line, and the
	// flee scars count
	bool IsCommJobSafe(const SCommState* state, const springai::AIFloat3& pos, const IBuilderTask* task = nullptr);
	// Cornered: a static defence on its far side from the danger; nullptr = none to build (or metal short), or one
	// already on the go (the current task). Other builders keep it as cornerTask
	IBuilderTask* MakeCommDefence(CCircuitUnit* unit, SCommState* state);
	// Hide mode: allied strength (the commander's own left out) at pos at least matches the enemy's, on our ground
	bool IsCommTerritory(CCircuitUnit* unit, const springai::AIFloat3& pos) const;
	// A flee spot outside every seen flee zone, not at its feet; false = none (cornered)
	bool FindCommFleeGoal(CCircuitUnit* unit, const springai::AIFloat3& dangerPos, bool isAir, float margin,
						  springai::AIFloat3& outGoal);
	void UpdateCommFlee();
	void StartCommFlee(CCircuitUnit* unit, SCommState* state);
	void ContinueCommFlee(CCircuitUnit* unit, CRetreatTask* retreat, SCommState* state);
	void LogFlee(CCircuitUnit* unit, const char* event, const SCommState* state) const;
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
	std::map<CCircuitUnit*, SCommState> comms;  // ground workers with the flee on, commanders and other builders
	std::vector<SCommDanger> commDangers;
	int commDangerFrame;  // frame commDangers was built at
	std::vector<SFleeScar> fleeScars;  // StartCommFlee adds them, UpdateCommDangers drops expired ones
	std::vector<SCommCover> commCovers;
	int commCoverFrame;  // frame commCovers was built at
	F3Vec commFleeSpots;  // FindCommFleeGoal scratch
	float commThrMod;  // quota.thr_mod.comm: undone for enemy commanders' threat in the flee
	SFleeCfg builderFlee;  // retreat.builder_flee: the flee of builders other than commanders
	bool isFleeLog;  // retreat.flee_log: LogFlee writes flee starts and ends
	bool isCommHide;  // hide by threat, latched once triggered
	bool isCommHideAir;  // enemy air now (live): hide, and only our buildings' ground counts as territory
	float prioRepairRange;  // range to pull busy workers onto NOW repair, 0 = disabled
	float prioRepairShare;  // max share of workers that can be on NOW repair at once
	SJobRank jobRanks[4];  // LOW, NORMAL, HIGH, NOW
	float jobSwitch;  // walking builder switches to a same-type job nearer than this share of its current one; 0 = off
	bool isPrioRepairSafe;  // apply builder threat check to NOW repair of mobile units
	bool isPrioRepairSafeStatic;  // same for static units
	bool isPrioRepairSafeHeld;  // the threat check also needs an enemy-held area (as regular jobs); false = threat alone
	bool isRepairHitBuilder;  // drop a priority repair when one of its builders is hit
	bool isRepairHitTarget;  // drop a priority repair of a mobile unit when the unit is hit while builders are on it
	int repairHitCooldown;  // frames without new priority repair for the unit after such a drop
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

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
#include <functional>
#include <map>
#include <unordered_map>
#include <set>
#include <string>
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
	const std::set<CCircuitUnit*>& GetWorkers() const { return workers; }
	float GetJobSwitch() const { return jobSwitch; }
	// economy.builder_jobs.travel of priority, in seconds: a job farther than this walk is taken only with nothing
	// nearer (max: no cap)
	float GetJobTravel(IBuilderTask::Priority priority) const {
		return jobRanks[std::min(static_cast<int>(priority), 3)].travelSec;
	}
	void AddBuildPower(CCircuitUnit* unit);
	void DelBuildPower(CCircuitUnit* unit);
	float GetBuildPower() const { return buildPower; }
	float GetWorkerCost() const { return workerCost; }  // metal value of mobile constructors, commanders excluded
	bool CanEnqueueTask(const unsigned mod = 8) const { return buildTasksCount < workers.size() * mod; }
	const std::set<IBuilderTask*>& GetTasks(IBuilderTask::BuildType type) const;
	// Every tower job not started yet (a static attacker): each queue's jobs and the chains waiting behind them, whatever
	// heads them (a porc chain's towers wait behind a tower, a big gun hub's behind its radar)
	void ForEachQueuedTower(const std::function<void (IBuilderTask*)>& fn) const;
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
	// A superweapon at site; a cannon (Big Bertha, the Disco Rave Party) on its best spot around it if one passes
	// (CSpotScorer::FindGunSpot), held on the blocking map from now and built right there
	IBuilderTask* EnqueueBigGun(IBuilderTask::Priority priority, CCircuitDef* buildDef, const springai::AIFloat3& site);
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
	// builder is within <range> and the cell is safe: no known enemy on it or next to it, and no threat on it unless
	// our army covers it (IsRestoreSafe). A job whose cell turns unsafe (an enemy staying near for <grace> s, or
	// uncovered threat), whose builder's path there crosses uncovered threat, or whose builder is hit, is dropped and
	// its builders stop. A started job keeps going at half the cover.
	// Any drop of a job a builder took (also a builder hit or killed) backs its cell off (cooldown, doubling).
	// Cells with an allied building are skipped (levelled pads); an ally's terraform gives its cell up.
	// Every <log> s while anything is dug, a line counts why dug cells have no job and how jobs ended
	struct SRestore {
		static constexpr int CELL = 16;  // cell side in heightmap squares (128 elmos): one terraform segment each
		static constexpr int INTERVAL = FRAMES_PER_SEC * 2;  // UpdateRestore
		// Why a dug cell has no job (the first rule that stopped it), and why a job a builder took was dropped
		enum Skip: int {SKIP_HELD, SKIP_TRIES, SKIP_BUILDING, SKIP_ENEMY, SKIP_THREAT, SKIP_STALL, SKIP_FAR, SKIP_CAP,
			SKIP_COUNT};
		enum Drop: int {DROP_ENEMY, DROP_THREAT, DROP_PATH, DROP_HIT, DROP_OTHER, DROP_COUNT};  // other: killed,
		// switched to a nearer job, unreachable
		struct SCell {
			CBRestoreTask* task = nullptr;
			int until = -1;  // frame the cell is held off until
			int tries = 0;  // jobs done while it stays dug; reset once it is back and not held
			int backoff = 0;  // last hold after a drop, frames (0 = none since the last finished job)
			int enemyFrame = -1;  // frame a known enemy was seen on or next to it
			int enemySince = -1;  // first frame of that run of sightings (one per update)
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
		float cover;  // our armed power within coverRadius over the threat that makes it acceptable, 0 = any blocks
		float coverRadius;
		int grace;  // frames a known enemy may stay near a working job before it's dropped
		int logInterval;  // frames, 0 = no log
		int logFrame = 0;
		int done = 0;  // since the last log line
		int drops[DROP_COUNT] = {};
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
	// No threat at pos (the unit's own layer, or surface threat for nullptr), or our armed units and static defences
	// within restore.coverRadius have at least restore.cover x that threat (half that to keep a started job: isKeep)
	bool IsRestoreSafe(CCircuitUnit* unit, const springai::AIFloat3& pos, bool isKeep = false);
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
	void NoteRadarJob(const IBuilderTask* task, bool isStart);  // choke.radar: the radar planner's pending jobs

	bool IsReclaimed(CAllyUnit* unit) const { return reclaimedUnits.find(unit) != reclaimedUnits.end(); }

	/*
	 * turret.after_flee, turret.near_enemy: a light turret for a constructor in the field (CTurretPicker)
	 */
	// A turret job at pos for unit: IsCommJobSafe by its flee state (always, with its flee off)
	bool IsTurretJobSafe(CCircuitUnit* unit, const springai::AIFloat3& pos);
	// choke_overlay turret: our nearest constructor that is free (UpdateTurrets' tests) orders one at pos, def or the
	// picker's choice (nullptr); what happened, for the chat
	std::string OrderTurretAt(const springai::AIFloat3& pos, CCircuitDef* def);
	// turret.creep (CCreepPlanner): lane's step, a turret within radius of centre queued at HIGH for any builder
	// looking for work (MakeBuilderTask); the spot is searched for unit. Metal not stalling, a def unit can build
	// there, the account holding GetCreepCost. The turret.light defs, and each turret.tiers entry whose dearest def
	// the account's spare covers (past a waiting ask; whatever our mex share); isAtLine (on the target, or pulled back
	// by enemy range): the tiers our mex share unlocked too, waited for. The picker tries the heaviest first.
	// 1: queued, 0: wait (stall or budget), -1: no spot (the step failed), -2: no def
	int OrderCreepTurret(CCircuitUnit* unit, const springai::AIFloat3& centre, float radius, bool isAtLine, int lane);
	// Our ground-hitting defence job nearest centre within radius, queued or being built, raised to priority if
	// lower (a nanoframe of ours with no job gets a repair job); false if none
	bool PromoteTurretJob(const springai::AIFloat3& centre, float radius, IBuilderTask::Priority priority);
	// What the turret account must hold for a creep order from defs: turret.creep_budget x the dearest; < 0 if none
	float GetCreepCost(const std::vector<CCircuitDef*>& defs) const;
	// Not player-led, retreating, morphing, on a defence job or on a HIGH or NOW one
	bool IsTurretFree(CCircuitUnit* unit) const;
	// A static defence of ours or an ally's (a nanoframe or a queued job of ours too) within radius of centre, of a
	// kind canHit accepts; outPos its position
	bool HasTurretDefence(const springai::AIFloat3& centre, float radius,
			const std::function<bool (const CCircuitDef*)>& canHit, springai::AIFloat3& outPos) const {
		return FindTurretDefence(centre, radius, canHit, outPos) != DefenceFound::NONE;
	}
	// The same, telling what it found, the best of: a finished one, ours being built or queued, an ally's nanoframe
	enum class DefenceFound: char {NONE, ALLY_PENDING, OURS_PENDING, FINISHED};
	DefenceFound FindTurretDefence(const springai::AIFloat3& centre, float radius,
			const std::function<bool (const CCircuitDef*)>& canHit, springai::AIFloat3& outPos) const;
	bool IsCommHiding() const { return isCommHide || isCommHideAir; }  // hide mode: the creep leaves the commander be
	// commander.json jobs: a commander past jobs.time, with more than 2 builders (hide mode's guard), takes no new job
	// of a jobs.skip kind. false for any other builder
	bool IsCommKindOff(const CCircuitUnit* unit, IBuilderTask::BuildType type) const;
	// IsCommKindOff for the job's kind, except a stall fix (CEconomyManager::IsStallFix, latched) and a geothermal
	bool IsCommJobOff(const CCircuitUnit* unit, const IBuilderTask* task) const;
private:
	// Once a second: a constructor whose flee ended clear or covered (centred where the flee started), then each
	// mobile ground constructor by the first field trigger that holds for it: near enemy, short choke, enemy line,
	// artillery (doc/chokepoints.md, G). Gates in order: the caller's switch (after a flee: and
	// turret.after_flee_since passed); the constructor is free; its cooldown (turret.cooldown); metal not stalling;
	// the trigger; then TryTurret
	void UpdateTurrets();
	// A light def unit can build at centre (canHit: that can hit what it fled from); helping a turret of ours being
	// built, or a picker job, within turret.radius instead (in the field, not isAfterFlee: of the constructor too);
	// after a flee, no other defence that hits it within turret.defended_radius; the choke analysis (asked for now,
	// waited for while an after-flee ask lasts); the budget; one order a second. true: done (ordered, helped, or
	// nothing to have), false: try again next second. budgetMod: x the balance the budget asks for (the charge is
	// the full cost). isAlly: STurretOrder::isAlly
	bool TryTurret(CCircuitUnit* unit, const springai::AIFloat3& centre,
			const std::function<bool (const CCircuitDef*)>& canHit, bool isAfterFlee, const char* caller,
			bool& isOrdered, float budgetMod = 1.f, bool isAlly = true);
	// The light defs and the tiers isTierOpen passes, given a tier's defs and whether our mex share unlocked it
	// (nullptr: the unlocked ones)
	std::vector<CCircuitDef*> GetTurretDefs(CCircuitUnit* unit, const springai::AIFloat3& centre,
			const std::function<bool (const CCircuitDef*)>& canHit,
			const std::function<bool (const std::vector<CCircuitDef*>&, bool)>& isTierOpen = nullptr) const;
	bool AssistTurret(CCircuitUnit* unit, const springai::AIFloat3& centre,
			const std::function<bool (const CCircuitDef*)>& canHit, bool isHelpNear, const char* caller);
	// Asks for a turret and starts the cooldown, whatever the result. isAlly: STurretOrder::isAlly
	IBuilderTask* OrderTurret(CCircuitUnit* unit, const springai::AIFloat3& centre,
			const std::vector<CCircuitDef*>& defs, const char* caller, bool isAlly = true);
	// Can hit what a flee is from: a flier with anti-air, in deep water with anti-water, else with anti-land
	static bool IsHitFor(const CCircuitDef* def, bool isAir, bool isWater);
	int turretChokeFrame = -1;  // the first order's frame while the choke analysis isn't ready
	std::map<ICoreUnit::Id, int> turretCooldowns;  // unit id -> frame it may order again
public:

	/*
	 * Commander flee (always on) and hide (commander.json "hide")
	 */
	// DefaultMakeTask sends this armed builder to fight at home (IBuilderTask::Reevaluate re-plans on the same test)
	bool IsHomeFight(CCircuitUnit* unit);
	// An uncovered danger (recently seen ones included) within the wide (stop-fleeing) line: no morph now
	bool IsCommInDanger(CCircuitUnit* unit);
	/*
	 * Commander protect (quota.protect, CProtect): the commander's view of its dangers, and where it may flee to
	 */
	struct SCommView {
		float own = 0.f;  // its cover power (power x sqrt(health %))
		float danger = 0.f;  // seen dangers (gunships and moving blips included) within the radius, summed
		float airDanger = 0.f;  // the gunships' part of it
		int count = 0;
		springai::AIFloat3 dangerPos = -RgtVector;  // the nearest of them; invalid: none
		float dangerRange = 0.f;  // its weapon range
		bool isCornered = false;  // IsCommCornered
	};
	// false: no flee state for it (a flying builder, or its flee is off)
	bool GetCommView(CCircuitUnit* unit, float radius, SCommView& out);
	// It can move to pos, and pos is outside every seen flee zone (its stop-fleeing line, as a flee goal)
	bool IsCommFleeSpot(CCircuitUnit* unit, const springai::AIFloat3& pos);

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
		// turret.after_flee: where its last flee started and whether from a gunship; a turret request left when that
		// flee ended clear or covered (its frame, -1 = none)
		springai::AIFloat3 fleeFrom = -RgtVector;
		bool isFleeAir = false;
		int turretAskFrame = -1;
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
	// commander.json jobs per commander def, looked up once (GetCommJobs reads the def name)
	mutable std::unordered_map<CCircuitDef::Id, const CSetupManager::SCommInfo::SJobs*> commJobs;
	// An ally that covers a commander: a ground-capable armed mobile or a static defence, with its cover power
	struct SCommCover {
		ICoreUnit::Id id;
		springai::AIFloat3 pos;
		float power;
		const CCircuitDef* def;
	};
	static float GetCommWideMargin(float margin) { return margin + std::max(0.25f * margin, 50.f); }
	void AddComm(CCircuitUnit* unit);  // a state for a ground builder with the flee on
	void UpdateCommDangers();
	// Outside every seen flee zone (range + margin)
	bool IsCommSafe(const springai::AIFloat3& pos, float margin);
	void UpdateCommCovers();
	// Cover power within radius of pos, skipId left out (and allies whose def fails isCounted); stops summing once it
	// reaches enough
	float GetCommCover(const springai::AIFloat3& pos, float radius, ICoreUnit::Id skipId, float enough,
			const std::function<bool (const CCircuitDef*)>& isCounted = nullptr);
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
	// Commander protect: the protect's meeting point (turrets) for our commander while it has one
	// (CProtect::GetMeetingPoint). Asked only by its flee: a health retreat keeps its own goal
	bool GetProtectMeet(CCircuitUnit* unit, springai::AIFloat3& outPos) const;
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

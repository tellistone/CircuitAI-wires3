/*
 * MilitaryManager.h
 *
 *  Created on: Sep 5, 2014
 *      Author: rlcevg
 */

#ifndef SRC_CIRCUIT_MODULE_MILITARYMANAGER_H_
#define SRC_CIRCUIT_MODULE_MILITARYMANAGER_H_

#include "module/SpotScorer.h"
#include "module/UnitModule.h"
#include "setup/DefenceMatrix.h"
#include "task/fighter/FighterTask.h"
#include "task/static/ArtyMath.h"
#include "unit/CircuitDef.h"

#include <algorithm>
#include <array>
#include <vector>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace Json {
class Value;
}

namespace circuit {

class CGameTask;
class CEnemyUnit;
class CBDefenceTask;
class IBuilderTask;
class CRetreatTask;
class CAttackTask;
class CRaidTask;
class ISquadTask;
class CChokeDefence;
class CSensorPlanner;
class CCreepPlanner;
class CAllyCover;
class CAttackBalance;
class CProtect;
class CFrontline;
class CTurretPicker;
class CAATurretPicker;

// Attack group route kinds (quota.attack_path_dynamic_cost)
enum class AttackRoute: int { TARGET = 0, REINFORCE, FRONT, FLEE, CORNERED, _SIZE_ };

class CMilitaryManager: public IUnitModule {
public:
	friend class CMilitaryScript;

	CMilitaryManager(CCircuitAI* circuit);
	virtual ~CMilitaryManager();

private:
	void ReadConfig();
	// [[x, multiplier], ...] config curve (quota.*_map_size, response.<role>.income_curve) for utils::eval_curve
	void ReadCurve(const Json::Value& value, const std::string& key, const char* xName,
			std::vector<std::pair<float, float>>& curve) const { ReadCurve(circuit, value, key, xName, curve); }
	void Init();
public:
	static void ReadCurve(CCircuitAI* circuit, const Json::Value& value, const std::string& key, const char* xName,
			std::vector<std::pair<float, float>>& curve);
	// Seconds for this map: a number, or [[<map size>, <seconds>], ...] (linear between points, flat beyond the ends),
	// map size = sqrt(width x height) in 512-elmo map units (8x8 = 8); missing = defSec. economy.mex_escort,
	// economy.opening.time, porcupine.budget.since
	static float ReadMapSeconds(CCircuitAI* circuit, const Json::Value& value, const std::string& key, float defSec);
	// Map player density = map area / 2 / allied team size, sides in map units (512 elmos, the 16x16 players quote):
	// 16x16 1v1 = 128, 8x8 5v5 = 6.4. x of response.<role>.density_curve and factory.json income_tier_density
	static float GetMapDensity(CCircuitAI* circuit);
	void Release();

	virtual int UnitCreated(CCircuitUnit* unit, CCircuitUnit* builder) override;
	virtual int UnitFinished(CCircuitUnit* unit) override;
	virtual int UnitIdle(CCircuitUnit* unit) override;
	virtual int UnitDamaged(CCircuitUnit* unit, CEnemyInfo* attacker) override;
	virtual int UnitDestroyed(CCircuitUnit* unit, CEnemyInfo* attacker) override;
	virtual int UnitGiven(CCircuitUnit* unit, int oldTeamId, int newTeamId) override;

	const std::set<IFighterTask*>& GetTasks(IFighterTask::FightType type) const {
		return fightTasks[static_cast<IFighterTask::FT>(type)];
	}

	IFighterTask* EnqueueTask(IFighterTask::FightType type);
	// Commander attack (CBuilderManager::TryCommAttack): an attack group led by our commander, comm, at threat
	// allowance 1 / ratio. Updated with the other fight tasks but kept off GetTasks(ATTACK), so army picks, escorts,
	// ally-cover discs, the attack balance, guard pulls, rallies, reinforcing and protect never see it; other attack
	// groups may still merge into it (ISquadTask::CheckMergeTask, GetCommAttacks). The caller assigns comm to it
	CAttackTask* EnqueueCommAttack(CCircuitUnit* comm, float ratio);
	const std::set<IFighterTask*>& GetCommAttacks() const { return commAttacks; }
	// The commander left a commander attack that still has merged units: they go on as a normal attack group, with
	// a normal attack group's threat allowance (NewAttackPowerMod)
	void PromoteCommAttack(CAttackTask* task);
	// Registers a new fight task under type for updates and GetTasks
	IFighterTask* AddFightTask(IFighterTask::FightType type, IFighterTask* task);
	IFighterTask* EnqueueDefend(IFighterTask::FightType promote, float power);
	IFighterTask* EnqueueDefend(IFighterTask::FightType check, IFighterTask::FightType promote, float power);
	IFighterTask* EnqueueGuard(CCircuitUnit* vip);
	// quota.attack_merge.base_gather: an attack-bound group gathers at base until it has quota.attack power and this
	// metal value: the known enemy army's (as GetEnemyArmyRatio) per ally alive
	float GetAttackGatherMetal() const;
	// Attack group rank: more attack power, ties broken by the lower leader id. A strict order, so groups that each
	// walk to a higher-ranked one (reinforcing, quota.attack_merge.idle_merge) end at one group per movement class
	static bool IsRankedAbove(float power, const CCircuitUnit* leader, float otherPower, const CCircuitUnit* otherLeader);
	// The group led by `unit` may join `squad` (CAttackTask::IsCompatible) and can reach its leader; `squad` has one
	bool CanReinforce(const CAttackTask* squad, CCircuitUnit* unit) const;
	// Nearest attack group ranked above (`power`, `unit`) (IsRankedAbove) that the group led by `unit` may join and
	// can reach (CanReinforce); nullptr if none. `self` is skipped. With `isRally` the rally host comes first, wherever it is
	// (quota.attack_merge.rally); fleeing passes false and keeps the nearest stronger group.
	CAttackTask* FindReinforceSquad(CCircuitUnit* unit, float power, const IFighterTask* self = nullptr,
			bool isRally = true) const;
	// New attack task that walks to FindReinforceSquad(unit, power) and merges into it; nullptr if there is none
	IFighterTask* EnqueueReinforce(CCircuitUnit* unit, float power);
	// An attack-bound unit's first task: a gathering group at base (quota.attack_merge.base_gather true), else
	// reinforce the rally host or the nearest compatible group, else a new attack group
	IFighterTask* EnqueueAttackBound(CCircuitUnit* unit);
	// quota.solo_hunt: a one-unit defend task that hunts enemies on our ground and never merges or promotes, until
	// the one-off switch flips (IsSoloHuntOver); then the unit is attack-bound
	IFighterTask* EnqueueSoloHunt();
	bool IsSoloHuntRole(const CCircuitDef* cdef) const;
	bool IsSoloHuntOver() const { return soloHunt.isOver; }
	// quota.solo_hunt.choke_dist > 0: frontline chokes feed solo hunters' parking (CFrontline::Update builds them)
	bool IsSoloParkOn() const { return !soloHunt.isOver && (soloHunt.chokeDist > 0.f); }
	// Where a solo hunter with no target parks: a short choke of our line (CFrontline's C) whose inner region holds
	// one of our structures, by shortness minus distance / choke_dist minus choke_spread per other hunter parked
	// there; kept while the choke is on our line and was short within SOLO_PARK_KEEP.
	// -RgtVector: none (no choke map yet, none short or reachable)
	springai::AIFloat3 GetSoloPark(const IFighterTask* task, CCircuitUnit* leader);
	// A solo hunter left for a quota.protect commit: it goes back to hunting alone once its group stops protecting
	void NoteSoloProtect(CCircuitUnit* unit);
	// An anti-heavy unit's squad: one of its type nearby with room (CAntiHeavyTask::CanAssignTo), else a new one.
	// Mines always get a new one (each lays its field somewhere else)
	IFighterTask* EnqueueAntiHeavy(CCircuitUnit* unit);
	// antiheavy.as_artillery: anti-heavy units (main role) on the ground with a long reload (5 s or more), not mines or
	// melee (Lance, Merlin, Phantom, Racketeer), use the artillery task with heavies as targets (EnqueueAhArty) instead of squads
	bool IsAhArty(const CCircuitDef* cdef) const;
	IFighterTask* EnqueueAhArty();
	// Overkill check: the damage the ready shots of anti-heavy units (squads and IsAhArty) targeting enemy would do,
	// those of task skip aside
	float GetAhReadyDamage(CEnemyInfo* enemy, const IFighterTask* skip) const;
	// Where every anti-heavy group but skip stands or heads (IFighterTask::AddSpreadClaims): anti-heavy squads (not mines
	// or aircraft) and artillery-mode anti-heavies, so groups keep antiheavy.group_spacing apart and one strike can't
	// take several.
	// outRanks (if given): each claim's group (IFighterTask::GetSpreadRank); a group's claims come together
	void GetAhClaims(const IFighterTask* skip, F3Vec& outClaims, std::vector<int64_t>* outRanks = nullptr) const;
	// A mobile enemy heavy (enemy role, as GetEnemyCost; buildings don't count, they stay targets) in radar or LOS within
	// the last frames (CEnemyUnit::IsRecentlySeen). The army pull starts on a fresh sighting (AH_HUNT_FRAMES), and the hunt
	// lasts until none has been seen for antiheavy.keep_sec (IsAhHunting: squads disband, new units go to the army), so a
	// heavy dipping in and out of radar can't send the units back and forth
	static constexpr int AH_HUNT_FRAMES = FRAMES_PER_SEC * 10;
	bool IsEnemyHeavySeen(int frames);
	bool IsAhHunting() { return IsEnemyHeavySeen(antiHeavy.keepFrames); }
	// Every known mobile enemy heavy: where it was last seen, and how long ago (frames)
	struct SHeavySeen {
		springai::AIFloat3 pos;
		int age;
	};
	const std::vector<SHeavySeen>& GetHeavySightings() { IsEnemyHeavySeen(0); return heavySightings; }
	// A quarry of this unit (CAntiHeavyTask::IsQuarryFor) in radar or LOS within range
	bool IsQuarryWithin(CCircuitUnit* unit, float range) const;
	// Where an anti-heavy unit was sent home for repair: a park spot near it scores lower for a while (CAntiHeavyTask)
	void NoteAhRetreat(const springai::AIFloat3& pos);
	bool IsNearAhRetreat(const springai::AIFloat3& pos, float radius);
	/*
	 * The ANTIHEAVY log, every minute, per anti-heavy unit type: unit-seconds by activity (CAntiHeavyTask::GetActivity,
	 * sampled every second) and event counts, for comparing builds
	 */
	enum class AhNote: int {HUNT = 0, PARK, ENGAGE, FLEE, SHOT, RADAR_ONLY, FLEE_END, RETREAT, SCOOT, DROP, _SIZE_};
	void NoteAh(const CCircuitDef* cdef, AhNote note) { ++ahStats[cdef->GetId()][static_cast<int>(note)]; }
	CRetreatTask* EnqueueRetreat();
	// Static artillery with behaviour.json "target_tiers" (CPriorityArtyTask)
	IFighterTask* EnqueuePriorityArty();
	// EnemyDamaged with the attacker: progress for that attacker's priority artillery
	void OnEnemyDamaged(CEnemyInfo* enemy, int attackerId);
private:
	void DequeueTask(IUnitTask* task, bool done = false);

public:
	virtual IUnitTask* MakeTask(CCircuitUnit* unit) override;
	virtual void AbortTask(IUnitTask* task) override;
	virtual void DoneTask(IUnitTask* task) override;
	virtual void FallbackTask(CCircuitUnit* unit) override;

	// porcupine.geo: geothermal vents get defence points (CDefenceMatrix::AddGeoPoints) and our geos are defended
	bool IsGeoDefence() const { return prevent.isGeo; }
	void MakeDefence(const springai::AIFloat3& pos);
	void MakeDefence(int cluster);
	// sightDef: a sighting of that enemy near our mex at pos (UpdatePreventSight): the preventive roll can only
	// raise the count, and a half-taken cluster below 10 m/s stays out of porc mode.
	// isFirstMex: the cluster's first mex job under guard.summon (UpdateMexTasks): only the preventive towers, and
	// an unowned cluster counts as ours (porcupine.own_clusters)
	void MakeDefence(int cluster, const springai::AIFloat3& pos, CCircuitDef* sightDef = nullptr, bool isFirstMex = false);
	void AbortDefence(const CBDefenceTask* task);
	// Aborts an unstarted tower the AI no longer wants there (porc mode ended, artillery near a choke). Unlike a
	// tower that failed, its scored spot stays frozen and counts no failure
	void CallOffDefence(IBuilderTask* task);
	bool IsCallingOff() const { return isCallingOff; }
	bool HasDefence(int cluster);
	// MakeDefence's cap on a point's towers: MIN_BUILD_SEC x porcupine.amount x income
	float GetDefenceCap() const;

	/*
	 * porcupine.budget: one turret account for every armed defence the AI queues (CTurretPicker::IsDefence: no
	 * shields, storages, caretakers, radars, Claw mines, missiles or turret.not_defence statics), but preventive ones.
	 * It fills at share x avg metal income x min(economy ms_pull, 1) x map x mex (curves of map area per player and
	 * of ally mex share), up to bank_sec seconds of that (or the dearest
	 * tower a waiting spender asked for in the last 2 minutes). Every armed DEFENCE job is charged as it is queued
	 * (CBuilderManager::AddTask), by the spender a TurretScope names (OTHER when none does), and refunded if it is
	 * called off before it starts. Porc (porc-mode towers of a cluster or its chokes) and the picker wait for the
	 * balance to cover a tower (CanAffordTurret); the rest are charged but never refused and may take it below zero.
	 * Preventive towers (a cluster outside porc mode) are free, and only logged. A finished armed defence of ours that
	 * dies to a known mobile killer that doesn't outrange it (CanFightBack) pays loss_refund x its charge back, past the
	 * cap if need be (the account then stops filling until it is spent below the cap), so a lost position can be
	 * rebuilt
	 */
	enum class TurretSpend: int {OTHER = 0, PORC, PREVENT, PICKER, CORNERED, BASE, GUARD, ESCORT, HUB, CREEP, _SIZE_};
	class TurretScope {  // the spender of the DEFENCE jobs queued while it lives
	public:
		TurretScope(CMilitaryManager* mgr, TurretSpend spend) : mgr(mgr), prev(mgr->turretSpend) { mgr->turretSpend = spend; }
		~TurretScope() { mgr->turretSpend = prev; }
	private:
		CMilitaryManager* mgr;
		TurretSpend prev;
	};
	bool CanAffordTurret(float cost);
	// porcupine.budget.since: before it only the reactive spenders draw on the account (a turret near an enemy or
	// after a flee, a cornered builder's, an enemy mex's); planned towers (porc, base, hub, creep, radar guard, the
	// choke and artillery callers) wait
	bool IsTurretEarly() const;
	// The balance past what a waiting ask saves for (no ask recorded)
	float GetTurretSpare() { UpdateTurretBudget(); return turretBudget.balance - turretBudget.askCost; }
	void ChargeTurret(CBDefenceTask* task);
	void RefundTurret(CBDefenceTask* task);
	static const char* GetSpendName(int spend);  // a TurretSpend, as the TURRET BUDGET line names it
	// turret.crowd_count: true when a light def at pos would join enough light turrets; logs and credits the account
	// (CTurretPicker::RefuseCrowdedLight)
	bool RefuseCrowdedLight(const CCircuitDef* def, const springai::AIFloat3& pos, const char* caller);
	// turret.crowd_credit: a light turret not placed for its crowd (CTurretPicker::RefuseCrowdedLight). Never past the
	// account's cap; returns what it credited
	float CreditTurret(float metal);
	void UpdateTurretBudget();  // accrues; logs the spend once a minute
	float* GetTurretShare() { return &turretBudget.share; }  // choke_overlay set turret.share

	// Chokepoint features (CChokeMap::SConfig), each off by default
	CChokeDefence* GetChokeDefence() const { return chokeDefence.get(); }
	CSpotScorer* GetSpotScorer() const { return spotScorer.get(); }
	CSensorPlanner* GetSensorPlanner() const { return sensorPlanner.get(); }
	CCreepPlanner* GetCreepPlanner() const { return creepPlanner.get(); }
	CFrontline* GetFrontline() const { return frontline.get(); }
	CTurretPicker* GetTurretPicker() const { return turretPicker.get(); }
	// aa_turret: the AA turret picker, and whether it is on. While on it is the only route that queues AA turrets: the
	// porcupine lists' and the base timetable's AA entries are skipped, and a flier sighting gets no prevent_air tower
	CAATurretPicker* GetAATurretPicker() const { return aaPicker.get(); }
	bool IsAAPickerOn() const;
	// A picker or cornered tower (CBDefenceTask::IsOffPoints): its death takes no cost off a defence point
	void AddOffPointsTower(int unitId) { offPointsTowers.insert(unitId); }
	// porcupine.budget: a tower whose job the turret account charged (CBDefenceTask::SetTarget). Only these pay
	// loss_refund back when they die, and only on what they were charged: a free preventive tower pays nothing
	void AddChargedTower(int unitId, float charged) { chargedTowers[unitId] = charged; }
	springai::AIFloat3 GetScoutPosition(CCircuitUnit* unit);
	// Best spot by score (near the enemy, unseen for longest, not defended by known static enemies, not on our side);
	// -RgtVector if none. Scouts (quota.scout_spot) and raid groups (quota.raider_spot) score the same spots but
	// keep their own claims; a raid group's claim is held by its leader at pick time
	springai::AIFloat3 PickScoutSpot(CCircuitUnit* unit, float safeThreat) {
		return PickSpot(unit, safeThreat, scoutSpot, IFighterTask::FightType::SCOUT, scoutClaims);
	}
	springai::AIFloat3 PickRaidSpot(CCircuitUnit* leader, float safeThreat) {
		return PickSpot(leader, safeThreat, raiderSpot, IFighterTask::FightType::RAID, raidClaims);
	}
	// Gave up on this spot (danger at it or no safe route): nobody of that kind picks it for a while
	void BlockScoutSpot(const springai::AIFloat3& spot, int frames) { BlockSpot(scoutClaims, spot, frames); }
	void BlockRaidSpot(const springai::AIFloat3& spot, int frames) { BlockSpot(raidClaims, spot, frames); }
	// A scout keeps its claim fresh while it works the area of its pick (claim_time counts from the last call: it
	// lapses while the scout flees), and gives it up when it picks again (PickSpot) or leaves its scout task
	void HoldScoutSpot(const CCircuitUnit* unit);
	void ReleaseScoutSpot(const CCircuitUnit* unit);
	// A raid group keeps its spot's claim fresh while it works it. By spot, not by id: its leader may have changed since
	// the pick, and the current one takes the claim over
	void HoldRaidSpot(const CCircuitUnit* leader, const springai::AIFloat3& spot);
	// The raid group led by leader no longer heads for its spot (a raid rally joiner): free its claim for others
	void ReleaseRaidSpot(const CCircuitUnit* leader);
	bool IsRaidSpotOn() const { return isRaiderSpot; }  // quota.raider_spot given
	springai::AIFloat3 GetRaidPosition(CCircuitUnit* unit);
	void FillFrontPos(CCircuitUnit* unit, F3Vec& outPositions);
	// Defence points of every metal cluster we hold (queued or finished, not on enemy ground) the unit can reach
	void FillFriendlyClusterPos(CCircuitUnit* unit, F3Vec& outPositions);
	void FillAttackSafePos(CCircuitUnit* unit, F3Vec& outPositions);
	void FillStaticSafePos(CCircuitUnit* unit, F3Vec& outPositions);
	void FillSafePos(CCircuitUnit* unit, F3Vec& outPositions);

	struct SArtilleryCfg {
		float allyThreat;  // firing spot needs at least this power of allied mobile armed units (not artillery, not air) within allyRadius; < 0 = off
		float allyRadius;
		float maxPathThreat;  // paths to a firing spot may not cross enemy threat above this * unit's own power; < 0 = no limit
		float followDist;  // with a group, stand this far behind its leader (towards base)
		float fleeMargin;  // flee when an armed mobile enemy is within its range + this; < 0 = off
		bool isNoScout;  // nothing to shoot or walk to: join the nearest group instead of scouting enemy mexes
	};
	const SArtilleryCfg& GetArtilleryCfg() const { return artillery; }

	struct SFormationCfg {  // behaviour.json "formation" (CFormation)
		bool isEnable;  // missing block or false: squads walk as before
		int minUnits;  // land or sea members needed to form a line, 2 at least (off again below 2)
		float spacing, spacingUnits, spacingExp;  // spacing(n) = spacing * (spacingUnits / max(n, spacingUnits))^spacingExp
		float gap;  // elmos between footprints, and clearance at the line's ends
		float widthRange;  // max width = this x the squad's fire range
		float leadSec;  // the line stops when a unit is this many seconds behind its slot
		float faceRange;  // standing at its path's end, the line faces threat within this x the fire range; 0: off
		float splashSpacing;  // neighbours and rows at least this x the largest splash radius of armed enemies near; 0: off
		std::vector<CCircuitDef::RoleT> rowOrder;  // main roles front to back (unit types may override: formation_row)
	};
	const SFormationCfg& GetFormationCfg() const { return formation; }

	IFighterTask* AddGuardTask(CCircuitUnit* unit);
	bool DelGuardTask(CCircuitUnit* unit);
	IFighterTask* GetGuardTask(CCircuitUnit* unit) const;

	struct SGuardCfg {
		CCircuitDef::RoleM roleMask;  // main roles that may guard a builder (behaviour.json guard.roles)
		unsigned int maxWorkers;  // guard slot for an unarmed builder while worker count < this, 0 = off
		float safeAlly;  // builder is safe (guard leaves) if allied influence minus guards >= this, <= 0 = never safe
		float safeEnemy;  // ... and enemy influence <= this
		int safeFrames;  // builder must stay safe this long before its guard leaves
		float interposeDist;  // guard stands this far from its vip towards the nearest armed enemy, <= 0 = plain engine Guard
		float interposeRange;  // ... for enemies within this distance of the vip
		float pullRange;  // empty active slot pulls the nearest eligible unit within this range, <= 0 = no pull
		float threatRatio;  // ... only when threat at the guard > this * guard's own threat (0 = any threat)
		float guardedMaxShare;  // same as maxShare, separate pool for slots of "guarded" units
		float maxShare;  // max share of a role's units (by acting main role) on guard duty at once, rounded up; >= 1 = no limit
		int threatCooldown;
		bool isDamageExit;  // a guard that takes damage is released at once and the slot cools down (threat_cooldown)
		bool isDebug;  // log guard slot events and a 10 s status line per slot to infolog  // guard in enemy threat is cancelled and the slot closes for this long, < 0 = never cancel
		// guard.summon: a builder's slot opens as soon as it or its job is off safe ground (IsSafeGround) and outside
		// our bubble (guard.safe), and stays empty in either; a builder never waits for its guard (CBMexTask)
		bool isSummon;
		float minThreat;  // min_threat: an enemy whose type's threat is below this doesn't count (nor for builder flee)
		int enemyFrames;  // enemy_sec: an enemy seen this recently makes its region and enemy_radius around unsafe
		float enemyRadius;
		bool isFromGroups;  // from_groups: a raid or attack group (not fighting) gives up a unit, keeping quota.raid/attack
		float openingShare;  // opening_share: max_share during the opening (CEconomyManager::IsOpening)
		float escortedAllyThreat;  // escorted_ally_threat: flee_ally_threat of a builder while its guard is with it
	};
	const SGuardCfg& GetGuardCfg() const { return guard; }
	float GetRaidMinPower() const { return raid.min; }  // quota.raid[0]

	struct SAntiAirCfg {
		float leash;  // chase aircraft only within this distance of a home point (attack/defend group leader, base); < 0 = no leash
		bool isOwnGround;  // aircraft over ground where our influence beats the enemy's also count as near home
		float fireSpot;  // threat is checked where the AA would shoot from: this fraction of its range short of the aircraft; < 0 = at the aircraft
		float fleeMargin;  // flee when an armed mobile ground enemy is within its range + this of a squad unit; < 0 = off
		float fleeAllyThreat;  // no flee while our mobile armed units (not AA, not aircraft) within fleeAllyRadius of the leader have this much power; < 0 = always flee
		float fleeAllyRadius;
	};
	const SAntiAirCfg& GetAntiAirCfg() const { return antiAir; }
	struct SAntiHeavyCfg {
		float pathThreat;  // routes may not cross threat above this x the squad's allowance (as anti-air's cap); < 0 = no cap
		float fleeMargin;  // flee and pick-zone margins (CAntiHeavyTask::GetDangerStart, GetPickMargin); < 0 = off
		float fleeAllyThreat;  // no flee while cover at the leader, net of the enemies, has this much power; < 0 = always flee
		float fleeAllyRadius;  // escorts counted within this, while the cover map is off
		int keepFrames;  // keep_sec: the hunt lasts this long after the last mobile heavy sighting (max: while one is known)
		float spacing;  // the two units of a squad stand at least this far apart
		float groupSpacing;  // different anti-heavy groups keep this far apart (GetAhClaims); 0 = off
		int maxGroup;  // units in a squad when its type has no antiheavy_max_group; 0 = no limit
		bool isArtillery;  // as_artillery: IsAhArty
		float artyScoot;  // artillery_scoot: elmos an IsAhArty unit steps back after each shot; 0 = off
	};
	const SAntiHeavyCfg& GetAntiHeavyCfg() const { return antiHeavy; }
	struct SResignCfg {
		int frame;  // no resign before this frame; < 0 = never resign
		float mexShare;  // resign only while our ally team holds less than this share of the map's metal spots
		float threatMult;  // ... and the known enemy army threat is more than this x ours (or we have none)
		int holdFrames;  // ... on every check for this long
	};
	bool IsGuardRole(CCircuitDef* cdef) const;
	// guard.summon's safe ground: no enemy over min_threat seen lately in pos's region or near it, and the region is
	// ours (held ground, our side or our half), not the enemy's, nor contested. Before the choke map is ready: our half
	// with no enemy influence
	bool IsSafeGround(const springai::AIFloat3& pos);
	// The unit has a guard with it now (guard.summon: its flee takes escorted_ally_threat)
	bool IsEscorted(CCircuitUnit* unit) const;
	// isGuardedPool: the slot belongs to a "guarded" unit, which has its own share pool
	bool IsGuardCapReached(CCircuitDef* cdef, bool isGuardedPool) const;
	// A guard released by damage or threat can't take any guard slot until the ban ends
	void BanGuard(CCircuitUnit* unit, int frames);
	bool IsGuardBanned(CCircuitUnit* unit) const;

	const std::set<CCircuitUnit*>& GetRoleUnits(CCircuitDef::RoleT type) const {
		return roleInfos[type].units;
	}
	void AddResponse(CCircuitUnit* unit);
	void DelResponse(CCircuitUnit* unit);
	float GetArmyCost() const { return armyCost; }
	const std::set<CCircuitUnit*>& GetArmy() const { return army; }
	float RoleProbability(const CCircuitDef* cdef) const;
	// The main role's response gates (response.<role>.territory_max, unlock_after) let it be built now: false keeps it
	// out of both the response roll and the factory's tier-weight roll. Updated every 2 s by UpdateResponseGates
	bool IsRoleAllowed(const CCircuitDef* cdef) const;
	bool IsNeedBigGun(const CCircuitDef* cdef) const;
	springai::AIFloat3 GetBigGunPos(CCircuitDef* bigDef) const;
	// Not a grid consumer, or a grid with enough energy is in pylon reach of GetBigGunPos
	bool IsBigGunPowerable(CCircuitDef* cdef) const;
	void DiceBigGun();
	const CSpotScorer::SGunConfig& GetGunSpotConfig() const { return gunSpotConfig; }  // porcupine.superweapon.spot

	// Escalation mode (build_chain.json "escalation"): turns on when the allied team's mobile army is at least
	// army_ratio[0] x the known enemy one, an enemy unit of escalation.enemy is known, and (terraform: true) a
	// terraunit has been seen this game. Off as soon as the army falls below army_ratio[1] x; off when the other two
	// fail, but not within min_on s of turning on. Factories then build constructors only (while
	// CBuilderManager::IsEscalationBusy), and CBuilderManager::UpdateEscalation sends
	// builders to superweapons and singus at NOW. Updated every interval s
	bool IsEscalation() const { return escalation.isOn; }
	float GetEscalationBuilderShare() const { return escalation.builderShare; }  // most of our build power on it
	bool IsEscalationComm() const { return escalation.isComm; }  // commanders take escalation jobs
	bool IsEscalationSuper(const CCircuitDef* cdef) const;
	CCircuitDef* GetEscalationSingu() const { return escalation.singuDef; }
	// Weighted pick among escalation.super that are available and can be powered, the affordable ones (cost over the
	// lower of income and our escalation build power within max_build_sec) first; nullptr = none now
	CCircuitDef* DiceEscalationSuper(bool isFrontOk = true) const;
	int RollEscalationDelay() const;  // frames, random within escalation.delay
	// Where escalation builds superDef: escalation.front ones on our frontline cluster (finished, not enemy-held, the
	// nearest to the lane), the rest (or with no such cluster) at GetBigGunPos
	// isFrontOk false: the last front job of escalation went stale, place this one as the rest
	springai::AIFloat3 GetEscalationSuperPos(CCircuitDef* superDef, bool isFrontOk = true) const;
	bool IsEscalationFront(const CCircuitDef* cdef) const;
	int GetEscalationStaleFrames() const { return escalation.staleFrames; }
	float GetEscalationSinguShare() const { return escalation.singuShare; }
	// economy.ms_pull_army and quota.attack_threat_floor: metal value of the known enemy army
	// (EnemyManager::GetMobileCostNoCommFaded, faded as quota.enemy_half_life) per enemy team over our army's (no
	// constructors or commanders on either side); negative while no enemy army is known. Writes both values out
	float GetEnemyArmyRatio(float& enemyPerTeam, float& ours) const;
	void UpdateDefenceTasks();
	// Every 15s: count known enemy mexes each scout (and raider) movement type can reach from our base without
	// crossing threat it couldn't take, divided by the number of enemy players: the mex bonus of each quota
	void UpdateScoutQuota();
	// May a unit of cdef's movement type that scouts through the scout attribute take a scout slot: fewer of them are
	// scouting than quota.scout + bonus, or than quota.scout_share of them (rounded down), both capped by scout_max
	bool IsScoutSlotFree(const CCircuitDef* cdef) const;
	// Raid stint: a scout with no safe route to its spot raids (any raid group, merges and rallies included) until
	// endFrame, then its raid group hands it back to a new scout task (CRaidTask::UpdateScoutStints)
	void SetScoutStint(const CCircuitUnit* unit, int endFrame);
	void EndScoutStint(const CCircuitUnit* unit);
	int GetScoutStintEnd(const CCircuitUnit* unit) const;  // -1: not on a stint
	bool IsScoutStint(const CCircuitUnit* unit) const { return GetScoutStintEnd(unit) >= 0; }
	bool HasScoutStints() const { return !scoutStints.empty(); }
	bool IsScouting(const CCircuitUnit* unit) const;  // on a scout task or a raid stint
	bool IsQuotaScout(const CCircuitDef* cdef) const;  // scouts through the scout attribute, not its own role
	// quota.raider (+ bonus): factories build a raider while we have fewer raider units (main role raider or bound
	// to it, not scouting; alive plus queued) than this. 0 = no floor
	unsigned int GetMinRaiders(const CCircuitDef* cdef) const { return raiderQuota.Get(cdef->GetMobileId()); }
	bool IsRaiderQuotaOn() const { return raiderQuota.IsOn(); }
	bool IsRaider(const CCircuitDef* cdef) const;
	unsigned int GetRaiderCount() const;  // raider units in the army not scouting (factory tasks are counted by the caller)
	// Base towers (build_chain.json "base") once due, and the hub towers HoldHubTower holds: an account turret waits
	// until the turret account (porcupine.budget) covers it, asking it to save, for budget.wait_sec at most, then
	// goes anyway (the balance may go below zero). Each goes on its own: none waits behind another
	void UpdateDefence();
	void MakeBaseDefence(const springai::AIFloat3& pos);
	// A build chain's hub tower: true when it's an account turret the account can't pay for yet. It then waits, and
	// UpdateDefence queues it at LOW, while the building it guards (hub, nullptr: none) stands. isScored: pos is the
	// spot scorer's site, kept as is
	bool HoldHubTower(CCircuitDef* def, const springai::AIFloat3& pos, bool isScored, const CCircuitUnit* hub);
	// On the turret account (porcupine.budget): a defence that can hit ground or water. Anti-air-only turrets are free
	bool IsAccountTurret(const CCircuitDef* def) const;

	const std::vector<CCircuitDef*>& GetLandDefenders() const { return landDefenders; }
	const std::vector<CCircuitDef*>& GetWaterDefenders() const { return waterDefenders; }
	CCircuitDef* GetBigGunDef() const { return bigGunDef; }
	CCircuitDef* GetDefaultPorc() const { return defaultPorc; }

	void SetBaseDefRange(float range) { defence->SetBaseRange(range); }
	float GetBaseDefRange() const { return defence->GetBaseRange(); }
	float GetCommDefRadBegin() const { return defence->GetCommRadBegin(); }
	float GetCommDefRad(float baseDist) const { return defence->GetCommRad(baseDist); }

	// Raid groups attack these first (nearest wins): enemy types with the "raid_target" attribute,
	// or whose roles are in quota.raid_target_roles (default: builder, commander)
	bool IsRaidPreferred(const CCircuitDef* edef) const {
		return edef->IsAttrRaidTarget() || edef->IsEnemyRoleAny(raidTargetRoles);
	}

	float GetRaidFleeShare() const { return raidFleeShare; }
	float GetAttackFleeShare() const { return attackFleeShare; }
	// quota.raid_flee_grace / quota.attack_flee_grace (frames; 0 = off): an engaged group whose target still passes its
	// own check re-places its firing spots and pulls back endangered units this long before it flees (ISquadTask::IsInFleeGrace)
	int GetRaidFleeGrace() const { return raidFleeGrace; }
	int GetAttackFleeGrace() const { return attackFleeGrace; }
	// quota.danger_margin [<share>, <cost x>] (share 1 = off): attack and raid routes cost more from share x the escape's
	// allowance on, up to mod x at the allowance (cost 1 = routes off), and firing spots prefer spots under share x it
	// (IFighterTask::FindFireSpot)
	float GetDangerMargin() const { return dangerMargin; }
	float GetDangerMarginMod() const { return dangerMarginMod; }
	float GetAttackKeepThreat() const { return attackKeepThreat; }
	// quota.raid_attack_factor (default true): attack groups judge enemy raiders and scouts outside the base
	// defence radius and their fire range with their strength x their units' raid_thr_mod (CAttackTask::GetRaidFactor)
	bool IsRaidAttackFactor() const { return isRaidAttackFactor; }
	// For tuning raid_thr_mod (the RAID log, once a minute): damage taken by ground units of raid and scout tasks
	// (CThreatMap::IsRaidView), by attacker type (-1: unknown), and escapes of raid groups
	void AddRaidDamage(CCircuitUnit* unit, CCircuitDef::Id attackerDefId, float damage);
	void AddRaidEscape() { ++raidEscapes; }
	// quota.group_count_aoe: an armed type whose fire's area of effect (CCircuitDef::GetFireAoe, radius) is at least
	// this. A squad mostly of these doesn't count the enemies (IFighterTask::IsSquadThreat): one shot hits several of
	// a swarm
	bool IsAreaDamage(const CCircuitDef* cdef) const {
		return (countAoe > 0.f) && cdef->IsAttacker() && (cdef->GetFireAoe() >= countAoe);
	}
	// quota.attack_own_ground_time: before this frame attack groups only walk to targets on our ground
	int GetAttackOwnGroundFrame() const { return attackOwnGroundFrame; }
	// quota.attack_floor_hold: a group past quota.attack (the floor) stays past it until its power drops below
	// quota.attack x this; under the floor it only walks to targets on our ground (as attack_own_ground_time)
	float GetAttackFloorHold() const { return attackFloorHold; }
	// quota.attack_threat_floor [<on>, <off>]: every attack group only walks to targets on our ground (as
	// attack_own_ground_time) from when our army's threat drops below <on> x the known enemy mobile threat per enemy
	// team, until it is back to <off> x that. Commanders don't count on either side. Judged once a second
	bool IsAttackThreatFloor() const { return isAttackThreatFloor; }
	// quota.contested: clusters whose threat blocks mex expansion (CMetalManager::GetMexToBuild walls)
	struct SContested {
		float bonus;  // enemies there count as this many times closer (attack ranking, raid route goals); 1 = off
		int maxAge;  // frames a wall mark lasts without being seen again
		float range;  // elmos beyond the cluster's radius that still count as "there"
	};
	const SContested& GetContested() const { return contested; }
	struct SContestedSpot {
		springai::AIFloat3 pos;
		float sqRadius;
	};
	// Contested clusters the unit's group can take: threat at the centre above quota.contested.wall but below
	// allowedAt(centre, that threat) (squad threat for a squad), reachable within 'reach'. Empty when
	// quota.contested.bonus <= 1. allowedAt gets the threat it is compared with (CAttackTask: allied cover's band)
	void GetTakeableContested(CCircuitUnit* unit, float reach,
			const std::function<float (const springai::AIFloat3&, float)>& allowedAt,
			std::vector<SContestedSpot>& outSpots);
	static bool IsInContested(const std::vector<SContestedSpot>& spots, const springai::AIFloat3& pos) {
		for (const SContestedSpot& c : spots) {
			if (pos.SqDistance2D(c.pos) <= c.sqRadius) {
				return true;
			}
		}
		return false;
	}
	float GetAttackPathIgnoreThreat() const { return attackPathIgnoreThreat; }
	// quota.attack_ally_cover: allies over a spot multiply an attack group's strength there (CAllyCover)
	CAllyCover* GetAllyCover() const { return allyCover.get(); }
	// quota.attack_balance: the enemy forces that could join a fight soon against ours there (CAttackBalance)
	CAttackBalance* GetAttackBalance() const { return attackBalance.get(); }
	// quota.protect: one go or no-go for the groups that can reach our commander under a rush (CProtect)
	CProtect* GetProtect() const { return protect.get(); }
	// quota.attack_path_dynamic_cost: per route kind, threat cost = threat x (1 + threat / the allowance there)
	bool IsAttackRouteDynamic(AttackRoute route) const { return attackPathDynamic[static_cast<int>(route)]; }
	float GetAttackFleePathMaxWeight() const { return attackFleePathMaxWeight; }
	float GetThrScaleMin() const { return thrScaleMin; }
	float GetAttackSpotThreat() const { return attackSpotThreat; }
	// quota.kite: ranged units with firing spots keep out of this x their range of enemies with the "kite" attribute;
	// 0: off (IFighterTask::FindFireSpot)
	float GetKiteRange() const { return kiteRange; }
	// quota.attack_melee_spot: melee units pick a firing spot this many elmos beyond both units' bodies; < 0: off
	float GetAttackMeleeSpot() const { return attackMeleeSpot; }
	// quota.attack_melee_max: at most this many melee units of a group close in on one target (fewer if its ring
	// around the target has room for fewer); the rest take other targets near it. 0: off (all on the group's target)
	int GetAttackMeleeMax() const { return attackMeleeMax; }
	// quota.attack_melee_spread: other targets for those melee units, within this many elmos of the group's target
	float GetAttackMeleeSpread() const { return attackMeleeSpread; }
	// quota.attack_melee_blast: a target whose death explosion deals at least this x a melee unit's health is a
	// blast for it: the unit fires from a normal firing spot within weapon range, not right next to it; < 0: off
	float GetAttackMeleeBlast() const { return attackMeleeBlast; }
	// quota.escort: support units (role or "support" attribute) in attack groups. Armed ones are normal members, except
	// in a group whose core (the non-support units) holds coreShareOn of its power or more (until it drops below
	// coreShareOff, each change held modeHoldFrames; no core: off at once): there they are escorts, which in a fight
	// shoot only from a stay disc around the core (CAttackTask::OrderEscorts). Unarmed ones never get attack orders
	struct SEscortCfg {
		float stayRadius;  // the disc's radius around the core centroid
		float offsetCap;  // most the disc grows for an escort that out-ranges the group (its range - the group's)
		float keepMod;  // the current target is kept while it can be hit from the disc grown by this
		float coreShareOn;  // escort mode on at or above this share of the group's power held by its core
		float coreShareOff;  // and off below this
		int modeHoldFrames;  // a mode change must hold this long
		float speedMin;  // support units prefer hosts whose core's slowest speed is at most 1 / this x theirs
	};
	const SEscortCfg& GetEscortCfg() const { return escortCfg; }
	// quota.attack_merge: how attack groups join, absorb and reinforce each other
	struct SAttackMerge {
		float speedRatio;  // leaders' speeds within this factor of each other
		float distance;  // leaders within this many elmos may join or absorb
		float travelSec;  // and at most this many seconds of straight travel apart (the walking group's slowest speed)
		int joinSmall;  // a group under quota.attack checks for a group to join every this many updates (~1 s each)
		int joinArmy;  // an army checks every this many updates
		int absorb;  // an army absorbs small groups every this many updates
		float supportRadius;  // within this many elmos of the host's leader, both groups' strength counts on the merge line
		float reinforceRange;  // a reinforcing group's route only has to get this close to the host's leader
		int reinforceFails;  // empty reinforce routes in a row before giving up
		int reinforceRetry;  // frames to act as a small attack group after giving up, before looking again
	};
	const SAttackMerge& GetAttackMerge() const { return attackMerge; }
	// quota.attack_merge.idle_merge: an army with nothing to attack for a while reinforces the nearest compatible group
	// ranked above it (IsRankedAbove) and merges, so all attack groups converge into one army
	bool IsAttackIdleMerge() const { return attackIdleMerge; }
	// Rallies: quota.attack_merge.rally (attack groups vs blobs of armed enemies near them) and quota.raid_rally (raid
	// groups vs packs of enemy raiders anywhere, by default). When a recently seen blob has more threat than match x the
	// host's strength (with the group size bonus), compatible groups join the host (the strongest group of the
	// movement class with the most strength) until it is (1 + margin) x match x the blob, or no blob is found for
	// seen_sec. A rally lasts at least RALLY_MIN_FRAMES and the next starts RALLY_GAP_FRAMES after it. match 0 = off
	struct SRallyCfg {
		float match = 0.f;
		float margin = 0.25f;
		float range = 3000.f;  // a blob starts a rally only within this of a group's leader (keeps one up to 1.5x); 0 = anywhere
		int seenFrames = FRAMES_PER_SEC * 20;  // enemies count only if seen this recently (seen_sec)
		bool IsOn() const { return match > 0.f; }
	};
	// The attack rally's host while a rally is on (validated), else nullptr
	CAttackTask* GetRallyHost() const;
	// A rally is on and this group should reinforce its host: not the host, compatible with it
	bool IsRallyJoiner(const CAttackTask* task) const;
	// The rally host if the group led by unit (task self) may reinforce it: compatible and able to reach it; else nullptr
	CAttackTask* GetRallyHostFor(CCircuitUnit* unit, const IFighterTask* self) const;
	// quota.raid_rally: the host while a raid rally is on (validated), else nullptr
	CRaidTask* GetRaidRallyHost() const;
	// A raid rally is on and this raid group joins it: not the host, compatible with it and able to reach it
	bool IsRaidRallyJoiner(const CRaidTask* task) const;
	// While a raid rally is on: the host's goal (match x the pack x (1 + margin)); raid groups may grow up to it. Else 0
	float GetRaidRallyGoal() const;
	// quota.attack_group_radius: [<x the group's range>, <min elmos>]; units farther from the leader are stragglers
	const std::pair<float, float>& GetAttackGroupRadius() const { return attackGroupRadius; }
	// quota.attack_static_cells: attack groups route to safe firing cells around static targets
	struct SAttackCells {
		bool isOn;  // false: static targets are routed to like any other (a disc of the group's range)
		float rings[2];  // ring radii, x the group's fire range: the first is sampled first, the second only if it yields nothing
		int angles;  // points per ring
		int sectors;  // sectors around the target: one cell each at most; a route goal is a wedge of 180 / sectors degrees each side
		float threatWeight;  // cell score = threatWeight x threat / group strength + distance from the leader / fire range
		float scoreSlack;  // cells scoring worse than the best by more than this are left out
		float aimHeight;  // elmos above the ground the line-of-sight test aims from
		// Parked mobiles (parked_sec; 0: off): a mobile target still on one spot this long, no faster than about our
		// slowest unit, gets firing cells too (near sectors only)
		int parkedFrames;
		// siege_bonus: a mobile type no faster than 1.25 x our slowest unit with 1.2 x our fire range against ground
		// (artillery) ranks this many times closer, parked or not, while in view or just out of it (1: off)
		float siegeBonus;
	};
	const SAttackCells& GetAttackCells() const { return attackCells; }
	// Multiplier on a task's thr_mod by distance from our base (quota.thr_mod.distance.<task> curve); 1 if not configured
	float GetThrDistMod(IFighterTask::FightType type, const springai::AIFloat3& pos) const;
	// Highest value of the task's thr_mod.distance curve (1 if it has none)
	float GetThrDistModMax(IFighterTask::FightType type) const {
		const ThrDistCurve& curve = thrDistCurves[static_cast<IFighterTask::FT>(type)];
		float result = curve.empty() ? 1.f : 0.f;
		for (const auto& pt : curve) {
			result = std::max(result, pt.second);
		}
		return result;
	}
	// Courage bonus inside the base defence radius: quota.thr_mod.base_bonus at the base centre, falling to 1 at the radius edge
	float GetBaseThrBonus(float sqDistToBase) const;
	// Lowest value GetBaseThrBonus can return: 1, or quota.thr_mod.base_bonus if that is below 1
	float GetBaseThrBonusMin() const { return std::min(baseThrBonus, 1.f); }

	void MarkPointOfInterest(CEnemyInfo* enemy);
	void UnmarkPointOfInterest(CEnemyInfo* enemy);
	// One of our units killed an enemy (the engine's enemy-destroyed event; attackerId -1: none, or not ours): the
	// quota.attack_balance outcome log counts it for the fight the unit is in
	void NoteEnemyKilled(const CEnemyInfo* enemy, int attackerId);
	// A turret seen for the first time (CCircuitAI::EnemyEnterLOS): attack and raid groups within its reach + their path
	// look-ahead re-plan at the next threat-map update and get the flee grace for a while (ISquadTask::OnSuddenThreat)
	void ReactSuddenThreat(CEnemyInfo* enemy);

private:
	IUnitTask* DefaultMakeTask(CCircuitUnit* unit);
	// A new attack group's random powerMod: 0.8 / thr_mod.attack
	float NewAttackPowerMod() const { return 0.8f / ((float)rand() / RAND_MAX * attackMod.len + attackMod.min); }
	IFighterTask* FindGuardTask(CCircuitUnit* unit) const;

	void Watchdog();
	void UpdateIdle();
	void UpdateFight();
	void UpdateCaptureHide();
	// An enemy heavy seen within AH_HUNT_FRAMES: anti-heavy units in attack and gathering groups (where they go while
	// there is none) and support tasks leave for anti-heavy squads, every 2 s, if a heavy they can shoot is within their
	// weapon range + 30 s of walking (CAntiHeavyTask::GetHuntReach); not while their group is fleeing, and while it is fighting
	// only for one within their weapon range
	void UpdateAhPull();
	void UpdateAhStats();  // every second (NoteAh)
	void LogAhStats();
	void UpdateProtect();  // quota.protect, once a second
	void UpdateAttackRally();  // quota.attack_merge.rally, once a second
	void UpdateRaidRally();  // quota.raid_rally, once a second
	void UpdateAttackThreatFloor();  // quota.attack_threat_floor, once a second
	void UpdateResign();  // "resign": concede a lost 1v1, every 10 seconds
	void UpdateDrowned();  // "drowned": self-destruct units stuck in water too deep for them, every second
	void UpdatePlant();  // "plant" attribute: plant (Stop, hold position) and unplant units, every 8 frames
	void Unplant(CCircuitUnit* unit, bool isPlayer);
	void ReadPlantThreats(int frame);  // plantThreats: the "plant".threats enemies that count now
	enum class PlantReason: char {NONE, TARGET, THREAT};  // (a) a target in reach, (b) a threat has it in range
	PlantReason GetPlantReason(CCircuitUnit* unit, int frame);
	// Response gates and income multipliers (response.<role>.territory_max, unlock_after, income_curve), every 2 s
	void UpdateResponseGates();
	void UpdateAllyResponse();
	void UpdateEscalation();
	void LogRaidStats();  // the RAID log, every minute
	// One rally's state; attack and raid rallies share the rules (UpdateRally)
	struct SRally {
		SRallyCfg cfg;
		IFighterTask::FightType type;  // its groups' fight type
		bool isRaiders;  // blobs of enemy raiders only (CEnemyManager::GetBlobs)
		const char* tag;  // log prefix
		ISquadTask* host = nullptr;  // nullptr: no rally; cleared in DequeueTask when the task ends
		ISquadTask* prevHost = nullptr;  // host of the last rally (preferred on a restart); validated before use
		int startFrame = -1;  // < 0: no rally
		int endFrame = -FRAMES_PER_SEC * 60;  // a new rally can't start within RALLY_GAP_FRAMES of this
		int blobFrame = -1;  // last frame a qualifying blob was found
		springai::AIFloat3 blobPos;
		float blobThreat = 0.f;
		SRally(IFighterTask::FightType type, bool isRaiders, const char* tag);
		bool IsActive() const { return startFrame >= 0; }
	};
	ISquadTask* GetValidHost(const SRally& rally) const;  // the host, if it is still alive and led
	// Once a second while the rally is on in config: start, keep or end it
	void UpdateRally(SRally& rally);
	// Ground groups of the rally's type with a leader (air groups never rally)
	void CollectRallyGroups(const SRally& rally, std::vector<ISquadTask*>& outGroups) const;
	void ReplaceRallyHost(SRally& rally, const IFighterTask* gone);  // PickRallyHost among the other groups
	// Host candidate: the strongest group (raw strength) in the movement class with the most total strength;
	// prefer keeps that group if it is within 20% of the best
	ISquadTask* PickRallyHost(const std::vector<ISquadTask*>& groups, ISquadTask* prefer) const;
	// Ends the rally (log), starting the gap before the next one
	void EndRally(SRally& rally, const char* reason, int frame);
	// A fight task ended: the rally drops it (a host is replaced at once)
	void OnRallyTaskEnd(SRally& rally, const IFighterTask* task);

	void AddArmyCost(CCircuitUnit* unit);
	void DelArmyCost(CCircuitUnit* unit);

	Handlers2 createdHandler;
	Handlers1 finishedHandler;
	Handlers1 idleHandler;
	EHandlers damagedHandler;
	EHandlers destroyedHandler;

	std::vector<std::set<IFighterTask*>> fightTasks;
	std::vector<IUnitTask*> fightUpdates;  // owner
	std::set<IFighterTask*> commAttacks;  // EnqueueCommAttack: CAttackTasks off fightTasks
	unsigned int fightIterator;
	std::set<IUnitTask*> priorityArtyTasks;  // CPriorityArtyTask, in fightTasks[SUPER]

	CDefenceMatrix* defence;
	unsigned int defenceIdx;
	// MakeDefence: queues towers at point in order while its total stays under cap; porc towers in a chain. A choke
	// point's list (its share of its pocket, CChokeDefence) is walked again while the cap has room.
	// goal: where choke.spot.enable scores the attackers' spots, nullptr: today's shake around the point
	void QueueTowers(CDefenceMatrix::SDefPoint* point, const std::vector<CCircuitDef*>& towers, float cap, bool isPorc,
			const CSpotScorer::SGoal* goal);
	springai::AIFloat3 GetBackPos(const springai::AIFloat3& pos) const;  // 16 squares towards our base
	bool IsSpotTaken(const springai::AIFloat3& pos) const;  // a tower of ours stands there, or is queued there
	// A frozen spot of the type's at the point that no tower stands on or is queued at, else one scored and frozen
	// now; -RgtVector if none (or the type is given up there). used: spot indices this call queued at already
	springai::AIFloat3 GetTowerSpot(CDefenceMatrix::SDefPoint* point, CCircuitDef* defDef, const CSpotScorer::SGoal& goal,
			std::set<int>& used);
	// A task queued at the point or at one of its frozen spots
	bool IsPointTask(const CDefenceMatrix::SDefPoint& point, const springai::AIFloat3& taskPos) const;
	// choke.spot.enable: the porc sweep visits each point of a cluster we hold once per round, the most traffic
	// our towers don't cover yet x income first (scored as the round starts)
	void SweepByTraffic();
	std::vector<std::pair<int, unsigned>> sweepOrder;  // {cluster, point index}: this round's
	size_t sweepIdx;
	bool isCallingOff;  // CallOffDefence is aborting a tower: AbortDefence counts no spot failure
	std::unique_ptr<CChokeDefence> chokeDefence;
	std::unique_ptr<CSpotScorer> spotScorer;
	std::unique_ptr<CSensorPlanner> sensorPlanner;
	std::unique_ptr<CCreepPlanner> creepPlanner;  // turret.creep
	std::unique_ptr<CFrontline> frontline;
	std::unique_ptr<CTurretPicker> turretPicker;
	std::unique_ptr<CAATurretPicker> aaPicker;  // aa_turret
	std::set<int> offPointsTowers;  // unit ids
	std::unordered_map<int, float> chargedTowers;  // unit id: what the turret account charged its job

	std::vector<unsigned int> scoutPath;  // list of spot ids
	unsigned int scoutIdx;
	// Scout spots: metal spots (same index as CMetalManager), then known geothermal vents
	void UpdateScoutSpots();
	bool IsScoutSpotOurs(unsigned idx) const;  // our side holds it: finished mex / any geothermal on the vent
	std::vector<springai::AIFloat3> scoutSpots;
	unsigned scoutMetalCount = 0;
	std::vector<int> scoutSeen;   // per spot: last frame it was in our LOS (-1 never); shared, it is our LOS history
	// One kind of picker's hold on the spots (scouts, raid groups)
	struct SSpotClaims {
		std::vector<int> frame;    // per spot: last frame it was picked (-1 never)
		std::vector<int> unitId;   // per spot: who picked it (a scout, or a raid group's leader)
		std::vector<int> blocked;  // per spot: frame until which nobody of this kind picks it
		void Clear() { frame.clear(); unitId.clear(); blocked.clear(); }
		void Resize(size_t n) { frame.resize(n, -1); unitId.resize(n, -1); blocked.resize(n, -1); }
	} scoutClaims, raidClaims;
	// quota.scout_spot / raider_spot: spot scoring
	struct SSpotCfg {
		float enemy;      // weight: closeness to enemy
		float stale;      // weight: time since last seen
		float travel;     // weight (penalty): distance from the picker
		int staleTime;    // frames until a seen spot is fully stale again
		int claimTime;    // frames a picked spot is reserved for its picker
		float rangeMult;  // static enemy weapon range multiplier for danger
		float homeRatio;  // skip spots nearer to our base than this fraction of their distance to the enemy
		float mex;        // bonus x stale term: a known enemy mex no known defence covers, in a cluster nobody else of
		                  // this kind is heading to; only for pickers that can hit land targets (0 = off)
	} scoutSpot, raiderSpot;
	bool isRaiderSpot = false;  // quota.raider_spot given
	springai::AIFloat3 PickSpot(CCircuitUnit* unit, float safeThreat, const SSpotCfg& cfg, IFighterTask::FightType type,
			SSpotClaims& claims);
	void ReleaseSpot(SSpotClaims& claims, int unitId);  // the picker's claim, if any
	int FindSpot(const SSpotClaims& claims, const springai::AIFloat3& spot) const;  // -1 = not a spot
	void BlockSpot(SSpotClaims& claims, const springai::AIFloat3& spot, int frames);
	void HoldSpot(SSpotClaims& claims, int unitId);  // renew the picker's claim, if any

	struct SRaidPoint {
		unsigned int idx;
		int lastFrame;
		float weight;
		std::set<CEnemyInfo*> units;
	};
	std::vector<SRaidPoint> raidPath;  // list of cluster ids

	struct SRoleInfo {
		float cost;  // this AI's finished army and builders of the response role
		float allyCost = 0.f;  // allied teams' finished mobile units of the role (UpdateAllyResponse, every 2 s)
		float maxPerc;
		float densityMult;  // density_curve at the map player density (missing = 1): scales the response amount
		std::set<CCircuitUnit*> units;
		std::string name;  // response.<name>, for logs
		// Response gates (UpdateResponseGates). Share = finished ally mexes / map spots; income = factory income_tier
		// measure, min(metal, energy) * economy ecoFactor
		float territoryMax = 1.f;  // not built while share > this; 1 = off
		float unlockIncome = 0.f;  // unlock_after.income: one-off, income must exceed it (0 = met)
		float unlockTerritory = 0.f;  // unlock_after.territory: one-off, share must exceed it (0 = met)
		bool isUnlocked = true;  // false until unlock_after is met (held RESPONSE_HOLD samples), then for good
		bool isAllowed = true;  // unlocked and within territory_max, debounced
		int unlockHold = 0;  // consecutive samples unlock_after held
		int flipHold = 0;  // consecutive samples the wanted isAllowed differed from the current one
		std::vector<std::pair<float, float>> incomeCurve;  // income_curve: [income, multiplier] points; empty = 1
		float incomeMult = 1.f;  // incomeCurve at the last sample: scales the response amount
		struct SVsInfo {
			SVsInfo(CCircuitDef::RoleT t, float r, float i) : role(t), ratio(r), importance(i) {}
			CCircuitDef::RoleT role;
			float ratio;
			float importance;
		};
		std::vector<SVsInfo> vs;
	};
	std::vector<SRoleInfo> roleInfos;
	bool isResponseGates = false;  // some role has a gate or an income curve: UpdateResponseGates runs
	int responseSamples = 0;  // UpdateResponseGates calls so far (every 30th logs)

	std::set<CCircuitUnit*> army;
	float armyCost;
	struct SCapturedUnit {
		int controllerId;  // our unit that captured it
		float cost;
	};
	std::unordered_map<int, int> scoutStints;  // scout unit id -> frame its raid stint ends (SetScoutStint)
	std::unordered_map<int, SCapturedUnit> capturedUnits;  // captured unit id -> who holds it, for "capture_hide"
	SArtilleryCfg artillery;
	SFormationCfg formation;

	std::map<CCircuitUnit*, IFighterTask*> guardTasks;
	SGuardCfg guard;
	struct SRecentEnemy {
		springai::AIFloat3 pos;
		int region;
	};
	std::vector<SRecentEnemy> recentEnemies;  // IsSafeGround's, refreshed once a second
	int recentEnemyFrame = -1;
	std::map<int, int> guardBanUntil;  // unit id -> frame
	SAntiAirCfg antiAir;
	SAntiHeavyCfg antiHeavy;

	struct SRaidQuota {
		float min;
		float avg;
	} raid;
	// quota.scout / quota.raider and their _per_mex, _max and _map_size keys
	struct SUnitQuota {
		unsigned int min = 0;  // never cut, even above max
		unsigned int max = 0;  // cap on min + bonus (0 = none)
		float perMex = 0.f;    // multiplier on the reachable-mex bonus (0 = off)
		// [<map side in Spring map units (512 elmos)>, <multiplier>] points: second multiplier on the bonus
		std::vector<std::pair<float, float>> mapCurve;
		std::map<int, unsigned int> mexBonus;  // by mobile type id (-1 = air), from UpdateScoutQuota
		bool IsOn() const { return (min > 0) || (perMex > 0.f); }
		unsigned int Get(int mobileId) const {
			auto it = mexBonus.find(mobileId);
			const unsigned int count = min + ((it == mexBonus.end()) ? 0 : it->second);
			return (max > 0) ? std::max(std::min(count, max), min) : count;
		}
	} scoutQuota, raiderQuota;
	float scoutShare = 0.f;  // quota.scout_share: share of scout-attribute units (per movement type) that may always scout
	void UpdateMexBonus(SUnitQuota& quota, bool isRaider);  // UpdateScoutQuota for one quota
	float minAttackers;
	CCircuitDef::RoleM raidTargetRoles;
	float raidFleeShare;
	float attackFleeShare;  // quota.attack_flee_share
	int raidFleeGrace;  // quota.raid_flee_grace (frames)
	int attackFleeGrace;  // quota.attack_flee_grace (frames)
	float dangerMargin = 1.f;  // quota.danger_margin[0]
	float dangerMarginMod = 1.f;  // quota.danger_margin[1]
	float attackKeepThreat;  // quota.attack_keep_threat
	bool isRaidAttackFactor;  // quota.raid_attack_factor
	std::map<CCircuitDef::Id, float> raidDamage;  // AddRaidDamage, by attacker def id (-1: unknown type) since the last log
	int raidEscapes = 0;  // AddRaidEscape since the last log
	float countAoe;  // quota.group_count_aoe, elmos (radius); 0 = off
	int attackOwnGroundFrame;  // quota.attack_own_ground_time, in frames
	float attackFloorHold;  // quota.attack_floor_hold
	struct SThreatFloor {
		float on = 0.f;  // our army / enemy per team below this: floor on; 0 = off
		float off = 0.f;  // ... at or above this: floor off
	} attackThreatFloor;  // quota.attack_threat_floor
	bool isAttackThreatFloor;
	float enemyHalfLife;  // quota.enemy_half_life, frames; 0 = enemies out of sight never fade
	int attackThreatFloorLogFrame;  // next periodic THREAT FLOOR log line
	SContested contested;  // quota.contested
	float attackPathIgnoreThreat;  // quota.attack_path_ignore_threat
	std::unique_ptr<CAllyCover> allyCover;  // quota.attack_ally_cover
	std::unique_ptr<CAttackBalance> attackBalance;  // quota.attack_balance
	std::unique_ptr<CProtect> protect;  // quota.protect
	bool attackPathDynamic[static_cast<int>(AttackRoute::_SIZE_)];  // quota.attack_path_dynamic_cost
	float attackFleePathMaxWeight;  // quota.attack_flee_path_max_weight
	float attackSpotThreat;  // quota.attack_spot_threat
	float kiteRange;  // quota.kite
	float attackMeleeSpot;  // quota.attack_melee_spot
	int attackMeleeMax;  // quota.attack_melee_max
	float attackMeleeSpread;  // quota.attack_melee_spread
	float attackMeleeBlast;  // quota.attack_melee_blast
	SAttackMerge attackMerge;  // quota.attack_merge
	SEscortCfg escortCfg;  // quota.escort
	SRally attackRally;  // quota.attack_merge.rally
	SRally raidRally;  // quota.raid_rally
	SResignCfg resign;  // "resign"
	int resignSince = -1;  // first frame of the current "reads lost" run; -1 = doesn't read lost
	int drownedFrames;  // "drowned".time in frames; < 0 = off
	std::unordered_map<int, int> drownedSince;  // unit id -> first frame stuck in water; -1 = self-destruct ordered
	struct SPlantCfg {
		std::unordered_set<CCircuitDef::Id> threats;  // "plant".threats: enemy types that plant a unit in their range
		float reach;  // "plant".reach: (a) counts within this x its (height-adjusted) range
		int keepFrames;  // "plant".keep_sec in frames: a threat on radar counts this long after it left LOS
		int holdFrames;  // "plant".hold_sec in frames: a planted unit stays this long after the reason last held
		int idleFrames;  // "plant".idle_sec in frames: planted for a target only, weapon ready this long: (a) muted
	} plantCfg;
	std::vector<CEnemyUnit*> plantThreats;  // UpdatePlant: the threats that count now
	std::unordered_map<CCircuitDef::Id, ballistics::SShell> plantShells;  // per plant type: its main weapon's shell
	bool attackBaseGather;  // quota.attack_merge.base_gather
	// quota.solo_hunt: units of these main roles hunt alone on our ground until the ally team first holds mex x the
	// map's metal spots (finished mexes); one-off, it never switches back. No roles = off
	struct SSoloHunt {
		CCircuitDef::RoleM roleMask = 0;
		float mexShare = 0.25f;
		float chokeDist = 2000.f;  // choke_dist: elmos away that cost as much as a choke's whole shortness; 0 = no parking
		float chokeSpread = 1.f;  // choke_spread: shortness taken off per other hunter parked at the choke
		int maxFrame = 0;  // max_sec in frames: the switch flips by then whatever the mex share; 0 = no cap
		bool isOver = true;
	} soloHunt;
	struct SSoloPark {
		int chokeId;
		springai::AIFloat3 spot;
		int shortFrame;  // its choke was last short then: the claim holds SOLO_PARK_KEEP after
	};
	std::map<const IFighterTask*, SSoloPark> soloParks;
	std::set<int> soloProtectIds;  // NoteSoloProtect
	void UpdateSoloHunt();
	bool attackIdleMerge;  // quota.attack_merge.idle_merge
	int heavySeenFrame;  // IsEnemyHeavySeen cache: frame heavySeenAge was measured at
	int heavySeenAge;  // frames since an enemy heavy was last in radar or LOS; max = none
	std::vector<SHeavySeen> heavySightings;  // GetHeavySightings, for heavySeenFrame
	std::vector<std::pair<springai::AIFloat3, int>> ahRetreats;  // NoteAhRetreat: where and when
	std::map<CCircuitDef::Id, std::array<int, static_cast<int>(AhNote::_SIZE_)>> ahStats;  // NoteAh since the last log
	std::set<int> ahReady;  // anti-heavy units whose weapon was ready at the last UpdateAhStats (shots)
	std::pair<float, float> attackGroupRadius;  // quota.attack_group_radius
	SAttackCells attackCells;  // quota.attack_static_cells
	using ThrDistCurve = std::vector<std::pair<float, float>>;  // [fraction of map diagonal, thr_mod multiplier], sorted
	std::vector<ThrDistCurve> thrDistCurves;  // by IFighterTask::FightType
	float invMapDiagonal;  // 1 / the map's diagonal (thr_mod.distance curves take a fraction of it)
	float baseThrBonus;  // quota.thr_mod.base_bonus
	float thrScaleMin;  // quota.thr_mod.scale_min
	struct SThreatQuota {
		float min;
		float len;
	} attackMod, defenceMod, raidMod, scoutMod;

	std::vector<CCircuitDef*> defenderDefs;
	std::vector<CCircuitDef*> landDefenders;
	std::vector<CCircuitDef*> waterDefenders;
	using BuildVector = std::vector<std::pair<CCircuitDef*, int>>;
	BuildVector baseDefence;
	// Preventive defences: towers a cluster gets outside porc mode, the first <count> of landDefenders/waterDefenders.
	// chance = prevent x prevent_map x prevent_mex; count = its whole part, plus 1 with the odds of its fraction.
	// The count is the cluster's total, over all its defence points. Each cluster throws its dice once for the whole
	// ally team (CMetalManager::GetClusterDice), the first
	// time it is defended; later rolls (every prevent_reroll[0] s, and on a sighting: an enemy that can hit ground
	// within mex LOS of our mex there, at most once per prevent_reroll[1] s since its last roll or sighting) reuse
	// it, so the count only moves with the chance (the mex share). A sighting's roll can only raise the count.
	// A roll never cancels a tower already queued or built.
	struct SPrevent {
		float base;  // prevent
		int rerollFrames;  // prevent_reroll[0]
		int sightFrames;  // prevent_reroll[1]
		float mapX;  // map area in 512-elmo squares / players on our team
		float mapMult;  // prevent_map at mapX, fixed for the game
		std::vector<std::pair<float, float>> mexCurve;  // prevent_mex: x = ally-team finished mexes / all spots
		CCircuitDef* airDef;  // prevent_air: first tower after a sighting of a flying enemy, nullptr = off
		bool isGeo;  // porcupine.geo: geothermal vents are defended like mex clusters
		bool isOwnOnly;  // porcupine.own_clusters: team games, towers only on clusters we own (IsOurCluster)
		bool isAllyCover;  // porcupine.ally_cover: built towers near a cluster count towards its preventive total
	} prevent;
	// porcupine.own_clusters: in team games only clusters we own (CMetalManager::GetClusterOwner) get towers;
	// isFreeOurs: one nobody owns counts too
	bool IsOurCluster(int cluster, bool isFreeOurs = false) const;
	// porcupine.ally_cover, team games: metal of the towers of the ally team (ours too, nanoframes and stunned ones
	// included) that can hit ground (water when isWater) within range of a mex point of the cluster; 0 when off
	float GetBuiltCoverCost(int cluster, bool isWater) const;
	struct SPreventRoll {
		int count = -1;  // -1 = never rolled
		int rollFrame = -1;
		int sightFrame = -1;  // last sighting acted on
	};
	std::vector<SPreventRoll> preventRolls;  // by cluster
	// Rolls when due; sightDef != nullptr: a sighting (raise only; a flying one makes the first slot prevent.airDef)
	unsigned GetPreventCount(int cluster, CCircuitDef* sightDef, bool& outIsAir);
	void UpdatePreventSight();
	float amountFactor;
	struct STurretBudget {
		float share = 0.1f;
		float bankSec = 60.f;
		float lossRefund = 2.f;  // x the charge of a finished turret of ours that dies
		int waitFrames = FRAMES_PER_SEC * 120;  // budget.wait_sec: the most a base or hub turret waits for the balance
		int sinceFrame = FRAMES_PER_SEC * 120;  // budget.since (IsTurretEarly)
		float mapX = 0.f;  // map area in 512-elmo squares / players on our team
		float mapMult = 1.f;  // budget.map at mapX, fixed for the game
		std::vector<std::pair<float, float>> mexCurve;  // budget.mex: x = ally-team finished mexes / all spots
		float mexMult = 1.f;  // budget.mex now, refreshed every 10 s
		float mexX = 0.f;
		// budget.army: x = the metal of our finished account turrets / our army's (GetArmyCost)
		std::vector<std::pair<float, float>> armyCurve;
		float armyMult = 1.f;  // budget.army now, refreshed every 10 s; 1 before armySince
		int armySince = 0;  // budget.army_since, frames
		float armyX = 0.f;
		int curveFrame = -1;  // the next refresh of mexMult and armyMult
		float lost = 0.f;  // since the last log line: what the lost turrets cost, and what they paid back
		float lossPaid = 0.f;
		float balance = 0.f;
		float rate = 0.f;  // metal/s, as last filled
		int frame = -1;
		float askCost = 0.f;  // the dearest tower a gated spender asked for and couldn't afford
		int askFrame = -1;
		int logFrame = -1;
		float spent[static_cast<int>(TurretSpend::_SIZE_)] = {};  // since the last log line
		float refunded = 0.f;
		float crowdCredit = 0.f;  // since the last log line: turret.crowd_credit for light turrets not placed
	} turretBudget;
	TurretSpend turretSpend = TurretSpend::OTHER;
	CCircuitDef* radarDef;
	CCircuitDef* sonarDef;
	CCircuitDef* bigGunDef;
	CCircuitDef* defaultPorc;

	struct SSuperInfo {
		CCircuitDef* cdef;
		float weight;
	};
	std::vector<SSuperInfo> superInfos;
	CSpotScorer::SGunConfig gunSpotConfig;

	struct SEscalation {
		bool isEnabled = false;
		bool isOn = false;
		float armyOn = 2.f;  // allied mobile army metal / known enemy mobile army metal to turn on
		float armyOff = 1.5f;  // ... below which it turns off, min_on or not
		int minOnFrames = FRAMES_PER_SEC * 60;  // shortest time on
		int onFrame = -1;  // when it last turned on
		int sinceFrame = 0;  // never on before this game frame
		float maxBuildSec = 420.f;  // affordability of a superweapon
		float builderShare = 0.7f;
		float singuShare = 0.34f;  // most of builderShare on singus
		int staleFrames = FRAMES_PER_SEC * 60;  // an unplaced job with no builders this long is dropped
		bool isComm = false;
		bool isNeedTerra = true;  // a terraunit must have been seen
		std::set<CCircuitDef::Id> enemyDefs;  // enemy big guns, superweapons and singus
		std::set<CCircuitDef::Id> frontDefs;  // supers built on the frontline cluster
		std::map<CCircuitDef::Id, std::vector<CCircuitDef::Id>> counters;  // enemy def: the supers it makes useless
		std::set<CCircuitDef::Id> countered;  // supers left out of the pick: a counter of each is known (every update)
		std::vector<SSuperInfo> supers;  // what we start; also what we assist
		CCircuitDef* singuDef = nullptr;
		int delayMin = FRAMES_PER_SEC;  // random wait before starting our own
		int delayMax = FRAMES_PER_SEC * 20;
		int interval = FRAMES_PER_SEC * 2;
	} escalation;

	std::shared_ptr<CGameTask> defend;
	std::vector<std::pair<springai::AIFloat3, BuildVector>> buildDefence;
	struct SHubTower {
		CCircuitDef* def;
		springai::AIFloat3 pos;
		bool isScored;
		int hubId;  // -1: none
		const CCircuitDef* hubDef;  // with hubId: an id the engine gave a new unit isn't the hub
		int frame;  // held at
	};
	std::vector<SHubTower> hubTowers;  // HoldHubTower's
	void StartDefend();  // UpdateDefence every second, if not yet
	// A due base tower at pos, or a held hub tower: false while it waits for the account (see UpdateDefence)
	bool TryBaseTower(const springai::AIFloat3& pos, CCircuitDef* buildDef, int dueFrame);
	bool TryHubTower(const SHubTower& t);
	bool IsWaiting(const CCircuitDef* def, int sinceFrame);
};

} // namespace circuit

#endif // SRC_CIRCUIT_MODULE_MILITARYMANAGER_H_

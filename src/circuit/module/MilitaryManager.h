/*
 * MilitaryManager.h
 *
 *  Created on: Sep 5, 2014
 *      Author: rlcevg
 */

#ifndef SRC_CIRCUIT_MODULE_MILITARYMANAGER_H_
#define SRC_CIRCUIT_MODULE_MILITARYMANAGER_H_

#include "module/UnitModule.h"
#include "setup/DefenceMatrix.h"
#include "task/fighter/FighterTask.h"
#include "unit/CircuitDef.h"

#include <algorithm>
#include <vector>
#include <functional>
#include <set>
#include <unordered_map>

namespace Json {
class Value;
}

namespace circuit {

class CGameTask;
class CBDefenceTask;
class CRetreatTask;
class CAttackTask;
class CRaidTask;
class ISquadTask;

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
			std::vector<std::pair<float, float>>& curve) const;
	void Init();
public:
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
	// An anti-heavy unit's squad: one of its type nearby with room (CAntiHeavyTask::CanAssignTo), else a new one.
	// Mines always get a new one (each lays its field somewhere else)
	IFighterTask* EnqueueAntiHeavy(CCircuitUnit* unit);
	// An enemy heavy (enemy role, as GetEnemyCost) in radar or LOS within the last frames (CEnemyUnit::IsRecentlySeen).
	// Anti-heavy hunting starts on a fresh sighting (AH_HUNT_FRAMES: new units, the army pull) and stops once none
	// has been seen for much longer (AH_KEEP_FRAMES: squads disband), so a heavy dipping in and out of radar can't
	// send the units back and forth
	static constexpr int AH_HUNT_FRAMES = FRAMES_PER_SEC * 10;
	static constexpr int AH_KEEP_FRAMES = FRAMES_PER_SEC * 60;
	bool IsEnemyHeavySeen(int frames);
	CRetreatTask* EnqueueRetreat();
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
	// raise the count, and a half-taken cluster below 10 m/s stays out of porc mode
	void MakeDefence(int cluster, const springai::AIFloat3& pos, CCircuitDef* sightDef = nullptr);
	void AbortDefence(const CBDefenceTask* task);
	bool HasDefence(int cluster);
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
		int minUnits;  // land or sea members needed to form a line (off again below 2)
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
	};
	const SGuardCfg& GetGuardCfg() const { return guard; }

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
		float fleeMargin;  // flee when an armed mobile enemy (not the heavy being hunted, unless reloading) is within its range + this; < 0 = off
		float fleeAllyThreat;  // no flee while escorts within fleeAllyRadius of the leader, net of the enemies, have this much power; < 0 = always flee
		float fleeAllyRadius;
	};
	const SAntiHeavyCfg& GetAntiHeavyCfg() const { return antiHeavy; }
	struct SResignCfg {
		int frame;  // no resign before this frame; < 0 = never resign
		float mexShare;  // resign only while our ally team holds less than this share of the map's metal spots
		float threatMult;  // ... and the known enemy army threat is more than this x ours (or we have none)
		int holdFrames;  // ... on every check for this long
	};
	bool IsGuardRole(CCircuitDef* cdef) const;
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
	void DiceBigGun();
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
	bool IsQuotaScout(const CCircuitDef* cdef) const;  // scouts through the scout attribute, not its own role
	// quota.raider (+ bonus): factories build a raider while we have fewer raider units (main role raider or bound
	// to it, not scouting; alive plus queued) than this. 0 = no floor
	unsigned int GetMinRaiders(const CCircuitDef* cdef) const { return raiderQuota.Get(cdef->GetMobileId()); }
	bool IsRaiderQuotaOn() const { return raiderQuota.IsOn(); }
	bool IsRaider(const CCircuitDef* cdef) const;
	unsigned int GetRaiderCount() const;  // raider units in the army not scouting (factory tasks are counted by the caller)
	void UpdateDefence();
	void MakeBaseDefence(const springai::AIFloat3& pos);

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
	float GetAttackKeepThreat() const { return attackKeepThreat; }
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
	// Contested clusters the unit's group can take: threat at the centre above quota.contested.wall but below allowedAt(centre)
	// (squad threat for a squad), reachable within 'reach'. Empty when quota.contested.bonus <= 1
	void GetTakeableContested(CCircuitUnit* unit, float reach, const std::function<float (const springai::AIFloat3&)>& allowedAt,
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
	// quota.attack_path_dynamic_cost: per route kind, threat cost = threat x (1 + threat / the allowance there)
	bool IsAttackRouteDynamic(AttackRoute route) const { return attackPathDynamic[static_cast<int>(route)]; }
	float GetAttackFleePathMaxWeight() const { return attackFleePathMaxWeight; }
	float GetThrScaleMin() const { return thrScaleMin; }
	float GetAttackSpotThreat() const { return attackSpotThreat; }
	// quota.attack_melee_spot: melee units pick a firing spot this many elmos beyond both units' bodies; < 0: off
	float GetAttackMeleeSpot() const { return attackMeleeSpot; }
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

private:
	IUnitTask* DefaultMakeTask(CCircuitUnit* unit);
	IFighterTask* FindGuardTask(CCircuitUnit* unit) const;

	void Watchdog();
	void UpdateIdle();
	void UpdateFight();
	void UpdateCaptureHide();
	// An enemy heavy seen within AH_HUNT_FRAMES: anti-heavy units in attack and gathering groups (where they go while
	// there is none) and support tasks leave for anti-heavy squads, every 2 s; not while their group is fleeing, nor
	// while it is fighting unless such a heavy is within the unit's weapon range (IsQuarryInRange)
	void UpdateAhPull();
	bool IsQuarryInRange(CCircuitUnit* unit) const;
	void UpdateAttackRally();  // quota.attack_merge.rally, once a second
	void UpdateRaidRally();  // quota.raid_rally, once a second
	void UpdateAttackThreatFloor();  // quota.attack_threat_floor, once a second
	void UpdateResign();  // "resign": concede a lost 1v1, every 10 seconds
	void UpdateDrowned();  // "drowned": self-destruct units stuck in water too deep for them, every second
	// Response gates and income multipliers (response.<role>.territory_max, unlock_after, income_curve), every 2 s
	void UpdateResponseGates();
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
	unsigned int fightIterator;

	CDefenceMatrix* defence;
	unsigned int defenceIdx;

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
	} scoutSpot, raiderSpot;
	bool isRaiderSpot = false;  // quota.raider_spot given
	springai::AIFloat3 PickSpot(CCircuitUnit* unit, float safeThreat, const SSpotCfg& cfg, IFighterTask::FightType type,
			SSpotClaims& claims);
	void ReleaseSpot(SSpotClaims& claims, int unitId);  // the picker's claim, if any
	void BlockSpot(SSpotClaims& claims, const springai::AIFloat3& spot, int frames);

	struct SRaidPoint {
		unsigned int idx;
		int lastFrame;
		float weight;
		std::set<CEnemyInfo*> units;
	};
	std::vector<SRaidPoint> raidPath;  // list of cluster ids

	struct SRoleInfo {
		float cost;
		float maxPerc;
		float factor;
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
	std::unordered_map<int, SCapturedUnit> capturedUnits;  // captured unit id -> who holds it, for "capture_hide"
	SArtilleryCfg artillery;
	SFormationCfg formation;

	std::map<CCircuitUnit*, IFighterTask*> guardTasks;
	SGuardCfg guard;
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
	float attackKeepThreat;  // quota.attack_keep_threat
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
	bool attackPathDynamic[static_cast<int>(AttackRoute::_SIZE_)];  // quota.attack_path_dynamic_cost
	float attackFleePathMaxWeight;  // quota.attack_flee_path_max_weight
	float attackSpotThreat;  // quota.attack_spot_threat
	float attackMeleeSpot;  // quota.attack_melee_spot
	SAttackMerge attackMerge;  // quota.attack_merge
	SRally attackRally;  // quota.attack_merge.rally
	SRally raidRally;  // quota.raid_rally
	SResignCfg resign;  // "resign"
	int resignSince = -1;  // first frame of the current "reads lost" run; -1 = doesn't read lost
	int drownedFrames;  // "drowned".time in frames; < 0 = off
	std::unordered_map<int, int> drownedSince;  // unit id -> first frame stuck in water; -1 = self-destruct ordered
	bool attackBaseGather;  // quota.attack_merge.base_gather
	bool attackIdleMerge;  // quota.attack_merge.idle_merge
	int heavySeenFrame;  // IsEnemyHeavySeen cache: frame heavySeenAge was measured at
	int heavySeenAge;  // frames since an enemy heavy was last in radar or LOS; max = none
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
	// Each cluster rolls the first time it is defended, again once prevent_reroll[0] s have passed (the new count
	// replaces the old), and on a sighting (an enemy that can hit ground within mex LOS of our mex there, at most
	// once per prevent_reroll[1] s since its last roll or sighting; that roll can only raise the count).
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
	} prevent;
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
	CCircuitDef* radarDef;
	CCircuitDef* sonarDef;
	CCircuitDef* bigGunDef;
	CCircuitDef* defaultPorc;

	struct SSuperInfo {
		CCircuitDef* cdef;
		float weight;
	};
	std::vector<SSuperInfo> superInfos;

	std::shared_ptr<CGameTask> defend;
	std::vector<std::pair<springai::AIFloat3, BuildVector>> buildDefence;
};

} // namespace circuit

#endif // SRC_CIRCUIT_MODULE_MILITARYMANAGER_H_

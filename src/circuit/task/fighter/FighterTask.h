/*
 * FighterTask.h
 *
 *  Created on: Aug 31, 2015
 *      Author: rlcevg
 */

#ifndef SRC_CIRCUIT_TASK_FIGHTER_FIGHTERTASK_H_
#define SRC_CIRCUIT_TASK_FIGHTER_FIGHTERTASK_H_

#include "task/UnitTask.h"
#include "util/Defines.h"

#include <limits>
#include <map>

namespace circuit {

class CEnemyInfo;
class CRetreatTask;
class CAllyUnit;
class CCircuitDef;

class IFighterTask: public IUnitTask {
public:
	enum class FightType: char {RALLY = 0, GUARD, DEFEND, SCOUT, RAID, ATTACK, BOMB, MELEE, ARTY, AA, AH, SUPPORT, SUPER, _SIZE_};
	using FT = std::underlying_type<FightType>::type;

protected:
	IFighterTask(ITaskManager* mgr, FightType type, float powerMod, int timeout = ASSIGN_TIMEOUT);
public:
	virtual ~IFighterTask();

	virtual void AssignTo(CCircuitUnit* unit) override;
	virtual void RemoveAssignee(CCircuitUnit* unit) override;

	virtual void Update() override {}

	virtual void OnUnitIdle(CCircuitUnit* unit) override;
	virtual void OnUnitDamaged(CCircuitUnit* unit, CEnemyInfo* attacker) override;
	virtual void OnUnitDestroyed(CCircuitUnit* unit, CEnemyInfo* attacker) override;
protected:
	// The retreat task its damaged units go to (fighters: military's, combat builders: builder's)
	virtual CRetreatTask* EnqueueRetreat();
public:

	FightType GetFightType() const { return fightType; }
	const springai::AIFloat3& GetPosition() const { return position; }

	float GetAttackPower() const { return attackPower; }
	float GetAreaPower() const { return areaPower; }  // of the units that are area damage (CMilitaryManager::IsAreaDamage)
	// Group size bonus to the group's own power where it is compared against enemy threat (Lanchester's square
	// law): 1 + quota.group_count_power x count, so 10 units at 0.025 count as 1.25x. The no-argument form uses
	// the task's unit count. Squads compare it against squad threat (GetTaskThreatAt), which counts the enemies
	// the same way, so the bonus cancels out at equal numbers
	float GetCountPowerMod(size_t count) const;
	float GetCountPowerMod() const { return GetCountPowerMod(units.size()); }
	// Judges threat with the enemy unit count (squad threat, CThreatMap::GetSquadThreatAt): a squad (ISquadTask)
	// with no more than half its power from area damage. Area damage hits several of a swarm with one shot, so the
	// swarm's numbers don't count against it. Changes only when units join or leave
	bool IsSquadThreat() const { return isSquad && (areaPower * 2.f <= attackPower); }
	// The unit's task IsSquadThreat: its routes use the squad threat layers (CPathFinder)
	static bool IsSquadThreat(CCircuitUnit* unit);
	// Route this unit on the cloak (decloak-range) layer with no threat cap: it is cloaked (not under water without
	// sonar stealth) and its task trusts the cloak (CAntiHeavyTask: mines and the cloak_route attribute only)
	virtual bool IsCloakRoute(CCircuitUnit* unit) const;
	static bool IsCloakRouteUnit(CCircuitUnit* unit);
	// Enemy threat at pos as this task judges it: squad threat if IsSquadThreat, else the plain threat. Use it
	// wherever the task weighs threat; threatMap->GetThreatAt only where our side gets no group size bonus (one unit
	// judged alone)
	float GetTaskThreatAt(CCircuitUnit* unit, const springai::AIFloat3& pos) const;
	// The same without one enemy's threat there (CThreatMap::GetSquadThreatExcl), never below 0
	float GetTaskThreatExcl(CCircuitUnit* unit, const springai::AIFloat3& pos, float exclThreat) const;
	CEnemyInfo* GetTarget() const { return target; }
	void ClearTarget() { target = nullptr; }  // Only for ~CEnemyUnit
	void ReleaseTarget() { SetTarget(nullptr); }  // unbound too: for a task that ends (CMilitaryManager::DequeueTask)

	const std::set<CCircuitUnit*>& GetShields() const { return shields; }

protected:
	void SetTarget(CEnemyInfo* enemy);
	// The task's thr_mod multiplier from its distance-from-base curve (quota.thr_mod.distance.<task>), read at the spot
	// being judged (an enemy, a destination, a unit's own position): divide a threat allowance by it. 1 if not configured.
	float GetThrDistModAt(const springai::AIFloat3& pos) const;
	// Units move to a firing spot (FindFireSpot; melee: one right next to the target, else a safe one within weapon
	// range), then Attack the target.
	// isDirect: units without a spot (no usable range, or quota.attack_melee_spot < 0 for melee) get a direct Attack instead of a fight-move, so the target keeps priority.
	// The engine still lets weapons shoot other units while the ordered target can't be hit.
	void Attack(const int frame, const bool isDirect = false);
	// Firing spot for a unit: within its spot range (GetSpotRange) of the target, trading walking distance against
	// threat and against spots other units are heading to.
	// NONE: no usable range (old orders); SPOT: a safe spot; HOLD: every spot dangerous but the unit's own
	// spot is safe, or no spot has a line of fire and a direct walk there (GetUnitSightLifts); RISKY: nowhere safe, the
	// unit's spot neither: the least dangerous spot
	enum class FireSpot: char {NONE, SPOT, HOLD, RISKY};
	// Where a unit of the group is heading (its last firing spot if still usable, else where it stands) and its
	// health-scaled power (GetUnitStrengths): it backs up spots near there
	struct SFireClaim {
		CCircuitUnit* unit;
		springai::AIFloat3 pos;
		float power;
	};
	// Max range against the current target's kind, or against this enemy's
	float GetFireRange(CCircuitUnit* unit) const { return GetFireRange(unit, target); }
	float GetFireRange(CCircuitUnit* unit, const CEnemyInfo* enemy) const;
	// Where its firing spots go: GetFireRange; melee: GetCloseRange, or GetFireRange on a blast it can fire at from
	// outside (IsCloseSpot); 0 with melee spots off (no spot)
	float GetSpotRange(CCircuitUnit* unit) const { return GetSpotRange(unit, target); }
	float GetSpotRange(CCircuitUnit* unit, const CEnemyInfo* enemy) const;
	// Melee close spots: both bodies + quota.attack_melee_spot (never beyond the fire range); 0 if that is < 0 (off)
	float GetCloseRange(CCircuitUnit* unit, const CEnemyInfo* enemy) const;
	// Is enemy's death explosion a blast for this melee unit (quota.attack_melee_blast): it deals at least that x the
	// unit's health and reaches its close spots. Never for a mobile bomb, whose own death is its weapon
	bool IsMeleeBlast(CCircuitUnit* unit, const CEnemyInfo* enemy) const;
	// Does a melee unit close in on this enemy (close spots, with melee spots on)? Not on a blast its weapon reaches
	// past (a normal firing spot, as ranged units pick); on a blast it can't fire at from outside it still does
	bool IsCloseSpot(CCircuitUnit* unit, const CEnemyInfo* enemy) const;
	// How many melee units of this kind may close in on enemy at once: bodies round its close-spot ring, at most
	// quota.attack_melee_max
	int GetMeleeCap(CCircuitUnit* unit, const CEnemyInfo* enemy) const;
	// The melee units that Attack sends elsewhere than the group's target (quota.attack_melee_max). Kept: one at its
	// close spot by another enemy, until that dies. Then the nearest ones closing in on the target (those already on
	// it first) take it up to its cap, each of the rest the nearest other enemy in sight within attack_melee_spread
	// of it that it can hit and may chase, isn't a blast for it, has room and isn't banned (meleeBans). Units left
	// out (no such enemy) attack the target as before, over its cap
	void AssignMeleeTargets(const std::vector<SFireClaim>& claims, int frame,
			std::map<CCircuitUnit*, CEnemyInfo*>& outTargets) const;
	// Attack's order for a melee unit AssignMeleeTargets sent to another enemy: to a safe close spot next to it, the
	// same search as on the group's target. false: none (the enemy is banned for all our melee units a while), and
	// Attack orders the unit on the group's target instead
	bool AttackMeleeOther(std::vector<SFireClaim>& claims, size_t self, float safeThreat, CEnemyInfo* enemy,
			int timeout);
	// A unit Attack orders: its claim (index in claims), spot range, and target (the group's, or another for melee).
	// An escort (discRadius > 0): its pick, and a second one (alt, may be nullptr) to try if the first has no spot in
	// its stay disc round spotDiscPos (each with its own radius: the current target's is grown by keep_mod)
	struct SOrder {
		size_t claim;
		float range;
		CEnemyInfo* enemy;
		float discRadius = 0.f;
		CEnemyInfo* alt = nullptr;
		float altRadius = 0.f;
	};
	// Attack's orders, longest range first: units on the group's target, melee ones AssignMeleeTargets sent
	// elsewhere, and escorts on their picks (OrderEscorts), all in one claim pass
	void OrderOnTarget(std::vector<SFireClaim>& claims, std::vector<SOrder>& orders, int frame, bool isDirect,
			float safeThreat, int timeout);
	// An escort's order: a safe firing spot in its disc by its pick (else by alt), with a FightFrom (it never chases
	// out of the disc); false: none (both are skipped by this unit type a while, spotDiscBans)
	bool OrderInDisc(std::vector<SFireClaim>& claims, const SOrder& order, float safeThreat, int timeout);
	// An escort with nothing to hit from its disc: back into it (a Move to ESCORT_HOLD_IN of the radius) if outside,
	// else it holds where it stands (fires at whatever comes into range)
	void HoldInDisc(SFireClaim& claim, float discRadius, int frame, int timeout);
	// Can unit find a firing spot by enemy inside a disc of radius r round spotDiscPos? Spots lie at FIRE_RADIUS x
	// its range from the enemy (a distance test, before any FindFireSpot)
	bool IsDiscReach(CCircuitUnit* unit, const CEnemyInfo* enemy, float r) const;
	// isClose (melee's close spots): 0-0.95x GetCloseRange from the enemy; else 0.6-0.95x GetFireRange
	bool IsInFireBand(CCircuitUnit* unit, const springai::AIFloat3& pos, bool isClose, const CEnemyInfo* enemy) const;
	// Its last spot, if still in that band
	bool GetKeptFireSpot(CCircuitUnit* unit, bool isClose, const CEnemyInfo* enemy, springai::AIFloat3& outPos) const;
	// After Attack: every unit with spots (melee too) holds without one (HOLD: every spot dangerous, or none in sight)
	// and the target is out of their weapon range, or out of sight (GetUnitSightLifts). The group would stand still for
	// as long as it keeps this target
	bool IsAllHolding() const;
	// Lifts (elmos above the ground) for a line of fire to this enemy: from our units' aim, at the enemy.
	// false (default): no line of fire needed. With lifts, firing spots must have one judged from the ground there, and
	// a direct walk there up or down a cliff (FindFireSpot, IsAllHolding; CTerrainManager::IsLineClear, IsWalkClear)
	virtual bool GetSightLifts(const CEnemyInfo* enemy, float& outAimLift, float& outTargetLift) const { return false; }
	// The same for one unit: false too if its weapon arcs over terrain (CCircuitDef::IsHighTrajectory), as it fires
	// from below, unseen
	bool GetUnitSightLifts(CCircuitUnit* unit, const CEnemyInfo* enemy, float& outAimLift, float& outTargetLift) const;
	// Sent to a firing spot (isRiskyOnly: a RISKY one) within 3 s: no pull-back
	bool IsOnFreshSpot(CCircuitUnit* unit, int frame, bool isRiskyOnly) const;
	// Centre of the firing-spot arc around the target for the whole group (radians, atan2(z, x)); false (default):
	// each unit's own bearing from the target
	virtual bool GetFireArcBearing(float& outBearing) const { return false; }
	// For the unit of claims[self]; the other claims crowd spots and back them up. arcBearing: GetFireArcBearing
	// (nullptr: none), then the unit's own and kept spots count only if on that arc. isClose: melee's close spots
	// (GetSpotRange), else within weapon range (Attack tries this for melee too when no close spot is safe)
	FireSpot FindFireSpot(const std::vector<SFireClaim>& claims, size_t self, float safeThreat,
			const float* arcBearing, bool isClose, const CEnemyInfo* enemy, springai::AIFloat3& outPos) const;

	// Each unit's health-scaled power (pwr_mod power x sqrt(health %)), in the order of 'units'. With shields in
	// the group, power has the unit's share of their charges (GetShieldShares) in place of its own full shield.
	// Health takes engine calls, so it is read once per frame, and again when the group changed or a unit was hit
	struct SUnitStrength {
		CCircuitUnit* unit;
		float power;
	};
	const std::vector<SUnitStrength>& GetUnitStrengths() const;
	// Per unit (order of 'units'): its share of the group's shields (each full charge split over the armed units
	// inside it, as the threat map splits enemy shields); empty when the group has no shields
	std::vector<float> GetShieldShares(int frame) const;
	float GetStrengthSum() const;  // sum of GetUnitStrengths
	// Group strength the task's escape compares threat against; <= 0: no escape, firing spots aren't limited
	virtual float GetEscapeSafeThreat() const { return -1.f; }
	int attackRepeat = FRAMES_PER_SEC * 3;  // re-send attack orders this often (frames)
	bool isFireSpotChase = true;  // at a firing spot: true = Attack order (engine may chase), false = fight from the spot
	// HOLD with the target in weapon range: true = an attack on it from the unit's spot (it shoots that target first,
	// even on hold fire, and stays there after), false = fight at the spot (whatever comes into range)
	bool isHoldAttack = false;
	// > 0: a unit already ordered on the target is re-ordered for a tile change only once the target moved
	// this far (elmos) from where it was at the last orders; the repeat timer still applies
	float retargetMove = -1.f;
	springai::AIFloat3 lastOrderTargetPos;
	float fireSpotReach = -1.f;  // > 0: units farther than this x their fire range from the target get no attack order and keep travelling
	float crowdRadius = 80.f;  // other claims within this of a firing spot crowd it (FindFireSpot)
	// May a unit fire from p (FindFireSpot skips the spot if not)? Default: yes. Anti-heavy: outside its pick zones
	virtual bool IsFireSpotAllowed(const springai::AIFloat3& p) const { return true; }
	// While isSpotDisc: FindFireSpot only weighs spots within the disc (an escort's stay disc, CAttackTask)
	bool isSpotDisc = false;
	springai::AIFloat3 spotDiscPos;
	float spotDiscSqRadius = 0.f;
	// Enemies with no spot in an escort's disc lately, per unit type: skipped by that type until then (compared only)
	std::map<std::pair<const CEnemyInfo*, int>, int> spotDiscBans;
	bool IsDiscBanned(const CEnemyInfo* enemy, const CCircuitDef* cdef) const;
	// Units Attack doesn't order on the group's target (quota.escort). Default: unarmed support units (shields), which
	// never get attack orders; CAttackTask adds its escorts
	virtual bool IsEscortUnit(CCircuitUnit* unit) const;
	// Escorts' orders: unarmed ones are moved, armed ones get an SOrder with a disc in orders (or hold). Before the
	// pass, so they claim spots in it with the others. isRepeat: the repeat timer ran out. Default: none
	virtual void OrderEscorts(std::vector<SFireClaim>& claims, std::vector<SOrder>& orders, int frame, bool isRepeat,
			int timeout) {}
public:
	// Has a real weapon: an attacker that isn't a mobile bomb (a unit without a weapon whose wide death explosion
	// CCircuitDef counts as one: a shield's would)
	static bool IsArmedUnit(const CCircuitDef* cdef);
	// Support (role or "support" attribute) without a real weapon: Aspis, Funnelweb
	static bool IsUnarmedSupport(const CCircuitDef* cdef);
protected:
	// Attack skips this unit (no attack order, keeps travelling). Default: fireSpotReach
	virtual bool IsOutOfReach(CCircuitUnit* unit, int frame) const;
	// Attack leaves this unit alone: it is stepping out of the target's reach while it reloads (ISquadTask scoot)
	virtual bool IsScooting(CCircuitUnit* unit) const { return false; }
	// Firing spot each unit was last sent to (pointers only compared, never dereferenced)
	struct SFireSpot {
		springai::AIFloat3 pos;
		int frame;  // when it was last ordered
		bool isRisky;
	};
	std::map<CCircuitUnit*, SFireSpot> fireSpots;
	// Other enemies with no safe close spot by them lately, until when (AttackMeleeOther; pointers only compared)
	std::map<const CEnemyInfo*, int> meleeBans;
	// Threat allowed at a spot for this strength (tasks with an escape override it)
	virtual float GetAllowedThreatAt(const springai::AIFloat3& pos, float strength) const {
		return std::numeric_limits<float>::max();
	}
	// The same, for a caller that compares it with threat (threat at or above it is danger): a task may skip work at
	// spots where that work can't change the comparison (CAttackTask: allied cover), so the result there may differ
	// from GetAllowedThreatAt but compares the same
	virtual float GetAllowedThreatFor(const springai::AIFloat3& pos, float strength, float threat) const {
		return GetAllowedThreatAt(pos, strength);
	}
	// Threat at p (GetTaskThreatAt) reaches what this strength allows there. No threat is never danger (allowances
	// are above 0), and the allowance is only worked out where there is threat
	bool IsDangerAt(CCircuitUnit* unit, const springai::AIFloat3& p, float strength) const;

	FightType fightType;
	springai::AIFloat3 position;  // attack/scout position

	float attackPower;
	float areaPower = 0.f;  // GetAreaPower
	float powerMod;
	int attackFrame;
	bool isSquad = false;  // an ISquadTask (IsSquadThreat)
	// NOTE: Never assign directly, use SetTarget() to avoid access to a dead target
	CEnemyInfo* target;

	std::set<CCircuitUnit*> cowards;
	std::set<CCircuitUnit*> shields;

	static F3Vec urgentPositions;  // NOTE: micro-opt
	static F3Vec enemyPositions;  // NOTE: micro-opt

public:  // flee helpers, also used by the commander flee (CBuilderManager::UpdateCommFlee)
	// AA flee cover: strength a friendly unit lends, health-scaled as squad strength is
	// (power * sqrt(health %)); 0 for nanoframes
	static float GetCoverPower(CAllyUnit* ally);
	// Flee cover: a static unit that defends against ground or water units nearby. The threat map's rule:
	// a ground/water threat range, which static AA and superweapons (range over 2000) don't get
	static bool IsStaticDefence(CCircuitDef* cdef);
	// Flee: an enemy we see now (not a last-known ghost) that can act and hit ground or water units: a mobile
	// armed unit that isn't anti-air only (mobile bombs count), not flying (isGunship: gunships count, planes
	// never do), or an unidentified radar blip that moves (a parked building blip is no danger).
	// outRange: its ground/water weapon range (a bomb's blast radius), 0 for a blip
	static bool IsGroundDanger(CEnemyInfo* enemy, bool isGunship, float& outRange);
	// IsGroundDanger's unit type test alone (no visibility or state checks)
	static bool IsGroundDangerDef(CCircuitDef* edef, bool isGunship, float& outRange);
	// Flee: the nearest spot further from the danger than pos is, else the one furthest from it; nullptr if none
	static const springai::AIFloat3* PickFleeSpot(const springai::AIFloat3& pos, const springai::AIFloat3& dangerPos,
			const F3Vec& spots);

private:
	static constexpr int STALE_FRAME = std::numeric_limits<int>::min();  // never a frame
	mutable std::vector<SUnitStrength> unitStrengths;  // GetUnitStrengths cache
	mutable float strengthSum = 0.f;
	mutable int strengthFrame = STALE_FRAME;  // frame it was read at

#ifdef DEBUG_VIS
public:
	virtual void Log() override;
#endif
};

} // namespace circuit

#endif // SRC_CIRCUIT_TASK_FIGHTER_FIGHTERTASK_H_

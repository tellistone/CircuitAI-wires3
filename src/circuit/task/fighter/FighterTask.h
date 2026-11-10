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

	virtual void Update() override;

	virtual void OnUnitIdle(CCircuitUnit* unit) override;
	virtual void OnUnitDamaged(CCircuitUnit* unit, CEnemyInfo* attacker) override;
	virtual void OnUnitDestroyed(CCircuitUnit* unit, CEnemyInfo* attacker) override;

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
	// Enemy threat at pos as this task judges it: squad threat if IsSquadThreat, else the plain threat. Use it
	// wherever the task weighs threat; threatMap->GetThreatAt only where our side gets no group size bonus (one unit
	// judged alone)
	float GetTaskThreatAt(CCircuitUnit* unit, const springai::AIFloat3& pos) const;
	// The same without one enemy's threat there (CThreatMap::GetSquadThreatExcl), never below 0
	float GetTaskThreatExcl(CCircuitUnit* unit, const springai::AIFloat3& pos, float exclThreat) const;
	CEnemyInfo* GetTarget() const { return target; }
	void ClearTarget() { target = nullptr; }  // Only for ~CEnemyUnit

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
	// spot is safe; RISKY: nowhere safe, the unit's spot neither: the least dangerous spot
	enum class FireSpot: char {NONE, SPOT, HOLD, RISKY};
	// Where a unit of the group is heading (its last firing spot if still usable, else where it stands) and its
	// health-scaled power (GetUnitStrengths): it backs up spots near there
	struct SFireClaim {
		CCircuitUnit* unit;
		springai::AIFloat3 pos;
		float power;
	};
	float GetFireRange(CCircuitUnit* unit) const;  // max range against the current target's kind
	// Where its firing spots go: GetFireRange; melee: both bodies + quota.attack_melee_spot (never beyond the fire
	// range), 0 if that is < 0 (no spot)
	float GetSpotRange(CCircuitUnit* unit) const;
	// isClose (melee's close spots): 0-0.95x GetSpotRange from the target; else 0.6-0.95x GetFireRange
	bool IsInFireBand(CCircuitUnit* unit, const springai::AIFloat3& pos, bool isClose) const;
	bool GetKeptFireSpot(CCircuitUnit* unit, bool isClose, springai::AIFloat3& outPos) const;  // last spot, if still in that band
	// After Attack: every unit with spots (melee too) holds without one (HOLD: every spot dangerous) and the target is
	// out of their weapon range. The group would stand still for as long as it keeps this target
	bool IsAllHolding() const;
	bool IsOnRiskySpot(CCircuitUnit* unit, int frame) const;  // sent to a RISKY spot within 3 s: no pull-back
	// Centre of the firing-spot arc around the target for the whole group (radians, atan2(z, x)); false (default):
	// each unit's own bearing from the target
	virtual bool GetFireArcBearing(float& outBearing) const { return false; }
	// For the unit of claims[self]; the other claims crowd spots and back them up. arcBearing: GetFireArcBearing
	// (nullptr: none), then the unit's own and kept spots count only if on that arc. isClose: melee's close spots
	// (GetSpotRange), else within weapon range (Attack tries this for melee too when no close spot is safe)
	FireSpot FindFireSpot(const std::vector<SFireClaim>& claims, size_t self, float safeThreat,
			const float* arcBearing, bool isClose, springai::AIFloat3& outPos) const;

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
	// > 0: a unit already ordered on the target is re-ordered for a tile change only once the target moved
	// this far (elmos) from where it was at the last orders; the repeat timer still applies
	float retargetMove = -1.f;
	springai::AIFloat3 lastOrderTargetPos;
	float fireSpotReach = -1.f;  // > 0: units farther than this x their fire range from the target get no attack order and keep travelling
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
	// Threat allowed at a spot for this strength (tasks with an escape override it)
	virtual float GetAllowedThreatAt(const springai::AIFloat3& pos, float strength) const {
		return std::numeric_limits<float>::max();
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

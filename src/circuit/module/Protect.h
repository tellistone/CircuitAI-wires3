/*
 * Protect.h
 *
 *  Created on: Nov 21, 2026
 *      Author: Claude
 */

#ifndef SRC_CIRCUIT_MODULE_PROTECT_H_
#define SRC_CIRCUIT_MODULE_PROTECT_H_

#include "unit/CoreUnit.h"
#include "util/Defines.h"
#include "util/Point.h"

#include "AIFloat3.h"

#include <unordered_map>
#include <vector>

namespace circuit {

class CCircuitAI;
class CCircuitDef;
class CCircuitUnit;
class CEnemyInfo;
class IFighterTask;
class ISquadTask;

/*
 * Commander protect (quota.protect): one central go or no-go decision for the groups that can reach our commander when
 * an enemy force threatens it, so they never judge that fight one by one and trickle in. Updated once a second.
 * 1. Anchor: the commander's seen danger (CBuilderManager's danger list within the zone) beats its own power, or it
 *    took damage, or seen planes with a ground weapon (bombers) within the zone and heading for it beat its anti-air
 *    bubble (its own power if it hits air, and the anti-air turrets covering it). It ends keep_sec after the last such
 *    second. Zone: zone elmos around the commander, mobile armed enemies (and moving radar dots) only.
 * 2. Danger: the force balance's enemy force at the commander (CAttackBalance::GetForceAt: mobiles by how soon they can
 *    shoot it, statics by their cover falloff), or the commander's own list (gunships on their way too) plus, in air
 *    mode only, those planes at their threat before thr_mod (held 5 s after they last headed for it), the larger. An
 *    air danger: aircraft over 60% of the list and the planes, until under 40%. Then anti-air groups are sent instead
 *    of ground ones, only anti-air turrets count, and the commander only if it hits air.
 * 3. Meeting point: a spot just behind our or allied turrets near the commander (away from the danger) when the static
 *    cover there plus the commander beats the danger, the commander reaches it before its time to death, and it is a
 *    safe flee spot (outside the seen flee zones, farther from the danger than the commander). Kept while the cover
 *    holds 1 / quota.attack_keep_threat of the danger and it can get there. The commander flees to it when it flees,
 *    and stays once at it (cornered there, it ends its flee and builds its defence). Without one it flees as it would.
 *    Its flee ignores planes, so planes alone don't move it.
 * 4. Commit: groups of the danger's kind (ground: attack, raid and gathering groups; air: anti-air groups, flying ones
 *    too) that can reach the commander within respond_sec (or its time to death if shorter, checked when a group
 *    joins), not fleeing or fighting elsewhere, soonest there first, until groups + commander + static cover reach
 *    1.25x the danger (or the go line + 0.25). Ratio = that sum / danger. Go at go_ratio with a group at least (0.6 x
 *    it once cornered: no escape or paralysed, latched for the anchor's life); no-go below the go line /
 *    quota.attack_keep_threat, and at once when the danger turns to the other kind (one kind per go). 5 s per state at
 *    least. On go the committed groups are latched until the anchor ends or no-go (a group that flees meanwhile still
 *    counts: a short flee doesn't end the fight), or until one is 1.5 x respond_sec of travel away; they merge only
 *    into each other.
 * 5. Go: committed groups head for the commander, the commander's recent attackers first. Ground groups only take zone
 *    enemies (and those in contact), which pass the own-ground check and the 3-group cap; anti-air groups count the
 *    commander as a leash home point. In the fight (within half the zone of the commander, kept to 0.625: the present
 *    radius) they judge with the strength present (GetBoost: the commander, the static cover where it stands, the
 *    committed groups present and themselves), with no allied cover on top.
 */
class CProtect {
public:
	enum class State: char {OFF = 0, NOGO, GO};
	struct SConfig {
		bool isEnabled = false;
		float zone = 1200.f;  // elmos around the commander
		int keepFrames = FRAMES_PER_SEC * 12;  // keep_sec
		float respondSec = 12.f;  // travel time
		float goRatio = 1.f;
	};

	CProtect(CCircuitAI* circuit, const SConfig& cfg);
	~CProtect();

	bool IsEnabled() const { return cfg.isEnabled; }

	void Update();  // once a second
	void OnCommDamaged(CCircuitUnit* comm, CEnemyInfo* attacker);

	bool IsActive() const { return state != State::OFF; }
	// A committed group while the state is go: it acts on the protect
	bool IsActing(const IFighterTask* task) const {
		return (state == State::GO) && (committed.find(task) != committed.end());
	}
	// A mobile armed enemy (or a moving radar dot) inside the zone, while the state is go
	bool IsZoneEnemy(const CEnemyInfo* enemy) const;
	// It hit the commander within the last 5 s
	bool IsRecentAttacker(ICoreUnit::Id id, int frame) const;
	// What an acting group's own strength is multiplied by where it judges the commander's fight: in the present
	// radius, (the strength present, with itself) / its own, at least 1; elsewhere, or for any other task, 1
	float GetBoost(const IFighterTask* task, const springai::AIFloat3& pos) const {
		return (state == State::GO) ? GetGoBoost(task, pos) : 1.f;  // the usual path (every threat check): one compare
	}
	// A committed group's units went over to another task (CDefendTask's promotion, ISquadTask::Merge): that one is
	// committed instead
	void Transfer(const IFighterTask* from, const IFighterTask* to);
	// A task ended (CMilitaryManager::DequeueTask): it is dropped at once, so no committed pointer outlives its task
	void OnTaskEnd(const IFighterTask* task);
	// Where acting groups head: the commander
	const springai::AIFloat3& GetGoal() const { return commPos; }
	// Where the commander flees: the turret point; invalid: none (always while off). Asked only while it flees (not on
	// a health retreat)
	const springai::AIFloat3& GetMeetingPoint() const { return turretPos; }
	// pos stands at the meeting point
	bool IsAtMeetingPoint(const springai::AIFloat3& pos) const;

private:
	struct SCandidate {
		ISquadTask* task;
		float travelSec;  // GetTravelSec
	};
	struct STurret {
		springai::AIFloat3 pos;
		float power;
		const CCircuitDef* def;
		bool isDefence;  // against ground (IFighterTask::IsStaticDefence)
		bool isAntiAir;  // against aircraft, within MAX_TURRET_RANGE
	};
	struct SCommit {
		float strength = 0.f;  // health-scaled power (ISquadTask::GetRallyStrength)
		bool isPresent = false;  // its leader in the present radius (IsInFight), not fleeing
	};
	void Start(int frame);
	void End(const char* reason, int frame);
	void SetState(State value, const char* reason, int frame);
	float GetGoBoost(const IFighterTask* task, const springai::AIFloat3& pos) const;
	float GetSum() const { return committedSum + own + staticCover; }  // the ratio's numerator
	void UpdateDeathTime(CCircuitUnit* comm, int frame);
	// Seen planes with a ground weapon in the zone heading for the commander (or that just hit it): planeDanger
	void UpdatePlanes(int frame);
	void CollectTurrets(int frame);  // both kinds
	float GetStaticCover(const springai::AIFloat3& pos, bool isAir, float range) const;  // range: the attacker's
	float GetStaticCover(const springai::AIFloat3& pos, bool isAir) const {
		return GetStaticCover(pos, isAir, dangerRange);
	}
	// The spot just behind a turret, away from the danger, where the commander can stand (else the turret's own)
	springai::AIFloat3 GetBehind(CCircuitUnit* comm, const springai::AIFloat3& turret) const;
	// A new turret point: strong enough, reached in time, away from the danger and a safe flee spot
	bool IsTurretSpotValid(CCircuitUnit* comm, const springai::AIFloat3& spot, float cover) const;
	// The turret point kept: 1 / attack_keep_threat of the danger, and reachable (the chasers near it are the point)
	bool IsTurretSpotHeld(CCircuitUnit* comm, const springai::AIFloat3& spot, float cover) const;
	void UpdateTurretPoint(CCircuitUnit* comm, int frame);
	// The present radius: half the zone around the commander; once in it (wasPresent), 0.625 of it
	bool IsInFight(const springai::AIFloat3& pos, bool wasPresent) const;
	float GetTravelSec(const ISquadTask* task, int frame) const;  // its leader to the commander, at the group's pace
	void CollectCandidates(int frame, std::vector<SCandidate>& out) const;
	void Refresh(const ISquadTask* task, int frame, SCommit& commit);  // its strength and flags, added to the sums
	// Committed groups refreshed, and new ones while the sum is short of need
	void UpdateCommitted(int frame, float need);

	CCircuitAI* circuit;
	SConfig cfg;

	State state;
	int startFrame;
	int stateFrame;
	int lastDangerFrame;
	int damagedFrame;
	bool isCornered;  // latched for the anchor's life
	springai::AIFloat3 commPos;
	springai::AIFloat3 dangerPos;  // the nearest seen danger, kept from the last update that saw one
	bool isAirDanger;  // aircraft over 60% of the commander's danger list and the planes heading for it, until under 40%
	bool isAirGo;  // the kind of the go: air (anti-air groups) or ground, set when it starts
	float airShare;  // the aircraft's share of that danger, kept while it is empty (logged)
	float peakAirShare;  // the highest airShare of this protect (the end line)
	int goCount;  // goes in this protect (the end line)
	float dangerRange;
	float planeDanger;  // UpdatePlanes: their threat before thr_mod (CCircuitDef::GetRawThreat)
	int planeCount;
	float own;  // the commander's power (0 against an air danger it can't hit)
	float danger;
	float staticCover;  // at the turret point, else where the commander stands
	float commCover;  // where the commander stands
	float committedSum;  // the committed groups
	float presentSum;  // own + commCover + the present committed groups
	float ratio;
	float deathSec;  // estimated time to death, std::numeric_limits<float>::max() while unhurt
	float prevHealth;
	int prevHealthFrame;
	float dps;  // smoothed damage rate
	springai::AIFloat3 turretPos;  // the meeting point (GetBehind a turret); invalid: none
	std::vector<STurret> turrets;  // CollectTurrets, each update
	std::unordered_map<const IFighterTask*, SCommit> committed;
	std::unordered_map<ICoreUnit::Id, int> attackers;  // enemy id -> frame it last hit the commander
	std::unordered_map<ICoreUnit::Id, int> planes;  // UpdatePlanes: plane id -> frame it last headed for the commander
};

} // namespace circuit

#endif // SRC_CIRCUIT_MODULE_PROTECT_H_

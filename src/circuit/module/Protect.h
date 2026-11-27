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

#include <algorithm>
#include <limits>
#include <map>
#include <string>
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
 * Protect (quota.protect): one central go or no-go decision, per threatened ward, for the groups that can reach it, so
 * they never judge that fight one by one and trickle in. Updated once a second.
 * 0. Wards: our commanders (role comm, with the builder flee), and anti-heavy squads (not flying ones) and artillery
 *    tasks holding a unit with the "protect" attribute. Those within 600 elmos of each other are one ward (and stay
 *    one until 800 apart). A ward's value is its members' summed metal cost; its goal is its costliest commander,
 *    else its costliest member. Its identity is kept while its members change (merged: the ward in a go, else on,
 *    else the oldest).
 * 1. Anchor: a commander's seen danger (CBuilderManager's danger list within the zone) beats its own power, or it
 *    took damage; a group member is outnumbered at the goal by the force balance's mobile enemies, at
 *    attack_balance.ratio (1 if off) x ours (our and allied mobiles nearby at 0.75: an army standing by holds it;
 *    the groups the protect sent there left out, as they would end it on arrival); or seen planes with a ground
 *    weapon (bombers) within the zone and heading for the goal beat its anti-air bubble (its anti-air members' power
 *    and the anti-air turrets covering it). It ends keep_sec after the last such second. Zone: zone elmos around
 *    the goal.
 * 2. Danger, in two sides answered together. Ground: its commanders' list without its gunships (the largest of
 *    them), or the force balance's enemy force at the goal (CAttackBalance::GetForceAt: mobiles by how soon they can
 *    shoot it, statics by their cover falloff) without its aircraft, the larger. Air: the list's gunships (a group
 *    ward: the force's aircraft) and those planes at their threat before thr_mod (held 5 s after they last headed
 *    for it). Its members count on the ground side, and on the air side if they hit air; ground turrets on the
 *    ground side, anti-air turrets on the air side.
 * 3. Meeting point, for a ward with a commander: a spot just behind our or allied turrets near the goal (away from
 *    the danger) when the static cover there plus the ward beats the danger (the ground side against its part and
 *    the gunships the anti-air turrets there leave; air only: the anti-air turrets against the air part), its lead
 *    commander reaches it before its time to death, and it is a safe flee spot. Kept while that holds
 *    1 / quota.attack_keep_threat and it can get there. Its commanders flee to it, and stay once at it. Group wards
 *    keep their own retreat.
 * 4. The pick, by side (air first, as its answer sets the gunship spill): goes keep their groups, then groups of the
 *    side's kind (ground: attack, raid and gathering groups; air: anti-air groups, flying ones too) that can reach a
 *    ward within respond_sec (a commander ward: or its time to death if shorter), not fleeing or fighting elsewhere,
 *    go to the best ward by value / (travel sec + 3 s): wards in a go first, then the others, each until groups +
 *    members + static cover reach 1.25x its danger (or the go line + 0.25). The ground side adds the gunships the
 *    air side leaves unanswered, to its ratio and to its commit up to its danger again. A ward in a no-go still
 *    under its go line after the pick hands its groups back, the lowest value first, and the others pick again; then
 *    those handed back try alone, the highest value first. A group latched to a go and not yet present moves, once,
 *    to a ward that scores it at least twice as high and is in a go (or can go now and reaches its go line with
 *    it), if the go it leaves stays above its keep line. Go at go_ratio with a group at least (0.6 x it once
 *    cornered); no-go below the go line / quota.attack_keep_threat. 5 s per
 *    state at least, each ward and side on its own. On go the groups are latched until the anchor ends or its
 *    no-go, or until one is 1.5 x respond_sec of travel away; they merge only into each other.
 * 5. Go: a side's committed groups head for their ward's goal, recent attackers of our commanders first. Ground
 *    groups only take enemies in their ward's zone (and those in contact), which pass the own-ground check and the
 *    3-group cap; anti-air groups count the goal as a leash home point. In the fight (within half the zone of the
 *    goal, kept to 0.625: the present radius) they judge with their ward side's strength present (GetBoost: the
 *    members, the side's static cover at the goal, its committed groups present and themselves).
 */
class CProtect {
public:
	enum class State: char {NOGO, GO};
	struct SConfig {
		bool isEnabled = false;
		float zone = 1200.f;  // elmos around a ward's goal
		int keepFrames = FRAMES_PER_SEC * 12;  // keep_sec
		float respondSec = 12.f;  // travel time
		float goRatio = 1.f;
	};

	CProtect(CCircuitAI* circuit, const SConfig& cfg);
	~CProtect();

	bool IsEnabled() const { return cfg.isEnabled; }

	void Update();  // once a second
	void OnCommDamaged(CCircuitUnit* comm, CEnemyInfo* attacker);  // any of our commanders

	// A committed group while its ward's side is in a go: it acts on the protect
	bool IsActing(const IFighterTask* task) const;
	// A mobile armed enemy (or a moving radar dot) inside the zone of the task's ward, while its ground side is in a go
	bool IsZoneEnemy(const IFighterTask* task, const CEnemyInfo* enemy) const;
	// It hit one of our commanders within the last 5 s
	bool IsRecentAttacker(ICoreUnit::Id id, int frame) const;
	// What an acting group's own strength is multiplied by where it judges its ward's fight: in the present radius,
	// (the strength present, with itself) / its own, at least 1; elsewhere, or for any other task, 1
	float GetBoost(const IFighterTask* task, const springai::AIFloat3& pos) const {
		return isAnyGo ? GetGoBoost(task, pos) : 1.f;  // the usual path (every threat check): one compare
	}
	// A task's units went over to another task (CDefendTask's promotion, ISquadTask::Merge): that one is committed, or
	// a ward member, instead
	void Transfer(const IFighterTask* from, const IFighterTask* to);
	// A task ended (CMilitaryManager::DequeueTask): it is dropped at once, so no pointer outlives its task
	void OnTaskEnd(const IFighterTask* task);
	// Where an acting group heads: its ward's goal (invalid for any other task)
	springai::AIFloat3 GetGoal(const IFighterTask* task) const;
	// Where the commander flees: its ward's turret point; invalid: none (always while off). Asked only while it flees
	// (not on a health retreat)
	springai::AIFloat3 GetMeetingPoint(const CCircuitUnit* comm) const;
	// pos stands at the commander's meeting point
	bool IsAtMeetingPoint(const CCircuitUnit* comm, const springai::AIFloat3& pos) const;

private:
	enum Side: char {GROUND = 0, AIR, SIDE_COUNT};
	struct SSide {
		State state = State::NOGO;
		int stateFrame = -1;
		int goCount = 0;  // goes in this protect (the end line)
		float own = 0.f;  // the members' power (on the air side those that hit air)
		float cover = 0.f;  // turrets of the side's kind at the turret point, else at the goal
		float commCover = 0.f;  // ... at the goal
		float danger = 0.f;
		float spill = 0.f;  // ground: the unanswered gunships (GetGunshipSpill), committed against up to danger again
		float need = 0.f;  // the commit's target this update
		float committedSum = 0.f;  // its committed groups
		float presentSum = 0.f;  // own + commCover + its present committed groups
		float ratio = 0.f;
		int count = 0;  // its committed groups
		float GetSum() const { return committedSum + own + cover; }  // the ratio's numerator
		float GetRatioWith(float extra) const {  // with extra strength, against its danger and the spill
			const float judged = danger + spill;
			return (judged > 0.f) ? (GetSum() + extra) / judged : std::numeric_limits<float>::max();
		}
		float GetRatio() const { return GetRatioWith(0.f); }
		float GetHelpPresent() const { return std::max(presentSum - own - commCover, 0.f); }  // committed groups there
		void ClearSums() { committedSum = 0.f; presentSum = own + commCover; count = 0; }
	};
	struct SWard {
		int id = 0;
		bool isOn = false;
		int startFrame = -1;
		int lastDangerFrame = -1;
		bool isCornered = false;  // latched for the anchor's life (commander wards)
		CCircuitUnit* comm = nullptr;  // the lead commander this update; nullptr: a group ward
		const char* name = "";  // the lead member's def (logs)
		int memberCount = 0;
		float value = 0.f;  // the members' metal cost
		float goLine = 1.f;
		springai::AIFloat3 goal = -RgtVector;
		springai::AIFloat3 dangerPos = -RgtVector;  // the nearest seen danger, kept from the last update that saw one
		float dangerRange = 0.f;
		float airShare = 0.f;  // the air side's share of the danger, kept while it is empty (logged)
		float peakAirShare = 0.f;  // the highest airShare of this protect (the end line)
		float planeDanger = 0.f;  // UpdatePlanes: their threat before thr_mod (CCircuitDef::GetRawThreat)
		int planeCount = 0;
		float gunshipDanger = 0.f;
		SSide sides[SIDE_COUNT];
		float deathSec = 0.f;  // the lead commander's estimated time to death (max: unhurt, or a group ward)
		ICoreUnit::Id healthId = -1;  // whose health it tracks
		float prevHealth = -1.f;
		int prevHealthFrame = -1;
		float dps = 0.f;  // smoothed damage rate
		springai::AIFloat3 turretPos = -RgtVector;  // the meeting point (GetBehind a turret); invalid: none
		std::unordered_map<ICoreUnit::Id, int> planes;  // plane id -> frame it last headed for the goal
		bool isReleased = false;  // the pick: handed its groups back this update
		bool IsGo() const { return (sides[GROUND].state == State::GO) || (sides[AIR].state == State::GO); }
	};
	struct SMember;
	struct SSums;
	struct SCandidate {
		ISquadTask* task;
		std::vector<float> travelSec;  // per on ward (Pick's order), < 0: can't answer it
		SWard* ward = nullptr;  // where the pick put it
	};
	struct STurret {
		springai::AIFloat3 pos;
		float power;
		const CCircuitDef* def;
		bool isDefence;  // against ground (IFighterTask::IsStaticDefence)
		bool isAntiAir;  // against aircraft, within MAX_TURRET_RANGE
	};
	struct SCommit {
		int ward = 0;
		Side side = GROUND;
		float strength = 0.f;  // health-scaled power (ISquadTask::GetRallyStrength)
		bool isPresent = false;  // its leader in the present radius (IsInFight), not fleeing
		bool isMoved = false;  // it moved to another ward once (the pick)
	};

	void CollectMembers(int frame, std::vector<SMember>& out);
	void AssignWards(std::vector<SMember>& members);  // clusters, with the wards' identities kept
	void UpdateWard(SWard& w, const std::vector<const SMember*>& members, int frame);
	// Its lead, value and the members' power and threats
	void SumMembers(SWard& w, const std::vector<const SMember*>& members, SSums& out) const;
	// Danger and power by side, and the cover (at the turret point of a commander ward)
	void UpdateSides(SWard& w, const SSums& sums, float forceGround, float forceAir, bool isStart, bool isOutnumbered);
	void ResetWard(SWard& w);  // its sides and its fight's state
	void Start(SWard& w, int frame);
	void End(SWard& w, const char* reason, int frame);
	void SetState(SWard& w, Side side, State value, int frame);
	std::string GetLabel(const SWard& w) const;  // the logs' [ward N: lead +others, value m]
	const SCommit* FindCommit(const IFighterTask* task, const SWard*& outWard) const;  // nullptr: not committed
	float GetGoBoost(const IFighterTask* task, const springai::AIFloat3& pos) const;
	// The gunships of the ward's air danger that airAnswer leaves unanswered: the ground side's spill
	float GetGunshipSpill(const SWard& w, float airAnswer) const;
	void UpdateDeathTime(SWard& w, int frame);
	void CollectPlanes();  // seen enemy planes with a ground weapon, once per update
	// Those in the zone heading for the goal (or that just hit a commander): planeDanger
	void UpdatePlanes(SWard& w, int frame);
	void CollectTurrets(int frame);  // both kinds, once per update when first needed
	float GetStaticCover(const springai::AIFloat3& pos, bool isAir, float range) const;  // range: the attacker's
	// The spot just behind a turret, away from the danger, where the commander can stand (else the turret's own)
	springai::AIFloat3 GetBehind(const SWard& w, const springai::AIFloat3& turret) const;
	// The turrets at pos and the ward against the danger: the ground side (with the gunships those turrets leave),
	// or the air side when there is no ground danger
	float GetSpotRatio(const SWard& w, const springai::AIFloat3& pos) const;
	void SetCover(SWard& w, const springai::AIFloat3& pos);  // both sides' cover
	bool IsTurretSpotValid(const SWard& w, const springai::AIFloat3& spot) const;
	bool IsTurretSpotHeld(const SWard& w, const springai::AIFloat3& spot) const;
	void UpdateTurretPoint(SWard& w);
	// The present radius: half the zone around the goal; once in it (wasPresent), 0.625 of it
	bool IsInFight(const SWard& w, const springai::AIFloat3& pos, bool wasPresent) const;
	float GetTravelSec(const ISquadTask* task, const springai::AIFloat3& goal, int frame) const;
	// Its travel to the ward when it can answer it (in time, and it can move there), else -1
	float GetReachSec(const ISquadTask* task, const SWard& w, int frame) const;
	void CollectCandidates(bool isAir, int frame, const std::vector<SWard*>& on, std::vector<SCandidate>& out) const;
	// Its strength and flags, added to its side's sums
	void Refresh(const ISquadTask* task, int frame, SCommit& commit, SWard& w);
	// The side's commit and go or no-go over all on wards
	void Pick(Side side, int frame, const std::vector<SWard*>& on);
	// The best (candidate, ward) pairs by value / (travel + 3 s), for the wards in state that are short of their need
	// (only: that ward alone)
	void Fill(Side side, State state, int frame, const std::vector<SWard*>& on, std::vector<SCandidate>& pool,
			const SWard* only = nullptr);
	// Its picks of the side handed back to the pool
	void Release(SWard& w, Side side, std::vector<SCandidate>& pool);
	// Committed groups not yet present move, once, to a ward short of its need that scores them 2x higher
	void MoveGroups(Side side, int frame, const std::vector<SWard*>& on);

	CCircuitAI* circuit;
	SConfig cfg;

	std::map<int, SWard> wards;  // id -> ward (on or off; off ones are kept while they have members)
	int nextWardId;
	bool isAnyGo;  // a side of some ward is in a go (GetBoost's quick path)
	int turretFrame;  // the frame turrets were collected at
	std::vector<springai::AIFloat3> goals;  // this update's ward goals (the turrets' reach)
	std::vector<STurret> turrets;
	std::vector<const CEnemyInfo*> enemyPlanes;  // CollectPlanes
	std::unordered_map<const IFighterTask*, SCommit> committed;
	std::unordered_map<const void*, int> memberWard;  // a commander (unit) or protected task -> its ward's id
	std::unordered_map<ICoreUnit::Id, int> attackers;  // enemy id -> frame it last hit one of our commanders
	std::unordered_map<ICoreUnit::Id, int> damaged;  // commander id -> frame it was last hit
};

} // namespace circuit

#endif // SRC_CIRCUIT_MODULE_PROTECT_H_

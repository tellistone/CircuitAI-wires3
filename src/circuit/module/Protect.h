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
 *    took damage. It ends keep_sec after the last danger seen. Zone: zone elmos around the commander, mobile armed
 *    enemies only.
 * 2. Danger: the larger of every enemy blob centred in the zone (CEnemyManager::GetBlobs, with the count mod, asked
 *    with the attack rally's seen time so the cache is shared) and the commander's own danger list in the zone, which
 *    also has gunships and moving blips; plus the enemy static defences whose range reaches into the zone.
 * 3. Meeting point: our or allied turrets near the commander when their static cover plus the commander beats the
 *    danger, the commander reaches them before its time to death, and the spot is a safe flee spot (outside the seen
 *    flee zones, farther from the danger than the commander). Kept while it holds 1 / quota.attack_keep_threat of the
 *    danger, replaced only when it fails. None during a health retreat. The commander flees to it when it flees, and
 *    stays once at it (cornered there, it still builds its defence). Without one it flees as it would.
 * 4. Commit: attack, raid and gathering groups that can reach the commander within respond_sec (or its time to death
 *    if shorter, checked when a group joins), not fighting elsewhere (against gunships: anti-air groups only), nearest
 *    first, until groups + commander + static cover reach 1.25x the danger. Ratio = that sum / danger. Go at go_ratio
 *    with a group at least (0.6 x it once cornered: no escape or paralysed, latched for the anchor's life); no-go below
 *    the go line / quota.attack_keep_threat. 5 s per state at least. On go the committed groups are latched until the
 *    anchor ends or no-go; they merge only into each other.
 * 5. Go: committed groups head for the goal (the turret point while the commander flees to it or stands at it, else
 *    the commander) and only take zone enemies (and those in contact), the commander's recent attackers first; zone
 *    enemies pass the own-ground check and the 3-group cap. Within their fire range of the commander or the goal they
 *    judge with the strength present (GetBoost: the commander, the static cover and the committed groups within half
 *    the zone of either, and themselves), with no allied cover on top: it is in that already.
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

	const SConfig& GetConfig() const { return cfg; }
	bool IsEnabled() const { return cfg.isEnabled; }

	void Update();  // once a second
	void OnCommDamaged(CCircuitUnit* comm, CEnemyInfo* attacker);

	bool IsActive() const { return state != State::OFF; }
	bool IsGo() const { return state == State::GO; }
	// A committed group while the state is go: it acts on the protect
	bool IsActing(const IFighterTask* task) const {
		return (state == State::GO) && (committed.find(task) != committed.end());
	}
	// A mobile armed enemy (or a blip) inside the zone, while the state is go
	bool IsZoneEnemy(const CEnemyInfo* enemy) const;
	// It hit the commander within the last 5 s
	bool IsRecentAttacker(ICoreUnit::Id id, int frame) const;
	// An acting group's (present groups + commander + static cover, with itself) / its own strength, at least 1;
	// 1 for any other task
	float GetJointMod(const IFighterTask* task) const;
	// GetJointMod where pos is within range of the commander or the goal, else 1: what an acting group's own strength
	// is multiplied by where it judges that fight
	float GetBoost(const IFighterTask* task, const springai::AIFloat3& pos, float range) const;
	// A committed group's units went over to another task (CDefendTask's promotion, ISquadTask::Merge): that one is
	// committed instead
	void Transfer(const IFighterTask* from, const IFighterTask* to);
	// A task ended (CMilitaryManager::DequeueTask): it is dropped at once
	void OnTaskEnd(const IFighterTask* task);
	// Where acting groups head: the turret point while the commander flees to it or stands at it, else the commander
	const springai::AIFloat3& GetGoal() const { return goalPos; }
	// Where the commander flees: the turret point; invalid: none (also during a health retreat)
	const springai::AIFloat3& GetMeetingPoint() const { return meetPos; }

private:
	struct SCandidate {
		ISquadTask* task;
		float sqDist;
	};
	struct STurret {
		springai::AIFloat3 pos;
		float power;
		const CCircuitDef* def;
	};
	struct SCommit {
		float strength;  // with the group size bonus
		bool isPresent;  // its leader within half the zone of the commander or the goal
	};
	void Start(int frame);
	void End(const char* reason, int frame);
	void SetState(State value, const char* reason, int frame);
	bool IsTaskAlive(const IFighterTask* task) const;
	float GetBlobDanger(int frame) const;
	float GetStaticDanger() const;
	void UpdateDeathTime(CCircuitUnit* comm, int frame);
	void CollectTurrets(int frame);
	float GetStaticCover(const springai::AIFloat3& pos) const;
	// line: the share of the danger turrets + commander must hold there (1 to pick, 1 / attack_keep_threat to keep)
	bool IsTurretSpotValid(CCircuitUnit* comm, const springai::AIFloat3& spot, float cover, float line) const;
	void UpdateTurretPoint(CCircuitUnit* comm, int frame);
	void UpdateGoal(CCircuitUnit* comm);
	bool IsPresent(const ISquadTask* task, int frame) const;
	void CollectCandidates(CCircuitUnit* comm, int frame, std::vector<SCandidate>& out) const;
	void UpdateCommitted(CCircuitUnit* comm, int frame);

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
	bool isAirDanger;
	float dangerRange;
	float own;  // the commander's power
	float danger;
	float staticDanger;  // enemy static defences, in danger
	float staticCover;  // at the turret point, else where the commander stands
	float goalCover;  // at the goal
	float committedSum;
	float presentSum;  // own + goalCover + the present committed groups
	float ratio;
	float deathSec;  // estimated time to death, std::numeric_limits<float>::max() while unhurt
	float prevHealth;
	int prevHealthFrame;
	float dps;  // smoothed damage rate
	springai::AIFloat3 turretPos;  // invalid: none
	springai::AIFloat3 meetPos;  // turretPos, but none during a health retreat
	springai::AIFloat3 goalPos;  // GetGoal
	std::vector<STurret> turrets;  // CollectTurrets, each update
	std::unordered_map<const IFighterTask*, SCommit> committed;
	std::unordered_map<ICoreUnit::Id, int> attackers;  // enemy id -> frame it last hit the commander
};

} // namespace circuit

#endif // SRC_CIRCUIT_MODULE_PROTECT_H_

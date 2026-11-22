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
#include <unordered_set>
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
 *    also has gunships and moving blips.
 * 3. Meeting point: our or allied turrets near the commander when their static cover plus the commander beats the
 *    danger, the commander reaches them before its time to death, and the spot is a safe flee spot (outside the seen
 *    flee zones, farther from the danger than the commander). Kept while it still passes, replaced only when it fails.
 *    Else the nearest committed group (the commander flees to it, the groups head for the commander).
 * 4. Commit: attack, raid and gathering groups that can reach the commander within respond_sec (or its time to death
 *    if shorter, checked when a group joins), not fighting elsewhere, nearest first, until groups + commander + static
 *    cover reach 1.25x the danger. Ratio = that sum / danger. Go at go_ratio (0.6 x it once cornered: no escape or
 *    paralysed, latched for the anchor's life); no-go below the go line / quota.attack_keep_threat. 5 s per state at
 *    least. On go the committed groups are latched until the anchor ends or no-go.
 * 5. Go: committed groups only take zone enemies (and those in contact), the commander's recent attackers first; zone
 *    enemies pass the own-ground check and the 3-group cap; within their fire range of the commander or the turret
 *    point they judge with the committed strength (GetBoost), leaving each other out of their cover.
 * 6. The commander flees to the meeting point when it flees, and stays once at it. Groups released by a no-go flee to
 *    it too, or with none to where the commander flees.
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
	// Committed earlier in this anchor and let go by a no-go: it flees to the meeting point
	bool IsReleased(const IFighterTask* task) const {
		return (state != State::OFF) && (released.find(task) != released.end());
	}
	// A mobile armed enemy (or a blip) inside the zone, while the state is go
	bool IsZoneEnemy(const CEnemyInfo* enemy) const;
	// It hit the commander within the last 5 s
	bool IsRecentAttacker(ICoreUnit::Id id, int frame) const;
	// An acting group's (groups + commander + static cover) / its own strength, at least 1; 1 for any other task
	float GetJointMod(const IFighterTask* task) const;
	// GetJointMod where pos is within range of the commander or the turret point, else 1: what an acting group's own
	// strength is multiplied by where it judges that fight
	float GetBoost(const IFighterTask* task, const springai::AIFloat3& pos, float range) const;
	// A committed group's units went over to another task (CDefendTask's promotion, ISquadTask::Merge): that one is
	// committed instead
	void Transfer(const IFighterTask* from, const IFighterTask* to);
	// A task ended (CMilitaryManager::DequeueTask): it is dropped at once
	void OnTaskEnd(const IFighterTask* task);
	// Where acting groups head: the turret point, else the commander
	const springai::AIFloat3& GetGoal() const { return utils::is_valid(turretPos) ? turretPos : commPos; }
	// Where the commander flees: the turret point, else on a go the nearest committed group; invalid: none
	const springai::AIFloat3& GetMeetingPoint() const { return meetPos; }
	// Where released groups flee: the meeting point, else (a no-go) the commander's own flee goal; invalid: none
	const springai::AIFloat3& GetFleeMeetingPoint() const {
		return utils::is_valid(meetPos) ? meetPos : fleeMeetPos;
	}

private:
	struct SCandidate {
		ISquadTask* task;
		float sqDist;
		float travelSec;
	};
	struct STurret {
		springai::AIFloat3 pos;
		float power;
		const CCircuitDef* def;
	};
	void Start(int frame);
	void End(const char* reason, int frame);
	void SetState(State value, const char* reason, int frame);
	bool IsTaskAlive(const IFighterTask* task) const;
	float GetBlobDanger(int frame) const;
	void UpdateDeathTime(CCircuitUnit* comm, int frame);
	void CollectTurrets(int frame);
	float GetStaticCover(const springai::AIFloat3& pos) const;
	bool IsTurretSpotValid(CCircuitUnit* comm, const springai::AIFloat3& spot, float cover) const;
	void UpdateTurretPoint(CCircuitUnit* comm, int frame);
	void CollectCandidates(CCircuitUnit* comm, int frame, std::vector<SCandidate>& out) const;
	void UpdateCommitted(CCircuitUnit* comm, int frame);
	void UpdateMeetingPoint(CCircuitUnit* comm, int frame);

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
	float staticCover;  // at the turret point, else where the commander stands
	float committedSum;
	float ratio;
	float deathSec;  // estimated time to death, std::numeric_limits<float>::max() while unhurt
	float prevHealth;
	int prevHealthFrame;
	float dps;  // smoothed damage rate
	springai::AIFloat3 turretPos;  // invalid: none
	springai::AIFloat3 meetPos;  // invalid: none
	springai::AIFloat3 fleeMeetPos;  // no-go: the commander's flee goal while it flees; invalid: none
	const ISquadTask* meetTask;  // the committed group meetPos follows; nullptr: none
	std::vector<STurret> turrets;  // CollectTurrets, each update
	std::unordered_map<const IFighterTask*, float> committed;  // group -> its strength (with the group size bonus)
	std::unordered_set<const IFighterTask*> released;
	std::unordered_map<ICoreUnit::Id, int> attackers;  // enemy id -> frame it last hit the commander
};

} // namespace circuit

#endif // SRC_CIRCUIT_MODULE_PROTECT_H_

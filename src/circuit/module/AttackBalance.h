/*
 * AttackBalance.h
 *
 *  Created on: Nov 18, 2026
 *      Author: Claude
 */

#ifndef SRC_CIRCUIT_MODULE_ATTACKBALANCE_H_
#define SRC_CIRCUIT_MODULE_ATTACKBALANCE_H_

#include "unit/CoreUnit.h"
#include "util/Defines.h"

#include "AIFloat3.h"

#include <map>
#include <set>
#include <vector>

namespace circuit {

class CCircuitAI;
class CCircuitDef;
class CAttackTask;

/*
 * Force balance (quota.attack_balance): before an attack group starts a fight, the enemy forces that could join it soon
 * are summed and compared with ours there. Point threat (the threat map) only sees enemies whose weapon range reaches a
 * spot, so a large army spread out puts little on any one cell; this sees all of it.
 * Every unit counts by how soon it could shoot at F (where our group stands to fire):
 *   t = max(0, |u - F| - range) / max(speed, 1),  weight = max(0, 1 - t / horizon),  force = sum of weight x power.
 * Aircraft count only where they already reach F (t = 0); statics with the cover falloff (CAllyCover::Falloff) over
 * their cover range (CThreatMap::GetCoverRange). A range is the one the threat map draws at F: land, and water
 * range on water (amphibious groups under water: water range only).
 * - Enemy side E: every hostile that can hit land or water (the threat map's surface and amphibious layers), finished,
 *   threat above 0, power its threat (CEnemyUnit::GetThreat: damage x sqrt(health), thr_mod, shields). Mobiles in radar
 *   or LOS, or out of both for under 10 s, at their last known position; statics while known (as the threat map).
 *   Unknown radar blips are left out of E: their share is only logged.
 * - Our side: the judging group's own units (CAttackTask, timed to its leader), plus ALLY_SHARE x the other allied
 *   mobile armed units (def power; every allied team, from the influence map's snapshot) and ALLY_SHARE x the allied
 *   statics (the threat map's cover layer). Left out: the judging group, attack groups fleeing or outnumbered, units
 *   retreating to repair.
 * The lists are rebuilt at most once per threat map update, on the first check after it.
 */
class CAttackBalance {
public:
	// Pick: a new target; keep: the target we hold, and the own spot; zone: an avoid zone's spot (CAttackTask)
	enum class Line: int {PICK = 0, KEEP, OWN, ZONE, _SIZE_};
	static constexpr int LINE_COUNT = static_cast<int>(Line::_SIZE_);
	static constexpr float ALLY_SHARE = 0.75f;  // allies and allied statics count this much
	static constexpr float KEEP_FACTOR = 1.34f;  // the keep line: ratio x this

	struct SConfig {
		float ratio = 0.f;  // fail at enemy >= ratio x ours; 0: off
		float horizon = 10.f;  // seconds of walking at which a unit's weight reaches 0
		bool isLogOnly = true;  // log what would be skipped, change nothing
	};
	// The force at a spot. own: the judging group's (CAttackTask fills it in, x powerMod)
	struct SForce {
		float enemy = 0.f;
		float own = 0.f;
		float allied = 0.f;  // raw: x ALLY_SHARE in GetOurs
		float cover = 0.f;  // allied statics, raw
		float blip = 0.f;  // unknown moving radar blips, logged only
		float GetOurs() const { return own + ALLY_SHARE * (allied + cover); }
		float GetRatio() const;  // enemy / ours (a large number for none of ours)
		float GetBlipShare() const { return (blip > 0.f) ? blip / (enemy + blip) : 0.f; }
	};

	CAttackBalance(CCircuitAI* circuit, const SConfig& cfg);
	~CAttackBalance();

	bool IsOn() const { return cfg.ratio > 0.f; }
	bool IsLogOnly() const { return cfg.isLogOnly; }
	// The line's ratio: ratio, x KEEP_FACTOR for the keep line and the own spot
	float GetLine(Line line) const;
	// enemy >= the line x ours (and some enemy at all)
	bool IsFail(const SForce& force, Line line) const;
	// A mobile unit's weight at dist from F (the formula above)
	float GetMobileWeight(float dist, float range, float speed) const;
	// A unit's own range against a spot, as the threat map draws it (header)
	static float GetRangeAt(float rangeLand, float rangeWater, bool isWater, bool isAmph);
	static float GetRangeAt(const CCircuitDef* cdef, bool isWater, bool isAmph);

	// Enemy force at F, the allied mobiles (the judging group's units left out: self, only compared) and the allied
	// statics there; isAmph: the judging group is amphibious. outForce.own is left alone
	void GetForceAt(const springai::AIFloat3& F, const CAttackTask* self, bool isAmph, SForce& outForce);

	/*
	 * The BALANCE log, every minute while on: checks and skips by line, flag and zone events, fights by their E/A at
	 * engage and how they ended, blocked time, and the main-thread time
	 */
	void NoteCheck(Line line, bool isSkip, ICoreUnit::Id targetId = -1);
	void NoteFlag(bool isOn) { ++(isOn ? stats.flagsOn : stats.flagsOff); }
	void NoteZone(bool isSet) { ++(isSet ? stats.zonesSet : stats.zonesLifted); }
	// One fight (CAttackTask's 20 s window after it engaged a new target): its E/A at engage, what it lost, fled or not
	void NoteFight(float ratio, float blipShare, float metalLost, bool isFled);
	// A group's blocked time (frames): outnumbered, or a decision whose candidates the balance all skipped
	void NoteBlocked(const CAttackTask* task, ICoreUnit::Id leaderId, int frames);
	void Log();

private:
	enum class Kind: char {MOBILE, AIR, STATIC};
	struct SEntry {
		springai::AIFloat3 pos;
		float power;
		float rangeLand;
		float rangeWater;
		float speed;
		float sqReach;  // beyond this from F it can't count: range + horizon x speed (aircraft, statics: range)
		Kind kind;
		const CAttackTask* task;  // allies: our attack group it is in (only compared, never dereferenced); else nullptr
	};
	void Update();  // rebuild the lists if stale
	void AddEntry(std::vector<SEntry>& entries, const CCircuitDef* cdef, const springai::AIFloat3& pos, float power,
			const CAttackTask* task);
	float GetWeight(const SEntry& e, float sqDist, bool isWater, bool isAmph) const;

	CCircuitAI* circuit;
	SConfig cfg;
	std::vector<SEntry> enemies;
	std::vector<SEntry> allies;
	std::vector<springai::AIFloat3> blips;  // unknown moving radar blips
	float blipPower = 0.f;  // the median power, speed and range of the known enemy mobiles
	float blipSpeed = 0.f;
	float blipRange = 0.f;
	int listFrame;

	struct SFights {
		int count = 0;
		float metalLost = 0.f;
		int flees = 0;
		float blipShare = 0.f;  // sum
	};
	struct SBlocked {
		ICoreUnit::Id leaderId;
		int frames;
	};
	struct SStats {
		int checks[LINE_COUNT] = {0, 0, 0, 0};
		int skips[LINE_COUNT] = {0, 0, 0, 0};
		std::set<ICoreUnit::Id> skippedTargets;
		int flagsOn = 0;
		int flagsOff = 0;
		int zonesSet = 0;
		int zonesLifted = 0;
		SFights fights[4];  // by E/A at engage: under 1, 1 to 1.5, 1.5 to 2, 2 or more
		std::map<const CAttackTask*, SBlocked> blocked;  // only compared, never dereferenced
		int rebuilds = 0;
		float rebuildMs = 0.f;
		int forces = 0;  // GetForceAt calls
		float forceMs = 0.f;
	} stats;
};

} // namespace circuit

#endif // SRC_CIRCUIT_MODULE_ATTACKBALANCE_H_

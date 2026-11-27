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

#include <functional>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

namespace circuit {

class CCircuitAI;
class CCircuitDef;
class CCircuitUnit;
class CAttackTask;
class IFighterTask;

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
	static constexpr int FIGHT_FRAMES = FRAMES_PER_SEC * 20;  // a fight's outcome: what happens this long after engage

	struct SConfig {
		float ratio = 0.f;  // fail at enemy >= ratio x ours; 0: off
		float horizon = 10.f;  // seconds of walking at which a unit's weight reaches 0
		bool isLogOnly = true;  // log what would be skipped, change nothing
	};
	// The force at a spot. own: the judging group's (CAttackTask fills it in, x powerMod)
	struct SForce {
		float enemy = 0.f;
		float air = 0.f;  // the aircraft's part of enemy
		float statics = 0.f;  // the statics' part of enemy
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
	float GetRatioLine() const { return cfg.ratio; }  // E / A at which a spot is outnumbered (0: off)
	// The line's ratio: ratio, x KEEP_FACTOR for the keep line and the own spot
	float GetLine(Line line) const;
	// enemy >= the line x ours (and some enemy at all)
	bool IsFail(const SForce& force, Line line) const;
	// A mobile unit's weight at dist from F (the formula above)
	float GetMobileWeight(float dist, float range, float speed) const;
	// A unit's weapon ranges against land and water (0 where it can't hit that layer)
	static void GetRanges(const CCircuitDef* cdef, float& outLand, float& outWater);
	// A unit's own range against a spot, as the threat map draws it (header)
	static float GetRangeAt(float rangeLand, float rangeWater, bool isWater, bool isAmph);
	static float GetRangeAt(const CCircuitDef* cdef, bool isWater, bool isAmph);

	// Enemy force at F, the allied mobiles (the judging group's units left out: self, only compared; and those of the
	// tasks isLeftOut picks, CProtect's own groups) and the allied statics there; isAmph: the judging group is
	// amphibious. outForce.own is left alone
	using LeftOut = std::function<bool (const IFighterTask*)>;
	void GetForceAt(const springai::AIFloat3& F, const CAttackTask* self, bool isAmph, SForce& outForce,
			const LeftOut& isLeftOut = nullptr);

	/*
	 * The BALANCE log, every minute while on: checks and skips by line, flag and zone events, fights by their E/A at
	 * engage and how they ended, blocked time, and the main-thread time
	 */
	void NoteCheck(Line line, bool isSkip, ICoreUnit::Id targetId = -1);
	void NoteFlag(bool isOn) { ++(isOn ? stats.flagsOn : stats.flagsOff); }
	void NoteZone(bool isSet) { ++(isSet ? stats.zonesSet : stats.zonesLifted); }
	/*
	 * Fights (the outcome log): a group that engages a new target opens one (CAttackTask, its E/A at engage), and for
	 * FIGHT_FRAMES the metal its units lose and kill and whether it flees are added. The units are counted by id
	 * wherever they are by then: a damaged unit leaves its group to retreat and usually dies after, so the group's
	 * own death events miss most losses. A fight that ran its time is counted at the next Log
	 */
	int OpenFight(const std::set<CCircuitUnit*>& units, float ratio, float blipShare);  // its id
	bool IsFightOpen(int fightId) const;
	void NoteFled(int fightId);
	void NoteUnitLost(ICoreUnit::Id unitId, float metal);  // any of our units died (CMilitaryManager::UnitDestroyed)
	void NoteKill(ICoreUnit::Id attackerId, float metal);  // one of our units killed an enemy
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
		const IFighterTask* task;  // allies: our fight task it is in (only compared, never dereferenced); else nullptr
	};
	void Update();  // rebuild the lists if stale
	void AddEntry(std::vector<SEntry>& entries, const CCircuitDef* cdef, const springai::AIFloat3& pos, float power,
			const IFighterTask* task);
	float GetWeight(const SEntry& e, float sqDist, bool isWater, bool isAmph) const;
	struct SFight;
	SFight* GetFightOf(ICoreUnit::Id unitId);  // the open fight a unit counts toward, or nullptr
	void CloseFights();  // count the fights that ran their time into the bands

	CCircuitAI* circuit;
	SConfig cfg;
	std::vector<SEntry> enemies;
	std::vector<SEntry> allies;
	std::vector<springai::AIFloat3> blips;  // unknown moving radar blips
	float blipPower = 0.f;  // the median power, speed and range of the known enemy mobiles
	float blipSpeed = 0.f;
	float blipRange = 0.f;
	int listFrame;

	struct SFight {
		int endFrame;
		float ratio;  // E/A at engage
		float blipShare;
		float metalLost = 0.f;
		float metalKilled = 0.f;
		bool isFled = false;
		std::vector<ICoreUnit::Id> unitIds;  // the group's units at engage
	};
	std::unordered_map<int, SFight> fights;  // open, by id
	std::unordered_map<ICoreUnit::Id, int> fightOfUnit;  // our units in an open fight: its id (the latest one)
	int nextFightId = 0;

	struct SFights {
		int count = 0;
		float metalLost = 0.f;
		float metalKilled = 0.f;
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
		SFights bands[4];  // fights by E/A at engage: under 1, 1 to 1.5, 1.5 to 2, 2 or more
		std::map<const CAttackTask*, SBlocked> blocked;  // only compared, never dereferenced
		int rebuilds = 0;
		float rebuildMs = 0.f;
		int forces = 0;  // GetForceAt calls
		float forceMs = 0.f;
	} stats;
};

} // namespace circuit

#endif // SRC_CIRCUIT_MODULE_ATTACKBALANCE_H_

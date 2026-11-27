/*
 * RaidDefence.h
 *
 *  Created on: Nov 27, 2026
 *      Author: Claude
 */

#ifndef SRC_CIRCUIT_MODULE_RAIDDEFENCE_H_
#define SRC_CIRCUIT_MODULE_RAIDDEFENCE_H_

#include "unit/CircuitDef.h"
#include "unit/CoreUnit.h"
#include "util/Defines.h"

#include "AIFloat3.h"

#include <map>
#include <unordered_map>
#include <vector>

namespace circuit {

class CCircuitAI;
class CCircuitUnit;
class CEnemyInfo;
struct STerrainMapArea;

/*
 * Raid defence (quota.raid_defence): early in the game a share of our raiders defends our mexes, constructors and
 * factories, sized by the enemy raider threat on our ground. Updated once a second.
 * Measure:
 * - Counted enemies: a known type of an enemy role in enemy_roles, mobile, not flying, armed against land or water,
 *   finished and not hidden, seen in LOS or radar now or within SEEN_FRAMES (fading at half weight every
 *   FADE_FRAMES out of both); an enemy commander too while within COMM_JOIN_DIST of a counted raider of the same
 *   set. Each weighs its threat x raid_thr_mod, its count weight the raid view's (the threat map's raid layers).
 * - On our ground: the ally team's territory (CInfluenceMap::IsAllyTerritory), or our held ground (CChokeMap) once
 *   the choke map is ready. Once counted, an enemy stays counted until GROUND_KEEP_FRAMES off it.
 * - Blobs: CEnemyManager::MakeBlobs over the counted enemies on our ground, each blob's threat with the squad
 *   layers' count bonus (CThreatMap::GetCountMod). A blob is an incident; it keeps its id while it shares at least
 *   half the members of the incident's blob a second ago.
 * - Answerable (CanOpenIncident): an area our raiders move in reaches it (none yet: any), and an undefended asset of
 *   ours is within protect's zone of it.
 * - T: the answerable incidents' threat summed; B: the biggest. Bmax: the biggest blob of counted enemies anywhere,
 *   a peak halving every BMAX_HALF_FRAMES, never below oneRaider: the most threat one enemy raider has shown (with
 *   its own count bonus).
 * - Assets: our finished mexes (value: half the metal cost + the spot's income x INCOME_SEC), our constructors and
 *   factories (half the metal cost). A mex cluster is defended once our and allied ground turrets covering its
 *   weakest mex (power x sqrt(health share), IFighterTask::GetCoverPower) reach Bmax, and undefended again below
 *   DEFENDED_KEEP x Bmax or when one of those turrets is gone; none at all: undefended. Factories the same. Every
 *   asset in a pocket whose entrances the army holds (quota.hold_choke, CFrontline) is defended, and so is a
 *   constructor inside a defended cluster.
 * - D, the reserve's target strength: margin x T, at most max_share of our raid groups' strength (GetSafeThreat).
 * Logs: RAIDDEF state every 10 s while there is something to show, RAIDDEF minute every minute.
 */
class CRaidDefence {
public:
	struct SConfig {
		bool isEnabled = false;
		CCircuitDef::RoleM roleMask = CCircuitDef::RoleMask::RAIDER;  // roles: our main roles that defend
		CCircuitDef::RoleM enemyRoleMask = CCircuitDef::RoleMask::RAIDER;  // enemy_roles: counted in T
		float mexShare = 0.33f;  // off for good once the ally team's finished mexes reach this share of the spots
		float margin = 1.3f;
		float maxShare = 0.6f;
		float postMin = 1.25f;
		float reactSec = 8.f;
		float escortLeashSec = 10.f;
	};
	// An asset of ours this second
	struct SAsset {
		enum class Kind: char {MEX, BUILDER, FACTORY};
		Kind kind;
		ICoreUnit::Id unitId;
		springai::AIFloat3 pos;
		float value;  // metal
		int cluster;  // the metal cluster it stands in (a constructor: within its radius), -1: none
		int pocket;  // CChokeMap::GetPockets index, -1: open ground
		bool isDefended;
	};
	// A blob of counted enemies on our ground, tracked across seconds
	struct SIncident {
		int id;
		springai::AIFloat3 pos;  // its seed
		springai::AIFloat3 vel;  // its members' mean velocity, elmos per frame
		float threat;  // with the count bonus
		std::vector<ICoreUnit::Id> members;  // ascending
		bool isAnswerable;  // CanOpenIncident
		int openFrame;
	};

	CRaidDefence(CCircuitAI* circuit, const SConfig& cfg);
	~CRaidDefence();

	const SConfig& GetConfig() const { return cfg; }
	bool IsEnabled() const { return cfg.isEnabled; }
	// Enabled, and the window still open (the ally team's mexes haven't reached mex_share yet)
	bool IsOn() const { return cfg.isEnabled && !isOver; }
	// A main role of ours that defends (matched as guard.roles: its own name or the role it acts as)
	bool IsRole(const CCircuitDef* cdef) const;
	// The ally team's territory, or our held ground once the choke map is ready
	bool IsOurGround(const springai::AIFloat3& pos) const;

	void Update();  // once a second
	void Log();  // RAIDDEF minute, once a minute

	// Events (CMilitaryManager): our assets hit or lost, enemies we killed
	void OnUnitDamaged(CCircuitUnit* unit, CEnemyInfo* attacker);
	void OnUnitDestroyed(CCircuitUnit* unit, CEnemyInfo* attacker);
	void OnEnemyKilled(const CEnemyInfo* enemy, int attackerId);
	// It hit one of our mexes, constructors or factories within the last ATTACKER_FRAMES
	bool IsRecentAttacker(ICoreUnit::Id id, int frame) const;

	float GetT() const { return threatT; }
	float GetB() const { return threatB; }
	float GetBmax() const { return threatBmax; }
	float GetOneRaider() const { return oneRaider; }
	float GetD() const { return needD; }
	const std::vector<SAsset>& GetAssets() const { return assets; }
	const std::vector<SIncident>& GetIncidents() const { return incidents; }

private:
	static constexpr int SEEN_FRAMES = FRAMES_PER_SEC * 20;
	static constexpr float FADE_FRAMES = FRAMES_PER_SEC * 15.f;
	static constexpr int GROUND_KEEP_FRAMES = FRAMES_PER_SEC * 5;
	static constexpr float COMM_JOIN_DIST = 600.f;
	static constexpr float BMAX_HALF_FRAMES = FRAMES_PER_SEC * 60.f;
	static constexpr float DEFENDED_KEEP = 0.8f;
	static constexpr float INCOME_SEC = 60.f;
	static constexpr float COST_SHARE = 0.5f;
	static constexpr int ATTACKER_FRAMES = FRAMES_PER_SEC * 5;
	static constexpr int COVER_FRAMES = FRAMES_PER_SEC * 5;  // the turret cover test's interval
	static constexpr int STATE_LOG_FRAMES = FRAMES_PER_SEC * 10;

	struct SCounted;
	struct SCluster {  // a metal cluster's cover, every COVER_FRAMES
		bool hasMex = false;  // one of our finished mexes stands there
		bool isDefended = false;
		float cover = 0.f;  // at its weakest mex
		std::vector<ICoreUnit::Id> turrets;  // those covering it when it was found defended
	};
	struct SFactoryCover {
		bool isDefended = false;
		std::vector<ICoreUnit::Id> turrets;
	};
	struct STurret {
		ICoreUnit::Id id;
		springai::AIFloat3 pos;
		float range;
		float power;
	};

	void CheckWindow(int frame);
	// Counted enemies anywhere and on our ground; oneRaider
	void CollectEnemies(int frame, std::vector<SCounted>& outAll, std::vector<SCounted>& outGround);
	void UpdateIncidents(int frame, std::vector<SCounted>& ground);
	void UpdateBmax(std::vector<SCounted>& all);
	void UpdateCover(int frame);  // the clusters' and factories' turret cover
	void CollectAssets(int frame);
	void UpdateDefenderAreas(int frame);
	bool CanOpenIncident(const SIncident& incident) const;
	// The nearest undefended asset within radius of pos, nullptr: none
	const SAsset* FindUndefended(const springai::AIFloat3& pos, float radius) const;
	float GetRaidStrength() const;  // our raid groups' GetSafeThreat summed
	int GetPocketAt(const springai::AIFloat3& pos) const;  // CChokeMap pocket whose inner side holds pos, -1: none
	bool IsPocketHeld(int pocket) const;  // the army holds all its entrances (CFrontline::GetHolds)
	float GetCoverAt(const springai::AIFloat3& pos, std::vector<ICoreUnit::Id>* outTurrets) const;
	bool IsCoverKept(const std::vector<ICoreUnit::Id>& turrets) const;  // none of them gone
	void LogState(int frame);

	CCircuitAI* circuit;
	SConfig cfg;
	bool isOver;  // the window closed

	float threatT;
	float threatB;
	float threatBmax;
	float oneRaider;
	float needD;
	int nextIncidentId;
	std::vector<SIncident> incidents;  // threat descending
	std::unordered_map<ICoreUnit::Id, int> groundFrames;  // enemy id -> the frame it was last on our ground

	int coverFrame;
	std::vector<STurret> turrets;
	std::vector<SCluster> clusters;  // by metal cluster index
	std::map<ICoreUnit::Id, SFactoryCover> factoryCovers;
	std::vector<SAsset> assets;
	struct SArea {
		STerrainMapArea* area;
		float range;
	};
	std::vector<SArea> defenderAreas;  // the areas our raiders of the roles move in, with their longest range there

	std::unordered_map<ICoreUnit::Id, int> attackers;  // enemy id -> the frame it last hit one of our assets
	int stateLogFrame;

	// RAIDDEF minute
	float lostMetal;  // our assets killed by counted enemy roles
	float defendKills;  // enemy raider metal killed by defence groups
	float raidKills;  // metal killed by raid groups on enemy ground (not ours)
	float shareSum;  // the share of our raid-role units in defence groups, summed per second
	int shareSamples;
};

} // namespace circuit

#endif // SRC_CIRCUIT_MODULE_RAIDDEFENCE_H_

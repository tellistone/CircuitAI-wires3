/*
 * RaidDefence.h
 *
 *  Created on: Nov 27, 2026
 *      Author: Claude
 */

#ifndef SRC_CIRCUIT_MODULE_RAIDDEFENCE_H_
#define SRC_CIRCUIT_MODULE_RAIDDEFENCE_H_

#include "terrain/path/TravelMap.h"
#include "unit/CircuitDef.h"
#include "unit/CoreUnit.h"
#include "util/Defines.h"

#include "AIFloat3.h"

#include <functional>
#include <map>
#include <unordered_map>
#include <vector>

namespace circuit {

class CCircuitAI;
class CCircuitUnit;
class CEnemyInfo;
class CRaidDefendTask;
class CRaidTask;
class IFighterTask;
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
 *   Warm floor: for WARM_FRAMES after a wave (T back to 0), at least post_min x oneRaider, all at the biggest post.
 * Reserve (defence groups, CRaidDefendTask), its strength R the groups' GetSafeThreat summed:
 * - Slots, at most MAX_SLOTS: posts first, then escorts. A post per pocket (CChokeMap) holding undefended mex
 *   clusters or factories of ours: at its busiest entrance's hold spot (CSpotScorer, as CFrontline holds) when it has
 *   1 or 2 entrances and the spot has no threat, else at the value-weighted centre. On open ground a cluster joins the
 *   nearest post within POST_JOIN_SEC, else opens one. Re-placed every POST_FRAMES, moved only beyond POST_MOVE_DIST;
 *   a new post count waits POST_COUNT_FRAMES. An escort beside each constructor that guard.summon's safe ground calls
 *   exposed (itself or its job) and no defended cluster covers, nearest first, while the budget lasts.
 * - Travel: every slot has a threat-aware travel map (CTravelMap) for the most common movement type of our raiders
 *   of the roles, from the post (every POST_FRAMES) or the constructor (once it moved MAP_MOVE_DIST).
 * - Split, every SPLIT_FRAMES and when a wave starts or ends: H = min(D, margin x B) for the posts by value, each
 *   share at least post_min x oneRaider (a smaller one folds into the nearest post); escorts from the rest, up to
 *   half of it, each post_min x oneRaider less the riot guards with it; what is left goes to the posts. Each second
 *   the posts share D less the escorts' needs.
 * - Recruit while R < RECRUIT_SHORT x D. Tier 1 (MakeTask, at once): a new raider starts a group at the slot short
 *   the most. Tier 2 (after TIER2_FRAMES): calm raid groups on our ground or within respond_sec of a slot short of
 *   strength, nearest first; a whole group while it is at most WHOLE_GROUP_MOD x the shortfall, else its units
 *   nearest the slot. Never a group released within RELEASE_IMMUNE_FRAMES, in a raid rally, or with scouts.
 * - Every SPLIT_FRAMES a calm group whose post keeps its need without it moves to the nearest slot short of
 *   strength; escorts stay. A group without a slot takes the nearest one short of strength, else the nearest post.
 * - Release while R > D / RELEASE_RATIO for RELEASE_FRAMES: calm groups back to raiding (a new raid group each), those
 *   without a slot first, then those at slots that need nothing, then by the slot's value, the warm floor's post
 *   last, never below D. Calm: no target, not fleeing, not committed by quota.protect.
 * - The window closing releases every group for good.
 * Logs: RAIDDEF state every 10 s while there is something to show, RAIDDEF recruit and release, RAIDDEF minute every
 * minute.
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
	// Where defence groups wait: a post by our undefended mexes and factories, or an escort beside an exposed
	// constructor
	struct SSlot {
		int id = -1;
		bool isEscort = false;
		ICoreUnit::Id builderId = -1;  // escort: its constructor; -1: a post
		springai::AIFloat3 pos;  // escort: its constructor's, each second
		std::vector<int> pocketKey;  // post: its pocket's choke ids (CChokeMap::SPocket); empty: open ground
		float value = 0.f;  // post: the undefended metal it covers; escort: the constructor's
		float weight = 0.f;  // post: its share of what the posts get (Split)
		float need = 0.f;  // the strength it should hold (GetSafeThreat's measure)
		float have = 0.f;  // the defence groups' strength there now, and parked solo hunters at a post
		CTravelMap map;  // threat-aware travel from it
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
	float GetR() const { return reserveR; }
	const std::vector<SAsset>& GetAssets() const { return assets; }
	const std::vector<SIncident>& GetIncidents() const { return incidents; }

	// Tier 1 (CMilitaryManager::DefaultMakeTask, before guard slots): while the reserve is short, a new raider of the
	// roles starts a defence group at the slot short the most that it can reach. nullptr: not wanted
	IFighterTask* MakeTask(CCircuitUnit* unit);
	const std::vector<SSlot>& GetSlots() const { return slots; }
	const SSlot* FindSlot(int id) const;  // nullptr: no such slot (any more)
	bool HasRoom(int slotId) const;  // the slot holds less than it needs
	// Seconds from the slot to pos at speed (elmos per second): its travel map when that is ready, for this movement
	// type and reaches pos, else the straight line. < 0: no such slot
	float GetSlotSec(int slotId, int mobileId, const springai::AIFloat3& pos, float speed) const;

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
	static constexpr int WARM_FRAMES = FRAMES_PER_SEC * 60;
	static constexpr size_t MAX_SLOTS = 6;
	static constexpr int POST_FRAMES = FRAMES_PER_SEC * 10;
	static constexpr int POST_COUNT_FRAMES = FRAMES_PER_SEC * 20;
	static constexpr float POST_MOVE_DIST = 400.f;
	static constexpr float POST_JOIN_SEC = 15.f;
	static constexpr float MAP_MOVE_DIST = 400.f;
	static constexpr float GUARD_NEAR_DIST = 600.f;  // a riot guard this near its constructor counts for its escort
	static constexpr float HUNTER_PARK_DIST = 400.f;  // a solo hunter this near its parking spot is parked
	static constexpr int SPLIT_FRAMES = FRAMES_PER_SEC * 5;
	static constexpr float RECRUIT_SHORT = 0.8f;
	static constexpr int TIER2_FRAMES = FRAMES_PER_SEC * 2;
	static constexpr float WHOLE_GROUP_MOD = 1.5f;
	static constexpr float RELEASE_RATIO = 0.7f;
	static constexpr int RELEASE_FRAMES = FRAMES_PER_SEC * 5;
	static constexpr int RELEASE_IMMUNE_FRAMES = FRAMES_PER_SEC * 30;

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

	// Reserve
	void UpdateWarm(int frame, float cap);
	void UpdateReserve(int frame);
	CCircuitUnit* FindRepUnit() const;  // a raider of the roles of the most common movement type among them
	void RefreshPosts(int frame, CCircuitUnit* rep);
	springai::AIFloat3 GetPocketPost(int pocket, const springai::AIFloat3& centre, CCircuitUnit* rep) const;
	void Split(int frame, CCircuitUnit* rep);
	void UpdateNeeds(int frame);  // each second: escorts follow their constructors, the posts share D
	void CountHave(int frame);  // R and each slot's have; a group without a slot gets one
	void Rebalance(int frame);
	void Recruit(int frame);  // tier 2
	void Release(int frame);
	void ReleaseAll(int frame);
	void ReleaseGroup(CRaidDefendTask* group, int frame);
	CRaidDefendTask* NewGroup(const SSlot& slot);
	// The slot nearest pos by its travel that passes isOk and that a unit in area reaches; nullptr: none
	SSlot* FindNearestSlot(STerrainMapArea* area, int mobileId, const springai::AIFloat3& pos, float speed,
			const std::function<bool (const SSlot&)>& isOk, float* outSec = nullptr);
	SSlot* FindSlotToEdit(int id);
	// guard.summon's safe ground calls the constructor or its job exposed, and it isn't retreating
	bool IsExposed(CCircuitUnit* builder, int frame) const;
	float GetGuardStrength(CCircuitUnit* builder, int frame) const;  // its riot guards with it (CFGuardTask)
	float GetSlotSec(const SSlot& slot, int mobileId, const springai::AIFloat3& pos, float speed) const;
	bool IsReleased(const CCircuitUnit* unit, int frame) const;  // released within RELEASE_IMMUNE_FRAMES
	static bool IsCalm(const CRaidTask* group, int frame);  // no target, not fleeing, not committed by protect

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

	// Reserve
	std::vector<SSlot> slots;  // posts first, then escorts
	int nextSlotId;
	int postFrame;  // RefreshPosts
	int pendingPostCount;  // a new post count (-1: none), and since when (POST_COUNT_FRAMES)
	int pendingPostFrame;
	int splitFrame;
	float splitD;  // D at the last split, and the warm floor then
	bool splitWarm;
	float reserveR;
	int shortFrame;  // R < RECRUIT_SHORT x D since this frame; -1: not short
	int surplusFrame;  // R > D / RELEASE_RATIO since this frame; -1: no surplus
	bool isWaveOn;  // T above 0 at the last update
	int warmFrame;  // the last wave's end; -1: none
	bool isWarm;
	int warmSlotId;  // the warm floor's post
	std::unordered_map<ICoreUnit::Id, int> releasedFrames;  // unit id -> the frame it was released

	// RAIDDEF minute
	float lostMetal;  // our assets killed by counted enemy roles
	float defendKills;  // enemy raider metal killed by defence groups
	float raidKills;  // metal killed by raid groups on enemy ground (not ours)
	float shareSum;  // the share of our raid-role units in defence groups, summed per second
	int shareSamples;
};

} // namespace circuit

#endif // SRC_CIRCUIT_MODULE_RAIDDEFENCE_H_

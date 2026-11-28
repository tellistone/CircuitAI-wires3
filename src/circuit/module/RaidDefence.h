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
 *   the choke map is ready, while seen there (LOS or radar). Once counted, an enemy stays counted until
 *   GROUND_KEEP_FRAMES off it or out of sight.
 * - Blobs: CEnemyManager::MakeBlobs over the counted enemies on our ground, each blob's threat with the squad
 *   layers' count bonus (CThreatMap::GetCountMod). A blob is an incident. It keeps the id of an incident a second ago
 *   that it shares at least half of the smaller one's members with, the pairs sharing the most first: a raid we cut
 *   down keeps its id, and of a split raid the bigger part does.
 * - Answerable (CanOpenIncident): an area our raiders move in reaches it (none yet: any), and an undefended asset of
 *   ours is within protect's zone of it.
 * - T: the answerable incidents' threat summed, half of it for one whose protect ward has been in a no-go for
 *   NOGO_HALF_FRAMES (a raid we don't answer can't pin the whole reserve); B: the biggest. Bmax: the biggest blob of
 *   counted enemies anywhere, a peak halving every BMAX_HALF_FRAMES, never below oneRaider: the most threat one enemy
 *   raider has shown (with its own count bonus).
 * - Assets: our finished mexes (value: half the metal cost + the spot's income x INCOME_SEC), our constructors and
 *   factories (half the metal cost). A mex cluster is defended once our and allied ground turrets covering its
 *   weakest mex (power x sqrt(health share), IFighterTask::GetCoverPower) reach Bmax, and undefended again below
 *   DEFENDED_KEEP x Bmax or when one of those turrets is gone; none at all: undefended. Factories the same. Every
 *   asset in a pocket whose entrances the army holds (quota.hold_choke, CFrontline) is defended, and so is a
 *   constructor inside a defended cluster.
 * - D, the reserve's target strength: margin x T, at most max_share of our raid groups' strength (GetSafeThreat).
 *   Warm floor: for WARM_FRAMES after a wave (T back to 0), at least post_min x oneRaider, all at the biggest post.
 * Reserve (defence groups, CRaidDefendTask), its strength R the groups' GetSafeThreat summed:
 * - Posts (slots), at most MAX_SLOTS (the least valuable folds into the nearest other). A post per pocket (CChokeMap)
 *   holding undefended mex clusters or factories of ours: at its busiest entrance's hold spot (CSpotScorer, as
 *   CFrontline holds) when it has 1 or 2 entrances and the spot has no threat, kept there while its threat stays
 *   under the post's need; else at the value-weighted centre. On open ground a cluster joins the nearest post within
 *   POST_JOIN_SEC, else opens one. Re-placed every POST_FRAMES, moved only beyond POST_MOVE_DIST; a new post count
 *   waits POST_COUNT_FRAMES.
 * - Travel: every post has a threat-aware travel map (CTravelMap) for the most common movement type of our raiders
 *   of the roles, made again every POST_FRAMES.
 * - Split, every SPLIT_FRAMES and when a wave starts or ends: H = min(D, margin x B) for the posts by value, each
 *   share at least post_min x oneRaider (the smallest folds into the nearest other while it is under that); each
 *   second the posts share D by those weights.
 * - Recruit while R < RECRUIT_SHORT x D. Tier 1 (MakeTask, at once): a new raider that takes no free guard slot
 *   starts a group at the post short the most. Tier 2 (after TIER2_FRAMES): raid groups within respond_sec of a post
 *   short of strength, fighting or not, and calm ones anywhere on our ground, nearest first; a whole group while it
 *   is at most WHOLE_GROUP_MOD x the shortfall, else its units nearest the post. Never a group fleeing, committed by
 *   quota.protect, released within RELEASE_IMMUNE_FRAMES, in a raid rally, or with scouts.
 * - Every SPLIT_FRAMES a calm group whose post keeps its need without it moves to the nearest post short of
 *   strength. A group without a post takes the nearest one short of strength, else the nearest post.
 * - Release while R > D / RELEASE_RATIO for RELEASE_FRAMES: calm groups back to raiding (a new raid group each), those
 *   without a post first, then those at posts that need nothing, then by the post's value, the warm floor's post
 *   last, never below D. Calm: no target, not fleeing, not committed by quota.protect (acting or gathering).
 * - The window closing releases every group for good. Raider guards (guard.summon) stay on throughout, and pick
 *   new raiders first.
 * Response (quota.protect, which must be on): each answerable incident is a protect ward (CProtect::SetIncidents).
 * - Goal: the first undefended asset within GOAL_TRACK_DIST of the blob's track ahead, else the nearest, within
 *   protect's zone. It changes when it is gone or defended, or once another asset has been the better pick for
 *   GOAL_SWITCH_FRAMES.
 * - Concede and ambush: once protect finds no committed group reaching a dying goal in time (isLate), that asset is
 *   given up for the rest of the incident, when another one is left to pick: the groups gather there and meet the
 *   raid. Not for a constructor: it flees.
 * - Gathering post, where committed groups meet in a no-go: kept while it has no threat and stays within the zone of
 *   the goal; else the post nearest the goal within the zone with no threat; else the first spot with no threat
 *   stepping from the goal away from the blob, up to the zone; else the last one for GATHER_KEEP_FRAMES; else none
 *   (they head for the goal).
 * - Protect's danger is the blob's threat on our groups' measure (no raid_thr_mod, no count bonus).
 * Home ground: defence groups judge with quota.thr_mod.distance.raid_defend (the raid curve if unset); a fleeing
 * constructor may run to a post that holds defence groups (FillFleeSpots), so the raiders chasing it meet them.
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
		float threat;  // its members' x raid_thr_mod, with the count bonus (T)
		float rawThreat = 0.f;  // its members' alone: the measure of our groups' strength (protect's danger)
		std::vector<ICoreUnit::Id> members;  // ascending
		bool isAnswerable;  // CanOpenIncident
		int openFrame;
		// Response
		ICoreUnit::Id goalId = -1;  // the asset it heads for (SAsset::unitId)
		ICoreUnit::Id candId = -1;  // a better pick since candFrame (GOAL_SWITCH_FRAMES)
		int candFrame = -1;
		std::vector<ICoreUnit::Id> conceded;  // the goals it gave up
		springai::AIFloat3 gather = -RgtVector;  // the gathering post; invalid: none
		int gatherFrame = -1;  // when a spot for it was last found (GATHER_KEEP_FRAMES)
	};
	// Where defence groups wait: a post by our undefended mexes and factories
	struct SSlot {
		int id = -1;
		springai::AIFloat3 pos;
		std::vector<int> pocketKey;  // its pocket's choke ids (CChokeMap::SPocket); empty: open ground
		float value = 0.f;  // the undefended metal it covers
		float weight = 0.f;  // its share of D (Split)
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

	// Tier 1 (CMilitaryManager::DefaultMakeTask, after guard slots): while the reserve is short, a new raider of the
	// roles starts a defence group at the post short the most that it can reach. nullptr: not wanted
	IFighterTask* MakeTask(CCircuitUnit* unit);
	const std::vector<SSlot>& GetSlots() const { return slots; }
	const SSlot* FindSlot(int id) const;  // nullptr: no such slot (any more)
	bool HasRoom(int slotId) const;  // the slot holds less than it needs
	// Seconds from the slot to pos at speed (elmos per second): its travel map when that is ready, for this movement
	// type and reaches pos, else the straight line. < 0: no such slot
	float GetSlotSec(int slotId, int mobileId, const springai::AIFloat3& pos, float speed) const;
	// The builder flee's extra spots: the posts that hold defence groups
	void FillFleeSpots(F3Vec& out) const;
	// The travel map of the post at pos when it is ready and for this movement type; nullptr: none
	const CTravelMap* GetPostMap(const springai::AIFloat3& pos, int mobileId) const;

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
	static constexpr float HUNTER_PARK_DIST = 400.f;  // a solo hunter this near its parking spot is parked
	static constexpr int SPLIT_FRAMES = FRAMES_PER_SEC * 5;
	static constexpr float RECRUIT_SHORT = 0.8f;
	static constexpr int TIER2_FRAMES = FRAMES_PER_SEC * 2;
	static constexpr float WHOLE_GROUP_MOD = 1.5f;
	static constexpr float RELEASE_RATIO = 0.7f;
	static constexpr int RELEASE_FRAMES = FRAMES_PER_SEC * 5;
	static constexpr int RELEASE_IMMUNE_FRAMES = FRAMES_PER_SEC * 30;
	static constexpr int NOGO_HALF_FRAMES = FRAMES_PER_SEC * 45;
	static constexpr float GOAL_TRACK_DIST = 600.f;
	static constexpr float GOAL_TRACK_SPEED = 0.5f;  // a blob slower than this (elmos per frame) has no track: nearest
	static constexpr int GOAL_SWITCH_FRAMES = FRAMES_PER_SEC * 3;
	static constexpr float GATHER_STEP = 150.f;
	static constexpr int GATHER_KEEP_FRAMES = FRAMES_PER_SEC * 3;

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
	// One pass over our raiders of the roles: defenderAreas, the share defending (the minute line), and returns a rep:
	// one of the most common movement type among them (posts and travel maps are for it); nullptr: none
	CCircuitUnit* ScanRaiders();
	bool CanOpenIncident(const SIncident& incident) const;
	// The nearest undefended asset within radius of pos, nullptr: none
	const SAsset* FindUndefended(const springai::AIFloat3& pos, float radius) const;
	float GetRaidStrength() const;  // our raid groups' GetSafeThreat summed
	int GetPocketAt(const springai::AIFloat3& pos) const;  // CChokeMap pocket whose inner side holds pos, -1: none
	void CountPocketInner();  // pocketInner, once per update
	bool IsPocketHeld(int pocket) const;  // the army holds all its entrances (CFrontline::GetHolds)
	float GetCoverAt(const springai::AIFloat3& pos, std::vector<ICoreUnit::Id>* outTurrets) const;
	bool IsCoverKept(const std::vector<ICoreUnit::Id>& turrets) const;  // none of them gone
	void LogState(int frame);

	// Reserve
	void UpdateWarm(int frame, float cap);
	void UpdateReserve(int frame, CCircuitUnit* rep);
	void RefreshPosts(int frame, CCircuitUnit* rep);
	springai::AIFloat3 GetPocketPost(int pocket, const springai::AIFloat3& centre, CCircuitUnit* rep,
			const SSlot* prev) const;
	void Split();  // the posts' weights
	void CountHave(int frame);  // R and each post's have; a group without a post gets one
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
	float GetSlotSec(const SSlot& slot, int mobileId, const springai::AIFloat3& pos, float speed) const;
	bool IsReleased(const CCircuitUnit* unit, int frame) const;  // released within RELEASE_IMMUNE_FRAMES
	static bool IsCalm(const CRaidTask* group, int frame);  // no target, not fleeing, not acting or gathering for protect

	// Response
	void UpdateResponse(int frame, CCircuitUnit* rep);  // the incidents' goals and gathering posts, sent to protect
	void UpdateGoal(SIncident& inc, int frame);
	const SAsset* PickGoal(const SIncident& inc) const;  // nullptr: none
	const SAsset* FindAsset(ICoreUnit::Id unitId) const;  // an undefended asset; nullptr: none (any more)
	springai::AIFloat3 GetGather(const SIncident& inc, const springai::AIFloat3& goal, CCircuitUnit* rep) const;

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
	std::vector<int> pocketInner;  // CChokeMap pocket -> its inner region count

	std::unordered_map<ICoreUnit::Id, int> attackers;  // enemy id -> the frame it last hit one of our assets
	int stateLogFrame;

	// Reserve
	std::vector<SSlot> slots;  // the posts, at most MAX_SLOTS
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

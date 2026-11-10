/*
 * EnemyManager.h
 *
 *  Created on: Dec 25, 2019
 *      Author: rlcevg
 */

#ifndef SRC_CIRCUIT_UNIT_ENEMY_ENEMYMANAGER_H_
#define SRC_CIRCUIT_UNIT_ENEMY_ENEMYMANAGER_H_

#include "unit/CoreUnit.h"
#include "unit/CircuitDef.h"
#include "unit/enemy/EnemyUnit.h"
#include "util/MaskHandler.h"

#include <string>

namespace circuit {

class CCircuitAI;
class CQuadField;
struct STerrainMapArea;

class CEnemyManager {
public:
	using EnemyUnits = std::unordered_map<ICoreUnit::Id, CEnemyUnit*>;
	using EnemyFakes = std::set<CEnemyFake*>;
	struct SEnemyGroup {
		SEnemyGroup(const springai::AIFloat3& p) : pos(p), cost(0.f), threat(0.f) {
			roleCosts.fill(0.f);
		}
		std::vector<ICoreUnit::Id> units;
		springai::AIFloat3 pos;
		std::array<float, CMaskHandler::GetMaxMasks()> roleCosts;
		float cost;
		float threat;  // thr_mod applied
	};

	CEnemyManager(CCircuitAI* circuit);
	virtual ~CEnemyManager();

	void SetAuthority(CCircuitAI* authority) { circuit = authority; }

	CEnemyUnit* GetEnemyUnit(ICoreUnit::Id unitId) const;
	const EnemyUnits& GetEnemyUnits() const { return enemyUnits; }

	const std::vector<ICoreUnit::Id>& GetGarbage() const { return enemyGarbage; }

	const std::vector<SEnemyData>& GetHostileDatas() const { return hostileDatas; }
	const std::vector<SEnemyData>& GetPeaceDatas() const { return peaceDatas; }

	void UpdateEnemyDatas(CQuadField& quadField);

	void PrepareUpdate();
	void EnqueueUpdate();
	bool IsUpdating() const { return isUpdating; }
	void DropUpdate() { isUpdating = false; }  // the update in flight was dropped with its scheduler

	bool UnitInLOS(CEnemyUnit* data);
	std::pair<CEnemyUnit*, bool> RegisterEnemyUnit(ICoreUnit::Id unitId, bool isInLOS);
	CEnemyUnit* RegisterEnemyUnit(springai::Unit* e);

	CEnemyFake* RegisterEnemyFake(CCircuitDef* cdef, const springai::AIFloat3& pos, int timeout);
	void UnregisterEnemyFake(CEnemyFake* data);

	void UnregisterEnemyUnit(CEnemyUnit* data);
private:
	void DeleteEnemyUnit(CEnemyUnit* data);
	void GarbageEnemy(CEnemyUnit* enemy);

public:
	float GetEnemyCost(CCircuitDef::RoleT type) const {
		return enemyInfos[type].cost;
	}
	float GetEnemyThreat(CCircuitDef::RoleT type) const {
		return enemyInfos[type].threat;
	}
	void AddEnemyCost(const CEnemyUnit* e);
	void DelEnemyCost(const CEnemyUnit* e);
	float GetMobileThreat() const { return mobileThreat; }
	// Metal value of the known enemy army: mobile units, commanders (enemy role COMM), constructors and nanoframes in
	// LOS left out (our army has none of them), summed over the enemies we know of now. One in radar or LOS counts in
	// full, one out of both fades by half every halfLife frames since it was last seen (halfLife <= 0: no fading).
	// Enemies seen dying or forgotten (20 min unseen) don't count. Used by economy.ms_pull_army,
	// quota.attack_threat_floor, the base gather size and resign. Walks all known enemies: about once a second at most
	float GetMobileCostNoCommFaded(int frame, float halfLife) const;
	// Log text for the same sum: its part from enemies in radar or LOS and from hidden ones (with their unfaded total),
	// and the enemy types that carry most of it
	std::string GetMobileCostReport(int frame, float halfLife) const;
	float GetStaticThreat() const { return staticThreat; }
	float GetEnemyThreat() const { return mobileThreat + staticThreat; }
	bool IsAirValid() const { return GetEnemyThreat(ROLE_TYPE(AA)) <= maxAAThreat; }
	bool IsEnemyNear(const springai::AIFloat3& pos, float maxThreat);

	const std::vector<SEnemyGroup>& GetEnemyGroups() const { return enemyGroups; }
	const springai::AIFloat3& GetEnemyPos() const { return enemyPos; }
	float GetMaxGroupThreat() const { return enemyGroups[maxThreatGroupIdx].threat; }
	float GetEnemyMobileCost() const { return enemyMobileCost; }

	// quota.attack_merge.rally: clusters of recently seen, armed, mobile enemies of known type. A blob's threat is the
	// plain sum of the threat of such enemies within BLOB_RADIUS of its seed (no weapon ranges, no falloff): it sizes
	// the enemy army the way attack groups size their own. Hottest first, at most MAX_BLOBS; computed once per frame
	// (allied AIs share this manager) for enemies seen within seenFrames.
	// isRaiders (quota.raid_rally): enemies of the raider role only, a separate cache
	struct SEnemyBlob {
		springai::AIFloat3 pos;  // the seed: the enemy with the most such threat around it
		float threat;
		float count;  // its enemy unit count, as squad threat counts them (CEnemyUnit::GetCountWeight)
	};
	const std::vector<SEnemyBlob>& GetBlobs(int frame, int seenFrames, bool isRaiders = false);

	void UpdateAreaUsers(CCircuitAI* ai);
	void SetAreaUpdated(bool value) { isAreaUpdated = value; }
	const std::unordered_set<const STerrainMapArea*>& GetEnemyAreas() const { return enemyAreas; }

private:
	void ReadConfig();
	void KMeansIteration();
	// What an enemy adds to GetMobileCostNoCommFaded, as a share of its cost: 0 when it isn't counted, 1 in radar or
	// LOS, fading by half every halfLife frames since last seen otherwise
	float GetArmyCostWeight(const CEnemyUnit* e, int frame, float halfLife) const;

	struct SGroupData {
		std::vector<SEnemyGroup> enemyGroups;
		springai::AIFloat3 enemyPos;
		int maxThreatGroupIdx;
	};

	void Prepare(SGroupData& groupData);
	void Update();
	void Apply();
	void SwapBuffers();
	SGroupData* GetNextGroupData() {
		return (pGroupData.load() == &groupData0) ? &groupData1 : &groupData0;
	}

	CCircuitAI* circuit;

	EnemyUnits enemyUnits;  // owner
	EnemyFakes enemyFakes;  // owner

	std::vector<CEnemyUnit*> enemyUpdates;
	unsigned int enemyIterator;

	std::vector<ICoreUnit::Id> enemyGarbage;

	std::vector<SEnemyData> hostileDatas;  // immutable during threaded processing
	std::vector<SEnemyData> peaceDatas;  // immutable during threaded processing

	SGroupData groupData0, groupData1;  // Double-buffer for threading
	std::atomic<SGroupData*> pGroupData;
	std::vector<SEnemyGroup>& enemyGroups;
	springai::AIFloat3 enemyPos;
	int maxThreatGroupIdx;
	bool isUpdating;

	float enemyMobileCost;
	float mobileThreat;  // thr_mod.mobile applied
	float staticThreat;  // thr_mod.static applied
	struct SInitThreatMod {
		float inMobile;
		float inStatic;
	} initThrMod;
	float maxAAThreat;
	struct SEnemyInfo {
		float cost;
		float threat;
	};
	std::array<SEnemyInfo, CMaskHandler::GetMaxMasks()> enemyInfos;

	struct SBlobCache {  // GetBlobs
		std::vector<SEnemyBlob> blobs;
		int frame = -1;
		int seenFrames = -1;
	} blobCaches[2];  // all, raiders

	bool isAreaUpdated;
	std::unordered_set<const STerrainMapArea*> enemyAreas;
};

} // namespace circuit

#endif // SRC_CIRCUIT_UNIT_ENEMY_ENEMYMANAGER_H_

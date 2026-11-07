/*
 * EconomyManager.h
 *
 *  Created on: Sep 5, 2014
 *      Author: rlcevg
 */

#ifndef SRC_CIRCUIT_MODULE_ECONOMYMANAGER_H_
#define SRC_CIRCUIT_MODULE_ECONOMYMANAGER_H_

#include "module/Module.h"
#include "util/AvailList.h"

#include "AIFloat3.h"

#include <set>
#include <map>
#include <memory>
#include <functional>
#include <vector>
#include <cstdint>

namespace springai {
	class Resource;
	class Economy;
}

namespace circuit {

#define INCOME_SAMPLES	5
#define HIDDEN_STORAGE	10000.0f

extern const char* RES_NAME_METAL;
extern const char* RES_NAME_ENERGY;

class IBuilderTask;
struct STerrainMapArea;
class CGameTask;
class CEnergyGrid;

class CEconomyManager: public IModule {
public:
	friend class CEconomyScript;

	CEconomyManager(CCircuitAI* circuit);
	virtual ~CEconomyManager();

private:
	void ReadConfig();
	void Init();

public:
	virtual int UnitCreated(CCircuitUnit* unit, CCircuitUnit* builder) override;
	virtual int UnitFinished(CCircuitUnit* unit) override;
	virtual int UnitDamaged(CCircuitUnit* unit, CEnemyInfo* attacker) override;
	virtual int UnitDestroyed(CCircuitUnit* unit, CEnemyInfo* attacker) override;

	springai::Resource* GetMetalRes() const { return metalRes; }
	springai::Resource* GetEnergyRes() const { return energyRes; }
	CEnergyGrid* GetEnergyGrid() const { return energyGrid; }
	CCircuitDef* GetMexDef() const { return mexDef; }
	CCircuitDef* GetLowEnergy(const springai::AIFloat3& pos, float& outMake) const;
	CCircuitDef* GetPylonDef() const { return pylonDef; }
	float GetPylonRange() const { return pylonRange; }
	void AddEnergyDefs(const std::set<CCircuitDef*>& buildDefs);  // add available energy defs
	void RemoveEnergyDefs(const std::set<CCircuitDef*>& buildDefs);
	CCircuitDef* GetDefaultDef() const { return defaultDef; }

	void UpdateResourceIncome();
	float GetAvgMetalIncome() const { return metalIncome; }
	float GetAvgEnergyIncome() const { return energyIncome; }
	float GetEcoFactor() const { return ecoFactor; }
	float GetPullMtoS() const { return pullMtoS; }
	float GetMetalCur();
	float GetMetalPull();
	float GetEnergyCur();
	float GetEnergyPull();
	float GetEnergyUse();
	bool IsMetalEmpty();
	bool IsMetalFull();
	bool IsEnergyStalling();
	bool IsEnergyEmpty();
	bool IsExcessed() const { return metalProduced > metalUsed; }
	int GetBuildDelay() const { return buildDelay; }

	bool IsAllyOpenSpot(int spotId) const;
	bool IsOpenSpot(int spotId) const;
	void SetOpenSpot(int spotId, bool value);
	bool IsIgnorePull(const IBuilderTask* task) const;
	bool IsIgnoreStallingPull(const IBuilderTask* task) const;

	IBuilderTask* MakeEconomyTasks(const springai::AIFloat3& position, CCircuitUnit* unit = nullptr);
	IBuilderTask* UpdateMetalTasks(const springai::AIFloat3& position, CCircuitUnit* unit = nullptr);
	// A visible reclaimable feature with at least 1 metal left
	struct SReclaimFeature {
		springai::AIFloat3 pos;  // corrected onto the map (impulsed flying wrecks)
		float metal;      // of the whole feature
		float metalLeft;
	};
	// Of the given feature ids (COOAICallback::GetFeatureIds/GetFeatureIdsIn)
	std::vector<SReclaimFeature> GetReclaimFeatures(const std::vector<int>& featureIds);
	// Metal left in visible reclaimable features that builderDef from position can reach with no threat on them.
	// Cached per terrain area for 10s.
	float GetReclaimMetal(CCircuitDef* builderDef, const springai::AIFloat3& position);
	IBuilderTask* UpdateReclaimTasks(const springai::AIFloat3& position, CCircuitUnit* unit, bool isNear = true);
	// Reclaim target rank: lower is better. Distance over (metal left)^reclaimValueExp
	float GetReclaimRank(float sqDist, float metalLeft) const;
	// Reachable, and not under enemy threat that beats unit in enemy-held area (set unit's threat type first)
	bool IsReclaimSafe(CCircuitUnit* unit, const springai::AIFloat3& pos) const;
	struct SReclaimTravel {
		float minSec;  // at 1 metal left
		float maxSec;  // at reclaimTravelMetal and above
	};
	// Seconds of travel allowed to a feature with metalLeft on the near reclaim search
	float GetReclaimTravelSec(const SReclaimTravel& travel, float metalLeft) const;
	// Reclaim job takes another constructor while its metal is above reclaimHelperSec of the build power on it
	float GetReclaimHelperSec() const { return reclaimHelperSec; }
	// Before this frame a non-combat constructor needs an escort (or a defended cluster) to build a mex
	int GetMexEscortFrame() const { return mexEscortFrame; }
	IBuilderTask* UpdateEnergyTasks(const springai::AIFloat3& position, CCircuitUnit* unit = nullptr);
	IBuilderTask* UpdateFactoryTasks(const springai::AIFloat3& position, CCircuitUnit* unit = nullptr);
	IBuilderTask* UpdateFactoryTasks();
	IBuilderTask* UpdateStorageTasks();
	IBuilderTask* UpdatePylonTasks();

	// Needs a geothermal vent (energygeo, energyheavygeo); read once per def, no engine call.
	// By def id: allied units' defs can belong to another AI instance on our ally team (CAllyTeam::UpdateFriendlyUnits)
	bool IsGeoDef(const CCircuitDef* cdef) const {
		return (cdef != nullptr) && (geoDefIds.find(cdef->GetId()) != geoDefIds.end());
	}
	// Nearest free geothermal vent for cdef (a geo def) that is safe and,
	// with a unit, reachable by it; -RgtVector if none
	springai::AIFloat3 FindGeoSpot(CCircuitDef* cdef, const springai::AIFloat3& position, CCircuitUnit* unit = nullptr);
	// Known geothermal vents (refreshed at most once a minute; vents are added as they are seen, never removed)
	const std::vector<springai::AIFloat3>& GetGeoSpots();

	void AddMorphee(CCircuitUnit* unit);
	void RemoveMorphee(CCircuitUnit* unit) { morphees.erase(unit); }
	void UpdateMorph();

	void OpenStrategy(const CCircuitDef* facDef, const springai::AIFloat3& pos);

private:
	float GetStorage(springai::Resource* res);
	// Mex job at the best open spot for a builder at position (unit = nullptr: any builder that can reach it)
	IBuilderTask* UpdateMexTasks(const springai::AIFloat3& position, CCircuitUnit* unit);
	// mex_front cluster score for CMetalManager::GetMexToBuild; empty = plain nearest-spot search
	std::function<float (int cluster, float dist)> GetMexFrontScore(float bias) const;
	// Metal of a whole feature of this def, 0 = none or not reclaimable; read once per def
	float GetFeatureDefMetal(int featureDefId);
	void UpdateEconomy();
	void UpdateGeoSpots();
	bool IsGeoFeatureDef(int featureDefId);  // cached per def, like GetFeatureDefMetal
	bool IsGeoSpotTaken(const springai::AIFloat3& spot) const;  // one of our geo jobs already went for it
	void UpdateGeoMorph();

	Handlers2 createdHandler;
	Handlers1 finishedHandler;
	EHandlers destroyedHandler;

	springai::Resource* metalRes;
	springai::Resource* energyRes;
	springai::Economy* economy;
	CEnergyGrid* energyGrid;

	struct SClusterInfo {
		CCircuitUnit* factory;
		int metalFrame;
	};
	std::vector<SClusterInfo> clusterInfos;
	float pylonRange;
	float pylonLinkInc;
	CCircuitDef* pylonDef;  // TODO: Move into CEnergyGrid?

	CCircuitDef* mexDef;
	CCircuitDef* storeDef;
	CCircuitDef* defaultDef;

	// NOTE: MetalManager::SetOpenSpot used by whole allyTeam. Therefore
	//       local spot's state descriptor needed for better expansion
	std::vector<bool> openSpots;  // AI-local metal info
	int mexCount;

	struct SEnergyExt {
		float make;
		int limit;
	};
	CAvailList<SEnergyExt> energyDefs;
	std::unordered_map<CCircuitDef*, int> engyLimits;

	float ecoStep;
	float ecoFactor;
	int switchTime;
	int lastFacFrame;

	struct SEnergyFactorInfo {
		float startFactor;
		float endFactor;
		int startFrame;
		int endFrame;
		float fraction;
	} efInfo;
	float energyFactor;

	struct SBuildDelayInfo {
		int startDelay;
		int endDelay;
		int startFrame;
		int endFrame;
		float fraction;
	} bdInfo;
	int buildDelay;  // frames

	std::vector<float> metalIncomes;
	std::vector<float> energyIncomes;
	int indexRes;
	float metalIncome;
	float energyIncome;
	float metalProduced;
	float metalUsed;
	float metalMod;
	int mexMax;
	bool isAllyMexMax;
	int mexEscortFrame;
	float mexJobMod;
	float mexFrontBias;
	float mexFrontCommBias;  // mex_front.comm_bias: bias when a commander starts the search
	unsigned mexFrontClusters;
	float mexFrontStarted;
	float reclaimValueExp;
	float reclaimHelperSec;
	float reclaimStoreAt;
	float reclaimTravelMetal;
	float reclaimTravelFloor;
	SReclaimTravel reclaimTravel;
	SReclaimTravel reclaimTravelStarved;

	struct SPullMtoS {
		float pull;
		int mex;
		float fraction;
		inline bool operator< (const SPullMtoS& rhs) const { return mex < rhs.mex; }
		inline bool operator() (const SPullMtoS& lhs, const int rhs) { return lhs.mex < rhs; }
	};
	std::vector<SPullMtoS> mspInfos;
	float pullMtoS;  // mobile to static metal pull ratio

	int ecoFrame;
	bool isMetalEmpty;
	bool isMetalFull;
	bool isEnergyStalling;
	bool isEnergyEmpty;

	int metalCurFrame;
	int metalPullFrame;
	int energyCurFrame;
	int energyPullFrame;
	int energyUseFrame;
	float metalCur;
	float metalPull;
	float energyCur;
	float energyPull;
	float energyUse;

	std::shared_ptr<CGameTask> morph;
	std::set<CCircuitUnit*> morphees;

	// Geothermal
	std::set<CCircuitDef::Id> geoDefIds;  // see IsGeoDef
	std::vector<springai::AIFloat3> geoSpots;  // known geothermal vents
	std::vector<int> geoBlockedFrame;  // per geoSpots: the engine said "can't build" there, don't ask again until this frame
	std::vector<int8_t> featureDefIsGeo;  // by FeatureDef id: 1 = vent, 0 = not, -1 = not read yet
	int geoSpotsFrame;  // next refresh of geoSpots
	std::map<STerrainMapArea*, std::pair<float, int>> reclaimMetal;  // area: <metal, next refresh frame>
	std::vector<float> featureDefMetal;  // by FeatureDef id, see GetFeatureDefMetal; < 0 = not read yet
	float geoThreat;  // max enemy threat at a vent to build there
	float geoMorphIncome;  // min avg metal income to morph a geothermal into its upgrade, <0 = never
	int geoMorphFrame;  // earliest frame for geothermal morph
	std::set<CCircuitUnit*> geoMorphees;  // finished geothermals that can morph
	CCircuitUnit* geoMorphing;  // geothermal being morphed (one at a time)
	int geoMorphStart;
};

} // namespace circuit

#endif // SRC_CIRCUIT_MODULE_ECONOMYMANAGER_H_

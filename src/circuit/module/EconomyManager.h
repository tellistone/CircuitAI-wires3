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
	// Metal income without reclaim: our share of the ally team's mex, overdrive and constructor metal, from ZK's
	// overdrive gadget (GetAvgMetalIncome when the gadget doesn't report it)
	float GetEcoMetalIncome() const;
	float GetAvgEnergyIncome() const { return energyIncome; }
	float GetEcoFactor() const { return ecoFactor; }
	float GetPullMtoS() const { return pullMtoS; }
	float GetPullMtoSArmyMult() const { return mspArmyMult; }
	float GetPullMtoSArmyRatio() const { return mspArmyRatio; }  // enemy/ours; < 0 = army factor off (before since, no enemy army)
	// ms_pull while metal stalls (IsMetalStall): a factory waits while the factories' pull, its own build power
	// included, x ms_pull is above the constructors' pull, unless no constructor spends at all. Pulls are the build
	// power on jobs (ITaskManager::GetMetalPull; mex and pylon jobs left out while mex_max >= 1). Constructors hold
	// all new spending in a stall (CBuilderManager stall rule), whatever ms_pull says
	bool IsFactoryOverShare(CCircuitUnit* unit);
	float GetMetalCur();
	float GetMetalPull();
	float GetEnergyCur();
	float GetEnergyPull();
	float GetEnergyUse();
	bool IsMetalEmpty();
	bool IsMetalFull();
	bool IsEnergyStalling();
	bool IsEnergyEmpty();
	// Stall latches (UpdateEconomy): on while a resource runs short, so build power added now only slows every job
	bool IsMetalStall();
	bool IsEnergyStall();
	bool IsStalling() { return IsMetalStall() || IsEnergyStall(); }
	// Short of energy: worse off than metal (IsEnergyStalling), or the energy stall latch is on
	bool IsEnergyShort() { return IsEnergyStalling() || IsEnergyStall(); }
	bool IsExcessed() const { return metalProduced > metalUsed; }
	int GetBuildDelay() const { return buildDelay; }

	bool IsAllyOpenSpot(int spotId) const;
	bool IsOpenSpot(int spotId) const;
	void SetOpenSpot(int spotId, bool value);
	bool IsIgnorePull(const IBuilderTask* task) const;
	bool IsIgnoreStallingPull(const IBuilderTask* task) const;
	// A job the stall rule lets builders start or join: a building that ends the current stall (mex or pylon in a
	// metal stall, energy in an energy stall), or a NOW factory (the replacement, or the early factory)
	bool IsStallFix(const IBuilderTask* task) const;

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
	// Best-ranked feature from position that isAccepted takes, or nullptr. Ordered lazily (heap), so a search
	// that stops early doesn't sort the whole list.
	const SReclaimFeature* FindReclaimFeature(const springai::AIFloat3& position, const std::vector<SReclaimFeature>& features,
			const std::function<bool (const SReclaimFeature&)>& isAccepted) const;
	// Reachable, and not under enemy threat that beats unit in enemy-held area. Sets unit's threat type.
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
	// Queue a geo on the nearest free, safe vent when none is queued and income allows
	IBuilderTask* UpdateGeoTasks(const springai::AIFloat3& position, CCircuitUnit* unit = nullptr);
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
	// Rear spot for a def listed in economy.energy.rear: behind our base, as far from the enemy as the
	// min/max distance from the base and the clear radius allow; safe and, with a unit, reachable. -RgtVector if none
	springai::AIFloat3 FindRearPos(CCircuitDef* cdef, CCircuitUnit* unit);
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
	float reclaimUntakenSec;  // a wreck job nobody takes expires after this (the task timeout runs only while untaken)
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
	// ms_pull multipliers: ms_pull_income [[<metal income>, <mult>], ...] from the start, ms_pull_army
	// {"since": <seconds>, "curve": [[<enemy/ours army metal>, <mult>], ...]} from that game time; empty curve = 1
	std::vector<std::pair<float, float>> mspIncomeCurve;
	std::vector<std::pair<float, float>> mspArmyCurve;
	int mspArmyFrame;
	float mspIncomeMult;
	float mspArmyMult;
	int mspLogFrame;  // next periodic MS_PULL log line
	float mspArmyRatio;  // last army ratio read, < 0 = off
	float mspChatChange;  // ms_pull_chat: relative change that posts an ms_pull chat line, 0 = off
	float mspChatPull;  // ms_pull at the last chat line
	int mspChatFrame;  // no ms_pull chat line before this frame
	float GetOwnPull(CCircuitUnit* unit) const;  // unit's build power unless a job of its already counts it

	int ecoFrame;
	bool isMetalEmpty;
	bool isMetalFull;
	bool isEnergyStalling;
	bool isEnergyEmpty;
	bool isMetalStall;
	bool isEnergyStall;

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
	struct SRearInfo {
		float minDist;  // min distance from the base position
		float maxDist;  // max distance from the base position
		float clear;  // no own building within this radius (blast radius); 0 = no check
		float clearCost;  // buildings cheaper than this don't count for clear
	};
	std::map<CCircuitDef*, SRearInfo> rearInfos;  // economy.energy.rear: energy built behind the base
};

} // namespace circuit

#endif // SRC_CIRCUIT_MODULE_ECONOMYMANAGER_H_

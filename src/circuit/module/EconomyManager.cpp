/*
 * EconomyManager.cpp
 *
 *  Created on: Sep 5, 2014
 *      Author: rlcevg
 */

#include "module/EconomyManager.h"
#include "module/BuilderManager.h"
#include "module/FactoryManager.h"
#include "module/MilitaryManager.h"
#include "setup/SetupManager.h"
#include "resource/MetalManager.h"
#include "resource/EnergyGrid.h"
#include "terrain/TerrainManager.h"
#include "map/ThreatMap.h"
#include "map/InfluenceMap.h"
#include "task/RetreatTask.h"
#include "CircuitAI.h"
#include "util/Scheduler.h"
#include "util/Utils.h"
#include "json/json.h"

#include "spring/SpringCallback.h"
#include "spring/SpringMap.h"

#include "AISCommands.h"
#include "Resource.h"
#include "Economy.h"
#include "Feature.h"
#include "FeatureDef.h"
#include "Team.h"
#include "Log.h"

#include <algorithm>

namespace circuit {

using namespace springai;

#define PYLON_RANGE		500.0f

const char* RES_NAME_METAL = "Metal";
const char* RES_NAME_ENERGY = "Energy";

CEconomyManager::CEconomyManager(CCircuitAI* circuit)
		: IModule(circuit)
		, energyGrid(nullptr)
		, pylonDef(nullptr)
		, mexDef(nullptr)
		, storeDef(nullptr)
		, mexCount(0)
		, lastFacFrame(-1)
		, indexRes(0)
		, metalIncome(.0f)
		, energyIncome(.0f)
		, metalProduced(.0f)
		, metalUsed(.0f)
		, ecoFrame(-1)
		, isMetalEmpty(false)
		, isMetalFull(false)
		, isEnergyStalling(false)
		, isEnergyEmpty(false)
		, metalCurFrame(-1)
		, metalPullFrame(-1)
		, energyCurFrame(-1)
		, energyPullFrame(-1)
		, energyUseFrame(-1)
		, metalCur(.0f)
		, metalPull(.0f)
		, energyCur(.0f)
		, energyPull(.0f)
		, energyUse(.0f)
		, geoSpotsFrame(-1)
		, geoThreat(1.f)
		, geoMorphIncome(-1.f)
		, geoMorphFrame(0)
		, geoMorphing(nullptr)
		, geoMorphStart(-1)
{
	metalRes = circuit->GetCallback()->GetResourceByName(RES_NAME_METAL);
	energyRes = circuit->GetCallback()->GetResourceByName(RES_NAME_ENERGY);
	economy = circuit->GetCallback()->GetEconomy();

	metalIncomes.resize(INCOME_SAMPLES, 4.0f);  // Init metal income
	energyIncomes.resize(INCOME_SAMPLES, 6.0f);  // Init energy income

	// TODO: Use A* ai planning... or sth... STRIPS https://ru.wikipedia.org/wiki/STRIPS
	//       https://ru.wikipedia.org/wiki/Марковский_процесс_принятия_решений

	circuit->GetScheduler()->RunOnInit(std::make_shared<CGameTask>(&CEconomyManager::Init, this));

	/*
	 * factory handlers
	 */
	auto factoryFinishedHandler = [this](CCircuitUnit* unit) {
		lastFacFrame = this->circuit->GetLastFrame();
		int index = this->circuit->GetMetalManager()->FindNearestCluster(unit->GetPos(lastFacFrame));
		if (index >= 0) {
			clusterInfos[index].factory = unit;
		}
	};
	auto factoryDestroyedHandler = [this](CCircuitUnit* unit, CEnemyInfo* attacker) {
		if (unit->GetUnit()->IsBeingBuilt()) {
			return;
		}
		for (auto& info : clusterInfos) {
			if (info.factory == unit) {
				info.factory = nullptr;
			}
		}
	};

	/*
	 * resources
	 */
	auto energyFinishedHandler = [this](CCircuitUnit* unit) {
		const SEnergyExt* energyExt = energyDefs.GetAvailInfo(unit->GetCircuitDef());
		if (energyExt != nullptr) {
			const float income = energyExt->make;
			for (int i = 0; i < INCOME_SAMPLES; ++i) {
				energyIncomes[i] += income;
			}
			energyIncome += income;
		}
	};
	auto mexFinishedHandler = [this](CCircuitUnit* unit) {
		float income = unit->GetUnit()->GetRulesParamFloat("mexIncome", 0.f);
		for (int i = 0; i < INCOME_SAMPLES; ++i) {
			metalIncomes[i] += income;
		}
		metalIncome += income;
	};

	/*
	 * morph & plop
	 */
	auto comFinishedHandler = [this](CCircuitUnit* unit) {
		AddMorphee(unit);
		this->circuit->GetSetupManager()->SetCommander(unit);

		ICoreUnit::Id unitId = unit->GetId();
		this->circuit->GetScheduler()->RunTaskAfter(std::make_shared<CGameTask>([this, unitId]() {
			CCircuitUnit* unit = this->circuit->GetTeamUnit(unitId);
			if (unit == nullptr) {
				return;
			}
			int frame = this->circuit->GetLastFrame();
			bool isStart = (frame < FRAMES_PER_SEC * 10);
			AIFloat3 buildPos = -RgtVector;
			if (unit->GetUnit()->GetRulesParamFloat("facplop", 0) == 1) {
				// TODO: check factory's customparam ploppable=1
				const AIFloat3& pos = unit->GetPos(frame);
				CCircuitDef* facDef = this->circuit->GetFactoryManager()->GetFactoryToBuild(pos, isStart);
				if (facDef != nullptr) {
					// Enqueue factory
					CTerrainManager* terrainMgr = this->circuit->GetTerrainManager();
					const float range = std::max(facDef->GetDef()->GetXSize(), facDef->GetDef()->GetZSize())
							* SQUARE_SIZE / 2 * 1.4f + unit->GetCircuitDef()->GetRadius();
					buildPos = terrainMgr->ShiftPos(facDef, pos, range, true);
					CBuilderManager* builderMgr = this->circuit->GetBuilderManager();
					IBuilderTask* task = builderMgr->EnqueueFactory(IBuilderTask::Priority::NOW, facDef, buildPos,
																	SQUARE_SIZE, true, true, 0);
					static_cast<ITaskManager*>(builderMgr)->AssignTask(unit, task);
				}
			}

			if (!isStart) {
				return;
			}
			int morphFrame = this->circuit->GetSetupManager()->GetMorphFrame(unit->GetCircuitDef());
			if (morphFrame >= 0) {
				this->circuit->GetScheduler()->RunTaskAt(std::make_shared<CGameTask>([this, unitId]() {
					// Force commander level 0 to morph
					CCircuitUnit* unit = this->circuit->GetTeamUnit(unitId);
					if ((unit != nullptr) && (unit->GetTask() != nullptr) &&
						(unit->GetTask()->GetType() != IUnitTask::Type::PLAYER))
					{
						const std::map<std::string, std::string>& customParams = unit->GetCircuitDef()->GetDef()->GetCustomParams();
						auto it = customParams.find("level");
						if ((it != customParams.end()) && (utils::string_to_int(it->second) <= 1)) {
							unit->Upgrade();  // Morph();
						}
					}
				}), morphFrame);
			}
		}), FRAMES_PER_SEC);
	};
	auto comDestroyedHandler = [this](CCircuitUnit* unit, CEnemyInfo* attacker) {
		RemoveMorphee(unit);

		CSetupManager* setupMgr = this->circuit->GetSetupManager();
		CCircuitUnit* commander = setupMgr->GetCommander();
		if (commander == unit) {
			setupMgr->SetCommander(nullptr);
		}
	};

	float maxAreaDivCost = .0f;
	float maxStoreDivCost = .0f;
	CCircuitDef* commDef = circuit->GetSetupManager()->GetCommChoice();

	for (CCircuitDef& cdef : circuit->GetCircuitDefs()) {
		const std::map<std::string, std::string>& customParams = cdef.GetDef()->GetCustomParams();

		if (!cdef.IsMobile()) {
			if (cdef.GetDef()->IsNeedGeo()) {
				geoDefIds.insert(cdef.GetId());
			}

			if (commDef->CanBuild(&cdef)) {
				// pylon
				auto it = customParams.find("pylonrange");
				if (it != customParams.end()) {
					const float range = utils::string_to_float(it->second);
					float areaDivCost = M_PI * SQUARE(range) / cdef.GetCostM();
					if (maxAreaDivCost < areaDivCost) {
						maxAreaDivCost = areaDivCost;
						pylonDef = &cdef;  // armestor
						pylonRange = range;
					}
				}

				// storage
				float storeDivCost = cdef.GetDef()->GetStorage(metalRes) / cdef.GetCostM();
				if (maxStoreDivCost < storeDivCost) {
					maxStoreDivCost = storeDivCost;
					storeDef = &cdef;  // armmstor
				}

				// mex
				// BA: float metalConverts = unitDef->GetMakesResource(metalRes);
				//     float metalExtracts = unitDef->GetExtractsResource(metalRes);
				//     float netMetal = unitDef->GetResourceMake(metalRes) - unitDef->GetUpkeep(metalRes);
				if ((((it = customParams.find(               "ismex")) != customParams.end()) && (utils::string_to_int(it->second) == 1))
				||  (((it = customParams.find("metal_extractor_mult")) != customParams.end()) && (utils::string_to_int(it->second) > 0))) {
					finishedHandler[cdef.GetId()] = mexFinishedHandler;
					mexDef = &cdef;  // cormex
					cdef.SetIsMex(true);
				}
			}

			// factory
			if (cdef.GetDef()->IsBuilder() && cdef.IsBuilder()) {
				finishedHandler[cdef.GetId()] = factoryFinishedHandler;
				destroyedHandler[cdef.GetId()] = factoryDestroyedHandler;
			}

			// energy
			// BA: float netEnergy = unitDef->GetResourceMake(energyRes) - unitDef->GetUpkeep(energyRes);
			auto it = customParams.find("income_energy");
			if ((it != customParams.end()) && (utils::string_to_float(it->second) > 1)) {
				finishedHandler[cdef.GetId()] = energyFinishedHandler;
				energyDefs.AddDef(&cdef);

				// geothermal that morphs into an upgrade (energygeo -> energyheavygeo)
				if (IsGeoDef(&cdef) && (customParams.find("morphto") != customParams.end())) {
					finishedHandler[cdef.GetId()] = [this, energyFinishedHandler](CCircuitUnit* unit) {
						energyFinishedHandler(unit);
						geoMorphees.insert(unit);
					};
					destroyedHandler[cdef.GetId()] = [this](CCircuitUnit* unit, CEnemyInfo* attacker) {
						geoMorphees.erase(unit);
						if (geoMorphing == unit) {
							geoMorphing = nullptr;
						}
					};
				}
			}

		} else {

			// commander
			if (cdef.IsRoleComm()) {
				finishedHandler[cdef.GetId()] = comFinishedHandler;
				destroyedHandler[cdef.GetId()] = comDestroyedHandler;
			}
		}
	}

	ReadConfig();

	if (mexDef == nullptr) {
		mexDef = defaultDef;
	}
	if (pylonDef == nullptr) {
		pylonDef = defaultDef;
		pylonRange = PYLON_RANGE;
	}
}

CEconomyManager::~CEconomyManager()
{
	delete metalRes;
	delete energyRes;
	delete economy;
}

void CEconomyManager::ReadConfig()
{
	const Json::Value& root = circuit->GetSetupManager()->GetConfig();
	const std::string& cfgName = circuit->GetSetupManager()->GetConfigName();
	const Json::Value& econ = root["economy"];
	ecoStep = econ.get("eps_step", 0.25f).asFloat();
	ecoFactor = (circuit->GetAllyTeam()->GetSize() - 1.0f) * ecoStep + 1.0f;
	metalMod = (1.f - econ.get("excess", -1.f).asFloat());
	const Json::Value& recl = econ["reclaim"];
	reclaimValueExp = recl.get("value_exp", 0.5f).asFloat();
	reclaimHelperSec = std::max(recl.get("helper_sec", 20.f).asFloat(), 0.f);
	reclaimUntakenSec = std::max(recl.get("untaken_sec", 30.f).asFloat(), 1.f);
	reclaimStoreAt = recl.get("store_at", 0.7f).asFloat();
	const Json::Value& travel = recl["travel"];
	reclaimTravelMetal = travel.get("metal", 2000.f).asFloat();
	reclaimTravelFloor = travel.get("floor", 5.f).asFloat();
	auto readTravel = [&travel](const char* key, float defMaxSec) {
		const Json::Value& sec = travel[key];  // [<seconds at 1 metal>, <seconds at travel.metal>]
		return SReclaimTravel{sec.get((unsigned)0, 1.f).asFloat(), sec.get((unsigned)1, defMaxSec).asFloat()};
	};
	reclaimTravel = readTravel("sec", 30.f);
	reclaimTravelStarved = readTravel("starved_sec", 60.f);
	const Json::Value& swch = econ["switch"];
	const int minSwitch = swch.get((unsigned)0, 800).asInt();
	const int maxSwitch = swch.get((unsigned)1, 900).asInt();
	switchTime = (minSwitch + rand() % (maxSwitch - minSwitch + 1)) * FRAMES_PER_SEC;

	{
		const Json::Value& bd = econ["build_delay"];
		float value = bd[0].get((unsigned)0, -1.f).asFloat();
		bdInfo.startDelay = (value > 0.f) ? (value * FRAMES_PER_SEC) : 0;
		bdInfo.startFrame = bd[0].get((unsigned)1, 0).asInt() * FRAMES_PER_SEC;
		value = bd[1].get((unsigned)0, -1.f).asFloat();
		bdInfo.endDelay = (value > 0.f) ? (value * FRAMES_PER_SEC) : 0;
		bdInfo.endFrame = bd[1].get((unsigned)1, 0).asInt() * FRAMES_PER_SEC;
		bdInfo.fraction = (bdInfo.endFrame != bdInfo.startFrame)
				? float(bdInfo.endDelay - bdInfo.startDelay) / (bdInfo.endFrame - bdInfo.startFrame)
				: 0.f;
		buildDelay = bdInfo.startDelay;
	}

	const Json::Value& energy = econ["energy"];
	{
		const Json::Value& factor = energy["factor"];
		efInfo.startFactor = factor[0].get((unsigned)0, 0.5f).asFloat();
		efInfo.startFrame = factor[0].get((unsigned)1, 300 ).asInt() * FRAMES_PER_SEC;
		efInfo.endFactor = factor[1].get((unsigned)0, 2.0f).asFloat();
		efInfo.endFrame = factor[1].get((unsigned)1, 3600).asInt() * FRAMES_PER_SEC;
		efInfo.fraction = (efInfo.endFrame != efInfo.startFrame)
				? (efInfo.endFactor - efInfo.startFactor) / (efInfo.endFrame - efInfo.startFrame)
				: 0.f;
		energyFactor = efInfo.startFactor;
	}

	pylonLinkInc = energy.get("link_inc", 16.0f).asFloat();

	{
		const Json::Value& geo = energy["geo"];
		geoThreat = geo.get("threat", 1.0f).asFloat();
		const Json::Value& morph = geo["morph"];
		geoMorphIncome = morph.get((unsigned)0, -1.f).asFloat();
		geoMorphFrame = morph.get((unsigned)1, 0).asInt() * FRAMES_PER_SEC;
	}

	std::vector<std::pair<std::string, int>> engies;
	std::string type = circuit->GetTerrainManager()->IsWaterMap() ? "water" : "land";
	const Json::Value& surf = energy[type];
	for (const std::string& engy : surf.getMemberNames()) {
		const int min = surf[engy][0].asInt();
		const int max = surf[engy].get(1, min).asInt();
		const int limit = min + rand() % (max - min + 1);
		engies.push_back(std::make_pair(engy, limit));
	}

	for (unsigned i = 0; i < engies.size(); ++i) {
		const char* name = engies[i].first.c_str();
		CCircuitDef* cdef = circuit->GetCircuitDef(name);
		if (cdef == nullptr) {
			circuit->LOG("CONFIG %s: has unknown UnitDef '%s'", cfgName.c_str(), name);
			continue;
		}
		engyLimits[cdef] = engies[i].second;
	}

	// NOTE: Must have
	defaultDef = circuit->GetCircuitDef(econ["default"].asCString());
	if (defaultDef == nullptr) {
		throw CException("economy.default");
	}
}

void CEconomyManager::Init()
{
	energyGrid = circuit->GetAllyTeam()->GetEnergyGrid().get();

	const size_t clSize = circuit->GetMetalManager()->GetClusters().size();
	clusterInfos.resize(clSize, {nullptr, -FRAMES_PER_SEC});
	const size_t spSize = circuit->GetMetalManager()->GetSpots().size();
	openSpots.resize(spSize, true);

	const Json::Value& econ = circuit->GetSetupManager()->GetConfig()["economy"];
	const Json::Value& mexCap = econ["mex_max"];
	const float mm = mexCap.get((unsigned)0, 2.f).asFloat();
	mexMax = (mm < 1.f) ? decltype(mexMax)(mm * spSize) : std::numeric_limits<decltype(mexMax)>::max();
	isAllyMexMax = mexCap.get((unsigned)1, true).asBool();
	mexJobMod = std::max(econ.get("mex_jobs", 1.5f).asFloat(), 0.f);
	const Json::Value& mexFront = econ["mex_front"];
	mexFrontBias = utils::clamp(mexFront.get("bias", 0.15f).asFloat(), -0.9f, 0.9f);
	mexFrontCommBias = utils::clamp(mexFront.get("comm_bias", mexFrontBias).asFloat(), -0.9f, 0.9f);  // missing = bias
	mexFrontClusters = std::max(mexFront.get("clusters", 3).asInt(), 1);
	mexFrontStarted = utils::clamp(mexFront.get("started", 0.3f).asFloat(), 0.f, 0.9f);

	// mex_escort: [[<map size>, <seconds>], ...], linear between points, flat beyond the ends; missing = 300 s.
	// Map size is sqrt(width * height) in 512-elmo map units, so 8x8 = 8 and 16x16 = 16.
	{
		std::vector<std::pair<float, float>> curve;
		const Json::Value& escort = econ["mex_escort"];
		if (escort.isArray()) {
			for (const Json::Value& pt : escort) {
				if (pt.isArray() && (pt.size() >= 2) && pt[0].isNumeric() && pt[1].isNumeric()) {
					curve.emplace_back(pt[0].asFloat(), pt[1].asFloat());
				}
			}
		}
		std::sort(curve.begin(), curve.end());
		const float mapSize = std::sqrt(float(CTerrainManager::GetTerrainWidth()) * CTerrainManager::GetTerrainHeight()) / 512.f;
		const float escortSec = std::max(utils::eval_curve(curve, mapSize, 300.f), 0.f);
		mexEscortFrame = escortSec * FRAMES_PER_SEC;
		circuit->LOG("mex_escort: map size %.1f -> %.0f seconds", mapSize, escortSec);
	}

	const Json::Value& pull = econ["ms_pull"];
	mspInfos.reserve(pull.size());
	mspInfos.push_back(SPullMtoS {
		.pull = pull[0].get((unsigned)0, 1.0f).asFloat(),
		.mex = (int)(pull[0].get((unsigned)1, 0.0f).asFloat() * spSize),
		.fraction = 0.f
	});
	for (unsigned i = 1; i < pull.size(); ++i) {
		SPullMtoS mspInfoEnd;
		mspInfoEnd.pull = pull[i].get((unsigned)0, 0.25f).asFloat();
		mspInfoEnd.mex = pull[i].get((unsigned)1, 0.75f).asFloat() * spSize;
		mspInfoEnd.fraction = 0.f;
		mspInfos.push_back(mspInfoEnd);
		SPullMtoS& mspInfoBegin = mspInfos[i - 1];
		mspInfoBegin.fraction = (mspInfoEnd.mex != mspInfoBegin.mex)
				? (mspInfoEnd.pull - mspInfoBegin.pull) / (mspInfoEnd.mex - mspInfoBegin.mex)
				: 0.f;
	}
	std::sort(mspInfos.begin(), mspInfos.end());
	pullMtoS = mspInfos.front().pull;

	CSetupManager::StartFunc subinit = [this](const AIFloat3& pos) {
		metalProduced = economy->GetCurrent(metalRes) * metalMod;

		CScheduler* scheduler = circuit->GetScheduler().get();
		CAllyTeam* allyTeam = circuit->GetAllyTeam();
		if (circuit->IsCommMerge() && !circuit->IsLoadSave()) {
			const int spotId = circuit->GetMetalManager()->FindNearestSpot(pos);
			const int clusterId = (spotId < 0) ? -1 : circuit->GetMetalManager()->GetCluster(spotId);
			int ownerId = allyTeam->GetClusterTeam(clusterId).teamId;
			if (ownerId < 0) {
				ownerId = circuit->GetTeamId();
				allyTeam->OccupyCluster(clusterId, ownerId);
			} else if (ownerId != circuit->GetTeamId()) {
				// Resign
				circuit->Resign(ownerId, economy);
				return;
			}
			// FIXME: DEBUG
//			CCircuitUnit* commander = circuit->GetSetupManager()->GetCommander();
//			if (commander == nullptr) {
//				commander = circuit->GetTeamUnits().begin()->second;
//			}
//			int ownerId = allyTeam->GetAreaTeam(commander->GetArea()).teamId;
//			if (ownerId < 0) {
//				ownerId = circuit->GetTeamId();
//				allyTeam->OccupyArea(commander->GetArea(), ownerId);
//			} else if (ownerId != circuit->GetTeamId()) {
//				// Resign
//				circuit->Resign(ownerId, economy);
//				return;
//			}
			// FIXME: DEBUG
		}

		scheduler->RunTaskAfter(std::make_shared<CGameTask>([this]() {
			ecoFactor = (circuit->GetAllyTeam()->GetAliveSize() - 1.0f) * ecoStep + 1.0f;
		}), FRAMES_PER_SEC * 10);

		const int interval = allyTeam->GetSize() * FRAMES_PER_SEC;
		auto update = static_cast<IBuilderTask* (CEconomyManager::*)(void)>(&CEconomyManager::UpdateFactoryTasks);
		scheduler->RunTaskEvery(std::make_shared<CGameTask>(update, this),
								interval, circuit->GetSkirmishAIId() + 0 + 10 * interval);
		scheduler->RunTaskEvery(std::make_shared<CGameTask>(&CEconomyManager::UpdateStorageTasks, this),
								interval, circuit->GetSkirmishAIId() + 1 + interval / 2);

		scheduler->RunTaskEvery(std::make_shared<CGameTask>(&CEconomyManager::UpdateResourceIncome, this), TEAM_SLOWUPDATE_RATE);

		if (geoMorphIncome >= 0.f) {
			scheduler->RunTaskEvery(std::make_shared<CGameTask>(&CEconomyManager::UpdateGeoMorph, this),
									FRAMES_PER_SEC * 10, circuit->GetSkirmishAIId() + 2);
		}
	};

	circuit->GetSetupManager()->ExecOnFindStart(subinit);
}

int CEconomyManager::UnitCreated(CCircuitUnit* unit, CCircuitUnit* builder)
{
	auto search = createdHandler.find(unit->GetCircuitDef()->GetId());
	if (search != createdHandler.end()) {
		search->second(unit, builder);
	}

	return 0; //signaling: OK
}

int CEconomyManager::UnitFinished(CCircuitUnit* unit)
{
	auto search = finishedHandler.find(unit->GetCircuitDef()->GetId());
	if (search != finishedHandler.end()) {
		search->second(unit);
	}

	return 0; //signaling: OK
}

int CEconomyManager::UnitDamaged(CCircuitUnit* unit, CEnemyInfo* attacker)
{
	// NOTE: If more actions should be done then consider moving into damagedHandler
	if (unit->IsMorphing() && (unit->GetUnit()->GetHealth() < unit->GetUnit()->GetMaxHealth() * 0.5f)) {
		unit->StopUpgrade();  // StopMorph();
		AddMorphee(unit);
	}

	return 0; //signaling: OK
}

int CEconomyManager::UnitDestroyed(CCircuitUnit* unit, CEnemyInfo* attacker)
{
	auto search = destroyedHandler.find(unit->GetCircuitDef()->GetId());
	if (search != destroyedHandler.end()) {
		search->second(unit, attacker);
	}

	return 0; //signaling: OK
}

CCircuitDef* CEconomyManager::GetLowEnergy(const AIFloat3& pos, float& outMake) const
{
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	const int frame = circuit->GetLastFrame();
	return energyDefs.GetWorstDef([this, frame, terrainMgr, &pos, &outMake](CCircuitDef* cdef, const SEnergyExt& data) {
		if (cdef->IsAvailable(frame) && !IsGeoDef(cdef) && terrainMgr->CanBeBuiltAtSafe(cdef, pos)) {
			outMake = data.make;
			return true;
		}
		return false;
	});
}

void CEconomyManager::AddEnergyDefs(const std::set<CCircuitDef*>& buildDefs)
{
	energyDefs.AddDefs(buildDefs, [this](CCircuitDef* cdef, SEnergyExt& data) -> float {
		data.make = utils::string_to_float(cdef->GetDef()->GetCustomParams().find("income_energy")->second);
		auto lit = engyLimits.find(cdef);
		data.limit = (lit != engyLimits.end()) ? lit->second : 0;
		// TODO: Instead of plain sizeX, sizeZ use AI's yardmap size
		return SQUARE(data.make) / ((cdef->GetCostM()/* + cdef->GetCostE() * 0.05f*/) * cdef->GetDef()->GetXSize() * cdef->GetDef()->GetZSize());
	});

	// DEBUG
//	for (const auto& ei : energyDefs.GetInfos()) {
//		circuit->LOG("%s | costM=%f | costE=%f | make=%f | efficiency=%f | limit=%i", ei.cdef->GetDef()->GetName(),
//				ei.cdef->GetCostM(), ei.cdef->GetCostE(), ei.data.make, ei.score, ei.data.limit);
//	}
}

void CEconomyManager::RemoveEnergyDefs(const std::set<CCircuitDef*>& buildDefs)
{
	energyDefs.RemoveDefs(buildDefs);

	// DEBUG
//	for (const auto& ei : energyDefs.GetInfos()) {
//		circuit->LOG("%s | costM=%f | costE=%f | make=%f | efficiency=%f | limit=%i", ei.cdef->GetDef()->GetName(),
//				ei.cdef->GetCostM(), ei.cdef->GetCostE(), ei.data.make, ei.score, ei.data.limit);
//	}
}

void CEconomyManager::UpdateResourceIncome()
{
	SCOPED_TIME(circuit, __PRETTY_FUNCTION__);
	float oddEnergyIncome = circuit->GetTeam()->GetRulesParamFloat("OD_energyIncome", 0.f);
	float oddEnergyChange = circuit->GetTeam()->GetRulesParamFloat("OD_energyChange", 0.f);

	energyIncomes[indexRes] = economy->GetIncome(energyRes) + oddEnergyIncome - std::max(.0f, oddEnergyChange);
	metalIncomes[indexRes] = economy->GetIncome(metalRes) + economy->GetReceived(metalRes);
	++indexRes %= INCOME_SAMPLES;

	metalIncome = .0f;
	for (int i = 0; i < INCOME_SAMPLES; i++) {
		metalIncome += metalIncomes[i];
	}
	metalIncome /= INCOME_SAMPLES;

	energyIncome = .0f;
	for (int i = 0; i < INCOME_SAMPLES; i++) {
		energyIncome += energyIncomes[i];
	}
	energyIncome /= INCOME_SAMPLES;

	metalProduced += metalIncome * metalMod;
	metalUsed += economy->GetUsage(metalRes);
}

float CEconomyManager::GetMetalCur()
{
	if (metalCurFrame/* + TEAM_SLOWUPDATE_RATE*/ < circuit->GetLastFrame()) {
		metalCurFrame = circuit->GetLastFrame();
		metalCur = economy->GetCurrent(metalRes);
	}
	return metalCur;
}

float CEconomyManager::GetMetalPull()
{
	if (metalPullFrame/* + TEAM_SLOWUPDATE_RATE*/ < circuit->GetLastFrame()) {
		metalPullFrame = circuit->GetLastFrame();
		metalPull = economy->GetPull(metalRes) + circuit->GetTeam()->GetRulesParamFloat("extraMetalPull", 0.f);
	}
	return metalPull;
}

float CEconomyManager::GetEnergyCur()
{
	if (energyCurFrame/* + TEAM_SLOWUPDATE_RATE*/ < circuit->GetLastFrame()) {
		energyCurFrame = circuit->GetLastFrame();
		energyCur = economy->GetCurrent(energyRes);
	}
	return energyCur;
}

float CEconomyManager::GetEnergyPull()
{
	if (energyPullFrame/* + TEAM_SLOWUPDATE_RATE*/ < circuit->GetLastFrame()) {
		energyPullFrame = circuit->GetLastFrame();
		float extraEnergyPull = circuit->GetTeam()->GetRulesParamFloat("extraEnergyPull", 0.f);
//		float oddEnergyOverdrive = circuit->GetTeam()->GetRulesParamFloat("OD_energyOverdrive", 0.f);
//		float oddEnergyChange = circuit->GetTeam()->GetRulesParamFloat("OD_energyChange", 0.f);
//		float extraChange = std::min(.0f, oddEnergyChange) - std::min(.0f, oddEnergyOverdrive);
//		float teamEnergyWaste = circuit->GetTeam()->GetRulesParamFloat("OD_team_energyWaste", 0.f);
//		float numAllies = circuit->GetTeam()->GetRulesParamFloat("OD_allies", 1.f);
//		if (numAllies < 1.f) {
//			numAllies = 1.f;
//		}
		energyPull = economy->GetPull(energyRes) + extraEnergyPull/* + extraChange - teamEnergyWaste / numAllies*/;
	}
	return energyPull;
}

float CEconomyManager::GetEnergyUse()
{
	if (energyUseFrame/* + TEAM_SLOWUPDATE_RATE*/ < circuit->GetLastFrame()) {
		energyUseFrame = circuit->GetLastFrame();
		energyUse = economy->GetUsage(energyRes);
	}
	return energyUse;
}

bool CEconomyManager::IsMetalEmpty()
{
	UpdateEconomy();
	return isMetalEmpty;
}

bool CEconomyManager::IsMetalFull()
{
	UpdateEconomy();
	return isMetalFull;
}

bool CEconomyManager::IsEnergyStalling()
{
	UpdateEconomy();
	return isEnergyStalling;
}

bool CEconomyManager::IsEnergyEmpty()
{
	UpdateEconomy();
	return isEnergyEmpty;
}

bool CEconomyManager::IsAllyOpenSpot(int spotId) const
{
	return IsOpenSpot(spotId) && circuit->GetMetalManager()->IsOpenSpot(spotId);
}

bool CEconomyManager::IsOpenSpot(int spotId) const
{
	return openSpots[spotId] && ((isAllyMexMax ? circuit->GetMetalManager()->GetMexCount() : mexCount) < mexMax);
}

void CEconomyManager::SetOpenSpot(int spotId, bool value)
{
	if (openSpots[spotId] == value) {
		return;
	}
	openSpots[spotId] = value;
	value ? --mexCount : ++mexCount;
}

bool CEconomyManager::IsIgnorePull(const IBuilderTask* task) const
{
	if (mexMax != std::numeric_limits<decltype(mexMax)>::max()) {
		return false;
	}
	return ((task->GetBuildType() == IBuilderTask::BuildType::MEX) ||
			(task->GetBuildType() == IBuilderTask::BuildType::PYLON));
}

bool CEconomyManager::IsIgnoreStallingPull(const IBuilderTask* task) const
{
//	if (mexMax != std::numeric_limits<decltype(mexMax)>::max()) {
//		return false;  // NOTE: may completely block expansion
//	}
	if ((task->GetBuildType() == IBuilderTask::BuildType::MEX) ||
		(task->GetBuildType() == IBuilderTask::BuildType::PYLON))
	{
		return true;
	}
	return ((task->GetBuildType() == IBuilderTask::BuildType::ENERGY) &&
			circuit->GetEconomyManager()->IsEnergyStalling());
}

IBuilderTask* CEconomyManager::MakeEconomyTasks(const AIFloat3& position, CCircuitUnit* unit)
{
	CBuilderManager* builderMgr = circuit->GetBuilderManager();
	if (!builderMgr->CanEnqueueTask()) {
		return nullptr;
	}
	CMetalManager* metalMgr = circuit->GetMetalManager();
	int index = metalMgr->FindNearestCluster(position);
	if ((index < 0) || (clusterInfos[index].metalFrame + FRAMES_PER_SEC >= circuit->GetLastFrame())) {
		return nullptr;
	}
	clusterInfos[index].metalFrame = circuit->GetLastFrame();

	return UpdateMetalTasks(position, unit);
}

IBuilderTask* CEconomyManager::UpdateMetalTasks(const AIFloat3& position, CCircuitUnit* unit)
{
	if (!circuit->GetBuilderManager()->CanEnqueueTask(16)) {
		return nullptr;
	}

	// Energy stall: queue energy, but keep expanding. Mex jobs then rank as NORMAL (CBuilderManager::GetJobRank).
	// Mex and reclaim are both searched on every pass (reclaim costs no energy, so a stall doesn't stop it);
	// the job picker then chooses between them by travel and priority.
	// Geo isn't only a stall fix: a free vent is the cheapest energy there is, so look for one on every pass
	IBuilderTask* task = IsEnergyStalling() ? UpdateEnergyTasks(position, unit) : UpdateGeoTasks(position, unit);
	IBuilderTask* mexTask = UpdateMexTasks(position, unit);
	IBuilderTask* reclaimTask = UpdateReclaimTasks(position, unit);

	return (task != nullptr) ? task : (mexTask != nullptr) ? mexTask : reclaimTask;
}

IBuilderTask* CEconomyManager::UpdateMexTasks(const AIFloat3& position, CCircuitUnit* unit)
{
	if (!mexDef->IsAvailable(circuit->GetLastFrame())) {
		return nullptr;
	}
	CBuilderManager* builderMgr = circuit->GetBuilderManager();
	const float cost = mexDef->GetCostM();
	const unsigned maxCount = (builderMgr->GetBuildPower() / cost * 8 + 2) * mexJobMod;
	if (builderMgr->GetTasks(IBuilderTask::BuildType::MEX).size() >= maxCount) {
		return nullptr;
	}

	CMetalManager* metalMgr = circuit->GetMetalManager();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	const CMetalData::Metals& spots = metalMgr->GetSpots();
	CMap* map = circuit->GetMap();
	// Open, not ours yet, no threat, reachable (by this unit, or by any builder) and free to build on
	CMetalData::PointPredicate predicate = [this, &spots, map, terrainMgr, builderMgr, unit](int index) {
		const AIFloat3& pos = spots[index].position;
		return (IsAllyOpenSpot(index) && !terrainMgr->IsZoneAlly(pos)
				&& terrainMgr->CanBeBuiltAtSafe(mexDef, pos)  // hostile environment
				&& ((unit != nullptr)
					? terrainMgr->CanReachAtSafe(unit, pos, unit->GetCircuitDef()->GetBuildDistance())
					: builderMgr->IsBuilderInArea(mexDef, pos))
				&& map->IsPossibleToBuildAt(mexDef->GetDef(), pos, UNIT_COMMAND_BUILD_NO_FACING));
	};
	// A commander's search uses its own bias (mex_front.comm_bias)
	const bool isComm = (unit != nullptr) && unit->GetCircuitDef()->IsRoleComm();
	const CMetalManager::ClusterScore score = GetMexFrontScore(isComm ? mexFrontCommBias : mexFrontBias);
	const int index = score
			? metalMgr->GetMexToBuild(position, predicate, mexFrontClusters, score)
			: metalMgr->GetMexToBuild(position, predicate);
	if (index == -1) {
		return nullptr;
	}

	const AIFloat3& pos = spots[index].position;
	IBuilderTask* task = builderMgr->EnqueueTask(IBuilderTask::Priority::HIGH, mexDef, pos, IBuilderTask::BuildType::MEX, cost, .0f);
	task->SetBuildPos(pos);
	SetOpenSpot(index, false);
	return task;
}

CMetalManager::ClusterScore CEconomyManager::GetMexFrontScore(float bias) const
{
	const AIFloat3& basePos = circuit->GetSetupManager()->GetBasePos();
	const AIFloat3& lanePos = circuit->GetSetupManager()->GetLanePos();
	if (((bias == 0.f) && (mexFrontClusters <= 1)) || !utils::is_valid(basePos) || !utils::is_valid(lanePos)
		|| (basePos.SqDistance2D(lanePos) <= SQUARE(SQUARE_SIZE)))
	{
		return nullptr;
	}
	// Pick the cluster, then its nearest open spot. Cluster score: distance to its nearest open spot
	// x (1 - bias x progress) x (1 - started) if we already queued or built a mex there.
	// progress = the cluster centre's projection on base->lane over the base-lane distance,
	// clamped to [-1, 1] (base = 0, lane = 1). The started bonus keeps clusters finishing one at a time.
	const CMetalManager* metalMgr = circuit->GetMetalManager();
	const AIFloat3 laneDir = lanePos - basePos;
	const float sqLaneLen = laneDir.SqLength2D();
	const float startedMod = 1.f - mexFrontStarted;
	return [metalMgr, basePos, laneDir, sqLaneLen, bias, startedMod](int cluster, float dist) {
		const AIFloat3& center = metalMgr->GetClusters()[cluster].position;
		const float progress = utils::clamp((center - basePos).dot2D(laneDir) / sqLaneLen, -1.f, 1.f);
		const float mod = metalMgr->IsClusterStarted(cluster) ? startedMod : 1.f;
		return dist * (1.f - bias * progress) * mod;
	};
}

std::vector<CEconomyManager::SReclaimFeature> CEconomyManager::GetReclaimFeatures(const std::vector<int>& featureIds)
{
	// Id-only callbacks and the per-def metal cache: no wrapper objects, and trees cost one call each
	COOAICallback* clb = circuit->GetCallback();
	std::vector<SReclaimFeature> features;
	for (int featureId : featureIds) {
		const float metal = GetFeatureDefMetal(clb->Feature_GetDefId(featureId));
		if (metal < 1.0f) {
			continue;
		}
		const float metalLeft = metal * clb->Feature_GetReclaimLeft(featureId);
		if (metalLeft < 1.0f) {
			continue;
		}
		AIFloat3 pos = clb->Feature_GetPosition(featureId);
		CTerrainManager::CorrectPosition(pos);  // Impulsed flying feature
		features.push_back({pos, metal, metalLeft});
	}
	return features;
}

float CEconomyManager::GetFeatureDefMetal(int featureDefId)
{
	if (featureDefId < 0) {
		return 0.f;  // not visible
	}
	if ((size_t)featureDefId >= featureDefMetal.size()) {
		featureDefMetal.resize(featureDefId + 1, -1.f);
	}
	float& metal = featureDefMetal[featureDefId];
	if (metal < 0.f) {  // defs don't change during a game
		COOAICallback* clb = circuit->GetCallback();
		metal = clb->FeatureDef_IsReclaimable(featureDefId)
				? std::max(clb->FeatureDef_GetContainedResource(featureDefId, metalRes->GetResourceId()), 0.f)
				: 0.f;
	}
	return metal;
}

float CEconomyManager::GetReclaimMetal(CCircuitDef* builderDef, const AIFloat3& position)
{
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	STerrainMapArea* area = terrainMgr->GetCurrentMapArea(builderDef, position).first;  // nullptr = flying
	const int frame = circuit->GetLastFrame();
	auto it = reclaimMetal.find(area);
	if ((it != reclaimMetal.end()) && (frame < it->second.second)) {
		return it->second.first;
	}

	// Reach + any-threat test by area (no unit at hand): stricter than UpdateReclaimTasks, which allows threat under our cover
	const float range = builderDef->GetBuildDistance();
	float metal = 0.f;
	for (const SReclaimFeature& feature : GetReclaimFeatures(circuit->GetCallback()->GetFeatureIds())) {
		if (terrainMgr->CanMobileReachAtSafe(area, feature.pos, range)) {
			metal += feature.metalLeft;
		}
	}

	reclaimMetal[area] = std::make_pair(metal, frame + FRAMES_PER_SEC * 10);
	return metal;
}

IBuilderTask* CEconomyManager::UpdateReclaimTasks(const AIFloat3& position, CCircuitUnit* unit, bool isNear)
{
	// No new reclaim jobs while metal storage is nearly full; running jobs carry on
	if ((unit == nullptr) || IsMetalFull()) {
		return nullptr;
	}

	// Jobs with constructors on them are uncapped; jobs nobody took yet are capped at max(1, constructors / 2)
	CBuilderManager* builderMgr = circuit->GetBuilderManager();
	const std::set<IBuilderTask*>& reclaimTasks = builderMgr->GetTasks(IBuilderTask::BuildType::RECLAIM);
	const unsigned untakenCount = std::count_if(reclaimTasks.begin(), reclaimTasks.end(),
			[](const IBuilderTask* t) { return t->GetAssignees().empty(); });
	const bool canOpenJob = untakenCount < std::max(1u, builderMgr->GetWorkerCount() / 2);
	if (!canOpenJob && std::none_of(reclaimTasks.begin(), reclaimTasks.end(),
			[unit](const IBuilderTask* t) { return t->CanAssignTo(unit); }))
	{
		return nullptr;  // nothing to open or join: skip the feature scan
	}

	// Near search: each feature's allowed travel (seconds at the unit's speed) scales with its metal left
	const float speed = unit->GetCircuitDef()->GetSpeed();  // elmos per second
	const bool isStarved = (GetMetalPull() * 0.8f > GetAvgMetalIncome());
	const SReclaimTravel& travel = isStarved ? reclaimTravelStarved : reclaimTravel;
	COOAICallback* clb = circuit->GetCallback();
	std::vector<SReclaimFeature> features = GetReclaimFeatures(isNear
			? clb->GetFeatureIdsIn(position, speed * std::max(travel.maxSec, reclaimTravelFloor))
			: clb->GetFeatureIds());
	if (isNear) {  // too far for its metal
		features.erase(std::remove_if(features.begin(), features.end(), [&](const SReclaimFeature& f) {
			return position.SqDistance2D(f.pos) > SQUARE(speed * GetReclaimTravelSec(travel, f.metalLeft));
		}), features.end());
	}

	// Best-ranked safe feature whose job can still take this unit: join it, or open a new job there.
	// A full job doesn't stop the search, the next feature is tried.
	IBuilderTask* job = nullptr;
	const SReclaimFeature* best = FindReclaimFeature(position, features, [&](const SReclaimFeature& f) {
		auto it = std::find_if(reclaimTasks.begin(), reclaimTasks.end(),
				[&f](const IBuilderTask* t) { return utils::is_equal_pos(f.pos, t->GetTaskPos()); });
		job = (it != reclaimTasks.end()) ? *it : nullptr;
		return ((job != nullptr) ? job->CanAssignTo(unit) : canOpenJob)  // not full, or room for another untaken job
			&& IsReclaimSafe(unit, f.pos);
	});
	if (best == nullptr) {
		return nullptr;
	}
	return (job != nullptr) ? job
			: builderMgr->EnqueueReclaim(IBuilderTask::Priority::HIGH, best->pos, best->metal, FRAMES_PER_SEC * reclaimUntakenSec,
										 8.0f/*unit->GetCircuitDef()->GetBuildDistance()*/);
}

const CEconomyManager::SReclaimFeature* CEconomyManager::FindReclaimFeature(const AIFloat3& position,
		const std::vector<SReclaimFeature>& features, const std::function<bool (const SReclaimFeature&)>& isAccepted) const
{
	std::vector<std::pair<float, const SReclaimFeature*>> heap;  // rank, feature; min-heap on rank
	heap.reserve(features.size());
	for (const SReclaimFeature& feature : features) {
		heap.emplace_back(GetReclaimRank(position.SqDistance2D(feature.pos), feature.metalLeft), &feature);
	}
	auto greater = [](const auto& a, const auto& b) { return a.first > b.first; };
	std::make_heap(heap.begin(), heap.end(), greater);
	while (!heap.empty()) {
		std::pop_heap(heap.begin(), heap.end(), greater);
		const SReclaimFeature* feature = heap.back().second;
		heap.pop_back();
		if (isAccepted(*feature)) {
			return feature;
		}
	}
	return nullptr;
}

float CEconomyManager::GetReclaimTravelSec(const SReclaimTravel& travel, float metalLeft) const
{
	// Linear from (1 metal, minSec) to (reclaimTravelMetal, maxSec), capped at maxSec, never below the floor
	const float t = std::min(std::max(metalLeft - 1.f, 0.f) / std::max(reclaimTravelMetal - 1.f, 1.f), 1.f);
	return std::max(travel.minSec + (travel.maxSec - travel.minSec) * t, reclaimTravelFloor);
}

bool CEconomyManager::IsReclaimSafe(CCircuitUnit* unit, const AIFloat3& pos) const
{
	// Same rule as other builder jobs (CBuilderManager::MakeBuilderTask): unsafe only when the enemy threat there
	// beats the builder's own AND the enemy holds the area (influence < 0), so wrecks behind our army count.
	// The map lookups go first: they are cheaper than the reach test.
	CThreatMap* threatMap = circuit->GetThreatMap();
	threatMap->SetThreatType(unit);
	const bool isCovered = (threatMap->GetThreatAt(pos) <= threatMap->GetUnitThreat(unit))
		|| (circuit->GetInflMap()->GetInfluenceAt(pos) >= -INFL_EPS);
	return isCovered && circuit->GetTerrainManager()->CanReachAt(unit, pos, unit->GetCircuitDef()->GetBuildDistance());
}

float CEconomyManager::GetReclaimRank(float sqDist, float metalLeft) const
{
	return std::sqrt(sqDist) / std::pow(std::max(metalLeft, 1.f), reclaimValueExp);
}

IBuilderTask* CEconomyManager::UpdateGeoTasks(const AIFloat3& position, CCircuitUnit* unit)
{
	if (geoDefIds.empty()) {
		return nullptr;
	}
	CBuilderManager* builderMgr = circuit->GetBuilderManager();
	if (!builderMgr->CanEnqueueTask(32)) {
		return nullptr;
	}
	// One geo job at a time. The shared energy-job cap in UpdateEnergyTasks allows a 500-metal job only
	// while no energy job at all is queued, and solars and winds almost always are, so geo has its own count
	const std::set<IBuilderTask*>& energyTasks = builderMgr->GetTasks(IBuilderTask::BuildType::ENERGY);
	for (const IBuilderTask* t : energyTasks) {
		if (IsGeoDef(t->GetBuildDef())) {
			return nullptr;
		}
	}

	// Same income test as other energy, but always with the full time budget: the stall budget (a quarter)
	// needs an income only a late game has, which is why a stall never queued a geo
	const float income = std::min(GetAvgMetalIncome(), GetAvgEnergyIncome()) * energyFactor;
	const int frame = circuit->GetLastFrame();
	for (const auto& engy : energyDefs.GetInfos()) {
		if (!IsGeoDef(engy.cdef) || !engy.cdef->IsAvailable(frame) || (engy.cdef->GetCount() >= engy.data.limit) ||
			(engy.cdef->GetCostM() * 16.0f >= MAX_BUILD_SEC * SQUARE(income)))
		{
			continue;
		}
		const AIFloat3 ventPos = FindGeoSpot(engy.cdef, position, unit);  // also checks safety and reach
		if (utils::is_valid(ventPos)) {
			const IBuilderTask::Priority priority = IsEnergyStalling() ? IBuilderTask::Priority::HIGH : IBuilderTask::Priority::NORMAL;
			// exactly on the vent: no shake, build position fixed (also marks the vent as taken)
			IBuilderTask* task = builderMgr->EnqueueTask(priority, engy.cdef, ventPos, IBuilderTask::BuildType::ENERGY, 0.f, true);
			task->SetBuildPos(ventPos);
			return task;
		}
	}
	return nullptr;
}

IBuilderTask* CEconomyManager::UpdateEnergyTasks(const AIFloat3& position, CCircuitUnit* unit)
{
	CBuilderManager* builderMgr = circuit->GetBuilderManager();
	if (!builderMgr->CanEnqueueTask(32)) {
		return nullptr;
	}

	// check energy / metal ratio
	float metalIncome = GetAvgMetalIncome();
	const float energyIncome = GetAvgEnergyIncome();
	const bool isEnergyStalling = IsEnergyStalling();
	// TODO: e-stalling needs separate array of energy-defs sorted by cost

	// Select proper energy UnitDef to build
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	metalIncome = std::min(metalIncome, energyIncome) * energyFactor;
	const float buildPower = std::min(builderMgr->GetBuildPower(), metalIncome);
	const int taskSize = builderMgr->GetTasks(IBuilderTask::BuildType::ENERGY).size();
	const float maxBuildTime = MAX_BUILD_SEC * (isEnergyStalling ? 0.25f : 1.f);
	// Room for one more job of this def: about 8 jobs per buildPower/cost
	auto canQueue = [taskSize, buildPower](CCircuitDef* cdef) {
		return taskSize < (int)(buildPower / cdef->GetCostM() * 8 + 1);
	};
	// TODO: Select proper scale/quadratic function (x*x) and smoothing coefficient (8).
	//       МЕТОД НАИМЕНЬШИХ КВАДРАТОВ ! (income|buildPower, make/cost) - points
	//       solar       geothermal    fusion         singu           ...
	//       (10, 2/70), (15, 25/500), (20, 35/1000), (30, 225/4000), ...
	auto isQuickToBuild = [maxBuildTime, metalIncome](CCircuitDef* cdef) {
		return cdef->GetCostM() * 16.0f < maxBuildTime * SQUARE(metalIncome);
	};
	const IBuilderTask::Priority priority = isEnergyStalling ? IBuilderTask::Priority::HIGH : IBuilderTask::Priority::NORMAL;

	// Geothermal first: by the efficiency sort it ranks below fusion, and fusion at its limit stops the
	// loop below, so geo would hardly ever be built (UpdateGeoTasks has its own queue and time checks)
	IBuilderTask* geoTask = UpdateGeoTasks(position, unit);
	if (geoTask != nullptr) {
		return geoTask;
	}

	const int frame = circuit->GetLastFrame();
	CCircuitDef* bestDef = nullptr;
	CCircuitDef* hopeDef = nullptr;
	bool isLastHope = isEnergyStalling;
	for (const auto& engy : energyDefs.GetInfos()) {  // sorted by high-tech first
		// Geothermal is handled above; skipping it here means its limit or a missing vent never blocks other energy
		if (!engy.cdef->IsAvailable(frame) || IsGeoDef(engy.cdef) || !terrainMgr->CanBeBuiltAtSafe(engy.cdef, position)) {
			continue;
		}

		if (engy.cdef->GetCount() < engy.data.limit) {
			isLastHope = false;
			if (canQueue(engy.cdef)) {
				bestDef = engy.cdef;
				if (isQuickToBuild(engy.cdef)) {
					break;
				}
			} else if (isQuickToBuild(engy.cdef)) {
				bestDef = nullptr;
				break;
			}
		} else if (!isEnergyStalling) {
			bestDef = nullptr;
			break;
		} else if (hopeDef == nullptr) {
			hopeDef = engy.cdef;
			isLastHope = isLastHope && canQueue(hopeDef);
		}
	}
	if (isLastHope) {
		bestDef = hopeDef;
	}

	if (bestDef == nullptr) {
		return nullptr;
	}

	// Find place to build
	AIFloat3 buildPos = -RgtVector;
	CMetalManager* metalMgr = circuit->GetMetalManager();
	if (!bestDef->IsRoleSupport()) {
		const CMetalData::Metals& spots = metalMgr->GetSpots();
		CMetalData::PointPredicate predicate = [&spots, terrainMgr](int index) {
			return !terrainMgr->IsZoneAlly(spots[index].position);
		};
		int index = metalMgr->FindNearestSpot(position, predicate);
		if (index != -1) {
			const CMetalData::Metals& spots = metalMgr->GetSpots();
			buildPos = spots[index].position;
		}
	} else {
		const AIFloat3& startPos = circuit->GetSetupManager()->GetBasePos();
		int index = metalMgr->FindNearestCluster(startPos);
		if (index >= 0) {
			const CMetalData::Clusters& clusters = metalMgr->GetClusters();
			buildPos = clusters[index].position;

			// TODO: Calc enemy vector and move position into opposite direction
			AIFloat3 mapCenter(circuit->GetTerrainManager()->GetTerrainWidth() / 2, 0, circuit->GetTerrainManager()->GetTerrainHeight() / 2);
			buildPos += (buildPos - mapCenter).Normalize2D() * 300.0f * (bestDef->GetCostM() / energyDefs.GetFirstDef()->GetCostM());
			CCircuitDef* bdef = (unit == nullptr) ? bestDef : unit->GetCircuitDef();
			CTerrainManager::CorrectPosition(buildPos);
			buildPos = circuit->GetTerrainManager()->GetBuildPosition(bdef, buildPos);
		}
	}

	if (utils::is_valid(buildPos) && terrainMgr->CanBeBuiltAtSafe(bestDef, buildPos) &&
		((unit == nullptr) || terrainMgr->CanReachAtSafe(unit, buildPos, unit->GetCircuitDef()->GetBuildDistance())))
	{
		return builderMgr->EnqueueTask(priority, bestDef, buildPos, IBuilderTask::BuildType::ENERGY, SQUARE_SIZE * 16.0f, true);
	}

	return nullptr;
}

void CEconomyManager::UpdateGeoSpots()
{
	// Zero-K keeps Gaia features (map vents) visible through fog (featureLOS.featureVisibility = 1), so this
	// normally finds every vent at once; merging with known spots covers games where features need LOS.
	// Id-only calls: one engine call per feature (its def), position only for vents.
	COOAICallback* clb = circuit->GetCallback();
	for (int featureId : clb->GetFeatureIds()) {
		if (!IsGeoFeatureDef(clb->Feature_GetDefId(featureId))) {
			continue;
		}
		const AIFloat3 pos = clb->Feature_GetPosition(featureId);
		const bool isKnown = std::any_of(geoSpots.begin(), geoSpots.end(), [&pos](const AIFloat3& spot) {
			return spot.SqDistance2D(pos) < SQUARE(SQUARE_SIZE * 2);
		});
		if (!isKnown) {
			geoSpots.push_back(pos);
			geoBlockedFrame.push_back(-1);
		}
	}
}

bool CEconomyManager::IsGeoFeatureDef(int featureDefId)
{
	if (featureDefId < 0) {
		return false;  // not visible
	}
	if ((size_t)featureDefId >= featureDefIsGeo.size()) {
		featureDefIsGeo.resize(featureDefId + 1, -1);
	}
	int8_t& isGeo = featureDefIsGeo[featureDefId];
	if (isGeo < 0) {  // defs don't change during a game
		isGeo = circuit->GetCallback()->FeatureDef_IsGeoThermal(featureDefId) ? 1 : 0;
	}
	return isGeo == 1;
}

const std::vector<AIFloat3>& CEconomyManager::GetGeoSpots()
{
	const int frame = circuit->GetLastFrame();
	if (frame >= geoSpotsFrame) {
		UpdateGeoSpots();
		geoSpotsFrame = frame + FRAMES_PER_SEC * 60;
	}
	return geoSpots;
}

bool CEconomyManager::IsGeoSpotTaken(const AIFloat3& spot) const
{
	for (IBuilderTask* t : circuit->GetBuilderManager()->GetTasks(IBuilderTask::BuildType::ENERGY)) {
		if (IsGeoDef(t->GetBuildDef()) && (t->GetPosition().SqDistance2D(spot) < SQUARE(SQUARE_SIZE * 4))) {
			return true;
		}
	}
	return false;
}

AIFloat3 CEconomyManager::FindGeoSpot(CCircuitDef* cdef, const AIFloat3& position, CCircuitUnit* unit)
{
	const std::vector<AIFloat3>& spots = GetGeoSpots();
	if (spots.empty()) {
		return -RgtVector;
	}

	// Nearest first, so the checks below stop at the first vent that passes
	std::vector<std::pair<float, unsigned>> byDist;
	byDist.reserve(spots.size());
	for (unsigned i = 0; i < spots.size(); ++i) {
		byDist.emplace_back(position.SqDistance2D(spots[i]), i);
	}
	std::sort(byDist.begin(), byDist.end());

	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	CThreatMap* threatMap = circuit->GetThreatMap();
	const int frame = circuit->GetLastFrame();
	for (const auto& [sqDist, i] : byDist) {
		const AIFloat3& spot = spots[i];
		if ((geoBlockedFrame[i] > frame) ||
			terrainMgr->IsZoneAlly(spot) ||
			!terrainMgr->CanBeBuiltAtSafe(cdef, spot) ||
			(threatMap->GetAllThreatAt(spot) > geoThreat) ||
			((unit != nullptr) && !terrainMgr->CanReachAtSafe(unit, spot, unit->GetCircuitDef()->GetBuildDistance())) ||
			IsGeoSpotTaken(spot))
		{
			continue;
		}
		// Engine call last. A vent it rejects (almost always: a geo stands on it) is skipped for 10 s,
		// so taken vents don't cost an engine call on every energy pick
		if (!circuit->GetMap()->IsPossibleToBuildAt(cdef->GetDef(), spot, UNIT_COMMAND_BUILD_NO_FACING)) {
			geoBlockedFrame[i] = frame + FRAMES_PER_SEC * 10;
			continue;
		}
		return spot;
	}
	return -RgtVector;
}

void CEconomyManager::UpdateGeoMorph()
{
	SCOPED_TIME(circuit, __PRETTY_FUNCTION__);
	const int frame = circuit->GetLastFrame();
	if (geoMorphing != nullptr) {
		if (frame < geoMorphStart + FRAMES_PER_SEC * 300) {
			return;  // one at a time
		}
		geoMorphing = nullptr;  // morph got stuck or was stopped: let another one try
	}
	if (geoMorphees.empty() || (frame < geoMorphFrame) || (GetAvgMetalIncome() < geoMorphIncome) ||
		IsMetalEmpty() || IsEnergyStalling())
	{
		return;
	}

	CThreatMap* threatMap = circuit->GetThreatMap();
	for (auto it = geoMorphees.begin(); it != geoMorphees.end(); ++it) {
		CCircuitUnit* unit = *it;
		if (((unit->GetTask() != nullptr) && (unit->GetTask()->GetType() == IUnitTask::Type::PLAYER)) ||
			(unit->GetHealthPercent() < 0.9f) ||
			(threatMap->GetAllThreatAt(unit->GetPos(frame)) > geoThreat))
		{
			continue;
		}
		unit->Morph();
		geoMorphing = unit;
		geoMorphStart = frame;
		geoMorphees.erase(it);
		break;
	}
}

IBuilderTask* CEconomyManager::UpdateFactoryTasks(const AIFloat3& position, CCircuitUnit* unit)
{
	CBuilderManager* builderMgr = circuit->GetBuilderManager();
	if (!builderMgr->CanEnqueueTask(64)) {
		return nullptr;
	}

	/*
	 * check air pads
	 */
	CFactoryManager* factoryMgr = circuit->GetFactoryManager();
	CMilitaryManager* militaryMgr = circuit->GetMilitaryManager();
	CCircuitDef* airpadDef = factoryMgr->GetAirpadDef();
	const std::set<IBuilderTask*> &factoryTasks = builderMgr->GetTasks(IBuilderTask::BuildType::FACTORY);
	const unsigned airpadFactor = SQUARE((airpadDef->GetCount() + factoryTasks.size()) * 4);
	const int frame = circuit->GetLastFrame();
	if (airpadDef->IsAvailable(frame) &&
		(militaryMgr->GetRoleUnits(ROLE_TYPE(BOMBER)).size() > airpadFactor))
	{
		CCircuitDef* bdef;
		AIFloat3 buildPos;
		if (unit == nullptr) {
			bdef = airpadDef;
			buildPos = factoryMgr->GetClosestHaven(circuit->GetSetupManager()->GetBasePos());
		} else {
			bdef = unit->GetCircuitDef();
			buildPos = factoryMgr->GetClosestHaven(unit);
		}
		if (!utils::is_valid(buildPos)) {
			buildPos = circuit->GetSetupManager()->GetBasePos();
		}
		CTerrainManager* terrainMgr = circuit->GetTerrainManager();
		buildPos = terrainMgr->GetBuildPosition(bdef, buildPos);

		if (terrainMgr->CanBeBuiltAtSafe(airpadDef, buildPos) &&
			((unit == nullptr) || terrainMgr->CanReachAtSafe(unit, buildPos, unit->GetCircuitDef()->GetBuildDistance())))
		{
			return builderMgr->EnqueueFactory(IBuilderTask::Priority::NORMAL, airpadDef, buildPos);
		}
	}

	/*
	 * check buildpower
	 */
	const float metalIncome = std::min(GetAvgMetalIncome(), GetAvgEnergyIncome())/* * ecoFactor*/;
	CCircuitDef* assistDef = factoryMgr->GetAssistDef();
	const float factoryFactor = (metalIncome - assistDef->GetBuildSpeed()) * 1.2f;
	const int nanoSize = builderMgr->GetTasks(IBuilderTask::BuildType::NANO).size();
	const float factoryPower = factoryMgr->GetFactoryPower() + nanoSize * assistDef->GetBuildSpeed();
	const bool isSwitchTime = (lastFacFrame + switchTime <= frame);
	if ((factoryPower >= factoryFactor) && !isSwitchTime) {
		return nullptr;
	}

	/*
	 * check nanos
	 */
	if (!isSwitchTime) {
		CCircuitUnit* factory = factoryMgr->NeedUpgrade();
		if ((factory != nullptr) && assistDef->IsAvailable(frame)) {
			AIFloat3 buildPos = factory->GetPos(frame);
			switch (factory->GetUnit()->GetBuildingFacing()) {
				default:
				case UNIT_FACING_SOUTH:
					buildPos.z -= 128.0f;  // def->GetZSize() * SQUARE_SIZE * 2;
					break;
				case UNIT_FACING_EAST:
					buildPos.x -= 128.0f;  // def->GetXSize() * SQUARE_SIZE * 2;
					break;
				case UNIT_FACING_NORTH:
					buildPos.z += 128.0f;  // def->GetZSize() * SQUARE_SIZE * 2;
					break;
				case UNIT_FACING_WEST:
					buildPos.x += 128.0f;  // def->GetXSize() * SQUARE_SIZE * 2;
					break;
			}

			CTerrainManager* terrainMgr = circuit->GetTerrainManager();
			CCircuitDef* bdef = (unit == nullptr) ? factory->GetCircuitDef() : unit->GetCircuitDef();
			CTerrainManager::CorrectPosition(buildPos);
			buildPos = terrainMgr->GetBuildPosition(bdef, buildPos);

			if (terrainMgr->CanBeBuiltAtSafe(assistDef, buildPos) &&
				((unit == nullptr) || terrainMgr->CanReachAtSafe(unit, buildPos, unit->GetCircuitDef()->GetBuildDistance())))
			{
				return builderMgr->EnqueueTask(IBuilderTask::Priority::HIGH, assistDef, buildPos,
											   IBuilderTask::BuildType::NANO, SQUARE_SIZE * 8, true);
			}
		}
	}

	/*
	 * check factories
	 */
	if (!factoryTasks.empty()) {
		return nullptr;
	}

	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	const AIFloat3& enemyPos = circuit->GetEnemyManager()->GetEnemyPos();
	const bool isStart = (factoryMgr->GetFactoryCount() == 0);
	AIFloat3 buildPos;
	if (isStart) {
		buildPos = circuit->GetSetupManager()->GetBasePos();
	} else {
		AIFloat3 pos(circuit->GetSetupManager()->GetBasePos());
		CMetalManager* metalMgr = circuit->GetMetalManager();
		AIFloat3 center = (pos + enemyPos) * 0.5f;
		float minSqDist = std::numeric_limits<float>::max();
		const CMetalData::Clusters& clusters = metalMgr->GetClusters();
		for (unsigned i = 0; i < clusters.size(); ++i) {
			if (!metalMgr->IsClusterFinished(i)) {
				continue;
			}
			const float sqDist = center.SqDistance2D(clusters[i].position);
			if (minSqDist > sqDist) {
				minSqDist = sqDist;
				pos = clusters[i].position;
			}
		}

		CMetalData::PointPredicate predicate = [this, terrainMgr, &clusters](const int index) {
			return (clusterInfos[index].factory == nullptr) && !terrainMgr->IsZoneAlly(clusters[index].position);
		};
		int index = metalMgr->FindNearestCluster(pos, predicate);
		if (index < 0) {
			return nullptr;
		}
		buildPos = clusters[index].position;
	}

	CCircuitDef* facDef = factoryMgr->GetFactoryToBuild(buildPos, isStart);
	if (facDef == nullptr) {
		return nullptr;
	}
	if (facDef->IsRoleSupport()) {
		buildPos = circuit->GetSetupManager()->GetBasePos();
	}

	const float sqStartDist = enemyPos.SqDistance2D(circuit->GetSetupManager()->GetStartPos());
	const float sqBuildDist = enemyPos.SqDistance2D(buildPos);
	float size = (sqStartDist > sqBuildDist) ? -200.0f : 200.0f;  // std::max(facUDef->GetXSize(), facUDef->GetZSize()) * SQUARE_SIZE;
	buildPos.x += (buildPos.x > enemyPos.x) ? -size : size;
	buildPos.z += (buildPos.z > enemyPos.z) ? -size : size;

	// identify area to build by factory representatives
	CCircuitDef* bdef;
	CCircuitDef* landDef = factoryMgr->GetLandDef(facDef);
	if (landDef != nullptr) {
		if (landDef->GetMobileId() < 0) {
			bdef = landDef;
		} else {
			STerrainMapArea* area = terrainMgr->GetMobileTypeById(landDef->GetMobileId())->areaLargest;
			// FIXME: area->percentOfMap < 40.0 doesn't seem right as water identifier
			bdef = ((area == nullptr) || (area->percentOfMap < 40.0)) ? factoryMgr->GetWaterDef(facDef) : landDef;
		}
	} else {
		bdef = factoryMgr->GetWaterDef(facDef);
	}
	if (bdef == nullptr) {
		return nullptr;
	}

	CTerrainManager::CorrectPosition(buildPos);
	buildPos = terrainMgr->GetBuildPosition(bdef, buildPos);

	if (terrainMgr->CanBeBuiltAtSafe(facDef, buildPos) &&
		((unit == nullptr) || terrainMgr->CanReachAtSafe(unit, buildPos, unit->GetCircuitDef()->GetBuildDistance())))
	{
		lastFacFrame = frame;
		IBuilderTask::Priority priority = (builderMgr->GetWorkerCount() <= 2) ?
										  IBuilderTask::Priority::NOW :
										  IBuilderTask::Priority::NORMAL;
		return builderMgr->EnqueueFactory(priority, facDef, buildPos);
	}

	return nullptr;
}

IBuilderTask* CEconomyManager::UpdateFactoryTasks()
{
	SCOPED_TIME(circuit, __PRETTY_FUNCTION__);
	return UpdateFactoryTasks(circuit->GetSetupManager()->GetBasePos());
}

IBuilderTask* CEconomyManager::UpdateStorageTasks()
{
	SCOPED_TIME(circuit, __PRETTY_FUNCTION__);
	CBuilderManager* builderMgr = circuit->GetBuilderManager();
	if (!builderMgr->CanEnqueueTask()) {
		return nullptr;
	}

	if ((storeDef == nullptr) || !storeDef->IsAvailable(circuit->GetLastFrame())) {
		return UpdatePylonTasks();
	}

	// Constructors reclaiming and metal close to full: storage at NOW, even if storage is already 10x income.
	// Running reclaim isn't stopped by full storage (it still denies the metal to the enemy).
	// The job scan first: it's cheaper than the storage engine call
	const std::set<IBuilderTask*>& reclaimTasks = builderMgr->GetTasks(IBuilderTask::BuildType::RECLAIM);
	const bool isReclaimStore = (reclaimStoreAt > 0.f)
			&& std::any_of(reclaimTasks.begin(), reclaimTasks.end(),
						   [](const IBuilderTask* t) { return !t->GetAssignees().empty(); })
			&& (GetMetalCur() > GetStorage(metalRes) * reclaimStoreAt);

	const std::set<IBuilderTask*>& storeTasks = builderMgr->GetTasks(IBuilderTask::BuildType::STORE);
	if (!storeTasks.empty()) {
		if (isReclaimStore) {
			for (IBuilderTask* t : storeTasks) {
				t->SetPriority(IBuilderTask::Priority::NOW);  // upgrade the one already queued
			}
		}
		return UpdatePylonTasks();
	}

	const float metalIncome = std::min(GetAvgMetalIncome(), GetAvgEnergyIncome());
	if (!isReclaimStore && (GetStorage(metalRes) > 10 * metalIncome)) {
		return UpdatePylonTasks();
	}

	const AIFloat3& startPos = circuit->GetSetupManager()->GetBasePos();
	CMetalManager* metalMgr = circuit->GetMetalManager();
	int index = metalMgr->FindNearestSpot(startPos);
	AIFloat3 buildPos;
	if (index != -1) {
		const CMetalData::Metals& spots = metalMgr->GetSpots();
		buildPos = spots[index].position;
	} else {
		CTerrainManager* terrainMgr = circuit->GetTerrainManager();
		int terWidth = terrainMgr->GetTerrainWidth();
		int terHeight = terrainMgr->GetTerrainHeight();
		float x = terWidth / 4 + rand() % (int)(terWidth / 2);
		float z = terHeight / 4 + rand() % (int)(terHeight / 2);
		buildPos = AIFloat3(x, circuit->GetMap()->GetElevationAt(x, z), z);
	}
	return builderMgr->EnqueueTask(isReclaimStore ? IBuilderTask::Priority::NOW : IBuilderTask::Priority::HIGH,
								   storeDef, buildPos, IBuilderTask::BuildType::STORE);
}

IBuilderTask* CEconomyManager::UpdatePylonTasks()
{
	CBuilderManager* builderMgr = circuit->GetBuilderManager();
	if (!builderMgr->CanEnqueueTask()) {
		return nullptr;
	}

	const float energyIncome = GetAvgEnergyIncome();
	const float metalIncome = std::min(GetAvgMetalIncome(), energyIncome);
	if (metalIncome < pylonLinkInc) {
		return nullptr;
	}

	const float cost = pylonDef->GetCostM();
	unsigned count = builderMgr->GetBuildPower() / cost * 8 + 1;
	if (builderMgr->GetTasks(IBuilderTask::BuildType::PYLON).size() >= count) {
		return nullptr;
	}

	energyGrid->SetAuthority(circuit);
	energyGrid->Update();

	CCircuitDef* buildDef;
	AIFloat3 buildPos;
	IGridLink* link = energyGrid->GetLinkToBuild(buildDef, buildPos);
	if ((link == nullptr) || (buildDef == nullptr)) {
		return nullptr;
	}

	if (utils::is_valid(buildPos)) {
		return builderMgr->EnqueuePylon(IBuilderTask::Priority::NORMAL, buildDef, buildPos, link, buildDef->GetCostM());
	} else {
		link->SetValid(false);
		energyGrid->SetForceRebuild(true);
		// TODO: Optimize: when invalid link appears start watchdog gametask
		//       that will traverse invalidLinks vector and enable link on timeout.
		//       When invalidLinks is empty remove watchdog gametask.
		circuit->GetScheduler()->RunTaskAfter(std::make_shared<CGameTask>([link](CEnergyGrid* energyGrid) {
			link->SetValid(true);
			energyGrid->SetForceRebuild(true);
		}, energyGrid), FRAMES_PER_SEC * 120);
	}

	return nullptr;
}

void CEconomyManager::AddMorphee(CCircuitUnit* unit)
{
	if (!unit->IsUpgradable() || (unit->GetTask() == nullptr)) {
		return;
	}
	morphees.insert(unit);
	if (morph == nullptr) {
		morph = std::make_shared<CGameTask>(&CEconomyManager::UpdateMorph, this);
		circuit->GetScheduler()->RunTaskEvery(morph, FRAMES_PER_SEC * 10);
	}
}

void CEconomyManager::UpdateMorph()
{
	SCOPED_TIME(circuit, __PRETTY_FUNCTION__);
	if (morphees.empty()) {
		circuit->GetScheduler()->RemoveTask(morph);
		morph = nullptr;
		return;
	}

	const float energyIncome = GetAvgEnergyIncome();
	const float metalIncome = std::min(GetAvgMetalIncome(), energyIncome);
	if ((metalIncome < 10) || !IsExcessed() || !IsMetalFull() || (GetMetalPull() * 0.8f > metalIncome)) {
		return;
	}

	auto it = morphees.begin();
	while (it != morphees.end()) {
		CCircuitUnit* unit = *it;
		if ((unit->GetTask()->GetType() == IUnitTask::Type::PLAYER) ||
			((unit->GetTask()->GetType() == IUnitTask::Type::RETREAT)
				&& static_cast<CRetreatTask*>(unit->GetTask())->IsFlee()) ||  // a health retreat may still morph
			(unit->GetUnit()->GetHealth() < unit->GetUnit()->GetMaxHealth() * 0.8f) ||
			circuit->GetBuilderManager()->IsCommInDanger(unit))  // no metal into a commander that may die mid-morph
		{
			++it;
		} else {
			unit->Upgrade();  // Morph();
			it = morphees.erase(it);
			break;  // one unit at a time
		}
	}
}

void CEconomyManager::OpenStrategy(const CCircuitDef* facDef, const AIFloat3& pos)
{
	const std::vector<CCircuitDef::RoleT>* opener = circuit->GetSetupManager()->GetOpener(facDef);
	if (opener == nullptr) {
		return;
	}
	CFactoryManager* factoryMgr = circuit->GetFactoryManager();
	for (CCircuitDef::RoleT type : *opener) {
		CCircuitDef* buildDef = factoryMgr->GetRoleDef(facDef, type);
		if ((buildDef == nullptr) || !buildDef->IsAvailable(circuit->GetLastFrame())) {
			continue;
		}
		CRecruitTask::Priority priotiry;
		CRecruitTask::RecruitType recruit;
		if (type == ROLE_TYPE(BUILDER)) {
			priotiry = CRecruitTask::Priority::NORMAL;
			recruit  = CRecruitTask::RecruitType::BUILDPOWER;
		} else {
			priotiry = CRecruitTask::Priority::HIGH;
			recruit  = CRecruitTask::RecruitType::FIREPOWER;
		}
		factoryMgr->EnqueueTask(priotiry, buildDef, pos, recruit, 128.f);
	}
}

float CEconomyManager::GetStorage(Resource* res)
{
	return economy->GetStorage(res) - HIDDEN_STORAGE;
}

void CEconomyManager::UpdateEconomy()
{
	if (ecoFrame + TEAM_SLOWUPDATE_RATE >= circuit->GetLastFrame()) {
		return;
	}
	ecoFrame = circuit->GetLastFrame();

	const float curMetal = GetMetalCur();
	const float storMetal = GetStorage(metalRes);
	isMetalEmpty = curMetal < storMetal * 0.2f;
	isMetalFull = curMetal > storMetal * 0.8f;
	isEnergyStalling = std::min(GetAvgMetalIncome() - GetMetalPull(), .0f)/* * 0.98f*/ > std::min(GetAvgEnergyIncome() - GetEnergyPull(), .0f);
	const float curEnergy = GetEnergyCur();
	const float storEnergy = GetStorage(energyRes);
	isEnergyEmpty = curEnergy < storEnergy * 0.1f;

	if (ecoFrame <= efInfo.startFrame) {
		energyFactor = efInfo.startFactor;
	} else if (ecoFrame >= efInfo.endFrame) {
		energyFactor = efInfo.endFactor;
	} else {
		energyFactor = efInfo.fraction * (ecoFrame - efInfo.startFrame) + efInfo.startFactor;
	}

	if (ecoFrame <= bdInfo.startFrame) {
		buildDelay = bdInfo.startDelay;
	} else if (ecoFrame >= bdInfo.endFrame) {
		buildDelay = bdInfo.endDelay;
	} else {
		buildDelay = int(bdInfo.fraction * (ecoFrame - bdInfo.startFrame)) + bdInfo.startDelay;
	}

	if (mexCount <= mspInfos.front().mex) {
		pullMtoS = mspInfos.front().pull;
	} else if (mexCount >= mspInfos.back().mex) {
		pullMtoS = mspInfos.back().pull;
	} else {
		auto it = std::lower_bound(mspInfos.cbegin(), mspInfos.cend(), mexCount, SPullMtoS());
		const SPullMtoS& mspInfo = *--it;
		pullMtoS = mspInfo.fraction * (mexCount - mspInfo.mex) + mspInfo.pull;
	}
	pullMtoS *= circuit->GetMilitaryManager()->ClampMobileCostRatio();
}

} // namespace circuit

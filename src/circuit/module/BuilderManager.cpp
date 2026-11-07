/*
 * BuilderManager.cpp
 *
 *  Created on: Dec 1, 2014
 *      Author: rlcevg
 */

#include "module/BuilderManager.h"
#include "module/EconomyManager.h"
#include "module/MilitaryManager.h"
#include "module/FactoryManager.h"
#include "map/InfluenceMap.h"
#include "map/ThreatMap.h"
#include "spring/SpringMap.h"
#include "resource/MetalManager.h"
#include "setup/SetupManager.h"
#include "terrain/TerrainManager.h"
#include "terrain/path/PathFinder.h"
#include "terrain/path/QueryCostMap.h"
#include "task/NilTask.h"
#include "task/IdleTask.h"
#include "task/RetreatTask.h"
#include "task/builder/GenericTask.h"
#include "task/builder/WaitTask.h"
#include "task/builder/FactoryTask.h"
#include "task/builder/NanoTask.h"
#include "task/builder/StoreTask.h"
#include "task/builder/PylonTask.h"
#include "task/builder/EnergyTask.h"
#include "task/builder/DefenceTask.h"
#include "task/builder/BunkerTask.h"
#include "task/builder/BigGunTask.h"
#include "task/builder/RadarTask.h"
#include "task/builder/SonarTask.h"
#include "task/builder/MexTask.h"
#include "task/builder/TerraformTask.h"
#include "task/builder/RepairTask.h"
#include "task/builder/ReclaimTask.h"
#include "task/builder/PatrolTask.h"
#include "task/builder/GuardTask.h"
#include "task/builder/EscortTask.h"
#include "task/fighter/SquadTask.h"
#include "task/builder/CombatTask.h"
#include "task/builder/BuildChain.h"
#include "CircuitAI.h"
#include "util/Scheduler.h"
#include "util/Utils.h"
#include "json/json.h"

#include "Command.h"
#include "Log.h"

namespace circuit {

using namespace springai;

static constexpr int COMM_FLEE_INTERVAL = FRAMES_PER_SEC / 2;  // commander flee check rate
static constexpr int COMM_FLEE_MIN_FRAMES = FRAMES_PER_SEC * 3;  // a flee lasts at least this long
static constexpr float COMM_ESCAPE_MIN = DEFAULT_SLACK * 4;  // a flee goal nearer than this is no escape: stay and fight
static constexpr float COMM_END_COVER_MOD = 1.25f;  // ending a flee, starting a home fight or a job in a flee zone on cover needs this x the enemy threat covered
static constexpr int COMM_DANGER_MEMORY = FRAMES_PER_SEC * 5;  // jobs and morph starts keep a danger this long after it left radar and LOS

CBuilderManager::CBuilderManager(CCircuitAI* circuit)
		: IUnitModule(circuit)
		, buildTasksCount(0)
		, buildPower(.0f)
		, workerCost(.0f)
		, buildIterator(0)
		, prioRepairRange(0.f)
		, prioRepairShare(0.f)
		, isPrioRepairSafe(false)
		, isPrioRepairSafeStatic(false)
		, prioRepairLeash(0.f)
		, prioRepairCooldown(0)
		, repairFleeShare(0.f)
		, repairChaseFrames(0)
		, repairChaseClose(0.f)
		, escort({0.f, 0.f, 0.f, 0.f, 0.f, false, static_cast<int>(IBuilderTask::Priority::HIGH)})
{
	commDangerFrame = -1;
	commThrMod = 1.f;
	isCommHide = false;
	isCommHideAir = false;
	circuit->GetScheduler()->RunOnInit(std::make_shared<CGameTask>(&CBuilderManager::Init, this));

	/*
	 * worker handlers
	 */
	auto workerCreatedHandler = [this](CCircuitUnit* unit, CCircuitUnit* builder) {
		if (unit->GetTask() == nullptr) {
			unit->SetManager(this);
			nilTask->AssignTo(unit);
			this->circuit->AddActionUnit(unit);
		}
	};
	auto workerFinishedHandler = [this](CCircuitUnit* unit) {
		if (unit->GetTask() == nullptr) {
			unit->SetManager(this);
			this->circuit->AddActionUnit(unit);
		}
		idleTask->AssignTo(unit);

		++buildAreas[unit->GetArea()][unit->GetCircuitDef()];

		AddBuildPower(unit);
		workers.insert(unit);
		if (unit->GetCircuitDef()->IsRoleComm()) {
			comms.insert(unit);
		} else {
			workerCost += unit->GetCircuitDef()->GetCostM();
		}

		AddBuildList(unit);

		CMilitaryManager* militaryMgr = this->circuit->GetMilitaryManager();
		// Flying builders get no slot: ground guards can't follow them, and they dodge danger by flying
		// "guarded" attribute gives any builder a slot, whatever max_workers says
		CCircuitDef* cdef = unit->GetCircuitDef();
		const bool isEarly = (workers.size() < militaryMgr->GetGuardCfg().maxWorkers) && !cdef->IsAttacker();
		if ((isEarly || cdef->IsAttrGuarded()) && !cdef->IsAbleToFly()) {
			militaryMgr->AddGuardTask(unit);
		}
	};
	auto workerIdleHandler = [this](CCircuitUnit* unit) {
		// FIXME: Avoid instant task reassignment, its only valid on build order fail.
		//        Can cause idle builder, but Watchdog should catch it eventually.
		//        Unfortunately can't use EVENT_COMMAND_FINISHED because there is no unique commandId like unitId
		if (this->circuit->GetLastFrame() > unit->GetTaskFrame()/* + FRAMES_PER_SEC*/) {
			unit->GetTask()->OnUnitIdle(unit);
		}
	};
	auto workerDamagedHandler = [](CCircuitUnit* unit, CEnemyInfo* attacker) {
		unit->GetTask()->OnUnitDamaged(unit, attacker);
	};
	auto workerDestroyedHandler = [this](CCircuitUnit* unit, CEnemyInfo* attacker) {
		comms.erase(unit);
		commStates.erase(unit);
		IUnitTask* task = unit->GetTask();
		task->OnUnitDestroyed(unit, attacker);  // can change task
		unit->GetTask()->RemoveAssignee(unit);  // Remove unit from IdleTask

		if (task->GetType() == IUnitTask::Type::NIL) {
			return;
		}
		--buildAreas[unit->GetArea()][unit->GetCircuitDef()];

		DelBuildPower(unit);
		workers.erase(unit);
		if (!unit->GetCircuitDef()->IsRoleComm()) {
			workerCost = std::max(workerCost - unit->GetCircuitDef()->GetCostM(), .0f);
		}
		costQueries.erase(unit);

		RemoveBuildList(unit);

		this->circuit->GetMilitaryManager()->DelGuardTask(unit);
	};

	/*
	 * building handlers
	 */
	auto buildingDamagedHandler = [this](CCircuitUnit* unit, CEnemyInfo* attacker) {
		const bool isPrio = unit->GetCircuitDef()->IsAttrPrioRepair()
				&& (unit->GetHealthPercent() < 0.9f) && !unit->GetUnit()->IsBeingBuilt();
		EnqueueRepair(isPrio ? IBuilderTask::Priority::NOW : IBuilderTask::Priority::HIGH, unit);
	};
	auto buildingDestroyedHandler = [this](CCircuitUnit* unit, CEnemyInfo* attacker) {
		int frame = this->circuit->GetLastFrame();
		int facing = unit->GetUnit()->GetBuildingFacing();
		this->circuit->GetTerrainManager()->DelBlocker(unit->GetCircuitDef(), unit->GetPos(frame), facing);
	};

	/*
	 * heavy handlers
	 */
//	auto heavyCreatedHandler = [this](CCircuitUnit* unit, CCircuitUnit* builder) {
//		CEconomyManager* economyMgr = this->circuit->GetEconomyManager();
//		if (economyMgr->GetAvgMetalIncome() * economyMgr->GetEcoFactor() > 32.0f) {
//			TRY_UNIT(this->circuit, unit,
//				unit->CmdPriority(2);
//			)
////			EnqueueRepair(IBuilderTask::Priority::LOW, unit);
//		}
//	};

	const Json::Value& root = circuit->GetSetupManager()->GetConfig();
	const Json::Value& retreat = root["retreat"]["builder"];
	const float builderRet = retreat.get((unsigned)0, 0.8f).asFloat();
	const float retMod = retreat.get((unsigned)1, 1.0f).asFloat();
	const Json::Value& pull = root["retreat"]["repair_pull"];
	prioRepairRange = pull.get((unsigned)0, 0.f).asFloat();
	prioRepairShare = pull.get((unsigned)1, 0.34f).asFloat();
	isPrioRepairSafe = root["retreat"].get("repair_safe", false).asBool();
	isPrioRepairSafeStatic = root["retreat"].get("repair_safe_static", false).asBool();
	const Json::Value& leash = root["retreat"]["repair_leash"];
	prioRepairLeash = leash.get((unsigned)0, 0.f).asFloat();
	prioRepairCooldown = leash.get((unsigned)1, 20.f).asFloat() * FRAMES_PER_SEC;
	const Json::Value& flee = root["retreat"]["repair_flee"];
	repairFleeShare = flee.get((unsigned)0, 0.f).asFloat();
	const Json::Value& chase = root["retreat"]["repair_chase"];
	repairChaseFrames = chase.get((unsigned)0, 0.f).asFloat() * FRAMES_PER_SEC;
	repairChaseClose = chase.get((unsigned)1, 64.f).asFloat();
	commThrMod = root["quota"]["thr_mod"].get("comm", 1.f).asFloat();
	const Json::Value& escortCfg = root["escort"];
	escort.minPower = escortCfg.get("min_power", 0.f).asFloat();
	escort.share = escortCfg.get("share", 0.2f).asFloat();
	escort.behind = escortCfg.get("behind", 150.f).asFloat();
	escort.follow = escortCfg.get("follow", 600.f).asFloat();
	escort.range = escortCfg.get("range", 600.f).asFloat();
	escort.isComm = escortCfg.get("comm", false).asBool();
	{
		const std::string takeFrom = escortCfg.get("take_from", "high").asString();
		escort.maxPriority = (takeFrom == "idle") ? -1
				: (takeFrom == "low") ? static_cast<int>(IBuilderTask::Priority::LOW)
				: (takeFrom == "normal") ? static_cast<int>(IBuilderTask::Priority::NORMAL)
				: static_cast<int>(IBuilderTask::Priority::HIGH);
		if ((takeFrom != "idle") && (takeFrom != "low") && (takeFrom != "normal") && (takeFrom != "high")) {
			circuit->LOG("CONFIG escort.take_from: unknown value '%s', using 'high'", takeFrom.c_str());
		}
	}

	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	for (CCircuitDef& cdef : circuit->GetCircuitDefs()) {
		CCircuitDef::Id unitDefId = cdef.GetId();
		if (cdef.IsMobile()) {
			if (cdef.GetDef()->IsBuilder() && cdef.IsBuilder()) {
				createdHandler[unitDefId]   = workerCreatedHandler;
				finishedHandler[unitDefId]  = workerFinishedHandler;
				idleHandler[unitDefId]      = workerIdleHandler;
				damagedHandler[unitDefId]   = workerDamagedHandler;
				destroyedHandler[unitDefId] = workerDestroyedHandler;

				int mtId = terrainMgr->GetMobileTypeId(unitDefId);
				if (mtId >= 0) {  // not air
					workerMobileTypes.insert(mtId);
				}
				workerDefs.insert(&cdef);

				if (cdef.GetRetreat() < 0.f) {
					cdef.SetRetreat(builderRet);
				} else {
					cdef.SetRetreat(cdef.GetRetreat() * retMod);
				}
//			} else if (cdef->GetCostM() > 999.0f) {
//				createdHandler[unitDefId] = heavyCreatedHandler;
			}
		} else {
			damagedHandler[unitDefId]   = buildingDamagedHandler;
			destroyedHandler[unitDefId] = buildingDestroyedHandler;
		}
	}

	/*
	 * staticmex handlers;
	 */
	CCircuitDef::Id unitDefId = circuit->GetEconomyManager()->GetMexDef()->GetId();
	destroyedHandler[unitDefId] = [this](CCircuitUnit* unit, CEnemyInfo* attacker) {
		const AIFloat3& pos = unit->GetPos(this->circuit->GetLastFrame());
		CCircuitDef* mexDef = unit->GetCircuitDef();
		const int facing = unit->GetUnit()->GetBuildingFacing();
		this->circuit->GetTerrainManager()->DelBlocker(mexDef, pos, facing);
		int index = this->circuit->GetMetalManager()->FindNearestSpot(pos);
		if (index < 0) {
			return;
		}
		this->circuit->GetMetalManager()->SetOpenSpot(index, true);
		this->circuit->GetEconomyManager()->SetOpenSpot(index, true);
		if (unit->GetUnit()->IsBeingBuilt()) {
			return;
		}
		// Check mex position in 20 seconds
		this->circuit->GetScheduler()->RunTaskAfter(std::make_shared<CGameTask>([this, mexDef, pos, index]() {
			if (this->circuit->GetEconomyManager()->IsAllyOpenSpot(index) &&
				this->circuit->GetBuilderManager()->IsBuilderInArea(mexDef, pos) &&
				this->circuit->GetTerrainManager()->CanBeBuiltAtSafe(mexDef, pos))  // hostile environment
			{
				EnqueueTask(IBuilderTask::Priority::HIGH, mexDef, pos, IBuilderTask::BuildType::MEX)->SetBuildPos(pos);
				this->circuit->GetEconomyManager()->SetOpenSpot(index, false);
			}
		}), FRAMES_PER_SEC * 20);
	};

	ReadConfig();

	buildTasks.resize(static_cast<IBuilderTask::BT>(IBuilderTask::BuildType::_SIZE_));

	for (auto mtId : workerMobileTypes) {
		for (auto& area : terrainMgr->GetMobileTypeById(mtId)->area) {
			buildAreas[&area] = std::map<CCircuitDef*, int>();
		}
	}
	buildAreas[nullptr] = std::map<CCircuitDef*, int>();  // air
}

CBuilderManager::~CBuilderManager()
{
	for (IUnitTask* task : buildUpdates) {
		task->ClearRelease();
	}
	for (auto& kv1 : buildChains) {
		for (auto& kv2 : kv1.second) {
			delete kv2.second;
		}
	}
}

void CBuilderManager::ReadConfig()
{
	const Json::Value& root = circuit->GetSetupManager()->GetConfig();
	const std::string& cfgName = circuit->GetSetupManager()->GetConfigName();

	terraDef = circuit->GetCircuitDef(root["economy"].get("terra", "").asCString());
	if (terraDef == nullptr) {
		terraDef = circuit->GetEconomyManager()->GetDefaultDef();
	}

	// Builder job choice: travel cost / weight, lowest wins; travel caps in seconds per priority; switch share
	const Json::Value& jobs = root["economy"]["builder_jobs"];
	const char* prioNames[] = {"low", "normal", "high"};
	const float defWeight[] = {1.f, 2.f, 4.f};
	const float defTravel[] = {10.f, 20.f, 30.f};
	for (int i = 0; i < 3; ++i) {
		jobRanks[i].invWeight = 1.f / std::max(jobs["weight"].get(prioNames[i], defWeight[i]).asFloat(), 0.01f);
		const float travel = jobs["travel"].get(prioNames[i], defTravel[i]).asFloat();
		jobRanks[i].travelSec = (travel > 0.f) ? travel : std::numeric_limits<float>::max();  // 0 = no cap
	}
	jobRanks[3] = {1.f / 10000.f, std::numeric_limits<float>::max()};  // NOW: wins at any distance, as before
	jobSwitch = utils::clamp(jobs.get("switch", 0.5f).asFloat(), 0.f, 1.f);

	const Json::Value& cond = root["porcupine"]["superweapon"]["condition"];
	super.minIncome = cond.get((unsigned)0, 50.f).asFloat();
	super.maxTime = cond.get((unsigned)1, 300.f).asFloat();

	IBuilderTask::BuildName& buildNames = IBuilderTask::GetBuildNames();
	const Json::Value& build = root["build_chain"];
	for (const std::string& catName : build.getMemberNames()) {
		auto it = buildNames.find(catName);
		if (it == buildNames.end()) {
			circuit->LOG("CONFIG %s: has unknown category '%s'", cfgName.c_str(), catName.c_str());
			continue;
		}

		std::unordered_map<CCircuitDef*, SBuildChain*>& defMap = buildChains[static_cast<IBuilderTask::BT>(it->second)];
		const Json::Value& catChain = build[catName];
		for (const std::string& defName : catChain.getMemberNames()) {
			CCircuitDef* cdef = circuit->GetCircuitDef(defName.c_str());
			if (cdef == nullptr) {
				circuit->LOG("CONFIG %s: has unknown UnitDef '%s'", cfgName.c_str(), defName.c_str());
				continue;
			}

			SBuildChain bc;
			const Json::Value& buildQueue = catChain[defName];
			bc.isPylon = buildQueue.get("pylon", false).asBool();
			bc.isPorc = buildQueue.get("porc", false).asBool();
			bc.isTerra = buildQueue.get("terra", false).asBool();

			const Json::Value& engy = buildQueue["energy"];
			bc.energy = engy.get(unsigned(0), -1.f).asFloat();
			bc.isMexEngy = engy[1].isString();

			const Json::Value& hub = buildQueue["hub"];
			bc.hub.reserve(hub.size());
			for (const Json::Value& que : hub) {
				std::vector<SBuildInfo> queue;
				queue.reserve(que.size());
				for (const Json::Value& part : que) {
					SBuildInfo bi;

					const std::string& partName = part.get("unit", "").asString();
					bi.cdef = circuit->GetCircuitDef(partName.c_str());
					if (bi.cdef == nullptr) {
						circuit->LOG("CONFIG %s: has unknown UnitDef '%s'", cfgName.c_str(), partName.c_str());
						continue;
					}

					const std::string& cat = part.get("category", "").asString();
					auto it = buildNames.find(cat);
					if (it == buildNames.end()) {
						circuit->LOG("CONFIG %s: has unknown category '%s'", cfgName.c_str(), cat.c_str());
						continue;
					}
					bi.buildType = it->second;

					UnitDef* unitDef = cdef->GetDef();
					bi.offset = ZeroVector;
					bi.direction = SBuildInfo::Direction::NONE;
					const Json::Value& off = part["offset"];
					if (off.isArray()) {
						bi.offset = AIFloat3(off.get((unsigned)0, 0.f).asFloat(), 0.f, off.get((unsigned)1, 0.f).asFloat());
						if (bi.offset.x < -1e-3f) {
							bi.offset.x -= unitDef->GetXSize() * SQUARE_SIZE / 2;
						} else if (bi.offset.x > 1e-3f) {
							bi.offset.x += unitDef->GetXSize() * SQUARE_SIZE / 2;
						}
						if (bi.offset.z < -1e-3f) {
							bi.offset.z -= unitDef->GetZSize() * SQUARE_SIZE / 2;
						} else if (bi.offset.z > 1e-3f) {
							bi.offset.z += unitDef->GetZSize() * SQUARE_SIZE / 2;
						}
					} else if (off.isObject() && !off.empty()) {
						float delta = 0.f;
						SBuildInfo::DirName& dirNames = SBuildInfo::GetDirNames();
						std::string dir = off.getMemberNames().front();
						auto it = dirNames.find(dir);
						if (it != dirNames.end()) {
							bi.direction = it->second;
							delta = off[dir].asFloat();
						}
						switch (bi.direction) {
							case DIRECTION(LEFT): {
								bi.offset.x = delta + unitDef->GetXSize() * SQUARE_SIZE / 2;
							} break;
							case DIRECTION(RIGHT): {
								bi.offset.x = -(delta + unitDef->GetXSize() * SQUARE_SIZE / 2);
							} break;
							case DIRECTION(FRONT): {
								bi.offset.z = delta + unitDef->GetZSize() * SQUARE_SIZE / 2;
							} break;
							case DIRECTION(BACK): {
								bi.offset.z = -(delta + unitDef->GetZSize() * SQUARE_SIZE / 2);
							} break;
							default: break;
						}
					}

					bi.condition = SBuildInfo::Condition::ALWAYS;
					const std::string& cond = part.get("condition", "").asString();
					if (!cond.empty()) {
						SBuildInfo::CondName& condNames = SBuildInfo::GetCondNames();
						auto it = condNames.find(cond);
						if (it != condNames.end()) {
							bi.condition = it->second;
						}
					}

					queue.push_back(bi);
				}
				bc.hub.push_back(queue);
			}

			defMap[cdef] = new SBuildChain(bc);
		}
	}
}

void CBuilderManager::Init()
{
	CSetupManager::StartFunc subinit = [this](const AIFloat3& pos) {
		CScheduler* scheduler = circuit->GetScheduler().get();
		const int interval = 8;
		const int offset = circuit->GetSkirmishAIId() % interval;
		scheduler->RunTaskEvery(std::make_shared<CGameTask>(&CBuilderManager::UpdateIdle, this), interval, offset + 0);
		scheduler->RunTaskEvery(std::make_shared<CGameTask>(&CBuilderManager::UpdateBuild, this), 1/*interval*/, offset + 1);

		scheduler->RunTaskEvery(std::make_shared<CGameTask>(&CBuilderManager::Watchdog, this),
								FRAMES_PER_SEC * 60,
								circuit->GetSkirmishAIId() * WATCHDOG_COUNT + 10);
		if (escort.minPower > 0.f) {
			scheduler->RunTaskEvery(std::make_shared<CGameTask>(&CBuilderManager::UpdateEscort, this),
									FRAMES_PER_SEC * 5, offset + 3);
		}
		scheduler->RunTaskEvery(std::make_shared<CGameTask>(&CBuilderManager::UpdateCommFlee, this),
								COMM_FLEE_INTERVAL, offset + 5);
	};

	circuit->GetSetupManager()->ExecOnFindStart(subinit);
}

void CBuilderManager::Release()
{
	// NOTE: Release expected to be called on CCircuit::Release.
	//       It doesn't stop scheduled GameTasks for that reason.
	for (IUnitTask* task : buildUpdates) {
		AbortTask(task);
		// NOTE: Do not delete task as other AbortTask may ask for it
	}
	for (IUnitTask* task : buildUpdates) {
		task->ClearRelease();
	}
	buildUpdates.clear();
}

int CBuilderManager::UnitCreated(CCircuitUnit* unit, CCircuitUnit* builder)
{
	auto search = createdHandler.find(unit->GetCircuitDef()->GetId());
	if (search != createdHandler.end()) {
		search->second(unit, builder);
	}

	if (builder == nullptr) {
		CTerrainManager* terrainMgr = circuit->GetTerrainManager();
		if (!unit->GetCircuitDef()->IsMobile() && !terrainMgr->ResignAllyBuilding(unit)) {
			// enemy unit captured
			const AIFloat3& pos = unit->GetPos(circuit->GetLastFrame());
			terrainMgr->AddBlocker(unit->GetCircuitDef(), pos, unit->GetUnit()->GetBuildingFacing());
		}
		return 0; //signaling: OK
	}

	IUnitTask* task = builder->GetTask();
	if ((task == nullptr) || task->GetType() != IUnitTask::Type::BUILDER) {
		return 0; //signaling: OK
	}

	IBuilderTask* taskB = static_cast<IBuilderTask*>(task);
	if (unit->GetUnit()->IsBeingBuilt()) {
		// NOTE: Try to cope with wrong event order, when different units created within same task.
		//       Real example: unit starts building, but hlt kills structure right away. UnitDestroyed invoked and new task assigned to unit.
		//       But for some engine-bugged reason unit is not idle and retries same building. UnitCreated invoked for new task with wrong target.
		//       Next workaround unfortunately doesn't mark bugged building on blocking map.
		if ((taskB->GetTarget() == nullptr) && (taskB->GetBuildDef() != nullptr) &&
			(*taskB->GetBuildDef() == *unit->GetCircuitDef()) && taskB->IsEqualBuildPos(unit))
		{
			// FIXME: DEBUG
//			AIFloat3 bp = taskB->GetBuildPos();
//			circuit->GetDrawer()->AddPoint(bp, "bp");
//			circuit->LOG("bp = %f, %f", bp.x, bp.z);
//			AIFloat3 up = unit->GetPos(circuit->GetLastFrame());
//			circuit->GetDrawer()->AddPoint(up, "up");
//			circuit->LOG("up = %f, %f", up.x, up.z);
			// FIXME: DEBUG
			taskB->UpdateTarget(unit);
			unfinishedUnits[unit] = taskB;
		} else {
			// reclaim lost unit
			AssignTask(builder, EnqueueReclaim(IBuilderTask::Priority::HIGH, unit));
		}
	} else {
		DoneTask(taskB);
	}

	return 0; //signaling: OK
}

int CBuilderManager::UnitFinished(CCircuitUnit* unit)
{
	auto iter = unfinishedUnits.find(unit);
	if (iter != unfinishedUnits.end()) {
		DoneTask(iter->second);
	}
	auto itre = repairedUnits.find(unit->GetId());
	if (itre != repairedUnits.end()) {
		DoneTask(itre->second);
	}
	auto itcl = reclaimedUnits.find(unit);
	if (itcl != reclaimedUnits.end()) {
		AbortTask(itcl->second);
	}

	auto search = finishedHandler.find(unit->GetCircuitDef()->GetId());
	if (search != finishedHandler.end()) {
		search->second(unit);
	}

	return 0; //signaling: OK
}

int CBuilderManager::UnitIdle(CCircuitUnit* unit)
{
	auto search = idleHandler.find(unit->GetCircuitDef()->GetId());
	if (search != idleHandler.end()) {
		search->second(unit);
	}

	return 0; //signaling: OK
}

int CBuilderManager::UnitDamaged(CCircuitUnit* unit, CEnemyInfo* attacker)
{
	auto search = damagedHandler.find(unit->GetCircuitDef()->GetId());
	if (search != damagedHandler.end()) {
		search->second(unit, attacker);
	}

	return 0; //signaling: OK
}

int CBuilderManager::UnitDestroyed(CCircuitUnit* unit, CEnemyInfo* attacker)
{
	auto iter = unfinishedUnits.find(unit);
	if (iter != unfinishedUnits.end()) {
		AbortTask(iter->second);
	}
	auto itre = repairedUnits.find(unit->GetId());
	if (itre != repairedUnits.end()) {
		AbortTask(itre->second);
	}
	auto itcl = reclaimedUnits.find(unit);
	if (itcl != reclaimedUnits.end()) {
		DoneTask(itcl->second);
	}

	auto search = destroyedHandler.find(unit->GetCircuitDef()->GetId());
	if (search != destroyedHandler.end()) {
		search->second(unit, attacker);
	}

	return 0; //signaling: OK
}

void CBuilderManager::AddBuildPower(CCircuitUnit* unit)
{
	buildPower += unit->GetBuildSpeed();
	circuit->GetMilitaryManager()->AddResponse(unit);
}

void CBuilderManager::DelBuildPower(CCircuitUnit* unit)
{
	buildPower -= unit->GetBuildSpeed();
	circuit->GetMilitaryManager()->DelResponse(unit);
}

const std::set<IBuilderTask*>& CBuilderManager::GetTasks(IBuilderTask::BuildType type) const
{
	assert(type < IBuilderTask::BuildType::_SIZE_);
	return buildTasks[static_cast<IBuilderTask::BT>(type)];
}

void CBuilderManager::ActivateTask(IBuilderTask* task)
{
	if ((task->GetType() == IUnitTask::Type::BUILDER) && (task->GetBuildType() < IBuilderTask::BuildType::_SIZE_)) {
		buildTasks[static_cast<IBuilderTask::BT>(task->GetBuildType())].insert(task);
		buildTasksCount++;
	}
	buildUpdates.push_back(task);
	task->Activate();
}

IBuilderTask* CBuilderManager::EnqueueTask(IBuilderTask::Priority priority,
										   CCircuitDef* buildDef,
										   const AIFloat3& position,
										   IBuilderTask::BuildType type,
										   float cost,
										   float shake,
										   bool isActive,
										   int timeout)
{
	return AddTask(priority, buildDef, position, type, cost, shake, isActive, timeout);
}

IBuilderTask* CBuilderManager::EnqueueTask(IBuilderTask::Priority priority,
										   CCircuitDef* buildDef,
										   const AIFloat3& position,
										   IBuilderTask::BuildType type,
										   float shake,
										   bool isActive,
										   int timeout)
{
	float cost = buildDef->GetCostM();
	return AddTask(priority, buildDef, position, type, cost, shake, isActive, timeout);
}

IBuilderTask* CBuilderManager::EnqueueFactory(IBuilderTask::Priority priority,
											  CCircuitDef* buildDef,
											  const AIFloat3& position,
											  float shake,
											  bool isPlop,
											  bool isActive,
											  int timeout)
{
	const float cost = isPlop ? 1.f : buildDef->GetCostM();
	IBuilderTask* task = new CBFactoryTask(this, priority, buildDef, position, cost, shake, isPlop, timeout);
	if (isActive) {
		buildTasks[static_cast<IBuilderTask::BT>(IBuilderTask::BuildType::FACTORY)].insert(task);
		buildTasksCount++;
		buildUpdates.push_back(task);
	} else {
		task->Deactivate();
	}
	return task;
}

IBuilderTask* CBuilderManager::EnqueuePylon(IBuilderTask::Priority priority,
											CCircuitDef* buildDef,
											const AIFloat3& position,
											IGridLink* link,
											float cost,
											bool isActive,
											int timeout)
{
	IBuilderTask* task = new CBPylonTask(this, priority, buildDef, position, link, cost, timeout);
	if (isActive) {
		buildTasks[static_cast<IBuilderTask::BT>(IBuilderTask::BuildType::PYLON)].insert(task);
		buildTasksCount++;
		buildUpdates.push_back(task);
	} else {
		task->Deactivate();
	}
	return task;
}

IBuilderTask* CBuilderManager::EnqueueRepair(IBuilderTask::Priority priority,
											 CCircuitUnit* target,
											 int timeout)
{
	if (priority == IBuilderTask::Priority::NOW) {
		auto itl = leashUntil.find(target->GetId());
		if (itl != leashUntil.end()) {
			if (circuit->GetLastFrame() < itl->second) {
				// Unit drove off mid-repair recently: don't send another builder chasing it
				auto it = repairedUnits.find(target->GetId());
				return (it != repairedUnits.end()) ? it->second : nullptr;
			}
			leashUntil.erase(itl);
		}
	}
	CBRepairTask* task;
	auto it = repairedUnits.find(target->GetId());
	if (it != repairedUnits.end()) {
		task = it->second;
		if (static_cast<int>(priority) > static_cast<int>(task->GetPriority())) {
			task->SetPriority(priority);  // upgrade, e.g. HIGH left over from retreat
		}
	} else {
		task = new CBRepairTask(this, priority, target, timeout);
		buildTasks[static_cast<IBuilderTask::BT>(IBuilderTask::BuildType::REPAIR)].insert(task);
		buildTasksCount++;
		buildUpdates.push_back(task);
		repairedUnits[target->GetId()] = task;
	}
	if ((task->GetPriority() == IBuilderTask::Priority::NOW) && task->GetAssignees().empty()) {
		PullRepairer(task, target);
	}
	return task;
}

bool CBuilderManager::IsPrioRepairFull(const CCircuitUnit* unit) const
{
	// Builders on priority repair, not counting the asking unit (it would only switch jobs)
	unsigned prioCount = 0;
	for (CCircuitUnit* worker : workers) {
		IUnitTask* curTask = worker->GetTask();
		if ((worker != unit)
			&& (curTask->GetType() == IUnitTask::Type::BUILDER)
			&& (static_cast<IBuilderTask*>(curTask)->GetBuildType() == IBuilderTask::BuildType::REPAIR)
			&& (curTask->GetPriority() == IBuilderTask::Priority::NOW))
		{
			++prioCount;
		}
	}
	return prioCount + 1 > std::max(1.f, workers.size() * prioRepairShare);
}

void CBuilderManager::PullRepairer(IBuilderTask* task, CCircuitUnit* target)
{
	if (prioRepairRange <= 0.f) {
		return;
	}
	// Do not strip the whole economy in a big fight
	if (IsPrioRepairFull(nullptr)) {
		return;
	}

	const int frame = circuit->GetLastFrame();
	const AIFloat3& pos = target->GetPos(frame);
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	CCircuitUnit* bestWorker = nullptr;
	float minSqDist = SQUARE(prioRepairRange);
	for (CCircuitUnit* worker : workers) {
		IUnitTask* curTask = worker->GetTask();
		if ((worker == target)
			|| ((curTask->GetType() != IUnitTask::Type::BUILDER) && (curTask->GetType() != IUnitTask::Type::IDLE))
			|| !task->CanAssignTo(worker))
		{
			continue;
		}
		if ((curTask->GetType() == IUnitTask::Type::BUILDER)
			&& ((curTask->GetPriority() == IBuilderTask::Priority::NOW)  // already on urgent work
				|| (static_cast<IBuilderTask*>(curTask)->GetBuildType() == IBuilderTask::BuildType::REPAIR)))  // keep repairs stable
		{
			continue;
		}
		const float sqDist = pos.SqDistance2D(worker->GetPos(frame));
		if ((sqDist < minSqDist) && terrainMgr->CanReachAt(worker, pos, worker->GetCircuitDef()->GetBuildDistance())
			&& !IsUnsafePrioRepair(task, worker, pos)
			&& !IsRepairTargetFleeing(task, worker, worker->GetPos(frame)))
		{
			minSqDist = sqDist;
			bestWorker = worker;
		}
	}
	if (bestWorker != nullptr) {
		AssignTask(bestWorker, task);
	}
}

void CBuilderManager::OnPrioRepairLeash(ICoreUnit::Id unitId)
{
	leashUntil[unitId] = circuit->GetLastFrame() + prioRepairCooldown;
}

bool CBuilderManager::IsRepairTargetFleeing(const IBuilderTask* task, CCircuitUnit* builder, const AIFloat3& pos,
										   const AIFloat3* targetVel) const
{
	if ((repairFleeShare <= 0.f) || (task->GetBuildType() != IBuilderTask::BuildType::REPAIR)) {
		return false;
	}
	CCircuitUnit* target = task->GetTarget();
	if ((target == nullptr) || !target->GetCircuitDef()->IsMobile()
		|| (target->GetTask()->GetType() == IUnitTask::Type::RETREAT))  // heading home to be repaired
	{
		return false;
	}
	const AIFloat3& targetPos = target->GetPos(circuit->GetLastFrame());
	const float dx = targetPos.x - pos.x;
	const float dz = targetPos.z - pos.z;
	const float sqDist = dx * dx + dz * dz;
	const float range = builder->GetCircuitDef()->GetBuildDistance() + SQUARE_SIZE * 8;
	if (sqDist <= SQUARE(range)) {
		return false;  // in reach: repair works even while it moves
	}
	// Speed away from the builder vs target's own max speed
	const AIFloat3 vel = (targetVel != nullptr) ? *targetVel : target->GetUnit()->GetVel();  // elmos per frame
	const float awayDot = vel.x * dx + vel.z * dz;  // awaySpeed * dist
	if (awayDot <= 0.f) {
		return false;
	}
	const float minAway = repairFleeShare * target->GetCircuitDef()->GetSpeed() / FRAMES_PER_SEC;
	return SQUARE(awayDot) > SQUARE(minAway) * sqDist;
}

bool CBuilderManager::IsChaseStalled(SChaseCheck& chase, ICoreUnit::Id targetId, float gap, int frame) const
{
	if ((repairChaseFrames <= 0) || (gap <= 0.f)) {
		chase.Reset();
		return false;
	}
	if ((chase.targetId != targetId) || (chase.frame < 0) || (chase.gap - gap >= repairChaseClose)) {
		// new chase, or closing in: checkpoint
		chase.targetId = targetId;
		chase.gap = gap;
		chase.frame = frame;
		return false;
	}
	return frame - chase.frame >= repairChaseFrames;
}

static constexpr int ENGINE_CMD_REPAIR = 40;  // engine's CMD_REPAIR

bool CBuilderManager::IsEngineChaseStalled(CCircuitUnit* unit, SChaseCheck& chase) const
{
	if (repairChaseFrames <= 0) {
		return false;
	}
	// Repair the engine picks while patrolling: CMD_REPAIR <unitId> <x> <y> <z> <radius> at the queue front
	ICoreUnit::Id targetId = -1;
	auto commands = unit->GetUnit()->GetCurrentCommands();
	if (!commands.empty() && (commands.front()->GetId() == ENGINE_CMD_REPAIR)) {
		const std::vector<float> params = commands.front()->GetParams();
		if (params.size() == 5) {
			targetId = static_cast<ICoreUnit::Id>(params[0]);
		}
	}
	utils::free_clear(commands);
	CAllyUnit* target = (targetId != -1) ? circuit->GetFriendlyUnit(targetId) : nullptr;
	if ((target == nullptr) || !target->GetCircuitDef()->IsMobile()) {
		chase.Reset();
		return false;
	}
	const int frame = circuit->GetLastFrame();
	const float range = unit->GetCircuitDef()->GetBuildDistance() + SQUARE_SIZE * 8;
	const float gap = target->GetPos(frame).distance2D(unit->GetPos(frame)) - range;
	return IsChaseStalled(chase, targetId, gap, frame);
}

void CBuilderManager::OnRetreatEnd(CCircuitUnit* unit)
{
	auto it = repairedUnits.find(unit->GetId());
	if ((it == repairedUnits.end()) || (it->second->GetPriority() == IBuilderTask::Priority::NOW)) {
		return;  // priority repair stays, under the leash and flee checks
	}
	// Back on duty: builders would follow it to the front
	AbortTask(it->second);
}

bool CBuilderManager::IsRepairAssigned(ICoreUnit::Id unitId) const
{
	auto it = repairedUnits.find(unitId);
	return (it != repairedUnits.end()) && !it->second->GetAssignees().empty();
}

bool CBuilderManager::IsUnsafePrioRepair(const IBuilderTask* task, CCircuitUnit* unit, const AIFloat3& pos) const
{
	if (task->GetBuildType() != IBuilderTask::BuildType::REPAIR) {
		return false;
	}
	CCircuitUnit* target = task->GetTarget();
	const bool isStatic = (target != nullptr) && !target->GetCircuitDef()->IsMobile();
	if (!(isStatic ? isPrioRepairSafeStatic : isPrioRepairSafe)) {
		return false;
	}
	// Same rule as regular builder tasks: threat beyond the builder's strength in enemy-held area
	CThreatMap* threatMap = circuit->GetThreatMap();
	threatMap->SetThreatType(unit);
	return (threatMap->GetThreatAt(pos) > threatMap->GetUnitThreat(unit))
		&& (circuit->GetInflMap()->GetInfluenceAt(pos) < -INFL_EPS);
}

IBuilderTask* CBuilderManager::EnqueueReclaim(IBuilderTask::Priority priority,
											  const AIFloat3& position,
											  float cost,
											  int timeout,
											  float radius,
											  bool isMetal)
{
	IBuilderTask* task = new CBReclaimTask(this, priority, position, cost, timeout, radius, isMetal);
	buildTasks[static_cast<IBuilderTask::BT>(IBuilderTask::BuildType::RECLAIM)].insert(task);
	buildTasksCount++;
	buildUpdates.push_back(task);
	return task;
}

IBuilderTask* CBuilderManager::EnqueueReclaim(IBuilderTask::Priority priority,
											  CCircuitUnit* target,
											  int timeout)
{
	auto it = reclaimedUnits.find(target);
	if (it != reclaimedUnits.end()) {
		return it->second;
	}
	CBReclaimTask* task = new CBReclaimTask(this, priority, target, timeout);
	buildTasks[static_cast<IBuilderTask::BT>(IBuilderTask::BuildType::RECLAIM)].insert(task);
	buildTasksCount++;
	buildUpdates.push_back(task);
	reclaimedUnits[target] = task;
	return task;
}

IBuilderTask* CBuilderManager::EnqueuePatrol(IBuilderTask::Priority priority,
											 const AIFloat3& position,
											 float cost,
											 int timeout)
{
	IBuilderTask* task = new CBPatrolTask(this, priority, position, cost, timeout);
	buildUpdates.push_back(task);
	return task;
}

IBuilderTask* CBuilderManager::EnqueueTerraform(IBuilderTask::Priority priority,
												CCircuitUnit* target,
												const AIFloat3& position,
												float cost,
												bool isActive,
												int timeout)
{
	IBuilderTask* task;
	if (target == nullptr) {
		task = new CBTerraformTask(this, priority, position, cost, timeout);
	} else {
		task = new CBTerraformTask(this, priority, target, cost, timeout);
	}
	if (isActive) {
		buildTasks[static_cast<IBuilderTask::BT>(IBuilderTask::BuildType::TERRAFORM)].insert(task);
		buildTasksCount++;
		buildUpdates.push_back(task);
	} else {
		task->Deactivate();
	}
	return task;
}

IBuilderTask* CBuilderManager::EnqueueGuard(IBuilderTask::Priority priority,
											CCircuitUnit* target,
											int timeout)
{
	IBuilderTask* task = new CBGuardTask(this, priority, target, timeout);
	buildUpdates.push_back(task);
	return task;
}

IUnitTask* CBuilderManager::EnqueueWait(int timeout)
{
	CBWaitTask* task = new CBWaitTask(this, timeout);
	buildUpdates.push_back(task);
	return task;
}

CRetreatTask* CBuilderManager::EnqueueRetreat()
{
	CRetreatTask* task = new CRetreatTask(this);
	buildUpdates.push_back(task);
	return task;
}

CCombatTask* CBuilderManager::EnqueueCombat(float powerMod)
{
	CCombatTask* task = new CCombatTask(this, powerMod);
	buildUpdates.push_back(task);
	return task;
}

IBuilderTask* CBuilderManager::AddTask(IBuilderTask::Priority priority,
									   CCircuitDef* buildDef,
									   const AIFloat3& position,
									   IBuilderTask::BuildType type,
									   float cost,
									   float shake,
									   bool isActive,
									   int timeout)
{
	IBuilderTask* task;
	switch (type) {
		case IBuilderTask::BuildType::NANO: {
			task = new CBNanoTask(this, priority, buildDef, position, cost, shake, timeout);
			break;
		}
		case IBuilderTask::BuildType::STORE: {
			task = new CBStoreTask(this, priority, buildDef, position, cost, shake, timeout);
			break;
		}
		case IBuilderTask::BuildType::ENERGY: {
			task = new CBEnergyTask(this, priority, buildDef, position, cost, shake, timeout);
			break;
		}
		case IBuilderTask::BuildType::DEFENCE: {
			task = new CBDefenceTask(this, priority, buildDef, position, cost, shake, timeout);
			break;
		}
		case IBuilderTask::BuildType::BUNKER: {
			task = new CBBunkerTask(this, priority, buildDef, position, cost, shake, timeout);
			break;
		}
		case IBuilderTask::BuildType::BIG_GUN: {
			task = new CBBigGunTask(this, priority, buildDef, position, cost, shake, timeout);
			break;
		}
		case IBuilderTask::BuildType::RADAR: {
			task = new CBRadarTask(this, priority, buildDef, position, cost, shake, timeout);
			break;
		}
		case IBuilderTask::BuildType::SONAR: {
			task = new CBSonarTask(this, priority, buildDef, position, cost, shake, timeout);
			break;
		}
		case IBuilderTask::BuildType::MEX: {
			task = new CBMexTask(this, priority, buildDef, position, cost, timeout);
			break;
		}
		// NOTE: Tasks created by config "hub"
//		case IBuilderTask::BuildType::FACTORY: {
//			task = new CBFactoryTask(this, priority, buildDef, nullptr, position, cost, shake, false, timeout);
//		} break;
		default: {
			// FIXME: CBGenericTask is a workaround and should be fixed to remain as crash handler.
			//        Currently used by build_chain->hub config.
			//        There's no way for AS to distinguish generic from real BuildType task. Save/Load gets confused.
			task = new CBGenericTask(this, type, priority, buildDef, position, cost, shake, timeout);
		} break;
	}

	if (isActive) {
		buildTasks[static_cast<IBuilderTask::BT>(type)].insert(task);
		buildTasksCount++;
		buildUpdates.push_back(task);
	} else {
		task->Deactivate();
	}
	return task;
}

void CBuilderManager::DequeueTask(IUnitTask* task, bool done)
{
	escortTasks.erase(task);
	switch (task->GetType()) {
		case IUnitTask::Type::BUILDER: {
			IBuilderTask* taskB = static_cast<IBuilderTask*>(task);
			if (taskB->GetBuildType() >= IBuilderTask::BuildType::_SIZE_) {
				break;
			}
			std::set<IBuilderTask*>& tasks = buildTasks[static_cast<IBuilderTask::BT>(taskB->GetBuildType())];
			auto it = tasks.find(taskB);
			if (it == tasks.end()) {
				break;
			}
			switch (taskB->GetBuildType()) {
				case IBuilderTask::BuildType::REPAIR: {
					repairedUnits.erase(static_cast<CBRepairTask*>(taskB)->GetTargetId());
				} break;
				case IBuilderTask::BuildType::RECLAIM: {
					reclaimedUnits.erase(taskB->GetTarget());
				} break;
				default: {
					unfinishedUnits.erase(taskB->GetTarget());
				} break;
			}
			tasks.erase(it);
			buildTasksCount--;
		} break;
		default: break;
	}
	task->Dead();
	task->Stop(done);
}

bool CBuilderManager::IsBuilderInArea(CCircuitDef* buildDef, const AIFloat3& position) const
{
	if (!utils::is_valid(position)) {  // any-area task
		return true;
	}
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	for (auto& kv : buildAreas) {
		for (auto& kvw : kv.second) {
			if ((kvw.second > 0) && kvw.first->CanBuild(buildDef) &&
				terrainMgr->CanMobileReachAt(kv.first, position, kvw.first->GetBuildDistance()))
			{
				return true;
			}
		}
	}
	return false;
}

IUnitTask* CBuilderManager::MakeTask(CCircuitUnit* unit)
{
	return DefaultMakeTask(unit);
}

void CBuilderManager::AbortTask(IUnitTask* task)
{
	// NOTE: Don't send Stop command, save some traffic.
	DequeueTask(task, false);
}

void CBuilderManager::DoneTask(IUnitTask* task)
{
	DequeueTask(task, true);
}

void CBuilderManager::FallbackTask(CCircuitUnit* unit)
{
	DequeueTask(unit->GetTask());

	const int frame = circuit->GetLastFrame();
	const AIFloat3& pos = unit->GetPos(frame);
	IBuilderTask* task = EnqueuePatrol(IBuilderTask::Priority::LOW, pos, .0f, FRAMES_PER_SEC * 5);
	task->AssignTo(unit);
	task->Start(unit);
}

SBuildChain* CBuilderManager::GetBuildChain(IBuilderTask::BuildType buildType, CCircuitDef* cdef)
{
	auto it1 = buildChains.find(static_cast<IBuilderTask::BT>(buildType));
	if (it1 == buildChains.end()) {
		return nullptr;
	}
	auto it2 = it1->second.find(cdef);
	if (it2 == it1->second.end()) {
		return nullptr;
	}
	return it2->second;
}

// Its current task is a static defence build (MakeCommDefence's, or one it picked up)
static bool IsBuildingDefence(CCircuitUnit* unit)
{
	IUnitTask* task = unit->GetTask();
	return (task != nullptr) && (task->GetType() == IUnitTask::Type::BUILDER)
		&& (static_cast<IBuilderTask*>(task)->GetBuildType() == IBuilderTask::BuildType::DEFENCE);
}

IUnitTask* CBuilderManager::DefaultMakeTask(CCircuitUnit* unit)
{
	CThreatMap* threatMap = circuit->GetThreatMap();
	const int frame = circuit->GetLastFrame();
	const AIFloat3& pos = unit->GetPos(frame);

	const CCircuitDef* cdef = unit->GetCircuitDef();
	const auto it = costQueries.find(unit);
	std::shared_ptr<IPathQuery> query = (it == costQueries.end()) ? nullptr : it->second;
	if ((query != nullptr) && (query->GetState() != IPathQuery::State::READY)) {  // not ready
		return nullptr;
	}

	SCommState* commState = GetCommState(unit);
	if (IsCommCornered(unit, commState)) {  // build cover rather than stand; with none to build it fights at home
		if (IsBuildingDefence(unit)) {
			return nullptr;  // keep at it
		}
		IBuilderTask* task = MakeCommDefence(unit, commState->dangerPos, commState->isAir);
		if (task != nullptr) {
			return task;
		}
	}
	if (IsHomeFight(unit)) {
		return EnqueueCombat(1.5f);
	}

	CPathFinder* pathfinder = circuit->GetPathfinder();
	std::shared_ptr<IPathQuery> q = pathfinder->CreateCostMapQuery(unit, threatMap, frame, pos);
	costQueries[unit] = q;
	pathfinder->RunQuery(q);

	if (query == nullptr) {
		return EnqueueWait(FRAMES_PER_SEC);  // 1st run
	}

	std::shared_ptr<CQueryCostMap> pQuery = std::static_pointer_cast<CQueryCostMap>(query);

	if (cdef->IsRoleComm()) {  // hide commander?
		CEnemyManager* enemyMgr = circuit->GetEnemyManager();
		const CSetupManager::SCommInfo::SHide* hide = circuit->GetSetupManager()->GetHide(cdef);
		if (hide != nullptr) {
			if ((frame < hide->frame) || (GetWorkerCount() <= 2)) {
				return MakeBuilderTask(unit, pQuery.get());
			}
			// hide.latch: the threat trigger stays on once tripped (the threat per ally jumps around in trades and when
			// an ally dies). The air trigger is live: one scout plane must not keep the commander home all game
			const bool isThreat = (enemyMgr->GetMobileThreat() / circuit->GetAllyTeam()->GetAliveSize() >= hide->threat);
			if (isThreat && hide->isLatch) {
				isCommHide = true;
			}
			isCommHideAir = hide->isAir && (enemyMgr->GetEnemyCost(ROLE_TYPE(AIR)) > 1.f);
			const bool isHide = isCommHideAir || isThreat || isCommHide;
			return isHide ? MakeCommTask(unit, pQuery.get(), hide->sqTaskRad) : MakeBuilderTask(unit, pQuery.get());
		}
	}

	return MakeBuilderTask(unit, pQuery.get());
}

const CBuilderManager::SJobRank& CBuilderManager::GetJobRank(const IBuilderTask* task, bool isEnergyStalling) const
{
	switch (task->GetPriority()) {
		case IBuilderTask::Priority::LOW:    return jobRanks[0];
		case IBuilderTask::Priority::NORMAL: return jobRanks[1];
		case IBuilderTask::Priority::HIGH:
			// Mex jobs still get picked during an energy stall, but rank as NORMAL so energy jobs (HIGH while stalling) go first
			return (isEnergyStalling && (task->GetBuildType() == IBuilderTask::BuildType::MEX)) ? jobRanks[1] : jobRanks[2];
		default:                             return jobRanks[3];  // NOW
	}
}

// Travel cost from pos to a job at buildPos on the builder's cost map, at least COST_BASE; < 0 = path blocked by buildings
static float GetJobTravelCost(const CQueryCostMap* query, const CPathFinder* pathfinder,
							  const AIFloat3& pos, const AIFloat3& buildPos, int buildDistance)
{
	// NOTE: squared distance against a plain one, so only a job right at the builder skips the cost map
	const float rawDist = pos.SqDistance2D(buildPos);
	const float distCost = (rawDist < buildDistance)
			? rawDist / pathfinder->GetSquareSize() * COST_BASE
			: query->GetCostAt(buildPos, buildDistance);
	return (distCost < 0.f) ? distCost : std::max(distCost, COST_BASE);
}

// Job on a unit that already exists (repair, or a building in progress): worth the trip
// while we would arrive before the builders already on it fill most of its missing health
static bool IsWorthTrip(const IBuilderTask* task, float distCost, float maxSpeed)
{
	// BA: float time_to_build = targetDef->GetBuildTime() / workerDef->GetBuildSpeed();
	Unit* tu = task->GetTarget()->GetUnit();
	const float maxHealth = tu->GetMaxHealth();
	const float health = tu->GetHealth() - maxHealth * 0.005f;
	const float healthSpeed = maxHealth * task->GetBuildPower() / task->GetCost();
	return ((maxHealth - health) * 0.6f) * maxSpeed > healthSpeed * distCost;
}

IBuilderTask* CBuilderManager::MakeCommTask(CCircuitUnit* unit, const CQueryCostMap* query, float sqMaxBaseRange)
{
	CThreatMap* threatMap = circuit->GetThreatMap();
	threatMap->SetThreatType(unit);
	const IBuilderTask* task = nullptr;
	const int frame = circuit->GetLastFrame();
	AIFloat3 pos = unit->GetPos(frame);

	CEconomyManager* economyMgr = circuit->GetEconomyManager();
	economyMgr->MakeEconomyTasks(pos, unit);
	const bool isNotReady = !economyMgr->IsExcessed();

	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	CInfluenceMap* inflMap = circuit->GetInflMap();
	CPathFinder* pathfinder = circuit->GetPathfinder();
//	CTerrainManager::CorrectPosition(pos);

	CCircuitDef* cdef = unit->GetCircuitDef();
	const float maxSpeed = cdef->GetSpeed() / pathfinder->GetSquareSize() * COST_BASE;
	const int buildDistance = std::max<int>(cdef->GetBuildDistance(), pathfinder->GetSquareSize());
	const AIFloat3& basePos = circuit->GetSetupManager()->GetBasePos();
	float metric = std::numeric_limits<float>::max();
	// Same cap as the pull: idle builders would otherwise all pick priority repairs (NOW outweighs everything)
	const bool isPrioRepairFull = IsPrioRepairFull(unit);
	const bool isEnergyStalling = economyMgr->IsEnergyStalling();
	// Never send the commander into a flee zone, measured at the line the flee stops at
	const float fleeMargin = GetCommFleeMargin(unit);
	const float zoneMargin = (fleeMargin < 0.f) ? -1.f : GetCommWideMargin(fleeMargin);
	for (const std::set<IBuilderTask*>& tasks : buildTasks) {
		for (const IBuilderTask* candidate : tasks) {
			const bool isNow = (candidate->GetPriority() == IBuilderTask::Priority::NOW);
			if ((isNow && isPrioRepairFull && (candidate->GetBuildType() == IBuilderTask::BuildType::REPAIR))
				|| !candidate->CanAssignTo(unit)
				|| (isNotReady && !isNow && (candidate->GetBuildDef() != nullptr)))
			{
				continue;
			}

			// Check time-distance to target first: it's cheaper than the reach and safety tests below
			const AIFloat3& bp = candidate->GetPosition();
			const AIFloat3& buildPos = utils::is_valid(bp) ? bp : pos;
			const float distCost = GetJobTravelCost(query, pathfinder, pos, buildPos, buildDistance);
			if (distCost < 0.f) {
				continue;
			}
			const SJobRank& rank = GetJobRank(candidate, isEnergyStalling);
			const float jobMetric = distCost * rank.invWeight;
			CCircuitUnit* target = candidate->GetTarget();
			if ((jobMetric >= metric)
				|| ((target == nullptr)  // no far fallback here: jobs beyond the travel caps are skipped
					&& ((distCost >= MAX_TRAVEL_SEC * maxSpeed) || (distCost >= rank.travelSec * maxSpeed))))
			{
				continue;
			}

			// Don't start a chase: the target is driving away from this builder; cheaper than the reach and safety tests below
			if (IsRepairTargetFleeing(candidate, unit, pos)) {
				continue;
			}
			if ((zoneMargin >= 0.f) && !IsCommJobSafe(unit, buildPos, zoneMargin)) {
				continue;
			}

			if (isNow) {
				// Disregard safety (unless configured for priority repair)
				if (!terrainMgr->CanReachAt(unit, buildPos, cdef->GetBuildDistance())  // ensure that path always exists
					|| IsUnsafePrioRepair(candidate, unit, buildPos))
				{
					continue;
				}

			} else {

				if (((sqMaxBaseRange > 0.f) && (basePos.SqDistance2D(buildPos) > sqMaxBaseRange))  // optional cap
					|| ((zoneMargin >= 0.f) && !IsCommTerritory(unit, buildPos))  // allied strength outweighs the enemy's there
					|| !terrainMgr->CanReachAtSafe(unit, buildPos, cdef->GetBuildDistance())  // ensure that path always exists
					|| (inflMap->GetInfluenceAt(buildPos) < -INFL_EPS))  // safety check
				{
					continue;
				}
			}

			if ((target == nullptr) || IsWorthTrip(candidate, distCost, maxSpeed)) {
				task = candidate;
				metric = jobMetric;
			}
		}
	}

	if ((task == nullptr) &&
		((unit->GetTask()->GetType() != IUnitTask::Type::BUILDER) || (static_cast<IBuilderTask*>(unit->GetTask())->GetBuildType() != IBuilderTask::BuildType::GUARD)))
	{
		CCircuitUnit* vip = circuit->GetFactoryManager()->GetClosestFactory(pos);
		if ((vip != nullptr) && ((zoneMargin < 0.f) || IsCommJobSafe(unit, vip->GetPos(frame), zoneMargin))) {
			task = EnqueueGuard(IBuilderTask::Priority::NORMAL, vip, FRAMES_PER_SEC * 60);
		} else {
			task = EnqueuePatrol(IBuilderTask::Priority::LOW, pos, .0f, FRAMES_PER_SEC * 5);
		}
	}

	return const_cast<IBuilderTask*>(task);
}

IBuilderTask* CBuilderManager::MakeBuilderTask(CCircuitUnit* unit, const CQueryCostMap* query)
{
	CThreatMap* threatMap = circuit->GetThreatMap();
	threatMap->SetThreatType(unit);
	const IBuilderTask* task = nullptr;
	const int frame = circuit->GetLastFrame();
	AIFloat3 pos = unit->GetPos(frame);

	CEconomyManager* economyMgr = circuit->GetEconomyManager();
	task = economyMgr->MakeEconomyTasks(pos, unit);
//	if (task != nullptr) {
//		return task;
//	}
	const bool isStalling = economyMgr->IsMetalEmpty() &&
							(economyMgr->GetAvgMetalIncome() * 1.2f < economyMgr->GetMetalPull()) &&
							(metalPull > economyMgr->GetPullMtoS() * circuit->GetFactoryManager()->GetMetalPull());
	const bool isNotReady = !economyMgr->IsExcessed() || isStalling;

	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	CInfluenceMap* inflMap = circuit->GetInflMap();
	CPathFinder* pathfinder = circuit->GetPathfinder();
//	CTerrainManager::CorrectPosition(pos);

	CCircuitDef* cdef = unit->GetCircuitDef();
	const float maxSpeed = cdef->GetSpeed() / pathfinder->GetSquareSize() * COST_BASE;
	const float maxThreat = threatMap->GetUnitThreat(unit);
	const int buildDistance = std::max<int>(cdef->GetBuildDistance(), pathfinder->GetSquareSize());
	float metric = std::numeric_limits<float>::max();
	// Jobs beyond their priority's travel cap (economy.builder_jobs.travel) only count when nothing is within its cap,
	// so a far mex still gets built when it is the only work, but never beats nearby work
	const IBuilderTask* farTask = nullptr;
	float farMetric = std::numeric_limits<float>::max();
	// Same cap as the pull: idle builders would otherwise all pick priority repairs (NOW outweighs everything)
	const bool isPrioRepairFull = IsPrioRepairFull(unit);
	const bool isEnergyStalling = economyMgr->IsEnergyStalling();
	// Never send the commander into a flee zone, measured at the line the flee stops at
	const float fleeMargin = GetCommFleeMargin(unit);
	const float zoneMargin = (fleeMargin < 0.f) ? -1.f : GetCommWideMargin(fleeMargin);
	for (const std::set<IBuilderTask*>& tasks : buildTasks) {
		for (const IBuilderTask* candidate : tasks) {
			const bool isNow = (candidate->GetPriority() == IBuilderTask::Priority::NOW);
			if ((isNow && isPrioRepairFull && (candidate->GetBuildType() == IBuilderTask::BuildType::REPAIR))
				|| !candidate->CanAssignTo(unit)
				|| (isNotReady && !isNow
					&& (candidate->GetBuildDef() != nullptr)
					&& !economyMgr->IsIgnoreStallingPull(candidate)))
			{
				continue;
			}

			// Check time-distance to target first: it's cheaper than the reach and safety tests below
			const AIFloat3& bp = candidate->GetPosition();
			const AIFloat3& buildPos = utils::is_valid(bp) ? bp : pos;
			const float distCost = GetJobTravelCost(query, pathfinder, pos, buildPos, buildDistance);
			if (distCost < 0.f) {
				continue;
			}
			const SJobRank& rank = GetJobRank(candidate, isEnergyStalling);
			const float jobMetric = distCost * rank.invWeight;
			const bool isFar = !isNow && (distCost >= rank.travelSec * maxSpeed);
			if (jobMetric >= (isFar ? farMetric : metric)) {
				continue;  // can't beat the best job so far
			}

			// Don't start a chase: the target is driving away from this builder; cheaper than the reach and safety tests below
			if (IsRepairTargetFleeing(candidate, unit, pos)) {
				continue;
			}
			if ((zoneMargin >= 0.f) && !IsCommJobSafe(unit, buildPos, zoneMargin)) {
				continue;
			}

			if (isNow) {
				// Disregard safety (unless configured for priority repair)
				if (!terrainMgr->CanReachAt(unit, buildPos, cdef->GetBuildDistance())  // ensure that path always exists
					|| IsUnsafePrioRepair(candidate, unit, buildPos))
				{
					continue;
				}

			} else {

				CCircuitDef* buildDef = candidate->GetBuildDef();
				const float buildThreat = (buildDef != nullptr) ? buildDef->GetPower() : 0.f;
				if (!terrainMgr->CanReachAt(unit, buildPos, cdef->GetBuildDistance())  // ensure that path always exists
					|| (((buildThreat < THREAT_MIN) && (threatMap->GetThreatAt(buildPos) > maxThreat))
						&& (inflMap->GetInfluenceAt(buildPos) < -INFL_EPS)))  // safety check
				{
					continue;
				}
			}

			if (isFar) {
				farTask = candidate;
				farMetric = jobMetric;
			} else if ((candidate->GetTarget() == nullptr) || IsWorthTrip(candidate, distCost, maxSpeed)) {
				task = candidate;
				metric = jobMetric;
			}
		}
	}

	if (task == nullptr) {
		task = farTask;
	}
	if (task == nullptr) {
		if ((unit->GetTask() != idleTask) || isNotReady) {
			return nullptr;  // current task is in danger or unreachable
		}
		if ((zoneMargin >= 0.f) && !IsCommJobSafe(unit, pos, zoneMargin)) {
			return nullptr;  // a commander makes no job where it stands inside a flee zone
		}
		task = CreateBuilderTask(pos, unit);
	}

	return const_cast<IBuilderTask*>(task);
}

IBuilderTask* CBuilderManager::CreateBuilderTask(const AIFloat3& position, CCircuitUnit* unit)
{
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	if (terrainMgr->IsZoneAlly(position)) {
		return EnqueuePatrol(IBuilderTask::Priority::LOW, position, .0f, FRAMES_PER_SEC * 20);
	}

	CEconomyManager* ecoMgr = circuit->GetEconomyManager();
	IBuilderTask* task = ecoMgr->UpdateEnergyTasks(position, unit);
	if (task != nullptr) {
		return task;
	}
	task = ecoMgr->UpdateReclaimTasks(position, unit, false);
//	if (task != nullptr) {
//		return task;
//	}

	// FIXME: Eco rules. It should never get here
	CCircuitDef* buildDef/* = nullptr*/;
	const float metalIncome = std::min(ecoMgr->GetAvgMetalIncome(), ecoMgr->GetAvgEnergyIncome()) * ecoMgr->GetEcoFactor();
	if (metalIncome < super.minIncome) {
		float energyMake;
		buildDef = ecoMgr->GetLowEnergy(position, energyMake);
		if (buildDef == nullptr) {  // position can be in danger
			buildDef = ecoMgr->GetDefaultDef();
		}
		if ((buildDef != nullptr) && (buildDef->GetCount() < 10) && buildDef->IsAvailable(circuit->GetLastFrame())) {
			return EnqueueTask(IBuilderTask::Priority::NORMAL, buildDef, position, IBuilderTask::BuildType::ENERGY);
		}
	} else {
		CMilitaryManager* militaryMgr = circuit->GetMilitaryManager();
		buildDef = militaryMgr->GetBigGunDef();
		if ((buildDef != nullptr) && (buildDef->GetCostM() < super.maxTime * metalIncome)) {
			const std::set<IBuilderTask*>& tasks = GetTasks(IBuilderTask::BuildType::BIG_GUN);
			if (tasks.empty()) {
				if (buildDef->IsAvailable(circuit->GetLastFrame()) && militaryMgr->IsNeedBigGun(buildDef)) {
					AIFloat3 pos = militaryMgr->GetBigGunPos(buildDef);
					return EnqueueTask(IBuilderTask::Priority::NORMAL, buildDef, pos,
									   IBuilderTask::BuildType::BIG_GUN);
				}
			} else {
				return *tasks.begin();
			}
		}
	}

	return EnqueuePatrol(IBuilderTask::Priority::LOW, position, .0f, FRAMES_PER_SEC * 20);
}

void CBuilderManager::AddBuildList(CCircuitUnit* unit)
{
	CCircuitDef* cDef = unit->GetCircuitDef();
	if (cDef->GetCount() > 1) {
		return;
	}

	const std::unordered_set<CCircuitDef::Id>& buildOptions = cDef->GetBuildOptions();
	std::set<CCircuitDef*> buildDefs;
	for (CCircuitDef::Id build : buildOptions) {
		CCircuitDef* cdef = circuit->GetCircuitDef(build);
		if (cdef->GetBuildCount() == 0) {
			buildDefs.insert(cdef);
		}
		cdef->IncBuild();
	}

	if (!buildDefs.empty()) {
		circuit->GetEconomyManager()->AddEnergyDefs(buildDefs);
	}

	// TODO: Same thing with factory, etc.
}

void CBuilderManager::RemoveBuildList(CCircuitUnit* unit)
{
	CCircuitDef* cDef = unit->GetCircuitDef();
	if (cDef->GetCount() > 1) {
		return;
	}

	const std::unordered_set<CCircuitDef::Id>& buildOptions = cDef->GetBuildOptions();
	std::set<CCircuitDef*> buildDefs;
	for (CCircuitDef::Id build : buildOptions) {
		CCircuitDef* cdef = circuit->GetCircuitDef(build);
		cdef->DecBuild();
		if (cdef->GetBuildCount() == 0) {
			buildDefs.insert(cdef);
		}
	}

	if (!buildDefs.empty()) {  // throws exception on set::erase otherwise
		circuit->GetEconomyManager()->RemoveEnergyDefs(buildDefs);
	}

	// TODO: Same thing with factory, etc.
}

void CBuilderManager::Watchdog()
{
	SCOPED_TIME(circuit, __PRETTY_FUNCTION__);
	CEconomyManager* economyMgr = circuit->GetEconomyManager();
	Resource* metalRes = economyMgr->GetMetalRes();
	// somehow workers get stuck
	for (CCircuitUnit* worker : workers) {
		if (worker->GetTask()->GetType() == IUnitTask::Type::PLAYER) {
			continue;
		}
		Unit* u = worker->GetUnit();
		auto commands = u->GetCurrentCommands();
		// TODO: Ignore workers with idle and wait task? (.. && worker->GetTask()->IsBusy())
		if (commands.empty() && (u->GetResourceUse(metalRes) == .0f) && (u->GetVel() == ZeroVector)) {
			worker->GetTask()->OnUnitMoveFailed(worker);
		}
		utils::free_clear(commands);
	}

	// find unfinished abandoned buildings
	float maxCost = MAX_BUILD_SEC * std::min(economyMgr->GetAvgMetalIncome(), buildPower) * economyMgr->GetEcoFactor();
	// TODO: Include special units
	for (auto& kv : circuit->GetTeamUnits()) {
		CCircuitUnit* unit = kv.second;
		if (!unit->GetCircuitDef()->IsMobile() &&
			(unfinishedUnits.find(unit) == unfinishedUnits.end()) &&
			(repairedUnits.find(unit->GetId()) == repairedUnits.end()) &&
			(reclaimedUnits.find(unit) == reclaimedUnits.end()))
		{
			Unit* u = unit->GetUnit();
			if (u->IsBeingBuilt()) {
				float maxHealth = u->GetMaxHealth();
				float buildPercent = (maxHealth - u->GetHealth()) / maxHealth;
				CCircuitDef* cdef = unit->GetCircuitDef();
				if ((cdef->GetBuildTime() * buildPercent < maxCost) || (*cdef == *terraDef)) {
					EnqueueRepair(IBuilderTask::Priority::NORMAL, unit);
				} else {
					EnqueueReclaim(IBuilderTask::Priority::NORMAL, unit);
				}
//			} else if (u->GetHealth() < u->GetMaxHealth()) {
//				EnqueueRepair(IBuilderTask::Priority::NORMAL, unit);
			}
		}
	}
}

void CBuilderManager::UpdateEscort()
{
	SCOPED_TIME(circuit, __PRETTY_FUNCTION__);
	const unsigned maxEscorts = workers.size() * escort.share;
	if (escortTasks.size() >= maxEscorts) {
		return;
	}

	std::set<ISquadTask*> escorted;
	for (IUnitTask* task : escortTasks) {
		escorted.insert(static_cast<CBEscortTask*>(task)->GetSquad());
	}

	const int frame = circuit->GetLastFrame();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	const std::set<IFighterTask*>& tasks = circuit->GetMilitaryManager()->GetTasks(IFighterTask::FightType::ATTACK);
	for (IFighterTask* candy : tasks) {
		ISquadTask* squad = static_cast<ISquadTask*>(candy);
		CCircuitUnit* leader = squad->GetLeader();
		if ((leader == nullptr) || (squad->GetAttackPower() < escort.minPower) || (escorted.find(squad) != escorted.end())) {
			continue;
		}

		// Nearest builder that is not busy with urgent or escort work, within range of any group member
		const AIFloat3& pos = leader->GetPos(frame);
		const std::set<CCircuitUnit*>& members = squad->GetAssignees();
		CCircuitUnit* bestWorker = nullptr;
		float minSqDist = SQUARE(escort.range);
		for (CCircuitUnit* worker : workers) {
			IUnitTask* curTask = worker->GetTask();
			if ((!escort.isComm && worker->GetCircuitDef()->IsRoleComm())
				|| !worker->GetCircuitDef()->IsAbleToRepair()
				|| ((curTask->GetType() != IUnitTask::Type::BUILDER) && (curTask->GetType() != IUnitTask::Type::IDLE)))
			{
				continue;
			}
			if (curTask->GetType() == IUnitTask::Type::BUILDER) {
				IBuilderTask* taskB = static_cast<IBuilderTask*>(curTask);
				if ((curTask->GetPriority() == IBuilderTask::Priority::NOW)
					|| (static_cast<int>(curTask->GetPriority()) > escort.maxPriority)
					|| (taskB->GetBuildType() == IBuilderTask::BuildType::REPAIR)
					|| (taskB->GetBuildType() == IBuilderTask::BuildType::GUARD))
				{
					continue;
				}
			}
			const AIFloat3& workerPos = worker->GetPos(frame);
			float sqDist = pos.SqDistance2D(workerPos);
			for (CCircuitUnit* member : members) {
				sqDist = std::min(sqDist, member->GetPos(frame).SqDistance2D(workerPos));
			}
			if ((sqDist < minSqDist) && terrainMgr->CanReachAt(worker, pos, worker->GetCircuitDef()->GetBuildDistance())) {
				minSqDist = sqDist;
				bestWorker = worker;
			}
		}
		if (bestWorker == nullptr) {
			continue;
		}

		CBEscortTask* task = new CBEscortTask(this, squad);
		buildUpdates.push_back(task);
		escortTasks.insert(task);
		AssignTask(bestWorker, task);
		return;  // one per update
	}
}

/*
 * Commander flee and hide
 */
bool CBuilderManager::IsHomeFight(CCircuitUnit* unit)
{
	// An armed builder near base with a weaker enemy group close by fights. A commander only where it wouldn't flee,
	// with the flee's end margin (1.25x covered) so a fight it starts doesn't turn into a flee at the first hit;
	// with nowhere better to go it fights anyway
	const AIFloat3& pos = unit->GetPos(circuit->GetLastFrame());
	if ((unit->GetCircuitDef()->GetPower() <= THREAT_MIN)
		|| (pos.SqDistance2D(circuit->GetSetupManager()->GetBasePos()) >= SQUARE(circuit->GetMilitaryManager()->GetBaseDefRange()))
		|| !circuit->GetEnemyManager()->IsEnemyNear(pos, circuit->GetThreatMap()->GetUnitThreat(unit) * 1.5f))
	{
		return false;
	}
	SCommState* state = GetCommState(unit);
	return (state == nullptr) || (state->wideCount == 0)
		|| IsCommCovered(state, state->wideThreat, COMM_END_COVER_MOD) || !HasCommEscape(unit, state);
}

float CBuilderManager::GetCommFleeMargin(CCircuitUnit* unit) const
{
	if (!unit->GetCircuitDef()->IsRoleComm()) {
		return -1.f;
	}
	const CSetupManager::SCommInfo::SHide* hide = circuit->GetSetupManager()->GetHide(unit->GetCircuitDef());
	return (hide == nullptr) ? -1.f : hide->fleeMargin;
}

void CBuilderManager::UpdateCommDangers()
{
	// As ISquadTask::FindFleeDanger: enemies that can act and hit ground or water units (IFighterTask::IsGroundDanger,
	// gunships included), unidentified blips while they move and sit low. Seen: in radar or LOS now; the flee only
	// counts those (the threat map keeps last-known ghosts, which would hold it back for long). Jobs and morph starts
	// also count known enemies gone from radar and LOS within COMM_DANGER_MEMORY, at their last spot, so an enemy at
	// the edge of sight doesn't pull the commander to a job and back
	const int frame = circuit->GetLastFrame();
	if (commDangerFrame == frame) {
		return;
	}
	commDangerFrame = frame;
	commDangers.clear();
	CMap* map = circuit->GetMap();
	for (auto& kv : circuit->GetEnemyInfos()) {
		CEnemyInfo* enemy = kv.second;
		float range;
		bool isSeen = true;
		if (IFighterTask::IsGroundDanger(enemy, true, range)) {
			if ((enemy->GetCircuitDef() == nullptr) && ISquadTask::IsHighBlip(map, enemy->GetPos())) {
				continue;  // a plane
			}
		} else {
			const int lastSeen = enemy->GetData()->GetLastSeen();
			if (enemy->IsHidden() || enemy->IsInRadarOrLOS() || (enemy->GetCircuitDef() == nullptr)
				|| (lastSeen < 0) || (frame - lastSeen > COMM_DANGER_MEMORY)
				|| !IFighterTask::IsGroundDangerDef(enemy->GetCircuitDef(), true, range))
			{
				continue;
			}
			isSeen = false;
		}
		CCircuitDef* edef = enemy->GetCircuitDef();
		float threat = enemy->GetThreat();
		if ((edef != nullptr) && edef->IsRoleComm() && (commThrMod > 0.f)) {
			threat /= commThrMod;  // quota.thr_mod.comm scales every commander down, ours is the one at risk
		}
		commDangers.push_back({enemy->GetPos(), range, threat, (edef != nullptr) && edef->IsAbleToFly(), isSeen});
	}
}

bool CBuilderManager::IsCommSafe(const AIFloat3& pos, float margin, bool isSeenOnly)
{
	UpdateCommDangers();
	for (const SCommDanger& danger : commDangers) {
		if ((danger.isSeen || !isSeenOnly) && (pos.SqDistance2D(danger.pos) <= SQUARE(danger.range + margin))) {
			return false;
		}
	}
	return true;
}

float CBuilderManager::GetCommCover(const AIFloat3& pos, float radius, CCircuitUnit* skip) const
{
	// Mobile armed units that can hit ground or water (not aircraft) and static defences within radius of pos, as
	// ISquadTask::IsFleeCovered. The list is the one the influence map last refreshed (every few frames): no forced
	// refresh here, which would re-create every allied unit's entry. A unit that died since counts 0 (no health)
	const int frame = circuit->GetLastFrame();
	const float sqRadius = SQUARE(radius);
	float cover = 0.f;
	for (auto& kv : circuit->GetFriendlyUnits()) {
		CAllyUnit* ally = kv.second;
		CCircuitDef* adef = ally->GetCircuitDef();
		if ((adef == nullptr) || !adef->IsAttacker() || adef->IsAbleToFly()
			|| ((skip != nullptr) && (ally->GetId() == skip->GetId()))
			|| (adef->IsMobile() ? !(adef->HasAntiLand() || adef->HasAntiWater()) : !IFighterTask::IsStaticDefence(adef)))
		{
			continue;
		}
		const AIFloat3& allyPos = ally->GetPos(frame);
		if (utils::is_valid(allyPos) && (allyPos.SqDistance2D(pos) <= sqRadius)) {
			cover += IFighterTask::GetCoverPower(ally);
		}
	}
	return cover;
}

CBuilderManager::SCommState* CBuilderManager::GetCommState(CCircuitUnit* unit)
{
	// Once per frame per commander: every flee, fight, job and morph decision reads the same numbers
	const float margin = GetCommFleeMargin(unit);
	if (margin < 0.f) {
		return nullptr;
	}
	const int frame = circuit->GetLastFrame();
	SCommState& state = commStates[unit];
	if (state.frame == frame) {
		return &state;
	}
	const CSetupManager::SCommInfo::SHide* hide = circuit->GetSetupManager()->GetHide(unit->GetCircuitDef());
	state = SCommState();
	state.frame = frame;
	state.allyThreat = hide->fleeAllyThreat;
	state.wideMargin = GetCommWideMargin(margin);
	const AIFloat3& pos = unit->GetPos(frame);
	UpdateCommDangers();
	float minSqDist = std::numeric_limits<float>::max();
	for (const SCommDanger& danger : commDangers) {
		const float sqDist = pos.SqDistance2D(danger.pos);
		if (sqDist > SQUARE(danger.range + state.wideMargin)) {
			continue;
		}
		state.allThreat += danger.threat;
		++state.allCount;
		if (!danger.isSeen) {
			continue;
		}
		state.wideThreat += danger.threat;
		++state.wideCount;
		if (sqDist <= SQUARE(danger.range + margin)) {
			state.nearThreat += danger.threat;
			++state.nearCount;
		}
		if (sqDist < minSqDist) {
			minSqDist = sqDist;
			state.dangerPos = danger.pos;
			state.isAir = danger.isAir;
		}
	}
	if ((state.allCount > 0) && (state.allyThreat >= 0.f)) {
		state.cover = GetCommCover(pos, hide->fleeAllyRadius, nullptr);  // the commander itself included
	}
	return &state;
}

bool CBuilderManager::HasCommEscape(CCircuitUnit* unit, SCommState* state)
{
	// A flee spot (FindCommFleeGoal) outside every seen flee zone and not at its feet; once per state
	if (state->escapeFrame != state->frame) {
		state->escapeFrame = state->frame;
		state->isEscape = FindCommFleeGoal(unit, state->dangerPos, state->isAir, state->wideMargin, state->escapeGoal);
	}
	return state->isEscape;
}

bool CBuilderManager::IsCommInDanger(CCircuitUnit* unit)
{
	// Morph start: no uncovered danger (recently seen ones included) within the wide line
	SCommState* state = GetCommState(unit);
	return (state != nullptr) && (state->allCount > 0) && !IsCommCovered(state, state->allThreat, 1.f);
}

bool CBuilderManager::IsCommJobSafe(CCircuitUnit* unit, const AIFloat3& pos, float margin)
{
	// Outside every flee zone at the wide line (recently seen dangers included), or covered there as a flee would end:
	// allies within flee_ally_radius of the job plus the commander itself reach 1.25x the threat that reaches it
	if (IsCommSafe(pos, margin, false)) {
		return true;
	}
	const CSetupManager::SCommInfo::SHide* hide = circuit->GetSetupManager()->GetHide(unit->GetCircuitDef());
	if (hide->fleeAllyThreat < 0.f) {
		return false;
	}
	float threat = 0.f;
	for (const SCommDanger& danger : commDangers) {  // filled by IsCommSafe
		if (pos.SqDistance2D(danger.pos) <= SQUARE(danger.range + margin)) {
			threat += danger.threat;
		}
	}
	const float cover = GetCommCover(pos, hide->fleeAllyRadius, unit) + IFighterTask::GetCoverPower(unit);
	return cover - threat * COMM_END_COVER_MOD >= hide->fleeAllyThreat;
}

bool CBuilderManager::IsCommCornered(CCircuitUnit* unit, SCommState* state)
{
	// An uncovered seen danger within the wide line and nowhere better to flee to
	return (state != nullptr) && (state->wideCount > 0) && !IsCommCovered(state, state->wideThreat, 1.f)
		&& !HasCommEscape(unit, state);
}
IBuilderTask* CBuilderManager::MakeCommDefence(CCircuitUnit* unit, const AIFloat3& dangerPos, bool isAir)
{
	// The defence adds the cover (GetCommCover counts finished static defences) that lets the commander stay.
	// It shoots while it builds. One at a time: an unfinished one nearby is helped rather than a new one queued.
	// None while metal is stalling: it would stand nanolathing a frame that never finishes, so it fights instead
	if (IsBuildingDefence(unit) || circuit->GetEconomyManager()->IsMetalEmpty()) {
		return nullptr;
	}
	const int frame = circuit->GetLastFrame();
	const AIFloat3& pos = unit->GetPos(frame);
	CCircuitDef* cdef = unit->GetCircuitDef();
	const float buildDist = cdef->GetBuildDistance();

	for (IBuilderTask* t : GetTasks(IBuilderTask::BuildType::DEFENCE)) {
		if ((t->GetTaskPos().SqDistance2D(pos) < SQUARE(buildDist + DEFAULT_SLACK * 2)) && t->CanAssignTo(unit)) {
			return t;
		}
	}

	// The first one porcupine.land/water lists (its build order; MakeDefence's water test) that can shoot the danger:
	// against gunships one that hits air
	CMilitaryManager* militaryMgr = circuit->GetMilitaryManager();
	const bool isWater = circuit->GetMap()->GetElevationAt(pos.x, pos.z) < -SQUARE_SIZE * 5;
	CCircuitDef* buildDef = nullptr;
	for (CCircuitDef* ddef : isWater ? militaryMgr->GetWaterDefenders() : militaryMgr->GetLandDefenders()) {
		if (ddef->IsAvailable(frame) && cdef->CanBuild(ddef) && ddef->IsAttacker()
			&& (isAir ? ddef->HasAntiAir() : (isWater ? ddef->HasAntiWater() : ddef->HasAntiLand())))
		{
			buildDef = ddef;
			break;
		}
	}
	if (buildDef == nullptr) {
		return nullptr;
	}

	// Behind the commander, away from the danger, within its build range
	AIFloat3 buildPos = pos;
	AIFloat3 away = pos - dangerPos;
	away.y = 0.f;
	const float len = away.Length2D();
	if (len > 1.f) {
		buildPos += away / len * (buildDist * 0.5f);
	}
	CTerrainManager::CorrectPosition(buildPos);
	return EnqueueTask(IBuilderTask::Priority::HIGH, buildDef, buildPos, IBuilderTask::BuildType::DEFENCE,
					   buildDef->GetCostM(), SQUARE_SIZE * 8.f);
}

bool CBuilderManager::IsCommTerritory(CCircuitUnit* unit, const AIFloat3& pos) const
{
	// The influence map spreads strength about 4x further than weapon reach: a territory test, not a range test.
	// Our ground: allied armed units (the commander's own influence left out) or our buildings and static defence
	// there; with enemy air seen, only the buildings', since bombers never set off the flee
	CInfluenceMap* inflMap = circuit->GetInflMap();
	const float ally = inflMap->GetAllyInflAt(pos) - inflMap->GetMobileArmedInflAt(unit, pos);
	const float enemy = inflMap->GetEnemyInflAt(pos);
	const float defend = inflMap->GetAllyDefendInflAt(pos);
	const bool isOurs = isCommHideAir ? (defend > INFL_EPS) : ((ally > INFL_EPS) || (defend > INFL_EPS));
	return isOurs && (ally >= enemy);
}

bool CBuilderManager::FindCommFleeGoal(CCircuitUnit* unit, const AIFloat3& dangerPos, bool isAir, float margin,
									   AIFloat3& outGoal)
{
	// As ISquadTask::FindFleeGoal (attack/defend groups, else defended clusters; then the base) plus the closest
	// factory, leaving out spots inside a seen flee zone. Gunships: factory and base only, where anti-air is likely.
	// false: no such spot, or only one at its feet (cornered)
	const int frame = circuit->GetLastFrame();
	const AIFloat3& pos = unit->GetPos(frame);
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	STerrainMapArea* area = unit->GetArea();
	commFleeSpots.clear();
	if (!isAir) {
		circuit->GetMilitaryManager()->FillSafePos(unit, commFleeSpots);
	}
	CCircuitUnit* factory = circuit->GetFactoryManager()->GetClosestFactory(pos);
	if (factory != nullptr) {
		commFleeSpots.push_back(factory->GetPos(frame));
	}
	commFleeSpots.push_back(circuit->GetSetupManager()->GetBasePos());
	auto it = std::remove_if(commFleeSpots.begin(), commFleeSpots.end(), [&](const AIFloat3& spot) {
		return !terrainMgr->CanMoveToPos(area, spot) || !IsCommSafe(spot, margin, true);
	});
	commFleeSpots.erase(it, commFleeSpots.end());
	const AIFloat3* goal = IFighterTask::PickFleeSpot(pos, dangerPos, commFleeSpots);
	if ((goal == nullptr) || (pos.SqDistance2D(*goal) < SQUARE(COMM_ESCAPE_MIN))) {
		return false;
	}
	outGoal = *goal;
	return true;
}

void CBuilderManager::UpdateCommFlee()
{
	// Flee (always on): a seen ground danger within its weapon range + hide.flee_margin and the commander with the
	// allies around can't take it (hide.flee_ally_threat). Keeps going till nothing is within the wide line or it is
	// covered 1.25x at the flee goal, and at least COMM_FLEE_MIN_FRAMES. Cornered (nowhere better to go): it builds a
	// static defence, else stays at it (IsHomeFight lets it fight at home). A health retreat wins
	SCOPED_TIME(circuit, __PRETTY_FUNCTION__);
	if (comms.empty()) {
		return;
	}
	const int frame = circuit->GetLastFrame();
	for (CCircuitUnit* unit : comms) {
		IUnitTask* task = unit->GetTask();
		if (task == nullptr) {
			continue;
		}
		const IUnitTask::Type type = task->GetType();
		if ((type == IUnitTask::Type::PLAYER) || (type == IUnitTask::Type::NIL)) {
			continue;
		}
		CRetreatTask* retreat = (type == IUnitTask::Type::RETREAT) ? static_cast<CRetreatTask*>(task) : nullptr;
		if ((retreat != nullptr) && !retreat->IsFlee()) {
			continue;  // health retreat
		}
		SCommState* state = GetCommState(unit);
		if (state == nullptr) {
			continue;  // flee off
		}
		if (retreat != nullptr) {
			if (unit->GetHealthPercent() < unit->GetCircuitDef()->GetRetreat()) {
				retreat->StopFlee(unit);  // hurt on the way: a plain retreat for repair from here
				continue;
			}
			if (frame < retreat->GetFleeFrame() + COMM_FLEE_MIN_FRAMES) {
				continue;
			}
			if ((state->wideCount == 0)
				|| (retreat->IsArrived() && IsCommCovered(state, state->wideThreat, COMM_END_COVER_MOD)))
			{
				retreat->EndFlee(unit);  // back to work: jobs stay out of uncovered flee zones
				continue;
			}
			const AIFloat3& goal = retreat->GetFleeGoal();
			if (!IsCommSafe(goal, state->wideMargin, true)) {
				if (!HasCommEscape(unit, state)) {
					if (retreat->IsArrived()) {
						retreat->EndFlee(unit);  // nowhere better to go (e.g. a turret reaches every spot): build or fight here
					}
				} else if (state->escapeGoal.SqDistance2D(goal) > SQUARE(DEFAULT_SLACK)) {
					retreat->Redirect(unit, state->escapeGoal);
				}
			}
			continue;
		}

		if ((state->nearCount == 0) || IsCommCovered(state, state->nearThreat, 1.f)) {
			continue;
		}
		if (!HasCommEscape(unit, state)) {
			IBuilderTask* defence = MakeCommDefence(unit, state->dangerPos, state->isAir);
			if (defence != nullptr) {
				AssignTask(unit, defence);
			}
			continue;
		}
		// A commander upgrade is a combat morph in Zero-K: it goes on while the commander moves, so it isn't stopped
		// (stopping refunds only 2/3 of the metal spent)
		CRetreatTask* flee = EnqueueRetreat();
		flee->SetFlee(state->escapeGoal, frame);
		AssignTask(unit, flee);
	}
}

void CBuilderManager::UpdateIdle()
{
	SCOPED_TIME(circuit, __PRETTY_FUNCTION__);
	idleTask->Update();
}

void CBuilderManager::UpdateBuild()
{
	SCOPED_TIME(circuit, __PRETTY_FUNCTION__);
	if (buildIterator >= buildUpdates.size()) {
		buildIterator = 0;
	}

	int lastFrame = circuit->GetLastFrame();
	// stagger the Update's
	unsigned int n = (buildUpdates.size() / TEAM_SLOWUPDATE_RATE) + 1;

	while ((buildIterator < buildUpdates.size()) && (n != 0)) {
		IUnitTask* task = buildUpdates[buildIterator];
		if (task->IsDead()) {
			buildUpdates[buildIterator] = buildUpdates.back();
			buildUpdates.pop_back();
			task->ClearRelease();  // delete task;
		} else {
			int frame = task->GetLastTouched();
			int timeout = task->GetTimeout();
			if ((frame != -1) && (timeout > 0) && (lastFrame - frame >= timeout)) {
				AbortTask(task);
			} else {
				task->Update();
			}
			++buildIterator;
			n--;
		}
	}
}

void CBuilderManager::UpdateAreaUsers()
{
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	buildAreas.clear();
	for (auto mtId : workerMobileTypes) {
		for (auto& area : terrainMgr->GetMobileTypeById(mtId)->area) {
			buildAreas[&area] = std::map<CCircuitDef*, int>();
		}
	}
	buildAreas[nullptr] = std::map<CCircuitDef*, int>();  // air

	for (auto& kv : circuit->GetTeamUnits()) {
		CCircuitUnit* unit = kv.second;
		if (workerDefs.find(unit->GetCircuitDef()) != workerDefs.end()) {
			++buildAreas[unit->GetArea()][unit->GetCircuitDef()];
		}
	}

	std::set<IBuilderTask*> removeTasks;
	for (auto& tasks : buildTasks) {
		for (IBuilderTask* task : tasks) {
			CCircuitDef* cdef = task->GetBuildDef();
			if ((cdef != nullptr) && (task->GetTarget() == nullptr) && !IsBuilderInArea(cdef, task->GetPosition())) {
				removeTasks.insert(task);
			}
		}
	}
	for (IBuilderTask* task : removeTasks) {
		AbortTask(task);
	}
}

void CBuilderManager::Load(std::istream& is)
{
//	std::streamsize y;
//	int yo, yoyo;
//	is >> y;
//	is.seekg(is.tellg() + std::streampos(sizeof('\n')));
//	is.read(reinterpret_cast<char*>(&yo), sizeof(yo));
//	is.read(reinterpret_cast<char*>(&yoyo), sizeof(yoyo));
//	circuit->LOG("LOAD ID: %i | %i | %i", y, yo, yoyo);

	/*
	 * Restore data
	 */
//	for (IUnitTask* task : buildUpdates) {
//		if (task->GetType() != IUnitTask::Type::BUILDER) {
//			continue;
//		}
//		IBuilderTask* bt = static_cast<IBuilderTask*>(task);
//		if (bt->GetBuildType() < IBuilderTask::BuildType::_SIZE_) {
//			buildTasks[static_cast<IBuilderTask::BT>(bt->GetBuildType())].insert(bt);
//			buildTasksCount++;
//		}
//	}
}

void CBuilderManager::Save(std::ostream& os) const
{
//	std::stringstream tmp;
//	int i = circuit->GetSkirmishAIId();
//	tmp.write(reinterpret_cast<const char*>(&i), sizeof(i));
//	i = 42;
//	tmp.write(reinterpret_cast<const char*>(&i), sizeof(i));
//	os << std::streamsize(1024) << '\n' << tmp.rdbuf();

	/*
	 * Save tasks
	 */
//	int size = buildUpdates.size();
//	os.write(reinterpret_cast<const char*>(&size), sizeof(size));
//	for (IUnitTask* task : buildUpdates) {
//		os << *task;
//	}
//	os.write(reinterpret_cast<const char*>(&buildIterator), sizeof(buildIterator));
}

} // namespace circuit

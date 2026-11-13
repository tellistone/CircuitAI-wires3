/*
 * BuilderManager.cpp
 *
 *  Created on: Dec 1, 2014
 *      Author: rlcevg
 */

#include "module/BuilderManager.h"
#include "module/EconomyManager.h"
#include "module/MilitaryManager.h"
#include "module/SensorPlanner.h"
#include "module/FactoryManager.h"
#include "map/InfluenceMap.h"
#include "map/ThreatMap.h"
#include "spring/SpringMap.h"
#include "resource/MetalManager.h"
#include "setup/SetupManager.h"
#include "terrain/TerrainManager.h"
#include "unit/enemy/EnemyUnit.h"
#include "unit/ally/AllyUnit.h"
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
#include "task/builder/RestoreTask.h"
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
static constexpr int COMM_STATE_FRAMES = 4;  // the danger list, cover list and each commander's state are reused this long
static constexpr int COMM_DANGER_MEMORY = FRAMES_PER_SEC * 5;  // jobs and morph starts keep a danger this long after it left radar and LOS
static constexpr int BUILDER_SCAR_FRAMES = FRAMES_PER_SEC * 5;  // other builders all keep jobs off a spot one of them fled from this long
static constexpr int BUILDER_CORNER_TIMEOUT = FRAMES_PER_SEC * 30;  // an unstarted cornered defence nobody builds is dropped after this
static constexpr int BUILDER_IDLE_WAIT = FRAMES_PER_SEC * 3;  // other builders idle in a flee zone with no safe job look again after this

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
		, isPrioRepairSafeHeld(true)
		, isRepairHitBuilder(false)
		, isRepairHitTarget(false)
		, repairHitCooldown(0)
		, prioRepairLeash(0.f)
		, prioRepairCooldown(0)
		, repairFleeShare(0.f)
		, repairChaseFrames(0)
		, repairChaseClose(0.f)
		, escort({0.f, 0.f, 0.f, 0.f, 0.f, false, static_cast<int>(IBuilderTask::Priority::HIGH)})
{
	commDangerFrame = -1;
	commCoverFrame = -1;
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
		AddComm(unit);
		if (!unit->GetCircuitDef()->IsRoleComm()) {
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
	isPrioRepairSafeHeld = root["retreat"].get("repair_safe_held", true).asBool();
	const Json::Value& hitExit = root["retreat"]["repair_hit_exit"];
	isRepairHitBuilder = hitExit.get((unsigned)0, false).asBool();
	isRepairHitTarget = hitExit.get((unsigned)1, false).asBool();
	repairHitCooldown = hitExit.get((unsigned)2, 5.f).asFloat() * FRAMES_PER_SEC;
	const Json::Value& leash = root["retreat"]["repair_leash"];
	prioRepairLeash = leash.get((unsigned)0, 0.f).asFloat();
	prioRepairCooldown = leash.get((unsigned)1, 20.f).asFloat() * FRAMES_PER_SEC;
	const Json::Value& flee = root["retreat"]["repair_flee"];
	repairFleeShare = flee.get((unsigned)0, 0.f).asFloat();
	const Json::Value& chase = root["retreat"]["repair_chase"];
	repairChaseFrames = chase.get((unsigned)0, 0.f).asFloat() * FRAMES_PER_SEC;
	repairChaseClose = chase.get((unsigned)1, 64.f).asFloat();
	commThrMod = root["quota"]["thr_mod"].get("comm", 1.f).asFloat();
	const Json::Value& bflee = root["retreat"]["builder_flee"];
	builderFlee.margin = bflee.get((unsigned)0, -1.f).asFloat();
	builderFlee.allyThreat = bflee.get((unsigned)1, 0.f).asFloat();
	builderFlee.allyRadius = std::max(bflee.get((unsigned)2, 500.f).asFloat(), 0.f);
	isFleeLog = root["retreat"].get("flee_log", false).asBool();
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

	const Json::Value& rest = root["economy"]["restore"];
	restore.range = rest.get("range", 0.f).asFloat();
	restore.depth = std::max(rest.get("depth", 24.f).asFloat(), 1.f);
	restore.share = utils::clamp(rest.get("share", 0.12f).asFloat(), 0.01f, 1.f);
	restore.maxTasks = std::max(rest.get("jobs", 2).asInt(), 1);
	restore.maxBuilders = std::max(rest.get("builders", 2).asInt(), 1);
	restore.maxTries = std::max(rest.get("tries", 3).asInt(), 1);
	restore.cooldown = std::max(rest.get("cooldown", 30.f).asFloat(), 1.f) * FRAMES_PER_SEC;
	{
		const std::string prio = rest.get("priority", "high").asString();
		restore.priority = (prio == "low") ? IBuilderTask::Priority::LOW
				: (prio == "normal") ? IBuilderTask::Priority::NORMAL
				: IBuilderTask::Priority::HIGH;
		if ((prio != "low") && (prio != "normal") && (prio != "high")) {
			circuit->LOG("CONFIG economy.restore.priority: unknown value '%s', using 'high'", prio.c_str());
		}
	}

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
		if (restore.range > 0.f) {
			scheduler->RunTaskEvery(std::make_shared<CGameTask>(&CBuilderManager::UpdateRestore, this),
									FRAMES_PER_SEC * 2, offset + 6);
		}
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
		// Unit hit while builders work on it: they are in the fight too, call them off (retreat.repair_hit_exit)
		if (isRepairHitTarget && target->GetCircuitDef()->IsMobile()) {
			auto it = repairedUnits.find(target->GetId());
			if ((it != repairedUnits.end()) && (it->second->GetPriority() == IBuilderTask::Priority::NOW)
				&& !it->second->GetAssignees().empty())
			{
				OnPrioRepairLeash(target->GetId(), repairHitCooldown);
				AbortTask(it->second);
				return nullptr;
			}
		}
	}
	CBRepairTask* task;
	auto it = repairedUnits.find(target->GetId());
	if (it != repairedUnits.end()) {
		task = it->second;
		if (static_cast<int>(priority) > static_cast<int>(task->GetPriority())) {
			task->ChangePriority(priority);  // upgrade, e.g. HIGH left over from retreat
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
			// Other builders with the flee on (retreat.builder_flee): as their own job picks, not into a flee zone,
			// else each one pulled walks in and flees; and not off the defence they were cornered into
			const SCommState* state = worker->GetCircuitDef()->IsRoleComm() ? nullptr : GetCommState(worker);
			if (IsFinishingDefence(worker, state) || !IsCommJobSafe(state, pos, task)) {
				continue;
			}
			minSqDist = sqDist;
			bestWorker = worker;
		}
	}
	if (bestWorker != nullptr) {
		AssignTask(bestWorker, task);
	}
}

void CBuilderManager::OnPrioRepairLeash(ICoreUnit::Id unitId, int cooldown)
{
	leashUntil[unitId] = circuit->GetLastFrame() + ((cooldown < 0) ? prioRepairCooldown : cooldown);
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
	// retreat.repair_safe_held false: threat alone, our army nearby doesn't make a fight safe
	return (threatMap->GetThreatAt(pos) > threatMap->GetUnitThreat(unit))
		&& (!isPrioRepairSafeHeld || (circuit->GetInflMap()->GetInfluenceAt(pos) < -INFL_EPS));
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
	if (task->GetType() == IUnitTask::Type::BUILDER) {
		escalationTasks.erase(static_cast<IBuilderTask*>(task));
		escIdleSince.erase(static_cast<IBuilderTask*>(task));
		escStarted.erase(static_cast<IBuilderTask*>(task));
	}
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
	for (auto& kv : comms) {
		if (kv.second.cornerTask == task) {
			kv.second.cornerTask = nullptr;  // gone: never matched against a later task at the same address
		}
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

// The job adds or brings back firepower where it is: an armed build (the old safety check's buildThreat), or a repair
// of an armed unit or turret
static bool IsFightBackJob(const IBuilderTask* task)
{
	const CCircuitUnit* target = task->GetTarget();
	const CCircuitDef* def = (task->GetBuildType() == IBuilderTask::BuildType::REPAIR)
			? ((target != nullptr) ? target->GetCircuitDef() : nullptr)
			: task->GetBuildDef();
	return (def != nullptr) && (def->GetPower() >= THREAT_MIN);
}

IUnitTask* CBuilderManager::DefaultMakeTask(CCircuitUnit* unit)
{
	CThreatMap* threatMap = circuit->GetThreatMap();
	const int frame = circuit->GetLastFrame();
	const AIFloat3& pos = unit->GetPos(frame);

	const CCircuitDef* cdef = unit->GetCircuitDef();
	// Before the cost-map readiness check, as fights at home always were: GetCommState is cached per frame
	SCommState* commState = GetCommState(unit);
	if (IsFinishingDefence(unit, commState)) {
		return nullptr;  // keep at it: a job elsewhere mustn't take it off either (IBuilderTask::Reevaluate)
	}
	if (IsCommCornered(unit, commState)) {  // build cover rather than stand; with none to build it fights at home
		if (IsBuildingDefence(unit)) {
			return nullptr;  // keep at it
		}
		IBuilderTask* task = MakeCommDefence(unit, commState);
		if (task != nullptr) {
			return task;
		}
	}
	if (IsHomeFight(unit)) {
		return EnqueueCombat(1.5f);
	}

	const auto it = costQueries.find(unit);
	std::shared_ptr<IPathQuery> query = (it == costQueries.end()) ? nullptr : it->second;
	if ((query != nullptr) && (query->GetState() != IPathQuery::State::READY)) {  // not ready
		return nullptr;
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
		const CSetupManager::SCommInfo::SHide* hide = (commState != nullptr) ? commState->hide
				: circuit->GetSetupManager()->GetHide(cdef);
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

	IBuilderTask* task = MakeBuilderTask(unit, pQuery.get());
	// A constructor idle with no job while stalling (only reclaim is allowed, and there's none) or in a flee zone with
	// no safe job: look again in a while, not with a new cost map and feature scan every update. Not a commander: it
	// keeps asking, so a fight at home (IsHomeFight above) isn't delayed
	if ((task == nullptr) && (unit->GetTask() == idleTask)
		&& ((!cdef->IsRoleComm() && circuit->GetEconomyManager()->IsStalling())
			|| ((commState != nullptr) && !commState->IsComm() && !IsCommJobSafe(commState, pos))))
	{
		return EnqueueWait(BUILDER_IDLE_WAIT);
	}
	return task;
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

// While stalling (CEconomyManager::IsStalling): would unit add its build power to spending? Spending = a building
// (placed or not), terraform, or repairing an unfinished unit; reclaim and finished-unit repair are free (guard and
// patrol jobs are never candidates). Its own job never counts, nor a stall fix (IsStallFix): it runs at ZK's high
// build priority (CmdPriority), so it takes the scarce resource first and more builders on it do finish it sooner
static bool IsStallSpend(const CEconomyManager* economyMgr, const IBuilderTask* task, const CCircuitUnit* unit)
{
	if (task == unit->GetTask()) {
		return false;
	}
	const IBuilderTask::BuildType type = task->GetBuildType();
	const CCircuitUnit* target = task->GetTarget();
	const bool isSpend = (task->GetBuildDef() != nullptr) || (type == IBuilderTask::BuildType::TERRAFORM)
		|| ((type == IBuilderTask::BuildType::REPAIR) && (target != nullptr) && target->GetUnit()->IsBeingBuilt());
	return isSpend && !economyMgr->IsStallFix(task);
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
	// While metal or energy stalls, no new spending (urgent jobs included): more build power only spreads the stall
	const bool isStalling = economyMgr->IsStalling();
	// Never send the commander into a flee zone, measured at the line the flee stops at; nullptr = flee off.
	// Hide mode's territory test goes with the flee
	const SCommState* commState = GetCommState(unit);
	for (const std::set<IBuilderTask*>& tasks : buildTasks) {
		for (const IBuilderTask* candidate : tasks) {
			const bool isNow = (candidate->GetPriority() == IBuilderTask::Priority::NOW);
			if ((isNow && !circuit->GetMilitaryManager()->IsEscalationComm() && IsEscalationTask(candidate))  // hiding
				|| (isNow && isPrioRepairFull && (candidate->GetBuildType() == IBuilderTask::BuildType::REPAIR))
				|| !candidate->CanAssignTo(unit)
				|| (isNotReady && !isNow && (candidate->GetBuildDef() != nullptr))
				|| (isStalling && IsStallSpend(economyMgr, candidate, unit)))
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
			if (!IsCommJobSafe(commState, buildPos, candidate)) {
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
					|| ((commState != nullptr) && !IsCommTerritory(unit, buildPos))  // allied strength outweighs the enemy's there
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
		// Guarding the factory adds build power to it, and a patrol helps any frame in reach: while stalling, reclaim
		// nearby first and patrol only if there is none that is safe and inside the hide range (as the loop above)
		if (isStalling) {
			task = economyMgr->UpdateReclaimTasks(pos, unit, true);
			if ((task != nullptr) && (!IsCommJobSafe(commState, task->GetPosition())
				|| ((sqMaxBaseRange > 0.f) && (basePos.SqDistance2D(task->GetPosition()) > sqMaxBaseRange))))
			{
				task = nullptr;  // left queued for constructors
			}
		}
		if (task == nullptr) {
			if ((vip != nullptr) && !isStalling && IsCommJobSafe(commState, vip->GetPos(frame))) {
				task = EnqueueGuard(IBuilderTask::Priority::NORMAL, vip, FRAMES_PER_SEC * 60);
			} else {
				task = EnqueuePatrol(IBuilderTask::Priority::LOW, pos, .0f, FRAMES_PER_SEC * 5);
			}
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
	const bool isNotReady = !economyMgr->IsExcessed();  // rarely true with excess -1 (economy.json); stalls are below
	// While metal or energy stalls, no new spending (urgent jobs included): more build power only spreads the stall
	const bool isStalling = economyMgr->IsStalling();

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
	// Never send the commander into a flee zone, measured at the line the flee stops at; nullptr = flee off
	const SCommState* commState = GetCommState(unit);
	// Escalation jobs: up to escalation.builder_share of our build power (the rest keeps the economy going), no
	// commander unless escalation.comm. One already on such a job (hidden from its power now) may stay
	const bool isEscalationFree = escalationTasks.empty() || (unit->GetTask()->GetType() == IUnitTask::Type::BUILDER
			&& IsEscalationTask(static_cast<IBuilderTask*>(unit->GetTask())));
	const bool isEscalationOff = !isEscalationFree
			&& ((cdef->IsRoleComm() && !circuit->GetMilitaryManager()->IsEscalationComm()) || IsEscalationFull(unit, false));
	// Singus within singu_share of that: they sit closer to base workers and would fill the share first
	const bool isEscSinguOff = !isEscalationFree && !isEscalationOff && IsEscalationFull(unit, true);
	for (const std::set<IBuilderTask*>& tasks : buildTasks) {
		for (const IBuilderTask* candidate : tasks) {
			const bool isNow = (candidate->GetPriority() == IBuilderTask::Priority::NOW);
			// Escalation jobs are free of the stall rule and the priority repair cap: the factories are idle for them
			const bool isEscalation = isNow && IsEscalationTask(candidate);
			if (isEscalation && (isEscalationOff || (isEscSinguOff && IsEscalationSingu(candidate)))) {
				continue;
			}
			if ((isNow && !isEscalation && isPrioRepairFull && (candidate->GetBuildType() == IBuilderTask::BuildType::REPAIR))
				|| !candidate->CanAssignTo(unit)
				|| (isNotReady && !isNow
					&& (candidate->GetBuildDef() != nullptr)
					&& !economyMgr->IsIgnoreStallingPull(candidate))
				|| (isStalling && !isEscalation && IsStallSpend(economyMgr, candidate, unit)))
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
			if (!IsCommJobSafe(commState, buildPos, candidate)) {
				continue;
			}

			if (isNow) {
				// Disregard safety (unless configured for priority repair). Escalation jobs keep it, armed or not:
				// a superweapon frame doesn't defend its builders
				if (!terrainMgr->CanReachAt(unit, buildPos, cdef->GetBuildDistance())  // ensure that path always exists
					|| IsUnsafePrioRepair(candidate, unit, buildPos)
					|| (isEscalation && (threatMap->GetThreatAt(buildPos) > maxThreat)
						&& (inflMap->GetInfluenceAt(buildPos) < -INFL_EPS)))
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
	// choke.radar.enable: a builder between jobs builds a radar nearby first, unless a NOW job waits for it
	if ((unit->GetTask() == idleTask) && ((task == nullptr) || (task->GetPriority() != IBuilderTask::Priority::NOW))) {
		IBuilderTask* radar = circuit->GetMilitaryManager()->GetSensorPlanner()->MakeRadarTask(unit,
				[this, commState](const AIFloat3& spot) { return IsCommJobSafe(commState, spot); });
		if (radar != nullptr) {
			return radar;
		}
	}
	if (task == nullptr) {
		if ((unit->GetTask() != idleTask) || isNotReady) {
			return nullptr;  // current task is in danger or unreachable
		}
		if (!IsCommJobSafe(commState, pos)) {
			return nullptr;  // a builder with the flee on makes no job where it stands inside a flee zone
		}
		// Stalling: no new building from an idle constructor, only reclaim anywhere on the map
		task = isStalling ? economyMgr->UpdateReclaimTasks(pos, unit, false) : CreateBuilderTask(pos, unit);
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
		const std::set<IBuilderTask*>& tasks = GetTasks(IBuilderTask::BuildType::BIG_GUN);
		// A gun with no grid of enough energy in reach would stand unpowered: draw again among the ones that can be
		// powered, and keep that choice. A queued gun is never dropped for it
		if (tasks.empty() && (buildDef != nullptr) && !militaryMgr->IsBigGunPowerable(buildDef)) {
			militaryMgr->DiceBigGun();
			buildDef = militaryMgr->GetBigGunDef();
			if ((buildDef != nullptr) && !militaryMgr->IsBigGunPowerable(buildDef)) {
				buildDef = nullptr;
			}
		}
		if ((buildDef != nullptr) && (buildDef->GetCostM() < super.maxTime * metalIncome)) {
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

void CBuilderManager::PromoteEscalation(IBuilderTask* task)
{
	if (task->GetPriority() != IBuilderTask::Priority::NOW) {
		task->ChangePriority(IBuilderTask::Priority::NOW);
	}
	escalationTasks.insert(task);
}

IBuilderTask* CBuilderManager::AssistEscalationFrame(CAllyUnit* frame)
{
	// Ours with a job of ours on it: raise that job
	CCircuitUnit* own = circuit->GetTeamUnit(frame->GetId());
	if (own != nullptr) {
		auto it = unfinishedUnits.find(own);
		if (it != unfinishedUnits.end()) {
			PromoteEscalation(it->second);
			return it->second;
		}
	}
	// An ally's frame (or ours that no job builds): assist it. The repair job follows it by id, ally units are
	// re-made on every friendly update
	CBRepairTask* task;
	auto it = repairedUnits.find(frame->GetId());
	if (it != repairedUnits.end()) {
		task = it->second;
	} else {
		task = new CBRepairTask(this, IBuilderTask::Priority::NOW, frame);
		buildTasks[static_cast<IBuilderTask::BT>(IBuilderTask::BuildType::REPAIR)].insert(task);
		buildTasksCount++;
		buildUpdates.push_back(task);
		repairedUnits[frame->GetId()] = task;
	}
	PromoteEscalation(task);
	return task;
}

bool CBuilderManager::IsEscalationSingu(const IBuilderTask* task) const
{
	const CCircuitDef* singuDef = circuit->GetMilitaryManager()->GetEscalationSingu();
	return (singuDef != nullptr) && (task->GetBuildDef() != nullptr) && (task->GetBuildDef()->GetId() == singuDef->GetId());
}

bool CBuilderManager::IsEscalationFull(CCircuitUnit* unit, bool isSingu) const
{
	CMilitaryManager* militaryMgr = circuit->GetMilitaryManager();
	float share = militaryMgr->GetEscalationBuilderShare();
	if (isSingu) {
		share *= militaryMgr->GetEscalationSinguShare();
	} else if (circuit->GetEconomyManager()->IsMetalFull()) {
		// Metal overflowing: the rest of the builders can't spend it, more on the superweapon can (factories stay
		// paused). One that joined may stay on after it drains (isEscalationFree), so this doesn't flicker
		share = 1.f;
	}
	float power = 0.f;
	for (const IBuilderTask* task : escalationTasks) {
		if (!isSingu || IsEscalationSingu(task)) {
			power += task->GetBuildPower();
		}
	}
	// The first builder always may: a job nobody takes would only wait out its timeout
	return (power > 0.f) && (power + unit->GetBuildSpeed() > buildPower * share);
}

IBuilderTask* CBuilderManager::StartEscalation(int kind)
{
	CMilitaryManager* militaryMgr = circuit->GetMilitaryManager();
	if (kind == 0) {
		SEscProject& project = escProjects[kind];
		const bool isFrontOk = !project.isFrontStale;
		CCircuitDef* superDef = militaryMgr->DiceEscalationSuper(isFrontOk);
		if (superDef == nullptr) {
			return nullptr;
		}
		if (militaryMgr->IsEscalationFront(superDef)) {
			project.isFrontStale = false;  // retried once as the rest
		}
		return EnqueueTask(IBuilderTask::Priority::NOW, superDef, militaryMgr->GetEscalationSuperPos(superDef, isFrontOk),
						   IBuilderTask::BuildType::BIG_GUN);
	}
	CCircuitDef* singuDef = militaryMgr->GetEscalationSingu();
	return ((singuDef == nullptr) || !singuDef->IsAvailable(circuit->GetLastFrame())) ? nullptr
			: circuit->GetEconomyManager()->EnqueueEscalationEnergy(singuDef);
}

void CBuilderManager::UpdateEscalation(bool isOn)
{
	CAllyTeam* allyTeam = circuit->GetAllyTeam();
	const int aiId = circuit->GetSkirmishAIId();
	if (!isOn) {
		for (IBuilderTask* task : escalationTasks) {
			task->ChangePriority(IBuilderTask::Priority::NORMAL);
		}
		escalationTasks.clear();
		escIdleSince.clear();
		for (int kind = 0; kind < ESC_KINDS; ++kind) {
			allyTeam->ReleaseEscalation(kind, aiId);
		}
		isEscalationBusy = false;
		return;  // start timers kept: back on soon, a timer that ran out starts at once
	}

	CMilitaryManager* militaryMgr = circuit->GetMilitaryManager();
	CCircuitDef* singuDef = militaryMgr->GetEscalationSingu();
	const int frame = circuit->GetLastFrame();
	// Kind of an escalation def: 0 superweapon, 1 singu, -1 neither
	auto getKind = [militaryMgr, singuDef](const CCircuitDef* cdef) {
		if (cdef == nullptr) {
			return -1;
		}
		if (militaryMgr->IsEscalationSuper(cdef)) {
			return 0;
		}
		return ((singuDef != nullptr) && (cdef->GetId() == singuDef->GetId())) ? 1 : -1;
	};

	// Per kind: held = something of it exists (no new start); live = work is going on (factories pause, claim)
	const int staleFrames = militaryMgr->GetEscalationStaleFrames();
	std::array<bool, ESC_KINDS> isHeld = {false, false};
	std::array<bool, ESC_KINDS> isLive = {false, false};
	std::array<bool, ESC_KINDS> isOwnLive = {false, false};  // a job of ours not placed yet: the claim covers it

	// Allied (ours included) frames being built: raise our job on it, or assist it. Live while its progress went up in
	// the last escalation.stale s, or our builders are on it: ZK frames don't decay, an abandoned one (under fire,
	// out of reach, given up) would pause factories for good. Still assisted: builders go back once it's safe
	std::map<int, SEscFrame> frames;
	circuit->UpdateFriendlyUnits();
	for (const auto& kv : circuit->GetFriendlyUnits()) {
		CAllyUnit* au = kv.second;
		const int kind = getKind(au->GetCircuitDef());
		if ((kind < 0) || !au->GetUnit()->IsBeingBuilt()) {
			continue;
		}
		IBuilderTask* task = AssistEscalationFrame(au);
		const float progress = au->GetUnit()->GetBuildProgress();
		auto it = escFrames.find(au->GetId());
		SEscFrame info = (it != escFrames.end()) ? it->second : SEscFrame{progress, frame};  // new: live for a while
		if (progress > info.progress) {
			info = {progress, frame};
		}
		frames[au->GetId()] = info;
		const bool isAssisted = !task->GetAssignees().empty();
		if ((frame - info.riseFrame < staleFrames) || isAssisted) {
			isLive[kind] = true;
		}
		// Stuck this long, it no longer blocks a new start (behaviour.json limit still counts it)
		if ((frame - info.riseFrame < staleFrames * ESC_STUCK_STALES) || isAssisted) {
			isHeld[kind] = true;
			escProjects[kind].staleCount = 0;  // a frame stands: the spot works
		}
	}
	escFrames.swap(frames);  // frames finished or gone drop out

	// Our own jobs not placed yet, of either kind (the normal paths can queue them too); a placed one is its frame,
	// above. Live with builders, or for escalation.stale s: escalation jobs keep the threat check, so one in a
	// threatened spot (a front Bertha) would otherwise pause factories until its 300 s timeout. Past that, one
	// escalation started is dropped; one of the normal paths (they often wait for an idle builder) stays, and only
	// blocks a new start until its own timeout
	std::vector<std::pair<IBuilderTask*, int>> staleJobs;
	for (IBuilderTask::BuildType type : {IBuilderTask::BuildType::BIG_GUN, IBuilderTask::BuildType::ENERGY}) {
		for (IBuilderTask* task : GetTasks(type)) {
			const int kind = getKind(task->GetBuildDef());
			if (kind < 0) {
				continue;
			}
			if (task->GetTarget() != nullptr) {
				escIdleSince.erase(task);  // placed: its frame tells (above)
				PromoteEscalation(task);
				continue;
			}
			bool isJobLive = true;
			if (task->GetAssignees().empty()) {
				auto it = escIdleSince.emplace(task, frame).first;
				if (frame - it->second >= staleFrames) {
					if (escStarted.find(task) != escStarted.end()) {
						staleJobs.push_back({task, kind});
						continue;
					}
					isJobLive = false;
				}
			} else {
				escIdleSince.erase(task);
			}
			PromoteEscalation(task);
			isHeld[kind] = true;
			if (isJobLive) {
				isLive[kind] = isOwnLive[kind] = true;
			}
		}
	}
	for (const auto& job : staleJobs) {
		circuit->LOG("ESCALATION: drop %s, no builder took it", job.first->GetBuildDef()->GetDef()->GetName());
		if ((job.second == 0) && militaryMgr->IsEscalationFront(job.first->GetBuildDef())) {
			escProjects[0].isFrontStale = true;
		}
		++escProjects[job.second].staleCount;
		AbortTask(job.first);  // DequeueTask forgets it in escalationTasks, escIdleSince and escStarted
	}

	isEscalationBusy = false;
	for (int kind = 0; kind < ESC_KINDS; ++kind) {
		SEscProject& project = escProjects[kind];
		// The claim tells allied AIs we hold a live job before its frame shows (a builder may walk a minute to the site)
		if (isOwnLive[kind]) {
			allyTeam->ClaimEscalation(kind, frame, frame + ESC_CLAIM_FRAMES, aiId);
		} else {
			allyTeam->ReleaseEscalation(kind, aiId);
		}
		const bool isClaimed = allyTeam->IsEscalationClaimed(kind, frame, aiId);
		if ((kind == 0) && (isLive[kind] || isClaimed)) {
			isEscalationBusy = true;
		}
		if (isHeld[kind] || isClaimed) {
			project.startFrame = -1;
			project.isLogged = false;
			continue;
		}
		// None under way: start our own after a random wait, longer after jobs that went stale
		if (project.startFrame < 0) {
			const int delay = militaryMgr->RollEscalationDelay() << std::min(project.staleCount, 3);
			project.startFrame = frame + std::min(delay, FRAMES_PER_SEC * 120);
			continue;
		}
		if (frame < project.startFrame) {
			continue;
		}
		IBuilderTask* task = StartEscalation(kind);
		if (task != nullptr) {
			if (kind == 0) {
				isEscalationBusy = true;
			}
			escStarted.insert(task);
			PromoteEscalation(task);
			allyTeam->ClaimEscalation(kind, frame, frame + ESC_CLAIM_FRAMES, aiId);
			circuit->LOG("ESCALATION: start %s", task->GetBuildDef()->GetDef()->GetName());
			project.startFrame = -1;
			project.isLogged = false;
		} else if (!project.isLogged) {
			circuit->LOG("ESCALATION: no %s to start (limits, or no site or grid)", (kind == 0) ? "superweapon" : "singu");
			project.isLogged = true;
		}
	}
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
	return (state == nullptr) || !state->IsExposed(state->wideZone, COMM_END_COVER_MOD) || !HasCommEscape(unit, state);
}

bool CBuilderManager::IsCommInDanger(CCircuitUnit* unit)
{
	// Morph start: no uncovered danger (recently seen ones included) within the wide line
	SCommState* state = GetCommState(unit);
	return (state != nullptr) && state->IsExposed(state->allZone, 1.f);
}

void CBuilderManager::AddComm(CCircuitUnit* unit)
{
	// Only builders with the flee on get a state, with their settings looked up once (GetHide reads the def name):
	// a commander's from commander.json hide, other builders' from retreat.builder_flee and builder_flee_margin.
	// Flying builders get none: the dangers and flee spots are for ground units, and they dodge by flying
	CCircuitDef* cdef = unit->GetCircuitDef();
	SCommState state;
	if (cdef->IsRoleComm()) {
		state.hide = circuit->GetSetupManager()->GetHide(cdef);
		if (state.hide == nullptr) {
			return;
		}
		state.cfg = {state.hide->fleeMargin, state.hide->fleeAllyThreat, state.hide->fleeAllyRadius};
	} else {
		if (cdef->IsAbleToFly()) {
			return;
		}
		state.cfg = builderFlee;
		state.cfg.margin = cdef->GetBuilderFleeMargin(builderFlee.margin);
	}
	if (state.cfg.margin < 0.f) {
		return;
	}
	state.id = unit->GetId();
	state.wideMargin = GetCommWideMargin(state.cfg.margin);
	comms[unit] = state;
}

void CBuilderManager::UpdateCommDangers()
{
	// As ISquadTask::FindFleeDanger: enemies that can act and hit ground or water units (IFighterTask::IsGroundDanger,
	// gunships included), unidentified blips while they move and sit low. Seen: in radar or LOS now; the flee only
	// counts those (the threat map keeps last-known ghosts, which would hold it back for long). Jobs and morph starts
	// also count known enemies gone from radar and LOS within COMM_DANGER_MEMORY, at their last spot, so an enemy at
	// the edge of sight doesn't pull the commander to a job and back. Rebuilt every COMM_STATE_FRAMES
	const int frame = circuit->GetLastFrame();
	if ((commDangerFrame >= 0) && (frame - commDangerFrame < COMM_STATE_FRAMES)) {
		return;
	}
	commDangerFrame = frame;
	commDangers.clear();
	fleeScars.erase(std::remove_if(fleeScars.begin(), fleeScars.end(),
			[frame](const SFleeScar& scar) { return scar.endFrame <= frame; }), fleeScars.end());
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

bool CBuilderManager::IsCommSafe(const AIFloat3& pos, float margin)
{
	UpdateCommDangers();
	for (const SCommDanger& danger : commDangers) {
		if (danger.isSeen && (pos.SqDistance2D(danger.pos) <= SQUARE(danger.range + margin))) {
			return false;
		}
	}
	return true;
}

void CBuilderManager::UpdateCommCovers()
{
	// Mobile armed units that can hit ground or water (not aircraft) and static defences, as ISquadTask::IsFleeCovered,
	// with their cover power, so a cover sum is a plain scan. The list is the one the influence map last refreshed
	// (every few frames): no forced refresh here, which would re-create every allied unit's entry. Rebuilt on demand,
	// at most every COMM_STATE_FRAMES
	const int frame = circuit->GetLastFrame();
	if ((commCoverFrame >= 0) && (frame - commCoverFrame < COMM_STATE_FRAMES)) {
		return;
	}
	SCOPED_TIME(circuit, __PRETTY_FUNCTION__);
	commCoverFrame = frame;
	commCovers.clear();
	for (auto& kv : circuit->GetFriendlyUnits()) {
		CAllyUnit* ally = kv.second;
		CCircuitDef* adef = ally->GetCircuitDef();
		if ((adef == nullptr) || !adef->IsAttacker() || adef->IsAbleToFly()
			|| (adef->IsMobile() ? !(adef->HasAntiLand() || adef->HasAntiWater()) : !IFighterTask::IsStaticDefence(adef)))
		{
			continue;
		}
		const AIFloat3& allyPos = ally->GetPos(frame);
		if (!utils::is_valid(allyPos)) {
			continue;
		}
		const float power = IFighterTask::GetCoverPower(ally);  // 0 for a nanoframe or a unit that died since
		if (power > 0.f) {
			commCovers.push_back({ally->GetId(), allyPos, power});
		}
	}
}

float CBuilderManager::GetCommCover(const AIFloat3& pos, float radius, ICoreUnit::Id skipId, float enough)
{
	UpdateCommCovers();
	const float sqRadius = SQUARE(radius);
	float cover = 0.f;
	for (const SCommCover& ally : commCovers) {
		if ((ally.id != skipId) && (ally.pos.SqDistance2D(pos) <= sqRadius)) {
			cover += ally.power;
			if (cover >= enough) {
				break;
			}
		}
	}
	return cover;
}

CBuilderManager::SCommState* CBuilderManager::GetCommState(CCircuitUnit* unit)
{
	// Every flee, fight, job and morph decision reads the same numbers, refreshed every COMM_STATE_FRAMES
	auto it = comms.find(unit);
	if (it == comms.end()) {
		return nullptr;  // a flying builder, or its flee is off
	}
	SCommState& state = it->second;
	const int frame = circuit->GetLastFrame();
	if ((state.frame >= 0) && (frame - state.frame < COMM_STATE_FRAMES)) {
		return &state;
	}
	state.frame = frame;
	state.nearZone = state.wideZone = state.allZone = SCommZone();
	state.cover = 0.f;  // dangerPos and isAir stay from the last refresh that saw a danger (see SCommState)
	state.own = IFighterTask::GetCoverPower(unit);
	const float margin = state.cfg.margin;
	const AIFloat3& pos = unit->GetPos(frame);
	UpdateCommDangers();
	float minSqDist = std::numeric_limits<float>::max();
	for (const SCommDanger& danger : commDangers) {
		const float sqDist = pos.SqDistance2D(danger.pos);
		if (sqDist > SQUARE(danger.range + state.wideMargin)) {
			continue;
		}
		state.allZone.Add(danger.threat);
		if (!danger.isSeen) {
			continue;
		}
		state.wideZone.Add(danger.threat);
		if (sqDist <= SQUARE(danger.range + margin)) {
			state.nearZone.Add(danger.threat);
		}
		if (sqDist < minSqDist) {
			minSqDist = sqDist;
			state.dangerPos = danger.pos;
			state.dangerRange = danger.range;
			state.isAir = danger.isAir;
		}
	}
	if ((state.allZone.count > 0) && (state.cfg.allyThreat >= 0.f)) {
		// The builder itself included. Enough once it passes the highest bar any check sets
		const float enough = state.allZone.threat * COMM_END_COVER_MOD + state.cfg.allyThreat;
		state.cover = GetCommCover(pos, state.cfg.allyRadius, -1, enough);
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

bool CBuilderManager::IsCommCornered(CCircuitUnit* unit, SCommState* state)
{
	return (state != nullptr) && state->IsExposed(state->wideZone, 1.f) && !HasCommEscape(unit, state);
}

bool CBuilderManager::IsFinishingDefence(CCircuitUnit* unit, const SCommState* state) const
{
	return (state != nullptr) && (state->cornerTask != nullptr) && (unit->GetTask() == state->cornerTask)
		&& !IsMetalStalling();
}

bool CBuilderManager::IsMetalStalling() const
{
	// Low metal (IsMetalEmpty: under 20% of storage) alone is the usual state of an economy that spends its income
	CEconomyManager* economyMgr = circuit->GetEconomyManager();
	return economyMgr->IsMetalEmpty()
		&& (economyMgr->GetAvgMetalIncome() * 1.2f < economyMgr->GetMetalPull())
		&& (metalPull > economyMgr->GetPullMtoS() * circuit->GetFactoryManager()->GetMetalPull());
}

bool CBuilderManager::IsCommJobSafe(const SCommState* state, const AIFloat3& pos, const IBuilderTask* task)
{
	// Any job for a builder with the flee off (no state). Else outside every flee zone at the wide line (recently
	// seen dangers included), or covered there as a flee would end: allies within flee_ally_radius of the job plus
	// the builder itself reach 1.25x the threat that reaches it.
	// Other builders: a fight-back job is measured at the start line instead, as the old builder safety check let
	// armed builds into threat (the turret or the repaired unit is what makes the spot safe); and a danger any of
	// them fled from in the last BUILDER_SCAR_FRAMES still counts at its spot (fleeScars), at the wide line for
	// every job (so fight-back jobs get the start/stop gap back there), as threatening as then, even unseen
	if (state == nullptr) {
		return true;
	}
	const bool isBuilder = !state->IsComm();
	const bool isFightBack = isBuilder && (task != nullptr) && IsFightBackJob(task);
	const float line = isFightBack ? state->cfg.margin : state->wideMargin;
	UpdateCommDangers();
	SCommZone zone;
	for (const SCommDanger& danger : commDangers) {
		if (pos.SqDistance2D(danger.pos) <= SQUARE(danger.range + line)) {
			zone.Add(danger.threat);
		}
	}
	if (isBuilder) {
		for (const SFleeScar& scar : fleeScars) {
			if (pos.SqDistance2D(scar.pos) <= SQUARE(scar.range + state->wideMargin)) {
				++zone.count;
				zone.threat = std::max(zone.threat, scar.threat);  // not added: its danger is often still in the list
			}
		}
	}
	if (zone.count == 0) {
		return true;
	}
	if (state->cfg.allyThreat < 0.f) {
		return false;
	}
	const float need = zone.threat * COMM_END_COVER_MOD + state->cfg.allyThreat;
	return (state->own >= need)
		|| (state->own + GetCommCover(pos, state->cfg.allyRadius, state->id, need - state->own) >= need);
}

IBuilderTask* CBuilderManager::MakeCommDefence(CCircuitUnit* unit, SCommState* state)
{
	// The defence adds the cover (GetCommCover counts finished static defences) that lets the builder stay.
	// A commander shoots while it builds. One at a time: an unfinished one nearby is helped rather than a new one
	// queued. None while metal is short (a commander: IsMetalEmpty; other builders: IsMetalStalling): it would
	// stand nanolathing a frame that never finishes.
	// Other builders also help one started in the danger's flee zone on their side of it (no nearer the danger than
	// they are, and nearer them than the danger), only help or build one that can hit the danger and outranges it
	// from behind them (else none), and keep it as cornerTask, to finish it (StartCommFlee)
	const bool isBuilder = !state->IsComm();
	if (IsBuildingDefence(unit) || (isBuilder ? IsMetalStalling() : circuit->GetEconomyManager()->IsMetalEmpty())) {
		return nullptr;
	}
	const int frame = circuit->GetLastFrame();
	const AIFloat3& pos = unit->GetPos(frame);
	CCircuitDef* cdef = unit->GetCircuitDef();
	const float buildDist = cdef->GetBuildDistance();
	const AIFloat3& dangerPos = state->dangerPos;
	const bool isAir = state->isAir;
	const bool isWater = circuit->GetMap()->GetElevationAt(pos.x, pos.z) < -SQUARE_SIZE * 5;  // MakeDefence's test
	const CCircuitDef::RangeType rangeType = isAir ? CCircuitDef::RangeType::AIR
			: (isWater ? CCircuitDef::RangeType::WATER : CCircuitDef::RangeType::LAND);
	const float minRange = isBuilder ? (state->dangerRange + buildDist * 0.5f) : 0.f;  // placed that far back
	// Shoots the danger: against gunships one that hits air
	auto canHit = [isAir, isWater, rangeType, minRange](const CCircuitDef* ddef) {
		return (ddef != nullptr) && ddef->IsAttacker()
			&& (isAir ? ddef->HasAntiAir() : (isWater ? ddef->HasAntiWater() : ddef->HasAntiLand()))
			&& (ddef->GetMaxRange(rangeType) >= minRange);
	};
	const float sqDangerDist = pos.SqDistance2D(dangerPos);
	const float sqReach = SQUARE(state->dangerRange + state->cfg.margin);

	IBuilderTask* task = nullptr;
	for (IBuilderTask* t : GetTasks(IBuilderTask::BuildType::DEFENCE)) {
		const float sqTaskDist = t->GetTaskPos().SqDistance2D(pos);
		const float sqTaskDanger = t->GetTaskPos().SqDistance2D(dangerPos);
		const bool isNear = sqTaskDist < SQUARE(buildDist + DEFAULT_SLACK * 2);
		const bool isOnSide = isBuilder && (sqTaskDanger <= sqReach) && (sqTaskDanger >= sqDangerDist)
				&& (sqTaskDist < sqDangerDist);
		if ((isNear || isOnSide) && (!isBuilder || canHit(t->GetBuildDef())) && t->CanAssignTo(unit)) {
			task = t;
			break;
		}
	}

	if (task == nullptr) {
		// The first one porcupine.land/water lists (its build order) that can hit the danger
		CMilitaryManager* militaryMgr = circuit->GetMilitaryManager();
		CCircuitDef* buildDef = nullptr;
		for (CCircuitDef* ddef : isWater ? militaryMgr->GetWaterDefenders() : militaryMgr->GetLandDefenders()) {
			if (ddef->IsAvailable(frame) && cdef->CanBuild(ddef) && canHit(ddef)) {
				buildDef = ddef;
				break;
			}
		}
		if (buildDef == nullptr) {
			return nullptr;
		}

		// Behind the builder, away from the danger, within its build range
		AIFloat3 buildPos = pos;
		AIFloat3 away = pos - dangerPos;
		away.y = 0.f;
		const float len = away.Length2D();
		if (len > 1.f) {
			buildPos += away / len * (buildDist * 0.5f);
		}
		CTerrainManager::CorrectPosition(buildPos);
		// Other builders: an unstarted one is dropped soon after the last builder leaves it, not left as a job
		const int timeout = isBuilder ? BUILDER_CORNER_TIMEOUT : ASSIGN_TIMEOUT;
		task = EnqueueTask(IBuilderTask::Priority::HIGH, buildDef, buildPos, IBuilderTask::BuildType::DEFENCE,
						   buildDef->GetCostM(), SQUARE_SIZE * 8.f, true, timeout);
	}
	if (isBuilder) {
		state->cornerTask = task;
	}
	return task;
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
	// Other builders skip the groups: one heads on to the front, and the builder would follow it there.
	// false: no such spot, or only one at its feet (cornered)
	const int frame = circuit->GetLastFrame();
	const AIFloat3& pos = unit->GetPos(frame);
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	STerrainMapArea* area = unit->GetArea();
	commFleeSpots.clear();
	if (!isAir) {
		CMilitaryManager* militaryMgr = circuit->GetMilitaryManager();
		if (unit->GetCircuitDef()->IsRoleComm()) {
			militaryMgr->FillSafePos(unit, commFleeSpots);
		} else {
			militaryMgr->FillStaticSafePos(unit, commFleeSpots);
		}
	}
	CCircuitUnit* factory = circuit->GetFactoryManager()->GetClosestFactory(pos);
	if (factory != nullptr) {
		commFleeSpots.push_back(factory->GetPos(frame));
	}
	commFleeSpots.push_back(circuit->GetSetupManager()->GetBasePos());
	auto it = std::remove_if(commFleeSpots.begin(), commFleeSpots.end(), [&](const AIFloat3& spot) {
		return !terrainMgr->CanMoveToPos(area, spot) || !IsCommSafe(spot, margin);
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
	// Flee (always on): a seen ground danger within its weapon range + flee_margin and the builder with the allies
	// around can't take it (flee_ally_threat). A health retreat wins
	SCOPED_TIME(circuit, __PRETTY_FUNCTION__);
	for (auto& kv : comms) {
		CCircuitUnit* unit = kv.first;
		IUnitTask* task = unit->GetTask();
		if (task == nullptr) {
			continue;
		}
		const IUnitTask::Type type = task->GetType();
		if ((type == IUnitTask::Type::PLAYER) || (type == IUnitTask::Type::NIL)) {
			continue;
		}
		if (type != IUnitTask::Type::RETREAT) {
			StartCommFlee(unit, GetCommState(unit));
		} else if (static_cast<CRetreatTask*>(task)->IsFlee()) {
			ContinueCommFlee(unit, static_cast<CRetreatTask*>(task), GetCommState(unit));
		}  // else a health retreat
	}
}

void CBuilderManager::StartCommFlee(CCircuitUnit* unit, SCommState* state)
{
	// Flee once an uncovered seen danger reaches it. Cornered (nowhere better to go): it builds a static defence,
	// else stays at it (IsHomeFight lets it fight at home).
	// Other builders finish the defence they were cornered into rather than leave it half-built when the danger
	// moves and opens an escape: only a health retreat, or metal running out, takes them off it
	if (!state->IsExposed(state->nearZone, 1.f)) {
		return;
	}
	if (IsFinishingDefence(unit, state)) {
		return;
	}
	if (!HasCommEscape(unit, state)) {
		IBuilderTask* defence = MakeCommDefence(unit, state);
		if (defence != nullptr) {
			AssignTask(unit, defence);
		}
		return;
	}
	// A commander upgrade is a combat morph in Zero-K: it goes on while the commander moves, so it isn't stopped
	// (stopping refunds only 2/3 of the metal spent)
	const int frame = circuit->GetLastFrame();
	CRetreatTask* flee = EnqueueRetreat();
	flee->SetFlee(state->escapeGoal, frame);
	AssignTask(unit, flee);
	if (!state->IsComm()) {
		fleeScars.push_back({state->dangerPos, state->dangerRange, state->nearZone.threat, frame + BUILDER_SCAR_FRAMES});
	}
	LogFlee(unit, "start", state);
}

void CBuilderManager::ContinueCommFlee(CCircuitUnit* unit, CRetreatTask* retreat, SCommState* state)
{
	// Keeps going for COMM_FLEE_MIN_FRAMES at least, till nothing seen within COMM_DANGER_MEMORY is within the wide
	// line or it is covered 1.25x at the flee goal. Hurt on the way: a plain retreat for repair from there
	if (unit->GetHealthPercent() < unit->GetCircuitDef()->GetRetreat()) {
		retreat->StopFlee(unit);
		LogFlee(unit, "end: hurt", state);
		return;
	}
	if (circuit->GetLastFrame() < retreat->GetFleeFrame() + COMM_FLEE_MIN_FRAMES) {
		return;
	}
	// Ends on recently seen dangers too, as jobs: an enemy stepping out of sight doesn't end it mid-way
	if ((state->allZone.count == 0) || (retreat->IsArrived() && state->IsCovered(state->allZone.threat, COMM_END_COVER_MOD))) {
		retreat->EndFlee(unit);  // back to work: jobs stay out of uncovered flee zones
		LogFlee(unit, (state->allZone.count == 0) ? "end: clear" : "end: covered", state);
		return;
	}
	const AIFloat3& goal = retreat->GetFleeGoal();
	if (IsCommSafe(goal, state->wideMargin)) {
		return;
	}
	if (!HasCommEscape(unit, state)) {
		if (retreat->IsArrived()) {
			retreat->EndFlee(unit);  // nowhere better to go (e.g. a turret reaches every spot): build or fight here
			LogFlee(unit, "end: cornered", state);
		}
	} else if (state->escapeGoal.SqDistance2D(goal) > SQUARE(DEFAULT_SLACK)) {
		retreat->Redirect(unit, state->escapeGoal);
	}
}

void CBuilderManager::LogFlee(CCircuitUnit* unit, const char* event, const SCommState* state) const
{
	// retreat.flee_log: one line per flee start and end, to count flees and quick restarts in test games.
	// threat: of the dangers within the wide line, recently seen ones included
	if (!isFleeLog) {
		return;
	}
	circuit->LOG("FLEE %s | %s | id: %i | frame: %i | threat: %.1f | cover: %.1f", event,
				 unit->GetCircuitDef()->GetDef()->GetName(), unit->GetId(), circuit->GetLastFrame(),
				 state->allZone.threat, state->cover);
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

int CBuilderManager::SRestore::CellOf(const AIFloat3& pos) const
{
	const float cellSize = CELL * SQUARE_SIZE;
	const int x = utils::clamp(int(pos.x / cellSize), 0, cellsX - 1);
	const int z = utils::clamp(int(pos.z / cellSize), 0, cellsZ - 1);
	return z * cellsX + x;
}

AIFloat3 CBuilderManager::SRestore::CellPos(int cell, float offset) const
{
	const float cellSize = CELL * SQUARE_SIZE;
	// ZK keeps terraform points inside the map: the max corner stays a terraform square (16 elmos) off the edge
	return AIFloat3(std::min((cell % cellsX + offset) * cellSize, CTerrainManager::GetTerrainWidth() - 2.f * SQUARE_SIZE), 0.f,
					std::min((cell / cellsX + offset) * cellSize, CTerrainManager::GetTerrainHeight() - 2.f * SQUARE_SIZE));
}

void CBuilderManager::ScanRestore()
{
	SCOPED_TIME(circuit, __PRETTY_FUNCTION__);
	const FloatVec& start = circuit->GetTerrainManager()->GetStartHeightMap();
	const FloatVec& now = restore.scanned->heightMap;
	const int width = circuit->GetMap()->GetWidth();
	if ((width <= 0) || now.empty() || (start.size() != now.size())) {
		return;
	}
	const int height = now.size() / width;
	if (restore.grid.empty()) {
		restore.cellsX = (width + SRestore::CELL - 1) / SRestore::CELL;
		restore.cellsZ = (height + SRestore::CELL - 1) / SRestore::CELL;
		restore.grid.resize(restore.cellsX * restore.cellsZ);
	}
	const int frame = circuit->GetLastFrame();
	restore.dugCount = 0;
	for (int cz = 0; cz < restore.cellsZ; ++cz) {
		const int zEnd = std::min((cz + 1) * SRestore::CELL, height);
		for (int cx = 0; cx < restore.cellsX; ++cx) {
			const int xEnd = std::min((cx + 1) * SRestore::CELL, width);
			int samples = 0;
			int off = 0;
			for (int z = cz * SRestore::CELL; z < zEnd; z += 2) {  // every other square: 64 samples a cell
				for (int x = cx * SRestore::CELL, i = z * width + x; x < xEnd; x += 2, i += 2) {
					++samples;
					if (std::fabs(now[i] - start[i]) >= restore.depth) {
						++off;
					}
				}
			}
			SRestore::SCell& cell = restore.grid[cz * restore.cellsX + cx];
			cell.isDug = (off > 0) && (off >= restore.share * samples);
			if (cell.isDug) {
				++restore.dugCount;
			} else if (cell.until < frame) {
				cell.tries = 0;  // back at the start height and not held: a new dig there gets new tries
				cell.backoff = 0;
			}
		}
	}
}

void CBuilderManager::UpdateRestore()
{
	SCOPED_TIME(circuit, __PRETTY_FUNCTION__);
	const SAreaData* areaData = circuit->GetTerrainManager()->GetAreaData();
	if (restore.scanned != areaData) {  // the heightmap is fetched again every 10 s
		restore.scanned = areaData;
		ScanRestore();
	}
	std::vector<SRestore::SCell>& grid = restore.grid;
	if (grid.empty()) {
		return;
	}
	const int frame = circuit->GetLastFrame();

	// An ally's terraform (its terraunit; ours are our restores) gives its cell up while it stays dug: no tug-of-war
	// over its walls and ramps. An allied building skips its cell (its levelled pad is off the start height too):
	// looked up only while some cell is dug
	const CCircuitDef::Id terraId = terraDef->GetId();
	circuit->UpdateFriendlyUnits();
	for (const auto& kv : circuit->GetFriendlyUnits()) {
		CAllyUnit* au = kv.second;
		const CCircuitDef* cdef = au->GetCircuitDef();
		if (cdef == nullptr) {
			continue;
		}
		if (cdef->GetId() == terraId) {
			if (circuit->GetTeamUnit(au->GetId()) == nullptr) {
				SRestore::SCell& cell = grid[restore.CellOf(au->GetPos(frame))];
				cell.tries = restore.maxTries;
				cell.until = std::max(cell.until, frame + restore.cooldown);  // over the heightmap's lag once it's done
			}
		} else if (!cdef->IsMobile() && (restore.dugCount > 0)) {
			grid[restore.CellOf(au->GetPos(frame))].buildingFrame = frame;
		}
	}
	// A known enemy on or next to a cell: ZK makes terraform 20x slower near enemies, and it may be digging there
	for (const auto& kv : circuit->GetEnemyManager()->GetEnemyUnits()) {
		const CEnemyUnit* enemy = kv.second;
		if (!enemy->IsInRadarOrLOS()) {
			continue;
		}
		const int c = restore.CellOf(enemy->GetPos());
		const int cx = c % restore.cellsX;
		const int cz = c / restore.cellsX;
		for (int z = std::max(cz - 1, 0); z <= std::min(cz + 1, restore.cellsZ - 1); ++z) {
			for (int x = std::max(cx - 1, 0); x <= std::min(cx + 1, restore.cellsX - 1); ++x) {
				grid[z * restore.cellsX + x].enemyFrame = frame;
			}
		}
	}
	CThreatMap* threatMap = circuit->GetThreatMap();
	auto isUnsafe = [this, &grid, frame, threatMap](int c) {
		return (grid[c].enemyFrame == frame) || (threatMap->GetAllThreatAt(restore.CellPos(c, 0.5f)) > THREAT_MIN);
	};

	// One pass over the cells. A job is kept while a builder is on it and its cell is safe. Otherwise it is dropped:
	// one no builder took is chosen again right here, by where the builders are now; one whose cell turned unsafe
	// stops its builders and its cell backs off (OnRestoreEnd). Then open cells by their nearest builder, within
	// restore.range. While stalling no constructor takes a terraform job (IsStallSpend): don't make jobs only to drop them
	const bool isStalling = circuit->GetEconomyManager()->IsStalling();
	const float sqRange = SQUARE(restore.range);
	int jobCount = 0;
	std::vector<std::pair<float, int>> candidates;  // sqDist to the nearest builder, cell
	for (int c = 0; c < (int)grid.size(); ++c) {
		SRestore::SCell& cell = grid[c];
		if (cell.task != nullptr) {
			if (!cell.task->GetAssignees().empty() && !isUnsafe(c)) {
				++jobCount;
				continue;
			}
			AbortTask(cell.task);  // OnRestoreEnd clears the cell's task and sets its hold; the grid stays as it is
		}
		if (!cell.isDug || isStalling || (cell.until >= frame) || (cell.tries >= restore.maxTries)
			|| (cell.buildingFrame == frame) || isUnsafe(c))
		{
			continue;
		}
		const AIFloat3 center = restore.CellPos(c, 0.5f);
		float minSqDist = std::numeric_limits<float>::max();
		for (CCircuitUnit* worker : workers) {
			minSqDist = std::min(minSqDist, worker->GetPos(frame).SqDistance2D(center));
		}
		if (minSqDist <= sqRange) {
			candidates.emplace_back(minSqDist, c);
		}
	}
	const int count = utils::clamp<int>(restore.maxTasks - jobCount, 0, candidates.size());
	std::partial_sort(candidates.begin(), candidates.begin() + count, candidates.end());
	for (int i = 0; i < count; ++i) {
		const int c = candidates[i].second;
		CBRestoreTask* task = new CBRestoreTask(this, restore.priority, c);
		buildTasks[static_cast<IBuilderTask::BT>(IBuilderTask::BuildType::TERRAFORM)].insert(task);
		buildTasksCount++;
		buildUpdates.push_back(task);
		grid[c].task = task;
	}
}

void CBuilderManager::OnRestoreEnd(CBRestoreTask* task, bool done)
{
	const int c = task->GetCell();
	if ((c >= (int)restore.grid.size()) || (restore.grid[c].task != task)) {
		return;
	}
	SRestore::SCell& cell = restore.grid[c];
	cell.task = nullptr;
	if (!task->IsAssigned()) {
		return;  // no builder took it: it's chosen again on the next update
	}
	// A builder was on it: wait for the heightmap (10 s). A drop is danger or failure (enemy near, threat on the way,
	// a builder hit or killed, unreachable): back off, 1x, 2x, 4x, 8x cooldown, until a job finishes
	if (done) {
		++cell.tries;  // still dug after <tries> finished jobs: something (a building, the enemy) keeps it so
		cell.backoff = 0;
	} else {
		cell.backoff = (cell.backoff > 0) ? std::min(cell.backoff * 2, restore.cooldown * 8) : restore.cooldown;
	}
	cell.until = std::max(cell.until, circuit->GetLastFrame() + (done ? restore.cooldown : cell.backoff));
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

/*
 * MilitaryManager.cpp
 *
 *  Created on: Sep 5, 2014
 *      Author: rlcevg
 */

#include "module/MilitaryManager.h"
#include "module/BuilderManager.h"
#include "module/EconomyManager.h"
#include "map/InfluenceMap.h"
#include "map/ThreatMap.h"
#include "map/MapManager.h"
#include "resource/MetalManager.h"
#include "setup/SetupManager.h"
#include "setup/DefenceMatrix.h"
#include "task/NilTask.h"
#include "task/IdleTask.h"
#include "task/RetreatTask.h"
#include "task/builder/DefenceTask.h"
#include "task/fighter/RallyTask.h"
#include "task/fighter/GuardTask.h"
#include "task/fighter/DefendTask.h"
#include "task/fighter/ScoutTask.h"
#include "task/fighter/RaidTask.h"
#include "task/fighter/AttackTask.h"
#include "task/fighter/BombTask.h"
#include "task/fighter/ArtilleryTask.h"
#include "task/fighter/AntiAirTask.h"
#include "task/fighter/AntiHeavyTask.h"
#include "task/fighter/SupportTask.h"
#include "task/static/SuperTask.h"
#include "terrain/TerrainManager.h"
#include "terrain/path/PathFinder.h"
#include "terrain/path/QueryPathMulti.h"
#include "terrain/path/QueryCostMap.h"
#include "unit/ally/AllyUnit.h"
#include "unit/enemy/EnemyUnit.h"
#include "unit/enemy/EnemyManager.h"
#include "CircuitAI.h"
#include "util/Scheduler.h"
#include "util/Utils.h"
#include "json/json.h"

#include "spring/SpringCallback.h"
#include "spring/SpringMap.h"
#include "Game.h"

#include <cmath>
#include <limits>

#include "AISCommands.h"
#include "Command.h"
#include "Team.h"
#include "Log.h"

namespace circuit {

using namespace springai;

#define RESPONSE_EPS_MIN	0.1f  // response eps floor
#define RESPONSE_HOLD		3  // UpdateResponseGates samples (2 s apart) a gate change or an unlock must hold

CMilitaryManager::CMilitaryManager(CCircuitAI* circuit)
		: IUnitModule(circuit)
		, fightIterator(0)
		, defenceIdx(0)
		, scoutIdx(0)
		, armyCost(0.f)
		, armyThreat(0.f)
		, isAttackThreatFloor(false)
		, enemyHalfLife(0.f)
		, attackThreatFloorLogFrame(0)
		, attackRally(IFighterTask::FightType::ATTACK, false, "RALLY")
		, raidRally(IFighterTask::FightType::RAID, true, "RAID RALLY")
		, attackBaseGather(true)
		, attackIdleMerge(true)
		, heavySeenFrame(-1)
		, heavySeenAge(std::numeric_limits<int>::max())
		, radarDef(nullptr)
		, sonarDef(nullptr)
		, bigGunDef(nullptr)
{
	const float width = CTerrainManager::GetTerrainWidth();
	const float height = CTerrainManager::GetTerrainHeight();
	invMapDiagonal = 1.f / std::max(sqrtf(width * width + height * height), 1.f);

	circuit->GetScheduler()->RunOnInit(std::make_shared<CGameTask>(&CMilitaryManager::Init, this));

	/*
	 * Defence handlers
	 */
	auto defenceFinishedHandler = [this](CCircuitUnit* unit) {
		TRY_UNIT(this->circuit, unit,
			unit->GetUnit()->Stockpile(UNIT_COMMAND_OPTION_SHIFT_KEY | UNIT_COMMAND_OPTION_CONTROL_KEY);
		)
	};
	auto defenceDestroyedHandler = [this](CCircuitUnit* unit, CEnemyInfo* attacker) {
		int frame = this->circuit->GetLastFrame();
		float defCost = unit->GetCircuitDef()->GetCostM();
		CDefenceMatrix::SDefPoint* point = defence->GetDefPoint(unit->GetPos(frame), defCost);
		if (point != nullptr) {
			point->cost -= defCost;
		}
	};

	/*
	 * Attacker handlers
	 */
	auto attackerCreatedHandler = [this](CCircuitUnit* unit, CCircuitUnit* builder) {
		if (unit->GetTask() == nullptr) {
			unit->SetManager(this);
			nilTask->AssignTo(unit);
			this->circuit->AddActionUnit(unit);
		}
	};
	auto attackerFinishedHandler = [this](CCircuitUnit* unit) {
		if (unit->GetTask() == nullptr) {
			unit->SetManager(this);
			this->circuit->AddActionUnit(unit);
		}
		idleTask->AssignTo(unit);

		army.insert(unit);
		AddArmyCost(unit);

		// "guarded" attribute: this unit gets a guard slot like an early builder
		if (unit->GetCircuitDef()->IsAttrGuarded() && !unit->GetCircuitDef()->IsAbleToFly()) {
			AddGuardTask(unit);
		}

		TRY_UNIT(this->circuit, unit,
			if (unit->GetCircuitDef()->IsAbleToFly()) {
				if (unit->GetCircuitDef()->IsAttrNoStrafe()) {
					unit->CmdAirStrafe(0);
				}
				if (unit->GetCircuitDef()->IsRoleMine()) {
					unit->GetUnit()->SetIdleMode(1);
				}
			}
			if (unit->GetCircuitDef()->IsAttrStock()) {
				unit->GetUnit()->Stockpile(UNIT_COMMAND_OPTION_SHIFT_KEY | UNIT_COMMAND_OPTION_CONTROL_KEY);
				unit->CmdMiscPriority(2);
			}
		)
	};
	auto attackerIdleHandler = [this](CCircuitUnit* unit) {
		// NOTE: Avoid instant task reassignment, though it may be not relevant for attackers
		if (this->circuit->GetLastFrame() > unit->GetTaskFrame()/* + FRAMES_PER_SEC*/) {
			unit->GetTask()->OnUnitIdle(unit);
		}
	};
	auto attackerDamagedHandler = [](CCircuitUnit* unit, CEnemyInfo* attacker) {
		unit->GetTask()->OnUnitDamaged(unit, attacker);
	};
	auto attackerDestroyedHandler = [this](CCircuitUnit* unit, CEnemyInfo* attacker) {
		DelGuardTask(unit);  // its own guard slot, if it has the "guarded" attribute
		IUnitTask* task = unit->GetTask();
		task->OnUnitDestroyed(unit, attacker);  // can change task
		unit->GetTask()->RemoveAssignee(unit);  // Remove unit from IdleTask

		if (task->GetType() == IUnitTask::Type::NIL) {
			return;
		}

		DelArmyCost(unit);
		army.erase(unit);
	};

	/*
	 * Superweapon handlers
	 */
	auto superCreatedHandler = [this](CCircuitUnit* unit, CCircuitUnit* builder) {
		if (unit->GetTask() == nullptr) {
			unit->SetManager(this);
			nilTask->AssignTo(unit);
			this->circuit->AddActionUnit(unit);
		}
	};
	auto superFinishedHandler = [this](CCircuitUnit* unit) {
		if (unit->GetTask() == nullptr) {
			unit->SetManager(this);
			this->circuit->AddActionUnit(unit);
		}
		idleTask->AssignTo(unit);

		TRY_UNIT(this->circuit, unit,
			unit->GetUnit()->SetTrajectory(1);
			if (unit->GetCircuitDef()->IsAttrStock()) {
				unit->GetUnit()->Stockpile(UNIT_COMMAND_OPTION_SHIFT_KEY | UNIT_COMMAND_OPTION_CONTROL_KEY);
				unit->CmdMiscPriority(2);
			}
		)
	};
	auto superDestroyedHandler = [](CCircuitUnit* unit, CEnemyInfo* attacker) {
		IUnitTask* task = unit->GetTask();
		task->OnUnitDestroyed(unit, attacker);  // can change task
		unit->GetTask()->RemoveAssignee(unit);  // Remove unit from IdleTask
	};

	/*
	 * Defend buildings handler
	 */
//	auto structDamagedHandler = [this](CCircuitUnit* unit, CEnemyInfo* attacker) {
//		const std::set<IFighterTask*>& tasks = GetTasks(IFighterTask::FightType::DEFEND);
//		if (tasks.empty()) {
//			return;
//		}
//		int frame = this->circuit->GetLastFrame();
//		const AIFloat3& pos = unit->GetPos(frame);
//		CTerrainManager* terrainMgr = this->circuit->GetTerrainManager();
//		CDefendTask* defendTask = nullptr;
//		float minSqDist = std::numeric_limits<float>::max();
//		for (IFighterTask* task : tasks) {
//			CDefendTask* dt = static_cast<CDefendTask*>(task);
//			const float sqDist = pos.SqDistance2D(dt->GetPosition());
//			if ((minSqDist <= sqDist) || !terrainMgr->CanMoveToPos(dt->GetLeader()->GetArea(), pos)) {
//				continue;
//			}
//			if ((dt->GetTarget() == nullptr) ||
//				(dt->GetTarget()->GetPos().SqDistance2D(dt->GetLeader()->GetPos(frame)) > sqDist))
//			{
//				minSqDist = sqDist;
//				defendTask = dt;
//			}
//		}
//		if (defendTask != nullptr) {
//			defendTask->SetPosition(pos);
//			defendTask->SetWantedTarget(attacker);
//		}
//	};

	// NOTE: IsRole used below
	ReadConfig();

	for (const CCircuitDef* cdef : defenderDefs) {
		destroyedHandler[cdef->GetId()] = defenceDestroyedHandler;
	}

	const Json::Value& root = circuit->GetSetupManager()->GetConfig();
	const Json::Value& retreat = root["retreat"]["fighter"];
	const float fighterRet = retreat.get((unsigned)0, 0.5f).asFloat();
	const float retMod = retreat.get((unsigned)1, 1.0f).asFloat();
	const float commMod = root["quota"]["thr_mod"].get("comm", 1.f).asFloat();
	const float commPwrMod = root["quota"]["pwr_mod"].get("comm", 1.f).asFloat();
	float maxRadarDivCost = 0.f;
	float maxSonarDivCost = 0.f;
	CCircuitDef* commDef = circuit->GetSetupManager()->GetCommChoice();

	for (CCircuitDef& cdef : circuit->GetCircuitDefs()) {
		CCircuitDef::Id unitDefId = cdef.GetId();
		if (cdef.IsRoleComm()) {
			cdef.ModThreat(commMod);
			cdef.ModPower(commPwrMod);  // quota.pwr_mod.comm: on top of any per-unit pwr_mod
		}
		if (cdef.GetDef()->IsBuilder()) {
//			damagedHandler[unitDefId] = structDamagedHandler;
			continue;
		}
		const std::map<std::string, std::string>& customParams = cdef.GetDef()->GetCustomParams();
		auto it = customParams.find("is_drone");
		if ((it != customParams.end()) && (utils::string_to_int(it->second) == 1)) {
			continue;
		}
		if (cdef.IsMobile()) {
			createdHandler[unitDefId] = attackerCreatedHandler;
			finishedHandler[unitDefId] = attackerFinishedHandler;
			idleHandler[unitDefId] = attackerIdleHandler;
			damagedHandler[unitDefId] = attackerDamagedHandler;
			destroyedHandler[unitDefId] = attackerDestroyedHandler;

			if (cdef.GetRetreat() < 0.f) {
				cdef.SetRetreat(fighterRet);
			} else {
				cdef.SetRetreat(cdef.GetRetreat() * retMod);
			}
		} else {
//			damagedHandler[unitDefId] = structDamagedHandler;
			if (cdef.IsAttacker()) {
				if (cdef.IsRoleSuper()) {
					createdHandler[unitDefId] = superCreatedHandler;
					finishedHandler[unitDefId] = superFinishedHandler;
					destroyedHandler[unitDefId] = superDestroyedHandler;
				} else if (cdef.IsAttrStock()) {
					finishedHandler[unitDefId] = defenceFinishedHandler;
				}
			}
			if (commDef->CanBuild(&cdef)) {
				float range = cdef.GetDef()->GetRadarRadius();
				float areaDivCost = M_PI * SQUARE(range) / cdef.GetCostM();
				if (maxRadarDivCost < areaDivCost) {
					maxRadarDivCost = areaDivCost;
					radarDef = &cdef;
				}
				range = cdef.GetDef()->GetSonarRadius();
				areaDivCost = M_PI * SQUARE(range) / cdef.GetCostM();
				if (maxSonarDivCost < areaDivCost) {
					maxSonarDivCost = areaDivCost;
					sonarDef = &cdef;
				}
			}
		}
	}

	defence = circuit->GetAllyTeam()->GetDefenceMatrix().get();

	fightTasks.resize(static_cast<IFighterTask::FT>(IFighterTask::FightType::_SIZE_));
}

CMilitaryManager::~CMilitaryManager()
{
	for (IUnitTask* task : fightUpdates) {
		task->ClearRelease();
	}
}

void CMilitaryManager::ReadCurve(const Json::Value& value, const std::string& key, const char* xName,
		std::vector<std::pair<float, float>>& curve) const
{
	// [[x, multiplier >= 0], ...], sorted by x; bad points are logged and skipped; missing = empty
	if (value.isNull()) {
		return;
	}
	const std::string& cfgName = circuit->GetSetupManager()->GetConfigName();
	if (!value.isArray()) {
		circuit->LOG("CONFIG %s: %s must be a list of [<%s>, <multiplier>] points", cfgName.c_str(), key.c_str(), xName);
		return;
	}
	for (const Json::Value& pt : value) {
		if (!pt.isArray() || (pt.size() < 2) || !pt[0].isNumeric() || !pt[1].isNumeric() || (pt[1].asFloat() < 0.f)) {
			circuit->LOG("CONFIG %s: %s points must be [<%s>, <multiplier >= 0>]", cfgName.c_str(), key.c_str(), xName);
			continue;
		}
		curve.push_back(std::make_pair(pt[0].asFloat(), pt[1].asFloat()));
	}
	std::sort(curve.begin(), curve.end());
}

void CMilitaryManager::ReadConfig()
{
	const Json::Value& root = circuit->GetSetupManager()->GetConfig();
	const std::string& cfgName = circuit->GetSetupManager()->GetConfigName();
	CCircuitDef::RoleName& roleNames = CCircuitDef::GetRoleNames();

	const Json::Value& responses = root["response"];
	const float teamSize = circuit->GetAllyTeam()->GetSize();
	roleInfos.resize(roleNames.size(), {.0f});
	isResponseGates = false;
	for (const auto& pair : roleNames) {
		SRoleInfo& info = roleInfos[pair.second.type];
		const Json::Value& response = responses[pair.first];
		info.name = pair.first;

		if (response.isNull()) {
			info.maxPerc = 1.0f;
			info.factor  = teamSize;
			continue;
		}

		info.maxPerc = response.get("max_percent", 1.0f).asFloat();
		// eps = 1 + (teamSize - 1) * eps_step. Enemy metal is the whole enemy team's, our role metal this AI's own:
		// eps_step 1 = each AI answers its 1/teamSize share, > 1 less, < 1 (or negative) more. Floored: at 0 one
		// sighting would let the role grow to max_percent whatever the enemy amount
		const float step = response.get("eps_step", 1.0f).asFloat();
		info.factor  = (teamSize - 1.0f) * step + 1.0f;
		if (info.factor < RESPONSE_EPS_MIN) {
			circuit->LOG("CONFIG %s: response.%s eps %g clamped to %g (teamSize %g, eps_step %g)", cfgName.c_str(),
					pair.first.c_str(), info.factor, RESPONSE_EPS_MIN, teamSize, step);
			info.factor = RESPONSE_EPS_MIN;
		}

		// Number in [lo, hi] or the default, logged
		auto readNum = [&](const Json::Value& v, const std::string& key, float def, float lo, float hi) {
			if (v.isNull()) {
				return def;
			}
			if (!v.isNumeric()) {
				circuit->LOG("CONFIG %s: response.%s.%s must be a number, using %g", cfgName.c_str(), pair.first.c_str(), key.c_str(), def);
				return def;
			}
			const float val = v.asFloat();
			const float clamped = utils::clamp(val, lo, hi);
			if (clamped != val) {
				circuit->LOG("CONFIG %s: response.%s.%s %g clamped to %g", cfgName.c_str(), pair.first.c_str(), key.c_str(), val, clamped);
			}
			return clamped;
		};
		info.territoryMax = readNum(response["territory_max"], "territory_max", 1.f, 0.f, 1.f);
		const Json::Value& unlock = response["unlock_after"];
		if (!unlock.isNull()) {
			if (!unlock.isObject()) {
				circuit->LOG("CONFIG %s: response.%s.unlock_after must be an object {income, territory}, ignored", cfgName.c_str(), pair.first.c_str());
			} else {
				info.unlockIncome = readNum(unlock["income"], "unlock_after.income", 0.f, 0.f, std::numeric_limits<float>::max());
				info.unlockTerritory = readNum(unlock["territory"], "unlock_after.territory", 0.f, 0.f, 1.f);
				// Both 0 = nothing to wait for: unlocked from the start
				info.isUnlocked = info.isAllowed = (info.unlockIncome <= 0.f) && (info.unlockTerritory <= 0.f);
			}
		}
		ReadCurve(response["income_curve"], "response." + pair.first + ".income_curve", "income", info.incomeCurve);
		if (std::all_of(info.incomeCurve.begin(), info.incomeCurve.end(), [](const auto& pt) { return pt.second == 1.f; })) {
			info.incomeCurve.clear();  // flat 1 = no curve
		}
		if ((info.territoryMax < 1.f) || !info.isUnlocked || !info.incomeCurve.empty()) {
			isResponseGates = true;
		}

		const Json::Value& vs = response["vs"];
		const Json::Value& ratio = response["ratio"];
		const Json::Value& importance = response["importance"];
		for (unsigned i = 0; i < vs.size(); ++i) {
			const std::string& roleName = vs[i].asString();
			auto it = roleNames.find(roleName);
			if (it == roleNames.end()) {
				circuit->LOG("CONFIG %s: response %s vs unknown role '%s'", cfgName.c_str(), pair.first.c_str(), roleName.c_str());
				continue;
			}
			float rat = ratio.get(i, 1.0f).asFloat();
			float imp = importance.get(i, 1.0f).asFloat();
			info.vs.push_back(SRoleInfo::SVsInfo(it->second.type, rat, imp));
		}
	}

	const Json::Value& quotas = root["quota"];
	{
		// Scout and raider quotas (quota.scout / raider, *_per_mex, *_max, *_map_size) and spot scoring
		// (quota.scout_spot / raider_spot). Every raider key missing = no raider floor, old raid roaming
		auto readCount = [&](const std::string& key, float def) {
			const Json::Value& v = quotas[key];
			if (v.isNull()) {
				return def;
			}
			if (!v.isNumeric() || (v.asFloat() < 0.f)) {
				circuit->LOG("CONFIG %s: quota.%s must be a number >= 0, using %g", cfgName.c_str(), key.c_str(), def);
				return def;
			}
			return v.asFloat();
		};
		using MapCurve = std::vector<std::pair<float, float>>;
		auto readMapCurve = [&](const std::string& key, MapCurve& curve, const MapCurve& def) {
			curve.clear();
			const Json::Value& qmap = quotas[key];
			if (qmap.isNull()) {
				curve = def;
				return;
			}
			ReadCurve(qmap, "quota." + key, "map size", curve);
		};
		auto readQuota = [&](const std::string& name, SUnitQuota& q, float defMin, float defPerMex, float defMax,
				const MapCurve& defCurve) {
			q.min = static_cast<unsigned int>(readCount(name, defMin));
			q.perMex = readCount(name + "_per_mex", defPerMex);
			q.max = static_cast<unsigned int>(readCount(name + "_max", defMax));
			readMapCurve(name + "_map_size", q.mapCurve, defCurve);
		};
		readQuota("scout", scoutQuota, 3.f, 1.f, 12.f, {{8.f, 1.f}, {20.f, 4.f}});
		readQuota("raider", raiderQuota, 0.f, 0.f, 0.f, scoutQuota.mapCurve);  // missing curve = the scout one

		// Returns whether the object was given
		auto readSpot = [&](const std::string& name, SSpotCfg& cfg) {
			const Json::Value& qspot = quotas[name];
			const bool isObject = qspot.isObject();
			if (!qspot.isNull() && !isObject) {
				circuit->LOG("CONFIG %s: quota.%s must be an object, ignored", cfgName.c_str(), name.c_str());
			}
			auto get = [&](const char* key, float def) {
				const Json::Value& v = isObject ? qspot[key] : Json::Value::nullSingleton();
				if (v.isNull()) {
					return def;
				}
				if (!v.isNumeric()) {
					circuit->LOG("CONFIG %s: quota.%s.%s must be a number, using %g", cfgName.c_str(), name.c_str(), key, def);
					return def;
				}
				return v.asFloat();
			};
			cfg.enemy = get("enemy", 1.f);
			cfg.stale = get("stale", 1.f);
			cfg.travel = get("travel", 0.5f);
			cfg.staleTime = get("stale_time", 90.f) * FRAMES_PER_SEC;
			cfg.claimTime = get("claim_time", 20.f) * FRAMES_PER_SEC;
			cfg.rangeMult = get("static_range_mult", 1.f);
			cfg.homeRatio = get("home_ratio", 0.5f);
			return isObject;
		};
		readSpot("scout_spot", scoutSpot);
		isRaiderSpot = readSpot("raider_spot", raiderSpot);
	}
	const Json::Value& qraid = quotas["raid"];
	raid.min = qraid.get((unsigned)0, 3.f).asFloat();
	raid.avg = qraid.get((unsigned)1, 5.f).asFloat();
	minAttackers = quotas.get("attack", 8.f).asFloat();
	const Json::Value& qfleeShare = quotas.get("raid_flee_share", 0.5f);
	if (!qfleeShare.isNumeric()) {
		circuit->LOG("CONFIG %s: quota.raid_flee_share must be a number 0..1, using 0.5", cfgName.c_str());
	}
	raidFleeShare = utils::clamp(qfleeShare.isNumeric() ? qfleeShare.asFloat() : 0.5f, 0.f, 1.f);
	// Attack group numbers: a missing key uses the default, a non-number logs and uses it, others are clamped
	auto readAttackNum = [&](const char* key, float def, float lo, float hi) {
		const Json::Value& v = quotas.get(key, def);
		if (v.isNumeric()) {
			return utils::clamp(v.asFloat(), lo, hi);
		}
		if (hi < std::numeric_limits<float>::max()) {
			circuit->LOG("CONFIG %s: quota.%s must be a number %g..%g, using %g", cfgName.c_str(), key, lo, hi, def);
		} else {
			circuit->LOG("CONFIG %s: quota.%s must be a number >= %g, using %g", cfgName.c_str(), key, lo, def);
		}
		return def;
	};
	const float noMax = std::numeric_limits<float>::max();
	attackFleeShare = readAttackNum("attack_flee_share", 0.5f, 0.f, 1.f);
	attackKeepThreat = readAttackNum("attack_keep_threat", 1.34f, 1.f, noMax);  // below 1 = off
	{
		// quota.group_count_power is read by the threat map (CThreatMap::GetCountPower), clamped. Allied AIs share
		// that map: the ally that built it first decides the value for all
		const float countPower = circuit->GetThreatMap()->GetCountPower();
		const Json::Value& qcount = quotas["group_count_power"];
		if (qcount.isNumeric() && (qcount.asFloat() != countPower)) {
			circuit->LOG("CONFIG %s: quota.group_count_power %g: using %g (0..%g; allied AIs share the first one's)",
					cfgName.c_str(), qcount.asFloat(), countPower, CThreatMap::MAX_COUNT_POWER);
		}
	}
	countAoe = readAttackNum("group_count_aoe", 64.f, 0.f, noMax);  // 0 = off
	if (countAoe > 0.f) {  // for tuning: the mobile types that count as area damage
		std::string names;
		for (CCircuitDef& cdef : circuit->GetCircuitDefs()) {
			if (cdef.IsMobile() && IsAreaDamage(&cdef)) {
				names += names.empty() ? "" : ", ";
				names += cdef.GetDef()->GetName();
			}
		}
		circuit->LOG("CONFIG %s: quota.group_count_aoe %g: area damage: %s", cfgName.c_str(), countAoe,
				names.empty() ? "none" : names.c_str());
	}
	attackOwnGroundFrame = static_cast<int>(readAttackNum("attack_own_ground_time", 270.f, 0.f, 3600.f) * FRAMES_PER_SEC);  // 0 = off
	attackFloorHold = readAttackNum("attack_floor_hold", 0.75f, 0.f, 1.f);  // 1 = no hysteresis
	{
		const Json::Value& q = quotas["attack_threat_floor"];
		const bool isOk = q.isNull() || (q.isArray() && (q.size() == 2) && q[0u].isNumeric() && q[1u].isNumeric()
				&& (q[0u].asFloat() >= 0.f) && (q[1u].asFloat() >= q[0u].asFloat()));
		if (!isOk) {
			circuit->LOG("CONFIG %s: quota.attack_threat_floor must be [<on >= 0>, <off >= on>], using [0.66, 0.8]", cfgName.c_str());
		}
		attackThreatFloor = (isOk && !q.isNull()) ? SThreatFloor{q[0u].asFloat(), q[1u].asFloat()} : SThreatFloor{0.66f, 0.8f};
	}
	enemyHalfLife = readAttackNum("enemy_half_life", 180.f, 0.f, 3600.f) * FRAMES_PER_SEC;  // 0 = no fading
	{
		const Json::Value& qcont = quotas["contested"];
		if (!qcont.isNull() && !qcont.isObject()) {
			circuit->LOG("CONFIG %s: quota.contested must be an object, using defaults", cfgName.c_str());
		}
		const Json::Value& qc = qcont.isObject() ? qcont : Json::Value::nullSingleton();
		auto num = [&](const char* key, float def, float lo) {
			const Json::Value& v = qc.get(key, def);
			if (!v.isNumeric() || (v.asFloat() < lo)) {
				circuit->LOG("CONFIG %s: quota.contested.%s must be a number >= %g, using %g", cfgName.c_str(), key, lo, def);
				return def;
			}
			return v.asFloat();
		};
		contested.bonus = num("bonus", 2.f, 1.f);
		contested.maxAge = static_cast<int>(num("age", 60.f, 1.f) * FRAMES_PER_SEC);
		contested.range = num("range", 300.f, 0.f);
		circuit->GetMetalManager()->SetWallThreat(std::max(num("wall", THREAT_MIN, 0.f), THREAT_MIN));
	}
	attackPathIgnoreThreat = readAttackNum("attack_path_ignore_threat", 0.25f, 0.f, 1.f);
	{
		// Each route kind on or off (default on); a bare true/false sets them all
		static const char* const kinds[] = {"target", "reinforce", "front", "flee", "cornered"};
		const Json::Value& qdyn = quotas["attack_path_dynamic_cost"];
		for (int i = 0; i < static_cast<int>(AttackRoute::_SIZE_); ++i) {
			bool value = true;
			if (qdyn.isBool()) {
				value = qdyn.asBool();
			} else if (qdyn.isObject()) {
				const Json::Value& v = qdyn[kinds[i]];
				if (v.isBool()) {
					value = v.asBool();
				} else if (!v.isNull()) {
					circuit->LOG("CONFIG %s: quota.attack_path_dynamic_cost.%s must be true or false, using true",
							cfgName.c_str(), kinds[i]);
				}
			} else if (!qdyn.isNull()) {
				circuit->LOG("CONFIG %s: quota.attack_path_dynamic_cost must be an object of true/false, using true",
						cfgName.c_str());
			}
			attackPathDynamic[i] = value;
		}
	}
	attackFleePathMaxWeight = readAttackNum("attack_flee_path_max_weight", 3.f, 1.f, noMax);
	const Json::Value& qspotThreat = quotas.get("attack_spot_threat", 1.f);
	if (!qspotThreat.isNumeric() || (qspotThreat.asFloat() < 0.f)) {
		circuit->LOG("CONFIG %s: quota.attack_spot_threat must be a number >= 0, using 1.0", cfgName.c_str());
	}
	attackSpotThreat = (qspotThreat.isNumeric() && (qspotThreat.asFloat() >= 0.f)) ? qspotThreat.asFloat() : 1.f;
	attackMeleeSpot = readAttackNum("attack_melee_spot", 32.f, -1.f, noMax);
	// quota.attack_merge.rally, quota.raid_rally: {match, margin, range, seen_sec}; not an object (or match 0) = off
	auto readRallyCfg = [&](const Json::Value& q, const char* name, float defRange, SRallyCfg& outCfg) {
		if (!q.isNull() && !q.isObject()) {
			circuit->LOG("CONFIG %s: quota.%s must be an object, it is off", cfgName.c_str(), name);
		}
		if (!q.isObject()) {
			return;
		}
		auto readNum = [&](const char* key, float def) {
			const Json::Value& v = q[key];
			if (v.isNull()) {
				return def;
			}
			if (!v.isNumeric() || (v.asFloat() < 0.f)) {
				circuit->LOG("CONFIG %s: quota.%s.%s must be a number >= 0, using %g", cfgName.c_str(), name, key, def);
				return def;
			}
			return v.asFloat();
		};
		outCfg.match = readNum("match", 1.f);
		outCfg.margin = readNum("margin", 0.25f);
		outCfg.range = readNum("range", defRange);
		outCfg.seenFrames = static_cast<int>(readNum("seen_sec", 20.f) * FRAMES_PER_SEC);
	};
	readRallyCfg(quotas["raid_rally"], "raid_rally", 0.f, raidRally.cfg);
	{
		const Json::Value& qmerge = quotas["attack_merge"];
		if (!qmerge.isNull() && !qmerge.isObject()) {
			circuit->LOG("CONFIG %s: quota.attack_merge must be an object, using defaults", cfgName.c_str());
		}
		// Missing keys use the default; a non-number or a value below the minimum logs and uses the default
		auto readNum = [&](const char* name, float def, float minVal) {
			const Json::Value& v = qmerge.isObject() ? qmerge[name] : Json::Value();
			if (v.isNull()) {
				return def;
			}
			if (!v.isNumeric() || (v.asFloat() < minVal)) {
				circuit->LOG("CONFIG %s: quota.attack_merge.%s must be a number >= %g, using %g", cfgName.c_str(), name, minVal, def);
				return def;
			}
			return v.asFloat();
		};
		attackMerge.speedRatio = readNum("speed_ratio", 1.5f, 1.f);
		attackMerge.distance = readNum("distance", 2000.f, 1.f);  // 0 would stop merging but not reinforcing
		attackMerge.travelSec = readNum("travel_sec", 30.f, 1.f);
		if (attackMerge.travelSec > MAX_TRAVEL_SEC) {  // ISquadTask::CheckMergeTask has its own fixed limit
			circuit->LOG("CONFIG %s: quota.attack_merge.travel_sec above %i has no effect, using %i", cfgName.c_str(), MAX_TRAVEL_SEC, MAX_TRAVEL_SEC);
			attackMerge.travelSec = MAX_TRAVEL_SEC;
		}
		const Json::Value& qjoin = qmerge.isObject() ? qmerge["join_interval"] : Json::Value();
		const bool isJoinOk = qjoin.isNull() || (qjoin.isArray() && (qjoin.size() == 2)
				&& qjoin[0u].isNumeric() && qjoin[1u].isNumeric() && (qjoin[0u].asInt() >= 1) && (qjoin[1u].asInt() >= 1));
		if (!isJoinOk) {
			circuit->LOG("CONFIG %s: quota.attack_merge.join_interval must be [<small group >= 1>, <army >= 1>], using [4, 32]", cfgName.c_str());
		}
		attackMerge.joinSmall = (isJoinOk && !qjoin.isNull()) ? qjoin[0u].asInt() : 4;
		attackMerge.joinArmy = (isJoinOk && !qjoin.isNull()) ? qjoin[1u].asInt() : 32;
		attackMerge.absorb = static_cast<int>(readNum("absorb_interval", 8.f, 1.f));
		attackMerge.supportRadius = readNum("support_radius", 400.f, 0.f);
		attackMerge.reinforceRange = readNum("reinforce_range", 800.f, 0.f);
		attackMerge.reinforceFails = static_cast<int>(readNum("reinforce_fails", 10.f, 1.f));
		attackMerge.reinforceRetry = static_cast<int>(readNum("reinforce_retry", 10.f, 0.f) * FRAMES_PER_SEC);

		const Json::Value& qgather = qmerge.isObject() ? qmerge["base_gather"] : Json::Value();
		if (!qgather.isNull() && !qgather.isBool()) {
			circuit->LOG("CONFIG %s: quota.attack_merge.base_gather must be true or false, using true", cfgName.c_str());
		}
		attackBaseGather = qgather.isBool() ? qgather.asBool() : true;

		const Json::Value& qidle = qmerge.isObject() ? qmerge["idle_merge"] : Json::Value();
		if (!qidle.isNull() && !qidle.isBool()) {
			circuit->LOG("CONFIG %s: quota.attack_merge.idle_merge must be true or false, using true", cfgName.c_str());
		}
		attackIdleMerge = qidle.isBool() ? qidle.asBool() : true;

		readRallyCfg(qmerge.isObject() ? qmerge["rally"] : Json::Value(), "attack_merge.rally", 3000.f, attackRally.cfg);
	}
	{
		const Json::Value& q = quotas["attack_group_radius"];
		const bool isOk = q.isNull() || (q.isArray() && (q.size() == 2) && q[0u].isNumeric() && q[1u].isNumeric()
				&& (q[0u].asFloat() >= 0.f) && (q[1u].asFloat() >= 0.f));
		if (!isOk) {
			circuit->LOG("CONFIG %s: quota.attack_group_radius must be [<x group range >= 0>, <min elmos >= 0>], using [2, 800]", cfgName.c_str());
		}
		attackGroupRadius = (isOk && !q.isNull()) ? std::make_pair(q[0u].asFloat(), q[1u].asFloat()) : std::make_pair(2.f, 800.f);
	}
	{
		// Missing object or key: the default (off when the object is missing); a wrong value logs and uses the default
		const Json::Value& q = quotas["attack_static_cells"];
		if (!q.isNull() && !q.isObject()) {
			circuit->LOG("CONFIG %s: quota.attack_static_cells must be an object, using defaults (off)", cfgName.c_str());
		}
		const Json::Value& qc = q.isObject() ? q : Json::Value::nullSingleton();
		auto num = [&](const char* key, float def, float lo, float hi) {
			const Json::Value& v = qc[key];
			if (v.isNull()) {
				return def;
			}
			if (!v.isNumeric() || (v.asFloat() < lo) || (v.asFloat() > hi)) {
				circuit->LOG("CONFIG %s: quota.attack_static_cells.%s must be a number %g..%g, using %g", cfgName.c_str(), key, lo, hi, def);
				return def;
			}
			return v.asFloat();
		};
		SAttackCells& c = attackCells;
		const Json::Value& qon = qc["on"];
		if (!qon.isNull() && !qon.isBool()) {
			circuit->LOG("CONFIG %s: quota.attack_static_cells.on must be true or false, using false", cfgName.c_str());
		}
		c.isOn = qon.isBool() && qon.asBool();
		c.rings[0] = 0.9f;
		c.rings[1] = 0.7f;
		const Json::Value& qrings = qc["rings"];
		if (qrings.isArray() && (qrings.size() == 2) && qrings[0u].isNumeric() && qrings[1u].isNumeric()
			&& (qrings[0u].asFloat() > 0.f) && (qrings[0u].asFloat() <= 1.f)
			&& (qrings[1u].asFloat() > 0.f) && (qrings[1u].asFloat() <= 1.f))
		{
			c.rings[0] = qrings[0u].asFloat();
			c.rings[1] = qrings[1u].asFloat();
		} else if (!qrings.isNull()) {
			circuit->LOG("CONFIG %s: quota.attack_static_cells.rings must be [<0..1>, <0..1>], using [0.9, 0.7]", cfgName.c_str());
		}
		c.sectors = static_cast<int>(num("sectors", 8.f, 1.f, 32.f));
		c.angles = std::max(static_cast<int>(num("angles", 16.f, 1.f, 128.f)), c.sectors);
		c.threatWeight = num("threat_weight", 1.f, 0.f, std::numeric_limits<float>::max());
		c.scoreSlack = num("score_slack", 0.5f, 0.f, std::numeric_limits<float>::max());
		c.aimHeight = num("cell_aim_height", 16.f, 0.f, 1000.f);
		c.parkedFrames = static_cast<int>(num("parked_sec", 0.f, 0.f, 600.f) * FRAMES_PER_SEC);
		c.siegeBonus = num("siege_bonus", 1.f, 1.f, 10.f);
	}
	const Json::Value& raidTargets = quotas["raid_target_roles"];
	if (!raidTargets.isArray()) {  // legacy: constructors and commanders
		if (!raidTargets.isNull()) {
			circuit->LOG("CONFIG %s: quota.raid_target_roles must be a list of roles, using [\"builder\", \"commander\"]", cfgName.c_str());
		}
		raidTargetRoles = CCircuitDef::RoleMask::BUILDER | CCircuitDef::RoleMask::COMM;
	} else {
		raidTargetRoles = 0;
		for (const Json::Value& rt : raidTargets) {
			const std::string roleName = rt.isString() ? rt.asString() : rt.toStyledString();
			auto it = roleNames.find(roleName);
			if (!rt.isString() || (it == roleNames.end())) {
				circuit->LOG("CONFIG %s: quota.raid_target_roles has unknown role '%s'", cfgName.c_str(), roleName.c_str());
				continue;
			}
			raidTargetRoles |= CCircuitDef::GetMask(it->second.type);
		}
	}
	const Json::Value& qthrMod = quotas["thr_mod"];
	// Raid and scout tasks have their own [min, max]; missing = same as "attack" (old behaviour)
	auto readThrMod = [this, &qthrMod, &cfgName](const char* name, const SThreatQuota& fallback) {
		const Json::Value& q = qthrMod[name];
		if (q.isNull()) {
			return fallback;
		}
		// Non-numbers would throw (AI fails to load) and values <= 0 give an infinite/negative powerMod
		const Json::Value& q0 = q.get((unsigned)0, Json::Value());
		const Json::Value& q1 = q.get((unsigned)1, q0);
		if (!q.isArray() || !q0.isNumeric() || !q1.isNumeric() || (q0.asFloat() <= 0.f) || (q1.asFloat() <= 0.f)) {
			circuit->LOG("CONFIG %s: quota.thr_mod.%s must be [<min > 0>, <max > 0>], using [%.2f, %.2f]", cfgName.c_str(), name, fallback.min, fallback.min + fallback.len);
			return fallback;
		}
		SThreatQuota mod;
		mod.min = q0.asFloat();
		mod.len = q1.asFloat() - mod.min;
		return mod;
	};
	const Json::Value& qbaseBonus = qthrMod.get("base_bonus", 2.f);
	SThreatQuota defaultMod;
	defaultMod.min = 1.f;
	defaultMod.len = 0.f;
	attackMod = readThrMod("attack", defaultMod);
	baseThrBonus = qbaseBonus.isNumeric() ? qbaseBonus.asFloat() : 2.f;
	const Json::Value& qscaleMin = qthrMod.get("scale_min", 0.25f);
	if (!qscaleMin.isNumeric()) {
		circuit->LOG("CONFIG %s: quota.thr_mod.scale_min must be a number 0..1, using 0.25", cfgName.c_str());
	}
	thrScaleMin = utils::clamp(qscaleMin.isNumeric() ? qscaleMin.asFloat() : 0.25f, 0.f, 1.f);
	if (!qbaseBonus.isNumeric() || (baseThrBonus <= 0.f)) {
		// 0 would make nothing near the base centre engageable
		circuit->LOG("CONFIG %s: quota.thr_mod.base_bonus must be a number > 0, using 2.0", cfgName.c_str());
		baseThrBonus = 2.f;
	}
	thrDistCurves.assign(static_cast<IFighterTask::FT>(IFighterTask::FightType::_SIZE_), ThrDistCurve());
	const Json::Value& qthrDist = qthrMod["distance"];
	if (qthrDist.isObject()) {
		static const std::map<std::string, IFighterTask::FightType> distTasks = {
			{"scout",  IFighterTask::FightType::SCOUT},
			{"raid",   IFighterTask::FightType::RAID},
			{"defend", IFighterTask::FightType::DEFEND},
			{"attack", IFighterTask::FightType::ATTACK},
			{"aa",     IFighterTask::FightType::AA},
			{"ah",     IFighterTask::FightType::AH},
			{"bomber", IFighterTask::FightType::BOMB},
		};
		for (const std::string& taskName : qthrDist.getMemberNames()) {
			auto it = distTasks.find(taskName);
			if (it == distTasks.end()) {
				circuit->LOG("CONFIG %s: quota.thr_mod.distance has unknown task '%s' (scout, raid, defend, attack, aa, ah, bomber)", cfgName.c_str(), taskName.c_str());
				continue;
			}
			ThrDistCurve& curve = thrDistCurves[static_cast<IFighterTask::FT>(it->second)];
			const Json::Value& qcurve = qthrDist[taskName];
			if (!qcurve.isArray()) {
				circuit->LOG("CONFIG %s: quota.thr_mod.distance.%s must be a list of [<map fraction>, <multiplier>] points", cfgName.c_str(), taskName.c_str());
				continue;
			}
			for (const Json::Value& pt : qcurve) {
				if (!pt.isArray() || (pt.size() < 2) || !pt[0].isNumeric() || !pt[1].isNumeric() || (pt[1].asFloat() <= 0.f)) {
					circuit->LOG("CONFIG %s: quota.thr_mod.distance.%s points must be [<map fraction>, <multiplier > 0>]", cfgName.c_str(), taskName.c_str());
					continue;
				}
				curve.push_back(std::make_pair(pt[0].asFloat(), pt[1].asFloat()));
			}
			std::sort(curve.begin(), curve.end());
		}
	} else if (!qthrDist.isNull()) {
		circuit->LOG("CONFIG %s: quota.thr_mod.distance must be an object of task curves, e.g. {\"raid\": [[0.1, 0.5], [0.9, 2.0]]}", cfgName.c_str());
	}
	raidMod = readThrMod("raid", attackMod);
	scoutMod = readThrMod("scout", attackMod);
	defenceMod = readThrMod("defence", defaultMod);

	const Json::Value& artyCfg = root["artillery"];
	artillery.allyThreat = artyCfg.get("ally_threat", -1.f).asFloat();
	artillery.allyRadius = std::max(0.f, artyCfg.get("ally_radius", 500.f).asFloat());
	artillery.maxPathThreat = artyCfg.get("max_path_threat", -1.f).asFloat();
	artillery.followDist = std::max(0.f, artyCfg.get("follow_dist", 0.f).asFloat());
	artillery.isNoScout = artyCfg.get("no_scout", false).asBool();
	artillery.fleeMargin = artyCfg.get("artillery_flee_margin", -1.f).asFloat();

	const Json::Value& formCfg = root["formation"];
	formation.isEnable = formCfg.get("enable", false).asBool();
	formation.minUnits = std::max(2, formCfg.get("min_units", 3).asInt());
	const Json::Value& formSpacing = formCfg["spacing"];
	formation.spacing = std::max(1.f, formSpacing.get((unsigned)0, 96.f).asFloat());
	formation.spacingUnits = std::max(1.f, formSpacing.get((unsigned)1, 4.f).asFloat());
	formation.spacingExp = std::max(0.f, formSpacing.get((unsigned)2, 0.35f).asFloat());
	formation.gap = std::max(0.f, formCfg.get("gap", 16.f).asFloat());
	formation.widthRange = std::max(0.f, formCfg.get("width_range", 1.5f).asFloat());
	formation.leadSec = std::max(0.1f, formCfg.get("lead_sec", 1.5f).asFloat());
	formation.faceRange = std::max(0.f, formCfg.get("face_range", 2.f).asFloat());
	formation.splashSpacing = std::max(0.f, formCfg.get("splash_spacing", 1.2f).asFloat());
	formation.rowOrder.clear();
	Json::Value rowOrder = formCfg["row_order"];
	if (rowOrder.isNull()) {
		for (const char* name : {"riot", "assault", "heavy", "raider", "skirmish", "anti_heavy", "anti_air", "artillery"}) {
			rowOrder.append(name);
		}
	}
	for (const Json::Value& ro : rowOrder) {
		auto it = roleNames.find(ro.asString());
		if (it == roleNames.end()) {
			circuit->LOG("CONFIG %s: formation.row_order has unknown role '%s'", cfgName.c_str(), ro.asCString());
			continue;
		}
		formation.rowOrder.push_back(it->second.type);
	}

	const Json::Value& guardCfg = root["guard"];
	guard.maxWorkers = std::max(0, guardCfg.get("max_workers", 3).asInt());
	const Json::Value& guardSafe = guardCfg["safe"];
	if (guardSafe.isArray()) {
		guard.safeAlly = guardSafe.get((unsigned)0, 20.f).asFloat();
		guard.safeEnemy = guardSafe.get((unsigned)1, INFL_EPS).asFloat();
	} else {
		if (!guardSafe.isNull()) {
			circuit->LOG("CONFIG %s: guard.safe must be [ally, enemy], using defaults", cfgName.c_str());
		}
		guard.safeAlly = 20.f;
		guard.safeEnemy = INFL_EPS;
	}
	guard.safeFrames = std::max(0.f, guardCfg.get("safe_time", 5.f).asFloat()) * FRAMES_PER_SEC;
	guard.pullRange = guardCfg.get("pull_range", 800.f).asFloat();
	const Json::Value& interpose = guardCfg["interpose"];
	guard.interposeDist = interpose.get((unsigned)0, 120.f).asFloat();
	guard.interposeRange = interpose.get((unsigned)1, 600.f).asFloat();
	const float threatCd = guardCfg.get("threat_cooldown", 10.f).asFloat();
	guard.threatCooldown = (threatCd < 0.f) ? -1 : int(threatCd * FRAMES_PER_SEC);
	guard.threatRatio = std::max(0.f, guardCfg.get("threat_ratio", 0.f).asFloat());
	guard.maxShare = std::max(0.f, guardCfg.get("max_share", 1.f).asFloat());
	const float guardedShare = guardCfg.get("guarded_max_share", -1.f).asFloat();
	guard.guardedMaxShare = (guardedShare < 0.f) ? guard.maxShare : guardedShare;
	guard.isDamageExit = guardCfg.get("damage_exit", true).asBool();
	guard.isDebug = guardCfg.get("debug", false).asBool();
	guard.roleMask = 0;
	const Json::Value& guardRoles = guardCfg["roles"];
	if (guardRoles.isNull()) {  // legacy behaviour
		guard.roleMask = CCircuitDef::RoleMask::RAIDER | CCircuitDef::RoleMask::RIOT;
	} else {
		for (const Json::Value& gr : guardRoles) {
			const std::string& roleName = gr.asString();
			auto it = roleNames.find(roleName);
			if (it == roleNames.end()) {
				circuit->LOG("CONFIG %s: guard.roles has unknown role '%s'", cfgName.c_str(), roleName.c_str());
				continue;
			}
			guard.roleMask |= CCircuitDef::GetMask(it->second.type);
		}
	}

	const Json::Value& aaCfg = root["antiair"];
	antiAir.leash = aaCfg.get("leash", -1.f).asFloat();
	antiAir.isOwnGround = aaCfg.get("own_ground", false).asBool();
	antiAir.fireSpot = aaCfg.get("fire_spot", -1.f).asFloat();
	antiAir.fleeMargin = aaCfg.get("flee_margin", -1.f).asFloat();
	// With the flee on and no cover key, cover defaults to 10: else AA flees even inside an army
	antiAir.fleeAllyThreat = aaCfg.get("flee_ally_threat", (antiAir.fleeMargin >= 0.f) ? 10.f : -1.f).asFloat();
	antiAir.fleeAllyRadius = std::max(0.f, aaCfg.get("flee_ally_radius", 500.f).asFloat());
	if (antiAir.fireSpot > 1.f) {
		circuit->LOG("CONFIG %s: antiair.fire_spot must be <= 1 (fraction of weapon range), using 1", cfgName.c_str());
		antiAir.fireSpot = 1.f;
	}

	const Json::Value& ahCfg = root["antiheavy"];
	antiHeavy.pathThreat = ahCfg.get("path_threat", -1.f).asFloat();
	antiHeavy.fleeMargin = ahCfg.get("flee_margin", -1.f).asFloat();
	antiHeavy.fleeAllyThreat = ahCfg.get("flee_ally_threat", (antiHeavy.fleeMargin >= 0.f) ? 10.f : -1.f).asFloat();
	antiHeavy.fleeAllyRadius = std::max(0.f, ahCfg.get("flee_ally_radius", 500.f).asFloat());

	const Json::Value& resignCfg = root["resign"];
	const float resignTime = resignCfg.get("time", 480.f).asFloat();  // on by default, even with an older behaviour.json
	resign.frame = (resignTime < 0.f) ? -1 : int(resignTime * FRAMES_PER_SEC);
	resign.mexShare = resignCfg.get("mex", 0.1f).asFloat();
	resign.threatMult = resignCfg.get("threat", 4.f).asFloat();
	resign.holdFrames = int(std::max(0.f, resignCfg.get("hold", 30.f).asFloat()) * FRAMES_PER_SEC);

	const float drownedTime = root["drowned"].get("time", -1.f).asFloat();
	drownedFrames = (drownedTime < 0.f) ? -1 : int(drownedTime * FRAMES_PER_SEC);

	const Json::Value& porc = root["porcupine"];
	const Json::Value& defs = porc["unit"];
	defenderDefs.reserve(defs.size());
	for (const Json::Value& def : defs) {
		CCircuitDef* cdef = circuit->GetCircuitDef(def.asCString());
		if (cdef == nullptr) {
			circuit->LOG("CONFIG %s: has unknown UnitDef '%s'", cfgName.c_str(), def.asCString());
		} else {
			defenderDefs.push_back(cdef);
		}
	}
	const Json::Value& land = porc["land"];
	landDefenders.reserve(land.size());
	for (const Json::Value& idx : land) {
		unsigned index = idx.asUInt();
		if (index < defenderDefs.size()) {
			landDefenders.push_back(defenderDefs[index]);
		}
	}
	const Json::Value& watr = porc["water"];
	waterDefenders.reserve(watr.size());
	for (const Json::Value& idx : watr) {
		unsigned index = idx.asUInt();
		if (index < defenderDefs.size()) {
			waterDefenders.push_back(defenderDefs[index]);
		}
	}

	preventCount = porc.get("prevent", 1).asUInt();
	const Json::Value& amount = porc["amount"];
	const Json::Value& amOff = amount["offset"];
	const Json::Value& amFac = amount["factor"];
	const Json::Value& amMap = amount["map"];
	const float minOffset = amOff.get((unsigned)0, -0.2f).asFloat();
	const float maxOffset = amOff.get((unsigned)1, 0.2f).asFloat();
	const float offset = (float)rand() / RAND_MAX * (maxOffset - minOffset) + minOffset;
	const float minFactor = amFac.get((unsigned)0, 2.0f).asFloat();
	const float maxFactor = amFac.get((unsigned)1, 1.0f).asFloat();
	const float minMap = amMap.get((unsigned)0, 8.0f).asFloat();
	const float maxMap = amMap.get((unsigned)1, 24.0f).asFloat();
	const float mapSize = (circuit->GetMap()->GetWidth() / 64) * (circuit->GetMap()->GetHeight() / 64);
	amountFactor = (maxFactor - minFactor) / (SQUARE(maxMap) - SQUARE(minMap)) * (mapSize - SQUARE(minMap)) + minFactor + offset;
//	amountFactor = std::max(amountFactor, 0.f);

	const Json::Value& base = porc["base"];
	baseDefence.reserve(base.size());
	for (const Json::Value& pair : base) {
		unsigned index = pair.get((unsigned)0, -1).asUInt();
		if (index >= defenderDefs.size()) {
			continue;
		}
		int frame = pair.get((unsigned)1, 0).asInt() * FRAMES_PER_SEC;
		baseDefence.push_back(std::make_pair(defenderDefs[index], frame));
	}
	auto compare = [](const std::pair<CCircuitDef*, int>& d1, const std::pair<CCircuitDef*, int>& d2) {
		return d1.second > d2.second;
	};
	std::sort(baseDefence.begin(), baseDefence.end(), compare);

	const Json::Value& super = porc["superweapon"];
	const Json::Value& items = super["unit"];
	const Json::Value& probs = super["weight"];
	superInfos.reserve(items.size());
	for (unsigned i = 0; i < items.size(); ++i) {
		SSuperInfo si;
		si.cdef = circuit->GetCircuitDef(items[i].asCString());
		if (si.cdef == nullptr) {
			circuit->LOG("CONFIG %s: has unknown UnitDef '%s'", cfgName.c_str(), items[i].asCString());
			continue;
		}
		si.cdef->SetMainRole(ROLE_TYPE(SUPER));  // override mainRole
		si.cdef->AddEnemyRole(ROLE_TYPE(SUPER));
		si.cdef->AddRole(ROLE_TYPE(SUPER));
		si.weight = probs.get(i, 1.f).asFloat();
		superInfos.push_back(si);
	}
	DiceBigGun();

	defaultPorc = circuit->GetCircuitDef(porc.get("default", "").asCString());
	if (defaultPorc == nullptr) {
		defaultPorc = circuit->GetEconomyManager()->GetDefaultDef();
	}
}

void CMilitaryManager::Init()
{
	CMetalManager* metalMgr = circuit->GetMetalManager();
	const CMetalData::Metals& spots = metalMgr->GetSpots();
	const CMetalData::Clusters& clusters = metalMgr->GetClusters();

	scoutPath.reserve(spots.size());
	for (unsigned i = 0; i < spots.size(); ++i) {
		scoutPath.push_back(i);
	}

	raidPath.reserve(clusters.size());
	for (unsigned i = 0; i < clusters.size(); ++i) {
		raidPath.push_back({i, -1, 1.f});
	}

	CSetupManager::StartFunc subinit = [this, &spots](const AIFloat3& pos) {
		auto compare = [&pos, &spots](int a, int b) {
			return pos.SqDistance2D(spots[a].position) > pos.SqDistance2D(spots[b].position);
		};
		std::sort(scoutPath.begin(), scoutPath.end(), compare);

		CScheduler* scheduler = circuit->GetScheduler().get();
		const int interval = 4;
		const int offset = circuit->GetSkirmishAIId() % interval;
		scheduler->RunTaskEvery(std::make_shared<CGameTask>(&CMilitaryManager::UpdateIdle, this), interval, offset + 0);
		scheduler->RunTaskEvery(std::make_shared<CGameTask>(&CMilitaryManager::UpdateFight, this), 1/*interval / 2*/, offset + 1);
		scheduler->RunTaskEvery(std::make_shared<CGameTask>(&CMilitaryManager::UpdateDefenceTasks, this), FRAMES_PER_SEC * 5, offset + 2);
		if ((scoutQuota.perMex > 0.f) || (raiderQuota.perMex > 0.f)) {
			scheduler->RunTaskEvery(std::make_shared<CGameTask>(&CMilitaryManager::UpdateScoutQuota, this), FRAMES_PER_SEC * 15, offset + 3);
		}

		scheduler->RunTaskEvery(std::make_shared<CGameTask>(&CMilitaryManager::UpdateCaptureHide, this), FRAMES_PER_SEC, offset + 3);
		scheduler->RunTaskEvery(std::make_shared<CGameTask>(&CMilitaryManager::UpdateAhPull, this), FRAMES_PER_SEC * 2, offset + 1);
		if (attackRally.cfg.IsOn()) {
			scheduler->RunTaskEvery(std::make_shared<CGameTask>(&CMilitaryManager::UpdateAttackRally, this), FRAMES_PER_SEC, offset + 2);
		}
		if (raidRally.cfg.IsOn()) {
			scheduler->RunTaskEvery(std::make_shared<CGameTask>(&CMilitaryManager::UpdateRaidRally, this), FRAMES_PER_SEC, offset + 3);
		}
		if (attackThreatFloor.on > 0.f) {
			scheduler->RunTaskEvery(std::make_shared<CGameTask>(&CMilitaryManager::UpdateAttackThreatFloor, this), FRAMES_PER_SEC, offset + 3);
		}

		if (resign.frame >= 0) {
			scheduler->RunTaskEvery(std::make_shared<CGameTask>(&CMilitaryManager::UpdateResign, this), FRAMES_PER_SEC * 10, offset + 1);
		}
		if (drownedFrames >= 0) {
			scheduler->RunTaskEvery(std::make_shared<CGameTask>(&CMilitaryManager::UpdateDrowned, this), FRAMES_PER_SEC, offset + 2);
		}
		if (isResponseGates) {
			scheduler->RunTaskEvery(std::make_shared<CGameTask>(&CMilitaryManager::UpdateResponseGates, this), FRAMES_PER_SEC * 2, offset + 3);
		}

		scheduler->RunTaskEvery(std::make_shared<CGameTask>(&CMilitaryManager::Watchdog, this),
								FRAMES_PER_SEC * 60,
								circuit->GetSkirmishAIId() * WATCHDOG_COUNT + 12);
	};

	circuit->GetSetupManager()->ExecOnFindStart(subinit);
}

void CMilitaryManager::Release()
{
	// NOTE: Release expected to be called on CCircuit::Release.
	//       It doesn't stop scheduled GameTasks for that reason.
	attackRally.startFrame = -1;  // no new rally host while every task ends
	raidRally.startFrame = -1;
	for (IUnitTask* task : fightUpdates) {
		AbortTask(task);
		// NOTE: Do not delete task as other AbortTask may ask for it
	}
	for (IUnitTask* task : fightUpdates) {
		task->ClearRelease();
	}
	fightUpdates.clear();
}

int CMilitaryManager::UnitCreated(CCircuitUnit* unit, CCircuitUnit* builder)
{
	auto search = createdHandler.find(unit->GetCircuitDef()->GetId());
	if (search != createdHandler.end()) {
		search->second(unit, builder);
	}

	return 0; //signaling: OK
}

int CMilitaryManager::UnitFinished(CCircuitUnit* unit)
{
	auto search = finishedHandler.find(unit->GetCircuitDef()->GetId());
	if (search != finishedHandler.end()) {
		search->second(unit);
	}

	return 0; //signaling: OK
}

int CMilitaryManager::UnitIdle(CCircuitUnit* unit)
{
	auto search = idleHandler.find(unit->GetCircuitDef()->GetId());
	if (search != idleHandler.end()) {
		search->second(unit);
	}

	return 0; //signaling: OK
}

int CMilitaryManager::UnitDamaged(CCircuitUnit* unit, CEnemyInfo* attacker)
{
	auto search = damagedHandler.find(unit->GetCircuitDef()->GetId());
	if (search != damagedHandler.end()) {
		search->second(unit, attacker);
	}

	return 0; //signaling: OK
}

int CMilitaryManager::UnitDestroyed(CCircuitUnit* unit, CEnemyInfo* attacker)
{
	capturedUnits.erase(unit->GetId());  // also called when a unit is taken back from us

	auto search = destroyedHandler.find(unit->GetCircuitDef()->GetId());
	if (search != destroyedHandler.end()) {
		search->second(unit, attacker);
	}

	return 0; //signaling: OK
}

int CMilitaryManager::UnitGiven(CCircuitUnit* unit, int oldTeamId, int newTeamId)
{
	// Zero-K's unit_capture gadget sets "capture_controller" before it hands a captured unit over
	const int controllerId = (int)unit->GetUnit()->GetRulesParamFloat("capture_controller", -1.f);
	if (controllerId >= 0) {
		capturedUnits[unit->GetId()] = {controllerId, unit->GetCircuitDef()->GetCostM()};
	}
	return IModule::UnitGiven(unit, oldTeamId, newTeamId);
}

IFighterTask* CMilitaryManager::EnqueueTask(IFighterTask::FightType type)
{
	IFighterTask* task;
	switch (type) {
		default:
		case IFighterTask::FightType::RALLY: {
//			CEconomyManager* economyMgr = circuit->GetEconomyManager();
//			float power = economyMgr->GetAvgMetalIncome() * economyMgr->GetEcoFactor() * 32.0f;
			task = new CRallyTask(this, /*power*/1);  // TODO: pass enemy's threat
			break;
		}
		case IFighterTask::FightType::SCOUT: {
			const float mod = (float)rand() / RAND_MAX * scoutMod.len + scoutMod.min;
			task = new CScoutTask(this, 0.75f / mod);
			break;
		}
		case IFighterTask::FightType::RAID: {
			const float mod = (float)rand() / RAND_MAX * raidMod.len + raidMod.min;
			task = new CRaidTask(this, raid.avg, 0.75f / mod);
			break;
		}
		case IFighterTask::FightType::ATTACK: {
			const float mod = (float)rand() / RAND_MAX * attackMod.len + attackMod.min;
			task = new CAttackTask(this, minAttackers, 0.8f / mod);
			break;
		}
		case IFighterTask::FightType::BOMB: {
			const float mod = (float)rand() / RAND_MAX * attackMod.len + attackMod.min;
			task = new CBombTask(this, 2.0f / mod);
			break;
		}
		case IFighterTask::FightType::ARTY: {
			task = new CArtilleryTask(this);
			break;
		}
		case IFighterTask::FightType::AA: {
			const float mod = (float)rand() / RAND_MAX * attackMod.len + attackMod.min;
			task = new CAntiAirTask(this, 1.0f / mod);
			break;
		}
		case IFighterTask::FightType::AH: {
			const float mod = (float)rand() / RAND_MAX * attackMod.len + attackMod.min;
			task = new CAntiHeavyTask(this, 2.0f / mod);
			break;
		}
		case IFighterTask::FightType::SUPPORT: {
			task = new CSupportTask(this);
			break;
		}
		case IFighterTask::FightType::SUPER: {
			task = new CSuperTask(this);
			break;
		}
	}

	fightTasks[static_cast<IFighterTask::FT>(type)].insert(task);
	fightUpdates.push_back(task);
	return task;
}

IFighterTask* CMilitaryManager::EnqueueDefend(IFighterTask::FightType promote, float power)
{
	const float mod = (float)rand() / RAND_MAX * defenceMod.len + defenceMod.min;
	IFighterTask* task = new CDefendTask(this, circuit->GetSetupManager()->GetBasePos(),
										 promote, promote, power, 1.0f / mod);
	fightTasks[static_cast<IFighterTask::FT>(IFighterTask::FightType::DEFEND)].insert(task);
	fightUpdates.push_back(task);
	return task;
}

IFighterTask* CMilitaryManager::EnqueueDefend(IFighterTask::FightType check, IFighterTask::FightType promote, float power)
{
	IFighterTask* task = new CDefendTask(this, circuit->GetSetupManager()->GetBasePos(),
										 check, promote, power, 1.0f);
	fightTasks[static_cast<IFighterTask::FT>(IFighterTask::FightType::DEFEND)].insert(task);
	fightUpdates.push_back(task);
	return task;
}

float CMilitaryManager::GetAttackGatherPower() const
{
	return std::max(minAttackers, circuit->GetEnemyManager()->GetEnemyThreat() / circuit->GetAllyTeam()->GetAliveSize());
}

bool CMilitaryManager::IsRankedAbove(float power, const CCircuitUnit* leader, float otherPower,
		const CCircuitUnit* otherLeader)
{
	return (power > otherPower) || ((power == otherPower) && (leader->GetId() < otherLeader->GetId()));
}

bool CMilitaryManager::CanReinforce(const CAttackTask* squad, CCircuitUnit* unit) const
{
	return squad->IsCompatible(unit) && circuit->GetTerrainManager()->CanMoveToPos(unit->GetArea(),
			squad->GetLeader()->GetPos(circuit->GetLastFrame()));
}

CAttackTask* CMilitaryManager::FindReinforceSquad(CCircuitUnit* unit, float power, const IFighterTask* self,
		bool isRally) const
{
	const int frame = circuit->GetLastFrame();
	const AIFloat3& pos = unit->GetPos(frame);
	// Rally: everything compatible reinforces the host, whatever its size
	CAttackTask* host = isRally ? GetRallyHostFor(unit, self) : nullptr;
	if (host != nullptr) {
		return host;
	}
	CAttackTask* best = nullptr;
	float minSqDist = std::numeric_limits<float>::max();
	for (IFighterTask* t : GetTasks(IFighterTask::FightType::ATTACK)) {
		CAttackTask* squad = static_cast<CAttackTask*>(t);
		CCircuitUnit* squadLeader = squad->GetLeader();
		if ((squad == self) || squad->IsDead() || (squadLeader == nullptr)
			|| !IsRankedAbove(squad->GetAttackPower(), squadLeader, power, unit))
		{
			continue;
		}
		const float sqDist = pos.SqDistance2D(squadLeader->GetPos(frame));
		if ((sqDist < minSqDist) && CanReinforce(squad, unit)) {
			minSqDist = sqDist;
			best = squad;
		}
	}
	return best;
}

IFighterTask* CMilitaryManager::EnqueueReinforce(CCircuitUnit* unit, float power)
{
	CAttackTask* squad = FindReinforceSquad(unit, power);
	if (squad == nullptr) {
		return nullptr;
	}
	CAttackTask* task = static_cast<CAttackTask*>(EnqueueTask(IFighterTask::FightType::ATTACK));
	task->SetReinforce(squad);
	return task;
}

/*
 * Rallies: quota.attack_merge.rally, quota.raid_rally
 */

CMilitaryManager::SRally::SRally(IFighterTask::FightType type, bool isRaiders, const char* tag)
		: type(type)
		, isRaiders(isRaiders)
		, tag(tag)
		, blobPos(-RgtVector)
{
}

ISquadTask* CMilitaryManager::GetValidHost(const SRally& rally) const
{
	// The host is cleared in DequeueTask when its task ends
	ISquadTask* host = rally.host;
	return ((host != nullptr) && !host->IsDead() && (host->GetLeader() != nullptr)) ? host : nullptr;
}

CAttackTask* CMilitaryManager::GetRallyHost() const
{
	return static_cast<CAttackTask*>(GetValidHost(attackRally));
}

CAttackTask* CMilitaryManager::GetRallyHostFor(CCircuitUnit* unit, const IFighterTask* self) const
{
	// Compatible with the host and able to reach it (another terrain area would hold for the whole rally)
	CAttackTask* host = GetRallyHost();
	return ((host != nullptr) && (host != self) && CanReinforce(host, unit)) ? host : nullptr;
}

bool CMilitaryManager::IsRallyJoiner(const CAttackTask* task) const
{
	return (task->GetLeader() != nullptr) && (GetRallyHostFor(task->GetLeader(), task) != nullptr);
}

CRaidTask* CMilitaryManager::GetRaidRallyHost() const
{
	return static_cast<CRaidTask*>(GetValidHost(raidRally));
}

bool CMilitaryManager::IsRaidRallyJoiner(const CRaidTask* task) const
{
	CRaidTask* host = GetRaidRallyHost();
	CCircuitUnit* leader = task->GetLeader();
	return (host != nullptr) && (host != task) && (leader != nullptr) && host->IsRallyCompatible(leader)
			&& circuit->GetTerrainManager()->CanMoveToPos(leader->GetArea(), host->GetLeader()->GetPos(circuit->GetLastFrame()));
}

float CMilitaryManager::GetRaidRallyGoal() const
{
	return raidRally.IsActive() ? raidRally.cfg.match * raidRally.blobThreat * (1.f + raidRally.cfg.margin) : 0.f;
}

void CMilitaryManager::CollectRallyGroups(const SRally& rally, std::vector<ISquadTask*>& outGroups) const
{
	outGroups.clear();
	for (IFighterTask* t : GetTasks(rally.type)) {
		ISquadTask* g = static_cast<ISquadTask*>(t);  // attack and raid tasks are squads
		if (!g->IsDead() && (g->GetLeader() != nullptr) && !g->GetLeader()->GetCircuitDef()->IsAbleToFly()) {
			outGroups.push_back(g);
		}
	}
}

ISquadTask* CMilitaryManager::PickRallyHost(const std::vector<ISquadTask*>& groups, ISquadTask* prefer) const
{
	// The movement class with the most total strength: for each group, the strength of all groups that may join it
	ISquadTask* best = nullptr;
	float bestTotal = -1.f;
	float bestStrength = -1.f;
	for (ISquadTask* g : groups) {
		float total = 0.f;
		for (ISquadTask* h : groups) {
			if ((h == g) || g->IsRallyCompatible(h->GetLeader())) {
				total += h->GetRallyStrength();
			}
		}
		const float strength = g->GetRallyStrength();
		if ((total > bestTotal * 1.001f) || ((total >= bestTotal * 0.999f) && (strength > bestStrength))) {
			best = g;
			bestTotal = total;
			bestStrength = strength;
		}
	}
	// Restart: keep the previous host while it is within 20% of the best (no new host for a near tie)
	if ((best != nullptr) && (prefer != nullptr) && (prefer != best)
		&& (std::find(groups.begin(), groups.end(), prefer) != groups.end())
		&& (prefer->GetRallyStrength() >= 0.8f * bestStrength) && prefer->IsRallyCompatible(best->GetLeader()))
	{
		return prefer;
	}
	return best;
}

void CMilitaryManager::EndRally(SRally& rally, const char* reason, int frame)
{
	ISquadTask* host = GetValidHost(rally);
	circuit->LOG("%s end (%s): host %.1f, blob %.1f at (%.0f, %.0f)", rally.tag, reason,
			(host != nullptr) ? host->GetRallyStrength() * host->GetCountPowerMod() : 0.f, rally.blobThreat,
			rally.blobPos.x, rally.blobPos.z);
	rally.prevHost = host;
	rally.host = nullptr;
	rally.startFrame = -1;
	rally.endFrame = frame;
}

void CMilitaryManager::ReplaceRallyHost(SRally& rally, const IFighterTask* gone)
{
	// The host died or merged away: the strongest group takes over at once, so joiners don't hunt for a decision
	// meanwhile (UpdateRally ends the rally if no group is left)
	static std::vector<ISquadTask*> groups;  // NOTE: micro-opt
	CollectRallyGroups(rally, groups);
	groups.erase(std::remove(groups.begin(), groups.end(), gone), groups.end());
	rally.host = groups.empty() ? nullptr : PickRallyHost(groups, nullptr);
	if (rally.host != nullptr) {
		circuit->LOG("%s new host %.1f", rally.tag, rally.host->GetRallyStrength());
	}
}

void CMilitaryManager::OnRallyTaskEnd(SRally& rally, const IFighterTask* task)
{
	if (task == rally.host) {
		rally.host = nullptr;
		if (rally.IsActive()) {
			ReplaceRallyHost(rally, task);
		}
	}
	if (task == rally.prevHost) {
		rally.prevHost = nullptr;
	}
}

void CMilitaryManager::UpdateAttackRally()
{
	UpdateRally(attackRally);
}

void CMilitaryManager::UpdateRaidRally()
{
	UpdateRally(raidRally);
}

void CMilitaryManager::UpdateRally(SRally& rally)
{
	SCOPED_TIME(circuit, __PRETTY_FUNCTION__);
	constexpr int RALLY_MIN_FRAMES = FRAMES_PER_SEC * 10;  // a rally lasts at least this long
	constexpr int RALLY_GAP_FRAMES = FRAMES_PER_SEC * 15;  // and the next can't start sooner than this after it
	const SRallyCfg& cfg = rally.cfg;
	const int frame = circuit->GetLastFrame();
	const bool isRally = rally.IsActive();
	if (!isRally && (frame < rally.endFrame + RALLY_GAP_FRAMES)) {
		return;
	}

	static std::vector<ISquadTask*> groups;  // NOTE: micro-opt
	CollectRallyGroups(rally, groups);
	if (groups.empty()) {
		if (isRally) {
			EndRally(rally, "no groups", frame);
		}
		return;
	}

	// Blobs near our groups (range 0: anywhere), hottest first: within range to start, 1.5x range to keep going
	const bool isAnywhere = (cfg.range <= 0.f);
	const float sqRange = SQUARE(isRally ? cfg.range * 1.5f : cfg.range);
	static std::vector<const CEnemyManager::SEnemyBlob*> nearBlobs;  // NOTE: micro-opt
	nearBlobs.clear();
	for (const CEnemyManager::SEnemyBlob& b : circuit->GetEnemyManager()->GetBlobs(frame, cfg.seenFrames, rally.isRaiders)) {
		if (isAnywhere || std::any_of(groups.begin(), groups.end(), [&](ISquadTask* g) {
			return g->GetLeader()->GetPos(frame).SqDistance2D(b.pos) < sqRange;
		})) {
			nearBlobs.push_back(&b);
		}
	}
	if (!isRally && nearBlobs.empty()) {
		return;  // nothing to rally against: no host to pick
	}

	ISquadTask* host = isRally ? GetValidHost(rally) : PickRallyHost(groups, rally.prevHost);
	if (host == nullptr) {  // fallback: DequeueTask normally replaces a host that ends
		ReplaceRallyHost(rally, nullptr);
		host = rally.host;
	} else if (isRally && rally.isRaiders) {
		// Raid rally: another group grew past the host (joiners merge into the nearest bigger group, not only the
		// host): it takes over once it is 25% stronger (PickRallyHost keeps the host within 20%), so the end check
		// reads the group that is actually growing. Only a group of the host's movement class: class totals have no
		// hysteresis, so close ones would swap the host and its joiners each second.
		// Not the attack rally: its joiners all go to the host, and a bigger army arriving there must still walk to
		// the host's fight instead of taking over and judging the blob alone
		ISquadTask* best = PickRallyHost(groups, host);
		if ((best != host) && best->IsRallyCompatible(host->GetLeader())) {
			rally.host = host = best;
			circuit->LOG("%s new host %.1f (grew past the old one)", rally.tag, host->GetRallyStrength());
		}
	}
	// The hottest near blob the host can reach
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	STerrainMapArea* hostArea = host->GetLeader()->GetArea();
	auto it = std::find_if(nearBlobs.begin(), nearBlobs.end(), [&](const CEnemyManager::SEnemyBlob* b) {
		return terrainMgr->CanMoveToPos(hostArea, b->pos);
	});
	const CEnemyManager::SEnemyBlob* blob = (it != nearBlobs.end()) ? *it : nullptr;
	// Both with their group size bonus, as squads compare themselves with squad threat (a host of mostly area
	// damage doesn't count the enemies: IFighterTask::IsSquadThreat)
	const float strength = host->GetRallyStrength() * host->GetCountPowerMod();
	const float blobCount = ((blob != nullptr) && host->IsSquadThreat()) ? blob->count : 0.f;
	const float blobThreat = (blob != nullptr) ? blob->threat * circuit->GetThreatMap()->GetCountMod(blobCount) : 0.f;
	if (blob != nullptr) {
		rally.blobFrame = frame;
		rally.blobPos = blob->pos;
		rally.blobThreat = blobThreat;
	}

	if (!isRally) {
		if ((blob != nullptr) && (strength < cfg.match * blobThreat)) {
			rally.host = host;
			rally.startFrame = frame;
			circuit->LOG("%s start: host %.1f < %.2f x blob %.1f (%.1f enemies) at (%.0f, %.0f)", rally.tag, strength,
					cfg.match, blobThreat, blob->count, blob->pos.x, blob->pos.z);
		}
		return;
	}
	if (frame < rally.startFrame + RALLY_MIN_FRAMES) {
		return;
	}
	if (blob == nullptr) {
		if (frame - rally.blobFrame > cfg.seenFrames) {
			EndRally(rally, "blob lost", frame);
		}
	} else if (strength >= cfg.match * blobThreat * (1.f + cfg.margin)) {
		EndRally(rally, "host big enough", frame);
	}
}

IFighterTask* CMilitaryManager::EnqueueAttackBound(CCircuitUnit* unit)
{
	if (attackBaseGather) {
		return EnqueueDefend(IFighterTask::FightType::ATTACK, GetAttackGatherPower());
	}
	// No gathering at base: reinforce the rally host, else the nearest compatible group, else start a group
	IFighterTask* task = EnqueueReinforce(unit, 0.f);
	return (task != nullptr) ? task : EnqueueTask(IFighterTask::FightType::ATTACK);
}

IFighterTask* CMilitaryManager::EnqueueGuard(CCircuitUnit* vip)
{
	IFighterTask* task = new CFGuardTask(this, vip, 1.0f);
	fightTasks[static_cast<IFighterTask::FT>(IFighterTask::FightType::GUARD)].insert(task);
	fightUpdates.push_back(task);
	return task;
}

CRetreatTask* CMilitaryManager::EnqueueRetreat()
{
	CRetreatTask* task = new CRetreatTask(this);
	fightUpdates.push_back(task);
	return task;
}

void CMilitaryManager::DequeueTask(IUnitTask* task, bool done)
{
	switch (task->GetType()) {
		case IUnitTask::Type::FIGHTER: {
			IFighterTask* taskF = static_cast<IFighterTask*>(task);
			fightTasks[static_cast<IFighterTask::FT>(taskF->GetFightType())].erase(taskF);
			OnRallyTaskEnd(attackRally, taskF);
			OnRallyTaskEnd(raidRally, taskF);
			if (taskF->GetFightType() == IFighterTask::FightType::GUARD) {
				// A guard slot can abort itself (builder gone): drop its builder entry so nothing points at a dead task
				for (auto it = guardTasks.begin(); it != guardTasks.end();) {
					it = (it->second == taskF) ? guardTasks.erase(it) : std::next(it);
				}
			}
		} break;
		default: break;
	}
	task->Dead();
	task->Stop(done);
}

IUnitTask* CMilitaryManager::MakeTask(CCircuitUnit* unit)
{
	if (unit->IsCaptureHide()) {
		return EnqueueRetreat();  // hide at base until enough captured units are gone
	}
	return DefaultMakeTask(unit);
}

void CMilitaryManager::AbortTask(IUnitTask* task)
{
	DequeueTask(task, false);
}

void CMilitaryManager::DoneTask(IUnitTask* task)
{
	DequeueTask(task, true);
}

void CMilitaryManager::FallbackTask(CCircuitUnit* unit)
{
}

void CMilitaryManager::MakeDefence(const AIFloat3& pos)
{
	int index = circuit->GetMetalManager()->FindNearestCluster(pos);
	if (index >= 0) {
		MakeDefence(index, pos);
	}
}

void CMilitaryManager::MakeDefence(int cluster)
{
	MakeDefence(cluster, circuit->GetMetalManager()->GetClusters()[cluster].position);
}

void CMilitaryManager::MakeDefence(int cluster, const AIFloat3& pos)
{
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	if (terrainMgr->IsZoneAlly(pos)) {
		return;
	}

	CMetalManager* mm = circuit->GetMetalManager();
	CEconomyManager* em = circuit->GetEconomyManager();
	const float metalIncome = std::min(em->GetAvgMetalIncome(), em->GetAvgEnergyIncome()) * em->GetEcoFactor();
	float maxCost = MIN_BUILD_SEC * amountFactor * metalIncome;
	CDefenceMatrix::SDefPoint* closestPoint = nullptr;
	float minDist = std::numeric_limits<float>::max();
	std::vector<CDefenceMatrix::SDefPoint>& points = defence->GetDefPoints(cluster);
	for (CDefenceMatrix::SDefPoint& defPoint : points) {
		if (defPoint.cost < maxCost) {
			float dist = defPoint.position.SqDistance2D(pos);
			if ((closestPoint == nullptr) || (dist < minDist)) {
				closestPoint = &defPoint;
				minDist = dist;
			}
		}
	}
	if (closestPoint == nullptr) {
		return;
	}
	CBuilderManager* builderMgr = circuit->GetBuilderManager();
	float totalCost = .0f;
	IBuilderTask* parentTask = nullptr;
	// NOTE: circuit->GetTerrainManager()->IsWaterSector(pos) checks whole sector
	//       but water recognized as height < 0
	bool isWater = circuit->GetMap()->GetElevationAt(pos.x, pos.z) < -SQUARE_SIZE * 5;
	std::vector<CCircuitDef*>& defenders = isWater ? waterDefenders : landDefenders;

	// Front-line porc
	bool isPorc = mm->GetMaxIncome() > mm->GetAvgIncome() + 1.f;
	if (isPorc) {
		const float income = (mm->GetAvgIncome() + mm->GetMaxIncome()) * 0.5f;
		int spotId = mm->FindNearestSpot(pos);
		isPorc = mm->GetSpots()[spotId].income > income;
	}
	if (!isPorc) {
		unsigned threatCount = 0;
		CThreatMap* threatMap = circuit->GetThreatMap();
		const CMetalData::Clusters& clusters = mm->GetClusters();
		const CMetalData::Metals& spots = mm->GetSpots();
		const CMetalData::ClusterGraph& clusterGraph = mm->GetClusterGraph();
		CMetalData::ClusterGraph::Node node = clusterGraph.nodeFromId(cluster);
		CMetalData::ClusterGraph::IncEdgeIt edgeIt(clusterGraph, node);
		for (; edgeIt != lemon::INVALID; ++edgeIt) {
			int idx0 = clusterGraph.id(clusterGraph.oppositeNode(node, edgeIt));
			if (mm->IsClusterFinished(idx0)) {
				continue;
			}
			// check if there is enemy neighbor
			for (int idx : clusters[idx0].idxSpots) {
				if (threatMap->GetAllThreatAt(spots[idx].position) > THREAT_MIN * 2) {
					threatCount++;
					break;
				}
			}
			if (threatCount >= 2) {  // if 2 nearby clusters are a threat
				isPorc = true;
				break;
			}
		}
	}
	if (!isPorc) {
		for (IBuilderTask* t : builderMgr->GetTasks(IBuilderTask::BuildType::DEFENCE)) {
			if ((t->GetTarget() == nullptr) && (t->GetNextTask() != nullptr) &&
				(closestPoint->position.SqDistance2D(t->GetTaskPos()) < SQUARE(SQUARE_SIZE)))
			{
				builderMgr->AbortTask(t);
				break;
			}
		}
	}
	unsigned num = std::min<unsigned>(isPorc ? defenders.size() : preventCount, defenders.size());

	AIFloat3 backDir = circuit->GetSetupManager()->GetBasePos() - closestPoint->position;
	AIFloat3 backPos = closestPoint->position + backDir.Normalize2D() * SQUARE_SIZE * 16;
	CTerrainManager::CorrectPosition(backPos);

	CEnemyManager* enemyMgr = circuit->GetEnemyManager();
	const int frame = circuit->GetLastFrame();
	for (unsigned i = 0; i < num; ++i) {
		CCircuitDef* defDef = defenders[i];
		if (!defDef->IsAvailable(frame) || (defDef->IsRoleAA() && (enemyMgr->GetEnemyCost(ROLE_TYPE(AIR)) < 1.f))) {
			continue;
		}
		float defCost = defDef->GetCostM();
		totalCost += defCost;
		if (totalCost <= closestPoint->cost) {
			continue;
		}
		if (totalCost < maxCost) {
			closestPoint->cost += defCost;
			bool isFirst = (parentTask == nullptr);
			const AIFloat3& buildPos = defDef->IsAttacker() ? closestPoint->position : backPos;
			IBuilderTask* task = builderMgr->EnqueueTask(IBuilderTask::Priority::HIGH, defDef, buildPos,
					IBuilderTask::BuildType::DEFENCE, defCost, SQUARE_SIZE * 32, isFirst);
			if (parentTask != nullptr) {
				parentTask->SetNextTask(task);
			}
			parentTask = task;
		} else {
			// TODO: Auto-sort defenders by cost OR remove break?
			break;
		}
	}

	// Build sensors
	auto checkSensor = [this, &backPos, builderMgr](IBuilderTask::BuildType type, CCircuitDef* cdef, float range) {
		bool isBuilt = false;
		COOAICallback* clb = circuit->GetCallback();
		auto friendlies = clb->GetFriendlyUnitIdsIn(backPos, range);
		for (int auId : friendlies) {
			if (auId == -1) {
				continue;
			}
			CCircuitDef::Id defId = clb->Unit_GetDefId(auId);
			if (defId == cdef->GetId()) {
				isBuilt = true;
				break;
			}
		}
		if (!isBuilt) {
			const IBuilderTask* task = nullptr;
			const float qdist = SQUARE(range);
			for (const IBuilderTask* t : builderMgr->GetTasks(type)) {
				if (backPos.SqDistance2D(t->GetTaskPos()) < qdist) {
					task = t;
					break;
				}
			}
			if (task == nullptr) {
				builderMgr->EnqueueTask(IBuilderTask::Priority::NORMAL, cdef, backPos, type);
			}
		}
	};
	// radar
	if ((radarDef != nullptr) && radarDef->IsAvailable(frame) && (radarDef->GetCostM() < maxCost)) {
		const float range = radarDef->GetDef()->GetRadarRadius() / (isPorc ? 4.f : SQRT_2);
		checkSensor(IBuilderTask::BuildType::RADAR, radarDef, range);
	}
	// sonar
	if (isWater && (sonarDef != nullptr) && sonarDef->IsAvailable(frame) && (sonarDef->GetCostM() < maxCost)) {
		checkSensor(IBuilderTask::BuildType::SONAR, sonarDef, sonarDef->GetDef()->GetSonarRadius());
	}
}

void CMilitaryManager::AbortDefence(const CBDefenceTask* task)
{
	float defCost = task->GetBuildDef()->GetCostM();
	CDefenceMatrix::SDefPoint* point = defence->GetDefPoint(task->GetPosition(), defCost);
	if (point != nullptr) {
		if ((task->GetTarget() == nullptr) && (point->cost >= defCost)) {
			point->cost -= defCost;
		}
		IBuilderTask* next = task->GetNextTask();
		while (next != nullptr) {
			if (next->GetBuildDef() != nullptr) {
				defCost = next->GetBuildDef()->GetCostM();
			} else{
				defCost = next->GetCost();
			}
			if (point->cost >= defCost) {
				point->cost -= defCost;
			}
			next = next->GetNextTask();
		}
	}
}

bool CMilitaryManager::HasDefence(int cluster)
{
	const std::vector<CDefenceMatrix::SDefPoint>& points = defence->GetDefPoints(cluster);
	for (const CDefenceMatrix::SDefPoint& defPoint : points) {
		if (defPoint.cost > .5f) {
			return true;
		}
	}
	return false;
}

AIFloat3 CMilitaryManager::GetScoutPosition(CCircuitUnit* unit)
{
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	CMetalManager* metalMgr = circuit->GetMetalManager();
	STerrainMapArea* area = unit->GetArea();
//	const AIFloat3& pos = unit->GetPos(circuit->GetLastFrame());
//	const float minSqRange = SQUARE(unit->GetCircuitDef()->GetLosRadius());
	const CMetalData::Metals& spots = metalMgr->GetSpots();
	decltype(scoutIdx) prevIdx = scoutIdx;
	while (scoutIdx < scoutPath.size()) {
		int index = scoutPath[scoutIdx++];
		if (!metalMgr->IsMexInFinished(index)
			&& terrainMgr->CanMoveToPos(area, spots[index].position)
			/*&& (pos.SqDistance2D(spots[index].position) > minSqRange)*/)
		{
			return spots[index].position;
		}
	}
	scoutIdx = 0;
	while (scoutIdx < prevIdx) {
		int index = scoutPath[scoutIdx++];
		if (!metalMgr->IsMexInFinished(index)
			&& terrainMgr->CanMoveToPos(area, spots[index].position)
			/*&& (pos.SqDistance2D(spots[index].position) > minSqRange)*/)
		{
			return spots[index].position;
		}
	}
//	++scoutIdx %= scoutPath.size();
	return -RgtVector;
}

void CMilitaryManager::UpdateScoutSpots()
{
	const CMetalData::Metals& metals = circuit->GetMetalManager()->GetSpots();
	const std::vector<AIFloat3>& geos = circuit->GetEconomyManager()->GetGeoSpots();
	if ((scoutMetalCount == metals.size()) && (scoutSpots.size() == metals.size() + geos.size())) {
		return;
	}
	if (scoutMetalCount != metals.size()) {  // metal spots changed (not expected mid-game): start over
		scoutSpots.clear();
		scoutSeen.clear();
		scoutClaims.Clear();
		raidClaims.Clear();
		for (const CMetalData::SMetal& m : metals) {
			scoutSpots.push_back(m.position);
		}
		scoutMetalCount = metals.size();
	}
	// New vents only ever get appended, so the per-spot history of earlier spots is kept
	for (size_t i = scoutSpots.size() - scoutMetalCount; i < geos.size(); ++i) {
		scoutSpots.push_back(geos[i]);
	}
	scoutSeen.resize(scoutSpots.size(), -1);
	scoutClaims.Resize(scoutSpots.size());
	raidClaims.Resize(scoutSpots.size());
}

bool CMilitaryManager::IsScoutSpotOurs(unsigned idx) const
{
	if (idx < scoutMetalCount) {
		return circuit->GetMetalManager()->IsMexInFinished(idx);
	}
	bool isOurs = false;
	auto units = circuit->GetCallback()->GetFriendlyUnitsIn(scoutSpots[idx], SQUARE_SIZE * 4);
	for (springai::Unit* u : units) {
		if (u == nullptr) {
			continue;
		}
		CAllyUnit* ally = circuit->GetFriendlyUnit(u);
		if ((ally != nullptr) && circuit->GetEconomyManager()->IsGeoDef(ally->GetCircuitDef()) && !u->IsBeingBuilt()) {
			isOurs = true;
			break;
		}
	}
	utils::free_clear(units);
	return isOurs;
}

AIFloat3 CMilitaryManager::PickSpot(CCircuitUnit* unit, float safeThreat, const SSpotCfg& cfg, IFighterTask::FightType type,
		SSpotClaims& claims)
{
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	CMapManager* mapMgr = circuit->GetMapManager();
	CThreatMap* threatMap = circuit->GetThreatMap();
	CCircuitDef* cdef = unit->GetCircuitDef();
	STerrainMapArea* area = unit->GetArea();
	UpdateScoutSpots();
	const std::vector<AIFloat3>& spots = scoutSpots;
	const int frame = circuit->GetLastFrame();
	const AIFloat3& pos = unit->GetPos(frame);
	// A picker holds one claim: release its previous spot so re-picks don't reserve spots for nobody
	const ICoreUnit::Id unitId = unit->GetId();
	ReleaseSpot(claims, unitId);
	const AIFloat3& basePos = circuit->GetSetupManager()->GetBasePos();
	const float mapDiag = std::max(sqrtf(SQUARE(terrainMgr->GetTerrainWidth()) + SQUARE(terrainMgr->GetTerrainHeight())), 1.f);

	// Danger at a destination comes from known static defences only: mobile units will have moved by the
	// time the picker arrives, and the route/own-spot checks of its task handle them on the way.
	const bool isAir = cdef->IsAbleToFly();
	const bool isAmph = !isAir && cdef->IsAmphibious();
	struct SDefence { AIFloat3 pos; float sqRange; float range; float threat; float count; };
	std::vector<SDefence> defences;
	// Points that mark where the enemy is: known enemy buildings and enemy groups
	std::vector<AIFloat3> anchors;
	for (auto& kv : circuit->GetEnemyInfos()) {
		CEnemyInfo* enemy = kv.second;
		CCircuitDef* edef = enemy->GetCircuitDef();
		if (enemy->IsHidden() || (edef == nullptr) || edef->IsMobile()) {
			continue;
		}
		anchors.push_back(enemy->GetPos());
		const float threat = enemy->GetThreat();
		const CEnemyUnit* data = enemy->GetData();
		const int rangeSq = isAir ? data->GetRange(CCircuitDef::ThreatType::AIR)
				: isAmph ? std::max(data->GetRange(CCircuitDef::ThreatType::LAND), data->GetRange(CCircuitDef::ThreatType::WATER))
				: data->GetRange(CCircuitDef::ThreatType::LAND);
		const float range = rangeSq * threatMap->GetSquareSize() * cfg.rangeMult;
		if ((threat > 0.f) && (range > 0.f) && !enemy->IsBeingBuilt()) {
			defences.push_back({enemy->GetPos(), SQUARE(range), range, threat, data->GetCountWeight()});
		}
	}
	for (const CEnemyManager::SEnemyGroup& group : circuit->GetEnemyManager()->GetEnemyGroups()) {
		if (!group.units.empty()) {
			anchors.push_back(group.pos);
		}
	}
	if (anchors.empty()) {
		// Nothing known yet: guess the enemy start is our start mirrored through the map centre
		const AIFloat3& start = circuit->GetSetupManager()->GetStartPos();
		anchors.push_back(AIFloat3(terrainMgr->GetTerrainWidth() - start.x, start.y, terrainMgr->GetTerrainHeight() - start.z));
	}
	// A raid group's safeThreat has its group size bonus: the defences get theirs too, as squad threat counts them
	const bool isSquad = IFighterTask::IsSquadThreat(unit);
	auto isDefended = [&](const AIFloat3& spot) {
		float danger = 0.f;
		float count = 0.f;
		for (const SDefence& d : defences) {
			const float sqDist = d.pos.SqDistance2D(spot);
			if (sqDist < d.sqRange) {
				danger += d.threat * (1.f - 0.5f * sqrtf(sqDist) / d.range);  // same falloff as the threat map
				count += d.count;
			}
		}
		if (isSquad) {
			danger *= threatMap->GetCountMod(count);
		}
		return danger >= safeThreat / GetThrDistMod(type, spot);  // thr_mod.distance.<scout|raid> at the spot
	};

	// Best unclaimed spot, else the best claimed one (every safe spot is taken: share it)
	int bestIdx[2] = {-1, -1};  // [isClaimed]
	float bestScore[2] = {-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};
	for (unsigned i = 0; i < spots.size(); ++i) {
		const AIFloat3& spot = spots[i];
		if (mapMgr->IsInLOS(spot)) {
			scoutSeen[i] = frame;
			continue;  // already watched
		}
		if ((frame < claims.blocked[i]) || !terrainMgr->CanMoveToPos(area, spot)) {
			continue;
		}

		float minSqAnchor = std::numeric_limits<float>::max();
		for (const AIFloat3& a : anchors) {
			minSqAnchor = std::min(minSqAnchor, a.SqDistance2D(spot));
		}
		const float anchorDist = sqrtf(minSqAnchor);
		if (basePos.distance2D(spot) < cfg.homeRatio * anchorDist) {
			continue;  // our side of the map: much nearer our base than any sign of the enemy
		}
		const float enemyTerm = 1.f - std::min(anchorDist / mapDiag, 1.f);
		const float staleTerm = (scoutSeen[i] < 0) ? 1.f
				: std::min(float(frame - scoutSeen[i]) / std::max(cfg.staleTime, 1), 1.f);
		const float travelTerm = pos.distance2D(spot) / mapDiag;
		const float score = cfg.enemy * enemyTerm + cfg.stale * staleTerm - cfg.travel * travelTerm;

		const int c = ((claims.frame[i] >= 0) && (frame < claims.frame[i] + cfg.claimTime)) ? 1 : 0;
		// The costlier checks only for a spot that would become the best: same pick, far fewer checks
		if ((score <= bestScore[c]) || IsScoutSpotOurs(i) || isDefended(spot)) {
			continue;  // not better, our territory, or defended
		}
		bestScore[c] = score;
		bestIdx[c] = i;
	}
	const int idx = (bestIdx[0] >= 0) ? bestIdx[0] : bestIdx[1];
	if (idx < 0) {
		return -RgtVector;
	}
	claims.frame[idx] = frame;
	claims.unitId[idx] = unitId;
	return spots[idx];
}

void CMilitaryManager::ReleaseSpot(SSpotClaims& claims, int unitId)
{
	for (unsigned i = 0; i < claims.unitId.size(); ++i) {
		if (claims.unitId[i] == unitId) {
			claims.frame[i] = -1;
			claims.unitId[i] = -1;
		}
	}
}

void CMilitaryManager::ReleaseRaidSpot(const CCircuitUnit* leader)
{
	ReleaseSpot(raidClaims, leader->GetId());
}

void CMilitaryManager::BlockSpot(SSpotClaims& claims, const AIFloat3& spot, int frames)
{
	if (claims.blocked.size() != scoutSpots.size()) {
		return;
	}
	for (unsigned i = 0; i < scoutSpots.size(); ++i) {
		if ((scoutSpots[i].x == spot.x) && (scoutSpots[i].z == spot.z)) {
			claims.blocked[i] = circuit->GetLastFrame() + frames;
			return;
		}
	}
}

bool CMilitaryManager::IsRaider(const CCircuitDef* cdef) const
{
	// Same test DefaultMakeTask uses to send a unit raiding: its main role, or the base role it is bound to
	return circuit->GetBindedRole(cdef->GetMainRole()) == ROLE_TYPE(RAIDER);
}

unsigned int CMilitaryManager::GetRaiderCount() const
{
	unsigned int count = 0;
	for (CCircuitUnit* unit : army) {
		if (!IsRaider(unit->GetCircuitDef())) {
			continue;
		}
		// Raiders taken as scouts (quota.scout) don't raid, so they don't fill the floor
		IUnitTask* task = unit->GetTask();
		if ((task != nullptr) && (task->GetType() == IUnitTask::Type::FIGHTER)
			&& (static_cast<IFighterTask*>(task)->GetFightType() == IFighterTask::FightType::SCOUT))
		{
			continue;
		}
		++count;
	}
	return count;
}

void CMilitaryManager::UpdateScoutQuota()
{
	SCOPED_TIME(circuit, __PRETTY_FUNCTION__);
	// Same count twice: for scouts (scout-capable units, one scout's courage) and for raiders (raider units,
	// a starting raid group's courage, quota.raid[0]); each only when its *_per_mex is on
	UpdateMexBonus(scoutQuota, false);
	UpdateMexBonus(raiderQuota, true);
}

void CMilitaryManager::UpdateMexBonus(SUnitQuota& quota, bool isRaider)
{
	std::map<int, unsigned int>& mexBonus = quota.mexBonus;
	if (quota.perMex <= 0.f) {
		mexBonus.clear();
		return;
	}
	const IFighterTask::FightType fightType = isRaider ? IFighterTask::FightType::RAID : IFighterTask::FightType::SCOUT;
	auto isCapable = [this, isRaider](const CCircuitDef* cdef) {
		return isRaider ? IsRaider(cdef) : cdef->IsRoleScout();
	};
	// One capable unit per movement type stands in for that type (its move map and threat layer;
	// the bottleneck query never uses the cloak layer, which is on a different scale).
	std::map<int, CCircuitUnit*> refUnits;
	for (CCircuitUnit* unit : army) {
		CCircuitDef* cdef = unit->GetCircuitDef();
		if (!isCapable(cdef) || (unit->GetTask()->GetType() == IUnitTask::Type::PLAYER)) {
			continue;
		}
		auto it = refUnits.find(cdef->GetMobileId());
		if (it == refUnits.end()) {
			refUnits[cdef->GetMobileId()] = unit;
		} else if ((unit->GetTask()->GetType() == IUnitTask::Type::FIGHTER)
			&& (static_cast<IFighterTask*>(unit->GetTask())->GetFightType() == fightType)
			&& !((it->second->GetTask()->GetType() == IUnitTask::Type::FIGHTER)
				&& (static_cast<IFighterTask*>(it->second->GetTask())->GetFightType() == fightType)))
		{
			it->second = unit;  // a unit already on that task is the best stand-in
		}
	}
	// Only movement types we actually have a capable unit of get a bonus
	for (auto it = mexBonus.begin(); it != mexBonus.end();) {
		it = (refUnits.find(it->first) == refUnits.end()) ? mexBonus.erase(it) : std::next(it);
	}

	// All players: enemy teams (engine count, minus Gaia) plus our ally team's players that haven't resigned
	const int enemyPlayers = std::max(circuit->GetEnemyTeamSize(), 1);
	const int players = std::max(circuit->GetEnemyTeamSize(), 0) + std::max(circuit->GetAllyTeam()->GetAliveSize(), 1);
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	CPathFinder* pathfinder = circuit->GetPathfinder();
	CThreatMap* threatMap = circuit->GetThreatMap();
	const int frame = circuit->GetLastFrame();
	const AIFloat3& basePos = circuit->GetSetupManager()->GetBasePos();
	// Average of the random powerMod the task gets (0.75 / thr_mod.scout or thr_mod.raid)
	const SThreatQuota& thrMod = isRaider ? raidMod : scoutMod;
	const float powerMod = 0.75f / std::max(thrMod.min + 0.5f * thrMod.len, 1e-3f);
	// Second multiplier: map size curve / all players. Size = side of a square map with the same area,
	// in Spring map units (512 elmos), so 8x8 -> 8 and a 10x16 map -> ~12.6
	const float mapSize = sqrtf(float(terrainMgr->GetTerrainWidth()) * terrainMgr->GetTerrainHeight()) / 512.f;
	const float mapMult = utils::eval_curve(quota.mapCurve, mapSize) / players;
	const float perMex = quota.perMex;

	for (auto& kv : refUnits) {
		const int mobileId = kv.first;
		CCircuitUnit* unit = kv.second;
		CCircuitDef* cdef = unit->GetCircuitDef();
		// Fresh unit of this type (scouts, same scale as ScoutTask::GetSafeThreat), or a starting raid group
		// of quota.raid[0] power (raiders, as CRaidTask::GetSafeThreat)
		// with the group size bonus for about that many units of the type (scouts are judged alone, without it)
		const float power = isRaider ? std::max(cdef->GetPower(), raid.min) : cdef->GetPower();
		const float count = isRaider ? std::ceil(power / std::max(cdef->GetPower(), 1e-3f)) : 0.f;
		const float safeThreat = std::max(power, THREAT_MIN) * powerMod * threatMap->GetCountMod(count);
		AIFloat3 startPos = basePos;
		if ((unit->GetArea() != nullptr) && !terrainMgr->CanMoveToPos(unit->GetArea(), startPos)) {
			startPos = unit->GetPos(frame);  // our base is not in its area (e.g. ships): flood from where it is
		}
		CTerrainManager::CorrectPosition(startPos);

		// A group of this type judges on squad threat unless it is area damage (IFighterTask::IsSquadThreat)
		std::shared_ptr<IPathQuery> query = pathfinder->CreateCostMapQuery(unit, threatMap, frame, startPos, true,
				isRaider && !IsAreaDamage(cdef));
		// The quota is a member: it outlives the query (a pointer, so that is plain to see)
		std::map<int, unsigned int>* bonus = &mexBonus;
		pathfinder->RunQuery(query, [this, bonus, fightType, perMex, mobileId, safeThreat, enemyPlayers, mapMult](const IPathQuery* query) {
			const CQueryCostMap* q = static_cast<const CQueryCostMap*>(query);
			const int sqSize = circuit->GetPathfinder()->GetSquareSize();
			unsigned int count = 0;
			for (auto& kv : circuit->GetEnemyInfos()) {
				CEnemyInfo* enemy = kv.second;
				CCircuitDef* edef = enemy->GetCircuitDef();
				if ((edef == nullptr) || !edef->IsMex() || enemy->IsHidden()) {
					continue;
				}
				AIFloat3 pos = enemy->GetPos();
				CTerrainManager::CorrectPosition(pos);
				// Least possible "highest threat crossed" from our base to the mex; -1 = no route for this movement type
				float bottleneck = q->GetCostAtSquare(pos);  // the mex's own square
				if (bottleneck < 0.f) {
					bottleneck = q->GetCostAt(pos, sqSize);  // blocked by the mex itself: the squares around it
				}
				if ((bottleneck >= 0.f)
					&& (bottleneck - THREAT_BASE < safeThreat / GetThrDistMod(fightType, pos)))
				{
					++count;
				}
			}
			// (reachable mexes / enemy players) x (map size curve / all players) x *_per_mex, rounded down
			(*bonus)[mobileId] = (unsigned int)(perMex * mapMult * count / enemyPlayers);
		});
	}
}

float CMilitaryManager::GetBaseThrBonus(float sqDistToBase) const
{
	const float baseRange = GetBaseDefRange();
	if ((baseRange <= 0.f) || (sqDistToBase >= SQUARE(baseRange))) {
		return 1.f;
	}
	return 1.f + (baseThrBonus - 1.f) * (1.f - sqrtf(sqDistToBase) / baseRange);
}

float CMilitaryManager::GetThrDistMod(IFighterTask::FightType type, const AIFloat3& pos) const
{
	const ThrDistCurve& thrDistCurve = thrDistCurves[static_cast<IFighterTask::FT>(type)];
	if (thrDistCurve.empty()) {
		return 1.f;
	}
	const float frac = circuit->GetSetupManager()->GetBasePos().distance2D(pos) * invMapDiagonal;
	return utils::eval_curve(thrDistCurve, frac);
}

void CMilitaryManager::GetTakeableContested(CCircuitUnit* unit, float reach,
		const std::function<float (const AIFloat3&)>& allowedAt, std::vector<SContestedSpot>& outSpots)
{
	outSpots.clear();
	if (contested.bonus <= 1.f) {
		return;
	}
	CMetalManager* metalMgr = circuit->GetMetalManager();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	CThreatMap* threatMap = circuit->GetThreatMap();
	const CMetalData::Clusters& clusters = metalMgr->GetClusters();
	for (int index : metalMgr->GetContestedClusters(circuit->GetLastFrame(), contested.maxAge)) {
		const AIFloat3& cPos = clusters[index].position;
		// Live threat on this unit's layer: the mark is from the builders' walk, maybe seconds ago. Still contested:
		// above the builders' wall (plain threat, as CMetalManager judges it). Takeable: below the group's allowance,
		// on the threat the group judges with
		const float threat = threatMap->GetThreatAt(unit, cPos);
		const float taskThreat = IFighterTask::IsSquadThreat(unit) ? threatMap->GetSquadThreatAt(unit, cPos) : threat;
		if ((threat <= metalMgr->GetWallThreat()) || (taskThreat >= allowedAt(cPos))
			|| !terrainMgr->CanMobileReachAt(unit->GetArea(), cPos, reach))
		{
			continue;
		}
		outSpots.push_back({cPos, SQUARE(clusters[index].radius + contested.range)});
	}
}

AIFloat3 CMilitaryManager::GetRaidPosition(CCircuitUnit* unit)
{
	// FIXME: Resume. Not well thought, not finished.
	return GetScoutPosition(unit);
	// FIXME: Resume. Not well thought, not finished.

//	const CMetalData::Clusters& clusters = circuit->GetMetalManager()->GetClusters();
//	const AIFloat3& pos = unit->GetPos(circuit->GetLastFrame());
//	STerrainMapArea* area = unit->GetArea();
//	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
//	CThreatMap* threatMap = circuit->GetThreatMap();
//	threatMap->SetThreatType(unit);  // TODO: Check if required? Upper function may already call it
//	float bestWeight = -1.f;
//	float sqBestDist = std::numeric_limits<float>::max();
//	int bestIndex = -1;
//	for (size_t index = 0; index < raidPath.size(); ++index) {
//		if (!terrainMgr->CanMoveToPos(area, clusters[index].position)) {
//			continue;
//		}
//		const SRaidPoint& rp = raidPath[index];
//		float weight = rp.weight / (threatMap->GetThreatAt(clusters[index].position) + 1.f);
//		if (bestWeight < weight) {
//			bestWeight = weight;
//			bestIndex = index;
//			sqBestDist = pos.SqDistance2D(clusters[index].position);
//		} else if (rp.weight == bestWeight) {
//			float sqDist = pos.SqDistance2D(clusters[index].position);
//			if (sqBestDist > sqDist) {
//				sqBestDist = sqDist;
//				bestWeight = weight;
//				bestIndex = index;
//			}
//		}
//	}
//	return (bestIndex != -1) ? clusters[bestIndex].position : AIFloat3(-RgtVector);
}

void CMilitaryManager::FillFrontPos(CCircuitUnit* unit, F3Vec& outPositions)
{
	outPositions.clear();

	CInfluenceMap* inflMap = circuit->GetInflMap();
	CMetalManager* metalMgr = circuit->GetMetalManager();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	STerrainMapArea* area = unit->GetArea();
	const CMetalData::Clusters& clusters = metalMgr->GetClusters();

	CMetalData::PointPredicate predicate = [inflMap, metalMgr, terrainMgr, area, &clusters](const int index) {
		return ((inflMap->GetInfluenceAt(clusters[index].position) > -INFL_EPS)
			&& (metalMgr->IsClusterQueued(index) || metalMgr->IsClusterFinished(index))
			&& terrainMgr->CanMoveToPos(area, clusters[index].position));
	};

	CSetupManager* setupMgr = circuit->GetSetupManager();
	int index = metalMgr->FindNearestCluster(setupMgr->GetLanePos(), predicate);

	if (index >= 0) {
		const std::vector<CDefenceMatrix::SDefPoint>& points = defence->GetDefPoints(index);
		for (const CDefenceMatrix::SDefPoint& defPoint : points) {
			outPositions.push_back(defPoint.position);
		}
	}
}

void CMilitaryManager::FillFriendlyClusterPos(CCircuitUnit* unit, F3Vec& outPositions)
{
	outPositions.clear();

	CInfluenceMap* inflMap = circuit->GetInflMap();
	CMetalManager* metalMgr = circuit->GetMetalManager();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	STerrainMapArea* area = unit->GetArea();
	const CMetalData::Clusters& clusters = metalMgr->GetClusters();
	for (int index = 0; index < (int)clusters.size(); ++index) {
		// Same test as FillFrontPos
		if ((inflMap->GetInfluenceAt(clusters[index].position) > -INFL_EPS)
			&& (metalMgr->IsClusterQueued(index) || metalMgr->IsClusterFinished(index))
			&& terrainMgr->CanMoveToPos(area, clusters[index].position))
		{
			for (const CDefenceMatrix::SDefPoint& defPoint : defence->GetDefPoints(index)) {
				outPositions.push_back(defPoint.position);
			}
		}
	}
}

void CMilitaryManager::FillAttackSafePos(CCircuitUnit* unit, F3Vec& outPositions)
{
	outPositions.clear();

	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	const int frame = circuit->GetLastFrame();

	STerrainMapArea* area = unit->GetArea();

	const std::array<IFighterTask::FightType, 2> types = {IFighterTask::FightType::ATTACK, IFighterTask::FightType::DEFEND};
	for (IFighterTask::FightType type : types) {
		const std::set<IFighterTask*>& atkTasks = GetTasks(type);
		for (IFighterTask* task : atkTasks) {
			const AIFloat3& ourPos = static_cast<ISquadTask*>(task)->GetLeaderPos(frame);
			if (terrainMgr->CanMoveToPos(area, ourPos)) {
				outPositions.push_back(ourPos);
			}
		}
	}
}

void CMilitaryManager::FillStaticSafePos(CCircuitUnit* unit, F3Vec& outPositions)
{
	outPositions.clear();

	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	const int frame = circuit->GetLastFrame();

	const AIFloat3& startPos = unit->GetPos(frame);
	STerrainMapArea* area = unit->GetArea();

	CDefenceMatrix* defMat = defence;
	CMetalData::PointPredicate predicate = [defMat, terrainMgr, area](const int index) {
		const std::vector<CDefenceMatrix::SDefPoint>& points = defMat->GetDefPoints(index);
		for (const CDefenceMatrix::SDefPoint& defPoint : points) {
			if ((defPoint.cost > 100.0f) && terrainMgr->CanMoveToPos(area, defPoint.position)) {
				return true;
			}
		}
		return false;
	};

	CMetalManager* metalMgr = circuit->GetMetalManager();
	int index = metalMgr->FindNearestCluster(startPos, predicate);

	if (index >= 0) {
		const std::vector<CDefenceMatrix::SDefPoint>& points = defence->GetDefPoints(index);
		for (const CDefenceMatrix::SDefPoint& defPoint : points) {
			outPositions.push_back(defPoint.position);
		}
	}
}

void CMilitaryManager::FillSafePos(CCircuitUnit* unit, F3Vec& outPositions)
{
	outPositions.clear();

	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	const int frame = circuit->GetLastFrame();

	const springai::AIFloat3& pos = unit->GetPos(frame);
	STerrainMapArea* area = unit->GetArea();

	const std::array<IFighterTask::FightType, 2> types = {IFighterTask::FightType::ATTACK, IFighterTask::FightType::DEFEND};
	for (IFighterTask::FightType type : types) {
		const std::set<IFighterTask*>& atkTasks = GetTasks(type);
		for (IFighterTask* task : atkTasks) {
			const AIFloat3& ourPos = static_cast<ISquadTask*>(task)->GetLeaderPos(frame);
			if (terrainMgr->CanMoveToPos(area, ourPos)) {
				outPositions.push_back(ourPos);
			}
		}
		if (!outPositions.empty()) {
			return;
		}
	}

	CDefenceMatrix* defMat = defence;
	CMetalData::PointPredicate predicate = [defMat, terrainMgr, area](const int index) {
		const std::vector<CDefenceMatrix::SDefPoint>& points = defMat->GetDefPoints(index);
		for (const CDefenceMatrix::SDefPoint& defPoint : points) {
			if ((defPoint.cost > 100.0f) && terrainMgr->CanMoveToPos(area, defPoint.position)) {
				return true;
			}
		}
		return false;
	};
	CMetalManager* metalMgr = circuit->GetMetalManager();
	int index = metalMgr->FindNearestCluster(pos, predicate);
	if (index >= 0) {
		const std::vector<CDefenceMatrix::SDefPoint>& points = defence->GetDefPoints(index);
		for (const CDefenceMatrix::SDefPoint& defPoint : points) {
			outPositions.push_back(defPoint.position);
		}
	}

	if (outPositions.empty()) {
		outPositions.push_back(circuit->GetSetupManager()->GetBasePos());
	}
}

IFighterTask* CMilitaryManager::AddGuardTask(CCircuitUnit* unit)
{
	auto it = guardTasks.find(unit);
	if (it != guardTasks.end()) {
		return it->second;
	}

	IFighterTask* task = EnqueueGuard(unit);
	guardTasks[unit] = task;
	return task;
}

bool CMilitaryManager::DelGuardTask(CCircuitUnit* unit)
{
	auto it = guardTasks.find(unit);
	if (it == guardTasks.end()) {
		return false;
	}

	IFighterTask* task = it->second;
	guardTasks.erase(it);  // erase first: DequeueTask also scrubs guardTasks
	AbortTask(task);
	return true;
}

IFighterTask* CMilitaryManager::GetGuardTask(CCircuitUnit* unit) const
{
	auto it = guardTasks.find(unit);
	return (it != guardTasks.end()) ? it->second : nullptr;
}

bool CMilitaryManager::IsGuardRole(CCircuitDef* cdef) const
{
	if (cdef->IsAttrGuarded()) {  // guarded units are protected, never protectors
		return false;
	}
	// Match the main role by its own name ("bullshit_raider") or by the role it acts as ("raider")
	const CCircuitDef::RoleT mainRole = cdef->GetMainRole();
	const CCircuitDef::RoleM mask = CCircuitDef::GetMask(mainRole)
			| CCircuitDef::GetMask(circuit->GetBindedRole(mainRole));
	return (guard.roleMask & mask) != 0;
}

bool CMilitaryManager::IsGuardCapReached(CCircuitDef* cdef, bool isGuardedPool) const
{
	// Two pools: builder slots and "guarded"-unit slots, so builders can't use up the whole share
	const float maxShare = isGuardedPool ? guard.guardedMaxShare : guard.maxShare;
	if (maxShare >= 1.f) {
		return false;
	}
	// Per role: e.g. 0.3 = at most 30% of raiders and 30% of riots guard at once (rounded up, so 1 raider may guard)
	const CCircuitDef::RoleT role = circuit->GetBindedRole(cdef->GetMainRole());
	unsigned int total = 0;
	for (CCircuitUnit* u : army) {
		if (u->GetCircuitDef()->IsAttrGuarded()) {  // can never guard, so not part of the share
			continue;
		}
		if (circuit->GetBindedRole(u->GetCircuitDef()->GetMainRole()) == role) {
			++total;
		}
	}
	unsigned int guarding = 0;
	for (IFighterTask* t : GetTasks(IFighterTask::FightType::GUARD)) {
		if (static_cast<CFGuardTask*>(t)->IsGuardedPool() != isGuardedPool) {
			continue;
		}
		for (CCircuitUnit* u : t->GetAssignees()) {
			if (circuit->GetBindedRole(u->GetCircuitDef()->GetMainRole()) == role) {
				++guarding;
			}
		}
	}
	// -1e-3: 0.3f * 10 is 3.0000001f in float, which would ceil to 4
	return guarding >= std::ceil(maxShare * total - 1e-3f);
}

void CMilitaryManager::BanGuard(CCircuitUnit* unit, int frames)
{
	const int frame = circuit->GetLastFrame();
	// Drop expired entries (and dead units) so the map stays small
	for (auto it = guardBanUntil.begin(); it != guardBanUntil.end();) {
		it = (it->second <= frame) ? guardBanUntil.erase(it) : std::next(it);
	}
	guardBanUntil[unit->GetId()] = frame + frames;
}

bool CMilitaryManager::IsGuardBanned(CCircuitUnit* unit) const
{
	auto it = guardBanUntil.find(unit->GetId());
	return (it != guardBanUntil.end()) && (circuit->GetLastFrame() < it->second);
}

IFighterTask* CMilitaryManager::FindGuardTask(CCircuitUnit* unit) const
{
	// Nearest eligible slot, not the first one in the set (which is ordered by pointer)
	const int frame = circuit->GetLastFrame();
	const AIFloat3& pos = unit->GetPos(frame);
	IFighterTask* best = nullptr;
	float minSqDist = std::numeric_limits<float>::max();
	for (IFighterTask* t : GetTasks(IFighterTask::FightType::GUARD)) {
		if (!t->CanAssignTo(unit)) {
			continue;
		}
		CCircuitUnit* vip = circuit->GetTeamUnit(static_cast<CFGuardTask*>(t)->GetVipId());
		const float sqDist = (vip != nullptr) ? pos.SqDistance2D(vip->GetPos(frame)) : minSqDist;
		if ((best == nullptr) || (sqDist < minSqDist)) {
			minSqDist = sqDist;
			best = t;
		}
	}
	return best;
}

void CMilitaryManager::AddResponse(CCircuitUnit* unit)
{
	const CCircuitDef* cdef = unit->GetCircuitDef();
	const float cost = cdef->GetCostM();
	const CCircuitDef::RoleT roleSize = CCircuitDef::GetRoleNames().size();
	assert(roleInfos.size() == roleSize);
	for (CCircuitDef::RoleT type = 0; type < roleSize; ++type) {
		if (cdef->IsRespRoleAny(CCircuitDef::GetMask(type))) {
			roleInfos[type].cost += cost;
			roleInfos[type].units.insert(unit);
		}
	}
}

void CMilitaryManager::DelResponse(CCircuitUnit* unit)
{
	const CCircuitDef* cdef = unit->GetCircuitDef();
	const float cost = cdef->GetCostM();
	const CCircuitDef::RoleT roleSize = CCircuitDef::GetRoleNames().size();
	assert(roleInfos.size() == roleSize);
	for (CCircuitDef::RoleT type = 0; type < roleSize; ++type) {
		if (cdef->IsRespRoleAny(CCircuitDef::GetMask(type))) {
			float& metal = roleInfos[type].cost;
			metal = std::max(metal - cost, .0f);
			roleInfos[type].units.erase(unit);
		}
	}
}

bool CMilitaryManager::IsRoleAllowed(const CCircuitDef* cdef) const
{
	return roleInfos[cdef->GetMainRole()].isAllowed;
}

void CMilitaryManager::UpdateResponseGates()
{
	// Territory share: finished ally mexes over all map spots (no spots, e.g. a metal map: territory checks pass)
	CMetalManager* metalMgr = circuit->GetMetalManager();
	metalMgr->MarkAllyMexes();
	const int spotCount = metalMgr->GetSpots().size();
	const bool isTerritory = (spotCount > 0);
	const float share = isTerritory ? (float)metalMgr->GetMexCount() / spotCount : 0.f;
	// Income as factory income_tier reads it: economy eps_step (ecoFactor) included
	CEconomyManager* economyMgr = circuit->GetEconomyManager();
	const float energyIncome = economyMgr->GetAvgEnergyIncome();
	const float income = std::min(economyMgr->GetAvgMetalIncome(), energyIncome) * economyMgr->GetEcoFactor();
	// unlock_after reads the same without reclaim (a wreck field must not unlock a role for good): our share of the
	// ally team's mex, overdrive and constructor metal, from ZK's overdrive gadget; total income without it
	const float ecoMetal = economyMgr->GetEcoMetalIncome();
	const float unlockIncome = std::min(ecoMetal, energyIncome) * economyMgr->GetEcoFactor();
	const int frame = circuit->GetLastFrame();

	for (SRoleInfo& info : roleInfos) {
		info.incomeMult = utils::eval_curve(info.incomeCurve, income);

		// unlock_after: one-off, both exceeded on RESPONSE_HOLD samples in a row (not a transfer spike)
		bool isJustUnlocked = false;
		if (!info.isUnlocked) {
			const bool isIncome = (info.unlockIncome <= 0.f) || (unlockIncome > info.unlockIncome);
			const bool isShare = !isTerritory || (info.unlockTerritory <= 0.f) || (share > info.unlockTerritory);
			info.unlockHold = (isIncome && isShare) ? info.unlockHold + 1 : 0;
			if (info.unlockHold >= RESPONSE_HOLD) {
				info.isUnlocked = isJustUnlocked = true;
				circuit->LOG("RESPONSE: %s unlocked at frame %i (income %.1f > %.1f, territory %.2f > %.2f)",
						info.name.c_str(), frame, unlockIncome, info.unlockIncome, share, info.unlockTerritory);
			}
		}

		// Live gate, debounced: the wanted state must hold RESPONSE_HOLD samples in a row; an unlock (held already)
		// opens it straight away. Starting states (share 0) need no first sample: open unless locked
		const bool isWant = info.isUnlocked && (!isTerritory || (share <= info.territoryMax));
		if (isWant == info.isAllowed) {
			info.flipHold = 0;
		} else if (isJustUnlocked || (++info.flipHold >= RESPONSE_HOLD)) {
			info.isAllowed = isWant;
			info.flipHold = 0;
			if (!isJustUnlocked) {
				circuit->LOG("RESPONSE: %s %s at frame %i (territory %.2f, max %.2f)", info.name.c_str(),
						isWant ? "reopened" : "closed", frame, share, info.territoryMax);
			}
		}
	}

	// Once a minute, for tuning thresholds against real games
	if (responseSamples % 30 == 0) {
		std::string locked, closed;
		for (const SRoleInfo& info : roleInfos) {
			if (!info.isUnlocked) {
				locked += " " + info.name;
			} else if (!info.isAllowed) {
				closed += " " + info.name;
			}
		}
		circuit->LOG("RESPONSE: frame %i share %.2f (%i / %i) income %.1f (no reclaim %.1f) locked [%s ] closed [%s ]",
				frame, share, metalMgr->GetMexCount(), spotCount, income, unlockIncome, locked.c_str(), closed.c_str());
	}
	++responseSamples;
}

float CMilitaryManager::RoleProbability(const CCircuitDef* cdef) const
{
	CEnemyManager* enemyMgr = circuit->GetEnemyManager();
	const SRoleInfo& info = roleInfos[cdef->GetMainRole()];
	if (!info.isAllowed) {
		return 0.f;
	}
	float maxProb = 0.f;
	for (const SRoleInfo::SVsInfo& vs : info.vs) {
		const float enemyMetal = enemyMgr->GetEnemyCost(vs.role);
		const float nextMetal = info.cost + cdef->GetCostM();
		const float prob = enemyMetal / (info.cost + 1.f) * vs.importance;
		// income_curve scales the amount wanted (not the roll weight); max_percent still caps it
		if ((prob > maxProb) &&
			(enemyMetal * vs.ratio * info.incomeMult >= nextMetal * info.factor) &&
			(nextMetal <= (armyCost + cdef->GetCostM()) * info.maxPerc))
		{
			maxProb = prob;
		}
	}
	return maxProb;
}

bool CMilitaryManager::IsNeedBigGun(const CCircuitDef* cdef) const
{
	return armyCost * circuit->GetEconomyManager()->GetEcoFactor() > cdef->GetCostM();
}

AIFloat3 CMilitaryManager::GetBigGunPos(CCircuitDef* bigDef) const
{
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	AIFloat3 pos = circuit->GetSetupManager()->GetBasePos();
	if (bigDef->GetMaxRange() < std::max(terrainMgr->GetTerrainWidth(), terrainMgr->GetTerrainHeight())) {
		CMetalManager* metalMgr = circuit->GetMetalManager();
		const CMetalData::Clusters& clusters = metalMgr->GetClusters();
		unsigned size = 1;
		for (unsigned i = 0; i < clusters.size(); ++i) {
			if (metalMgr->IsClusterFinished(i)) {
				pos += clusters[i].position;
				++size;
			}
		}
		pos /= size;
	}
	return pos;
}

void CMilitaryManager::DiceBigGun()
{
	if (superInfos.empty()) {
		return;
	}

	std::vector<SSuperInfo> candidates;
	candidates.reserve(superInfos.size());
	float magnitude = 0.f;
	for (SSuperInfo& info : superInfos) {
		if (info.cdef->IsAvailable(circuit->GetLastFrame())) {
			candidates.push_back(info);
			magnitude += info.weight;
		}
	}
	if ((magnitude == 0.f) || candidates.empty()) {
		bigGunDef = superInfos[0].cdef;
		return;
	}

	unsigned choice = 0;
	float dice = (float)rand() / RAND_MAX * magnitude;
	for (unsigned i = 0; i < candidates.size(); ++i) {
		dice -= candidates[i].weight;
		if (dice < 0.f) {
			choice = i;
			break;
		}
	}
	bigGunDef = candidates[choice].cdef;
}

float CMilitaryManager::GetEnemyThreatPerTeam() const
{
	// Known enemy mobile threat (thr_mod.mobile applied) shared over the enemy teams, against our own army (allies'
	// armies don't count: each AI judges its own). Commanders count on neither side
	return circuit->GetEnemyManager()->GetMobileThreatNoCommFaded(circuit->GetLastFrame(), enemyHalfLife)
			/ std::max(circuit->GetEnemyTeamSize(), 1);
}

float CMilitaryManager::GetEnemyArmyRatio(float& enemyPerTeam, float& ours) const
{
	enemyPerTeam = circuit->GetEnemyManager()->GetMobileCostNoCommFaded(circuit->GetLastFrame(), enemyHalfLife)
			/ std::max(circuit->GetEnemyTeamSize(), 1);
	ours = armyCost;
	if (enemyPerTeam <= 0.f) {
		return -1.f;
	}
	// No army of ours: the enemy's counts as far ahead (the curve's last point)
	return (armyCost > 0.f) ? enemyPerTeam / armyCost : std::numeric_limits<float>::max();
}

void CMilitaryManager::UpdateDefenceTasks()
{
	/*
	 * Defend expansion
	 */
	const std::set<IFighterTask*>& tasks = GetTasks(IFighterTask::FightType::DEFEND);
	CMetalManager* mm = circuit->GetMetalManager();
//	CEconomyManager* em = circuit->GetEconomyManager();
//	CTerrainManager* tm = circuit->GetTerrainManager();
//	const CMetalData::Metals& spots = mm->GetSpots();
	const CMetalData::Clusters& clusters = mm->GetClusters();
//	const std::vector<CEnemyManager::SEnemyGroup>& enemyGroups = circuit->GetEnemyManager()->GetEnemyGroups();
	for (IFighterTask* task : tasks) {
		CDefendTask* dt = static_cast<CDefendTask*>(task);
//		if (dt->GetTarget() != nullptr) {
//			continue;
//		}
//		STerrainMapArea* area = dt->GetLeader()->GetArea();
//		CMetalData::PointPredicate predicate = [em, tm, area, &spots, &clusters](const int index) {
//			const CMetalData::MetalIndices& idcs = clusters[index].idxSpots;
//			for (int idx : idcs) {
//				if (!em->IsOpenSpot(idx) && tm->CanMoveToPos(area, spots[idx].position)) {
//					return true;
//				}
//			}
//			return false;
//		};
//		AIFloat3 center(tm->GetTerrainWidth() / 2, 0, tm->GetTerrainHeight() / 2);
//		int index = mm->FindNearestCluster(center, predicate);
//		if (index >= 0) {
//			dt->SetPosition(clusters[index].position);
//		}

		if (dt->GetPromote() != IFighterTask::FightType::ATTACK) {
			continue;
		}
//		int groupIdx = -1;
//		float minSqDist = std::numeric_limits<float>::max();
//		const AIFloat3& position = dt->GetPosition();
//		for (unsigned i = 0; i < enemyGroups.size(); ++i) {
//			const CEnemyManager::SEnemyGroup& group = enemyGroups[i];
//			const float sqDist = position.SqDistance2D(group.pos);
//			if (sqDist < minSqDist) {
//				minSqDist = sqDist;
//				groupIdx = i;
//			}
//		}
//		if (groupIdx >= 0) {
//			dt->SetMaxPower(std::max(minAttackers, enemyGroups[groupIdx].threat));
//		}
		dt->SetMaxPower(std::max(minAttackers, circuit->GetEnemyManager()->GetMaxGroupThreat()));
	}

	/*
	 * Porc update
	 */
	decltype(defenceIdx) prevIdx = defenceIdx;
	while (defenceIdx < clusters.size()) {
		int index = defenceIdx++;
		if (mm->IsClusterQueued(index) || mm->IsClusterFinished(index)) {
			MakeDefence(index);
			return;
		}
	}
	defenceIdx = 0;
	while (defenceIdx < prevIdx) {
		int index = defenceIdx++;
		if (mm->IsClusterQueued(index) || mm->IsClusterFinished(index)) {
			MakeDefence(index);
			return;
		}
	}
}

void CMilitaryManager::UpdateDefence()
{
	SCOPED_TIME(circuit, __PRETTY_FUNCTION__);
	const int frame = circuit->GetLastFrame();
	decltype(buildDefence)::iterator ibd = buildDefence.begin();
	while (ibd != buildDefence.end()) {
		const auto& defElem = ibd->second.back();
		if (frame >= defElem.second) {
			CCircuitDef* buildDef = defElem.first;
			if (buildDef->IsAvailable(frame)) {
				circuit->GetBuilderManager()->EnqueueTask(IBuilderTask::Priority::NORMAL, buildDef, ibd->first,
														  IBuilderTask::BuildType::DEFENCE, 0.f, true, 0);
			}
			ibd->second.pop_back();
		}
		if (ibd->second.empty()) {
			ibd = buildDefence.erase(ibd);
		} else {
			++ibd;
		}
	}
	if (buildDefence.empty()) {
		circuit->GetScheduler()->RemoveTask(defend);
		defend = nullptr;
	}
}

void CMilitaryManager::MakeBaseDefence(const AIFloat3& pos)
{
	if (baseDefence.empty() || (circuit->IsLoadSave() && (circuit->GetLastFrame() < 0))) {
		return;
	}
	buildDefence.push_back(std::make_pair(pos, baseDefence));
	if (defend == nullptr) {
		defend = std::make_shared<CGameTask>(&CMilitaryManager::UpdateDefence, this);
		circuit->GetScheduler()->RunTaskEvery(defend, FRAMES_PER_SEC);
	}
}

void CMilitaryManager::MarkPointOfInterest(CEnemyInfo* enemy)
{
	if (enemy->GetCircuitDef() != circuit->GetEconomyManager()->GetMexDef()) {  // TODO: if one of the list
		return;
	}
	int index = circuit->GetMetalManager()->FindNearestCluster(enemy->GetPos());
	SRaidPoint& rp = raidPath[index];
	rp.lastFrame = circuit->GetLastFrame();
	rp.units.insert(enemy);
	rp.weight = rp.units.size();  // TODO
}

void CMilitaryManager::UnmarkPointOfInterest(CEnemyInfo* enemy)
{
	if (enemy->GetCircuitDef() != circuit->GetEconomyManager()->GetMexDef()) {  // TODO: if one of the list
		return;
	}
	int index = circuit->GetMetalManager()->FindNearestCluster(enemy->GetPos());
	SRaidPoint& rp = raidPath[index];
	rp.lastFrame = circuit->GetLastFrame();
	rp.units.erase(enemy);
	rp.weight = rp.units.size();  // TODO
}

IUnitTask* CMilitaryManager::DefaultMakeTask(CCircuitUnit* unit)
{
	// FIXME: Make central task assignment system.
	//        MilitaryManager should decide what tasks to merge.
	static const std::map<CCircuitDef::RoleT, IFighterTask::FightType> types = {
		{ROLE_TYPE(SCOUT),   IFighterTask::FightType::SCOUT},
		{ROLE_TYPE(RAIDER),  IFighterTask::FightType::RAID},
		{ROLE_TYPE(RIOT),    IFighterTask::FightType::DEFEND},
		{ROLE_TYPE(ARTY),    IFighterTask::FightType::ARTY},
		{ROLE_TYPE(AA),      IFighterTask::FightType::AA},
		{ROLE_TYPE(AH),      IFighterTask::FightType::AH},
		{ROLE_TYPE(BOMBER),  IFighterTask::FightType::BOMB},
		{ROLE_TYPE(SUPPORT), IFighterTask::FightType::SUPPORT},
		{ROLE_TYPE(MINE),    IFighterTask::FightType::SCOUT},  // FIXME
		{ROLE_TYPE(SUPER),   IFighterTask::FightType::SUPER},
	};
	IFighterTask* task = nullptr;
	CCircuitDef* cdef = unit->GetCircuitDef();
	auto it = types.find(circuit->GetBindedRole(cdef->GetMainRole()));
	// Any main role with the scout attribute (scout as a secondary role) fills the scout quota first.
	// Units whose main role already leads to scouting keep their own path below.
	if (cdef->IsRoleScout() && ((it == types.end()) || (it->second != IFighterTask::FightType::SCOUT))
		&& (GetTasks(IFighterTask::FightType::SCOUT).size() < GetMaxScouts(cdef)))
	{
		task = EnqueueTask(IFighterTask::FightType::SCOUT);
	}
	if (task != nullptr) {
		// scout picked above
	} else if (cdef->IsRoleSupport()) {
		if (/*cdef->IsAttacker() && */GetTasks(IFighterTask::FightType::ATTACK).empty() && GetTasks(IFighterTask::FightType::DEFEND).empty()) {
			task = EnqueueDefend(IFighterTask::FightType::ATTACK,
								 IFighterTask::FightType::SUPPORT, minAttackers);
		} else {
			task = EnqueueTask(IFighterTask::FightType::SUPPORT);
		}
	} else {
		// Any configured role may take a free guard slot
		task = FindGuardTask(unit);
		if (task != nullptr) {
			// guard picked above
		} else if (it != types.end()) {
			switch (it->second) {
				case IFighterTask::FightType::RAID: {
					if (GetTasks(IFighterTask::FightType::RAID).empty()) {
						task = EnqueueDefend(IFighterTask::FightType::RAID, raid.min);
					}
				} break;
				case IFighterTask::FightType::AH: {
					if (!cdef->IsRoleMine() && !IsEnemyHeavySeen(AH_HUNT_FRAMES)) {
						task = EnqueueAttackBound(unit);  // see below; UpdateAhPull takes it out once a heavy is seen
					} else {
						task = EnqueueAntiHeavy(unit);
					}
				} break;
				case IFighterTask::FightType::DEFEND: {
					task = EnqueueAttackBound(unit);  // see below
				} break;
				default: break;
			}
			if (task == nullptr) {
				task = EnqueueTask(it->second);
			}
		} else {
			// Attack-bound units start in a gathering (defend) group at base. While no attack task exists this
			// is the first army gathering to its power. Otherwise CDefendTask promotes it on its first update:
			// it reinforces the nearest stronger group it can join (EnqueueReinforce), or leaves as a small attack group.
			// quota.attack_merge.base_gather false: no gathering at all (EnqueueAttackBound)
			task = EnqueueAttackBound(unit);
		}
	}

	return task;
}

/*
 * "capture_hide" (behaviour.json, per unit): a capturer whose captured units cost more than
 * capture_hide x its own cost goes home and hides, because if it dies they go back to the enemy.
 * Its retreat task keeps it there until enough of them are gone to drop it below the line.
 */
void CMilitaryManager::UpdateCaptureHide()
{
	SCOPED_TIME(circuit, __PRETTY_FUNCTION__);
	std::unordered_map<int, float> costByController;
	for (const auto& kv : capturedUnits) {
		costByController[kv.second.controllerId] += kv.second.cost;
	}

	for (CCircuitUnit* unit : army) {
		CCircuitDef* cdef = unit->GetCircuitDef();
		if (cdef->GetCaptureHide() <= 0.f) {
			continue;
		}
		auto it = costByController.find(unit->GetId());
		const float capturedCost = (it != costByController.end()) ? it->second : 0.f;
		unit->SetCaptureHide(capturedCost > cdef->GetCaptureHide() * cdef->GetCostM());

		const IUnitTask::Type type = unit->GetTask()->GetType();
		if (unit->IsCaptureHide() && (type != IUnitTask::Type::RETREAT) && (type != IUnitTask::Type::PLAYER)) {
			AssignTask(unit, EnqueueRetreat());
		}
	}
}

void CMilitaryManager::UpdateAhPull()
{
	// MakeTask sends a new anti-heavy unit to the army while no enemy heavy has been seen lately, and an anti-heavy
	// squad disbands once none has been seen for a while; without this pull the units stay in the army after a heavy
	// shows up
	SCOPED_TIME(circuit, __PRETTY_FUNCTION__);
	if (!IsEnemyHeavySeen(AH_HUNT_FRAMES)) {
		return;
	}
	for (CCircuitUnit* unit : army) {
		if (circuit->GetBindedRole(unit->GetCircuitDef()->GetMainRole()) != ROLE_TYPE(AH)) {
			continue;
		}
		IUnitTask* task = unit->GetTask();
		if (task->GetType() != IUnitTask::Type::FIGHTER) {
			continue;  // retreat, player control, idle, ...
		}
		const IFighterTask::FightType type = static_cast<IFighterTask*>(task)->GetFightType();
		if ((type != IFighterTask::FightType::ATTACK) && (type != IFighterTask::FightType::DEFEND)) {
			continue;  // already anti-heavy, or a guard
		}
		if ((task->GetState() == IUnitTask::State::ENGAGE) || (task->GetState() == IUnitTask::State::DISENGAGE)) {
			continue;  // fighting or fleeing: losing it now could tip the fight; the next pass takes it
		}
		AssignTask(unit, EnqueueAntiHeavy(unit));
	}
}

IFighterTask* CMilitaryManager::EnqueueAntiHeavy(CCircuitUnit* unit)
{
	if (!unit->GetCircuitDef()->IsRoleMine()) {
		for (IFighterTask* t : GetTasks(IFighterTask::FightType::AH)) {
			if (t->CanAssignTo(unit)) {
				return t;
			}
		}
	}
	return EnqueueTask(IFighterTask::FightType::AH);
}

bool CMilitaryManager::IsEnemyHeavySeen(int frames)
{
	const int frame = circuit->GetLastFrame();
	if (heavySeenFrame != frame) {  // once a frame: MakeTask asks for every new anti-heavy unit
		heavySeenFrame = frame;
		heavySeenAge = std::numeric_limits<int>::max();
		if (circuit->GetEnemyManager()->GetEnemyCost(ROLE_TYPE(HEAVY)) >= 1.f) {  // else none is known: no scan
			for (auto& kv : circuit->GetEnemyInfos()) {
				const CEnemyInfo* enemy = kv.second;
				CCircuitDef* edef = enemy->GetCircuitDef();
				if ((edef == nullptr) || !edef->IsEnemyRoleAny(CCircuitDef::RoleMask::HEAVY)) {
					continue;
				}
				if (enemy->IsInRadarOrLOS()) {
					heavySeenAge = 0;
					break;
				}
				const int lastSeen = enemy->GetData()->GetLastSeen();  // the frame it left radar
				if (lastSeen >= 0) {
					heavySeenAge = std::min(heavySeenAge, frame - lastSeen);
				}
			}
		}
	}
	return heavySeenAge < frames;
}

void CMilitaryManager::Watchdog()
{
	SCOPED_TIME(circuit, __PRETTY_FUNCTION__);
	for (CCircuitUnit* unit : army) {
		if (unit->GetTask()->GetType() == IUnitTask::Type::PLAYER) {
			continue;
		}
		auto commands = unit->GetUnit()->GetCurrentCommands();
		if (commands.empty()) {
			UnitIdle(unit);
		}
		utils::free_clear(commands);
	}
}

void CMilitaryManager::UpdateIdle()
{
	SCOPED_TIME(circuit, __PRETTY_FUNCTION__);
	idleTask->Update();
}

void CMilitaryManager::UpdateFight()
{
	SCOPED_TIME(circuit, __PRETTY_FUNCTION__);
	if (fightIterator >= fightUpdates.size()) {
		fightIterator = 0;
	}

	// stagger the Update's
	unsigned int n = (fightUpdates.size() / TEAM_SLOWUPDATE_RATE) + 1;

	while ((fightIterator < fightUpdates.size()) && (n != 0)) {
		IUnitTask* task = fightUpdates[fightIterator];
		if (task->IsDead()) {
			fightUpdates[fightIterator] = fightUpdates.back();
			fightUpdates.pop_back();
			task->ClearRelease();  // delete task;
		} else {
			task->Update();
			if (!task->IsDead()) {
				task->LateUpdate();
			}
			++fightIterator;
			n--;
		}
	}
}

void CMilitaryManager::UpdateResign()
{
	const int frame = circuit->GetLastFrame();
	// 1v1 only: our ally team is just us (start script) and one enemy team (engine count less Gaia)
	if ((frame < resign.frame) || (circuit->GetAllyTeam()->GetSize() != 1) || (circuit->GetEnemyTeamSize() != 1)) {
		return;
	}

	CMetalManager* metalMgr = circuit->GetMetalManager();
	metalMgr->MarkAllyMexes();
	const int mexCount = metalMgr->GetMexCount();
	const int spotCount = metalMgr->GetSpots().size();

	// Army against army, commanders left out on both sides (a levelled commander dwarfs any unit; builders have no
	// threat). Ours: the finished army (no commander, no builders). Theirs: every live enemy seen in LOS
	float ourThreat = 0.f;
	for (const CCircuitUnit* unit : army) {
		ourThreat += unit->GetCircuitDef()->GetThreat();
	}
	float enemyThreat = 0.f;
	for (const auto& kv : circuit->GetEnemyManager()->GetEnemyUnits()) {
		const CEnemyUnit* enemy = kv.second;
		const CCircuitDef* edef = enemy->GetCircuitDef();
		if (!enemy->IsDead() && (edef != nullptr) && enemy->IsKnown(frame) && edef->IsMobile() && !edef->IsRoleComm()) {
			enemyThreat += edef->GetThreat();
		}
	}

	// No army left reads lost whatever we have seen of theirs (their artillery or bombers may never show)
	const bool isLost = (mexCount < resign.mexShare * spotCount)
			&& ((ourThreat <= 0.f) || (enemyThreat > resign.threatMult * ourThreat));
	if (!isLost) {
		if (resignSince >= 0) {
			circuit->LOG("RESIGN: no longer reads lost (frame %i)", frame);
			resignSince = -1;
		}
		return;
	}
	if (resignSince < 0) {
		resignSince = frame;
		circuit->LOG("RESIGN: reads lost (frame %i): mexes %i / %i spots, army threat %.1f vs enemy %.1f",
				frame, mexCount, spotCount, ourThreat, enemyThreat);
	}
	// It must read lost on every check for resign.holdFrames: a trade or a raid can look lost for a moment
	if (frame - resignSince >= resign.holdFrames) {
		circuit->LOG("RESIGN: now (frame %i)", frame);
		circuit->Surrender();
	}
}

/*
 * "drowned" (behaviour.json): a ground unit that ends up in water deeper than its move type allows (knocked or
 * pushed off a shore, dropped there, or the seabed terraformed under it) can't move or do anything useful.
 * After "time" seconds stuck there it self-destructs, so the squads and builder tasks waiting on it are freed.
 * @see CTerrainManager::IsStuckInWater. The order is sent once: CMD_SELFD toggles the countdown, the AI can't
 * read it back, and Zero-K gadgets (capture, unit given, mass self-D blocker) toggle it too.
 */
void CMilitaryManager::UpdateDrowned()
{
	SCOPED_TIME(circuit, __PRETTY_FUNCTION__);
	const int frame = circuit->GetLastFrame();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	std::unordered_map<int, int> stillStuck;  // drops dead units and the ones that got out
	for (auto& kv : circuit->GetTeamUnits()) {
		CCircuitUnit* unit = kv.second;
		const AIFloat3& pos = unit->GetPos(frame);
		if (!terrainMgr->IsStuckInWater(unit->GetCircuitDef(), pos) || unit->GetUnit()->IsBeingBuilt()) {
			continue;
		}
		auto it = drownedSince.find(unit->GetId());
		int since = (it != drownedSince.end()) ? it->second : frame;
		if ((since >= 0) && (frame - since >= drownedFrames)) {
			TRY_UNIT(circuit, unit,
				unit->GetUnit()->SelfDestruct();
			)
			circuit->LOG("DROWNED: frame %i | %s (%i) stuck in deep water at (%.0f, %.0f) for %.1f s, self-destruct",
					frame, unit->GetCircuitDef()->GetDef()->GetName(), unit->GetId(), pos.x, pos.z,
					float(frame - since) / FRAMES_PER_SEC);
			since = -1;
		}
		stillStuck.emplace(unit->GetId(), since);
	}
	drownedSince.swap(stillStuck);
}

void CMilitaryManager::AddArmyCost(CCircuitUnit* unit)
{
	AddResponse(unit);
	armyCost += unit->GetCircuitDef()->GetCostM();
	armyThreat += unit->GetCircuitDef()->GetThreat();
}

void CMilitaryManager::DelArmyCost(CCircuitUnit* unit)
{
	DelResponse(unit);
	armyCost = std::max(armyCost - unit->GetCircuitDef()->GetCostM(), .0f);
	armyThreat = std::max(armyThreat - unit->GetCircuitDef()->GetThreat(), .0f);
}

void CMilitaryManager::UpdateAttackThreatFloor()
{
	const float enemyPerTeam = GetEnemyThreatPerTeam();
	const bool wasFloor = isAttackThreatFloor;
	if (armyThreat < attackThreatFloor.on * enemyPerTeam) {
		isAttackThreatFloor = true;
	} else if (armyThreat >= attackThreatFloor.off * enemyPerTeam) {
		isAttackThreatFloor = false;
	}  // else: keep, so the floor doesn't flip on every unit made or lost
	const bool isSwitch = (isAttackThreatFloor != wasFloor);
	// The numbers go to the log on every switch and every 30 s, so the floor can be checked against a game
	const int frame = circuit->GetLastFrame();
	if (isSwitch || (frame >= attackThreatFloorLogFrame)) {
		attackThreatFloorLogFrame = frame + FRAMES_PER_SEC * 30;
		circuit->LOG("THREAT FLOOR %s%s: army %.1f vs enemy %.1f per team (%.2f; on below %.2f, off from %.2f)",
				isAttackThreatFloor ? "on" : "off", isSwitch ? " (switched)" : "", armyThreat, enemyPerTeam,
				(enemyPerTeam > 0.f) ? armyThreat / enemyPerTeam : 0.f, attackThreatFloor.on, attackThreatFloor.off);
		// Where the two numbers come from: our army by type (threat, as compared, and power, pwr_mod's view), and the
		// known enemies split into seen now and hidden (fading), all enemy teams together
		struct SType { int count = 0; float threat = 0.f; };
		std::map<const CCircuitDef*, SType> types;
		float armyPower = 0.f;
		for (CCircuitUnit* unit : army) {
			const CCircuitDef* cdef = unit->GetCircuitDef();
			SType& t = types[cdef];
			++t.count;
			t.threat += cdef->GetThreat();
			armyPower += cdef->GetPower();
		}
		std::vector<std::pair<const CCircuitDef*, SType>> top(types.begin(), types.end());
		std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.second.threat > b.second.threat; });
		std::string ours = utils::int_to_string(army.size(), "%i units") + utils::float_to_string(armyPower, ", power %.1f")
				+ "; top:";
		for (size_t i = 0; (i < top.size()) && (i < 6); ++i) {
			ours += " " + std::string(top[i].first->GetDef()->GetName()) + utils::int_to_string(top[i].second.count, " %ix")
					+ utils::float_to_string(top[i].second.threat, " %.1f");
		}
		circuit->LOG("THREAT FLOOR ours: %s", ours.c_str());
		circuit->LOG("THREAT FLOOR enemy: %s", circuit->GetEnemyManager()->GetMobileThreatReport(frame, enemyHalfLife).c_str());
	}
	if (isSwitch) {
		// Public chat (Game_sendTextMessage only runs "/" commands, as for the resign), with the numbers behind the
		// switch: enemy threat per team and our army's (as compared above), and the economy's mobile:static metal pull
		// with its army factor (economy.ms_pull_army; off before its since or with no enemy army known)
		const CEconomyManager* economyMgr = circuit->GetEconomyManager();
		const std::string msg = std::string("/say <CircuitAI> ") + (isAttackThreatFloor ? "Defending" : "Attacking")
				+ " | enemy threat: " + utils::float_to_string(enemyPerTeam, "%.0f")
				+ " | our threat: " + utils::float_to_string(armyThreat, "%.0f")
				+ " | ms_pull: " + utils::float_to_string(economyMgr->GetPullMtoS(), "%.2f")
				+ ((economyMgr->GetPullMtoSArmyRatio() >= 0.f)
					? " (army x" + utils::float_to_string(economyMgr->GetPullMtoSArmyMult(), "%.2f") + ")"
					: std::string(" (army off)"));
		circuit->GetGame()->SendTextMessage(msg.c_str(), 0);
	}
}

} // namespace circuit

/*
 * TurretPicker.cpp
 *
 *  Created on: Nov 11, 2026
 *      Author: Claude
 */

#include "module/TurretPicker.h"
#include "map/ThreatMap.h"
#include "module/BuilderManager.h"
#include "module/MilitaryManager.h"
#include "resource/MetalManager.h"
#include "setup/SetupManager.h"
#include "task/builder/DefenceTask.h"
#include "terrain/ChokeMap.h"
#include "terrain/TerrainManager.h"
#include "unit/ally/AllyUnit.h"
#include "unit/enemy/EnemyUnit.h"
#include "unit/CircuitDef.h"
#include "unit/CircuitUnit.h"
#include "CircuitAI.h"
#include "util/Defines.h"
#include "util/Utils.h"

#include "spring/SpringMap.h"

#include "json/json.h"

#include "Log.h"
#include "UnitDef.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace circuit {

using namespace springai;

#define CELL			64.f  // the candidate grid
#define MAX_TRAFFIC		64  // heaviest traffic cells kept as targets
#define JOB_TIMEOUT		(FRAMES_PER_SEC * 30)  // with no builder on it, as the cornered defence's
#define MAX_SHOWN		300  // candidates kept for the overlay

CTurretPicker::CTurretPicker(CCircuitAI* circuit)
		: circuit(circuit)
		, inflRadius(2048.f)
{
	ReadConfig();
}

CTurretPicker::~CTurretPicker()
{
}

void CTurretPicker::ReadConfig()
{
	const Json::Value& root = circuit->GetSetupManager()->GetConfig();
	const std::string& cfgName = circuit->GetSetupManager()->GetConfigName();
	const Json::Value& turret = root["turret"];
	SConfig& c = config;
	c.radius = turret.get("radius", c.radius).asFloat();
	c.minValue = turret.get("min_value", c.minValue).asFloat();
	c.traffic = turret.get("traffic", c.traffic).asFloat();
	c.horizon = turret.get("horizon", c.horizon).asFloat();
	c.ally = turret.get("ally", c.ally).asFloat();
	c.enemy = turret.get("enemy", c.enemy).asFloat();
	c.front = turret.get("front", c.front).asFloat();
	c.height = turret.get("height", c.height).asFloat();
	c.heightCap = turret.get("height_cap", c.heightCap).asFloat();
	c.travel = turret.get("travel", c.travel).asFloat();
	c.choke = turret.get("choke", c.choke).asFloat();
	c.isAfterFlee = turret.get("after_flee", c.isAfterFlee).asBool();
	c.afterFleeSince = turret.get("after_flee_since", c.afterFleeSince).asFloat();
	c.isNearEnemy = turret.get("near_enemy", c.isNearEnemy).asBool();
	c.enemyReach = turret.get("enemy_reach", c.enemyReach).asFloat();
	c.allyRadius = turret.get("ally_radius", c.allyRadius).asFloat();
	c.cooldown = turret.get("cooldown", c.cooldown).asFloat();
	c.share = turret.get("share", c.share).asFloat();
	std::vector<std::string> names = {"turretlaser", "turretmissile"};
	const Json::Value& light = turret["light"];
	if (light.isArray()) {
		names.clear();
		for (const Json::Value& name : light) {
			names.push_back(name.asString());
		}
	}
	for (const std::string& name : names) {
		CCircuitDef* cdef = circuit->GetCircuitDef(name.c_str());
		if ((cdef != nullptr) && cdef->IsAttacker() && !cdef->IsMobile()) {
			c.light.push_back(cdef);
		} else {
			circuit->LOG("CONFIG %s: turret.light '%s' is not a turret, ignored", cfgName.c_str(), name.c_str());
		}
	}
	inflRadius = DEFAULT_SLACK * 4 * root["defence"].get("infl_rad", 5.f).asFloat();  // CInfluenceMap::AddTerritory's
}

AIFloat3 CTurretPicker::FindSpot(CCircuitDef* def, const STurretOrder& order, float& outValue)
{
	outValue = 0.f;
	if (order.builder == nullptr) {
		return -RgtVector;
	}
	const AIFloat3 centre = utils::is_valid(order.centre) ? order.centre : order.builder->GetPos(circuit->GetLastFrame());
	const float radius = (order.radius >= 0.f) ? order.radius : config.radius;
	const SGather gather = Gather(centre, radius, def->GetMaxRange(), -RgtVector);
	const SDefResult res = SearchDef(def, order.builder, gather);
	if (!res.search.IsFound()) {
		return -RgtVector;
	}
	const CTurretValue::SScored& best = res.search.scored.front();
	outValue = best.terms.value;
	return AIFloat3(best.cand.pos.x, best.cand.pos.y, best.cand.pos.z);
}

IBuilderTask* CTurretPicker::Order(const STurretOrder& order)
{
	const auto t0 = std::chrono::steady_clock::now();
	if (order.builder == nullptr) {
		return nullptr;
	}
	CCircuitUnit* builder = order.builder;
	const int frame = circuit->GetLastFrame();
	const AIFloat3 centre = utils::is_valid(order.centre) ? order.centre : builder->GetPos(frame);
	const float radius = (order.radius >= 0.f) ? order.radius : config.radius;
	const std::string who = std::string(order.caller) + ", by " + builder->GetCircuitDef()->GetDef()->GetName()
			+ utils::int_to_string(builder->GetId(), " %i");
	last = SLast();
	last.frame = frame;
	last.centre = centre;
	last.radius = radius;
	auto say = [this, &t0](const std::string& text) {
		const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
		last.line = text + utils::float_to_string(ms, ", %.2f ms");
		circuit->LOG("TURRET %s", last.line.c_str());
	};

	const std::vector<CCircuitDef*> defs = GetDefs(order, centre);
	if (defs.empty()) {
		say(who + ": nothing, no def it can build");
		return nullptr;
	}
	float maxRange = 0.f;
	for (CCircuitDef* def : defs) {
		maxRange = std::max(maxRange, def->GetMaxRange());
	}
	const SGather gather = Gather(centre, radius, maxRange, -RgtVector);

	// The def whose best spot has the most value per metal
	SDefResult best;
	float bestPerCost = -1.f;
	SDefResult shown;  // the last tried, when none is found
	float bestBasePerCost = 0.f;  // for the log, worth or not
	for (CCircuitDef* def : defs) {
		SDefResult res = SearchDef(def, builder, gather);
		const float cost = std::max(def->GetCostM(), 1.f);
		bestBasePerCost = std::max(bestBasePerCost, res.search.bestBase / cost);
		if (res.search.IsFound()) {
			const float perCost = res.search.scored.front().terms.value / cost;
			if (perCost > bestPerCost) {
				bestPerCost = perCost;
				best = std::move(res);
			}
		} else {
			shown = std::move(res);
		}
	}
	SDefResult& res = (best.def != nullptr) ? best : shown;
	last.def = res.def;
	last.isFound = (best.def != nullptr);
	last.frontNorm = res.search.frontNorm;
	last.passed = res.passed;
	last.outside = res.outside;
	last.scored = res.search.scored;
	if (last.scored.size() > MAX_SHOWN) {
		last.scored.resize(MAX_SHOWN);
	}

	if (best.def == nullptr) {
		const int* n = res.counts;
		auto count = [n](Reject r) { return n[static_cast<int>(r)]; };
		// SITE and UNSAFE are tested only on candidates about to be scored
		const int inRadius = res.candidates + count(Reject::MASK) + count(Reject::INFLUENCE) + count(Reject::THREAT)
				+ count(Reject::REACH);
		std::string reason;
		if (!res.search.scored.empty()) {
			reason = utils::float_to_string(bestBasePerCost, "the best at %.2f x cost");
		} else if (res.search.unfinished > 0) {
			reason = "no buildable spot among the best";
		} else if (res.candidates > 0) {
			reason = "nothing in range worth its cost";
		} else if ((inRadius > 0) && (count(Reject::INFLUENCE) == inRadius)) {
			reason = "outside our influence";
		} else if ((count(Reject::THREAT) > 0) && (count(Reject::THREAT) + count(Reject::INFLUENCE) == inRadius)) {
			reason = "all threatened";
		} else {
			reason = "no spot";
		}
		say(who + ": nothing, " + reason + utils::int_to_string(res.candidates, " (%i passed")
				+ utils::int_to_string(count(Reject::INFLUENCE), ", %i outside our influence")
				+ utils::int_to_string(count(Reject::THREAT), ", %i threatened")
				+ utils::int_to_string(count(Reject::SITE), ", %i not buildable")
				+ utils::int_to_string(count(Reject::REACH), ", %i out of reach")
				+ utils::int_to_string(count(Reject::UNSAFE), ", %i unsafe")
				+ utils::int_to_string(count(Reject::MASK), ", %i masked)"));
		return nullptr;
	}

	CCircuitDef* def = best.def;
	const CTurretValue::SScored& pick = best.search.scored.front();
	const AIFloat3 spot(pick.cand.pos.x, pick.cand.pos.y, pick.cand.pos.z);
	CBuilderManager* builderMgr = circuit->GetBuilderManager();
	IBuilderTask* task = builderMgr->EnqueueTask(order.priority, def, spot, IBuilderTask::BuildType::DEFENCE,
			def->GetCostM(), 0.f /*shake*/, true, JOB_TIMEOUT);
	static_cast<CBDefenceTask*>(task)->SetPicker();
	// Its footprint goes on the blocking map now: no other job (a picker's included) takes the spot meanwhile
	task->SetFacing(UNIT_FACING_SOUTH);
	task->SetBuildPos(spot);
	builderMgr->AssignTask(builder, task);

	const CTurretValue::STerms& t = pick.terms;
	const float cost = std::max(def->GetCostM(), 1.f);
	char buf[512];
	snprintf(buf, sizeof(buf), ": %s at (%.0f, %.0f): T %.0f, A %.0f, E %.0f, C %.0f, height %.2f, front %.2f "
			"(enemy %.1f of %.1f), our building at %.0f, walk %.0f, base %.2f x cost, value %.2f x cost, "
			"%i of %i candidates", def->GetDef()->GetName(), spot.x, spot.z, t.traffic, t.ally, t.enemy, t.choke,
			1.f + config.height * t.height, t.front, pick.cand.enemyInfl, best.search.frontNorm,
			GetNearestOurs(gather, spot), pick.cand.walk, t.base / cost, t.value / cost, best.search.tested,
			best.candidates);
	say(who + buf);
	return task;
}

bool CTurretPicker::IsStillWorth(const CBDefenceTask* task)
{
	CCircuitDef* def = task->GetBuildDef();
	const AIFloat3& spot = task->GetBuildPos();
	if ((def == nullptr) || !utils::is_valid(spot)) {
		return false;
	}
	if (circuit->GetThreatMap()->GetAllThreatAt(spot) > THREAT_MIN) {
		circuit->LOG("TURRET dropped on arrival: %s at (%.0f, %.0f), threat reached it", def->GetDef()->GetName(),
				spot.x, spot.z);
		return false;
	}
	// Its own tower (queued at the spot) left out of the halving
	const SGather gather = Gather(spot, 0.f, def->GetMaxRange(), spot);
	const std::vector<CTurretValue::STarget> targets = MakeTargets(def, gather);
	const CTurretValue::SParams params = MakeParams(def);
	const float emit = def->GetDef()->GetLosHeight();
	const bool isHighArc = (def->GetDef()->GetHighTrajectoryType() == 1);
	CSpotScorer* spotScorer = circuit->GetMilitaryManager()->GetSpotScorer();
	CTurretValue::SCandidate c = {{spot.x, circuit->GetTerrainManager()->GetHeightAt(spot.x, spot.z), spot.z}, 0.f, 0.f};
	const CTurretValue::STerms terms = CTurretValue::Evaluate(c, targets, params, 0.f,
			[&](const CTurretValue::SPos& from, const CTurretValue::SPos& to) {
		return isHighArc || spotScorer->IsClearLine(AIFloat3(from.x, from.y, from.z), emit, AIFloat3(to.x, to.y, to.z));
	});
	if (terms.base < params.minBase) {
		circuit->LOG("TURRET dropped on arrival: %s at (%.0f, %.0f), now at %.2f x cost", def->GetDef()->GetName(),
				spot.x, spot.z, terms.base / std::max(def->GetCostM(), 1.f));
		return false;
	}
	return true;
}

std::vector<CCircuitDef*> CTurretPicker::GetDefs(const STurretOrder& order, const AIFloat3& centre) const
{
	const int frame = circuit->GetLastFrame();
	CCircuitDef* builderDef = order.builder->GetCircuitDef();
	std::vector<CCircuitDef*> list = order.defs;
	if (list.empty()) {
		CMilitaryManager* militaryMgr = circuit->GetMilitaryManager();
		const bool isWater = circuit->GetMap()->GetElevationAt(centre.x, centre.z) < -SQUARE_SIZE * 5;  // MakeDefence's
		for (CCircuitDef* def : isWater ? militaryMgr->GetWaterDefenders() : militaryMgr->GetLandDefenders()) {
			if (!def->IsRoleAA()) {
				list.push_back(def);
			}
		}
	}
	std::vector<CCircuitDef*> defs;
	for (CCircuitDef* def : list) {
		if ((def != nullptr) && def->IsAttacker() && def->IsAvailable(frame) && builderDef->CanBuild(def)
			&& (std::find(defs.begin(), defs.end(), def) == defs.end()))
		{
			defs.push_back(def);
		}
	}
	return defs;
}

/*
 * Everything a spot within radius of centre could count, and the buildings both influence rules read. Targets are
 * halved here for each tower (ours and allies', queued too) already covering them, skipTower's left out
 */
CTurretPicker::SGather CTurretPicker::Gather(const AIFloat3& centre, float radius, float maxRange,
		const AIFloat3& skipTower) const
{
	SGather g;
	g.centre = centre;
	g.radius = radius;
	const float reach = radius + maxRange;
	const float sqReach = SQUARE(reach);
	const float sqInflReach = SQUARE(radius + inflRadius);
	const int frame = circuit->GetLastFrame();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	const CChokeMap* chokeMap = circuit->GetChokeMap();
	const bool isChokeReady = chokeMap->IsReady();
	auto ground = [terrainMgr](const AIFloat3& p) {
		return CTurretValue::SPos{p.x, terrainMgr->GetHeightAt(p.x, p.z), p.z};
	};

	// T: the heaviest traffic cells within reach
	std::vector<SRaw> traffic;
	const CTrafficField& field = chokeMap->GetTraffic();
	if (isChokeReady && !field.IsEmpty()) {
		const int cellSize = field.GetCellSize();
		ForEachCellIn(field.GetWidth(), field.GetHeight(), cellSize, centre.x - reach, centre.z - reach,
				centre.x + reach, centre.z + reach, [&](int x, int z) {
			const AIFloat3 c((x + 0.5f) * cellSize, 0.f, (z + 0.5f) * cellSize);
			const float amount = field.GetAt(z * field.GetWidth() + x);
			if ((amount > 0.f) && (c.SqDistance2D(centre) <= sqReach)) {
				traffic.push_back({ground(c), amount, CTurretValue::Kind::TRAFFIC, false});
			}
		});
	}
	auto keepBest = [](std::vector<SRaw>& raws, size_t count) {
		std::stable_sort(raws.begin(), raws.end(), [](const SRaw& a, const SRaw& b) {
			return a.amount > b.amount;
		});
		if (raws.size() > count) {
			raws.resize(count);
		}
	};
	keepBest(traffic, MAX_TRAFFIC * 4);  // the line-of-fire tests of the halving only for those that may make it

	// A, and our buildings for the influence rule (armed or not, nanoframes too; constructors, Claw mines and missiles
	// left out)
	for (const auto& kv : circuit->GetFriendlyUnits()) {
		const CCircuitDef* cdef = kv.second->GetCircuitDef();
		if ((cdef == nullptr) || cdef->IsMobile() || cdef->IsMobileBuilding()) {
			continue;
		}
		const AIFloat3& pos = kv.second->GetPos(frame);
		if (!utils::is_valid(pos)) {
			continue;
		}
		const float sqDist = pos.SqDistance2D(centre);
		if (sqDist <= sqInflReach) {
			g.ourBuildings.push_back(pos);
		}
		if (!cdef->IsAttacker() && (sqDist <= sqReach)) {
			g.raws.push_back({ground(pos), cdef->GetCostM(), CTurretValue::Kind::ALLY, false});
		}
	}

	// E, as choke.spot.enemy: buildings by their last known position, mobiles while in radar or sight; an enemy mex at
	// its income x the horizon. And known enemy buildings (fakes, Claw mines and missiles left out) for F
	CMetalManager* metalMgr = circuit->GetMetalManager();
	for (const auto& kv : circuit->GetEnemyInfos()) {
		const CEnemyInfo* e = kv.second;
		const CCircuitDef* edef = e->GetCircuitDef();
		if (e->IsHidden() || (edef == nullptr)) {
			continue;
		}
		const AIFloat3& pos = e->GetPos();
		const float sqDist = pos.SqDistance2D(centre);
		if (!edef->IsMobile() && !edef->IsMobileBuilding() && !e->GetData()->IsFake() && (sqDist <= sqInflReach)) {
			g.enemyBuildings.push_back(pos);
		}
		if ((sqDist > sqReach) || (edef->IsMobile() && !e->IsInRadarOrLOS())) {
			continue;
		}
		float amount = e->GetCost();
		if (edef->IsMex()) {
			const int spot = metalMgr->FindNearestSpot(pos);
			if (spot >= 0) {
				amount = metalMgr->GetSpots()[spot].income * config.horizon;
			}
		}
		g.raws.push_back({ground(pos), amount, CTurretValue::Kind::ENEMY, edef->IsAbleToFly()});
	}

	// C: the middles of the chokes on our current line and the enemy line
	if (isChokeReady) {
		const std::vector<CRegionGraph::SChoke>& chokes = chokeMap->GetGraph().GetChokes();
		std::vector<int> ids = chokeMap->GetCurrentLine();
		ids.insert(ids.end(), chokeMap->GetEnemyLine().begin(), chokeMap->GetEnemyLine().end());
		std::sort(ids.begin(), ids.end());
		ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
		for (int c : ids) {
			if ((c < 0) || (c >= (int)chokes.size())) {
				continue;
			}
			const AIFloat3 pos = chokeMap->GetCellPos(chokes[c].centre);
			if (pos.SqDistance2D(centre) <= sqReach) {
				g.raws.push_back({ground(pos), config.choke, CTurretValue::Kind::CHOKE, false});
			}
		}
	}

	// Halved for each tower that covers it already (a Claw minefield doesn't)
	std::vector<CSpotScorer::STower> towers = circuit->GetMilitaryManager()->GetSpotScorer()->GetTowers(false);
	if (utils::is_valid(skipTower)) {
		towers.erase(std::remove_if(towers.begin(), towers.end(), [&skipTower](const CSpotScorer::STower& t) {
			return t.pos.SqDistance2D(skipTower) < SQUARE(SQUARE_SIZE * 2);
		}), towers.end());
	}
	towers.erase(std::remove_if(towers.begin(), towers.end(), [&](const CSpotScorer::STower& t) {
		return t.pos.SqDistance2D(centre) > SQUARE(reach + t.range);
	}), towers.end());
	const CSpotScorer* spotScorer = circuit->GetMilitaryManager()->GetSpotScorer();
	auto halve = [&](SRaw& r) {
		const AIFloat3 p(r.pos.x, r.pos.y, r.pos.z);
		for (const CSpotScorer::STower& tower : towers) {
			if ((tower.pos.SqDistance2D(p) <= SQUARE(tower.range))
				&& (tower.isHighArc || spotScorer->IsClearLine(tower.pos, tower.emit, p)))
			{
				r.amount *= 0.5f;
			}
		}
	};
	for (SRaw& r : traffic) {
		halve(r);
	}
	keepBest(traffic, MAX_TRAFFIC);
	for (SRaw& r : g.raws) {
		halve(r);
	}
	g.raws.insert(g.raws.end(), traffic.begin(), traffic.end());
	return g;
}

std::vector<CTurretValue::STarget> CTurretPicker::MakeTargets(CCircuitDef* def, const SGather& gather) const
{
	const float range = std::max(def->GetMaxRange(), 1.f);
	const float sqReach = SQUARE(gather.radius + range);
	const CTrafficField& field = circuit->GetChokeMap()->GetTraffic();
	// The traffic field carries the income across every cross-section of its routes: summed over the cells in range,
	// it counts each route once per cell length its range spans
	const float cellLengths = std::max(2.f * range / std::max(field.GetCellSize(), 1), 1.f);
	std::vector<CTurretValue::STarget> targets;
	for (const SRaw& r : gather.raws) {
		if (SQUARE(r.pos.x - gather.centre.x) + SQUARE(r.pos.z - gather.centre.z) > sqReach) {
			continue;
		}
		float metal = 0.f;
		switch (r.kind) {
			case CTurretValue::Kind::TRAFFIC: {
				metal = r.amount * config.horizon / cellLengths * config.traffic;
			} break;
			case CTurretValue::Kind::ALLY: {
				metal = r.amount * config.ally;
			} break;
			case CTurretValue::Kind::ENEMY: {
				if (r.isAir ? !def->HasAntiAir() : !def->HasAntiLand()) {
					continue;
				}
				metal = r.amount * config.enemy;
			} break;
			case CTurretValue::Kind::CHOKE: {
				metal = r.amount;
			} break;
		}
		if (metal > 0.f) {
			targets.push_back({r.pos, metal, r.kind});
		}
	}
	return targets;
}

CTurretPicker::Reject CTurretPicker::MakeCandidate(CCircuitUnit* builder, const SGather& gather, float x, float z,
		CTurretValue::SCandidate& out) const
{
	AIFloat3 pos(x, 0.f, z);
	CTerrainManager::CorrectPosition(pos);
	if (pos.SqDistance2D(gather.centre) > SQUARE(gather.radius)) {
		return Reject::RADIUS;
	}
	if (circuit->GetMilitaryManager()->GetSpotScorer()->IsMasked(pos)) {
		return Reject::MASK;
	}
	const float sqInfl = SQUARE(inflRadius);
	if (std::none_of(gather.ourBuildings.begin(), gather.ourBuildings.end(), [&](const AIFloat3& b) {
		return b.SqDistance2D(pos) <= sqInfl;
	})) {
		return Reject::INFLUENCE;
	}
	if (circuit->GetThreatMap()->GetThreatAt(builder, pos) > THREAT_MIN) {
		return Reject::THREAT;
	}
	if (!circuit->GetTerrainManager()->CanReachAt(builder, pos, builder->GetCircuitDef()->GetBuildDistance())) {
		return Reject::REACH;
	}
	FillCandidate(builder, gather, pos, out);
	return Reject::NONE;
}

CTurretPicker::Reject CTurretPicker::FinishCandidate(CCircuitDef* def, CCircuitUnit* builder, const SGather& gather,
		CTurretValue::SCandidate& c) const
{
	// Buildable there facing south, and open on the blocking map (lanes, queued jobs): on the build grid, at most a
	// block square away
	const AIFloat3 site = circuit->GetTerrainManager()->FindSiteAt(def, AIFloat3(c.pos.x, c.pos.y, c.pos.z),
			UNIT_FACING_SOUTH);
	if (!utils::is_valid(site)) {
		return Reject::SITE;
	}
	if (!circuit->GetBuilderManager()->IsTurretJobSafe(builder, site)) {
		return Reject::UNSAFE;
	}
	FillCandidate(builder, gather, site, c);
	return Reject::NONE;
}

void CTurretPicker::FillCandidate(CCircuitUnit* builder, const SGather& gather, const AIFloat3& pos,
		CTurretValue::SCandidate& out) const
{
	out.pos = {pos.x, circuit->GetTerrainManager()->GetHeightAt(pos.x, pos.z), pos.z};
	out.enemyInfl = 0.f;
	for (const AIFloat3& b : gather.enemyBuildings) {
		const float d = b.distance2D(pos);
		if (d < inflRadius) {
			out.enemyInfl += 2.f * (1.f - d / inflRadius);
		}
	}
	out.walk = builder->GetPos(circuit->GetLastFrame()).distance2D(pos);
}

CTurretPicker::SDefResult CTurretPicker::SearchDef(CCircuitDef* def, CCircuitUnit* builder, const SGather& gather) const
{
	SDefResult res;
	res.def = def;
	std::vector<CTurretValue::SCandidate> candidates;
	const int x0 = (int)std::floor((gather.centre.x - gather.radius) / CELL);
	const int x1 = (int)std::floor((gather.centre.x + gather.radius) / CELL);
	const int z0 = (int)std::floor((gather.centre.z - gather.radius) / CELL);
	const int z1 = (int)std::floor((gather.centre.z + gather.radius) / CELL);
	for (int gz = z0; gz <= z1; ++gz) {
		for (int gx = x0; gx <= x1; ++gx) {
			const float x = (gx + 0.5f) * CELL;
			const float z = (gz + 0.5f) * CELL;
			if ((x < 0.f) || (z < 0.f) || (x > CTerrainManager::GetTerrainWidth())
				|| (z > CTerrainManager::GetTerrainHeight()))
			{
				continue;
			}
			CTurretValue::SCandidate c;
			const Reject reject = MakeCandidate(builder, gather, x, z, c);
			++res.counts[static_cast<int>(reject)];
			if (reject == Reject::NONE) {
				candidates.push_back(c);
				res.passed.push_back(AIFloat3(c.pos.x, c.pos.y, c.pos.z));
			} else if (reject == Reject::INFLUENCE) {
				res.outside.push_back(AIFloat3(x, 0.f, z));
			}
		}
	}
	res.candidates = candidates.size();
	if (candidates.empty()) {
		return res;
	}
	const std::vector<CTurretValue::STarget> targets = MakeTargets(def, gather);
	const float emit = def->GetDef()->GetLosHeight();
	const bool isHighArc = (def->GetDef()->GetHighTrajectoryType() == 1);
	const CSpotScorer* spotScorer = circuit->GetMilitaryManager()->GetSpotScorer();
	res.search = CTurretValue::Search(candidates, targets, MakeParams(def),
			[&](const CTurretValue::SPos& from, const CTurretValue::SPos& to) {
				return isHighArc || spotScorer->IsClearLine(AIFloat3(from.x, from.y, from.z), emit, AIFloat3(to.x, to.y, to.z));
			},
			[&](float x, float z, CTurretValue::SCandidate& out) {
				return (MakeCandidate(builder, gather, x, z, out) == Reject::NONE)
						&& (FinishCandidate(def, builder, gather, out) == Reject::NONE);
			},
			[&](CTurretValue::SCandidate& c) {
				const Reject reject = FinishCandidate(def, builder, gather, c);
				++res.counts[static_cast<int>(reject)];
				return reject == Reject::NONE;
			});
	return res;
}

CTurretValue::SParams CTurretPicker::MakeParams(CCircuitDef* def) const
{
	CTurretValue::SParams p;
	p.range = std::max(def->GetMaxRange(), 1.f);
	p.heightBonus = config.height;
	p.heightCap = config.heightCap;
	p.front = config.front;
	p.travel = config.travel;
	p.minBase = config.minValue * def->GetCostM();
	p.refineStep = CELL * 0.25f;
	return p;
}

float CTurretPicker::GetNearestOurs(const SGather& gather, const AIFloat3& pos) const
{
	float minSq = std::numeric_limits<float>::max();
	for (const AIFloat3& b : gather.ourBuildings) {
		minSq = std::min(minSq, b.SqDistance2D(pos));
	}
	return gather.ourBuildings.empty() ? -1.f : std::sqrt(minSq);
}

} // namespace circuit

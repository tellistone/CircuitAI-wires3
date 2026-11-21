/*
 * AATurretPicker.cpp
 *
 *  Created on: Nov 21, 2026
 *      Author: Claude
 */

#include "module/AATurretPicker.h"
#include "module/BuilderManager.h"
#include "module/MilitaryManager.h"
#include "module/SpotScorer.h"
#include "module/TurretPicker.h"
#include "setup/SetupManager.h"
#include "task/builder/DefenceTask.h"
#include "terrain/CellMask.h"
#include "terrain/ChokeMap.h"
#include "terrain/TerrainManager.h"
#include "unit/ally/AllyUnit.h"
#include "unit/enemy/EnemyUnit.h"
#include "unit/CircuitDef.h"
#include "unit/CircuitUnit.h"
#include "CircuitAI.h"
#include "util/Defines.h"
#include "util/Utils.h"

#include "json/json.h"

#include "Log.h"
#include "spring/SpringCallback.h"
#include "Unit.h"
#include "UnitDef.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace circuit {

using namespace springai;

#define CELL			128.f  // the raster grid of A and G
#define BUCKET			256.f  // assets are summed per cell of this, at their value-weighted centre
#define SPACING			256.f  // between the cells tried for one def
#define MAX_TRIED		64  // cells tried per def
#define MAX_COSTLY		8  // of them that reach the site and reach tests
#define THREAT_RANGE	1200.f  // an enemy that outranges this is artillery: no threat to a spot
#define THREAT_MARGIN	100.f
#define GHOST_FRAMES	(FRAMES_PER_SEC * 30)  // a mobile enemy out of radar and sight longer is no threat
#define LINE_REACH		600.f  // from a live choke's middle
#define BASE_RADIUS		1500.f  // mobile AA this near our base position counts
#define TRAVEL			2000.f  // walk that halves a spot's rank
#define FRAME_FLOOR		0.5f  // a nanoframe's value, at least this share of it
#define JOB_TIMEOUT		(FRAMES_PER_SEC * 60)  // with no builder on it
#define SAVE_FRAMES		(FRAMES_PER_SEC * 60)  // the most a dearer winner is saved for
#define HISTORY_FRAMES	(FRAMES_PER_SEC * 60)  // the rise saving looks at (in two halves), and the mobile AA hold
#define LOG_FRAMES		(FRAMES_PER_SEC * 60)

CAATurretPicker::CAATurretPicker(CCircuitAI* circuit)
		: circuit(circuit)
		, saveFrame(-1)
		, logFrame(-LOG_FRAMES)
		, slowestMs(0.f)
		, queuedMetal(0.f)
{
	gridW = std::max(int(std::ceil(CTerrainManager::GetTerrainWidth() / CELL)), 1);
	gridH = std::max(int(std::ceil(CTerrainManager::GetTerrainHeight() / CELL)), 1);
	bucketW = std::max(int(std::ceil(CTerrainManager::GetTerrainWidth() / BUCKET)), 1);
	ReadConfig();
}

CAATurretPicker::~CAATurretPicker()
{
}

void CAATurretPicker::ReadConfig()
{
	const Json::Value& root = circuit->GetSetupManager()->GetConfig();
	const std::string& cfgName = circuit->GetSetupManager()->GetConfigName();
	const Json::Value& aa = root["aa_turret"];
	SConfig& c = config;
	if (!aa.isObject()) {  // a missing block: off, and the old AA routes as they were
		c.isEnable = false;
		return;
	}
	c.isEnable = aa.get("enable", true).asBool();
	c.factor = std::max(aa.get("factor", c.factor).asFloat(), 0.f);
	c.mobileShare = std::max(aa.get("mobile_share", c.mobileShare).asFloat(), 0.f);
	c.interval = std::max(aa.get("interval", c.interval).asFloat(), 1.f);
	c.minValue = std::max(aa.get("min_value", c.minValue).asFloat(), 0.f);
	c.overlap = std::min(std::max(aa.get("overlap", c.overlap).asFloat(), 0.f), 1.f);
	c.umbrella = std::max(aa.get("umbrella", c.umbrella).asFloat(), 0.f);
	c.line = std::max(aa.get("line", c.line).asFloat(), 0.f);

	// A list of def names (the defaults when the key is missing); an unknown name is logged
	auto readNames = [&](const char* key, const std::vector<std::string>& defaults) {
		std::vector<std::string> names = defaults;
		const Json::Value& list = aa[key];
		if (list.isArray()) {
			names.clear();
			for (const Json::Value& name : list) {
				names.push_back(name.asString());
			}
		} else if (!list.isNull()) {
			circuit->LOG("CONFIG %s: aa_turret.%s must be a list of unit names, defaults used", cfgName.c_str(), key);
		}
		std::vector<CCircuitDef*> defs;
		for (const std::string& name : names) {
			CCircuitDef* cdef = circuit->GetCircuitDef(name.c_str());
			if (cdef == nullptr) {
				circuit->LOG("CONFIG %s: aa_turret.%s has unknown UnitDef '%s'", cfgName.c_str(), key, name.c_str());
			} else {
				defs.push_back(cdef);
			}
		}
		return defs;
	};
	for (CCircuitDef* cdef : readNames("unit", {"turretaalaser", "turretaaclose", "turretaaflak", "turretaafar",
			"turretaaheavy"}))
	{
		if (IsAATower(cdef)) {
			c.defs.push_back(cdef);
		} else {
			circuit->LOG("CONFIG %s: aa_turret.unit '%s' is not a static with the anti_air role, ignored", cfgName.c_str(),
					cdef->GetDef()->GetName());
		}
	}
	// Swift, Raptor, Trident, Owl, Sparrow, Charon, Hercules, Wasp, Crane, Athena
	for (CCircuitDef* cdef : readNames("ignore_air", {"planefighter", "planeheavyfighter", "gunshipaa", "planescout",
			"planelightscout", "gunshiptrans", "gunshipheavytrans", "gunshipcon", "planecon", "athena"}))
	{
		c.ignoreAir.insert(cdef->GetId());
	}

	// mix: def name: [[enemy unit cost, multiplier], ...]; a def without one has M = 1
	const Json::Value& mix = aa["mix"];
	if (mix.isObject()) {
		for (const std::string& name : mix.getMemberNames()) {
			CCircuitDef* cdef = circuit->GetCircuitDef(name.c_str());
			if (cdef == nullptr) {
				circuit->LOG("CONFIG %s: aa_turret.mix has unknown UnitDef '%s'", cfgName.c_str(), name.c_str());
				continue;
			}
			Curve curve;
			CMilitaryManager::ReadCurve(circuit, mix[name], "aa_turret.mix." + name, "enemy unit cost", curve);
			c.mix[cdef->GetId()] = curve;
		}
	} else if (!mix.isNull()) {
		circuit->LOG("CONFIG %s: aa_turret.mix must be an object of unit name: curve, ignored", cfgName.c_str());
	}

	circuit->LOG("AA TURRET config: %s, %i defs, factor %.2f, mobile_share %.2f, every %.0f s, min_value %.2f, "
			"overlap %.2f, umbrella %.2f, line %.2f, %i fliers ignored", c.isEnable ? "on" : "off", (int)c.defs.size(),
			c.factor, c.mobileShare, c.interval, c.minValue, c.overlap, c.umbrella, c.line, (int)c.ignoreAir.size());
}

int CAATurretPicker::GetIntervalFrames() const
{
	return std::max(int(config.interval * FRAMES_PER_SEC), 1);
}

bool CAATurretPicker::IsAATower(const CCircuitDef* cdef) const
{
	return (cdef != nullptr) && !cdef->IsMobile() && !cdef->IsMobileBuilding() && cdef->IsRoleAA();
}

bool CAATurretPicker::IsGroundThreat(const CEnemyInfo* enemy, int frame, float& outReach) const
{
	// Identified only: a radar blip of unknown type may be a plane as well as a tank. Fliers are left out (an AA tower
	// is meant to stand where they are), and so is artillery: a Cerberus or a Big Bertha would block a whole base
	const CCircuitDef* edef = enemy->GetCircuitDef();
	if ((edef == nullptr) || edef->IsAbleToFly() || !edef->IsAttacker() || !edef->HasAntiLand()
		|| enemy->GetData()->IsFake() || enemy->GetData()->IsIgnore() || enemy->IsHidden())
	{
		return false;
	}
	const float range = edef->GetMaxRange(CCircuitDef::RangeType::LAND);
	if ((range <= 0.f) || (range > THREAT_RANGE)) {
		return false;
	}
	// Statics until seen dead; mobiles in radar or sight now, or lately: the ghost of a raider long gone blocks nothing
	if (edef->IsMobile() && !enemy->IsRecentlySeen(frame, GHOST_FRAMES)) {
		return false;
	}
	outReach = range + THREAT_MARGIN;
	return true;
}

bool CAATurretPicker::IsInThreat(const std::vector<SThreat>& threats, const AIFloat3& pos)
{
	return std::any_of(threats.begin(), threats.end(), [&pos](const SThreat& t) {
		return t.pos.SqDistance2D(pos) <= t.sqReach;
	});
}

bool CAATurretPicker::IsThreatened(const AIFloat3& pos) const
{
	const int frame = circuit->GetLastFrame();
	for (const auto& kv : circuit->GetEnemyInfos()) {
		float reach;
		if (IsGroundThreat(kv.second, frame, reach) && (kv.second->GetPos().SqDistance2D(pos) <= SQUARE(reach))) {
			return true;
		}
	}
	return false;
}

float CAATurretPicker::GetMix(SCheck& check, const CCircuitDef* def) const
{
	// M: the def's curve averaged over the known enemy air, weighted by metal; 1 with no curve or no air
	auto it = check.mixCache.find(def->GetId());
	if (it != check.mixCache.end()) {
		return it->second;
	}
	float m = 1.f;
	auto ic = config.mix.find(def->GetId());
	if ((ic != config.mix.end()) && !ic->second.empty() && (check.airMetal > 0.f)) {
		float sum = 0.f;
		for (float cost : check.airCosts) {
			sum += cost * utils::eval_curve(ic->second, cost);
		}
		m = sum / check.airMetal;
	}
	check.mixCache[def->GetId()] = m;
	return m;
}

void CAATurretPicker::GatherEnemies(SCheck& check) const
{
	// The air sum and the mix: every known enemy flier, by its def, unless in ignore_air. The threat list for the spots
	for (const auto& kv : circuit->GetEnemyInfos()) {
		const CEnemyInfo* enemy = kv.second;
		const CCircuitDef* edef = enemy->GetCircuitDef();
		if ((edef == nullptr) || enemy->GetData()->IsFake() || enemy->GetData()->IsIgnore()) {
			continue;
		}
		if (edef->IsAbleToFly()) {
			if (config.ignoreAir.find(edef->GetId()) == config.ignoreAir.end()) {
				const float cost = enemy->GetCost();
				check.airMetal += cost;
				check.airCosts.push_back(cost);
			}
			continue;
		}
		float reach;
		if (IsGroundThreat(enemy, check.frame, reach)) {
			check.threats.push_back({enemy->GetPos(), SQUARE(reach)});
		}
	}
}

void CAATurretPicker::GatherOurs(SCheck& check) const
{
	CBuilderManager* builderMgr = circuit->GetBuilderManager();
	auto addCover = [this, &check](const AIFloat3& pos, const CCircuitDef* cdef, float share) {
		const float range = cdef->GetMaxRange(CCircuitDef::RangeType::AIR);
		if (range > 0.f) {
			check.cover.push_back({pos, SQUARE(range), std::min(GetMix(check, cdef), 1.f) * share});
		}
	};

	// Our AA jobs: those not started (in every queue and chain) count in our AA metal and as cover; the nanoframes
	// a job of ours is on count at full cost
	builderMgr->ForEachQueuedTower([&](IBuilderTask* t) {
		CCircuitDef* cdef = t->GetBuildDef();
		if (!IsAATower(cdef)) {
			return;
		}
		check.queuedAA += cdef->GetCostM();
		addCover(utils::is_valid(t->GetBuildPos()) ? t->GetBuildPos() : t->GetTaskPos(), cdef, 1.f);
		if ((t->GetBuildType() == IBuilderTask::BuildType::DEFENCE) && static_cast<CBDefenceTask*>(t)->IsAAPicker()) {
			++check.ownUnstarted;
		}
	});
	std::unordered_set<int> worked;
	for (IBuilderTask* t : builderMgr->GetTasks(IBuilderTask::BuildType::DEFENCE)) {
		if ((t->GetTarget() != nullptr) && IsAATower(t->GetBuildDef())) {
			worked.insert(t->GetTarget()->GetId());
		}
	}

	// One pass over the ally team's units: who still has units, our AA, the assets, the guns for G and the cover
	circuit->UpdateFriendlyUnits();
	const int frame = check.frame;
	const int ourTeam = circuit->GetTeamId();
	const AIFloat3& basePos = circuit->GetSetupManager()->GetBasePos();
	const bool isBase = utils::is_valid(basePos);
	const CTurretPicker* turretPicker = circuit->GetMilitaryManager()->GetTurretPicker();
	std::unordered_set<int> teams;
	for (const auto& kv : circuit->GetFriendlyUnits()) {
		CAllyUnit* au = kv.second;
		const CCircuitDef* cdef = au->GetCircuitDef();
		if (cdef == nullptr) {
			continue;
		}
		const AIFloat3& pos = au->GetPos(frame);
		if (!utils::is_valid(pos)) {
			continue;
		}
		Unit* unit = au->GetUnit();
		const int team = unit->GetTeam();
		teams.insert(team);
		const bool isOurs = (team == ourTeam);
		const bool isBuilt = !unit->IsBeingBuilt();
		const float cost = cdef->GetCostM();
		if (cdef->IsMobile()) {
			// Mobile AA at half cost, only at our base: the army's AA guards the front, not the base
			if (isOurs && isBuilt && cdef->IsRoleAA()) {
				if (isBase && (pos.SqDistance2D(basePos) <= SQUARE(BASE_RADIUS))) {
					check.mobileAA += cost * config.mobileShare;
				} else {
					check.mobileAway += cost;
				}
			}
			continue;
		}
		if (cdef->IsMobileBuilding()) {  // Claw mines, missiles: no building to guard
			continue;
		}
		if (IsAATower(cdef)) {  // cover, and no asset: AA doesn't gather around AA
			// A frame nobody of ours builds counts at its progress, so an abandoned one can't block the budget or
			// leave the buildings around it uncovered
			const float share = (isBuilt || (worked.find(au->GetId()) != worked.end())) ? 1.f : unit->GetBuildProgress();
			addCover(pos, cdef, share);
			if (isOurs) {
				check.staticAA += share * cost;
			}
			continue;
		}
		const float value = cost * cdef->GetAAValue() * (isBuilt ? 1.f : std::max(unit->GetBuildProgress(), FRAME_FLOOR));
		if (value > 0.f) {
			const int bx = std::min(std::max(int(pos.x / BUCKET), 0), bucketW - 1);
			const int bz = std::max(int(pos.z / BUCKET), 0);
			SBucket& b = check.buckets[bz * bucketW + bx];
			b.value += value;
			b.x += pos.x * value;
			b.z += pos.z * value;
		}
		if (isBuilt && (turretPicker != nullptr) && turretPicker->IsGroundGun(cdef)) {
			check.guns.push_back({pos, cdef->GetMaxRange(CCircuitDef::RangeType::LAND), cost});
		}
	}
	check.players = std::max((int)teams.size(), 1);
	for (auto& kv : check.buckets) {
		SBucket& b = kv.second;
		b.pos = AIFloat3(b.x / b.value, 0.f, b.z / b.value);
	}

	for (CCircuitUnit* worker : builderMgr->GetWorkers()) {
		check.workers.push_back({worker, worker->GetPos(frame)});
	}

	// The live chokes of our current line and of the enemy line, at their middles
	const CChokeMap* chokeMap = circuit->GetChokeMap();
	if (chokeMap->IsReady()) {
		const std::vector<CRegionGraph::SChoke>& chokes = chokeMap->GetGraph().GetChokes();
		auto addLine = [&](const std::vector<int>& line, std::vector<AIFloat3>& out) {
			for (int c : line) {
				if ((c >= 0) && (c < (int)chokes.size()) && chokeMap->IsChokeLive(c, frame)) {
					out.push_back(chokeMap->GetCellPos(chokes[c].centre));
				}
			}
		};
		addLine(chokeMap->GetCurrentLine(), check.ourLine);
		addLine(chokeMap->GetEnemyLine(), check.enemyLine);
	}
}

float CAATurretPicker::GetTargetAgo(int frame, int ago) const
{
	for (auto it = history.rbegin(); it != history.rend(); ++it) {
		if (it->frame <= frame - ago) {
			return it->target;
		}
	}
	return history.empty() ? 0.f : history.front().target;
}

const CAATurretPicker::SWorker* CAATurretPicker::NearestWorker(const SCheck& check, const AIFloat3& pos,
		CCircuitDef* def)
{
	const SWorker* best = nullptr;  // def: one that builds it; nullptr: any
	float minSq = std::numeric_limits<float>::max();
	for (const SWorker& w : check.workers) {
		const float sq = w.pos.SqDistance2D(pos);
		if ((sq < minSq) && ((def == nullptr) || w.unit->GetCircuitDef()->CanBuild(def))) {
			minSq = sq;
			best = &w;
		}
	}
	return best;
}

std::vector<CCircuitDef*> CAATurretPicker::GetUsableDefs(const SCheck& check) const
{
	std::vector<CCircuitDef*> defs;
	for (CCircuitDef* def : config.defs) {
		if (def->IsAvailable(check.frame) && std::any_of(check.workers.begin(), check.workers.end(),
			[def](const SWorker& w) { return w.unit->GetCircuitDef()->CanBuild(def); }))
		{
			defs.push_back(def);
		}
	}
	return defs;
}

void CAATurretPicker::Update()
{
	if (!config.isEnable) {
		return;
	}
	const auto t0 = std::chrono::steady_clock::now();
	SCheck check;
	check.frame = circuit->GetLastFrame();
	GatherEnemies(check);
	if (check.airMetal > 0.f) {  // with no enemy air the check is the enemy pass alone
		GatherOurs(check);
	}
	check.target = check.airMetal * config.factor / check.players;
	history.push_back({check.frame, check.target, check.mobileAA});
	while ((history.size() > 1) && (history[1].frame <= check.frame - HISTORY_FRAMES)) {
		history.pop_front();  // one at or before HISTORY_FRAMES ago is kept
	}
	for (const SSample& s : history) {
		if (s.frame > check.frame - HISTORY_FRAMES) {
			check.mobileHeld = std::max(check.mobileHeld, s.mobileAA);
		}
	}
	check.net = check.target - (check.staticAA + check.queuedAA + check.mobileHeld);

	// why: an early return, logged once until it changes; detail goes with it but doesn't count as a change
	auto finish = [this, &check, &t0](const std::string& why, const std::string& detail = "") {
		const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
		slowestMs = std::max(slowestMs, ms);
		if (!why.empty() && (why != lastWhy)) {
			circuit->LOG("AA TURRET %s%s", why.c_str(), detail.c_str());
		}
		lastWhy = why;
		if (check.frame >= logFrame + LOG_FRAMES) {
			LogBudget(check, slowestMs);
		}
	};

	if (check.airMetal <= 0.f) {
		saveFrame = -1;
		finish("nothing: no enemy air");
		return;
	}
	const std::vector<CCircuitDef*> usable = GetUsableDefs(check);
	if (usable.empty()) {
		finish("nothing: no AA def we can build now");
		return;
	}
	float cheapest = std::numeric_limits<float>::max();
	for (CCircuitDef* def : usable) {
		cheapest = std::min(cheapest, def->GetCostM());
	}
	if (check.net < cheapest) {
		if ((saveFrame >= 0) && (check.frame - saveFrame >= SAVE_FRAMES)) {
			saveFrame = -1;  // a spent save ends at a dip; one under way runs on, so a dip can't restart it
		}
		finish(utils::float_to_string(cheapest, "nothing: net below the cheapest def (%.0f)"));  // net: AA BUDGET
		return;
	}
	if (check.ownUnstarted > 0) {
		finish("nothing: a job of ours is queued and not started");
		return;
	}

	// The defs it may choose from: usable and no dearer than target (net may grow to it), each one's best spot
	std::vector<CCircuitDef*> defs;
	for (CCircuitDef* def : usable) {
		if (def->GetCostM() <= check.target) {
			defs.push_back(def);
		}
	}
	const SResult result = Search(check, defs);
	const SPick& winner = result.best;
	if (!winner.isFound) {
		finish("nothing: no spot", result.why);
		return;
	}

	const SPick* choice = &winner;
	std::string why;
	const float gap = winner.def->GetCostM() - check.net;
	if (gap > 0.f) {
		// Save for it only while enemy air keeps rising: twice the smaller rise of the last two 30 s must cover the
		// gap, so a one-off jump (a fleet just seen) buys now. 60 s at most, from the first check that saved
		const float ago30 = GetTargetAgo(check.frame, HISTORY_FRAMES / 2);
		const float ago60 = GetTargetAgo(check.frame, HISTORY_FRAMES);
		const float projected = 2.f * std::min(check.target - ago30, ago30 - ago60);
		const bool isProjected = (projected >= gap);
		if (isProjected && ((saveFrame < 0) || (check.frame - saveFrame < SAVE_FRAMES))) {
			if (saveFrame < 0) {
				saveFrame = check.frame;
			}
			char buf[160];
			snprintf(buf, sizeof(buf), ": net %.0f, target %.0f, rise %.0f then %.0f over the last two 30 s",
					check.net, check.target, ago30 - ago60, check.target - ago30);
			finish(utils::string_format("saving for %s (%.0f)", winner.def->GetDef()->GetName(),
					winner.def->GetCostM()), buf);
			return;
		}
		why = std::string(isProjected ? ", saved 60 s for " : ", not saving for ") + winner.def->GetDef()->GetName();
		choice = &result.affordable;
		if (!choice->isFound) {
			finish(std::string("nothing: ") + winner.def->GetDef()->GetName()
					+ " costs more than net, and no def net covers has a spot");
			return;
		}
	}
	Queue(check, *choice, why);
	saveFrame = -1;
	finish("");
}

CAATurretPicker::SResult CAATurretPicker::Search(SCheck& check, const std::vector<CCircuitDef*>& defs)
{
	SResult result;
	if (defs.empty()) {
		result.why = ": no def costs at most the target";
		return result;
	}
	const int cells = gridW * gridH;
	auto raster = [this](std::vector<float>& grid, const AIFloat3& p, float r, float v) {
		const float sqR = SQUARE(r);
		ForEachCellIn(gridW, gridH, int(CELL), p.x - r, p.z - r, p.x + r, p.z + r, [&](int x, int z) {
			if (SQUARE((x + 0.5f) * CELL - p.x) + SQUARE((z + 0.5f) * CELL - p.z) <= sqR) {
				grid[z * gridW + x] += v;
			}
		});
	};

	// (d) Overlap: each bucket x overlap^c, c its cover from allied AA and our jobs, each min(M, 1)
	for (auto& kv : check.buckets) {
		SBucket& b = kv.second;
		float c = 0.f;
		for (const SCover& t : check.cover) {
			if (t.pos.SqDistance2D(b.pos) <= t.sqRange) {
				c += t.weight;
			}
		}
		b.disc = b.value * std::pow(config.overlap, c);
	}
	// (a) A per distinct air range, (b) G, rasterised once
	std::unordered_map<int, std::vector<float>> aGrids;
	for (CCircuitDef* def : defs) {
		const int range = int(def->GetMaxRange(CCircuitDef::RangeType::AIR));
		if ((range <= 0) || (aGrids.find(range) != aGrids.end())) {
			continue;
		}
		std::vector<float>& grid = aGrids[range];
		grid.assign(cells, 0.f);
		for (const auto& kv : check.buckets) {
			raster(grid, kv.second.pos, range, kv.second.disc);
		}
	}
	std::vector<float> gGrid(cells, 0.f);
	for (const SGun& g : check.guns) {
		raster(gGrid, g.pos, g.range, g.metal);
	}
	std::vector<float> walkGrid(cells, -1.f);

	std::string reasons;
	int rejects[Reject::_SIZE_] = {0};
	for (CCircuitDef* def : defs) {
		auto it = aGrids.find(int(def->GetMaxRange(CCircuitDef::RangeType::AIR)));
		if (it == aGrids.end()) {
			continue;
		}
		const SPick pick = SearchDef(check, def, it->second, gGrid, walkGrid);
		for (int r = 0; r < Reject::_SIZE_; ++r) {
			rejects[r] += pick.rejects[r];
		}
		if (!pick.isFound) {
			reasons += utils::string_format(", %s best A %.2f x cost (%i worth it)", def->GetDef()->GetName(),
					pick.bestA / std::max(def->GetCostM(), 1.f), pick.candidates);
			continue;
		}
		if ((def->GetCostM() <= check.net) && (!result.affordable.isFound || (pick.score > result.affordable.score))) {
			result.affordable = pick;
		}
		if (!result.best.isFound || (pick.score > result.best.score)) {
			result.best = pick;
		}
	}
	if (!result.best.isFound) {
		result.why = reasons + utils::string_format("; rejected: %i masked, %i on a lane, %i threatened, "
				"%i not buildable, %i out of reach", rejects[Reject::MASK], rejects[Reject::LANE],
				rejects[Reject::THREAT], rejects[Reject::SITE], rejects[Reject::REACH]);
	}
	return result;
}

CAATurretPicker::SPick CAATurretPicker::SearchDef(SCheck& check, CCircuitDef* def, const std::vector<float>& aGrid,
		const std::vector<float>& gGrid, std::vector<float>& walkGrid)
{
	SPick pick;
	pick.def = def;
	const float cost = std::max(def->GetCostM(), 1.f);
	const float worth = std::max(config.minValue * cost, 1.f);
	auto isNear = [](const std::vector<AIFloat3>& points, const AIFloat3& pos) {
		return std::any_of(points.begin(), points.end(), [&](const AIFloat3& p) {
			return p.SqDistance2D(pos) <= SQUARE(LINE_REACH);
		});
	};
	auto centre = [this](int cell) {
		return AIFloat3(((cell % gridW) + 0.5f) * CELL, 0.f, ((cell / gridW) + 0.5f) * CELL);
	};
	std::vector<SCand> cands;
	for (int cell = 0; cell < (int)aGrid.size(); ++cell) {
		const float a = aGrid[cell];
		pick.bestA = std::max(pick.bestA, a);
		if (a < worth) {
			continue;
		}
		const AIFloat3 pos = centre(cell);
		const float u = 1.f + config.umbrella * std::min(gGrid[cell] / cost, 1.f);
		const int lineKind = isNear(check.enemyLine, pos) ? 2 : isNear(check.ourLine, pos) ? 1 : 0;
		float& walk = walkGrid[cell];
		if (walk < 0.f) {
			const SWorker* w = NearestWorker(check, pos, nullptr);
			walk = (w == nullptr) ? 0.f : w->pos.distance2D(pos);
		}
		const float l = 1.f + lineKind * config.line;  // L: 1, 1 + line, 1 + 2 x line
		cands.push_back({cell, a * u * l / (1.f + walk / TRAVEL), u, lineKind, walk});
	}
	pick.candidates = cands.size();
	std::sort(cands.begin(), cands.end(), [](const SCand& a, const SCand& b) { return a.rank > b.rank; });

	// The hard rules, best rank first, on cells 256 elmos apart: the best sit at the value centroid, often a packed
	// base where neighbours fail for the same reason. The cheap ones on up to MAX_TRIED cells, the site and reach
	// tests on MAX_COSTLY of them
	CSpotScorer* spotScorer = circuit->GetMilitaryManager()->GetSpotScorer();
	const CChokeMap* chokeMap = circuit->GetChokeMap();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	std::vector<AIFloat3> tried;
	int costly = 0;
	for (const SCand& c : cands) {
		if (((int)tried.size() >= MAX_TRIED) || (costly >= MAX_COSTLY)) {
			break;
		}
		AIFloat3 pos = centre(c.cell);
		CTerrainManager::CorrectPosition(pos);
		if (std::any_of(tried.begin(), tried.end(), [&pos](const AIFloat3& t) {
			return t.SqDistance2D(pos) < SQUARE(SPACING);
		})) {
			continue;
		}
		tried.push_back(pos);
		if (spotScorer->IsMasked(pos)) {
			++pick.rejects[Reject::MASK];
			continue;
		}
		if (chokeMap->IsOnLane(pos)) {
			++pick.rejects[Reject::LANE];
			continue;
		}
		if (IsInThreat(check.threats, pos)) {
			++pick.rejects[Reject::THREAT];
			continue;
		}
		++costly;
		const AIFloat3 site = terrainMgr->FindSiteAt(def, pos, UNIT_FACING_SOUTH);
		if (!utils::is_valid(site)) {
			++pick.rejects[Reject::SITE];
			continue;
		}
		if (spotScorer->IsMasked(site) || IsInThreat(check.threats, site)) {  // the site search moved it onto one
			++pick.rejects[spotScorer->IsMasked(site) ? Reject::MASK : Reject::THREAT];
			continue;
		}
		const SWorker* builder = NearestWorker(check, site, def);
		if ((builder == nullptr)
			|| !terrainMgr->CanReachAt(builder->unit, site, builder->unit->GetCircuitDef()->GetBuildDistance()))
		{
			++pick.rejects[Reject::REACH];
			continue;
		}
		pick.isFound = true;
		pick.site = site;
		pick.a = aGrid[c.cell];
		pick.g = gGrid[c.cell];
		pick.cand = c;
		pick.m = GetMix(check, def);
		pick.score = c.rank * pick.m / cost;
		break;
	}
	return pick;
}

void CAATurretPicker::Queue(SCheck& check, const SPick& pick, const std::string& why)
{
	// HIGH until begun (CBDefenceTask::SetTarget), so it outranks economy work twice as far and gets the HIGH travel cap
	CCircuitDef* def = pick.def;
	IBuilderTask* task = circuit->GetBuilderManager()->EnqueueTask(IBuilderTask::Priority::HIGH, def, pick.site,
			IBuilderTask::BuildType::DEFENCE, def->GetCostM(), 0.f /*shake*/, true, JOB_TIMEOUT);
	if (task == nullptr) {
		return;
	}
	static_cast<CBDefenceTask*>(task)->SetAAPicker();
	// Its footprint goes on the blocking map now: built on this exact spot or not at all
	task->SetFacing(UNIT_FACING_SOUTH);
	task->SetBuildPos(pick.site);
	queuedMetal += def->GetCostM();

	const float sqRange = SQUARE(def->GetMaxRange(CCircuitDef::RangeType::AIR));
	float raw = 0.f;  // A before the overlap discount
	for (const auto& kv : check.buckets) {
		if (kv.second.pos.SqDistance2D(pick.site) <= sqRange) {
			raw += kv.second.value;
		}
	}
	const char* lineName[] = {"none", "our line", "enemy line"};
	const SCand& c = pick.cand;
	circuit->LOG("AA TURRET queued %s (%.0f) at (%.0f, %.0f)%s: A %.0f (raw %.0f), G %.0f U %.2f, L %.2f (%s), "
			"walk %.0f (/ %.2f), M %.2f, rank x M / cost %.3f; net %.0f of target %.0f", def->GetDef()->GetName(),
			def->GetCostM(), pick.site.x, pick.site.z, why.c_str(), pick.a, raw, pick.g, c.u,
			1.f + c.lineKind * config.line, lineName[c.lineKind], c.walk, 1.f + c.walk / TRAVEL, pick.m, pick.score,
			check.net, check.target);
}

void CAATurretPicker::LogBudget(SCheck& check, float slowMs)
{
	logFrame = check.frame;
	if (check.airMetal <= 0.f) {  // our side isn't gathered then
		circuit->LOG("AA BUDGET no enemy air, queued last minute %.0f, slowest check %.2f ms", queuedMetal, slowMs);
	} else {
		std::string mix;
		for (CCircuitDef* def : config.defs) {
			mix += utils::string_format(" %s %.2f", def->GetDef()->GetName(), GetMix(check, def));
		}
		circuit->LOG("AA BUDGET enemy air %.0f (%i fliers; M:%s), players %i, target %.0f (%.0f 30 s, %.0f 60 s ago), "
				"static AA %.0f, queued %.0f, mobile AA at base %.0f (%.0f now, %.0f away), net %.0f, queued last "
				"minute %.0f, slowest check %.2f ms", check.airMetal, (int)check.airCosts.size(), mix.c_str(),
				check.players, check.target, GetTargetAgo(check.frame, HISTORY_FRAMES / 2),
				GetTargetAgo(check.frame, HISTORY_FRAMES), check.staticAA, check.queuedAA, check.mobileHeld,
				check.mobileAA, check.mobileAway, check.net, queuedMetal, slowMs);
	}
	queuedMetal = 0.f;
	slowestMs = 0.f;
}

} // namespace circuit

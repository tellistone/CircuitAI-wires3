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
#define RISE_FRAMES		(FRAMES_PER_SEC * 60)  // saving only while target is above where it was this long ago
#define BACKOFF_FRAMES	(FRAMES_PER_SEC * 60)  // after a search that found nothing, at most
#define BACKOFF_ASSETS	300.f  // asset value gained that ends the backoff
#define HIT_FRAMES		(FRAMES_PER_SEC * 30)  // a flier's hit on a building of ours makes a job in range HIGH
#define LOG_FRAMES		(FRAMES_PER_SEC * 60)

CAATurretPicker::CAATurretPicker(CCircuitAI* circuit)
		: circuit(circuit)
		, saveFrame(-1)
		, failFrame(-1)
		, failNet(0.f)
		, failAssets(0.f)
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

	// mix: def name: [[enemy unit cost, multiplier], ...]; a missing key, the starting curves
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
	} else {
		if (!mix.isNull()) {
			circuit->LOG("CONFIG %s: aa_turret.mix must be an object of unit name: curve, defaults used",
					cfgName.c_str());
		}
		const std::vector<std::pair<std::string, Curve>> defaults = {
			{"turretaalaser", {{150.f, 1.3f}, {500.f, 1.f}, {2000.f, 0.6f}}},  // Razor
			{"turretaaflak",  {{150.f, 1.5f}, {500.f, 1.f}, {2000.f, 0.5f}}},  // Thresher
			{"turretaaclose", {{150.f, 0.4f}, {500.f, 1.f}, {2000.f, 1.5f}}},  // Hacksaw
			{"turretaafar",   {{150.f, 0.8f}, {500.f, 1.f}, {2000.f, 1.3f}}},  // Chainsaw
			{"turretaaheavy", {{150.f, 0.6f}, {500.f, 1.f}, {2000.f, 1.5f}}}   // Artemis
		};
		for (const auto& kv : defaults) {
			CCircuitDef* cdef = circuit->GetCircuitDef(kv.first.c_str());
			if (cdef != nullptr) {
				c.mix[cdef->GetId()] = kv.second;
			}
		}
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
		|| enemy->GetData()->IsFake() || enemy->IsHidden())
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

void CAATurretPicker::OnUnitDamaged(CCircuitUnit* unit, CEnemyInfo* attacker)
{
	if (!config.isEnable || (attacker == nullptr) || unit->GetCircuitDef()->IsMobile()) {
		return;
	}
	const CCircuitDef* edef = attacker->GetCircuitDef();
	if ((edef == nullptr) || !edef->IsAbleToFly()) {
		return;
	}
	const int frame = circuit->GetLastFrame();
	airHits.erase(std::remove_if(airHits.begin(), airHits.end(), [frame](const SAirHit& h) {
		return frame - h.frame > HIT_FRAMES;
	}), airHits.end());
	const AIFloat3& pos = unit->GetPos(frame);
	for (SAirHit& h : airHits) {  // one per building: each shot of a swarm would add one
		if (h.pos.SqDistance2D(pos) < SQUARE(SQUARE_SIZE * 2)) {
			h.frame = frame;
			return;
		}
	}
	airHits.push_back({pos, frame});
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
	auto addCover = [this, &check](const AIFloat3& pos, const CCircuitDef* cdef) {
		const float range = cdef->GetMaxRange(CCircuitDef::RangeType::AIR);
		if (range > 0.f) {
			check.cover.push_back({pos, SQUARE(range), std::min(GetMix(check, cdef), 1.f)});
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
		addCover(utils::is_valid(t->GetBuildPos()) ? t->GetBuildPos() : t->GetTaskPos(), cdef);
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
			addCover(pos, cdef);
			if (isOurs) {  // an abandoned frame counts at its progress, so it can't block the budget
				check.staticAA += (isBuilt || (worked.find(au->GetId()) != worked.end()))
						? cost : unit->GetBuildProgress() * cost;
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
			check.assets += value;
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

float CAATurretPicker::GetTargetAgo(int frame) const
{
	for (auto it = targets.rbegin(); it != targets.rend(); ++it) {
		if (it->first <= frame - RISE_FRAMES) {
			return it->second;
		}
	}
	return targets.empty() ? 0.f : targets.front().second;
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
	check.net = check.target - (check.staticAA + check.queuedAA + check.mobileAA);
	targets.push_back({check.frame, check.target});
	while ((targets.size() > 1) && (targets[1].first <= check.frame - RISE_FRAMES)) {
		targets.pop_front();  // one at or before RISE_FRAMES ago is kept
	}

	auto finish = [this, &check, &t0](const std::string& why) {
		const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
		slowestMs = std::max(slowestMs, ms);
		if (!why.empty() && (why != lastWhy)) {  // an early return, once until it changes
			circuit->LOG("AA TURRET %s", why.c_str());
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
		saveFrame = -1;  // nothing to save from
		finish(utils::float_to_string(cheapest, "nothing: net below the cheapest def (%.0f)"));  // net: AA BUDGET
		return;
	}
	if (check.ownUnstarted > 0) {
		finish("nothing: a job of ours is queued and not started");
		return;
	}
	if ((failFrame >= 0) && (check.frame - failFrame < BACKOFF_FRAMES) && (check.net < failNet + cheapest)
		&& (check.assets < failAssets + BACKOFF_ASSETS))
	{
		finish("nothing: the last search found no spot, and nothing has changed since");
		return;
	}

	// The defs it may choose from: usable and no dearer than target (net may grow to it), each one's best spot
	std::vector<CCircuitDef*> defs;
	for (CCircuitDef* def : usable) {
		if (def->GetCostM() <= check.target) {
			defs.push_back(def);
		}
	}
	SPick winner = Search(check, defs);
	if (!winner.isFound) {
		failFrame = check.frame;
		failNet = check.net;
		failAssets = check.assets;
		finish(lastSearch);
		return;
	}
	failFrame = -1;

	const SPick* choice = &winner;
	const char* why = "";
	if (winner.def->GetCostM() > check.net) {
		// Save for it while enemy air is growing, 60 s at most from the first check that chose to. The clock doesn't
		// restart when the winner changes, and ends on a purchase
		const float ago = GetTargetAgo(check.frame);
		const bool isRising = (check.target > ago + 1.f);
		if (isRising && (saveFrame < 0)) {
			saveFrame = check.frame;
		}
		if (isRising && (check.frame - saveFrame < SAVE_FRAMES)) {
			char buf[256];
			snprintf(buf, sizeof(buf), "saving for %s (%.0f): net %.0f, target %.0f (%.0f 60 s ago), %i s of %i",
					winner.def->GetDef()->GetName(), winner.def->GetCostM(), check.net, check.target, ago,
					(check.frame - saveFrame) / FRAMES_PER_SEC, SAVE_FRAMES / FRAMES_PER_SEC);
			finish(buf);
			return;
		}
		choice = nullptr;
		for (const SPick& p : affordable) {
			if (p.isFound && ((choice == nullptr) || (p.score > choice->score))) {
				choice = &p;
			}
		}
		why = isRising ? ", saved 60 s for " : ", not saving (target flat) for ";
		if (choice == nullptr) {
			finish(std::string("nothing: ") + (isRising ? "saved 60 s" : "not saving: target flat") + " for "
					+ winner.def->GetDef()->GetName() + ", and no affordable def has a spot");
			return;
		}
	}
	Queue(check, *choice, (choice == &winner) ? "" : (std::string(why) + winner.def->GetDef()->GetName()).c_str());
	saveFrame = -1;
	finish("");
}

CAATurretPicker::SPick CAATurretPicker::Search(SCheck& check, const std::vector<CCircuitDef*>& defs)
{
	affordable.clear();
	lastSearch.clear();
	SPick best;
	if (defs.empty()) {
		lastSearch = "nothing: no def costs at most the target";
		return best;
	}
	const int cells = gridW * gridH;
	auto raster = [this](std::vector<float>& grid, const AIFloat3& p, float r, float v) {
		const int x0 = std::max(int((p.x - r) / CELL), 0);
		const int x1 = std::min(int((p.x + r) / CELL), gridW - 1);
		const int z0 = std::max(int((p.z - r) / CELL), 0);
		const int z1 = std::min(int((p.z + r) / CELL), gridH - 1);
		const float sqR = SQUARE(r);
		for (int z = z0; z <= z1; ++z) {
			const float dz = (z + 0.5f) * CELL - p.z;
			for (int x = x0; x <= x1; ++x) {
				const float dx = (x + 0.5f) * CELL - p.x;
				if (SQUARE(dx) + SQUARE(dz) <= sqR) {
					grid[z * gridW + x] += v;
				}
			}
		}
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
		SPick pick = SearchDef(check, def, it->second, gGrid, walkGrid);
		for (int r = 0; r < Reject::_SIZE_; ++r) {
			rejects[r] += pick.rejects[r];
		}
		if (!pick.isFound) {
			reasons += utils::string_format(", %s best A %.2f x cost (%i worth it)", def->GetDef()->GetName(),
					pick.bestA / std::max(def->GetCostM(), 1.f), pick.candidates);
			continue;
		}
		if (def->GetCostM() <= check.net) {
			affordable.push_back(pick);
		}
		if (!best.isFound || (pick.score > best.score)) {
			best = pick;
		}
	}
	if (!best.isFound) {
		lastSearch = "nothing: no spot" + reasons + utils::string_format("; rejected: %i masked, %i on a lane, "
				"%i threatened, %i not buildable, %i out of reach", rejects[Reject::MASK], rejects[Reject::LANE],
				rejects[Reject::THREAT], rejects[Reject::SITE], rejects[Reject::REACH]);
	}
	return best;
}

CAATurretPicker::SPick CAATurretPicker::SearchDef(SCheck& check, CCircuitDef* def, const std::vector<float>& aGrid,
		const std::vector<float>& gGrid, std::vector<float>& walkGrid)
{
	SPick pick;
	pick.def = def;
	const float cost = std::max(def->GetCostM(), 1.f);
	const float worth = std::max(config.minValue * cost, 1.f);
	const float sqLine = SQUARE(LINE_REACH);
	auto isNear = [sqLine](const std::vector<AIFloat3>& points, const AIFloat3& pos) {
		return std::any_of(points.begin(), points.end(), [&](const AIFloat3& p) { return p.SqDistance2D(pos) <= sqLine; });
	};
	struct SCand {
		int cell;
		float rank;
	};
	std::vector<SCand> cands;
	for (int cell = 0; cell < (int)aGrid.size(); ++cell) {
		const float a = aGrid[cell];
		pick.bestA = std::max(pick.bestA, a);
		if (a < worth) {
			continue;
		}
		const AIFloat3 pos(((cell % gridW) + 0.5f) * CELL, 0.f, ((cell / gridW) + 0.5f) * CELL);
		const float u = 1.f + config.umbrella * std::min(gGrid[cell] / cost, 1.f);
		const float l = isNear(check.enemyLine, pos) ? 1.f + 2.f * config.line
				: isNear(check.ourLine, pos) ? 1.f + config.line : 1.f;
		float& walk = walkGrid[cell];
		if (walk < 0.f) {
			float minSq = std::numeric_limits<float>::max();
			for (const SWorker& w : check.workers) {
				minSq = std::min(minSq, w.pos.SqDistance2D(pos));
			}
			walk = check.workers.empty() ? 0.f : std::sqrt(minSq);
		}
		cands.push_back({cell, a * u * l / (1.f + walk / TRAVEL)});
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
		AIFloat3 pos(((c.cell % gridW) + 0.5f) * CELL, 0.f, ((c.cell / gridW) + 0.5f) * CELL);
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
		if (!utils::is_valid(site) || spotScorer->IsMasked(site) || IsInThreat(check.threats, site)) {
			++pick.rejects[Reject::SITE];
			continue;
		}
		CCircuitUnit* builder = nullptr;
		float minSq = std::numeric_limits<float>::max();
		for (const SWorker& w : check.workers) {
			const float sq = w.pos.SqDistance2D(site);
			if ((sq < minSq) && w.unit->GetCircuitDef()->CanBuild(def)) {
				minSq = sq;
				builder = w.unit;
			}
		}
		if ((builder == nullptr)
			|| !terrainMgr->CanReachAt(builder, site, builder->GetCircuitDef()->GetBuildDistance()))
		{
			++pick.rejects[Reject::REACH];
			continue;
		}
		pick.isFound = true;
		pick.site = site;
		pick.a = aGrid[c.cell];
		pick.g = gGrid[c.cell];
		pick.u = 1.f + config.umbrella * std::min(pick.g / cost, 1.f);
		pick.lineKind = isNear(check.enemyLine, pos) ? 2 : isNear(check.ourLine, pos) ? 1 : 0;
		pick.l = (pick.lineKind == 2) ? 1.f + 2.f * config.line : (pick.lineKind == 1) ? 1.f + config.line : 1.f;
		pick.walk = walkGrid[c.cell];
		pick.rank = c.rank;
		pick.m = GetMix(check, def);
		pick.score = pick.rank * pick.m / cost;
		break;
	}
	return pick;
}

void CAATurretPicker::Queue(SCheck& check, const SPick& pick, const char* why)
{
	CCircuitDef* def = pick.def;
	const float range = def->GetMaxRange(CCircuitDef::RangeType::AIR);
	bool isHit = false;
	for (const SAirHit& h : airHits) {
		isHit |= (check.frame - h.frame <= HIT_FRAMES) && (h.pos.SqDistance2D(pick.site) <= SQUARE(range));
	}
	const IBuilderTask::Priority priority = isHit ? IBuilderTask::Priority::HIGH : IBuilderTask::Priority::NORMAL;
	IBuilderTask* task = circuit->GetBuilderManager()->EnqueueTask(priority, def, pick.site,
			IBuilderTask::BuildType::DEFENCE, def->GetCostM(), 0.f /*shake*/, true, JOB_TIMEOUT);
	if (task == nullptr) {
		return;
	}
	static_cast<CBDefenceTask*>(task)->SetAAPicker();
	// Its footprint goes on the blocking map now: built on this exact spot or not at all
	task->SetFacing(UNIT_FACING_SOUTH);
	task->SetBuildPos(pick.site);
	queuedMetal += def->GetCostM();

	float raw = 0.f;  // A before the overlap discount
	for (const auto& kv : check.buckets) {
		if (kv.second.pos.SqDistance2D(pick.site) <= SQUARE(range)) {
			raw += kv.second.value;
		}
	}
	const char* lineName[] = {"none", "our line", "enemy line"};
	circuit->LOG("AA TURRET queued %s (%.0f) at (%.0f, %.0f)%s%s: A %.0f (raw %.0f), G %.0f U %.2f, L %.2f (%s), "
			"walk %.0f (/ %.2f), M %.2f, rank x M / cost %.3f; net %.0f of target %.0f", def->GetDef()->GetName(),
			def->GetCostM(), pick.site.x, pick.site.z, why, isHit ? ", HIGH: air damage in range" : "", pick.a, raw,
			pick.g, pick.u, pick.l, lineName[pick.lineKind], pick.walk, 1.f + pick.walk / TRAVEL, pick.m, pick.score,
			check.net, check.target);
}

void CAATurretPicker::LogBudget(SCheck& check, float slowMs)
{
	logFrame = check.frame;
	std::string mix;
	for (CCircuitDef* def : config.defs) {
		mix += utils::string_format(" %s %.2f", def->GetDef()->GetName(), GetMix(check, def));
	}
	circuit->LOG("AA BUDGET enemy air %.0f (%i fliers; M:%s), players %i, target %.0f (%.0f 60 s ago), static AA %.0f, "
			"queued %.0f, mobile AA at base %.0f (%.0f away), net %.0f, queued last minute %.0f, slowest check %.2f ms",
			check.airMetal, (int)check.airCosts.size(), mix.c_str(), check.players, check.target,
			GetTargetAgo(check.frame), check.staticAA, check.queuedAA, check.mobileAA, check.mobileAway, check.net,
			queuedMetal, slowMs);
	queuedMetal = 0.f;
	slowestMs = 0.f;
}

} // namespace circuit

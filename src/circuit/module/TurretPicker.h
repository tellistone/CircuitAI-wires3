/*
 * TurretPicker.h
 *
 *  Created on: Nov 11, 2026
 *      Author: Claude
 */

#ifndef SRC_CIRCUIT_MODULE_TURRETPICKER_H_
#define SRC_CIRCUIT_MODULE_TURRETPICKER_H_

#include "module/SpotScorer.h"
#include "module/TurretValue.h"
#include "task/builder/BuilderTask.h"

#include "AIFloat3.h"

#include <limits>
#include <string>
#include <vector>

namespace circuit {

class CCircuitAI;
class CCircuitDef;
class CCircuitUnit;
class CBDefenceTask;
class CEnemyInfo;

// What a caller asks CTurretPicker for: who builds, where to look, what may be built
struct STurretOrder {
	CCircuitUnit* builder = nullptr;
	springai::AIFloat3 centre = -RgtVector;  // -RgtVector: where the builder stands
	float radius = -1.f;  // < 0: turret.radius
	std::vector<CCircuitDef*> defs;  // empty: the porcupine list's land or water towers, AA left out
	IBuilderTask::Priority priority = IBuilderTask::Priority::NORMAL;
	const char* caller = "test";  // "after flee", "near enemy" or "test", for the log
	// turret.creep (CCreepPlanner): the lane's line choke id; -1: not the creep's. Enemy statics' range + creep_margin
	// is the threat rule, mobiles under cover pass, and the job is left queued at priority for any builder looking for
	// work (builder: whom the spot is searched for)
	int creepLane = -1;
	bool isAlly = true;  // false: no A (near enemy, enemy line), so the turret goes for the enemy, not to cover ours
	bool isCrowdCheck = true;  // false: never refused for turret.crowd_count (a person's test order, the creep's steps)
	bool IsCreep() const { return creepLane >= 0; }
};

/*
 * turret.*: the best turret spot within a radius of any centre, with no defence point, choke or cluster behind it
 * (CSpotScorer guards one asset; this weighs everything around the spot). Each term is in metal, so a spot's worth
 * compares with the turret's cost (see CTurretValue):
 *   T, traffic: the traffic field over the cells in range, per cell length of its range across (2 x range / cell),
 *      is the income whose enemy routes pass under it in metal/s; x turret.horizon s. None while spiders and jumpers
 *      are choke.gate_share of the enemy army (until they fall below half of that)
 *   A, our and allied unarmed buildings in range, at cost; our economy there (ECON) only above what C carries of it
 *   E, known enemy buildings in range seen in the last 3 minutes, at cost; an enemy mex at its income x
 *      turret.horizon. Mobiles don't count: they move on, and their routes are in T already
 *   C, turret.choke when a choke of our current line or the enemy line has its middle in range; a current line
 *      choke adds turret.choke_econ x our economy (energy, pylons, factories on our held ground) split among the
 *      current line's chokes by the traffic through each, an enemy line choke turret.choke_enemy (one on both lines, both),
 *      at most turret.choke_cap, none for a choke not settled or with enemy artillery on it;
 *      x (1 - turret.choke_depth x its distance / range): worth the most on the choke, so a turret stands at it, not
 *      just reaching it with its outer range
 * each one the def hits, in a clear line of fire (high-arc: any), and halved for every tower of ours or an ally's,
 * queued too, that covers it already and hits it. F, the front, is the known enemy buildings' influence at the spot,
 * 2 x (1 - d / 2048) each (the enemy territory layer's), over the highest among the candidates. V, the view
 * (turret.view), is the share of its range circle's ground in a clear line of fire from it. A spot must be
 * within the radius, within defence.infl_rad of a building of ours or an ally's (constructors left out), buildable
 * and open on the blocking map, out of the builder's threat, in its reach (and IsCommJobSafe for a builder whose
 * flee is on), and not masked by a build the engine refused or a job dropped there. The def with the best value per
 * metal wins; it is built on that exact spot or not at all. A Claw mine, a missile or anything else with
 * customParams.mobilebuilding is no building or tower to any of these, nor is turret.not_defence a tower
 */
class CTurretPicker {
public:
	struct SConfig {
		float radius = 600.f;
		float minValue = 1.f;
		float traffic = 1.f;
		float horizon = 120.f;  // seconds
		float ally = 1.f;
		float enemy = 1.f;
		float front = 1.f;
		float foremost = 0.f;  // metal: a spot nearer the enemy's base than any ground turret of ours or an ally's
		float view = 1.f;
		float height = 0.5f;
		float choke = 100.f;  // metal
		float chokeDepth = 1.f;  // C x (1 - this x the choke's distance / range)
		float chokeEcon = 1.f;  // x our economy, split among the current line chokes by traffic, added to C
		float chokeEnemy = 400.f;  // metal an enemy line choke adds to C
		float chokeCap = 1000.f;  // the most C one choke carries; 0 = no cap
		// Armed statics that defend nothing in the field: no tower covering a target, nor a defence to the callers
		std::vector<CCircuitDef*> notDefence;
		// Callers (CBuilderManager::UpdateTurrets)
		bool isAfterFlee = true;
		float afterFleeSince = 0.f;  // seconds of game time before the after-flee caller starts
		float defendedRadius = 600.f;  // a defence this near where a flee started (or our artillery) means no turret
		bool isNearEnemy = true;
		bool isShortChoke = true;  // near a short choke of our current line (CFrontline::SFront::IsShort)
		float shortChokeReach = 600.f;
		bool isEnemyLine = true;  // near a choke of the enemy line
		float enemyLineReach = 800.f;
		float enemyLineBudget = 0.5f;  // x the balance the account must hold for a choke on the enemy line (0 to 1)
		bool isArtillery = true;  // near our artillery shooting a target in its range, no turret within defendedRadius
		float artilleryReach = 600.f;
		std::vector<CCircuitDef*> light;
		// A light def that would stand within crowdRadius of crowdCount light turrets of the ally team (finished or
		// being built, and our jobs not started yet) isn't placed: by the picker (not the creep), as a preventive or
		// porc tower, a hub chain's, a base tower or a radar guard (not a cornered builder's or a mex escort); the turret
		// account (porcupine.budget) gets crowdCredit instead, up to its cap. crowdCount 0 = off
		float crowdRadius = 600.f;
		int crowdCount = 3;
		float crowdCredit = 90.f;  // metal
		// Heavier defs, each tier unlocked once our ally team holds mexShare of the map's metal spots; by mexShare up
		struct STier {
			float mexShare;
			std::vector<CCircuitDef*> defs;
		};
		std::vector<STier> tiers;
		float enemyReach = 700.f;
		float allyRadius = 500.f;
		float cooldown = 30.f;  // seconds
		// The creep (CCreepPlanner): builders step turrets forward to just outside enemy static range
		bool isCreep = true;
		int creepBuilders = 1;  // steps queued or under way at once, each a HIGH job for any builder
		float creepIncome = 30.f;  // metal/s of income per extra step at once; 0 = none
		float creepInterval = 10.f;  // seconds between orders on one lane
		float creepStep = 0.7f;  // x the forward turret's range
		float creepMargin = 64.f;  // elmos kept outside enemy static range
		float creepRadius = 250.f;  // the picker's radius about the step's end
		int creepLosses = 2;  // turrets lost near a step within 2 minutes stop it
		float creepBudget = 0.5f;  // x the balance the turret account must hold (0 to 1); the full cost is charged
	};
	enum class Reject: char {NONE, RADIUS, MASK, LANE, INFLUENCE, THREAT, REACH, SITE, UNSAFE, _SIZE_};
	// The cheap hard rules over the candidate grid of one order: the same for every def
	struct SGrid {
		std::vector<CTurretValue::SCandidate> passed;
		std::vector<springai::AIFloat3> outside;  // rejected by the influence rule
		int counts[static_cast<int>(Reject::_SIZE_)] = {0};
	};
	// One def's search over the grid
	struct SDefResult {
		CCircuitDef* def = nullptr;
		CTurretValue::SResult search;
		int counts[static_cast<int>(Reject::_SIZE_)] = {0};  // the costly rules (SITE, UNSAFE)
	};
	// The last order, for choke_overlay turrets: the def built, else the last tried
	struct SLast {
		int frame = -1;
		springai::AIFloat3 centre;
		float radius = 0.f;
		bool isFound = false;
		SGrid grid;
		SDefResult res;
		std::string line;  // the log line
	};

	CTurretPicker(CCircuitAI* circuit);
	virtual ~CTurretPicker();

	// Picks the def and spot and queues the DEFENCE job there for the builder; nullptr if nothing is worth its cost
	IBuilderTask* Order(const STurretOrder& order);
	// On arrival of builder: the job's spot is still out of its threat, and worth KEEP_SHARE of the order's threshold
	bool IsStillWorth(const CBDefenceTask* task, CCircuitUnit* builder);

	// A static that defends the ground around it: armed, not a Claw mine or a missile (customParams.mobilebuilding), not
	// in turret.not_defence
	bool IsDefence(const CCircuitDef* cdef) const;
	bool IsGroundGun(const CCircuitDef* cdef) const;  // IsDefence and hits ground: what the creep steps and fears
	// The creep's threat rule at pos for builder (as its orders and their arrival use it): the creep waits on it
	bool IsCreepThreatened(CCircuitUnit* builder, const springai::AIFloat3& pos) const;
	// A known enemy building: not a fake, a Claw mine or a missile. isFresh: and in radar or sight in the last 3
	// minutes, so the ghost of one that died out of sight drops out
	bool IsEnemyBuilding(const CEnemyInfo* enemy, bool isFresh) const;

	// Our ally team's share of the map's metal spots, finished mexes only
	float GetMexShare() const;
	// Traffic of the field summed over a range's cells, in metal: x turret.horizon x weight, once per route
	float TrafficMetal(float amount, float range, float weight) const;
	// E before its weight: known enemy buildings within reach of centre seen in the last 3 minutes, at cost, an enemy mex
	// at its income x turret.horizon. Shared with CSpotScorer::FindSpot
	std::vector<CTurretValue::STarget> GetEnemyTargets(const springai::AIFloat3& centre, float reach) const;
	// A and C for CSpotScorer::FindSpot, as an order counts them for def at a spot within radius of centre (weighed,
	// line-tested for what def hits, halved for covering towers), and the known enemy buildings F reads there
	struct SSpotShare {
		std::vector<CTurretValue::STarget> targets;  // A and C
		std::vector<springai::AIFloat3> enemyBuildings;  // within radius + defence.infl_rad
	};
	SSpotShare GetSpotShare(CCircuitDef* def, const springai::AIFloat3& centre, float radius) const;
	// F before it is normalised (CTurretValue::SCandidate::enemyInfl): 2 x (1 - d / defence.infl_rad) per building
	float GetEnemyInfl(const std::vector<springai::AIFloat3>& enemyBuildings, const springai::AIFloat3& pos) const;
	// The turret.tiers entry def is in, from 1; 0 if none (turret.light)
	int GetTier(const CCircuitDef* def) const;
	bool IsLight(const CCircuitDef* def) const;
	// turret.crowd_count: true when def is light and pos has that many other light turrets within turret.crowd_radius:
	// the caller doesn't place it. Logs the refusal and credits the turret account turret.crowd_credit
	bool RefuseCrowdedLight(const CCircuitDef* def, const springai::AIFloat3& pos, const char* caller);

	SConfig& GetConfig() { return config; }
	const SConfig& GetConfig() const { return config; }
	const SLast& GetLast() const { return last; }
	bool IsAnyCaller() const {
		return config.isAfterFlee || config.isNearEnemy || config.isShortChoke || config.isEnemyLine || config.isArtillery;
	}

private:
	// Gathered once per order
	struct SGun {  // an armed static for the creep's threat rule
		springai::AIFloat3 pos;
		float sqReach;
		float power;
	};
	struct SGather {
		springai::AIFloat3 centre;
		float radius;
		bool isCreep = false;
		bool isAlly = true;  // STurretOrder::isAlly
		std::vector<SGun> enemyGuns;  // known enemy statics that hit ground, reach = range + creep_margin
		std::vector<SGun> ourGuns;  // finished ones of ours and allies' that hit ground, reach = range
		// Within radius + the longest range, before they are weighed for a def (metal: traffic in metal/s, the rest in
		// metal), halved per covering tower
		std::vector<CTurretValue::STarget> raws;
		std::vector<springai::AIFloat3> ourBuildings;  // within radius + defence.infl_rad
		std::vector<springai::AIFloat3> enemyBuildings;  // within radius + ENEMY_INFL_RADIUS
		// turret.foremost: the enemy's bases (CChokeMap::GetEnemyBases), and our ground turrets' nearest distance to
		// one (finished, being built or queued, ours and allies'; the order's own tower left out)
		std::vector<springai::AIFloat3> enemyBases;
		float foremostDist = std::numeric_limits<float>::max();
	};

	void ReadConfig();
	std::vector<CCircuitDef*> GetDefs(const STurretOrder& order, const springai::AIFloat3& centre) const;
	SGather Gather(const springai::AIFloat3& centre, float radius, float maxRange,
			const springai::AIFloat3& skipTower, bool isTraffic = true) const;
	std::vector<CTurretValue::STarget> MakeTargets(CCircuitDef* def, const SGather& gather) const;
	SGrid MakeGrid(CCircuitUnit* builder, const SGather& gather) const;
	// The cheap hard rules at (x, z): NONE and out filled, or the first rule it fails
	Reject MakeCandidate(CCircuitUnit* builder, const SGather& gather, float x, float z,
			CTurretValue::SCandidate& out) const;
	// The costly ones, for a candidate about to be scored: buildable and open there (c moves to the build grid), and
	// safe for the builder
	Reject FinishCandidate(CCircuitDef* def, CCircuitUnit* builder, const SGather& gather,
			CTurretValue::SCandidate& c) const;
	void FillCandidate(CCircuitUnit* builder, const SGather& gather, const springai::AIFloat3& pos,
			CTurretValue::SCandidate& out) const;
	SDefResult SearchDef(CCircuitDef* def, CCircuitUnit* builder, const SGather& gather,
			const std::vector<CTurretValue::SCandidate>& candidates) const;
	CTurretValue::SParams MakeParams(CCircuitDef* def) const;
	float GetNearestOurs(const SGather& gather, const springai::AIFloat3& pos) const;
	float GetForemostBonus(const SGather& gather, const springai::AIFloat3& pos) const;  // turret.foremost, or 0
	void GatherGuns(SGather& gather) const;  // the creep's: enemyGuns and ourGuns about the radius
	// Threat at pos for builder: above THREAT_MIN; for the creep (gather.isCreep), its own rule
	bool IsThreatened(const SGather& gather, CCircuitUnit* builder, const springai::AIFloat3& pos) const;

	CCircuitAI* circuit;
	SConfig config;
	float inflRadius;  // defence.infl_rad in elmos
	SLast last;
};

} // namespace circuit

#endif // SRC_CIRCUIT_MODULE_TURRETPICKER_H_

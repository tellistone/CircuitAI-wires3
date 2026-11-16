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

#include <string>
#include <vector>

namespace circuit {

class CCircuitAI;
class CCircuitDef;
class CCircuitUnit;
class CBDefenceTask;

// What a caller asks CTurretPicker for: who builds, where to look, what may be built
struct STurretOrder {
	CCircuitUnit* builder = nullptr;
	springai::AIFloat3 centre = -RgtVector;  // -RgtVector: where the builder stands
	float radius = -1.f;  // < 0: turret.radius
	std::vector<CCircuitDef*> defs;  // empty: the porcupine list's land or water towers, AA left out
	IBuilderTask::Priority priority = IBuilderTask::Priority::NORMAL;
	const char* caller = "test";  // "after flee", "near enemy" or "test", for the log
};

/*
 * turret.*: the best turret spot within a radius of any centre, with no defence point, choke or cluster behind it
 * (CSpotScorer guards one asset; this weighs everything around the spot). Each term is in metal, so a spot's worth
 * compares with the turret's cost (see CTurretValue):
 *   T, traffic: the traffic field over the cells in range, per cell length of its range across (2 x range / cell),
 *      is the income whose enemy routes pass under it in metal/s; x turret.horizon s
 *   A, our and allied unarmed buildings in range, at cost
 *   E, known enemies it could shoot (choke.spot.enemy's rules), at cost; an enemy mex at its income x turret.horizon
 *   C, turret.choke when a choke of our current line or the enemy line has its middle in range
 * each in a clear line of fire (high-arc: any) and halved for every tower of ours or an ally's, queued too, that
 * covers it already. F, the front, is the known enemy buildings' influence at the spot, 2 x (1 - d / 2048) each (the
 * enemy territory layer's), over the highest among the candidates. A spot must be within the radius, within
 * defence.infl_rad of a building of ours or an ally's (constructors left out), buildable and open on the blocking map,
 * out of the builder's threat, in its reach (and IsCommJobSafe for a builder whose flee is on), and not masked by a
 * build the engine refused. The def with the best value per metal wins; it is built on that exact spot or not at all
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
		float height = 0.5f;
		float heightCap = 150.f;
		float travel = 1000.f;
		float choke = 100.f;  // metal
		// Callers (CBuilderManager::UpdateTurrets)
		bool isAfterFlee = true;
		bool isNearEnemy = true;
		std::vector<CCircuitDef*> light;
		float enemyReach = 600.f;
		float allyRadius = 500.f;
		float cooldown = 30.f;  // seconds
		float share = 0.15f;
	};
	// The last order, for choke_overlay turrets
	struct SLast {
		int frame = -1;
		springai::AIFloat3 centre;
		float radius = 0.f;
		CCircuitDef* def = nullptr;  // the def shown: the one built, else the last tried
		bool isFound = false;
		std::vector<CTurretValue::SScored> scored;
		std::vector<springai::AIFloat3> passed;  // every candidate that passed the cheap hard rules
		std::vector<springai::AIFloat3> outside;  // rejected by the influence rule
		float frontNorm = 0.f;
		std::string line;  // the log line
	};

	CTurretPicker(CCircuitAI* circuit);
	virtual ~CTurretPicker();

	// The best spot for def, -RgtVector if none passes or is worth its cost; outValue = its value in metal
	springai::AIFloat3 FindSpot(CCircuitDef* def, const STurretOrder& order, float& outValue);
	// Picks the def and spot and queues the DEFENCE job there for the builder; nullptr if nothing is worth its cost
	IBuilderTask* Order(const STurretOrder& order);
	// On arrival: the job's spot is still out of threat and worth its cost
	bool IsStillWorth(const CBDefenceTask* task);

	SConfig& GetConfig() { return config; }
	const SLast& GetLast() const { return last; }
	bool IsAnyCaller() const { return config.isAfterFlee || config.isNearEnemy; }

private:
	// A target before it is weighed for a def: traffic in metal/s, the rest in metal; halved per covering tower
	struct SRaw {
		CTurretValue::SPos pos;
		float amount;
		CTurretValue::Kind kind;
		bool isAir;  // an enemy flier: only for defs that hit air
	};
	// Gathered once per order
	struct SGather {
		springai::AIFloat3 centre;
		float radius;
		std::vector<SRaw> raws;  // within radius + the longest range
		std::vector<springai::AIFloat3> ourBuildings;  // within radius + defence.infl_rad
		std::vector<springai::AIFloat3> enemyBuildings;  // within radius + ENEMY_INFL_RADIUS
	};
	enum class Reject: char {NONE, RADIUS, MASK, INFLUENCE, THREAT, REACH, SITE, UNSAFE, _SIZE_};
	struct SDefResult {
		CCircuitDef* def = nullptr;
		CTurretValue::SResult search;
		std::vector<springai::AIFloat3> passed;  // the cheap rules
		std::vector<springai::AIFloat3> outside;
		int counts[static_cast<int>(Reject::_SIZE_)] = {0};
		int candidates = 0;
	};

	void ReadConfig();
	std::vector<CCircuitDef*> GetDefs(const STurretOrder& order, const springai::AIFloat3& centre) const;
	SGather Gather(const springai::AIFloat3& centre, float radius, float maxRange,
			const springai::AIFloat3& skipTower) const;
	std::vector<CTurretValue::STarget> MakeTargets(CCircuitDef* def, const SGather& gather) const;
	// The cheap hard rules at (x, z): NONE and out filled, or the first rule it fails
	Reject MakeCandidate(CCircuitUnit* builder, const SGather& gather, float x, float z,
			CTurretValue::SCandidate& out) const;
	// The costly ones, for a candidate about to be scored: buildable and open there (c moves to the build grid), and
	// safe for the builder
	Reject FinishCandidate(CCircuitDef* def, CCircuitUnit* builder, const SGather& gather,
			CTurretValue::SCandidate& c) const;
	void FillCandidate(CCircuitUnit* builder, const SGather& gather, const springai::AIFloat3& pos,
			CTurretValue::SCandidate& out) const;
	SDefResult SearchDef(CCircuitDef* def, CCircuitUnit* builder, const SGather& gather) const;
	CTurretValue::SParams MakeParams(CCircuitDef* def) const;
	float GetNearestOurs(const SGather& gather, const springai::AIFloat3& pos) const;

	CCircuitAI* circuit;
	SConfig config;
	float inflRadius;  // defence.infl_rad in elmos
	SLast last;
};

} // namespace circuit

#endif // SRC_CIRCUIT_MODULE_TURRETPICKER_H_

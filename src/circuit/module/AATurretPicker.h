/*
 * AATurretPicker.h
 *
 *  Created on: Nov 21, 2026
 *      Author: Claude
 */

#ifndef SRC_CIRCUIT_MODULE_AATURRETPICKER_H_
#define SRC_CIRCUIT_MODULE_AATURRETPICKER_H_

#include "AIFloat3.h"

#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace circuit {

class CCircuitAI;
class CCircuitDef;
class CCircuitUnit;
class CEnemyInfo;

/*
 * aa_turret.*: the one route that queues anti-air turrets while it is on. Every aa_turret.interval seconds:
 *   budget: target = enemy air x factor / our team's players that still have units, net = target - our AA metal.
 *      Enemy air: every known enemy flier whose def isn't in ignore_air (fighters, scouts, transports and air
 *      constructors by default), at cost. Our AA metal: our anti_air statics (a nanoframe at full cost while a job of
 *      ours is on it, else at build progress x cost), our AA jobs not started, and mobile_share x our finished mobile
 *      anti_air units within 1500 elmos of our base position, the most of it seen in the last 60 s (an army stepping
 *      out of base doesn't open the budget). Allies' AA is left out. Nothing is built until net covers the cheapest AA
 *      def we can build, and nothing is ever cancelled, so our AA follows the peak of enemy air
 *   worth: A, the ally team's building value within the def's air range (cost x behaviour.json aa_value, a nanoframe at
 *      max(progress, 0.5) of that; anti_air statics left out), each building's value x overlap^c, c the cover it has
 *      from allied AA turrets and our AA jobs, each counted min(M, 1), a nanoframe nobody of ours builds at its
 *      progress of that. A >= min_value x cost
 *   rank: A x U x L / (1 + walk / 2000). U = 1 + umbrella x min(G / cost, 1), G the finished allied ground turrets'
 *      metal whose land range covers the spot. L = 1 + 2 x line within 600 elmos of a live enemy line choke, 1 + line of
 *      a live choke of our current line, else 1. walk: to our nearest constructor
 *   M, the air mix: each def's mix curve of [enemy unit cost, multiplier], averaged over the enemy air by metal
 *   hard rules, on the best-ranked cells 256 elmos apart: not masked, not on a lane, out of the ground-only threat (an
 *      identified enemy that doesn't fly, hits ground and has a land range of 1200 or less, within it + 100; mobiles
 *      in radar or LOS now or in the last 30 s); then buildable facing south and in reach, on at most 8 cells per def
 *   choice: the def with the best rank x M / cost among defs costing no more than target. Dearer than net: saved for
 *      only while enemy air keeps rising, when twice the smaller of the rises over the last two 30 s covers what net
 *      lacks, and 60 s at most; else the best def net covers
 * The job is an exact-spot DEFENCE job any constructor may take (CBDefenceTask::SetAAPicker): HIGH until begun, then
 * normal unless enemies come near. One unstarted job at a time; one dropped before it began masks its spot
 */
class CAATurretPicker {
public:
	using Curve = std::vector<std::pair<float, float>>;
	struct SConfig {
		bool isEnable = false;  // a missing block: off
		std::vector<CCircuitDef*> defs;
		std::unordered_set<int> ignoreAir;  // def ids
		std::unordered_map<int, Curve> mix;  // def id: [[enemy unit cost, multiplier], ...]
		float factor = 0.25f;
		float mobileShare = 0.5f;
		float interval = 10.f;  // seconds
		float minValue = 1.f;
		float overlap = 0.5f;
		float umbrella = 0.5f;
		float line = 0.25f;
	};

	CAATurretPicker(CCircuitAI* circuit);
	virtual ~CAATurretPicker();

	bool IsEnabled() const { return config.isEnable; }
	int GetIntervalFrames() const;

	void Update();
	// The ground-only threat rule at pos, as it stands now (the job's arrival check)
	bool IsThreatened(const springai::AIFloat3& pos) const;

private:
	struct SThreat {
		springai::AIFloat3 pos;
		float sqReach;
	};
	struct SBucket {  // the ally team's buildings in one 256-elmo cell
		float value = 0.f;  // cost x aa_value
		float x = 0.f;  // value-weighted
		float z = 0.f;
		springai::AIFloat3 pos;
		float disc = 0.f;  // value x overlap^cover
	};
	struct SCover {  // an allied AA turret or our AA job
		springai::AIFloat3 pos;
		float sqRange;
		float weight;  // min(M, 1)
	};
	struct SGun {
		springai::AIFloat3 pos;
		float range;
		float metal;
	};
	struct SWorker {
		CCircuitUnit* unit;
		springai::AIFloat3 pos;
	};
	enum Reject: int {MASK = 0, LANE, THREAT, SITE, REACH, _SIZE_};
	struct SCand {  // a cell worth a def's cost
		int cell;
		float rank;
		float u;
		int lineKind;  // 0: none, 1: our line, 2: enemy line
		float walk;
	};
	struct SPick {
		CCircuitDef* def = nullptr;
		springai::AIFloat3 site;
		float a = 0.f;  // after overlap
		float g = 0.f;
		SCand cand = {-1, 0.f, 1.f, 0, 0.f};
		float m = 1.f;
		float score = -1.f;  // rank x M / cost
		float bestA = 0.f;  // the best A over its cost's threshold or not, for the log
		int candidates = 0;  // cells worth its cost
		int rejects[Reject::_SIZE_] = {0};
		bool isFound = false;  // site passed every rule
	};
	struct SResult {
		SPick best;  // the best score
		SPick affordable;  // the best score among defs net covers
		std::string why;  // when nothing was found
	};
	// What one check gathers
	struct SCheck {
		int frame = 0;
		float airMetal = 0.f;
		std::vector<float> airCosts;  // each known enemy flier's, for M
		std::vector<SThreat> threats;
		int players = 1;
		float staticAA = 0.f;
		float mobileAA = 0.f;  // mobile_share x cost, at base now
		float mobileHeld = 0.f;  // the most of mobileAA over the last 60 s: what net counts
		float mobileAway = 0.f;  // full cost, away from base
		float queuedAA = 0.f;
		int ownUnstarted = 0;
		float target = 0.f;
		float net = 0.f;
		std::unordered_map<int, SBucket> buckets;
		std::vector<SCover> cover;
		std::vector<SGun> guns;
		std::vector<SWorker> workers;
		std::vector<springai::AIFloat3> ourLine;  // live choke middles
		std::vector<springai::AIFloat3> enemyLine;
		std::unordered_map<int, float> mixCache;  // def id: M
	};

	void ReadConfig();
	bool IsAATower(const CCircuitDef* cdef) const;
	static const SWorker* NearestWorker(const SCheck& check, const springai::AIFloat3& pos, CCircuitDef* def);
	// An identified enemy that may shoot a builder at a spot: outReach its land range + margin
	bool IsGroundThreat(const CEnemyInfo* enemy, int frame, float& outReach) const;
	static bool IsInThreat(const std::vector<SThreat>& threats, const springai::AIFloat3& pos);
	float GetMix(SCheck& check, const CCircuitDef* def) const;
	void GatherEnemies(SCheck& check) const;
	void GatherOurs(SCheck& check) const;
	float GetTargetAgo(int frame, int ago) const;  // the target at the check ago frames back, or the oldest kept
	std::vector<CCircuitDef*> GetUsableDefs(const SCheck& check) const;  // available, and a constructor of ours builds it
	SResult Search(SCheck& check, const std::vector<CCircuitDef*>& defs);
	SPick SearchDef(SCheck& check, CCircuitDef* def, const std::vector<float>& aGrid, const std::vector<float>& gGrid,
			std::vector<float>& walkGrid);
	void Queue(SCheck& check, const SPick& pick, const std::string& why);
	void LogBudget(SCheck& check, float slowMs);

	CCircuitAI* circuit;
	SConfig config;
	int gridW;  // 128-elmo cells
	int gridH;
	int bucketW;  // 256-elmo cells
	struct SSample {
		int frame;
		float target;
		float mobileAA;
	};
	std::deque<SSample> history;  // per check, back to the first at or before HISTORY_FRAMES ago
	int saveFrame;  // the check that began saving; -1: not saving
	int logFrame;
	float slowestMs;  // since the last budget log
	float queuedMetal;  // since the last budget log
	std::string lastWhy;  // the last early return, logged once until it changes
};

} // namespace circuit

#endif // SRC_CIRCUIT_MODULE_AATURRETPICKER_H_

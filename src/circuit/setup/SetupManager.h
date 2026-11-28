/*
 * SetupManager.h
 *
 *  Created on: Dec 9, 2014
 *      Author: rlcevg
 */

#ifndef SRC_CIRCUIT_STATIC_SETUPMANAGER_H_
#define SRC_CIRCUIT_STATIC_SETUPMANAGER_H_

#include "unit/CircuitDef.h"
#include "util/Defines.h"
#include "json/json-forwards.h"

#include "AIFloat3.h"

#include <functional>
#include <limits>

namespace circuit {

class CCircuitAI;
class CSetupData;
class CAllyTeam;
class CGameTask;
class CCircuitUnit;

class CSetupManager {
public:
	enum class StartPosType: char {METAL_SPOT = 0, MIDDLE = 1, RANDOM = 2};
	// TODO: class CCommander;
	struct SCommInfo {
		struct SMorph {
			std::vector<std::vector<float>> modules;
			int frame;
		} morph;
		struct SHide {
			int frame;
			float threat;
			bool isAir;
			bool isLatch;  // the threat trigger stays on once tripped
			float sqTaskRad;  // hide mode: optional cap on job distance from base, 0 = off
			// Flee (always on, hide or not): run when a ground danger is within its weapon range + fleeMargin,
			// unless the commander and allies within fleeAllyRadius, minus the danger's threat, add up to
			// fleeAllyThreat; < 0 margin = no flee, < 0 ally threat = flee regardless of cover
			float fleeMargin;
			float fleeAllyThreat;
			float fleeAllyRadius;
		} hide;
		// Jobs a commander stops taking once "time" has passed (commander.json "jobs"): bit i = IBuilderTask::BuildType i
		struct SJobs {
			int frame = std::numeric_limits<int>::max();  // from this frame on; max = never
			unsigned skipMask = 0;
			// jobs.jump: it jumps along its path to a job farther than min_dist (< 0 = never); keep_for_flee: only
			// from and onto safe ground (IBuilderTask::CanJumpTo)
			float jumpMinDist = -1.f;
			bool isJumpKeep = true;
		} jobs;
		// Commander attack (commander.json "attack"): within radius of an enemy it clearly beats, the commander leaves
		// its job and fights it as an attack group (CBuilderManager::TryCommAttack). A missing block: off
		struct SAttack {
			enum Kind: unsigned {STATIC = 0x01, RAID = 0x02, COMM = 0x04};
			bool isEnable = false;
			float radius = 660.f;  // targets within this of the commander
			unsigned kinds = 0;  // "targets": static, raid_target, commander
			float ratio = 1.5f;  // cover over threat at the firing spot and on the walk; kept at 1.0
			float ratioComm = 1.1f;  // duel: their time to kill us over ours to kill them; kept at 1.0
			float minHealth = 0.6f;  // and at least its retreat + 0.15
			float leash = 900.f;  // from where the attacks began (the anchor)
			int maxFrames = FRAMES_PER_SEC * 30;  // per attack; walk + kill under half of it (a third in the opening)
			int cooldownFrames = FRAMES_PER_SEC * 10;  // after any end but a kill; doubles on such ends in a row, to 4x
			bool isInOpening = true;  // false: off until the opening ends
		} attack;
	};
	using StartFunc = std::function<void (const springai::AIFloat3& pos)>;

	CSetupManager(CCircuitAI* circuit, CSetupData* setupData);
	virtual ~CSetupManager();
	void DisabledUnits(const char* setupScript);

	bool OpenConfig(const std::string& profile, const std::vector<std::string>& parts);
	void CloseConfig();
	const Json::Value& GetConfig() const { return *config; }
	const std::string& GetConfigName() const { return configName; }

	bool HasStartBoxes() const;
	bool CanChooseStartPos() const;

	void PickStartPos(StartPosType type);
	void SetStartPos(const springai::AIFloat3& pos) { startPos = basePos = pos; }
	const springai::AIFloat3& GetStartPos() const { return startPos; }
	void SetBasePos(const springai::AIFloat3& pos) { basePos = pos; }
	const springai::AIFloat3& GetBasePos() const { return basePos; }
	void SetLanePos(const springai::AIFloat3& pos) { lanePos = pos; }
	const springai::AIFloat3& GetLanePos() const { return lanePos; }
	void ExecOnFindStart(StartFunc& func) { startFuncs.push_back(func); }

	bool PickCommander();
	CCircuitDef* GetCommChoice() const { return commChoice; }
	void SetCommander(CCircuitUnit* unit) { commander = unit; }
	CCircuitUnit* GetCommander() const { return commander; }

	CAllyTeam* GetAllyTeam() const;

	void ReadConfig();
	float GetEmptyShield() const { return emptyShield; }
	float GetFullShield() const { return fullShield; }
	// Per-unit "retreat_shield" when unset (retreat.shield[0]), clamped to [0, 0.9]
	float GetRetreatShield(float value) const;
	int GetRetreatWait() const { return retreatWait; }

	bool HasModules(const CCircuitDef* cdef, unsigned level) const;
	const std::vector<float>& GetModules(const CCircuitDef* cdef, unsigned level) const;
	int GetMorphFrame(const CCircuitDef* cdef) const;
	const std::vector<CCircuitDef::RoleT>* GetOpener(const CCircuitDef* facDef) const;
	const SCommInfo::SHide* GetHide(const CCircuitDef* cdef) const;
	const SCommInfo::SJobs* GetCommJobs(const CCircuitDef* cdef) const;  // nullptr: not a configured commander
	const SCommInfo::SAttack* GetCommAttack(const CCircuitDef* cdef) const;  // nullptr: not a configured commander

	void Welcome() const;

private:
	const SCommInfo* FindCommInfo(const CCircuitDef* cdef) const;  // by the def name, as commander.json keys it
	void FindStart();
	void CalcStartPos();
	void CalcLanePos();
	springai::AIFloat3 MakeStartPosOffset(const springai::AIFloat3& pos, int clusterId, float range);
	bool LocatePath(std::string& filename);
	bool LoadConfig(const std::string& profile, const std::vector<std::string>& parts);
	Json::Value* ReadConfig(const std::string& dirName, const std::string& profile, const std::vector<std::string>& parts);
	Json::Value* ParseConfig(const std::string& cfgStr, const std::string& cfgName, Json::Value* cfg = nullptr);
	void UpdateJson(Json::Value& a, Json::Value& b);
	void OverrideConfig();

	CCircuitAI* circuit;
	CSetupData* setupData;
	Json::Value* config;  // owner;
	std::string configName;

	CCircuitUnit* commander;
	springai::AIFloat3 startPos;
	springai::AIFloat3 basePos;
	springai::AIFloat3 lanePos;
	std::shared_ptr<CGameTask> findStart;
	std::vector<StartFunc> startFuncs;

	float emptyShield;
	float fullShield;
	int retreatWait;  // frames a retreated unit waits for repair at haven, 0 = no limit

	std::string commPrefix;
	std::string commSuffix;
	CCircuitDef* commChoice;
	std::map<std::string, SCommInfo> commInfos;

	struct SOpener {
		SOpener(float p, const std::vector<CCircuitDef::RoleT>& q) : prob(p), queue(q) {}
		float prob;
		std::vector<CCircuitDef::RoleT> queue;
	};
	struct SStart {
		std::map<CCircuitDef::Id, std::vector<SOpener>> openers;  // fac: openers
		std::vector<CCircuitDef::RoleT> defaultStart;
	};
	std::map<CCircuitDef::Id, SStart> start;  // comm: start
};

} // namespace circuit

#endif // SRC_CIRCUIT_STATIC_SETUPMANAGER_H_

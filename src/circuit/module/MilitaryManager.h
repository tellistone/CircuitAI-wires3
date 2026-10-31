/*
 * MilitaryManager.h
 *
 *  Created on: Sep 5, 2014
 *      Author: rlcevg
 */

#ifndef SRC_CIRCUIT_MODULE_MILITARYMANAGER_H_
#define SRC_CIRCUIT_MODULE_MILITARYMANAGER_H_

#include "module/UnitModule.h"
#include "setup/DefenceMatrix.h"
#include "task/fighter/FighterTask.h"
#include "unit/CircuitDef.h"

#include <vector>
#include <set>

namespace circuit {

class CGameTask;
class CBDefenceTask;
class CRetreatTask;

class CMilitaryManager: public IUnitModule {
public:
	friend class CMilitaryScript;

	CMilitaryManager(CCircuitAI* circuit);
	virtual ~CMilitaryManager();

private:
	void ReadConfig();
	void Init();
public:
	void Release();

	virtual int UnitCreated(CCircuitUnit* unit, CCircuitUnit* builder) override;
	virtual int UnitFinished(CCircuitUnit* unit) override;
	virtual int UnitIdle(CCircuitUnit* unit) override;
	virtual int UnitDamaged(CCircuitUnit* unit, CEnemyInfo* attacker) override;
	virtual int UnitDestroyed(CCircuitUnit* unit, CEnemyInfo* attacker) override;

	const std::set<IFighterTask*>& GetTasks(IFighterTask::FightType type) const {
		return fightTasks[static_cast<IFighterTask::FT>(type)];
	}

	IFighterTask* EnqueueTask(IFighterTask::FightType type);
	IFighterTask* EnqueueDefend(IFighterTask::FightType promote, float power);
	IFighterTask* EnqueueDefend(IFighterTask::FightType check, IFighterTask::FightType promote, float power);
	IFighterTask* EnqueueGuard(CCircuitUnit* vip);
	CRetreatTask* EnqueueRetreat();
private:
	void DequeueTask(IUnitTask* task, bool done = false);

public:
	virtual IUnitTask* MakeTask(CCircuitUnit* unit) override;
	virtual void AbortTask(IUnitTask* task) override;
	virtual void DoneTask(IUnitTask* task) override;
	virtual void FallbackTask(CCircuitUnit* unit) override;

	void MakeDefence(const springai::AIFloat3& pos);
	void MakeDefence(int cluster);
	void MakeDefence(int cluster, const springai::AIFloat3& pos);
	void AbortDefence(const CBDefenceTask* task);
	bool HasDefence(int cluster);
	springai::AIFloat3 GetScoutPosition(CCircuitUnit* unit);
	springai::AIFloat3 GetRaidPosition(CCircuitUnit* unit);
	void FillFrontPos(CCircuitUnit* unit, F3Vec& outPositions);
	void FillAttackSafePos(CCircuitUnit* unit, F3Vec& outPositions);
	void FillStaticSafePos(CCircuitUnit* unit, F3Vec& outPositions);
	void FillSafePos(CCircuitUnit* unit, F3Vec& outPositions);

	IFighterTask* AddGuardTask(CCircuitUnit* unit);
	bool DelGuardTask(CCircuitUnit* unit);
	IFighterTask* GetGuardTask(CCircuitUnit* unit) const;

	struct SGuardCfg {
		CCircuitDef::RoleM roleMask;  // main roles that may guard a builder (behaviour.json guard.roles)
		unsigned int maxWorkers;  // guard slot for an unarmed builder while worker count < this, 0 = off
		float safeAlly;  // builder is safe (guard leaves) if allied influence minus guards >= this, <= 0 = never safe
		float safeEnemy;  // ... and enemy influence <= this
		int safeFrames;  // builder must stay safe this long before its guard leaves
		float pullRange;  // empty active slot pulls the nearest eligible unit within this range, <= 0 = no pull
		float threatRatio;  // ... only when threat at the guard > this * guard's own threat (0 = any threat)
		float maxShare;  // max share of a role's units (by acting main role) on guard duty at once, rounded up; >= 1 = no limit
		int threatCooldown;
		bool isDebug;  // log guard slot events and a 10 s status line per slot to infolog  // guard in enemy threat is cancelled and the slot closes for this long, < 0 = never cancel
	};
	const SGuardCfg& GetGuardCfg() const { return guard; }
	bool IsGuardRole(CCircuitDef* cdef) const;
	bool IsGuardCapReached(CCircuitDef* cdef) const;

	const std::set<CCircuitUnit*>& GetRoleUnits(CCircuitDef::RoleT type) const {
		return roleInfos[type].units;
	}
	void AddResponse(CCircuitUnit* unit);
	void DelResponse(CCircuitUnit* unit);
	float GetArmyCost() const { return armyCost; }
	const std::set<CCircuitUnit*>& GetArmy() const { return army; }
	float RoleProbability(const CCircuitDef* cdef) const;
	bool IsNeedBigGun(const CCircuitDef* cdef) const;
	springai::AIFloat3 GetBigGunPos(CCircuitDef* bigDef) const;
	void DiceBigGun();
	float ClampMobileCostRatio() const;
	void UpdateDefenceTasks();
	void UpdateDefence();
	void MakeBaseDefence(const springai::AIFloat3& pos);

	const std::vector<CCircuitDef*>& GetLandDefenders() const { return landDefenders; }
	const std::vector<CCircuitDef*>& GetWaterDefenders() const { return waterDefenders; }
	CCircuitDef* GetBigGunDef() const { return bigGunDef; }
	CCircuitDef* GetDefaultPorc() const { return defaultPorc; }

	void SetBaseDefRange(float range) { defence->SetBaseRange(range); }
	float GetBaseDefRange() const { return defence->GetBaseRange(); }
	float GetCommDefRadBegin() const { return defence->GetCommRadBegin(); }
	float GetCommDefRad(float baseDist) const { return defence->GetCommRad(baseDist); }

	void MarkPointOfInterest(CEnemyInfo* enemy);
	void UnmarkPointOfInterest(CEnemyInfo* enemy);

private:
	IUnitTask* DefaultMakeTask(CCircuitUnit* unit);
	IFighterTask* FindGuardTask(CCircuitUnit* unit) const;

	void Watchdog();
	void UpdateIdle();
	void UpdateFight();

	void AddArmyCost(CCircuitUnit* unit);
	void DelArmyCost(CCircuitUnit* unit);

	Handlers2 createdHandler;
	Handlers1 finishedHandler;
	Handlers1 idleHandler;
	EHandlers damagedHandler;
	EHandlers destroyedHandler;

	std::vector<std::set<IFighterTask*>> fightTasks;
	std::vector<IUnitTask*> fightUpdates;  // owner
	unsigned int fightIterator;

	CDefenceMatrix* defence;
	unsigned int defenceIdx;

	std::vector<unsigned int> scoutPath;  // list of spot ids
	unsigned int scoutIdx;

	struct SRaidPoint {
		unsigned int idx;
		int lastFrame;
		float weight;
		std::set<CEnemyInfo*> units;
	};
	std::vector<SRaidPoint> raidPath;  // list of cluster ids

	struct SRoleInfo {
		float cost;
		float maxPerc;
		float factor;
		std::set<CCircuitUnit*> units;
		struct SVsInfo {
			SVsInfo(CCircuitDef::RoleT t, float r, float i) : role(t), ratio(r), importance(i) {}
			CCircuitDef::RoleT role;
			float ratio;
			float importance;
		};
		std::vector<SVsInfo> vs;
	};
	std::vector<SRoleInfo> roleInfos;

	std::set<CCircuitUnit*> army;
	float armyCost;

	std::map<CCircuitUnit*, IFighterTask*> guardTasks;
	SGuardCfg guard;

	struct SRaidQuota {
		float min;
		float avg;
	} raid;
	unsigned int maxScouts;
	float minAttackers;
	struct SThreatQuota {
		float min;
		float len;
	} attackMod, defenceMod;

	std::vector<CCircuitDef*> defenderDefs;
	std::vector<CCircuitDef*> landDefenders;
	std::vector<CCircuitDef*> waterDefenders;
	using BuildVector = std::vector<std::pair<CCircuitDef*, int>>;
	BuildVector baseDefence;
	unsigned int preventCount;
	float amountFactor;
	CCircuitDef* radarDef;
	CCircuitDef* sonarDef;
	CCircuitDef* bigGunDef;
	CCircuitDef* defaultPorc;

	struct SSuperInfo {
		CCircuitDef* cdef;
		float weight;
	};
	std::vector<SSuperInfo> superInfos;

	std::shared_ptr<CGameTask> defend;
	std::vector<std::pair<springai::AIFloat3, BuildVector>> buildDefence;
};

} // namespace circuit

#endif // SRC_CIRCUIT_MODULE_MILITARYMANAGER_H_

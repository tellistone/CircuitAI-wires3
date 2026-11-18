/*
 * AntiHeavyTask.h
 *
 *  Created on: Jun 30, 2016
 *      Author: rlcevg
 */

#ifndef SRC_CIRCUIT_TASK_FIGHTER_ANTIHEAVYTASK_H_
#define SRC_CIRCUIT_TASK_FIGHTER_ANTIHEAVYTASK_H_

#include "task/fighter/SquadTask.h"
#include "module/MilitaryManager.h"

namespace circuit {

/*
 * Anti-heavy squads: at most antiheavy.max_group units of one type (2 by default), never merged, standing
 * antiheavy.spacing apart. They attack mobile heavies (and commanders, heavy buildings) they can shoot from a spot clear of
 * escorts (FindTarget, firing spots: IFighterTask::Attack). With no such heavy, types whose weapon outranges their LOS
 * (Lance, Merlin) park under cover (FallbackPark): at our towers or behind an attack group, near where heavies were seen.
 * The flee and every spot check share one danger model: per danger, the flee starts at its range + GetDangerStart, ends
 * a quarter of the flee margin further out, and spots are picked only outside range + GetPickMargin; cover (the cover
 * map, CAllyCover) counts only against dangers it reaches
 */
class CAntiHeavyTask: public ISquadTask {
public:
	CAntiHeavyTask(ITaskManager* mgr, float powerMod);
	virtual ~CAntiHeavyTask();

	virtual bool CanAssignTo(CCircuitUnit* unit) const override;
	virtual void AssignTo(CCircuitUnit* unit) override;
	virtual void RemoveAssignee(CCircuitUnit* unit) override;
	virtual void Stop(bool done) override;  // gives units their own move and fire state back

	virtual void Start(CCircuitUnit* unit) override;
	virtual void Update() override;

	virtual void OnUnitIdle(CCircuitUnit* unit) override;
	virtual void OnUnitDamaged(CCircuitUnit* unit, CEnemyInfo* attacker) override;

	// A heavy or commander a unit of type cdef can shoot (role, target category, anti-static, air)
	static bool IsQuarryFor(const CCircuitDef* cdef, CEnemyInfo* enemy);
	// For the ANTIHEAVY log: fleeing, engaged, parked (or walking to its park spot), else hunting
	CMilitaryManager::AhNote GetActivity() const;
	const springai::AIFloat3& GetParkPos() const { return park.pos; }  // invalid: none

protected:
	// Cloaked units route like the rest (threat map, path_threat cap) unless their type has the cloak_route attribute
	virtual bool IsCloakRoute(CCircuitUnit* unit) const override;
	virtual float GetPathThreatMod() const override;  // antiheavy.path_threat; < 0 = no cap (old behaviour, and aircraft)
	// Heavies are ignored, except one that out-ranges a long-reload weapon: a danger unless that weapon is ready and the
	// heavy in its reach (we shoot first)
	virtual bool IsFleeIgnored(CEnemyInfo* enemy) const override;
	virtual float GetDangerStart(const CEnemyInfo* enemy) const override;  // max(half the flee margin, its speed x 1 s)
	// Cover map on: a danger the cover doesn't reach (no cover where it stands) weighs more than any cover
	virtual float GetFleeThreat(CEnemyInfo* enemy) const override;
	// Cover map on: the cover there (towers, attack groups, commander), net of enemyThreat, reaches allyThreat
	virtual bool IsFleeCovered(const springai::AIFloat3& pos, float enemyThreat, float allyThreat, float allyRadius,
			CCircuitDef::RoleT ownRole) const override;
	// antiheavy_max_group of the squad's unit type (else antiheavy.max_group): new units join only while there is room
	virtual bool IsRoomFor(size_t count) const override;
	// Shoot and scoot (ISquadTask). reload_scoot units use their own threshold and margin (default: the flee
	// margin); others scoot while their long-reload weapon (5 s or more) isn't ready, when the flee is on.
	// Never mines or aircraft
	virtual float GetScootMargin(CCircuitUnit* unit) const override;
	virtual bool IsUnitReloading(CCircuitUnit* unit, int frame) const override;
	virtual void OnScootStart(CCircuitUnit* unit) override;
	// A unit near its park spot steps back towards it, so it stays in cover
	virtual springai::AIFloat3 GetScootAnchor(CCircuitUnit* unit, const springai::AIFloat3& uPos) const override;
	virtual int GetMoveState(CCircuitUnit* unit) const override;  // hold position while parked at towers

	// Firing spots (IFighterTask::Attack): outside the pick zones, with lines of fire as attack groups have. A sniper's
	// must also be no hotter than a route may be; a hunter's needn't (it closes in, where the heavy's own threat is)
	virtual float GetEscapeSafeThreat() const override;
	virtual float GetAllowedThreatAt(const springai::AIFloat3& pos, float strength) const override;
	virtual bool GetSightLifts(const CEnemyInfo* enemy, float& outAimLift, float& outTargetLift) const override;
	virtual bool IsFireSpotAllowed(const springai::AIFloat3& p) const override;

private:
	const CMilitaryManager::SAntiHeavyCfg& GetCfg() const;  // antiheavy block of behaviour.json
	// The leader type's antiheavy_flee_margin, else the block's; -1 (off) for mines and aircraft
	float GetFleeMargin() const;
	float GetPickMargin(const CEnemyInfo* enemy) const;  // max(flee margin, GetDangerStart + half the flee margin)
	// The leader's long-reload weapon (5 s or more) isn't ready; never for mines
	bool IsReloading(int frame) const;
	static bool IsLongReload(const CCircuitDef* cdef);  // 5 s or more
	bool IsQuarry(CEnemyInfo* enemy) const;  // IsQuarryFor the leader's type
	bool IsCoverOn() const;
	float GetCoverAt(const springai::AIFloat3& pos) const;  // CAllyCover's total there
	// Ready damage of the other anti-heavy squads that target it (focus fire)
	float GetOthersReadyDamage(CEnemyInfo* enemy) const;

	void AttackTarget(int frame);
	// Mines and aircraft: a plain attack order (mines: only as many as it takes)
	bool AttackDirect(CCircuitUnit* unit, int frame, float& minePower);

	// Stand-off distance from a heavy: just outside its own threat ring on our threat layer, but never less than
	// engageRange. Routes end there; within it the squad attacks
	float GetStandOff(CEnemyInfo* enemy, float engageRange) const;
	// The point on the line from ePos towards pos, dist from ePos (pos itself if it's nearer)
	static springai::AIFloat3 GetPointTowards(const springai::AIFloat3& ePos, const springai::AIFloat3& pos, float dist);
	// Pick zones of the enemies the flee doesn't ignore (range + GetPickMargin), for IsSpotSafe; every update
	void CollectDangers();
	// No pick zone reaches spot, or the cover there covers the enemies that reach it (IsFleeCovered)
	bool IsSpotSafe(const springai::AIFloat3& spot) const;
	// Stand-off point for a heavy at ePos: the first walkable point from standOff outwards (one threat cell at a
	// time, a few cells at most) towards pos whose threat is under the route cap; false if none
	bool ProbeStandOff(const springai::AIFloat3& ePos, const springai::AIFloat3& pos, float standOff,
			springai::AIFloat3& outPos) const;

	bool FindTarget();
	void ApplyTargetPath(const CQueryPathMulti* query);
	void FallbackTargetEmpty();
	void FallbackAttackSafe();
	void ApplyAttackSafe(const CQueryPathMulti* query);
	void FallbackStaticSafe();
	void ApplyStaticSafe(const CQueryPathMulti* query);
	void FallbackBasePos(bool isCapped);
	void ApplyBasePos(const CQueryPathSingle* query, bool isCapped);
	void FallbackCommPos();

	/*
	 * Parking (types whose weapon range is over their LOS, with the cover map on)
	 */
	struct SParkSpot {
		springai::AIFloat3 pos;
		const IFighterTask* group;  // the attack group it follows (only compared); nullptr: a tower group
		float cover;
		float score;
	};
	bool IsSniper() const;  // weapon range over LOS (Lance, Merlin): not mines or aircraft
	bool IsParker() const;  // a sniper, with the cover map on
	// Candidates: tower groups (our and allied built armed statics by cell), on our side of them a third of their range
	// back, and spots behind our attack groups. Walkable, under the route cap, safe (IsSpotSafe), covered, and not
	// claimed by another squad. Score: cover x heavy pressure (sightings weighted down over keep_sec, full within our
	// weapon range), halved per minute of walking, and halved near a recent retreat. Best first
	void FillParkSpots(std::vector<SParkSpot>& outSpots);
	void AddParkSpot(const springai::AIFloat3& pos, const IFighterTask* group, std::vector<SParkSpot>& outSpots);
	void SetPark(const SParkSpot& spot);  // claims it (GetParkPos) from now
	void ClearPark() { park = {-RgtVector, nullptr, 0.f, 0.f}; }
	void UpdatePark();  // keep the spot, or switch (see .cpp)
	void FallbackPark();
	void ApplyParkPath(const CQueryPathSingle* query);
	void SpreadOut(CCircuitUnit* unit, int frame);  // parked: the second unit stands spacing to the side of the leader
	// Hold position while parked at towers; hold fire (parkers) while a mobile heavy is within 1.25x weapon range
	void UpdateHolds(int frame);
	void ApplyHolds(CCircuitUnit* unit);

	bool CheckFlee();  // antiheavy.flee_margin (UpdateFlee); true while fleeing
	virtual void FleeFrom(const springai::AIFloat3& dangerPos) override;  // to the nearest safe park spot away from it
	virtual void OnFleeEnd() override;  // stop walking the flee path (unless it leads to our park spot)
	void ApplyFleePath(const CQueryPathSingle* query);

	struct SDanger {
		springai::AIFloat3 pos;
		float sqReach;  // (weapon range + GetPickMargin)^2
		float threat;  // GetFleeThreat
	};
	std::vector<SDanger> dangers;  // CollectDangers
	static std::vector<SParkSpot> parkSpots;  // FillParkSpots scratch

	SParkSpot park = {-RgtVector, nullptr, 0.f, 0.f};  // pos invalid: none; cover: when claimed
	int parkFrame = 0;  // when it was claimed
	int parkCheckFrame = 0;  // UpdatePark from then
	int routeFrame = 0;  // last walk-in route
	bool isParking = false;  // nothing to attack or walk to: the squad parks
	int walkInFrame = 0;  // no new walk-in to a heavy before this (after a flee)
	int fireHoldFrame = -FRAMES_PER_SEC * 60;  // a mobile heavy was last within 1.25x weapon range
	bool isParkHold = false;  // units on hold position (UpdateHolds)
	bool isFireHold = false;  // units on hold fire
	bool wasFleeing = false;
};

} // namespace circuit

#endif // SRC_CIRCUIT_TASK_FIGHTER_ANTIHEAVYTASK_H_

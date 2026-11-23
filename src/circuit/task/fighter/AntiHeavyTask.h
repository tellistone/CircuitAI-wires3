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
#include "unit/CoreUnit.h"

#include <map>

namespace circuit {

/*
 * Anti-heavy squads: at most antiheavy.max_group units of one type (2 by default), never merged, standing
 * antiheavy.spacing apart. They attack mobile heavies (and commanders, heavy buildings) they can shoot from a spot clear of
 * escorts (FindTarget, firing spots: IFighterTask::Attack). Types whose weapon outranges their LOS (Lance, Merlin) walk
 * in only to heavies within GetHuntReach; with none, they park under cover (FallbackPark): behind an attack group
 * (at our towers only when no group offers a spot), near where heavies were seen. The flee and every spot check share one danger model: per danger, the
 * flee starts at its range + GetDangerStart, ends a quarter of the flee margin further out, and spots are picked only
 * outside range + GetPickMargin; cover (the cover map, CAllyCover) counts only against dangers it reaches
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

	// For the ANTIHEAVY log: fleeing, engaged, parked (or walking to its park spot), else hunting
	CMilitaryManager::AhNote GetActivity() const;
	// Engaged, and a unit of the squad has its target within its weapon range; never mines or aircraft
	bool IsAttacking() const;
	const springai::AIFloat3& GetParkPos() const { return park.pos; }  // invalid: none
	// Weapon range + 30 s of walking: snipers walk in to heavies this near, and the army pull (UpdateAhPull) takes
	// units for them
	static float GetHuntReach(const CCircuitDef* cdef);

protected:
	// Cloaked units route like the rest (threat map, path_threat cap) unless their type has the cloak_route attribute
	virtual bool IsCloakRoute(CCircuitUnit* unit) const override;
	virtual float GetPathThreatMod() const override;  // antiheavy.path_threat; < 0 = no cap (old behaviour, and aircraft)
	// Heavies are ignored, except one that out-ranges a long-reload weapon: a danger unless that weapon is ready and the
	// heavy in its reach (we shoot first)
	virtual bool IsFleeIgnored(CEnemyInfo* enemy) const override;
	virtual float GetDangerStart(const CEnemyInfo* enemy) const override;  // max(half the flee margin, its speed x 1 s)
	// Cover map on (not on water): a danger the cover doesn't reach (no cover where it stands) weighs more than any cover
	virtual float GetFleeThreat(CEnemyInfo* enemy) const override;
	// Cover map on (not on water, which it doesn't cover): the cover there (towers, attack groups, commander), net of
	// enemyThreat, reaches allyThreat. Else our units around pos, as other squads
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
	virtual int GetMoveState(CCircuitUnit* unit) const override;  // hold position while parked

	// Firing spots (IFighterTask::Attack): safe (IsSpotSafe, the target's own threat aside where it reaches, against the
	// flee end zones: the squad is committed to the target), with lines of fire as attack groups have
	virtual bool GetSightLifts(const CEnemyInfo* enemy, float& outAimLift, float& outTargetLift) const override;
	virtual bool IsFireSpotAllowed(const springai::AIFloat3& p) const override;

private:
	const CMilitaryManager::SAntiHeavyCfg& GetCfg() const;  // antiheavy block of behaviour.json
	// The leader type's antiheavy_flee_margin, else the block's; -1 (off) for mines and aircraft
	float GetFleeMargin() const;
	float GetPickMargin(const CEnemyInfo* enemy) const;  // GetDangerStart + half the flee margin
	// The leader's long-reload weapon (5 s or more) isn't ready; never for mines
	bool IsReloading(int frame) const;
	bool IsQuarry(CEnemyInfo* enemy) const;  // CCircuitDef::IsQuarry for the leader's type
	bool IsCoverOn() const;
	bool IsCoverMapAt(const springai::AIFloat3& pos) const;  // cover map on, and pos not on water (no cover there)
	float GetCoverAt(const springai::AIFloat3& pos) const;  // CAllyCover's total there
	// Ready damage of the other anti-heavy squads that target it, from units within their weapon range (focus fire)

	void AttackTarget(int frame);
	// Mines and aircraft: a plain attack order (mines: only as many as it takes)
	void AttackDirect(CCircuitUnit* unit, int frame, float& minePower);
	// Stand the unit down (travel action, Stop) and clear its target, so IFighterTask::Attack orders it again
	void StopUnit(CCircuitUnit* unit);
	// No target now (dropped), or one out of LOS a moment: units still ordered on a target are stopped once, else the
	// engine goes on shooting it (at a radar blip too) or chasing it. Not busy or scooting units
	void StopAttacks();
	// Targets the whole squad held off from (IFighterTask::IsAllHolding): FindTarget skips each until 'until'
	struct SHeld {
		int until = 0;
		int frames = 0;  // the last skip's length (doubles when it happens again soon)
	};
	std::map<ICoreUnit::Id, SHeld> helds;  // forgotten a minute after the skip ends
	bool IsHeld(CEnemyInfo* enemy, int frame) const;
	int targetSeenFrame = 0;  // the target was last in LOS (a long-reload squad keeps it a moment out of LOS)

	// Stand-off distance from a heavy: just outside its own threat ring on our threat layer, but never less than
	// engageRange. Routes end there; within it the squad attacks
	float GetStandOff(CEnemyInfo* enemy, float engageRange) const;
	// The point on the line from ePos towards pos, dist from ePos (pos itself if it's nearer)
	static springai::AIFloat3 GetPointTowards(const springai::AIFloat3& ePos, const springai::AIFloat3& pos, float dist);
	// The enemies the flee doesn't ignore, for IsSpotSafe; every update
	void CollectDangers();
	// The one test for stand-off, firing and park spots: no hotter, excl's threat aside where its ring reaches, than
	// a route there may be (GetPathThreatAllowance: not loosened by hot ground where the squad stands), and no pick zone
	// reaches it, or the cover there covers the enemies that reach it (IsFleeCovered). isKept: a spot for the current
	// target or the heavy the squad already walks to, judged against the flee's end zones
	bool IsSpotSafe(const springai::AIFloat3& spot, CEnemyInfo* excl, bool isKept = false) const;
	// Stand-off point for a heavy at ePos: the first walkable point from standOff outwards (one threat cell at a
	// time, a few cells at most) towards pos whose threat is under the route cap; false if none
	bool ProbeStandOff(const springai::AIFloat3& ePos, const springai::AIFloat3& pos, float standOff,
			springai::AIFloat3& outPos) const;

	bool FindTarget();  // skips held-off targets for a while
	bool TakePath(const std::shared_ptr<PathInfo>& path);  // walk it, if it isn't empty
	void ApplyTargetPath(const CQueryPathMulti* query);
	void FallbackTargetEmpty();
	// Nothing to attack: look at the park spot (or the way to the army) again? Every CHECK_FRAMES, or with no route
	bool IsCheckDue();
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
		int slot;  // behind a group: which of its PARK_SLOTS
		float cover;
		float score;
	};
	bool IsSniper() const;  // weapon range over LOS (Lance, Merlin): not mines or aircraft
	bool IsParker() const;  // a sniper, with the cover map on
	// Candidates: behind our attack groups (follow_dist back, and side slots), else (no group spot) tower groups (our and
	// allied built armed statics by cell), on our side of them a third of their range back; never within 256 of our
	// base. Walkable, safe (IsSpotSafe), covered, and not claimed by another squad. Score: cover (at most 3x
	// flee_ally_threat) x heavy pressure (sightings weighted down over keep_sec, full within our weapon range),
	// / (1 + minutes of walking), and halved near a recent retreat. Best first
	void FillParkSpots(std::vector<SParkSpot>& outSpots);
	void AddParkSpot(const springai::AIFloat3& pos, const IFighterTask* group, int slot, std::vector<SParkSpot>& outSpots);
	// Another squad's park spot this near blocks a spot; side slots are this far apart; a squad this far from its spot
	// walks back. max(256, 2x spacing)
	float GetClaimRadius() const;
	void SetPark(const SParkSpot& spot);  // claims it (GetParkPos) from now
	void ClearPark() { park = {-RgtVector, nullptr, 0, 0.f, 0.f}; }
	void UpdatePark();  // keep the spot, or switch (see .cpp)
	void FallbackPark();
	void ApplyParkPath(const CQueryPathSingle* query);
	void SpreadOut(CCircuitUnit* unit, int frame);  // parked: the others stand to alternate sides of the leader
	// Hold position while parked; return fire only while attacking a heavy (isAttack), the unit's own fire state otherwise
	void UpdateHolds(bool isAttack);
	void ApplyHolds(CCircuitUnit* unit);

	bool CheckFlee();  // antiheavy.flee_margin (UpdateFlee); true while fleeing
	virtual void FleeFrom(const springai::AIFloat3& dangerPos) override;  // towards the army; drops the park spot
	virtual void OnFleeEnd() override;  // stop where the squad is, out of reach; counts the flee
	void ApplyFleePath(const CQueryPathSingle* query);

	struct SDanger {
		springai::AIFloat3 pos;
		float sqPick;  // pick zone: (weapon range + GetPickMargin)^2
		float sqKeep;  // flee end zone: (weapon range + GetDangerStart + a quarter of the flee margin)^2
		float threat;  // GetFleeThreat
	};
	std::vector<SDanger> dangers;  // CollectDangers
	static std::vector<SParkSpot> parkSpots;  // FillParkSpots scratch

	SParkSpot park = {-RgtVector, nullptr, 0, 0.f, 0.f};  // pos invalid: none; cover: when claimed
	int parkFrame = 0;  // when it was claimed
	int checkFrame = 0;  // IsCheckDue from then
	int routeFrame = 0;  // last walk-in route
	bool isParking = false;  // nothing to attack or walk to: the squad parks
	int walkInFrame = 0;  // no new walk-in to a heavy before this (after a flee, or no route to any heavy)
	bool isParkHold = false;  // units on hold position (UpdateHolds)
	bool isReturnFire = false;  // units on return fire (attacking a heavy)
};

} // namespace circuit

#endif // SRC_CIRCUIT_TASK_FIGHTER_ANTIHEAVYTASK_H_

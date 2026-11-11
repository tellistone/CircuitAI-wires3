/*
 * ThreatMap.h
 *
 *  Created on: Aug 25, 2015
 *      Author: rlcevg
 *      Original implementation: https://github.com/spring/KAIK/blob/master/ThreatMap.h
 */

#ifndef SRC_CIRCUIT_TERRAIN_THREATMAP_H_
#define SRC_CIRCUIT_TERRAIN_THREATMAP_H_

#include "unit/enemy/EnemyUnit.h"

#include <map>
#include <vector>

namespace circuit {

#define THREAT_UPDATE_RATE	(FRAMES_PER_SEC / 4)
#define THREAT_BASE			0.f

class CMapManager;
class CCircuitUnit;

class CThreatMap {
public:
	CThreatMap(CMapManager* manager, float decloakRadius);
	virtual ~CThreatMap();

	void EnqueueUpdate();
	bool IsUpdating() const { return isUpdating; }
	void DropUpdate() { isUpdating = false; }  // the update in flight was dropped with its scheduler

	void SetEnemyUnitRange(CEnemyUnit* e) const;
	void SetEnemyUnitThreat(CEnemyUnit* e) const { e->SetThreat(GetEnemyUnitThreat(e)); }
	void NewEnemy(CEnemyUnit* e) const;

	float GetAllThreatAt(const springai::AIFloat3& position) const;
	/*
	 * Two views of the same enemies. The plain view, and the raid view for units whose task IsRaidView (raid and
	 * scout tasks): on the surface and amphibious layers each enemy's threat is x its type's raid_thr_mod
	 * (CCircuitDef::GetRaidThrMod), its unit count weight is CCircuitDef::GetRaidCountWeight; air is the same in both.
	 * GetThreatAt(unit), GetSquadThreatAt, GetSquadThreatExcl and routes (CPathFinder) read the unit's task's view.
	 * SetThreatType and GetThreatAt(position) always read the plain view: other modules read that shared layer
	 */
	static bool IsRaidView(const CCircuitUnit* unit);
	void SetThreatType(CCircuitUnit* unit);
	float GetThreatAt(const springai::AIFloat3& position) const;
	float GetThreatAt(CCircuitUnit* unit, const springai::AIFloat3& position) const;
	// The plain view whatever the unit's task (CMilitaryManager::GetTakeableContested's builder wall)
	float GetPlainThreatAt(CCircuitUnit* unit, const springai::AIFloat3& position) const;
	/*
	 * Squad threat, for squads (IFighterTask::IsSquadThreat) and their routes (Lanchester's square law): the
	 * threat x (1 + quota.group_count_power x the number of enemies that can fire at the cell, each weighted by
	 * SEnemyData::GetCountWeight). Squads weigh their own power the same way (IFighterTask::GetCountPowerMod),
	 * so the bonus cancels out at equal numbers. With group_count_power 0 it is GetThreatAt
	 */
	float GetSquadThreatAt(CCircuitUnit* unit, const springai::AIFloat3& position) const;
	// Squad threat at position without one enemy's threat there. The enemy stays in the count, which overstates
	// by at most group_count_power
	float GetSquadThreatExcl(CCircuitUnit* unit, const springai::AIFloat3& position, float exclThreat) const;
	static constexpr float MAX_COUNT_POWER = 0.1f;  // quota.group_count_power is clamped to 0..this
	float GetCountPower() const { return countPower; }  // quota.group_count_power, for both sides
	// The group size bonus for count units: 1 + quota.group_count_power x count. Ours (IFighterTask::GetCountPowerMod)
	// and the enemy's (squad threat, enemy blobs, defences) alike
	float GetCountMod(float count) const { return 1.f + countPower * count; }

	float* GetAirThreatArray() { return airThreat; }
	float* GetSurfThreatArray() { return surfThreat; }
	float* GetAmphThreatArray() { return amphThreat; }
	float* GetCloakThreatArray() { return cloakThreat; }
	// Route layers (CPathFinder::FillMapData): isRaidView: the raid view (air has none); isSquad: squad threat
	float* GetAirLayer(bool isSquad) const { return isSquad ? airSquad : airThreat; }
	float* GetSurfLayer(bool isRaidView, bool isSquad) const {
		return isRaidView ? (isSquad ? raidSurfSquad : raidSurf) : (isSquad ? surfSquad : surfThreat);
	}
	float* GetAmphLayer(bool isRaidView, bool isSquad) const {
		return isRaidView ? (isSquad ? raidAmphSquad : raidAmph) : (isSquad ? amphSquad : amphThreat);
	}
	int GetThreatMapWidth() const { return width; }
	int GetThreatMapHeight() const { return height; }

	float GetUnitThreat(CCircuitUnit* unit) const;
	int GetSquareSize() const { return squareSize; }
	int GetMapSize() const { return mapSize; }

private:
	/*
	 * http://stackoverflow.com/questions/872544/precision-of-floating-point
	 * Single precision: for accuracy of +/-0.5 (or 2^-1) the maximum size that the number can be is 2^23.
	 */
	struct SThreatData {
		FloatVec airThreat;  // air layer
		FloatVec surfThreat;  // surface (water and land)
		FloatVec amphThreat;  // under water and surface on land
		FloatVec cloakThreat;  // decloakers
		FloatVec shield;  // shield power per armed enemy in the cell: each shield's share (AddShields)
		FloatVec shieldAmmo;  // linked shield charge per shield-fed enemy (Felon) in the cell: each one's share
		// Squad layers of air, surf and amph (GetSquadThreatAt); while drawing, the enemy unit count.
		// Empty when group_count_power is 0
		FloatVec airSquad;
		FloatVec surfSquad;
		FloatVec amphSquad;
		// Raid view of surf and amph (IsRaidView); while drawing, the difference the enemies with a raid multiplier
		// make (FinishLayers). Their squad layers: while drawing, the difference in the unit count; empty when
		// group_count_power is 0
		FloatVec raidSurf;
		FloatVec raidAmph;
		FloatVec raidSurfSquad;
		FloatVec raidAmphSquad;
	};
	// Where an enemy's ground and water threat is drawn: one view's surf and amph layers, and their squad layers
	// (the unit count; nullptr unless isCounting)
	struct SDrawLayers {
		float* surf;
		float* amph;
		float* surfSquad;
		float* amphSquad;
	};

	CMapManager* manager;
	SAreaData* areaData;

	inline void PosToXZ(const springai::AIFloat3& pos, int& x, int& z) const;
	inline int PosToIndex(const springai::AIFloat3& pos) const;  // the cell's index in a layer
	inline springai::AIFloat3 XZToPos(int x, int z) const;

	void AddEnemyUnit(const SEnemyData& e);
	void AddEnemyUnitAll(const SEnemyData& e);
	// count: what the enemy adds to the unit count of the squad layers where it adds threat
	// (SEnemyData::GetCountWeight; 0 unless isCounting)
	void AddEnemyAir(const SEnemyData& e, const int slack = 0, const float count = 0.f);  // Enemy AntiAir
	// Enemy AntiAmph, threat and count (may be < 0: a raid view difference) drawn into layers
	void AddEnemyAmphConst(const SEnemyData& e, const int slack, const float threat, const float count,
			const SDrawLayers& layers);
	void AddEnemyAmphGradient(const SEnemyData& e, const int slack, const float threat, const float count,
			const SDrawLayers& layers);
	void AddDecloaker(const SEnemyData& e);
	// Shields of hostile and peace enemies (Aspis is unarmed), each charge shared by the armed enemies inside it
	void AddShields(const std::vector<SEnemyData>& hostiles, const std::vector<SEnemyData>& peaces);
	// fedCells: cells of the shield-fed enemies, which may fire from the charge of linked shields (AddShieldAmmo)
	void AddShield(const SEnemyData& e, const std::vector<int>& armedCells, const std::vector<int>& fedCells);
	void AddShieldAmmo(const SEnemyData& e, const std::vector<int>& fedCells);
	// Cells whose centre is within radius of e, and its own cell: f(index) for each on the map. Count: of cells, the
	// ones inside, by the same test, so that a charge split by the count adds up to the charge once painted
	inline bool IsCellInRadius(const SEnemyData& e, float sqRadius, int x, int z) const;
	template<typename F> void ForCellsInRadius(const SEnemyData& e, float radius, F&& f) const;
	int CountCellsInRadius(const SEnemyData& e, float radius, const std::vector<int>& cells) const;

	int GetCloakRange(const CCircuitDef* edef) const;
	int GetShieldRange(const CCircuitDef* edef) const;
	float GetEnemyUnitThreat(const CEnemyUnit* e) const;
	float GetOwnShieldMod(const CEnemyUnit* e, int index) const;  // threat x this by its shield charge (index: its cell)
	// The unit's threat layer: air for aircraft, amph for amphibious units, else surf; isSquad: its squad layer;
	// isRaidView: of the raid view (no raid air layer). The 2-argument form: the view of the unit's task
	float* GetLayer(CCircuitUnit* unit, bool isSquad, bool isRaidView) const;
	float* GetLayer(CCircuitUnit* unit, bool isSquad) const { return GetLayer(unit, isSquad, IsRaidView(unit)); }
	// After drawing: squad layers (enemy unit count) -> squad threat; raid layers (differences) -> raid view
	void FinishLayers(SThreatData& threatData);

	void Prepare(SThreatData& threatData);
	void SetReadPointers(SThreatData& threatData);  // the layers GetThreatAt etc read: threatData's
	void Update();
	void Apply();
	void SwapBuffers();
	SThreatData* GetNextThreatData() {
		return (pThreatData.load() == &threatData0) ? &threatData1 : &threatData0;
	}

	int squareSize;
	int width;
	int height;
	int mapSize;

	float countPower;  // quota.group_count_power
	float emptyShieldMod;  // quota.shield_empty_threat: threat x this for an own-shield enemy at empty shield
	float ammoTime;  // quota.shield_ammo_time: seconds of fire a shield-fed weapon is judged over
	float ammoReach;  // largest shield radius of the shield-fed types: a shield links to theirs this much farther out
	bool isCounting;  // countPower > 0: squad layers of their own, else they are the threat layers
	float maxCount;  // most enemies (weighted) that could fire at one cell, last update
	int countLogFrame;
	// For the THREAT log (tuning raid_thr_mod): hostile enemies and those drawn on the raid view again (a raid
	// multiplier) at the last update; Update's run time since the last log
	int hostileCount = 0;
	int raidModCount = 0;
	float updateMsSum = 0.f;
	float updateMsMax = 0.f;
	int updateNum = 0;
	void LogStats();

	int rangeDefault;
	int distCloak;
	struct {
		float allMod;
		float staticMod;
		float speedMod;
		int speedModMax;
	} slackMod;

	SThreatData threatData0, threatData1;  // Double-buffer for threading
	std::atomic<SThreatData*> pThreatData;
	float* drawAirThreat;
	float* drawCloakThreat;
	float* drawShieldArray;
	float* drawShieldAmmoArray;
	float* drawAirSquad;  // nullptr unless isCounting
	SDrawLayers drawPlain;  // surf and amph of the plain view
	SDrawLayers drawRaid;  // ... and of the raid view (differences)
	bool isUpdating;

	float* airThreat;
	float* surfThreat;
	float* amphThreat;
	float* cloakThreat;
	float* shieldArray;
	float* shieldAmmoArray;
	float* airSquad;  // the threat layers themselves unless isCounting
	float* surfSquad;
	float* amphSquad;
	float* raidSurf;
	float* raidAmph;
	float* raidSurfSquad;  // the raid layers themselves unless isCounting
	float* raidAmphSquad;
	float* threatArray;  // current threat array for multiple GetThreatAt() calls

#ifdef DEBUG_VIS
private:
	std::vector<std::pair<uint32_t, float*>> sdlWindows;
	bool isWidgetDrawing = false;
	bool isWidgetPrinting = false;
	void UpdateVis();
public:
	void ToggleSDLVis();
	void ToggleWidgetDraw();
	void ToggleWidgetPrint();
	void SetMaxThreat(float maxThreat);
#endif
};

} // namespace circuit

#endif // SRC_CIRCUIT_TERRAIN_THREATMAP_H_

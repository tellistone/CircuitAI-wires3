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
	void SetThreatType(CCircuitUnit* unit);
	float GetThreatAt(const springai::AIFloat3& position) const;
	float GetThreatAt(CCircuitUnit* unit, const springai::AIFloat3& position) const;
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
	float* GetAirSquadArray() { return airSquad; }
	float* GetSurfSquadArray() { return surfSquad; }
	float* GetAmphSquadArray() { return amphSquad; }
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
		// Squad layers of air, surf and amph (GetSquadThreatAt); while drawing, the enemy unit count.
		// Empty when group_count_power is 0
		FloatVec airSquad;
		FloatVec surfSquad;
		FloatVec amphSquad;
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
	void AddEnemyAmphConst(const SEnemyData& e, const int slack = 0, const float count = 0.f);  // Enemy AntiAmph
	void AddEnemyAmphGradient(const SEnemyData& e, const int slack = 0, const float count = 0.f);  // Enemy AntiAmph
	void AddDecloaker(const SEnemyData& e);
	// Shields of hostile and peace enemies (Aspis is unarmed), each charge shared by the armed enemies inside it
	void AddShields(const std::vector<SEnemyData>& hostiles, const std::vector<SEnemyData>& peaces);
	void AddShield(const SEnemyData& e, const std::vector<int>& armedCells);

	int GetCloakRange(const CCircuitDef* edef) const;
	int GetShieldRange(const CCircuitDef* edef) const;
	float GetEnemyUnitThreat(const CEnemyUnit* e) const;
	// The unit's threat layer: air for aircraft, amph for amphibious units, else surf; isSquad: its squad layer
	float* GetLayer(CCircuitUnit* unit, bool isSquad) const;
	void ApplyCountPower(SThreatData& threatData);  // squad layers: enemy unit count -> squad threat

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
	bool isCounting;  // countPower > 0: squad layers of their own, else they are the threat layers
	float maxCount;  // most enemies (weighted) that could fire at one cell, last update
	int countLogFrame;

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
	float* drawSurfThreat;
	float* drawAmphThreat;
	float* drawCloakThreat;
	float* drawShieldArray;
	float* drawAirSquad;  // nullptr unless isCounting
	float* drawSurfSquad;
	float* drawAmphSquad;
	bool isUpdating;

	float* airThreat;
	float* surfThreat;
	float* amphThreat;
	float* cloakThreat;
	float* shieldArray;
	float* airSquad;  // the threat layers themselves unless isCounting
	float* surfSquad;
	float* amphSquad;
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

/*
 * InfluenceMap.h
 *
 *  Created on: Oct 20, 2019
 *      Author: rlcevg
 */

#ifndef SRC_CIRCUIT_TERRAIN_INFLUENCEMAP_H_
#define SRC_CIRCUIT_TERRAIN_INFLUENCEMAP_H_

#include "unit/enemy/EnemyUnit.h"

#include <unordered_map>
#include <vector>
#ifdef DEBUG_VIS
#include <stdint.h>
#endif

namespace springai {
	class AIFloat3;
	class Feature;
}

namespace circuit {

#define INFL_BASE		0.f
#define INFL_EPS		0.01f

class CMapManager;
class CAllyUnit;

class CInfluenceMap {
public:
	CInfluenceMap(CMapManager* manager);
	virtual ~CInfluenceMap();

private:
	void ReadConfig();

public:
	void EnqueueUpdate();
	bool IsUpdating() const { return isUpdating; }
	void DropUpdate() { isUpdating = false; }  // the update in flight was dropped with its scheduler

	float GetEnemyInflAt(const springai::AIFloat3& position) const;
	float GetAllyInflAt(const springai::AIFloat3& position) const;
	float GetAllyDefendInflAt(const springai::AIFloat3& position) const;
	float GetInfluenceAt(const springai::AIFloat3& position) const;
	// Our territory: our buildings (static defences too) and land constructors outweigh the enemy's known buildings
	// by more than margin there. Every one of them counts the same (AddTerritory); enemy mobile units don't count
	bool IsAllyTerritory(const springai::AIFloat3& position, float margin) const;
	// What one mobile armed ally adds to allyInfl at position, by the same formula as AddMobileArmed
	float GetMobileArmedInflAt(CAllyUnit* u, const springai::AIFloat3& position) const;

	int Pos2Index(const springai::AIFloat3& pos) const;

private:
	struct SInfluenceData {
		FloatVec enemyInfl;
		FloatVec allyInfl;
		FloatVec allyDefendInfl;
		FloatVec allyTerritory;
		FloatVec enemyTerritory;
		FloatVec influence;
//		FloatVec tension;
//		FloatVec vulnerability;
//		FloatVec featureInfl;
	};

	CMapManager* manager;

	int GetUnitRange(CAllyUnit* u) const;

	void AddMobileArmed(CAllyUnit* u);
	void AddStaticArmed(CAllyUnit* u);
	void AddUnarmed(CAllyUnit* u);
	void AddEnemy(const SEnemyData& e);
	void AddTerritory(float* layer, const springai::AIFloat3& pos);  // IsAllyTerritory: 2 over defence.infl_rad
	// Adds val, fading linearly to 0 at range cells, to layer around cell (posx, posz)
	void AddDisc(float* layer, int posx, int posz, int range, float val);
//	void AddFeature(springai::Feature* f);
	inline void PosToXZ(const springai::AIFloat3& pos, int& x, int& z) const;

	void Prepare(SInfluenceData& inflData);
	void Update();
	void Apply();
	void SwapBuffers();
	SInfluenceData* GetNextInflData() {
		return (pInflData.load() == &inflData0) ? &inflData1 : &inflData0;
	}

	int squareSize;
	int width;
	int height;
	int mapSize;

//	float vulnMax;

	SInfluenceData inflData0, inflData1;  // Double-buffer for threading
	std::atomic<SInfluenceData*> pInflData;
	float* drawEnemyInfl;
	float* drawAllyInfl;
	float* drawAllyDefendInfl;
	float* drawAllyTerritory;
	float* drawEnemyTerritory;
	float* drawInfluence;
//	float* drawTension;
//	float* drawVulnerability;
//	float* drawFeatureInfl;
	bool isUpdating;
	// Where each mobile armed ally stood when the current maps were applied (GetMobileArmedInflAt)
	std::unordered_map<int, springai::AIFloat3> mobileArmedPos;

	float* enemyInfl;
	float* allyInfl;
	float* allyDefendInfl;
	float* allyTerritory;
	float* enemyTerritory;
	float* influence;
//	float* tension;
//	float* vulnerability;
//	float* featureInfl;

	float defRadius;

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

#endif // SRC_CIRCUIT_TERRAIN_INFLUENCEMAP_H_

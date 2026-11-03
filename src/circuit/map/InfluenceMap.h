/*
 * InfluenceMap.h
 *
 *  Created on: Oct 20, 2019
 *      Author: rlcevg
 */

#ifndef SRC_CIRCUIT_TERRAIN_INFLUENCEMAP_H_
#define SRC_CIRCUIT_TERRAIN_INFLUENCEMAP_H_

#include "unit/enemy/EnemyUnit.h"

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
	// Number of static allies (armed or not) whose infl_rad covers position
	float GetAllyStructAt(const springai::AIFloat3& position) const;
	// Number of known static enemies (armed or not, seen or remembered) whose front_enemy_rad covers position
	float GetEnemyStructAt(const springai::AIFloat3& position) const;
	// Covered by both ally and enemy structures, with no armed influence from either side
	bool IsFrontLineAt(const springai::AIFloat3& position) const;
	// Metal cluster whose centre or any mex spot lies in enemy structure influence (GetEnemyStructAt > 0)
	bool IsFrontLineCluster(int clusterIndex) const;
	void ToggleFrontLineDraw();
	// What one mobile armed ally adds to allyInfl at position, by the same formula as AddMobileArmed
	float GetMobileArmedInflAt(CAllyUnit* u, const springai::AIFloat3& position) const;

	int Pos2Index(const springai::AIFloat3& pos) const;

private:
	struct SInfluenceData {
		FloatVec enemyInfl;
		FloatVec allyInfl;
		FloatVec allyDefendInfl;
		FloatVec influence;
		FloatVec allyStruct;
		FloatVec enemyStruct;
		FloatVec frontLine;  // 1 = front line, 0 = not
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
	void AddStruct(const springai::AIFloat3& pos, int range, float* layer);
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
	float* drawInfluence;
	float* drawAllyStruct;
	float* drawEnemyStruct;
	float* drawFrontLine;
//	float* drawTension;
//	float* drawVulnerability;
//	float* drawFeatureInfl;
	bool isUpdating;

	float* enemyInfl;
	float* allyInfl;
	float* allyDefendInfl;
	float* influence;
	float* allyStruct;
	float* enemyStruct;
	float* frontLine;
//	float* tension;
//	float* vulnerability;
//	float* featureInfl;

	float defRadius;
	int frontAllyRange;  // cells
	int frontEnemyRange;  // cells
	float frontMaxThreat;
	std::vector<springai::AIFloat3> frontMarks;

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

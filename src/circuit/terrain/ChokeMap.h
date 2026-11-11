/*
 * ChokeMap.h
 *
 *  Created on: Nov 11, 2026
 *      Author: Claude
 */

#ifndef SRC_CIRCUIT_TERRAIN_CHOKEMAP_H_
#define SRC_CIRCUIT_TERRAIN_CHOKEMAP_H_

#include "terrain/CellMask.h"
#include "terrain/RegionGraph.h"
#include "terrain/TrafficField.h"
#include "unit/CoreUnit.h"
#include "util/Defines.h"

#include "AIFloat3.h"

#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace Json {
class Value;
}

namespace circuit {

class CCircuitAI;
class CCircuitDef;
class CGameTask;
class CChokeOverlay;

/*
 * The shape of the map for one ally team: which gaps are chokes, which mex clusters sit behind which chokes, where
 * the enemy is likely to walk, and how open the map is. Shared by allied AIs, built on the area worker.
 *
 * - The threat graph (CRegionGraph) is built from the cells any gating enemy movement type can cross. A cut in it
 *   cuts every gating type. Leak types (amphibious, spiders, jumpers) only make the clusters they reach leaky.
 * - The traffic field (CTrafficField) has one layer per enemy movement type, blended by the enemy's army mix.
 * - Map sides (start boxes, enemy factories) drive gating, pockets and openness. Territory (buildings per region,
 *   20 s to join, 60 s to leave) drives the current and forward lines.
 * - Defence points (CDefenceMatrix) get a pocket: the 1 to choke.entrances chokes the enemy must pass to walk in.
 *
 * Built only when a feature that reads it is on in config, or when the overlay asks. Everything here is analysis:
 * the features that act on it each have their own flag (SConfig)
 */
class CChokeMap {
public:
	// Enemy movement types: a terrain mobile type, split into walkers and jumpers
	using MoveKey = int;  // mobile type id x 2, + 1 for a jumper
	enum class MoveKind: char {GROUND, HOVER, AMPHIBIOUS, CLIMBER, JUMPER, NAVAL};
	enum class MoveRole: char {GATING, LEAK, NAVAL};
	struct SMoveClass {
		MoveKey key;
		int mobileId;
		float jumpRange;  // > 0: also crosses wall bands thinner than this
		MoveKind kind;
		MoveRole role;  // before promotion (choke.gate_share)
		std::string name;  // the terrain mobile type's move def, "-jump" for jumpers
	};
	// A start box, or a guess at one
	struct SRect {
		float x0, z0, x1, z1;
		bool Contains(const springai::AIFloat3& pos) const {
			return (pos.x >= x0) && (pos.x <= x1) && (pos.z >= z0) && (pos.z <= z1);
		}
	};

	struct SConfig {
		bool isChoke = false;  // choke.enable: choke defence (A), region splits of defence points, lanes
		bool isSpot = false;  // choke.spot.enable: scored spots for towers (F)
		bool isRadar = false;  // choke.radar.enable: the radar planner (B)
		bool isObjective = false;  // choke.objective.enable: forward objectives (D), rankings steering (E)
		bool isHold = false;  // quota.hold_choke: the idle army holds a frontline choke (C)
		bool isExpand = false;  // choke.expand: expansion bonus behind a defended cut
		float minArea = 0.5f;  // choke.region.min_area: main-tower ranges squared
		float merge = 0.7f;  // choke.region.merge
		float defensible = 2.f;  // choke.defensible: main-tower ranges
		int entrances = 3;  // choke.entrances
		float nearRanges = 2.5f;  // choke.near: main-tower ranges (not "near": a macro on Windows)
		float exposure = 600.f;  // choke.exposure: elmos
		float setback[2] = {0.6f, 0.8f};  // choke.setback: weapon ranges
		float lane = 96.f;  // choke.lane: elmos
		float gateShare = 0.25f;  // choke.gate_share
		float spread = 0.15f;  // choke.traffic.spread
		float bands[3] = {0.35f, 0.65f, 0.05f};  // choke.openness.bands
		float radarCoverage = 0.75f;  // choke.radar.coverage
		float radarShare = 0.05f;  // choke.radar.share
		float objGain = 1.5f;  // choke.objective.gain
		float objPower = 1.3f;  // choke.objective.power
		int objRetryFrames = FRAMES_PER_SEC * 180;  // choke.objective.retry
		bool IsAnyOn() const { return isChoke || isSpot || isRadar || isObjective || isHold || isExpand; }
	};

	// A defence point of a cluster (CDefenceMatrix), as the graph sees it
	struct SPointInfo {
		int cluster;
		int index;  // in CDefenceMatrix::GetDefPoints(cluster)
		springai::AIFloat3 pos;
		float income;  // of its mexes
		int region;  // NO_REGION: on a wall, open
		int entrances;  // max-flow from the enemy side, capped at choke.entrances + 1
		bool isOpen;  // more entrances than choke.entrances, or on the enemy side
		int pocket;  // index in GetPockets(), -1 when open or nothing can walk in
		bool isExposed;  // ground outside its pocket's cut within choke.exposure of its mexes
		bool isLeaky;  // exposed, or reached by enemy aircraft or a leak type without passing the cut
		std::vector<float> chokeDist;  // per choke of its pocket: walking elmos from the point, -1 out of reach
	};
	// Clusters whose defence points share a cut
	struct SPocket {
		std::vector<int> chokes;  // indices of its cut, ascending
		std::vector<int> chokeIds;  // their ids: the pocket's key across rebuilds
		std::vector<int> outer;  // per choke: its region on the enemy's side
		std::vector<int> points;  // indices in GetPointInfos()
		std::vector<bool> isInner;  // per region: behind the cut
		bool isQualified;  // every entrance defensible, within choke.near, outer side not enemy territory
		bool isOuterEnemy;  // an entrance's outer side is enemy territory
		bool isLeaky;  // one of its points is
		bool isHeld;  // one of its clusters is ours (queued or finished)
	};
	// A gap kept free of buildings through a narrow choke: a rectangle along the path through it
	struct SLane {
		int chokeId;
		springai::AIFloat3 centre;
		springai::AIFloat3 dir;  // unit vector along the path
		float halfLength;
		float halfWidth;
		bool Contains(float x, float z) const {
			const float dx = x - centre.x;
			const float dz = z - centre.z;
			return (std::fabs(dx * dir.x + dz * dir.z) <= halfLength) && (std::fabs(dz * dir.x - dx * dir.z) <= halfWidth);
		}
	};
	enum class Openness: char {CHOKEPOINTED = 0, SEMI_OPEN = 1, OPEN = 2};

public:
	CChokeMap(CCircuitAI* circuit);
	virtual ~CChokeMap();

	void SetAuthority(CCircuitAI* authority);
	CCircuitAI* GetAuthority() const { return circuit; }

	const SConfig& GetConfig() const { return config; }
	SConfig& GetConfig() { return config; }  // choke_overlay set
	bool IsReady() const { return isReady; }
	// Builds now if nothing is built yet (the overlay's first toggle with every flag off)
	void Request();
	// Rebuilds the graph and the traffic on the worker (choke_overlay set)
	void Rebuild();
	// Territory, pockets, leaks, lines and openness now (CDefenceMatrix::Init, once its points exist)
	void Reanalyze();
	int GetVersion() const { return version; }  // bumped whenever anything below changes

	float GetMainRange() const { return mainRange; }  // the main tower's range: the unit of size of the tuning keys
	CRegionGraph::SParams GetGraphParams() const;  // from the tuning keys
	const CRegionGraph& GetGraph() const { return graph; }
	const CTrafficField& GetTraffic() const { return traffic; }
	const SCellMask& GetReachMask() const { return reachMask; }  // cells any movement type in the game can cross
	springai::AIFloat3 GetCellPos(int cell) const;  // centre of a graph cell, on the ground
	int GetRegionAt(const springai::AIFloat3& pos) const;  // nearest passable cell's region within 2 cells
	float GetChokeTraffic(int choke) const;  // the field summed over the choke's line
	// Nothing acts on a choke until it is 20 s old (found after a rebuild)
	bool IsChokeSettled(int choke, int frame) const;
	// Enemy artillery seen in range of the choke within the last 5 minutes: no new towers there
	bool IsArtilleryNear(int choke, int frame) const;

	const std::vector<SMoveClass>& GetMoveClasses() const { return moveClasses; }
	const SCellMask& GetClassMask(int classIdx) const { return classMasks[classIdx]; }
	int FindMoveClass(const std::string& name) const;  // by unit def or class name, -1 if none
	bool IsGating(int classIdx) const;
	bool IsInSet(int classIdx) const { return typeStates[classIdx].isInSet; }
	float GetShare(int classIdx) const { return typeStates[classIdx].share; }
	bool IsEnemyAirSeen() const { return isAirSeen; }
	bool IsEnemyShipSeen() const { return !shipCells.empty(); }

	bool IsEnemySide(int region) const { return isEnemySide[region]; }
	bool IsOurSide(int region) const { return isOurSide[region]; }
	bool IsEnemyTerritory(int region) const { return isEnemyTerritory[region]; }
	bool IsOurTerritory(int region) const { return isOurTerritory[region]; }

	const std::vector<SPointInfo>& GetPointInfos() const { return pointInfos; }
	const SPointInfo* GetPointInfo(int cluster, int index) const;
	const std::vector<SPocket>& GetPockets() const { return pockets; }
	// Chokes of the cut nearest our territory against enemy territory (C), and against the enemy side (D)
	const std::vector<int>& GetCurrentLine() const { return currentLine; }
	const std::vector<int>& GetForwardLine() const { return forwardLine; }
	// Per region: the chokes of the cut nearest it from our territory (E), up to choke.entrances of them
	const std::vector<int>& GetEntrancesFromUs(int region) const;

	float GetOpenness() const { return openness; }
	Openness GetOpennessBand() const { return opennessBand; }
	static const char* GetBandName(Openness band);

	const std::vector<SLane>& GetLanes() const { return lanes; }
	int GetLaneVersion() const { return laneVersion; }
	bool IsOnLane(const springai::AIFloat3& pos) const;

	CChokeOverlay* GetOverlay() const { return overlay.get(); }
	// choke_overlay save: the chokes and pocket classes as a JSON fixture
	std::string MakeFixture() const;

private:
	void ReadConfig(const Json::Value& root);
	void Init();
	void Update();  // every 2 s on the authority's scheduler

	/*
	 * Building (ChokeMap.cpp). One job at a time on the area worker: masks from the height and slope maps (when
	 * the terrain changed), the threat graph (when masks or gating types changed), traffic layers (for the classes
	 * asked). Inputs are copied in, results swapped in on the main thread
	 */
	struct SBuildJob;
	void InitMoveClasses();
	std::shared_ptr<SBuildJob> MakeJob(bool isMasks, bool isGraph, const std::vector<int>& trafficClasses);
	static void RunJob(SBuildJob& job);
	void ApplyJob(SBuildJob& job);
	void StartJob(const std::shared_ptr<SBuildJob>& job);
	void ScheduleBuilds();
	void MakeLanes();

	/*
	 * Enemy types and traffic sources (ChokeTypes.cpp)
	 */
	struct STypeState {
		bool isInSet = false;
		bool isPromoted = false;  // its cost reached choke.gate_share (UpdateTypeSet): a gating type until it leaves
		bool isDefault = false;  // in the set by the opening default, not yet seen
		int lastSeen = -1;  // frame a unit of it (no commanders) or a live factory for it was last seen
		std::set<ICoreUnit::Id> factories;  // known live enemy factories that build it
		float cost = 0.f;  // faded army cost (CEnemyManager::GetArmyCostWeight)
		float share = 0.f;  // of the set's army cost, with a prior so shares start equal
		float blendShare = -1.f;  // share in the last traffic blend, -1: not in it
	};
	struct SFactory {
		std::vector<int> classes;  // that it builds
		int team;
	};
	// One enemy ally team's traffic source: its start box until one of its factories is seen, then its factories
	struct SSourceGroup {
		std::vector<SRect> boxes;  // its start box, or the 3 mirrors of our start (a square each) with no boxes
		int allyTeam = -1;  // learnt from its first factory
		std::vector<springai::AIFloat3> factoryPos;  // only adds: a dead factory's ground stays a source
		bool isGuess = false;  // a mirror of our start
	};
	struct STypeChange {
		bool isGating = false;  // the gating set (the graph's mask) changed
		bool isLeak = false;  // the set changed, the gating set didn't
		std::vector<int> joined;  // classes that need traffic floods
		std::vector<int> left;
	};
	static MoveKey KeyOf(const CCircuitDef* cdef);
	int ClassOf(const CCircuitDef* cdef) const;  // -1: no class (air, static, commander)
	void InitTypeSet();
	void InitSources();
	void ScanEnemies();
	void AddFactory(const springai::AIFloat3& pos, int allyTeam);
	STypeChange UpdateTypeSet();
	float UpdateShares();  // the set's faded cost
	void Reblend();
	std::vector<CTrafficField::Cells> GetSourceCells() const;

	/*
	 * Graph queries (ChokePockets.cpp): sides, territory, pockets, leaks, lines, openness
	 */
	void UpdateSides();
	void UpdateTerritory();
	void Analyze();
	void AnalyzePoints();
	void UpdateLeaks();
	void UpdateLines();
	void UpdateOpenness();
	void MakeDestinations();
	std::vector<float> ChokeCapacity(float defensibleCap, float wideCap) const;
	std::vector<int> RegionsWhere(const std::vector<bool>& flags) const;
	void CompareFixture() const;

	CCircuitAI* circuit;  // authority
	SConfig config;
	std::shared_ptr<CGameTask> updateTask;
	bool isRequested;  // something reads it: build and keep it current
	bool isReady;
	bool isBuilding;  // a job is on the worker
	bool isGraphDirty;  // gating types changed, or choke_overlay set
	bool isSourceDirty;  // enemy sources changed since the last traffic build
	bool isOpennessDirty;  // sides or the type set changed
	int jobSerial;  // drops a job's result after SetAuthority or a newer job
	int graphSerial;  // bumped on every graph build
	int version;
	float mainRange;

	// Terrain
	std::vector<SMoveClass> moveClasses;
	std::vector<SCellMask> classMasks;  // per class, 32 elmo cells
	SCellMask threatMask;  // union of the gating classes
	SCellMask reachMask;  // union of every class
	CRegionGraph graph;
	std::vector<SLane> lanes;
	std::vector<uint8_t> laneCells;  // per graph cell: under a lane
	int laneVersion;
	int heightVersion;  // CTerrainData's when the masks were built
	int maskFrame;  // last mask build
	int trafficFrame;  // last full traffic build
	int sourceFrame;  // last traffic build for a source change
	int nextScanFrame;
	int nextAnalysisFrame;
	bool isTrafficDirty;  // masks changed since the last traffic build
	std::vector<int> pendingTraffic;  // classes waiting for floods

	// Enemy types and sources
	std::vector<STypeState> typeStates;  // per class
	bool isOpening;
	int openingEnd;  // frame
	std::map<ICoreUnit::Id, SFactory> factories;  // known live enemy factories
	std::set<int> factoryTeams;  // enemy teams with a factory seen
	std::vector<SSourceGroup> sourceGroups;
	int enemyAllyCount;
	bool isAirSeen;
	std::vector<int> shipCells;  // graph cells where enemy ships were seen, ascending
	std::map<int, int> artySeen;  // choke id -> last frame enemy artillery was in range
	int blendFrame;

	// Sides and territory, per region index
	std::vector<SRect> ourBoxes;  // our start box, or a square around our start; empty until that is known
	bool isStartKnown;
	std::vector<bool> isEnemySide, isOurSide;
	std::vector<bool> isEnemyTerritory, isOurTerritory;
	struct STerritory {
		int since = -1;  // frame buildings were first seen there
		int lastSeen = -1;
	};
	std::map<int, STerritory> ourHold, enemyHold;  // region id -> buildings seen

	// Pockets and lines
	std::vector<SPointInfo> pointInfos;
	std::vector<SPocket> pockets;
	std::vector<std::vector<float>> pointChokeDist;  // per point info, per choke: walking elmos, -1 out of reach
	int distSerial;  // graphSerial of pointChokeDist
	std::vector<bool> isLeakCell;  // per graph cell: a leak type or aircraft reaches it without passing a cut
	std::vector<int> currentLine, forwardLine;
	// GetEntrancesFromUs, per region on first ask after each UpdateLines (main thread only)
	std::vector<int> entranceSources;
	std::vector<float> entranceCapacity;
	mutable std::vector<std::vector<int>> entrancesFromUs;
	mutable std::vector<bool> isEntranceKnown;
	float openness;
	Openness opennessBand;
	bool isOpennessSet;
	std::vector<CTrafficField::SDestination> destinations;

	CTrafficField traffic;
	std::unique_ptr<CChokeOverlay> overlay;
};

} // namespace circuit

#endif // SRC_CIRCUIT_TERRAIN_CHOKEMAP_H_

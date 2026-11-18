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
class CEnemyUnit;

/*
 * The shape of the map for one ally team: which gaps are chokes, which mex clusters sit behind which chokes, where
 * the enemy is likely to walk, and how open the map is. Shared by allied AIs, built on the area worker.
 *
 * - The threat graph (CRegionGraph) is built from the cells the gating masks can cross. A cut in it cuts every
 *   gating type. A mask gates while its share of the enemy's army holds (choke.gate_share on, half of it off), and
 *   the vehicles' mask with none (UpdateGating); the tower each cluster keeps answers the rest. Spiders and jumpers
 *   count for the kbots' mask (SMoveClass::graphClass): the cliffs they cross stay walls in it.
 * - The traffic field (CTrafficField) has one layer per enemy movement type, blended by the enemy's army mix.
 * - Map sides (start boxes, enemy factories) drive gating, pockets and openness. Held ground (who owns each region
 *   by the units the AI knows of, armies included, as the sides overlay tints it, once that has held 20 s, and
 *   joined to its side's main ground) drives the lines, pocket qualification and radar.
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
		MoveRole role;  // the graph's default with no vehicle type in the game (UpdateGating)
		int graphClass;  // whose graph mask its army cost counts for: spiders and jumpers walk as kbots
		bool isVehicle;  // the ground type that climbs the least (tanks): gates until an enemy army is seen
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
		bool isHold = false;  // quota.hold_choke: the idle army holds a frontline choke (C)
		bool isExpand = false;  // choke.expand: expansion bonus behind a defended cut
		float minArea = 0.5f;  // choke.region.min_area: main-tower ranges squared
		float merge = 0.7f;  // choke.region.merge
		int minCount = 12;  // choke.region.min_count: fewer regions relax merge and min_area (CRegionGraph::Build)
		float defensible = 2.f;  // choke.defensible: main-tower ranges
		int entrances = 3;  // choke.entrances
		float nearRanges = 2.5f;  // choke.near: main-tower ranges (not "near": a macro on Windows)
		float setback[2] = {0.6f, 0.8f};  // choke.setback: weapon ranges
		float lane = 96.f;  // choke.lane: elmos
		float gateShare = 0.25f;  // choke.gate_share
		float spread = 0.15f;  // choke.traffic.spread
		float bands[3] = {0.35f, 0.65f, 0.05f};  // choke.openness.bands
		float spotRadius = 300.f;  // choke.spot.radius: elmos from a defence point its towers may stand
		float spotHeight = 0.5f;  // choke.spot.height: the most height adds to a spot's score (0.5 = +50%)
		float spotHeightCap = 150.f;  // choke.spot.height_cap: elmos above the targets that earn all of it
		float spotEnemy = 1.f;  // choke.spot.enemy: weight of the enemy value a tower can shoot, per its own cost
		float spotView = 1.f;  // choke.spot.view: the most a spot gains for seeing all of its range (turret.view's)
		float spotForward = 0.5f;  // choke.spot.forward: the most a position's spot gains for leaning toward the enemy
		float radarGain = 0.02f;  // choke.radar.min_gain
		float radarShare = 0.05f;  // choke.radar.share
		float radarEnemy = 2.f;  // choke.radar.enemy
		float radarInside = 0.25f;  // choke.radar.inside
		std::string radarGuard;  // choke.radar.guard: the tower def that guards a new radar, "" = none
		float radarGuardRadius = 300.f;  // choke.radar.guard_radius
		bool IsAnyOn() const { return isChoke || isSpot || isRadar || isHold || isExpand; }
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
		std::vector<float> chokeDist;  // per choke of its pocket: walking elmos from the point, -1 out of reach
	};
	// Clusters whose defence points share a cut
	struct SPocket {
		std::vector<int> chokes;  // indices of its cut, ascending
		std::vector<int> chokeIds;  // their ids: the pocket's key across rebuilds
		std::vector<int> points;  // indices in GetPointInfos()
		std::vector<bool> isInner;  // per region: behind the cut
		bool isQualified;  // every entrance defensible, within choke.near, outer side not the enemy's built ground
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
	// Held ground, pockets, lines and openness now (CDefenceMatrix::Init, once its points exist)
	void Reanalyze();
	int GetVersion() const { return version; }  // bumped whenever what the overlay draws changes

	// The main tower (the first land defender, water on water maps), and its range: the unit of the tuning keys
	CCircuitDef* GetMainDef() const;
	float GetMainRange() const { return mainRange; }
	CRegionGraph::SParams GetGraphParams() const;  // from the tuning keys
	std::string DescribeRelaxed() const;  // "" unless the graph relaxed merge and min_area for min_count
	const CRegionGraph& GetGraph() const { return graph; }
	const CTrafficField& GetTraffic() const { return traffic; }
	const SCellMask& GetReachMask() const { return reachMask; }  // cells any movement type in the game can cross
	springai::AIFloat3 GetCellPos(int cell) const;  // centre of a graph cell, on the ground
	int GetRegionAt(const springai::AIFloat3& pos) const;  // nearest passable cell's region within 2 cells
	float GetChokeTraffic(int choke) const { return chokeTraffic[choke]; }  // the field summed over its line
	// Nothing acts on a choke until it is 20 s old (found after a rebuild)
	bool IsChokeSettled(int choke, int frame) const;
	// Enemy artillery seen in range of the choke within the last 5 minutes: no new towers there
	bool IsArtilleryNear(int choke, int frame) const;

	const std::vector<SMoveClass>& GetMoveClasses() const { return moveClasses; }
	const SCellMask& GetClassMask(int classIdx) const { return classMasks[classIdx]; }
	// Connected parts of a class's mask by cell (ForEachNeighbour's moves), -1 on walls: made when first asked
	const std::vector<int>& GetClassParts(int classIdx) const;
	int GetWalkClass(const CCircuitDef* cdef) const;  // the class it moves as, -1 if none (air, or no factory builds it)
	int FindMoveClass(const std::string& name) const;  // by unit def or class name, -1 if none
	bool IsGating(int classIdx) const;
	// Spiders and jumpers are a big share of the enemy army (UpdateGating): the turret picker skips traffic
	bool IsCliffCrosserArmy() const { return isCliffCrosserArmy; }
	std::string DescribeTypeSet() const;  // each type in the set: its name, role and share

	bool IsEnemySide(int region) const { return isEnemySide[region]; }
	bool IsOurSide(int region) const { return isOurSide[region]; }
	// Per region: the region itself, and each region the enemy side walks into only through it (a mesa or a dead end
	// off it). Only the region itself while the enemy side is unknown
	std::vector<bool> GetShelteredBy(int region) const;

	// Who owns ground (ChokePockets.cpp), by the units the AI knows of. The sides overlay tints regions by it, and the
	// lines go round the ground each side holds by it
	enum class Side: char {NONE, OURS, ENEMY, CONTESTED};
	struct SSideUnit {
		springai::AIFloat3 pos;
		bool isMobile;
		float range;
		float threat;  // > 0: armed (ours: power)
		float eco;  // unarmed buildings: metal
		bool isAntiLand;  // armed and hits ground units: a tower that can defend a choke
	};
	struct SStrength {
		float mobile = 0.f;  // armed mobiles' threat (ours: power)
		float statics = 0.f;  // armed buildings' threat
		float eco = 0.f;  // metal in unarmed buildings
		int structures = 0;  // buildings, armed or not
		float Threat() const { return mobile + statics; }
		void Add(const SSideUnit& u) {
			(u.isMobile ? mobile : (u.threat > 0.f) ? statics : eco) += (u.threat > 0.f) ? u.threat : u.eco;
			structures += u.isMobile ? 0 : 1;
		}
	};
	static const int ARMY_SEEN;  // frames: an enemy mobile counts where it was seen this long ago
	static const float CONTEST_SHARE;  // the other side's threat that contests a side's region or choke, per its own
	// A region: the side with buildings in it, contested when both have, or when the other side's threat there is
	// CONTEST_SHARE of the owner's; nobody's with no buildings, whatever armies stand in it
	static Side OwnerOf(const SStrength& ours, const SStrength& enemy);
	// A choke: defended by a side whose ground-hitting towers cover all of its line (cover: the share they do),
	// contested when both cover it, or when the other side's towers reach any of it or its mobiles there are
	// CONTEST_SHARE of the defender's threat; nobody's when no side covers all of it
	static Side ChokeOwnerOf(const SStrength& ours, const SStrength& enemy, float ourCover, float enemyCover);
	static bool IsCountedEnemy(const CEnemyUnit* enemy, int frame);
	// Both sides' units the owners count, aircraft aside: ours where they are now, enemy buildings until seen dead,
	// armed enemy mobiles where they were seen in the last ARMY_SEEN. Constructors bring nothing
	void GetSideUnits(std::vector<SSideUnit>& outOurs, std::vector<SSideUnit>& outEnemies) const;
	// The share of a choke's line (wall to wall: all of its crossable gap) in range of these units' towers that hit
	// ground, 0 to 1
	float GetCover(int choke, const std::vector<SSideUnit>& units) const;
	// Held ground (UpdateHeld), per region. Ours: regions we own. The enemy's: regions they own, and the enemy side
	// where we don't. Each only where it joins its side's main ground. Contested and nobody's regions are neither
	bool IsOurGround(int region) const { return isOurGround[region]; }
	bool IsEnemyGround(int region) const { return isEnemyGround[region]; }
	Side GetSettledOwner(int region) const { return settledOwner[region]; }  // the owner once settled, before joining
	std::vector<int> GetOurRegions() const;  // our held ground, or our side before we hold any
	// Nearer our start box than any enemy source (true until both are known): the half of the map we defend
	bool IsOurHalf(const springai::AIFloat3& pos) const;

	const std::vector<SPointInfo>& GetPointInfos() const { return pointInfos; }
	const SPointInfo* GetPointInfo(int cluster, int index) const;  // nullptr before the analysis
	const std::vector<SPocket>& GetPockets() const { return pockets; }
	// Our line (C): the chokes out of our held ground (GetOurRegions) into regions the enemy reaches from their held
	// ground without crossing ours. The enemy line: the chokes out of their held ground into regions we reach from
	// ours without crossing theirs. The front line: the chokes between our ground and theirs, neither side contested.
	// Ascending
	const std::vector<int>& GetCurrentLine() const { return currentLine; }
	const std::vector<int>& GetEnemyLine() const { return enemyLine; }
	const std::vector<int>& GetFrontLine() const { return frontLine; }
	// Per region: the chokes of the cut nearest it from our held ground (E), up to choke.entrances of them
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
	 * the terrain changed), the threat graph (when masks or gating types changed), and one pending class's traffic
	 * layer (with the others whose mask is the same at 64 elmos). Inputs are copied in, results swapped in on the
	 * main thread, and the next job starts right away
	 */
	struct SBuildJob;
	void InitMoveClasses();
	std::shared_ptr<SBuildJob> MakeJob(bool isMasks, bool isGraph, const std::vector<int>& trafficClasses);
	static void RunJob(SBuildJob& job);
	void ApplyJob(SBuildJob& job);
	void StartJob(const std::shared_ptr<SBuildJob>& job);
	void ScheduleBuilds();
	void QueueTraffic(int classIdx);
	void QueueAllTraffic();  // every class in the set
	void MakeLanes();
	void UpdateChokeTraffic();

	/*
	 * Enemy types and traffic sources (ChokeTypes.cpp)
	 */
	struct STypeState {
		bool isInSet = false;
		bool isMaskOn = false;  // as a graph mask (SMoveClass::graphClass): its share holds it on (UpdateGating)
		bool isGating = false;  // its mask shapes the threat graph (UpdateGating)
		bool isDefault = false;  // in the set by the opening default, not yet seen
		int lastSeen = -1;  // frame a unit of it (no commanders) or a live factory for it was last seen
		std::set<ICoreUnit::Id> factories;  // known live enemy factories that build it
		float cost = 0.f;  // faded army cost (CEnemyManager::GetArmyCostWeight)
		float share = 0.f;  // of the set's army cost, with a prior so shares start equal: the traffic blend
		float graphShare = 0.f;  // as a graph mask: the cost of the types walking as it over the set's plus the prior
		float blendShare = -1.f;  // share in the last traffic blend, -1: not in it
	};
	// One enemy ally team's traffic source: its start box until one of its factories is seen, then its factories
	struct SSourceGroup {
		std::vector<SRect> boxes;  // its start box, or the mirrors of our start (a square each) with no boxes
		int allyTeam = -1;  // learnt from its first factory
		std::vector<springai::AIFloat3> factoryPos;  // only adds: a dead factory's ground stays a source
		bool isGuess = false;  // a mirror of our start
	};
	struct STypeChange {
		bool isGating = false;  // the gating set (the graph's mask) changed
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
	void UpdateGating(float total);
	float UpdateShares();  // the set's faded cost
	void Reblend();
	std::vector<CTrafficField::Cells> GetSourceCells() const;

	/*
	 * Graph queries (ChokePockets.cpp): sides, held ground, pockets, lines, openness
	 */
	// Cuts from one set of source regions to each region, found on first ask (main thread only)
	struct SCutCache {
		std::vector<int> sources;
		std::vector<float> capacity;  // per choke
		float maxFlow = 0.f;
		std::vector<CRegionGraph::SCut> cuts;  // per region
		std::vector<bool> isKnown;
		void Reset(std::vector<int>&& from, std::vector<float>&& cap, float limit, size_t regionCount);
		const CRegionGraph::SCut& Get(const CRegionGraph& graph, int region);  // an empty cut out of range
	};
	void UpdateSides();
	void UpdateHeld();
	void Analyze();
	void AnalyzePoints();
	void UpdateLines();
	void UpdateOpenness();
	void MakeDestinations();
	std::vector<float> ChokeCapacity(float defensibleCap, float wideCap) const;
	std::vector<int> RegionsWhere(const std::vector<bool>& flags) const;

	CCircuitAI* circuit;  // authority
	SConfig config;
	std::shared_ptr<CGameTask> updateTask;
	bool isRequested;  // something reads it: build and keep it current
	bool isReady;
	bool isBuilding;  // a job is on the worker
	bool isGraphDirty;  // gating types changed, or choke_overlay set
	bool isSourceDirty;  // enemy sources changed since the last traffic build
	bool isOpennessDirty;  // sides or masks changed
	int jobSerial;  // drops a job's result after SetAuthority or a newer job
	int version;
	std::size_t analysisHash;  // of what the overlay draws from Analyze
	float mainRange;

	// Terrain
	std::vector<SMoveClass> moveClasses;
	std::vector<SCellMask> classMasks;  // per class, 32 elmo cells
	mutable std::vector<std::vector<int>> classParts;  // per class, empty until asked (GetClassParts)
	SCellMask reachMask;  // union of every class
	CRegionGraph graph;  // built on the union of the gating classes
	std::vector<SLane> lanes;
	int laneVersion;
	int heightVersion;  // CTerrainData's when the masks were built
	int maskFrame;  // last mask build
	int graphFrame;  // last threat graph build
	bool isTerrainPending;  // the masks changed since the graph was last checked against them
	int trafficFrame;  // last full traffic build
	int sourceFrame;  // last traffic build for a source change
	int nextScanFrame;
	int nextAnalysisFrame;
	bool isTrafficDirty;  // masks changed since the last traffic build
	std::vector<int> pendingTraffic;  // classes waiting for floods
	std::vector<float> chokeTraffic;  // per choke

	// Enemy types and sources
	std::vector<STypeState> typeStates;  // per class
	bool isCliffCrosserArmy = false;
	bool isArmyGraph = false;  // a mask has gated: the vehicles' default is over (UpdateGating)
	bool isOpening;
	std::map<ICoreUnit::Id, std::vector<int>> factories;  // known live enemy factories: the classes each builds
	std::set<int> factoryTeams;  // enemy teams with a factory seen
	std::vector<SSourceGroup> sourceGroups;
	int enemyAllyCount;
	std::map<int, int> artySeen;  // choke id -> last frame enemy artillery was in range
	int blendFrame;

	// Sides and held ground, per region index
	std::vector<SRect> ourBoxes;  // our start box, or a square around our start; empty until that is known
	bool isStartKnown;
	std::vector<bool> isEnemySide, isOurSide;
	struct SHeld {
		Side side = Side::NONE;  // the owner the lines take
		Side next = Side::NONE;  // the owner now, settling in
		int since = -1;  // frame next became the owner
	};
	std::map<int, SHeld> held;  // region id
	std::vector<Side> settledOwner;
	std::vector<bool> isOurGround, isEnemyGround;

	// Pockets and lines
	std::vector<SPointInfo> pointInfos;
	std::vector<int> firstPointOf;  // per cluster: index of its first point info
	std::vector<SPocket> pockets;
	std::vector<std::vector<float>> pointChokeDist;  // per point info, per choke: walking elmos, -1 out of reach
	SCutCache sideCuts;  // from the enemy side, up to choke.entrances (AnalyzePoints)
	mutable SCutCache entranceCuts;  // from our regions (GetEntrancesFromUs)
	std::vector<int> currentLine, enemyLine, frontLine;
	float openness;
	Openness opennessBand;
	bool isOpennessSet;
	std::vector<CTrafficField::SDestination> destinations;

	CTrafficField traffic;
	std::unique_ptr<CChokeOverlay> overlay;
};

} // namespace circuit

#endif // SRC_CIRCUIT_TERRAIN_CHOKEMAP_H_

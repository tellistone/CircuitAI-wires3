/*
 * ChokeOverlay.h
 *
 *  Created on: Nov 11, 2026
 *      Author: Claude
 */

#ifndef SRC_CIRCUIT_TERRAIN_CHOKEOVERLAY_H_
#define SRC_CIRCUIT_TERRAIN_CHOKEOVERLAY_H_

#include "terrain/RegionGraph.h"

#include "AIFloat3.h"

#include <map>
#include <string>
#include <vector>

namespace circuit {

class CCircuitAI;
class CChokeMap;

/*
 * The choke_overlay chat command: draws what CChokeMap found with the AI interface's map figures, so it works in
 * release builds with no widget. One overlay per ally team (it belongs to the choke map), so allied AIs never draw
 * twice. See doc/chokepoints.md for the commands
 */
class CChokeOverlay {
public:
	enum class Layer: char {CHOKES, POCKETS, LINES, RANKS, TRAFFIC, SPOTS, RADAR, REGIONS, COUNT};

	CChokeOverlay(CChokeMap* chokeMap);
	virtual ~CChokeOverlay();

	void SetAuthority(CCircuitAI* authority);
	// A chat line any of our AIs got: true when it was a choke_overlay command (handled once per ally team)
	bool OnChat(CCircuitAI* ai, int playerId, const char* message);
	// Redraws when the choke map changed, and every 10 s for the layers other modules feed (CChokeMap::Update)
	void Update();
	bool IsOn() const { return isOn; }

private:
	void Toggle();
	void NextLayer();
	void ShowType(const std::string& name);
	void SetValue(const std::string& key, const std::string& value);
	void Save();
	bool IsHumanOpponent() const;
	bool IsHumanAlly() const;
	void Say(const std::string& text) const;  // to the log, and to the sender's allies when it answers a command

	void Draw();
	void Clear();
	void DrawChokes(const CRegionGraph& graph);
	void DrawRegions(const CRegionGraph& graph);
	void DrawPockets();
	void DrawLines();
	void DrawRanks();
	void DrawTraffic();
	void DrawSpots();
	void DrawRadar();
	void DrawLanes();

	// One figure group per colour
	enum class Colour: char {GREEN, GREY, RED, ORANGE, YELLOW, BLUE, CYAN, WHITE, MAGENTA};
	void AddLine(const springai::AIFloat3& from, const springai::AIFloat3& to, Colour colour, float width = 16.f);
	void AddCross(const springai::AIFloat3& pos, Colour colour, float size = 48.f);
	void AddLabel(const springai::AIFloat3& pos, const std::string& text);
	springai::AIFloat3 OnGround(float x, float z, float lift = 24.f) const;

	CChokeMap* chokeMap;
	CCircuitAI* circuit;  // authority
	bool isOn;
	bool isLabels;  // map markers, which human allies see: off by default when there are any
	Layer layer;
	int typeClass;  // -1: the threat graph; else one movement class's own graph
	CRegionGraph typeGraph;
	int drawnVersion;
	int drawnFrame;
	int commandFrame;  // allied AIs all get the same chat line: handle it once
	std::string command;
	std::map<Colour, int> groups;  // figure group ids
	std::vector<springai::AIFloat3> labels;
};

} // namespace circuit

#endif // SRC_CIRCUIT_TERRAIN_CHOKEOVERLAY_H_

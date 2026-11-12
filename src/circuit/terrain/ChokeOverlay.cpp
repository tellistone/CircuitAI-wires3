/*
 * ChokeOverlay.cpp
 *
 *  Created on: Nov 11, 2026
 *      Author: Claude
 */

#include "terrain/ChokeOverlay.h"
#include "terrain/ChokeMap.h"
#include "module/ChokeDefence.h"
#include "module/Frontline.h"
#include "module/MilitaryManager.h"
#include "module/SensorPlanner.h"
#include "module/SpotScorer.h"
#include "setup/DefenceMatrix.h"
#include "setup/SetupData.h"
#include "terrain/TerrainManager.h"
#include "unit/ally/AllyTeam.h"
#include "CircuitAI.h"
#include "util/GameAttribute.h"
#include "util/FileSystem.h"
#include "util/Utils.h"

#include "spring/SpringCallback.h"
#include "spring/SpringMap.h"

#include "AIColor.h"
#include "DataDirs.h"
#include "Drawer.h"
#include "Figure.h"
#include "Game.h"
#include "Log.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace circuit {

using namespace springai;

#define LIVE_FRAMES		(FRAMES_PER_SEC * 10)  // layers fed by other modules redraw this often
#define TRAFFIC_SHOWN	1500  // busiest traffic cells drawn
#define COMMAND			"choke_overlay"

namespace {

const char* LAYER_NAMES[] = {"chokes", "pockets", "lines", "ranks", "traffic", "spots", "radar", "regions"};

AIColor ColourOf(int colour)
{
	static const AIColor colours[] = {
		AIColor(0.2f, 0.9f, 0.2f),  // GREEN
		AIColor(0.6f, 0.6f, 0.6f),  // GREY
		AIColor(1.0f, 0.2f, 0.2f),  // RED
		AIColor(1.0f, 0.6f, 0.1f),  // ORANGE
		AIColor(1.0f, 1.0f, 0.2f),  // YELLOW
		AIColor(0.2f, 0.4f, 1.0f),  // BLUE
		AIColor(0.2f, 0.9f, 0.9f),  // CYAN
		AIColor(1.0f, 1.0f, 1.0f),  // WHITE
		AIColor(1.0f, 0.3f, 1.0f),  // MAGENTA
	};
	return colours[colour];
}

}  // namespace

CChokeOverlay::CChokeOverlay(CChokeMap* chokeMap)
		: chokeMap(chokeMap)
		, circuit(chokeMap->GetAuthority())
		, isOn(false)
		, isLabels(false)
		, layer(Layer::CHOKES)
		, typeClass(-1)
		, drawnVersion(-1)
		, drawnFrame(-1)
		, commandFrame(-1)
{
	isLabels = !IsHumanIn(true);
}

CChokeOverlay::~CChokeOverlay()
{
}

void CChokeOverlay::SetAuthority(CCircuitAI* authority)
{
	circuit = authority;
}

bool CChokeOverlay::OnChat(CCircuitAI* ai, int playerId, const char* message)
{
	std::istringstream in(message);
	std::string word;
	if (!(in >> word) || (word != COMMAND)) {
		return false;
	}
	const int frame = ai->GetLastFrame();
	if ((frame == commandFrame) && (command == message)) {
		return true;  // one of our allied AIs got it first
	}
	commandFrame = frame;
	command = message;
	// From our ally team, or from a spectator of a game with no human opponents
	COOAICallback* clb = ai->GetCallback();
	const int team = clb->Game_GetPlayerTeam(playerId);
	const bool isOurs = (team >= 0) && (clb->Game_GetTeamAllyTeam(team) == ai->GetAllyTeamId());
	if (!isOurs && ((team >= 0) || IsHumanIn(false))) {
		return true;
	}

	std::string verb;
	in >> verb;
	const auto layerIt = std::find(std::begin(LAYER_NAMES), std::end(LAYER_NAMES), verb);
	if (verb.empty() || (verb == "toggle")) {
		Toggle();
	} else if (verb == "next") {
		NextLayer();
	} else if (layerIt != std::end(LAYER_NAMES)) {
		layer = static_cast<Layer>(layerIt - std::begin(LAYER_NAMES));
		isOn = true;
		chokeMap->Request();
		Draw();
		Say(std::string("layer ") + verb);
	} else if (verb == "labels") {
		isLabels = !isLabels;
		Say(isLabels ? "labels on" : "labels off");
		Draw();
	} else if (verb == "set") {
		std::string key, value;
		in >> key;
		std::getline(in >> std::ws, value);
		SetValue(key, value);
	} else if (verb == "save") {
		Save();
	} else {
		ShowType(verb);
	}
	return true;
}

void CChokeOverlay::Update()
{
	if (!isOn) {
		return;
	}
	const bool isLive = (layer == Layer::LINES) || (layer == Layer::RANKS) || (layer == Layer::SPOTS)
			|| (layer == Layer::RADAR);
	const int frame = circuit->GetLastFrame();
	if ((drawnVersion != chokeMap->GetVersion()) || (isLive && (frame >= drawnFrame + LIVE_FRAMES))) {
		Draw();
	}
}

void CChokeOverlay::OnGraphBuilt()
{
	if (isOn) {
		Say("threat graph rebuilt: " + DescribeGraph());
	}
}

/*
 * Its size and the enemy types it was built from: a leak type promoted to gating (choke.gate_share, spiders say)
 * opens most of the walls the old graph had, so its chokes move a long way
 */
std::string CChokeOverlay::DescribeGraph() const
{
	const CRegionGraph& graph = chokeMap->GetGraph();
	return utils::int_to_string(graph.GetRegions().size(), "%i regions, ")
			+ utils::int_to_string(graph.GetChokes().size(), "%i chokes; enemy types: ") + chokeMap->DescribeTypeSet()
			+ "; '" COMMAND " <class>' shows one class's own graph";
}

void CChokeOverlay::Toggle()
{
	isOn = !isOn;
	if (!isOn) {
		Clear();
		Say("off");
		return;
	}
	chokeMap->Request();  // builds it if no feature asked for it
	Draw();
	Say(std::string("on, layer ") + LAYER_NAMES[static_cast<int>(layer)] + ", openness "
			+ utils::float_to_string(chokeMap->GetOpenness(), "%.2f ") + CChokeMap::GetBandName(chokeMap->GetOpennessBand())
			+ (chokeMap->IsReady() ? "; " + DescribeGraph() : " (building)"));
}

void CChokeOverlay::NextLayer()
{
	layer = static_cast<Layer>((static_cast<int>(layer) + 1) % static_cast<int>(Layer::COUNT));
	isOn = true;
	chokeMap->Request();
	Draw();
	Say(std::string("layer ") + LAYER_NAMES[static_cast<int>(layer)]);
}

/*
 * One movement class's own graph (by its name or a unit def's), "all" for the threat graph
 */
void CChokeOverlay::ShowType(const std::string& name)
{
	if (name == "all") {
		typeClass = -1;
		Say("threat graph");
		Draw();
		return;
	}
	const int classIdx = chokeMap->FindMoveClass(name);
	if (classIdx < 0) {
		std::string names;
		for (const CChokeMap::SMoveClass& mc : chokeMap->GetMoveClasses()) {
			names += " " + mc.name;
		}
		Say("unknown command or movement class '" + name + "'; classes:" + names);
		return;
	}
	typeClass = classIdx;
	typeGraph.Build(chokeMap->GetClassMask(classIdx), chokeMap->GetGraphParams());
	Say(chokeMap->GetMoveClasses()[classIdx].name + ": " + utils::int_to_string(typeGraph.GetRegions().size(), "%i regions, ")
			+ utils::int_to_string(typeGraph.GetChokes().size(), "%i chokes"));
	isOn = true;
	Draw();
}

/*
 * Tuning keys as in the "choke" config block (quota.hold_choke as hold_choke), then a rebuild. Only in games with no
 * human opponents
 */
void CChokeOverlay::SetValue(const std::string& key, const std::string& value)
{
	if (IsHumanIn(false)) {
		Say("set is off in games with human opponents");
		return;
	}
	CChokeMap::SConfig& c = chokeMap->GetConfig();
	const std::map<std::string, float*> floats = {
		{"region.min_area", &c.minArea}, {"region.merge", &c.merge}, {"defensible", &c.defensible},
		{"near", &c.nearRanges}, {"lane", &c.lane}, {"gate_share", &c.gateShare}, {"traffic.spread", &c.spread},
		{"spot.radius", &c.spotRadius}, {"spot.height", &c.spotHeight}, {"spot.height_cap", &c.spotHeightCap},
		{"spot.enemy", &c.spotEnemy}, {"spot.forward", &c.spotForward},
		{"radar.min_gain", &c.radarGain}, {"radar.share", &c.radarShare}, {"radar.enemy", &c.radarEnemy},
		{"radar.inside", &c.radarInside}, {"radar.guard_radius", &c.radarGuardRadius},
		{"objective.gain", &c.objGain}, {"objective.power", &c.objPower},
	};
	const std::map<std::string, bool*> flags = {
		{"enable", &c.isChoke}, {"spot.enable", &c.isSpot}, {"radar.enable", &c.isRadar},
		{"objective.enable", &c.isObjective}, {"expand", &c.isExpand}, {"hold_choke", &c.isHold},
	};
	// Read into copies, so a bad value changes nothing
	std::istringstream in(value);
	auto floatIt = floats.find(key);
	auto flagIt = flags.find(key);
	bool isRead = false;
	if (floatIt != floats.end()) {
		float v;
		isRead = bool(in >> v);
		if (isRead) {
			*floatIt->second = v;
		}
	} else if (flagIt != flags.end()) {
		bool v;
		isRead = bool(in >> std::boolalpha >> v);
		if (isRead) {
			*flagIt->second = v;
		}
	} else if (key == "entrances") {
		int v;
		isRead = bool(in >> v);
		if (isRead) {
			c.entrances = v;
		}
	} else if (key == "setback") {
		float a, b;
		isRead = bool(in >> a >> b);
		if (isRead) {
			c.setback[0] = a;
			c.setback[1] = b;
		}
	}
	if (!isRead) {
		Say("can't set '" + key + "' to '" + value + "'");
		return;
	}
	Say(key + " = " + value + ", rebuilding");
	chokeMap->Rebuild();
}

/*
 * The chokes and point classes as a JSON fixture, which debug builds compare against at start
 */
void CChokeOverlay::Save()
{
	if (IsHumanIn(false)) {
		Say("save is off in games with human opponents");
		return;
	}
	const std::string relPath = "choke/" + utils::MakeFileSystemCompatible(circuit->GetMap()->GetName()) + ".json";
	DataDirs* dataDirs = circuit->GetCallback()->GetDataDirs();
	char absPath[2048];
	const bool isLocated = dataDirs->LocatePath(absPath, sizeof(absPath), relPath.c_str(), true /*writeable*/,
			true /*create*/, false /*dir*/, false /*common*/);
	delete dataDirs;
	std::ofstream file;
	if (isLocated) {
		file.open(absPath);
		file << chokeMap->MakeFixture();
	}
	Say((isLocated && file.good()) ? std::string("saved ") + absPath : "can't write " + relPath);
}

bool CChokeOverlay::IsHumanIn(bool isAlly) const
{
	CSetupData& setupData = circuit->GetGameAttribute()->GetSetupData();
	COOAICallback* clb = circuit->GetCallback();
	for (int i = 0; i < setupData.GetAllyTeamCount(); ++i) {
		if ((i == circuit->GetAllyTeamId()) != isAlly) {
			continue;
		}
		for (int team : setupData.GetAllyTeam(i)->GetTeamIds()) {
			if (!clb->Team_HasAIController(team)) {
				return true;
			}
		}
	}
	return false;
}

void CChokeOverlay::Say(const std::string& text) const
{
	circuit->LOG("CHOKE overlay: %s", text.c_str());
	circuit->GetGame()->SendTextMessage(("/say a:<CircuitAI> " COMMAND ": " + text).c_str(), 0);
}

void CChokeOverlay::Draw()
{
	Clear();
	drawnVersion = chokeMap->GetVersion();
	drawnFrame = circuit->GetLastFrame();
	if (!isOn || !chokeMap->IsReady()) {
		return;
	}
	const CRegionGraph& graph = (typeClass >= 0) ? typeGraph : chokeMap->GetGraph();
	switch (layer) {
		case Layer::CHOKES: {
			DrawChokes(graph);
			DrawLanes();
		} break;
		case Layer::POCKETS: DrawPockets(); break;
		case Layer::LINES: DrawLines(); break;
		case Layer::RANKS: DrawRanks(); break;
		case Layer::TRAFFIC: DrawTraffic(); break;
		case Layer::SPOTS: DrawSpots(); break;
		case Layer::RADAR: DrawRadar(); break;
		case Layer::REGIONS: DrawRegions(graph); break;
		default: break;
	}
}

void CChokeOverlay::Clear()
{
	Figure* figure = circuit->GetDrawer()->GetFigure();
	for (const auto& kv : groups) {
		figure->Remove(kv.second);
	}
	delete figure;
	groups.clear();
	for (const AIFloat3& pos : labels) {
		circuit->GetDrawer()->DeletePointsAndLines(pos);
	}
	labels.clear();
}

/*
 * Defensible chokes green, wider ones grey, each from end to end of its line
 */
void CChokeOverlay::DrawChokes(const CRegionGraph& graph)
{
	const SCellMask& mask = graph.GetMask();
	for (const CRegionGraph::SChoke& choke : graph.GetChokes()) {
		int a = choke.line.front(), b = choke.line.front();
		float bestSq = -1.f;
		for (int i : choke.line) {
			for (int j : choke.line) {
				const float sq = SQUARE(mask.CentreX(i) - mask.CentreX(j)) + SQUARE(mask.CentreZ(i) - mask.CentreZ(j));
				if (sq > bestSq) {
					bestSq = sq;
					a = i;
					b = j;
				}
			}
		}
		const Colour colour = choke.isDefensible ? Colour::GREEN : Colour::GREY;
		AddLine(OnGround(mask.CentreX(a), mask.CentreZ(a)), OnGround(mask.CentreX(b), mask.CentreZ(b)), colour, 24.f);
		AddLabel(OnGround(mask.CentreX(choke.centre), mask.CentreZ(choke.centre)),
				utils::int_to_string(choke.id, "choke %i") + utils::int_to_string(choke.width, ", %i wide"));
	}
}

void CChokeOverlay::DrawLanes()
{
	for (const CChokeMap::SLane& lane : chokeMap->GetLanes()) {
		const AIFloat3 along = lane.dir * lane.halfLength;
		const AIFloat3 across = AIFloat3(-lane.dir.z, 0.f, lane.dir.x) * lane.halfWidth;
		const AIFloat3 corners[] = {lane.centre + along + across, lane.centre + along - across,
				lane.centre - along - across, lane.centre - along + across};
		for (int i = 0; i < 4; ++i) {
			const AIFloat3& p = corners[i];
			const AIFloat3& q = corners[(i + 1) % 4];
			AddLine(OnGround(p.x, p.z), OnGround(q.x, q.z), Colour::YELLOW, 8.f);
		}
	}
}

/*
 * Each pocket's entrances by its mode (green at chokes, white on cluster, grey undecided), its points linked to them;
 * points red when open, green gated, grey unreached; choke points' anchors magenta
 */
void CChokeOverlay::DrawPockets()
{
	const CRegionGraph& graph = chokeMap->GetGraph();
	CDefenceMatrix* defence = circuit->GetAllyTeam()->GetDefenceMatrix().get();
	const std::vector<CChokeMap::SPointInfo>& infos = chokeMap->GetPointInfos();
	for (const CChokeMap::SPocket& pocket : chokeMap->GetPockets()) {
		const CDefenceMatrix::SPocketState* state = defence->GetPocketState(pocket.chokeIds);
		const Colour colour = (state == nullptr) ? Colour::GREY
				: (state->mode == CDefenceMatrix::DefenceMode::CHOKES) ? Colour::GREEN : Colour::WHITE;
		for (int c : pocket.chokes) {
			const AIFloat3 centre = chokeMap->GetCellPos(graph.GetChokes()[c].centre);
			AddCross(centre, colour, 96.f);
			for (int p : pocket.points) {
				AddLine(OnGround(infos[p].pos.x, infos[p].pos.z), OnGround(centre.x, centre.z), Colour::BLUE, 8.f);
			}
		}
		const AIFloat3 first = chokeMap->GetCellPos(graph.GetChokes()[pocket.chokes.front()].centre);
		AddLabel(first, std::string("pocket ") + ((state != nullptr) ? CChokeDefence::GetModeName(state->mode) : "undecided")
				+ (pocket.isQualified ? "" : ", not qualified"));
	}
	for (const CChokeMap::SPointInfo& pi : infos) {
		AddCross(pi.pos, pi.isOpen ? Colour::RED : (pi.pocket < 0) ? Colour::GREY : Colour::GREEN);
		AddLabel(pi.pos, pi.isOpen ? std::string("open") : (pi.pocket < 0) ? std::string("unreached")
				: utils::int_to_string(pi.entrances, "gated %i"));
	}
	for (const auto& kv : defence->GetChokePoints()) {
		AddCross(kv.second.position, Colour::MAGENTA);
		AddLabel(kv.second.position, utils::int_to_string(kv.first, "anchor of choke %i")
				+ utils::float_to_string(kv.second.cost, ", %.0f metal"));
	}
}

/*
 * Current line orange, forward line yellow, objective magenta; hold slots cyan, with their spots
 */
void CChokeOverlay::DrawLines()
{
	const CRegionGraph& graph = chokeMap->GetGraph();
	const std::vector<CRegionGraph::SChoke>& chokes = graph.GetChokes();
	auto drawChoke = [&](int c, Colour colour, float size) {
		AddCross(chokeMap->GetCellPos(chokes[c].centre), colour, size);
	};
	for (int c : chokeMap->GetForwardLine()) {
		drawChoke(c, Colour::YELLOW, 128.f);
	}
	for (int c : chokeMap->GetCurrentLine()) {
		drawChoke(c, Colour::ORANGE, 96.f);
	}
	const CFrontline* frontline = circuit->GetMilitaryManager()->GetFrontline();
	for (int id : frontline->GetObjectiveChokes()) {
		const int c = graph.FindChoke(id);
		if (c >= 0) {
			drawChoke(c, Colour::MAGENTA, 160.f);
		}
	}
	for (const CFrontline::SFront& f : frontline->GetFront()) {
		const int c = graph.FindChoke(f.chokeId);
		if (c >= 0) {
			AddLabel(chokeMap->GetCellPos(chokes[c].centre), utils::float_to_string(f.rank, "front: rank %.1f")
					+ utils::float_to_string(f.wanted, ", wants %.0f") + utils::float_to_string(f.towers, ", towers %.0f"));
		}
	}
	for (const CFrontline::SHold& hold : frontline->GetHolds()) {
		const int c = graph.FindChoke(hold.chokeId);
		if (c < 0) {
			continue;
		}
		const AIFloat3 centre = chokeMap->GetCellPos(chokes[c].centre);
		drawChoke(c, Colour::CYAN, 64.f);
		if (utils::is_valid(hold.spot)) {
			AddCross(hold.spot, Colour::CYAN);
			AddLine(OnGround(hold.spot.x, hold.spot.z), OnGround(centre.x, centre.z), Colour::CYAN, 8.f);
		}
		AddLabel(centre, (hold.task != nullptr) ? "hold: held" : "hold: free");
	}
}

/*
 * Enemy-held chokes: red when defended, white when not, with E's numbers
 */
void CChokeOverlay::DrawRanks()
{
	const CRegionGraph& graph = chokeMap->GetGraph();
	for (const CFrontline::SRank& rank : circuit->GetMilitaryManager()->GetFrontline()->GetRanks()) {
		const int c = graph.FindChoke(rank.chokeId);
		if (c < 0) {
			continue;
		}
		const AIFloat3 centre = chokeMap->GetCellPos(graph.GetChokes()[c].centre);
		AddCross(centre, (rank.defence > 0.f) ? Colour::RED : Colour::WHITE, 96.f);
		AddLabel(centre, utils::float_to_string(rank.defence, "enemy-held: defence %.0f")
				+ utils::float_to_string(rank.value, ", value %.0f")
				+ utils::float_to_string(rank.confidence * 100.f, ", seen %.0f%%"));
	}
}

/*
 * The blended traffic field's busiest cells, bigger and redder the busier
 */
void CChokeOverlay::DrawTraffic()
{
	const CTrafficField& traffic = chokeMap->GetTraffic();
	const int cellCount = traffic.GetWidth() * traffic.GetHeight();
	float maxValue = 0.f;
	for (int cell = 0; cell < cellCount; ++cell) {
		maxValue = std::max(maxValue, traffic.GetAt(cell));
	}
	if (maxValue <= 0.f) {
		return;
	}
	std::vector<std::pair<float, int>> cells;
	for (int cell = 0; cell < cellCount; ++cell) {
		const float value = traffic.GetAt(cell) / maxValue;
		if (value >= 0.2f) {
			cells.push_back({value, cell});
		}
	}
	std::sort(cells.begin(), cells.end(), std::greater<std::pair<float, int>>());
	cells.resize(std::min<size_t>(cells.size(), TRAFFIC_SHOWN));
	const int size = traffic.GetCellSize();
	for (const auto& vc : cells) {
		const AIFloat3 pos((vc.second % traffic.GetWidth() + 0.5f) * size, 0.f, (vc.second / traffic.GetWidth() + 0.5f) * size);
		const Colour colour = (vc.first >= 0.6f) ? Colour::RED : (vc.first >= 0.35f) ? Colour::ORANGE : Colour::YELLOW;
		AddCross(pos, colour, 8.f + 24.f * vc.first);
	}
}

/*
 * The last spot search's candidates: the best green, the next 4 cyan, the rest grey
 */
void CChokeOverlay::DrawSpots()
{
	std::vector<CSpotScorer::SCandidate> candidates = circuit->GetMilitaryManager()->GetSpotScorer()->GetLastCandidates();
	std::sort(candidates.begin(), candidates.end(), [](const CSpotScorer::SCandidate& a, const CSpotScorer::SCandidate& b) {
		return a.score > b.score;
	});
	for (size_t i = 0; i < candidates.size(); ++i) {
		const Colour colour = (i == 0) ? Colour::GREEN : (i < 5) ? Colour::CYAN : Colour::GREY;
		AddCross(candidates[i].pos, colour, (i == 0) ? 64.f : 24.f);
	}
	if (!candidates.empty()) {
		AddLabel(candidates.front().pos, utils::float_to_string(candidates.front().score, "best spot, score %.2f"));
	}
}

/*
 * The radar planner's candidates blue, the edge of today's coverage cyan
 */
void CChokeOverlay::DrawRadar()
{
	const CSensorPlanner* planner = circuit->GetMilitaryManager()->GetSensorPlanner();
	for (const AIFloat3& pos : planner->GetCandidates()) {
		AddCross(pos, Colour::BLUE, 32.f);
	}
	const CTrafficField& traffic = chokeMap->GetTraffic();
	const std::vector<uint8_t>& isCovered = planner->GetCovered();
	const int width = traffic.GetWidth();
	const int height = traffic.GetHeight();
	const float size = traffic.GetCellSize();
	const AIFloat3 centre = OnGround(width * size * 0.5f, height * size * 0.5f);
	if ((int)isCovered.size() != width * height) {
		AddLabel(centre, "radar planner off (choke.radar.enable)");
		return;
	}
	for (int z = 0; z < height; ++z) {
		for (int x = 0; x < width; ++x) {
			const bool is = isCovered[z * width + x];
			if ((x + 1 < width) && (is != (bool)isCovered[z * width + x + 1])) {
				AddLine(OnGround((x + 1) * size, z * size), OnGround((x + 1) * size, (z + 1) * size), Colour::CYAN, 8.f);
			}
			if ((z + 1 < height) && (is != (bool)isCovered[(z + 1) * width + x])) {
				AddLine(OnGround(x * size, (z + 1) * size), OnGround((x + 1) * size, (z + 1) * size), Colour::CYAN, 8.f);
			}
		}
	}
	AddLabel(centre, utils::float_to_string(planner->GetCoveredShare() * 100.f, "radar covers %.0f%% of the weight"));
}

/*
 * The graph itself: region centres (green on our side, red on the enemy's, white otherwise) linked to their chokes
 */
void CChokeOverlay::DrawRegions(const CRegionGraph& graph)
{
	const SCellMask& mask = graph.GetMask();
	const bool isThreatGraph = (typeClass < 0);
	for (int r = 0; r < (int)graph.GetRegions().size(); ++r) {
		const CRegionGraph::SRegion& region = graph.GetRegions()[r];
		const AIFloat3 centre = OnGround(mask.CentreX(region.centre), mask.CentreZ(region.centre));
		const Colour colour = !isThreatGraph ? Colour::WHITE : chokeMap->IsOurSide(r) ? Colour::GREEN
				: chokeMap->IsEnemySide(r) ? Colour::RED : Colour::WHITE;
		AddCross(centre, colour, 96.f);
		for (int c : region.chokes) {
			const int cell = graph.GetChokes()[c].centre;
			AddLine(centre, OnGround(mask.CentreX(cell), mask.CentreZ(cell)), Colour::BLUE, 8.f);
		}
		std::string text = utils::int_to_string(region.id, "region %i") + utils::int_to_string(region.area, ", %i cells");
		if (isThreatGraph) {
			text += chokeMap->IsOurTerritory(r) ? ", our territory" : chokeMap->IsEnemyTerritory(r) ? ", enemy territory" : "";
		}
		AddLabel(centre, text);
	}
}

void CChokeOverlay::AddLine(const AIFloat3& from, const AIFloat3& to, Colour colour, float width)
{
	Figure* figure = circuit->GetDrawer()->GetFigure();
	auto it = groups.find(colour);
	const int groupId = figure->DrawLine(from, to, width, false, 0 /* stays */, (it != groups.end()) ? it->second : 0);
	if (it == groups.end()) {
		groups[colour] = groupId;
		figure->SetColor(groupId, ColourOf(static_cast<int>(colour)), 255);
	}
	delete figure;
}

void CChokeOverlay::AddCross(const AIFloat3& pos, Colour colour, float size)
{
	AddLine(OnGround(pos.x - size, pos.z - size), OnGround(pos.x + size, pos.z + size), colour, 12.f);
	AddLine(OnGround(pos.x - size, pos.z + size), OnGround(pos.x + size, pos.z - size), colour, 12.f);
}

void CChokeOverlay::AddLabel(const AIFloat3& pos, const std::string& text)
{
	if (isLabels) {
		circuit->GetDrawer()->AddPoint(pos, text.c_str());
		labels.push_back(pos);
	}
}

AIFloat3 CChokeOverlay::OnGround(float x, float z, float lift) const
{
	AIFloat3 pos(x, 0.f, z);
	CTerrainManager::CorrectPosition(pos);
	pos.y = std::max(circuit->GetMap()->GetElevationAt(pos.x, pos.z), 0.f) + lift;
	return pos;
}

} // namespace circuit

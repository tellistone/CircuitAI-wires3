/*
 * ChokeSides.cpp
 *
 *  Created on: Nov 18, 2026
 *      Author: Claude
 */

#include "terrain/ChokeOverlay.h"
#include "terrain/ChokeMap.h"
#include "module/Frontline.h"
#include "module/MilitaryManager.h"
#include "unit/ally/AllyUnit.h"
#include "unit/enemy/EnemyManager.h"
#include "unit/enemy/EnemyUnit.h"
#include "unit/CircuitDef.h"
#include "CircuitAI.h"
#include "util/Utils.h"

#include "Lua.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

namespace circuit {

using namespace springai;

#define RECENT_FRAMES	(FRAMES_PER_SEC * 180)  // an army counts in a region this long after it was seen there
#define WIDGET_PREFIX	"circuit_sides_"  // see util/LuaUI/Widgets/dbg_circuit_sides.lua

/*
 * Each region by its side of the lines, contested when it is between them, when both sides' armed units were there
 * in the last 3 minutes (enemy towers until seen dead), or when both sides have buildings there
 */
std::vector<CChokeOverlay::SRegionView> CChokeOverlay::MakeRegionViews() const
{
	const std::vector<CRegionGraph::SRegion>& regions = chokeMap->GetGraph().GetRegions();
	const int frame = circuit->GetLastFrame();
	std::vector<SRegionView> views(regions.size());
	std::vector<bool> isEnemyArmy(regions.size(), false);
	for (const auto& kv : circuit->GetEnemyManager()->GetEnemyUnits()) {
		const CEnemyUnit* enemy = kv.second;
		CCircuitDef* edef = enemy->GetCircuitDef();
		if (enemy->IsDead() || enemy->IsFake() || (edef == nullptr) || !edef->IsAttacker()
			|| (edef->IsMobile() && !enemy->IsRecentlySeen(frame, RECENT_FRAMES)))
		{
			continue;
		}
		const int region = chokeMap->GetRegionAt(enemy->GetPos());
		if (region < 0) {
			continue;
		}
		isEnemyArmy[region] = true;
		if (edef->IsMobile()) {
			views[region].threat += enemy->GetThreat();
		}
	}
	for (size_t r = 0; r < regions.size(); ++r) {
		SRegionView& view = views[r];
		auto it = ourArmySeen.find(regions[r].id);
		const bool isOurArmy = (it != ourArmySeen.end()) && (frame - it->second < RECENT_FRAMES);
		view.isArmies = isOurArmy && isEnemyArmy[r];
		view.isBuilt = chokeMap->IsOurTerritory(r) && chokeMap->IsEnemyTerritory(r);
		const CChokeMap::LineSide line = chokeMap->GetLineSide(r);
		view.isBetween = (line == CChokeMap::LineSide::BETWEEN);
		if (view.isBetween || view.isArmies || view.isBuilt) {
			view.side = Side::CONTESTED;
		} else if (line == CChokeMap::LineSide::OURS) {
			view.side = Side::OURS;
			view.isTerritory = chokeMap->IsOurTerritory(r);
		} else if (line == CChokeMap::LineSide::ENEMY) {
			view.side = Side::ENEMY;
			view.isTerritory = chokeMap->IsEnemyTerritory(r);
		}
	}
	return views;
}

/*
 * Our armed units' regions (towers too), every 2 s while the overlay is on: the other half of a contested army
 */
void CChokeOverlay::UpdatePresence()
{
	if (!chokeMap->IsReady()) {
		return;
	}
	const int frame = circuit->GetLastFrame();
	const std::vector<CRegionGraph::SRegion>& regions = chokeMap->GetGraph().GetRegions();
	for (const auto& kv : circuit->GetFriendlyUnits()) {
		CAllyUnit* unit = kv.second;
		if (!unit->GetCircuitDef()->IsAttacker()) {
			continue;
		}
		const int region = chokeMap->GetRegionAt(unit->GetPos(frame));
		if (region >= 0) {
			ourArmySeen[regions[region].id] = frame;
		}
	}
	for (auto it = ourArmySeen.begin(); it != ourArmySeen.end();) {
		it = (frame - it->second >= RECENT_FRAMES) ? ourArmySeen.erase(it) : std::next(it);
	}
}

/*
 * Region centres: side, why it's contested, enemy threat there. Enemy-line and enemy-held chokes: as the ranks
 * layer says them
 */
std::vector<std::pair<AIFloat3, std::string>> CChokeOverlay::MakeSideLabels(const std::vector<SRegionView>& views) const
{
	std::vector<std::pair<AIFloat3, std::string>> result;
	const CRegionGraph& graph = chokeMap->GetGraph();
	const SCellMask& mask = graph.GetMask();
	static const char* sideNames[] = {"unknown", "ours", "enemy", "contested"};
	for (size_t r = 0; r < views.size(); ++r) {
		const SRegionView& view = views[r];
		const CRegionGraph::SRegion& region = graph.GetRegions()[r];
		std::string text = utils::int_to_string(region.id, "region %i: ") + sideNames[static_cast<int>(view.side)];
		if (view.isTerritory) {
			text += ", territory";
		}
		if (view.side == Side::CONTESTED) {
			std::string why;
			for (const auto& reason : {std::make_pair(view.isBetween, "between the lines"),
					std::make_pair(view.isArmies, "both armies"), std::make_pair(view.isBuilt, "both built")})
			{
				if (reason.first) {
					why += (why.empty() ? "" : ", ") + std::string(reason.second);
				}
			}
			text += " (" + why + ")";
		}
		if (view.threat > 0.f) {
			text += utils::float_to_string(view.threat, ", enemy threat %.0f");
		}
		result.emplace_back(AIFloat3(mask.CentreX(region.centre), 0.f, mask.CentreZ(region.centre)), text);
	}
	for (const CFrontline::SRank& rank : circuit->GetMilitaryManager()->GetFrontline()->GetRanks()) {
		const int c = graph.FindChoke(rank.chokeId);
		if (c < 0) {
			continue;
		}
		const AIFloat3 centre = chokeMap->GetCellPos(graph.GetChokes()[c].centre);
		result.emplace_back(centre, std::string(rank.isHeld ? "enemy-held" : "enemy line")
				+ (rank.isUndefended ? ", undefended" : "") + utils::float_to_string(rank.defence, ": defence %.0f")
				+ utils::float_to_string(rank.confidence * 100.f, ", seen %.0f%%"));
	}
	return result;
}

/*
 * The widget draws the fills when the machine running the AI has it, never in games with human opponents (it may be
 * one of theirs); else lines, region crosses and arrows with figures
 */
void CChokeOverlay::DrawSides()
{
	const std::vector<SRegionView> views = MakeRegionViews();
	const bool isAllowed = !IsHumanIn(false);
	if (isAllowed) {
		const std::string answer = CallWidget("hello", "");
		if ((answer == "1") || (answer == "0")) {
			isGridSent &= (answer == "1");  // "0": it has no grid for us (just loaded)
			SendWidgetData(views);
			return;
		}
	}
	isWidgetDrawn = false;
	if (!isWidgetHinted) {
		isWidgetHinted = true;
		Say(isAllowed ? "sides: region fills need the dbg_circuit_sides widget on the machine running the AI"
				" (util/LuaUI/Widgets in the AI's repo); drawing lines and region crosses instead"
				: "sides: no widget in games with human opponents; drawing lines and region crosses instead");
	}
	DrawSidesWithFigures(views);
}

/*
 * One message each, prefixed circuit_sides_<verb>:<ally team>. Positions in whole elmos
 * - grid: width height cellSize, then (region index, count) runs over the cells row by row; -1 is a wall
 * - state: a letter per region index: u unknown, o ours, e enemy, c contested; capital in that side's territory
 * - lines, arrows, labels: a record per line, after the header line
 *   lines: kind (w forward, c current, e enemy, f front) x1 z1 x2 z2
 *   arrows: x1 z1 x2 z2 full half (threat of enemy mobiles it's the nearest line choke for, and of the others)
 *   labels: x z text
 */
void CChokeOverlay::SendWidgetData(const std::vector<SRegionView>& views)
{
	const CRegionGraph& graph = chokeMap->GetGraph();
	const SCellMask& mask = graph.GetMask();
	const std::vector<CRegionGraph::SChoke>& chokes = graph.GetChokes();
	auto put = [](std::ostringstream& out, float value) {
		out << ' ' << (int)std::lround(value);
	};

	if (!isGridSent) {
		std::ostringstream out;
		out << ' ' << mask.width << ' ' << mask.height << ' ' << mask.cellSize;
		int prev = graph.GetRegionAt(0), run = 0;
		for (int cell = 0; cell < mask.Size(); ++cell) {
			const int region = graph.GetRegionAt(cell);
			if (region != prev) {
				out << ' ' << prev << ' ' << run;
				prev = region;
				run = 0;
			}
			++run;
		}
		out << ' ' << prev << ' ' << run;
		CallWidget("grid", out.str());
		isGridSent = true;
	}

	std::string letters(views.size(), 'u');
	for (size_t r = 0; r < views.size(); ++r) {
		static const char sideLetters[] = {'u', 'o', 'e', 'c'};
		const char letter = sideLetters[static_cast<int>(views[r].side)];
		letters[r] = views[r].isTerritory ? (char)std::toupper(letter) : letter;
	}
	CallWidget("state", " " + letters);

	std::ostringstream lines;
	auto addLine = [&](const std::vector<int>& line, char kind) {
		for (int c : line) {
			AIFloat3 a, b;
			GetChokeEnds(graph, chokes[c], a, b);
			lines << '\n' << kind;
			put(lines, a.x); put(lines, a.z); put(lines, b.x); put(lines, b.z);
		}
	};
	addLine(chokeMap->GetForwardLine(), 'w');
	addLine(chokeMap->GetCurrentLine(), 'c');
	addLine(chokeMap->GetEnemyLine(), 'e');
	addLine(chokeMap->GetFrontLine(), 'f');
	CallWidget("lines", lines.str());

	std::ostringstream arrows;
	for (const CFrontline::SPress& press : circuit->GetMilitaryManager()->GetFrontline()->GetPresses()) {
		const int c = graph.FindChoke(press.chokeId);
		if (c < 0) {
			continue;
		}
		const AIFloat3 centre = chokeMap->GetCellPos(chokes[c].centre);
		arrows << '\n';
		put(arrows, press.from.x); put(arrows, press.from.z); put(arrows, centre.x); put(arrows, centre.z);
		put(arrows, press.full); put(arrows, press.half);
	}
	CallWidget("arrows", arrows.str());

	std::ostringstream labelText;
	if (isLabels) {
		for (const auto& label : MakeSideLabels(views)) {
			labelText << '\n';
			put(labelText, label.first.x); put(labelText, label.first.z);
			labelText << ' ' << label.second;
		}
	}
	CallWidget("labels", labelText.str());
	isWidgetDrawn = true;
}

/*
 * Lines blue (current), red (enemy), white (front), yellow (forward); a cross per region, blue ours, red the enemy's,
 * half of each contested, bigger in territory; orange arrows from enemy armies to the line chokes they press, thick
 * to the nearest
 */
void CChokeOverlay::DrawSidesWithFigures(const std::vector<SRegionView>& views)
{
	const CRegionGraph& graph = chokeMap->GetGraph();
	const SCellMask& mask = graph.GetMask();
	const std::vector<CRegionGraph::SChoke>& chokes = graph.GetChokes();
	auto drawLine = [&](const std::vector<int>& line, Colour colour, float width) {
		for (int c : line) {
			AIFloat3 a, b;
			GetChokeEnds(graph, chokes[c], a, b);
			AddLine(OnGround(a.x, a.z), OnGround(b.x, b.z), colour, width);
		}
	};
	drawLine(chokeMap->GetForwardLine(), Colour::YELLOW, 16.f);
	drawLine(chokeMap->GetCurrentLine(), Colour::BLUE, 32.f);
	drawLine(chokeMap->GetEnemyLine(), Colour::RED, 32.f);
	drawLine(chokeMap->GetFrontLine(), Colour::WHITE, 48.f);

	for (size_t r = 0; r < views.size(); ++r) {
		const SRegionView& view = views[r];
		const int cell = graph.GetRegions()[r].centre;
		const float x = mask.CentreX(cell), z = mask.CentreZ(cell);
		const float size = view.isTerritory ? 160.f : 96.f;
		switch (view.side) {
			case Side::OURS: AddCross(AIFloat3(x, 0.f, z), Colour::BLUE, size); break;
			case Side::ENEMY: AddCross(AIFloat3(x, 0.f, z), Colour::RED, size); break;
			case Side::CONTESTED: {
				AddLine(OnGround(x - size, z - size), OnGround(x + size, z + size), Colour::BLUE, 12.f);
				AddLine(OnGround(x - size, z + size), OnGround(x + size, z - size), Colour::RED, 12.f);
			} break;
			default: break;
		}
	}

	for (const CFrontline::SPress& press : circuit->GetMilitaryManager()->GetFrontline()->GetPresses()) {
		const int c = graph.FindChoke(press.chokeId);
		if (c < 0) {
			continue;
		}
		const AIFloat3 centre = chokeMap->GetCellPos(chokes[c].centre);
		AddLine(OnGround(press.from.x, press.from.z), OnGround(centre.x, centre.z), Colour::ORANGE,
				(press.full > 0.f) ? 24.f : 8.f, true);
	}

	for (const auto& label : MakeSideLabels(views)) {
		AddLabel(OnGround(label.first.x, label.first.z), label.second);
	}
}

std::string CChokeOverlay::CallWidget(const std::string& verb, const std::string& body) const
{
	const std::string message = WIDGET_PREFIX + verb + utils::int_to_string(circuit->GetAllyTeamId(), ":%i") + body;
	return circuit->GetLua()->CallUI(message.c_str(), message.size());
}

void CChokeOverlay::ClearWidget()
{
	if (isWidgetDrawn) {
		isWidgetDrawn = false;
		CallWidget("clear", "");
	}
}

} // namespace circuit

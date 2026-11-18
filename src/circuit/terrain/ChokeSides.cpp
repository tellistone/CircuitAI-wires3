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

#define ARMY_SEEN		(FRAMES_PER_SEC * 60)  // an enemy mobile counts where it was seen this long ago
#define ARMY_FLOOR		300.f  // metal: a side's mobiles in a region make it an army from this much
#define WIDGET_PREFIX	"circuit_sides_"  // see util/LuaUI/Widgets/dbg_circuit_sides.lua

/*
 * Each region by its side of the lines, contested when it is between them, when both sides have an army there
 * (ARMY_FLOOR of armed mobiles each: ours now, the enemy's seen in the last minute), or when both sides have
 * buildings there
 */
std::vector<CChokeOverlay::SRegionView> CChokeOverlay::MakeRegionViews() const
{
	const std::vector<CRegionGraph::SRegion>& regions = chokeMap->GetGraph().GetRegions();
	const int frame = circuit->GetLastFrame();
	std::vector<SRegionView> views(regions.size());
	std::vector<float> ourArmy(regions.size(), 0.f), enemyArmy(regions.size(), 0.f);
	for (const auto& kv : circuit->GetFriendlyUnits()) {
		CAllyUnit* unit = kv.second;
		const CCircuitDef* cdef = unit->GetCircuitDef();
		if (cdef->IsAttacker() && cdef->IsMobile()) {
			const int region = chokeMap->GetRegionAt(unit->GetPos(frame));
			if (region >= 0) {
				ourArmy[region] += cdef->GetCostM();
			}
		}
	}
	for (const auto& kv : circuit->GetEnemyManager()->GetEnemyUnits()) {
		const CEnemyUnit* enemy = kv.second;
		const CCircuitDef* edef = enemy->GetCircuitDef();
		if (enemy->IsDead() || enemy->IsFake() || (edef == nullptr) || !edef->IsAttacker() || !edef->IsMobile()
			|| !enemy->IsRecentlySeen(frame, ARMY_SEEN))
		{
			continue;
		}
		const int region = chokeMap->GetRegionAt(enemy->GetPos());
		if (region >= 0) {
			enemyArmy[region] += enemy->GetCost();
			views[region].threat += enemy->GetThreat();
		}
	}
	for (size_t r = 0; r < regions.size(); ++r) {
		SRegionView& view = views[r];
		view.isArmies = (ourArmy[r] >= ARMY_FLOOR) && (enemyArmy[r] >= ARMY_FLOOR);
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
 * Region centres: side, why it's contested, enemy threat there. Ranked chokes: as the ranks layer says them. Our
 * current line's chokes, a little south: what they want against the enemy outside and what our towers give
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
	const CFrontline* frontline = circuit->GetMilitaryManager()->GetFrontline();
	for (const CFrontline::SRank& rank : frontline->GetRanks()) {
		const int c = graph.FindChoke(rank.chokeId);
		if (c < 0) {
			continue;
		}
		const AIFloat3 centre = chokeMap->GetCellPos(graph.GetChokes()[c].centre);
		result.emplace_back(centre, std::string(rank.isHeld ? "enemy-held" : "enemy line")
				+ (rank.isUndefended ? ", undefended" : "") + utils::float_to_string(rank.defence, ": defence %.0f")
				+ utils::float_to_string(rank.confidence * 100.f, ", seen %.0f%%"));
	}
	for (const CFrontline::SFront& f : frontline->GetFront()) {
		const int c = graph.FindChoke(f.chokeId);
		if (c < 0) {
			continue;
		}
		const AIFloat3 centre = chokeMap->GetCellPos(graph.GetChokes()[c].centre);
		result.emplace_back(AIFloat3(centre.x, 0.f, centre.z + 96.f), std::string("our line")
				+ ((f.GetShortfall() > 0.f) ? ", short" : "") + utils::float_to_string(f.wanted, ": wants %.0f")
				+ utils::float_to_string(f.towers, ", towers %.0f"));
	}
	return result;
}

/*
 * The widget draws the layer when the machine running the AI has it, never in games with human opponents (it may be
 * one of theirs); else the lines layer stands in
 */
void CChokeOverlay::DrawSides()
{
	const bool isAllowed = !IsHumanIn(false);
	if (isAllowed) {
		const std::string answer = CallWidget("hello", "");
		if ((answer == "1") || (answer == "0")) {
			isGridSent &= (answer == "1");  // "0": it has no grid for us (just loaded)
			SendWidgetData(MakeRegionViews());
			return;
		}
	}
	isWidgetDrawn = false;
	if (!isWidgetHinted) {
		isWidgetHinted = true;
		Say(isAllowed ? "sides needs the dbg_circuit_sides widget on the machine running the AI"
				" (util/LuaUI/Widgets in the AI's repo); showing the lines layer instead"
				: "sides has no widget in games with human opponents; showing the lines layer instead");
	}
	DrawLines();
}

/*
 * One message each, prefixed circuit_sides_<verb>:<ally team>. Positions in whole elmos
 * - grid: width height cellSize, then (region index, count) runs over the cells row by row; -1 is a wall
 * - state: a letter per region index: u unknown, o ours, e enemy, c contested; capital in that side's territory
 * - lines, labels: a record per line, after the header line
 *   lines: kind x1 z1 x2 z2. Kinds: w forward line; f front line; enemy line chokes by rank: e defended,
 *   u undefended, g not seen enough; h enemy-held off the enemy line; current line: c covered, s short
 *   labels: x z text
 */
void CChokeOverlay::SendWidgetData(const std::vector<SRegionView>& views)
{
	const CRegionGraph& graph = chokeMap->GetGraph();
	const SCellMask& mask = graph.GetMask();
	const std::vector<CRegionGraph::SChoke>& chokes = graph.GetChokes();
	const CFrontline* frontline = circuit->GetMilitaryManager()->GetFrontline();
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
	auto addChoke = [&](int c, char kind) {
		AIFloat3 a, b;
		GetChokeEnds(graph, chokes[c], a, b);
		lines << '\n' << kind;
		put(lines, a.x); put(lines, a.z); put(lines, b.x); put(lines, b.z);
	};
	for (int c : chokeMap->GetForwardLine()) {
		addChoke(c, 'w');
	}
	for (int c : chokeMap->GetFrontLine()) {
		addChoke(c, 'f');
	}
	std::vector<bool> isRanked(chokes.size(), false);
	for (const CFrontline::SRank& rank : frontline->GetRanks()) {
		const int c = graph.FindChoke(rank.chokeId);
		if (c >= 0) {
			isRanked[c] = true;
			const bool isOnLine = std::binary_search(chokeMap->GetEnemyLine().begin(), chokeMap->GetEnemyLine().end(), c);
			addChoke(c, !isOnLine ? 'h' : rank.isUndefended ? 'u' : (rank.defence > 0.f) ? 'e' : 'g');
		}
	}
	for (int c : chokeMap->GetEnemyLine()) {
		if (!isRanked[c]) {
			addChoke(c, 'g');  // not ranked since the lines moved
		}
	}
	for (int c : chokeMap->GetCurrentLine()) {
		const auto it = std::find_if(frontline->GetFront().begin(), frontline->GetFront().end(),
				[&](const CFrontline::SFront& f) { return f.chokeId == chokes[c].id; });
		addChoke(c, ((it != frontline->GetFront().end()) && (it->GetShortfall() > 0.f)) ? 's' : 'c');
	}
	CallWidget("lines", lines.str());

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

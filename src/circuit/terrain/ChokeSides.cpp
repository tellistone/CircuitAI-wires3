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
#include "map/InfluenceMap.h"
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
#define CONTROL_RATIO	1.5f  // where both sides are, one controls with this many times the other's strength
#define WIDGET_PREFIX	"circuit_sides_"  // see util/LuaUI/Widgets/dbg_circuit_sides.lua

static std::string Amount(float value)
{
	return (value >= 1000.f) ? utils::float_to_string(value / 1000.f, "%.1fk") : utils::float_to_string(value, "%.0f");
}

/*
 * Nobody's where neither side is; one side's where only it is; where both are, the side with CONTROL_RATIO times the
 * other's strength, else contested
 */
CChokeOverlay::Side CChokeOverlay::ControlOf(bool isOurs, bool isEnemy, float power, float threat)
{
	if (isOurs != isEnemy) {
		return isOurs ? Side::OURS : Side::ENEMY;
	}
	if (!isOurs) {
		return Side::NONE;
	}
	if ((power > 0.f) && (power >= CONTROL_RATIO * threat)) {
		return Side::OURS;
	}
	if ((threat > 0.f) && (threat >= CONTROL_RATIO * power)) {
		return Side::ENEMY;
	}
	return Side::CONTESTED;
}

/*
 * A region by the units in it, aircraft aside: ours where they are now, enemy buildings until seen dead, enemy
 * mobiles where they were seen in the last minute. Strength is the armed ones' power (ours) and threat (the enemy's)
 */
std::vector<CChokeOverlay::SRegionView> CChokeOverlay::MakeRegionViews() const
{
	const size_t count = chokeMap->GetGraph().GetRegions().size();
	const int frame = circuit->GetLastFrame();
	std::vector<SRegionView> views(count);
	std::vector<bool> isOurs(count, false), isEnemy(count, false);
	for (const auto& kv : circuit->GetFriendlyUnits()) {
		CAllyUnit* unit = kv.second;
		const CCircuitDef* cdef = unit->GetCircuitDef();
		if (cdef->IsAbleToFly()) {
			continue;
		}
		const int region = chokeMap->GetRegionAt(unit->GetPos(frame));
		if (region >= 0) {
			isOurs[region] = true;
			views[region].power += cdef->IsAttacker() ? cdef->GetPower() : 0.f;
		}
	}
	for (const auto& kv : circuit->GetEnemyManager()->GetEnemyUnits()) {
		const CEnemyUnit* enemy = kv.second;
		const CCircuitDef* edef = enemy->GetCircuitDef();
		if (enemy->IsDead() || enemy->IsFake() || (edef == nullptr) || edef->IsAbleToFly()
			|| (edef->IsMobile() && !enemy->IsRecentlySeen(frame, ARMY_SEEN)))
		{
			continue;
		}
		const int region = chokeMap->GetRegionAt(enemy->GetPos());
		if (region >= 0) {
			isEnemy[region] = true;
			views[region].threat += edef->IsAttacker() ? enemy->GetThreat() : 0.f;
		}
	}
	for (size_t r = 0; r < count; ++r) {
		SRegionView& view = views[r];
		view.side = ControlOf(isOurs[r], isEnemy[r], view.power, view.threat);
		view.isTerritory = ((view.side == Side::OURS) && chokeMap->IsOurTerritory(r))
				|| ((view.side == Side::ENEMY) && chokeMap->IsEnemyTerritory(r));
	}
	return views;
}

/*
 * A choke by whose threat reaches across it: our and the enemy's influence (CInfluenceMap: armed units' power or
 * threat, fading to nothing at their range) averaged along its line. Defended by the towers whose range reaches its
 * line, as CFrontline counts them: ours and allies', and the enemy's known ones until seen dead
 */
std::vector<CChokeOverlay::SChokeView> CChokeOverlay::MakeChokeViews() const
{
	const CRegionGraph& graph = chokeMap->GetGraph();
	const std::vector<CRegionGraph::SChoke>& chokes = graph.GetChokes();
	const SCellMask& mask = graph.GetMask();
	const CInfluenceMap* inflMap = circuit->GetInflMap();
	const int frame = circuit->GetLastFrame();
	struct STower {
		AIFloat3 pos;
		float range, strength;
	};
	std::vector<STower> ourTowers, enemyTowers;
	for (const auto& kv : circuit->GetFriendlyUnits()) {
		const CCircuitDef* cdef = kv.second->GetCircuitDef();
		if (!cdef->IsMobile() && cdef->IsAttacker()) {
			ourTowers.push_back({kv.second->GetPos(frame), cdef->GetMaxRange(), cdef->GetPower()});
		}
	}
	for (const auto& kv : circuit->GetEnemyManager()->GetEnemyUnits()) {
		const CEnemyUnit* enemy = kv.second;
		const CCircuitDef* edef = enemy->GetCircuitDef();
		if (!enemy->IsDead() && !enemy->IsFake() && (edef != nullptr) && !edef->IsMobile() && edef->IsAttacker()) {
			enemyTowers.push_back({enemy->GetPos(), edef->GetMaxRange(), enemy->GetThreat()});
		}
	}
	auto towersAt = [&graph, this](const CRegionGraph::SChoke& choke, const std::vector<STower>& towers) {
		const AIFloat3 centre = chokeMap->GetCellPos(choke.centre);
		float sum = 0.f;
		for (const STower& t : towers) {
			if ((centre.distance2D(t.pos) <= t.range + choke.width * 0.5f + 64.f)
				&& (graph.GetLineDist(choke, t.pos.x, t.pos.z) <= t.range))
			{
				sum += t.strength;
			}
		}
		return sum;
	};

	std::vector<SChokeView> views(chokes.size());
	for (size_t c = 0; c < views.size(); ++c) {
		const std::vector<int>& line = chokes[c].line;
		SChokeView& view = views[c];
		view.ourTowers = towersAt(chokes[c], ourTowers);
		view.enemyTowers = towersAt(chokes[c], enemyTowers);
		for (int cell : line) {
			const AIFloat3 pos(mask.CentreX(cell), 0.f, mask.CentreZ(cell));
			view.power += std::max(inflMap->GetAllyInflAt(pos), 0.f);
			view.threat += std::max(inflMap->GetEnemyInflAt(pos), 0.f);
		}
		if (!line.empty()) {
			view.power /= line.size();
			view.threat /= line.size();
		}
		view.side = ControlOf(view.power > INFL_EPS, view.threat > INFL_EPS, view.power, view.threat);
	}
	return views;
}

/*
 * Controlled regions at their centres; chokes with control, a rank or a place on our line at theirs. "|" breaks a
 * label's lines
 */
std::vector<std::pair<AIFloat3, std::string>> CChokeOverlay::MakeSideLabels(const std::vector<SRegionView>& regionViews,
		const std::vector<SChokeView>& chokeViews) const
{
	std::vector<std::pair<AIFloat3, std::string>> result;
	const CRegionGraph& graph = chokeMap->GetGraph();
	const SCellMask& mask = graph.GetMask();
	static const char* sideNames[] = {"no control", "ours", "enemy", "contested"};
	auto describe = [](const SControl& view) {
		std::string text = sideNames[static_cast<int>(view.side)];
		std::string strength;
		if (view.power > 0.f) {
			strength = "power " + Amount(view.power);
		}
		if (view.threat > 0.f) {
			strength += (strength.empty() ? "" : ", ") + std::string("threat ") + Amount(view.threat);
		}
		return strength.empty() ? text : (text + "|" + strength);
	};
	for (size_t r = 0; r < regionViews.size(); ++r) {
		const SRegionView& view = regionViews[r];
		if (view.side == Side::NONE) {
			continue;
		}
		const CRegionGraph::SRegion& region = graph.GetRegions()[r];
		std::string text = describe(view);
		if (view.isTerritory) {
			text.insert(text.find('|') == std::string::npos ? text.size() : text.find('|'), ", territory");
		}
		result.emplace_back(AIFloat3(mask.CentreX(region.centre), 0.f, mask.CentreZ(region.centre)),
				utils::int_to_string(region.id, "region %i: ") + text);
	}

	const CFrontline* frontline = circuit->GetMilitaryManager()->GetFrontline();
	const std::vector<CRegionGraph::SChoke>& chokes = graph.GetChokes();
	for (size_t c = 0; c < chokes.size(); ++c) {
		const SChokeView& view = chokeViews[c];
		std::string text = utils::int_to_string(chokes[c].id, "choke %i: ") + describe(view);
		bool isNamed = (view.side != Side::NONE);
		if ((view.ourTowers > 0.f) || (view.enemyTowers > 0.f)) {
			text += "|defended by " + std::string((view.ourTowers > 0.f) ? "our towers " + Amount(view.ourTowers) : "")
					+ ((view.ourTowers > 0.f) && (view.enemyTowers > 0.f) ? ", " : "")
					+ ((view.enemyTowers > 0.f) ? "enemy towers " + Amount(view.enemyTowers) : "");
			isNamed = true;
		} else {
			text += "|undefended";
		}
		for (const CFrontline::SRank& rank : frontline->GetRanks()) {
			if (rank.chokeId == chokes[c].id) {
				text += std::string(rank.isHeld ? "|enemy-held" : "|enemy line") + (rank.isUndefended ? ", undefended" : "")
						+ utils::float_to_string(rank.defence, ": defence %.0f")
						+ utils::float_to_string(rank.confidence * 100.f, ", seen %.0f%%");
				isNamed = true;
			}
		}
		for (const CFrontline::SFront& f : frontline->GetFront()) {
			if (f.chokeId == chokes[c].id) {
				text += std::string("|our line") + ((f.GetShortfall() > 0.f) ? ", short" : "")
						+ utils::float_to_string(f.wanted, ": wants %.0f") + utils::float_to_string(f.towers, ", towers %.0f");
				isNamed = true;
			}
		}
		if (isNamed) {
			result.emplace_back(chokeMap->GetCellPos(chokes[c].centre), text);
		}
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
			SendWidgetData();
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
 * - state: a letter per region index: u nobody's, o ours, e the enemy's, c contested; capital in that side's
 *   territory
 * - control, lines, labels: a record per line, after the header line
 *   control: who defended x1 z1 x2 z2, a choke's line end to end. Who controls it: n nobody, o ours, e the enemy,
 *   c contested. Whose towers reach its line: n nobody's, o ours, e the enemy's, b both sides'
 *   lines: kind x1 z1 x2 z2. Kinds: w forward line; f front line; enemy line chokes by rank: e defended,
 *   u undefended, g not seen enough; h enemy-held off the enemy line; current line: c covered, s short
 *   labels: x z text, "|" between its lines
 */
void CChokeOverlay::SendWidgetData()
{
	const CRegionGraph& graph = chokeMap->GetGraph();
	const SCellMask& mask = graph.GetMask();
	const std::vector<CRegionGraph::SChoke>& chokes = graph.GetChokes();
	const CFrontline* frontline = circuit->GetMilitaryManager()->GetFrontline();
	const std::vector<SRegionView> regionViews = MakeRegionViews();
	const std::vector<SChokeView> chokeViews = MakeChokeViews();
	static const char sideLetters[] = {'u', 'o', 'e', 'c'};
	auto put = [](std::ostringstream& out, float value) {
		out << ' ' << (int)std::lround(value);
	};
	auto putEnds = [&](std::ostringstream& out, int c) {
		AIFloat3 a, b;
		GetChokeEnds(graph, chokes[c], a, b);
		put(out, a.x); put(out, a.z); put(out, b.x); put(out, b.z);
	};
	auto putChoke = [&](std::ostringstream& out, int c, char kind) {
		out << '\n' << kind;
		putEnds(out, c);
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

	std::string letters(regionViews.size(), 'u');
	for (size_t r = 0; r < regionViews.size(); ++r) {
		const char letter = sideLetters[static_cast<int>(regionViews[r].side)];
		letters[r] = regionViews[r].isTerritory ? (char)std::toupper(letter) : letter;
	}
	CallWidget("state", " " + letters);

	std::ostringstream control;
	for (size_t c = 0; c < chokes.size(); ++c) {
		static const char chokeLetters[] = {'n', 'o', 'e', 'c'};
		const SChokeView& view = chokeViews[c];
		control << '\n' << chokeLetters[static_cast<int>(view.side)] << ' '
				<< (((view.ourTowers > 0.f) && (view.enemyTowers > 0.f)) ? 'b'
				: (view.ourTowers > 0.f) ? 'o' : (view.enemyTowers > 0.f) ? 'e' : 'n');
		putEnds(control, c);
	}
	CallWidget("control", control.str());

	std::ostringstream lines;
	for (int c : chokeMap->GetForwardLine()) {
		putChoke(lines, c, 'w');
	}
	for (int c : chokeMap->GetFrontLine()) {
		putChoke(lines, c, 'f');
	}
	std::vector<bool> isRanked(chokes.size(), false);
	for (const CFrontline::SRank& rank : frontline->GetRanks()) {
		const int c = graph.FindChoke(rank.chokeId);
		if (c >= 0) {
			isRanked[c] = true;
			const bool isOnLine = std::binary_search(chokeMap->GetEnemyLine().begin(), chokeMap->GetEnemyLine().end(), c);
			putChoke(lines, c, !isOnLine ? 'h' : rank.isUndefended ? 'u' : (rank.defence > 0.f) ? 'e' : 'g');
		}
	}
	for (int c : chokeMap->GetEnemyLine()) {
		if (!isRanked[c]) {
			putChoke(lines, c, 'g');  // not ranked since the lines moved
		}
	}
	for (int c : chokeMap->GetCurrentLine()) {
		const auto it = std::find_if(frontline->GetFront().begin(), frontline->GetFront().end(),
				[&](const CFrontline::SFront& f) { return f.chokeId == chokes[c].id; });
		putChoke(lines, c, ((it != frontline->GetFront().end()) && (it->GetShortfall() > 0.f)) ? 's' : 'c');
	}
	CallWidget("lines", lines.str());

	std::ostringstream labelText;
	if (isLabels) {
		for (const auto& label : MakeSideLabels(regionViews, chokeViews)) {
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

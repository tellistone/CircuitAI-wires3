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
#define OWN_RATIO		1.5f  // where both sides have threat, one owns with this many times the other's
#define WIDGET_PREFIX	"circuit_sides_"  // see util/LuaUI/Widgets/dbg_circuit_sides.lua

/*
 * Nobody's where neither side has anything; one side's where only it has; where both have, the side with OWN_RATIO
 * times the other's threat, else contested (both sides' eco and no guns is contested too)
 */
CChokeOverlay::Side CChokeOverlay::OwnerOf(const SStrength& ours, const SStrength& enemy)
{
	const bool isOurs = ours.IsAny(), isEnemy = enemy.IsAny();
	if (isOurs != isEnemy) {
		return isOurs ? Side::OURS : Side::ENEMY;
	}
	if (!isOurs) {
		return Side::NONE;
	}
	const float power = ours.Threat(), threat = enemy.Threat();
	if ((power > 0.f) && (power >= OWN_RATIO * threat)) {
		return Side::OURS;
	}
	if ((threat > 0.f) && (threat >= OWN_RATIO * power)) {
		return Side::ENEMY;
	}
	return Side::CONTESTED;
}

/*
 * The enemies the views count, all the AI knows of them: buildings until seen dead, armed mobiles seen (in sight or on
 * radar) in the last ARMY_SEEN, at the last place seen; aircraft and unidentified radar blips aside
 */
static bool IsCountedEnemy(const CEnemyUnit* enemy, int frame)
{
	const CCircuitDef* edef = enemy->GetCircuitDef();
	return !enemy->IsDead() && !enemy->IsFake() && (edef != nullptr) && !edef->IsAbleToFly()
			&& (!edef->IsMobile() || (edef->IsAttacker() && enemy->IsRecentlySeen(frame, ARMY_SEEN)));
}

/*
 * Both sides' units, aircraft aside: ours where they are now, enemy buildings until seen dead, enemy mobiles where
 * they were seen in the last minute. Armed ones bring threat (ours: power, as the influence map counts them); unarmed
 * buildings bring eco, their metal cost; constructors bring nothing.
 * - A region: the units in it.
 * - A choke: armed buildings whose range reaches its line, armed mobiles within their range or a main-tower range of
 *   it (whichever is longer: they walk), unarmed buildings within a main-tower range of it.
 */
void CChokeOverlay::MakeViews(std::vector<SView>& regionViews, std::vector<SView>& chokeViews) const
{
	struct SUnit {
		AIFloat3 pos;
		bool isMobile;
		float range, threat, eco;
	};
	const int frame = circuit->GetLastFrame();
	std::vector<SUnit> ours, enemies;
	for (const auto& kv : circuit->GetFriendlyUnits()) {
		const CCircuitDef* cdef = kv.second->GetCircuitDef();
		if (cdef->IsAbleToFly() || (cdef->IsMobile() && !cdef->IsAttacker())) {
			continue;
		}
		const bool isArmed = cdef->IsAttacker();
		ours.push_back({kv.second->GetPos(frame), cdef->IsMobile(), cdef->GetMaxRange(),
				isArmed ? cdef->GetPower() : 0.f, isArmed ? 0.f : cdef->GetCostM()});
	}
	for (const auto& kv : circuit->GetEnemyManager()->GetEnemyUnits()) {
		const CEnemyUnit* enemy = kv.second;
		if (!IsCountedEnemy(enemy, frame)) {
			continue;
		}
		const CCircuitDef* edef = enemy->GetCircuitDef();
		const bool isArmed = edef->IsAttacker();
		enemies.push_back({enemy->GetPos(), edef->IsMobile(), edef->GetMaxRange(),
				isArmed ? enemy->GetThreat() : 0.f, isArmed ? 0.f : enemy->GetCost()});
	}
	auto add = [](SStrength& to, const SUnit& u) {
		(u.isMobile ? to.mobile : (u.threat > 0.f) ? to.statics : to.eco) += (u.threat > 0.f) ? u.threat : u.eco;
	};

	const CRegionGraph& graph = chokeMap->GetGraph();
	regionViews.assign(graph.GetRegions().size(), SView());
	for (const SUnit& u : ours) {
		const int region = chokeMap->GetRegionAt(u.pos);
		if (region >= 0) {
			add(regionViews[region].ours, u);
		}
	}
	for (const SUnit& u : enemies) {
		const int region = chokeMap->GetRegionAt(u.pos);
		if (region >= 0) {
			add(regionViews[region].enemy, u);
		}
	}
	for (size_t r = 0; r < regionViews.size(); ++r) {
		SView& view = regionViews[r];
		view.side = OwnerOf(view.ours, view.enemy);
		view.isTerritory = ((view.side == Side::OURS) && chokeMap->IsOurTerritory(r))
				|| ((view.side == Side::ENEMY) && chokeMap->IsEnemyTerritory(r));
	}

	const float mainRange = chokeMap->GetMainRange();
	const std::vector<CRegionGraph::SChoke>& chokes = graph.GetChokes();
	chokeViews.assign(chokes.size(), SView());
	auto addReaching = [&](const CRegionGraph::SChoke& choke, const AIFloat3& centre, const std::vector<SUnit>& units,
			SStrength& to) {
		for (const SUnit& u : units) {
			const float reach = (u.threat <= 0.f) ? mainRange : u.isMobile ? std::max(u.range, mainRange) : u.range;
			if ((centre.distance2D(u.pos) <= reach + choke.width * 0.5f + 64.f)
				&& (graph.GetLineDist(choke, u.pos.x, u.pos.z) <= reach))
			{
				add(to, u);
			}
		}
	};
	for (size_t c = 0; c < chokes.size(); ++c) {
		const AIFloat3 centre = chokeMap->GetCellPos(chokes[c].centre);
		SView& view = chokeViews[c];
		addReaching(chokes[c], centre, ours, view.ours);
		addReaching(chokes[c], centre, enemies, view.enemy);
		view.side = OwnerOf(view.ours, view.enemy);
	}
}

/*
 * A record per enemy the views count: kind x z. Kinds: m an armed mobile, s an armed building, e an unarmed one;
 * capital while in sight or on radar now, else where it was last seen
 */
std::string CChokeOverlay::MakeKnownEnemies() const
{
	const int frame = circuit->GetLastFrame();
	std::ostringstream out;
	for (const auto& kv : circuit->GetEnemyManager()->GetEnemyUnits()) {
		const CEnemyUnit* enemy = kv.second;
		if (!IsCountedEnemy(enemy, frame)) {
			continue;
		}
		const CCircuitDef* edef = enemy->GetCircuitDef();
		const char kind = edef->IsMobile() ? 'm' : edef->IsAttacker() ? 's' : 'e';
		const AIFloat3& pos = enemy->GetPos();
		out << '\n' << (enemy->IsInRadarOrLOS() ? (char)std::toupper(kind) : kind) << ' ' << (int)std::lround(pos.x)
				<< ' ' << (int)std::lround(pos.z);
	}
	return out.str();
}

/*
 * A record per region and per choke: key x z owner, the enemy's mobile, static and eco, ours the same, then a title
 * and, for chokes, what the lines say of them, "|" between lines
 */
std::string CChokeOverlay::MakeInfo(const std::vector<SView>& regionViews, const std::vector<SView>& chokeViews) const
{
	const CRegionGraph& graph = chokeMap->GetGraph();
	const SCellMask& mask = graph.GetMask();
	const CFrontline* frontline = circuit->GetMilitaryManager()->GetFrontline();
	static const char ownerLetters[] = {'n', 'o', 'e', 'c'};
	std::ostringstream out;
	out.setf(std::ios::fixed);
	out.precision(1);
	auto put = [&out](char kind, int index, float x, float z, const SView& view, const std::string& title) {
		out << '\n' << kind << index << ' ' << (int)std::lround(x) << ' ' << (int)std::lround(z) << ' '
				<< ownerLetters[static_cast<int>(view.side)]
				<< ' ' << view.enemy.mobile << ' ' << view.enemy.statics << ' ' << view.enemy.eco
				<< ' ' << view.ours.mobile << ' ' << view.ours.statics << ' ' << view.ours.eco << ' ' << title;
	};
	for (size_t r = 0; r < regionViews.size(); ++r) {
		const CRegionGraph::SRegion& region = graph.GetRegions()[r];
		const SView& view = regionViews[r];
		put('r', r, mask.CentreX(region.centre), mask.CentreZ(region.centre), view, utils::int_to_string(region.id, "Region %i")
				+ (!view.isTerritory ? "" : (view.side == Side::OURS) ? "|our territory" : "|enemy territory"));
	}
	const std::vector<CRegionGraph::SChoke>& chokes = graph.GetChokes();
	for (size_t c = 0; c < chokes.size(); ++c) {
		std::string title = utils::int_to_string(chokes[c].id, "Choke %i");
		for (const CFrontline::SRank& rank : frontline->GetRanks()) {
			if (rank.chokeId == chokes[c].id) {
				title += std::string(rank.isHeld ? "|enemy-held" : "|enemy line") + (rank.isUndefended ? ", undefended" : "")
						+ utils::float_to_string(rank.defence, ": defence %.0f")
						+ utils::float_to_string(rank.confidence * 100.f, ", seen %.0f%%");
			}
		}
		for (const CFrontline::SFront& f : frontline->GetFront()) {
			if (f.chokeId == chokes[c].id) {
				title += std::string("|our line") + ((f.GetShortfall() > 0.f) ? ", short" : "")
						+ utils::float_to_string(f.wanted, ": wants %.0f") + utils::float_to_string(f.towers, ", towers %.0f");
			}
		}
		const AIFloat3 centre = chokeMap->GetCellPos(chokes[c].centre);
		put('c', c, centre.x, centre.z, chokeViews[c], title);
	}
	return out.str();
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
 * - control, lines, info, known: a record per line, after the header line
 *   control: who defended x1 z1 x2 z2, a choke's line end to end. Who controls it: n nobody, o ours, e the enemy,
 *   c contested. Whose towers reach its line: n nobody's, o ours, e the enemy's, b both sides'
 *   lines: kind x1 z1 x2 z2. Kinds: w forward line; f front line; enemy line chokes by rank: e defended,
 *   u undefended, g not seen enough; h enemy-held off the enemy line; current line: c covered, s short
 *   info: see MakeInfo
 *   known: see MakeKnownEnemies
 * - status: " cheating" when the AI's LOS cheating option is on (it knows every enemy unit), else empty
 */
void CChokeOverlay::SendWidgetData()
{
	const CRegionGraph& graph = chokeMap->GetGraph();
	const SCellMask& mask = graph.GetMask();
	const std::vector<CRegionGraph::SChoke>& chokes = graph.GetChokes();
	const CFrontline* frontline = circuit->GetMilitaryManager()->GetFrontline();
	std::vector<SView> regionViews, chokeViews;
	MakeViews(regionViews, chokeViews);
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
		const SView& view = chokeViews[c];
		const bool isOurs = (view.ours.statics > 0.f), isEnemy = (view.enemy.statics > 0.f);
		control << '\n' << chokeLetters[static_cast<int>(view.side)] << ' '
				<< ((isOurs && isEnemy) ? 'b' : isOurs ? 'o' : isEnemy ? 'e' : 'n');
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

	CallWidget("info", isLabels ? MakeInfo(regionViews, chokeViews) : "");
	CallWidget("known", MakeKnownEnemies());
	CallWidget("status", circuit->IsCheating() ? " cheating" : "");
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

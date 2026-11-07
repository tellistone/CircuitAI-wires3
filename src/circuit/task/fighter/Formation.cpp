/*
 * Formation.cpp
 *
 *  Created on: Nov 7, 2026
 *      Author: Claude
 */

#include "task/fighter/Formation.h"
#include "task/fighter/SquadTask.h"
#include "module/MilitaryManager.h"
#include "terrain/TerrainManager.h"
#include "terrain/path/PathFinder.h"
#include "unit/action/TravelAction.h"
#include "unit/CircuitUnit.h"
#include "CircuitAI.h"
#include "util/Utils.h"

#include <algorithm>
#include <cmath>
#include <tuple>

namespace circuit {

using namespace springai;

namespace {
constexpr int SOLVE_FRAMES = FRAMES_PER_SEC / 2;  // lay the line out at most this often
constexpr int WIDEN_FRAMES = FRAMES_PER_SEC * 3;  // a wider way must last this long before the line widens
constexpr int STRAGGLE_FRAMES = FRAMES_PER_SEC * 12;  // out of its slot this long: a straggler
constexpr int STUCK_FRAMES = FRAMES_PER_SEC * 30;  // out of its slot without a STUCK_DIST move this long: released
constexpr float STUCK_DIST = 64.f;
constexpr float TRACK_STEP = 32.f;  // the leader's track keeps a point every this many elmos
constexpr float MARCH_STEP = 32.f;  // width march and slot slide
constexpr float MAX_WIDTH = 900.f;
constexpr float MIN_HALF_WIDTH = 64.f;  // the line's direction is smoothed over its width, at least twice this
constexpr float COMPRESS = 0.7f;  // spacing squeezes to this share of normal before a second row is added
constexpr float ROW_GAPS = 3.f;  // rows are their widest footprint + this many gaps apart
constexpr float WIDTH_AHEAD_SEC = 3.f;  // the column count fits the narrowest spot this far ahead of the front row
constexpr float LOOK_AHEAD_SEC = 3.f;  // targets lead slots by the travel action's order interval + this
constexpr float LINE_AHEAD_SEC = 10.f;  // the line follows the path this far ahead of the leader
constexpr float CATCH_UP = 0.5f;  // extra speed (elmos/s) per elmo behind the slot
constexpr float MAX_CATCH_UP = 1.5f;  // catching up at most this x the slowest unit's speed
constexpr float MIN_SPEED = 0.25f;  // wanted speed never below this x the slowest unit's
constexpr float SLOT_SEC = 1.5f;  // near its slot: within max(spacing, this many seconds of its own walking)
constexpr float MIN_SLACK = 48.f;  // re-order slack at least
constexpr size_t PATH_SEARCH = 32;  // path segments searched for the leader's spot, from the last one
constexpr float ON_PATH_DIST = 64.f;  // the leader is on a path segment this close

// Share along segment ab of the point closest to p (2D)
float ProjectOnSegment(const AIFloat3& p, const AIFloat3& a, const AIFloat3& b)
{
	const float3 ab = b - a;
	const float sqLen = ab.SqLength2D();
	return (sqLen < 1e-3f) ? 0.f : utils::clamp((p - a).dot2D(ab) / sqLen, 0.f, 1.f);
}
}  // namespace

CFormation::CFormation(ISquadTask* task)
		: task(task)
		, widenFrame(-WIDEN_FRAMES)
{
}

void CFormation::Update(int frame, bool isActive, std::vector<CCircuitUnit*>& outStuck)
{
	if (!isActive) {
		Reset();
		return;
	}
	const std::shared_ptr<PathInfo>& path = task->GetSquadPath();
	if ((frame < solveFrame + SOLVE_FRAMES) && (path == solvedPath)) {
		return;
	}
	CCircuitAI* circuit = task->GetManager()->GetCircuit();
	SCOPED_TIME(circuit, __PRETTY_FUNCTION__);
	const CMilitaryManager::SFormationCfg& cfg = circuit->GetMilitaryManager()->GetFormationCfg();
	const float dt = isOn ? float(frame - solveFrame) / FRAMES_PER_SEC : 0.f;
	solveFrame = frame;
	if (path != solvedPath) {
		solvedPath = path;
		pathIndex = 0;  // a new path starts at the leader
	}

	// Land and sea units take slots (not support, not no_formation). Newcomers to a formed line straggle to theirs
	for (CCircuitUnit* unit : task->GetAssignees()) {
		CCircuitDef* cdef = unit->GetCircuitDef();
		if (cdef->IsAbleToFly() || cdef->IsRoleSupport() || cdef->IsAttrNoForm()) {
			continue;
		}
		auto result = members.try_emplace(unit);
		if (!result.second) {
			continue;
		}
		SMember& m = result.first->second;
		m.foot = std::max(cdef->GetDef()->GetXSize(), cdef->GetDef()->GetZSize()) * SQUARE_SIZE;
		m.maxSpeed = cdef->GetSpeed();
		// formation_row, else the main role's place in row_order (or that of the role it acts as), else last
		auto rankOf = [&cfg](CCircuitDef::RoleT role) {
			return int(std::find(cfg.rowOrder.begin(), cfg.rowOrder.end(), role) - cfg.rowOrder.begin());
		};
		m.rank = rankOf(cdef->GetMainRole());
		if (m.rank == (int)cfg.rowOrder.size()) {
			m.rank = rankOf(circuit->GetBindedRole(cdef->GetMainRole()));
		}
		if (cdef->GetFormationRow() >= 0) {
			m.rank = cdef->GetFormationRow();
		}
		m.isStraggler = isOn;
		m.slotFrame = m.moveFrame = frame;
		m.movePos = unit->GetPos(frame);
		m.row = -1;
		isDirty = true;
	}
	CCircuitUnit* leader = task->GetLeader();
	auto itLeader = members.find(leader);
	if (itLeader == members.end()) {
		isOn = false;  // a leader that doesn't walk in line
		return;
	}
	leaderM = &itLeader->second;
	leaderM->isStraggler = false;  // the line is centred on it
	if (leader != trackLeader) {
		track.clear();  // a new leader walked elsewhere
		trackLeader = leader;
	}
	const int count = members.size();
	const float baseSpacing = cfg.spacing * std::pow(cfg.spacingUnits / std::max<float>(count, cfg.spacingUnits), cfg.spacingExp);
	const float maxWidth = std::min(std::max(cfg.widthRange * task->GetFormationRange(), 3.f * baseSpacing), MAX_WIDTH);
	const float backLen = depth + maxWidth / 2 + baseSpacing;  // rows behind the leader, and smoothing
	if (!BuildLine(leader->GetPos(frame), *path, backLen, depth + maxWidth + leaderM->maxSpeed * LINE_AHEAD_SEC, maxWidth)
		|| (count < (isOn ? 2 : cfg.minUnits)))
	{
		isOn = false;  // the leader is far off its path (e.g. a new leader that just merged in), or too few units
		return;
	}

	float slowest = leaderM->maxSpeed;
	float maxFoot = 0.f;
	for (auto& kv : members) {
		CCircuitUnit* unit = kv.first;
		SMember& m = kv.second;
		ITravelAction* travelAct = unit->GetTravelAct();
		m.isSteered = (unit->Blocker() == nullptr) && travelAct->IsActive() && (travelAct->GetPath() == path);
		m.pos = unit->GetPos(frame);
		m.arc = Project(m.pos, -std::numeric_limits<float>::max(), &m.side);
		maxFoot = std::max(maxFoot, m.foot);
		if (m.isSteered && !m.isStraggler) {
			slowest = std::min(slowest, m.maxSpeed);
		}
	}

	// Free width across the line where the front row walks in the next few seconds, on the leader's move array:
	// the narrowest spot sets the columns, so the line narrows before a choke, not in it
	const int mobileId = leader->GetCircuitDef()->GetMobileId();
	const float frontArc = isOn ? Project(anchorPos, 0.f) : 0.f;
	const float endArc = std::min(frontArc + slowest * WIDTH_AHEAD_SEC, arcs.back());
	float freeWidth = maxWidth;
	for (float s = frontArc; ; s = std::min(s + MARCH_STEP * 2, endArc)) {
		const AIFloat3 p = PointAt(s);
		const AIFloat3 axis = AxisAt(s);
		freeWidth = std::min(freeWidth, FreeDist(p, axis, maxWidth, mobileId) + FreeDist(p, -axis, maxWidth, mobileId));
		if (s >= endArc) {
			break;
		}
	}
	// Narrower at once; wider only once it has fit half a spacing more for WIDEN_FRAMES
	if (freeWidth < heldWidth + baseSpacing / 2) {
		widenFrame = frame;
	}
	if ((freeWidth < heldWidth) || (frame >= widenFrame + WIDEN_FRAMES)) {
		heldWidth = freeWidth;
	}
	// Columns: half the widest footprint + gap clear at each end; spacing squeezes before a second row is added
	const float room = std::max(heldWidth - maxFoot - 2 * cfg.gap, 0.f);
	spacing = baseSpacing;
	if ((count - 1) * spacing > room) {
		spacing = std::max(room / (count - 1), COMPRESS * baseSpacing);
	}
	const int fitCols = std::min(count, int(room / spacing + 1e-3f) + 1);
	if (fitCols < 2) {
		isOn = false;  // no line fits: follow the path as before
		return;
	}

	const bool isSwitchOn = !isOn;
	if (isSwitchOn) {
		SwitchOn(frame);
	}
	if (isSwitchOn || isDirty || (fitCols != matchCols)) {
		Rematch(fitCols);
	}
	LayOut(spacing, cfg.gap);

	// Pace: the slowest unit's speed, slowing in proportion as the unit furthest behind its slot along the line falls
	// behind (one still spreading sideways isn't behind), to a stop at lead_sec behind: the line waits for everyone,
	// so the slowest can catch up too. Stragglers and busy units don't count
	const float leaderOffset = rows[leaderM->row].offset;
	float anchorArc = isSwitchOn ? leaderOffset : std::max(frontArc, leaderOffset);  // the leader's slot isn't behind it
	float lag = 0.f;
	for (const auto& kv : members) {
		const SMember& m = kv.second;
		if (m.isSteered && !m.isStraggler) {
			lag = std::max(lag, LagAt(anchorArc - rows[m.row].offset, m.pos));
		}
	}
	const float pace = std::max(slowest - lag / cfg.leadSec, 0.f);
	anchorArc = std::min(anchorArc + pace * dt, arcs.back());
	anchorPos = PointAt(anchorArc);

	// Each row shifts sideways as little as it takes to fit between obstacles where it walks to
	const float clear = maxFoot / 2 + cfg.gap;
	for (SRow& row : rows) {
		const float s = std::min(anchorArc + pace * (1.f + LOOK_AHEAD_SEC), arcs.back()) - row.offset;
		const AIFloat3 p = PointAt(s);
		const AIFloat3 axis = AxisAt(s);
		const float lo = clear - FreeDist(p, -axis, maxWidth, mobileId) - row.units.front()->x;
		const float hi = FreeDist(p, axis, maxWidth, mobileId) - clear - row.units.back()->x;
		row.shift = (lo <= hi) ? utils::clamp(0.f, lo, hi) : (lo + hi) / 2;
	}

	for (auto& kv : members) {
		CCircuitUnit* unit = kv.first;
		SMember& m = kv.second;
		const SRow& row = rows[m.row];
		const float x = row.shift + m.x;
		const int unitMobileId = unit->GetCircuitDef()->GetMobileId();
		const float slotArc = anchorArc - row.offset;
		const float slotLag = LagAt(slotArc, m.pos);

		// In its slot: near it, and not so far behind that the line has slowed to half. Out of it for STRAGGLE_FRAMES:
		// a straggler, until it is back (the leader never is); so a unit that holds the line up does so for a while only
		const bool isNear = (slotLag < cfg.leadSec * slowest / 2)
				&& (m.pos.SqDistance2D(SlotAt(slotArc, x, unitMobileId)) < SQUARE(std::max(spacing, m.maxSpeed * SLOT_SEC)));
		if (isNear || (&m == leaderM)) {
			m.slotFrame = frame;
			m.isStraggler = false;
		} else if (frame >= m.slotFrame + STRAGGLE_FRAMES) {
			m.isStraggler = true;
		}
		// Progress watchdog: out of its slot and walking it, but no STUCK_DIST move for STUCK_FRAMES
		if (!m.isSteered || isNear || (m.pos.SqDistance2D(m.movePos) >= SQUARE(STUCK_DIST))) {
			m.movePos = m.pos;
			m.moveFrame = frame;
		} else if (frame >= m.moveFrame + STUCK_FRAMES) {
			outStuck.push_back(unit);
		}

		// Target: the slot ahead by the order interval + LOOK_AHEAD_SEC at the line's pace, so the engine doesn't brake
		// on the way. While walking never behind the unit (it waits instead); at the path's end the slot itself
		const float ahead = pace * (float(unit->GetTravelAct()->GetOrderInterval()) / FRAMES_PER_SEC + LOOK_AHEAD_SEC);
		float targetArc = std::min(anchorArc + ahead, arcs.back()) - row.offset;
		if (!isLineToEnd || (anchorArc + ahead < arcs.back())) {
			targetArc = std::max(targetArc, slotArc - slotLag);  // level with the unit
		}
		m.target = SlotAt(targetArc, x, unitMobileId);
		// Speed: the pace, plus catching up along the line; stragglers at full speed
		const float catchUp = pace + CATCH_UP * slotLag;
		m.speed = m.isStraggler ? NO_SPEED_LIMIT : std::max(
				std::min(std::max(catchUp, 0.5f * pace), std::min(MAX_CATCH_UP * slowest, m.maxSpeed)),
				MIN_SPEED * slowest);
	}
}

void CFormation::Remove(CCircuitUnit* unit)
{
	if (members.erase(unit) > 0) {
		isDirty = true;
	}
	if (unit == trackLeader) {
		trackLeader = nullptr;
	}
}

bool CFormation::GetOrder(CCircuitUnit* unit, SFormationOrder& outOrder) const
{
	auto it = members.find(unit);
	if (!isOn || (it == members.end()) || !it->second.isSteered) {
		return false;
	}
	outOrder = {it->second.target, it->second.speed, std::max(spacing / 2, MIN_SLACK)};
	return true;
}

void CFormation::Reset()
{
	isOn = false;
	members.clear();
	rows.clear();
	leaderM = nullptr;
	track.clear();
	trackLeader = nullptr;
	solvedPath = nullptr;
	heldWidth = width = depth = 0.f;
	widenFrame = -WIDEN_FRAMES;
}

void CFormation::SwitchOn(int frame)
{
	// Everyone starts afresh where it stands (after a fight, a collapse or a flee)
	isOn = true;
	for (auto& kv : members) {
		SMember& m = kv.second;
		m.isStraggler = false;
		m.slotFrame = m.moveFrame = frame;
		m.movePos = m.pos;
	}
}

void CFormation::Rematch(int count)
{
	// Rows front to back: stragglers last, then by row order; within a rank, units further along first. Within a row,
	// left to right as the units stand, so none walk across each other; the leader takes the middle column of its row
	order.clear();
	for (auto& kv : members) {
		order.push_back(&kv.second);
	}
	std::sort(order.begin(), order.end(), [](const SMember* a, const SMember* b) {
		return std::tie(a->isStraggler, a->rank, b->arc) < std::tie(b->isStraggler, b->rank, a->arc);
	});
	rows.resize((order.size() + count - 1) / count);
	for (size_t r = 0; r < rows.size(); ++r) {
		std::vector<SMember*>& units = rows[r].units;
		units.assign(order.begin() + r * count, order.begin() + std::min((r + 1) * count, order.size()));
		std::sort(units.begin(), units.end(), [](const SMember* a, const SMember* b) { return a->side < b->side; });
		auto it = std::find(units.begin(), units.end(), leaderM);
		if (it != units.end()) {
			auto mid = units.begin() + (units.size() - 1) / 2;
			if (it < mid) {
				std::rotate(it, it + 1, mid + 1);
			} else {
				std::rotate(mid, it, it + 1);
			}
		}
		for (SMember* m : units) {
			m->row = r;
		}
	}
	matchCols = count;
	isDirty = false;
}

void CFormation::LayOut(float spacing, float gap)
{
	// Rows their widest footprint + ROW_GAPS gaps apart. In a row, neighbours spacing apart, or their half footprints
	// + gap if more; the row centred, or on the leader in its row
	float offset = 0.f;
	float prevFoot = 0.f;
	for (size_t r = 0; r < rows.size(); ++r) {
		SRow& row = rows[r];
		float foot = 0.f;
		float x = 0.f;
		for (size_t c = 0; c < row.units.size(); ++c) {
			SMember* m = row.units[c];
			if (c > 0) {
				x += std::max(spacing, (row.units[c - 1]->foot + m->foot) / 2 + gap);
			}
			m->x = x;
			foot = std::max(foot, m->foot);
		}
		if (r > 0) {
			offset += (prevFoot + foot) / 2 + ROW_GAPS * gap;
		}
		row.offset = offset;
		prevFoot = foot;
		const float mid = ((int)r == leaderM->row) ? leaderM->x : x / 2;
		for (SMember* m : row.units) {
			m->x -= mid;
		}
	}
	width = rows.front().units.back()->x - rows.front().units.front()->x;
	depth = offset;
}

void CFormation::UpdateTrack(const AIFloat3& pos, float keepLen)
{
	if (track.empty() || (track.back().SqDistance2D(pos) >= SQUARE(TRACK_STEP))) {
		track.push_back(pos);
	}
	// Only as far back as the line reaches (BuildLine extends it straight from there)
	float len = 0.f;
	for (size_t i = track.size() - 1; i > 0; --i) {
		len += track[i].distance2D(track[i - 1]);
		if (len > keepLen) {
			track.erase(track.begin(), track.begin() + i);
			break;
		}
	}
}

bool CFormation::BuildLine(const AIFloat3& leaderPos, const PathInfo& path, float backLen, float aheadLen, float maxOff)
{
	// The leader's spot on its path: the nearest point, searched on from the last segment (the leader walks a reused
	// path on; a new path starts at the leader), up to the first segment it is on, so a hairpin's far leg isn't taken
	const F3Vec& pts = path.posPath;
	size_t seg = std::min(pathIndex, pts.size() - 1);
	AIFloat3 spot = pts[seg];
	float sqOff = leaderPos.SqDistance2D(spot);
	for (size_t i = seg; (i + 1 < pts.size()) && (i < pathIndex + PATH_SEARCH); ++i) {
		const float t = ProjectOnSegment(leaderPos, pts[i], pts[i + 1]);
		const AIFloat3 p = pts[i] + (pts[i + 1] - pts[i]) * t;
		const float sq = leaderPos.SqDistance2D(p);
		if (sq < sqOff) {
			sqOff = sq;
			seg = i;
			spot = p;
		}
		if ((t < 1.f) && (sq < SQUARE(ON_PATH_DIST))) {
			break;
		}
	}
	pathIndex = seg;
	if (sqOff > SQUARE(maxOff)) {
		return false;
	}

	// The spot's track, the spot, the path ahead: through the spot, not the leader, so a leader that its row shifts
	// sideways doesn't bend the line
	UpdateTrack(spot, backLen);
	line.assign(track.begin(), track.end());
	if (line.back().SqDistance2D(spot) < 1.f) {
		line.pop_back();
	}
	size_t leaderIdx = line.size();
	line.push_back(spot);
	float len = 0.f;
	isLineToEnd = true;
	for (size_t i = seg + 1; i < pts.size(); ++i) {
		if (len > aheadLen) {
			isLineToEnd = false;
			break;
		}
		const float d = line.back().distance2D(pts[i]);
		if (d >= 1.f) {
			line.push_back(pts[i]);
			len += d;
		}
	}
	// Short of track (a new leader): extend the line backwards along its first stretch
	float back = 0.f;
	for (size_t i = 0; i < leaderIdx; ++i) {
		back += line[i].distance2D(line[i + 1]);
	}
	if (back < backLen) {
		float3 dir = (line.size() > 1) ? (line[1] - line[0])
				: ((pts.size() > 1) ? (pts.back() - pts[pts.size() - 2]) : RgtVector);
		dir.y = 0.f;
		dir = (dir.SqLength2D() < 1.f) ? RgtVector : dir.Normalize2D();
		const AIFloat3 start = line.front() - dir * (backLen - back);
		line.insert(line.begin(), start);
		++leaderIdx;
	}
	arcs.resize(line.size());
	arcs[leaderIdx] = 0.f;
	for (size_t i = leaderIdx; i > 0; --i) {
		arcs[i - 1] = arcs[i] - line[i].distance2D(line[i - 1]);
	}
	for (size_t i = leaderIdx + 1; i < line.size(); ++i) {
		arcs[i] = arcs[i - 1] + line[i].distance2D(line[i - 1]);
	}
	return true;
}

AIFloat3 CFormation::PointAt(float arc) const
{
	if (arc <= arcs.front()) {
		return line.front();
	}
	if (arc >= arcs.back()) {
		return line.back();
	}
	const size_t i = std::upper_bound(arcs.begin(), arcs.end(), arc) - arcs.begin() - 1;
	const float t = (arc - arcs[i]) / (arcs[i + 1] - arcs[i]);
	return line[i] + (line[i + 1] - line[i]) * t;
}

AIFloat3 CFormation::AxisAt(float arc) const
{
	// Direction from half the line's width behind to half its width ahead, so one kinked path point doesn't swing
	// the line; turned 90 degrees
	const float half = std::max(width / 2, MIN_HALF_WIDTH);
	const float3 d = PointAt(arc + half) - PointAt(arc - half);
	const float len = d.Length2D();
	return (len < 1.f) ? AIFloat3(RgtVector) : AIFloat3(-d.z / len, 0.f, d.x / len);
}

float CFormation::LagAt(float arc, const AIFloat3& pos) const
{
	// Along the line's smoothed direction there, as slots are laid out: a unit in its slot lags 0 on a bend too
	const AIFloat3 axis = AxisAt(arc);
	return (PointAt(arc) - pos).dot2D(AIFloat3(axis.z, 0.f, -axis.x));
}

float CFormation::Project(const AIFloat3& pos, float minArc, float* outSide) const
{
	float bestSq = std::numeric_limits<float>::max();
	float bestArc = minArc;
	float side = 0.f;
	for (size_t i = 0; i + 1 < line.size(); ++i) {
		if (arcs[i + 1] < minArc) {
			continue;
		}
		const float t = ProjectOnSegment(pos, line[i], line[i + 1]);
		const float3 d = line[i + 1] - line[i];
		const float3 q = line[i] + d * t;
		const float sq = pos.SqDistance2D(q);
		if (sq < bestSq) {
			bestSq = sq;
			bestArc = arcs[i] + t * (arcs[i + 1] - arcs[i]);
			side = (pos - q).dot2D(float3(-d.z, 0.f, d.x)) / (arcs[i + 1] - arcs[i]);  // along AxisAt
		}
	}
	if (outSide != nullptr) {
		*outSide = side;
	}
	return std::max(bestArc, minArc);
}

float CFormation::FreeDist(const AIFloat3& p, const AIFloat3& dir, float maxDist, int mobileTypeId) const
{
	CPathFinder* pathfinder = task->GetManager()->GetCircuit()->GetPathfinder();
	float dist = 0.f;
	while ((dist + MARCH_STEP <= maxDist) && pathfinder->IsMovable(mobileTypeId, p + dir * (dist + MARCH_STEP))) {
		dist += MARCH_STEP;
	}
	return dist;
}

AIFloat3 CFormation::SlotAt(float arc, float x, int mobileTypeId) const
{
	// The line itself is the path or the leader's track, where the squad can walk
	CPathFinder* pathfinder = task->GetManager()->GetCircuit()->GetPathfinder();
	const AIFloat3 base = PointAt(arc);
	const AIFloat3 axis = AxisAt(arc);
	while ((x != 0.f) && !pathfinder->IsMovable(mobileTypeId, base + axis * x)) {
		x = (std::fabs(x) > MARCH_STEP) ? x - std::copysign(MARCH_STEP, x) : 0.f;
	}
	AIFloat3 slot = base + axis * x;
	CTerrainManager::CorrectPosition(slot);
	return slot;
}

#ifdef DEBUG_VIS
void CFormation::Log() const
{
	CCircuitAI* circuit = task->GetManager()->GetCircuit();
	circuit->LOG("formation: %s | units: %i | rows: %i | columns: %i | free width: %.0f | spacing: %.0f",
			isOn ? "on" : "off", (int)members.size(), (int)rows.size(), matchCols, heldWidth, spacing);
	if (!isOn) {
		return;
	}
	circuit->GetDrawer()->AddPoint(anchorPos, "anchor");
	for (const auto& kv : members) {
		const SMember& m = kv.second;
		const std::string label = "row " + std::to_string(m.row) + (m.isStraggler ? " straggler" : "")
				+ (m.isSteered ? "" : " busy");
		circuit->GetDrawer()->AddPoint(m.target, label.c_str());
	}
}
#endif

} // namespace circuit

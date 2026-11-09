/*
 * Formation.cpp
 *
 *  Created on: Nov 7, 2026
 *      Author: Claude
 */

#include "task/fighter/Formation.h"
#include "task/fighter/SquadTask.h"
#include "module/MilitaryManager.h"
#include "map/ThreatMap.h"
#include "terrain/TerrainManager.h"
#include "terrain/path/PathFinder.h"
#include "unit/action/TravelAction.h"
#include "unit/enemy/EnemyUnit.h"
#include "unit/CircuitUnit.h"
#include "CircuitAI.h"
#include "util/Utils.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

namespace circuit {

using namespace springai;

namespace {
constexpr int SOLVE_FRAMES = FRAMES_PER_SEC / 2;  // lay the line out at most this often
constexpr int WIDEN_FRAMES = FRAMES_PER_SEC * 3;  // a wider way must last this long before the line widens
constexpr int STRAGGLE_FRAMES = FRAMES_PER_SEC * 12;  // out of its slot this long: a straggler
constexpr int STUCK_FRAMES = FRAMES_PER_SEC * 30;  // out of its slot without a DEFAULT_SLACK move this long: released
constexpr float STEP = SQUARE_SIZE * 4;  // the leader's track and the free distance tests
constexpr float MAX_WIDTH = 900.f;
constexpr float COMPRESS = 0.7f;  // spacing squeezes to this share of normal before a second row is added
constexpr float ROW_GAPS = 3.f;  // rows are their widest footprint + this many gaps apart
constexpr float WIDTH_AHEAD_SEC = 3.f;  // the column count fits the narrowest spot this far ahead of the front row
constexpr float LOOK_AHEAD_SEC = 3.f;  // targets lead slots by the travel action's order interval + this
constexpr float LINE_AHEAD_SEC = 10.f;  // the line follows the path this far ahead of the leader
constexpr float CATCH_UP = 0.5f;  // extra speed (elmos/s) per elmo behind the slot
constexpr float MAX_CATCH_UP = 1.5f;  // catching up at most this x the slowest unit's speed
constexpr float MIN_SPEED = 0.25f;  // wanted speed never below this x the slowest unit's
constexpr float SLOT_SEC = 1.5f;  // near its slot: within max(spacing, this many seconds of its own walking)
constexpr float THREAT_TOLERANCE = 1.5f;  // the threat map's cells differ up to this much within one enemy's reach
constexpr float THREAT_EPS = 1e-3f;
constexpr float TURN_BACK_COS = -0.5f;  // a new path that turns back by more than 120 degrees starts the rows afresh
constexpr float TURN_LOOK = 256.f;  // the path's heading: towards its point this far along
constexpr int FACE_EVAL_FRAMES = FRAMES_PER_SEC;  // facing: the threat around is read this often
constexpr int FACE_VOTES = 2;  // turn only for a threat that is far off this many reads in a row
constexpr float FACE_TURN_COS = 0.7071f;  // far off: more than 45 degrees
constexpr float FACE_CLEAR = 0.3f;  // a clear side: the threat's vector sum is at least this share of all of it
constexpr float FACE_KEEP = 256.f;  // a new path that ends this near the last one keeps the line facing
constexpr int SPLASH_SCAN_FRAMES = FRAMES_PER_SEC;  // enemies near are scanned for splash this often
constexpr int SPLASH_HOLD_FRAMES = FRAMES_PER_SEC * 10;  // a splash radius is kept this long after it was last seen
constexpr float SPLASH_PAD = 600.f;  // an enemy's splash counts within its weapon range + this of the leader
constexpr float SPLASH_MIN = 16.f;  // smaller splash radii don't spread the line
constexpr float MAX_SPLASH_PITCH = 256.f;  // splash spreads neighbours and rows at most this far apart
constexpr size_t PATH_SEARCH = 32;  // path segments searched for the leader's spot, from the last one
constexpr int SIGHT_HOLD_FRAMES = FRAMES_PER_SEC * 5;  // an enemy seen this recently: the line doesn't wait
constexpr int SWAP_PASSES = 8;  // SwapPlaces: passes of place swaps at most

// Share along segment ab of the point closest to p (2D)
float ProjectOnSegment(const AIFloat3& p, const AIFloat3& a, const AIFloat3& b)
{
	const float3 ab = b - a;
	const float sqLen = ab.SqLength2D();
	return (sqLen < 1e-3f) ? 0.f : utils::clamp((p - a).dot2D(ab) / sqLen, 0.f, 1.f);
}

// b turned more than 120 degrees from a (either zero: no)
bool IsTurnBack(const AIFloat3& a, const AIFloat3& b)
{
	return a.dot2D(b) < TURN_BACK_COS * a.Length2D() * b.Length2D();
}

// Where the path heads from spot on segment seg: towards its point TURN_LOOK further along (zero at the path's end)
float3 Heading(const F3Vec& pts, size_t seg, const AIFloat3& spot)
{
	AIFloat3 p = spot;
	float left = TURN_LOOK;
	for (size_t i = seg + 1; i < pts.size(); ++i) {
		const float d = p.distance2D(pts[i]);
		if (d >= left) {
			p += (pts[i] - p) * (left / d);
			break;
		}
		left -= d;
		p = pts[i];
	}
	return p - spot;
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
	CCircuitUnit* squadLeader = task->GetLeader();
	const std::shared_ptr<PathInfo>& path = squadLeader->GetTravelAct()->GetPath();  // the path the squad walks
	if ((frame < solveFrame + SOLVE_FRAMES) && (path == solvedPath)) {
		return;
	}
	CCircuitAI* circuit = task->GetManager()->GetCircuit();
	SCOPED_TIME(circuit, __PRETTY_FUNCTION__);
	const CMilitaryManager::SFormationCfg& cfg = circuit->GetMilitaryManager()->GetFormationCfg();
	const float dt = isOn ? float(frame - solveFrame) / FRAMES_PER_SEC : 0.f;
	solveFrame = frame;
	const bool isNewPath = (path != solvedPath);
	if (isNewPath) {
		solvedPath = path;
		pathIndex = 0;  // a new path starts at the leader
	}
	if (squadLeader != leader) {
		leader = squadLeader;
		track.clear();  // a new leader walked elsewhere
		isDirty = true;  // and takes the middle of its row
	}

	const float supportSpeed = AddMembers(frame);
	auto itLeader = members.find(leader);
	if (itLeader == members.end()) {
		isOn = false;  // a leader that doesn't walk in line
		return;
	}
	leaderM = &itLeader->second;
	leaderM->isStraggler = false;  // the line is centred on it
	const int count = members.size();
	// Near splash, neighbours and rows no closer than splash_spacing x its radius, so one blast doesn't hit two
	splashPitch = std::min(cfg.splashSpacing * UpdateSplash(frame), MAX_SPLASH_PITCH);
	const float baseSpacing = std::max(splashPitch,
			cfg.spacing * std::pow(cfg.spacingUnits / std::max<float>(count, cfg.spacingUnits), cfg.spacingExp));
	maxWidth = std::min(std::max(cfg.widthRange * task->GetFormationRange(), 3.f * baseSpacing), MAX_WIDTH);
	const float backLen = depth + maxWidth / 2 + baseSpacing;  // rows behind the leader, and smoothing
	// Facing stops on a path that ends elsewhere, or once the line is off
	if (isFacing && (!isOn || (path->posPath.back().SqDistance2D(faceEnd) > SQUARE(FACE_KEEP)))) {
		EndFacing();
	}
	// Running away: rows in reverse (riots at the back, towards the pursuers), matched afresh on the change. Not once
	// it stands facing (a cornered line at its quiet spot): the threat is ahead then
	const bool isFleeNow = task->IsFormationFlee(frame) && !isFacing;
	if (isFleeNow != isFlee) {
		isFlee = isFleeNow;
		isDirty = true;
	}
	bool isLine = true;
	if (isFacing) {
		BuildFacingLine(leader->GetPos(frame), backLen);
	} else {
		isLine = BuildLine(leader->GetPos(frame), *path, isNewPath, backLen,
				depth + maxWidth + leaderM->maxSpeed * LINE_AHEAD_SEC);
	}
	if (!isLine || (count < (isOn ? 2 : cfg.minUnits))) {
		isOn = false;  // the leader is far off its path (e.g. a new leader that just merged in), or too few units
		return;
	}
	MeasureMembers(frame, path, supportSpeed);
	// Facing: the anchor stands at the line's end
	const float frontArc = !isOn ? 0.f : (isFacing ? arcs.back() : Project(anchorPos, 0.f));
	const int cols = FitColumns(frame, frontArc, count, baseSpacing, cfg.gap);
	if (cols < 2) {
		isOn = false;  // no line fits: follow the path as before
		return;
	}

	const bool isSwitchOn = !isOn;
	if (isSwitchOn) {
		SwitchOn(frame);
	}
	const bool isMatch = isSwitchOn || isDirty || (cols != matchCols);
	if (isMatch) {
		Rematch(cols);
	}
	LayOut(cfg.gap);
	if (isMatch) {
		SwapPlaces(StartArc(isSwitchOn, frontArc));
	}
	const float pace = MoveAnchor(frame, isSwitchOn, frontArc, dt, cfg.leadSec);
	ShiftRows(pace, cfg.gap);
	PlaceSlots(frame, pace, cfg.leadSec, outStuck);
	if (cfg.faceRange > 0.f) {
		UpdateFacing(frame, path->posPath.back(), cfg.faceRange * task->GetFormationRange());
	}
}

float CFormation::AddMembers(int frame)
{
	// Land and sea units take slots (not support, not no_formation: the same tests as IsSlotUnit, kept in step with
	// it). Newcomers to a formed line straggle to theirs. Support units follow the leader instead (CSupportAction), so
	// the line walks no faster than they do
	CCircuitAI* circuit = task->GetManager()->GetCircuit();
	const CMilitaryManager::SFormationCfg& cfg = circuit->GetMilitaryManager()->GetFormationCfg();
	float supportSpeed = std::numeric_limits<float>::max();
	for (CCircuitUnit* unit : task->GetAssignees()) {
		CCircuitDef* cdef = unit->GetCircuitDef();
		if (cdef->IsAbleToFly() || cdef->IsAttrNoForm()) {
			continue;
		}
		if (cdef->IsRoleSupport()) {
			supportSpeed = std::min(supportSpeed, cdef->GetSpeed());
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
		m.isInSlot = false;
		m.slotFrame = m.moveFrame = frame;
		m.movePos = unit->GetPos(frame);
		m.row = -1;
		isDirty = true;
	}
	return supportSpeed;
}

void CFormation::MeasureMembers(int frame, const std::shared_ptr<PathInfo>& path, float supportSpeed)
{
	// The line's top speed: the slowest unit that keeps up with it (or will follow it), the leader at least
	slowest = std::min(leaderM->maxSpeed, supportSpeed);
	maxFoot = 0.f;
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
}

int CFormation::FitColumns(int frame, float frontArc, int count, float baseSpacing, float gap)
{
	// Free width across the line where the front row walks in the next few seconds: the narrowest spot sets the
	// columns, so the line narrows before a choke (or a gap between threats), not in it
	const int mobileId = leader->GetCircuitDef()->GetMobileId();
	const float endArc = std::min(frontArc + slowest * WIDTH_AHEAD_SEC, arcs.back());
	float freeWidth = maxWidth;
	for (float s = frontArc; ; s = std::min(s + DEFAULT_SLACK, endArc)) {
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
	// Columns: half the widest footprint + gap clear at each end; spacing squeezes before a second row is added. Fitted
	// with the widest unit's pitch (LayOut), so no row comes out wider than the room
	const float room = std::max(heldWidth - maxFoot - 2 * gap, 0.f);
	spacing = baseSpacing;
	if ((count - 1) * spacing > room) {
		spacing = std::max(room / (count - 1), std::max(COMPRESS * baseSpacing, splashPitch));
	}
	return std::min(count, int(room / std::max(spacing, maxFoot + gap) + 1e-3f) + 1);
}

void CFormation::SwitchOn(int frame)
{
	// Everyone starts afresh where it stands (after a fight, a collapse or a flee)
	isOn = true;
	rows.clear();  // and no row keeps a sideways shift from before
	for (auto& kv : members) {
		SMember& m = kv.second;
		m.isStraggler = false;
		m.slotFrame = m.moveFrame = frame;
		m.movePos = m.pos;
	}
}

void CFormation::Rematch(int cols)
{
	// Rows front to back: stragglers last, then by row order (reversed while running away); within a rank, units
	// further along first. Within a row, left to right as the units stand, so none walk across each other; the leader
	// takes the middle column of its row
	order.clear();
	for (auto& kv : members) {
		order.push_back(&kv.second);
	}
	const int sign = isFlee ? -1 : 1;
	std::sort(order.begin(), order.end(), [sign](const SMember* a, const SMember* b) {
		return std::make_tuple(a->isStraggler, sign * a->rank, b->arc) < std::make_tuple(b->isStraggler, sign * b->rank, a->arc);
	});
	rows.resize((order.size() + cols - 1) / cols);
	for (size_t r = 0; r < rows.size(); ++r) {
		std::vector<SMember*>& units = rows[r].units;
		units.assign(order.begin() + r * cols, order.begin() + std::min((r + 1) * cols, order.size()));
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
	matchCols = cols;
	isDirty = false;
}

void CFormation::SwapPlaces(float startArc)
{
	// After LayOut: two units of a row swap places where that shortens their walks to the places by more than half a
	// spacing (the order by side alone crosses paths when a column turns into a row); the leader keeps the middle
	for (SRow& row : rows) {
		const float arc = startArc - row.offset;
		const AIFloat3 base = PointAt(arc);
		const AIFloat3 axis = AxisAt(arc);
		std::vector<SMember*>& units = row.units;
		auto walk = [&](size_t unitIdx, size_t placeIdx) {
			return units[unitIdx]->pos.distance2D(base + axis * (row.shift + units[placeIdx]->x));
		};
		bool isSwapped = true;
		for (int pass = 0; isSwapped && (pass < SWAP_PASSES); ++pass) {
			isSwapped = false;
			for (size_t i = 0; i < units.size(); ++i) {
				for (size_t j = i + 1; j < units.size(); ++j) {
					if ((units[i] == leaderM) || (units[j] == leaderM)
						|| (walk(i, j) + walk(j, i) >= walk(i, i) + walk(j, j) - spacing / 2))
					{
						continue;
					}
					std::swap(units[i]->x, units[j]->x);  // x belongs to the place
					std::swap(units[i], units[j]);
					isSwapped = true;
				}
			}
		}
	}
}

void CFormation::LayOut(float gap)
{
	// Rows their widest footprint + ROW_GAPS gaps apart. In a row, one pitch: spacing, or the widest footprint + gap if
	// more; the row centred, or on the leader in its row
	float offset = 0.f;
	float prevFoot = 0.f;
	for (size_t r = 0; r < rows.size(); ++r) {
		SRow& row = rows[r];
		float foot = 0.f;
		for (SMember* m : row.units) {
			foot = std::max(foot, m->foot);
		}
		if (r > 0) {
			offset += std::max((prevFoot + foot) / 2 + ROW_GAPS * gap, splashPitch);
		}
		row.offset = offset;
		prevFoot = foot;
		const float pitch = std::max(spacing, foot + gap);
		auto itLeader = std::find(row.units.begin(), row.units.end(), leaderM);
		const float mid = (itLeader != row.units.end()) ? float(itLeader - row.units.begin()) : (row.units.size() - 1) / 2.f;
		for (size_t c = 0; c < row.units.size(); ++c) {
			row.units[c]->x = (c - mid) * pitch;
		}
	}
	width = rows.front().units.back()->x - rows.front().units.front()->x;
	depth = offset;
}

float CFormation::StartArc(bool isSwitchOn, float frontArc) const
{
	// Where the front row's centre starts this solve: the leader's slot isn't behind it, nor the anchor past the line's
	// end (a leader ahead of its row's slot there: after the line turned back while facing)
	const float leaderOffset = rows[leaderM->row].offset;
	return std::min(isSwitchOn ? leaderOffset : std::max(frontArc, leaderOffset), arcs.back());
}

float CFormation::MoveAnchor(int frame, bool isSwitchOn, float frontArc, float dt, float leadSec)
{
	// Pace: the slowest unit's speed, slowing in proportion as the unit furthest behind its slot along the line falls
	// behind (one still spreading sideways isn't behind), to a stop at lead_sec behind: the line waits for everyone,
	// so the slowest can catch up too. Stragglers, busy units and the squad's newcomers (ISquadTask::IsNewcomer: a
	// merge or a new unit, which catch up on their own) don't count; the leader always does, as the line is centred on it
	anchorArc = StartArc(isSwitchOn, frontArc);
	float lag = 0.f;
	for (const auto& kv : members) {
		const SMember& m = kv.second;
		if ((m.isSteered && !m.isStraggler && !task->IsNewcomer(kv.first, frame)) || (&m == leaderM)) {
			lag = std::max(lag, LagAt(anchorArc - rows[m.row].offset, m.pos));
		}
	}
	// Running away or near the enemy the line doesn't wait: it walks on at the slowest unit's speed and units catch up
	// as they can. Standing at the path's end the pace doesn't matter
	if ((lag > 0.f) && !IsAtEnd() && (isFlee || IsEnemyInSight(frame))) {
		lag = 0.f;
	}
	const float pace = std::max(slowest - lag / leadSec, 0.f);
	anchorArc = std::min(anchorArc + pace * dt, arcs.back());
	anchorPos = PointAt(anchorArc);
	return pace;
}

bool CFormation::IsEnemyInSight(int frame)
{
	// An enemy in LOS within some member's sight radius, now or in the last SIGHT_HOLD_FRAMES (so the line doesn't
	// stop and go as enemies step in and out of sight)
	if (frame < sightFrame + SIGHT_HOLD_FRAMES) {
		return true;
	}
	CCircuitAI* circuit = task->GetManager()->GetCircuit();
	const AIFloat3& leaderPos = leaderM->pos;
	float reach = 0.f;  // no member sees further than this from the leader
	for (const auto& kv : members) {
		reach = std::max(reach, leaderPos.distance2D(kv.second.pos) + kv.first->GetCircuitDef()->GetLosRadius());
	}
	for (auto& kv : circuit->GetEnemyInfos()) {
		const CEnemyInfo* enemy = kv.second;
		if (!enemy->IsInLOS() || (leaderPos.SqDistance2D(enemy->GetPos()) > SQUARE(reach))) {
			continue;
		}
		for (const auto& km : members) {
			if (km.second.pos.SqDistance2D(enemy->GetPos()) <= SQUARE(km.first->GetCircuitDef()->GetLosRadius())) {
				sightFrame = frame;
				return true;
			}
		}
	}
	return false;
}

float CFormation::UpdateSplash(int frame)
{
	// The largest splash radius (weapon area of effect, or a mobile bomb's blast) of enemies that can hit the ground
	// (mobile bombs too: no weapon, CCircuitDef takes their blast) within their range + SPLASH_PAD of the leader.
	// Finished, not hidden; mobile ones in radar or LOS, or seen within SPLASH_HOLD_FRAMES (not old ghosts). A larger
	// radius is taken at once, a smaller one only once the larger hasn't been seen for SPLASH_HOLD_FRAMES (so the line
	// doesn't widen and narrow as they move)
	if (frame < splashScanFrame + SPLASH_SCAN_FRAMES) {
		return splash;
	}
	splashScanFrame = frame;
	CCircuitAI* circuit = task->GetManager()->GetCircuit();
	const AIFloat3& leaderPos = leader->GetPos(frame);
	float nearAoe = 0.f;
	for (auto& kv : circuit->GetEnemyInfos()) {
		const CEnemyInfo* enemy = kv.second;
		const CCircuitDef* edef = enemy->GetCircuitDef();
		if ((edef == nullptr) || (edef->GetAoe() < std::max(SPLASH_MIN, nearAoe))
			|| !(edef->HasAntiLand() || (edef->IsMobile() && !edef->IsAttacker()))
			|| enemy->IsBeingBuilt() || enemy->IsHidden()
			|| (edef->IsMobile() && !enemy->IsRecentlySeen(frame, SPLASH_HOLD_FRAMES)))
		{
			continue;
		}
		if (leaderPos.SqDistance2D(enemy->GetPos()) < SQUARE(edef->GetMaxRange() + SPLASH_PAD)) {
			nearAoe = edef->GetAoe();
		}
	}
	if ((nearAoe >= splash) || (frame >= splashFrame + SPLASH_HOLD_FRAMES)) {
		splash = nearAoe;
		splashFrame = frame;
	}
	return splash;
}

void CFormation::ShiftRows(float pace, float gap)
{
	// Each row shifts sideways as little as it takes to fit where it walks to
	const int mobileId = leader->GetCircuitDef()->GetMobileId();
	const float clear = maxFoot / 2 + gap;
	for (SRow& row : rows) {
		const float s = std::min(anchorArc + pace * (1.f + LOOK_AHEAD_SEC), arcs.back()) - row.offset;
		const AIFloat3 p = PointAt(s);
		const AIFloat3 axis = AxisAt(s);
		const float lo = clear - FreeDist(p, -axis, maxWidth, mobileId) - row.units.front()->x;
		const float hi = FreeDist(p, axis, maxWidth, mobileId) - clear - row.units.back()->x;
		row.shift = (lo <= hi) ? utils::clamp(0.f, lo, hi) : (lo + hi) / 2;
	}
}

void CFormation::PlaceSlots(int frame, float pace, float leadSec, std::vector<CCircuitUnit*>& outStuck)
{
	// Running away, a unit ahead of its slot keeps the line's pace rather than dropping back towards the pursuers
	const float minSpeed = isFlee ? std::max(pace, MIN_SPEED * slowest) : std::max(0.5f * pace, MIN_SPEED * slowest);
	const bool isWalking = !IsAtEnd();  // not yet standing at the path's end
	for (auto& kv : members) {
		CCircuitUnit* unit = kv.first;
		SMember& m = kv.second;
		const SRow& row = rows[m.row];
		const float x = row.shift + m.x;
		const int mobileId = unit->GetCircuitDef()->GetMobileId();
		const float slotArc = anchorArc - row.offset;
		const float slotLag = LagAt(slotArc, m.pos);

		// In its slot: near it, and not so far behind that the line has slowed to half. Out of it for STRAGGLE_FRAMES:
		// a straggler, until it is back (the leader never is); so a unit that holds the line up does so for a while only
		const bool isNear = (slotLag < leadSec * slowest / 2)
				&& (m.pos.SqDistance2D(SlotAt(slotArc, x, mobileId)) < SQUARE(std::max(spacing, m.maxSpeed * SLOT_SEC)));
		m.isInSlot = isNear || (&m == leaderM);
		if (m.isInSlot) {
			m.slotFrame = frame;
			m.isStraggler = false;
		} else if (frame >= m.slotFrame + STRAGGLE_FRAMES) {
			m.isStraggler = true;
		}
		// Progress watchdog while the line walks: out of its slot and walking to it, but no DEFAULT_SLACK move for
		// STUCK_FRAMES (standing at the path's end, nothing waits for it)
		if (!isWalking || !m.isSteered || isNear || (m.pos.SqDistance2D(m.movePos) >= SQUARE(DEFAULT_SLACK))) {
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
		m.target = SlotAt(targetArc, x, mobileId);
		// Speed: the pace plus catching up along the line, within MIN_SPEED and MAX_CATCH_UP; stragglers at full speed.
		// Standing, by the way left to the slot instead: a unit ahead of it after the line turned isn't held to MIN_SPEED
		const float maxSpeed = std::max(std::min(MAX_CATCH_UP * slowest, m.maxSpeed), minSpeed);
		const float behind = isWalking ? slotLag : m.pos.distance2D(m.target);
		m.speed = m.isStraggler ? NO_SPEED_LIMIT : utils::clamp(pace + CATCH_UP * behind, minSpeed, maxSpeed);
	}
}

void CFormation::Remove(CCircuitUnit* unit)
{
	if (members.erase(unit) > 0) {
		isDirty = true;
	}
	if (unit == leader) {
		leader = nullptr;
	}
}

const CFormation::SMember* CFormation::FindSteered(const CCircuitUnit* unit) const
{
	if (!isOn) {
		return nullptr;
	}
	auto it = members.find(const_cast<CCircuitUnit*>(unit));
	return ((it != members.end()) && it->second.isSteered) ? &it->second : nullptr;
}

bool CFormation::GetOrder(CCircuitUnit* unit, SFormationOrder& outOrder) const
{
	const SMember* m = FindSteered(unit);
	if (m == nullptr) {
		return false;
	}
	outOrder = {m->target, m->speed};
	return true;
}

bool CFormation::IsKeptUp(const CCircuitUnit* unit) const
{
	const SMember* m = FindSteered(unit);
	return (m != nullptr) && (!m->isStraggler || (m->maxSpeed >= 1.1f * slowest));
}

bool CFormation::IsSteered(const CCircuitUnit* unit) const
{
	return FindSteered(unit) != nullptr;
}

bool CFormation::IsInSlot(const CCircuitUnit* unit) const
{
	const SMember* m = FindSteered(unit);
	return (m != nullptr) && m->isInSlot;
}

float CFormation::GetLeaderOffset() const
{
	// leader: nullptr once the leader left the squad (Remove), until the next solve
	if (!isOn || (leader == nullptr) || (leaderM == nullptr) || (leaderM->row < 0)
		|| (leaderM->row >= (int)rows.size()))
	{
		return 0.f;
	}
	return rows[leaderM->row].offset;
}

void CFormation::GetStandPath(F3Vec& outPath) const
{
	// From the line's point level with the leader (not the leader itself, which walks a little off the line) to the
	// front row's centre, along the line as it lies. Facing, to where it began to face instead: a block that turned
	// has its front up to its depth from there, past FACE_KEEP for a deep one, which would stop facing and match the
	// slots afresh; the facing block doesn't follow the path, so it stands as it is
	outPath.push_back(PointAt(0.f));
	for (size_t i = 0; i < line.size(); ++i) {
		if ((arcs[i] > 0.f) && (arcs[i] < anchorArc)) {
			outPath.push_back(line[i]);
		}
	}
	const AIFloat3& end = isFacing ? faceEnd : anchorPos;
	if (outPath.back().SqDistance2D(end) >= 1.f) {
		outPath.push_back(end);
	}
}

bool CFormation::SlideBy(const AIFloat3& shift, F3Vec& outPath)
{
	// The block's centre and the end it faces from move by shift; the next solve lays the block out there (the path
	// ends at the new faceEnd, so it keeps facing) and each unit walks to its slot, shifted as it is
	if (!isOn || !isFacing) {
		return false;
	}
	const AIFloat3 d(shift.x, 0.f, shift.z);
	const AIFloat3 oldCentre = faceCentre;
	faceCentre = oldCentre + d;
	float ahead;
	if (!FitFacing(faceDir, ahead)) {
		faceCentre = oldCentre;
		return false;
	}
	faceAhead = ahead;
	faceEnd = faceEnd + d;
	AIFloat3 start = PointAt(0.f) + d;  // level with the leader there
	CTerrainManager::CorrectPosition(start);
	outPath.assign({start, faceEnd});
	return true;
}

void CFormation::EndFacing()
{
	if (!isFacing) {
		return;
	}
	isFacing = false;
	track.clear();  // the rows stood across faceDir, not along the track
	isDirty = true;  // and walk a new way: match units to slots afresh
}

bool CFormation::IsSlotUnit(const CCircuitDef* cdef)
{
	return !cdef->IsAbleToFly() && !cdef->IsAttrNoForm() && !cdef->IsRoleSupport();
}

void CFormation::Reset()
{
	isOn = false;
	members.clear();
	rows.clear();
	leader = nullptr;
	leaderM = nullptr;
	track.clear();
	heading = ZeroVector;  // sightFrame stays: a line forming right after a fight doesn't wait either
	isFacing = false;
	solvedPath = nullptr;
	heldWidth = width = depth = 0.f;
	widenFrame = -WIDEN_FRAMES;
}

void CFormation::UpdateTrack(const AIFloat3& pos, float keepLen)
{
	if (track.empty() || (track.back().SqDistance2D(pos) >= SQUARE(STEP))) {
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

bool CFormation::BuildLine(const AIFloat3& leaderPos, const PathInfo& path, bool isNewPath, float backLen, float aheadLen)
{
	// The leader's spot on its path: the nearest point, searched on from the last segment (the leader walks a reused
	// path on), up to the first segment it is on, so a hairpin's far leg isn't taken. A new path usually starts at the
	// leader; one sent again (the walk resumed after a regroup) is searched to its end, once
	const F3Vec& pts = path.posPath;
	size_t seg = std::min(pathIndex, pts.size() - 1);
	AIFloat3 spot = pts[seg];
	float sqOff = leaderPos.SqDistance2D(spot);
	const size_t searchEnd = isNewPath ? pts.size() : (pathIndex + PATH_SEARCH);
	for (size_t i = seg; (i + 1 < pts.size()) && (i < searchEnd); ++i) {
		const float t = ProjectOnSegment(leaderPos, pts[i], pts[i + 1]);
		const AIFloat3 p = pts[i] + (pts[i + 1] - pts[i]) * t;
		const float sq = leaderPos.SqDistance2D(p);
		if (sq < sqOff) {
			sqOff = sq;
			seg = i;
			spot = p;
		}
		if ((t < 1.f) && (sq < SQUARE(DEFAULT_SLACK))) {
			break;
		}
	}
	pathIndex = seg;
	// A new path that turns back from where the line was heading at the last solve: the track lies ahead now, so it
	// is dropped (the rows behind are laid straight behind the new heading) and units are matched to slots afresh.
	// Along one path the heading turns gradually between solves, even round a sharp corner; a reversal jumps
	const float3 ahead = Heading(pts, seg, spot);
	if (isNewPath && (track.size() > 1) && IsTurnBack(heading, ahead)) {
		track.clear();
		isDirty = true;
	}
	if (ahead.SqLength2D() > 1.f) {
		heading = ahead;  // at the path's end: the way it came
	}

	if (sqOff > SQUARE(maxWidth)) {
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

void CFormation::BuildFacingLine(const AIFloat3& leaderPos, float backLen)
{
	// Arc 0 level with the leader (not past the front), as on a path
	const AIFloat3 front = faceCentre + faceDir * faceAhead;
	const float frontArc = std::max((front - leaderPos).dot2D(faceDir), 0.f);
	const float len = backLen + frontArc;
	line.assign({front - faceDir * len, front});
	arcs.assign({frontArc - len, frontArc});
	isLineToEnd = true;
}

void CFormation::UpdateFacing(int frame, const AIFloat3& pathEnd, float radius)
{
	if (frame < faceFrame + FACE_EVAL_FRAMES) {
		return;
	}
	AIFloat3 dir;
	float ahead;
	if (isFacing) {
		// Turn for a clear threat more than 45 degrees off, FACE_VOTES reads running, where the block fits that way
		faceFrame = frame;
		if (!ThreatDir(faceCentre, radius, dir) || (dir.dot2D(faceDir) >= FACE_TURN_COS)) {
			faceVotes = 0;
		} else if ((++faceVotes >= FACE_VOTES) && FitFacing(dir, ahead)) {
			faceVotes = 0;
			Face(dir, ahead);
		}
		return;
	}
	if (!IsAtEnd()) {
		return;  // not standing at the path's end yet
	}
	// Standing at the path's end: face the way the line came (over its last TURN_LOOK, not the last jog of the path),
	// or a clear threat where the block fits
	float3 arrival = PointAt(arcs.back()) - PointAt(arcs.back() - TURN_LOOK);
	arrival.y = 0.f;
	if (arrival.SqLength2D() < 1.f) {
		return;
	}
	faceDir = arrival.Normalize2D();
	faceCentre = anchorPos - faceDir * (depth / 2);
	faceEnd = pathEnd;
	faceFrame = frame;
	faceVotes = 0;
	isFacing = true;
	if (ThreatDir(faceCentre, radius, dir) && FitFacing(dir, ahead)) {
		Face(dir, ahead);
	} else {
		Face(faceDir, depth / 2);  // the front row where it stands: the line just walked there
	}
}

bool CFormation::ThreatDir(const AIFloat3& centre, float radius, AIFloat3& outDir) const
{
	// The threat map: every armed enemy that can hit the leader's type (seen, on radar or last seen), by its strength
	if (radius <= 0.f) {
		return false;  // no land weapon (torpedoes only): the way the line came
	}
	radius = std::max(radius, float(DEFAULT_SLACK * 4));
	float3 sum = ZeroVector;
	float total = 0.f;
	for (int a = 0; a < 8; ++a) {
		const float angle = a * static_cast<float>(M_PI) / 4.f;
		const AIFloat3 dir(std::cos(angle), 0.f, std::sin(angle));
		for (float r : {radius / 2, radius}) {
			AIFloat3 p = centre + dir * r;
			CTerrainManager::CorrectPosition(p);
			const float threat = task->GetTaskThreatAt(leader, p);
			sum += dir * threat;
			total += threat;
		}
	}
	const float len = sum.Length2D();
	if ((total <= THREAT_EPS) || (len < FACE_CLEAR * total)) {
		return false;
	}
	outDir = AIFloat3(sum.x / len, 0.f, sum.z / len);
	return true;
}

bool CFormation::FitFacing(const AIFloat3& dir, float& outAhead) const
{
	// The front row half the depth ahead of the centre, or less: short of a spot that isn't free (FreeDist, more threat
	// than at the centre), so it doesn't step into a turret's reach; measured once per turn, so a threat edge moving
	// across the block doesn't move it to and fro. The rows behind walkable; and 2 columns at least across the front,
	// as FitColumns fits them, else the line would switch off there and face that way again on its way back
	CCircuitAI* circuit = task->GetManager()->GetCircuit();
	const float gap = circuit->GetMilitaryManager()->GetFormationCfg().gap;
	const int mobileId = leader->GetCircuitDef()->GetMobileId();
	outAhead = FreeDist(faceCentre, dir, depth / 2, mobileId);
	const float back = depth - outAhead;
	if (FreeDist(faceCentre, -dir, back, mobileId, false) < back) {
		return false;
	}
	const AIFloat3 front = faceCentre + dir * outAhead;
	const AIFloat3 axis(-dir.z, 0.f, dir.x);
	const float room = FreeDist(front, axis, maxWidth, mobileId) + FreeDist(front, -axis, maxWidth, mobileId)
			- maxFoot - 2 * gap;
	return room >= std::max(spacing, maxFoot + gap);
}

void CFormation::Face(const AIFloat3& dir, float ahead)
{
	if (IsTurnBack(faceDir, dir)) {
		isDirty = true;  // the rows swap ends: match units to slots afresh (turned less, the block turns as it is)
	}
	faceDir = dir;
	faceAhead = ahead;
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
	const float half = std::max(width / 2, float(DEFAULT_SLACK));
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

float CFormation::FreeDist(const AIFloat3& p, const AIFloat3& dir, float maxDist, int mobileTypeId, bool isSafe) const
{
	// Free: walkable, and (isSafe) no more than THREAT_TOLERANCE x the enemy threat at p (none where p has none). The
	// last step may be short, so slots aren't rounded to STEP
	CCircuitAI* circuit = task->GetManager()->GetCircuit();
	CPathFinder* pathfinder = circuit->GetPathfinder();
	AIFloat3 ref = p;
	CTerrainManager::CorrectPosition(ref);  // the line's straight start can reach past the map's edge
	const float maxThreat = isSafe ? (THREAT_TOLERANCE * task->GetTaskThreatAt(leader, ref) + THREAT_EPS) : 0.f;
	float dist = 0.f;
	while (dist < maxDist) {
		const float next = std::min(dist + STEP, maxDist);
		const AIFloat3 q = p + dir * next;
		if (!pathfinder->IsMovable(mobileTypeId, q)  // a movable spot is inside the map
			|| (isSafe && (task->GetTaskThreatAt(leader, q) > maxThreat)))
		{
			break;
		}
		dist = next;
	}
	return dist;
}

AIFloat3 CFormation::SlotAt(float arc, float x, int mobileTypeId) const
{
	// No further out than the first spot that isn't free: the line itself is the path or the leader's track
	const AIFloat3 base = PointAt(arc);
	const AIFloat3 axis = AxisAt(arc);
	x = (x >= 0.f) ? FreeDist(base, axis, x, mobileTypeId) : -FreeDist(base, -axis, -x, mobileTypeId);
	AIFloat3 slot = base + axis * x;
	CTerrainManager::CorrectPosition(slot);
	return slot;
}

#ifdef DEBUG_VIS
void CFormation::Log() const
{
	CCircuitAI* circuit = task->GetManager()->GetCircuit();
	circuit->LOG("formation: %s%s | units: %i | rows: %i | columns: %i | free width: %.0f | spacing: %.0f",
			isOn ? "on" : "off", isFacing ? " facing" : "", (int)members.size(), (int)rows.size(), matchCols, heldWidth,
			spacing);
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

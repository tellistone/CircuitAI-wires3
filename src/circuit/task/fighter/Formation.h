/*
 * Formation.h
 *
 *  Created on: Nov 7, 2026
 *      Author: Claude
 */

#ifndef SRC_CIRCUIT_TASK_FIGHTER_FORMATION_H_
#define SRC_CIRCUIT_TASK_FIGHTER_FORMATION_H_

#include "util/Defines.h"

#include <deque>
#include <map>
#include <memory>
#include <vector>

namespace circuit {

class CCircuitUnit;
class ISquadTask;

// One unit's move from its squad's formation (CFormation::GetOrder)
struct SFormationOrder {
	springai::AIFloat3 pos;  // walk here
	float speed;  // wanted speed; NO_SPEED_LIMIT: full speed
};

/*
 * Line formation (behaviour.json "formation"): a squad of land or sea units walking its path outside combat goes as
 * a line across the path, with more rows only where the line has to narrow. The task plans its path as before; the
 * formation decides where along and beside it each unit walks, and how fast. The travel action asks GetOrder.
 * - The line: the path the leader walks, through the leader's spot on it, and behind the spot its track. A new path
 *   that turns back by more than 120 degrees drops the track, so the rows behind are laid behind the new heading.
 * - Width: the free width across the line where it walks in the next few seconds, at most width_range x the
 *   squad's fire range. Free: ground the leader's type can walk, with no more than 1.5 x the threat on the line
 *   there (the threat map's grain), so flanks don't step into a turret's range the path kept out of, or deeper into
 *   one it crosses. Each row shifts sideways to fit, and a slot never lies past the first spot that isn't free, seen
 *   from the line. Spacing shrinks as the squad grows, and squeezes to 0.7 of that before a second row is added.
 *   Below 2 columns the formation lets go and the units follow the path as before, until 2 fit again for a few
 *   seconds.
 * - Slots: rows fill front to back by row_order. In a row, one pitch; units take the places left to right as they
 *   stand, then two swap places where that shortens their walks by more than half a spacing, so paths don't cross;
 *   the leader takes the middle. Matched again when units join or leave, the columns or the leader change, or the
 *   path turns back.
 * - Pace: the front row's centre (anchor) walks at the speed of the slowest unit in line or support unit (those
 *   follow the leader), and slows in proportion as the unit furthest behind its slot along the line falls behind
 *   (the leader counts even while busy), stopping when it is lead_sec behind. Each unit walks to its slot a few
 *   seconds ahead, at the anchor's speed plus a catch-up term. At the path's end the line stands in its slots.
 * - A unit out of its slot for 12 s straggles: full speed, and the line no longer waits for it (the squad's regroup
 *   gathers it if it falls far behind). While the line walks, one that hasn't moved 64 elmos in 30 s while out of
 *   its slot is released (ISquadTask::ReleaseStuck), the leader too.
 * - A unit allowed to jump on the move (attribute "jump") jumps along the path as before whenever its jump is ready
 *   (ITravelAction), then walks back into line. Jumps into combat and when retreating don't go through the line.
 */
class CFormation {
public:
	CFormation(ISquadTask* task);

	// Every update of the task. isActive: the squad walks the leader's path outside combat; if not, everything is
	// dropped. Lays the line out at most twice a second. outStuck: units to release
	void Update(int frame, bool isActive, std::vector<CCircuitUnit*>& outStuck);
	void Remove(CCircuitUnit* unit);  // it left the squad
	bool IsOn() const { return isOn; }  // the line is formed: its units walk to GetOrder
	// Where the unit walks now; false: it has no slot (busy, no_formation, or the line is off): follow the path
	bool GetOrder(CCircuitUnit* unit, SFormationOrder& outOrder) const;

#ifdef DEBUG_VIS
	void Log() const;
#endif

private:
	struct SMember {
		float foot;  // footprint width
		float maxSpeed;
		int rank;  // row order, front first
		bool isSteered;  // walks the leader's path now (not busy, not on an order of its own): GetOrder steers it
		bool isStraggler;
		int slotFrame;  // last time near its slot
		int moveFrame;  // progress watchdog: last time it was 64 elmos from movePos (or had nowhere to go)
		springai::AIFloat3 movePos;
		springai::AIFloat3 pos;  // this solve
		float arc;  // along the line, at the nearest point (for matching units to slots)
		float side;  // beside the line (signed)
		int row;  // its slot: row and lateral offset from the row's centre (or from the leader)
		float x;
		springai::AIFloat3 target;  // GetOrder
		float speed;
	};
	struct SRow {
		std::vector<SMember*> units;  // left to right
		float offset;  // behind the front row, along the line
		float shift;  // sideways, to fit between obstacles
	};

	// The steps of Update, in order
	float AddMembers(int frame);  // returns the slowest support unit's speed
	void MeasureMembers(int frame, const std::shared_ptr<PathInfo>& path, float supportSpeed);
	int FitColumns(int frame, float frontArc, int count, float baseSpacing, float gap);  // returns the columns
	void SwitchOn(int frame);
	void Rematch(int cols, bool isSwitchOn, float frontArc, float gap);
	void LayOut(float gap);
	float StartArc(bool isSwitchOn, float frontArc) const;  // the anchor before this solve's step
	float MoveAnchor(bool isSwitchOn, float frontArc, float dt, float leadSec);  // returns the pace
	void ShiftRows(float pace, float gap);
	void PlaceSlots(int frame, float pace, float leadSec, std::vector<CCircuitUnit*>& outStuck);
	void Reset();

	// The centre line: the track of the leader's spot on its path, the spot, then the path ahead; arc 0 at the spot.
	// False: the leader is more than maxWidth from its path
	bool BuildLine(const springai::AIFloat3& leaderPos, const PathInfo& path, bool isNewPath, float backLen, float aheadLen);
	void UpdateTrack(const springai::AIFloat3& pos, float keepLen);
	springai::AIFloat3 PointAt(float arc) const;
	springai::AIFloat3 AxisAt(float arc) const;  // unit vector across the line (smoothed)
	float LagAt(float arc, const springai::AIFloat3& pos) const;  // how far pos is behind the point at arc, along the line
	float Project(const springai::AIFloat3& pos, float minArc, float* outSide = nullptr) const;
	// Free distance from p along dir, up to maxDist (see the class comment)
	float FreeDist(const springai::AIFloat3& p, const springai::AIFloat3& dir, float maxDist, int mobileTypeId) const;
	// The point x beside the line at arc, no further out than the free distance on that side
	springai::AIFloat3 SlotAt(float arc, float x, int mobileTypeId) const;

	ISquadTask* task;
	std::map<CCircuitUnit*, SMember> members;  // land and sea units that take a slot
	std::vector<SRow> rows;  // front first
	std::vector<SMember*> order;  // Rematch scratch
	CCircuitUnit* leader = nullptr;  // the line is centred on it (a new one clears the track)
	SMember* leaderM = nullptr;
	bool isOn = false;
	bool isDirty = false;  // match units to slots again (members changed, or the path turned back)
	int matchCols = 0;  // columns of the current match

	std::deque<springai::AIFloat3> track;  // the leader's last spots on its path, oldest first
	F3Vec line;  // BuildLine
	std::vector<float> arcs;
	bool isLineToEnd = false;  // the line reaches the path's end
	size_t pathIndex = 0;  // the leader's segment on its path
	std::shared_ptr<PathInfo> solvedPath;  // a new path is laid out at once

	// This solve
	int solveFrame = -1;
	float slowest = 0.f;  // the line's top speed
	float maxWidth = 0.f;
	float maxFoot = 0.f;
	float spacing = 0.f;
	float anchorArc = 0.f;  // the front row's centre on the line
	springai::AIFloat3 anchorPos;  // the same, for the next solve's line
	float heldWidth = 0.f;  // free width in use (narrows at once, widens after WIDEN_FRAMES)
	int widenFrame;
	float width = 0.f;  // of the front row
	float depth = 0.f;  // front to back row
};

} // namespace circuit

#endif // SRC_CIRCUIT_TASK_FIGHTER_FORMATION_H_

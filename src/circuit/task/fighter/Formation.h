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
	float slack;  // a new pos this far off the line to the last one is worth a new order
};

/*
 * Line formation (behaviour.json "formation"): a squad of land or sea units walking its path outside combat goes as
 * a line across the path, with more rows only where the line has to narrow. The task plans its path as before; the
 * formation decides where along and beside it each unit walks, and how fast. The travel action asks GetOrder.
 * - Centred on the leader: the line is its path from the leader's spot on it, and behind that the spot's track.
 * - Width: the free width across the path (the leader's move array) where the line walks in the next few seconds,
 *   at most width_range x the squad's fire range. Each row shifts sideways to fit between obstacles; a slot on
 *   ground its unit can't stand on slides towards the path. Spacing shrinks as the squad grows, and squeezes to 0.7
 *   of that before a second row is added. Rows fill front to back by row_order. Below 2 columns the formation lets
 *   go and the units follow the path as before, until 2 fit again for a few seconds.
 * - Pace: the front row's centre (anchor) walks at the slowest unit's speed, and slows in proportion as the unit
 *   furthest behind its slot along the line falls behind, stopping when it is lead_sec behind. Each unit walks to
 *   its slot a few seconds ahead, at the anchor's speed plus a catch-up term.
 * - A unit out of its slot for 12 s straggles: full speed, and the line no longer waits for it (the squad's regroup
 *   gathers it if it falls far behind). One that hasn't moved 64 elmos in 30 s while out of its slot is released
 *   (ISquadTask::ReleaseStuck), the leader too.
 */
class CFormation {
public:
	CFormation(ISquadTask* task);

	// Every update of the task. isActive: the squad walks its shared path outside combat; if not, everything is
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
		bool isSteered;  // walks the squad's path now (not busy, not on an order of its own): GetOrder steers it
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

	void Reset();
	void SwitchOn(int frame);
	void Rematch(int cols);
	void LayOut(float spacing, float gap);
	void UpdateTrack(const springai::AIFloat3& pos, float keepLen);
	// The centre line: the track of the leader's spot on its path, the spot, then the path ahead; arc 0 at the spot.
	// False: the leader is more than maxOff from its path (e.g. a new leader that just merged in)
	bool BuildLine(const springai::AIFloat3& leaderPos, const PathInfo& path, float backLen, float aheadLen, float maxOff);

	springai::AIFloat3 PointAt(float arc) const;
	springai::AIFloat3 AxisAt(float arc) const;  // unit vector across the line (smoothed)
	float LagAt(float arc, const springai::AIFloat3& pos) const;  // how far pos is behind the point at arc, along the line
	float Project(const springai::AIFloat3& pos, float minArc, float* outSide = nullptr) const;
	// Free distance from p along dir on the mobile type's move array, in MARCH_STEP steps up to maxDist
	float FreeDist(const springai::AIFloat3& p, const springai::AIFloat3& dir, float maxDist, int mobileTypeId) const;
	// The point x beside the line at arc, slid towards the line until the mobile type can stand there
	springai::AIFloat3 SlotAt(float arc, float x, int mobileTypeId) const;

	ISquadTask* task;
	std::map<CCircuitUnit*, SMember> members;  // land and sea units that take a slot
	std::vector<SRow> rows;  // front first
	std::vector<SMember*> order;  // Rematch scratch
	SMember* leaderM = nullptr;
	bool isOn = false;
	bool isDirty = false;  // members changed: match units to slots again
	int matchCols = 0;  // columns of the current match

	std::deque<springai::AIFloat3> track;  // the leader's last spots on its path, oldest first
	CCircuitUnit* trackLeader = nullptr;
	F3Vec line;  // BuildLine
	std::vector<float> arcs;
	bool isLineToEnd = false;  // the line reaches the path's end
	size_t pathIndex = 0;  // the leader's segment on its path
	std::shared_ptr<PathInfo> solvedPath;  // a new path is laid out at once

	int solveFrame = -1;
	springai::AIFloat3 anchorPos;  // the front row's centre on the line
	float heldWidth = 0.f;  // free width in use (narrows at once, widens after WIDEN_FRAMES)
	int widenFrame;
	float spacing = 0.f;
	float width = 0.f;  // of the front row
	float depth = 0.f;  // front to back row
};

} // namespace circuit

#endif // SRC_CIRCUIT_TASK_FIGHTER_FORMATION_H_

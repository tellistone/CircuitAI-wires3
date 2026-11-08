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
 *   that turns back by more than 120 degrees from where the line was heading drops the track, so the rows behind are
 *   laid behind the new heading (a sharp corner of the same route doesn't: it turns the heading gradually).
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
 *   (the leader counts even while busy), stopping when it is lead_sec behind; but while an enemy is in some member's
 *   LOS (or was in the last 5 s) it doesn't wait for anyone. Each unit walks to its slot a few
 *   seconds ahead, at the anchor's speed plus a catch-up term. At the path's end the line stands in its slots.
 * - Facing (face_range): once the anchor stands at the path's end, the line becomes a straight segment that points
 *   at the threat, its rows across it: the threat map read 8 ways on two rings around the block (half and all of
 *   face_range x the fire range), if it has a clear side, else the way the line came. The block turns about its own
 *   centre. It turns again only for a clear threat more than 45 degrees off, two reads a second apart, and only where
 *   its rows can stand that way (walkable); a turn back (> 120 degrees) matches slots afresh. It keeps facing across
 *   new paths that end within 256 elmos of the same end, and stops on a path elsewhere or when the line switches off.
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
	// The line steers the unit and it keeps up (in line, or a straggler faster than the line): no regroup for it
	bool IsKeptUp(const CCircuitUnit* unit) const;

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
	void Rematch(int cols);  // rows, and places by side
	void LayOut(float gap);
	void SwapPlaces(float startArc);  // after a Rematch and LayOut: uncross walks to the places
	float StartArc(bool isSwitchOn, float frontArc) const;  // the anchor before this solve's step
	float MoveAnchor(int frame, bool isSwitchOn, float frontArc, float dt, float leadSec);  // returns the pace
	bool IsEnemyInSight(int frame);  // an enemy in some member's LOS (or was, a few seconds ago): the line won't wait
	void ShiftRows(float pace, float gap);
	void PlaceSlots(int frame, float pace, float leadSec, std::vector<CCircuitUnit*>& outStuck);
	void UpdateFacing(int frame, const PathInfo& path, float radius);  // start facing at the path's end, or turn
	void Reset();

	// The centre line: the track of the leader's spot on its path, the spot, then the path ahead; arc 0 at the spot.
	// False: the leader is more than maxWidth from its path
	bool BuildLine(const springai::AIFloat3& leaderPos, const PathInfo& path, bool isNewPath, float backLen, float aheadLen);
	// Facing: the straight line across faceDir, its end (the front row's centre) half the depth ahead of faceCentre
	void BuildFacingLine(const springai::AIFloat3& leaderPos, float backLen);
	// Where the threat is around centre (8 ways, rings at radius / 2 and radius); false: none, or no clear side
	bool ThreatDir(const springai::AIFloat3& centre, float radius, springai::AIFloat3& outDir) const;
	bool IsBlockFree(const springai::AIFloat3& centre, const springai::AIFloat3& dir) const;  // rows can stand facing dir
	void Face(const springai::AIFloat3& dir);  // turn the block; a turn back (> 120 degrees) matches slots afresh
	void UpdateTrack(const springai::AIFloat3& pos, float keepLen);
	springai::AIFloat3 PointAt(float arc) const;
	springai::AIFloat3 AxisAt(float arc) const;  // unit vector across the line (smoothed)
	float LagAt(float arc, const springai::AIFloat3& pos) const;  // how far pos is behind the point at arc, along the line
	float Project(const springai::AIFloat3& pos, float minArc, float* outSide = nullptr) const;
	// Free distance from p along dir, up to maxDist (see the class comment); isSafe: and no more threat than at p
	float FreeDist(const springai::AIFloat3& p, const springai::AIFloat3& dir, float maxDist, int mobileTypeId,
			bool isSafe = true) const;
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
	springai::AIFloat3 heading = ZeroVector;  // where the line headed at the last solve (Heading), for turn-backs
	bool isFacing = false;  // standing at the path's end, the line is the straight segment across faceDir
	springai::AIFloat3 faceCentre;  // the block's centre when it began facing (it turns about it)
	springai::AIFloat3 faceEnd;  // the path's end then: a new path ending near it keeps facing
	springai::AIFloat3 faceDir;  // unit vector, towards the threat
	int faceFrame = 0;  // last facing evaluation
	int faceVotes = 0;  // evaluations in a row that found a clear threat far off faceDir
	F3Vec line;  // BuildLine
	std::vector<float> arcs;
	bool isLineToEnd = false;  // the line reaches the path's end
	size_t pathIndex = 0;  // the leader's segment on its path
	std::shared_ptr<PathInfo> solvedPath;  // a new path is laid out at once

	// This solve
	int solveFrame = -1;
	int sightFrame = -FRAMES_PER_SEC * 5;  // last time an enemy was in sight (IsEnemyInSight)
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

/*
 * TravelMap.cpp
 *
 *  Created on: Nov 27, 2026
 *      Author: Claude
 */

#include "terrain/path/TravelMap.h"
#include "terrain/path/PathFinder.h"
#include "terrain/path/QueryCostMap.h"
#include "terrain/TerrainManager.h"
#include "unit/CircuitUnit.h"
#include "CircuitAI.h"
#include "util/Utils.h"

namespace circuit {

using namespace springai;

CTravelMap::CTravelMap()
		: readyFrame(-1)
		, readyMobileId(-1)
		, readyStart(-RgtVector)
		, pendingFrame(-1)
		, pendingMobileId(-1)
		, pendingStart(-RgtVector)
		, squareSize(SQUARE_SIZE)
{
}

CTravelMap::~CTravelMap()
{
}

void CTravelMap::TakePending()
{
	if (pending == nullptr) {
		return;
	}
	if (pending->GetState() == IPathQuery::State::READY) {
		ready = std::move(pending);
		readyFrame = pendingFrame;
		readyMobileId = pendingMobileId;
		readyStart = pendingStart;
	} else if (pending->IsLost()) {
		pending = nullptr;
	}
}

void CTravelMap::Refresh(CCircuitAI* circuit, CCircuitUnit* unit, const AIFloat3& pos, int maxFrames, float maxMove)
{
	TakePending();
	if ((pending != nullptr) || (unit == nullptr)) {
		return;
	}
	const int frame = circuit->GetLastFrame();
	const int mobileId = unit->GetCircuitDef()->GetMobileId();
	if ((ready != nullptr) && (frame - readyFrame < maxFrames) && (readyStart.SqDistance2D(pos) <= SQUARE(maxMove))
		&& (readyMobileId == mobileId))
	{
		return;
	}
	AIFloat3 start = pos;
	CTerrainManager::CorrectPosition(start);
	// The raid view and squad threat, as a raid group judges and routes (CRaidTask); threat is a cost, not a wall
	CPathFinder* pathfinder = circuit->GetPathfinder();
	squareSize = pathfinder->GetSquareSize();
	pending = pathfinder->CreateCostMapQuery(unit, circuit->GetThreatMap(), frame, start, false, true, true);
	pendingFrame = frame;
	pendingMobileId = mobileId;
	pendingStart = start;
	pathfinder->RunQuery(pending);  // no callback: read once READY (the unit isn't used after this call)
}

float CTravelMap::GetSec(const AIFloat3& pos, float speed) const
{
	if (ready == nullptr) {
		return -1.f;
	}
	const CQueryCostMap* map = static_cast<const CQueryCostMap*>(ready.get());
	AIFloat3 p = pos;
	CTerrainManager::CorrectPosition(p);
	float cost = map->GetCostAtSquare(p);
	if (cost < 0.f) {
		cost = map->GetCostAt(p, squareSize * 2);  // a building on the square itself: the squares around it
	}
	if (cost < 0.f) {
		return -1.f;
	}
	// MexBoard's conversion: a square costs COST_BASE plus its slope and threat terms
	return cost / (std::max(speed, 1.f) / squareSize * COST_BASE);
}

} // namespace circuit

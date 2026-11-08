/*
 * AttackCells.cpp
 *
 * CAttackTask's firing cells (quota.attack_static_cells): where an attack group shoots a building or a parked
 * mobile from, beyond range. The target pick (FindTarget) and its routes (AttackTask.cpp) use these.
 */

#include "task/fighter/AttackTask.h"
#include "map/ThreatMap.h"
#include "module/MilitaryManager.h"
#include "setup/SetupManager.h"
#include "terrain/TerrainManager.h"
#include "terrain/path/PathFinder.h"
#include "unit/enemy/EnemyUnit.h"
#include "unit/CircuitUnit.h"
#include "CircuitAI.h"
#include "util/Utils.h"

#include <algorithm>
#include <cmath>

namespace circuit {

using namespace springai;

// Parked mobiles get cells only if no faster than this x our slowest unit: faster ones leave before a walk arrives
static constexpr float PARKED_SPEED_MOD = 1.25f;
// quota.attack_static_cells.siege_bonus: for a slow mobile type whose weapon range is at least this x our fire range
static constexpr float SIEGE_RANGE_MOD = 1.2f;
// Firing cells' line-of-sight test aims at half the target's height, within these (elmos)
static constexpr float CELL_TARGET_LIFT_MIN = SQUARE_SIZE;
static constexpr float CELL_TARGET_LIFT_MAX = 32.f;
// A cell target with no safe firing cell is banned for CELL_BAN_FRAMES << level, level up to CELL_BAN_MAX_LEVEL
static constexpr int CELL_BAN_FRAMES = FRAMES_PER_SEC * 6;
static constexpr int CELL_BAN_MAX_LEVEL = 2;
// ... and the ban and its level are dropped this long after the first ban in a row, or when the threat at the target
// changes by more than CELL_BAN_THREAT_CHANGE (fraction)
static constexpr int CELL_BAN_RESET_FRAMES = FRAMES_PER_SEC * 60;
static constexpr float CELL_BAN_THREAT_CHANGE = 0.25f;

bool CAttackTask::GetCellAnchor(const CEnemyInfo* enemy, int frame, AIFloat3& outPos, int& outEpoch) const
{
	const CMilitaryManager::SAttackCells& cfg = GetMilitaryMgr()->GetAttackCells();
	const CCircuitDef* edef = enemy->GetCircuitDef();
	if (!cfg.isOn || (edef == nullptr)) {
		return false;
	}
	if (!edef->IsMobile()) {
		outPos = enemy->GetPos();  // an identified building has no radar error
		outEpoch = 0;  // it never moves (its type, and so this anchor, is only known once seen in LOS)
		return true;
	}
	return (cfg.parkedFrames > 0) && (edef->GetSpeed() <= lowestSpeed * PARKED_SPEED_MOD)
		&& !enemy->IsBeingBuilt()  // a nanoframe sits in its factory, which is the cell target there
		&& enemy->GetParkedSpot(frame, cfg.parkedFrames, outPos, outEpoch);
}

bool CAttackTask::IsSameAnchor(const CEnemyInfo* enemy, int epoch, int frame) const
{
	AIFloat3 pos;
	int now;
	return (enemy != nullptr) && GetCellAnchor(enemy, frame, pos, now) && (now == epoch);
}

AIFloat3 CAttackTask::GetRangePos(const CEnemyInfo* enemy, int frame) const
{
	AIFloat3 anchor;
	int epoch;
	if (!enemy->IsInLOS() && GetCellAnchor(enemy, frame, anchor, epoch)) {
		return anchor;
	}
	return enemy->GetPos();
}

bool CAttackTask::IsSiegeTarget(const CEnemyInfo* enemy, int frame) const
{
	// A slow mobile type that outranges us on the ground (artillery). By type, parked or not: it doesn't come and go
	// with each hop, which would swap targets. Only while in view or just out of it, not at a spot it was last seen
	// on minutes ago
	const CMilitaryManager::SAttackCells& cfg = GetMilitaryMgr()->GetAttackCells();
	const CCircuitDef* edef = enemy->GetCircuitDef();
	return cfg.isOn && (cfg.siegeBonus > 1.f) && (edef != nullptr) && edef->IsMobile() && edef->HasAntiLand()
		&& (fireRangeLand > 0.f) && (edef->GetMaxRange(CCircuitDef::RangeType::LAND) >= SIEGE_RANGE_MOD * fireRangeLand)
		&& (edef->GetSpeed() <= lowestSpeed * PARKED_SPEED_MOD)
		&& enemy->IsRecentlySeen(frame, cfg.parkedFrames);
}

bool CAttackTask::FillFiringCells(const CEnemyInfo* enemy, const AIFloat3& anchor, float power, float ref,
		bool nearOnly, std::vector<SFiringCell>& outCells) const
{
	// Points on two rings around the anchor, the outer first (the inner only if the outer yields no cell), and on
	// each the sectors facing us first (the far ones only if fewer than 2 near sectors yield a cell, and never if
	// nearOnly). A point is a cell if we can reach it, its threat is below the allowance (the pick's power and
	// reference) and it sees the target. Each sector keeps its best-scoring point; cells scoring over score_slack
	// worse than the best one are left out
	outCells.clear();
	const CMilitaryManager::SAttackCells& cfg = GetMilitaryMgr()->GetAttackCells();
	CCircuitAI* circuit = manager->GetCircuit();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	const AIFloat3& tPos = anchor;
	const AIFloat3& lPos = leader->GetPos(circuit->GetLastFrame());
	const float fireRange = (tPos.y < -SQUARE_SIZE * 5) ? fireRangeWater : fireRangeLand;
	if (fireRange < SQUARE_SIZE * 4) {
		return true;  // melee group: as close as it gets (a disc)
	}
	const AIFloat3 toUs = lPos - tPos;
	const float bearingUs = (toUs.Length2D() > 1.f) ? atan2f(toUs.z, toUs.x) : 0.f;
	auto pointAt = [&tPos](float angle, float radius) {
		AIFloat3 p(tPos.x + radius * cosf(angle), tPos.y, tPos.z + radius * sinf(angle));
		CTerrainManager::CorrectPosition(p);
		return p;
	};
	// Fast path: undefended (no threat at it, nor at the outer ring point facing us): a disc
	if ((GetTaskThreatAt(leader, tPos) <= THREAT_MIN)
		&& (GetTaskThreatAt(leader, pointAt(bearingUs, fireRange * cfg.rings[0])) <= THREAT_MIN))
	{
		return true;
	}

	// Sectors facing us: the sector's middle within 90 degrees of our bearing
	const int sectors = cfg.sectors;
	const int angles = cfg.angles;
	const float twoPi = 2.f * static_cast<float>(M_PI);
	auto sectorOf = [sectors, angles](int i) { return i * sectors / angles; };
	static std::vector<char> isNear;  // by sector; NOTE: micro-opt
	isNear.resize(sectors);
	for (int sec = 0; sec < sectors; ++sec) {
		isNear[sec] = (utils::angle_diff((sec + 0.5f) * twoPi / sectors, bearingUs) <= static_cast<float>(M_PI) * 0.5f);
	}

	const int sqSize = circuit->GetPathfinder()->GetSquareSize();
	const int maxX = terrainMgr->GetSectorXSize() - 1;
	const int maxZ = terrainMgr->GetSectorZSize() - 1;
	auto toPathXY = [sqSize, maxX, maxZ](const AIFloat3& p) {
		return int2(utils::clamp(int(p.x) / sqSize, 0, maxX), utils::clamp(int(p.z) / sqSize, 0, maxZ));
	};
	const int2 targetXY = toPathXY(tPos);
	// Aimed from cell_aim_height above a cell at half the target's height (a tall strider shows over a low ridge)
	const float targetLift = utils::clamp(enemy->GetCircuitDef()->GetHeight() * 0.5f,
			CELL_TARGET_LIFT_MIN, CELL_TARGET_LIFT_MAX);
	const NSMicroPather::TestFunc hitTest = ISquadTask::MakeHitTest(terrainMgr, cfg.aimHeight, targetLift);
	STerrainMapArea* area = leader->GetArea();
	const float strength = std::max(power, THREAT_MIN);
	constexpr float NO_SCORE = std::numeric_limits<float>::max();
	struct SBest {
		AIFloat3 pos;
		float score;  // NO_SCORE: no cell in this sector
	};
	static std::vector<SBest> best;  // by sector; NOTE: micro-opt

	bool isReachable = false;  // some point is reachable (none: an island, plateau or far shore)
	bool isThreatOk = false;  // some reachable point is below the allowance (no line of sight is not a ban)
	// Point i of the ring at radius: a cell if reachable, below the allowance and in sight of the target; it then
	// replaces its sector's best if it scores better. true: the first cell of its sector
	auto tryPoint = [&](int i, float radius) {
		const AIFloat3 p = pointAt(i * twoPi / angles, radius);
		if (!terrainMgr->CanMoveToPos(area, p)) {
			return false;
		}
		isReachable = true;
		const float threat = GetTaskThreatAt(leader, p);
		if ((threat > 0.f) && (threat >= GetAllowedThreatAt(p, power, ref))) {  // the allowance is always above 0
			return false;
		}
		isThreatOk = true;
		if (!hitTest(toPathXY(p), targetXY)) {  // the costly test last
			return false;
		}
		const float score = cfg.threatWeight * std::max(threat, 0.f) / strength + p.distance2D(lPos) / fireRange;
		SBest& b = best[sectorOf(i)];
		const bool isFirst = (b.score == NO_SCORE);
		if (score < b.score) {
			b = {p, score};
		}
		return isFirst;
	};
	for (const float ringMod : cfg.rings) {
		const float radius = fireRange * ringMod;
		best.assign(sectors, {AIFloat3(), NO_SCORE});
		// The near sectors' points first; the far ones' only if fewer than 2 near sectors yield a cell
		int nearCells = 0;
		for (int i = 0; i < angles; ++i) {
			if (isNear[sectorOf(i)] && tryPoint(i, radius)) {
				++nearCells;
			}
		}
		if (!nearOnly && (nearCells < 2)) {
			for (int i = 0; i < angles; ++i) {
				if (!isNear[sectorOf(i)]) {
					tryPoint(i, radius);
				}
			}
		}
		// Every sector's best within score_slack of the best one
		float bestScore = NO_SCORE;
		for (const SBest& b : best) {
			bestScore = std::min(bestScore, b.score);
		}
		if (bestScore < NO_SCORE) {
			for (int sec = 0; sec < sectors; ++sec) {
				if (best[sec].score <= bestScore + cfg.scoreSlack) {
					outCells.push_back({best[sec].pos, sec});
				}
			}
			return true;
		}
	}
	// No cell. Nothing reachable (a disc route may still reach ground in range, e.g. a shore) or no line of sight (a
	// pessimistic test): a disc. Too hot everywhere we can reach: ban
	return !isReachable || isThreatOk;
}

bool CAttackTask::CollectFiringCells(const CEnemyInfo* enemy, const AIFloat3& anchor, int epoch, float power,
		float ref, const AIFloat3* planned, int frame, std::vector<SFiringCell>& outCells)
{
	// Judged at the pick power, as their route's approach will be (not the keep allowance). A parked mobile: near
	// sectors only
	const bool isMobile = enemy->GetCircuitDef()->IsMobile();
	if (!FillFiringCells(enemy, anchor, power, ref, isMobile, outCells) && (planned == nullptr)) {
		BanCell(enemy, epoch, frame);
		return false;
	}
	// Sides whose walk in was unsafe stay out for a while (RejectGoal); all of them: so does the target. The planned
	// cell replaces its sector's fresh one, so losses or a re-route don't move the spot we walk to
	const ICoreUnit::Id id = enemy->GetId();
	const int plannedSector = (planned != nullptr) ? lastRoute.sector : -1;
	const bool hadCells = !outCells.empty();
	outCells.erase(std::remove_if(outCells.begin(), outCells.end(), [&](const SFiringCell& cell) {
		return (cell.sector == plannedSector) || IsRejected(id, cell.sector, epoch, frame);
	}), outCells.end());
	if (planned != nullptr) {
		outCells.push_back({*planned, plannedSector});
	}
	return !hadCells || !outCells.empty();
}

bool CAttackTask::GetPlannedCell(ICoreUnit::Id targetId, float strength, float refSqDist, AIFloat3& outCell) const
{
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	if (!lastRoute.IsCell() || (lastRoute.targetId != targetId)
		|| !IsSameAnchor(circuit->GetEnemyInfo(targetId), lastRoute.epoch, frame)  // moved, or out of view
		|| IsRejected(targetId, lastRoute.sector, lastRoute.epoch, frame)
		|| IsDangerAt(leader, lastRoute.cell, strength, refSqDist))
	{
		return false;
	}
	outCell = lastRoute.cell;
	return true;
}

bool CAttackTask::GetFireArcBearing(float& outBearing) const
{
	if (target == nullptr) {
		return false;
	}
	const float strength = GetKeepStrength(target->GetPos(), GetPickStrength(manager->GetCircuit()->GetLastFrame()));
	AIFloat3 cell;
	if (!GetPlannedCell(target->GetId(), strength, scaleRefSqDist, cell)) {
		return false;
	}
	const AIFloat3& tPos = lastRoute.goal;  // a cell route's goal is its anchor (KeepRoute)
	outBearing = atan2f(cell.z - tPos.z, cell.x - tPos.x);
	return true;
}

bool CAttackTask::IsCellBanned(ICoreUnit::Id id, int frame) const
{
	auto it = cellBans.find(id);
	return (it != cellBans.end()) && (frame < it->second.until);
}

void CAttackTask::BanCell(const CEnemyInfo* enemy, int epoch, int frame)
{
	// At its range position: a radar-only parked mobile's wobble would change the threat read and drop the ban
	const float threat = GetTaskThreatAt(leader, GetRangePos(enemy, frame));
	auto it = cellBans.find(enemy->GetId());
	if (it == cellBans.end()) {
		cellBans[enemy->GetId()] = {frame + CELL_BAN_FRAMES, frame, threat, 0, epoch};
		return;
	}
	SCellBan& ban = it->second;  // banned again with nothing changed: longer
	ban.level = std::min(ban.level + 1, CELL_BAN_MAX_LEVEL);
	ban.until = frame + (CELL_BAN_FRAMES << ban.level);
	ban.threat = threat;
}

void CAttackTask::UpdateCellBans(int frame)
{
	CCircuitAI* circuit = manager->GetCircuit();
	for (auto it = cellBans.begin(); it != cellBans.end();) {
		const SCellBan& ban = it->second;
		const CEnemyInfo* enemy = circuit->GetEnemyInfo(it->first);
		bool isDrop = (frame >= ban.since + CELL_BAN_RESET_FRAMES) || !IsSameAnchor(enemy, ban.epoch, frame);
		if (!isDrop) {
			const float threat = GetTaskThreatAt(leader, GetRangePos(enemy, frame));
			isDrop = fabsf(threat - ban.threat) > CELL_BAN_THREAT_CHANGE * std::max(ban.threat, THREAT_MIN);
		}
		it = isDrop ? cellBans.erase(it) : std::next(it);
	}
}

} // namespace circuit

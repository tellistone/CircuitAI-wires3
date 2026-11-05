/*
 * SquadTask.cpp
 *
 *  Created on: Jan 23, 2016
 *      Author: rlcevg
 */

#include "task/fighter/SquadTask.h"
#include "task/TaskManager.h"
#include "map/InfluenceMap.h"
#include "map/ThreatMap.h"
#include "module/BuilderManager.h"
#include "module/MilitaryManager.h"
#include "setup/SetupManager.h"
#include "terrain/TerrainManager.h"
#include "terrain/path/PathFinder.h"
#include "terrain/path/QueryLineMap.h"
#include "terrain/path/QueryPathSingle.h"
#include "terrain/path/QueryPathMulti.h"
#include "unit/action/TravelAction.h"
#include "unit/enemy/EnemyUnit.h"
#include "unit/CircuitUnit.h"
#include "CircuitAI.h"
#include "util/Utils.h"

#include "spring/SpringMap.h"

#include "AISCommands.h"

namespace circuit {

using namespace springai;

ISquadTask::ISquadTask(ITaskManager* mgr, FightType type, float powerMod)
		: IFighterTask(mgr, type, powerMod)
		, lowestRange(std::numeric_limits<float>::max())
		, highestRange(.0f)
		, lowestSpeed(std::numeric_limits<float>::max())
		, highestSpeed(.0f)
		, leader(nullptr)
		, groupPos(-RgtVector)
		, prevGroupPos(-RgtVector)
		, pPath(std::make_shared<PathInfo>())
		, groupFrame(0)
{
}

ISquadTask::~ISquadTask()
{
}

void ISquadTask::AssignTo(CCircuitUnit* unit)
{
	IFighterTask::AssignTo(unit);

	if (leader == nullptr) {
		lowestRange  = unit->GetCircuitDef()->GetMaxRange();
		highestRange = unit->GetCircuitDef()->GetMaxRange();
		lowestSpeed  = unit->GetCircuitDef()->GetSpeed();
		highestSpeed = unit->GetCircuitDef()->GetSpeed();
		leader = unit;
	} else {
		lowestRange  = std::min(lowestRange,  unit->GetCircuitDef()->GetMaxRange());
		highestRange = std::max(highestRange, unit->GetCircuitDef()->GetMaxRange());
		lowestSpeed  = std::min(lowestSpeed,  unit->GetCircuitDef()->GetSpeed());
		highestSpeed = std::max(highestSpeed, unit->GetCircuitDef()->GetSpeed());
		if (unit->GetCircuitDef()->IsRoleSupport()) {
			return;
		}
		if ((leader->GetArea() == nullptr) ||
			leader->GetCircuitDef()->IsRoleSupport() ||
			((unit->GetArea() != nullptr) && (unit->GetArea()->percentOfMap < leader->GetArea()->percentOfMap)))
		{
			leader = unit;
		}
	}
}

void ISquadTask::RemoveAssignee(CCircuitUnit* unit)
{
	IFighterTask::RemoveAssignee(unit);
	leader = nullptr;
	lowestRange = lowestSpeed = std::numeric_limits<float>::max();
	highestRange = highestSpeed = .0f;
	if (units.empty()) {
		return;
	}

	FindLeader(units.begin(), units.end());
}

void ISquadTask::Merge(ISquadTask* task)
{
	const std::set<CCircuitUnit*>& rookies = task->GetAssignees();
	IAction::State state = leader->GetTravelAct()->GetState();
	const std::shared_ptr<PathInfo>& lPath = leader->GetTravelAct()->GetPath();
	for (CCircuitUnit* unit : rookies) {
		unit->SetTask(this);
		if (unit->GetCircuitDef()->IsRoleSupport()) {
			continue;
		}
		unit->GetTravelAct()->SetPath(lPath);
		unit->GetTravelAct()->SetState(state);
	}
	units.insert(rookies.begin(), rookies.end());
	attackPower += task->GetAttackPower();
	const std::set<CCircuitUnit*>& sh = task->GetShields();
	shields.insert(sh.begin(), sh.end());

	FindLeader(rookies.begin(), rookies.end());
}

const AIFloat3& ISquadTask::GetLeaderPos(int frame) const
{
	return (leader != nullptr) ? leader->GetPos(frame) : GetPosition();
}

void ISquadTask::FindLeader(decltype(units)::iterator itBegin, decltype(units)::iterator itEnd)
{
	if (leader == nullptr) {
		for (; itBegin != itEnd; ++itBegin) {
			CCircuitUnit* ass = *itBegin;
			lowestRange  = std::min(lowestRange,  ass->GetCircuitDef()->GetMaxRange());
			highestRange = std::max(highestRange, ass->GetCircuitDef()->GetMaxRange());
			lowestSpeed  = std::min(lowestSpeed,  ass->GetCircuitDef()->GetSpeed());
			highestSpeed = std::max(highestSpeed, ass->GetCircuitDef()->GetSpeed());
			if (!ass->GetCircuitDef()->IsRoleSupport()) {
				leader = ass;
				++itBegin;
				break;
			}
		}
	}
	for (; itBegin != itEnd; ++itBegin) {
		CCircuitUnit* ass = *itBegin;
		lowestRange  = std::min(lowestRange,  ass->GetCircuitDef()->GetMaxRange());
		highestRange = std::max(highestRange, ass->GetCircuitDef()->GetMaxRange());
		lowestSpeed  = std::min(lowestSpeed,  ass->GetCircuitDef()->GetSpeed());
		highestSpeed = std::max(highestSpeed, ass->GetCircuitDef()->GetSpeed());
		if (ass->GetCircuitDef()->IsRoleSupport() || (ass->GetArea() == nullptr)) {
			continue;
		}
		if ((leader->GetArea() == nullptr) ||
			leader->GetCircuitDef()->IsRoleSupport() ||
			(ass->GetArea()->percentOfMap < leader->GetArea()->percentOfMap))
		{
			leader = ass;
		}
	}
}

bool ISquadTask::IsMergeSafe() const
{
	CCircuitAI* circuit = manager->GetCircuit();
	const AIFloat3& pos = leader->GetPos(circuit->GetLastFrame());
	return (circuit->GetInflMap()->GetInfluenceAt(pos) > -INFL_EPS);
}

ISquadTask* ISquadTask::CheckMergeTask()
{
	const ISquadTask* task = nullptr;

	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	const AIFloat3& pos = leader->GetPos(frame);
	STerrainMapArea* area = leader->GetArea();
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();
	const float sqMaxDistCost = SQUARE(MAX_TRAVEL_SEC * lowestSpeed);
	float metric = std::numeric_limits<float>::max();

	const std::set<IFighterTask*>& tasks = static_cast<CMilitaryManager*>(manager)->GetTasks(fightType);
	for (const IFighterTask* candidate : tasks) {
		if ((candidate == this) ||
			(candidate->GetAttackPower() < attackPower) ||
			!candidate->CanAssignTo(leader))
		{
			continue;
		}
		const ISquadTask* candy = static_cast<const ISquadTask*>(candidate);

		const AIFloat3& tp = candy->GetLeaderPos(frame);
		const AIFloat3& taskPos = utils::is_valid(tp) ? tp : pos;

		if (!terrainMgr->CanMoveToPos(area, taskPos)) {  // ensure that path always exists
			continue;
		}

		// Check time-distance to target
		float sqDistCost = pos.SqDistance2D(taskPos);
		if ((sqDistCost >= metric) || (sqDistCost >= sqMaxDistCost)) {
			continue;
		}

		if (!IsSafeMergeLine(candy)) {  // ensure safe passage
			continue;
		}
		task = candy;
		metric = sqDistCost;
	}

	return const_cast<ISquadTask*>(task);
}

bool ISquadTask::IsSafeMergeLine(const ISquadTask* host) const
{
	// Default: the straight line is walkable and has no threat at all
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	const AIFloat3& pos = leader->GetPos(frame);
	const AIFloat3& tp = host->GetLeaderPos(frame);
	CPathFinder* pathfinder = circuit->GetPathfinder();
	std::shared_ptr<CQueryLineMap> query = std::static_pointer_cast<CQueryLineMap>(
			pathfinder->CreateLineMapQuery(leader, circuit->GetThreatMap(), frame));
	return query->IsSafeLine(pos, utils::is_valid(tp) ? tp : pos);
}

ISquadTask* ISquadTask::GetMergeTask()
{
	if (updCount % 32 == 1) {
		return IsMergeSafe() ? CheckMergeTask() : nullptr;
	}
	return nullptr;
}

bool ISquadTask::IsMustRegroup()
{
	if ((State::ENGAGE == state) || (updCount % 16 != 15)) {
		return false;
	}

	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	if (circuit->GetInflMap()->GetInfluenceAt(leader->GetPos(frame)) < -INFL_EPS) {
		state = State::ROAM;
		return false;
	}

	static std::vector<CCircuitUnit*> validUnits;  // NOTE: micro-opt
	static std::vector<CCircuitUnit*> stuckUnits;  // released after the loops over units
//	validUnits.reserve(units.size());
	CTerrainManager* terrainMgr = circuit->GetTerrainManager();;
	for (CCircuitUnit* unit : units) {
		if (!unit->GetCircuitDef()->IsPlane() &&
			terrainMgr->CanMoveToPos(unit->GetArea(), unit->GetPos(frame)))
		{
			validUnits.push_back(unit);
		}
	}
	if (validUnits.empty()) {
		state = State::ROAM;
		return false;
	}

	if (State::REGROUP != state) {
		groupPos = leader->GetPos(frame);
		groupFrame = frame;
	} else if (frame >= groupFrame + FRAMES_PER_SEC * 60) {
		// eliminate buggy units
		const float sqMaxDist = SQUARE(std::max<float>(SQUARE_SIZE * 8 * validUnits.size(), highestRange));
		for (CCircuitUnit* unit : units) {
			if (unit->GetCircuitDef()->IsPlane()) {
				continue;
			}
			const AIFloat3& pos = unit->GetPos(frame);
			const float sqDist = groupPos.SqDistance2D(pos);
			if ((sqDist > sqMaxDist) &&
				((unit->GetTaskFrame() < groupFrame) || !terrainMgr->CanMoveToPos(unit->GetArea(), pos)))
			{
				stuckUnits.push_back(unit);
			}
		}

		validUnits.clear();
		state = State::ROAM;
		ReleaseStuck(stuckUnits);  // may leave leader == nullptr (task aborted): callers check
		return false;
	}

	bool wasRegroup = (State::REGROUP == state);
	state = State::ROAM;
	const float sqMaxDist = SQUARE(std::max<float>(SQUARE_SIZE * 8 * validUnits.size(), highestRange));
	for (CCircuitUnit* unit : validUnits) {
		const float sqDist = groupPos.SqDistance2D(unit->GetPos(frame));
		if (sqDist > sqMaxDist) {
			state = State::REGROUP;
			break;
		}
	}

	if (!wasRegroup && (State::REGROUP == state)) {
		if (utils::is_equal_pos(prevGroupPos, groupPos)) {
			stuckUnits.push_back(leader);
		}
		prevGroupPos = groupPos;
	}

	validUnits.clear();
	ReleaseStuck(stuckUnits);  // may leave leader == nullptr (task aborted): callers check
	return (leader != nullptr) && (State::REGROUP == state);
}

void ISquadTask::ReleaseStuck(std::vector<CCircuitUnit*>& stuck)
{
	// A unit that looks stuck leaves the squad and goes back to the idle pool, which gives it a new task
	// like a fresh unit (it used to be dropped from the AI for good, left idle where it stood)
	CCircuitAI* circuit = manager->GetCircuit();
	for (CCircuitUnit* unit : stuck) {
		TRY_UNIT(circuit, unit,
			unit->GetUnit()->Stop();
		)
		RemoveAssignee(unit);  // to the idle task; a squad left without units aborts
	}
	stuck.clear();
}

void ISquadTask::ActivePath(float speed)
{
	for (CCircuitUnit* unit : units) {
		unit->GetTravelAct()->SetPath(pPath, speed);
		unit->GetTravelAct()->StateActivate();
	}
}

NSMicroPather::TestFunc ISquadTask::GetHitTest() const
{
	CTerrainManager* terrainMgr = manager->GetCircuit()->GetTerrainManager();
	const std::vector<STerrainMapSector>& sectors = terrainMgr->GetAreaData()->sector;
	const int sectorXSize = terrainMgr->GetSectorXSize();
	const float aimLift = leader->GetCircuitDef()->GetHeight() / 2;  // TODO: Use aim-pos of attacker and enemy
	return [&sectors, sectorXSize, aimLift](int2 start, int2 end) {  // losTest
		float startHeight = sectors[start.y * sectorXSize + start.x].maxElevation + aimLift;
		float diffHeight = sectors[end.y * sectorXSize + end.x].maxElevation + SQUARE_SIZE - startHeight;
		// All octant line draw
		const int dx =  abs(end.x - start.x), sx = start.x < end.x ? 1 : -1;
		const int dy = -abs(end.y - start.y), sy = start.y < end.y ? 1 : -1;
		int err = dx + dy;  // error value e_xy
		for (int x = start.x, y = start.y;;) {
			const int e2 = 2 * err;
			if (e2 >= dy) {  // e_xy + e_x > 0
				if (x == end.x) break;
				err += dy; x += sx;
			}
			if (e2 <= dx) {  // e_xy + e_y < 0
				if (y == end.y) break;
				err += dx; y += sy;
			}

			const float t = fabs((dx > -dy) ? float(x - start.x) / dx : float(y - start.y) / dy);
			if (sectors[y * sectorXSize + x].maxElevation > diffHeight * t + startHeight) {
				return false;
			}
		}
		return true;
	};
}

bool ISquadTask::IsInDanger(int frame, float fleeShare)
{
	isPathAheadUnsafe = false;
	endangered.clear();
	CThreatMap* threatMap = manager->GetCircuit()->GetThreatMap();
	const float safeThreat = GetEscapeSafeThreat();
	float dangerPower = 0.f;
	for (const SUnitStrength& s : GetUnitStrengths()) {
		const AIFloat3& pos = s.unit->GetPos(frame);
		// Cloaked: unseen, enemies in range can't shoot it (asked last: an engine call)
		if ((threatMap->GetThreatAt(s.unit, pos) >= GetAllowedThreatAt(pos, safeThreat))
			&& !s.unit->GetUnit()->IsCloaked())
		{
			endangered.push_back(s.unit);
			dangerPower += s.power;
		}
	}
	if (!endangered.empty() && (dangerPower >= fleeShare * GetStrengthSum())) {
		return true;
	}

	if (!leader->GetTravelAct()->IsActive()) {
		return false;
	}
	const std::shared_ptr<PathInfo>& path = leader->GetTravelAct()->GetPath();
	if (path == nullptr) {
		return false;
	}
	// The stretch of path the group covers in the next ~2s
	const AIFloat3& pos = leader->GetPos(frame);
	const float lookAhead = std::max(leader->GetCircuitDef()->GetSpeed() * 2.f, 200.f);
	const float sqLookAhead = SQUARE(lookAhead);
	for (const AIFloat3& p : path->posPath) {
		if ((p.SqDistance2D(pos) < sqLookAhead) && (threatMap->GetThreatAt(leader, p) >= GetAllowedThreatAt(p, safeThreat))) {
			isPathAheadUnsafe = !leader->GetUnit()->IsCloaked();  // a cloaked leader walks through unseen
			break;
		}
	}
	return false;
}

void ISquadTask::PullBackEndangered(int frame)
{
	if (endangered.empty() || (frame < pullFrame + FRAMES_PER_SEC)) {
		return;
	}
	AIFloat3 safePos(ZeroVector);
	float safePower = 0.f;
	for (const SUnitStrength& s : GetUnitStrengths()) {
		if (std::find(endangered.begin(), endangered.end(), s.unit) != endangered.end()) {
			continue;
		}
		const float power = std::max(s.power, 1e-3f);
		safePos += s.unit->GetPos(frame) * power;
		safePower += power;
	}
	if (safePower <= 0.f) {
		return;  // nobody is safe (cloaked-only remainder etc.): nothing to fall back to
	}
	safePos /= safePower;
	CTerrainManager::CorrectPosition(safePos);
	pullFrame = frame;

	CCircuitAI* circuit = manager->GetCircuit();
	for (CCircuitUnit* unit : endangered) {
		if ((unit->Blocker() != nullptr) || IsOnRiskySpot(unit, frame)) {
			continue;  // busy, or just sent to the least dangerous firing spot: don't bounce it back
		}
		unit->GetTravelAct()->StateWait();  // re-activated with the group's next path
		TRY_UNIT(circuit, unit,
			unit->CmdMoveTo(safePos, UNIT_COMMAND_OPTION_RIGHT_MOUSE_KEY, frame + FRAMES_PER_SEC * 60);
			unit->CmdWantedSpeed(NO_SPEED_LIMIT);
		)
	}
}

/*
 * Threat-capped paths and the squad flee
 */

static constexpr float UNKNOWN_AIR_HEIGHT = 50.f;  // an unidentified blip this far above ground counts as a plane
static constexpr float BLIP_POS_ERROR = 96.f;  // engine default radar error size (ZK keeps it): sideways shift of a blip

bool ISquadTask::IsHighBlip(CMap* map, const AIFloat3& p)
{
	// The blip is shifted sideways by up to BLIP_POS_ERROR, so it must clear the highest ground around it, else a
	// ground unit near a cliff reads as flying. Each read can only make the answer "no", so stop at the first that
	// does: a blip near sea level needs no read, most ground blips one
	if (p.y < UNKNOWN_AIR_HEIGHT) {
		return false;  // not above the water surface: a ship or hover floats at 0 over a deep sea floor
	}
	const float maxX = CTerrainManager::GetTerrainWidth() - 1.f;
	const float maxZ = CTerrainManager::GetTerrainHeight() - 1.f;
	const float offsets[][2] = {{0.f, 0.f}, {1.f, 0.f}, {-1.f, 0.f}, {0.f, 1.f}, {0.f, -1.f},
								{0.7f, 0.7f}, {0.7f, -0.7f}, {-0.7f, 0.7f}, {-0.7f, -0.7f}};
	for (const auto& o : offsets) {
		const float x = std::min(std::max(p.x + o[0] * BLIP_POS_ERROR, 0.f), maxX);
		const float z = std::min(std::max(p.z + o[1] * BLIP_POS_ERROR, 0.f), maxZ);
		if (p.y - map->GetElevationAt(x, z) < UNKNOWN_AIR_HEIGHT) {
			return false;
		}
	}
	return true;
}

float ISquadTask::GetSafeThreat() const
{
	return std::max(GetStrengthSum(), THREAT_MIN) * powerMod;
}

float ISquadTask::GetPathThreatCap(CCircuitUnit* unit, const AIFloat3& startPos, const F3Vec& goals,
		float extraThreat) const
{
	// The pathfinder skips cells hotter than this. One value for the whole route: the squad's strength with
	// the strictest thr_mod.distance and the loosest base_bonus of start and goals (as attack groups do), times
	// GetPathThreatMod, plus extraThreat. Never below the threat where the unit stands, so a squad already in
	// danger can still walk out through ground no hotter than here.
	const float capMod = GetPathThreatMod();
	if ((capMod < 0.f) || unit->GetUnit()->IsCloaked()) {
		return std::numeric_limits<float>::max();  // off, or cloaked: the pathfinder uses the decloak-range array, a different scale
	}
	CCircuitAI* circuit = manager->GetCircuit();
	CMilitaryManager* militaryMgr = circuit->GetMilitaryManager();
	const AIFloat3& basePos = circuit->GetSetupManager()->GetBasePos();
	float curve = GetThrDistModAt(startPos);
	float bonus = militaryMgr->GetBaseThrBonus(basePos.SqDistance2D(startPos));
	for (const AIFloat3& g : goals) {
		curve = std::max(curve, GetThrDistModAt(g));
		bonus = std::max(bonus, militaryMgr->GetBaseThrBonus(basePos.SqDistance2D(g)));
	}
	const float cap = GetSafeThreat() * bonus / curve * capMod + extraThreat;
	return std::max(cap, circuit->GetThreatMap()->GetThreatAt(unit, startPos) + THREAT_MIN);
}

void ISquadTask::QueryPath(CCircuitUnit* unit, const AIFloat3& goal, float range, bool isCapped,
		std::function<void (const CQueryPathSingle*)>&& apply)
{
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	const AIFloat3& startPos = unit->GetPos(frame);
	const float maxThreat = isCapped ? GetPathThreatCap(unit, startPos, {goal}) : std::numeric_limits<float>::max();

	CPathFinder* pathfinder = circuit->GetPathfinder();
	std::shared_ptr<IPathQuery> query = pathfinder->CreatePathSingleQuery(
			unit, circuit->GetThreatMap(), frame, startPos, goal, range, nullptr, maxThreat);
	pathQueries[unit] = query;
	query->HoldTask(this);

	pathfinder->RunQuery(query, [this, apply = std::move(apply)](const IPathQuery* query) {
		if (this->IsQueryAlive(query)) {
			apply(static_cast<const CQueryPathSingle*>(query));
		}
	});
}

void ISquadTask::QueryPathMulti(const F3Vec& goals, float range, std::function<void (const CQueryPathMulti*)>&& apply,
		float extraThreat)
{
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	const AIFloat3& startPos = leader->GetPos(frame);

	CPathFinder* pathfinder = circuit->GetPathfinder();
	std::shared_ptr<IPathQuery> query = pathfinder->CreatePathMultiQuery(
			leader, circuit->GetThreatMap(), frame, startPos, range, goals, nullptr,
			GetPathThreatCap(leader, startPos, goals, extraThreat));
	pathQueries[leader] = query;
	query->HoldTask(this);

	pathfinder->RunQuery(query, [this, apply = std::move(apply)](const IPathQuery* query) {
		if (this->IsQueryAlive(query)) {
			apply(static_cast<const CQueryPathMulti*>(query));
		}
	});
}

CEnemyInfo* ISquadTask::FindFleeDanger(float margin, bool isGunship, float& dangerThreat) const
{
	// An unidentified blip counts while it moves and sits low
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	const AIFloat3& leaderPos = leader->GetPos(frame);
	float squadRadius = 0.f;  // no unit is further than this from the leader
	for (CCircuitUnit* unit : units) {
		squadRadius = std::max(squadRadius, leaderPos.distance2D(unit->GetPos(frame)));
	}

	CEnemyInfo* danger = nullptr;
	float minSqDist = std::numeric_limits<float>::max();
	dangerThreat = 0.f;
	for (auto& kv : circuit->GetEnemyInfos()) {
		CEnemyInfo* enemy = kv.second;
		float range;  // its ground/water weapon range; 0 for an unidentified blip (the margin only)
		if (!IsGroundDanger(enemy, isGunship, range)) {
			continue;  // can't act or shoot the squad: enemy AA, aircraft, a parked blip, ...
		}
		const AIFloat3& ePos = enemy->GetPos();
		const float reach = range + margin;
		if (leaderPos.SqDistance2D(ePos) > SQUARE(reach + squadRadius)) {
			continue;  // out of reach of every unit
		}
		float sqDist = std::numeric_limits<float>::max();  // to the nearest squad unit
		for (CCircuitUnit* unit : units) {
			sqDist = std::min(sqDist, unit->GetPos(frame).SqDistance2D(ePos));
		}
		if ((sqDist > SQUARE(reach)) || ((enemy->GetCircuitDef() == nullptr) && IsHighBlip(circuit->GetMap(), ePos))) {
			continue;  // out of reach, or a high blip: a plane
		}
		if (IsFleeIgnored(enemy)) {
			continue;
		}
		dangerThreat += enemy->GetThreat();
		if (sqDist < minSqDist) {
			minSqDist = sqDist;
			danger = enemy;
		}
	}
	return danger;
}

bool ISquadTask::IsFleeCovered(const AIFloat3& pos, float enemyThreat, float allyThreat, float allyRadius,
		CCircuitDef::RoleT ownRole) const
{
	// As CArtilleryTask::HasCover: each unit's power with its pwr_mod, scaled by sqrt(health %) as squad strength
	// is. When the army leaves or dies, or the enemy outgrows it, the cover goes.
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	const float sqRadius = SQUARE(allyRadius);
	circuit->UpdateFriendlyUnits();  // drop units that died since the last refresh
	float cover = 0.f;
	for (auto& kv : circuit->GetFriendlyUnits()) {
		CAllyUnit* ally = kv.second;
		CCircuitDef* adef = ally->GetCircuitDef();
		if ((adef == nullptr) || !adef->IsAttacker() || adef->IsAbleToFly()
			|| (adef->IsMobile() ? (adef->GetMainRole() == ownRole) : !IsStaticDefence(adef)))
		{
			continue;
		}
		const AIFloat3& allyPos = ally->GetPos(frame);
		if (utils::is_valid(allyPos) && (allyPos.SqDistance2D(pos) <= sqRadius)) {
			cover += GetCoverPower(ally);
			if (cover - enemyThreat >= allyThreat) {
				return true;
			}
		}
	}
	return false;
}

const AIFloat3* ISquadTask::FindFleeGoal(const AIFloat3& dangerPos)
{
	CCircuitAI* circuit = manager->GetCircuit();
	circuit->GetMilitaryManager()->FillSafePos(leader, fleeSpots);
	const AIFloat3& basePos = circuit->GetSetupManager()->GetBasePos();
	if (circuit->GetTerrainManager()->CanMoveToPos(leader->GetArea(), basePos)) {
		fleeSpots.push_back(basePos);
	}
	return PickFleeSpot(leader->GetPos(circuit->GetLastFrame()), dangerPos, fleeSpots);
}

#ifdef DEBUG_VIS
void ISquadTask::Log()
{
	IFighterTask::Log();

	CCircuitAI* circuit = manager->GetCircuit();
	circuit->LOG("pPath: %i | size: %i | TravelAct: %i", pPath.get(), pPath ? pPath->posPath.size() : 0,
			leader->GetTravelAct()->GetState());
}
#endif

} // namespace circuit

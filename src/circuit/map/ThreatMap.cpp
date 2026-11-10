/*
 * ThreatMap.cpp
 *
 *  Created on: Aug 25, 2015
 *      Author: rlcevg
 */

#include "map/ThreatMap.h"
#include "map/MapManager.h"
#include "setup/SetupManager.h"
#include "task/UnitTask.h"
#include "terrain/TerrainManager.h"
#include "unit/CircuitUnit.h"
#include "CircuitAI.h"
#include "util/Scheduler.h"
#include "util/Utils.h"
#include "json/json.h"

#include "Log.h"
#ifdef DEBUG_VIS
#include "Lua.h"
#endif

//#undef NDEBUG
#include <cassert>
#include <chrono>
#include <cmath>

namespace circuit {

using namespace springai;

#define THREAT_DECAY	1e-2f
#define THREAT_CLOAK	16.0f
#define VEL_EPSILON		1e-2f

CThreatMap::CThreatMap(CMapManager* manager, float decloakRadius)
		: manager(manager)
		, maxCount(0.f)
		, countLogFrame(0)
		, pThreatData(&threatData0)
		, drawAirSquad(nullptr)
		, drawPlain({nullptr, nullptr, nullptr, nullptr})
		, drawRaid({nullptr, nullptr, nullptr, nullptr})
		, isUpdating(false)
{
	CCircuitAI* circuit = manager->GetCircuit();
	areaData = circuit->GetTerrainManager()->GetAreaData();
	squareSize = circuit->GetTerrainManager()->GetConvertStoP();
	width = circuit->GetTerrainManager()->GetSectorXSize();
	height = circuit->GetTerrainManager()->GetSectorZSize();
	mapSize = width * height;

	rangeDefault = (DEFAULT_SLACK * 4) / squareSize;
	distCloak = (decloakRadius + DEFAULT_SLACK) / squareSize;

	const Json::Value& quota = circuit->GetSetupManager()->GetConfig()["quota"];
	// One value for both sides of every squad comparison: allied AIs share this map, whatever their own configs say
	constexpr float defCountPower = 0.025f;
	const Json::Value& qcount = quota.get("group_count_power", defCountPower);
	if (qcount.isNumeric()) {
		countPower = utils::clamp(qcount.asFloat(), 0.f, MAX_COUNT_POWER);  // out of range: CMilitaryManager logs it
	} else {
		circuit->LOG("CONFIG %s: quota.group_count_power must be a number 0..%g, using %g",
				circuit->GetSetupManager()->GetConfigName().c_str(), MAX_COUNT_POWER, defCountPower);
		countPower = defCountPower;
	}
	isCounting = (countPower > 0.f);

	for (SThreatData* data : {&threatData0, &threatData1}) {
		data->airThreat.resize(mapSize, THREAT_BASE);
		data->surfThreat.resize(mapSize, THREAT_BASE);
		data->amphThreat.resize(mapSize, THREAT_BASE);
		data->cloakThreat.resize(mapSize, THREAT_BASE);
		data->shield.resize(mapSize, 0.f);
		// Raid view: always its own layers (the multipliers come from behaviour.json, read after this). With every
		// multiplier at 1 they come out equal to surf and amph (FinishLayers)
		data->raidSurf.resize(mapSize, THREAT_BASE);
		data->raidAmph.resize(mapSize, THREAT_BASE);
		if (isCounting) {  // else no squad layers of their own (SetReadPointers)
			data->airSquad.resize(mapSize, THREAT_BASE);
			data->surfSquad.resize(mapSize, THREAT_BASE);
			data->amphSquad.resize(mapSize, THREAT_BASE);
			data->raidSurfSquad.resize(mapSize, THREAT_BASE);
			data->raidAmphSquad.resize(mapSize, THREAT_BASE);
		}
	}
	SetReadPointers(threatData0);
	drawAirThreat = threatData1.airThreat.data();
	drawPlain.surf = threatData1.surfThreat.data();
	drawPlain.amph = threatData1.amphThreat.data();
	drawRaid.surf = threatData1.raidSurf.data();
	drawRaid.amph = threatData1.raidAmph.data();
	drawCloakThreat = threatData1.cloakThreat.data();
	drawShieldArray = threatData1.shield.data();

	const Json::Value& slack = quota["slack_mod"];
	slackMod.allMod = slack.get("all", 1.f).asFloat();
	slackMod.staticMod = slack.get("static", 1.f).asFloat();
	const Json::Value& speedSlack = slack["speed"];
	slackMod.speedMod = speedSlack.get((unsigned)0, 1.f).asFloat() * DEFAULT_SLACK / squareSize;
	slackMod.speedModMax = speedSlack.get((unsigned)1, 2).asInt() * DEFAULT_SLACK / squareSize;
	constexpr float allowedRange = 2000.f;
	for (CCircuitDef& cdef : circuit->GetCircuitDefs()) {
		float slack = squareSize - 1 + cdef.GetAoe() / 2 + DEFAULT_SLACK * slackMod.allMod;
		if (cdef.IsMobile()) {
			// TODO: slack should also depend on own unit's path update rate
			slack += THREAT_UPDATE_RATE * cdef.GetSpeed() / FRAMES_PER_SEC;
		} else {
			slack += DEFAULT_SLACK * slackMod.staticMod;
		}
		float realRange;
		int range;

		realRange = cdef.GetMaxRange(CCircuitDef::RangeType::AIR);
		range = cdef.HasAntiAir() ? int(realRange + slack) / squareSize + 1 : 0;
		cdef.SetThreatRange(CCircuitDef::ThreatType::AIR, range);

		realRange = cdef.GetMaxRange(CCircuitDef::RangeType::LAND);
		range = (cdef.HasAntiLand() && (realRange <= allowedRange)) ? int(realRange + slack) / squareSize + 1 : 0;
		cdef.SetThreatRange(CCircuitDef::ThreatType::LAND, range);

		realRange = cdef.GetMaxRange(CCircuitDef::RangeType::WATER);
		range = (cdef.HasAntiWater() && (realRange <= allowedRange)) ? int(realRange + slack) / squareSize + 1 : 0;
		cdef.SetThreatRange(CCircuitDef::ThreatType::WATER, range);

		cdef.SetThreatRange(CCircuitDef::ThreatType::CLOAK, GetCloakRange(&cdef));
		cdef.SetThreatRange(CCircuitDef::ThreatType::SHIELD, GetShieldRange(&cdef));
	}
}

CThreatMap::~CThreatMap()
{
#ifdef DEBUG_VIS
	CCircuitAI* circuit = manager->GetCircuit();
	for (const std::pair<Uint32, float*>& win : sdlWindows) {
		circuit->GetDebugDrawer()->DelSDLWindow(win.first);
		delete[] win.second;
	}
#endif
}

void CThreatMap::EnqueueUpdate()
{
//	if (isUpdating) {
//		return;
//	}
	isUpdating = true;

	CCircuitAI* circuit = manager->GetCircuit();
	areaData = circuit->GetTerrainManager()->GetAreaData();

	circuit->GetScheduler()->RunParallelTask(std::make_shared<CGameTask>(&CThreatMap::Update, this),
											 std::make_shared<CGameTask>(&CThreatMap::Apply, this));
}

void CThreatMap::SetEnemyUnitRange(CEnemyUnit* e) const
{
	const CCircuitDef* edef = e->GetCircuitDef();
	assert(edef != nullptr);

	// FIXME: DEBUG  comm's threat value is not based on proper weapons
	if (edef->IsRoleComm() && !e->IsFake()) {
		CCircuitAI* circuit = manager->GetCircuit();
		// TODO: by weapons 1,2 descriptions set proper land/air/water ranges/threats
		float maxRange = 0.f;
		float maxAoe = 0.f;
		for (int num = 1; num < 3; ++num) {
			std::string str = utils::int_to_string(num, "comm_weapon_id_%i");
			int weaponDefId = int(e->GetUnit()->GetRulesParamFloat(str.c_str(), -1));
			if (weaponDefId < 0) {
				continue;
			}

			CWeaponDef* weaponDef = circuit->GetWeaponDef(weaponDefId);
			const float range = weaponDef->GetRange();
			if (maxRange < range) {
				maxRange = range;
				maxAoe = weaponDef->GetAoe();
			}
		}
		const float slack = squareSize - 1 + maxAoe / 2
				+ DEFAULT_SLACK * slackMod.allMod
				+ THREAT_UPDATE_RATE * edef->GetSpeed() / FRAMES_PER_SEC;
		const float mult = e->GetUnit()->GetRulesParamFloat("comm_range_mult", 1.f);
		const float range = int(maxRange * mult + slack) / squareSize + 1;
		e->SetRange(CCircuitDef::ThreatType::AIR, range);
		e->SetRange(CCircuitDef::ThreatType::LAND, range);
		e->SetRange(CCircuitDef::ThreatType::WATER, range);
		e->SetRange(CCircuitDef::ThreatType::CLOAK, edef->GetThreatRange(CCircuitDef::ThreatType::CLOAK));
		e->SetRange(CCircuitDef::ThreatType::SHIELD, edef->GetThreatRange(CCircuitDef::ThreatType::SHIELD));
	} else {
	// FIXME: DEBUG

		for (CCircuitDef::ThreatT tt = 0; tt < static_cast<CCircuitDef::ThreatT>(CCircuitDef::ThreatType::_SIZE_); ++tt) {
			CCircuitDef::ThreatType type = static_cast<CCircuitDef::ThreatType>(tt);
			e->SetRange(type, edef->GetThreatRange(type));
		}
	}
}

void CThreatMap::NewEnemy(CEnemyUnit* e) const
{
	e->SetThreat(e->GetDamage());  // TODO: Randomize
	e->SetRange(CCircuitDef::ThreatType::AIR, rangeDefault);
	e->SetRange(CCircuitDef::ThreatType::LAND, rangeDefault);
	e->SetRange(CCircuitDef::ThreatType::WATER, rangeDefault);
	e->SetRange(CCircuitDef::ThreatType::CLOAK, distCloak);
//	e->SetRange(CCircuitDef::ThreatType::SHIELD, 0);
}

float CThreatMap::GetAllThreatAt(const AIFloat3& position) const
{
	const int index = PosToIndex(position);
//	float air = airThreat[index] - THREAT_BASE;
	float land = surfThreat[index] - THREAT_BASE;
//	float water = amphThreat[index] - THREAT_BASE;
	return /*air + */land/* + water*/;
}

bool CThreatMap::IsRaidView(const CCircuitUnit* unit)
{
	const IUnitTask* task = unit->GetTask();
	return (task != nullptr) && task->IsRaidView();
}

void CThreatMap::SetThreatType(CCircuitUnit* unit)
{
	assert(unit != nullptr);
	threatArray = GetLayer(unit, false, false);  // plain view: MetalManager, CanReachAtSafe etc read it unset
}

float CThreatMap::GetThreatAt(const AIFloat3& position) const
{
	return threatArray[PosToIndex(position)] - THREAT_BASE;
}

float CThreatMap::GetThreatAt(CCircuitUnit* unit, const AIFloat3& position) const
{
	return GetLayer(unit, false)[PosToIndex(position)] - THREAT_BASE;
}

float CThreatMap::GetPlainThreatAt(CCircuitUnit* unit, const AIFloat3& position) const
{
	return GetLayer(unit, false, false)[PosToIndex(position)] - THREAT_BASE;
}

float CThreatMap::GetSquadThreatAt(CCircuitUnit* unit, const AIFloat3& position) const
{
	return GetLayer(unit, true)[PosToIndex(position)] - THREAT_BASE;
}

float CThreatMap::GetSquadThreatExcl(CCircuitUnit* unit, const AIFloat3& position, float exclThreat) const
{
	const int index = PosToIndex(position);
	const float threat = GetLayer(unit, false)[index] - THREAT_BASE;
	if (threat <= exclThreat) {
		return 0.f;  // nothing else there (also: no threat at all)
	}
	// The squad layer is threat x (1 + k x count): take the enemy's threat off and keep the count's factor
	// (the enemy stays in the count: at most k too much)
	const float rest = threat - exclThreat;
	return isCounting ? rest * (GetLayer(unit, true)[index] - THREAT_BASE) / threat : rest;
}

float* CThreatMap::GetLayer(CCircuitUnit* unit, bool isSquad, bool isRaidView) const
{
	assert(unit != nullptr);
	CCircuitDef* cdef = unit->GetCircuitDef();
	if (cdef->IsAbleToFly()) {
		return GetAirLayer(isSquad);
	}
//	if (unit->GetPos(circuit->GetLastFrame()).y < -SQUARE_SIZE * 5) {
	if (cdef->IsAmphibious()) {
		return GetAmphLayer(isRaidView, isSquad);
	}
	return GetSurfLayer(isRaidView, isSquad);
}

float CThreatMap::GetUnitThreat(CCircuitUnit* unit) const
{
	float health = unit->GetUnit()->GetHealth() + unit->GetShieldPower() * SHIELD_MOD;
	return unit->GetDamage() * sqrtf(std::max(health, 0.f));  // / unit->GetUnit()->GetMaxHealth();
}

inline void CThreatMap::PosToXZ(const AIFloat3& pos, int& x, int& z) const
{
	x = (int)pos.x / squareSize;
	z = (int)pos.z / squareSize;
}

inline int CThreatMap::PosToIndex(const AIFloat3& pos) const
{
	assert((pos.x >= 0) && (pos.x < CTerrainManager::GetTerrainWidth()) &&
		   (pos.z >= 0) && (pos.z < CTerrainManager::GetTerrainHeight()));
	int x, z;
	PosToXZ(pos, x, z);
	return z * width + x;
}

inline AIFloat3 CThreatMap::XZToPos(int x, int z) const
{
	AIFloat3 pos;
	pos.z = z * squareSize + squareSize / 2;
	pos.x = x * squareSize + squareSize / 2;
	return pos;
}

void CThreatMap::AddEnemyUnit(const SEnemyData& e)
{
	CCircuitDef* cdef = e.cdef;
	if (cdef == nullptr) {
		AddEnemyUnitAll(e);
		return;
	}

	const int vsl = std::min(int(e.vel.Length2D() * slackMod.speedMod), slackMod.speedModMax);
	const float count = isCounting ? e.GetCountWeight() : 0.f;
	if (cdef->HasAntiAir()) {
		AddEnemyAir(e, vsl, count);
	}
	if (cdef->HasAntiLand() || cdef->HasAntiWater()) {
		auto addAmph = cdef->IsAlwaysHit() ? &CThreatMap::AddEnemyAmphConst : &CThreatMap::AddEnemyAmphGradient;
		(this->*addAmph)(e, vsl, e.threat, count, drawPlain);
		// Raid view: only a type with a raid multiplier is drawn again, as the difference it makes
		// (threat x (raid_thr_mod - 1), count weight raid - plain); FinishLayers adds the plain layers to it
		const float raidMod = cdef->GetRaidThrMod();
		if ((raidMod != 1.f) && (e.threat > 0.f)) {
			const float raidCount = isCounting ? e.GetRaidCountWeight() - count : 0.f;
			(this->*addAmph)(e, vsl, e.threat * (raidMod - 1.f), raidCount, drawRaid);
			++raidModCount;
		}
	}
	AddDecloaker(e);
	// Shields: AddShields, once all enemies are known
}

void CThreatMap::AddEnemyUnitAll(const SEnemyData& e)
{
	// Unknown type (radar blip): might be a constructor or eco unit, so it adds no unit count. No raid multiplier
	// either: the raid view gets it as the plain view does
	AddEnemyAir(e);
	AddEnemyAmphGradient(e, 0, e.threat, 0.f, drawPlain);
	AddDecloaker(e);
}

void CThreatMap::AddEnemyAir(const SEnemyData& e, const int slack, const float count)
{
	int posx, posz;
	PosToXZ(e.pos, posx, posz);

	const float threat = e.threat/* - THREAT_DECAY*/;
	const int range = e.GetRange(CCircuitDef::ThreatType::AIR) + slack;
	const int rangeSq = SQUARE(range);

	// Threat circles are large and often have appendix, decrease it by 1 for micro-optimization
	const int beginX = std::max(int(posx - range + 1),      0);
	const int endX   = std::min(int(posx + range    ),  width);
	const int beginZ = std::max(int(posz - range + 1),      0);
	const int endZ   = std::min(int(posz + range    ), height);

	for (int z = beginZ; z < endZ; ++z) {
		const int dzSq = SQUARE(posz - z);
		for (int x = beginX; x < endX; ++x) {
			const int dxSq = SQUARE(posx - x);
			const int sum = dxSq + dzSq;
			if (sum > rangeSq) {
				continue;
			}

			const int index = z * width + x;
			const float heat = threat * (1.0f - 0.5f * sqrtf(sum) / range);
			// MicroPather cannot deal with negative costs
			// (which may arise due to floating-point drift)
			// nor with zero-cost nodes (see MP::SetMapData,
			// threat is not used as an additive overlay)
			drawAirThreat[index] += heat;
			if (count > 0.f) {
				drawAirSquad[index] += count;
			}
		}
	}
}

void CThreatMap::AddEnemyAmphConst(const SEnemyData& e, const int slack, const float threat, const float count,
		const SDrawLayers& layers)
{
	int posx, posz;
	PosToXZ(e.pos, posx, posz);

	float* drawSurf = layers.surf;
	float* drawAmph = layers.amph;
	float* drawSurfCount = layers.surfSquad;
	float* drawAmphCount = layers.amphSquad;
	const bool isCount = (count != 0.f);  // squad layers only while isCounting: count is 0 otherwise
	const int rangeLand = e.GetRange(CCircuitDef::ThreatType::LAND) + slack;
	const int rangeLandSq = SQUARE(rangeLand);
	const int rangeWater = e.GetRange(CCircuitDef::ThreatType::WATER) + slack;
	const int rangeWaterSq = SQUARE(rangeWater);
	const int range = std::max(rangeLand, rangeWater);
	const std::vector<STerrainMapSector>& sector = areaData->sector;

	const int beginX = std::max(int(posx - range + 1),      0);
	const int endX   = std::min(int(posx + range    ),  width);
	const int beginZ = std::max(int(posz - range + 1),      0);
	const int endZ   = std::min(int(posz + range    ), height);

	for (int z = beginZ; z < endZ; ++z) {
		const int dzSq = SQUARE(posz - z);
		for (int x = beginX; x < endX; ++x) {
			const int dxSq = SQUARE(posx - x);

			const int sum = dxSq + dzSq;
			const int index = z * width + x;
			bool isWaterThreat = (sum <= rangeWaterSq) && sector[index].isWater;
			if (isWaterThreat || ((sum <= rangeLandSq) && (sector[index].position.y >= -SQUARE_SIZE * 5)))
			{
				drawAmph[index] += threat;
				if (isCount) {
					drawAmphCount[index] += count;
				}
			}
			if (isWaterThreat || (sum <= rangeLandSq))
			{
				drawSurf[index] += threat;
				if (isCount) {
					drawSurfCount[index] += count;
				}
			}
		}
	}
}

void CThreatMap::AddEnemyAmphGradient(const SEnemyData& e, const int slack, const float threat, const float count,
		const SDrawLayers& layers)
{
	int posx, posz;
	PosToXZ(e.pos, posx, posz);

	float* drawSurf = layers.surf;
	float* drawAmph = layers.amph;
	float* drawSurfCount = layers.surfSquad;
	float* drawAmphCount = layers.amphSquad;
	const bool isCount = (count != 0.f);  // squad layers only while isCounting: count is 0 otherwise
	const int rangeLand = e.GetRange(CCircuitDef::ThreatType::LAND) + slack;
	const int rangeLandSq = SQUARE(rangeLand);
	const int rangeWater = e.GetRange(CCircuitDef::ThreatType::WATER) + slack;
	const int rangeWaterSq = SQUARE(rangeWater);
	const int range = std::max(rangeLand, rangeWater);
	const std::vector<STerrainMapSector>& sector = areaData->sector;

	const int beginX = std::max(int(posx - range + 1),      0);
	const int endX   = std::min(int(posx + range    ),  width);
	const int beginZ = std::max(int(posz - range + 1),      0);
	const int endZ   = std::min(int(posz + range    ), height);

	for (int z = beginZ; z < endZ; ++z) {
		const int dzSq = SQUARE(posz - z);
		for (int x = beginX; x < endX; ++x) {
			const int dxSq = SQUARE(posx - x);

			// TODO: 1) Draw as LOS. 2) Separate draw rules for artillery, superweapons, instant-hit weapons
			// Arty: center have no/little threat
			// Super: same as arty, or totally ignore its threat
			// Laser: no gradient
			const int sum = dxSq + dzSq;
			const int index = z * width + x;
			const float heat = threat * (1.0f - 0.5f * sqrtf(sum) / range);
			bool isWaterThreat = (sum <= rangeWaterSq) && sector[index].isWater;
			if (isWaterThreat || ((sum <= rangeLandSq) && (sector[index].position.y >= -SQUARE_SIZE * 5)))
			{
				drawAmph[index] += heat;
				if (isCount) {
					drawAmphCount[index] += count;
				}
			}
			if (isWaterThreat || (sum <= rangeLandSq))
			{
				drawSurf[index] += heat;
				if (isCount) {
					drawSurfCount[index] += count;
				}
			}
		}
	}
}

void CThreatMap::AddDecloaker(const SEnemyData& e)
{
	int posx, posz;
	PosToXZ(e.pos, posx, posz);

	const float threatCloak = THREAT_CLOAK;
	const int rangeCloak = e.GetRange(CCircuitDef::ThreatType::CLOAK);
	const int rangeCloakSq = SQUARE(rangeCloak);

	// For small decloak ranges full range shouldn't hit performance
	const int beginX = std::max(int(posx - rangeCloak + 1),      0);
	const int endX   = std::min(int(posx + rangeCloak    ),  width);
	const int beginZ = std::max(int(posz - rangeCloak + 1),      0);
	const int endZ   = std::min(int(posz + rangeCloak    ), height);

	for (int z = beginZ; z < endZ; ++z) {
		const int dzSq = SQUARE(posz - z);
		for (int x = beginX; x < endX; ++x) {
			const int dxSq = SQUARE(posx - x);
			const int sum = dxSq + dzSq;
			if (sum > rangeCloakSq) {
				continue;
			}

			const int index = z * width + x;
			const float heat = threatCloak * (1.0f - 0.75f * sqrtf(sum) / rangeCloak);
			drawCloakThreat[index] += heat;
		}
	}
}

void CThreatMap::AddShields(const std::vector<SEnemyData>& hostiles, const std::vector<SEnemyData>& peaces)
{
	// Each shield's charge is shared by the armed enemies inside it, so that a ball's shields count once in all.
	// Painting the whole charge on every cell it covers counted it again for every unit under it (and a ball of
	// overlapping shields looked several times stronger than it is). Armed: unarmed units (Aspis, cons) add no
	// threat, so a share on them would be lost. A cell is inside when its centre is within the real shield radius
	// (or it is the shield's own cell); the units are counted by the same test, so the shares add up to the charge.
	std::vector<int> armedCells;  // cell of each armed enemy that a shield can cover
	armedCells.reserve(hostiles.size());
	for (const SEnemyData& e : hostiles) {
		if ((e.cdef == nullptr) || e.isBeingBuilt || e.IsFake()) {
			continue;
		}
		int x, z;
		PosToXZ(e.pos, x, z);
		if ((x >= 0) && (x < width) && (z >= 0) && (z < height)) {
			armedCells.push_back(z * width + x);
		}
	}
	if (armedCells.empty()) {
		return;
	}
	for (const std::vector<SEnemyData>* enemies : {&hostiles, &peaces}) {
		for (const SEnemyData& e : *enemies) {
			if ((e.cdef != nullptr) && (e.cdef->GetShieldMount() != nullptr) && !e.isBeingBuilt && !e.IsFake()) {
				AddShield(e, armedCells);
			}
		}
	}
}

void CThreatMap::AddShield(const SEnemyData& e, const std::vector<int>& armedCells)
{
	if (e.shieldPower <= 0.f) {
		return;
	}
	int posx, posz;
	PosToXZ(e.pos, posx, posz);
	const float sqRadius = SQUARE(e.cdef->GetShieldRadius());
	auto isInside = [&](int x, int z) {
		return ((x == posx) && (z == posz)) || (XZToPos(x, z).SqDistance2D(e.pos) <= sqRadius);
	};

	int count = 0;
	for (int index : armedCells) {
		if (isInside(index % width, index / width)) {
			++count;
		}
	}
	if (count == 0) {
		return;  // shields no armed enemy
	}
	const float shieldVal = e.shieldPower / count;

	const int rangeShield = e.cdef->GetThreatRange(CCircuitDef::ThreatType::SHIELD);  // bounds: cells test the radius
	const int beginX = std::max(int(posx - rangeShield + 1),      0);
	const int endX   = std::min(int(posx + rangeShield    ),  width);
	const int beginZ = std::max(int(posz - rangeShield + 1),      0);
	const int endZ   = std::min(int(posz + rangeShield    ), height);

	for (int z = beginZ; z < endZ; ++z) {
		for (int x = beginX; x < endX; ++x) {
			if (isInside(x, z)) {
				drawShieldArray[z * width + x] += shieldVal;
			}
		}
	}
}

int CThreatMap::GetCloakRange(const CCircuitDef* edef) const
{
	const int sizeX = edef->GetDef()->GetXSize() * (SQUARE_SIZE / 2);
	const int sizeZ = edef->GetDef()->GetZSize() * (SQUARE_SIZE / 2);
	int threatRange = distCloak;
	if (edef->IsMobile()) {
		threatRange += (DEFAULT_SLACK * 2) / squareSize;
	}
	return (int)sqrtf(SQUARE(sizeX) + SQUARE(sizeZ)) / squareSize + threatRange;
}

int CThreatMap::GetShieldRange(const CCircuitDef* edef) const
{
	return (edef->GetShieldMount() != nullptr) ? (int)edef->GetShieldRadius() / squareSize + 1 : 0;
}

float CThreatMap::GetEnemyUnitThreat(const CEnemyUnit* e) const
{
	if (e->IsBeingBuilt()) {
		return .0f;  // THREAT_BASE;
	}
	const float health = e->GetHealth();
	if (health <= .0f) {
		return .0f;
	}
	// Its share of the shields over its cell (AddShields), its own shield included
	int x, z;
	PosToXZ(e->GetPos(), x, z);
	return e->GetDamage() * sqrtf(health + shieldArray[z * width + x] * SHIELD_MOD);  // / unit->GetUnit()->GetMaxHealth();
}

void CThreatMap::Prepare(SThreatData& threatData)
{
	std::fill(threatData.airThreat.begin(), threatData.airThreat.end(), THREAT_BASE);
	std::fill(threatData.surfThreat.begin(), threatData.surfThreat.end(), THREAT_BASE);
	std::fill(threatData.amphThreat.begin(), threatData.amphThreat.end(), THREAT_BASE);
	std::fill(threatData.cloakThreat.begin(), threatData.cloakThreat.end(), THREAT_BASE);
	std::fill(threatData.shield.begin(), threatData.shield.end(), 0.f);
	// Raid layers hold differences while drawing (FinishLayers): 0, not THREAT_BASE
	std::fill(threatData.raidSurf.begin(), threatData.raidSurf.end(), 0.f);
	std::fill(threatData.raidAmph.begin(), threatData.raidAmph.end(), 0.f);

	drawAirThreat = threatData.airThreat.data();
	drawPlain.surf = threatData.surfThreat.data();
	drawPlain.amph = threatData.amphThreat.data();
	drawRaid.surf = threatData.raidSurf.data();
	drawRaid.amph = threatData.raidAmph.data();
	drawCloakThreat = threatData.cloakThreat.data();
	drawShieldArray = threatData.shield.data();

	if (isCounting) {
		std::fill(threatData.airSquad.begin(), threatData.airSquad.end(), 0.f);
		std::fill(threatData.surfSquad.begin(), threatData.surfSquad.end(), 0.f);
		std::fill(threatData.amphSquad.begin(), threatData.amphSquad.end(), 0.f);
		std::fill(threatData.raidSurfSquad.begin(), threatData.raidSurfSquad.end(), 0.f);
		std::fill(threatData.raidAmphSquad.begin(), threatData.raidAmphSquad.end(), 0.f);
		drawAirSquad = threatData.airSquad.data();
		drawPlain.surfSquad = threatData.surfSquad.data();
		drawPlain.amphSquad = threatData.amphSquad.data();
		drawRaid.surfSquad = threatData.raidSurfSquad.data();
		drawRaid.amphSquad = threatData.raidAmphSquad.data();
	}
}

void CThreatMap::FinishLayers(SThreatData& threatData)
{
	// Squad layers held the enemy unit count: squad threat = threat x CountMod(count).
	// Raid layers held the difference the enemies with a raid multiplier make, their squad layers the difference in
	// the count: raid = plain + difference (at least THREAT_BASE: float drift), raid squad = raid x CountMod(count +
	// count difference). Every multiplier at 1: the differences are all 0, and the raid layers equal the plain ones
	// bit for bit (x + 0.f is x, and the squad formula is the same expression)
	float maxCnt = 0.f;
	auto countOnly = [this, &maxCnt](const FloatVec& threat, FloatVec& squad) {
		for (size_t i = 0; i < squad.size(); ++i) {
			const float count = squad[i];
			maxCnt = std::max(maxCnt, count);
			squad[i] = THREAT_BASE + (threat[i] - THREAT_BASE) * GetCountMod(count);
		}
	};
	auto withRaid = [this, &maxCnt](const FloatVec& threat, FloatVec& squad, FloatVec& raid, FloatVec& raidSquad) {
		for (size_t i = 0; i < raid.size(); ++i) {
			const float count = squad[i];  // read before it turns into squad threat
			maxCnt = std::max(maxCnt, count);
			squad[i] = THREAT_BASE + (threat[i] - THREAT_BASE) * GetCountMod(count);
			raid[i] = std::max(threat[i] + raid[i], THREAT_BASE);
			raidSquad[i] = THREAT_BASE + (raid[i] - THREAT_BASE) * GetCountMod(std::max(count + raidSquad[i], 0.f));
		}
	};
	auto raidOnly = [](const FloatVec& threat, FloatVec& raid) {
		for (size_t i = 0; i < raid.size(); ++i) {
			raid[i] = std::max(threat[i] + raid[i], THREAT_BASE);
		}
	};
	if (isCounting) {
		countOnly(threatData.airThreat, threatData.airSquad);
		withRaid(threatData.surfThreat, threatData.surfSquad, threatData.raidSurf, threatData.raidSurfSquad);
		withRaid(threatData.amphThreat, threatData.amphSquad, threatData.raidAmph, threatData.raidAmphSquad);
		maxCount = maxCnt;
	} else {
		raidOnly(threatData.surfThreat, threatData.raidSurf);
		raidOnly(threatData.amphThreat, threatData.raidAmph);
	}
}

void CThreatMap::Update()
{
	const auto startTime = std::chrono::steady_clock::now();
	Prepare(*GetNextThreatData());
	raidModCount = 0;

	CEnemyManager* enemyMgr = manager->GetCircuit()->GetEnemyManager();

	for (const SEnemyData& e : enemyMgr->GetHostileDatas()) {
		AddEnemyUnit(e);
	}
	hostileCount = enemyMgr->GetHostileDatas().size();

	for (const SEnemyData& e : enemyMgr->GetPeaceDatas()) {
		AddDecloaker(e);
	}

	AddShields(enemyMgr->GetHostileDatas(), enemyMgr->GetPeaceDatas());

	FinishLayers(*GetNextThreatData());

	const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - startTime).count();
	updateMsSum += ms;
	updateMsMax = std::max(updateMsMax, ms);
	++updateNum;
}

void CThreatMap::Apply()
{
	SwapBuffers();
	isUpdating = false;

	const int frame = manager->GetCircuit()->GetLastFrame();
	if (frame >= countLogFrame + FRAMES_PER_SEC * 60) {
		countLogFrame = frame;
		LogStats();
	}

#ifdef DEBUG_VIS
	UpdateVis();
#endif
}

void CThreatMap::LogStats()
{
	// For tuning quota.group_count_power and raid_thr_mod, once a minute
	CCircuitAI* circuit = manager->GetCircuit();
	if (isCounting) {
		circuit->LOG("THREAT count: up to %.1f enemies can fire at one cell: squad threat up to x%.2f",
				maxCount, GetCountMod(maxCount));
	}
	// The cell where the raid view differs most from the plain one (the read layers: the update just applied)
	int gapIdx = -1;
	float maxGap = 0.f;
	for (int i = 0; i < mapSize; ++i) {
		const float gap = std::fabs(raidSurf[i] - surfThreat[i]);
		if (gap > maxGap) {
			maxGap = gap;
			gapIdx = i;
		}
	}
	const float avgMs = (updateNum > 0) ? updateMsSum / updateNum : 0.f;
	if (gapIdx >= 0) {
		const AIFloat3 gapPos = XZToPos(gapIdx % width, gapIdx / width);
		circuit->LOG("THREAT raid view: %i of %i enemies have a raid multiplier; biggest gap at (%.0f, %.0f):"
				" surface %.2f, raid %.2f | update %.2f ms avg, %.2f ms max (%i updates)",
				raidModCount, hostileCount, gapPos.x, gapPos.z, surfThreat[gapIdx] - THREAT_BASE,
				raidSurf[gapIdx] - THREAT_BASE, avgMs, updateMsMax, updateNum);
	} else {
		circuit->LOG("THREAT raid view: %i of %i enemies have a raid multiplier; same as surface"
				" | update %.2f ms avg, %.2f ms max (%i updates)",
				raidModCount, hostileCount, avgMs, updateMsMax, updateNum);
	}
	updateMsSum = 0.f;
	updateMsMax = 0.f;
	updateNum = 0;
}

void CThreatMap::SwapBuffers()
{
	pThreatData = GetNextThreatData();
	SetReadPointers(*pThreatData.load());
}

void CThreatMap::SetReadPointers(SThreatData& threatData)
{
	airThreat = threatData.airThreat.data();
	surfThreat = threatData.surfThreat.data();
	amphThreat = threatData.amphThreat.data();
	cloakThreat = threatData.cloakThreat.data();
	shieldArray = threatData.shield.data();
	// Not counting: the squad layers are the threat layers (squad threat = threat). Read only: the draw pointers
	// never alias them (Prepare), or drawing the count would add into the threat
	airSquad = isCounting ? threatData.airSquad.data() : airThreat;
	surfSquad = isCounting ? threatData.surfSquad.data() : surfThreat;
	amphSquad = isCounting ? threatData.amphSquad.data() : amphThreat;
	raidSurf = threatData.raidSurf.data();
	raidAmph = threatData.raidAmph.data();
	raidSurfSquad = isCounting ? threatData.raidSurfSquad.data() : raidSurf;
	raidAmphSquad = isCounting ? threatData.raidAmphSquad.data() : raidAmph;
	threatArray = surfThreat;
}

#ifdef DEBUG_VIS
void CThreatMap::UpdateVis()
{
	CCircuitAI* circuit = manager->GetCircuit();
	if (isWidgetDrawing || isWidgetPrinting) {
		std::ostringstream cmd;
		cmd << "ai_thr_data:";
		for (int i = 0; i < mapSize; ++i) {
			cmd << surfThreat[i] << " ";
		}
		std::string s = cmd.str();
		circuit->GetLua()->CallRules(s.c_str(), s.size());
	}

	if (sdlWindows.empty()/* || (currMaxThreat < .1f)*/) {
		return;
	}

	Uint32 sdlWindowId;
	float* dbgMap;
	std::tie(sdlWindowId, dbgMap) = sdlWindows[0];
	for (int i = 0; i < mapSize; ++i) {
		dbgMap[i] = std::min<float>((airThreat[i] - THREAT_BASE) / 40.0f /*currMaxThreat*/, 1.0f);
	}
	circuit->GetDebugDrawer()->DrawMap(sdlWindowId, dbgMap);

	std::tie(sdlWindowId, dbgMap) = sdlWindows[1];
	for (int i = 0; i < mapSize; ++i) {
		dbgMap[i] = std::min<float>((surfThreat[i] - THREAT_BASE) / 40.0f, 1.0f);
	}
	circuit->GetDebugDrawer()->DrawMap(sdlWindowId, dbgMap);

	std::tie(sdlWindowId, dbgMap) = sdlWindows[2];
	for (int i = 0; i < mapSize; ++i) {
		dbgMap[i] = std::min<float>((amphThreat[i] - THREAT_BASE) / 40.0f, 1.0f);
	}
	circuit->GetDebugDrawer()->DrawMap(sdlWindowId, dbgMap);

	std::tie(sdlWindowId, dbgMap) = sdlWindows[3];
	for (int i = 0; i < mapSize; ++i) {
		dbgMap[i] = std::min<float>((cloakThreat[i] - THREAT_BASE) / THREAT_CLOAK, 1.0f);
	}
	circuit->GetDebugDrawer()->DrawMap(sdlWindowId, dbgMap);

	std::tie(sdlWindowId, dbgMap) = sdlWindows[4];
	for (int i = 0; i < mapSize; ++i) {
		dbgMap[i] = std::min<float>((surfSquad[i] - THREAT_BASE) / 40.0f, 1.0f);
	}
	circuit->GetDebugDrawer()->DrawMap(sdlWindowId, dbgMap);

	std::tie(sdlWindowId, dbgMap) = sdlWindows[5];
	for (int i = 0; i < mapSize; ++i) {
		dbgMap[i] = std::min<float>((raidSurf[i] - THREAT_BASE) / 40.0f, 1.0f);
	}
	circuit->GetDebugDrawer()->DrawMap(sdlWindowId, dbgMap);
}

void CThreatMap::ToggleSDLVis()
{
	CCircuitAI* circuit = manager->GetCircuit();
	if (sdlWindows.empty()) {
		// ~threat
		std::pair<Uint32, float*> win;
		std::string label;

		win.second = new float [mapSize];
		label = utils::int_to_string(circuit->GetSkirmishAIId(), "Circuit AI [%i] :: AIR Threat Map");
		win.first = circuit->GetDebugDrawer()->AddSDLWindow(width, height, label.c_str());
		sdlWindows.push_back(win);

		win.second = new float [mapSize];
		label = utils::int_to_string(circuit->GetSkirmishAIId(), "Circuit AI [%i] :: SURFACE Threat Map");
		win.first = circuit->GetDebugDrawer()->AddSDLWindow(width, height, label.c_str());
		sdlWindows.push_back(win);

		win.second = new float [mapSize];
		label = utils::int_to_string(circuit->GetSkirmishAIId(), "Circuit AI [%i] :: AMPHIBIOUS Threat Map");
		win.first = circuit->GetDebugDrawer()->AddSDLWindow(width, height, label.c_str());
		sdlWindows.push_back(win);

		win.second = new float [mapSize];
		label = utils::int_to_string(circuit->GetSkirmishAIId(), "Circuit AI [%i] :: CLOAK Threat Map");
		win.first = circuit->GetDebugDrawer()->AddSDLWindow(width, height, label.c_str());
		sdlWindows.push_back(win);

		win.second = new float [mapSize];
		label = utils::int_to_string(circuit->GetSkirmishAIId(), "Circuit AI [%i] :: SURFACE SQUAD Threat Map");
		win.first = circuit->GetDebugDrawer()->AddSDLWindow(width, height, label.c_str());
		sdlWindows.push_back(win);

		win.second = new float [mapSize];
		label = utils::int_to_string(circuit->GetSkirmishAIId(), "Circuit AI [%i] :: SURFACE RAID Threat Map");
		win.first = circuit->GetDebugDrawer()->AddSDLWindow(width, height, label.c_str());
		sdlWindows.push_back(win);

		UpdateVis();
	} else {
		for (const std::pair<Uint32, float*>& win : sdlWindows) {
			circuit->GetDebugDrawer()->DelSDLWindow(win.first);
			delete[] win.second;
		}
		sdlWindows.clear();
	}
}

void CThreatMap::ToggleWidgetDraw()
{
	CCircuitAI* circuit = manager->GetCircuit();
	std::string cmd("ai_thr_draw:");
	std::string result = circuit->GetLua()->CallRules(cmd.c_str(), cmd.size());

	isWidgetDrawing = (result == "1");
	if (isWidgetDrawing) {
		cmd = utils::int_to_string(squareSize, "ai_thr_size:%i");
		cmd += utils::float_to_string(THREAT_BASE, " %f");
		circuit->GetLua()->CallRules(cmd.c_str(), cmd.size());

		UpdateVis();
	}
}

void CThreatMap::ToggleWidgetPrint()
{
	CCircuitAI* circuit = manager->GetCircuit();
	std::string cmd("ai_thr_print:");
	std::string result = circuit->GetLua()->CallRules(cmd.c_str(), cmd.size());

	isWidgetPrinting = (result == "1");
	if (isWidgetPrinting) {
		cmd = utils::int_to_string(squareSize, "ai_thr_size:%i");
		cmd += utils::float_to_string(THREAT_BASE, " %f");
		circuit->GetLua()->CallRules(cmd.c_str(), cmd.size());

		UpdateVis();
	}
}

void CThreatMap::SetMaxThreat(float maxThreat)
{
	CCircuitAI* circuit = manager->GetCircuit();
	std::string cmd = utils::float_to_string(maxThreat, "ai_thr_div:%f");
	circuit->GetLua()->CallRules(cmd.c_str(), cmd.size());
}
#endif

} // namespace circuit

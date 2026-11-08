/*
 * ThreatMap.cpp
 *
 *  Created on: Aug 25, 2015
 *      Author: rlcevg
 */

#include "map/ThreatMap.h"
#include "map/MapManager.h"
#include "setup/SetupManager.h"
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
		, drawSurfSquad(nullptr)
		, drawAmphSquad(nullptr)
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
		if (isCounting) {  // else no squad layers of their own (SetReadPointers)
			data->airSquad.resize(mapSize, THREAT_BASE);
			data->surfSquad.resize(mapSize, THREAT_BASE);
			data->amphSquad.resize(mapSize, THREAT_BASE);
		}
	}
	SetReadPointers(threatData0);
	drawAirThreat = threatData1.airThreat.data();
	drawSurfThreat = threatData1.surfThreat.data();
	drawAmphThreat = threatData1.amphThreat.data();
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

void CThreatMap::SetThreatType(CCircuitUnit* unit)
{
	assert(unit != nullptr);
	threatArray = GetLayer(unit, false);
}

float CThreatMap::GetThreatAt(const AIFloat3& position) const
{
	return threatArray[PosToIndex(position)] - THREAT_BASE;
}

float CThreatMap::GetThreatAt(CCircuitUnit* unit, const AIFloat3& position) const
{
	return GetLayer(unit, false)[PosToIndex(position)] - THREAT_BASE;
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

float* CThreatMap::GetLayer(CCircuitUnit* unit, bool isSquad) const
{
	assert(unit != nullptr);
	CCircuitDef* cdef = unit->GetCircuitDef();
	if (cdef->IsAbleToFly()) {
		return isSquad ? airSquad : airThreat;
	}
//	if (unit->GetPos(circuit->GetLastFrame()).y < -SQUARE_SIZE * 5) {
	if (cdef->IsAmphibious()) {
		return isSquad ? amphSquad : amphThreat;
	}
	return isSquad ? surfSquad : surfThreat;
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
		cdef->IsAlwaysHit() ? AddEnemyAmphConst(e, vsl, count) : AddEnemyAmphGradient(e, vsl, count);
	}
	AddDecloaker(e);

	if (cdef->GetShieldMount() != nullptr) {
		AddShield(e);
	}
}

void CThreatMap::AddEnemyUnitAll(const SEnemyData& e)
{
	// Unknown type (radar blip): might be a constructor or eco unit, so it adds no unit count
	AddEnemyAir(e);
	AddEnemyAmphGradient(e);
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

void CThreatMap::AddEnemyAmphConst(const SEnemyData& e, const int slack, const float count)
{
	int posx, posz;
	PosToXZ(e.pos, posx, posz);

	const float threat = e.threat/* - THREAT_DECAY*/;
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
				drawAmphThreat[index] += threat;
				if (count > 0.f) {
					drawAmphSquad[index] += count;
				}
			}
			if (isWaterThreat || (sum <= rangeLandSq))
			{
				drawSurfThreat[index] += threat;
				if (count > 0.f) {
					drawSurfSquad[index] += count;
				}
			}
		}
	}
}

void CThreatMap::AddEnemyAmphGradient(const SEnemyData& e, const int slack, const float count)
{
	int posx, posz;
	PosToXZ(e.pos, posx, posz);

	const float threat = e.threat/* - THREAT_DECAY*/;
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
				drawAmphThreat[index] += heat;
				if (count > 0.f) {
					drawAmphSquad[index] += count;
				}
			}
			if (isWaterThreat || (sum <= rangeLandSq))
			{
				drawSurfThreat[index] += heat;
				if (count > 0.f) {
					drawSurfSquad[index] += count;
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

void CThreatMap::AddShield(const SEnemyData& e)
{
	int posx, posz;
	PosToXZ(e.pos, posx, posz);

	const float shieldVal = e.shieldPower;
	const int rangeShield = e.GetRange(CCircuitDef::ThreatType::SHIELD);
	const int rangeShieldSq = SQUARE(rangeShield);

	const int beginX = std::max(int(posx - rangeShield + 1),      0);
	const int endX   = std::min(int(posx + rangeShield    ),  width);
	const int beginZ = std::max(int(posz - rangeShield + 1),      0);
	const int endZ   = std::min(int(posz + rangeShield    ), height);

	for (int z = beginZ; z < endZ; ++z) {
		const int rrz = rangeShieldSq - SQUARE(posz - z);
		for (int x = beginX; x < endX; ++x) {
			if (SQUARE(posx - x) > rrz) {
				continue;
			}
			drawShieldArray[z * width + x] += shieldVal;
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

	drawAirThreat = threatData.airThreat.data();
	drawSurfThreat = threatData.surfThreat.data();
	drawAmphThreat = threatData.amphThreat.data();
	drawCloakThreat = threatData.cloakThreat.data();
	drawShieldArray = threatData.shield.data();

	if (isCounting) {
		std::fill(threatData.airSquad.begin(), threatData.airSquad.end(), 0.f);
		std::fill(threatData.surfSquad.begin(), threatData.surfSquad.end(), 0.f);
		std::fill(threatData.amphSquad.begin(), threatData.amphSquad.end(), 0.f);
		drawAirSquad = threatData.airSquad.data();
		drawSurfSquad = threatData.surfSquad.data();
		drawAmphSquad = threatData.amphSquad.data();
	}
}

void CThreatMap::ApplyCountPower(SThreatData& threatData)
{
	float maxCnt = 0.f;
	auto apply = [this, &maxCnt](const FloatVec& threat, FloatVec& squad) {
		for (size_t i = 0; i < squad.size(); ++i) {
			maxCnt = std::max(maxCnt, squad[i]);
			squad[i] = THREAT_BASE + (threat[i] - THREAT_BASE) * GetCountMod(squad[i]);
		}
	};
	apply(threatData.airThreat, threatData.airSquad);
	apply(threatData.surfThreat, threatData.surfSquad);
	apply(threatData.amphThreat, threatData.amphSquad);
	maxCount = maxCnt;
}

void CThreatMap::Update()
{
	Prepare(*GetNextThreatData());

	CEnemyManager* enemyMgr = manager->GetCircuit()->GetEnemyManager();

	for (const SEnemyData& e : enemyMgr->GetHostileDatas()) {
		AddEnemyUnit(e);
	}

	for (const SEnemyData& e : enemyMgr->GetPeaceDatas()) {
		AddDecloaker(e);
	}

	if (isCounting) {
		ApplyCountPower(*GetNextThreatData());
	}
}

void CThreatMap::Apply()
{
	SwapBuffers();
	isUpdating = false;

	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	if (isCounting && (frame >= countLogFrame + FRAMES_PER_SEC * 60)) {  // for tuning group_count_power
		countLogFrame = frame;
		circuit->LOG("THREAT count: up to %.1f enemies can fire at one cell: squad threat up to x%.2f",
				maxCount, GetCountMod(maxCount));
	}

#ifdef DEBUG_VIS
	UpdateVis();
#endif
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

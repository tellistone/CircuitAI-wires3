/*
 * CircuitDef.cpp
 *
 *  Created on: Dec 9, 2014
 *      Author: rlcevg
 */

#include "unit/CircuitDef.h"
#include "CircuitAI.h"
#include "util/GameAttribute.h"
#include "util/Utils.h"

#include "spring/SpringCallback.h"

#include "Unit.h"
#include "WeaponMount.h"
#include "WeaponDef.h"
#include "Damage.h"
#include "Shield.h"
#include "MoveData.h"
#include "Map.h"
#include "Log.h"

#include <regex>

namespace circuit {

using namespace springai;

#define THREAT_MOD		(1.0f / 128.0f)

CCircuitDef::RoleName* CCircuitDef::roleNames;
CCircuitDef::AttrName CCircuitDef::attrNames = {
	{"melee",     CCircuitDef::AttrType::MELEE},
	{"boost",     CCircuitDef::AttrType::BOOST},
	{"no_jump",   CCircuitDef::AttrType::NO_JUMP},
	{"no_strafe", CCircuitDef::AttrType::NO_STRAFE},
	{"stockpile", CCircuitDef::AttrType::STOCK},
	{"siege",     CCircuitDef::AttrType::SIEGE},
	{"ret_hold",  CCircuitDef::AttrType::RET_HOLD},
	{"ret_fight", CCircuitDef::AttrType::RET_FIGHT},
	{"jump",      CCircuitDef::AttrType::JUMP},
	{"dg_cost",   CCircuitDef::AttrType::DG_COST},
	{"dg_still",  CCircuitDef::AttrType::DG_STILL},
	{"anti_stat", CCircuitDef::AttrType::ANTI_STAT},
	{"rearm",     CCircuitDef::AttrType::REARM},
	{"no_dgun",   CCircuitDef::AttrType::NO_DGUN},
	{"prio_repair", CCircuitDef::AttrType::PRIO_REPAIR},
	{"guarded",     CCircuitDef::AttrType::GUARDED},
	{"raid_target", CCircuitDef::AttrType::RAID_TARGET},
	{"no_formation", CCircuitDef::AttrType::NO_FORM},
	{"cloak_route",  CCircuitDef::AttrType::CLOAK_ROUTE},
	{"plant",        CCircuitDef::AttrType::PLANT},
	{"kite",         CCircuitDef::AttrType::KITE},
	{"protect",      CCircuitDef::AttrType::PROTECT},
};
CCircuitDef::FireName CCircuitDef::fireNames = {
	{"hold",   CCircuitDef::FireType::HOLD},
	{"return", CCircuitDef::FireType::RETURN},
	{"open",   CCircuitDef::FireType::OPEN},
};

void CCircuitDef::InitStatic(CCircuitAI* circuit, CMaskHandler* roleMasker)
{
	std::vector<std::pair<std::string, CMaskHandler::TypeMask>> roles = {
		{"",           {ROLE_TYPE(NONE),    CCircuitDef::RoleMask::NONE}},
		{"builder",    {ROLE_TYPE(BUILDER), CCircuitDef::RoleMask::BUILDER}},
		{"scout",      {ROLE_TYPE(SCOUT),   CCircuitDef::RoleMask::SCOUT}},
		{"raider",     {ROLE_TYPE(RAIDER),  CCircuitDef::RoleMask::RAIDER}},
		{"riot",       {ROLE_TYPE(RIOT),    CCircuitDef::RoleMask::RIOT}},
		{"assault",    {ROLE_TYPE(ASSAULT), CCircuitDef::RoleMask::ASSAULT}},
		{"skirmish",   {ROLE_TYPE(SKIRM),   CCircuitDef::RoleMask::SKIRM}},
		{"artillery",  {ROLE_TYPE(ARTY),    CCircuitDef::RoleMask::ARTY}},
		{"anti_air",   {ROLE_TYPE(AA),      CCircuitDef::RoleMask::AA}},
		{"anti_sub",   {ROLE_TYPE(AS),      CCircuitDef::RoleMask::AS}},
		{"anti_heavy", {ROLE_TYPE(AH),      CCircuitDef::RoleMask::AH}},
		{"bomber",     {ROLE_TYPE(BOMBER),  CCircuitDef::RoleMask::BOMBER}},
		{"support",    {ROLE_TYPE(SUPPORT), CCircuitDef::RoleMask::SUPPORT}},
		{"mine",       {ROLE_TYPE(MINE),    CCircuitDef::RoleMask::MINE}},
		{"transport",  {ROLE_TYPE(TRANS),   CCircuitDef::RoleMask::TRANS}},
		{"air",        {ROLE_TYPE(AIR),     CCircuitDef::RoleMask::AIR}},
		{"sub",        {ROLE_TYPE(SUB),     CCircuitDef::RoleMask::SUB}},
		{"static",     {ROLE_TYPE(STATIC),  CCircuitDef::RoleMask::STATIC}},
		{"heavy",      {ROLE_TYPE(HEAVY),   CCircuitDef::RoleMask::HEAVY}},
		{"super",      {ROLE_TYPE(SUPER),   CCircuitDef::RoleMask::SUPER}},
		{"commander",  {ROLE_TYPE(COMM),    CCircuitDef::RoleMask::COMM}},
	};
	for (auto& kv : roles) {
		CMaskHandler::TypeMask tm = roleMasker->GetTypeMask(kv.first);
		if ((tm.type != kv.second.type) || (tm.mask != kv.second.mask)) {
			circuit->LOG("RoleError: %s = (%i, 0x%08X) != (%i, 0x%08X)", kv.first.c_str(),
						 kv.second.type, kv.second.mask, tm.type, tm.mask);
		}
	}

	CCircuitDef::roleNames = &roleMasker->GetMasks();
}

namespace {

using CustomParams = std::map<std::string, std::string>;

bool IsParamOn(const CustomParams& customParams, const char* name)
{
	auto it = customParams.find(name);
	return (it != customParams.end()) && (utils::string_to_int(it->second) == 1);
}

// A weapon's damage scale (paralyser, disarm and slow scaled down, capture up, impulse nearly 0) and its extra
// damage per shot (extra_damage, area_damage_dps)
float ReadWeaponScale(WeaponDef* wd, const CustomParams& customParams, float& outExtraDmg)
{
	float scale = wd->IsParalyzer() ? 0.5f : 1.0f;
	outExtraDmg = 0.f;
	auto it = customParams.find("extra_damage");
	if (it != customParams.end()) {
		outExtraDmg += utils::string_to_float(it->second);
	}
	if (IsParamOn(customParams, "disarmdamageonly")) {
		scale = 0.5f;
	}
	if (IsParamOn(customParams, "timeslow_onlyslow")) {
		scale = 0.5f;
	} else {
		it = customParams.find("timeslow_damagefactor");
		if (it != customParams.end()) {
			scale = utils::string_to_float(it->second);
		}
	}
	if (IsParamOn(customParams, "is_capture")) {
		scale = 2.0f;
	}
	it = customParams.find("area_damage_dps");
	if (it != customParams.end()) {
		outExtraDmg += utils::string_to_float(it->second);
		if (IsParamOn(customParams, "area_damage_is_impulse")) {
			scale = 0.02f;
		}
	}
	return scale;
}

// One shot's damage: statsdamage, else the average over armour types; with the dynamic damage falloff
float ReadWeaponDamage(WeaponDef* wd, const CustomParams& customParams)
{
	float ldmg = .0f;
	auto it = customParams.find("statsdamage");
	if (it != customParams.end()) {
		ldmg = utils::string_to_float(it->second);
	} else {
		Damage* damage = wd->GetDamage();
		const std::vector<float>& damages = damage->GetTypes();
		delete damage;
		for (float d : damages) {
			ldmg += d;
		}
		ldmg /= std::max<size_t>(damages.size(), 1);
	}
	return ldmg * std::pow(2.0f, (wd->IsDynDamageInverted() ? 1 : -1) * wd->GetDynDamageExp());
}

// "truerange" when it has one
float ReadWeaponRange(WeaponDef* wd, const CustomParams& customParams)
{
	auto it = customParams.find("truerange");
	return (it != customParams.end()) ? utils::string_to_float(it->second) : wd->GetRange();
}

// Hits what it fires at (@see https://springrts.com/wiki/Gamedev:WeaponDefs): cannons with fast projectiles,
// instant hit, tracking missiles
bool IsAlwaysHitWeapon(WeaponDef* wd, const std::string& wt, float range)
{
	return (((wt == "Cannon") || (wt == "DGun") || (wt == "EmgCannon") || (wt == "Flame") ||
			(wt == "LaserCannon") || (wt == "AircraftBomb")) && (wd->GetProjectileSpeed() * FRAMES_PER_SEC >= .8f * range))
		|| (wt == "BeamLaser") || (wt == "LightningCannon") || (wt == "Rifle")
		|| (((wt == "MissileLauncher") || (wt == "StarburstLauncher") || ((wt == "TorpedoLauncher") && wd->IsSubMissile()))
			&& wd->IsTracks());
}

// Fires at land units: all but surface torpedoes
bool IsLandWeapon(WeaponDef* wd, const std::string& wt)
{
	return (wt != "TorpedoLauncher") || wd->IsSubMissile();
}

}  // namespace

CCircuitDef::CCircuitDef(CCircuitAI* circuit, UnitDef* def, std::unordered_set<Id>& buildOpts,
		Resource* resM, Resource* resE)
		: def(def)
		, mainRole(ROLE_TYPE(ASSAULT))
		, enemyRole(RoleMask::NONE)
		, respRole(RoleMask::NONE)
		, role(RoleMask::NONE)
		, attr(NONE)
		, buildOptions(buildOpts)
		, count(0)
		, queued(0)
		, buildCounts(0)
		, sinceFrame(-1)
		, dgunDef(nullptr)
		, dgunMount(nullptr)
		, shieldMount(nullptr)
		, weaponMount(nullptr)
		, pwrDmg(.0f)
		, thrDmg(.0f)
		, thrMod(1.f)
		, aoe(.0f)
		, fireAoe(.0f)
		, shotDmg(.0f)
		, deathAoe(.0f)
		, deathDmg(.0f)
		, isDeathBomb(false)
		, power(.0f)
		, threat(.0f)
		, rawThreat(.0f)
		, minRange(.0f)
		, maxRangeType(RangeType::AIR)
		, shieldRadius(.0f)
		, maxShield(.0f)
		, shieldRegen(.0f)
		, shieldDrain(.0f)
		, isShieldLinked(false)
		, reloadTime(0)
		, targetCategory(0)
		, immobileTypeId(-1)
		, mobileTypeId(-1)
		, isIgnore(false)
		, isAttacker(false)
		, hasDGunAA(false)
		, hasAntiAir(false)
		, hasAntiLand(false)
		, hasAntiWater(false)
		, isAlwaysHit(false)
		, isHighTrajectory(false)
		, isAmphibious(false)
		, isLander(false)
		, isMex(false)
		, isPylon(false)
		, isAssist(false)
		, isPopup(false)
		, isPopupSwitch(false)
		, stockCost(.0f)
		, jumpRange(.0f)
		, retreat(-1.f)
		, radius(-1.f)
		, height(-1.f)
		, topOffset(-1.f)
{
	maxRange.fill(.0f);
	threatRange.fill(0);

	id = def->GetUnitDefId();

	buildDistance = def->GetBuildDistance();
	buildSpeed    = def->GetBuildSpeed();
	maxThisUnit   = def->GetMaxThisUnit();

//	maxRange[static_cast<RangeT>(RangeType::MAX)] = def->GetMaxWeaponRange();
	hasDGun         = def->CanManualFire();
	category        = def->GetCategory();
	noChaseCategory = (def->GetNoChaseCategory() | circuit->GetBadCategory())
					  & ~circuit->GetGoodCategory();

	const int ft = def->GetFireState();
	fireState = (ft < 0) ? FireType::OPEN : static_cast<FireType>(ft);

	health    = def->GetHealth();
	speed     = def->GetSpeed();  // elmos per second
	losRadius = def->GetLosRadius();
	costM     = def->GetCost(resM);
	costE     = def->GetCost(resE);
	cloakCost = std::max(def->GetCloakCost(), def->GetCloakCostMoving());
	buildTime = def->GetBuildTime();
//	altitude  = def->GetWantedHeight();

	COOAICallback* clb = circuit->GetCallback();
	MoveData* md = def->GetMoveData();
	isSubmarine  = (md == nullptr) ? false : md->IsSubMarine();
	delete md;
	isAbleToFly     = def->IsAbleToFly();
	isPlane         = !def->IsHoverAttack() && isAbleToFly;
	isFloater       = def->IsFloater() && !isSubmarine && !isAbleToFly;
	isSonarStealth  = def->IsSonarStealth();
	isTurnLarge     = (speed / (def->GetTurnRate() + 1e-3f) > 0.09f);  // empirical magic number
	isAbleToCloak   = def->IsAbleToCloak();
	isAbleToRepair  = def->IsAbleToRepair();
	isAbleToReclaim = def->IsAbleToReclaim();
	isAbleToAssist  = def->IsAbleToAssist() && !clb->UnitDef_HasYardMap(id);
	// Factory: def->IsBuilder() && !def->GetYardMap(0).empty() && !def->GetBuildOptions().empty()

	const std::map<std::string, std::string>& customParams = def->GetCustomParams();
	auto it = customParams.find("canjump");
	isAbleToJump = (it != customParams.end()) && (utils::string_to_int(it->second) == 1);
	if (isAbleToJump) {
		// Zero-K's defaults (gamedata/unitdef_defaults/jump_defs.lua) where a def has none
		auto jumpParam = [&customParams](const char* name, float defValue) {
			auto it = customParams.find(name);
			return (it != customParams.end()) ? utils::string_to_float(it->second) : defValue;
		};
		jumpRange = jumpParam("jump_range", 400.f);
		jumpSpeed = jumpParam("jump_speed", 6.f);
		jumpHeight = jumpParam("jump_height", 200.f);
		// Several charges (Detriment, 3): one used leaves another ready, so none counts as reloading (the safe side)
		jumpReload = (jumpParam("jump_charges", 1.f) > 1.f) ? 0 : int(jumpParam("jump_reload", 10.f) * FRAMES_PER_SEC);
	}

	isMobileBuilding = customParams.find("mobilebuilding") != customParams.end();  // Zero-K tests it is there

	it = customParams.find("is_drone");
	if ((it != customParams.end()) && (utils::string_to_int(it->second) == 1)) {
		category |= circuit->GetBadCategory();
		costM *= 0.1f;  // avoid threat metal
	}

//	if (customParams.find("boost_speed_mult") != customParams.end()) {
//		AddAttribute(AttrType::BOOST);
//	}

	bool isDynamic = false;
	if (customParams.find("level") != customParams.end()) {
		isDynamic = customParams.find("dynamic_comm") != customParams.end();
		isDynamicComm = isDynamic;
		AddRole(ROLE_TYPE(COMM));
		AddAttribute(AttrType::PRIO_REPAIR);  // commanders get priority repair by default
	}

	it = customParams.find("midposoffset");
	if (it != customParams.end()) {
		const std::string& str = it->second;
		std::string::const_iterator start = str.begin();
		std::string::const_iterator end = str.end();
		std::regex pattern("(-?\\d+)");
		std::smatch section;
		int index = 0;
		while (std::regex_search(start, end, section, pattern) && (index < 3)) {
			midPosOffset[index++] = utils::string_to_float(section[1]);
			start = section[0].second;
		}
	} else {
		midPosOffset = ZeroVector;
	}

	WeaponDef* sd = def->GetShieldDef();
	const bool isShield = (sd != nullptr);
	if (isShield) {
		Shield* shield = sd->GetShield();
		shieldRadius = shield->GetRadius();
		maxShield = shield->GetPower();
		// Zero-K's weapondefs_post moves the regen to customParam shield_rate (and zeroes the engine's)
		const std::map<std::string, std::string>& shieldParams = sd->GetCustomParams();
		auto itRate = shieldParams.find("shield_rate");
		shieldRegen = (itRate != shieldParams.end()) ? utils::string_to_float(itRate->second) : shield->GetPowerRegen();
		isShieldLinked = (shieldParams.find("unlinked") == shieldParams.end());  // Zero-K's unit_shield_link
		delete shield;
	}
	delete sd;

	WeaponDef* stockDef = def->GetStockpileDef();
	if (stockDef != nullptr) {
		it = customParams.find("stockpilecost");
		if (it != customParams.end()) {
			stockCost = utils::string_to_float(it->second);
		}
		AddAttribute(AttrType::STOCK);
		delete stockDef;
	}

	if (!def->IsAbleToAttack()) {
		if (isShield) {
			auto mounts = def->GetWeaponMounts();
			for (WeaponMount* mount : mounts) {
				WeaponDef* wd = mount->GetWeaponDef();
				if ((shieldMount == nullptr) && wd->IsShield()) {
					shieldMount = mount;  // NOTE: Unit may have more than 1 shield
				} else {
					delete mount;
				}
				delete wd;
			}
		}
		// NOTE: Aspis (mobile shield) has 10 damage for some reason, break
		return;
	}

	/*
	 * DPS and Weapon calculations
	 */
	std::set<CWeaponDef::Id> allWeaponDefs;
	minRange = std::numeric_limits<float>::max();
	float minReloadTime = std::numeric_limits<float>::max();
	float bestDGunReload = std::numeric_limits<float>::max();
	float bestWpRange = std::numeric_limits<float>::max();
	float dps = .0f;  // TODO: split dps like ranges on air, land, water
	float dmg = .0f;
	// GetFireAoe sums; double, so that weapons of one radius average to exactly that radius (group_count_aoe edge)
	double fireDps = .0;
	double fireAoeDps = .0;
	float shotDmgSum = .0f;  // GetShotDamage
	int shotWeapons = 0;
	CWeaponDef* bestDGunDef = nullptr;
	WeaponMount* bestDGunMnt = nullptr;
	WeaponMount* bestWpMnt = nullptr;
	bool canTargetAir = false;
	bool canTargetLand = false;
	bool canTargetWater = false;
	auto mounts = def->GetWeaponMounts();
	for (WeaponMount* mount : mounts) {
		WeaponDef* wd = mount->GetWeaponDef();
		const std::map<std::string, std::string>& customParams = wd->GetCustomParams();

		if (customParams.find("fake_weapon") != customParams.end()) {
			delete wd;
			delete mount;
			continue;
		}

		const float prevDps = dps;
		float extraDmg;
		const float scale = ReadWeaponScale(wd, customParams, extraDmg);

		if (customParams.find("reammoseconds") != customParams.end()) {
			AddAttribute(AttrType::REARM);
		}

		float reloadTime = wd->GetReload();  // seconds
		const int wpBurstFrames = int((wd->GetSalvoSize() - 1) * wd->GetSalvoDelay() * FRAMES_PER_SEC);
		burstFrames = std::max(burstFrames, wpBurstFrames);
		if (!wd->IsShield()) {
			soleFire = {mount->GetWeaponMountId(), int(reloadTime * FRAMES_PER_SEC), wpBurstFrames};
			++fireCount;
		}
		auto it = customParams.find("shield_drain");  // charge per shot, taken from the unit's own shield
		if ((it != customParams.end()) && (reloadTime > 0.f) && (maxShield > 0.f)) {
			shieldDrain += std::max(utils::string_to_float(it->second), 0.f) * wd->GetSalvoSize() / reloadTime;
		}
		if (minReloadTime > reloadTime) {
			minReloadTime = reloadTime;
		}
		if (extraDmg > 0.1f) {
			dmg += extraDmg;
			dps += extraDmg * wd->GetSalvoSize() / reloadTime * scale;
		}

		const float ldmg = ReadWeaponDamage(wd, customParams);
		dmg += ldmg;
		dps += ldmg * wd->GetSalvoSize() / reloadTime * scale;
		int weaponCat = mount->GetOnlyTargetCategory();
		targetCategory |= weaponCat;

		aoe = std::max(aoe, wd->GetAreaOfEffect());
		if (!wd->IsManualFire() && !wd->IsShield()) {  // GetFireAoe: its radius weighted by its dps
			const double wpDps = dps - prevDps;
			fireDps += wpDps;
			fireAoeDps += wpDps * wd->GetAreaOfEffect();
			shotDmgSum += wd->IsParalyzer() ? 0.f : ldmg;
			++shotWeapons;
		}

		const std::string wt(wd->GetType());
		const float range = ReadWeaponRange(wd, customParams);

		isAlwaysHit |= IsAlwaysHitWeapon(wd, wt, range);
		const bool isAirWeapon = isAlwaysHit && (range > 150.f);
		canTargetAir |= isAirWeapon;

		bool isLandWeapon = IsLandWeapon(wd, wt);
		canTargetLand |= isLandWeapon;
		bool isWaterWeapon = wd->IsWaterWeapon();
		canTargetWater |= isWaterWeapon;

		minRange = std::min(minRange, range);
		if ((weaponCat & circuit->GetAirCategory()) && isAirWeapon) {
			float& mr = maxRange[static_cast<RangeT>(RangeType::AIR)];
			mr = std::max(mr, range);
			if (mr > maxRange[static_cast<RangeT>(maxRangeType)]) {
				maxRangeType = RangeType::AIR;
			}
		}
		if ((weaponCat & circuit->GetLandCategory()) && isLandWeapon) {
			float& mr = maxRange[static_cast<RangeT>(RangeType::LAND)];
			const float landRange = (isAbleToFly && (wt == "Cannon")) ? range * 1.25f : range;  // 1.25 - gunship height hax
			if (landRange > mr) {
				mr = landRange;
				isHighTrajectory = circuit->GetWeaponDef(wd->GetWeaponDefId())->IsHighTrajectory();
			}
			if (mr > maxRange[static_cast<RangeT>(maxRangeType)]) {
				maxRangeType = RangeType::LAND;
			}
		}
		if ((weaponCat & circuit->GetWaterCategory()) && isWaterWeapon) {
			float& mr = maxRange[static_cast<RangeT>(RangeType::WATER)];
			mr = std::max(mr, range);
			if (mr > maxRange[static_cast<RangeT>(maxRangeType)]) {
				maxRangeType = RangeType::WATER;
			}
		}

		if (wd->IsManualFire() && (reloadTime < bestDGunReload)) {
			// NOTE: Disable commander's dgun, because no usage atm
			if (customParams.find("manualfire") == customParams.end()) {
				bestDGunReload = reloadTime;
				bestDGunDef = circuit->GetWeaponDef(wd->GetWeaponDefId());
				delete bestDGunMnt;
				bestDGunMnt = mount;
				hasDGunAA |= (weaponCat & circuit->GetAirCategory()) && isAirWeapon;
			} else {  // FIXME: Dynamo com workaround
				delete mount;
			}
		} else if (wd->IsShield()) {
			if (shieldMount == nullptr) {
				shieldMount = mount;  // NOTE: Unit may have more than 1 shield
			} else {
				delete mount;
			}
		} else if (range < bestWpRange) {
			delete bestWpMnt;
			bestWpMnt = mount;
			bestWpRange = range;
		} else {
			delete mount;
		}

		allWeaponDefs.insert(wd->GetWeaponDefId());

		delete wd;
	}

	circuit->BindUnitToWeaponDefs(GetId(), allWeaponDefs, IsMobile());

	if (isDynamic) {  // FIXME: Dynamo com workaround
		dps /= mounts.size();
		dmg /= mounts.size();
	}
	this->dps = dps;

	if (minReloadTime < std::numeric_limits<float>::max()) {
		reloadTime = minReloadTime * FRAMES_PER_SEC;
	}
	if (bestDGunReload < std::numeric_limits<float>::max()) {
		dgunDef = bestDGunDef;
		dgunMount = bestDGunMnt;
	}
	if (bestWpRange < std::numeric_limits<float>::max()) {
		weaponMount = bestWpMnt;
	}

	fireAoe = (fireDps > 0.) ? static_cast<float>(fireAoeDps / fireDps) : 0.f;
	shotDmg = (shotWeapons > 0) ? shotDmgSum / shotWeapons : 0.f;

	isAttacker = dps > .1f;
	if (IsMobile() && !IsAttacker()) {  // mobile bomb?
		isDeathBomb = true;
		WeaponDef* wd = def->GetDeathExplosion();
		aoe = fireAoe = wd->GetAreaOfEffect();
		if (aoe > 64.0f) {
			// power
			float ldmg = .0f;
			it = customParams.find("statsdamage");
			if (it != customParams.end()) {
				ldmg = utils::string_to_float(it->second);
			} else {
				Damage* damage = wd->GetDamage();
				const std::vector<float>& damages = damage->GetTypes();
				delete damage;
				for (float d : damages) {
					ldmg += d;
				}
				ldmg /= damages.size();
			}
			dmg += ldmg;
			dps = ldmg * wd->GetSalvoSize();
			isAttacker = dps > .1f;
			// range
			minRange = aoe;
			for (RangeT rt = 0; rt < static_cast<RangeT>(RangeType::_SIZE_); ++rt) {
				float& mr = maxRange[rt];
				mr = std::max(mr, aoe);
			}
			// category
			targetCategory = wd->GetOnlyTargetCategory();  // 0xFFFFFFFF
			if (~targetCategory == 0) {
				targetCategory = ~circuit->GetBadCategory();
			}
			category |= circuit->GetBadCategory();  // do not chase bombs
		}
		delete wd;
	}

	// Pop-up turret (Faraday, Gauss, Desolator, Lucifer, Hacksaw): a building with a weapon that takes less damage
	// while armoured (closed), see CCircuitDef::IsPopup
	isPopup = !IsMobile() && isAttacker && (def->GetArmoredMultiple() < 1.f);
	isPopupSwitch = isPopup && def->IsOnOffable();

	// Death explosion (explodeAs): its radius and its worst damage over armour types (CCircuitDef::GetDeathDamage)
	{
		WeaponDef* wd = def->GetDeathExplosion();
		if (wd != nullptr) {
			deathAoe = wd->GetAreaOfEffect();
			Damage* damage = wd->GetDamage();
			for (float d : damage->GetTypes()) {
				deathDmg = std::max(deathDmg, d);
			}
			delete damage;
			delete wd;
		}
	}

	// NOTE: isTracks filters units with slow weapon (hermit, recluse, rocko)
	hasAntiAir   = (targetCategory & circuit->GetAirCategory()) && canTargetAir;
	hasAntiLand  = (targetCategory & circuit->GetLandCategory()) && canTargetLand;
	hasAntiWater = (targetCategory & circuit->GetWaterCategory()) && canTargetWater;

	// TODO: Include projectile-speed/range, armor
	//       health /= def->GetArmoredMultiple();
	thrDmg = pwrDmg = dmg = sqrtf(dps) * std::pow(dmg, 0.25f) * THREAT_MOD;
	threat = power = rawThreat = dmg * sqrtf(def->GetHealth() + maxShield * SHIELD_MOD);
}

int STargetPriority::GetTier(const CCircuitDef* edef, bool isInLos) const
{
	if (edef == nullptr) {
		return unknownTier[isInLos ? 1 : 0];
	}
	const size_t id = static_cast<size_t>(edef->GetId());
	return (id < tierOf.size()) ? tierOf[id][isInLos ? 1 : 0] : NO_TIER;
}

CCircuitDef::~CCircuitDef()
{
	delete def;
	delete dgunMount;
	delete shieldMount;
	delete weaponMount;
}

void CCircuitDef::Init(CCircuitAI* circuit)
{
	CTerrainData& terrainData = circuit->GetGameAttribute()->GetTerrainData();
	assert(terrainData.IsInitialized());

	if (IsAbleToFly()) {

	} else if (!IsMobile()) {  // for immobile units

		immobileTypeId = terrainData.udImmobileType[GetId()];
		// If a unit can build mobile units then it will inherit mobileType from it's options
		std::map<STerrainMapMobileType::Id, float> mtUsability;
		for (CCircuitDef::Id buildId : GetBuildOptions()) {
			CCircuitDef* bdef = circuit->GetCircuitDef(buildId);
			if ((bdef == nullptr) || !bdef->IsMobile() || !bdef->IsAttacker()) {
				continue;
			}
			STerrainMapMobileType::Id mtId = terrainData.udMobileType[bdef->GetId()];
			if ((mtId < 0) || (mtUsability.find(mtId) != mtUsability.end())) {
				continue;
			}
			STerrainMapMobileType& mt = terrainData.areaData0.mobileType[mtId];
			mtUsability[mtId] = mt.area.empty() ? 00.0 : mt.areaLargest->percentOfMap;
		}
		float useMost = .0f;
		STerrainMapMobileType::Id mtId = mobileTypeId;  // -1
		for (auto& mtkv : mtUsability) {
			if (mtkv.second > useMost) {
				mtId = mtkv.first;
				useMost = mtkv.second;
			}
		}
		mobileTypeId = mtId;

	} else {  // for mobile units

		mobileTypeId = terrainData.udMobileType[GetId()];
	}

	if (IsMobile()) {
		if (mobileTypeId >= 0) {
			STerrainMapMobileType& mt = terrainData.areaData0.mobileType[mobileTypeId];
			isAmphibious = ((mt.minElevation < -SQUARE_SIZE * 5) || (mt.maxElevation < SQUARE_SIZE * 5)) && !IsFloater();
		}
	} else {
		if (immobileTypeId >= 0) {
			STerrainMapImmobileType& it = terrainData.areaData0.immobileType[immobileTypeId];
			isAmphibious = ((it.minElevation < -SQUARE_SIZE * 5) || (it.maxElevation < SQUARE_SIZE * 5)) && !IsFloater();
		}
	}
	isLander = !IsFloater() && !IsAbleToFly() && !IsAmphibious() && !IsSubmarine();
}

void CCircuitDef::AddRole(RoleT type, RoleT bindType)
{
	respRole |= GetMask(type);
	role |= GetMask(bindType);
}

void CCircuitDef::RemDGun()
{
	dgunDef = nullptr;
	delete dgunMount;
	dgunMount = nullptr;
	hasDGun = false;
	hasDGunAA = false;
}

CCircuitDef::RangeType CCircuitDef::GetRangeType(const CCircuitDef* cdef, const AIFloat3& pos)
{
	return ((cdef != nullptr) && cdef->IsAbleToFly()) ? RangeType::AIR
			: (pos.y < -SQUARE_SIZE * 5) ? RangeType::WATER : RangeType::LAND;
}

int CCircuitDef::GetDefaultMoveState() const
{
	const int state = def->GetMoveState();
	return (state >= 0) ? state : 1;
}

float CCircuitDef::GetRadius()
{
	if (radius < 0.f) {
		radius = def->GetRadius();  // Forces loading of the unit model
	}
	return radius;
}

float CCircuitDef::GetHeight()
{
	if (height < 0.f) {
		height    = def->GetHeight();  // Forces loading of the unit model
		topOffset = height / 2 - def->GetWaterline();
	}
	return height;
}

const CWeaponDef::SStats* CCircuitDef::GetWeaponStats(CCircuitAI* circuit, int weaponDefId)
{
	// As the weapon loop in the constructor works a weapon out, for one weapon
	if (!circuit->IsValidWeaponDefId(weaponDefId)) {
		return nullptr;
	}
	const CWeaponDef* cwd = circuit->GetWeaponDef(weaponDefId);
	const CWeaponDef::SStats* known = cwd->GetStats();
	if (known != nullptr) {
		return known;
	}
	CWeaponDef::SStats stats;
	WeaponDef* wd = cwd->GetDef();
	const std::map<std::string, std::string>& customParams = wd->GetCustomParams();
	if (customParams.find("fake_weapon") != customParams.end()) {
		cwd->SetStats(stats);  // all 0
		return cwd->GetStats();
	}
	float extraDmg;
	const float scale = ReadWeaponScale(wd, customParams, extraDmg);
	const float reloadTime = std::max(wd->GetReload(), 1.f / FRAMES_PER_SEC);  // seconds
	const float salvo = (ReadWeaponDamage(wd, customParams) + ((extraDmg > 0.1f) ? extraDmg : 0.f)) * wd->GetSalvoSize() * scale;
	stats.salvo = salvo;
	stats.dps = salvo / reloadTime;

	const std::string wt(wd->GetType());
	stats.range = ReadWeaponRange(wd, customParams);
	stats.aoe = wd->GetAreaOfEffect();
	const int weaponCat = wd->GetOnlyTargetCategory();
	stats.isAir = IsAlwaysHitWeapon(wd, wt, stats.range) && (stats.range > 150.f) && (weaponCat & circuit->GetAirCategory());
	stats.isLand = IsLandWeapon(wd, wt) && (weaponCat & circuit->GetLandCategory());
	stats.isWater = wd->IsWaterWeapon() && (weaponCat & circuit->GetWaterCategory());
	stats.isManual = wd->IsManualFire();
	stats.isShield = wd->IsShield();
	if (stats.isShield) {
		stats.dps = stats.salvo = 0.f;
	}
	cwd->SetStats(stats);
	return cwd->GetStats();
}

bool CCircuitDef::ReadCommWeapons(CCircuitAI* circuit, springai::Unit* unit, SCommWeapons& out)
{
	// Zero-K's dynamic commanders (dyncomm_chassis_generator.lua) set, per slot N in 1..2: comm_weapon_id_N (the
	// weapon def), comm_weapon_manual_N (> 0: manual fire, the D-gun slot); the modules scale every weapon by
	// comm_range_mult and comm_damage_mult
	out = SCommWeapons();
	const float rangeMult = unit->GetRulesParamFloat("comm_range_mult", 1.f);
	const float damageMult = unit->GetRulesParamFloat("comm_damage_mult", 1.f);
	for (int num = 1; num < 3; ++num) {
		std::string str = utils::int_to_string(num, "comm_weapon_id_%i");
		const int weaponDefId = int(unit->GetRulesParamFloat(str.c_str(), -1));
		const CWeaponDef::SStats* stats = (weaponDefId < 0) ? nullptr : GetWeaponStats(circuit, weaponDefId);
		if (stats == nullptr) {
			continue;
		}
		out.isRead = true;
		if (stats->isShield) {
			continue;
		}
		str = utils::int_to_string(num, "comm_weapon_manual_%i");
		const bool isManual = stats->isManual || (unit->GetRulesParamFloat(str.c_str(), -1) > 0.f);
		const float range = stats->range * rangeMult;
		if (range > out.reach) {
			out.reach = range;
			out.reachAoe = stats->aoe;
		}
		if (isManual) {
			out.alpha = std::max(out.alpha, stats->salvo * damageMult);
			out.alphaRange = std::max(out.alphaRange, range);
			continue;
		}
		out.dps += stats->dps * damageMult;
		if (stats->isAir) {
			float& r = out.range[static_cast<RangeT>(RangeType::AIR)];
			r = std::max(r, range);
		}
		if (stats->isLand) {
			float& r = out.range[static_cast<RangeT>(RangeType::LAND)];
			r = std::max(r, range);
		}
		if (stats->isWater) {
			float& r = out.range[static_cast<RangeT>(RangeType::WATER)];
			r = std::max(r, range);
		}
		out.maxRange = std::max(out.maxRange, range);
	}
	return out.isRead;
}

bool CCircuitDef::IsYTargetable(float elevation, float posY) {
	GetHeight();
	return (elevation > -height || posY > -topOffset);
}

bool CCircuitDef::IsLongReload() const
{
	return reloadTime >= FRAMES_PER_SEC * 5;
}

bool CCircuitDef::IsQuarry(const CCircuitDef* edef) const
{
	if ((edef == nullptr) || !edef->IsEnemyRoleAny(RoleMask::HEAVY | RoleMask::COMM)) {
		return false;
	}
	return !(IsAttrAntiStat() && edef->IsMobile())
		&& ((edef->GetCategory() & targetCategory) != 0)
		&& !(edef->IsAbleToFly() && !HasAntiAir());
}

} // namespace circuit

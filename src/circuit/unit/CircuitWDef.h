/*
 * CircuitWDef.h
 *
 *  Created on: Oct 27, 2019
 *      Author: rlcevg
 */

#ifndef SRC_CIRCUIT_UNIT_CIRCUITWDEF_H_
#define SRC_CIRCUIT_UNIT_CIRCUITWDEF_H_

#include "WeaponDef.h"

namespace circuit {

class CWeaponDef {
public:
	using Id = int;

//	CWeaponDef(const CWeaponDef& that) = delete;
	CWeaponDef& operator=(const CWeaponDef&) = delete;
	CWeaponDef(springai::WeaponDef* def, springai::Resource* resE);
	virtual ~CWeaponDef();

	static Id WeaponIdFromLua(int luaId);

	springai::WeaponDef* GetDef() const { return def; }

	float GetRange() const { return range; }
	float GetAoe() const { return aoe; }
	float GetCostE() const { return costE; }
	float GetFireTime() const { return fireTime; }
	bool IsStockpile() const { return isStockpile; }
	bool IsHighTrajectory() const { return isHigh; }

	// One weapon's fight numbers, as CCircuitDef's weapon loop works them out (CCircuitDef::GetWeaponStats fills them
	// once): for a dynamic commander's fitted weapons (CCircuitDef::SCommWeapons)
	struct SStats {
		float dps = 0.f;  // damage a second: salvo / reload, extra and area damage in, paralyser and slow scaled down
		float salvo = 0.f;  // one salvo's damage (a manual-fire weapon's alpha)
		float range = 0.f;  // "truerange" when it has one
		float aoe = 0.f;
		bool isLand = false;
		bool isWater = false;
		bool isAir = false;
		bool isManual = false;
		bool isShield = false;
	};
	const SStats* GetStats() const { return isStats ? &stats : nullptr; }
	void SetStats(const SStats& value) const { stats = value; isStats = true; }

private:
	springai::WeaponDef* def;  // owner

	float range;
	float aoe;
	float costE;
	float fireTime;
	bool isStockpile : 1;
	bool isHigh : 1;
	mutable SStats stats;  // SetStats
	mutable bool isStats = false;
};

} // namespace circuit

#endif // SRC_CIRCUIT_UNIT_CIRCUITWDEF_H_

/*
 * AttackBalance.cpp
 *
 *  Created on: Nov 18, 2026
 *      Author: Claude
 */

#include "module/AttackBalance.h"
#include "map/InfluenceMap.h"
#include "map/MapManager.h"
#include "map/ThreatMap.h"
#include "module/AllyCover.h"
#include "module/MilitaryManager.h"
#include "task/fighter/AttackTask.h"
#include "unit/CircuitUnit.h"
#include "unit/ally/AllyUnit.h"
#include "unit/enemy/EnemyUnit.h"
#include "CircuitAI.h"
#include "util/Utils.h"

#include "Log.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace circuit {

using namespace springai;

// Mobile enemies out of radar and LOS count at their last known position for this long
static constexpr int SEEN_FRAMES = FRAMES_PER_SEC * 10;

CAttackBalance::CAttackBalance(CCircuitAI* circuit, const SConfig& cfg)
		: circuit(circuit)
		, cfg(cfg)
		, listFrame(-1)
{
	if (IsOn()) {
		// Allied statics: the threat map's defences layer (shared by the ally team; on once any of its AIs asks)
		circuit->GetThreatMap()->EnableCover();
	}
}

CAttackBalance::~CAttackBalance()
{
}

float CAttackBalance::SForce::GetRatio() const
{
	const float ours = GetOurs();
	return (ours > 0.f) ? enemy / ours : ((enemy > 0.f) ? 99.f : 0.f);
}

float CAttackBalance::GetLine(Line line) const
{
	return ((line == Line::KEEP) || (line == Line::OWN)) ? cfg.ratio * KEEP_FACTOR : cfg.ratio;
}

bool CAttackBalance::IsFail(const SForce& force, Line line) const
{
	return (force.enemy > 0.f) && (force.enemy >= GetLine(line) * force.GetOurs());
}

float CAttackBalance::GetMobileWeight(float dist, float range, float speed) const
{
	const float t = std::max(dist - range, 0.f) / std::max(speed, 1.f);
	return std::max(1.f - t / cfg.horizon, 0.f);
}

float CAttackBalance::GetRangeAt(float rangeLand, float rangeWater, bool isWater, bool isAmph)
{
	// As the threat map draws it: the land range on land, and on the surface layer over water too; the water range on
	// water. The amphibious layer has no land range under water
	return std::max((isWater && isAmph) ? 0.f : rangeLand, isWater ? rangeWater : 0.f);
}

float CAttackBalance::GetRangeAt(const CCircuitDef* cdef, bool isWater, bool isAmph)
{
	return GetRangeAt(cdef->HasAntiLand() ? cdef->GetMaxRange(CCircuitDef::RangeType::LAND) : 0.f,
			cdef->HasAntiWater() ? cdef->GetMaxRange(CCircuitDef::RangeType::WATER) : 0.f, isWater, isAmph);
}

void CAttackBalance::AddEntry(std::vector<SEntry>& entries, const CCircuitDef* cdef, const AIFloat3& pos, float power,
		const CAttackTask* task)
{
	float rangeLand = cdef->HasAntiLand() ? cdef->GetMaxRange(CCircuitDef::RangeType::LAND) : 0.f;
	float rangeWater = cdef->HasAntiWater() ? cdef->GetMaxRange(CCircuitDef::RangeType::WATER) : 0.f;
	const Kind kind = !cdef->IsMobile() ? Kind::STATIC : cdef->IsAbleToFly() ? Kind::AIR : Kind::MOBILE;
	if (kind == Kind::STATIC) {  // the defences layer's reach (cover falloff, as our statics)
		rangeLand = CThreatMap::GetCoverRange(cdef, rangeLand);
		rangeWater = CThreatMap::GetCoverRange(cdef, rangeWater);
	}
	const float range = std::max(rangeLand, rangeWater);
	if ((range <= 0.f) || (power <= 0.f) || !utils::is_valid(pos)) {
		return;
	}
	const float speed = cdef->GetSpeed();
	const float reach = (kind == Kind::MOBILE) ? range + cfg.horizon * speed : range;
	entries.push_back({pos, power, rangeLand, rangeWater, speed, SQUARE(reach), kind, task});
}

void CAttackBalance::Update()
{
	// At most once per threat map update (the enemies' data and the allies' snapshot change no more often), on the
	// first check after it: quiet periods cost nothing
	const int frame = circuit->GetLastFrame();
	if ((listFrame >= 0) && (frame < listFrame + THREAT_UPDATE_RATE)) {
		return;
	}
	listFrame = frame;
	const auto startTime = std::chrono::steady_clock::now();

	enemies.clear();
	allies.clear();
	blips.clear();
	static std::vector<float> powers, speeds, ranges;  // NOTE: micro-opt; the known enemy mobiles, for blips
	powers.clear();
	speeds.clear();
	ranges.clear();
	for (const auto& kv : circuit->GetMapManager()->GetHostileUnits()) {
		const CEnemyUnit* e = kv.second;
		const CCircuitDef* edef = e->GetCircuitDef();
		const bool isMobile = (edef == nullptr) || edef->IsMobile();
		if (e->IsHidden() || e->IsBeingBuilt() || (isMobile && !e->IsRecentlySeen(frame, SEEN_FRAMES))) {
			continue;
		}
		if (edef == nullptr) {  // an unknown radar blip: logged only, if it moves (a parked building blip is no danger)
			if (e->GetVel().SqLength2D() > 1e-4f) {
				blips.push_back(e->GetPos());
			}
			continue;
		}
		if (!edef->HasAntiLand() && !edef->HasAntiWater()) {
			continue;  // not on the surface or amphibious layers: anti-air only
		}
		const size_t count = enemies.size();
		AddEntry(enemies, edef, e->GetPos(), e->GetThreat(), nullptr);
		if ((enemies.size() > count) && (enemies.back().kind == Kind::MOBILE)) {
			powers.push_back(enemies.back().power);
			speeds.push_back(enemies.back().speed);
			ranges.push_back(std::max(enemies.back().rangeLand, enemies.back().rangeWater));
		}
	}
	auto median = [](std::vector<float>& v) {
		if (v.empty()) {
			return 0.f;
		}
		auto mid = v.begin() + v.size() / 2;
		std::nth_element(v.begin(), mid, v.end());
		return *mid;
	};
	blipPower = median(powers);
	blipSpeed = median(speeds);
	blipRange = median(ranges);

	// Allied mobiles where the influence map saw them (every allied team). Ours carry their task's states, read now:
	// a task is never dereferenced later (a merge may delete it between this and the checks)
	for (const auto& kv : circuit->GetInflMap()->GetMobileArmedPos()) {
		const CCircuitDef* adef = nullptr;
		const CAttackTask* task = nullptr;
		CCircuitUnit* unit = circuit->GetTeamUnit(kv.first);
		if (unit != nullptr) {
			adef = unit->GetCircuitDef();
			const IUnitTask* t = unit->GetTask();
			if ((t != nullptr) && (t->GetType() == IUnitTask::Type::RETREAT)) {
				continue;  // retreating to repair
			}
			if ((t != nullptr) && (t->GetType() == IUnitTask::Type::FIGHTER)
				&& (static_cast<const IFighterTask*>(t)->GetFightType() == IFighterTask::FightType::ATTACK))
			{
				const CAttackTask* attack = static_cast<const CAttackTask*>(t);
				if (attack->IsFleeing(frame) || attack->IsOutnumbered()) {
					continue;
				}
				task = attack;
			}
		} else {
			CAllyUnit* ally = circuit->GetFriendlyUnit(kv.first);
			adef = (ally != nullptr) ? ally->GetCircuitDef() : nullptr;
		}
		if ((adef != nullptr) && adef->IsMobile()) {
			AddEntry(allies, adef, kv.second, adef->GetPower(), task);
		}
	}

	++stats.rebuilds;
	stats.rebuildMs += std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - startTime).count();
}

float CAttackBalance::GetWeight(const SEntry& e, float sqDist, bool isWater, bool isAmph) const
{
	if (sqDist >= e.sqReach) {
		return 0.f;  // most entries: one compare
	}
	const float range = GetRangeAt(e.rangeLand, e.rangeWater, isWater, isAmph);
	if (range <= 0.f) {
		return 0.f;
	}
	const float sqRange = SQUARE(range);
	switch (e.kind) {
		case Kind::STATIC: {
			return (sqDist < sqRange) ? CAllyCover::Falloff(sqDist, sqRange) : 0.f;
		}
		case Kind::AIR: {
			return (sqDist <= sqRange) ? 1.f : 0.f;  // holding back doesn't avoid them: only those already there
		}
		default: {
			return GetMobileWeight(sqrtf(sqDist), range, e.speed);
		}
	}
}

void CAttackBalance::GetForceAt(const AIFloat3& F, const CAttackTask* self, bool isAmph, SForce& outForce)
{
	Update();
	const auto startTime = std::chrono::steady_clock::now();

	CThreatMap* threatMap = circuit->GetThreatMap();
	const bool isWater = threatMap->IsWaterAt(F);
	float enemy = 0.f;
	for (const SEntry& e : enemies) {
		enemy += e.power * GetWeight(e, F.SqDistance2D(e.pos), isWater, isAmph);
	}
	float allied = 0.f;
	for (const SEntry& a : allies) {
		if ((a.task != self) || (self == nullptr)) {
			allied += a.power * GetWeight(a, F.SqDistance2D(a.pos), isWater, isAmph);
		}
	}
	float blip = 0.f;
	const float sqBlipReach = SQUARE(blipRange + cfg.horizon * blipSpeed);
	for (const AIFloat3& p : blips) {
		const float sqDist = F.SqDistance2D(p);
		if (sqDist < sqBlipReach) {
			blip += blipPower * GetMobileWeight(sqrtf(sqDist), blipRange, blipSpeed);
		}
	}
	outForce.enemy = enemy;
	outForce.allied = allied;
	outForce.cover = threatMap->GetDefenceCoverAt(F);  // none on water
	outForce.blip = blip;

	++stats.forces;
	stats.forceMs += std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - startTime).count();
}

void CAttackBalance::NoteCheck(Line line, bool isSkip, ICoreUnit::Id targetId)
{
	const int i = static_cast<int>(line);
	++stats.checks[i];
	if (isSkip) {
		++stats.skips[i];
		if (targetId >= 0) {
			stats.skippedTargets.insert(targetId);
		}
	}
}

void CAttackBalance::NoteFight(float ratio, float blipShare, float metalLost, bool isFled)
{
	SFights& f = stats.fights[(ratio < 1.f) ? 0 : (ratio < 1.5f) ? 1 : (ratio < 2.f) ? 2 : 3];
	++f.count;
	f.metalLost += metalLost;
	f.flees += isFled ? 1 : 0;
	f.blipShare += blipShare;
}

void CAttackBalance::NoteBlocked(const CAttackTask* task, ICoreUnit::Id leaderId, int frames)
{
	SBlocked& b = stats.blocked.insert({task, {leaderId, 0}}).first->second;
	b.leaderId = leaderId;
	b.frames += frames;
}

void CAttackBalance::Log()
{
	// For tuning quota.attack_balance: how often each line skipped, the fights by the E/A they were engaged at and how
	// they ended (the ratio is where they start costing units), how long groups were held back, and the cost
	SStats& st = stats;
	circuit->LOG("BALANCE last 60 s%s: checks pick %i, keep %i, own spot %i, zone %i; skips pick %i, keep %i, own spot %i,"
			" zone %i (%i targets); outnumbered on %i, off %i; zones set %i, lifted %i",
			cfg.isLogOnly ? " (log only)" : "",
			st.checks[0], st.checks[1], st.checks[2], st.checks[3], st.skips[0], st.skips[1], st.skips[2], st.skips[3],
			static_cast<int>(st.skippedTargets.size()), st.flagsOn, st.flagsOff, st.zonesSet, st.zonesLifted);
	std::string fights;
	static const char* const bands[] = {"under 1", "1-1.5", "1.5-2", "2+"};
	for (int i = 0; i < 4; ++i) {
		const SFights& f = st.fights[i];
		fights += utils::string_format("%s%s: %i fights, %.0f metal lost, %i fled, blips %.0f%%", (i > 0) ? "; " : "",
				bands[i], f.count, f.metalLost, f.flees, (f.count > 0) ? 100.f * f.blipShare / f.count : 0.f);
	}
	circuit->LOG("BALANCE fights last 60 s by E/A at engage: %s", fights.c_str());
	std::vector<SBlocked> blocked;
	int frames = 0;
	for (const auto& kv : st.blocked) {
		blocked.push_back(kv.second);
		frames += kv.second.frames;
	}
	std::sort(blocked.begin(), blocked.end(), [](const SBlocked& a, const SBlocked& b) { return a.frames > b.frames; });
	std::string top;
	for (size_t i = 0; (i < blocked.size()) && (i < 5); ++i) {
		top += utils::string_format("%s#%i %.0f s", (i > 0) ? ", " : "", blocked[i].leaderId,
				static_cast<float>(blocked[i].frames) / FRAMES_PER_SEC);
	}
	circuit->LOG("BALANCE blocked last 60 s: %.0f s over %i groups (%i attack groups now)%s%s",
			static_cast<float>(frames) / FRAMES_PER_SEC, static_cast<int>(blocked.size()),
			static_cast<int>(circuit->GetMilitaryManager()->GetTasks(IFighterTask::FightType::ATTACK).size()),
			top.empty() ? "" : ": ", top.c_str());
	circuit->LOG("BALANCE cost last 60 s: %i list rebuilds %.2f ms, %i force sums %.2f ms | now: %i enemies, %i allies,"
			" %i moving blips (at the median power %.1f, speed %.0f, range %.0f)",
			st.rebuilds, st.rebuildMs, st.forces, st.forceMs, static_cast<int>(enemies.size()),
			static_cast<int>(allies.size()), static_cast<int>(blips.size()), blipPower, blipSpeed, blipRange);
	st = SStats();
}

} // namespace circuit

/*
 * AllyCover.cpp
 *
 *  Created on: Nov 16, 2026
 *      Author: Claude
 */

#include "module/AllyCover.h"
#include "map/ThreatMap.h"
#include "module/MilitaryManager.h"
#include "setup/SetupManager.h"
#include "task/fighter/AttackTask.h"
#include "unit/CircuitUnit.h"
#include "CircuitAI.h"
#include "util/Utils.h"

#include "Log.h"
#include "Unit.h"

#include <chrono>

namespace circuit {

using namespace springai;

// A clock read costs about as much as a lookup, so one lookup in this many is timed (the log scales it up)
static constexpr int LOOKUP_SAMPLE = 8;

CAllyCover::CAllyCover(CCircuitAI* circuit, float weight)
		: circuit(circuit)
		, weight(weight)
		, discFrame(-1)
{
	if (IsOn()) {
		// The defences layer of the threat map is shared by the ally team: on once any of its AIs uses cover
		circuit->GetThreatMap()->EnableCover();
	}
}

CAllyCover::~CAllyCover()
{
}

CAllyCover::Source CAllyCover::SParts::GetBiggest() const
{
	return static_cast<Source>(std::max_element(at, at + SOURCE_COUNT) - at);
}

const std::vector<CAllyCover::SDisc>& CAllyCover::GetDiscs()
{
	const int frame = circuit->GetLastFrame();
	if (discFrame == frame) {
		return discs;
	}
	discFrame = frame;
	discs.clear();
	if (!IsOn()) {
		return discs;
	}
	// Our attack groups where they stand, whatever their goal; a fleeing one gives none (CAttackTask::GetCoverDisc)
	for (IFighterTask* t : circuit->GetMilitaryManager()->GetTasks(IFighterTask::FightType::ATTACK)) {
		CAttackTask* task = static_cast<CAttackTask*>(t);
		AIFloat3 pos;
		float range, power;
		if (task->GetCoverDisc(frame, pos, range, power)) {
			discs.push_back({task, pos, SQUARE(range), power});
		}
	}
	// Our commander (a builder, in no attack task), while it isn't retreating (its flee and hide use retreat tasks)
	CCircuitUnit* comm = circuit->GetSetupManager()->GetCommander();
	if ((comm != nullptr) && (comm->GetTask() != nullptr) && (comm->GetTask()->GetType() != IUnitTask::Type::RETREAT)
		&& !comm->GetCircuitDef()->IsAbleToFly() && !comm->GetUnit()->IsParalyzed())
	{
		const float range = GetCommRange(comm);
		const float power = comm->GetCircuitDef()->GetPower() * sqrtf(std::max(comm->GetHealthPercent(), 0.f));
		const AIFloat3& pos = comm->GetPos(frame);
		if ((range > 0.f) && (power > 0.f) && utils::is_valid(pos)) {
			discs.push_back({nullptr, pos, SQUARE(range), power});
		}
	}
	return discs;
}

float CAllyCover::GetCommRange(CCircuitUnit* comm) const
{
	CCircuitDef* cdef = comm->GetCircuitDef();
	if (cdef->IsRoleComm()) {
		// Dynamic commanders: the def's weapons aren't the ones fitted (as the threat map reads an enemy commander's)
		float range, aoe;
		CCircuitDef::GetCommWeaponRange(circuit, comm->GetUnit(), range, aoe);
		if (range > 0.f) {
			return range;
		}
	}
	return cdef->GetMaxRange(CCircuitDef::RangeType::LAND);  // its real reach, as the statics' (no threat-range slack)
}

CAllyCover::SParts CAllyCover::GetCoverAt(const AIFloat3& p, const SJudge& judge)
{
	// Water: the defences layer has none there, and neither do the discs (as the route layer)
	SParts parts;
	CThreatMap* threatMap = circuit->GetThreatMap();
	if (threatMap->IsWaterAt(p)) {
		return parts;
	}
	parts.at[static_cast<int>(Source::DEFENCES)] = threatMap->GetDefenceCoverAt(p);
	for (const SDisc& d : GetDiscs()) {
		if (IsLeftOut(d, judge)) {
			continue;
		}
		const float sqDist = p.SqDistance2D(d.pos);
		if (sqDist < d.sqRange) {
			const Source source = (d.task == nullptr) ? Source::COMMANDER : Source::GROUPS;
			parts.at[static_cast<int>(source)] += d.power * Falloff(sqDist, d.sqRange);
		}
	}
	return parts;
}

float CAllyCover::GetMod(const AIFloat3& p, const SJudge& judge, Source& outSource)
{
	const bool isTimed = (stats.lookups++ % LOOKUP_SAMPLE == 0);
	const auto startTime = isTimed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();

	const SParts parts = GetCoverAt(p, judge);
	const float cover = parts.GetSum();
	const float mod = (cover > 0.f) ? (1.f + std::min(weight * cover / judge.base, 1.f)) : 1.f;
	outSource = parts.GetBiggest();

	if (isTimed) {
		stats.lookupMs += LOOKUP_SAMPLE
				* std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - startTime).count();
	}
	stats.maxMod = std::max(stats.maxMod, mod);
	return mod;
}

std::shared_ptr<FloatVec> CAllyCover::MakeRouteLayer(const float* threat, const SJudge& judge)
{
	const auto startTime = std::chrono::steady_clock::now();
	++stats.routes;
	CThreatMap* threatMap = circuit->GetThreatMap();
	const int width = threatMap->GetThreatMapWidth();
	const int height = threatMap->GetThreatMapHeight();
	const float squareSize = threatMap->GetSquareSize();
	auto finish = [&]() {
		stats.routeMs += std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - startTime).count();
	};

	// The box around all cover we count: the defences layer's and that of each disc that counts. None: no change
	struct SDiscCells {
		const SDisc* disc;
		CThreatMap::SCoverBox box;
	};
	static std::vector<SDiscCells> counted;  // NOTE: micro-opt
	counted.clear();
	CThreatMap::SCoverBox box = threatMap->GetCoverBox();
	for (const SDisc& d : GetDiscs()) {
		if (IsLeftOut(d, judge)) {
			continue;
		}
		const float range = sqrtf(d.sqRange);
		const CThreatMap::SCoverBox b = {
			std::max(static_cast<int>((d.pos.x - range) / squareSize), 0),
			std::max(static_cast<int>((d.pos.z - range) / squareSize), 0),
			std::min(static_cast<int>((d.pos.x + range) / squareSize) + 1, width),
			std::min(static_cast<int>((d.pos.z + range) / squareSize) + 1, height)
		};
		if (!b.IsEmpty()) {
			counted.push_back({&d, b});
			box.Add(b);
		}
	}
	if (box.IsEmpty()) {
		finish();
		return nullptr;
	}

	// Cover over the box: the defences layer, plus the discs at cell centres; none on water
	const int boxWidth = box.x1 - box.x0;
	static FloatVec cover;  // NOTE: micro-opt
	cover.resize(boxWidth * (box.z1 - box.z0));
	const float* layer = threatMap->GetCoverArray();
	for (int z = box.z0; z < box.z1; ++z) {
		const float* row = layer + z * width;
		std::copy(row + box.x0, row + box.x1, cover.begin() + (z - box.z0) * boxWidth);
	}
	for (const SDiscCells& dc : counted) {
		const SDisc& d = *dc.disc;
		for (int z = dc.box.z0; z < dc.box.z1; ++z) {
			const float dz = (z + 0.5f) * squareSize - d.pos.z;
			for (int x = dc.box.x0; x < dc.box.x1; ++x) {
				const float dx = (x + 0.5f) * squareSize - d.pos.x;
				const float sqDist = dx * dx + dz * dz;
				if ((sqDist >= d.sqRange) || threatMap->IsWaterCell(z * width + x)) {
					continue;
				}
				cover[(z - box.z0) * boxWidth + (x - box.x0)] += d.power * Falloff(sqDist, d.sqRange);
			}
		}
	}

	// e = t / the multiplier where both are; a copy of the threat layer only once a cell changes
	const float factor = weight / judge.base;
	std::shared_ptr<FloatVec> result;
	for (int z = box.z0; z < box.z1; ++z) {
		for (int x = box.x0; x < box.x1; ++x) {
			const int index = z * width + x;
			const float t = threat[index] - THREAT_BASE;
			const float c = cover[(z - box.z0) * boxWidth + (x - box.x0)];
			if ((t <= 0.f) || (c <= 0.f)) {
				continue;
			}
			if (result == nullptr) {
				result = std::make_shared<FloatVec>(threat, threat + threatMap->GetMapSize());
			}
			(*result)[index] = THREAT_BASE + t / (1.f + std::min(factor * c, 1.f));
		}
	}
	if (result != nullptr) {
		++stats.routeLayers;
	}
	finish();
	return result;
}

void CAllyCover::Log()
{
	// For tuning quota.attack_ally_cover: the fights cover licensed, how many of them ended in a quick escape (by
	// source), escapes on losing cover mid-fight, how strong it got, and its main-thread cost
	SStats& st = stats;
	int groups = 0;
	bool isComm = false;
	for (const SDisc& d : GetDiscs()) {
		if (d.task == nullptr) {
			isComm = true;
		} else {
			++groups;
		}
	}
	circuit->LOG("ATTACK cover last 60 s: %i targets picked only with cover (defences %i, groups %i, commander %i),"
			" %i escapes within 5 s of engaging one (%i, %i, %i), %i escapes on losing cover; strongest cover x%.2f;"
			" %i lookups about %.2f ms, %i route layers (%i changed) %.2f ms | now: %i defences, %i groups%s",
			st.picks[0] + st.picks[1] + st.picks[2], st.picks[0], st.picks[1], st.picks[2],
			st.quickEscapes[0] + st.quickEscapes[1] + st.quickEscapes[2], st.quickEscapes[0], st.quickEscapes[1],
			st.quickEscapes[2], st.coverLossEscapes, st.maxMod, st.lookups, st.lookupMs, st.routes, st.routeLayers,
			st.routeMs, circuit->GetThreatMap()->GetCoverSourceCount(), groups, isComm ? " and the commander" : "");
	st = SStats();
}

} // namespace circuit

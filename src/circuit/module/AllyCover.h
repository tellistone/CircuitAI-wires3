/*
 * AllyCover.h
 *
 *  Created on: Nov 16, 2026
 *      Author: Claude
 */

#ifndef SRC_CIRCUIT_MODULE_ALLYCOVER_H_
#define SRC_CIRCUIT_MODULE_ALLYCOVER_H_

#include "util/Defines.h"

#include "AIFloat3.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace circuit {

class CCircuitAI;
class CCircuitUnit;
class CAttackTask;
class CProtect;

/*
 * Allied cover (quota.attack_ally_cover): allies over a spot multiply an attack group's strength there by
 * 1 + min(weight x cover / its strength, 1), at most double, in every danger check (CAttackTask::GetAllowedThreatAt)
 * and in its routes (MakeRouteLayer), so the pick, the approach, the escape and the routes always agree. Cover at a
 * spot, each source as its power x Falloff(distance / its reach), none on water:
 * - defences: built allied armed statics of every allied team, over their weapon's land range (the threat map's
 *   layer, CThreatMap::PrepareCover; drawn on its worker and shared by the ally team);
 * - groups: our attack groups that aren't fleeing, their strength around their leader out to their fire range past
 *   their units' spread (CAttackTask::GetCoverDisc);
 * - commander: ours while it isn't retreating, its power x sqrt(health share) out to its weapon range.
 * Never the judging group's own disc, nor its partner's in a joint check (SJudge). Off (weight 0): nothing is
 * collected and the threat map draws nothing.
 */
class CAllyCover {
public:
	enum class Source: int {DEFENCES = 0, GROUPS, COMMANDER, _SIZE_};
	static constexpr int SOURCE_COUNT = static_cast<int>(Source::_SIZE_);
	// Cover at a spot, by source
	struct SParts {
		float at[SOURCE_COUNT] = {0.f, 0.f, 0.f};
		float GetSum() const { return at[0] + at[1] + at[2]; }
		Source GetBiggest() const;
	};
	// Who judges: its own disc is never cover, nor its partner's in a joint check (both groups' strength, which has
	// the partner in it already); self nullptr: not an attack group (anti-heavy). Cover is weighed against base (> 0)
	struct SJudge {
		const CAttackTask* self;
		const CAttackTask* partner;  // nullptr: not a joint check
		float base;
	};

	CAllyCover(CCircuitAI* circuit, float weight);
	~CAllyCover();

	float GetWeight() const { return weight; }
	bool IsOn() const { return weight > 0.f; }

	// w(u) = min(1, 2 x (1 - u)), u = distance / reach (sqDist < sqRange): full out to half the reach, then linear to 0
	// at it. No cliff at the edge, so a group just inside it doesn't judge twice as boldly as one just outside
	static float Falloff(float sqDist, float sqRange) {
		return std::min(1.f, 2.f * (1.f - sqrtf(sqDist / sqRange)));
	}
	// The multiplier for cover > 0, factor = weight / base: 1 + min(factor x cover, 1). The danger checks' and the
	// routes' alike
	static float Multiplier(float cover, float factor) {
		return 1.f + std::min(factor * cover, 1.f);
	}

	SParts GetCoverAt(const springai::AIFloat3& p, const SJudge& judge);
	// 1 + min(weight x cover / judge.base, 1); outSource: the biggest source there
	float GetMod(const springai::AIFloat3& p, const SJudge& judge, Source& outSource);
	// A route's threat layer with cover: threat / the multiplier at each cell with both (discs at cell centres), so
	// its cap, cost, light-threat floor and straightening match the danger checks. Only the box around the cover is
	// scanned. nullptr: no cell changes (no cover, or none where there is threat)
	std::shared_ptr<FloatVec> MakeRouteLayer(const float* threat, const SJudge& judge);
	// Mobile cover changed within the frame (a merge, a group starting to run): the next lookup collects it again
	void Invalidate() { discFrame = -1; }

	/*
	 * The ATTACK cover log, every minute while on, for tuning the weight: fights cover licensed and how they ended,
	 * losses where only cover kept a unit standing, the strongest multiplier, and the main-thread time spent
	 */
	// A target that passed only with cover (its own spot, its approach line or its route's approach), engaged
	void NoteEngagedPick(int source) { ++stats.picks[source]; }
	// An escape within 5 s of engaging such a pick, by the source that licensed it
	void NoteQuickEscape(int source) { ++stats.quickEscapes[source]; }
	// An escape with the multiplier at the leader under 0.75x what it was when the fight was engaged: isCoverLeft,
	// the cover at the spot we engaged from fell as well (it moved off, or died); else we walked out of it
	void NoteCoverLossEscape(bool isCoverLeft) { ++(isCoverLeft ? stats.coverLeftEscapes : stats.walkedOutEscapes); }
	// An attack group's unit died (metal: its cost); source >= 0: the threat where it stood was over its group's
	// escape allowance without cover and under it with cover (only cover kept it there), by the biggest source
	void NoteLoss(float metal, int source);
	void Log();

private:
	// Mobile cover, once a frame on first use (no lookup walks another group's units)
	struct SDisc {
		const CAttackTask* task;  // only compared, never dereferenced; nullptr: the commander
		springai::AIFloat3 pos;  // the group's leader, the commander
		float sqRange;
		float power;
	};
	const std::vector<SDisc>& GetDiscs();
	// The commander's disc (task nullptr) never is. protect: the commander protect while judge.self is committed to it
	// (CProtect::IsActing), else nullptr: the other committed groups are left out too, their strength is in the
	// judging group's already (CProtect::GetBoost)
	static bool IsLeftOut(const SDisc& d, const SJudge& judge, const CProtect* protect);
	const CProtect* GetJudgeProtect(const SJudge& judge) const;
	// The commander's reach: its fitted weapons' longest range (dynamic commanders), else its def's land range
	float GetCommRange(CCircuitUnit* comm) const;

	CCircuitAI* circuit;
	float weight;  // quota.attack_ally_cover
	std::vector<SDisc> discs;  // GetDiscs, for discFrame
	int discFrame;

	struct SStats {
		int picks[SOURCE_COUNT] = {0, 0, 0};  // NoteEngagedPick, by source
		int quickEscapes[SOURCE_COUNT] = {0, 0, 0};  // NoteQuickEscape, by source
		int coverLeftEscapes = 0;  // NoteCoverLossEscape
		int walkedOutEscapes = 0;
		int losses = 0;  // NoteLoss: all, and those only cover kept standing (by source), with their metal
		float lossMetal = 0.f;
		int coverLosses[SOURCE_COUNT] = {0, 0, 0};
		float coverLossMetal = 0.f;
		float maxMod = 1.f;  // the strongest multiplier any lookup gave
		int lookups = 0;  // GetMod calls (spots where cover can decide) and their time, sampled
		float lookupMs = 0.f;
		int routes = 0;  // route layers worked out, those that changed a cell, and their time
		int routeLayers = 0;
		float routeMs = 0.f;
	} stats;
};

} // namespace circuit

#endif // SRC_CIRCUIT_MODULE_ALLYCOVER_H_

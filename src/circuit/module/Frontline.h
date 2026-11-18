/*
 * Frontline.h
 *
 *  Created on: Nov 11, 2026
 *      Author: Claude
 */

#ifndef SRC_CIRCUIT_MODULE_FRONTLINE_H_
#define SRC_CIRCUIT_MODULE_FRONTLINE_H_

#include "AIFloat3.h"

#include <map>
#include <vector>

namespace circuit {

class CCircuitAI;
class CCircuitUnit;
class IFighterTask;

/*
 * Chokes by what they mean for the army, every 10 s on CChokeMap's lines (choke ids, so a rebuild can't mix them up):
 * - E, rankings: every enemy-held choke (its outer side from us is the enemy's held ground, or known enemy towers
 *   cover its line) and every choke of the enemy line, by its defence (enemy towers reaching the line or our approach
 *   to it, plus enemy mobiles seen in the last minute near it), what we saw of it in the last minute (the lower
 *   of the shares of its line and of the ground a main-tower range beyond it on the enemy's side), and the unarmed
 *   enemy value directly behind it. One is undefended when no armed enemy is in reach and we saw a quarter of it.
 * - C, quota.hold_choke: the current line's chokes ranked by our income behind each one x its traffic. The idle army
 *   holds at the top-ranked short one: our towers that hit ground don't cover all of its line, or our towers are
 *   short of the enemy army outside it (x 1.2), where each enemy mobile seen in the last minute in a region our line
 *   opens into counts at the nearest of those chokes in full and at the others by half. 1 slot, 2 on chokepointed maps, none on open maps. A group claims a slot only if its power makes
 *   up the shortfall, and loses the claim when it hasn't asked for its spot in a minute. A claimed slot keeps its
 *   choke until 30 s after it was last short, unless a short one ranks 1.3x higher; then it moves to the top short
 *   one, or ends if none is. With choke.enable, the top-ranked short choke's pocket is fortified (MakeDefence).
 * The rankings only feed the overlay.
 */
class CFrontline {
public:
	struct SRank {
		int chokeId;
		float defence;  // threat
		float defenceCost;  // metal
		float confidence;  // the lower share of its line's and the ground beyond's samples in our LOS in the last minute
		float value;  // metal
		bool isHeld;  // its outer side is enemy ground, or enemy towers cover its line (else only on the enemy line)
		bool isUndefended;  // no armed enemy in reach, and confidence of a quarter or more
	};
	struct SFront {
		int chokeId;
		int innerRegionId;  // its side towards our ground
		float rank;  // income behind x traffic
		float wanted;  // threat of enemy mobiles seen in the last minute in the region outside it (shared, see C) x 1.2
		float wantedCost;  // their metal x 1.2
		float towers;  // threat of our towers near it
		float cover;  // the share of its line our towers that hit ground cover
		float GetShortfall() const { return wanted - towers; }
		// Our towers that hit ground don't cover all of its line, or are short of the enemy army outside it
		bool IsShort() const { return (cover < 1.f) || (GetShortfall() > 0.f); }
	};
	struct SHold {
		int chokeId = -1;
		springai::AIFloat3 spot = -RgtVector;
		const IFighterTask* task = nullptr;  // the attack group holding it
		int retryFrame = 0;  // no hold spot was found: no new claim before
		int shortFrame = 0;  // its choke was last short then
		int askFrame = 0;  // its group last asked for the spot then
	};

	CFrontline(CCircuitAI* circuit);
	virtual ~CFrontline();

	void Update();  // every 10 s

	// quota.hold_choke: where the attack group led by leader holds (claiming a free slot once it has a spot there),
	// -RgtVector if nowhere
	springai::AIFloat3 GetHoldSpot(CCircuitUnit* leader);
	// The task ended, or ran from danger: its claim goes, and it may not claim again for banFrames
	void ReleaseHold(const IFighterTask* task, int banFrames);

	// choke_overlay
	const std::vector<SRank>& GetRanks() const { return ranks; }  // weakest first
	const std::vector<SFront>& GetFront() const { return front; }  // highest rank first
	const std::vector<SHold>& GetHolds() const { return holds; }

private:
	struct SArmed {
		springai::AIFloat3 pos;
		bool isMobile;
		float range;
		float threat;
		float cost;
	};
	void UpdateRanks(const std::vector<SArmed>& armed);
	void UpdateFront(const std::vector<SArmed>& armed);
	void UpdateHolds();
	void Fortify();
	springai::AIFloat3 FindHoldSpot(int chokeId, CCircuitUnit* leader) const;  // -RgtVector if none
	// Regions reachable from our held ground (our side before we hold any) without crossing these chokes
	std::vector<bool> ReachableFromUs(const std::vector<int>& blocked) const;
	const SFront* FindFront(int chokeId) const;

	CCircuitAI* circuit;
	std::vector<SRank> ranks;
	std::vector<SFront> front;
	std::vector<SHold> holds;
	std::map<const IFighterTask*, int> holdBans;  // task -> frame it may claim again
	std::map<int, std::vector<int>> sampleSeen;  // choke id -> per sample (its line's, then beyond): last in our LOS
};

} // namespace circuit

#endif // SRC_CIRCUIT_MODULE_FRONTLINE_H_

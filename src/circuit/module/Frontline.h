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
 * - E, rankings: every enemy-held choke (its outer side from us is enemy territory, or known enemy towers cover its
 *   line) and every choke of the enemy line, by its defence (enemy towers reaching the line or our approach to it,
 *   plus enemy mobiles near it), the share of it we saw in the last 3 minutes, and the unarmed enemy value directly
 *   behind it. One is undefended when no armed enemy is in reach and we saw a quarter of it lately.
 * - C, quota.hold_choke: the current line's chokes ranked by our income behind each one x its traffic. The idle army
 *   holds at the top-ranked one whose towers are short of the enemy army outside it (x 1.2): each enemy mobile in a
 *   region our line opens into counts at the nearest of those chokes in full and at the others by half. 1 slot, 2 on
 *   chokepointed maps, none on open maps. A group claims a slot only if its power makes up the shortfall, and loses
 *   the claim when it hasn't asked for its spot in a minute. A claimed slot keeps its choke until 30 s after it was
 *   last short, unless a short one ranks 1.3x higher; then it moves to the top short one, or ends if none is. With
 *   choke.enable, the top-ranked short choke's pocket is fortified (MakeDefence).
 * - D, choke.objective.enable: when the forward line is shorter than the current line (or as long, with more
 *   income behind it), its chokes become an objective if the gain (the retired chokes' towers and wanted power, plus
 *   3 minutes of the income it brings behind the line) is choke.objective.gain x the cost (their defence plus what
 *   holding them takes), and our attack power is choke.objective.power x their defence. Their defence is what we
 *   saw over the share of their line we saw lately (at least a quarter). Idle attack groups head there. It ends
 *   once taken, after 4 minutes, or when our power falls below their defence now (retried after
 *   choke.objective.retry)
 */
class CFrontline {
public:
	struct SRank {
		int chokeId;
		float defence;  // threat
		float defenceCost;  // metal
		float confidence;  // share of its line's samples in our LOS within the last 3 minutes
		float value;  // metal
		bool isHeld;  // its outer side is enemy territory, or enemy towers cover its line (else only on the enemy line)
		bool isUndefended;  // no armed enemy in reach, and confidence of a quarter or more
	};
	struct SFront {
		int chokeId;
		int innerRegionId;  // its side towards our territory
		float rank;  // income behind x traffic
		float wanted;  // threat of enemy mobiles in the region outside it (shared, see C) x 1.2
		float wantedCost;  // their metal x 1.2
		float towers;  // threat of our towers near it
		float GetShortfall() const { return wanted - towers; }  // short when above 0
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
	// choke.objective.enable: where idle attack groups go, empty if no objective
	const std::vector<springai::AIFloat3>& GetObjective() const { return objective.positions; }

	// choke_overlay
	const std::vector<SRank>& GetRanks() const { return ranks; }  // weakest first
	const std::vector<SFront>& GetFront() const { return front; }  // highest rank first
	const std::vector<SHold>& GetHolds() const { return holds; }
	const std::vector<int>& GetObjectiveChokes() const { return objective.chokeIds; }

private:
	struct SObjective {
		std::vector<int> chokeIds;
		std::vector<springai::AIFloat3> positions;
		float score = 0.f;  // gain / cost
		float defence = 0.f;  // threat
		int startFrame = -1;
	};
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
	void UpdateObjective();
	SObjective MakeObjective(float& outArmyPower) const;
	springai::AIFloat3 FindHoldSpot(int chokeId, CCircuitUnit* leader) const;  // -RgtVector if none
	// Regions reachable from our territory (our side before we hold any) without crossing these chokes
	std::vector<bool> ReachableFromUs(const std::vector<int>& blocked) const;
	float GetIncomeIn(const std::vector<bool>& regions) const;  // of every defence point in them
	const SRank* FindHeld(int chokeId) const;  // an enemy-held choke's rank, nullptr if it isn't held
	const SFront* FindFront(int chokeId) const;

	CCircuitAI* circuit;
	std::vector<SRank> ranks;
	std::vector<SFront> front;
	std::vector<SHold> holds;
	std::map<const IFighterTask*, int> holdBans;  // task -> frame it may claim again
	std::map<int, std::vector<int>> sampleSeen;  // choke id -> per line sample: frame last in our LOS
	SObjective objective;
	int retryFrame;  // no new objective before
};

} // namespace circuit

#endif // SRC_CIRCUIT_MODULE_FRONTLINE_H_

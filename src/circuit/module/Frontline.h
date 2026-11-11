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
 *   line) by its defence (enemy towers reaching the line or our approach to it, plus enemy mobiles near it), the
 *   share of it we saw in the last 3 minutes, and the unarmed enemy value directly behind it.
 * - C, quota.hold_choke: the current line's chokes ranked by our income behind each one x its traffic. The idle army
 *   holds at the top-ranked one whose towers are short of the enemy army near it (x 1.2): 1 slot, 2 on chokepointed
 *   maps, none on open maps. A slot moves only to a choke ranked 1.3x higher. With choke.enable, the top-ranked
 *   short choke's pocket is fortified (MakeDefence) too.
 * - D, choke.objective.enable: when the forward line is shorter than the current line (or as long, with more
 *   income behind it), its chokes become an objective if the gain (the retired chokes' towers and wanted power, plus
 *   3 minutes of the income it brings behind the line) is choke.objective.gain x the cost (their defence plus what
 *   holding them takes), and our attack power is choke.objective.power x their defence. Idle attack groups head
 *   there. It ends once taken, after 4 minutes, or when our power falls below their defence (retried after
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
	};
	struct SFront {
		int chokeId;
		int innerRegionId;  // its side towards our territory
		float rank;  // income behind x traffic
		float wanted;  // threat of enemy mobiles near it x 1.2
		float wantedCost;  // their metal x 1.2
		float towers;  // threat of our towers near it
	};
	struct SHold {
		int chokeId = -1;
		springai::AIFloat3 spot = -RgtVector;
		const IFighterTask* task = nullptr;  // the attack group holding it
		int retryFrame = 0;  // no hold spot was found: no new claim before
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
	void UpdateRanks();
	void UpdateFront();
	void UpdateHolds();
	void Fortify();
	void UpdateObjective();
	SObjective MakeObjective(float& outArmyPower) const;
	springai::AIFloat3 FindHoldSpot(int chokeId, CCircuitUnit* leader) const;  // -RgtVector if none
	// Regions reachable from our territory (our side before we hold any) without crossing these chokes
	std::vector<bool> ReachableFromUs(const std::vector<int>& blocked) const;
	float GetIncomeIn(const std::vector<bool>& regions) const;  // of every defence point in them
	const SRank* FindRank(int chokeId) const;

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

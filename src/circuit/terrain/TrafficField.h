/*
 * TrafficField.h
 *
 *  Created on: Nov 11, 2026
 *      Author: Claude
 */

#ifndef SRC_CIRCUIT_TERRAIN_TRAFFICFIELD_H_
#define SRC_CIRCUIT_TERRAIN_TRAFFICFIELD_H_

#include "terrain/CellMask.h"

#include <cstdint>
#include <vector>

namespace circuit {

/*
 * Where enemies are likely to walk: KAIK's choke map, done deterministically. For each enemy movement type, each
 * enemy source (an ally team's start box, then its factories) and each of our destinations:
 * - De: walking distance from the source, Dg: from the destination (chamfer floods, CDistanceFlood);
 * - a cell's detour is De + Dg - De(g). Cells within 3 tau of the shortest route form its corridor, weighted
 *   exp(-detour / tau) with tau = spread x De(g): every near-shortest route, back doors included in proportion to
 *   how short they are, as KAIK's pushed-apart reruns approximate;
 * - density: corridor cells are grouped by De, one group per cell length, and each cell's weight is divided by its
 *   group's total, so every cross-section carries the destination's whole value: traffic piles up where corridors
 *   narrow and thins out where they're wide. Cells within skipRadius of the destination are left out.
 * Sources share a destination's value equally. Each type's field is kept apart, and Blend weights them by the
 * enemy's army mix. No engine types: built on the area worker and tested on its own
 */
class CTrafficField {
public:
	using Cells = std::vector<int>;
	struct SDestination {
		Cells cells;
		float value;
	};
	struct SParams {
		float spread = 0.15f;  // traffic.spread
		float skipRadius = 0.f;  // elmos
	};
	struct SType {
		int key = -1;  // movement class (CChokeMap::MoveKey)
		std::vector<float> field;
		std::vector<std::vector<uint16_t>> sourceDist;  // De per source, NO_DIST where unreached
	};
	static constexpr uint16_t NO_DIST = 0xFFFF;

	// One type's floods and field. sources: one cell list per enemy source
	static SType BuildType(int key, const SCellMask& mask, const std::vector<Cells>& sources,
			const std::vector<SDestination>& destinations, const SParams& params);

public:
	CTrafficField();

	void Init(int width, int height, int cellSize, const SParams& params);
	bool IsEmpty() const { return types.empty(); }
	int GetWidth() const { return width; }
	int GetHeight() const { return height; }
	int GetCellSize() const { return cellSize; }
	int IndexAt(float x, float z) const;
	const std::vector<SType>& GetTypes() const { return types; }
	void SetType(SType&& type);  // replaces the type with the same key, or adds it
	void RemoveType(int key);
	// Blended field: each type's field x its share (key, share)
	void Blend(const std::vector<std::pair<int, float>>& shares);
	const std::vector<std::pair<int, float>>& GetShares() const { return shares; }

	float GetAt(int cell) const { return blended.empty() ? 0.f : blended[cell]; }
	float GetAt(float x, float z) const { return blended.empty() ? 0.f : blended[IndexAt(x, z)]; }
	float GetMax() const { return maxValue; }
	/*
	 * A position's own approach, for any asset (even one away from the fixed destinations): the weight, 0..1, of
	 * cell x being on a near-shortest route from the enemy to the asset, from the De floods: detour = De(x) +
	 * |x - asset| - De(asset) under 3 tau, weighted and blended as the field
	 */
	float GetApproachWeight(int cell, int assetCell, float distToAsset) const;

private:
	SParams params;
	int width, height, cellSize;
	std::vector<SType> types;
	std::vector<std::pair<int, float>> shares;
	std::vector<float> blended;
	float maxValue;
};

} // namespace circuit

#endif // SRC_CIRCUIT_TERRAIN_TRAFFICFIELD_H_

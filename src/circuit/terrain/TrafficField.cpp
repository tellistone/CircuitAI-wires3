/*
 * TrafficField.cpp
 *
 *  Created on: Nov 11, 2026
 *      Author: Claude
 */

#include "terrain/TrafficField.h"

#include <algorithm>
#include <cmath>

namespace circuit {

CTrafficField::SType CTrafficField::BuildType(int key, const SCellMask& mask, const std::vector<Cells>& sources,
		const std::vector<SDestination>& destinations, const SParams& params)
{
	const int n = mask.Size();
	const size_t sourceCount = sources.size();
	SType type;
	type.key = key;
	type.field.assign(n, 0.f);
	type.sourceDist.resize(sourceCount);
	std::vector<int> dist;
	for (size_t k = 0; k < sourceCount; ++k) {
		CDistanceFlood::Run(mask, sources[k], CDistanceFlood::UNREACHED / 2, dist);
		type.sourceDist[k].resize(n);
		for (int i = 0; i < n; ++i) {
			type.sourceDist[k][i] = (dist[i] < NO_DIST) ? dist[i] : NO_DIST;
		}
	}

	const int skip = CDistanceFlood::FromElmos(mask, params.skipRadius);
	std::vector<int> deG(sourceCount), maxDetour(sourceCount), reached, corridor;
	std::vector<float> tau(sourceCount), weight(n, 0.f);
	std::vector<double> levelSum;
	for (const SDestination& dest : destinations) {
		// A corridor cell has De + Dg <= De(g) + 3 tau: the destination's flood stops at the largest such bound
		int bound = -1;
		for (size_t k = 0; k < sourceCount; ++k) {
			deG[k] = NO_DIST;
			for (int c : dest.cells) {
				deG[k] = std::min<int>(deG[k], type.sourceDist[k][c]);
			}
			tau[k] = std::max(params.spread * deG[k], 1.f);
			maxDetour[k] = int(3.f * tau[k]);
			if ((deG[k] != NO_DIST) && (deG[k] != 0)) {  // else this source can't walk there, or stands on it
				bound = std::max(bound, deG[k] + maxDetour[k]);
			}
		}
		if (bound < 0) {
			continue;
		}
		CDistanceFlood::Run(mask, dest.cells, bound, dist, &reached);
		const double value = dest.value / sourceCount;  // sources share it equally
		for (size_t k = 0; k < sourceCount; ++k) {
			if ((deG[k] == NO_DIST) || (deG[k] == 0)) {
				continue;
			}
			const std::vector<uint16_t>& de = type.sourceDist[k];
			levelSum.assign((deG[k] + maxDetour[k]) / 2 + 1, 0.0);
			corridor.clear();
			for (int i : reached) {
				if ((de[i] == NO_DIST) || (dist[i] < skip)) {
					continue;
				}
				const int detour = de[i] + dist[i] - deG[k];  // >= 0: chamfer distance is a metric
				if (detour > maxDetour[k]) {
					continue;
				}
				weight[i] = std::exp(-detour / tau[k]);
				levelSum[de[i] / 2] += weight[i];
				corridor.push_back(i);
			}
			for (int i : corridor) {
				type.field[i] += value * weight[i] / levelSum[de[i] / 2];
			}
		}
	}
	return type;
}

CTrafficField::CTrafficField()
		: width(0)
		, height(0)
		, cellSize(1)
{
}

void CTrafficField::Init(int w, int h, int size, const SParams& p)
{
	width = w;
	height = h;
	cellSize = size;
	params = p;
	types.clear();
	blendShares.clear();
	blended.clear();
}

int CTrafficField::IndexAt(float x, float z) const
{
	const int cx = std::min(std::max(int(x) / cellSize, 0), width - 1);
	const int cz = std::min(std::max(int(z) / cellSize, 0), height - 1);
	return cz * width + cx;
}

void CTrafficField::SetType(SType&& type)
{
	for (SType& t : types) {
		if (t.key == type.key) {
			t = std::move(type);
			return;
		}
	}
	types.push_back(std::move(type));
}

void CTrafficField::RemoveType(int key)
{
	types.erase(std::remove_if(types.begin(), types.end(), [key](const SType& t) { return t.key == key; }),
			types.end());
	blendShares.clear();  // its indices moved
}

void CTrafficField::Blend(const std::vector<std::pair<int, float>>& shares)
{
	const int n = width * height;
	blendShares.clear();
	blended.assign(n, 0.f);
	for (const std::pair<int, float>& ks : shares) {
		for (size_t t = 0; t < types.size(); ++t) {
			if ((types[t].key == ks.first) && ((int)types[t].field.size() == n)) {
				blendShares.push_back({(int)t, ks.second});
				for (int i = 0; i < n; ++i) {
					blended[i] += ks.second * types[t].field[i];
				}
			}
		}
	}
}

float CTrafficField::GetApproachWeight(int cell, int assetCell, float distToAsset) const
{
	const float straight = distToAsset * 2.f / cellSize;  // chamfer units: 2 per cell length
	float weight = 0.f;
	for (const std::pair<int, float>& ts : blendShares) {
		const SType& t = types[ts.first];
		for (const std::vector<uint16_t>& de : t.sourceDist) {
			const uint16_t deAsset = de[assetCell];
			const uint16_t deCell = de[cell];
			if ((deAsset == NO_DIST) || (deCell == NO_DIST) || (deAsset == 0)) {
				continue;
			}
			const float tau = std::max(params.spread * deAsset, 1.f);
			const float detour = std::max(deCell + straight - deAsset, 0.f);
			if (detour < 3.f * tau) {
				weight += ts.second / t.sourceDist.size() * std::exp(-detour / tau);
			}
		}
	}
	return weight;
}

} // namespace circuit

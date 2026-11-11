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
	constexpr int UNREACHED = CDistanceFlood::UNREACHED;
	const int n = mask.Size();
	SType type;
	type.key = key;
	type.field.assign(n, 0.f);
	type.sourceDist.resize(sources.size());
	if (sources.empty()) {
		return type;
	}
	const int skip = CDistanceFlood::FromElmos(mask, params.skipRadius);
	const float sourceShare = 1.f / sources.size();
	std::vector<int> de, dg, reached, corridor;
	std::vector<float> weight(n, 0.f);
	std::vector<double> levelSum;
	for (size_t k = 0; k < sources.size(); ++k) {
		CDistanceFlood::Run(mask, sources[k], UNREACHED / 2, de);
		std::vector<uint16_t>& dist = type.sourceDist[k];
		dist.resize(n);
		for (int i = 0; i < n; ++i) {
			dist[i] = (de[i] < NO_DIST) ? de[i] : NO_DIST;
		}

		for (const SDestination& dest : destinations) {
			int deG = UNREACHED;
			for (int c : dest.cells) {
				deG = std::min(deG, de[c]);
			}
			if ((deG == UNREACHED) || (deG == 0)) {
				continue;  // this source can't walk there, or stands on it
			}
			const float tau = std::max(params.spread * deG, 1.f);
			const int maxDetour = int(3.f * tau);
			// A corridor cell has De + Dg <= De(g) + 3 tau, so the destination's flood can stop there
			CDistanceFlood::Run(mask, dest.cells, deG + maxDetour, dg, &reached);
			levelSum.assign((deG + maxDetour) / 2 + 1, 0.0);
			corridor.clear();
			for (int i : reached) {
				if ((de[i] == UNREACHED) || (dg[i] < skip)) {
					continue;
				}
				const int detour = de[i] + dg[i] - deG;  // >= 0: chamfer distance is a metric
				if (detour > maxDetour) {
					continue;
				}
				weight[i] = std::exp(-detour / tau);
				levelSum[de[i] / 2] += weight[i];
				corridor.push_back(i);
			}
			const double value = dest.value * sourceShare;
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
		, maxValue(0.f)
{
}

void CTrafficField::Init(int w, int h, int size, const SParams& p)
{
	width = w;
	height = h;
	cellSize = size;
	params = p;
	types.clear();
	shares.clear();
	blended.clear();
	maxValue = 0.f;
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
}

void CTrafficField::Blend(const std::vector<std::pair<int, float>>& newShares)
{
	shares = newShares;
	const int n = width * height;
	blended.assign(n, 0.f);
	for (const std::pair<int, float>& ks : shares) {
		for (const SType& t : types) {
			if ((t.key == ks.first) && ((int)t.field.size() == n)) {
				for (int i = 0; i < n; ++i) {
					blended[i] += ks.second * t.field[i];
				}
			}
		}
	}
	maxValue = 0.f;
	for (float v : blended) {
		maxValue = std::max(maxValue, v);
	}
}

float CTrafficField::GetApproachWeight(int cell, int assetCell, float distToAsset) const
{
	const float straight = distToAsset * 2.f / cellSize;  // chamfer units: 2 per cell length
	float weight = 0.f;
	for (const std::pair<int, float>& ks : shares) {
		for (const SType& t : types) {
			if ((t.key != ks.first) || t.sourceDist.empty()) {
				continue;
			}
			const float perSource = ks.second / t.sourceDist.size();
			for (const std::vector<uint16_t>& de : t.sourceDist) {
				const uint16_t deAsset = de[assetCell];
				const uint16_t deCell = de[cell];
				if ((deAsset == NO_DIST) || (deCell == NO_DIST) || (deAsset == 0)) {
					continue;
				}
				const float tau = std::max(params.spread * deAsset, 1.f);
				const float detour = std::max(deCell + straight - deAsset, 0.f);
				if (detour < 3.f * tau) {
					weight += perSource * std::exp(-detour / tau);
				}
			}
		}
	}
	return weight;
}

} // namespace circuit

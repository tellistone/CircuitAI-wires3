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
	type.mask = mask;
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

std::vector<std::pair<int, float>> CTrafficField::GetApproach(int assetCell, float maxElmos) const
{
	for (const std::pair<int, float>& ts : blendShares) {
		const SType& t = types[ts.first];
		const int seed = t.mask.NearestPassable(assetCell, 2);  // an asset on a slope: the nearest cell it walks
		if (seed < 0) {
			continue;
		}
		CDistanceFlood::Rerun(t.mask, {seed}, CDistanceFlood::FromElmos(t.mask, maxElmos), floodDist, floodReached);
		approach.resize(floodDist.size(), 0.f);
		for (const std::vector<uint16_t>& de : t.sourceDist) {
			if ((de[seed] == NO_DIST) || (de[seed] == 0)) {
				continue;
			}
			const float tau = std::max(params.spread * de[seed], 1.f);
			const float share = ts.second / t.sourceDist.size();
			for (int cell : floodReached) {
				const int detour = de[cell] + floodDist[cell] - de[seed];  // >= 0: chamfer distance is a metric
				if ((de[cell] == NO_DIST) || (t.field[cell] <= 0.f) || (detour >= 3.f * tau)) {
					continue;
				}
				if (approach[cell] == 0.f) {
					touched.push_back(cell);
				}
				approach[cell] += share * t.field[cell] * std::exp(-detour / tau);
			}
		}
	}
	std::vector<std::pair<int, float>> out;
	for (int cell : touched) {  // nearest the asset first
		out.push_back({cell, approach[cell]});
		approach[cell] = 0.f;
	}
	touched.clear();
	return out;
}

float CTrafficField::GetEnemyDist(int cell, int reach) const
{
	float sum = 0.f;
	float shares = 0.f;
	for (const std::pair<int, float>& ts : blendShares) {
		const SType& t = types[ts.first];
		const int c = t.mask.NearestPassable(cell, reach);
		if (c < 0) {
			continue;
		}
		int de = NO_DIST;
		for (const std::vector<uint16_t>& d : t.sourceDist) {
			de = std::min<int>(de, d[c]);
		}
		if (de == NO_DIST) {
			continue;
		}
		sum += ts.second * CDistanceFlood::ToElmos(t.mask, de);
		shares += ts.second;
	}
	return (shares > 0.f) ? sum / shares : -1.f;
}

} // namespace circuit

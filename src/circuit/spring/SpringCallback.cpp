/*
 * SpringCallback.cpp
 *
 *  Created on: Nov 8, 2019
 *      Author: rlcevg
 */

#include "spring/SpringCallback.h"

#include "util/Defines.h"

#include "SSkirmishAICallback.h"	// "direct" C API
#include "WrappUnit.h"

namespace circuit {

using namespace springai;

COOAICallback::COOAICallback(OOAICallback* clb)
		: sAICallback(nullptr)
		, callback(clb)
		, skirmishAIId(callback->GetSkirmishAIId())
{
	unitIds.resize(MAX_UNITS);
	units.resize(MAX_UNITS);
}

COOAICallback::~COOAICallback()
{
}

void COOAICallback::Init(const struct SSkirmishAICallback* clb)
{
	sAICallback = clb;
}

int COOAICallback::GetEnemyTeamSize() const
{
	return sAICallback->getEnemyTeams(skirmishAIId, nullptr, -1) - 1;  // -1 for Gaia team :(
}

const std::vector<Unit*>& COOAICallback::GetFriendlyUnits()
{
	unitIds.resize(MAX_UNITS);
	int size = sAICallback->getFriendlyUnits(skirmishAIId, unitIds.data(), MAX_UNITS);

	units.resize(size);
	for (int i = 0; i < size; ++i) {
		units[i] = WrappUnit::GetInstance(skirmishAIId, unitIds[i]);
	}

	return units;
}

const std::vector<Unit*>& COOAICallback::GetFriendlyUnitsIn(const AIFloat3& pos, float radius, bool spherical)
{
	float pos_posF3[3];
	pos.LoadInto(pos_posF3);
	unitIds.resize(MAX_UNITS);
	int size = sAICallback->getFriendlyUnitsIn(skirmishAIId, pos_posF3, radius, spherical, unitIds.data(), MAX_UNITS);

	units.resize(size);
	for (int i = 0; i < size; ++i) {
		units[i] = WrappUnit::GetInstance(skirmishAIId, unitIds[i]);
	}

	return units;
}

bool COOAICallback::IsFriendlyUnitsIn(const AIFloat3& pos, float radius, bool spherical) const
{
	float pos_posF3[3];
	pos.LoadInto(pos_posF3);
	int size = sAICallback->getFriendlyUnitsIn(skirmishAIId, pos_posF3, radius, spherical, nullptr, -1);
	return size > 0;
}

const std::vector<int>& COOAICallback::GetFriendlyUnitIdsIn(const springai::AIFloat3& pos, float radius, bool spherical)
{
	float pos_posF3[3];
	pos.LoadInto(pos_posF3);
	unitIds.resize(MAX_UNITS);
	int size = sAICallback->getFriendlyUnitsIn(skirmishAIId, pos_posF3, radius, spherical, unitIds.data(), MAX_UNITS);
	unitIds.resize(size);
	return unitIds;
}

const std::vector<Unit*>& COOAICallback::GetEnemyUnits()
{
	unitIds.resize(MAX_UNITS);
	int size = sAICallback->getEnemyUnits(skirmishAIId, unitIds.data(), MAX_UNITS);

	units.resize(size);
	for (int i = 0; i < size; ++i) {
		units[i] = WrappUnit::GetInstance(skirmishAIId, unitIds[i]);
	}

	return units;
}

const std::vector<Unit*>& COOAICallback::GetEnemyUnitsIn(const AIFloat3& pos, float radius, bool spherical)
{
	float pos_posF3[3];
	pos.LoadInto(pos_posF3);
	unitIds.resize(MAX_UNITS);
	int size = sAICallback->getEnemyUnitsIn(skirmishAIId, pos_posF3, radius, spherical, unitIds.data(), MAX_UNITS);

	units.resize(size);
	for (int i = 0; i < size; ++i) {
		units[i] = WrappUnit::GetInstance(skirmishAIId, unitIds[i]);
	}

	return units;
}

const std::vector<int>& COOAICallback::GetEnemyUnitIdsIn(const AIFloat3& pos, float radius, bool spherical)
{
	float pos_posF3[3];
	pos.LoadInto(pos_posF3);
	unitIds.resize(MAX_UNITS);
	int size = sAICallback->getEnemyUnitsIn(skirmishAIId, pos_posF3, radius, spherical, unitIds.data(), MAX_UNITS);
	unitIds.resize(size);
	return unitIds;
}

const std::vector<Unit*>& COOAICallback::GetNeutralUnits()
{
	unitIds.resize(MAX_UNITS);
	int size = sAICallback->getNeutralUnits(skirmishAIId, unitIds.data(), MAX_UNITS);

	units.resize(size);
	for (int i = 0; i < size; ++i) {
		units[i] = WrappUnit::GetInstance(skirmishAIId, unitIds[i]);
	}

	return units;
}

const std::vector<Unit*>& COOAICallback::GetNeutralUnitsIn(const AIFloat3& pos, float radius, bool spherical)
{
	float pos_posF3[3];
	pos.LoadInto(pos_posF3);
	unitIds.resize(MAX_UNITS);
	int size = sAICallback->getNeutralUnitsIn(skirmishAIId, pos_posF3, radius, spherical, unitIds.data(), MAX_UNITS);

	units.resize(size);
	for (int i = 0; i < size; ++i) {
		units[i] = WrappUnit::GetInstance(skirmishAIId, unitIds[i]);
	}

	return units;
}

bool COOAICallback::IsNeutralUnitsIn(const AIFloat3& pos, float radius, bool spherical) const
{
	float pos_posF3[3];
	pos.LoadInto(pos_posF3);
	int size = sAICallback->getNeutralUnitsIn(skirmishAIId, pos_posF3, radius, spherical, nullptr, -1);
	return size > 0;
}

bool COOAICallback::IsFeatures() const
{
	int size = sAICallback->getFeatures(skirmishAIId, nullptr, -1);
	return size > 0;
}

bool COOAICallback::IsFeaturesIn(const AIFloat3& pos, float radius, bool spherical) const
{
	float pos_posF3[3];
	pos.LoadInto(pos_posF3);
	int size = sAICallback->getFeaturesIn(skirmishAIId, pos_posF3, radius, spherical, nullptr, -1);
	return size > 0;
}

int COOAICallback::Unit_GetDefId(int unitId) const
{
	return sAICallback->Unit_getDef(skirmishAIId, unitId);
}

bool COOAICallback::Unit_HasCommands(int unitId) const
{
	return sAICallback->Unit_getCurrentCommands(skirmishAIId, unitId) > 0;
}

bool COOAICallback::Feature_IsResurrectable(int featureId) const
{
	return sAICallback->Feature_getResurrectDef(skirmishAIId, featureId) != -1;
}

std::vector<int> COOAICallback::GetFeatureIds() const
{
	// Count first, then fill: the cheating path of the engine call doesn't respect a smaller buffer
	std::vector<int> featureIds(sAICallback->getFeatures(skirmishAIId, nullptr, -1));
	if (!featureIds.empty()) {
		featureIds.resize(sAICallback->getFeatures(skirmishAIId, featureIds.data(), featureIds.size()));
	}
	return featureIds;
}

std::vector<int> COOAICallback::GetFeatureIdsIn(const AIFloat3& pos, float radius, bool spherical) const
{
	float pos_posF3[3];
	pos.LoadInto(pos_posF3);
	std::vector<int> featureIds(sAICallback->getFeaturesIn(skirmishAIId, pos_posF3, radius, spherical, nullptr, -1));
	if (!featureIds.empty()) {
		featureIds.resize(sAICallback->getFeaturesIn(skirmishAIId, pos_posF3, radius, spherical, featureIds.data(), featureIds.size()));
	}
	return featureIds;
}

int COOAICallback::Feature_GetDefId(int featureId) const
{
	return sAICallback->Feature_getDef(skirmishAIId, featureId);
}

float COOAICallback::Feature_GetReclaimLeft(int featureId) const
{
	return sAICallback->Feature_getReclaimLeft(skirmishAIId, featureId);
}

AIFloat3 COOAICallback::Feature_GetPosition(int featureId) const
{
	float pos_posF3[3];
	sAICallback->Feature_getPosition(skirmishAIId, featureId, pos_posF3);
	return AIFloat3(pos_posF3[0], pos_posF3[1], pos_posF3[2]);
}

bool COOAICallback::FeatureDef_IsReclaimable(int featureDefId) const
{
	return sAICallback->FeatureDef_isReclaimable(skirmishAIId, featureDefId);
}

float COOAICallback::FeatureDef_GetContainedResource(int featureDefId, int resourceId) const
{
	return sAICallback->FeatureDef_getContainedResource(skirmishAIId, featureDefId, resourceId);
}

bool COOAICallback::UnitDef_HasYardMap(int unitDefId) const
{
	return sAICallback->UnitDef_getYardMap(skirmishAIId, unitDefId, UNIT_FACING_SOUTH, nullptr, -1) > 0;
}

} // namespace circuit

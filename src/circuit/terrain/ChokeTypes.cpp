/*
 * ChokeTypes.cpp
 *
 *  Created on: Nov 11, 2026
 *      Author: Claude
 */

// CChokeMap: the enemy's movement types (the type set), their army shares, and where their traffic starts

#include "terrain/ChokeMap.h"
#include "setup/SetupData.h"
#include "setup/SetupManager.h"
#include "terrain/TerrainManager.h"
#include "unit/enemy/EnemyManager.h"
#include "unit/CircuitDef.h"
#include "CircuitAI.h"
#include "util/GameAttribute.h"
#include "util/Point.h"
#include "util/Utils.h"

#include "spring/SpringCallback.h"
#include "spring/SpringMap.h"

#include "Log.h"

#include "Unit.h"
#include "UnitDef.h"

#include <algorithm>
#include <sstream>

namespace circuit {

using namespace springai;

#define LEAVE_FRAMES	(FRAMES_PER_SEC * 300)  // a type leaves 5 minutes after it, or a factory for it, was last seen
#define COST_HALF_LIFE	(FRAMES_PER_SEC * 180.f)  // an unseen enemy's cost fades by half every 3 minutes
#define SHARE_PRIOR		300.f  // metal: shares start equal, and early sightings don't swing them
#define GUESS_HALF		(SQUARE_SIZE * 32.f)  // half the side of a guessed box around a start position

CChokeMap::MoveKey CChokeMap::KeyOf(const CCircuitDef* cdef)
{
	const bool isJumper = cdef->IsAbleToJump() && (cdef->GetJumpRange() > 0.f);
	return cdef->GetMobileId() * 2 + (isJumper ? 1 : 0);
}

int CChokeMap::ClassOf(const CCircuitDef* cdef) const
{
	// Every Zero-K commander is amphibious: counting them would open every water lane from the first frame
	if ((cdef == nullptr) || !cdef->IsMobile() || cdef->IsAbleToFly() || (cdef->GetMobileId() < 0)
		|| cdef->IsRoleComm() || cdef->IsEnemyRoleAny(CCircuitDef::GetMask(ROLE_TYPE(COMM))))
	{
		return -1;
	}
	const MoveKey key = KeyOf(cdef);
	auto it = std::lower_bound(moveClasses.begin(), moveClasses.end(), key, [](const SMoveClass& mc, MoveKey k) {
		return mc.key < k;
	});
	return ((it != moveClasses.end()) && (it->key == key)) ? int(it - moveClasses.begin()) : -1;
}

/*
 * The opening default: walkers and tanks gate, and hovers too when the map has water. Amphibious bots are in the set
 * as a leak type. Spiders and jumpers join once seen
 */
void CChokeMap::InitTypeSet()
{
	typeStates.assign(moveClasses.size(), STypeState());
	const bool isWet = circuit->GetTerrainManager()->GetPercentLand() < 90.f;
	for (size_t c = 0; c < moveClasses.size(); ++c) {
		const MoveKind kind = moveClasses[c].kind;
		STypeState& ts = typeStates[c];
		ts.isInSet = (kind == MoveKind::GROUND) || (kind == MoveKind::AMPHIBIOUS) || ((kind == MoveKind::HOVER) && isWet);
		ts.isDefault = ts.isInSet;
	}
	UpdateShares();
}

/*
 * Map sides and traffic sources from the start boxes: ours, and a source group for every other box. A box covering
 * over half the map counts as none. With no other boxes, one guessed group: squares at the 3 mirrors of our start
 * (point, left to right, top to bottom). Called again until our start is known, while no enemy factory is
 */
void CChokeMap::InitSources()
{
	CMap* map = circuit->GetMap();
	const float mapW = map->GetWidth() * SQUARE_SIZE;
	const float mapH = map->GetHeight() * SQUARE_SIZE;
	auto toRect = [](const CAllyTeam::SBox& b) {
		return SRect{b.left, std::min(b.top, b.bottom), b.right, std::max(b.top, b.bottom)};
	};
	auto isValid = [mapW, mapH](const SRect& r) {
		const float area = (r.x1 - r.x0) * (r.z1 - r.z0);
		return (area > SQUARE(SQUARE_SIZE * 8)) && (area <= 0.5f * mapW * mapH);
	};
	auto square = [](const AIFloat3& p) {
		return SRect{p.x - GUESS_HALF, p.z - GUESS_HALF, p.x + GUESS_HALF, p.z + GUESS_HALF};
	};
	const AIFloat3& start = circuit->GetSetupManager()->GetBasePos();
	isStartKnown = utils::is_valid(start);

	ourBoxes.clear();
	const CAllyTeam::SBox& ourBox = circuit->GetAllyTeam()->GetStartBox();
	const SRect ours = toRect(ourBox);
	if (isValid(ours)) {
		ourBoxes.push_back(ours);
	} else if (isStartKnown) {
		ourBoxes.push_back(square(start));
	}

	sourceGroups.clear();
	for (const auto& kv : circuit->GetGameAttribute()->GetSetupData().GetBoxes()) {
		const CAllyTeam::SBox& box = kv.second;
		const bool isOurs = (box.left == ourBox.left) && (box.right == ourBox.right) && (box.top == ourBox.top)
				&& (box.bottom == ourBox.bottom);
		if (!isOurs && isValid(toRect(box))) {
			SSourceGroup group;
			group.boxes.push_back(toRect(box));
			sourceGroups.push_back(group);
		}
	}
	if (sourceGroups.empty() && isStartKnown) {
		SSourceGroup group;
		group.isGuess = true;
		group.boxes.push_back(square(AIFloat3(mapW - start.x, 0.f, mapH - start.z)));
		group.boxes.push_back(square(AIFloat3(mapW - start.x, 0.f, start.z)));
		group.boxes.push_back(square(AIFloat3(start.x, 0.f, mapH - start.z)));
		sourceGroups.push_back(group);
	}

	// Enemy ally teams in the start script: when each has a factory located, boxes with none are unused
	const CSetupData& setupData = circuit->GetGameAttribute()->GetSetupData();
	enemyAllyCount = 0;
	for (int i = 0; i < setupData.GetAllyTeamCount(); ++i) {
		if ((i != circuit->GetAllyTeamId()) && (setupData.GetAllyTeam(i)->GetSize() > 0)) {
			++enemyAllyCount;
		}
	}
	enemyAllyCount = std::max(enemyAllyCount, 1);
}

/*
 * Every 5 s: enemy factories (and the types they build), sightings of each type, its faded army cost, enemy aircraft,
 * ships and artillery near chokes
 */
void CChokeMap::ScanEnemies()
{
	const int frame = circuit->GetLastFrame();
	CEnemyManager* enemyMgr = circuit->GetEnemyManager();
	const CEnemyManager::EnemyUnits& enemies = enemyMgr->GetEnemyUnits();

	// Factories seen dead, or forgotten because their spot was seen empty. A live one keeps its types fresh
	for (auto it = factories.begin(); it != factories.end();) {
		auto ie = enemies.find(it->first);
		if ((ie == enemies.end()) || ie->second->IsDead()) {
			for (int c : it->second.classes) {
				typeStates[c].factories.erase(it->first);
			}
			it = factories.erase(it);
		} else {
			for (int c : it->second.classes) {
				typeStates[c].lastSeen = frame;
			}
			++it;
		}
	}

	for (STypeState& ts : typeStates) {
		ts.cost = 0.f;
	}
	const std::vector<CRegionGraph::SChoke>& chokes = graph.GetChokes();
	const SCellMask& mask = graph.GetMask();
	for (const auto& kv : enemies) {
		const CEnemyUnit* e = kv.second;
		const CCircuitDef* cdef = e->GetCircuitDef();
		if ((cdef == nullptr) || e->IsDead()) {
			continue;  // type unknown: never seen in LOS
		}
		if (cdef->IsMobile()) {
			const int c = ClassOf(cdef);
			if (c >= 0) {
				typeStates[c].cost += e->GetCost() * enemyMgr->GetArmyCostWeight(e, frame, COST_HALF_LIFE);
				if (e->IsInLOS()) {
					if (moveClasses[c].role != MoveRole::NAVAL) {
						typeStates[c].lastSeen = frame;
					} else {
						const int cell = mask.IndexAt(e->GetPos().x, e->GetPos().z);
						auto is = std::lower_bound(shipCells.begin(), shipCells.end(), cell);
						if ((is == shipCells.end()) || (*is != cell)) {
							shipCells.insert(is, cell);
							nextAnalysisFrame = frame;  // exposure
						}
					}
				}
			} else if (!isAirSeen && cdef->IsAbleToFly() && e->IsInLOS()
				&& (cdef->IsAttacker() || (cdef->GetDef()->GetTransportCapacity() > 0)))  // no scouts
			{
				isAirSeen = true;
				nextAnalysisFrame = frame;
				circuit->LOG("CHOKE enemy aircraft seen (%s): every cluster is leaky", cdef->GetDef()->GetName());
			}
		} else if (e->IsInLOS() && (factories.find(kv.first) == factories.end())
			&& std::any_of(cdef->GetBuildOptions().begin(), cdef->GetBuildOptions().end(), [this](CCircuitDef::Id id) {
				const CCircuitDef* option = circuit->GetCircuitDef(id);
				return (option != nullptr) && option->IsMobile();
			}))
		{
			// A factory (air ones too: a base is a base)
			SFactory factory;
			for (CCircuitDef::Id id : cdef->GetBuildOptions()) {
				const int c = ClassOf(circuit->GetCircuitDef(id));
				if ((c >= 0) && (std::find(factory.classes.begin(), factory.classes.end(), c) == factory.classes.end())) {
					factory.classes.push_back(c);
					typeStates[c].factories.insert(kv.first);
					typeStates[c].lastSeen = frame;
				}
			}
			factory.team = e->GetUnit()->GetTeam();
			factories[kv.first] = factory;
			factoryTeams.insert(factory.team);
			AddFactory(e->GetPos(), e->GetUnit()->GetAllyTeam());
		}
		// Artillery in range of a choke's defences: no new towers there for a while
		if (e->IsInRadarOrLOS() && cdef->IsEnemyRoleAny(CCircuitDef::GetMask(ROLE_TYPE(ARTY)))) {
			const float range = cdef->GetMaxRange() + config.setback[1] * mainRange;
			for (const CRegionGraph::SChoke& choke : chokes) {
				const AIFloat3 centre(mask.CentreX(choke.centre), 0.f, mask.CentreZ(choke.centre));
				if (centre.SqDistance2D(e->GetPos()) < SQUARE(range)) {
					artySeen[choke.id] = frame;
				}
			}
		}
	}
}

/*
 * An enemy factory joins its ally team's source group, else the box group it stands in, else a new group. A group
 * switches from its box to factories once, and after that its sources only add
 */
void CChokeMap::AddFactory(const AIFloat3& pos, int allyTeam)
{
	auto isOwner = [allyTeam](const SSourceGroup& g) { return g.allyTeam == allyTeam; };
	auto isBoxOf = [&pos](const SSourceGroup& g) {
		return (g.allyTeam < 0) && !g.isGuess && std::any_of(g.boxes.begin(), g.boxes.end(), [&pos](const SRect& r) {
			return r.Contains(pos);
		});
	};
	auto it = std::find_if(sourceGroups.begin(), sourceGroups.end(), isOwner);
	if (it == sourceGroups.end()) {
		it = std::find_if(sourceGroups.begin(), sourceGroups.end(), isBoxOf);
	}
	if (it == sourceGroups.end()) {
		// The mirror guess goes with the first real factory
		sourceGroups.erase(std::remove_if(sourceGroups.begin(), sourceGroups.end(), [](const SSourceGroup& g) {
			return g.isGuess;
		}), sourceGroups.end());
		sourceGroups.push_back(SSourceGroup());
		it = sourceGroups.end() - 1;
	}
	it->allyTeam = allyTeam;
	it->factoryPos.push_back(pos);

	// Once every enemy ally team has a factory located, the boxes nobody came from are unused
	const int located = std::count_if(sourceGroups.begin(), sourceGroups.end(), [](const SSourceGroup& g) {
		return !g.factoryPos.empty();
	});
	if (located >= enemyAllyCount) {
		sourceGroups.erase(std::remove_if(sourceGroups.begin(), sourceGroups.end(), [](const SSourceGroup& g) {
			return g.factoryPos.empty();
		}), sourceGroups.end());
	}
	isSourceDirty = true;
	isOpennessDirty = true;
	UpdateSides();
	nextAnalysisFrame = circuit->GetLastFrame();
}

/*
 * The type set rules:
 * - Opening: the default holds until a factory was seen for every enemy team or 5 minutes pass, and something was
 *   seen. Then the default types never seen leave.
 * - Joining: a factory that builds the type, or one of its units (no commanders), was seen.
 * - Leaving: every known factory for it was seen dead, and nothing of it was seen for 5 minutes.
 * - Promotion: a leak type whose own cost reached choke.gate_share of the set's cost plus the prior gates until it
 *   leaves. The prior counts for nobody here, so a lone Flea early on doesn't change gating.
 * Ships never join: they don't walk through chokes
 */
CChokeMap::STypeChange CChokeMap::UpdateTypeSet()
{
	const int frame = circuit->GetLastFrame();
	const size_t n = moveClasses.size();
	std::vector<bool> wasIn(n), wasGating(n);
	for (size_t c = 0; c < n; ++c) {
		wasIn[c] = typeStates[c].isInSet;
		wasGating[c] = IsGating(c);
	}

	for (STypeState& ts : typeStates) {
		ts.isDefault &= (ts.lastSeen < 0);
	}
	if (isOpening) {
		const bool isAnySeen = std::any_of(typeStates.begin(), typeStates.end(), [](const STypeState& ts) {
			return ts.lastSeen >= 0;
		});
		const bool isScouted = (int)factoryTeams.size() >= circuit->GetCallback()->GetEnemyTeamSize();
		if (isAnySeen && (isScouted || (frame >= openingEnd))) {
			isOpening = false;
			for (STypeState& ts : typeStates) {
				if (ts.isDefault) {
					ts.isInSet = false;
					ts.isDefault = false;
				}
			}
		}
	}
	for (size_t c = 0; c < n; ++c) {
		STypeState& ts = typeStates[c];
		const bool isRecent = (ts.lastSeen >= 0) && (frame < ts.lastSeen + LEAVE_FRAMES);
		if (!ts.isInSet && isRecent && (moveClasses[c].role != MoveRole::NAVAL)) {
			ts.isInSet = true;
		} else if (ts.isInSet && !ts.isDefault && !isRecent && ts.factories.empty()) {
			ts.isInSet = false;
			ts.isPromoted = false;
		}
	}
	const float total = UpdateShares();
	for (size_t c = 0; c < n; ++c) {
		STypeState& ts = typeStates[c];
		if (ts.isInSet && (moveClasses[c].role == MoveRole::LEAK) && (ts.cost >= config.gateShare * (total + SHARE_PRIOR))) {
			ts.isPromoted = true;
		}
	}

	STypeChange change;
	for (size_t c = 0; c < n; ++c) {
		if (typeStates[c].isInSet && !wasIn[c]) {
			change.joined.push_back(c);
		} else if (!typeStates[c].isInSet && wasIn[c]) {
			change.left.push_back(c);
		}
		change.isGating |= (IsGating(c) != wasGating[c]);
	}
	change.isLeak = !change.isGating && (!change.joined.empty() || !change.left.empty());
	if (change.isGating || change.isLeak) {
		std::ostringstream text;
		text << "CHOKE type set:";
		for (size_t c = 0; c < n; ++c) {
			if (typeStates[c].isInSet) {
				text << " " << moveClasses[c].name << (IsGating(c) ? "(gating " : "(leak ")
						<< utils::float_to_string(typeStates[c].share, "%.2f") << ")";
			}
		}
		circuit->LOG("%s", text.str().c_str());
	}
	return change;
}

/*
 * A type's share is (its faded cost + prior / n) / (total faded cost + prior), for n types in the set. Returns the total
 */
float CChokeMap::UpdateShares()
{
	int count = 0;
	float total = 0.f;
	for (const STypeState& ts : typeStates) {
		if (ts.isInSet) {
			++count;
			total += ts.cost;
		}
	}
	for (STypeState& ts : typeStates) {
		ts.share = ts.isInSet ? (ts.cost + SHARE_PRIOR / count) / (total + SHARE_PRIOR) : 0.f;
	}
	return total;
}

void CChokeMap::Reblend()
{
	std::vector<std::pair<int, float>> shares;
	for (size_t c = 0; c < moveClasses.size(); ++c) {
		STypeState& ts = typeStates[c];
		ts.blendShare = ts.isInSet ? ts.share : -1.f;
		if (ts.isInSet) {
			shares.push_back({moveClasses[c].key, ts.share});
		}
	}
	traffic.Blend(shares);
	blendFrame = circuit->GetLastFrame();
	++version;
}

/*
 * Per source group, its traffic cells: its factories once one is seen, else every cell of its boxes
 */
std::vector<CTrafficField::Cells> CChokeMap::GetSourceCells() const
{
	std::vector<CTrafficField::Cells> sources;
	const float step = traffic.GetCellSize();
	for (const SSourceGroup& g : sourceGroups) {
		CTrafficField::Cells cells;
		if (!g.factoryPos.empty()) {
			for (const AIFloat3& pos : g.factoryPos) {
				cells.push_back(traffic.IndexAt(pos.x, pos.z));
			}
		} else {
			for (const SRect& r : g.boxes) {
				for (float z = r.z0 + step * 0.5f; z < r.z1; z += step) {
					for (float x = r.x0 + step * 0.5f; x < r.x1; x += step) {
						cells.push_back(traffic.IndexAt(x, z));
					}
				}
			}
		}
		std::sort(cells.begin(), cells.end());
		cells.erase(std::unique(cells.begin(), cells.end()), cells.end());
		if (!cells.empty()) {
			sources.push_back(std::move(cells));
		}
	}
	return sources;
}

} // namespace circuit

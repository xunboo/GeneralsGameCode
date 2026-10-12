/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2025 Electronic Arts Inc.
** SPDX-License-Identifier: GPL-3.0-or-later
*/

#include "PreRTS.h"
#include "GameLogic/CompetitiveSkirmishAI.h"
#include "Common/ActionManager.h"
#include "Common/BuildAssistant.h"
#include "Common/Energy.h"
#include "Common/GlobalData.h"
#include "Common/Money.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/Science.h"
#include "Common/SpecialPower.h"
#include "Common/ThingFactory.h"
#include "Common/ThingTemplate.h"
#include "Common/Upgrade.h"
#include "Common/Xfer.h"
#include "GameClient/ControlBar.h"
#include "GameClient/TerrainVisual.h"
#include "GameLogic/AI.h"
#include "GameLogic/AIPathfind.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Object.h"
#include "GameLogic/PartitionManager.h"
#include "GameLogic/ScriptEngine.h"
#include "GameLogic/TerrainLogic.h"
#include "GameLogic/Weapon.h"
#include "GameLogic/Module/AIUpdate.h"
#include "GameLogic/Module/BodyModule.h"
#include "GameLogic/Module/ContainModule.h"
#include "GameLogic/Module/DozerAIUpdate.h"
#include "GameLogic/Module/ProductionUpdate.h"
#include "GameLogic/Module/SupplyTruckAIUpdate.h"
#include "GameLogic/Module/SupplyWarehouseDockUpdate.h"
#include "GameLogic/Module/SpecialPowerModule.h"
#include "GameLogic/Module/JetAIUpdate.h"
#include "GameLogic/Module/RebuildHoleBehavior.h"

namespace
{
	Real distanceSquared(const Coord3D& a, const Coord3D& b)
	{
		return sqr(a.x - b.x) + sqr(a.y - b.y);
	}
	Bool named(const Object *object, const char *name)
	{
		return object->getTemplate()->getName() == name;
	}
	Bool contains(const AsciiString& name, const char *part)
	{
		AsciiString lower = name;
		lower.toLower();
		return strstr(lower.str(), part) != nullptr;
	}
	Bool operational(const Object *object)
	{
		return !object->isEffectivelyDead() && !object->isDisabled()
			&& !object->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION) && !object->testStatus(OBJECT_STATUS_SOLD);
	}
	const char *passengerPrior(const AsciiString& name, Int& expected)
	{
		expected = 0;
		if (contains(name, "humvee")) { expected = 3; return "AmericaInfantryMissileDefender"; }
		if (contains(name, "battlebus")) { expected = 6; return "GLAInfantryTunnelDefender"; }
		if (contains(name, "listeningoutpost")) { expected = 2; return "ChinaInfantryTankHunter"; }
		if (contains(name, "helix")) { expected = 5; return "ChinaInfantryTankHunter"; }
		if (contains(name, "chinabunker")) { expected = 3; return "ChinaInfantryTankHunter"; }
		if (contains(name, "glapalace")) { expected = 4; return "GLAInfantryTunnelDefender"; }
		return nullptr;
	}
	Bool objectLess(const Object *a, const Object *b) { return a->getID() < b->getID(); }
	struct ObjectIDLess
	{
		Bool operator()(const Object *object, ObjectID id) const { return object->getID() < id; }
	};
}

CompetitiveAIObservation::CompetitiveAIObservation() :
	id(INVALID_ID), lastSeen(0), health(0), playerIndex(0), structure(false), aircraft(false),
	infantry(false), vehicle(false), economy(false), defense(false), superweapon(false),
	armed(false), antiAir(false), detector(false), publicReadyFrame(0)
{
	position.zero();
}

void CompetitiveAIObservation::observe(const CompetitiveAIObservation& observation, Bool visible)
{
	if (visible) *this = observation;
}

Real CompetitiveAIObservation::confidence(UnsignedInt frame) const
{
	UnsignedInt age = frame >= lastSeen ? frame - lastSeen : 0;
	if (structure || age <= 15 * LOGICFRAMES_PER_SECOND) return 1.0f;
	if (age >= 90 * LOGICFRAMES_PER_SECOND) return 0.0f;
	if (age >= 45 * LOGICFRAMES_PER_SECOND)
		return 0.25f * Real(90 * LOGICFRAMES_PER_SECOND - age) / (45 * LOGICFRAMES_PER_SECOND);
	return 1.0f - 0.75f * Real(age - 15 * LOGICFRAMES_PER_SECOND) / (30 * LOGICFRAMES_PER_SECOND);
}

Bool CompetitiveAIObservation::hasPrecisePosition(UnsignedInt frame) const
{
	return structure || frame - lastSeen <= 15 * LOGICFRAMES_PER_SECOND;
}

Int CompetitiveAIPolicy::reserve(Int counterCost) { return max(1000, 2 * max(0, counterCost)); }
Bool CompetitiveAIPolicy::attack(Real ours, Real theirs, Bool scouted, Bool escape)
{
	return scouted && escape && ours > 0 && ours >= 1.25f * theirs;
}
Bool CompetitiveAIPolicy::retreat(Real ours, Real theirs) { return theirs > 0 && ours < 0.85f * theirs; }
Bool CompetitiveAIPolicy::changeState(UnsignedInt frame, UnsignedInt changed, Bool emergency)
{
	return emergency || frame - changed >= 6 * LOGICFRAMES_PER_SECOND;
}

struct CompetitiveSkirmishAI::Profile
{
	const char *builder, *power, *supply, *barracks, *factory, *tech, *secondary, *superweapon, *commandCenter;
	const char *collector, *basic, *antiAir, *infantry, *siege, *hero;
	Int collectors;
};

const CompetitiveSkirmishAI::Profile& CompetitiveSkirmishAI::profile() const
{
	// Template identifiers come from the installed game; costs and prerequisites are queried at runtime.
	static const Profile usa = { "AmericaVehicleDozer", "AmericaPowerPlant", "AmericaSupplyCenter",
		"AmericaBarracks", "AmericaWarFactory", "AmericaStrategyCenter", "AmericaSupplyDropZone",
		"AmericaParticleCannonUplink", "AmericaCommandCenter", "AmericaVehicleChinook", "AmericaVehicleHumvee",
		"AmericaTankAvenger", "AmericaInfantryMissileDefender", "AmericaVehicleTomahawk",
		"AmericaInfantryColonelBurton", 2 };
	static const Profile china = { "ChinaVehicleDozer", "ChinaPowerPlant", "ChinaSupplyCenter",
		"ChinaBarracks", "ChinaWarFactory", "ChinaPropagandaCenter", "ChinaInternetCenter",
		"ChinaNuclearMissileLauncher", "ChinaCommandCenter", "ChinaVehicleSupplyTruck", "ChinaTankBattleMaster",
		"ChinaTankGattling", "ChinaInfantryTankHunter", "ChinaVehicleInfernoCannon", "ChinaInfantryBlackLotus", 2 };
	static const Profile gla = { "GLAInfantryWorker", nullptr, "GLASupplyStash", "GLABarracks",
		"GLAArmsDealer", "GLAPalace", "GLABlackMarket", "GLAScudStorm", "GLACommandCenter", "GLAInfantryWorker",
		"GLATankScorpion", "GLAVehicleQuadCannon", "GLAInfantryTunnelDefender",
		"GLAVehicleRocketBuggy", "GLAInfantryJarmenKell", 6 };
	if (m_player->getSide() == "America") return usa;
	if (m_player->getSide() == "China") return china;
	return gla;
}

Bool CompetitiveSkirmishAI::supportsSide(const AsciiString& side)
{
	return side == "America" || side == "China" || side == "GLA";
}

Bool CompetitiveSkirmishAI::canObserve(const Player *viewer, const Object *object)
{
	if (!viewer || !object) return false;
	if (object->getControllingPlayer() == viewer) return true;
	ObjectShroudStatus shroud = object->getShroudedStatus(viewer->getPlayerIndex());
	if (shroud != OBJECTSHROUD_CLEAR && shroud != OBJECTSHROUD_PARTIAL_CLEAR) return false;
	if (object->getContainedBy()) return false;
	// A disguise must not expose the real template, or an invisible passenger its weapon.
	return !(object->testStatus(OBJECT_STATUS_STEALTHED) && !object->testStatus(OBJECT_STATUS_DETECTED))
		&& !(object->testStatus(OBJECT_STATUS_DISGUISED) && !object->testStatus(OBJECT_STATUS_DETECTED));
}

CompetitiveSkirmishAI::UnitOrder::UnitOrder() :
	role(MAIN), order(NONE), target(INVALID_ID), issued(0), progressFrame(0),
	retreatUntil(0), lastHealth(0), damagePerSecond(0), supplyBoxes(0), delivered(0), cycleFrames(0), waitingFrames(0),
	raidTarget(INVALID_ID), raidCost(0), raidGain(0), raidHealth(0), raidMaxHealth(0), raidValue(0)
{
	goal.zero(); progressPosition.zero(); raidGoal.zero();
}
CompetitiveSkirmishAI::Threat::Threat() :
	air(0), infantry(0), vehicles(0), defenses(0), antiAir(0), baseAttacked(false), explored(false) {}
CompetitiveSkirmishAI::CompetitiveSkirmishAI(Player *player) : m_player(player) { reset(); }

void CompetitiveSkirmishAI::reset()
{
	m_base.zero(); m_goal.zero(); m_baseSet = false; m_hasGoal = false;
	m_state = COMPETITIVE_OPENING; m_changed = 0;
	m_nextEconomy = m_nextStrategy = m_nextTactics = m_nextAbilities = 0;
	m_lastProgress = m_nextScout = m_nextTunnelExit = 0;
	m_battlePlanCenter = INVALID_ID; m_scoutIndex = 0; m_raidFailures = 0;
	m_raidFailedFrame = 0; m_raidFailedGoal.zero(); m_goalHealth = 0; m_threat = Threat();
	m_raidTarget = INVALID_ID; m_raidPayload = 0;
	m_collectorQuota = 0; m_nextCollectorAdjustment = 0;
	m_observations.clear(); m_orders.clear(); m_owned.clear(); m_visible.clear(); m_supplyMarkers.clear();
	m_raidRoutes.clear();
}

void CompetitiveSkirmishAI::observePublicSuperweapon(const Object *object, UnsignedInt readyFrame)
{
	if (!object || m_player->getRelationship(object->getTeam()) != ENEMIES
		|| (m_observations.size() >= 512 && m_observations.find(object->getID()) == m_observations.end())) return;
	// Published timers expose identity, owner, position and countdown. They never expose hidden health or orders.
	CompetitiveAIObservation& seen = m_observations[object->getID()];
	seen.id = object->getID(); seen.templateName = object->getTemplate()->getName();
	seen.position = *object->getPosition(); seen.playerIndex = object->getControllingPlayer()->getPlayerIndex();
	seen.structure = true; seen.superweapon = true; seen.publicReadyFrame = readyFrame;
}

void CompetitiveSkirmishAI::observe()
{
	m_owned.clear(); m_visible.clear(); m_threat = Threat();
	UnsignedInt frame = TheGameLogic->getFrame();
	for (Object *object = TheGameLogic->getFirstObject(); object; object = object->getNextObject())
	{
		if (object->getControllingPlayer() == m_player)
		{
			m_observations.erase(object->getID());
			if (!object->isEffectivelyDead() && !object->testStatus(OBJECT_STATUS_SOLD)) m_owned.push_back(object);
			continue;
		}
		if (!canObserve(m_player, object)) continue;
		Relationship relation = m_player->getRelationship(object->getTeam());
		if (relation == ALLIES) { m_observations.erase(object->getID()); continue; }
		// Attribute an estimated economic benefit only to a raid's legally observed target.
		// Hidden damage or destruction cannot turn a failed route into a profitable one.
		for (OrderMap::iterator raid = m_orders.begin(); raid != m_orders.end(); ++raid)
		{
			UnitOrder& mission = raid->second;
			if (mission.raidTarget != object->getID() || mission.raidMaxHealth <= 0) continue;
			Real health = object->getBodyModule()->getHealth();
			mission.raidGain += max(0.0f, mission.raidHealth - health) * mission.raidValue / mission.raidMaxHealth;
			mission.raidHealth = health;
		}
		if (object->isEffectivelyDead())
		{
			m_observations.erase(object->getID());
			continue;
		}
		if (!object->isKindOf(KINDOF_STRUCTURE) && !object->isKindOf(KINDOF_VEHICLE)
			&& !object->isKindOf(KINDOF_INFANTRY) && !object->isKindOf(KINDOF_REBUILD_HOLE)
			&& !contains(object->getTemplate()->getName(), "salvage")) continue;
		m_visible.push_back(object);
		CompetitiveAIObservation seen;
		seen.id = object->getID(); seen.templateName = object->getTemplate()->getName();
		seen.position = *object->getPosition(); seen.lastSeen = frame;
		seen.playerIndex = object->getControllingPlayer()->getPlayerIndex();
		seen.health = object->getBodyModule()->getHealth();
		seen.structure = object->isKindOf(KINDOF_STRUCTURE) || object->isKindOf(KINDOF_REBUILD_HOLE);
		seen.aircraft = object->isKindOf(KINDOF_AIRCRAFT);
		seen.infantry = object->isKindOf(KINDOF_INFANTRY);
		seen.vehicle = object->isKindOf(KINDOF_VEHICLE);
		seen.economy = object->isKindOf(KINDOF_HARVESTER) || object->isKindOf(KINDOF_DOZER)
			|| object->isKindOf(KINDOF_FS_SUPPLY_CENTER) || object->isKindOf(KINDOF_CASH_GENERATOR);
		seen.defense = object->isKindOf(KINDOF_FS_BASE_DEFENSE);
		seen.superweapon = object->isKindOf(KINDOF_FS_SUPERWEAPON);
		ObservationMap::const_iterator previous = m_observations.find(seen.id);
		if (previous != m_observations.end()) seen.publicReadyFrame = previous->second.publicReadyFrame;
		// Threat classification uses public templates, not a visible vehicle's private weapon/upgrade state.
		Int expected = 0;
		const char *passenger = passengerPrior(seen.templateName, expected);
		const ThingTemplate *knownTypes[2] = { object->getTemplate(), passenger ? TheThingFactory->findTemplate(passenger) : nullptr };
		WeaponBonus bonus;
		for (Int type = 0; type < 2; ++type)
		{
			const WeaponTemplateSet *weapons = knownTypes[type] ? knownTypes[type]->findWeaponTemplateSet(WeaponSetFlags()) : nullptr;
			if (!weapons) continue;
			for (Int slot = 0; slot < WEAPONSLOT_COUNT; ++slot)
			{
				const WeaponTemplate *weapon = weapons->getNth(WeaponSlotType(slot));
				if (!weapon || weapon->getPrimaryDamage(bonus) <= 0) continue;
				seen.armed = true;
				if (weapon->getAntiMask() & WEAPON_ANTI_AIRBORNE_VEHICLE) seen.antiAir = true;
			}
		}
		seen.defense = seen.defense || (seen.structure && seen.armed && !seen.economy);
		seen.detector = object->findUpdateModule(NAMEKEY("StealthDetectorUpdate")) != nullptr;
		if (m_observations.size() < 512 || m_observations.find(seen.id) != m_observations.end())
			m_observations[seen.id].observe(seen, true);
	}
	std::sort(m_owned.begin(), m_owned.end(), objectLess);
	std::sort(m_visible.begin(), m_visible.end(), objectLess);
	if (!m_baseSet)
	{
		// Cache the map preview once. Hidden destruction/depletion must not change public resource locations.
		m_supplyMarkers.clear();
		for (Object *object = TheGameLogic->getFirstObject(); object; object = object->getNextObject())
			if (object->isKindOf(KINDOF_SUPPLY_SOURCE_ON_PREVIEW)) m_supplyMarkers.push_back(*object->getPosition());
		for (size_t i = 0; i < m_owned.size(); ++i)
		{
			if (m_owned[i]->isKindOf(KINDOF_COMMANDCENTER))
			{
				m_base = *m_owned[i]->getPosition(); m_baseSet = true; break;
			}
		}
	}
	for (ObservationMap::iterator it = m_observations.begin(); it != m_observations.end(); )
	{
		CompetitiveAIObservation& seen = it->second;
		Bool hostile = m_player->getRelationship(ThePlayerList->getNthPlayer(seen.playerIndex)->getDefaultTeam()) == ENEMIES;
		Real certainty = seen.confidence(frame);
		if (hostile && certainty > 0)
		{
			m_threat.explored = true;
			m_threat.air += seen.aircraft && seen.armed && !seen.economy;
			m_threat.infantry += seen.infantry && !seen.economy;
			m_threat.vehicles += seen.vehicle && !seen.aircraft && !seen.economy;
			m_threat.defenses += seen.defense;
			m_threat.antiAir += seen.antiAir;
			if (seen.lastSeen == frame && seen.armed && !seen.economy && distanceSquared(seen.position, m_base) < sqr(350.0f))
				m_threat.baseAttacked = true;
		}
		// Only seeing an empty remembered position can invalidate a building; hidden destruction cannot.
		if (seen.lastSeen != frame && seen.structure
			&& ThePartitionManager->getShroudStatusForPlayer(m_player->getPlayerIndex(), &seen.position) == CELLSHROUD_CLEAR)
		{
			Bool occupied = false;
			for (size_t i = 0; i < m_visible.size(); ++i)
				if (m_visible[i]->getID() == seen.id) occupied = true;
			if (!occupied) { ObservationMap::iterator old = it++; m_observations.erase(old); continue; }
		}
		if (!seen.structure && certainty == 0)
		{
			ObservationMap::iterator old = it++; m_observations.erase(old); continue;
		}
		++it;
	}
	for (OrderMap::iterator it = m_orders.begin(); it != m_orders.end(); )
	{
		if (!findOwned(it->first))
		{
			const UnitOrder& mission = it->second;
			if (mission.role == RAID && mission.raidCost > 0)
			{
				size_t route = 0;
				for (; route < m_raidRoutes.size(); ++route)
					if (distanceSquared(mission.raidGoal, m_raidRoutes[route].position) < sqr(300.0f)) break;
				if (route == m_raidRoutes.size() && route < 64)
				{
					RaidRoute recorded; recorded.position = mission.raidGoal; m_raidRoutes.push_back(recorded);
				}
				if (route < m_raidRoutes.size())
				{
					RaidRoute& recorded = m_raidRoutes[route];
					if (mission.raidGain < mission.raidCost) { ++recorded.failures; recorded.failedFrame = frame; }
					else recorded.failures = 0;
				}
			}
			OrderMap::iterator old = it++; m_orders.erase(old);
		}
		else ++it;
	}
	m_raidFailures = 0;
	for (size_t route = 0; route < m_raidRoutes.size(); ++route)
		if (m_raidRoutes[route].failures >= 2)
		{
			m_raidFailures = m_raidRoutes[route].failures;
			m_raidFailedGoal = m_raidRoutes[route].position; m_raidFailedFrame = m_raidRoutes[route].failedFrame;
			break;
		}
}

Object *CompetitiveSkirmishAI::findOwned(ObjectID id) const
{
	std::vector<Object*>::const_iterator it = std::lower_bound(m_owned.begin(), m_owned.end(), id, ObjectIDLess());
	return it != m_owned.end() && (*it)->getID() == id ? *it : nullptr;
}
Object *CompetitiveSkirmishAI::visibleTarget(ObjectID id) const
{
	std::vector<Object*>::const_iterator it = std::lower_bound(m_visible.begin(), m_visible.end(), id, ObjectIDLess());
	return it != m_visible.end() && (*it)->getID() == id ? *it : nullptr;
}
Int CompetitiveSkirmishAI::price(const char *name) const
{
	const ThingTemplate *thing = name ? TheThingFactory->findTemplate(name) : nullptr;
	return thing ? thing->calcCostToBuild(m_player) : 0;
}
Int CompetitiveSkirmishAI::counterCost() const
{
	if (m_player->getSide() == "America" && (m_threat.air || m_threat.vehicles)) return price(profile().infantry);
	if (m_threat.air || m_threat.infantry >= 4) return price(profile().antiAir);
	return price(profile().basic);
}
Int CompetitiveSkirmishAI::count(const char *name, Bool queued) const
{
	if (!name) return 0;
	const ThingTemplate *thing = TheThingFactory->findTemplate(name);
	if (!thing) return 0;
	Int result = 0;
	for (size_t i = 0; i < m_owned.size(); ++i)
	{
		if (m_owned[i]->getTemplate()->isEquivalentTo(thing)) ++result;
		if (m_owned[i]->isKindOf(KINDOF_REBUILD_HOLE))
		{
			RebuildHoleBehaviorInterface *hole = RebuildHoleBehavior::getRebuildHoleBehaviorInterfaceFromObject(m_owned[i]);
			if (hole && hole->getRebuildTemplate() && hole->getRebuildTemplate()->isEquivalentTo(thing)
				&& !findOwned(hole->getReconstructedBuildingID())) ++result;
		}
		ProductionUpdateInterface *production = m_owned[i]->getProductionUpdateInterface();
		if (queued && production) result += production->countUnitTypeInQueue(thing);
	}
	return result;
}
const CommandButton *CompetitiveSkirmishAI::button(Object *object, const char *name) const
{
	const CommandSet *commands = TheControlBar->findCommandSet(object->getCommandSetString());
	if (!commands) return nullptr;
	for (Int i = 0; i < MAX_COMMANDS_PER_SET; ++i)
	{
		const CommandButton *command = commands->getCommandButton(i);
		if (command && command->getName() == name) return command;
	}
	return nullptr;
}
Bool CompetitiveSkirmishAI::train(const char *name, Int desired, Bool emergency)
{
	const ThingTemplate *thing = name ? TheThingFactory->findTemplate(name) : nullptr;
	if (!thing || count(name) >= desired || !m_player->getCanBuildUnits()) return false;
	Int cost = thing->calcCostToBuild(m_player);
	Int reserve = emergency ? 0 : CompetitiveAIPolicy::reserve(counterCost());
	const Profile& p = profile();
	Bool savingTech = m_state == COMPETITIVE_TECHING && !m_threat.baseAttacked && !count(p.tech)
		&& count(p.basic, false) + count(p.antiAir, false) >= 2;
	if (savingTech)
	{
		Bool critical = thing->getName() == p.builder || thing->getName() == p.collector;
		if (thing->getName() == p.basic) critical = count(name, false) < 2;
		if (thing->getName() == p.infantry)
		{
			Int minimum = m_player->getSide() == "America" ? 3 * count(p.basic, false)
				: (m_player->getSide() == "China" ? (m_threat.vehicles ? 5 : 2) : 4);
			critical = count(name, false) < minimum;
		}
		if (thing->getName() == p.antiAir)
		{
			Int minimum = m_threat.air ? max(2, m_threat.air) : (m_threat.infantry >= 4 ? 2 : 0);
			critical = count(name, false) < minimum;
		}
		if (thing->getName() == "AmericaVehicleMedic" || thing->getName() == "AmericaInfantryRanger"
			|| thing->getName() == "ChinaInfantryRedguard" || thing->getName() == "ChinaVehicleListeningOutpost"
			|| thing->getName() == "GLAInfantryRebel") critical = count(name, false) == 0;
		if (!critical) reserve = max(reserve, price(p.tech) + price(m_threat.air ? p.antiAir : p.siege)
			+ CompetitiveAIPolicy::reserve(counterCost()));
	}
	if (m_player->getMoney()->countMoney() < UnsignedInt(cost + reserve)) return false;
	for (size_t i = 0; i < m_owned.size(); ++i)
	{
		Object *factory = m_owned[i];
		ProductionUpdateInterface *production = factory->getProductionUpdateInterface();
		if (!operational(factory) || !production || production->getProductionCount() > 0) continue;
		if (TheBuildAssistant->canMakeUnit(factory, thing) != CANMAKE_OK
			|| production->canQueueCreateUnit(thing) != CANMAKE_OK) continue;
		if (production->queueCreateUnit(thing, production->requestUniqueUnitID())) return true;
	}
	return false;
}
Object *CompetitiveSkirmishAI::builder(const ThingTemplate *thing, const Coord3D& position) const
{
	Object *best = nullptr;
	Real nearest = HUGE_DIST * HUGE_DIST;
	Int workers = 0;
	for (size_t i = 0; i < m_owned.size(); ++i)
	{
		Object *object = m_owned[i];
		if (!object->isKindOf(KINDOF_DOZER) || !operational(object) || !object->getAI()) continue;
		if (object->isKindOf(KINDOF_HARVESTER) && ++workers > 2) continue;
		DozerAIInterface *dozer = object->getAI()->getDozerAIInterface();
		if (!dozer || dozer->isAnyTaskPending() || (thing && TheBuildAssistant->canMakeUnit(object, thing) != CANMAKE_OK)) continue;
		Real dist = distanceSquared(*object->getPosition(), position);
		if (dist < nearest) { nearest = dist; best = object; }
	}
	return best;
}
Bool CompetitiveSkirmishAI::safe(const Coord3D& position, Real radius) const
{
	UnsignedInt frame = TheGameLogic->getFrame();
	for (ObservationMap::const_iterator it = m_observations.begin(); it != m_observations.end(); ++it)
	{
		const CompetitiveAIObservation& seen = it->second;
		if (seen.confidence(frame) <= 0 || !seen.armed) continue;
		if (m_player->getRelationship(ThePlayerList->getNthPlayer(seen.playerIndex)->getDefaultTeam()) != ENEMIES) continue;
		Real uncertainty = seen.hasPrecisePosition(frame) ? 0 : 120;
		if (distanceSquared(position, seen.position) < sqr(radius + uncertainty)) return false;
	}
	return true;
}
Bool CompetitiveSkirmishAI::airRouteSafe(const Coord3D& from, const Coord3D& to) const
{
	Real lengthSquared = max(1.0f, distanceSquared(from, to));
	UnsignedInt frame = TheGameLogic->getFrame();
	for (ObservationMap::const_iterator it = m_observations.begin(); it != m_observations.end(); ++it)
	{
		const CompetitiveAIObservation& seen = it->second;
		if (!seen.antiAir || seen.confidence(frame) <= 0 || m_player->getRelationship(
			ThePlayerList->getNthPlayer(seen.playerIndex)->getDefaultTeam()) != ENEMIES) continue;
		Real fraction = max(0.0f, min(1.0f, ((seen.position.x - from.x) * (to.x - from.x)
			+ (seen.position.y - from.y) * (to.y - from.y)) / lengthSquared));
		Coord3D closest = from;
		closest.x += fraction * (to.x - from.x); closest.y += fraction * (to.y - from.y);
		const ThingTemplate *thing = TheThingFactory->findTemplate(seen.templateName);
		const WeaponTemplateSet *weapons = thing ? thing->findWeaponTemplateSet(WeaponSetFlags()) : nullptr;
		Real range = 180;
		if (weapons) for (Int slot = 0; slot < WEAPONSLOT_COUNT; ++slot)
		{
			const WeaponTemplate *weapon = weapons->getNth(WeaponSlotType(slot));
			WeaponBonus bonus;
			if (weapon && (weapon->getAntiMask() & WEAPON_ANTI_AIRBORNE_VEHICLE)) range = max(range, weapon->getAttackRange(bonus));
		}
		if (!seen.hasPrecisePosition(frame)) range += 120;
		if (distanceSquared(closest, seen.position) < sqr(range + 50)) return false;
	}
	return true;
}
Bool CompetitiveSkirmishAI::build(const char *name, const Coord3D& anchor, Bool emergency, Int followupCost)
{
	if (!name || !m_player->getCanBuildBase()) return false;
	const ThingTemplate *thing = TheThingFactory->findTemplate(name);
	if (!thing) return false;
	Int reserve = emergency ? 0 : CompetitiveAIPolicy::reserve(counterCost());
	if (m_player->getMoney()->countMoney() < UnsignedInt(thing->calcCostToBuild(m_player) + reserve + followupCost)) return false;
	Object *dozer = builder(thing, anchor);
	if (!dozer) return false;
	if (ThePartitionManager->getShroudStatusForPlayer(m_player->getPlayerIndex(), &anchor) != CELLSHROUD_CLEAR)
	{
		// Public supply/start markers do not reveal their surroundings. Bring the constructor's vision first.
		if (safe(anchor, 300))
		{
			Coord3D approach = anchor;
			Real distance = max(1.0f, sqrt(distanceSquared(anchor, *dozer->getPosition())));
			approach.x += (dozer->getPosition()->x - anchor.x) * 130 / distance;
			approach.y += (dozer->getPosition()->y - anchor.y) * 130 / distance;
			move(dozer, approach, MOVE);
		}
		return false;
	}
	Real angle = thing->getPlacementViewAngle();
	// Use normal placement and path checks. The legacy builder can teleport an unreachable dozer.
	for (Int ring = 1; ring <= 10; ++ring)
	{
		for (Int direction = 0; direction < 12; ++direction)
		{
			Real theta = direction * (2 * PI / 12);
			Coord3D position = anchor;
			position.x += cos(theta) * ring * 35;
			position.y += sin(theta) * ring * 35;
			position.z = TheTerrainLogic->getGroundHeight(position.x, position.y);
			if (ThePartitionManager->getShroudStatusForPlayer(m_player->getPlayerIndex(), &position) != CELLSHROUD_CLEAR) continue;
			if (!safe(position, 220)) continue;
			Bool legal = TheBuildAssistant->isLocationLegalToBuild(&position, thing, angle,
				BuildAssistant::CLEAR_PATH | BuildAssistant::TERRAIN_RESTRICTIONS | BuildAssistant::NO_OBJECT_OVERLAP,
				dozer, m_player) == LBC_OK;
			if (TheTerrainVisual) TheTerrainVisual->removeAllBibs();
			if (!legal || !TheAI->pathfinder()->clientSafeQuickDoesPathExist(dozer->getAI()->getLocomotorSet(),
				dozer->getPosition(), &position)) continue;
			Object *building = TheBuildAssistant->buildObjectNow(dozer, thing, &position, angle, m_player);
			if (building)
			{
				AsciiString reason; reason.format("Competitive AI constructs %s", name);
				TheScriptEngine->AppendDebugMessage(reason, false);
				return true;
			}
		}
	}
	return false;
}

Bool CompetitiveSkirmishAI::supplyAvailable(const Coord3D& position) const
{
	if (ThePartitionManager->getShroudStatusForPlayer(m_player->getPlayerIndex(), &position) != CELLSHROUD_CLEAR) return true;
	for (size_t i = 0; i < m_visible.size(); ++i)
	{
		Object *source = m_visible[i];
		if (!source->isKindOf(KINDOF_SUPPLY_SOURCE) || distanceSquared(position, *source->getPosition()) > sqr(80.0f)) continue;
		SupplyWarehouseDockUpdate *dock = (SupplyWarehouseDockUpdate*)source->findUpdateModule(NAMEKEY("SupplyWarehouseDockUpdate"));
		if (dock && dock->getBoxesStored() > 0) return true;
	}
	return false;
}

Coord3D CompetitiveSkirmishAI::supplyGoal(Bool expansion) const
{
	Coord3D result = m_base;
	Real nearest = HUGE_DIST * HUGE_DIST;
	// Supply markers and start locations are public map geometry. Stock levels are not.
	for (size_t marker = 0; marker < m_supplyMarkers.size(); ++marker)
	{
		const Coord3D& position = m_supplyMarkers[marker];
		if (!supplyAvailable(position)) continue;
		if (expansion)
		{
			if (ThePartitionManager->getShroudStatusForPlayer(m_player->getPlayerIndex(), &position) != CELLSHROUD_CLEAR) continue;
			Bool served = false;
			for (size_t i = 0; i < m_owned.size(); ++i)
				if (m_owned[i]->isKindOf(KINDOF_FS_SUPPLY_CENTER)
					&& distanceSquared(position, *m_owned[i]->getPosition()) < sqr(200.0f)) served = true;
			if (served || !safe(position, 300)) continue;
		}
		Real dist = distanceSquared(position, m_base);
		if (dist < nearest) { result = position; nearest = dist; }
	}
	return result;
}

Coord3D CompetitiveSkirmishAI::scoutGoal()
{
	if (m_raidFailures >= 2) return m_raidFailedGoal;
	if (!m_threat.baseAttacked && count(profile().supply, false) == 1 && TheGameLogic->getFrame() < 120 * LOGICFRAMES_PER_SECOND)
	{
		Real closest = sqr(850.0f);
		Coord3D expansion = m_base;
		for (size_t marker = 0; marker < m_supplyMarkers.size(); ++marker)
		{
			const Coord3D& position = m_supplyMarkers[marker];
			if (ThePartitionManager->getShroudStatusForPlayer(m_player->getPlayerIndex(), &position) == CELLSHROUD_CLEAR) continue;
			Bool served = false;
			for (size_t i = 0; i < m_owned.size(); ++i)
				if (m_owned[i]->isKindOf(KINDOF_FS_SUPPLY_CENTER)
					&& distanceSquared(position, *m_owned[i]->getPosition()) < sqr(200.0f)) served = true;
			Real distance = distanceSquared(position, m_base);
			if (!served && safe(position, 300) && distance < closest) { expansion = position; closest = distance; }
		}
		if (distanceSquared(expansion, m_base) > sqr(150.0f)) return expansion;
	}
	std::vector<Coord3D> goals;
	for (Waypoint *way = TheTerrainLogic->getFirstWaypoint(); way; way = way->getNext())
	{
		if (contains(way->getName(), "player_") && contains(way->getName(), "_start")
			&& distanceSquared(*way->getLocation(), m_base) > sqr(250.0f)) goals.push_back(*way->getLocation());
	}
	if (goals.empty()) return m_base;
	return goals[m_scoutIndex % goals.size()];
}

void CompetitiveSkirmishAI::strategy()
{
	UnsignedInt frame = TheGameLogic->getFrame();
	const Profile& p = profile();
	CompetitiveAIState proposed = COMPETITIVE_PRESSURING;
	if (!count(p.supply, false) || !count(p.builder, false)) proposed = COMPETITIVE_RECOVERING;
	else if (m_threat.baseAttacked) proposed = COMPETITIVE_DEFENDING;
	else if (!count(p.factory, false)) proposed = COMPETITIVE_OPENING;
	else if (!count(p.tech, false)) proposed = COMPETITIVE_TECHING;
	else if (m_hasGoal && m_owned.size() >= 20) proposed = COMPETITIVE_FINISHING;
	if (proposed != m_state && CompetitiveAIPolicy::changeState(frame, m_changed,
		proposed == COMPETITIVE_DEFENDING || proposed == COMPETITIVE_RECOVERING))
	{
		m_state = proposed; m_changed = frame;
		AsciiString reason; reason.format("Competitive AI state %d (air %d, infantry %d, vehicles %d)",
			Int(m_state), m_threat.air, m_threat.infantry, m_threat.vehicles);
		TheScriptEngine->AppendDebugMessage(reason, false);
	}
	Real best = -1;
	Coord3D goal = m_goal;
	Real goalHealth = 0;
	for (ObservationMap::const_iterator it = m_observations.begin(); it != m_observations.end(); ++it)
	{
		const CompetitiveAIObservation& seen = it->second;
		if (m_player->getRelationship(ThePlayerList->getNthPlayer(seen.playerIndex)->getDefaultTeam()) != ENEMIES) continue;
		if (!seen.hasPrecisePosition(frame)) continue;
		Real value = seen.superweapon ? 12 : (seen.economy ? 8 : (seen.structure ? 4 : 2));
		if (seen.superweapon && seen.publicReadyFrame && seen.publicReadyFrame < frame + 120 * LOGICFRAMES_PER_SECOND) value += 12;
		if (m_threat.baseAttacked && distanceSquared(seen.position, m_base) < sqr(350.0f)) value += 20;
		value = value * seen.confidence(frame) / (1 + sqrt(distanceSquared(seen.position, m_base)) / 1000);
		if (value > best) { best = value; goal = seen.position; goalHealth = seen.health; }
	}
	if (best >= 0)
	{
		if (!m_hasGoal || distanceSquared(goal, m_goal) > sqr(100.0f) || goalHealth < m_goalHealth)
			m_lastProgress = frame;
		m_goal = goal; m_goalHealth = goalHealth; m_hasGoal = true;
	}
	else m_hasGoal = false;
	planRaid();
}

void CompetitiveSkirmishAI::planRaid()
{
	if (m_player->getSide() != "GLA" || m_threat.baseAttacked) return;
	ObservationMap::const_iterator existing = m_observations.find(m_raidTarget);
	if (existing != m_observations.end() && !raidBlocked(existing->second.position)) return;
	m_raidTarget = INVALID_ID; m_raidPayload = 0;
	const ThingTemplate *terrorist = TheThingFactory->findTemplate("GLAInfantryTerrorist");
	const ThingTemplate *technical = TheThingFactory->findTemplate("GLAVehicleTechnical");
	const WeaponTemplateSet *weapons = terrorist ? terrorist->findWeaponTemplateSet(WeaponSetFlags()) : nullptr;
	const WeaponTemplate *bomb = weapons ? weapons->getNth(PRIMARY_WEAPON) : nullptr;
	if (!bomb || !technical) return;
	Object *transport = nullptr;
	for (size_t i = 0; i < m_owned.size(); ++i)
		if (m_owned[i]->getTemplate()->isEquivalentTo(technical) && operational(m_owned[i])) transport = m_owned[i];
	if (!transport || !transport->getContain()) return;
	Real best = 0;
	WeaponBonus bonus;
	for (size_t i = 0; i < m_visible.size(); ++i)
	{
		Object *target = m_visible[i];
		if (raidBlocked(*target->getPosition())) continue;
		if (m_player->getRelationship(target->getTeam()) != ENEMIES || !target->isKindOf(KINDOF_STRUCTURE)
			|| !(target->isKindOf(KINDOF_FS_SUPPLY_CENTER) || target->isKindOf(KINDOF_FS_FACTORY) || target->isKindOf(KINDOF_CASH_GENERATOR))
			|| !safe(*target->getPosition(), 230)) continue;
		Real damage = bomb->estimateWeaponTemplateDamage(transport, target, nullptr, bonus);
		if (damage <= 0) continue;
		Int passengers = Int(ceil(target->getBodyModule()->getHealth() * 1.1f / damage));
		if (passengers <= 0 || passengers > transport->getContain()->getContainMax()) continue;
		Real value = target->getTemplate()->friend_getBuildCost() - passengers * price("GLAInfantryTerrorist");
		if (value > best) { best = value; m_raidTarget = target->getID(); m_raidPayload = passengers; }
	}
}

void CompetitiveSkirmishAI::recordRaid(Object *unit, Object *target)
{
	if (!target || !canObserve(m_player, target)) return;
	UnitOrder& mission = m_orders[unit->getID()];
	Int invested = unit->getTemplate()->calcCostToBuild(m_player);
	const ContainedItemsList *contents = unit->getContain() ? unit->getContain()->getContainedItemsList() : nullptr;
	if (contents) for (ContainedItemsList::const_iterator it = contents->begin(); it != contents->end(); ++it)
		invested += (*it)->getTemplate()->calcCostToBuild(m_player);
	mission.raidCost = max(mission.raidCost, invested);
	if (mission.raidTarget == target->getID()) return;
	if (mission.raidTarget == INVALID_ID || distanceSquared(mission.raidGoal, *target->getPosition()) >= sqr(300.0f))
	{
		mission.raidGoal = *target->getPosition(); mission.raidGain = 0;
		mission.raidCost = invested;
	}
	mission.raidTarget = target->getID();
	mission.raidHealth = target->getBodyModule()->getHealth();
	mission.raidMaxHealth = target->getBodyModule()->getMaxHealth();
	mission.raidValue = target->getTemplate()->friend_getBuildCost();
}

Bool CompetitiveSkirmishAI::raidBlocked(const Coord3D& position) const
{
	for (size_t route = 0; route < m_raidRoutes.size(); ++route)
		if (m_raidRoutes[route].failures >= 2 && distanceSquared(position, m_raidRoutes[route].position) < sqr(300.0f)) return true;
	return false;
}

void CompetitiveSkirmishAI::economy()
{
	const Profile& p = profile();
	UnsignedInt frame = TheGameLogic->getFrame();
	if (!m_collectorQuota) m_collectorQuota = p.collectors;
	Int supplies = count(p.supply, false);
	Int army = 0;
	for (size_t i = 0; i < m_owned.size(); ++i)
		if (m_owned[i]->isKindOf(KINDOF_SELECTABLE) && !m_owned[i]->isKindOf(KINDOF_PROJECTILE)
			&& !m_owned[i]->isKindOf(KINDOF_STRUCTURE) && !m_owned[i]->isKindOf(KINDOF_DOZER)
			&& !m_owned[i]->isKindOf(KINDOF_HARVESTER) && m_owned[i]->getAI()
			&& m_owned[i]->hasAnyDamageWeapon()) ++army;

	// Two reserved GLA Workers construct; the rest retain the game's supply autopilot.
	Bool gla = m_player->getSide() == "GLA";
	Int desiredBuilders = gla ? 2 + m_collectorQuota * supplies : 2;
	train(p.builder, desiredBuilders);
	for (size_t i = 0; i < m_owned.size(); ++i)
	{
		Object *building = m_owned[i];
		if (!building->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION)) continue;
		Object *oldBuilder = findOwned(building->getBuilderID());
		DozerAIInterface *working = oldBuilder && oldBuilder->getAI() ? oldBuilder->getAI()->getDozerAIInterface() : nullptr;
		if ((!oldBuilder || !operational(oldBuilder) || !working || !working->isTaskPending(DOZER_TASK_BUILD)
			|| working->getTaskTarget(DOZER_TASK_BUILD) != building->getID()) && safe(*building->getPosition(), 220))
		{
			Object *replacement = builder(nullptr, *building->getPosition());
			if (replacement && TheActionManager->canResumeConstructionOf(replacement, building, CMD_FROM_PLAYER))
				replacement->getAI()->aiResumeConstruction(building, CMD_FROM_AI);
		}
	}
	if (p.power && !m_player->getEnergy()->hasSufficientPower())
	{
		Bool pending = false;
		for (size_t i = 0; i < m_owned.size(); ++i)
			if (named(m_owned[i], p.power) && m_owned[i]->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION)) pending = true;
		if (!pending && build(p.power, m_base, true)) return;
	}
	if (p.power && !count(p.power)) { build(p.power, m_base, true); return; }
	if (!supplies) { build(p.supply, supplyGoal(false), true); return; }
	if (!count(p.commandCenter) && build(p.commandCenter, m_base, true)) return;
	if (!count(p.barracks)) { if (build(p.barracks, m_base, true)) return; }
	if (!count(p.factory)) { if (build(p.factory, m_base, true)) return; }
	for (size_t i = 0; i < m_owned.size(); ++i)
	{
		Object *building = m_owned[i];
		if (!building->isKindOf(KINDOF_STRUCTURE) || !operational(building)
			|| building->getBodyModule()->getHealth() >= 0.6f * building->getBodyModule()->getMaxHealth()
			|| !(building->isKindOf(KINDOF_COMMANDCENTER) || building->isKindOf(KINDOF_FS_FACTORY)
				|| building->isKindOf(KINDOF_FS_SUPPLY_CENTER) || building->isKindOf(KINDOF_FS_POWER))
			|| !safe(*building->getPosition(), 220)) continue;
		Object *repairer = builder(nullptr, *building->getPosition());
		if (repairer && TheActionManager->canRepairObject(repairer, building, CMD_FROM_PLAYER))
			repairer->getAI()->aiRepair(building, CMD_FROM_AI);
	}

	if (!gla) train(p.collector, m_collectorQuota * supplies);
	Int workerIndex = 0;
	UnsignedInt longestCycle = 0, longestWait = 0;
	for (size_t i = 0; i < m_owned.size(); ++i)
	{
		Object *object = m_owned[i];
		if (!named(object, p.collector) || !object->getAI() || !operational(object)) continue;
		if (object->isKindOf(KINDOF_DOZER) && ++workerIndex <= 2) continue;
		SupplyTruckAIInterface *collector = object->getAI()->getSupplyTruckAIInterface();
		if (collector)
		{
			UnitOrder& gathering = m_orders[object->getID()];
			Int boxes = collector->getNumberBoxes();
			if (gathering.supplyBoxes > 0 && boxes == 0)
			{
				if (gathering.delivered) gathering.cycleFrames = gathering.cycleFrames
					? (gathering.cycleFrames * 3 + frame - gathering.delivered) / 4 : frame - gathering.delivered;
				gathering.delivered = frame;
			}
			gathering.supplyBoxes = boxes;
			if (!boxes && collector->isCurrentlyFerryingSupplies() && !object->getAI()->isMoving())
				gathering.waitingFrames += LOGICFRAMES_PER_SECOND;
			else gathering.waitingFrames = gathering.waitingFrames * 3 / 4;
			longestCycle = max(longestCycle, gathering.cycleFrames);
			longestWait = max(longestWait, gathering.waitingFrames);
		}
		if (collector && !collector->isCurrentlyFerryingSupplies() && object->getAI()->isIdle())
		{
			Object *dockTarget = nullptr;
			Real nearest = HUGE_DIST * HUGE_DIST;
			const std::vector<Object*>& docks = collector->getNumberBoxes() > 0 ? m_owned : m_visible;
			for (size_t j = 0; j < docks.size(); ++j)
			{
				Object *dock = docks[j];
				if (collector->getNumberBoxes() > 0)
				{
					if (!named(dock, p.supply) || !operational(dock)) continue;
				}
				else
				{
					if (!dock->isKindOf(KINDOF_SUPPLY_SOURCE)) continue;
					SupplyWarehouseDockUpdate *warehouse = (SupplyWarehouseDockUpdate*)dock->findUpdateModule(NAMEKEY("SupplyWarehouseDockUpdate"));
					if (!warehouse || warehouse->getBoxesStored() <= 0) continue;
				}
				Real dist = distanceSquared(*object->getPosition(), *dock->getPosition());
				if (safe(*dock->getPosition(), 220) && dist < nearest) { nearest = dist; dockTarget = dock; }
			}
			if (dockTarget && TheActionManager->canDockAt(object, dockTarget, CMD_FROM_PLAYER))
				object->getAI()->aiDock(dockTarget, CMD_FROM_PLAYER);
			else collector->setForceWantingState(true);
		}
	}
	if (frame >= m_nextCollectorAdjustment)
	{
		if (longestWait > 20 * LOGICFRAMES_PER_SECOND) m_collectorQuota = max(gla ? 5 : 2, m_collectorQuota - 1);
		else if (longestCycle > 25 * LOGICFRAMES_PER_SECOND && longestWait < 5 * LOGICFRAMES_PER_SECOND)
			m_collectorQuota = min(p.collectors + (gla ? 2 : 1), m_collectorQuota + 1);
		m_nextCollectorAdjustment = frame + 30 * LOGICFRAMES_PER_SECOND;
	}
	if (!m_threat.baseAttacked && army >= 2 && supplies < 2)
	{
		Coord3D expansion = supplyGoal(true);
		if (distanceSquared(expansion, m_base) > sqr(150.0f) && build(p.supply, expansion, false)) return;
	}
	if (m_player->getSide() == "GLA" && count("GLATunnelNetwork") < supplies)
	{
		for (size_t i = 0; i < m_owned.size(); ++i)
		{
			if (!named(m_owned[i], p.supply)) continue;
			Bool covered = false;
			for (size_t j = 0; j < m_owned.size(); ++j)
				if (named(m_owned[j], "GLATunnelNetwork") && distanceSquared(*m_owned[i]->getPosition(),
					*m_owned[j]->getPosition()) < sqr(180.0f)) covered = true;
			if (!covered && build("GLATunnelNetwork", *m_owned[i]->getPosition(), true)) return;
		}
	}
	if (!m_threat.baseAttacked && army >= 4 && !count(p.tech))
		if (build(p.tech, m_base, false, price(m_threat.air ? p.antiAir : p.siege))) return;
	// Stabilize the opening before saving for the complete technology package. Necessary counters still override this cap.
	Bool savingTech = !m_threat.baseAttacked && !count(p.tech);

	if (m_player->getSide() == "America")
	{
		Int humvees = count(p.basic);
		train("AmericaInfantryRanger", m_threat.infantry ? max(1, humvees) : 1);
		train(p.infantry, max(3, 3 * humvees));
		if (count(p.basic, false) >= 2) train("AmericaVehicleMedic", max(1, (humvees + 3) / 4));
		if (m_threat.air || m_threat.vehicles >= 4) train(p.antiAir, max(1, humvees / 4));
		if (m_threat.defenses) train(p.siege, max(2, m_threat.defenses));
		if (m_threat.vehicles >= 6 && m_threat.infantry < 3 && humvees >= 2) train("AmericaTankCrusader", 2);
		train(p.basic, m_threat.baseAttacked ? 10 : (savingTech ? 3 : 6));
		if (!m_threat.baseAttacked && army >= 6 && m_threat.explored && m_hasGoal && m_threat.antiAir == 0
			&& (m_threat.air >= 2 || m_threat.vehicles >= 6) && airRouteSafe(m_base, m_goal))
		{
			if (!count("AmericaAirfield")) build("AmericaAirfield", m_base, false, 4 * price("AmericaJetRaptor"));
			train("AmericaJetRaptor", 4, false);
		}
	}
	else if (m_player->getSide() == "China")
	{
		train("ChinaInfantryRedguard", 1);
		train("ChinaVehicleListeningOutpost", 1);
		train(p.antiAir, m_threat.air ? max(4, m_threat.air) : max(2, m_threat.infantry / 3));
		train(p.infantry, m_threat.vehicles ? 5 : 2);
		train("ChinaTankDragon", m_threat.infantry >= 4 ? 1 : 0);
		if (count(p.tech, false))
		{
			train("ChinaTankOverlord", 1);
			train("ChinaTankECM", 2);
			train("ChinaTankOverlord", 3);
			if (m_threat.defenses || m_threat.infantry >= 5) train(p.siege, 3);
		}
		else train(p.basic, m_threat.vehicles ? 4 : 2);
		if (!m_threat.baseAttacked && army >= 6 && m_threat.explored && m_hasGoal
			&& (m_threat.defenses >= 2 || m_threat.infantry >= 5) && airRouteSafe(m_base, m_goal))
		{
			if (!count("ChinaAirfield")) build("ChinaAirfield", m_base, false, 4 * price("ChinaJetMIG"));
			train("ChinaJetMIG", 4, false);
		}
		if (!m_threat.baseAttacked && army >= 6 && m_threat.explored && m_hasGoal && m_threat.antiAir == 0
			&& airRouteSafe(m_base, m_goal))
		{
			if (!count("ChinaAirfield")) build("ChinaAirfield", m_base, false, price("ChinaVehicleHelix") + 5 * price(p.infantry));
			train("ChinaVehicleHelix", 1, false);
			if (count("ChinaVehicleHelix")) train(p.infantry, 5 + count("ChinaVehicleHelix") * 5);
		}
	}
	else
	{
		train("GLAInfantryRebel", 1);
		train(p.antiAir, m_threat.air ? max(4, 2 * m_threat.air) : 3);
		train(p.basic, m_threat.vehicles ? 5 : 3);
		train(p.infantry, 4 + count("GLAVehicleBattleBus") * 6);
		if (!m_threat.baseAttacked && (m_raidFailures < 2 || m_raidTarget != INVALID_ID
			|| (m_hasGoal && !raidBlocked(m_goal)))) train("GLAVehicleTechnical", 1);
		if (m_raidTarget != INVALID_ID) train("GLAInfantryTerrorist", m_raidPayload, false);
		if (count(p.tech, false))
		{
			if (m_threat.vehicles >= 4) train("GLAVehicleBattleBus", 1);
			train(p.siege, min(6, max(3, m_threat.vehicles + m_threat.defenses)));
		}
		if (count("GLAVehicleRadarVan") == 0 && army >= 4) train("GLAVehicleRadarVan", 1);
	}
	if (count(p.tech, false) && !m_threat.baseAttacked) train(p.hero, 1, false);

	// Inspect remaining stock only at supply sources we can currently see.
	Int remaining = 0;
	Bool stockKnown = false;
	for (size_t i = 0; i < m_visible.size(); ++i)
	{
		Object *source = m_visible[i];
		if (!source->isKindOf(KINDOF_SUPPLY_SOURCE) || distanceSquared(*source->getPosition(), m_base) > sqr(1000.0f)) continue;
		SupplyWarehouseDockUpdate *dock = (SupplyWarehouseDockUpdate*)source->findUpdateModule(NAMEKEY("SupplyWarehouseDockUpdate"));
		if (dock) { remaining += dock->getBoxesStored() * TheGlobalData->m_baseValuePerSupplyBox; stockKnown = true; }
	}
	UnsignedInt income = m_player->getMoney()->getCashPerMinute();
	Bool depleting = stockKnown && income > 0 && UnsignedInt(remaining) < 2 * income;
	if (!m_threat.baseAttacked && count(p.tech, false) && (depleting || frame > 360 * LOGICFRAMES_PER_SECOND))
	{
		Coord3D expansion = supplyGoal(true);
		if (depleting && supplies < 3 && distanceSquared(expansion, m_base) > sqr(150.0f)
			&& build(p.supply, expansion, false)) return;
		if (m_player->getSide() == "China")
		{
			Int hackers = 2 + Int(frame / (60 * LOGICFRAMES_PER_SECOND));
			train("ChinaInfantryHacker", hackers, false);
			if (count("ChinaInfantryHacker", false) >= 8 && !count(p.secondary)) build(p.secondary, m_base, false);
		}
		else if (count(p.secondary) < 1 + Int(frame / (180 * LOGICFRAMES_PER_SECOND)))
			build(p.secondary, m_base, false);
	}
	if (m_hasGoal && !m_threat.baseAttacked && income > 0 && count(p.tech, false)
		&& frame - m_lastProgress > 90 * LOGICFRAMES_PER_SECOND && !count(p.superweapon))
		build(p.superweapon, m_base, false);
	if (gla && !m_threat.baseAttacked && army >= 5 && count("GLATunnelNetwork", false) >= 2
		&& count("GLATunnelNetwork") < 3 && m_hasGoal)
	{
		// A forward tunnel needs our scout's present vision and a useful position outside the enemy's fire.
		Coord3D forward = m_base;
		forward.x += (m_goal.x - m_base.x) * 0.55f; forward.y += (m_goal.y - m_base.y) * 0.55f;
		if (distanceSquared(forward, m_base) > sqr(350.0f) && safe(forward, 350)
			&& ThePartitionManager->getShroudStatusForPlayer(m_player->getPlayerIndex(), &forward) == CELLSHROUD_CLEAR)
			build("GLATunnelNetwork", forward, false);
	}
}

Real CompetitiveSkirmishAI::templateDamageRate(const ThingTemplate *thing, Object *target, const Coord3D& position) const
{
	const WeaponTemplateSet *weapons = thing ? thing->findWeaponTemplateSet(WeaponSetFlags()) : nullptr;
	if (!thing || !target) return 0;
	Real best = 0;
	WeaponBonus bonus;
	if (weapons) for (Int slot = 0; slot < WEAPONSLOT_COUNT; ++slot)
	{
		const WeaponTemplate *weapon = weapons->getNth(WeaponSlotType(slot));
		if (!weapon) continue;
		Int mask = target->isAirborneTarget() ? WEAPON_ANTI_AIRBORNE_VEHICLE : WEAPON_ANTI_GROUND;
		if (!(weapon->getAntiMask() & mask)) continue;
		DamageInfoInput estimate;
		estimate.m_damageType = weapon->getDamageType(); estimate.m_deathType = weapon->getDeathType();
		estimate.m_amount = weapon->getPrimaryDamage(bonus);
		Real damage = target->getBodyModule()->estimateDamage(estimate);
		Int delay = max(1, weapon->getDelayBetweenShots(bonus));
		Real shots = Real(5 * LOGICFRAMES_PER_SECOND) / delay;
		if (weapon->getClipSize() > 0)
		{
			shots = min(shots, Real(weapon->getClipSize()));
			if (weapon->getAutoReloadsClip() && weapon->getClipReloadTime(bonus) < 5 * LOGICFRAMES_PER_SECOND)
				shots += Real(weapon->getClipSize()) * 0.5f;
		}
		Real contact = min(1.0f, weapon->getAttackRange(bonus) / max(1.0f, sqrt(distanceSquared(position, *target->getPosition()))));
		best = max(best, damage * shots * contact / 5);
	}
	Int expected = 0;
	const char *passenger = passengerPrior(thing->getName(), expected);
	if (passenger) best += expected * templateDamageRate(TheThingFactory->findTemplate(passenger), target, position);
	return best;
}

Real CompetitiveSkirmishAI::damageRate(Object *source, Object *target, Bool passengers) const
{
	if (!source || !target || !operational(source)) return 0;
	if (source->getControllingPlayer() != m_player)
	{
		// Visibility does not disclose internal ammunition, passenger lists or unobserved upgrade state.
		return templateDamageRate(source->getTemplate(), target, *source->getPosition());
	}
	Real best = 0;
	for (Int slot = 0; slot < WEAPONSLOT_COUNT; ++slot)
	{
		Weapon *weapon = source->getWeaponInWeaponSlot(WeaponSlotType(slot));
		if (!weapon || !weapon->isDamageWeapon()) continue;
		if (source->getAbleToUseWeaponAgainstTarget(ATTACK_NEW_TARGET, target, nullptr, CMD_FROM_PLAYER,
			WeaponSlotType(slot)) == ATTACKRESULT_NOT_POSSIBLE) continue;
		WeaponBonus bonus;
		WeaponBonusConditionFlags flags = source->getWeaponBonusCondition();
		if (source->getControllingPlayer() == m_player && source->getContainedBy())
		{
			ContainModuleInterface *container = source->getContainedBy()->getContain();
			if (container && container->isWeaponBonusPassedToPassengers()) flags |= container->getWeaponBonusPassedToPassengers();
		}
		if (TheGlobalData->m_weaponBonusSet) TheGlobalData->m_weaponBonusSet->appendBonuses(flags, bonus);
		if (weapon->getTemplate()->getExtraBonus()) weapon->getTemplate()->getExtraBonus()->appendBonuses(flags, bonus);
		Int delay = max(1, weapon->getTemplate()->getDelayBetweenShots(bonus));
		Real shots = Real(5 * LOGICFRAMES_PER_SECOND) / delay;
		if (weapon->getClipSize() > 0)
		{
			shots = min(shots, Real(weapon->getRemainingAmmo()));
			if (weapon->getAutoReloadsClip() && weapon->getClipReloadTime(source) < 5 * LOGICFRAMES_PER_SECOND)
				shots += Real(weapon->getClipSize()) * 0.5f;
		}
		Real range = weapon->getAttackRange(source);
		Real dist = sqrt(distanceSquared(*source->getPosition(), *target->getPosition()));
		Real contact = min(1.0f, range / max(1.0f, dist));
		best = max(best, weapon->estimateWeaponDamage(source, target) * shots * contact / 5);
	}
	if (passengers && source->getControllingPlayer() == m_player && source->getContain())
	{
		const ContainedItemsList *contents = source->getContain()->getContainedItemsList();
		if (contents)
			for (ContainedItemsList::const_iterator it = contents->begin(); it != contents->end(); ++it)
				best += damageRate(*it, target, false);
	}
	return best;
}

Real CompetitiveSkirmishAI::localStrength(const Coord3D& position, Bool ours, Object *against) const
{
	if (!against) return 0;
	const std::vector<Object*>& objects = ours ? m_owned : m_visible;
	Real result = 0;
	for (size_t i = 0; i < objects.size(); ++i)
	{
		Object *object = objects[i];
		if (object->getContainedBy() || !operational(object)) continue;
		if (!ours && m_player->getRelationship(object->getTeam()) != ENEMIES) continue;
		if (distanceSquared(*object->getPosition(), position) > sqr(450.0f)) continue;
		Real health = object->getBodyModule()->getHealth();
		result += damageRate(object, against, ours) * sqrt(max(0.0f, health));
	}
	if (!ours)
	{
		UnsignedInt frame = TheGameLogic->getFrame();
		for (ObservationMap::const_iterator it = m_observations.begin(); it != m_observations.end(); ++it)
		{
			const CompetitiveAIObservation& seen = it->second;
			if (visibleTarget(seen.id) || seen.confidence(frame) <= 0 || seen.economy
				|| (!seen.armed && !seen.vehicle) || m_player->getRelationship(
					ThePlayerList->getNthPlayer(seen.playerIndex)->getDefaultTeam()) != ENEMIES) continue;
			Real uncertainty = seen.hasPrecisePosition(frame) ? 0 : 120;
			if (distanceSquared(position, seen.position) > sqr(450.0f + uncertainty)) continue;
			const ThingTemplate *thing = TheThingFactory->findTemplate(seen.templateName);
			Real rate = templateDamageRate(thing, against, seen.position);
			// The remembered location represents an uncertain region after 15 seconds, not a live target.
			result += rate * sqrt(max(0.0f, seen.health)) * max(0.25f, seen.confidence(frame));
		}
	}
	return result;
}

Bool CompetitiveSkirmishAI::move(Object *object, const Coord3D& position, Order action, Object *target)
{
	AIUpdateInterface *ai = object->getAI();
	if (!ai || !operational(object)) return false;
	if (target && target->getControllingPlayer() != m_player && !canObserve(m_player, target)) return false;
	UnitOrder& record = m_orders[object->getID()];
	UnsignedInt frame = TheGameLogic->getFrame();
	ObjectID targetID = target ? target->getID() : INVALID_ID;
	Bool urgent = action == RETREAT && record.order != RETREAT;
	if (!urgent && frame < record.issued + LOGICFRAMES_PER_SECOND / 2) return false;
	if (!urgent && record.order == action && record.target == targetID
		&& distanceSquared(record.goal, position) < sqr(20.0f) && !ai->isIdle()) return false;
	if (action == ATTACK && target)
	{
		if (!object->isAbleToAttack() || object->getAbleToAttackSpecificObject(ATTACK_NEW_TARGET, target, CMD_FROM_PLAYER)
			== ATTACKRESULT_NOT_POSSIBLE) return false;
		ai->aiAttackObject(target, 0, CMD_FROM_AI);
		if (record.role == RAID) recordRaid(object, target);
	}
	else if (action == ENTER && target)
	{
		if (!TheActionManager->canEnterObject(object, target, CMD_FROM_PLAYER, CHECK_CAPACITY)) return false;
		ai->aiEnter(target, CMD_FROM_AI);
	}
	else
	{
		// Native movement finds an approach to occupied destinations. A public start/supply marker
		// can be inside a building; probing its exact cell previously prevented scouts from ever moving.
		ai->aiMoveToPosition(&position, CMD_FROM_AI);
	}
	record.order = action; record.target = targetID; record.goal = position; record.issued = frame;
	return true;
}

void CompetitiveSkirmishAI::loadTransports()
{
	std::map<ObjectID, Int> reserved;
	UnsignedInt frame = TheGameLogic->getFrame();
	for (OrderMap::const_iterator it = m_orders.begin(); it != m_orders.end(); ++it)
		if (it->second.order == ENTER && frame - it->second.issued < 10 * LOGICFRAMES_PER_SECOND)
		{
			Object *unit = findOwned(it->first);
			if (unit && !unit->getContainedBy()) ++reserved[it->second.target];
		}
	for (size_t i = 0; i < m_owned.size(); ++i)
	{
		Object *infantry = m_owned[i];
		Bool defender = named(infantry, "AmericaInfantryMissileDefender");
		Bool ranger = named(infantry, "AmericaInfantryRanger");
		Bool rpg = named(infantry, "GLAInfantryTunnelDefender");
		Bool tankHunter = named(infantry, "ChinaInfantryTankHunter");
		Bool terrorist = named(infantry, "GLAInfantryTerrorist") && m_raidTarget != INVALID_ID;
		if ((!defender && !rpg && !tankHunter && !terrorist && !(ranger && m_threat.infantry)) || infantry->getContainedBy()) continue;
		UnitOrder& order = m_orders[infantry->getID()];
		if (order.order == ENTER && frame - order.issued < 10 * LOGICFRAMES_PER_SECOND && findOwned(order.target)) continue;
		for (size_t j = 0; j < m_owned.size(); ++j)
		{
			Object *transport = m_owned[j];
			Int desired = 0;
			if ((defender || ranger) && named(transport, "AmericaVehicleHumvee")) desired = 3 + (m_threat.infantry > 0);
			if (rpg && named(transport, "GLAVehicleBattleBus")) desired = 6;
			if (tankHunter && named(transport, "ChinaVehicleHelix")) desired = 5;
			if (terrorist && contains(transport->getTemplate()->getName(), "technical")) desired = m_raidPayload;
			ContainModuleInterface *container = transport->getContain();
			if (!desired || !container || !operational(transport) || transport->getBodyModule()->getHealth()
				< 0.6f * transport->getBodyModule()->getMaxHealth()) continue;
			desired = min(desired, container->getContainMax());
			if (Int(container->getContainCount()) + reserved[transport->getID()] >= desired) continue;
			if (ranger && container->getContainCount() < 3) continue;
			if (distanceSquared(*infantry->getPosition(), *transport->getPosition()) > sqr(250.0f)) continue;
			if (move(infantry, *transport->getPosition(), ENTER, transport))
			{
				++reserved[transport->getID()]; break;
			}
		}
	}
}

Bool CompetitiveSkirmishAI::bombTransport(Object *object)
{
	if (!contains(object->getTemplate()->getName(), "technical") || m_raidTarget == INVALID_ID || !object->getContain()) return false;
	UnitOrder& record = m_orders[object->getID()];
	Real health = object->getBodyModule()->getHealth();
	if (health < object->getBodyModule()->getMaxHealth() * 0.35f
		|| (record.damagePerSecond > 0 && health / record.damagePerSecond <= 2))
	{
		if (safe(*object->getPosition(), 80)) object->getAI()->aiEvacuate(false, CMD_FROM_AI);
		move(object, m_base, RETREAT); return true;
	}
	ObservationMap::const_iterator remembered = m_observations.find(m_raidTarget);
	if (remembered == m_observations.end()) return false;
	const CompetitiveAIObservation& target = remembered->second;
	const ContainedItemsList *contents = object->getContain()->getContainedItemsList();
	Int loaded = 0;
	if (contents) for (ContainedItemsList::const_iterator it = contents->begin(); it != contents->end(); ++it)
		if (named(*it, "GLAInfantryTerrorist")) ++loaded;
	if (loaded < m_raidPayload)
	{
		if (distanceSquared(*object->getPosition(), m_base) > sqr(180.0f)) move(object, m_base, MOVE);
		return true;
	}
	Object *visible = visibleTarget(m_raidTarget);
	if (visible) recordRaid(object, visible);
	if (!safe(target.position, 230)) { move(object, m_base, RETREAT); return true; }
	if (visible && distanceSquared(*object->getPosition(), target.position) < sqr(120.0f))
	{
		for (size_t i = 0; i < m_owned.size(); ++i)
			if (m_owned[i] != object && !m_owned[i]->getContainedBy() && !named(m_owned[i], "GLAInfantryTerrorist")
				&& distanceSquared(*m_owned[i]->getPosition(), target.position) < sqr(120.0f)) return true;
		// Unloading is the normal containment command. The passengers then receive visible-object attack orders.
		if (contents) for (ContainedItemsList::const_iterator it = contents->begin(); it != contents->end(); ++it)
			if (named(*it, "GLAInfantryTerrorist"))
			{
				UnitOrder& passenger = m_orders[(*it)->getID()];
				passenger.target = m_raidTarget; passenger.order = NONE;
			}
		object->getAI()->aiEvacuate(false, CMD_FROM_AI);
		move(object, m_base, RETREAT);
	}
	else move(object, target.position, MOVE);
	return true;
}

Bool CompetitiveSkirmishAI::transportReady(Object *object) const
{
	Int desired = named(object, "AmericaVehicleHumvee") ? 3
		: (named(object, "GLAVehicleBattleBus") ? 6 : (named(object, "ChinaVehicleHelix") ? 5 : 0));
	if (!desired) return true;
	ContainModuleInterface *container = object->getContain();
	if (!container || container->getContainMax() <= 0) return false;
	Int defenders = 0;
	const ContainedItemsList *contents = container->getContainedItemsList();
	if (contents) for (ContainedItemsList::const_iterator it = contents->begin(); it != contents->end(); ++it)
		if (named(*it, "AmericaInfantryMissileDefender") || named(*it, "GLAInfantryTunnelDefender")
			|| named(*it, "ChinaInfantryTankHunter")) ++defenders;
	return defenders >= min(desired, container->getContainMax());
}

Bool CompetitiveSkirmishAI::attackAircraft(Object *object)
{
	JetAIUpdate *jet = object->getAI()->getJetAIUpdate();
	if (!jet) return false;
	UnitOrder& record = m_orders[object->getID()];
	Weapon *weapon = object->getCurrentWeapon();
	if (jet->isReloading() || jet->isTaxiingToParking()) return true;
	if (!weapon || weapon->getRemainingAmmo() <= 0 || object->getBodyModule()->getHealth()
		< 0.4f * object->getBodyModule()->getMaxHealth())
	{
		// The native jet AI owns the landing, parking reservation and ammunition refill.
		object->getAI()->aiIdle(CMD_FROM_AI); record.order = NONE; return true;
	}
	Object *target = nullptr;
	Real best = 0;
	for (size_t i = 0; i < m_visible.size(); ++i)
	{
		Object *candidate = m_visible[i];
		if (m_player->getRelationship(candidate->getTeam()) != ENEMIES
			|| (candidate->isKindOf(KINDOF_AIRCRAFT) && !named(object, "AmericaJetRaptor"))
			|| !airRouteSafe(*object->getPosition(), *candidate->getPosition())
			|| !airRouteSafe(*candidate->getPosition(), m_base)) continue;
		Int ready = 0;
		Real salvo = 0;
		for (size_t j = 0; j < m_owned.size(); ++j)
		{
			Object *plane = m_owned[j];
			if (!plane->getTemplate()->isEquivalentTo(object->getTemplate()) || !operational(plane) || !plane->getAI()) continue;
			Weapon *gun = plane->getCurrentWeapon();
			JetAIUpdate *otherJet = plane->getAI()->getJetAIUpdate();
			if (!gun || gun->getRemainingAmmo() <= 0 || !otherJet || otherJet->isReloading()) continue;
			++ready; salvo += gun->estimateWeaponDamage(plane, candidate) * gun->getRemainingAmmo();
		}
		if (ready < 2 || salvo < 1.1f * candidate->getBodyModule()->getHealth()) continue;
		Real value = candidate->getTemplate()->friend_getBuildCost();
		for (size_t j = 0; j < m_visible.size(); ++j)
			if (m_player->getRelationship(m_visible[j]->getTeam()) == ENEMIES
				&& distanceSquared(*candidate->getPosition(), *m_visible[j]->getPosition()) < sqr(60.0f))
				value += m_visible[j]->getTemplate()->friend_getBuildCost();
		for (size_t j = 0; j < m_owned.size(); ++j)
			if (!m_owned[j]->getContainedBy() && distanceSquared(*candidate->getPosition(), *m_owned[j]->getPosition()) < sqr(80.0f))
				value -= 3 * m_owned[j]->getTemplate()->calcCostToBuild(m_player);
		if (value > best) { best = value; target = candidate; }
	}
	if (target) move(object, *target->getPosition(), ATTACK, target);
	return true;
}

Bool CompetitiveSkirmishAI::objectAbility(Object *object, Object *target)
{
	if (!target || !canObserve(m_player, target)) return false;
	if (named(object, "AmericaInfantryColonelBurton") || named(object, "ChinaInfantryBlackLotus") || named(object, "GLAInfantryJarmenKell"))
	{
		for (ObservationMap::const_iterator it = m_observations.begin(); it != m_observations.end(); ++it)
			if (it->second.detector && it->second.confidence(TheGameLogic->getFrame()) > 0
				&& m_player->getRelationship(ThePlayerList->getNthPlayer(it->second.playerIndex)->getDefaultTeam()) == ENEMIES
				&& distanceSquared(it->second.position, *target->getPosition()) < sqr(250.0f)) return false;
	}
	const CommandSet *commands = TheControlBar->findCommandSet(object->getCommandSetString());
	if (!commands) return false;
	UnsignedInt frame = TheGameLogic->getFrame();
	UnitOrder& record = m_orders[object->getID()];
	if (record.order == ABILITY && frame - record.issued < 15 * LOGICFRAMES_PER_SECOND) return false;
	for (Int i = 0; i < MAX_COMMANDS_PER_SET; ++i)
	{
		const CommandButton *command = commands->getCommandButton(i);
		if (!command) continue;
		const AsciiString& name = command->getName();
		Bool use = false;
		if (contains(name, "missiledefenderlaserguided"))
			use = target->isKindOf(KINDOF_VEHICLE) && safe(*object->getPosition(), 65);
		if (contains(name, "jarmenkellsnipe"))
			use = target->isKindOf(KINDOF_VEHICLE) && safe(*object->getPosition(), 65);
		if (contains(name, "blacklotus") && contains(name, "capture"))
			use = target->isKindOf(KINDOF_STRUCTURE) && safe(*object->getPosition(), 150);
		if (contains(name, "burton") && contains(name, "timeddemo"))
			use = target->isKindOf(KINDOF_STRUCTURE) && safe(*object->getPosition(), 120);
		if (contains(name, "capturebuilding") && contains(target->getTemplate()->getName(), "oil"))
			use = safe(*target->getPosition(), 250);
		if (contains(name, "ecmdisable"))
		{
			for (size_t j = 0; j < m_owned.size(); ++j)
			{
				Object *escort = m_owned[j];
				if (escort != object && named(escort, "ChinaTankECM") && operational(escort)
					&& m_orders[escort->getID()].order != ABILITY
					&& distanceSquared(*object->getPosition(), *escort->getPosition()) < sqr(120.0f))
					use = target->isKindOf(KINDOF_VEHICLE);
			}
		}
		if (!use) continue;
		if (command->getCommandType() == GUI_COMMAND_SPECIAL_POWER)
		{
			const SpecialPowerTemplate *power = command->getSpecialPowerTemplate();
			if (!power || !TheActionManager->canDoSpecialPowerAtObject(object, target, CMD_FROM_PLAYER, power, command->getOptions())) continue;
			object->doSpecialPowerAtObject(power, target, command->getOptions());
		}
		else if (command->getCommandType() == GUI_COMMAND_FIRE_WEAPON)
		{
			if (!TheActionManager->canFireWeaponAtObject(object, target, CMD_FROM_PLAYER, command->getWeaponSlot())) continue;
			object->doCommandButtonAtObject(command, target, CMD_FROM_AI);
		}
		else continue;
		record.order = ABILITY; record.target = target->getID(); record.goal = *target->getPosition(); record.issued = frame;
		return true;
	}
	return false;
}

Coord3D CompetitiveSkirmishAI::economyRefuge(Object *object) const
{
	Coord3D refuge = m_base;
	Real nearest = HUGE_DIST * HUGE_DIST;
	for (size_t i = 0; i < m_owned.size(); ++i)
	{
		Object *site = m_owned[i];
		if (!(site->isKindOf(KINDOF_COMMANDCENTER) || site->isKindOf(KINDOF_FS_SUPPLY_CENTER))
			|| !operational(site) || !safe(*site->getPosition(), 300)) continue;
		Real distance = distanceSquared(*object->getPosition(), *site->getPosition());
		if (distance < nearest) { nearest = distance; refuge = *site->getPosition(); }
	}
	if (nearest < HUGE_DIST * HUGE_DIST) return refuge;
	// If our bases are under fire, escape the closest observed threat rather than returning into it.
	for (ObservationMap::const_iterator it = m_observations.begin(); it != m_observations.end(); ++it)
	{
		const CompetitiveAIObservation& threat = it->second;
		if (!threat.armed || !threat.hasPrecisePosition(TheGameLogic->getFrame())
			|| m_player->getRelationship(ThePlayerList->getNthPlayer(threat.playerIndex)->getDefaultTeam()) != ENEMIES) continue;
		Real distance = distanceSquared(*object->getPosition(), threat.position);
		if (distance >= nearest) continue;
		nearest = distance;
		Real separation = max(1.0f, sqrt(distance));
		Coord3D escape = *object->getPosition();
		escape.x += (escape.x - threat.position.x) * 200 / separation;
		escape.y += (escape.y - threat.position.y) * 200 / separation;
		if (safe(escape, 80)) refuge = escape;
	}
	return refuge;
}

void CompetitiveSkirmishAI::tactics()
{
	UnsignedInt frame = TheGameLogic->getFrame();
	if (frame >= m_nextScout)
	{
		++m_scoutIndex; m_nextScout = frame + 30 * LOGICFRAMES_PER_SECOND;
	}
	loadTransports();
	Int readyArmy = 0;
	for (size_t i = 0; i < m_owned.size(); ++i)
		if (m_owned[i]->isKindOf(KINDOF_VEHICLE) && !m_owned[i]->isKindOf(KINDOF_DOZER)
			&& !m_owned[i]->isKindOf(KINDOF_HARVESTER) && m_owned[i]->hasAnyDamageWeapon()
			&& operational(m_owned[i]) && transportReady(m_owned[i])) ++readyArmy;
	std::map<ObjectID, Real> committedDamage;
	Int combatIndex = 0;
	Bool assignedScout = false;
	for (size_t i = 0; i < m_owned.size(); ++i)
	{
		Object *object = m_owned[i];
		AIUpdateInterface *ai = object->getAI();
		if (!ai || !operational(object) || object->getContainedBy() || object->isKindOf(KINDOF_STRUCTURE)
			|| !object->isKindOf(KINDOF_SELECTABLE) || object->isKindOf(KINDOF_PROJECTILE)) continue;
		const AsciiString& name = object->getTemplate()->getName();
		// Attached drones retain their native follow, repair and detection controller.
		if (contains(name, "drone")) continue;
		UnitOrder& record = m_orders[object->getID()];
		Real health = object->getBodyModule()->getHealth();
		Real maxHealth = object->getBodyModule()->getMaxHealth();
		Real recentDamage = record.lastHealth > 0 ? max(0.0f, record.lastHealth - health) * 5 : 0;
		record.damagePerSecond = 0.5f * record.damagePerSecond + 0.5f * recentDamage;
		record.lastHealth = health;
		Bool dying = record.damagePerSecond > 0 && health / record.damagePerSecond <= 2;
		if (record.order == ATTACK || record.order == ABILITY)
		{
			Object *target = visibleTarget(record.target);
			if (!target)
			{
				// Abort tracking a target's object ID immediately when sight is lost.
				ai->aiIdle(CMD_FROM_AI); record.order = NONE; record.target = INVALID_ID;
			}
		}
		if (object->isKindOf(KINDOF_DOZER) || object->isKindOf(KINDOF_HARVESTER))
		{
			if (dying) move(object, economyRefuge(object), RETREAT);
			continue;
		}
		if (named(object, "ChinaInfantryHacker"))
		{
			if (dying) move(object, m_base, RETREAT);
			else if (record.order == ENTER && frame - record.issued < 10 * LOGICFRAMES_PER_SECOND && findOwned(record.target)) continue;
			else
			{
				Object *center = nullptr;
				for (size_t j = 0; j < m_owned.size(); ++j)
					if (named(m_owned[j], "ChinaInternetCenter") && operational(m_owned[j]) && safe(*m_owned[j]->getPosition(), 350)) center = m_owned[j];
				if (center && Int(object->getID()) % 3 != 0 && move(object, *center->getPosition(), ENTER, center)) continue;
				Coord3D home = m_base;
				Int place = Int(object->getID()) % 12;
				home.x += (place % 4 - 2) * 65; home.y += (place / 4 - 1) * 65;
				if (safe(home, 350) && distanceSquared(*object->getPosition(), home) > sqr(35.0f)) move(object, home, MOVE);
				else if (ai->isIdle()) ai->aiHackInternet(CMD_FROM_AI);
			}
			continue;
		}
		if (attackAircraft(object)) continue;
		if (record.order == ENTER && frame - record.issued < 10 * LOGICFRAMES_PER_SECOND
			&& findOwned(record.target) && !dying) continue;
		if (named(object, "GLAInfantryTerrorist") && record.target != INVALID_ID)
		{
			Object *target = visibleTarget(record.target);
			Bool friendlyRisk = false;
			for (size_t j = 0; target && j < m_owned.size(); ++j)
				if (!m_owned[j]->getContainedBy() && !named(m_owned[j], "GLAInfantryTerrorist")
					&& distanceSquared(*m_owned[j]->getPosition(), *target->getPosition()) < sqr(120.0f)) friendlyRisk = true;
			if (target && !friendlyRisk && safe(*target->getPosition(), 180)) move(object, *target->getPosition(), ATTACK, target);
			else move(object, m_base, RETREAT);
			continue;
		}
		if (bombTransport(object)) { record.role = RAID; continue; }
		Bool support = contains(name, "medic") || contains(name, "avenger") || contains(name, "ecm") || contains(name, "radarvan");
		Bool hero = contains(name, "burton") || contains(name, "blacklotus") || contains(name, "jarmenkell");
		if (!support && !hero && !object->hasAnyDamageWeapon()) continue;
		++combatIndex;
		record.role = support ? SUPPORT : (hero ? HERO : MAIN);
		if (contains(name, "tomahawk") || contains(name, "inferno") || contains(name, "nukelauncher") || contains(name, "rocketbuggy")) record.role = SIEGE;
		if (contains(name, "technical")) record.role = RAID;
		if (!assignedScout && (named(object, "AmericaInfantryRanger") || named(object, "ChinaVehicleListeningOutpost")
			|| named(object, "ChinaInfantryRedguard") || named(object, "GLAInfantryRebel")))
		{
			record.role = SCOUT; assignedScout = true;
		}
		else if (record.role == MAIN && combatIndex % 5 == 0 && !m_threat.baseAttacked) record.role = RESERVE;
		if (record.order == ENTER && frame - record.issued < 10 * LOGICFRAMES_PER_SECOND) continue;
		if (record.order == ABILITY && frame - record.issued < 15 * LOGICFRAMES_PER_SECOND && !dying) continue;
		if (!transportReady(object) && !m_threat.baseAttacked
			&& distanceSquared(*object->getPosition(), m_base) < sqr(400.0f))
		{
			move(object, m_base, MOVE); continue;
		}

		Object *best = nullptr;
		Real bestValue = -1;
		for (size_t j = 0; j < m_visible.size(); ++j)
		{
			Object *target = m_visible[j];
			if (record.role == RAID && !m_threat.baseAttacked && raidBlocked(*target->getPosition())) continue;
			Bool hostile = m_player->getRelationship(target->getTeam()) == ENEMIES;
			Bool oil = contains(target->getTemplate()->getName(), "oil");
			Bool abandoned = target->isDisabledByType(DISABLED_UNMANNED) && target->isKindOf(KINDOF_VEHICLE);
			if (!hostile && !oil && !abandoned) continue;
			Real dist = sqrt(distanceSquared(*object->getPosition(), *target->getPosition()));
			if (abandoned && object->isKindOf(KINDOF_INFANTRY) && dist < 150 && safe(*target->getPosition(), 180))
			{
				if (move(object, *target->getPosition(), ENTER, target)) break;
			}
			if ((record.role == SCOUT || named(object, "ChinaInfantryBlackLotus")) && oil && objectAbility(object, target)) break;
			if (!hostile || dist > 650) continue;
			if (!hero && object->getAbleToAttackSpecificObject(ATTACK_NEW_TARGET, target, CMD_FROM_PLAYER) == ATTACKRESULT_NOT_POSSIBLE) continue;
			Real value = target->isKindOf(KINDOF_FS_SUPERWEAPON) ? 100 : 10;
			if (target->isKindOf(KINDOF_HARVESTER) || target->isKindOf(KINDOF_DOZER)) value += 40;
			if (target->isKindOf(KINDOF_CASH_GENERATOR) || target->isKindOf(KINDOF_FS_SUPPLY_CENTER)) value += 30;
			if (target->isKindOf(KINDOF_REBUILD_HOLE)) value += 25;
			if (contains(target->getTemplate()->getName(), "ecm") || contains(target->getTemplate()->getName(), "inferno")
				|| contains(target->getTemplate()->getName(), "tomahawk")) value += 30;
			if (m_threat.baseAttacked && distanceSquared(*target->getPosition(), m_base) < sqr(350.0f)) value += 80;
			if (committedDamage[target->getID()] >= target->getBodyModule()->getHealth()) value *= 0.1f;
			value /= 1 + dist / 150;
			if (value > bestValue) { bestValue = value; best = target; }
		}
		if (record.order == ABILITY && frame - record.issued < 15 * LOGICFRAMES_PER_SECOND && !dying) continue;
		if (record.order == ENTER && frame - record.issued < 10 * LOGICFRAMES_PER_SECOND) continue;
		Real ours = best ? localStrength(*object->getPosition(), true, best) : 0;
		Real theirs = best ? localStrength(*object->getPosition(), false, object) : 0;
		Bool retreat = dying || health < maxHealth * 0.35f || CompetitiveAIPolicy::retreat(ours, theirs);
		Weapon *weapon = object->getCurrentWeapon();
		if (record.role == SIEGE && weapon && weapon->getStatus() == RELOADING_CLIP) retreat = true;
		if (retreat || (frame < record.retreatUntil && health < maxHealth * 0.8f))
		{
			record.retreatUntil = frame + 3 * LOGICFRAMES_PER_SECOND;
			if (dying && object->getContain() && object->getContain()->getContainCount() && safe(*object->getPosition(), 80))
				ai->aiEvacuate(false, CMD_FROM_AI);
			Object *repair = nullptr;
			for (size_t j = 0; j < m_owned.size(); ++j)
				if (named(m_owned[j], "AmericaVehicleMedic") && operational(m_owned[j]) && safe(*m_owned[j]->getPosition(), 180))
					repair = m_owned[j];
			if (m_player->getSide() == "GLA")
			{
				Real nearest = HUGE_DIST * HUGE_DIST;
				for (size_t j = 0; j < m_owned.size(); ++j)
					if (named(m_owned[j], "GLATunnelNetwork") && operational(m_owned[j]) && safe(*m_owned[j]->getPosition(), 180))
					{
						Real dist = distanceSquared(*object->getPosition(), *m_owned[j]->getPosition());
						if (dist < nearest) { nearest = dist; repair = m_owned[j]; }
					}
				if (repair && move(object, *repair->getPosition(), ENTER, repair)) continue;
			}
			move(object, repair ? *repair->getPosition() : m_base, RETREAT);
			continue;
		}
		if (best && objectAbility(object, best)) continue;
		if (record.role == SUPPORT && (!best || contains(name, "medic") || contains(name, "radarvan") || contains(name, "ecm")))
		{
			Coord3D goal = m_base;
			for (size_t j = 0; j < m_owned.size(); ++j)
			{
				Object *leader = m_owned[j];
				if (!(named(leader, "AmericaVehicleHumvee") || named(leader, "ChinaTankOverlord") || named(leader, "GLAVehicleRocketBuggy"))) continue;
				if (!leader->getContainedBy() && leader->getBodyModule()->getHealth() > leader->getBodyModule()->getMaxHealth() * 0.5f)
				{
					goal = *leader->getPosition();
					Real dist = max(1.0f, sqrt(distanceSquared(goal, m_base)));
					goal.x += (m_base.x - goal.x) * 25 / dist;
					goal.y += (m_base.y - goal.y) * 25 / dist;
					break;
				}
			}
			move(object, goal, MOVE); continue;
		}
		Coord3D retreatPoint = m_base;
		Real homeDistance = max(1.0f, sqrt(distanceSquared(*object->getPosition(), m_base)));
		retreatPoint.x += (object->getPosition()->x - m_base.x) * 170 / homeDistance;
		retreatPoint.y += (object->getPosition()->y - m_base.y) * 170 / homeDistance;
		Bool escape = TheAI->pathfinder()->clientSafeQuickDoesPathExist(ai->getLocomotorSet(), object->getPosition(), &retreatPoint);
		if (best && CompetitiveAIPolicy::attack(ours, theirs, true, escape))
		{
			Real range = object->getLargestWeaponRange();
			if (object->getContain())
			{
				const ContainedItemsList *contents = object->getContain()->getContainedItemsList();
				if (contents) for (ContainedItemsList::const_iterator it = contents->begin(); it != contents->end(); ++it)
					range = max(range, (*it)->getLargestWeaponRange());
			}
			Real dist = sqrt(distanceSquared(*object->getPosition(), *best->getPosition()));
			if ((record.role == SIEGE || named(object, "AmericaVehicleHumvee")) && dist < range * 0.7f)
			{
				Coord3D away = *object->getPosition();
				Real separation = max(1.0f, dist);
				away.x += (away.x - best->getPosition()->x) * 60 / separation;
				away.y += (away.y - best->getPosition()->y) * 60 / separation;
				if (safe(away, 80) && move(object, away, MOVE)) continue;
			}
			if (move(object, *best->getPosition(), ATTACK, best)) committedDamage[best->getID()] += damageRate(object, best, true) * 2;
			continue;
		}
		if (best)
		{
			// The intermediate strength band trades shots from safety instead of walking into the target.
			if (weapon && weapon->isWithinAttackRange(object, best))
				move(object, *best->getPosition(), ATTACK, best);
			else
			{
				Coord3D hold = *object->getPosition();
				Real dist = max(1.0f, sqrt(distanceSquared(hold, *best->getPosition())));
				hold.x += (hold.x - best->getPosition()->x) * 35 / dist;
				hold.y += (hold.y - best->getPosition()->y) * 35 / dist;
				move(object, hold, MOVE);
			}
			continue;
		}
		if (record.role == SCOUT)
		{
			Coord3D goal = scoutGoal();
			if (m_raidFailures >= 2 && distanceSquared(*object->getPosition(), m_raidFailedGoal) < sqr(120.0f)
				&& frame > m_raidFailedFrame + 30 * LOGICFRAMES_PER_SECOND
				&& ThePartitionManager->getShroudStatusForPlayer(m_player->getPlayerIndex(), &m_raidFailedGoal) == CELLSHROUD_CLEAR)
			{
				for (size_t route = 0; route < m_raidRoutes.size(); ++route)
					if (distanceSquared(m_raidFailedGoal, m_raidRoutes[route].position) < sqr(300.0f)) m_raidRoutes[route].failures = 0;
				m_raidFailures = 0;
			}
			move(object, goal, MOVE); continue;
		}
		if (record.role == RESERVE || (record.role == RAID && m_raidFailures >= 2
			&& (!m_hasGoal || raidBlocked(m_goal))))
		{
			move(object, m_base, MOVE); continue;
		}
		if (m_player->getSide() == "GLA" && object->isKindOf(KINDOF_VEHICLE)
			&& !support && !hero && health > 0.6f * maxHealth)
		{
			Bool collecting = false;
			for (size_t j = 0; j < m_visible.size(); ++j)
			{
				Object *scrap = m_visible[j];
				if (!contains(scrap->getTemplate()->getName(), "salvage") || !safe(*scrap->getPosition(), 220)
					|| distanceSquared(*object->getPosition(), *scrap->getPosition()) > sqr(180.0f)) continue;
				if (move(object, *scrap->getPosition(), MOVE)) collecting = true;
				break;
			}
			if (collecting) continue;
		}
		if (!m_threat.baseAttacked && record.role != SCOUT && record.role != RAID && readyArmy < 2)
		{
			move(object, m_base, MOVE); continue;
		}
		Coord3D goal = m_hasGoal ? m_goal : scoutGoal();
		// Formation offsets separate firestorm and artillery victims; supports remain behind the main group.
		Int lane = Int(object->getID()) % 3 - 1;
		goal.x += lane * 35;
		if (record.role == SIEGE || record.role == HERO)
		{
			Real dist = max(1.0f, sqrt(distanceSquared(goal, m_base)));
			goal.x += (m_base.x - goal.x) * 100 / dist;
			goal.y += (m_base.y - goal.y) * 100 / dist;
		}
		if (record.role == HERO)
		{
			Bool detectedRoute = false;
			for (ObservationMap::const_iterator it = m_observations.begin(); it != m_observations.end(); ++it)
				if (it->second.detector && it->second.confidence(frame) > 0
					&& m_player->getRelationship(ThePlayerList->getNthPlayer(it->second.playerIndex)->getDefaultTeam()) == ENEMIES
					&& distanceSquared(it->second.position, goal) < sqr(300.0f)) detectedRoute = true;
			if (detectedRoute) goal = m_base;
		}
		if (distanceSquared(record.progressPosition, *object->getPosition()) > sqr(15.0f))
		{
			record.progressPosition = *object->getPosition(); record.progressFrame = frame;
		}
		else if (frame - record.progressFrame > 10 * LOGICFRAMES_PER_SECOND && !best)
		{
			goal = scoutGoal(); ++m_scoutIndex; record.progressFrame = frame;
		}
		move(object, goal, MOVE);
	}
	tunnels();
}

void CompetitiveSkirmishAI::tunnels()
{
	if (m_player->getSide() != "GLA") return;
	UnsignedInt frame = TheGameLogic->getFrame();
	Coord3D goal = m_threat.baseAttacked ? m_base : (m_hasGoal ? m_goal : scoutGoal());
	Object *exit = nullptr;
	Real distance = HUGE_DIST * HUGE_DIST;
	for (size_t i = 0; i < m_owned.size(); ++i)
	{
		Object *tunnel = m_owned[i];
		if (!named(tunnel, "GLATunnelNetwork") || !operational(tunnel) || !safe(*tunnel->getPosition(), 180)) continue;
		Real dist = distanceSquared(*tunnel->getPosition(), goal);
		if (dist < distance) { distance = dist; exit = tunnel; }
	}
	if (!exit) return;
	for (size_t i = 0; i < m_owned.size(); ++i)
	{
		Object *unit = m_owned[i];
		if (!unit->getAI() || unit->getContainedBy() || unit->isKindOf(KINDOF_DOZER)
			|| unit->isKindOf(KINDOF_STRUCTURE) || !unit->hasAnyDamageWeapon()) continue;
		const UnitOrder& task = m_orders[unit->getID()];
		if (task.role == HERO || task.role == RAID || task.role == SCOUT || task.role == RESERVE
			|| (task.order == ABILITY && frame - task.issued < 15 * LOGICFRAMES_PER_SECOND)
			|| (task.order == ENTER && frame - task.issued < 10 * LOGICFRAMES_PER_SECOND)) continue;
		if (distanceSquared(*unit->getPosition(), *exit->getPosition()) < sqr(500.0f)) continue;
		for (size_t j = 0; j < m_owned.size(); ++j)
		{
			Object *entry = m_owned[j];
			if (entry == exit || !named(entry, "GLATunnelNetwork") || !operational(entry)) continue;
			if (distanceSquared(*unit->getPosition(), *entry->getPosition()) < sqr(150.0f))
				if (move(unit, *entry->getPosition(), ENTER, entry)) break;
		}
	}
	if (frame < m_nextTunnelExit || !exit->getContain() || !exit->getAI()) return;
	const ContainedItemsList *contents = exit->getContain()->getContainedItemsList();
	Bool ready = contents && !contents->empty();
	if (contents) for (ContainedItemsList::const_iterator it = contents->begin(); it != contents->end(); ++it)
	{
		if ((*it)->getBodyModule()->getHealth() < (*it)->getBodyModule()->getMaxHealth() * 0.75f) ready = false;
	}
	if (ready)
	{
		exit->getAI()->aiEvacuate(false, CMD_FROM_AI);
		m_nextTunnelExit = frame + 3 * LOGICFRAMES_PER_SECOND;
	}
}

Bool CompetitiveSkirmishAI::upgrade(Object *object, const CommandButton *command, Bool emergency)
{
	if (!command || !operational(object)) return false;
	const UpgradeTemplate *upgrade = command->getUpgradeTemplate();
	ProductionUpdateInterface *production = object->getProductionUpdateInterface();
	if (!upgrade || !production || production->getProductionCount() != 0
		|| production->canQueueUpgrade(upgrade) != CANMAKE_OK || !object->canProduceUpgrade(upgrade)) return false;
	Int reserve = emergency ? 0 : CompetitiveAIPolicy::reserve(counterCost());
	if (m_player->getMoney()->countMoney() < UnsignedInt(upgrade->calcCostToBuild(m_player) + reserve)) return false;
	return production->queueUpgrade(upgrade);
}

void CompetitiveSkirmishAI::buyScience(const char *name)
{
	ScienceType science = TheScienceStore->getScienceFromInternalName(name);
	if (m_player->isCapableOfPurchasingScience(science)) m_player->attemptToPurchaseScience(science);
}

Bool CompetitiveSkirmishAI::powerTarget(const SpecialPowerTemplate *power, Coord3D *position) const
{
	if (!power || !position) return false;
	Real radius = max(180.0f, power->getRadiusCursorRadius());
	Real best = 0;
	UnsignedInt frame = TheGameLogic->getFrame();
	for (ObservationMap::const_iterator candidate = m_observations.begin(); candidate != m_observations.end(); ++candidate)
	{
		const CompetitiveAIObservation& center = candidate->second;
		if (!center.hasPrecisePosition(frame)) continue;
		if (m_player->getRelationship(ThePlayerList->getNthPlayer(center.playerIndex)->getDefaultTeam()) != ENEMIES) continue;
		Real value = 0;
		for (ObservationMap::const_iterator it = m_observations.begin(); it != m_observations.end(); ++it)
		{
			const CompetitiveAIObservation& seen = it->second;
			if (!seen.hasPrecisePosition(frame) || seen.aircraft || distanceSquared(center.position, seen.position) > sqr(radius)) continue;
			if (m_player->getRelationship(ThePlayerList->getNthPlayer(seen.playerIndex)->getDefaultTeam()) != ENEMIES) continue;
			const ThingTemplate *thing = TheThingFactory->findTemplate(seen.templateName);
			if (thing) value += thing->friend_getBuildCost() * seen.confidence(frame);
		}
		for (size_t i = 0; i < m_owned.size(); ++i)
		{
			Object *own = m_owned[i];
			if (!own->getContainedBy() && distanceSquared(center.position, *own->getPosition()) < sqr(radius + 40))
				value -= own->getTemplate()->calcCostToBuild(m_player) * 3;
		}
		if (value > best) { best = value; *position = center.position; }
	}
	return best > 0;
}

void CompetitiveSkirmishAI::abilities()
{
	if (m_player->getSide() == "America")
	{
		buyScience("SCIENCE_SpyDrone");
		if (m_threat.infantry >= 4) buyScience("SCIENCE_Pathfinder");
		buyScience("SCIENCE_A10ThunderboltMissileStrike1");
		buyScience("SCIENCE_A10ThunderboltMissileStrike2");
		buyScience("SCIENCE_A10ThunderboltMissileStrike3");
		buyScience("SCIENCE_SpectreGunship1");
		buyScience("SCIENCE_DaisyCutter");
		if (m_threat.infantry >= 4) train("AmericaInfantryPathfinder", 2);
	}
	else if (m_player->getSide() == "China")
	{
		if (count("ChinaVehicleInfernoCannon", false)) buyScience("SCIENCE_ArtilleryTraining");
		buyScience("SCIENCE_ChinaCarpetBomb");
		buyScience("SCIENCE_ArtilleryBarrage1");
		buyScience("SCIENCE_ArtilleryBarrage2");
		buyScience("SCIENCE_ArtilleryBarrage3");
		buyScience("SCIENCE_EMPPulse");
	}
	else
	{
		if (count("GLAVehicleTechnical", false)) buyScience("SCIENCE_TechnicalTraining");
		buyScience("SCIENCE_CashBounty1");
		buyScience("SCIENCE_CashBounty2");
		buyScience("SCIENCE_CashBounty3");
		if (m_hasGoal && count("GLATunnelNetwork", false) >= 2 && safe(m_goal, 180)) buyScience("SCIENCE_SneakAttack");
		buyScience("SCIENCE_GPSScrambler");
		buyScience("SCIENCE_AnthraxBomb");
	}
	Bool oil = false;
	for (size_t i = 0; i < m_visible.size(); ++i)
		if (contains(m_visible[i]->getTemplate()->getName(), "oil") && safe(*m_visible[i]->getPosition(), 250)) oil = true;
	for (size_t i = 0; i < m_owned.size(); ++i)
	{
		Object *object = m_owned[i];
		if (!operational(object)) continue;
		const CommandSet *commands = TheControlBar->findCommandSet(object->getCommandSetString());
		if (!commands) continue;
		for (Int j = 0; j < MAX_COMMANDS_PER_SET; ++j)
		{
			const CommandButton *command = commands->getCommandButton(j);
			if (!command) continue;
			if (command->getCommandType() == GUI_COMMAND_OBJECT_UPGRADE || command->getCommandType() == GUI_COMMAND_PLAYER_UPGRADE)
			{
				const UpgradeTemplate *upgradeT = command->getUpgradeTemplate();
				if (!upgradeT) continue;
				const AsciiString& name = upgradeT->getUpgradeName();
				Bool wanted = false;
				Bool emergency = contains(name, "controlrods") && m_player->getEnergy()->getProduction() - m_player->getEnergy()->getConsumption() < 4;
				if (emergency) wanted = true;
				if (contains(name, "towmissile")) wanted = count("AmericaVehicleHumvee", false) >= 2;
				if (contains(name, "flashbang")) wanted = m_threat.infantry >= 2;
				if (contains(name, "supplylines")) wanted = m_player->getMoney()->getCashPerMinute() >= 1500;
				if (contains(name, "scoutdrone")) wanted = named(object, "AmericaVehicleHumvee") && !count("AmericaVehicleScoutDrone");
				if (contains(name, "battledrone")) wanted = named(object, "AmericaVehicleHumvee") && count("AmericaVehicleScoutDrone", false) > 0;
				if (contains(name, "chainguns")) wanted = count("ChinaTankGattling", false) >= 4;
				if (contains(name, "subliminal")) wanted = count("ChinaTankOverlord", false) > 0;
				if (contains(name, "nationalism")) wanted = count("ChinaInfantryTankHunter", false) >= 5 || count("ChinaTankBattleMaster", false) >= 5;
				if (contains(name, "blacknapalm")) wanted = count("ChinaVehicleInfernoCannon", false) >= 3 || count("ChinaJetMIG", false) >= 4;
				if (contains(name, "migarmor")) wanted = count("ChinaJetMIG", false) >= 2;
				if (contains(name, "overlordpropagandatower"))
				{
					wanted = true;
					for (size_t k = 0; k < m_owned.size(); ++k)
					{
						ProductionUpdateInterface *production = m_owned[k]->getProductionUpdateInterface();
						if (m_owned[k]->hasUpgrade(upgradeT) || (production && production->isUpgradeInQueue(upgradeT))) wanted = false;
					}
				}
				if (contains(name, "overlordgattlingcannon")) wanted = m_threat.air > 0 && count("ChinaTankOverlord", false) >= 2;
				if (contains(name, "helixbattlebunker")) wanted = count("ChinaVehicleHelix", false) > 0;
				if (contains(name, "scorpionrocket")) wanted = count("GLATankScorpion", false) >= 2;
				if (contains(name, "buggyammo")) wanted = count("GLAVehicleRocketBuggy", false) >= 3;
				if (contains(name, "apbullets")) wanted = count("GLAVehicleQuadCannon", false) >= 4;
				if (contains(name, "aprockets")) wanted = count("GLAVehicleRocketBuggy", false) >= 3;
				if (contains(name, "junkrepair")) wanted = count("GLAVehicleRocketBuggy", false) + count("GLATankScorpion", false) + count("GLAVehicleQuadCannon", false) >= 8;
				if (contains(name, "workershoes")) wanted = count("GLAInfantryWorker", false) >= 12;
				if (contains(name, "radarvanscan")) wanted = count("GLAVehicleRadarVan", false) > 0;
				if (contains(name, "infantrycapture")) wanted = oil;
				if (wanted && upgrade(object, command, emergency)) break;
			}
			if (command->getCommandType() != GUI_COMMAND_SPECIAL_POWER) continue;
			const SpecialPowerTemplate *power = command->getSpecialPowerTemplate();
			if (!power) continue;
			SpecialPowerModuleInterface *module = object->getSpecialPowerModule(power);
			if (!module || !module->isReady()) continue;
			const AsciiString& name = command->getName();
			if (contains(name, "searchanddestroy") && m_battlePlanCenter != object->getID())
			{
				if (TheActionManager->canDoSpecialPower(object, power, CMD_FROM_PLAYER, command->getOptions()))
				{
					object->doSpecialPower(power, command->getOptions()); m_battlePlanCenter = object->getID();
				}
				continue;
			}
			Coord3D target;
			Bool reconnaissance = contains(name, "spydrone") || contains(name, "spysatellite") || contains(name, "radarvanscan");
			if (reconnaissance) target = m_hasGoal ? m_goal : scoutGoal();
			else if (contains(name, "gps") || contains(name, "emergencyrepair")) target = m_base;
			else
			{
				// Unit abilities are scheduled by their own controller, not as global bombardments.
				if (!object->isKindOf(KINDOF_COMMANDCENTER) && !object->isKindOf(KINDOF_FS_SUPERWEAPON)) continue;
				if (contains(name, "sneakattack"))
				{
					if (!m_hasGoal || !safe(m_goal, 180)) continue;
					target = m_goal;
				}
				else if (!powerTarget(power, &target)) continue;
				if ((contains(name, "a10") || contains(name, "carpet") || contains(name, "spectre") || contains(name, "daisycutter"))
					&& !airRouteSafe(m_base, target)) continue;
			}
			if (!TheActionManager->canDoSpecialPowerAtLocation(object, &target, CMD_FROM_PLAYER, power, nullptr, command->getOptions())) continue;
			object->doSpecialPowerAtLocation(power, &target, 0, command->getOptions());
			break;
		}
	}
}

void CompetitiveSkirmishAI::update()
{
	if (TheGameLogic->getFrame() < 2 || !supportsSide(m_player->getSide())) return;
	UnsignedInt frame = TheGameLogic->getFrame();
	if (frame < m_nextTactics) return;
	observe();
	if (!m_baseSet) return;
	if (TheGlobalData->m_headless && frame % (60 * LOGICFRAMES_PER_SECOND) < UnsignedInt(max(1, LOGICFRAMES_PER_SECOND / 5)))
	{
		Int combat = 0, waiting = 0, loaded = 0;
		for (size_t i = 0; i < m_owned.size(); ++i)
		{
			Object *object = m_owned[i];
			if (object->isKindOf(KINDOF_SELECTABLE) && !object->isKindOf(KINDOF_PROJECTILE)
				&& !object->isKindOf(KINDOF_STRUCTURE) && !object->isKindOf(KINDOF_HARVESTER)
				&& !object->isKindOf(KINDOF_DOZER) && object->hasAnyDamageWeapon()) ++combat;
			if (object->getProductionUpdateInterface() && object->getProductionUpdateInterface()->getProductionCount()) ++waiting;
			if ((named(object, "AmericaVehicleHumvee") || named(object, "GLAVehicleBattleBus") || named(object, "ChinaVehicleHelix"))
				&& transportReady(object)) ++loaded;
		}
		printf("CompetitiveAI frame=%u player=%d side=%s state=%d cash=%u income=%u owned=%u combat=%d supplies=%d builders=%d queues=%d sightings=%u loaded=%d tech=%d unitsLost=%d unitsKilled=%d earned=%d\n",
			frame, m_player->getPlayerIndex(), m_player->getSide().str(), Int(m_state), m_player->getMoney()->countMoney(),
			m_player->getMoney()->getCashPerMinute(), UnsignedInt(m_owned.size()), combat,
			count(profile().supply, false), count(profile().builder, false), waiting, UnsignedInt(m_observations.size()), loaded,
			count(profile().tech, false), m_player->getScoreKeeper()->getTotalUnitsLost(),
			m_player->getScoreKeeper()->getTotalUnitsDestroyed(), m_player->getScoreKeeper()->getTotalMoneyEarned());
		fflush(stdout);
	}
	if (frame >= m_nextStrategy) { strategy(); m_nextStrategy = frame + 3 * LOGICFRAMES_PER_SECOND; }
	if (frame >= m_nextAbilities) { abilities(); m_nextAbilities = frame + LOGICFRAMES_PER_SECOND; }
	if (frame >= m_nextEconomy) { economy(); m_nextEconomy = frame + LOGICFRAMES_PER_SECOND; }
	tactics();
	m_nextTactics = frame + max(1, LOGICFRAMES_PER_SECOND / 5);
}

void CompetitiveSkirmishAI::xfer(Xfer *xfer)
{
	XferVersion version = 1;
	xfer->xferVersion(&version, 1);
	xfer->xferCoord3D(&m_base); xfer->xferCoord3D(&m_goal);
	xfer->xferBool(&m_baseSet); xfer->xferBool(&m_hasGoal);
	xfer->xferUser(&m_state, sizeof(m_state));
	if (m_state < COMPETITIVE_OPENING || m_state > COMPETITIVE_FINISHING) RELEASE_CRASH("Invalid competitive AI state");
	xfer->xferUnsignedInt(&m_changed);
	xfer->xferUnsignedInt(&m_nextEconomy); xfer->xferUnsignedInt(&m_nextStrategy);
	xfer->xferUnsignedInt(&m_nextTactics); xfer->xferUnsignedInt(&m_nextAbilities);
	xfer->xferUnsignedInt(&m_lastProgress); xfer->xferUnsignedInt(&m_nextScout);
	xfer->xferUnsignedInt(&m_nextTunnelExit); xfer->xferObjectID(&m_battlePlanCenter);
	xfer->xferInt(&m_scoutIndex); xfer->xferInt(&m_raidFailures);
	xfer->xferUnsignedInt(&m_raidFailedFrame); xfer->xferCoord3D(&m_raidFailedGoal); xfer->xferReal(&m_goalHealth);
	xfer->xferObjectID(&m_raidTarget); xfer->xferInt(&m_raidPayload);
	xfer->xferInt(&m_collectorQuota); xfer->xferUnsignedInt(&m_nextCollectorAdjustment);
	UnsignedInt markers = m_supplyMarkers.size();
	xfer->xferUnsignedInt(&markers);
	if (markers > 4096) RELEASE_CRASH("Invalid competitive AI supply marker count");
	if (xfer->getXferMode() == XFER_LOAD) m_supplyMarkers.resize(markers);
	for (UnsignedInt marker = 0; marker < markers; ++marker) xfer->xferCoord3D(&m_supplyMarkers[marker]);
	UnsignedInt routes = m_raidRoutes.size();
	xfer->xferUnsignedInt(&routes);
	if (routes > 64) RELEASE_CRASH("Invalid competitive AI raid route count");
	if (xfer->getXferMode() == XFER_LOAD) m_raidRoutes.resize(routes);
	for (UnsignedInt route = 0; route < routes; ++route)
	{
		xfer->xferCoord3D(&m_raidRoutes[route].position); xfer->xferInt(&m_raidRoutes[route].failures);
		xfer->xferUnsignedInt(&m_raidRoutes[route].failedFrame);
	}
	UnsignedInt count = m_observations.size();
	xfer->xferUnsignedInt(&count);
	if (count > 512) RELEASE_CRASH("Invalid competitive AI observation count");
	if (xfer->getXferMode() == XFER_LOAD) m_observations.clear();
	ObservationMap::iterator it = m_observations.begin();
	for (UnsignedInt i = 0; i < count; ++i)
	{
		CompetitiveAIObservation seen;
		if (xfer->getXferMode() != XFER_LOAD) seen = (it++)->second;
		xfer->xferObjectID(&seen.id); xfer->xferAsciiString(&seen.templateName);
		xfer->xferCoord3D(&seen.position); xfer->xferUnsignedInt(&seen.lastSeen);
		xfer->xferReal(&seen.health); xfer->xferInt(&seen.playerIndex);
		xfer->xferBool(&seen.structure); xfer->xferBool(&seen.aircraft); xfer->xferBool(&seen.infantry);
		xfer->xferBool(&seen.vehicle); xfer->xferBool(&seen.economy); xfer->xferBool(&seen.defense);
		xfer->xferBool(&seen.superweapon); xfer->xferBool(&seen.armed);
		xfer->xferBool(&seen.antiAir); xfer->xferBool(&seen.detector);
		xfer->xferUnsignedInt(&seen.publicReadyFrame);
		if (xfer->getXferMode() == XFER_LOAD)
		{
			if (seen.playerIndex < 0 || seen.playerIndex >= MAX_PLAYER_COUNT) RELEASE_CRASH("Invalid competitive AI player");
			m_observations[seen.id] = seen;
		}
	}
	count = m_orders.size();
	xfer->xferUnsignedInt(&count);
	if (count > 16384) RELEASE_CRASH("Invalid competitive AI order count");
	if (xfer->getXferMode() == XFER_LOAD) m_orders.clear();
	OrderMap::iterator order = m_orders.begin();
	for (UnsignedInt i = 0; i < count; ++i)
	{
		ObjectID id = INVALID_ID;
		UnitOrder unit;
		if (xfer->getXferMode() != XFER_LOAD) { id = order->first; unit = order->second; ++order; }
		xfer->xferObjectID(&id); xfer->xferUser(&unit.role, sizeof(unit.role)); xfer->xferUser(&unit.order, sizeof(unit.order));
		xfer->xferObjectID(&unit.target); xfer->xferCoord3D(&unit.goal); xfer->xferCoord3D(&unit.progressPosition);
		xfer->xferUnsignedInt(&unit.issued); xfer->xferUnsignedInt(&unit.progressFrame);
		xfer->xferUnsignedInt(&unit.retreatUntil); xfer->xferReal(&unit.lastHealth); xfer->xferReal(&unit.damagePerSecond);
		xfer->xferInt(&unit.supplyBoxes); xfer->xferUnsignedInt(&unit.delivered);
		xfer->xferUnsignedInt(&unit.cycleFrames); xfer->xferUnsignedInt(&unit.waitingFrames);
		xfer->xferObjectID(&unit.raidTarget); xfer->xferCoord3D(&unit.raidGoal); xfer->xferInt(&unit.raidCost);
		xfer->xferReal(&unit.raidGain); xfer->xferReal(&unit.raidHealth);
		xfer->xferReal(&unit.raidMaxHealth); xfer->xferReal(&unit.raidValue);
		if (xfer->getXferMode() == XFER_LOAD)
		{
			if (unit.role < MAIN || unit.role > HERO || unit.order < NONE || unit.order > RETREAT)
				RELEASE_CRASH("Invalid competitive AI order");
			m_orders[id] = unit;
		}
	}
	if (xfer->getXferMode() == XFER_LOAD) { m_owned.clear(); m_visible.clear(); m_threat = Threat(); }
}

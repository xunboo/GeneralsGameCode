/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2025 Electronic Arts Inc.
** SPDX-License-Identifier: GPL-3.0-or-later
*/

#pragma once

#include "Common/GameMemory.h"
#include "Common/GameCommon.h"
#include "Common/GameType.h"
#include "Common/AsciiString.h"
#include <map>
#include <vector>

class Player;
class Object;
class ThingTemplate;
class CommandButton;
class SpecialPowerTemplate;
class Xfer;

// TheSuperHackers @feature Fair, opt-in skirmish strategy. No enemy pointers survive an observation.
enum CompetitiveAIState
{
	COMPETITIVE_OPENING, COMPETITIVE_DEFENDING, COMPETITIVE_PRESSURING,
	COMPETITIVE_TECHING, COMPETITIVE_RECOVERING, COMPETITIVE_FINISHING
};

struct CompetitiveAIObservation
{
	ObjectID id;
	AsciiString templateName;
	Coord3D position;
	UnsignedInt lastSeen;
	Real health;
	Int playerIndex;
	Bool structure;
	Bool aircraft;
	Bool infantry;
	Bool vehicle;
	Bool economy;
	Bool defense;
	Bool superweapon;
	Bool armed;
	Bool antiAir;
	Bool detector;
	UnsignedInt publicReadyFrame;

	CompetitiveAIObservation();
	void observe(const CompetitiveAIObservation& observation, Bool visible);
	Real confidence(UnsignedInt frame) const;
	Bool hasPrecisePosition(UnsignedInt frame) const;
};

struct CompetitiveAIPolicy
{
	static Int reserve(Int counterCost);
	static Bool attack(Real ours, Real theirs, Bool scouted, Bool escape);
	static Bool retreat(Real ours, Real theirs);
	static Bool changeState(UnsignedInt frame, UnsignedInt changed, Bool emergency);
};

class CompetitiveSkirmishAI
{
public:
	explicit CompetitiveSkirmishAI(Player *player);
	static Bool supportsSide(const AsciiString& side);
	static Bool canObserve(const Player *viewer, const Object *object);
	void reset();
	void update();
	void observePublicSuperweapon(const Object *object, UnsignedInt readyFrame);
	void xfer(Xfer *xfer);
	Bool powerTarget(const SpecialPowerTemplate *power, Coord3D *position) const;
	CompetitiveAIState getState() const { return m_state; }

private:
	friend class CompetitiveAITestAccess;
	enum Role { MAIN, SCOUT, RAID, RESERVE, SIEGE, SUPPORT, HERO };
	enum Order { NONE, MOVE, ATTACK, ENTER, ABILITY, RETREAT };
	struct UnitOrder
	{
		Role role;
		Order order;
		ObjectID target;
		Coord3D goal;
		Coord3D progressPosition;
		UnsignedInt issued;
		UnsignedInt progressFrame;
		UnsignedInt retreatUntil;
		Real lastHealth;
		Real damagePerSecond;
		Int supplyBoxes;
		UnsignedInt delivered, cycleFrames, waitingFrames;
		ObjectID raidTarget;
		Coord3D raidGoal;
		Int raidCost;
		Real raidGain, raidHealth, raidMaxHealth, raidValue;
		UnitOrder();
	};
	struct Threat
	{
		Int air, infantry, vehicles, defenses, antiAir;
		Bool baseAttacked, explored;
		Threat();
	};
	struct RaidRoute
	{
		Coord3D position;
		Int failures;
		UnsignedInt failedFrame;
		RaidRoute() : failures(0), failedFrame(0) { position.zero(); }
	};
	struct Profile;
	typedef std::map<ObjectID, CompetitiveAIObservation> ObservationMap;
	typedef std::map<ObjectID, UnitOrder> OrderMap;

	const Profile& profile() const;
	void observe();
	void strategy();
	void economy();
	void tactics();
	void abilities();
	Int count(const char *name, Bool queued = true) const;
	Object *findOwned(ObjectID id) const;
	Object *visibleTarget(ObjectID id) const;
	Object *builder(const ThingTemplate *thing, const Coord3D& position) const;
	Bool build(const char *name, const Coord3D& anchor, Bool emergency, Int followupCost = 0);
	Bool train(const char *name, Int desired, Bool emergency = true);
	Bool upgrade(Object *object, const CommandButton *button, Bool emergency = false);
	Int price(const char *name) const;
	Int counterCost() const;
	const CommandButton *button(Object *object, const char *name) const;
	Real damageRate(Object *source, Object *target, Bool passengers) const;
	Real templateDamageRate(const ThingTemplate *thing, Object *target, const Coord3D& position) const;
	Real localStrength(const Coord3D& position, Bool ours, Object *against) const;
	Bool safe(const Coord3D& position, Real radius) const;
	Bool airRouteSafe(const Coord3D& from, const Coord3D& to) const;
	Bool transportReady(Object *object) const;
	Bool attackAircraft(Object *object);
	void planRaid();
	void recordRaid(Object *unit, Object *target);
	Bool raidBlocked(const Coord3D& position) const;
	Bool bombTransport(Object *object);
	Bool move(Object *object, const Coord3D& position, Order order, Object *target = nullptr);
	Bool objectAbility(Object *object, Object *target);
	Coord3D scoutGoal();
	Coord3D supplyGoal(Bool expansion) const;
	Bool supplyAvailable(const Coord3D& position) const;
	Coord3D economyRefuge(Object *object) const;
	void loadTransports();
	void tunnels();
	void buyScience(const char *name);

	Player *m_player;
	Coord3D m_base;
	Coord3D m_goal;
	Bool m_baseSet;
	Bool m_hasGoal;
	CompetitiveAIState m_state;
	UnsignedInt m_changed;
	UnsignedInt m_nextEconomy, m_nextStrategy, m_nextTactics, m_nextAbilities;
	UnsignedInt m_lastProgress, m_nextScout, m_nextTunnelExit;
	ObjectID m_battlePlanCenter;
	Int m_scoutIndex;
	Int m_raidFailures;
	UnsignedInt m_raidFailedFrame;
	Coord3D m_raidFailedGoal;
	Real m_goalHealth;
	ObjectID m_raidTarget;
	Int m_raidPayload;
	Int m_collectorQuota;
	UnsignedInt m_nextCollectorAdjustment;
	Threat m_threat;
	ObservationMap m_observations;
	OrderMap m_orders;
	std::vector<Coord3D> m_supplyMarkers;
	std::vector<RaidRoute> m_raidRoutes;
	std::vector<Object*> m_owned;
	std::vector<Object*> m_visible;
};

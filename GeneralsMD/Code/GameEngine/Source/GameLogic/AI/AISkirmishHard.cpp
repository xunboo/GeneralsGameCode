/*
**	Command & Conquer Generals(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// Skirmish AI on hard difficulty: a wider base.
//
// On top of the fixed build list of its side, the hard skirmish AI keeps building, as long as there is money and room:
//   power when it runs short, supply centers next to the supply sources, income buildings (black markets, supply drop zones, internet centers),
//   defence structures around the base and around its supply centers, and more war factories, airfields and barracks.
// The targets are settings (GameData.ini, AIHard...). Buildings are recognised by their KindOf flags, never by name, so every faction and
// mod works. The new buildings are put on the player's priority build list; the stock base building code lets a dozer build them.
//
// Army (processHardArmy): idle factories keep producing the most expensive combat units they can afford; those that stand around the base
// leave together once enough of them have gathered, so attacks are rarer but bigger.
//
// Deterministic: the AI is part of the game logic, so this only uses game state, the object list order, name order and fixed search patterns.
// No random numbers and nothing is stored (the step is chosen from the frame number), so saved games are not affected.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/BuildAssistant.h"
#include "Common/GlobalData.h"
#include "Common/KindOf.h"
#include "Common/Player.h"
#include "Common/ThingFactory.h"
#include "Common/ThingTemplate.h"

#include "GameClient/TerrainVisual.h"

#include "GameLogic/AI.h"
#include "GameLogic/AIPathfind.h"
#include "GameLogic/AISkirmishPlayer.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/SidesList.h"
#include "GameLogic/Module/AIUpdate.h"
#include "GameLogic/Weapon.h"
#include "GameLogic/Module/ProductionUpdate.h"
#include "GameLogic/Object.h"
#include "GameLogic/TerrainLogic.h"

#include <algorithm>
#include <vector>

namespace
{

struct Census
{
	Int warFactories, airfields, barracks, blackMarkets, dropZones, internetCenters, supplyCenters, powerPlants, baseDefenses;
	Int dozers;
	Bool powerUnderConstruction;
	Object *anyDozer;
	std::vector<Coord3D> supplyCenterPos;

	// footprints of the structures, to leave room between buildings
	std::vector<Coord3D> structPos;
	std::vector<Real> structRadius;

	Census() : warFactories(0), airfields(0), barracks(0), blackMarkets(0), dropZones(0), internetCenters(0), supplyCenters(0),
		powerPlants(0), baseDefenses(0), dozers(0), powerUnderConstruction(false), anyDozer(nullptr) {}
};

enum ExpansionKind
{
	EXP_WAR_FACTORY,
	EXP_AIRFIELD,
	EXP_BARRACKS,
	EXP_BLACK_MARKET,
	EXP_DROP_ZONE,
	EXP_INTERNET_CENTER,
	EXP_SUPPLY_CENTER,
	EXP_POWER,
	EXP_COUNT
};

KindOfType kindFor( Int kind )
{
	switch (kind)
	{
		case EXP_WAR_FACTORY:		return KINDOF_FS_WARFACTORY;
		case EXP_AIRFIELD:			return KINDOF_FS_AIRFIELD;
		case EXP_BARRACKS:			return KINDOF_FS_BARRACKS;
		case EXP_BLACK_MARKET:		return KINDOF_FS_BLACK_MARKET;
		case EXP_DROP_ZONE:			return KINDOF_FS_SUPPLY_DROPZONE;
		case EXP_INTERNET_CENTER:	return KINDOF_FS_INTERNET_CENTER;
		case EXP_SUPPLY_CENTER:		return KINDOF_FS_SUPPLY_CENTER;
		default:					return KINDOF_FS_POWER;
	}
}

/// Which expansion kind (if any) a structure template counts as.
Int classify( const ThingTemplate *t )
{
	if (!t->isKindOf( KINDOF_STRUCTURE ) || t->isKindOf( KINDOF_FS_FAKE ))
		return -1;
	if (t->isKindOf( KINDOF_FS_WARFACTORY ))		return EXP_WAR_FACTORY;
	if (t->isKindOf( KINDOF_FS_AIRFIELD ))			return EXP_AIRFIELD;
	if (t->isKindOf( KINDOF_FS_BARRACKS ))			return EXP_BARRACKS;
	if (t->isKindOf( KINDOF_FS_BLACK_MARKET ))		return EXP_BLACK_MARKET;
	if (t->isKindOf( KINDOF_FS_SUPPLY_DROPZONE ))	return EXP_DROP_ZONE;
	if (t->isKindOf( KINDOF_FS_INTERNET_CENTER ))	return EXP_INTERNET_CENTER;
	if (t->isKindOf( KINDOF_FS_SUPPLY_CENTER ))		return EXP_SUPPLY_CENTER;
	if (t->isKindOf( KINDOF_FS_POWER ) && !t->isKindOf( KINDOF_CASH_GENERATOR ))	return EXP_POWER;
	return -1;
}

void countKind( Census &c, Int kind, Int amount )
{
	switch (kind)
	{
		case EXP_WAR_FACTORY:		c.warFactories += amount; break;
		case EXP_AIRFIELD:			c.airfields += amount; break;
		case EXP_BARRACKS:			c.barracks += amount; break;
		case EXP_BLACK_MARKET:		c.blackMarkets += amount; break;
		case EXP_DROP_ZONE:			c.dropZones += amount; break;
		case EXP_INTERNET_CENTER:	c.internetCenters += amount; break;
		case EXP_SUPPLY_CENTER:		c.supplyCenters += amount; break;
		case EXP_POWER:				c.powerPlants += amount; break;
		default: break;
	}
}

}	// namespace

//-------------------------------------------------------------------------------------------------
/**
 * One expansion step for the hard skirmish AI. Called every frame from doBaseBuilding, acts every few seconds.
 */
void AISkirmishPlayer::processHardExpansion()
{
	if (!TheGlobalData->m_aiHardEnabled || m_difficulty != DIFFICULTY_HARD || !m_player->getCanBuildBase())
		return;

	const UnsignedInt frame = TheGameLogic->getFrame();
	const UnsignedInt period = (UnsignedInt)std::max( 1, TheGlobalData->m_aiHardIntervalSeconds ) * LOGICFRAMES_PER_SECOND;
	if (frame < (UnsignedInt)TheGlobalData->m_aiHardStartSeconds * LOGICFRAMES_PER_SECOND)
		return;
	if ((frame + (UnsignedInt)m_player->getPlayerIndex() * 13u) % period != 0)
		return;	// every AI player acts on its own frame

	// ---- what the player owns ----
	Census census;
	for (Object *obj = TheGameLogic->getFirstObject(); obj; obj = obj->getNextObject())
	{
		if (obj->getControllingPlayer() != m_player || obj->isEffectivelyDead())
			continue;
		if (obj->isKindOf( KINDOF_DOZER ))
		{
			++census.dozers;
			if (census.anyDozer == nullptr)
				census.anyDozer = obj;
			continue;
		}
		if (!obj->isKindOf( KINDOF_STRUCTURE ) || obj->isKindOf( KINDOF_REBUILD_HOLE ))
			continue;

		census.structPos.push_back( *obj->getPosition() );
		census.structRadius.push_back( obj->getGeometryInfo().getBoundingCircleRadius() );

		if (obj->isKindOf( KINDOF_FS_BASE_DEFENSE ))
			++census.baseDefenses;
		const Int kind = classify( obj->getTemplate() );
		if (kind >= 0)
		{
			countKind( census, kind, 1 );
			if (kind == EXP_SUPPLY_CENTER)
				census.supplyCenterPos.push_back( *obj->getPosition() );
			if (kind == EXP_POWER && obj->getStatusBits().test( OBJECT_STATUS_UNDER_CONSTRUCTION ))
				census.powerUnderConstruction = true;
		}
	}

	// buildings that are queued but not standing yet count as well
	Int pendingPriority = 0;
	for (BuildListInfo *info = m_player->getBuildList(); info; info = info->getNext())
	{
		if (info->getObjectID() != INVALID_ID || !info->isBuildable())
			continue;
		const ThingTemplate *t = TheThingFactory->findTemplate( info->getTemplateName() );
		if (t == nullptr)
			continue;
		if (info->isPriorityBuild())
			++pendingPriority;
		const Int kind = classify( t );
		if (kind >= 0)
			countKind( census, kind, 1 );
		if (t->isKindOf( KINDOF_FS_BASE_DEFENSE ))
			++census.baseDefenses;
	}

	// ---- dozers: without enough of them nothing gets built ----
	const Int money = m_player->getMoney()->countMoney();
	if (census.anyDozer == nullptr)
	{
		queueDozer();
		return;
	}
	if (census.dozers < 3 && money > 3 * TheGlobalData->m_aiHardMoneyReserve)
		queueDozer();

	// do not pile up orders: a few waiting buildings at a time
	if (pendingPriority >= 2)
		return;

	// ---- placement ----
	const Real baseRadius = std::max( m_baseRadius, 150.0f );

	// A free spot on rings around 'center' (inner to outer, at every ring a fixed angle pattern), with room around the building.
	struct Placer
	{
		AISkirmishPlayer *ai;
		Census &census;
		Bool find( const ThingTemplate *t, const Coord3D &center, Real minR, Real maxR, Int rotateSteps, Coord3D &out, Real &angleOut )
		{
			const Real radius = t->getTemplateGeometryInfo().getBoundingCircleRadius();
			const Real placeAngle = t->getPlacementViewAngle();
			Bool found = false;
			for (Real r = minR; r <= maxR && !found; r += 2.0f * PATHFIND_CELL_SIZE_F)
			{
				const Real step = std::max( 0.1f, (2.0f * radius + 30.0f) / r );
				const Int count = std::min( 64, (Int)(2.0f * PI / step) );
				for (Int a = 0; a < count && !found; ++a)
				{
					const Real ang = (Real)((a + rotateSteps) % count) * (2.0f * PI / (Real)count);
					Coord3D p = center;
					p.x += r * cos( ang );
					p.y += r * sin( ang );
					p.z = TheTerrainLogic->getGroundHeight( p.x, p.y );

					Bool roomAround = true;
					for (size_t s = 0; s < census.structPos.size() && roomAround; ++s)
					{
						const Real dx = census.structPos[s].x - p.x;
						const Real dy = census.structPos[s].y - p.y;
						const Real need = census.structRadius[s] + radius + 25.0f;
						if (dx * dx + dy * dy < need * need)
							roomAround = false;
					}
					if (!roomAround)
						continue;
					if (TheBuildAssistant->isLocationLegalToBuild( &p, t, placeAngle,
							BuildAssistant::TERRAIN_RESTRICTIONS | BuildAssistant::NO_OBJECT_OVERLAP, nullptr, ai->m_player ) != LBC_OK)
						continue;
					if (!ai->isLocationSafe( &p, t ))
						continue;
					out = p;
					angleOut = placeAngle;
					found = true;
				}
			}
			TheTerrainVisual->removeAllBibs();	// isLocationLegalToBuild adds bib feedback, turn it off.
			return found;
		}
	};
	Placer placer = { this, census };

	// ---- the structure template of a kind that this player can build right now ----
	struct Chooser
	{
		AISkirmishPlayer *ai;
		Object *dozer;
		Int money;
		const ThingTemplate *pick( Int kind )
		{
			const ThingTemplate *best = nullptr;
			for (const ThingTemplate *t = TheThingFactory->firstTemplate(); t; t = t->friend_getNextTemplate())
			{
				if (classify( t ) != kind)
					continue;
				if (!ai->m_player->canBuild( t ) || !TheBuildAssistant->isPossibleToMakeUnit( dozer, t ))
					continue;
				if (t->calcCostToBuild( ai->m_player ) > money)
					continue;
				if (best == nullptr || strcmp( t->getName().str(), best->getName().str() ) < 0)
					best = t;	// name order, so the choice is the same everywhere
			}
			return best;
		}
	};
	Chooser chooser = { this, census.anyDozer, money - TheGlobalData->m_aiHardMoneyReserve / 2 };

	const Coord3D base = m_baseCenter;
	const Int rotate = (Int)(frame / period);	// a different start angle every step, so the spots spread around

	// builds one structure of 'kind' in the base; true when ordered
	struct BaseBuilder
	{
		Placer &placer;
		Chooser &chooser;
		AISkirmishPlayer *ai;
		Coord3D base;
		Real baseRadius;
		Int rotate;
		Bool build( Int kind )
		{
			const ThingTemplate *t = chooser.pick( kind );
			if (t == nullptr)
				return false;
			Coord3D pos;
			Real angle = 0.0f;
			if (!placer.find( t, base, 60.0f, baseRadius + (Real)TheGlobalData->m_aiHardBaseExtra, rotate, pos, angle ))
				return false;
			ai->m_player->addToPriorityBuildList( t->getName(), &pos, angle );
			return true;
		}
	};
	BaseBuilder builder = { placer, chooser, this, base, baseRadius, rotate };

	// ---- 1. power, whenever the base runs short ----
	if (!m_player->getEnergy()->hasSufficientPower() && !census.powerUnderConstruction && census.powerPlants < 14)
	{
		if (builder.build( EXP_POWER ))
			return;
	}

	// ---- 2. a supply center next to every supply source we can reach ----
	if (census.supplyCenters < TheGlobalData->m_aiHardSupplyCenters && money > TheGlobalData->m_aiHardMoneyReserve)
	{
		const ThingTemplate *t = chooser.pick( EXP_SUPPLY_CENTER );
		if (t != nullptr)
		{
			buildBySupplies( 3000, t->getName() );
			return;
		}
	}

	// ---- 3. income that needs no trucks ----
	{
		struct Target { Int kind; Int have; Int want; };
		const Target income[] =
		{
			{ EXP_BLACK_MARKET,		census.blackMarkets,		TheGlobalData->m_aiHardBlackMarkets },
			{ EXP_DROP_ZONE,		census.dropZones,			TheGlobalData->m_aiHardDropZones },
			{ EXP_INTERNET_CENTER,	census.internetCenters,		TheGlobalData->m_aiHardInternetCenters },
		};
		Int bestIdx = -1, bestDeficit = 0;
		for (Int i = 0; i < 3; ++i)
		{
			const Int deficit = income[i].want - income[i].have;
			if (deficit > bestDeficit)
			{
				bestDeficit = deficit;
				bestIdx = i;
			}
		}
		if (bestIdx >= 0 && money > TheGlobalData->m_aiHardMoneyReserve && builder.build( income[bestIdx].kind ))
			return;
	}

	// ---- 4. defences: first around the supply centers, then around the base ----
	{
		const AISideInfo *sideInfo = TheAI->getAiData()->m_sideInfo;
		while (sideInfo && sideInfo->m_side != m_player->getSide())
			sideInfo = sideInfo->m_next;
		const ThingTemplate *tower = sideInfo ? TheThingFactory->findTemplate( sideInfo->m_baseDefenseStructure1 ) : nullptr;

		if (tower && money > TheGlobalData->m_aiHardMoneyReserve && tower->calcCostToBuild( m_player ) <= money - TheGlobalData->m_aiHardMoneyReserve / 2
			&& m_player->canBuild( tower ) && TheBuildAssistant->isPossibleToMakeUnit( census.anyDozer, tower ))
		{
			for (size_t c = 0; c < census.supplyCenterPos.size(); ++c)
			{
				Int nearTowers = 0;
				// towers (base defence structures) already within reach of this supply center
				for (Object *obj = TheGameLogic->getFirstObject(); obj; obj = obj->getNextObject())
				{
					if (obj->getControllingPlayer() != m_player || !obj->isKindOf( KINDOF_FS_BASE_DEFENSE ) || obj->isEffectivelyDead())
						continue;
					const Real dx = obj->getPosition()->x - census.supplyCenterPos[c].x;
					const Real dy = obj->getPosition()->y - census.supplyCenterPos[c].y;
					if (dx * dx + dy * dy < 300.0f * 300.0f)
						++nearTowers;
				}
				if (nearTowers < TheGlobalData->m_aiHardSupplyDefenses)
				{
					Coord3D pos;
					Real angle = 0.0f;
					if (placer.find( tower, census.supplyCenterPos[c], 80.0f, 260.0f, rotate, pos, angle ))
					{
						m_player->addToPriorityBuildList( tower->getName(), &pos, angle );
						return;
					}
				}
			}

			if (census.baseDefenses < TheGlobalData->m_aiHardBaseDefenses)
			{
				buildAIBaseDefense( (rotate & 1) != 0 );	// the stock placement on the front, then the flank
				return;
			}
		}
	}

	// ---- 5. production: the kind that is furthest from its target ----
	{
		struct Target { Int kind; Int have; Int want; };
		const Target production[] =
		{
			{ EXP_WAR_FACTORY,	census.warFactories,	TheGlobalData->m_aiHardWarFactories },
			{ EXP_AIRFIELD,		census.airfields,		TheGlobalData->m_aiHardAirfields },
			{ EXP_BARRACKS,		census.barracks,		TheGlobalData->m_aiHardBarracks },
		};
		Int order[3] = { 0, 1, 2 };
		// biggest deficit first, ties keep the fixed order above
		for (Int i = 0; i < 3; ++i)
			for (Int j = i + 1; j < 3; ++j)
				if (production[order[j]].want - production[order[j]].have > production[order[i]].want - production[order[i]].have)
					std::swap( order[i], order[j] );

		for (Int i = 0; i < 3; ++i)
		{
			const Target &tg = production[order[i]];
			if (tg.want - tg.have <= 0 || money <= TheGlobalData->m_aiHardMoneyReserve)
				continue;
			if (builder.build( tg.kind ))
				return;
		}
	}
}

//-------------------------------------------------------------------------------------------------
/**
 * Hard skirmish AI army: fills idle factories, and sends the units gathered at home out in one big wave.
 * Only units on the player's default team are used, so the teams of the skirmish scripts are left alone.
 */
void AISkirmishPlayer::processHardArmy()
{
	if (!TheGlobalData->m_aiHardEnabled || m_difficulty != DIFFICULTY_HARD || !m_player->getCanBuildUnits())
		return;

	const UnsignedInt frame = TheGameLogic->getFrame();
	if (frame < (UnsignedInt)TheGlobalData->m_aiHardStartSeconds * LOGICFRAMES_PER_SECOND)
		return;
	const UnsignedInt period = 5u * LOGICFRAMES_PER_SECOND;
	if ((frame + (UnsignedInt)m_player->getPlayerIndex() * 7u) % period != 0)
		return;

	const Team *defaultTeam = m_player->getDefaultTeam();
	const Real homeRadius = std::max( m_baseRadius, 150.0f ) + 400.0f;
	const Real homeRadiusSqr = homeRadius * homeRadius;

	std::vector<Object *> home;		// army units standing at the base, in object list order
	std::vector<Object *> away;		// army units elsewhere that have nothing to do
	std::vector<Object *> factories;

	for (Object *obj = TheGameLogic->getFirstObject(); obj; obj = obj->getNextObject())
	{
		if (obj->getControllingPlayer() != m_player || obj->isEffectivelyDead())
			continue;
		if (obj->isKindOf( KINDOF_STRUCTURE ))
		{
			if ((obj->isKindOf( KINDOF_FS_WARFACTORY ) || obj->isKindOf( KINDOF_FS_AIRFIELD ) || obj->isKindOf( KINDOF_FS_BARRACKS ))
				&& !obj->isKindOf( KINDOF_FS_FAKE )
				&& !obj->testStatus( OBJECT_STATUS_UNDER_CONSTRUCTION ) && !obj->testStatus( OBJECT_STATUS_SOLD ))
				factories.push_back( obj );
			continue;
		}
		if (obj->getTeam() != defaultTeam || !obj->isKindOf( KINDOF_CAN_ATTACK ) || obj->isKindOf( KINDOF_DOZER ) || obj->isKindOf( KINDOF_HARVESTER ))
			continue;
		AIUpdateInterface *ai = obj->getAIUpdateInterface();
		if (ai == nullptr)
			continue;
		const Real dx = obj->getPosition()->x - m_baseCenter.x;
		const Real dy = obj->getPosition()->y - m_baseCenter.y;
		if (dx * dx + dy * dy <= homeRadiusSqr)
			home.push_back( obj );
		else if (ai->isIdle())
			away.push_back( obj );
	}

	// ---- 1. production: each idle factory gets the most expensive combat unit it can build and afford ----
	Int money = m_player->getMoney()->countMoney();
	const Int reserve = TheGlobalData->m_aiHardMoneyReserve;
	for (size_t f = 0; f < factories.size() && money > reserve; ++f)
	{
		Object *factory = factories[f];
		ProductionUpdateInterface *pu = factory->getProductionUpdateInterface();
		if (pu == nullptr || pu->getProductionCount() > 0)
			continue;

		const ThingTemplate *best[4] = { nullptr, nullptr, nullptr, nullptr };	// the four dearest, dearest first
		for (const ThingTemplate *t = TheThingFactory->firstTemplate(); t; t = t->friend_getNextTemplate())
		{
			if (t->isKindOf( KINDOF_STRUCTURE ) || !t->isKindOf( KINDOF_CAN_ATTACK ) || t->isKindOf( KINDOF_DOZER ) || t->isKindOf( KINDOF_HARVESTER ))
				continue;
			if (!m_player->canBuild( t ) || !TheBuildAssistant->isPossibleToMakeUnit( factory, t ))
				continue;
			const Int cost = t->calcCostToBuild( m_player );
			if (cost > money - reserve / 2)
				continue;
			// insert into the top four (cost descending, then name)
			for (Int i = 0; i < 4; ++i)
			{
				if (best[i] == nullptr || cost > best[i]->calcCostToBuild( m_player )
					|| (cost == best[i]->calcCostToBuild( m_player ) && strcmp( t->getName().str(), best[i]->getName().str() ) < 0))
				{
					for (Int j = 3; j > i; --j)
						best[j] = best[j - 1];
					best[i] = t;
					break;
				}
			}
		}
		Int choices = 0;
		while (choices < 4 && best[choices] != nullptr)
			++choices;
		if (choices == 0)
			continue;

		// rotate through the dearest units so the army is mixed
		const ThingTemplate *pick = best[(frame / period + (UnsignedInt)f) % (UnsignedInt)choices];
		if (pu->queueCreateUnit( pick, pu->requestUniqueUnitID() ))
			money -= pick->calcCostToBuild( m_player );
	}

	// ---- 2. the wave ----
	const Int waveSize = TheGlobalData->m_aiHardArmyWave;
	if (waveSize <= 0)
		return;
	if ((Int)home.size() < waveSize && away.empty())
		return;

	// target: the enemy structure closest to our base
	Object *target = nullptr;
	Real targetDist = 0.0f;
	for (Object *obj = TheGameLogic->getFirstObject(); obj; obj = obj->getNextObject())
	{
		if (obj->isEffectivelyDead() || !obj->isKindOf( KINDOF_STRUCTURE ) || obj->isKindOf( KINDOF_MP_COUNT_FOR_VICTORY ) == false)
			continue;
		Player *owner = obj->getControllingPlayer();
		if (owner == nullptr || owner == m_player || m_player->getRelationship( owner->getDefaultTeam() ) != ENEMIES)
			continue;
		const Real dx = obj->getPosition()->x - m_baseCenter.x;
		const Real dy = obj->getPosition()->y - m_baseCenter.y;
		const Real d = dx * dx + dy * dy;
		if (target == nullptr || d < targetDist)
		{
			target = obj;
			targetDist = d;
		}
	}
	if (target == nullptr)
		return;

	const Coord3D goal = *target->getPosition();
	if ((Int)home.size() >= waveSize)
	{
		for (size_t i = 0; i < home.size(); ++i)
			home[i]->getAIUpdateInterface()->aiAttackMoveToPosition( &goal, NO_MAX_SHOTS_LIMIT, CMD_FROM_AI );
	}
	// units that finished their attack elsewhere keep going
	for (size_t i = 0; i < away.size(); ++i)
		away[i]->getAIUpdateInterface()->aiAttackMoveToPosition( &goal, NO_MAX_SHOTS_LIMIT, CMD_FROM_AI );
}

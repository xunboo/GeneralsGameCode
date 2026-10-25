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

// Block formations for big ground armies.
//
// A move order for a group with enough movable ground units lays the units out as a rectangular block (width : depth = FormationAspect):
//   - front rows: heavily armoured combat vehicles (tanks)
//   - middle rows: infantry and light units
//   - back rows: long range weapons (artillery, rocket launchers) and unarmed support (dozers, harvesters, transports)
// The block follows the group's ground path. Where the corridor is too narrow for the full width (bridges, passes) the block becomes narrower and
// deeper, down to a single column, and widens again when the corridor opens up.
//
// Everything here is a pure function of the group members (sorted by rank and object ID), the pathfinder's terrain data and the global settings,
// so every machine of a multiplayer game computes exactly the same waypoints. No random numbers, no unordered containers, no hardware dependent state,
// and nothing is stored: the units simply receive their own list of waypoints.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include <algorithm>
#include <vector>

#include "Common/GlobalData.h"
#include "Common/KindOf.h"

#include "GameLogic/AI.h"
#include "GameLogic/AIPathfind.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Locomotor.h"
#include "GameLogic/Module/AIUpdate.h"
#include "GameLogic/Module/BodyModule.h"
#include "GameLogic/Object.h"
#include "GameLogic/TerrainLogic.h"

extern void clampWaypointPosition( Coord3D &position, Int margin );

namespace
{

enum FormationRank
{
	RANK_FRONT = 0,
	RANK_MIDDLE = 1,
	RANK_BACK = 2
};

const Real LONG_RANGE_WEAPON = 175.0f;		///< a weapon reaching this far belongs in the back (artillery, rocket launchers)
const Real HEAVY_ARMOR_HEALTH = 300.0f;	///< combat vehicles with at least this much health stand in front
const Real MIN_SPACING = 14.0f;
const Real STATION_STEP = 6.0f * PATHFIND_CELL_SIZE_F;	///< distance between the places where the corridor width is measured
const Int GROUP_PATH_DIAMETER_IN_CELLS = 6;				///< same corridor width the stock group move plans with

struct Member
{
	Object *obj;
	Int rank;
	Int key;		///< order inside the rank (see rankOf)
	ObjectID id;
};

bool memberBefore( const Member &a, const Member &b )
{
	if (a.rank != b.rank)
		return a.rank < b.rank;
	if (a.key != b.key)
		return a.key < b.key;
	return a.id < b.id;	// object IDs are unique, so the order is total and the same everywhere
}

/// Rank of a unit and its order inside the rank, from the unit's own data (kind of flags, weapon range, health): no unit names.
///   front:  the toughest first (most health), back: unarmed support first, then by growing weapon range, so the longest range stands last.
/// Both keys are whole numbers, so comparing them is exact on every machine.
Int rankOf( const Object *obj, Int &key )
{
	key = 0;
	if (obj->isKindOf( KINDOF_DOZER ) || obj->isKindOf( KINDOF_HARVESTER ))
		return RANK_BACK;

	const Real range = obj->getLargestWeaponRange();
	if (!obj->isKindOf( KINDOF_CAN_ATTACK ) || range <= 0.0f)
		return RANK_BACK;	// unarmed support
	if (range >= LONG_RANGE_WEAPON)
	{
		key = (Int)range;
		return RANK_BACK;	// artillery and the like
	}

	if (!obj->isKindOf( KINDOF_INFANTRY ))
	{
		const BodyModuleInterface *body = obj->getBodyModule();
		if (body && body->getMaxHealth() >= HEAVY_ARMOR_HEALTH)
		{
			key = -(Int)body->getMaxHealth();
			return RANK_FRONT;
		}
	}
	return RANK_MIDDLE;
}

/// Lateral offset (to the right of the walking direction) and depth (behind the front row) of slot k of n in a block that is cols wide.
void slotOffset( Int k, Int n, Int cols, Real spacing, Real &lateral, Real &depth )
{
	const Int row = k / cols;
	const Int col = k % cols;
	const Int inRow = std::min( cols, n - row * cols );
	lateral = ((Real)col - (Real)(inRow - 1) * 0.5f) * spacing;
	depth = (Real)row * spacing;
}

struct Station
{
	Coord3D pos;
	PathfindLayerEnum layer;
	Real arc;			///< distance from the first station along the path
	Coord2D fwd;		///< walking direction
	Real extLeft, extRight;	///< free space to both sides
	Int cols;			///< block width that fits here
	Bool vertex;		///< a node of the pathfinder's path (not just a measuring point)
};

inline Coord2D rightOf( const Coord2D &f )
{
	Coord2D r;
	r.x = f.y;
	r.y = -f.x;
	return r;
}

Coord2D directionBetween( const Coord3D &a, const Coord3D &b, const Coord2D &fallback )
{
	Coord2D d;
	d.x = b.x - a.x;
	d.y = b.y - a.y;
	const Real len = d.length();
	if (len < 0.001f)
		return fallback;
	d.x /= len;
	d.y /= len;
	return d;
}

/// Free space on both sides of a station (terrain and buildings only), measured in path cell steps up to maxHalf.
void measureCorridor( Station &st, const LocomotorSet &locomotors, Real maxHalf )
{
	Pathfinder *pf = TheAI->pathfinder();
	const Coord2D r = rightOf( st.fwd );

	Real ext[2] = { 0.0f, 0.0f };
	for (Int side = 0; side < 2; ++side)
	{
		const Real sign = side == 0 ? 1.0f : -1.0f;
		for (Real t = PATHFIND_CELL_SIZE_F; t <= maxHalf; t += PATHFIND_CELL_SIZE_F)
		{
			Coord3D q = st.pos;
			q.x += r.x * sign * t;
			q.y += r.y * sign * t;
			if (!pf->validMovementPosition( false, st.layer, locomotors, &q ))
				break;
			ext[side] = t;
		}
	}
	st.extRight = ext[0];
	st.extLeft = ext[1];
}

/// A point on the stations' polyline at the given arc length, shifted sideways.
Coord3D pointAt( const std::vector<Station> &stations, Real arc, Real lateral, size_t &hint )
{
	const size_t last = stations.size() - 1;
	if (arc < 0.0f)
		arc = 0.0f;
	if (arc > stations[last].arc)
		arc = stations[last].arc;

	while (hint + 1 < last && stations[hint + 1].arc <= arc)
		++hint;

	const Station &a = stations[hint];
	const Station &b = stations[std::min( hint + 1, last )];
	Real t = 0.0f;
	if (b.arc > a.arc)
		t = (arc - a.arc) / (b.arc - a.arc);

	const Real extLeft = std::min( a.extLeft, b.extLeft );
	const Real extRight = std::min( a.extRight, b.extRight );
	if (lateral > extRight)
		lateral = extRight;
	if (lateral < -extLeft)
		lateral = -extLeft;

	const Coord2D r = rightOf( a.fwd );
	Coord3D p;
	p.x = a.pos.x + (b.pos.x - a.pos.x) * t + r.x * lateral;
	p.y = a.pos.y + (b.pos.y - a.pos.y) * t + r.y * lateral;
	p.z = TheTerrainLogic->getLayerHeight( p.x, p.y, a.layer );
	return p;
}

}	// namespace

//-------------------------------------------------------------------------------------------------
/**
 * Moves the group to pos as a block formation. Returns false (and orders nothing) when the group is not eligible, so the stock move code runs.
 */
Bool AIGroup::friend_moveInFormation( const Coord3D *posIn, CommandSourceType cmdSource )
{
	if (!TheGlobalData->m_formationEnabled || cmdSource != CMD_FROM_PLAYER)
		return false;

	// ---- who takes part ----
	std::vector<Member> members;
	for (std::list<Object *>::iterator it = m_memberList.begin(); it != m_memberList.end(); ++it)
	{
		Object *obj = *it;
		if (obj->isDisabledByType( DISABLED_HELD ) || obj->isKindOf( KINDOF_IMMOBILE ))
			continue;
		AIUpdateInterface *ai = obj->getAIUpdateInterface();
		if (ai == nullptr)
			continue;
		// aircraft and anything that is not driving on the ground keeps the stock behaviour for the whole group
		if (obj->isKindOf( KINDOF_AIRCRAFT ) || obj->isKindOf( KINDOF_PRODUCED_AT_HELIPAD ) || !ai->isDoingGroundMovement())
			return false;

		Member m;
		m.obj = obj;
		m.rank = rankOf( obj, m.key );
		m.id = obj->getID();
		members.push_back( m );
	}

	const Int n = (Int)members.size();
	if (n < 2 || n < TheGlobalData->m_formationMinUnits)
		return false;

	std::sort( members.begin(), members.end(), memberBefore );

	// ---- block size ----
	Real spacing = MIN_SPACING;
	for (Int i = 0; i < n; ++i)
		spacing = std::max( spacing, 2.0f * members[i].obj->getGeometryInfo().getBoundingCircleRadius() + TheGlobalData->m_formationGap );

	const Real aspect = std::max( 1.0f, TheGlobalData->m_formationAspect );
	const Int fullCols = std::max( 1, std::min( n, (Int)ceil( sqrt( (Real)n * aspect ) ) ) );

	// ---- where the block starts: the unit nearest to the group's centre ----
	Coord3D centroid;
	centroid.zero();
	for (Int i = 0; i < n; ++i)
	{
		centroid.x += members[i].obj->getPosition()->x;
		centroid.y += members[i].obj->getPosition()->y;
	}
	centroid.x /= (Real)n;
	centroid.y /= (Real)n;

	Int leadIndex = 0;
	Real leadDist = 0.0f;
	for (Int i = 0; i < n; ++i)
	{
		const Real dx = members[i].obj->getPosition()->x - centroid.x;
		const Real dy = members[i].obj->getPosition()->y - centroid.y;
		const Real d = dx * dx + dy * dy;
		if (i == 0 || d < leadDist)
		{
			leadDist = d;
			leadIndex = i;
		}
	}
	Object *lead = members[leadIndex].obj;
	const Coord3D start = *lead->getPosition();
	const LocomotorSet &locomotors = lead->getAIUpdateInterface()->getLocomotorSet();

	Coord3D dest = *posIn;
	clampWaypointPosition( dest, (Int)(PATHFIND_CELL_SIZE_F * 4.0f) );
	dest.z = TheTerrainLogic->getGroundHeight( dest.x, dest.y );

	// a tiny move (or a click inside the army itself) is left to the stock code, which gathers the group
	{
		const Real dx = dest.x - start.x;
		const Real dy = dest.y - start.y;
		if (dx * dx + dy * dy < sqr( 2.0f * spacing * (Real)fullCols * 0.5f ))
			return false;
	}

	// ---- the way: the stock group path, or a straight line when there is none ----
	std::vector<Station> stations;
	Path *groundPath = TheAI->pathfinder()->findGroundPath( &start, &dest, GROUP_PATH_DIAMETER_IN_CELLS, false );
	{
		std::vector<Coord3D> points;
		std::vector<PathfindLayerEnum> layers;
		std::vector<Bool> isVertex;
		points.push_back( start );
		layers.push_back( TheTerrainLogic->getLayerForDestination( &start ) );
		isVertex.push_back( TRUE );

		if (groundPath)
		{
			const Real farEnoughSqr = sqr( (Real)GROUP_PATH_DIAMETER_IN_CELLS * PATHFIND_CELL_SIZE_F );
			for (PathNode *node = groundPath->getFirstNode(); node; node = node->getNextOptimized())
			{
				const Coord3D *np = node->getPosition();
				const Real dx = np->x - start.x;
				const Real dy = np->y - start.y;
				if (dx * dx + dy * dy < farEnoughSqr)
					continue;	// do not send the army backwards to the path's first node
				points.push_back( *np );
				layers.push_back( node->getLayer() );
				isVertex.push_back( TRUE );
			}
		}
		// the path ends where we want to be
		points.push_back( dest );
		layers.push_back( TheTerrainLogic->getLayerForDestination( &dest ) );
		isVertex.push_back( TRUE );

		Coord2D lastFwd = directionBetween( start, dest, Coord2D() );
		if (lastFwd.x == 0.0f && lastFwd.y == 0.0f)
			lastFwd.y = 1.0f;

		Real arc = 0.0f;
		for (size_t i = 0; i + 1 < points.size(); ++i)
		{
			const Coord2D segFwd = directionBetween( points[i], points[i + 1], lastFwd );
			const Real dx = points[i + 1].x - points[i].x;
			const Real dy = points[i + 1].y - points[i].y;
			const Real len = sqrt( dx * dx + dy * dy );
			const Int steps = std::max( 1, (Int)ceil( len / STATION_STEP ) );
			for (Int s = 0; s < steps; ++s)
			{
				const Real t = (Real)s / (Real)steps;
				Station st;
				st.pos.x = points[i].x + dx * t;
				st.pos.y = points[i].y + dy * t;
				st.pos.z = points[i].z;
				st.layer = layers[i];
				st.arc = arc + len * t;
				st.fwd = segFwd;
				st.extLeft = st.extRight = 0.0f;
				st.cols = fullCols;
				st.vertex = (s == 0) ? isVertex[i] : FALSE;
				stations.push_back( st );
			}
			arc += len;
			lastFwd = segFwd;
		}
		Station endStation;
		endStation.pos = points.back();
		endStation.layer = layers.back();
		endStation.arc = arc;
		endStation.fwd = lastFwd;
		endStation.extLeft = endStation.extRight = 0.0f;
		endStation.cols = fullCols;
		endStation.vertex = TRUE;
		stations.push_back( endStation );
	}
	if (groundPath)
	{
		deleteInstance( groundPath );
		groundPath = nullptr;
	}

	// ---- how wide is the corridor at every station ----
	const Real maxHalf = 0.5f * (Real)fullCols * spacing + 2.0f * PATHFIND_CELL_SIZE_F;
	for (size_t i = 0; i + 1 < stations.size(); ++i)
	{
		measureCorridor( stations[i], locomotors, maxHalf );
		const Real width = stations[i].extLeft + stations[i].extRight + PATHFIND_CELL_SIZE_F;
		stations[i].cols = std::max( 1, std::min( fullCols, (Int)floor( width / spacing ) ) );
	}
	// the destination is measured too, but its block is always laid out at full width (see below)
	stations.back().extLeft = stations.back().extRight = maxHalf;
	stations.back().cols = fullCols;
	if (stations.size() >= 2)
	{
		stations.back().extLeft = stations[stations.size() - 2].extLeft;
		stations.back().extRight = stations[stations.size() - 2].extRight;
	}

	// ---- final block around the destination ----
	const Station &endSt = stations.back();
	const Coord2D endFwd = endSt.fwd;
	const Coord2D endRight = rightOf( endFwd );
	const PathfindLayerEnum destLayer = TheTerrainLogic->getLayerForDestination( &dest );

	// ---- cohesion: everybody moves at the speed of the slowest member of the group ----
	// (the same value the stock group moves use; a fixed cap for the whole trip, lifted when the path ends, see AIFollowPathState::onExit)
	Real groupSpeed = getSpeed();
	if (groupSpeed <= 0.0f || groupSpeed >= 100000.0f)
		groupSpeed = FAST_AS_POSSIBLE;

	// ---- orders ----
	for (Int k = 0; k < n; ++k)
	{
		Object *unit = members[k].obj;
		AIUpdateInterface *ai = unit->getAIUpdateInterface();

		TheAI->pathfinder()->removeGoal( unit );
		unit->setFormationID( NO_FORMATION_ID );

		// The unit's waypoints begin where it stands on the path (the nearest station, the lower index on a tie), so nobody is sent back to the
		// group's centre first. Units that are not in their row yet sort themselves out while they travel: a station's slot is 'depth' behind it.
		size_t nearest = 0;
		{
			Real nearestDist = 0.0f;
			for (size_t s = 0; s < stations.size(); ++s)
			{
				const Real dx = stations[s].pos.x - unit->getPosition()->x;
				const Real dy = stations[s].pos.y - unit->getPosition()->y;
				const Real d = dx * dx + dy * dy;
				if (s == 0 || d < nearestDist)
				{
					nearestDist = d;
					nearest = s;
				}
			}
		}

		std::vector<Coord3D> waypoints;
		size_t hint = nearest;
		Real prevArc = stations[nearest].arc;
		Coord3D prevPos = *unit->getPosition();

		if (stations.size() > 2)
		{
			for (size_t i = nearest; i + 1 < stations.size(); ++i)
			{
				const Station &st = stations[i];
				Real lateral, depth;
				slotOffset( k, n, st.cols, spacing, lateral, depth );

				// the unit stands 'depth' behind the station along the path, but never steps back
				Real arc = std::max( prevArc, st.arc - depth );

				// corners of the path between the last waypoint and this one are kept
				for (size_t v = 0; v < stations.size(); ++v)
				{
					if (stations[v].vertex && stations[v].arc > prevArc && stations[v].arc < arc)
					{
						size_t vh = 0;
						const Coord3D vp = pointAt( stations, stations[v].arc, lateral, vh );
						waypoints.push_back( vp );
					}
				}

				// a waypoint only where something changes: the first one, the width of the block, or a node of the path. The pathfinder
				// finds the way between them, and fewer waypoints means far fewer path requests for a big army.
				const Bool emit = waypoints.empty() || (i > 0 && st.cols != stations[i - 1].cols) || st.cols != stations[i + 1].cols || st.vertex;
				if (emit)
				{
					const Coord3D p = pointAt( stations, arc, lateral, hint );
					const Real dx = p.x - prevPos.x;
					const Real dy = p.y - prevPos.y;
					if (waypoints.empty() || dx * dx + dy * dy > 1.0f)
					{
						waypoints.push_back( p );
						prevPos = p;
					}
				}
				prevArc = arc;
			}
		}

		// the unit's place in the full width block at the destination
		Real lateral, depth;
		slotOffset( k, n, fullCols, spacing, lateral, depth );
		Coord3D slot;
		slot.x = dest.x + endRight.x * lateral - endFwd.x * depth;
		slot.y = dest.y + endRight.y * lateral - endFwd.y * depth;
		slot.z = TheTerrainLogic->getLayerHeight( slot.x, slot.y, destLayer );

		if (ai->isDoingGroundMovement())
		{
			TheAI->pathfinder()->adjustDestination( unit, ai->getLocomotorSet(), &slot, nullptr );
			TheAI->pathfinder()->updateGoal( unit, &slot, LAYER_GROUND );
		}

		if (waypoints.empty())
		{
			ai->aiMoveToPosition( &slot, cmdSource );
		}
		else
		{
			waypoints.push_back( slot );
			ai->aiFollowPath( &waypoints, nullptr, cmdSource );
		}

		// the move states reset the speed when they start, so the cap goes on after the order
		if (groupSpeed != FAST_AS_POSSIBLE)
			ai->setDesiredSpeed( groupSpeed );
	}

	return TRUE;
}

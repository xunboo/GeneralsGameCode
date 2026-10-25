/*
**	Command & Conquer Generals Zero Hour(tm)
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

#pragma once

#include "PathfindCellInfo.h"
#include "PathSearchContext.h"

class Object;

typedef UnsignedShort zoneStorageType;

/**
 * This represents one cell in the pathfinding grid.
 * These cells categorize the world into idealized cellular states,
 * and are also used for efficient A* pathfinding.
 * The A* state of the cell is not stored here but in a PathSearchNode of the current PathSearchContext,
 * so the map itself is only read by a search. The units on the cell are in m_info.
 * @todo Optimize memory usage of pathfind grid.
 */
class PathfindCell
{
public:

	enum CellType
	{
		CELL_CLEAR		= 0x00,									///< clear, unobstructed ground
		CELL_WATER		= 0x01,									///< water area
		CELL_CLIFF		= 0x02,									///< steep altitude change
		CELL_RUBBLE		= 0x03,									///< Cell is occupied by rubble.
		CELL_OBSTACLE	= 0x04,									///< Occupied by a structure
		CELL_BRIDGE_IMPASSABLE = 0x05,				///< Piece of a bridge that is impassable.
		CELL_IMPASSABLE = 0x06								///< Just plain impassable except for aircraft.
	};

	enum CellFlags
	{
		NO_UNITS		= 0x00,						///< No units in this cell.
		UNIT_GOAL		= 0x01,						///< A unit is heading to this cell.
		UNIT_PRESENT_MOVING	= 0x02,		///< A unit is moving through this cell.
		UNIT_PRESENT_FIXED	= 0x03,		///< A unit is stationary in this cell.
		UNIT_GOAL_OTHER_MOVING	= 0x05		///< A unit is moving through this cell, and another unit has this as it's goal.
	};

	/// reset the cell
	void reset();

	PathfindCell();
	~PathfindCell();

	Bool setTypeAsObstacle( Object *obstacle, Bool isFence, const ICoord2D &pos );				///< flag this cell as an obstacle, from the given one
	Bool removeObstacle( Object *obstacle );				///< unflag this cell as an obstacle, from the given one
	void setType( CellType type );	///< set the cell type
	CellType getType() const { return (CellType)m_type; }				///< get the cell type
	CellFlags getFlags() const { return (CellFlags)m_flags; }				///< get the cell type
	Bool isAircraftGoal() const {return m_aircraftGoal != 0;}

	Bool isObstaclePresent( ObjectID objID ) const;					///< return true if the given object ID is registered as an obstacle in this cell

	Bool isObstacleTransparent() const;
	Bool isObstacleFence() const;

	/// Return estimated cost from given cell to reach goal cell
	UnsignedInt costToGoal( PathfindCell *goal );

	UnsignedInt costToHierGoal( PathfindCell *goal );

	UnsignedInt costSoFar( PathfindCell *parent );


	/// put self on "open" list in ascending cost order
	void putOnSortedOpenList( PathfindOpenList &list );

	/// remove self from "open" list
	void removeFromOpenList( PathfindOpenList &list );

	/// put self on "closed" list, return new list
	void putOnClosedList( PathfindCellList &list );

	/// remove self from "closed" list
	void removeFromClosedList( PathfindCellList &list );

	/// remove all cells from closed list.
	static Int releaseClosedList( PathfindCellList &list );

	/// remove all cells from open list.
	static Int releaseOpenList( PathfindOpenList &list );

	__forceinline PathfindCell *getNextOpen() const {const PathSearchNode *next = searchNode().m_nextOpen; return next ? next->m_cell : nullptr;}
	__forceinline PathfindCell *getPrevOpen() const {const PathSearchNode *prev = searchNode().m_prevOpen; return prev ? prev->m_cell : nullptr;}

	__forceinline UnsignedShort getXIndex() const;
	__forceinline UnsignedShort getYIndex() const;

	Bool isBlockedByAlly() const;
	void setBlockedByAlly(Bool blocked);

	__forceinline Bool getOpen() const {const PathSearchNode *node = activeSearchNode(); return node && node->m_open;}
	__forceinline Bool getClosed() const {const PathSearchNode *node = activeSearchNode(); return node && node->m_closed;}
	__forceinline UnsignedInt getCostSoFar() const {const PathSearchNode *node = activeSearchNode(); return node ? node->m_costSoFar : 0;}
	__forceinline UnsignedInt getTotalCost() const {const PathSearchNode *node = activeSearchNode(); return node ? node->m_totalCost : 0;}

	UnsignedInt getTotalCostDifference(PathfindCell& other) const;

	__forceinline void setCostSoFar(UnsignedInt cost) { PathSearchNode *node = activeSearchNode(); if( node ) node->m_costSoFar = cost;}
	__forceinline void setTotalCost(UnsignedInt cost) { PathSearchNode *node = activeSearchNode(); if( node ) node->m_totalCost = cost;}

	void setParentCell(PathfindCell* parent);
	void clearParentCell();
	void setParentCellHierarchical(PathfindCell* parent);
	__forceinline PathfindCell* getParentCell() const {const PathSearchNode *node = activeSearchNode(); return node ? node->m_parent : nullptr;}

	Bool startPathfind( PathfindCell *goalCell );
	Bool getPinched() const {return m_pinched;}
	void setPinched(Bool pinch) {noteWrite(); m_pinched = pinch;	}

	Bool allocateInfo(const ICoord2D &pos);	///< Starts the search node of this cell in the current search
	void releaseInfo();											///< Ends the search node of this cell, unless it is still on a list
	__forceinline Bool hasInfo() const {return activeSearchNode()!=nullptr;}	///< True when the cell takes part in the current search

	UnsignedInt getSearchIndex() const {return m_searchIndex;}
	static void resetSearchIndices() {s_numSearchCells = 0;}
	static void assignSearchIndices(PathfindCell *cells, Int count);	///< Gives each cell of a newly allocated block its own node index
	static UnsignedInt getNumSearchCells() {return s_numSearchCells;}
	static void setWriteLog(std::vector<UnsignedInt> *log) {s_writeLog = log;}	///< While set, every change of a cell adds its index to the log
	zoneStorageType getZone() const {return m_zone;}
	void setZone(zoneStorageType zone) {noteWrite(); m_zone = zone;}
	void setGoalUnit(ObjectID unit, const ICoord2D &pos );
	void setGoalAircraft(ObjectID unit, const ICoord2D &pos );
	void setPosUnit(ObjectID unit, const ICoord2D &pos );
	ObjectID getGoalUnit() const {ObjectID id = m_info?m_info->m_goalUnitID:INVALID_ID; return id;}
	ObjectID getGoalAircraft() const {ObjectID id = m_info?m_info->m_goalAircraftID:INVALID_ID; return id;}
	ObjectID getPosUnit() const {ObjectID id = m_info?m_info->m_posUnitID:INVALID_ID; return id;}

	ObjectID getObstacleID() const;

	void setLayer( PathfindLayerEnum layer ) { noteWrite(); m_layer = layer; }	///< set the cell layer
	PathfindLayerEnum getLayer() const { return (PathfindLayerEnum)m_layer; }				///< get the cell layer

	void setConnectLayer( PathfindLayerEnum layer ) { noteWrite(); m_connectsToLayer = layer; }	///< set the cell layer	connect id
	PathfindLayerEnum getConnectLayer() const { return (PathfindLayerEnum)m_connectsToLayer; }				///< get the cell layer connect id

	static Bool typesMatch(const PathfindCell& targetCell, const PathfindCell& sourceCell);
	static Bool waterGround(const PathfindCell& targetCell, const PathfindCell& sourceCell);
	static Bool groundRubble(const PathfindCell& targetCell, const PathfindCell& sourceCell);
	static Bool terrain(const PathfindCell& targetCell, const PathfindCell& sourceCell);
	static Bool crusherGround(const PathfindCell& targetCell, const PathfindCell& sourceCell);
	static Bool groundCliff(const PathfindCell& targetCell, const PathfindCell& sourceCell);

private:
	Bool allocatePersistentInfo(const ICoord2D &pos);	///< Info record holding the units on this cell
	void releasePersistentInfo();

	__forceinline PathSearchNode &searchNode() const {return PathSearchContext::current().getNode(m_searchIndex);}
#if PATHFIND_LEGACY_NODE_LIFETIME
	// The node is in use whenever the original pathfinder would have had a PathfindCellInfo on this cell.
	__forceinline PathSearchNode *activeSearchNode() const {return (m_info || m_searchInfo) ? &searchNode() : nullptr;}
#else
	__forceinline PathSearchNode *activeSearchNode() const
	{
		PathSearchContext &context = PathSearchContext::current();
		return context.isNodeActive(m_searchIndex) ? &context.getNode(m_searchIndex) : nullptr;
	}
#endif

	__forceinline void noteWrite() const {if (s_writeLog) s_writeLog->push_back(m_searchIndex);}

	static UnsignedInt s_numSearchCells;
	static std::vector<UnsignedInt> *s_writeLog;
#if PATHFIND_LEGACY_NODE_LIFETIME
	static Int s_numSearchInfos;							///< Records the original pathfinder would have taken from the pool for searches
#endif

	PathfindCellInfo *m_info;
	UnsignedInt m_searchIndex;                ///< index of this cell's node in a PathSearchContext
	ObjectID m_obstacleID;	                  ///< the object ID who overlaps this cell
#if PATHFIND_LEGACY_NODE_LIFETIME
	UnsignedInt m_searchInfo : 1;             ///< The original pathfinder would have a search record on this cell, see s_numSearchInfos
#endif
	UnsignedInt m_blockedByAlly : 1;          ///< True if this cell is blocked by an allied unit.
	UnsignedInt m_obstacleIsFence : 1;        ///< True if occupied by a fence.
	UnsignedInt m_obstacleIsTransparent : 1;  ///< True if obstacle is transparent (undefined if obstacleid is invalid)

	zoneStorageType m_zone : 14;              ///< Zone. Each zone is a set of adjacent terrain type.  If from & to in the same zone, you can successfully pathfind.  If not,
	                                          /// you still may be able to if you can cross multiple terrain types.
	UnsignedShort m_aircraftGoal : 1;         ///< This is an aircraft goal cell.
	UnsignedShort m_pinched : 1;              ///< This cell is surrounded by obstacle cells.
	UnsignedByte m_type : 4;                  ///< what type of cell terrain this is.
	UnsignedByte m_flags : 4;                 ///< what type of units are in or moving through this cell.
	UnsignedByte m_connectsToLayer : 4;       ///< This cell can pathfind onto this layer, if > LAYER_TOP.
	UnsignedByte m_layer : 4;                 ///< Layer of this cell.
};

typedef PathfindCell *PathfindCellP;

__forceinline UnsignedShort PathfindCell::getXIndex() const
{
	const PathSearchNode *node = activeSearchNode();
	return node ? node->m_pos.x : m_info->m_pos.x;
}

__forceinline UnsignedShort PathfindCell::getYIndex() const
{
	const PathSearchNode *node = activeSearchNode();
	return node ? node->m_pos.y : m_info->m_pos.y;
}

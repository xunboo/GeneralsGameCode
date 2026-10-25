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

#include "PreRTS.h"
#include "Common/GlobalData.h"
#include "GameLogic/AIPathfind.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Object.h"
#include "GameLogic/Pathfinder/PathfindCell.h"
#include "GameLogic/Pathfinder/PathfindCellInfo.h"
#include "GameLogic/Pathfinder/PathfindCellList.h"
#include "GameLogic/Pathfinder/PathfindConstants.h"
#include "GameLogic/Pathfinder/PathSearchContext.h"
#include "GameLogic/TerrainLogic.h"

PathfindCell::PathfindCell() :m_info(nullptr), m_searchIndex(0)
{
#if PATHFIND_LEGACY_NODE_LIFETIME
	m_searchInfo = false;
#endif
	reset();
}

UnsignedInt PathfindCell::s_numSearchCells = 0;
std::vector<UnsignedInt> *PathfindCell::s_writeLog = nullptr;
#if PATHFIND_LEGACY_NODE_LIFETIME
Int PathfindCell::s_numSearchInfos = 0;
#endif

void PathfindCell::assignSearchIndices(PathfindCell *cells, Int count)
{
	for (Int i = 0; i < count; i++) {
		cells[i].m_searchIndex = s_numSearchCells++;
	}
}

/**
 * Destructor
 */
PathfindCell::~PathfindCell()
{
	if (m_info) PathfindCellInfo::releaseACellInfo(m_info);
	m_info = nullptr;
#if PATHFIND_LEGACY_NODE_LIFETIME
	if (m_searchInfo) {
		m_searchInfo = false;
		s_numSearchInfos--;
	}
#endif
	static Bool warn = true;
	if (warn) {
		warn = false;
		DEBUG_LOG( ("PathfindCell::~PathfindCell m_info Allocated."));
	}
}

/**
 * Reset the cell to default values
 */
void PathfindCell::reset()
{
	noteWrite();
	m_type = PathfindCell::CELL_CLEAR;
	m_flags = PathfindCell::NO_UNITS;
	m_zone = 0;
	m_aircraftGoal = false;
	m_pinched = false;
	if (m_info) {
		m_info->m_obstacleID = INVALID_ID;
		PathfindCellInfo::releaseACellInfo(m_info);
		m_info = nullptr;
	}
#if PATHFIND_LEGACY_NODE_LIFETIME
	if (m_searchInfo) {
		m_searchInfo = false;
		s_numSearchInfos--;
	}
#endif
	m_obstacleID = INVALID_ID;
	m_blockedByAlly = false;
	m_obstacleIsFence = false;
	m_obstacleIsTransparent = false;

	m_connectsToLayer = LAYER_INVALID;
	m_layer = LAYER_GROUND;

}

/**
 * Reset the pathfinding values in the cell.
 */
Bool PathfindCell::startPathfind( PathfindCell *goalCell  )
{
	PathSearchNode *node = activeSearchNode();
	DEBUG_ASSERTCRASH(node, ("Has to have info."));
	if (!node) return false;
	node->m_nextOpen = nullptr;
	node->m_prevOpen = nullptr;
	node->m_parent = nullptr;
	node->m_costSoFar = 0;		// start node, no cost to get here
	node->m_totalCost = 0;
	if (goalCell) {
		node->m_totalCost = costToGoal( goalCell );
	}
	node->m_open = FALSE;
	node->m_closed = FALSE;
	return true;
}

/**
 * Set the blocked by ally flag on the pathfind cell info.
 */
Bool PathfindCell::isBlockedByAlly() const
{
#if PATHFIND_LEGACY_NODE_LIFETIME
	return m_blockedByAlly;
#else
	// The flag belongs to the search, so searches on worker threads never write the shared map.
	const PathSearchNode *node = activeSearchNode();
	return node && node->m_blockedByAlly;
#endif
}

void PathfindCell::setBlockedByAlly(Bool blocked)
{
#if PATHFIND_LEGACY_NODE_LIFETIME
	m_blockedByAlly = (blocked != 0);
#else
	PathSearchNode *node = activeSearchNode();
	if (node) node->m_blockedByAlly = (blocked != 0);
#endif
}


/**
 * Set the parent pointer.
 */
void PathfindCell::setParentCell( PathfindCell* parent  )
{
	PathSearchNode *node = activeSearchNode();
	DEBUG_ASSERTCRASH(node, ("Has to have info."));
	if (!node) return;
	node->m_parent = parent;
	Int dx = node->m_pos.x - parent->getXIndex();
	Int dy = node->m_pos.y - parent->getYIndex();
	if (dx<-1 || dx>1 || dy<-1 || dy>1) {
		DEBUG_CRASH(("Invalid parent index."));
	}
}

/**
 * Set the parent pointer.
 */
void PathfindCell::setParentCellHierarchical( PathfindCell* parent  )
{
	PathSearchNode *node = activeSearchNode();
	DEBUG_ASSERTCRASH(node, ("Has to have info."));
	if (!node) return;
	node->m_parent = parent;
}

/**
 * Reset the parent cell.
 */
void PathfindCell::clearParentCell(  )
{
	PathSearchNode *node = activeSearchNode();
	DEBUG_ASSERTCRASH(node, ("Has to have info."));
	if (!node) return;
	node->m_parent = nullptr;
}


#if PATHFIND_LEGACY_NODE_LIFETIME

/**
 * Takes a search record for this cell. Like the original pathfinder, a cell that already has a record keeps it
 * with whatever values it holds, and no more records are given out once CELL_INFOS_TO_ALLOCATE are in use.
 */
Bool PathfindCell::allocateInfo( const ICoord2D &pos )
{
	if (!m_info && !m_searchInfo) {
		if (PathfindCellInfo::getNumAllocated() + s_numSearchInfos >= (Int)CELL_INFOS_TO_ALLOCATE) {
			return false;
		}
		m_searchInfo = true;
		s_numSearchInfos++;
		PathSearchContext::current().resetNode(this, m_searchIndex, pos);
	}
	return true;
}

/**
 * Gives back the record of this cell, unless the cell still needs it for its units or obstacle or it is still on a list.
 */
void PathfindCell::releaseInfo()
{
	if (!m_info && !m_searchInfo) {
		return;
	}
	PathSearchNode &node = searchNode();

	// TheSuperHackers @bugfix Mauller/SkyAero 05/06/2025 Parent cell links need clearing to prevent dangling pointers on starting points that can link them to an invalid parent cell.
	// Parent cells are only cleared within Pathfinder::prependCells, so cells that do not make it onto the final path do not get their parent cell cleared.
	// Cells with a special flags also do not get their PathfindCellInfo cleared and therefore can leave a parent cell set on a starting cell.
	node.m_parent = nullptr;

	if (m_type == PathfindCell::CELL_OBSTACLE || m_flags != NO_UNITS || m_aircraftGoal) {
		return;
	}

	DEBUG_ASSERTCRASH(node.m_prevOpen==nullptr && node.m_nextOpen==nullptr, ("Shouldn't be linked."));
	DEBUG_ASSERTCRASH(node.m_open==0 && node.m_closed==0, ("Shouldn't be linked."));
	if (node.m_prevOpen || node.m_nextOpen || node.m_open || node.m_closed) {
		// Bad release.  Skip for now, better leak than crash.  jba.
		return;
	}

	if (m_info) {
		DEBUG_ASSERTCRASH(m_info->m_goalUnitID==INVALID_ID && m_info->m_posUnitID==INVALID_ID, ("Shouldn't be occupied."));
		DEBUG_ASSERTCRASH(m_info->m_goalAircraftID==INVALID_ID , ("Shouldn't be occupied by aircraft."));
		PathfindCellInfo::releaseACellInfo(m_info);
		m_info = nullptr;
	} else {
		m_searchInfo = false;
		s_numSearchInfos--;
	}
}

/**
 * Allocates the info record that holds the units on this cell. A search record already on the cell
 * becomes this record and keeps its search values, as the original used one record for both.
 */
Bool PathfindCell::allocatePersistentInfo( const ICoord2D &pos )
{
	if (!m_info) {
		if (!m_searchInfo && PathfindCellInfo::getNumAllocated() + s_numSearchInfos >= (Int)CELL_INFOS_TO_ALLOCATE) {
			return false;
		}
		m_info = PathfindCellInfo::getACellInfo(this, pos);
		if (!m_info) {
			return false;
		}
		if (m_searchInfo) {
			m_searchInfo = false;
			s_numSearchInfos--;
		} else {
			PathSearchContext::current().resetNode(this, m_searchIndex, pos);
		}
	}
	return true;
}

/**
 * The original pathfinder used the same release for unit and search records.
 */
void PathfindCell::releasePersistentInfo()
{
	releaseInfo();
}

#else

/**
 * Starts the search node of this cell in the current search.
 * Nodes come from the context of the calling thread and never run out.
 */
Bool PathfindCell::allocateInfo( const ICoord2D &pos )
{
	PathSearchContext &context = PathSearchContext::current();
	if (!context.canActivateNode(m_searchIndex)) {
		context.m_nodeLimitHits++;
		return false; // out of nodes, like the original running out of PathfindCellInfos
	}
	context.activateNode(this, m_searchIndex, pos);
	return true;
}

/**
 * Ends the search node of this cell.
 */
void PathfindCell::releaseInfo()
{
	PathSearchNode *node = activeSearchNode();
	if (!node) {
		return;
	}

	// TheSuperHackers @bugfix Mauller/SkyAero 05/06/2025 Parent cell links need clearing to prevent dangling pointers on starting points that can link them to an invalid parent cell.
	// Parent cells are only cleared within Pathfinder::prependCells, so cells that do not make it onto the final path do not get their parent cell cleared.
	node->m_parent = nullptr;

	DEBUG_ASSERTCRASH(node->m_prevOpen==nullptr && node->m_nextOpen==nullptr, ("Shouldn't be linked."));
	DEBUG_ASSERTCRASH(node->m_open==0 && node->m_closed==0, ("Shouldn't be linked."));
	if (node->m_prevOpen || node->m_nextOpen || node->m_open || node->m_closed) {
		// Bad release.  Skip for now, better leak than crash.  jba.
		return;
	}

	PathSearchContext::current().releaseNode(m_searchIndex);
}

/**
 * Allocates the info record that holds the units on this cell.
 */
Bool PathfindCell::allocatePersistentInfo( const ICoord2D &pos )
{
	if (!m_info) {
		m_info = PathfindCellInfo::getACellInfo(this, pos);
		return (m_info != nullptr);
	}
	return true;
}

/**
 * Releases the info record that holds the units on this cell, once nothing uses it.
 */
void PathfindCell::releasePersistentInfo()
{
	if (m_type == PathfindCell::CELL_OBSTACLE || m_flags != NO_UNITS || m_aircraftGoal) {
		return;
	}

	if (!m_info) {
		return;
	}

	DEBUG_ASSERTCRASH(m_info->m_goalUnitID==INVALID_ID && m_info->m_posUnitID==INVALID_ID, ("Shouldn't be occupied."));
	DEBUG_ASSERTCRASH(m_info->m_goalAircraftID==INVALID_ID , ("Shouldn't be occupied by aircraft."));

	PathfindCellInfo::releaseACellInfo(m_info);
	m_info = nullptr;

}

#endif

/**
 * Sets the goal unit into the info record for a cell.
 */
void PathfindCell::setGoalUnit(ObjectID unitID, const ICoord2D &pos )
{
	noteWrite();
	if (unitID==INVALID_ID) {
		// removing goal.
		if (m_info) {
			m_info->m_goalUnitID = INVALID_ID;
			if (m_info->m_posUnitID == INVALID_ID) {
				// No units here.
				DEBUG_ASSERTCRASH(m_flags==UNIT_GOAL, ("Bad flags."));
				m_flags = NO_UNITS;
				releasePersistentInfo();
			} else{
				m_flags = UNIT_PRESENT_MOVING;
			}
		}	else {
			DEBUG_ASSERTCRASH(m_flags == NO_UNITS, ("Bad flags."));
		}
	} else {
		// adding goal.
		if (!m_info) {
			DEBUG_ASSERTCRASH(m_flags == NO_UNITS, ("Bad flags."));
			allocatePersistentInfo(pos);
		}
		if (!m_info) {
			DEBUG_CRASH(("Ran out of pathfind cells - fatal error!!!!! jba."));
			return;
		}
		m_info->m_goalUnitID = unitID;
		if (unitID==m_info->m_posUnitID) {
			m_flags = UNIT_PRESENT_FIXED;
		} else if (m_info->m_posUnitID==INVALID_ID) {
			m_flags = UNIT_GOAL;
		}	else {
			m_flags = UNIT_GOAL_OTHER_MOVING;
		}
	}
}


/**
 * Sets the goal aircraft into the info record for a cell.
 */
void PathfindCell::setGoalAircraft(ObjectID unitID, const ICoord2D &pos )
{
	noteWrite();
	if (unitID==INVALID_ID) {
		// removing goal.
		if (m_info) {
			m_info->m_goalAircraftID = INVALID_ID;
			m_aircraftGoal = false;
			releasePersistentInfo();
		}	else {
			DEBUG_ASSERTCRASH(m_aircraftGoal==false, ("Bad flags."));
		}
	} else {
		// adding goal.
		if (!m_info) {
			DEBUG_ASSERTCRASH(m_aircraftGoal==false, ("Bad flags."));
			allocatePersistentInfo(pos);
		}
		if (!m_info) {
			DEBUG_CRASH(("Ran out of pathfind cells - fatal error!!!!! jba."));
			return;
		}
		m_info->m_goalAircraftID = unitID;
		m_aircraftGoal = true;
	}
}


/**
 * Sets the position unit into the info record for a cell.
 */
void PathfindCell::setPosUnit(ObjectID unitID, const ICoord2D &pos )
{
	noteWrite();
	if (unitID==INVALID_ID) {
		// removing position.
		if (m_info) {
			m_info->m_posUnitID = INVALID_ID;
			if (m_info->m_goalUnitID == INVALID_ID) {
				// No units here.
				DEBUG_ASSERTCRASH(m_flags==UNIT_PRESENT_MOVING, ("Bad flags."));
				m_flags = NO_UNITS;
				releasePersistentInfo();
			}	else {
				m_flags = UNIT_GOAL;
			}
		}	else {
			DEBUG_ASSERTCRASH(m_flags == NO_UNITS, ("Bad flags."));
		}
	} else {
		// adding goal.
		if (!m_info) {
			DEBUG_ASSERTCRASH(m_flags == NO_UNITS, ("Bad flags."));
			allocatePersistentInfo(pos);
		}
		if (!m_info) {
			DEBUG_CRASH(("Ran out of pathfind cells - fatal error!!!!! jba."));
			return;
		}
		if (m_info->m_goalUnitID!=INVALID_ID && (m_info->m_goalUnitID==m_info->m_posUnitID)) {
			// A unit is already occupying this cell.
			return;
		}
		m_info->m_posUnitID = unitID;
		if (unitID==m_info->m_goalUnitID) {
			m_flags = UNIT_PRESENT_FIXED;
		} else if (m_info->m_goalUnitID==INVALID_ID) {
			m_flags = UNIT_PRESENT_MOVING;
		}	else {
			m_flags = UNIT_GOAL_OTHER_MOVING;
		}
	}
}


/**
 * Return the relevant obstacle ID.
 */
ObjectID PathfindCell::getObstacleID() const
{
	return m_obstacleID;
}


/**
 * Flag this cell as an obstacle, from the given one.
 * Return true if cell was flagged.
 */
Bool PathfindCell::setTypeAsObstacle( Object *obstacle, Bool isFence, const ICoord2D &pos )
{
	noteWrite();
	if (m_type!=PathfindCell::CELL_CLEAR && m_type != PathfindCell::CELL_IMPASSABLE) {
		return false;
	}

	Bool isRubble = false;
	if (obstacle->getBodyModule() && obstacle->getBodyModule()->getDamageState() == BODY_RUBBLE)
	{
		isRubble = true;
	}

	if (isRubble) {
		m_type = PathfindCell::CELL_RUBBLE;
		m_obstacleID = INVALID_ID;
		m_obstacleIsFence = false;
		m_obstacleIsTransparent = false;
		return true;
	}

	m_type = PathfindCell::CELL_OBSTACLE;
	m_obstacleID = obstacle->getID();
	m_obstacleIsFence = isFence;
	m_obstacleIsTransparent = obstacle->isKindOf(KINDOF_CAN_SEE_THROUGH_STRUCTURE);
	return true;
}

/**
 * Flag this cell as given type.
 */
void PathfindCell::setType( CellType type )
{
	noteWrite();
	if (m_obstacleID != INVALID_ID) {
		DEBUG_ASSERTCRASH(type == PathfindCell::CELL_OBSTACLE, ("Wrong type."));
		m_type = PathfindCell::CELL_OBSTACLE;
		return;
	}
	m_type = type;
}

/**
 * Unflag this cell as an obstacle, from the given one.
 * Return true if this cell was previously flagged as an obstacle by this object.
 */
Bool PathfindCell::removeObstacle( Object *obstacle )
{
	noteWrite();
	if (m_type == PathfindCell::CELL_RUBBLE) {
		m_type = PathfindCell::CELL_CLEAR;
	}
	if (m_obstacleID != obstacle->getID()) return false;
	m_type = PathfindCell::CELL_CLEAR;
	m_obstacleID = INVALID_ID;
	m_obstacleIsFence = false;
	m_obstacleIsTransparent = false;
	return true;
}


/// put self on "open" list in ascending cost order
void PathfindCell::putOnSortedOpenList( PathfindOpenList &list )
{
	PathSearchNode &node = searchNode();
	DEBUG_ASSERTCRASH(hasInfo(), ("Has to have info."));
	DEBUG_ASSERTCRASH(node.m_closed == FALSE && node.m_open == FALSE, ("Serious error - Invalid flags. jba"));
	if (node.m_open) {
		list.remove(&node);
	}

	// mark the new cell as being on the open list
	node.m_open = true;
	node.m_closed = false;
	list.push(&node);
}

/// remove self from "open" list
void PathfindCell::removeFromOpenList( PathfindOpenList &list )
{
	PathSearchNode &node = searchNode();
	DEBUG_ASSERTCRASH(hasInfo(), ("Has to have info."));
	DEBUG_ASSERTCRASH(node.m_closed==FALSE && node.m_open==TRUE, ("Serious error - Invalid flags. jba"));
	list.remove(&node);
	node.m_open = false;
}

/// remove all cells from "open" list
Int PathfindCell::releaseOpenList( PathfindOpenList &list )
{
	Int count = 0;
	while (!list.empty()) {
		count++;
		PathSearchNode *node = list.popHead();
		DEBUG_ASSERTCRASH(node->m_closed==FALSE && node->m_open==TRUE, ("Serious error - Invalid flags. jba"));
		node->m_open = FALSE;
		node->m_cell->releaseInfo();
	}
	return count;
}

void PathfindOpenList::push( PathSearchNode *node )
{
	node->m_openSequence = ++m_sequence;
	node->m_openIndex = (Int)m_heap.size();
	m_heap.push_back(node);
	siftUp(node->m_openIndex);
}

void PathfindOpenList::remove( PathSearchNode *node )
{
	const Int index = node->m_openIndex;
	DEBUG_ASSERTCRASH(index >= 0 && index < (Int)m_heap.size() && m_heap[index] == node, ("Not on the open list."));
	PathSearchNode *last = m_heap.back();
	m_heap.pop_back();
	if (last != node) {
		m_heap[index] = last;
		last->m_openIndex = index;
		siftUp(index);
		siftDown(last->m_openIndex);
	}
	node->m_openIndex = -1;
}

PathSearchNode *PathfindOpenList::popHead()
{
	PathSearchNode *head = m_heap[0];
	remove(head);
	return head;
}

void PathfindOpenList::siftUp( Int index )
{
	PathSearchNode *node = m_heap[index];
	while (index > 0) {
		const Int parent = (index - 1) / 2;
		if (!before(node, m_heap[parent])) {
			break;
		}
		m_heap[index] = m_heap[parent];
		m_heap[index]->m_openIndex = index;
		index = parent;
	}
	m_heap[index] = node;
	node->m_openIndex = index;
}

void PathfindOpenList::siftDown( Int index )
{
	const Int count = (Int)m_heap.size();
	PathSearchNode *node = m_heap[index];
	for (;;) {
		Int child = 2*index + 1;
		if (child >= count) {
			break;
		}
		if (child + 1 < count && before(m_heap[child + 1], m_heap[child])) {
			child++;
		}
		if (!before(m_heap[child], node)) {
			break;
		}
		m_heap[index] = m_heap[child];
		m_heap[index]->m_openIndex = index;
		index = child;
	}
	m_heap[index] = node;
	node->m_openIndex = index;
}

/// remove all cells from "closed" list
Int PathfindCell::releaseClosedList( PathfindCellList &list )
{
	PathSearchContext &context = PathSearchContext::current();
	Int count = 0;
	while (list.m_head) {
		count++;
		PathfindCell *cur = list.m_head;
		PathSearchNode &curNode = context.getNode(cur->m_searchIndex);
		DEBUG_ASSERTCRASH(cur->hasInfo(), ("Has to have info."));
		DEBUG_ASSERTCRASH(curNode.m_closed==TRUE && curNode.m_open==FALSE, ("Serious error - Invalid flags. jba"));

		if (curNode.m_nextOpen) {
			list.m_head = curNode.m_nextOpen->m_cell;
		} else {
			list.reset();
		}
		curNode.m_nextOpen = nullptr;
		curNode.m_prevOpen = nullptr;
		curNode.m_closed = FALSE;
		cur->releaseInfo();
	}
	return count;
}

/// put self on "closed" list, return new list
void PathfindCell::putOnClosedList( PathfindCellList &list )
{
	PathSearchContext &context = PathSearchContext::current();
	PathSearchNode &node = context.getNode(m_searchIndex);
	DEBUG_ASSERTCRASH(hasInfo(), ("Has to have info."));
	DEBUG_ASSERTCRASH(node.m_closed==FALSE && node.m_open==FALSE, ("Serious error - Invalid flags. jba"));
	// only put on list if not already on it
	if (node.m_closed == FALSE)
	{
		node.m_closed = TRUE;

		node.m_prevOpen = nullptr;
		node.m_nextOpen = list.m_head ? &context.getNode(list.m_head->m_searchIndex) : nullptr;
		if (node.m_nextOpen)
			node.m_nextOpen->m_prevOpen = &node;

		list.m_head = this;
	}

}

/// remove self from "closed" list
void PathfindCell::removeFromClosedList( PathfindCellList &list )
{
	PathSearchNode &node = searchNode();
	DEBUG_ASSERTCRASH(hasInfo(), ("Has to have info."));
	DEBUG_ASSERTCRASH(node.m_closed==TRUE && node.m_open==FALSE, ("Serious error - Invalid flags. jba"));
	if (node.m_nextOpen)
		node.m_nextOpen->m_prevOpen = node.m_prevOpen;

	if (node.m_prevOpen)
		node.m_prevOpen->m_nextOpen = node.m_nextOpen;
	else
		list.m_head = node.m_nextOpen ? node.m_nextOpen->m_cell : nullptr;

	node.m_closed = false;
	node.m_nextOpen = nullptr;
	node.m_prevOpen = nullptr;

}

/**
 * Return true if the given object ID is registered as an obstacle in this cell
 */
Bool PathfindCell::isObstaclePresent(ObjectID objID) const
{
	if (objID != INVALID_ID && (getType() == PathfindCell::CELL_OBSTACLE))
	{
		return m_obstacleID == objID;
	}

	return false;
}


/**
 * return true if the obstacle in the cell is KINDOF_CAN_SEE_THROUGHT_STRUCTURE
 */
Bool PathfindCell::isObstacleTransparent() const
{
	return m_obstacleIsTransparent;
}

/**
 * return true if the given obstacle in the cell is a fence.
 */
Bool PathfindCell::isObstacleFence() const
{
	return m_obstacleIsFence;
}



UnsignedInt PathfindCell::costToGoal( PathfindCell *goal )
{
	DEBUG_ASSERTCRASH(hasInfo(), ("Has to have info."));
	Int dx = getXIndex() - goal->getXIndex();
	Int dy = getYIndex() - goal->getYIndex();
#define NO_REAL_DIST
#ifdef REAL_DIST
	Int cost = COST_ORTHOGONAL*sqrt(dx*dx + dy*dy);
#else
	if (dx<0) dx = -dx;
	if (dy<0) dy = -dy;
	Int cost;
	if (dx>dy) {
		cost= COST_ORTHOGONAL*dx + (COST_ORTHOGONAL*dy)/2;
	}	else {
		cost= COST_ORTHOGONAL*dy + (COST_ORTHOGONAL*dx)/2;
	}

#endif


	return cost;
}

UnsignedInt PathfindCell::costToHierGoal( PathfindCell *goal )
{
	if( !hasInfo() )
	{
		DEBUG_CRASH( ("Has to have info.") );
		return 100000; //...patch hack 1.01
	}
	Int dx = getXIndex() - goal->getXIndex();
	Int dy = getYIndex() - goal->getYIndex();
	Int cost = REAL_TO_INT_FLOOR(COST_ORTHOGONAL*sqrt(dx*dx + dy*dy) + 0.5f);
	return cost;
}

UnsignedInt PathfindCell::costSoFar( PathfindCell *parent )
{
	DEBUG_ASSERTCRASH(hasInfo(), ("Has to have info."));
	// very first node in path - no turns, no cost
	if (parent == nullptr)
		return 0;

	// add in number of turns in path so far
	ICoord2D prevDir;
	Int cost;

	prevDir.x = parent->getXIndex() - getXIndex();
	prevDir.y = parent->getYIndex() - getYIndex();

	// diagonal moves cost a bit more than orthogonal ones
	if (prevDir.x == 0 || prevDir.y == 0)
		cost = parent->getCostSoFar() + COST_ORTHOGONAL;
	else
		cost = parent->getCostSoFar() + COST_DIAGONAL;
	if (getPinched()) {
		cost += 1*COST_DIAGONAL;
	}

#if 1
	// Increase cost of turns.
	Int numTurns = 0;
	PathfindCell *prevCell = parent->getParentCell();
	if (prevCell) {


		ICoord2D dir;
		dir.x = prevCell->getXIndex() - parent->getXIndex();
		dir.y = prevCell->getYIndex() - parent->getYIndex();

		// count number of direction changes
		if (dir.x != prevDir.x || dir.y != prevDir.y)
		{
			Int dot = dir.x * prevDir.x + dir.y * prevDir.y;
			if (dot > 0)
				numTurns=4;				// 45 degree turn
			else if (dot == 0)
				numTurns = 8;		// 90 degree turn
			else
				numTurns = 16;		// 135 degree turn
		}
	}

	return cost + numTurns;
#else
	return cost;
#endif

}



/**
 * Determine absolute total path cost difference between two cells.
 * Returns UINT_MAX if used with an uninitialised cell, so will be sorted as maximally dissimilar.
 */
UnsignedInt PathfindCell::getTotalCostDifference(PathfindCell& other) const
{
	const PathSearchNode *thisNode = activeSearchNode();
	const PathSearchNode *otherNode = other.activeSearchNode();
	if (thisNode && otherNode)
		return abs((Int)thisNode->m_totalCost - (Int)otherNode->m_totalCost);

	return UINT_MAX;
}

Bool PathfindCell::typesMatch(const PathfindCell& targetCell, const PathfindCell& sourceCell)
{
	CellType targetType = targetCell.getType();
	CellType srcType = sourceCell.getType();
	if (targetType == srcType)
	{
		return true;
	}

	return false;
}

Bool PathfindCell::waterGround(const PathfindCell& targetCell, const PathfindCell& sourceCell)
{
	CellType targetType = targetCell.getType();
	CellType srcType = sourceCell.getType();
	if ((targetType == CELL_CLEAR &&
	     (srcType & CELL_WATER)))
	{
		return true;
	}
	if ((srcType == CELL_CLEAR &&
	     (targetType & CELL_WATER)))
	{
		return true;
	}

	return false;
}

Bool PathfindCell::groundRubble(const PathfindCell& targetCell, const PathfindCell& sourceCell)
{
	CellType targetType = targetCell.getType();
	CellType srcType = sourceCell.getType();
	if ((targetType == CELL_CLEAR &&
	     (srcType == CELL_RUBBLE)))
	{
		return true;
	}
	if ((srcType == CELL_CLEAR &&
	     (targetType == CELL_RUBBLE)))
	{
		return true;
	}

	return false;
}

Bool PathfindCell::terrain(const PathfindCell& targetCell, const PathfindCell& sourceCell)
{
	Int targetType = targetCell.getType();
	Int srcType = sourceCell.getType();
	if (targetType == CELL_OBSTACLE)
	{
		targetType = CELL_CLEAR;
	}
	if (srcType == CELL_OBSTACLE)
	{
		srcType = CELL_CLEAR;
	}
	if (targetType == srcType)
	{
		return true;
	}
	return false;
}

Bool PathfindCell::crusherGround(const PathfindCell& targetCell, const PathfindCell& sourceCell)
{
	Int targetType = targetCell.getType();
	Int srcType = sourceCell.getType();
	if (targetType == CELL_OBSTACLE)
	{
		if (targetCell.isObstacleFence())
		{
			if (srcType == CELL_CLEAR)
			{
				return true;
			}
		}
	}
	if (srcType == CELL_OBSTACLE)
	{
		if (sourceCell.isObstacleFence())
		{
			if (targetType == CELL_CLEAR)
			{
				return true;
			}
		}
	}
	return false;
}

Bool PathfindCell::groundCliff(const PathfindCell& targetCell, const PathfindCell& sourceCell)
{
	CellType targetType = targetCell.getType();
	CellType srcType = sourceCell.getType();

	if ((targetType == CELL_CLIFF) &&
	    (srcType == CELL_CLEAR))
	{
		return true;
	}
	if ((targetType == CELL_CLEAR) &&
	    (srcType == CELL_CLIFF))
	{
		return true;
	}
	return false;
}

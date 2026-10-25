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

#include "PathfindCellList.h"

#include <vector>

class PathfindCell;

// 1: a node lives exactly as long as the PathfindCellInfo of the original pathfinder did, including the records the original never
//    gave back and its limit of CELL_INFOS_TO_ALLOCATE records, so paths match the original bit for bit. Only the game logic thread may search.
// 0: every search starts with fresh nodes and nothing carries over from one search to the next, so searches can run on worker threads.
#ifndef PATHFIND_LEGACY_NODE_LIFETIME
#define PATHFIND_LEGACY_NODE_LIFETIME (0)
#endif

/**
 * The A* state of one pathfind cell during one search.
 * It lives in a PathSearchContext instead of in the shared pathfind map,
 * so that searches running on different threads never touch each other's state.
 */
struct PathSearchNode
{
	PathSearchNode *m_nextOpen;	///< for A* "open" list, shared by closed list
	PathSearchNode *m_prevOpen;
	PathfindCell *m_cell;				///< the cell this node belongs to
	PathfindCell *m_parent;			///< "parent" cell from pathfinder
	UnsignedInt m_stamp;				///< The node is in use when this equals the stamp of its context
	UnsignedInt m_openSequence;	///< order in which the node went onto the open list
	Int m_openIndex;						///< position in the open list heap
	UnsignedShort m_totalCost;	///< cost estimates for A* search
	UnsignedShort m_costSoFar;
	ICoord2D m_pos;							///< cell coordinates, since cells are often accessed via pointer only
	Bool m_open;								///< cell is on the open list
	Bool m_closed;							///< cell is on the closed list
	Bool m_blockedByAlly;				///< an allied unit has to move off this cell (only used when nodes do not outlive their search)
};

/**
 * The open list of a search: a binary heap ordered by total cost and, among equal costs, by the order the cells were added.
 * The original kept the open list as a list sorted the same way (a cell went behind all cells of equal cost), so the
 * head is always the same cell, but adding a cell no longer walks the list.
 */
class PathfindOpenList
{
	friend class PathfindCell;
public:
	PathfindOpenList() : m_sequence(0) {}
	void reset() { m_heap.clear(); m_sequence = 0; }
	Bool empty() const { return m_heap.empty(); }
	PathfindCell *getHead() const { return m_heap.empty() ? nullptr : m_heap[0]->m_cell; }

private:
	static __forceinline Bool before( const PathSearchNode *a, const PathSearchNode *b )
	{
		return a->m_totalCost < b->m_totalCost || (a->m_totalCost == b->m_totalCost && a->m_openSequence < b->m_openSequence);
	}
	void push( PathSearchNode *node );
	void remove( PathSearchNode *node );
	PathSearchNode *popHead();
	void siftUp( Int index );
	void siftDown( Int index );

	std::vector<PathSearchNode *> m_heap;
	UnsignedInt m_sequence;
};

/**
 * Everything a pathfind search writes while it runs: the A* nodes, the open and closed lists,
 * the zone block passable flags and a few flags that used to be members of Pathfinder.
 * The game logic thread uses the main context; a worker thread installs its own with setCurrent().
 */
class PathSearchContext
{
public:
	PathSearchContext();

	// Called for nearly every cell access of a search, so it has to stay a single thread local load.
	/// Called for nearly every cell access of a search. Outside of the parallel phase only the logic thread searches,
	/// so it does not need the thread local at all.
	static __forceinline PathSearchContext &current() { return s_parallel ? *t_current : s_main; }
	static PathSearchContext &mainContext() { return s_main; }
	static void setCurrent( PathSearchContext *context ) { t_current = context ? context : &s_main; }
	/// Set by the logic thread around the parallel searches, see Pathfinder::prefetchPaths().
	static void setParallel( Bool parallel ) { s_parallel = parallel; }

	void reset();																	///< Empty the lists and clear the flags
	void resize( UnsignedInt numCells, Int numZoneBlocks );	///< Make room for every cell and zone block of the map
	void beginSearch();														///< Release every node, so a new search starts clean

	__forceinline PathSearchNode &getNode( UnsignedInt cellIndex ) { return m_nodes[cellIndex]; }
	__forceinline Bool isNodeActive( UnsignedInt cellIndex ) const { return m_nodes[cellIndex].m_stamp == m_stamp; }
	/// Nodes one search may use: by default every cell (no limit), -pathfindNodeLimit sets a limit for testing.
	/// A limit turns long paths that exist into failed searches, and the units then park at the closest point
	/// they could reach, often the edge of a cliff (10000 nodes failed some 15000 searches on Twilight Flame).
	Bool canActivateNode( UnsignedInt cellIndex ) const { return isNodeActive(cellIndex) || m_activeNodes < m_nodeLimit; }
	PathSearchNode &activateNode( PathfindCell *cell, UnsignedInt cellIndex, const ICoord2D &pos );	///< Starts the node unless it is already in use
	PathSearchNode &resetNode( PathfindCell *cell, UnsignedInt cellIndex, const ICoord2D &pos );		///< Starts the node with fresh values
	void releaseNode( UnsignedInt cellIndex ) { m_nodes[cellIndex].m_stamp = 0; m_activeNodes--; }

	/// Records the cells a prefetched search looks at, see Pathfinder::prefetchPaths()
	void beginReadTracking( UnsignedInt numCells );
	__forceinline void noteCellRead( UnsignedInt cellIndex )
	{
		if (!m_readMarks[cellIndex]) {
			m_readMarks[cellIndex] = 1;
			m_readCells.push_back(cellIndex);
		}
	}
	void endReadTracking( std::vector<UnsignedInt> &sortedCells );

	void setAllPassable( Bool passable );
	void setPassable( Int block, Bool passable ) { m_passable[block] = passable; }
	Bool isPassable( Int block ) const { return m_passable[block] != 0; }

public:
	PathfindOpenList m_openList;									///< Cells ready to be explored
	PathfindCellList m_closedList;								///< Cells already explored
	Bool m_isTunneling;														///< True if path started in an obstacle
	ObjectID m_ignoreObstacleID;									///< Ignore the given obstacle
	Int m_cumulativeCellsAllocated;								///< Cells taken off the lists, counted against the per frame budget
	Int m_nodeLimit;															///< see canActivateNode()
	Int m_nodeLimitHits;													///< how often a search ran out of nodes, for the profile log
	Int m_nodesActivated;													///< nodes started by all searches so far, for the profile log
	Int m_hierFailure;														///< why the last coarse search failed, for the profile log: 1 setup, 2 zones, 3 search

private:
	std::vector<PathSearchNode> m_nodes;
	std::vector<UnsignedByte> m_passable;					///< One flag per zone block
	std::vector<UnsignedByte> m_readMarks;				///< One flag per cell, set while read tracking
	std::vector<UnsignedInt> m_readCells;
	UnsignedInt m_stamp;
	Int m_activeNodes;														///< nodes in use by the current search

	static PathSearchContext s_main;
	static Bool s_parallel;																	///< worker threads are searching right now
	static thread_local PathSearchContext *t_current;	///< Never null: points at s_main unless a worker installed its own context
};

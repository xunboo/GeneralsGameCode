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

class PathfindCell;

class PathfindCellInfo
{
	friend class PathfindCell;
public:
	static void allocateCellInfos();
	static void releaseCellInfos();

	static PathfindCellInfo * getACellInfo(PathfindCell *cell, const ICoord2D &pos);
	static void releaseACellInfo(PathfindCellInfo *theInfo);
	static Int getNumAllocated() {return s_numAllocated;}

protected:
	static Int s_numAllocated;
	static PathfindCellInfo *s_infoArray;
	static PathfindCellInfo *s_firstFree;							///<


	// The A* search state is in PathSearchNode, see PathSearchContext.h. This record only holds the units on the cell.

	PathfindCellInfo *m_nextFree;													///< free list link while in the pool
	PathfindCell *m_cell;															///< Cell this info belongs to currently.

	/// have to include cell's coordinates, since cells are often accessed via pointer only
	ICoord2D m_pos;

	ObjectID m_goalUnitID; ///< The objectID of the ground unit whose goal this is.
	ObjectID m_posUnitID;  ///< The objectID of the ground unit that is occupying this cell.
	ObjectID m_goalAircraftID; ///< The objectID of the aircraft whose goal this is.

	ObjectID m_obstacleID;	///< the object ID who overlaps this cell

	UnsignedInt m_isFree:1;
	UnsignedInt m_blockedByAlly:1;///< True if this cell is blocked by an allied unit.
	UnsignedInt m_obstacleIsFence:1;///< True if occupied by a fence.
	UnsignedInt m_obstacleIsTransparent:1;///< True if obstacle is transparent (undefined if obstacleid is invalid)
};

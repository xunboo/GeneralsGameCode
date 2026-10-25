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

// Where does the time of a logic frame go? Sections of GameLogic::update() are timed and, every few hundred frames, the average and
// the worst time of each are written to logic_profile.log (next to the game). Only measures, never changes the game. Setting
// LogicProfile = No in GameData.ini turns it off.

#pragma once

namespace LogicProfile
{
	enum Section
	{
		SEC_SCRIPTS,
		SEC_TERRAIN,
		SEC_COMMANDS,
		SEC_OBJECTS,		///< all object update modules
		SEC_AI,				///< AI update, includes the pathfinding below
		SEC_PATHFIND,		///< the pathfinding queue
		SEC_BUILD,
		SEC_PARTITION,
		SEC_CLEANUP,
		SEC_TOTAL,
		SEC_COUNT
	};

	void add( Section section, __int64 ticks );
	__int64 now();
	/// Called once at the end of every logic frame.
	void frameDone( UnsignedInt frame, Int objectCount );
	/// Appends a line of text to the log.
	void log( const char *text );

	class Scope
	{
	public:
		explicit Scope( Section s ) : m_section( s ), m_start( now() ) {}
		~Scope() { add( m_section, now() - m_start ); }
	private:
		Section m_section;
		__int64 m_start;
	};
}

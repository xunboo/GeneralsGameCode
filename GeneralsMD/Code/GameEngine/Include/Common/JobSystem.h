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

// A small pool of worker threads for the game logic.
//
// The game logic is lockstep: every machine must compute exactly the same result, so work is only handed to the pool in a form
// whose result cannot depend on the number of threads or on which thread finishes first:
//   * parallelFor() cuts [0,count) into chunks of 'grain' items. The chunk borders depend on count and grain only.
//   * the callback gets a chunk [begin,end), may only READ state that nobody changes during the call, and may only WRITE into
//     output slots that belong to its own chunk (an array sized before the call, indexed by item).
//   * the caller applies the outputs afterwards on the logic thread, in index order (for objects: in ObjectID order).
// The calling thread works on chunks as well, and the call returns when all chunks are done. With no workers (setting
// JobThreads = 0, or a single core machine) the chunks simply run one after another on the calling thread, with the same result.
//
// Only the logic thread may call parallelFor, and calls may not be nested.

#pragma once

#include <functional>

class JobSystem
{
public:
	/// The pool, created on first use. The number of workers comes from TheGlobalData->m_jobThreads.
	static JobSystem &instance();
	/// Stops the workers. Called when the engine shuts down.
	static void shutdown();

	/// Number of worker threads (not counting the calling thread).
	virtual Int getWorkerCount() const = 0;

	/// Runs fn(begin,end) for the chunks of [0,count). See the rules at the top of this file.
	virtual void parallelFor( Int count, Int grain, const std::function<void( Int, Int )> &fn ) = 0;

	/// Runs the same work with the pool and without it and compares the results.
	static Bool selfTest();

protected:
	virtual ~JobSystem() {}
};

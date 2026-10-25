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

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/GlobalData.h"
#include "Common/JobSystem.h"
#include "Common/LogicProfile.h"

#include "GameLogic/AI.h"
#include "GameLogic/AIPathfind.h"
#include <stdio.h>

namespace LogicProfile
{

namespace
{
	const UnsignedInt WINDOW_FRAMES = 300;	// 10 seconds of game time

	const char *const NAMES[SEC_COUNT] =
	{
		"scripts", "terrain", "commands", "objects", "ai", "pathfind", "build", "partition", "cleanup", "TOTAL"
	};

	__int64 g_sum[SEC_COUNT];
	__int64 g_max[SEC_COUNT];
	__int64 g_frame[SEC_COUNT];		// time spent in the current frame
	UnsignedInt g_framesInWindow = 0;
	Bool g_started = FALSE;

	double ticksToMs( double ticks )
	{
		static double perMs = 0.0;
		if (perMs == 0.0)
		{
			LARGE_INTEGER f;
			QueryPerformanceFrequency( &f );
			perMs = (double)f.QuadPart / 1000.0;
		}
		return ticks / perMs;
	}
}

__int64 now()
{
	LARGE_INTEGER t;
	QueryPerformanceCounter( &t );
	return t.QuadPart;
}

void log( const char *text )
{
	FILE *f = fopen( "logic_profile.log", "a" );
	if (f)
	{
		fputs( text, f );
		fputc( '\n', f );
		fclose( f );
	}
}

void add( Section section, __int64 ticks )
{
	if (TheGlobalData && !TheGlobalData->m_logicProfile)
		return;
	g_frame[section] += ticks;
}

void frameDone( UnsignedInt frame, Int objectCount )
{
	if (!TheGlobalData || !TheGlobalData->m_logicProfile)
		return;

	if (!g_started)
	{
		g_started = TRUE;
		char line[256];
		sprintf( line, "---- new game, %d worker threads, %d objects at the first frame", JobSystem::instance().getWorkerCount(), objectCount );
		log( line );
		log( JobSystem::selfTest() ? "job system self test: ok (same result with and without the pool)" : "job system self test: FAILED" );
	}

	for (Int s = 0; s < SEC_COUNT; ++s)
	{
		g_sum[s] += g_frame[s];
		if (g_frame[s] > g_max[s])
			g_max[s] = g_frame[s];
		g_frame[s] = 0;
	}
	if (++g_framesInWindow < WINDOW_FRAMES)
		return;

	char line[1024];
	int n = sprintf( line, "frame %u objects %d | ms avg/max:", (unsigned)frame, objectCount );
	for (Int s = 0; s < SEC_COUNT; ++s)
		n += sprintf( line + n, " %s %.2f/%.2f", NAMES[s], ticksToMs( (double)g_sum[s] / (double)g_framesInWindow ), ticksToMs( (double)g_max[s] ) );
	if (TheAI && TheAI->pathfinder())
	{
		Int taken, computed, recomputed;
		TheAI->pathfinder()->getPrefetchStats( taken, computed, recomputed );
		n += sprintf( line + n, " | prefetched searches computed %d taken %d recomputed %d", computed, taken, recomputed );
		Int counts[Pathfinder::SEARCH_KIND_COUNT];
		double ms[Pathfinder::SEARCH_KIND_COUNT];
		TheAI->pathfinder()->getSearchStats( counts, ms );
		const char *const kinds[Pathfinder::SEARCH_KIND_COUNT] = { "path", "closest", "attack", "patch", "safe" };
		n += sprintf( line + n, " | logic thread searches" );
		for (Int k = 0; k < Pathfinder::SEARCH_KIND_COUNT; ++k)
			n += sprintf( line + n, " %s %d/%.1fms", kinds[k], counts[k], ms[k] );
		Int bucketCounts[Pathfinder::SEARCH_TIME_BUCKETS];
		double bucketMs[Pathfinder::SEARCH_TIME_BUCKETS];
		TheAI->pathfinder()->getSearchTimeBuckets( bucketCounts, bucketMs );
		const char *const buckets[Pathfinder::SEARCH_TIME_BUCKETS] = { "<1", "1-5", "5-20", "20-50", ">50" };
		n += sprintf( line + n, " | by length" );
		for (Int b = 0; b < Pathfinder::SEARCH_TIME_BUCKETS; ++b)
			n += sprintf( line + n, " %sms %d/%.1fms", buckets[b], bucketCounts[b], bucketMs[b] );
		Int limited, failedPaths;
		TheAI->pathfinder()->getFailureStats( limited, failedPaths );
		n += sprintf( line + n, " | out of nodes %d failed paths %d", limited, failedPaths );
	}
	log( line );

	for (Int s = 0; s < SEC_COUNT; ++s)
		g_sum[s] = g_max[s] = 0;
	g_framesInWindow = 0;
}

}	// namespace LogicProfile

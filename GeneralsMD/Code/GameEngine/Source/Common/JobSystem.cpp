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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <intrin.h>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace
{

struct Batch
{
	const std::function<void( Int, Int )> *fn;
	Int count;
	Int grain;
	Int chunks;
	std::atomic<Int> next;
	std::atomic<Int> done;
	Batch() : fn( nullptr ), count( 0 ), grain( 1 ), chunks( 0 ), next( 0 ), done( 0 ) {}

	void work()
	{
		for (;;)
		{
			const Int c = next.fetch_add( 1 );
			if (c >= chunks)
				return;
			const Int begin = c * grain;
			const Int end = std::min( count, begin + grain );
			(*fn)( begin, end );
			done.fetch_add( 1 );
		}
	}
};

class Pool : public JobSystem
{
public:
	explicit Pool( Int workers ) : m_quit( false ), m_generation( 0 )
	{
		for (Int i = 0; i < workers; ++i)
			m_threads.push_back( std::thread( &Pool::workerMain, this ) );
	}

	~Pool() override
	{
		{
			std::lock_guard<std::mutex> lock( m_mutex );
			m_quit = true;
		}
		m_wake.notify_all();
		for (size_t i = 0; i < m_threads.size(); ++i)
			m_threads[i].join();
	}

	Int getWorkerCount() const override { return (Int)m_threads.size(); }

	void parallelFor( Int count, Int grain, const std::function<void( Int, Int )> &fn ) override
	{
		if (count <= 0)
			return;
		if (grain < 1)
			grain = 1;

		std::shared_ptr<Batch> batch = std::make_shared<Batch>();
		batch->fn = &fn;
		batch->count = count;
		batch->grain = grain;
		batch->chunks = (count + grain - 1) / grain;

		if (m_threads.empty() || batch->chunks == 1)
		{
			batch->work();	// nothing to share
			return;
		}

		{
			std::lock_guard<std::mutex> lock( m_mutex );
			m_batch = batch;
			m_generation.fetch_add( 1 );
		}
		m_wake.notify_all();

		batch->work();
		while (batch->done.load() < batch->chunks)
			std::this_thread::yield();

		std::lock_guard<std::mutex> lock( m_mutex );
		m_batch.reset();
	}

private:
	void workerMain()
	{
		UnsignedInt seen = 0;
		for (;;)
		{
			// Batches often come in quick succession (the pathfinder hands over a group of searches, applies the results,
			// then hands over the next group). Waking a sleeping thread takes long enough for the calling thread to do
			// most of a batch alone, so first watch for the next batch for a moment before going to sleep.
			const std::chrono::steady_clock::time_point spinUntil = std::chrono::steady_clock::now() + std::chrono::microseconds( 500 );
			while (m_generation.load() == seen && std::chrono::steady_clock::now() < spinUntil)
				_mm_pause();

			std::shared_ptr<Batch> batch;
			{
				std::unique_lock<std::mutex> lock( m_mutex );
				m_wake.wait( lock, [&] { return m_quit || m_generation != seen; } );
				if (m_quit)
					return;
				seen = m_generation;
				batch = m_batch;
			}
			if (batch)
				batch->work();
		}
	}

	std::vector<std::thread> m_threads;
	std::mutex m_mutex;
	std::condition_variable m_wake;
	std::shared_ptr<Batch> m_batch;
	Bool m_quit;
	std::atomic<UnsignedInt> m_generation;
};

Pool *g_pool = nullptr;

Int chooseWorkerCount()
{
	if (TheGlobalData && TheGlobalData->m_jobThreads >= 0)
		return std::min( TheGlobalData->m_jobThreads, 32 );
	// leave cores for the renderer, the audio and the network
	const Int cores = (Int)std::thread::hardware_concurrency();
	return std::max( 0, std::min( cores - 3, 8 ) );
}

}	// namespace

JobSystem &JobSystem::instance()
{
	if (g_pool == nullptr)
		g_pool = new Pool( chooseWorkerCount() );
	return *g_pool;
}

void JobSystem::shutdown()
{
	delete g_pool;
	g_pool = nullptr;
}

Bool JobSystem::selfTest()
{
	const Int N = 200000;
	std::vector<UnsignedInt> a( N ), b( N );
	auto work = []( std::vector<UnsignedInt> &out ) -> std::function<void( Int, Int )>
	{
		return [&out]( Int begin, Int end )
		{
			for (Int i = begin; i < end; ++i)
			{
				UnsignedInt h = (UnsignedInt)i * 2654435761u;
				for (Int k = 0; k < 20; ++k)
					h = (h ^ (h >> 13)) * 0x5bd1e995u;
				out[i] = h;
			}
		};
	};
	work( a )( 0, N );								// one thread
	instance().parallelFor( N, 1000, work( b ) );	// the pool
	return a == b;
}

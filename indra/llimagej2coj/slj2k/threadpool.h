/**
 * @file threadpool.h
 *
 * slj2k is Charis Viewer's own JPEG 2000 decoder, written from ITU-T T.800; it
 * holds no OpenJPEG source code. Where it follows OpenJPEG (constants and order
 * of operations, so that the pixels agree), the comments say so
 * (licenses-charis.txt, section 1).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

// Minimal fork-join thread pool: parallelFor(n, fn) runs fn(i) for i in [0, n) on the workers
// plus the calling thread and returns when all are done.  fn must not throw (it also runs on the
// workers).
//
// The workers are detached and share the pool's state, so destroying the pool never waits for a
// thread: the viewer keeps one pool in a thread_local of each decode thread, and its destructor
// runs while that thread ends, under the loader lock, where waiting for another thread to end
// would hang (that thread needs the lock to end).  The workers see `stop` and finish by
// themselves.
#pragma once
#include <atomic>
#include <condition_variable>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>

namespace slj2k
{

class ThreadPool
{
public:
    explicit ThreadPool(int workers) : mState(std::make_shared<State>())
    {
        for (int i = 0; i < workers; i++)
        {
            try
            {
                std::thread([s = mState] { s->loop(); }).detach();
            }
            catch (const std::exception&)  // system_error, or bad_alloc for the thread's arguments
            {
                break;  // fewer workers; the calling thread still runs whatever they do not take
            }
            mWorkers++;
        }
    }
    ~ThreadPool()
    {
        {
            std::lock_guard<std::mutex> l(mState->mutex);
            mState->stop = true;
        }
        mState->wake.notify_all();
    }
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    int size() const { return mWorkers + 1; }

    template <class F>
    void parallelFor(int n, F&& fn)
    {
        if (n <= 0) return;
        if (mWorkers == 0 || n == 1)
        {
            for (int i = 0; i < n; i++) fn(i);
            return;
        }
        State& s = *mState;
        std::lock_guard<std::mutex> serial(s.submit);  // one job at a time
        Job job;
        job.n = n;
        job.fn = [&fn](int i) { fn(i); };
        {
            std::lock_guard<std::mutex> l(s.mutex);
            s.job = &job;
            s.active = 0;
        }
        s.wake.notify_all();
        State::run(job);
        {
            std::unique_lock<std::mutex> l(s.mutex);
            s.job = nullptr;
            s.done.wait(l, [&] { return s.active == 0; });
        }
    }

private:
    struct Job
    {
        int n = 0;
        std::atomic<int> next{0};
        std::function<void(int)> fn;
    };

    struct State
    {
        std::mutex mutex, submit;
        std::condition_variable wake, done;
        Job* job = nullptr;
        int active = 0;
        bool stop = false;

        static void run(Job& job)
        {
            for (;;)
            {
                int i = job.next.fetch_add(1);
                if (i >= job.n) return;
                job.fn(i);
            }
        }

        void loop()
        {
            for (;;)
            {
                Job* j;
                {
                    std::unique_lock<std::mutex> l(mutex);
                    wake.wait(l, [&] { return stop || (job && job->next.load() < job->n); });
                    if (stop) return;
                    j = job;
                    active++;
                }
                run(*j);
                {
                    std::lock_guard<std::mutex> l(mutex);
                    active--;
                }
                done.notify_all();
            }
        }
    };

    std::shared_ptr<State> mState;
    int mWorkers = 0;
};

}  // namespace slj2k

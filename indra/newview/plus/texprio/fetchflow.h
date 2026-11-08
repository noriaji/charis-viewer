/**
 * @file fetchflow.h
 * @brief Where a texture waits between "asked for" and "created": counters (research/294 tex-fetch-flow)
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

#ifndef PLUS_TEXPRIO_FETCHFLOW_H
#define PLUS_TEXPRIO_FETCHFLOW_H

#include "stdtypes.h"

class LLImageRaw;
class LLViewerFetchedTexture;

// research/294 in the sl-viewer repository. Counts, over the same window as
// plus/texprio's "texture view" line (LLRHI_TEX_VIEW_STATS), where the
// fetch pipeline holds a texture: the time each worker spends in each of its
// states (cache read, waiting for an HTTP slot, the request in flight, the
// decode queue and the decode), how often the fetch thread comes back to a
// waiting worker, the queues' lengths, the HTTP slots all taken, the decode
// pool's wait and run times, and the two hand-offs to the main thread (decoded
// image taken, texture created). The workers' classes are told from their
// fetch priority (plus/texprio's bands, plus/selffirst's own textures).
//
// Nothing here changes what is fetched or in which order; it only counts.
// Every function but the main thread's ones below is safe on any thread
// (relaxed atomics); each is a few adds. LLRHI_TEX_FLOW=0, or
// LLRHI_TEX_VIEW_STATS=0 (no log line to read them), stops all of it, the decode
// pool's timing too (read once).
namespace plus::texprio::flow
{
    // A fetch worker (LLTextureFetchWorker::setState): it left `state` (its
    // e_state) after `seconds`, with this fetch priority. (Not INIT: that
    // state ends within microseconds; noteFirstLook has its wait.)
    void noteStateLeft(S32 state, F32 seconds, F32 priority);

    // A worker's first doWork() after it was asked for (its INIT): `seconds`
    // since its state timer was last reset: a new worker's creation, or the
    // request that took it up again (setState(INIT) resets the timer). A
    // priority raised while it still sits in INIT resets the timer too, so the
    // wait of a worker raised in INIT shows shorter than it was.
    void noteFirstLook(F32 seconds, F32 priority);

    // LLTextureFetchWorker::doWork() (fetch thread) around one call: the cost
    // of the outermost call (doWork calls itself to go on in the next state:
    // only the outermost counts) and, for a worker in a waiting state, the time
    // since its previous outermost call (last_poll is the worker's own stamp,
    // updated here; state is its state on entry).
    class DoWorkScope
    {
    public:
        DoWorkScope(S32 state, U64& last_poll);
        ~DoWorkScope();
        DoWorkScope(const DoWorkScope&) = delete;
        DoWorkScope& operator=(const DoWorkScope&) = delete;

    private:
        U64 mStamp = 0;         // 0: not the outermost call, or not counting
        bool mEntered = false;  // counted in the thread's depth
    };

    // The fetcher's queues, sampled by the fetch thread about twice a second
    // (gaugesDue() says when).
    struct Gauges
    {
        S32 work_queue = 0;     // the fetch thread's own queue (workers to come back to and commands)
        S32 http_in_flight = 0; // mHttpSemaphore
        S32 http_high = 0;      // its high water
        S32 http_waiters = 0;   // workers waiting for a slot
        S32 decode_pending = 0; // the decode pool's queue
        S32 cache_pending = 0;  // the texture cache thread's queue
        S32 cache_reads = 0;    // its reads and writes outstanding (not yet taken back by the fetch workers)
        S32 cache_writes = 0;
    };
    bool gaugesDue();
    void noteGauges(const Gauges& gauges);

    // A worker was sent to wait for an HTTP slot (others already waiting, the
    // agent-first hold, or all the slots taken); slots_all_taken: they were
    // all taken as it was sent (a count within the first).
    void noteHttpWaited(bool slots_all_taken);
    // An HTTP request ended (callbackHttpGet): bytes got, whether it worked,
    // and how many bytes it asked for, taken before the callback changes it
    // (a first look at the header is FIRST_PACKET_SIZE).
    void noteHttpDone(S32 bytes, bool ok, S32 requested_bytes);

    // research/306 (P1): a worker of a cacheable texture is about to read the cache. missing: the index has no entry
    // for it, so no read is made (the switch is plus::texprio::cacheMissSkipOn). Counted by the priority's class (own,
    // near, view, off). A worker whose next step was wanted while its write was out is told with noteWriteWait (seconds
    // from the write's start to its end: an upper bound of what the next step waited).
    void noteCacheRead(F32 priority, bool missing);
    void noteWriteWait(F32 seconds);

    // The texture cache thread (LLTextureCacheWorker::doWork, one call = one request's read or write, in whole
    // or one stage): the stamp from cacheOpStart (0: not counting), then cacheOpEnd with it.
    U64 cacheOpStart();
    void cacheOpEnd(U64 start_stamp, bool write);
    // research/306 (diagnostics): where the time of one cache request goes, by stage (the cache thread's index look-up
    // and the wait for the header mutex in it, the header record of texture.cache, the body file, the fast cache),
    // for reads and writes. cacheStageStart/cacheStageEnd as above. A stage's time includes the wait for the header
    // mutex taken in it, which is also counted by itself (every take, with the time it waited). The stages' calls are
    // not alike (a read's index look-up also counts misses, header record and body only hits, the fast cache only
    // new entries, and a write's fast cache is inside its index): sum calls x avg of the stages (the fast cache
    // apart) to compare with the cache thread's calls x avg.
    enum CacheStage : U32
    {
        STAGE_INDEX = 0,    // the entry in texture.entries (look-up, read, create, update), the lock wait included
        STAGE_HEADER,       // the 600-byte record in texture.cache
        STAGE_BODY,         // the body file <UUID>.texture (stat, open, read or write, close)
        STAGE_FAST,         // the fast cache (writes only)
        STAGE_COUNT
    };
    U64 cacheStageStart();
    void cacheStageEnd(U64 start_stamp, CacheStage stage, bool write);
    void cacheLockWaitEnd(U64 start_stamp);     // mHeaderMutex taken: the time it waited (the threads that take it through lockHeaders / getHeaderCacheEntry), from cacheStageStart
    class CacheStageScope
    {
    public:
        CacheStageScope(CacheStage stage, bool write) : mStamp(cacheStageStart()), mStage(stage), mWrite(write) {}
        ~CacheStageScope() { cacheStageEnd(mStamp, mStage, mWrite); }
        CacheStageScope(const CacheStageScope&) = delete;
        CacheStageScope& operator=(const CacheStageScope&) = delete;

    private:
        U64 mStamp;
        CacheStage mStage;
        bool mWrite;
    };
    class CacheOpScope
    {
    public:
        explicit CacheOpScope(bool write) : mStamp(cacheOpStart()), mWrite(write) {}
        ~CacheOpScope() { cacheOpEnd(mStamp, mWrite); }
        CacheOpScope(const CacheOpScope&) = delete;
        CacheOpScope& operator=(const CacheOpScope&) = delete;

    private:
        U64 mStamp;
        bool mWrite;
    };

    // The decode finished (stamp from nowStamp(), 0: not set) and the main
    // thread took the image: the wait between them. The stamp is cleared.
    U64 nowStamp();
    void noteDecodedTaken(U64& decoded_stamp, F32 priority);

    // The decode of a texture finished from no more than its first bytes (the 600 bytes): the image is looked at
    // for colour (research/302: whether what stays on screen is a nearly monochrome picture). Any thread.
    void noteFirstBytesImage(const LLImageRaw* raw);

    // Main thread. A texture's decoded image was queued to be made into a GL
    // texture (scheduleCreateTexture) and made (postCreateTexture), and the
    // length of the queue as the frame's creation starts.
    void noteCreateQueued(LLViewerFetchedTexture* tex);
    void noteCreated(LLViewerFetchedTexture* tex);
    void noteCreateQueue(size_t length);

    // plus/texprio's logStats (main thread): the lines for the window that
    // ended, then the counters start again. Nothing when not counting.
    void logWindow(F32 seconds);
}

#endif // PLUS_TEXPRIO_FETCHFLOW_H

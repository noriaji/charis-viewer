/**
 * @file fetchflow.cpp
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

#include "llviewerprecompiledheaders.h"

#include "plus.h"

#include "fetchflow.h"
#include "texprio.h"

#include "llimage.h"
#include "llimageworker.h"
#include "llrhi.h"
#include "llviewertexture.h"
#include "plus/selffirst/selffirst.h"

#include <atomic>
#include <chrono>
#include <sstream>

namespace plus::texprio::flow
{
namespace
{
    // The states of LLTextureFetchWorker (its e_state, 0..13).
    constexpr S32 STATE_COUNT = 14;
    constexpr S32 STATE_INIT = 1;
    constexpr S32 STATE_CACHE_READ = 2;
    constexpr S32 STATE_CACHE_POST = 3;
    constexpr S32 STATE_WAIT_SLOT = 6;      // WAIT_HTTP_RESOURCE2
    constexpr S32 STATE_HTTP = 8;           // WAIT_HTTP_REQ
    constexpr S32 STATE_DECODE_GATE = 9;    // DECODE_IMAGE (plusDecodeMayStart's wait)
    constexpr S32 STATE_DECODE = 10;        // DECODE_IMAGE_UPDATE
    constexpr S32 STATE_WAIT_WRITE = 12;    // WAIT_ON_WRITE

    // What the log line tells apart, in this order.
    struct StateLabel
    {
        S32 state;
        const char* name;
    };
    constexpr StateLabel SHOWN[] = {
        { STATE_INIT, "to first look" },
        { STATE_CACHE_READ, "cache read" },
        { STATE_CACHE_POST, "after cache read" },
        { STATE_WAIT_SLOT, "wait for an HTTP slot" },
        { STATE_HTTP, "HTTP request" },
        { STATE_DECODE_GATE, "decode gate (memory)" },
        { STATE_DECODE, "decode queue and decode" },
        { STATE_WAIT_WRITE, "cache write" },
    };

    // The classes: the agent's own textures (plus/selffirst), then the fetch
    // priority's bands (plus/texprio).
    enum Cls : U32
    {
        CLS_SELF = 0,
        CLS_NEAR,
        CLS_VIEW,
        CLS_OFF,
        CLS_COUNT
    };
    const char* const CLS_NAMES[CLS_COUNT] = { "own", "near", "view", "off" };

    // Waits for the hand-offs and the polls: under 0.05 s / 0.2 / 1 / 3 / more.
    constexpr U32 LAT_BUCKETS = 5;
    constexpr F64 LAT_LIMITS[LAT_BUCKETS - 1] = { 0.05, 0.2, 1.0, 3.0 };
    constexpr F64 SLOW_SECONDS = 3.0;
    constexpr U32 MONOCHROME_DIFFERENCE = 6;    // the mean spread of red, green and blue (of 255) under which a picture is nearly monochrome
    constexpr F64 GAUGE_SECONDS = 0.5;

    // Counting needs the log line to read it: not with LLRHI_TEX_FLOW=0 or LLRHI_TEX_VIEW_STATS=0. The decode pool
    // (llimage) is told once, by the first call.
    bool counting()
    {
        static const bool on = []()
        {
            const bool value = LLRHI::envDefaultOn("LLRHI_TEX_FLOW") && statsSeconds() > 0;
            LLImageDecodeThread::sStats.on.store(value, std::memory_order_relaxed);
            return value;
        }();
        return on;
    }

    using Clock = std::chrono::steady_clock;

    U64 stampNow()
    {
        return (U64)std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
    }

    U32 classOf(F32 priority)
    {
        if (plus::selffirst::isOwnPriority(priority))
        {
            return CLS_SELF;
        }
        const U8 tier = tierOfPriority(priority);
        return tier == TIER_NEAR ? CLS_NEAR : tier == TIER_VIEW ? CLS_VIEW : CLS_OFF;
    }

    U32 latencyBucket(F64 seconds)
    {
        U32 bucket = 0;
        while (bucket < LAT_BUCKETS - 1 && seconds >= LAT_LIMITS[bucket])
        {
            ++bucket;
        }
        return bucket;
    }

    void raiseMax(std::atomic<U64>& slot, U64 value)
    {
        U64 seen = slot.load(std::memory_order_relaxed);
        while (value > seen && !slot.compare_exchange_weak(seen, value, std::memory_order_relaxed))
        {
        }
    }

    U64 microseconds(F64 seconds)
    {
        return seconds > 0.0 ? (U64)(seconds * 1.0e6) : 0;
    }

    struct StateSum
    {
        std::atomic<U64> count{ 0 };
        std::atomic<U64> micros{ 0 };
        std::atomic<U64> slow{ 0 };         // left after SLOW_SECONDS or more
        std::atomic<U64> max_micros{ 0 };
    };

    struct Latency
    {
        std::atomic<U32> buckets[LAT_BUCKETS] = {};
    };

    // One gauge: its sum over the samples and its largest.
    struct Gauge
    {
        std::atomic<U64> sum{ 0 };
        std::atomic<U64> max{ 0 };
        void add(S32 value)
        {
            const U64 v = value > 0 ? (U64)value : 0;
            sum.fetch_add(v, std::memory_order_relaxed);
            raiseMax(max, v);
        }
    };

    thread_local S32 tDoWorkDepth = 0;      // doWork() calls itself to go on in the next state

    StateSum sStates[STATE_COUNT][CLS_COUNT];
    Latency sTaken[CLS_COUNT];      // decoded image to the main thread taking it
    Latency sCreated[CLS_COUNT];    // queued for creation to created (main thread)
    Latency sPoll;                  // a waiting worker's calls to doWork, start to start
    std::atomic<U64> sDoWorkCalls{ 0 };
    std::atomic<U64> sDoWorkMicros{ 0 };
    std::atomic<U64> sDoWorkMaxMicros{ 0 };

    Gauge sGaugeWork, sGaugeHttp, sGaugeWaiters, sGaugeDecode, sGaugeCache;
    std::atomic<U64> sGaugeSamples{ 0 };
    std::atomic<U64> sGaugeHttpAtHigh{ 0 };     // samples with every slot taken
    std::atomic<S32> sGaugeHighWater{ 0 };
    std::atomic<U64> sGaugeLast{ 0 };           // stamp of the last sample

    // The texture cache thread's calls: [0] reads, [1] writes
    std::atomic<U64> sCacheOps[2] = {};
    std::atomic<U64> sCacheMicros[2] = {};
    std::atomic<U64> sCacheMaxMicros[2] = {};
    Gauge sGaugeCacheReads, sGaugeCacheWrites;
    // research/306 (diagnostics): the cache request's time by stage, [read, write][stage], and the header mutex's waits
    std::atomic<U64> sStageCalls[2][STAGE_COUNT] = {};
    std::atomic<U64> sStageMicros[2][STAGE_COUNT] = {};
    std::atomic<U64> sStageMaxMicros[2][STAGE_COUNT] = {};
    std::atomic<U64> sLockWaits{ 0 };
    std::atomic<U64> sLockWaitMicros{ 0 };
    std::atomic<U64> sLockWaitMaxMicros{ 0 };
    Gauge sGaugeCacheOther;     // research/306: the cache thread's queue less its reads and writes: the update lambdas, at least
    // research/306 (P1): cache reads asked for by class (sent) and misses not sent, a second step that waited for the
    // first step's write
    std::atomic<U64> sReadsSent[CLS_COUNT] = {};
    std::atomic<U64> sReadsSkipped[CLS_COUNT] = {};
    std::atomic<U64> sWriteWaits{ 0 };
    std::atomic<U64> sWriteWaitMicros{ 0 };
    std::atomic<U64> sWriteWaitMaxMicros{ 0 };

    std::atomic<U64> sFirstBytesImages{ 0 };    // decoded from the first bytes only
    std::atomic<U64> sFirstBytesMono{ 0 };      // ... and nearly monochrome
    std::atomic<U64> sHttpWaited{ 0 };
    std::atomic<U64> sHttpFull{ 0 };
    std::atomic<U64> sHttpOk{ 0 };
    std::atomic<U64> sHttpFailed{ 0 };
    std::atomic<U64> sHttpProbes{ 0 };          // requests of no more than FIRST_PACKET_SIZE
    std::atomic<U64> sHttpBytes{ 0 };

    // Main thread only.
    U64 sCreateFrames = 0;
    U64 sCreateLengthSum = 0;
    U64 sCreateLengthMax = 0;

    std::string latencyText(const std::atomic<U32>* buckets)
    {
        return llformat("%u/%u/%u/%u/%u", buckets[0].load(), buckets[1].load(), buckets[2].load(), buckets[3].load(),
                        buckets[4].load());
    }

    void clearLatency(Latency& latency)
    {
        for (U32 b = 0; b < LAT_BUCKETS; ++b)
        {
            latency.buckets[b].store(0, std::memory_order_relaxed);
        }
    }

    // "n/avg s/max s/n over 3 s" for one state in one class, then the counters
    // for it start again; "-" when nothing left that state.
    std::string stateText(S32 state, U32 cls)
    {
        StateSum& sum = sStates[state][cls];
        const U64 count = sum.count.exchange(0, std::memory_order_relaxed);
        const U64 micros = sum.micros.exchange(0, std::memory_order_relaxed);
        const U64 slow = sum.slow.exchange(0, std::memory_order_relaxed);
        const U64 max_micros = sum.max_micros.exchange(0, std::memory_order_relaxed);
        if (!count)
        {
            return "-";
        }
        return llformat("%llu/%.2f/%.1f/%llu", (unsigned long long)count, 1.0e-6 * (F64)micros / (F64)count,
                        1.0e-6 * (F64)max_micros, (unsigned long long)slow);
    }

    std::string gaugeText(Gauge& gauge, U64 samples)
    {
        const U64 sum = gauge.sum.exchange(0, std::memory_order_relaxed);
        const U64 max = gauge.max.exchange(0, std::memory_order_relaxed);
        return llformat("%.0f/%llu", samples ? (F64)sum / (F64)samples : 0.0, (unsigned long long)max);
    }
}

namespace
{
    void addToState(S32 state, F32 seconds, F32 priority)
    {
        StateSum& sum = sStates[state][classOf(priority)];
        const U64 micros = microseconds(seconds);
        sum.count.fetch_add(1, std::memory_order_relaxed);
        sum.micros.fetch_add(micros, std::memory_order_relaxed);
        if (seconds >= SLOW_SECONDS)
        {
            sum.slow.fetch_add(1, std::memory_order_relaxed);
        }
        raiseMax(sum.max_micros, micros);
    }
}

void noteStateLeft(S32 state, F32 seconds, F32 priority)
{
    if (!counting() || state < 0 || state >= STATE_COUNT || state == STATE_INIT)
    {
        return;
    }
    addToState(state, seconds, priority);
}

void noteFirstLook(F32 seconds, F32 priority)
{
    if (counting())
    {
        addToState(STATE_INIT, seconds, priority);
    }
}

DoWorkScope::DoWorkScope(S32 state, U64& last_poll)
{
    if (!counting())
    {
        return;
    }
    mEntered = true;
    if (tDoWorkDepth++ > 0)
    {
        return;     // doWork() went on into the next state by itself: one call
    }
    const U64 now = stampNow();
    const bool waiting = state == STATE_CACHE_READ || state == STATE_CACHE_POST || state == STATE_WAIT_SLOT ||
                         state == STATE_HTTP || state == STATE_DECODE_GATE || state == STATE_DECODE ||
                         state == STATE_WAIT_WRITE;
    if (waiting && last_poll && now >= last_poll)
    {
        const F64 gap = 1.0e-9 * (F64)(now - last_poll);
        sPoll.buckets[latencyBucket(gap)].fetch_add(1, std::memory_order_relaxed);
    }
    last_poll = now;
    mStamp = now;
}

DoWorkScope::~DoWorkScope()
{
    if (!mEntered)
    {
        return;
    }
    --tDoWorkDepth;
    if (!mStamp)
    {
        return;
    }
    const U64 end = stampNow();
    const U64 micros = end >= mStamp ? (end - mStamp) / 1000 : 0;
    sDoWorkCalls.fetch_add(1, std::memory_order_relaxed);
    sDoWorkMicros.fetch_add(micros, std::memory_order_relaxed);
    raiseMax(sDoWorkMaxMicros, micros);
}

bool gaugesDue()
{
    if (!counting())
    {
        return false;
    }
    const U64 now = stampNow();
    const U64 last = sGaugeLast.load(std::memory_order_relaxed);
    if (now >= last && now - last < (U64)(GAUGE_SECONDS * 1.0e9))
    {
        return false;
    }
    sGaugeLast.store(now, std::memory_order_relaxed);
    return true;
}

void noteCacheRead(F32 priority, bool missing)
{
    if (!counting())
    {
        return;
    }
    (missing ? sReadsSkipped : sReadsSent)[classOf(priority)].fetch_add(1, std::memory_order_relaxed);
}

void noteWriteWait(F32 seconds)
{
    if (!counting() || seconds < 0.f)
    {
        return;
    }
    const U64 micros = microseconds(seconds);
    sWriteWaits.fetch_add(1, std::memory_order_relaxed);
    sWriteWaitMicros.fetch_add(micros, std::memory_order_relaxed);
    raiseMax(sWriteWaitMaxMicros, micros);
}

void noteGauges(const Gauges& gauges)
{
    sGaugeSamples.fetch_add(1, std::memory_order_relaxed);
    sGaugeWork.add(gauges.work_queue);
    sGaugeHttp.add(gauges.http_in_flight);
    sGaugeWaiters.add(gauges.http_waiters);
    sGaugeDecode.add(gauges.decode_pending);
    sGaugeCache.add(gauges.cache_pending);
    sGaugeCacheReads.add(gauges.cache_reads);
    sGaugeCacheWrites.add(gauges.cache_writes);
    sGaugeCacheOther.add(llmax(gauges.cache_pending - gauges.cache_reads - gauges.cache_writes, 0));
    sGaugeHighWater.store(gauges.http_high, std::memory_order_relaxed);
    if (gauges.http_high > 0 && gauges.http_in_flight >= gauges.http_high)
    {
        sGaugeHttpAtHigh.fetch_add(1, std::memory_order_relaxed);
    }
}

void noteHttpWaited(bool slots_all_taken)
{
    if (counting())
    {
        sHttpWaited.fetch_add(1, std::memory_order_relaxed);
        if (slots_all_taken)
        {
            sHttpFull.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void noteHttpDone(S32 bytes, bool ok, S32 requested_bytes)
{
    if (!counting())
    {
        return;
    }
    (ok ? sHttpOk : sHttpFailed).fetch_add(1, std::memory_order_relaxed);
    if (requested_bytes > 0 && requested_bytes <= FIRST_PACKET_SIZE)
    {
        sHttpProbes.fetch_add(1, std::memory_order_relaxed);
    }
    if (bytes > 0)
    {
        sHttpBytes.fetch_add((U64)bytes, std::memory_order_relaxed);
    }
}

U64 cacheStageStart()
{
    return counting() ? stampNow() : 0;
}

void cacheStageEnd(U64 start_stamp, CacheStage stage, bool write)
{
    if (!start_stamp)
    {
        return;
    }
    const U64 end = stampNow();
    const U64 micros = end >= start_stamp ? (end - start_stamp) / 1000 : 0;
    sStageCalls[write ? 1 : 0][stage].fetch_add(1, std::memory_order_relaxed);
    sStageMicros[write ? 1 : 0][stage].fetch_add(micros, std::memory_order_relaxed);
    raiseMax(sStageMaxMicros[write ? 1 : 0][stage], micros);
}

void cacheLockWaitEnd(U64 start_stamp)
{
    if (!start_stamp)
    {
        return;
    }
    const U64 end = stampNow();
    const U64 micros = end >= start_stamp ? (end - start_stamp) / 1000 : 0;
    sLockWaits.fetch_add(1, std::memory_order_relaxed);
    sLockWaitMicros.fetch_add(micros, std::memory_order_relaxed);
    raiseMax(sLockWaitMaxMicros, micros);
}

U64 cacheOpStart()
{
    return counting() ? stampNow() : 0;
}

void cacheOpEnd(U64 start_stamp, bool write)
{
    if (!start_stamp)
    {
        return;
    }
    const U64 end = stampNow();
    const U64 micros = end >= start_stamp ? (end - start_stamp) / 1000 : 0;
    const U32 kind = write ? 1 : 0;
    sCacheOps[kind].fetch_add(1, std::memory_order_relaxed);
    sCacheMicros[kind].fetch_add(micros, std::memory_order_relaxed);
    raiseMax(sCacheMaxMicros[kind], micros);
}

void noteFirstBytesImage(const LLImageRaw* raw)
{
    if (!counting() || !raw || !raw->getData() || raw->getComponents() < 3 || raw->getWidth() <= 0 || raw->getHeight() <= 0)
    {
        return;
    }
    sFirstBytesImages.fetch_add(1, std::memory_order_relaxed);
    const S32 pixels = raw->getWidth() * raw->getHeight();
    const S32 components = raw->getComponents();
    const S32 step = llmax(1, (pixels + 63) / 64);     // at most 64 points
    const U8* data = raw->getData();
    U64 difference = 0;
    U32 looked = 0;
    for (S32 i = 0; i < pixels; i += step)
    {
        const U8* p = data + (size_t)i * components;
        const U8 high = llmax(p[0], llmax(p[1], p[2]));
        const U8 low = llmin(p[0], llmin(p[1], p[2]));
        difference += high - low;
        ++looked;
    }
    if (looked && difference < (U64)looked * MONOCHROME_DIFFERENCE)
    {
        sFirstBytesMono.fetch_add(1, std::memory_order_relaxed);
    }
}

U64 nowStamp()
{
    return counting() ? stampNow() : 0;
}

void noteDecodedTaken(U64& decoded_stamp, F32 priority)
{
    if (!decoded_stamp)
    {
        return;
    }
    const U64 now = stampNow();
    if (counting() && now >= decoded_stamp)
    {
        const F64 wait = 1.0e-9 * (F64)(now - decoded_stamp);
        sTaken[classOf(priority)].buckets[latencyBucket(wait)].fetch_add(1, std::memory_order_relaxed);
    }
    decoded_stamp = 0;
}

void noteCreateQueued(LLViewerFetchedTexture* tex)
{
    if (counting())
    {
        tex->mRhiCreateQueuedAt = LLTimer::getTotalSeconds();
    }
}

void noteCreated(LLViewerFetchedTexture* tex)
{
    if (tex->mRhiCreateQueuedAt <= 0.0)
    {
        return;
    }
    const F64 wait = LLTimer::getTotalSeconds() - tex->mRhiCreateQueuedAt;
    tex->mRhiCreateQueuedAt = 0.0;
    if (counting() && wait >= 0.0)
    {
        // By the texture's view tier, boosted ones (UI, HUD, selected...) with the near ones as in the fetch
        // priority; "own" (the agent's avatar) is not told apart here
        const U32 cls = tex->getBoostLevel() >= LLGLTexture::BOOST_HIGH ? CLS_NEAR
                        : tex->mRhiViewTier == TIER_NEAR                ? CLS_NEAR
                        : tex->mRhiViewTier == TIER_VIEW                ? CLS_VIEW
                                                                        : CLS_OFF;
        sCreated[cls].buckets[latencyBucket(wait)].fetch_add(1, std::memory_order_relaxed);
    }
}

void noteCreateQueue(size_t length)
{
    if (!counting())
    {
        return;
    }
    ++sCreateFrames;
    sCreateLengthSum += length;
    sCreateLengthMax = llmax<U64>(sCreateLengthMax, length);
}

void logWindow(F32 seconds)
{
    if (!counting())
    {
        return;
    }
    const U64 samples = sGaugeSamples.exchange(0, std::memory_order_relaxed);
    std::ostringstream out;
    out << llformat("texture fetch flow (research/294) over %.0f s. Time in each state of the fetch workers, "
                    "left/avg s/max s/left after 3 s or more, by the fetch priority's band [own | near | view | off screen]"
                    "%s: ",
                    seconds, fetchOn() ? "" : " (the bands mean nothing with the fetch switch off: PlusTexViewPriority)");
    for (const StateLabel& label : SHOWN)
    {
        out << label.name << " [";
        for (U32 cls = 0; cls < CLS_COUNT; ++cls)
        {
            out << (cls ? " | " : "") << stateText(label.state, cls);
        }
        out << "]; ";
    }
    // The states not shown are cleared too, so a window starts clean.
    for (S32 state = 0; state < STATE_COUNT; ++state)
    {
        for (U32 cls = 0; cls < CLS_COUNT; ++cls)
        {
            StateSum& sum = sStates[state][cls];
            sum.count.store(0, std::memory_order_relaxed);
            sum.micros.store(0, std::memory_order_relaxed);
            sum.slow.store(0, std::memory_order_relaxed);
            sum.max_micros.store(0, std::memory_order_relaxed);
        }
    }
    out << "decoded to taken by the main thread, under 0.05/0.2/1/3/more s: ";
    for (U32 cls = 0; cls < CLS_COUNT; ++cls)
    {
        out << (cls ? ", " : "") << CLS_NAMES[cls] << " " << latencyText(sTaken[cls].buckets);
        clearLatency(sTaken[cls]);
    }
    out << "; queued for creation to created, same buckets, by the texture's view tier (boosted with near; own is not told "
           "apart): ";
    for (U32 cls = 0; cls < CLS_COUNT; ++cls)
    {
        out << (cls ? ", " : "") << CLS_NAMES[cls] << " " << latencyText(sCreated[cls].buckets);
        clearLatency(sCreated[cls]);
    }
    LL_INFOS("TextureMemory") << out.str() << LL_ENDL;

    // The queues and the threads.
    std::ostringstream flow;
    const U64 decode_count = LLImageDecodeThread::sStats.count.exchange(0, std::memory_order_relaxed);
    const U64 decode_wait = LLImageDecodeThread::sStats.wait_micros.exchange(0, std::memory_order_relaxed);
    const U64 decode_run = LLImageDecodeThread::sStats.run_micros.exchange(0, std::memory_order_relaxed);
    const U64 decode_wait_max = LLImageDecodeThread::sStats.wait_max_micros.exchange(0, std::memory_order_relaxed);
    const U64 decode_run_max = LLImageDecodeThread::sStats.run_max_micros.exchange(0, std::memory_order_relaxed);
    const U32 decode_threads = LLImageDecodeThread::sStats.threads.load(std::memory_order_relaxed);
    const U64 calls = sDoWorkCalls.exchange(0, std::memory_order_relaxed);
    const U64 call_micros = sDoWorkMicros.exchange(0, std::memory_order_relaxed);
    const U64 call_max = sDoWorkMaxMicros.exchange(0, std::memory_order_relaxed);
    const U64 http_ok = sHttpOk.exchange(0, std::memory_order_relaxed);
    const U64 http_failed = sHttpFailed.exchange(0, std::memory_order_relaxed);
    const U64 http_probes = sHttpProbes.exchange(0, std::memory_order_relaxed);
    const U64 http_bytes = sHttpBytes.exchange(0, std::memory_order_relaxed);
    const U64 http_full = sHttpFull.exchange(0, std::memory_order_relaxed);
    const U64 http_waited = sHttpWaited.exchange(0, std::memory_order_relaxed);
    const U64 at_high = sGaugeHttpAtHigh.exchange(0, std::memory_order_relaxed);
    const F64 window = llmax((F64)seconds, 0.001);
    flow << llformat("texture fetch flow (research/294) queues, avg/max over %llu samples: fetch thread's queue %s, "
                     "HTTP in flight %s (high water %d, all taken in %llu samples), workers sent to wait for a slot %llu "
                     "(%llu of them with all the slots taken), workers waiting for a slot %s, decode pool queue %s, cache thread queue %s; ",
                     (unsigned long long)samples, gaugeText(sGaugeWork, samples).c_str(),
                     gaugeText(sGaugeHttp, samples).c_str(), sGaugeHighWater.load(std::memory_order_relaxed),
                     (unsigned long long)at_high, (unsigned long long)http_waited, (unsigned long long)http_full,
                     gaugeText(sGaugeWaiters, samples).c_str(),
                     gaugeText(sGaugeDecode, samples).c_str(), gaugeText(sGaugeCache, samples).c_str());
    flow << llformat("HTTP requests ended %llu (failed %llu, of no more than %d bytes %llu), %.1f/s, %.2f MB/s; ",
                     (unsigned long long)(http_ok + http_failed), (unsigned long long)http_failed, FIRST_PACKET_SIZE,
                     (unsigned long long)http_probes, (F64)(http_ok + http_failed) / window,
                     (F64)http_bytes / (1024.0 * 1024.0) / window);
    flow << llformat("decodes %llu on %u threads, wait in the queue avg %.3f s max %.2f s, run avg %.3f s max %.2f s, "
                     "threads busy %.1f; ",
                     (unsigned long long)decode_count, decode_threads,
                     decode_count ? 1.0e-6 * (F64)decode_wait / (F64)decode_count : 0.0, 1.0e-6 * (F64)decode_wait_max,
                     decode_count ? 1.0e-6 * (F64)decode_run / (F64)decode_count : 0.0, 1.0e-6 * (F64)decode_run_max,
                     1.0e-6 * (F64)decode_run / window);
    flow << llformat("fetch thread: doWork calls %llu, avg %.1f us max %.1f ms, busy %.0f%%; "
                     "a waiting worker's calls apart under 0.05/0.2/1/3/more s: %s; ",
                     (unsigned long long)calls, calls ? (F64)call_micros / (F64)calls : 0.0, 1.0e-3 * (F64)call_max,
                     100.0 * 1.0e-6 * (F64)call_micros / window, latencyText(sPoll.buckets).c_str());
    clearLatency(sPoll);
    {
        const char* const names[2] = { "reads", "writes" };
        U64 busy_micros = 0;
        flow << "texture cache thread: ";
        for (U32 kind = 0; kind < 2; ++kind)
        {
            const U64 ops = sCacheOps[kind].exchange(0, std::memory_order_relaxed);
            const U64 micros = sCacheMicros[kind].exchange(0, std::memory_order_relaxed);
            const U64 max_micros = sCacheMaxMicros[kind].exchange(0, std::memory_order_relaxed);
            busy_micros += micros;
            flow << llformat("%s %llu calls, avg %.2f ms max %.1f ms, outstanding %s; ", names[kind], (unsigned long long)ops,
                             ops ? 1.0e-3 * (F64)micros / (F64)ops : 0.0, 1.0e-3 * (F64)max_micros,
                             gaugeText(kind ? sGaugeCacheWrites : sGaugeCacheReads, samples).c_str());
        }
        flow << llformat("busy %.0f%%; ", 100.0 * 1.0e-6 * (F64)busy_micros / window);
    }
    {
        // research/306 (P1): the reads asked for (sent to the cache thread / the index had no entry: not sent), by class
        flow << "cache reads (sent/not sent as the index has no entry) ";
        for (U32 cls = 0; cls < CLS_COUNT; ++cls)
        {
            flow << llformat("%s %llu/%llu%s", CLS_NAMES[cls], (unsigned long long)sReadsSent[cls].exchange(0, std::memory_order_relaxed),
                             (unsigned long long)sReadsSkipped[cls].exchange(0, std::memory_order_relaxed), cls + 1 < CLS_COUNT ? ", " : "");
        }
        const U64 waits = sWriteWaits.exchange(0, std::memory_order_relaxed);
        const U64 wait_micros = sWriteWaitMicros.exchange(0, std::memory_order_relaxed);
        flow << llformat("; cache thread queue besides reads and writes (update lambdas, at least: running and finished ones not yet taken back are taken off) %s; a next step was wanted while a write was out %llu times, the write took avg %.2f s max %.2f s (an upper bound of the wait); ",
                         gaugeText(sGaugeCacheOther, samples).c_str(), (unsigned long long)waits,
                         waits ? 1.0e-6 * (F64)wait_micros / (F64)waits : 0.0,
                         1.0e-6 * (F64)sWriteWaitMaxMicros.exchange(0, std::memory_order_relaxed));
    }
    {
        // research/306 (diagnostics): one cache request's time by stage (calls, avg ms, max ms), and the waits for the header mutex
        const char* const stage_names[STAGE_COUNT] = { "index", "header record", "body file", "fast cache" };
        flow << "cache request by stage: ";
        for (U32 kind = 0; kind < 2; ++kind)
        {
            flow << (kind ? "write {" : "read {");
            bool any = false;
            for (U32 stage = 0; stage < STAGE_COUNT; ++stage)
            {
                const U64 calls = sStageCalls[kind][stage].exchange(0, std::memory_order_relaxed);
                const U64 micros = sStageMicros[kind][stage].exchange(0, std::memory_order_relaxed);
                const U64 max_micros = sStageMaxMicros[kind][stage].exchange(0, std::memory_order_relaxed);
                if (calls)
                {
                    flow << llformat("%s%s %llu avg %.2f max %.1f ms", any ? ", " : "", stage_names[stage], (unsigned long long)calls,
                                     1.0e-3 * (F64)micros / (F64)calls, 1.0e-3 * (F64)max_micros);
                    any = true;
                }
            }
            flow << (any ? "}; " : "-}; ");
        }
        const U64 lock_waits = sLockWaits.exchange(0, std::memory_order_relaxed);
        const U64 lock_micros = sLockWaitMicros.exchange(0, std::memory_order_relaxed);
        flow << llformat("header mutex taken %llu times, the wait avg %.3f ms max %.1f ms; ", (unsigned long long)lock_waits,
                         lock_waits ? 1.0e-3 * (F64)lock_micros / (F64)lock_waits : 0.0,
                         1.0e-3 * (F64)sLockWaitMaxMicros.exchange(0, std::memory_order_relaxed));
    }
    flow << llformat("colour images decoded from 600 bytes or less %llu, nearly monochrome %llu (research/302); ",
                     (unsigned long long)sFirstBytesImages.exchange(0, std::memory_order_relaxed),
                     (unsigned long long)sFirstBytesMono.exchange(0, std::memory_order_relaxed));
    flow << llformat("creation queue at the start of a frame's creation, avg %.0f max %llu over %llu frames",
                     sCreateFrames ? (F64)sCreateLengthSum / (F64)sCreateFrames : 0.0, (unsigned long long)sCreateLengthMax,
                     (unsigned long long)sCreateFrames);
    sCreateFrames = 0;
    sCreateLengthSum = 0;
    sCreateLengthMax = 0;
    LL_INFOS("TextureMemory") << flow.str() << LL_ENDL;
}
}

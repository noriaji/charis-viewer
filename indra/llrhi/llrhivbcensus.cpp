/**
 * @file llrhivbcensus.cpp
 * @brief Counts for the vertex buffer double-hold work (research/293 branch 0, LLRHI_VB_CENSUS=1).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Charis Viewer Source Code
 * Copyright (C) 2026, Charis Viewer contributors
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

#include "linden_common.h"

#include "llrhivbcensus.h"

#include "llglheaders.h"
#include "llrhi.h"
#include "llrhiuploadtrace.h"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <vector>

bool LLRHIVBCensus::sOn = false;
std::atomic<bool> LLRHIVBCensus::sSnapshot(false);
std::atomic<U64> LLRHIVBCensus::detail::sCounters[LLRHIVBCensus::COUNTER_COUNT];

namespace
{
    using namespace LLRHIVBCensus;

    constexpr U32 IDLE_SECONDS[3] = { 2, 10, 60 };      // research/293 6.2 (g)
    constexpr U64 REPORT_INTERVAL_MS = 55000;
    constexpr U64 SNAPSHOT_INTERVAL_MS = 1000;           // the peak breakdown is taken at most this often
    constexpr U32 NOINDEX_LIMITS[4] = { 64, 256, 1024, 4096 };

    // A buffer live now, by its number.
    struct Rec
    {
        bool live = false;
        Shape shape;
        U32 last_draw_ms = 0;       // the last draw asked for (the birth, until one)
        U32 draws = 0;              // draws asked for in the window
        U8 refused = 0;             // bits of Refusal seen in the window
        bool index_by_data = false; // the indices were written by setIndexData: mMappedIndexData is not a copy of them
    };

    struct MaskAgg
    {
        U32 mask = 0;
        U64 count = 0;
        U64 vertices = 0;
        U64 vertex_bytes = 0;
        U64 index_bytes = 0;
        U64 draws = 0;
    };

    std::mutex sMutex;
    std::vector<Rec> sRecs;                 // by number
    std::vector<U32> sTypeSizes;            // bytes per vertex of each AttributeType bit
    std::atomic<U64> sReads[READ_COUNT];
    U64 sLiveBytes = 0;
    U64 sLiveCount = 0;
    U64 sPeakBytes = 0;
    U64 sPeakCount = 0;
    U64 sSnapBytes = 0;                     // the peak the breakdown below belongs to
    U64 sSnapCount = 0;
    U32 sSnapAtMs = 0;
    bool sSnapPending = false;              // the live bytes rose 5% past the last breakdown and it is not taken yet
    void (*sSetOwner)(U32 gl_name, U32 number) = nullptr;
    void (*sClearOwner)(U32 gl_name, U32 number) = nullptr;
    std::vector<MaskAgg> sSnapMasks;        // top masks by bytes at that peak
    U32 sRunLastMask = 0;
    U32 sRunCurDraws = 0;
    bool sRunSeen = false;
    U64 sRunHist[5] = {};                   // draws per run: 1, 2-3, 4-7, 8-15, 16+
    U64 sRunMax = 0;
    U32 sLastReportMs = 0;
    bool sReportedOnce = false;

    U32 now_ms()
    {
        using namespace std::chrono;
        static const steady_clock::time_point start = steady_clock::now();
        return (U32)duration_cast<milliseconds>(steady_clock::now() - start).count();
    }

    Rec& rec_at(U32 number)
    {
        if (number >= sRecs.size())
        {
            sRecs.resize(std::max((size_t)number + 1, sRecs.size() * 2));      // doubling: not per allocation
        }
        return sRecs[number];
    }

    U32 per_vertex_bytes(U32 mask)
    {
        U32 bytes = 0;
        for (U32 i = 0; i < sTypeSizes.size() && i < 32; ++i)
        {
            if (mask & (1u << i))
            {
                bytes += sTypeSizes[i];
            }
        }
        return bytes;
    }

    std::string mb(U64 bytes)
    {
        std::ostringstream out;
        out << std::fixed << std::setprecision(1) << (double)bytes / (1024.0 * 1024.0);
        return out.str();
    }

    std::string hex(U32 value)
    {
        std::ostringstream out;
        out << "0x" << std::hex << value;
        return out.str();
    }

    // Under sMutex: the masks of the live buffers (count, vertices, bytes).
    void aggregate_locked(std::map<U32, MaskAgg>& out)
    {
        for (const Rec& r : sRecs)
        {
            if (!r.live)
            {
                continue;
            }
            MaskAgg& a = out[r.shape.type_mask];
            a.mask = r.shape.type_mask;
            ++a.count;
            a.vertices += r.shape.vertices;
            a.vertex_bytes += r.shape.vertex_bytes;
            a.index_bytes += r.shape.index_bytes;
        }
    }

    // Under sMutex: remember the top masks (by bytes) of the live buffers as the peak's breakdown.
    void snapshot_locked()
    {
        std::map<U32, MaskAgg> masks;
        aggregate_locked(masks);
        sSnapMasks.clear();
        for (const auto& m : masks)
        {
            sSnapMasks.push_back(m.second);
        }
        std::sort(sSnapMasks.begin(), sSnapMasks.end(), [](const MaskAgg& a, const MaskAgg& b)
        {
            return a.vertex_bytes + a.index_bytes > b.vertex_bytes + b.index_bytes;
        });
        if (sSnapMasks.size() > 6)
        {
            sSnapMasks.resize(6);
        }
        sSnapBytes = sLiveBytes;
        sSnapCount = sLiveCount;
        sSnapAtMs = now_ms();
        sSnapPending = false;
    }

    // Desk calculation of how much room a stream per vertex type leaves unused when the
    // buffers of several type masks share one family (research/293 2.2 item 2): the
    // vertices of the buffers times the bytes of the family's types they do not have.
    // `kept` buffers' masks stay a family of their own; the rest go into the smallest
    // kept mask that has all their types, else into one family of the rest.
    struct Waste
    {
        U32 families = 0;
        double ratio = 0.0;     // unused / (used + unused)
    };

    Waste family_waste(const std::vector<MaskAgg>& masks, U32 kept_count)
    {
        std::vector<const MaskAgg*> order;
        for (const MaskAgg& m : masks)
        {
            order.push_back(&m);
        }
        std::sort(order.begin(), order.end(), [](const MaskAgg* a, const MaskAgg* b)
        {
            return a->vertices * per_vertex_bytes(a->mask) > b->vertices * per_vertex_bytes(b->mask);
        });
        const size_t kept = std::min((size_t)kept_count, order.size());
        double used = 0.0, unused = 0.0;
        U32 rest_mask = 0;
        std::vector<const MaskAgg*> rest;
        for (size_t i = 0; i < order.size(); ++i)
        {
            const MaskAgg& m = *order[i];
            used += (double)m.vertices * per_vertex_bytes(m.mask);
            if (i < kept)
            {
                continue;
            }
            const MaskAgg* home = nullptr;
            for (size_t k = 0; k < kept; ++k)
            {
                const MaskAgg& c = *order[k];
                if ((c.mask & m.mask) == m.mask && (!home || per_vertex_bytes(c.mask) < per_vertex_bytes(home->mask)))
                {
                    home = &c;
                }
            }
            if (home)
            {
                unused += (double)m.vertices * (per_vertex_bytes(home->mask) - per_vertex_bytes(m.mask));
            }
            else
            {
                rest.push_back(&m);
                rest_mask |= m.mask;
            }
        }
        for (const MaskAgg* m : rest)
        {
            unused += (double)m->vertices * (per_vertex_bytes(rest_mask) - per_vertex_bytes(m->mask));
        }
        Waste out;
        out.families = (U32)kept + (rest.empty() ? 0 : 1);
        out.ratio = used + unused > 0.0 ? unused / (used + unused) : 0.0;
        return out;
    }

    const char* const COUNTER_NAMES[COUNTER_COUNT] = {
        "allocs", "allocs without indices", "first allocs", "vertices only again (indices kept)", "indices only again (vertices kept)",
        "both again", "vertices shrunk", "kept index past the new vertex count", "kept index copy unknown (setIndexData)",
        "index type to 32 bit (partial)", "index type to 32 bit (whole)",
        "writes", "write bytes", "setXData bytes", "index bytes written",
        "pool moved", "pool write without recording", "pool write without room in the ring", "pool write over the frame budget",
        "snapshot allocs", "snapshot writes", "snapshot write bytes", "snapshot moved",
        "pool ranges taken", "pool ranges failed",
        "draws of numbered buffers", "runs opened", "run draws", "run draws with another type mask", "scene buckets",
        "refused draws", "fan or line loop draws with indices (CPU copy)",
    };
    static_assert(sizeof(COUNTER_NAMES) / sizeof(COUNTER_NAMES[0]) == COUNTER_COUNT, "a name for every counter");
    const char* const READ_NAMES[READ_COUNT] = {
        "gpu_buffer", "gpu_buffer_named (cached; drawBuffer reads through it)", "bufferContents", "sObjectVersions", "buffer_changed",
        "sBuffers.find of the data (a CPU copy of the indices read)", "sBuffers.find for the size or existence of the data",
    };
    const char* const REFUSAL_NAMES[REFUSE_COUNT] = {
        "no program", "unbuilt", "shape", "descriptor unit", "not direct", "no shader", "no buffer",
    };
}

void LLRHIVBCensus::init()
{
    sOn = LLRHI::envSet("LLRHI_VB_CENSUS");
    if (sOn)
    {
        now_ms();   // the clock's zero
        LL_INFOS("VBCensus") << "vertex buffer census on (LLRHI_VB_CENSUS, research/293): counting only, a report about every minute" << LL_ENDL;
    }
}

void LLRHIVBCensus::setOwnerHooks(void (*set_owner)(U32 gl_name, U32 number), void (*clear_owner)(U32 gl_name, U32 number))
{
    sSetOwner = set_owner;
    sClearOwner = clear_owner;
}

void LLRHIVBCensus::setTypeSizes(const U32* sizes, U32 count)
{
    std::lock_guard<std::mutex> lock(sMutex);
    if (sTypeSizes.empty() && sizes)
    {
        sTypeSizes.assign(sizes, sizes + count);
    }
}

void LLRHIVBCensus::layout(U32 number, const Shape& now)
{
    if (!number)
    {
        return;
    }
    U32 old_vertex_name = 0, old_index_name = 0;
    {
        std::lock_guard<std::mutex> lock(sMutex);
        Rec& r = rec_at(number);
        if (r.live)
        {
            old_vertex_name = r.shape.vertex_name;
            old_index_name = r.shape.index_name;
            sLiveBytes -= (U64)r.shape.vertex_bytes + r.shape.index_bytes;
        }
        else
        {
            r = Rec();
            r.live = true;
            r.last_draw_ms = now_ms();
            ++sLiveCount;
        }
        r.shape = now;
        sLiveBytes += (U64)now.vertex_bytes + now.index_bytes;
        if (sLiveBytes > sPeakBytes)
        {
            sPeakBytes = sLiveBytes;
            sPeakCount = sLiveCount;
            // A new peak: its breakdown is taken when it rose by 5% since the last one; within a second of it, at the next call.
            if (sPeakBytes > sSnapBytes + sSnapBytes / 20)
            {
                sSnapPending = true;
            }
        }
        if (sSnapPending && sLiveBytes >= sPeakBytes - sPeakBytes / 20 && now_ms() - sSnapAtMs >= SNAPSHOT_INTERVAL_MS)
        {
            snapshot_locked();
        }
    }
    // The owner marks are the tracker's (its lock is taken there, not under ours).
    if (sSetOwner && sClearOwner)
    {
        if (old_vertex_name != now.vertex_name)
        {
            if (old_vertex_name) sClearOwner(old_vertex_name, number);
            if (now.vertex_name) sSetOwner(now.vertex_name, number);
        }
        else if (now.vertex_name)
        {
            sSetOwner(now.vertex_name, number);     // a new buffer's first allocation, or the mark was never set (no Buffer yet)
        }
        if (old_index_name != now.index_name)
        {
            if (old_index_name) sClearOwner(old_index_name, number);
            if (now.index_name) sSetOwner(now.index_name, number);
        }
        else if (now.index_name)
        {
            sSetOwner(now.index_name, number);
        }
    }
}

void LLRHIVBCensus::release(U32 number)
{
    if (!number)
    {
        return;
    }
    U32 vertex_name = 0, index_name = 0;
    {
        std::lock_guard<std::mutex> lock(sMutex);
        if (number >= sRecs.size() || !sRecs[number].live)
        {
            return;
        }
        Rec& r = sRecs[number];
        vertex_name = r.shape.vertex_name;
        index_name = r.shape.index_name;
        sLiveBytes -= (U64)r.shape.vertex_bytes + r.shape.index_bytes;
        --sLiveCount;
        r = Rec();
    }
    if (sClearOwner)
    {
        if (vertex_name) sClearOwner(vertex_name, number);
        if (index_name) sClearOwner(index_name, number);
    }
}

void LLRHIVBCensus::indexSetByData(U32 number)
{
    std::lock_guard<std::mutex> lock(sMutex);
    if (number && number < sRecs.size() && sRecs[number].live)
    {
        sRecs[number].index_by_data = true;
    }
}

bool LLRHIVBCensus::indexCopyUnknown(U32 number)
{
    std::lock_guard<std::mutex> lock(sMutex);
    return number && number < sRecs.size() && sRecs[number].live && sRecs[number].index_by_data;
}

void LLRHIVBCensus::drawn(U32 number)
{
    if (!number)
    {
        return;
    }
    count(C_DRAWS);
    std::lock_guard<std::mutex> lock(sMutex);
    if (number < sRecs.size() && sRecs[number].live)
    {
        Rec& r = sRecs[number];
        ++r.draws;
        r.last_draw_ms = now_ms();
    }
}

void LLRHIVBCensus::refused(U32 number, Refusal why)
{
    count(C_REFUSED_DRAWS);
    if (!number)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(sMutex);
    if (number < sRecs.size() && sRecs[number].live)
    {
        sRecs[number].refused |= (U8)(1u << why);
    }
}

void LLRHIVBCensus::runDraw(U32 number, bool opened)
{
    std::lock_guard<std::mutex> lock(sMutex);
    const U32 mask = number < sRecs.size() && sRecs[number].live ? sRecs[number].shape.type_mask : 0;
    if (opened)
    {
        if (sRunSeen)
        {
            const U32 n = sRunCurDraws;
            ++sRunHist[n <= 1 ? 0 : n <= 3 ? 1 : n <= 7 ? 2 : n <= 15 ? 3 : 4];
            sRunMax = std::max<U64>(sRunMax, n);
        }
        sRunSeen = true;
        sRunCurDraws = 0;
        count(C_RUNS_OPENED);
    }
    else if (mask != sRunLastMask)
    {
        count(C_RUN_MASK_BREAKS);
    }
    ++sRunCurDraws;
    count(C_RUN_DRAWS);
    sRunLastMask = mask;
}

void LLRHIVBCensus::nameRead(Read kind, U32 number)
{
    (void)number;
    sReads[kind].fetch_add(1, std::memory_order_relaxed);
    // One read in 64 leaves its caller's function names (a stack walk is dear).
    static thread_local U32 sTick = 0;
    if ((++sTick & 63u) == 0)
    {
        LLRHIUploadTrace::record(64, LLRHIUploadTrace::NAME_READS);
    }
}

void LLRHIVBCensus::report()
{
    U32 window_ms = 0;
    {
        std::lock_guard<std::mutex> lock(sMutex);
        const U32 at = now_ms();
        if (sReportedOnce && at - sLastReportMs < REPORT_INTERVAL_MS)
        {
            return;
        }
        window_ms = at - sLastReportMs;    // the first window is the whole start
        sLastReportMs = at;
        sReportedOnce = true;
    }
    U64 counters[COUNTER_COUNT];
    for (U32 i = 0; i < COUNTER_COUNT; ++i)
    {
        counters[i] = detail::sCounters[i].exchange(0, std::memory_order_relaxed);
    }
    U64 reads[READ_COUNT];
    for (U32 i = 0; i < READ_COUNT; ++i)
    {
        reads[i] = sReads[i].exchange(0, std::memory_order_relaxed);
    }

    // The records, once, under the lock.
    std::map<U32, MaskAgg> masks;
    U64 refused_vbs[REFUSE_COUNT] = {};
    U64 idle_bytes[3] = {}, idle_count[3] = {};
    U64 noindex[5] = {};
    U64 index16 = 0, index32 = 0, live_count = 0, live_bytes = 0, peak_bytes = 0, peak_count = 0, snap_bytes = 0, snap_count = 0;
    U64 runs[5] = {}, run_max = 0;
    std::vector<MaskAgg> snap_masks;
    {
        std::lock_guard<std::mutex> lock(sMutex);
        const U32 now = now_ms();      // under the lock: no draw sets a later last_draw_ms than this
        for (Rec& r : sRecs)
        {
            if (!r.live)
            {
                continue;
            }
            ++live_count;
            const U64 bytes = (U64)r.shape.vertex_bytes + r.shape.index_bytes;
            live_bytes += bytes;
            MaskAgg& a = masks[r.shape.type_mask];
            a.mask = r.shape.type_mask;
            ++a.count;
            a.vertices += r.shape.vertices;
            a.vertex_bytes += r.shape.vertex_bytes;
            a.index_bytes += r.shape.index_bytes;
            a.draws += r.draws;
            if (r.shape.index_type == GL_UNSIGNED_INT) index32 += r.shape.index_bytes; else index16 += r.shape.index_bytes;
            if (!r.shape.indices)
            {
                U32 k = 0;
                while (k < 4 && r.shape.vertices > NOINDEX_LIMITS[k]) ++k;
                ++noindex[k];
            }
            for (U32 w = 0; w < REFUSE_COUNT; ++w)
            {
                if (r.refused & (1u << w))
                {
                    ++refused_vbs[w];
                }
            }
            for (U32 t = 0; t < 3; ++t)
            {
                if (now > r.last_draw_ms && now - r.last_draw_ms >= IDLE_SECONDS[t] * 1000u)
                {
                    idle_bytes[t] += bytes;
                    ++idle_count[t];
                }
            }
            r.draws = 0;
            r.refused = 0;
        }
        peak_bytes = sPeakBytes;
        peak_count = sPeakCount;
        snap_bytes = sSnapBytes;
        snap_count = sSnapCount;
        snap_masks = sSnapMasks;
        for (U32 i = 0; i < 5; ++i)
        {
            runs[i] = sRunHist[i];
            sRunHist[i] = 0;
        }
        run_max = sRunMax;
        sRunMax = 0;
    }

    // ---- The lines ----
    std::ostringstream out;
    out << "vertex buffer census (research/293), window " << window_ms / 1000 << " s: live " << live_count << " buffers " << mb(live_bytes) << " MB (indices 16 bit " << mb(index16)
        << " MB, 32 bit " << mb(index32) << " MB; 16 bit indices would take " << mb(index16 * 2) << " MB as 32 bit)"
        << ", peak " << mb(peak_bytes) << " MB (" << peak_count << " buffers) since the start";
    LL_INFOS("VBCensus") << out.str() << LL_ENDL;

    out.str("");
    out << "window counts:";
    for (U32 i = 0; i < COUNTER_COUNT; ++i)
    {
        out << (i ? ", " : " ") << COUNTER_NAMES[i] << " " << counters[i];
    }
    LL_INFOS("VBCensus") << out.str() << LL_ENDL;

    out.str("");
    out << "reads by GL name of buffers with a number:";
    for (U32 i = 0; i < READ_COUNT; ++i)
    {
        out << (i ? ", " : " ") << READ_NAMES[i] << " " << reads[i];
    }
    LL_INFOS("VBCensus") << out.str() << LL_ENDL;

    out.str("");
    out << "buffers whose draw was not taken in the window, by reason:";
    for (U32 i = 0; i < REFUSE_COUNT; ++i)
    {
        out << (i ? ", " : " ") << REFUSAL_NAMES[i] << " " << refused_vbs[i];
    }
    LL_INFOS("VBCensus") << out.str() << LL_ENDL;

    out.str("");
    out << "buffers without indices by vertices: <=64 " << noindex[0] << ", <=256 " << noindex[1] << ", <=1024 " << noindex[2] << ", <=4096 "
        << noindex[3] << ", more " << noindex[4] << "; draws per run: 1 " << runs[0] << ", 2-3 " << runs[1] << ", 4-7 " << runs[2] << ", 8-15 "
        << runs[3] << ", 16+ " << runs[4] << " (longest " << run_max << ")";
    LL_INFOS("VBCensus") << out.str() << LL_ENDL;

    out.str("");
    out << "idle (not drawn for at least) 2 s: " << idle_count[0] << " buffers " << mb(idle_bytes[0]) << " MB, 10 s: " << idle_count[1] << " "
        << mb(idle_bytes[1]) << " MB, 60 s: " << idle_count[2] << " " << mb(idle_bytes[2]) << " MB (draws that do not pass drawNative or a draw list are not counted as drawn: the scene's, and those by name from llvknative and llvkgpudriven (drawBuffer); so these figures are an upper bound there)";
    LL_INFOS("VBCensus") << out.str() << LL_ENDL;

    // By type mask: the shapes the families would be made of.
    std::vector<MaskAgg> by_mask;
    for (const auto& m : masks)
    {
        by_mask.push_back(m.second);
    }
    std::sort(by_mask.begin(), by_mask.end(), [](const MaskAgg& a, const MaskAgg& b)
    {
        return a.vertex_bytes + a.index_bytes > b.vertex_bytes + b.index_bytes;
    });
    out.str("");
    out << "type masks live " << by_mask.size() << ", top by bytes (mask: buffers, vertices, MB, draws in the window):";
    for (size_t i = 0; i < by_mask.size() && i < 10; ++i)
    {
        const MaskAgg& a = by_mask[i];
        out << " [" << hex(a.mask) << ": " << a.count << ", " << a.vertices << ", " << mb(a.vertex_bytes + a.index_bytes) << ", " << a.draws << "]";
    }
    LL_INFOS("VBCensus") << out.str() << LL_ENDL;

    if (!by_mask.empty())
    {
        const Waste all = family_waste(by_mask, 1), top8 = family_waste(by_mask, 8);
        out.str("");
        out << "desk estimate of unused stream room if buffers share families (vertices x the family's types they lack): one family of the "
            << "largest mask then the rest " << all.families << " famil" << (all.families == 1 ? "y " : "ies ") << std::fixed << std::setprecision(1)
            << all.ratio * 100.0 << "%, the 8 largest masks apart and the rest merged " << top8.families << " families " << top8.ratio * 100.0
            << "%, one per mask " << by_mask.size() << " families 0%";
        LL_INFOS("VBCensus") << out.str() << LL_ENDL;
    }

    out.str("");
    out << "peak " << mb(snap_bytes) << " MB (" << snap_count << " buffers), top masks then:";
    for (const MaskAgg& a : snap_masks)
    {
        out << " [" << hex(a.mask) << ": " << a.count << ", " << a.vertices << ", " << mb(a.vertex_bytes + a.index_bytes) << "]";
    }
    LL_INFOS("VBCensus") << out.str() << LL_ENDL;

    LLRHIUploadTrace::reportChannel(LLRHIUploadTrace::NAME_READS, 1);
}

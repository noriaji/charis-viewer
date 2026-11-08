/**
 * @file llrhiocclusion.cpp
 * @brief Occlusion boxes in one batch (research/137 N8): the switch, the hooks
 *        and the viewer side's batch.
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

#include "llrhiocclusion.h"

#include "llrhi.h"
#include "llrhigl.h"

#include <atomic>
#include <cstdlib>

U32 LLRHI::Occlusion::sMode = []()
{
    U64 value = 1;
    LLRHI::envNumber("LLRHI_OCCLUSION_BATCH", 0, ~(U64)0, value);
    return value > 0 ? 1u : 0u;
}();
std::atomic<U32> LLRHI::Occlusion::sStoreMode{ []()
{
    U64 value = 1;      // STORE_DEVICE
    LLRHI::envNumber("LLRHI_OCCLUSION_STORE", 0, ~(U64)0, value);
    return value == 0 ? (U32)LLRHI::Occlusion::STORE_HOST : value >= 2 ? (U32)LLRHI::Occlusion::STORE_VERIFY : (U32)LLRHI::Occlusion::STORE_DEVICE;
}() };
bool (*LLRHI::sOcclusionBoxesReady)(LLRHI::Occlusion::Context&) = nullptr;
bool (*LLRHI::sOcclusionBoxes)(const LLRHI::Occlusion::List&) = nullptr;
void (*LLRHI::sOcclusionResult)(U32, U32*, U32*) = nullptr;

using namespace LLRHI::Occlusion;

namespace
{
    // The viewer's side: set during the viewer's static initialization
    // (Batch::setViewer), so kept in a function's static, made on first use.
    Viewer& viewer_side()
    {
        static Viewer viewer;
        return viewer;
    }
    Stats sStats;

    // GL_QUERY_RESULT / GL_QUERY_RESULT_AVAILABLE
    constexpr U32 QUERY_RESULT = 0x8866;
    constexpr U32 QUERY_RESULT_AVAILABLE = 0x8867;

    // The result sOcclusionResult gave with the calling thread's last
    // GL_QUERY_RESULT_AVAILABLE (getQueryObjectuiv): valid for its next call
    // only, and only for a GL_QUERY_RESULT of the same name. Per thread: the
    // octree walks may read queries off the main thread.
    struct KeptResult
    {
        bool valid = false;
        U32 name = 0;
        U32 result = 0;
    };
    thread_local KeptResult tKept;
    std::atomic<U32> sResults{ 0 };
    std::atomic<U32> sResultsKept{ 0 };

    bool wanted()
    {
        const Viewer& v = viewer_side();
        return sMode != 0 && LLRHI::sOcclusionBoxes && LLRHI::sOcclusionBoxesReady && v.prepare && v.old_path && v.cube.positions &&
               v.cube.indices && v.cube.fan_size;
    }
}

// static
Batch& Batch::get()
{
    static Batch batch;
    return batch;
}

// static
void Batch::setViewer(const Viewer& viewer)
{
    viewer_side() = viewer;
}

bool Batch::open(const char* name)
{
    if (mOpen)
    {
        submit();   // batches do not nest
    }
    tKept.valid = false;
    if (!wanted())
    {
        return false;
    }
    Context context;
    if (!LLRHI::sOcclusionBoxesReady(context))
    {
        ++sStats.not_ready;
        return false;
    }
    mBoxes.clear();
    mOpen = true;
    mNeedReady = false;
    mReady = true;
    mName = name ? name : "";
    mScope = LLRHI::sOcclusionScope;
    mContext = context;
    ++sStats.batches;
    return true;
}

bool Batch::askReady()
{
    if (mNeedReady)
    {
        // Something drew the old way since the readiness was given.
        mNeedReady = false;
        mReady = LLRHI::sOcclusionBoxesReady && LLRHI::sOcclusionBoxesReady(mContext);
        if (!mReady)
        {
            ++sStats.not_ready;
        }
    }
    return mReady;
}

bool Batch::add(const Box& box)
{
    if (!mOpen)
    {
        return false;
    }
    tKept.valid = false;
    if (LLRHI::sOcclusionScope != mScope)
    {
        // A list's boxes share one scope (the backend counts by it).
        send(false);
        mScope = LLRHI::sOcclusionScope;
    }
    if (!askReady())
    {
        return false;
    }
    mBoxes.push_back(box);
    return true;
}

void Batch::cut()
{
    if (!mOpen)
    {
        return;
    }
    tKept.valid = false;
    send(true);
    mNeedReady = true;
}

void Batch::submit()
{
    if (!mOpen)
    {
        return;
    }
    tKept.valid = false;
    send(false);
    mOpen = false;
}

void Batch::send(bool cut)
{
    if (mBoxes.empty())
    {
        return;
    }
    struct Clear
    {
        std::vector<Box>& boxes;
        ~Clear() { boxes.clear(); }     // also when something throws: never submitted twice
    } clear{ mBoxes };
    // What a box's drawRange does before its draw (promise 3).
    viewer_side().prepare();
    List list;
    list.name = mName;
    list.boxes = mBoxes.data();
    list.box_count = (U32)mBoxes.size();
    list.cube = viewer_side().cube;
    list.scope = mScope;
    list.stamp = mContext.stamp;
    if (LLRHI::sOcclusionBoxes && LLRHI::sOcclusionBoxes(list))
    {
        ++sStats.submissions;
        sStats.boxes += list.box_count;
        sStats.cuts += cut ? 1 : 0;
        return;
    }
    // Nothing recorded (promise 5): every box the old way, in order. The
    // next box asks the backend again (the old way changed state).
    sStats.refused += list.box_count;
    LL_WARNS_ONCE("RHIOcclusion") << "occlusion boxes '" << mName << "' refused after the backend said it was ready ("
                                  << list.box_count << " boxes drawn the old way)" << LL_ENDL;
    for (const Box& box : mBoxes)
    {
        viewer_side().old_path(box);
    }
    mNeedReady = true;
}

void LLRHI::Occlusion::takeStats(Stats& out)
{
    out = sStats;
    out.results = sResults.exchange(0, std::memory_order_relaxed);
    out.results_kept = sResultsKept.exchange(0, std::memory_order_relaxed);
    sStats = Stats();
}

void LLRHI::Occlusion::getQueryObjectuiv(U32 name, U32 pname, U32* params)
{
    const KeptResult kept = tKept;
    tKept.valid = false;
    if (!params || sMode == 0 || !LLRHI::sOcclusionResult)
    {
        LLRHI::getQueryObjectuiv(name, pname, params);
        return;
    }
    if (pname == QUERY_RESULT && kept.valid && kept.name == name)
    {
        *params = kept.result;
        sResultsKept.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (pname != QUERY_RESULT_AVAILABLE)
    {
        LLRHI::getQueryObjectuiv(name, pname, params);
        return;
    }
    U32 available = 1;
    U32 result = 1;
    LLRHI::sOcclusionResult(name, &available, &result);
    sResults.fetch_add(1, std::memory_order_relaxed);
    *params = available;
    tKept.valid = true;
    tKept.name = name;
    tKept.result = result;
}

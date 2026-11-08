/**
 * @file llrhiframe.h
 * @brief The frame as a render thread would take it (research/208, stage 4a): the census (S0) and the pass head's switch (S1).
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

#ifndef LL_LLRHIFRAME_H
#define LL_LLRHIFRAME_H

#include "stdtypes.h"

#include <atomic>

namespace LLRHI
{
    // --- S0: the render thread census (research/208 6.2) ------------------
    // LLRHI_RENDERTHREAD_CENSUS=1 at start, or the cue rtcensus.toggle
    // (LLRHI::sRenderThreadCensus, llrhi.h). It changes nothing that is drawn.
    // While it is on, the main thread's frame is cut at the frame cost marks
    // (cpuMark, cpuDetailMark and gpuMark fire, without frame cost's GPU
    // timestamps or its lock), the backend lock's takes by the main thread are
    // timed (LLVKTrackerMutex), and the retained scene's parts are timed, each
    // without the parts inside it. The hand-off point is the "pre-gbuffer sky"
    // mark (right after the main view's stateSort and rebuildPools), the join
    // point present_frame's entry. Every 5 s a VKPerf line "rt census: ...":
    // the main thread's time after the hand-off (in and out of the lock), what
    // a render thread could take (the full-screen passes, the scene's choice,
    // binder, expand and indirect draws), the time before it (images, idle,
    // loop), and the overlaps research/208 3.2 predicts from them, with the
    // S0 verdict (alpha with owned tables >= 0.3 ms). Off: one flag test where
    // it would count (latched at the present, so a frame is counted whole).
    extern std::atomic<bool> sRenderThreadCensusOn;
    inline bool renderThreadCensusOn()
    {
        return sRenderThreadCensusOn.load(std::memory_order_relaxed);
    }

    // A mark of the main thread's frame (the backend's sCPUMark and sGPUMark,
    // other threads' are ignored). Called while the census is on.
    void renderThreadCensusMark(const char* name);
    // present_frame's entry, on the main thread (the join point): closes the
    // frame, latches the switch, writes the line every 5 s.
    void renderThreadCensusJoin();
    // The backend lock (LLVKTrackerMutex): taken and left at depth 0, and a
    // take that found it held (start: renderThreadCensusTicks() before the
    // wait). Only the main thread's are counted.
    void renderThreadCensusLockTaken();
    void renderThreadCensusLockLeft();
    U64 renderThreadCensusTicks();
    void renderThreadCensusLockWaited(U64 start);

    // A take that waits for the lock, timed while the census is on.
    struct CensusLockWait
    {
        CensusLockWait() : mStart(renderThreadCensusOn() ? renderThreadCensusTicks() : 0) {}
        ~CensusLockWait()
        {
            if (mStart)
            {
                renderThreadCensusLockWaited(mStart);
            }
        }
        CensusLockWait(const CensusLockWait&) = delete;
        CensusLockWait& operator=(const CensusLockWait&) = delete;
        const U64 mStart;
    };

    // The retained scene's parts (research/208 S0 2): the viewer's, which stay
    // on the main thread (the waiting records and the frame's start, the
    // views' marks, a pass's entry), and the backend's, which a render thread
    // could take (the choice, the binder's head and admission, the expand,
    // the indirect draws' binding and recording).
    enum CensusPart : U32
    {
        CENSUS_SCENE_BEGIN_FRAME,   // LLSceneRecords::beginFrame (registration included)
        CENSUS_SCENE_VIEWS,         // beginView, markVisibleInView, endView (postSort's record part)
        CENSUS_SCENE_PASS_ENTRY,    // drawPassInView without the backend's drawPass
        CENSUS_SCENE_CHOOSE,        // the backend's choice for a view
        CENSUS_SCENE_BINDER,        // beginPass, the textures, the matrix rows, the admission
        CENSUS_SCENE_EXPAND,        // the expand (rows and commands)
        CENSUS_SCENE_RECORD,        // bindChunk and the indirect draws
        CENSUS_PARTS
    };
    // Times a part (main thread only) without the parts begun inside it.
    bool renderThreadCensusPartBegin(CensusPart part);
    void renderThreadCensusPartEnd();
    struct CensusTimer
    {
        explicit CensusTimer(CensusPart part) : mOn(renderThreadCensusOn() && renderThreadCensusPartBegin(part)) {}
        ~CensusTimer() { stop(); }
        void stop()
        {
            if (mOn)
            {
                mOn = false;
                renderThreadCensusPartEnd();
            }
        }
        CensusTimer(const CensusTimer&) = delete;
        CensusTimer& operator=(const CensusTimer&) = delete;
        bool mOn;
    };

    // research/208 2.4: a call that hands an answer back to the main thread
    // after the hand-off: the batches drawn the usual way for a pass the
    // scene could not draw (main thread; counted after the hand-off only).
    void renderThreadCensusRescue(U32 batches);

    // --- S1: the pass head (research/208 6.2 S1) ----------------------------
    // LLRHI_FRAME_SNAPSHOT: off (the binder reads the main thread's GL context
    // at every chunk, as before), verify (it also makes the pass head, the
    // values read once at the pass's entry, and compares what both make at
    // every chunk; the context's way draws), auto (verify until 600 frames of
    // chunks agree, then on; default), on (bindChunk binds from the head
    // only: the program, its state, the draw target, the units' words and
    // descriptors and the vertex inputs as the pass's entry found them; what
    // it could fail at is made before the pass draws, the admission). Only
    // while the retained scene draws (LLRHI_SCENE) is anything bound.
    enum class PassHeadMode : U8
    {
        Off,
        Verify,
        Auto,
        On,
    };
    PassHeadMode passHeadMode();
    const char* passHeadModeName(PassHeadMode mode);

    // --- S2: the render thread (research/208 6.2 S2, section 11) -------------
    // LLRHI_RENDER_THREAD: off (default: the main thread records everything, as
    // before), verify (the main thread still records the retained scene's
    // passes, from the values it hands the render thread; the thread reads the
    // same values and the two are compared at the join: the jobs, their order
    // and every value), auto (verify until 600 frames agree, then on; not on a
    // PC with fewer than 8 logical processors), on (the render thread records
    // the expands and the indirect draws of the main and alpha views' retained
    // passes into streams spliced where the main thread would have recorded
    // them; the main thread goes on and joins before every submission). The
    // cue renderthread.toggle (LLRHI::sRenderThread) turns the thread's use off
    // and on again while the viewer runs. Read once at start.
    enum class RenderThreadMode : U8
    {
        Off,
        Verify,
        Auto,
        On,
    };
    RenderThreadMode renderThreadMode();
    const char* renderThreadModeName(RenderThreadMode mode);

    // --- S4 (research/208 section 13, research/209 section 6; vk-s4a4) -------
    // The same four values as RenderThreadMode, each read once at start.
    // LLRHI_SCENE_BATCH (209 E4): the retained scene's choices and expands are
    // queued and recorded together at the cut points (before a flush copies
    // rows, before a submission), their barriers merged. Off (default):
    // recorded where they are made, as before.
    RenderThreadMode sceneBatchMode();
    // LLRHI_PASS_SECONDARY (208 13.3): the render thread's passes record
    // their draws straight into secondary command buffers of the thread's
    // own (verify: also, and thrown away). Off (default) without the render
    // thread too.
    RenderThreadMode passSecondaryMode();
    // LLRHI_RENDER_THREAD_VIEWS (208 13.4): "all" hands the probe faces',
    // mirrors' and water mirror's passes to the render thread as well; else
    // (default) the main and alpha views' only.
    bool renderThreadAllViews();
    // LLRHI_SCENE_SHADOW_PROBE (208 13.6): on draws the probes' shared sun
    // shadow cascades from the retained scene too (under LLRHI_SCENE_SHADOW's
    // mode, layers and checks). Off (default).
    bool sceneShadowProbe();
}

#endif // LL_LLRHIFRAME_H

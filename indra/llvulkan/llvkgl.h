/**
 * @file llvkgl.h
 * @brief The GL subset the viewer uses, executed on Vulkan (state tracker + draw translation).
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

#ifndef LL_LLVKGL_H
#define LL_LLVKGL_H

#include "stdtypes.h"

#include <string>

#include "llrhi.h"

// Wraps the viewer's GL function pointers. Vulkan renders and presents
// alone: the tracker keeps the Vulkan side of each buffer, texture and
// program, tracks fixed-function state, and turns each draw into Vulkan
// commands in LLVKMirror's frame. There is no GL context or driver
// (research/117): object names, shader objects and GL's queries are answered
// here too, and every GL function the viewer loads comes from getProcAddress.
//
// Scope grows milestone by milestone (research/07-vulkan-plan.md); anything
// not handled yet is counted and reported instead of guessed at.
namespace LLVKGL
{
    void install();

    // research/117 G5: what LL_GET_PROC_ADDRESS gives
    // the viewer for a GL or WGL function name instead of wglGetProcAddress
    // (a PROC). Every name gets a function: the Vulkan side's answer, or one
    // that logs the name once and returns 0. The Vulkan device must be ready.
    void* getProcAddress(const char* name);

    // research/207 T3: LLRHI::deviceInfo's answer, from the Vulkan device (llvkgl_glquery.cpp).
    bool deviceInfo(LLRHI::DeviceInfo& out);

    // research/88: at the start of a frame, before anything of it is
    // recorded: textures waiting to be stored BC7 are encoded, a few a frame.
    void compressTextures();

    // research/245: at the start of a frame, before anything of it is
    // recorded, while VRAM is short (LLRHI::sBufferCompact): the Vulkan copies of
    // the GL buffers in the emptiest GL buffer pool blocks go, so the blocks go
    // back to the driver. Off: one flag test.
    void compactBuffers();

    // research/285 X2: at the start of a frame, before anything of it is
    // recorded (and before compressTextures' BC7 swaps), while VRAM is short
    // (LLRHI::sTexCompact with sBufferCompact): the textures of the emptiest
    // texture pool blocks are copied into fuller blocks on the GPU, so the
    // blocks go back to the driver. Off: two flag tests; on, a clock read a
    // frame and the lock only when a check is due (every 0.5 s at most).
    // True: it looked over the textures this frame (compactBuffers' check then
    // waits for the next frame: one such scan a frame).
    bool compactTextures();

    // research/111 B-PL: render thread, once a frame after present. Keeps the
    // keys of the pipelines made (saved beside the pipeline cache) and, once
    // the programs stop changing, makes those of earlier sessions on a low
    // priority thread before a draw needs them. LLRHI_PIPELINE_PREWARM=0: off.
    void prewarmPipelines();
    // fix-threads4: at shutdown (LLRHIVKMirror::onGLShutdown), the pipeline
    // and shader prewarm threads end before the statics their jobs read are
    // destroyed at exit. Waits for one pipeline or one compile at most per
    // thread (a shader prewarm's planning job: its own loop).
    void stopPrewarm();

    // One-line summary of this frame's translation (draws replayed / skipped and why).
    std::string frameReport();
    // Translation cost over the last `frames` frames (descriptor reuse; timings with LLRHI_VK_PERF); resets the counters.
    std::string perfReport(U32 frames);
    // research/155 T0b: the VKPerf line's last columns, per frame over the
    // last `frames` frames (always counted, zeros shown so the columns stay):
    // pass cache misses by reason, render target state changes, draw buffer
    // calls that changed nothing, LLRender's VB cache misses. Taken before
    // perfReport (which starts the miss reasons over); starts its own over.
    std::string counterReport(U32 frames);
    // research/218 vk-s7f (research/207 7.1, 7.3): the old paths' use over
    // the last `frames` frames, totals (GL-name lookups and direct draws, the
    // attribute pool by GL name, the material loop's GL names, every GL entry
    // point's calls, direct refusals). Taken before perfReport; starts its own over.
    std::string oldPathReport(U32 frames);
    void resetFrameStats();
    // Frame cost (research/83): draws translated this frame (resetFrameStats
    // starts it over) and direct draws since the last perfReport.
    void drawCounts(U32& draws, U32& direct);

    // What the viewer sets around its draws and the draw path reads
    // (LLRHI::sDepthOnlyPass, sSuppressTranslation, sOcclusionScope,
    // sCensusTag, sOITPriority, sShadowDepthBias; research/118 TT5c). A draw
    // list (llvkdrawlist.cpp) records with the values of when it was
    // submitted and each draw's OIT priority; any other draw reads LLRHI's.
    struct DrawGlobals
    {
        bool depth_only = false;
        bool suppress_translation = false;
        U32 occlusion_scope = 0;
        U32 census_tag = 0;
        U32 oit_priority = 0;
        LLRHI::ShadowDepthBias shadow_bias;
    };
    // LLRHI's values now (the caller's thread).
    DrawGlobals drawGlobalsNow();
    // The draw path reads `globals` instead of LLRHI's while this lives (on
    // this thread).
    class QueuedGlobals
    {
    public:
        explicit QueuedGlobals(const DrawGlobals& globals);
        ~QueuedGlobals();
        QueuedGlobals(const QueuedGlobals&) = delete;
        QueuedGlobals& operator=(const QueuedGlobals&) = delete;
    };
}

#endif // LL_LLVKGL_H

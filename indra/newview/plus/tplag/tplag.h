/**
 * @file tplag.h
 * @brief After a teleport from a heavy place, the frame time, VRAM and what the
 *        old region still holds, on a timeline; and the switches of the GL
 *        buffer compaction (research/245).
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

#ifndef PLUS_TPLAG_H
#define PLUS_TPLAG_H

#include "stdtypes.h"

// research/245. After a teleport from a heavy place to a light one the frame
// stayed slow for minutes (2026-10-30, 180-1960 ms a frame): VRAM past the
// driver's budget, the GL buffer pools' blocks part full once the old region
// had gone (977 MB used in 3168 MB), the discard bias at 4 and texture
// pressure at 3 throughout.
//
// PlusTPLagLog (on): a line (tag TPLag) at the arrival and +1, +3, +10, +30,
// +60, +120, +180 and +300 s after it: the frames since the line before
// (count, mean and longest ms), VRAM against the driver's budget, the texture
// and GL buffer pools (used in blocks; the backend's full memory report too in
// development starts, LLRHI::sPeriodicLogs), the vertex buffers, the textures, bias and pressure, the regions
// (the one arrived in and the others still open, with their objects alive),
// the objects and avatars, and what the compaction gave back. Nothing runs
// between arrivals except the counts a timeline asks for (below).
//
// PlusBufferCompact (on by default since research/285) and PlusBufferCompactIdle (20 s, 0 to
// 60; 0: every buffer counts as in use) go to LLRHI::sBufferCompact / sBufferCompactIdleSeconds when
// they change (the setting's signal: nothing a frame).
//
// research/309: the bias watch (tag BiasWatch, PlusTPLagLog too): once the discard bias has stayed over 1.5 for
// 20 s, whatever raised it (a teleport or not), a line every 60 s with the bias frames counted as in the TPLag lines,
// the memory guard's statistics (plus/memguard: free memory range, how often it went under the line, for how
// long) and the viewer's own memory, until the bias has been at or under 1.05 for 30 s. The 20 s before it opens are
// counted and said in the line it opens with.
//
// Main thread only. One hook in an official file: LLViewerTexture::updateClass hands noteBiasFrame the frame's
// values while a timeline or a watch is open (biasFrameWanted; research/296, 309); the texture scan
// (plus/texprio) reads timelineOpen().
namespace plus::tplag
{
    // research/296 (tp-release, diagnostics only: nothing here changes a decision). Why the discard bias does
    // not come down after a teleport, as counted frames since the line before, and the figures behind it. One
    // call a frame from LLViewerTexture::updateClass (a few values copied, while biasFrameWanted says so: a
    // timeline or a bias watch is open, or the bias is over 1.5). The booleans and values are the frame's own, as
    // updateClass worked them out.
    struct BiasFrame
    {
        F32 bias = 1.f;                 // after this frame's update
        F32 over_pct = 0.f;             // (used - target) / target
        F32 used_mb = 0.f;              // textures' image bytes (Vulkan) + vertex buffers once
        F32 settings_target_mb = 0.f;   // RenderTextureVRAMDivisor's target
        F32 real_target_mb = 0.f;       // the driver's budget less the rest of VRAM and the margin (0: no real budget)
        F32 vertex_mb = 0.f;
        F32 sys_free_mb = 0.f;          // free system memory (checked once a second)
        F32 fall_free_mb = 0.f;         // the free system memory the fall needs more than (the threshold setting + 100 MB)
        F32 room_s = 0.f;               // updateClass's room timer: seconds since the bias was raised or over_pct was not under -10%
        U32 max_vram_budget_mb = 0;     // RenderMaxVRAMBudget (0: not set)
        S32 pressure = 0;
        bool real_budget = false;
        bool is_low = false;            // the bias is being raised this frame (system memory short, or over the target)
        bool sys_low = false;           // system memory short (free < RenderMinFreeMainMemoryThreshold): a reason to raise
        bool guard_low = false;         // plus/memguard is raising: Emergency, or Soft for 3 s (research/309); sys_low has only Emergency
        bool budget_held = false;       // a snapshot holds the budget: no rise, no pressure stage
        bool bias_all = false;
        bool was_backgrounded = false;  // the bias fixed at 5 after TextureDiscardBackgroundedTime
        bool not_visible = false;       // the window is not visible
        bool no_focus = false;          // the app has no focus
        bool kept_in_front = false;     // ... and plus/background keeps its textures as in front (PlusBackgroundKeepVisible: no bias fixed)
        bool guard_may_fall = true;     // plus/memguard::biasMayFallPeek()
        bool sys_raised = false;        // part of bias_all: system memory was short within the last 60 s while the bias is up
        bool guard_held = false;        // part of bias_all: plus/memguard raised the bias and holds it (biasHeld())
    };
    void noteBiasFrame(const BiasFrame& frame);

    // research/309: whether updateClass is to fill and hand over a BiasFrame this frame (a timeline is open, a bias watch
    // is open, or the bias is over 1.5 and a watch may open: it opens once the bias has stayed over 1.5 for 20 s, and
    // closes after 30 s at or under 1.05). Returns false at once while the bias is low and nothing is open.
    bool biasFrameWanted(F32 bias);

    // Whether a TPLag timeline is open (an arrival logged, its last line not yet): plus/texprio counts only then.
    bool timelineOpen();
}

#endif // PLUS_TPLAG_H

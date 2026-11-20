/**
 * @file multiinstance.h
 * @brief Charis viewers of one user on one GPU make room for the one in front (plus/multiinstance)
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

#ifndef PLUS_MULTIINSTANCE_H
#define PLUS_MULTIINSTANCE_H

#include "stdtypes.h"

// research/337 stage 1-2 (multi-coord). Two Charis viewers on one PC each take the whole GPU for themselves: each
// aims its textures at its own share of VRAM and draws at full rate even when the other is the one in use.
//
// The registry: a 4 KB shared memory of this user's Charis viewers in this logon session
// (Local\CharisViewer.Instances.v1.<a hash of the user's SID>). Only that SID may open it, a process of low integrity can
// neither read nor write it, and one that is there already is used only when its owner, access list and integrity label
// are as this code makes them and its page is committed memory of the paging file. It has eight slots of numbers only
// (process id and start time, the GPU's LUID, a heartbeat, in front or seen, VRAM figures); no command, handle, path or
// name. Each viewer writes its own slot once a second on the main thread (a sequence count around the write, no lock
// across processes) and reads the others', each value checked before use: the process is alive with that start time
// and has this program's file name, the heartbeat is under HEARTBEAT_STALE_MS old, the values are in range. A slot of
// a viewer that is gone is free for the next one.
//
// With no other viewer on this GPU (most starts) nothing changes: one write a second; per frame a few compares (and
// LLApp::isRunning() only while yielding). With others:
//  B (yield): while another viewer has been in front for YIELD_AFTER_MS and this one is not, this one draws at most
//    PlusMultiBackgroundFps frames per second (30; 0: off) and runs at below-normal priority
//    (PlusMultiBackgroundPriority; only when it was at normal priority, and not while it can speak on voice, as the speak
//    button tells: WebRTC runs in this process). Back at once when this one is in front.
//  A (VRAM share, PlusMultiShareVRAM, off until measured: research/337 M1, M2): the texture target is held to this
//    viewer's part of the driver's budget less the margin, by weights (in front 2, seen 1, minimized or hidden 0.5),
//    less what else of VRAM this viewer takes; never under max(SHARE_FLOOR_MB, a quarter of the budget). A larger part
//    applies at once, a smaller one only after it has held SHARE_LOWER_AFTER_MS.
// LLRHI_MULTI_INSTANCE=0 (and so a safe start, plus/safemode) makes no registry: all as without this feature.
//
// Main thread only. Log (tag MultiInstance): joining or not (why), leaving, and each change of the others on this
// GPU, who is in front, yielding and the share, at most one line every LOG_MIN_MS (process ids and MB only).
namespace plus::multiinstance
{
    // Once, in LLAppViewer::init() after the window (the GPU is known) and the cache folders. Joins the registry or
    // logs why not. A second call does nothing.
    void init();

    // Every frame, from LLAppViewer::doFrame() before plus::fpslimit::wait(). A few compares unless a second has
    // passed (then LLApp::isRunning(), the slot's write and the others' reading) or it is yielding (then whether this
    // window has come to the front or the viewer quits: the yield ends at once).
    void update();

    // Early in LLAppViewer::cleanup(): the priority as it was, the slot given back. Safe without init().
    void shutdown();

    // The frame rate cap while yielding (B), for plus::fpslimit::wait(); 0: none.
    S32 fpsCap();

    // What LLViewerTexture::updateClass() has of VRAM this frame, with the driver's budget only (Vulkan with
    // VK_EXT_memory_budget). In MB.
    struct VRAMReading
    {
        F32 budget_mb = 0.f;    // the driver's budget
        F32 margin_mb = 0.f;    // what updateClass leaves free of it
        F32 textures_mb = 0.f;  // textures and vertex buffers, as the bias counts them
        F32 usage_mb = 0.f;     // all VRAM this process uses
    };

    // This viewer's share (A) for this frame, in MB.
    struct VRAMShare
    {
        F32 limit_mb = 0.f;     // the most the texture target may be: part_mb - (usage - textures), at least the floor;
                                // 0: no share (alone, the setting off, not in the registry, no driver's budget)
        F32 part_mb = 0.f;      // (budget - margin) x this viewer's part of the weights
        F32 other_mb = 0.f;     // the VRAM it takes besides textures (usage - textures), taken off part_mb
        bool floored = false;   // limit_mb is the floor, max(SHARE_FLOOR_MB, a quarter of the budget): it does not
                                // move with this viewer's other VRAM then
    };

    // This viewer's share (A). Keeps the reading for the registry's figures. From LLViewerTexture::updateClass(). While
    // the share is the texture target and not at its floor, the bias rises over usage part_mb and falls under part_mb -
    // textures / 9 (as with the driver's limit, budget - margin in place of part_mb): what the VRAM compaction is given.
    VRAMShare vramShare(const VRAMReading& reading);
}

#endif // PLUS_MULTIINSTANCE_H

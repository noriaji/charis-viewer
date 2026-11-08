/**
 * @file memguard.h
 * @brief The PC short of free memory, or decoded textures piling up: large
 *        texture decodes wait and the textures go down, so the viewer does not
 *        take the PC's last memory (igpu-safe).
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

#ifndef PLUS_MEMGUARD_H
#define PLUS_MEMGUARD_H

#include "stdtypes.h"

// igpu-safe (2026-11-03). A texture is
// decoded on the texture fetch thread and waits, decoded (4 to 16 MB a
// texture of 1024 or 2048), until the main thread makes its image. On a GPU
// that shares the PC's memory, decoded textures and the GPU's own memory come
// out of the same RAM, and nothing held them back: the official viewer only
// reacts once less than RenderMinFreeMainMemoryThreshold (512 MB) is free,
// and only on the main thread (LLViewerTexture::updateClass).
//
// The PC's free physical memory against three lines (L: the guard's line;
// B, the band: L/2, 512 MB to 1 GB, so that an 8 GB PC can reach it):
//   under L                     low(): the discard bias goes up (updateClass
//                               takes it as low system memory)
//   L .. L + max(L/4, 256 MB)   still low() once low (it ends above)
//   .. L + B                    neither: the bias stays where it is
//   above that for 10 s         roomy()
// What the guard raised (noteBias: the bias it found when it first raised it)
// comes down only when roomy(), or once not low for 60 s (in the band), and at
// the base step alone (RenderHighMemMinDiscardDecrement, 0.1 a second;
// biasHeld()): the textures come back seconds after it, and a fast fall (up to
// 0.9 a second) went past what memory allows before memory showed it. Once the
// bias is back where the guard found it, the official rules alone again. A
// bias raised by something else (the VRAM budget, a teleport) comes down as
// before, wherever the free memory is (biasMayFall).
//
// Decodes (LLTextureFetchWorker's DECODE_IMAGE, decodeHoldMs): with the guard
// on the decodes, a large one (more than 128x128 texels; the size not known
// and not the smallest level: large) waits while the decoded images the main
// thread has not taken pass WAITING_CAP (one passes every 2 s), or while low()
// (one passes every 250 ms). Never stopped altogether, and never waiting: a
// texture's first image (requested at the smallest level, 1/32 a side, when
// nothing is known of it), local files (the UI's images) and bakes. A waiting
// decode deleted (or the viewer quitting) ends there instead of taking a pass;
// one at priority 0 waits without taking one.
//
// The line (PlusMemGuardFreeMB, 0: automatic):
// - a GPU without VRAM of its own (LLRHI::DeviceInfo::shared_memory): a
//   sixteenth of the PC's memory, 768 MB to 2 GB (2 GB on a 32 GB PC; 1 GB
//   on 16 GB, 768 MB on 8 GB); the decodes are guarded. (research/309: it was a
//   tenth, 768 MB to 3 GB: a 32 GB PC with other heavy programs, the free
//   memory at 2.8 to 4.2 GB, sat under it and swung over it every few seconds,
//   the bias going up each time and not coming down.)
// - a discrete GPU: none (the official reaction alone), unless
//   PlusMemGuardFreeMB is set (then as above, decodes too).
// PlusMemGuard off: none. Windows only (elsewhere none).

// research/309 M1, M3, M4 (PlusMemGuardSoftOnly, on by default; off: all as above). The guard has three states:
//   Normal      free memory at or over the line (L).
//   Soft        under L (until L + the low end), over the emergency line H = max(512 MB, L/3): the viewer holds back
//               what it adds (large decodes wait, one every 100 ms) and the textures of what is off screen go down
//               (the discard bias goes up as for a short VRAM, after the memory has stayed low for 3 s: a swing of the
//               free memory of a few seconds, the other programs', raises nothing). What is on screen is not
//               made coarser: bias_all is not set for it, so plus/texprio's split and the floor of the faces in
//               view stay.
//   Emergency   under H (until H + max(H/4, 128 MB)): as the guard has always been: the bias goes up as for low
//               system memory, what is on screen goes down too, the floor is lifted.
// The bias the guard raised comes down once the memory has not been low for 10 s (60 s without the setting), a
// Soft-only raise (no Emergency in the last 60 s) by up to three times the base step (at most 0.3 a second: the step
// is the official one, 0.1 plus what the memory has in room), a raise with an Emergency in it by the base step.
// The pass over every texture that a low system memory starts (llviewertexture.cpp) runs at most every 10 s when
// the guard's Emergency alone starts it (one held back is owed and runs when it may, if the memory is still short);
// the Soft to Emergency step is an edge too (the bias goes to 1.5 times the factor, the pass runs). An Emergency
// is as the guard has always been except for these and the 10 s wait. The states act only with plus/texprio's split
// (PlusTexViewPriority on, LLRHI_TEX_VIEW_BIAS not 0: the safe start turns it off): without it the guard is as before.

namespace plus
{
namespace memguard
{
    // Any thread. Atomic reads; at most every 250 ms one thread reads the
    // PC's free memory (GlobalMemoryStatusEx) and updates them.
    bool low();
    bool roomy();       // true without a line
    // research/309: the setting PlusMemGuardSoftOnly (the Soft and Emergency states) and the states themselves.
    // The states act only while plus/texprio's split is available (PlusTexViewPriority on, LLRHI_TEX_VIEW_BIAS not 0:
    // it is what keeps what is on screen out of a Soft raise); noteSplit tells it each update.
    void noteSplit(bool available);     // main thread, each update (LLViewerTexture::updateClass)
    bool softOnly();            // the guard has a line, the setting is on and the split is available (main thread)
    // One reading of the state (one refresh, so one frame sees one state): Normal; SoftWait: under the line for less
    // than 3 s (nothing is raised yet); Soft: under the line for 3 s on end, over H (the off-screen textures go down);
    // Emergency: under H (as the guard has always been).
    enum class GuardState : U8 { Normal, SoftWait, Soft, Emergency };
    GuardState guardState();
    bool biasHeldHard();        // bias_all for a held raise: with the setting off every held raise (as before), on: none
                                // (an Emergency in it is sys_raised's 60 s)
    F32 fallStepCap(F32 base);  // the most a held raise comes down by (base; with the setting three times it for a raise
                                // that had no Emergency in the last 60 s)
    // The pass over every texture a low system memory starts may run now. guard_only: the memory guard's Emergency is
    // the only reason (not the official line, not the VRAM): with the setting on it runs at most every 10 s, and one
    // held back is owed (purgeOwed) until it may. Otherwise it always may.
    bool emergencyPurgeDue(bool guard_only);
    bool purgeOwed();
    void dropPurgeOwed();       // the memory is no longer short: the pass held back is not needed
    // LLViewerTexture::updateClass (main thread), each update: raising: the
    // guard's low() is raising the bias now; bias: the bias before this update.
    void noteBias(bool raising, F32 bias);
    bool biasHeld();    // a raise of the guard's is in effect
    bool biasMayFall(); // the guard lets the bias come down (always unless biasHeld())
    // The same answer from the state as it stands: it does not read the free memory (refresh) and moves
    // nothing, so a log can ask without being the one that updates the state (research/296).
    bool biasMayFallPeek();

    // research/309 (diagnostics only): what the guard saw since the last call (main thread; the call starts the
    // next interval): how often the free memory went under the line, for how long, and its range. The state as it
    // stands: the line values, low, roomy, how long it has been quiet (not low) and over the room line, whether
    // a raise of the guard's is held. has_line false: no line (a discrete GPU, or off): nothing was counted.
    struct Stats
    {
        bool has_line = false;
        U32 low_edges = 0;          // times the free memory went under the line
        F32 low_seconds = 0.f;      // time spent low (at the guard's check, 250 ms)
        F32 interval_seconds = 0.f;
        U32 free_now_mb = 0, free_min_mb = 0, free_max_mb = 0;
        U32 line_mb = 0, low_end_mb = 0, room_mb = 0;
        // research/309 M1: the emergency line H, whether the guard is in Emergency now, how often it went under H in
        // the interval, and whether the Soft/Emergency states are on (PlusMemGuardSoftOnly)
        U32 emergency_mb = 0;
        U32 emergency_edges = 0;
        F32 emergency_seconds = 0.f;    // time spent in Emergency (at the guard's check)
        bool emergency = false;
        bool soft_only = false;
        bool low = false, roomy = false, bias_held = false;
        F32 quiet_seconds = 0.f;    // not low for this long (0: low now)
        F32 room_seconds = 0.f;     // over the room line for this long (0: not over)
    };
    void takeStats(Stats& stats);

    // LLTextureFetchWorker (fetch thread): 0: this decode starts now; else it
    // waits for a pass, one every that many ms (takeDecodePass). exempt: a
    // local file or a bake; bytes: what it will make (decodedBytes).
    U32 decodeHoldMs(bool exempt, U64 bytes);
    bool takeDecodePass(U32 period_ms);
    // The bytes a decode of a width x height image of `components` makes at
    // `discard`. Size not known (0): small at the smallest level
    // (MAX_DISCARD_LEVEL), else taken as 1024 x 1024.
    U64 decodedBytes(S32 width, S32 height, S32 components, S32 discard);
    // The decoded images held for the main thread, as a change in bytes (a
    // worker's decode done: +, taken, replaced or gone: -). Any thread.
    void addWaiting(S64 delta);
}
}

#endif // PLUS_MEMGUARD_H

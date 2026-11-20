/**
 * @file vramdiag.h
 * @brief A census of the images over 8 MB, for an overrun of the driver's VRAM budget (research/354 vram-stall-diag).
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

#ifndef PLUS_VRAMDIAG_VRAMDIAG_H
#define PLUS_VRAMDIAG_VRAMDIAG_H

#include "linden_common.h"

// LLRHI_VRAM_DIAG=1: a VKPerf "VRAM diag textures" log message every 10 s about the textures of the list whose images
// take over 8 MB (the ones the Vulkan backend gives memory of their own: 2048x2048 plain RGBA8 and up), to tell what the
// ones a BC7 store would shrink are and why the others are not (research/354 section 5.1 (1)):
//   by origin: the viewer's own rules for BC7 (LLViewerFetchedTexture's rhi_compressible: a LOD texture without a boost,
//              not a bake, a local file, a sculpt map or a projector light) split the images into own bakes, other avatars'
//              bakes, boosted ones, local files, sculpt maps and projector lights, not LOD textures, and the ones the
//              viewer lets be BC7 ("eligible"); each with how many are stored BC7, how many plain, and of the plain ones how
//              many were scaled down (discard level over 0: the full size is larger);
//   by discard level, by view tier (plus/texprio: on screen near, on screen, off screen), by size now;
//   what a BC7 store would save at most (3/4 of the plain bytes): of the eligible ones, and of all of them;
//   the backend's BC7 report (what waits for the encoder, why it is off, what it left plain), the five largest (the start of the UUID, never an own bake's; size,
//   discard level, origin), and the images over 8 MB that are not in the texture list.
// Reading only: the texture list, the faces and the backend are read, never written, and the backend's two lookups
// (LLRHI::sTextureImageBytes, sTextureCompressed) are the ones the texture census (LLRHI_TEX_CENSUS) calls. Off: the
// hook is one flag test. Main thread only (LLViewerTexture::updateClass). One scan of the list every 10 s (some 7000
// textures; the message says how long it took).
namespace plus::vramdiag
{
    // Set once from the environment.
    extern const bool sOn;

    // What the viewer's texture budget has at the end of an update: the real budget's numbers (usage and budget of VRAM,
    // bytes; real: the driver gave them), the textures and vertex buffers it counts and the limit it keeps them under (MB).
    struct Budget
    {
        bool real = false;
        U64 usage = 0;
        U64 budget = 0;
        F32 used_mb = 0.f;
        F32 limit_mb = 0.f;
    };

    // Counts the time to the next message; the message (a scan of the texture list) is every 10 s. The caller tests sOn
    // first (the hook builds the Budget only when it is on).
    void frame(const Budget& budget);
}

#endif // PLUS_VRAMDIAG_VRAMDIAG_H

/**
 * @file skinonce.h
 * @brief Skin once, part (a): an avatar's skinning palette is asked of the
 *        backend once per build, not once per pass and draw
 *        (LLRHI_SKIN_ONCE / PlusSkinOnce, research/133-skin-once.md in the
 *        sl-viewer repository).
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

#ifndef PLUS_SKINONCE_SKINONCE_H
#define PLUS_SKINONCE_SKINONCE_H

#include "stdtypes.h"
#include "llrhi.h"

// The <plus> hook in LLRenderPass::rhiPaletteRow() (lldrawpool.cpp).
//
// Since research/56 (LLRHI::sPalettePrebuild, on by default) every pass of a
// frame already reads one copy of an avatar x skin palette: the first pass
// that uses it has the backend place it in the frame's palette table
// (LLRHI::sPlacePalette) and keeps the row in MatrixPaletteCache::mRHIRow;
// every later pass names that row. What is left per pass is the asking: each
// palette upload (LLRenderPass::uploadMatrixPalette, per avatar or skin
// change in every pass) and each rigged native shadow packet (rhi_add_packet,
// per batch and cascade) calls sPlacePalette again, which takes the backend's
// tracker lock to answer "already placed".
//
// The row is reset by every build of the palette (updateSkinInfoMatrixPalette,
// once per gFrameCount) and the backend fills it the first time it is asked,
// placed or not (row.room 0: it did not fit, not asked again this frame). So a
// row whose frame is set holds the backend's answer for this build, and
// asking again gives the same answer, unless the backend's frame moved on
// (a present) within the same gFrameCount: then the row is an older frame's,
// and both readers check that themselves (LLRHI::sUsePaletteRow refuses it,
// the uniform is set instead; the native shadow casters copy the joints
// instead of reading the row), so the pixels are the same either way.
namespace plus::skinonce
{
    // LLRHI_SKIN_ONCE=1 turns it on, =0 off, whatever PlusSkinOnce says
    // (default off). Main thread.
    bool enabled();

    // In LLRenderPass::rhiPaletteRow(), before asking the backend: true when
    // the row already holds the backend's answer for this build (row.room != 0
    // is that answer) and this is on. Main thread (the rigged shadow packets
    // are built there too).
    bool keepRow(const LLRHI::PaletteRow& row);
}

#endif // PLUS_SKINONCE_SKINONCE_H

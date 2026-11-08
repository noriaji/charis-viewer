/**
 * @file llrhilook.h
 * @brief Looks (colour grading with a LUT): what the viewer hands the backend.
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

#ifndef LL_LLRHILOOK_H
#define LL_LLRHILOOK_H

#include "stdtypes.h"

// Research/102 (stage 1a). The viewer (plus/look) grades the finished image
// of renderFinalize (tone mapped, anti-aliased, before the UI: the values
// ReShade applies its LUTs to) in one pass into the GL framebuffer it has
// bound, a target of the source's size:
//   out = mix(source, LUT(source), strength)
// The LUT is a 3D colour table of size N held as a 2D strip, N tiles of N x N
// side by side (N^2 x N texels): the tile is blue, x in a tile red, y green
// (row 0 is green 0, as uploaded). The texture filters bilinearly and clamps;
// the pass blends two tiles for blue.
namespace LLRHI
{
    // Screen effects drawn in the same pass, after the LUT. Each amount is
    // 0 (off) to 1 (the strongest the pass makes). Sizes follow the picture's
    // height and places its pixels (LookGrade::image_*), so the screen and
    // snapshots look alike, also a large snapshot drawn in tiles; only what
    // reads neighbouring pixels (the painted styles, halation, the colour
    // fringes, clarity, texture and the soft glow) cannot read across a tile's edge
    // (clarity, texture and the soft glow fade out towards a tile's inner edges).
    struct LookFx
    {
        F32 grain = 0.f;                // film grain (monochrome, ~24 changes a second)
        F32 vignette = 0.f;             // darkened corners
        F32 chromatic = 0.f;            // colour fringes growing towards the edges
        F32 halation = 0.f;             // red-orange glow round bright areas
        F32 dust = 0.f;                 // specks and scratches of old film (~8 changes a second)
        F32 scanlines = 0.f;            // video lines, with a little horizontal wobble
        // Local contrast (research/249 2.1), after the LUT: the brightness moved
        // away from the mean round the pixel, the hue kept.
        F32 clarity = 0.f;              // large radius (about 2 % of the picture's height)
        F32 texture = 0.f;              // small radius (about 0.3 %): fine detail
        U32 clarity_taps = 24;          // the taps of clarity's disc (more while a snapshot is taken)
        // Soft glow (research/286): a wide blur of the whole picture laid back over it, the way a
        // blurred copy is put over a photo in an editor; the picture glows softly all over (after
        // the LUT, clarity and the miniature; the blur has the LUT's colours too).
        F32 soft_glow = 0.f;            // how much of the blur replaces the picture (0..1; 1 is 60 %)
        F32 soft_glow_size = 0.5f;      // how wide the blur is, 0..1 (about 0.6 % to 3 % of the picture's height, as a sigma)
        F32 soft_glow_lift = 0.3f;      // 0..1: how much light the blur adds on top (screen), so the glow is not only a blur
        U32 soft_glow_taps = 32;        // the taps of the blur (more while a snapshot is taken)
        F32 time = 0.f;                 // seconds, wrapped; seeds the moving noise

        // A painted style replaces the picture (before the LUT and the effects
        // above; the colour fringes and the scanlines' wobble are lost under it).
        // paint is how much of the painting is mixed in, 0..1 (research/142).
        enum PaintStyle : U32
        {
            PAINT_NONE = 0,
            PAINT_CARTOON = 1,
            PAINT_WATERCOLOR = 2,
            PAINT_SKETCH = 3,
            PAINT_OIL = 4,
            PAINT_COUNT
        };
        U32 paint_style = PAINT_NONE;
        F32 paint = 0.f;

        // Miniature (tilt-shift, research/236): outside a sharp band across
        // the picture, a blur that grows with the distance from it, then a
        // little more saturation and contrast (after the LUT). Places are
        // shares of the whole picture, so snapshots of any size look alike.
        F32 miniature_blur = 0.f;       // 0..1: how strong the blur gets far from the band
        F32 miniature_saturation = 0.f; // 0..1: the saturation and contrast added
        F32 miniature_band = 0.f;       // -1..1: how far the band's centre line lies from the picture's centre, across the band
                                        // (level: -1 the bottom, 1 the top; tilted: the picture's far corners)
        F32 miniature_width = 0.25f;    // the sharp band's height as a share of the picture's
        F32 miniature_tilt = 0.f;       // degrees, anticlockwise: the band's slant
        U32 miniature_max_taps = 32;    // the most taps of the blur's disc (more while a snapshot is taken)

        bool painting() const { return paint_style > PAINT_NONE && paint_style < PAINT_COUNT && paint > 0.f; }
        bool miniature() const { return miniature_blur > 0.f || miniature_saturation > 0.f; }
        bool localContrast() const { return clarity > 0.f || texture > 0.f; }
        bool softGlow() const { return soft_glow > 0.f; }
        bool any() const { return painting() || miniature() || localContrast() || softGlow() || grain > 0.f || vignette > 0.f || chromatic > 0.f || halation > 0.f || dust > 0.f || scanlines > 0.f; }
    };

    // lut_texture 0 (and lut_size 0): no LUT, only fx; the pass then needs fx.any().
    struct LookGrade
    {
        U32 source_texture = 0;         // GL name of the finished image (the bound target's size)
        U32 lut_texture = 0;            // GL name of the strip (N^2 x N, 8-bit RGBA), or 0: none
        U32 lut_size = 0;               // N
        LookFx fx;
        F32 domain_min[3] = {};         // the colours the table spans (as a .cube's DOMAIN_MIN)
        F32 domain_max[3] = {};         // and DOMAIN_MAX, each above its minimum
        F32 strength = 0.f;             // 0: the source as it is, 1: the LUT's colours
        U32 width = 0;                  // the size of the target the pass is called for
        U32 height = 0;                 // (the bound one must have it, or nothing is drawn)
        // The whole picture the target is part of (fix-round6 4): a large
        // snapshot drawn in tiles (LLViewerWindow::rawSnapshot, when targets
        // of its size cannot be made) has one target per tile. Where the
        // target's first pixel lies in it (GL-style, from the bottom left)
        // and its size, in pixels. Size 0: the target is the whole picture.
        F32 image_offset[2] = {};
        F32 image_size[2] = {};
    };

    // On unless LLRHI_LOOK=0 (a switch for tests: the pass is then never
    // recorded). The viewer's setting (PlusLook, empty by default) decides
    // whether it runs.
    extern bool sLook;

    // LLRHI_LOOK_DEBUG (a switch for finding faults, 0 by default): what the
    // pass draws instead of the grade.
    //   1  the source as it is (reads and writes without the LUT)
    //   2  solid green (the target only)
    //   3  the LUT strip stretched over the screen (the LUT texture only)
    //   4  a red-green ramp from the pixel position (the target's size and orientation)
    //   5  nothing recorded, the caller still swaps (shows what the target held)
    extern U32 sLookDebug;

    // Set by the Vulkan backend. Records the pass into the bound draw
    // framebuffer; false when it could not (nothing recorded).
    extern bool (*sLookGrade)(const LookGrade& pass);
}

#endif // LL_LLRHILOOK_H

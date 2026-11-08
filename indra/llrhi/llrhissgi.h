/**
 * @file llrhissgi.h
 * @brief Screen space indirect light (SSGI) with a visibility bitmask: what
 *        the viewer hands the backend.
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

#ifndef LL_LLRHISSGI_H
#define LL_LLRHISSGI_H

#include "llrhigtao.h"

// Research/164 (sl-viewer). The viewer (plus/ssgi) widens GTAO's occlusion
// pass (llrhigtao.h) into a visibility bitmask (Therrien, Levesque, Gilet,
// "Screen Space Indirect Lighting with Visibility Bitmask", 2023): the same
// march gives the ambient occlusion and the light the occluders send back.
// Each pass draws into the GL framebuffer bound when it is called:
//   1. SsgiCapture (the end of a frame's opaque lighting): the lit screen at
//      half size each way: rgb the average of each 2x2 block's lit opaque
//      pixels (the sky left out), their brightest channel held at light_cap,
//      hue kept; a the nearest view depth among them (m; no opaque pixel:
//      rgb 0, a the largest depth). RGBA16F. The next frame's light.
//   2. SsgiReproject (the next frame, after GtaoDepth): per pixel of GTAO's
//      reduced view depth, where that point was on the last frame's screen
//      and what the capture holds there; nothing (0) where it was off that
//      screen or hidden (the capture's depth differs by more than
//      disocclusion), towards nothing within edge_fade of its edges.
//      R11F_G11F_B10F where the GPU can draw into it (sSsgiCompactLight),
//      else RGBA16F.
//   3. SsgiOcclusion, in place of GtaoOcclusion (the same grid): two targets,
//      0 as GtaoOcclusion's (RG16F: visibility, view depth), 1 the indirect
//      light (RGBA16F: rgb irradiance as the lighting adds it, a view depth).
//   4. SsgiDenoise, in place of GtaoDenoise: the same depth-aware 4x4 average
//      over both (two targets, as 3). GtaoApply reads target 0; the lighting
//      (softenLightF.glsl) reads target 1.
//   5. SsgiDebugView (PlusSsgiDebug 1-3 only): over the bound lit screen's
//      opaque pixels, source as it is (the nearest reduced pixel).
namespace LLRHI
{
    struct SsgiCapture
    {
        GtaoView view;                  // the frame's camera and scene depth (the lit screen's size)
        U32 color_texture = 0;          // GL name of the lit screen (HDR, linear)
        U32 width = 0;                  // the bound target's size: (view size + 1) / 2
        U32 height = 0;
        F32 light_cap = 1.f;            // the brightest channel at most
    };

    struct SsgiReproject
    {
        GtaoView view;                  // this frame's
        U32 view_depth_texture = 0;     // GtaoDepth's target
        U32 width = 0;                  // its size (the bound target's too)
        U32 height = 0;
        U32 scale = 1;                  // full-resolution pixels per target pixel, each way
        U32 light_texture = 0;          // SsgiCapture's target (the last frame); 0: no light (the target gets 0)
        U32 light_width = 0;            // its size
        U32 light_height = 0;
        F32 reproject[16] = {};         // this frame's eye space -> the capture's clip space (GL, column-major)
        F32 disocclusion = 0.05f;       // relative view depth difference from which a point counts as hidden then
        F32 edge_fade = 0.05f;          // share of the last frame's screen at its edges over which the light fades out
        bool show_misses = false;       // PlusSsgiDebug 3: red where the light was not found
    };

    struct SsgiOcclusion
    {
        GtaoOcclusion occlusion;        // GTAO's (its radius: how far the ambient occlusion reaches)
        U32 light_texture = 0;          // SsgiReproject's target (occlusion.width x height)
        F32 light_radius = 0.f;         // m: how far the indirect light reaches (the march goes to the larger radius)
        F32 thickness = 0.f;            // m: how deep a surface is taken to be behind its depth
    };

    struct SsgiDenoise
    {
        U32 occlusion_texture = 0;      // SsgiOcclusion's target 0
        U32 indirect_texture = 0;       // and its target 1
        U32 width = 0;
        U32 height = 0;
    };

    struct SsgiDebugView
    {
        GtaoView view;                  // the bound lit screen's camera and depth
        U32 source_texture = 0;         // a reduced target: SsgiDenoise's 1 or SsgiReproject's
        U32 width = 0;                  // its size
        U32 height = 0;
        U32 scale = 1;                  // as SsgiReproject's
    };

    // On unless LLRHI_SSGI=0 (a switch for tests: the passes are then never
    // recorded). The viewer's setting (PlusSsgi, off by default) decides
    // whether they run.
    extern bool sSsgi;

    // Set by the Vulkan backend. Each records its pass into the bound draw
    // framebuffer; false when it could not (nothing recorded).
    extern bool (*sSsgiCapture)(const SsgiCapture& pass);
    extern bool (*sSsgiReproject)(const SsgiReproject& pass);
    extern bool (*sSsgiOcclusion)(const SsgiOcclusion& pass);
    extern bool (*sSsgiDenoise)(const SsgiDenoise& pass);
    extern bool (*sSsgiDebugView)(const SsgiDebugView& pass);
    // A lookup: whether R11F_G11F_B10F can be drawn into and sampled (not
    // required by Vulkan); SsgiReproject's target is RGBA16F otherwise.
    extern bool (*sSsgiCompactLight)();
}

#endif // LL_LLRHISSGI_H

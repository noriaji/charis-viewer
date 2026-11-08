/**
 * @file llrhiskinfill.h
 * @brief Skin fill (softer shadows on faces): what the viewer hands the
 *        backend.
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

#ifndef LL_LLRHISKINFILL_H
#define LL_LLRHISKINFILL_H

#include "stdtypes.h"

// Research/110. On the pixels the skin mark (llrhiskinsss.h, STENCIL_SKIN)
// marks, the viewer (plus/skinfill) weakens the ambient occlusion and fills
// the shadows with some of the sun's light; on the eye pixels (STENCIL_EYE)
// it does the same, by eye_share of it, so the eyes are not left dark in a
// lit face (section 14). Both fade out towards the edge of the skin and eyes
// together (a soft mask), so a small patch of skin among other surfaces (a
// hole in a face's overlay layer) is not lit apart from them, while the eyes
// are no hole in the face. Each pass draws into the GL framebuffer bound when
// it is called:
//   1. SkinFillMask: value on the skin pixels, eye_value on the eye pixels
//      (stencil tests), nothing elsewhere: the share of the lift each pixel
//      takes. The bound target must have the main view's depth.
//   2. SkinFillShrink: per texel of the bound target, the share of the
//      factor x factor block of the mask it covers that is set (skin or eye).
//   3. SkinFillSoften: a 5 x 5 binomial blur of the shrunk mask. Sampled
//      with bilinear filtering, it is the soft mask: about 0.5 on the edge of
//      the skin, 1 well inside. The passes below weigh by
//      edge = smoothstep(0.5, 0.95, soft mask): 0 on the edge, 1 inside.
//   4. SkinFillOcclusion: the deferred light map (r the sun's shadow, g
//      ambient occlusion). Where the mask is set, g becomes
//      g + mask * edge * lift * (1 - g); nothing else is written.
//   5. SkinFillLight: the lit HDR scene (mRT->screen, with the main view's
//      depth). On the skin pixels, and by eye_share on the eye pixels, a
//      fill light is added to RGB (alpha kept):
//        reach = min(max(N.L, 0), r)               the sun reaching the pixel now
//        shape = (1 - directionality) + directionality * max(N.V, 0)
//        lift  = edge * near_shadow * max(shape - reach, 0)
//        room  = max(sunlit_share * (sun + ambient) - (ambient + sun * reach), 0)
//        fill  = albedo * min(lift * light_color, room)
//      light_color is grey (fix-round41: a tinted sky's sun does not tint
//      the skin); sun and ambient are the light's and the sky's ambient's
//      luminance (linear), so that the filled skin stays at most sunlit_share
//      as bright as the same skin in full sun (the official lighting gives it
//      about ambient + sun * reach already). near_shadow is how much light
//      reaches the points near_reach metres (three, 0.5 to 1.5 times it) from
//      the pixel towards the light, filtered as the official sun shadows are
//      (a 3 x 3 texel tent): 1 where the shadow is cast by something nearer
//      than that (brows, nose, hair, a hat's brim), 0 where it is cast
//      farther (a roof, a tree), smooth in between.
// Projection values are GL's (column-major, eye space looking down -z).
namespace LLRHI
{
    // The sun's shadow cascades the fill reads: the skin it lights is near
    // the camera (the skin mark stops at some 15 to 25 m).
    constexpr U32 SKIN_FILL_CASCADES = 3;

    struct SkinFillMask
    {
        F32 value = 1.f;                // written into the bound target's R on skin
        F32 eye_value = 0.f;            // and on the eyes (0: they stay out of the fill)
    };

    struct SkinFillShrink
    {
        U32 mask_texture = 0;           // SkinFillMask's target
        U32 mask_width = 0;             // its size
        U32 mask_height = 0;
        U32 factor = 1;                 // mask texels per target texel, each way
    };

    struct SkinFillSoften
    {
        U32 shrunk_texture = 0;         // SkinFillShrink's target (the bound target's size)
    };

    struct SkinFillOcclusion
    {
        U32 mask_texture = 0;           // SkinFillMask's target (the light map's size)
        U32 soft_mask_texture = 0;      // SkinFillSoften's target, filtered bilinearly; 0: no fade
        F32 soft_uv_scale[2] = {};      // full-resolution pixel centre -> soft mask uv (1 / (its size x factor))
        F32 lift = 0.f;                 // 0..1: how much of the occlusion goes on skin
    };

    struct SkinFillLight
    {
        U32 depth_texture = 0;          // the scene depth (the bound target's own)
        U32 diffuse_texture = 0;        // G-buffer 0: base colour, alpha the fullbright mix
        U32 normal_texture = 0;         // G-buffer 2: encoded normal, alpha the G-buffer flag
        U32 soft_mask_texture = 0;      // SkinFillSoften's target; 0: no fade at the skin's edge
        F32 soft_uv_scale[2] = {};      // as SkinFillOcclusion's
        U32 light_map_texture = 0;      // the light map (r: the sun's shadow); with shadows only
        U32 shadow_textures[SKIN_FILL_CASCADES] = {};       // the sun's shadow maps; with shadows only
        F32 shadow_matrices[SKIN_FILL_CASCADES][16] = {};   // eye space -> shadow map texture space
        F32 shadow_far[SKIN_FILL_CASCADES] = {};            // each cascade's far end (view depth, m)
        F32 inverse_projection[16] = {};
        F32 light_direction[3] = {};    // towards the light, eye space, unit length
        F32 light_color[3] = {};        // linear, times the fill's strength (grey)
        F32 sun_luminance = 0.f;        // the light's luminance (linear), as it lights the skin
        F32 ambient_luminance = 0.f;    // the sky's ambient's luminance (linear), an estimate
        F32 sunlit_share = 1.f;         // the filled skin at most this share of the skin in full sun
        F32 directionality = 0.f;       // 0..1, see shape above
        F32 near_reach = 0.f;           // m
        F32 eye_share = 0.f;            // 0..1: the share of the fill on the eyes (0: none)
        bool shadows = false;           // the light map's r and the shadow maps are there to read
    };

    // On unless LLRHI_SKIN_FILL=0 (a switch for tests: the passes are then
    // never recorded). The viewer's setting (PlusSkinFill, off by default)
    // decides whether they run.
    extern bool sSkinFill;

    // Set by the Vulkan backend. Each records its pass into the bound draw
    // framebuffer; false when it could not (nothing recorded).
    extern bool (*sSkinFillMask)(const SkinFillMask& pass);
    extern bool (*sSkinFillShrink)(const SkinFillShrink& pass);
    extern bool (*sSkinFillSoften)(const SkinFillSoften& pass);
    extern bool (*sSkinFillOcclusion)(const SkinFillOcclusion& pass);
    extern bool (*sSkinFillLight)(const SkinFillLight& pass);
}

#endif // LL_LLRHISKINFILL_H

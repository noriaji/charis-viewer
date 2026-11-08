/**
 * @file llrhiglare.h
 * @brief Glare (HDR bloom) and the sun's lens flare: the passes' description
 *        and the backend's entry points (research/108).
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

#ifndef LL_LLRHIGLARE_H
#define LL_LLRHIGLARE_H

#include "stdtypes.h"

// Research/108. The viewer (plus/glare) draws, after the exposure is measured
// and before tone mapping, into the GL framebuffer it has bound for each pass:
//   1. GlareDownsample with prefilter: the scene's HDR target (mRT->screen,
//      linear, before exposure) into a target of half its size: a 13-tap
//      average of the exposed colour, only what lies above a soft threshold.
//   2. GlareDownsample: each level into one of half its size (the same 13 taps).
//   3. GlareUpsample: the next smaller level, tent-filtered, added to a level
//      (the blend adds), from the smallest up to the first.
//   4. GlareSunVisibility: a 1x1 target (R16F) that keeps how much of the sun
//      is seen, from the exposed brightness around its place on screen, mixed
//      with the value the target held (a running average across frames).
//   5. GlareComposite: the scene's HDR target; the bloom (the first level) and
//      the sun's lens flare, both in exposed units, divided by the exposure
//      and added (the tone mapping multiplies by the exposure again).
// The cross filter (star filter, research/119), between 3 and 4 (its own targets):
//   a. GlareStreakExtract: the scene's HDR target into a target of a quarter
//      of its size: the exposed colour of each 4x4 block, kept only where it
//      is far above white and brighter than everything on a ring around it
//      (a light, not a lit wall or the sky). Points of the sky (the scene's
//      depth at the far end: the stars) are weakened, but not the sun.
//   b. GlareStreak: one ray of the star, in a few passes (Kawase): each pass
//      adds up taps behind the texel along the ray's direction, weighted so
//      that the passes together fall off exponentially with the distance.
//      The last pass of each ray adds to (the first: replaces) the target the
//      composite reads.
//   GlareComposite then adds the rays, less the extracted light itself (each
//   ray holds it once at its root).
// Places on screen are texture coordinates: (0, 0) at the first texel of the
// GL target (bottom left as GL draws it), (1, 1) at the opposite corner.
// The exposure is the official one: the exposure map's texel (1x1 R16F) times
// the RenderExposure setting, as the tone mapping applies it.
namespace LLRHI
{
    struct GlareExposure
    {
        U32 texture = 0;                // GL name of the exposure map (1x1)
        F32 scale = 1.f;                // RenderExposure, as the tone mapping clamps it; 0: none (1, the map not read: "no post")
    };

    struct GlareDownsample
    {
        U32 source_texture = 0;         // GL name of the level above (filtered bilinearly)
        U32 source_width = 0;
        U32 source_height = 0;
        U32 width = 0;                  // the bound target's size
        U32 height = 0;
        bool prefilter = false;         // the first level: exposure, Karis average, threshold
        GlareExposure exposure;         // prefilter only
        F32 threshold = 1.f;            // prefilter only: exposed brightness (max of RGB) where the glare starts
        F32 knee = 0.5f;                // prefilter only: the soft range around it
    };

    struct GlareUpsample
    {
        U32 source_texture = 0;         // GL name of the next smaller level (filtered bilinearly)
        U32 source_width = 0;
        U32 source_height = 0;
        U32 width = 0;                  // the bound target's size (the level it is added to)
        U32 height = 0;
    };

    struct GlareSunVisibility
    {
        U32 screen_texture = 0;         // GL name of the scene's HDR target
        GlareExposure exposure;
        F32 sun_place[2] = {};          // the sun on screen (may be outside 0..1)
        F32 radius[2] = {};             // the sampled disc's radius, in texture coordinates each way
        F32 dark = 0.8f;                // exposed brightness: below, the sun counts as hidden
        F32 bright = 2.5f;              // and above, as seen
        F32 blend = 0.2f;               // the share of this frame's value in the target's (1: no averaging)
    };

    struct GlareStreakExtract
    {
        U32 screen_texture = 0;         // GL name of the scene's HDR target (filtered bilinearly)
        U32 screen_width = 0;
        U32 screen_height = 0;
        U32 width = 0;                  // the bound target's size (a quarter of the screen's each way)
        U32 height = 0;
        GlareExposure exposure;
        F32 threshold = 1.5f;           // exposed brightness (max of RGB) where the rays start
        F32 knee = 0.5f;                // the soft range around it
        F32 limit = 16.f;               // the exposed brightness of full rays (the cap)
        F32 ring_radius[2] = {};        // the ring around each texel, in texture coordinates each way
        F32 point_full = 0.25f;         // the ring's brightest at most this share of the texel's: a light
        F32 point_none = 0.75f;         // at least this share: part of a bright area, no rays
        U32 depth_texture = 0;          // GL name of the scene's depth (any size); 0: the sky is not told apart
        F32 sky = 1.f;                  // the light of points of the sky (depth 1: the stars) times this
        F32 sun_place[2] = {};          // the sun's place (texture coordinates); around it the sky is not weakened
        F32 sun_radius[2] = {};         // how far around it, each way; 0: no sun (down, or off the screen)
    };

    struct GlareStreak
    {
        U32 source_texture = 0;         // GL name of the extracted light or the ray's previous pass
        U32 width = 0;                  // the bound target's size (the source's too)
        U32 height = 0;
        F32 step[2] = {};               // from one tap to the next, in texture coordinates (towards the root)
        F32 tap_decay = 1.f;            // each tap's weight, times the one before
        U32 taps = 0;                   // taps per texel (the first at the texel itself, weight 1)
        bool add = false;               // added to the bound target (the rays after the first)
    };

    struct GlareComposite
    {
        U32 width = 0;                  // the bound target's size (the scene's HDR target)
        U32 height = 0;
        GlareExposure exposure;
        // The bloom: 0 for none.
        U32 bloom_texture = 0;          // GL name of the first level (all levels added up)
        U32 bloom_width = 0;
        U32 bloom_height = 0;
        F32 bloom_intensity = 0.f;      // times the first level, in exposed units
        // The lens flare: 0 for none.
        U32 visibility_texture = 0;     // GL name of GlareSunVisibility's target
        F32 sun_place[2] = {};          // as GlareSunVisibility's
        F32 flare_intensity = 0.f;      // already faded for a sun outside the screen
        F32 flare_color[3] = {};        // linear, about 1 in brightness (the sun's colour)
        // The cross filter's rays: 0 for none.
        U32 streak_texture = 0;         // GL name of the rays (all added up)
        U32 extract_texture = 0;        // GL name of the extracted light (the same size)
        U32 streak_width = 0;
        U32 streak_height = 0;
        F32 streak_intensity = 0.f;     // times the rays, in exposed units
        F32 streak_rays = 0.f;          // how many rays hold the extracted light at their root
    };

    // On unless LLRHI_GLARE=0 (a switch for tests: the passes are then never
    // recorded). The viewer's settings (PlusGlare, PlusLensFlare,
    // PlusCrossFilter, all off by default) decide whether they run.
    extern bool sGlare;

    // Set by the Vulkan backend. Each records its pass into the bound draw
    // framebuffer; false when it could not (nothing recorded).
    extern bool (*sGlareDownsample)(const GlareDownsample& pass);
    extern bool (*sGlareUpsample)(const GlareUpsample& pass);
    extern bool (*sGlareSunVisibility)(const GlareSunVisibility& pass);
    extern bool (*sGlareStreakExtract)(const GlareStreakExtract& pass);
    extern bool (*sGlareStreak)(const GlareStreak& pass);
    extern bool (*sGlareComposite)(const GlareComposite& pass);
}

#endif // LL_LLRHIGLARE_H

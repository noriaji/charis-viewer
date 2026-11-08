/**
 * @file shadowdist.cpp
 * @brief The shadow distance (PlusShadowDistance): how far the sun's shadows
 *        reach, in four steps, and the shadow detail (PlusShadowPrecision): how
 *        thin a shadow survives at a distance, in three. Registers with the
 *        plus foundation (research/107-shadow-distance.md).
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

// What decides where the sun's shadows fade (official code, research/107):
//  - LLPipeline::generateSunShadow() splits the view into four cascades. The
//    last one ends at twice the depth of the farthest thing in view, at most
//    512 m and at most the draw distance; the shader fades the shadows out over
//    the second half of it. The splits are at (i/4)^e of that distance
//    (RenderShadowSplitExponent, 3 by default: most of the detail near the eye).
//  - Each cascade's map is as large as the screen (RenderShadowResolutionScale),
//    so a cascade that covers more ground has coarser texels, and the depth bias
//    (RenderShadowBias, a constant in the map's depth units) is a longer gap in
//    metres: four to nine texels. With the official splits, from the third
//    cascade on (24 m at a draw distance of 256, 48 m at 512) the shadow of a
//    person or a chair is mostly lost in that gap.
//  - The sun light map is then blurred in screen space (RenderShadowBlurSize,
//    about 12 pixels each way), the same number of pixels at any distance
//    (RenderShadowBlurDistFactor 0): far away that is wider than a leg's
//    shadow, which thins out and breaks up.
//
// The shadow distance sets the farthest distance, the split exponent and the
// maps' resolution together. The shadow detail sets the bias as a length
// along the light per cascade (a few texels plus what the sun's height needs
// on flat ground, never more than the official one; applied by moving the
// receivers in the sampling matrix, so the shaders are untouched), narrows the
// blur with distance and refines the maps. The snapshot boost takes the
// farthest and the finest for the frames of a snapshot.

#include "llviewerprecompiledheaders.h"

#include "plus.h"

#include "shadowdist.h"

#include "presets/presets.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llcallbacklist.h"
#include "llcamera.h"
#include "llfloater.h"
#include "llframetimer.h"
#include "llfloaterreg.h"
#include "llgl.h"
#include "llrendertarget.h"
#include "llrhi.h"
#include "lluictrl.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llvoavatarself.h"
#include "pipeline.h"

#include "glm/gtc/matrix_transform.hpp"

#include <algorithm>
#include <cmath>
#include <memory>

extern bool gCubeSnapshot;      // llviewerdisplay.cpp: a reflection probe face or mirror is being drawn

namespace
{
    constexpr char SETTING_LEVEL[] = "PlusShadowDistance";
    constexpr char SETTING_PRECISION[] = "PlusShadowPrecision";
    constexpr char SETTING_SOFTNESS[] = "PlusShadowSoftness";
    constexpr char SETTING_FOCUS[] = "PlusShadowFocus";
    constexpr char SETTING_DIAGNOSTICS[] = "PlusShadowDiagnostics";
    constexpr char SETTING_CASTER_BIAS[] = "PlusShadowCasterBias";
    constexpr char SETTING_NORMAL_OFFSET[] = "PlusShadowNormalOffset";

    constexpr S32 OFFICIAL = -1;    // either setting: the official behaviour (not in the UI; for comparisons)
    constexpr S32 LEVEL_DEFAULT = 1;
    constexpr S32 PRECISION_DEFAULT = 0;

    struct Level
    {
        const char* name;       // for the log
        F32 max_distance;       // m: the end of the last cascade
        F32 split_exponent;     // RenderShadowSplitExponent's
        F32 resolution;         // the sun maps', times RenderShadowResolutionScale
    };

    // From a model of the cascades (research/107 section 2): each step keeps a
    // person's shadow (the bias under about half a metre, texels under about
    // 15 cm) as far as it can within its distance.
    constexpr Level LEVELS[] =
    {
        { "near",      96.f, 2.0f,  1.0f },
        { "normal",   192.f, 2.25f, 1.0f },
        { "far",      384.f, 2.5f,  1.5f },
        { "very far", 512.f, 2.5f,  2.0f },
    };
    constexpr S32 LEVEL_COUNT = S32(sizeof(LEVELS) / sizeof(LEVELS[0]));

    // The frames of a snapshot (the snapshot boost): as far as the official
    // viewer goes, with maps twice as fine.
    constexpr Level SNAPSHOT_BOOST = { "snapshot boost", 512.f, 2.5f, 2.0f };

    struct Precision
    {
        const char* name;       // for the log
        F32 bias_texels;        // the receivers' bias on ground facing the light, in this cascade's texels
                                // (plus the ground's slope, without the casters' bias)
        F32 receiver_texels;    // the receivers' whole bias with the casters' slope-scaled bias
        F32 resolution;         // the sun maps', at least (the distance's may be finer)
    };

    // research/107 sections 10 and 20.
    constexpr Precision PRECISIONS[] =
    {
        { "normal",    2.0f, 1.5f, 1.0f },
        { "fine",      1.0f, 1.0f, 1.5f },
        { "very fine", 0.5f, 1.0f, 2.0f },
    };
    constexpr S32 PRECISION_COUNT = S32(sizeof(PRECISIONS) / sizeof(PRECISIONS[0]));
    const Precision& SNAPSHOT_BOOST_PRECISION = PRECISIONS[PRECISION_COUNT - 1];

    // The shadow softness (PlusShadowSoftness): the screen-space blur of the sun
    // light map (renderDeferredLighting). 13 taps each way, steps of 1.5 x
    // RenderShadowBlurSize pixels, Gaussian weights of sigma RenderShadowGaussian
    // (x: the sun's shadow, y: ambient occlusion). The official blur is the same
    // number of pixels at every distance; here it reaches at most `reach`
    // metres each way (RenderShadowBlurDistFactor narrows it beyond that), so
    // near shadows keep their edges and far ones are not smeared away. Only the
    // shadow's sigma changes: ambient occlusion keeps its smoothing.
    struct Softness
    {
        const char* name;       // for the log
        F32 shadow_sigma;       // RenderShadowGaussian.x: in units of two taps (4.2 pixels at the official size)
        F32 blur_size;          // times RenderShadowBlurSize
        F32 reach;              // m, each way
    };

    // research/107 sections 12 and 16. The official sigma (3) weighs every tap
    // almost the same: a box of 25 pixels, which halves a 10 cm shadow from
    // 10 m on. The shader's in-between taps take the mean of their neighbours,
    // so even the smallest sigma keeps a +-2 pixel blur. The edges' texel steps
    // are smoothed by the shadow maps' tent (shadowUtil.glsl), not by this blur.
    constexpr Softness SOFTNESSES[] =
    {
        { "sharp",           0.35f, 1.0f, 0.03f },
        { "slightly sharp",  0.6f,  1.0f, 0.05f },
        { "normal",          1.0f,  1.0f, 0.08f },
        { "slightly soft",   2.0f,  1.0f, 0.15f },
        { "soft",            3.0f,  1.5f, 0.3f  },
    };
    constexpr S32 SOFTNESS_COUNT = S32(sizeof(SOFTNESSES) / sizeof(SOFTNESSES[0]));
    constexpr S32 SOFTNESS_DEFAULT = 2;
    constexpr F32 BLUR_TAPS_REACH = 6.f;    // the farthest tap, in steps (blurLightF.glsl: k.z up to 3, times 2)
    constexpr F32 BLUR_STEP = 1.5f;         // pixels a step per RenderShadowBlurSize (kern_length / 2 - 0.5)

    // Ground the light grazes needs more bias: across a texel its depth along
    // the light changes by texel x its slope in the map (cot of the sun's
    // height in all, shared between the map's x and y). The shader's tent
    // (shadowUtil.glsl, research/107 section 24) compares texels up to 2 away
    // from the read in x and in y.
    constexpr F32 SLOPE_TEXELS_X = 2.0f;
    constexpr F32 SLOPE_TEXELS_Y = 2.0f;

    // The casters' slope-scaled depth bias (PlusShadowCasterBias, Vulkan): the
    // map's depth of every caster is pushed back by CASTER_SLOPE times its own
    // depth change across a texel. The ground's texels the tent compares (2
    // away at most, half a texel to spare) then stay behind it, so the receivers keep only
    // receiver_texels. Clamped (where the GPU can) at what ground twice as
    // steep as the sun's needs, and never below a 63-degree slope's: a caster
    // the light grazes (the edge of a leg) is not pushed back further.
    constexpr F32 CASTER_SLOPE = 2.5f;
    constexpr F32 CASTER_CLAMP_SLOPES = 2.f;
    constexpr F32 MAX_SLOPE = 6.f;          // sun about 9.5 degrees up; lower, the official bias decides
    constexpr F32 MIN_SUN_SINE = 0.05f;
    constexpr F32 PROBE_STEP = 0.5f;        // m: the step that measures a cascade's texel and depth scale
    constexpr F32 BAND_NEAR = 0.75f;        // the shader's bands around each split (shadowUtil.glsl)
    constexpr F32 BAND_FAR = 1.25f;

    // Finer sun maps than the official scale gives are made only while the
    // four maps and their copies in LLRHI's static sun shadow cache (four
    // more), 4 bytes a texel, take at most this share of the VRAM, and no side
    // is longer than MAX_MAP_SIZE (or what the GPU allows).
    constexpr F64 VRAM_SHARE = 1.0 / 16.0;
    constexpr F64 SUN_MAPS = 8.0;
    constexpr F64 BYTES_PER_TEXEL = 4.0;
    constexpr U32 FALLBACK_VRAM_MB = 2048;  // VRAM not known: count on a small card
    constexpr S32 MAX_MAP_SIZE = 8192;
    constexpr F64 SIZE_ROUNDING = 16.0;     // BlurHappySize() may add up to this

    // The cascades of a new step are logged once they have been drawn.
    constexpr S32 LOG_AFTER_FRAMES = 3;

    // The shadow focus (PlusShadowFocus): 1 fits the splits to the subject
    // (the camera's focus: the avatar, or what the camera was pointed at), 0
    // keeps them spread from the camera (the distance's exponent). The
    // subject's shadow is read by the cascade that holds it alone: between
    // 1.25 x the split before and 0.75 x its own (shadowUtil.glsl's bands),
    // so that cascade ends right past it instead of wherever the exponent puts it.
    constexpr S32 FOCUS_SUBJECT = 1;
    constexpr S32 FOCUS_DEFAULT = FOCUS_SUBJECT;
    constexpr F32 SUBJECT_BODY = 1.0f;          // m: around the subject (half an avatar's width, the ground at its feet)
    constexpr F32 SUBJECT_HEIGHT = 1.8f;        // m: what casts its shadow
    constexpr F32 MAX_SHADOW_LENGTH = 8.f;      // m: a low sun's shadow is kept this long at most
    constexpr F32 MIN_FOREGROUND = 2.f;         // m: a cascade before the subject's ends no nearer
    constexpr F32 MIN_SPLIT_RATIO = 1.25f;      // between one split and the next
    constexpr F32 FIT_GAIN = 0.9f;              // fitted only when the subject's cascade ends at least 10% nearer
    constexpr F32 DEPTH_STEP = 1.05f;           // the subject's depth in 5% steps: splits stay put (the shadow cache)
    constexpr F32 STEP_HOLD = 0.75f;            // a step is left only when the depth is this many steps from it

    // Stable cascades (PlusShadowStabilize, research/107 section 38): the
    // official cascades are fitted every frame to the slice's points in a
    // light view that turns with the camera (and some are perspective), so the
    // shadow maps' texel grid slides, turns and stretches as the camera moves:
    // fine shadows (leaves) shimmer. Here each cascade is orthographic, in a
    // light view anchored at the agent origin whose "up" turns with the camera
    // only in STABLE_TURN steps, sized in STABLE_SIZE_STEPS per doubling, and
    // its centre snapped to whole texels: from frame to frame the grid stays
    // put unless a step is crossed.
    constexpr char SETTING_STABILIZE[] = "PlusShadowStabilize";
    // World-locked grid (PlusShadowWorldLock, research/297): a thin lit gap (a fence's slit) is under a
    // texel, so whether it shows depends on the grid's phase against it. The grid then does not turn with
    // the camera at all (the up is the fixed reference), and its size is that of the receivers' box's
    // diagonal on both axes (what any turn of the box fits in; the texels need not be square), on a coarser ladder that is left only
    // when clearly too small or too large: turning the camera mostly leaves it, and the grid stays put.
    constexpr char SETTING_WORLD_LOCK[] = "PlusShadowWorldLock";
    constexpr F32 LOCK_SIZE_STEPS = 4.f;        // 19% a step
    constexpr F32 LOCK_SIZE_SLACK = 3.f;        // steps a kept size may be too large before it shrinks (2^(3/4) = 1.68x of the stepped size: a turned box's diagonal changes by up to 1.41x, plus a ladder step)
    constexpr F32 STABLE_TURN = 22.5f * F_PI / 180.f;
    constexpr F32 STABLE_TURN_HOLD = 0.6f;     // a turn step is left this far past its middle (in steps)
    constexpr F32 STABLE_SIZE_STEPS = 8.f;      // 9% a step
    constexpr F32 STABLE_SIZE_SLACK = 2.f;      // steps a kept size may be too large before it shrinks
    constexpr F32 STABLE_LOOKING_AT_LIGHT = 0.95f;  // |view . light| above this: the camera's up is used
    constexpr F32 STABLE_MIN_DEPTH = 1.f;       // m: the least depth range
    constexpr F32 STABLE_INSIDE = 1.0001f;      // receivers inside the stable cascade's clip box (float slack)

    // The depth range in steps (research/151 2.3): with the shadow detail on
    // (its biases are lengths, so a wider range changes no comparison), the
    // range along the light runs from whole step to whole step, a step at least
    // 1 / Z_STEPS of the receivers' depth (a power of two in metres), with a
    // step to spare at each end, and is kept while they stay inside it and it
    // is not much longer than they need.
    // A cached cascade then holds still in depth too, and plus/shadowshift can
    // move it by whole texels. On with PlusShadowShift, or alone for
    // comparisons (PlusShadowZRound).
    constexpr char SETTING_SHIFT[] = "PlusShadowShift";
    constexpr char SETTING_Z_ROUND[] = "PlusShadowZRound";
    constexpr F32 Z_STEPS = 8.f;                // the receivers' depth over the step (cascade 0: Z_STEPS_FIRST)
    constexpr F32 Z_STEPS_FIRST = 4.f;          // the first cascade is small: its depth moves faster
    constexpr F32 Z_SLACK_STEPS = 4.f;          // a kept range may be this many steps longer than needed

    struct KeptDepth
    {
        bool valid = false;
        F32 lo = 0.f;       // the view's z (along -light): far end
        F32 hi = 0.f;       // near end (towards the light)
    };
    KeptDepth sKeptDepth[4];

    // The z hold (research/178 C1): the depth range in steps whenever the
    // static sun shadow cache is on (see depthRoundActive). On unless
    // LLRHI_SHADOW_Z_HOLD=0; shadowzhold.toggle (LLRHI_CUE_DIR) flips it.
    constexpr char Z_HOLD_ENV[] = "LLRHI_SHADOW_Z_HOLD";
    constexpr char Z_HOLD_CUE[] = "shadowzhold.toggle";
    constexpr F64 Z_HOLD_CUE_POLL_SECONDS = 1.0;
    bool& zHold()
    {
        static bool on = LLRHI::envDefaultOn(Z_HOLD_ENV);
        return on;
    }

    struct StableGrid
    {
        F32 turn = 0.f;
        F32 size[2] = {};
    };
    StableGrid sStableGrid[4];
    U32 sStableRejected[4] = {};
    // A close-up (a portrait): the subject is the head, not the shadow on the
    // ground. From CLOSE_UP_FAR in to CLOSE_UP_NEAR, the room kept around the
    // subject shrinks smoothly to CLOSE_UP_BODY (face, hair, neck) and its
    // shadow's length to nothing, so the first cascade ends just past them
    // (research/107 sections 32 and 34).
    constexpr F32 CLOSE_UP_NEAR = 1.f;          // m
    constexpr F32 CLOSE_UP_FAR = 4.f;           // m
    constexpr F32 CLOSE_UP_BODY = 0.5f;         // m: a head with its hair and neck
    // Once fitted, the splits stay fitted until the subject's cascade would end
    // FIT_KEEP of the way to the distance's (not FIT_GAIN): no back and forth.
    constexpr F32 FIT_KEEP = 1.f;
    // The receivers' bias and the shader's texel for its normal offset never go
    // below these, however fine a close-up's texels get: the shadow pass and
    // the main view's skinning and depth differ by about that much.
    constexpr F32 MIN_RECEIVER_BIAS = 0.005f;   // m (shadowUtil.glsl: MIN_TEXEL_FOR_OFFSET 0.004 m)
    constexpr F32 MIN_SUBJECT_DEPTH = 0.2f;     // m

    // shadowUtil.glsl's normal offset (plus_normal_offset_texels, set from
    // NORMAL_OFFSET_TEXELS; NORMAL_OFFSET_MAX): the most a receiver moves (the
    // receiver cull) and what the diagnostics show.
    constexpr F32 NORMAL_OFFSET_TEXELS = 1.5f;
    constexpr F32 NORMAL_OFFSET_MAX = 0.1f;     // m

    // Diagnostics (PlusShadowDiagnostics, off): a line every few seconds.
    constexpr F32 DIAGNOSTICS_INTERVAL = 3.f;   // s
    constexpr F32 THIN_SHADOW = 0.1f;           // m: the shadow the line measures against (a leg)

    // The main view's sun maps as last made (allocateShadowBuffer).
    struct Allocation
    {
        F32 official_scale = 1.f;
        U32 res_x = 0;
        U32 res_y = 0;
        F32 factor = 1.f;       // on official_scale
    };
    Allocation sAllocation;

    // The main view's cascades as last drawn, for the log and the diagnostics
    // (gPipeline.mSunClipPlanes may hold a reflection probe's by then).
    F32 sCascadeSplit[4] = {};      // m: the end of each (the shader's shadow_clip)
    F32 sCascadeBias[4] = {};       // m: the bias each got
    F32 sCascadeTexel[4] = {};      // m: the larger side of its texels
    F32 sSunSine = 1.f;             // the sun's (or moon's) height, as a sine
    F32 sSubjectDepth = -1.f;       // m: the subject's eye depth the splits were fitted to (-1: not fitted)
    F32 sSubjectRadius = 0.f;       // m: what was kept around it
    F32 sBlurFactor = 0.f;          // RenderShadowBlurDistFactor as last used
    F32 sBlurSize = 0.f;            // RenderShadowBlurSize as last used
    F32 sBlurSigma = 0.f;           // RenderShadowGaussian.x as last used
    F32 sCasterClamp[4] = {};       // the casters' depth bias clamp per cascade ([0, 1] depth)
    S32 sCasterBiasState = -1;      // what the static shadow cache was last drawn with (-1: not yet)

    S32 levelSetting()
    {
        static LLCachedControl<S32> level(gSavedSettings, SETTING_LEVEL, LEVEL_DEFAULT);
        return level;
    }

    S32 precisionSetting()
    {
        static LLCachedControl<S32> precision(gSavedSettings, SETTING_PRECISION, PRECISION_DEFAULT);
        return precision;
    }

    S32 softnessSetting()
    {
        static LLCachedControl<S32> softness(gSavedSettings, SETTING_SOFTNESS, SOFTNESS_DEFAULT);
        return softness;
    }

    S32 focusSetting()
    {
        static LLCachedControl<S32> focus(gSavedSettings, SETTING_FOCUS, FOCUS_DEFAULT);
        return focus;
    }

    // The softness in effect (the user's choice, snapshots too): none for the
    // official blur, and for probes and mirrors (they are not blurred anyway).
    const Softness* activeSoftness()
    {
        const S32 softness = softnessSetting();
        if (gCubeSnapshot || softness <= OFFICIAL)
        {
            return nullptr;
        }
        return &SOFTNESSES[llclamp(softness, 0, SOFTNESS_COUNT - 1)];
    }

    const Level& levelRow(S32 level)
    {
        return LEVELS[llclamp(level, 0, LEVEL_COUNT - 1)];
    }

    const Precision& precisionRow(S32 precision)
    {
        return PRECISIONS[llclamp(precision, 0, PRECISION_COUNT - 1)];
    }

    // The steps in effect for the main view: none for the official behaviour.
    const Level* activeLevel()
    {
        const S32 level = levelSetting();
        if (gCubeSnapshot || level <= OFFICIAL)
        {
            return nullptr;
        }
        return plus::presets::snapshotBoostActive() ? &SNAPSHOT_BOOST : &levelRow(level);
    }

    const Precision* activePrecision()
    {
        const S32 precision = precisionSetting();
        if (gCubeSnapshot || precision <= OFFICIAL)
        {
            return nullptr;
        }
        return plus::presets::snapshotBoostActive() ? &SNAPSHOT_BOOST_PRECISION : &precisionRow(precision);
    }

    // The casters' slope-scaled bias: with the detail on, in Vulkan, unless
    // turned off (PlusShadowCasterBias, for comparisons).
    bool casterBiasActive()
    {
        static LLCachedControl<bool> caster_bias(gSavedSettings, SETTING_CASTER_BIAS, true);
        return caster_bias && LLRHI::sVulkanGL && activePrecision() != nullptr;
    }

    // The depth range in steps (research/151 2.3). Not with the official
    // detail: its bias is a depth in the map's units, which a longer range
    // would make longer in metres.
    // research/178 C1: with the static sun shadow cache on, too (the z hold).
    // A tight depth range follows the receivers' highest and lowest points every
    // frame, and moving receivers (turning prims, avatars, attachments) move them
    // a little all the time: the receivers leave the cached cascade along the
    // light and it is drawn again whole, about half of them every frame in a busy
    // place (research/178 1.3). Kept in steps, the cached cascade holds them.
    // The shadows look the same (the detail's biases are lengths, research/151
    // 2.3; the receivers' bias and the casters' clamp come from the cascade's
    // projection as used, cached or not).
    bool depthRoundActive()
    {
        static LLCachedControl<bool> shift(gSavedSettings, SETTING_SHIFT, false);
        static LLCachedControl<bool> z_round(gSavedSettings, SETTING_Z_ROUND, false);
        return (shift || z_round || (zHold() && LLRHI::sCacheSunShadows)) && activePrecision() != nullptr;
    }

    // At most once a second, with LLRHI_CUE_DIR only: shadowzhold.toggle flips
    // the z hold for an A/B in one start (tools/native-ab.ps1).
    void pollZHoldCue()
    {
        static LLRHI::CuePoll cue_poll(Z_HOLD_CUE_POLL_SECONDS);
        if (!cue_poll.due() || !LLRHI::takeCue(Z_HOLD_CUE))
        {
            return;
        }
        zHold() = !zHold();
        LL_INFOS("RHIProfile") << "shadow z hold (research/178 C1) " << (zHold() ? "on" : "off") << LL_ENDL;
    }

    // Cascade j's depth range for receivers from lo to hi (the view's z):
    // near_z (towards the light) and far_z, from whole step to whole step.
    void roundDepthRange(S32 j, F32 lo, F32 hi, F32& near_z, F32& far_z)
    {
        const F32 needed = llmax(hi - lo, STABLE_MIN_DEPTH);
        const F32 step = std::exp2(std::ceil(std::log2(needed / (j == 0 ? Z_STEPS_FIRST : Z_STEPS))));
        KeptDepth& kept = sKeptDepth[j];
        const bool keep = kept.valid && kept.lo <= lo && kept.hi >= hi && kept.hi - kept.lo <= needed + Z_SLACK_STEPS * step;
        if (!keep)
        {
            kept.lo = std::floor((lo - step) / step) * step;
            kept.hi = std::ceil((hi + step) / step) * step;
            kept.valid = std::isfinite(kept.lo) && std::isfinite(kept.hi);
        }
        // Not finite (nor are the receivers): the check after it rejects the cascade.
        near_z = kept.valid ? kept.hi : hi;
        far_z = kept.valid ? kept.lo : lo;
    }

    // The finer of what the distance and the detail want.
    F32 wantedResolution(S32 level, S32 precision, bool boost)
    {
        F32 wanted = 1.f;
        if (level > OFFICIAL)
        {
            wanted = llmax(wanted, boost ? SNAPSHOT_BOOST.resolution : levelRow(level).resolution);
        }
        if (precision > OFFICIAL)
        {
            wanted = llmax(wanted, boost ? SNAPSHOT_BOOST_PRECISION.resolution : precisionRow(precision).resolution);
        }
        return wanted;
    }

    // The factor on the official scale for maps of res_x x res_y: the wanted
    // one, within the VRAM share and the largest map. Never below 1 (the
    // official maps are never made smaller).
    F32 budgetedResolution(F32 wanted, F32 official_scale, U32 res_x, U32 res_y)
    {
        if (wanted <= 1.f || official_scale <= 0.f || res_x == 0 || res_y == 0)
        {
            return 1.f;
        }
        const U32 vram_mb = gGLManager.mVRAM > 0 ? gGLManager.mVRAM : FALLBACK_VRAM_MB;
        const F64 budget = F64(vram_mb) * 1024.0 * 1024.0 * VRAM_SHARE;
        const F64 texels = F64(res_x) * official_scale * F64(res_y) * official_scale;
        const F64 by_vram = std::sqrt(budget / (texels * SUN_MAPS * BYTES_PER_TEXEL));

        const S32 gpu_max = gGLManager.mGLMaxTextureSize > 0 ? gGLManager.mGLMaxTextureSize : MAX_MAP_SIZE;
        const F64 max_side = F64(llmin(gpu_max, MAX_MAP_SIZE)) - SIZE_ROUNDING;
        const F64 by_size = max_side / (F64(llmax(res_x, res_y)) * official_scale);

        const F64 limit = llmax(1.0, llmin(by_vram, by_size));
        return F32(llmin(F64(wanted), limit));
    }

    // The sun maps are made again (at the start of the next frame) when the
    // factor they would get now differs from the one they have.
    void resizeIfChanged(S32 level, S32 precision, bool boost)
    {
        if (sAllocation.res_x == 0)
        {
            return;     // none made yet: the first allocation reads the settings
        }
        const F32 factor = budgetedResolution(wantedResolution(level, precision, boost), sAllocation.official_scale,
                                              sAllocation.res_x, sAllocation.res_y);
        if (factor != sAllocation.factor)
        {
            gPipeline.requestResizeShadowTexture();
        }
    }

    // ------------------------------------------------------------------
    // The bias as a length along the light

    // Where agent-space point p lands in the cascade's NDC; false behind its eye.
    bool toNdc(const glm::mat4& proj, const glm::vec3& light_view_point, glm::vec3& ndc)
    {
        const glm::vec4 clip = proj * glm::vec4(light_view_point, 1.f);
        if (clip.w <= 1e-4f)
        {
            return false;
        }
        ndc = glm::vec3(clip) / clip.w;
        return true;
    }

    struct CascadeScale
    {
        F32 texel_x = 0.f;          // m, a texel along the map's x
        F32 texel_y = 0.f;          // and y
        F32 depth_per_m = 0.f;      // NDC depth lost per metre along the view's +z (> 0: +z runs towards the light)

        F32 texel() const { return llmax(texel_x, texel_y); }
    };

    // A texel's size and the depth scale of the cascade at a point (light view space).
    bool measure(const glm::mat4& proj, const glm::vec3& p, U32 width, U32 height, CascadeScale& out)
    {
        glm::vec3 c, x, y, z;
        if (!toNdc(proj, p, c) ||
            !toNdc(proj, p + glm::vec3(PROBE_STEP, 0.f, 0.f), x) ||
            !toNdc(proj, p + glm::vec3(0.f, PROBE_STEP, 0.f), y) ||
            !toNdc(proj, p + glm::vec3(0.f, 0.f, PROBE_STEP), z))
        {
            return false;
        }
        // A texel is 2 / size of NDC; the probe's step moves by |d| NDC. The
        // map's x and y follow the view's x and y (a perspective cascade warps
        // along y, its depth along z).
        const F32 dx = std::fabs(x.x - c.x);
        const F32 dy = std::fabs(y.y - c.y);
        if (dx <= 0.f || dy <= 0.f)
        {
            return false;
        }
        out.texel_x = PROBE_STEP * 2.f / (dx * F32(width));
        out.texel_y = PROBE_STEP * 2.f / (dy * F32(height));
        // The view's +z runs towards or away from the light depending on the
        // look matrix: the sign tells which.
        out.depth_per_m = -(z.z - c.z) / PROBE_STEP;
        return std::fabs(out.depth_per_m) > 0.f;
    }

    // ------------------------------------------------------------------
    // The log

    void logCascades(const char* what)
    {
        const F32* c = sCascadeSplit;
        const LLRenderTarget& map = gPipeline.mRT->shadow[0];
        LL_INFOS("Plus") << what << ": sun shadow cascades end at " << llformat("%.1f / %.1f / %.1f / %.1f", c[0], c[1], c[2], c[3])
                         << " m (draw distance " << llformat("%.0f", gAgentCamera.mDrawDistance) << " m), maps "
                         << map.getWidth() << "x" << map.getHeight() << ", texels "
                         << llformat("%.1f / %.1f / %.1f / %.1f", sCascadeTexel[0] * 100.f, sCascadeTexel[1] * 100.f,
                                     sCascadeTexel[2] * 100.f, sCascadeTexel[3] * 100.f)
                         << " cm, bias "
                         << llformat("%.1f / %.1f / %.1f / %.1f", sCascadeBias[0] * 100.f, sCascadeBias[1] * 100.f,
                                     sCascadeBias[2] * 100.f, sCascadeBias[3] * 100.f)
                         << " cm" << (activePrecision() ? "" : " (the official bias)")
                         << (sSubjectDepth > 0.f ? llformat(", fitted to a subject %.1f m away (+-%.1f m)", sSubjectDepth, sSubjectRadius) : "")
                         << LL_ENDL;
    }

    void describe()
    {
        const S32 level = levelSetting();
        const S32 precision = precisionSetting();
        std::string text = "Shadow distance: ";
        if (level <= OFFICIAL)
        {
            text += "official";
        }
        else
        {
            const Level& row = levelRow(level);
            text += llformat("%s (up to %.0f m, split exponent %.2f, sun shadow resolution x%.1f)", row.name, row.max_distance,
                             row.split_exponent, row.resolution);
        }
        text += "; shadow detail: ";
        if (precision <= OFFICIAL)
        {
            text += "official";
        }
        else
        {
            const Precision& row = precisionRow(precision);
            text += llformat("%s (bias %.1f texels + %.1f / %.1f per slope in x / y, resolution x%.1f)", row.name,
                             row.bias_texels, SLOPE_TEXELS_X, SLOPE_TEXELS_Y, row.resolution);
        }
        const S32 focus = focusSetting();
        text += focus == FOCUS_SUBJECT ? "; shadow focus: the subject" : (focus <= OFFICIAL ? "; shadow focus: official" : "; shadow focus: the camera");
        text += gSavedSettings.getBOOL(SETTING_STABILIZE) ? "; stable cascades" : "; official cascades";
        text += "; shadow softness: ";
        if (const Softness* softness = activeSoftness())
        {
            text += llformat("%s (shadow blur sigma %.1f, blur size x%.2f, at most %.0f cm each way)", softness->name,
                             softness->shadow_sigma, softness->blur_size, softness->reach * 100.f);
        }
        else
        {
            text += "official";
        }
        LL_INFOS("Plus") << text << LL_ENDL;
    }

    // The steps and the cascades, once a few frames have drawn them (the cached
    // settings have the new value by then).
    void logSettings()
    {
        auto frames_left = std::make_shared<S32>(LOG_AFTER_FRAMES);
        doOnIdleRepeating([frames_left]()
                          {
                              if (--*frames_left > 0)
                              {
                                  return false;
                              }
                              describe();
                              logCascades("Shadow distance");
                              return true;
                          });
    }

    void onSnapshotBoost(bool active)
    {
        if (!active)
        {
            logCascades("Snapshot boost");     // the snapshot's frames, still in place
        }
        resizeIfChanged(levelSetting(), precisionSetting(), active);
    }

    // The softness is a matter of taste: a change of it must not clear the
    // official "Preset in use", which Preferences > Graphics clears for any
    // edited control (LLPanelPreferenceGraphics::hasDirtyChilds) the next
    // time the preset list changes. Its combos forget the edit.
    void forgetSoftnessEdits()
    {
        for (const char* floater_name : { "preferences", "prefs_graphics_advanced" })
        {
            LLFloater* floater = LLFloaterReg::findInstance(floater_name);
            LLUICtrl* combo = floater ? floater->findChild<LLUICtrl>(SETTING_SOFTNESS, true) : nullptr;
            if (combo)
            {
                combo->resetDirty();
            }
        }
    }

    // ------------------------------------------------------------------
    // The subject (the shadow focus)

    // Splits fitted to the subject: in snapshots always, otherwise when chosen.
    bool focusActive()
    {
        const S32 focus = focusSetting();
        if (gCubeSnapshot || focus <= OFFICIAL)
        {
            return false;
        }
        return plus::presets::snapshotBoostActive() || focus == FOCUS_SUBJECT;
    }

    // The eye depth of the camera's focus (the avatar, or what the camera was
    // pointed at), in DEPTH_STEP steps; -1 in mouselook or behind the eye.
    F32 subjectDepth(const LLCamera& camera)
    {
        if (gAgentCamera.cameraMouselook())
        {
            return -1.f;
        }
        const LLVector3 focus = gAgent.getPosAgentFromGlobal(gAgentCamera.getFocusGlobal());
        const F32 depth = (focus - camera.getOrigin()) * camera.getAtAxis();
        if (depth <= MIN_SUBJECT_DEPTH)
        {
            return -1.f;
        }
        // In DEPTH_STEP steps, and the step kept until the depth is most of a
        // step past it (no back and forth at a step's edge).
        static F32 kept_step = -1.f;
        const F32 step = std::log(DEPTH_STEP);
        const F32 at = std::log(depth) / step;
        if (kept_step < 0.f || std::fabs(at - kept_step) > STEP_HOLD)
        {
            kept_step = std::round(at);
        }
        return std::exp(kept_step * step);
    }

    // How much of the cascade's reading the shader gives cascade k at eye
    // depth d (shadowUtil.glsl's weights: the bands 0.75 / 1.25 x each split).
    F32 cascadeWeight(S32 k, F32 d, const F32* c)
    {
        F32 w = 0.f;
        switch (k)
        {
        case 0:
            if (d < BAND_FAR * c[0]) w = 1.f - llmax(d - BAND_NEAR * c[0], 0.f) / ((BAND_FAR - BAND_NEAR) * c[0]);
            break;
        case 1:
        case 2:
            if (d > BAND_NEAR * c[k - 1] && d < BAND_FAR * c[k])
            {
                w = 1.f - llmax(BAND_FAR * c[k - 1] - d, 0.f) / ((BAND_FAR - BAND_NEAR) * c[k - 1]) -
                    llmax(d - BAND_NEAR * c[k], 0.f) / ((BAND_FAR - BAND_NEAR) * c[k]);
            }
            break;
        case 3:
            if (d > BAND_NEAR * c[2] && d < c[3]) w = 1.f - llmax(BAND_FAR * c[2] - d, 0.f) / ((BAND_FAR - BAND_NEAR) * c[2]);
            break;
        }
        return llmax(w, 0.f);
    }

    // Where the cascades that read eye depths lo..hi end, at the farthest
    // (what their texels follow: the frustum's width there).
    F32 farthestReader(F32 lo, F32 hi, const F32* c)
    {
        F32 end = 0.f;
        for (S32 k = 0; k < 4; ++k)
        {
            const F32 from = k == 0 ? 0.f : BAND_NEAR * c[k - 1];
            const F32 to = k == 3 ? c[3] : BAND_FAR * c[k];
            if (hi > from && lo < to)
            {
                end = llmax(end, to);
            }
        }
        return end;
    }

    // ------------------------------------------------------------------
    // Diagnostics

    void logDiagnostics()
    {
        if (!isAgentAvatarValid() || sCascadeSplit[3] <= 0.f)
        {
            return;
        }
        LLViewerCamera* camera = LLViewerCamera::getInstance();
        const F32 depth = (gAgentAvatarp->getPositionAgent() - camera->getOrigin()) * camera->getAtAxis();
        const U32 height = gPipeline.mRT->deferredScreen.getHeight();
        const F32 pixel_angle = height ? 2.f * std::tan(camera->getView() * 0.5f) / F32(height) : 0.f;
        const F32 cos_sun = std::sqrt(llmax(1.f - sSunSine * sSunSine, 0.f));

        std::string text = llformat("Shadow diagnostics: avatar %.2f m deep (camera near clip %.2f m)", depth, camera->getNear());
        text += sSubjectDepth > 0.f ? llformat(" (splits fitted to a subject %.1f m deep, +-%.1f m)", sSubjectDepth, sSubjectRadius)
                                    : " (splits not fitted)";
        text += llformat("; splits %.1f / %.1f / %.1f / %.1f m", sCascadeSplit[0], sCascadeSplit[1], sCascadeSplit[2], sCascadeSplit[3]);
        for (S32 k = 0; k < 4; ++k)
        {
            const F32 w = cascadeWeight(k, depth, sCascadeSplit);
            if (w <= 0.f)
            {
                continue;
            }
            const F32 texel = sCascadeTexel[k];
            text += llformat("; cascade %d (weight %.2f): texel %.1f cm (a %.0f cm shadow is %.1f texels), bias %.1f cm"
                             " (the shadow starts about %.0f cm from the feet), normal offset up to %.1f cm",
                             k, w, texel * 100.f, THIN_SHADOW * 100.f, texel > 0.f ? THIN_SHADOW / texel : 0.f,
                             sCascadeBias[k] * 100.f, sCascadeBias[k] * cos_sun * 100.f,
                             llmin(plus::shadowdist::normalOffsetTexels() * texel, NORMAL_OFFSET_MAX) * 100.f);
        }
        if (depth > 0.f && pixel_angle > 0.f)
        {
            // The blur's farthest tap and its sigma, in pixels and in metres at the avatar.
            const F32 shrink = llmax(depth * sBlurFactor, 1.f);
            const F32 reach_px = BLUR_TAPS_REACH * BLUR_STEP * sBlurSize / shrink;
            const F32 sigma_px = sBlurSigma * 2.f * BLUR_STEP * sBlurSize / shrink;
            const F32 metres_per_px = depth * pixel_angle;
            text += llformat("; blur up to %.1f px = %.1f cm each way, sigma %.1f px = %.1f cm; a %.0f cm shadow is %.1f px wide",
                             reach_px, reach_px * metres_per_px * 100.f, sigma_px, sigma_px * metres_per_px * 100.f,
                             THIN_SHADOW * 100.f, THIN_SHADOW / metres_per_px);
        }
        text += casterBiasActive() ? llformat("; casters' slope bias x%.1f", CASTER_SLOPE) : "; no casters' slope bias";
        text += llformat("; stable cascades rejected since the last line %u / %u / %u / %u", sStableRejected[0], sStableRejected[1],
                         sStableRejected[2], sStableRejected[3]);
        for (U32& rejected : sStableRejected)
        {
            rejected = 0;
        }
        text += llformat("; sun %.0f degrees up; maps %ux%u", std::asin(sSunSine) * 180.f / F_PI,
                         gPipeline.mRT->shadow[0].getWidth(), gPipeline.mRT->shadow[0].getHeight());
        LL_INFOS("Plus") << text << LL_ENDL;
    }

    // ------------------------------------------------------------------
    // Your avatar's shadow casters, every shadow pass (research/107 section 27)

    constexpr F32 AGENT_LOG_INTERVAL = 0.25f;  // s: lines about the same thing at most this often
    constexpr F32 AVATAR_BOX_MARGIN = 0.5f;    // m: pipeline.cpp's grown box (recv_group_box)

    struct AgentFrame
    {
        U32 kept[4] = {};       // per cascade index: your rigged groups the receiver cull kept
        U32 tested[4] = {};
        U32 skinned = 0;        // skinned casters the native passes drew (everyone's)
        LLVector3 box_size;     // your animation box's size (a change: recalculated)
        U32 frames_since_box = 0;
        F32 logged_at = -1.f;
    };
    AgentFrame sAgentLast;

    // How far p is outside the box lo..hi (0 inside).
    F32 outside(const LLVector3& p, const LLVector3& lo, const LLVector3& hi)
    {
        F32 d2 = 0.f;
        for (U32 i = 0; i < 3; ++i)
        {
            const F32 d = p.mV[i] < lo.mV[i] ? lo.mV[i] - p.mV[i] : p.mV[i] > hi.mV[i] ? p.mV[i] - hi.mV[i] : 0.f;
            d2 += d * d;
        }
        return std::sqrt(d2);
    }

    void checkAgentCasters(const S32* index, const U32* tested, const U32* kept)
    {
        const LLRHI::ShadowSkinnedCounts skinned = LLRHI::sShadowSkinned;
        LLRHI::sShadowSkinned = LLRHI::ShadowSkinnedCounts();
        static LLCachedControl<bool> enabled(gSavedSettings, SETTING_DIAGNOSTICS, false);
        if (!enabled || !isAgentAvatarValid())
        {
            return;
        }

        AgentFrame now;
        for (U32 k = 0; k < 4; ++k)
        {
            if (index[k] >= 0 && index[k] < 4)
            {
                now.kept[index[k]] = kept[k];
                now.tested[index[k]] = tested[k];
            }
        }
        now.skinned = skinned.casters;

        // The feet against the animation box the culls use (as LLVOAvatar left it
        // for this frame) and against the receiver cull's grown one.
        const LLVector3* box = gAgentAvatarp->getLastAnimExtents();
        now.box_size = box[1] - box[0];
        now.frames_since_box = now.box_size == sAgentLast.box_size ? sAgentLast.frames_since_box + 1 : 0;
        const LLVector3 centre = (box[0] + box[1]) * 0.5f;
        const LLVector3 half = now.box_size * 0.5f;
        const F32 reach = std::sqrt(half.mV[0] * half.mV[0] + half.mV[1] * half.mV[1]) + AVATAR_BOX_MARGIN;
        const LLVector3 grown_lo(centre.mV[0] - reach, centre.mV[1] - reach, box[0].mV[2] - AVATAR_BOX_MARGIN);
        const LLVector3 grown_hi(centre.mV[0] + reach, centre.mV[1] + reach, box[1].mV[2] + AVATAR_BOX_MARGIN);
        F32 feet_out = 0.f, feet_out_grown = 0.f;
        for (LLJoint* foot : { gAgentAvatarp->mFootLeftp, gAgentAvatarp->mFootRightp })
        {
            if (foot)
            {
                const LLVector3 p = foot->getWorldPosition();
                feet_out = llmax(feet_out, outside(p, box[0], box[1]));
                feet_out_grown = llmax(feet_out_grown, outside(p, grown_lo, grown_hi));
            }
        }

        std::string events;
        for (U32 k = 0; k < 4; ++k)
        {
            if (now.kept[k] < sAgentLast.kept[k] && now.tested[k] >= sAgentLast.tested[k])
            {
                events += llformat(" cascade %u: the receiver cull kept %u of your %u rigged groups (%u last pass);", k,
                                   now.kept[k], now.tested[k], sAgentLast.kept[k]);
            }
        }
        if (skinned.unresolved || skinned.no_palette || skinned.no_weights)
        {
            events += llformat(" skinned casters not drawn: %u without their buffers on the GPU, %u without room for"
                               " their palette, %u without skin weights;", skinned.unresolved, skinned.no_palette, skinned.no_weights);
        }
        if (sAgentLast.skinned && now.skinned * 10 < sAgentLast.skinned * 9)
        {
            events += llformat(" skinned casters drawn fell from %u to %u;", sAgentLast.skinned, now.skinned);
        }
        if (feet_out_grown > 0.f)
        {
            events += llformat(" a foot is %.2f m outside even the receiver cull's grown box;", feet_out_grown);
        }

        if (!events.empty() && gFrameTimeSeconds - sAgentLast.logged_at >= AGENT_LOG_INTERVAL)
        {
            now.logged_at = gFrameTimeSeconds;
            LL_INFOS("Plus") << "Shadow casters (frame " << LLFrameTimer::getFrameCount() << "):" << events
                             << llformat(" your animation box was recalculated %u frames ago; the feet are %.2f m outside it",
                                         now.frames_since_box, feet_out)
                             << LL_ENDL;
        }
        else
        {
            now.logged_at = sAgentLast.logged_at;
        }
        sAgentLast = now;
    }

    bool onIdleDiagnostics()
    {
        static LLCachedControl<bool> enabled(gSavedSettings, SETTING_DIAGNOSTICS, false);
        static F32 logged_at = 0.f;
        if (enabled && gFrameTimeSeconds - logged_at >= DIAGNOSTICS_INTERVAL)
        {
            logged_at = gFrameTimeSeconds;
            logDiagnostics();
        }
        return false;   // for the whole session
    }

    void init()
    {
        // Settings: app_settings/settings_plus.xml (PlusShadowDistance, PlusShadowPrecision,
        // PlusShadowSoftness); the first two are also written by the quality levels
        // (featuretable.txt, research/183; the softness is a matter of taste, not there).
        // UI: the photo tools (app_settings/plus/phototools/catalog.xml, tab "color" = Light & Color, Details),
        // Preferences > Graphics and Advanced Graphics (panel_preferences_graphics1.xml,
        // floater_preferences_graphics_advanced.xml). Read by the <plus> hooks in pipeline.cpp.
        // The handlers get the new value: the cached controls may not have it yet.
        if (LLControlVariable* control = gSavedSettings.getControl(SETTING_LEVEL).get())
        {
            control->getCommitSignal()->connect([](LLControlVariable*, const LLSD& value, const LLSD&)
                                                {
                                                    logSettings();
                                                    resizeIfChanged((S32)value.asInteger(), precisionSetting(),
                                                                    plus::presets::snapshotBoostActive());
                                                });
        }
        if (LLControlVariable* control = gSavedSettings.getControl(SETTING_PRECISION).get())
        {
            control->getCommitSignal()->connect([](LLControlVariable*, const LLSD& value, const LLSD&)
                                                {
                                                    logSettings();
                                                    resizeIfChanged(levelSetting(), (S32)value.asInteger(),
                                                                    plus::presets::snapshotBoostActive());
                                                });
        }
        if (LLControlVariable* control = gSavedSettings.getControl(SETTING_FOCUS).get())
        {
            control->getCommitSignal()->connect([](LLControlVariable*, const LLSD&, const LLSD&) { logSettings(); });
        }
        if (LLControlVariable* control = gSavedSettings.getControl(SETTING_SOFTNESS).get())
        {
            control->getCommitSignal()->connect([](LLControlVariable*, const LLSD&, const LLSD&)
                                                {
                                                    logSettings();
                                                    doOnIdleOneTime(&forgetSoftnessEdits);  // after the combo marks itself
                                                });
        }
        plus::presets::addSnapshotBoostListener(&onSnapshotBoost);
    }

    void onUIReady()
    {
        describe();
        doOnIdleRepeating(&onIdleDiagnostics);
    }

    plus::FeatureRegistration sRegistration({ "shadowdist", &init, &onUIReady });
}

namespace plus::shadowdist
{
    F32 maxDistance(F32 official_max)
    {
        const Level* level = activeLevel();
        return level ? level->max_distance : official_max;
    }

    void fitSplits(LLVector4& clip, F32 near_clip, F32 far_clip, const LLCamera& camera, const LLVector3& light_dir)
    {
        if (gCubeSnapshot)
        {
            return;
        }
        sSubjectDepth = -1.f;
        const F32 depth = focusActive() ? subjectDepth(camera) : -1.f;
        if (depth <= 0.f)
        {
            return;
        }

        // The subject's eye depths: its body, and its shadow on the ground
        // (away from the sun, as long as its height makes it, along the view).
        const F32 sun_sine = llclamp(std::fabs(light_dir.mV[2]), MIN_SUN_SINE, 1.f);
        const F32 shadow_length = llmin(SUBJECT_HEIGHT * std::sqrt(1.f - sun_sine * sun_sine) / sun_sine, MAX_SHADOW_LENGTH);
        LLVector3 shadow_dir(light_dir.mV[0], light_dir.mV[1], 0.f);
        LLVector3 view_dir(camera.getAtAxis().mV[0], camera.getAtAxis().mV[1], 0.f);
        F32 along = 0.f;
        if (shadow_dir.normVec() > 0.f && view_dir.normVec() > 0.f)
        {
            along = (shadow_dir * view_dir) * shadow_length;   // + away from the camera
        }
        // A close-up keeps the head only.
        const F32 t = llclamp((depth - CLOSE_UP_NEAR) / (CLOSE_UP_FAR - CLOSE_UP_NEAR), 0.f, 1.f);
        const F32 wide = t * t * (3.f - 2.f * t);    // smoothstep: no kink at either end
        const F32 body = CLOSE_UP_BODY + (SUBJECT_BODY - CLOSE_UP_BODY) * wide;
        along *= wide;
        const F32 lo = llmax(depth - body + llmin(along, 0.f), MIN_SUBJECT_DEPTH);
        const F32 hi = depth + body + llmax(along, 0.f);

        // The subject's cascade ends at hi / 0.75; the one before (if there is
        // room for it) at lo / 1.25. The rest share what is left to far_clip.
        F32 c[4];
        const F32 subject_end = hi / BAND_NEAR;
        const F32 foreground_end = lo / BAND_FAR;
        const bool foreground = foreground_end >= llmax(MIN_FOREGROUND, near_clip * MIN_SPLIT_RATIO) &&
                                subject_end >= foreground_end * MIN_SPLIT_RATIO;
        if (foreground)
        {
            if (far_clip < subject_end * MIN_SPLIT_RATIO * MIN_SPLIT_RATIO)
            {
                return;     // no room behind the subject
            }
            c[0] = foreground_end;
            c[1] = subject_end;
            c[2] = std::sqrt(subject_end * far_clip);
            c[3] = far_clip;
        }
        else
        {
            const F32 ratio = far_clip > subject_end ? std::pow(far_clip / subject_end, 1.f / 3.f) : 0.f;
            if (ratio < MIN_SPLIT_RATIO)
            {
                return;
            }
            c[0] = subject_end;
            c[1] = subject_end * ratio;
            c[2] = c[1] * ratio;
            c[3] = far_clip;
        }

        if (c[0] <= near_clip)
        {
            return;
        }

        // Only when the subject's cascade ends nearer than the distance's splits
        // would have it (its texels follow the frustum's width there).
        const F32 level[4] = { clip.mV[0], clip.mV[1], clip.mV[2], clip.mV[3] };
        const F32 fitted_end = BAND_FAR * subject_end;
        static bool fitted_last = false;
        if (fitted_end > (fitted_last ? FIT_KEEP : FIT_GAIN) * farthestReader(lo, hi, level))
        {
            fitted_last = false;
            return;
        }
        fitted_last = true;
        clip.set(c[0], c[1], c[2], c[3]);
        sSubjectDepth = depth;
        sSubjectRadius = 0.5f * (hi - lo);
    }

    bool stabilizeCascade(S32 j, const std::vector<LLVector3>& points, const LLVector3& light_dir, const LLCamera& camera,
                          U32 map_width, U32 map_height, glm::mat4& view, glm::mat4& proj)
    {
        static LLCachedControl<bool> stabilize(gSavedSettings, SETTING_STABILIZE, true);
        if (!stabilize || gCubeSnapshot || j < 0 || j > 3 || points.empty() || map_width < 16 || map_height < 16)
        {
            return false;
        }
        if (j == 0)
        {
            pollZHoldCue();     // between frames' cascades only: one frame's are all held or none
        }
        LLVector3 light = light_dir;
        if (light.normVec() <= 0.f)
        {
            return false;
        }

        // The view's up: the camera's forward across the light, turned about the
        // light only in STABLE_TURN steps from a fixed reference (world x).
        LLVector3 ref(1.f, 0.f, 0.f);
        if (std::fabs(ref * light) > 0.9f)
        {
            ref.set(0.f, 1.f, 0.f);
        }
        ref -= light * (ref * light);
        ref.normVec();
        const LLVector3 ref2 = light % ref;
        LLVector3 forward = camera.getAtAxis();
        if (std::fabs(forward * light) > STABLE_LOOKING_AT_LIGHT)
        {
            forward = camera.getUpAxis();
        }
        static LLCachedControl<bool> world_lock(gSavedSettings, SETTING_WORLD_LOCK, false);
        static F32 kept_turn = -1000.f;
        if (world_lock)
        {
            kept_turn = 0.f;    // the fixed reference: the grid never turns with the camera
        }
        else
        {
            const F32 turn = std::atan2(forward * ref2, forward * ref) / STABLE_TURN;
            F32 delta = turn - kept_turn;
            const F32 turns = 2.f * F_PI / STABLE_TURN;
            delta -= turns * std::round(delta / turns);  // the shorter way round
            if (kept_turn < -999.f || std::fabs(delta) > STABLE_TURN_HOLD)
            {
                kept_turn = std::round(turn);
            }
        }
        const F32 angle = kept_turn * STABLE_TURN;
        const LLVector3 up = ref * std::cos(angle) + ref2 * std::sin(angle);

        // The light's view from the agent origin, looking along the light (as
        // LLPipeline's look(): x left, y up, -z the light's way).
        LLVector3 left = light % (-up);
        left.normVec();
        LLVector3 view_up = left % light;
        view_up.normVec();
        glm::mat4 v(1.f);
        for (U32 i = 0; i < 3; ++i)
        {
            v[i][0] = left.mV[i];
            v[i][1] = view_up.mV[i];
            v[i][2] = -light.mV[i];
        }

        // The receivers' box in that view.
        glm::vec3 lo(FLT_MAX), hi(-FLT_MAX);
        for (const LLVector3& p : points)
        {
            const glm::vec3 q = glm::vec3(v * glm::vec4(p.mV[0], p.mV[1], p.mV[2], 1.f));
            lo = glm::min(lo, q);
            hi = glm::max(hi, q);
        }

        // Sizes in steps (two texels to spare for the snapped centre), kept while
        // they still fit and are not too large.
        static F32 kept_size[4][2] = {};
        const F32 sizes[2] = { (F32)map_width, (F32)map_height };
        const F32 size_steps = world_lock ? LOCK_SIZE_STEPS : STABLE_SIZE_STEPS;
        const F32 size_slack = world_lock ? LOCK_SIZE_SLACK : STABLE_SIZE_SLACK;
        static bool lock_last = false;
        if (lock_last != (bool)world_lock)
        {
            lock_last = world_lock;
            std::fill(&kept_size[0][0], &kept_size[0][0] + 8, 0.f);     // the other ladder: start again
        }
        const F32 box_diagonal = std::sqrt((hi.x - lo.x) * (hi.x - lo.x) + (hi.y - lo.y) * (hi.y - lo.y));
        F32 size[2];
        for (U32 a = 0; a < 2; ++a)
        {
            const F32 extent = world_lock ? box_diagonal : hi[a] - lo[a];
            const F32 needed = llmax(extent, 0.01f) / (1.f - 2.f / sizes[a]);
            const F32 stepped = std::exp2(std::ceil(std::log2(needed) * size_steps) / size_steps);
            F32& kept = kept_size[j][a];
            if (kept < needed || kept > stepped * std::exp2(size_slack / size_steps))
            {
                kept = stepped;
            }
            size[a] = kept;
        }

        // The centre on whole texels (the sizes hold an even number of texels:
        // the maps' sides are multiples of 16).
        F32 centre[2];
        for (U32 a = 0; a < 2; ++a)
        {
            const F32 texel = size[a] / sizes[a];
            centre[a] = std::round(0.5f * (lo[a] + hi[a]) / texel) * texel;
        }

        // A depth range of at least STABLE_MIN_DEPTH (a flat point cloud seen
        // straight along the light would make a degenerate projection).
        F32 near_z = hi.z, far_z = lo.z;
        if (depthRoundActive())
        {
            roundDepthRange(j, lo.z, hi.z, near_z, far_z);
        }
        else
        {
            sKeptDepth[j].valid = false;
            if (near_z - far_z < STABLE_MIN_DEPTH)
            {
                const F32 mid = 0.5f * (near_z + far_z);
                near_z = mid + 0.5f * STABLE_MIN_DEPTH;
                far_z = mid - 0.5f * STABLE_MIN_DEPTH;
            }
        }
        const glm::mat4 stable = glm::ortho(centre[0] - 0.5f * size[0], centre[0] + 0.5f * size[0], centre[1] - 0.5f * size[1],
                                            centre[1] + 0.5f * size[1], -near_z, -far_z);

        // Checked before it replaces the official one: finite, and every receiver
        // inside it. Otherwise the official cascade stays (and the diagnostics say so).
        bool valid = true;
        for (U32 c = 0; c < 4 && valid; ++c)
        {
            for (U32 r = 0; r < 4; ++r)
            {
                valid = valid && std::isfinite(stable[c][r]) && std::isfinite(v[c][r]);
            }
        }
        const glm::mat4 to_clip = stable * v;
        for (size_t i = 0; i < points.size() && valid; ++i)
        {
            const glm::vec4 c = to_clip * glm::vec4(points[i].mV[0], points[i].mV[1], points[i].mV[2], 1.f);
            valid = std::fabs(c.x) <= c.w * STABLE_INSIDE && std::fabs(c.y) <= c.w * STABLE_INSIDE &&
                    std::fabs(c.z) <= c.w * STABLE_INSIDE;
        }
        static LLCachedControl<bool> diagnostics(gSavedSettings, SETTING_DIAGNOSTICS, false);
        if (!valid)
        {
            ++sStableRejected[j];
            if (diagnostics)
            {
                LL_INFOS("Plus") << "Stable shadow cascade " << j << " rejected (not finite, or a receiver outside it): "
                                 << llformat("box %.2f x %.2f x %.2f m, size %.2f x %.2f m", hi.x - lo.x, hi.y - lo.y, hi.z - lo.z,
                                             size[0], size[1])
                                 << "; the official cascade is used this frame" << LL_ENDL;
            }
            return false;
        }

        // The grid's steps (turn, sizes) as last used, for the diagnostics.
        StableGrid& grid = sStableGrid[j];
        const bool stepped = grid.turn != kept_turn || grid.size[0] != size[0] || grid.size[1] != size[1];
        if (stepped && diagnostics && grid.size[0] > 0.f)
        {
            LL_INFOS("Plus") << "Stable shadow cascade " << j << llformat(": turn step %.0f -> %.0f, size %.2f x %.2f -> %.2f x %.2f m",
                                                                       grid.turn, kept_turn, grid.size[0], grid.size[1], size[0], size[1])
                             << LL_ENDL;
        }
        grid.turn = kept_turn;
        grid.size[0] = size[0];
        grid.size[1] = size[1];

        view = v;
        proj = stable;
        return true;
    }

    bool depthStepsActive()
    {
        static LLCachedControl<bool> stabilize(gSavedSettings, SETTING_STABILIZE, true);
        return stabilize && depthRoundActive();
    }

    bool zHoldOn()
    {
        return zHold();
    }

    bool sameStableGrid(const glm::mat4& view_a, const glm::mat4& proj_a, const glm::mat4& view_b, const glm::mat4& proj_b)
    {
        static LLCachedControl<bool> stabilize(gSavedSettings, SETTING_STABILIZE, true);
        if (!stabilize || gCubeSnapshot)
        {
            return false;
        }
        // Both orthographic, the same turn (the views of stable cascades have no
        // translation) and the same size in x and y.
        return proj_a[3][3] == 1.f && proj_b[3][3] == 1.f && view_a == view_b && proj_a[0][0] == proj_b[0][0] &&
               proj_a[1][1] == proj_b[1][1];
    }

    LLVector3 splitExponent(const LLVector3& official)
    {
        // Official: x where the light crosses the view, y where it runs along
        // it, z the curve between them. One exponent here, whatever the light.
        const Level* level = activeLevel();
        return level ? LLVector3(level->split_exponent, level->split_exponent, official.mV[2]) : official;
    }

    F32 sunResolutionScale(F32 official_scale, U32 res_x, U32 res_y)
    {
        if (gCubeSnapshot)
        {
            return official_scale;  // a probe's or mirror's own maps
        }
        const bool boost = plus::presets::snapshotBoostActive();
        const F32 wanted = wantedResolution(levelSetting(), precisionSetting(), boost);
        const F32 factor = budgetedResolution(wanted, official_scale, res_x, res_y);

        if (factor != 1.f || sAllocation.factor != 1.f)
        {
            // Once a second while the window frame is dragged (fix-round95 D).
            static plus::ResizeLog resize_log;
            const U32 detail = ((U32)(official_scale * 256.f) << 16) ^ ((U32)(factor * 256.f) << 1) ^ (boost ? 1u : 0u);
            const LLError::ELevel log_level = resize_log.level(res_x, res_y, detail);    // once: LL_VLOGS reads its level twice
            LL_VLOGS(log_level, "Plus") << "Sun shadow maps for " << res_x << "x" << res_y << ": scale " << official_scale << " x " << factor
                             << (boost ? " (snapshot boost)" : "")
                             << (factor < wanted ? llformat(" (x%.2f wanted; held to 1/%.0f of %u MB VRAM or %d texels a side)",
                                                            wanted, 1.0 / VRAM_SHARE, gGLManager.mVRAM, MAX_MAP_SIZE) : "")
                             << LL_ENDL;
        }
        plus::presets::noteScreenTargets();     // research/109
        if (LLRHI::sNoteEvent)
        {
            // research/109: the GPU fault report's frame events (the screen's and shadow maps' targets made again).
            LLRHI::sNoteEvent(llformat("screen and sun shadow targets for %ux%u: shadow scale %.2f x %.2f%s", res_x, res_y,
                                       official_scale, factor, boost ? " (snapshot boost)" : "").c_str());
        }
        sAllocation.official_scale = official_scale;
        sAllocation.res_x = res_x;
        sAllocation.res_y = res_y;
        sAllocation.factor = factor;
        return official_scale * factor;
    }

    LLVector3 shadowCasterDir(const LLVector3& live, bool sun_up)
    {
        static LLCachedControl<F32> step_degrees(gSavedSettings, "PlusShadowLightStep", 0.3f);
        static LLVector3 held;
        static bool held_sun_up = true;
        static bool have_held = false;
        if (gCubeSnapshot || step_degrees <= 0.f)
        {
            return live;
        }
        LLVector3 a = live, b = held;
        a.normVec();
        b.normVec();
        // The sine of the angle between them (the dot product loses the small angles in a float).
        const F32 sine = (a % b).length();
        if (!have_held || sun_up != held_sun_up || a * b < 0.f || sine > std::sin(step_degrees * DEG_TO_RAD))
        {
            held = live;
            held_sun_up = sun_up;
            have_held = true;
        }
        return held;
    }

    F32 biasScale()
    {
        if (gCubeSnapshot)
        {
            return 1.f;     // probes and mirrors: their own maps, the official bias
        }
        if (activePrecision())
        {
            return 0.f;     // samplingProjection() holds the whole bias
        }
        return 1.f / sAllocation.factor;
    }

    glm::mat4 samplingProjection(S32 j, const glm::mat4& proj, const glm::mat4& view, const LLCamera& camera,
                                 F32 near_depth, F32 far_depth, const LLVector3& light_dir, const LLRenderTarget& map)
    {
        if (gCubeSnapshot || j < 0 || j > 3)
        {
            return proj;    // probes and mirrors: official
        }
        sCascadeSplit[j] = far_depth;
        sSunSine = llclamp(std::fabs(light_dir.mV[2]), MIN_SUN_SINE, 1.f);

        // The official bias, as bindDeferredShader() makes it: 2 x shadow_bias in
        // [0, 1] (4 x in NDC), the camera height's part included.
        const F32 camera_height = std::fabs(LLViewerCamera::getInstance()->getOrigin().mV[2]);
        const F32 bias_error = LLPipeline::RenderShadowBiasError * camera_height / 3000.f;
        const F32 bias_constant = LLPipeline::RenderShadowBias;

        // The cascade at the far end of what reads it (coarsest texels there in
        // a perspective cascade), and halfway.
        const LLVector3 eye = camera.getOrigin();
        const LLVector3 at = camera.getAtAxis();
        const F32 far_read = j == 3 ? far_depth : far_depth * BAND_FAR;
        const F32 mid_read = 0.5f * (llmax(near_depth * BAND_NEAR, PROBE_STEP) + far_read);
        const LLVector3 far_point = eye + at * far_read;
        const LLVector3 mid_point = eye + at * mid_read;
        const glm::vec3 far_view(view * glm::vec4(far_point.mV[0], far_point.mV[1], far_point.mV[2], 1.f));
        const glm::vec3 mid_view(view * glm::vec4(mid_point.mV[0], mid_point.mV[1], mid_point.mV[2], 1.f));
        CascadeScale far_scale, mid_scale;
        const bool measured = map.getWidth() && map.getHeight() &&
                              measure(proj, far_view, map.getWidth(), map.getHeight(), far_scale);
        F32 texel_x = far_scale.texel_x;
        F32 texel_y = far_scale.texel_y;
        if (measured && measure(proj, mid_view, map.getWidth(), map.getHeight(), mid_scale))
        {
            texel_x = llmax(texel_x, mid_scale.texel_x);
            texel_y = llmax(texel_y, mid_scale.texel_y);
        }
        const F32 metres_per_ndc = measured ? 1.f / std::fabs(far_scale.depth_per_m) : 0.f;
        const F32 official = 4.f * std::fabs(bias_constant) * metres_per_ndc;
        const F32 error = 4.f * std::fabs(bias_error) * metres_per_ndc;
        sCascadeTexel[j] = llmax(texel_x, texel_y);
        sCascadeBias[j] = official + error;

        const Precision* precision = activePrecision();
        if (!precision)
        {
            return proj;
        }
        if (!measured)
        {
            // The shader's bias is 0 now (biasScale()): the official one goes into
            // the matrix instead, as the same shift of NDC depth (z += 4 bias w).
            glm::mat4 official_shift(1.f);
            official_shift[3][2] = 4.f * (bias_constant + bias_error);
            return official_shift * proj;
        }

        // The ground's slope in the map, along its x and y: the ground's normal
        // in the light's view (z: along the light) is (n.x, n.y, n.z); a metre
        // across the map along x changes the depth by |n.x / n.z| metres.
        const glm::vec3 ground_normal = glm::mat3(view) * glm::vec3(0.f, 0.f, 1.f);
        const F32 nz = llmax(std::fabs(ground_normal.z), MIN_SUN_SINE);
        const F32 slope_x = llmin(std::fabs(ground_normal.x) / nz, MAX_SLOPE);
        const F32 slope_y = llmin(std::fabs(ground_normal.y) / nz, MAX_SLOPE);
        const bool caster_bias = casterBiasActive();
        const F32 wanted = caster_bias ? precision->receiver_texels * llmax(texel_x, texel_y)
                                       : precision->bias_texels * llmax(texel_x, texel_y) +
                                             SLOPE_TEXELS_X * texel_x * slope_x + SLOPE_TEXELS_Y * texel_y * slope_y;

        // The casters' clamp in the map's [0, 1] depth (half of NDC).
        const F32 clamp_metres = CASTER_SLOPE * CASTER_CLAMP_SLOPES *
                                 llmax(llmax(texel_x, texel_y), llmax(texel_x * slope_x, texel_y * slope_y));
        sCasterClamp[j] = caster_bias ? clamp_metres / (2.f * metres_per_ndc) : 0.f;

        // Never more than the official bias (metres for 4 |bias| NDC at the far end),
        // never less than MIN_RECEIVER_BIAS.
        const F32 bias = llmax(llmin(wanted, official), MIN_RECEIVER_BIAS) + error;
        sCascadeBias[j] = bias;

        // Move the receivers by `bias` metres towards the light: the way that
        // lowers their depth.
        const F32 towards_light = far_scale.depth_per_m > 0.f ? 1.f : -1.f;
        return proj * glm::translate(glm::mat4(1.f), glm::vec3(0.f, 0.f, towards_light * bias));
    }

    CasterBiasScope::CasterBiasScope(S32 cascade)
        : mSaved(LLRHI::sShadowDepthBias)
    {
        if (gCubeSnapshot || cascade < 0 || cascade > 3 || !casterBiasActive())
        {
            return;
        }
        LLRHI::sShadowDepthBias.active = true;
        LLRHI::sShadowDepthBias.constant = 0.f;
        LLRHI::sShadowDepthBias.slope = CASTER_SLOPE;
        LLRHI::sShadowDepthBias.clamp = sCasterClamp[cascade];
    }

    CasterBiasScope::~CasterBiasScope()
    {
        LLRHI::sShadowDepthBias = mSaved;
    }

    F32 casterClamp(S32 cascade)
    {
        return cascade >= 0 && cascade < 4 ? sCasterClamp[cascade] : 0.f;
    }

    bool casterBiasChanged()
    {
        if (gCubeSnapshot)
        {
            return false;
        }
        const S32 state = casterBiasActive() ? 1 : 0;
        const bool changed = sCasterBiasState >= 0 && state != sCasterBiasState;
        sCasterBiasState = state;
        return changed;
    }

    LLVector3 blurGaussian(const LLVector3& official)
    {
        const Softness* softness = activeSoftness();
        const LLVector3 gaussian = softness ? LLVector3(softness->shadow_sigma, official.mV[1], official.mV[2]) : official;
        sBlurSigma = gaussian.mV[0];
        return gaussian;
    }

    F32 blurSize(F32 official)
    {
        const Softness* softness = activeSoftness();
        sBlurSize = softness ? official * softness->blur_size : official;
        return sBlurSize;
    }

    F32 receiverNormalOffsetMax()
    {
        return NORMAL_OFFSET_MAX;
    }

    F32 normalOffsetTexels()
    {
        static LLCachedControl<bool> normal_offset(gSavedSettings, SETTING_NORMAL_OFFSET, true);
        return normal_offset ? NORMAL_OFFSET_TEXELS : 0.f;
    }

    void noteAgentCasters(const S32* index, const U32* tested, const U32* kept)
    {
        checkAgentCasters(index, tested, kept);
    }

    F32 blurDistFactor(F32 official, F32 blur_size)
    {
        const Softness* softness = activeSoftness();
        const U32 height = gPipeline.mRT->deferredScreen.getHeight();
        sBlurFactor = official;
        if (!softness || height == 0 || blur_size <= 0.f)
        {
            return official;
        }
        // The farthest tap is BLUR_TAPS_REACH x BLUR_STEP x blur_size pixels away;
        // a pixel at eye depth d is d x pixel_angle metres. The shader divides
        // the reach by d x factor beyond 1 / factor metres, which holds it at
        // reach_px x pixel_angle / factor metres: the factor that makes that `reach`.
        const F32 pixel_angle = 2.f * std::tan(LLViewerCamera::getInstance()->getView() * 0.5f) / F32(height);
        const F32 reach_px = BLUR_TAPS_REACH * BLUR_STEP * blur_size;
        sBlurFactor = llmax(official, reach_px * pixel_angle / softness->reach);
        return sBlurFactor;
    }
}

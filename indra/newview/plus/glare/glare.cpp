/**
 * @file glare.cpp
 * @brief Glare (HDR bloom), the sun's lens flare and the cross filter,
 *        added to the scene before tone mapping. Registers with the plus
 *        foundation (research/108-glare.md, 119-cross-filter-idea.md).
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

// Both off by default (PlusGlare, PlusLensFlare: 0 off, 1 weak, 2 medium,
// 3 strong). Vulkan only: the passes are recorded by the backend
// (llvulkan/llvkglare.cpp); with GL nothing is drawn.
//
// Each frame, after the exposure is measured and before tone mapping:
//  1. Glare: the scene's HDR target (mRT->screen) exposed, only what lies
//     above a soft threshold (what the picture blows out), into half its size;
//     then halved again level by level (6 levels at most, 1/2 .. 1/64); then
//     from the smallest up, each level tent-filtered and added to the one
//     above. The first level then holds the light spread at every width.
//  2. Lens flare: the sun's place on screen from its direction; a 1x1 target
//     keeps how much of it is seen (the exposed brightness around it, averaged
//     over a few frames), so the flare fades as the sun goes behind a building.
//  3. Both are added to mRT->screen in one pass, in the scene's units (divided
//     by the exposure): exposure and tone mapping then treat them like the
//     rest of the light. The exposure was measured without them, so turning
//     them on does not darken the picture.
//     With PlusGlareInTonemap (off; research/125 #7) the tone mapping pass
//     adds them instead (postDeferredTonemap.glsl's <plus> part, the same
//     sums): one full-screen read and write of mRT->screen fewer.
//
// The glare is part of the quality levels (featuretable.txt, research/183: off
// up to MidHigh, weak at High, medium above) and the snapshot boost (at least medium). The lens flare
// is neither: it is a matter of taste, drawn only as the user sets it.
//
// The cross filter (PlusCrossFilter: 0 off .. 3 strong; PlusCrossFilterRays
// 4, 6 or 8; PlusCrossFilterAngle in degrees; research/119) is drawn in the
// same hook, before the composite: the lights alone (far above white and
// brighter than a ring around them, so not a lit wall nor the sky) into a
// quarter-size target, then each ray of the star as a streak (a few passes of
// taps along its direction, Kawase), all added up; the composite adds them.
// Its length is a share of the screen's height, so a large snapshot looks as
// the screen does, and has a ceiling whatever the light. Points of the sky
// (the stars) get only a faint short cross (PlusCrossFilterSky), the sun its
// whole rays (fix-round40), and so does the moon at night (fix-round43).
// Like the lens flare, it is in no preset nor the boost.
// A large snapshot drawn in tiles (only when targets of its size cannot be
// made) gets the glare and the streaks of each tile, a share of the tile's
// height and stopping at its edges, and no lens flare (fix-round6 4).

#include "llviewerprecompiledheaders.h"

#include "plus.h"

#include "glare.h"

#include "presets/presets.h"

#include "llenvironment.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llrhi.h"
#include "llshadermgr.h"
#include "llrhiglare.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "pipeline.h"
#include "v3color.h"

#include <glm/glm.hpp>

extern bool gSnapshotNoPost;    // llviewerdisplay.cpp: a snapshot taken without post-processing

namespace
{
    constexpr char SETTING_GLARE[] = "PlusGlare";
    constexpr char SETTING_FLARE[] = "PlusLensFlare";
    constexpr char SETTING_CROSS[] = "PlusCrossFilter";
    constexpr char SETTING_CROSS_RAYS[] = "PlusCrossFilterRays";
    constexpr char SETTING_CROSS_ANGLE[] = "PlusCrossFilterAngle";
    constexpr char SETTING_CROSS_SKY[] = "PlusCrossFilterSky";

    // PlusGlare and PlusLensFlare: 0 off, 1 weak, 2 medium, 3 strong.
    constexpr S32 LEVEL_OFF = 0;
    constexpr S32 LEVEL_COUNT = 4;
    // The glare: times the first level (all levels added up, divided by their count), exposed units.
    constexpr F32 GLARE_INTENSITY[LEVEL_COUNT] = { 0.f, 0.15f, 0.3f, 0.6f };
    // The lens flare: times its ghosts, ring and halo.
    constexpr F32 FLARE_INTENSITY[LEVEL_COUNT] = { 0.f, 0.5f, 1.f, 1.8f };
    // A snapshot with the boost on (plus/presets): the glare at least this.
    constexpr S32 SNAPSHOT_BOOST_GLARE = 2;

    // Exposed brightness (the largest of R, G, B) where the glare starts: about
    // where the tone mapping turns white. Below, by the knee, it fades in.
    constexpr F32 THRESHOLD = 0.9f;
    constexpr F32 KNEE = 0.5f;
    // Levels 1/2 .. 1/64 of the screen, none smaller than this many pixels across.
    constexpr U32 MAX_LEVELS = 6;
    constexpr U32 MIN_LEVEL_SIZE = 4;

    // The sun's visibility: the disc sampled around its place (share of the
    // screen's height), the exposed brightness from hidden to seen, and this
    // frame's share in the running average.
    // The disc is wider than the sun's own (a few pixels at 1080p), and the
    // average slow, so leaves, edges and a moving camera do not make the
    // flare flicker.
    constexpr F32 SUN_RADIUS = 0.02f;
    constexpr F32 SUN_DARK = 0.8f;
    constexpr F32 SUN_BRIGHT = 2.5f;
    constexpr F32 SUN_BLEND = 0.2f;
    // The flare fades out as the sun leaves the screen, by this much of its
    // width or height past the edge.
    constexpr F32 OFFSCREEN_FADE = 0.3f;
    // The flare's colour: the sun's, this much towards white (a deep red sunset
    // still leaves the ghosts their own tints).
    constexpr F32 FLARE_WHITE = 0.3f;

    // The cross filter, by level: the rays' brightness at their root (times
    // the light they start from, exposed units, for 4 rays; more rays share
    // it, by the square root of 4 / their count), and their length (the share
    // of the screen's height where a ray has faded to e^-CROSS_FADE, 5%).
    // A ray is seen as far as it stays above a faint level, so its seen length
    // grows with the log of its root's brightness; the strength's cap (the
    // light at CROSS_FULL) stops that, and the passes end it at CROSS_REACH lengths whatever the
    // light: at most 4.5% (weak), 7.5% (medium) and 12% (strong) of the
    // screen's height each way, a lamp's ray mostly shorter (fix-round40: they
    // were 12%, 21% and 33%, and every star had them).
    constexpr F32 CROSS_INTENSITY[LEVEL_COUNT] = { 0.f, 0.1f, 0.18f, 0.3f };
    constexpr F32 CROSS_LENGTH[LEVEL_COUNT] = { 0.f, 0.03f, 0.05f, 0.08f };
    constexpr F32 CROSS_FADE = 3.f;
    // The passes reach this many lengths (the ray has then faded to about 1%).
    constexpr F32 CROSS_REACH = 1.5f;
    // The lights: the exposed brightness (max of R, G, B) of a 4x4 block from
    // CROSS_THRESHOLD - CROSS_KNEE (none) up. Like a camera's, only a light
    // that reaches the sensor strongly has rays: the rays' strength rises with
    // the square of the light's place between that start and CROSS_FULL (by
    // level, the exposed brightness of full rays; a weaker level wants a
    // stronger light), so a lamp has long rays, a window or a dim lamp a
    // faint short one or none (research/269). A light is brighter than every
    // point of a ring of CROSS_RING (share of the screen's height) around it:
    // the ring's brightest at most CROSS_POINT_FULL of it gives the whole
    // rays, at least CROSS_POINT_NONE none (a wall, the sky, their edges, a
    // lit window).
    constexpr F32 CROSS_THRESHOLD = 1.5f;
    constexpr F32 CROSS_KNEE = 0.5f;
    constexpr F32 CROSS_FULL[LEVEL_COUNT] = { 0.f, 8.f, 6.f, 4.f };
    // Points of the sky (the scene's depth at the far end): the stars, dozens
    // of them, each as bright as a lamp once the night's exposure is raised.
    // Their light times PlusCrossFilterSky (by default a short faint cross, 0
    // none, 1 as a lamp's), except within CROSS_SUN_RADIUS (share of the
    // screen's height) of the sun, whose rays stay whole (of the moon at
    // night, fix-round43 4).
    constexpr F32 CROSS_DEFAULT_SKY = 0.05f;
    constexpr F32 CROSS_SUN_RADIUS = 0.05f;
    constexpr F32 CROSS_RING = 0.02f;
    constexpr F32 CROSS_POINT_FULL = 0.15f;
    constexpr F32 CROSS_POINT_NONE = 0.5f;
    // The rays are drawn at this fraction of the screen's size, each way (the
    // whole target then fits the GPU's cache at 1440p), none smaller than
    // CROSS_MIN_SIZE texels across.
    constexpr U32 CROSS_DOWNSCALE = 4;
    constexpr U32 CROSS_MIN_SIZE = 16;
    // A ray's passes: each reads at most CROSS_MAX_TAPS (llvkglare.cpp
    // MAX_STREAK_TAPS); as few passes as reach CROSS_REACH lengths.
    constexpr U32 CROSS_MAX_TAPS = 16;
    constexpr U32 CROSS_MAX_PASSES = 4;
    constexpr S32 CROSS_DEFAULT_RAYS = 4;

    S32 levelIndex(S32 level)
    {
        return llclamp(level, LEVEL_OFF, LEVEL_COUNT - 1);
    }

    // PlusCrossFilterRays: 4, 6 or 8 (others to the nearest).
    U32 rayCount(S32 rays)
    {
        return rays >= 7 ? 8 : (rays >= 5 ? 6 : 4);
    }

    struct Settings
    {
        F32 glare = 0.f;    // GLARE_INTENSITY of the level in use; 0: off
        F32 flare = 0.f;    // FLARE_INTENSITY; 0: off
        F32 cross = 0.f;    // CROSS_INTENSITY, shared by the rays; 0: off
        F32 cross_length = 0.f;     // CROSS_LENGTH
        F32 cross_full = 0.f;       // CROSS_FULL
        U32 cross_rays = 4;
        F32 cross_angle = 0.f;      // the first ray's direction, radians (0: to the right)
        F32 cross_sky = CROSS_DEFAULT_SKY;  // PlusCrossFilterSky, 0 .. 1
    };

    Settings readSettings()
    {
        static LLCachedControl<S32> glare(gSavedSettings, SETTING_GLARE, LEVEL_OFF);
        static LLCachedControl<S32> flare(gSavedSettings, SETTING_FLARE, LEVEL_OFF);
        S32 glare_level = levelIndex(glare);
        if (plus::presets::snapshotBoostActive())
        {
            glare_level = llmax(glare_level, SNAPSHOT_BOOST_GLARE);
        }
        Settings out;
        out.glare = GLARE_INTENSITY[glare_level];
        out.flare = FLARE_INTENSITY[levelIndex(flare)];     // never boosted: a matter of taste

        static LLCachedControl<S32> cross(gSavedSettings, SETTING_CROSS, LEVEL_OFF);
        static LLCachedControl<S32> cross_rays(gSavedSettings, SETTING_CROSS_RAYS, CROSS_DEFAULT_RAYS);
        static LLCachedControl<F32> cross_angle(gSavedSettings, SETTING_CROSS_ANGLE, 0.f);
        const S32 cross_level = levelIndex(cross);      // never boosted either
        out.cross_rays = rayCount(cross_rays);
        out.cross = CROSS_INTENSITY[cross_level] * sqrtf(4.f / (F32)out.cross_rays);
        out.cross_length = CROSS_LENGTH[cross_level];
        out.cross_full = CROSS_FULL[cross_level];
        const F32 angle = cross_angle;
        out.cross_angle = (llfinite(angle) ? angle : 0.f) * DEG_TO_RAD;
        static LLCachedControl<F32> cross_sky(gSavedSettings, SETTING_CROSS_SKY, CROSS_DEFAULT_SKY);
        const F32 sky = cross_sky;
        out.cross_sky = llfinite(sky) ? llclamp(sky, 0.f, 1.f) : CROSS_DEFAULT_SKY;
        return out;
    }

    // ------------------------------------------------------------------
    // The targets: the glare's levels (RGBA16F), the sun's visibility (1x1
    // R16F) and the cross filter's (RGBA16F, a quarter of the screen each
    // way). Made on first use and kept while in use; never destroyed by a
    // static destructor, which would run without a GL context.

    // The cross filter's targets: the extracted light, two for a ray's
    // passes in between, and the rays added up.
    enum CrossTarget : U32
    {
        CROSS_EXTRACT,
        CROSS_PING,
        CROSS_PONG,
        CROSS_RAYS,
        CROSS_TARGETS
    };

    struct Targets
    {
        LLRenderTarget levels[MAX_LEVELS];
        LLRenderTarget visibility;
        bool visibility_fresh = false;  // just made: its value is not yet a running average
        LLRenderTarget cross[CROSS_TARGETS];
        plus::RetryWait retry;          // after any of them could not be made (VRAM full)
    };

    Targets& targets()
    {
        static Targets* targets = new Targets();
        return *targets;
    }

    void releaseTarget(LLRenderTarget& target)
    {
        if (target.isComplete())
        {
            target.release();
        }
    }

    void releaseLevels(U32 from = 0)
    {
        for (U32 i = from; i < MAX_LEVELS; ++i)
        {
            releaseTarget(targets().levels[i]);
        }
    }

    void releaseVisibility()
    {
        releaseTarget(targets().visibility);
    }

    void releaseCross()
    {
        for (LLRenderTarget& target : targets().cross)
        {
            releaseTarget(target);
        }
    }

    // The target at width x height, sampled bilinearly by the passes; false
    // when it could not be made. made: true when it was (re)made now.
    bool sizeTarget(LLRenderTarget& target, U32 width, U32 height, U32 format, bool& made)
    {
        made = false;
        if (target.isComplete() && target.getWidth() == width && target.getHeight() == height)
        {
            return true;
        }
        plus::RetryWait& retry = targets().retry;
        if (!retry.due())
        {
            return false;       // one could not be made a moment ago: not again every frame
        }
        if (!target.allocate(width, height, format))
        {
            target.release();   // a half-made target is not isComplete(): releaseTarget would skip it
            retry.failed();
            return false;
        }
        retry.succeeded();
        made = true;
        // Then unit 0 is left empty, not holding the new texture in the middle of the post-processing.
        target.bindTexture(0, 0, LLTexUnit::TFO_BILINEAR);
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        return true;
    }

    // The levels for a screen of width x height; their count (0 when none could be made).
    U32 sizeLevels(U32 width, U32 height)
    {
        Targets& t = targets();
        U32 count = 0;
        for (; count < MAX_LEVELS; ++count)
        {
            width = (width + 1) / 2;
            height = (height + 1) / 2;
            bool made = false;
            if (llmin(width, height) < MIN_LEVEL_SIZE || !sizeTarget(t.levels[count], width, height, GL_RGBA16F, made))
            {
                break;
            }
        }
        releaseLevels(count);
        return count;
    }

    // ------------------------------------------------------------------

    bool backendReady()
    {
        return LLRHI::sGlare && LLRHI::sGlareDownsample && LLRHI::sGlareUpsample && LLRHI::sGlareSunVisibility &&
               LLRHI::sGlareStreakExtract && LLRHI::sGlareStreak && LLRHI::sGlareComposite;
    }

    // The scene's HDR target and the exposure measured for it, not bound.
    bool sceneReady()
    {
        const LLRenderTarget& screen = gPipeline.mRT->screen;
        return screen.isComplete() && !screen.isBoundInStack() && gPipeline.mExposureMap.isComplete();
    }

    LLRHI::GlareExposure exposureOf()
    {
        LLRHI::GlareExposure out;
        out.texture = gPipeline.mExposureMap.getTexture();
        out.scale = plus::exposureScaleForPasses();     // 0 under plus::isNoPost: the exposure taken as 1 (fix-round48)
        return out;
    }

    // Draws the levels; the count drawn (the first level then holds them all
    // added up), 0 when the first could not be drawn.
    U32 renderBloom(LLRenderTarget& screen, const LLRHI::GlareExposure& exposure)
    {
        const U32 count = sizeLevels(screen.getWidth(), screen.getHeight());
        if (!count)
        {
            return 0;
        }
        Targets& t = targets();
        U32 drawn = 0;
        for (U32 i = 0; i < count; ++i)
        {
            const LLRenderTarget& source = i ? t.levels[i - 1] : screen;
            LLRenderTarget& level = t.levels[i];
            LLRHI::GlareDownsample pass;
            pass.source_texture = source.getTexture();
            pass.source_width = source.getWidth();
            pass.source_height = source.getHeight();
            pass.width = level.getWidth();
            pass.height = level.getHeight();
            pass.prefilter = i == 0;
            pass.exposure = exposure;
            pass.threshold = THRESHOLD;
            pass.knee = KNEE;
            level.bindTarget();
            const bool ok = LLRHI::sGlareDownsample(pass);
            level.flush();
            if (!ok)
            {
                break;
            }
            ++drawn;
        }
        for (U32 i = drawn; i-- > 1;)
        {
            const LLRenderTarget& source = t.levels[i];
            LLRenderTarget& level = t.levels[i - 1];
            LLRHI::GlareUpsample pass;
            pass.source_texture = source.getTexture();
            pass.source_width = source.getWidth();
            pass.source_height = source.getHeight();
            pass.width = level.getWidth();
            pass.height = level.getHeight();
            level.bindTarget();
            LLRHI::sGlareUpsample(pass);    // when it fails, the level keeps its own light only
            level.flush();
        }
        return drawn;
    }

    // A direction's place on the screen (towards: eye space, the sun's or the
    // moon's; texture coordinates, may be a little outside 0..1) and how much
    // it is faded for lying outside; false behind the camera or far off the screen.
    bool skyPlace(const glm::mat4& projection, const LLVector4& towards, F32 place[2], F32& fade)
    {
        const glm::vec4 clip = projection * glm::vec4(towards.mV[VX], towards.mV[VY], towards.mV[VZ], 0.f);
        if (clip.w <= 1e-4f)
        {
            return false;
        }
        place[0] = clip.x / clip.w * 0.5f + 0.5f;
        place[1] = clip.y / clip.w * 0.5f + 0.5f;
        const F32 outside = llmax(llmax(-place[0], place[0] - 1.f), llmax(-place[1], place[1] - 1.f));
        if (outside >= OFFSCREEN_FADE)
        {
            return false;
        }
        const F32 x = llclamp(outside / OFFSCREEN_FADE, 0.f, 1.f);
        fade = 1.f - x * x * (3.f - 2.f * x);
        return fade > 0.f;
    }

    // The sun's place on the screen and the flare's fade (skyPlace); false
    // while the sun is down, behind the camera, or far off the screen.
    bool sunPlace(const glm::mat4& projection, F32 place[2], F32& fade)
    {
        if (!LLEnvironment::instance().getIsSunUp())
        {
            return false;   // no flare from the moon
        }
        return skyPlace(projection, gPipeline.mTransformedSunDir, place, fade);
    }

    // The place of the sky's one large light for the cross filter: the sun
    // while it is up, else the moon (a single bright body, not a star: its rays
    // are not weakened as the stars' are, fix-round43 4). As sunPlace's; false
    // too while both are under the horizon (fix-round44 6).
    bool brightBodyPlace(const glm::mat4& projection, F32 place[2], F32& fade)
    {
        const LLEnvironment& environment = LLEnvironment::instance();
        const bool sun = environment.getIsSunUp();
        if (!sun && !environment.getIsMoonUp())
        {
            return false;
        }
        return skyPlace(projection, sun ? gPipeline.mTransformedSunDir : gPipeline.mTransformedMoonDir, place, fade);
    }

    // The flare's colour: the sun's, about 1 in brightness; false when it gives no light.
    bool flareColor(F32 out[3])
    {
        const LLColor3 sun = linearColor3(gPipeline.mSunDiffuse);
        const F32 brightness = sun.mV[VRED] * 0.2126f + sun.mV[VGREEN] * 0.7152f + sun.mV[VBLUE] * 0.0722f;
        if (!(brightness > 1e-3f))
        {
            return false;
        }
        for (U32 c = 0; c < 3; ++c)
        {
            const F32 own = sun.mV[c] / brightness;
            out[c] = own + (1.f - own) * FLARE_WHITE;
        }
        return true;
    }

    // Updates the sun's visibility target; false when it could not.
    bool renderSunVisibility(LLRenderTarget& screen, const LLRHI::GlareExposure& exposure, const F32 place[2])
    {
        Targets& t = targets();
        bool made = false;
        if (!sizeTarget(t.visibility, 1, 1, GL_R16F, made))
        {
            return false;
        }
        t.visibility_fresh = t.visibility_fresh || made;
        LLRHI::GlareSunVisibility pass;
        pass.screen_texture = screen.getTexture();
        pass.exposure = exposure;
        pass.sun_place[0] = place[0];
        pass.sun_place[1] = place[1];
        pass.radius[0] = SUN_RADIUS * (F32)screen.getHeight() / (F32)llmax(1u, screen.getWidth());
        pass.radius[1] = SUN_RADIUS;
        pass.dark = SUN_DARK;
        pass.bright = SUN_BRIGHT;
        pass.blend = t.visibility_fresh ? 1.f : SUN_BLEND;     // a new target holds nothing to average with
        t.visibility.bindTarget();
        const bool drawn = LLRHI::sGlareSunVisibility(pass);
        t.visibility.flush();
        if (drawn)
        {
            t.visibility_fresh = false;
        }
        return drawn;
    }

    // ------------------------------------------------------------------
    // The cross filter

    // A ray's passes: taps per pass and how many passes, the fewest that
    // reach `reach` texels (taps^passes - 1 >= reach).
    struct StreakPlan
    {
        U32 passes = 1;
        U32 taps = 2;
    };

    StreakPlan planStreak(F32 reach)
    {
        StreakPlan plan;
        for (plan.passes = 1; plan.passes <= CROSS_MAX_PASSES; ++plan.passes)
        {
            plan.taps = (U32)ceilf(powf(llmax(reach, 1.f) + 1.f, 1.f / (F32)plan.passes));
            if (plan.taps <= CROSS_MAX_TAPS)
            {
                plan.taps = llmax(plan.taps, 2u);
                return plan;
            }
        }
        plan.passes = CROSS_MAX_PASSES;     // 16^4 texels: never short in practice
        plan.taps = CROSS_MAX_TAPS;
        return plan;
    }

    // Draws the extracted light and the rays (targets().cross); false when
    // they could not be drawn (then nothing of them is added).
    bool renderCross(LLRenderTarget& screen, const LLRHI::GlareExposure& exposure, const Settings& settings)
    {
        Targets& t = targets();
        const U32 width = (screen.getWidth() + CROSS_DOWNSCALE - 1) / CROSS_DOWNSCALE;
        const U32 height = (screen.getHeight() + CROSS_DOWNSCALE - 1) / CROSS_DOWNSCALE;
        if (llmin(width, height) < CROSS_MIN_SIZE)
        {
            releaseCross();
            return false;
        }
        for (LLRenderTarget& target : t.cross)
        {
            bool made = false;
            if (!sizeTarget(target, width, height, GL_RGBA16F, made))
            {
                return false;
            }
        }

        LLRenderTarget& extract = t.cross[CROSS_EXTRACT];
        LLRHI::GlareStreakExtract lights;
        lights.screen_texture = screen.getTexture();
        lights.screen_width = screen.getWidth();
        lights.screen_height = screen.getHeight();
        lights.width = width;
        lights.height = height;
        lights.exposure = exposure;
        lights.threshold = CROSS_THRESHOLD;
        lights.knee = CROSS_KNEE;
        lights.limit = settings.cross_full;
        lights.ring_radius[0] = CROSS_RING * (F32)screen.getHeight() / (F32)llmax(1u, screen.getWidth());
        lights.ring_radius[1] = CROSS_RING;
        lights.point_full = CROSS_POINT_FULL;
        lights.point_none = CROSS_POINT_NONE;
        lights.depth_texture = gPipeline.mRT->deferredScreen.getDepth();    // the sky: depth 1 (0: not told apart)
        lights.sky = settings.cross_sky;
        F32 sun_fade = 0.f;
        if (brightBodyPlace(get_last_projection(), lights.sun_place, sun_fade))
        {
            lights.sun_radius[0] = CROSS_SUN_RADIUS * (F32)screen.getHeight() / (F32)llmax(1u, screen.getWidth());
            lights.sun_radius[1] = CROSS_SUN_RADIUS;
        }
        extract.bindTarget();
        bool ok = LLRHI::sGlareStreakExtract(lights);
        extract.flush();
        if (!ok)
        {
            return false;
        }

        // The ray falls off by `decay` a texel: to e^-CROSS_FADE at its length.
        const F32 length = llmax(settings.cross_length * (F32)height, 1.f);
        const F32 decay = expf(-CROSS_FADE / length);
        const StreakPlan plan = planStreak(length * CROSS_REACH);
        LLRenderTarget* between[2] = { &t.cross[CROSS_PING], &t.cross[CROSS_PONG] };
        for (U32 ray = 0; ray < settings.cross_rays; ++ray)
        {
            const F32 angle = settings.cross_angle + F_TWO_PI * (F32)ray / (F32)settings.cross_rays;
            const F32 dx = cosf(angle);
            const F32 dy = sinf(angle);
            const LLRenderTarget* source = &extract;
            F32 stride = 1.f;   // texels from one tap to the next in this pass
            for (U32 p = 0; p < plan.passes; ++p)
            {
                const bool last = p + 1 == plan.passes;
                LLRenderTarget& out = last ? t.cross[CROSS_RAYS] : *between[p % 2];
                LLRHI::GlareStreak streak;
                streak.source_texture = source->getTexture();
                streak.width = width;
                streak.height = height;
                // The taps lie towards the ray's root, against its direction.
                streak.step[0] = -dx * stride / (F32)width;
                streak.step[1] = -dy * stride / (F32)height;
                streak.tap_decay = powf(decay, stride);
                streak.taps = plan.taps;
                streak.add = last && ray > 0;   // the first ray replaces what the target held
                out.bindTarget();
                ok = LLRHI::sGlareStreak(streak);
                out.flush();
                if (!ok)
                {
                    return false;
                }
                source = &out;
                stride *= (F32)plan.taps;
            }
        }
        return true;
    }

    // ------------------------------------------------------------------
    // The composite in the tone mapping (PlusGlareInTonemap, research/125 #7)

    // Off by default: the sums are the same, but the screen no longer holds
    // them rounded to half floats before the tone mapping reads it, so a few
    // pixels may come out 1/255 apart (a hair more precise, not the same).
    bool inTonemapWanted()
    {
        static LLCachedControl<bool> in_tonemap(gSavedSettings, "PlusGlareInTonemap", false);
        return in_tonemap;
    }

    // What render() left to bindTonemap this frame.
    struct Pending
    {
        bool valid = false;
        LLRHI::GlareComposite composite;
    };
    Pending sPending;
    // The texture units bindTonemap took (unbindTonemap frees them); -1: none.
    S32 sTonemapFirstUnit = -1;
    constexpr S32 TONEMAP_UNITS = 4;    // bloom, visibility, rays, extracted light

    // Settings: app_settings/settings_plus.xml (PlusGlare, PlusLensFlare, PlusCrossFilter,
    // PlusCrossFilterRays, PlusCrossFilterAngle); PlusGlare is also written by the quality
    // levels (featuretable.txt).
    // Set in the photo tools: the glare's slider (plus/photomode/phototools.cpp, "param:glare"),
    // the cross filter's and the lens flare's rows (app_settings/plus/phototools/catalog.xml,
    // tab "color": the Light & Color tab).
    // Drawn from the <plus> hook in LLPipeline::renderFinalize().
    plus::FeatureRegistration sRegistration({ "glare", nullptr, {} });
}

namespace plus::glare
{
    void releaseWithoutHDR()
    {
        sPending.valid = false;
        releaseLevels();
        releaseVisibility();
        releaseCross();
    }

    void render()
    {
        sPending.valid = false;
        const Settings settings = readSettings();
        if (settings.glare <= 0.f)
        {
            releaseLevels();
        }
        if (settings.flare <= 0.f)
        {
            releaseVisibility();
        }
        if (settings.cross <= 0.f)
        {
            releaseCross();
        }
        if ((settings.glare <= 0.f && settings.flare <= 0.f && settings.cross <= 0.f) || !backendReady() || gSnapshotNoPost ||
            !sceneReady())
        {
            return;
        }

        LLRHI::gpuMark("glare");    // the GPU ms line's segment; the tone mapping's is marked again below
        LLRenderTarget& screen = gPipeline.mRT->screen;
        const LLRHI::GlareExposure exposure = exposureOf();
        // Read bilinearly here (the 13 taps and the sun's disc fall between texels);
        // the official tone mapping sets it back to point sampling when it binds it.
        screen.bindTexture(0, 0, LLTexUnit::TFO_BILINEAR);
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

        LLRHI::GlareComposite composite;
        composite.width = screen.getWidth();
        composite.height = screen.getHeight();
        composite.exposure = exposure;

        if (settings.glare > 0.f)
        {
            const U32 levels = renderBloom(screen, exposure);
            if (levels)
            {
                const LLRenderTarget& first = targets().levels[0];
                composite.bloom_texture = first.getTexture();
                composite.bloom_width = first.getWidth();
                composite.bloom_height = first.getHeight();
                composite.bloom_intensity = settings.glare / (F32)levels;
            }
        }

        // A large snapshot drawn in tiles (the camera's zoom is above 1 only
        // then, LLViewerWindow::rawSnapshot): the ghosts lie on the line from
        // the sun through the whole picture's centre, across other tiles,
        // which one tile cannot draw (it would put them round its own centre
        // and only near the sun). Left out there (fix-round6 4).
        const bool tiled = LLViewerCamera::getInstance()->getZoomFactor() > 1.f;
        if (settings.flare > 0.f && !tiled)
        {
            // The 3D scene's projection (renderDeferredLighting keeps it; HUDs may have loaded theirs since).
            F32 fade = 0.f;
            if (sunPlace(get_last_projection(), composite.sun_place, fade) && flareColor(composite.flare_color) &&
                renderSunVisibility(screen, exposure, composite.sun_place))
            {
                composite.visibility_texture = targets().visibility.getTexture();
                composite.flare_intensity = settings.flare * fade;
            }
            else
            {
                targets().visibility_fresh = true;  // the sun comes back without the old average
            }
        }

        if (settings.cross > 0.f)
        {
            LLRHI::gpuMark("crossfilter");
            if (renderCross(screen, exposure, settings))
            {
                const Targets& t = targets();
                composite.streak_texture = t.cross[CROSS_RAYS].getTexture();
                composite.extract_texture = t.cross[CROSS_EXTRACT].getTexture();
                composite.streak_width = t.cross[CROSS_RAYS].getWidth();
                composite.streak_height = t.cross[CROSS_RAYS].getHeight();
                composite.streak_intensity = settings.cross;
                composite.streak_rays = (F32)settings.cross_rays;
            }
            LLRHI::gpuMark("glare");    // the composite counts as the glare's
        }

        if (composite.bloom_texture || composite.visibility_texture || composite.streak_texture)
        {
            if (inTonemapWanted())
            {
                // The tone mapping adds them (bindTonemap), from the same targets.
                sPending.composite = composite;
                sPending.valid = true;
            }
            else
            {
                screen.bindTarget();
                LLRHI::sGlareComposite(composite);
                screen.flush();
            }
        }
        screen.bindTexture(0, 0, LLTexUnit::TFO_POINT);   // as the official passes left it
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        LLRHI::gpuMark("tonemap");
    }

    void bindTonemap(LLGLSLShader* shader)
    {
        static LLStaticHashedString s_on("plusGlare");
        static LLStaticHashedString s_bloom("plusGlareBloom");
        static LLStaticHashedString s_visibility("plusGlareVisibility");
        static LLStaticHashedString s_streak("plusGlareStreak");
        static LLStaticHashedString s_extract("plusGlareExtract");
        static LLStaticHashedString s_flare_color("plusGlareFlareColor");
        static LLStaticHashedString s_sizes("plusGlareSizes");
        static LLStaticHashedString s_sun("plusGlareSun");
        static LLStaticHashedString s_rays("plusGlareRays");
        const LLStaticHashedString* samplers[TONEMAP_UNITS] = { &s_bloom, &s_visibility, &s_streak, &s_extract };

        sTonemapFirstUnit = -1;
        if (!shader)
        {
            return;
        }
        // Samplers the program does not use point at its scene texture (bound, never read).
        const S32 scene_unit = shader->getTextureChannel(LLShaderMgr::DEFERRED_DIFFUSE);
        const LLRHI::GlareComposite& c = sPending.composite;
        const bool bloom = c.bloom_texture && c.bloom_width && c.bloom_height && c.bloom_intensity > 0.f;
        const bool flare = c.visibility_texture && c.flare_intensity > 0.f;
        const bool streak = c.streak_texture && c.extract_texture && c.streak_width && c.streak_height && c.streak_intensity > 0.f;
        // The units after the program's own (its samplers' are 0 .. active - 1).
        const S32 first_unit = shader->mActiveTextureChannels;
        const bool draw = sPending.valid && (bloom || flare || streak) && c.exposure.texture && c.width && c.height &&
                          scene_unit >= 0 && first_unit + TONEMAP_UNITS <= (S32)LL_NUM_TEXTURE_LAYERS;
        if (sPending.valid && !draw)
        {
            LL_WARNS_ONCE("PlusGlare") << "The tone mapping program cannot add the glare (scene texture unit " << scene_unit
                                       << ", first free unit " << first_unit << "); this frame has none" << LL_ENDL;
        }
        sPending.valid = false;
        shader->uniform1i(s_on, draw ? 1 : 0);
        if (!draw)
        {
            for (const LLStaticHashedString* sampler : samplers)
            {
                shader->uniform1i(*sampler, llmax(scene_unit, 0));
            }
            return;
        }

        // As the composite pass's uniforms (llvkglare.cpp Renderer::composite).
        const U32 textures[TONEMAP_UNITS] = { bloom ? c.bloom_texture : 0, flare ? c.visibility_texture : 0, streak ? c.streak_texture : 0,
                                              streak ? c.extract_texture : 0 };
        for (S32 i = 0; i < TONEMAP_UNITS; ++i)
        {
            if (textures[i])
            {
                gGL.getTexUnit(first_unit + i)->bindManual(LLTexUnit::TT_TEXTURE, textures[i]);
                shader->uniform1i(*samplers[i], first_unit + i);
            }
            else
            {
                shader->uniform1i(*samplers[i], scene_unit);
            }
        }
        gGL.getTexUnit(0)->activate();
        sTonemapFirstUnit = first_unit;

        shader->uniform4f(s_flare_color, flare ? c.flare_color[0] : 0.f, flare ? c.flare_color[1] : 0.f, flare ? c.flare_color[2] : 0.f,
                          flare ? c.flare_intensity : 0.f);
        shader->uniform4f(s_sizes, (F32)c.width, (F32)c.height, bloom ? 1.f / (F32)c.bloom_width : 0.f,
                          bloom ? 1.f / (F32)c.bloom_height : 0.f);
        shader->uniform4f(s_sun, c.sun_place[0], c.sun_place[1], bloom ? c.bloom_intensity : 0.f, c.exposure.scale);
        shader->uniform4f(s_rays, streak ? 1.f / (F32)c.streak_width : 0.f, streak ? 1.f / (F32)c.streak_height : 0.f,
                          streak ? c.streak_intensity : 0.f, streak ? c.streak_rays : 0.f);
    }

    void unbindTonemap()
    {
        if (sTonemapFirstUnit < 0)
        {
            return;
        }
        for (S32 i = 0; i < TONEMAP_UNITS; ++i)
        {
            gGL.getTexUnit(sTonemapFirstUnit + i)->unbind(LLTexUnit::TT_TEXTURE);
        }
        gGL.getTexUnit(0)->activate();
        sTonemapFirstUnit = -1;
    }
}

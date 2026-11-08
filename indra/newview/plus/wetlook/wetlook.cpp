/**
 * @file wetlook.cpp
 * @brief Wet look: wet ground and puddles, made in the G-buffer before the
 *        lighting. Registers with the plus foundation
 *        (research/120-rain-idea.md, stage 1).
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

// A rainy street photographs well for its wet ground: darker, deeper
// colours, the lights and the sky mirrored in it, puddles. This makes the
// main view's G-buffer wet between the G-buffer pass and the lighting, so
// the official lighting (the sun, local lights, reflection probes, SSR)
// lights wet surfaces as it lights any other; no official shader changes.
// The setting is the viewer's, not the region's weather.
//
// PlusWetLook: 0 off (the default), 1 weak, 2 medium, 3 strong.
// PlusWetPuddles: 0..1, how much of the level ground is under puddles.
// PlusWetAvatars: 0 avatars stay dry (the default), 1 a little wet, 2 as
// wet as the rest. Vulkan only: the passes are recorded by the backend
// (llvulkan/llvkwetlook.cpp). Not part of the graphics presets.
//
// Each frame (research/120 section 6):
//  1. Into a copy target (the G-buffer's first three attachments, sharing
//     the main view's depth): the G-buffer made wet. A water film on
//     surfaces by how they face up (walls a share, undersides none), more
//     under a cloudy sky; base colours darker and more saturated by how
//     porous the surface is, roughness lower; puddles on level ground (a
//     world-space noise, so they stay put) with rain ripples.
//  2. Back into the G-buffer, except the sky. While avatars stay dry or
//     take less, both passes test the avatar bit of plus/skinsss's mark.
// Snow lies on what faces up, but never on avatars' bare skin (their skin
// bakes, the system body, their eyes: the body's warmth melts it). Avatars
// told apart take none; avatars as wet as the rest take it like everything
// else, and a second pass on the bare skin bit of the mark draws the skin
// again without it.
// Under a roof the ground stays dry: plus/rain's depth map seen from straight
// above (research/120 section 8), read by the shader's skyExposure(). While
// it rains (PlusRain) the puddles' ripples are stronger and come faster.
//
// Rain wets what it falls on (PlusRainWetAll, on by default; research/120
// section 12): while it rains the passes run whatever PlusWetLook says, and
// everything open to the sky takes the rain's own film (walls more than the
// after-rain look gives them, level surfaces glossier, no dry patches), the
// puddles fill, avatars get a little wet even while PlusWetAvatars keeps
// them dry. The film builds up over seconds (faster in a downpour) and dries
// over a minute or two after the rain stops, the puddles slower; the wet
// look's own level, if higher, still applies.

#include "llviewerprecompiledheaders.h"

#include "plus.h"

#include "wetlook.h"

#include "plus/freeze/freeze.h"
#include "rain/rain.h"
#include "skinsss/skinsss.h"

#include "llagent.h"
#include "llappviewer.h"
#include "llcallbacklist.h"
#include "llenvironment.h"
#include "llframetimer.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llrhi.h"
#include "llrhiwetlook.h"
#include "llviewercontrol.h"
#include "pipeline.h"

#include <cmath>
#include <cstring>

extern bool gCubeSnapshot;      // llviewerdisplay.cpp: a reflection probe face or mirror is being drawn
extern bool gSnapshot;          // llviewerdisplay.cpp: a snapshot is being drawn

namespace
{
    constexpr char SETTING_LEVEL[] = "PlusWetLook";
    constexpr char SETTING_PUDDLES[] = "PlusWetPuddles";
    constexpr char SETTING_AVATARS[] = "PlusWetAvatars";
    constexpr char SETTING_RAIN_WET_ALL[] = "PlusRainWetAll";
    constexpr char SETTING_SNOW_GROUND[] = "PlusSnowGround";
    constexpr char SETTING_SNOW_ACCUMULATE[] = "PlusSnowAccumulate";

    enum Level : S32
    {
        LEVEL_OFF = 0,
        LEVEL_WEAK = 1,
        LEVEL_MEDIUM = 2,
        LEVEL_STRONG = 3,
        LEVEL_COUNT = 4,
    };

    // Per level: the water film on surfaces facing up (0..1), the share of
    // PlusWetPuddles the puddles take, and the rain ripples' tilt of the
    // puddles' normal.
    struct Strength
    {
        F32 wetness;
        F32 puddles;
        F32 ripples;
    };
    constexpr Strength STRENGTHS[LEVEL_COUNT] =
    {
        { 0.f,   0.f,  0.f   },     // off
        { 0.45f, 0.5f, 0.15f },     // weak: damp
        { 0.75f, 0.8f, 0.25f },     // medium: wet
        { 1.f,   1.f,  0.35f },     // strong: after a downpour
    };

    enum Avatars : S32
    {
        AVATARS_DRY = 0,
        AVATARS_WEAK = 1,
        AVATARS_WET = 2,
        AVATARS_COUNT = 3,
    };
    // A little wet avatars: this share of the film, and of its darkening
    // (skin and cloth take mostly the gloss); no puddles on them.
    constexpr F32 AVATAR_WEAK_WETNESS = 0.35f;
    constexpr F32 AVATAR_WEAK_DARKENING = 0.25f;

    // Under a cloudless sky the film is this share of the level's; under a
    // fully clouded one all of it (the sky's cloud coverage in between).
    // Not for the rain's film: what the rain falls on is wet whatever the sky.
    constexpr F32 CLEAR_SKY_SHARE = 0.7f;

    // What the rain (plus/rain's level: 1 drizzle, 2 rain, 3 downpour) wets
    // while PlusRainWetAll is on: the film it brings everything open to the
    // sky to, and the share of PlusWetPuddles its puddles fill to; how long
    // (s) the film and the puddles take to build up from dry to that.
    struct RainWet
    {
        F32 film;
        F32 puddles;
        F32 film_seconds;
        F32 puddle_seconds;
    };
    constexpr RainWet RAIN_WET[] =
    {
        { 0.f,  0.f,  1.f,  1.f  },     // no rain
        { 0.5f, 0.4f, 30.f, 120.f },    // drizzle: damp, few puddles
        { 0.8f, 0.8f, 15.f, 60.f },     // rain
        { 1.f,  1.f,  6.f,  30.f },     // downpour: soaked at once
    };
    constexpr S32 RAIN_LEVELS = sizeof(RAIN_WET) / sizeof(RAIN_WET[0]);
    // What falling snow (plus/rain's snow level: 1 light, 2 snow, 3 heavy)
    // lays: the cover it builds up to, and the seconds that takes from bare.
    // The ground snow (PlusSnowGround) is the least there is: the cover
    // rises to it in GROUND_SNOW_SECONDS, and melts back to it, from full to
    // none in SNOW_MELT_SECONDS, once the snow stops or gets lighter.
    // Turning the ground snow down takes the cover down to it at once (0: no
    // snow lying at all), falling snow then laying it again from there.
    // PlusSnowAccumulate off: no snow lies at all (cleared at once, nothing laid).
    struct SnowLaid
    {
        F32 cover;
        F32 seconds;
    };
    constexpr SnowLaid SNOW_LAID[] =
    {
        { 0.f,  1.f  },     // no snow
        { 0.35f, 90.f },    // light snow: a dusting
        { 0.65f, 60.f },    // snow
        { 1.f,  40.f },     // heavy snow: deep at once
    };
    constexpr S32 SNOW_LEVELS = sizeof(SNOW_LAID) / sizeof(SNOW_LAID[0]);
    constexpr F32 GROUND_SNOW_SECONDS = 2.f;
    constexpr F32 SNOW_MELT_SECONDS = 300.f;

    // After the rain (or when it gets lighter), from soaked to dry (s): the
    // film first, the puddles later.
    constexpr F32 FILM_DRY_SECONDS = 90.f;
    constexpr F32 PUDDLE_DRY_SECONDS = 240.f;
    // A frame longer than this (s: a stall, a minimized window) counts as this.
    constexpr F32 MAX_STEP = 0.25f;

    S32 levelIndex(S32 level)
    {
        return llclamp(level, (S32)LEVEL_OFF, (S32)LEVEL_COUNT - 1);
    }

    S32 currentLevel()
    {
        static LLCachedControl<S32> level(gSavedSettings, SETTING_LEVEL, LEVEL_OFF);
        return levelIndex(level);
    }

    F32 currentPuddles()
    {
        static LLCachedControl<F32> puddles(gSavedSettings, SETTING_PUDDLES, 0.4f);
        return llfinite((F32)puddles) ? llclamp((F32)puddles, 0.f, 1.f) : 0.4f;
    }

    S32 currentAvatars()
    {
        static LLCachedControl<S32> avatars(gSavedSettings, SETTING_AVATARS, AVATARS_DRY);
        return llclamp((S32)avatars, (S32)AVATARS_DRY, (S32)AVATARS_COUNT - 1);
    }

    bool rainWetAllOn()
    {
        static LLCachedControl<bool> wet_all(gSavedSettings, SETTING_RAIN_WET_ALL, true);
        return wet_all;
    }

    F32 snowGround()
    {
        static LLCachedControl<F32> ground(gSavedSettings, SETTING_SNOW_GROUND, 0.f);
        return llfinite((F32)ground) ? llclamp((F32)ground, 0.f, 1.f) : 0.f;
    }

    // PlusSnowAccumulate: off, no snow lies (neither the ground snow nor what falls).
    bool snowSettles()
    {
        static LLCachedControl<bool> settles(gSavedSettings, SETTING_SNOW_ACCUMULATE, true);
        return settles;
    }

    bool backendReady()
    {
        return LLRHI::sWetLook && LLRHI::sWetLookCompute && LLRHI::sWetLookWriteBack;
    }

    // ------------------------------------------------------------------
    // How wet the rain has made things: built up while it rains, dried after.
    // Advanced once a frame (not again for a snapshot's tiles, not while a
    // snapshot is drawn: its tiles show the same wetness; not while the world
    // is stopped, plus/freeze: neither wetter nor drier until it resumes).

    struct RainWetness
    {
        F32 film = 0.f;         // 0..1: the rain's film on what is open to the sky
        F32 puddles = 0.f;      // 0..1: the share of PlusWetPuddles its puddles fill
        U32 frame = 0;          // gFrameCount when last advanced
    };
    RainWetness sRainWetness;
    // avatarMarkWanted() when render() last asked: plus/skinsss is told when
    // it changes (the rain started wetting things or they have dried, snow
    // came to lie or melted away).
    bool sAvatarMarkWanted = false;

    // How deep the snow lies (0..1): built up while it snows, melting after,
    // never below the ground snow. Advanced with the rain's wetness.
    F32 sSnowCover = 0.f;
    // PlusSnowGround changed: on the next advance the cover is cut down to it
    // (set from the setting's signal; the cached control has the new value by then).
    bool sSnowGroundChanged = false;

    // value moved towards target: up in rise_seconds from 0 to 1, down in fall_seconds.
    F32 approach(F32 value, F32 target, F32 dt, F32 rise_seconds, F32 fall_seconds)
    {
        if (value < target)
        {
            return llmin(value + dt / llmax(rise_seconds, 1e-3f), target);
        }
        return llmax(value - dt / llmax(fall_seconds, 1e-3f), target);
    }

    // The rain's level now (0 without rain, or while it does not wet things).
    S32 rainWetLevel()
    {
        if (!rainWetAllOn())
        {
            return 0;
        }
        return llclamp(plus::rain::rainLevel(), 0, RAIN_LEVELS - 1);
    }

    void advanceRainWetness()
    {
        RainWetness& w = sRainWetness;
        if (w.frame == gFrameCount || gSnapshot)
        {
            return;
        }
        w.frame = gFrameCount;
        if (sSnowGroundChanged)
        {
            // Turned down: the snow lying goes down to it now (also while the
            // world is stopped: it is the photographer's say, not the weather).
            // Turned up: it rises to it as before, below.
            sSnowGroundChanged = false;
            sSnowCover = llmin(sSnowCover, snowGround());
        }
        if (!snowSettles())
        {
            // No snow lies at all, falling or ground snow (also while the world is stopped).
            sSnowCover = 0.f;
        }
        else if (!plus::freeze::isFrozen())
        {
            const F32 dt = llclamp(gFrameIntervalSeconds.value(), 0.f, MAX_STEP);
            const SnowLaid& laid = SNOW_LAID[llclamp(plus::rain::snowLevel(), 0, SNOW_LEVELS - 1)];
            const F32 ground = snowGround();
            const F32 target = llmax(laid.cover, ground);
            sSnowCover = approach(sSnowCover, target, dt, laid.cover > ground ? laid.seconds : GROUND_SNOW_SECONDS, SNOW_MELT_SECONDS);
        }

        if (!rainWetAllOn())
        {
            // Switched off: dry at once (only the rain stopping dries slowly),
            // also while the world is stopped.
            w.film = 0.f;
            w.puddles = 0.f;
            return;
        }
        if (plus::freeze::isFrozen())
        {
            return;
        }
        const RainWet& target = RAIN_WET[rainWetLevel()];
        const F32 dt = llclamp(gFrameIntervalSeconds.value(), 0.f, MAX_STEP);
        w.film = approach(w.film, target.film, dt, target.film_seconds, FILM_DRY_SECONDS);
        w.puddles = approach(w.puddles, target.puddles, dt, target.puddle_seconds, PUDDLE_DRY_SECONDS);
    }

    // Whether the rain keeps the passes running: it rains, or what it wetted has not dried yet.
    bool rainWetting()
    {
        // Puddles left to dry count only while there are puddles to show (PlusWetPuddles above 0).
        return rainWetAllOn() &&
               (rainWetLevel() > 0 || sRainWetness.film > 0.f || (sRainWetness.puddles > 0.f && currentPuddles() > 0.f));
    }

    // Whether the passes run at all.
    bool wetLookActive()
    {
        return backendReady() && (currentLevel() > LEVEL_OFF || rainWetting() || sSnowCover > 0.f);
    }

    // For plus::skinsss::addAvatarMarkUser: avatars are told from the rest
    // while they stay dry or take less, and their bare skin while snow lies
    // on them as on the rest (no snow on skin: the body's warmth melts it).
    bool avatarMarkWanted()
    {
        return wetLookActive() && (currentAvatars() != AVATARS_WET || sSnowCover > 0.f);
    }

    // The film's share by the sky: more under clouds.
    F32 skyShare()
    {
        LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky();
        const F32 coverage = sky ? llclamp(sky->getCloudShadow(), 0.f, 1.f) : 1.f;
        return lerp(CLEAR_SKY_SHARE, 1.f, coverage);
    }

    // x modulo period, in [0, period).
    F32 wrapped(F64 x, F64 period)
    {
        const F64 r = fmod(x, period);
        return (F32)(r < 0.0 ? r + period : r);
    }

    // ------------------------------------------------------------------
    // The copy target: the G-buffer's first three attachments (base colour
    // and specular RGBA8, the normal RGBA16: at least the G-buffer's own
    // precision), sharing the main view's depth (its stencil holds the
    // avatar mark). Made on first use and kept while the feature is on;
    // never destroyed by a static destructor, which would run without a GL
    // context.

    struct Scratch
    {
        LLRenderTarget target;
        U32 generation = 0;     // the screen buffers it shares the depth of
        plus::RetryWait retry;  // after it could not be made (VRAM full)
    };

    Scratch& scratch()
    {
        static Scratch* scratch = new Scratch();
        return *scratch;
    }

    void releaseScratch()
    {
        LLRenderTarget& target = scratch().target;
        if (target.isComplete())
        {
            target.release();
            LL_INFOS("WetLook") << "Wet look: copy target released" << LL_ENDL;
        }
    }

    // The copy target at the size of gbuffer, with its depth; false when it could not be made.
    bool prepareScratch(LLRenderTarget& gbuffer)
    {
        Scratch& s = scratch();
        const U32 width = gbuffer.getWidth();
        const U32 height = gbuffer.getHeight();
        const U32 generation = plus::skinsss::screenBuffersGeneration();
        if (s.target.isComplete() && s.generation == generation && s.target.getWidth() == width && s.target.getHeight() == height)
        {
            return true;
        }
        if (!s.retry.due())
        {
            return false;       // it could not be made a moment ago: not again every frame
        }
        releaseScratch();
        if (!s.target.allocate(width, height, GL_RGBA) || !s.target.addColorAttachment(GL_RGBA) ||
            !s.target.addColorAttachment(GL_RGBA16))
        {
            s.target.release();     // a half-made target may not be isComplete(): releaseScratch would skip it
            s.retry.failed();
            LL_WARNS_ONCE("WetLook") << "Wet look: no copy target at " << width << "x" << height << LL_ENDL;
            return false;
        }
        s.retry.succeeded();
        gbuffer.shareDepthBuffer(s.target);
        s.generation = generation;
        static plus::ResizeLog resize_log;     // once a second while the window frame is dragged (fix-round95 D)
        const LLError::ELevel log_level = resize_log.level(width, height);    // once: LL_VLOGS reads its level twice
        LL_VLOGS(log_level, "WetLook") << "Wet look: copy target " << width << "x" << height << LL_ENDL;
        return true;
    }

    void init()
    {
        // Settings: app_settings/settings_plus.xml (PlusWetLook, PlusWetPuddles,
        // PlusWetAvatars, PlusRainWetAll, PlusSnowGround, PlusSnowAccumulate). Photo tools:
        // app_settings/plus/phototools/catalog.xml (tab "light": the Sky & Weather tab).
        // Drawn from the <plus> hook in LLPipeline::renderDeferredLighting().
        plus::skinsss::addAvatarMarkUser(&avatarMarkWanted);
        // The avatar mark comes and goes with the settings (and with the
        // rain's wetness: render() asks when it starts or has dried). Asked
        // once idle, when the cached controls have the new values.
        for (const char* setting : { SETTING_LEVEL, SETTING_AVATARS })
        {
            if (LLControlVariable* control = gSavedSettings.getControl(setting))
            {
                control->getSignal()->connect([](LLControlVariable*, const LLSD&, const LLSD&)
                                              { doOnIdleOneTime(&plus::skinsss::markWantChanged); });
            }
        }
        // The ground snow slider sets how much snow lies now (only the snow's
        // cover: the rain's wetness is left as it is).
        if (LLControlVariable* control = gSavedSettings.getControl(SETTING_SNOW_GROUND))
        {
            control->getSignal()->connect([](LLControlVariable*, const LLSD&, const LLSD&)
                                          { sSnowGroundChanged = true; });
        }
    }

    plus::FeatureRegistration sRegistration({ "wetlook", &init, {} });
}

namespace plus::wetlook
{
    bool wantsOverhead()
    {
        return wetLookActive();
    }

    void render()
    {
        advanceRainWetness();
        const bool active = wetLookActive();
        const bool mark_wanted = avatarMarkWanted();
        if (mark_wanted != sAvatarMarkWanted)
        {
            // The rain started wetting things or they have dried, snow came to
            // lie or melted away: the avatar mark follows.
            sAvatarMarkWanted = mark_wanted;
            doOnIdleOneTime(&plus::skinsss::markWantChanged);
        }
        if (!active)
        {
            releaseScratch();
            return;
        }
        if (gCubeSnapshot || gPipeline.mRT != &gPipeline.mMainRT)
        {
            return;
        }
        LLRenderTarget& gbuffer = gPipeline.mRT->deferredScreen;
        if (!gbuffer.isComplete() || !gbuffer.getDepth() || gbuffer.getNumTextures() < 3 || gbuffer.isBoundInStack() ||
            !prepareScratch(gbuffer))
        {
            return;
        }

        const Strength& strength = STRENGTHS[currentLevel()];
        const RainWetness& rain = sRainWetness;
        LLRHI::WetLookCompute pass;
        pass.depth_texture = gbuffer.getDepth();
        pass.diffuse_texture = gbuffer.getTexture(0);
        pass.specular_texture = gbuffer.getTexture(1);
        pass.normal_texture = gbuffer.getTexture(2);
        // The camera's matrices, loaded for this frame's scene.
        const glm::mat4 modelview = get_current_modelview();
        const glm::mat4 inverse_projection = glm::inverse(get_current_projection());
        memcpy(pass.inverse_projection, glm::value_ptr(inverse_projection), sizeof(pass.inverse_projection));
        // Eye space -> agent coordinates, plus where the agent frame is in the
        // world (wrapped): the puddles stay put when the frame moves to another region.
        glm::mat4 eye_to_world = glm::inverse(modelview);
        const LLVector3d origin = gAgent.getPosGlobalFromAgent(LLVector3::zero);
        eye_to_world[3][0] += wrapped(origin.mdV[VX], LLRHI::WET_LOOK_WORLD_PERIOD);
        eye_to_world[3][1] += wrapped(origin.mdV[VY], LLRHI::WET_LOOK_WORLD_PERIOD);
        memcpy(pass.eye_to_world, glm::value_ptr(eye_to_world), sizeof(pass.eye_to_world));
        const glm::vec3 up = glm::normalize(glm::vec3(modelview * glm::vec4(0.f, 0.f, 1.f, 0.f)));
        pass.up[0] = up.x;
        pass.up[1] = up.y;
        pass.up[2] = up.z;
        // The wet look's level (after rain: by the sky) or the rain's, whichever is wetter.
        pass.wetness = llmax(strength.wetness * skyShare(), rain.film);
        pass.soaked = rain.film;
        pass.water_height = LLEnvironment::instance().getWaterHeight();
        pass.darkening = 1.f;
        pass.puddles = llmax(strength.puddles, rain.puddles) * currentPuddles();
        pass.ripples = strength.ripples;
        pass.snow = sSnowCover;
        pass.time = wrapped(plus::freeze::worldSeconds(), LLRHI::WET_LOOK_TIME_PERIOD);    // the ripples stop with the world
        // plus/rain: nothing under a roof (the overhead map); while it rains the puddles ripple more.
        pass.overhead = plus::rain::overheadForEye(modelview);
        F32 rain_ripples = 0.f;
        F32 rain_ripple_rate = 0.f;
        if (plus::rain::wetLookRipples(rain_ripples, rain_ripple_rate))
        {
            pass.ripples = llmax(pass.ripples, rain_ripples);
            pass.ripple_rate = rain_ripple_rate;
        }

        // Avatars told apart by the mark only when this frame's G-buffer pass
        // wrote it (otherwise, for the frame or two until it does, as wet as the rest).
        const S32 avatars = currentAvatars();
        const bool avatars_marked = plus::skinsss::avatarsMarkedThisFrame();
        const bool marked = avatars != AVATARS_WET && avatars_marked;
        // Avatars as wet as the rest take the snow too, but not on their bare
        // skin (LLRHI::STENCIL_AVATAR_SKIN): drawn again there without it.
        // (Avatars told apart lay none: their own pass below has no snow.)
        const bool bare_skin = avatars == AVATARS_WET && avatars_marked && pass.snow > 0.f;
        // A little wet avatars: the film they take a share of. By the setting
        // (all of it), or by the rain while the setting keeps them dry (the
        // rain's own: the after-rain look leaves them dry).
        const F32 avatar_film = avatars == AVATARS_WEAK ? pass.wetness : rain.film;

        LLRHI::gpuMark("wetlook");
        gGL.flush();
        LLRenderTarget& copy = scratch().target;
        copy.bindTarget();
        bool drawn = false;
        LLRHI::WetLookPixels written = LLRHI::WET_LOOK_EVERYWHERE;
        if (!marked)
        {
            drawn = LLRHI::sWetLookCompute(pass);
            if (drawn && bare_skin)
            {
                // Over the pass above (the copy target holds what it drew;
                // this draws the same, less the snow): the write-back stays everywhere.
                LLRHI::WetLookCompute on_skin = pass;
                on_skin.pixels = LLRHI::WET_LOOK_AVATAR_SKIN;
                on_skin.snow = 0.f;
                LLRHI::sWetLookCompute(on_skin);
            }
        }
        else
        {
            pass.pixels = LLRHI::WET_LOOK_NOT_AVATARS;
            drawn = LLRHI::sWetLookCompute(pass);
            written = LLRHI::WET_LOOK_NOT_AVATARS;
            if (drawn && avatar_film > 0.f)
            {
                LLRHI::WetLookCompute on_avatars = pass;
                on_avatars.pixels = LLRHI::WET_LOOK_AVATARS;
                on_avatars.wetness = avatar_film * AVATAR_WEAK_WETNESS;
                on_avatars.soaked *= AVATAR_WEAK_WETNESS;    // the rain's gloss in the same share
                on_avatars.darkening = AVATAR_WEAK_DARKENING;
                on_avatars.puddles = 0.f;
                on_avatars.snow = 0.f;
                on_avatars.ripples = 0.f;
                if (LLRHI::sWetLookCompute(on_avatars))
                {
                    written = LLRHI::WET_LOOK_EVERYWHERE;
                }
            }
        }
        copy.flush();
        if (drawn)
        {
            // Only where the copy target was drawn: elsewhere it holds an older frame.
            LLRHI::WetLookWriteBack write_back;
            write_back.diffuse_texture = copy.getTexture(0);
            write_back.specular_texture = copy.getTexture(1);
            write_back.normal_texture = copy.getTexture(2);
            write_back.pixels = written;
            gbuffer.bindTarget();
            LLRHI::sWetLookWriteBack(write_back);
            gbuffer.flush();
        }
        LLRHI::gpuMark("lighting");     // the GPU ms line: the rest of the lighting goes back to its segment
    }
}

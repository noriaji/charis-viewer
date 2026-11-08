/**
 * @file atmosphere.cpp
 * @brief Atmosphere effects for photography: ground fog (and the light shafts
 *        in it), fireflies. Registers with the plus foundation
 *        (research/148-atmosphere-effects.md).
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

// All off by default, none part of the graphics presets. Vulkan only: the
// passes are recorded by the backend (llvulkan/llvkatmosphere.cpp); with GL
// nothing is drawn. Settings: app_settings/settings_plus.xml (PlusGroundFog*,
// PlusFirefl*), photo tools: app_settings/plus/phototools/catalog.xml (tab
// "light").
//
// Fireflies (PlusFireflies 0 off, 1 a few, 2 some, 3 many): points of light
// drifting in a layer of air above the ground (from just off it to
// PlusFireflyHeight), fixed in the world: they stay where they are when the
// camera turns or moves. Each wanders slowly around the middle of its own
// patch of ground and blinks. They are added to the scene after the exposure
// is measured and before the glare (so that the glare and the cross filter
// turn the bright ones into stars), nearer than anything solid. PlusFireflyColor
// picks their colour, PlusFireflyBrightness how bright they shine.
//
// Ground fog (PlusGroundFog 0 off, 1 light, 2 morning mist, 3 thick): air whose
// density is greatest at a reference level, the ground (the land or the
// water, whichever is higher, under the camera, followed with a few seconds'
// delay so that walking up a slope does not make the fog jump), and falls off
// exponentially above it (PlusGroundFogHeight: by e over this many metres).
// One pass over the scene before the alpha (the way the volumetric light is
// drawn): each pixel's scene is dimmed by the fog along its ray, and the fog's
// own light is added: the sky's light, and the sun's (moon's) light scattered
// forwards, so that looking into the sun the fog glows. The glow is not
// shadowed (a roof does not darken it); the shafts of plus/volumetric are
// (research/148, section 2).

#include "llviewerprecompiledheaders.h"

#include "plus.h"

#include "atmosphere.h"

#include "llagent.h"
#include "llenvironment.h"
#include "llframetimer.h"
#include "llgl.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llrhi.h"
#include "llrhiatmosphere.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llworld.h"
#include "pipeline.h"
#include "v3color.h"
#include "plus/freeze/freeze.h"
#include "plus/volumetric/volumetric.h"

#include <glm/gtc/type_ptr.hpp>

#include <cmath>
#include <cstring>

extern bool gCubeSnapshot;      // llviewerdisplay.cpp: a reflection probe face or mirror is being drawn
extern bool gSnapshot;          // llviewerdisplay.cpp: a snapshot is being drawn

namespace
{
    constexpr char SETTING_FOG[] = "PlusGroundFog";
    constexpr char SETTING_FOG_HEIGHT[] = "PlusGroundFogHeight";
    constexpr char SETTING_FOG_GLOW[] = "PlusGroundFogGlow";

    enum FogLevel : S32
    {
        FOG_OFF = 0,
        FOG_LIGHT = 1,
        FOG_MIST = 2,
        FOG_THICK = 3,
        FOG_LEVEL_COUNT = 4,
    };

    // Per level: extinction per metre at the ground (a ray along the ground
    // keeps 37% of the scene's light over 100 m, 40 m and 17 m).
    constexpr F32 FOG_DENSITIES[FOG_LEVEL_COUNT] = { 0.f, 0.010f, 0.025f, 0.060f };

    constexpr F32 DEFAULT_HEIGHT = 6.f;     // m
    constexpr F32 MIN_HEIGHT = 1.f;
    constexpr F32 MAX_HEIGHT = 60.f;
    constexpr F32 MAX_GLOW = 4.f;

    // No ray is followed farther; the sky counts as this far.
    constexpr F32 FOG_MAX_DISTANCE = 400.f;

    // The fog's light. Its colour is the sky's light, this much towards white
    // (mist is whiter than the sky above it); the sun's light is scattered by
    // this share (an isotropic fog would show all of it) times the phase
    // function (anisotropy: bright towards the sun, 7.7 times the isotropic at
    // the sun itself, 0.5 at right angles); the moon's is weaker. When the
    // shafts of plus/volumetric are drawn too, the fog's own sun glow is only
    // this share of it (the shafts add the rest, shadowed).
    constexpr F32 FOG_TINT_WHITE = 0.5f;
    constexpr F32 SUN_SCATTER_SHARE = 0.15f;
    constexpr F32 FOG_ANISOTROPY = 0.55f;
    constexpr F32 MOON_SCALE = 0.4f;
    constexpr F32 OWN_SHARE_WITH_SHAFTS = 0.4f;
    // Under a classic sky the scene is lit by the sun's colour as it is (not made
    // linear) times plus::CLASSIC_SUNLIGHT_SCALE, and by the sky's ambient as it is
    // (plus::skyAmbient); the fog takes both the same way.
    // Under the "no post" tone mapping (plus::isNoPost: a classic sky, or a newer
    // sky with its reflection probe ambiance at 0) nothing scales the picture down
    // and every channel is clamped to 1: the fog's light (its brightest channel, hue
    // kept) is held so that the sun's glow at its brightest (the phase function's
    // peak) and the sky's light together stay under 1, and the fog never paints the
    // sky white. (fix-round43 1) The sky's light alone is held under 1 less
    // FOG_NO_POST_MIN_SUN first (a sky brighter than 1). (fix-round48 3)
    constexpr F32 FOG_NO_POST_CEILING = 1.f;
    constexpr F32 FOG_NO_POST_MIN_SUN = 0.25f;

    // The reference level follows the ground under the camera with this time
    // constant (s); a jump bigger than SNAP_DISTANCE (m) (a teleport) is taken at once.
    constexpr F32 GROUND_SMOOTHING = 3.f;
    constexpr F32 SNAP_DISTANCE = 30.f;

    constexpr char SETTING_FIREFLIES[] = "PlusFireflies";
    constexpr char SETTING_FIREFLY_COLOR[] = "PlusFireflyColor";
    constexpr char SETTING_FIREFLY_BRIGHTNESS[] = "PlusFireflyBrightness";
    constexpr char SETTING_FIREFLY_HEIGHT[] = "PlusFireflyHeight";

    constexpr S32 FIREFLY_LEVEL_COUNT = 4;
    constexpr S32 FIREFLY_COLOR_COUNT = 4;

    // Per level: the chance that a cell holds a firefly (a cell is FIREFLY_CELL
    // square: 6 m^2, so "some" is one in 40 m^2, 120 or so within the range).
    constexpr F32 FIREFLY_PROBABILITY[FIREFLY_LEVEL_COUNT] = { 0.f, 0.06f, 0.15f, 0.4f };
    // Peaks, scene units after the exposure: far above 1, so that the glare and the cross filter find them.
    constexpr F32 FIREFLY_COLORS[FIREFLY_COLOR_COUNT][3] =
    {
        { 0.85f, 1.00f, 0.30f },    // yellow-green, a firefly's
        { 1.00f, 0.95f, 0.85f },    // warm white
        { 1.00f, 0.65f, 0.20f },    // gold
        { 0.40f, 0.70f, 1.00f },    // blue
    };
    constexpr F32 FIREFLY_PEAK = 6.f;
    // Under the "no post" tone mapping (plus::isNoPost) nothing scales the picture
    // down and every channel is clamped to 1: the peak, PlusFireflyBrightness
    // included, is no more than this (its brightest channel, so the hue stays; at
    // 6 the core came out white whatever the colour). (fix-round43 1, fix-round67 1)
    constexpr F32 FIREFLY_NO_POST_PEAK = 0.9f;
    constexpr F32 FIREFLY_CELL = 2.5f;          // m
    constexpr F32 FIREFLY_RANGE = 40.f;         // m
    constexpr F32 FIREFLY_BOTTOM = 0.4f;        // m above the ground
    constexpr F32 FIREFLY_DEFAULT_TOP = 3.f;
    constexpr F32 FIREFLY_MIN_TOP = 1.5f;
    constexpr F32 FIREFLY_MAX_TOP = 10.f;
    constexpr F32 FIREFLY_MAX_BRIGHTNESS = 4.f;
    constexpr F32 FIREFLY_CORE = 0.02f;         // m
    constexpr F32 FIREFLY_GLOW = 0.12f;         // m (standard deviation)
    constexpr F32 FIREFLY_GLOW_SHARE = 0.05f;

    S32 fogLevel()
    {
        static LLCachedControl<S32> level(gSavedSettings, SETTING_FOG, FOG_OFF);
        return llclamp((S32)level, (S32)FOG_OFF, (S32)FOG_LEVEL_COUNT - 1);
    }

    F32 finiteOr(F32 value, F32 fallback)
    {
        return llfinite(value) ? value : fallback;
    }

    bool backendReady()
    {
        return LLRHI::sAtmosphere && LLRHI::sGroundFog;
    }

    // The reference level, m (agent space z): the higher of the land and the
    // water under the camera, followed with a delay;
    // cheap enough to call more than once a frame (it follows elapsed time).
    F32 groundLevel(const LLVector3& camera_agent)
    {
        static F32 level = 0.f;
        static F64 last = -1.0;
        const F64 now = LLTimer::getElapsedSeconds();
        if (now == last)
        {
            return level;
        }
        F32 target = LLWorld::getInstance()->resolveLandHeightAgent(camera_agent);
        if (const LLViewerRegion* region = gAgent.getRegion())
        {
            target = llmax(target, region->getWaterHeight());
        }
        // A gap of over a second (the fog and the fireflies were both off, or the frames stalled) starts
        // again from where the ground is now, as a snap does; not from the level of back then.
        if (last < 0.0 || now - last > 1.0 || fabsf(target - level) > SNAP_DISTANCE)
        {
            level = target;
        }
        else
        {
            const F32 dt = llclamp((F32)(now - last), 0.f, 1.f);
            level += (target - level) * (1.f - expf(-dt / GROUND_SMOOTHING));
        }
        last = now;
        return level;
    }

    // The fog's settings, or false while it is off or cannot be drawn.
    bool fogSettings(F32& density, F32& height, F32& glow)
    {
        const S32 level = fogLevel();
        if (level == FOG_OFF || !backendReady() || LLViewerCamera::getInstance()->cameraUnderWater())
        {
            return false;
        }
        static LLCachedControl<F32> height_setting(gSavedSettings, SETTING_FOG_HEIGHT, DEFAULT_HEIGHT);
        static LLCachedControl<F32> glow_setting(gSavedSettings, SETTING_FOG_GLOW, 1.f);
        density = FOG_DENSITIES[level];
        height = llclamp(finiteOr(height_setting, DEFAULT_HEIGHT), MIN_HEIGHT, MAX_HEIGHT);
        glow = llclamp(finiteOr(glow_setting, 1.f), 0.f, MAX_GLOW);
        return true;
    }

    // The fireflies' clock: advanced once a frame, held while a snapshot draws
    // (its tiles then show the same fireflies) and while the world is stopped
    // (plus/freeze).
    F64 sFireflyTime = 0.0;

    S32 fireflyLevel()
    {
        static LLCachedControl<S32> level(gSavedSettings, SETTING_FIREFLIES, 0);
        return llclamp((S32)level, 0, FIREFLY_LEVEL_COUNT - 1);
    }

    // Whether the fireflies can be seen: the backend draws them and HDR is on, as
    // LLPipeline::renderFinalize() decides whether to call renderFireflies().
    bool fireflyBackendReady()
    {
        static LLCachedControl<bool> has_hdr(gSavedSettings, "RenderHDREnabled", true);
        return LLRHI::sAtmosphere && LLRHI::sFireflies && gGLManager.mGLVersion > 4.05f && has_hdr();
    }

    // Set by renderFogBeforeAlpha() when it drew this frame's fog.
    bool sFogDrawnBeforeAlpha = false;

    // The fog's pass for the matrices of the frame, or false when there is none to draw.
    bool fogPass(const glm::mat4& modelview, const glm::mat4& projection, LLRHI::GroundFog& pass)
    {
        F32 density, height, glow;
        if (!fogSettings(density, height, glow))
        {
            return false;
        }
        LLRenderTarget& screen = gPipeline.mRT->screen;
        LLRenderTarget& gbuffer = gPipeline.mRT->deferredScreen;
        if (!screen.isComplete() || !gbuffer.getDepth())
        {
            return false;
        }

        const glm::mat4 eye_to_agent = glm::inverse(modelview);
        const LLVector3 camera_agent(eye_to_agent[3][0], eye_to_agent[3][1], eye_to_agent[3][2]);

        pass = LLRHI::GroundFog();
        pass.depth_texture = gbuffer.getDepth();
        memcpy(pass.inverse_projection, glm::value_ptr(glm::inverse(projection)), sizeof(pass.inverse_projection));
        glm::mat4 eye_to_world = eye_to_agent;
        eye_to_world[3] = glm::vec4(0.f, 0.f, 0.f, 1.f);
        memcpy(pass.eye_to_world, glm::value_ptr(eye_to_world), sizeof(pass.eye_to_world));
        pass.density = density;
        pass.inverse_height = 1.f / height;
        pass.camera_height = camera_agent.mV[VZ] - groundLevel(camera_agent);
        pass.max_distance = FOG_MAX_DISTANCE;

        // The light: the sky's, towards white; the sun's (the moon's at night).
        LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky();
        const bool classic = plus::isClassicSky(sky.get());
        // Under a classic sky the scene takes the sky's ambient as it is (sRGB, not made linear), as it
        // takes the sun's: so does the fog (made linear there, its sky light was about half of the scene's
        // ambient, the fog darker than what it covers; a sky brighter than 1 is held below). (fix-round67 7a)
        const LLColor3 ambient = sky ? plus::skyAmbient(sky.get(), classic) : LLColor3(0.5f, 0.5f, 0.5f);
        const F32 luma = ambient.mV[VRED] * 0.2126f + ambient.mV[VGREEN] * 0.7152f + ambient.mV[VBLUE] * 0.0722f;
        const bool sun_up = LLEnvironment::instance().getIsSunUp();
        const LLColor4& diffuse = sun_up ? gPipeline.mSunDiffuse : gPipeline.mMoonDiffuse;
        const LLColor3 light = classic ? LLColor3(diffuse.mV[VRED], diffuse.mV[VGREEN], diffuse.mV[VBLUE]) * plus::CLASSIC_SUNLIGHT_SCALE
                                       : linearColor3(diffuse);
        LLVector3 towards(sun_up ? gPipeline.mSunDir.mV : gPipeline.mMoonDir.mV);
        if (towards.normalize() <= 0.f)
        {
            towards.set(0.f, 0.f, 1.f);
        }
        const F32 own_share = plus::volumetric::drawsShafts() ? OWN_SHARE_WITH_SHAFTS : 1.f;
        F32 sun_share = SUN_SCATTER_SHARE * glow * own_share * (sun_up ? 1.f : MOON_SCALE);
        // The sky's light, towards white.
        LLColor3 fog_ambient;
        for (U32 c = 0; c < 3; ++c)
        {
            const F32 tint = luma > 1e-4f ? ambient.mV[c] / luma : 1.f;
            fog_ambient.mV[c] = luma * lerp(tint, 1.f, FOG_TINT_WHITE);
        }
        if (plus::isNoPost(sky.get()))
        {
            // The sky's light (its brightest channel, hue kept) leaves the sun at least
            // FOG_NO_POST_MIN_SUN of room under the ceiling: a sky brighter than 1 (luma
            // 1 or more) turned thick fog white. (fix-round48 3)
            const F32 ambient_ceiling = FOG_NO_POST_CEILING - FOG_NO_POST_MIN_SUN;
            const F32 brightest_ambient = llmax(fog_ambient.mV[VRED], fog_ambient.mV[VGREEN], fog_ambient.mV[VBLUE]);
            if (brightest_ambient > ambient_ceiling)
            {
                fog_ambient *= ambient_ceiling / brightest_ambient;
            }
            // The phase function's peak (towards the light): (1 + g) / (1 - g)^2.
            const F32 peak_phase = (1.f + FOG_ANISOTROPY) / ((1.f - FOG_ANISOTROPY) * (1.f - FOG_ANISOTROPY));
            const F32 brightest_light = llmax(light.mV[VRED], light.mV[VGREEN], light.mV[VBLUE]);
            const F32 room = llmax(FOG_NO_POST_CEILING - llmin(brightest_ambient, ambient_ceiling), FOG_NO_POST_MIN_SUN);
            if (brightest_light * sun_share * peak_phase > room)
            {
                sun_share = room / (brightest_light * peak_phase);
            }
        }
        for (U32 c = 0; c < 3; ++c)
        {
            pass.ambient[c] = fog_ambient.mV[c];
            pass.sun[c] = light.mV[c] * sun_share;
            pass.sun_direction[c] = towards.mV[c];
        }
        pass.anisotropy = FOG_ANISOTROPY;
        pass.width = screen.getWidth();
        pass.height = screen.getHeight();
        return true;
    }

    // True when drawn. screen_bound: mRT->screen is the bound target (drawn
    // into as it is); otherwise it is bound for the pass.
    bool drawFog(const glm::mat4& modelview, const glm::mat4& projection, bool screen_bound)
    {
        LLRHI::GroundFog pass;
        if (!fogPass(modelview, projection, pass))
        {
            return false;
        }
        LLRenderTarget& screen = gPipeline.mRT->screen;
        if (screen_bound ? LLRenderTarget::getCurrentBoundTarget() != &screen : screen.isBoundInStack())
        {
            return false;
        }
        gGL.flush();                    // what was drawn into the bound target before
        LLRHI::gpuMark("ground fog");   // the GPU ms line's segment, until the caller's next mark
        if (screen_bound)
        {
            return LLRHI::sGroundFog(pass);
        }
        screen.bindTarget();
        const bool drawn = LLRHI::sGroundFog(pass);
        screen.flush();
        return drawn;
    }

    void init()
    {
        // Settings: app_settings/settings_plus.xml; photo tools: app_settings/plus/phototools/catalog.xml.
    }

    plus::FeatureRegistration sRegistration({ "atmosphere", &init, {} });
}

namespace plus::atmosphere
{
    void renderFogBeforeAlpha()
    {
        // The main view only, as renderFog and shaftMist (a probe face or a mirror drawn
        // here would also mark the main view's fog as drawn and renderFog would skip it).
        if (fogLevel() == FOG_OFF || gCubeSnapshot || gPipeline.mRT != &gPipeline.mMainRT)
        {
            return;
        }
        // The camera view of this frame (gGLModelView) and the projection loaded for its scene (the "last" matrices are
        // kept only at the end of renderDeferredLighting, after the alpha).
        if (drawFog(glm::make_mat4(gGLModelView), get_current_projection(), true))
        {
            sFogDrawnBeforeAlpha = true;
        }
        LLRHI::gpuMark("alpha");    // the rest of the pool goes back to its segment
    }

    bool shaftMist(const glm::mat4& modelview, ShaftMist& mist)
    {
        F32 density, height, glow;
        if (!fogSettings(density, height, glow) || gPipeline.mRT != &gPipeline.mMainRT)
        {
            return false;
        }
        const glm::mat4 eye_to_agent = glm::inverse(modelview);
        const LLVector3 camera_agent(eye_to_agent[3][0], eye_to_agent[3][1], eye_to_agent[3][2]);
        // The world's up (z) in eye space: the view matrix's third column.
        const glm::vec3 up = glm::normalize(glm::vec3(modelview[2]));
        mist.up_eye[0] = up.x;
        mist.up_eye[1] = up.y;
        mist.up_eye[2] = up.z;
        mist.camera_height = camera_agent.mV[VZ] - groundLevel(camera_agent);
        mist.inverse_height = 1.f / height;
        mist.scatter = density * SUN_SCATTER_SHARE * glow * (1.f - OWN_SHARE_WITH_SHAFTS);
        mist.extinction = density;
        return true;
    }

    void renderFireflies()
    {
        const S32 level = fireflyLevel();
        if (level == 0 || !fireflyBackendReady())
        {
            return;
        }
        if (gCubeSnapshot || gPipeline.mRT != &gPipeline.mMainRT || LLViewerCamera::getInstance()->cameraUnderWater())
        {
            return;
        }
        LLRenderTarget& screen = gPipeline.mRT->screen;
        LLRenderTarget& gbuffer = gPipeline.mRT->deferredScreen;
        if (!screen.isComplete() || screen.isBoundInStack() || !gPipeline.mExposureMap.isComplete() || !gbuffer.getDepth())
        {
            return;
        }

        static LLCachedControl<S32> color_setting(gSavedSettings, SETTING_FIREFLY_COLOR, 0);
        static LLCachedControl<F32> brightness_setting(gSavedSettings, SETTING_FIREFLY_BRIGHTNESS, 1.f);
        static LLCachedControl<F32> height_setting(gSavedSettings, SETTING_FIREFLY_HEIGHT, FIREFLY_DEFAULT_TOP);
        const S32 color = llclamp((S32)color_setting, 0, FIREFLY_COLOR_COUNT - 1);
        const F32 brightness = llclamp(finiteOr(brightness_setting, 1.f), 0.f, FIREFLY_MAX_BRIGHTNESS);
        const F32 top = llclamp(finiteOr(height_setting, FIREFLY_DEFAULT_TOP), FIREFLY_MIN_TOP, FIREFLY_MAX_TOP);

        if (!gSnapshot && !plus::freeze::isFrozen())
        {
            const F32 dt = llclamp(gFrameIntervalSeconds.value(), 0.f, 0.25f);
            sFireflyTime = fmod(sFireflyTime + (F64)dt, (F64)LLRHI::FIREFLY_TIME_PERIOD);
        }

        // The 3D scene's matrices (renderDeferredLighting keeps them; HUDs may have loaded theirs since).
        const glm::mat4 modelview = get_last_modelview();
        const glm::mat4 projection = get_last_projection();
        const glm::mat4 eye_to_agent = glm::inverse(modelview);
        const LLVector3 camera_agent(eye_to_agent[3][0], eye_to_agent[3][1], eye_to_agent[3][2]);
        const LLVector3d camera_global = gAgent.getPosGlobalFromAgent(camera_agent);
        const F32 ground = groundLevel(camera_agent);

        LLRHI::Fireflies pass;
        pass.depth_texture = gbuffer.getDepth();
        pass.exposure_texture = gPipeline.mExposureMap.getTexture();
        pass.exposure_scale = plus::exposureScaleForPasses();  // 0 under plus::isNoPost: the exposure taken as 1 (fix-round48)
        memcpy(pass.inverse_projection, glm::value_ptr(glm::inverse(projection)), sizeof(pass.inverse_projection));
        glm::mat4 eye_to_world = eye_to_agent;
        eye_to_world[3] = glm::vec4(0.f, 0.f, 0.f, 1.f);
        memcpy(pass.eye_to_world, glm::value_ptr(eye_to_world), sizeof(pass.eye_to_world));
        // The cells are the world's: the camera's cell and its place in it.
        pass.cell_size = FIREFLY_CELL;
        for (U32 axis = 0; axis < 2; ++axis)
        {
            const F64 cells = floor(camera_global.mdV[axis] / (F64)FIREFLY_CELL);
            pass.cell_base[axis] = (U32)(S64)cells;
            pass.camera_in_cell[axis] = (F32)(camera_global.mdV[axis] - cells * (F64)FIREFLY_CELL);
        }
        pass.layer_bottom = ground + FIREFLY_BOTTOM - camera_agent.mV[VZ];
        pass.layer_top = ground + top - camera_agent.mV[VZ];
        pass.range = FIREFLY_RANGE;
        pass.probability = FIREFLY_PROBABILITY[level];
        pass.time = (F32)sFireflyTime;
        pass.core_radius = FIREFLY_CORE;
        pass.glow_radius = FIREFLY_GLOW;
        pass.glow_share = FIREFLY_GLOW_SHARE;
        // Under no post the ceiling holds the brightness setting too: the setting dims the
        // fireflies below 1 as before, but no brightness takes them above the ceiling (it was
        // taken after it, and a brightness over about 1.1 drove the core past 1). (fix-round67 1)
        const F32 peak = plus::isNoPost() ? FIREFLY_NO_POST_PEAK * llmin(brightness, 1.f) : FIREFLY_PEAK * brightness;
        for (U32 c = 0; c < 3; ++c)
        {
            pass.color[c] = FIREFLY_COLORS[color][c] * peak;
        }
        // One pixel's height seen from the camera (projection[1][1] = 1 / tan(fov / 2)).
        pass.pixel_angle = projection[1][1] > 0.f ? 2.f / (projection[1][1] * (F32)llmax(1u, screen.getHeight())) : 0.f;
        pass.width = screen.getWidth();
        pass.height = screen.getHeight();

        LLRHI::gpuMark("fireflies");    // the GPU ms line's segment; the tone mapping's is marked again below
        screen.bindTarget();
        LLRHI::sFireflies(pass);
        screen.flush();
        LLRHI::gpuMark("tonemap");
    }

    void renderFog()
    {
        if (sFogDrawnBeforeAlpha)
        {
            sFogDrawnBeforeAlpha = false;   // this frame's is in the scene already
            return;
        }
        if (fogLevel() == FOG_OFF || gCubeSnapshot || gPipeline.mRT != &gPipeline.mMainRT)
        {
            return;
        }
        // The 3D scene's matrices (renderDeferredLighting keeps them; HUDs may have loaded theirs since).
        drawFog(get_last_modelview(), get_last_projection(), false);
    }
}

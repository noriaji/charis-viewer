/**
 * @file volumetric.cpp
 * @brief "Volumetric light": sun shafts, the light the air scatters towards
 *        the eye where the sun reaches it. Registers with the plus foundation
 *        (research/84-volumetric-lighting.md).
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

// Off by default (PlusVolumetric). Vulkan only: the passes are recorded by the
// backend (llvulkan/llvkvolumetric.cpp); with GL nothing is drawn.
//
// Each frame, before tone mapping:
//  1. march: into a target of 1/scale of the screen each way, each pixel
//     walks its view ray from the eye to the scene (at most to the end of the
//     sun's last shadow cascade) and sums, sample by sample, the light the
//     sun's shadow maps say reaches that point, as much of it as the air
//     scatters towards the eye (Henyey-Greenstein, forwards).
//  2. composite: that light, upsampled with the scene depth as the guide, is
//     laid over mRT->screen: the scene behind the air dims by what the air
//     takes out of it, and the air's own light (the sun's, shadowed, and the
//     sky's) takes its place, never more than the air took (fix-r9). Exposure
//     and tone mapping then treat it like the rest of the scene.
// The sun by day, the moon by night: whichever cast this frame's shadows.
//
// Sunbeams through the volumetric clouds (PlusCloudShafts, off by default;
// research/253): with plus/volclouds' map of the clouds' shadow, the march
// dims the light reaching its near air by the clouds above it, and past its
// end, up to the clouds' base or the scene, it lays the sky's own haze out
// along the clouds' shadow: brighter where the map is clearer than around
// the camera, darker where it is darker (the sky draws that haze already;
// the air around the camera stays as the sky draws it).

#include "llviewerprecompiledheaders.h"

#include "plus.h"

#include "volumetric.h"

#include "presets/presets.h"
#include "atmosphere/atmosphere.h"
#include "skyscreen/skyscreen.h"
#include "volclouds/volclouds.h"

#include "llenvironment.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llrhi.h"
#include "llrhivolumetric.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "pipeline.h"
#include "v3color.h"

#include <glm/gtc/type_ptr.hpp>

#include <cstring>

extern bool gCubeSnapshot;      // llviewerdisplay.cpp: a reflection probe face or mirror is being drawn

namespace
{
    constexpr char SETTING_ENABLED[]  = "PlusVolumetric";
    constexpr char SETTING_QUALITY[]  = "PlusVolumetricQuality";
    constexpr char SETTING_STRENGTH[] = "PlusVolumetricStrength";
    constexpr char SETTING_SAMPLES[]  = "PlusVolumetricSamples";
    // The clouds' shadow in the air (research/253): how many times the sky's own haze the far air's
    // sunbeams take (0: off, also in the near air).
    constexpr char SETTING_CLOUD_SHAFTS[] = "PlusCloudShafts";
    // How the shafts meet the alpha (alpha-sky; diagnostic, not saved).
    // Alpha (OIT's rigged alpha, and all non-rigged alpha even without OIT)
    // writes no depth: added after it, a ray through a leaf or a hair runs on
    // to the sky behind and its air's light covers the leaf.
    //  0: after the alpha (renderFinalize), as first made: no alpha depth, no
    //     edge fallback in the composite.
    //  1: after the alpha, with the rigged alpha's depth written after the OIT
    //     resolve (hair), and the edge fallback.
    //  2: as 1, and all the other alpha's depth too, exactly as upstream's
    //     depth of field pass writes it (non-rigged alpha at 0.33, material and
    //     PBR alpha at 0.004, custom blends at 0; the rigged alpha at 0.33
    //     after the OIT resolve). Costs a depth-only redraw of the alpha.
    //     1 and 2 cut the shafts by a threshold: where an alpha surface wrote
    //     depth the light of the air behind it is gone even through its clear
    //     parts, so nearly clear attachments (alpha 0.004 and up) showed as
    //     flat, tinted shapes and ribbons as blotches (alpha-sky 5). Kept to
    //     compare only.
    //  3 (default): before the above-water alpha (LLDrawPoolAlpha), like the
    //     official haze (doAtmospherics): the alpha blends over the shafts as
    //     over the rest of the scene behind it, with no depth drawn again. Only
    //     the light of the air between the eye and an alpha surface is left out
    //     (at most MAX_MARCH_METRES of thin air; the haze of the alpha's own
    //     shader covers the distance). With the edge fallback. No redraw, no
    //     threshold: each alpha surface hides the light behind it by its own
    //     alpha, as it hides the rest of the scene.
    constexpr char SETTING_ALPHA[] = "PlusVolumetricAlpha";
    constexpr U32 ALPHA_AFTER_PLAIN = 0;
    constexpr U32 ALPHA_BEFORE = 3;

    U32 alphaSetting()
    {
        static LLCachedControl<U32> mode(gSavedSettings, SETTING_ALPHA, ALPHA_BEFORE);
        return llmin((U32)mode, ALPHA_BEFORE);
    }

    // Set when renderBeforeAlpha drew this frame's shafts; render() (renderFinalize) then draws none.
    bool sDrawnBeforeAlpha = false;

    // PlusVolumetricQuality 1..3 (research/84, section 5).
    struct QualityLevel
    {
        U32 scale;      // screen pixels per march pixel, each way
        U32 steps;      // samples along a ray that crosses every shadow cascade
        U32 far_stages; // the far air's stages with the clouds' shadow in the air (research/253 3.3)
    };
    constexpr QualityLevel QUALITY_LEVELS[] =
    {
        { 4, 24, 8 },   // 1 low: a sixteenth of the pixels
        { 2, 32, 12 },  // 2 medium: a quarter
        { 2, 64, 16 },  // 3 high: a quarter, twice the samples
    };
    constexpr U32 FALLBACK_QUALITY = 2;
    constexpr U32 MIN_SAMPLES = 4;      // PlusVolumetricSamples, when not 0 (the quality's)
    constexpr U32 MAX_SAMPLES = 128;
    constexpr F32 MAX_STRENGTH = 10.f;

    // At strength 1, the share of the light a metre of air scatters. Nothing
    // is absorbed: the light coming back to the eye dims by the same amount.
    // (Was 0.002 until alpha-sky: tuned while the passes' uniforms never
    // arrived; once they did, a ray to the sky crossed hundreds of metres of
    // such air and the sky went white, at night the most.)
    constexpr F32 SCATTERING_PER_METRE = 0.0005f;
    // Haze scatters mostly forwards: shafts are brightest towards the light
    // (g 0.5: towards the light six times the light of no preference, 0.6 was ten).
    constexpr F32 ANISOTROPY = 0.5f;
    // The light the shafts take (shadowingLight) stands for what a white surface
    // facing the light sends back, L = E / pi for the light's irradiance E (a
    // classic sky's sun times 1.35, as the scene takes it; a newer sky's linear
    // sun, of which its PBR shading gives a white surface some 1.4 times, so the
    // shafts are on the weak side there). The shader's phase
    // function is 1 for no preference, 4 pi times the one that sums to 1 over the
    // sphere. A metre of air of scattering s then sends s * E * phase / (4 pi) =
    // s * L * phase / 4 towards the eye. Without this quarter the air shone four
    // times as bright as the scene it lies over: a white veil over the sky and
    // everything far, the distant palms flat silhouettes (fix-r9). The ground
    // fog's sun share (plus/atmosphere, SUN_SCATTER_SHARE) is in these units
    // already: its mist is handed over as scattering s = share / PHASE_NORMALISATION.
    constexpr F32 PHASE_NORMALISATION = 0.25f;
    // The sky's light the air scatters towards the eye (its ambient, the light
    // that reaches the air from every side), as the scene takes it (plus::skyAmbient):
    // the air takes as much of the scene behind it out, so that thicker air goes
    // towards the air's light instead of only darkening (fix-r9). Under the "no post"
    // tone mapping its brightest channel is held under 1 (a sky brighter than 1).
    constexpr F32 NO_POST_AIR_CEILING = 1.f;
    // No ray marches farther than this, m (the sky's own haze is drawn by the
    // sky; past this the shafts would be a sheet of light over everything).
    constexpr F32 MAX_MARCH_METRES = 160.f;
    // The moon's light for the shafts, times the light the pipeline gives it
    // (the sun's colour, LLSettingsSky::getMoonlightColor): against a dark
    // night sky any glow shows.
    constexpr F32 MOON_SCALE = 0.4f;
    // Under a classic sky (plus::isClassicSky) the scene is lit by the light's
    // colour as it is (sRGB, not made linear) times 1.35. The shafts take the
    // light the same way, so they keep the balance with the scene they have
    // under the other skies (made linear, the shafts were some 0.17 times as
    // strong at an evening sun of 0.3 and 2.7 times at a sun of 3, fix-round39).
    // Under the "no post" tone mapping (plus::isNoPost: a classic sky, or a newer
    // sky with its reflection probe ambiance at 0) nothing scales the picture down
    // (no exposure, no tone mapping: the colour is clamped to 1), and the shafts'
    // light is no brighter than this (its brightest channel, its hue kept): the
    // light of a sun of colour 1 in that sky, which already shows a white surface
    // white; a brighter sun (up to 3) painted the sky white with the shafts alone.
    // (fix-round43 1)
    constexpr F32 CLASSIC_LIGHT_CEILING = plus::CLASSIC_SUNLIGHT_SCALE;
    constexpr F32 NO_POST_LIGHT_CEILING = 1.f;  // a newer sky: linear, not taken 1.35 times

    // The clouds' shadow in the air (research/253). The far air is the sky's own haze (Windlight's
    // atmospherics: skyF.glsl), as thick as the sky makes it times PlusCloudShafts; where the clouds'
    // shadow map is clearer than around the camera it takes more of the haze's own sun light in its
    // direction, where darker it takes the same share out of the picture (a share, never more than the
    // air's), both in the units the sky is drawn in (skyF's terms) and taken to the screen's light as
    // the haze is (made linear, times sky_hdr_scale): at a pixel of the sky the sky's haze (softenLightF.glsl),
    // at a pixel of the scene the scene's own (atmosphericsFuncs.glsl, as hazeF.glsl lays it over the scene).
    constexpr F32 MAX_CLOUD_SHAFTS = 4.f;
    constexpr F32 MAX_HAZE_EXTINCTION = 0.02f;  // per metre (the backend holds it there too)
    // skyF.glsl: the sky's colour is twice its haze; the moon's light is taken 0.7 times.
    constexpr F32 SKY_HAZE_SCALE = 2.f;
    constexpr F32 SKY_MOONLIGHT_SCALE = 0.7f;
    // The sky dims its sunlight by the air it crosses on the way down to a view ray (skyF's off_axis,
    // 1 / (the ray's up + the light's)): the composite does so for each pixel's ray, the march's shares
    // (its a(theta), one for the frame) for a ray this much up, where the beams are seen (some 12 degrees).
    constexpr F32 BEAM_VIEW_UP = 0.2f;
    // softenLightF.glsl's final_scale under a classic sky (classic_mode: plus::isClassicSky): the sky's
    // colour, made linear and times sky_hdr_scale, goes to the screen this many times.
    constexpr F32 CLASSIC_SKY_FINAL_SCALE = 1.1f;

    constexpr U32 SHADOW_CASCADES = 4;

    struct Settings
    {
        U32 scale = 1;
        U32 steps = 0;
        U32 far_stages = 0;
        F32 scattering = 0.f;   // per metre
        F32 cloud_shafts = 0.f; // PlusCloudShafts (0: no clouds' shadow in the air)
    };

    // False while the feature is off.
    bool readSettings(Settings& out)
    {
        static LLCachedControl<bool> enabled(gSavedSettings, SETTING_ENABLED, false);
        static LLCachedControl<U32>  quality(gSavedSettings, SETTING_QUALITY, FALLBACK_QUALITY);
        static LLCachedControl<F32>  strength(gSavedSettings, SETTING_STRENGTH, 1.f);
        static LLCachedControl<U32>  samples(gSavedSettings, SETTING_SAMPLES, 0);
        static LLCachedControl<F32>  cloud_shafts(gSavedSettings, SETTING_CLOUD_SHAFTS, 0.f);
        // A snapshot with the boost on (plus/presets): on, at the best quality.
        const bool boost = plus::presets::snapshotBoostActive();
        if (!enabled && !boost)
        {
            return false;
        }
        const U32 level_count = (U32)(sizeof(QUALITY_LEVELS) / sizeof(QUALITY_LEVELS[0]));
        const U32 wanted = boost ? plus::presets::SNAPSHOT_BOOST_QUALITY : (U32)quality;
        const QualityLevel& level = QUALITY_LEVELS[llclamp(wanted, 1u, level_count) - 1];
        out.scale = level.scale;
        out.steps = (U32)samples != 0 ? llclamp((U32)samples, MIN_SAMPLES, MAX_SAMPLES) : level.steps;
        if (boost)
        {
            out.steps = llmax(out.steps, level.steps);
        }
        out.far_stages = level.far_stages;
        out.scattering = SCATTERING_PER_METRE * llclamp((F32)strength, 0.f, MAX_STRENGTH);
        const F32 shafts = cloud_shafts;
        out.cloud_shafts = shafts > 0.f ? llmin(shafts, MAX_CLOUD_SHAFTS) : 0.f;   // also a NaN: off
        return out.scattering > 0.f;
    }

    // The march's target. Made on first use and kept while the feature is on;
    // never destroyed by a static destructor, which would run without a GL context.
    LLRenderTarget& scatterTarget()
    {
        static LLRenderTarget* target = new LLRenderTarget();
        return *target;
    }

    // The blur's other half (across into this, then down back into the march's).
    LLRenderTarget& blurTarget()
    {
        static LLRenderTarget* target = new LLRenderTarget();
        return *target;
    }

    // After either target could not be made (VRAM full): not tried again every frame. (fix-round67 4)
    plus::RetryWait sTargetRetry;

    void releaseScatterTarget()
    {
        for (LLRenderTarget* target : { &scatterTarget(), &blurTarget() })
        {
            if (target->isComplete())
            {
                target->release();
            }
        }
    }

    // Both targets at width x height; false when they could not be made (or
    // one could not be a moment ago: sTargetRetry).
    bool sizeScatterTarget(U32 width, U32 height)
    {
        for (LLRenderTarget* target : { &scatterTarget(), &blurTarget() })
        {
            if (target->isComplete() && target->getWidth() == width && target->getHeight() == height)
            {
                continue;
            }
            if (!sTargetRetry.due())
            {
                return false;
            }
            if (!target->allocate(width, height, GL_RGBA16F))
            {
                target->release();  // a half-made target is not isComplete(): releaseScatterTarget would skip it
                sTargetRetry.failed();
                return false;
            }
            sTargetRetry.succeeded();
        }
        return true;
    }

    // Whether the targets are (or may be) there to draw into: false while waiting after a failure.
    bool scatterTargetsUsable()
    {
        return sTargetRetry.due() || (scatterTarget().isComplete() && blurTarget().isComplete());
    }

    // The march's light blurred across into the blur target, then down back
    // into the march's (LLRHI::VolumetricBlur); false when a pass failed (the
    // march's light is then left unblurred or half blurred, still whole).
    bool blurScatter(U32 width, U32 height)
    {
        if (!LLRHI::sVolumetricBlur)
        {
            return false;
        }
        LLRenderTarget& scatter = scatterTarget();
        LLRenderTarget& blurred = blurTarget();
        LLRHI::VolumetricBlur pass;
        pass.width = width;
        pass.height = height;
        pass.source_texture = scatter.getTexture();
        pass.direction[0] = 1;
        pass.direction[1] = 0;
        blurred.bindTarget();
        const bool across = LLRHI::sVolumetricBlur(pass);
        blurred.flush();
        if (!across)
        {
            return false;
        }
        pass.source_texture = blurred.getTexture();
        pass.direction[0] = 0;
        pass.direction[1] = 1;
        scatter.bindTarget();
        const bool down = LLRHI::sVolumetricBlur(pass);
        scatter.flush();
        return down;
    }

    // The backend draws it and the frame has sun shadows, above water.
    bool canDrawThisFrame()
    {
        if (!LLRHI::sVolumetric || !LLRHI::sVolumetricMarch || !LLRHI::sVolumetricComposite)
        {
            return false;
        }
        // Under water the air's light would be the water's (research/84, section 4.9).
        return LLPipeline::RenderShadowDetail >= 1 && !LLViewerCamera::getInstance()->cameraUnderWater();
    }

    // canDrawThisFrame, and the frame has what the passes read: the sun's
    // shadow maps and the scene target, bound on top (screen_bound: drawn
    // into as it is) or not bound at all.
    bool canDraw(bool screen_bound)
    {
        if (!canDrawThisFrame())
        {
            return false;
        }
        const LLPipeline::RenderTargetPack& rt = *gPipeline.mRT;
        const bool bound_right = screen_bound ? LLRenderTarget::getCurrentBoundTarget() == &rt.screen : !rt.screen.isBoundInStack();
        if (!rt.screen.isComplete() || !bound_right || !rt.deferredScreen.getDepth())
        {
            return false;
        }
        for (U32 i = 0; i < SHADOW_CASCADES; ++i)
        {
            if (!rt.shadow[i].getDepth())
            {
                return false;
            }
        }
        return true;
    }

    // The light that cast this frame's shadow maps (LLPipeline::generateSunShadow:
    // the sun while it is up, else the moon), eye space; false when it gives no light.
    bool shadowingLight(LLVector3& direction, LLColor3& color)
    {
        const bool sun = LLEnvironment::instance().getIsSunUp();
        const LLVector4& towards = sun ? gPipeline.mTransformedSunDir : gPipeline.mTransformedMoonDir;
        const LLColor4& diffuse = sun ? gPipeline.mSunDiffuse : gPipeline.mMoonDiffuse;
        direction.set(towards.mV[VX], towards.mV[VY], towards.mV[VZ]);
        const bool classic = plus::isClassicSky();
        if (classic)
        {
            color.set(diffuse.mV[VRED], diffuse.mV[VGREEN], diffuse.mV[VBLUE]);
            color *= plus::CLASSIC_SUNLIGHT_SCALE;
        }
        else
        {
            color = linearColor3(diffuse);
        }
        if (plus::isNoPost())
        {
            const F32 ceiling = classic ? CLASSIC_LIGHT_CEILING : NO_POST_LIGHT_CEILING;
            const F32 brightest = llmax(color.mV[VRED], color.mV[VGREEN], color.mV[VBLUE]);
            if (brightest > ceiling)
            {
                color *= ceiling / brightest;
            }
        }
        if (!sun)
        {
            color *= MOON_SCALE;
        }
        return direction.normalize() > 0.f && (color.mV[VRED] > 0.f || color.mV[VGREEN] > 0.f || color.mV[VBLUE] > 0.f);
    }

    // The sky's light the air scatters towards the eye (NO_POST_AIR_CEILING above).
    LLColor3 airAmbient()
    {
        LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky();
        if (!sky)
        {
            return LLColor3::black;
        }
        LLColor3 ambient = plus::skyAmbient(sky.get(), plus::isClassicSky(sky.get()));
        if (plus::isNoPost(sky.get()))
        {
            const F32 brightest = llmax(ambient.mV[VRED], ambient.mV[VGREEN], ambient.mV[VBLUE]);
            if (brightest > NO_POST_AIR_CEILING)
            {
                ambient *= NO_POST_AIR_CEILING / brightest;
            }
        }
        return ambient;
    }

    // The sky's haze as skyF.glsl draws it, for the light that casts the shadows (sun: whether it is the sun),
    // in the sky's units: its terms, from which the composite works out the sky in each pixel's direction (the
    // light dimmed by the air above the ray; above the clouds as thick as the air up the ray, mixed towards
    // below them near the horizon) and the scene's own haze (the same terms below the clouds, as thick as the
    // air up to the scene: atmosphericsFuncs.glsl), and its thickness.
    struct SkyHaze
    {
        LLColor3 sun;           // 2 x blue_horizon x blue weight x the light, before the air dims it
        LLColor3 around;        // 2 x haze_horizon x haze weight x the light (times the glow), likewise
        LLColor3 ambient;       // 2 x (blue_horizon x blue weight + haze_horizon x haze weight) x the ambient with the clouds' (below them)
        LLColor3 ambient_above; // the same with the ambient as it is (above the clouds)
        LLColor3 light_depth;   // the light's optical depth to a ray (skyF's light_atten): dimmed by exp(-this / (the ray's up + the light's))
        LLColor3 air_depth;     // the air's optical depth straight up to max_y (skyF's combined haze x density_multiplier x max_y)
        F32 drawn = 1.f;        // the share of the light the sky draws below the clouds (1 - cloud_shadow)
        F32 glow[3] = {};       // glow.x, glow.z, the glow's factor (0: none, the moon)
        F32 extinction = 0.f;   // per metre: the atmospherics' (blue_density + haze_density) x density x distance multipliers
        LLColor3 scene_extinction;  // per metre, each channel: atmosphericsFuncs.glsl's combined_haze x density x distance
                                    // multipliers (the scene's own haze)
        LLColor3 scene_ambient;     // ambient's term below the clouds for the scene's own haze: atmosphericsFuncs.glsl's
                                    // tmpAmbient has no max(0, ...) (skyF.glsl's has): an ambient above 1 goes down there
    };

    SkyHaze skyHaze(const LLSettingsSky& sky, bool sun)
    {
        SkyHaze haze;
        const LLColor3 blue_density = sky.getBlueDensity();
        const F32 haze_density = sky.getHazeDensity();
        const F32 density_multiplier = sky.getDensityMultiplier();
        const LLColor3 light = sun ? sky.getSunlightColor() : sky.getMoonlightColor() * SKY_MOONLIGHT_SCALE;
        const F32 max_y = sky.getMaxY();
        const LLColor3 blue_horizon = sky.getBlueHorizon();
        const F32 haze_horizon = sky.getHazeHorizon();
        const LLColor4 total_ambient = sky.getTotalAmbient();
        const F32 cloud_shadow = sky.getCloudShadow();
        const F32 distance_multiplier = sky.getDistanceMultiplier();
        for (U32 c = 0; c < 3; ++c)
        {
            const F32 combined = llmax(fabsf(blue_density.mV[c]) + fabsf(haze_density), 1e-6f);
            const F32 blue = blue_horizon.mV[c] * blue_density.mV[c] / combined;
            const F32 around = haze_horizon * haze_density / combined;
            const F32 ambient = total_ambient.mV[c] + llmax(0.f, 1.f - total_ambient.mV[c]) * cloud_shadow * 0.5f;
            haze.sun.mV[c] = SKY_HAZE_SCALE * blue * light.mV[c];
            haze.around.mV[c] = SKY_HAZE_SCALE * around * light.mV[c];
            haze.ambient.mV[c] = SKY_HAZE_SCALE * (blue + around) * ambient;
            haze.ambient_above.mV[c] = SKY_HAZE_SCALE * (blue + around) * total_ambient.mV[c];
            haze.scene_ambient.mV[c] = SKY_HAZE_SCALE * (blue + around) * (total_ambient.mV[c] + (1.f - total_ambient.mV[c]) * cloud_shadow * 0.5f);
            haze.light_depth.mV[c] = llmax((blue_density.mV[c] + haze_density * 0.25f) * density_multiplier * max_y, 0.f);
            haze.air_depth.mV[c] = llmax(combined * density_multiplier * max_y, 0.f);
            haze.scene_extinction.mV[c] = llmax(llmax(blue_density.mV[c] + haze_density, 1e-6f) * density_multiplier * distance_multiplier, 0.f);
        }
        haze.drawn = llmax(0.f, 1.f - cloud_shadow);    // skyF.glsl dims the light below the clouds by their cover
        const LLColor3 glow = sky.getGlow();
        const F32 glow_factor = sky.getSunMoonGlowFactor();
        haze.glow[0] = glow.mV[0];
        haze.glow[1] = glow.mV[2];
        haze.glow[2] = glow_factor >= 1.f ? glow_factor : 0.f;     // skyF: none for the moon
        const F32 mean_blue = (blue_density.mV[0] + blue_density.mV[1] + blue_density.mV[2]) / 3.f;
        haze.extinction = (mean_blue + haze_density) * density_multiplier * distance_multiplier;
        return haze;
    }

    // The clouds' shadow in the air (research/253): march.clouds (modelview: the view's, agent -> eye) and
    // the composite's light for the march's shares (light). Left off (march.clouds' strength 0) without it:
    // PlusCloudShafts 0, no clouds' shadow map in this view (plus::volclouds::airShadow), no sky. The far air
    // only before_alpha (renderBeforeAlpha, PlusVolumetricAlpha 3): drawn after the alpha (renderFinalize, also
    // when the pass before the alpha could not draw), the alpha surfaces that write no depth would take the
    // sky's shadows too.
    void cloudsInAir(const Settings& settings, const glm::mat4& modelview, bool before_alpha, LLRHI::VolumetricMarch& march,
                     LLRHI::VolumetricCloudsLight& light)
    {
        if (settings.cloud_shafts <= 0.f)
        {
            return;
        }
        plus::volclouds::AirShadow shadow;
        LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky();
        if (!sky || !plus::volclouds::airShadow(shadow) || !shadow.map || shadow.to_light[2] <= 0.f)
        {
            return;
        }
        LLRHI::VolumetricClouds& clouds = march.clouds;
        clouds.map_texture = shadow.map->getTexture();
        clouds.strength = shadow.strength * shadow.fade;
        for (U32 axis = 0; axis < 3; ++axis)    // east, north, up: the modelview's columns
        {
            for (U32 c = 0; c < 3; ++c)
            {
                clouds.axes[axis][c] = modelview[axis][c];
            }
        }
        clouds.inverse_width = shadow.inverse_width;
        clouds.layer_base = shadow.layer_base;
        clouds.layer_middle = shadow.layer_middle;
        for (U32 c = 0; c < 2; ++c)
        {
            clouds.light_slope[c] = shadow.to_light[c] / shadow.to_light[2];
            clouds.camera_place[c] = (clouds.light_slope[c] * shadow.layer_middle - shadow.centre[c]) * shadow.inverse_width + 0.5f;
        }

        const bool sun = LLEnvironment::instance().getIsSunUp();    // as shadowingLight and the map's light
        const SkyHaze haze = skyHaze(*sky, sun);
        // The far air, only before the alpha and with the map's mips (its stages read them).
        const F32 extinction = haze.extinction * settings.cloud_shafts;
        if (before_alpha && shadow.mean_ready && extinction > 0.f)
        {
            clouds.far_extinction = llmin(extinction, MAX_HAZE_EXTINCTION);
            clouds.far_stages = settings.far_stages;
            // The air around the camera: the mip whose texel spans the far air's first half of its light.
            const F32 texels = F_LN2 / clouds.far_extinction * shadow.inverse_width * (F32)shadow.map->getWidth();
            clouds.reference_lod = llclamp(log2f(llmax(texels, 1.f)), 0.f, shadow.mean_lod);
        }
        // The march's shares (a(theta)): the light dimmed as skyF.glsl dims it for a ray BEAM_VIEW_UP up, and the
        // sun's share as much as the sky draws of it below the clouds (their cover dims it), so that the darker
        // share never takes out more of the sun's light than the haze shows (research/253 review 2 low 1).
        // For a ray to the scene, the scene's own haze's (review 4 middle 1): the light dimmed for lightnorm.y (the
        // scene's haze's glow and its light's up as hazeF.glsl has them: LLPipeline::renderDeferredLighting), tmpAmbient.
        const LLVector4 lightnorm = LLEnvironment::instance().getClampedLightNorm();
        const F32 off_axis = 1.f / llmax(1e-6f, BEAM_VIEW_UP + shadow.to_light[2]);
        const F32 scene_off_axis = 1.f / llmax(1e-6f, lightnorm.mV[VY]);
        LLColor3 dimmed, scene_dimmed;
        for (U32 c = 0; c < 3; ++c)
        {
            dimmed.mV[c] = expf(-haze.light_depth.mV[c] * off_axis);
            scene_dimmed.mV[c] = expf(-haze.light_depth.mV[c] * scene_off_axis);
        }
        clouds.haze_shares[0] = plus::luminance(haze.ambient);
        clouds.haze_shares[1] = plus::luminance(haze.sun * dimmed) * haze.drawn;
        clouds.haze_shares[2] = plus::luminance(haze.around * dimmed) * haze.drawn;
        clouds.scene_shares[0] = plus::luminance(haze.scene_ambient);
        clouds.scene_shares[1] = plus::luminance(haze.sun * scene_dimmed) * haze.drawn;
        clouds.scene_shares[2] = plus::luminance(haze.around * scene_dimmed) * haze.drawn;
        for (U32 i = 0; i < 3; ++i)
        {
            clouds.scene_lightnorm[i] = lightnorm.mV[i];
            clouds.glow[i] = haze.glow[i];
            light.glow[i] = haze.glow[i];
            light.haze_sun[i] = haze.sun.mV[i];
            light.haze_glow[i] = haze.around.mV[i];
            light.haze_ambient[i] = haze.ambient.mV[i];
            light.haze_above[i] = haze.ambient_above.mV[i];
            light.light_depth[i] = haze.light_depth.mV[i];
            light.air_depth[i] = haze.air_depth.mV[i];
            light.up[i] = clouds.axes[2][i];
            light.light_color[i] = march.light_color[i];
            light.light_direction[i] = march.light_direction[i];
            light.scene_extinction[i] = haze.scene_extinction.mV[i];
            light.scene_ambient[i] = haze.scene_ambient.mV[i];
            light.scene_lightnorm[i] = lightnorm.mV[i];
        }
        light.haze_drawn = haze.drawn;
        light.light_up = shadow.to_light[2];
        // The scene's haze to the screen's light as hazeF.glsl takes it (plus/skyscreen's sky_hdr_scale), the sky's
        // as softenLightF.glsl does (times 1.1 under a classic sky).
        light.scene_scale = plus::skyscreen::current(sky.get()).hdr_scale;
        light.sky_scale = light.scene_scale * (plus::isClassicSky(sky.get()) ? CLASSIC_SKY_FINAL_SCALE : 1.f);
        // The haze is lit at most as skyF.glsl lets the sky be bright or, under the "no post" tone mapping (the
        // screen clamped to 1), to white: past that its light would only be clamped away.
        light.far_ceiling = plus::isNoPost(sky.get()) ? llmin(linearTosRGB(1.f / light.sky_scale), LLRHI::SKY_COLOUR_MAX)
                                                      : LLRHI::SKY_COLOUR_MAX;
    }

    LLRHI::VolumetricMarch marchPass(const Settings& settings, const glm::mat4& modelview, const glm::mat4& projection,
                                     const LLVector3& light_direction, const LLColor3& light_color)
    {
        const LLPipeline::RenderTargetPack& rt = *gPipeline.mRT;
        LLRHI::VolumetricMarch pass;
        pass.depth_texture = rt.deferredScreen.getDepth();
        pass.depth_width = rt.deferredScreen.getWidth();
        pass.depth_height = rt.deferredScreen.getHeight();
        pass.scale = settings.scale;
        for (U32 i = 0; i < SHADOW_CASCADES; ++i)
        {
            pass.shadow_textures[i] = rt.shadow[i].getDepth();
            memcpy(pass.shadow_matrices[i], glm::value_ptr(gPipeline.mSunShadowMatrix[i]), sizeof(pass.shadow_matrices[i]));
            pass.shadow_far[i] = gPipeline.mSunClipPlanes.mV[i];
        }
        memcpy(pass.inverse_projection, glm::value_ptr(glm::inverse(projection)), sizeof(pass.inverse_projection));
        for (U32 c = 0; c < 3; ++c)
        {
            pass.light_direction[c] = light_direction.mV[c];
            pass.light_color[c] = light_color.mV[c] * PHASE_NORMALISATION;
        }
        // Nothing is absorbed: the air scatters all it takes out of the light.
        pass.extinction = settings.scattering;
        pass.anisotropy = ANISOTROPY;
        pass.steps = settings.steps;
        pass.max_length = MAX_MARCH_METRES;
        // The ground fog (plus/atmosphere) is mist for the shafts to show in. The base air's
        // scattering and extinction are the setting's (settings.scattering).
        plus::atmosphere::ShaftMist mist;
        if (settings.scattering > 0.f && plus::atmosphere::shaftMist(modelview, mist))
        {
            memcpy(pass.up_eye, mist.up_eye, sizeof(pass.up_eye));
            pass.mist_camera_height = mist.camera_height;
            pass.mist_inverse_height = mist.inverse_height;
            pass.mist_scatter = mist.scatter / (PHASE_NORMALISATION * settings.scattering);
            pass.mist_extinction = mist.extinction;
        }
        return pass;
    }

    // Settings: app_settings/settings_plus.xml (PlusVolumetric*), set in the photo tools
    // (app_settings/plus/phototools/catalog.xml, tab "light").
    // Drawn from the <plus> hook in LLDrawPoolAlpha::renderPostDeferred()
    // (PlusVolumetricAlpha 3, the default) or in LLPipeline::renderFinalize().
    plus::FeatureRegistration sRegistration({ "volumetric", nullptr, {} });
}

namespace
{
    // Both passes; true when drawn. screen_bound: mRT->screen is the bound
    // target (drawn into as it is, and bound again after the march's target).
    // before_alpha: drawn from renderBeforeAlpha (cloudsInAir's far air).
    bool draw(const Settings& settings, const glm::mat4& modelview, const glm::mat4& projection, bool screen_bound,
              bool before_alpha)
    {
        LLVector3 light_direction;
        LLColor3 light_color;
        if (!shadowingLight(light_direction, light_color))
        {
            return false;   // night without moonlight: keep the target, it is wanted again at dawn
        }
        LLRenderTarget& screen = gPipeline.mRT->screen;
        const U32 width = (screen.getWidth() + settings.scale - 1) / settings.scale;
        const U32 height = (screen.getHeight() + settings.scale - 1) / settings.scale;
        if (!sizeScatterTarget(width, height))
        {
            return false;
        }

        LLRHI::VolumetricMarch march = marchPass(settings, modelview, projection, light_direction, light_color);
        LLRHI::VolumetricComposite composite;
        cloudsInAir(settings, modelview, before_alpha, march, composite.clouds);

        gGL.flush();                    // what was drawn into the bound target before
        LLRHI::gpuMark("volumetric");   // the GPU ms line's segment, until the caller's next mark
        LLRenderTarget& scatter = scatterTarget();
        scatter.bindTarget();
        const bool marched = LLRHI::sVolumetricMarch(march);
        scatter.flush();                // with screen_bound, mRT->screen is bound again
        if (!marched)
        {
            return false;
        }
        // Smooths the march's dither and the shadow texels before the upsample
        // (without it they showed as grain and blocks in the haze, alpha-sky 6).
        blurScatter(width, height);

        composite.scatter_texture = scatter.getTexture();
        composite.scatter_width = width;
        composite.scatter_height = height;
        composite.scale = settings.scale;
        composite.depth_texture = gPipeline.mRT->deferredScreen.getDepth();
        composite.depth_a = projection[2][2];
        composite.depth_b = projection[3][2];
        // The march's far end for the edge fallback; 0 (PlusVolumetricAlpha 0) leaves it out.
        composite.march_far = alphaSetting() != ALPHA_AFTER_PLAIN ? llmin(gPipeline.mSunClipPlanes.mV[SHADOW_CASCADES - 1], MAX_MARCH_METRES) : 0.f;
        // The air the march crossed, for how much of the scene behind it comes through (as the march's
        // rays: to the scene, the last cascade's end, MAX_MARCH_METRES, whichever is nearest).
        composite.ray_scale[0] = 1.f / projection[0][0];
        composite.ray_scale[1] = 1.f / projection[1][1];
        composite.ray_offset[0] = projection[2][0];
        composite.ray_offset[1] = projection[2][1];
        composite.shadow_end = gPipeline.mSunClipPlanes.mV[SHADOW_CASCADES - 1];
        composite.max_length = MAX_MARCH_METRES;
        composite.extinction = settings.scattering;
        const LLColor3 ambient = airAmbient();
        for (U32 c = 0; c < 3; ++c)
        {
            composite.air_ambient[c] = ambient.mV[c];
        }
        if (screen_bound)
        {
            return LLRHI::sVolumetricComposite(composite);
        }
        screen.bindTarget();
        const bool composited = LLRHI::sVolumetricComposite(composite);
        screen.flush();
        return composited;
    }
}

namespace plus::volumetric
{
    U32 alphaDepth()
    {
        const U32 mode = alphaSetting();
        if (mode == ALPHA_AFTER_PLAIN || mode == ALPHA_BEFORE)
        {
            return 0;
        }
        Settings settings;
        LLVector3 light_direction;
        LLColor3 light_color;
        return readSettings(settings) && canDrawThisFrame() && shadowingLight(light_direction, light_color) ? mode : 0;
    }

    bool drawsShafts()
    {
        // As draw() decides: also a light that gives light (not a night without moonlight) and
        // targets to march into (not while waiting after VRAM ran out), or the fog would keep only
        // its own share of the glow with no shafts to add the rest. (fix-round67 7c)
        Settings settings;
        LLVector3 light_direction;
        LLColor3 light_color;
        return readSettings(settings) && canDrawThisFrame() && scatterTargetsUsable() && shadowingLight(light_direction, light_color);
    }

    void renderBeforeAlpha()
    {
        // The main view only, as render() (renderFinalize, never run for a probe
        // face or a mirror) and the fog before the alpha: no shafts in a probe
        // face or avatarrefl's pass, and sDrawnBeforeAlpha not set by one.
        if (gCubeSnapshot || gPipeline.mRT != &gPipeline.mMainRT)
        {
            return;
        }
        Settings settings;
        if (alphaSetting() != ALPHA_BEFORE || !readSettings(settings) || !canDraw(true))
        {
            return;     // render() releases the target when the feature is off
        }
        // The camera's projection, loaded for this frame's scene (the "last" one
        // is kept only at the end of renderDeferredLighting, after the alpha).
        if (draw(settings, glm::make_mat4(gGLModelView), get_current_projection(), true, true))
        {
            sDrawnBeforeAlpha = true;
        }
        LLRHI::gpuMark("alpha");    // the rest of the pool goes back to its segment
    }

    void render()
    {
        if (sDrawnBeforeAlpha)
        {
            sDrawnBeforeAlpha = false;  // this frame's are in the scene already
            return;
        }
        Settings settings;
        if (!readSettings(settings) || !canDraw(false))
        {
            releaseScatterTarget();
            return;
        }
        // The 3D scene's projection (renderDeferredLighting keeps it; HUDs may have loaded theirs since).
        draw(settings, get_last_modelview(), get_last_projection(), false, false);
    }
}

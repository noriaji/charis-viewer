/**
 * @file volclouds.cpp
 * @brief The volumetric clouds, an option next to the 2D clouds (research/186).
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

#include "llviewerprecompiledheaders.h"

#include "plus.h"

#include "volclouds.h"

#include "llenvironment.h"
#include "llframetimer.h"
#include "llglslshader.h"
#include "llpassscope.h"
#include "llrendertarget.h"
#include "llrhi.h"
#include "llsettingssky.h"
#include "llshadermgr.h"
#include "llsky.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewershadermgr.h"
#include "llviewertexture.h"
#include "llvosky.h"
#include "pipeline.h"

extern bool gCubeSnapshot;      // llviewerdisplay.cpp: a reflection probe face or mirror is being drawn

namespace
{
    constexpr char SETTING_ON[] = "PlusVolumetricClouds";
    constexpr char SETTING_THICKNESS[] = "PlusCloudThickness";
    constexpr char SETTING_DETAIL[] = "PlusCloudDetail";
    constexpr char SETTING_SHADE[] = "PlusCloudShade";
    constexpr char SETTING_GRAIN[] = "PlusCloudGrain";
    constexpr char SETTING_STEPS[] = "PlusCloudSteps";
    constexpr char SETTING_SHADOW[] = "PlusCloudShadow";
    constexpr char SETTING_SHADOW_RADIUS[] = "PlusCloudShadowRadius";

    constexpr U32 MAP_SIZE = 256;           // the shadow map's width and height (R8, with mips)
    constexpr U32 MEAN_LEVEL = 8;           // the map's last mip level, one texel: its mean (research/253 3.4)
    static_assert((1u << MEAN_LEVEL) == MAP_SIZE, "the mean's level is the map's last");
    constexpr F64 REMAKE_AFTER_SECONDS = 0.5;   // the sky changes (its blend): the map is made again at least this often
    constexpr F32 MIN_RADIUS = 2000.f;
    constexpr F32 MAX_RADIUS = 12000.f;
    // The middle of the layer's thickness, as a share of it: where the way to the sun is cut for the map.
    constexpr F32 LAYER_MIDDLE = 0.4f;
    // The light's height (the sine of it) over which its clouds' shadow fades in, 2.3 to 7 degrees: lower,
    // the shadows fall too far and too long (research/253 3.6; the map is centred towards the light, so
    // that a low sun's shadows near the camera are still on it).
    constexpr F32 LOW_LIGHT_FADE_FROM = 0.04f;
    constexpr F32 LOW_LIGHT_FADE_TO = 0.12f;
    // How far the sun sets (the sine of it below the horizon, 2.9 degrees) while the moon's shadow comes in.
    constexpr F32 SUN_SET_FADE = 0.05f;

    constexpr F32 MIN_THICKNESS = 0.1f;
    constexpr F32 MAX_THICKNESS = 0.8f;
    constexpr S32 MIN_STEPS = 8;
    constexpr S32 MAX_STEPS = 32;       // the shader's loop
    constexpr F32 MAX_DETAIL = 1.5f;
    constexpr F32 MAX_SHADE = 2.f;
    // The shaders' square root takes 1 - offset^2: the camera must be inside the dome
    constexpr F32 MAX_DOME_OFFSET = 0.99f;

    const LLStaticHashedString sParams("plus_cloud_params");
    const LLStaticHashedString sShape("plus_cloud_shape");
    const LLStaticHashedString sAlt("plus_cloud_alt");
    const LLStaticHashedString sMapRadius("plus_cloud_map_radius");
    const LLStaticHashedString sMapCentre("plus_cloud_map_centre");
    const LLStaticHashedString sShadow("plus_cloud_shadow");
    const LLStaticHashedString sLight("plus_cloud_light");
    const LLStaticHashedString sViewX("plus_cloud_view_x");
    const LLStaticHashedString sViewY("plus_cloud_view_y");
    const LLStaticHashedString sViewZ("plus_cloud_view_z");

    // What a map is made for: it is made again when any of it changes.
    struct MapKey
    {
        F32 max_y = 0.f;
        F32 cloud_scale = 0.f;
        F32 thickness = 0.f;
        F32 radius = 0.f;
        F32 centre[2] = {};     // where it is centred, east and north of the camera (m): mapCentre
        F32 flow[2] = {};       // how far the clouds have flowed (LLEnvironment's cloud scroll, uv)

        bool operator==(const MapKey& other) const { return !memcmp(this, &other, sizeof(MapKey)); }
    };
    static_assert(sizeof(MapKey) == 8 * sizeof(F32), "MapKey is compared as its bytes");

    // The shadow map and what it was made for.
    LLRenderTarget* sMap = nullptr;
    bool sMapValid = false;
    bool sMapBound = false;         // bindShadow bound it (unbindShadow lets it go)
    F64 sMadeAt = 0.0;
    MapKey sMadeKey;

    F32 shadowStrength()
    {
        static LLCachedControl<F32> strength(gSavedSettings, SETTING_SHADOW, 0.7f);
        const F32 value = strength;
        return value > 0.f ? llmin(value, 1.f) : 0.f;   // also a NaN: none
    }

    F32 shadowRadius()
    {
        static LLCachedControl<F32> radius(gSavedSettings, SETTING_SHADOW_RADIUS, 6000.f);
        return llclamp((F32)radius, MIN_RADIUS, MAX_RADIUS);
    }

    F32 thicknessShare()
    {
        static LLCachedControl<F32> thickness(gSavedSettings, SETTING_THICKNESS, 0.2f);
        return llclamp((F32)thickness, MIN_THICKNESS, MAX_THICKNESS);
    }

    F32 detailRelief()
    {
        static LLCachedControl<F32> detail(gSavedSettings, SETTING_DETAIL, 0.6f);
        const F32 value = detail;
        return value > 0.f ? llmin(value, MAX_DETAIL) : 0.f;   // also a NaN: none
    }

    // How much of the 2D clouds' fine pattern shapes the tops, 0 to 1 (cloudsF.glsl): 0 the large shape
    // alone (round tops), 1 all of it (a sky of small lumps)
    F32 grain()
    {
        static LLCachedControl<F32> value(gSavedSettings, SETTING_GRAIN, 0.f);
        const F32 grain = value;
        return grain > 0.f ? llmin(grain, 1.f) : 0.f;   // also a NaN: none
    }

    // How much a cloud's top is lit above the 2D clouds' light (cloudsF.glsl)
    F32 shading()
    {
        static LLCachedControl<F32> shade(gSavedSettings, SETTING_SHADE, 1.f);
        const F32 value = shade;
        return value > 0.f ? llmin(value, MAX_SHADE) : 0.f;   // also a NaN: none
    }

    // The camera's height in the 2D clouds' dome, a share of its radius (renderDome's camHeightLocal):
    // the shaders give each place on the layer the uv and the altitude blend the 2D clouds have in its
    // direction (with the dome's radius, getDomeRadius, the units of the dome's vertices).
    F32 domeOffset(const LLSettingsSky& sky)
    {
        return llclamp(sky.getDomeOffset(), 0.f, MAX_DOME_OFFSET);
    }

    // The frame bind() drew the volumetric clouds in: the shadow follows what is drawn (not drawn with an
    // HDRI sky, clouds hidden, a camera above the layer: the 2D program or nothing is drawn then).
    U64 sDrawnFrame = ~(U64)0;

    // The setting is on (the map is kept while it is).
    bool shadowSetting()
    {
        static LLCachedControl<bool> on(gSavedSettings, SETTING_ON, false);
        return on && shadowStrength() > 0.f;
    }

    // The map is wanted in this view of this frame: the volumetric clouds were drawn in it, the shadow is
    // on, and the sun light pass that reads it runs (LLPipeline::renderDeferredLighting's condition in the
    // main view; the volumetric light reads it too, and it needs the sun's shadows).
    bool shadowWanted()
    {
        const bool sun_pass = LLPipeline::RenderDeferredSSAO || LLPipeline::RenderShadowDetail > 0;
        return shadowSetting() && sun_pass && sDrawnFrame == LLFrameTimer::getFrameCount() && !gCubeSnapshot && !LLPipeline::sReflectionRender;
    }

    // GLSL's smoothstep (from < to; a NaN: 0).
    F32 smoothStep(F32 from, F32 to, F32 x)
    {
        if (!(x > from))
        {
            return 0.f;
        }
        const F32 t = llmin((x - from) / (to - from), 1.f);
        return t * t * (3.f - 2.f * t);
    }

    // How much of the shadow a light this high (to_light's z) casts: the low light's fade.
    F32 lowLightFade(F32 up)
    {
        return smoothStep(LOW_LIGHT_FADE_FROM, LOW_LIGHT_FADE_TO, up);
    }

    // The light the clouds' shadow is cast from, in the agent's axes (east, north, up): the sun, the moon
    // while the sun is down, as the sun's shadow maps take it (LLPipeline::generateSunShadow); and how much
    // of the shadow it casts (0: none).
    struct ShadowLight
    {
        LLVector3 to_light;     // unit
        F32 fade = 0.f;         // lowLightFade, for the moon times its fade in at sunset
    };
    ShadowLight shadowLight()
    {
        const LLEnvironment& environment = LLEnvironment::instance();
        const bool sun = environment.getIsSunUp();
        ShadowLight light;
        light.to_light = sun ? environment.getSunDirection() : environment.getMoonDirection();
        light.to_light.normVec();
        light.fade = lowLightFade(light.to_light.mV[VZ]);
        if (!sun && light.fade > 0.f)
        {
            // The light turns to the moon in the frame the sun goes below the horizon (getIsSunUp): its
            // shadow comes in while the sun sets SUN_SET_FADE further, not all at once (km of dark ground).
            LLVector3 to_sun = environment.getSunDirection();
            to_sun.normVec();
            light.fade *= smoothStep(0.f, SUN_SET_FADE, -to_sun.mV[VZ]);
        }
        return light;
    }

    // The world altitude of the layer's base (m): the height the 2D clouds stand for, the sky's max_y
    // (at least 100). Its top is thicknessShare() of it higher.
    F32 layerBase(const LLSettingsSky& sky)
    {
        return llmax(sky.getMaxY(), 100.f);
    }

    // The world altitude of the layer's middle (m), over a base (layerBase).
    F32 layerMiddle(F32 base)
    {
        return base + LAYER_MIDDLE * thicknessShare() * base;
    }

    // Where the map is centred, east and north of the camera (research/253 3.6): where the way from the
    // camera to the light crosses the layer's middle (rise metres above it). The places around the camera
    // read the map there (with the sun 20 degrees up, 4.7 km off, and 9.7 km at 10 degrees: off a map
    // centred on the camera). On a whole number of texels, so that while the centre moves the texels stay
    // where they were on the clouds (which go with the camera) and the shadows' rims do not shimmer.
    void mapCentre(const LLVector3& to_light, F32 rise, F32 texel, F32 centre[2])
    {
        const F32 along = llmax(rise, 0.f) / llmax(to_light.mV[VZ], LOW_LIGHT_FADE_FROM);
        centre[0] = texel * (F32)ll_round(to_light.mV[VX] * along / texel);
        centre[1] = texel * (F32)ll_round(to_light.mV[VY] * along / texel);
    }

    // The map is made and wanted in this view of this frame. Looked at before the light (shadowLight), so
    // that nothing more is read while the shadow is off; it is read where the light casts it too (fade).
    bool mapWanted()
    {
        return sMap && sMapValid && shadowWanted();
    }
}

namespace plus::volclouds
{
    bool wanted()
    {
        static LLCachedControl<bool> on(gSavedSettings, SETTING_ON, false);
        if (!on || gCubeSnapshot || LLPipeline::sReflectionRender)
        {
            return false;
        }
        // A camera above the layer sees none of it (the dome is the upper half): the 2D clouds then
        LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky();
        if (sky)
        {
            const F32 top = layerBase(*sky) * (1.f + thicknessShare());
            if (LLViewerCamera::getInstance()->getOrigin().mV[VZ] >= top)
            {
                return false;
            }
        }
        return true;
    }

    void bind(LLGLSLShader& shader, const LLSettingsSky& sky)
    {
        static LLCachedControl<S32> steps(gSavedSettings, SETTING_STEPS, 24);

        const F32 max_y = layerBase(sky);
        sDrawnFrame = LLFrameTimer::getFrameCount();

        // The clouds stand where the 2D clouds are, around the camera (research/244): the shader takes
        // each place's uv from its offset to the camera, the layer's height and the dome's offset.
        const LLVector3 camera = LLViewerCamera::getInstance()->getOrigin();
        const F32 slab = thicknessShare() * max_y;
        const S32 march = llclamp((S32)steps, MIN_STEPS, MAX_STEPS);
        shader.uniform4f(sParams, slab, (F32)march, grain(), max_y);
        shader.uniform4f(sShape, domeOffset(sky), detailRelief(), sky.getDomeRadius(), shading());
        shader.uniform1f(sAlt, camera.mV[VZ]);
    }

    void releaseTargets()
    {
        if (sMap)
        {
            sMap->release();
            delete sMap;
            sMap = nullptr;
        }
        sMapValid = false;
        sMapBound = false;
    }

    void updateShadowMap()
    {
        if (!shadowSetting())
        {
            releaseTargets();
            return;
        }
        if (!shadowWanted())
        {
            return;     // a probe's, a mirror's or a frame without the volumetric clouds: the map stays as it is
        }
        LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky();
        if (!sky || !gSky.mVOSkyp)
        {
            return;
        }
        LLViewerTexture* noise = gSky.mVOSkyp->getCloudNoiseTex();
        LLViewerTexture* noise_next = gSky.mVOSkyp->getCloudNoiseTexNext();
        if (!noise && !noise_next)
        {
            return;                     // the 2D clouds draw nothing then either
        }

        const ShadowLight light = shadowLight();
        if (!(light.fade > 0.f))
        {
            return;                     // a light at the horizon or below casts none: the map is not read
        }
        const LLVector3& to_light = light.to_light;

        MapKey key;
        key.max_y = layerBase(*sky);
        key.cloud_scale = llmax(sky->getCloudScale(), 0.001f);
        key.thickness = thicknessShare();
        key.radius = shadowRadius();
        const F32 rise = layerMiddle(key.max_y) - LLViewerCamera::getInstance()->getOrigin().mV[VZ];
        mapCentre(to_light, rise, 2.f * key.radius / MAP_SIZE, key.centre);
        const LLVector2 flow = LLEnvironment::instance().getCloudScrollDelta();
        key.flow[0] = flow.mV[VX];
        key.flow[1] = flow.mV[VY];

        // The clouds are placed around the camera as the 2D clouds are, and so is the map: it does not
        // change when the camera moves. It is made again in every frame the clouds flow in (made every
        // half second, it moved on in steps: research/253 3.6), when it is centred elsewhere (the light or
        // the camera's height moved) and every half second for the sky's own changes.
        const F64 now = LLTimer::getTotalSeconds();
        if (sMap && sMapValid && key == sMadeKey && now - sMadeAt < REMAKE_AFTER_SECONDS)
        {
            return;
        }

        if (!sMap)
        {
            sMap = new LLRenderTarget();
            // with mips (made in flush): the last is the map's mean, for the volumetric light (airShadow)
            if (!sMap->allocate(MAP_SIZE, MAP_SIZE, GL_R8, false, LLTexUnit::TT_TEXTURE, LLTexUnit::TMG_AUTO))
            {
                releaseTargets();
                return;
            }
        }

        LLRHI::gpuMark("cloud map");    // its own segment in the GPU time (VKPerf), until the sun pass's

        // The noise images as renderSkyCloudsDeferred binds them
        F32 blend = (F32)sky->getBlendFactor();
        if (sky->getCloudScrollRate().isExactlyZero())
        {
            blend = 0.f;
        }
        LLViewerTexture* image_a = noise;
        LLViewerTexture* image_b = nullptr;
        if (noise && (!noise_next || noise == noise_next))
        {
            blend = 0.f;
        }
        else if (noise_next && !noise)
        {
            image_a = noise_next;
            blend = 0.f;
        }
        else
        {
            image_b = noise_next;
        }

        sMap->bindTarget();
        const LLPassScope native_pass("cloud shadow map", sMap, { LLPassScope::texture(image_a->getTexName()), LLPassScope::texture(image_b ? image_b->getTexName() : 0) });
        LLGLDepthTest depth(GL_FALSE, GL_FALSE);
        LLGLDisable blend_off(GL_BLEND);

        LLGLSLShader& program = gCloudColumnMapProgram;
        program.bind();
        program.bindTexture(LLShaderMgr::CLOUD_NOISE_MAP, image_a, LLTexUnit::TT_TEXTURE);
        if (image_b)
        {
            program.bindTexture(LLShaderMgr::CLOUD_NOISE_MAP_NEXT, image_b, LLTexUnit::TT_TEXTURE);
        }
        program.uniform1f(LLShaderMgr::BLEND_FACTOR, blend);
        program.uniform1f(LLShaderMgr::CLOUD_VARIANCE, (F32)sky->getCloudVariance());

        // The map's square around its centre (cloudShadowMapF.glsl)
        program.uniform4f(sParams, key.thickness * key.max_y, 0.f, 0.f, key.max_y);
        program.uniform3f(sShape, domeOffset(*sky), 0.f, sky->getDomeRadius());
        program.uniform1f(sMapRadius, key.radius);
        program.uniform2f(sMapCentre, key.centre[0], key.centre[1]);

        gPipeline.mScreenTriangleVB->setBuffer();
        gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
        sMap->flush();                  // and its mips
        program.unbind();

        sMapValid = true;
        sMadeAt = now;
        sMadeKey = key;
    }

    void bindShadow(LLGLSLShader& shader)
    {
        sMapBound = false;
        AirShadow air;
        if (!airShadow(air))
        {
            shader.uniform4f(sShadow, 0.f, 0.f, 0.f, 0.f);
            return;
        }
        // sunLightF.glsl's plusCloudShadow: what AirShadow describes, the light's fade in the strength
        shader.uniform4f(sShadow, air.strength * air.fade, air.inverse_width, air.layer_middle, 0.f);
        shader.uniform2f(sLight, air.to_light[0] / air.to_light[2], air.to_light[1] / air.to_light[2]);
        shader.uniform2f(sMapCentre, air.centre[0], air.centre[1]);

        // The view space's x is to the right, y up and z backwards; in the agent's east, north, up:
        LLViewerCamera* camera = LLViewerCamera::getInstance();
        const LLVector3 right = -camera->getLeftAxis();
        const LLVector3 up = camera->getUpAxis();
        const LLVector3 back = -camera->getAtAxis();
        shader.uniform3f(sViewX, right.mV[VX], right.mV[VY], right.mV[VZ]);
        shader.uniform3f(sViewY, up.mV[VX], up.mV[VY], up.mV[VZ]);
        shader.uniform3f(sViewZ, back.mV[VX], back.mV[VY], back.mV[VZ]);

        // Trilinear like the flush that makes its mips (one view of all its levels, research/253 3.4);
        // the sun pass reads the map itself (textureLod 0)
        shader.bindTexture(LLShaderMgr::PLUS_CLOUD_MAP, sMap, false, LLTexUnit::TFO_TRILINEAR);
        sMapBound = true;
    }

    const LLRenderTarget* shadowTarget()
    {
        return mapWanted() && shadowLight().fade > 0.f ? sMap : nullptr;
    }

    bool airShadow(AirShadow& out)
    {
        out = AirShadow();
        if (!mapWanted())
        {
            return false;
        }
        const ShadowLight light = shadowLight();
        LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky();
        if (!sky || !(light.fade > 0.f))
        {
            return false;
        }
        const LLVector3& to_light = light.to_light;
        const F32 base = layerBase(*sky);
        const F32 camera_z = LLViewerCamera::getInstance()->getOrigin().mV[VZ];
        out.map = sMap;
        out.strength = shadowStrength();
        out.inverse_width = 1.f / (2.f * sMadeKey.radius);     // the map's own (made for it)
        out.centre[0] = sMadeKey.centre[0];
        out.centre[1] = sMadeKey.centre[1];
        out.layer_middle = layerMiddle(base) - camera_z;
        out.layer_base = base - camera_z;
        out.low_light_fade[0] = LOW_LIGHT_FADE_FROM;
        out.low_light_fade[1] = LOW_LIGHT_FADE_TO;
        out.mean_ready = true;                                  // its mips are made with it (flush)
        out.to_light[0] = to_light.mV[VX];
        out.to_light[1] = to_light.mV[VY];
        out.to_light[2] = to_light.mV[VZ];
        out.fade = light.fade;
        out.mean_lod = (F32)MEAN_LEVEL;
        return true;
    }

    void unbindShadow(LLGLSLShader& shader)
    {
        if (sMapBound)
        {
            shader.unbindTexture(LLShaderMgr::PLUS_CLOUD_MAP);
            sMapBound = false;
        }
    }
}

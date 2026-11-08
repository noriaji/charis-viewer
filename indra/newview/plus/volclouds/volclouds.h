/**
 * @file volclouds.h
 * @brief The volumetric clouds, an option next to the 2D clouds (research/186
 *        in the sl-viewer repository).
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

#ifndef PLUS_VOLCLOUDS_VOLCLOUDS_H
#define PLUS_VOLCLOUDS_VOLCLOUDS_H

#include "stdtypes.h"

class LLGLSLShader;
class LLRenderTarget;
class LLSettingsSky;

// PlusVolumetricClouds (off by default, on at Ultra): the clouds of the main
// view are a slab at the world altitude [max_y, max_y + PlusCloudThickness *
// max_y], marched along each pixel's view ray in the sky dome's cloud pass
// (cloudsF.glsl, VOLUMETRIC), so that they have a thickness (they come nearer
// when the camera flies up). They stand where the 2D clouds are, around the
// camera, and let through what the 2D clouds do (research/244); their colours
// are the 2D clouds' own. The 2D clouds stay for everything else: it is off,
// the reflection probes' faces, the mirrors, and a camera above the slab.
namespace plus::volclouds
{
    // True while the main view's clouds are drawn volumetrically.
    bool wanted();

    // In renderSkyCloudsDeferred, with the volumetric program bound: its
    // uniforms (the slab, the steps, the dome's offset, the tops' relief) for the sky.
    void bind(LLGLSLShader& shader, const LLSettingsSky& sky);

    // The clouds' shadow on the ground (PlusCloudShadow, with the volumetric clouds): a small map of
    // how much of the light the clouds above each place take out (with mips: the last is its mean),
    // around the camera (the clouds are placed around it) and centred where the way from the camera to
    // the light (the sun; the moon while the sun is down) crosses the layer, made again in every frame
    // the clouds flow in (research/253 3.6). The sun light pass multiplies its shadow by what the map
    // says where the way to the light crosses the layer (sunLightF.glsl, PLUS_CLOUD_SHADOW). Not made,
    // and not kept, while off.
    //
    // Before the sun light pass (own target): makes or lets go of the map.
    void updateShadowMap();

    // In the sun light pass, once the shader is bound: its uniforms (strength 0 when there is no
    // map: the probes' passes, off, the light too low) and the map's unit. unbindShadow after the pass.
    void bindShadow(LLGLSLShader& shader);
    void unbindShadow(LLGLSLShader& shader);

    // The map the sun light pass is going to sample (null: none, it is not bound then).
    const LLRenderTarget* shadowTarget();

    // What the volumetric light needs to put the clouds' shadow into the air (research/253 3.8). The
    // places are from the camera in the agent's axes: x east, y north, z up (m). The light at a point p
    // is what sunLightF.glsl's plusCloudShadow gives:
    //   rise = layer_middle - p.z (none above the layer's middle: 1)
    //   uv = (p.xy + to_light.xy / to_light.z * rise - centre) * inverse_width + 0.5
    //   c = textureLod(map, uv, 0).x * smoothstep(0, 0.1, the distance of uv to the map's nearest side)
    //   v = 1 - strength * fade * c
    // and textureLod(map, any uv, mean_lod).x is the mean of c over the map, once mean_ready.
    struct AirShadow
    {
        const LLRenderTarget* map = nullptr; // this frame's map (shadowTarget()); R8, sampled trilinear, with mips when mean_ready
        F32  strength = 0.f;                 // PlusCloudShadow (0..1)
        F32  inverse_width = 0.f;            // 1 / the map's width (1/m)
        F32  centre[2] = {};                 // where the map was made around, east and north of the camera (m)
        F32  layer_middle = 0.f;             // the layer's middle above the camera (m): where the way to the light is cut
        F32  layer_base = 0.f;               // the layer's base above the camera (m): where a ray to the sky leaves the air
        F32  low_light_fade[2] = {};         // the smoothstep of the light's height (to_light z) that fades the shadow
        bool mean_ready = false;             // the top mip is the map's mean
        F32  to_light[3] = {};               // the light the shadow is cast from (the sun; the moon while the sun is down), unit
        F32  fade = 0.f;                     // smoothstep(low_light_fade[0], low_light_fade[1], to_light z), for the
                                             // moon times its fade in while the sun sets; above 0
        F32  mean_lod = 0.f;                 // the mip level of one texel: the mean's
    };
    // False: no clouds' shadow in this view of this frame (off, a probe face or mirror, no map yet, the
    // sun pass not run (no sun shadow, no SSAO), the light too low). Call it where the map is read (after
    // updateShadowMap, in the main view).
    bool airShadow(AirShadow& out);

    // With the screen's targets (LLPipeline::releaseScreenBuffers).
    void releaseTargets();
}

#endif // PLUS_VOLCLOUDS_VOLCLOUDS_H

/**
 * @file rainbow.cpp
 * @brief The rainbow worked out in the sky's shaders: its settings and uniforms (research/255).
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

#include "rainbow.h"

#include "llglslshader.h"
#include "llsettingssky.h"
#include "llviewercontrol.h"
#include "plus/plus.h"
#include "plus/skyscreen/skyscreen.h"

namespace plus::rainbow
{
    namespace
    {
        LLStaticHashedString sRainbow("plus_rainbow");
        LLStaticHashedString sTwin("plus_rainbow_twin");
        LLStaticHashedString sLook("plus_rainbow_look");
        LLStaticHashedString sCeiling("plus_rainbow_ceiling");

        constexpr S32 MODE_OFF = 0;
        constexpr S32 MODE_MANUAL = 1;

        // A kind of PlusRainbowKind: a set of drops and a light (research/255 section 2). The kinds are not
        // apart in nature: each is what the drops' size and the light make, and the numbers below are
        // checked in research/255-rainbows-sim/results.md (the angles, the colours, the secondary).
        struct Kind
        {
            F32 radius;         // the drops' radius (um, as EEP's droplet_radius); 10 - 2000 (the shader clamps)
            F32 secondary;      // the secondary bow's share (1: as the drops make it, about 1/7 of the primary)
            F32 supernumerary;  // the supernumeraries' share (0: smeared out by a spread of drop sizes, 1: Airy's)
            F32 saturation;     // 1: the colours as worked out, 0: white
            F32 tint[3];        // the light's filter (sRGB)
            F32 twin_radius;    // a second set of larger drops, flattened as they fall (0: none)
            F32 twin_shrink;    // their bow taken in at its top (degrees)
            F32 twin_share;
        };

        const Kind KINDS[] = {
            // 0 the primary alone: rain's drops (0.5 mm), their supernumeraries smeared out
            { 500.f, 0.f, 0.15f, 1.f, { 1.f, 1.f, 1.f }, 0.f, 0.f, 0.f },
            // 1 with the secondary (its colours the other way round, Alexander's dark band between)
            { 500.f, 1.f, 0.15f, 1.f, { 1.f, 1.f, 1.f }, 0.f, 0.f, 0.f },
            // 2 with supernumeraries: small drops all of a size (0.2 mm), pink and green bands inside
            { 200.f, 1.f, 1.f, 1.f, { 1.f, 1.f, 1.f }, 0.f, 0.f, 0.f },
            // 3 fogbow: fog's droplets (20 um), broad, white, at about 40 degrees
            { 20.f, 0.f, 0.6f, 1.f, { 1.f, 1.f, 1.f }, 0.f, 0.f, 0.f },
            // 4 sunset: the red of a low sun on the drops
            { 600.f, 1.f, 0.15f, 1.f, { 1.f, 0.55f, 0.25f }, 0.f, 0.f, 0.f },
            // 5 moonbow: pale, as the eye sees it by night (the moon's light comes with the night itself)
            { 500.f, 1.f, 0.15f, 0.3f, { 1.f, 1.f, 1.f }, 0.f, 0.f, 0.f },
            // 6 twinned: small drops and large flattened ones, two bows from one foot (research/255 [low 18])
            { 300.f, 1.f, 0.5f, 1.f, { 1.f, 1.f, 1.f }, 1200.f, 1.2f, 0.8f },
        };
        constexpr S32 KIND_COUNT = (S32)(sizeof(KINDS) / sizeof(KINDS[0]));
        constexpr S32 KIND_DEFAULT = 1;

        // The light's fade, by the z of the sun's (moon's) direction. The sun's bow goes in its last
        // degree above the horizon (the official rainbow stops at the horizon: getIsSunUp()); the moon's
        // comes in once the sun is some 6 degrees under (a dark sky) and the moon a degree up. Between the
        // two, at the switch from the sun's light to the moon's, there is none: no jump.
        constexpr F32 SUN_Z_FULL = 0.0175f;
        constexpr F32 NIGHT_SUN_Z_FULL = -0.105f;
        constexpr F32 MOON_Z_FULL = 0.0175f;

        // softenLightF.glsl's final_scale in a classic sky (classic_mode)
        constexpr F32 CLASSIC_FINAL_SCALE = 1.1f;

        F32 smooth01(F32 t)
        {
            t = llclamp(t, 0.f, 1.f);
            return t * t * (3.f - 2.f * t);
        }

        S32 mode()
        {
            static LLCachedControl<S32> setting(gSavedSettings, "PlusRainbow", MODE_OFF);
            return llclamp((S32)setting, MODE_OFF, MODE_MANUAL);
        }

        // 0 - 1: how much of the bow the light gives (the sun's by day, the moon's by night; none
        // without either, as the official rainbow)
        F32 lightShare(const LLSettingsSky& sky)
        {
            const F32 sun_z = sky.getSunDirection().mV[VZ];
            if (sky.getIsSunUp())
            {
                return smooth01(sun_z / SUN_Z_FULL);
            }
            if (sky.getIsMoonUp())
            {
                const F32 moon_brightness = sky.getMoonBrightness();
                const F32 moon = llfinite(moon_brightness) ? llclamp(moon_brightness, 0.f, 1.f) : 0.f;
                return smooth01(sun_z / NIGHT_SUN_Z_FULL) * smooth01(sky.getMoonDirection().mV[VZ] / MOON_Z_FULL) * moon;
            }
            return 0.f;
        }

        // Under "no post" (plus::isNoPost) the most a channel of the sky's linear light may reach with the
        // bow: 1 on the screen through softenLightF.glsl's sky_hdr_scale and its classic 1.1; else 0 (none:
        // the tone mapping and skyF.glsl's clamp hold an HDR sky)
        F32 ceiling(const LLSettingsSky& sky)
        {
            if (!plus::isNoPost(&sky))
            {
                return 0.f;
            }
            const F32 scale = plus::skyscreen::current(&sky).hdr_scale * (plus::isClassicSky(&sky) ? CLASSIC_FINAL_SCALE : 1.f);
            return 1.f / llmax(scale, 1e-3f);
        }
    }

    bool enabled()
    {
        return mode() != MODE_OFF;
    }

    void bind(LLGLSLShader& shader, const LLSettingsSky& sky, bool drawn)
    {
        F32 strength = 0.f;
        if (drawn && enabled())
        {
            static LLCachedControl<F32> strength_setting(gSavedSettings, "PlusRainbowStrength", 0.6f);
            const F32 wanted = llfinite((F32)strength_setting) ? llclamp((F32)strength_setting, 0.f, 1.f) : 0.f;
            strength = wanted * lightShare(sky);
        }
        if (!(strength > 0.f))
        {
            // the shaders read nothing else then
            shader.uniform4f(sRainbow, 0.f, 0.f, 0.f, 0.f);
            return;
        }

        static LLCachedControl<S32> kind_setting(gSavedSettings, "PlusRainbowKind", KIND_DEFAULT);
        const Kind& kind = KINDS[llclamp((S32)kind_setting, 0, KIND_COUNT - 1)];
        shader.uniform4f(sRainbow, strength, kind.radius, kind.supernumerary, kind.secondary);
        shader.uniform4f(sTwin, kind.twin_radius, kind.twin_shrink * DEG_TO_RAD, kind.twin_share, 0.f);
        shader.uniform4f(sLook, kind.tint[0], kind.tint[1], kind.tint[2], kind.saturation);
        shader.uniform1f(sCeiling, ceiling(sky));
    }
}

/**
 * @file rainbow.h
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

#ifndef PLUS_RAINBOW_H
#define PLUS_RAINBOW_H

#include "stdtypes.h"

class LLGLSLShader;
class LLSettingsSky;

// The rainbow of research/255, stage 1 (rainbow-core): worked out per pixel in skyRainbowF.glsl, which
// the WindLight sky (skyF.glsl) and the clouds (cloudsF.glsl) both add to what they write. This side
// reads the settings and hands the shaders the kind's drops, the strength and the "no post" ceiling.
//
//   PlusRainbow          0: off, the official rainbow (the sky's texture, its moisture_level) as it was;
//                        1: by hand: the bow of PlusRainbowKind, always (the official one off)
//   PlusRainbowKind      0 primary, 1 with the secondary, 2 with supernumeraries, 3 fogbow, 4 sunset
//                        (red), 5 moonbow (pale), 6 twinned
//   PlusRainbowStrength  0 - 1: the bow's brightest point, times its light
//
// Its light is the sky's own (the sun's, or by night the moon's): none while neither is up (as the
// official one), fading as the sun sets and coming in as the moonbow once the sky is dark.
namespace plus::rainbow
{
    // PlusRainbow is on: the worked-out bow replaces the official one, whose moisture_level is then
    // to be 0. Cached. Main thread.
    bool enabled();

    // Sets the plus_rainbow* uniforms of the bound sky (skyF.glsl) or cloud (cloudsF.glsl) shader for
    // sky. drawn: false where no bow is drawn (a reflection probe's irradiance pass, the HDRI sky): the
    // strength is then 0 and the shaders leave the sky as it is. Off, it sets the strength (0) only. No
    // allocation. Main thread.
    void bind(LLGLSLShader& shader, const LLSettingsSky& sky, bool drawn);
}

#endif // PLUS_RAINBOW_H

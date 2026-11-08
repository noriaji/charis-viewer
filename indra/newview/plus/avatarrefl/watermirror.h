/**
 * @file watermirror.h
 * @brief The water as a mirror (research/139): the sky, the terrain and, by
 *        choice, the plants, the objects and the avatars drawn again from the
 *        camera mirrored in the region's water, and read by the water shader
 *        in place of the probes, as the water reflections before PBR did.
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

#ifndef PLUS_AVATARREFL_WATERMIRROR_H
#define PLUS_AVATARREFL_WATERMIRROR_H

#include <glm/glm.hpp>

#include <map>
#include <string>

class LLGLSLShader;

// Part of plus/avatarrefl: its hooks call these (avatarrefl.h), and one hook of
// its own in LLSettingsVOWater::applySpecial() (the clip at the water). With
// PlusWaterMirror at 0 (the default) each is a flag test and the shaders are
// the official ones.
namespace plus::avatarrefl::watermirror
{
    // From plus::avatarrefl::addShaderDefines(): PLUS_WATER_MIRROR while the
    // mirror is on (Vulkan only). class3/deferred/reflectionProbeF.glsl then
    // samples it on the water.
    void addShaderDefines(std::map<std::string, std::string>& attribs);

    // From plus::avatarrefl::renderPlanar(), before the avatar reflection:
    // draws the world mirrored in the water into a target of its own (every
    // PlusWaterMirrorRate frames), or releases the target when the mirror is
    // off.
    void render();

    // From plus::avatarrefl::bind(): the mirror's two textures and uniforms
    // for the water program in the main view; off (weight 0, the samplers on a
    // blank texture) for every other program and pass.
    void bind(LLGLSLShader& shader);

    // Whether the mirror holds the avatars this frame (PlusWaterMirror 4 and
    // a picture to read): the avatar reflection then leaves the water to it.
    bool coversAvatars();

    // The <plus> lines in LLSettingsVOWater::applySpecial(): while the
    // mirror's pass is drawn, the mirrors' clip (mirrorClip() in the fragment
    // shaders, clipPlane and mirror_flag) keeps what is above the water and
    // discards the rest. water_plane is the water in eye space as applySpecial
    // makes it (xyz up, w distance). Returns false outside the pass, leaving
    // clip_plane as it is.
    bool clipAtWater(const glm::vec4& water_plane, glm::vec4& clip_plane);
}

#endif // PLUS_AVATARREFL_WATERMIRROR_H

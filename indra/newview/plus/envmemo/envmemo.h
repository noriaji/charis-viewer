/**
 * @file envmemo.h
 * @brief The sky and water uniforms sent again only when they change
 *        (research/187 N4).
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

#ifndef PLUS_ENVMEMO_H
#define PLUS_ENVMEMO_H

#include "llglslshader.h"   // LLShaderUniforms, LLGLSLShader::SG_COUNT
#include "llrhi.h"          // LLRHI::sEnvUniformMemo, sEnvUniformMemoVerify

// LLEnvironment::update runs for the main view and again for every
// reflection probe face; each time it makes the sky and water uniforms again
// and marks every shader (and its rigged and GLTF variants) dirty, and every
// shader bound after it sets all of them again (about 45 values through the
// value cache). Most of them are what they were (research/187 N4).
//
// Here, after the uniforms are made, they are folded per shader group into
// the values a shader ends up with (the order of LLEnvironment::
// updateShaderUniforms: sky then water, SG_ANY then the group; the last one
// set wins) and compared with the time before. Only when one changed does
// LLGLSLShader::sEnvUniformsVersion move. The same shaders as before are
// marked dirty (the 53 of LLViewerShaderMgr's list and their variants: a
// short loop); a dirty shader bound gets nothing when it holds the version,
// the values changed in the steps it missed (kept for the last few
// versions), or all of them as before when it missed more, the layout
// changed (an irradiance face sets fewer), it is new, or a value at one of
// their locations was set by someone else since (LLGLSLShader::
// noteEnvLocation: the water's rotated light, the sky's HDRI exposure; those
// shaders get all of them, as before).
//
// The values that change by design (cloud scroll, camera position, the water
// plane in eye space, the irradiance face's ambient) simply change the
// version. Per-bind values set outside the environment (SSR's Poisson offset,
// research/178 s10 F5) are not touched.
namespace plus::envmemo
{
    // LLEnvironment::update, after updateSettingsUniforms.
    void afterUpdate(const LLShaderUniforms* sky, const LLShaderUniforms* water);

    // LLEnvironment::updateEnvironment, after updateSettingsUniforms: the
    // uniforms were made again without a step, so every shader gets all of
    // them at its next dirty bind and the next update starts a full table.
    void invalidate();

    // LLEnvironment::updateShaderUniforms. True: done here (else apply as before).
    bool apply(LLGLSLShader* shader, LLShaderUniforms* sky, LLShaderUniforms* water);
}

#endif // PLUS_ENVMEMO_H

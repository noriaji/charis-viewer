/**
 * @file llworldpassstate.h
 * @brief The world pools' pass state declarations (research/204, research/197 stages 3d and 4c).
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

#ifndef LL_LLWORLDPASSSTATE_H
#define LL_LLWORLDPASSSTATE_H

#include "llgl.h"
#include "llglslshader.h"
#include "llrhipassstate.h"
#include "pipeline.h"

extern bool gCubeSnapshot;
extern U32 gFrameCount;

// A world pool's pass, opened at its head (llrhipassstate.h): the view it is
// drawn for and the frame come from the pipeline. name: a string literal;
// null: not declared (such as a pool's draws for a shadow map, stage 5).
class LLWorldPassState : public LLRHI::PassStateScope
{
public:
    explicit LLWorldPassState(const char* name)
        : LLRHI::PassStateScope(name,
                                gCubeSnapshot ? LLRHI::PASS_VIEW_PROBE :
                                LLPipeline::sReflectionRender ? LLRHI::PASS_VIEW_REFLECTION : LLRHI::PASS_VIEW_MAIN,
                                gFrameCount)
    {
    }
};

// The texture unit of a shader's channel (LLShaderMgr's texture uniforms), as
// LLGLSLShader::bindTexture finds it; -1: the shader has no such channel.
inline S32 world_pass_channel(const LLGLSLShader& shader, S32 channel)
{
    return channel >= 0 && channel < (S32)shader.mTexture.size() ? shader.mTexture[channel] : -1;
}

// The declarations the pools share. What they say is what the code around
// the pools sets today; verify checks it (research/204 3.1).

// An opaque pool of the G-buffer (LLPipeline::renderGeomDeferred): culling on
// (its LLGLEnable), the colour mask all (its setColorMask(true, true)), the
// depth test of the frame (display()'s LLGLDepthTest(GL_TRUE, GL_TRUE,
// GL_LEQUAL)), and no blending, polygon offset or depth clamp. The stencil is
// the surroundings' (plus::skinsss marks skin in the main view): not declared.
inline LLRHI::PassState world_gbuffer_pass_state(const LLGLSLShader& shader)
{
    LLRHI::PassState state;
    state.noBlend().colorMask(true, true, true, true).depth(true, true, GL_LEQUAL).cullFace(GL_BACK).noPolygonOffset().noDepthClamp()
         .useProgram(shader.mProgramObject);
    return state;
}

// A part of the sky (LLGLSPipelineDepthTestSkyBox, LLGLSPipelineBlendSkyBox:
// culling off, the depth test with GL_LEQUAL, writing as asked) in the
// G-buffer's colour mask. blend_dst 0: no blending; else GL_SRC_ALPHA and
// blend_dst (LLGLSPipelineBlendSkyBox's BT_ALPHA, the stars' BT_ADD_WITH_ALPHA).
// The units the pass binds itself follow with sampler() (samplersPart: the
// program may read others).
inline LLRHI::PassState world_sky_pass_state(const LLGLSLShader& shader, bool depth_write, U32 blend_dst)
{
    LLRHI::PassState state;
    if (blend_dst)
    {
        state.blendFunc(GL_SRC_ALPHA, blend_dst);
    }
    else
    {
        state.noBlend();
    }
    state.colorMask(true, true, true, true).depth(true, depth_write, GL_LEQUAL).noCull().useProgram(shader.mProgramObject).samplersPart();
    return state;
}

#endif // LL_LLWORLDPASSSTATE_H

/**
 * @file llvkskinsss.cpp
 * @brief Skin subsurface scattering recorded directly in Vulkan (research/100).
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

#include "linden_common.h"

#include "llvkskinsss.h"

#include "llrhiskinsss.h"
#include "llvkshader.h"
#include "llvkfullscreen.h"
#include "llvktracker.h"

#include <mutex>

namespace
{
    // SkinSssDebug: one colour on every pixel the stencil test lets through.
    const char* const DEBUG_FRAGMENT = R"GLSL(#version 450
layout(std140, set = 0, binding = 0) uniform SkinSssDebug
{
    vec4 color;     // premultiplied
} u;

layout(location = 0) out vec4 frag_color;

void main()
{
    frag_color = u.color;
}
)GLSL";

    struct DebugUniforms
    {
        F32 color[4];
    };
    static_assert(sizeof(DebugUniforms) == 16, "DebugUniforms must match the std140 block SkinSssDebug");

    // Only where the skin bit is set; only where the eye bit is set.
    constexpr LLVKFullscreen::StencilTest ON_SKIN = { true, LLRHI::STENCIL_SKIN, LLRHI::STENCIL_SKIN };
    constexpr LLVKFullscreen::StencilTest ON_EYE = { true, LLRHI::STENCIL_EYE, LLRHI::STENCIL_EYE };

    class Renderer
    {
    public:
        bool debug(const LLRHI::SkinSssDebug& pass);

        // research/159 3.1 (--bake-shaders): the passes set up ahead of their first use.
        bool bake() { return ready(); }

    private:
        bool ready();

        bool mTried = false;
        bool mReady = false;
        LLVKFullscreen::Pass mDebug;
        LLVKFullscreen::Pass mDebugEye;
        bool mLoggedDebug = false;
    };

    Renderer sRenderer;

    bool Renderer::ready()
    {
        if (!mTried)
        {
            mTried = true;
            mReady = mDebug.init("skin sss debug", DEBUG_FRAGMENT, 0, LLVKFullscreen::Blend::PremultipliedOver,
                                 LLVKFullscreen::WRITE_RGBA, ON_SKIN) &&
                     mDebugEye.init("skin sss debug eyes", DEBUG_FRAGMENT, 0, LLVKFullscreen::Blend::PremultipliedOver,
                                    LLVKFullscreen::WRITE_RGBA, ON_EYE);
            if (!mReady)
            {
                LL_WARNS("VKSkinSss") << "The skin passes could not be set up; they stay off" << LL_ENDL;
            }
        }
        return mReady;
    }

    bool Renderer::debug(const LLRHI::SkinSssDebug& pass)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        if (!ready())
        {
            return false;
        }
        LLVKMirror::Target target;
        VkCommandBuffer cmd = LLVKTracker::beginPass({}, target);
        if (!cmd)
        {
            return false;
        }
        DebugUniforms uniforms;
        DebugUniforms eye_uniforms;
        for (U32 c = 0; c < 4; ++c)
        {
            uniforms.color[c] = pass.color[c];
            eye_uniforms.color[c] = pass.eye_color[c];
        }
        bool drawn = mDebug.draw(cmd, target, &uniforms, sizeof(uniforms), nullptr, 0);
        drawn = drawn && mDebugEye.draw(cmd, target, &eye_uniforms, sizeof(eye_uniforms), nullptr, 0);
        LLVKTracker::endPass();
        if (drawn && !mLoggedDebug)
        {
            mLoggedDebug = true;
            LL_INFOS("VKSkinSss") << "Skin mark shown: " << target.width << "x" << target.height << ", depth format "
                                  << (U32)target.depth_format << LL_ENDL;
        }
        return drawn;
    }

    bool record_debug(const LLRHI::SkinSssDebug& pass)
    {
        return sRenderer.debug(pass);
    }
}

void LLVKSkinSss::install()
{
    if (!LLRHI::sSkinSss)
    {
        LL_INFOS("VKSkinSss") << "Skin passes off (LLRHI_SKIN_SSS=0)" << LL_ENDL;
        return;
    }
    LLRHI::sSkinSssDebug = &record_debug;
    LLVKShader::addBakeStep("skin SSS", []() { std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock()); return sRenderer.bake(); });   // research/159 3.1
}

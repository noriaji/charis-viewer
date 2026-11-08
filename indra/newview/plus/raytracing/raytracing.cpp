/**
 * @file raytracing.cpp
 * @brief Hardware ray tracing for photos: an opt-in (research/230)
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

#include "llviewerprecompiledheaders.h"

#include "raytracing.h"

#include "llcontrol.h"
#include "llrhi.h"
#include "llrhiscene.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "lldrawpool.h"

namespace plus::raytracing
{
    bool available()
    {
        return LLRHI::sRayTracing;
    }

    std::string status()
    {
        return LLRHI::rayTracingStatus();
    }

    bool enabled()
    {
        static LLCachedControl<bool> wanted(gSavedSettings, "PlusRayTracing", false);
        return wanted && LLRHI::sRayTracing;
    }

    F32 range()
    {
        static LLCachedControl<F32> metres(gSavedSettings, "PlusRayTracingRange", 64.f);
        return llclamp((F32)metres, 8.f, 512.f);
    }

    namespace
    {
        // The scene passes whose records are opaque and not rigged: the ones the first
        // stages build acceleration structures of (research/230 4.2). Alpha, masks,
        // glow, invisible and rigged passes stay out.
        void add_opaque_passes(U32 (&mask)[4])
        {
            static const U32 passes[] = {
                LLRenderPass::PASS_SIMPLE,        LLRenderPass::PASS_FULLBRIGHT, LLRenderPass::PASS_FULLBRIGHT_SHINY,
                LLRenderPass::PASS_SHINY,         LLRenderPass::PASS_BUMP,       LLRenderPass::PASS_POST_BUMP,
                LLRenderPass::PASS_MATERIAL,      LLRenderPass::PASS_SPECMAP,    LLRenderPass::PASS_NORMMAP,
                LLRenderPass::PASS_NORMSPEC,      LLRenderPass::PASS_GLTF_PBR,
            };
            for (U32 pass : passes)
            {
                if (pass < LLRHI::SCENE_MAX_PASSES)
                {
                    mask[pass >> 5] |= 1u << (pass & 31);
                }
            }
        }

        // LLRHI::sRayTracingSource: asked once a frame at the submission (main thread).
        // Off (the common case): one cached read and out.
        bool source(LLRHI::RayTracingWanted& out)
        {
            out = LLRHI::RayTracingWanted();
            if (!enabled())
            {
                return true;
            }
            out.on = true;
            out.range = range();
            const LLVector3 origin = LLViewerCamera::getInstance()->getOrigin();
            out.camera[0] = origin.mV[0];
            out.camera[1] = origin.mV[1];
            out.camera[2] = origin.mV[2];
            add_opaque_passes(out.opaque_passes);
            return true;
        }

        [[maybe_unused]] const bool sSourceSet = (LLRHI::sRayTracingSource = &source, true);
    }
}

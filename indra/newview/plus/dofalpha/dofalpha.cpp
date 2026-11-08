/**
 * @file dofalpha.cpp
 * @brief Depth of field behind alpha: the depth of field's depth pass writes
 *        an alpha surface's depth only where it is mostly opaque.
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

// Which pixels keep the background sharp (official code, handoff 2026-10-20-dof-alpha):
//  - Depth of field runs in LLPipeline::renderFinalize(), after the alpha is
//    blended into the image and tone mapped; each pixel's circle of confusion
//    comes from the depth buffer (cofF.glsl), one depth per pixel.
//  - LLDrawPoolAlpha::renderPostDeferred() ends, with depth of field on, with
//    a depth-only pass over the alpha (colour writes off, renderAlpha(...,
//    true)): the plain alpha (gDeferredFullbrightAlphaMaskProgram) discards
//    below 0.33, but in renderAlpha a batch with a legacy material takes
//    gDeferredMaterialProgram[mask], which never discards when blended
//    (materialF.glsl), and a PBR blend batch gDeferredPBRAlphaProgram, whose
//    minimum alpha LLFetchedGLTFMaterial::bindUniforms() sets to -1. Their
//    whole triangles wrote depth.
//  - So most mesh hair (materials or PBR) set its depth, in focus, over all
//    of its cards, and the background seen through their clear parts stayed
//    sharp. Without OIT (RenderOITMode 0) the rigged alpha also writes depth
//    while it is drawn (upstream's first pass), which this does not change.
//
// Here those programs discard below the cutoff in that pass, as the plain
// alpha does, and skip their lighting there. Pixels at or above the cutoff
// still take the surface's depth and keep what is behind them sharp, in the
// proportion it shows through (one depth per pixel).
//
// Focus through glass (research/218, the focus-glass branch): the plain alpha
// program's minimum alpha looks at the texture's alpha only (fullbrightF.glsl),
// and a window pane is usually an opaque or blank texture made see-through by
// its colour's alpha, so the pass wrote the pane's depth over everything seen
// through it: a face behind the glass, in focus by the focus lock's distance,
// was blurred by the glass's distance. With plus::focuslock::throughGlass()
// the world's batches of the pass discard below SEE_THROUGH_OPACITY of the
// texture's alpha times the colour's, in all three kinds of program.

#include "llviewerprecompiledheaders.h"

#include "plus.h"

#include "dofalpha.h"

#include "llglslshader.h"
#include "llmaterial.h"
#include "llstaticstringtable.h"
#include "llviewercontrol.h"
#include "llviewershadermgr.h"

#include "plus/focuslock/focuslock.h"

namespace plus::dofalpha
{
    namespace
    {
        constexpr char SETTING_ON[]     = "PlusDoFAlpha";
        constexpr char SETTING_CUTOFF[] = "PlusDoFAlphaCutoff";

        // Upstream's minimum alpha for the depth pass (renderPostDeferred).
        constexpr F32 UPSTREAM_CUTOFF = 0.33f;

        bool sActive = false;   // between beginDepthPass() and endDepthPass()
        bool sGlass  = false;   // the same, with focus through glass in the pass

        // batchCutoff()'s last program and kind of batch, to skip the uniform's name
        // lookup while consecutive batches want the same
        const LLGLSLShader* sLastProgram = nullptr;
        bool                sLastWorld   = false;

        bool enabled()
        {
            static LLCachedControl<bool> on(gSavedSettings, SETTING_ON, true);
            return on;
        }

        // The uniform of materialF.glsl (blend) and pbralphaF.glsl.
        const LLStaticHashedString& cutoffUniform()
        {
            static const LLStaticHashedString name("plus_dof_alpha_cutoff");
            return name;
        }

        // A legacy material program of the blend mode (rigged or not).
        bool blendMaterialProgram(const LLGLSLShader* program)
        {
            constexpr U32 COUNT = LLMaterial::SHADER_COUNT * 2;
            for (U32 i = LLMaterial::DIFFUSE_ALPHA_MODE_BLEND; i < COUNT; i += 4)  // (i & 0x3) == BLEND
            {
                if (program == &gDeferredMaterialProgram[i])
                {
                    return true;
                }
            }
            return false;
        }

        // The cutoff on every program the depth pass can take for a blended
        // legacy material or PBR batch, rigged ones included, and with plain the
        // plain alpha mask program's (focus through glass); the bound program
        // is bound again after.
        void setProgramsCutoff(F32 cutoff, bool plain)
        {
            LLGLSLShader* const bound = LLGLSLShader::sCurBoundShaderPtr;
            auto set = [cutoff](LLGLSLShader& program)
            {
                if (program.isComplete())
                {
                    program.bind();
                    program.uniform1f(cutoffUniform(), cutoff);
                }
            };
            // Bit 0x10 of the index is the rigged variant (llviewershadermgr.cpp).
            for (U32 i = 0; i < LLMaterial::SHADER_COUNT * 2; ++i)
            {
                if ((i & 0x3) == LLMaterial::DIFFUSE_ALPHA_MODE_BLEND)
                {
                    set(gDeferredMaterialProgram[i]);
                }
            }
            set(gDeferredPBRAlphaProgram);
            if (gDeferredPBRAlphaProgram.mRiggedVariant)
            {
                set(*gDeferredPBRAlphaProgram.mRiggedVariant);
            }
            if (plain)
            {
                set(gDeferredFullbrightAlphaMaskProgram);   // only the world's batches set it (not rigged)
            }

            if (bound)
            {
                bound->bind();
            }
            else
            {
                LLGLSLShader::unbind();
            }
        }
    }

    F32 depthCutoff()
    {
        if (!enabled())
        {
            return UPSTREAM_CUTOFF;
        }
        static LLCachedControl<F32> cutoff(gSavedSettings, SETTING_CUTOFF, UPSTREAM_CUTOFF);
        // Above 0: 0 in the shaders means "not the depth pass".
        // NaN would pass llclamp() into the shaders.
        return llfinite((F32)cutoff) ? llclamp((F32)cutoff, 0.01f, 1.f) : UPSTREAM_CUTOFF;
    }

    void beginDepthPass(bool dof)
    {
        if (sActive || sGlass)
        {
            return;
        }
        sLastProgram = nullptr;
        sGlass       = dof && plus::focuslock::throughGlass();
        if (enabled())
        {
            setProgramsCutoff(depthCutoff(), false);
            sActive = true;
        }
    }

    void endDepthPass()
    {
        if (!sActive && !sGlass)
        {
            return;
        }
        setProgramsCutoff(0.f, sGlass);
        sActive      = false;
        sGlass       = false;
        sLastProgram = nullptr;
    }

    bool depthPassActive()
    {
        return sActive;
    }

    bool glassActive()
    {
        return sGlass;
    }

    void batchCutoff(LLGLSLShader* program, bool world)
    {
        if (!sGlass || !program || (program == sLastProgram && world == sLastWorld))
        {
            return;
        }
        sLastProgram = program;
        sLastWorld   = world;

        constexpr F32 GLASS = plus::focuslock::SEE_THROUGH_OPACITY;
        F32 cutoff;
        if (program == &gDeferredFullbrightAlphaMaskProgram)
        {
            // 0 for an avatar's: as upstream, its minimum alpha alone (the texture's alpha)
            cutoff = world ? GLASS : 0.f;
        }
        else if (program == &gDeferredPBRAlphaProgram || blendMaterialProgram(program))
        {
            const F32 base = sActive ? depthCutoff() : 0.f;     // what beginDepthPass() set
            cutoff = world ? llmax(base, GLASS) : base;
        }
        else
        {
            return;
        }
        llassert(LLGLSLShader::sCurBoundShaderPtr == program);
        program->uniform1f(cutoffUniform(), cutoff);
    }
}

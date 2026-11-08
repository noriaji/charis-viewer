/**
 * @file lldrawpooltree.cpp
 * @brief LLDrawPoolTree class implementation
 *
 * $LicenseInfo:firstyear=2002&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "lldrawpooltree.h"

#include "lldrawable.h"
#include "llface.h"
#include "llsky.h"
#include "llvotree.h"
#include "pipeline.h"
#include "llviewercamera.h"
#include "llviewershadermgr.h"
#include "llrender.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llenvironment.h"
#include "llrhigl.h"
#include "plus/shadowrest/shadowrest.h" // LLRHI native (research/137 N10)
#include "llworldpassstate.h" // <plus> research/204: the pass state </plus>
#include "llscenerecords.h" // <plus> research/204 10 (vk-s4c2): the world pools' records </plus>

S32 LLDrawPoolTree::sDiffTex = 0;
static LLGLSLShader* shader = NULL;

LLDrawPoolTree::LLDrawPoolTree(LLViewerTexture *texturep) :
    LLFacePool(POOL_TREE),
    mTexturep(texturep)
{
    mTexturep->setAddressMode(LLTexUnit::TAM_WRAP);
}

//============================================
// deferred implementation
//============================================
void LLDrawPoolTree::beginDeferredPass(S32 pass)
{
    LL_RECORD_BLOCK_TIME(FTM_RENDER_TREES);

    shader = &gDeferredTreeProgram;
    shader->bind();
    shader->setMinimumAlpha(0.5f);
}

void LLDrawPoolTree::renderDeferred(S32 pass)
{
    LL_PROFILE_ZONE_SCOPED;

    // <plus> research/204: the pass state (not for a shadow map: renderShadow comes here too).
    // research/204 10 (vk-s4c2): opened before the empty list returns, so a view that comes with
    // no tree face is counted too (passes, no draws: the "tree main" that never showed).
    LLWorldPassState pass_state(LLPipeline::sShadowRender ? nullptr : "tree");
    // </plus>
    if (mDrawFace.empty())
    {
        return;
    }

    // <plus> research/204
    if (pass_state.declaring())
    {
        pass_state.declare(world_gbuffer_pass_state(*shader).sampler(sDiffTex));
    }
    // </plus>
    gGL.getTexUnit(sDiffTex)->bindFast(mTexturep);
    mTexturep->addTextureStats(1024.f * 1024.f); // <=== keep Linden tree textures at full res
    pass_state.check((U32)mDrawFace.size()); // <plus> research/204: the loop below draws only </plus>
    LLRHI::PassStateWrite write_through(pass_state); // <plus> research/207 T4: on, the declaration for the loop </plus>
    // <plus> research/204 10 (vk-s4c2): the faces of the groups the retained scene drew are left out
    LLSceneRecords::WorldDraw scene_draw = gSceneRecords.drawWorldPool(this);
    // </plus>

    for (std::vector<LLFace*>::iterator iter = mDrawFace.begin();
        iter != mDrawFace.end(); iter++)
    {
        LLFace* face = *iter;
        if (gSceneRecords.worldFaceDrawn(scene_draw, face)) { continue; } // <plus> research/204 10 </plus>
        LLVertexBuffer* buff = face->getVertexBuffer();

        if (buff)
        {
            LLMatrix4* model_matrix = &(face->getDrawable()->getRegion()->mRenderMatrix);

            llassert(gGL.getMatrixMode() == LLRender::MM_MODELVIEW);
            LLRenderPass::applyModelMatrix(model_matrix);

            buff->setBuffer();
            buff->drawRange(LLRender::TRIANGLES, 0, buff->getNumVerts() - 1, buff->getNumIndices(), 0);
        }
    }
}

void LLDrawPoolTree::endDeferredPass(S32 pass)
{
    LL_RECORD_BLOCK_TIME(FTM_RENDER_TREES);

    shader->unbind();
}

//============================================
// shadow implementation
//============================================
void LLDrawPoolTree::beginShadowPass(S32 pass)
{
    LL_PROFILE_ZONE_SCOPED;

    static LLCachedControl<F32> shadow_offset(gSavedSettings, "RenderDeferredTreeShadowOffset");
    static LLCachedControl<F32> shadow_bias(gSavedSettings, "RenderDeferredTreeShadowBias");
    LLRHI::polygonOffset(shadow_offset(), shadow_bias());

    LLEnvironment& environment = LLEnvironment::instance();

    gDeferredTreeShadowProgram.bind();
    gDeferredTreeShadowProgram.uniform1i(LLShaderMgr::SUN_UP_FACTOR, environment.getIsSunUp() ? 1 : 0);
    gDeferredTreeShadowProgram.setMinimumAlpha(0.5f);
}

void LLDrawPoolTree::renderShadow(S32 pass)
{
    if (plus::shadowrest::treeDrawn(this)) { return; } // LLRHI native (research/137 N10): drawn with the native shadow packets
    renderDeferred(pass);
}

void LLDrawPoolTree::endShadowPass(S32 pass)
{
    LL_PROFILE_ZONE_SCOPED;

    // <plus> research/204: cached (was a lookup by name for each shadow map)
    static LLCachedControl<F32> spot_offset(gSavedSettings, "RenderDeferredSpotShadowOffset");
    static LLCachedControl<F32> spot_bias(gSavedSettings, "RenderDeferredSpotShadowBias");
    LLRHI::polygonOffset(spot_offset(), spot_bias());
    // </plus>
    gDeferredTreeShadowProgram.unbind();
}

bool LLDrawPoolTree::verify() const
{
    return true;
}

LLViewerTexture *LLDrawPoolTree::getTexture()
{
    return mTexturep;
}

LLViewerTexture *LLDrawPoolTree::getDebugTexture()
{
    return mTexturep;
}


LLColor3 LLDrawPoolTree::getDebugColor() const
{
    return LLColor3(1.f, 0.f, 1.f);
}


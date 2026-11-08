/**
 * @file lldrawpool.cpp
 * @brief LLDrawPool class implementation
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

#include "lldrawpool.h"
#include "llrender.h"
#include "llfasttimer.h"
#include "llviewercontrol.h"

#include "lldrawable.h"
#include "lldrawpoolalpha.h"
#include "lldrawpoolavatar.h"
#include "lldrawpoolbump.h"
#include "lldrawpoolmaterials.h"
#include "lldrawpoolpbropaque.h"
#include "lldrawpoolsimple.h"
#include "lldrawpoolsky.h"
#include "lldrawpooltree.h"
#include "lldrawpoolterrain.h"
#include "lldrawpoolwater.h"
#include "lldrawpoolwaterexclusion.h"
#include "llface.h"
#include "llgraywire.h"
#include "llviewerobjectlist.h" // For debug listing.
#include "pipeline.h"
#include "llspatialpartition.h"
#include "llviewercamera.h"
#include "lldrawpoolwlsky.h"
#include "llglslshader.h"
#include "llglcommonfunc.h"
#include "llvoavatar.h"
#include "llviewershadermgr.h"
#include "llrhi.h"
// <plus>
#include "plus/hairsway/hairsway.h"
#include "plus/skinsss/skinsss.h"
#include "plus/skinonce/skinonce.h"
// </plus>
#include "plus/drawlist/drawlist.h" // LLRHI native (research/137 N2)
#include "llrhidrawlist.h" // LLRHI native (research/137 N2)
#include "llfetchedgltfmaterial.h"
#include "llscenerecords.h" // <plus> research/197 stage 2 </plus>
#include "glm/gtc/type_ptr.hpp"

#include <chrono>

S32 LLDrawPool::sNumDrawPools = 0;

//=============================
// Draw Pool Implementation
//=============================
LLDrawPool *LLDrawPool::createPool(const U32 type, LLViewerTexture *tex0)
{
    LLDrawPool *poolp = NULL;
    switch (type)
    {
    case POOL_SIMPLE:
        poolp = new LLDrawPoolSimple();
        break;
    case POOL_GRASS:
        poolp = new LLDrawPoolGrass();
        break;
    case POOL_ALPHA_MASK:
        poolp = new LLDrawPoolAlphaMask();
        break;
    case POOL_FULLBRIGHT_ALPHA_MASK:
        poolp = new LLDrawPoolFullbrightAlphaMask();
        break;
    case POOL_FULLBRIGHT:
        poolp = new LLDrawPoolFullbright();
        break;
    case POOL_GLOW:
        poolp = new LLDrawPoolGlow();
        break;
    case POOL_ALPHA_PRE_WATER:
        poolp = new LLDrawPoolAlpha(LLDrawPool::POOL_ALPHA_PRE_WATER);
        break;
    case POOL_ALPHA_POST_WATER:
        poolp = new LLDrawPoolAlpha(LLDrawPool::POOL_ALPHA_POST_WATER);
        break;
    case POOL_AVATAR:
    case POOL_CONTROL_AV:
        poolp = new LLDrawPoolAvatar(type);
        break;
    case POOL_TREE:
        poolp = new LLDrawPoolTree(tex0);
        break;
    case POOL_TERRAIN:
        poolp = new LLDrawPoolTerrain(tex0);
        break;
    case POOL_SKY:
        poolp = new LLDrawPoolSky();
        break;
    case POOL_VOIDWATER:
    case POOL_WATER:
        poolp = new LLDrawPoolWater();
        break;
    case POOL_BUMP:
        poolp = new LLDrawPoolBump();
        break;
    case POOL_MATERIALS:
        poolp = new LLDrawPoolMaterials();
        break;
    case POOL_WL_SKY:
        poolp = new LLDrawPoolWLSky();
        break;
    case POOL_GLTF_PBR:
        poolp = new LLDrawPoolGLTFPBR();
        break;
    case POOL_GLTF_PBR_ALPHA_MASK:
        poolp = new LLDrawPoolGLTFPBR(LLDrawPool::POOL_GLTF_PBR_ALPHA_MASK);
        break;
    case POOL_WATEREXCLUSION:
        poolp = new LLDrawPoolWaterExclusion();
        break;
    default:
        LL_ERRS() << "Unknown draw pool type!" << LL_ENDL;
        return NULL;
    }

    llassert(poolp->mType == type);
    return poolp;
}

LLDrawPool::LLDrawPool(const U32 type)
{
    mType = type;
    sNumDrawPools++;
    mId = sNumDrawPools;
    mShaderLevel = 0;
    mSkipRender = false;
}

LLDrawPool::~LLDrawPool()
{

}

LLViewerTexture *LLDrawPool::getDebugTexture()
{
    return NULL;
}

//virtual
void LLDrawPool::beginRenderPass( S32 pass )
{
}

//virtual
S32  LLDrawPool::getNumPasses()
{
    return 1;
}

//virtual
void LLDrawPool::beginDeferredPass(S32 pass)
{

}

//virtual
void LLDrawPool::endDeferredPass(S32 pass)
{

}

//virtual
S32 LLDrawPool::getNumDeferredPasses()
{
    return 0;
}

//virtual
void LLDrawPool::renderDeferred(S32 pass)
{

}

//virtual
void LLDrawPool::beginPostDeferredPass(S32 pass)
{

}

//virtual
void LLDrawPool::endPostDeferredPass(S32 pass)
{

}

//virtual
S32 LLDrawPool::getNumPostDeferredPasses()
{
    return 0;
}

//virtual
void LLDrawPool::renderPostDeferred(S32 pass)
{

}

//virtual
void LLDrawPool::endRenderPass( S32 pass )
{
    //make sure channel 0 is active channel
    gGL.getTexUnit(0)->activate();
}

//virtual
void LLDrawPool::beginShadowPass(S32 pass)
{

}

//virtual
void LLDrawPool::endShadowPass(S32 pass)
{

}

//virtual
S32 LLDrawPool::getNumShadowPasses()
{
    return 0;
}

//virtual
void LLDrawPool::renderShadow(S32 pass)
{

}

//=============================
// Face Pool Implementation
//=============================
LLFacePool::LLFacePool(const U32 type)
: LLDrawPool(type)
{
    resetDrawOrders();
}

LLFacePool::~LLFacePool()
{
    gSceneRecords.onWorldPoolGone(this); // <plus> research/204 10 (vk-s4c2): a world pool's scene pass is free </plus>
    destroy();
}

void LLFacePool::destroy()
{
    if (!mReferences.empty())
    {
        LL_INFOS() << mReferences.size() << " references left on deletion of draw pool!" << LL_ENDL;
    }
}

void LLFacePool::dirtyTextures(const std::set<LLViewerFetchedTexture*>& textures)
{
}

void LLFacePool::enqueue(LLFace* facep)
{
    mDrawFace.push_back(facep);
}

// virtual
bool LLFacePool::addFace(LLFace *facep)
{
    addFaceReference(facep);
    return true;
}

// virtual
bool LLFacePool::removeFace(LLFace *facep)
{
    removeFaceReference(facep);

    vector_replace_with_last(mDrawFace, facep);

    return true;
}

// Not absolutely sure if we should be resetting all of the chained pools as well - djs
void LLFacePool::resetDrawOrders()
{
    mDrawFace.resize(0);
}

LLViewerTexture *LLFacePool::getTexture()
{
    return NULL;
}

void LLFacePool::removeFaceReference(LLFace *facep)
{
    if (facep->getReferenceIndex() != -1)
    {
        if (facep->getReferenceIndex() != (S32)mReferences.size())
        {
            LLFace *back = mReferences.back();
            mReferences[facep->getReferenceIndex()] = back;
            back->setReferenceIndex(facep->getReferenceIndex());
        }
        mReferences.pop_back();
    }
    facep->setReferenceIndex(-1);
}

void LLFacePool::addFaceReference(LLFace *facep)
{
    if (-1 == facep->getReferenceIndex())
    {
        facep->setReferenceIndex(static_cast<S32>(mReferences.size()));
        mReferences.push_back(facep);
    }
}

void LLFacePool::pushFaceGeometry()
{
    for (LLFace* const& face : mDrawFace)
    {
        face->renderIndexed();
    }
}

bool LLFacePool::verify() const
{
    bool ok = true;

    for (std::vector<LLFace*>::const_iterator iter = mDrawFace.begin();
         iter != mDrawFace.end(); iter++)
    {
        const LLFace* facep = *iter;
        if (facep->getPool() != this)
        {
            LL_INFOS() << "Face in wrong pool!" << LL_ENDL;
            facep->printDebugInfo();
            ok = false;
        }
        else if (!facep->verify())
        {
            ok = false;
        }
    }

    return ok;
}

void LLFacePool::printDebugInfo() const
{
    LL_INFOS() << "Pool " << this << " Type: " << getType() << LL_ENDL;
}

bool LLFacePool::LLOverrideFaceColor::sOverrideFaceColor = false;

void LLFacePool::LLOverrideFaceColor::setColor(const LLColor4& color)
{
    gGL.diffuseColor4fv(color.mV);
}

void LLFacePool::LLOverrideFaceColor::setColor(const LLColor4U& color)
{
    gGL.diffuseColor4ubv(color.mV);
}

void LLFacePool::LLOverrideFaceColor::setColor(F32 r, F32 g, F32 b, F32 a)
{
    gGL.diffuseColor4f(r,g,b,a);
}


//=============================
// Render Pass Implementation
//=============================
LLRenderPass::LLRenderPass(const U32 type)
: LLDrawPool(type)
{

}

LLRenderPass::~LLRenderPass()
{

}

void LLRenderPass::renderGroup(LLSpatialGroup* group, U32 type, bool texture)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    if (plus::drawlist::renderGroup(*this, group, type, texture)) { return; } // LLRHI native (research/137 N2)
    LLSpatialGroup::drawmap_elem_t& draw_info = group->mDrawMap[type];

    for (LLSpatialGroup::drawmap_elem_t::iterator k = draw_info.begin(); k != draw_info.end(); ++k)
    {
        LLDrawInfo *pparams = *k;
        if (pparams)
        {
            pushBatch(*pparams, texture);
        }
    }
}

void LLRenderPass::renderRiggedGroup(LLSpatialGroup* group, U32 type, bool texture)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    if (plus::drawlist::renderRiggedGroup(*this, group, type, texture)) { return; } // LLRHI native (research/137 N2)
    LLSpatialGroup::drawmap_elem_t& draw_info = group->mDrawMap[type];
    const LLVOAvatar* lastAvatar = nullptr;
    U64 lastMeshId = 0;
    bool skipLastSkin = false;

    for (LLSpatialGroup::drawmap_elem_t::iterator k = draw_info.begin(); k != draw_info.end(); ++k)
    {
        LLDrawInfo* pparams = *k;
        if (pparams)
        {
            if (uploadMatrixPalette(pparams->mAvatar, pparams->mSkinInfo, lastAvatar, lastMeshId, skipLastSkin))
            {
                pushBatch(*pparams, texture);
            }
        }
    }
}

namespace
{
    // LLRHI: the batch loops below as the GPU probe list draws or registers a
    // batch of them (LLPipeline::rhiProbeDraw; research/46 G5, research/50 step 4).
    struct RHIProbePassLoop
    {
        LLRenderPass* pass = nullptr;
        bool texture = false;
        bool batch_textures = false;
        bool mask = false;      // pushMaskBatches: the batch's alpha cutoff first
    };

    void rhi_probe_pass_body(LLDrawInfo& batch, void* context)
    {
        const RHIProbePassLoop& loop = *(const RHIProbePassLoop*)context;
        if (loop.mask)
        {
            LLGLSLShader::sCurBoundShaderPtr->setMinimumAlpha(batch.mAlphaMaskCutoff);
            loop.pass->pushBatch(batch, loop.texture, loop.batch_textures);
        }
        else if (loop.texture)
        {
            loop.pass->pushBatch(batch, true, loop.batch_textures);
        }
        else
        {
            loop.pass->pushUntexturedBatch(batch);
        }
    }

    // A texture as a direct draw names it (false: not ready to be named).
    bool rhi_probe_name(LLViewerTexture* texture, std::vector<LLPipeline::RHIProbeTexture>& out)
    {
        LLPipeline::RHIProbeTexture t;
        t.texture = texture;
        t.name = LLTexUnit::directName(texture);
        if (t.name == LLRHI::DirectDraw::KEEP_UNIT)
        {
            return false;
        }
        out.push_back(t);
        return true;
    }

    // What pushBatch binds: the batch's texture list (batch_textures), or its texture.
    S32 rhi_probe_pass_textures(LLDrawInfo& batch, void* context, std::vector<LLPipeline::RHIProbeTexture>& out)
    {
        const RHIProbePassLoop& loop = *(const RHIProbePassLoop*)context;
        if (!loop.texture)
        {
            return -1;
        }
        if (loop.batch_textures && batch.mTextureList.size() > 1)
        {
            if (batch.mTextureList.size() > LLRHI::ProbeList::MAX_UNITS)
            {
                return -1;
            }
            for (const LLPointer<LLViewerTexture>& texture : batch.mTextureList)
            {
                if (texture.isNull())
                {
                    return -1;      // the draw keeps the unit's binding
                }
                if (!rhi_probe_name(texture.get(), out))
                {
                    return 0;
                }
            }
            return 1;
        }
        return batch.mTexture.isNull() || rhi_probe_name(batch.mTexture.get(), out) ? 1 : 0;
    }

    // pushGLTFBatch.
    void rhi_probe_gltf_body(LLDrawInfo& batch, void*)
    {
        LLRenderPass::pushGLTFBatch(batch);
    }

    // What a GLTF material binds (bindDirect), each from the material's texture
    // slot it was in; the normal map only while its discard level is at most 4.
    S32 rhi_probe_gltf_textures(LLDrawInfo& batch, void*, std::vector<LLPipeline::RHIProbeTexture>& out)
    {
        LLFetchedGLTFMaterial* mat = batch.mGLTFMaterial.get();
        if (!mat || batch.mTexture.notNull())
        {
            return -1;      // no material, or a media texture (it changes)
        }
        LLViewerTexture* textures[LLFetchedGLTFMaterial::MAX_DIRECT_TEXTURES] = {};
        const U32 count = mat->getDirectTextures(nullptr, textures);
        const LLPointer<LLViewerFetchedTexture>* sources[] = { &mat->mBaseColorTexture, &mat->mNormalTexture,
                                                               &mat->mMetallicRoughnessTexture, &mat->mEmissiveTexture };
        for (U32 i = 0; i < count; ++i)
        {
            if (!rhi_probe_name(textures[i], out))
            {
                return 0;
            }
            LLPipeline::RHIProbeTexture& t = out.back();
            if (i < sizeof(sources) / sizeof(sources[0]))
            {
                t.source = sources[i];
                t.held = sources[i]->get();
            }
            if (i == 1 && mat->mNormalTexture.notNull())
            {
                t.choice = mat->mNormalTexture.get();
                t.chosen = mat->mNormalTexture->getDiscardLevel() <= 4;
            }
        }
        return 1;
    }
}

void LLRenderPass::pushBatches(U32 type, bool texture, bool batch_textures)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    // <plus> research/197 stage 2 (research/198 3.3): the retained scene's records of the pass first (verifying: chosen and compared)
    if (texture)
    {
        // research/198 5.5: batches postSort cut that the scene could not draw (almost always none)
        for (LLDrawInfo* params : gSceneRecords.drawPass(type))
        {
            pushBatch(*params, texture, batch_textures);
        }
    }
    // </plus>
    // LLRHI: a probe face's static batches the render map leaves to the GPU probe
    // list, drawn in their places with the render map (research/46 G5).
    RHIProbePassLoop probe_loop;
    probe_loop.pass = this;
    probe_loop.texture = texture;
    probe_loop.batch_textures = batch_textures;
    if (gPipeline.rhiProbeDraw(type, texture && batch_textures, &rhi_probe_pass_body, &rhi_probe_pass_textures, &probe_loop))
    {
        return;
    }
    if (plus::drawlist::pushBatches(*this, type, texture, batch_textures)) { return; } // LLRHI native (research/137 N2)
    if (texture)
    {
        auto* begin = gPipeline.beginRenderMap(type);
        auto* end = gPipeline.endRenderMap(type);
        LLRHI::TrackerHold tracker_hold(begin != end); // <plus> research/180 9 3: the backend's lock once for the loop </plus>
        for (LLCullResult::drawinfo_iterator i = begin; i != end; )
        {
            LLDrawInfo* pparams = *i;
            LLCullResult::increment_iterator(i, end);

            llassert(pparams); // figure out how null got here, it shouldn't be happening
            if (pparams)
            {
                pushBatch(*pparams, texture, batch_textures);
            }
        }
    }
    else
    {
        pushUntexturedBatches(type);
    }
}

void LLRenderPass::pushUntexturedBatches(U32 type)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    if (plus::drawlist::pushUntexturedBatches(*this, type)) { return; } // LLRHI native (research/137 N2)
    auto* begin = gPipeline.beginRenderMap(type);
    auto* end = gPipeline.endRenderMap(type);
    LLRHI::TrackerHold tracker_hold(begin != end); // <plus> research/180 9 3: the backend's lock once for the loop </plus>
    for (LLCullResult::drawinfo_iterator i = begin; i != end; )
    {
        LLDrawInfo* pparams = *i;
        LLCullResult::increment_iterator(i, end);

        if (pparams)
        {
            pushUntexturedBatch(*pparams);
        }
    }
}

void LLRenderPass::pushRiggedBatches(U32 type, bool texture, bool batch_textures)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    // <plus> research/219: the retained scene's rigged records of the pass first (verifying: chosen and compared)
    if (texture)
    {
        // batches postSort cut that the scene could not draw (almost always none), each with its palette
        const LLVOAvatar* rescue_avatar = nullptr;
        U64 rescue_mesh = 0;
        bool rescue_skip = false;
        for (LLDrawInfo* params : gSceneRecords.drawPass(type))
        {
            if (uploadMatrixPalette(params->mAvatar, params->mSkinInfo, rescue_avatar, rescue_mesh, rescue_skip))
            {
                pushBatch(*params, texture, batch_textures);
            }
        }
    }
    // </plus>
    if (plus::drawlist::pushRiggedBatches(*this, type, texture, batch_textures)) { return; } // LLRHI native (research/137 N2)

    if (texture)
    {
        const LLVOAvatar* lastAvatar = nullptr;
        U64 lastMeshId = 0;
        bool skipLastSkin = false;
        auto* begin = gPipeline.beginRenderMap(type);
        auto* end = gPipeline.endRenderMap(type);
        for (LLCullResult::drawinfo_iterator i = begin; i != end; )
        {
            LLDrawInfo* pparams = *i;
            LLCullResult::increment_iterator(i, end);

            if (pparams && uploadMatrixPalette(pparams->mAvatar, pparams->mSkinInfo, lastAvatar, lastMeshId, skipLastSkin))
            {
                pushBatch(*pparams, texture, batch_textures);
            }
        }
    }
    else
    {
        pushUntexturedRiggedBatches(type);
    }
}

void LLRenderPass::pushUntexturedRiggedBatches(U32 type)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    if (plus::drawlist::pushUntexturedRiggedBatches(*this, type)) { return; } // LLRHI native (research/137 N2)
    const LLVOAvatar* lastAvatar = nullptr;
    U64 lastMeshId = 0;
    bool skipLastSkin = false;
    auto* begin = gPipeline.beginRenderMap(type);
    auto* end = gPipeline.endRenderMap(type);
    for (LLCullResult::drawinfo_iterator i = begin; i != end; )
    {
        LLDrawInfo* pparams = *i;
        LLCullResult::increment_iterator(i, end);

        if (pparams && uploadMatrixPalette(pparams->mAvatar, pparams->mSkinInfo, lastAvatar, lastMeshId, skipLastSkin))
        {
            pushUntexturedBatch(*pparams);
        }
    }
}

void LLRenderPass::pushMaskBatches(U32 type, bool texture, bool batch_textures)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    // <plus> research/197 stage 2 (research/198 3.3): the retained scene's records of the pass first
    if (texture)
    {
        // research/198 5.5, 5.6: batches postSort cut that the scene could not draw (almost always none)
        for (LLDrawInfo* params : gSceneRecords.drawPass(type))
        {
            LLGLSLShader::sCurBoundShaderPtr->setMinimumAlpha(params->mAlphaMaskCutoff);
            pushBatch(*params, texture, batch_textures);
        }
    }
    // </plus>
    // LLRHI: a probe face's static batches the render map leaves to the GPU
    // probe list, drawn in their places with the render map (research/50 step 4).
    RHIProbePassLoop probe_loop;
    probe_loop.pass = this;
    probe_loop.texture = texture;
    probe_loop.batch_textures = batch_textures;
    probe_loop.mask = true;
    if (gPipeline.rhiProbeDraw(type, texture && batch_textures, &rhi_probe_pass_body, &rhi_probe_pass_textures, &probe_loop))
    {
        return;
    }
    if (plus::drawlist::pushMaskBatches(*this, type, texture, batch_textures)) { return; } // LLRHI native (research/137 N2)
    auto* begin = gPipeline.beginRenderMap(type);
    auto* end = gPipeline.endRenderMap(type);
    LLRHI::TrackerHold tracker_hold(begin != end); // <plus> research/180 9 3: the backend's lock once for the loop </plus>
    for (LLCullResult::drawinfo_iterator i = begin; i != end; )
    {
        LLDrawInfo* pparams = *i;
        LLCullResult::increment_iterator(i, end);
        if (pparams)
        {
            LLGLSLShader::sCurBoundShaderPtr->setMinimumAlpha(pparams->mAlphaMaskCutoff);
            pushBatch(*pparams, texture, batch_textures);
        }
    }
}

void LLRenderPass::pushRiggedMaskBatches(U32 type, bool texture, bool batch_textures)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    // <plus> research/219: the retained scene's rigged records of the pass first
    if (texture)
    {
        // batches postSort cut that the scene could not draw (almost always none): cutoff, palette
        const LLVOAvatar* rescue_avatar = nullptr;
        U64 rescue_mesh = 0;
        bool rescue_skip = false;
        for (LLDrawInfo* params : gSceneRecords.drawPass(type))
        {
            LLGLSLShader::sCurBoundShaderPtr->setMinimumAlpha(params->mAlphaMaskCutoff);
            if (uploadMatrixPalette(params->mAvatar, params->mSkinInfo, rescue_avatar, rescue_mesh, rescue_skip))
            {
                pushBatch(*params, texture, batch_textures);
            }
        }
    }
    // </plus>
    if (plus::drawlist::pushRiggedMaskBatches(*this, type, texture, batch_textures)) { return; } // LLRHI native (research/137 N2)
    const LLVOAvatar* lastAvatar = nullptr;
    U64 lastMeshId = 0;
    bool skipLastSkin = false;
    auto* begin = gPipeline.beginRenderMap(type);
    auto* end = gPipeline.endRenderMap(type);
    for (LLCullResult::drawinfo_iterator i = begin; i != end; )
    {
        LLDrawInfo* pparams = *i;

        LLCullResult::increment_iterator(i, end);

        llassert(pparams); // figure out how null got here, it shouldn't be happening

        if (pparams)
        {
            LLGLSLShader::sCurBoundShaderPtr->setMinimumAlpha(pparams->mAlphaMaskCutoff);

            if (uploadMatrixPalette(pparams->mAvatar, pparams->mSkinInfo, lastAvatar, lastMeshId, skipLastSkin))
            {
                pushBatch(*pparams, texture, batch_textures);
            }
        }
    }
}

void LLRenderPass::applyModelMatrix(const LLDrawInfo& params)
{
    applyModelMatrix(params.mModelMatrix);
}

void LLRenderPass::applyModelMatrix(const LLMatrix4* model_matrix)
{
    if (model_matrix != gGLLastMatrix)
    {
        gGLLastMatrix = model_matrix;
        gGL.matrixMode(LLRender::MM_MODELVIEW);
        gGL.loadMatrix(gGLModelView);
        if (model_matrix)
        {
            gGL.multMatrix((GLfloat*) model_matrix->mMatrix);
        }
        gPipeline.mMatrixOpCount++;
    }
}

bool LLRenderPass::pushDirectBatch(LLDrawInfo& params, bool texture, bool batch_textures, const U32* unit_textures, U32 unit_count,
                                   bool texture_matrix)
{
    if (LLRHI::sAvatarStats && params.mAvatar.notNull())
    {
        rhiCountRigged(params);     // research/56: every rigged batch of the draw pools comes here, direct or not
    }
    LLGLSLShader* shader = LLGLSLShader::sCurBoundShaderPtr;
    LLVertexBuffer* vb = params.mVertexBuffer;
    if (!LLRHI::sDirect || !LLRHI::sDirectDraw || !shader || !vb || vb->isMapped())
    {
        return false;
    }
    // <plus> skin mark (plus/skinsss): a batch of an avatar's skin writes the skin stencil bit
    // (after the refusal above: a refused batch gets its mark from the caller's scope)
    plus::skinsss::BatchScope skin_mark(params);
    // </plus>
    // Projection, lights and the pass's modelview reach the shader as through
    // GL (nothing to do after the first batch: gGL does not change here).
    gGL.syncMatrices();
    LLRHI::DirectDraw draw;

    // Textures as pushBatch binds them (units without one keep their binding).
    constexpr U32 MAX_UNITS = 32;
    U32 textures[MAX_UNITS];
    if (unit_textures)
    {
        // The caller's textures per unit (a GLTF material's), with the texture
        // matrix setup_texture_matrix would load.
        draw.texture_count = unit_count;
        draw.textures = unit_textures;
        if (texture_matrix && params.mTextureMatrix)
        {
            draw.texture_matrix0 = (const F32*)params.mTextureMatrix->mMatrix;
        }
    }
    else if (texture)
    {
        if (batch_textures && params.mTextureList.size() > 1)
        {
            draw.texture_count = (U32)llmin(params.mTextureList.size(), (size_t)MAX_UNITS);
            for (U32 i = 0; i < draw.texture_count; ++i)
            {
                textures[i] = params.mTextureList[i].notNull() ? gGL.getTexUnit(i)->directTexture(params.mTextureList[i])
                                                               : LLRHI::DirectDraw::KEEP_UNIT;
            }
        }
        else
        {
            draw.texture_count = 1;
            if (params.mTexture.notNull())
            {
                textures[0] = gGL.getTexUnit(0)->directTexture(params.mTexture);
                if (params.mTextureMatrix)
                {
                    draw.texture_matrix0 = (const F32*)params.mTextureMatrix->mMatrix;
                }
            }
            else
            {
                textures[0] = 0;    // unbound, as unbindFast leaves it
            }
        }
        draw.textures = textures;
    }

    draw.vertex_buffer = vb->getGLBuffer();
    draw.index_buffer = vb->getGLIndices();
    draw.index_type = vb->getIndicesType();
    draw.first_index = params.mOffset;
    draw.index_count = params.mCount;
    // The whole buffers, for the backend's attribute pool (LLRHI::sDrawRuns).
    draw.buffer_vertices = vb->getNumVerts();
    draw.buffer_indices = vb->getNumIndices();
    draw.vb = vb->rhiDrawNumber(); // LLRHI (research/197 1b): the pair by its number

    // The matrices applyModelMatrix and syncMatrices would set. Most batches have
    // no model matrix of their own and share the pass's: computed once while the
    // model matrix, view, projection and shader stay the same.
    struct MatrixCache
    {
        U32 frame = U32_MAX;
        const LLMatrix4* model = nullptr;
        const LLGLSLShader* shader = nullptr;
        F32 view[16] = {};
        glm::mat4 projection;
        LLRender::DirectMatrices matrices;
        LLRHI::DirectDraw pointers;     // only its matrix pointers
        U32 id = 0;
    };
    static MatrixCache cache;
    const glm::mat4& projection = gGL.getProjectionMatrix();
    // Bitwise, as memcmp would compare, but inline (memcmp is a call per batch).
    auto same_view = [&]()
    {
        for (U32 i = 0; i < 16; ++i)
        {
            U32 a, b;
            memcpy(&a, &cache.view[i], 4);
            memcpy(&b, &gGLModelView[i], 4);
            if (a != b)
            {
                return false;
            }
        }
        return true;
    };
    if (cache.frame != gFrameCount || cache.model != params.mModelMatrix || cache.shader != shader ||
        !same_view() || cache.projection != projection)
    {
        cache.frame = gFrameCount;
        cache.model = params.mModelMatrix;
        cache.shader = shader;
        memcpy(cache.view, gGLModelView, sizeof(cache.view));
        cache.projection = projection;
        glm::mat4 modelview = glm::make_mat4(gGLModelView);
        if (params.mModelMatrix)
        {
            modelview *= glm::make_mat4((const F32*)params.mModelMatrix->mMatrix);
        }
        cache.pointers = LLRHI::DirectDraw();
        gGL.fillDirectMatrices(modelview, cache.matrices, cache.pointers);
        cache.id = LLRHI::DrawList::newMatrixId(); // LLRHI native (research/137 N2): one id source with the draw lists
    }
    draw.modelview = cache.pointers.modelview;
    draw.modelview_projection = cache.pointers.modelview_projection;
    draw.normal = cache.pointers.normal;
    draw.inverse_modelview = cache.pointers.inverse_modelview;
    draw.matrix_id = cache.id;

    // A batch that joined a draw run before goes by its template if the run
    // open now may take it (research/46 G4c): no attribute layout, and the
    // backend skips the rest of the draw path. Else the usual way, which
    // makes or updates the template.
    if (LLRHI::sDrawTemplates && LLRHI::sDrawRuns && LLRHI::sDrawTemplate)
    {
        draw.draw_template = &params.mRHIDrawTemplate;
        if (params.mRHIDrawTemplate && LLRHI::sDrawTemplate(draw))
        {
            return true;
        }
    }

    // Vertex data as setBuffer() lays it out for the bound shader.
    LLRHI::DirectDraw::Attribute attributes[LLRHI::DirectDraw::MAX_ATTRIBUTES];
    draw.attribute_count = vb->getAttributes(shader->mAttributeMask, attributes);
    draw.attributes = attributes;

    return LLRHI::sDirectDraw(draw);
}

// static
void LLRenderPass::drawGeometry(LLDrawInfo& params)
{
    GrayWire::drawBatch(params, [&]() { // <plus> research/256: GrayWire's Scope, and twice while the materialize band sweeps it </plus>
    const bool direct = pushDirectBatch(params, false, false);
    if (direct)     // <plus> research/207 6.3 </plus>
    {
        return;
    }
    plus::skinsss::BatchScope skin_mark(params, !direct); // <plus> skin mark on the GL draw of a refused batch </plus>

    // Callers that set a texture matrix are still in its matrix mode.
    const LLRender::eMatrixMode mode = gGL.getMatrixMode();
    applyModelMatrix(params);
    gGL.matrixMode(mode);
    params.mVertexBuffer->setBuffer();
    params.mVertexBuffer->drawRange(LLRender::TRIANGLES, params.mStart, params.mEnd, params.mCount, params.mOffset);
    }); // <plus> research/256 </plus>
}

void LLRenderPass::pushBatch(LLDrawInfo& params, bool texture, bool batch_textures)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    llassert(texture);

    if (!params.mCount)
    {
        return;
    }

    GrayWire::drawBatch(params, [&]() { // <plus> a batch with a texture not loaded yet (or of such an avatar) is a wireframe, not gray; research/256: twice while the materialize band sweeps it </plus>
    const bool direct = pushDirectBatch(params, true, batch_textures);
    if (direct)     // <plus> research/207 6.3 </plus>
    {
        return;
    }
    plus::skinsss::BatchScope skin_mark(params, !direct); // <plus> skin mark on the GL draw of a refused batch </plus>

    applyModelMatrix(params);

    bool tex_setup = false;

    {
        if (batch_textures && params.mTextureList.size() > 1)
        {
            for (U32 i = 0; i < params.mTextureList.size(); ++i)
            {
                if (params.mTextureList[i].notNull())
                {
                    gGL.getTexUnit(i)->bindFast(params.mTextureList[i]);
                }
            }
        }
        else
        { //not batching textures or batch has only 1 texture -- might need a texture matrix
            if (params.mTexture.notNull())
            {
                gGL.getTexUnit(0)->bindFast(params.mTexture);
                if (params.mTextureMatrix)
                {
                    tex_setup = true;
                    gGL.getTexUnit(0)->activate();
                    gGL.matrixMode(LLRender::MM_TEXTURE);
                    gGL.loadMatrix((GLfloat*) params.mTextureMatrix->mMatrix);
                    gPipeline.mTextureMatrixOps++;
                }
            }
            else
            {
                gGL.getTexUnit(0)->unbindFast(LLTexUnit::TT_TEXTURE);
            }
        }
    }

    params.mVertexBuffer->setBuffer();
    params.mVertexBuffer->drawRange(LLRender::TRIANGLES, params.mStart, params.mEnd, params.mCount, params.mOffset);

    if (tex_setup)
    {
        gGL.matrixMode(LLRender::MM_TEXTURE0);
        gGL.loadIdentity();
        gGL.matrixMode(LLRender::MM_MODELVIEW);
    }
    }); // <plus> research/256 </plus>
}

void LLRenderPass::pushUntexturedBatch(LLDrawInfo& params)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;

    if (!params.mCount)
    {
        return;
    }

    GrayWire::drawBatch(params, [&]() { // <plus> research/256: GrayWire's Scope, and twice while the materialize band sweeps it </plus>
    const bool direct = pushDirectBatch(params, false, false);
    if (direct)     // <plus> research/207 6.3 </plus>
    {
        return;
    }
    plus::skinsss::BatchScope skin_mark(params, !direct); // <plus> skin mark on the GL draw of a refused batch </plus>

    applyModelMatrix(params);

    params.mVertexBuffer->setBuffer();
    params.mVertexBuffer->drawRange(LLRender::TRIANGLES, params.mStart, params.mEnd, params.mCount, params.mOffset);
    }); // <plus> research/256 </plus>
}

extern bool gCubeSnapshot;

// static
void LLRenderPass::rhiCountRigged(const LLDrawInfo& params)
{
    using Counters = LLRHI::AvatarCounters;
    U32 pass = Counters::PASS_WORLD;
    const U32 camera = (U32)LLViewerCamera::sCurCameraID;
    const bool shadow = camera >= LLViewerCamera::CAMERA_SUN_SHADOW0 && camera <= LLViewerCamera::CAMERA_SPOT_SHADOW1;
    if (LLPipeline::sImpostorRender)
    {
        pass = Counters::PASS_IMPOSTOR;
    }
    else if (gCubeSnapshot)
    {
        pass = gPipeline.mHeroProbeManager.isMirrorPass() ? Counters::PASS_MIRROR : shadow ? Counters::PASS_PROBE_SHADOW : Counters::PASS_PROBE;
    }
    else if (shadow)
    {
        pass = Counters::PASS_SUN0 + (camera - LLViewerCamera::CAMERA_SUN_SHADOW0);   // the spot cameras follow the sun's, as the passes do
    }
    else if (camera == LLViewerCamera::CAMERA_WATER0 || camera == LLViewerCamera::CAMERA_WATER1)
    {
        pass = Counters::PASS_WATER;
    }
    Counters& c = LLRHI::sAvatarCounters;
    ++c.rigged_draws[pass];
    c.skinned_vertices[pass] += params.mEnd >= params.mStart ? (U64)(params.mEnd - params.mStart + 1) : 0;
}

// static
bool LLRenderPass::rhiPaletteRow(const std::vector<F32>& gl_mp, LLRHI::PaletteRow& row)
{
    if (!LLRHI::sPlacePalette || gl_mp.empty())
    {
        return false;
    }
    // <plus> skin once (plus/skinonce): the row holds the backend's answer for this build already, not asked again
    if (plus::skinonce::keepRow(row))
    {
        return row.room != 0;
    }
    // </plus>
    if (!LLRHI::sAvatarStats)
    {
        return LLRHI::sPlacePalette(gl_mp.data(), (U32)(gl_mp.size() / 4), row);
    }
    const LLRHI::PaletteRow before = row;
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    const bool ok = LLRHI::sPlacePalette(gl_mp.data(), (U32)(gl_mp.size() / 4), row);
    if (ok && (row.frame != before.frame || row.base != before.base))
    {
        LLRHI::AvatarCounters& c = LLRHI::sAvatarCounters;
        ++c.palette_places;
        c.palette_place_ns += (U64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
    }
    return ok;
}

namespace
{
    // The bound program's skinning palette: the palette's row in the backend's
    // frame table when it can take one (LLRHI::sPalettePrebuild, research/56),
    // else the joints as its uniform, as upstream sets them.
    void upload_palette(const LLVOAvatar::MatrixPaletteCache& mpc, U32 count)
    {
        LLGLSLShader* shader = LLGLSLShader::sCurBoundShaderPtr;
        // <plus> hair sway (plus/hairsway): where this palette's sway entries are, 0: none
        plus::hairsway::bindShader(shader, mpc.mMatrixPalette);
        // </plus>
        if (LLRHI::sPalettePrebuild && LLRHI::sUsePaletteRow && shader->mNativeProgram &&
            LLRenderPass::rhiPaletteRow(mpc.mGLMp, mpc.mRHIRow) && LLRHI::sUsePaletteRow(shader->mNativeProgram, mpc.mRHIRow))
        {
            if (LLRHI::sAvatarStats)
            {
                ++LLRHI::sAvatarCounters.palette_row_uses;
            }
            return;
        }
        if (LLRHI::sAvatarStats)
        {
            ++LLRHI::sAvatarCounters.palette_uploads;
        }
        shader->uniformMatrix3x4fv(LLViewerShaderMgr::AVATAR_MATRIX,
            count,
            false,
            (GLfloat*)&(mpc.mGLMp[0]));
    }
}

// static
bool LLRenderPass::uploadMatrixPalette(LLDrawInfo& params)
{
    // upload matrix palette to shader
    return uploadMatrixPalette(params.mAvatar, params.mSkinInfo);
}

//static
bool LLRenderPass::uploadMatrixPalette(LLVOAvatar* avatar, LLMeshSkinInfo* skinInfo)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;

    if (!avatar)
    {
        return false;
    }
    const LLVOAvatar::MatrixPaletteCache& mpc = avatar->updateSkinInfoMatrixPalette(skinInfo);
    U32 count = static_cast<U32>(mpc.mMatrixPalette.size());

    if (count == 0)
    {
        //skin info not loaded yet, don't render
        return false;
    }

    upload_palette(mpc, count);

    return true;
}

// Returns true if rendering should proceed
//static
bool LLRenderPass::uploadMatrixPalette(LLVOAvatar* avatar, LLMeshSkinInfo* skinInfo, const LLVOAvatar*& lastAvatar, U64& lastMeshId, bool& skipLastSkin)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;

    llassert(skinInfo);
    llassert(LLGLSLShader::sCurBoundShaderPtr);

    if (!avatar)
    {
        return false;
    }

    if (avatar == lastAvatar && skinInfo->mHash == lastMeshId)
    {
        return !skipLastSkin;
    }

    const LLVOAvatar::MatrixPaletteCache& mpc = avatar->updateSkinInfoMatrixPalette(skinInfo);
    U32 count = static_cast<U32>(mpc.mMatrixPalette.size());
    // skipLastSkin -> skin info not loaded yet, don't render
    skipLastSkin = !bool(count);
    lastAvatar = avatar;
    lastMeshId = skinInfo->mHash;

    if (!skipLastSkin)
    {
        upload_palette(mpc, count);
    }

    return !skipLastSkin;
}

// Returns true if rendering should proceed
//static
bool LLRenderPass::uploadMatrixPalette(LLVOAvatar* avatar, LLMeshSkinInfo* skinInfo, const LLVOAvatar*& lastAvatar, U64& lastMeshId, const LLGLSLShader*& lastAvatarShader, bool& skipLastSkin)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;

    llassert(skinInfo);
    llassert(LLGLSLShader::sCurBoundShaderPtr);

    if (!avatar)
    {
        return false;
    }

    if (avatar == lastAvatar && skinInfo->mHash == lastMeshId && lastAvatarShader == LLGLSLShader::sCurBoundShaderPtr)
    {
        return !skipLastSkin;
    }

    const LLVOAvatar::MatrixPaletteCache& mpc = avatar->updateSkinInfoMatrixPalette(skinInfo);
    U32 count = static_cast<U32>(mpc.mMatrixPalette.size());
    // skipLastSkin -> skin info not loaded yet, don't render
    skipLastSkin = !bool(count);
    lastAvatar = avatar;
    lastMeshId = skinInfo->mHash;
    lastAvatarShader = LLGLSLShader::sCurBoundShaderPtr;

    if (!skipLastSkin)
    {
        upload_palette(mpc, count);
    }

    return !skipLastSkin;
}

void setup_texture_matrix(LLDrawInfo& params)
{
    if (params.mTextureMatrix)
    { //special case implementation of texture animation here because of special handling of textures for PBR batches
        gGL.getTexUnit(0)->activate();
        gGL.matrixMode(LLRender::MM_TEXTURE);
        gGL.loadMatrix((GLfloat*)params.mTextureMatrix->mMatrix);
        gPipeline.mTextureMatrixOps++;
    }
}

void teardown_texture_matrix(LLDrawInfo& params)
{
    if (params.mTextureMatrix)
    {
        gGL.matrixMode(LLRender::MM_TEXTURE0);
        gGL.loadIdentity();
        gGL.matrixMode(LLRender::MM_MODELVIEW);
    }
}

void LLRenderPass::pushGLTFBatches(U32 type, bool textured)
{
    if (textured)
    {
        pushGLTFBatches(type);
    }
    else
    {
        pushUntexturedGLTFBatches(type);
    }
}

void LLRenderPass::pushGLTFBatches(U32 type)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    // LLRHI: a probe face's static batches the render map leaves to the GPU
    // probe list, drawn in their places with the render map (research/50 step 4).
    if (gPipeline.rhiProbeDraw(type, true, &rhi_probe_gltf_body, &rhi_probe_gltf_textures, nullptr))
    {
        return;
    }
    if (plus::drawlist::pushGLTFBatches(type)) { return; } // LLRHI native (research/137 N2)
    auto* begin = gPipeline.beginRenderMap(type);
    auto* end = gPipeline.endRenderMap(type);
    for (LLCullResult::drawinfo_iterator i = begin; i != end; )
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DRAWPOOL("pushGLTFBatch");
        LLDrawInfo& params = **i;
        LLCullResult::increment_iterator(i, end);

        pushGLTFBatch(params);
    }
}

void LLRenderPass::pushUntexturedGLTFBatches(U32 type)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    if (plus::drawlist::pushUntexturedGLTFBatches(type)) { return; } // LLRHI native (research/137 N2)
    auto* begin = gPipeline.beginRenderMap(type);
    auto* end = gPipeline.endRenderMap(type);
    for (LLCullResult::drawinfo_iterator i = begin; i != end; )
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DRAWPOOL("pushGLTFBatch");
        LLDrawInfo& params = **i;
        LLCullResult::increment_iterator(i, end);

        pushUntexturedGLTFBatch(params);
    }
}

// static
void LLRenderPass::pushGLTFBatch(LLDrawInfo& params)
{
    auto& mat = params.mGLTFMaterial;
    // <plus> research/243: a base colour not loaded yet is a wireframe here too (the direct draw below);
    // research/256: twice while the materialize band sweeps it (drawGeometry inside draws once then)
    GrayWire::drawBatch(params, [&]() {
    // </plus>

    // LLRHI: the material's textures (and the texture matrix) go with a direct
    // draw instead of being bound through GL.
    const bool direct = mat.notNull() && LLRHI::sDirect && LLRHI::sDirectDraw;     // <plus> research/207 6.3 </plus>
    constexpr U32 MAX_UNITS = 32;
    U32 textures[MAX_UNITS];
    U32 texture_count = 0;
    if (direct)
    {
        texture_count = mat->bindDirect(params.mTexture, textures, MAX_UNITS);
    }
    else if (mat.notNull())
    {
        mat->bind(params.mTexture);
    }

    LLGLDisable cull_face(mat.notNull() && mat->mDoubleSided ? GL_CULL_FACE : 0);

    if (direct)
    {
        if (pushDirectBatch(params, false, false, textures, texture_count))
        {
            return;
        }
        mat->bindTextures(params.mTexture);     // refused: through GL as bind() would
    }

    setup_texture_matrix(params);

    drawGeometry(params);

    teardown_texture_matrix(params);
    }); // <plus> research/256 </plus>
}

// static
void LLRenderPass::pushUntexturedGLTFBatch(LLDrawInfo& params)
{
    auto& mat = params.mGLTFMaterial;

    LLGLDisable cull_face(mat->mDoubleSided ? GL_CULL_FACE : 0);

    drawGeometry(params);
}

void LLRenderPass::pushRiggedGLTFBatches(U32 type, bool textured)
{
    if (textured)
    {
        pushRiggedGLTFBatches(type);
    }
    else
    {
        pushUntexturedRiggedGLTFBatches(type);
    }
}

void LLRenderPass::pushRiggedGLTFBatches(U32 type)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    if (plus::drawlist::pushRiggedGLTFBatches(type)) { return; } // LLRHI native (research/137 N2)
    const LLVOAvatar* lastAvatar = nullptr;
    U64 lastMeshId = 0;
    bool skipLastSkin = false;

    auto* begin = gPipeline.beginRenderMap(type);
    auto* end = gPipeline.endRenderMap(type);
    for (LLCullResult::drawinfo_iterator i = begin; i != end; )
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DRAWPOOL("pushRiggedGLTFBatch");
        LLDrawInfo& params = **i;
        LLCullResult::increment_iterator(i, end);

        pushRiggedGLTFBatch(params, lastAvatar, lastMeshId, skipLastSkin);
    }
}

void LLRenderPass::pushUntexturedRiggedGLTFBatches(U32 type)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    if (plus::drawlist::pushUntexturedRiggedGLTFBatches(type)) { return; } // LLRHI native (research/137 N2)
    const LLVOAvatar* lastAvatar = nullptr;
    U64 lastMeshId = 0;
    bool skipLastSkin = false;

    auto* begin = gPipeline.beginRenderMap(type);
    auto* end = gPipeline.endRenderMap(type);
    for (LLCullResult::drawinfo_iterator i = begin; i != end; )
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DRAWPOOL("pushRiggedGLTFBatch");
        LLDrawInfo& params = **i;
        LLCullResult::increment_iterator(i, end);

        pushUntexturedRiggedGLTFBatch(params, lastAvatar, lastMeshId, skipLastSkin);
    }
}


// static
void LLRenderPass::pushRiggedGLTFBatch(LLDrawInfo& params, const LLVOAvatar*& lastAvatar, U64& lastMeshId, bool& skipLastSkin)
{
    if (uploadMatrixPalette(params.mAvatar, params.mSkinInfo, lastAvatar, lastMeshId, skipLastSkin))
    {
        pushGLTFBatch(params);
    }
}

// static
void LLRenderPass::pushUntexturedRiggedGLTFBatch(LLDrawInfo& params, const LLVOAvatar*& lastAvatar, U64& lastMeshId, bool& skipLastSkin)
{
    if (uploadMatrixPalette(params.mAvatar, params.mSkinInfo, lastAvatar, lastMeshId, skipLastSkin))
    {
        pushUntexturedGLTFBatch(params);
    }
}


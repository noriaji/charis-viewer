/**
 * @file lldrawpool.cpp
 * @brief LLDrawPoolMaterials class implementation
 * @author Jonathan "Geenz" Goodman
 *
 * $LicenseInfo:firstyear=2002&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2013, Linden Research, Inc.
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

#include "lldrawpoolmaterials.h"
#include "llviewershadermgr.h"
#include "pipeline.h"
#include "llgraywire.h" // <plus> research/243 </plus>
#include "llglcommonfunc.h"
#include "llvoavatar.h"
#include "plus/materiallist/materiallist.h" // LLRHI native (research/137 N4)
#include "llrhi.h" // <plus> LLRHI::TrackerHold (research/180 9 3) </plus>
#include "llscenerecords.h" // <plus> research/197 stage 2 </plus>

LLDrawPoolMaterials::LLDrawPoolMaterials()
:  LLRenderPass(LLDrawPool::POOL_MATERIALS)
{

}

void LLDrawPoolMaterials::prerender()
{
    mShaderLevel = LLViewerShaderMgr::instance()->getShaderLevel(LLViewerShaderMgr::SHADER_OBJECT);
}

S32 LLDrawPoolMaterials::getNumDeferredPasses()
{
    // 12 render passes times 2 (one for each rigged and non rigged)
    return 12*2;
}

void LLDrawPoolMaterials::beginDeferredPass(S32 pass)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_MATERIAL;

    bool rigged = false;
    if (pass >= 12)
    {
        rigged = true;
        pass -= 12;
    }
    U32 shader_idx[] =
    {
        0, //LLRenderPass::PASS_MATERIAL,
        //1, //LLRenderPass::PASS_MATERIAL_ALPHA,
        2, //LLRenderPass::PASS_MATERIAL_ALPHA_MASK,
        3, //LLRenderPass::PASS_MATERIAL_ALPHA_GLOW,
        4, //LLRenderPass::PASS_SPECMAP,
        //5, //LLRenderPass::PASS_SPECMAP_BLEND,
        6, //LLRenderPass::PASS_SPECMAP_MASK,
        7, //LLRenderPass::PASS_SPECMAP_GLOW,
        8, //LLRenderPass::PASS_NORMMAP,
        //9, //LLRenderPass::PASS_NORMMAP_BLEND,
        10, //LLRenderPass::PASS_NORMMAP_MASK,
        11, //LLRenderPass::PASS_NORMMAP_GLOW,
        12, //LLRenderPass::PASS_NORMSPEC,
        //13, //LLRenderPass::PASS_NORMSPEC_BLEND,
        14, //LLRenderPass::PASS_NORMSPEC_MASK,
        15, //LLRenderPass::PASS_NORMSPEC_GLOW,
    };

    U32 idx = shader_idx[pass];

    mShader = &(gDeferredMaterialProgram[idx]);

    if (rigged)
    {
        llassert(mShader->mRiggedVariant != nullptr);
        mShader = mShader->mRiggedVariant;
    }

    gPipeline.bindDeferredShader(*mShader);
}

void LLDrawPoolMaterials::endDeferredPass(S32 pass)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_MATERIAL;

    mShader->unbind();

    LLRenderPass::endRenderPass(pass);
}

namespace
{
    // LLRHI (research/50 step 4): one batch of the loop below as the GPU probe
    // list draws or registers it, every value set (the loop sets only what
    // changed since the batch before: the same state after each batch).
    struct RHIProbeMaterialLoop
    {
        LLGLSLShader* shader = nullptr;
        GLint intensity = -1, brightness = -1, min_alpha = -1, specular = -1;
        GLint diffuse_channel = -1, spec_channel = -1, norm_channel = -1;
    };

    void rhi_probe_material_body(LLDrawInfo& params, void* context)
    {
        const RHIProbeMaterialLoop& loop = *(const RHIProbeMaterialLoop*)context;
        if (loop.specular > -1)
        {
            loop.shader->uniform4fv(LLShaderMgr::SPECULAR_COLOR, 1, params.mSpecColor.mV);
        }
        if (loop.intensity > -1)
        {
            loop.shader->uniform1f(LLShaderMgr::ENVIRONMENT_INTENSITY, params.mEnvIntensity);
        }
        if (loop.min_alpha > -1)
        {
            loop.shader->uniform1f(LLShaderMgr::MINIMUM_ALPHA, params.mAlphaMaskCutoff);
        }
        if (loop.brightness > -1)
        {
            loop.shader->uniform1f(LLShaderMgr::EMISSIVE_BRIGHTNESS, params.mFullbright ? 1.f : 0.f);
        }
        if (loop.norm_channel > -1 && params.mNormalMap.notNull())
        {
            gGL.getTexUnit(loop.norm_channel)->bindFast(params.mNormalMap);
        }
        if (loop.spec_channel > -1 && params.mSpecularMap.notNull())
        {
            gGL.getTexUnit(loop.spec_channel)->bindFast(params.mSpecularMap);
        }
        if (params.mTexture.notNull())
        {
            gGL.getTexUnit(loop.diffuse_channel)->bindFast(params.mTexture);
        }
        else
        {
            gGL.getTexUnit(loop.diffuse_channel)->unbindFast(LLTexUnit::TT_TEXTURE);
        }
        // The texture animation, as the loop loads and resets it (the list
        // never takes such batches, but the render map's go through here too).
        const bool tex_setup = params.mTextureMatrix != nullptr;
        if (tex_setup)
        {
            gGL.getTexUnit(0)->activate();
            gGL.matrixMode(LLRender::MM_TEXTURE);
            gGL.loadMatrix((GLfloat*)params.mTextureMatrix->mMatrix);
            gPipeline.mTextureMatrixOps++;
        }
        LLRenderPass::drawGeometry(params);
        if (tex_setup)
        {
            gGL.getTexUnit(0)->activate();
            gGL.loadIdentity();
            gGL.matrixMode(LLRender::MM_MODELVIEW);
        }
    }

    // The textures the loop binds for the batch: diffuse (or none), and the
    // normal and specular maps the program samples (which it needs).
    S32 rhi_probe_material_textures(LLDrawInfo& params, void* context, std::vector<LLPipeline::RHIProbeTexture>& out)
    {
        const RHIProbeMaterialLoop& loop = *(const RHIProbeMaterialLoop*)context;
        if ((loop.norm_channel > -1 && params.mNormalMap.isNull()) || (loop.spec_channel > -1 && params.mSpecularMap.isNull()))
        {
            return -1;      // the loop would keep the unit's binding
        }
        auto add = [&out](LLViewerTexture* texture)
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
        };
        if ((params.mTexture.notNull() && !add(params.mTexture.get())) || (loop.norm_channel > -1 && !add(params.mNormalMap.get())) ||
            (loop.spec_channel > -1 && !add(params.mSpecularMap.get())))
        {
            return 0;
        }
        return 1;
    }
}

void LLDrawPoolMaterials::renderDeferred(S32 pass)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_MATERIAL;
    static const U32 type_list[] =
    {
        LLRenderPass::PASS_MATERIAL,
        //LLRenderPass::PASS_MATERIAL_ALPHA,
        LLRenderPass::PASS_MATERIAL_ALPHA_MASK,
        LLRenderPass::PASS_MATERIAL_ALPHA_EMISSIVE,
        LLRenderPass::PASS_SPECMAP,
        //LLRenderPass::PASS_SPECMAP_BLEND,
        LLRenderPass::PASS_SPECMAP_MASK,
        LLRenderPass::PASS_SPECMAP_EMISSIVE,
        LLRenderPass::PASS_NORMMAP,
        //LLRenderPass::PASS_NORMMAP_BLEND,
        LLRenderPass::PASS_NORMMAP_MASK,
        LLRenderPass::PASS_NORMMAP_EMISSIVE,
        LLRenderPass::PASS_NORMSPEC,
        //LLRenderPass::PASS_NORMSPEC_BLEND,
        LLRenderPass::PASS_NORMSPEC_MASK,
        LLRenderPass::PASS_NORMSPEC_EMISSIVE,
    };

    bool rigged = false;
    if (pass >= 12)
    {
        rigged = true;
        pass -= 12;
    }

    llassert(pass < sizeof(type_list)/sizeof(U32));

    U32 type = type_list[pass];
    if (rigged)
    {
        type += 1;
    }

    LLCullResult::drawinfo_iterator begin = gPipeline.beginRenderMap(type);
    LLCullResult::drawinfo_iterator end = gPipeline.endRenderMap(type);

    F32 lastIntensity = 0.f;
    F32 lastFullbright = 0.f;
    F32 lastMinimumAlpha = 0.f;
    LLVector4 lastSpecular = LLVector4(0, 0, 0, 0);

    GLint intensity = mShader->getUniformLocation(LLShaderMgr::ENVIRONMENT_INTENSITY);
    GLint brightness = mShader->getUniformLocation(LLShaderMgr::EMISSIVE_BRIGHTNESS);
    GLint minAlpha = mShader->getUniformLocation(LLShaderMgr::MINIMUM_ALPHA);
    GLint specular = mShader->getUniformLocation(LLShaderMgr::SPECULAR_COLOR);
    // LLRHI: values go through LLGLSLShader (native uniforms in Vulkan mode, and
    // its value cache stays true); the locations above only say which exist.

    GLint diffuseChannel = mShader->enableTexture(LLShaderMgr::DIFFUSE_MAP);
    GLint specChannel = mShader->enableTexture(LLShaderMgr::SPECULAR_MAP);
    GLint normChannel = mShader->enableTexture(LLShaderMgr::BUMP_MAP);

    // <plus> research/219: the retained scene's rigged records of the pass first; batches postSort
    // cut that the scene could not draw (almost always none), every value set, each with its palette
    if (rigged)
    {
        RHIProbeMaterialLoop rescue_loop;
        rescue_loop.shader = mShader;
        rescue_loop.intensity = intensity;
        rescue_loop.brightness = brightness;
        rescue_loop.min_alpha = minAlpha;
        rescue_loop.specular = specular;
        rescue_loop.diffuse_channel = diffuseChannel;
        rescue_loop.spec_channel = specChannel;
        rescue_loop.norm_channel = normChannel;
        const LLVOAvatar* rescue_avatar = nullptr;
        U64 rescue_mesh = 0;
        bool rescue_skip = false;
        for (LLDrawInfo* params : gSceneRecords.drawPass(type))
        {
            if (uploadMatrixPalette(params->mAvatar, params->mSkinInfo, rescue_avatar, rescue_mesh, rescue_skip))
            {
                rhi_probe_material_body(*params, &rescue_loop);
            }
        }
    }
    // </plus>
    // LLRHI: a probe face's static batches the render map leaves to the GPU
    // probe list, drawn in their places with the render map (research/50 step
    // 4), before the loop sets its first values.
    if (!rigged)
    {
        RHIProbeMaterialLoop probe_loop;
        probe_loop.shader = mShader;
        probe_loop.intensity = intensity;
        probe_loop.brightness = brightness;
        probe_loop.min_alpha = minAlpha;
        probe_loop.specular = specular;
        probe_loop.diffuse_channel = diffuseChannel;
        probe_loop.spec_channel = specChannel;
        probe_loop.norm_channel = normChannel;
        // <plus> research/197 stage 2 (research/198 3.3): the retained scene's records of the pass first (not rigged)
        // research/198 5.6: batches postSort cut that the scene could not draw (almost always none), every value set
        for (LLDrawInfo* params : gSceneRecords.drawPass(type))
        {
            rhi_probe_material_body(*params, &probe_loop);
        }
        // </plus>
        if (gPipeline.rhiProbeDraw(type, diffuseChannel > -1, &rhi_probe_material_body, &rhi_probe_material_textures, &probe_loop))
        {
            return;     // the render map with the list's batches in their places, each by the body
        }
    }

    LLTexture* lastNormalMap = nullptr;
    LLTexture* lastSpecMap = nullptr;
    LLTexture* lastDiffuse = nullptr;

    gGL.getTexUnit(diffuseChannel)->unbindFast(LLTexUnit::TT_TEXTURE);

    if (intensity > -1)
    {
        mShader->uniform1f(LLShaderMgr::ENVIRONMENT_INTENSITY, lastIntensity);
    }

    if (brightness > -1)
    {
        mShader->uniform1f(LLShaderMgr::EMISSIVE_BRIGHTNESS, lastFullbright);
    }

    if (minAlpha > -1)
    {
        mShader->uniform1f(LLShaderMgr::MINIMUM_ALPHA, lastMinimumAlpha);
    }

    if (specular > -1)
    {
        mShader->uniform4fv(LLShaderMgr::SPECULAR_COLOR, 1, lastSpecular.mV);
    }

    const LLVOAvatar* lastAvatar = nullptr;
    U64 lastMeshId = 0;
    bool skipLastSkin = false;

    if (!rigged && plus::packet::drawMaterials(mShader, begin, end, diffuseChannel, specChannel, normChannel)) { return; } // <plus> LLRHI native (research/180 stage 4) </plus>
    if (!rigged && plus::materiallist::drawMaterials(mShader, begin, end, diffuseChannel, specChannel, normChannel)) { return; } // LLRHI native (research/137 N4)

    // <plus> research/180 stage 2: the textures by name with each direct draw (plus/materiallist DirectUnits)
    plus::materiallist::DirectUnits direct_units(diffuseChannel, specChannel, normChannel); // LLRHI native (research/137 N4)
    LLRHI::TrackerHold tracker_hold(!rigged && begin != end);   // research/180 9 3: the backend's lock once for a static loop
    // </plus>
    for (LLCullResult::drawinfo_iterator i = begin; i != end; )
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_MATERIAL("materials draw loop");
        LLDrawInfo& params = **i;

        LLCullResult::increment_iterator(i, end);

        if (specular > -1 && params.mSpecColor != lastSpecular)
        {
            lastSpecular = params.mSpecColor;
            mShader->uniform4fv(LLShaderMgr::SPECULAR_COLOR, 1, lastSpecular.mV);
        }

        if (intensity != -1 && lastIntensity != params.mEnvIntensity)
        {
            lastIntensity = params.mEnvIntensity;
            mShader->uniform1f(LLShaderMgr::ENVIRONMENT_INTENSITY, lastIntensity);
        }

        if (minAlpha > -1 && lastMinimumAlpha != params.mAlphaMaskCutoff)
        {
            lastMinimumAlpha = params.mAlphaMaskCutoff;
            mShader->uniform1f(LLShaderMgr::MINIMUM_ALPHA, lastMinimumAlpha);
        }

        F32 fullbright = params.mFullbright ? 1.f : 0.f;
        if (brightness > -1 && lastFullbright != fullbright)
        {
            lastFullbright = fullbright;
            mShader->uniform1f(LLShaderMgr::EMISSIVE_BRIGHTNESS, lastFullbright);
        }

        // <plus> research/180 stage 2: by name (direct_units.byName()), the draw takes the textures: no binds </plus>
        if (!direct_units.byName() && normChannel > -1 && params.mNormalMap != lastNormalMap) // <plus> byName </plus>
        {
            lastNormalMap = params.mNormalMap;
            llassert(lastNormalMap);
            gGL.getTexUnit(normChannel)->bindFast(lastNormalMap);
        }

        if (!direct_units.byName() && specChannel > -1 && params.mSpecularMap != lastSpecMap) // <plus> byName </plus>
        {
            lastSpecMap = params.mSpecularMap;
            llassert(lastSpecMap);
            gGL.getTexUnit(specChannel)->bindFast(lastSpecMap);
        }

        if (!direct_units.byName() && params.mTexture != lastDiffuse) // <plus> byName </plus>
        {
            lastDiffuse = params.mTexture;
            if (lastDiffuse)
            {
                gGL.getTexUnit(diffuseChannel)->bindFast(lastDiffuse);
            }
            else
            {
                gGL.getTexUnit(diffuseChannel)->unbindFast(LLTexUnit::TT_TEXTURE);
            }
        }

        // upload matrix palette to shader
        if (rigged)
        {
            if (!uploadMatrixPalette(params.mAvatar, params.mSkinInfo, lastAvatar, lastMeshId, skipLastSkin))
            {
                continue;
            }
        }

        bool tex_setup = false;

        //not batching textures or batch has only 1 texture -- might need a texture matrix
        if (params.mTextureMatrix)
        {
            gGL.getTexUnit(0)->activate();
            gGL.matrixMode(LLRender::MM_TEXTURE);

            gGL.loadMatrix((GLfloat*)params.mTextureMatrix->mMatrix);
            gPipeline.mTextureMatrixOps++;

            tex_setup = true;
        }

        /*if (params.mGroup)  // TOO LATE
        {
            params.mGroup->rebuildMesh();
        }*/

        // <plus> research/243: GrayWire for the draw by name too (drawGeometry has its own);
        // research/256: twice while the materialize band sweeps it (drawGeometry inside draws once then)
        GrayWire::drawBatch(params, [&]() {
        // </plus>
            if (!direct_units.draw(params)) { drawGeometry(params); } // <plus> research/180 stage 2: drawn by name; else as upstream </plus>
        // <plus>
        });
        // </plus>

        if (tex_setup)
        {
            gGL.getTexUnit(0)->activate();
            gGL.loadIdentity();
            gGL.matrixMode(LLRender::MM_MODELVIEW);
        }
    }
}

/**
 * @file drawlist.cpp
 * @brief Draw lists for LLRenderPass's batch loops (research/137 N2,
 *        LLRHI_DRAW_LISTS with LLRHI_DRAW_LISTS_OPAQUE).
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

// Copies of official loops (tools/shadowed-functions.py watches them):
// lldrawpool.cpp LLRenderPass::pushBatches, pushUntexturedBatches,
// pushRiggedBatches, pushUntexturedRiggedBatches, pushMaskBatches,
// pushRiggedMaskBatches, renderGroup, renderRiggedGroup, pushGLTFBatches,
// pushUntexturedGLTFBatches, pushRiggedGLTFBatches,
// pushUntexturedRiggedGLTFBatches and the per batch pushGLTFBatch,
// pushUntexturedGLTFBatch; llfetchedgltfmaterial.cpp
// LLFetchedGLTFMaterial::bindUniforms and bindDirect (getBindTextures' order).
// A batch the list does not take goes through the official per batch call
// (pushBatch, pushUntexturedBatch, pushGLTFBatch ...), so only the loops and
// what decides a list draw's values are copied.

#include "llviewerprecompiledheaders.h"

#include "drawlist.h"
#include "drawlistbatch.h"

#include "lldrawpool.h"
#include "llfetchedgltfmaterial.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llshadermgr.h"
#include "llspatialpartition.h"
#include "llviewertexture.h"
#include "pipeline.h"

namespace plus::drawlist
{
namespace
{
    using LLRHI::DrawList::KIND_OPAQUE;

    // A render map's batches of `type` (gPipeline.beginRenderMap ..
    // endRenderMap) through body(list, batch) inside one list.
    // flush_first: the loop's first batch flushes gGL (setMinimumAlpha does).
    template <typename Body>
    bool render_map_loop(const char* name, U32 type, const Needs& needs, Body&& body, bool flush_first = false)
    {
        if (!LLRHI::DrawList::wanted(KIND_OPAQUE))
        {
            return false;
        }
        LLCullResult::drawinfo_iterator begin = gPipeline.beginRenderMap(type);
        LLCullResult::drawinfo_iterator end = gPipeline.endRenderMap(type);
        if (begin == end)
        {
            return false;   // nothing to draw: the official loop does nothing either
        }
        if (flush_first)
        {
            gGL.flush();
        }
        BatchList& list = BatchList::get();
        if (!list.open(name, KIND_OPAQUE, needs))
        {
            return false;
        }
        if (needs.rigged)
        {
            // The palettes first, in the loop's order (research/140 7-3).
            for (LLCullResult::drawinfo_iterator i = begin; i != end; ++i)
            {
                if (*i)
                {
                    list.preparePalette(**i);
                }
            }
        }
        for (LLCullResult::drawinfo_iterator i = begin; i != end; )
        {
            LLDrawInfo* params = *i;
            LLCullResult::increment_iterator(i, end);
            llassert(params);
            if (params)
            {
                body(list, *params);
            }
        }
        list.close();
        return true;
    }

    // A group's batches of `type` (renderGroup, renderRiggedGroup).
    template <typename Body>
    bool group_loop(const char* name, LLSpatialGroup* group, U32 type, const Needs& needs, Body&& body)
    {
        if (!LLRHI::DrawList::wanted(KIND_OPAQUE))
        {
            return false;
        }
        LLSpatialGroup::drawmap_elem_t& draw_info = group->mDrawMap[type];    // inserts as the official loop does
        if (draw_info.empty())
        {
            return false;
        }
        BatchList& list = BatchList::get();
        if (!list.open(name, KIND_OPAQUE, needs))
        {
            return false;
        }
        if (needs.rigged)
        {
            for (const LLPointer<LLDrawInfo>& params : draw_info)
            {
                if (params.notNull())
                {
                    list.preparePalette(*params);
                }
            }
        }
        for (LLSpatialGroup::drawmap_elem_t::iterator k = draw_info.begin(); k != draw_info.end(); ++k)
        {
            LLDrawInfo* params = *k;
            if (params)
            {
                body(list, *params);
            }
        }
        list.close();
        return true;
    }

    // pushBatch(params, texture, batch_textures): always with textures (it
    // hands pushDirectBatch texture = true whatever it is given).
    void textured_batch(BatchList& list, LLRenderPass& pass, LLDrawInfo& params, bool texture, bool batch_textures)
    {
        if (!list.takes(params) || !list.add(params, Textures::BATCH, batch_textures))
        {
            list.drawOld([&] { pass.pushBatch(params, texture, batch_textures); });
        }
    }

    // if (uploadMatrixPalette(..., lastAvatar, lastMeshId, skipLastSkin)) pushBatch(...).
    void rigged_textured_batch(BatchList& list, LLRenderPass& pass, LLDrawInfo& params, bool texture, bool batch_textures)
    {
        const bool in_list = list.takes(params);
        if (!list.palette(params, in_list))
        {
            return;
        }
        if (!in_list || !list.add(params, Textures::BATCH, batch_textures))
        {
            list.drawOld([&] { pass.pushBatch(params, texture, batch_textures); });
        }
    }

    // ------------------------------------------------------------------
    // GLTF

    Needs gltf_needs()
    {
        Needs needs;
        needs.uniform(LLShaderMgr::MINIMUM_ALPHA)
             .uniform(LLShaderMgr::TEXTURE_BASE_COLOR_TRANSFORM)
             .uniform(LLShaderMgr::ROUGHNESS_FACTOR)
             .uniform(LLShaderMgr::METALLIC_FACTOR)
             .uniform(LLShaderMgr::EMISSIVE_COLOR)
             .uniform(LLShaderMgr::TEXTURE_NORMAL_TRANSFORM)
             .uniform(LLShaderMgr::TEXTURE_METALLIC_ROUGHNESS_TRANSFORM)
             .uniform(LLShaderMgr::TEXTURE_EMISSIVE_TRANSFORM);
        return needs;
    }

    // LLFetchedGLTFMaterial::bindUniforms outside a shadow pass (open() takes
    // none), the same values in the same uniforms.
    void gltf_uniforms(BatchList& list, const LLFetchedGLTFMaterial& mat)
    {
        // glTF 2.0 Specification 3.9.4. Alpha Coverage: mAlphaCutoff only for ALPHA_MODE_MASK.
        list.uniform(LLShaderMgr::MINIMUM_ALPHA, mat.mAlphaMode == LLGLTFMaterial::ALPHA_MODE_MASK ? mat.mAlphaCutoff : -1.f);

        LLGLTFMaterial::TextureTransform::Pack packed;
        mat.mTextureTransform[LLGLTFMaterial::GLTF_TEXTURE_INFO_BASE_COLOR].getPacked(packed);
        list.uniform(LLShaderMgr::TEXTURE_BASE_COLOR_TRANSFORM, 2, 4, packed);

        list.uniform(LLShaderMgr::ROUGHNESS_FACTOR, mat.mRoughnessFactor);
        list.uniform(LLShaderMgr::METALLIC_FACTOR, mat.mMetallicFactor);
        list.uniform(LLShaderMgr::EMISSIVE_COLOR, 1, 3, mat.mEmissiveColor.mV);

        mat.mTextureTransform[LLGLTFMaterial::GLTF_TEXTURE_INFO_NORMAL].getPacked(packed);
        list.uniform(LLShaderMgr::TEXTURE_NORMAL_TRANSFORM, 2, 4, packed);
        mat.mTextureTransform[LLGLTFMaterial::GLTF_TEXTURE_INFO_METALLIC_ROUGHNESS].getPacked(packed);
        list.uniform(LLShaderMgr::TEXTURE_METALLIC_ROUGHNESS_TRANSFORM, 2, 4, packed);
        mat.mTextureTransform[LLGLTFMaterial::GLTF_TEXTURE_INFO_EMISSIVE].getPacked(packed);
        list.uniform(LLShaderMgr::TEXTURE_EMISSIVE_TRANSFORM, 2, 4, packed);
    }

    // pushGLTFBatch (after the palette, for the rigged loop): in_list from takes().
    void gltf_batch(BatchList& list, LLDrawInfo& params, bool in_list)
    {
        LLFetchedGLTFMaterial* mat = params.mGLTFMaterial.get();
        if (in_list)
        {
            gltf_uniforms(list, *mat);
            Extras extras;
            extras.cull = mat->mDoubleSided ? 0 : LLRHI::DrawList::State::INHERIT;  // LLGLDisable cull_face
            if (list.add(params, Textures::GLTF, false, &extras))
            {
                return;
            }
        }
        list.drawOld([&] { LLRenderPass::pushGLTFBatch(params); });
    }

    // pushUntexturedGLTFBatch.
    void untextured_gltf_batch(BatchList& list, LLDrawInfo& params, bool in_list)
    {
        if (in_list)
        {
            Extras extras;
            extras.cull = params.mGLTFMaterial->mDoubleSided ? 0 : LLRHI::DrawList::State::INHERIT;
            if (list.add(params, Textures::NONE, false, &extras))
            {
                return;
            }
        }
        list.drawOld([&] { LLRenderPass::pushUntexturedGLTFBatch(params); });
    }
}

bool pushBatches(LLRenderPass& pass, U32 type, bool texture, bool batch_textures)
{
    if (!texture)
    {
        return false;   // the official pushBatches goes on to pushUntexturedBatches (hooked there)
    }
    return render_map_loop("opaque.pushBatches", type, Needs(), [&](BatchList& list, LLDrawInfo& params)
    {
        textured_batch(list, pass, params, texture, batch_textures);
    });
}

bool pushUntexturedBatches(LLRenderPass& pass, U32 type)
{
    return render_map_loop("opaque.pushUntexturedBatches", type, Needs(), [&](BatchList& list, LLDrawInfo& params)
    {
        if (!list.takes(params) || !list.add(params, Textures::NONE))
        {
            list.drawOld([&] { pass.pushUntexturedBatch(params); });
        }
    });
}

bool pushRiggedBatches(LLRenderPass& pass, U32 type, bool texture, bool batch_textures)
{
    if (!texture)
    {
        return false;   // the official pushRiggedBatches goes on to pushUntexturedRiggedBatches (hooked there)
    }
    return render_map_loop("opaque.pushRiggedBatches", type, Needs().skinned(), [&](BatchList& list, LLDrawInfo& params)
    {
        rigged_textured_batch(list, pass, params, texture, batch_textures);
    });
}

bool pushUntexturedRiggedBatches(LLRenderPass& pass, U32 type)
{
    return render_map_loop("opaque.pushUntexturedRiggedBatches", type, Needs().skinned(), [&](BatchList& list, LLDrawInfo& params)
    {
        const bool in_list = list.takes(params);
        if (!list.palette(params, in_list))
        {
            return;
        }
        if (!in_list || !list.add(params, Textures::NONE))
        {
            list.drawOld([&] { pass.pushUntexturedBatch(params); });
        }
    });
}

bool pushMaskBatches(LLRenderPass& pass, U32 type, bool texture, bool batch_textures)
{
    return render_map_loop("opaque.pushMaskBatches", type, Needs().uniform(LLShaderMgr::MINIMUM_ALPHA),
                           [&](BatchList& list, LLDrawInfo& params)
    {
        list.uniform(LLShaderMgr::MINIMUM_ALPHA, params.mAlphaMaskCutoff);     // setMinimumAlpha
        textured_batch(list, pass, params, texture, batch_textures);
    }, true);
}

bool pushRiggedMaskBatches(LLRenderPass& pass, U32 type, bool texture, bool batch_textures)
{
    return render_map_loop("opaque.pushRiggedMaskBatches", type, Needs().uniform(LLShaderMgr::MINIMUM_ALPHA).skinned(),
                           [&](BatchList& list, LLDrawInfo& params)
    {
        list.uniform(LLShaderMgr::MINIMUM_ALPHA, params.mAlphaMaskCutoff);     // setMinimumAlpha, also for a batch not drawn
        rigged_textured_batch(list, pass, params, texture, batch_textures);
    }, true);
}

bool renderGroup(LLRenderPass& pass, LLSpatialGroup* group, U32 type, bool texture)
{
    return group_loop("opaque.renderGroup", group, type, Needs(), [&](BatchList& list, LLDrawInfo& params)
    {
        textured_batch(list, pass, params, texture, false);
    });
}

bool renderRiggedGroup(LLRenderPass& pass, LLSpatialGroup* group, U32 type, bool texture)
{
    return group_loop("opaque.renderRiggedGroup", group, type, Needs().skinned(), [&](BatchList& list, LLDrawInfo& params)
    {
        rigged_textured_batch(list, pass, params, texture, false);
    });
}

bool pushGLTFBatches(U32 type)
{
    return render_map_loop("opaque.pushGLTFBatches", type, gltf_needs(), [&](BatchList& list, LLDrawInfo& params)
    {
        gltf_batch(list, params, params.mGLTFMaterial.notNull() && list.takes(params));
    });
}

bool pushUntexturedGLTFBatches(U32 type)
{
    return render_map_loop("opaque.pushUntexturedGLTFBatches", type, Needs(), [&](BatchList& list, LLDrawInfo& params)
    {
        untextured_gltf_batch(list, params, params.mGLTFMaterial.notNull() && list.takes(params));
    });
}

bool pushRiggedGLTFBatches(U32 type)
{
    return render_map_loop("opaque.pushRiggedGLTFBatches", type, gltf_needs().skinned(), [&](BatchList& list, LLDrawInfo& params)
    {
        // pushRiggedGLTFBatch: uploadMatrixPalette, then pushGLTFBatch.
        const bool in_list = params.mGLTFMaterial.notNull() && list.takes(params);
        if (list.palette(params, in_list))
        {
            gltf_batch(list, params, in_list);
        }
    });
}

bool pushUntexturedRiggedGLTFBatches(U32 type)
{
    return render_map_loop("opaque.pushUntexturedRiggedGLTFBatches", type, Needs().skinned(), [&](BatchList& list, LLDrawInfo& params)
    {
        const bool in_list = params.mGLTFMaterial.notNull() && list.takes(params);
        if (list.palette(params, in_list))
        {
            untextured_gltf_batch(list, params, in_list);
        }
    });
}
}

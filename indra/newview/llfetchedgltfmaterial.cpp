/**
 * @file   llfetchedgltfmaterial.cpp
 *
 * $LicenseInfo:firstyear=2022&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2022, Linden Research, Inc.
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

#include "llfetchedgltfmaterial.h"

#include "llviewertexturelist.h"
#include "llavatarappearancedefines.h"
#include "llviewerobject.h"
#include "llselectmgr.h"
#include "llshadermgr.h"
#include "pipeline.h"
#include "llrender.h"
#include "llrhi.h"

//static
LLFetchedGLTFMaterial LLFetchedGLTFMaterial::sDefault;

LLFetchedGLTFMaterial::LLFetchedGLTFMaterial()
    : LLGLTFMaterial()
    , mExpectedFlusTime(0.f)
{

}

LLFetchedGLTFMaterial::~LLFetchedGLTFMaterial()
{

}

LLFetchedGLTFMaterial& LLFetchedGLTFMaterial::operator=(const LLFetchedGLTFMaterial& rhs)
{
    LLGLTFMaterial::operator =(rhs);

    mBaseColorTexture = rhs.mBaseColorTexture;
    mNormalTexture = rhs.mNormalTexture;
    mMetallicRoughnessTexture = rhs.mMetallicRoughnessTexture;
    mEmissiveTexture = rhs.mEmissiveTexture;

    return *this;
}

void LLFetchedGLTFMaterial::bind(LLViewerTexture* media_tex)
{
    bindUniforms();
    bindTextures(media_tex);
}

void LLFetchedGLTFMaterial::bindUniforms()
{
    // glTF 2.0 Specification 3.9.4. Alpha Coverage
    // mAlphaCutoff is only valid for LLGLTFMaterial::ALPHA_MODE_MASK
    F32 min_alpha = -1.0;

    LLGLSLShader* shader = LLGLSLShader::sCurBoundShaderPtr;

    if (!LLPipeline::sShadowRender || (mAlphaMode == LLGLTFMaterial::ALPHA_MODE_MASK))
    {
        if (mAlphaMode == LLGLTFMaterial::ALPHA_MODE_MASK)
        {
            min_alpha = mAlphaCutoff;
        }
        shader->uniform1f(LLShaderMgr::MINIMUM_ALPHA, min_alpha);
    }

    F32 base_color_packed[8];
    mTextureTransform[GLTF_TEXTURE_INFO_BASE_COLOR].getPacked(base_color_packed);
    shader->uniform4fv(LLShaderMgr::TEXTURE_BASE_COLOR_TRANSFORM, 2, (F32*)base_color_packed);

    if (!LLPipeline::sShadowRender)
    {
        // NOTE: base color factor is baked into vertex stream

        shader->uniform1f(LLShaderMgr::ROUGHNESS_FACTOR, mRoughnessFactor);
        shader->uniform1f(LLShaderMgr::METALLIC_FACTOR, mMetallicFactor);
        shader->uniform3fv(LLShaderMgr::EMISSIVE_COLOR, 1, mEmissiveColor.mV);

        F32 normal_packed[8];
        mTextureTransform[GLTF_TEXTURE_INFO_NORMAL].getPacked(normal_packed);
        shader->uniform4fv(LLShaderMgr::TEXTURE_NORMAL_TRANSFORM, 2, (F32*)normal_packed);

        F32 metallic_roughness_packed[8];
        mTextureTransform[GLTF_TEXTURE_INFO_METALLIC_ROUGHNESS].getPacked(metallic_roughness_packed);
        shader->uniform4fv(LLShaderMgr::TEXTURE_METALLIC_ROUGHNESS_TRANSFORM, 2, (F32*)metallic_roughness_packed);

        F32 emissive_packed[8];
        mTextureTransform[GLTF_TEXTURE_INFO_EMISSIVE].getPacked(emissive_packed);
        shader->uniform4fv(LLShaderMgr::TEXTURE_EMISSIVE_TRANSFORM, 2, (F32*)emissive_packed);
    }
}

U32 LLFetchedGLTFMaterial::getBindTextures(LLViewerTexture* media_tex, S32* uniforms, LLViewerTexture** textures) const
{
    // override emissive and base color textures with media tex if present
    LLViewerTexture* baseColorTex = media_tex ? media_tex : mBaseColorTexture.get();
    LLViewerTexture* emissiveTex = media_tex ? media_tex : mEmissiveTexture.get();

    U32 count = 0;
    uniforms[count] = LLShaderMgr::DIFFUSE_MAP;
    textures[count++] = baseColorTex ? baseColorTex : LLViewerFetchedTexture::sWhiteImagep.get();

    if (!LLPipeline::sShadowRender)
    {
        uniforms[count] = LLShaderMgr::BUMP_MAP;
        textures[count++] = mNormalTexture.notNull() && mNormalTexture->getDiscardLevel() <= 4 ? mNormalTexture.get()
                                                                                                 : LLViewerFetchedTexture::sFlatNormalImagep.get();

        // PBR linear packed Occlusion, Roughness, Metal.
        uniforms[count] = LLShaderMgr::SPECULAR_MAP;
        textures[count++] = mMetallicRoughnessTexture.notNull() ? mMetallicRoughnessTexture.get()
                                                                : LLViewerFetchedTexture::sWhiteImagep.get();

        // PBR sRGB Emissive
        uniforms[count] = LLShaderMgr::EMISSIVE_MAP;
        textures[count++] = emissiveTex ? emissiveTex : LLViewerFetchedTexture::sWhiteImagep.get();
    }
    return count;
}

U32 LLFetchedGLTFMaterial::getDirectTextures(LLViewerTexture* media_tex, LLViewerTexture** textures) const
{
    static_assert(MAX_DIRECT_TEXTURES == MAX_BIND_TEXTURES, "the same textures");
    S32 uniforms[MAX_BIND_TEXTURES];
    return getBindTextures(media_tex, uniforms, textures);
}

void LLFetchedGLTFMaterial::bindTextures(LLViewerTexture* media_tex)
{
    LLGLSLShader* shader = LLGLSLShader::sCurBoundShaderPtr;
    S32 uniforms[MAX_BIND_TEXTURES];
    LLViewerTexture* textures[MAX_BIND_TEXTURES];
    const U32 count = getBindTextures(media_tex, uniforms, textures);
    for (U32 i = 0; i < count; ++i)
    {
        shader->bindTexture(uniforms[i], textures[i]);
    }
}

U32 LLFetchedGLTFMaterial::bindDirect(LLViewerTexture* media_tex, U32* unit_textures, U32 max_units)
{
    bindUniforms();

    LLGLSLShader* shader = LLGLSLShader::sCurBoundShaderPtr;
    S32 uniforms[MAX_BIND_TEXTURES];
    LLViewerTexture* textures[MAX_BIND_TEXTURES];
    const U32 count = getBindTextures(media_tex, uniforms, textures);
    U32 unit_count = 0;
    for (U32 i = 0; i < count; ++i)
    {
        // The unit bindTexture would bind (none: the shader does not sample it).
        if (uniforms[i] < 0 || uniforms[i] >= (S32)shader->mTexture.size())
        {
            continue;
        }
        const S32 unit = shader->getTextureChannel(uniforms[i]);
        if (unit < 0)
        {
            continue;
        }
        if ((U32)unit >= LL_NUM_TEXTURE_LAYERS)
        {
            continue;   // no such unit (getTexUnit would hand out the dummy unit)
        }
        if ((U32)unit >= max_units)
        {
            gGL.getTexUnit(unit)->bindFast(textures[i]);    // beyond the draw's units: through GL
            continue;
        }
        while (unit_count <= (U32)unit)
        {
            unit_textures[unit_count++] = LLRHI::DirectDraw::KEEP_UNIT;
        }
        // KEEP_UNIT (and bound through GL) when the texture is not ready to be named.
        unit_textures[unit] = gGL.getTexUnit(unit)->directTexture(textures[i]);
    }
    return unit_count;
}

LLViewerFetchedTexture* fetch_texture(const LLUUID& id)
{
    LLViewerFetchedTexture* img = nullptr;
    if (id.notNull())
    {
        img = LLViewerTextureManager::getFetchedTexture(id, FTT_DEFAULT, true, LLGLTexture::BOOST_NONE, LLViewerTexture::LOD_TEXTURE);
        img->addTextureStats(64.f * 64.f, true);
    }
    return img;
};

bool LLFetchedGLTFMaterial::replaceLocalTexture(const LLUUID& tracking_id, const LLUUID& old_id, const LLUUID& new_id)
{
    bool res = false;
    if (mTextureId[LLGLTFMaterial::GLTF_TEXTURE_INFO_BASE_COLOR] == old_id)
    {
        mTextureId[LLGLTFMaterial::GLTF_TEXTURE_INFO_BASE_COLOR] = new_id;
        mBaseColorTexture = fetch_texture(new_id);
        res = true;
    }
    if (mTextureId[LLGLTFMaterial::GLTF_TEXTURE_INFO_NORMAL] == old_id)
    {
        mTextureId[LLGLTFMaterial::GLTF_TEXTURE_INFO_NORMAL] = new_id;
        mNormalTexture = fetch_texture(new_id);
        res = true;
    }
    if (mTextureId[LLGLTFMaterial::GLTF_TEXTURE_INFO_METALLIC_ROUGHNESS] == old_id)
    {
        mTextureId[LLGLTFMaterial::GLTF_TEXTURE_INFO_METALLIC_ROUGHNESS] = new_id;
        mMetallicRoughnessTexture = fetch_texture(new_id);
        res = true;
    }
    if (mTextureId[LLGLTFMaterial::GLTF_TEXTURE_INFO_EMISSIVE] == old_id)
    {
        mTextureId[LLGLTFMaterial::GLTF_TEXTURE_INFO_EMISSIVE] = new_id;
        mEmissiveTexture = fetch_texture(new_id);
        res = true;
    }

    for (int i = 0; i < GLTF_TEXTURE_INFO_COUNT; ++i)
    {
        if (mTextureId[i] == new_id)
        {
            res = true;
        }
    }

    if (res)
    {
        mTrackingIdToLocalTexture[tracking_id] = new_id;
    }
    else
    {
        mTrackingIdToLocalTexture.erase(tracking_id);
    }
    updateLocalTexDataDigest();

    return res;
}

void LLFetchedGLTFMaterial::addTextureEntry(LLTextureEntry* te)
{
    mTextureEntires.insert(te);
}

void LLFetchedGLTFMaterial::removeTextureEntry(LLTextureEntry* te)
{
    mTextureEntires.erase(te);
}

void LLFetchedGLTFMaterial::updateTextureTracking()
{
    for (local_tex_map_t::value_type &val : mTrackingIdToLocalTexture)
    {
        LLLocalBitmapMgr::getInstance()->associateGLTFMaterial(val.first, this);
    }
}

void LLFetchedGLTFMaterial::clearFetchedTextures()
{
    mBaseColorTexture = nullptr;
    mNormalTexture = nullptr;
    mMetallicRoughnessTexture = nullptr;
    mEmissiveTexture = nullptr;
}

void LLFetchedGLTFMaterial::materialBegin()
{
    llassert(!mFetching);
    mFetching = true;
}

void LLFetchedGLTFMaterial::onMaterialComplete(std::function<void()> material_complete)
{
    if (!material_complete) { return; }

    if (!mFetching)
    {
        material_complete();
        return;
    }

    materialCompleteCallbacks.push_back(material_complete);
}

void LLFetchedGLTFMaterial::materialComplete(bool success)
{
    llassert(mFetching);
    mFetching = false;
    mFetchSuccess = success;

    for (std::function<void()> material_complete : materialCompleteCallbacks)
    {
        material_complete();
    }
    materialCompleteCallbacks.clear();
    materialCompleteCallbacks.shrink_to_fit();
}

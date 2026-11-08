/**
 * @file   llfetchedgltfmaterial.h
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


#pragma once

#include "llgltfmaterial.h"
#include "llpointer.h"
#include "llviewertexture.h"

class LLGLSLShader;
class LLGLTFMaterialList;
class LLTerrainMaterials;

class LLFetchedGLTFMaterial: public LLGLTFMaterial
{
    // for lifetime management
    friend class LLGLTFMaterialList;
    friend class LLTerrainMaterials;
public:
    LLFetchedGLTFMaterial();
    virtual ~LLFetchedGLTFMaterial();

    LLFetchedGLTFMaterial& operator=(const LLFetchedGLTFMaterial& rhs);
    // LLGLTFMaterial::operator== is defined, but LLFetchedGLTFMaterial::operator== is not.
    bool operator==(const LLGLTFMaterial& rhs) const = delete;

    // If this material is loaded, fire the given function
    void onMaterialComplete(std::function<void()> material_complete);

    // bind this material for rendering
    //   media_tex - optional media texture that may override the base color texture
    void bind(LLViewerTexture* media_tex = nullptr);

    // LLRHI: bind() for a direct draw (LLRHI::DirectDraw). Sets the uniforms
    // and, instead of binding the textures, writes the GL texture name each
    // texture unit should sample to unit_textures (KEEP_UNIT for the units it
    // leaves alone, and where a texture had to be bound through GL after all).
    // Returns the number of units written (at most max_units).
    U32 bindDirect(LLViewerTexture* media_tex, U32* unit_textures, U32 max_units);
    // The texture half of bind(), through GL: for a direct draw that was refused.
    void bindTextures(LLViewerTexture* media_tex = nullptr);
    // LLRHI (research/50 step 4): the textures bind() binds (base color, then
    // normal, metallic-roughness and emissive while not rendering shadows), as
    // the GPU probe list checks them. Returns the count.
    static constexpr U32 MAX_DIRECT_TEXTURES = 4;
    U32 getDirectTextures(LLViewerTexture* media_tex, LLViewerTexture** textures) const;

    bool isFetching() const { return mFetching; }
    bool isLoaded() const { return !mFetching && mFetchSuccess; }

    void addTextureEntry(LLTextureEntry* te) override;
    void removeTextureEntry(LLTextureEntry* te) override;
    virtual bool replaceLocalTexture(const LLUUID& tracking_id, const LLUUID& old_id, const LLUUID& new_id) override;
    virtual void updateTextureTracking() override;

    // Textures used for fetching/rendering
    LLPointer<LLViewerFetchedTexture> mBaseColorTexture;
    LLPointer<LLViewerFetchedTexture> mNormalTexture;
    LLPointer<LLViewerFetchedTexture> mMetallicRoughnessTexture;
    LLPointer<LLViewerFetchedTexture> mEmissiveTexture;
    void clearFetchedTextures();

    std::set<LLTextureEntry*> mTextureEntires;

    // default material for when assets don't have one
    static LLFetchedGLTFMaterial sDefault;
protected:
    // The parts of bind(): the uniforms, and the textures it binds with the
    // sampler uniform (LLShaderMgr) each goes to. Returns the texture count.
    static constexpr U32 MAX_BIND_TEXTURES = 4;
    void bindUniforms();
    U32 getBindTextures(LLViewerTexture* media_tex, S32* uniforms, LLViewerTexture** textures) const;

    // Lifetime management

    void materialBegin();
    void materialComplete(bool success);

    F64 mExpectedFlusTime; // since epoch in seconds
    bool mActive = true;
    bool mFetching = false;
    bool mFetchSuccess = false;
    std::vector<std::function<void()>> materialCompleteCallbacks;
};


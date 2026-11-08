/**
 * @file lldrawpoolalpha.h
 * @brief LLDrawPoolAlpha class definition
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

#ifndef LL_LLDRAWPOOLALPHA_H
#define LL_LLDRAWPOOLALPHA_H

#include "lldrawpool.h"
#include "llrender.h"
#include "llframetimer.h"

class LLFace;
class LLColor4;
class LLGLSLShader;

class LLDrawPoolAlpha final: public LLRenderPass
{
public:

    // set by llsettingsvo so lldrawpoolalpha has quick access to the water plane in eye space
    static LLVector4 sWaterPlane;

    enum
    {
        VERTEX_DATA_MASK =  LLVertexBuffer::MAP_VERTEX |
                            LLVertexBuffer::MAP_NORMAL |
                            LLVertexBuffer::MAP_COLOR |
                            LLVertexBuffer::MAP_TEXCOORD0
    };
    virtual U32 getVertexDataMask() { return VERTEX_DATA_MASK; }

    LLDrawPoolAlpha(U32 type);
    /*virtual*/ ~LLDrawPoolAlpha();

    /*virtual*/ S32 getNumPostDeferredPasses();
    /*virtual*/ void renderPostDeferred(S32 pass);
    /*virtual*/ S32  getNumPasses() { return 1; }

    // LLRHI (research/41): which non-rigged alpha groups a pass draws. The
    // hybrid OIT session draws the avatars' (attachments) apart from the rest.
    enum EAlphaGroups { GROUPS_ALL, GROUPS_AVATAR, GROUPS_WORLD };

    void forwardRender(bool write_depth = false, EAlphaGroups groups = GROUPS_ALL);
    /*virtual*/ void prerender();

    void renderDebugAlpha();

    void renderGroupAlpha(LLSpatialGroup* group, U32 type, U32 mask, bool texture = true);
    void renderAlpha(U32 mask, bool depth_only = false, bool rigged = false, EAlphaGroups groups = GROUPS_ALL);
    void renderAlphaHighlight();

    static bool sShowDebugAlpha;

private:
    LLGLSLShader* target_shader;

    // setup by beginFooPass, [0] is static variant, [1] is rigged variant
    LLGLSLShader* simple_shader = nullptr;
    LLGLSLShader* fullbright_shader = nullptr;
    LLGLSLShader* emissive_shader = nullptr;
    LLGLSLShader* pbr_emissive_shader = nullptr;
    LLGLSLShader* pbr_shader = nullptr;

    void drawEmissive(LLDrawInfo* draw);
    void renderEmissives(std::vector<LLDrawInfo*>& emissives);
    void renderRiggedEmissives(std::vector<LLDrawInfo*>& emissives);
    void renderPbrEmissives(std::vector<LLDrawInfo*>& emissives);
    void renderRiggedPbrEmissives(std::vector<LLDrawInfo*>& emissives);
    bool TexSetup(LLDrawInfo* draw, bool use_material);
    void RestoreTexSetup(bool tex_setup);
    // LLRHI native (research/137 N3): renderAlpha with draw lists (plus/alphalist/alphalist.cpp); false: renderAlpha draws
    bool renderAlphaList(U32 mask, bool depth_only, bool rigged, EAlphaGroups groups);
    // <plus> research/205 3a-3: an OIT "all" session's alpha from the scene's records (3a-5:
    // the sorted way's passes expanded for their spans; true: the alpha view is open, the
    // loop leaves out what it drew and draws the rest, not plus/alphalist)
    bool drawSceneAlpha(bool depth_only, bool rigged, EAlphaGroups groups);
    // </plus>

    // our 'normal' alpha blend function for this pass
    LLRender::eBlendFactor mColorSFactor;
    LLRender::eBlendFactor mColorDFactor;
    LLRender::eBlendFactor mAlphaSFactor;
    LLRender::eBlendFactor mAlphaDFactor;

    // if true, we're executing a rigged render pass
    bool mRigged = false;

    // LLRHI (research/41): an OIT session is open; the priorities of the
    // current group's batches (LLRHI::sOITPriority), for its glow pass.
    bool mOIT = false;
    std::vector<std::pair<const LLDrawInfo*, U32>> mOITPriorities;
    void setOITPriority(const LLDrawInfo* draw);
};

#endif // LL_LLDRAWPOOLALPHA_H

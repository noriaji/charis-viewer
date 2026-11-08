/**
 * @file llscenealpha.cpp
 * @brief The alpha pool's batches on the retained scene's way (research/205, stage 3a): census and verify.
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

#include "llviewerprecompiledheaders.h"

#include "llscenealpha.h"

#include "lldrawpool.h"
#include "lldrawpoolalpha.h"
#include "llfetchedgltfmaterial.h"
#include "llframetimer.h"
#include "llrender.h"
#include "llrhiscene.h"
#include "llrhihash.h"
#include "llscenerecords.h"     // research/218 vk-s2f: the alpha's auto
#include "llspatialpartition.h"
#include "lltimer.h"
#include "llvertexbuffer.h"
#include "llviewerregion.h"
#include "llviewershadermgr.h"
#include "pipeline.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <sstream>

// lldrawpoolalpha.cpp (research/41): the OIT priority of an alpha group's batches.
U32 oit_group_priority(LLSpatialGroup* group, bool rigged);
extern bool gCubeSnapshot;

// Never destroyed: batches and groups may still go away while static objects do at exit.
LLSceneAlpha& gSceneAlpha = *new LLSceneAlpha();

namespace
{
    // The program the alpha loop binds for a batch (renderAlpha's choice, not rigged).
    enum ProgramKind : U8
    {
        PROG_OTHER,
        PROG_PLAIN,         // gDeferredAlphaProgram (simple_shader)
        PROG_FULLBRIGHT,    // gDeferredFullbrightAlphaMaskAlphaProgram (fullbright_shader)
        PROG_MATERIAL,      // gDeferredMaterialProgram[mShaderMask]
        PROG_PBR,           // gDeferredPBRAlphaProgram (a blended GLTF material)
    };
    const char* const PROGRAM_NAMES[] = { "other", "alpha", "fullbright alpha", "legacy material", "PBR alpha" };

    const char* const CATEGORY_NAMES[LLSceneAlpha::CAT_COUNT] =
    {
        "rigged", "gltf", "partition", "texture list", "legacy material", "custom blend", "fullbright", "plain"
    };
    const char* const KIND_NAMES[LLSceneAlpha::KIND_COUNT] =
    {
        "oit all", "hybrid avatars", "hybrid world", "no oit", "pre-water"
    };
    const char* const DIFF_NAMES[LLSceneAlpha::DIFF_COUNT] =
    {
        "missing", "group", "place", "content", "program", "priority", "stale"
    };

    // The main view's alpha (as renderPostDeferred's main_view, either pool).
    bool main_view()
    {
        return !LLPipeline::sRenderingHUDs && !LLPipeline::sImpostorRender && !LLPipeline::sUnderWaterRender &&
               !LLPipeline::sReflectionRender && !LLPipeline::sShadowRender && !gCubeSnapshot;
    }

    bool emissive(const LLDrawInfo& info)
    {
        return info.mVertexBuffer.notNull() && info.mVertexBuffer->hasDataType(LLVertexBuffer::TYPE_EMISSIVE);
    }

    // research/205 3a-3's scope first, then what keeps a batch out of it.
    LLSceneAlpha::Category category_of(const LLDrawInfo& info, LLSpatialGroup* group)
    {
        if (info.mAvatar.notNull() || info.mSkinInfo)
        {
            return LLSceneAlpha::CAT_RIGGED;
        }
        if (info.mGLTFMaterial.notNull())
        {
            return LLSceneAlpha::CAT_GLTF;
        }
        const LLSpatialPartition* part = group ? group->getSpatialPartition() : nullptr;
        if (!part || group->isHUDGroup() ||
            (part->mPartitionType != LLViewerRegion::PARTITION_VOLUME && part->mPartitionType != LLViewerRegion::PARTITION_BRIDGE) ||
            info.mVertexBuffer.isNull() || !info.mCount)
        {
            return LLSceneAlpha::CAT_PARTITION;
        }
        if (info.mBlendFuncSrc != LLRender::BF_SOURCE_ALPHA || info.mBlendFuncDst != LLRender::BF_ONE_MINUS_SOURCE_ALPHA)
        {
            return LLSceneAlpha::CAT_CUSTOM_BLEND;
        }
        // research/205 H5 (vk-s3a6): a list of several is in the scope (the usual blend, no material).
        if (info.mTextureList.size() > 1)
        {
            return LLSceneAlpha::CAT_TEXTURE_LIST;
        }
        if (info.mMaterial.notNull())
        {
            return LLSceneAlpha::CAT_MATERIAL;
        }
        return info.mFullbright ? LLSceneAlpha::CAT_FULLBRIGHT : LLSceneAlpha::CAT_PLAIN;
    }

    // fix-r6: whether the group's alpha batches get a record (onCreated): those
    // LLVolumeGeometryManager::registerFace makes (volumes and the bridges:
    // linksets, attachments, animesh, HUD). Particles, clouds, grass, trees,
    // water and terrain make theirs elsewhere, with none (verify skips them).
    bool records_made(LLSpatialGroup* group)
    {
        const LLSpatialPartition* part = group ? group->getSpatialPartition() : nullptr;
        if (!part)
        {
            return false;
        }
        switch (part->mPartitionType)
        {
        case LLViewerRegion::PARTITION_VOLUME:
        case LLViewerRegion::PARTITION_BRIDGE:
        case LLViewerRegion::PARTITION_AVATAR:
        case LLViewerRegion::PARTITION_CONTROL_AV:
        case LLViewerRegion::PARTITION_HUD:
            return true;
        default:
            return false;
        }
    }

    bool in_scope(U8 category)
    {
        return category == LLSceneAlpha::CAT_FULLBRIGHT || category == LLSceneAlpha::CAT_PLAIN || category == LLSceneAlpha::CAT_TEXTURE_LIST;
    }

    // renderAlpha's program for a batch that is not rigged, in the main view (no HUD, no impostor).
    U8 predicted_program(const LLDrawInfo& info)
    {
        if (info.mGLTFMaterial.notNull() && info.mGLTFMaterial->mAlphaMode == LLGLTFMaterial::ALPHA_MODE_BLEND)
        {
            return PROG_PBR;
        }
        if (info.mMaterial.notNull())
        {
            return PROG_MATERIAL;
        }
        return info.mFullbright ? PROG_FULLBRIGHT : PROG_PLAIN;
    }

    U8 bound_program(const LLGLSLShader* shader, U32& mask)
    {
        mask = 0;
        if (shader == &gDeferredAlphaProgram)
        {
            return PROG_PLAIN;
        }
        if (shader == &gDeferredFullbrightAlphaMaskAlphaProgram)
        {
            return PROG_FULLBRIGHT;
        }
        if (shader == &gDeferredPBRAlphaProgram)
        {
            return PROG_PBR;
        }
        const LLGLSLShader* first = &gDeferredMaterialProgram[0];
        if (shader >= first && shader < first + LLMaterial::SHADER_COUNT * 2)
        {
            mask = (U32)(shader - first);
            return PROG_MATERIAL;
        }
        return PROG_OTHER;
    }

    // What a GPU record would be written from (the content verify compares).
    U64 content_hash(const LLDrawInfo& info)
    {
        U64 h = LLRHI::FNV1A_BASIS_SHORT;
        auto mix = [&h](U64 v) { h = LLRHI::fnv1aMix(h, v); };
        mix((U64)(uintptr_t)info.mVertexBuffer.get());
        mix(((U64)info.mStart << 16) | info.mEnd);
        mix(((U64)info.mCount << 32) | info.mOffset);
        mix((U64)(uintptr_t)info.mTexture.get());
        mix(info.mTextureList.size());
        mix((U64)(uintptr_t)info.mTextureMatrix);
        mix((U64)(uintptr_t)info.mModelMatrix);
        mix((U64)(uintptr_t)info.mMaterial.get());
        mix((U64)(uintptr_t)info.mGLTFMaterial.get());
        mix(((U64)info.mShaderMask << 16) | ((U64)info.mBlendFuncSrc << 8) | (U64)info.mBlendFuncDst);
        mix(((U64)info.mFullbright << 1) | (emissive(info) ? 1u : 0u));
        return h;
    }

    // The census's bucket key: program, glow, vertex format (never 0).
    U64 bucket_key(const LLDrawInfo& info)
    {
        const U64 program = info.mFullbright ? 2 : 1;     // the plain or fullbright program (a texture list's too)
        const U64 format = info.mVertexBuffer.notNull() ? info.mVertexBuffer->getTypeMask() : 0;
        return program | ((emissive(info) ? 1ull : 0ull) << 2) | (format << 3);
    }
}

const bool LLSceneAlpha::sCensus = LLRHI::envSet("LLRHI_SCENE_ALPHA_CENSUS");
const bool LLSceneAlpha::sVerify = LLRHI::sceneAlphaMode() != LLRHI::SceneMode::Off;
const bool LLSceneAlpha::sActive = LLSceneAlpha::sCensus || LLSceneAlpha::sVerify;

// --- events ----------------------------------------------------------------------

void LLSceneAlpha::onCreated(LLDrawInfo& info, LLSpatialGroup* group, U32 pass)
{
    if (!sVerify || pass != LLRenderPass::PASS_ALPHA || !group)
    {
        return;
    }
    const auto old = mRecords.find(&info);
    if (old != mRecords.end())
    {
        forget(&info, old->second);     // the address of a batch that went without onDestroyed
        mRecords.erase(old);
    }
    Record& r = mRecords[&info];
    r = Record();
    r.group = group;
    mGroupRecords[group].push_back(&info);
    mPending.push_back(&info);
    ++mCreated;
}

void LLSceneAlpha::onDestroyed(const LLDrawInfo& info)
{
    if (!sVerify)
    {
        return;
    }
    const auto it = mRecords.find(&info);
    if (it != mRecords.end())
    {
        forget(&info, it->second);
        mRecords.erase(it);
        ++mDropped;
    }
}

void LLSceneAlpha::onGroupCleared(LLSpatialGroup* group)
{
    if (!sVerify)
    {
        return;
    }
    const auto g = mGroupRecords.find(group);
    if (g == mGroupRecords.end())
    {
        return;
    }
    for (const LLDrawInfo* info : g->second)
    {
        if (mRecords.erase(info))
        {
            ++mDropped;
        }
    }
    mGroupRecords.erase(g);
    if (mGroup == group)
    {
        mGroup = nullptr;
    }
}

// Out of its group's list (the record itself is the caller's to erase).
void LLSceneAlpha::forget(const LLDrawInfo* info, Record& record)
{
    const auto g = mGroupRecords.find(record.group);
    if (g == mGroupRecords.end())
    {
        return;
    }
    std::vector<const LLDrawInfo*>& infos = g->second;
    const auto at = std::find(infos.begin(), infos.end(), info);
    if (at != infos.end())
    {
        *at = infos.back();
        infos.pop_back();
    }
    if (infos.empty())
    {
        mGroupRecords.erase(g);
    }
}

// The records made since: their place in their group's draw map and what
// they would be written from (the group's rebuild is done by now).
void LLSceneAlpha::writePending()
{
    if (mPending.empty())
    {
        return;
    }
    std::vector<LLSpatialGroup*> groups;
    groups.reserve(mPending.size());
    for (const LLDrawInfo* info : mPending)
    {
        const auto it = mRecords.find(info);
        if (it != mRecords.end() && it->second.pending)
        {
            groups.push_back(it->second.group);
        }
    }
    mPending.clear();
    std::sort(groups.begin(), groups.end());
    groups.erase(std::unique(groups.begin(), groups.end()), groups.end());
    for (LLSpatialGroup* group : groups)
    {
        const auto map = group->mDrawMap.find(LLRenderPass::PASS_ALPHA);
        if (map != group->mDrawMap.end())
        {
            const LLSpatialGroup::drawmap_elem_t& draw_vec = map->second;
            for (size_t i = 0; i < draw_vec.size(); ++i)
            {
                const LLDrawInfo* info = draw_vec[i].get();
                const auto it = mRecords.find(info);
                if (it == mRecords.end() || !it->second.pending || it->second.group != group)
                {
                    continue;
                }
                Record& r = it->second;
                r.pending = false;
                r.place = (U32)i;
                r.category = category_of(*info, group);
                r.program = predicted_program(*info);
                r.shader_mask = info->mShaderMask;
                r.hash = content_hash(*info);
                ++mWritten;
            }
        }
        // A record not found in its group's draw map: written with no place
        // (a draw of it is a difference).
        const auto g = mGroupRecords.find(group);
        if (g != mGroupRecords.end())
        {
            for (const LLDrawInfo* info : g->second)
            {
                const auto it = mRecords.find(info);
                if (it != mRecords.end() && it->second.pending)
                {
                    it->second.pending = false;
                    it->second.place = U32_MAX;
                    it->second.hash = content_hash(*info);
                    it->second.category = category_of(*info, group);
                    it->second.program = predicted_program(*info);
                    it->second.shader_mask = info->mShaderMask;
                }
            }
        }
    }
}

// --- the alpha pool -----------------------------------------------------------

LLSceneAlpha::PassScope::PassScope(bool post_water, bool depth_only, bool rigged, U32 groups, bool oit)
{
    if (sActive)
    {
        gSceneAlpha.beginPass(post_water, depth_only, rigged, groups, oit);
        mOpen = gSceneAlpha.mPass;
    }
}

LLSceneAlpha::PassScope::~PassScope()
{
    if (mOpen)
    {
        gSceneAlpha.endPass();
    }
}

void LLSceneAlpha::beginPass(bool post_water, bool depth_only, bool rigged, U32 groups, bool oit)
{
    mPass = false;
    if (!mStarted)
    {
        mStarted = true;
        mReportAt = LLTimer::getTotalSeconds() + REPORT_SECONDS;
        const LLRHI::SceneMode asked = LLRHI::sceneAlphaMode();     // as asked, for the log (what it does: alphaMode)
        LL_INFOS("SceneAlpha") << "alpha on the scene's way (research/205): census " << (sCensus ? "on" : "off")
                               << " (LLRHI_SCENE_ALPHA_CENSUS), records " << LLRHI::sceneModeName(asked)
                               << " (LLRHI_SCENE_ALPHA, needs LLRHI_SCENE)"
                               << (asked == LLRHI::SceneMode::On
                                       ? "; an OIT \"all\" session's plain and fullbright alpha is drawn from the scene's records "
                                         "while the scene draws (research/205 section 9); the batches it drew are checked as drawn"
                                       : asked == LLRHI::SceneMode::Auto
                                       ? "; verified until 600 world frames agree, then drawn from the scene's records while the "
                                         "scene draws (research/218 vk-s2f); a difference: off for the run"
                                       : "")
                               << LL_ENDL;
    }
    const U32 frame = LLFrameTimer::getFrameCount();
    if (!mFrameKnown || frame != mFrame)
    {
        mFrameKnown = true;
        mFrame = frame;
        mVerifyFrame = frame % VERIFY_EVERY == 0;
        ++mReportFrames;
        if (LLTimer::getTotalSeconds() >= mReportAt)
        {
            report();
            mReportAt = LLTimer::getTotalSeconds() + REPORT_SECONDS;
        }
    }
    if (sVerify)
    {
        writePending();
    }
    if (depth_only || !main_view())
    {
        return;
    }
    mKind = !post_water ? KIND_PRE_WATER :
            !oit ? KIND_NO_OIT :
            groups == LLDrawPoolAlpha::GROUPS_AVATAR ? KIND_HYBRID_AVATAR :
            groups == LLDrawPoolAlpha::GROUPS_WORLD ? KIND_HYBRID_WORLD : KIND_OIT_ALL;
    mCensusPass = sCensus;
    // research/218 vk-s2f: not after its auto went off (nothing of the alpha is the scene's then).
    mVerifyPass = sVerify && mVerifyFrame && post_water && !rigged && gSceneRecords.alphaMode() != LLRHI::SceneMode::Off;
    mCheckPriority = mVerifyPass && oit;
    // Every batch of a drawn group is drawn (no hybrid half picks some).
    mCheckStale = mVerifyPass && groups == LLDrawPoolAlpha::GROUPS_ALL;
    mPass = mCensusPass || mVerifyPass;
    if (!mPass)
    {
        return;
    }
    ++mPassSerial;
    mGroup = nullptr;
    mSpanKey = 0;
    mSpanLength = 0;
    mBuckets.clear();
    if (mCensusPass)
    {
        ++mCounts[mKind].passes;
    }
    if (mVerifyPass)
    {
        ++mVerifiedPasses;
        mCheckedBefore = mChecked;
    }
}

void LLSceneAlpha::endPass()
{
    endGroup();
    if (mCensusPass)
    {
        PassCounts& c = mCounts[mKind];
        c.longest = llmax(c.longest, mSpanLength);
        c.buckets += mBuckets.size();
    }
    if (mVerifyPass && mChecked != mCheckedBefore)
    {
        gSceneRecords.alphaChecked(false);  // research/218 vk-s2f: a check of the alpha's world frame (its differences: difference())
    }
    mPass = false;
    mGroup = nullptr;
}

void LLSceneAlpha::noteGroup(LLSpatialGroup* group)
{
    if (!mPass)
    {
        return;
    }
    endGroup();
    mGroup = group;
}

// The group's end: its span closes; verify: its records were all drawn.
void LLSceneAlpha::endGroup()
{
    if (mCensusPass && mSpanKey)
    {
        mCounts[mKind].longest = llmax(mCounts[mKind].longest, mSpanLength);
    }
    mSpanKey = 0;
    mSpanLength = 0;
    if (mCheckStale && mGroup)
    {
        const auto g = mGroupRecords.find(mGroup);
        if (g != mGroupRecords.end())
        {
            for (const LLDrawInfo* info : g->second)
            {
                const auto it = mRecords.find(info);
                if (it != mRecords.end() && !it->second.pending && it->second.seen != mPassSerial)
                {
                    difference(DIFF_STALE, info, "a record of a drawn group, not drawn");
                }
            }
        }
    }
    mGroup = nullptr;
}

void LLSceneAlpha::noteBatch(LLSpatialGroup* group, const LLDrawInfo& params, size_t place, const LLGLSLShader* shader, U32 priority)
{
    if (!mPass)
    {
        return;
    }
    if (group != mGroup)
    {
        noteGroup(group);   // a loop that did not name the group (never so; keeps the spans right)
    }
    const U8 category = category_of(params, group);

    if (mCensusPass)
    {
        PassCounts& c = mCounts[mKind];
        ++c.batches;
        ++c.categories[category];
        if (in_scope(category))
        {
            c.glow += emissive(params) ? 1 : 0;
            c.animated += params.mTextureMatrix ? 1 : 0;
            const U64 key = bucket_key(params);
            if (key != mSpanKey)
            {
                c.longest = llmax(c.longest, mSpanLength);
                ++c.spans;
                mSpanKey = key;
                mSpanLength = 0;
            }
            ++mSpanLength;
            if (std::find(mBuckets.begin(), mBuckets.end(), key) == mBuckets.end())
            {
                mBuckets.push_back(key);
            }
        }
        else
        {
            c.longest = llmax(c.longest, mSpanLength);
            mSpanKey = 0;       // breaks the span
            mSpanLength = 0;
        }
    }

    // Only the batches that get records (fix-r6: a particle's or a cloud's
    // batch counted as drawn without one, every verified pass).
    if (!mVerifyPass || category == CAT_RIGGED || !records_made(group))
    {
        return;
    }
    ++mChecked;
    const auto it = mRecords.find(&params);
    if (it == mRecords.end())
    {
        difference(DIFF_MISSING, &params, "drawn without a record");
        return;
    }
    Record& r = it->second;
    if (r.pending)
    {
        ++mPendingSeen;     // made after this frame's first alpha pass: not compared yet
        return;
    }
    r.seen = mPassSerial;
    if (r.group != group)
    {
        difference(DIFF_GROUP, &params, "its record's group is another");
    }
    if (r.place != place)
    {
        difference(DIFF_PLACE, &params, "its record's place in the draw map is another");
    }
    if (r.hash != content_hash(params))
    {
        difference(DIFF_CONTENT, &params, "changed since its record was written");
    }
    U32 mask = 0;
    const U8 program = bound_program(shader, mask);
    if (program != r.program || (program == PROG_MATERIAL && mask != r.shader_mask))
    {
        std::ostringstream detail;
        detail << "drawn with " << PROGRAM_NAMES[program] << ", its record says " << PROGRAM_NAMES[r.program];
        difference(DIFF_PROGRAM, &params, detail.str().c_str());
    }
    if (mCheckPriority)
    {
        // What the scene's way would give it: the group's priority (a row the
        // CPU writes per frame) with the record's place.
        const U32 expected = oit_group_priority(group, false) | (U32)llmin(r.place, 4094u);
        if (expected != priority)
        {
            std::ostringstream detail;
            detail << "OIT priority " << std::hex << priority << ", its record gives " << expected;
            difference(DIFF_PRIORITY, &params, detail.str().c_str());
        }
    }
}

void LLSceneAlpha::difference(Difference what, const LLDrawInfo* info, const char* detail)
{
    ++mDifferences[what];
    gSceneRecords.alphaChecked(true);       // research/218 vk-s2f: auto goes off
    if (mLogged < LOG_DIFFERENCES)
    {
        ++mLogged;
        LL_WARNS("SceneAlpha") << "verify: " << DIFF_NAMES[what] << ": batch " << (const void*)info << " of group " << (const void*)mGroup
                               << ": " << detail << LL_ENDL;
    }
}

void LLSceneAlpha::report()
{
    const F64 frames = llmax(mReportFrames, 1u);
    if (sCensus)
    {
        for (U32 k = 0; k < KIND_COUNT; ++k)
        {
            const PassCounts& c = mCounts[k];
            if (!c.passes)
            {
                continue;
            }
            U64 scope = c.categories[CAT_FULLBRIGHT] + c.categories[CAT_PLAIN] + c.categories[CAT_TEXTURE_LIST];   // vk-s3a6: H5's too
            std::ostringstream line;
            line << std::fixed << std::setprecision(1);
            line << "census (research/205 3a-0), " << KIND_NAMES[k] << ", " << mReportFrames << " frames: passes " << c.passes / frames
                 << ", batches " << c.batches / frames << " a frame (";
            for (U32 i = 0; i < CAT_COUNT; ++i)
            {
                line << (i ? ", " : "") << CATEGORY_NAMES[i] << ' ' << c.categories[i] / frames;
            }
            line << "); in the scope (3a-3, H5) " << scope / frames << " ("
                 << (c.batches ? 100.0 * (F64)scope / (F64)c.batches : 0.0) << "%; glow " << c.glow / frames << ", animated "
                 << c.animated / frames << "); spans " << c.spans / frames << " a frame, mean "
                 << (c.spans ? (F64)scope / (F64)c.spans : 0.0) << " batches, longest " << c.longest << "; buckets "
                 << (F64)c.buckets / (F64)c.passes << " a pass";
            LL_INFOS("SceneAlpha") << line.str() << LL_ENDL;
        }
    }
    if (sVerify)
    {
        std::ostringstream line;
        line << "verify (" << LLRHI::sceneModeName(LLRHI::sceneAlphaMode()) << "), " << mReportFrames << " frames: records "
             << mRecords.size() << " (made " << mCreated << ", written " << mWritten << ", gone " << mDropped << "), passes "
             << mVerifiedPasses << ", batches checked " << mChecked << " (not written yet " << mPendingSeen << "); differences";
        U64 total = 0;
        for (U32 d = 0; d < DIFF_COUNT; ++d)
        {
            line << (d ? ", " : " ") << DIFF_NAMES[d] << ' ' << mDifferences[d];
            total += mDifferences[d];
        }
        line << (total ? "" : " (none)");
        if (total)
        {
            LL_WARNS("SceneAlpha") << line.str() << LL_ENDL;
        }
        else
        {
            LL_INFOS("SceneAlpha") << line.str() << LL_ENDL;
        }
    }
    for (PassCounts& c : mCounts)
    {
        c = PassCounts();
    }
    mReportFrames = 0;
    mChecked = mPendingSeen = mVerifiedPasses = 0;
    mCreated = mWritten = mDropped = 0;
    for (U64& d : mDifferences)
    {
        d = 0;
    }
}

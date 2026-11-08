/**
 * @file llsceneshadow.cpp
 * @brief The shadow passes' casters drawn from the retained scene's records (research/206, stage 5 of research/197).
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

#include "llsceneshadow.h"

#include "llcamera.h"
#include "llcharacter.h"
#include "llcontrolavatar.h"
#include "lldrawable.h"
#include "lldrawpool.h"
#include "llframetimer.h"
#include "llimagegl.h"
#include "llscenerecords.h"
#include "llspatialpartition.h"
#include "lltimer.h"
#include "llviewerjointattachment.h"
#include "llviewerregion.h"
#include "llviewertexture.h"
#include "llvoavatar.h"
#include "llvovolume.h"
#include "pipeline.h"

#include <algorithm>
#include <cstring>
#include <sstream>

LLSceneShadow gSceneShadow;

namespace
{
    // The opaque types renderShadowCasters draws with shadowV that the scene
    // holds records of (LLSceneRecords::targetPass): not PASS_BUMP, SHINY,
    // FULLBRIGHT_SHINY (no records yet), not GLOW (no shadow pass draws it),
    // not the masks (alpha tested: SHADOW_MASK_TYPES, research/206 5c).
    const U32 SHADOW_OPAQUE_TYPES[] = {
        LLRenderPass::PASS_SIMPLE,          LLRenderPass::PASS_FULLBRIGHT,       LLRenderPass::PASS_MATERIAL,
        LLRenderPass::PASS_MATERIAL_ALPHA_EMISSIVE, LLRenderPass::PASS_SPECMAP,  LLRenderPass::PASS_SPECMAP_EMISSIVE,
        LLRenderPass::PASS_NORMMAP,         LLRenderPass::PASS_NORMMAP_EMISSIVE, LLRenderPass::PASS_NORMSPEC,
        LLRenderPass::PASS_NORMSPEC_EMISSIVE,
    };

    // research/206 5c: the alpha masks drawNativeShadowCasters draws with
    // shadowAlphaMask (PASS_ALPHA_MASK: the vertex alpha too; the fullbright
    // one: not) and treeShadow (the legacy materials' masks), each with the
    // batch's own cutoff (mAlphaMaskCutoff, the record's material value 5),
    // and their packets' modes. Not the rigged maps (type + 1), not
    // PASS_ALPHA (blended: no records of the shadows' kind), not the grass.
    struct MaskType
    {
        U32 type;
        U8 mode;
    };
    const MaskType SHADOW_MASK_TYPES[] = {
        { LLRenderPass::PASS_ALPHA_MASK, (U8)LLRHI::ShadowPacket::MODE_ALPHA_MASK },
        { LLRenderPass::PASS_FULLBRIGHT_ALPHA_MASK, (U8)LLRHI::ShadowPacket::MODE_ALPHA_MASK_FULLBRIGHT },
        { LLRenderPass::PASS_NORMSPEC_MASK, (U8)LLRHI::ShadowPacket::MODE_ALPHA_TEST },
        { LLRenderPass::PASS_MATERIAL_ALPHA_MASK, (U8)LLRHI::ShadowPacket::MODE_ALPHA_TEST },
        { LLRenderPass::PASS_SPECMAP_MASK, (U8)LLRHI::ShadowPacket::MODE_ALPHA_TEST },
        { LLRenderPass::PASS_NORMMAP_MASK, (U8)LLRHI::ShadowPacket::MODE_ALPHA_TEST },
    };

    const char* const LAYER_NAMES[LLSceneShadow::LAYER_COUNT] = { "none", "moving", "cache", "spot" };

    static_assert(LLViewerRegion::NUM_PARTITIONS < LLSceneShadow::PARTITION_SLOTS, "a slot per partition type and one past");

    const char* partitionName(U32 type)
    {
        switch (type)
        {
        case LLViewerRegion::PARTITION_HUD:          return "hud";
        case LLViewerRegion::PARTITION_TERRAIN:      return "terrain";
        case LLViewerRegion::PARTITION_VOIDWATER:    return "void water";
        case LLViewerRegion::PARTITION_WATER:        return "water";
        case LLViewerRegion::PARTITION_TREE:         return "tree";
        case LLViewerRegion::PARTITION_PARTICLE:     return "particle";
        case LLViewerRegion::PARTITION_GRASS:        return "grass";
        case LLViewerRegion::PARTITION_VOLUME:       return "volume";
        case LLViewerRegion::PARTITION_BRIDGE:       return "bridge";
        case LLViewerRegion::PARTITION_AVATAR:       return "avatar";
        case LLViewerRegion::PARTITION_CONTROL_AV:   return "control av";
        case LLViewerRegion::PARTITION_HUD_PARTICLE: return "hud particle";
        case LLViewerRegion::PARTITION_VO_CACHE:     return "vo cache";
        default:                                     return "other";
        }
    }

    // research/206 H2 (research/218 vk-s0b): the batches of a spatial bridge's
    // groups (an attachment's linkset, an animesh), rigged or not. Read every
    // REPORT_SECONDS only.
    struct BridgeBatches
    {
        U32 bridges = 0;
        U32 groups = 0;
        U64 rigged = 0;
        U64 plain = 0;
    };

    void countNode(const OctreeNode* node, BridgeBatches& out)
    {
        const LLSpatialGroup* group = node ? (const LLSpatialGroup*)node->getListener(0) : nullptr;
        if (!group)
        {
            return;
        }
        ++out.groups;
        for (const auto& batches : group->mDrawMap)
        {
            for (const LLDrawInfo* info : batches.second)
            {
                (info && info->mAvatar.notNull() ? out.rigged : out.plain) += 1;
            }
        }
        for (U32 i = 0; i < node->getChildCount(); ++i)
        {
            countNode(node->getChild(i), out);
        }
    }

    void countDrawable(LLDrawable* drawable, BridgeBatches& out)
    {
        LLSpatialBridge* bridge = drawable && !drawable->isDead() ? drawable->getSpatialBridge() : nullptr;
        if (bridge && !bridge->isDead() && bridge->mOctree)
        {
            ++out.bridges;
            countNode(bridge->mOctree, out);
        }
    }
}

U8 LLSceneShadow::casterMode(U32 type)
{
    // research/219: a rigged map's batches as its base's (rhi_add_packet skins them by their avatar)
    const U32 base = LLSceneRecords::riggedBase(type);
    type = base != U32_MAX ? base : type;
    for (const MaskType& mask : SHADOW_MASK_TYPES)
    {
        if (mask.type == type)
        {
            return mask.mode;
        }
    }
    return (U8)LLRHI::ShadowPacket::MODE_OPAQUE;
}

// The head of a call (the sun's cascades, or a spot pass): the mode logged
// once, and the backend's own checks (the GPU's choices against the CPU's,
// also of these views): a new difference stops the cuts as one of ours does.
void LLSceneShadow::openCall(LLRHI::SceneMode mode)
{
    const U32 problems = LLRHI::sScene->problems();
    if (problems != mSeenProblems)
    {
        if (mProblemsKnown && mode == LLRHI::SceneMode::On)
        {
            stop("the backend's checks found a difference (VKPerf scene line)");
        }
        else if (mProblemsKnown)
        {
            autoCheck(true, "the backend's checks found a difference (VKPerf scene line)");    // research/218 vk-s2f
        }
        mSeenProblems = problems;
    }
    mProblemsKnown = true;
}

// research/218 vk-s2f: auto's step at a check of a layer (several in a frame
// count once), or at a difference found elsewhere. Nothing while
// LLRHI_SCENE_SHADOW asked for another mode.
void LLSceneShadow::autoCheck(bool differ, const char* why)
{
    if (LLRHI::sceneShadowMode() != LLRHI::SceneMode::Auto)
    {
        return;
    }
    const LLRHI::SceneMode before = mAuto.now;
    const LLRHI::SceneMode now = mAuto.step(LLFrameTimer::getFrameCount(), differ);
    if (now == before)
    {
        return;
    }
    if (now == LLRHI::SceneMode::On)
    {
        LL_INFOS("Scene") << "LLRHI_SCENE_SHADOW=auto: " << mAuto.clean_frames
                          << " world frames without a difference, the casters are cut from the next call on" << LL_ENDL;
    }
    else
    {
        LL_WARNS("Scene") << "LLRHI_SCENE_SHADOW=auto: off for the run after " << mAuto.clean_frames << " clean world frames (" << why
                          << "); the shadow casters are drawn the usual way from the next call on" << LL_ENDL;
    }
}

// The types the sort or postSort may cut: as they list a group's batches (hasRenderType of the type).
// 5c: the masks too once the backend draws them. research/219: and each one's rigged map (type + 1)
// while the native shadows draw the rigged maps, the backend draws skinned shadows and the rigged
// passes are on (or this call only chooses: verify); skinned (SCENE_SHADOW_SKINNED) by their rows.
void LLSceneShadow::chooseTypes(bool cutting)
{
    memset(mCutType, 0, sizeof(mCutType));
    memset(mMaskBits, 0, sizeof(mMaskBits));
    mPasses.clear();
    mModes.clear();
    mMasks = false;
    mRigged = false;
    const bool rigged = LLRHI::sNativeRiggedShadows && LLRHI::sScene && LLRHI::sScene->shadowSkins() &&
                        LLRHI::sceneRiggedMode() != LLRHI::SceneMode::Off &&
                        (!cutting || gSceneRecords.riggedMode() == LLRHI::SceneMode::On);
    const auto add = [&](U32 type, U8 mode, bool mask)
    {
        if (type < LLRHI::SCENE_MAX_PASSES && LLSceneRecords::targetPass(type) && gPipeline.hasRenderType(type))
        {
            mCutType[type] = true;
            mPasses.push_back(type);
            mModes.push_back(mode);
            if (mask)
            {
                mMaskBits[type / 64] |= 1ull << (type % 64);
                mMasks = true;
            }
            mRigged = mRigged || (mode & LLRHI::SCENE_SHADOW_SKINNED) != 0;
        }
    };
    for (U32 type : SHADOW_OPAQUE_TYPES)
    {
        add(type, (U8)LLRHI::ShadowPacket::MODE_OPAQUE, false);
        if (rigged)
        {
            add(type + 1, (U8)(LLRHI::ShadowPacket::MODE_OPAQUE | LLRHI::SCENE_SHADOW_SKINNED), false);
        }
    }
    if (!LLRHI::sScene || !LLRHI::sScene->shadowMasks())
    {
        return;
    }
    for (const MaskType& mask : SHADOW_MASK_TYPES)
    {
        add(mask.type, mask.mode, true);
        if (rigged)
        {
            add(mask.type + 1, (U8)(mask.mode | LLRHI::SCENE_SHADOW_SKINNED), true);
        }
    }
}

void LLSceneShadow::beginCascades(bool eligible, U32 count, const S32* indices, bool probe)
{
    mInCall = false;
    mCutting = false;
    mPosition = U32_MAX;
    mLayer = LAYER_NONE;
    mCount = 0;
    mGroupIds.clear();
    const LLRHI::SceneMode asked = LLRHI::sceneShadowMode();
    if (asked == LLRHI::SceneMode::Off)
    {
        return;
    }
    if (!mLoggedMode)
    {
        mLoggedMode = true;
        LL_INFOS("Scene") << "scene shadow: " << LLRHI::sceneModeName(asked)
                          << " (LLRHI_SCENE_SHADOW, research/206): the sun shadow cascades' moving casters and cached depths, and the spot "
                             "light shadows, from the retained scene"
                          << (LLRHI::sScene ? "" : "; no retained scene on this device: nothing to draw them from") << LL_ENDL;
        mReportAt = LLTimer::getTotalSeconds() + REPORT_SECONDS;
    }
    if (!eligible || !gSceneRecords.active() || !count || !indices || mode() == LLRHI::SceneMode::Off)
    {
        return;     // research/218 vk-s2f: mode() off: its auto found a difference
    }
    openCall(mode());
    const LLRHI::SceneMode now = mode();    // research/218 vk-s2f: auto may have gone off at the backend's checks
    if (now == LLRHI::SceneMode::Off)
    {
        return;
    }
    mInCall = true;
    mCount = llmin(count, CASCADES);
    for (U32 k = 0; k < CASCADES; ++k)
    {
        mIndex[k] = k < mCount ? indices[k] : -1;
    }
    ++mCalls;
    ++mReportCalls;
    mReportProbeCalls += probe ? 1 : 0;
    mCutting = now == LLRHI::SceneMode::On && !mStopped;
    chooseTypes(mCutting);
    const U32 words = (gSceneRecords.groupSpan() + 31) / 32;
    for (U32 k = 0; k < CASCADES; ++k)
    {
        mBits[k].assign(k < mCount ? words : 0, 0u);
        mCacheBits[k].assign(k < mCount ? words : 0, 0u);
        mMarked[k] = 0;
        mCacheMarked[k] = 0;
        mLayerTextures[0][k].clear();
        mLayerTextures[1][k].clear();
    }
}

void LLSceneShadow::endCascades()
{
    if (!mInCall)
    {
        return;
    }
    mInCall = false;
    mCutting = false;
    mPosition = U32_MAX;
    mLayer = LAYER_NONE;
    report();
}

// The groups of the volume and bridge partitions, and (research/218 vk-s4b2)
// the attachments' and animesh's while the shadows may take them
// (LLSceneRecords::shadowTakes; the scene holds records of no other): their
// ids when resident with every record placed. The sort decides per group
// which layer (if any) takes it (the worn objects' groups are bridges: moving).
void LLSceneShadow::noteGroups(const std::vector<LLSpatialGroup*>& groups)
{
    mGroupIds.clear();
    mGroupTextures.clear();
    mGroupTexFirst.clear();
    mGroupTexCount.clear();
    if (!mInCall || mPasses.empty())
    {
        return;
    }
    mGroupIds.assign(groups.size(), 0u);
    if (mMasks)
    {
        mGroupTexFirst.assign(groups.size(), 0u);
        mGroupTexCount.assign(groups.size(), 0u);
    }
    // research/206 H2's census on a sampled call (the attachments' batches are read then only).
    const bool sample = mCalls % VERIFY_EVERY == 0;
    mCensusSamples += sample ? 1 : 0;
    for (size_t g = 0; g < groups.size(); ++g)
    {
        LLSpatialGroup* group = groups[g];
        const LLSpatialPartition* part = group ? group->getSpatialPartition() : nullptr;
        if (!part || group->isDead())
        {
            continue;
        }
        const U32 type = part->mPartitionType;
        ++mCensusPartitions[type < PARTITION_SLOTS - 1 ? type : PARTITION_SLOTS - 1];  // research/218 vk-s0b
        // research/218 vk-s4b2: an attachment's and an animesh's groups as a bridge's (the sort's moving layer).
        const bool worn = type == LLViewerRegion::PARTITION_AVATAR || type == LLViewerRegion::PARTITION_CONTROL_AV;
        if (worn)
        {
            ++mCensusAttachment;
            if (sample)
            {
                // research/206 H2: the rigged batches (research/219: records with LLRHI_SCENE_RIGGED not off).
                mCensusAnimesh += type == LLViewerRegion::PARTITION_CONTROL_AV ? 1 : 0;
                for (const auto& batches : group->mDrawMap)
                {
                    for (const LLDrawInfo* info : batches.second)
                    {
                        (info && info->mAvatar.notNull() ? mCensusAttachRigged : mCensusAttachPlain) += 1;
                    }
                }
            }
        }
        if (type == LLViewerRegion::PARTITION_VOLUME || type == LLViewerRegion::PARTITION_BRIDGE || worn)
        {
            // An id past the views' bits (none is given out during a call) would be cut without a bit to draw it by.
            U32 id = LLSceneRecords::shadowTakes(type, mCutting) ? gSceneRecords.shadowGroup(group) : 0;
            id = id && (id - 1) / 32 < mBits[0].size() ? id : 0;
            // 5c: the masks' textures, every one of them samplable from its number now (else the
            // group stays on the usual path this time, all of it).
            if (id && mMasks)
            {
                const size_t first = mGroupTextures.size();
                if (gSceneRecords.shadowTextures(group, mMaskBits, mGroupTextures))
                {
                    mGroupTexFirst[g] = (U32)first;
                    mGroupTexCount[g] = (U32)(mGroupTextures.size() - first);
                }
                else
                {
                    id = 0;
                    ++mCensusMaskWait;
                }
            }
            // research/219: its rigged records' palettes in their rows (the cut layers draw them).
            if (id && mRigged && mCutting && !gSceneRecords.shadowSkinsReady(group))
            {
                id = 0;
                ++mCensusSkinWait;
            }
            mGroupIds[g] = id;
            if (worn)
            {
                mCensusAttachmentScene += id ? 1 : 0;
            }
            else
            {
                const bool volume = type == LLViewerRegion::PARTITION_VOLUME;
                (volume ? mCensusVolume : mCensusBridge) += 1;
                (volume ? mCensusVolumeScene : mCensusBridgeScene) += id ? 1 : 0;
            }
        }
        else
        {
            ++mCensusOther;
        }
    }
}

void LLSceneShadow::markGroup(U32 id, U32 moving, U32 cached, size_t g)
{
    if (!mInCall || !id)
    {
        return;
    }
    const U32 bit = id - 1;
    bool any_moving = false, any_cached = false;
    // 5c: the group's masks' textures go with it into each layer it is marked in.
    const bool textures = g < mGroupTexCount.size() && mGroupTexCount[g];
    const auto textures_begin = textures ? mGroupTextures.begin() + mGroupTexFirst[g] : mGroupTextures.end();
    const auto textures_end = textures ? textures_begin + mGroupTexCount[g] : mGroupTextures.end();
    for (U32 k = 0; k < mCount; ++k)
    {
        const U32 position = 1u << k;
        if ((moving & position) && bit / 32 < mBits[k].size())
        {
            mBits[k][bit / 32] |= 1u << (bit % 32);
            ++mMarked[k];
            any_moving = true;
            mLayerTextures[0][k].insert(mLayerTextures[0][k].end(), textures_begin, textures_end);
        }
        if ((cached & position) && bit / 32 < mCacheBits[k].size())
        {
            mCacheBits[k][bit / 32] |= 1u << (bit % 32);
            ++mCacheMarked[k];
            any_cached = true;
            mLayerTextures[1][k].insert(mLayerTextures[1][k].end(), textures_begin, textures_end);
        }
    }
    mGroupsCut += any_moving ? 1 : 0;
    mCacheGroups += any_cached ? 1 : 0;
}

LLSceneShadow::CascadeScope::CascadeScope(U32 position)
{
    gSceneShadow.mPosition = position;
    gSceneShadow.mLayer = LAYER_NONE;
}

LLSceneShadow::CascadeScope::~CascadeScope()
{
    gSceneShadow.mPosition = U32_MAX;
    gSceneShadow.mLayer = LAYER_NONE;
}

LLSceneShadow::MovingScope::MovingScope() : mSaved(gSceneShadow.mLayer)
{
    gSceneShadow.mLayer = LAYER_MOVING;
}

LLSceneShadow::MovingScope::~MovingScope()
{
    gSceneShadow.mLayer = mSaved;
}

LLSceneShadow::CacheScope::CacheScope() : mSaved(gSceneShadow.mLayer)
{
    gSceneShadow.mLayer = LAYER_CACHE;
    gSceneShadow.mCacheDiffered = false;
}

LLSceneShadow::CacheScope::~CacheScope()
{
    gSceneShadow.mLayer = mSaved;
}

// research/206 5b (H7): a spot light's shadow pass. Its groups are marked in
// its postSort (markSpotGroup); its cull and draw are one renderShadow.
LLSceneShadow::SpotScope::SpotScope(U32 spot, bool eligible) : mSaved(gSceneShadow.mLayer)
{
    LLSceneShadow& s = gSceneShadow;
    s.endSpot();
    s.mLayer = LAYER_SPOT;
    if (s.mode() == LLRHI::SceneMode::Off || !eligible || spot >= SPOTS || s.mInCall || !gSceneRecords.active())
    {
        return;     // research/218 vk-s2f: mode(): or its auto found a difference
    }
    s.openCall(s.mode());
    const LLRHI::SceneMode mode = s.mode();     // research/218 vk-s2f: auto may have gone off at the backend's checks
    if (mode == LLRHI::SceneMode::Off)
    {
        return;
    }
    const bool cutting = mode == LLRHI::SceneMode::On && !s.mStopped;
    s.chooseTypes(cutting);
    if (s.mPasses.empty())
    {
        return;
    }
    s.mSpot = spot;
    s.mSpotBits.assign((gSceneRecords.groupSpan() + 31) / 32, 0u);
    s.mSpotGroups.clear();
    s.mSpotCutting = cutting;
    s.mSpotOpen = true;
    ++s.mReportSpots;
}

LLSceneShadow::SpotScope::~SpotScope()
{
    LLSceneShadow& s = gSceneShadow;
    const bool was_open = s.mSpotOpen;
    s.endSpot();
    s.mLayer = mSaved;
    if (was_open)
    {
        s.report();
    }
}

void LLSceneShadow::endSpot()
{
    mSpotOpen = false;
    mSpotCutting = false;
    mSpotGroups.clear();
    mSpotTextures.clear();
}

bool LLSceneShadow::markSpotGroup(LLSpatialGroup* group)
{
    if (!mSpotOpen || !group)
    {
        return false;
    }
    // research/218 vk-s4b2: a worn object's group only while it may be cut (or the pass only chooses).
    const LLSpatialPartition* part = group->getSpatialPartition();
    if (!part || !LLSceneRecords::shadowTakes(part->mPartitionType, mSpotCutting))
    {
        return false;
    }
    const U32 id = gSceneRecords.shadowGroup(group);
    if (!id || (id - 1) / 32 >= mSpotBits.size())
    {
        return false;
    }
    // research/219: its rigged records' palettes in their rows (a cutting pass draws them).
    if (mRigged && mSpotCutting && !gSceneRecords.shadowSkinsReady(group))
    {
        ++mCensusSkinWait;
        return false;
    }
    // 5c: its masks' textures, every one samplable now (else the usual path draws the group).
    if (mMasks && !gSceneRecords.shadowTextures(group, mMaskBits, mSpotTextures))
    {
        ++mCensusMaskWait;
        return false;
    }
    const U32 bit = id - 1;
    mSpotBits[bit / 32] |= 1u << (bit % 32);
    mSpotGroups.push_back(group);
    ++mSpotGroupsCut;
    return mSpotCutting;
}

bool LLSceneShadow::layerActive() const
{
    if (!LLRHI::sScene)
    {
        return false;
    }
    switch (mLayer)
    {
        case LAYER_MOVING:
        case LAYER_CACHE:
            if (!mInCall || mPosition >= mCount || mIndex[mPosition] < 0 || mIndex[mPosition] >= (S32)CASCADES)
            {
                return false;
            }
            return mLayer == LAYER_MOVING ? mMarked[mPosition] != 0 : mCacheMarked[mPosition] != 0;
        case LAYER_SPOT:
            return mSpotOpen && !mSpotGroups.empty();
        default:
            return false;
    }
}

bool LLSceneShadow::layerCut() const
{
    return layerActive() && (mLayer == LAYER_SPOT ? mSpotCutting : mCutting);
}

U32 LLSceneShadow::layerView() const
{
    return mLayer == LAYER_SPOT ? LLRHI::SCENE_VIEW_SPOT0 + mSpot : LLRHI::SCENE_VIEW_SUN0 + (U32)mIndex[mPosition];
}

const std::vector<U32>* LLSceneShadow::layerBits() const
{
    switch (mLayer)
    {
        case LAYER_MOVING:
            return &mBits[mPosition];
        case LAYER_CACHE:
            return &mCacheBits[mPosition];
        case LAYER_SPOT:
            return &mSpotBits;
        default:
            return nullptr;
    }
}

const std::vector<LLViewerTexture*>* LLSceneShadow::layerTextures() const
{
    switch (mLayer)
    {
        case LAYER_MOVING:
            return &mLayerTextures[0][mPosition];
        case LAYER_CACHE:
            return &mLayerTextures[1][mPosition];
        case LAYER_SPOT:
            return &mSpotTextures;
        default:
            return nullptr;
    }
}

// research/206 5c: the layer's masks' textures, each once, as their numbers
// and GL names now (sorted by number), and marked active when the scene draws
// them (what the usual way's draws of them do). False: one cannot be sampled
// from its number any more (it could when its group was marked).
bool LLSceneShadow::listLayerTextures()
{
    mTexPairs.clear();
    mTexIds.clear();
    mTexNames.clear();
    const std::vector<LLViewerTexture*>* textures = mMasks ? layerTextures() : nullptr;
    if (!textures || textures->empty())
    {
        return true;
    }
    mTexScratch.assign(textures->begin(), textures->end());
    std::sort(mTexScratch.begin(), mTexScratch.end());
    mTexScratch.erase(std::unique(mTexScratch.begin(), mTexScratch.end()), mTexScratch.end());
    const bool cut = layerCut();
    bool all = true;
    for (LLViewerTexture* texture : mTexScratch)
    {
        const LLImageGL* image = texture->getGLTexture();
        const U32 id = image ? image->rhiTex().id : 0;
        const U32 name = image ? image->getTexName() : 0;
        if (!id || !name || image->rhiOptionsDirty())
        {
            all = false;
            continue;
        }
        if (cut)
        {
            texture->setActive();
        }
        mTexPairs.emplace_back(id, name);
    }
    std::sort(mTexPairs.begin(), mTexPairs.end());
    for (const std::pair<U32, U32>& pair : mTexPairs)
    {
        mTexIds.push_back(pair.first);
        mTexNames.push_back(pair.second);
    }
    mLayerTextureCount += mTexIds.size();
    return all;
}

bool LLSceneShadow::drawLayer(const F32* view_proj, LLCamera& camera)
{
    mVerifyDue = false;
    if (!layerActive())
    {
        return false;
    }
    // 5c: a mask's texture that cannot be sampled now: the usual way draws the layer this time
    // (verifying: it is not compared this time).
    const bool textures_ready = listLayerTextures();
    if (!textures_ready)
    {
        ++mLayerTexturesWait;
        if (layerCut())
        {
            ++mLayers[mLayer];
            ++mLayersFailed[mLayer];
            return false;
        }
    }
    const U32 serial = mLayerSerial[mLayer]++;
    ++mLayers[mLayer];
    // The moving casters: every VERIFY_EVERY calls one cascade in turn (5a); a cached depth and a spot
    // pass: every VERIFY_EVERY of their own.
    mVerifyDue = mLayer == LAYER_MOVING ? mCount && mCalls % VERIFY_EVERY == 0 && mPosition == (mCalls / VERIFY_EVERY) % mCount
                                        : serial % VERIFY_EVERY == 0;
    mVerifyDue = mVerifyDue && textures_ready;
    const U32 view = layerView();
    // The pass camera's four side planes only: a sun cascade's passes clamp
    // depth, so the near and far planes (and the water's) leave out no pixel;
    // a batch wholly outside a side plane draws none (as the GPU shadow
    // list's rows, research/44 P1). A spot pass does not clamp depth, but a
    // batch it keeps beyond its near or far plane is only clipped. The
    // group's visible bit is the CPU cull's.
    LLRHI::SceneViewDesc desc;
    const U32 sides[4] = { LLCamera::AGENT_PLANE_LEFT, LLCamera::AGENT_PLANE_RIGHT, LLCamera::AGENT_PLANE_BOTTOM,
                           LLCamera::AGENT_PLANE_TOP };
    for (U32 i = 0; i < 4; ++i)
    {
        const LLPlane& p = camera.getAgentPlane(sides[i]);
        for (U32 j = 0; j < 4; ++j)
        {
            desc.planes[i][j] = p[j];
        }
    }
    desc.plane_count = 4;
    const LLVector3& origin = camera.getOrigin();
    for (U32 j = 0; j < 3; ++j)
    {
        desc.origin[j] = origin.mV[j];
    }
    const std::vector<U32>& bits = *layerBits();
    LLRHI::sScene->setView(view, desc);
    LLRHI::sScene->setGroupVisible(view, bits.data(), (U32)bits.size());
    LLRHI::SceneShadowContext ctx;
    ctx.passes = mPasses.data();
    ctx.pass_count = (U32)mPasses.size();
    ctx.view_proj = view_proj;
    ctx.draw = layerCut();
    ctx.modes = mModes.data();      // 5c
    ctx.textures = mTexIds.data();
    ctx.texture_names = mTexNames.data();
    ctx.texture_count = (U32)mTexIds.size();
    const bool drawn = LLRHI::sScene->drawShadow(view, ctx);
    mLayersFailed[mLayer] += drawn ? 0 : 1;
    return drawn;
}

// The cut (or, verifying, cuttable) batches of the layer against the view's
// choice as the CPU works it out from the same tables: every chosen record
// is one of them, every one of them was chosen or left out by its own box
// only, and each still is what its record was written from.
void LLSceneShadow::verifyLayer(const std::vector<std::pair<U32, LLDrawInfo*> >& batches)
{
    if (!layerActive())
    {
        return;
    }
    ++mVerified[mLayer];
    const U32 view = layerView();
    U32 chosen_not_drawn = 0, drawn_not_chosen = 0, no_record = 0, content = 0, texture_missing = 0;
    // 5c: a mask's texture is among those resolved for the layer (else its record would sample
    // whatever slot the table kept for the number).
    for (const std::pair<U32, LLDrawInfo*>& batch : batches)
    {
        if (!batch.second || casterMode(batch.first) == LLRHI::ShadowPacket::MODE_OPAQUE || batch.second->mTexture.isNull())
        {
            continue;
        }
        const LLImageGL* image = batch.second->mTexture->getGLTexture();
        const U32 id = image ? image->rhiTex().id : 0;
        texture_missing += id && std::binary_search(mTexIds.begin(), mTexIds.end(), id) ? 0 : 1;
    }
    bool rigged_seen = false, rigged_differ = false;     // research/219: the rigged maps' share
    for (U32 pass : mPasses)
    {
        const U32 differ_before = chosen_not_drawn + drawn_not_chosen + no_record + content;
        mOld.clear();
        for (const std::pair<U32, LLDrawInfo*>& batch : batches)
        {
            if (batch.first != pass || !batch.second)
            {
                continue;
            }
            bool same = false;
            const U32 record = gSceneRecords.shadowRecord(*batch.second, pass, same);
            if (!record)
            {
                ++no_record;
                continue;
            }
            content += same ? 0 : 1;
            mOld.push_back(record);
        }
        LLRHI::sScene->collect(view, pass, mDrawn, mBounded);
        std::sort(mOld.begin(), mOld.end());
        std::sort(mDrawn.begin(), mDrawn.end());
        std::sort(mBounded.begin(), mBounded.end());
        for (U32 record : mDrawn)
        {
            chosen_not_drawn += std::binary_search(mOld.begin(), mOld.end(), record) ? 0 : 1;
        }
        for (U32 record : mOld)
        {
            drawn_not_chosen += (std::binary_search(mDrawn.begin(), mDrawn.end(), record) ||
                                 std::binary_search(mBounded.begin(), mBounded.end(), record)) ? 0 : 1;
        }
        if (LLSceneRecords::riggedBase(pass) != U32_MAX)
        {
            const U32 differ_now = chosen_not_drawn + drawn_not_chosen + no_record + content;
            rigged_seen = rigged_seen || !mOld.empty() || !mDrawn.empty() || differ_now != differ_before;
            rigged_differ = rigged_differ || differ_now != differ_before;
        }
    }
    if (rigged_seen)
    {
        gSceneRecords.riggedChecked(rigged_differ, "a shadow layer's verify found a difference in a rigged map");
    }
    mChosenNotDrawn += chosen_not_drawn;
    mDrawnNotChosen += drawn_not_chosen;
    mNoRecord += no_record;
    mContent += content;
    mTextureMissing += texture_missing;
    const bool differ = chosen_not_drawn || drawn_not_chosen || no_record || content || texture_missing;
    autoCheck(differ, "its verify found a difference");     // research/218 vk-s2f: a check of the shadows' world frame
    if (differ)
    {
        ++mDifferences;
        if (mLogged < 8)
        {
            ++mLogged;
            LL_WARNS("Scene") << "scene shadow verify: " << LAYER_NAMES[mLayer] << " "
                              << (mLayer == LAYER_SPOT ? mSpot : (U32)mIndex[mPosition]) << ": chosen not drawn " << chosen_not_drawn
                              << ", drawn not chosen " << drawn_not_chosen << ", batches without a record " << no_record
                              << ", records that differ from their batch " << content << ", masks whose texture was not resolved "
                              << texture_missing << LL_ENDL;
        }
        if (layerCut())
        {
            mCacheDiffered = mCacheDiffered || mLayer == LAYER_CACHE;
            stop("its verify found a difference");
        }
    }
}

void LLSceneShadow::rescued(U32 batches, bool native_failed)
{
    if (native_failed)
    {
        mRescuedMap += batches;
        stop("a native shadow pass failed with the scene's casters in it");
    }
    else
    {
        mRescuedPackets += batches;
    }
}

// From the next call or spot pass on (this one's cut batches are drawn by the
// scene or the rescue: its passes still expect that).
void LLSceneShadow::stop(const char* why)
{
    autoCheck(true, why);   // research/218 vk-s2f: auto's on goes off for the run (nothing chosen from the next call)
    if (mStopped)
    {
        return;
    }
    mStopped = true;
    LL_WARNS("Scene") << "scene shadow: " << why << "; the shadow casters are drawn the usual way from the next frame on" << LL_ENDL;
}

void LLSceneShadow::report()
{
    const F64 now = LLTimer::getTotalSeconds();
    if (now < mReportAt)
    {
        return;
    }
    mReportAt = now + REPORT_SECONDS;
    const F64 per = 1.0 / llmax(mReportCalls, 1u);
    const LLRHI::SceneMode asked = LLRHI::sceneShadowMode();
    const LLRHI::SceneMode doing = mode();
    LL_INFOS("Scene") << "scene shadow (" << LLRHI::sceneModeName(asked)
                      << (asked == LLRHI::SceneMode::Auto ? std::string(": ") + LLRHI::sceneModeName(doing) + ", clean " +
                                                                std::to_string(mAuto.clean_frames)
                                                          : std::string())
                      << ", " << (mStopped ? "stopped" : doing == LLRHI::SceneMode::On ? "cutting" : "choosing") << "): calls " << mReportCalls
                      << " (probes' shared maps " << mReportProbeCalls << ")"
                      << "; moving: groups marked " << mGroupsCut * per << " a call, batches marked (x cascades) " << mBatchesCut * per
                      << " a call, layers " << mLayers[LAYER_MOVING] << " (not drawn by the scene " << mLayersFailed[LAYER_MOVING]
                      << "); cached depths: groups marked " << mCacheGroups * per << " a call, layers drawn again " << mLayers[LAYER_CACHE]
                      << " (not drawn by the scene " << mLayersFailed[LAYER_CACHE] << "), batches cut " << mCacheBatchesCut
                      << "; spot passes " << mReportSpots << ", groups marked " << mSpotGroupsCut << ", layers " << mLayers[LAYER_SPOT]
                      << " (not drawn by the scene " << mLayersFailed[LAYER_SPOT] << "); the union's groups a call: volume "
                      << mCensusVolume * per << " (the scene's " << mCensusVolumeScene * per << "), bridge " << mCensusBridge * per
                      << " (the scene's " << mCensusBridgeScene * per << "), attachments and animesh " << mCensusAttachment * per
                      << " (the scene's " << mCensusAttachmentScene * per << "), other " << mCensusOther * per << "; rescued as packets " << mRescuedPackets
                      << ", into the render map " << mRescuedMap << "; verify " << mVerified[LAYER_MOVING] << " moving, "
                      << mVerified[LAYER_CACHE] << " cache, " << mVerified[LAYER_SPOT] << " spot, " << mDifferences
                      << " with differences (chosen not drawn " << mChosenNotDrawn << ", drawn not chosen " << mDrawnNotChosen
                      << ", without a record " << mNoRecord << ", content " << mContent << ", masks' textures not resolved "
                      << mTextureMissing << ")" << LL_ENDL;
    // research/206 5c (the masks) and H2 (what attachments and animesh would bring, sampled calls).
    const F64 per_sample = 1.0 / llmax(mCensusSamples, 1u);
    LL_INFOS("Scene") << "scene shadow masks: " << (mMasks ? "drawn by the scene" : "the usual way (the backend's mask shaders not ready, or off)")
                      << "; rigged maps (research/219): " << (mRigged ? "skinned by the scene" : "the usual way")
                      << "; groups left out for their masks' textures " << mCensusMaskWait * per
                      << " a call, for their rigged records' palettes (research/219) " << mCensusSkinWait * per
                      << " a call (spot passes' too), layers whose textures could not all be sampled "
                      << mLayerTexturesWait << ", textures resolved for the layers " << mLayerTextureCount
                      << "; H2: attachments' and animesh's batches a call, rigged (research/219: records with LLRHI_SCENE_RIGGED) "
                      << mCensusAttachRigged * per_sample
                      << ", not rigged (research/218 vk-s4b2: records) " << mCensusAttachPlain * per_sample << ", animesh groups "
                      << mCensusAnimesh * per_sample
                      << " (" << mCensusSamples << " sampled calls)" << LL_ENDL;
    reportAvatars();
    std::ostringstream partitions;
    for (U32 p = 0; p < PARTITION_SLOTS; ++p)
    {
        if (mCensusPartitions[p])
        {
            partitions << (partitions.tellp() > 0 ? ", " : "") << partitionName(p) << " " << mCensusPartitions[p] * per;
        }
    }
    LL_INFOS("Scene") << "scene shadow H2 (research/218 vk-s0b): the union's groups a call by partition type: "
                      << (partitions.tellp() > 0 ? partitions.str() : std::string("none")) << LL_ENDL;
    memset(mCensusPartitions, 0, sizeof(mCensusPartitions));
    mReportCalls = mReportSpots = mReportProbeCalls = 0;
    mGroupsCut = mBatchesCut = mCacheGroups = mCacheBatchesCut = mSpotGroupsCut = 0;
    mCensusVolume = mCensusVolumeScene = mCensusBridge = mCensusBridgeScene = mCensusAttachment = mCensusAttachmentScene = mCensusOther = 0;
    mCensusMaskWait = mCensusAttachRigged = mCensusAttachPlain = mCensusAnimesh = 0;
    mCensusSkinWait = 0;
    mCensusSamples = 0;
    mLayerTextureCount = 0;
    mLayerTexturesWait = 0;
    memset(mLayers, 0, sizeof(mLayers));
    memset(mLayersFailed, 0, sizeof(mLayersFailed));
    memset(mVerified, 0, sizeof(mVerified));
    mRescuedPackets = mRescuedMap = 0;
    mDifferences = mChosenNotDrawn = mDrawnNotChosen = mNoRecord = mContent = mTextureMissing = 0;
}

// research/206 H2, from the avatars (research/218 vk-s0b). noteGroups counts
// the attachments' and animesh's groups that reach the union of the cascades'
// culls; that stays 0 when none gets there (rigged-5 in bugloop round 11: no
// group of the avatar partitions at all). The sun cascades skip a whole
// avatar's attachments for an impostor, a jellydoll, a muted avatar, one not
// fully loaded or not seen by the main view this frame (LLSpatialBridge::
// setVisible, LLPipeline::markVisible). This counts the avatars by those
// reasons and their attachments' batches, all of them and those of the
// avatars drawn in full: the bound H2 could take whatever the cull leaves out.
// Every REPORT_SECONDS, from report(): a walk of the avatars, not a frame's work.
void LLSceneShadow::reportAvatars()
{
    U32 avatars = 0, visible = 0, full = 0, impostors = 0, jellydolls = 0, muted = 0, loading = 0, attachments = 0;
    U32 animesh = 0, animesh_visible = 0;
    BridgeBatches all, drawn, animesh_batches;
    for (LLCharacter* character : LLCharacter::sInstances)
    {
        LLVOAvatar* avatar = dynamic_cast<LLVOAvatar*>(character);
        if (!avatar || avatar->isDead() || avatar->isUIAvatar())
        {
            continue;
        }
        if (avatar->isControlAvatar())
        {
            ++animesh;
            animesh_visible += avatar->isVisible() ? 1 : 0;
            LLVOVolume* root = static_cast<LLControlAvatar*>(avatar)->mRootVolp;
            if (root && !root->isDead())
            {
                countDrawable(root->mDrawable, animesh_batches);
            }
            continue;
        }
        ++avatars;
        const bool seen = avatar->isVisible();
        const bool impostor = avatar->isImpostor();
        const bool jellydoll = avatar->getOverallAppearance() == LLVOAvatar::AOA_JELLYDOLL;
        const bool in_mute_list = avatar->isInMuteList();
        const bool loaded = avatar->isFullyLoaded();
        visible += seen ? 1 : 0;
        impostors += impostor ? 1 : 0;
        jellydolls += jellydoll ? 1 : 0;
        muted += in_mute_list ? 1 : 0;
        loading += loaded ? 0 : 1;
        const bool drawn_in_full = seen && !impostor && !jellydoll && !in_mute_list && loaded;
        full += drawn_in_full ? 1 : 0;
        for (const auto& point : avatar->mAttachmentPoints)
        {
            const LLViewerJointAttachment* joint = point.second;
            if (!joint)
            {
                continue;
            }
            for (const LLPointer<LLViewerObject>& object : joint->mAttachedObjects)
            {
                if (object.isNull() || object->isDead() || object->isHUDAttachment())
                {
                    continue;
                }
                ++attachments;
                countDrawable(object->mDrawable, all);
                if (drawn_in_full)
                {
                    countDrawable(object->mDrawable, drawn);
                }
            }
        }
    }
    LL_INFOS("Scene") << "scene shadow H2 avatars (research/218 vk-s0b; what the cascades' culls could bring): avatars "
                      << avatars << " (seen by the main view " << visible << ", drawn in full " << full << "; impostors "
                      << impostors << ", jellydolls " << jellydolls << ", muted " << muted << ", not fully loaded " << loading
                      << "), their attachments " << attachments << " (with a bridge " << all.bridges << ", groups "
                      << all.groups << ", batches rigged " << all.rigged << ", not rigged " << all.plain
                      << "; of the avatars drawn in full: groups " << drawn.groups << ", batches rigged " << drawn.rigged
                      << ", not rigged " << drawn.plain << "); animesh " << animesh << " (seen " << animesh_visible
                      << ", groups " << animesh_batches.groups << ", batches rigged " << animesh_batches.rigged
                      << ", not rigged " << animesh_batches.plain << ")" << LL_ENDL;
}

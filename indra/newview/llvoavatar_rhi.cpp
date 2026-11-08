/**
 * @file llvoavatar_rhi.cpp
 * @brief LLVOAvatar's resident palette rows and the compute skinning check
 *        (research/199 stage 4b, LLRHIAvatar; the retained scene's rigged
 *        records, research/219). Moved out of llvoavatar.cpp as it stood, so
 *        the official file keeps only the hooks; declared in llvoavatar.h.
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

#include "llvoavatar.h"

#include "lldrawpool.h"             // LLRenderPass::rhiPaletteRow
#include "llmeshrepository.h"
#include "llrhiavatar.h"
#include "llskinningutil.h"
#include "llviewerjointattachment.h"
#include "llvovolume.h"
#include "plus/hairsway/hairsway.h"

namespace
{
    // The owner of an avatar x skin's resident row, never 0. Two live entries
    // never share one: an entry is one (avatar, skin hash) pair.
    U64 rhi_row_owner(const LLVOAvatar* avatar, U64 skin_hash)
    {
        U64 h = (U64)(uintptr_t)avatar * 0x9E3779B97F4A7C15ull ^ skin_hash;
        h ^= h >> 31;
        h *= 0xBF58476D1CE4E5B9ull;
        h ^= h >> 29;
        return h ? h : 1;
    }

    // Skins that went unbuilt this many frames give their row back (outfit
    // changes: the cache keeps its entries, as upstream), checked this often.
    constexpr U32 RHI_ROW_IDLE_FRAMES = 300;
    constexpr U32 RHI_ROW_SWEEP_FRAMES = 256;

    // ==== scene palette rules: begin ====
    // research/219 (the selftest sim-riggedviewer takes these lines on their
    // own: plain values only). What rhiScenePalette answers once the skin's
    // palette is built: the reasons as LLVOAvatar::RHIScenePalette's numbers
    // (0 ready), or SCENE_PALETTE_WRITE: the row holds another frame's table's
    // joints (or none): write it into this frame's table first, ready when the
    // write takes.
    constexpr U8 SCENE_PALETTE_WRITE = 0xff;
    U8 scene_palette_rule(bool loaded, U32 slot, U32 base, bool sway, U32 vec4s, U32 room, U32 written_base, U32 written_frame,
                          U32 frame)
    {
        if (!loaded)
        {
            return 2;   // not loaded
        }
        if (slot != base)
        {
            return 3;   // row changed
        }
        if (sway || !vec4s || vec4s > room)
        {
            return 4;   // sway, or longer than a row
        }
        if (written_frame != frame || written_base != base)
        {
            return SCENE_PALETTE_WRITE;
        }
        return 0;
    }

    // Whether the sweep of long unbuilt skins gives the skin's row back: not
    // while records of the retained scene name it.
    bool row_swept(U32 slot, U32 idle_frames, U32 record_users)
    {
        return slot != 0xffffffffu && idle_frames > RHI_ROW_IDLE_FRAMES && record_users == 0;
    }

    // The records naming a row after delta (never below 0).
    U32 pinned_users(U32 users, S32 delta)
    {
        if (delta < 0)
        {
            const U32 less = (U32)(-(S64)delta);
            return users > less ? users - less : 0u;
        }
        return users + (U32)delta;
    }
    // ==== scene palette rules: end ====
}

U32 LLVOAvatar::rhiPaletteSlot(const LLMeshSkinInfo* skin)
{
    LLRHIAvatar::Backend* backend = LLRHIAvatar::backend();
    if (!backend || !skin || !LLRHIAvatar::paletteWrites() || isDead())
    {
        return LLRHIAvatar::SLOT_NONE;
    }
    MatrixPaletteCache& entry = mMatrixPaletteCache[skin->mHash];
    if (entry.mRHISlot == LLRHIAvatar::SLOT_NONE)
    {
        entry.mRHISlot = backend->allocateRow(rhi_row_owner(this, skin->mHash));
    }
    return entry.mRHISlot;
}

void LLVOAvatar::rhiReleasePaletteRows()
{
    LLRHIAvatar::Backend* backend = LLRHIAvatar::backend();
    for (auto& it : mMatrixPaletteCache)
    {
        MatrixPaletteCache& entry = it.second;
        if (entry.mRHISlot != LLRHIAvatar::SLOT_NONE)
        {
            if (backend)
            {
                backend->releaseRow(entry.mRHISlot, rhi_row_owner(this, it.first));
            }
            if (entry.mRHIRow.base == entry.mRHISlot)
            {
                // the build's row was the resident one (on); the rolling rows
                // start after the region, so no rolling row has this base
                entry.mRHIRow = LLRHI::PaletteRow();
            }
            entry.mRHIWritten = LLRHI::PaletteRow();    // research/219
            entry.mRHISlot = LLRHIAvatar::SLOT_NONE;
        }
    }
}

void LLVOAvatar::rhiAfterPaletteBuild(const LLMeshSkinInfo* skin, MatrixPaletteCache& entry)
{
    LLRHIAvatar::Backend* backend = LLRHIAvatar::backend();
    entry.mRHIWritten = LLRHI::PaletteRow();    // research/219: the row holds this build's joints only once written below
    if (LLRHIAvatar::paletteWrites() && !isDead())
    {
        // Long unbuilt skins give their row back (one look per RHI_ROW_SWEEP_FRAMES);
        // research/219: not a row the retained scene's records name.
        if (gFrameCount - mRHIRowSweepFrame >= RHI_ROW_SWEEP_FRAMES)
        {
            mRHIRowSweepFrame = gFrameCount;
            for (auto& it : mMatrixPaletteCache)
            {
                MatrixPaletteCache& other = it.second;
                if (row_swept(other.mRHISlot, gFrameCount - other.mFrame, other.mRHIRecordUsers))
                {
                    backend->releaseRow(other.mRHISlot, rhi_row_owner(this, it.first));
                    other.mRHISlot = LLRHIAvatar::SLOT_NONE;
                    other.mRHIWritten = LLRHI::PaletteRow();
                }
            }
        }
        // The joints as built into the entry's resident row, in the table of
        // the frame being recorded. Longer than a row (the agent's hair with
        // its sway entries past 110 joints): none, the rolling row as before.
        const U32 vec4s = (U32)(entry.mGLMp.size() / 4);
        const U64 owner = rhi_row_owner(this, skin->mHash);
        if (vec4s && vec4s <= backend->rowRoom())
        {
            if (entry.mRHISlot == LLRHIAvatar::SLOT_NONE)
            {
                entry.mRHISlot = backend->allocateRow(owner);
            }
            LLRHI::PaletteRow row;
            if (entry.mRHISlot != LLRHIAvatar::SLOT_NONE && backend->writeRow(entry.mRHISlot, owner, entry.mGLMp.data(), vec4s, row))
            {
                entry.mRHIWritten = row;    // research/219: what the scene's rigged records read
                if (LLRHIAvatar::paletteDraws())
                {
                    // on: every pass names this row (LLRenderPass::rhiPaletteRow
                    // finds it placed this frame and asks nothing), as it named the
                    // frame's rolling row before.
                    entry.mRHIRow = row;
                }
            }
        }
    }
    if (LLRHIAvatar::computeSkinVerify())
    {
        rhiSampleSkinning(skin, entry);
    }
}

// research/219: the retained scene's rigged records (llscenerecords.cpp readySkins).
LLVOAvatar::RHIScenePalette LLVOAvatar::rhiScenePalette(const LLMeshSkinInfo* skin, U32 base)
{
    LLRHIAvatar::Backend* backend = LLRHIAvatar::backend();
    if (!backend || !skin || !LLRHIAvatar::paletteWrites() || isDead())
    {
        return RHI_PALETTE_OFF;
    }
    updateSkinInfoMatrixPalette(skin);      // built once a frame: by the first pass or view that needs it
    const auto it = mMatrixPaletteCache.find(skin->mHash);
    if (it == mMatrixPaletteCache.end() || it->second.mMatrixPalette.empty())
    {
        return RHI_PALETTE_NOT_LOADED;
    }
    MatrixPaletteCache& entry = it->second;
    static_assert(RHI_PALETTE_READY == 0 && RHI_PALETTE_NOT_LOADED == 2 && RHI_PALETTE_ROW_CHANGED == 3 && RHI_PALETTE_SWAY == 4,
                  "scene_palette_rule's numbers");
    // The sway: one value of plusHairSway for the pass, the usual way draws it. Written in another
    // backend frame (its table is the other one's), or not at all: written again into this frame's
    // table (a present between two passes of one build, as a tiled snapshot).
    const U32 vec4s = (U32)(entry.mGLMp.size() / 4);
    const U8 rule = scene_palette_rule(true, entry.mRHISlot, base, plus::hairsway::swayIndex(entry.mMatrixPalette) != 0, vec4s,
                                       backend->rowRoom(), entry.mRHIWritten.base, entry.mRHIWritten.frame, backend->frame());
    if (rule != SCENE_PALETTE_WRITE)
    {
        return (RHIScenePalette)rule;
    }
    LLRHI::PaletteRow row;
    if (!backend->writeRow(base, rhi_row_owner(this, skin->mHash), entry.mGLMp.data(), vec4s, row))
    {
        return RHI_PALETTE_NOT_WRITTEN;
    }
    entry.mRHIWritten = row;
    return RHI_PALETTE_READY;
}

S32 LLVOAvatar::rhiComparePaletteRow(const LLMeshSkinInfo* skin, U32 base, U32& first)
{
    first = 0;
    LLRHIAvatar::Backend* backend = LLRHIAvatar::backend();
    if (!backend || rhiScenePalette(skin, base) != RHI_PALETTE_READY)
    {
        return -1;
    }
    const auto it = mMatrixPaletteCache.find(skin->mHash);
    if (it == mMatrixPaletteCache.end())
    {
        return -1;
    }
    const std::vector<F32>& joints = it->second.mGLMp;
    const U32 vec4s = (U32)(joints.size() / 4);
    static std::vector<F32> row;    // main thread; kept for its capacity
    row.resize(joints.size());
    if (!backend->readRow(base, vec4s, row.data()))
    {
        return -1;
    }
    for (U32 v = 0; v < vec4s; ++v)
    {
        if (memcmp(&row[(size_t)v * 4], &joints[(size_t)v * 4], 4 * sizeof(F32)) != 0)
        {
            first = v;
            return 0;
        }
    }
    return 1;
}

void LLVOAvatar::rhiPinPaletteSlot(U64 skin_hash, S32 delta)
{
    const auto it = mMatrixPaletteCache.find(skin_hash);
    if (it == mMatrixPaletteCache.end())
    {
        return;
    }
    it->second.mRHIRecordUsers = pinned_users(it->second.mRHIRecordUsers, delta);
}

void LLVOAvatar::rhiSampleSkinning(const LLMeshSkinInfo* skin, MatrixPaletteCache& entry)
{
    // A few jobs a frame, taken in turn from the builds (all avatars, all skins).
    static U32 frame = 0;
    static U32 jobs = 0;
    static U32 turn = 0;
    if (frame != gFrameCount)
    {
        frame = gFrameCount;
        jobs = 0;
    }
    if (jobs >= LLRHIAvatar::SKIN_JOBS_PER_FRAME || isDead())
    {
        return;
    }
    // The palette as the shaders read it; a sway-extended hair palette (more
    // entries than joints) moves vertices past what the kernel does: skipped.
    const U32 joints = (U32)LLSkinningUtil::getMeshJointCount(skin);
    if (!joints || entry.mMatrixPalette.size() != joints || entry.mGLMp.size() != (size_t)joints * 12)
    {
        return;
    }
    // A rigged face of this skin among the attachments (the next one in turn).
    ++turn;
    const LLVolumeFace* face = nullptr;
    for (const auto& point : mAttachmentPoints)
    {
        const LLViewerJointAttachment* attachment = point.second;
        if (!attachment)
        {
            continue;
        }
        for (const LLPointer<LLViewerObject>& root : attachment->mAttachedObjects)
        {
            if (root.isNull())
            {
                continue;
            }
            auto consider = [&](const LLViewerObject* object)
            {
                if (face || !object || object->getPCode() != LL_PCODE_VOLUME || object->isDead())
                {
                    return;
                }
                const LLVOVolume* volume = static_cast<const LLVOVolume*>(object);
                const LLMeshSkinInfo* own = volume->isRiggedMesh() ? volume->getSkinInfo() : nullptr;
                const LLVolume* shape = own && own->mHash == skin->mHash ? volume->getVolume() : nullptr;
                if (!shape || shape->getNumVolumeFaces() <= 0)
                {
                    return;
                }
                const LLVolumeFace& candidate = shape->getVolumeFace((S32)(turn % (U32)shape->getNumVolumeFaces()));
                if (candidate.mNumVertices > 0 && candidate.mPositions && candidate.mWeights)
                {
                    face = &candidate;
                }
            };
            consider(root.get());
            for (const LLPointer<LLViewerObject>& child : root->getChildren())
            {
                consider(child.get());
            }
        }
    }
    if (!face)
    {
        return;
    }
    // The row the draws read this frame (placed now if no pass has yet).
    if (!LLRenderPass::rhiPaletteRow(entry.mGLMp, entry.mRHIRow))
    {
        return;
    }
    LLRHIAvatar::SkinVerifyJob job;
    job.positions = face->mPositions[0].getF32ptr();
    job.weights = face->mWeights[0].getF32ptr();
    job.vertices = (U32)face->mNumVertices;
    job.palette = entry.mGLMp.data();
    job.joints = joints;
    job.row_base = entry.mRHIRow.base;
    job.row_frame = entry.mRHIRow.frame;
    job.label = rhi_row_owner(this, skin->mHash);
    if (LLRHIAvatar::backend()->skinVerify(job))
    {
        ++jobs;
    }
}

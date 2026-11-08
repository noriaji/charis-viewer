/**
 * @file llvkscenetable.cpp
 * @brief The retained scene's tables as the CPU keeps them (research/197 stage 2).
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

#include "linden_common.h"

#include "llvkscenetable.h"
#include "llrhihash.h"

#include <algorithm>
#include <bitset>
#include <cmath>
#include <cstring>

namespace LLVKSceneShader
{
    namespace
    {
        // scene_cull.comp's shared functions built as C++ (llvulkan.cmake
        // writes them out as llvkscene_cull_funcs.inl): the buffers the shader
        // reads are members of the names it reads them by.
        struct ShaderEmu
        {
            const Record* records;
            const Transform* transforms;
            const uint* page_bucket;
            ViewData view;
            const uint* visible;
            const BucketInfo* bucket_info;
            const uint* page_list;
            uint* out_words;

            // GLSL's built-ins the functions use.
            static float abs(float x) { return std::fabs(x); }
            static int bitCount(uint x) { return (int)std::bitset<32>(x).count(); }

#include "llvkscene_cull_funcs.inl"
        };
    }
}

namespace
{
    using LLVKSceneShader::ShaderEmu;

    constexpr U32 HANDLE_INDEX_BITS = 24;
    constexpr U32 HANDLE_INDEX_MASK = (1u << HANDLE_INDEX_BITS) - 1;

    // The lowest lane a page has free (used is not all ones).
    U32 lowest_free_lane(U64 used)
    {
        for (U32 lane = 0; lane < LLVKSceneTable::PAGE; ++lane)
        {
            if (!(used & (1ull << lane)))
            {
                return lane;
            }
        }
        return LLVKSceneTable::NONE;
    }

    // FNV-1a over a material row: equal rows are found by their hash first.
    U64 material_hash(const LLRHI::SceneMaterialDesc& material)
    {
        return LLRHI::fnv1a(LLRHI::FNV1A_BASIS, &material, sizeof(material));
    }

    // research/205 H5: the same over a texture list's row.
    U64 list_hash(const LLVKSceneTable::TextureList& list)
    {
        return LLRHI::fnv1a(LLRHI::FNV1A_BASIS, &list, sizeof(list));
    }

    // A desc's list as its row holds it (count 0: none, all 0).
    LLVKSceneTable::TextureList list_row(const LLRHI::SceneTextureList& list)
    {
        LLVKSceneTable::TextureList row = {};
        const U32 count = list.count < LLRHI::SceneTextureList::MAX ? list.count : LLRHI::SceneTextureList::MAX;
        for (U32 i = 0; i < count; ++i)
        {
            row.textures[i] = list.textures[i].id;
        }
        return row;
    }

    U64 bucket_key(const LLRHI::SceneRecordDesc& desc)
    {
        return ((U64)desc.vertex_format << 32) | ((U64)(desc.range.chunk & 0xffffff) << 8) | ((U64)(desc.pass & 0x7f) << 1) |
               (desc.range.index32 ? 1u : 0u);
    }

    // A row of a slot table: a freed one, or a new one at the end (append).
    // NONE: the table is full.
    template <class SLOTS>
    U32 take_row(SLOTS& slots, U32 limit, bool& append)
    {
        append = false;
        if (!slots.free.empty())
        {
            const U32 row = slots.free.back();
            slots.free.pop_back();
            slots.live[row] = 1;
            slots.users[row] = 0;
            return row;
        }
        if (slots.live.size() >= limit)
        {
            return LLVKSceneTable::NONE;
        }
        append = true;
        slots.live.push_back(1);
        slots.users.push_back(0);
        return (U32)slots.live.size() - 1;
    }
}

LLVKSceneTable::LLVKSceneTable(const Limits& limits)
    : mLimits(limits)
{
    // Row 0 of the transforms is the identity, of the texture transforms "none".
    Transform identity = {};
    identity.m[0] = identity.m[5] = identity.m[10] = identity.m[15] = 1.f;
    mTransforms.push_back(identity);
    mTransformSlots.live.push_back(1);
    mTransformSlots.users.push_back(0);
    markWrite(TABLE_TRANSFORMS, 0);
    mUV.push_back(Transform());
    mUVSlots.live.push_back(1);
    mUVSlots.users.push_back(0);
    markWrite(TABLE_UV, 0);
    // research/205 H5: row 0 of the texture lists is "none" (never given out).
    mLists.push_back(TextureList());
    mListSlots.live.push_back(1);
    mListSlots.users.push_back(0);
    markWrite(TABLE_TEXLISTS, 0);
}

void LLVKSceneTable::beginFrame(U32 frame)
{
    mFrame = frame;
    mFrameStarted = true;

    size_t kept = 0;
    for (size_t i = 0; i < mSlotReleases.size(); ++i)
    {
        const Release release = mSlotReleases[i];
        if (frame - release.frame < RELEASE_DELAY)
        {
            mSlotReleases[kept++] = release;
            continue;
        }
        const U32 page = release.row / PAGE;
        const U32 lane = release.row % PAGE;
        mPageUsed[page] &= ~(1ull << lane);
        Bucket& bucket = mBuckets[mPageBucket[page]];
        const auto at = std::find(bucket.pages.begin(), bucket.pages.end(), page);
        const U32 rank = (U32)(at - bucket.pages.begin());
        if (mPageUsed[page] == 0)
        {
            // An empty page leaves its bucket (it had a free lane: rank >= open).
            bucket.pages.erase(at);
            bucket.open = std::min(bucket.open, rank);
            mPageBucket[page] = NONE;
            markWrite(TABLE_PAGES, page);
            mFreePages.push_back(page);
            ++mLayoutSerial;
        }
        else if (rank < bucket.open)
        {
            bucket.open = rank;
        }
    }
    mSlotReleases.resize(kept);

    releaseRows(mTransformSlots, frame);
    releaseRows(mUVSlots, frame);
    releaseRows(mMaterialSlots, frame);
    releaseRows(mListSlots, frame);
}

void LLVKSceneTable::releaseRows(Slots& slots, U32 frame)
{
    size_t kept = 0;
    for (size_t i = 0; i < slots.pending.size(); ++i)
    {
        if (frame - slots.pending[i].frame >= RELEASE_DELAY)
        {
            slots.free.push_back(slots.pending[i].row);
        }
        else
        {
            slots.pending[kept++] = slots.pending[i];
        }
    }
    slots.pending.resize(kept);
}

// --- groups ------------------------------------------------------------------

U32 LLVKSceneTable::addGroup()
{
    U32 slot;
    if (!mFreeGroups.empty())
    {
        slot = mFreeGroups.back();
        mFreeGroups.pop_back();
    }
    else if (mGroups.size() < mLimits.groups)
    {
        slot = (U32)mGroups.size();
        mGroups.emplace_back();
    }
    else
    {
        ++mCounters.refused;
        return 0;
    }
    mGroups[slot].used = true;
    mGroups[slot].handles.clear();
    return slot + 1;
}

bool LLVKSceneTable::groupLive(U32 group) const
{
    return group && group - 1 < mGroups.size() && mGroups[group - 1].used;
}

void LLVKSceneTable::removeGroup(U32 group)
{
    if (!groupLive(group))
    {
        ++mCounters.stale;
        return;
    }
    Group& g = mGroups[group - 1];
    while (!g.handles.empty())
    {
        const U32 index = g.handles.back();
        const Handle& h = mHandles[index];
        removeRecord(((U32)h.gen << HANDLE_INDEX_BITS) | (index + 1));
    }
    g.used = false;
    mFreeGroups.push_back(group - 1);
}

// --- records -----------------------------------------------------------------

U32 LLVKSceneTable::handleIndex(U32 id) const
{
    const U32 low = id & HANDLE_INDEX_MASK;
    if (!low)
    {
        return NONE;
    }
    const U32 index = low - 1;
    if (index >= mHandles.size() || !mHandles[index].live || mHandles[index].gen != (U8)(id >> HANDLE_INDEX_BITS))
    {
        return NONE;
    }
    return index;
}

bool LLVKSceneTable::recordLive(U32 id) const
{
    return handleIndex(id) != NONE;
}

U32 LLVKSceneTable::slotOf(U32 id) const
{
    const U32 index = handleIndex(id);
    return index == NONE ? NONE : mHandles[index].slot;
}

U32 LLVKSceneTable::recordAt(U32 slot) const
{
    if (slot >= mSlotHandle.size() || mSlotHandle[slot] == NONE)
    {
        return 0;
    }
    const U32 index = mSlotHandle[slot];
    return ((U32)mHandles[index].gen << HANDLE_INDEX_BITS) | (index + 1);
}

bool LLVKSceneTable::validDesc(const LLRHI::SceneRecordDesc& desc) const
{
    if (desc.pass >= LLRHI::SCENE_MAX_PASSES || !desc.range.index_count || !groupLive(desc.group))
    {
        return false;
    }
    if (desc.transform >= mTransformSlots.live.size() || !mTransformSlots.live[desc.transform] ||
        desc.uv_transform >= mUVSlots.live.size() || !mUVSlots.live[desc.uv_transform])
    {
        return false;
    }
    if (!desc.loose)
    {
        for (U32 i = 0; i < 3; ++i)
        {
            if (!(desc.lo[i] <= desc.hi[i]) || !std::isfinite(desc.lo[i]) || !std::isfinite(desc.hi[i]))
            {
                return false;   // also NaN
            }
        }
    }
    // research/219: a rigged record's palette, bounds, list and transform.
    return LLVKSceneShader::rigged_desc_ok(desc.palette, desc.loose, desc.list.count, desc.transform);
}

U32 LLVKSceneTable::findBucket(const LLRHI::SceneRecordDesc& desc, bool create)
{
    const U64 key = bucket_key(desc);
    const auto it = mBucketIndex.find(key);
    if (it != mBucketIndex.end())
    {
        return it->second;
    }
    if (!create || mBuckets.size() >= mLimits.buckets)
    {
        return NONE;
    }
    const U32 bucket = (U32)mBuckets.size();
    mBuckets.emplace_back();
    BucketKey& k = mBuckets.back().key;
    k.pass = desc.pass;
    k.format = desc.vertex_format;
    k.chunk = desc.range.chunk;
    k.index32 = desc.range.index32;
    mBucketIndex.emplace(key, bucket);
    mPassBuckets[desc.pass].push_back(bucket);
    ++mLayoutSerial;
    return bucket;
}

const std::vector<U32>& LLVKSceneTable::passBuckets(U32 pass) const
{
    static const std::vector<U32> none;
    return pass < LLRHI::SCENE_MAX_PASSES ? mPassBuckets[pass] : none;
}

U32 LLVKSceneTable::takeSlot(U32 bucket_index)
{
    Bucket& bucket = mBuckets[bucket_index];
    for (U32 rank = bucket.open; rank < bucket.pages.size(); ++rank)
    {
        const U32 page = bucket.pages[rank];
        const U32 lane = lowest_free_lane(mPageUsed[page]);
        if (lane != NONE)
        {
            mPageUsed[page] |= 1ull << lane;
            bucket.open = rank;
            return page * PAGE + lane;
        }
    }
    bucket.open = (U32)bucket.pages.size();

    U32 page;
    if (!mFreePages.empty())
    {
        page = mFreePages.back();
        mFreePages.pop_back();
    }
    else if (mPageBucket.size() < mLimits.pages)
    {
        // A new page: every lane written dead first, so no lane of it reads
        // as a record of its bucket before one is written there.
        page = (U32)mPageBucket.size();
        mPageBucket.push_back(NONE);
        mPageUsed.push_back(0);
        Record dead = {};
        dead.bucket = NONE;
        mRecords.resize(mRecords.size() + PAGE, dead);
        mSlotHandle.resize(mSlotHandle.size() + PAGE, NONE);
        for (U32 lane = 0; lane < PAGE; ++lane)
        {
            markWrite(TABLE_RECORDS, page * PAGE + lane);
        }
    }
    else
    {
        return NONE;
    }
    mPageBucket[page] = bucket_index;
    markWrite(TABLE_PAGES, page);
    bucket.pages.push_back(page);
    ++mLayoutSerial;
    mPageUsed[page] = 1;
    return page * PAGE;
}

// The slot's record is dead from now on; the slot is given out again
// RELEASE_DELAY frames later (beginFrame).
void LLVKSceneTable::freeSlot(U32 slot)
{
    mRecords[slot].bucket = NONE;
    markWrite(TABLE_RECORDS, slot);
    mSlotHandle[slot] = NONE;
    Release release;
    release.row = slot;
    release.frame = mFrame;
    mSlotReleases.push_back(release);
}

void LLVKSceneTable::writeRecord(U32 slot, U32 handle, U32 bucket, const LLRHI::SceneRecordDesc& desc)
{
    Record& r = mRecords[slot];
    for (U32 i = 0; i < 3; ++i)
    {
        r.lo[i] = desc.loose ? 0.f : desc.lo[i];
        r.hi[i] = desc.loose ? 0.f : desc.hi[i];
    }
    r.group_slot = desc.group - 1;
    r.transform = desc.transform;
    r.index_count = desc.range.index_count;
    r.first_index = desc.range.first_index;
    r.vertex_offset = desc.range.vertex_offset;
    r.bucket = bucket;
    r.uv_transform = desc.uv_transform;
    // r.material, and the list's row in r.flags: set by the caller (interned).
    // research/219: a rigged record's palette base in the same bits (validDesc:
    // such a record has no list, so the caller's row is 0).
    r.flags = LLVKSceneShader::record_flags(desc.loose, desc.palette);
    r.record_id = ((U32)mHandles[handle].gen << HANDLE_INDEX_BITS) | (handle + 1);
    mSlotHandle[slot] = handle;
    markWrite(TABLE_RECORDS, slot);
}

U32 LLVKSceneTable::internMaterial(const LLRHI::SceneMaterialDesc& material)
{
    const U64 hash = material_hash(material);
    const auto range = mMaterialIndex.equal_range(hash);
    for (auto it = range.first; it != range.second; ++it)
    {
        if (!memcmp(&mMaterials[it->second], &material, sizeof(material)))
        {
            ++mMaterialSlots.users[it->second];
            return it->second;
        }
    }
    bool append = false;
    const U32 row = take_row(mMaterialSlots, mLimits.materials, append);
    if (row == NONE)
    {
        return NONE;
    }
    if (append)
    {
        mMaterials.push_back(material);
    }
    else
    {
        mMaterials[row] = material;
    }
    mMaterialSlots.users[row] = 1;
    mMaterialIndex.emplace(hash, row);
    markWrite(TABLE_MATERIALS, row);
    return row;
}

void LLVKSceneTable::releaseMaterial(U32 row)
{
    if (row >= mMaterialSlots.live.size() || !mMaterialSlots.live[row] || !mMaterialSlots.users[row])
    {
        ++mCounters.stale;
        return;
    }
    if (--mMaterialSlots.users[row])
    {
        return;
    }
    const U64 hash = material_hash(mMaterials[row]);
    const auto range = mMaterialIndex.equal_range(hash);
    for (auto it = range.first; it != range.second; ++it)
    {
        if (it->second == row)
        {
            mMaterialIndex.erase(it);
            break;
        }
    }
    mMaterialSlots.live[row] = 0;
    Release release;
    release.row = row;
    release.frame = mFrame;
    mMaterialSlots.pending.push_back(release);
}

// research/205 H5: as internMaterial; row 0 for a desc without a list.
U32 LLVKSceneTable::internList(const LLRHI::SceneTextureList& list)
{
    if (!list.count)
    {
        return 0;
    }
    if (list.count > LLRHI::SceneTextureList::MAX)
    {
        return NONE;
    }
    const TextureList words = list_row(list);
    const U64 hash = list_hash(words);
    const auto range = mListIndex.equal_range(hash);
    for (auto it = range.first; it != range.second; ++it)
    {
        if (!memcmp(&mLists[it->second], &words, sizeof(words)))
        {
            ++mListSlots.users[it->second];
            return it->second;
        }
    }
    bool append = false;
    const U32 row = take_row(mListSlots, mLimits.texture_lists, append);
    if (row == NONE)
    {
        return NONE;
    }
    if (append)
    {
        mLists.push_back(words);
    }
    else
    {
        mLists[row] = words;
    }
    mListSlots.users[row] = 1;
    mListIndex.emplace(hash, row);
    markWrite(TABLE_TEXLISTS, row);
    return row;
}

void LLVKSceneTable::releaseList(U32 row)
{
    if (!row)
    {
        return;     // none
    }
    if (row >= mListSlots.live.size() || !mListSlots.live[row] || !mListSlots.users[row])
    {
        ++mCounters.stale;
        return;
    }
    if (--mListSlots.users[row])
    {
        return;
    }
    const U64 hash = list_hash(mLists[row]);
    const auto range = mListIndex.equal_range(hash);
    for (auto it = range.first; it != range.second; ++it)
    {
        if (it->second == row)
        {
            mListIndex.erase(it);
            break;
        }
    }
    mListSlots.live[row] = 0;
    Release release;
    release.row = row;
    release.frame = mFrame;
    mListSlots.pending.push_back(release);
}

U32 LLVKSceneTable::addRecord(const LLRHI::SceneRecordDesc& desc)
{
    if (!validDesc(desc) || (mHandles.size() >= HANDLE_INDEX_MASK && mFreeHandles.empty()))
    {
        ++mCounters.refused;
        return 0;
    }
    const U32 bucket = findBucket(desc, true);
    if (bucket == NONE)
    {
        ++mCounters.refused;
        return 0;
    }
    const U32 material = internMaterial(desc.material);
    if (material == NONE)
    {
        ++mCounters.refused;
        return 0;
    }
    const U32 list = internList(desc.list);
    if (list == NONE)
    {
        releaseMaterial(material);
        ++mCounters.refused;
        return 0;
    }
    const U32 slot = takeSlot(bucket);
    if (slot == NONE)
    {
        releaseMaterial(material);
        releaseList(list);
        ++mCounters.refused;
        return 0;
    }
    U32 index;
    if (!mFreeHandles.empty())
    {
        index = mFreeHandles.back();
        mFreeHandles.pop_back();
    }
    else
    {
        index = (U32)mHandles.size();
        mHandles.emplace_back();
    }
    Handle& h = mHandles[index];
    h.gen = (U8)(h.gen + 1);
    h.live = true;
    h.slot = slot;
    h.group = desc.group - 1;
    Group& group = mGroups[h.group];
    h.in_group = (U32)group.handles.size();
    group.handles.push_back(index);
    ++mTransformSlots.users[desc.transform];
    ++mUVSlots.users[desc.uv_transform];
    writeRecord(slot, index, bucket, desc);
    mRecords[slot].material = material;
    mRecords[slot].flags |= list << LLVKSceneShader::SCENE_TEXLIST_SHIFT;
    ++mLiveRecords;
    ++mCounters.added;
    return ((U32)h.gen << HANDLE_INDEX_BITS) | (index + 1);
}

bool LLVKSceneTable::updateRecord(U32 id, const LLRHI::SceneRecordDesc& desc)
{
    const U32 index = handleIndex(id);
    if (index == NONE)
    {
        ++mCounters.stale;
        return false;
    }
    if (!validDesc(desc))
    {
        ++mCounters.refused;
        return false;
    }
    const U32 bucket = findBucket(desc, true);
    if (bucket == NONE)
    {
        ++mCounters.refused;
        return false;
    }
    Handle& h = mHandles[index];
    const U32 old_slot = h.slot;
    const Record old = mRecords[old_slot];
    U32 slot = old_slot;
    if (bucket != old.bucket)
    {
        slot = takeSlot(bucket);
        if (slot == NONE)
        {
            ++mCounters.refused;
            return false;
        }
    }
    const U32 material = internMaterial(desc.material);
    const U32 list = material != NONE ? internList(desc.list) : NONE;
    if (material == NONE || list == NONE)
    {
        if (material != NONE)
        {
            releaseMaterial(material);
        }
        if (slot != old_slot)
        {
            freeSlot(slot);     // never written: dead until it is given out again
        }
        ++mCounters.refused;
        return false;
    }
    releaseMaterial(old.material);
    releaseList(LLVKSceneShader::record_list(old.flags));
    --mTransformSlots.users[old.transform];
    --mUVSlots.users[old.uv_transform];
    ++mTransformSlots.users[desc.transform];
    ++mUVSlots.users[desc.uv_transform];
    if (desc.group - 1 != h.group)
    {
        // Leaves its group's list (the last one takes its place) for the new group's.
        std::vector<U32>& from = mGroups[h.group].handles;
        const U32 moved = from.back();
        from[h.in_group] = moved;
        mHandles[moved].in_group = h.in_group;
        from.pop_back();
        h.group = desc.group - 1;
        std::vector<U32>& to = mGroups[h.group].handles;
        h.in_group = (U32)to.size();
        to.push_back(index);
    }
    if (slot != old_slot)
    {
        freeSlot(old_slot);
        h.slot = slot;
        ++mCounters.moved;
    }
    writeRecord(slot, index, bucket, desc);
    mRecords[slot].material = material;
    mRecords[slot].flags |= list << LLVKSceneShader::SCENE_TEXLIST_SHIFT;
    ++mCounters.updated;
    return true;
}

bool LLVKSceneTable::updateRange(U32 id, const LLRHI::SceneRange& range)
{
    const U32 index = handleIndex(id);
    if (index == NONE)
    {
        ++mCounters.stale;
        return false;
    }
    const Handle& h = mHandles[index];
    const Record& r = mRecords[h.slot];
    const BucketKey& key = mBuckets[r.bucket].key;
    LLRHI::SceneRecordDesc desc;
    desc.group = h.group + 1;
    desc.pass = key.pass;
    desc.vertex_format = key.format;
    desc.range = range;
    desc.transform = r.transform;
    desc.uv_transform = r.uv_transform;
    desc.material = mMaterials[r.material];
    // research/205 H5: the list as its row holds it (the same row again).
    const U32 list = LLVKSceneShader::record_list(r.flags);
    if (list && list < mLists.size())
    {
        desc.list.count = LLRHI::SceneTextureList::MAX;
        for (U32 i = 0; i < LLRHI::SceneTextureList::MAX; ++i)
        {
            desc.list.textures[i].id = mLists[list].textures[i];
        }
    }
    desc.loose = (r.flags & LLVKSceneShader::SCENE_LOOSE) != 0;
    desc.palette = LLVKSceneShader::record_palette(r.flags);    // research/219
    for (U32 i = 0; i < 3; ++i)
    {
        desc.lo[i] = r.lo[i];
        desc.hi[i] = r.hi[i];
    }
    return updateRecord(id, desc);
}

void LLVKSceneTable::removeRecord(U32 id)
{
    const U32 index = handleIndex(id);
    if (index == NONE)
    {
        ++mCounters.stale;
        return;
    }
    Handle& h = mHandles[index];
    const Record& r = mRecords[h.slot];
    releaseMaterial(r.material);
    releaseList(LLVKSceneShader::record_list(r.flags));
    --mTransformSlots.users[r.transform];
    --mUVSlots.users[r.uv_transform];
    freeSlot(h.slot);
    std::vector<U32>& list = mGroups[h.group].handles;
    const U32 moved = list.back();
    list[h.in_group] = moved;
    mHandles[moved].in_group = h.in_group;
    list.pop_back();
    h.live = false;
    h.slot = NONE;
    mFreeHandles.push_back(index);
    --mLiveRecords;
    ++mCounters.removed;
}

// --- transforms --------------------------------------------------------------

U32 LLVKSceneTable::addRow(Slots& slots, std::vector<Transform>& rows, U32 limit, const F32* words, Table table)
{
    bool append = false;
    const U32 row = take_row(slots, limit, append);
    if (row == NONE)
    {
        ++mCounters.refused;
        return 0;
    }
    if (append)
    {
        rows.emplace_back();
    }
    memcpy(rows[row].m, words, sizeof(rows[row].m));
    markWrite(table, row);
    return row;
}

void LLVKSceneTable::setRow(Slots& slots, std::vector<Transform>& rows, U32 row, const F32* words, Table table)
{
    if (!row || row >= slots.live.size() || !slots.live[row])
    {
        ++mCounters.stale;
        return;
    }
    memcpy(rows[row].m, words, sizeof(rows[row].m));
    markWrite(table, row);
}

bool LLVKSceneTable::removeRow(Slots& slots, U32 row)
{
    if (!row || row >= slots.live.size() || !slots.live[row])
    {
        ++mCounters.stale;
        return false;
    }
    if (slots.users[row])
    {
        ++mCounters.in_use_refused;
        return false;
    }
    slots.live[row] = 0;
    Release release;
    release.row = row;
    release.frame = mFrame;
    slots.pending.push_back(release);
    return true;
}

U32 LLVKSceneTable::addTransform(const F32 matrix[16])
{
    return addRow(mTransformSlots, mTransforms, mLimits.transforms, matrix, TABLE_TRANSFORMS);
}

void LLVKSceneTable::setTransform(U32 slot, const F32 matrix[16])
{
    setRow(mTransformSlots, mTransforms, slot, matrix, TABLE_TRANSFORMS);
    ++mCounters.transform_writes;
}

bool LLVKSceneTable::removeTransform(U32 slot)
{
    return removeRow(mTransformSlots, slot);
}

U32 LLVKSceneTable::addUVTransform(const F32 words[16])
{
    return addRow(mUVSlots, mUV, mLimits.uv_transforms, words, TABLE_UV);
}

void LLVKSceneTable::setUVTransform(U32 slot, const F32 words[16])
{
    setRow(mUVSlots, mUV, slot, words, TABLE_UV);
    ++mCounters.uv_writes;
}

bool LLVKSceneTable::removeUVTransform(U32 slot)
{
    return removeRow(mUVSlots, slot);
}

// --- writes ------------------------------------------------------------------

void LLVKSceneTable::takeWrites(Table table, std::vector<Run>& runs)
{
    runs.clear();
    std::vector<U32>& rows = mWrites[table];
    if (rows.empty())
    {
        return;
    }
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    for (U32 row : rows)
    {
        if (!runs.empty() && runs.back().first + runs.back().count == row)
        {
            ++runs.back().count;
        }
        else
        {
            Run run;
            run.first = row;
            run.count = 1;
            runs.push_back(run);
        }
    }
    rows.clear();
}

void LLVKSceneTable::keepWrites(Table table, const std::vector<Run>& runs)
{
    std::vector<U32>& rows = mWrites[table];
    for (const Run& run : runs)
    {
        for (U32 i = 0; i < run.count; ++i)
        {
            rows.push_back(run.first + i);
        }
    }
}

bool LLVKSceneTable::anyWrites() const
{
    for (U32 t = 0; t < TABLE_COUNT; ++t)
    {
        if (!mWrites[t].empty())
        {
            return true;
        }
    }
    return false;
}

U32 LLVKSceneTable::rowBytes(Table table) const
{
    switch (table)
    {
        case TABLE_RECORDS:
            return sizeof(Record);
        case TABLE_TRANSFORMS:
        case TABLE_UV:
            return sizeof(Transform);
        case TABLE_MATERIALS:
            return sizeof(LLRHI::SceneMaterialDesc);
        case TABLE_TEXLISTS:
            return sizeof(TextureList);
        default:
            return sizeof(U32);
    }
}

U32 LLVKSceneTable::rowSpan(Table table) const
{
    switch (table)
    {
        case TABLE_RECORDS:
            return (U32)mRecords.size();
        case TABLE_TRANSFORMS:
            return (U32)mTransforms.size();
        case TABLE_UV:
            return (U32)mUV.size();
        case TABLE_MATERIALS:
            return (U32)mMaterials.size();
        case TABLE_TEXLISTS:
            return (U32)mLists.size();
        default:
            return (U32)mPageBucket.size();
    }
}

U32 LLVKSceneTable::rowCapacity(Table table) const
{
    switch (table)
    {
        case TABLE_RECORDS:
            return mLimits.pages * PAGE;
        case TABLE_TRANSFORMS:
            return mLimits.transforms;
        case TABLE_UV:
            return mLimits.uv_transforms;
        case TABLE_MATERIALS:
            return mLimits.materials;
        case TABLE_TEXLISTS:
            return mLimits.texture_lists;
        default:
            return mLimits.pages;
    }
}

const void* LLVKSceneTable::rows(Table table) const
{
    switch (table)
    {
        case TABLE_RECORDS:
            return mRecords.data();
        case TABLE_TRANSFORMS:
            return mTransforms.data();
        case TABLE_UV:
            return mUV.data();
        case TABLE_MATERIALS:
            return mMaterials.data();
        case TABLE_TEXLISTS:
            return mLists.data();
        default:
            return mPageBucket.data();
    }
}

// --- the choice ----------------------------------------------------------------

const LLVKSceneTable::Plan& LLVKSceneTable::plan()
{
    if (mPlan.serial == mLayoutSerial)
    {
        return mPlan;
    }
    mPlan.serial = mLayoutSerial;
    mPlan.pages = (U32)mPageBucket.size();
    mPlan.buckets = (U32)mBuckets.size();
    mPlan.bucket_info.resize(mBuckets.size());
    mPlan.page_list.clear();
    U32 commands = 0;
    for (U32 b = 0; b < mBuckets.size(); ++b)
    {
        BucketInfo& info = mPlan.bucket_info[b];
        info.first_command = commands;
        info.first_listed = (U32)mPlan.page_list.size();
        info.page_count = (U32)mBuckets[b].pages.size();
        info.pad = 0;
        mPlan.page_list.insert(mPlan.page_list.end(), mBuckets[b].pages.begin(), mBuckets[b].pages.end());
        commands += info.page_count * PAGE;
    }
    mPlan.commands = commands;
    return mPlan;
}

U32 LLVKSceneTable::makeView(const LLRHI::SceneViewDesc& desc, U32 group_words, const Plan& plan, ViewData& out)
{
    out = ViewData();
    const U32 planes = std::min(desc.plane_count, LLRHI::SCENE_MAX_PLANES);
    for (U32 k = 0; k < planes; ++k)
    {
        for (U32 i = 0; i < 4; ++i)
        {
            out.planes[k * 4 + i] = desc.planes[k][i];
        }
    }
    out.plane_count = planes;
    out.origin[0] = desc.origin[0];
    out.origin[1] = desc.origin[1];
    out.origin[2] = desc.origin[2];
    out.pages = plan.pages;
    out.buckets = plan.buckets;
    out.group_words = group_words;
    out.count_base = 0;
    out.mask_base = plan.buckets;
    out.pagecount_base = out.mask_base + plan.pages * 2;
    out.offset_base = out.pagecount_base + plan.pages;
    out.command_base = out.offset_base + plan.pages;
    return out.command_base + plan.commands * 5;
}

void LLVKSceneTable::emulate(const Record* records, const Transform* transforms, const U32* page_bucket, const ViewData& view,
                             const U32* visible, const BucketInfo* bucket_info, const U32* page_list, U32* out_words)
{
    ShaderEmu shader{ records, transforms, page_bucket, view, visible, bucket_info, page_list, out_words };
    // main()'s phases in turn, each over all of its workgroups.
    for (U32 page = 0; page < view.pages; ++page)
    {
        U32 mask[2] = { 0, 0 };
        for (U32 lane = 0; lane < PAGE; ++lane)
        {
            if (shader.scene_lane_visible(page, lane))
            {
                mask[lane >> 5] |= 1u << (lane & 31);
            }
        }
        shader.scene_page_done(page, mask[0], mask[1]);
    }
    for (U32 b = 0; b < view.buckets; ++b)
    {
        shader.scene_scan_bucket(b);
    }
    for (U32 page = 0; page < view.pages; ++page)
    {
        for (U32 lane = 0; lane < PAGE; ++lane)
        {
            shader.scene_emit(page, lane);
        }
    }
}

void LLVKSceneTable::emulate(const ViewData& view, const U32* visible, const Plan& plan, std::vector<U32>& out_words) const
{
    out_words.assign((size_t)view.command_base + (size_t)plan.commands * 5, 0);
    emulate(mRecords.data(), mTransforms.data(), mPageBucket.data(), view, visible, plan.bucket_info.data(), plan.page_list.data(),
            out_words.data());
}

F32 LLVKSceneTable::boxExcess(const ViewData& view, U32 slot) const
{
    ShaderEmu shader{ mRecords.data(), mTransforms.data(), mPageBucket.data(), view, nullptr, nullptr, nullptr, nullptr };
    return shader.scene_box_excess(slot);
}

void LLVKSceneTable::collect(const LLRHI::SceneViewDesc& desc, const U32* visible, U32 words, U32 pass, std::vector<U32>& drawn,
                             std::vector<U32>& bounded_out)
{
    drawn.clear();
    bounded_out.clear();
    const Plan& p = plan();
    ViewData view;
    makeView(desc, visible ? words : 0, p, view);
    emulate(view, visible, p, mCollectWords);
    ShaderEmu shader{ mRecords.data(), mTransforms.data(), mPageBucket.data(), view, visible, nullptr, nullptr, nullptr };
    for (U32 b : passBuckets(pass))
    {
        const U32 count = mCollectWords[view.count_base + b];
        const U32 first = p.bucket_info[b].first_command;
        for (U32 k = 0; k < count; ++k)
        {
            drawn.push_back(recordAt(mCollectWords[view.command_base + (first + k) * 5 + 4]));
        }
        for (U32 page : mBuckets[b].pages)
        {
            for (U32 lane = 0; lane < PAGE; ++lane)
            {
                const U32 slot = page * PAGE + lane;
                const Record& r = mRecords[slot];
                if (r.bucket == b && !(r.flags & LLVKSceneShader::SCENE_LOOSE) && shader.scene_group_visible(r.group_slot) &&
                    shader.scene_box_excess(slot) > 0.f)
                {
                    bounded_out.push_back(recordAt(slot));
                }
            }
        }
    }
}

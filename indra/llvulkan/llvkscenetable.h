/**
 * @file llvkscenetable.h
 * @brief The retained scene's tables as the CPU keeps them (research/197 stage 2): no Vulkan here.
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

#ifndef LL_LLVKSCENETABLE_H
#define LL_LLVKSCENETABLE_H

#include "stdtypes.h"
#include "llrhiscene.h"

#include <unordered_map>
#include <vector>

// The shader's own structs (scene_cull.comp, between its "shared types"
// markers; llvulkan.cmake writes them out as llvkscene_cull_types.inl), so the
// CPU rows are the GPU rows byte for byte.
namespace LLVKSceneShader
{
    typedef U32 uint;
#define SCENE_CONST static constexpr
#include "llvkscene_cull_types.inl"
#undef SCENE_CONST
    static_assert(sizeof(Record) == 64, "a record is 64 bytes (std430)");
    static_assert(sizeof(Transform) == 64, "a transform is 64 bytes (std430)");
    static_assert(sizeof(ViewData) == 176, "the view block is 176 bytes (std430)");
    static_assert(sizeof(BucketInfo) == 16, "a bucket is 16 bytes (std430)");
    static_assert(SCENE_TEXLIST_SHIFT == 8, "the expand reads a record's texture list row from flags >> 8 (llvkscene.cpp)");
    // research/219: a rigged record's palette base, in the same bits (a
    // rigged record has no texture list).
    static_assert(SCENE_SKINNED == 2u && SCENE_PALETTE_SHIFT == 8u,
                  "the expand and the shadows' skinned shaders read flags & 2 and flags >> 8 (llvkscene.cpp, llvknative.cpp)");
    static_assert(SCENE_PALETTE_LIMIT == LLRHI::SCENE_PALETTE_LIMIT, "the palette bases flags hold");

    // ==== scene rigged rules: begin ====
    // (tools/selftest/sim/gen_riggedtable.py checks this section and its mutants in sim_riggedtable.)
    // research/219: a record's flags. Bits 8 on hold a rigged record's
    // palette (SCENE_SKINNED: the first vec4 of its resident palette row)
    // or, else, its texture list's row (0: none).
    inline U32 record_flags(bool loose, U32 palette)
    {
        U32 flags = loose ? SCENE_LOOSE : 0u;
        if (palette != LLRHI::SCENE_NO_PALETTE)
        {
            flags |= SCENE_SKINNED | (palette << SCENE_PALETTE_SHIFT);
        }
        return flags;
    }
    inline U32 record_list(U32 flags)
    {
        return (flags & SCENE_SKINNED) ? 0u : flags >> SCENE_TEXLIST_SHIFT;
    }
    inline U32 record_palette(U32 flags)
    {
        return (flags & SCENE_SKINNED) ? flags >> SCENE_PALETTE_SHIFT : LLRHI::SCENE_NO_PALETTE;
    }
    // A rigged description the table takes: a palette its flags hold, loose
    // (its group's CPU cull bounds it), no texture list (the same bits), the
    // identity transform (its draws place it by its joints alone).
    inline bool rigged_desc_ok(U32 palette, bool loose, U32 list_count, U32 transform)
    {
        return palette == LLRHI::SCENE_NO_PALETTE || (palette < SCENE_PALETTE_LIMIT && loose && !list_count && transform == 0);
    }
    // The expand's word w of a record's row, after its textures and its
    // matrix row (value): a rigged record's palette word names its row.
    inline U32 expand_palette_word(U32 flags, U32 w, U32 palette_word, U32 value)
    {
        return w == palette_word && (flags & SCENE_SKINNED) ? flags >> SCENE_PALETTE_SHIFT : value;
    }
    // The expand's command of a rigged record in a pass whose program reads
    // no palette draws nothing.
    inline bool expand_draws_none(U32 flags, U32 palette_word)
    {
        return (flags & SCENE_SKINNED) != 0 && palette_word == 0xffffffffu;
    }
    // drawPass: a pass with rigged records is drawn from them only while
    // its program reads a palette and all its records are rigged (a pass
    // without rigged records as before).
    inline bool main_pass_palette_ok(U32 skinned, U32 plain, bool reads_palette)
    {
        return !skinned || (reads_palette && !plain);
    }
    // drawShadow: a skinned pass's records all rigged, another's none.
    inline bool shadow_pass_palette_ok(U32 skinned, U32 plain, bool skinned_mode)
    {
        return skinned_mode ? !plain : !skinned;
    }
    // ==== scene rigged rules: end ====
}

// The record table and its companions (research/197 2.2), the CPU's copy of
// what is on the GPU. Each write marks its row; takeWrites hands the rows
// written since its last call to the backend, which copies those rows (and
// only those) in the frame's upload commands.
//
// Records live in pages of SCENE_PAGE slots; a page belongs to one bucket
// (pass x vertex format x pool chunk x index size) and a bucket's pages are
// in the order it took them. A new record takes the first free lane of its
// bucket's first page with one, else a new page at the end: the table's order
// (pages, then lanes) is the order the GPU draws a bucket's records in, and
// changes only when records come and go (research/197 2.2 "順").
//
// A slot, a page, a transform or a texture transform freed in frame F is
// given out again from frame F + RELEASE_DELAY on: until then a frame in
// flight may still draw from it (its row stays as it was, the record marked
// dead so no later choice draws it).
//
// Not thread safe: the backend calls it under its lock.
class LLVKSceneTable
{
public:
    using Record = LLVKSceneShader::Record;
    using Transform = LLVKSceneShader::Transform;
    using ViewData = LLVKSceneShader::ViewData;
    using BucketInfo = LLVKSceneShader::BucketInfo;
    // research/205 H5: a texture list's row on the GPU: its numbers, 0 past its count.
    struct TextureList
    {
        U32 textures[LLRHI::SceneTextureList::MAX];
    };
    static_assert(sizeof(TextureList) == 64, "a texture list row is 64 bytes (the expand reads 16 words a row)");

    static constexpr U32 PAGE = LLVKSceneShader::SCENE_PAGE;
    static constexpr U32 NONE = LLVKSceneShader::SCENE_NONE;
    static constexpr U32 RELEASE_DELAY = 3;

    struct Limits
    {
        U32 pages = 4096;               // 262,144 records (16 MB)
        U32 transforms = 1u << 16;      // 4 MB
        U32 uv_transforms = 1u << 16;   // 4 MB
        U32 materials = 1u << 16;       // 6 MB
        U32 texture_lists = 1u << 14;   // research/205 H5: 1 MB (flags hold 24 bits of a row)
        U32 buckets = 1024;
        U32 groups = 1u << 20;
    };

    enum Table : U32
    {
        TABLE_RECORDS,      // Record
        TABLE_TRANSFORMS,   // Transform
        TABLE_UV,           // Transform (16 words)
        TABLE_MATERIALS,    // LLRHI::SceneMaterialDesc
        TABLE_PAGES,        // U32: the page's bucket (NONE: free)
        TABLE_TEXLISTS,     // research/205 H5: TextureList (row 0: none)
        TABLE_COUNT,
    };

    // A run of rows written since the last takeWrites.
    struct Run
    {
        U32 first = 0;
        U32 count = 0;
    };

    struct BucketKey
    {
        U32 pass = 0;
        U32 format = 0;
        U32 chunk = 0;
        bool index32 = false;
    };

    // The layout of a view's choice: per bucket its first command and pages.
    struct Plan
    {
        U32 serial = 0;             // the layout it was made for
        U32 pages = 0;              // page span
        U32 buckets = 0;            // bucket span
        U32 commands = 0;           // command room (every bucket's pages x PAGE)
        std::vector<BucketInfo> bucket_info;
        std::vector<U32> page_list;
    };

    struct Counters
    {
        U32 added = 0, updated = 0, moved = 0, removed = 0, refused = 0, stale = 0;
        U32 transform_writes = 0, uv_writes = 0, in_use_refused = 0;
    };

    explicit LLVKSceneTable(const Limits& limits = Limits());

    const Limits& limits() const { return mLimits; }

    // Hands out again what was freed RELEASE_DELAY frames before `frame`.
    // Frames count up (wrapping); the first call starts the count.
    void beginFrame(U32 frame);
    U32 frame() const { return mFrame; }

    U32 addGroup();                     // 0: refused
    void removeGroup(U32 group);        // and its records
    U32 groupSpan() const { return (U32)mGroups.size(); }
    bool groupLive(U32 group) const;

    U32 addRecord(const LLRHI::SceneRecordDesc& desc);      // 0: refused
    bool updateRecord(U32 id, const LLRHI::SceneRecordDesc& desc);
    // research/198 5.5: the record as it is with another range (its buffer's
    // place moved, was made or went: a new chunk is a new bucket).
    bool updateRange(U32 id, const LLRHI::SceneRange& range);
    void removeRecord(U32 id);
    bool recordLive(U32 id) const;
    U32 slotOf(U32 id) const;           // NONE: not live
    U32 recordAt(U32 slot) const;       // the live record's id in a slot, 0: none
    U32 liveRecords() const { return mLiveRecords; }
    // research/230 R2: moves whenever a row of any table is written (a record added, changed or moved, a transform set).
    U64 writeSerial() const { return mWriteSerial; }

    U32 addTransform(const F32 matrix[16]);
    void setTransform(U32 slot, const F32 matrix[16]);
    bool removeTransform(U32 slot);     // false: records still use it (nothing done)
    U32 addUVTransform(const F32 words[16]);
    void setUVTransform(U32 slot, const F32 words[16]);
    bool removeUVTransform(U32 slot);

    // Rows written since the last call, sorted and merged into runs.
    void takeWrites(Table table, std::vector<Run>& runs);
    // fix-r8: runs taken but not copied (no upload room): handed out again by the next takeWrites.
    void keepWrites(Table table, const std::vector<Run>& runs);
    bool anyWrites() const;
    U32 rowBytes(Table table) const;
    U32 rowSpan(Table table) const;     // rows the GPU table must hold
    U32 rowCapacity(Table table) const; // rows the GPU table is made for
    const void* rows(Table table) const;

    // Buckets.
    U32 bucketSpan() const { return (U32)mBuckets.size(); }
    const BucketKey& bucketKey(U32 bucket) const { return mBuckets[bucket].key; }
    U32 bucketPages(U32 bucket) const { return (U32)mBuckets[bucket].pages.size(); }
    const std::vector<U32>& passBuckets(U32 pass) const;

    // The layout as it is now (made again when pages came or went).
    const Plan& plan();

    // A view's block for the plan: planes, origin and where each output goes.
    // Returns the words the dispatch writes.
    static U32 makeView(const LLRHI::SceneViewDesc& desc, U32 group_words, const Plan& plan, ViewData& out);

    // The three phases of scene_cull.comp, run on the CPU over the tables as
    // given (the same functions, built as C++). out_words: makeView's words.
    static void emulate(const Record* records, const Transform* transforms, const U32* page_bucket, const ViewData& view,
                        const U32* visible, const BucketInfo* bucket_info, const U32* page_list, U32* out_words);
    // The same over this table's rows.
    void emulate(const ViewData& view, const U32* visible, const Plan& plan, std::vector<U32>& out_words) const;
    // scene_box_excess for a slot of this table (> 0: outside the view).
    F32 boxExcess(const ViewData& view, U32 slot) const;

    // Verify (LLRHI::Scene::collect): the records the choice for the view
    // draws for the pass (in draw order), and those of visible groups it
    // leaves out only by their bounds.
    void collect(const LLRHI::SceneViewDesc& desc, const U32* visible, U32 words, U32 pass, std::vector<U32>& drawn,
                 std::vector<U32>& bounded_out);

    const Counters& counters() const { return mCounters; }
    void resetCounters() { mCounters = Counters(); }

private:
    struct Handle
    {
        U32 slot = NONE;
        U32 group = 0;          // group slot
        U32 in_group = 0;       // place in its group's list
        U8 gen = 0;
        bool live = false;
    };

    struct Group
    {
        bool used = false;
        std::vector<U32> handles;
    };

    struct Bucket
    {
        BucketKey key;
        std::vector<U32> pages;     // in order
        U32 open = 0;               // pages below this have no free lane
    };

    struct Release
    {
        U32 row = 0;
        U32 frame = 0;
    };

    struct Slots
    {
        std::vector<U32> free;
        std::vector<Release> pending;
        std::vector<U32> users;     // records using the row
        std::vector<U8> live;
    };

    U32 findBucket(const LLRHI::SceneRecordDesc& desc, bool create);
    U32 takeSlot(U32 bucket);
    void freeSlot(U32 slot);
    void writeRecord(U32 slot, U32 handle, U32 bucket, const LLRHI::SceneRecordDesc& desc);
    bool validDesc(const LLRHI::SceneRecordDesc& desc) const;
    U32 internMaterial(const LLRHI::SceneMaterialDesc& material);
    void releaseMaterial(U32 row);
    // research/205 H5: the list's row (0: the desc has none; NONE: the table is full).
    U32 internList(const LLRHI::SceneTextureList& list);
    void releaseList(U32 row);
    U32 addRow(Slots& slots, std::vector<Transform>& rows, U32 limit, const F32* words, Table table);
    void setRow(Slots& slots, std::vector<Transform>& rows, U32 row, const F32* words, Table table);
    bool removeRow(Slots& slots, U32 row);
    void releaseRows(Slots& slots, U32 frame);
    void markWrite(Table table, U32 row) { mWrites[table].push_back(row); ++mWriteSerial; }
    U32 handleIndex(U32 id) const;      // NONE: not a live record's id

    Limits mLimits;
    U32 mFrame = 0;
    bool mFrameStarted = false;

    std::vector<Record> mRecords;           // PAGE per page in the span
    std::vector<U32> mSlotHandle;           // per slot: its handle, NONE
    std::vector<Handle> mHandles;
    std::vector<U32> mFreeHandles;
    std::vector<Release> mSlotReleases;
    U32 mLiveRecords = 0;

    std::vector<U32> mPageBucket;           // TABLE_PAGES
    std::vector<U64> mPageUsed;             // lanes live or waiting for release
    std::vector<U32> mFreePages;

    std::vector<Bucket> mBuckets;
    std::unordered_map<U64, U32> mBucketIndex;
    std::vector<U32> mPassBuckets[LLRHI::SCENE_MAX_PASSES];

    std::vector<Group> mGroups;
    std::vector<U32> mFreeGroups;

    std::vector<Transform> mTransforms;     // row 0: identity
    Slots mTransformSlots;
    std::vector<Transform> mUV;             // row 0: none
    Slots mUVSlots;

    std::vector<LLRHI::SceneMaterialDesc> mMaterials;
    Slots mMaterialSlots;
    std::unordered_multimap<U64, U32> mMaterialIndex;

    std::vector<TextureList> mLists;        // research/205 H5, row 0: none
    Slots mListSlots;
    std::unordered_multimap<U64, U32> mListIndex;

    std::vector<U32> mWrites[TABLE_COUNT];
    U32 mLayoutSerial = 1;
    U64 mWriteSerial = 0;
    Plan mPlan;
    Counters mCounters;

    // collect's scratch
    std::vector<U32> mCollectWords;
};

#endif // LL_LLVKSCENETABLE_H

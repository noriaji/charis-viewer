/**
 * @file llvkgl_texcompact.cpp
 * @brief The texture pools' blocks emptied into fuller ones while VRAM is short (research/285 X2).
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

#include "llvkgl_internal.h"

#if LL_GL_FUNC_POINTER

namespace llvkgl
{
    // ------------------------------------------------------------------
    // research/285 X2 (LLRHI::sTexCompact, with sBufferCompact). After an
    // event's peak the texture pools' blocks stayed part full (2028 MB used in
    // 3304 MB of blocks for minutes): textures went down a size class or away
    // and left holes, and VMA gives a block back only once it is empty. VRAM
    // stayed past the driver's budget and the discard bias at 4.
    //
    // While VRAM is short (vram_compaction_check below, both compactions'
    // state: from 92% of the budget or earlier while the bias is up, on down
    // to a little under it, or while the bias is up to 85% or under where it
    // comes down), the textures of the blocks at most half in use get new
    // images in fuller blocks of their own pool
    // (LLVKDevice::ImageMemory::ExistingPoolBlocks: never a new block), their
    // texels copied on the GPU in the frame's upload commands, and the old
    // images go once the GPU is done (llvkgl_texture.cpp, make_moved_image ...
    // take_moved_image). The emptied blocks go back to the driver (VMA frees
    // an empty block while over its budget, or when it has another empty
    // one; for the frames after a pass any, research/285 X4). A block is
    // emptied only when every allocation in it (VMA's count,
    // whoever made it) is a texture that can move; the others (render
    // targets, BC7 work, images of other kinds, textures written lately) keep
    // it. At the start of a frame, before anything of it is recorded (and
    // before the frame's BC7 swaps): nothing of this frame has written a
    // texture yet.
namespace
{
    constexpr U64 MB = 1024ull * 1024ull;
    constexpr F64 VRAM_SHORT = 0.92;            // of the driver's budget: both compactions start (vram_compaction_check)
    constexpr U64 STOP_UNDER_START = 64 * MB;   // ... and once started run down to this much under the start
    constexpr U64 UNDER_BIAS_FALL = 128 * MB;   // while the bias is up: start this far under where it comes down, if lower,
    constexpr F64 VRAM_TARGET = 0.85;           // ... and run down to this share of the budget, or STOP_UNDER_START
                                                // under the start, the lower
    constexpr F64 STATE_LOG_SECONDS = 30.0;     // between the lines that tell the compactions started (and then stopped)
    constexpr U64 COPY_BYTES_PER_PASS = 48 * MB;    // copied on the GPU a pass (research/285: 32-64 MB a frame)
    constexpr U32 MOVES_PER_PASS = 64;          // new images a pass (each an image, an allocation, later a view)
    constexpr U64 MOVE_COST_BYTES = 64 * 1024;  // a move weighed as this many bytes more to copy (the order of the blocks)
    constexpr U32 NO_PROGRESS_PASSES = 6;
    constexpr F64 WRITTEN_SECONDS = 1.0;        // written this lately (media, textures written every frame): stays
    constexpr U32 WRITTEN_FRAMES_UNKNOWN = 120; // ... in frames, before the frame rate is known
    constexpr F64 IDLE_LOG_SECONDS = 20.0;      // between the lines of checks that moved nothing while VRAM was short
    constexpr U64 POOL_LARGEST_BYTES = 8 * MB;  // the texture pools take images up to this (llvkdevice.cpp pool_group)
    constexpr U32 BIG_MOVES_PER_PASS = 2;       // images over it moved to memory of their own a pass (of the frame's 4),
                                                // out of default blocks that empty with them (that many in it at most)
    constexpr U64 BIG_MOVE_GAIN_MIN = 16 * MB;  // ... that give back at least this over the new memory (or a quarter of the block)

    // A pool block's part in a check (same index as sBlocks).
    struct BlockPlan
    {
        U32 first = 0;          // its textures in sItems (sorted by block)
        U32 textures = 0;       // that can move
        U64 room_fuller = 0;    // free bytes in the blocks of its pool fuller than it: where its textures may go
        bool candidate = false; // at most half in use
        bool moved_out = false; // this pass moved textures out of it: none go in
        bool moved_in = false;  // this pass moved textures into it: none go out
    };
    struct Item
    {
        U32 block;
        Texture* tex;
        U64 size;
    };
    // The candidate blocks a check left alone, and why.
    struct PlanCounts
    {
        U32 held = 0;           // an allocation in it cannot move
        U32 crowded = 0;        // its textures do not fit in the fuller blocks of its pool
    };
    struct PassResult
    {
        U64 freed = 0;          // the blocks emptied (an estimate: what VMA gives back of them)
        U64 copied = 0;
        U32 blocks = 0;
        U32 no_room = 0;        // moves given up: the new image found no fuller block
        U32 big = 0;            // of the moves, images over 8 MB out of VMA's default blocks
    };
    struct CompactCounts
    {
        U64 passes = 0;         // passes that moved textures
        U64 textures = 0;       // textures moved
        U64 bytes = 0;          // their images' bytes, copied
        U64 blocks = 0;         // blocks whose every texture moved
        U64 no_room = 0;        // moves given up: no fuller block of the pool had room
        U64 big = 0;            // images over 8 MB moved to memory of their own
    };
    // Why the checks while VRAM was short moved nothing, since the last such line.
    struct IdleCounts
    {
        U32 checks = 0;
        U32 no_candidates = 0;  // checks that found no block at most half in use
        U64 candidates = 0;     // blocks at most half in use (over the checks)
        U64 held = 0;           // PlanCounts', over the checks
        U64 crowded = 0;
        U64 no_room = 0;        // PassResult's, over the checks
        U32 not_recorded = 0;   // checks whose copies could not be recorded
    };

    // Kept between checks (no allocation once grown). Under sLock.
    std::vector<LLVKDevice::PoolBlock> sBlocks;
    std::vector<BlockPlan> sPlans;
    std::vector<U32> sByUsed;           // block indices by pool, the fullest first (find_candidates)
    std::vector<Item> sItems;
    std::vector<LLVKDevice::PoolBlock> sDefaultBlocks;  // VMA's default blocks (choose_big_moves)
    std::vector<U32> sDefaultTextures;  // by sDefaultBlocks' index: its images over 8 MB that can move
    std::vector<Item> sBigItems;        // those images (block: into sDefaultBlocks), by block
    std::vector<U32> sOrder;            // the blocks to empty, the cheapest first
    std::vector<U8> sPoolFull;          // by PoolBlock::pool: no fuller block took a texture this pass
    std::vector<ImageMove> sMoves;      // this pass's; emptied at its end
    CompactCounts sCounts;
    CompactCounts sLogged;              // the totals at the last log line
    IdleCounts sIdle;
    F64 sLastLog = -1.0e9;
    F64 sLastIdleLog = -1.0e9;
    // The time of the next check: read by compact_textures_due before sLock is
    // taken, on the frame's thread as compact_textures, which writes it.
    std::atomic<F64> sNextCheck{ 0.0 };
    // The tracker frame at about each second (compact_textures_due, every
    // frame while on; the same thread): WRITTEN_SECONDS in frames.
    FrameHistory sFrameHistory;
    bool sPassWaiting = false;          // the old images of the last pass may not be freed yet
    U32 sLastPassFrame = 0;             // the tracker frame of that pass
    U64 sLastPassBlocks = 0;            // the texture pools' block bytes then (0: none since VRAM had room)
    U32 sNoProgressPasses = 0;
    // vram_compaction_check's state, both compactions' (the frame's thread).
    bool sCompacting = false;
    CompactCheck sLastCheck;            // the last check's numbers (the line when the rests end the state)
    U8 sRested = 0;                     // CompactKind: the compactions whose last check while compacting rested
    F64 sLastStateLog = -1.0e9;
    bool sStartLogged = false;          // the start had a line: its stop gets one too
    bool sQuietStarts = false;          // the rests ended the state and VRAM has not been under the start since: no lines
    bool sTrimming = false;             // VMA keeps no empty block (LLVKDevice::setTrimEmptyBlocks)
    // What the passes of both freed lately (their estimates), not given back
    // yet: until the frames in flight are done (vram_compaction_freed).
    struct Freed
    {
        U32 frame = 0;
        U64 bytes = 0;
    };
    constexpr U32 FREED_SLOTS = 8;      // passes within COMPACT_PASS_FRAMES: two or three at most
    Freed sFreed[FREED_SLOTS];
    U32 sFreedNext = 0;

    // A plain image the BC7 encoder is likely to replace with one of its own
    // soon (as ensure_image's interim, research/285 X3): left in its default
    // block. Once the encoder would not take it (past what this GPU encodes,
    // the encoder off), it may move.
    bool bcn_may_replace(const Texture& tex)
    {
        return LLRHI::sTexBCN && tex.bcn_wanted && !tex.bcn_encoded && tex.format.format == VK_FORMAT_R8G8B8A8_UNORM &&
               LLVKTexCompress::mayEncode(tex.width, tex.height, tex.levels);
    }

    U64 freed_pending()
    {
        U64 bytes = 0;
        for (const Freed& f : sFreed)
        {
            bytes += f.bytes && sFrame - f.frame < COMPACT_PASS_FRAMES ? f.bytes : 0;
        }
        return bytes;
    }

    void rest(F64 now)
    {
        sNextCheck.store(now + COMPACT_REST_SECONDS, std::memory_order_relaxed);
    }

    // research/285 X4: VMA keeps no empty block while a pass's frees are to
    // come (vram_compaction_freed ... vram_compaction_tick); LLRHI_VK_EMPTY_TRIM=0
    // (safe mode: 0) never, for a comparison of the allocations' churn.
    void set_trimming(bool on)
    {
        static const bool allowed = LLRHI::envDefaultOn("LLRHI_VK_EMPTY_TRIM");
        on = on && allowed;
        if (sTrimming != on)
        {
            sTrimming = on;
            LLVKDevice::instance().setTrimEmptyBlocks(on);
        }
    }

    // The compactions that run now (CompactKind): the state ends once each rested.
    U8 running_compactions()
    {
        const bool setting = LLRHI::sBufferCompact.load(std::memory_order_relaxed);
        return (setting && buffer_compaction_allowed() ? COMPACT_BUFFERS : 0) | (setting && LLRHI::sTexCompact ? COMPACT_TEXTURES : 0);
    }

    // About WRITTEN_SECONDS in frames.
    U32 frames_lately(F64 now)
    {
        U32 frame = 0;
        return sFrameHistory.frameBefore(now, WRITTEN_SECONDS, frame) ? llmax(sFrame - frame, 2u) : WRITTEN_FRAMES_UNKNOWN;
    }

    // The index in `blocks` (sorted by memory) of the block whose memory is `memory` (U32_MAX: none).
    U32 find_block_in(const std::vector<LLVKDevice::PoolBlock>& blocks, VkDeviceMemory memory)
    {
        const auto it = std::lower_bound(blocks.begin(), blocks.end(), memory,
                                         [](const LLVKDevice::PoolBlock& b, VkDeviceMemory m) { return b.memory < m; });
        return it != blocks.end() && it->memory == memory ? (U32)(it - blocks.begin()) : U32_MAX;
    }

    // The index in sBlocks of the block whose memory is `memory` (U32_MAX: not a texture pool's).
    U32 find_block(VkDeviceMemory memory)
    {
        return find_block_in(sBlocks, memory);
    }

    // Each block's room in the blocks of its pool fuller than it (choose_moves
    // puts a texture only into a block fuller than its own was): by pool, from
    // the fullest down; blocks as full as each other are not fuller than one
    // another.
    void find_room_in_fuller()
    {
        const U32 count = (U32)sBlocks.size();
        sByUsed.resize(count);
        for (U32 i = 0; i < count; ++i)
        {
            sByUsed[i] = i;
        }
        std::sort(sByUsed.begin(), sByUsed.end(), [](U32 a, U32 b)
        {
            const LLVKDevice::PoolBlock& x = sBlocks[a];
            const LLVKDevice::PoolBlock& y = sBlocks[b];
            return x.pool != y.pool ? x.pool < y.pool : x.used > y.used;
        });
        for (U32 i = 0; i < count;)
        {
            const U32 pool = sBlocks[sByUsed[i]].pool;
            U64 room = 0;
            while (i < count && sBlocks[sByUsed[i]].pool == pool)
            {
                const U64 used = sBlocks[sByUsed[i]].used;
                U64 same_room = 0;
                for (; i < count && sBlocks[sByUsed[i]].pool == pool && sBlocks[sByUsed[i]].used == used; ++i)
                {
                    const LLVKDevice::PoolBlock& block = sBlocks[sByUsed[i]];
                    sPlans[sByUsed[i]].room_fuller = room;
                    same_room += block.size - block.used;
                }
                room += same_room;
            }
        }
    }

    // The blocks at most half in use, what all the texture pools take (used,
    // in blocks) and each block's room in fuller ones. Returns how many blocks
    // are at most half in use.
    U32 find_candidates(U64& pool_used, U64& pool_blocks)
    {
        sPlans.assign(sBlocks.size(), BlockPlan());
        pool_used = pool_blocks = 0;
        U32 candidates = 0, pools = 0;
        for (U32 i = 0; i < (U32)sBlocks.size(); ++i)
        {
            const LLVKDevice::PoolBlock& block = sBlocks[i];
            pool_used += block.used;
            pool_blocks += block.size;
            pools = llmax(pools, block.pool + 1);
            sPlans[i].candidate = block.allocations && block.used * 2 <= block.size;
            candidates += sPlans[i].candidate ? 1 : 0;
        }
        sPoolFull.assign(pools, 0);
        if (candidates)
        {
            find_room_in_fuller();
        }
        return candidates;
    }

    // The textures that can move out of the candidate blocks, by block and
    // the largest first (when the largest finds no room, the block is left
    // before its smaller ones were copied in vain), and in sOrder the blocks
    // to empty: every allocation in them such a texture, and room for them in
    // the fuller blocks of the pool; the most freed for the least copying first.
    // Also, in sBigItems (big_images: while images over 8 MB get memory of
    // their own), such images left in VMA's default blocks (the frame's few
    // with memory of their own taken, the cap reached, or to be stored BC7
    // and never were): in a default block they hold it, up to 64 MB, until
    // all else in it goes. Only of the blocks that hold nothing else, and few
    // enough to go in a pass: moved out of another, memory of their own would
    // add to VRAM and the block stay.
    PlanCounts plan_blocks(VmaAllocator allocator, U32 written_frames, bool big_images)
    {
        sItems.clear();
        sBigItems.clear();
        if (big_images)
        {
            LLVKDevice::instance().defaultBlocks(sDefaultBlocks);
        }
        else
        {
            sDefaultBlocks.clear();
        }
        sDefaultTextures.assign(sDefaultBlocks.size(), 0);
        sTextures.forEachNamed([&](GLuint, Texture& tex)
        {
            // Written in this frame's main commands (none can be at its start;
            // kept for safety), or lately: a texture written every frame (media)
            // would be copied for nothing and its draws recorded again.
            // Queued for a smaller image by the viewer (move_hold): that replaces
            // it soon, a copy now would be in vain.
            if (!image_movable(tex) || tex.late_write_frame == sFrame || tex.move_hold ||
                (tex.write_frame != U32_MAX && sFrame - tex.write_frame < written_frames))
            {
                return;
            }
            VmaAllocationInfo2 info = {};
            vmaGetAllocationInfo2(allocator, tex.memory, &info);
            if (info.dedicatedMemory)
            {
                return;
            }
            const U32 block = find_block(info.allocationInfo.deviceMemory);
            if (block == U32_MAX)
            {
                // Not a texture pool's: one of VMA's default blocks (textures go nowhere else).
                const U32 in_default = big_images ? find_block_in(sDefaultBlocks, info.allocationInfo.deviceMemory) : U32_MAX;
                if (in_default != U32_MAX && info.allocationInfo.size > POOL_LARGEST_BYTES && !bcn_may_replace(tex))
                {
                    ++sDefaultTextures[in_default];
                    sBigItems.push_back({ in_default, &tex, info.allocationInfo.size });
                }
                return;
            }
            if (!sPlans[block].candidate)
            {
                return;
            }
            ++sPlans[block].textures;
            sItems.push_back({ block, &tex, info.allocationInfo.size });
        });
        std::sort(sItems.begin(), sItems.end(), [](const Item& a, const Item& b)
        {
            return a.block != b.block ? a.block < b.block : a.size > b.size;
        });
        // The big images of the default blocks that empty with them, by block.
        sBigItems.erase(std::remove_if(sBigItems.begin(), sBigItems.end(), [](const Item& item)
        {
            const U32 count = sDefaultTextures[item.block];
            return count != sDefaultBlocks[item.block].allocations || count > BIG_MOVES_PER_PASS;
        }), sBigItems.end());
        std::sort(sBigItems.begin(), sBigItems.end(), [](const Item& a, const Item& b)
        {
            return a.block != b.block ? a.block < b.block : a.size > b.size;
        });
        for (U32 i = (U32)sItems.size(); i--;)
        {
            sPlans[sItems[i].block].first = i;
        }
        PlanCounts counts;
        sOrder.clear();
        for (U32 i = 0; i < (U32)sBlocks.size(); ++i)
        {
            const LLVKDevice::PoolBlock& block = sBlocks[i];
            const BlockPlan& plan = sPlans[i];
            if (!plan.candidate)
            {
                continue;
            }
            if (plan.textures != block.allocations)
            {
                ++counts.held;
            }
            else if (plan.room_fuller < block.used)
            {
                ++counts.crowded;
            }
            else
            {
                sOrder.push_back(i);
            }
        }
        std::sort(sOrder.begin(), sOrder.end(), [](U32 a, U32 b)
        {
            const LLVKDevice::PoolBlock& x = sBlocks[a];
            const LLVKDevice::PoolBlock& y = sBlocks[b];
            const F64 x_cost = (F64)(x.used + x.allocations * MOVE_COST_BYTES) / (F64)x.size;
            const F64 y_cost = (F64)(y.used + y.allocations * MOVE_COST_BYTES) / (F64)y.size;
            return x_cost != y_cost ? x_cost < y_cost : x.memory < y.memory;
        });
        return counts;
    }

    // New images in fuller blocks for the planned blocks' textures, into
    // sMoves, within the pass's limits; target: the bytes to free.
    PassResult choose_moves(VmaAllocator allocator, U64 target)
    {
        PassResult r;
        sMoves.clear();
        for (U32 index : sOrder)
        {
            if (r.freed >= target || sMoves.size() >= MOVES_PER_PASS || r.copied >= COPY_BYTES_PER_PASS)
            {
                break;
            }
            const LLVKDevice::PoolBlock& block = sBlocks[index];
            BlockPlan& plan = sPlans[index];
            if (plan.moved_in || sPoolFull[block.pool])
            {
                continue;
            }
            U32 left = plan.textures;
            for (U32 i = plan.first; i < plan.first + plan.textures; ++i)
            {
                const Item& item = sItems[i];
                if (sMoves.size() >= MOVES_PER_PASS || (r.copied && r.copied + item.size > COPY_BYTES_PER_PASS))
                {
                    break;      // the rest of the block in a later pass
                }
                ImageMove move;
                if (!make_moved_image(*item.tex, 0, LLVKDevice::ImageMemory::ExistingPoolBlocks, move))
                {
                    sPoolFull[block.pool] = 1;
                    ++r.no_room;
                    break;
                }
                // Only into a block fuller than this one was, that gives nothing
                // up this pass: never back into its own, an emptier one (blocks
                // would trade textures), one being emptied, or one made since
                // the blocks were read. VMA keeps its blocks in order only
                // roughly: the pool waits for the next pass.
                VmaAllocationInfo2 info = {};
                vmaGetAllocationInfo2(allocator, move.memory, &info);
                const U32 to = info.dedicatedMemory ? U32_MAX : find_block(info.allocationInfo.deviceMemory);
                if (to == U32_MAX || to == index || sPlans[to].moved_out || sBlocks[to].used <= block.used)
                {
                    drop_moved_image(move);
                    sPoolFull[block.pool] = 1;
                    ++r.no_room;
                    break;
                }
                sMoves.push_back(move);
                plan.moved_out = true;
                sPlans[to].moved_in = true;
                sBlocks[to].used += item.size;
                r.copied += item.size;
                --left;
            }
            if (!left)
            {
                r.freed += block.size;
                ++r.blocks;
            }
        }
        sCounts.no_room += r.no_room;
        return r;
    }

    // sBigItems' images into memory of their own (ImageMemory::OwnOnly: while
    // the frame's few and the cap allow, never a default block), a block's
    // all in one pass, into sMoves within the pass's limits. Only a block that
    // gives back a good share over the new memory (BIG_MOVE_GAIN_MIN or a
    // quarter of it): a full one would cost the copies and two new
    // allocations for next to nothing. The new memory is in VRAM's use at once
    // and the block goes only frames later: only while that keeps VRAM under
    // where the discard bias rises (LLRHI::sVRAMBiasRiseBytes; not known: no
    // move), so a move does not raise the bias for those frames; and the
    // whole block counts as freed (what comes off the use measured next).
    void choose_big_moves(const CompactCheck& check, PassResult& r)
    {
        const U64 limit = LLRHI::sVRAMBiasRiseBytes.load(std::memory_order_relaxed);
        U64 added = 0;      // the new memory of this pass's moves
        for (size_t first = 0; first < sBigItems.size();)
        {
            const U32 block = sBigItems[first].block;
            size_t end = first;
            U64 bytes = 0;
            for (; end < sBigItems.size() && sBigItems[end].block == block; ++end)
            {
                bytes += sBigItems[end].size;
            }
            const size_t count = end - first;
            const U64 block_size = sDefaultBlocks[block].size;
            const U64 gain = block_size > bytes ? block_size - bytes : 0;
            if (gain < llmax(BIG_MOVE_GAIN_MIN, block_size / 4) || check.usage + added + bytes >= limit)
            {
                first = end;
                continue;   // too little back, or too much new memory meanwhile: the block stays
            }
            if (r.big + count > BIG_MOVES_PER_PASS || sMoves.size() + count > MOVES_PER_PASS ||
                (r.copied && r.copied + bytes > COPY_BYTES_PER_PASS))
            {
                break;
            }
            size_t moved = 0;
            for (; moved < count; ++moved)
            {
                ImageMove move;
                if (!make_moved_image(*sBigItems[first + moved].tex, 0, LLVKDevice::ImageMemory::OwnOnly, move))
                {
                    break;      // the frame's few or the cap
                }
                sMoves.push_back(move);
            }
            if (moved < count)
            {
                // Not the whole block: what was made goes (moved alone, the block
                // would stay and the new memory add to VRAM); a later pass.
                for (size_t i = 0; i < moved; ++i)
                {
                    drop_moved_image(sMoves.back());
                    sMoves.pop_back();
                }
                break;
            }
            r.copied += bytes;
            r.big += (U32)count;     // counted once the copies are recorded (note_pass)
            r.freed += block_size;
            added += bytes;
            first = end;
        }
    }

    // A pass moved textures (sMoves): counted, and the next one waits for its
    // old images to go. No progress over a few passes (the blocks fill again
    // as fast as they empty): a rest, so textures are not moved again and again.
    void note_pass(F64 now, U64 pool_blocks, const PassResult& pass)
    {
        bool no_progress = false;
        if (sLastPassBlocks && pool_blocks >= sLastPassBlocks)
        {
            if (++sNoProgressPasses >= NO_PROGRESS_PASSES)
            {
                sNoProgressPasses = 0;
                rest(now);
                no_progress = true;
            }
        }
        else
        {
            sNoProgressPasses = 0;
        }
        sLastPassBlocks = pool_blocks;
        sLastPassFrame = sFrame;
        sPassWaiting = true;
        vram_compaction_freed(pass.freed);
        vram_compaction_rested(COMPACT_TEXTURES, no_progress);
        if (pass.big)
        {
            LLVKDevice::instance().noteBigMoved(pass.big);  // the large images' moves, now recorded
            sCounts.big += pass.big;
        }
        ++sCounts.passes;
        sCounts.textures += sMoves.size();
        sCounts.bytes += pass.copied;
        sCounts.blocks += pass.blocks;
    }

    // A line every 5 s at most while passes move textures (tag VKTextures).
    void log_pass(F64 now, U64 usage, U64 budget, U64 pool_used, U64 pool_blocks, const PassResult& pass)
    {
        if (now - sLastLog < 5.0)
        {
            return;
        }
        sLastLog = now;
        const CompactCounts& c = sCounts;
        const CompactCounts& l = sLogged;
        U32 empty_blocks = 0;
        U64 empty_bytes = 0;
        LLVKDevice::instance().emptyBlocks(empty_blocks, empty_bytes);
        LL_INFOS("VKTextures") << "VRAM short (" << (usage / MB) << " of " << (budget / MB)
                               << " MB): texture images moved into fuller blocks (research/285 X2): this pass " << sMoves.size()
                               << " textures (" << (pass.copied / MB) << " MB copied), " << pass.blocks << " blocks emptied, texture pools "
                               << (pool_used / MB) << " MB in " << (pool_blocks / MB) << " MB before it; since the last line "
                               << (c.passes - l.passes) << " passes, " << (c.textures - l.textures) << " textures, "
                               << ((c.bytes - l.bytes) / MB) << " MB, " << (c.blocks - l.blocks) << " blocks emptied, "
                               << (c.no_room - l.no_room) << " times no fuller block had room, " << (c.big - l.big)
                               << " images over 8 MB moved out of VMA's default blocks into memory of their own; VMA keeps " << empty_blocks
                               << " empty blocks (" << (empty_bytes / MB) << " MB)" << LL_ENDL;
        sLogged = sCounts;
    }

    // A check while VRAM was short moved nothing, and the compaction rests:
    // why, in a line every IDLE_LOG_SECONDS at most (tag VKTextures), so the
    // log tells "nothing it could do" from "VRAM has room" (no line at all).
    void note_idle(F64 now, U64 usage, U64 budget, U64 pool_used, U64 pool_blocks, U32 candidates, const PlanCounts& plan,
                   U32 no_room, bool not_recorded)
    {
        IdleCounts& i = sIdle;
        ++i.checks;
        i.no_candidates += candidates ? 0 : 1;
        i.candidates += candidates;
        i.held += plan.held;
        i.crowded += plan.crowded;
        i.no_room += no_room;
        i.not_recorded += not_recorded ? 1 : 0;
        if (now - sLastIdleLog < IDLE_LOG_SECONDS)
        {
            return;
        }
        sLastIdleLog = now;
        U32 empty_blocks = 0;
        U64 empty_bytes = 0;
        LLVKDevice::instance().emptyBlocks(empty_blocks, empty_bytes);
        LL_INFOS("VKTextures") << "VRAM short (" << (usage / MB) << " of " << (budget / MB)
                               << " MB) and no texture image moved (research/285 X2) in " << i.checks
                               << " checks since the last such line: no block at most half in use " << i.no_candidates
                               << " times; blocks at most half in use " << i.candidates << ", of them holding an allocation that cannot move "
                               << i.held << ", without room in the fuller blocks of their pool " << i.crowded
                               << "; moves given up for no fuller block with room " << i.no_room << ", copies not recorded "
                               << i.not_recorded << "; texture pools " << (pool_used / MB) << " MB in " << (pool_blocks / MB)
                               << " MB; VMA keeps " << empty_blocks << " empty blocks (" << (empty_bytes / MB) << " MB)" << LL_ENDL;
        sIdle = IdleCounts();
    }

    // The state on or off; `why` the stop's reason for its line.
    void set_compacting(bool on, const CompactCheck& c, const char* why)
    {
        if (sCompacting == on)
        {
            return;
        }
        sCompacting = on;
        sRested = 0;
        // A start every STATE_LOG_SECONDS at most has a line, and then its stop
        // too (none without the other in the log). After the rests ended the
        // state, its starts again while VRAM stays over the start line (every
        // rest, 10 s) have none: the passes' and the idle checks' lines tell.
        const F64 now = steady_seconds();
        if (on ? sQuietStarts || now - sLastStateLog < STATE_LOG_SECONDS : !sStartLogged)
        {
            return;
        }
        sStartLogged = on;
        if (on)
        {
            sLastStateLog = now;
        }
        if (!c.budget)
        {
            LL_INFOS("VKTextures") << "VRAM compaction (GL buffers and textures, research/285) stops: " << why << LL_ENDL;
            return;
        }
        LL_INFOS("VKTextures") << "VRAM compaction (GL buffers and textures, research/285) " << (on ? "starts" : "stops")
                               << (on ? "" : llformat(" (%s)", why)) << ": VRAM " << (c.usage / MB) << " of " << (c.budget / MB)
                               << " MB, starts at " << (c.start / MB) << " MB, runs down to " << (c.stop / MB) << " MB"
                               << (c.fall ? llformat(" (the discard bias and texture pressure come down under %llu MB)",
                                                     (unsigned long long)(c.fall / MB))
                                          : std::string())
                               << LL_ENDL;
    }
} // namespace

    bool vram_compaction_check(CompactCheck& out)
    {
        out = CompactCheck();
        LLVKDevice& device = LLVKDevice::instance();
        U64 image_bytes = 0;
        bool floored = false;
        const bool known = device.getAllocator() && device.vramBudget(image_bytes, out.usage, out.budget, &floored) && out.budget;
        if (known)
        {
            // A little under the start, so the two are apart.
            out.start = (U64)((F64)out.budget * VRAM_SHORT);
            out.stop = out.start > STOP_UNDER_START ? out.start - STOP_UNDER_START : 0;
            // A budget at its floor falls with the use (LLVKDevice::vramBudget):
            // neither the bias's line nor a state would ever be passed.
            out.fall = floored ? 0 : LLRHI::sVRAMBiasFallBytes.load(std::memory_order_relaxed);
            if (out.fall)
            {
                // The bias and the pressure stages come down under `fall`: run
                // down to below it, also where the 92% start was never reached
                // (a small GPU's margin is a larger share of its budget), or
                // to 85% if that is lower.
                const U64 line = out.fall > UNDER_BIAS_FALL ? out.fall - UNDER_BIAS_FALL : 0;
                out.start = llmin(out.start, line);
                out.stop = llmin((U64)((F64)out.budget * VRAM_TARGET), line > STOP_UNDER_START ? line - STOP_UNDER_START : 0);
            }
        }
        // Once started, on until under the stop line (a pass frees at most its
        // limits, the next one goes on); at its floor, each check on its own.
        // After the rests ended it, the next start is at the start line again
        // (at once while VRAM stays over it: what moves may have changed).
        if (!known || out.usage < out.start)
        {
            sQuietStarts = false;
        }
        const bool held = sCompacting && !floored;
        set_compacting(known && out.usage >= (held ? out.stop : out.start), out,
                       known ? "VRAM in use under the line" : "VRAM's budget not known");
        sLastCheck = out;
        out.compacting = sCompacting;
        if (!sCompacting)
        {
            return false;
        }
        // What the other compaction (or this one) freed in the last frames is
        // not given back yet: not asked for again.
        const U64 pending = freed_pending();
        const U64 over = out.usage > out.stop ? out.usage - out.stop : 0;
        out.to_free = over > pending ? over - pending : 0;
        return out.to_free > 0;
    }

    void vram_compaction_freed(U64 bytes)
    {
        if (bytes)
        {
            sFreed[sFreedNext] = { sFrame, bytes };
            sFreedNext = (sFreedNext + 1) % FREED_SLOTS;
            set_trimming(true);     // the blocks emptied go back (vram_compaction_tick ends it)
        }
    }

    void vram_compaction_tick()
    {
        // Also after the state ended: the last passes' frees are done with it.
        if (sTrimming && !freed_pending())
        {
            set_trimming(false);
        }
    }

    void vram_compaction_rested(CompactKind kind, bool rested)
    {
        if (!sCompacting)
        {
            return;
        }
        sRested = (U8)(rested ? (sRested | kind) : (sRested & ~kind));
        const U8 running = running_compactions();
        if (rested && running && (sRested & running) == running)
        {
            set_compacting(false, sLastCheck, "nothing more to give back: each compaction rested");
            sQuietStarts = true;
        }
    }

    void vram_compaction_stop()
    {
        set_compacting(false, CompactCheck(), "switched off, the device lost or the viewer quitting");
        set_trimming(false);
        sQuietStarts = false;
    }

    bool compact_textures_due()
    {
        if (!LLRHI::sTexCompact || !LLRHI::sBufferCompact.load(std::memory_order_relaxed) || vram_compaction_halted())
        {
            return false;
        }
        const F64 now = steady_seconds();
        sFrameHistory.note(now, sFrame);    // a frame about each second (frames_lately)
        return now >= sNextCheck.load(std::memory_order_relaxed);
    }

    bool compact_textures()
    {
        if (!compact_textures_due())
        {
            return false;   // off, or not yet: the switches and a clock
        }
        LLVKMirror& mirror = LLVKMirror::instance();
        LLVKDevice& device = LLVKDevice::instance();
        if (!mirror.isRecording() || (sPassWaiting && sFrame - sLastPassFrame < COMPACT_PASS_FRAMES))
        {
            return false;
        }
        sPassWaiting = false;
        const F64 now = steady_seconds();
        sNextCheck.store(now + COMPACT_CHECK_SECONDS, std::memory_order_relaxed);
        const U32 written_frames = frames_lately(now);

        if (!device.sizedPools())
        {
            rest(now);
            return false;   // LLRHI_VK_POOLS=0: textures share VMA's blocks with the rest (no state started)
        }
        VmaAllocator allocator = device.getAllocator();
        CompactCheck check;
        if (!allocator || !vram_compaction_check(check))
        {
            if (!check.compacting)
            {
                sNoProgressPasses = 0;
                sLastPassBlocks = 0;    // the next shortage starts counting progress afresh
            }
            return false;   // VRAM has room (or the last passes' frees are still to come): nothing moves
        }
        const U64 usage = check.usage, budget = check.budget;
        if (!device.poolBlocks(LLVKDevice::POOL_TEXTURE, sBlocks))
        {
            rest(now);
            return false;   // no allocator any more
        }
        U64 pool_used = 0, pool_blocks = 0;
        const U32 candidates = find_candidates(pool_used, pool_blocks);
        const bool big_images = device.bigImagesOwnMemory();
        if (!candidates && !big_images)
        {
            rest(now);
            note_idle(now, usage, budget, pool_used, pool_blocks, 0, PlanCounts(), 0, false);
            vram_compaction_rested(COMPACT_TEXTURES, true);
            return false;
        }
        const PlanCounts plan = plan_blocks(allocator, written_frames, big_images);
        PassResult pass = choose_moves(allocator, check.to_free);
        choose_big_moves(check, pass);
        if (sMoves.empty())
        {
            rest(now);
            note_idle(now, usage, budget, pool_used, pool_blocks, candidates, plan, pass.no_room, false);
            vram_compaction_rested(COMPACT_TEXTURES, true);
            return true;
        }
        // The copies before anything else of the frame, then the textures name
        // their new images (the old ones go after this frame's fence).
        if (!record_image_moves(mirror.uploadCommands(), sMoves.data(), (U32)sMoves.size()))
        {
            for (ImageMove& move : sMoves)
            {
                drop_moved_image(move);
            }
            sMoves.clear();
            rest(now);
            note_idle(now, usage, budget, pool_used, pool_blocks, candidates, plan, pass.no_room, true);
            return true;
        }
        for (const ImageMove& move : sMoves)
        {
            take_moved_image(move);
        }
        note_pass(now, pool_blocks, pass);
        log_pass(now, usage, budget, pool_used, pool_blocks, pass);
        sMoves.clear();     // no texture or image of this pass kept past it
        return true;
    }
} // namespace llvkgl

#endif // LL_GL_FUNC_POINTER

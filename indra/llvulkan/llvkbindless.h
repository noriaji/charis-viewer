/**
 * @file llvkbindless.h
 * @brief One descriptor array holding every sampled 2D texture, indexed by slot.
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

#ifndef LL_LLVKBINDLESS_H
#define LL_LLVKBINDLESS_H

#include "stdtypes.h"

#include <vulkan/vulkan.h>

#include <string>

// Shaders index the table (set 0, binding 0) with a slot number instead of
// having a descriptor set per combination of textures. Every pipeline layout
// that uses it puts it at set 0 and declares the same push constant range,
// so the set stays bound across pipelines (layout compatibility).
// One set per frame in flight, written with update-after-bind.
namespace LLVKBindless
{
    constexpr U32 WHITE_SLOT = 0;           // 1x1 white stand-in
    constexpr U32 PUSH_CONSTANT_BYTES = 128;

    // The table's slots: MIN_TABLE_SIZE unless LLRHI_BINDLESS_SLOTS asks for
    // another number, never more than tableLimit. A GPU whose limits do not
    // allow MIN_TABLE_SIZE lacks a required feature (LLVKDevice passes it
    // over, and the user is told as for a missing extension).
    constexpr U32 MIN_TABLE_SIZE = 16384;
    constexpr U32 MAX_TABLE_SIZE = 65536;
    // Room left under the limits for what the pipeline layouts hold beside
    // the table: they count every set's descriptors (the table's storage
    // buffers, the program's uniform blocks and samplers, the OIT set), not
    // only the update-after-bind ones.
    constexpr U32 TABLE_HEADROOM = 256;
    // The most slots this device's update-after-bind limits allow with
    // TABLE_HEADROOM to spare, at most MAX_TABLE_SIZE. The pool holds both
    // frames' sets (two tables and four storage buffers).
    inline U32 tableLimit(const VkPhysicalDeviceDescriptorIndexingProperties& indexing)
    {
        U32 limit = MAX_TABLE_SIZE + TABLE_HEADROOM;
        const U32 pool = indexing.maxUpdateAfterBindDescriptorsInAllPools;
        const U32 limits[] = { indexing.maxDescriptorSetUpdateAfterBindSampledImages,
                               indexing.maxPerStageDescriptorUpdateAfterBindSampledImages,
                               indexing.maxDescriptorSetUpdateAfterBindSamplers,
                               indexing.maxPerStageDescriptorUpdateAfterBindSamplers,
                               indexing.maxPerStageUpdateAfterBindResources,
                               pool / 2 };
        for (U32 value : limits)
        {
            limit = value < limit ? value : limit;
        }
        return limit > TABLE_HEADROOM ? limit - TABLE_HEADROOM : 0;
    }

    bool init();                            // once the device exists; false: unsupported
    VkDescriptorSetLayout layout();
    VkDescriptorSet set();                  // the set of the frame being recorded
    VkPushConstantRange pushConstantRange();

    constexpr U32 MAX_PUSHED_SLOTS = PUSH_CONSTANT_BYTES / 4;   // slot numbers a draw can pass

    // Slot for a GL texture name (0: white). Remembers what the slot must hold;
    // flush() writes it into the current frame's set.
    U32 slotFor(unsigned int texture);
    // The same in two steps, for callers that keep the slot with the texture:
    // acquire() once per texture, use() per draw with its current view (right
    // after acquire too: the reclaim's backoff counts on it). use()
    // returns the slot to sample: a slot already sampled this frame keeps its
    // entry (update-after-bind would hand the new view to the frame's earlier
    // draws too, which GL says still read the old image), so a texture whose
    // view or sampler changed mid-frame moves to a new slot.
    // serial: the view's serial (never the same twice, llvkgl's
    // Texture::view_serial): the sets' entries are compared by it too, so a
    // new view given a destroyed one's handle value is still written
    // (texture-leak 2026-10-27).
    U32 acquire(unsigned int texture);
    U32 use(U32 slot, unsigned int texture, VkImageView view, VkSampler sampler, U64 serial);
    void flush();
    // A deleted GL name gives its slot back.
    void release(unsigned int texture);
    // The slot's view or image is being destroyed. Its handle may come back for
    // a new view, so what the sets hold must not be trusted by handle any more.
    void forget(U32 slot);
    // The same for the slot the table gives the GL name now, if any
    // (texture-leak 2026-10-27: a texture's own note of its slot can be one it
    // left, after a reclaim or a move it did not see).
    void forgetTexture(unsigned int texture);
    // Moves whenever a texture leaves its slot (moved, released, taken back,
    // forgotten): caches of slot numbers compare it.
    U32 moves();
    // Slots held by textures, slots given back but not reusable yet, and the
    // table's size (LLRHI_VK_LIVE, research/77).
    void slotStats(U32& in_use, U32& waiting, U32& table);

    // What a slot holds in each frame's set, for diagnosing (frameReport).
    std::string describe(U32 slot);

    // ---- research/180 stage 1: what the slot publication needs (bindless side) ----
    // llvkgl keeps "GL name n samples slot s" while that stays true (its
    // slot publication). Draws that take such a slot skip acquire/use/flush:
    //  - setSlotLeft: called (under sLock) whenever a texture leaves its slot
    //    or the slot's set entries are forgotten (use's move, release, a
    //    reclaim, forget), with that texture's GL name.
    //  - setPendingTracking(true): use() also queues the slot for the other
    //    frame's set when that set does not hold what is wanted; writePending()
    //    writes the current frame's set from that queue (once a frame, before
    //    any draw takes a published slot), so both sets hold every published
    //    slot's entry although use() is no longer called for it every frame.
    //  - markUsed: what use() notes of a slot sampled this frame (its move of
    //    a slot whose view changes mid-frame, and the reclaim's age, read it).
    struct UsedMarks
    {
        U32* frames = nullptr;      // the table's used_frame (sized once, at init)
    };
    extern UsedMarks sUsedMarks;
    inline void markUsed(U32 slot, U32 frame) { sUsedMarks.frames[slot] = frame; }
    void setSlotLeft(void (*left)(unsigned int texture));
    void setPendingTracking(bool on);
    U32 writePending();             // entries written
    // What a published slot must hold (llvkgl's verify, research/180 5.1):
    // its owner, its wanted entry, and the current frame's set entry.
    enum SlotCheck : U32 { SLOT_OWNER_OK = 1, SLOT_WANTED_OK = 2, SLOT_WRITTEN_OK = 4 };
    U32 checkSlot(U32 slot, unsigned int texture, VkImageView view, VkSampler sampler, U64 serial);
    // ---- end of the slot publication (bindless side) ----

    // --- Per-draw matrices --------------------------------------------------
    // Set 0 binding 1: the frame's table of draw matrices. Translated shaders
    // read modelview / MVP / texture matrix 0 / normal matrix from the row
    // whose index the draw pushes (after its slot numbers) instead of from
    // their default uniform block, so a draw that only moves the model matrix
    // writes one row instead of copying and rebinding the whole block.
    struct DrawMatrices
    {
        F32 modelview[16];
        F32 modelview_projection[16];
        F32 texture0[16];
        F32 normal[12];     // mat3 in std430: three columns of four floats
    };
    static_assert(sizeof(DrawMatrices) == 240, "matches llrhi_draw_matrices in the shaders");
    constexpr U32 MATRIX_ROWS = 131072;     // per frame
    // A new row in the frame's table; its index goes out with the draw. Any
    // thread may take rows while a frame is recorded (research/66 step 1):
    // each takes blocks of rows atomically, so rows taken by one thread alone
    // come out numbered as before. The taker writes the row before the commands
    // that read it are handed to the recording worker (a stream filler: before
    // its Fill ends).
    DrawMatrices* matrixRow(U32& index);
    // count rows in one run (first, first + 1, ...): the rows count matrixRow
    // calls on this thread give while no other thread takes rows in between,
    // even when another does. Null: the run would reach the table's last row
    // (the rest of the table is used up, as by those calls).
    DrawMatrices* matrixRows(U32 count, U32& first);

    // --- Skinning palettes (research/42 C2) ----------------------------------
    // Set 0 binding 2: the frame's joint matrices as a flat array of vec4s.
    // Translated skinning shaders read matrixPalette[i] from it, starting at
    // the vec4 whose index the draw pushes (after its matrix row), instead of
    // from their default uniform block: a new palette is one copy into the
    // table and a push constant, not a copy of the whole block.
    //
    // Each frame slot has its own buffer. A frame that runs out of room gets
    // null for the rest of its palettes (their draws are skipped, never drawn
    // with other data) and the table grows to fit at the start of the next
    // frames, when each slot's last frame is done with its buffer (beginFrame).
    constexpr U32 PALETTE_INITIAL_VEC4S = 1u << 20;     // per frame slot (16 MB)
    constexpr U32 PALETTE_MAX_VEC4S = 1u << 24;         // growth limit (256 MB)
    // Room for `vec4s` vec4s (4 floats each) in the frame's palette table;
    // base: the first one's index, pushed with the draw. Null: no table, or
    // the frame's table is full. Any thread may call it while a frame is
    // recorded (one atomic step; the same rows as before when one thread
    // takes them all). The callers' caches of rows by content (llvkgl) are
    // not shared: they stay on the main thread.
    F32* paletteRows(U32 vec4s, U32& base);
    // The frame's palette table buffer (binding 2 of set()), for a compute pass
    // that writes rows it took with paletteRows (research/46 G5). Null: none.
    VkBuffer paletteBuffer();
    // research/199 (stage 4b): a resident region at the head of every slot's
    // table, whose rows keep their number from frame to frame (avatar x skin
    // palettes, llvkavatar.cpp); a frame's rolling rows (paletteRows) start
    // after it. 0 (the default) leaves the tables as before.
    // The region grows to vec4s at a later frame head (beginFrame; never shrinks),
    // once both slots' tables hold it (they grow first): never past a table.
    void setPaletteReserve(U32 vec4s);
    // vec4s of the region now (set at the frame head, the same all frame).
    U32 paletteReserved();
    // The table the frame being recorded writes (getSlot() & 1).
    U32 paletteTableSlot();
    // Rows [base, base + vec4s) of the region in a frame slot's table (host
    // memory, written as draws are recorded). Null: past the region, or the
    // slot's table is not that large yet (it grows at its own frame head).
    F32* paletteReservedRows(U32 table_slot, U32 base, U32 vec4s);
    // After LLVKMirror::beginFrame waited for the slot's last frame, before
    // anything binds the frame's set: grows the slot's palette table if the
    // frames before ran out of room. Every few seconds it also logs what the
    // texture table went through (a VKPerf "bindless table" line).
    void beginFrame();
    // vec4s the busiest frame asked for since the last call (also what did not
    // fit), and the current slot's room.
    void paletteStats(U32& peak, U32& capacity);
}

#endif // LL_LLVKBINDLESS_H

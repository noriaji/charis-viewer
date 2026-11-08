/**
 * @file llvkgl_packet.cpp
 * @brief Draw packets (research/180 stage 4 P1): a stretch of batches of the
 *        legacy material loop appended to the open draw run under one lock,
 *        each by the record beside its draw template.
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
#include "llvkgl_drawpath.h"
#include "llrhihash.h"

#include <intrin.h>

#if LL_GL_FUNC_POINTER

// What a packet does per call and per entry (research/180 10.3 P1).
//
// Per call (packet_head): the lock once; the program bound, the open run of
// it and nothing changed since its last draw (run_unchanged, run_pass_hit:
// the functions draw_template asks, once); the words every entry of the call
// pushes, from one gather_slots of a draw with the pass's matrices and no
// texture of its own on the packet's units (the other samplers, the matrix
// row, the constants row, the rows word: nothing the loop changes between
// its batches); where the packet's units and the material row go in them;
// and the material row with the loop's uniforms taken out (its hash), which
// each entry's patched row is made from.
//
// Per entry (packet_entry, packet_push): the template and the record beside
// it against the entry (buffers, range, names, the publication of each name,
// the uniform values), the pool's placement (redeem), the patched material
// row (made once per values and head per frame), the row and the indirect
// command. Nothing per entry reads a global version: the record's own words
// say whether it still holds (175 section 10.6, 180 10.2).
//
// The program's uniforms (material, uniform_data) are never written: an
// entry's values go into a copy, so the program keeps what the usual way's
// last batch set, as the viewer's uniform cache does.

namespace llvkgl
{
    // Never destroyed (as sTemplates): a slot may be released at exit.
    std::vector<PacketPart>& sPacketParts = *new std::vector<PacketPart>();

namespace
{
    constexpr U32 PATCH_NONE = U32_MAX;
    constexpr U32 MAX_PATCHES = 1u << 16;

    // ---- research/180 stage 4: the packet entry rule (tools/selftest sim_packet extracts it) ----
    // An entry's uniform values, interned by content (a record names one; permanent).
    struct PacketPatch
    {
        F32 values[LLRHI::PACKET_VALUES];
    };
    // The patch's material row this frame, for the head it was made under.
    struct PatchRow
    {
        U32 frame = U32_MAX;
        const Program* program = nullptr;
        U32 program_changes = 0;
        U64 key = 0, check = 0;     // the head's material row without the patched floats
        U32 base = 0;               // U32_MAX: the table was full
    };
    std::vector<PacketPatch>& sPatches = *new std::vector<PacketPatch>();
    std::vector<PatchRow>& sPatchRows = *new std::vector<PatchRow>();
    std::vector<F32> sHeadMaterial;     // the head's material row, the patched floats zeroed
    std::vector<F32> sPatchScratch;     // a patched row being made

    // What holds for every entry of a call (packet_head).
    struct PacketHead
    {
        const Program* program = nullptr;
        U32 program_changes = 0;
        U64 format = 0;                 // the open run's
        U32 chunk = 0;
        U32 unit_count = 0;
        U32 units[LLRHI::MAX_PACKET_UNITS] = {};
        U32 words[LLVKBindless::MAX_PUSHED_SLOTS] = {};
        U32 word_count = 0;
        // The words of the samplers reading a packet unit, and which unit (k).
        U32 slot_words[LLVKBindless::MAX_PUSHED_SLOTS] = {};
        U32 slot_word_units[LLVKBindless::MAX_PUSHED_SLOTS] = {};
        U32 slot_word_count = 0;
        U32 matrix_word = U32_MAX;
        U32 material_word = U32_MAX;
        // Per value float of an entry, its float in the material row (PATCH_NONE: not set by the program).
        U32 patch_at[LLRHI::PACKET_VALUES] = {};
        bool patched = false;
        U32 material_floats = 0;
        U64 key = 0, check = 0;
    };

    struct PacketAnswer
    {
        U32 slots[LLRHI::MAX_PACKET_UNITS] = {};
        U32 material = 0;
        LLVKAttribPool::Placement place;
    };

    inline bool same_values(const F32* a, const F32* b)
    {
        return memcmp(a, b, LLRHI::PACKET_VALUES * sizeof(F32)) == 0;
    }

    // The patch's material row under head h: the head's row with the patch's
    // values in their floats, by content (material_row_content); once per
    // patch, frame and head.
    U32 packet_patch_row(const PacketHead& h, U32 patch)
    {
        PatchRow& r = sPatchRows[patch];
        if (r.frame == sFrame && r.program == h.program && r.program_changes == h.program_changes &&
            r.key == h.key && r.check == h.check)
        {
            return r.base;
        }
        F32* row = sPatchScratch.data();
        memcpy(row, sHeadMaterial.data(), (size_t)h.material_floats * sizeof(F32));
        const PacketPatch& p = sPatches[patch];
        for (U32 i = 0; i < LLRHI::PACKET_VALUES; ++i)
        {
            if (h.patch_at[i] != PATCH_NONE)
            {
                row[h.patch_at[i]] = p.values[i];
            }
        }
        r.frame = sFrame;
        r.program = h.program;
        r.program_changes = h.program_changes;
        r.key = h.key;
        r.check = h.check;
        r.base = material_row_content(row, h.material_floats / 4);
        return r.base;
    }

    // Whether the entry may go as recorded (LLRHI::PACKET_DONE) and what it
    // takes; otherwise why not (nothing done but the pool's redeem).
    U32 packet_entry(const PacketHead& h, const LLRHI::PacketEntry& e, PacketAnswer& a)
    {
        const U32 id = *e.owner;
        if (!id || id > sTemplates.size() || id > sPacketParts.size())
        {
            return LLRHI::PACKET_OWNER;
        }
        DrawTemplate& t = sTemplates[id - 1];
        if (t.owner != e.owner)
        {
            return LLRHI::PACKET_OWNER;
        }
        if (t.program != h.program || t.program_changes != h.program_changes || t.format != h.format)
        {
            return LLRHI::PACKET_PROGRAM;
        }
        // The buffers the template was made from, and a range inside them (as draw_template).
        if (e.vertex_buffer != t.vertex_buffer || e.index_buffer != t.index_buffer || e.index_type != t.index_type)
        {
            return LLRHI::PACKET_OWNER;
        }
        if (e.buffer_vertices != t.buffer_vertices || e.buffer_indices != t.buffer_indices)
        {
            return LLRHI::PACKET_OWNER;
        }
        if (!e.index_count || (U64)e.first_index + e.index_count > e.buffer_indices)
        {
            return LLRHI::PACKET_OWNER;
        }
        const PacketPart& part = sPacketParts[id - 1];
        if (!part.live || part.program != h.program || part.program_changes != h.program_changes || part.unit_count != h.unit_count)
        {
            return LLRHI::PACKET_PART;
        }
        for (U32 k = 0; k < h.unit_count; ++k)
        {
            if (part.units[k] != h.units[k])
            {
                return LLRHI::PACKET_PART;
            }
            if (part.names[k] != e.names[k])
            {
                return LLRHI::PACKET_NAME;
            }
            a.slots[k] = part.slots[k];
            if (part.slots[k] == PACKET_NO_SLOT)
            {
                continue;   // no sampler reads the unit
            }
            // The publication the record was made from: the same slot, not cleared since.
            const PublishedSlot pub = published_handle_entry(e.names[k]);
            if (pub.slot1 != part.slots[k] + 1)
            {
                return LLRHI::PACKET_TEXTURE;
            }
            if (pub.gen != part.gens[k])
            {
                return LLRHI::PACKET_TEXTURE;
            }
        }
        a.material = h.material_word != U32_MAX ? h.words[h.material_word] : 0;
        if (h.material_word != U32_MAX && h.patched)
        {
            if (part.patch == PATCH_NONE || part.patch >= sPatches.size())
            {
                return LLRHI::PACKET_PART;
            }
            if (!same_values(sPatches[part.patch].values, e.values))
            {
                return LLRHI::PACKET_PATCH;
            }
            a.material = packet_patch_row(h, part.patch);
            if (a.material == U32_MAX)
            {
                return LLRHI::PACKET_ROW;
            }
        }
        if (!LLVKAttribPool::redeem(t.ticket, a.place))
        {
            return LLRHI::PACKET_POOL;
        }
        if (a.place.chunk != h.chunk)
        {
            return LLRHI::PACKET_RUN;
        }
        return LLRHI::PACKET_DONE;
    }
    // ---- end of the packet entry rule ----

    // Since the last report (perfReport); totals for the auto mode.
    struct PacketCounts
    {
        U64 calls = 0, drawn = 0, textures = 0;
        U64 own_matrices = 0;           // P1d: entries drawn with matrices of their own
        U64 stops[LLRHI::PACKET_STOP_COUNT] = {};
        U64 ticks = 0;                  // __rdtsc in the calls that drew (not verify's)
        U64 records = 0, record_refused = 0, record_unpublished = 0, record_patches_full = 0;
        U64 heads = 0, heads_ok = 0;    // verify's
        U64 checked = 0, would_take = 0, unrecorded = 0, mismatches = 0;
        U64 would_stop[LLRHI::PACKET_STOP_COUNT] = {};
        // Heads turned away because the program still had a batch's texture
        // animation in texture_matrix0 (the entries draw by the program's; a
        // loop that drew one the usual way and did not sync back). 0 expected.
        U64 texture_left = 0;
    };
    PacketCounts sCounts;
    U64 sTotalMismatches = 0;           // since the switches last changed (auto)
    U32 sMismatchesLogged = 0;          // the first few, once a run
    const char* const sStopNames[LLRHI::PACKET_STOP_COUNT] = { "done", "refused", "run", "owner", "program", "part", "name", "texture",
                                                               "patch", "pool", "row" };

    PacketHead sHead;
    // Verify's head (the last loop's that took one), compared within that loop.
    struct VerifyHead
    {
        bool ok = false;
        U32 loop = 0;
        U32 frame = U32_MAX;
        PacketHead head;
    };
    VerifyHead sVerifyHead;

    // The auto mode (LLRHI::sPacketsAuto): verify for this many frames of the
    // world with this many entries compared in each and not one mismatch, then on.
    constexpr U32 AUTO_FRAMES = 600;
    constexpr U64 AUTO_FRAME_ENTRIES = 1000;
    U32 sFrameSeen = U32_MAX;
    U64 sFrameChecked = 0;
    U32 sAutoFrames = 0;
    bool sSeenPackets = false, sSeenVerify = false, sSeenAny = false;

    // Once a frame, at its first packet call: the switches as the cue left
    // them, and the auto mode's decision on the frame before.
    void packet_frame()
    {
        if (sFrameSeen == sFrame)
        {
            return;
        }
        sFrameSeen = sFrame;
        const U64 checked = sFrameChecked;
        sFrameChecked = 0;
        if (!sSeenAny || LLRHI::sPackets != sSeenPackets || LLRHI::sPacketsVerify != sSeenVerify)
        {
            if (sSeenAny && LLRHI::sPacketsAuto)
            {
                // A cue changed a switch (the auto mode's own changes end it
                // first): the auto mode is over. packets.toggle from it is
                // off, not verify (verify alone draws the usual way).
                LLRHI::sPacketsAuto = false;
                if (!LLRHI::sPackets)
                {
                    LLRHI::sPacketsVerify = false;
                }
            }
            sSeenAny = true;
            sSeenPackets = LLRHI::sPackets;
            sSeenVerify = LLRHI::sPacketsVerify;
            sTotalMismatches = 0;
            sAutoFrames = 0;
            LL_INFOS("Vulkan") << "draw packets " << (sSeenVerify ? (LLRHI::sPacketsAuto ? "auto (verify first)" : "verify")
                                                                  : sSeenPackets ? "on" : "off")
                               << " (research/180 stage 4)" << LL_ENDL;
            return;
        }
        if (!LLRHI::sPacketsAuto)
        {
            return;
        }
        if (!LLRHI::sSlotPublish)
        {
            // The slot publication is off for this run (its own auto found a
            // mismatch, or a cue): no packet could go (packet_head refuses).
            LLRHI::sPackets = false;
            LLRHI::sPacketsVerify = false;
            LLRHI::sPacketsAuto = false;
            LL_INFOS("Vulkan") << "draw packets (auto): the slot publication is off: off for this run (research/180 stage 4)" << LL_ENDL;
            return;
        }
        if (sTotalMismatches)
        {
            LLRHI::sPackets = false;
            LLRHI::sPacketsVerify = false;
            LLRHI::sPacketsAuto = false;
            LL_WARNS("Vulkan") << "draw packets (auto): verify found " << sTotalMismatches
                               << " mismatches: off for this run (research/180 stage 4)" << LL_ENDL;
            return;
        }
        // Only frames of the world in which the slot publication takes (the
        // state packets draw in: before it, packet_head refuses) count.
        if (LLRHI::sWorldShown && slot_publish_mode() == 2 && checked >= AUTO_FRAME_ENTRIES && ++sAutoFrames >= AUTO_FRAMES)
        {
            LLRHI::sPacketsVerify = false;
            LLRHI::sPacketsAuto = false;
            LL_INFOS("Vulkan") << "draw packets (auto): " << AUTO_FRAMES << " frames of the world with " << AUTO_FRAME_ENTRIES
                               << " entries or more compared and no mismatch: on (research/180 stage 4)" << LL_ENDL;
        }
    }

    // Where the packet's uniforms go in the program's material row (patch_at,
    // PATCH_NONE where none). False: one is not a material uniform (the
    // program keeps it elsewhere): no packet for this program.
    bool patch_places(const Program& program, const LLRHI::Packet& p, U32* patch_at)
    {
        for (U32 i = 0; i < LLRHI::PACKET_VALUES; ++i)
        {
            patch_at[i] = PATCH_NONE;
        }
        if (p.uniform_count > LLRHI::MAX_PACKET_UNIFORMS || (p.uniform_count && !program.material_vec4s))
        {
            return false;
        }
        for (U32 j = 0; j < p.uniform_count; ++j)
        {
            const LLRHI::PacketUniform& u = p.uniforms[j];
            if (u.slot < 0 || (size_t)u.slot >= program.native_slots.size() || !u.components || u.first + u.components > LLRHI::PACKET_VALUES)
            {
                return false;
            }
            const UniformLocation& loc = program.native_slots[u.slot];
            if (loc.material < 0 || (size_t)loc.material >= program.material_members.size() || loc.element < 0)
            {
                return false;
            }
            const MaterialMember& member = program.material_members[loc.material];
            if ((U32)loc.element >= member.count)
            {
                return false;
            }
            // As apply_material writes it: the call's first 4 components at the element's vec4.
            const U32 words = llmin(u.components, 4u);
            for (U32 w = 0; w < words; ++w)
            {
                patch_at[u.first + w] = (member.first + (U32)loc.element) * 4 + w;
            }
        }
        return true;
    }

    // Two 64-bit hashes of `floats` words (the head's key; independent mixes).
    void hash_words(const F32* data, U32 floats, U64& key, U64& check)
    {
        U64 a = LLRHI::FNV1A_BASIS_SHORT ^ floats;
        U64 b = 0x9e3779b97f4a7c15ull + floats;
        for (U32 i = 0; i < floats; ++i)
        {
            U32 v;
            memcpy(&v, data + i, 4);
            a = LLRHI::fnv1aMix(a, v);
            b = (b + v) * 0xc2b2ae3d27d4eb4full;
            b ^= b >> 29;
        }
        key = a;
        check = b;
    }

    bool is_identity(const F32* m)
    {
        for (U32 i = 0; i < 16; ++i)
        {
            if (m[i] != ((i % 5) == 0 ? 1.f : 0.f))
            {
                return false;
            }
        }
        return true;
    }

    // What holds for every entry of this call, or why the packet cannot go
    // (PACKET_REFUSED: not this program or pass at all; PACKET_RUN: not now).
    U32 packet_head(const LLRHI::Packet& p, PacketHead& h)
    {
        DrawRun& run = sRun;
        if (!LLRHI::sDrawRuns || !LLRHI::sDrawTemplates || sCapture.on || LLVKDevice::instance().checkpointsEnabled() || !p.matrices ||
            p.unit_count > LLRHI::MAX_PACKET_UNITS)
        {
            return LLRHI::PACKET_REFUSED;   // a capture needs each draw's attributes; checkpoints, each draw recorded
        }
        Program* program = current_program();
        if (!program || !program->built || !program->draw_rows || program->palette_columns)
        {
            return LLRHI::PACKET_REFUSED;
        }
        // The entries' matrix rows take texture0 from the program (no entry
        // has an animation of its own): it has to be the identity, or every
        // entry would draw with the last animated batch's (verify sees it too).
        if ((program->matrix_mask & (1u << MATRIX_TEXTURE0)) && !is_identity(program->matrices.texture0))
        {
            ++sCounts.texture_left;
            return LLRHI::PACKET_RUN;   // this one the usual way (which syncs), then packets again
        }
        // The packet's units are the draws' own textures: none may be sampled
        // through descriptor images, which take the GL binding only (as draw_template).
        U32 texture_count = 0;
        for (U32 k = 0; k < p.unit_count; ++k)
        {
            if (p.units[k] >= 32)
            {
                return LLRHI::PACKET_REFUSED;
            }
            for (GLint unit : program->unit_values)
            {
                if (unit == (GLint)p.units[k])
                {
                    return LLRHI::PACKET_REFUSED;
                }
            }
            texture_count = llmax(texture_count, p.units[k] + 1);
        }
        if (!patch_places(*program, p, h.patch_at))
        {
            return LLRHI::PACKET_REFUSED;
        }
        if (!run.open || run.program != program || run.rows != program->draw_rows || run.frame != sFrame)
        {
            return LLRHI::PACKET_RUN;
        }
        if (!run_unchanged(*program) || !run_pass_hit(*program))
        {
            return LLRHI::PACKET_RUN;
        }
        // The words of a draw with the pass's matrices and none of the
        // packet's textures (name 0: the white stand-in, nothing resolved).
        LLRHI::DirectDraw head;
        head.modelview = p.matrices->modelview;
        head.modelview_projection = p.matrices->modelview_projection;
        head.normal = p.matrices->normal;
        head.inverse_modelview = p.matrices->inverse_modelview;
        head.matrix_id = p.matrices->matrix_id;
        U32 textures[32];
        for (U32 i = 0; i < texture_count; ++i)
        {
            textures[i] = LLRHI::DirectDraw::KEEP_UNIT;
        }
        for (U32 k = 0; k < p.unit_count; ++k)
        {
            textures[p.units[k]] = 0;
        }
        head.textures = textures;
        head.texture_count = texture_count;
        bool ok = false;
        {
            const DirectScope direct(&head);
            PerfScope scope(PERF_DESCRIPTORS);
            gather_slots(*program, run.target);
            ok = !tPaletteFull;
            for (const Texture* tex : tBindlessSampled)
            {
                ok &= tex->layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }
        }
        if (!ok || run.commands != run_commands())
        {
            return LLRHI::PACKET_RUN;
        }
        if (tSlotCount != program->draw_row_words + 1 || tSlotCount > LLVKBindless::MAX_PUSHED_SLOTS)
        {
            return LLRHI::PACKET_REFUSED;
        }
        // The entries' textures take published slots (verify: published, not taken).
        if (slot_publish_mode() < (p.verify ? 1u : 2u))
        {
            return LLRHI::PACKET_REFUSED;
        }
        memcpy(h.words, tSlots, (size_t)tSlotCount * 4);
        h.word_count = tSlotCount;
        // gather_slots' order: samplers, matrix, palette, light, material, constants, rows.
        const U32 samplers = (U32)program->bindless.size();
        h.slot_word_count = 0;
        for (U32 i = 0; i < samplers && i < h.word_count; ++i)
        {
            for (U32 k = 0; k < p.unit_count; ++k)
            {
                if (program->bindless[i].unit == (GLint)p.units[k])
                {
                    h.slot_words[h.slot_word_count] = i;
                    h.slot_word_units[h.slot_word_count] = k;
                    ++h.slot_word_count;
                }
            }
        }
        h.matrix_word = program->matrix_mask ? samplers : U32_MAX;
        h.material_word = U32_MAX;
        if (program->material_vec4s)
        {
            h.material_word = samplers + (program->matrix_mask ? 1 : 0) + (program->light_table ? 1 : 0);
            if (h.material_word + 1 >= h.word_count)
            {
                return LLRHI::PACKET_REFUSED;   // not the layout gather_slots makes
            }
        }
        h.patched = p.uniform_count != 0;
        h.material_floats = program->material_vec4s * 4;
        if (h.patched)
        {
            // The head's material row without the loop's uniforms: what every patched row is made from.
            if (program->material.size() < h.material_floats)
            {
                return LLRHI::PACKET_REFUSED;
            }
            // Both grow only (not per frame).
            if (sHeadMaterial.size() < h.material_floats)
            {
                sHeadMaterial.resize(h.material_floats);
            }
            if (sPatchScratch.size() < h.material_floats)
            {
                sPatchScratch.resize(h.material_floats);
            }
            memcpy(sHeadMaterial.data(), program->material.data(), (size_t)h.material_floats * sizeof(F32));
            for (U32 i = 0; i < LLRHI::PACKET_VALUES; ++i)
            {
                if (h.patch_at[i] != PATCH_NONE)
                {
                    if (h.patch_at[i] >= h.material_floats)
                    {
                        return LLRHI::PACKET_REFUSED;
                    }
                    sHeadMaterial[h.patch_at[i]] = 0.f;
                }
            }
            hash_words(sHeadMaterial.data(), h.material_floats, h.key, h.check);
        }
        h.program = program;
        h.program_changes = sProgramChanges;
        h.format = run.format;
        h.chunk = run.chunk;
        h.unit_count = p.unit_count;
        for (U32 k = 0; k < p.unit_count; ++k)
        {
            h.units[k] = p.units[k];
        }
        return LLRHI::PACKET_DONE;
    }

    // The entry's draw, appended to the open run (room checked): its words in
    // a row, its indirect command, its published slots marked used, and the
    // counts draw_template makes. False: the frame's table is full.
    bool packet_push(Program& program, const PacketHead& h, const LLRHI::PacketEntry& e, const PacketAnswer& a)
    {
        memcpy(tSlots, h.words, (size_t)h.word_count * 4);
        tSlotCount = h.word_count;
        for (U32 j = 0; j < h.slot_word_count; ++j)
        {
            tSlots[h.slot_words[j]] = a.slots[h.slot_word_units[j]];
        }
        if (h.material_word != U32_MAX)
        {
            tSlots[h.material_word] = a.material;
        }
        if (e.matrices && h.matrix_word != U32_MAX)
        {
            // P1d: the entry's own matrices: their row, as gather_slots makes
            // it for a draw with them (the same id in a row: the same row).
            const DirectScope direct(e.matrices);
            tSlots[h.matrix_word] = matrix_row(program);
            ++sCounts.own_matrices;
        }
        U32 first_instance = 0;
        if (!run_row(first_instance))
        {
            return false;
        }
        for (U32 k = 0; k < h.unit_count; ++k)
        {
            if (a.slots[k] != PACKET_NO_SLOT)
            {
                LLVKBindless::markUsed(a.slots[k], sFrame);     // as slot_publish_take
                ++sCounts.textures;
            }
        }
        run_push(e.index_count, a.place.first_index + e.first_index, a.place.vertex_offset, first_instance);
        ++program.generic_draws;
        ++sPassHits;
        ++sRunDraws;
        ++sRunFast;
        ++sTemplateDraws;
        ++sDirectDraws;
        return true;
    }

    // The interned patch of these values (PATCH_NONE: no room, or a hash shared with other values).
    U32 intern_patch(const F32* values)
    {
        static std::unordered_map<U64, U32>& index = *new std::unordered_map<U64, U32>();  // record time only (not per entry); never destroyed
        U64 key = 0, check = 0;
        hash_words(values, LLRHI::PACKET_VALUES, key, check);
        auto it = index.find(key);
        if (it != index.end())
        {
            return same_values(sPatches[it->second].values, values) ? it->second : PATCH_NONE;
        }
        if (sPatches.size() >= MAX_PATCHES)
        {
            ++sCounts.record_patches_full;
            return PATCH_NONE;
        }
        const U32 id = (U32)sPatches.size();
        PacketPatch patch;
        memcpy(patch.values, values, sizeof(patch.values));
        sPatches.push_back(patch);
        sPatchRows.emplace_back();
        index.emplace(key, id);
        return id;
    }

    void mismatch(const char* what, U32 id, U64 a, U64 b)
    {
        ++sCounts.mismatches;
        ++sTotalMismatches;
        if (sMismatchesLogged < 8)
        {
            ++sMismatchesLogged;
            LL_WARNS("Vulkan") << "packet verify: template " << id << " " << what << ": packet " << a << ", usual way " << b
                               << " (research/180 stage 4)" << LL_ENDL;
        }
    }

    bool same_floats(const F32* a, const F32* b, U32 count)
    {
        return (!a && !b) || (a && b && memcmp(a, b, (size_t)count * sizeof(F32)) == 0);
    }

    // Verify: the draw `d` the usual way just made for the entry (tSlots its
    // words) against what a packet would have made from the record.
    void packet_verify(const Program& program, const LLRHI::DirectDraw& d, const LLRHI::Packet& p, const LLRHI::PacketEntry& e,
                       const PacketPart& part, U32 id)
    {
        PacketCounts& c = sCounts;
        ++c.checked;
        // What the viewer gave the entry: the draw's own names, buffers, range and matrices.
        for (U32 k = 0; k < p.unit_count; ++k)
        {
            if (d.textures && p.units[k] < d.texture_count && d.textures[p.units[k]] != LLRHI::DirectDraw::KEEP_UNIT &&
                d.textures[p.units[k]] != e.names[k])
            {
                mismatch("texture name (viewer)", id, e.names[k], d.textures[p.units[k]]);
            }
        }
        if (e.vertex_buffer != d.vertex_buffer || e.index_buffer != d.index_buffer || e.index_type != d.index_type ||
            e.buffer_vertices != d.buffer_vertices || e.buffer_indices != d.buffer_indices || e.first_index != d.first_index ||
            e.index_count != d.index_count)
        {
            mismatch("buffers or range", id, e.vertex_buffer, d.vertex_buffer);
        }
        const LLRHI::DirectDraw* m = e.matrices ? e.matrices : p.matrices;     // P1d: the entry's own, else the packet's
        if (!m || !same_floats(m->modelview, d.modelview, 16) || !same_floats(m->modelview_projection, d.modelview_projection, 16) ||
            !same_floats(m->normal, d.normal, 9) || !same_floats(m->inverse_modelview, d.inverse_modelview, 16) || d.texture_matrix0)
        {
            mismatch("matrices", id, 0, 1);
        }
        // Would a packet take it from the record, and with what?
        if (!part.live)
        {
            ++c.unrecorded;
            return;
        }
        if (part.program != &program || part.program_changes != sProgramChanges || part.unit_count != p.unit_count)
        {
            ++c.would_stop[LLRHI::PACKET_PART];
            return;
        }
        for (U32 k = 0; k < p.unit_count; ++k)
        {
            if (part.units[k] != p.units[k])
            {
                ++c.would_stop[LLRHI::PACKET_PART];
                return;
            }
            if (part.names[k] != e.names[k])
            {
                ++c.would_stop[LLRHI::PACKET_NAME];
                return;
            }
            if (part.slots[k] == PACKET_NO_SLOT)
            {
                continue;
            }
            const PublishedSlot pub = published_handle_entry(e.names[k]);
            if (pub.slot1 != part.slots[k] + 1 || pub.gen != part.gens[k])
            {
                ++c.would_stop[LLRHI::PACKET_TEXTURE];
                return;
            }
        }
        // The slots it would take: what the usual way took.
        const U32 samplers = (U32)program.bindless.size();
        for (U32 i = 0; i < samplers && i < tSlotCount; ++i)
        {
            for (U32 k = 0; k < p.unit_count; ++k)
            {
                if (program.bindless[i].unit == (GLint)p.units[k] && part.slots[k] != PACKET_NO_SLOT && tSlots[i] != part.slots[k])
                {
                    mismatch("texture slot", id, part.slots[k], tSlots[i]);
                }
            }
        }
        // The material row it would make: the program's row (the usual way
        // just set these values) with the record's values in their floats.
        const U32 material_word = program.material_vec4s ? samplers + (program.matrix_mask ? 1 : 0) + (program.palette_columns ? 1 : 0) +
                                                           (program.light_table ? 1 : 0)
                                                         : U32_MAX;
        if (p.uniform_count && material_word < tSlotCount)
        {
            if (part.patch == PATCH_NONE || part.patch >= sPatches.size())
            {
                ++c.would_stop[LLRHI::PACKET_PART];
                return;
            }
            if (!same_values(sPatches[part.patch].values, e.values))
            {
                ++c.would_stop[LLRHI::PACKET_PATCH];
                return;
            }
            U32 patch_at[LLRHI::PACKET_VALUES];
            const U32 floats = program.material_vec4s * 4;
            if (patch_places(program, p, patch_at) && program.material.size() >= floats && !tPaletteFull)
            {
                if (sPatchScratch.size() < floats)
                {
                    sPatchScratch.resize(floats);   // grows only
                }
                F32* row = sPatchScratch.data();
                memcpy(row, program.material.data(), (size_t)floats * sizeof(F32));
                for (U32 i = 0; i < LLRHI::PACKET_VALUES; ++i)
                {
                    if (patch_at[i] != PATCH_NONE && patch_at[i] < floats)
                    {
                        row[patch_at[i]] = sPatches[part.patch].values[i];
                    }
                }
                const U32 base = material_row_content(row, program.material_vec4s);
                if (base != U32_MAX && base != tSlots[material_word])
                {
                    mismatch("material row", id, base, tSlots[material_word]);
                }
            }
        }
        // The words every entry of the loop shares (the head's): the usual way's.
        const VerifyHead& v = sVerifyHead;
        if (v.ok && v.loop == p.loop && v.frame == sFrame && v.head.program == &program && v.head.program_changes == sProgramChanges)
        {
            if (v.head.word_count != tSlotCount)
            {
                mismatch("word count", id, v.head.word_count, tSlotCount);
            }
            else
            {
                for (U32 w = 0; w < tSlotCount; ++w)
                {
                    bool own = w == v.head.matrix_word || w == v.head.material_word;
                    for (U32 j = 0; j < v.head.slot_word_count && !own; ++j)
                    {
                        own = v.head.slot_words[j] == w;
                    }
                    if (!own && v.head.words[w] != tSlots[w])
                    {
                        mismatch("head word", id, ((U64)w << 32) | v.head.words[w], tSlots[w]);
                    }
                }
            }
        }
        ++c.would_take;
        ++sFrameChecked;
    }

    // The record of what the usual way's draw of the entry resolved (tSlots,
    // tSlotNames its words and names), written over the old one. A record is
    // only made from a draw whose textures came by the entry's names and are
    // published at the slots it took, and whose material row holds the
    // entry's values: otherwise the part stays empty (the packet stops at it).
    void packet_record(const Program& program, const LLRHI::Packet& p, const LLRHI::PacketEntry& e, PacketPart& part)
    {
        PacketCounts& c = sCounts;
        ++c.records;
        part = PacketPart();
        PacketPart r;
        r.program = &program;
        r.program_changes = sProgramChanges;
        r.unit_count = (U8)p.unit_count;
        const U32 samplers = (U32)program.bindless.size();
        for (U32 k = 0; k < p.unit_count; ++k)
        {
            r.units[k] = (U8)p.units[k];
            r.names[k] = e.names[k];
            r.slots[k] = PACKET_NO_SLOT;
            for (U32 i = 0; i < samplers; ++i)
            {
                if (program.bindless[i].unit != (GLint)p.units[k])
                {
                    continue;
                }
                if (i >= tSlotCount || tSlotNames[i] != e.names[k] || (r.slots[k] != PACKET_NO_SLOT && r.slots[k] != tSlots[i]))
                {
                    ++c.record_refused;
                    return;
                }
                r.slots[k] = tSlots[i];
            }
            if (r.slots[k] == PACKET_NO_SLOT)
            {
                continue;
            }
            const PublishedSlot pub = published_handle_entry(e.names[k]);
            if (pub.slot1 != r.slots[k] + 1)
            {
                ++c.record_unpublished;
                return;
            }
            r.gens[k] = pub.gen;
        }
        if (p.uniform_count)
        {
            U32 patch_at[LLRHI::PACKET_VALUES];
            if (!patch_places(program, p, patch_at))
            {
                ++c.record_refused;
                return;
            }
            // The program's row holds the entry's values (the usual way just set them).
            for (U32 i = 0; i < LLRHI::PACKET_VALUES; ++i)
            {
                if (patch_at[i] != PATCH_NONE &&
                    (patch_at[i] >= program.material.size() || memcmp(&program.material[patch_at[i]], &e.values[i], sizeof(F32))))
                {
                    ++c.record_refused;
                    return;
                }
            }
            r.patch = intern_patch(e.values);
            if (r.patch == PATCH_NONE)
            {
                return;
            }
        }
        r.live = true;
        part = r;
    }
} // namespace

    void packet_note(const Program& program, const LLRHI::DirectDraw& d)
    {
        const LLRHI::PacketNote& n = *LLRHI::tPacketNote;
        if (!n.entry || !n.packet || d.draw_template != n.entry->owner)
        {
            return;     // another draw than the note's batch
        }
        const U32 id = *d.draw_template;
        if (!id || id > sTemplates.size() || sTemplates[id - 1].owner != d.draw_template)
        {
            return;
        }
        if (sPacketParts.size() < sTemplates.size())
        {
            sPacketParts.resize(sTemplates.size());     // with the templates (geometric growth, not per frame)
        }
        PacketPart& part = sPacketParts[id - 1];
        if (n.verify)
        {
            packet_verify(program, d, *n.packet, *n.entry, part, id);
        }
        packet_record(program, *n.packet, *n.entry, part);
    }

    U32 packet_draw(LLRHI::Packet& p)
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        PerfScope total(PERF_DRAW);
        const U64 start = __rdtsc();
        packet_frame();
        PacketCounts& c = sCounts;
        PacketHead& h = sHead;
        U32 stop = packet_head(p, h);
        if (p.verify)
        {
            // Verify draws nothing here: the head is kept for the loop's usual-way draws.
            ++c.heads;
            c.heads_ok += stop == LLRHI::PACKET_DONE ? 1 : 0;
            sVerifyHead.ok = stop == LLRHI::PACKET_DONE;
            if (sVerifyHead.ok)
            {
                sVerifyHead.loop = p.loop;
                sVerifyHead.frame = sFrame;
                sVerifyHead.head = h;
            }
            p.stop = stop;
            return 0;
        }
        ++c.calls;
        U32 drawn = 0;
        if (stop == LLRHI::PACKET_DONE)
        {
            Program& program = *current_program();     // h.program (packet_head checked it)
            DrawRun& run = sRun;
            for (; drawn < p.count; ++drawn)
            {
                const LLRHI::PacketEntry& e = p.entries[drawn];
                PacketAnswer a;
                stop = packet_entry(h, e, a);
                if (stop != LLRHI::PACKET_DONE)
                {
                    break;
                }
                if (run.used >= run.capacity)
                {
                    stop = LLRHI::PACKET_RUN;
                    break;
                }
                if (!packet_push(program, h, e, a))
                {
                    stop = LLRHI::PACKET_ROW;
                    break;
                }
            }
        }
        if (drawn < p.count)
        {
            ++c.stops[stop < LLRHI::PACKET_STOP_COUNT ? stop : LLRHI::PACKET_REFUSED];
        }
        p.stop = drawn < p.count ? stop : (U32)LLRHI::PACKET_DONE;
        c.drawn += drawn;
        c.ticks += __rdtsc() - start;
        return drawn;
    }

    std::string packet_report(U32 frames)
    {
        const PacketCounts& c = sCounts;
        const LLRHI::PacketCensus& n = LLRHI::sPacketCensus;
        if (!frames || (!c.calls && !c.records && !c.heads && !n.loops))
        {
            return std::string();
        }
        std::ostringstream out;
        out << ", packets [" << (LLRHI::sPacketsVerify ? (LLRHI::sPacketsAuto ? "auto verify" : "verify")
                                 : LLRHI::sPackets ? "on" : LLRHI::sPacketsCensus ? "census" : "off");
        U64 stopped = 0;
        for (U64 s : c.stops)
        {
            stopped += s;
        }
        out << ", calls " << c.calls / frames << " drawn " << c.drawn / frames << " stopped " << stopped / frames << " [";
        for (U32 i = LLRHI::PACKET_REFUSED; i < LLRHI::PACKET_STOP_COUNT; ++i)
        {
            out << (i > LLRHI::PACKET_REFUSED ? ", " : "") << sStopNames[i] << " " << c.stops[i] / frames;
        }
        U32 live = 0;
        for (const PacketPart& part : sPacketParts)
        {
            live += part.live ? 1 : 0;
        }
        const F64 ticks_per_ns = perf_ticks_per_ns();
        out << "] records " << c.records / frames << " (refused " << c.record_refused / frames << " unpublished "
            << c.record_unpublished / frames << " patches full " << c.record_patches_full << ") parts " << live << " patches "
            << sPatches.size() << " textures " << c.textures / frames << " own matrices " << c.own_matrices / frames << " texture left " << c.texture_left << " ns/entry "
            << (c.drawn && ticks_per_ns > 0.0 ? (U64)((F64)c.ticks / ticks_per_ns / (F64)c.drawn) : 0);
        if (c.heads || c.checked)
        {
            out << " (verify heads " << c.heads_ok / frames << "/" << c.heads / frames << " checked " << c.checked / frames << " would take "
                << c.would_take / frames << " unrecorded " << c.unrecorded / frames << " would stop [";
            for (U32 i = LLRHI::PACKET_RUN; i < LLRHI::PACKET_STOP_COUNT; ++i)
            {
                out << (i > LLRHI::PACKET_RUN ? ", " : "") << sStopNames[i] << " " << c.would_stop[i] / frames;
            }
            out << "] mismatches " << c.mismatches << ")";
        }
        if (n.loops)
        {
            const F64 f = n.batches ? (F64)n.eligible / (F64)n.batches : 0.0;
            out << " census [loops " << n.loops / frames << " batches " << n.batches / frames << " eligible " << n.eligible / frames
                << " (f " << (U32)(f * 100.0 + 0.5) << "%) no template " << n.no_template / frames << " avatar " << n.avatar / frames
                << " texture matrix " << n.texture_matrix / frames << " model matrix " << n.model_matrix / frames << " unnamed "
                << n.unnamed / frames << " mapped " << n.mapped / frames << " region matrix " << n.region_matrix / frames << " matrix cuts "
                << n.matrix_cuts / frames << " matrix sets " << n.matrix_sets / frames << " (full " << n.matrix_sets_full / frames
                << ") value sets " << n.value_sets / frames;
            // P1c: the batches with an object's matrix, by why and how they move.
            const U64 taken = n.eligible > n.short_stretch ? n.eligible - n.short_stretch : 0;
            out << " object matrix " << n.object_matrix / frames << " short stretch " << n.short_stretch / frames << " short taken "
                << n.short_taken / frames << " (taken "
                << (n.batches ? (U32)((F64)taken * 100.0 / (F64)n.batches + 0.5) : 0) << "%) objects [" << n.objects / frames
                << ": animated child " << n.animated_child / frames << " active root " << n.active_root / frames << " active child "
                << n.active_child / frames << " attachment " << n.attachment / frames << " flexible " << n.flexible / frames << " animesh "
                << n.animesh / frames << " omega " << n.omega / frames << " unknown " << n.kind_unknown / frames << "; moving "
                << n.moving / frames << " still " << n.still / frames << " returnable " << n.returnable / frames << "]"
                << " loop ns/batch "
                << (n.loop_batches && ticks_per_ns > 0.0 ? (U64)((F64)n.loop_ticks / ticks_per_ns / (F64)n.loop_batches) : 0) << "]";
        }
        if (n.eye_attachment)
        {
            out << " eye attachments " << n.eye_attachment / frames;
        }
        out << "]";
        return out.str();
    }

    void packet_reset_counters()
    {
        sCounts = PacketCounts();
        LLRHI::sPacketCensus = LLRHI::PacketCensus();
    }
} // namespace llvkgl

#endif // LL_GL_FUNC_POINTER

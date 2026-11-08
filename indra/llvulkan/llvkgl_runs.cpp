/**
 * @file llvkgl_runs.cpp
 * @brief Draw runs, draw templates and what the probe faces' GPU list asks of the translation (research/46 G4b, G4c, G5, research/91).
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
#include "llrhiresource.h"     // research/197 stage 1b: LLRHI::sVBNative

#if LL_GL_FUNC_POINTER

namespace llvkgl
{
    // ------------------------------------------------------------------
    // Draw runs (research/46 G4b). Consecutive draws of a marked program
    // (Program::draw_runs) that the full path or the pass cache would record
    // one after the other with nothing in between (same pipeline, set and
    // dynamic state, so no command between them), and whose vertex and index
    // buffers are in the attribute pool (llvkattribpool), go out as one
    // vkCmdDrawIndexedIndirectCount: the run's first draw records the pool's
    // vertex and index buffers, the push constants and the indirect draw, with
    // the draw count in a word of the frame's ring; each later draw only
    // writes its VkDrawIndexedIndirectCommand and the count. The GPU reads
    // both when the frame is submitted, after the last draw was added.
    //
    // A draw continues the open run only if nothing was recorded since the
    // run's last draw (the vkCmd counters of llvkdispatch.h, which every
    // command of the frame goes through, did not move; no new rendering
    // scope, frame or binding reset), in the same pool chunk, with room left.
    // Draws of an indirect draw execute in order, so the run keeps the order
    // of the draws; its draws read the same vertex bytes (copied as GL has
    // them) and indices (as 32-bit values). What each draw pushed reaches the
    // shaders as before: with draw_rows the words go into a row of the frame's
    // table named by the draw's first instance; without, every draw of the
    // run must push the same words.
    DrawRun sRun;
    U32 sDispatchResets = 0;        // LLVKDispatch::sCounts were reset (perfReport)
    U32 sRunsOpened = 0, sRunDraws = 0, sRunRows = 0, sRunRowsFull = 0, sRunFast = 0;
    U32 sRunRefused[RUN_REFUSAL_COUNT] = {};
    U32 sGeneratedExecutes = 0;     // vkCmdExecuteGeneratedCommandsEXT recorded (research/50 step 4)

    // ------------------------------------------------------------------
    // Draw templates (research/46 G4c). A batch that joined a run leaves a
    // template behind: its program, vertex format and pool placement (a
    // ticket of llvkattribpool), named by an id the backend writes into a slot
    // the batch owns (DirectDraw::draw_template: LLDrawInfo or LLVertexBuffer::
    // mRHIDrawTemplate). In later frames the viewer offers the batch by that
    // id before laying out its attributes (LLRHI::sDrawTemplate). If the open
    // run may take a next draw (run_unchanged, run_pass_hit: nothing changed
    // or was recorded since its last draw) and the template's program,
    // format, buffers and placement still hold, the draw's push constants are
    // gathered as for any draw (gather_slots: textures, matrix, palette and
    // material rows from the current state) and only its row and indirect
    // command are written. Everything a template does not name comes from
    // the draw or the current state exactly as on the usual way, and the
    // checks are the ones run_continue makes, so the result is the same.

    // Never destroyed: slots can still be released while static objects go away at exit.
    std::vector<DrawTemplate>& sTemplates = *new std::vector<DrawTemplate>();
    std::vector<U32>& sTemplateFree = *new std::vector<U32>();
    U32 sTemplateDraws = 0, sTemplateRefused = 0, sTemplatesMade = 0, sTemplatesFull = 0;

namespace
{
    // The last row written this frame: consecutive draws pushing the same words share it.
    U32 sRunRowFrame = U32_MAX;
    U32 sRunRowBase = 0;
    U32 sRunRowWords = 0;
    U32 sRunRow[LLVKBindless::MAX_PUSHED_SLOTS] = {};
    U32 sRunRowVec4s = 0;           // taken from the table this frame (sRunRowFrame)
    // Rows a frame may take from the table (it is shared with the palettes, the
    // lights and the materials, whose draws are skipped when it is full).
    constexpr U32 RUN_ROW_VEC4S_PER_FRAME = 1u << 18;
    // RUN_MIN_CAPACITY, RUN_MAX_CAPACITY: llvkgl_drawpath.h (a draw list's segments open runs too).
    const char* const sRunRefusalNames[RUN_REFUSAL_COUNT] = { "unmarked", "not direct", "shape", "unsized", "slots", "pool", "inputs", "ring" };
    std::vector<VkBuffer> tRunBuffers;
    std::vector<VkDeviceSize> tRunOffsets;
    constexpr U32 MAX_DRAW_TEMPLATES = 1u << 18;
} // namespace

    // Commands recorded so far (into any of the frame's command buffers);
    // descriptor writes and allocations are not commands.
    U64 run_commands()
    {
        // All counters, then the two that are not commands taken off: the same
        // sum without a branch per counter (asked several times per run draw).
        U64 sum = 0;
        for (U32 i = 0; i < LLVKDispatch::COUNTER_COUNT; ++i)
        {
            sum += LLVKDispatch::sCounts[i];
        }
        return sum - LLVKDispatch::sCounts[LLVKDispatch::C_UpdateDescriptorSets] -
               LLVKDispatch::sCounts[LLVKDispatch::C_AllocateDescriptorSets];
    }

    // The draw's row of the open run (a draw_rows program): the words it
    // would push (tSlots without the rows word) in the frame's table, shared
    // with the row before when they are the same. False: the frame's share of
    // the table is spent (the draw goes the usual way).
    bool run_row(U32& first_instance)
    {
        const U32 words = tSlotCount - 1;
        if (sRunRowFrame != sFrame)
        {
            sRunRowFrame = sFrame;
            sRunRowVec4s = 0;
            sRunRowWords = 0;
        }
        if (sRunRowWords == words && same_words(sRunRow, tSlots, words))
        {
            first_instance = sRunRowBase;
            return true;
        }
        const U32 vec4s = (words + 3) / 4;
        U32 base = 0;
        F32* row = sRunRowVec4s + vec4s <= RUN_ROW_VEC4S_PER_FRAME ? LLVKBindless::paletteRows(vec4s, base) : nullptr;
        if (!row)
        {
            ++sRunRowsFull;
            return false;
        }
        sRunRowVec4s += vec4s;
        U32 padded[LLVKBindless::MAX_PUSHED_SLOTS + 3] = {};
        memcpy(padded, tSlots, words * 4);
        memcpy(row, padded, (size_t)vec4s * 16);
        memcpy(sRunRow, tSlots, words * 4);
        sRunRowWords = words;
        sRunRowBase = base;
        first_instance = base;
        ++sRunRows;
        return true;
    }

    // One more draw of the open run (room checked by the caller): its
    // indirect command, and the count the GPU reads at submit.
    void run_push(U32 index_count, U32 first_index, S32 vertex_offset, U32 first_instance)
    {
        DrawRun& run = sRun;
        VkDrawIndexedIndirectCommand& c = run.args[run.used++];
        c.indexCount = index_count;
        c.instanceCount = 1;
        c.firstIndex = first_index;
        c.vertexOffset = vertex_offset;
        c.firstInstance = first_instance;
        *run.count = run.used;
    }

namespace
{
    // A draw of `program` joined a run (run_draw): the batch's template, made
    // or brought up to date.
    void template_register(const Program& program, const LLRHI::DirectDraw& d, const LLVKAttribPool::Ticket& ticket, U64 format)
    {
        if (!LLRHI::sDrawTemplates || !d.draw_template || !ticket.serial)
        {
            return;
        }
        U32 id = *d.draw_template;
        if (!id || id > sTemplates.size() || sTemplates[id - 1].owner != d.draw_template)
        {
            if (!sTemplateFree.empty())
            {
                id = sTemplateFree.back();
                sTemplateFree.pop_back();
            }
            else if (sTemplates.size() < MAX_DRAW_TEMPLATES)
            {
                sTemplates.emplace_back();
                id = (U32)sTemplates.size();
            }
            else
            {
                ++sTemplatesFull;
                return;
            }
            *d.draw_template = id;
            ++sTemplatesMade;
            packet_part_reset(id);  // research/180 stage 4: a record of the id's last owner is not this batch's
        }
        DrawTemplate& t = sTemplates[id - 1];
        t.owner = d.draw_template;
        t.program = &program;
        t.program_changes = sProgramChanges;
        t.format = format;
        t.ticket = ticket;
        t.vertex_buffer = d.vertex_buffer;
        t.index_buffer = d.index_buffer;
        t.index_type = d.index_type;
        t.buffer_vertices = d.buffer_vertices;
        t.buffer_indices = d.buffer_indices;
    }
} // namespace

    // The open run may take a next draw of `program` as far as the state goes:
    // nothing that the pass cache keys on changed since the run's last draw
    // (the same counters, compared for equality only), and nothing was
    // recorded since.
    bool run_unchanged(Program& program)
    {
        DrawRun& run = sRun;
        // A run of OIT split draws continues only the usual way (oit_bind_set pushes each draw's priority).
        if (!run.open || !run.fast || !LLRHI::sDrawRuns || run.program != &program || run.oit_mode != tOITDraw ||
            suppress_translation() || depth_only_pass() || sActiveQuery || census_tag())
        {
            return false;
        }
        LLVKMirror& mirror = LLVKMirror::instance();
        if (run.frame != sFrame || run.program_name != sCurrentProgram || run.program_changes != sProgramChanges ||
            run.pipeline_state != sPipelineState.load(std::memory_order_relaxed) || run.fixed_state != sFixedState ||
            run.target_state != sTargetState.load(std::memory_order_relaxed) || run.framebuffer != sState.draw_framebuffer ||
            run.attrib_constants != sAttribConstants || run.layouts != sLayoutState || run.record_epoch != sRecordEpoch ||
            run.scope_epoch != mirror.mScopeEpoch || !mirror.isRendering() || run.bindless_moves != LLVKBindless::moves() ||
            run.uniform_version != program.uniform_version || run.resets != sDispatchResets || run.commands != run_commands())
        {
            return false;
        }
        if (!program.sampler_slots.empty() && (run.texture_state != sTextureState || run.texture_deletes != sTextureDeletes))
        {
            return false;
        }
        const PassVersions v = pass_versions(program);
        return v.unit_version == run.versions.unit_version && v.unit_bindings == run.versions.unit_bindings &&
               v.uniform_blocks == run.versions.uniform_blocks && v.buffer_deletes == run.versions.buffer_deletes;
    }

    // After run_unchanged and a draw of the run's vertex format: the usual way
    // would find a plain pass cache hit (same target: its framebuffer and
    // attachments did not change) and bind nothing: its pipeline and set are
    // bound, and the dynamic state is as the run's last draw left it. Asked
    // once per run state (run.pass_ok): everything the answer depends on is in
    // what run_unchanged compares, or changes only by recording (which ends
    // the run's state) or by a draw the usual way (which clears pass_ok).
    bool run_pass_hit(Program& program)
    {
        DrawRun& run = sRun;
        if (run.pass_ok)
        {
            return true;
        }
        if (pass_hit(program, run.target, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, run.inputs) != PASS_HIT)
        {
            return false;
        }
        const PassCache& p = *program.pass;
        const Recorded& r = sRecorded;
        // No set (the shared layout): only the table must be bound.
        if (r.pipeline != p.pipeline || (p.set ? r.set != p.set || !r.table_bound : !table_at_set0(r)) ||
            (p.has_dynamic && r.dynamic_offset != p.dynamic_offset))
        {
            return false;
        }
        run.pass_ok = true;
        return true;
    }

    // perfReport's breakdown of sRunRefused, per frame.
    std::string run_refusals(U32 frames)
    {
        std::ostringstream out;
        for (U32 i = 0; i < RUN_REFUSAL_COUNT; ++i)
        {
            out << (i ? ", " : "") << sRunRefusalNames[i] << " " << sRunRefused[i] / frames;
        }
        return out.str();
    }

    U64 direct_attributes_hash(const LLRHI::DirectDraw& d)
    {
        U64 format = LLRHI::FNV1A_BASIS_SHORT;
        for (U32 k = 0; k < d.attribute_count; ++k)
        {
            const LLRHI::DirectDraw::Attribute& a = d.attributes[k];
            format = LLRHI::fnv1aMix(format, ((U64)a.location << 56) ^ ((U64)a.size << 48) ^ ((U64)a.type << 32) ^
                                             ((U64)a.stride << 2) ^ (a.normalized ? 2u : 0u) ^ (a.integer ? 1u : 0u));
            if (LLRHI::sVBSourceStreams)
            {
                // research/293 branch 1: draws of one format bind the same streams at the same offsets (the first
                // draw's bindings are reused for the later ones)
                format = LLRHI::fnv1aMix(format, ((U64)a.streamKey() << 32) ^ a.streamShift());
            }
        }
        return format;
    }

    // A direct draw's vertex format: its attributes without their offsets
    // (which change with the buffer), as direct_inputs keys its inputs.
    U64 run_format(const LLRHI::DirectDraw& d)
    {
        return direct_attributes_hash(d) ^ d.attribute_count;
    }

    // LLRHI::sDrawTemplateReleased: the slot holding the template goes away.
    void draw_template_released(U32 id, const U32* slot)
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        if (id && id <= sTemplates.size() && sTemplates[id - 1].owner == slot)
        {
            sTemplates[id - 1] = DrawTemplate();
            sTemplateFree.push_back(id);
            packet_part_reset(id);  // research/180 stage 4: its record goes with it
        }
    }

    // Records the draw (tDirect, whose pipeline, set and dynamic state are
    // bound, its push constants in tSlots, drawn into target) as part of a
    // run: the open one if it may continue it, else (unless join_only) a new
    // one. False: record it the usual way (record_draw_commands); nothing was
    // recorded.
    bool run_draw(VkCommandBuffer cmd, Program& program, const ResolvedTarget& target, VkPrimitiveTopology prim, GLsizei count,
                  GLsizei instances, const IndexInfo& index, bool join_only)
    {
        // A draw the alpha census counts (LLRHI::sCensusTag) goes the usual way, with its own query.
        if (!LLRHI::sDrawRuns || LLVKDevice::instance().checkpointsEnabled() || (!tDirect && !program.draw_runs) || census_tag())
        {
            return false;
        }
        auto refuse = [join_only](RunRefusal why)
        {
            if (!join_only)
            {
                ++sRunRefused[why];
            }
            return false;
        };
        if (!program.draw_runs)
        {
            return refuse(RUN_UNMARKED);
        }
        if (!tDirect)
        {
            return refuse(RUN_NOT_DIRECT);
        }
        if (!index.indexed || tFanIndexBuffer || prim != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST || instances != 1 || count <= 0 ||
            tDirect->table_base != LLRHI::DirectDraw::NO_TABLE)
        {
            return refuse(RUN_SHAPE);
        }
        if (!tDirect->buffer_vertices)
        {
            return refuse(RUN_UNSIZED);
        }
        const LLRHI::DirectDraw& d = *tDirect;
        if (program.draw_rows && (!tSlotCount || tSlotCount != program.draw_row_words + 1))
        {
            return refuse(RUN_SLOTS);
        }
        // Counted before the pool may record its copies (into the upload commands,
        // which run before all of the frame's draws).
        const U64 before = run_commands();
        LLVKAttribPool::Placement place;
        LLVKAttribPool::Ticket ticket;
        if (!LLVKAttribPool::place(d, place, ticket))
        {
            return refuse(RUN_POOL);
        }
        LLVKMirror& mirror = LLVKMirror::instance();
        DrawRun& run = sRun;
        const bool join = run.open && run.frame == sFrame && run.cmd == cmd && run.program == &program &&
                          run.program_changes == sProgramChanges && run.scope_epoch == mirror.mScopeEpoch &&
                          run.record_epoch == sRecordEpoch && run.resets == sDispatchResets && run.commands == before &&
                          run.attrib_constants == sAttribConstants &&
                          run.chunk == place.chunk && run.used < run.capacity && run.rows == program.draw_rows &&
                          (run.rows || (run.slot_count == tSlotCount && same_words(run.slots, tSlots, tSlotCount)));
        if (!join && join_only)
        {
            return false;
        }

        // The draw's row: the words it would push (without the rows word).
        U32 first_instance = 0;
        if (program.draw_rows && !run_row(first_instance))
        {
            return false;
        }

        bool census_opened = false;     // research/293 branch 0: this draw opened the run (counting only)
        if (!join)
        {
            // How long this program's runs get: the next one reserves for that.
            if (run.open && run.program == &program && run.program_changes == sProgramChanges)
            {
                if (run.used == run.capacity)
                {
                    program.run_capacity = llmin(program.run_capacity * 2, RUN_MAX_CAPACITY);
                }
                else if (run.used * 4 < run.capacity)
                {
                    program.run_capacity = llmax(program.run_capacity / 2, RUN_MIN_CAPACITY);
                }
            }
            run.open = false;
            const U32 capacity = llclamp(program.run_capacity, RUN_MIN_CAPACITY, RUN_MAX_CAPACITY);
            // The vertex inputs: each attribute from its stream in the pool, the
            // constants as direct_inputs found them.
            const DirectInputs& f = tDirectInputs;
            const size_t n = f.source.size();
            if (tVertexBuffers.size() != n)
            {
                return refuse(RUN_INPUTS);
            }
            tRunBuffers.resize(n);
            tRunOffsets.resize(n);
            for (size_t i = 0; i < n; ++i)
            {
                const S32 k = f.source[i];
                if (k >= 0)
                {
                    const LLRHI::DirectDraw::Attribute& a = d.attributes[k];
                    tRunBuffers[i] = LLVKAttribPool::streamBuffer(place.chunk, a.streamKey(), a.stride);
                    tRunOffsets[i] = a.streamShift();   // research/293 branch 1: inside the stream (TEXTURE_INDEX: 12)
                    if (!tRunBuffers[i])
                    {
                        return refuse(RUN_INPUTS);
                    }
                }
                else
                {
                    tRunBuffers[i] = tVertexBuffers[i];
                    tRunOffsets[i] = tVertexOffsets[i];
                }
            }
            const VkBuffer index_buffer = LLVKAttribPool::indexBuffer(place.chunk);
            const PFN_vkCmdDrawIndexedIndirectCount draw_indirect_count = LLVKAttribPool::drawIndexedIndirectCount();
            VkBuffer ring = VK_NULL_HANDLE;
            VkDeviceSize offset = 0;
            void* data = nullptr;
            if (!index_buffer || !draw_indirect_count ||
                !mirror.allocate(16 + (VkDeviceSize)capacity * sizeof(VkDrawIndexedIndirectCommand), 16, ring, offset, data))
            {
                return refuse(RUN_RING);
            }

            // Push constants: the draw's, the rows word saying "read the rows".
            Recorded& r = sRecorded;
            U32 pushed[LLVKBindless::MAX_PUSHED_SLOTS];
            memcpy(pushed, tSlots, tSlotCount * 4);
            if (program.draw_rows)
            {
                pushed[tSlotCount - 1] = 0;
            }
            if (tSlotCount && (tSlotCount > r.slot_count || !same_words(pushed, r.slots, tSlotCount)))
            {
                const VkPushConstantRange range = LLVKBindless::pushConstantRange();
                vkCmdPushConstants(cmd, program.layout, range.stageFlags, 0, tSlotCount * 4, pushed);
                memcpy(r.slots, pushed, tSlotCount * 4);
                r.slot_count = llmax(r.slot_count, tSlotCount);
            }
            if (!same_elements(tRunBuffers, r.vertex_buffers) || !same_elements(tRunOffsets, r.vertex_offsets))
            {
                vkCmdBindVertexBuffers(cmd, 0, (U32)n, tRunBuffers.data(), tRunOffsets.data());
                r.vertex_buffers = tRunBuffers;
                r.vertex_offsets = tRunOffsets;
            }
            if (index_buffer != r.index_buffer || r.index_type != VK_INDEX_TYPE_UINT32 || r.index_offset != 0)
            {
                vkCmdBindIndexBuffer(cmd, index_buffer, 0, VK_INDEX_TYPE_UINT32);
                r.index_buffer = index_buffer;
                r.index_type = VK_INDEX_TYPE_UINT32;
                r.index_offset = 0;
            }
            U32* count_word = (U32*)data;
            *count_word = 0;
            LLVKRecorder::value(draw_indirect_count, cmd, ring, (VkDeviceSize)(offset + 16), ring, (VkDeviceSize)offset, (uint32_t)capacity,
                                (uint32_t)sizeof(VkDrawIndexedIndirectCommand));
            run.open = true;
            run.frame = sFrame;
            run.cmd = cmd;
            run.program = &program;
            run.program_changes = sProgramChanges;
            run.scope_epoch = mirror.mScopeEpoch;
            run.record_epoch = sRecordEpoch;
            run.resets = sDispatchResets;
            run.chunk = place.chunk;
            run.rows = program.draw_rows;
            memcpy(run.slots, tSlots, tSlotCount * 4);
            run.slot_count = tSlotCount;
            run.args = (VkDrawIndexedIndirectCommand*)((U8*)data + 16);
            run.count = count_word;
            run.capacity = capacity;
            run.used = 0;
            // What device generated commands would bind after this run (research/50 step 4).
            run.generated = LLRHI::sGPUDrivenDGC && LLRHI::sGPUDriven && program.shared_layout && !tOITDraw &&
                            LLVKDevice::instance().generatedCommands() != nullptr;
            if (run.generated)
            {
                run.vertex_inputs = f.inputs;
                run.buffers = tRunBuffers;
                run.offsets = tRunOffsets;
                run.stream_location.resize(n);
                run.stream_stride.resize(n);
                for (size_t i = 0; i < n; ++i)
                {
                    const S32 k = f.source[i];
                    run.stream_location[i] = k >= 0 ? (S32)d.attributes[k].location : -1;
                    run.stream_stride[i] = k >= 0 ? d.attributes[k].stride : 0;
                }
            }
            ++sRunsOpened;
            census_opened = true;
        }

        run_push((U32)count, place.first_index + d.first_index, place.vertex_offset, first_instance);
        // Everything recorded so far, the pool's copies included: the next draw
        // continues the run only if nothing comes after this one.
        run.commands = run_commands();
        // The state this draw was recorded with: a next draw of the program
        // that finds all of it unchanged would be a pass cache hit recording
        // nothing before its draw, so it may go straight to the run (run_continue).
        run.fast = sPassCacheEnabled && !direct_writes_block(program);
        run.oit_mode = tOITDraw;
        run.target = target;
        run.program_name = sCurrentProgram;
        run.pipeline_state = sPipelineState.load(std::memory_order_relaxed);
        run.fixed_state = sFixedState;
        run.target_state = sTargetState.load(std::memory_order_relaxed);
        run.framebuffer = sState.draw_framebuffer;
        run.attrib_constants = sAttribConstants;
        run.layouts = sLayoutState;
        run.bindless_moves = LLVKBindless::moves();
        run.uniform_version = program.uniform_version;
        run.texture_state = sTextureState;
        run.texture_deletes = sTextureDeletes;
        run.versions = pass_versions(program);
        run.format = run_format(d);
        if (!join_only)
        {
            run.inputs = tDirectInputs.signature;   // this draw's (direct_inputs ran for it)
            // Recorded the usual way: whether the next draw would be a plain
            // pass cache hit is asked again (run_pass_hit).
            run.pass_ok = false;
        }
        ++sRunDraws;
        if (LLRHIVBCensus::sOn)     // research/293 branch 0: counting only
        {
            LLRHIVBCensus::runDraw(census_owner(d.vertex_buffer), census_opened);
        }
        if (LLRHI::sAvatarStats && program.palette_columns == 3)
        {
            ++sAvRiggedRun;     // research/56
        }
        // The batch's template (research/46 G4c): later frames may offer it by that.
        template_register(program, d, ticket, run.format);
        if (LLRHI::tPacketNote)
        {
            packet_note(program, d);    // research/180 stage 4: the batch's record beside it
        }
        return true;
    }

    // Research/197 stage 1b: a template made by number goes on only with the
    // draw's number; one made by name, with numbers on (not verifying), is
    // made again by number (else its pair would stay in the pool twice).
    static bool template_numbered_as(const LLRHI::DirectDraw& d, const LLVKAttribPool::Ticket& ticket)
    {
        if (ticket.vb)
        {
            return d.vb == ticket.vb;
        }
        return !d.vb || !LLRHI::sVBNative || LLRHI::sVBHandlesVerify;
    }

    // LLRHI::sDrawTemplate (research/46 G4c): the batch whose template the draw
    // names, as the next draw of the open run. The draw brings what changes per
    // draw (buffers, index range, textures, matrices) but no attributes: the
    // template has the format and the placement. False: nothing was recorded
    // (what gather_slots did is what the usual way does again).
    bool draw_template(const LLRHI::DirectDraw& d)
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        PerfScope total(PERF_DRAW);
        const U32 id = d.draw_template ? *d.draw_template : 0;
        DrawRun& run = sRun;
        if (!LLRHI::sDrawRuns || !LLRHI::sDrawTemplates || !id || id > sTemplates.size() || !run.open || sCapture.on)
        {
            return false;   // a capture needs the draw's attributes (direct_draw)
        }
        const DrawTemplate& t = sTemplates[id - 1];
        Program* program = current_program();
        if (t.owner != d.draw_template || !program || program != run.program)
        {
            return false;   // not a draw of the open run (another pass or program): not a refusal
        }
        if (t.program != program || t.program_changes != sProgramChanges || t.format != run.format || run.used >= run.capacity ||
            run.rows != program->draw_rows)
        {
            ++sTemplateRefused;
            return false;
        }
        // The draw as run_continue takes it, on the buffers the template was made from.
        if (d.mode != GL_TRIANGLES || d.instance_count > 1 || d.table_base != LLRHI::DirectDraw::NO_TABLE || !d.index_count ||
            d.vertex_buffer != t.vertex_buffer || d.index_buffer != t.index_buffer || d.index_type != t.index_type ||
            d.buffer_vertices != t.buffer_vertices || d.buffer_indices != t.buffer_indices ||
            (U64)d.first_index + d.index_count > d.buffer_indices || !template_numbered_as(d, t.ticket))
        {
            ++sTemplateRefused;
            return false;
        }
        // As direct_draw: units sampled through descriptor images take their texture from the GL binding only.
        for (GLint unit : program->unit_values)
        {
            if (unit >= 0 && (U32)unit < d.texture_count && d.textures[unit] != LLRHI::DirectDraw::KEEP_UNIT)
            {
                ++sTemplateRefused;
                return false;
            }
        }
        if (!run_unchanged(*program) || !run_pass_hit(*program))
        {
            ++sTemplateRefused;
            return false;
        }
        LLVKAttribPool::Placement place;
        if (!LLVKAttribPool::redeem(sTemplates[id - 1].ticket, place) || place.chunk != run.chunk)
        {
            ++sTemplateRefused;
            return false;
        }
        // The draw's push constants, and its own textures readable as they are.
        const ResolvedTarget& target = run.target;      // read only (gather_slots)
        bool ok = false;
        U32 first_instance = 0;
        {
            const DirectScope direct(&d);
            {
                PerfScope scope(PERF_DESCRIPTORS);
                gather_slots(*program, target);
                ok = !tPaletteFull;
                for (const Texture* tex : tBindlessSampled)
                {
                    ok &= tex->layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                }
            }
            // Still nothing recorded after the run's last draw, and words the run can take.
            ok = ok && run.commands == run_commands();
            if (ok)
            {
                ok = program->draw_rows ? tSlotCount && tSlotCount == program->draw_row_words + 1
                                        : run.slot_count == tSlotCount && same_words(run.slots, tSlots, tSlotCount);
            }
            if (ok && program->draw_rows)
            {
                PerfScope scope(PERF_RECORD);
                ok = run_row(first_instance);
            }
        }
        if (!ok)
        {
            ++sTemplateRefused;
            return false;
        }
        run_push(d.index_count, place.first_index + d.first_index, place.vertex_offset, first_instance);
        if (LLRHI::tPacketNote)
        {
            packet_note(*program, d);   // research/180 stage 4: the batch's record beside its template
        }
        ++program->generic_draws;
        ++sPassHits;
        ++sRunDraws;
        if (LLRHIVBCensus::sOn)     // research/293 branch 0: counting only
        {
            LLRHIVBCensus::runDraw(census_owner(d.vertex_buffer), false);
        }
        ++sRunFast;
        ++sTemplateDraws;
        ++sDirectDraws;
        if (LLRHI::sAvatarStats && program->palette_columns == 3)
        {
            // Research/56: a rigged draw, in a run, by its template.
            ++sAvRiggedDraws;
            ++sAvRiggedRun;
            ++sAvRiggedTemplate;
        }
        return true;
    }

    // A direct draw right after a draw of an open run (LLRHI::sDirectDraw):
    // when nothing that the pass cache keys on changed since (the same
    // counters, compared for equality only), nothing was recorded since, and
    // the draw is of the run's kind, the full path would find a pass cache hit
    // and record nothing before the draw itself. Then only what changes per
    // draw is done (push constants into tSlots as gather_slots makes them, the
    // pool placement, the row) and the draw joins the run. False: the draw
    // goes the usual way (nothing was recorded; what gather_slots did is what
    // the usual way does again).
    bool run_continue(Program& program, const LLRHI::DirectDraw& d)
    {
        PerfScope total(PERF_DRAW);
        DrawRun& run = sRun;
        if (!run_unchanged(program))
        {
            return false;
        }
        if (d.mode != GL_TRIANGLES || d.instance_count > 1 || d.table_base != LLRHI::DirectDraw::NO_TABLE || !d.index_count ||
            !d.buffer_vertices || !d.attributes || (d.index_type != GL_UNSIGNED_SHORT && d.index_type != GL_UNSIGNED_INT) ||
            run_format(d) != run.format)
        {
            return false;
        }
        if (!run_pass_hit(program))
        {
            return false;
        }
        // From here as the pass cache hit would: the draw's push constants, and
        // its own textures readable as they are.
        ResolvedTarget target = run.target;
        bool ok = false;
        {
            const DirectScope direct(&d);
            {
                PerfScope scope(PERF_DESCRIPTORS);
                gather_slots(program, target);
                ok = !tPaletteFull;
                for (const Texture* tex : tBindlessSampled)
                {
                    ok &= tex->layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                }
            }
            if (ok)
            {
                PerfScope scope(PERF_RECORD);
                IndexInfo index;
                index.indexed = true;
                index.type = d.index_type;
                index.offset = (size_t)d.first_index * (d.index_type == GL_UNSIGNED_INT ? 4 : 2);
                ok = run_draw(run.cmd, program, target, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, (GLsizei)d.index_count, 1, index, true);
            }
        }
        if (ok)
        {
            ++program.generic_draws;
            ++sPassHits;
            ++sRunFast;
            if (LLRHI::sAvatarStats && program.palette_columns == 3)
            {
                ++sAvRiggedStraight;    // research/56 (run_draw counted it in the run)
            }
        }
        return ok;
    }
} // namespace llvkgl

using namespace llvkgl;

// ----------------------------------------------------------------------------
// LLVKGLProbe: what the probe faces' GPU list (llvkgpulist.cpp, research/46 G5)
// asks of the translation. Each piece does what the draw path does for a
// direct draw, so the list's draws get the words, slots and matrices the
// draws they stand for would have pushed.

namespace
{
    // The open run, as run_draw and run_continue require it to take one more
    // draw: same frame, scope, recording and program, nothing recorded since.
    bool probe_run_open()
    {
        const DrawRun& run = sRun;
        LLVKMirror& mirror = LLVKMirror::instance();
        return LLRHI::sDrawRuns && run.open && run.rows && run.frame == sFrame && run.program &&
               run.program == current_program() && run.program_changes == sProgramChanges &&
               run.scope_epoch == mirror.mScopeEpoch && run.record_epoch == sRecordEpoch && run.resets == sDispatchResets &&
               run.commands == run_commands() && mirror.isRendering() && !LLVKDevice::instance().checkpointsEnabled();
    }
}

bool LLVKGLProbe::lastRun(Run& out)
{
    std::lock_guard<LLVKTrackerMutex> lock(sLock);
    out = Run();
    if (!probe_run_open())
    {
        return false;
    }
    out.program = sRun.program;
    out.chunk = sRun.chunk;
    out.format = sRun.format;
    return true;
}

bool LLVKGLProbe::templateWords(Words& out)
{
    std::lock_guard<LLVKTrackerMutex> lock(sLock);
    out = Words();
    Program* program = current_program();
    if (!probe_run_open() || !program || !program->draw_rows || program->draw_row_words > Words::MAX ||
        program->bindless.size() > Words::MAX)
    {
        return false;
    }
    // gather_slots for a direct draw that names no texture and no matrices:
    // every sampler as its unit is bound, the program's own rows.
    LLRHI::DirectDraw none;
    none.texture_count = 0;
    ResolvedTarget target = sRun.target;
    {
        const DirectScope direct(&none);
        gather_slots(*program, target);
    }
    if (tPaletteFull || tSlotCount != program->draw_row_words + 1)
    {
        return false;
    }
    out.count = program->draw_row_words;
    memcpy(out.words, tSlots, (size_t)out.count * 4);
    out.samplers = (U32)program->bindless.size();
    for (U32 i = 0; i < out.samplers; ++i)
    {
        out.units[i] = program->bindless[i].unit;
    }
    out.matrix_word = program->matrix_mask ? out.samplers : U32_MAX;
    // gather_slots' order: samplers, matrix, palette, light, material, constants.
    if (program->material_vec4s)
    {
        out.material_word = out.samplers + (program->matrix_mask ? 1 : 0) + (program->palette_columns ? 1 : 0) +
                            (program->light_table ? 1 : 0);
        out.material_vec4s = program->material_vec4s;
    }
    out.consts_hash = consts_hash(*program);
    return (out.matrix_word == U32_MAX || out.matrix_word < out.count) &&
           (out.material_word == U32_MAX || out.material_word < out.count);
}

bool LLVKGLProbe::matrixRow(const LLRHI::DirectDraw& matrices, U32& row)
{
    std::lock_guard<LLVKTrackerMutex> lock(sLock);
    row = 0;
    Program* program = current_program();
    if (!program || !program->matrix_mask || matrices.matrix_id || matrices.texture_matrix0 ||
        !(matrices.modelview || matrices.modelview_projection || matrices.normal))
    {
        return false;
    }
    // matrix_row's direct branch: a row of its own (no matrix_id to share by).
    {
        const DirectScope direct(&matrices);
        row = matrix_row(*program);
    }
    // A full table hands out its last row again (shared): not this list's.
    return row < LLVKBindless::MATRIX_ROWS - 1;
}

U32 LLVKGLProbe::textureSlot(U32 name, bool& readable)
{
    std::lock_guard<LLVKTrackerMutex> lock(sLock);
    readable = true;
    U32 slot = LLVKBindless::WHITE_SLOT;
    const ResolvedTarget& target = sRun.target;
    // gather_slots, for a texture the direct draw names (from_draw).
    Texture* tex = name ? sTextures.find(name) : nullptr;
    if (tex && texture_slot_cached(*tex, target, LLRHI::sSlotCache))
    {
        // Already resolved for an earlier draw of this frame.
        readable = tex->layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        return tex->bindless_slot;
    }
    // Sampling a texture that is also being written: GL leaves this undefined.
    if (tex && tex->image && tex->defined_levels && !is_attachment_of(target, tex))
    {
        VkImageView view = texture_view(*tex);
        if (view)
        {
            if (tex->bindless_slot == U32_MAX)
            {
                tex->bindless_slot = LLVKBindless::acquire(name);
            }
            if (tex->bindless_slot != LLVKBindless::WHITE_SLOT)
            {
                // May move the texture to a new slot (its view changed mid-frame).
                tex->bindless_slot = LLVKBindless::use(tex->bindless_slot, name, view, sampler_for(tex, false), tex->view_serial);
            }
            slot = tex->bindless_slot;
            if (slot != LLVKBindless::WHITE_SLOT)
            {
                readable = tex->layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                tex->direct_frame = sFrame;
                tex->direct_version = tex->version;
                tex->direct_target = target.stamp;
                tex->direct_view = view;
            }
            else
            {
                tex->bindless_slot = U32_MAX;   // table full: try again next time
            }
        }
    }
    if (slot == LLVKBindless::WHITE_SLOT && sWhiteFrame != sFrame)
    {
        LLVKBindless::slotFor(0);   // the stand-in, once per frame
        sWhiteFrame = sFrame;
    }
    return slot;
}

void LLVKGLProbe::flushSlots()
{
    std::lock_guard<LLVKTrackerMutex> lock(sLock);
    LLVKBindless::flush();
}

U64 LLVKGLProbe::commands()
{
    std::lock_guard<LLVKTrackerMutex> lock(sLock);
    return run_commands();
}

void LLVKGLProbe::forgiveUploads(U64 before)
{
    std::lock_guard<LLVKTrackerMutex> lock(sLock);
    if (sRun.open && sRun.commands == before)
    {
        sRun.commands = run_commands();
    }
}

bool LLVKGLProbe::drawIndirect(const Run& run, VkBuffer buffer, VkDeviceSize offset, U32 count)
{
    std::lock_guard<LLVKTrackerMutex> lock(sLock);
    if (!count || !buffer || !probe_run_open() || run.program != sRun.program || run.chunk != sRun.chunk ||
        run.format != sRun.format)
    {
        return false;
    }
    // After the run's indirect draw, with its pipeline, set, push constants
    // (rows: read the table), pool streams and index buffer still bound.
    vkCmdDrawIndexedIndirect(sRun.cmd, buffer, offset, count, (uint32_t)sizeof(VkDrawIndexedIndirectCommand));
    // The indirect draw moved run_commands(): sRun.commands stays behind it, so
    // no later draw joins the run (a run's draws execute where the run opened,
    // which is before this draw). The next draw opens a new run.
    return true;
}

U64 LLVKGLProbe::drawFormat(const LLRHI::DirectDraw& draw)
{
    return run_format(draw);
}

U32 LLVKGLProbe::programChanges()
{
    std::lock_guard<LLVKTrackerMutex> lock(sLock);
    return sProgramChanges;
}

void LLVKGLProbe::beginCapture()
{
    std::lock_guard<LLVKTrackerMutex> lock(sLock);
    sCapture.on = true;
    sCapture.draws.clear();
}

void LLVKGLProbe::endCapture()
{
    std::lock_guard<LLVKTrackerMutex> lock(sLock);
    sCapture.on = false;
    sCapture.draws.clear();
}

U32 LLVKGLProbe::captureCount()
{
    std::lock_guard<LLVKTrackerMutex> lock(sLock);
    return (U32)sCapture.draws.size();
}

const LLVKGLProbe::Capture* LLVKGLProbe::capture(U32 index)
{
    std::lock_guard<LLVKTrackerMutex> lock(sLock);
    return index < sCapture.draws.size() ? &sCapture.draws[index] : nullptr;
}

namespace
{
    // The open run as the probe list's prepare found it, with the GL state
    // its pipeline was chosen for unchanged since its last draw. Not the
    // dynamic render state (sFixedState: cull, depth, stencil, offset,
    // viewport): the commands draw with what the run's last draw recorded,
    // nothing being recorded since, and the segment's batches had that state
    // (their capture's state hash is the first batch's). A pool's scoped state
    // (a double-sided GLTF material's LLGLDisable of culling) ends when its
    // body returns, before the segment's other batches go out.
    bool generated_run_open(const LLVKGLProbe::Run& run)
    {
        const DrawRun& r = sRun;
        return probe_run_open() && r.generated && run.program == r.program && run.chunk == r.chunk && run.format == r.format &&
               r.program_name == sCurrentProgram && r.pipeline_state == sPipelineState.load(std::memory_order_relaxed) &&
               r.target_state == sTargetState.load(std::memory_order_relaxed) &&
               r.framebuffer == sState.draw_framebuffer && r.attrib_constants == sAttribConstants &&
               !r.vertex_inputs.empty() && r.vertex_inputs.size() == r.buffers.size() && r.buffers.size() == r.offsets.size() &&
               r.stream_location.size() == r.buffers.size() && r.stream_stride.size() == r.buffers.size();
    }
}

bool LLVKGLProbe::generatedPipeline(const Run& run, VkPipeline& pipeline, Streams& streams)
{
    std::lock_guard<LLVKTrackerMutex> lock(sLock);
    pipeline = VK_NULL_HANDLE;
    streams = Streams();
    Program* program = current_program();
    if (!program || !generated_run_open(run) || program != sRun.program || !LLVKDevice::instance().generatedCommands())
    {
        return false;
    }
    const DrawRun& r = sRun;
    for (size_t i = 0; i < r.stream_location.size(); ++i)
    {
        if (r.stream_location[i] < 0)
        {
            continue;   // a constant: stays bound from the run
        }
        if (streams.count >= Streams::MAX)
        {
            return false;
        }
        streams.binding[streams.count] = (U32)i;
        streams.location[streams.count] = (U32)r.stream_location[i];
        streams.stride[streams.count] = r.stream_stride[i];
        ++streams.count;
    }
    if (!streams.count)
    {
        return false;
    }
    // The run's pipeline with dynamic vertex strides: the same program, inputs,
    // target and (unchanged since the run's last draw) GL state.
    pipeline = get_pipeline(r.program_name, *program, r.vertex_inputs, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, r.target.target, true);
    return pipeline != VK_NULL_HANDLE;
}

bool LLVKGLProbe::executeGenerated(const Run& run, VkPipeline pipeline, const VkGeneratedCommandsInfoEXT& info)
{
    std::lock_guard<LLVKTrackerMutex> lock(sLock);
    const LLVKDevice::GeneratedCommands* dgc = LLVKDevice::instance().generatedCommands();
    Program* program = current_program();
    if (!dgc || !pipeline || !program || program != sRun.program || !generated_run_open(run))
    {
        return false;
    }
    DrawRun& r = sRun;
    const VkCommandBuffer cmd = r.cmd;
    // The pipeline the commands draw with, and the run's vertex buffers with
    // their strides (the tokens rebind the pool streams per sequence; the
    // constants stay as bound here). Descriptor sets, push constants (rows:
    // read the table) and dynamic state stay as the run's first draw set them.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    static std::vector<VkDeviceSize> strides;
    strides.resize(r.vertex_inputs.size());
    for (size_t i = 0; i < r.vertex_inputs.size(); ++i)
    {
        strides[i] = r.vertex_inputs[i].stride;
    }
    vkCmdBindVertexBuffers2(cmd, 0, (U32)r.buffers.size(), r.buffers.data(), r.offsets.data(), nullptr, strides.data());

    const PFN_vkCmdExecuteGeneratedCommandsEXT execute = dgc->cmdExecute;
    if (!LLVKRecorder::deferred(cmd))
    {
        VkGeneratedCommandsPipelineInfoEXT bound = { VK_STRUCTURE_TYPE_GENERATED_COMMANDS_PIPELINE_INFO_EXT };
        bound.pipeline = pipeline;
        VkGeneratedCommandsInfoEXT copy = info;
        copy.pNext = &bound;
        execute(cmd, VK_FALSE, &copy);
    }
    else
    {
        // A packet for the recording worker, with the info and its pNext copied.
        struct P
        {
            LLVKRecorder::Header header;
            VkCommandBuffer c;
            PFN_vkCmdExecuteGeneratedCommandsEXT fn;
            VkGeneratedCommandsInfoEXT info;
            VkGeneratedCommandsPipelineInfoEXT bound;
        };
        P* p = LLVKRecorder::packet<P>([](void* self)
        {
            P* p = (P*)self;
            p->info.pNext = &p->bound;
            p->fn(p->c, VK_FALSE, &p->info);
        }, 0);
        p->c = cmd;
        p->fn = execute;
        p->info = info;
        p->info.pNext = nullptr;
        p->bound = { VK_STRUCTURE_TYPE_GENERATED_COMMANDS_PIPELINE_INFO_EXT };
        p->bound.pipeline = pipeline;
    }
    ++sGeneratedExecutes;
    LLRHI::sGPUDrivenDGCUsed.store(true, std::memory_order_relaxed);   // for a lost device (plus/gpudgc)

    // The bindings the commands set are undefined now: nothing recorded is
    // assumed bound, the pass cache and the run start over (as after a native pass).
    Recorded& rec = sRecorded;
    rec.pipeline = VK_NULL_HANDLE;
    rec.vertex_buffers.clear();
    rec.vertex_offsets.clear();
    rec.index_buffer = VK_NULL_HANDLE;
    rec.slot_count = 0;
    rec.dynamic = false;
    pass_invalidate();
    r.open = false;
    r.pass_ok = false;
    return true;
}

#endif // LL_GL_FUNC_POINTER

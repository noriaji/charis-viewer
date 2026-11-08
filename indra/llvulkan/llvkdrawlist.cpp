/**
 * @file llvkdrawlist.cpp
 * @brief The Vulkan backend's side of the draw lists (research/137 N1).
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

#include "llvkdrawlist.h"

#include "llrhidrawlist.h"
#include "llrhihash.h"
// Before the #if: llvkgl_internal.h brings llglheaders.h, which defines
// LL_GL_FUNC_POINTER (this file has no PCH: tested before it, the whole
// backend compiled to the stubs at the end; research/137 N7).
#include "llvkgl_internal.h"
#include "llvkgl_drawpath.h"

#if LL_GL_FUNC_POINTER

#include "llvkattribpool.h"
#include "llvkrecorder.h"

#include <array>
#include <atomic>
#include <deque>
#include <memory>
#include <thread>

// A list is recorded under one hold of the tracker lock, with the program
// resolved and the list-wide conditions checked once (sDrawListReady said
// them before the list began; no hook ran since, the guard saw to that, and
// no GL entry point: they cut too, research/137 N7). Per draw: its State
// (blend factors, culling, stencil reference, uniform patches) and palette
// row are applied the way the native state and shader API would apply them,
// its values become a DirectDraw, and the draw goes the way sDirectDraw
// takes it:
//   LLRHI_DRAW_LISTS=1 (light): list_begin resolves what draw_one resolves
//     before a draw's own values once for the list (the checks, the program,
//     the draw target); per draw list_draw (llvkgl_draw.cpp) does the rest of
//     direct_draw: a run's next draw, else the pass cache hit in its two
//     halves (pass_hit_resolve, pass_hit_record), else the full path.
//     Where list_begin says no (a probe capture, an occlusion query, a
//     depth-only pass, the census, no frame, draws writing nothing) the list
//     goes the reference way, which has those branches.
//   LLRHI_DRAW_LISTS=2 (reference): direct_draw itself, draw by draw.
// The viewer's flags for the draws (LLRHI::sDepthOnlyPass and the rest of
// LLVKGL::DrawGlobals) are the list's: taken when it was submitted, set for
// its recording (QueuedGlobals), the OIT priority per draw.
//
// research/137 N11 (LLRHI_DRAW_LISTS_THREADS, THREADS_RECORD): a long list on
// the light path goes in segments. This thread still does everything a draw
// decides (apply_state, the pass cache hit and its resolve: slots, rows, the
// uniform copy, the pool placement) and keeps the results as plain values; a
// cull thread records the segment's commands from them into a stream spliced
// in where they belong (LLVKRecorder), with a record of the bindings, the
// dynamic state and the open draw run of its own (the same decisions as
// pass_hit_record, run_draw and record_draw_commands make). Draws that are no
// plain pass cache hit go the usual way between segments. See "Segments" below.
namespace
{
    using namespace llvkgl;
    namespace DL = LLRHI::DrawList;

    struct Counts
    {
        U32 lists = 0;
        U32 draws = 0;
        U32 reference_draws = 0;    // LLRHI_DRAW_LISTS=2, and lists list_begin did not take
        U32 head_refused = 0;       // LLRHI_DRAW_LISTS=1 lists list_begin did not take (drawn the reference way)
        U32 refused = 0;            // direct_draw's refusals (the Builder should have kept them out): not drawn
        U32 invalid = 0;            // an index out of its array: not drawn
        U32 palette_refused = 0;    // a palette row the program does not take (drawn with the palette it had)
        U32 patch_refused = 0;      // a uniform patch with a slot the program does not have, or a sampler (skipped)
        U32 leaks = 0;              // lists whose state changed past the hooks after sDrawListReady (List::stamp)
        U32 captured = 0;           // lists drawn into a ProbeList capture (direct_draw keeps them)
        U32 lost = 0;               // lists not recorded at all (no program): their draws are lost (should stay 0)
        // Lists by length (research/140 7-6): up to 63, 64-255, 256-1023, 1024 and more draws.
        U32 length_lists[4] = {};
        U32 length_draws[4] = {};
        // Lists by their name (a string literal; the first ones seen).
        static constexpr U32 NAMES = 16;
        const char* names[NAMES] = {};
        U32 name_lists[NAMES] = {};
        U32 name_draws[NAMES] = {};
        U64 ticks = 0;              // recording, __rdtsc
    };
    Counts sCounts;

    void count_name(const char* name, U32 draws)
    {
        Counts& c = sCounts;
        for (U32 i = 0; i < Counts::NAMES; ++i)
        {
            if (!c.names[i])
            {
                c.names[i] = name;
            }
            if (c.names[i] == name)
            {
                ++c.name_lists[i];
                c.name_draws[i] += draws;
                return;
            }
        }
    }

    // What the state is now, for List::stamp: what sDrawListReady checks and
    // what the draws read from the GL state, on the thread that records
    // (sFixedState and sState are per thread; sPipelineState is not: texture
    // threads move it).
    U64 state_stamp()
    {
        const llvkgl::State& s = sState;
        U64 h = LLRHI::FNV1A_BASIS_SHORT;
        auto mix = [&h](U64 v) { h = LLRHI::fnv1aMix(h, v); };
        mix(sCurrentProgram);
        mix(sFixedState);
        mix(s.blend);
        mix(s.blend_src_rgb);
        mix(s.blend_dst_rgb);
        mix(s.blend_src_a);
        mix(s.blend_dst_a);
        mix(s.blend_eq_rgb);
        mix(s.blend_eq_a);
        mix((U32)s.color_mask[0] | ((U32)s.color_mask[1] << 1) | ((U32)s.color_mask[2] << 2) | ((U32)s.color_mask[3] << 3));
        mix(s.cull);
        mix(s.cull_face);
        mix(s.depth_test);
        mix(s.depth_write);
        mix(s.depth_func);
        mix(s.stencil);
        mix(s.polygon_mode);
        mix(s.draw_framebuffer);
        mix((U64)depth_only_pass() | ((U64)suppress_translation() << 1) | ((U64)sCapture.on << 2) | ((U64)(sActiveQuery != 0) << 3));
        mix(census_tag());
        mix(occlusion_scope());
        return h ? h : 1;
    }

    // A list's texture matrices, copied where they stay for the frame: the
    // matrix row cache (matrix_row) and the default block copy key on the
    // texture matrix's address with the matrix id, and a list's floats are
    // reused by the next list (research/137 N1 review 7). Lists are recorded
    // on one thread (the render thread), under sLock.
    struct TextureMatrices
    {
        U32 frame = U32_MAX;
        std::deque<std::array<F32, 16>> kept;
    };
    TextureMatrices sTextureMatrices;

    const F32* kept_texture_matrix(const F32* m)
    {
        TextureMatrices& t = sTextureMatrices;
        if (t.frame != sFrame)
        {
            t.frame = sFrame;
            t.kept.clear();
        }
        if (t.kept.empty() || !same_words_at(t.kept.back().data(), m, 16))
        {
            t.kept.emplace_back();
            memcpy(t.kept.back().data(), m, sizeof(F32) * 16);
        }
        return t.kept.back().data();
    }

    // The state at the head of the list that a State's INHERIT means.
    struct Head
    {
        GLenum blend[4];
        bool cull;
        GLenum cull_face;
        GLint stencil_ref[2];       // front, back
    };

    Head head_now()
    {
        const llvkgl::State& gl = sState;
        return { { gl.blend_src_rgb, gl.blend_dst_rgb, gl.blend_src_a, gl.blend_dst_a }, gl.cull, gl.cull_face,
                 { gl.stencil_front.ref, gl.stencil_back.ref } };
    }

    // Back to the head's blend factors, culling and stencil reference after
    // the list (promise 4: what the viewer's caches still say).
    struct RestoreHead
    {
        const Head& head;
        ~RestoreHead()
        {
            const llvkgl::State& s = sState;
            if (s.blend_src_rgb != head.blend[0] || s.blend_dst_rgb != head.blend[1] || s.blend_src_a != head.blend[2] ||
                s.blend_dst_a != head.blend[3])
            {
                set_blend_func(head.blend[0], head.blend[1], head.blend[2], head.blend[3]);
            }
            if (s.cull != head.cull)
            {
                set_cap(GL_CULL_FACE, head.cull);
            }
            if (s.cull_face != head.cull_face)
            {
                set_cull_face(head.cull_face);
            }
            if (s.stencil_front.ref != head.stencil_ref[0] || s.stencil_back.ref != head.stencil_ref[1])
            {
                set_stencil_ref(head.stencil_ref[0], head.stencil_ref[1]);
            }
        }
    };

    bool valid_draw(const DL::List& l, const DL::Draw& d)
    {
        if (d.state >= l.state_count || (U64)d.first_attribute + d.attribute_count > l.attribute_count ||
            (d.texture_count && (d.first_texture == DL::NONE || (U64)d.first_texture + d.texture_count > l.texture_count)) ||
            (d.matrices != DL::NONE && d.matrices >= l.matrix_count) ||
            (d.texture_matrix != DL::NONE && (U64)d.texture_matrix + 16 > l.float_count))
        {
            return false;
        }
        const DL::State& s = l.states[d.state];
        if ((U64)s.first_patch + s.patch_count > l.patch_count)
        {
            return false;
        }
        for (U32 i = 0; i < s.patch_count; ++i)
        {
            const DL::UniformPatch& p = l.patches[s.first_patch + i];
            if ((U64)p.first_float + (U64)p.count * p.components * llmax((U32)p.rows, 1u) > l.float_count)
            {
                return false;
            }
        }
        return true;
    }

    // What the native state API (sBlendFunc, sSetCap, sCullFace, glStencilFunc's
    // reference) and shader API (sSetUniform, sSetUniformMatrix, sUsePaletteRow)
    // would do.
    void apply_state(Program& program, const DL::List& l, const DL::Draw& d, const Head& head)
    {
        const DL::State& s = l.states[d.state];
        llvkgl::State& gl = sState;
        GLenum blend[4];
        for (U32 i = 0; i < 4; ++i)
        {
            blend[i] = s.blend[i] == DL::State::INHERIT ? head.blend[i] : (GLenum)s.blend[i];
        }
        if (gl.blend_src_rgb != blend[0] || gl.blend_dst_rgb != blend[1] || gl.blend_src_a != blend[2] || gl.blend_dst_a != blend[3])
        {
            set_blend_func(blend[0], blend[1], blend[2], blend[3]);
        }
        const bool cull = s.cull == DL::State::INHERIT ? head.cull : s.cull != 0;
        const GLenum face = s.cull == DL::State::INHERIT ? head.cull_face : s.cull ? (GLenum)s.cull : gl.cull_face;
        if (gl.cull != cull)
        {
            set_cap(GL_CULL_FACE, cull);
        }
        if (gl.cull_face != face)
        {
            set_cull_face(face);
        }
        const GLint ref_front = s.stencil_ref == DL::State::INHERIT ? head.stencil_ref[0] : (GLint)s.stencil_ref;
        const GLint ref_back = s.stencil_ref == DL::State::INHERIT ? head.stencil_ref[1] : (GLint)s.stencil_ref;
        if (gl.stencil_front.ref != ref_front || gl.stencil_back.ref != ref_back)
        {
            set_stencil_ref(ref_front, ref_back);
        }
        for (U32 i = 0; i < s.patch_count; ++i)
        {
            const DL::UniformPatch& p = l.patches[s.first_patch + i];
            if (p.slot < 0 || (size_t)p.slot >= program.native_slots.size())
            {
                ++sCounts.patch_refused;
                continue;
            }
            const UniformLocation& loc = program.native_slots[p.slot];
            if (loc.bindless >= 0 || (loc.uniform >= 0 && (size_t)loc.uniform < program.vk.uniforms.size() &&
                                      program.vk.uniforms[loc.uniform].binding >= 0))
            {
                // A sampler: it would move texture units the list's readiness
                // (Context::descriptor_units) was given for.
                ++sCounts.patch_refused;
                continue;
            }
            if (p.rows)
            {
                apply_uniform_matrix(program, loc, (GLsizei)p.count, p.components, p.rows, GL_FALSE, l.floats + p.first_float);
            }
            else
            {
                apply_uniform(program, loc, (GLsizei)p.count, p.components, l.floats + p.first_float);
            }
        }
        if (d.palette_base != DL::NONE)
        {
            LLRHI::PaletteRow row;
            row.base = d.palette_base;
            row.room = PREBUILT_PALETTE_ROOM;
            row.frame = sFrame;
            if (palette_row_takes(&program, row))
            {
                program.palette_prebuilt = true;
                program.palette_prebuilt_frame = sFrame;
                program.palette_prebuilt_base = row.base;
            }
            else
            {
                ++sCounts.palette_refused;
            }
        }
    }

    void fill_direct(const DL::List& l, const DL::Draw& d, LLRHI::DirectDraw& out)
    {
        out.vertex_buffer = d.vertex_buffer;
        out.index_buffer = d.index_buffer;
        out.index_type = d.index_type;
        if (d.index_buffer)
        {
            out.first_index = d.first;
            out.index_count = d.count;
        }
        else
        {
            out.first_vertex = d.first;
            out.vertex_count = d.count;
        }
        out.mode = d.mode;
        out.attributes = l.attributes + d.first_attribute;
        out.attribute_count = d.attribute_count;
        out.textures = d.texture_count ? l.textures + d.first_texture : nullptr;
        out.texture_count = d.texture_count;
        if (d.matrices != DL::NONE)
        {
            const DL::Matrices& m = l.matrices[d.matrices];
            out.modelview = m.modelview;
            out.modelview_projection = m.modelview_projection;
            out.normal = m.normal;
            out.inverse_modelview = m.inverse_modelview;
            out.matrix_id = m.id;
        }
        out.texture_matrix0 = d.texture_matrix != DL::NONE ? kept_texture_matrix(l.floats + d.texture_matrix) : nullptr;
        out.instance_count = d.instance_count;
        out.table_base = d.table_base;      // NONE is DirectDraw::NO_TABLE
        out.buffer_vertices = d.buffer_vertices;
        out.buffer_indices = d.buffer_indices;
        out.draw_template = nullptr;        // templates stay with direct draws (research/137 2.4)
    }

    // What sDrawListReady tells about the program (Context's palette_rows
    // and descriptor_units). Reads only what a link and the sampler
    // uniforms set.
    void fill_context(const Program& program, DL::Context& context)
    {
        context = DL::Context();
        context.palette_rows = program.palette_columns == 3 &&
                               (U64)program.palette_elements * program.palette_columns <= PREBUILT_PALETTE_ROOM;
        for (GLint unit : program.unit_values)
        {
            if (unit >= 0 && unit < 32)
            {
                context.descriptor_units |= 1u << unit;
            }
        }
    }

    // ---------------------------------------------------------------------
    // Segments (research/137 N11, research/140 3.2 R1)
    //
    // Who does what:
    //  - The list's thread (the render thread; under sLock): per draw everything list_draw does up to
    //    pass_hit_resolve, the draw run's checks and pool placement
    //    (run_draw's, up to LLVKAttribPool::place) and the counts; the
    //    results go into a SegJob. The first draw of a segment acquires a
    //    stream and splices it in (LLVKRecorder), so the segment's commands
    //    replay there, in order. A full segment (or the end of the chain)
    //    is posted to a cull thread (LLRHI::sPostWork). Nothing waits for it:
    //    the recording worker waits at the splice if it has to, and every
    //    stream is complete before its frame ends (LLVKRecorder::finish).
    //  - The cull thread: binds, dynamic state, push constants, buffers,
    //    draws and draw runs, from the SegJob alone, with a Recorded and a
    //    run of its own that start empty (it knows nothing of what is bound
    //    before the splice: the first draw binds everything). It reads no
    //    Program, texture, GL state or viewer object; it writes the stream,
    //    the job's counts, ring memory for its runs' indirect commands
    //    (LLVKMirror::allocate, a CAS) and their rows (LLVKBindless::
    //    paletteRows, atomic): what llvkrecorder.h allows a filler.
    //  - After the chain (the list's end, or before a draw that goes the
    //    usual way) the list's thread forgets what it knew of the bindings
    //    (sRecorded, and the open run: a later draw must not join an
    //    indirect draw recorded before the splice). The pass cache stays:
    //    a hit binds what differs from sRecorded, which is now nothing.
    //  - The job's counts reach the frame statistics once it is done
    //    (seg_fold, at the next list): a frame's last segments count in
    //    the next frame (sDrawsReplayed, the frame cost's draw counts), and
    //    the VKPerf line gets them a line late at most.
    // Same commands as recording the draws at once, but for the bindings
    // and dynamic state each segment records again at its start, the draw
    // runs that end at a segment's end, and the run capacity a segment
    // grows on its own (written back to the program afterwards).

    // At least this many draws for a list to go in segments, and at most
    // this many per segment (LLRHI_DRAW_LISTS_THREADS_MIN / _SEGMENT).
    U32 env_draws(const char* name, U32 fallback)
    {
        U64 value = 0;
        LLRHI::envNumber(name, 0, U32_MAX, value);
        return value > 0 ? (U32)value : fallback;
    }
    const U32 sSegMinDraws = env_draws("LLRHI_DRAW_LISTS_THREADS_MIN", 64);
    const U32 sSegDraws = env_draws("LLRHI_DRAW_LISTS_THREADS_SEGMENT", 256);
    constexpr U32 MAX_SEG_JOBS = 64;            // segments in flight (more: the rest of the list as before)
    // The table rows all segments' runs may take in a frame (run_row's
    // RUN_ROW_VEC4S_PER_FRAME, a share of its own: the table is shared with
    // the palettes, the lights and the materials). sSegRowVec4s: taken this
    // frame (cull threads, atomic); the list's thread sets it back to 0 at a
    // frame's first segment (the frame before has no segment left to record:
    // LLVKRecorder::finish waited for them).
    constexpr U32 SEG_ROW_VEC4S_PER_FRAME = 1u << 18;
    std::atomic<U32> sSegRowVec4s{ 0 };
    U32 sSegRowFrame = U32_MAX;     // the list's thread
    // A list goes the usual way from its MAX_SEG_BREAKS-th draw that is no
    // plain pass cache hit on (short segments cost more than they save).
    constexpr U32 MAX_SEG_BREAKS = 8;

    // One draw as the main thread resolved it: what pass_hit_record,
    // run_draw and record_draw_commands read, as values.
    struct SegDraw
    {
        VkPipeline pipeline = VK_NULL_HANDLE;   // PassHitDraw's
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;   // the program's
        U32 dynamic_offset = 0;
        bool has_dynamic = false;
        bool indexed = false;
        // The list's thread recorded commands (an upload) since the draw
        // before: run_draw's "nothing recorded since" fails for this one.
        bool break_run = false;
        bool rows = false;                      // Program::draw_rows
        bool rigged = false;                    // counted in sAvRiggedRun when it joins a run
        U32 dynamic = 0;                        // SegJob::dynamics
        U32 first_slot = 0, slot_count = 0;     // SegJob::slots (tSlots)
        U32 first_input = 0, input_count = 0;   // SegJob::buffers / offsets (tVertexBuffers / tVertexOffsets)
        VkBuffer index_buffer = VK_NULL_HANDLE;
        VkIndexType index_type = VK_INDEX_TYPE_UINT16;
        U32 first = 0;                          // first index (indexed), else first vertex
        U32 count = 0;
        U32 instances = 1;
        // A draw run may take it (run_draw's checks up to the placement
        // passed): SegJob::runs; DL::NONE: not.
        U32 run_inputs = DL::NONE;
        U32 chunk = 0;                          // LLVKAttribPool::Placement
        U32 pool_first_index = 0;
        S32 vertex_offset = 0;
        const void* program = nullptr;          // identity only: a run takes the draws of one program
        GLuint program_name = 0;
        U32 program_changes = 0;
        U32 run_capacity = 0;                   // Program::run_capacity when it was resolved
    };

    // A new run's vertex inputs (run_draw: each attribute from its stream in
    // the pool, the constants as direct_inputs found them) and index buffer.
    struct SegRunInputs
    {
        U32 first = 0, count = 0;               // SegJob::run_buffers / run_offsets
        VkBuffer index_buffer = VK_NULL_HANDLE;
        bool ok = false;                        // every stream there (else a new run refuses: RUN_INPUTS)
    };

    struct SegCounts
    {
        U32 draws = 0;                          // recorded
        U32 replayed = 0;                       // sDrawsReplayed
        U32 runs_opened = 0, run_draws = 0, run_rows = 0, run_rows_full = 0;   // sRunsOpened ...
        U32 run_inputs_refused = 0, run_ring_refused = 0;                      // sRunRefused
        U32 rebinds = 0;                        // sPassRebinds
        U32 rigged_run = 0;                     // sAvRiggedRun
    };

    // A program's run capacity as a segment's runs left it.
    struct SegCapacity
    {
        const void* program;
        GLuint program_name;
        U32 program_changes;
        U32 capacity;
    };

    enum SegState : int { SEG_IDLE, SEG_POSTED, SEG_RUNNING, SEG_DONE };

    struct SegJob
    {
        // IDLE: only the list's thread touches it. POSTED: handed over (the
        // arrays no longer change). RUNNING / DONE: claimed, recorded; the
        // list's thread reads the results once it sees DONE (acquire).
        std::atomic<int> state{ SEG_IDLE };
        LLVKRecorder::Stream* stream = nullptr;
        VkCommandBuffer cmd = VK_NULL_HANDLE;   // the frame's, as beginRendering returned it
        VkDescriptorSet table = VK_NULL_HANDLE; // LLVKBindless::set()
        PFN_vkCmdDrawIndexedIndirectCount draw_indirect_count = nullptr;
        std::vector<SegDraw> draws;
        std::vector<U32> slots;
        std::vector<VkBuffer> buffers;
        std::vector<VkDeviceSize> offsets;
        std::vector<DynamicValues> dynamics;
        std::vector<SegRunInputs> runs;
        std::vector<VkBuffer> run_buffers;
        std::vector<VkDeviceSize> run_offsets;
        // Results.
        SegCounts counts;
        std::vector<SegCapacity> capacities;
        U64 ticks = 0;
        size_t thread = 0;                      // std::hash of the recording thread's id
        bool on_main = false;                   // no cull thread took it: recorded where it was made
    };
    // Never freed (as the scene and the pools): a cull thread's late segment
    // task never finds it destroyed by the static destructors at exit
    // (security audit B 13).
    std::vector<std::unique_ptr<SegJob>>& sSegJobs = *new std::vector<std::unique_ptr<SegJob>>();  // the list's thread, under sLock

    // The VKPerf line's counts (the list's thread, under sLock).
    struct SegReport
    {
        U32 lists = 0;              // lists that recorded segments
        U32 segments = 0;
        U32 segment_draws = 0;
        U32 usual = 0;              // draws of such lists that went the usual way
        U32 on_main = 0;            // segments recorded on the list's thread (no cull thread)
        U32 no_job = 0;             // lists whose rest went the usual way: no free job or stream
        U64 ticks = 0;              // recording them
        static constexpr U32 THREADS = 8;
        size_t threads[THREADS] = {};
        U32 thread_count = 0;
    };
    SegReport sSegReport;

    // run_commands() (llvkgl_runs.cpp) on the calling thread's counters: in
    // a stream being filled, the stream's own.
    U64 commands_here()
    {
        const unsigned int* c = LLVKDispatch::counts();
        U64 sum = 0;
        for (U32 i = 0; i < LLVKDispatch::COUNTER_COUNT; ++i)
        {
            sum += c[i];
        }
        return sum - c[LLVKDispatch::C_UpdateDescriptorSets] - c[LLVKDispatch::C_AllocateDescriptorSets];
    }

    // The segment's open draw run (DrawRun's part run_draw reads) and its last row (run_row's).
    struct SegRun
    {
        bool open = false;
        const void* program = nullptr;
        U32 program_changes = 0;
        U64 commands = 0;
        U32 chunk = 0;
        bool rows = false;
        U32 slots[LLVKBindless::MAX_PUSHED_SLOTS] = {};
        U32 slot_count = 0;
        VkDrawIndexedIndirectCommand* args = nullptr;
        U32* count = nullptr;
        U32 capacity = 0;
        U32 used = 0;
        U32 row[LLVKBindless::MAX_PUSHED_SLOTS] = {};
        U32 row_words = 0;
        U32 row_base = 0;
    };

    // run_row: the draw's words in a row of the frame's table, shared with the row before when the same.
    bool seg_run_row(SegRun& run, const U32* slots, U32 slot_count, U32& first_instance, SegCounts& c)
    {
        const U32 words = slot_count - 1;
        if (run.row_words == words && same_words(run.row, slots, words))
        {
            first_instance = run.row_base;
            return true;
        }
        const U32 vec4s = (words + 3) / 4;
        U32 base = 0;
        // This frame's share, taken or not (the CAS: other segments take theirs at the same time).
        U32 taken = sSegRowVec4s.load(std::memory_order_relaxed);
        bool room = false;
        while (taken + vec4s <= SEG_ROW_VEC4S_PER_FRAME &&
               !(room = sSegRowVec4s.compare_exchange_weak(taken, taken + vec4s, std::memory_order_relaxed)))
        {
        }
        F32* row = room ? LLVKBindless::paletteRows(vec4s, base) : nullptr;
        if (!row)
        {
            ++c.run_rows_full;
            return false;
        }
        U32 padded[LLVKBindless::MAX_PUSHED_SLOTS + 3] = {};
        memcpy(padded, slots, words * 4);
        memcpy(row, padded, (size_t)vec4s * 16);
        memcpy(run.row, slots, words * 4);
        run.row_words = words;
        run.row_base = base;
        first_instance = base;
        ++c.run_rows;
        return true;
    }

    // A program's run capacity in this segment (from the program's when first met).
    U32& seg_capacity(SegJob& job, const SegDraw& d)
    {
        for (SegCapacity& cap : job.capacities)
        {
            if (cap.program == d.program && cap.program_changes == d.program_changes)
            {
                return cap.capacity;
            }
        }
        job.capacities.push_back({ d.program, d.program_name, d.program_changes, d.run_capacity });
        return job.capacities.back().capacity;
    }

    bool same_vertex_inputs(const Recorded& r, const VkBuffer* buffers, const VkDeviceSize* offsets, U32 count)
    {
        if (r.vertex_buffers.size() != count || r.vertex_offsets.size() != count)
        {
            return false;
        }
        for (U32 i = 0; i < count; ++i)
        {
            if (r.vertex_buffers[i] != buffers[i] || r.vertex_offsets[i] != offsets[i])
            {
                return false;
            }
        }
        return true;
    }

    // run_draw for a draw the list's thread found a run may take (its checks
    // up to the placement passed there). False: record it the usual way.
    bool seg_run_draw(SegJob& job, const SegDraw& d, const U32* slots, Recorded& r, SegRun& run, SegCounts& c)
    {
        const VkCommandBuffer cmd = job.cmd;
        const U32 n = d.slot_count;
        // A rows draw ends its words with the rows word (seg_run_row reads n - 1 words, pushed[n - 1] is
        // written below); without it, or with more words than the push block holds, record it the usual way.
        if ((d.rows && n == 0) || n > LLVKBindless::MAX_PUSHED_SLOTS)
        {
            return false;
        }
        const U64 before = commands_here();
        const bool join = run.open && run.program == d.program && run.program_changes == d.program_changes && run.commands == before &&
                          !d.break_run && run.chunk == d.chunk && run.used < run.capacity && run.rows == d.rows &&
                          (run.rows || (run.slot_count == n && same_words(run.slots, slots, n)));
        U32 first_instance = 0;
        if (d.rows && !seg_run_row(run, slots, n, first_instance, c))
        {
            return false;
        }
        if (!join)
        {
            U32& program_capacity = seg_capacity(job, d);
            if (run.open && run.program == d.program && run.program_changes == d.program_changes)
            {
                if (run.used == run.capacity)
                {
                    program_capacity = llmin(program_capacity * 2, RUN_MAX_CAPACITY);
                }
                else if (run.used * 4 < run.capacity)
                {
                    program_capacity = llmax(program_capacity / 2, RUN_MIN_CAPACITY);
                }
            }
            run.open = false;
            const U32 capacity = llclamp(program_capacity, RUN_MIN_CAPACITY, RUN_MAX_CAPACITY);
            const SegRunInputs& in = job.runs[d.run_inputs];
            if (!in.ok)
            {
                ++c.run_inputs_refused;
                return false;
            }
            VkBuffer ring = VK_NULL_HANDLE;
            VkDeviceSize offset = 0;
            void* data = nullptr;
            if (!in.index_buffer || !job.draw_indirect_count ||
                !LLVKMirror::instance().allocate(16 + (VkDeviceSize)capacity * sizeof(VkDrawIndexedIndirectCommand), 16, ring, offset, data))
            {
                ++c.run_ring_refused;
                return false;
            }
            // Push constants: the draw's, the rows word saying "read the rows".
            U32 pushed[LLVKBindless::MAX_PUSHED_SLOTS] = {};
            if (n)
            {
                memcpy(pushed, slots, n * 4);
            }
            if (d.rows)
            {
                pushed[n - 1] = 0;
            }
            if (n && (n > r.slot_count || !same_words(pushed, r.slots, n)))
            {
                const VkPushConstantRange range = LLVKBindless::pushConstantRange();
                vkCmdPushConstants(cmd, d.layout, range.stageFlags, 0, n * 4, pushed);
                memcpy(r.slots, pushed, n * 4);
                r.slot_count = llmax(r.slot_count, n);
            }
            const VkBuffer* buffers = in.count ? &job.run_buffers[in.first] : nullptr;
            const VkDeviceSize* offsets = in.count ? &job.run_offsets[in.first] : nullptr;
            if (!same_vertex_inputs(r, buffers, offsets, in.count))
            {
                vkCmdBindVertexBuffers(cmd, 0, in.count, buffers, offsets);
                r.vertex_buffers.assign(buffers, buffers + in.count);
                r.vertex_offsets.assign(offsets, offsets + in.count);
            }
            if (in.index_buffer != r.index_buffer || r.index_type != VK_INDEX_TYPE_UINT32 || r.index_offset != 0)
            {
                vkCmdBindIndexBuffer(cmd, in.index_buffer, 0, VK_INDEX_TYPE_UINT32);
                r.index_buffer = in.index_buffer;
                r.index_type = VK_INDEX_TYPE_UINT32;
                r.index_offset = 0;
            }
            U32* count_word = (U32*)data;
            *count_word = 0;
            LLVKRecorder::value(job.draw_indirect_count, cmd, ring, (VkDeviceSize)(offset + 16), ring, (VkDeviceSize)offset, (uint32_t)capacity,
                                (uint32_t)sizeof(VkDrawIndexedIndirectCommand));
            run.open = true;
            run.program = d.program;
            run.program_changes = d.program_changes;
            run.chunk = d.chunk;
            run.rows = d.rows;
            if (n)
            {
                memcpy(run.slots, slots, n * 4);
            }
            run.slot_count = n;
            run.args = (VkDrawIndexedIndirectCommand*)((U8*)data + 16);
            run.count = count_word;
            run.capacity = capacity;
            run.used = 0;
            ++c.runs_opened;
        }
        // run_push.
        VkDrawIndexedIndirectCommand& command = run.args[run.used++];
        command.indexCount = d.count;
        command.instanceCount = 1;
        command.firstIndex = d.pool_first_index;
        command.vertexOffset = d.vertex_offset;
        command.firstInstance = first_instance;
        *run.count = run.used;
        run.commands = commands_here();
        ++c.run_draws;
        c.rigged_run += d.rigged ? 1 : 0;
        return true;
    }

    // record_draw_commands (no checkpoints and no census here: segments are
    // not made while either is on).
    void seg_record_draw(SegJob& job, const SegDraw& d, const U32* slots, Recorded& r, SegCounts& c)
    {
        const VkCommandBuffer cmd = job.cmd;
        const U32 n = d.slot_count;
        if (n && (n > r.slot_count || !same_words(slots, r.slots, n)))
        {
            const VkPushConstantRange range = LLVKBindless::pushConstantRange();
            vkCmdPushConstants(cmd, d.layout, range.stageFlags, 0, n * 4, slots);
            memcpy(r.slots, slots, n * 4);
            r.slot_count = llmax(r.slot_count, n);
        }
        if (d.input_count)
        {
            const VkBuffer* buffers = &job.buffers[d.first_input];
            const VkDeviceSize* offsets = &job.offsets[d.first_input];
            if (!same_vertex_inputs(r, buffers, offsets, d.input_count))
            {
                vkCmdBindVertexBuffers(cmd, 0, d.input_count, buffers, offsets);
                r.vertex_buffers.assign(buffers, buffers + d.input_count);
                r.vertex_offsets.assign(offsets, offsets + d.input_count);
            }
        }
        if (d.indexed)
        {
            if (d.index_buffer != r.index_buffer || d.index_type != r.index_type || r.index_offset != 0)
            {
                vkCmdBindIndexBuffer(cmd, d.index_buffer, 0, d.index_type);
                r.index_buffer = d.index_buffer;
                r.index_type = d.index_type;
                r.index_offset = 0;
            }
            vkCmdDrawIndexed(cmd, d.count, d.instances, d.first, 0, 0);
        }
        else
        {
            vkCmdDraw(cmd, d.count, d.instances, d.first, 0);
        }
        ++c.replayed;
    }

    // fix-threads4: a segment that threw (bad_alloc, ...) on `where`: logged
    // (the first few). Its stream holds the draws recorded before (each
    // segment binds everything again, so the rest of the frame is not
    // affected); the job still ends, so nothing waits for it forever.
    void seg_failed(const char* where)
    {
        static std::atomic<U32> sFailures{ 0 };
        if (sFailures.fetch_add(1, std::memory_order_relaxed) < 5)
        {
            try
            {
                LL_WARNS("VKDrawList") << "a draw list segment failed (" << where << "): some of its draws are missing this frame" << LL_ENDL;
            }
            catch (...)
            {
            }
        }
    }

    // The segment's commands into its stream (a cull thread, or the list's
    // thread when none took it). Reads the job only (see "Who does what").
    void seg_record(SegJob& job)
    {
        const U64 start = __rdtsc();
        try
        {
            LLVKRecorder::Fill fill(job.stream);    // complete for the recording worker at the end of this scope
            Recorded r;         // nothing known of what is bound at the splice: the first draw binds it all
            SegRun run;
            SegCounts& c = job.counts;
            const VkCommandBuffer cmd = job.cmd;
            for (const SegDraw& d : job.draws)
            {
                const U32* slots = d.slot_count ? &job.slots[d.first_slot] : nullptr;
                // pass_hit_record.
                if (r.pipeline != d.pipeline || (d.set ? r.set != d.set : !table_at_set0(r)))
                {
                    ++c.rebinds;
                }
                if (r.pipeline != d.pipeline)
                {
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, d.pipeline);
                    r.pipeline = d.pipeline;
                }
                bind_program_set(cmd, r, d.layout, job.table, d.set, d.has_dynamic, d.dynamic_offset);
                record_dynamic_values(cmd, r, job.dynamics[d.dynamic]);
                if (d.run_inputs == DL::NONE || !seg_run_draw(job, d, slots, r, run, c))
                {
                    seg_record_draw(job, d, slots, r, c);
                }
                ++c.draws;
            }
        }
        catch (const std::exception& e)
        {
            seg_failed(e.what());
        }
        catch (...)
        {
            seg_failed("an unknown exception");
        }
        job.ticks = __rdtsc() - start;
        job.thread = std::hash<std::thread::id>()(std::this_thread::get_id());
        job.state.store(SEG_DONE, std::memory_order_release);
        job.state.notify_all();     // fix-threads4: seg_join may wait for it
    }

    // LLRHI::sPostWork's task.
    void seg_task(void* arg)
    {
        SegJob& job = *static_cast<SegJob*>(arg);
        int expected = SEG_POSTED;
        if (job.state.compare_exchange_strong(expected, SEG_RUNNING, std::memory_order_acquire, std::memory_order_relaxed))
        {
            seg_record(job);
        }
    }

    // fix-threads4: LLVKRecorder::sJoinFillers, from finish() on the list's
    // thread (the render thread), with no list running. A segment no cull thread has started is recorded
    // here; one running is waited for. After this no segment records any
    // more: none is left running past the frame (after the splice timeout,
    // it would take ring memory and rows of the next frame).
    void seg_join()
    {
        for (const std::unique_ptr<SegJob>& job : sSegJobs)
        {
            int expected = SEG_POSTED;
            if (job->state.compare_exchange_strong(expected, SEG_RUNNING, std::memory_order_acquire, std::memory_order_relaxed))
            {
                job->on_main = true;
                seg_record(*job);
            }
            else if (expected == SEG_RUNNING)
            {
                job->state.wait(SEG_RUNNING, std::memory_order_acquire);   // seg_record's notify_all
            }
        }
    }

    // A recorded job's counts into the frame statistics; the job is free again.
    void seg_fold(SegJob& job)
    {
        const SegCounts& c = job.counts;
        sDrawsReplayed += c.replayed;
        sRunsOpened += c.runs_opened;
        sRunDraws += c.run_draws;
        sRunRows += c.run_rows;
        sRunRowsFull += c.run_rows_full;
        sRunRefused[RUN_INPUTS] += c.run_inputs_refused;
        sRunRefused[RUN_RING] += c.run_ring_refused;
        sPassRebinds += c.rebinds;
        sAvRiggedRun += c.rigged_run;
        for (const SegCapacity& cap : job.capacities)
        {
            // The program is still the one the draws were made for (not
            // deleted, not linked again): it keeps the capacity its runs grew.
            auto it = sPrograms.find(cap.program_name);
            if (it != sPrograms.end() && &it->second == cap.program && sProgramChanges == cap.program_changes)
            {
                it->second.run_capacity = cap.capacity;
            }
        }
        SegReport& rep = sSegReport;
        ++rep.segments;
        rep.segment_draws += c.draws;
        rep.on_main += job.on_main ? 1 : 0;
        rep.ticks += job.ticks;
        bool seen = false;
        for (U32 i = 0; i < rep.thread_count && !seen; ++i)
        {
            seen = rep.threads[i] == job.thread;
        }
        if (!seen && rep.thread_count < SegReport::THREADS)
        {
            rep.threads[rep.thread_count++] = job.thread;
        }
        job.state.store(SEG_IDLE, std::memory_order_relaxed);
    }

    void seg_fold_done()
    {
        for (const std::unique_ptr<SegJob>& job : sSegJobs)
        {
            if (job->state.load(std::memory_order_acquire) == SEG_DONE)
            {
                seg_fold(*job);
            }
        }
    }

    // A job no thread holds (folding the done ones on the way); null: none.
    SegJob* seg_free_job()
    {
        for (const std::unique_ptr<SegJob>& job : sSegJobs)
        {
            int state = job->state.load(std::memory_order_acquire);
            if (state == SEG_DONE)
            {
                seg_fold(*job);
                state = SEG_IDLE;
            }
            if (state == SEG_IDLE)
            {
                return job.get();
            }
        }
        if (sSegJobs.size() < MAX_SEG_JOBS)
        {
            sSegJobs.push_back(std::make_unique<SegJob>());
            return sSegJobs.back().get();
        }
        return nullptr;
    }

    // One list's segments, on the list's thread (draw_list).
    struct SegChain
    {
        bool on = false;                // the list goes in segments
        bool counted = false;           // counted in SegReport::lists
        SegJob* job = nullptr;          // the open segment (spliced, not posted yet)
        bool spliced = false;           // segments were spliced since this thread last recorded
        U64 serial_mark = 0;            // LLVKMirror::recordSerial() after the open segment's last draw
        // The open segment's last dynamic values, by what they were made from.
        U32 dyn_index = DL::NONE;
        U32 dyn_fixed = 0, dyn_pipeline = 0;
        VkPrimitiveTopology dyn_prim = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
        // The open segment's last run inputs, by what they were made from.
        U32 run_index = DL::NONE;
        U32 run_chunk = U32_MAX;
        const void* run_program = nullptr;
        U32 run_program_changes = 0, run_constants = 0;
        U64 run_format = 0;

        ~SegChain() { end(); }

        // Hands the open segment to a cull thread (to this thread without one).
        void post()
        {
            SegJob* const open = job;
            job = nullptr;
            if (!open)
            {
                return;
            }
            open->state.store(SEG_POSTED, std::memory_order_release);
            if (open->draws.empty() || !LLRHI::sPostWork || !LLRHI::sPostWork(&seg_task, open))
            {
                int expected = SEG_POSTED;
                if (open->state.compare_exchange_strong(expected, SEG_RUNNING, std::memory_order_acquire, std::memory_order_relaxed))
                {
                    open->on_main = true;
                    seg_record(*open);
                }
            }
        }

        // The chain ends: what this thread knew of the bindings is stale.
        void end()
        {
            post();
            if (spliced)
            {
                spliced = false;
                sRecorded = Recorded();
                sRecorded.frame = sFrame;
                sRun.open = false;      // its indirect draw was recorded before the splice
            }
        }

        // A segment for a draw recorded into `target` (pass_hit_record's,
        // with the hit's depth_read_only). False: none (no free job or no
        // stream): the draw goes the usual way.
        bool open(const LLVKMirror::Target& target)
        {
            SegJob* free = seg_free_job();
            if (!free)
            {
                return false;
            }
            LLVKMirror& mirror = LLVKMirror::instance();
            const U32 scope = mirror.mScopeEpoch;
            // The pass cache hit's scope is the open one: this records nothing
            // (and moves recordSerial, as each pass_hit_record does).
            const VkCommandBuffer cmd = mirror.beginRendering(target);
            if (!cmd || scope != mirror.mScopeEpoch)
            {
                return false;   // the hit's draw goes the usual way (into the scope just begun)
            }
            LLVKRecorder::Stream* stream = LLVKRecorder::acquire();
            if (!stream)
            {
                return false;
            }
            LLVKRecorder::splice(stream);
            spliced = true;
            counted = true;
            if (sSegRowFrame != sFrame)
            {
                sSegRowFrame = sFrame;
                sSegRowVec4s.store(0, std::memory_order_relaxed);   // seen by the cull threads through the post's mutex
            }
            SegJob& j = *free;
            j.stream = stream;
            j.cmd = cmd;
            j.table = LLVKBindless::set();
            j.draw_indirect_count = LLVKAttribPool::drawIndexedIndirectCount();
            j.draws.clear();
            j.slots.clear();
            j.buffers.clear();
            j.offsets.clear();
            j.dynamics.clear();
            j.runs.clear();
            j.run_buffers.clear();
            j.run_offsets.clear();
            j.counts = SegCounts();
            j.capacities.clear();
            j.ticks = 0;
            j.thread = 0;
            j.on_main = false;
            job = &j;
            serial_mark = mirror.recordSerial();
            dyn_index = DL::NONE;
            run_index = DL::NONE;
            return true;
        }
    };

    // Whether a list goes in segments (checked once at its head, after list_begin).
    // sCullThreads: with the cull threads off every post would fail and each
    // segment be recorded here, in spliced streams for nothing.
    bool seg_wanted(const DL::List& l)
    {
        return (DL::sThreads & DL::THREADS_RECORD) && DL::sMode == 1 && l.draw_count >= sSegMinDraws && LLRHI::sPostWork &&
               LLRHI::sCullThreads &&
               LLRHI::sRecordSplice && LLVKRecorder::enabled() && !sOIT.active && !sCapture.on &&
               !LLVKDevice::instance().checkpointsEnabled() && sPassCacheEnabled;
    }

    // run_draw's checks before the join, for the segment's draw (tDirect is
    // `d`, its slots in tSlots), counted as there; then the pool placement.
    bool seg_run_eligible(Program& program, const LLRHI::DirectDraw& d, VkPrimitiveTopology prim, GLsizei count, GLsizei instances,
                          bool indexed, LLVKAttribPool::Placement& place)
    {
        if (!LLRHI::sDrawRuns || LLVKDevice::instance().checkpointsEnabled() || census_tag())
        {
            return false;
        }
        RunRefusal why = RUN_REFUSAL_COUNT;
        if (!program.draw_runs)
        {
            why = RUN_UNMARKED;
        }
        else if (!indexed || prim != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST || instances != 1 || count <= 0 ||
                 d.table_base != LLRHI::DirectDraw::NO_TABLE)
        {
            why = RUN_SHAPE;
        }
        else if (!d.buffer_vertices)
        {
            why = RUN_UNSIZED;
        }
        else if (program.draw_rows && (!tSlotCount || tSlotCount != program.draw_row_words + 1))
        {
            why = RUN_SLOTS;
        }
        else
        {
            LLVKAttribPool::Ticket ticket;
            if (!LLVKAttribPool::place(d, place, ticket))
            {
                why = RUN_POOL;
            }
        }
        if (why != RUN_REFUSAL_COUNT)
        {
            ++sRunRefused[why];
            return false;
        }
        return true;
    }

    // The new run's inputs for a draw placed in `chunk` (run_draw's, made
    // here: the pool's streams are this thread's), shared by the segment's
    // draws of one vertex format.
    U32 seg_run_inputs(SegChain& chain, const LLRHI::DirectDraw& d, U32 chunk)
    {
        const DirectInputs& f = tDirectInputs;
        if (chain.run_index != DL::NONE && chain.run_chunk == chunk && chain.run_program == f.program &&
            chain.run_program_changes == f.program_changes && chain.run_constants == f.constants && chain.run_format == f.format)
        {
            return chain.run_index;
        }
        SegJob& job = *chain.job;
        SegRunInputs in;
        in.first = (U32)job.run_buffers.size();
        const size_t n = f.source.size();
        in.ok = tVertexBuffers.size() == n;
        for (size_t i = 0; in.ok && i < n; ++i)
        {
            const S32 k = f.source[i];
            if (k >= 0)
            {
                const LLRHI::DirectDraw::Attribute& a = d.attributes[k];
                const VkBuffer stream = LLVKAttribPool::streamBuffer(chunk, a.streamKey(), a.stride);
                in.ok = stream != VK_NULL_HANDLE;
                job.run_buffers.push_back(stream);
                job.run_offsets.push_back(a.streamShift());     // research/293 branch 1
            }
            else
            {
                job.run_buffers.push_back(tVertexBuffers[i]);
                job.run_offsets.push_back(tVertexOffsets[i]);
            }
        }
        in.count = (U32)job.run_buffers.size() - in.first;
        in.index_buffer = LLVKAttribPool::indexBuffer(chunk);
        job.runs.push_back(in);
        chain.run_index = (U32)job.runs.size() - 1;
        chain.run_chunk = chunk;
        chain.run_program = f.program;
        chain.run_program_changes = f.program_changes;
        chain.run_constants = f.constants;
        chain.run_format = f.format;
        return chain.run_index;
    }

    enum class SegResult
    {
        TAKEN,      // handled (in the segment, or dropped or skipped as list_draw would)
        REFUSED,    // direct_draw's refusal (counted): not drawn
        USUAL,      // list_draw draws it (after the chain ends): nothing was counted for it here
    };

    // list_draw for a draw of a list in segments: the same checks, counts and
    // resolve; a pass cache hit goes into the open segment instead of being
    // recorded. Under sLock, on the list's thread.
    SegResult seg_draw(ListHead& head, const LLRHI::DirectDraw& d, SegChain& chain)
    {
        Program& program = *head.program;
        const U32 refusal = direct_refusal(&program, d);
        if (refusal != DIRECT_REFUSAL_COUNT)
        {
            ++sDirectRefused[refusal];
            if (LLRHIVBCensus::sOn)     // research/293 branch 0: counting only
            {
                LLRHIVBCensus::refused(census_owner(d.vertex_buffer), (LLRHIVBCensus::Refusal)refusal);
            }
            return SegResult::REFUSED;
        }
        const GLenum mode = (GLenum)d.mode;
        if (mode == GL_TRIANGLE_FAN || mode == GL_LINE_LOOP || mode == GL_QUADS ||
            head.target_state != sTargetState.load(std::memory_order_relaxed) || head.framebuffer != sState.draw_framebuffer ||
            !head.target_ok)
        {
            // Converted, refused, a target to resolve again or one that failed to: list_draw's own branches.
            return SegResult::USUAL;
        }
        PerfScope total(PERF_DRAW);
        const bool indexed = d.index_count > 0;
        const GLsizei count = (GLsizei)(indexed ? d.index_count : d.vertex_count);
        const GLsizei instances = (GLsizei)llmax(d.instance_count, 1u);
        // What list_draw counts for a draw it takes (whether it ends up drawn or skipped).
        auto counted = [&]()
        {
            if (LLRHI::sAvatarStats && program.palette_columns == 3)
            {
                ++sAvRiggedDraws;
            }
            if (count > 0)
            {
                if (head.depth_only_program)
                {
                    ++sDepthOnlyFailures["outside the pass"];
                }
                ++program.generic_draws;
            }
            ++sDirectDraws;
            if (d.table_base != LLRHI::DirectDraw::NO_TABLE)
            {
                ++sLightBatches;
                sLightInstances += d.instance_count;
            }
            return SegResult::TAKEN;
        };
        if (count <= 0)
        {
            return counted();
        }
        struct Current
        {
            explicit Current(const LLRHI::DirectDraw& draw) { tDirect = &draw; }
            ~Current() { tDirect = nullptr; }
        } current(d);
        // draw_into_target (no OIT session: seg_wanted).
        U64 signature = 0;
        if (!direct_inputs(program, signature))
        {
            return counted();   // skipped (counted by direct_inputs)
        }
        VkBuffer index_buffer = VK_NULL_HANDLE;
        if (indexed)
        {
            index_buffer = gpu_buffer_named(d.index_buffer);
            if (!index_buffer)
            {
                skip("index buffer");
                return counted();
            }
            const GLenum index_type = d.index_type == GL_UNSIGNED_INT ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
            if (!index_range_fits(sBuffers.find(d.index_buffer), index_type, (size_t)d.first_index * (index_type == GL_UNSIGNED_INT ? 4 : 2), count))
            {
                skip("index range");
                return counted();
            }
        }
        const VkPrimitiveTopology prim = topology(mode);
        if (direct_writes_block(program))
        {
            return SegResult::USUAL;
        }
        const PassHit hit = pass_hit(program, head.target, prim, signature);
        if (hit == PASS_MISS)
        {
            return SegResult::USUAL;    // the full path (it records: barriers, sets, pipelines)
        }
        ResolvedTarget target = head.target;
        PassHitDraw h;
        const PassHitResult result = pass_hit_resolve(program, target, hit, h);
        if (result == PASS_HIT_FULL_PATH)
        {
            return SegResult::USUAL;
        }
        if (result == PASS_HIT_RECORD)
        {
            target.target.depth_read_only = h.depth_read_only;
            if (!chain.job && !chain.open(target.target))
            {
                // No job or stream: this draw and the rest of the list as
                // before (what the resolve did, the usual way does again).
                chain.on = false;
                ++sSegReport.no_job;
                return SegResult::USUAL;
            }
        }
        counted();
        if (result == PASS_HIT_DROP)
        {
            return SegResult::TAKEN;    // not drawn (pass_hit_resolve counted it)
        }

        LLVKMirror& mirror = LLVKMirror::instance();
        SegJob& job = *chain.job;
        SegDraw s;
        s.pipeline = h.pipeline;
        s.set = h.set;
        s.layout = program.layout;
        s.dynamic_offset = h.dynamic_offset;
        s.has_dynamic = h.has_dynamic;
        s.indexed = indexed;
        const U32 fixed = sFixedState;
        const U32 pipeline_state = sPipelineState.load(std::memory_order_relaxed);
        if (chain.dyn_index == DL::NONE || chain.dyn_fixed != fixed || chain.dyn_pipeline != pipeline_state || chain.dyn_prim != prim)
        {
            // Every write to sState a draw reads moves one of these (llvkgl_internal.h).
            job.dynamics.push_back(dynamic_values(target.target, prim));
            chain.dyn_index = (U32)job.dynamics.size() - 1;
            chain.dyn_fixed = fixed;
            chain.dyn_pipeline = pipeline_state;
            chain.dyn_prim = prim;
        }
        s.dynamic = chain.dyn_index;
        s.first_slot = (U32)job.slots.size();
        s.slot_count = tSlotCount;
        job.slots.insert(job.slots.end(), tSlots, tSlots + tSlotCount);
        s.first_input = (U32)job.buffers.size();
        s.input_count = (U32)tVertexBuffers.size();
        job.buffers.insert(job.buffers.end(), tVertexBuffers.begin(), tVertexBuffers.end());
        job.offsets.insert(job.offsets.end(), tVertexOffsets.begin(), tVertexOffsets.end());
        s.index_buffer = index_buffer;
        s.index_type = d.index_type == GL_UNSIGNED_INT ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
        s.first = indexed ? d.first_index : d.first_vertex;
        s.count = (U32)count;
        s.instances = (U32)instances;
        s.rows = program.draw_rows;
        s.rigged = LLRHI::sAvatarStats && program.palette_columns == 3;
        s.program = &program;
        s.program_name = sCurrentProgram;
        s.program_changes = sProgramChanges;
        s.run_capacity = program.run_capacity;
        // run_draw's "nothing recorded since the run's last draw": an upload
        // this thread recorded for this draw (gpu_buffer) counts as there.
        s.break_run = mirror.recordSerial() != chain.serial_mark;
        LLVKAttribPool::Placement place;
        if (seg_run_eligible(program, d, prim, count, instances, indexed, place))
        {
            s.run_inputs = seg_run_inputs(chain, d, place.chunk);
            s.chunk = place.chunk;
            s.pool_first_index = place.first_index + d.first_index;
            s.vertex_offset = place.vertex_offset;
        }
        job.draws.push_back(s);
        chain.serial_mark = mirror.recordSerial();
        ++sPassHits;
        if (job.draws.size() >= sSegDraws)
        {
            chain.post();
        }
        return SegResult::TAKEN;
    }

    // Every state change the viewer makes inside a pool loop must reach a
    // hook or a GL entry point (both cut): with one of these hooks missing it
    // would go to a GL entry point the backend does not wrap (review 1).
    bool hooks_there()
    {
        return LLRHI::sUseProgram && LLRHI::sSetUniform && LLRHI::sSetUniformMatrix && LLRHI::sSetCap && LLRHI::sBlendFunc &&
               LLRHI::sCullFace && LLRHI::sBindTexture && LLRHI::sActiveTexture;
    }

    // LLRHI::DrawList::sDrawListReady (on the thread that records: the render
    // thread).
    bool draw_list_ready(DL::Context& context)
    {
        if (!hooks_there())
        {
            return false;
        }
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        if (!LLRHI::sDirect || !LLVKMirror::instance().isRecording() || sCapture.on || depth_only_pass() || sActiveQuery ||
            suppress_translation() || census_tag())
        {
            return false;
        }
        const Program* program = current_program();
        if (!program || !program->built)
        {
            return false;
        }
        fill_context(*program, context);
        context.stamp = state_stamp();
        return true;
    }

    // LLRHI::DrawList::sDrawList: records the list with the viewer's flags
    // of now as its draws' (each draw's own OIT priority).
    bool draw_list(const DL::List& l)
    {
        const U64 start = __rdtsc();
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        LLVKGL::DrawGlobals list_globals = LLVKGL::drawGlobalsNow();
        const LLVKGL::QueuedGlobals globals_scope(list_globals);   // depth_only_pass() and the rest read the list's
        if (l.stamp && l.stamp != state_stamp())
        {
            // Something changed the state past the hooks since sDrawListReady
            // (a ProbeList or ShadowList call, a plain flag). The draws go
            // with the state as it is now, as the old way's would.
            ++sCounts.leaks;
            LL_WARNS_ONCE("VKDrawList") << "draw list '" << l.name << "': the state changed past the hooks while it was open"
                                        << LL_ENDL;
        }
        Program* program = current_program();
        if (!program || !program->built)
        {
            ++sCounts.lost;
            return false;   // only after such a change: sDrawListReady said no to these
        }
        const Head head = head_now();
        const RestoreHead restore_head{ head };
        // In a ProbeList capture (begun past the hooks) direct_draw keeps each
        // draw for the capture, as the old way's direct draws.
        sCounts.captured += sCapture.on ? 1 : 0;
        ListHead list_head;
        const bool light = DL::sMode == 1 && list_begin(list_head);
        sCounts.head_refused += DL::sMode == 1 && !light ? 1 : 0;
        // research/137 N11: a long list's pass cache hits recorded in
        // segments on the cull threads (see "Segments"). Ends (and makes this
        // thread forget the bindings) before a draw that goes the usual way
        // and at the end of the list, also when something throws.
        SegChain chain;
        chain.on = light && seg_wanted(l);
        U32 seg_breaks = 0;
        if (!sSegJobs.empty())
        {
            // The segments recorded since into the frame statistics, before
            // the VKPerf line prints them (in the present) and sets them back.
            seg_fold_done();
        }
        for (U32 i = 0; i < l.draw_count; ++i)
        {
            const DL::Draw& d = l.draws[i];
            if (!valid_draw(l, d))
            {
                ++sCounts.invalid;
                skip("draw list index");
                continue;
            }
            apply_state(*program, l, d, head);
            if (sOIT.active)
            {
                list_globals.oit_priority = d.oit_priority;     // the draw's (oit_priority())
            }
            LLRHI::DirectDraw direct;
            fill_direct(l, d, direct);
            if (LLRHIVBCensus::sOn)     // research/293 branch 0: counting only (a list's draws do not pass drawNative)
            {
                LLRHIVBCensus::drawn(census_owner(direct.vertex_buffer));
            }
            bool drawn = false;
            if (chain.on)
            {
                const SegResult seg = seg_draw(list_head, direct, chain);
                if (seg == SegResult::USUAL)
                {
                    chain.end();
                    ++sSegReport.usual;
                    chain.on = chain.on && ++seg_breaks < MAX_SEG_BREAKS;
                    drawn = list_draw(list_head, direct);
                }
                else
                {
                    drawn = seg == SegResult::TAKEN;
                }
            }
            else
            {
                drawn = light ? list_draw(list_head, direct) : direct_draw(direct);
            }
            sCounts.reference_draws += light ? 0 : 1;
            sCounts.refused += drawn ? 0 : 1;
        }
        chain.end();
        sSegReport.lists += chain.counted ? 1 : 0;
        ++sCounts.lists;
        sCounts.draws += l.draw_count;
        const U32 bucket = l.draw_count < 64 ? 0 : l.draw_count < 256 ? 1 : l.draw_count < 1024 ? 2 : 3;
        ++sCounts.length_lists[bucket];
        sCounts.length_draws[bucket] += l.draw_count;
        count_name(l.name, l.draw_count);
        sCounts.ticks += __rdtsc() - start;
        return true;
    }
}

void LLVKDrawList::install()
{
    if (!LLRHI::sDirect)
    {
        LL_INFOS("VKDrawList") << "draw lists off: direct draws are off (LLRHI_DIRECT=0)" << LL_ENDL;
        return;
    }
    LLRHI::DrawList::sDrawList = &draw_list;
    LLRHI::DrawList::sDrawListReady = &draw_list_ready;
    LLVKRecorder::sJoinFillers = &seg_join;     // fix-threads4
    LL_INFOS("VKDrawList") << "draw lists available: LLRHI_DRAW_LISTS=" << LLRHI::DrawList::sMode << " (opaque "
                           << LLRHI::DrawList::sKind[LLRHI::DrawList::KIND_OPAQUE] << ", alpha "
                           << LLRHI::DrawList::sKind[LLRHI::DrawList::KIND_ALPHA] << ", material "
                           << LLRHI::DrawList::sKind[LLRHI::DrawList::KIND_MATERIAL] << ")" << LL_ENDL;
    // research/137 switch matrix 2: the kinds are off by default, so the mode
    // alone opens no list.
    const bool* kind = LLRHI::DrawList::sKind;
    if (LLRHI::DrawList::sMode && !kind[LLRHI::DrawList::KIND_OPAQUE] && !kind[LLRHI::DrawList::KIND_ALPHA] &&
        !kind[LLRHI::DrawList::KIND_MATERIAL])
    {
        LL_WARNS("VKDrawList") << "LLRHI_DRAW_LISTS=" << LLRHI::DrawList::sMode << " but every kind is 0: no list opens. Set at least one "
                               << "of LLRHI_DRAW_LISTS_OPAQUE / _ALPHA / _MATERIAL to 1" << LL_ENDL;
    }
}

std::string LLVKDrawList::report(U32 frames)
{
    std::lock_guard<LLVKTrackerMutex> lock(sLock);
    frames = llmax(frames, 1u);
    const F64 per = 1.0 / frames;
    const Counts c = sCounts;
    sCounts = Counts();
    // Segments are folded at the next list (draw_list), not here: perfReport
    // has printed the run counters already and sets them back after this.
    const SegReport seg = sSegReport;
    sSegReport = SegReport();
    if (!c.lists && !c.lost && !LLRHI::DrawList::sMode)
    {
        return std::string();
    }
    std::ostringstream out;
    out << std::fixed << std::setprecision(2);
    out << "backend: lists " << c.lists * per << ", draws " << c.draws * per << " (avg " << (c.lists ? (F64)c.draws / c.lists : 0.0)
        << " a list, reference " << c.reference_draws * per << ", lists the light path sent the reference way " << c.head_refused * per
        << "), lost " << c.lost << ", not drawn: refused " << c.refused * per << " invalid " << c.invalid * per << ", palette rows refused "
        << c.palette_refused * per << ", patches refused " << c.patch_refused * per << ", state changed past the hooks "
        << c.leaks * per << ", in a capture " << c.captured * per << "; by length (lists/draws) <64 " << c.length_lists[0] * per << "/"
        << c.length_draws[0] * per << " 64-255 " << c.length_lists[1] * per << "/" << c.length_draws[1] * per << " 256-1023 "
        << c.length_lists[2] * per << "/" << c.length_draws[2] * per << " 1024+ " << c.length_lists[3] * per << "/"
        << c.length_draws[3] * per << "; by name (lists/draws)";
    for (U32 i = 0; i < Counts::NAMES && c.names[i]; ++i)
    {
        out << (i ? ", " : " ") << c.names[i] << " " << c.name_lists[i] * per << "/" << c.name_draws[i] * per;
    }
    out << "; record " << c.ticks / (perf_ticks_per_ns() * 1e6) * per << " ms";
    if (LLRHI::DrawList::sThreads || seg.lists || seg.segments)
    {
        // research/137 N11: the record ms above is the list's thread's (it
        // resolves the segments' draws); the cull threads' recording is here.
        out << "; threads (LLRHI_DRAW_LISTS_THREADS=" << LLRHI::DrawList::sThreads << "): segments: lists " << seg.lists * per
            << ", segments " << seg.segments * per << " (avg " << (seg.segments ? (F64)seg.segment_draws / seg.segments : 0.0)
            << " draws), draws " << seg.segment_draws * per << ", draws the usual way between them " << seg.usual * per
            << ", recorded on the list's thread " << seg.on_main * per << ", lists without a job or stream " << seg.no_job * per
            << ", cull threads' record " << seg.ticks / (perf_ticks_per_ns() * 1e6) * per << " ms on " << seg.thread_count << " threads";
    }
    return out.str();
}

#else // !LL_GL_FUNC_POINTER

void LLVKDrawList::install()
{
}

std::string LLVKDrawList::report(U32)
{
    return std::string();
}

#endif // LL_GL_FUNC_POINTER

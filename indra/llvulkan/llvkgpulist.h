/**
 * @file llvkgpulist.h
 * @brief GPU-driven drawing: the shadow casters' list on the GPU, culled by a compute shader (research/46 G2, G3).
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

#ifndef LL_LLVKGPULIST_H
#define LL_LLVKGPULIST_H

#include "llrhi.h"
#include "llvkgpudriven.h"

#include <vulkan/vulkan.h>

#include <string>

// LLRHI::ShadowList on the GPU (stages G2 and G3 of research/46).
//
// Rows (one opaque caster: an index range of a buffer pair copied into the G1
// geometry pool) and groups (a spatial group's cull bounds while it is
// resident) live in two device-local tables. A row's data is written once,
// when it is added; a group's when it becomes resident. Writes go through the
// frame's upload commands.
//
// Each shadow pass records one compute dispatch into the frame's upload
// commands, which run before every draw of the frame in the order they were
// recorded: the dispatch sees the tables as they were when its pass was
// recorded (later writes come after it, slots are reused only frames after
// they were freed). It writes the pass's VkDrawIndexedIndirectCommands and a
// count per pool buffer and index size ("bucket") into this frame slot's
// scratch buffer; the pass draws them with vkCmdDrawIndexedIndirectCount.
//
// The passes' dispatches are recorded together, between one pair of
// barriers, when the upload commands end (LLVKMirror::sBeforeUploadEnd) or
// before the next table writes, whichever comes first, so each still sees
// the tables as its pass left them (research/98; LLRHI_GPU_LIST_BATCH=0:
// each pass's dispatch right away, between barriers of its own).
//
// Render thread only, under LLVKTracker::lock().
namespace LLVKGPUList
{
    constexpr U32 BUCKETS = LLVKGPUDriven::MAX_CHUNKS * 2;     // pool buffer x index size

    // Installs LLRHI::sShadowList when the device can draw from it.
    void install();

    // A pass's rows, ready to draw: per bucket, its commands start at
    // commands + first * 20 bytes, at most capacity of them, the count at
    // counts + bucket * 4 bytes.
    struct Draws
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize counts = 0;
        VkDeviceSize commands = 0;
        U32 first[BUCKETS] = {};
        U32 capacity[BUCKETS] = {};
        bool any = false;           // some bucket has rows
    };

    // Prepares the pass's compute dispatch (recorded outside any rendering scope:
    // into the upload commands) and the draw matrix rows its commands name. False when it
    // could not: the list is then broken (LLRHI::ShadowList::broken) and stays
    // unused, since the caller already left these rows out of its packets.
    bool prepare(const LLRHI::ShadowListPass& pass, const F32* view_proj, Draws& out);

    // Counters since the last call, for the periodic perf log (empty when unused).
    std::string takeStats(U32 frames);

    // Stage G5: LLRHI::sProbeList (the probe faces' G-buffer list). Nothing is
    // made until it is first used.
    void installProbe();
    std::string takeProbeStats(U32 frames);
}

// What the probe list needs from the GL translation (defined in llvkgl.cpp,
// under its lock; the caller holds LLVKTracker::lock()). Research/46 G5.
namespace LLVKGLProbe
{
    // The draw run the last direct draw went into, while nothing was recorded
    // after that draw: its program (an identity only), pool chunk and vertex format.
    struct Run
    {
        const void* program = nullptr;
        U32 chunk = 0;
        U64 format = 0;
    };
    bool lastRun(Run& out);

    // The words a draw of the bound program (a run's: rows) pushes when it names
    // no texture of its own (every sampler as its unit is bound, the program's
    // matrix, material and palette rows), without the rows word; and per word
    // below `samplers` the texture unit it samples (-1: none), the matrix word.
    struct Words
    {
        static constexpr U32 MAX = 32;
        U32 words[MAX] = {};
        U32 count = 0;
        S32 units[MAX] = {};
        U32 samplers = 0;
        U32 matrix_word = U32_MAX;  // U32_MAX: none
        U32 material_word = U32_MAX;    // the material row's word (U32_MAX: none)
        U32 material_vec4s = 0;         // the program's material row length
        U64 consts_hash = 0;            // the program's constants row as it is (LLVKGLProbe::Capture::consts_hash)
    };
    bool templateWords(Words& out);

    // A draw matrix row for the bound program with the draw's matrices, as a
    // direct draw of it would get. False: the frame's matrix table is full.
    bool matrixRow(const LLRHI::DirectDraw& matrices, U32& row);

    // The bindless slot a direct draw naming this texture (GL name) would push
    // into the last run's target; readable false when the texture is not in
    // shader read layout (such a draw would move it there first).
    U32 textureSlot(U32 name, bool& readable);
    // Writes the slots handed out into the frame's set.
    void flushSlots();

    // Commands recorded so far (any command buffer of the frame).
    U64 commands();
    // Commands recorded since `before` went into the frame's upload commands
    // only (they run before all of the frame's draws): the last run still has
    // nothing after its last draw.
    void forgiveUploads(U64 before);

    // The run's other draws: an indirect draw of `count` commands at offset in
    // buffer with the run's bindings, after its last draw. False: the run
    // changed (nothing recorded).
    bool drawIndirect(const Run& run, VkBuffer buffer, VkDeviceSize offset, U32 count);

    // A draw's vertex format as draw runs key it.
    U64 drawFormat(const LLRHI::DirectDraw& draw);

    // --- Research/50 step 4 ---------------------------------------------------
    // Programs were linked or deleted since (a captured program may be stale).
    U32 programChanges();

    // Capture: while on, direct draws record nothing; each leaves what it would
    // have pushed and drawn from (LLRHI::ProbeList::beginCapture).
    struct Capture
    {
        static constexpr U32 UNITS = 8;
        static constexpr U32 MAX_STREAMS = 16;
        static constexpr U32 MAX_MATERIAL_VEC4S = 32;
        bool ok = false;
        const char* why = nullptr;          // !ok: the reason
        const void* program = nullptr;      // an identity only
        U32 program_changes = 0;
        U64 format = 0;                     // drawFormat
        U32 state = 0;                      // the render state it was drawn with (a hash)
        U32 vertex_buffer = 0;              // GL name (the pool pair)
        U32 chunk = 0;                      // pool placement
        S32 vertex_offset = 0;
        U32 first_index = 0;                // in the chunk's index buffer
        U32 index_count = 0;
        U32 names[UNITS] = {};              // GL texture name per unit its samplers read
        U32 sampled = 0;                    // units its samplers read (bit per unit)
        U32 streams = 0;                    // pool streams it reads (location, stride)
        U32 stream_location[MAX_STREAMS] = {};
        U32 stream_stride[MAX_STREAMS] = {};
        // research/293 branch 1: the pool stream each reads (its key; the location without the switch) and where in it
        U32 stream_key[MAX_STREAMS] = {};
        U32 stream_shift[MAX_STREAMS] = {};
        U32 material_vec4s = 0;
        F32 material[MAX_MATERIAL_VEC4S * 4] = {};
        // The program's constants row then: the list writes the face's first
        // draw's, so a block's draws must all have had the one it draws with.
        U64 consts_hash = 0;
    };
    void beginCapture();
    void endCapture();
    U32 captureCount();
    const Capture* capture(U32 index);

    // The last run's pipeline for device generated commands (its program,
    // vertex inputs, target and state, with dynamic vertex strides), made on
    // first use, and the inputs that read pool streams: binding, location and
    // stride each. False: the run is not open as it was, or no such pipeline.
    struct Streams
    {
        static constexpr U32 MAX = 8;       // LLVKDevice::GeneratedCommands::MAX_VERTEX_TOKENS
        U32 count = 0;
        U32 binding[MAX] = {};
        U32 location[MAX] = {};
        U32 stride[MAX] = {};
    };
    bool generatedPipeline(const Run& run, VkPipeline& pipeline, Streams& streams);
    // Binds that pipeline and the run's vertex buffers (with their strides)
    // and records info's commands (VkGeneratedCommandsPipelineInfoEXT added)
    // after the run's last draw. The command buffer's bindings are then
    // unknown: the run ends and the next draw binds everything again.
    bool executeGenerated(const Run& run, VkPipeline pipeline, const VkGeneratedCommandsInfoEXT& info);
}

#endif // LL_LLVKGPULIST_H

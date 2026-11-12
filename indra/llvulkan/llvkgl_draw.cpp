/**
 * @file llvkgl_draw.cpp
 * @brief Draws: vertex attributes and inputs, index conversions, draw_one (the full path), direct draws, captures and clears (research/91).
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
#include "llvkpassstate.h"  // research/204 10 (vk-s4c2): LLRHI_PASS_UNITS' verify

#if LL_GL_FUNC_POINTER

namespace llvkgl
{
    // ------------------------------------------------------------------
    // Vertex attributes (single VAO in the viewer; per context)

    Buffer* element_buffer()
    {
        if (!sElementBuffer)
        {
            return nullptr;
        }
        if (sElementResolvedName != sElementBuffer || sElementResolvedAt != sBufferDeletes)
        {
            sElementResolved = sBuffers.find(sElementBuffer);
            sElementResolvedName = sElementBuffer;
            sElementResolvedAt = sBufferDeletes;
        }
        return sElementResolved;
    }

    Buffer* attrib_buffer(Attrib& a)
    {
        if (!a.buffer)
        {
            return nullptr;
        }
        if (a.resolved_at != sBufferDeletes)
        {
            a.resolved = sBuffers.find(a.buffer);
            a.resolved_at = sBufferDeletes;
        }
        return a.resolved;
    }

    thread_local Attrib sAttribs[32];

    VkFormat attrib_format(const Attrib& a)
    {
        const U32 n = (U32)llclamp(a.size, 1, 4) - 1;
        switch (a.type)
        {
        case GL_FLOAT:
        {
            static const VkFormat f[] = { VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT };
            return f[n];
        }
        case GL_UNSIGNED_BYTE:
        {
            static const VkFormat unorm[] = { VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8A8_UNORM };
            static const VkFormat uint[] = { VK_FORMAT_R8_UINT, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8B8_UINT, VK_FORMAT_R8G8B8A8_UINT };
            static const VkFormat scaled[] = { VK_FORMAT_R8_USCALED, VK_FORMAT_R8G8_USCALED, VK_FORMAT_R8G8B8_USCALED, VK_FORMAT_R8G8B8A8_USCALED };
            return a.integer ? uint[n] : a.normalized ? unorm[n] : scaled[n];
        }
        case GL_UNSIGNED_SHORT:
        {
            static const VkFormat unorm[] = { VK_FORMAT_R16_UNORM, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16B16_UNORM, VK_FORMAT_R16G16B16A16_UNORM };
            static const VkFormat uint[] = { VK_FORMAT_R16_UINT, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16B16_UINT, VK_FORMAT_R16G16B16A16_UINT };
            static const VkFormat scaled[] = { VK_FORMAT_R16_USCALED, VK_FORMAT_R16G16_USCALED, VK_FORMAT_R16G16B16_USCALED, VK_FORMAT_R16G16B16A16_USCALED };
            return a.integer ? uint[n] : a.normalized ? unorm[n] : scaled[n];
        }
        case GL_UNSIGNED_INT:
        {
            static const VkFormat uint[] = { VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32A32_UINT };
            return uint[n];
        }
        case GL_INT:
        {
            static const VkFormat sint[] = { VK_FORMAT_R32_SINT, VK_FORMAT_R32G32_SINT, VK_FORMAT_R32G32B32_SINT, VK_FORMAT_R32G32B32A32_SINT };
            return sint[n];
        }
        default:
            return VK_FORMAT_UNDEFINED;
        }
    }
namespace
{

    // Vulkan requires the attribute format signedness to match the shader input.
    // Assumed (fix-round15 9): only scalar int / uint inputs are matched here;
    // the viewer's ivecN / uvecN inputs are fed with the signedness they are
    // declared with, so GL_INT_VECn / GL_UNSIGNED_INT_VECn are left as given.
    VkFormat match_signedness(VkFormat format, U32 gl_type)
    {
        static const std::pair<VkFormat, VkFormat> pairs[] =
        {
            { VK_FORMAT_R8_UINT, VK_FORMAT_R8_SINT }, { VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8_SINT },
            { VK_FORMAT_R8G8B8_UINT, VK_FORMAT_R8G8B8_SINT }, { VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_SINT },
            { VK_FORMAT_R16_UINT, VK_FORMAT_R16_SINT }, { VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16_SINT },
            { VK_FORMAT_R16G16B16_UINT, VK_FORMAT_R16G16B16_SINT }, { VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_R16G16B16A16_SINT },
            { VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT }, { VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32_SINT },
            { VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32_SINT }, { VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_SINT },
        };
        for (const auto& p : pairs)
        {
            if (gl_type == GL_INT && format == p.first) return p.second;
            if (gl_type == GL_UNSIGNED_INT && format == p.second) return p.first;
        }
        return format;
    }

    bool is_integer_input(U32 gl_type)
    {
        switch (gl_type)
        {
        case GL_INT: case GL_INT_VEC2: case GL_INT_VEC3: case GL_INT_VEC4:
        case GL_UNSIGNED_INT: case GL_UNSIGNED_INT_VEC2: case GL_UNSIGNED_INT_VEC3: case GL_UNSIGNED_INT_VEC4:
            return true;
        default:
            return false;
        }
    }
} // namespace

    // A triangle fan redrawn as a list, or a line loop as a strip: its indices
    // in the frame's upload ring. Set for the one draw (draw(), FanIndexScope
    // clears it), read where index buffers are resolved.
    thread_local VkBuffer tFanIndexBuffer = VK_NULL_HANDLE;
namespace
{
    thread_local std::vector<U32> tFanIndices;
} // namespace
    U32 sFansConverted = 0;
namespace
{

    // tOITDraw for one draw_one (every return).
    struct OITDrawScope
    {
        explicit OITDrawScope(U8 mode) { tOITDraw = mode; }
        ~OITDrawScope() { tOITDraw = 0; }
    };

    std::vector<VertexInput> tInputs;
} // namespace
    std::vector<VkBuffer> tVertexBuffers;
    std::vector<VkDeviceSize> tVertexOffsets;
namespace
{

    // What tInputs / tVertexBuffers / tVertexOffsets were last resolved for.
    // Consecutive draws from one vertex buffer (the batches of one group) find
    // nothing changed and reuse them.
    struct VertexInputsFor
    {
        const void* program = nullptr;
        U32 program_changes = U32_MAX;
        U32 vertex_state = U32_MAX;
        U32 buffer_deletes = U32_MAX;
        U32 frame = U32_MAX;            // gpu_buffer and the constant ring are per frame
        U64 signature = 0;
    };
    VertexInputsFor tInputsFor;
} // namespace
    U32 sVertexInputsReused = 0;
namespace
{

    // Ring copies of disabled attributes' constant values, shared by the draws of a
    // frame: a fresh copy per draw would also rebind the vertex buffers every draw.
    struct ConstantAttribute
    {
        F32 value[4];
        VkBuffer buffer;
        VkDeviceSize offset;
    };
    std::vector<ConstantAttribute> sConstantAttributes;
    U32 sConstantAttributesFrame = U32_MAX;

    // value null: integer inputs, which read zero.
    bool constant_attribute(const F32* value, VkBuffer& buffer, VkDeviceSize& offset)
    {
        static const F32 zero[4] = {};
        const F32* v = value ? value : zero;
        if (sConstantAttributesFrame != sFrame)
        {
            sConstantAttributes.clear();
            sConstantAttributesFrame = sFrame;
        }
        for (const ConstantAttribute& c : sConstantAttributes)
        {
            if (same_words_at(c.value, v, 4))
            {
                buffer = c.buffer;
                offset = c.offset;
                return true;
            }
        }
        void* dst = nullptr;
        if (!LLVKMirror::instance().allocate(16, 16, buffer, offset, dst))
        {
            return false;
        }
        memcpy(dst, v, 16);
        if (sConstantAttributes.size() < 64)
        {
            ConstantAttribute c;
            memcpy(c.value, v, sizeof(c.value));
            c.buffer = buffer;
            c.offset = offset;
            sConstantAttributes.push_back(c);
        }
        return true;
    }

    void record_draw()
    {
        if (!LLVKDevice::instance().checkpointsEnabled())
        {
            return;
        }
        if (!sDrawLog.empty() && sDrawLog.front().frame + 3 < sFrame)
        {
            sDrawLog.erase(std::remove_if(sDrawLog.begin(), sDrawLog.end(), [](const DrawRecord& r) { return r.frame + 3 < sFrame; }),
                           sDrawLog.end());
        }
        DrawRecord r = {};
        r.frame = sFrame;
        r.draw = sDrawsReplayed;
        r.program = sCurrentProgram;
        r.count = llmin(tSlotCount, 4u);
        for (U32 i = 0; i < r.count; ++i)
        {
            r.slots[i] = tSlots[i];
            r.names[i] = tSlotNames[i];
            r.views[i] = tSlotViews[i];
        }
        sDrawLog.push_back(r);
    }

} // namespace
    void report_lost_draws(const std::vector<U64>& markers)
    {
        std::ostringstream out;
        out << "draws at the GPU fault (current frame " << sFrame << "):";
        for (U64 marker : markers)
        {
            const GLuint program = (GLuint)(marker >> 32);
            const U32 draw = (U32)(marker & 0xffffffffu);
            if (program == 0xffffffffu)
            {
                out << "\n  native shadow draw " << draw;
                continue;
            }
            if (program == LLVKTexCompress::CHECKPOINT_BEGIN || program == LLVKTexCompress::CHECKPOINT_END)
            {
                // bc7-fault: draw is the GL name of the texture.
                out << "\n  BC7 encoding (research/88) of texture " << draw
                    << (program == LLVKTexCompress::CHECKPOINT_BEGIN ? ": begun" : ": recorded to its end");
                continue;
            }
            out << "\n  marker: program " << program << " draw " << draw << (sDepthOnlyPrograms.count(program) ? " (depth-only path)" : "");
            for (const DrawRecord& r : sDrawLog)
            {
                if (r.program != program || r.draw != draw)
                {
                    continue;
                }
                out << "\n  frame " << r.frame << " program " << r.program << " draw " << r.draw << ":";
                for (U32 i = 0; i < r.count; ++i)
                {
                    auto destroyed = sDestroyedViews.find(r.views[i]);
                    out << "\n    slot " << r.slots[i] << " texture " << r.names[i] << " view " << (void*)r.views[i]
                        << (destroyed != sDestroyedViews.end() ? " RELEASED in frame " + std::to_string(destroyed->second) : std::string())
                        << "; " << LLVKBindless::describe(r.slots[i]);
                }
            }
        }
        LL_WARNS("Vulkan") << out.str() << LL_ENDL;
    }
    thread_local DirectInputs tDirectInputs;
    U32 sDirectInputsReused = 0;

    // Fills tVertexBuffers / tVertexOffsets for tDirect; false: the draw cannot be recorded.
    bool direct_inputs(Program& program, U64& signature)
    {
        const LLRHI::DirectDraw& d = *tDirect;
        const U64 format = direct_attributes_hash(d);
        DirectInputs& f = tDirectInputs;
        if (f.program == &program && f.program_changes == sProgramChanges && f.constants == sAttribConstants && f.frame == sFrame &&
            f.format == format)
        {
            ++sDirectInputsReused;
        }
        else
        {
            PerfScope scope(PERF_VERTEX);
            // Invalid until complete; the vectors keep their storage (no allocations per rebuild).
            f.program = nullptr;
            f.program_changes = f.constants = f.frame = U32_MAX;
            f.format = 0;
            f.signature = 0;
            f.needs_buffer = false;
            f.inputs.clear();
            f.source.clear();
            f.constant_buffers.clear();
            f.constant_offsets.clear();
            U64 sig = LLRHI::FNV1A_BASIS_SHORT;
            std::vector<VertexInput>& inputs = f.inputs;
            std::vector<S32>& source = f.source;
            std::vector<VkBuffer>& constant_buffers = f.constant_buffers;
            std::vector<VkDeviceSize>& constant_offsets = f.constant_offsets;
            bool needs_buffer = false;
            for (size_t i = 0; i < program.vk.inputs.size(); ++i)
            {
                const auto& input = program.vk.inputs[i];
                const S32 attrib_index = program.input_attribs[i];
                S32 k_found = -1;
                for (U32 k = 0; attrib_index >= 0 && k < d.attribute_count; ++k)
                {
                    if (d.attributes[k].location == (U32)attrib_index)
                    {
                        k_found = (S32)k;
                        break;
                    }
                }
                if (k_found >= 0)
                {
                    const LLRHI::DirectDraw::Attribute& s = d.attributes[k_found];
                    Attrib a;
                    a.enabled = true;
                    a.size = (GLint)s.size;
                    a.type = s.type;
                    a.normalized = s.normalized;
                    a.integer = s.integer;
                    a.stride = (GLsizei)s.stride;
                    VkFormat vk_format = match_signedness(attrib_format(a), input.gl_type);
                    if (vk_format == VK_FORMAT_UNDEFINED)
                    {
                        skip("vertex attribute type");
                        return false;
                    }
                    U32 tight = (U32)llclamp(a.size, 1, 4) * (a.type == GL_FLOAT || a.type == GL_UNSIGNED_INT || a.type == GL_INT ? 4 :
                                                             a.type == GL_UNSIGNED_SHORT ? 2 : 1);
                    inputs.push_back({ input.location, vk_format, a.stride ? (U32)a.stride : tight });
                    sig = LLRHI::fnv1aMix(sig, (U64)vk_format << 32 | inputs.back().stride);
                    source.push_back(k_found);
                    constant_buffers.push_back(VK_NULL_HANDLE);
                    constant_offsets.push_back(0);
                    needs_buffer = true;
                }
                else
                {
                    // Not in the vertex buffer: GL supplies the attribute's current constant.
                    const bool integer = is_integer_input(input.gl_type);
                    const Attrib* a = attrib_index >= 0 && attrib_index < 32 ? &sAttribs[attrib_index] : nullptr;
                    VkBuffer buffer = VK_NULL_HANDLE;
                    VkDeviceSize offset = 0;
                    if (!constant_attribute(integer ? nullptr : (a ? a->constant : Attrib().constant), buffer, offset))
                    {
                        return false;
                    }
                    // Assumed (fix-round15 9): an ivecN input is never left without a buffer
                    // (only the scalar GL_INT takes SINT; the vectors would take UINT).
                    VkFormat vk_format = !integer ? VK_FORMAT_R32G32B32A32_SFLOAT :
                                         input.gl_type == GL_INT ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_UINT;
                    inputs.push_back({ input.location, vk_format, 0 });
                    sig = LLRHI::fnv1aMix(sig, (U64)vk_format << 32);
                    source.push_back(-1);
                    constant_buffers.push_back(buffer);
                    constant_offsets.push_back(offset);
                }
            }
            f.needs_buffer = needs_buffer;
            f.signature = sig;
            f.program = &program;
            f.program_changes = sProgramChanges;
            f.constants = sAttribConstants;
            f.frame = sFrame;
            f.format = format;
        }

        // Per draw: the one vertex buffer every attribute comes from, and the offsets.
        VkBuffer vertex_buffer = VK_NULL_HANDLE;
        if (f.needs_buffer)
        {
            // research/137 N14: a draw from the immediate ring brings its Vulkan buffer.
            vertex_buffer = tDirectVertexBuffer ? tDirectVertexBuffer : gpu_buffer_named(d.vertex_buffer);
            if (!vertex_buffer)
            {
                skip("vertex buffer");
                return false;
            }
        }
        const size_t n = f.source.size();
        tVertexBuffers.resize(n);
        tVertexOffsets.resize(n);
        for (size_t i = 0; i < n; ++i)
        {
            const S32 k = f.source[i];
            tVertexBuffers[i] = k >= 0 ? vertex_buffer : f.constant_buffers[i];
            tVertexOffsets[i] = k >= 0 ? (VkDeviceSize)d.attributes[k].offset : f.constant_offsets[i];
        }
        tInputsFor = VertexInputsFor();     // tVertexBuffers no longer hold the GL-state inputs
        signature = f.signature;
        return true;
    }
namespace
{

    // Triangle fans become lists (LLRHI_FAN_LIST=0 keeps them): GL draws the
    // occlusion boxes as fans, and the GPU faulted whenever they were drawn
    // (research/37). index becomes 32-bit indices in the frame's upload ring.
    const bool sFanToList = LLRHI::envFirstChar("LLRHI_FAN_LIST") != '0';

    // The vertex numbers a draw reads, in order: its indices, or first.. for an
    // array draw. False when the indices cannot be read.
    bool source_indices(GLint first, GLsizei count, const IndexInfo& index, std::vector<U32>& source)
    {
        if (index.indexed)
        {
            Buffer* buffer = tDirect ? sBuffers.find(tDirect->index_buffer) : element_buffer();
            if (LLRHIVBCensus::sOn && buffer && buffer->owner_number)   // research/293 branch 0: the CPU copy of the indices is read
            {
                LLRHIVBCensus::nameRead(LLRHIVBCensus::READ_FIND, buffer->owner_number);
            }
            const size_t size = index.type == GL_UNSIGNED_INT ? 4 : index.type == GL_UNSIGNED_SHORT ? 2 : 0;
            if (!buffer || !size || index.offset + (size_t)count * size > buffer->data.size())
            {
                return false;
            }
            const U8* data = buffer->data.data() + index.offset;
            for (GLsizei i = 0; i < count; ++i)
            {
                source.push_back(size == 4 ? ((const U32*)data)[i] : ((const U16*)data)[i]);
            }
        }
        else
        {
            for (GLsizei i = 0; i < count; ++i)
            {
                source.push_back((U32)first + (U32)i);
            }
        }
        return true;
    }

    // n 32-bit indices into the frame's upload ring; index then draws them
    // (with first 0: they are absolute vertex numbers).
    bool ring_indices(const U32* indices, size_t n, IndexInfo& index)
    {
        VkBuffer ring = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
        void* dst = nullptr;
        if (!n || !LLVKMirror::instance().allocate(n * 4, 4, ring, offset, dst))
        {
            return false;
        }
        memcpy(dst, indices, n * 4);
        tFanIndexBuffer = ring;
        index.indexed = true;
        index.type = GL_UNSIGNED_INT;
        index.offset = (size_t)offset;
        return true;
    }

    bool fan_to_list(GLint first, GLsizei count, IndexInfo& index)
    {
        tFanIndices.clear();
        std::vector<U32>& source = tFanIndices;   // reused: fan order first, then the list
        if (!source_indices(first, count, index, source))
        {
            return false;
        }
        const size_t fan = source.size();
        for (size_t i = 1; i + 1 < fan; ++i)
        {
            source.push_back(source[0]);
            source.push_back(source[i]);
            source.push_back(source[i + 1]);
        }
        if (!ring_indices(source.data() + fan, source.size() - fan, index))
        {
            return false;
        }
        ++sFansConverted;
        return true;
    }

} // namespace
    // Line loops become strips that end where they began (Vulkan has no loops):
    // the UI's outlined triangles and circles (gl_triangle_2d, gl_circle_2d).
    U32 sLoopsConverted = 0;
namespace
{

    bool loop_to_strip(GLint first, GLsizei count, IndexInfo& index)
    {
        tFanIndices.clear();
        std::vector<U32>& source = tFanIndices;
        if (!source_indices(first, count, index, source) || source.empty())
        {
            return false;
        }
        source.push_back(source[0]);
        if (!ring_indices(source.data(), source.size(), index))
        {
            return false;
        }
        ++sLoopsConverted;
        return true;
    }

    void draw_one(GLenum mode, GLint first, GLsizei count, GLsizei instances, const IndexInfo& index);

} // namespace
    CaptureState sCapture;

    void draw(GLenum mode, GLint first, GLsizei count, GLsizei instances, const IndexInfo& index_in)
    {
        if (sCapture.on)
        {
            LLVKGLProbe::Capture c;
            c.why = "a draw through GL";
            sCapture.draws.push_back(c);
            return;
        }
        if (mode == GL_TRIANGLE_FAN && sFanToList && count >= 3 && !suppress_translation() &&
            LLVKMirror::instance().isRecording())
        {
            IndexInfo index = index_in;
            const FanIndexScope fan_scope;
            if (!fan_to_list(first, count, index))
            {
                skip("fan indices");
                return;
            }
            const GLsizei list_count = (GLsizei)(3 * (count - 2));
            draw_one(GL_TRIANGLES, 0, list_count, instances, index);
            return;
        }
        if (mode == GL_LINE_LOOP && count == 1)
        {
            return;     // GL draws nothing for a single vertex
        }
        if (mode == GL_LINE_LOOP && count >= 2 && !suppress_translation() && LLVKMirror::instance().isRecording())
        {
            IndexInfo index = index_in;
            const FanIndexScope loop_scope;
            if (!loop_to_strip(first, count, index))
            {
                skip("line loop indices");
                return;
            }
            draw_one(GL_LINE_STRIP, 0, count + 1, instances, index);
            return;
        }
        draw_one(mode, first, count, instances, index_in);
    }

    // Security audit B 4: a draw's indices lie in its index buffer (without
    // robustBufferAccess the GPU would read past it). One compare a draw.
    bool index_range_fits(const Buffer* buffer, GLenum type, size_t offset, GLsizei count)
    {
        const size_t bytes = buffer ? buffer->data.size() : 0;
        const U64 size = type == GL_UNSIGNED_INT ? 4 : 2;
        return count >= 0 && offset <= bytes && (U64)count * size <= (U64)(bytes - offset);
    }

    // Binds the program's set (set 1) with its default block at dynamic_offset,
    // unless that is what is bound; the bindless table (set 0) too when it is not.
    // No set (a program on the shared layout, research/50 step 3): only the
    // table, when it is not bound; whatever set 1 has stays (unused).
    void bind_program_set(VkCommandBuffer cmd, const Program& program, VkDescriptorSet set, bool has_dynamic, U32 dynamic_offset)
    {
        bind_program_set(cmd, sRecorded, program.layout, LLVKBindless::set(), set, has_dynamic, dynamic_offset);
    }

    void bind_program_set(VkCommandBuffer cmd, Recorded& r, VkPipelineLayout layout, VkDescriptorSet table, VkDescriptorSet set,
                          bool has_dynamic, U32 dynamic_offset)
    {
        if (!set)
        {
            if (!table_at_set0(r))
            {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &table, 0, nullptr);
                r.table_bound = true;
                r.table_only = true;
                r.set = VK_NULL_HANDLE;
                r.dynamic_offset = 0;
                r.oit_compatible += layout == r.oit_program_layout ? 1 : 0;
            }
            return;
        }
        if (set == r.set && r.table_bound && (!has_dynamic || dynamic_offset == r.dynamic_offset))
        {
            return;
        }
        if (table_at_set0(r))
        {
            // Only the program's set: rebinding the large table each draw cost the driver.
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1, &set,
                                    has_dynamic ? 1 : 0, has_dynamic ? &dynamic_offset : nullptr);
        }
        else
        {
            const VkDescriptorSet sets[2] = { table, set };
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 2, sets,
                                    has_dynamic ? 1 : 0, has_dynamic ? &dynamic_offset : nullptr);
            r.table_bound = true;
        }
        // The same program's layout again leaves an OIT set 2 bound (oit_bind_set).
        r.oit_compatible += layout == r.oit_program_layout ? 1 : 0;
        r.set = set;
        r.table_only = false;
        r.dynamic_offset = has_dynamic ? dynamic_offset : 0;
    }

    // The per-draw commands of a draw whose pipeline, sets and dynamic state are
    // bound: push constants, vertex and index buffers (each only when it differs
    // from what is bound) and the draw.
    void record_draw_commands(VkCommandBuffer cmd, const Program& program, GLint first, GLsizei count, GLsizei instances,
                              const IndexInfo& index, VkBuffer index_buffer, VkIndexType index_type)
    {
        Recorded& r = sRecorded;
        if (tSlotCount && (tSlotCount > r.slot_count || !same_words(tSlots, r.slots, tSlotCount)))
        {
            // Every translated layout declares the same range, so pushes survive pipeline changes.
            const VkPushConstantRange range = LLVKBindless::pushConstantRange();
            vkCmdPushConstants(cmd, program.layout, range.stageFlags, 0, tSlotCount * 4, tSlots);
            memcpy(r.slots, tSlots, tSlotCount * 4);
            r.slot_count = llmax(r.slot_count, tSlotCount);
        }
        if (!tVertexBuffers.empty() && (!same_elements(tVertexBuffers, r.vertex_buffers) || !same_elements(tVertexOffsets, r.vertex_offsets)))
        {
            vkCmdBindVertexBuffers(cmd, 0, (U32)tVertexBuffers.size(), tVertexBuffers.data(), tVertexOffsets.data());
            r.vertex_buffers = tVertexBuffers;
            r.vertex_offsets = tVertexOffsets;
        }
        if (index.indexed)
        {
            if (index_buffer != r.index_buffer || index_type != r.index_type || r.index_offset != 0)
            {
                vkCmdBindIndexBuffer(cmd, index_buffer, 0, index_type);
                r.index_buffer = index_buffer;
                r.index_type = index_type;
                r.index_offset = 0;
            }
            U32 first_index = (U32)(index.offset / (index_type == VK_INDEX_TYPE_UINT32 ? 4 : 2));
            record_draw();
            LLVKDevice::instance().checkpoint(cmd, ((U64)sCurrentProgram << 32) | sDrawsReplayed);
            // The alpha census (research/54): a query around the draw the viewer named.
            const U32 census = census_tag() ? LLVKMirror::instance().censusBegin(cmd, census_tag()) : U32_MAX;
            vkCmdDrawIndexed(cmd, (U32)count, (U32)instances, first_index, 0, 0);
            if (census != U32_MAX)
            {
                LLVKMirror::instance().censusEnd(cmd, census);     // no call when no query was begun (the usual draw)
            }
        }
        else
        {
            record_draw();
            LLVKDevice::instance().checkpoint(cmd, ((U64)sCurrentProgram << 32) | sDrawsReplayed);
            const U32 census = census_tag() ? LLVKMirror::instance().censusBegin(cmd, census_tag()) : U32_MAX;
            vkCmdDraw(cmd, (U32)count, (U32)instances, (U32)first, 0);
            if (census != U32_MAX)
            {
                LLVKMirror::instance().censusEnd(cmd, census);     // no call when no query was begun (the usual draw)
            }
        }
        ++sDrawsReplayed;
    }

    // research/137 N7: draw_one's pass cache hit, in two halves.
    PassHitResult pass_hit_resolve(Program& program, ResolvedTarget& target, PassHit hit, PassHitDraw& out)
    {
        PassCache& p = *program.pass;
        PerfScope scope(PERF_DESCRIPTORS);
        gather_slots(program, target);
        if (tPaletteFull)
        {
            // Never drawn with another palette: the table grows for the next frame.
            skip("palette table full");
            ++sPaletteSkips;
            return PASS_HIT_DROP;
        }
        // The draw's own textures must be readable as they are: a barrier
        // ends the rendering scope, which the full path handles.
        bool usable = true;
        for (const Texture* tex : tBindlessSampled)
        {
            usable &= tex->layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        scope.stop();
        U32 dynamic_offset = p.dynamic_offset;
        if (usable && hit == PASS_HIT_UNIFORMS && p.has_dynamic)
        {
            // Only the default block changed: its new copy, read through the
            // same set (a dynamic uniform buffer) at its own offset. The set's
            // descriptor names the ring buffer: the copy must be in it.
            PerfScope ubo_scope(PERF_UBO);
            VkBuffer buffer = VK_NULL_HANDLE;
            VkDeviceSize offset = 0;
            usable = default_block_copy(program, default_block_size(program), buffer, offset) && buffer == p.dynamic_buffer;
            dynamic_offset = (U32)offset;
        }
        if (!usable)
        {
            return PASS_HIT_FULL_PATH;
        }
        if (hit == PASS_HIT_UNIFORMS)
        {
            p.uniform_version = program.uniform_version;
            p.dynamic_offset = dynamic_offset;
            ++sPassUniformHits;
        }
        out.pipeline = p.pipeline;
        out.set = p.set;
        out.has_dynamic = p.has_dynamic;
        out.dynamic_offset = p.dynamic_offset;
        out.depth_read_only = p.depth_read_only;
        return PASS_HIT_RECORD;
    }

    void pass_hit_record(Program& program, ResolvedTarget& target, const PassHitDraw& h, const OITVariant* oit, U8 oit_mode,
                         VkPrimitiveTopology prim, GLint first, GLsizei count, GLsizei instances, const IndexInfo& index,
                         VkBuffer index_buffer, VkIndexType index_type)
    {
        PerfScope record_scope(PERF_RECORD);
        target.target.depth_read_only = h.depth_read_only;
        // Still the same rendering scope (pass_hit: mScopeEpoch): no new scope begins.
        VkCommandBuffer cmd = LLVKMirror::instance().beginRendering(target.target);
        Recorded& r = sRecorded;
        if (r.pipeline != h.pipeline || (h.set ? r.set != h.set : !table_at_set0(r)))
        {
            ++sPassRebinds;     // another program's draws came in between
        }
        if (r.pipeline != h.pipeline)
        {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, h.pipeline);
            r.pipeline = h.pipeline;
        }
        bind_program_set(cmd, program, h.set, h.has_dynamic, h.dynamic_offset);
        if (oit)
        {
            oit_bind_set(cmd, program, *oit);
        }
        set_dynamic_state(cmd, target.target, prim);    // viewport / scissor may differ (same_draw_state)
        // Node draws never join draw runs; split draws may (their set 2 and priority stay bound),
        // unless their variant reads the priority from the row (research/205 3a-2: a run's rows have none).
        if (oit_mode == OIT_DRAW_NODES || (oit && oit->row_priority_word != U32_MAX) ||
            !run_draw(cmd, program, target, prim, count, instances, index, false))
        {
            record_draw_commands(cmd, program, first, count, instances, index, index_buffer, index_type);
        }
        ++sPassHits;
    }
namespace
{

    void draw_into_target(Program& program, ResolvedTarget& target, GLenum mode, GLint first, GLsizei count, GLsizei instances,
                          const IndexInfo& index);

    void draw_one(GLenum mode, GLint first, GLsizei count, GLsizei instances, const IndexInfo& index)
    {
        if (suppress_translation())
        {
            return;     // the native path drew this already
        }
        {
            // Draws that write nothing outside an occlusion query (its boxes have
            // color and depth writes off) leave no trace in the frame.
            const State& s = sState;
            const bool writes_color = s.color_mask[0] || s.color_mask[1] || s.color_mask[2] || s.color_mask[3];
            const bool writes_depth = s.depth_test && s.depth_write;
            const bool writes_stencil = s.stencil && (s.stencil_front.write_mask || s.stencil_back.write_mask);
            if (!writes_color && !writes_depth && !writes_stencil && !sActiveQuery)
            {
                ++sInvisibleDraws;
                return;
            }
        }
        LLVKMirror& mirror = LLVKMirror::instance();
        if (!mirror.isRecording())
        {
            skip("no frame");
            return;
        }
        if (count <= 0 || instances <= 0)
        {
            return;
        }
        if (mode == GL_LINE_LOOP || mode == GL_QUADS)
        {
            skip("primitive mode");
            return;
        }
        PerfScope total(PERF_DRAW);
        Program* current = current_program();
        if (!current || !current->built)
        {
            skip(!current ? "no program" : "program not built (binary cache?)");
            return;
        }
        Program& program = *current;
        if (sRecorded.frame != sFrame)
        {
            sRecorded = Recorded();
            sRecorded.frame = sFrame;
            pass_invalidate();
        }
        if (sActiveQuery)
        {
            // LLRHI_OCCLUSION_MAX (for finding faults): at most that many boxes a frame; the rest read visible.
            static const U32 max_boxes = []() { U64 v = U32_MAX; LLRHI::envNumber("LLRHI_OCCLUSION_MAX", 0, U32_MAX, v); return (U32)v; }();
            static U32 boxes_frame = U32_MAX, boxes = 0;
            if (boxes_frame != sFrame)
            {
                boxes_frame = sFrame;
                boxes = 0;
            }
            if (++boxes > max_boxes)
            {
                if (GLQuery* q = gl_query(sActiveQuery))
                {
                    q->unmarked = true;
                }
                return;
            }
        }
        if (sActiveQuery && sOcclusionMode != 3)
        {
            // LLRHI_OCCLUSION_SKIP (for finding faults): bit 1 leaves out the sun
            // shadow cameras' boxes, bit 2 the reflection probes'. Their queries read visible.
            static const U32 skip = []() { U64 v = 0; LLRHI::envNumber("LLRHI_OCCLUSION_SKIP", 0, U32_MAX, v); return (U32)v; }();
            const U32 scope = occlusion_scope();
            if (scope && (skip & (1u << (scope - 1))))
            {
                if (GLQuery* q = gl_query(sActiveQuery))
                {
                    q->unmarked = true;
                }
                return;
            }
            // An occlusion box: marks its query's word where it passes the depth test.
            const U32 word = visibility_word();
            if (word != U32_MAX && draw_depth_only(program, mode, first, count, instances, index, word))
            {
                return;
            }
            if (GLQuery* q = gl_query(sActiveQuery))
            {
                q->unmarked = true;     // reads visible; the draw itself goes the generic way
            }
            ++sQueriesUnmarked;
        }
        else if (depth_only_pass() && draw_depth_only(program, mode, first, count, instances, index))
        {
            return;
        }
        else if (!depth_only_pass() && is_depth_only_program(program))
        {
            ++sDepthOnlyFailures["outside the pass"];
        }
        ++program.generic_draws;

        ResolvedTarget target;
        if (!resolve_draw_target(target))
        {
            skip("incomplete render target");
            return;
        }
        draw_into_target(program, target, mode, first, count, instances, index);
    }

    // The rest of draw_one, once the program and the draw target are known
    // (research/137 N7: a draw list's draws come here with its head's).
    void draw_into_target(Program& program, ResolvedTarget& target, GLenum mode, GLint first, GLsizei count, GLsizei instances,
                          const IndexInfo& index)
    {
        if (LLVKPassState::tUnitsLeft)
        {
            LLVKPassState::noteDraw(program);   // research/204 10 (vk-s4c2): before the pass cache (a hit gathers nothing)
        }
        LLVKMirror& mirror = LLVKMirror::instance();
        // Inside an OIT session (research/41) a draw into its target writes list
        // nodes with the program's OIT variant: its own pipeline, set 2 and no
        // depth writes (tOITDraw, which the pass cache keys on too).
        U8 oit_mode = OIT_DRAW_NONE;
        OITVariant* const oit = sOIT.active ? oit_for_draw(program, target, oit_mode) : nullptr;
        const OITDrawScope oit_scope(oit_mode);

        // Vertex inputs.
        U64 input_signature = LLRHI::FNV1A_BASIS_SHORT;
        if (tDirect)
        {
            if (!direct_inputs(program, input_signature))
            {
                return;
            }
        }
        else if (tInputsFor.program == &program && tInputsFor.program_changes == sProgramChanges &&
            tInputsFor.vertex_state == sVertexState && tInputsFor.buffer_deletes == sBufferDeletes && tInputsFor.frame == sFrame)
        {
            input_signature = tInputsFor.signature;
            ++sVertexInputsReused;
        }
        else
        {
            PerfScope scope(PERF_VERTEX);
            tInputsFor = VertexInputsFor();     // invalid until the loop completes
            tInputs.clear();
            tVertexBuffers.clear();
            tVertexOffsets.clear();
            for (size_t i = 0; i < program.vk.inputs.size(); ++i)
            {
                const auto& input = program.vk.inputs[i];
                S32 attrib_index = program.input_attribs[i];
                Attrib* a = attrib_index >= 0 && attrib_index < 32 ? &sAttribs[attrib_index] : nullptr;
                VkBuffer buffer = VK_NULL_HANDLE;
                VkDeviceSize offset = 0;
                Buffer* source = a && a->enabled ? attrib_buffer(*a) : nullptr;
                if (source && (buffer = gpu_buffer(*source)) == VK_NULL_HANDLE && !source->data.empty())
                {
                    // research/285: a buffer with data but no Vulkan copy this frame
                    // (its remake left to a later frame, or no memory): not drawn,
                    // rather than drawn from a constant input (a new pipeline for
                    // nothing). A buffer with no data reads the constant, as GL does.
                    skip("vertex buffer");
                    return;
                }
                if (buffer)
                {
                    VkFormat format = match_signedness(attrib_format(*a), input.gl_type);
                    if (format == VK_FORMAT_UNDEFINED)
                    {
                        skip("vertex attribute type");
                        return;
                    }
                    U32 tight = (U32)llclamp(a->size, 1, 4) * (a->type == GL_FLOAT || a->type == GL_UNSIGNED_INT || a->type == GL_INT ? 4 :
                                                              a->type == GL_UNSIGNED_SHORT ? 2 : 1);
                    tInputs.push_back({ input.location, format, a->stride ? (U32)a->stride : tight });
                    input_signature = LLRHI::fnv1aMix(input_signature, (U64)format << 32 | tInputs.back().stride);
                    tVertexBuffers.push_back(buffer);
                    tVertexOffsets.push_back(a->offset);
                }
                else
                {
                    // Disabled attribute: GL supplies the current constant value.
                    bool integer = is_integer_input(input.gl_type);
                    if (!constant_attribute(integer ? nullptr : (a ? a->constant : Attrib().constant), buffer, offset))
                    {
                        return;
                    }
                    // Assumed as above (fix-round15 9): no ivecN input without a buffer.
                    VkFormat format = !integer ? VK_FORMAT_R32G32B32A32_SFLOAT :
                                      input.gl_type == GL_INT ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_UINT;
                    tInputs.push_back({ input.location, format, 0 });
                    input_signature = LLRHI::fnv1aMix(input_signature, (U64)format << 32);
                    tVertexBuffers.push_back(buffer);
                    tVertexOffsets.push_back(offset);
                }
            }
            tInputsFor = { &program, sProgramChanges, sVertexState, sBufferDeletes, sFrame, input_signature };
        }

        VkBuffer index_buffer = VK_NULL_HANDLE;
        VkIndexType index_type = VK_INDEX_TYPE_UINT16;
        if (tFanIndexBuffer)
        {
            index_buffer = tFanIndexBuffer;
            index_type = VK_INDEX_TYPE_UINT32;
        }
        else if (index.indexed)
        {
            const Buffer* index_source = nullptr;
            if (tDirect)
            {
                index_buffer = gpu_buffer_named(tDirect->index_buffer);
                index_source = sBuffers.find(tDirect->index_buffer);
                if (LLRHIVBCensus::sOn && index_source && index_source->owner_number)   // research/293 branch 0: counting only
                {
                    LLRHIVBCensus::nameRead(LLRHIVBCensus::READ_FIND_SIZE, index_source->owner_number);
                }
            }
            else if (Buffer* source = element_buffer())
            {
                index_buffer = gpu_buffer(*source);
                index_source = source;
            }
            if (!index_buffer)
            {
                skip("index buffer");
                return;
            }
            if (index.type == GL_UNSIGNED_BYTE)
            {
                skip("8-bit indices");
                return;
            }
            if (!index_range_fits(index_source, index.type, index.offset, count))
            {
                skip("index range");
                return;
            }
            index_type = index.type == GL_UNSIGNED_INT ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
        }

        // Pass cache: the same pass state as the program's last draw recorded
        // through the full path (and its uniform copy not per draw: matrices in the table).
        VkPrimitiveTopology prim = topology(mode);
        const bool cacheable = tDirect && sPassCacheEnabled && !direct_writes_block(program);
        PassMissWhy why = MISS_NO_ENTRY;
        const PassHit hit = cacheable ? pass_hit(program, target, prim, input_signature, &why) : PASS_MISS;
        if (hit != PASS_MISS)
        {
            PassHitDraw h;
            const PassHitResult result = pass_hit_resolve(program, target, hit, h);
            if (result == PASS_HIT_DROP)
            {
                return;
            }
            if (result == PASS_HIT_RECORD)
            {
                pass_hit_record(program, target, h, oit, oit_mode, prim, first, count, instances, index, index_buffer, index_type);
                return;
            }
        }
        if (cacheable)
        {
            ++sPassMisses;
            ++sPassMissWhy[hit != PASS_MISS ? MISS_UNUSABLE : why];    // research/155 T0b: always (no second test)
        }
        // This draw replaces its program's entry (other programs keep theirs:
        // their hits bind their own pipeline and set again).
        if (program.pass)
        {
            program.pass->valid = false;
        }
        sRun.pass_ok = false;   // the run's next draw asks the pass cache again (run_pass_hit)

        {
            PerfScope scope(PERF_DESCRIPTORS);
            if (!gather_descriptors(program, target))
            {
                skip("descriptor data");
                return;
            }
            if (tPaletteFull)
            {
                skip("palette table full");
                ++sPaletteSkips;
                return;
            }
        }

        // Layouts: sampled textures readable, attachments writable. Barriers
        // cannot go inside a rendering scope, so close it only when needed.
        {
            PerfScope scope(PERF_BARRIERS);
            auto sampled_layout = [&](const Texture* tex)
            {
                return tex == target.depth && target.target.depth_read_only ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                                                             : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            };
            bool barriers = needs_attachment_layouts(target);
            for (Texture* tex : *tSampledUsed)
            {
                barriers |= tex->layout != sampled_layout(tex);
            }
            for (Texture* tex : tBindlessSampled)
            {
                barriers |= tex->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }
            if (barriers)
            {
                VkCommandBuffer cmd = mirror.transferCommands();
                LLVKMirror::BarrierBatch batch(cmd);    // the draw's layout changes in one call (research/111 A-BB)
                for (Texture* tex : *tSampledUsed)
                {
                    transition(cmd, *tex, sampled_layout(tex));
                }
                for (Texture* tex : tBindlessSampled)
                {
                    transition(cmd, *tex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                }
                to_attachment_layouts(cmd, target);
            }
        }

        VkPipeline pipeline = VK_NULL_HANDLE;
        {
            PerfScope scope(PERF_PIPELINE);
            const U32 prim_class = topology_class(prim);
            const U32 pipeline_state = sPipelineState.load(std::memory_order_relaxed);
            if (program.last_pipeline && program.last_pipeline_state == pipeline_state && target.stamp &&
                program.last_pipeline_target == target.stamp && program.last_pipeline_prim == prim_class &&
                program.last_pipeline_inputs == input_signature && program.last_pipeline_oit == oit_mode)
            {
                pipeline = program.last_pipeline;
            }
            else
            {
                pipeline = get_pipeline(sCurrentProgram, program, tDirect ? tDirectInputs.inputs : tInputs, prim, target.target, false, oit,
                                        oit_mode == OIT_DRAW_SPLIT);
                program.last_pipeline = pipeline;
                program.last_pipeline_state = pipeline_state;
                program.last_pipeline_target = target.stamp;
                program.last_pipeline_prim = prim_class;
                program.last_pipeline_inputs = input_signature;
                program.last_pipeline_oit = oit_mode;
            }
        }
        if (!pipeline)
        {
            skip("pipeline creation failed");
            return;
        }

        // Same program and same descriptor contents as its last draw this frame: reuse the set.
        // On the shared layout there is none (research/50 step 3).
        VkDescriptorSet set = VK_NULL_HANDLE;
        U64 set_hash = 0;
        U32* cached_entry = nullptr;    // set_cache's entry for set_hash (null: none)
        if (sSetPersist && !program.shared_layout)
        {
            // research/156 A-DS: kept across frames, in the mirror's persistent pools.
            PerfScope scope(PERF_SETS);
            set = persistent_set(program);
            if (!set)
            {
                skip("descriptor allocation");
                return;
            }
        }
        else if (!program.shared_layout && program.last_frame == sFrame && program.last_key == tKey)
        {
            set = program.last_set;
            ++sSetsReused;
        }
        else if (!program.shared_layout)
        {
            PerfScope scope(PERF_SETS);
            if (program.set_cache_frame != sFrame)
            {
                program.set_cache.clear();      // keeps its slots, and set_cache_entries the memory of their keys
                program.set_cache_frame = sFrame;
            }
            set_hash = LLRHI::fnv1aWords(LLRHI::FNV1A_BASIS_SHORT, tKey.data(), tKey.size());
            cached_entry = program.set_cache.find(set_hash);
            if (cached_entry && program.set_cache_entries[*cached_entry].first == tKey)
            {
                set = program.set_cache_entries[*cached_entry].second;
                program.last_key = tKey;
                program.last_set = set;
                program.last_frame = sFrame;
                ++sSetsCached;
            }
        }
        if (!set && !program.shared_layout)
        {
            PerfScope scope(PERF_SETS);
            set = mirror.allocateSet(program.set_layout);
            if (!set)
            {
                skip("descriptor allocation");
                return;
            }
            for (VkWriteDescriptorSet& write : tWrites)
            {
                write.dstSet = set;
            }
            vkUpdateDescriptorSets(LLVKDevice::instance().getDevice(), (U32)tWrites.size(), tWrites.data(), 0, nullptr);
            program.last_key = tKey;
            program.last_set = set;
            program.last_frame = sFrame;
            // Another key with the same hash takes over its entry, as before; a
            // new hash takes the next entry (the memory of an earlier frame's).
            U32 entry = 0;
            if (cached_entry)
            {
                entry = *cached_entry;
            }
            else
            {
                entry = program.set_cache.size();
                program.set_cache.set(set_hash, entry);
                if (entry == program.set_cache_entries.size())
                {
                    program.set_cache_entries.emplace_back();
                }
            }
            program.set_cache_entries[entry].first.assign(tKey.begin(), tKey.end());
            program.set_cache_entries[entry].second = set;
            ++sSetsAllocated;
        }

        PerfScope scope(PERF_RECORD);
        Recorded& r = sRecorded;
        PerfScope begin_scope(PERF_BEGIN);
        VkCommandBuffer cmd = mirror.beginRendering(target.target);
        begin_scope.stop();
        if (pipeline != r.pipeline)
        {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            r.pipeline = pipeline;
        }
        bind_program_set(cmd, program, set, tHasDynamic, tDynamicOffset);
        if (oit)
        {
            oit_bind_set(cmd, program, *oit);
        }
        PerfScope dynamic_scope(PERF_DYNAMIC);
        set_dynamic_state(cmd, target.target, prim);
        dynamic_scope.stop();
        // Node draws never join draw runs; split draws may (their set 2 and
        // priority stay bound while they are the same: oit_bind_set), unless
        // their variant reads the priority from the row (research/205 3a-2).
        if (oit_mode == OIT_DRAW_NODES || (oit && oit->row_priority_word != U32_MAX) ||
            !run_draw(cmd, program, target, prim, count, instances, index, false))
        {
            record_draw_commands(cmd, program, first, count, instances, index, index_buffer, index_type);
        }
        if (cacheable)
        {
            pass_store(program, target, prim, input_signature, pipeline, set);
        }
    }

} // namespace
    // A capture (research/50 step 4) keeps, for the list to write later: the
    // draw's program, pool placement (copied as for a run), vertex format, the
    // pool streams its inputs read, the GL texture each sampler's unit holds
    // (the draw's own or the unit's binding, as gather_slots resolves them),
    // its material row and a hash of the render state. The program's other
    // words (constants, matrices) come from the face's own draw later.
    // The program's constants row as it is (research/50 step 4).
    U64 consts_hash(const Program& program)
    {
        const size_t floats = llmin(program.consts.size(), (size_t)program.consts_vec4s * 4);
        return LLRHI::fnv1aFloats(LLRHI::FNV1A_BASIS_SHORT ^ program.consts_vec4s, program.consts.data(), floats);
    }
namespace
{

    U32 capture_state_hash()
    {
        const State& s = sState;
        U64 h = LLRHI::FNV1A_BASIS_SHORT;
        auto mix = [&h](U64 v) { h = LLRHI::fnv1aMix(h, v); };
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
        mix(s.front_face);
        mix(s.depth_test);
        mix(s.depth_write);
        mix(s.depth_func);
        mix(s.polygon_offset);
        mix(s.depth_clamp);
        U32 bits = 0;
        memcpy(&bits, &s.offset_factor, 4);
        mix(bits);
        memcpy(&bits, &s.offset_units, 4);
        mix(bits);
        mix(s.polygon_mode);
        mix(s.stencil);
        const Stencil* faces[2] = { &s.stencil_front, &s.stencil_back };
        for (const Stencil* st : faces)
        {
            mix(st->func);
            mix((U32)st->ref);
            mix(st->compare_mask);
            mix(st->write_mask);
            mix(st->fail);
            mix(st->depth_fail);
            mix(st->pass);
        }
        // Not the framebuffer: the same for all of a pass's batches, and blocks
        // registered in other faces join their runs if nothing else differs.
        const U32 folded = (U32)(h ^ (h >> 32));
        return folded ? folded : 1;
    }

    void capture_draw(Program& program, const LLRHI::DirectDraw& d)
    {
        if (LLVKPassState::tUnitsLeft)
        {
            LLVKPassState::noteDraw(program);   // research/204 10 (vk-s4c2): the probe list reads the units too
        }
        LLVKGLProbe::Capture c;
        auto done = [&c](const char* why)
        {
            c.ok = why == nullptr;
            c.why = why;
            sCapture.draws.push_back(c);
        };
        if (!LLRHI::sDrawRuns || !program.draw_runs || !program.draw_rows || !program.shared_layout)
        {
            return done("not a draw rows program on the shared layout");
        }
        if (program.palette_columns || program.light_table || program.draw_row_words > LLVKGLProbe::Words::MAX ||
            program.bindless.size() > LLVKGLProbe::Words::MAX || program.material_vec4s > LLVKGLProbe::Capture::MAX_MATERIAL_VEC4S)
        {
            return done("the program's words");
        }
        // A batch draw as a run takes it, with the matrices pushDirectBatch hands
        // over (the list makes the face's own) and no texture matrix of its own.
        if (d.mode != GL_TRIANGLES || !d.index_count || d.instance_count > 1 || d.table_base != LLRHI::DirectDraw::NO_TABLE ||
            !d.buffer_vertices || !d.attributes || d.texture_matrix0 || !d.matrix_id ||
            (d.index_type != GL_UNSIGNED_SHORT && d.index_type != GL_UNSIGNED_INT) ||
            (U64)d.first_index + d.index_count > d.buffer_indices)
        {
            return done("not a batch draw");
        }
        // Units sampled through descriptor images (direct_draw refuses those too).
        for (GLint unit : program.unit_values)
        {
            if (unit >= 0 && (U32)unit < d.texture_count && d.textures[unit] != LLRHI::DirectDraw::KEEP_UNIT)
            {
                return done("a descriptor image unit");
            }
        }
        // The texture each sampler reads, as gather_slots resolves its unit.
        for (const Program::BindlessSampler& sampler : program.bindless)
        {
            if (sampler.unit < 0)
            {
                continue;
            }
            if ((U32)sampler.unit >= LLVKGLProbe::Capture::UNITS)
            {
                return done("a sampler beyond unit 7");
            }
            const U32 unit = (U32)sampler.unit;
            const bool from_draw = unit < d.texture_count && d.textures[unit] != LLRHI::DirectDraw::KEEP_UNIT;
            // research/197 1a: a draw handle's number as its image's GL name
            c.names[unit] = from_draw ? handle_image(d.textures[unit]) : sUnits[unit].names[target_slot(GL_TEXTURE_2D)];
            c.sampled |= 1u << unit;
        }
        // The pool placement a run would draw it from (copies go into the upload commands).
        // research/197 1b: by the GL names (the probe lists keep a pair by name: its pin and drop callback).
        LLRHI::DirectDraw named = d;
        named.vb = 0;
        LLVKAttribPool::Placement place;
        LLVKAttribPool::Ticket ticket;
        if (!LLVKAttribPool::place(named, place, ticket))
        {
            return done("no pool placement");
        }
        // The vertex inputs its pipeline would have, and the pool streams they read.
        U64 signature = 0;
        bool inputs = false;
        {
            const DirectScope direct(&d);
            inputs = direct_inputs(program, signature);
        }
        if (!inputs)
        {
            return done("vertex inputs");
        }
        const DirectInputs& f = tDirectInputs;
        for (size_t i = 0; i < f.source.size(); ++i)
        {
            const S32 k = f.source[i];
            if (k < 0)
            {
                continue;
            }
            const LLRHI::DirectDraw::Attribute& a = d.attributes[k];
            if (c.streams >= LLVKGLProbe::Capture::MAX_STREAMS || !a.stride)
            {
                return done("vertex streams");
            }
            c.stream_location[c.streams] = a.location;
            c.stream_stride[c.streams] = a.stride;
            c.stream_key[c.streams] = a.streamKey();        // research/293 branch 1
            c.stream_shift[c.streams] = a.streamShift();
            ++c.streams;
        }
        c.material_vec4s = program.material_vec4s;
        if (c.material_vec4s)
        {
            memcpy(c.material, program.material.data(), (size_t)c.material_vec4s * 16);
        }
        c.consts_hash = consts_hash(program);
        c.program = &program;
        c.program_changes = sProgramChanges;
        c.format = run_format(d);
        c.state = capture_state_hash();
        c.vertex_buffer = d.vertex_buffer;
        c.chunk = place.chunk;
        c.vertex_offset = place.vertex_offset;
        c.first_index = place.first_index + d.first_index;
        c.index_count = d.index_count;
        done(nullptr);
    }

} // namespace
    U32 sDirectRefused[DIRECT_REFUSAL_COUNT] = {};

    std::string direct_refusals(U32 frames)
    {
        static const char* const names[DIRECT_REFUSAL_COUNT] = { "no program", "unbuilt", "shape", "descriptor unit" };
        std::ostringstream out;
        for (U32 i = 0; i < DIRECT_REFUSAL_COUNT; ++i)
        {
            out << (i ? ", " : "") << names[i] << " " << sDirectRefused[i] / frames;
        }
        return out.str();
    }

    // LLRHI::sDirectDraw: one batch of a draw pool, with the current GL state
    // for everything the draw does not bring.
    bool direct_draw(const LLRHI::DirectDraw& d)
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        Program* program = current_program();
        const bool indexed = d.index_count > 0;
        if (sCapture.on)
        {
            // Kept, never drawn: every draw leaves an entry (the viewer counts them).
            if (!program || !program->built)
            {
                LLVKGLProbe::Capture c;
                c.why = "no program";
                sCapture.draws.push_back(c);
            }
            else
            {
                capture_draw(*program, d);
            }
            return true;
        }
        const U32 refusal = direct_refusal(program, d);
        if (refusal != DIRECT_REFUSAL_COUNT)
        {
            ++sDirectRefused[refusal];
            if (LLRHIVBCensus::sOn)     // research/293 branch 0: counting only
            {
                LLRHIVBCensus::refused(census_owner(d.vertex_buffer), (LLRHIVBCensus::Refusal)refusal);
            }
            return false;
        }
        if (LLRHI::sAvatarStats && program->palette_columns == 3)
        {
            ++sAvRiggedDraws;   // research/56: a rigged draw handed over directly
        }
        if (sRun.open && run_continue(*program, d))
        {
            ++sDirectDraws;     // the next draw of a run (research/46 G4b)
            return true;
        }
        IndexInfo index;
        index.indexed = indexed;
        index.type = d.index_type;
        index.offset = indexed ? (size_t)d.first_index * (d.index_type == GL_UNSIGNED_INT ? 4 : 2) : 0;
        {
            // The draw being recorded, while it is (also when it throws).
            struct Current
            {
                explicit Current(const LLRHI::DirectDraw& draw) { tDirect = &draw; }
                ~Current() { tDirect = nullptr; }
            } current(d);
            draw((GLenum)d.mode, indexed ? 0 : (GLint)d.first_vertex, (GLsizei)(indexed ? d.index_count : d.vertex_count),
                 (GLsizei)llmax(d.instance_count, 1u), index);
        }
        ++sDirectDraws;
        if (d.table_base != LLRHI::DirectDraw::NO_TABLE)
        {
            ++sLightBatches;
            sLightInstances += d.instance_count;
        }
        return true;
    }

    // research/137 N7: what draw_one checks and resolves before a draw's own
    // values, once for a draw list (its draws change none of it: the guard
    // cuts the list at any hook, and the GL entry points cut it too).
    bool list_begin(ListHead& head)
    {
        LLVKMirror& mirror = LLVKMirror::instance();
        if (sCapture.on || suppress_translation() || !mirror.isRecording() || sActiveQuery || depth_only_pass() || census_tag())
        {
            return false;   // direct_draw's own branches for these
        }
        {
            // Draws writing nothing (draw_one counts and drops each).
            const State& s = sState;
            const bool writes_color = s.color_mask[0] || s.color_mask[1] || s.color_mask[2] || s.color_mask[3];
            const bool writes_depth = s.depth_test && s.depth_write;
            const bool writes_stencil = s.stencil && (s.stencil_front.write_mask || s.stencil_back.write_mask);
            if (!writes_color && !writes_depth && !writes_stencil)
            {
                return false;
            }
        }
        Program* program = current_program();
        if (!program || !program->built)
        {
            return false;
        }
        if (sRecorded.frame != sFrame)
        {
            sRecorded = Recorded();
            sRecorded.frame = sFrame;
            pass_invalidate();
        }
        head.target_state = sTargetState.load(std::memory_order_relaxed);
        head.framebuffer = sState.draw_framebuffer;
        head.target_ok = resolve_draw_target(head.target);
        if (!head.target_ok)
        {
            return false;
        }
        head.program = program;
        head.depth_only_program = is_depth_only_program(*program);
        return true;
    }

    // research/137 N7: direct_draw for a draw of a list whose head list_begin
    // resolved: draw_one's per-draw part, with the head's program and target.
    bool list_draw(ListHead& head, const LLRHI::DirectDraw& d)
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
            return false;
        }
        if (LLRHI::sAvatarStats && program.palette_columns == 3)
        {
            ++sAvRiggedDraws;
        }
        if (sRun.open && run_continue(program, d))
        {
            ++sDirectDraws;
            return true;
        }
        const bool indexed = d.index_count > 0;
        IndexInfo index;
        index.indexed = indexed;
        index.type = d.index_type;
        index.offset = indexed ? (size_t)d.first_index * (d.index_type == GL_UNSIGNED_INT ? 4 : 2) : 0;
        const GLenum mode = (GLenum)d.mode;
        const GLint first = indexed ? 0 : (GLint)d.first_vertex;
        const GLsizei count = (GLsizei)(indexed ? d.index_count : d.vertex_count);
        const GLsizei instances = (GLsizei)llmax(d.instance_count, 1u);
        {
            struct Current
            {
                explicit Current(const LLRHI::DirectDraw& draw) { tDirect = &draw; }
                ~Current() { tDirect = nullptr; }
            } current(d);
            if (mode == GL_TRIANGLE_FAN || mode == GL_LINE_LOOP || mode == GL_QUADS)
            {
                draw(mode, first, count, instances, index);     // converted, or refused, the usual way
            }
            else if (count > 0)
            {
                PerfScope total(PERF_DRAW);
                if (head.depth_only_program)
                {
                    ++sDepthOnlyFailures["outside the pass"];
                }
                ++program.generic_draws;
                // A target that failed to resolve stays refused until the state changes
                // again (head.target may be partly filled then).
                if (head.target_state != sTargetState.load(std::memory_order_relaxed) || head.framebuffer != sState.draw_framebuffer)
                {
                    head.target_state = sTargetState.load(std::memory_order_relaxed);
                    head.framebuffer = sState.draw_framebuffer;
                    head.target_ok = resolve_draw_target(head.target);
                }
                if (head.target_ok)
                {
                    ResolvedTarget target = head.target;    // the draw may mark it depth read-only
                    draw_into_target(program, target, mode, first, count, instances, index);
                }
                else
                {
                    skip("incomplete render target");
                }
            }
        }
        ++sDirectDraws;
        if (d.table_base != LLRHI::DirectDraw::NO_TABLE)
        {
            ++sLightBatches;
            sLightInstances += d.instance_count;
        }
        return true;
    }

    // LLRHI::sLightTableProgram (research/47): the program reads its light from
    // the table for draws with a table_base.
    bool native_light_table_program(U32 gl_program)
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        auto it = sPrograms.find(gl_program);
        return it != sPrograms.end() && it->second.built && it->second.light_table;
    }

    // LLRHI::sTableRows: rows of the frame's palette table (vec4s), which the
    // light table rewrite reads (binding 2).
    F32* native_table_rows(U32 vec4s, U32& base)
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        return LLVKBindless::paletteRows(vec4s, base);
    }

    void clear(GLbitfield mask)
    {
        LLVKMirror& mirror = LLVKMirror::instance();
        if (!mirror.isRecording())
        {
            return;
        }
        ResolvedTarget target;
        if (!resolve_draw_target(target))
        {
            skip("clear on incomplete render target");
            return;
        }
        const State& s = sState;
        // Every color attachment plus depth/stencil: no allocation per clear.
        constexpr U32 MAX_COLORS = (U32)std::extent_v<decltype(target.target.colors)>;
        VkClearAttachment attachments[MAX_COLORS + 1];
        U32 attachment_count = 0;
        // Assumed (fix-round15 9): the viewer clears with the color mask all on
        // or all off and the stencil mask all on. vkCmdClearAttachments has no
        // masks, so a partial glColorMask clears every channel and glStencilMask
        // is not applied; masking would need a drawn full-screen clear instead.
        bool any_color_write = s.color_mask[0] || s.color_mask[1] || s.color_mask[2] || s.color_mask[3];
        if ((mask & GL_COLOR_BUFFER_BIT) && any_color_write)
        {
            for (U32 i = 0; i < target.target.color_count && i < MAX_COLORS; ++i)
            {
                if (target.target.colors[i])
                {
                    VkClearAttachment& a = attachments[attachment_count++];
                    a = {};
                    a.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    a.colorAttachment = i;
                    memcpy(a.clearValue.color.float32, s.clear_color, sizeof(F32) * 4);
                }
            }
        }
        VkImageAspectFlags ds = 0;
        if ((mask & GL_DEPTH_BUFFER_BIT) && target.target.depth && s.depth_write)
        {
            ds |= VK_IMAGE_ASPECT_DEPTH_BIT;
        }
        if ((mask & GL_STENCIL_BUFFER_BIT) && target.target.stencil)
        {
            ds |= VK_IMAGE_ASPECT_STENCIL_BIT;
        }
        if (ds)
        {
            VkClearAttachment& a = attachments[attachment_count++];
            a = {};
            a.aspectMask = ds;
            a.clearValue.depthStencil = { s.clear_depth, (U32)s.clear_stencil };
        }
        if (!attachment_count)
        {
            return;
        }

        S32 x0 = 0, y0 = 0, x1 = (S32)target.target.width, y1 = (S32)target.target.height;
        if (s.scissor)
        {
            x0 = llclamp(s.scissor_rect[0], 0, (S32)target.target.width);
            y0 = llclamp(s.scissor_rect[1], 0, (S32)target.target.height);
            x1 = llclamp(s.scissor_rect[0] + s.scissor_rect[2], 0, (S32)target.target.width);
            y1 = llclamp(s.scissor_rect[1] + s.scissor_rect[3], 0, (S32)target.target.height);
        }
        if (x1 <= x0 || y1 <= y0)
        {
            return;
        }
        if (needs_attachment_layouts(target))
        {
            to_attachment_layouts(mirror.transferCommands(), target);
        }
        VkCommandBuffer cmd = mirror.beginRendering(target.target);
        VkClearRect rect = {};
        rect.rect = { { x0, y0 }, { (U32)(x1 - x0), (U32)(y1 - y0) } };
        rect.layerCount = 1;
        vkCmdClearAttachments(cmd, attachment_count, attachments, 1, &rect);
    }
} // namespace llvkgl

#endif // LL_GL_FUNC_POINTER

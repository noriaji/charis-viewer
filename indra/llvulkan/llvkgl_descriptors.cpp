/**
 * @file llvkgl_descriptors.cpp
 * @brief A draw's descriptors and push constants (texture slots, matrix, palette, material and constants rows), its dynamic state, and the pass cache (research/39 N3, research/91).
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
    // Draws: their descriptors

    // Descriptor contents for one draw, gathered before any rendering scope is
    // opened so sampled textures can be transitioned first. Scratch vectors are
    // reused across draws (draws run on the render thread under sLock).
    std::vector<VkWriteDescriptorSet> tWrites;
namespace
{
    std::vector<VkDescriptorBufferInfo> tBuffers;
    std::vector<VkDescriptorImageInfo> tImages;
    std::vector<Texture*> tSampled;
} // namespace
    // The textures this draw samples through descriptors: tSampled, or the
    // program's saved list when its image descriptors are reused (no copy).
    const std::vector<Texture*>* tSampledUsed = &tSampled;
    std::vector<U64> tKey;          // identity of the descriptor contents, for set reuse
    std::vector<Texture*> tBindlessSampled;    // textures sampled through the bindless table
    U32 tSlots[LLVKBindless::MAX_PUSHED_SLOTS];  // their slot numbers, pushed with the draw
    GLuint tSlotNames[LLVKBindless::MAX_PUSHED_SLOTS];     // diagnostics (LLRHI_VK_CHECKPOINTS)
    VkImageView tSlotViews[LLVKBindless::MAX_PUSHED_SLOTS];
    U32 tSlotCount = 0;
    // The direct draw being recorded (LLRHI::sDirectDraw), null for GL draws.
    const LLRHI::DirectDraw* tDirect = nullptr;
    U32 sDirectDraws = 0;
    U32 sLightBatches = 0, sLightInstances = 0;    // instanced point light draws (research/47)
    U32 sWhiteFrame = U32_MAX;                  // frame the white stand-in was last made current
    U32 tDynamicOffset = 0;         // ring offset of the default uniform block
namespace
{
    VkBuffer tDynamicBuffer = VK_NULL_HANDLE;   // the buffer that offset is in (the set's descriptor)
} // namespace
    bool tHasDynamic = false;
namespace
{

    // A column-major matrix into a std140 block: columns 16 bytes apart.
    void write_block_matrix(U8* block, size_t size, S32 offset, const F32* m, U32 columns, U32 rows)
    {
        if (offset < 0 || !m)
        {
            return;
        }
        for (U32 c = 0; c < columns; ++c)
        {
            const size_t at = (size_t)offset + c * 16;
            if (at + rows * 4 > size)
            {
                return;
            }
            memcpy(block + at, m + c * rows, rows * 4);
        }
    }

    // The direct draw's matrices into its copy of the default uniform block.
    void resolve_direct_uniforms(Program& program);

} // namespace
    // Whether a direct draw writes its matrices into the default block (matrix
    // uniforms the table does not carry).
    bool direct_writes_block(Program& program)
    {
        resolve_direct_uniforms(program);
        const Program::DirectUniforms& u = program.direct;
        return u.modelview >= 0 || u.modelview_projection >= 0 || u.normal >= 0 || u.inverse_modelview >= 0 || u.texture_matrix0 >= 0;
    }
namespace
{

    void resolve_direct_uniforms(Program& program)
    {
        if (program.direct_resolved_at != sProgramChanges)
        {
            program.direct = Program::DirectUniforms();
            for (const LLVKShader::Uniform& uniform : program.vk.uniforms)
            {
                if (uniform.block != program.default_block || uniform.offset < 0)
                {
                    continue;
                }
                if (uniform.name == "modelview_matrix") program.direct.modelview = uniform.offset;
                else if (uniform.name == "modelview_projection_matrix") program.direct.modelview_projection = uniform.offset;
                else if (uniform.name == "normal_matrix") program.direct.normal = uniform.offset;
                else if (uniform.name == "inv_modelview") program.direct.inverse_modelview = uniform.offset;
                else if (uniform.name == "texture_matrix0") program.direct.texture_matrix0 = uniform.offset;
            }
            program.direct_resolved_at = sProgramChanges;
        }
    }

    void write_direct_matrices(Program& program, U8* block, size_t size)
    {
        resolve_direct_uniforms(program);
        const LLRHI::DirectDraw& d = *tDirect;
        write_block_matrix(block, size, program.direct.modelview, d.modelview, 4, 4);
        write_block_matrix(block, size, program.direct.modelview_projection, d.modelview_projection, 4, 4);
        write_block_matrix(block, size, program.direct.normal, d.normal, 3, 3);
        write_block_matrix(block, size, program.direct.inverse_modelview, d.inverse_modelview, 4, 4);
        F32 tex_anim[16];   // research/194 11: a texture scroll's description as its matrix
        write_block_matrix(block, size, program.direct.texture_matrix0, LLRHI::texAnimMatrix(d.texture_matrix0, tex_anim), 4, 4);
    }

    // The draw's row in the draw matrix table: the program's matrices (as GL set
    // them), or a direct draw's own. Rows are shared while nothing changed.
    U32 tDirectRowFrame = U32_MAX;
    U32 tDirectRowId = 0;
    const F32* tDirectRowTexture = nullptr;
    const Program* tDirectRowProgram = nullptr;
    U32 tDirectRowVersion = 0;
    U32 tDirectRow = 0;

    void write_rows_mat3(F32* dst, const F32* m3)
    {
        for (U32 c = 0; c < 3; ++c)
        {
            dst[c * 4 + 0] = m3[c * 3 + 0];
            dst[c * 4 + 1] = m3[c * 3 + 1];
            dst[c * 4 + 2] = m3[c * 3 + 2];
            dst[c * 4 + 3] = 0.f;
        }
    }

} // namespace
    U32 matrix_row(Program& program)
    {
        U32 index = 0;
        // A direct draw with no matrices of its own (LLVertexBuffer::drawNative:
        // terrain patches, UI) has the program's: the program's row below
        // holds the same values and is shared while they stay the same, so
        // consecutive draws push the same row (and share a draw run's row).
        if (tDirect && (tDirect->matrix_id || tDirect->modelview || tDirect->modelview_projection || tDirect->normal ||
                        tDirect->texture_matrix0))
        {
            const LLRHI::DirectDraw& d = *tDirect;
            const bool own_texture = d.texture_matrix0 != nullptr;
            if (d.matrix_id && tDirectRowFrame == sFrame && tDirectRowId == d.matrix_id && tDirectRowTexture == d.texture_matrix0 &&
                (own_texture || (tDirectRowProgram == &program && tDirectRowVersion == program.matrix_version)))
            {
                return tDirectRow;
            }
            LLVKBindless::DrawMatrices* row = LLVKBindless::matrixRow(index);
            const LLVKBindless::DrawMatrices& m = program.matrices;
            memcpy(row->modelview, d.modelview ? d.modelview : m.modelview, 64);
            memcpy(row->modelview_projection, d.modelview_projection ? d.modelview_projection : m.modelview_projection, 64);
            F32 tex_anim[16];   // research/194 11: a texture scroll's description as its matrix (the key stays d.texture_matrix0)
            memcpy(row->texture0, d.texture_matrix0 ? LLRHI::texAnimMatrix(d.texture_matrix0, tex_anim) : m.texture0, 64);
            if (d.normal)
            {
                write_rows_mat3(row->normal, d.normal);
            }
            else
            {
                memcpy(row->normal, m.normal, sizeof(row->normal));
            }
            tDirectRowFrame = sFrame;
            tDirectRowId = d.matrix_id;
            tDirectRowTexture = d.texture_matrix0;
            tDirectRowProgram = &program;
            tDirectRowVersion = program.matrix_version;
            tDirectRow = index;
            return index;
        }
        if (program.matrix_row_frame == sFrame && program.matrix_row_version == program.matrix_version)
        {
            return program.matrix_row;
        }
        LLVKBindless::DrawMatrices* row = LLVKBindless::matrixRow(index);
        memcpy(row, &program.matrices, sizeof(*row));
        program.matrix_row_frame = sFrame;
        program.matrix_row_version = program.matrix_version;
        program.matrix_row = index;
        return index;
    }
namespace
{

    // Palettes copied into the table this frame, by content: the main pass, the
    // shadow cascades and the probes each upload a mesh's joints to their own
    // program, and those copies share one row. Keyed by one 64-bit hash of the
    // copied range and its shape, checked against a second, independent one.
    struct PaletteCopy
    {
        U64 check;
        U32 extent;     // vec4s copied
        U32 room;       // vec4s reserved
        U32 base;
    };
    std::unordered_map<U64, PaletteCopy> sPaletteCopiesByContent;
    U32 sPaletteCopiesFrame = U32_MAX;
} // namespace
    bool tPaletteFull = false;      // the draw's palette did not fit in the table: skip it
namespace
{

    inline U64 rotl64(U64 x, U32 r)
    {
        return (x << r) | (x >> (64 - r));
    }

    inline U64 fmix64(U64 h)
    {
        h ^= h >> 33;
        h *= 0xff51afd7ed558ccdull;
        h ^= h >> 33;
        h *= 0xc4ceb9fe1a85ec53ull;
        h ^= h >> 33;
        return h;
    }

    // Two independent 64-bit hashes of `vec4s` vec4s, seeded with the shape.
    void palette_hash(const F32* data, U32 vec4s, U32 room, U64& key, U64& check)
    {
        U64 a = 0x9e3779b97f4a7c15ull ^ ((U64)room << 32 | vec4s);
        U64 b = 0x27d4eb2f165667c5ull + ((U64)vec4s << 32 | room);
        const size_t words = (size_t)vec4s * 2;
        for (size_t i = 0; i < words; ++i)
        {
            U64 v;
            memcpy(&v, data + i * 2, 8);
            a = rotl64(a ^ (v * 0x87c37b91114253d5ull), 31) * 0x4cf5ad432745937full;
            b = rotl64(b + (v * 0x165667b19e3779f9ull), 27) * 0xc2b2ae3d27d4eb4full + 0x52dce729ull;
        }
        key = fmix64(a);
        check = fmix64(b ^ 0x85ebca77c2b2ae63ull);
    }

    // The program's skinning palette in the frame's palette table: copied once
    // per change per frame, so the draws of one avatar or mesh share it, and
    // shared with other programs' copies of the same joints. The room covers
    // the whole array (the shader may index any element); the copy stops at
    // palette_extent. Table full: tPaletteFull (the draw is skipped).
    U32 palette_row(Program& program)
    {
        if (program.palette_prebuilt)
        {
            // The row LLRHI::sUsePaletteRow named (research/56). A row of an
            // earlier frame with no upload since: skipped, never drawn with other joints.
            if (program.palette_prebuilt_frame == sFrame)
            {
                ++sPaletteNamed;
                sAvPaletteNamed += LLRHI::sAvatarStats ? 1 : 0;
                return program.palette_prebuilt_base;
            }
            tPaletteFull = true;
            return 0;
        }
        // Same values, and no longer a write than the row holds (a longer write of
        // unchanged values keeps the version but needs the longer copy).
        if (program.palette_row_frame == sFrame && program.palette_row_version == program.palette_version &&
            llmin(program.palette_extent, program.palette_elements * program.palette_columns) <= program.palette_row_extent)
        {
            if (program.palette_base == U32_MAX)
            {
                tPaletteFull = true;    // did not fit earlier this frame
                return 0;
            }
            ++sPaletteReused;
            return program.palette_base;
        }
        if (sPaletteCopiesFrame != sFrame)
        {
            sPaletteCopiesByContent.clear();
            sPaletteCopiesFrame = sFrame;
        }
        const U64 stats_start = LLRHI::sAvatarStats ? __rdtsc() : 0;
        const U32 room = program.palette_elements * program.palette_columns;
        const U32 extent = llmin(program.palette_extent, room);
        U64 key = 0, check = 0;
        palette_hash(program.palette.data(), extent, room, key, check);
        U32 base = 0;
        auto it = sPaletteCopiesByContent.find(key);
        if (it != sPaletteCopiesByContent.end() && it->second.check == check && it->second.extent == extent && it->second.room == room)
        {
            base = it->second.base;
            ++sPaletteShared;
        }
        else if (F32* dst = LLVKBindless::paletteRows(room, base))
        {
            memcpy(dst, program.palette.data(), (size_t)extent * 16);
            sPaletteCopiesByContent[key] = { check, extent, room, base };
            ++sPaletteCopies;
        }
        else
        {
            // Remembered as not fitting (U32_MAX) for this frame, so the table
            // is asked once per palette and its demand stays what the frame needs.
            base = U32_MAX;
            sPaletteCopiesByContent[key] = { check, extent, room, base };
        }
        program.palette_row_frame = sFrame;
        program.palette_row_version = program.palette_version;
        program.palette_row_extent = extent;
        program.palette_base = base;
        program.palette_written = false;
        if (stats_start)
        {
            ++sAvPaletteRows;
            sAvPaletteRowTicks += __rdtsc() - stats_start;
        }
        if (base == U32_MAX)
        {
            tPaletteFull = true;
            return 0;
        }
        return base;
    }

} // namespace
    // A material row of these contents in the frame's table: the earlier
    // copy of the same values this frame, else a new one. U32_MAX: the table
    // is full (asked once per contents this frame, see palette_row). Shared
    // with the draw packets' patched rows (research/180 stage 4), which are
    // material rows by content as well.
    U32 material_row_content(const F32* data, U32 vec4s)
    {
        if (sPaletteCopiesFrame != sFrame)
        {
            sPaletteCopiesByContent.clear();
            sPaletteCopiesFrame = sFrame;
        }
        U64 key = 0, check = 0;
        palette_hash(data, vec4s, vec4s, key, check);
        U32 base = 0;
        auto it = sPaletteCopiesByContent.find(key);
        if (it != sPaletteCopiesByContent.end() && it->second.check == check && it->second.extent == vec4s && it->second.room == vec4s)
        {
            base = it->second.base;
            ++sMaterialShared;
        }
        else if (F32* dst = LLVKBindless::paletteRows(vec4s, base))
        {
            memcpy(dst, data, (size_t)vec4s * 16);
            sPaletteCopiesByContent[key] = { check, vec4s, vec4s, base };
            ++sMaterialCopies;
        }
        else
        {
            base = U32_MAX;
            sPaletteCopiesByContent[key] = { check, vec4s, vec4s, base };
        }
        return base;
    }
namespace
{

    // The program's material uniforms in the frame's table (research/46 G4a):
    // copied once per change per frame, and shared by content with every
    // earlier copy of the same values this frame (other programs, the probe
    // faces, the palettes' map: a row of the same length and bits is the
    // same data). Table full: tPaletteFull (the draw is skipped, as for a
    // palette; the table grows for the next frame).
    U32 material_row(Program& program)
    {
        if (program.material_row_frame == sFrame && program.material_row_version == program.material_version)
        {
            if (program.material_base == U32_MAX)
            {
                tPaletteFull = true;
                return 0;
            }
            ++sMaterialReused;
            return program.material_base;
        }
        const U32 base = material_row_content(program.material.data(), program.material_vec4s);
        program.material_row_frame = sFrame;
        program.material_row_version = program.material_version;
        program.material_base = base;
        if (base == U32_MAX)
        {
            tPaletteFull = true;
            return 0;
        }
        return base;
    }

    // The program's constants row (research/50 step 3): the uniforms its
    // default block held, in the frame's table, as material_row does it
    // (copied once per change per frame, shared by content; table full:
    // tPaletteFull). Pass constants (the projection, the mirror clip, the
    // terrain's planes) change a few times a frame, so a frame takes few rows.
    U32 consts_row(Program& program)
    {
        if (program.consts_row_frame == sFrame && program.consts_row_version == program.consts_version)
        {
            if (program.consts_base == U32_MAX)
            {
                tPaletteFull = true;
                return 0;
            }
            ++sConstsReused;
            return program.consts_base;
        }
        if (sPaletteCopiesFrame != sFrame)
        {
            sPaletteCopiesByContent.clear();
            sPaletteCopiesFrame = sFrame;
        }
        const U32 vec4s = program.consts_vec4s;
        U64 key = 0, check = 0;
        palette_hash(program.consts.data(), vec4s, vec4s, key, check);
        U32 base = 0;
        auto it = sPaletteCopiesByContent.find(key);
        if (it != sPaletteCopiesByContent.end() && it->second.check == check && it->second.extent == vec4s && it->second.room == vec4s)
        {
            base = it->second.base;
            ++sConstsShared;
        }
        else if (F32* dst = LLVKBindless::paletteRows(vec4s, base))
        {
            memcpy(dst, program.consts.data(), (size_t)vec4s * 16);
            sPaletteCopiesByContent[key] = { check, vec4s, vec4s, base };
            ++sConstsCopies;
        }
        else
        {
            base = U32_MAX;     // asked once per row this frame (see palette_row)
            sPaletteCopiesByContent[key] = { check, vec4s, vec4s, base };
        }
        program.consts_row_frame = sFrame;
        program.consts_row_version = program.consts_version;
        program.consts_base = base;
        if (base == U32_MAX)
        {
            tPaletteFull = true;
            return 0;
        }
        return base;
    }

} // namespace
    // Whether the texture's bindless resolution of this frame (direct_*) is what
    // resolving it again for a draw into `target` would give: the same frame
    // (the slot was marked used this frame and its set entry queued), the same
    // version (same image, defined levels, view and sampler parameters), a
    // slot of its own, and (research/58, LLRHI_SLOT_CACHE) not an attachment
    // of the target, which is the only thing the resolution reads of it.
    // LLRHI_SLOT_CACHE=0: only for the same target resolution, as before.
    bool texture_slot_cached(const Texture& tex, const ResolvedTarget& target, bool slot_cache)
    {
        if (tex.direct_frame != sFrame || tex.direct_version != tex.version || tex.bindless_slot == U32_MAX ||
            tex.bindless_slot == LLVKBindless::WHITE_SLOT)
        {
            return false;
        }
        return slot_cache ? !is_attachment_of(target, &tex) : target.stamp && tex.direct_target == target.stamp;
    }

    // ---- research/180 slot publish rule (tools/selftest sim_slotpublish extracts it) ----
    // While a name is published (sPublishedSlots[name].slot1 = slot + 1):
    //  I1 the bindless slot's owner is the name (and the texture's bindless_slot),
    //  I2 the slot's wanted entry is the texture's view and sampler,
    //  I3 the current frame's set holds that entry (both sets: writePending),
    //  I4 the texture's layout is SHADER_READ_ONLY_OPTIMAL (recording order),
    //  I5 it is never an attachment (not attachable, attach_wanted, rendered_to),
    //  I6 its version is what it was when published.
    // So resolving it again for any draw would give the same slot, with nothing
    // to write, transition or check: a draw takes the slot and marks it used.
    // Every event that breaks one clears the publication (unpublish), also
    // while the switch is off; only publishing follows the switch. A clear
    // advances the name's gen (research/180 stage 4: a packet part's record).
    std::vector<PublishedSlot> sPublishedSlots;
namespace
{
    std::vector<U32> sPublishedVersions;            // by name: the version at publication (verify's I6)
    constexpr GLuint PUBLISH_NAME_LIMIT = 1u << 22; // as NameTable's dense names
    bool sPublishOn = false;        // look up and publish (on or verify)
    bool sPublishTake = false;      // draws take published slots (on, not verify)
    bool sPublishVerify = false;    // resolve the usual way and compare
    struct PublishCounts
    {
        U32 hits = 0, publishes = 0, clears = 0, pending_writes = 0, checked = 0, mismatches = 0, breaks = 0;
    };
    PublishCounts sPublishCounts;   // since the last report
    PublishCounts sPublishTotals;   // since the switch was last turned on (the auto mode reads it)
    U32 sPublishLogged = 0;         // verify warnings, at most 8 a run

    // The usual way resolved tex to `slot` for a draw: may it be published?
    // (2D color images that are never attachments, readable, with their view.)
    bool publishable(const Texture& tex, U32 slot)
    {
        return slot != LLVKBindless::WHITE_SLOT && slot != U32_MAX && tex.bindless_slot == slot && tex.view &&
               tex.target == GL_TEXTURE_2D && !tex.format.depth && !tex.attachable && !tex.attach_wanted && !tex.rendered_to &&
               tex.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    void publish_slot(GLuint name, Texture& tex, U32 slot)
    {
        if (!name || name >= PUBLISH_NAME_LIMIT)
        {
            return;
        }
        if (name >= sPublishedSlots.size())
        {
            // Grows like NameTable (doubling): not per frame.
            const size_t size = std::max((size_t)name + 1, sPublishedSlots.size() * 2);
            sPublishedSlots.resize(size, PublishedSlot());
            sPublishedVersions.resize(size, 0);
        }
        sPublishedSlots[name].slot1 = slot + 1;
        sPublishedVersions[name] = tex.version;
        tex.published_name = name;
        ++sPublishCounts.publishes;
    }
} // namespace

    void unpublish(Texture& tex)
    {
        unpublish_ref(tex);     // research/197 1a: a number's publication of it stands on the same
        const GLuint name = tex.published_name;
        if (!name)
        {
            return;
        }
        tex.published_name = 0;
        if (name < sPublishedSlots.size() && sPublishedSlots[name].slot1)
        {
            sPublishedSlots[name].slot1 = 0;
            ++sPublishedSlots[name].gen;
            ++sPublishCounts.clears;
        }
    }

    void unpublish_name(GLuint name)
    {
        Texture* tex = sTextures.find(name);
        if (tex)
        {
            unpublish_ref(*tex);    // research/197 1a: its slot left it (or it is deleted) for a number's publication too
        }
        if (name < sPublishedSlots.size() && sPublishedSlots[name].slot1)
        {
            sPublishedSlots[name].slot1 = 0;
            ++sPublishedSlots[name].gen;
            ++sPublishCounts.clears;
            if (tex)
            {
                tex->published_name = 0;
            }
        }
    }
namespace
{
    // A draw's sampler whose name looked up `p` (published_slot): taken
    // (on, not verify) as slot p - 1, marked used this frame as use() would.
    inline bool slot_publish_take(U32 p, U32& slot)
    {
        if (!p || !sPublishTake)
        {
            return false;
        }
        slot = p - 1;
        LLVKBindless::markUsed(slot, sFrame);
        ++sPublishCounts.hits;
        return true;
    }

    // After the usual way resolved `name` (tex: its texture, null if none) to
    // `slot` (WHITE if it could not). p: the publication looked up before it.
    // Verify: the publication must be that slot and still stand (the usual
    // way cleared nothing of it: no view made again, no slot moved). Else a
    // slot that may be published is, when it was resolved right now (`used`:
    // LLVKBindless::use ran for it, so the current set is written at this
    // draw and the other one queued). Not from the frame's earlier resolution
    // (texture_slot_cached): another texture's stale slot may have forgotten
    // the entries since (its view made again), which use() would write again.
    void slot_publish_after(GLuint name, Texture* tex, U32 slot, U32 p, bool used)
    {
        if (p)
        {
            ++sPublishCounts.checked;
            ++sPublishTotals.checked;
            if (p - 1 != slot || published_slot(name) != p)
            {
                ++sPublishCounts.mismatches;
                ++sPublishTotals.mismatches;
                if (sPublishLogged < 8)
                {
                    ++sPublishLogged;
                    LL_WARNS("Vulkan") << "slot publish verify: texture " << name << " published slot " << p - 1 << ", resolved slot " << slot
                                       << (published_slot(name) == p ? "" : " (the usual way cleared it)") << ", version "
                                       << (tex ? tex->version : 0) << ", layout " << (tex ? (S32)tex->layout : -1) << ", attach "
                                       << (tex ? (tex->attachable ? 1 : 0) + (tex->attach_wanted ? 2 : 0) + (tex->rendered_to ? 4 : 0) : -1)
                                       << LL_ENDL;
                }
            }
            return;
        }
        if (used && tex && publishable(*tex, slot))
        {
            publish_slot(name, *tex, slot);
        }
    }

    // Verify (research/180 5.1): every publication's I1..I6 at the frame's
    // head, after the pending entries were written. O(names), verify only.
    void slot_publish_check_all()
    {
        for (GLuint name = 1; name < sPublishedSlots.size(); ++name)
        {
            const U32 p = sPublishedSlots[name].slot1;
            if (!p)
            {
                continue;
            }
            const Texture* tex = sTextures.find(name);
            U32 broken = 0;     // I1 1, I2 2, I3 4, I4 8, I5 16, I6 32
            if (!tex || tex->published_name != name || tex->bindless_slot != p - 1)
            {
                broken |= 1;
            }
            else
            {
                const U32 ok = LLVKBindless::checkSlot(p - 1, name, tex->view, sampler_for(tex, false), tex->view_serial);
                broken |= ok & LLVKBindless::SLOT_OWNER_OK ? 0 : 1;
                broken |= ok & LLVKBindless::SLOT_WANTED_OK ? 0 : 2;
                broken |= ok & LLVKBindless::SLOT_WRITTEN_OK ? 0 : 4;
                broken |= tex->layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ? 0 : 8;
                broken |= tex->attachable || tex->attach_wanted || tex->rendered_to ? 16 : 0;
                broken |= name < sPublishedVersions.size() && sPublishedVersions[name] == tex->version ? 0 : 32;
            }
            if (broken)
            {
                ++sPublishCounts.breaks;
                ++sPublishTotals.breaks;
                if (sPublishLogged < 8)
                {
                    ++sPublishLogged;
                    LL_WARNS("Vulkan") << "slot publish verify: texture " << name << " published slot " << p - 1 << " breaks"
                                       << (broken & 1 ? " I1" : "") << (broken & 2 ? " I2" : "") << (broken & 4 ? " I3" : "")
                                       << (broken & 8 ? " I4" : "") << (broken & 16 ? " I5" : "") << (broken & 32 ? " I6" : "") << " ("
                                       << LLVKBindless::describe(p - 1) << ")" << LL_ENDL;
                }
            }
        }
    }
} // namespace
    // ---- end of the slot publish rule ----

    // ---- research/197 stage 1a: the texture number publication rule (tools/selftest sim_texhandles extracts it) ----
    // A draw's sampler named by a texture number (a draw handle) takes the
    // slot the number's entry published (I1..I6, R1, R2: llvkgl_internal.h)
    // instead of resolving its image again, as a published name's sampler does.
namespace
{
    U32 sTexRefLogged = 0;      // verify warnings, at most 8 a run

    // The number's publication p (its entry's slot1): taken (on, not verify)
    // as slot p - 1, marked used this frame as use() would.
    inline bool texref_take(U32 p, U32& slot)
    {
        if (!p || !sTexRefTake)
        {
            return false;
        }
        slot = p - 1;
        LLVKBindless::markUsed(slot, sFrame);
        ++sTexRefCounts.hits;
        return true;
    }

    // After the usual way resolved number id's image (tex: its texture, null
    // if none) to `slot` (WHITE if it could not). p: the publication looked
    // up before it. Verify: the publication must be that slot and still
    // stand. Else a slot that may be published is, when it was resolved right
    // now (`used`), for a texture no other number publishes (R2).
    void texref_after(U32 id, Texture* tex, U32 slot, U32 p, bool used)
    {
        TexRefEntry* e = texref_entry(id);
        if (!e)
        {
            return;
        }
        if (p)
        {
            ++sTexRefCounts.checked;
            ++sTexRefTotals.checked;
            if (p - 1 != slot || e->pub.slot1 != p)
            {
                ++sTexRefCounts.mismatches;
                ++sTexRefTotals.mismatches;
                if (sTexRefLogged < 8)
                {
                    ++sTexRefLogged;
                    LL_WARNS("Vulkan") << "texture number verify: number " << id << " (image " << e->image << ") published slot " << p - 1
                                       << ", resolved slot " << slot << (e->pub.slot1 == p ? "" : " (the usual way cleared it)") << ", version "
                                       << (tex ? tex->version : 0) << LL_ENDL;
                }
            }
            return;
        }
        if (used && tex && publishable(*tex, slot) && (!tex->published_ref || tex->published_ref == id))
        {
            e->pub.slot1 = slot + 1;
            e->version = tex->version;
            tex->published_ref = id;
            ++sTexRefCounts.publishes;
        }
    }
} // namespace
    // ---- end of the texture number publication rule ----

    // research/198 5.5: the retained scene resolved number id's image to
    // `slot` right now (LLVKBindless::use ran for it), as gather_slots does
    // for a draw naming the number: the same rule publishes it.
    void texref_resolved(U32 id, Texture* tex, U32 slot)
    {
        texref_after(id, tex, slot, 0, true);
    }
namespace
{
    // The frame's head of the slot publication (the first gather_slots of a
    // frame): the switches, the auto mode, the other set's pending entries,
    // and verify's check of every publication.
    U32 sPublishFrame = U32_MAX;
    bool sPublishSeenOn = false, sPublishSeenVerify = false;
    bool sPublishSeenRefs = false;      // research/197 1a: numbers published (texref_frame)
    U32 sPublishAutoFrames = 0;
    U32 sPublishAutoChecksFrom = 0;     // sPublishTotals.checked when the world was first drawn
    // The auto mode turns verify off (the publication on) after this many
    // frames of the world (LLRHI::sWorldShown) with this many draws compared
    // in them and not one mismatch or break (those of the login screen too).
    constexpr U32 PUBLISH_AUTO_FRAMES = 600;
    constexpr U32 PUBLISH_AUTO_CHECKS = 20000;

    void slot_publish_clear()
    {
        for (GLuint name = 0; name < sPublishedSlots.size(); ++name)
        {
            if (sPublishedSlots[name].slot1)
            {
                unpublish_name(name);
            }
        }
    }

    // LLRHI::sSlotPublishAuto: verify until enough draws compared clean, then
    // on; off at the first mismatch or break. True: it changed a switch.
    bool slot_publish_auto()
    {
        if (!LLRHI::sSlotPublish || !LLRHI::sSlotPublishVerify)
        {
            LLRHI::sSlotPublishAuto = false;    // not in the state it starts from (a cue changed it)
            return false;
        }
        const PublishCounts& c = sPublishTotals;
        if (!LLRHI::sWorldShown)
        {
            // The login screen (research/180 8): verified, but not counted toward on.
            sPublishAutoFrames = 0;
            sPublishAutoChecksFrom = c.checked;
        }
        else
        {
            ++sPublishAutoFrames;
        }
        if (c.mismatches || c.breaks)
        {
            LLRHI::sSlotPublish = false;
            LLRHI::sSlotPublishAuto = false;
            LL_WARNS("Vulkan") << "slot publish (auto): verify found " << c.mismatches << " mismatches and " << c.breaks
                               << " broken invariants in " << c.checked << " draws: off for this run (research/180)" << LL_ENDL;
            return true;
        }
        const U32 world_checked = c.checked - sPublishAutoChecksFrom;
        if (sPublishAutoFrames >= PUBLISH_AUTO_FRAMES && world_checked >= PUBLISH_AUTO_CHECKS)
        {
            LLRHI::sSlotPublishVerify = false;
            LLRHI::sSlotPublishAuto = false;
            LL_INFOS("Vulkan") << "slot publish (auto): " << world_checked << " draws compared over " << sPublishAutoFrames
                               << " frames of the world (" << sPublishAutoChecksFrom << " before it), no mismatch: on (research/180)"
                               << LL_ENDL;
            return true;
        }
        return false;
    }

    void slot_publish_frame()
    {
        sPublishFrame = sFrame;
        static bool hooked = false;
        if (!hooked)
        {
            hooked = true;
            LLVKBindless::setSlotLeft(&unpublish_name);     // always: clearing does not follow the switch
        }
        const bool by_auto = LLRHI::sSlotPublishAuto && sPublishSeenOn && slot_publish_auto();
        const bool on = LLRHI::sSlotPublish;
        const bool verify = on && LLRHI::sSlotPublishVerify;
        // research/197 1a: texture numbers publish through the same bindless
        // side (pending tracking, writePending), with a switch of their own.
        const bool refs = texref_frame();
        if (on != sPublishSeenOn || verify != sPublishSeenVerify || refs != sPublishSeenRefs)
        {
            if (sPublishSeenOn && !by_auto && LLRHI::sSlotPublishAuto && (on != sPublishSeenOn || verify != sPublishSeenVerify))
            {
                LLRHI::sSlotPublishAuto = false;    // a cue changed a switch: the auto mode is over
            }
            // Started over from nothing, whatever came before (both publications).
            slot_publish_clear();
            texref_clear();
            LLVKBindless::setPendingTracking(on || refs);
            if (on != sPublishSeenOn || verify != sPublishSeenVerify)
            {
                sPublishTotals = PublishCounts();
                sPublishAutoFrames = 0;
                sPublishAutoChecksFrom = 0;
                LL_INFOS("Vulkan") << "slot publish " << (!on ? "off" : verify ? (LLRHI::sSlotPublishAuto ? "auto (verify first)" : "verify") : "on")
                                   << LL_ENDL;
            }
            sPublishSeenOn = on;
            sPublishSeenVerify = verify;
            sPublishSeenRefs = refs;
        }
        sPublishOn = on;
        sPublishVerify = verify;
        sPublishTake = false;
        if (!on && !refs)
        {
            return;
        }
        // Both sets hold every publication from here on (I3), before any draw takes one.
        const U32 written = LLVKBindless::writePending();
        sPublishCounts.pending_writes += written;
        sPublishTake = on && !verify;
        if (verify)
        {
            slot_publish_check_all();
        }
        if (refs && sTexRefVerify)
        {
            texref_check_all();
        }
    }
} // namespace

    std::string slot_publish_report(U32 frames)
    {
        if (!frames || !(sPublishSeenOn || sPublishCounts.clears))
        {
            return std::string();
        }
        const PublishCounts& c = sPublishCounts;
        std::ostringstream out;
        out << ", published [" << (!sPublishSeenOn ? "off" : sPublishSeenVerify ? (LLRHI::sSlotPublishAuto ? "auto verify" : "verify") : "on")
            << ", hits " << c.hits / frames << " publishes " << c.publishes / frames << " clears " << c.clears / frames << " pending writes "
            << c.pending_writes / frames;
        if (sPublishSeenVerify)
        {
            out << " (verify checked " << c.checked / frames << " mismatches " << c.mismatches << " breaks " << c.breaks << ")";
        }
        out << "]";
        return out.str();
    }

    void slot_publish_reset_counters()
    {
        sPublishCounts = PublishCounts();
    }

    U32 slot_publish_mode()
    {
        return sPublishFrame != sFrame || !sPublishOn ? 0 : sPublishTake ? 2 : 1;
    }

    // Texture slot lookups by gather_slots (research/58, perfReport): through
    // the sampler's last unit binding, through the texture's resolution of
    // this frame, and resolved (texture_view, sampler, LLVKBindless::use).
    U32 sSlotsUnitCached = 0, sSlotsFrameCached = 0, sSlotsResolved = 0;
namespace
{
    // LLRHI_BINDLESS_DEBUG: keep each program's last slots (Program::bindless_slots) for draw_report.
    const bool sBindlessDebug = std::getenv("LLRHI_BINDLESS_DEBUG") != nullptr;

    // research/218 vk-s7f (research/207 7.1): the old paths of gather_slots,
    // since the last old_texture_path_report (under sLock, as the counts
    // above). Lookups of a GL name in the name publication, for a direct
    // draw's texture and for a unit's binding; direct draws (and their
    // samplers) giving a texture by its GL name instead of a number; the
    // first distinct such names of the run (fixed room, no allocation).
    U32 sOldDrawNameLookups = 0, sOldUnitNameLookups = 0;
    U32 sOldNamedDraws = 0, sOldNamedSamplers = 0;
    constexpr U32 OLD_NAMES_KEPT = 8;
    GLuint sOldNames[OLD_NAMES_KEPT] = {};
    U32 sOldNamesKept = 0;

    void note_old_name(GLuint name)
    {
        ++sOldNamedSamplers;
        if (sOldNamesKept == OLD_NAMES_KEPT)
        {
            return;
        }
        for (U32 i = 0; i < sOldNamesKept; ++i)
        {
            if (sOldNames[i] == name)
            {
                return;
            }
        }
        sOldNames[sOldNamesKept++] = name;
    }
} // namespace

    std::string old_texture_path_report()
    {
        std::ostringstream out;
        out << "slot publish lookups " << sOldDrawNameLookups + sOldUnitNameLookups << " (draw names " << sOldDrawNameLookups
            << ", unit bindings " << sOldUnitNameLookups << "; hits " << sPublishCounts.hits << "), GL-name direct draws " << sOldNamedDraws
            << " (samplers " << sOldNamedSamplers << "; first names of the run";
        for (U32 i = 0; i < sOldNamesKept; ++i)
        {
            out << " " << sOldNames[i];
        }
        out << (sOldNamesKept ? ")" : " none)");
        sOldDrawNameLookups = sOldUnitNameLookups = sOldNamedDraws = sOldNamedSamplers = 0;
        return out.str();
    }
    // The draw's push constants: the table slots of its rewritten 2D samplers
    // (tSlots, tBindlessSampled) and its matrix row after them. Per draw, also
    // when the pass cache keeps the rest of the draw's state (research/39 N3).
    void gather_slots(Program& program, const ResolvedTarget& target)
    {
        tBindlessSampled.clear();
        tPaletteFull = false;
        const bool slot_cache = LLRHI::sSlotCache;
        if (sPublishFrame != sFrame)
        {
            slot_publish_frame();   // research/180: once a frame, before any draw takes a published slot
        }

        // Rewritten 2D samplers: each unit's texture keeps its table slot; the
        // slot numbers go out as push constants when the draw is recorded.
        PerfScope slots_scope(PERF_SLOTS);
        tSlotCount = (U32)program.bindless.size();
        bool named_by_gl = false;   // research/218 vk-s7f: a texture of the direct draw by its GL name
        for (U32 i = 0; i < tSlotCount; ++i)
        {
            Program::BindlessSampler& sampler = program.bindless[i];
            U32 slot = LLVKBindless::WHITE_SLOT;
            tSlotNames[i] = 0;
            tSlotViews[i] = VK_NULL_HANDLE;
            const bool from_draw = tDirect && sampler.unit >= 0 && (U32)sampler.unit < tDirect->texture_count &&
                                   tDirect->textures[sampler.unit] != LLRHI::DirectDraw::KEEP_UNIT;
            // research/197 stage 1a: a draw handle names the draw's texture by
            // its number. The number's published slot (I1..I6, R1, R2) is taken
            // without reading the texture, as a published name's is below;
            // otherwise the number's image is resolved as a name would be.
            const U32 ref = from_draw && LLRHI::isTexHandle(tDirect->textures[sampler.unit])
                                ? LLRHI::texHandleRef(tDirect->textures[sampler.unit]).id : 0;
            if (from_draw && !ref && tDirect->textures[sampler.unit])
            {
                note_old_name(tDirect->textures[sampler.unit]);     // research/218 vk-s7f: by GL name (the old path)
                named_by_gl = true;
            }
            U32 ref_published = 0;
            if (ref)
            {
                const TexRefEntry* entry = texref_entry(ref);
                ref_published = entry ? entry->pub.slot1 : 0;
                if (texref_take(ref_published, tSlots[i]))
                {
                    tSlotNames[i] = tDirect->textures[sampler.unit];
                    continue;
                }
            }
            // research/180 stage 1: a published name's slot is what everything
            // below would give (I1..I6): taken without reading the texture. Not
            // in tBindlessSampled: it is readable (I4) and never attached (I5).
            U32 published = 0;
            if (sPublishOn && !ref && sampler.unit >= 0 && sampler.unit < 32)
            {
                const GLuint name = from_draw ? tDirect->textures[sampler.unit] : sUnits[sampler.unit].names[target_slot(GL_TEXTURE_2D)];
                ++(from_draw ? sOldDrawNameLookups : sOldUnitNameLookups);  // research/218 vk-s7f
                published = published_slot(name);
                if (slot_publish_take(published, tSlots[i]))
                {
                    tSlotNames[i] = name;
                    continue;
                }
            }
            const Texture* bound = !from_draw && sampler.unit >= 0 && sampler.unit < 32 ? unit_texture((U32)sampler.unit, GL_TEXTURE_2D) : nullptr;
            if (!from_draw && sampler.unit >= 0 && sampler.unit < 32 && sampler.cached_frame == sFrame &&
                sampler.cached_unit == sampler.unit && sampler.cached_binding == sUnitBindings[sampler.unit] &&
                sampler.cached_bound == bound && (!bound || sampler.cached_version == bound->version) &&
                ((target.stamp && sampler.cached_target == target.stamp) ||
                 // Research/58: a texture (not the stand-in) reads the same into
                 // any target it is not attached to (the target is all the
                 // resolution below depends on besides the texture's version).
                 (slot_cache && sampler.cached_texture && sampler.cached_texture == bound && !is_attachment_of(target, bound))))
            {
                // Same binding as the last draw (the stand-in, if any, was made current this frame).
                tSlots[i] = sampler.cached_slot;
                tSlotNames[i] = sampler.cached_name;
                tSlotViews[i] = sampler.cached_view;
                if (sampler.cached_texture)
                {
                    tBindlessSampled.push_back(sampler.cached_texture);
                }
                ++sSlotsUnitCached;
                continue;
            }
            Texture* sampled = nullptr;
            bool resolved = false;      // research/180: LLVKBindless::use ran for it (slot_publish_after)
            if (sampler.unit >= 0 && sampler.unit < 32)
            {
                GLuint name = 0;
                Texture* tex = nullptr;
                if (from_draw)
                {
                    name = handle_image(tDirect->textures[sampler.unit]);   // the draw's own texture (research/197 1a: a number's image)
                    tex = name ? sTextures.find(name) : nullptr;
                }
                else
                {
                    name = sUnits[sampler.unit].names[target_slot(GL_TEXTURE_2D)];
                    tex = unit_texture((U32)sampler.unit, GL_TEXTURE_2D);
                }
                if (tex && (from_draw || slot_cache) && texture_slot_cached(*tex, target, slot_cache))
                {
                    // Already resolved for an earlier draw of this frame: what
                    // the resolution below would find again (same version:
                    // same image, levels, view and sampler; not attached here).
                    slot = tex->bindless_slot;
                    tSlotNames[i] = name;
                    tSlotViews[i] = tex->direct_view;
                    tBindlessSampled.push_back(tex);
                    sampled = tex;
                    ++sSlotsFrameCached;
                }
                // Sampling a texture that is also being written: GL leaves this undefined.
                else if (tex && tex->image && tex->defined_levels && !is_attachment_of(target, tex))
                {
                    ++sSlotsResolved;
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
                            resolved = true;
                        }
                        slot = tex->bindless_slot;
                        tSlotNames[i] = name;
                        tSlotViews[i] = view;
                        if (slot != LLVKBindless::WHITE_SLOT)
                        {
                            tBindlessSampled.push_back(tex);
                            sampled = tex;
                            if (from_draw || slot_cache)
                            {
                                tex->direct_frame = sFrame;
                                tex->direct_version = tex->version;
                                tex->direct_target = target.stamp;
                                tex->direct_view = view;
                            }
                        }
                        else
                        {
                            tex->bindless_slot = U32_MAX;   // table full: try again next time
                        }
                    }
                }
                if (ref)
                {
                    tSlotNames[i] = tDirect->textures[sampler.unit];    // research/197 1a: what the draw named it by (packets compare it)
                    texref_after(ref, tex, slot, ref_published, resolved);
                }
                if (sPublishOn && name && !ref)
                {
                    slot_publish_after(name, tex, slot, published, resolved);
                }
            }
            if (slot == LLVKBindless::WHITE_SLOT && sWhiteFrame != sFrame)
            {
                LLVKBindless::slotFor(0);   // the stand-in, once per frame
                sWhiteFrame = sFrame;
            }
            tSlots[i] = slot;
            if (!from_draw && sampler.unit >= 0 && sampler.unit < 32)
            {
                sampler.cached_frame = sFrame;
                sampler.cached_unit = sampler.unit;
                sampler.cached_binding = sUnitBindings[sampler.unit];
                sampler.cached_bound = bound;
                sampler.cached_version = bound ? bound->version : 0;
                sampler.cached_target = target.stamp;
                sampler.cached_slot = slot;
                sampler.cached_name = tSlotNames[i];
                sampler.cached_view = tSlotViews[i];
                sampler.cached_texture = sampled;
            }
        }
        if (named_by_gl)
        {
            ++sOldNamedDraws;
        }
        if (tSlotCount)
        {
            if (sBindlessDebug)
            {
                program.bindless_slots.assign(tSlots, tSlots + tSlotCount);     // read only by the debug report
            }
            LLVKBindless::flush();
        }
        if (program.matrix_mask)
        {
            // The row index goes after the slot numbers (llrhi_slots.matrix).
            tSlots[tSlotCount++] = matrix_row(program);
        }
        if (program.palette_columns)
        {
            // The palette's first vec4 after them (llrhi_slots.palette).
            tSlots[tSlotCount++] = palette_row(program);
        }
        if (program.light_table)
        {
            // The light table's first row (llrhi_slots.light): the direct
            // draw's, else none (the shader reads its uniforms).
            tSlots[tSlotCount++] = tDirect ? tDirect->table_base : LLRHI::DirectDraw::NO_TABLE;
        }
        if (program.material_vec4s)
        {
            // The material row's first vec4 (llrhi_slots.material).
            tSlots[tSlotCount++] = material_row(program);
        }
        if (program.consts_vec4s)
        {
            // The constants row's first vec4 last (llrhi_slots.consts, research/50 step 3).
            tSlots[tSlotCount++] = consts_row(program);
        }
        if (program.draw_rows)
        {
            // The rows word after everything (llrhi_slots.rows, research/46 G4b):
            // NO_TABLE, the shaders read the words above; a draw run pushes its
            // own and writes the words above into each draw's row (run_draw).
            tSlots[tSlotCount++] = LLRHI::DirectDraw::NO_TABLE;
        }
    }

    // The program's default uniform block in the frame's ring, copied once per
    // change per frame (not for direct draws that write their matrices into it).
    // size: as the block's descriptor declares it.
    bool default_block_copy(Program& program, VkDeviceSize size, VkBuffer& buffer, VkDeviceSize& offset)
    {
        if (program.ubo_frame == sFrame && program.ubo_version == program.uniform_version)
        {
            buffer = program.ubo_buffer;
            offset = program.ubo_offset;
            ++sUboReused;
            return true;
        }
        const VkDeviceSize ubo_align = LLVKDevice::instance().getLimits().minUniformBufferOffsetAlignment;
        void* dst = nullptr;
        if (!LLVKMirror::instance().allocate(size, ubo_align, buffer, offset, dst))
        {
            return false;
        }
        memset(dst, 0, (size_t)size);
        memcpy(dst, program.uniform_data.data(), llmin(program.uniform_data.size(), (size_t)size));
        program.ubo_buffer = buffer;
        program.ubo_offset = offset;
        program.ubo_frame = sFrame;
        program.ubo_version = program.uniform_version;
        ++sUboCopies;
        return true;
    }

    bool gather_descriptors(Program& program, ResolvedTarget& target)
    {
        LLVKMirror& mirror = LLVKMirror::instance();
        const VkDeviceSize ubo_align = LLVKDevice::instance().getLimits().minUniformBufferOffsetAlignment;
        tWrites.clear();
        tBuffers.clear();
        tImages.clear();
        tSampled.clear();
        tSampledUsed = &tSampled;
        tKey.clear();
        tHasDynamic = false;
        tDynamicOffset = 0;
        tDynamicBuffer = VK_NULL_HANDLE;

        if (LLVKPassState::tUnitsLeft)
        {
            LLVKPassState::noteDraw(program);   // research/204 10 (vk-s4c2): the scene's pass heads gather here too
        }
        gather_slots(program, target);
        PerfScope ubo_scope(PERF_UBO);
        // Pointers into these vectors are taken below: size them up front.
        tBuffers.reserve(program.vk.blocks.size());
        tImages.reserve(program.image_count);

        for (size_t i = 0; i < program.vk.blocks.size(); ++i)
        {
            const auto& block = program.vk.blocks[i];
            if (block.binding < 0)
            {
                continue;
            }
            VkDeviceSize size = (VkDeviceSize)llmax(block.size, 16);
            VkBuffer buffer = VK_NULL_HANDLE;
            VkDeviceSize offset = 0;
            U64 serial = 0;     // research/156 A-DS: of a GL buffer's Vulkan copy (the ring is never remade)
            if ((S32)i == program.default_block)
            {
                // The default block only changes through glUniform*: copy it once per change per frame.
                const bool direct_matrices = tDirect && direct_writes_block(program);
                if (!direct_matrices)
                {
                    if (!default_block_copy(program, size, buffer, offset))
                    {
                        return false;
                    }
                }
                else if (tDirect->matrix_id && program.direct_ubo_frame == sFrame &&
                         program.direct_ubo_version == program.uniform_version && program.direct_ubo_matrix_id == tDirect->matrix_id &&
                         program.direct_ubo_texture_matrix == tDirect->texture_matrix0)
                {
                    buffer = program.direct_ubo_buffer;
                    offset = program.direct_ubo_offset;
                    ++sUboReused;
                }
                else
                {
                    void* dst = nullptr;
                    if (!mirror.allocate(size, ubo_align, buffer, offset, dst))
                    {
                        return false;
                    }
                    memset(dst, 0, (size_t)size);
                    memcpy(dst, program.uniform_data.data(), llmin(program.uniform_data.size(), (size_t)size));
                    // This draw's matrices: the copy serves direct draws with the same ones.
                    write_direct_matrices(program, (U8*)dst, (size_t)size);
                    program.direct_ubo_frame = sFrame;
                    program.direct_ubo_version = program.uniform_version;
                    program.direct_ubo_matrix_id = tDirect->matrix_id;
                    program.direct_ubo_texture_matrix = tDirect->texture_matrix0;
                    program.direct_ubo_buffer = buffer;
                    program.direct_ubo_offset = offset;
                    ++sUboCopies;
                }
            }
            else
            {
                // Uniform blocks backed by GL buffers bind that buffer's GPU copy directly.
                Buffer* source = nullptr;
                GLintptr source_offset = 0;
                S32 point = i < program.block_point.size() ? program.block_point[i] : -1;
                if (point >= 0 && (U32)point < MAX_UNIFORM_BINDINGS)
                {
                    UniformBinding& binding = sUniformBindings[point];
                    if (binding.buffer && binding.resolved_at != sBufferDeletes)
                    {
                        binding.resolved = sBuffers.find(binding.buffer);
                        binding.resolved_at = sBufferDeletes;
                    }
                    if (binding.buffer && binding.resolved)
                    {
                        source = binding.resolved;
                        source_offset = binding.offset;
                        source->uniform_source = true;  // its writes now move sUniformBlockState
                    }
                }
                if (source && (size_t)source_offset + (size_t)size <= source->data.size() && source_offset % ubo_align == 0)
                {
                    buffer = gpu_buffer(*source);
                    offset = (VkDeviceSize)source_offset;
                    serial = source->gpu_serial;
                }
                if (!buffer)
                {
                    void* dst = nullptr;
                    if (!mirror.allocate(size, ubo_align, buffer, offset, dst))
                    {
                        return false;
                    }
                    memset(dst, 0, (size_t)size);
                    if (source && (size_t)source_offset < source->data.size())
                    {
                        memcpy(dst, source->data.data() + source_offset, llmin((size_t)size, source->data.size() - source_offset));
                    }
                    ++sUboCopies;
                }
            }
            const bool dynamic = (S32)i == program.default_block;
            if (dynamic)
            {
                tHasDynamic = true;
                tDynamicOffset = (U32)offset;
                tDynamicBuffer = buffer;
                offset = 0;
            }
            tBuffers.push_back({ buffer, offset, size });
            tKey.push_back((U64)buffer);
            tKey.push_back(offset);
            if (sSetPersist)
            {
                tKey.push_back(buffer ? serial : 0);   // kept sets: a buffer made again under the same handle differs
            }
            VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            write.dstBinding = block.binding;
            write.descriptorCount = 1;
            write.descriptorType = dynamic ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            write.pBufferInfo = &tBuffers.back();
            tWrites.push_back(write);
        }

        ubo_scope.stop();
        PerfScope images_scope(PERF_IMAGES);
        // Stencil shares the attachment: a draw that writes stencil needs it writable too.
        // In depth_state so that the reuse below makes the same read-only choice as the loop.
        const bool stencil_writes = sState.stencil && target.target.stencil &&
                                    (sState.stencil_front.write_mask || sState.stencil_back.write_mask);
        const U32 depth_state = (sState.depth_test ? 1u : 0u) | (sState.depth_write ? 2u : 0u) | (stencil_writes ? 4u : 0u);
        U64 units = LLRHI::FNV1A_BASIS_SHORT;
        for (GLint unit : program.unit_values)
        {
            const U32 binding = unit >= 0 && unit < 32 ? sUnitBindings[unit] : 0;
            units = LLRHI::fnv1aMix(units, ((U64)(U32)unit << 32) | binding);
            if (unit >= 0 && unit < 32)
            {
                const UnitBindings& u = sUnits[unit];
                for (U32 s = 0; s < 6; ++s)
                {
                    if (u.names[s])
                    {
                        const Texture* tex = unit_texture((U32)unit, s == 0 ? GL_TEXTURE_2D : s == 1 ? GL_TEXTURE_CUBE_MAP :
                                                                     s == 2 ? GL_TEXTURE_CUBE_MAP_ARRAY : s == 3 ? GL_TEXTURE_2D_ARRAY :
                                                                     s == 4 ? GL_TEXTURE_3D : 0);
                        units = LLRHI::fnv1aMix(units, ((U64)s << 60) | (tex ? tex->version : 0xffffffffu));
                        if (sSetPersist && tex)
                        {
                            units = LLRHI::fnv1aMix(units, tex->view_serial);  // research/156 A-DS: its view as made now
                        }
                    }
                }
            }
        }
        // research/156 A-DS (LLRHI_SET_PERSIST=1): also from an earlier frame
        // (made for this link: U32_MAX after one; the units' hash has every
        // texture's version and view serial).
        const bool images_current = sSetPersist ? program.images_frame != U32_MAX : program.images_frame == sFrame;
        if (images_current && program.images_units == units &&
            program.images_target_stamp == target.stamp && program.images_depth_state == depth_state && target.stamp)
        {
            tSampledUsed = &program.sampled;
            tKey.insert(tKey.end(), program.image_key.begin(), program.image_key.end());
            target.target.depth_read_only |= program.images_depth_read_only;
            size_t first = 0;
            for (const SamplerSlot& slot : program.sampler_slots)
            {
                VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                write.dstBinding = slot.binding;
                write.descriptorCount = slot.count;
                write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                write.pImageInfo = &program.images[first];
                tWrites.push_back(write);
                first += slot.count;
            }
            ++sImagesReused;
            return true;
        }
        const size_t key_images_begin = tKey.size();
        const bool depth_read_only_before = target.target.depth_read_only;

        for (const SamplerSlot& slot : program.sampler_slots)
        {
            size_t first = tImages.size();
            for (U32 e = 0; e < slot.count; ++e)
            {
                GLint unit = program.unit_values[slot.first + e];
                Texture* tex = nullptr;
                VkImageView view = VK_NULL_HANDLE;
                if (unit >= 0 && unit < 32)
                {
                    Texture* bound = unit_texture((U32)unit, slot.target);
                    // Shadow samplers need depth; plain samplers read color, or depth as values.
                    if (bound && (bound->format.depth || !slot.shadow))
                    {
                        tex = bound;
                        view = texture_view(*tex);
                    }
                }
                VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                if (tex && view && is_attachment_of(target, tex))
                {
                    if (tex == target.depth && !(sState.depth_test && sState.depth_write) && !stencil_writes)
                    {
                        // Depth tested but not written while sampled: legal as a read-only attachment.
                        target.target.depth_read_only = true;
                        layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
                    }
                    else
                    {
                        // Sampling a texture that is also being written: GL leaves this undefined.
                        skip(tex == target.depth ? "feedback loop on written depth, used stand-in"
                                                 : "feedback loop on color target, used stand-in");
                        unhandled("feedback loop through sampler '" + slot.name + (tex == target.depth ? "' (depth, writes on)" : "' (color)"),
                                  ", first in program " + std::to_string(sCurrentProgram));
                        view = VK_NULL_HANDLE;
                    }
                }
                if (!view)
                {
                    tex = dummy_texture(slot.view_type, slot.shadow);
                    if (!tex)
                    {
                        return false;
                    }
                    view = texture_view(*tex);
                    if (!view)
                    {
                        return false;   // memory full: no stand-in view either (fix-round95 C)
                    }
                }
                VkSampler sampler = sampler_for(tex, slot.shadow);
                tSampled.push_back(tex);
                tImages.push_back({ sampler, view, layout });
                tKey.push_back((U64)view);
                tKey.push_back((U64)sampler ^ ((U64)layout << 56));
                if (sSetPersist)
                {
                    tKey.push_back(tex->view_serial);   // research/156 A-DS (samplers are never destroyed)
                }
            }
            VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            write.dstBinding = slot.binding;
            write.descriptorCount = slot.count;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &tImages[first];
            tWrites.push_back(write);
        }

        // Hand the lists to the program without copying: swap keeps the elements
        // where they are, so the writes' pImageInfo pointers stay valid.
        program.images.swap(tImages);
        program.sampled.swap(tSampled);
        tSampledUsed = &program.sampled;
        program.image_key.assign(tKey.begin() + key_images_begin, tKey.end());
        program.images_depth_read_only = target.target.depth_read_only && !depth_read_only_before;
        program.images_frame = sFrame;
        program.images_texture_state = sTextureState;
        program.images_units = units;
        program.images_target_stamp = target.stamp;
        program.images_depth_state = depth_state;
        return true;
    }

    Recorded sRecorded;

    // The draw being recorded with an OIT variant (research/41), an
    // OITDrawMode: no depth writes whatever GL says (the lists are tested
    // against the opaque depth only; the split draws' GL state writes none).
    thread_local U8 tOITDraw = 0;

    void set_dynamic_state(VkCommandBuffer cmd, const LLVKMirror::Target& target, VkPrimitiveTopology prim)
    {
        record_dynamic_values(cmd, sRecorded, dynamic_values(target, prim));
    }

    DynamicValues dynamic_values(const LLVKMirror::Target& target, VkPrimitiveTopology prim)
    {
        const State& s = sState;
        DynamicValues v;
        v.viewport = { (F32)s.viewport[0], (F32)s.viewport[1], (F32)llmax(1, s.viewport[2]), (F32)llmax(1, s.viewport[3]), 0.f, 1.f };
        v.scissor = { { 0, 0 }, { target.width, target.height } };
        if (s.scissor)
        {
            S32 x0 = llclamp(s.scissor_rect[0], 0, (S32)target.width);
            S32 y0 = llclamp(s.scissor_rect[1], 0, (S32)target.height);
            S32 x1 = llclamp(s.scissor_rect[0] + s.scissor_rect[2], 0, (S32)target.width);
            S32 y1 = llclamp(s.scissor_rect[1] + s.scissor_rect[3], 0, (S32)target.height);
            v.scissor = { { x0, y0 }, { (U32)llmax(0, x1 - x0), (U32)llmax(0, y1 - y0) } };
        }
        v.cull = !s.cull ? VK_CULL_MODE_NONE : s.cull_face == GL_FRONT ? VK_CULL_MODE_FRONT_BIT :
                 s.cull_face == GL_FRONT_AND_BACK ? VK_CULL_MODE_FRONT_AND_BACK : VK_CULL_MODE_BACK_BIT;
        // GL counter-clockwise reads as clockwise in Vulkan's framebuffer orientation.
        v.front = s.front_face == GL_CCW ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
        v.prim = prim;

        // GL only writes depth while the depth test is enabled. OIT nodes never do.
        v.depth_test = s.depth_test && target.depth;
        v.depth_write = v.depth_test && s.depth_write && !tOITDraw;
        v.depth_op = compare_op(s.depth_func);
        // LLRHI::sShadowDepthBias (plus/shadowdist): the sun shadow casters' slope-scaled
        // bias, in place of GL's polygon offset while it is active.
        const LLRHI::ShadowDepthBias& shadow_bias = shadow_depth_bias();
        v.bias_enable = shadow_bias.active ? VK_TRUE : (VkBool32)s.polygon_offset;
        v.bias_units = shadow_bias.active ? shadow_bias.constant : s.offset_units;
        v.bias_factor = shadow_bias.active ? shadow_bias.slope : s.offset_factor;
        v.bias_clamp = shadow_bias.active && LLRHI::sShadowDepthBiasClamp ? shadow_bias.clamp : 0.f;

        v.stencil_test = s.stencil && target.stencil;
        const Stencil* faces[2] = { &s.stencil_front, &s.stencil_back };
        for (int f = 0; f < 2; ++f)
        {
            const U32 now[7] = { faces[f]->fail, faces[f]->pass, faces[f]->depth_fail, faces[f]->func,
                                 faces[f]->compare_mask, faces[f]->write_mask, (U32)faces[f]->ref };
            memcpy(v.stencil[f], now, sizeof(now));
        }
        return v;
    }

    void record_dynamic_values(VkCommandBuffer cmd, Recorded& r, const DynamicValues& v)
    {
        const bool all = !r.dynamic;
        r.dynamic = true;

        if (all || !same_bytes(v.viewport, r.viewport))
        {
            vkCmdSetViewport(cmd, 0, 1, &v.viewport);
            r.viewport = v.viewport;
        }
        if (all || !same_bytes(v.scissor, r.scissor))
        {
            vkCmdSetScissor(cmd, 0, 1, &v.scissor);
            r.scissor = v.scissor;
        }
        if (all || v.cull != r.cull)
        {
            vkCmdSetCullMode(cmd, v.cull);
            r.cull = v.cull;
        }
        if (all || v.front != r.front)
        {
            vkCmdSetFrontFace(cmd, v.front);
            r.front = v.front;
        }
        if (all || v.prim != r.prim)
        {
            vkCmdSetPrimitiveTopology(cmd, v.prim);
            r.prim = v.prim;
        }

        if (all || v.depth_test != r.depth_test)
        {
            vkCmdSetDepthTestEnable(cmd, v.depth_test);
            r.depth_test = v.depth_test;
        }
        if (all || v.depth_write != r.depth_write)
        {
            vkCmdSetDepthWriteEnable(cmd, v.depth_write);
            r.depth_write = v.depth_write;
        }
        if (all || v.depth_op != r.depth_op)
        {
            vkCmdSetDepthCompareOp(cmd, v.depth_op);
            r.depth_op = v.depth_op;
        }
        if (all || v.bias_enable != r.bias_enable)
        {
            vkCmdSetDepthBiasEnable(cmd, v.bias_enable);
            r.bias_enable = v.bias_enable;
        }
        if (all || v.bias_units != r.bias_units || v.bias_factor != r.bias_factor || v.bias_clamp != r.bias_clamp)
        {
            vkCmdSetDepthBias(cmd, v.bias_units, v.bias_clamp, v.bias_factor);
            r.bias_units = v.bias_units;
            r.bias_factor = v.bias_factor;
            r.bias_clamp = v.bias_clamp;
        }

        if (all || v.stencil_test != r.stencil_test)
        {
            vkCmdSetStencilTestEnable(cmd, v.stencil_test);
            r.stencil_test = v.stencil_test;
        }
        const VkStencilFaceFlags flags[2] = { VK_STENCIL_FACE_FRONT_BIT, VK_STENCIL_FACE_BACK_BIT };
        for (int f = 0; f < 2; ++f)
        {
            const U32* now = v.stencil[f];  // fail, pass, depth fail, function, compare mask, write mask, reference
            if (!all && same_words(now, r.stencil[f], 7))
            {
                continue;
            }
            vkCmdSetStencilOp(cmd, flags[f], stencil_op(now[0]), stencil_op(now[1]), stencil_op(now[2]), compare_op(now[3]));
            vkCmdSetStencilCompareMask(cmd, flags[f], now[4]);
            vkCmdSetStencilWriteMask(cmd, flags[f], now[5]);
            vkCmdSetStencilReference(cmd, flags[f], now[6]);
            memcpy(r.stencil[f], now, sizeof(r.stencil[f]));
        }
    }

    // ------------------------------------------------------------------
    // ------------------------------------------------------------------
    // research/156 A-DS: descriptor sets kept across frames (LLRHI_SET_PERSIST=1).
    // The frame's pools are emptied at every frame start, so each program's
    // first draw of a frame used to allocate and write its set again (1.6k a
    // frame at a heavy place). Kept sets live in the mirror's persistent pools,
    // found by their contents: tKey with the serial of every GL buffer copy and
    // view in it, so a destroyed object whose handle value comes back never
    // matches. Sets no longer asked for stay in their pool until all the pools
    // are full; then they all go together (retired after the frames that may
    // bind them) and every program starts again.
    const bool sSetPersist = LLRHI::envFirstChar("LLRHI_SET_PERSIST") == '1';
    U32 sPersistGeneration = 0;
    U32 sPersistHits = 0, sPersistNew = 0, sPersistRetires = 0;
namespace
{
    std::atomic<U64> sObjectSerials{ 0 };

    void persist_reset(Program& program)
    {
        program.persist_table.clear();
        program.persist_count = 0;
        program.persist_last = U32_MAX;
        program.persist_generation = sPersistGeneration;
    }
} // namespace
    U64 next_object_serial()
    {
        return sObjectSerials.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    // The program's set for tKey / tWrites (gather_descriptors), kept or made;
    // null when none could be allocated.
    VkDescriptorSet persistent_set(Program& program)
    {
        if (program.persist_generation != sPersistGeneration)
        {
            persist_reset(program);
        }
        if (program.persist_last < program.persist_count && program.persist_entries[program.persist_last].first == tKey)
        {
            ++sPersistHits;
            ++sSetsReused;
            return program.persist_entries[program.persist_last].second;
        }
        const U64 hash = LLRHI::fnv1aWords(LLRHI::FNV1A_BASIS_SHORT, tKey.data(), tKey.size());
        U32* found = program.persist_table.find(hash);
        if (found && program.persist_entries[*found].first == tKey)
        {
            program.persist_last = *found;
            ++sPersistHits;
            ++sSetsCached;
            return program.persist_entries[*found].second;
        }
        LLVKMirror& mirror = LLVKMirror::instance();
        VkDescriptorSet set = mirror.allocatePersistentSet(program.set_layout);
        if (!set)
        {
            // Every pool full: they all go (this frame's draws keep theirs until it is done).
            ++sPersistGeneration;
            ++sPersistRetires;
            mirror.retirePersistentSets();
            persist_reset(program);
            found = nullptr;
            set = mirror.allocatePersistentSet(program.set_layout);
            if (!set)
            {
                return VK_NULL_HANDLE;
            }
        }
        for (VkWriteDescriptorSet& write : tWrites)
        {
            write.dstSet = set;
        }
        vkUpdateDescriptorSets(LLVKDevice::instance().getDevice(), (U32)tWrites.size(), tWrites.data(), 0, nullptr);
        // Another key with the same hash takes over its entry, as in set_cache.
        U32 entry = 0;
        if (found)
        {
            entry = *found;
        }
        else
        {
            entry = program.persist_count++;
            program.persist_table.set(hash, entry);
            if (entry == program.persist_entries.size())
            {
                program.persist_entries.emplace_back();
            }
        }
        program.persist_entries[entry].first.assign(tKey.begin(), tKey.end());
        program.persist_entries[entry].second = set;
        program.persist_last = entry;
        ++sPersistNew;
        ++sSetsAllocated;
        return set;
    }

    // Pass cache (research/39 N3): a direct draw whose pass state is the same as
    // the last draw of its program recorded through the full path reuses that
    // draw's pipeline, descriptor set (images included), barriers and dynamic
    // state. Per draw it records only its push constants (texture slots, matrix
    // row), vertex and index buffers and the draw, plus:
    // - when only the program's default uniform block changed: the block's new
    //   copy in the ring, and the same set bound at the new dynamic offset
    //   (a uniform hit);
    // - when the command buffer has another program's pipeline or set bound
    //   (draws of other programs came in between): the cached pipeline and set
    //   bound again. Dynamic state is sent where it differs, as for every hit.
    // One entry per program (Program::pass), so programs that take turns (the
    // alpha pool) keep theirs. The key is made of change counters; anything that
    // may change what the full path would record must move one of them.
    // LLRHI_PASS_CACHE=0 sends every draw through the full path.
    const bool sPassCacheEnabled = LLRHI::envFirstChar("LLRHI_PASS_CACHE") != '0';

    U32 sPassHits = 0, sPassMisses = 0;
    U32 sPassUniformHits = 0;           // hits that copied the default block (also in sPassHits)
    U32 sPassRebinds = 0;               // hits that bound their pipeline or set again (also in sPassHits)
    U32 sPassUnitsKept = 0;             // units tests passed by the recheck (LLRHI::sPassUnitsRecheck; a later test may still miss)
namespace
{

    bool same_stencil(const Stencil& a, const Stencil& b)
    {
        return a.func == b.func && a.ref == b.ref && a.compare_mask == b.compare_mask && a.write_mask == b.write_mask &&
               a.fail == b.fail && a.depth_fail == b.depth_fail && a.pass == b.pass;
    }

    // Everything of the GL state a draw's pipeline, dynamic state or images depend
    // on, except the viewport and scissor: a cached draw re-records those
    // (set_dynamic_state sends only what differs), so UI clipping still hits.
    //
    // Cull mode, front face, depth function and polygon offset are dynamic state
    // only: a hit sends them again (set_dynamic_state diffs against sRecorded), so
    // a GLTF material's LLGLDisable(GL_CULL_FACE) around a draw still hits. Depth
    // test / write and stencil stay: gather_descriptors reads them (read-only depth).
    bool same_draw_state(const State& a, const State& b)
    {
        return a.blend == b.blend && a.blend_src_rgb == b.blend_src_rgb && a.blend_dst_rgb == b.blend_dst_rgb &&
               a.blend_src_a == b.blend_src_a && a.blend_dst_a == b.blend_dst_a && a.blend_eq_rgb == b.blend_eq_rgb &&
               a.blend_eq_a == b.blend_eq_a && same_bytes(a.color_mask, b.color_mask) &&
               a.depth_test == b.depth_test && a.depth_write == b.depth_write &&
               a.depth_clamp == b.depth_clamp &&
               a.polygon_mode == b.polygon_mode && a.stencil == b.stencil && same_stencil(a.stencil_front, b.stencil_front) &&
               same_stencil(a.stencil_back, b.stencil_back) && a.draw_framebuffer == b.draw_framebuffer;
    }

    // What the program's descriptor samplers read, as gather_descriptors looks
    // it up: per element its unit, texture and version. Versions are unique
    // across textures (next_texture_version) and move with whatever a sampler
    // of the texture sees (image, levels, view, parameters, the format a
    // shadow sampler checks), so a deleted texture, or one made under its name
    // later, never matches. The units' binding counters are not in it: the
    // key says which textures are read, whoever bound them (speed-pass-units;
    // pass_hit compares the counters before it, as it did when they were in
    // the key).
    U64 sampled_units_key(const Program& program)
    {
        U64 key = LLRHI::FNV1A_BASIS_SHORT;
        for (const SamplerSlot& slot : program.sampler_slots)
        {
            for (U32 e = 0; e < slot.count; ++e)
            {
                const size_t element = (size_t)slot.first + e;
                const GLint unit = element < program.unit_values.size() ? program.unit_values[element] : -1;
                if (unit < 0 || unit >= 32)
                {
                    key = LLRHI::fnv1aMix(key, 0xffffffffffffffffull);
                    continue;
                }
                const Texture* tex = unit_texture((U32)unit, slot.target);
                key = LLRHI::fnv1aMix(key, (U64)(U32)unit);
                key = LLRHI::fnv1aMix(key, (U64)(uintptr_t)tex);
                key = LLRHI::fnv1aMix(key, tex ? (U64)tex->version : 0xffffffffffffffffull);
            }
        }
        return key;
    }

} // namespace
    // Whether the draw may reuse its program's cached pass state; PASS_HIT_UNIFORMS:
    // with a new copy of the default block. target: as resolved, before gather_descriptors.
    // why (research/155 T0b): on a miss, its first failing test, in the order
    // of PassMissWhy (counted for every miss: the same tests, made once).
    PassHit pass_hit(Program& program, const ResolvedTarget& target, VkPrimitiveTopology prim, U64 inputs, PassMissWhy* why)
    {
        auto miss = [why](PassMissWhy reason)
        {
            if (why)
            {
                *why = reason;
            }
            return PASS_MISS;
        };
        if (!program.pass || !program.pass->valid)
        {
            return miss(MISS_NO_ENTRY);
        }
        PassCache& p = *program.pass;
        LLVKMirror& mirror = LLVKMirror::instance();
        if (p.program != &program || p.program_name != sCurrentProgram || p.program_changes != sProgramChanges) return miss(MISS_PROGRAM);
        if (p.frame != sFrame) return miss(MISS_FRAME);
        if (p.prim != prim || p.inputs != inputs) return miss(MISS_SHAPE);
        if (p.framebuffer != sState.draw_framebuffer || p.target_state != sTargetState || p.target_stamp != target.stamp ||
            (!target.stamp && p.target != target.target)) return miss(MISS_TARGET);
        if (p.attrib_constants != sAttribConstants) return miss(MISS_ATTRIB_CONSTANTS);
        if (p.layouts != sLayoutState) return miss(MISS_LAYOUTS);
        if (p.scope_epoch != mirror.mScopeEpoch) return miss(MISS_SCOPE);
        if (p.record_epoch != sRecordEpoch) return miss(MISS_RECORD);
        if (p.bindless_moves != LLVKBindless::moves()) return miss(MISS_BINDLESS_MOVES);
        if (!mirror.isRendering()) return miss(MISS_NOT_RENDERING);
        if (p.oit != tOITDraw) return miss(MISS_OIT);
        const PassVersions v = pass_versions(program);
        if (p.unit_version != v.unit_version) return miss(MISS_UNITS);
        if (p.unit_bindings != v.unit_bindings)
        {
            // A unit it samples through a descriptor was bound again (research/167
            // C2b: mostly another program's draws binding their textures there,
            // and the viewer binding these back): still the same if the samplers
            // read the same textures, as the textures test below decides.
            // unit_version is the same, so unit_values are; sampler_slots is
            // not empty (else unit_bindings stays 0).
            if (!LLRHI::sPassUnitsRecheck || p.units != sampled_units_key(program))
            {
                return miss(MISS_UNITS);
            }
            p.unit_bindings = v.unit_bindings;
            p.texture_state = sTextureState;    // the key was confirmed as things are now
            p.texture_deletes = sTextureDeletes;
            ++sPassUnitsKept;
        }
        if (p.uniform_blocks != v.uniform_blocks || p.buffer_deletes != v.buffer_deletes) return miss(MISS_BLOCKS);
        if (!program.sampler_slots.empty() && (p.texture_state != sTextureState || p.texture_deletes != sTextureDeletes))
        {
            // Some texture changed or was deleted: still the same if none of this program's did.
            if (p.units != sampled_units_key(program))
            {
                return miss(MISS_TEXTURES);
            }
            p.texture_state = sTextureState;
            p.texture_deletes = sTextureDeletes;
        }
        if (p.pipeline_state != sPipelineState || p.fixed_state != sFixedState)
        {
            // Something was set since: still the same if it was set back.
            if (!same_draw_state(p.state, sState))
            {
                return miss(MISS_STATE);
            }
            p.pipeline_state = sPipelineState;
            p.fixed_state = sFixedState;
        }
        // What the command buffer has bound is checked when the hit is recorded
        // (the cached pipeline and set are bound again where they differ).
        return p.uniform_version == program.uniform_version ? PASS_HIT : PASS_HIT_UNIFORMS;
    }
namespace
{
    const char* const sPassMissNames[MISS_WHY_COUNT] = { "no entry", "program", "frame", "shape", "target", "attribute constants",
                                                         "layouts", "scope", "record", "bindless moves", "not rendering", "oit",
                                                         "units", "blocks", "textures", "state", "unusable hit" };
} // namespace
    U32 sPassMissWhy[MISS_WHY_COUNT] = {};

    std::string pass_miss_report(U32 frames)
    {
        std::ostringstream out;
        bool first = true;
        for (U32 i = 0; i < MISS_WHY_COUNT; ++i)
        {
            if (sPassMissWhy[i] / frames)
            {
                out << (first ? "" : ", ") << sPassMissNames[i] << " " << sPassMissWhy[i] / frames;
                first = false;
            }
        }
        return out.str();
    }

    std::string pass_miss_columns(U32 frames)
    {
        std::ostringstream out;
        for (U32 i = 0; i < MISS_WHY_COUNT; ++i)
        {
            out << (i ? ", " : "") << sPassMissNames[i] << " " << sPassMissWhy[i] / frames;
        }
        return out.str();
    }

    // After a draw recorded through the full path: its results serve its program's draws after it.
    // target: as the draw used it (gather_descriptors may have made its depth read-only).
    void pass_store(Program& program, const ResolvedTarget& target, VkPrimitiveTopology prim, U64 inputs,
                    VkPipeline pipeline, VkDescriptorSet set)
    {
        if (!program.pass)
        {
            program.pass = std::make_shared<PassCache>();
        }
        PassCache& p = *program.pass;
        p.program = &program;
        p.program_name = sCurrentProgram;
        p.program_changes = sProgramChanges;
        p.frame = sFrame;
        p.pipeline_state = sPipelineState;
        p.fixed_state = sFixedState;
        p.state = sState;
        p.framebuffer = sState.draw_framebuffer;
        p.target_state = sTargetState;
        p.target_stamp = target.stamp;
        p.target = target.target;
        p.target.depth_read_only = false;   // as resolve_draw_target hands it out (pass_hit compares that)
        p.prim = prim;
        p.inputs = inputs;
        const PassVersions v = pass_versions(program);
        p.uniform_version = program.uniform_version;
        p.unit_version = v.unit_version;
        p.unit_bindings = v.unit_bindings;
        p.units = program.sampler_slots.empty() ? 0 : sampled_units_key(program);
        p.texture_state = sTextureState;
        p.texture_deletes = sTextureDeletes;
        p.uniform_blocks = v.uniform_blocks;
        p.buffer_deletes = v.buffer_deletes;
        p.attrib_constants = sAttribConstants;
        p.layouts = sLayoutState;
        p.scope_epoch = LLVKMirror::instance().mScopeEpoch;
        p.record_epoch = sRecordEpoch;
        p.bindless_moves = LLVKBindless::moves();
        p.oit = tOITDraw;
        p.pipeline = pipeline;
        p.set = set;
        p.has_dynamic = tHasDynamic;
        p.dynamic_buffer = tDynamicBuffer;
        p.dynamic_offset = tDynamicOffset;
        p.depth_read_only = target.target.depth_read_only;
        p.valid = true;
    }
} // namespace llvkgl

#endif // LL_GL_FUNC_POINTER

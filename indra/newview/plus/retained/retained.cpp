/**
 * @file retained.cpp
 * @brief Stage 0 of the retained, GPU driven main view: counts only
 *        (LLRHI_RETAINED_CENSUS, research/195 3.1 in the sl-viewer repository).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
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

#include "llviewerprecompiledheaders.h"

#include "retained.h"

#include "lldrawpool.h"
#include "llfetchedgltfmaterial.h"
#include "llframetimer.h"
#include "llrhi.h"
#include "llspatialpartition.h"
#include "llvertexbuffer.h"
#include "llviewercamera.h"
#include "llviewerregion.h"
#include "llviewertexture.h"
#include "pipeline.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>
#include <vector>

extern bool gCubeSnapshot;

namespace plus::retained
{
namespace detail
{
    const bool sOn = !LLRHI::envUnset("LLRHI_RETAINED_CENSUS") && LLRHI::envFirstChar("LLRHI_RETAINED_CENSUS") != '0';
    std::atomic<U32> sDrawInfoMade{ 0 };
    std::atomic<U32> sDrawInfoFreed{ 0 };
    std::atomic<U32> sGeomBuilds{ 0 };
}

namespace
{
    constexpr F32 LOG_PERIOD = 5.f;             // as FrameCost's windows
    constexpr F32 SETTLE_SECONDS = 1.f;         // as RHI_PROBE_SETTLE_SECONDS (pipeline.cpp): unchanged this long before a block
    constexpr F32 KEEP_SECONDS = 30.f;          // as RHI_PROBE_UNUSED_SECONDS: a group out of view this long is forgotten
    constexpr U32 MAX_UNITS = 8;                // LLRHI::ProbeList::MAX_UNITS: textures a block's batch may name

    // The GPU probe list's tables (llvkgpulist.cpp): would the main view fit.
    constexpr U32 G5_MAX_ROWS = 1u << 18;       // PROBE_MAX_ROWS
    constexpr U32 G5_MAX_BLOCK_ROWS = 1u << 12; // PROBE_MAX_BLOCK_ROWS
    constexpr U32 G5_MAX_HANDLES = 0xffffu;     // PROBE_MAX_HANDLES
    constexpr U32 G5_MAX_MODELS = 64;           // MAX_MODELS

    // The stage 1 estimate (research/195 3.2), in microseconds of main thread
    // per frame. Measured: a batch on today's path costs 0.45..0.5 us
    // (packet loop 456..544 ns/batch, viewer side 213..252 + chunk 206..246
    // ns, research/189 11) plus its render map push in postSort (0.05..0.1 ms
    // over about 4,000, research/195 3.2). Guessed: what a block costs (its
    // liveness, its texture names checked, its place in the render map), what
    // a texture it names costs, what a type drawn from blocks costs a frame
    // (prepare, the compute dispatch, each run's first batch the usual way).
    constexpr F64 US_PER_BATCH = 0.48;          // measured (see above)
    constexpr F64 US_PER_BLOCK = 0.35;          // guess
    constexpr F64 US_PER_BLOCK_TEXTURE = 0.03;  // guess
    constexpr F64 US_PER_TYPE = 25.0;           // guess
    constexpr F64 S1_WORTH_MS = 0.5;            // the judgment's threshold

    enum EKind : U8
    {
        KIND_OTHER,
        KIND_GBUFFER,   // opaque, drawn into the G-buffer (renderGeomDeferred)
        KIND_POST,      // opaque, drawn after the lighting (renderGeomPostDeferred: fullbright, glow)
        KIND_ALPHA,
        KIND_RIGGED,
    };

    EKind kind_of(U32 type)
    {
        switch (type)
        {
        case LLRenderPass::PASS_SIMPLE:
        case LLRenderPass::PASS_GRASS:
        case LLRenderPass::PASS_SHINY:
        case LLRenderPass::PASS_BUMP:
        case LLRenderPass::PASS_MATERIAL:
        case LLRenderPass::PASS_MATERIAL_ALPHA_MASK:
        case LLRenderPass::PASS_MATERIAL_ALPHA_EMISSIVE:
        case LLRenderPass::PASS_SPECMAP:
        case LLRenderPass::PASS_SPECMAP_MASK:
        case LLRenderPass::PASS_SPECMAP_EMISSIVE:
        case LLRenderPass::PASS_NORMMAP:
        case LLRenderPass::PASS_NORMMAP_MASK:
        case LLRenderPass::PASS_NORMMAP_EMISSIVE:
        case LLRenderPass::PASS_NORMSPEC:
        case LLRenderPass::PASS_NORMSPEC_MASK:
        case LLRenderPass::PASS_NORMSPEC_EMISSIVE:
        case LLRenderPass::PASS_ALPHA_MASK:
        case LLRenderPass::PASS_GLTF_PBR:
        case LLRenderPass::PASS_GLTF_PBR_ALPHA_MASK:
            return KIND_GBUFFER;
        case LLRenderPass::PASS_FULLBRIGHT:
        case LLRenderPass::PASS_FULLBRIGHT_SHINY:
        case LLRenderPass::PASS_POST_BUMP:
        case LLRenderPass::PASS_GLOW:
        case LLRenderPass::PASS_GLTF_GLOW:
        case LLRenderPass::PASS_FULLBRIGHT_ALPHA_MASK:
            return KIND_POST;
        case LLRenderPass::PASS_ALPHA:
        case LLRenderPass::PASS_MATERIAL_ALPHA:
        case LLRenderPass::PASS_SPECMAP_BLEND:
        case LLRenderPass::PASS_NORMMAP_BLEND:
        case LLRenderPass::PASS_NORMSPEC_BLEND:
            return KIND_ALPHA;
        case LLRenderPass::PASS_SIMPLE_RIGGED:
        case LLRenderPass::PASS_FULLBRIGHT_RIGGED:
        case LLRenderPass::PASS_FULLBRIGHT_SHINY_RIGGED:
        case LLRenderPass::PASS_SHINY_RIGGED:
        case LLRenderPass::PASS_BUMP_RIGGED:
        case LLRenderPass::PASS_POST_BUMP_RIGGED:
        case LLRenderPass::PASS_MATERIAL_RIGGED:
        case LLRenderPass::PASS_MATERIAL_ALPHA_RIGGED:
        case LLRenderPass::PASS_MATERIAL_ALPHA_MASK_RIGGED:
        case LLRenderPass::PASS_MATERIAL_ALPHA_EMISSIVE_RIGGED:
        case LLRenderPass::PASS_SPECMAP_RIGGED:
        case LLRenderPass::PASS_SPECMAP_BLEND_RIGGED:
        case LLRenderPass::PASS_SPECMAP_MASK_RIGGED:
        case LLRenderPass::PASS_SPECMAP_EMISSIVE_RIGGED:
        case LLRenderPass::PASS_NORMMAP_RIGGED:
        case LLRenderPass::PASS_NORMMAP_BLEND_RIGGED:
        case LLRenderPass::PASS_NORMMAP_MASK_RIGGED:
        case LLRenderPass::PASS_NORMMAP_EMISSIVE_RIGGED:
        case LLRenderPass::PASS_NORMSPEC_RIGGED:
        case LLRenderPass::PASS_NORMSPEC_BLEND_RIGGED:
        case LLRenderPass::PASS_NORMSPEC_MASK_RIGGED:
        case LLRenderPass::PASS_NORMSPEC_EMISSIVE_RIGGED:
        case LLRenderPass::PASS_GLOW_RIGGED:
        case LLRenderPass::PASS_GLTF_GLOW_RIGGED:
        case LLRenderPass::PASS_ALPHA_RIGGED:
        case LLRenderPass::PASS_ALPHA_MASK_RIGGED:
        case LLRenderPass::PASS_FULLBRIGHT_ALPHA_MASK_RIGGED:
        case LLRenderPass::PASS_GLTF_PBR_RIGGED:
        case LLRenderPass::PASS_GLTF_PBR_ALPHA_MASK_RIGGED:
            return KIND_RIGGED;
        default:
            return KIND_OTHER;
        }
    }

    const char* type_name(U32 type)
    {
        switch (type)
        {
        case LLRenderPass::PASS_SIMPLE: return "simple";
        case LLRenderPass::PASS_GRASS: return "grass";
        case LLRenderPass::PASS_SHINY: return "shiny";
        case LLRenderPass::PASS_BUMP: return "bump";
        case LLRenderPass::PASS_MATERIAL: return "mat";
        case LLRenderPass::PASS_MATERIAL_ALPHA_MASK: return "mat_mask";
        case LLRenderPass::PASS_MATERIAL_ALPHA_EMISSIVE: return "mat_emis";
        case LLRenderPass::PASS_SPECMAP: return "spec";
        case LLRenderPass::PASS_SPECMAP_MASK: return "spec_mask";
        case LLRenderPass::PASS_SPECMAP_EMISSIVE: return "spec_emis";
        case LLRenderPass::PASS_NORMMAP: return "norm";
        case LLRenderPass::PASS_NORMMAP_MASK: return "norm_mask";
        case LLRenderPass::PASS_NORMMAP_EMISSIVE: return "norm_emis";
        case LLRenderPass::PASS_NORMSPEC: return "normspec";
        case LLRenderPass::PASS_NORMSPEC_MASK: return "normspec_mask";
        case LLRenderPass::PASS_NORMSPEC_EMISSIVE: return "normspec_emis";
        case LLRenderPass::PASS_ALPHA_MASK: return "alpha_mask";
        case LLRenderPass::PASS_GLTF_PBR: return "pbr";
        case LLRenderPass::PASS_GLTF_PBR_ALPHA_MASK: return "pbr_mask";
        case LLRenderPass::PASS_FULLBRIGHT: return "fullbright";
        case LLRenderPass::PASS_FULLBRIGHT_SHINY: return "fb_shiny";
        case LLRenderPass::PASS_POST_BUMP: return "post_bump";
        case LLRenderPass::PASS_GLOW: return "glow";
        case LLRenderPass::PASS_GLTF_GLOW: return "pbr_glow";
        case LLRenderPass::PASS_FULLBRIGHT_ALPHA_MASK: return "fb_mask";
        default: return nullptr;
        }
    }

    // Why a group's batches could not be a block (the group as a whole).
    enum EGroupLeft
    {
        GROUP_BRIDGE,       // a moving linkset or attachment (not the region's matrix)
        GROUP_PARTITION,    // not the volume partition (terrain, trees, particles, avatars...)
        GROUP_DIRTY,        // waiting for a rebuild, or no region
        GROUP_SETTLING,     // changed less than SETTLE_SECONDS ago
        GROUP_LEFT_REASONS
    };
    const char* const GROUP_LEFT_NAMES[GROUP_LEFT_REASONS] = { "bridge", "partition", "dirty", "settling" };

    // Why a batch could not be in a block (rhi_probe_batch_common and
    // rhi_probe_batch in pipeline.cpp, the GPU probe list's own tests).
    enum EBatchLeft
    {
        BATCH_NO_BUFFER,
        BATCH_RIGGED,
        BATCH_NO_BOUNDS,
        BATCH_MATRIX,           // not the region's matrix
        BATCH_TEXTURE_MATRIX,   // texture animation
        BATCH_UNITS,            // more textures than a row names
        BATCH_NULL_TEXTURE,     // a batched unit without a texture
        BATCH_MAPPED,           // the buffer is mapped or not in GL yet (not now)
        BATCH_BLOCK_MATE,       // fine itself, but another batch of its block is not
        BATCH_LEFT_REASONS
    };
    const char* const BATCH_LEFT_NAMES[BATCH_LEFT_REASONS] = { "no buffer", "rigged", "no bounds", "not region space",
                                                               "texture matrix", "too many textures", "null texture",
                                                               "buffer mapped", "block mate" };

    constexpr U32 HISTOGRAM_BINS = 6;   // batches per block: 1, 2, 3-4, 5-8, 9-16, 17+
    const char* const HISTOGRAM_NAMES[HISTOGRAM_BINS] = { "1", "2", "3-4", "5-8", "9-16", "17+" };

    U32 histogram_bin(U32 batches)
    {
        return batches <= 1 ? 0 : batches == 2 ? 1 : batches <= 4 ? 2 : batches <= 8 ? 3 : batches <= 16 ? 4 : 5;
    }

    inline U64 mix(U64 h, U64 v)
    {
        h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        return h;
    }

    // The textures a batch reads (diffuse or its list, normal, specular, the
    // GLTF material's four).
    U32 textures_of(const LLDrawInfo& info)
    {
        U32 count = info.mTextureList.size() > 1 ? (U32)info.mTextureList.size() : (info.mTexture.notNull() ? 1 : 0);
        count += info.mNormalMap.notNull() ? 1 : 0;
        count += info.mSpecularMap.notNull() ? 1 : 0;
        if (const LLFetchedGLTFMaterial* m = info.mGLTFMaterial.get())
        {
            count += m->mBaseColorTexture.notNull() ? 1 : 0;
            count += m->mNormalTexture.notNull() ? 1 : 0;
            count += m->mMetallicRoughnessTexture.notNull() ? 1 : 0;
            count += m->mEmissiveTexture.notNull() ? 1 : 0;
        }
        return count;
    }

    // A texture's GL name (0: none yet).
    U32 tex_name(const LLViewerTexture* tex)
    {
        return tex && tex->hasGLTexture() ? tex->getTexName() : 0;
    }

    // A group as last seen: what its batches were (identity) and since when.
    struct Record
    {
        const LLSpatialGroup* group = nullptr;
        U64 identity = 0;   // its builds, every batch's address, buffer, range and texture
        U64 names = 0;      // its opaque batches' textures' GL names
        F32 since = 0.f;    // when identity was first what it is
        F32 seen = 0.f;     // when last in the main view
        bool operator<(const Record& r) const { return group < r.group; }
    };

    // Summed over the period (per frame: divided by frames; max: the most in a frame).
    struct Counts
    {
        U64 frames = 0;
        F64 census_ms = 0.0;

        U64 groups = 0;             // visible, drawn (not occluded, not auto-hidden)
        U64 groups_static = 0;      // volume partition, not a bridge
        U64 groups_bridge = 0;
        U64 groups_other = 0;
        U64 groups_same = 0;        // seen before (in the last KEEP_SECONDS) with the same batches
        U64 groups_new = 0;         // not seen in the last KEEP_SECONDS
        U64 groups_changed = 0;     // seen before with other batches
        U64 groups_renamed = 0;     // same batches, a texture's GL name changed
        U64 groups_settled = 0;     // same batches for SETTLE_SECONDS or more
        U64 groups_left[GROUP_LEFT_REASONS] = {};

        U64 cull_drawable_groups = 0;   // the cull result's lists the state sort walks
        U64 cull_drawables = 0;
        U64 cull_bridges = 0;
        U64 render_map_opaque = 0;      // render map entries of the opaque types (what the pools draw)

        U64 batches_gbuffer = 0;    // opaque batches (mCount > 0) in the render types drawn
        U64 batches_post = 0;
        U64 batches_alpha = 0;
        U64 batches_rigged = 0;
        U64 batches_other = 0;
        U64 batches_same = 0;       // opaque batches in groups seen before with the same batches
        U64 blocks = 0;             // group x opaque type with batches
        U64 block_hist[HISTOGRAM_BINS] = {};
        U64 tex0 = 0, tex1 = 0, tex2 = 0;   // opaque batches reading 0, 1, 2 or more textures
        U64 lists = 0;              // opaque batches with a texture list (more than one unit)
        U64 buckets = 0;            // distinct (type, shader mask, vertex format) of the opaque batches
        U64 batch_left[BATCH_LEFT_REASONS] = {};

        U64 el_batches_gbuffer = 0; // in blocks stage 1 could make now
        U64 el_batches_post = 0;
        U64 el_blocks = 0;
        U64 el_block_hist[HISTOGRAM_BINS] = {};
        U64 el_block_textures = 0;  // summed per block: its batches' texture reads
        U64 el_types = 0;           // types with such blocks
        U64 el_buckets = 0;

        U64 drawinfo_made = 0;      // everywhere, not only the main view
        U64 drawinfo_freed = 0;
        U64 geom_builds = 0;

        U32 max_rows = 0;           // eligible batches in a frame (the probe list's rows)
        U32 max_block = 0;          // batches of the largest eligible block
        U32 max_textures = 0;       // distinct textures of eligible batches in a frame (its handles)
        U32 max_models = 0;         // region matrices of eligible blocks in a frame

        U64 type_batches[LLRenderPass::NUM_RENDER_TYPES] = {};
        U64 type_blocks[LLRenderPass::NUM_RENDER_TYPES] = {};
        U64 type_el_batches[LLRenderPass::NUM_RENDER_TYPES] = {};
        U64 type_el_blocks[LLRenderPass::NUM_RENDER_TYPES] = {};
    };

    // The census's state, all kept and reused (no allocation once grown).
    struct Census
    {
        std::vector<Record> prev;   // the groups seen in the last KEEP_SECONDS, sorted by address
        std::vector<Record> cur;    // this frame's
        std::vector<Record> merged;
        std::vector<U64> buckets;
        std::vector<U64> el_buckets;
        std::vector<const void*> el_textures;
        std::vector<const LLMatrix4*> el_models;
        Counts counts;
        F32 logged_at = 0.f;
        U32 counted_frame = U32_MAX;
        bool announced = false;
    };
    Census sCensus;

    void count_unique(std::vector<U64>& keys, U64& sum)
    {
        std::sort(keys.begin(), keys.end());
        sum += (U64)(std::unique(keys.begin(), keys.end()) - keys.begin());
    }

    // One group's batches (the render map loop's filters already passed).
    void count_group(LLSpatialGroup* group, Census& c, Counts& n, bool type_used[LLRenderPass::NUM_RENDER_TYPES])
    {
        LLSpatialPartition* part = group->getSpatialPartition();
        const bool bridge = part->asBridge() != nullptr;
        const bool volume = part->mPartitionType == LLViewerRegion::PARTITION_VOLUME && !bridge;
        n.groups_bridge += bridge ? 1 : 0;
        n.groups_static += volume ? 1 : 0;
        n.groups_other += !bridge && !volume ? 1 : 0;

        // What the group's batches are now.
        U64 identity = mix(0, group->rhiBuilds());
        U64 names = 0;
        for (const auto& entry : group->mDrawMap)
        {
            identity = mix(identity, entry.first);
            identity = mix(identity, entry.second.size());
            const EKind kind = kind_of(entry.first);
            for (const LLDrawInfo* info : entry.second)
            {
                identity = mix(identity, (U64)(uintptr_t)info);
                identity = mix(identity, (U64)(uintptr_t)info->mVertexBuffer.get());
                identity = mix(identity, ((U64)info->mOffset << 32) | info->mCount);
                identity = mix(identity, (U64)(uintptr_t)info->mTexture.get());
                if (kind == KIND_GBUFFER || kind == KIND_POST)
                {
                    if (info->mTextureList.size() > 1)
                    {
                        for (const LLPointer<LLViewerTexture>& t : info->mTextureList)
                        {
                            names = mix(names, tex_name(t.get()));
                        }
                    }
                    else
                    {
                        names = mix(names, tex_name(info->mTexture.get()));
                    }
                }
            }
        }

        // Against last frame.
        const F32 now = gFrameTimeSeconds;
        Record rec;
        rec.group = group;
        rec.identity = identity;
        rec.names = names;
        rec.since = now;
        rec.seen = now;
        const auto it = std::lower_bound(c.prev.begin(), c.prev.end(), rec);
        const bool seen = it != c.prev.end() && it->group == group;
        const bool same = seen && it->identity == identity;
        if (same)
        {
            rec.since = it->since;
            ++n.groups_same;
            n.groups_renamed += it->names != names ? 1 : 0;
        }
        else
        {
            n.groups_new += seen ? 0 : 1;
            n.groups_changed += seen ? 1 : 0;
        }
        c.cur.push_back(rec);
        const bool settled = same && now - rec.since >= SETTLE_SECONDS;
        n.groups_settled += settled ? 1 : 0;

        // Whether stage 1 could make blocks of it now.
        const LLViewerRegion* region = part->mRegionp;
        const LLMatrix4* region_matrix = region ? &region->mRenderMatrix : nullptr;
        S32 group_left = bridge                                                                      ? GROUP_BRIDGE
                         : !volume                                                                   ? GROUP_PARTITION
                         : !region || group->hasState(LLSpatialGroup::GEOM_DIRTY | LLSpatialGroup::MESH_DIRTY) ? GROUP_DIRTY
                         : !settled                                                                  ? GROUP_SETTLING
                                                                                                     : -1;
        if (group_left >= 0)
        {
            ++n.groups_left[group_left];
        }

        for (const auto& entry : group->mDrawMap)
        {
            const U32 type = entry.first;
            if (type >= LLRenderPass::NUM_RENDER_TYPES || !gPipeline.hasRenderType(type))
            {
                continue;
            }
            const EKind kind = kind_of(type);
            U32 batches = 0;
            for (const LLDrawInfo* info : entry.second)
            {
                batches += info->mCount ? 1 : 0;
            }
            if (!batches)
            {
                continue;
            }
            if (kind != KIND_GBUFFER && kind != KIND_POST)
            {
                n.batches_alpha += kind == KIND_ALPHA ? batches : 0;
                n.batches_rigged += kind == KIND_RIGGED ? batches : 0;
                n.batches_other += kind == KIND_OTHER ? batches : 0;
                continue;
            }

            // An opaque block: group x type.
            (kind == KIND_GBUFFER ? n.batches_gbuffer : n.batches_post) += batches;
            n.batches_same += same ? batches : 0;
            ++n.blocks;
            ++n.block_hist[histogram_bin(batches)];
            n.type_batches[type] += batches;
            ++n.type_blocks[type];

            U32 block_ok = group_left < 0 ? batches : 0;
            U32 block_textures = 0;
            for (const LLDrawInfo* info : entry.second)
            {
                if (!info->mCount)
                {
                    continue;   // pushBatch draws nothing
                }
                const U32 textures = textures_of(*info);
                block_textures += textures;
                (textures == 0 ? n.tex0 : textures == 1 ? n.tex1 : n.tex2) += 1;
                n.lists += info->mTextureList.size() > 1 ? 1 : 0;
                const LLVertexBuffer* vb = info->mVertexBuffer.get();
                const U64 bucket = mix(mix(type, info->mShaderMask), vb ? vb->getTypeMask() : 0);
                c.buckets.push_back(bucket);
                if (group_left >= 0)
                {
                    continue;
                }
                const S32 left = !vb                                          ? BATCH_NO_BUFFER
                                 : info->mAvatar.notNull()                    ? BATCH_RIGGED
                                 : !info->mHasExtents                         ? BATCH_NO_BOUNDS
                                 : info->mModelMatrix != region_matrix        ? BATCH_MATRIX
                                 : info->mTextureMatrix                       ? BATCH_TEXTURE_MATRIX
                                 : info->mTextureList.size() > MAX_UNITS      ? BATCH_UNITS
                                 : vb->isMapped() || !vb->getGLBuffer() || !vb->getGLIndices() ? BATCH_MAPPED
                                                                              : -1;
                S32 why = left;
                if (why < 0 && info->mTextureList.size() > 1)
                {
                    for (const LLPointer<LLViewerTexture>& t : info->mTextureList)
                    {
                        if (t.isNull())
                        {
                            why = BATCH_NULL_TEXTURE;
                            break;
                        }
                    }
                }
                if (why >= 0)
                {
                    ++n.batch_left[why];
                    --block_ok;
                }
            }

            if (group_left < 0 && block_ok < batches)
            {
                // One batch it cannot take and the probe list makes no block of the group's type.
                n.batch_left[BATCH_BLOCK_MATE] += block_ok;
                continue;
            }
            if (group_left >= 0)
            {
                continue;
            }

            // A block stage 1 could make now.
            (kind == KIND_GBUFFER ? n.el_batches_gbuffer : n.el_batches_post) += batches;
            ++n.el_blocks;
            ++n.el_block_hist[histogram_bin(batches)];
            n.el_block_textures += block_textures;
            n.type_el_batches[type] += batches;
            ++n.type_el_blocks[type];
            type_used[type] = true;
            n.max_block = llmax(n.max_block, batches);
            if (std::find(c.el_models.begin(), c.el_models.end(), region_matrix) == c.el_models.end())
            {
                c.el_models.push_back(region_matrix);
            }
            for (const LLDrawInfo* info : entry.second)
            {
                if (!info->mCount)
                {
                    continue;
                }
                const LLVertexBuffer* vb = info->mVertexBuffer.get();
                c.el_buckets.push_back(mix(mix(type, info->mShaderMask), vb->getTypeMask()));
                if (info->mTextureList.size() > 1)
                {
                    for (const LLPointer<LLViewerTexture>& t : info->mTextureList)
                    {
                        c.el_textures.push_back(t.get());
                    }
                }
                else if (info->mTexture.notNull())
                {
                    c.el_textures.push_back(info->mTexture.get());
                }
            }
        }
    }

    // The stage 1 estimate, ms of main thread a frame (per-frame averages).
    F64 s1_estimate_ms(F64 el_batches, F64 el_blocks, F64 el_block_textures, F64 el_types)
    {
        return (el_batches * US_PER_BATCH - el_blocks * US_PER_BLOCK - el_block_textures * US_PER_BLOCK_TEXTURE -
                el_types * US_PER_TYPE) / 1000.0;
    }

    void append_histogram(std::string& line, const U64 (&hist)[HISTOGRAM_BINS], F64 per)
    {
        for (U32 b = 0; b < HISTOGRAM_BINS; ++b)
        {
            line += llformat("%s%s:%.0f", b ? " " : "", HISTOGRAM_NAMES[b], hist[b] * per);
        }
    }

    void log_counts(Census& c)
    {
        if (gFrameTimeSeconds - c.logged_at < LOG_PERIOD)
        {
            return;
        }
        c.logged_at = gFrameTimeSeconds;
        const Counts n = c.counts;
        c.counts = Counts();
        if (!n.frames)
        {
            return;
        }
        const F64 per = 1.0 / (F64)n.frames;
        const U64 opaque = n.batches_gbuffer + n.batches_post;
        const U64 el = n.el_batches_gbuffer + n.el_batches_post;
        const F64 s1 = s1_estimate_ms(el * per, n.el_blocks * per, n.el_block_textures * per, n.el_types * per);
        auto pct = [](U64 part, U64 whole) { return whole ? 100.0 * (F64)part / (F64)whole : 0.0; };

        // 1. The main line: the numbers stage 1's gain depends on, and the judgment.
        std::string line = llformat(
            "retained census (research/195 S0): frames %llu, census %.3f ms/frame; per frame: groups %.0f (static %.0f, bridge %.0f, other %.0f; "
            "same as last seen %.1f%%, settled %.1f%%, new %.1f, changed %.1f, renamed %.2f); state sort lists: drawable groups %.0f, "
            "drawables %.0f, bridges %.0f; render map opaque %.0f; opaque batches %.0f (gbuffer %.0f, post %.0f; "
            "in same groups %.1f%%) in blocks %.0f (%.2f batches/block; ",
            (unsigned long long)n.frames, n.census_ms * per, n.groups * per, n.groups_static * per, n.groups_bridge * per,
            n.groups_other * per, pct(n.groups_same, n.groups), pct(n.groups_settled, n.groups), n.groups_new * per,
            n.groups_changed * per, n.groups_renamed * per, n.cull_drawable_groups * per, n.cull_drawables * per,
            n.cull_bridges * per, n.render_map_opaque * per, opaque * per, n.batches_gbuffer * per, n.batches_post * per,
            pct(n.batches_same, opaque), n.blocks * per, n.blocks ? (F64)opaque / (F64)n.blocks : 0.0);
        append_histogram(line, n.block_hist, per);
        line += llformat("), alpha %.0f, rigged %.0f, other %.0f; textures/batch 0:%.0f 1:%.0f 2+:%.0f, lists %.0f; buckets %.1f; "
                         "S1 blocks: batches %.0f (gbuffer %.0f, post %.0f; %.1f%% of opaque) in blocks %.0f (%.2f batches/block; ",
                         n.batches_alpha * per, n.batches_rigged * per, n.batches_other * per, n.tex0 * per, n.tex1 * per,
                         n.tex2 * per, n.lists * per, n.buckets * per, el * per, n.el_batches_gbuffer * per,
                         n.el_batches_post * per, pct(el, opaque), n.el_blocks * per,
                         n.el_blocks ? (F64)el / (F64)n.el_blocks : 0.0);
        append_histogram(line, n.el_block_hist, per);
        line += llformat("), block textures %.0f, types %.1f, buckets %.1f; changes/frame: draw infos made %.1f freed %.1f, "
                         "geom builds %.1f; G5 fit (max a frame): rows %u/%u, block %u/%u, textures %u/%u, models %u/%u; "
                         "S1 est %.3f ms (%.2f us/batch x %.0f - %.2f us/block x %.0f - %.2f us/texture x %.0f - %.0f us/type x %.1f); "
                         "S1 est >= %.1fms: %s",
                         n.el_block_textures * per, n.el_types * per, n.el_buckets * per, n.drawinfo_made * per,
                         n.drawinfo_freed * per, n.geom_builds * per, n.max_rows, G5_MAX_ROWS, n.max_block, G5_MAX_BLOCK_ROWS,
                         n.max_textures, G5_MAX_HANDLES, n.max_models, G5_MAX_MODELS, s1, US_PER_BATCH, el * per, US_PER_BLOCK,
                         n.el_blocks * per, US_PER_BLOCK_TEXTURE, n.el_block_textures * per, US_PER_TYPE, n.el_types * per,
                         S1_WORTH_MS, s1 >= S1_WORTH_MS ? "yes" : "no");
        LL_INFOS("VKPerf") << line << LL_ENDL;

        // 2. Why batches are not in blocks.
        line = "retained census left (per frame): groups";
        for (U32 r = 0; r < GROUP_LEFT_REASONS; ++r)
        {
            line += llformat("%s %s %.1f", r ? "," : "", GROUP_LEFT_NAMES[r], n.groups_left[r] * per);
        }
        line += "; batches of eligible groups";
        for (U32 r = 0; r < BATCH_LEFT_REASONS; ++r)
        {
            line += llformat("%s %s %.1f", r ? "," : "", BATCH_LEFT_NAMES[r], n.batch_left[r] * per);
        }
        LL_INFOS("VKPerf") << line << LL_ENDL;

        // 3. By render type (each type is drawn with its pool's programs): batches/blocks, in S1 blocks batches/blocks.
        line = "retained census types (per frame, batches/blocks, S1 batches/blocks):";
        for (U32 t = 0; t < LLRenderPass::NUM_RENDER_TYPES; ++t)
        {
            if (!n.type_blocks[t])
            {
                continue;
            }
            const char* name = type_name(t);
            line += llformat(" %s%s %.0f/%.0f %.0f/%.0f;", name ? name : "type", name ? "" : std::to_string(t).c_str(),
                             n.type_batches[t] * per, n.type_blocks[t] * per, n.type_el_batches[t] * per,
                             n.type_el_blocks[t] * per);
        }
        LL_INFOS("VKPerf") << line << LL_ENDL;
    }
}

namespace detail
{
void censusSlow(LLCamera& /*camera*/, LLCullResult& cull)
{
    // The main camera's world view only, once a frame.
    if (LLPipeline::sShadowRender || LLPipeline::sReflectionRender || gCubeSnapshot ||
        LLViewerCamera::sCurCameraID != LLViewerCamera::CAMERA_WORLD)
    {
        return;
    }
    Census& c = sCensus;
    const U32 frame = LLFrameTimer::getFrameCount();
    if (c.counted_frame == frame)
    {
        return;
    }
    c.counted_frame = frame;
    if (!c.announced)
    {
        c.announced = true;
        LL_INFOS("RHIProfile") << "retained census on (LLRHI_RETAINED_CENSUS, research/195 S0): a VKPerf line every "
                               << LOG_PERIOD << " s" << LL_ENDL;
    }

    const auto start = std::chrono::steady_clock::now();
    Counts& n = c.counts;
    c.cur.clear();
    c.buckets.clear();
    c.el_buckets.clear();
    c.el_textures.clear();
    c.el_models.clear();
    bool type_used[LLRenderPass::NUM_RENDER_TYPES] = {};
    const U64 el_before = n.el_batches_gbuffer + n.el_batches_post;

    // The groups the render map loop drew from, with its filters.
    for (LLCullResult::sg_iterator i = cull.beginVisibleGroups(); i != cull.endVisibleGroups(); ++i)
    {
        LLSpatialGroup* group = *i;
        if (group->isDead())
        {
            continue;
        }
        if ((LLPipeline::sUseOcclusion && group->isOcclusionState(LLSpatialGroup::OCCLUDED)) ||
            (LLPipeline::RenderAutoHideSurfaceAreaLimit > 0.f &&
             group->mSurfaceArea > LLPipeline::RenderAutoHideSurfaceAreaLimit * llmax(group->mObjectBoxSize, 10.f)))
        {
            continue;
        }
        ++n.groups;
        count_group(group, c, n, type_used);
    }

    // This frame's groups (a group listed twice keeps its first), and those
    // seen in the last KEEP_SECONDS that are out of view now: a block stays
    // registered while its group is out of view (only compared by address,
    // never read: a freed group's address taken again by a new group shows
    // as changed).
    std::sort(c.cur.begin(), c.cur.end());
    c.cur.erase(std::unique(c.cur.begin(), c.cur.end(), [](const Record& a, const Record& b) { return a.group == b.group; }),
                c.cur.end());
    c.merged.clear();
    {
        const F32 now = gFrameTimeSeconds;
        auto p = c.prev.begin();
        for (const Record& r : c.cur)
        {
            for (; p != c.prev.end() && p->group < r.group; ++p)
            {
                if (now - p->seen < KEEP_SECONDS)
                {
                    c.merged.push_back(*p);
                }
            }
            if (p != c.prev.end() && p->group == r.group)
            {
                ++p;
            }
            c.merged.push_back(r);
        }
        for (; p != c.prev.end(); ++p)
        {
            if (now - p->seen < KEEP_SECONDS)
            {
                c.merged.push_back(*p);
            }
        }
    }
    c.prev.swap(c.merged);

    count_unique(c.buckets, n.buckets);
    count_unique(c.el_buckets, n.el_buckets);
    std::sort(c.el_textures.begin(), c.el_textures.end());
    const U32 textures = (U32)(std::unique(c.el_textures.begin(), c.el_textures.end()) - c.el_textures.begin());
    n.max_textures = llmax(n.max_textures, textures);
    n.max_models = llmax(n.max_models, (U32)c.el_models.size());
    n.max_rows = llmax(n.max_rows, (U32)(n.el_batches_gbuffer + n.el_batches_post - el_before));
    for (bool used : type_used)
    {
        n.el_types += used ? 1 : 0;
    }
    n.cull_drawable_groups += cull.getDrawableGroupsSize();
    n.cull_drawables += cull.getVisibleListSize();
    n.cull_bridges += cull.getVisibleBridgeSize();
    for (U32 t = 0; t < LLRenderPass::NUM_RENDER_TYPES; ++t)
    {
        const EKind kind = kind_of(t);
        n.render_map_opaque += kind == KIND_GBUFFER || kind == KIND_POST ? cull.getRenderMapSize(t) : 0;
    }
    n.drawinfo_made += sDrawInfoMade.exchange(0, std::memory_order_relaxed);
    n.drawinfo_freed += sDrawInfoFreed.exchange(0, std::memory_order_relaxed);
    n.geom_builds += sGeomBuilds.exchange(0, std::memory_order_relaxed);
    ++n.frames;
    n.census_ms += std::chrono::duration<F64, std::milli>(std::chrono::steady_clock::now() - start).count();

    log_counts(c);
}
}
}

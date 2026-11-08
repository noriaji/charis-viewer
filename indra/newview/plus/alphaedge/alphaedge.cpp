/**
 * @file alphaedge.cpp
 * @brief The colour of a texture's transparent texels pushed out from the opaque ones when it
 *        is decoded (PlusAlphaEdgeFix, research/alpha-mask-edge-2026-10-26.md (d)).
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

#include "alphaedge.h"

#include "llface.h"
#include "llfetchedgltfmaterial.h"
#include "llgltfmaterial.h"
#include "llimage.h"
#include "llmaterial.h"
#include "llrender.h"
#include "lltextureentry.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewertexture.h"

#include <atomic>
#include <mutex>
#include <unordered_map>

namespace plus::alphaedge
{
    static_assert(rule::DIFFUSE_NONE == LLMaterial::DIFFUSE_ALPHA_MODE_NONE &&
                  rule::DIFFUSE_BLEND == LLMaterial::DIFFUSE_ALPHA_MODE_BLEND &&
                  rule::DIFFUSE_MASK == LLMaterial::DIFFUSE_ALPHA_MODE_MASK &&
                  rule::DIFFUSE_EMISSIVE == LLMaterial::DIFFUSE_ALPHA_MODE_EMISSIVE,
                  "rule::DIFFUSE_* must be LLMaterial::eDiffuseAlphaMode");
    static_assert(rule::GLTF_OPAQUE == LLGLTFMaterial::ALPHA_MODE_OPAQUE &&
                  rule::GLTF_BLEND == LLGLTFMaterial::ALPHA_MODE_BLEND &&
                  rule::GLTF_MASK == LLGLTFMaterial::ALPHA_MODE_MASK,
                  "rule::GLTF_* must be LLGLTFMaterial::AlphaMode");

    namespace
    {
        constexpr char SETTING[] = "PlusAlphaEdgeFix";

        // A decode thread keeps its work memory up to this many cells (a 1024 x 1024 image's
        // pyramid is about 350 000, 2.8 MB); a larger image's is freed after it.
        constexpr size_t KEEP_CELLS = 1u << 19;

        std::atomic<bool> sEnabled{ false };

        // The uses by texture id. Written on the main thread (noteUse, noteMaterial), read by
        // the decoders.
        std::mutex& useMutex()
        {
            static std::mutex m;
            return m;
        }

        std::unordered_map<LLUUID, U8>& uses()
        {
            static std::unordered_map<LLUUID, U8> map;
            return map;
        }

        bool colourOnly(const LLUUID& id)
        {
            std::lock_guard<std::mutex> lock(useMutex());
            const auto it = uses().find(id);
            return it != uses().end() && rule::fills(it->second);
        }

        void record(const LLUUID& id, U8 use)
        {
            if (id.isNull() || use == 0)
            {
                return;
            }
            std::lock_guard<std::mutex> lock(useMutex());
            U8& recorded = uses()[id];
            recorded = rule::merge(recorded, use);
        }

        rule::Face describe(const LLTextureEntry* te)
        {
            rule::Face face;
            if (!te)
            {
                return face;
            }
            face.te = true;
            if (const LLGLTFMaterial* gltf = te->getGLTFRenderMaterial())
            {
                face.pbr = true;
                face.gltf_alpha_mode = (U8)gltf->mAlphaMode;
            }
            face.material_id = !te->getMaterialID().isNull();
            const LLMaterialPtr material = te->getMaterialParams();
            if (material.notNull())
            {
                face.material_params = true;
                face.diffuse_alpha_mode = material->getDiffuseAlphaMode();
            }
            return face;
        }

        void readSetting()
        {
            static LLCachedControl<bool> setting(gSavedSettings, SETTING, true);
            sEnabled.store((bool)setting, std::memory_order_relaxed);
        }
    }

    void noteUse(const LLUUID& id, U32 channel, const LLFace* face)
    {
        readSetting();
        if (id.isNull())
        {
            return;
        }

        // The sun and the moon: their veil of low alpha is part of the picture (a square of the sky's own light
        // around the disc), and the sky blends it as it is. Filled with the colour of the disc it showed as a
        // saturated square, so never filled.
        if (face)
        {
            const LLViewerObject* sky = face->getViewerObject();
            if (sky && sky->getPCode() == LLViewerObject::LL_VO_SKY)
            {
                record(id, rule::USE_OTHER);
                return;
            }
        }

        // Channel 1 is the normal map of an object (the sky's next sun or moon is not filled either, above)
        rule::Slot slot = rule::SLOT_OTHER_MAP;
        if (channel == LLRender::DIFFUSE_MAP)
        {
            slot = rule::SLOT_DIFFUSE;
        }
        else if (channel == LLRender::BASECOLOR_MAP)
        {
            slot = rule::SLOT_BASECOLOR;
        }
        const rule::Face kind = (slot == rule::SLOT_DIFFUSE || slot == rule::SLOT_BASECOLOR) && face
                                    ? describe(face->getTextureEntry())
                                    : rule::Face();
        record(id, rule::useOf(slot, kind));
    }

    void noteMaterial(const LLViewerObject* object, U8 te)
    {
        const LLTextureEntry* entry = object ? object->getTE(te) : nullptr;
        if (!entry)
        {
            return;
        }
        const rule::Face kind = describe(entry);
        if (const LLViewerTexture* diffuse = object->getTEImage(te))
        {
            record(diffuse->getID(), rule::useOf(rule::SLOT_DIFFUSE, kind));
        }
        // The base colour texture the face is bound to (as LLVolumeGeometryManager::rebuildGeom
        // does, the material is an LLFetchedGLTFMaterial); before it is fetched, its id
        if (const LLGLTFMaterial* gltf = entry->getGLTFRenderMaterial())
        {
            const LLViewerFetchedTexture* bound = static_cast<const LLFetchedGLTFMaterial*>(gltf)->mBaseColorTexture;
            record(bound ? bound->getID() : gltf->mTextureId[LLGLTFMaterial::GLTF_TEXTURE_INFO_BASE_COLOR],
                   rule::useOf(rule::SLOT_BASECOLOR, kind));
        }
    }

    void onDecoded(const LLUUID& id, LLImageRaw* raw)
    {
        if (!raw || !sEnabled.load(std::memory_order_relaxed))
        {
            return;
        }
        const S32 components = raw->getComponents();
        if ((components != 2 && components != 4) || !colourOnly(id))
        {
            return;
        }

        thread_local Scratch scratch;
        {
            LLImageDataLock lock(raw);
            U8* data = raw->getData();
            if (data && !raw->isBufferInvalid())
            {
                fillTransparent(data, raw->getWidth(), raw->getHeight(), components, scratch);
            }
        }
        if (scratch.cells.capacity() > KEEP_CELLS)
        {
            std::vector<Scratch::Cell>().swap(scratch.cells);
        }
    }

    // ---- alphaedge fill (fix-r5) ----
    // Plain values only (tools/selftest sim-alphaedge compiles this block with the rule block).
    namespace
    {
        U8 average(const F32* acc, S32 i, F32 w)
        {
            return (U8)llclamp((S32)(acc[i] / w + 0.5f), 0, 255);
        }
    }

    bool fillTransparent(U8* data, S32 width, S32 height, S32 components, Scratch& scratch)
    {
        if (!data || width <= 0 || height <= 0 || (components != 2 && components != 4))
        {
            return false;
        }
        const S32 colours = components - 1;   // the colour channels come first, the alpha last
        const S32 alpha = components - 1;
        const size_t texels = (size_t)width * (size_t)height;

        // Anything to do: some texels of low alpha and some opaque
        size_t low = 0;
        for (size_t i = 0; i < texels; ++i)
        {
            low += data[i * components + alpha] < SOLID_ALPHA ? 1 : 0;
        }
        if (low == 0 || low == texels)
        {
            return false;
        }

        // The pyramid's sizes, from the half size (rounded up) to 1 x 1
        scratch.offset.clear();
        scratch.width.clear();
        scratch.height.clear();
        size_t cells = 0;
        for (S32 w = width, h = height; w > 1 || h > 1;)
        {
            w = (w + 1) / 2;
            h = (h + 1) / 2;
            scratch.offset.push_back(cells);
            scratch.width.push_back(w);
            scratch.height.push_back(h);
            cells += (size_t)w * (size_t)h;
        }
        if (cells == 0)
        {
            return false;   // 1 x 1: no neighbours
        }
        scratch.cells.resize(cells);
        const size_t levels = scratch.offset.size();

        // Push: each cell has the alpha-weighted colour of the opaque texels under it
        for (size_t level = 0; level < levels; ++level)
        {
            const S32 w = scratch.width[level];
            const S32 h = scratch.height[level];
            const S32 child_w = level == 0 ? width : scratch.width[level - 1];
            const S32 child_h = level == 0 ? height : scratch.height[level - 1];
            Scratch::Cell* out = &scratch.cells[scratch.offset[level]];
            const Scratch::Cell* child = level == 0 ? nullptr : &scratch.cells[scratch.offset[level - 1]];
            for (S32 y = 0; y < h; ++y)
            {
                for (S32 x = 0; x < w; ++x)
                {
                    F32 acc[3] = { 0.f, 0.f, 0.f };
                    F32 sum = 0.f;
                    for (S32 cy = 2 * y; cy < llmin(2 * y + 2, child_h); ++cy)
                    {
                        for (S32 cx = 2 * x; cx < llmin(2 * x + 2, child_w); ++cx)
                        {
                            const size_t ci = (size_t)cy * child_w + cx;
                            if (child)
                            {
                                const Scratch::Cell& c = child[ci];
                                for (S32 k = 0; k < colours; ++k)
                                {
                                    acc[k] += c.c[k] * c.w;
                                }
                                sum += c.w;
                            }
                            else
                            {
                                const U8* t = data + ci * components;
                                if (t[alpha] >= SOLID_ALPHA)
                                {
                                    const F32 a = t[alpha];
                                    for (S32 k = 0; k < colours; ++k)
                                    {
                                        acc[k] += t[k] * a;
                                    }
                                    sum += a;
                                }
                            }
                        }
                    }
                    Scratch::Cell& cell = out[(size_t)y * w + x];
                    cell.w = sum;
                    cell.pad = 0;
                    for (S32 k = 0; k < 3; ++k)
                    {
                        cell.c[k] = (k < colours && sum > 0.f) ? average(acc, k, sum) : 0;
                    }
                }
            }
        }

        // Pull: a cell with no opaque texel under it takes its parent's colour (the top has
        // some: not every texel is of low alpha)
        for (size_t level = levels - 1; level-- > 0;)
        {
            const S32 w = scratch.width[level];
            const S32 h = scratch.height[level];
            const S32 parent_w = scratch.width[level + 1];
            Scratch::Cell* cells_here = &scratch.cells[scratch.offset[level]];
            const Scratch::Cell* parent = &scratch.cells[scratch.offset[level + 1]];
            for (S32 y = 0; y < h; ++y)
            {
                for (S32 x = 0; x < w; ++x)
                {
                    Scratch::Cell& cell = cells_here[(size_t)y * w + x];
                    if (cell.w <= 0.f)
                    {
                        const Scratch::Cell& p = parent[(size_t)(y / 2) * parent_w + x / 2];
                        cell.c[0] = p.c[0];
                        cell.c[1] = p.c[1];
                        cell.c[2] = p.c[2];
                    }
                }
            }
        }

        // The texels of low alpha: the opaque neighbours' colour (what bilinear filtering mixes
        // with), else the pyramid's; their own colour kept by (a / SOLID_ALPHA)^2
        const Scratch::Cell* half = &scratch.cells[scratch.offset[0]];
        const S32 half_w = scratch.width[0];
        for (S32 y = 0; y < height; ++y)
        {
            for (S32 x = 0; x < width; ++x)
            {
                U8* t = data + ((size_t)y * width + x) * components;
                const U8 a = t[alpha];
                if (a >= SOLID_ALPHA)
                {
                    continue;
                }
                F32 acc[3] = { 0.f, 0.f, 0.f };
                F32 sum = 0.f;
                for (S32 ny = llmax(y - 1, 0); ny <= llmin(y + 1, height - 1); ++ny)
                {
                    for (S32 nx = llmax(x - 1, 0); nx <= llmin(x + 1, width - 1); ++nx)
                    {
                        const U8* n = data + ((size_t)ny * width + nx) * components;
                        if (n[alpha] >= SOLID_ALPHA)
                        {
                            const F32 na = n[alpha];
                            for (S32 k = 0; k < colours; ++k)
                            {
                                acc[k] += n[k] * na;
                            }
                            sum += na;
                        }
                    }
                }
                const Scratch::Cell& cell = half[(size_t)(y / 2) * half_w + x / 2];
                const F32 keep = ((F32)a / SOLID_ALPHA) * ((F32)a / SOLID_ALPHA);
                for (S32 k = 0; k < colours; ++k)
                {
                    const F32 fill = sum > 0.f ? acc[k] / sum : (F32)cell.c[k];
                    t[k] = (U8)llclamp((S32)(fill + (t[k] - fill) * keep + 0.5f), 0, 255);
                }
            }
        }
        return true;
    }
    // ---- end of the alphaedge fill ----
}

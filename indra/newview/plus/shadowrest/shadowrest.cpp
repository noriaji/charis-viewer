/**
 * @file shadowrest.cpp
 * @brief The shadow casters still drawn through the GL translation, handed to
 *        the native shadow packets instead (research/137 N10).
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

#include "shadowrest.h"

#include "lldrawable.h"
#include "lldrawpooltree.h"
#include "llface.h"
#include "llframetimer.h"
#include "llrender.h"
#include "llvertexbuffer.h"
#include "llviewerregion.h"
#include "llviewertexture.h"
#include "pipeline.h"

#include <algorithm>
#include <string>

namespace
{
    // treeShadowF's cut, as LLDrawPoolTree::beginShadowPass sets it (setMinimumAlpha).
    constexpr F32 TREE_ALPHA_CUTOFF = 0.5f;
    // LLDrawPoolTree::renderDeferred: Linden tree textures kept at full resolution.
    constexpr F32 TREE_TEXTURE_PIXELS = 1024.f * 1024.f;
    constexpr F64 REPORT_SECONDS = 10.0;

    bool sTreesOn = LLRHI::envSet("LLRHI_NATIVE_TREE_SHADOWS");

    // The shadow pass in progress (renderShadowCasters, main thread).
    struct Pass
    {
        bool open = false;
        U32 nested = 0;                         // TreePasses inside the open one (take nothing)
        bool trees = false;                     // this pass may take the tree pools
        std::vector<LLDrawPoolTree*> taken;     // pools whose faces are in the packets
        U64 packets = 0;                        // for the counters, once drawn
    };
    Pass sPass;
    std::vector<LLRHI::ShadowPacket> sPackets;  // treePackets' result, reused

    // For the report, since the last one.
    struct Counters
    {
        U64 passes = 0;         // passes that took trees
        U64 packets = 0;
        U64 pools = 0;          // pools taken
        U64 not_ready = 0;      // pools left to GL: texture not ready
        U64 no_buffer = 0;      // pools left to GL: a face the packets cannot draw
        U64 not_drawn = 0;      // passes whose packets were not drawn (pools drew as before)
    };
    Counters sCounters;
    F64 sReportAt = -1.0;

    // At most once a second (frame time): the cue file, and the report while on.
    void poll()
    {
        static F64 next_poll = 0.0;
        const F64 now = LLFrameTimer::getElapsedSeconds();
        if (now < next_poll)
        {
            return;
        }
        next_poll = now + 1.0;

        if (LLRHI::takeCue("nativetree.toggle"))
        {
            // Turning on takes effect only on the native shadows' packet
            // path without GL draws (LLPipeline::renderShadowCasters and
            // TreePass): otherwise stay off and say why (as N8 and N9 do).
            // Turning off always goes.
            const char* missing = !LLRHI::sNativeShadows       ? "native shadows are off (nativeshadows.toggle)"
                                  : !LLRHI::sDrawShadowPackets ? "the backend draws no shadow packets"
                                  : !LLRHI::sGLDrawsDisabled   ? "GL draws too (mirror mode)"
                                                               : nullptr;
            if (!sTreesOn && missing)
            {
                LL_INFOS("RHIProfile") << "native tree shadows (research/137 N10): " << missing << ", unchanged" << LL_ENDL;
            }
            else
            {
                sTreesOn = !sTreesOn;
                sCounters = Counters();
                sReportAt = now;
                LL_INFOS("RHIProfile") << "native tree shadows " << (sTreesOn ? "on" : "off") << LL_ENDL;
            }
        }

        if (!sTreesOn)
        {
            return;
        }
        if (sReportAt < 0.0)
        {
            sReportAt = now;
            return;
        }
        if (now - sReportAt >= REPORT_SECONDS)
        {
            LL_INFOS("RHIShadow") << "native tree shadows: " << sCounters.packets << " packets from " << sCounters.pools
                                  << " pools in " << sCounters.passes << " passes; pools left to the translation: texture not ready "
                                  << sCounters.not_ready << ", a face without GL buffers " << sCounters.no_buffer
                                  << "; passes not drawn natively " << sCounters.not_drawn << LL_ENDL;
            sCounters = Counters();
            sReportAt = now;
        }
    }

    // As LLDrawPoolTree::renderDeferred draws a face: the whole buffer
    // (drawRange from index 0, getNumIndices), the region's render matrix
    // (applyModelMatrix). False: the packets cannot draw it.
    bool tree_packet(LLFace* face, U32 texture, std::vector<LLRHI::ShadowPacket>& out)
    {
        LLVertexBuffer* vb = face->getVertexBuffer();
        if (!vb)
        {
            return true;    // GL skips it too
        }
        if (!vb->getNumIndices())
        {
            return true;    // nothing to draw
        }
        LLDrawable* drawable = face->getDrawable();
        LLViewerRegion* region = drawable ? drawable->getRegion() : nullptr;
        // Still mapped: the GL path's setBuffer would send the writes first.
        if (!region || !vb->getGLBuffer() || !vb->getGLIndices() || vb->isMapped())
        {
            return false;
        }
        LLRHI::ShadowPacket p;
        p.vertex_buffer = vb->getGLBuffer();
        p.index_buffer = vb->getGLIndices();
        p.index_type = vb->getIndicesType();
        p.first_index = 0;
        p.index_count = vb->getNumIndices();
        p.position_offset = vb->getOffset(LLVertexBuffer::TYPE_VERTEX);
        if (vb->hasDataType(LLVertexBuffer::TYPE_TEXCOORD0))
        {
            p.texcoord_offset = vb->getOffset(LLVertexBuffer::TYPE_TEXCOORD0);
        }
        if (vb->hasDataType(LLVertexBuffer::TYPE_COLOR))
        {
            p.color_offset = vb->getOffset(LLVertexBuffer::TYPE_COLOR);
        }
        // One texture: the vertex texture index (the positions' w, not written
        // for trees) picks among equal slots (the backend fills the empty ones
        // with the first). treeShadowV: the texture matrix is gGL's, identity.
        p.textures[0] = texture;
        p.mode = LLRHI::ShadowPacket::MODE_ALPHA_TEST;
        p.alpha_cutoff = TREE_ALPHA_CUTOFF;
        p.model = (const F32*)region->mRenderMatrix.mMatrix;
        p.vertex_count = vb->getNumVerts();
        p.index_total = vb->getNumIndices();
        out.push_back(p);
        return true;
    }
}

namespace plus
{
namespace shadowrest
{
    TreePass::TreePass(bool pools)
    {
        poll();
        if (sPass.open)
        {
            ++sPass.nested;     // not expected (renderShadowCasters does not nest)
            return;
        }
        mOwner = true;
        sPass.open = true;
        // Only where nothing but the packets draws the casters: with GL drawing
        // too (mirror mode) the translated tree draws stay, as before.
        sPass.trees = pools && sTreesOn && LLRHI::sGLDrawsDisabled;
        sPass.taken.clear();
    }

    TreePass::~TreePass()
    {
        if (mOwner)
        {
            sPass = Pass();
        }
        else if (sPass.nested)
        {
            --sPass.nested;
        }
    }

    void TreePass::drawn(bool native)
    {
        if (!mOwner || sPass.taken.empty())
        {
            return;
        }
        if (!native)
        {
            ++sCounters.not_drawn;
            sPass.taken.clear();
            return;
        }
        ++sCounters.passes;
        sCounters.pools += sPass.taken.size();
        sCounters.packets += sPass.packets;
    }

    const std::vector<LLRHI::ShadowPacket>& treePackets(const std::map<uintptr_t, LLDrawPool*>& tree_pools)
    {
        sPackets.clear();
        if (!sPass.open || sPass.nested || !sPass.trees)
        {
            return sPackets;
        }
        sPass.taken.clear();
        sPass.packets = 0;
        for (const auto& entry : tree_pools)
        {
            LLDrawPoolTree* pool = static_cast<LLDrawPoolTree*>(entry.second);
            // As renderGeomShadow: a pool of a type not rendered draws no shadow.
            if (!pool || !gPipeline.hasRenderType(pool->getType()) || pool->getNumShadowPasses() <= 0 || pool->mDrawFace.empty())
            {
                continue;
            }
            // The GL name as the draw would bind it; not ready: bindFast in the
            // GL path makes it so (the white stand-in, the texture's options).
            const U32 texture = LLTexUnit::directName(pool->getTexture());
            if (texture == LLRHI::DirectDraw::KEEP_UNIT)
            {
                ++sCounters.not_ready;
                continue;
            }
            const size_t first = sPackets.size();
            bool whole = true;
            for (LLFace* face : pool->mDrawFace)
            {
                if (face && !tree_packet(face, texture, sPackets))
                {
                    whole = false;
                    break;
                }
            }
            if (!whole)
            {
                sPackets.resize(first);     // the pool draws through GL, all of it
                ++sCounters.no_buffer;
                continue;
            }
            sPass.taken.push_back(pool);
        }
        sPass.packets = sPackets.size();
        return sPackets;
    }

    bool treeDrawn(LLDrawPoolTree* pool)
    {
        if (!sPass.open || sPass.nested || sPass.taken.empty() || std::find(sPass.taken.begin(), sPass.taken.end(), pool) == sPass.taken.end())
        {
            return false;
        }
        // LLDrawPoolTree::renderDeferred before its loop.
        if (!pool->mDrawFace.empty())
        {
            LLViewerTexture* texture = pool->getTexture();
            gGL.getTexUnit(LLDrawPoolTree::sDiffTex)->bindFast(texture);
            texture->addTextureStats(TREE_TEXTURE_PIXELS);
        }
        return true;
    }
}
}

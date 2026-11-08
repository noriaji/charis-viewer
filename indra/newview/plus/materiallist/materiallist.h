/**
 * @file materiallist.h
 * @brief Material lists (research/137 N4): the legacy material pool's and the
 *        bump pool's batch loops as one draw list each (LLRHI::DrawList).
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

#ifndef PLUS_MATERIALLIST_MATERIALLIST_H
#define PLUS_MATERIALLIST_MATERIALLIST_H

#include "stdtypes.h"
// research/180 stage 4: lldrawpoolmaterials.cpp's packet line (plus::packet::drawMaterials) comes in with this header.
#include "plus/packet/packet.h"

class LLDrawInfo;
class LLGLSLShader;
class LLRenderPass;
class LLTexture;

// The "LLRHI native (research/137 N4)" lines in lldrawpoolmaterials.cpp and
// lldrawpoolbump.cpp.
//
// Each loop below is a copy of the official loop that writes its batches into
// a draw list (llrhidrawlist.h) instead of setting the uniforms, binding the
// textures and drawing each batch: the list carries per draw only what the
// loop would change (the uniforms as patches, the textures as names per unit,
// the texture animation, the model matrix). The backend resolves the program,
// draw target and state once for the whole list.
//
// A function returns false, having changed nothing, when the list is off
// (LLRHI_DRAW_LISTS, LLRHI_DRAW_LISTS_MATERIAL), the backend does not take a
// list now (a ProbeList capture, a depth-only pass, ...), or the program has a
// uniform the list cannot set (no native slot): the official loop runs then.
// True: every batch of the range was drawn (in its place; a batch the list
// cannot take, such as one whose texture has no GL name yet, is drawn by the
// official body for one batch, after the list so far is submitted).
//
// After the list the uniforms, the texture units and the texture matrix are
// as after the official loop (research/137 section 2.3, promise 4).
//
// Static (not rigged) batches only for now: a rigged batch sets its skinning
// palette and marks the skin stencil (plus/skinsss) per batch, which waits for
// research/137 N2's shared palette handling.
namespace plus
{
namespace materiallist
{
    // LLDrawPoolMaterials::renderDeferred's loop over [begin, end) (a static
    // pass), called right before it with the pass's program bound, its
    // uniforms and diffuse unit reset as the official code does, and the
    // channels enableTexture gave it (-1: the program has no such map).
    bool drawMaterials(LLGLSLShader* shader, LLDrawInfo** begin, LLDrawInfo** end, S32 diffuse_channel, S32 spec_channel,
                       S32 norm_channel);

    // LLDrawPoolBump::renderDeferred's loop over [begin, end) for the static
    // batches (gDeferredBumpProgram bound, its two units reset), with the
    // channels it found. pool: the bump pool (pushBumpBatch, its shader level).
    bool drawBumpDeferred(LLRenderPass& pool, LLDrawInfo** begin, LLDrawInfo** end, S32 diffuse_channel, S32 bump_channel);

    // LLDrawPoolBump::pushBumpBatches for a static pass (the emboss bump pass
    // after the deferred passes, gObjectBumpProgram bound).
    bool drawBumpPost(LLRenderPass& pool, U32 type);

    // research/137 N11 (LLRHI_DRAW_LISTS_THREADS with THREADS_BUILD): a long
    // drawMaterials loop (256 batches or more) is made into draws on a cull
    // thread, chunk after chunk, while the main thread submits the chunks
    // made before. The counts since the last call (main thread; the VKPerf
    // line, plus/drawlistreport).
    struct BuildStats
    {
        U32 loops = 0;          // loops built on a cull thread
        U32 no_thread = 0;      // long loops built on the main thread: no cull thread to take it
        U32 chunks = 0;         // chunks submitted
        U32 draws = 0;          // their draws
        U32 old_path = 0;       // batches drawn the old way in such loops
        U32 refused = 0;        // loops whose rest went the old way (the backend took no list any more)
        U64 wait_ns = 0;        // the main thread waiting for a chunk
        U64 build_ns = 0;       // the cull threads building
    };
    void takeBuildStats(BuildStats& out);

    // research/180 stage 2 (9 2): the official loop of
    // LLDrawPoolMaterials::renderDeferred, when the list above does not take
    // it, hands each batch's three textures (normal, specular, diffuse) to
    // its direct draw by GL name (its texture number while it has one,
    // research/197 1a), as pushGLTFBatch does, instead of binding
    // them to the units first (bindFast per change: the unit hook, the
    // backend's lock and its unit record, then the backend reads the units
    // back). The backend's slot publication (stage 1) then answers each name
    // with one read.
    //
    // Same draws: a name is what bindFast would have bound (the unit's
    // texture name, sWhiteTexture for a batch without a diffuse map, as
    // unbindFast binds), worked out when the batch's texture changes, as the
    // loop binds; a texture without a name yet or with its sampler options
    // still to set is bound to its unit as before (directTexture) and the
    // draw reads that unit. A batch the backend refuses gets its units bound
    // as the official loop would have them, then the GL way (drawGeometry).
    // The units are not bound otherwise: after the loop they are as before it
    // (the texture units' own bookkeeping, mCurrTexture, agrees with them).
    //
    // Whenever the direct draws are there (research/218 vk-s7g, research/207
    // 7.1 T8: the switch LLRHI_MATERIAL_TEXTURES_BY_NAME and its verify are
    // retired). Main thread, one per loop.
    class DirectUnits
    {
    public:
        DirectUnits(S32 diffuse_channel, S32 spec_channel, S32 norm_channel);
        ~DirectUnits();
        DirectUnits(const DirectUnits&) = delete;
        DirectUnits& operator=(const DirectUnits&) = delete;

        // Whether the loop leaves its binds to draw(): skip them when true.
        bool byName() const { return mByName; }
        // In place of drawGeometry(params), after the palette and the
        // texture matrix: true when the batch was drawn (directly, with its
        // textures by name). False: drawGeometry draws it (its units are
        // bound as the official loop would have them by then).
        bool draw(LLDrawInfo& params);

    private:
        struct Channel
        {
            S32 unit = -1;              // the program's channel (-1: no such map)
            LLTexture* named = nullptr; // the texture `name` is for
            U32 name = 0;               // its GL name, or DirectDraw::KEEP_UNIT (bound to the unit)
            bool have = false;          // named/name hold
            LLTexture* bound = nullptr; // what the official loop's last bind put on the unit
        };
        U32 nameOf(Channel& c, LLTexture* texture, bool diffuse);
        void bindAsOfficial(Channel& c, LLTexture* texture, bool diffuse);

        Channel mNormal;
        Channel mSpecular;
        Channel mDiffuse;
        U32 mCount = 0;                 // units the draw's table covers (the highest channel + 1)
        bool mByName = false;
    };
}
}

#endif // PLUS_MATERIALLIST_MATERIALLIST_H

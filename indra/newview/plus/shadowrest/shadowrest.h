/**
 * @file shadowrest.h
 * @brief The shadow casters still drawn through the GL translation, handed to
 *        the native shadow packets instead (research/137 N10 in the
 *        sl-viewer repository). First: the Linden trees (LLDrawPoolTree).
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

#ifndef PLUS_SHADOWREST_SHADOWREST_H
#define PLUS_SHADOWREST_SHADOWREST_H

#include "stdtypes.h"
#include "llrhi.h"

#include <cstdint>
#include <map>
#include <vector>

class LLDrawPool;
class LLDrawPoolTree;

namespace plus
{
namespace shadowrest
{
    // Tree shadows as packets: LLRHI_NATIVE_TREE_SHADOWS (not set or "0": off),
    // switched while running by the cue file nativetree.toggle in LLRHI_CUE_DIR.
    //
    // A tree pool's shadow (LLDrawPoolTree::renderShadow, treeShadowV/F) is
    // each face's whole buffer with the region's render matrix and the pool's
    // texture, cut at alpha 0.5: a treeShadowF packet (MODE_ALPHA_TEST). The
    // pass's other state is the packets' own:
    //  - depth bias: the pool sets RenderDeferredTreeShadowOffset/Bias, but no
    //    shadow pass enables GL_POLYGON_OFFSET_FILL, so its draws get the same
    //    bias as every caster (none, or plus/shadowdist's slope bias);
    //  - SUN_UP_FACTOR: set by the pool, read by no line of treeShadowF;
    //  - cull, depth test and clamp, colour mask: renderGeomShadow runs inside
    //    renderShadowCasters' scopes, the packets' pass reads the same state.
    // The pool's begin and end (program, polygon offset values) still run, and
    // renderShadow binds the texture as before (treeDrawn), so GL and the
    // viewer's caches are left as the translated draws leave them.
    //
    // Held by LLPipeline::renderShadowCasters around one shadow pass. pools:
    // whether the pass draws the pools (renderGeomShadow).
    class TreePass
    {
    public:
        explicit TreePass(bool pools);
        ~TreePass();
        // After the packets went to LLRHI::sDrawShadowPackets: whether they
        // were drawn. Not drawn: the pools draw their faces as before.
        void drawn(bool native);

        TreePass(const TreePass&) = delete;
        TreePass& operator=(const TreePass&) = delete;

    private:
        bool mOwner = false;    // false: inside another pass (does nothing)
    };

    // LLPipeline::drawNativeShadowCasters: the tree pools' faces as packets,
    // and the pools they came from remembered for treeDrawn. Empty when the
    // pass takes no trees. A pool goes whole or not at all: one whose texture
    // is not ready (no GL name, options to set: the GL path's bindFast sets
    // them) or with a face the packets cannot draw stays with the GL path.
    const std::vector<LLRHI::ShadowPacket>& treePackets(const std::map<uintptr_t, LLDrawPool*>& tree_pools);

    // LLDrawPoolTree::renderShadow: true when the pool's faces went with this
    // pass's packets. It then does what the pool does around its draws (the
    // texture bound on its unit and kept at full resolution).
    bool treeDrawn(LLDrawPoolTree* pool);
}
}

#endif // PLUS_SHADOWREST_SHADOWREST_H

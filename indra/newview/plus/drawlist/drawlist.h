/**
 * @file drawlist.h
 * @brief Draw lists for LLRenderPass's batch loops (research/137 N2,
 *        LLRHI_DRAW_LISTS with LLRHI_DRAW_LISTS_OPAQUE).
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

#ifndef PLUS_DRAWLIST_DRAWLIST_H
#define PLUS_DRAWLIST_DRAWLIST_H

#include "stdtypes.h"

class LLRenderPass;
class LLSpatialGroup;

// The hooks at the head of LLRenderPass's batch loops (lldrawpool.cpp,
// marked "LLRHI native (research/137 N2)"; after the GPU probe list's turn
// where the loop has one). Each is a copy of its official loop that hands the
// loop's batches to the backend as draw lists (drawlistbatch.h), in the same
// order and on the same conditions. True: the loop was drawn here. False (the
// switches off, mirror mode, a shadow pass, the backend not ready ...): the
// official loop runs, unchanged.
namespace plus::drawlist
{
    bool pushBatches(LLRenderPass& pass, U32 type, bool texture, bool batch_textures);
    bool pushUntexturedBatches(LLRenderPass& pass, U32 type);
    bool pushRiggedBatches(LLRenderPass& pass, U32 type, bool texture, bool batch_textures);
    bool pushUntexturedRiggedBatches(LLRenderPass& pass, U32 type);
    bool pushMaskBatches(LLRenderPass& pass, U32 type, bool texture, bool batch_textures);
    bool pushRiggedMaskBatches(LLRenderPass& pass, U32 type, bool texture, bool batch_textures);
    bool renderGroup(LLRenderPass& pass, LLSpatialGroup* group, U32 type, bool texture);
    bool renderRiggedGroup(LLRenderPass& pass, LLSpatialGroup* group, U32 type, bool texture);

    // GLTF (LLFetchedGLTFMaterial): its uniforms as patches, its textures as
    // pushGLTFBatch's direct draw names them, double sided as the draw's cull.
    bool pushGLTFBatches(U32 type);
    bool pushUntexturedGLTFBatches(U32 type);
    bool pushRiggedGLTFBatches(U32 type);
    bool pushUntexturedRiggedGLTFBatches(U32 type);
}

#endif // PLUS_DRAWLIST_DRAWLIST_H

/**
 * @file alphalist.h
 * @brief Alpha draw lists: the alpha pool's batch loop as draw lists
 *        (research/137 N3).
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

#ifndef PLUS_ALPHALIST_ALPHALIST_H
#define PLUS_ALPHALIST_ALPHALIST_H

#include "stdtypes.h"

// LLDrawPoolAlpha::renderAlpha's loop, drawn as draw lists (research/137 N3,
// llrhi/llrhidrawlist.h). The entry is LLDrawPoolAlpha::renderAlphaList,
// defined in alphalist.cpp (a copy of renderAlpha's loop): renderAlpha calls
// it first and draws as before when it returns false (the switch off, HUDs,
// the depth pass, the alpha census or GPU timing segments, mirror mode).
//
// research/205 3a-5 (H6): while the retained scene's alpha takes a pass
// (LLRHI_SCENE_ALPHA verify or on: the main view's above-water colour
// passes of non-rigged alpha, with the scene's alpha view open), renderAlpha
// does not call renderAlphaList: its own loop draws what the scene does not
// (llscenerecords.h). This copy then serves only the other passes (rigged,
// the HUDs, under water, the pre-water pool) and LLRHI_SCENE_ALPHA off. Once
// the scene's alpha has run in auto without a difference, the copy can go
// with the draw lists of those passes (research/205 section 10).
//
// With LLRHI_DRAW_LISTS (1 or 2) and LLRHI_DRAW_LISTS_ALPHA=1, the batches the
// loop would draw one by one (LLRenderPass::drawGeometry: a direct draw each,
// whose program, units and uniforms the translation layer resolves again for
// every draw) go into a list while the program stays the same. What a batch
// changes on the way goes with its draw instead of through GL:
//  - the blend function (gGL.blendFunc) and the culling of particle groups and
//    double-sided materials: Draw::state;
//  - the specular colour, environment intensity, brightness, minimum alpha and
//    hair sway uniforms (LLGLSLShader::uniform*): uniform patches, only where
//    the value changes, as the shader's value cache would send them;
//  - the textures TexSetup binds (by unit, as bindFast would) and its texture
//    matrix: Draw::textures and Draw::texture_matrix;
//  - the skinning palette (LLRenderPass::uploadMatrixPalette): the palette's
//    row (Draw::palette_base), when the backend says the program reads rows;
//  - the OIT priority (LLRHI::sOITPriority): Draw::oit_priority.
// After each submission the viewer's caches (the shader's values, LLRender's
// blend, the units' bindings, sOITPriority) are set the usual way to what the
// old path leaves (promise 4 of research/137 section 2.3).
//
// A batch goes the old way (the list submitted first, then the original code)
// when its program changes, for blended GLTF materials, palettes without a
// row, textures not ready for a direct draw (no GL name yet, sampler options
// to apply), mapped buffers, programs whose uniforms have no native slot, and
// the glow pass of a group that has emissive batches. Those cuts are counted
// by reason (takeStats).
namespace plus::alphalist
{
    // Why a list was submitted before the loop's end.
    enum Cut : U8
    {
        CUT_PROGRAM,        // the next batch takes another program
        CUT_GLTF,           // a blended GLTF material (its bind sets uniforms and textures through GL)
        CUT_PALETTE,        // a palette without a row (or a program that reads no rows)
        CUT_TEXTURE,        // a texture without a GL name or with sampler options to apply
        CUT_BUFFER,         // a mapped vertex buffer
        CUT_UNIFORMS,       // a program with a uniform without a native slot
        CUT_NOT_TAKEN,      // the backend took no list now, or did not take a draw
        CUT_GLOW,           // the group's glow pass (emissive batches)
        CUT_OIT_VARIANT,    // asked the backend whether a program makes OIT nodes
        CUT_END,            // the end of renderAlpha
        CUT_COUNT
    };
    const char* cutName(U32 cut);

    // Counts since the last takeStats (main thread). research/137 N7 shows
    // them with the draw lists' own (LLRHI::DrawList::takeStats).
    struct Stats
    {
        U32 passes = 0;         // renderAlpha calls drawn by renderAlphaList
        U32 listed = 0;         // batches that went into lists
        U32 old_path = 0;       // batches drawn the old way inside those calls
        U32 cuts[CUT_COUNT] = {};
    };
    void takeStats(Stats& out);
}

#endif // PLUS_ALPHALIST_ALPHALIST_H

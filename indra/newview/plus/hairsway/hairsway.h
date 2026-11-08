/**
 * @file hairsway.h
 * @brief "Hair sway": the agent's rigged hair lags behind the head's motion
 *        (two damped springs); the skinning shaders move each hair vertex by
 *        its height below the head, the roots not at all.
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

#ifndef PLUS_HAIRSWAY_H
#define PLUS_HAIRSWAY_H

#include "stdtypes.h"

#include <vector>

class LLGLSLShader;
class LLMatrix4a;
class LLMeshSkinInfo;
class LLVOAvatar;

namespace plus::hairsway
{
    // The agent's hair palettes carry SWAY_ENTRIES more mat3x4 entries after
    // the joints (no vertex indexes them): what the skinning shaders need to
    // move each hair vertex by how far it hangs below the hairline, the hair
    // on the scalp not at all. As the shaders read them (column c of an
    // entry: the LLMatrix4a's row c xyz, and its row 3's component c):
    //   entry 0: (hairline plane n, w: depth = w - dot(n, p)), (head's centre, SWAY_MARK), (middle offset, 0)
    //   entry 1: (tips' offset, 0), (depths: still, middle, tips; part of an inward offset kept), 0
    // Shaders: objectSkinV.glsl (GL and the translated Vulkan shaders, entry
    // index in the uniform plusHairSway) and the native skinned shadow shaders
    // in llvulkan/llvknative.cpp (which find the entries by SWAY_MARK).
    constexpr U32 SWAY_ENTRIES = 2;
    constexpr F32 SWAY_MARK    = -98765.f;  // never a joint's translation; llvknative.cpp has the same value

    // The <plus> hook in LLVOAvatar::updateSkinInfoMatrixPalette(): called
    // right after the joints of one rigged mesh (skin) were built for this
    // frame, before they are copied for GL / Vulkan. When they belong to the
    // agent's hair and the feature is on, appends the sway entries and returns
    // the new count; else returns count and leaves the palette as it is.
    // Any thread.
    U32 adjustPalette(LLVOAvatar* avatar, const LLMeshSkinInfo* skin, std::vector<LLMatrix4a>& palette, U32 count);

    // Where a palette's sway entries start (its joint count); 0: none.
    U32 swayIndex(const std::vector<LLMatrix4a>& palette);

    // The <plus> hook in LLRenderPass's palette upload (lldrawpool.cpp): tells
    // the bound skinning shader where this palette's sway entries are
    // (uniform plusHairSway, 0: none). Does nothing until the feature has been
    // on once. Render thread.
    void bindShader(LLGLSLShader* shader, const std::vector<LLMatrix4a>& palette);

    // research/219: the retained scene draws a rigged pass's records with one
    // value of plusHairSway for the pass (never a palette with sway entries):
    // 0 in the bound skinning shader, as bindShader with no sway. Render thread.
    void clearShader(LLGLSLShader* shader);
}

#endif // PLUS_HAIRSWAY_H

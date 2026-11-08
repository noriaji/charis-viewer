/**
 * @file texanim.h
 * @brief Smooth texture scrolls worked out where they are drawn, not stepped
 *        every frame (research/194 11).
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

#ifndef PLUS_TEXANIM_H
#define PLUS_TEXANIM_H

#include "llrhi.h"
#include "m4math.h"

class LLViewerTextureAnim;
class LLVOVolume;

// Upstream steps every texture animation in the world every frame
// (LLViewerTextureAnim::updateClass: 882 us of the 1.70 ms idle objects in
// heavy-leeaf-2, research/193 5a) and writes each face's texture matrix.
//
// Here, after upstream's step has set a smooth scroll up (the face matrices
// made, mTexAnimMode set), the faces' matrices take the scroll's description
// (llrhitexanim.h) and the animation is marked (mPlusDrawTime): updateClass
// leaves it out from then on. Whoever turns a texture matrix into GPU data or
// uses it on the CPU turns the description into the matrix at the frame's
// animation time (LLRHI::texAnimMatrix in the backend, matrixOf() in
// llface.cpp) with upstream's arithmetic. The animation's timer and
// mLastTime are kept as upstream's last step left them (the anchor), so
// giving it back (release) continues where upstream would be.
//
// Given back to upstream's step: the animation changes (animationChanging,
// before the update message's values are read), the faces change
// (LLVOVolume::updateGeometry with a face, LOD or texture coordinate change:
// texture entry edits, new faces), the switch goes off (keepDrawTime). The
// next step sets it up again and it is described again.
//
// LLRHI::sTexAnimGpu / sTexAnimGpuVerify / sTexAnimGpuAuto (llrhi.h).
// Main thread.
namespace plus::texanim
{
    // Top of updateClass: the frame's animation time (LLRHI::sTexAnimTime),
    // the log line every 5 s (verify, LLRHI_IDLE_SKIP_STATS=1 or
    // LLRHI_IDLE_CENSUS=1).
    void beginFrame();

    // updateClass, for a marked animation: true while the switch is on (no
    // step); else it is given back (false: upstream's step runs).
    bool keepDrawTime(LLViewerTextureAnim* anim);

    // End of LLVOVolume::animateTextures (a step that ran; result: upstream's
    // animateTextures result): verify, or the faces take the description.
    void afterStepOn(LLVOVolume* vobj, S32 result);
    inline void afterStep(LLVOVolume* vobj, S32 result)
    {
        if (LLRHI::sTexAnimGpu || LLRHI::sTexAnimGpuVerify)
        {
            afterStepOn(vobj, result);
        }
    }

    // processUpdateMessage, before a texture animation update is read.
    void animationChanging(LLViewerTextureAnim* anim);

    // Back to upstream's step: the timer as upstream's last step would have
    // left it, every face holding a description gets the matrix at the
    // frame's animation time (from its texture entry now). No-op unless marked.
    void release(LLViewerTextureAnim* anim);

    // A face's texture matrix as a matrix: `m`, or its description's matrix
    // now in `scratch`.
    inline const LLMatrix4& matrixOf(const LLMatrix4& m, LLMatrix4& scratch)
    {
        const F32* f = &m.mMatrix[0][0];
        if (!LLRHITexAnim::isDescription(f))
        {
            return m;
        }
        LLRHITexAnim::matrixAt(f, LLRHI::sTexAnimTime, &scratch.mMatrix[0][0]);
        return scratch;
    }
}

#endif // PLUS_TEXANIM_H

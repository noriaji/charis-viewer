/**
 * @file idleskip.h
 * @brief Texture animations and spins (llTargetOmega) of objects nobody sees
 *        are not stepped every frame (research/178 D1, D2).
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

#ifndef PLUS_IDLESKIP_H
#define PLUS_IDLESKIP_H

#include "m4math.h"

class LLDrawable;
class LLViewerObject;
class LLVOVolume;

namespace plus::idleskip
{
    // Whether any camera (the main view, the sun and spot shadow maps, the
    // reflection probes' faces, the water and mirror views) took the drawable,
    // its octree group or its bridge in the last few frames. Reads the frame
    // numbers the culls write; changes nothing (unlike
    // LLDrawable::isRecentlyVisible, which marks and reads the current camera
    // only). Main thread.
    bool seenRecently(LLDrawable* drawable);

    // The <plus> hook at the top of LLVOVolume::animateTextures: true when this
    // frame's step can be left out (LLRHI::sIdleSkipTexAnim on, the animation
    // already set up (animating: mTexAnimMode), the object not selected and
    // not on the HUD, not seen recently, and not its turn of the one frame in
    // HIDDEN_PERIOD a hidden one still steps). The animation is a function of
    // its timer (LLViewerTextureAnim::animateTextures), so the next step lands
    // where it would have been. Counts. Main thread.
    bool skipTexAnim(LLVOVolume* vobj, bool animating);
    // research/179 (cpu-present), LLVOVolume::animateTextures: the faces of one
    // step mostly have the same five values (offset, scale, rotation: the
    // animation's, the rest from each face's texture entry), and upstream builds
    // the same matrix for each (two quaternion-to-matrix steps and a 4x4
    // product a face). One of these per step, on the stack: copy() before the
    // face's build hands over the face before's matrix when the values are
    // the same bit for bit (true: the face is done; LLRHI::sTexAnimMemo), and
    // built() after a build keeps it for the next face. With
    // LLRHI::sTexAnimMemoVerify copy() never hands over; built() compares the
    // fresh build with what it would have copied and counts differences
    // (IdleSkip line; must stay 0). Main thread.
    class TexAnimMatrices
    {
    public:
        TexAnimMatrices() = default;
        ~TexAnimMatrices();
        TexAnimMatrices(const TexAnimMatrices&) = delete;
        TexAnimMatrices& operator=(const TexAnimMatrices&) = delete;
        bool copy(F32 off_s, F32 off_t, F32 scale_s, F32 scale_t, F32 rot, LLMatrix4& out);
        void built(const LLMatrix4& matrix);

    private:
        F32 mValues[5] = {};    // copy()'s, for built()
        F32 mKept[5] = {};      // of mMatrix
        LLMatrix4 mMatrix;
        bool mHave = false;     // mMatrix holds a build
        bool mCandidate = false;    // verify: copy() would have handed mMatrix over
        U32 mBuilt = 0;
        U32 mCopied = 0;
        U32 mDiffered = 0;
    };

    // The <plus> hook in LLViewerObject::idleUpdate, where upstream would
    // interpolate (not static, velocity interpolation on, not selected): true
    // when the object's idle update this frame can be left out: a pure spin (angular
    // velocity only, no velocity or acceleration, nothing moved it since the
    // last update), a volume, not an avatar or attachment, not selected, no
    // particle source, sound or light on it or on a child, no avatar sitting
    // on it, not seen recently, not its turn. The update after it applies the
    // whole time passed (applyAngularVelocity with dt since the last update):
    // the same rotation, constant omega. Counts. Main thread.
    bool skipSpin(LLViewerObject* objectp);

    // For the texture visit pace (llviewertexturelist.cpp, research/178 G1):
    // whether the world has stopped loading. Loading: before login is done,
    // a new object this frame (LLViewerObjectList::mNumNewObjects), a mesh
    // header or LOD queued or in flight, a texture fetch request, a decoded
    // texture waiting for GL creation, or groups the load radius or the
    // priority rebuild budget left waiting. Settled once none of these has
    // been seen for SETTLE_SECONDS on end (one loading frame starts the count
    // over: no flapping while things trickle in). A gap in the calls (the pace
    // was off) starts the count over too. Once a frame, main thread.
    bool loadSettled();
    // Whether the last loadSettled() call saw something loading that frame.
    bool loadingNow();

    // Once a frame after the object updates (LLViewerObjectList::update):
    // the counters' log line every 5 s while LLRHI::sIdleSkipStats is on.
    void endFrame();
}

#endif // PLUS_IDLESKIP_H

/**
 * @file texanim.cpp
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

#include "llviewerprecompiledheaders.h"

#include "texanim.h"

#include "llappviewer.h"
#include "lldrawable.h"
#include "llface.h"
#include "llframetimer.h"
#include "llrhi.h"
#include "lltextureentry.h"
#include "llviewertextureanim.h"
#include "llvovolume.h"

namespace
{
    // auto: this many faces compared with no mismatch, then on.
    constexpr U64 AUTO_CHECKS = 2000;
    // verify: the first mismatches logged one by one.
    constexpr U64 LOGGED_MISMATCHES = 5;
    // A matrix element differs when |a - b| > TOLERANCE * (1 + |b|): the
    // arithmetic is upstream's, so a right description gives the same floats
    // (the anchor time's two words may round the timer's F32 elapsed time one
    // step the other way: far below this).
    constexpr F32 TOLERANCE = 1e-4f;

    struct Counts
    {
        U64 frames = 0;
        U64 kept = 0;           // marked animations updateClass left out (summed over frames)
        U64 described = 0;      // animations whose faces took a description
        U64 released = 0;       // given back to upstream's step
        U64 faces = 0;          // faces that took a description
        U64 checked = 0;        // verify: faces compared
        U64 mismatches = 0;
        F32 worst = 0.f;        // verify: the largest difference seen
    };
    Counts sCounts;
    U64 sAutoChecked = 0;       // auto: compared so far (not cleared with the window)
    U64 sLogged = 0;            // mismatches logged
    F32 sWindowStart = 0.f;     // gFrameTimeSeconds of the window's start

    F64 frame_time()
    {
        return LLFrameTimer::getElapsedSeconds().value();
    }

    // Stage S1: the smooth scroll (ON | LOOP | SMOOTH, nothing else), with
    // finite values and a rate that moves it.
    bool scroll(const LLViewerTextureAnim& anim)
    {
        const U8 need = LLTextureAnim::ON | LLTextureAnim::LOOP | LLTextureAnim::SMOOTH;
        const U8 other = LLTextureAnim::PING_PONG | LLTextureAnim::REVERSE | LLTextureAnim::ROTATE | LLTextureAnim::SCALE;
        return (anim.mMode & need) == need && !(anim.mMode & other) && anim.mRate != 0.f && llfinite(anim.mRate) &&
               llfinite(anim.mStart) && llfinite(anim.mLength);
    }

    // LLViewerTextureAnim::animateTextures: num_frames (no ping-pong: the full length).
    F32 full_length(const LLViewerTextureAnim& anim)
    {
        if (anim.mLength)
        {
            return anim.mLength;
        }
        return llmax(1.f, (F32)(anim.mSizeX * anim.mSizeY));
    }

    // LLVOVolume::animateTextures: the faces one step writes.
    void face_range(LLVOVolume& vobj, const LLViewerTextureAnim& anim, S32& start, S32& end)
    {
        start = 0;
        end = vobj.mDrawable->getNumFaces() - 1;
        if (anim.mFace >= 0 && anim.mFace <= end)
        {
            start = end = anim.mFace;
        }
    }

    // Upstream's step leaves a small face that has a matrix alone.
    bool step_writes(LLFace& face)
    {
        return !(face.getVirtualSize() <= MIN_TEX_ANIM_SIZE && face.mTextureMatrix);
    }

    void describe(F32* d, const LLViewerTextureAnim& anim, F64 anchor_time, F32 last, const LLTextureEntry& te)
    {
        F32 rot = 0.f;
        F32 scale_s = 1.f;
        F32 scale_t = 1.f;
        te.getRotation(&rot);
        te.getScale(&scale_s, &scale_t);
        LLRHITexAnim::describeScroll(d, anim.mRate, anchor_time, last, full_length(anim), anim.mStart, (F32)anim.mSizeX,
                                     (F32)anim.mSizeY, rot, scale_s, scale_t);
    }

    void set_anchor(LLViewerTextureAnim& anim, F64 time, F32 last)
    {
        LLRHITexAnim::splitTime(time, anim.mPlusAnchor[0], anim.mPlusAnchor[1]);
        anim.mPlusAnchor[2] = last;
        anim.mPlusAnchored = true;
    }

    F64 anchor_time(const LLViewerTextureAnim& anim)
    {
        return (F64)anim.mPlusAnchor[0] + (F64)anim.mPlusAnchor[1];
    }

    // verify: the faces upstream's step just wrote against the description
    // made from the anchor of the step before (the timer's state that step
    // left: upstream's step reads exactly it), at this frame's time.
    void compare_step(LLVOVolume& vobj, LLViewerTextureAnim& anim)
    {
        const F64 now = LLRHI::sTexAnimTime;
        S32 start, end;
        face_range(vobj, anim, start, end);
        for (S32 i = start; i <= end; ++i)
        {
            LLFace* facep = vobj.mDrawable->getFace(i);
            const LLTextureEntry* te = facep ? facep->getTextureEntry() : nullptr;
            if (!te || !facep->mTextureMatrix || facep->getVirtualSize() <= MIN_TEX_ANIM_SIZE)
            {
                continue;   // not written by this step
            }
            F32 d[16];
            describe(d, anim, anchor_time(anim), anim.mPlusAnchor[2], *te);
            F32 m[16];
            LLRHITexAnim::matrixAt(d, now, m);
            const F32* upstream = &facep->mTextureMatrix->mMatrix[0][0];
            F32 worst = 0.f;
            bool differs = false;
            for (S32 k = 0; k < 16; ++k)
            {
                const F32 diff = fabsf(m[k] - upstream[k]);
                worst = llmax(worst, diff);
                differs |= !(diff <= TOLERANCE * (1.f + fabsf(upstream[k])));    // a NaN differs
            }
            ++sCounts.checked;
            ++sAutoChecked;
            sCounts.worst = llmax(sCounts.worst, worst);
            if (!differs)
            {
                continue;
            }
            ++sCounts.mismatches;
            if (sLogged++ < LOGGED_MISMATCHES)
            {
                LL_WARNS("VKPerf") << "texanim gpu mismatch: object " << vobj.getID() << " face " << i << " rate " << anim.mRate
                                   << " size " << (S32)anim.mSizeX << "x" << (S32)anim.mSizeY << " largest difference " << worst
                                   << " upstream offset " << upstream[12] << "," << upstream[13] << " description " << m[12]
                                   << "," << m[13] << LL_ENDL;
            }
            if (LLRHI::sTexAnimGpuAuto)
            {
                // auto: off for the run at the first mismatch (upstream steps every animation).
                LLRHI::sTexAnimGpuAuto = false;
                LLRHI::sTexAnimGpu = false;
                LLRHI::sTexAnimGpuVerify = false;
                LL_WARNS("VKPerf") << "texanim gpu [auto]: a mismatch after " << sAutoChecked << " compared faces; off for this run"
                                   << LL_ENDL;
                return;
            }
        }
    }

    // The faces of a step just taken take the description. All or none: a
    // face the step should have written without a matrix (none made yet)
    // leaves the animation to upstream.
    void describe_faces(LLVOVolume& vobj, LLViewerTextureAnim& anim)
    {
        S32 start, end;
        face_range(vobj, anim, start, end);
        for (S32 i = start; i <= end; ++i)
        {
            LLFace* facep = vobj.mDrawable->getFace(i);
            if (facep && facep->getTextureEntry() && step_writes(*facep) && !facep->mTextureMatrix)
            {
                return;
            }
        }
        // The anchor: the timer started at this frame's time (upstream's
        // SMOOTH step just reset it) and mLastTime, set again so that they
        // are the anchor whatever happened before.
        const F64 now = LLRHI::sTexAnimTime;
        const F32 last = (F32)anim.plusLastTime();
        anim.plusSetState(last, frame_time() - now);
        set_anchor(anim, now, last);
        for (S32 i = start; i <= end; ++i)
        {
            LLFace* facep = vobj.mDrawable->getFace(i);
            const LLTextureEntry* te = facep ? facep->getTextureEntry() : nullptr;
            if (te && step_writes(*facep))
            {
                describe(&facep->mTextureMatrix->mMatrix[0][0], anim, now, last, *te);
                ++sCounts.faces;
            }
        }
        anim.mPlusDrawTime = true;
        ++sCounts.described;
    }
}

namespace plus::texanim
{
    void beginFrame()
    {
        LLRHI::sTexAnimTime = frame_time();

        ++sCounts.frames;
        const F32 now = gFrameTimeSeconds;
        if (now - sWindowStart < 5.f)
        {
            return;
        }
        sWindowStart = now;
        const Counts c = sCounts;
        sCounts = Counts();
        const U32 decodes = LLRHI::sTexAnimDecodes.exchange(0, std::memory_order_relaxed);
        if (!LLRHI::sTexAnimGpuVerify && !LLRHI::sIdleSkipStats && !LLRHI::sIdleCensus)
        {
            return;
        }
        const F64 f = (F64)llmax(c.frames, (U64)1);
        LL_INFOS("VKPerf") << llformat("texanim gpu [%s] over %llu frames, per frame: drawn-time scrolls %.0f, decoded by the backend %.0f, "
                                       "described %.2f (faces %.2f), given back %.2f",
                                       LLRHI::sTexAnimGpuAuto ? "auto" : LLRHI::sTexAnimGpuVerify ? "verify" : LLRHI::sTexAnimGpu ? "on" : "off",
                                       (unsigned long long)c.frames, c.kept / f, decodes / f, c.described / f, c.faces / f,
                                       c.released / f)
                           << (c.checked || LLRHI::sTexAnimGpuVerify
                                   ? llformat("; verify checked %llu, mismatches %llu, largest difference %g",
                                              (unsigned long long)c.checked, (unsigned long long)c.mismatches, c.worst)
                                   : std::string())
                           << LL_ENDL;
    }

    bool keepDrawTime(LLViewerTextureAnim* anim)
    {
        if (LLRHI::sTexAnimGpu && !LLRHI::sTexAnimGpuVerify)
        {
            ++sCounts.kept;
            return true;
        }
        release(anim);
        return false;
    }

    void afterStepOn(LLVOVolume* vobj, S32 result)
    {
        LLViewerTextureAnim* anim = vobj->mTextureAnimp;
        if (!anim || anim->mPlusDrawTime)
        {
            return;
        }
        if (!scroll(*anim) || vobj->isFlexible() || vobj->mDrawable.isNull() || vobj->mDrawable->isDead())
        {
            anim->mPlusAnchored = false;
            return;
        }
        if (LLRHI::sTexAnimGpuVerify)
        {
            if (anim->mPlusAnchored && result && (vobj->mTexAnimMode & LLViewerTextureAnim::TRANSLATE))
            {
                compare_step(*vobj, *anim);
                if (LLRHI::sTexAnimGpuAuto && LLRHI::sTexAnimGpuVerify && sAutoChecked >= AUTO_CHECKS)
                {
                    LLRHI::sTexAnimGpuAuto = false;
                    LLRHI::sTexAnimGpuVerify = false;
                    LL_INFOS("VKPerf") << "texanim gpu [auto]: " << sAutoChecked
                                       << " faces compared with no mismatch; on (verify off)" << LL_ENDL;
                }
            }
            // The timer's state upstream's SMOOTH step just left: reset at this frame's time.
            set_anchor(*anim, LLRHI::sTexAnimTime, (F32)anim->plusLastTime());
            return;
        }
        if (LLRHI::sTexAnimGpu && (vobj->mTexAnimMode & LLViewerTextureAnim::TRANSLATE))
        {
            describe_faces(*vobj, *anim);
        }
    }

    void animationChanging(LLViewerTextureAnim* anim)
    {
        if (!anim)
        {
            return;
        }
        release(anim);
        anim->mPlusAnchored = false;
    }

    void release(LLViewerTextureAnim* anim)
    {
        if (!anim || !anim->mPlusDrawTime)
        {
            return;
        }
        anim->mPlusDrawTime = false;
        ++sCounts.released;

        // Upstream's SMOOTH step at the animation time (its last step: this
        // frame's in the drawing, the frame before's in the update message).
        const F64 now = LLRHI::sTexAnimTime;
        const F32 elapsed = (F32)(now - anchor_time(*anim));
        const F32 last = elapsed * anim->mRate + anim->mPlusAnchor[2];
        anim->plusSetState(last, frame_time() - now);
        set_anchor(*anim, now, last);

        LLVOVolume* vobj = anim->plusVolume();
        if (!vobj || vobj->mDrawable.isNull())
        {
            return;
        }
        // Every face still holding a description (the range may have changed
        // under it): the matrix at that time, from its texture entry now.
        const S32 faces = vobj->mDrawable->getNumFaces();
        for (S32 i = 0; i < faces; ++i)
        {
            LLFace* facep = vobj->mDrawable->getFace(i);
            if (!facep || !facep->mTextureMatrix)
            {
                continue;
            }
            F32* m = &facep->mTextureMatrix->mMatrix[0][0];
            if (!LLRHITexAnim::isDescription(m))
            {
                continue;
            }
            F32 d[16];
            if (const LLTextureEntry* te = facep->getTextureEntry())
            {
                describe(d, *anim, now, last, *te);
            }
            else
            {
                memcpy(d, m, sizeof(d));
            }
            LLRHITexAnim::matrixForCounter(d, last, m);
        }
    }
}

/**
 * @file imagescensus.h
 * @brief What the texture list's pass (pre-gbuffer images list) takes, and why (research/348 branch 0).
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

#ifndef PLUS_IMAGESCENSUS_IMAGESCENSUS_H
#define PLUS_IMAGESCENSUS_IMAGESCENSUS_H

#include "linden_common.h"

#include <intrin.h> // __rdtsc

class LLVOAvatar;

// LLRHI_IMAGES_CENSUS=1: a VKPerf "images census" set of lines every 10 s about
// LLViewerTextureList::updateImages (the "pre-gbuffer images list" segment of
// FrameCost), counting only; nothing it reads is changed and nothing it calls
// has an effect (no getWorldMatrix, no isVisuallyMuted, no cache written):
//   (i)   the budget: how often the fetch pass (and the urgent pass) stopped on its
//         time limit, and what the pass would take uncut (due / visited x its ms);
//   (ii)  of the rigged faces whose area was worked out, how many had a joint with
//         a dirty world matrix, by joint kind (bone, collision volume, attachment
//         point) and avatar kind, both at the moment of the work and as the pass
//         began (what a scan on another thread, before any of this pass's own
//         rebuilds, would meet);
//   (iii) textures with such a face, in three groups by their worst face (all
//         joints clean / only collision volumes dirty / a bone or attachment dirty);
//   (iv)  the time split: the stages of updateImages, and in the rigged walk of
//         LLFace::calcPixelArea the 216-entry isRiggedTo scan against the work
//         for the joints it finds (getJoint, world matrix, box).
// The stages are also cut at FrameCost marks (LLRHI_FRAME_COST=2, names
// "pre-gbuffer images list ..."), only while the census is on. Time is the TSC.
// Off: every entry point is one flag test. Main thread only.
namespace plus::imagescensus
{
    // sOn: set once from the environment. sActive: a pass of updateImages is open.
    extern bool sOn;
    extern bool sActive;

    enum Stage : U8
    {
        PROLOGUE = 0,   // updateImages up to the fast cache
        FAST_CACHE,     // updateImagesLoadingFastCache
        CREATE,         // updateImagesCreateTextures
        URGENT,         // updateImagesUrgent
        FETCH_COPY,     // updateImagesFetchTextures: the copy of the entries
        FETCH_LOOP,     // ... the visit loop (planning and the visits)
        AFTER,          // loaded callbacks, stats, the texture census
        STAGE_COUNT
    };

    // The pass of updateImages (after its early returns), one frame.
    class PassScope
    {
    public:
        PassScope() : mOn(sOn)
        {
            if (mOn)
            {
                begin();
            }
        }
        ~PassScope()
        {
            if (mOn)
            {
                end();
            }
        }
        PassScope(const PassScope&) = delete;
        PassScope& operator=(const PassScope&) = delete;

    private:
        void begin();
        void end();
        bool mOn;
    };

    // The pass enters a stage (closing the one before): the time is counted and FrameCost is marked.
    void enterStage(Stage stage);
    inline void stage(Stage stage_in)
    {
        if (sActive)
        {
            enterStage(stage_in);
        }
    }

    // The time limit updateImages was given (after its own doubling).
    void noteBudget(F32 seconds);
    inline void budget(F32 seconds)
    {
        if (sActive)
        {
            noteBudget(seconds);
        }
    }

    // The fetch pass ended: the limit it had, the textures in the list, those it took up (copied: due and
    // left out by the pace), those due, those visited, whether it stopped on the limit with some left, its
    // loop's seconds and the planning ms.
    void noteFetch(F32 budget_seconds, size_t textures, size_t copied, size_t due, size_t visited, bool cut, F32 loop_seconds,
                   F64 plan_ms);
    inline void fetchDone(F32 budget_seconds, size_t textures, size_t copied, size_t due, size_t visited, bool cut, F32 loop_seconds,
                          F64 plan_ms)
    {
        if (sActive)
        {
            noteFetch(budget_seconds, textures, copied, due, visited, cut, loop_seconds, plan_ms);
        }
    }

    // The urgent pass ended: its entries, those reached, whether it stopped on the limit with some left.
    void noteUrgent(size_t entries, size_t reached, bool cut);
    inline void urgentDone(size_t entries, size_t reached, bool cut)
    {
        if (sActive)
        {
            noteUrgent(entries, reached, cut);
        }
    }

    // One texture's visit in the fetch loop (the face scan and updateFetch): counted for the group
    // of its rigged faces (see RiggedWalk), if it had any whose area was worked out.
    void beginTexture();
    void endTexture();
    inline void textureBegin()
    {
        if (sActive)
        {
            beginTexture();
        }
    }
    inline void textureEnd()
    {
        if (sActive)
        {
            endTexture();
        }
    }

    // The rigged walk of LLFace::calcPixelArea (the face is rigged and its area is due), main thread:
    //     RiggedWalk census(avatar);
    //     [if the face's table needs an update: census.infoBegin(); ...; census.infoEnd();]
    //     for each entry that isRiggedTo(): census.beforeJoint(i); [the work]; census.afterJoint();
    //     census.end(has_extents);
    // beforeJoint reads (before the joint's world matrix is asked for) and never calls anything that writes.
    class RiggedWalk
    {
    public:
        explicit RiggedWalk(LLVOAvatar* avatar) : mOn(sActive)
        {
            if (mOn)
            {
                begin(avatar);
            }
        }
        ~RiggedWalk()
        {
            end(false);
        }
        RiggedWalk(const RiggedWalk&) = delete;
        RiggedWalk& operator=(const RiggedWalk&) = delete;

        void infoBegin()
        {
            if (mOn)
            {
                mInfoStart = __rdtsc();
            }
        }
        void infoEnd()
        {
            if (mOn)
            {
                mInfoTicks += __rdtsc() - mInfoStart;
                ++mInfoPairs;
            }
        }
        void beforeJoint(S32 index)
        {
            if (mOn)
            {
                joint(index);
            }
        }
        void afterJoint()
        {
            if (mOn)
            {
                mBodyTicks += __rdtsc() - mBodyStart;
            }
        }
        // Closes the walk (once): had the face any joint to give a box?
        void end(bool has_extents)
        {
            if (mOn && !mEnded)
            {
                mEnded = true;
                finish(has_extents);
            }
        }

    private:
        void begin(LLVOAvatar* avatar);
        void joint(S32 index);
        void finish(bool has_extents);

        LLVOAvatar* mAvatar = nullptr;
        void* mSlot = nullptr;      // the avatar's record of this pass (null: none left)
        U64 mStart = 0;
        U64 mBodyStart = 0;
        U64 mBodyTicks = 0;
        U64 mInfoStart = 0;
        U64 mInfoTicks = 0;
        U64 mSelfTicks = 0;         // what the census itself took outside the joints (taken out of the stage)
        U64 mJointSelfTicks = 0;    // ... in the reads of the joints
        U32 mJoints = 0;            // joints that were there
        U32 mCalls = 0;             // beforeJoint calls (3 TSC reads each, with afterJoint)
        U32 mInfoPairs = 0;         // infoBegin / infoEnd pairs (2 reads each)
        U8 mKind = 0;
        U8 mComputeMask = 0;        // joint kinds dirty as the work was done
        U8 mStartMask = 0;          // ... as the pass began
        bool mOn;
        bool mEnded = false;
    };
}

#endif // PLUS_IMAGESCENSUS_IMAGESCENSUS_H

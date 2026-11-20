/**
 * @file idlecensus.h
 * @brief The idle objects segment of FrameCost cut into its steps (research/193 A).
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

#ifndef PLUS_IDLECHEAP_IDLECENSUS_H
#define PLUS_IDLECHEAP_IDLECENSUS_H

#include "llrhi.h"  // LLRHI::sIdleCensus

#include <intrin.h> // __rdtsc

class LLViewerObject;
class LLVOAvatar;

// LLRHI_IDLE_CENSUS=1 (idlecensus.toggle): the main thread's time in each
// step of the idle objects segment (llappviewer.cpp, between the
// "idle objects" and "idle cleanup" marks), read with the TSC (a few ns a
// read, converted to us against the steady clock over each 5 s window), and
// the calls a frame; a VKPerf line every 5 s. Steps nest (an avatar's idle
// update holds its character update, which holds its motions): the line
// names the outer step of each. The updateMove calls of the "idle move"
// segment come along (the moved list), apart. Off: a step is one flag test,
// no clock read, nothing allocated. Main thread only (a step on another
// thread is not counted).
namespace plus::idlecensus
{
    enum Step : U8
    {
        // llappviewer.cpp idle(), the idle objects segment
        DESELECT = 0,       // LLSelectMgr::deselectAllIfTooFar
        AGENT,              // gestures, gAgent.updateAgentPosition
        OBJECT_LIST,        // gObjectList.update (all of it)
        GLTF,               // LL::GLTFSceneManager::update
        // LLViewerObjectList::update
        LIST_SETUP,         // the settings, the clock, the copy of the active list
        AVATAR_PREPARE,     // rhi_prepare_avatar_motions (keyframes on the cull threads)
        IDLE_AVATAR,        // idleUpdate of an avatar (not an animesh)
        IDLE_ANIMESH,       // ... of a control avatar
        IDLE_VOLUME,        // ... of a volume
        IDLE_OTHER,         // ... of anything else (tree, grass, particles, sky, water)
        FLEXIBLE,           // LLVolumeImplFlexible::updateClass
        TEXANIM,            // LLViewerTextureAnim::updateClass
        AVATAR_FINISH,      // rhi_finish_avatar_motions
        IDLESKIP_END,       // plus::idleskip::endFrame
        OBJECT_COSTS,       // fetchObjectCosts, fetchPhysicsFlags
        COMPLEXITY,         // LLVOVolume::updateRenderComplexity
        FRAME_STATS,        // LLViewerStats::updateFrameStats
        // inside the object updates
        TEXANIM_STEP,       // LLVOVolume::animateTextures that stepped (built the matrices)
        TEXANIM_SKIP,       // ... that the idle skip left (research/178 D1)
        UPDATE_DRAWABLE,    // LLViewerObject::idleUpdate: updateDrawable (the move list)
        AV_TOO_SLOW,        // LLVOAvatar::updateTooSlow
        AV_TEXTURE_LOADING, // LLVOAvatar::checkTextureLoading (the call from idleUpdate only)
        AV_CHARACTER,       // LLVOAvatar::updateCharacter
        AV_MOTIONS,         // ... of which updateMotions (and the gaze over it)
        AV_MISC,            // LLVOAvatar::idleUpdateMisc (attachments)
        AV_APPEARANCE,      // LLVOAvatar::idleUpdateAppearanceAnimation
        AV_LOADING,         // LLVOAvatar::idleUpdateLoadingEffect
        AV_NAME_TAG,        // LLVOAvatar::idleUpdateNameTag
        AV_COMPLEXITY,      // LLVOAvatar::idleUpdateRenderComplexity
        AV_VPARAM_DEFAULT,  // LLVOAvatar::visualParamWeightsAreDefault (from wherever)
        // the rest of LLVOAvatar::idleUpdate, each apart (research/339 §22; measurement only)
        AV_GRAYWIRE,        // GrayWire::updateAvatar
        AV_EXTENT,          // the extent refresh period (upd_freq, mNeedsExtentUpdate)
        AV_PIXEL_AREA,      // setPixelAreaAndAngle and the cull-rank check
        AV_OFFSCREEN_LIGHT, // plusOffscreenLightFrame (the decision of a light frame)
        AV_BASE_IDLE,       // LLViewerObject::idleUpdate (the base class; updateDrawable is inside)
        AV_LAZY_ATTACH,     // lazyAttach (attachments waiting for a drawable)
        AV_DRAWABLE_MOVE,   // the "force asynchronous drawable update" block (updateMove*Async of the avatar or its seat)
        AV_VOICE,           // the voice test (getVoiceEnabled, inProximalChannel)
        AV_VOICE_VIS,       // idleUpdateVoiceVisualizer
        AV_NAME_POS,        // idleCalcNameTagPosition
        AV_LIPSYNC,         // idleUpdateLipSync
        AV_WIND,            // idleUpdateBelowWater and idleUpdateWindEffect
        AV_CATCHUP,         // plusCatchUpAttachments
        AV_DEBUG_INFO,      // idleUpdateDebugInfo (the debug text)
        MOVE_ASYNC,         // LLPipeline::updateMoveDampedAsync / NormalAsync (avatars, sitters)
        MOVE_LIST,          // LLDrawable::updateMove from updateMovedList ("idle move", not idle objects)
        // inside AV_GRAYWIRE (research/339 §29, 349; measurement only). Nested in it, so none is added to the avatar's
        // apart steps. The light part of graywire (what each avatar does every frame) is graywire less the band and
        // the heavy checks, worked out in the line.
        GW_ENTRY,           // GrayWire::updateAvatar: loadingOn and the table lookup of the avatar's entry
        GW_BAND,            // ... updateAvatarBand (a band sweeps the avatar: every frame of it)
        GW_HEAVY_FULL,      // ... the periodic check (every CHECK_EVERY_FRAMES) of an avatar in view, not an impostor (this includes your own avatar and the animesh in view: both always have update period 1, so the count can be above the RHIAvatar "full" number)
        GW_HEAVY_IMPOSTOR,  // ... of an avatar in view that is an impostor (update period over 1)
        GW_HEAVY_OFFSCREEN, // ... of an avatar out of view
        GW_NOTE_BOUNDS,     // inside the check: noteBounds
        GW_KEEP,            // ... keepMaterialized (a materialized avatar's short way out)
        GW_LISTED,          // ... noteListedAttachments
        GW_BAKES,           // ... bakesHaveGray
        GW_SCAN,            // ... scanAttachments (Walk::SCAN, the attachments' face walk)
        GW_WAITS,           // ... waitsForAttachments
        GW_HOLD,            // ... scanAttachments (Walk::HOLD_WIRE, a wireframe's walk)
        GW_FLIP,            // ... the change of its gray state (startAvatarBand and the rest)
        GW_SEEN,            // ... seenWhole
        GW_CLASS,           // GrayWire::updateClass (once a frame, from the texture update: outside the idle objects)
        STEP_COUNT
    };

    void add(Step step, U64 ticks);

    // Which periodic-check step of graywire (GW_HEAVY_FULL, _IMPOSTOR or _OFFSCREEN) an avatar's check is, from flags
    // it has already (out of view; the update period of an impostor). Asked only while the census is on.
    Step graywireHeavyStep(LLVOAvatar* avatar);

    // A step from construction to destruction (or stop()).
    class Scope
    {
    public:
        explicit Scope(Step step) : mStep(step), mOn(LLRHI::sIdleCensus)
        {
            if (mOn)
            {
                mStart = __rdtsc();
            }
        }
        ~Scope()
        {
            stop();
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        // Counted as another step (decided after it started).
        void relabel(Step step)
        {
            mStep = step;
        }
        void stop()
        {
            if (mOn)
            {
                add(mStep, __rdtsc() - mStart);
                mOn = false;
            }
        }
        // Ends this step and starts the next (one clock read).
        void next(Step step)
        {
            if (mOn)
            {
                const U64 now = __rdtsc();
                add(mStep, now - mStart);
                mStart = now;
            }
            else if (LLRHI::sIdleCensus)
            {
                mOn = true;
                mStart = __rdtsc();
            }
            mStep = step;
        }

    private:
        U64 mStart = 0;
        Step mStep;
        bool mOn;
    };

    // The idle update of one object, by its kind (decided only when on).
    class ObjectScope
    {
    public:
        explicit ObjectScope(LLViewerObject* objectp) : mOn(LLRHI::sIdleCensus)
        {
            if (mOn)
            {
                start(objectp);
            }
        }
        ~ObjectScope()
        {
            if (mOn)
            {
                add(mStep, __rdtsc() - mStart);
            }
        }
        ObjectScope(const ObjectScope&) = delete;
        ObjectScope& operator=(const ObjectScope&) = delete;

    private:
        void start(LLViewerObject* objectp);
        U64 mStart = 0;
        Step mStep = IDLE_OTHER;
        bool mOn;
    };

    // Once a frame, after the idle objects segment: the VKPerf line every
    // 5 s while the census is on (with the parameter memo's counters), or
    // the memo's line alone while it verifies. Off: one flag test.
    void endFrame();
}

#endif // PLUS_IDLECHEAP_IDLECENSUS_H

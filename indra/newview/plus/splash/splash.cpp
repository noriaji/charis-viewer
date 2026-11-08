/**
 * @file splash.cpp
 * @brief Splash when you dive into water (and a small one when you come out), made by the viewer.
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

// Charis's own (research/120, the rain's splashes are the model for the look).
//
// What it does: every frame, while PlusSplash is on, it watches your pelvis against the region's water
// height. When the pelvis goes from above the water to under it, a splash is made at that spot, sized by
// how fast you came in (falling speed, plus a share of the sideways speed); a jump from the water to the
// air makes a smaller one. The speed is the velocity the simulator gives (yours, or that of what you sit
// on), not the pelvis' move over the frame: a sit or a stand moves the pelvis a metre in a frame, and
// a frame hitch makes any move look fast. It is not a fluid: it is the official particle system (LLViewerPart, the
// same thing that draws a script's llParticleSystem) given a burst of its own:
//   - the column: bright drops thrown up and out, pulled down by gravity, gone when they fall back to
//     the water;
//   - the fine spray: many small, fast, short-lived drops around the column;
//   - the foam: flat-moving white dots that spread out over the water surface and fade.
// So it draws through the existing particle path (no new Vulkan pass), costs nothing while off, and
// respects the particle count limits (LLViewerPartSim::shouldAddPart() / the particle render type).
// Only your own avatar for now: other avatars and objects entering the water are a later step.
//
// Not made: right after a teleport or when the region changes (the pelvis jumps), for SIT_QUIET seconds
// after you sit down or stand up (the pelvis jumps onto the seat or off it, in water too), when the world
// is frozen (plus/freeze), when the water is not in this region's range (no water height), when the
// pelvis moved more than JUMP_LIMIT metres in one frame, and while particles are not drawn (the particle
// render type off: particles are not simulated then, so a splash would wait and come out when they are
// drawn again).

#include "llviewerprecompiledheaders.h"

#include "plus.h"
#include "plus/freeze/freeze.h"

#include "llagent.h"
#include "llcallbacklist.h"
#include "llcharacter.h"
#include "llframetimer.h"
#include "llviewercontrol.h"
#include "llviewerpartsim.h"
#include "llviewerpartsource.h"
#include "llviewerregion.h"
#include "llviewertexture.h"
#include "llvoavatarself.h"
#include "llstartup.h"
#include "pipeline.h"

namespace
{
    // Falling speed (m/s) that makes the biggest splash; a slower entry is smaller.
    constexpr F32 FULL_SPEED = 9.f;
    // Splashes under this strength are not made (wading, bobbing at the surface).
    constexpr F32 MIN_STRENGTH = 0.12f;
    // A pelvis that moved this far in one frame was teleported or warped: no splash.
    constexpr F32 JUMP_LIMIT = 8.f;
    // After a splash, the next one at least this many seconds later (bobbing across the surface).
    constexpr F32 COOLDOWN = 0.6f;
    // After you sit down or stand up, no splash for this many seconds.
    constexpr F32 SIT_QUIET = 1.f;

    // The source the splash particles name (they need one; it is never added to the simulation, since
    // all particles are made at once).
    class SplashSource : public LLViewerPartSource
    {
    public:
        SplashSource() : LLViewerPartSource(LL_PART_SOURCE_NULL) {}
        void update(const F32) override {}
    };

    // The drops of the column and the spray fall back; they die when they are under the water they came
    // from (mParameter holds the water height). The foam (no vertical speed) stays.
    void dropCallback(LLViewerPart& part, const F32)
    {
        if (part.mVelocity.mV[VZ] < 0.f && part.mPosAgent.mV[VZ] < part.mParameter)
        {
            part.mFlags = LLPartData::LL_PART_DEAD_MASK;
        }
    }

    F32 frand(F32 lo, F32 hi)
    {
        return lo + (hi - lo) * ll_frand();
    }

    struct State
    {
        bool        have_last = false;
        LLVector3   last_pelvis;
        LLUUID      last_region;
        F32         cooldown = 0.f;
        bool        last_sitting = false;
        LLFrameTimer sit_timer;     // since you last sat down or stood up
        LLFrameTimer timer;
        LLPointer<SplashSource> source;
    };

    State& state()
    {
        static State s;
        return s;
    }

    LLViewerPart* makePart(State& st, LLViewerTexture* image, const LLVector3& pos, const LLVector3& vel, const LLVector3& accel,
                           F32 life, F32 scale_start, F32 scale_end, F32 alpha, F32 water, U32 extra_flags)
    {
        LLViewerPart* part = new LLViewerPart();
        part->init(st.source, image, dropCallback);
        part->mPosAgent = pos;
        part->mVelocity = vel;
        part->mAccel = accel;
        part->mMaxAge = life;
        part->mLastUpdateTime = 0.f;
        part->mStartColor = LLColor4(0.93f, 0.97f, 1.f, alpha);
        part->mEndColor = LLColor4(0.93f, 0.97f, 1.f, 0.f);
        part->mStartScale = LLVector2(scale_start, scale_start);
        part->mEndScale = LLVector2(scale_end, scale_end);
        part->mScale = part->mStartScale;
        part->mFlags = LLPartData::LL_PART_INTERP_COLOR_MASK | LLPartData::LL_PART_INTERP_SCALE_MASK | extra_flags;
        part->mParameter = water;
        part->mBlendFuncSource = LLRender::BF_SOURCE_ALPHA;
        part->mBlendFuncDest = LLRender::BF_ONE_MINUS_SOURCE_ALPHA;
        part->mStartGlow = 0.f;
        part->mEndGlow = 0.f;
        part->mGlow = LLColor4U(0, 0, 0, 0);
        return part;
    }

    // One splash at the water's surface. strength 0..1, level 1..3 (the setting).
    void burst(const LLVector3& at, F32 water, F32 strength, S32 level, bool leaving)
    {
        State& st = state();
        if (!st.source)
        {
            st.source = new SplashSource();
        }
        LLViewerTexture* image = LLViewerFetchedTexture::sDefaultParticleImagep;
        if (!image)
        {
            return;
        }
        LLViewerPartSim& sim = LLViewerPartSim::instance();

        // The level scales the count and the size: 1 small, 2 normal, 3 big.
        const F32 amount = (level == 1) ? 0.6f : (level == 3 ? 1.6f : 1.f);
        const F32 size   = (level == 1) ? 0.8f : (level == 3 ? 1.3f : 1.f);
        const F32 s      = strength;

        const LLVector3 base(at.mV[VX], at.mV[VY], water + 0.02f);
        const LLVector3 gravity(0.f, 0.f, -9.8f);

        // The column: bright drops thrown up and out.
        const S32 column = (S32)((leaving ? 14.f : 24.f) * (0.4f + s) * amount);
        for (S32 i = 0; i < column && sim.shouldAddPart(); ++i)
        {
            const F32 angle = frand(0.f, F_TWO_PI);
            const F32 out   = frand(0.3f, 1.6f) * (0.5f + s) * (leaving ? 0.5f : 1.f);
            const F32 up    = frand(1.5f, 4.5f) * (0.4f + 0.9f * s) * (leaving ? 0.7f : 1.f);
            const F32 ring  = frand(0.f, 0.35f);
            LLVector3 pos = base + LLVector3(cosf(angle) * ring, sinf(angle) * ring, 0.f);
            LLVector3 vel(cosf(angle) * out, sinf(angle) * out, up);
            const F32 d = frand(0.1f, 0.22f) * size * (0.7f + 0.6f * s);
            sim.addPart(makePart(st, image, pos, vel, gravity, frand(0.8f, 1.5f), d, d * 0.6f, 0.75f, water, 0));
        }

        // The fine spray: many small, fast drops.
        const S32 spray = (S32)((leaving ? 30.f : 70.f) * (0.3f + s) * amount);
        for (S32 i = 0; i < spray && sim.shouldAddPart(); ++i)
        {
            const F32 angle = frand(0.f, F_TWO_PI);
            const F32 out   = frand(0.6f, 3.2f) * (0.5f + s);
            const F32 up    = frand(1.f, 6.f) * (0.4f + 0.9f * s);
            LLVector3 pos = base + LLVector3(cosf(angle) * 0.1f, sinf(angle) * 0.1f, 0.f);
            LLVector3 vel(cosf(angle) * out, sinf(angle) * out, up);
            const F32 d = frand(0.04f, 0.09f) * size;
            sim.addPart(makePart(st, image, pos, vel, gravity, frand(0.5f, 1.1f), d, d * 0.5f, 0.6f, water, 0));
        }

        // The foam: white dots spreading out over the surface, then fading.
        const S32 foam = (S32)((leaving ? 10.f : 26.f) * (0.4f + s) * amount);
        for (S32 i = 0; i < foam && sim.shouldAddPart(); ++i)
        {
            const F32 angle = frand(0.f, F_TWO_PI);
            const F32 out   = frand(0.4f, 1.8f) * (0.4f + s);
            LLVector3 vel(cosf(angle) * out, sinf(angle) * out, 0.f);
            LLVector3 accel(-vel.mV[VX] * 0.5f, -vel.mV[VY] * 0.5f, 0.f);   // slows as it spreads
            const F32 d = frand(0.18f, 0.35f) * size * (0.6f + 0.8f * s);
            sim.addPart(makePart(st, image, base, vel, accel, frand(1.2f, 2.4f), d, d * 1.8f, 0.5f, water, 0));
        }
    }

    void onIdle()
    {
        State& st = state();
        const F32 dt = llmin(st.timer.getElapsedTimeAndResetF32(), 0.25f);
        st.cooldown = llmax(0.f, st.cooldown - dt);

        static LLCachedControl<S32> splash_level(gSavedSettings, "PlusSplash", 0);     // every frame: no lookup by name (fix-pipeleak)
        const S32 level = splash_level;
        if (level <= 0 || !isAgentAvatarValid() || LLStartUp::getStartupState() != STATE_STARTED ||
            gAgent.getTeleportState() != LLAgent::TELEPORT_NONE || plus::freeze::isFrozen())
        {
            st.have_last = false;
            return;
        }

        LLVOAvatarSelf* avatar = gAgentAvatarp;
        LLViewerRegion* region = avatar->getRegion();
        if (!region)
        {
            st.have_last = false;
            return;
        }
        const LLVector3 pelvis = avatar->mPelvisp ? avatar->mPelvisp->getWorldPosition() : avatar->getPositionAgent();
        const F32 water = region->getWaterHeight();

        const bool sitting = avatar->isSitting();
        if (sitting != st.last_sitting)
        {
            st.last_sitting = sitting;
            st.sit_timer.reset();
        }
        const bool sit_quiet = st.sit_timer.getElapsedTimeF32() < SIT_QUIET;

        if (st.have_last && st.last_region == region->getRegionID() && !sit_quiet)
        {
            const LLVector3 move = pelvis - st.last_pelvis;
            if (move.magVec() < JUMP_LIMIT)
            {
                const bool was_above = st.last_pelvis.mV[VZ] > water;
                const bool is_above  = pelvis.mV[VZ] > water;
                if (was_above != is_above && st.cooldown <= 0.f)
                {
                    // Yours, or that of what you sit on (a vehicle: getRootEdit() stops at it).
                    const LLVector3 vel = avatar->getRootEdit()->getVelocity();
                    const F32 fall    = fabsf(vel.mV[VZ]);
                    const F32 side    = sqrtf(vel.mV[VX] * vel.mV[VX] + vel.mV[VY] * vel.mV[VY]);
                    F32 strength      = llclamp((fall + 0.3f * side) / FULL_SPEED, 0.f, 1.f);
                    // Particles off: they are not simulated either, so the splash would wait and come
                    // out when particles are on again.
                    if (strength >= MIN_STRENGTH && gPipeline.hasRenderType(LLPipeline::RENDER_TYPE_PARTICLES))
                    {
                        // Where the path crossed the water, not where the frame ended.
                        const F32 span = move.mV[VZ];
                        const F32 t = (fabsf(span) > 1e-4f) ? llclamp((water - st.last_pelvis.mV[VZ]) / span, 0.f, 1.f) : 1.f;
                        burst(st.last_pelvis + move * t, water, strength, level, is_above);
                        st.cooldown = COOLDOWN;
                    }
                }
            }
        }
        st.last_pelvis = pelvis;
        st.last_region = region->getRegionID();
        st.have_last = true;
    }

    void init()
    {
        gIdleCallbacks.addFunction([](void*) { onIdle(); }, nullptr);
    }

    plus::FeatureRegistration sRegistration({ "splash", &init, {} });
}

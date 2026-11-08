/**
 * @file nudge.cpp
 * @brief Fine adjustment of the avatar's facing and position.
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

#include "nudge.h"

#include "llagent.h"
#include "llbutton.h"
#include "llcallbacklist.h"
#include "llfloater.h"
#include "lltimer.h"
#include "llviewercontrol.h"
#include "llviewerinput.h"
#include "llviewermessage.h"
#include "llviewerregion.h"
#include "llvoavatarself.h"

namespace
{
    constexpr char TURN_STEP_SETTING[] = "PlusNudgeTurnStep"; // degrees per step

    // A held key or button repeats after a pause, at a slow fixed rate: a step is
    // meant to be small and countable, not a spin.
    constexpr F32 REPEAT_DELAY = 0.5f;   // seconds before the repeat starts
    constexpr F64 REPEAT_PERIOD = 0.12;  // seconds between repeated steps

    F64 sLastStep = 0.0;

    bool repeat_due()
    {
        const F64 now = LLTimer::getTotalSeconds();
        if (now - sLastStep < REPEAT_PERIOD)
        {
            return false;
        }
        sLastStep = now;
        return true;
    }

    // Sends the update now, with the flags set so far. Small changes (a turn under
    // about 2.5 degrees, a one-frame nudge flag) would otherwise be left out or
    // delayed by the viewer's send throttle.
    void send_now()
    {
        send_agent_update(true);
    }

    // Turns the avatar's body by the step in the settings, and nothing else first.
    // The official turn (LLAgent::yaw) only turns the view; the body follows once it
    // is more than AvatarRotateThresholdSlow (about 45 degrees) off the view, so a
    // small turn would change only the camera. Here the root joint (the pelvis) is
    // turned by exactly the step, and the view by the same step so that the gap
    // between them stays what it was (no jump of the camera, no snap-back of the body).
    // TURN_LEFT/RIGHT tells the simulator to turn its avatar to the BodyRotation.
    void turn_step(S32 dir)
    {
        if (!isAgentAvatarValid() || gAgentAvatarp->isSitting() || gAgent.isMovementLocked()
            || !gAgentAvatarp->mRoot)
        {
            return;
        }
        const F32 degrees = llclamp(gSavedSettings.getF32(TURN_STEP_SETTING), 0.1f, 45.f);
        const F32 radians = dir * degrees * DEG_TO_RAD;

        const LLVector3 up = gAgent.getReferenceUpVector();
        const LLQuaternion body = gAgentAvatarp->mRoot->getWorldRotation();
        gAgentAvatarp->mRoot->setWorldRotation(body * LLQuaternion(radians, up));
        gAgent.yaw(radians);

        gAgent.setControlFlags(dir > 0 ? AGENT_CONTROL_TURN_LEFT : AGENT_CONTROL_TURN_RIGHT);
        send_now();
    }

    // The simulator's own small step: a nudge flag in one AgentUpdate. The measured
    // movement (log of the 3rd version: 68 and 83 cm for flags of 56 to 78 ms, whatever
    // the time) shows that the distance does not follow the time the flag is on: the
    // simulator moves the avatar by a fixed amount for each update that carries the
    // flag (about 17 cm). So one nudge = the flag in exactly one frame (one update, like
    // the official arrow key tap), a frame without it, and the next nudge. The distance
    // wanted (PlusNudgeStepCm) is a whole number of nudges, never less than one. How far
    // one nudge goes is learned (PlusNudgeUnitCm): after a burst the avatar is left to
    // stop and the distance it went is measured. A burst that did not move the avatar
    // at all (an update lost) is sent again, up to twice.
    constexpr char STEP_CM_SETTING[] = "PlusNudgeStepCm";
    constexpr char UNIT_CM_SETTING[] = "PlusNudgeUnitCm";
    // A nudge is about 17 cm walking (the logs above). A measurement of more than UNIT_MAX_CM a nudge is not
    // one: the avatar was pushed, fell or was teleported while it was measured. It is not learned (and a
    // saved unit above it, learned before this limit, is put back to the default).
    constexpr F32 UNIT_MIN_CM = 3.f;
    constexpr F32 UNIT_MAX_CM = 50.f;
    constexpr F32 UNIT_FOLLOW = 0.5f;       // how far one measurement moves the unit
    // The movement must go the way asked: its sideways part at most this much of its part along the way
    // (about 35 degrees). Else something else moved the avatar (a push, a slope, an object).
    constexpr F64 MAX_ACROSS_RATIO = 0.7;
    constexpr S32 MAX_QUEUED = 30;          // nudges waiting at most (a held key keeps adding)
    constexpr F32 STILL_SPEED = 0.05f;      // m/s: the avatar counts as stopped below this
    constexpr F32 MOVED_MIN_CM = 2.f;       // less than this: the nudge did not move it
    constexpr S32 MAX_RETRIES = 2;
    constexpr F64 MEASURE_WAIT = 0.8;       // s after the last flag: the movement may not have started yet (network)
    constexpr F64 MEASURE_STILL = 0.2;      // s the avatar must stay stopped
    constexpr F64 MEASURE_TIMEOUT = 3.0;

    struct Burst
    {
        S32 at = 0;         // +1 forward, -1 back
        S32 left = 0;       // +1 left, -1 right
        S32 remaining = 0;  // nudges still to send
        S32 sent = 0;
        U32 last_frame = 0; // the frame of the last flag; the next one is two frames later
        bool active = false;
    } sBurst;

    // The measurement of the burst that has just been made.
    struct Measure
    {
        bool pending = false;   // set at the start of a burst made from standing still
        bool active = false;    // the last flag is sent, the avatar is left to stop
        bool registered = false;
        S32 at = 0;
        S32 left = 0;
        S32 count = 0;          // nudges asked for
        S32 retries = 0;
        LLVector3d from;
        LLVector3d way;         // the horizontal unit vector the burst moves the avatar along
        U64 region = 0;         // the region's handle at the start: another one at the end is a teleport (or a crossing)
        F64 end_time = 0.0;
        F64 still_since = -1.0;
    } sMeasure;

    void start_burst(S32 at, S32 left, S32 count, S32 retries);

    bool measure_tick()
    {
        const F64 now = LLTimer::getTotalSeconds();
        if (!sMeasure.active || sBurst.active || !isAgentAvatarValid())
        {
            // a new burst began (or none is measured): this one is not clean
            sMeasure.active = false;
            sMeasure.registered = false;
            return true;
        }
        if (gAgentAvatarp->getVelocity().magVec() < STILL_SPEED)
        {
            if (sMeasure.still_since < 0.0)
            {
                sMeasure.still_since = now;
            }
        }
        else
        {
            sMeasure.still_since = -1.0;
        }
        const bool settled = sMeasure.still_since >= 0.0
            && now - sMeasure.still_since > MEASURE_STILL
            && now - sMeasure.end_time > MEASURE_WAIT;
        if (!settled && now - sMeasure.end_time < MEASURE_TIMEOUT)
        {
            return false;
        }

        sMeasure.active = false;
        sMeasure.registered = false;
        if (!settled)
        {
            return true;
        }
        const LLViewerRegion* region = gAgent.getRegion();
        if (!region || region->getHandle() != sMeasure.region || gAgent.getTeleportState() != LLAgent::TELEPORT_NONE)
        {
            LL_INFOS("Nudge") << "burst: the region changed or a teleport is under way; not measured" << LL_ENDL;
            return true;
        }
        LLVector3d moved = gAgent.getPositionGlobal() - sMeasure.from;
        moved.mdV[VZ] = 0.0;
        const F32 cm = (F32)moved.magVec() * 100.f;
        const F64 along = moved * sMeasure.way;                        // metres the way asked (dot product)
        const F64 across = (moved - sMeasure.way * along).magVec();    // metres off it
        const F32 along_cm = (F32)along * 100.f;
        const F32 unit = gSavedSettings.getF32(UNIT_CM_SETTING);
        F32 learned = unit;
        const bool did_move = cm >= MOVED_MIN_CM;
        const char* rejected = nullptr;
        if (did_move)
        {
            if (along <= 0.0 || across > along * MAX_ACROSS_RATIO)
            {
                rejected = " (not the way asked: pushed? not learned)";
            }
            else if (along_cm / sMeasure.count > UNIT_MAX_CM)
            {
                rejected = " (too far for the nudges: pushed or fell? not learned)";
            }
            else
            {
                learned = llclamp(lerp(unit, along_cm / sMeasure.count, UNIT_FOLLOW), UNIT_MIN_CM, UNIT_MAX_CM);
                gSavedSettings.setF32(UNIT_CM_SETTING, learned);
            }
        }
        LL_INFOS("Nudge") << (sMeasure.left ? "sideways" : "forward/back") << " burst: "
                          << sMeasure.count << " nudge(s) (retry " << sMeasure.retries << "), moved "
                          << (S32)cm << " cm (" << (S32)along_cm << " the way asked), one nudge " << unit << " -> " << learned << " cm"
                          << (did_move ? (rejected ? rejected : "") : " (did not move)") << LL_ENDL;
        if (!did_move && sMeasure.retries < MAX_RETRIES
            && !gAgentAvatarp->isSitting() && !gAgent.isMovementLocked())
        {
            start_burst(sMeasure.at, sMeasure.left, sMeasure.count, sMeasure.retries + 1);
        }
        return true;
    }

    void begin_measure()
    {
        if (!sMeasure.pending)
        {
            return;
        }
        sMeasure.pending = false;
        sMeasure.active = true;
        sMeasure.end_time = LLTimer::getTotalSeconds();
        sMeasure.still_since = -1.0;
        if (!sMeasure.registered)
        {
            sMeasure.registered = true;
            doOnIdleRepeating(measure_tick);
        }
    }

    // One nudge = the flag in one frame, with a frame without it before the next
    // nudge (that frame's update clears the flag; without it two nudges would be one
    // long flag, and the second would not be an update of its own).
    bool burst_tick()
    {
        if (!isAgentAvatarValid() || gAgentAvatarp->isSitting() || gAgent.isMovementLocked())
        {
            sBurst = Burst();
            sMeasure.pending = false;
            return true;
        }
        const U32 frame = LLFrameTimer::getFrameCount();
        if (sBurst.sent > 0 && frame < sBurst.last_frame + 2)
        {
            return false;
        }
        if (sBurst.remaining > 0)
        {
            // the flag in this frame only: the frame's own update (the viewer sends a
            // change of control flags at once) carries it, once
            if (sBurst.at)
            {
                gAgent.moveAtNudge(sBurst.at);
            }
            if (sBurst.left)
            {
                gAgent.moveLeftNudge(sBurst.left);
            }
            sBurst.last_frame = frame;
            ++sBurst.sent;
            --sBurst.remaining;
            return false;
        }
        begin_measure();
        sBurst = Burst();
        return true;
    }

    void start_burst(S32 at, S32 left, S32 count, S32 retries)
    {
        if (!isAgentAvatarValid())
        {
            return;
        }
        if (sBurst.active)
        {
            // a held key or a second press: more nudges behind the ones waiting (this burst is not measured)
            sBurst.remaining = llmin(sBurst.remaining + count, MAX_QUEUED);
            sBurst.at = at;
            sBurst.left = left;
            sMeasure.pending = false;
            return;
        }
        sMeasure.active = false;
        sMeasure.pending = gAgentAvatarp->getVelocity().magVec() < STILL_SPEED;
        sMeasure.at = at;
        sMeasure.left = left;
        sMeasure.count = count;
        sMeasure.retries = retries;
        sMeasure.from = gAgent.getPositionGlobal();
        sMeasure.region = gAgent.getRegion() ? gAgent.getRegion()->getHandle() : 0;
        // the way the nudge flags move the avatar: along its facing (at) and its left
        LLVector3d way = LLVector3d(gAgent.getAtAxis()) * (F64)at + LLVector3d(gAgent.getLeftAxis()) * (F64)left;
        way.mdV[VZ] = 0.0;
        if (way.normalize() <= 0.0)
        {
            sMeasure.pending = false; // no way to measure along (looking straight up or down)
        }
        sMeasure.way = way;

        sBurst = Burst();
        sBurst.active = true;
        sBurst.at = at;
        sBurst.left = left;
        sBurst.remaining = count;
        doOnIdleRepeating(burst_tick);
        burst_tick(); // the first flag in this frame (the keyboard and the buttons run before the frame's update)
    }

    void start_nudge(S32 at, S32 left)
    {
        const F32 step_cm = llclamp(gSavedSettings.getF32(STEP_CM_SETTING), 1.f, 100.f);
        if (gSavedSettings.getF32(UNIT_CM_SETTING) > UNIT_MAX_CM)
        {
            // learned from a teleport or a push before such measurements were left out: not a nudge
            if (LLControlVariable* control = gSavedSettings.getControl(UNIT_CM_SETTING))
            {
                control->resetToDefault(true);
            }
        }
        const F32 unit_cm = llclamp(gSavedSettings.getF32(UNIT_CM_SETTING), UNIT_MIN_CM, UNIT_MAX_CM);
        start_burst(at, left, llmax(1, (S32)llround(step_cm / unit_cm)), 0);
    }

    void step_at(S32 dir)
    {
        start_nudge(dir, 0);
    }

    void step_left(S32 dir)
    {
        start_nudge(0, dir);
    }

    // One step on the key press, then repeats while it is held.
    bool key_step(EKeystate state, S32 dir, void (*step)(S32))
    {
        if (state == KEYSTATE_UP)
        {
            return true;
        }
        if (state == KEYSTATE_DOWN)
        {
            sLastStep = LLTimer::getTotalSeconds();
            step(dir);
        }
        else if (gKeyboard->getCurKeyElapsedTime() > REPEAT_DELAY && repeat_due())
        {
            step(dir);
        }
        return true;
    }

    void connect(LLFloater* floater, const char* name, void (*step)(S32), S32 dir)
    {
        LLButton* button = floater->findChild<LLButton>(name);
        if (!button)
        {
            LL_WARNS("Nudge") << "button " << name << " not found in the Move floater" << LL_ENDL;
            return;
        }
        // The press is one step; holding repeats it.
        button->setMouseDownCallback([step, dir](LLUICtrl*, const LLSD&)
        {
            sLastStep = LLTimer::getTotalSeconds();
            step(dir);
        });
        button->setHeldDownDelay(REPEAT_DELAY);
        button->setHeldDownCallback([step, dir](LLUICtrl*, const LLSD&)
        {
            if (repeat_due())
            {
                step(dir);
            }
        });
    }
}

namespace plus::nudge
{
    bool keyTurn(EKeystate state, S32 dir)     { return key_step(state, dir, turn_step); }
    bool keyStepAt(EKeystate state, S32 dir)   { return key_step(state, dir, step_at); }
    bool keyStepLeft(EKeystate state, S32 dir) { return key_step(state, dir, step_left); }

    void bindKeys(const BindKey& bind)
    {
        struct Binding { KEY key; MASK mask; const char* command; };
        static const Binding BINDINGS[] =
        {
            { KEY_LEFT,  MASK_CONTROL,              "nudge_turn_left" },
            { KEY_RIGHT, MASK_CONTROL,              "nudge_turn_right" },
            { KEY_UP,    MASK_CONTROL | MASK_SHIFT, "nudge_step_forward" },
            { KEY_DOWN,  MASK_CONTROL | MASK_SHIFT, "nudge_step_back" },
            { KEY_LEFT,  MASK_CONTROL | MASK_SHIFT, "nudge_step_left" },
            { KEY_RIGHT, MASK_CONTROL | MASK_SHIFT, "nudge_step_right" },
        };
        for (S32 mode : { (S32)MODE_FIRST_PERSON, (S32)MODE_THIRD_PERSON })
        {
            for (const Binding& b : BINDINGS)
            {
                bind(mode, b.key, b.mask, b.command);
            }
        }
    }

    void setupFloater(LLFloater* floater)
    {
        connect(floater, "nudge turn left btn",  turn_step, 1);
        connect(floater, "nudge turn right btn", turn_step, -1);
        connect(floater, "nudge forward btn",    step_at,   1);
        connect(floater, "nudge back btn",       step_at,   -1);
        connect(floater, "nudge left btn",       step_left, 1);
        connect(floater, "nudge right btn",      step_left, -1);
    }
}

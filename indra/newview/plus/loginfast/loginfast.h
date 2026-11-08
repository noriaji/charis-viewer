/**
 * @file loginfast.h
 * @brief Fast login and teleport arrival: the curtain lifts when the world is
 *        there, not after fixed waits; the timeline of both in the log.
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

#ifndef PLUS_LOGINFAST_H
#define PLUS_LOGINFAST_H

#include "stdtypes.h"

#include <string>

// research/212, research/241. Upstream holds the login curtain for a fixed
// PrecachingDelay (6 s) and then for the agent's clothes (up to 10 s), fades it
// in 1 s, holds the teleport curtain for a fixed TeleportArrivalDelay (2 s), and
// gives the simulator back the full draw distance 10% a second after arriving.
//
// PlusFastLogin (on by default): the login curtain lifts once the agent's
// avatar is there and 97% of the agent region's land patches have come, at
// least MIN_LOGIN_SECONDS after the world began to draw under it; at the latest
// at upstream's PrecachingDelay. The clothes are not waited for (they go on
// loading in the world, as after upstream's 10 s). The fade takes 0.7 s.
// A first login (initial outfit, gender chooser) keeps upstream's ways.
// PlusFastTeleport (on by default): the teleport curtain lifts once 97% of the
// new region's land has come, at least MIN_TELEPORT_SECONDS after the arrival;
// at the latest at TeleportArrivalDelay. After an arrival the far range sent to
// the simulator comes back from its half in half steps (one or two
// AgentUpdates) instead of tenths; other steps up stay in tenths.
// Off: upstream's waits, unchanged.
//
// PlusLoginTimeline (on by default, log only): one "login timeline" line (tag
// LoginTimeline) with every startup state's time since the login began, and one
// "teleport timeline" line per teleport, in the format of plus/regioncross. It
// does not depend on the two switches, so the waits can be compared on and off.
// Main thread.
namespace plus::loginfast
{
    // PlusFastLogin / PlusFastTeleport.
    bool loginOn();
    bool teleportOn();

    // ---- login (llstartup.cpp) ----

    // LLStartUp::setStartupState(): the timeline's steps (EStartupState values).
    void onStartupState(S32 from, S32 to, const std::string& to_name);

    // STATE_PRECACHE, each frame: true when the curtain may lift before the
    // PrecachingDelay (seconds since STATE_PRECACHE began).
    bool loginCurtainReady(F32 precache_seconds);

    // STATE_WEARABLES_WAIT: true when the clothes are not waited for.
    bool skipWearablesWait();

    // LLProgressView: the fade into the world (upstream 1 s).
    F32 fadeToWorldSeconds();
    // the curtain coming down when the login button is pressed (upstream: 1 s)
    F32 fadeFromLoginSeconds();

    // LLProgressView: the login curtain has gone (end of the fade).
    void onLoginCurtainGone();

    // ---- login, the waits between the states (llstartup.cpp, llviewerdisplay.cpp) ----
    // PlusFastLogin also (these four apply to a first login too): the decoding
    // before UseCircuitCode takes 0.2 s at most (upstream 2 s); a state that is
    // done goes on to the next one in the same frame while the frame has taken
    // less than CHAIN_SECONDS; and the presents from inside the startup states
    // come at most PRESENT_FPS times a second (each waits for the display's
    // refresh with VSync; the main loop's own present is never skipped).
    // PlusFastLoginEarlyUCC (off by default until it is tried on the grids) with
    // PlusFastLogin: UseCircuitCode goes on entering STATE_SEED_GRANTED_WAIT,
    // while the seed capability is asked (upstream: after the seed's answer and
    // the decoding). The circuit is then not read until STATE_WORLD_WAIT; the
    // login timeline gives how long ("UseCircuitCode unread N s").

    // STATE_SEED_GRANTED_WAIT, each frame: true the first time in a login
    // attempt when UseCircuitCode is to be sent early.
    bool sendCircuitEarly();

    // STATE_SEED_CAP_GRANTED: true when it was sent early in this attempt.
    bool circuitSentEarly();

    // STATE_WORLD_WAIT, before it reads the circuit: the timeline's step.
    void onCircuitRead();

    // STATE_SEED_CAP_GRANTED: the most each of the two decodeAllImages() takes.
    F32 decodeSeconds();

    // Head of idle_startup(): this frame's states begin.
    void startupFrameBegin();

    // End of a state's block: true when the state that follows (next) is to run
    // now, in this frame. False when the state is not next (a wait not over, a
    // failure back to the login screen) or the frame has taken long enough.
    bool chain(S32 next);

    // display(): the next display_startup() is the main loop's own, not skipped.
    void presentFromLoop();

    // display_startup(): true when this present is skipped (one was shown less
    // than 1 / PRESENT_FPS s ago, and it is not the main loop's own). Only
    // between the login button and STATE_STARTED.
    bool skipStartupPresent();

    // ---- teleport (llviewerdisplay.cpp, llviewermessage.cpp) ----

    // update_tp_display(), each call: the timeline follows the teleport state.
    void onTeleportFrame(S32 teleport_state);

    // TELEPORT_ARRIVING: true when the curtain may lift before the
    // TeleportArrivalDelay (seconds since the arrival).
    bool teleportCurtainReady(F32 arrival_seconds);

    // TELEPORT_PENDING / TELEPORT_START: whether the teleport curtain comes
    // down. PlusDisableTeleportScreens (off by default; Firestorm's
    // FSDisableTeleportScreens, Preferences > Move & View) keeps it from
    // coming down: the world stays in view through the teleport, the states
    // and their waits run as with the curtain (the hidden progress view still
    // takes the percent and the text), and TELEPORT_NONE hides it as ever.
    // Not the login's or the logout's curtain.
    bool teleportScreenShown();

    // AgentUpdate: the far range was halved for an arrival (a teleport's, or the
    // login's before STATE_MISC).
    void onFarHalved();
    // AgentUpdate: the far range is the full draw distance again.
    void onFarFull();
    // AgentUpdate: the part of the draw distance added to the far range per
    // step (upstream 0.1). FAR_STEP only from the halving for an arrival until
    // the full range is back, and not while memory is low at a step: the steps
    // for a raised draw distance, an area search started after the arrival or
    // memory coming back stay upstream's. (An area search running across the
    // arrival comes back by FAR_STEP too, its range being the one halved; memory
    // short only between two steps, a second apart, leaves the arrival's mark.)
    F32 farStepFraction(bool memory_low);
}

#endif // PLUS_LOGINFAST_H

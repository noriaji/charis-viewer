/**
 * @file loginfast.cpp
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

// The conditions follow what research/212 read of Aurora (the curtain on
// AgentMovementComplete, 97% of the land and a short least time); the code is
// this viewer's own.

#include "llviewerprecompiledheaders.h"

#include "loginfast.h"

#include "llagent.h"
#include "llagentwearables.h"
#include "llapp.h"
#include "llcallbacklist.h"
#include "llstartup.h"
#include "llsurface.h"
#include "llsurfacepatch.h"
#include "lltimer.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llvoavatarself.h"

#include <string>
#include <vector>

namespace plus::loginfast
{
namespace
{
    // The curtain lifts when this part of the agent region's land has come.
    constexpr F32 LAND_READY            = 0.97f;
    // Least time the world draws under the login curtain (from STATE_PRECACHE).
    constexpr F32 MIN_LOGIN_SECONDS     = 1.0f;
    // Least time after a teleport's arrival (AgentMovementComplete).
    constexpr F32 MIN_TELEPORT_SECONDS  = 0.4f;
    constexpr F32 FADE_SECONDS          = 0.7f;
    constexpr F32 FADE_FROM_LOGIN_SECONDS = 0.25f;
    constexpr F32 UPSTREAM_FADE_SECONDS = 1.0f;     // llprogressview.cpp FADE_TO_WORLD_TIME
    constexpr F32 FAR_STEP              = 0.5f;
    constexpr F32 UPSTREAM_FAR_STEP     = 0.1f;
    // After STATE_STARTED, the login timeline waits this long for the curtain.
    constexpr F64 CURTAIN_DEADLINE      = 20.0;
    // The two decodeAllImages() before the circuit: each at most (upstream 1 s).
    constexpr F32 DECODE_SECONDS        = 0.1f;
    constexpr F32 UPSTREAM_DECODE_SECONDS = 1.0f;
    // States go on in one frame while the frame has taken less than this.
    constexpr F64 CHAIN_SECONDS         = 0.050;
    // The login's progress screen: presents a second at most.
    constexpr F64 PRESENT_FPS           = 30.0;

    bool                sCircuitEarly = false;  // UseCircuitCode sent early in this attempt
    F64                 sCircuitSentAt = -1.0;  // when, until the circuit is first read; or -1
    bool                sFromLoop = false;      // the next display_startup() is display()'s
    F64                 sFrameBegin = 0.0;
    F64                 sLastPresent = 0.0;

    struct Step
    {
        F64         time;
        std::string text;
    };

    bool timelineOn()
    {
        static LLCachedControl<bool> on(gSavedSettings, "PlusLoginTimeline", true);
        return on;
    }

    // Part of the region's land patches that have had their heights.
    F32 landFraction(const LLViewerRegion* region)
    {
        if (!region)
        {
            return 0.f;
        }
        const LLSurface& land = region->getLand();
        const S32 per_edge = land.getPatchesPerEdge();
        if (per_edge <= 0)
        {
            return 0.f;
        }
        S32 got = 0;
        for (S32 j = 0; j < per_edge; ++j)
        {
            for (S32 i = 0; i < per_edge; ++i)
            {
                const LLSurfacePatch* patch = land.getPatch(i, j);
                if (patch && patch->getHasReceivedData())
                {
                    ++got;
                }
            }
        }
        return (F32)got / (F32)(per_edge * per_edge);
    }

    std::string percent(F32 fraction)
    {
        return std::to_string((int)(fraction * 100.f + 0.5f)) + "%";
    }

    void writeLine(const char* kind, const char* how, F64 start, const std::vector<Step>& steps, const std::string& tail)
    {
        std::string line = kind;
        line += " timeline (";
        line += how;
        line += "): ";
        bool first = true;
        for (const Step& s : steps)
        {
            char t[32];
            snprintf(t, sizeof(t), "%+.3f", s.time - start);
            if (!first)
            {
                line += " | ";
            }
            first = false;
            line += t;
            line += " ";
            line += s.text;
        }
        if (!tail.empty())
        {
            line += " || ";
            line += tail;
        }
        LL_INFOS("LoginTimeline") << line << LL_ENDL;
    }

    // ---- login ----
    bool                sLoginActive = false;
    bool                sLoginIdleOn = false;
    F64                 sLoginStart = 0.0;
    F64                 sStarted = -1.0;        // STATE_STARTED, or -1
    bool                sLoginEarly = false;    // the curtain's condition was met before PrecachingDelay
    std::vector<Step>   sLoginSteps;

    void finishLogin(const char* how)
    {
        std::string tail = "PlusFastLogin ";
        tail += loginOn() ? "on" : "off";
        tail += sLoginEarly ? ", curtain on the land and avatar" : ", curtain on the upstream delays";
        writeLine("login", how, sLoginStart, sLoginSteps, tail);
        sLoginActive = false;
        sStarted = -1.0;
        sLoginSteps.clear();
    }

    // After STATE_STARTED, until the curtain has gone (or the deadline).
    bool onLoginIdle()
    {
        if (!sLoginActive || sStarted < 0.0)
        {
            sLoginIdleOn = false;
            return true;
        }
        if (LLTimer::getTotalSeconds() - sStarted >= CURTAIN_DEADLINE)
        {
            finishLogin("curtain not seen to go");
            sLoginIdleOn = false;
            return true;
        }
        return false;
    }

    // ---- teleport ----
    bool                sTpActive = false;
    S32                 sTpLast = LLAgent::TELEPORT_NONE;
    F64                 sTpStart = 0.0;
    F64                 sTpArrival = -1.0;      // TELEPORT_START_ARRIVAL seen, or -1
    F32                 sTpLand = -1.f;         // land at the curtain's end, or -1
    bool                sTpEarly = false;
    std::vector<Step>   sTpSteps;

    // ---- far range ----
    bool                sFarArrival = false;    // halved for an arrival, not full again yet

    void finishTeleport(const char* how)
    {
        std::string tail = "PlusFastTeleport ";
        tail += teleportOn() ? "on" : "off";
        if (sTpArrival >= 0.0)
        {
            const F64 now = LLTimer::getTotalSeconds();
            char t[96];
            snprintf(t, sizeof(t), ", curtain %.3f s after the arrival", now - sTpArrival);
            tail += t;
            tail += sTpEarly ? " (on the land)" : " (on TeleportArrivalDelay)";
        }
        if (!teleportScreenShown())
        {
            tail += ", no teleport screen";
        }
        if (sTpLand >= 0.f)
        {
            tail += ", land " + percent(sTpLand);
        }
        writeLine("teleport", how, sTpStart, sTpSteps, tail);
        sTpActive = false;
        sTpArrival = -1.0;
        sTpLand = -1.f;
        sTpEarly = false;
        sTpSteps.clear();
    }
}

bool loginOn()
{
    static LLCachedControl<bool> on(gSavedSettings, "PlusFastLogin", true);
    return on;
}

bool teleportOn()
{
    static LLCachedControl<bool> on(gSavedSettings, "PlusFastTeleport", true);
    return on;
}

void onStartupState(S32 from, S32 to, const std::string& to_name)
{
    if (to == STATE_LOGIN_CLEANUP || to <= STATE_LOGIN_WAIT)
    {
        // a login attempt begins, or has ended: nothing sent early yet
        sCircuitEarly = false;
        sCircuitSentAt = -1.0;
    }
    if (!timelineOn())
    {
        sLoginActive = false;
        sLoginSteps.clear();
        return;
    }
    const F64 now = LLTimer::getTotalSeconds();
    if (to == STATE_LOGIN_CLEANUP)
    {
        // the login button (or an automatic login): a timeline begins
        if (sLoginActive)
        {
            finishLogin("a new login began");
        }
        sLoginActive = true;
        sLoginStart = now;
        sStarted = -1.0;
        sLoginEarly = false;
        sLoginSteps.clear();
        sLoginSteps.reserve(32);
        sLoginSteps.push_back({now, to_name});
        return;
    }
    if (!sLoginActive)
    {
        return;
    }
    if (to <= STATE_LOGIN_WAIT && from > STATE_LOGIN_WAIT)
    {
        // a failed login (or a cancel): back to the login screen
        sLoginSteps.push_back({now, to_name});
        finishLogin("back to the login screen");
        return;
    }
    std::string text = to_name;
    if (to == STATE_WEARABLES_WAIT)
    {
        // the curtain's condition: the land and the avatar at the end of STATE_PRECACHE
        text += " [land " + percent(landFraction(gAgent.getRegion())) + ", avatar ";
        text += isAgentAvatarValid() ? "yes]" : "no]";
    }
    else if (to == STATE_CLEANUP)
    {
        text += " [clothes ";
        text += gAgentWearables.areWearablesLoaded() ? "loaded]" : "still loading]";
    }
    sLoginSteps.push_back({now, std::move(text)});
    if (to == STATE_STARTED)
    {
        sStarted = now;
        if (!sLoginIdleOn)
        {
            sLoginIdleOn = true;
            doOnIdleRepeating(onLoginIdle);
        }
    }
}

bool loginCurtainReady(F32 precache_seconds)
{
    if (!loginOn()
        || precache_seconds < MIN_LOGIN_SECONDS
        || gAgent.isFirstLogin()        // initial outfit and gender chooser: upstream's ways
        || !gAgentMovementCompleted
        || !isAgentAvatarValid())
    {
        return false;
    }
    if (landFraction(gAgent.getRegion()) < LAND_READY)
    {
        return false;
    }
    sLoginEarly = true;
    return true;
}

bool skipWearablesWait()
{
    return loginOn() && !gAgent.isFirstLogin() && gAgent.isOutfitChosen();
}

F32 fadeToWorldSeconds()
{
    return loginOn() ? FADE_SECONDS : UPSTREAM_FADE_SECONDS;
}

F32 fadeFromLoginSeconds()
{
    return loginOn() ? FADE_FROM_LOGIN_SECONDS : UPSTREAM_FADE_SECONDS;
}

void onLoginCurtainGone()
{
    if (sLoginActive && sStarted >= 0.0)
    {
        sLoginSteps.push_back({LLTimer::getTotalSeconds(), "curtain gone"});
        finishLogin("curtain gone");
    }
}

void onTeleportFrame(S32 teleport_state)
{
    if (teleport_state == sTpLast)
    {
        return;
    }
    const S32 last = sTpLast;
    sTpLast = teleport_state;
    if (!timelineOn())
    {
        if (sTpActive)
        {
            sTpActive = false;
            sTpSteps.clear();
        }
        return;
    }
    const F64 now = LLTimer::getTotalSeconds();
    const bool beginning = teleport_state == LLAgent::TELEPORT_START
        || teleport_state == LLAgent::TELEPORT_PENDING
        || teleport_state == LLAgent::TELEPORT_LOCAL;
    if (sTpActive && beginning && last == LLAgent::TELEPORT_NONE)
    {
        finishTeleport("no end seen");
    }
    if (!sTpActive)
    {
        if (teleport_state == LLAgent::TELEPORT_NONE)
        {
            return;
        }
        sTpActive = true;
        sTpStart = now;
        sTpArrival = -1.0;
        sTpLand = -1.f;
        sTpEarly = false;
        sTpSteps.clear();
        sTpSteps.reserve(8);
    }
    sTpSteps.push_back({now, LLAgent::teleportStateName(teleport_state)});
    if (teleport_state == LLAgent::TELEPORT_START_ARRIVAL)
    {
        sTpArrival = now;
    }
    else if (teleport_state == LLAgent::TELEPORT_NONE)
    {
        if (sTpArrival >= 0.0)
        {
            sTpLand = landFraction(gAgent.getRegion());
        }
        finishTeleport(last == LLAgent::TELEPORT_ARRIVING ? "curtain gone" : "ended");
    }
}

bool teleportCurtainReady(F32 arrival_seconds)
{
    if (!teleportOn()
        || arrival_seconds < MIN_TELEPORT_SECONDS
        || !isAgentAvatarValid())
    {
        return false;
    }
    if (landFraction(gAgent.getRegion()) < LAND_READY)
    {
        return false;
    }
    sTpEarly = true;
    return true;
}

bool teleportScreenShown()
{
    static LLCachedControl<bool> disabled(gSavedSettings, "PlusDisableTeleportScreens", false);
    return !disabled;
}

void onFarHalved()
{
    sFarArrival = true;
}

void onFarFull()
{
    sFarArrival = false;
}

F32 farStepFraction(bool memory_low)
{
    if (memory_low)
    {
        sFarArrival = false;    // memory coming back always in tenths
    }
    return sFarArrival && teleportOn() ? FAR_STEP : UPSTREAM_FAR_STEP;
}

bool sendCircuitEarly()
{
    static LLCachedControl<bool> early(gSavedSettings, "PlusFastLoginEarlyUCC", false);
    if (sCircuitEarly || !loginOn() || !early)
    {
        return false;
    }
    sCircuitEarly = true;
    sCircuitSentAt = LLTimer::getTotalSeconds();
    if (sLoginActive)
    {
        sLoginSteps.push_back({sCircuitSentAt, "UseCircuitCode sent early"});
    }
    return true;
}

void onCircuitRead()
{
    if (sCircuitSentAt < 0.0)
    {
        return;
    }
    const F64 now = LLTimer::getTotalSeconds();
    if (sLoginActive)
    {
        char t[64];
        snprintf(t, sizeof(t), "circuit first read [UseCircuitCode unread %.3f s]", now - sCircuitSentAt);
        sLoginSteps.push_back({now, t});
    }
    if (timelineOn())
    {
        LL_INFOS("LoginTimeline") << "early UseCircuitCode: " << (F32)(now - sCircuitSentAt)
                                  << " s from its sending to the circuit's first read" << LL_ENDL;
    }
    sCircuitSentAt = -1.0;
}

void presentFromLoop()
{
    sFromLoop = true;
}

bool circuitSentEarly()
{
    return sCircuitEarly;
}

F32 decodeSeconds()
{
    return loginOn() ? DECODE_SECONDS : UPSTREAM_DECODE_SECONDS;
}

void startupFrameBegin()
{
    sFrameBegin = LLTimer::getTotalSeconds();
}

bool chain(S32 next)
{
    if (!loginOn() || LLStartUp::getStartupState() != next || LLApp::isExiting())
    {
        return false;
    }
    const F64 now = LLTimer::getTotalSeconds();
    return now - sFrameBegin < CHAIN_SECONDS;
}

bool skipStartupPresent()
{
    if (!loginOn())
    {
        return false;
    }
    const bool from_loop = sFromLoop;
    sFromLoop = false;
    const EStartupState state = LLStartUp::getStartupState();
    if (from_loop)
    {
        sLastPresent = LLTimer::getTotalSeconds();
        return false;   // the main loop's own: it keeps the loop to the display's pace
    }
    if (state <= STATE_LOGIN_WAIT || state >= STATE_STARTED)
    {
        return false;   // the login screen itself, and the world, as upstream
    }
    const F64 now = LLTimer::getTotalSeconds();
    if (now - sLastPresent < 1.0 / PRESENT_FPS && now >= sLastPresent)
    {
        return true;
    }
    sLastPresent = now;
    return false;
}
}

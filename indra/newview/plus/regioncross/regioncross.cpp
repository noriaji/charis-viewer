/**
 * @file regioncross.cpp
 * @brief Region crossing timeline: when each step of a crossing happens (log only).
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

// The idea (a timeline line per crossing, with the link events before it) is the one Aurora's
// transition log has; this is written from the description in research/218, not from its code.

#include "llviewerprecompiledheaders.h"

#include "regioncross.h"

#include "llcallbacklist.h"
#include "llhost.h"
#include "llregionhandle.h"
#include "lltimer.h"
#include "llviewerregion.h"
#include "llworld.h"

#include <deque>
#include <string>
#include <vector>

namespace plus::regioncross
{
namespace
{
    struct Step
    {
        F64         time;   // LLTimer::getTotalSeconds(); shown relative to the crossing's start
        std::string text;
    };

    // After the agent moves again, the frames are counted this long.
    constexpr F64 SETTLE_SECONDS    = 5.0;
    // A crossing that never arrives is written (and dropped) after this long.
    constexpr F64 DEADLINE_SECONDS  = 30.0;
    // Neighbour links older than this are not part of a crossing's timeline.
    constexpr F64 PRE_SECONDS       = 60.0;
    constexpr size_t PRE_MAX        = 24;
    constexpr F64 SLOW_FRAME        = 0.050;

    bool                sActive = false;
    bool                sIdleOn = false;
    F64                 sStart = 0.0;
    F64                 sArrived = -1.0;
    F64                 sLastFrame = 0.0;
    S32                 sSlowFrames = 0;
    F64                 sWorstFrame = 0.0;
    std::vector<Step>   sSteps;
    std::deque<Step>    sPre;

    std::string regionName(U64 handle)
    {
        if (!handle)
        {
            return std::string();
        }
        U32 x = 0;
        U32 y = 0;
        from_region_handle(handle, &x, &y);
        return " (" + std::to_string(x / 256) + "," + std::to_string(y / 256) + ")";
    }

    void finish(const char* how)
    {
        std::string line = "crossing timeline (";
        line += how;
        line += "): ";
        bool first = true;
        auto add = [&](const Step& s)
        {
            char t[32];
            snprintf(t, sizeof(t), "%+.3f", s.time - sStart);
            if (!first)
            {
                line += " | ";
            }
            first = false;
            line += t;
            line += " ";
            line += s.text;
        };
        for (const Step& s : sPre)
        {
            if (s.time >= sStart - PRE_SECONDS && s.time < sStart)
            {
                add(s);
            }
        }
        for (const Step& s : sSteps)
        {
            add(s);
        }
        char tail[96];
        snprintf(tail, sizeof(tail), " || frames over 50 ms after arrival: %d, worst %.0f ms",
                 (int)sSlowFrames, sWorstFrame * 1000.0);
        line += tail;
        LL_INFOS("RegionCross") << line << LL_ENDL;
        sActive = false;
        sSteps.clear();
        sPre.clear();
    }

    // Per idle while a crossing is recorded; removes itself (true) when it is done.
    bool onIdle()
    {
        if (!sActive)
        {
            sIdleOn = false;
            return true;
        }
        const F64 now = LLTimer::getTotalSeconds();
        if (sArrived >= 0.0)
        {
            const F64 dt = now - sLastFrame;
            if (dt > SLOW_FRAME)
            {
                ++sSlowFrames;
                if (dt > sWorstFrame)
                {
                    sWorstFrame = dt;
                }
            }
        }
        sLastFrame = now;
        if (sArrived >= 0.0 && now - sArrived >= SETTLE_SECONDS)
        {
            finish("settled");
        }
        else if (now - sStart >= DEADLINE_SECONDS)
        {
            finish(sArrived >= 0.0 ? "settled" : "no AgentMovementComplete within 30 s");
        }
        if (!sActive)
        {
            sIdleOn = false;
            return true;
        }
        return false;
    }
}

void begin(U64 region_handle)
{
    if (sActive)
    {
        finish("a new crossing began");
    }
    sActive = true;
    sStart = LLTimer::getTotalSeconds();
    sArrived = -1.0;
    sLastFrame = sStart;
    sSlowFrames = 0;
    sWorstFrame = 0.0;
    sSteps.clear();
    sSteps.push_back({sStart, "CrossedRegion" + regionName(region_handle)});
    if (!sIdleOn)
    {
        sIdleOn = true;
        doOnIdleRepeating(onIdle);
    }
}

void step(const char* what, U64 region_handle)
{
    Step s{LLTimer::getTotalSeconds(), std::string(what) + regionName(region_handle)};
    if (sActive)
    {
        sSteps.push_back(std::move(s));
    }
    else
    {
        // a neighbour's link; kept for the next crossing
        sPre.push_back(std::move(s));
        while (sPre.size() > PRE_MAX)
        {
            sPre.pop_front();
        }
    }
}

void stepHost(const char* what, const LLHost& host)
{
    LLViewerRegion* region = LLWorld::instanceExists() ? LLWorld::getInstance()->getRegion(host) : nullptr;
    step(what, region ? region->getHandle() : 0);
}

void arrived(U64 region_handle)
{
    if (!sActive || sArrived >= 0.0)
    {
        return;
    }
    step("AgentMovementComplete", region_handle);
    sArrived = LLTimer::getTotalSeconds();
}
}

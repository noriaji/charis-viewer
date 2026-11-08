/**
 * @file idlecensus.cpp
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

#include "llviewerprecompiledheaders.h"

#include "idlecensus.h"
#include "vparammemo.h"

#include "llthread.h"           // on_main_thread
#include "llviewerobject.h"
#include "llvoavatar.h"

#include <chrono>
#include <sstream>

namespace
{
    using plus::idlecensus::Step;
    using plus::idlecensus::STEP_COUNT;

    constexpr F64 REPORT_SECONDS = 5.0;

    const char* const STEP_NAMES[STEP_COUNT] =
    {
        "deselect", "gestures and agent", "object list update", "gltf",
        "setup", "avatar prepare", "avatars", "animesh", "volumes", "other objects",
        "flexible", "texture animations", "avatar finish", "idle skip end", "object costs",
        "complexity", "frame stats",
        "stepped", "skipped", "update drawable",
        "too slow", "texture loading", "character", "motions", "misc", "appearance animation",
        "loading effect", "name tag", "render complexity", "visualParamWeightsAreDefault",
        "updateMove async", "updateMove moved list",
    };

    struct Window
    {
        U64 ticks[STEP_COUNT] = {};
        U64 calls[STEP_COUNT] = {};
        U64 frames = 0;
        U64 avatars = 0;            // LLCharacter::sInstances, summed a frame
    };
    Window sWindow;
    bool sWindowOpen = false;
    U64 sWindowTicks = 0;           // __rdtsc at the window's start
    S64 sWindowNanoseconds = 0;     // the steady clock at the window's start
    F64 sScopeTicks = -1.0;         // what one step costs itself (measured once)

    S64 now_nanoseconds()
    {
        return (S64)std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // The cost of one step (two TSC reads and the add), measured once.
    F64 measure_scope_ticks()
    {
        constexpr U32 N = 1000;
        Window saved = sWindow;
        const U64 start = __rdtsc();
        for (U32 i = 0; i < N; ++i)
        {
            plus::idlecensus::Scope scope(plus::idlecensus::DESELECT);
        }
        const U64 ticks = __rdtsc() - start;
        sWindow = saved;
        return (F64)ticks / N;
    }

    void open_window()
    {
        sWindow = Window();
        sWindowTicks = __rdtsc();
        sWindowNanoseconds = now_nanoseconds();
        sWindowOpen = true;
    }

    void report()
    {
        const U64 ticks_now = __rdtsc();
        const S64 ns_now = now_nanoseconds();
        const F64 ns_per_tick = ticks_now > sWindowTicks ? (F64)(ns_now - sWindowNanoseconds) / (F64)(ticks_now - sWindowTicks) : 0.0;
        const Window& w = sWindow;
        const F64 frames = (F64)llmax(w.frames, (U64)1);
        auto us = [&](Step s) { return (F64)w.ticks[s] * ns_per_tick / 1000.0 / frames; };
        auto calls = [&](Step s) { return (F64)w.calls[s] / frames; };

        std::ostringstream out;
        out.setf(std::ios::fixed);
        out.precision(1);
        auto item = [&](Step s, const char* sep)
        {
            out << sep << STEP_NAMES[s] << " " << us(s) << " (" << calls(s) << ")";
        };
        out << "idle census over " << w.frames << " frames, us a frame (calls a frame): idle objects "
            << us(plus::idlecensus::DESELECT) + us(plus::idlecensus::AGENT) + us(plus::idlecensus::OBJECT_LIST)
                   + us(plus::idlecensus::GLTF);
        item(plus::idlecensus::DESELECT, " = ");
        item(plus::idlecensus::AGENT, " + ");
        item(plus::idlecensus::OBJECT_LIST, " + ");
        item(plus::idlecensus::GLTF, " + ");
        out << "; object list update:";
        item(plus::idlecensus::LIST_SETUP, " ");
        item(plus::idlecensus::AVATAR_PREPARE, ", ");
        item(plus::idlecensus::IDLE_AVATAR, ", idle updates: ");
        item(plus::idlecensus::IDLE_ANIMESH, ", ");
        item(plus::idlecensus::IDLE_VOLUME, ", ");
        item(plus::idlecensus::IDLE_OTHER, ", ");
        item(plus::idlecensus::FLEXIBLE, "; ");
        item(plus::idlecensus::TEXANIM, ", ");
        item(plus::idlecensus::AVATAR_FINISH, ", ");
        item(plus::idlecensus::IDLESKIP_END, ", ");
        item(plus::idlecensus::OBJECT_COSTS, ", ");
        item(plus::idlecensus::COMPLEXITY, ", ");
        item(plus::idlecensus::FRAME_STATS, ", ");
        out << "; texture animations (animateTextures calls):";
        item(plus::idlecensus::TEXANIM_STEP, " ");
        item(plus::idlecensus::TEXANIM_SKIP, ", ");
        out << "; in the idle updates:";
        item(plus::idlecensus::UPDATE_DRAWABLE, " ");
        out << "; avatars (" << (F64)w.avatars / frames << "):";
        item(plus::idlecensus::AV_TOO_SLOW, " ");
        item(plus::idlecensus::AV_TEXTURE_LOADING, ", ");
        item(plus::idlecensus::AV_CHARACTER, ", ");
        item(plus::idlecensus::AV_MOTIONS, " of which ");
        item(plus::idlecensus::AV_MISC, ", ");
        item(plus::idlecensus::AV_APPEARANCE, ", ");
        item(plus::idlecensus::AV_LOADING, ", ");
        item(plus::idlecensus::AV_NAME_TAG, ", ");
        item(plus::idlecensus::AV_COMPLEXITY, ", ");
        item(plus::idlecensus::AV_VPARAM_DEFAULT, ", ");
        item(plus::idlecensus::MOVE_ASYNC, ", ");
        out << "; idle move:";
        item(plus::idlecensus::MOVE_LIST, " ");
        U64 steps = 0;
        for (U32 s = 0; s < STEP_COUNT; ++s)
        {
            steps += w.calls[s];
        }
        const F64 scope_ticks = llmax(sScopeTicks, 0.0);
        out << "; census cost about " << (F64)steps / frames * scope_ticks * ns_per_tick / 1000.0 << " us a frame ("
            << (F64)steps / frames << " steps, " << scope_ticks * ns_per_tick << " ns a step)";

        const plus::vparammemo::Counts v = plus::vparammemo::takeCounts();
        out << "; vparam memo ["
            << (LLRHI::sVParamMemoVerify ? (LLRHI::sVParamMemoAuto ? "auto verify" : "verify") : LLRHI::sVParamMemo ? "on" : "off")
            << "] calls " << (F64)v.calls / frames << ", witness " << (F64)v.witness_hits / frames << ", walks "
            << (F64)v.walks / frames << " (all default " << (F64)v.default_answers / frames << "), lists " << v.listed
            << ", verify checked " << v.checked << ", mismatches " << v.mismatches;
        LL_INFOS("VKPerf") << out.str() << LL_ENDL;
    }

    // The memo's line without the census, while it verifies (explicitly).
    void report_memo_only(U64 frames)
    {
        const plus::vparammemo::Counts v = plus::vparammemo::takeCounts();
        LL_INFOS("VKPerf") << "vparam memo [verify] over " << frames << " frames: calls " << v.calls << ", witness "
                           << v.witness_hits << ", walks " << v.walks << " (all default " << v.default_answers
                           << "), lists " << v.listed << ", checked " << v.checked << ", mismatches " << v.mismatches
                           << LL_ENDL;
    }
}

namespace plus::idlecensus
{
    void add(Step step, U64 ticks)
    {
        if (!on_main_thread())
        {
            return;
        }
        sWindow.ticks[step] += ticks;
        ++sWindow.calls[step];
    }

    void ObjectScope::start(LLViewerObject* objectp)
    {
        if (objectp->isAvatar())
        {
            const LLVOAvatar* avatar = objectp->asAvatar();
            mStep = avatar && avatar->isControlAvatar() ? IDLE_ANIMESH : IDLE_AVATAR;
        }
        else
        {
            mStep = objectp->getPCode() == LL_PCODE_VOLUME ? IDLE_VOLUME : IDLE_OTHER;
        }
        mStart = __rdtsc();
    }

    void endFrame()
    {
        const bool memo_verify_only = LLRHI::sVParamMemoVerify && !LLRHI::sVParamMemoAuto;
        if (!LLRHI::sIdleCensus && !memo_verify_only)
        {
            if (sWindowOpen)
            {
                sWindowOpen = false;
                plus::vparammemo::takeCounts();
            }
            return;
        }
        if (!sWindowOpen)
        {
            if (sScopeTicks < 0.0 && LLRHI::sIdleCensus)
            {
                sScopeTicks = measure_scope_ticks();
            }
            plus::vparammemo::takeCounts();
            open_window();
            return;
        }
        ++sWindow.frames;
        sWindow.avatars += LLCharacter::sInstances.size();
        if ((F64)(now_nanoseconds() - sWindowNanoseconds) < REPORT_SECONDS * 1e9)
        {
            return;
        }
        if (LLRHI::sIdleCensus)
        {
            report();
        }
        else
        {
            report_memo_only(sWindow.frames);
        }
        open_window();
    }
}

/**
 * @file skinonce.cpp
 * @brief Skin once, part (a): an avatar's skinning palette is asked of the
 *        backend once per build, not once per pass and draw
 *        (LLRHI_SKIN_ONCE / PlusSkinOnce, research/133-skin-once.md in the
 *        sl-viewer repository).
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

// Why the pixels cannot change (see skinonce.h): a kept row is the very row
// the backend would hand back (its frame check "row.frame == sFrame" answers
// with the row as it is), and when the backend's frame moved on since, the
// readers fall back to the joints themselves (the same values). The palette
// table, its rows and every value in them are as with this off.
//
// Part (b) of research/133 (skinning once in a compute pass, the passes
// reading skinned vertices) is a design only; nothing of it is here.

#include "llviewerprecompiledheaders.h"

#include "skinonce.h"

#include "llappviewer.h"    // gFrameCount
#include "llcontrol.h"
#include "llframetimer.h"
#include "llviewercontrol.h"

#include <cstdlib>
#include <cstring>
#include <sstream>

namespace
{
    constexpr char SETTING_ENABLED[] = "PlusSkinOnce";
    constexpr char ENV_ENABLED[] = "LLRHI_SKIN_ONCE";   // 1: on, 0: off, whatever PlusSkinOnce says

    constexpr F64 LOG_PERIOD_SECONDS = 5.0;

    // -1: no environment override, 0: off, 1: on.
    S32 envOverride()
    {
        const char* env = std::getenv(ENV_ENABLED);
        if (!env || !*env)
        {
            return -1;
        }
        return std::strcmp(env, "0") ? 1 : 0;
    }

    // LLRHI_AVATAR_STATS only (LLRHI::sAvatarStats): the rows kept here and
    // the ones asked of the backend, and an "RHISkinOnce" line every 5 s.
    struct Stats
    {
        U64 kept = 0;
        U64 asked = 0;
        U32 first_frame = 0;
        F64 next_log = 0.0;
    };
    Stats sStats;

    void note(bool kept)
    {
        Stats& st = sStats;
        const F64 now = LLFrameTimer::getTotalSeconds();
        if (st.next_log == 0.0)
        {
            st.first_frame = gFrameCount;
            st.next_log = now + LOG_PERIOD_SECONDS;
        }
        if (kept)
        {
            ++st.kept;
        }
        else
        {
            ++st.asked;
        }
        if (now < st.next_log)
        {
            return;
        }
        const U32 frames = gFrameCount > st.first_frame ? gFrameCount - st.first_frame : 1;
        const F64 per = 1.0 / frames;
        std::ostringstream line;
        line.setf(std::ios::fixed);
        line.precision(1);
        line << "skin once (LLRHI_SKIN_ONCE) " << (plus::skinonce::enabled() ? "on" : "off") << " over " << frames
             << " frames: palette rows kept on the viewer side " << st.kept * per << " per frame, asked of the backend "
             << st.asked * per << " (the uploads and the rigged native shadow packets)";
        LL_INFOS("RHISkinOnce") << line.str() << LL_ENDL;
        st.kept = 0;
        st.asked = 0;
        st.first_frame = gFrameCount;
        st.next_log = now + LOG_PERIOD_SECONDS;
    }
}

namespace plus::skinonce
{
    bool enabled()
    {
        static const S32 env = envOverride();
        if (env >= 0)
        {
            return env != 0;
        }
        static LLCachedControl<bool> setting(gSavedSettings, SETTING_ENABLED, false);
        return setting();
    }

    bool keepRow(const LLRHI::PaletteRow& row)
    {
        // The frame first: a row not asked for yet this build costs no more than before.
        const bool keep = row.frame != U32_MAX && enabled();
        if (LLRHI::sAvatarStats)
        {
            note(keep);
        }
        return keep;
    }
}

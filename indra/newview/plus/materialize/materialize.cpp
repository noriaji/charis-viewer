/**
 * @file materialize.cpp
 * @brief The materialize band's settings, colours and count (see materialize.h).
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

#include "materialize.h"

#include <algorithm>
#include <iterator>
#include <sstream>

#include "llappviewer.h"        // gFrameCount
#include "llimagegl.h"          // research/278: LLImageGL::sBandTime, the bands' clock
#include "llrhi.h"              // research/289: LLRHI::sTextureImageFailures, sTargetImageFailures
#include "llviewercontrol.h"
#include "plus.h"               // effectiveExposure, isNoPost, isClassicSky

namespace
{
    // research/256 6b: the same four as research/257 derez (sRGB), one table for both.
    using plus::objectfx::PALETTE;

    // PlusMaterializeSeconds' default (settings_plus.xml), for a value that is not a number.
    constexpr F32 DEFAULT_SECONDS = 1.2f;

    // The edge's strength on screen under an HDR sky (over 1: the bloom takes
    // it), divided by the exposure the tone mapping applies.
    constexpr F32 EDGE_STRENGTH = 3.f;
    constexpr F32 EDGE_STRENGTH_MIN = 0.5f;
    constexpr F32 EDGE_STRENGTH_MAX = 8.f;
    // Under the "no post" tone mapping every channel is clamped to 1, and there
    // is no bloom: the edge stays a colour, not a white fill. A classic sky
    // scales it by softenLightF.glsl's final 1.1 too (its 1.35 is the
    // sunlight's only, plus::CLASSIC_SUNLIGHT_SCALE: the edge is unlit), so
    // under 1 / 1.1 there (the shader branch's handoff, research/256).
    constexpr F32 LDR_CAP = 0.9f;

    S32 palette()
    {
        static LLCachedControl<S32> palette(gSavedSettings, "PlusMaterializePalette", 0);
        return llclamp((S32)palette, 0, plus::materialize::PALETTE_CUSTOM);
    }

    // The end of the last band admitted (anyBand).
    F32 sLastEnd = 0.f;

    // At most N bands under way: their ends, oldest first. The ends come in
    // order while PlusMaterializeSeconds stays the same; a change of it only
    // makes the count a little high for one band's time.
    template <U32 N>
    class BandCount
    {
    public:
        bool admit(F32 now, F32 until)
        {
            while (mCount && mEnds[mFirst] <= now)
            {
                mFirst = (mFirst + 1) % N;
                --mCount;
            }
            if (mCount >= N)
            {
                return false;
            }
            mEnds[(mFirst + mCount) % N] = until;
            ++mCount;
            sLastEnd = llmax(sLastEnd, until);
            return true;
        }

    private:
        F32 mEnds[N] = {};
        U32 mFirst = 0;
        U32 mCount = 0;
    };

    BandCount<plus::materialize::MAX_BANDS> sTextureBands;
    BandCount<plus::materialize::MAX_AVATAR_BANDS> sAvatarBands;   // research/259

    // research/278: the frame time the clock was moved to last (< 0: never), and the clock in full (summed in
    // a double: steps of a few ms added to a float of hours would round to nothing or to twice as much).
    F32 sTickedAt = -1.f;
    F64 sBandClock = 0.0;
    // review L1: the recent frame interval (an average over about 1 / FRAME_AVERAGE frames).
    F32 sFrameInterval = 1.f / 60.f;
    constexpr F32 FRAME_AVERAGE = 0.05f;

    // research/278: PlusMaterializeLog's counts since its last line, the unbanded draws by render pass (a
    // pass is a U8 in LLDrawInfo::mBandPass), research/289: and the gray stand-in's (each in its table), and
    // when that line was (frame time).
    U32 sTallies[plus::materialize::TALLY_COUNT] = {};
    constexpr U32 PASSES = 256;
    enum PassTable : U8 { PASS_UNBANDED, PASS_GIVEN_UP, PASS_GIVEN_UP_IDLE, PASS_MISSING, PASS_TABLES };
    const char* const PASS_TABLE_NAMES[PASS_TABLES] = { "unbanded", "given_up", "given_up_idle", "missing" };
    U32 sPasses[PASS_TABLES][PASSES] = {};
    F32 sLoggedAt = -1.f;

    // The table a tally counted by pass goes in (tallyPass: DRAWN_NONE and DRAWN_ALPHA, GRAY_*).
    PassTable passTable(plus::materialize::Tally what)
    {
        switch (what)
        {
        case plus::materialize::GRAY_GIVEN_UP:
            return PASS_GIVEN_UP;
        case plus::materialize::GRAY_GIVEN_UP_IDLE:
            return PASS_GIVEN_UP_IDLE;
        case plus::materialize::GRAY_MISSING:
            return PASS_MISSING;
        default:
            return PASS_UNBANDED;
        }
    }

    // The names in the line, in Tally's order, and the group each one opens (nullptr: the same group).
    struct TallyName
    {
        const char* group;
        const char* name;
    };
    const TallyName TALLY_NAMES[] = {
        { "came", "unwaited" }, { nullptr, "no_image" }, { nullptr, "off" }, { nullptr, "wireframe" },
        { nullptr, "not_shown" }, { nullptr, "given_up" }, { nullptr, "quick" }, { nullptr, "curtain" },
        { nullptr, "full" }, { nullptr, "band" },
        { "mask", "wait" }, { nullptr, "late" }, { nullptr, "same" }, { nullptr, "changed" }, { nullptr, "groups" },
        { nullptr, "again" }, { nullptr, "full" }, { nullptr, "gave_up" },    // research/284, 289
        { "avatar", "not_allowed" }, { nullptr, "curtain" }, { nullptr, "not_seen" }, { nullptr, "no_skeleton" },
        { nullptr, "full" }, { nullptr, "band" }, { nullptr, "kept" },
        { "drawn", "sweep" }, { nullptr, "flash" }, { nullptr, "flat" }, { nullptr, "lost" }, { nullptr, "odd" },
        { nullptr, "worn" }, { nullptr, "worn_own" }, { nullptr, "body" },
        { nullptr, "no_program" }, { nullptr, "none" }, { nullptr, "alpha" },
        { "gray", "given_up" }, { nullptr, "given_up_idle" }, { nullptr, "missing" },  // research/289
        { nullptr, "image_failed" }, { nullptr, "target_failed" },
    };
    static_assert(std::size(TALLY_NAMES) == plus::materialize::TALLY_COUNT, "a name for each Tally");

    void clearTallies()
    {
        std::fill(std::begin(sTallies), std::end(sTallies), 0u);
        for (U32 (&table)[PASSES] : sPasses)
        {
            std::fill(std::begin(table), std::end(table), 0u);
        }
    }

    // The line of the counts since the last one, when there is any; the counts back to 0.
    void writeLog(F32 seconds)
    {
        bool any = false;
        for (const U32 count : sTallies)
        {
            any = any || count != 0;
        }
        if (!any)
        {
            return;
        }
        std::ostringstream line;
        line.setf(std::ios::fixed);
        line.precision(1);
        line << seconds << "s:";
        for (U32 i = 0; i < plus::materialize::TALLY_COUNT; ++i)
        {
            if (TALLY_NAMES[i].group)
            {
                line << (i ? "; " : " ") << TALLY_NAMES[i].group;
            }
            line << ' ' << TALLY_NAMES[i].name << '=' << sTallies[i];
        }
        for (U32 table = 0; table < PASS_TABLES; ++table)
        {
            line << "; " << PASS_TABLE_NAMES[table] << " by pass:";
            for (U32 pass = 0; pass < PASSES; ++pass)
            {
                if (sPasses[table][pass])
                {
                    line << ' ' << pass << '=' << sPasses[table][pass];
                }
            }
        }
        LL_INFOS("Materialize") << line.str() << LL_ENDL;
        clearTallies();
    }

    // research/289 (the integration check's L10): a count of the backend's failed tries to make an image since
    // the last call (`seen`: the count then; it only grows, and stays 0 without a backend that counts).
    U32 newFailures(const std::atomic<U32>& count, U32& seen)
    {
        const U32 now = count.load(std::memory_order_relaxed);
        const U32 failures = now - seen;
        seen = now;
        return failures;
    }

    // Into the tallies (`count`: false, only to forget those from before).
    void tallyImageFailures(bool count)
    {
        static U32 seen_textures = 0;
        static U32 seen_targets = 0;
        const U32 textures = newFailures(LLRHI::sTextureImageFailures, seen_textures);
        const U32 targets = newFailures(LLRHI::sTargetImageFailures, seen_targets);
        if (count)
        {
            sTallies[plus::materialize::GRAY_IMAGE_FAILED] += textures;
            sTallies[plus::materialize::GRAY_TARGET_FAILED] += targets;
        }
    }

    // Once a frame (tick): PlusMaterializeLog read, its counts from 0 when it is turned on, its line every
    // LOG_SECONDS while it stays on.
    void updateLog(F32 frame_time)
    {
        static LLCachedControl<bool> log(gSavedSettings, "PlusMaterializeLog", false);
        const bool on = log;
        if (on && !plus::materialize::sLogOn)
        {
            clearTallies();
            tallyImageFailures(false);  // not those from before
            sLoggedAt = frame_time;
        }
        plus::materialize::sLogOn = on;
        if (on)
        {
            tallyImageFailures(true);
        }
        if (on && frame_time - sLoggedAt >= plus::materialize::LOG_SECONDS)
        {
            writeLog(frame_time - sLoggedAt);
            sLoggedAt = frame_time;
        }
    }
}

namespace plus::materialize
{
    bool on()
    {
        static LLCachedControl<bool> on(gSavedSettings, "PlusMaterialize", true);
        return on;
    }

    F32 seconds()
    {
        static LLCachedControl<F32> seconds(gSavedSettings, "PlusMaterializeSeconds", DEFAULT_SECONDS);
        const F32 value = seconds;
        // Third review 2: NaN passes llclamp (and would fill the count of bands with ends that never come).
        return llfinite(value) ? llclamp(value, 0.4f, 4.f) : DEFAULT_SECONDS;
    }

    LLColor3 colour()
    {
        static LLCachedControl<LLColor3> custom(gSavedSettings, "PlusMaterializeColor", PALETTE[0]);
        return plus::objectfx::paletteColour(palette(), custom);
    }

    U32 lookKey()
    {
        if (!on())
        {
            return 1;
        }
        const LLColor3 c = colour();
        return 2 | ((U32)ll_round(c.mV[VRED] * 255.f) << 8) | ((U32)ll_round(c.mV[VGREEN] * 255.f) << 16) |
               ((U32)ll_round(c.mV[VBLUE] * 255.f) << 24);
    }

    bool admit(F32 now, F32 until)
    {
        return sTextureBands.admit(now, until);
    }

    bool avatarOn()
    {
        static LLCachedControl<bool> avatars(gSavedSettings, "PlusMaterializeAvatar", true);
        return avatars && on();
    }

    bool admitAvatar(F32 now, F32 until)
    {
        return sAvatarBands.admit(now, until);
    }

    bool anyBand(F32 now)
    {
        return sLastEnd > now && on();
    }

    F32 bandClock()
    {
        return LLImageGL::sBandTime;
    }

    void tick(F32 frame_time)
    {
        if (sTickedAt >= 0.f)
        {
            const F32 step = frame_time - sTickedAt;
            if (step > 0.f)     // not NaN, nor a clock that went back
            {
                sBandClock += llmin(step, llmax(2.f * sFrameInterval, BAND_STEP_MIN));
                LLImageGL::sBandTime = (F32)sBandClock;
                sFrameInterval += (llmin(step, FRAME_TYPICAL_MAX) - sFrameInterval) * FRAME_AVERAGE;
            }
        }
        sTickedAt = frame_time;
        updateLog(frame_time);
    }

    bool sLogOn = false;

    void addTally(Tally what, U32 pass)
    {
        if (what < TALLY_COUNT)
        {
            ++sTallies[what];
        }
        if (pass < PASSES)
        {
            ++sPasses[passTable(what)][pass];
        }
    }

    const Edge& edge()
    {
        static Edge edge;
        static U32 frame = 0;
        if (frame == gFrameCount && frame)
        {
            return edge;
        }
        frame = gFrameCount;
        const LLColor3 linear = linearColor3(colour());
        edge.colour[0] = linear.mV[VRED];
        edge.colour[1] = linear.mV[VGREEN];
        edge.colour[2] = linear.mV[VBLUE];
        edge.colour[3] = 1.f;
        // research/256 section 5, research/149 (B): the three skies. HDR: the
        // same on screen whatever the exposure; no post and classic: under 1
        // after the classic sky's final scale.
        F32 strength = llclamp(EDGE_STRENGTH / plus::effectiveExposure(), EDGE_STRENGTH_MIN, EDGE_STRENGTH_MAX);
        if (plus::isNoPost() || plus::isClassicSky())
        {
            strength = llmin(strength, LDR_CAP);
        }
        edge.strength = strength;
        return edge;
    }
}

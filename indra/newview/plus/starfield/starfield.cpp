/**
 * @file starfield.cpp
 * @brief The night sky's stars, and the Milky Way as an option (research/176-star-field.md).
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

#include "starfield.h"
#include "starfieldmath.h"

#include "plus/freeze/freeze.h"

#include "llrhi.h"
#include "llviewercontrol.h"

#include <cmath>

extern bool gSnapshot;  // llviewerdisplay.cpp: a snapshot is being drawn

namespace plus::starfield
{
    namespace
    {
        constexpr char SETTING_MODE[] = "PlusSkyStars";
        constexpr char SETTING_GAIN[] = "PlusSkyStarGain";
        constexpr char SETTING_MILKY_WAY_GAIN[] = "PlusSkyStarsMilkyWayGain";
        constexpr F32 DEFAULT_MILKY_WAY_GAIN = 0.25f; // settings_plus.xml (fix-r4: was 0.6)
        constexpr F32 MAX_MILKY_WAY_GAIN = 2.f;
        constexpr char SETTING_DENSE[] = "PlusSkyStarsDense";
        constexpr char SETTING_MILKY_WAY_HQ[] = "PlusSkyStarsMilkyWayHQ";
        constexpr char SETTING_TWINKLE[] = "PlusSkyStarsTwinkle";
        constexpr char SETTING_TWINKLE_HOLD[] = "PlusSkyStarsTwinkleHold";
        constexpr F32 DEFAULT_TWINKLE = 0.6f;       // settings_plus.xml
        constexpr F32 MAX_TWINKLE = 2.f;
        constexpr char TWINKLE_ENV[] = "LLRHI_STAR_TWINKLE";      // "0": no twinkle
        constexpr char SETTING_CONSTELLATION_BOOST[] = "PlusSkyStarsConstellationBoost";
        constexpr F32 DEFAULT_CONSTELLATION_BOOST = 1.f;    // settings_plus.xml
        constexpr F32 MAX_CONSTELLATION_BOOST = 2.f;
        constexpr char CUE_TWINKLE[] = "startwinkle.toggle";      // the twinkle off / on, not saved

        bool sTwinkleCueOff = false;    // startwinkle.toggle turned it off

        bool twinkleAllowed()
        {
            static const bool allowed = LLRHI::envDefaultOn(TWINKLE_ENV);
            return allowed;
        }

        LLVector3 toLL(const math::Vec3& v) { return LLVector3(v.x, v.y, v.z); }
        LLColor4 toLL(const math::Color& c) { return LLColor4(c.r, c.g, c.b, c.a); }
    }

    S32 mode()
    {
        static LLCachedControl<S32> setting(gSavedSettings, SETTING_MODE, MODE_FIELD);
        return llclamp((S32)setting, MODE_OFFICIAL, MODE_MILKY_WAY);
    }

    F32 gain()
    {
        static LLCachedControl<F32> setting(gSavedSettings, SETTING_GAIN, 1.f);
        const F32 value = setting;
        return llfinite(value) ? llclamp(value, 0.25f, 4.f) : 1.f;
    }

    F32 milkyWayGain()
    {
        static LLCachedControl<F32> setting(gSavedSettings, SETTING_MILKY_WAY_GAIN, DEFAULT_MILKY_WAY_GAIN);
        const F32 value = setting;
        return llfinite(value) ? llclamp(value, 0.f, MAX_MILKY_WAY_GAIN) : DEFAULT_MILKY_WAY_GAIN;
    }

    bool dense()
    {
        static LLCachedControl<bool> setting(gSavedSettings, SETTING_DENSE, false);
        return setting;
    }

    bool milkyWayHQ()
    {
        static LLCachedControl<bool> setting(gSavedSettings, SETTING_MILKY_WAY_HQ, false);
        return setting;
    }

    bool bakedMilkyWay(S32 starMode)
    {
        return starMode >= MODE_MILKY_WAY && milkyWayHQ();
    }

    void madeUpPlane(LLVector3& pole, LLVector3& centre)
    {
        math::Vec3 p, c;
        math::madeUpPlane(p, c);
        pole = toLL(p);
        centre = toLL(c);
    }

    F32 twinkle()
    {
        static LLCachedControl<F32> setting(gSavedSettings, SETTING_TWINKLE, DEFAULT_TWINKLE);
        if (sTwinkleCueOff || !twinkleAllowed())
        {
            return 0.f;
        }
        const F32 value = setting;
        return llfinite(value) ? llclamp(value, 0.f, MAX_TWINKLE) : DEFAULT_TWINKLE;
    }

    F32 twinkleTime()
    {
        static LLCachedControl<bool> hold(gSavedSettings, SETTING_TWINKLE_HOLD, false);
        static F32 held = 0.f;
        if (!gSnapshot && !hold())
        {
            held = (F32)std::fmod(plus::freeze::worldSeconds(), (F64)TWINKLE_PERIOD);
        }
        return held;
    }

    F32 constellationBoost()
    {
        static LLCachedControl<F32> setting(gSavedSettings, SETTING_CONSTELLATION_BOOST, DEFAULT_CONSTELLATION_BOOST);
        const F32 value = setting;
        return llfinite(value) ? llclamp(value, 0.f, MAX_CONSTELLATION_BOOST) : DEFAULT_CONSTELLATION_BOOST;
    }

    void boostedColors(const Field& field, F32 boost, std::vector<LLColor4>& colors)
    {
        colors = field.star_color;
        const F32 raise = std::log2(1.f + CONSTELLATION_LIGHT_STEP * llmax(boost, 0.f)) / math::LEVEL_STOPS;
        if (raise <= 0.f)
        {
            return;
        }
        const size_t count = llmin(field.star_mark.size(), colors.size());
        for (size_t i = 0; i < count; ++i)
        {
            if (field.star_mark[i])
            {
                colors[i].mV[VALPHA] = llmin(colors[i].mV[VALPHA] + raise, 1.f);
            }
        }
    }

    void pollCues()
    {
        if (!LLRHI::takeCue(CUE_TWINKLE))
        {
            return;     // no cue (or no LLRHI_CUE_DIR: never a file at the drive's root)
        }
        sTwinkleCueOff = !sTwinkleCueOff;
        LL_INFOS("RHIProfile") << "star twinkle (research/176 section 11): " << (sTwinkleCueOff ? "off" : "on")
                               << (twinkleAllowed() ? "" : " (held off by LLRHI_STAR_TWINKLE=0)") << LL_ENDL;
    }

    void Field::clear()
    {
        star_dir.clear();
        star_color.clear();
        star_mark.clear();
        glow_dir.clear();
        glow_color.clear();
        glow_radius.clear();
        line_a.clear();
        line_b.clear();
    }

    void generate(S32 star_mode, Field& field)
    {
        math::Catalogue catalogue;
        math::generate(star_mode, catalogue);
        if (dense())
        {
            // research/224: faint stars more, along the made-up sky's Milky Way, above the horizon
            math::Vec3 pole, centre;
            math::madeUpPlane(pole, centre);
            math::generateDenseOn(pole, centre, catalogue, false);
        }
        if (bakedMilkyWay(star_mode))
        {
            // the Milky Way is the baked picture (skymilky.h), not patches
            catalogue.glow_dir.clear();
            catalogue.glow_color.clear();
            catalogue.glow_radius.clear();
        }

        field.clear();
        field.star_dir.reserve(catalogue.star_dir.size());
        field.star_color.reserve(catalogue.star_color.size());
        for (size_t i = 0; i < catalogue.star_dir.size(); ++i)
        {
            field.star_dir.push_back(toLL(catalogue.star_dir[i]));
            field.star_color.push_back(toLL(catalogue.star_color[i]));
        }
        field.glow_dir.reserve(catalogue.glow_dir.size());
        field.glow_color.reserve(catalogue.glow_color.size());
        for (size_t i = 0; i < catalogue.glow_dir.size(); ++i)
        {
            field.glow_dir.push_back(toLL(catalogue.glow_dir[i]));
            field.glow_color.push_back(toLL(catalogue.glow_color[i]));
        }
        field.glow_radius = catalogue.glow_radius;
    }
}

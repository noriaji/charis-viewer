/**
 * @file slwx.cpp
 * @brief The parcel's weather as the rain asks for it: the tag's values at
 *        this UTC second, blended over `trans` when the tag changes
 *        (research/229 section 4).
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

#include "plus.h"

#include "slwx.h"

#include "slwxparcel.h"
#include "slwxsync.h"
#include "slwxtag.h"

#include "plus/freeze/freeze.h"
#include "skinsss/skinsss.h"

#include "llcallbacklist.h"
#include "llframetimer.h"
#include "llviewercontrol.h"

#include <cmath>

namespace plus::slwx
{
    namespace
    {
        constexpr char SETTING_ALLOW[] = "PlusWeatherAllowParcel";
        constexpr char SETTING_RAIN[] = "PlusRain";
        constexpr char SETTING_SNOW[] = "PlusSnow";
        // Below this a rate counts as none.
        constexpr F32 NONE = 0.001f;

        // The weather as continuous values, the thing that is blended.
        struct Values
        {
            F32 mm = 0.f;       // mm/h of rain
            F32 cm = 0.f;       // cm/h of snow
            F32 wind_x = 0.f;   // the wind's speed (m/s) along sin(from), cos(from): east, north of where it blows from
            F32 wind_y = 0.f;
            F32 base = 0.f;     // the wind's steady speed
            F32 storm = 0.f;
        };

        Values blend(const Values& a, const Values& b, F32 t)
        {
            Values v;
            v.mm = a.mm + (b.mm - a.mm) * t;
            v.cm = a.cm + (b.cm - a.cm) * t;
            v.wind_x = a.wind_x + (b.wind_x - a.wind_x) * t;
            v.wind_y = a.wind_y + (b.wind_y - a.wind_y) * t;
            v.base = a.base + (b.base - a.base) * t;
            v.storm = a.storm + (b.storm - a.storm) * t;
            return v;
        }

        bool sActive = false;               // the parcel's weather was asked for on the last frame
        U32 sSeen = 0;                      // the parcel target's generation taken up
        Values sFrom;                       // the values when the blend began
        Values sLast;                       // the values given on the last frame
        F64 sBlendStart = 0.0;
        F32 sBlendDuration = 0.f;
        F32 sTrans = tag::TRANS_DEFAULT;    // the last tag's blending time, for the fade out when it goes
        U64 sFrame = ~(U64)0;               // the frame the answer below is for
        bool sAnswer = false;
        Effective sEffective = {};
        bool sRainWasOn = false;

        void leave()
        {
            if (sActive)
            {
                sActive = false;
                plus::slwx::parcel::reset();
                sLast = Values();
                sFrom = Values();
                if (sRainWasOn)
                {
                    sRainWasOn = false;
                    doOnIdleOneTime(&plus::skinsss::markWantChanged);
                }
            }
        }

        bool compute()
        {
            static LLCachedControl<bool> allow(gSavedSettings, SETTING_ALLOW, false);
            static LLCachedControl<S32> own_rain(gSavedSettings, SETTING_RAIN, 0);
            static LLCachedControl<S32> own_snow(gSavedSettings, SETTING_SNOW, 0);
            if (!allow || own_rain != 0 || own_snow != 0)
            {
                leave();
                return false;
            }
            if (!sActive)
            {
                // Taken up from nothing: the parcel's target is read afresh and taken at once.
                sActive = true;
                parcel::reset();
                sSeen = parcel::target().generation - 1;
            }

            parcel::update();
            const parcel::Target& target = parcel::target();
            const F64 utc = sync::now();
            const F64 clock = plus::freeze::worldSeconds();

            Values goal = sLast;    // gone: the fall and the storm fade, the wind stays
            goal.mm = goal.cm = goal.storm = 0.f;
            if (target.active)
            {
                const tag::Params& p = target.params;
                const sync::Wind w = sync::wind(p, target.seed, utc);
                const F32 from = w.from_deg * DEG_TO_RAD;
                // The level is taken from the tag's own rate: pvar's variation would carry it back and forth across a level's edge.
                goal.mm = tag::rainMm(p);
                goal.cm = tag::snowCm(p);
                goal.base = p.wind_speed;
                goal.wind_x = sinf(from) * w.speed;
                goal.wind_y = cosf(from) * w.speed;
                goal.storm = p.storm;
            }
            if (target.generation != sSeen)
            {
                sSeen = target.generation;
                sFrom = sLast;
                sBlendStart = clock;
                if (target.active)
                {
                    sTrans = target.params.trans;
                }
                sBlendDuration = target.immediate ? 0.f : sTrans;
            }

            F32 t = 1.f;
            if (sBlendDuration > 0.f)
            {
                t = llclamp((F32)((clock - sBlendStart) / sBlendDuration), 0.f, 1.f);
                t = t * t * (3.f - 2.f * t);
            }
            const Values now = (t >= 1.f) ? goal : blend(sFrom, goal, t);
            sLast = now;

            if (!target.active && now.mm < NONE && now.cm < NONE && now.storm < NONE)
            {
                // Faded out: the settings' values stand again.
                sLast = Values();
                if (sRainWasOn)
                {
                    sRainWasOn = false;
                    doOnIdleOneTime(&plus::skinsss::markWantChanged);
                }
                return false;
            }

            sEffective.fromParcel = true;
            sEffective.rainLevel = tag::rainLevelOfMm(now.mm);
            sEffective.rainMm = llmax(now.mm, 0.f);
            sEffective.snowLevel = tag::snowLevelOfCm(now.cm);
            const F32 speed = sqrtf(now.wind_x * now.wind_x + now.wind_y * now.wind_y);
            sEffective.windSpeed = llclamp(speed, 0.f, tag::WIND_SPEED_MAX);
            sEffective.windBaseSpeed = llclamp(now.base, 0.f, tag::WIND_SPEED_MAX);
            F32 from_deg = (speed > NONE) ? atan2f(now.wind_x, now.wind_y) * RAD_TO_DEG : target.params.wind_from;
            if (from_deg < 0.f)
            {
                from_deg += 360.f;
            }
            sEffective.windFromDeg = from_deg;
            sEffective.storm = llclamp(now.storm, 0.f, 1.f);

            // The avatars' mark for the rain's splashes comes and goes with the rain.
            // (as plus/rain's avatarMarkWanted: it rains and does not snow)
            const bool rain_on = sEffective.rainLevel > 0 && sEffective.snowLevel == 0;
            if (rain_on != sRainWasOn)
            {
                sRainWasOn = rain_on;
                doOnIdleOneTime(&plus::skinsss::markWantChanged);
            }
            return true;
        }

        void init()
        {
            // Settings: app_settings/settings_plus.xml (PlusWeatherAllowParcel); the switch
            // is on the photo tools' Sky & Weather tab (app_settings/plus/phototools/catalog.xml).
            // plus/rain asks effective() for the level of the rain and snow and the wind.
#ifdef _DEBUG
            std::string report;
            if (!tag::selftest(report))
            {
                LL_WARNS("SLWX") << report << LL_ENDL;
            }
#endif
        }

        plus::FeatureRegistration sRegistration({ "slwx", &init, {} });
    }

    bool effective(Effective& out)
    {
        const U64 frame = LLFrameTimer::getFrameCount();
        if (frame != sFrame)
        {
            sFrame = frame;
            sAnswer = compute();
        }
        if (sAnswer)
        {
            out = sEffective;
        }
        return sAnswer;
    }

    bool allowParcel()
    {
        static LLCachedControl<bool> allow(gSavedSettings, SETTING_ALLOW, false);
        return allow;
    }

    bool canEditCurrentParcel()
    {
        return parcel::canEdit();
    }

    bool setParcelTag(const std::string& tag_text)
    {
        return parcel::setTag(tag_text);
    }

    bool clearParcelTag()
    {
        return parcel::clearTag();
    }
}

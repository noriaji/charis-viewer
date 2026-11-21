/**
 * @file slwxparceledit.cpp
 * @brief Parcel weather window: what the XUI window and the RmlUi one share.
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

#include "slwxparceledit.h"
#include "slwx.h"

#include "llagent.h"
#include "llfloater.h"
#include "llnotificationsutil.h"
#include "llparcel.h"
#include "llviewerparcelmgr.h"
#include "llviewerregion.h"

#include <cctype>
#include <cmath>

namespace plus::slwx::parceledit
{
    namespace
    {
        // The XUI window's sliders (floater_plus_weather_parcel.xml), in the order of the Slider enum.
        const SliderSpec SPECS[SLIDER_COUNT] = {
            { "rain",       0.f,   1.f, 0.05f, 2 },
            { "snow",       0.f,   1.f, 0.05f, 2 },
            { "wind_dir",   0.f, 360.f, 5.f,   0 },
            { "wind_speed", 0.f,  40.f, 0.5f,  1 },
            { "gust",       0.f,   1.f, 0.05f, 2 },
            { "storm",      0.f,   1.f, 0.05f, 2 },
        };

        // The same separators as the tag reader (slwxtag.cpp).
        bool isSpace(char c)
        {
            return c == ' ' || c == '\t' || c == '\n' || c == '\r';
        }

        bool isKnownKey(const std::string& key)
        {
            static const char* const KEYS[] = { "off", "clear", "rain", "snow", "rainmm", "snowcm", "pvar", "wind",
                                                "var", "gust", "wdirvar", "storm", "fog", "cloud", "trans", "seed",
                                                "depth", "rate", "since" };
            for (const char* known : KEYS)
            {
                if (key == known)
                {
                    return true;
                }
            }
            return false;
        }

        // The items of the tag in the description that this viewer does not know (a newer Slipstream's
        // keys), each with a space before it. tag::write() builds a tag from the known keys only, so
        // these are put back as they were.
        std::string unknownItems(const std::string& description)
        {
            size_t begin = 0;
            size_t end   = 0;
            if (!tag::find(description, begin, end) || end < begin + 7)
            {
                return std::string();
            }
            std::string  result;
            size_t       at   = begin + 5;   // after "[slwx"
            const size_t stop = end - 1;     // the "]"
            while (at < stop)
            {
                while (at < stop && isSpace(description[at]))
                {
                    ++at;
                }
                const size_t first = at;
                while (at < stop && !isSpace(description[at]))
                {
                    ++at;
                }
                if (at == first)
                {
                    break;
                }
                const std::string item = description.substr(first, at - first);
                std::string       key  = item.substr(0, item.find('='));
                for (char& c : key)
                {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                if (!isKnownKey(key))
                {
                    result += ' ';
                    result += item;
                }
            }
            return result;
        }

        // The window may be closed by the time the box is answered: its own text is used if it is
        // still there, the English one if not.
        void tellFailed(const LLHandle<LLFloater>& handle)
        {
            std::string message = "The parcel weather could not be written.";
            if (LLFloater* floater = handle.get())
            {
                message = floater->getString("write_failed");
            }
            LLNotificationsUtil::add("SystemMessageTip", LLSD().with("MESSAGE", message));
        }
    }

    const SliderSpec& spec(int slider)
    {
        return SPECS[slider];
    }

    F32 snap(int slider, F32 value)
    {
        const SliderSpec& s = SPECS[slider];
        value = llclamp(value, s.min_value, s.max_value);
        // Round to nearest increment (bias towards rounding down), as LLSlider::setValue
        value -= s.min_value;
        value += s.increment / 2.0001f;
        value -= std::fmod(value, s.increment);
        value += s.min_value;
        return value;
    }

    std::string valueText(int slider, F32 value)
    {
        const int digits = SPECS[slider].digits;
        // Don't display very small negative values as -0.000
        const F32 shown = static_cast<F32>(std::floor(value * std::pow(10.0, digits) + 0.5) / std::pow(10.0, digits));
        return llformat("%.*f", digits, shown);
    }

    void valuesOf(const std::string& description, F32 (&out)[SLIDER_COUNT])
    {
        const tag::ParseResult parsed = tag::parse(description);
        if (parsed.found && !parsed.off)
        {
            // Rain and snow show what the tag asks for (rainmm and snowcm through the scale, not rain alone).
            const tag::Params& p = parsed.params;
            out[RAIN]       = tag::mmToRain(tag::rainMm(p));
            out[SNOW]       = tag::cmToSnow(tag::snowCm(p));
            out[WIND_DIR]   = p.wind_from;
            out[WIND_SPEED] = p.wind_speed;
            out[GUST]       = p.gust;
            out[STORM]      = p.storm;
        }
        else
        {
            // No tag (or "off"): the defaults, not what an earlier parcel left in the sliders.
            const tag::Params defaults;
            out[RAIN]       = defaults.rain;
            out[SNOW]       = defaults.snow;
            out[WIND_DIR]   = defaults.wind_from;
            out[WIND_SPEED] = defaults.wind_speed;
            out[GUST]       = defaults.gust;
            out[STORM]      = defaults.storm;
        }
    }

    // A slider the user did not move keeps the value the tag in the parcel has (a slider rounds to its step, so
    // reading it back would turn 0.72 into 0.70); the keys the sliders do not show (seed, variation, fog ...) are
    // the old tag's too.
    std::string buildTag(const std::string& description, const tag::ParseResult& existing,
                         const F32 (&v)[SLIDER_COUNT], const F32 (&base)[SLIDER_COUNT])
    {
        tag::Params p = existing.found && !existing.off ? existing.params : tag::Params();
        if (v[RAIN] != base[RAIN])
        {
            p.rain       = v[RAIN];
            p.has_rainmm = false;   // the writer derives rainmm from rain
        }
        if (v[SNOW] != base[SNOW])
        {
            p.snow       = v[SNOW];
            p.has_snowcm = false;
        }
        if (v[WIND_DIR] != base[WIND_DIR])
        {
            p.wind_from = v[WIND_DIR];
        }
        if (v[WIND_SPEED] != base[WIND_SPEED])
        {
            p.wind_speed = v[WIND_SPEED];
        }
        if (v[GUST] != base[GUST])
        {
            p.gust = v[GUST];
        }
        if (v[STORM] != base[STORM])
        {
            p.storm = v[STORM];
        }

        std::string text = tag::write(p);
        const std::string extra = unknownItems(description);
        if (!extra.empty() && !text.empty() && text.back() == ']')
        {
            text.insert(text.size() - 1, extra);
        }
        return text;
    }

    int bytesLeftWith(const std::string& description, const std::string& tagText)
    {
        std::string after = description;
        if (!tag::replaceIn(after, tagText))
        {
            return -1;
        }
        return static_cast<int>(tag::DESCRIPTION_LIMIT) - static_cast<int>(after.size());
    }

    const std::string& parcelDescription()
    {
        static const std::string none;
        const LLParcel* parcel = LLViewerParcelMgr::getInstance()->getAgentParcel();
        return parcel ? parcel->getDesc() : none;
    }

    std::pair<LLUUID, S32> parcelKey()
    {
        const LLViewerRegion* region = gAgent.getRegion();
        return { region ? region->getRegionID() : LLUUID::null, LLViewerParcelMgr::getInstance()->getAgentParcelId() };
    }

    void askApply(LLFloater* owner, const std::string& tagText)
    {
        // The description is public: ask first, and say what is written and how much is left.
        LLStringUtil::format_map_t args;
        args["[TAG]"]  = tagText;
        args["[LEFT]"] = std::to_string(bytesLeftWith(parcelDescription(), tagText));
        const std::string   message = owner->getString("confirm_apply", args);
        const std::string   text    = tagText;
        const auto          where   = parcelKey();
        LLHandle<LLFloater> handle  = owner->getHandle();
        LLNotificationsUtil::add("GenericAlertYesCancel", LLSD().with("MESSAGE", message), LLSD(),
                                 [text, where, handle](const LLSD& notification, const LLSD& response)
                                 {
                                     if (LLNotificationsUtil::getSelectedOption(notification, response) != 0)
                                     {
                                         return;
                                     }
                                     // The agent may have walked into another parcel while the box stood open.
                                     if (parcelKey() != where || !canEditCurrentParcel() || !setParcelTag(text))
                                     {
                                         tellFailed(handle);
                                     }
                                 });
    }

    void askClear(LLFloater* owner)
    {
        const auto          where  = parcelKey();
        LLHandle<LLFloater> handle = owner->getHandle();
        LLNotificationsUtil::add("GenericAlertYesCancel", LLSD().with("MESSAGE", owner->getString("confirm_clear")), LLSD(),
                                 [where, handle](const LLSD& notification, const LLSD& response)
                                 {
                                     if (LLNotificationsUtil::getSelectedOption(notification, response) != 0)
                                     {
                                         return;
                                     }
                                     if (parcelKey() != where || !canEditCurrentParcel() || !clearParcelTag())
                                     {
                                         tellFailed(handle);
                                     }
                                 });
    }
}

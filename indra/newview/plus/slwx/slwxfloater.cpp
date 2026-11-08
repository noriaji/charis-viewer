/**
 * @file slwxfloater.cpp
 * @brief Parcel weather: the window that writes the [slwx ...] tag into the parcel description.
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

// No menu entry for now (withdrawn 2026-11-07; it was "Parcel Weather..." in menu_plus.xml, Floater.Toggle plus_weather_parcel), and the
// openfloater deny lists in llviewerfloaterreg.cpp keep links from opening it. Design: research/229 §8.
// The tag's text is built and read by slwxtag.h and written by slwx.h (the core branch); this window
// holds the sliders, shows what would be written and how many bytes would be left, asks before it
// writes (the description is public) and says why it cannot when the agent is not the owner/manager.

#include "llviewerprecompiledheaders.h"

#include "slwxfloater.h"
#include "slwx.h"
#include "slwxtag.h"
#include "plus.h"

#include "llagent.h"
#include "llbutton.h"
#include "llfloater.h"
#include "llfloaterreg.h"
#include "llnotificationsutil.h"
#include "llparcel.h"
#include "llsliderctrl.h"
#include "lltextbox.h"
#include "llviewerparcelmgr.h"
#include "llviewerregion.h"

#include <cctype>

namespace plus::slwx
{
    namespace
    {
        enum Slider { RAIN, SNOW, WIND_DIR, WIND_SPEED, GUST, STORM, SLIDER_COUNT };

        // The bytes left in the description when `tag` replaces the tag there (or is added); -1: no fit.
        int bytesLeftWith(const std::string& description, const std::string& tag)
        {
            std::string after = description;
            if (!tag::replaceIn(after, tag))
            {
                return -1;
            }
            return static_cast<int>(tag::DESCRIPTION_LIMIT) - static_cast<int>(after.size());
        }

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

        class ParcelWeatherFloater : public LLFloater
        {
            friend class ::LLFloaterReg;

        public:
            bool postBuild() override
            {
                static const char* const NAMES[SLIDER_COUNT] = { "rain", "snow", "wind_dir", "wind_speed", "gust", "storm" };
                for (int i = 0; i < SLIDER_COUNT; ++i)
                {
                    mSlider[i] = getChild<LLSliderCtrl>(NAMES[i]);
                }
                mReason  = getChild<LLTextBox>("reason");
                mPreview = getChild<LLTextBox>("preview");
                mBytes   = getChild<LLTextBox>("bytes");
                mWarning = getChild<LLTextBox>("warning");
                mApply   = getChild<LLButton>("apply");
                mClear   = getChild<LLButton>("clear");
                mApply->setClickedCallback([this](LLUICtrl*, const LLSD&) { onApply(); });
                mClear->setClickedCallback([this](LLUICtrl*, const LLSD&) { onClear(); });
                return true;
            }

            void onOpen(const LLSD& key) override
            {
                LLFloater::onOpen(key);
                loadFromParcel();
                mDirty = true;
            }

            void draw() override
            {
                refresh();
                LLFloater::draw();
            }

        private:
            ParcelWeatherFloater(const LLSD& key) : LLFloater(key) {}

            static const std::string& parcelDescription()
            {
                static const std::string none;
                const LLParcel* parcel = LLViewerParcelMgr::getInstance()->getAgentParcel();
                return parcel ? parcel->getDesc() : none;
            }

            // The parcel the agent stands in, as the pair the server names it by.
            static std::pair<LLUUID, S32> parcelKey()
            {
                const LLViewerRegion* region = gAgent.getRegion();
                return { region ? region->getRegionID() : LLUUID::null, LLViewerParcelMgr::getInstance()->getAgentParcelId() };
            }

            // The sliders start from the tag that stands in the parcel now, if there is one. Rain and
            // snow show what the tag asks for (rainmm and snowcm through the scale, not rain alone).
            void loadFromParcel()
            {
                const tag::ParseResult parsed = tag::parse(parcelDescription());
                if (parsed.found && !parsed.off)
                {
                    const tag::Params& p = parsed.params;
                    mSlider[RAIN]->setValue(tag::mmToRain(tag::rainMm(p)));
                    mSlider[SNOW]->setValue(tag::cmToSnow(tag::snowCm(p)));
                    mSlider[WIND_DIR]->setValue(p.wind_from);
                    mSlider[WIND_SPEED]->setValue(p.wind_speed);
                    mSlider[GUST]->setValue(p.gust);
                    mSlider[STORM]->setValue(p.storm);
                }
                else
                {
                    // No tag (or "off"): the defaults, not what an earlier parcel left in the sliders.
                    const tag::Params defaults;
                    mSlider[RAIN]->setValue(defaults.rain);
                    mSlider[SNOW]->setValue(defaults.snow);
                    mSlider[WIND_DIR]->setValue(defaults.wind_from);
                    mSlider[WIND_SPEED]->setValue(defaults.wind_speed);
                    mSlider[GUST]->setValue(defaults.gust);
                    mSlider[STORM]->setValue(defaults.storm);
                }
                // A slider rounds to its step: what it holds now is what "not touched" means below.
                for (int i = 0; i < SLIDER_COUNT; ++i)
                {
                    mBase[i] = mSlider[i]->getValueF32();
                }
            }

            // The tag to write. A slider the user did not move keeps the value the tag in the parcel has
            // (a slider rounds to its step, so reading it back would turn 0.72 into 0.70); the keys the
            // sliders do not show (seed, variation, fog ...) are the old tag's too.
            std::string buildTag(const std::string& description, const tag::ParseResult& existing) const
            {
                tag::Params p = existing.found && !existing.off ? existing.params : tag::Params();
                F32 v[SLIDER_COUNT];
                for (int i = 0; i < SLIDER_COUNT; ++i)
                {
                    v[i] = mSlider[i]->getValueF32();
                }
                if (v[RAIN] != mBase[RAIN])
                {
                    p.rain       = v[RAIN];
                    p.has_rainmm = false;   // the writer derives rainmm from rain
                }
                if (v[SNOW] != mBase[SNOW])
                {
                    p.snow       = v[SNOW];
                    p.has_snowcm = false;
                }
                if (v[WIND_DIR] != mBase[WIND_DIR])
                {
                    p.wind_from = v[WIND_DIR];
                }
                if (v[WIND_SPEED] != mBase[WIND_SPEED])
                {
                    p.wind_speed = v[WIND_SPEED];
                }
                if (v[GUST] != mBase[GUST])
                {
                    p.gust = v[GUST];
                }
                if (v[STORM] != mBase[STORM])
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

            bool sliderTouched() const
            {
                for (int i = 0; i < SLIDER_COUNT; ++i)
                {
                    if (mSlider[i]->getValueF32() != mBase[i])
                    {
                        return true;
                    }
                }
                return false;
            }

            // Looked at every frame; the texts are set only when something they depend on changed.
            void refresh()
            {
                const std::string& description = parcelDescription();
                const bool         canEdit     = canEditCurrentParcel();
                F32  values[SLIDER_COUNT];
                bool same = !mDirty && canEdit == mCanEdit && description == mDescription;
                for (int i = 0; i < SLIDER_COUNT; ++i)
                {
                    values[i] = mSlider[i]->getValueF32();
                    same = same && values[i] == mValues[i];
                }
                if (same)
                {
                    return;
                }
                // The description changed under an untouched window (another parcel, or a new tag): follow it.
                if (!mDirty && description != mDescription && !sliderTouched())
                {
                    loadFromParcel();
                    for (int i = 0; i < SLIDER_COUNT; ++i)
                    {
                        values[i] = mSlider[i]->getValueF32();
                    }
                }
                mDirty       = false;
                mCanEdit     = canEdit;
                mDescription = description;
                for (int i = 0; i < SLIDER_COUNT; ++i)
                {
                    mValues[i] = values[i];
                }

                const tag::ParseResult existing = tag::parse(description);
                mUnclosed = tag::hasUnclosedOpen(description);
                mTag      = buildTag(description, existing);
                const int  left = bytesLeftWith(description, mTag);
                const bool fits = left >= 0 && !mUnclosed;

                setText(mPreview, mTag);
                LLStringUtil::format_map_t args;
                args["[LEFT]"] = std::to_string(left);
                setText(mBytes, getString(left >= 0 ? "bytes_left" : "bytes_over", args));
                setText(mReason, canEdit ? getString("can_edit") : getString("cannot_edit"));
                LLStringUtil::format_map_t bad;
                bad["[N]"] = std::to_string(existing.bad_items);
                setText(mWarning, mUnclosed ? getString("tag_unclosed")
                                            : (existing.bad_items > 0 ? getString("tag_warning", bad) : std::string()));
                mApply->setEnabled(canEdit && fits);
                mClear->setEnabled(canEdit && existing.found);
            }

            static void setText(LLTextBox* box, const std::string& text)
            {
                if (box->getText() != text)
                {
                    box->setText(text);
                }
            }

            void onApply()
            {
                // The description is public: ask first, and say what is written and how much is left.
                LLStringUtil::format_map_t args;
                args["[TAG]"]  = mTag;
                args["[LEFT]"] = std::to_string(bytesLeftWith(parcelDescription(), mTag));
                const std::string   message = getString("confirm_apply", args);
                const std::string   text    = mTag;
                const auto          where   = parcelKey();
                LLHandle<LLFloater> handle  = getHandle();
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

            void onClear()
            {
                const auto          where  = parcelKey();
                LLHandle<LLFloater> handle = getHandle();
                LLNotificationsUtil::add("GenericAlertYesCancel", LLSD().with("MESSAGE", getString("confirm_clear")), LLSD(),
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

            // The window may be closed by the time the box is answered: its own text is used if it is
            // still there, the English one if not.
            static void tellFailed(const LLHandle<LLFloater>& handle)
            {
                std::string message = "The parcel weather could not be written.";
                if (auto* floater = dynamic_cast<ParcelWeatherFloater*>(handle.get()))
                {
                    message = floater->getString("write_failed");
                }
                LLNotificationsUtil::add("SystemMessageTip", LLSD().with("MESSAGE", message));
            }

            LLSliderCtrl* mSlider[SLIDER_COUNT] = {};
            LLTextBox*    mReason  = nullptr;
            LLTextBox*    mPreview = nullptr;
            LLTextBox*    mBytes   = nullptr;
            LLTextBox*    mWarning = nullptr;
            LLButton*     mApply   = nullptr;
            LLButton*     mClear   = nullptr;

            F32 mBase[SLIDER_COUNT] = {};   // the sliders as loaded from the parcel
            // What the texts were last made from.
            bool        mDirty    = true;
            bool        mCanEdit  = false;
            bool        mUnclosed = false;
            std::string mDescription;
            F32         mValues[SLIDER_COUNT] = {};
            std::string mTag;
        };

        void init()
        {
            LLFloaterReg::add(PARCEL_FLOATER_NAME, "floater_plus_weather_parcel.xml",
                              (LLFloaterBuildFunc)&LLFloaterReg::build<ParcelWeatherFloater>);
        }

        plus::FeatureRegistration sRegistration({ "slwxfloater", &init, {} });
    }
}

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

// Opened from the Charis menu ("Parcel Weather..." in menu_plus.xml, Floater.Toggle plus_weather_parcel; withdrawn 2026-11-07, back
// 2026-11-15). The openfloater deny lists in llviewerfloaterreg.cpp still keep links from opening it (it writes the public parcel
// description). Design: research/229 §8.
// The tag's text is built and read by slwxtag.h and written by slwx.h (the core branch); this window
// holds the sliders, shows what would be written and how many bytes would be left, asks before it
// writes (the description is public) and says why it cannot when the agent is not the owner/manager.

#include "llviewerprecompiledheaders.h"

#include "slwxfloater.h"
#include "slwx.h"
#include "slwxparceledit.h"
#include "slwxparcelrml.h"
#include "slwxtag.h"
#include "plus.h"
#include "plus/safemode/safemode.h"

#include "llbutton.h"
#include "llfloater.h"
#include "llfloaterreg.h"
#include "llsliderctrl.h"
#include "lltextbox.h"
#include "llviewercontrol.h"

namespace plus::slwx
{
    namespace
    {
        using namespace parceledit;

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

            // The sliders start from the tag that stands in the parcel now, if there is one. Rain and
            // snow show what the tag asks for (rainmm and snowcm through the scale, not rain alone).
            void loadFromParcel()
            {
                F32 wanted[SLIDER_COUNT];
                valuesOf(parcelDescription(), wanted);
                for (int i = 0; i < SLIDER_COUNT; ++i)
                {
                    mSlider[i]->setValue(wanted[i]);
                }
                // A slider rounds to its step: what it holds now is what "not touched" means below.
                for (int i = 0; i < SLIDER_COUNT; ++i)
                {
                    mBase[i] = mSlider[i]->getValueF32();
                }
            }

            // The tag to write (parceledit::buildTag: a slider the user did not move keeps the tag's own value).
            std::string buildTag(const std::string& description, const tag::ParseResult& existing) const
            {
                F32 v[SLIDER_COUNT];
                for (int i = 0; i < SLIDER_COUNT; ++i)
                {
                    v[i] = mSlider[i]->getValueF32();
                }
                return parceledit::buildTag(description, existing, v, mBase);
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

            void onApply() { askApply(this, mTag); }
            void onClear() { askClear(this); }

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

        // The window is the RmlUi one (slwxparcelrml.cpp, research/389). This XUI version is frozen (not changed any
        // more: a thing added to the window goes to both) and is used in safe mode, where RmlUi is not started, and
        // with PlusRmlUiWindows off (the choice is made once, here: it takes effect at the next start).
        void init()
        {
            if (plus::safemode::active() || !gSavedSettings.getBOOL("PlusRmlUiWindows"))
            {
                LLFloaterReg::add(PARCEL_FLOATER_NAME, "floater_plus_weather_parcel.xml",
                                  (LLFloaterBuildFunc)&LLFloaterReg::build<ParcelWeatherFloater>);
            }
            else
            {
                addParcelRmlFloater(PARCEL_FLOATER_NAME);
            }
        }

        plus::FeatureRegistration sRegistration({ "slwxfloater", &init, {} });
    }
}

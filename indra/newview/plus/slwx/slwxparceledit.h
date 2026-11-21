/**
 * @file slwxparceledit.h
 * @brief Parcel weather window: what the XUI window (slwxfloater.cpp) and the RmlUi one (slwxparcelrml.cpp) share.
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

// The six sliders of the window and the tag they make, the parcel the window is about, and the two questions that
// write or remove the tag. Moved out of slwxfloater.cpp unchanged when the window was made again with RmlUi
// (research/389), so the two windows do not each hold a copy. Main thread only.

#ifndef PLUS_SLWXPARCELEDIT_H
#define PLUS_SLWXPARCELEDIT_H

#include "slwxtag.h"

#include "lluuid.h"

#include <string>
#include <utility>

class LLFloater;

namespace plus::slwx::parceledit
{
    enum Slider { RAIN, SNOW, WIND_DIR, WIND_SPEED, GUST, STORM, SLIDER_COUNT };

    // A slider as the XUI window declares it (floater_plus_weather_parcel.xml): its range, step (increment) and the
    // digits after the point of the number beside it (decimal_digits).
    struct SliderSpec
    {
        const char* name;
        F32 min_value;
        F32 max_value;
        F32 increment;
        int digits;
    };
    const SliderSpec& spec(int slider);

    // LLSlider::setValue's rounding: into the range, then to the nearest step (a half goes down).
    F32 snap(int slider, F32 value);
    // The number beside a slider (LLSliderCtrl::updateText: "%.nf" after rounding to n digits).
    std::string valueText(int slider, F32 value);
    // The slider values the tag in `description` asks for, or the defaults when there is none (or "off"). Not rounded.
    void valuesOf(const std::string& description, F32 (&out)[SLIDER_COUNT]);

    // The tag to write. A slider that is not `base` (what it held as loaded) overrides the old tag's value; the
    // keys the sliders do not show are the old tag's too, with the items this viewer does not know put back.
    std::string buildTag(const std::string& description, const tag::ParseResult& existing,
                         const F32 (&values)[SLIDER_COUNT], const F32 (&base)[SLIDER_COUNT]);
    // The bytes left in the description when `tag` replaces the tag there (or is added); -1: no fit.
    int bytesLeftWith(const std::string& description, const std::string& tag);

    const std::string& parcelDescription();
    // The parcel the agent stands in, as the pair the server names it by.
    std::pair<LLUUID, S32> parcelKey();

    // The two buttons: ask first (the description is public), then write or remove. The strings confirm_apply,
    // confirm_clear and write_failed are `owner`'s (its XML); the answer may come after the window was closed.
    void askApply(LLFloater* owner, const std::string& tag);
    void askClear(LLFloater* owner);
}

#endif // PLUS_SLWXPARCELEDIT_H

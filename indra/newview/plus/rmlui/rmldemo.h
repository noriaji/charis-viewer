/**
 * @file rmldemo.h
 * @brief Plus.RmlDemo, the RmlUi test window (development), and its XUI twin (research/364 4, 3.5).
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

// Two development windows with the same contents, opened from the Charis menu (menu_plus.xml), to try RmlUi and
// to compare its cost with the XUI's (research/364 3.5: open one, then the other, and compare the FrameCost
// lines' "ui 2d" / "ui rml" and the "RmlUi:" line):
//   plus_rml_demo      app_settings/plus/rmlui/demo.rml in a plus_rml_view: a button and its count, a check box,
//                      a slider and a number box that follow each other (a data model; the box takes a typed
//                      number), a text field (Japanese input through the IME),
//                      1000 rows of which only the visible ones are elements, tool tips (title), and two
//                      buttons that switch CSS variables: the accent colour (set from C++, as a user's theme
//                      will be: research/320) and the body text size (a class that sets the variable).
//                      The host's parts (research/387, rmlparts.h): a read-only log that follows its end, a field
//                      that selects all when it gets the focus, a number box (arrows, wheel, Up / Down, Esc), a
//                      name cut with "..." and shown whole in a tool tip, the text editor menu on a right click of
//                      any field, and the window's own menu (a right click on the Count button: rmlmenu.h).
//   plus_rml_demo_xui  the same with psui's parts and the official widgets (the list: a scroll_list).
// The words are each window's <string>s (en and ja).

#ifndef PLUS_RMLDEMO_H
#define PLUS_RMLDEMO_H

#include "rmlcommon.h"
#include "rmlmenu.h"

#include "llfloater.h"

#include <string>

class LLRmlView;

namespace plus::rmlui
{
    class RmlDemoFloater : public LLFloater
    {
    public:
        explicit RmlDemoFloater(const LLSD& key);
        ~RmlDemoFloater() override;
        bool postBuild() override;

    private:
        void beforeLoad(Rml::Context& context);
        void afterLoad(Rml::ElementDocument& document);
        void beforeUpdate();
        void afterUpdate();
        void onEvent(Rml::Event& event);
        void switchColour();
        void switchTextSize();
        // The number box beside the slider was committed (Enter or the focus left): into the range, or back to the
        // value it had when the text is no number. The slider and the box follow mAmount (the data model).
        void commitAmount(Rml::Event& event);
        // The right click on the Count button: its own menu (research/387 3.2), and what its two items do.
        void showCountMenu(Rml::Event& event);
        void onMenu(const std::string& what);
        void addLogLine();
        // The rows the list shows now (its scroll position): spacers above and below the elements made.
        void fillRows();

        LLHandle<LLRmlView> mHost;
        Rml::DataModelHandle mModel;            // valid while the document is loaded
        int mClicks = 0;
        bool mOption = false;
        int mAmount = 50;
        bool mAltColour = false;
        bool mLargeText = false;
        bool mRowsDirty = true;                 // fillRows on the next update (set in RmlUi's callbacks)
        S32 mFirstRow = -1;
        S32 mRowCount = 0;
        F32 mListHeightPx = -1.f;               // the list's client height after the last update (-1: not seen yet)
        std::string mRowFormat;                 // "Row [N]"
        std::string mLogFormat;                 // "Log line [N]"
        std::string mLog;                       // the read-only log's text (the newest lines)
        int mLogLines = 0;
        int mSpinValue = 50;                    // the number box's value (the data model: spin_value)
        plus::rmlui::ContextMenu mMenu;         // the Count button's menu
    };

    class RmlDemoXuiFloater : public LLFloater
    {
    public:
        explicit RmlDemoXuiFloater(const LLSD& key);
        bool postBuild() override;

    private:
        int mClicks = 0;
        bool mAltColour = false;
        bool mLargeText = false;
    };
}

#endif // PLUS_RMLDEMO_H

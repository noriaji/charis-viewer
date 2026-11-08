/**
 * @file flycamexit.cpp
 * @brief While the flycam is on: a "Leave the flycam" button at the bottom, and a short how-to once a session.
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

// The screen did not say whether the flycam was on. The button sits where the official Stand and Stop Flying
// buttons do (the bottom middle, in the same container, the same button), to the right of them and of Hide Beacon
// when one of those shows too (sitting, flying, a beacon being tracked), and never past the container's right
// edge. It shows while the flycam is on, and not in the mouselook (the Stand button is not shown there either),
// with the UI hidden or in the photo screen (which has its own switch and line saying the flycam is on:
// plus/photoscreen, photo-flycam). Its panel is made the first time the flycam goes on and stays; nothing of it
// runs before. Each frame it compares a few flags and moves its origin, as the Stand panel does
// (LLPanelStandStopFlying::draw()); the label is written only when the button shows.

#include "llviewerprecompiledheaders.h"

#include "flycammouse.h"

#include "plus/photoscreen/photoscreen.h"

#include "llagentcamera.h"
#include "llbutton.h"
#include "llfloaterworldmap.h"
#include "llmoveview.h"
#include "llnotificationsutil.h"
#include "llpanel.h"
#include "lltoolbar.h"
#include "lltoolbarview.h"
#include "llviewercontrol.h"
#include "llviewerinput.h"
#include "llviewerwindow.h"

#include <algorithm>

namespace plus::flycammouse
{
    namespace
    {
        constexpr char SETTING_HINT[] = "PlusFlycamHint";
        constexpr char SETTING_MODE[] = "PlusFlycamMouseMode";
        constexpr char PANEL_FILE[]   = "panel_plus_flycam_exit.xml";
        constexpr char CONTAINER[]    = "state_management_buttons_container";  // the official Stand / Stop Flying's
        constexpr char MENU_KEY[]     = "Alt+Shift+F";  // the menu item's shortcut: it works whatever the key bindings say
        constexpr char KEY_SEPARATOR[] = " | ";         // between the keys of getKeyBindingAsString()

        bool sHintShown = false;    // this session

        // The keys of a mode's toggle_flycam as the key settings have them ("Z | Alt+Shift+F"), the menu item's when
        // none is bound.
        std::string toggleKeys(const char* mode)
        {
            const std::string keys = gViewerInput.getKeyBindingAsString(mode, "toggle_flycam");
            return keys.empty() ? std::string(MENU_KEY) : keys;
        }

        // The one of them to name in a short line: the first with Alt or Ctrl (it reaches the key bindings also while
        // a text field has the keyboard, toggleFromKey()), else the first.
        std::string oneKey(const std::string& keys)
        {
            std::string first;
            for (size_t start = 0; start <= keys.size();)
            {
                const size_t end = std::min(keys.find(KEY_SEPARATOR, start), keys.size());
                const std::string key = keys.substr(start, end - start);
                if (key.find("Alt+") != std::string::npos || key.find("Ctrl+") != std::string::npos)
                {
                    return key;
                }
                if (first.empty())
                {
                    first = key;
                }
                start = end + sizeof(KEY_SEPARATOR) - 1;
            }
            return first;
        }

        class ExitPanel : public LLPanel
        {
        public:
            bool postBuild() override
            {
                mButton = getChild<LLButton>("leave_btn");
                mButton->setCommitCallback([this](LLUICtrl*, const LLSD&) { onLeave(); });
                mButton->setVisible(false);
                return true;
            }

            void draw() override
            {
                const bool wanted = gAgentCamera.isUsingFlycam() && !gAgentCamera.cameraMouselook()
                                    && gViewerWindow->getUIVisibility() && !plus::photoscreen::isActive();
                if (wanted != mShown)
                {
                    mShown = wanted;
                    if (wanted)
                    {
                        // one key only, so the label stays short (the how-to has them all): the one the photo screen
                        // names too (leaveKey(): one with Alt or Ctrl first)
                        mButton->setLabelArg("[KEY]", leaveKey());
                    }
                    mButton->setVisible(wanted);
                }
                if (mShown)
                {
                    place();
                }
                LLPanel::draw();
            }

        private:
            // The bottom middle, as LLPanelStandStopFlying::updatePosition(); to the right of the official panels of
            // the same container that show a button: Stand / Stop Flying (the middle) and Hide Beacon (to its right,
            // LLPanelHideBeacon::updatePosition())
            void place()
            {
                if (!mLookedUp)
                {
                    // both panels live as long as the viewer (static instances), so their buttons are looked up once
                    mLookedUp = true;
                    LLPanel* stand = LLPanelStandStopFlying::getInstance();
                    mStand = stand;
                    mStandButton = stand->findChild<LLView>("stand_btn");
                    mStopFlyingButton = stand->findChild<LLView>("stop_fly_btn");
                    LLPanel* beacon = LLPanelHideBeacon::getInstance();
                    mBeacon = beacon;
                    mBeaconButton = beacon->findChild<LLView>("hide_beacon_btn");
                }
                S32 right = S32_MIN;
                if (shows(mStand, mStandButton) || shows(mStand, mStopFlyingButton))
                {
                    right = mStand->getRect().mRight;
                }
                if (shows(mBeacon, mBeaconButton))
                {
                    right = llmax(right, mBeacon->getRect().mRight);
                }
                if (right != S32_MIN)
                {
                    placeAt(right);
                    return;
                }
                S32 center = 0;
                LLToolBar* bottom = gToolBarView ? gToolBarView->getToolbar(LLToolBarEnums::TOOLBAR_BOTTOM) : nullptr;
                if (bottom)
                {
                    center = bottom->getRect().getCenterX();
                }
                LLToolBar* left = gToolBarView ? gToolBarView->getToolbar(LLToolBarEnums::TOOLBAR_LEFT) : nullptr;
                if (left && left->hasButtons())
                {
                    center -= left->getRect().getWidth();
                }
                placeAt(center - getRect().getWidth() / 2);
            }

            // Kept inside the container: beside Hide Beacon in a narrow world view (or a large UI scale) it would
            // otherwise go past the right edge, out of reach (research/275 L061).
            void placeAt(S32 left)
            {
                const LLView* parent = getParent();
                const S32 max_left = parent ? parent->getRect().getWidth() - getRect().getWidth() : left;
                setOrigin(llmax(0, llmin(left, max_left)), 0);
            }

            // An official panel beside ours (the same container) that shows this button
            bool shows(const LLPanel* panel, const LLView* button) const
            {
                return panel && button && panel->getParent() == getParent() && panel->getVisible() && button->getVisible();
            }

            void onLeave()
            {
                setFocus(false);
                if (isOn())
                {
                    toggleFromUi();   // the menu's way: with no 3D mouse toggle(), with one the official
                }
            }

            LLButton*     mButton = nullptr;
            const LLPanel* mStand = nullptr;
            const LLView* mStandButton = nullptr;
            const LLView* mStopFlyingButton = nullptr;
            const LLPanel* mBeacon = nullptr;
            const LLView* mBeaconButton = nullptr;
            bool          mLookedUp = false;
            bool          mShown = false;
        };

        LLHandle<LLPanel> sPanel;

        void makePanel()
        {
            if (sPanel.get() || !gToolBarView)
            {
                return;
            }
            LLPanel* container = gToolBarView->findChild<LLPanel>(CONTAINER);
            if (!container)
            {
                return;
            }
            ExitPanel* panel = new ExitPanel();
            if (!panel->buildFromFile(PANEL_FILE))
            {
                delete panel;
                return;
            }
            container->addChild(panel);
            sPanel = panel->getHandle();
        }

        void showHint()
        {
            if (sHintShown || !gSavedSettings.getBOOL(SETTING_HINT))
            {
                return;
            }
            sHintShown = true;
            LLPanel* panel = sPanel.get();
            const S32 mode = llclamp(gSavedSettings.getS32(SETTING_MODE), 0, 2);
            LLSD args;
            args["LOOK"] = panel ? panel->getString(llformat("look_%d", mode)) : std::string();
            args["KEY"] = leaveKeys();
            LLNotificationsUtil::add("PlusFlycamHint", args, LLSD(), [](const LLSD& notification, const LLSD& response)
            {
                if (LLNotificationsUtil::getSelectedOption(notification, response) == 1)
                {
                    gSavedSettings.setBOOL(SETTING_HINT, false);   // "Do not show again"
                }
            });
        }
    }

    void showLeaveButtonAndHint()
    {
        makePanel();
        showHint();
    }

    std::string leaveKeys()
    {
        return toggleKeys("flycam");
    }

    std::string leaveKey()
    {
        return oneKey(leaveKeys());
    }

    std::string switchKey()
    {
        return oneKey(toggleKeys("third_person"));
    }
}

/**
 * @file slurlhandler.cpp
 * @brief Preferences > Setup: which viewer opens secondlife:// links from the browser,
 *        the buttons that make it this one or put back the one before, and the
 *        optional notice at start.
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

// research/237-slurl-handler.md.
//   Preferences > Setup (panel_plus_slurl_handler.xml, placed by panel_preferences_setup.xml):
//     the registry is read when the tab is shown and after a button; written only by the
//     buttons ("Open them with this viewer" asks first when another viewer has the links).
//   PlusSLURLCheckAtStart (off by default): at start the registry is read once and, when
//     another viewer has the links, a notice says so once the world is up. Nothing is written.
// No per-frame work. A link that comes while the viewer runs is the official path, unchanged:
// the new process hands it to the running one (LLAppViewerWin32::sendURLToOtherInstance, our
// own window class), which dispatches it as an untrusted external link.

#include "llviewerprecompiledheaders.h"

#include "slurlregistry.h"
#include "plus.h"

#include "llapp.h"
#include "llbutton.h"
#include "llcallbacklist.h"
#include "llnotificationsutil.h"
#include "llpanel.h"
#include "llstartup.h"
#include "lltextbox.h"
#include "lltrans.h"
#include "llviewercontrol.h"

namespace plus
{
    namespace slurlhandler
    {
        namespace
        {
            constexpr const char* SETTING_CHECK_AT_START = "PlusSLURLCheckAtStart";

            // A tip once the world is up; before that (Preferences opened at the login screen) the
            // tips may have nowhere to show, so a small window instead (review note A).
            void tell(const std::string& text)
            {
                const bool world_up = LLStartUp::getStartupState() >= STATE_STARTED;
                LLNotificationsUtil::add(world_up ? "SystemMessageTip" : "GenericAlert", LLSD().with("MESSAGE", text));
            }

            class SlurlHandlerPanel : public LLPanel
            {
            public:
                bool postBuild() override
                {
                    mStatus = getChild<LLTextBox>("plus_slurl_status");
                    mSecond = findChild<LLTextBox>("plus_slurl_second");   // the Charis page's; not in the old panel
                    mRegister = getChild<LLButton>("plus_slurl_register");
                    mUndo = getChild<LLButton>("plus_slurl_undo");
                    mRegister->setCommitCallback([this](LLUICtrl*, const LLSD&) { onRegister(); });
                    mUndo->setCommitCallback([this](LLUICtrl*, const LLSD&) { onUndo(); });
                    if (!supported())
                    {
                        setVisible(false);   // not Windows: the desktop registers the links
                    }
                    return true;
                }

                void onVisibilityChange(bool new_visibility) override
                {
                    LLPanel::onVisibilityChange(new_visibility);
                    if (new_visibility && supported())
                    {
                        refresh();
                    }
                }

            private:
                static bool isOther(Owner owner)
                {
                    return owner == Owner::Other || owner == Owner::OtherCharis;
                }

                // x-grid-location-info:// opens something else than secondlife:// does (another viewer, a
                // program that is not there, or nothing): a press of "Open them with this viewer" still has
                // work to do (review-slurl-handler-recheck low 1).
                static bool secondDiffers(const Status& st)
                {
                    return st.secondOwner != Owner::This
                           && (st.secondOwner != st.owner || st.secondName != st.name);
                }

                std::string ownerText(Owner owner, const std::string& name) const
                {
                    switch (owner)
                    {
                    case Owner::None:        return getString("status_none");
                    case Owner::This:        return getString("status_this");
                    case Owner::OtherCharis: return getString("status_other_charis");
                    case Owner::Other:       return withName("status_other", name);
                    case Owner::Missing:     return withName("status_missing", name);
                    }
                    return {};
                }

                // A string of the panel with [NAME] (the x-grid line: status_second, tip_second).
                std::string withName(const char* name, const std::string& value) const
                {
                    LLStringUtil::format_map_t args;
                    args["[NAME]"] = value;
                    return getString(name, args);
                }

                // Who opens x-grid-location-info:// links, as the status words it.
                std::string secondOwnerText(const Status& st) const
                {
                    return ownerText(st.secondOwner, st.secondName);
                }

                void refresh()
                {
                    const Status st = status();
                    const bool second_differs = secondDiffers(st);
                    std::string text = ownerText(st.owner, st.name);
                    if (st.userChoice)
                    {
                        text += getString("status_user_choice");
                    }
                    // The x-grid line: under the status (its own row, so neither is cut short; research/275 L058),
                    // or after it in a panel without that row.
                    const std::string second = second_differs
                                               ? withName("status_second", secondOwnerText(st))
                                               : std::string();
                    if (!mSecond && second_differs)
                    {
                        text += "; " + second;
                    }
                    LLStringUtil::format_map_t args;
                    args["[EXE]"] = st.exe.empty() ? getString("tip_no_exe") : st.exe;
                    args["[OURS]"] = ourExe();
                    std::string tip = getString("tip_status", args);
                    if (second_differs)
                    {
                        tip += "\n\n" + withName("tip_second", secondOwnerText(st));
                    }
                    if (text != mShownText)
                    {
                        mStatus->setText(text);
                        mShownText = text;
                    }
                    if (tip != mShownTip)
                    {
                        mStatus->setToolTip(tip);
                        if (mSecond)
                        {
                            mSecond->setToolTip(tip);
                        }
                        mShownTip = tip;
                    }
                    if (mSecond && second != mShownSecond)
                    {
                        mSecond->setText(second);
                        mShownSecond = second;
                    }
                    mRegister->setEnabled(!(st.owner == Owner::This && st.undoable) || second_differs);
                    mUndo->setEnabled(st.undoable);
                }

                void onRegister()
                {
                    const Status st = status();
                    const bool main_other   = isOther(st.owner);
                    const bool second_other = isOther(st.secondOwner);
                    if (main_other || second_other)
                    {
                        // Another viewer has the links (either scheme): never taken over without asking.
                        // Only x-grid-location-info:// is another viewer's: the question says so.
                        // Both another viewer's, not the same one: the x-grid one is named too.
                        const Owner owner = main_other ? st.owner : st.secondOwner;
                        std::string question = withName(main_other ? "confirm_take" : "confirm_take_grid",
                                                        ownerText(owner, main_other ? st.name : st.secondName));
                        if (main_other && second_other && secondDiffers(st))
                        {
                            question += "\n\n" + withName("confirm_also_grid", secondOwnerText(st));
                        }
                        LLHandle<LLPanel> handle = getHandle();
                        LLNotificationsUtil::add("GenericAlertYesCancel",
                            LLSD().with("MESSAGE", question), LLSD(),
                            [handle](const LLSD& notification, const LLSD& response)
                            {
                                if (LLNotificationsUtil::getSelectedOption(notification, response) != 0)
                                {
                                    return;
                                }
                                if (SlurlHandlerPanel* panel = static_cast<SlurlHandlerPanel*>(handle.get()))
                                {
                                    panel->doRegister();
                                }
                            });
                        return;
                    }
                    doRegister();
                }

                void doRegister()
                {
                    switch (registerHere())
                    {
                    case Result::Done:
                        tell(getString(status().userChoice ? "done_register_user_choice" : "done_register"));
                        break;
                    case Result::BadExe:      tell(getString("bad_exe")); break;
                    case Result::NothingToDo: break;
                    case Result::Failed:      tell(getString("failed")); break;
                    case Result::Partial:     tell(getString("failed_partial")); break;
                    }
                    refresh();
                }

                void onUndo()
                {
                    switch (undo())
                    {
                    case Result::Done:        tell(getString("done_undo")); break;
                    case Result::NothingToDo: tell(getString("nothing_to_undo")); break;
                    case Result::BadExe:
                    case Result::Partial:
                    case Result::Failed:      tell(getString("failed_undo")); break;
                    }
                    refresh();
                }

                LLTextBox*  mStatus   = nullptr;
                LLTextBox*  mSecond   = nullptr;   // the x-grid line (null: the old panel)
                LLButton*   mRegister = nullptr;
                LLButton*   mUndo     = nullptr;
                std::string mShownText;
                std::string mShownTip;
                std::string mShownSecond;
            };

            // panel_preferences_setup.xml names this class on its <panel filename="panel_plus_slurl_handler.xml">.
            LLPanelInjector<SlurlHandlerPanel> sSlurlHandlerPanel("plus_slurl_handler_panel");

            // Optional: tell, once the world is up, that another viewer opens the links. Reads only.
            void onUIReady()
            {
                if (!supported() || !gSavedSettings.getBOOL(SETTING_CHECK_AT_START))
                {
                    return;
                }
                const Status st = status();
                if (st.owner == Owner::This)
                {
                    return;
                }
                LLStringUtil::format_map_t args;
                args["[NAME]"] = st.owner == Owner::OtherCharis ? LLTrans::getString("PlusSLURLOtherCharis") : st.name;
                const std::string text = LLTrans::getString(st.owner == Owner::None ? "PlusSLURLNotRegistered"
                                                                                    : "PlusSLURLOtherViewer", args);
                LL_INFOS("SLURLHandler") << "secondlife:// links do not open this viewer (they open '"
                                         << st.exe << "')" << LL_ENDL;
                doPeriodically([text]()
                    {
                        if (LLApp::isExiting())
                        {
                            return true;
                        }
                        if (LLStartUp::getStartupState() < STATE_STARTED)
                        {
                            return false;
                        }
                        tell(text);
                        return true;
                    }, 1.0f);
            }

            plus::FeatureRegistration sRegistration({ "slurlhandler", nullptr, &onUIReady });
        }
    }
}

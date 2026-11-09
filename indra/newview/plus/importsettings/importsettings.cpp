/**
 * @file importsettings.cpp
 * @brief The window that brings settings over from another viewer, shown at
 *        the first start and from Preferences (research/143).
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

// How it runs:
//   init: no settings.xml in our folder yet (the first start of this install)
//     -> PlusImportSettingsPending is set. Every start notes our folder for other
//        Charis Viewer installs (sources.cpp).
//   onUIReady -> PlusImportSettingsPending set: at the login screen (or once the
//     world is up, when the login started by itself) the window opens, if there
//     is a viewer to bring settings over from; closing it clears the setting.
//   Preferences > Setup "Bring settings over..." and Preferences > Charis open it at
//     any time. There it asks first: our settings are overwritten.
// The window lists the viewers found (sources.cpp) and takes a folder the user
// picks; what to bring over is chosen with check boxes (importer.cpp does it).
// The graphics box is ticked for another Charis Viewer only: the graphics
// settings of other viewers were made for their own renderers.

#include "llviewerprecompiledheaders.h"

#include "importsettings.h"
#include "plus.h"

#include "llapp.h"
#include "llbutton.h"
#include "llcallbacklist.h"
#include "llcheckboxctrl.h"
#include "lldir.h"
#include "lldirpicker.h"
#include "llfloater.h"
#include "llfloaterreg.h"
#include "llnotificationsutil.h"
#include "llscrolllistctrl.h"
#include "llstartup.h"
#include "llviewercontrol.h"

#include <algorithm>
#include <ctime>

namespace
{
    using namespace plus::importsettings;

    constexpr char FLOATER_NAME[] = "plus_import_settings";
    constexpr char SETTING_PENDING[] = "PlusImportSettingsPending";
    // Pressed: starts on the next frame (runImport). Not the window's: one opened again meanwhile
    // does not start it a second time.
    bool sStarting = false;
    // Started this session (from any window): what is done is done, and a press after it asks
    // first (ours would be overwritten). Not the window's: one opened again while it starts or
    // runs knows it too (fix-round10 5).
    bool sStarted = false;

    std::string dateText(std::time_t time)
    {
        if (time == 0)
        {
            return std::string();
        }
        std::tm local{};
#if LL_WINDOWS
        localtime_s(&local, &time);
#else
        localtime_r(&time, &local);
#endif
        char text[32];
        std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &local);
        return text;
    }

    class ImportFloater : public LLFloater
    {
        friend class ::LLFloaterReg;

    public:
        bool postBuild() override
        {
            mList = getChild<LLScrollListCtrl>("sources");
            mList->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSelect(); });
            getChild<LLButton>("btn_browse")->setCommitCallback([this](LLUICtrl*, const LLSD&) { browse(); });
            getChild<LLButton>("btn_import")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onImport(); });
            getChild<LLButton>("btn_close")->setCommitCallback([this](LLUICtrl*, const LLSD&) { closeFloater(); });
            return true;
        }

        void onOpen(const LLSD&) override
        {
            mFirstStart = gSavedSettings.getBOOL(SETTING_PENDING) && !sStarted;
            if (mFirstStart)
            {
                getChild<LLUICtrl>("intro")->setValue(getString("intro_first"));
                getChild<LLButton>("btn_close")->setLabel(getString("not_now"));
            }
            mSources = findSources();
            refreshList(0);
            getChild<LLUICtrl>("status")->setValue(running() ? getString("busy") : sStarting ? getString("applying")
                                                   : mSources.empty() ? getString("none") : std::string());
        }

        // The files being copied (started by this window, or by one closed since).
        void onProgress(U32 copied, U32 total)
        {
            LLStringUtil::format_map_t args;
            args["[COPIED]"] = llformat("%u", copied);
            args["[TOTAL]"] = llformat("%u", total);
            getChild<LLUICtrl>("status")->setValue(getString("copying", args));
            onSelect();
        }

        void onDone(const std::string& name, const Result& result)
        {
            LLStringUtil::format_map_t args;
            args["[NAME]"] = name;
            args["[SETTINGS]"] = llformat("%u", result.settings);
            args["[FILES]"] = llformat("%u", result.files);
            args["[KEPT]"] = llformat("%u", result.kept);
            args["[OVER]"] = llformat("%u", result.over_limit);
            args["[ERRORS]"] = llformat("%u", result.errors);
            std::string text = getString("done", args);
            if (result.keys)
            {
                text += " " + getString("done_keys", args);
            }
            if (result.kept > 0)
            {
                text += " " + getString("done_kept", args);
            }
            if (result.over_limit > 0)
            {
                text += " " + getString("done_over", args);
            }
            if (result.errors > 0)
            {
                text += " " + getString("done_errors", args);
            }
            text += "\n" + getString("restart");
            getChild<LLUICtrl>("status")->setValue(text);
            onSelect();
        }

        void onClose(bool app_quitting) override
        {
            if (!app_quitting)      // left open at the end: offered again at the next start
            {
                gSavedSettings.setBOOL(SETTING_PENDING, false);
            }
        }

    private:
        explicit ImportFloater(const LLSD& key) : LLFloater(key) {}

        void refreshList(size_t selected)
        {
            mList->deleteAllItems();
            for (size_t i = 0; i < mSources.size(); ++i)
            {
                const Source& source = mSources[i];
                LLSD row;
                row["value"] = (S32)i;
                row["columns"][0]["column"] = "name";
                row["columns"][0]["value"] = source.name;
                row["columns"][1]["column"] = "folder";
                row["columns"][1]["value"] = source.root;
                row["columns"][2]["column"] = "date";
                row["columns"][2]["value"] = dateText(source.last_used);
                mList->addElement(row);
            }
            if (!mSources.empty())
            {
                mList->selectByValue((S32)std::min(selected, mSources.size() - 1));
            }
            onSelect();
        }

        const Source* selected() const
        {
            const LLScrollListItem* item = mList->getFirstSelected();
            if (!item)
            {
                return nullptr;
            }
            const S32 index = item->getValue().asInteger();
            return index >= 0 && index < (S32)mSources.size() ? &mSources[index] : nullptr;
        }

        void onSelect()
        {
            const Source* source = selected();
            getChild<LLButton>("btn_import")->setEnabled(source != nullptr && !running() && !sStarting);
            const std::string root = source ? source->root : std::string();
            if (!mGraphicsSet || root != mGraphicsFor)   // a new choice of viewer, not a refresh while copying
            {
                mGraphicsSet = true;
                mGraphicsFor = root;
                getChild<LLCheckBoxCtrl>("chk_graphics")->set(source && source->family == Family::CHARIS);
            }
        }

        void browse()
        {
            LLHandle<LLFloater> handle = getHandle();
            (new LLDirPickerThread([handle](const std::vector<std::string>& folders, std::string)
                                   {
                                       ImportFloater* self = static_cast<ImportFloater*>(handle.get());
                                       if (self && !folders.empty() && !folders[0].empty())
                                       {
                                           self->onPicked(folders[0]);
                                       }
                                   },
                                   gDirUtilp->getOSUserDir()))->getFile();
        }

        void onPicked(const std::string& folder)
        {
            Source source;
            if (!sourceFromFolder(folder, source))
            {
                LLStringUtil::format_map_t args;
                args["[FOLDER]"] = folder;
                getChild<LLUICtrl>("status")->setValue(getString("not_viewer_folder", args));
                return;
            }
            for (size_t i = 0; i < mSources.size(); ++i)
            {
                if (LLStringUtil::compareInsensitive(mSources[i].root, source.root) == 0)
                {
                    refreshList(i);
                    return;
                }
            }
            mSources.insert(mSources.begin(), source);
            refreshList(0);
            getChild<LLUICtrl>("status")->setValue(std::string());
        }

        Choice choice()
        {
            Choice c;
            c.general = getChild<LLCheckBoxCtrl>("chk_general")->get();
            c.graphics = getChild<LLCheckBoxCtrl>("chk_graphics")->get();
            c.keys = getChild<LLCheckBoxCtrl>("chk_keys")->get();
            c.accounts = getChild<LLCheckBoxCtrl>("chk_accounts")->get();
            c.chat = getChild<LLCheckBoxCtrl>("chk_chat")->get();
            c.files = getChild<LLCheckBoxCtrl>("chk_files")->get();
            return c;
        }

        void onImport()
        {
            const Source* source = selected();
            if (!source)
            {
                return;
            }
            if (mFirstStart && !sStarted)
            {
                runImport(*source, choice());       // nothing of ours to overwrite yet
                return;
            }

            LLStringUtil::format_map_t args;
            args["[NAME]"] = source->name;
            LLHandle<LLFloater> handle = getHandle();
            const Source chosen_source = *source;
            const Choice chosen = choice();
            LLNotificationsUtil::add("GenericAlertYesCancel", LLSD().with("MESSAGE", getString("confirm", args)), LLSD(),
                                     [handle, chosen_source, chosen](const LLSD& notification, const LLSD& response)
                                     {
                                         ImportFloater* self = static_cast<ImportFloater*>(handle.get());
                                         if (self && LLNotificationsUtil::getSelectedOption(notification, response) == 0)
                                         {
                                             self->runImport(chosen_source, chosen);
                                         }
                                     });
        }

        void runImport(const Source& source, const Choice& chosen);
        static void startImport(const Source& source, const Choice& chosen, ImportFloater* self);

        LLScrollListCtrl* mList = nullptr;
        std::vector<Source> mSources;
        bool mFirstStart = false;
        std::string mGraphicsFor;       // the viewer the graphics box was last set for (its folder)
        bool mGraphicsSet = false;
    };

    ImportFloater* shownFloater()
    {
        return LLFloaterReg::findTypedInstance<ImportFloater>(FLOATER_NAME);
    }

    void ImportFloater::runImport(const Source& source, const Choice& chosen)
    {
        if (sStarting)
        {
            return;     // one pressed already starts on the next frame
        }
        if (chosen.graphics)
        {
            // The graphics settings go in at once and the shaders are built again after them
            // (importer.cpp): the viewer stands still for a while. Say so first, and start
            // when that has been drawn (the frame after this one).
            sStarting = true;
            getChild<LLUICtrl>("status")->setValue(getString("applying"));
            onSelect();
            LLHandle<LLFloater> handle = getHandle();
            doOnIdleRepeating([handle, source, chosen, frames = 0]() mutable
            {
                if (LLApp::isExiting())
                {
                    return true;
                }
                if (++frames < 2)
                {
                    return false;
                }
                sStarting = false;
                startImport(source, chosen, static_cast<ImportFloater*>(handle.get()));
                return true;
            });
            return;
        }
        startImport(source, chosen, this);
    }

    // Starts it; the window (if it is still open) shows how it goes.
    // fix-round68 4C: between the start of a login (STATE_LOGIN_CLEANUP) and its end
    // (STATE_STARTED) the account's settings are read into this session already, but it does
    // not count as logged in (importer.cpp's isLoggedIn()): its file would be written, then
    // saved over from memory at the end of the session. Refused then; at the login screen
    // or once in-world it goes.
    bool loginInProgress()
    {
        const EStartupState state = LLStartUp::getStartupState();
        return state >= STATE_LOGIN_CLEANUP && state < STATE_STARTED;
    }

    void ImportFloater::startImport(const Source& source, const Choice& chosen, ImportFloater* self)
    {
        if (loginInProgress())
        {
            if (self)
            {
                self->getChild<LLUICtrl>("status")->setValue(self->getString("logging_in"));
                self->onSelect();
            }
            return;
        }
        // The copying goes on when this window is closed: it reports to the window open then.
        const std::string name = source.name;
        const bool started = start(source, chosen,
                   [](U32 copied, U32 total)
                   {
                       if (ImportFloater* floater = shownFloater())
                       {
                           floater->onProgress(copied, total);
                       }
                   },
                   [name](const Result& result)
                   {
                       if (ImportFloater* floater = shownFloater())
                       {
                           floater->onDone(name, result);
                       }
                   });
        if (started)
        {
            sStarted = true;    // what is done is done: a second press asks first (in any window)
        }
        if (!self)
        {
            return;
        }
        if (!started)
        {
            self->getChild<LLUICtrl>("status")->setValue(self->getString("busy"));
            self->onSelect();
            return;
        }
        self->mFirstStart = false;
        self->onSelect();
    }

    void showAtLoginScreen()
    {
        doOnIdleRepeating([]()
        {
            if (LLApp::isExiting())
            {
                return true;
            }
            const EStartupState state = LLStartUp::getStartupState();
            if (state != STATE_LOGIN_WAIT && state != STATE_STARTED)
            {
                return false;
            }
            if (findSources().empty())
            {
                LL_INFOS("ImportSettings") << "First start: no other viewer to bring settings over from" << LL_ENDL;
                gSavedSettings.setBOOL(SETTING_PENDING, false);
            }
            else
            {
                LLFloaterReg::showInstance(FLOATER_NAME);
            }
            return true;
        });
    }

    void init()
    {
        // settings.xml is written at the end of a session: without one, this install has never run
        const std::string settings_file = gSavedSettings.getString("ClientSettingsFile");
        if (!settings_file.empty() && !gDirUtilp->fileExists(settings_file))
        {
            gSavedSettings.setBOOL(SETTING_PENDING, true);
        }
        plus::importsettings::rememberThisInstall();

        LLFloaterReg::add(FLOATER_NAME, "floater_plus_import_settings.xml",
                          (LLFloaterBuildFunc)&LLFloaterReg::build<ImportFloater>);
        LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Plus.ImportSettings.Open",
            [](LLUICtrl*, const LLSD&) { LLFloaterReg::showInstance(FLOATER_NAME); });
    }

    void onUIReady()
    {
        if (gSavedSettings.getBOOL(SETTING_PENDING))
        {
            showAtLoginScreen();
        }
    }

    plus::FeatureRegistration sRegistration({ "importsettings", &init, &onUIReady });
}

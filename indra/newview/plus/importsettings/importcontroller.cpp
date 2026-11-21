/**
 * @file importcontroller.cpp
 * @brief The bring-settings-over window's work, apart from how it is shown (research/386).
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

// See importcontroller.h. The code is what importsettings.cpp's window class held, with the check boxes, the list and the
// texts behind a View.

#include "llviewerprecompiledheaders.h"

#include "importcontroller.h"

#include "llapp.h"
#include "llcallbacklist.h"
#include "lldir.h"
#include "lldirpicker.h"
#include "llnotificationsutil.h"
#include "llstartup.h"
#include "llviewercontrol.h"

#include <algorithm>
#include <ctime>

namespace plus::importsettings
{
    namespace
    {
        constexpr char SETTING_PENDING[] = "PlusImportSettingsPending";
        // Pressed: starts on the next frame (runImport). Not the window's: one opened again meanwhile
        // does not start it a second time.
        bool sStarting = false;
        // Started this session (from any window): what is done is done, and a press after it asks
        // first (ours would be overwritten). Not the window's: one opened again while it starts or
        // runs knows it too (fix-round10 5).
        bool sStarted = false;
        // The window that is open (there is one at most: single_instance): the copying reports to it.
        ImportController* sOpen = nullptr;

        // Between the start of a login (STATE_LOGIN_CLEANUP) and its end (STATE_STARTED) the account's settings are
        // read into this session already, but it does not count as logged in (importer.cpp's isLoggedIn()): its file
        // would be written, then saved over from memory at the end of the session. Refused then; at the login screen
        // or once in-world it goes (fix-round68 4C).
        bool loginInProgress()
        {
            const EStartupState state = LLStartUp::getStartupState();
            return state >= STATE_LOGIN_CLEANUP && state < STATE_STARTED;
        }
    }

    std::string ImportController::dateText(std::time_t time)
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

    ImportController::ImportController(View& view) : mView(view)
    {
        sOpen = this;
    }

    ImportController::~ImportController()
    {
        if (sOpen == this)
        {
            sOpen = nullptr;
        }
    }

    void ImportController::open()
    {
        mFirstStart = gSavedSettings.getBOOL(SETTING_PENDING) && !sStarted;
        const LLStringUtil::format_map_t none;
        if (mFirstStart)
        {
            mView.setIntro(mView.text("intro_first", none));
            mView.setCloseLabel(mView.text("not_now", none));
        }
        mSources = findSources();
        refreshList(0);
        mView.setStatus(running() ? mView.text("busy", none) : sStarting ? mView.text("applying", none)
                        : mSources.empty() ? mView.text("none", none) : std::string());
    }

    void ImportController::onClose(bool app_quitting)
    {
        if (!app_quitting)      // left open at the end: offered again at the next start
        {
            gSavedSettings.setBOOL(SETTING_PENDING, false);
        }
    }

    // The files being copied (started by this window, or by one closed since).
    void ImportController::onProgress(U32 copied, U32 total)
    {
        LLStringUtil::format_map_t args;
        args["[COPIED]"] = llformat("%u", copied);
        args["[TOTAL]"] = llformat("%u", total);
        mView.setStatus(mView.text("copying", args));
        onSelect();
    }

    void ImportController::onDone(const std::string& name, const Result& result)
    {
        LLStringUtil::format_map_t args;
        args["[NAME]"] = name;
        args["[SETTINGS]"] = llformat("%u", result.settings);
        args["[FILES]"] = llformat("%u", result.files);
        args["[KEPT]"] = llformat("%u", result.kept);
        args["[OVER]"] = llformat("%u", result.over_limit);
        args["[ERRORS]"] = llformat("%u", result.errors);
        std::string text = mView.text("done", args);
        if (result.keys)
        {
            text += " " + mView.text("done_keys", args);
        }
        if (result.kept > 0)
        {
            text += " " + mView.text("done_kept", args);
        }
        if (result.over_limit > 0)
        {
            text += " " + mView.text("done_over", args);
        }
        if (result.errors > 0)
        {
            text += " " + mView.text("done_errors", args);
        }
        const LLStringUtil::format_map_t none;
        text += "\n" + mView.text("restart", none);
        mView.setStatus(text);
        onSelect();
    }

    void ImportController::refreshList(size_t selected)
    {
        mView.setSources(mSources, mSources.empty() ? 0 : std::min(selected, mSources.size() - 1));
        onSelect();
    }

    const Source* ImportController::selected() const
    {
        const int index = mView.selectedSource();
        return index >= 0 && index < (int)mSources.size() ? &mSources[index] : nullptr;
    }

    void ImportController::onSelect()
    {
        const Source* source = selected();
        mView.setImportEnabled(source != nullptr && !running() && !sStarting);
        const std::string root = source ? source->root : std::string();
        if (!mGraphicsSet || root != mGraphicsFor)   // a new choice of viewer, not a refresh while copying
        {
            mGraphicsSet = true;
            mGraphicsFor = root;
            mView.setGraphicsChecked(source && source->family == Family::CHARIS);
        }
    }

    void ImportController::browse()
    {
        std::weak_ptr<char> alive = mAlive;
        (new LLDirPickerThread([alive, this](const std::vector<std::string>& folders, std::string)
                               {
                                   if (!alive.lock())
                                   {
                                       return;      // the window went while the picker was open
                                   }
                                   if (!folders.empty() && !folders[0].empty())
                                   {
                                       onPicked(folders[0]);
                                   }
                               },
                               gDirUtilp->getOSUserDir()))->getFile();
    }

    void ImportController::onPicked(const std::string& folder)
    {
        Source source;
        if (!sourceFromFolder(folder, source))
        {
            LLStringUtil::format_map_t args;
            args["[FOLDER]"] = folder;
            mView.setStatus(mView.text("not_viewer_folder", args));
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
        mView.setStatus(std::string());
    }

    void ImportController::onImport()
    {
        const Source* source = selected();
        if (!source)
        {
            return;
        }
        if (mFirstStart && !sStarted)
        {
            runImport(*source, mView.choice());       // nothing of ours to overwrite yet
            return;
        }

        LLStringUtil::format_map_t args;
        args["[NAME]"] = source->name;
        std::weak_ptr<char> alive = mAlive;
        const Source chosen_source = *source;
        const Choice chosen = mView.choice();
        LLNotificationsUtil::add("GenericAlertYesCancel", LLSD().with("MESSAGE", mView.text("confirm", args)), LLSD(),
                                 [alive, this, chosen_source, chosen](const LLSD& notification, const LLSD& response)
                                 {
                                     if (alive.lock() && LLNotificationsUtil::getSelectedOption(notification, response) == 0)
                                     {
                                         runImport(chosen_source, chosen);
                                     }
                                 });
    }

    void ImportController::runImport(const Source& source, const Choice& chosen)
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
            const LLStringUtil::format_map_t none;
            mView.setStatus(mView.text("applying", none));
            onSelect();
            std::weak_ptr<char> alive = mAlive;
            doOnIdleRepeating([alive, this, source, chosen, frames = 0]() mutable
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
                startImport(source, chosen, alive.lock() ? this : nullptr);
                return true;
            });
            return;
        }
        startImport(source, chosen, this);
    }

    // Starts it; the window (if it is still open) shows how it goes.
    void ImportController::startImport(const Source& source, const Choice& chosen, ImportController* self)
    {
        const LLStringUtil::format_map_t none;
        if (loginInProgress())
        {
            if (self)
            {
                self->mView.setStatus(self->mView.text("logging_in", none));
                self->onSelect();
            }
            return;
        }
        // The copying goes on when this window is closed: it reports to the window open then.
        const std::string name = source.name;
        const bool started = start(source, chosen,
                   [](U32 copied, U32 total)
                   {
                       if (sOpen)
                       {
                           sOpen->onProgress(copied, total);
                       }
                   },
                   [name](const Result& result)
                   {
                       if (sOpen)
                       {
                           sOpen->onDone(name, result);
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
            self->mView.setStatus(self->mView.text("busy", none));
            self->onSelect();
            return;
        }
        self->mFirstStart = false;
        self->onSelect();
    }
}

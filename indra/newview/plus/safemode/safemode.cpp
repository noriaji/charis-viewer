/**
 * @file safemode.cpp
 * @brief Safe mode (research/254): the log lines of a safe start, the
 *        "starting" marker that tells the next start whether this one got
 *        through, "start in safe mode next time too", and what a safe start
 *        does once the UI is there (the badge, the environment).
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

// The marker, charis_starting.marker in the logs folder, holds the number of
// starts in a row that have not got through, this one included. It is
// written by the feature's init (before the window and the Vulkan device are
// made) and removed when the start has got through: the world shown for
// WORLD_SECONDS, or a normal quit (LLApp's status "quitting", from the login
// screen too). A crash or a kill before that leaves it, and the next start
// reads the count: in wWinMain, to ask whether to start safe after two
// (safemode_launch.cpp, which then removes it), and here
// (unfinishedStartsBefore()). A safe start reads it but neither counts nor
// removes it: the count is about normal starts, and a safe mode that ends on
// the way must not bring the question back. Its own file rather than the
// official exec marker: that one covers the whole session, and a setting is
// saved only at a normal end.
//
// "Start in safe mode next time too" is a file too (safemode_files.h), for the
// same reason: the next start reads it in wWinMain, before the settings.

#include "llviewerprecompiledheaders.h"

#include "safemode.h"
#include "safemode_badge.h"
#include "safemode_files.h"
#include "safemode_table.h"

#include "llappviewer.h"
#include "llcallbacklist.h"
#include "lldir.h"
#include "llevents.h"
#include "llfile.h"
#include "llstartup.h"
#include "llnotificationsutil.h"
#include "lluictrl.h"

#include "plus.h"

#include <iterator>
#include <mutex>

namespace
{
    constexpr F32 WORLD_SECONDS = 30.f;     // in the world this long: the start got through

    std::mutex sMarkerMutex;                // the LLApp status can come from another thread
    std::string sMarkerPath;                // empty: no marker of this start (left, or never written)
    S32 sUnfinishedBefore = 0;

    // The count a marker left by an earlier start holds (safemode_files.h).
    S32 readCount(const std::string& path)
    {
        llifstream in(path);
        return plus::safemode::files::readStartingCount(in);
    }

    bool writeCount(const std::string& path, S32 count)
    {
        llofstream out(path, std::ios::out | std::ios::trunc);
        out << count << '\n';
        out.close();
        return !out.fail();
    }

    void removeMarker(const char* why)
    {
        std::lock_guard<std::mutex> lock(sMarkerMutex);
        if (sMarkerPath.empty())
        {
            return;
        }
        LLFile::remove(sMarkerPath, ENOENT);
        sMarkerPath.clear();
        LL_INFOS("SafeMode") << "The start got through (" << why << "): starting marker removed" << LL_ENDL;
    }

    bool onStartupState(const LLSD& info)
    {
        if (info["enum"].asInteger() == STATE_STARTED)
        {
            doAfterInterval([]() { removeMarker("in the world"); }, WORLD_SECONDS);
        }
        return false;
    }

    bool onAppStatus(const LLSD& info)
    {
        if (info["status"].asString() == "quitting")
        {
            removeMarker("quit");
        }
        return false;
    }

    void writeMarker()
    {
        if (LLAppViewer::instance()->isSecondInstance())
        {
            return;     // the logs folder is the other instance's
        }
        const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_LOGS, plus::safemode::files::STARTING_MARKER);
        sUnfinishedBefore = LLFile::isfile(path) ? readCount(path) : 0;
        if (plus::safemode::active() || plus::safemode::unattended())
        {
            // Not counted: a safe start that ends on the way must not bring the question back at the
            // next start (it is about the normal starts, by the user), and the count of those stays as
            // it was. Nor is a tool's start (a smoke test that kills the viewer).
            if (sUnfinishedBefore > 0)
            {
                LL_INFOS("SafeMode") << sUnfinishedBefore << " normal start(s) in a row before did not get through; "
                                     << (plus::safemode::active() ? "a safe start" : "an unattended start")
                                     << " does not count" << LL_ENDL;
            }
            return;
        }
        if (!writeCount(path, llmin(sUnfinishedBefore + 1, plus::safemode::files::STARTING_MARKER_MAX)))
        {
            LL_WARNS("SafeMode") << "Could not write the starting marker " << path << LL_ENDL;
            return;
        }
        {
            std::lock_guard<std::mutex> lock(sMarkerMutex);
            sMarkerPath = path;
        }
        LLEventPumps::instance().obtain("StartupState").listen("PlusSafeMode", &onStartupState);
        LLEventPumps::instance().obtain("LLApp").listen("PlusSafeMode", &onAppStatus);
        if (sUnfinishedBefore > 0)
        {
            LL_WARNS("SafeMode") << sUnfinishedBefore << " start(s) in a row before this one did not get through" << LL_ENDL;
        }
    }

    void logMode()
    {
        if (plus::safemode::active())
        {
            LL_INFOS("SafeMode") << "Safe start (" << plus::safemode::reason() << "): " << std::size(plus::safemode::ENV_SWITCHES)
                                 << " LLRHI switches and " << std::size(plus::safemode::SETTINGS) << " settings off (safemode_table.h)"
                                 << LL_ENDL;
            if (!plus::safemode::inJobEndingWithFirst())
            {
                LL_WARNS("SafeMode") << "Not in the job of the process that started this one (it could not be made): "
                                     << "this one goes on if that one is killed" << LL_ENDL;
            }
        }
        else if (plus::safemode::relaunchError())
        {
            LL_WARNS("SafeMode") << "Safe mode was asked for, but the safe start could not be made (Windows error "
                                 << plus::safemode::relaunchError() << "): a normal start" << LL_ENDL;
        }
        const plus::safemode::Question& question = plus::safemode::question();
        if (question.error)
        {
            LL_WARNS("SafeMode") << question.after << " start(s) in a row had not got through, but the starting marker could not be "
                                 << "removed (Windows error " << question.error << "): not asked whether to start in safe mode"
                                 << LL_ENDL;
        }
        else if (question.asked && !question.yes)
        {
            LL_INFOS("SafeMode") << question.after << " start(s) in a row had not got through: asked whether to start in safe "
                                 << "mode, the answer was no" << LL_ENDL;
        }
        if (plus::safemode::unattended() && !plus::safemode::active() && plus::safemode::nextStartSafe())
        {
            LL_INFOS("SafeMode") << "CHARIS_SAFE_MODE_NO_PROMPT is set: \"start in safe mode next time too\" is on but not "
                                 << "followed (a normal start)" << LL_ENDL;
        }
    }

    // "Start in safe mode next time too": the file's path, and whether it is there (read once at init,
    // kept up to date by setNextStartSafe(); the menu asks every time it is opened).
    std::string sNextStartPath;
    bool sNextStartSafe = false;
    bool sEarlyFilesFound = false;

    // The next start finds the user folder before gDirUtilp has set it up (safemode_launch.cpp):
    // the two ways must agree, or the files would be written where that start does not look.
    void checkEarlyFiles()
    {
        const std::string early = plus::safemode::files::userAppDirBeforeInit();
        const std::string& here = gDirUtilp->getOSUserAppDir();
        sEarlyFilesFound = !early.empty() && LLStringUtil::compareInsensitive(early, here) == 0;
        if (!sEarlyFilesFound)
        {
            LL_WARNS("SafeMode") << "The user folder found before the start (" << early << ") is not " << here
                                 << ": \"start in safe mode next time too\" and the question after starts that did not "
                                 << "get through are off" << LL_ENDL;
        }
        sNextStartPath = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, plus::safemode::files::NEXT_START_SAFE);
        sNextStartSafe = LLFile::isfile(sNextStartPath);
    }

    // Tells the user why "start in safe mode next time too" did not change (an alert; text of
    // panel_plus_safemode.xml with [FILE]).
    void tellNextStartFailed(const char* text_name)
    {
        LLSD args;
        args["MESSAGE"] = plus::safemode::uiText(text_name, { { "[FILE]", sNextStartPath } });
        LLNotificationsUtil::add("GenericAlert", args);
    }

    void registerMenuCallbacks()
    {
        // Preferences > Charis: skins/default/xui/<lang>/panel_preferences_charis.xml (c_safe_mode_next;
        // plus/prefscharis reads Is and Choosable).
        LLUICtrl::EnableCallbackRegistry::defaultRegistrar().add("Plus.SafeMode.NextStart.Is",
            [](LLUICtrl*, const LLSD&) { return plus::safemode::nextStartSafe(); });
        LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Plus.SafeMode.NextStart.Toggle",
            [](LLUICtrl*, const LLSD&) { plus::safemode::setNextStartSafe(!plus::safemode::nextStartSafe()); });
        LLUICtrl::EnableCallbackRegistry::defaultRegistrar().add("Plus.SafeMode.NextStart.Choosable",
            [](LLUICtrl*, const LLSD&) { return plus::safemode::earlyFilesFound(); });
        LLUICtrl::EnableCallbackRegistry::defaultRegistrar().add("Plus.SafeMode.Never",
            [](LLUICtrl*, const LLSD&) { return false; });
    }

    void init()
    {
        checkEarlyFiles();  // before logMode(): it tells of "next time too" not followed
        logMode();
        writeMarker();
        registerMenuCallbacks();
    }

    // At the login screen: the window, the Vulkan device and the GPU benchmark (a subprocess that is
    // to run safe too) are made.
    void onUIReady()
    {
        if (!plus::safemode::active())
        {
            return;
        }
        plus::safemode::forgetSafeEnvironment();
        plus::safemode::badge::attach();
    }

    plus::FeatureRegistration sRegistration({ "safemode", &init, &onUIReady });
}

int plus::safemode::unfinishedStartsBefore()
{
    return sUnfinishedBefore;
}

bool plus::safemode::nextStartSafe()
{
    return sNextStartSafe;
}

void plus::safemode::setNextStartSafe(bool on)
{
    if (on == sNextStartSafe || sNextStartPath.empty() || !sEarlyFilesFound)
    {
        return;
    }
    if (on)
    {
        llofstream out(sNextStartPath, std::ios::out | std::ios::trunc);
        out << "Charis Viewer: every start is a safe start while this file is here (research/254).\n";
        out.close();
        if (out.fail())
        {
            // A part of it may be there, and its being there is what counts: not left half on.
            LLFile::remove(sNextStartPath, ENOENT);
            LL_WARNS("SafeMode") << "Could not write " << sNextStartPath << LL_ENDL;
            if (LLFile::isfile(sNextStartPath))
            {
                sNextStartSafe = true;  // as the file says: the next start is a safe one
                tellNextStartFailed("next_start_not_removed");
            }
            else
            {
                tellNextStartFailed("next_start_not_written");
            }
            return;
        }
    }
    else
    {
        LLFile::remove(sNextStartPath, ENOENT);     // gone already (removed by hand) is fine too
        if (LLFile::isfile(sNextStartPath))
        {
            LL_WARNS("SafeMode") << "Could not remove " << sNextStartPath << LL_ENDL;
            tellNextStartFailed("next_start_not_removed");
            return;
        }
    }
    sNextStartSafe = on;
    LL_INFOS("SafeMode") << "Start in safe mode next time too: " << (on ? "on" : "off") << LL_ENDL;
}

bool plus::safemode::earlyFilesFound()
{
    return sEarlyFilesFound;
}

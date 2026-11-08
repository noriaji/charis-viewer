/**
 * @file safemode_files.h
 * @brief Safe mode (research/254): the two small files a start reads before
 *        anything of the viewer is made, and where they are.
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

#ifndef PLUS_SAFEMODE_FILES_H
#define PLUS_SAFEMODE_FILES_H

#include <istream>
#include <string>

// Both files are written by the viewer once its folders are set up
// (safemode.cpp, through gDirUtilp) and read again by the next start in
// wWinMain, before LLAppViewer exists (safemode_launch.cpp). That start finds
// the user folder the way LLDir_Win32::initAppDirs() will make it
// (userAppDirBeforeInit()); the feature's init checks that both ways agree
// and says so in the log when they do not (then the setting and the offer
// are off: earlyFilesFound()).
namespace plus::safemode::files
{
    // Folders under the user folder, as lldir.cpp names them.
    inline constexpr char LOGS_FOLDER[] = "logs";               // LL_PATH_LOGS
    inline constexpr char SETTINGS_FOLDER[] = "user_settings";  // LL_PATH_USER_SETTINGS

    // In LOGS_FOLDER: the starts in a row that have not got through (a number), while a start goes on.
    inline constexpr char STARTING_MARKER[] = "charis_starting.marker";
    inline constexpr int STARTING_MARKER_MAX = 99;     // a hand-edited marker cannot make the count larger
    // From this many starts in a row that did not get through, the next start asks whether to start safe;
    // a start that went into safe mode by itself (--safe, Shift, next time too) forgets such a row.
    inline constexpr int OFFER_AFTER = 2;

    // The count of a starting marker that is there, read from it: 1..STARTING_MARKER_MAX (1 when it
    // cannot be read: it is there all the same).
    inline int readStartingCount(std::istream& in)
    {
        int count = 0;
        if (!(in >> count))
        {
            return 1;
        }
        return count < 1 ? 1 : count > STARTING_MARKER_MAX ? STARTING_MARKER_MAX : count;
    }

    // In SETTINGS_FOLDER: there, every start is a safe start ("start in safe mode next time too").
    inline constexpr char NEXT_START_SAFE[] = "charis_safe_mode_next_start.flag";

    // The user folder (UTF-8) as LLDir_Win32::initAppDirs() will set it up: the OS
    // folder LLDir_Win32 found (a static object made before wWinMain) with
    // plus::independence::APP_DIR_NAME, or a portable install's own folder. No
    // logging, nothing made. Empty when it cannot be told.
    std::string userAppDirBeforeInit();
}

#endif // PLUS_SAFEMODE_FILES_H

/**
 * @file importsettings.h
 * @brief Bringing settings over from another viewer on this computer: the
 *        official viewer, Firestorm and other viewers of the same family, and
 *        other Charis Viewer installs (research/143).
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

#ifndef PLUS_IMPORTSETTINGS_H
#define PLUS_IMPORTSETTINGS_H

#include "stdtypes.h"

#include <ctime>
#include <functional>
#include <string>
#include <vector>

namespace plus::importsettings
{
    enum class Family
    {
        OFFICIAL,       // the Second Life viewer
        FIRESTORM,
        OTHER,          // Black Dragon, Alchemy, ... or a folder the user picked
        CHARIS          // another install of this viewer (its own settings, skin and graphics are ours too)
    };

    // A viewer's folder to bring settings over from: the one that holds user_settings\ and
    // the account folders (%APPDATA%\SecondLife, %APPDATA%\Firestorm_x64, ...).
    struct Source
    {
        Family family = Family::OTHER;
        std::string name;               // "Second Life", "Firestorm", "Charis Viewer", ...
        std::string root;
        std::time_t last_used = 0;      // when its settings.xml was last written; 0: not known
    };

    // ---- sources.cpp -------------------------------------------------------

    // The viewers found on this computer, the most recently used first. This
    // install's own folder is never one of them.
    std::vector<Source> findSources();

    // A folder the user picked: the viewer's folder itself, or its user_settings.
    // False when it holds no settings.xml or is this install's own folder.
    bool sourceFromFolder(const std::string& folder, Source& source);

    // Notes this install's folder in the registry, so that other Charis Viewer
    // installs (a portable one keeps its folder next to itself) find it. Every start.
    void rememberThisInstall();

    // ---- importer.cpp ------------------------------------------------------

    // What to bring over.
    struct Choice
    {
        bool general = true;    // settings.xml (but the graphics), the warnings not to show again, the colors
        bool graphics = false;  // the Render* settings (and the Charis Viewer's own), the window, the graphics presets
        bool keys = true;       // key_bindings.xml (ours kept as key_bindings_before_import_<date>.xml)
        bool accounts = true;   // settings_per_account.xml and the other files of each account
        bool chat = true;       // chat and IM transcripts
        bool files = true;      // user_settings folders (poses, environments, dictionaries, presets) and favorites
    };

    struct Result
    {
        U32 settings = 0;       // values that changed
        bool keys = false;      // the key bindings were replaced
        U32 files = 0;          // files copied
        U32 kept = 0;           // files this install had already (left as they were)
        U32 over_limit = 0;     // files not copied: too big, or past the limit of one bringing over
        U32 errors = 0;
        bool stopped = false;   // the viewer quit before the files were all copied
        std::vector<std::string> notes;     // details, for the log
    };

    // Brings the chosen settings over from the source, into this session (settings
    // take effect now; some of them only at the next start) and into this install's
    // folder. The source is only read. Main thread.
    // The settings, the key bindings and the accounts' own files are
    // done at once; the chat logs and the other saved files (thousands, for some) are
    // copied a few each frame, so the window goes on drawing: progress(copied, total)
    // is called as they go, done(result) at the end (also when there was nothing to
    // copy). False, and nothing done, while an earlier one is still copying.
    bool start(const Source& source, const Choice& choice,
               std::function<void(U32 copied, U32 total)> progress,
               std::function<void(const Result& result)> done);

    // An earlier start() is still copying files.
    bool running();
}

#endif // PLUS_IMPORTSETTINGS_H

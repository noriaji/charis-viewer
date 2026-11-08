/**
 * @file release.h
 * @brief What the downloadable build (LL_RELEASE_FOR_DOWNLOAD) does at
 *        startup that a developer build does not.
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

#ifndef PLUS_RELEASE_H
#define PLUS_RELEASE_H

// The downloadable build is the one tools/build-release.sh makes: CMake option
// CHARIS_RELEASE_FOR_DOWNLOAD (00-Common.cmake) defines LL_RELEASE_FOR_DOWNLOAD,
// as the official Release configuration does (research/138 section 8).
//
// A developer build (no LL_RELEASE_FOR_DOWNLOAD) turns QAMode on at every
// start (llappviewer.cpp initConfiguration()), which shows the Develop and
// Debug menus. Developer builds before fix-release-flags saved that QAMode=1 in
// the user's settings.xml (QAMode is a persisted setting; since then a developer
// build does not persist it at all), and the settings folder
// is the same for both kinds of builds, so a downloadable build started on
// such settings would show the menus too. clearDeveloperQAModeOnce() turns
// QAMode off once, the first time a downloadable build starts on a settings
// folder; after that QAMode is the user's own choice again (Ctrl+Alt+Q, the
// preferences, --qa), as in the official viewer.
//
// Hook:
//   llappviewer.cpp   initConfiguration(): right after the user's settings.xml
//                     is read, before the command line is applied (so --qa
//                     still turns it on for that start)

namespace plus::release
{
    // Downloadable build only (a no-op in a developer build): the first time,
    // turns a saved QAMode off and remembers that it did (PlusReleaseQAModeCleared,
    // settings_plus.xml).
    void clearDeveloperQAModeOnce();
}

#endif // PLUS_RELEASE_H

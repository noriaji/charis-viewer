/**
 * @file version.h
 * @brief Charis Viewer's own version number, apart from the number of the
 *        official Second Life viewer it is based on.
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

#ifndef PLUS_VERSION_H
#define PLUS_VERSION_H

// Two numbers:
//
//   Charis      MAJOR.MINOR.PATCH below, and Charis's own build number
//               (LL_VIEWER_VERSION_BUILD: the commits since Charis branched
//               from the official viewer, buildnumber.cmake; it goes up with
//               every commit). LLVersionInfo returns these, so everything
//               that sends or stores the viewer version (the login request,
//               user agents, the exec markers, LastRunVersion, crash data)
//               sees "0.1.0.<build>": four numbers, as the official viewer.
//   Base        the official release this source is merged from: the channel
//               name BASE_CHANNEL below and the number of
//               indra/newview/VIEWER_VERSION.txt (official file, left as it
//               is: an upstream merge brings the new number with it).
//
// What people read (About, the window title at start, the log) is displayVersion():
//   "Charis Viewer 0.1.0 alpha 1 (rev 1967) based on Second Life Release 26.5.0"
// The window title once the world is up keeps the shorter shortTitle() in
// front of what it says about the session (setTitlePlace(), setTitleStatus()).
//
// Included by official files (llversioninfo.cpp, llappviewer.cpp,
// llvvmquery.cpp, llteleporthistory.cpp, pipeline.cpp): constants and
// declarations only.

#include <string>

namespace plus::version
{
    // ---- Set these for each Charis release (the only place) ------------

    inline constexpr int MAJOR = 0;
    inline constexpr int MINOR = 1;
    inline constexpr int PATCH = 0;

    // Pre-release stage shown after the number ("alpha", "beta", "rc"),
    // and its number: "alpha 1". STAGE "" for a release (nothing shown);
    // STAGE_NUMBER 0 shows the stage without a number ("beta").
    inline constexpr char STAGE[] = "alpha";
    inline constexpr int STAGE_NUMBER = 2;

    // ---------------------------------------------------------------------

    // Name in front of the shown version (the channel, plus::independence::CHANNEL,
    // can be changed from the command line; this cannot).
    inline constexpr char PRODUCT_NAME[] = "Charis Viewer";

    // The official viewer this build is based on (the number is VIEWER_VERSION.txt).
    inline constexpr char BASE_CHANNEL[] = "Second Life Release";

    // "0.1.0"
    std::string shortVersion();

    // "alpha 1", "beta", or "" for a release
    std::string stage();

    // "26.5.0": the official version this build is based on (VIEWER_VERSION.txt)
    std::string baseVersion();

    // "Charis Viewer 0.1.0 alpha 1 (rev 1967) based on Second Life Release 26.5.0"
    std::string displayVersion();

    // The same with another name in front (the window title uses APP_NAME
    // of strings.xml; it is "Charis Viewer" in every language).
    std::string displayVersion(const std::string& name);

    // "Charis Viewer 0.1.0 alpha 1 (rev 1967)": the version the window
    // title always keeps (the base release is left out: too long for it).
    std::string shortTitle(const std::string& name);

    // The main window's title after the start: shortTitle(APP_NAME), then
    // " - " and the place, then " - " and the status, each when not empty:
    //   "Charis Viewer 0.1.0 alpha 1 (rev N) - user.name @ Region"
    //   "Charis Viewer 0.1.0 alpha 1 (rev N) - loading nearby first (40m / 256m)"
    // Place: "user @ region" (llteleporthistory.cpp, when UpdateAppWindowTitleBar
    // is on). Status: the nearby-first loading (LLPipeline::updateLoadRadius()).
    // Each call keeps the other's part. Main thread.
    void setTitlePlace(const std::string& place);
    void setTitleStatus(const std::string& status);
}

// Updates: the official viewer asks the Second Life update service (VVM) at
// start; Charis never does (the <plus> hook in initVVMUpdateCheck(),
// llvvmquery.cpp), so an official update is never offered and the official
// service never sees Charis numbers. Charis's own check (plus/updatecheck)
// reads a small file from our site and compares it with the numbers above;
// it does not install anything.

#endif // PLUS_VERSION_H

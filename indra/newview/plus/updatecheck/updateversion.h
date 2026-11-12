/**
 * @file updateversion.h
 * @brief Update check: Charis version numbers read from the update file,
 *        compared with the running version (no viewer dependencies).
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

// Order of two versions (research/123-update-check.md, section 2):
//   MAJOR.MINOR.PATCH, then the stage (alpha < beta < rc < release),
//   then the stage number, then the build number (only when both have one).
// Standard library only, so updateversion_test.cpp can test it alone.

#ifndef PLUS_UPDATECHECK_UPDATEVERSION_H
#define PLUS_UPDATECHECK_UPDATEVERSION_H

#include <cstdint>
#include <string>
#include <string_view>

namespace plus::updatecheck
{
    enum StageRank : int
    {
        STAGE_UNKNOWN = -1,
        STAGE_ALPHA   = 1,
        STAGE_BETA    = 2,
        STAGE_RC      = 3,
        STAGE_RELEASE = 4,  // no stage ("")
    };

    struct Version
    {
        int      major        = 0;
        int      minor        = 0;
        int      patch        = 0;
        int      stage_rank   = STAGE_RELEASE;
        int      stage_number = 0;      // "alpha 2": 2; 0 = none
        uint64_t build        = 0;      // 0 = not given (then not compared)
    };

    // "alpha", "beta", "rc" (any case), "" for a release; STAGE_UNKNOWN otherwise.
    int stageRank(std::string_view stage);

    // "alpha", "beta", "rc" or "" (release).
    const char* stageName(int rank);

    // "0.1.0": exactly three numbers (0..99999) with dots, nothing else.
    bool parseNumbers(std::string_view text, int& major, int& minor, int& patch);

    // < 0 when a is older than b, 0 when the same, > 0 when a is newer.
    int compare(const Version& a, const Version& b);

    // Whether v's MAJOR.MINOR.PATCH is below major.minor.patch (the stage and
    // the build do not count: min_version "0.1.0" accepts 0.1.0 alpha 1).
    bool numbersBelow(const Version& v, int major, int minor, int patch);

    // "0.1.0": the three numbers alone (in key(), displayText() and the copy of the update file).
    std::string numbersText(int major, int minor, int patch);

    // "0.1.0-alpha.2+262911132": what PlusUpdateCheckSkipped keeps.
    std::string key(const Version& v);

    // "0.1.0 alpha 2 (rev 1970)": what the window shows (the build
    // number is shown as "rev": the commits since the branch from the official viewer).
    std::string displayText(const Version& v);

    // The installer's version of v (Velopack's packVersion), as plus/version/packversion.py pack_version()
    // makes it for the package: "0.1.0-alpha.2.3300", a release "0.1.0" (its build left out), a stage
    // without a number "0.1.0-beta.0". Without a build (v.build 0): "0.1.0-alpha.2". "" for a stage that
    // is not one (research/326 section 4.2).
    std::string packageVersion(const Version& v);

    // Whether feed (the update feed's version) is agreed (packageVersion() of the version the user agreed
    // to): the same text, or, for agreed without a build ("0.1.0-alpha.2"), that and "." and ASCII digits
    // ("0.1.0-alpha.2.3300"; leading zeros and 0 pass, as the build number is not checked here). A release
    // and a version with a build match only themselves. An empty one matches nothing.
    bool packageVersionMatches(std::string_view feed, std::string_view agreed);

    // A web address that may be fetched or opened (research/130 M3):
    //   - https://, or http:// only for this computer (localhost, 127.0.0.1,
    //     [::1]: a test server);
    //   - a host of ASCII letters, digits, dots and hyphens (or [::1]), then
    //     an optional port of 1 to 5 digits;
    //   - no user name before the host ("https://good.example@evil.example/"
    //     goes to evil.example) and no backslash anywhere;
    //   - printable ASCII only: no spaces, control characters or other
    //     letters (U+202E and the like turn the shown text around); a real
    //     address writes those as %xx. Nor " < > ^ ` { } |;
    //   - at most 2048 bytes.
    // Returns the address with its scheme and host in lower case (the
    // browser's list of allowed schemes compares in lower case), or "" when
    // it is not such an address. Nothing else is fetched or opened.
    std::string webURL(std::string_view url);

    // !webURL(url).empty()
    bool isWebURL(std::string_view url);

    // The host of an address webURL() accepts, in lower case, without the
    // port ("updates.example.org"); "" for any other text.
    std::string urlHost(std::string_view url);

    // Whether host is one of the hosts in list (separated by commas, semicolons
    // or spaces; any case). An empty host or list matches nothing.
    bool hostInList(std::string_view host, std::string_view list);

    // Whether list names no host (empty, or only separators).
    bool hostListEmpty(std::string_view list);

    // Whether a check is due: never checked (last 0), the clock went back,
    // or at least interval seconds since the last check.
    bool dueForCheck(uint32_t now, uint32_t last, uint32_t interval);
}

#endif // PLUS_UPDATECHECK_UPDATEVERSION_H

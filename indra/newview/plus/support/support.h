/**
 * @file support.h
 * @brief Charis's own place for bug reports and release notes, in place of
 *        Linden Lab's (Help > Report Bug, the release notes link in About).
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

#ifndef PLUS_SUPPORT_H
#define PLUS_SUPPORT_H

#include <string>

// The official viewer sends its users to Linden Lab for its own bugs: Help >
// Report Bug opens ReportBugURL (feedback.secondlife.com), and About links to
// releasenotes.secondlife.com/viewer/<version>.html, which has no page for
// Charis's version numbers (research/123 H1, M3). Charis has its own settings
// for both, empty by default: with no place for reports yet, the menu item is
// hidden and nothing opens; setting a URL turns them on.
//
// ReportBugURL is not changed or used: a copy of the official settings may
// hold Linden Lab's address in it.
//
// Hooks:
//   llviewermenu.cpp   handle_report_bug(): reportBug() in place of opening ReportBugURL
//   llappviewer.cpp    getViewerInfo(): VIEWER_RELEASE_NOTES_URL = releaseNotesURL();
//                      getViewerInfoString(): the RELEASE_NOTES_LINE argument of AboutHeader
//   menu_viewer.xml, menu_login.xml (en)   "Report Bug": on_visible Plus.Support.HasReportURL

namespace plus::support
{
    // PlusSupportURL: where to report bugs and ask questions. Empty: none.
    std::string supportURL();

    // Help > Report Bug: opens supportURL() in the system browser, or does
    // nothing (a line in the log) when it is empty.
    void reportBug();

    // PlusReleaseNotesURL: the release notes page linked from About (and shown
    // at start-up after an update, by the official code, for non-test
    // channels). Empty: no link.
    std::string releaseNotesURL();

    // AboutHeader's [RELEASE_NOTES_LINE]: a new line with the link
    // "[url label]", or "" when there is no release notes page.
    std::string releaseNotesLine(const std::string& url, const std::string& label);
}

#endif // PLUS_SUPPORT_H

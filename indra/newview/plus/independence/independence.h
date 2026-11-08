/**
 * @file independence.h
 * @brief Keeps Charis Viewer apart from the official Second Life viewer on the
 *        same computer: its own user folders, cache, single-instance marker,
 *        window class, taskbar identity, channel and installer identity
 *        (research/114, research/311).
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

#ifndef PLUS_INDEPENDENCE_H
#define PLUS_INDEPENDENCE_H

// Only constants and one function: this header is included by official files
// (llappviewer.h, llappviewerwin32.cpp, llvelopack.cpp), so it pulls in nothing.

namespace plus::independence
{
    // Folder name under %APPDATA% (settings, logs, per-account files) and
    // %LOCALAPPDATA% (cache). The official viewer uses "SecondLife".
    inline constexpr char APP_DIR_NAME[] = "CharisViewer";

    // The official viewer's folder name: settings can be brought over from it
    // (plus/importsettings), it is never written.
    inline constexpr char OFFICIAL_APP_DIR_NAME[] = "SecondLife";

    // Win32 window class of the main window. A secondlife:// link handed to a
    // running viewer (LLAppViewerWin32::sendURLToOtherInstance()) goes to the
    // window of this class only, so the two viewers never take each other's links.
    inline constexpr char WINDOW_CLASS[] = "Charis Viewer";

    // Viewer channel: the login server, the HTTP user agent, About and the
    // update service see this name. No "Release"/"Beta"/"Project"/"Test"
    // word; LLVersionInfo::getViewerMaturity() treats this exact name as a
    // release build (fix-round106).
    inline constexpr char CHANNEL[] = "Charis Viewer";

    // Taskbar identity (SetCurrentProcessExplicitAppUserModelID): the taskbar
    // groups and pins Charis apart from the official viewer.
    inline constexpr wchar_t APP_USER_MODEL_ID[] = L"Charis.CharisViewer";

    // NVIDIA driver profile (NvAPI, LLAppViewerWin32): fixed, not the translated APP_NAME.
    // The driver goes by the exe name. Before the rename to CharisViewer.exe
    // (research/227 F4) SecondLifeViewer.exe was in the official viewer's "Second Life"
    // profile; CharisViewer.exe is in none, so the driver's defaults apply.
    // ll_nvapi_init() is not called any more (fix-round106: it failed with
    // NVAPI_EXECUTABLE_ALREADY_IN_USE and never saved); kept for a later decision.
    inline constexpr char DRIVER_PROFILE_NAME[] = "Charis Viewer";

    // Named mutex created at start (debug information only: the real
    // single-instance check is the exec marker in the logs folder, which is
    // ours now that the logs folder is ours).
    inline constexpr wchar_t APP_MUTEX_NAME[] = L"CharisViewerAppMutex";

    // ---- The installer (Velopack, research/311 section 2.3) ----------------
    // The package id: the install folder %LOCALAPPDATA%\<id>, the key of the
    // entry in the installed apps list, the name of the update packages. Not
    // APP_DIR_NAME: uninstalling removes the install folder, and the cache in
    // %LOCALAPPDATA%\CharisViewer has to stay. viewer_manifest.py reads this
    // line (keep it a plain "..." literal on one line); changing it later makes
    // a second, separate installation.
    inline constexpr char VELOPACK_PACK_ID[] = "CharisViewerApp";

    // The publisher shown in the installed apps list and in Setup.exe: the viewer's
    // name, as other viewers do, not a person (2026-11-08; the same as CompanyName in
    // res/viewerRes.rc). viewer_manifest.py reads this line too.
    inline constexpr char PUBLISHER[] = "Charis Viewer";

    // Where releases are published. The installed apps list links to it, and
    // the update feed is fixed to <PROJECT_URL>/releases/latest/download (the
    // update service of the official viewer never names it, llvelopack.cpp).
    inline constexpr char PROJECT_URL[] = "https://github.com/noriaji/charis-viewer";

    // The only hosts the updater fetches from, the feed's redirects included:
    // GitHub and the hosts its release downloads are served from.
    inline constexpr char UPDATE_HOSTS[] = "github.com objects.githubusercontent.com release-assets.githubusercontent.com";

    // In place of gDirUtilp->initAppDirs("SecondLife") in LLAppViewer::LLAppViewer():
    // sets up the folders under APP_DIR_NAME and the channel. Nothing is copied from
    // the official viewer's folder (plus/importsettings offers that at the login screen).
    void initAppDirs();
}

#endif // PLUS_INDEPENDENCE_H

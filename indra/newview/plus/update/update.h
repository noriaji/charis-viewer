/**
 * @file update.h
 * @brief Installing a new Charis Viewer the user agreed to (research/326): the state,
 *        the chip at the bottom right, and the backend that does the work (Velopack,
 *        registered by llvelopack.cpp for a Setup.exe installation).
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

#ifndef PLUS_UPDATE_UPDATE_H
#define PLUS_UPDATE_UPDATE_H

#include "updatestate.h"

#include <cstdint>
#include <string>

// Whether there is a newer version is plus/updatecheck's (latest.json, once a day). This
// takes over once the user agreed ("Download and update" on its band or window, or
// PlusUpdateInstall 2): it asks the backend to read the feed and fetch the package,
// shows how far it is, and offers "Restart and update" when it is ready. Main thread.
namespace plus::update
{
    // What installs updates. Registered once, before the UI exists, by the code that knows
    // the installation (llvelopack.cpp, only when this exe was installed by Setup.exe);
    // none for the zip. Every function is called on the main thread; start() reports
    // through the report*() functions below, from a coroutine, until one of ready / failed /
    // cancelled.
    struct Backend
    {
        void (*start)();        // read the feed, fetch the package, hand it to the installer
        void (*cancel)();       // stop start()'s work as soon as it can (then reportCancelled())
        void (*discard)();      // a ready update is not put in when the viewer ends
        void (*restartNow)();   // a ready update is put in at the end and the viewer started again
        void (*keepRunning)();  // the quit after restartNow() was called off: no restart after the update
    };

    // backend lives as long as the process (a static of the caller). Once; later calls are ignored.
    void setBackend(const Backend* backend);

    // A backend is there (Setup.exe), or the test one (PlusUpdateDummy): the setting
    // PlusUpdateInstall is shown in Preferences > Setup.
    bool hasInstaller();
    // hasInstaller(), PlusUpdateCheck on and PlusUpdateInstall is not 0
    // ("only tell me"): the band and the window offer "Download and update".
    bool canInstall();
    // PlusUpdateInstall is 2: a newer version found is downloaded without asking (installed
    // when the viewer ends).
    bool installsWithoutAsking();
    // An update the user agreed to is being fetched, is ready, or failed (not Idle): its chip
    // tells of it, so no band or window.
    bool engaged();

    // The user agreed to version (its words, for the chip). package_version is the installer's
    // version of it ("0.1.0-alpha.2.3300", plus/version/packversion.py), "0.1.0-alpha.2" when the update
    // file gives no build (then with any build), "" not known: then nothing is downloaded
    // (Failure::VersionMismatch). The backend installs nothing else. False when it cannot start (no
    // backend, turned off, already going, quitting; a log line says why).
    bool begin(const std::string& version, const std::string& package_version);
    // What begin() was given as package_version ("" not known).
    const std::string& expectedPackageVersion();
    // From the backend: the version the feed offers (shown on the chip when it is not the one agreed to).
    void reportPackageVersion(const std::string& package_version);

    // From the backend (main thread).
    bool isCancelRequested();
    void reportDownloading(uint64_t total_bytes);
    void reportProgress(uint64_t done_bytes, uint64_t total_bytes);
    void reportReady();
    void reportFailed(Failure why);
    void reportCancelled();
}

#endif

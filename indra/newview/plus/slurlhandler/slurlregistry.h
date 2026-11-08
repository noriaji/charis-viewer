/**
 * @file slurlregistry.h
 * @brief Which program opens secondlife:// links from the browser, and making
 *        this viewer that program (or undoing it) for the current Windows user.
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

// research/237-slurl-handler.md in the sl-viewer repository.
// Only what the user asks for is written, and only under HKEY_CURRENT_USER:
//   HKCU\Software\Classes\secondlife, ...\x-grid-location-info   four values each, and an old
//                                                                FriendlyAppName removed
//   HKCU\Software\CharisViewer\SLURLBackup\<scheme>               what those five were before, and
//                                                                the command we wrote (OwnerCommand)
// Nothing is written at start, at an update or at login. No administrator rights.
// Everything here runs on the main thread (a few registry reads and writes).

#ifndef PLUS_SLURLHANDLER_SLURLREGISTRY_H
#define PLUS_SLURLHANDLER_SLURLREGISTRY_H

#include <string>

namespace plus
{
    namespace slurlhandler
    {
        enum class Owner
        {
            None,        // nothing opens secondlife:// links
            This,        // this viewer (this very exe)
            OtherCharis, // a Charis Viewer in another folder
            Other,       // another viewer (the official viewer, Firestorm, ...)
            Missing      // registered, but the exe it names is not there
        };

        struct Status
        {
            Owner       owner = Owner::None;
            std::string name;            // the owner's name (its FileDescription, else its file name)
            std::string exe;             // the owner's exe, full path (empty for None)
            bool        userChoice = false; // picked in Windows' default apps (we cannot change that)
            bool        perUser    = false; // found under HKCU (else HKLM, an installer for all users)
            bool        undoable   = false; // HKCU names this exe: "Undo" has something to do
            Owner       secondOwner = Owner::None; // who opens x-grid-location-info:// links
            std::string secondName;                // and its name
        };

        enum class Result
        {
            Done,
            NothingToDo, // undo: the links are not ours (another viewer registered since, or never)
            BadExe,      // our own path cannot go into a command line safely (nothing written)
            Failed,      // a registry call failed; what was there before was put back
            Partial      // a registry call failed, and a scheme that held another Charis Viewer's
                         // registration (no backup of this press) could not be put back: it may open
                         // this viewer now (written before the failure), or a restore failed
        };

        // False where the links are not registered this way (not Windows).
        bool supported();

        // Who opens secondlife:// links now. Reads the registry only.
        Status status();

        // Makes this exe open secondlife:// and x-grid-location-info:// links for this user
        // (HKCU only), keeping what was there before for undo().
        Result registerHere();

        // Puts back what registerHere() replaced, for the schemes that still name this exe.
        Result undo();

        // This exe's full path, UTF-8 (for the tooltip).
        std::string ourExe();
    }
}

#endif // PLUS_SLURLHANDLER_SLURLREGISTRY_H

/**
 * @file fastexit.h
 * @brief Fast exit (research/276 stage 2): hide the window when the quit is settled, save what must be
 *        saved, and end the process without taking the viewer apart.
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

#ifndef PLUS_FASTEXIT_FASTEXIT_H
#define PLUS_FASTEXIT_FASTEXIT_H

// The flow (the state table is in research/276 7.1; main thread only):
//   F0  the quit is requested and the floaters are closed (the official code, window shown; an unsaved
//       edit is asked about here and the quit can still be cancelled).
//   F1  commit(): the window is hidden, the sound, the media and the voice are silenced (audio_update_volume
//       reads hidden()) and a named mutex says "closing" (a viewer started now waits for it,
//       waitForClosingInstance). The official idleShutdown goes on (uploads, LogoutRequest, the wait for the
//       LogoutReply); display() takes its own not-visible branch.
//   F2  LLAppViewer::fastExit(): the saves of LLAppViewer::cleanup() that matter (the UI is taken apart for the
//       state its panels write, then the settings and caches), then terminate(). The world, the textures and
//       the threads are not destroyed or joined.
// Off (PlusFastExit false, or LLRHI_FAST_EXIT=0, or a rare exit such as "purge the cache on exit"): the
// official cleanup runs as before.
namespace plus::fastexit
{
    // PlusFastExit and the environment variable LLRHI_FAST_EXIT (0 / 1, it wins over the setting). Read
    // once, at the first call (the first quit request).
    bool wanted();

    // F1. Hides the main window, silences the sound, the voice and the internet stream, takes the "closing" mutex. Idempotent; a quit that is
    // cancelled after it (the window comes back) is not possible: it is called after the last point
    // where the quit can be cancelled.
    void commit();

    // True from commit() on: the main loop draws nothing.
    bool hidden();

    // True when the window has been hidden for longer than the logout can take (25 s): the main loop then
    // runs the fast exit even though the official exit has not come (a dialog or a disconnect stopped it).
    bool overdue();

    // At the start of the process (wWinMain): a viewer that is closing (its window already gone) is
    // waited for, 10 s at most, so the new one does not become a "second instance" with read-only caches.
    void waitForClosingInstance();

    // Ends the process at once (TerminateProcess, _exit when that fails). Does not return.
    [[noreturn]] void terminate();
}

#endif

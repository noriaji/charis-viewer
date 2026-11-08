/**
 * @file exitwatch.h
 * @brief Exit watchdog: a process that is still alive after the cleanup has
 *        finished is logged and ended (research/292).
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

#ifndef PLUS_FASTEXIT_EXITWATCH_H
#define PLUS_FASTEXIT_EXITWATCH_H

// Seen 2026-11-07: the viewer logged "Goodbye!" and "status: stopped", yet SecondLifeViewer.exe stayed
// as a background process and held the exe and pdb, so the next link failed. This is the safety net
// and the first look at the cause.
//  - prepare(): the head of cleanup(), main thread. Reads PlusExitWatchdog / PlusExitWatchdogSeconds
//    (gSavedSettings is emptied near the end of cleanup, so they cannot be read later).
//  - arm(): the end of cleanup(), once the settings and caches are saved. Logs how many threads and
//    child processes are alive, then starts a native watchdog thread. When the process is still
//    there after the time-out, the thread writes the live threads and children to exit-watchdog.log
//    (in the log folder) and ends the process with TerminateProcess.
//  - snapshot(): one "Plus" log line with the thread and child process counts (a no-op when the
//    watchdog is off); with list_threads, also each thread's start address and name. wWinMain calls
//    it with the list just before the app is deleted, so every exit leaves a clue in the log.
// A normal exit is not touched: the process ends first and takes the thread with it. With a debugger
// or Tracy attached, the report is written but the process is left alone.
namespace plus::exitwatch
{
    void prepare();
    void arm();
    void snapshot(const char* where, bool list_threads = false);
}

#endif

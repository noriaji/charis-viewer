/**
 * @file safemode.h
 * @brief Safe mode (research/254): a start with the experimental paths off.
 *        --safe or Shift held at the start runs the viewer again in a new
 *        process with the switches of safemode_table.h, and records whether
 *        a start got through (the "starting" marker).
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

#ifndef PLUS_SAFEMODE_H
#define PLUS_SAFEMODE_H

// Included by an official file (llappviewerwin32.cpp): declarations only.

namespace plus::safemode
{
    // The first thing in wWinMain, before anything of the viewer is made.
    // When this start asks for safe mode (--safe on the command line, Shift
    // held, "start in safe mode next time too" on, or a yes to the question
    // asked after starts that did not get through) and is not a safe start
    // already, it starts the viewer again with the safe switches, waits for
    // it and returns true with its exit code: wWinMain returns that code.
    // False otherwise (a normal start, a safe start, the GPU benchmark
    // subprocess, or the new process could not be started: then this start
    // goes on as a normal one, and the log says why).
    bool relaunchIfAsked(int& exit_code);

    // This process is a safe start (it has the safe switches). Any thread.
    bool active();

    // An unattended tool started this process (CHARIS_SAFE_MODE_NO_PROMPT is set): no
    // question, "next time too" not followed, the start not counted. Any thread.
    bool unattended();

    // A safe start: it is in the job of the process that started it, which ends it when that
    // process ends (false: that job could not be made). For the log.
    bool inJobEndingWithFirst();

    // Why this is a safe start ("argument", "shift", "setting", "offer"),
    // empty in a normal start.
    const char* reason();

    // A safe start, once its own subprocesses that are to run safe too (the
    // GPU benchmark) have run: takes the safe switches and the mark out of
    // the environment that processes started from now on get (media
    // plugins, a folder opened in Explorer, a viewer started from there).
    // This process keeps reading its own values. Main thread.
    void forgetSafeEnvironment();

    // The Windows error when this start asked for safe mode and the safe start
    // could not be made (it went on as a normal start), else 0.
    unsigned long relaunchError();

    // What this start did about the question asked after starts that did
    // not get through (for the log; a yes goes on in the safe start, so this
    // process only tells of a no, or of a question not asked).
    struct Question
    {
        int after = 0;              // the starts in a row that had not got through; 0: not about to ask
        bool asked = false;         // the question was shown
        bool yes = false;
        unsigned long error = 0;    // the starting marker could not be removed (Windows error): not asked
    };
    const Question& question();

    // How many starts in a row before this one did not get through (the
    // "starting" marker was left behind: a crash or a kill before the world
    // had been shown for a while, or before a normal quit). 0 until the
    // feature's init (early in LLAppViewer::init(), before the window).
    // A safe start reads it but does not count itself.
    int unfinishedStartsBefore();

    // "Start in safe mode next time too" (a file read by the next start's
    // relaunchIfAsked()). After the feature's init. Main thread.
    bool nextStartSafe();
    void setNextStartSafe(bool on);

    // The files of the next start's relaunchIfAsked() are where this start
    // keeps them (the user folder found before the start agrees with
    // gDirUtilp's). When not, the setting above cannot work and is greyed out.
    bool earlyFilesFound();
}

#endif // PLUS_SAFEMODE_H

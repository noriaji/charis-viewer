/**
 * @file exittime.h
 * @brief Exit time log: how long each stage of closing the viewer takes
 *        (research/276 stage 1, measuring before the fast exit).
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

#ifndef PLUS_FASTEXIT_EXITTIME_H
#define PLUS_FASTEXIT_EXITTIME_H

// With PlusExitTimeLog on, each stage of the exit writes one "Plus" log line:
//   ExitTime [<stage>] +<ms since the clock started> ms (step <ms since the previous line> ms)
// "step" counts from the previous line of the same side: these marks, or
// LLRHIVKMirror::onGLShutdown's own steps (its first step from its start). So the
// mark after onGLShutdown ("cleanup: viewer window deleted") has a step that
// holds the whole of onGLShutdown.
// The clock is a steady clock (QueryPerformanceCounter), not LLFrameTimer: the
// frames stop turning inside LLAppViewer::cleanup().
// Main thread only. Nothing runs while the viewer is not closing.
namespace plus::exittime
{
    // Where an exit begins (requestQuit, forceQuit, the head of cleanup): the
    // first call of an exit reads PlusExitTimeLog once and, when it is on,
    // starts the clock and sets LLRHI::sExitTimeLog (the Vulkan shutdown's own
    // steps, written from llrhi, which cannot read the setting). Later calls of
    // the same exit only mark. Every call, measured or not, also sets
    // LLRHI::sQuitting (research/285: the VRAM compactions stop; cancel()
    // clears it).
    void begin(const char* stage);

    // One line, when the clock runs; returns at once otherwise.
    void mark(const char* stage);

    // The quit was cancelled (LLAppViewer::abortQuit): the next quit starts a
    // new clock and reads the setting again.
    void cancel();
}

#endif

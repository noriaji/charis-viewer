/**
 * @file hitchlog.h
 * @brief Hitch log: finds out what the main thread was doing during a frame
 *        that took far too long (a freeze of a second when objects load).
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

#ifndef PLUS_HITCHLOG_H
#define PLUS_HITCHLOG_H

namespace plus::hitchlog
{
    // Once at the start of every frame, on the main thread (LLAppViewer::doFrame).
    // The first call starts the watchdog thread. A frame that has not ended
    // PlusHitchLogMs milliseconds after its start (0 = off) gets its main
    // thread's call stack sampled every 100 ms; when the frame ends (or, if it
    // does not, after 12 samples), one "HitchLog" group of log lines tells the
    // frame's length and the samples.
    // Windows only; elsewhere a no-op. Costs a time stamp and a counter a frame.
    void frameTick();
}

#endif

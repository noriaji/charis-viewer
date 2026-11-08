/**
 * @file uiframe.h
 * @brief "UI frames": while the user works the UI over a slow world, most
 *        frames draw only the UI over the last world image (research/86 E1).
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

#ifndef PLUS_UIFRAME_H
#define PLUS_UIFRAME_H

namespace plus::uiframe
{
    // The <plus> hook in LLAppViewer::doFrame(), in place of display(): input,
    // coroutines and idle() (network, object updates, texture fetches) have
    // run as always. Returns true when this loop drew a UI-only frame (the
    // last world image, the UI over it, presented), so the caller skips the
    // world: display() and the reflection probe update. False: draw the world
    // as usual (always when the feature is off, the world is fast, or nobody
    // works the UI). Main thread.
    bool drawInsteadOfWorld();

    // The <plus> hook in render_ui() (llviewerdisplay.cpp), after the world,
    // HUD and name tags are in the default framebuffer and before the 2D UI:
    // keeps that image for the UI-only frames that follow, while the world is
    // slow. Does nothing otherwise. Main thread.
    void onWorldDrawn();

    // The <plus> hook in update_statistics() (llviewerstats.cpp), in place of
    // counting the loop in the FPS stat: idle() runs before the loop decides
    // whether it draws the world, so the count is left to
    // drawInsteadOfWorld(), which counts 1 for a world frame (as the caller
    // would) and nothing for a UI-only frame. Returns false when the caller
    // counts every loop as before (LLRHI_UI_FRAME_STAT=0).
    // Main thread.
    bool countFrameLater();

    // The world's frames per second for the FPS displays (plus/fpscorner, the
    // performance floater): the world frames of the last second over the
    // time between the first and the last of them; UI-only frames are not
    // counted. The FPS stat's per-loop readings (the median over the last 50
    // loops) take the UI-only frames as loops of 0 FPS, so they read 0 while
    // the UI is worked over a slow world. With no world frame for a while
    // (the world held), the rate of the last two world frames.
    // Returns `stat_fps` (what the caller read from the FPS stat, the median of
    // the last 50 loops) when none of the last 50 loops was UI-only (the median
    // is right then, and steadier than a mean over hitches), when the FPS stat
    // counts every loop (LLRHI_UI_FRAME_STAT=0), or before the second world
    // frame. Main thread.
    F64 worldFps(F64 stat_fps);
}

#endif // PLUS_UIFRAME_H

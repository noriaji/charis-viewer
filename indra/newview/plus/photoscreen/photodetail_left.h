/**
 * @file photodetail_left.h
 * @brief The left of the detailed photo screen: the rail of eight tabs (scene, avatar, environment, lighting,
 *        pose & animation, camera, effects, presets) and the tab's column of headings and rows (research/221).
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

#ifndef PLUS_PHOTOSCREEN_PHOTODETAIL_LEFT_H
#define PLUS_PHOTOSCREEN_PHOTODETAIL_LEFT_H

#include "photoscreen.h"

namespace plus::photoscreen::detail
{
    // The column of the tab chosen, made again. A tab of before set in sS.tab by another area (TabFocus: the
    // focus button over the world) shows the tab its rows are in now (the camera tab, its focus section open).
    void fillLeft();

    // The rail's buttons (once, when the panels are made).
    void bindLeftPanel();

    // The left panel's place for the frame `m`; `open`: the body is shown (wide, or the folded panel opened).
    void layoutLeft(const Margins& m, bool open);

    // A quarter of a second: what the tab shown follows outside the rows (the focus lock, the sky lists).
    void tickLeft();
}

#endif // PLUS_PHOTOSCREEN_PHOTODETAIL_LEFT_H

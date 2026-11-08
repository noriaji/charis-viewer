/**
 * @file photodetail_center.h
 * @brief The middle of the detailed photo screen: the chips inside the world at its top left (look, size,
 *        shape, composition lines) and the panel over the world (read-out, chat, histogram's frame, icon boxes,
 *        rounded corners; the drawing view of the focus frame and the chat lines is photodetail_hud.h). The way
 *        out is F12 (plus/photomode/photokeys) and the simple view's exit (research/221).
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

#ifndef PLUS_PHOTOSCREEN_PHOTODETAIL_CENTER_H
#define PLUS_PHOTOSCREEN_PHOTODETAIL_CENTER_H

#include "photoscreen.h"

class LLPanel;

namespace plus::photoscreen::detail
{
    // The chips' choices and callbacks (once, when the panels are made).
    void bindTopPanel();

    // The chat input and the icons over the world (once, when the panels are made).
    void bindHudPanel();

    // The chips' place inside the world (the frame `m` is not used: they are inside the world's rectangle).
    void layoutTop(const Margins& m);

    // The panel over the world on the world's rectangle, with the chat's width and height.
    void layoutHud();

    // A quarter of a second: the chips, the read-out, the label under the focus frame, the icons' states.
    void tickCenter(LLPanel* hud);
}

#endif // PLUS_PHOTOSCREEN_PHOTODETAIL_CENTER_H

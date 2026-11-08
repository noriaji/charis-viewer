/**
 * @file photodetail_hud.h
 * @brief The drawing view over the world of the detailed photo screen: the focus frame and the chat lines.
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

#ifndef PLUS_PHOTOSCREEN_PHOTODETAIL_HUD_H
#define PLUS_PHOTOSCREEN_PHOTODETAIL_HUD_H

#include <string>

#include "llrect.h"
#include "stdtypes.h"

class LLView;

namespace plus::photoscreen::detail::hud
{
    // Makes the view that fills `parent` (the panel over the world), behind the parent's other children, and
    // starts collecting the nearby chat. Returns it (null if it cannot be made). It takes no mouse.
    LLView* create(LLView* parent);

    // Where the chat lines are drawn, in the view's own coordinates (the lines end at the bottom of it).
    void setChatRect(LLView* view, const LLRect& rect);

    // The text under the focus frame ("Name · Eyes · 1.8 m"); changed a few times a second, not every frame.
    void setFrameLabel(const std::string& label);
}

#endif // PLUS_PHOTOSCREEN_PHOTODETAIL_HUD_H

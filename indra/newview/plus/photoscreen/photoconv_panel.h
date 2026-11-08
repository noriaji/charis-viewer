/**
 * @file photoconv_panel.h
 * @brief The photo screen's conversations tab (research/247, first stage): the list of IM and group chats, the
 *        last messages of the one chosen and a line to answer in, in both modes; the unread badge of the
 *        simple mode's tab. The data are photoconv.h's.
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

#ifndef PLUS_PHOTOSCREEN_PHOTOCONV_PANEL_H
#define PLUS_PHOTOSCREEN_PHOTOCONV_PANEL_H

#include "stdtypes.h"

class LLPanel;
class LLView;

namespace plus::photoscreen::conv
{
    // The conversations panel (panel_plus_photoscreen_conv.xml), made on parent, filling it and following its
    // size. Hidden at first. Null when its XML did not load.
    LLPanel* createPanel(LLView* parent);

    // The tab was opened (the panel is shown): the conversation with the newest unread messages is chosen and
    // read, and the line to type in takes the keyboard.
    void openPanel(LLPanel* panel);

    // A badge over the right end of a view (the simple mode's tab button) that does not take the mouse.
    LLView* addBadge(LLView* view);
    void setBadge(LLView* badge, S32 count);
}

#endif // PLUS_PHOTOSCREEN_PHOTOCONV_PANEL_H

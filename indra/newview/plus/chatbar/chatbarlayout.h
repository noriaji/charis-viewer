/**
 * @file chatbarlayout.h
 * @brief Chat bar (plus/chatbar, research/334): the few functions official files call, without the chat bar's
 *        widget class (chatbar.h), so that they need no UI headers. Defined in chatbar.cpp.
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

#ifndef PLUS_CHATBARLAYOUT_H
#define PLUS_CHATBARLAYOUT_H

#include "stdtypes.h"

class LLView;

namespace plus::chatbar
{
    // The bottom panel of the toolbar view (bottom_toolbar_panel) holds the chat bar's panel and the bottom
    // toolbar's own panel side by side (panel_toolbar_view.xml). LLToolBarView::draw() shows or hides the
    // toolbar's own panel (it has buttons) and sizes it to the toolbar; this, called right after, does the same for
    // the bottom panel: shown while the toolbars are (show_toolbars) and the chat bar (PlusShowChatBar) or the
    // toolbar's panel is, as high as the toolbar (and at least the chat bar's height while the bar is shown: an empty
    // toolbar is lower). While both are shown, the bar is as wide as the user wants (320 at start, then as dragged)
    // but narrowed (to its min_dim at least) when it would leave the toolbar's panel less than 480 (a narrow window,
    // a pull to the right edge), and widened back when the window is. Writes only what changed. Main thread, each
    // frame.
    // Hook: one call in LLToolBarView::draw() (lltoolbarview.cpp).
    void syncBottomPanel(LLView* bottom_panel, const LLView* bottom_toolbar, bool show_toolbars);

    // The bottom toolbar's centre in x, in the bottom panel (bottom_toolbar_panel): what bottom_toolbar's
    // getRect().getCenterX() was before the toolbar moved into a panel of its own beside the chat bar. For the
    // buttons centred over the toolbar (LLPanelStandStopFlying::updatePosition(), llmoveview.cpp;
    // LLPanelHideBeacon::updatePosition(), llfloaterworldmap.cpp; the fly camera's exit button, plus/flycammouse).
    // While the toolbar's own panel is hidden (no buttons) the centre of the bottom panel, as an empty toolbar
    // across the whole width was before; with the chat bar hidden, the same value as before.
    S32 bottomToolbarCenterX(const LLView* bottom_toolbar);

    // The chat bar has the keyboard focus (LLViewerWindow::handleKey(): ArrowKeysAlwaysMove moves the avatar
    // with the arrows while typing there, as in the nearby chat floater's field).
    bool hasFocus();

    // LLFloaterIMNearbyChat::startChat() (Enter, "/", the letter keys with nothing focused): while the chat bar is
    // shown, it takes the focus instead of the nearby chat floater (as in Firestorm), with line (when not null) as
    // its text while it is empty (a line half typed is kept). False when it is not there: the floater opens as before.
    // Also LLViewerWindow::handleKey()'s letter keys (LetterKeysFocusChatBar), before it opens the floater.
    bool startChat(const char* line);
}

#endif // PLUS_CHATBARLAYOUT_H

/**
 * @file chatbar.h
 * @brief Chat bar: a "Nearby Chat" button and a one-line nearby chat field at the bottom left, beside the bottom
 *        toolbar, as Firestorm's chat bar (research/334; see chatbar.cpp).
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

#ifndef PLUS_CHATBAR_H
#define PLUS_CHATBAR_H

#include "chatbarlayout.h"     // the functions official files call (syncBottomPanel, bottomToolbarCenterX, ...)

#include "llchatentry.h"

#include <boost/signals2/connection.hpp>

namespace plus::chatbar
{
    // The field (XUI tag plus_chat_bar, panel_toolbar_view.xml): the official nearby chat field (LLChatEntry, so
    // the line history on Ctrl+Up/Down, the spell check, the emoji and gesture helpers are the same) on one line.
    // Return says, Shift+Return whispers, Ctrl+Return shouts, on channel 0 unless the text starts with "/number"
    // (Firestorm's chat bar: the nearby chat floater's channel selector and chat type button are not used), through
    // what the nearby chat floater's field does (LLFloaterIMNearbyChat::onChatEntryKeystroke / sendChatFromEntry).
    // The chat font, at most SansSerif (a one-line field of a fixed height). Holds no state of its own; while
    // disconnected (gDisconnected) it is made read-only when it gets the focus, as the floater's field is.
    class ChatBar : public LLChatEntry
    {
    public:
        struct Params : public LLInitParam::Block<Params, LLChatEntry::Params>
        {
            Params();
        };

        bool postBuild() override;
        bool handleKeyHere(KEY key, MASK mask) override;
        void onFocusReceived() override;
        void onFocusLost() override;

    protected:
        friend class LLUICtrlFactory;
        explicit ChatBar(const Params& p);

    private:
        boost::signals2::scoped_connection mFontChanged;   // LLViewerChat's chat font size (ChatFontSize)
    };
}

#endif // PLUS_CHATBAR_H

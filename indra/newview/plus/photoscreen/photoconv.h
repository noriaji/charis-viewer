/**
 * @file photoconv.h
 * @brief The photo screen's conversations (research/247, first stage): the IM, group and conference chats
 *        of the official model (LLIMModel) as the conversations tab shows them, their unread counts for the
 *        badge, the one chosen, what was typed and not sent, and sending. No UI here (photoconv_panel.h).
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

#ifndef PLUS_PHOTOSCREEN_PHOTOCONV_H
#define PLUS_PHOTOSCREEN_PHOTOCONV_H

#include "lluuid.h"
#include "stdtypes.h"

#include <boost/signals2.hpp>
#include <string>
#include <vector>

namespace plus::photoscreen::conv
{
    enum class Kind
    {
        IM,         // one person
        Group,
        Conference  // ad-hoc
    };

    // One conversation of the list, as it was when the list was last made.
    struct Entry
    {
        LLUUID      id;             // the official session id
        std::string name;           // the person's or the group's name (another user's text: shown only)
        Kind        kind = Kind::IM;
        S32         unread = 0;     // messages of people not read yet (not the viewer's notes nor your own)
        bool        counted = true; // the badge counts them: the official notice setting is not "no action", not muted
        bool        connecting = false; // a group or conference whose session has not started yet
    };

    // Called by photoscreen on the way in and out. Between them the official model's signals are listened to;
    // outside, nothing runs. stop() flashes the toolbar's conversations button once when unread messages are
    // left (the photo screen kept the official flash and toast back).
    void start();
    void stop();
    bool started();

    // The list (unread first, then the ones talked in most lately), the badge's count and a number that grows
    // each time either may have changed. Made again at most four times a second, only after the model changed.
    const std::vector<Entry>& entries();
    const Entry* find(const LLUUID& id);
    S32 badgeCount();
    U32 version();

    // Fired (four times a second at most) when the list, a count or a conversation's messages changed. The
    // conversations panels and the tabs' badges connect to it.
    using changed_signal_t = boost::signals2::signal<void()>;
    boost::signals2::connection onChanged(const changed_signal_t::slot_type& slot);

    // The conversation shown (null: none). choose() keeps what was typed in the one before (setDraft()).
    const LLUUID& chosen();
    void choose(const LLUUID& id);
    // The id a conversation has now: a group or conference gets its id from the server when it starts, and the
    // first one leads to it. The same id when it was not changed.
    LLUUID currentId(const LLUUID& id);
    // When the tab is opened: the newest conversation with unread messages, else the one of before if it is
    // still there, else the first.
    void chooseForOpening();

    // The last messages of a conversation, oldest first, at most `count` (from the official model's memory;
    // no file is read here).
    struct Line
    {
        std::string from;
        std::string text;
        bool        own = false;    // sent by you
        bool        system = false; // the viewer's note (no speaker)
        bool operator==(const Line&) const = default;
    };
    void lines(const LLUUID& id, size_t count, std::vector<Line>& out);

    // A chat line as the viewer shows it: "Name: text", "Name does something" for /me, the bare text when there is
    // no name. Also the detailed mode's nearby chat lines (photodetail_hud.cpp).
    std::string speakerLine(const std::string& who, const std::string& text);

    // Marks the conversation read in the official model (its counters and the official windows follow).
    void markRead(const LLUUID& id);

    // What was typed in a conversation and not sent (kept in memory only, for this session).
    std::string draft(const LLUUID& id);
    void setDraft(const LLUUID& id, const std::string& text);

    enum class SendResult
    {
        Sent,       // sent, or queued by the official window until the session has started
        Empty,
        Waiting,    // the session has not started and there is no official window to queue it: not sent
        Refused     // no such conversation, or one that cannot take text
    };
    // Sends as the official conversation window does (LLFloaterIMSession::sendMsg, or LLIMModel::sendMessage).
    SendResult send(const LLUUID& id, const std::string& text);

    // The <plus> hook in notify_of_message() (llimview.cpp), after the official sound: true when the photo
    // screen shows the notice itself (its badge), so the official toast, flash and opened window are left out.
    // False for the nearby chat, in Do Not Disturb, while the official conversations window is open, in
    // mouselook (the photo screen's frame is hidden), and when the photo screen is off.
    bool takesNotice(const LLUUID& session_id);
}

#endif // PLUS_PHOTOSCREEN_PHOTOCONV_H

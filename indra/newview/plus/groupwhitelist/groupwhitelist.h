/**
 * @file groupwhitelist.h
 * @brief Group chat black and white lists: the chat of a group is never
 *        received (blacklist), or, with "only whitelisted groups" on, only the
 *        chat of whitelisted groups is.
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

#ifndef PLUS_GROUPWHITELIST_H
#define PLUS_GROUPWHITELIST_H

#include "llinstantmessage.h"

class LLSD;
class LLUUID;

namespace plus::groupwhitelist
{
    // How the chat of one group is treated. Saved per account (the group ids).
    enum class Mode
    {
        Normal,     // received, unless "only whitelisted groups" is on
        Blacklist,  // never received
        Whitelist,  // received, also with "only whitelisted groups" on
    };

    Mode mode(const LLUUID& group_id);
    void setMode(const LLUUID& group_id, Mode mode);

    // "Only whitelisted groups": the chat of every other group is refused
    // (PlusGroupChatWhitelistOnly, per account).
    bool whitelistOnly();
    void setWhitelistOnly(bool on);

    // Changes each time a list or the switch changes (for the group window).
    U32 revision();

    // The two hooks in the official viewer (llimview.cpp). Each returns true when
    // the chat is refused: the caller drops it and returns. A refused group's chat
    // is not kept anywhere (no session, no history, no notice); the viewer leaves
    // the session on the server, and closes it here if it was open.
    //
    // refuseInvitation(): the body of a ChatterBoxInvitation from the event queue,
    //   at the start of LLViewerChatterBoxInvitation::post() (all three kinds:
    //   "instantmessage", "voice" and "immediate"), so a refused invitation is
    //   neither shown nor accepted.
    // refuseMessage(): at the start of LLIMMgr::addMessage(), which every group
    //   chat line goes through (the invitation above, IM_SESSION_SEND from the
    //   simulator, online, offline or in do-not-disturb).
    bool refuseInvitation(const LLSD& body);
    bool refuseMessage(const LLUUID& session_id, const LLUUID& from_id, EInstantMessage dialog);
}

#endif // PLUS_GROUPWHITELIST_H

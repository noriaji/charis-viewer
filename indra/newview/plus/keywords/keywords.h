/**
 * @file keywords.h
 * @brief Chat keyword alerts (ported from Firestorm's FSKeywords): a line of chat that holds one of
 *        the words the user listed gets a color, a sound and/or a notice.
 *
 * Ported from Firestorm's fskeywords.h (see ORIGIN.md).
 *
 * $LicenseInfo:firstyear=2011&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2011, The Phoenix Firestorm Project, Inc.
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
 *
 * The Phoenix Firestorm Project, Inc., 1831 Oakwood Drive, Fairmont, Minnesota 56031-3225 USA
 * http://www.firestormviewer.org
 * $/LicenseInfo$
 */

#ifndef PLUS_KEYWORDS_H
#define PLUS_KEYWORDS_H

#include <string>

#include "lluicolor.h"
#include "lluuid.h"

class LLChat;

namespace plus::keywords
{
    // Official hooks (marked <plus>):
    //   process_chat_from_simulator()  llviewermessage.cpp   onLocalChat(chat)
    //   LLIMMgr::addMessage()          llimview.cpp          onInstantMessage(...)
    //   LLViewerChat::getChatColor()   llviewerchat.cpp      colorFor(chat, ...), both overloads

    // A line of nearby chat (or an object's say) has arrived: sound and notice when it holds a keyword.
    void onLocalChat(const LLChat& chat);

    // A line of IM or group chat has arrived and is kept (just before it is stored: not one the viewer
    // drops, from someone blocked or, with "only friends and groups", from someone else); from_id is
    // the speaker.
    void onInstantMessage(const LLUUID& session_id, const LLUUID& from_id, const std::string& from_name,
                          const std::string& text);

    // The color for a line of chat shown in a window or a toast, when it holds a keyword and the color
    // is switched on. A line with a session id is IM / group chat, one without is nearby chat.
    bool colorFor(const LLChat& chat, LLUIColor& color);

    // The same, as the name of a color of the color table (for toasts, which take a name).
    bool colorNameFor(const LLChat& chat, std::string& color_name);

    // For the window's try-out field: does the text hold a keyword of the list as it is now (own name
    // included), whether or not the alerts are switched on? word gets the keyword as typed.
    bool testText(const std::string& text, std::string& word);
    // Counts up (from 1) each time the list is made again from the settings (or your names became known):
    // testText() of the same text gives the same answer while this stays.
    unsigned listGeneration();
}

#endif // PLUS_KEYWORDS_H

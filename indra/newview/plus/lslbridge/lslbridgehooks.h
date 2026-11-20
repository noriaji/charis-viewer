/**
 * @file lslbridgehooks.h
 * @brief The calls official viewer files (and plus/outfitbackup) make into plus/lslbridge (research/336, 353).
 *        Forward declarations only (and llpointer.h for the inventory item lists, stdtypes.h for U32).
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

#ifndef PLUS_LSLBRIDGE_LSLBRIDGEHOOKS_H
#define PLUS_LSLBRIDGE_LSLBRIDGEHOOKS_H

#include <string>
#include <vector>

#include "llpointer.h"
#include "stdtypes.h"

class LLChat;
class LLUUID;
class LLViewerInventoryItem;
class LLViewerObject;

namespace plus::lslbridge
{
    // process_chat_from_simulator() (llviewermessage.cpp), right after the speaker was looked up and before the
    // speech particles, RLVa, the chat and its translation see the line; called for the llOwnerSay lines
    // (CHAT_TYPE_OWNER) of our own objects, chatter null when the speaker is not known. True: the line is taken
    // here and not shown: any line in the form of the bridge's URL lines ("<charisBridge..."), whoever said it (it
    // may hold a URL and a token: never shown, logged or translated), and a pose line ("<auroraPose>") of the bridge
    // known as the speaker: the worn bridge seen as the chatter, or the speaker's id (from_id) being that of the
    // bridge whose contents were seen holding no other script for the URL held (cleanOf). Such a pose line is
    // passed on only from the bridge seen clean, and dropped unread when it does not pass the checks. False (the
    // line goes on as before): anything else, also a pose line of a speaker not known as the bridge (another object,
    // or one neither seen nor of the id seen clean, which only a script that is not the bridge's could have said).
    // Main thread.
    bool handleOwnerSay(const LLChat& chat, const std::string& text, const LLUUID& from_id, const LLUUID& owner_id,
                        LLViewerObject* chatter);

    // llappearancemgr.cpp (research/353): the pose bridge is not a part of the person's outfit, as Firestorm's is not.
    // All main thread.
    // The bridge object this viewer made for the account (the one recorded when it was copied; never one known by its
    // name). item_id: an inventory item, or what a link points to (getLinkedUUID()). Null: false.
    bool isBridgeItem(const LLUUID& item_id);
    // updateIsDirty(): the links to the bridge left out of the COF and the outfit compared (it is no change of the
    // outfit; Firestorm's FIRE-3018).
    void dropBridgeLinks(std::vector<LLPointer<LLViewerInventoryItem>>& items);
    // The bridge item a new COF of other_objects attachments (the bridge not counted) is to hold, or null: the bridge
    // is to run (the setting on, not a safe start), is worn or on its way (linked in the COF), and has a place with the
    // person's attachments first (theirs, the temporary ones worn, the bridge, and one place left free; a limit not
    // known: it fits). Left out, it comes off and is put on again once a change of the attachments leaves a place.
    // updateCOF() (placeBridgeInNewCof) and the outfit backup's restore (plus/outfitbackup) ask it.
    LLViewerInventoryItem* bridgeForOutfit(U32 other_objects);
    // updateCOF(), replacing the outfit or adding to it: obj_items (the new COF's attachments) holds the bridge once
    // when bridgeForOutfit() gives it, else not at all (a link of it the new outfit holds too). No bridge made, the
    // setting off or a safe start: obj_items is left as it is.
    void placeBridgeInNewCof(std::vector<LLPointer<LLViewerInventoryItem>>& obj_items, bool append);
}

#endif // PLUS_LSLBRIDGE_LSLBRIDGEHOOKS_H

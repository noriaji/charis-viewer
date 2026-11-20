/**
 * @file cshooks.h
 * @brief Contact sets (plus/contactsets, research/333) where they show: the set colours of the name tags, the chat
 *        bubbles, the chat, the minimap, the radar and the friends lists; the online notices of a set; a set's
 *        busy (do not disturb) answer; the alias quotes in the avatar lists' sort.
 *
 * The data is LGGContactSets (cs-core); the official files call these functions only (marked <plus> contactsets).
 * What each place asks, with Firestorm's place:
 *   colorFor(id, TAG)        LLVOAvatar::idleUpdateNameTagText()      llvoavatar.cpp getNameTagColor()
 *   colorFor(id, CHAT)       the chat bubble above the tag, LLViewerChat::getChatColor()
 *   colorFor(id, MINIMAP)    LLNetMap::draw()                          llnetmap.cpp getAvatarColor()
 *   colorFor(id, RADAR)      plus/radar radarfloater.cpp styleRow()   fsradar.cpp updateRadarList()
 *   colorFor(id, FRIENDS)    LLAvatarListItem::setNameInternal()      the same
 *   notifyToast / notifyNearbyChat   llcallingcard.cpp processNotify(), llnotificationtiphandler.cpp
 *   busyResponse             llviewermessage.cpp send_do_not_disturb_message()
 *   hasPseudonym             llavatarlist.cpp LLAvatarItemNameComparator::doCompare()
 *
 * Speed (research/333 5.): the name tags and the minimap ask for every avatar on every frame. colorFor() returns at
 * once, before LGGContactSets is looked at, while the place's FSContactSetsColorize* is off; with it on, once a frame
 * it asks whether any set exists (no set, the default: nothing more). An answer is kept per place and avatar for
 * CACHE_SECONDS and dropped at once when LGGContactSets says a set or its members changed (its change signal), or
 * when RLVa's @shownames changes. A set's colour change that comes without the signal shows within CACHE_SECONDS.
 * Everything here runs on the main thread.
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

#ifndef PLUS_CONTACTSETHOOKS_CSHOOKS_H
#define PLUS_CONTACTSETHOOKS_CSHOOKS_H

#include <functional>
#include <string>

#include <boost/signals2.hpp>

#include "lluuid.h"
#include "v4color.h"

#include "plus/contactsets/lggcontactsets.h" // ContactSetType

namespace plus::cshooks
{
    // The set colour of an avatar for one place: true and color set when that place's FSContactSetsColorize* is on,
    // RLVa lets the name show (@shownames) and LGGContactSets::hasFriendColorThatShouldShow() gives one; otherwise
    // false and color left as it is. ContactSetType::IM is asked as CHAT (one setting for both, as in Firestorm).
    bool colorFor(const LLUUID& id, ContactSetType type, LLColor4& color);

    // A set the avatar is in asks for online notices (LGGContactSets::notifyForFriend()), and they are to show as a
    // toast (FSContactSetsNotificationToast) / in the nearby chat (FSContactSetsNotificationNearbyChat).
    bool notifyToast(const LLUUID& id);
    bool notifyNearbyChat(const LLUUID& id);

    // The busy (do not disturb) answer of the avatar's set, when one of its sets has its own (the smallest set
    // wins, LGGContactSets::getAutoresponseForFriend()). False and response left as it is otherwise.
    bool busyResponse(const LLUUID& id, std::string& response);

    // The avatar has an alias (LGGContactSets::hasPseudonym()); false while contact sets are not loaded.
    bool hasPseudonym(const LLUUID& id);

    // Called after LGGContactSets changed (a set, its members or the aliases), for lists that show set colours to
    // build their names again. The callback must not change contact sets.
    boost::signals2::connection onChanged(const std::function<void()>& callback);
}

#endif // PLUS_CONTACTSETHOOKS_CSHOOKS_H

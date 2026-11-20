/* @file lggcontactsets.cpp
 * Copyright (C) 2011 Greg Hendrickson (LordGregGreg Back)
 *
 * This is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * This is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with the viewer; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */

// From Firestorm (indra/newview/lggcontactsets.cpp, research/333). What changed (plus/contactsets/ORIGIN.md):
// - The file is read through plus::contactsets::readFile() (size, depth and map checked; a file that cannot be
//   read is moved aside to .bad and the sets start empty) and every value of it is checked (ids, colours,
//   lengths, how many): it may have been brought over from another viewer. Saved through
//   plus::writeFileReplacing() (never a cut-off file). loadFromDisk() can read the file again (the bringing over
//   of settings): what was read before goes first.
// - getFriendColor() and the checks the name tags, the minimap and the lists make every frame make no copies:
//   no sets is a return at the top, and the colour of an avatar's sets is kept until the next change
//   (mSetColorCache, emptied at every change: markChanged()).
// - The alias is applied by the name cache when a name is read (llavatarnamecache.cpp, setCustomNameCheckCallback),
//   not written into it: the cache's file keeps the real names.
// - Not here (nothing calls them in this viewer): colorize() (the official colours stay where they are,
//   plus/contactsethooks/ORIGIN.md), the export of one set to a file, getFriendsInSet(), isFriendInSet(),
//   getListOfNonFriends(), getPseudonym().
// - No names, aliases or set names in the log (research/287 P13).

#include "llviewerprecompiledheaders.h"

#include "lggcontactsets.h"

#include "contactsets.h"
#include "plus.h"
#include "rlv/rlvhooks.h"

#include "llagent.h"
#include "llavatarnamecache.h"
#include "llcallingcard.h"
#include "lldir.h"
#include "llnotifications.h"
#include "llnotificationsutil.h"
#include "llsdserialize.h"
#include "llsdutil.h"
#include "llviewercontrol.h"
#include "llvoavatar.h"

#include <algorithm>
#include <cmath>

namespace
{
    constexpr F32 COLOR_DAMPENING = 0.8f;

    // Limits of what is read from the file (it may come from another viewer; research/287 P09). A real one
    // is well under them; past them the rest is left out (and said in the log, as counts).
    constexpr size_t MAX_SETS = 1000;
    constexpr size_t MAX_IDS = 200000;              // members of all sets, the extra avatars and the aliases
    constexpr size_t MAX_SET_NAME_BYTES = 255;
    constexpr size_t MAX_PSEUDONYM_BYTES = 255;
    constexpr size_t MAX_AUTORESPONSE_BYTES = 1023; // an IM's length
    constexpr size_t MAX_COLOR_CACHE = 4096;        // avatars in mSetColorCache before it is emptied

    struct contact_set_autoresponse_ref_t
    {
        bool* enabled;
        std::string* message;
    };
    struct const_contact_set_autoresponse_ref_t
    {
        const bool* enabled;
        const std::string* message;
    };
    contact_set_autoresponse_ref_t get_autoresponse_ref(LGGContactSets::ContactSet* set, ContactSetAutoresponseMode mode)
    {
        switch (mode)
        {
            case ContactSetAutoresponseMode::BUSY:
                return { &set->mAutoresponseBusyEnabled, &set->mAutoresponseBusy };
            case ContactSetAutoresponseMode::AUTORESPONSE:
                return { &set->mAutoresponseModeEnabled, &set->mAutoresponseMode };
            case ContactSetAutoresponseMode::AUTORESPONSE_NONFRIENDS:
                return { &set->mAutoresponseNonFriendsEnabled, &set->mAutoresponseNonFriends };
        }

        return { nullptr, nullptr };
    }
    const_contact_set_autoresponse_ref_t get_autoresponse_ref(const LGGContactSets::ContactSet* set, ContactSetAutoresponseMode mode)
    {
        switch (mode)
        {
            case ContactSetAutoresponseMode::BUSY:
                return { &set->mAutoresponseBusyEnabled, &set->mAutoresponseBusy };
            case ContactSetAutoresponseMode::AUTORESPONSE:
                return { &set->mAutoresponseModeEnabled, &set->mAutoresponseMode };
            case ContactSetAutoresponseMode::AUTORESPONSE_NONFRIENDS:
                return { &set->mAutoresponseNonFriendsEnabled, &set->mAutoresponseNonFriends };
        }

        return { nullptr, nullptr };
    }

    // At most max_bytes, not cut inside a character.
    std::string truncated(const std::string& text, size_t max_bytes)
    {
        return text.size() > max_bytes ? utf8str_truncate(text, (S32)max_bytes) : text;
    }

    // A set's name or an alias: one line of printable text, no spaces at either end, at most max_bytes.
    // Empty when nothing is left.
    std::string cleanName(std::string_view text, size_t max_bytes)
    {
        std::string out = LLStringFn::strip_invalid_xml(std::string(text));
        for (char& c : out)
        {
            if (c == '\n' || c == '\r' || c == '\t')
            {
                c = ' ';
            }
        }
        LLStringUtil::trim(out);
        out = truncated(out, max_bytes);
        LLStringUtil::trimTail(out);
        return out;
    }

    // An autoresponse: printable text (lines kept), at most an IM's length.
    std::string cleanMessage(std::string_view text)
    {
        return truncated(LLStringFn::strip_invalid_xml(std::string(text)), MAX_AUTORESPONSE_BYTES);
    }

    // A colour of the file: 3 or 4 finite numbers, each held to 0..1; anything else is fallback.
    LLColor4 colorFrom(const LLSD& sd, const LLColor4& fallback)
    {
        if (!sd.isArray() || sd.size() < 3)
        {
            return fallback;
        }
        LLColor4 color(LLColor4::white);
        for (S32 i = 0; i < 4 && i < (S32)sd.size(); ++i)
        {
            const F64 value = sd[i].asReal();
            if (!std::isfinite(value))
            {
                return fallback;
            }
            color.mV[i] = (F32)llclamp(value, 0.0, 1.0);
        }
        return color;
    }

    // An avatar's id of the file (a map's key); null when it is not one.
    LLUUID idFrom(const std::string& text)
    {
        return LLUUID::validate(text) ? LLUUID(text) : LLUUID::null;
    }

    // The smallest of the sets friend_id is in that take(name, set) accepts; of the smallest ones, the first by
    // name (the colour, the autoresponse: Firestorm's rule). Null when there is none. No copies.
    template<typename Sets, typename Take>
    const LGGContactSets::ContactSet* smallestSetOf(const Sets& sets, const LLUUID& friend_id, Take take)
    {
        const LGGContactSets::ContactSet* smallest = nullptr;
        for (const auto& [name, set] : sets)
        {
            if (set->hasFriend(friend_id) && (!smallest || set->mFriends.size() < smallest->mFriends.size())
                && take(name, *set))
            {
                smallest = set;
            }
        }
        return smallest;
    }

    // The name cache asks for the alias of every name it gives out: no lock, no copy when there is none.
    LGGContactSets* sNameCheckInstance = nullptr;

    bool checkCustomNameOf(const LLUUID& id, bool& dn_removed, std::string& custom_name)
    {
        return sNameCheckInstance && sNameCheckInstance->checkCustomName(id, dn_removed, custom_name);
    }
}

LGGContactSets::LGGContactSets()
{
}

LGGContactSets::~LGGContactSets()
{
    clearAll();     // no saving here (every change was saved when made), no other singleton read
}

void LGGContactSets::cleanupSingleton()
{
    disconnectNameRequests();
    if (sNameCheckInstance == this)
    {
        sNameCheckInstance = nullptr;
        if (LLAvatarNameCache::instanceExists())
        {
            LLAvatarNameCache::getInstance()->setCustomNameCheckCallback(nullptr);
        }
    }
}

void LGGContactSets::disconnectNameRequests()
{
    for (auto& [id, connection] : mAvatarNameCacheConnections)
    {
        if (connection.connected())
        {
            connection.disconnect();
        }
    }
    mAvatarNameCacheConnections.clear();
}

void LGGContactSets::toneDownColor(LLColor4& color) const
{
    color.mV[VALPHA] = COLOR_DAMPENING;
}

void LGGContactSets::markChanged()
{
    mSetColorCache.clear();
}

bool LGGContactSets::callbackAliasReset(const LLSD& notification, const LLSD& response)
{
    if (S32 option = LLNotification::getSelectedOption(notification, response); option == 0)
    {
        clearPseudonym(notification["payload"]["agent_id"].asUUID());
    }
    return false;
}

std::string LGGContactSets::getFilename() const
{
    if (gDirUtilp->getExpandedFilename(LL_PATH_PER_SL_ACCOUNT, "").empty())
    {
        return std::string();   // no account yet
    }
    return gDirUtilp->getExpandedFilename(LL_PATH_PER_SL_ACCOUNT, plus::contactsets::FILE_NAME);
}

std::string LGGContactSets::getDefaultFilename() const
{
    if (gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "").empty())
    {
        return std::string();
    }
    return gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, plus::contactsets::FILE_NAME);
}

void LGGContactSets::clearAll()
{
    disconnectNameRequests();
    for (auto& [name, set] : mContactSets)
    {
        delete set;
    }
    mContactSets.clear();
    mExtraAvatars.clear();
    mPseudonyms.clear();
    mDefaultColor = LLColor4::grey;
}

void LGGContactSets::loadFromDisk()
{
    const bool had_pseudonyms = !mPseudonyms.empty();
    clearAll();

    const std::string filename(getFilename());
    if (filename.empty())
    {
        LL_WARNS("ContactSets") << "No account folder: no contact sets" << LL_ENDL;
    }
    else
    {
        LLSD data;
        switch (plus::contactsets::readFile(filename, data))
        {
            case plus::contactsets::ReadResult::ok:
                importFromLLSD(data);
                break;
            case plus::contactsets::ReadResult::unreadable:
            {
                // Kept for the user (the next save would write over it); the sets start empty.
                const std::string bad = plus::putAside(filename, 5);
                LL_WARNS("ContactSets") << "The contact sets file cannot be read: "
                                        << (bad.empty() ? "it could not be moved aside either" : "moved aside to .bad")
                                        << "; starting with no contact sets" << LL_ENDL;
                break;
            }
            case plus::contactsets::ReadResult::missing:
            {
                // As Firestorm: the default file of the viewer (none is shipped), then a file of ours.
                LLSD defaults;
                const std::string default_name(getDefaultFilename());
                if (!default_name.empty()
                    && plus::contactsets::readFile(default_name, defaults) == plus::contactsets::ReadResult::ok)
                {
                    importFromLLSD(defaults);
                }
                saveToDisk();
                break;
            }
        }
    }

    LL_INFOS("ContactSets") << mContactSets.size() << " contact sets, " << mExtraAvatars.size() << " people who are not friends, "
                            << mPseudonyms.size() << " aliases" << LL_ENDL;
    markChanged();
    if (had_pseudonyms || !mPseudonyms.empty())
    {
        LLVOAvatar::invalidateNameTags();
    }
    mChangedSignal(UPDATED_LISTS);
}

void LGGContactSets::saveToDisk()
{
    const std::string filename(getFilename());
    if (filename.empty())
    {
        LL_WARNS("ContactSets") << "No account folder: the contact sets are not saved" << LL_ENDL;
        return;
    }
    const LLSD data = exportToLLSD();
    const plus::FileWrite wrote =
        plus::writeFileReplacing(filename, [&data](std::ostream& out) { LLSDSerialize::toPrettyXML(data, out); });
    if (wrote != plus::FileWrite::written)
    {
        // The sets stay as they are in this session; the next change tries again.
        LL_WARNS("ContactSets") << "The contact sets could not be saved ("
                                << (wrote == plus::FileWrite::cannot_replace ? "the file could not be replaced" : "could not write")
                                << ")" << LL_ENDL;
    }
}

LLSD LGGContactSets::exportToLLSD()
{
    LLSD output;

    // Global settings
    output[CS_GLOBAL_SETTINGS]["defaultColor"] = mDefaultColor.getValue();

    // Extra avatars
    for (const auto& avid : mExtraAvatars)
    {
        output[CS_SET_EXTRA_AVS][avid.asString()] = "";
    }

    // Pseudonyms
    for (const auto& [avid, pseudonym] : mPseudonyms)
    {
        output[CS_SET_PSEUDONYM][avid.asString()] = pseudonym;
    }

    // Contact Sets
    for (const auto& [name, set] : mContactSets)
    {
        output[name]["color"] = set->mColor.getValue();
        output[name]["notify"] = set->mNotify;
        output[name]["sort_by_online_status"] = set->mSortByOnlineStatus;
        output[name]["autoresponse_busy_enabled"] = set->mAutoresponseBusyEnabled;
        output[name]["autoresponse_busy"] = set->mAutoresponseBusy;
        output[name]["autoresponse_mode_enabled"] = set->mAutoresponseModeEnabled;
        output[name]["autoresponse_mode"] = set->mAutoresponseMode;
        output[name]["autoresponse_nonfriends_enabled"] = set->mAutoresponseNonFriendsEnabled;
        output[name]["autoresponse_nonfriends"] = set->mAutoresponseNonFriends;
        output[name]["friends"] = LLSD::emptyMap();
        for (const auto& friend_id : set->mFriends)
        {
            output[name]["friends"][friend_id.asString()] = "";
        }
    }

    return output;
}

// Every value is checked: the file may come from another viewer or have been edited (research/287 P09).
void LGGContactSets::importFromLLSD(const LLSD& data)
{
    if (!data.isMap())
    {
        return;
    }
    size_t ids = 0;
    size_t left_out = 0;    // entries not taken (not valid, or past a limit): a count for the log
    auto takeId = [&ids, &left_out](const std::string& key) -> LLUUID
    {
        const LLUUID id = idFrom(key);
        if (id.isNull() || ids >= MAX_IDS)
        {
            ++left_out;
            return LLUUID::null;
        }
        ++ids;
        return id;
    };

    // The default colour first: a set without a colour of its own takes it.
    if (const LLSD& global = data[CS_GLOBAL_SETTINGS]; global.isMap() && global.has("defaultColor"))
    {
        mDefaultColor = colorFrom(global["defaultColor"], LLColor4::grey);
    }

    for (const auto& [key, value] : llsd::inMap(data))
    {
        if (key == CS_GLOBAL_SETTINGS)
        {
            continue;
        }
        if (key == CS_SET_EXTRA_AVS)
        {
            for (const auto& [av_key, av_value] : llsd::inMap(value))
            {
                if (const LLUUID id = takeId(av_key); id.notNull())
                {
                    mExtraAvatars.insert(id);
                }
            }
            continue;
        }
        if (key == CS_SET_PSEUDONYM)
        {
            for (const auto& [av_key, av_value] : llsd::inMap(value))
            {
                const std::string pseudonym = cleanName(av_value.asString(), MAX_PSEUDONYM_BYTES);
                if (pseudonym.empty())
                {
                    ++left_out;
                    continue;
                }
                if (const LLUUID id = takeId(av_key); id.notNull())
                {
                    mPseudonyms[id] = pseudonym;
                }
            }
            continue;
        }
        if (isInternalSetName(key))
        {
            continue;   // "All Sets", "No Sets": names of the lists, never sets (as Firestorm)
        }

        const std::string name = cleanName(key, MAX_SET_NAME_BYTES);
        if (!value.isMap() || name.empty() || isInternalSetName(name) || isValidSet(name) || mContactSets.size() >= MAX_SETS)
        {
            ++left_out;
            continue;
        }
        const LLSD& set_data = value;

        ContactSet* new_set = new ContactSet();
        new_set->mName = name;
        new_set->mColor = set_data.has("color") ? colorFrom(set_data["color"], getDefaultColor()) : getDefaultColor();
        new_set->mNotify = set_data["notify"].asBoolean();
        new_set->mSortByOnlineStatus = set_data.has("sort_by_online_status") ? set_data["sort_by_online_status"].asBoolean() : true;
        new_set->mAutoresponseBusyEnabled = set_data["autoresponse_busy_enabled"].asBoolean();
        new_set->mAutoresponseBusy = cleanMessage(set_data["autoresponse_busy"].asString());
        new_set->mAutoresponseModeEnabled = set_data["autoresponse_mode_enabled"].asBoolean();
        new_set->mAutoresponseMode = cleanMessage(set_data["autoresponse_mode"].asString());
        new_set->mAutoresponseNonFriendsEnabled = set_data["autoresponse_nonfriends_enabled"].asBoolean();
        new_set->mAutoresponseNonFriends = cleanMessage(set_data["autoresponse_nonfriends"].asString());
        for (const auto& [av_key, av_value] : llsd::inMap(set_data["friends"]))
        {
            if (const LLUUID id = takeId(av_key); id.notNull())
            {
                new_set->mFriends.emplace(id);
            }
        }

        mContactSets[name] = new_set;
    }

    if (left_out)
    {
        LL_WARNS("ContactSets") << left_out << " entries of the contact sets file were left out (not valid, or past "
                                << MAX_SETS << " sets / " << MAX_IDS << " people)" << LL_ENDL;
    }
}

LLColor4 LGGContactSets::getSetColor(std::string_view set_name) const
{
    if (ContactSet* set = getContactSet(set_name); set)
    {
        return set->mColor;
    }

    return getDefaultColor();
};

const LGGContactSets::CachedSetColor& LGGContactSets::cachedSetColor(const LLUUID& friend_id) const
{
    if (auto found = mSetColorCache.find(friend_id); found != mSetColorCache.end())
    {
        return found->second;
    }
    if (mSetColorCache.size() >= MAX_COLOR_CACHE)
    {
        mSetColorCache.clear();     // many avatars asked about: made again as they are asked
    }
    CachedSetColor entry;
    if (const ContactSet* smallest = smallestSetOf(mContactSets, friend_id, [](const std::string&, const ContactSet&) { return true; }))
    {
        entry.mInSet = true;
        entry.mColor = smallest->mColor;
    }
    return mSetColorCache.emplace(friend_id, entry).first->second;
}

// The colour of the smallest of the friend's sets (the first by name of the smallest ones), set_name left out
// (toned down for one who is not a friend); when the friend is in no other set, that of ignored_set_name itself.
LLColor4 LGGContactSets::getFriendColor(const LLUUID& friend_id, std::string_view ignored_set_name) const
{
    LLColor4 color = getDefaultColor();
    if (ignored_set_name == CS_SET_NO_SETS || mContactSets.empty())
    {
        return color;
    }

    if (ignored_set_name.empty())
    {
        if (const CachedSetColor& cached = cachedSetColor(friend_id); cached.mInSet)
        {
            color = cached.mColor;
            if (isNonFriend(friend_id))
            {
                toneDownColor(color);
            }
        }
        return color;
    }

    const ContactSet* smallest = smallestSetOf(mContactSets, friend_id,
                                               [ignored_set_name](const std::string& name, const ContactSet&) { return name != ignored_set_name; });
    if (smallest)
    {
        color = smallest->mColor;
        if (isNonFriend(friend_id))
        {
            toneDownColor(color);
        }
    }
    else if (const ContactSet* ignored = getContactSet(ignored_set_name); ignored && ignored->hasFriend(friend_id))
    {
        color = ignored->mColor;
    }
    return color;
}

bool LGGContactSets::hasFriendColorThatShouldShow(const LLUUID& friend_id, ContactSetType type) const
{
    LLColor4 color = LLColor4::white;
    return hasFriendColorThatShouldShow(friend_id, type, color);
}

// handle all settings and rlv that would prevent us from showing the cs color
bool LGGContactSets::hasFriendColorThatShouldShow(const LLUUID& friend_id, ContactSetType type, LLColor4& color) const
{
    if (mContactSets.empty())
    {
        return false;   // no sets (the default): nothing to look up, every frame
    }

    static LLCachedControl<bool> fsContactSetsColorizeChat(gSavedSettings, "FSContactSetsColorizeChat", false);
    static LLCachedControl<bool> fsContactSetsColorizeTag(gSavedSettings,"FSContactSetsColorizeNameTag", false);
    static LLCachedControl<bool> fsContactSetsColorizeRadar(gSavedSettings,"FSContactSetsColorizeRadar", false);
    static LLCachedControl<bool> fsContactSetsColorizeMiniMap(gSavedSettings,"FSContactSetsColorizeMiniMap", false);
    static LLCachedControl<bool> fsContactSetsColorizeFriends(gSavedSettings,"FSContactSetsColorizeFriends", false);

    switch (type)
    {
        case ContactSetType::CHAT:
        case ContactSetType::IM:
            if (!fsContactSetsColorizeChat)
                return false;
            break;
        case ContactSetType::TAG:
            if (!fsContactSetsColorizeTag)
                return false;
            break;
        case ContactSetType::RADAR:
            if (!fsContactSetsColorizeRadar)
                return false;
            break;
        case ContactSetType::MINIMAP:
            if (!fsContactSetsColorizeMiniMap)
                return false;
            break;
        case ContactSetType::FRIENDS:
            if (!fsContactSetsColorizeFriends)
                return false;
            break;
    };

    if (!plus::rlv::canShowMenuName(friend_id))
    {
        return false; // don't show colors if we cant show names
    }

    /// don't show friend color if they are no longer a friend
    /// (and if are also not on the "non friends" list)
    if ((!LLAvatarTracker::instance().isBuddy(friend_id)) && (!isNonFriend(friend_id)))
    {
        return false;
    }

    LLColor4 friend_color = getFriendColor(friend_id);
    if (friend_color == getDefaultColor())
    {
        return false;
    }

    color = friend_color;

    return true;
}

LGGContactSets::string_vec_t LGGContactSets::getFriendSets(const LLUUID& friend_id) const
{
    string_vec_t sets{};

    for (const auto& [set_name, set] : mContactSets)
    {
        if (set->hasFriend(friend_id))
        {
            sets.emplace_back(set->mName);
        }
    }
    return sets;
}

LGGContactSets::string_vec_t LGGContactSets::getAllContactSets() const
{
    string_vec_t sets{};
    sets.reserve(mContactSets.size());

    for (const auto& [set_name, set] : mContactSets)
    {
        sets.push_back(set->mName);
    }

    return sets;
}

uuid_vec_t LGGContactSets::getFriendsInAnySet() const
{
    uuid_set_t friendsInAnySet{};

    for (const auto& [set_name, set] : mContactSets)
    {
        friendsInAnySet.insert(set->mFriends.begin(), set->mFriends.end());
    }

    return uuid_vec_t(friendsInAnySet.begin(), friendsInAnySet.end());
}

bool LGGContactSets::isFriendInAnySet(const LLUUID& friend_id) const
{
    for (const auto& [set_name, set] : mContactSets)
    {
        if (set->hasFriend(friend_id))
        {
            return true;
        }
    }

    return false;
}

bool LGGContactSets::notifyForFriend(const LLUUID& friend_id) const
{
    for (const auto& [set_name, set] : mContactSets)
    {
        if (set->mNotify && set->hasFriend(friend_id))
        {
            return true;
        }
    }
    return false;
}

void LGGContactSets::addToSet(const uuid_vec_t& avatar_ids, std::string_view set_name)
{
    LLAvatarTracker& tracker = LLAvatarTracker::instance();
    ContactSet* set = getContactSet(set_name);

    for (auto const& avatar_id : avatar_ids)
    {
        if (avatar_id.isNull())
        {
            continue;
        }
        if (!tracker.isBuddy(avatar_id))
        {
            mExtraAvatars.insert(avatar_id);
        }

        if (set)
        {
            set->mFriends.insert(avatar_id);
        }
    }

    markChanged();
    saveToDisk();
    mChangedSignal(UPDATED_MEMBERS);
}

void LGGContactSets::removeNonFriendFromList(const LLUUID& non_friend_id, bool save_changes /*= true*/)
{
    LLAvatarTracker& tracker = LLAvatarTracker::instance();
    if (auto found = mExtraAvatars.find(non_friend_id); found != mExtraAvatars.end())
    {
        mExtraAvatars.erase(found);
        markChanged();

        if (!tracker.isBuddy(non_friend_id))
        {
            clearPseudonym(non_friend_id, save_changes);
            removeFriendFromAllSets(non_friend_id, save_changes);
        }

        if (save_changes)
        {
            saveToDisk();
            mChangedSignal(UPDATED_MEMBERS);
        }
    }
}

void LGGContactSets::removeFriendFromAllSets(const LLUUID& friend_id, bool save_changes /*= true*/)
{
    for (const auto& set_name : getFriendSets(friend_id))
    {
        removeFriendFromSet(friend_id, set_name, save_changes);
    }
}

bool LGGContactSets::isNonFriend(const LLUUID& non_friend_id) const
{
    if (mExtraAvatars.empty() || LLAvatarTracker::instance().isBuddy(non_friend_id))
    {
        return false;
    }

    return (mExtraAvatars.find(non_friend_id) != mExtraAvatars.end());
}

uuid_vec_t LGGContactSets::getListOfPseudonymAvs() const
{
    uuid_vec_t pseudonyms{};
    pseudonyms.reserve(mPseudonyms.size());

    for (const auto& [id, pseudonym] : mPseudonyms)
    {
        pseudonyms.emplace_back(id);
    }

    return pseudonyms;
}

void LGGContactSets::signalWhenNameKnown(const LLUUID& av_id)
{
    if (LLAvatarName av_name; LLAvatarNameCache::get(av_id, &av_name))
    {
        mChangedSignal(UPDATED_MEMBERS);
        return;
    }

    if (auto it = mAvatarNameCacheConnections.find(av_id); it != mAvatarNameCacheConnections.end())
    {
        if (it->second.connected())
        {
            it->second.disconnect();
        }
        mAvatarNameCacheConnections.erase(it);
    }
    mAvatarNameCacheConnections.try_emplace(av_id, LLAvatarNameCache::get(av_id, boost::bind(&LGGContactSets::onAvatarNameCache, this, _1)));
}

void LGGContactSets::setPseudonym(const LLUUID& friend_id, std::string_view pseudonym)
{
    const std::string clean = cleanName(pseudonym, MAX_PSEUDONYM_BYTES);
    if (friend_id.isNull())
    {
        return;
    }
    if (clean.empty())
    {
        clearPseudonym(friend_id);      // an empty alias is none (Firestorm's showed '')
        return;
    }
    mPseudonyms[friend_id] = clean;
    markChanged();
    // The name cache asks for the alias whenever it gives the name out: the tag is made again with it.
    LLVOAvatar::invalidateNameTag(friend_id);
    saveToDisk();
    signalWhenNameKnown(friend_id);
}

void LGGContactSets::clearPseudonym(const LLUUID& friend_id, bool save_changes /*= true*/)
{
    if (auto found = mPseudonyms.find(friend_id); found != mPseudonyms.end())
    {
        mPseudonyms.erase(found);
        markChanged();
        LLVOAvatar::invalidateNameTag(friend_id);
        if (!LLAvatarTracker::instance().isBuddy(friend_id) && !isFriendInAnySet(friend_id))
        {
            removeNonFriendFromList(friend_id, save_changes);
        }

        signalWhenNameKnown(friend_id);

        if (save_changes)
        {
            saveToDisk();
        }
    }
}

void LGGContactSets::onAvatarNameCache(const LLUUID& av_id)
{
    if (auto it = mAvatarNameCacheConnections.find(av_id); it != mAvatarNameCacheConnections.end())
    {
        if (it->second.connected())
        {
            it->second.disconnect();
        }
        mAvatarNameCacheConnections.erase(it);
    }
    mChangedSignal(UPDATED_MEMBERS);
}

bool LGGContactSets::hasPseudonym(const LLUUID& friend_id) const
{
    return !mPseudonyms.empty() && mPseudonyms.find(friend_id) != mPseudonyms.end();
}

bool LGGContactSets::hasPseudonym(const uuid_vec_t& ids) const
{
    return std::any_of(ids.begin(), ids.end(), [this](const LLUUID& id) { return hasPseudonym(id); });
}

bool LGGContactSets::hasDisplayNameRemoved(const LLUUID& friend_id) const
{
    if (auto found = mPseudonyms.find(friend_id); found != mPseudonyms.end())
    {
        return found->second == CS_PSEUDONYM;
    }
    return false;
}

bool LGGContactSets::hasDisplayNameRemoved(const uuid_vec_t& ids) const
{
    return std::any_of(ids.begin(), ids.end(), [this](const LLUUID& id) { return hasDisplayNameRemoved(id); });
}

void LGGContactSets::removeDisplayName(const LLUUID& friend_id)
{
    setPseudonym(friend_id, CS_PSEUDONYM);
}

void LGGContactSets::removeFriendFromSet(const LLUUID& friend_id, std::string_view set_name, bool save_changes /*= true*/)
{
    if (set_name == CS_SET_EXTRA_AVS)
    {
        return removeNonFriendFromList(friend_id, save_changes);
    }
    else if (set_name == CS_SET_PSEUDONYM)
    {
        return clearPseudonym(friend_id, save_changes);
    }

    if (ContactSet* set = getContactSet(set_name); set)
    {
        set->mFriends.erase(friend_id);
        markChanged();
        if (save_changes)
        {
            saveToDisk();
            mChangedSignal(UPDATED_MEMBERS);
        }
    }
}

bool LGGContactSets::isValidSet(std::string_view set_name) const
{
    return !set_name.empty() && mContactSets.find(set_name) != mContactSets.end();
}

void LGGContactSets::addSet(std::string_view set_name)
{
    const std::string name = cleanName(set_name, MAX_SET_NAME_BYTES);
    if (!isInternalSetName(name) && !isValidSet(name) && mContactSets.size() < MAX_SETS)
    {
        ContactSet* set = new ContactSet();
        set->mName = name;
        set->mColor = getDefaultColor();
        set->mNotify = false;
        set->mSortByOnlineStatus = true;
        set->mAutoresponseBusyEnabled = false;
        set->mAutoresponseBusy.clear();
        set->mAutoresponseModeEnabled = false;
        set->mAutoresponseMode.clear();
        set->mAutoresponseNonFriendsEnabled = false;
        set->mAutoresponseNonFriends.clear();
        mContactSets[name] = set;
        markChanged();
        saveToDisk();
        mChangedSignal(UPDATED_LISTS);
    }
}

bool LGGContactSets::renameSet(std::string_view set_name, std::string_view new_set_name)
{
    const std::string old_name(set_name);
    const std::string new_name = cleanName(new_set_name, MAX_SET_NAME_BYTES);
    if (!isInternalSetName(old_name) && isValidSet(old_name) &&
        !isInternalSetName(new_name) && !isValidSet(new_name))
    {
        ContactSet* set = getContactSet(old_name);
        set->mName = new_name;
        mContactSets.erase(old_name);
        mContactSets[new_name] = set;
        markChanged();
        saveToDisk();
        mChangedSignal(UPDATED_LISTS);
        return true;
    }
    return false;
}

void LGGContactSets::removeSet(std::string_view set_name)
{
    contact_set_map_t::iterator found = mContactSets.find(set_name);
    if (found != mContactSets.end())
    {
        LLAvatarTracker& tracker = LLAvatarTracker::instance();
        uuid_vec_t non_friends_for_removal;
        ContactSet* cset = found->second;
        for (const auto& friend_id : cset->mFriends)
        {
            if (!tracker.isBuddy(friend_id) &&
                getFriendSets(friend_id).size() == 1 && // Current set is only set!
                !hasPseudonym(friend_id))
            {
                non_friends_for_removal.emplace_back(friend_id);
            }
        }

        for (const auto& non_friend_id : non_friends_for_removal)
        {
            removeNonFriendFromList(non_friend_id, false);
        }

        // Looked up again: removeNonFriendFromList() above took the members out of this set (the map's
        // node and the set itself stay, but the iterator is not relied on).
        found = mContactSets.find(set_name);
        if (found != mContactSets.end())
        {
            delete found->second;
            mContactSets.erase(found);
        }
        markChanged();
        saveToDisk();
        mChangedSignal(UPDATED_LISTS);
    }
}

void LGGContactSets::setNotifyForSet(std::string_view set_name, bool notify)
{
    ContactSet* set = getContactSet(set_name);
    if (set)
    {
        set->mNotify = notify;
        markChanged();
        saveToDisk();
    }
}

bool LGGContactSets::getNotifyForSet(std::string_view set_name) const
{
    if (ContactSet* set = getContactSet(set_name); set)
    {
        return set->mNotify;
    }
    return false;
}

void LGGContactSets::setSortByOnlineStatusForSet(std::string_view set_name, bool sort_by_online_status)
{
    ContactSet* set = getContactSet(set_name);
    if (set)
    {
        set->mSortByOnlineStatus = sort_by_online_status;
        markChanged();
        saveToDisk();
        mChangedSignal(UPDATED_LISTS);      // <plus> the list is ordered again (Firestorm: UPDATED_MEMBERS) </plus>
    }
}

bool LGGContactSets::getSortByOnlineStatusForSet(std::string_view set_name) const
{
    if (ContactSet* set = getContactSet(set_name); set)
    {
        return set->mSortByOnlineStatus;
    }
    return false;
}

void LGGContactSets::setAutoresponseForSet(std::string_view set_name, ContactSetAutoresponseMode mode, bool enabled, std::string_view response)
{
    if (ContactSet* set = getContactSet(set_name); set)
    {
        contact_set_autoresponse_ref_t autoresponse = get_autoresponse_ref(set, mode);
        if (autoresponse.enabled && autoresponse.message)
        {
            *autoresponse.enabled = enabled;
            *autoresponse.message = cleanMessage(response);
            markChanged();
            saveToDisk();
        }
    }
}

void LGGContactSets::getAutoresponseForSet(std::string_view set_name, ContactSetAutoresponseMode mode, bool& enabled, std::string& response) const
{
    enabled = false;
    response.clear();
    if (const ContactSet* set = getContactSet(set_name); set)
    {
        const_contact_set_autoresponse_ref_t autoresponse = get_autoresponse_ref(set, mode);
        if (autoresponse.enabled && autoresponse.message)
        {
            enabled = *autoresponse.enabled;
            response = *autoresponse.message;
        }
    }
}

// The message of the smallest of the friend's sets that has one for that mode (as the colour).
bool LGGContactSets::getAutoresponseForFriend(const LLUUID& friend_id, ContactSetAutoresponseMode mode, std::string& response) const
{
    response.clear();
    const ContactSet* best = smallestSetOf(mContactSets, friend_id, [mode](const std::string&, const ContactSet& set)
    {
        const_contact_set_autoresponse_ref_t autoresponse = get_autoresponse_ref(&set, mode);
        return autoresponse.enabled && autoresponse.message && *autoresponse.enabled && !autoresponse.message->empty();
    });
    if (best)
    {
        response = *get_autoresponse_ref(best, mode).message;
    }
    return best != nullptr;
}

void LGGContactSets::setSetColor(std::string_view set_name, const LLColor4& color)
{
    if (ContactSet* set = getContactSet(set_name); set)
    {
        set->mColor = color;
        markChanged();
        saveToDisk();
        mChangedSignal(UPDATED_MEMBERS);    // the lists show the new colour (Firestorm's showed it at their next update)
    }
}

void LGGContactSets::setDefaultColor(const LLColor4& default_color)
{
    mDefaultColor = default_color;
    markChanged();
    saveToDisk();
    mChangedSignal(UPDATED_MEMBERS);
}

bool LGGContactSets::isInternalSetName(std::string_view set_name) const
{
    return (set_name.empty() ||
            set_name == CS_SET_EXTRA_AVS ||
            set_name == CS_SET_PSEUDONYM ||
            set_name == CS_SET_NO_SETS ||
            set_name == CS_SET_ALL_SETS ||
            set_name == CS_GLOBAL_SETTINGS);
}

// No warning for a name that is not a set: the lists ask with their own names ("All Sets") too, and a set's
// name is the user's (not for the log).
LGGContactSets::ContactSet* LGGContactSets::getContactSet(std::string_view set_name) const
{
    if (set_name.empty())
    {
        return nullptr;
    }

    if (auto found = mContactSets.find(set_name); found != mContactSets.end())
    {
        return found->second;
    }
    return nullptr;
}

// The name cache's question for every name it gives out (llavatarnamecache.cpp): false at once with no aliases.
bool LGGContactSets::checkCustomName(const LLUUID& id, bool& dn_removed, std::string& pseudonym) const
{
    if (mPseudonyms.empty())
    {
        return false;
    }
    auto found = mPseudonyms.find(id);
    if (found == mPseudonyms.end())
    {
        return false;
    }
    dn_removed = (found->second == CS_PSEUDONYM);
    pseudonym = "'" + found->second + "'";
    return true;
}

// static
bool LGGContactSets::handleAddContactSetCallback(const LLSD& notification, const LLSD& response)
{
    if (S32 option = LLNotificationsUtil::getSelectedOption(notification, response); option == 0)
    {
        const std::string set_name = response["message"].asString();
        LGGContactSets::getInstance()->addSet(set_name);
    }
    return false;
}

// static
bool LGGContactSets::handleRemoveContactSetCallback(const LLSD& notification, const LLSD& response)
{
    if (S32 option = LLNotificationsUtil::getSelectedOption(notification, response); option == 0)
    {
        LGGContactSets::getInstance()->removeSet(notification["payload"]["contact_set"].asString());
    }
    return false;
}

// static
bool LGGContactSets::handleRemoveAvatarFromSetCallback(const LLSD& notification, const LLSD& response)
{
    if (S32 option = LLNotificationsUtil::getSelectedOption(notification, response); option == 0)
    {
        LGGContactSets& instance = LGGContactSets::instance();
        LLAvatarTracker& tracker = LLAvatarTracker::instance();

        const std::string set_name = notification["payload"]["contact_set"].asString();
        for (const auto& item : llsd::inArray(notification["payload"]["ids"]))
        {
            const LLUUID id = item.asUUID();

            instance.removeFriendFromSet(id, set_name, false);

            if (!tracker.isBuddy(id) &&
                !instance.isFriendInAnySet(id) &&
                !instance.hasPseudonym(id))
            {
                instance.removeNonFriendFromList(id, false);
            }
        }

        instance.saveToDisk();
        instance.mChangedSignal(UPDATED_MEMBERS);
    }
    return false;
}

// static
bool LGGContactSets::handleSetAvatarPseudonymCallback(const LLSD& notification, const LLSD& response)
{
    if (S32 option = LLNotificationsUtil::getSelectedOption(notification, response); option == 0)
    {
        const std::string pseudonym(response["message"].asString());
        LGGContactSets* contact_sets = LGGContactSets::getInstance();
        if (notification["payload"].has("ids"))
        {
            for (const auto& item : llsd::inArray(notification["payload"]["ids"]))
            {
                const LLUUID id(item.asUUID());
                contact_sets->setPseudonym(id, pseudonym);
            }
        }
        else
        {
            const LLUUID id(notification["payload"]["id"].asUUID());
            contact_sets->setPseudonym(id, pseudonym);
        }
    }
    return false;
}

// <plus> The name cache's alias check (plus/contactsets/contactsets.cpp, at login).
namespace plus::contactsets
{
    void giveNameCacheTheAliases()
    {
        LGGContactSets* instance = LGGContactSets::getInstance();
        sNameCheckInstance = instance;
        LLAvatarNameCache::getInstance()->setCustomNameCheckCallback(&checkCustomNameOf);
    }
}
// </plus>

/**
 * @file grouptitles.cpp
 * @brief Group titles: activating one, and Auto Region (the title given to each region)
 *        (plus/grouptitles, from Firestorm's fsgrouptitleregionmgr.cpp).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Copyright (c) 2026 The Phoenix Firestorm Project, Inc.
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

// The file (Firestorm's region_group_titles.xml, per account):
//   { none_on_unassigned: bool,
//     assignments: { <region name, lower case>: { group_id: uuid, role_id: uuid, display_name: string } } }
// What it holds is checked as it is read (it may have been edited by hand or by another viewer): names are
// made the way a typed one is (control characters out, spaces collapsed, at most MAX_REGION_NAME_BYTES), keys
// that are not such a name are dropped, a role without a group is no role, at most MAX_ASSIGNMENTS. A file
// that does not parse is put aside as <file>.bad (not overwritten by the next save).
//
// When it applies: the agent's region-changed signal (login, teleport, crossing). A region whose name is not
// known yet (before its handshake) or, at login, a group list not received yet is looked at again every
// RETRY_SECONDS, at most MAX_RETRIES times; and once LOGIN_RETRY_SECONDS after the file was read, as
// Firestorm does (the active group and the group list come in after the first region). A title of a group
// the agent is not in is skipped (not looked at again). A title is applied once per region:
// changing it by hand in that region keeps it until the next region.

#include "llviewerprecompiledheaders.h"

#include "grouptitles.h"
#include "plus.h"
#include "plus/rlv/rlvhooks.h"

#include "llagent.h"
#include "llcallbacklist.h"
#include "lldir.h"
#include "llevents.h"
#include "llfile.h"
#include "llgroupactions.h"
#include "llgroupmgr.h"
#include "llnotificationsutil.h"
#include "llsdserialize.h"
#include "llstartup.h"
#include "llviewerregion.h"
#include "llworldmap.h"
#include "llworldmapmessage.h"

#include <algorithm>
#include <map>

namespace plus::grouptitles
{
    namespace
    {
        constexpr char   FILE_NAME[]            = "region_group_titles.xml";   // Firestorm's name
        constexpr size_t MAX_REGION_NAME_BYTES  = 63;                          // Firestorm's limit
        constexpr size_t MAX_ASSIGNMENTS        = 1000;
        constexpr F32    VALIDATION_SECONDS     = 10.f;
        constexpr F32    RETRY_SECONDS          = 2.f;
        constexpr int    MAX_RETRIES            = 15;
        constexpr F32    LOGIN_RETRY_SECONDS    = 5.f;
        constexpr char   NOTIFY_ASK[]           = "PlusGroupTitleRegion";
        constexpr char   NOTIFY_NOT_FOUND[]     = "PlusGroupTitleRegionNotFound";

        struct Assignment
        {
            LLUUID      group_id;
            LLUUID      role_id;
            std::string display_name;   // as the grid spells the region
        };

        struct State
        {
            std::map<std::string, Assignment> assignments;     // by normalizeRegionName()
            bool        none_on_unassigned = false;
            bool        loaded = false;             // the account's file was read (this login)
            bool        following = false;          // the region-changed callback is in
            std::string last_applied;               // normalizeRegionName() of the region last applied
            U32         region_generation = 0;      // a retry made for an older region change does nothing
            int         retries = 0;

            // askRegionName(): the map's name lookup in flight
            bool        validating = false;
            U32         validation_generation = 0;
            LLUUID      pending_group;
            LLUUID      pending_role;
            std::string pending_name;

            boost::signals2::signal<void()> changed;
        };

        State& state()
        {
            static State s;
            return s;
        }

        // ---- names ----------------------------------------------------------------------------

        // A region name as typed or read: control characters out, runs of spaces (and tabs) one space, none
        // at either end, at most MAX_REGION_NAME_BYTES (cut at a character's start, not inside UTF-8).
        std::string sanitizeRegionName(const std::string& input)
        {
            std::string out;
            out.reserve(input.size());
            bool space = false;
            for (const char ch : input)
            {
                const unsigned char c = static_cast<unsigned char>(ch);
                if (c == ' ' || c == '\t')
                {
                    space = !out.empty();
                    continue;
                }
                if (c < 32 || c == 127)
                {
                    continue;
                }
                if (space)
                {
                    out += ' ';
                    space = false;
                }
                out += ch;
            }
            if (out.size() > MAX_REGION_NAME_BYTES)
            {
                size_t cut = MAX_REGION_NAME_BYTES;
                while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80)
                {
                    --cut;
                }
                out.resize(cut);
                while (!out.empty() && out.back() == ' ')
                {
                    out.pop_back();
                }
            }
            return out;
        }

        // The key of a region: its sanitized name in lower case (region names are not case sensitive).
        std::string normalizeRegionName(const std::string& name)
        {
            std::string key = sanitizeRegionName(name);
            LLStringUtil::toLower(key);
            return key;
        }

        // ---- the file -------------------------------------------------------------------------

        std::string filePath()
        {
            if (gDirUtilp->getExpandedFilename(LL_PATH_PER_SL_ACCOUNT, "").empty())
            {
                return std::string();   // no account yet
            }
            return gDirUtilp->getExpandedFilename(LL_PATH_PER_SL_ACCOUNT, FILE_NAME);
        }

        // A file that cannot be read is kept as <file>.bad, so the next save does not lose it.
        void putAside(const std::string& path)
        {
            const std::string bad = plus::putAside(path, 1);
            if (!bad.empty())
            {
                LL_WARNS("Plus") << "Group titles: the unreadable " << path << " was kept as " << bad << LL_ENDL;
            }
        }

        void load()
        {
            State& s = state();
            s.assignments.clear();
            s.none_on_unassigned = false;
            s.last_applied.clear();
            s.loaded = true;

            const std::string path = filePath();
            if (path.empty())
            {
                LL_WARNS("Plus") << "Group titles: no account folder, Auto Region not read" << LL_ENDL;
                return;
            }
            if (!LLFile::isfile(path))
            {
                return;     // none given yet
            }
            if (!plus::xmlNestingOk(path))
            {
                LL_WARNS("Plus") << "Group titles: " << path << " is nested too deep, left out" << LL_ENDL;
                putAside(path);
                return;
            }

            LLSD data;
            {
                llifstream in(path.c_str());
                if (!in.is_open())
                {
                    LL_WARNS("Plus") << "Group titles: cannot read " << path << LL_ENDL;
                    return;
                }
                if (LLSDSerialize::fromXML(data, in) == LLSDParser::PARSE_FAILURE || !data.isMap())
                {
                    in.close();
                    LL_WARNS("Plus") << "Group titles: " << path << " does not parse" << LL_ENDL;
                    putAside(path);
                    return;
                }
            }

            s.none_on_unassigned = data["none_on_unassigned"].asBoolean();
            const LLSD& list = data["assignments"];
            if (list.isMap())
            {
                for (LLSD::map_const_iterator it = list.beginMap(); it != list.endMap(); ++it)
                {
                    if (s.assignments.size() >= MAX_ASSIGNMENTS)
                    {
                        LL_WARNS("Plus") << "Group titles: more than " << MAX_ASSIGNMENTS << " regions in " << path
                                         << ", the rest left out" << LL_ENDL;
                        break;
                    }
                    const std::string key = normalizeRegionName(it->first);
                    const LLSD& entry = it->second;
                    if (key.empty() || !entry.isMap())
                    {
                        continue;
                    }
                    Assignment assignment;
                    assignment.group_id = entry["group_id"].asUUID();
                    assignment.role_id  = assignment.group_id.notNull() ? entry["role_id"].asUUID() : LLUUID::null;
                    assignment.display_name = sanitizeRegionName(entry["display_name"].asString());
                    if (normalizeRegionName(assignment.display_name) != key)
                    {
                        assignment.display_name = sanitizeRegionName(it->first);
                    }
                    s.assignments[key] = assignment;
                }
            }
            LL_INFOS("Plus") << "Group titles: " << s.assignments.size() << " regions with a title" << LL_ENDL;
        }

        // Written next to the file first and moved over it once whole.
        void save()
        {
            const State& s = state();
            const std::string path = filePath();
            if (path.empty())
            {
                LL_WARNS("Plus") << "Group titles: no account folder, Auto Region not saved" << LL_ENDL;
                return;
            }

            LLSD list = LLSD::emptyMap();
            for (const auto& [key, assignment] : s.assignments)
            {
                LLSD entry;
                entry["group_id"]     = assignment.group_id;
                entry["role_id"]      = assignment.role_id;
                entry["display_name"] = assignment.display_name;
                list[key] = entry;
            }
            LLSD data;
            data["none_on_unassigned"] = s.none_on_unassigned;
            data["assignments"]        = list;

            const plus::FileWrite wrote = plus::writeFileReplacing(path, [&](std::ostream& out) { LLSDSerialize::toPrettyXML(data, out); });
            if (wrote == plus::FileWrite::cannot_replace)
            {
                LL_WARNS("Plus") << "Group titles: cannot replace " << path << LL_ENDL;
            }
            else if (wrote != plus::FileWrite::written)
            {
                LL_WARNS("Plus") << "Group titles: cannot write " << plus::tempPathFor(path) << LL_ENDL;
            }
        }

        void changedAndSave()
        {
            save();
            state().changed();
        }

        // ---- applying -------------------------------------------------------------------------

        // Whether the group manager's cached titles of group_id have role_id's selected.
        bool titleSelected(const LLUUID& group_id, const LLUUID& role_id)
        {
            const LLGroupMgrGroupData* data = LLGroupMgr::getInstance()->getGroupData(group_id);
            if (!data)
            {
                return false;
            }
            for (const LLGroupTitle& title : data->mTitles)
            {
                if (title.mRoleID == role_id)
                {
                    return title.mSelected;
                }
            }
            return false;
        }

        // activateTitle(); send_if_selected false (Auto Region, as Firestorm): no GroupTitleUpdate when the cached
        // titles have it selected already (the window's button always sends: the cache may be old).
        bool applyTitle(const LLUUID& group_id, const LLUUID& role_id, bool send_if_selected)
        {
            if (!plus::rlv::canChangeActiveGroup())
            {
                return false;
            }
            if (group_id.notNull() && (send_if_selected || !titleSelected(group_id, role_id)))
            {
                LLGroupMgr::getInstance()->sendGroupTitleUpdate(group_id, role_id);
            }
            if (gAgent.getGroupID() != group_id)
            {
                LLGroupActions::activate(group_id);
            }
            return true;
        }

        void applyRegion();

        // Looks again later, while the region change it was made for is the latest.
        void retryLater()
        {
            State& s = state();
            if (s.retries >= MAX_RETRIES)
            {
                return;
            }
            ++s.retries;
            const U32 generation = s.region_generation;
            doAfterInterval([generation]()
                            {
                                if (state().region_generation == generation)
                                {
                                    applyRegion();
                                }
                            },
                            RETRY_SECONDS);
        }

        void applyRegion()
        {
            State& s = state();
            if (!s.loaded || (s.assignments.empty() && !s.none_on_unassigned))
            {
                s.last_applied.clear();     // nothing given: a title given later applies on the next arrival
                return;
            }
            const LLViewerRegion* region = gAgent.getRegion();
            if (!region)
            {
                return;
            }
            const std::string key = normalizeRegionName(region->getName());
            if (key.empty())
            {
                retryLater();   // the region's handshake is not in yet
                return;
            }
            if (key == s.last_applied)
            {
                return;
            }
            if (!plus::rlv::canChangeActiveGroup())
            {
                LL_DEBUGS("Plus") << "Group titles: RLVa holds the active group, Auto Region skipped" << LL_ENDL;
                return;
            }

            const auto found = s.assignments.find(key);
            if (found != s.assignments.end())
            {
                const Assignment assignment = found->second;
                s.last_applied = key;
                if (assignment.group_id.notNull() && !gAgent.isInGroup(assignment.group_id))
                {
                    // Not looked at again: a group list that comes late at login is met by the login retry
                    // (onStartupState), and a group left stays left (its row in the window can clear it).
                    LL_WARNS("Plus") << "Group titles: the title of region " << assignment.display_name
                                     << " is of a group you are not in, skipped" << LL_ENDL;
                    return;
                }
                LL_INFOS("Plus") << "Group titles: region " << assignment.display_name << ", its title made active" << LL_ENDL;
                applyTitle(assignment.group_id, assignment.role_id, false);
            }
            else
            {
                s.last_applied = key;
                if (s.none_on_unassigned && gAgent.getGroupID().notNull())
                {
                    LL_INFOS("Plus") << "Group titles: region without a title, no group made active" << LL_ENDL;
                    applyTitle(LLUUID::null, LLUUID::null, false);
                }
            }
        }

        void onRegionChanged()
        {
            State& s = state();
            ++s.region_generation;
            s.retries = 0;
            applyRegion();
        }

        // Once the account's folder is known (setLindenUserDir() is in the login response's state): read
        // the file and apply. A login that fails before it and is tried again (perhaps as another account)
        // reads it again.
        bool onStartupState(const LLSD& info)
        {
            const S32 to = info["enum"].asInteger();
            State& s = state();
            if (to <= STATE_LOGIN_CLEANUP)
            {
                s.loaded = false;
                return false;
            }
            if (s.loaded || to < STATE_INVENTORY_SEND2)
            {
                return false;
            }
            if (!s.following)
            {
                s.following = true;
                gAgent.addRegionChangedCallback([]() { onRegionChanged(); });
            }
            load();
            onRegionChanged();      // the first region came before the file was read
            const U32 generation = s.region_generation;
            doAfterInterval([generation]()
                            {
                                State& later = state();
                                if (later.region_generation == generation)
                                {
                                    later.last_applied.clear();     // the active group may only now be known
                                    applyRegion();
                                }
                            },
                            LOGIN_RETRY_SECONDS);
            return false;
        }

        // ---- the name lookup of askRegionName() ---------------------------------------------------

        void sayNotFound(const std::string& name)
        {
            LLSD args;
            args["REGION"] = name;
            LLNotificationsUtil::add(NOTIFY_NOT_FOUND, args);
        }

        // The lookup's answer or its timeout, whichever comes first, takes it (true); a later one, or one of an
        // older lookup, does nothing.
        bool takeLookup(U32 generation)
        {
            State& s = state();
            if (!s.validating || generation != s.validation_generation)
            {
                return false;
            }
            s.validating = false;
            return true;
        }

        void onLookup(U32 generation, U64 region_handle)
        {
            if (!takeLookup(generation))
            {
                return;
            }
            const State& s = state();
            const LLSimInfo* info = LLWorldMap::getInstance()->simInfoFromHandle(region_handle);
            if (!info || normalizeRegionName(info->getName()) != normalizeRegionName(s.pending_name))
            {
                sayNotFound(s.pending_name);
                return;
            }
            setAssignment(s.pending_group, s.pending_role, info->getName());
        }

        void onLookupTimeout(U32 generation)
        {
            if (takeLookup(generation))
            {
                sayNotFound(state().pending_name);
            }
        }

        void lookUpAndAssign(const LLUUID& group_id, const LLUUID& role_id, const std::string& name)
        {
            State& s = state();
            const U32 generation = ++s.validation_generation;
            s.validating    = true;
            s.pending_group = group_id;
            s.pending_role  = role_id;
            s.pending_name  = name;
            LLWorldMapMessage::getInstance()->sendNamedRegionRequest(
                name,
                [generation](U64 region_handle, const std::string&, const LLUUID&, bool) { onLookup(generation, region_handle); },
                std::string(),
                false);
            doAfterInterval([generation]() { onLookupTimeout(generation); }, VALIDATION_SECONDS);
        }

        bool onRegionNameEntered(const LLSD& notification, const LLSD& response)
        {
            if (LLNotificationsUtil::getSelectedOption(notification, response) != 0)
            {
                return false;
            }
            const std::string name = sanitizeRegionName(response["region_name"].asString());
            if (!name.empty())
            {
                const LLSD& payload = notification["payload"];
                lookUpAndAssign(payload["group_id"].asUUID(), payload["role_id"].asUUID(), name);
            }
            return false;
        }
    }

    bool activateTitle(const LLUUID& group_id, const LLUUID& role_id)
    {
        return applyTitle(group_id, role_id, true);
    }

    std::vector<std::pair<LLUUID, LLUUID>> assignedTitles()
    {
        std::vector<std::pair<LLUUID, LLUUID>> titles;
        for (const auto& [key, assignment] : state().assignments)
        {
            const std::pair<LLUUID, LLUUID> title(assignment.group_id, assignment.role_id);
            if (std::find(titles.begin(), titles.end(), title) == titles.end())
            {
                titles.push_back(title);
            }
        }
        return titles;
    }

    void setAssignment(const LLUUID& group_id, const LLUUID& role_id, const std::string& region_name)
    {
        State& s = state();
        const std::string display = sanitizeRegionName(region_name);
        const std::string key     = normalizeRegionName(display);
        if (!s.loaded || key.empty())
        {
            return;
        }
        const LLUUID role = group_id.notNull() ? role_id : LLUUID::null;
        const auto found = s.assignments.find(key);
        if (found != s.assignments.end())
        {
            if (found->second.group_id == group_id && found->second.role_id == role && found->second.display_name == display)
            {
                return;
            }
        }
        else if (s.assignments.size() >= MAX_ASSIGNMENTS)
        {
            LL_WARNS("Plus") << "Group titles: " << MAX_ASSIGNMENTS << " regions have a title already" << LL_ENDL;
            return;
        }
        s.assignments[key] = { group_id, role, display };
        changedAndSave();
    }

    void setAssignmentForCurrentRegion(const LLUUID& group_id, const LLUUID& role_id)
    {
        if (const LLViewerRegion* region = gAgent.getRegion())
        {
            setAssignment(group_id, role_id, region->getName());
        }
    }

    void askRegionName(const LLUUID& group_id, const LLUUID& role_id)
    {
        LLSD payload;
        payload["group_id"] = group_id;
        payload["role_id"]  = role_id;
        LLNotificationsUtil::add(NOTIFY_ASK, LLSD(), payload, &onRegionNameEntered);
    }

    void clearAssignment(const LLUUID& group_id, const LLUUID& role_id)
    {
        State& s = state();
        if (!s.loaded)
        {
            return;
        }
        bool removed = false;
        for (auto it = s.assignments.begin(); it != s.assignments.end();)
        {
            if (it->second.group_id == group_id && it->second.role_id == role_id)
            {
                it = s.assignments.erase(it);
                removed = true;
            }
            else
            {
                ++it;
            }
        }
        if (removed)
        {
            changedAndSave();
        }
    }

    void clearAssignmentByRegion(const std::string& region_name)
    {
        State& s = state();
        if (s.loaded && s.assignments.erase(normalizeRegionName(region_name)) > 0)
        {
            changedAndSave();
        }
    }

    std::vector<std::string> regionsForTitle(const LLUUID& group_id, const LLUUID& role_id)
    {
        std::vector<std::string> names;
        for (const auto& [key, assignment] : state().assignments)
        {
            if (assignment.group_id == group_id && assignment.role_id == role_id)
            {
                names.push_back(assignment.display_name);
            }
        }
        return names;
    }

    std::string regionListForTitle(const LLUUID& group_id, const LLUUID& role_id)
    {
        std::string list;
        for (const std::string& name : regionsForTitle(group_id, role_id))
        {
            if (!list.empty())
            {
                list += ", ";
            }
            list += name;
        }
        return list;
    }

    bool noneOnUnassigned()
    {
        return state().none_on_unassigned;
    }

    void setNoneOnUnassigned(bool none)
    {
        State& s = state();
        if (s.loaded && s.none_on_unassigned != none)
        {
            s.none_on_unassigned = none;
            save();
        }
    }

    boost::signals2::connection onAssignmentsChanged(const std::function<void()>& callback)
    {
        return state().changed.connect(callback);
    }

    void startAutoRegion()
    {
        LLEventPumps::instance().obtain("StartupState").listen("PlusGroupTitles", &onStartupState);
    }
}

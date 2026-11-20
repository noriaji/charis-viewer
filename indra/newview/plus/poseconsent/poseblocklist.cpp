/**
 * @file poseblocklist.cpp
 * @brief The block list of pose requests: see poseblocklist.h.
 *
 * The file: { "version": 1, "agent": <the account's id>, "blocked": [ <agent id>, ... ] } (LLSD XML). Logs name
 * the file only (never its folder, which is the account's name) and counts, never who is on the list.
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

#include "llviewerprecompiledheaders.h"

#include "poseblocklist.h"

#include "plus.h"

#include "llagent.h"
#include "lldir.h"
#include "llfile.h"
#include "llsd.h"
#include "llsdserialize.h"
#include "llsdutil.h"
#include "lluuid.h"

#include <algorithm>
#include <string>
#include <vector>

namespace plus::poseconsent::blocklist
{
    namespace
    {
        constexpr S32 FILE_VERSION = 1;
        // The file of MAX_ENTRIES ids is about 20 KB; anything far larger is not ours and is not read.
        constexpr S64 MAX_FILE_BYTES = 256 * 1024;
        constexpr int MAX_XML_DEPTH = 8;
        constexpr int ASIDE_NAMES = 3;     // plus::putAside(): .bad, .bad2, .bad3

        std::vector<LLUUID> sIds;          // oldest first
        LLUUID sLoadedFor;                 // the account sIds belong to; null: not read yet

        std::string filePath()
        {
            const std::string& dir = gDirUtilp->getLindenUserDir();
            return dir.empty() ? std::string() : gDirUtilp->add(dir, FILE_NAME);
        }

        // Moves a file that cannot be used out of the way, so that the next save does not write over it.
        void putAside(const std::string& path, const char* why)
        {
            const std::string aside = plus::putAside(path, ASIDE_NAMES);
            LL_WARNS("PoseConsent") << FILE_NAME << ": " << why << "; "
                                    << (aside.empty() ? "it could not be moved aside" : "moved aside")
                                    << ", the block list starts empty" << LL_ENDL;
        }

        void read(const std::string& path, const LLUUID& agent)
        {
            if (!LLFile::isfile(path))
            {
                return;
            }
            const S64 bytes = LLFile::size(path);
            if (bytes < 0 || bytes > MAX_FILE_BYTES)
            {
                putAside(path, "too large or unreadable");
                return;
            }
            if (!plus::xmlNestingOk(path, MAX_XML_DEPTH))
            {
                putAside(path, "nested too deep");
                return;
            }
            llifstream in(path.c_str());
            LLSD sd;
            if (!in.is_open() || LLSDSerialize::fromXML(sd, in) == LLSDParser::PARSE_FAILURE || !sd.isMap())
            {
                putAside(path, "cannot be read");
                return;
            }
            if (sd["version"].asInteger() != FILE_VERSION || sd["agent"].asUUID() != agent || !sd["blocked"].isArray())
            {
                putAside(path, "of another version or account");
                return;
            }
            for (const LLSD& entry : llsd::inArray(sd["blocked"]))
            {
                const LLUUID id = entry.asUUID();
                if (id.notNull() && id != agent && std::find(sIds.begin(), sIds.end(), id) == sIds.end())
                {
                    sIds.push_back(id);
                    if (sIds.size() >= MAX_ENTRIES)
                    {
                        break;
                    }
                }
            }
            LL_INFOS("PoseConsent") << FILE_NAME << ": " << sIds.size() << " blocked" << LL_ENDL;
        }

        // The list of the account logged in, read once per login. False before a login (no account folder).
        bool ensureLoaded()
        {
            const LLUUID agent = gAgent.getID();
            if (agent.isNull())
            {
                return false;
            }
            if (sLoadedFor == agent)
            {
                return true;
            }
            const std::string path = filePath();
            if (path.empty())
            {
                return false;
            }
            sIds.clear();
            sLoadedFor = agent;
            read(path, agent);
            return true;
        }

        void save()
        {
            const std::string path = filePath();
            if (path.empty() || sLoadedFor.isNull())
            {
                return;
            }
            LLSD sd;
            sd["version"] = FILE_VERSION;
            sd["agent"] = sLoadedFor;
            LLSD& blocked = sd["blocked"] = LLSD::emptyArray();
            for (const LLUUID& id : sIds)
            {
                blocked.append(id);
            }
            const plus::FileWrite wrote = plus::writeFileReplacing(path, [&](std::ostream& out) { LLSDSerialize::toPrettyXML(sd, out); });
            if (wrote != plus::FileWrite::written)
            {
                LL_WARNS("PoseConsent") << FILE_NAME << ": not saved ("
                                        << (wrote == plus::FileWrite::cannot_replace ? "cannot replace" : "cannot write")
                                        << "); the list stays as it is for this session" << LL_ENDL;
            }
        }
    }

    bool contains(const LLUUID& agent_id)
    {
        return agent_id.notNull() && ensureLoaded() && std::find(sIds.begin(), sIds.end(), agent_id) != sIds.end();
    }

    Added add(const LLUUID& agent_id)
    {
        if (agent_id.isNull() || agent_id == gAgent.getID() || !ensureLoaded())
        {
            return Added::Unavailable;
        }
        if (std::find(sIds.begin(), sIds.end(), agent_id) != sIds.end())
        {
            return Added::AlreadyThere;
        }
        if (sIds.size() >= MAX_ENTRIES)
        {
            LL_INFOS("PoseConsent") << FILE_NAME << ": full (" << MAX_ENTRIES << "), not added" << LL_ENDL;
            return Added::Full;
        }
        sIds.push_back(agent_id);
        save();
        return Added::Added;
    }

    std::size_t count()
    {
        return ensureLoaded() ? sIds.size() : 0;
    }

    void clear()
    {
        if (!ensureLoaded() || sIds.empty())
        {
            return;
        }
        sIds.clear();
        save();
    }

    void forget()
    {
        sIds.clear();
        sLoadedFor.setNull();
    }
}

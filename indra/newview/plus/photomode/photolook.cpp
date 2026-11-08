/**
 * @file photolook.cpp
 * @brief Photo tools: the looks as the tool lists them, the favourite and the recent ones.
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

// The looks themselves (the folders, look.xml, the LUT, the menu) are plus/look's; this file only
// lists them for the photo tools and keeps two short lists of ids, in the shared settings (the
// looks are the same for every account): the favourites (PlusLookFavorites) and the ones put on
// last (PlusLookRecent). The look in use is PlusLook, nothing else.

#include "llviewerprecompiledheaders.h"

#include "photolook.h"

#include "plus/look/look.h"

#include <algorithm>

#include "llsdutil.h"
#include "llviewercontrol.h"

namespace plus::photomode
{
    namespace
    {
        constexpr char SETTING_LOOK[]      = "PlusLook";
        constexpr char SETTING_FAVORITES[] = "PlusLookFavorites";
        constexpr char SETTING_RECENT[]    = "PlusLookRecent";

        std::vector<std::string> readIds(const char* setting)
        {
            std::vector<std::string> ids;
            if (gSavedSettings.controlExists(setting))
            {
                for (const LLSD& id : llsd::inArray(gSavedSettings.getLLSD(setting)))
                {
                    if (!id.asString().empty())
                    {
                        ids.push_back(id.asString());
                    }
                }
            }
            return ids;
        }

        void writeIds(const char* setting, const std::vector<std::string>& ids)
        {
            if (!gSavedSettings.controlExists(setting))
            {
                return;
            }
            LLSD array = LLSD::emptyArray();
            for (const std::string& id : ids)
            {
                array.append(id);
            }
            gSavedSettings.setLLSD(setting, array);
        }

        std::vector<LookChoice> describeIds(const std::vector<std::string>& ids)
        {
            std::vector<LookChoice> list;
            for (const std::string& id : ids)
            {
                LookChoice choice;
                choice.id     = id;
                choice.label  = plus::look::labelOf(id);
                choice.exists = !choice.label.empty();
                choice.user   = id.rfind("user:", 0) == 0;
                list.push_back(choice);
            }
            return list;
        }
    }

    std::vector<LookChoice> allLooks()
    {
        std::vector<LookChoice> list;
        list.push_back(LookChoice()); // no look
        for (const plus::look::Entry& entry : plus::look::entries())
        {
            list.push_back({ entry.id, entry.label, entry.user, true });
        }
        return list;
    }

    std::vector<LookChoice> favoriteLooks()
    {
        return describeIds(favoriteLookIds());
    }

    std::vector<LookChoice> recentLooks()
    {
        return describeIds(recentLookIds());
    }

    std::vector<std::string> favoriteLookIds()
    {
        return readIds(SETTING_FAVORITES);
    }

    std::vector<std::string> recentLookIds()
    {
        return readIds(SETTING_RECENT);
    }

    bool isFavoriteLook(const std::string& id)
    {
        const std::vector<std::string> ids = readIds(SETTING_FAVORITES);
        return !id.empty() && std::find(ids.begin(), ids.end(), id) != ids.end();
    }

    bool addFavoriteLook(const std::string& id)
    {
        std::vector<std::string> ids = readIds(SETTING_FAVORITES);
        if (id.empty() || ids.size() >= MAX_LOOK_FAVORITES)
        {
            return false;
        }
        if (std::find(ids.begin(), ids.end(), id) == ids.end())
        {
            ids.push_back(id);
            writeIds(SETTING_FAVORITES, ids);
        }
        return true;
    }

    void removeFavoriteLook(const std::string& id)
    {
        std::vector<std::string> ids = readIds(SETTING_FAVORITES);
        const auto end = std::remove(ids.begin(), ids.end(), id);
        if (end != ids.end())
        {
            ids.erase(end, ids.end());
            writeIds(SETTING_FAVORITES, ids);
        }
    }

    std::string currentLook()
    {
        return gSavedSettings.controlExists(SETTING_LOOK) ? gSavedSettings.getString(SETTING_LOOK) : std::string();
    }

    void applyLook(const std::string& id)
    {
        if (!gSavedSettings.controlExists(SETTING_LOOK))
        {
            return;
        }
        gSavedSettings.setString(SETTING_LOOK, id); // plus/look makes the LUT's texture at the next idle
        if (id.empty())
        {
            return;
        }
        std::vector<std::string> ids = readIds(SETTING_RECENT);
        if (!ids.empty() && ids.front() == id)
        {
            return;
        }
        ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
        ids.insert(ids.begin(), id);
        if (ids.size() > MAX_LOOK_RECENT)
        {
            ids.resize(MAX_LOOK_RECENT);
        }
        writeIds(SETTING_RECENT, ids);
    }

    std::string stepLook(const std::vector<LookChoice>& list, int direction)
    {
        const std::string current = currentLook();
        std::vector<const LookChoice*> usable;
        for (const LookChoice& choice : list)
        {
            if (choice.exists)
            {
                usable.push_back(&choice);
            }
        }
        if (usable.empty())
        {
            return current;
        }
        int at = -1;
        for (size_t i = 0; i < usable.size(); ++i)
        {
            if (usable[i]->id == current)
            {
                at = (int)i;
                break;
            }
        }
        const int count = (int)usable.size();
        const int next  = at < 0 ? (direction > 0 ? 0 : count - 1) : (at + (direction > 0 ? 1 : -1) + count) % count;
        return usable[next]->id;
    }
}

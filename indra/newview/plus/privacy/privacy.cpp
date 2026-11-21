/**
 * @file privacy.cpp
 * @brief Keeps the changed settings and the names of the Charis windows out of the ViewerStats report.
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

#include "privacy.h"

#include "llsd.h"
#include "llstring.h"

#include <string>
#include <vector>

namespace plus::privacy
{
    namespace
    {
        // Names of Charis windows, panels and commands in the UI counts
        // (body["ui"], LLUIUsage), compared in lower case. LLUIUsage has
        // turned the dots of "Plus.Presets.Apply" into "_". "fs_", "poser",
        // "pose_": the poser from Firestorm (fs_poser, Poser.*, Pose.Menu).
        const char* const OWN_UI_PREFIXES[] = { "plus", "panel_plus", "floater_plus", "menu_plus", "fs_", "poser", "pose_" };

        bool isOwnUI(const std::string& name)
        {
            std::string lower(name);
            LLStringUtil::toLower(lower);
            for (const char* prefix : OWN_UI_PREFIXES)
            {
                if (LLStringUtil::startsWith(lower, prefix))
                {
                    return true;
                }
            }
            return false;
        }

        // Removes the Charis names from a map of UI counts, and from the maps
        // in it down to depth levels (ui.controls is window, then control).
        size_t filterUINames(LLSD& map, int depth)
        {
            if (!map.isMap())
            {
                return 0;
            }

            size_t removed = 0;
            std::vector<std::string> names;
            for (LLSD::map_iterator it = map.beginMap(); it != map.endMap(); ++it)
            {
                if (isOwnUI(it->first))
                {
                    names.push_back(it->first);
                }
                else if (depth > 1)
                {
                    removed += filterUINames(it->second, depth - 1);
                }
            }
            for (const std::string& name : names)
            {
                map.erase(name);
            }
            return removed + names.size();
        }

        // body["ui"] (LLUIUsage::asLLSD()): commands, controls, floaters and
        // panels with how often they were used (research/130 L3).
        size_t filterUI(LLSD& ui)
        {
            if (!ui.isMap())
            {
                return 0;
            }
            size_t removed = 0;
            for (const char* part : { "commands", "floaters", "panels" })
            {
                if (ui.has(part))
                {
                    removed += filterUINames(ui[part], 1);
                }
            }
            if (ui.has("controls"))
            {
                removed += filterUINames(ui["controls"], 2);
            }
            return removed;
        }
    }

    void filterStats(LLSD& body)
    {
        if (!body.isMap())
        {
            return;
        }

        // The settings that differ from the defaults are not sent at all (the official viewer sends them): what the user
        // changed is not for Linden Lab, and a Charis setting or a key must not leave by this way.
        body.erase("preferences");

        if (body.has("ui"))
        {
            const size_t removed = filterUI(body["ui"]);
            LL_INFOS("Plus") << "ViewerStats: " << removed << " Charis UI names left out" << LL_ENDL;
        }
    }
}

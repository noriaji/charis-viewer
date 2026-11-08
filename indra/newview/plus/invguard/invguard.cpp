/**
 * @file invguard.cpp
 * @brief Inventory guards (see invguard.h).
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

#include "invguard.h"

#include <algorithm>
#include <vector>

namespace plus::invguard
{
    namespace
    {
        // Few guards (protected folders, RLV): a vector, asked in the order they were added.
        // Function-local so a guard may register during static initialization.
        std::vector<Guard*>& guards()
        {
            static std::vector<Guard*> sGuards;
            return sGuards;
        }

        // True if some guard says yes to ask(guard); stops at the first.
        template <typename Ask>
        bool anyGuard(const Ask& ask)
        {
            // By index, so a guard that registers while being asked does not invalidate the loop.
            const std::vector<Guard*>& all = guards();
            for (size_t i = 0; i < all.size(); ++i)
            {
                if (ask(*all[i]))
                {
                    return true;
                }
            }
            return false;
        }

        // The two-step "refuses" question (see invguard.h): the first guard that does not ask the user and blocks
        // (pure question) tells why and answers; otherwise every guard is asked its "refuses" question in turn.
        template <typename Blocks, typename Refuses>
        bool refusesTwoStep(const Blocks& blocks, const Refuses& refuses)
        {
            const std::vector<Guard*>& all = guards();
            for (size_t i = 0; i < all.size(); ++i)
            {
                if (!all[i]->asksUser() && blocks(*all[i]))
                {
                    refuses(*all[i]);
                    return true;
                }
            }
            return anyGuard(refuses);
        }
    }

    void add(Guard* guard)
    {
        std::vector<Guard*>& all = guards();
        if (guard && std::find(all.begin(), all.end(), guard) == all.end())
        {
            all.push_back(guard);
        }
    }

    void remove(Guard* guard)
    {
        std::vector<Guard*>& all = guards();
        all.erase(std::remove(all.begin(), all.end(), guard), all.end());
    }

    bool blocksDrag(const LLUUID& folder_id)
    {
        return anyGuard([&](Guard& g) { return g.blocksDrag(folder_id); });
    }

    bool blocksMove(const LLUUID& folder_id, const LLUUID& new_parent_id, std::string& tooltip)
    {
        return anyGuard([&](Guard& g) { return g.blocksMove(folder_id, new_parent_id, tooltip); });
    }

    bool refusesMove(const LLUUID& folder_id, const LLUUID& new_parent_id)
    {
        std::string tooltip;
        return refusesTwoStep([&](Guard& g) { return g.blocksMove(folder_id, new_parent_id, tooltip); },
                              [&](Guard& g) { return g.refusesMove(folder_id, new_parent_id); });
    }

    bool blocksRemoval(const LLUUID& folder_id)
    {
        return anyGuard([&](Guard& g) { return g.blocksRemoval(folder_id); });
    }

    bool refusesRemoval(const LLUUID& folder_id)
    {
        return refusesTwoStep([&](Guard& g) { return g.blocksRemoval(folder_id); },
                              [&](Guard& g) { return g.refusesRemoval(folder_id); });
    }

    bool blocksFolderRename(const LLUUID& folder_id)
    {
        return anyGuard([&](Guard& g) { return g.blocksFolderRename(folder_id); });
    }

    bool blocksItemRemoval(const LLUUID& item_id)
    {
        return anyGuard([&](Guard& g) { return g.blocksItemRemoval(item_id); });
    }

    bool blocksItemMove(const LLUUID& item_id, const LLUUID& new_parent_id)
    {
        return anyGuard([&](Guard& g) { return g.blocksItemMove(item_id, new_parent_id); });
    }

    bool blocksItemRename(const LLUUID& item_id)
    {
        return anyGuard([&](Guard& g) { return g.blocksItemRename(item_id); });
    }

    bool refusesItemMove(const LLUUID& item_id, const LLUUID& new_parent_id)
    {
        return refusesTwoStep([&](Guard& g) { return g.blocksItemMove(item_id, new_parent_id); },
                              [&](Guard& g) { return g.refusesItemMove(item_id, new_parent_id); });
    }

    bool refusesPaste(const LLUUID& source_id, bool source_is_folder, const LLUUID& dest_folder_id)
    {
        return anyGuard([&](Guard& g) { return g.refusesPaste(source_id, source_is_folder, dest_folder_id); });
    }
}

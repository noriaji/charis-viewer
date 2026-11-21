/**
 * @file grouptitlescore.h
 * @brief Group titles: the titles of the agent's groups as rows, asked of the server and kept up to date (research/386).
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

// What the XUI window (grouptitlesfloater.cpp) and the RmlUi window (grouptitlesrml.cpp) share: one request per group
// (the titles come back as the group manager's observers say), the rows built from the group manager's titles and the
// agent's groups, and the "something changed" signal. The window is told through the callback that its rows are to be
// built again (it does that on its own turn, once however many replies came in the frame). Main thread only.

#ifndef PLUS_GROUPTITLESCORE_H
#define PLUS_GROUPTITLESCORE_H

#include "lluuid.h"

#include "llevent.h"
#include "llhandle.h"

#include <boost/signals2/connection.hpp>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

class LLContextMenu;
class LLView;

namespace plus::grouptitles
{
    // One title (a role of a group), or the "no group" row (null ids).
    struct TitleRow
    {
        LLUUID group_id;
        LLUUID role_id;
        std::string key;            // group id + role id: only the pair is unique
        std::string title;
        std::string group_name;
        std::string regions;        // Auto Region: the regions given to the title
        bool active = false;        // the title in use (shown bold)
        bool is_group = false;      // false: the "no group" row
    };

    // The words of the rows the window has in its <string>s.
    struct TitleLabels
    {
        std::string no_title;       // "<No group title>"
        std::string none_group;     // the group column of that row
        std::string unknown_title;  // a title of a group the agent is not in
        std::string not_member;
    };

    class TitlesObserver;

    class GroupTitlesData : public LLOldEvents::LLSimpleListener
    {
    public:
        // changed: the rows are to be built again (a group's titles came in, the groups or the assignments changed).
        explicit GroupTitlesData(std::function<void()> changed);
        ~GroupTitlesData() override;
        GroupTitlesData(const GroupTitlesData&) = delete;
        GroupTitlesData& operator=(const GroupTitlesData&) = delete;

        // Every group's titles asked again (the window opened, the refresh button).
        void requestAll();
        // The rows that hold filter_upper (upper case; empty: all) in the title or the group name. A group joined is asked
        // for, a group left is forgotten. active_key: the row of the active title ("" when none shows).
        std::vector<TitleRow> rows(const TitleLabels& labels, const std::string& filter_upper, std::string& active_key);

        // The buttons' state for a row: a row of a group the agent is not in can only have its regions cleared.
        static bool usable(const LLUUID& group_id);

        bool handleEvent(LLPointer<LLOldEvents::LLEvent> event, const LLSD&) override;

    private:
        void requestGroup(const LLUUID& group_id);
        void followGroups();

        std::function<void()> mChanged;
        std::map<LLUUID, std::unique_ptr<TitlesObserver>> mObservers;   // the groups asked for
        boost::signals2::connection mAssignmentsChanged;
    };

    // "Clear Region" of a title: one region is taken away at once, more give a menu of them and "all" (all_label) at the
    // screen point (where the XUI menu opened: localPointToScreen(0, height) of the button, its top edge), the menu kept in `menu` (a menu left from before is deleted first).
    void clearRegionsOf(const LLUUID& group_id, const LLUUID& role_id, const std::string& all_label, S32 screen_x, S32 screen_y,
                        LLView* spawning, LLHandle<LLContextMenu>& menu);
    // The menu is gone with its window.
    void deleteClearMenu(LLHandle<LLContextMenu>& menu);
}

#endif // PLUS_GROUPTITLESCORE_H

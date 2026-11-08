/**
 * @file invview.cpp
 * @brief Shows ids in the inventory window's "AI Search" tab.
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

#include "invview.h"

#include "plus.h"

#include "llcallbacklist.h"
#include "llfloater.h"
#include "llfloaterreg.h"
#include "llfloatersidepanelcontainer.h"
#include "llinventoryfilter.h"
#include "llinventorypanel.h"
#include "lltabcontainer.h"
#include "lltimer.h"

#include <algorithm>
#include <memory>
#include <unordered_set>
#include <vector>

namespace plus::invfinder
{
    namespace
    {
        constexpr char TAB_NAME[]   = "AI Search";
        constexpr char TABS_NAME[]  = "inventory filter tabs";
        constexpr char FLOATER[]    = "inventory";

        // The tab starts out showing nothing, not the whole inventory: until an AI search
        // has been shown, its filter gets an empty id set (see watchForTheTabs()). Every
        // inventory window has the tab ("New inventory window" makes more of them).
        std::vector<LLHandle<LLView>>    sTabsSetUp;        // the tabs that have an id set
        std::vector<LLHandle<LLFloater>> sWindowsSeen;      // the inventory windows looked at

        // Drops the handles of what is gone.
        template <typename T> void prune(std::vector<LLHandle<T>>& handles)
        {
            handles.erase(std::remove_if(handles.begin(), handles.end(), [](const LLHandle<T>& h) { return h.isDead(); }),
                          handles.end());
        }

        template <typename T> bool contains(const std::vector<LLHandle<T>>& handles, const LLHandle<T>& handle)
        {
            return std::find(handles.begin(), handles.end(), handle) != handles.end();
        }

        LLFloater* inventoryFloater(bool show)
        {
            if (show)
                LLFloaterSidePanelContainer::showPanel(FLOATER, LLSD());
            LLFloater* floater = LLFloaterSidePanelContainer::getTopmostInventoryFloater();
            return floater ? floater : LLFloaterReg::findInstance(FLOATER);
        }

        struct Tab
        {
            LLTabContainer*  tabs  = nullptr;
            LLInventoryPanel* panel = nullptr;
        };

        Tab findTab(LLFloater* floater)
        {
            Tab tab;
            if (floater)
            {
                tab.tabs  = floater->findChild<LLTabContainer>(TABS_NAME);
                tab.panel = floater->findChild<LLInventoryPanel>(TAB_NAME);
            }
            return tab;
        }

        // The tab is filled while its views are still being built and filtered; open the
        // folders (so the matches show) once the filter is done, and again a little later.
        void openFoldersWhenFiltered(LLInventoryPanel* panel)
        {
            LLHandle<LLView> handle  = panel->getHandle();
            const F64        started = LLTimer::getTotalSeconds();
            auto             opened  = std::make_shared<int>(0);
            doOnIdleRepeating(
                [handle, started, opened]() -> bool
                {
                    LLInventoryPanel* p = dynamic_cast<LLInventoryPanel*>(handle.get());
                    const F64         age = LLTimer::getTotalSeconds() - started;
                    if (!p || age > 8.0)
                        return true;
                    if (!p->getVisible() || p->getFilter().isModified())
                        return false;
                    if (*opened == 0 && age >= 0.5)
                    {
                        p->openAllFolders();
                        *opened = 1;
                    }
                    else if (*opened == 1 && age >= 2.5)
                    {
                        p->openAllFolders(); // views built after the first pass
                        return true;
                    }
                    return false;
                });
        }

        void setIds(LLInventoryPanel* panel, const std::vector<std::string>& ids, std::size_t& count)
        {
            auto set = std::make_shared<std::unordered_set<LLUUID>>();
            for (const std::string& text : ids)
            {
                LLUUID id;
                if (id.set(text, false))
                    set->insert(id);
            }
            count = set->size();
            panel->getFilter().setPlusIdSet(std::move(set));
            const LLHandle<LLView> handle = panel->getHandle();
            if (!contains(sTabsSetUp, handle))
                sTabsSetUp.push_back(handle);
        }

        const char kNoTab[] = "The inventory window has no AI Search tab in this view (single-folder or gallery view?). "
                              "Switch the inventory to the normal tabbed view.";

        // Each inventory window, when it is first seen: its tab is emptied, unless an AI search
        // was shown in it already. Every 30 frames for the viewer's lifetime; a window is
        // searched for its tab once (the inventory's views are many).
        bool watchForTheTabs()
        {
            static U32 frame = 0;
            if (++frame % 30 != 0)
                return false;
            prune(sWindowsSeen);
            prune(sTabsSetUp);
            for (LLFloater* floater : LLFloaterReg::getFloaterList(FLOATER))
            {
                if (!floater || contains(sWindowsSeen, floater->getHandle()))
                    continue;
                const Tab tab = findTab(floater);
                if (!tab.panel)
                    continue;   // not built yet (every inventory window has the tab: panel_main_inventory.xml)
                sWindowsSeen.push_back(floater->getHandle());
                if (!contains(sTabsSetUp, LLHandle<LLView>(tab.panel->getHandle())))
                {
                    std::size_t count = 0;
                    setIds(tab.panel, {}, count);
                }
            }
            return false;
        }

        void init()
        {
            doOnIdleRepeating([]() { return watchForTheTabs(); });
        }

        plus::FeatureRegistration sRegistration({ "invview", &init });
    }

    ShowResult showInInventory(const std::vector<std::string>& ids)
    {
        ShowResult result;
        const Tab  tab = findTab(inventoryFloater(true));
        if (!tab.panel || !tab.tabs || !tab.tabs->getVisible())
        {
            result.error = kNoTab;
            return result;
        }
        setIds(tab.panel, ids, result.shown);
        tab.tabs->setTabVisibility(tab.panel, true);
        tab.tabs->selectTabByName(TAB_NAME);
        openFoldersWhenFiltered(tab.panel);
        return result;
    }

    ShowResult clearInventoryTab()
    {
        ShowResult result;
        const Tab  tab = findTab(inventoryFloater(false));
        if (!tab.panel)
        {
            result.error = kNoTab;
            return result;
        }
        setIds(tab.panel, {}, result.shown);
        return result;
    }
}

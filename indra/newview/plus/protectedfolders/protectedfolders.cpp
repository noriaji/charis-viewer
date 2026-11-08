/**
 * @file protectedfolders.cpp
 * @brief Protected folders: the list, and the questions the inventory asks (see protectedfolders.h).
 *
 * Ported from Firestorm's "Protect folder" checks in llinventorybridge.cpp and
 * llinventorymodel.cpp (see ORIGIN.md).
 *
 * $LicenseInfo:firstyear=2020&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (c) 2020 Ansariel Hiller @ Second Life
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

#include "llviewerprecompiledheaders.h"

#include "protectedfolders.h"

#include "plus.h"
#include "plus/invguard/invguard.h"

#include <algorithm>
#include <unordered_set>

#include <boost/signals2/signal.hpp>

#include "llfoldertype.h"
#include "llinventorymodel.h"
#include "llnotificationsutil.h"
#include "llstartup.h"
#include "lltimer.h"
#include "lltrans.h"
#include "llviewercontrol.h"

namespace plus::protectedfolders
{
    namespace
    {
        const char* SETTING = "PlusProtectedFolders";
        constexpr F64 MESSAGE_INTERVAL = 3.0;       // seconds between two "protected" messages

        // The setting, as a set for the questions (asked for every folder of a tree). Rebuilt
        // when the setting changes. The connection is made at the first question: the control
        // is declared by then (plus.cpp reads settings_plus.xml at start-up), and the account's
        // values arrive later through the same signal.
        struct State
        {
            std::unordered_set<LLUUID> set;
            boost::signals2::connection connection;
            boost::signals2::signal<void()> changed;
            bool connected = false;
            F64 lastMessage = 0.0;
        };

        State& state()
        {
            static State sState;
            return sState;
        }

        void rebuild(const LLSD& value)
        {
            State& s = state();
            s.set.clear();
            for (LLSD::array_const_iterator it = value.beginArray(); it != value.endArray(); ++it)
            {
                const LLUUID id = it->asUUID();
                if (id.notNull())
                {
                    s.set.insert(id);
                }
            }
        }

        void onSettingChanged(LLControlVariable* control, const LLSD&, const LLSD&)
        {
            rebuild(control->getValue());
            state().changed();
        }

        void ensureConnected()
        {
            State& s = state();
            if (s.connected)
            {
                return;
            }
            LLControlVariable* control = gSavedPerAccountSettings.getControl(SETTING);
            if (!control)
            {
                return;
            }
            s.connection = control->getCommitSignal()->connect(&onSettingChanged);
            s.connected = true;
            rebuild(control->getValue());
        }

        void store(const std::vector<LLUUID>& ids)
        {
            if (!gSavedPerAccountSettings.controlExists(SETTING))
            {
                return;
            }
            LLSD array = LLSD::emptyArray();
            for (const LLUUID& id : ids)
            {
                array.append(id);
            }
            gSavedPerAccountSettings.setLLSD(SETTING, array);
            // Saved now, not only at logout: after a crash the folder would be unprotected again.
            if (LLStartUp::getStartupState() == STATE_STARTED)
            {
                plus::saveSettingsFile(gSavedPerAccountSettings, gSavedSettings.getString("PerAccountSettingsFile"), true);
            }
        }

        LLUUID trashId()
        {
            return gInventory.findCategoryUUIDForType(LLFolderType::FT_TRASH);
        }

        bool inTrash(const LLUUID& folder_id)
        {
            const LLUUID trash_id = trashId();
            return trash_id.notNull() && gInventory.isObjectDescendentOf(folder_id, trash_id);
        }

        // The protected folders that are in the Trash (put there by another viewer, or protected
        // there): protection does not hold in the Trash (they can be restored, or deleted).
        std::vector<LLUUID> protectedInTrash()
        {
            std::vector<LLUUID> ids;
            const LLUUID trash_id = trashId();
            if (trash_id.isNull())
            {
                return ids;
            }
            for (const LLUUID& protected_id : state().set)
            {
                if (protected_id != trash_id && gInventory.isObjectDescendentOf(protected_id, trash_id))
                {
                    ids.push_back(protected_id);
                }
            }
            return ids;
        }

        std::string folderName(const LLUUID& folder_id)
        {
            const LLViewerInventoryCategory* cat = gInventory.getCategory(folder_id);
            return cat ? cat->getName() : std::string();
        }

        // Why folder_id is not moved, blocker being moveBlocker()'s answer: the message after
        // a refused move, or the tooltip while dragging.
        std::string refusal(const LLUUID& folder_id, const LLUUID& blocker, bool tooltip)
        {
            // A folder's name may be someone else's (folders can be given): no links of theirs
            // in the message, whose text box trusts its text. Not in the tooltip, which shows
            // the text as it is.
            const LLSD args = tooltip ? LLSD().with("NAME", folderName(folder_id)).with("INSIDE", folderName(blocker))
                                      : LLSD().with("NAME", plus::safeText(folderName(folder_id)))
                                              .with("INSIDE", plus::safeText(folderName(blocker)));
            if (blocker == folder_id)
            {
                return LLTrans::getString(tooltip ? "PlusProtectedFolderNoMoveTip" : "PlusProtectedFolderMoveRefused", args);
            }
            return LLTrans::getString(tooltip ? "PlusProtectedFolderNoTrashTip" : "PlusProtectedFolderTrashRefused", args);
        }

        // Shows the message, once in a while (not for every folder of a batch).
        void tell(const std::string& message)
        {
            State& s = state();
            const F64 now = F64(LLTimer::getElapsedSeconds());
            if (now - s.lastMessage >= MESSAGE_INTERVAL)
            {
                s.lastMessage = now;
                LLNotificationsUtil::add("SystemMessageTip", LLSD().with("MESSAGE", message));
            }
        }

        // Emptying the Trash with protected folders in it: asked first, as they go for good (and
        // with them their protection). Yes: they are unprotected and the Trash is emptied (this
        // question stands for the official one). True: not emptied now.
        bool asksBeforeEmptyingTrash()
        {
            const std::vector<LLUUID> ids = protectedInTrash();
            if (ids.empty())
            {
                return false;
            }
            constexpr size_t MAX_NAMES = 5;
            std::string names;
            for (size_t i = 0; i < ids.size() && i < MAX_NAMES; ++i)
            {
                names += (names.empty() ? "" : ", ") + plus::safeText(folderName(ids[i]));
            }
            if (ids.size() > MAX_NAMES)
            {
                names += ", ...";
            }
            LL_INFOS("PlusProtectedFolders") << "Emptying the Trash: " << ids.size() << " protected folders in it; asking first" << LL_ENDL;
            LLNotificationsUtil::add("GenericAlertYesCancel",
                                     LLSD().with("MESSAGE", LLTrans::getString("PlusProtectedFolderEmptyTrash", LLSD().with("NAMES", names))),
                                     LLSD(),
                                     [](const LLSD& notification, const LLSD& response)
                                     {
                                         if (LLNotificationsUtil::getSelectedOption(notification, response) != 0)
                                         {
                                             return;
                                         }
                                         // Those still in the Trash now (one may have been restored meanwhile).
                                         unprotect(protectedInTrash());
                                         gInventory.emptyFolderType(std::string(), LLFolderType::FT_TRASH);
                                     });
            return true;
        }
    }

    std::vector<LLUUID> folders()
    {
        std::vector<LLUUID> ids;
        if (!gSavedPerAccountSettings.controlExists(SETTING))
        {
            return ids;
        }
        const LLSD array = gSavedPerAccountSettings.getLLSD(SETTING);
        for (LLSD::array_const_iterator it = array.beginArray(); it != array.endArray(); ++it)
        {
            const LLUUID id = it->asUUID();
            if (id.notNull() && std::find(ids.begin(), ids.end(), id) == ids.end())
            {
                ids.push_back(id);
            }
        }
        return ids;
    }

    bool isProtected(const LLUUID& folder_id)
    {
        ensureConnected();
        const State& s = state();
        return !s.set.empty() && s.set.count(folder_id) != 0;
    }

    bool holdsProtected(const LLUUID& folder_id)
    {
        ensureConnected();
        const State& s = state();
        if (s.set.empty())
        {
            return false;
        }
        // Nothing in the Trash is protected (as moveBlocker() and refusesRemoval() have it): a
        // protected folder there is deleted one by one as emptying the Trash deletes it.
        const LLUUID trash_id = trashId();
        for (const LLUUID& protected_id : s.set)
        {
            if ((protected_id == folder_id || gInventory.isObjectDescendentOf(protected_id, folder_id))
                && !(trash_id.notNull() && gInventory.isObjectDescendentOf(protected_id, trash_id)))
            {
                return true;
            }
        }
        return false;
    }

    void protect(const LLUUID& folder_id)
    {
        if (folder_id.isNull() || isProtected(folder_id))
        {
            return;
        }
        std::vector<LLUUID> ids = folders();
        ids.push_back(folder_id);
        store(ids);
    }

    void unprotect(const std::vector<LLUUID>& folder_ids)
    {
        std::vector<LLUUID> ids = folders();
        const std::size_t before = ids.size();
        ids.erase(std::remove_if(ids.begin(), ids.end(),
                                 [&folder_ids](const LLUUID& id)
                                 {
                                     return std::find(folder_ids.begin(), folder_ids.end(), id) != folder_ids.end();
                                 }),
                  ids.end());
        if (ids.size() != before)
        {
            store(ids);
        }
    }

    bool blocksMove(const LLUUID& folder_id)
    {
        return isProtected(folder_id) && !inTrash(folder_id);
    }

    LLUUID moveBlocker(const LLUUID& folder_id, const LLUUID& new_parent_id)
    {
        ensureConnected();
        const State& s = state();
        if (s.set.empty())
        {
            return LLUUID::null;
        }
        const LLViewerInventoryCategory* cat = gInventory.getCategory(folder_id);
        if (cat && cat->getParentUUID() == new_parent_id)
        {
            return LLUUID::null;    // not a move
        }
        if (inTrash(folder_id))
        {
            return LLUUID::null;    // nothing in the Trash is protected (and it can be restored)
        }
        if (s.set.count(folder_id) != 0)
        {
            return folder_id;
        }
        if (!inTrash(new_parent_id))
        {
            return LLUUID::null;
        }
        // Into the Trash: not with a protected folder inside. folder_id is not in the Trash,
        // so neither is anything inside it.
        for (const LLUUID& protected_id : s.set)
        {
            if (gInventory.isObjectDescendentOf(protected_id, folder_id))
            {
                return protected_id;
            }
        }
        return LLUUID::null;
    }

    bool blocksMove(const LLUUID& folder_id, const LLUUID& new_parent_id, std::string& tooltip)
    {
        const LLUUID blocker = moveBlocker(folder_id, new_parent_id);
        if (blocker.isNull())
        {
            return false;
        }
        tooltip = refusal(folder_id, blocker, true);
        return true;
    }

    bool refusesMove(const LLUUID& folder_id, const LLUUID& new_parent_id)
    {
        const LLUUID blocker = moveBlocker(folder_id, new_parent_id);
        if (blocker.isNull())
        {
            return false;
        }
        tell(refusal(folder_id, blocker, false));
        return true;
    }

    bool refusesRemoval(const LLUUID& folder_id)
    {
        ensureConnected();
        const State& s = state();
        if (s.set.empty() || folder_id.isNull())
        {
            return false;
        }
        if (folder_id == trashId())
        {
            return asksBeforeEmptyingTrash();
        }
        if (inTrash(folder_id))
        {
            return false;
        }
        LLUUID blocker;
        if (s.set.count(folder_id) != 0)
        {
            blocker = folder_id;
        }
        else
        {
            for (const LLUUID& protected_id : s.set)
            {
                if (gInventory.isObjectDescendentOf(protected_id, folder_id))
                {
                    blocker = protected_id;
                    break;
                }
            }
        }
        if (blocker.isNull())
        {
            return false;
        }
        LL_WARNS("PlusProtectedFolders") << "Not removing or emptying " << folderName(folder_id)
                                         << ": it is or holds the protected folder " << folderName(blocker) << LL_ENDL;
        tell(blocker == folder_id
                 ? refusal(folder_id, blocker, false)
                 : LLTrans::getString("PlusProtectedFolderRemoveRefused",
                                      LLSD().with("NAME", plus::safeText(folderName(folder_id)))
                                            .with("INSIDE", plus::safeText(folderName(blocker)))));
        return true;
    }

    bool handleAction(const std::string& action, const LLUUID& folder_id)
    {
        if (action == "protect_folder")
        {
            protect(folder_id);
            return true;
        }
        if (action == "unprotect_folder")
        {
            unprotect({ folder_id });
            return true;
        }
        return false;
    }

    boost::signals2::connection onChanged(const std::function<void()>& slot)
    {
        ensureConnected();
        return state().changed.connect(slot);
    }

    namespace
    {
        // The protected folders as an inventory guard (plus/invguard): the official entries ask
        // it with the others'. Added when the program starts, removed when it ends.
        class FolderGuard final : public plus::invguard::Guard
        {
        public:
            FolderGuard() { plus::invguard::add(this); }
            ~FolderGuard() override { plus::invguard::remove(this); }

            bool blocksDrag(const LLUUID& folder_id) override { return plus::protectedfolders::blocksMove(folder_id); }

            bool blocksMove(const LLUUID& folder_id, const LLUUID& new_parent_id, std::string& tooltip) override
            {
                return plus::protectedfolders::blocksMove(folder_id, new_parent_id, tooltip);
            }

            bool refusesMove(const LLUUID& folder_id, const LLUUID& new_parent_id) override
            {
                return plus::protectedfolders::refusesMove(folder_id, new_parent_id);
            }

            bool blocksRemoval(const LLUUID& folder_id) override { return plus::protectedfolders::holdsProtected(folder_id); }

            bool refusesRemoval(const LLUUID& folder_id) override { return plus::protectedfolders::refusesRemoval(folder_id); }

            // Emptying the Trash asks first: this guard answers after the ones that do not ask (plus/invguard)
            bool asksUser() const override { return true; }
        };

        FolderGuard sFolderGuard;
    }
}

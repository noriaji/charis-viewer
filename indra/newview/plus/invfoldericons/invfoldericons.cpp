/**
 * @file invfoldericons.cpp
 * @brief Inventory folder icons: one colour and one emblem per system folder type.
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

#include "plus/invfoldericons/invfoldericons.h"

#include "plus/ao/aostore.h"
#include "plus/plus.h"

#include "llapp.h"
#include "llcontrol.h"
#include "llfolderview.h"
#include "llfolderviewitem.h"
#include "llinventorymodel.h"
#include "llrootview.h"
#include "llviewercontrol.h"
#include "llviewerwindow.h"

#include <array>
#include <set>

namespace plus::invfoldericons
{
    namespace
    {
        constexpr char SETTING[] = "PlusInvFolderIcons";

        struct Names
        {
            std::string closed; // empty: the official icon
            std::string open;
        };

        // Indexed by folder type + 1 (FT_NONE is -1). Filled once by init().
        std::array<Names, LLFolderType::FT_COUNT + 1> sNames;
        bool sReady = false;

        constexpr char CHARIS_ICON_CLOSED[]   = "Plus_InvFolder_charis_Closed";
        constexpr char CHARIS_ICON_OPEN[]     = "Plus_InvFolder_charis_Open";
        const std::string sCharisClosed(CHARIS_ICON_CLOSED);
        const std::string sCharisOpen(CHARIS_ICON_OPEN);

        // Folder type -> the icon key of make_icons.py
        // (skins/default/textures/plus/inv_folder_<key>_closed.png / _open.png).
        struct TypeIcon
        {
            LLFolderType::EType type;
            const char* key;
        };
        constexpr TypeIcon TYPE_ICONS[] = {
            { LLFolderType::FT_NONE,              "plain" },
            { LLFolderType::FT_ROOT_INVENTORY,    "root" },
            { LLFolderType::FT_FAVORITE,          "favorites" },
            { LLFolderType::FT_MY_OUTFITS,        "outfits" },
            { LLFolderType::FT_OUTFIT,            "outfits" },
            { LLFolderType::FT_CURRENT_OUTFIT,    "currentlook" },
            { LLFolderType::FT_CLOTHING,          "clothing" },
            { LLFolderType::FT_BODYPART,          "bodyparts" },
            { LLFolderType::FT_ANIMATION,         "animations" },
            { LLFolderType::FT_GESTURE,           "gestures" },
            { LLFolderType::FT_OBJECT,            "objects" },
            { LLFolderType::FT_LSL_TEXT,          "scripts" },
            { LLFolderType::FT_TEXTURE,           "textures" },
            { LLFolderType::FT_SNAPSHOT_CATEGORY, "snapshots" },
            { LLFolderType::FT_NOTECARD,          "notecards" },
            { LLFolderType::FT_LANDMARK,          "landmarks" },
            { LLFolderType::FT_SOUND,             "sounds" },
            { LLFolderType::FT_CALLINGCARD,       "callingcards" },
            { LLFolderType::FT_MESH,              "meshes" },
            { LLFolderType::FT_SETTINGS,          "settings" },
            { LLFolderType::FT_MATERIAL,          "materials" },
            { LLFolderType::FT_TRASH,             "trash" },
            { LLFolderType::FT_LOST_AND_FOUND,    "lostandfound" },
            { LLFolderType::FT_INBOX,             "inbox" },
            { LLFolderType::FT_OUTBOX,            "outbox" },
            // Left official: FT_BASIC_ROOT and the marketplace folders (their
            // stock / version icons tell the listing's state).
        };

        Names& namesOf(LLFolderType::EType type)
        {
            return sNames[S32(type) + 1];
        }

        void declare(LLFolderType::EType type, const std::string& key, std::set<std::string>& declared)
        {
            Names& names = namesOf(type);
            names.closed = "Plus_InvFolder_" + key + "_Closed";
            names.open   = "Plus_InvFolder_" + key + "_Open";
            // Two types share the outfit icons: their images are declared once.
            if (declared.insert(key).second)
            {
                plus::declareUIImage(names.closed, "plus/inv_folder_" + key + "_closed.png");
                plus::declareUIImage(names.open,   "plus/inv_folder_" + key + "_open.png");
            }
        }

        // The folder views keep the icons they were given; after the switch
        // they take them again. Folders only: an item's icon is not a folder
        // icon (a link to a folder keeps its old icon until it is refreshed).
        struct RefreshFolderIcons : public LLFolderViewFunctor
        {
            void doFolder(LLFolderViewFolder* folder) override
            {
                if (folder)
                {
                    folder->refreshSuffix();
                }
            }
            void doItem(LLFolderViewItem*) override {}
        };

        void refreshIn(LLView* view, RefreshFolderIcons& refresh)
        {
            if (LLFolderView* folder_view = dynamic_cast<LLFolderView*>(view))
            {
                folder_view->applyFunctorRecursively(refresh);
                return; // a folder view holds no other folder view
            }
            for (LLView* child : *view->getChildList())
            {
                refreshIn(child, refresh);
            }
        }

        // Once per switch, not per frame: walks the views of the open windows
        // (the inventory, its extra windows, the outfit and object panels).
        void onSwitched()
        {
            if (!gViewerWindow || !gViewerWindow->getRootView() || LLApp::isExiting())
            {
                return;
            }
            RefreshFolderIcons refresh;
            refreshIn(gViewerWindow->getRootView(), refresh);
        }

        void init()
        {
            std::set<std::string> declared;
            for (const TypeIcon& entry : TYPE_ICONS)
            {
                declare(entry.type, entry.key, declared);
            }
            // Outfit folders of the ensemble range are plain folders, as in the official table.
            for (S32 type = LLFolderType::FT_ENSEMBLE_START; type <= LLFolderType::FT_ENSEMBLE_END; ++type)
            {
                namesOf(LLFolderType::EType(type)) = namesOf(LLFolderType::FT_NONE);
            }
            plus::declareUIImage(sCharisClosed, "plus/inv_folder_charis_closed.png");
            plus::declareUIImage(sCharisOpen,   "plus/inv_folder_charis_open.png");
            sReady = true;

            if (LLControlVariable* control = gSavedSettings.getControl(SETTING))
            {
                control->getSignal()->connect([](LLControlVariable*, const LLSD&, const LLSD&) { onSwitched(); });
            }
        }

        plus::FeatureRegistration sRegistration({ "invfoldericons", &init, {} });
    }

    const std::string* iconName(LLFolderType::EType type, bool is_open)
    {
        if (!sReady)
        {
            return nullptr; // before init(): the settings may not be read yet
        }
        static LLCachedControl<bool> on(gSavedSettings, SETTING, true);
        if (!on)
        {
            return nullptr;
        }
        const S32 index = S32(type) + 1;
        if (index < 0 || index >= S32(sNames.size()))
        {
            return nullptr;
        }
        const Names& names = sNames[index];
        if (names.closed.empty())
        {
            return nullptr;
        }
        return is_open ? &names.open : &names.closed;
    }

    const std::string* charisFolderIconName(LLFolderType::EType type, const LLUUID& folder_id, bool is_open)
    {
        if (type != LLFolderType::FT_NONE)
        {
            return nullptr;
        }
        // Where the AO store makes it: in the inventory root. A "#Charis" folder anywhere
        // else (a copy, a subfolder of the user's, one in the trash) stays a plain folder.
        const LLViewerInventoryCategory* cat = gInventory.getCategory(folder_id);
        if (!cat || cat->getParentUUID() != gInventory.getRootFolderID()
            || cat->getName() != plus::ao::rootFolderName())
        {
            return nullptr;
        }
        // The same switch as the other folder icons: off, and the plain folder comes back.
        if (!iconName(LLFolderType::FT_NONE, is_open))
        {
            return nullptr;
        }
        return is_open ? &sCharisOpen : &sCharisClosed;
    }
}

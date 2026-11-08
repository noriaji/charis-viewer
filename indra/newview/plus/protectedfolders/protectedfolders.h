/**
 * @file protectedfolders.h
 * @brief Protected folders: inventory folders the user marked so they cannot be deleted
 *        or moved elsewhere.
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

// Ported from Firestorm's "Protect folder" (FIRE-29342: fsfloaterprotectedfolders.*, the
// bridge actions and LLInventoryModel::mProtectedCategories; Ansariel Hiller; LGPL 2.1),
// research/140.
//
// The folders are kept in the per-account setting PlusProtectedFolders (an array of UUIDs,
// in the account's settings_per_account.xml like Firestorm's FSProtectedFolders).
//
// Where it takes effect (the official hooks, each a few lines; they ask plus/invguard, where
// this feature is registered as a guard, so other guards are asked with it):
//  - get_is_category_removable(): a protected folder, and so every folder that contains one,
//    cannot be deleted (holdsProtected()). All the ways to delete go through it: the inventory tree and its
//    tabs (all, recent, AI search), the gallery, LLInventoryModel::removeCategory().
//  - LLInventoryModel::changeCategoryParent(): a protected folder cannot be moved, and a
//    folder that holds one cannot go to the Trash (moveBlocker()). Every move of a folder
//    ends there (drag and drop in the tree, the gallery, cut and paste).
//  - LLInventoryModel::consolidateForType(): a duplicate system folder that still holds a
//    protected folder is not deleted at login.
//  - LLInventoryModel::emptyFolderType() and callbackEmptyFolderType(): Lost and Found is not
//    emptied while it holds a protected folder (refusesRemoval()). Nothing in the Trash is
//    protected, but emptying it with a protected folder in it (one put there by another
//    viewer) asks first; yes unprotects it and empties the Trash. plus/ao's
//    AOStore::removeFolder() asks the same.
//  - dragCategoryIntoFolder() of the tree (LLFolderBridge) and of the gallery: the same as
//    changeCategoryParent(), shown while dragging (no drop, with a tooltip).
//  - LLFolderViewModelInventory::startDrag() (tree) and LLInventoryGallery::startDrag(): a
//    protected folder is not dragged. Only the folder itself: its parents can still be
//    dragged and moved (not into the Trash). (Not LLFolderBridge::isItemMovable(): the tree
//    asks that of every folder inside the one dragged, LLFolderViewFolder::isMovable().)
//  - LLFolderBridge::buildContextMenuFolderOptions() and performAction(): the "Protect" and
//    "Unprotect" items of a folder's menu.
// A protected folder in the Trash can still be moved out of it (to restore it), or deleted
// there one by one, as emptying the Trash deletes it.
// A change of the list is saved at once (a crash must not unprotect a folder).
//
// Main thread only.

#ifndef PLUS_PROTECTEDFOLDERS_PROTECTEDFOLDERS_H
#define PLUS_PROTECTEDFOLDERS_PROTECTEDFOLDERS_H

#include <functional>
#include <string>
#include <vector>

#include <boost/signals2/connection.hpp>

#include "lluuid.h"

namespace plus::protectedfolders
{
    // The folders, in the order they were protected.
    std::vector<LLUUID> folders();

    bool isProtected(const LLUUID& folder_id);

    // True if the folder is protected or has a protected folder anywhere inside it: such a
    // folder cannot be deleted either (it would take the protected one with it). Protected
    // folders in the Trash do not count.
    bool holdsProtected(const LLUUID& folder_id);

    void protect(const LLUUID& folder_id);
    void unprotect(const std::vector<LLUUID>& folder_ids);

    // True if the folder cannot be moved now: protected and not in the Trash. No message.
    bool blocksMove(const LLUUID& folder_id);

    // The protected folder that keeps folder_id from moving into new_parent_id, or null when
    // the move is fine:
    //  - folder_id itself, when it is protected and not in the Trash;
    //  - when new_parent_id is the Trash or inside it, a protected folder inside folder_id
    //    (in the Trash it would not be protected any more, and emptying the Trash deletes it).
    // Moving a folder to where it already is, or within the Trash, is fine.
    LLUUID moveBlocker(const LLUUID& folder_id, const LLUUID& new_parent_id);

    // True if moveBlocker() is not null; then also sets tooltip to say why. For the drag and
    // drop checks (no message).
    bool blocksMove(const LLUUID& folder_id, const LLUUID& new_parent_id, std::string& tooltip);

    // True if moveBlocker() is not null, and then tells the user (once in a while, not for
    // every folder of a batch). For code that has to refuse a move
    // (LLInventoryModel::changeCategoryParent).
    bool refusesMove(const LLUUID& folder_id, const LLUUID& new_parent_id);

    // True if the folder is protected or holds one (and is not in the Trash), and then tells
    // the user. For code that deletes a folder, or empties one, without asking
    // get_is_category_removable(): emptying Lost and Found, the AO removing its folders.
    // The Trash itself: true while it holds protected folders, and then asks whether to
    // empty it all the same (yes: unprotects them and empties it).
    bool refusesRemoval(const LLUUID& folder_id);

    // "protect_folder" and "unprotect_folder" of the folder menu: does it and returns true;
    // false for any other action.
    bool handleAction(const std::string& action, const LLUUID& folder_id);

    // Called when the list changes (also when the account's settings are read at login).
    boost::signals2::connection onChanged(const std::function<void()>& slot);
}

#endif // PLUS_PROTECTEDFOLDERS_PROTECTEDFOLDERS_H

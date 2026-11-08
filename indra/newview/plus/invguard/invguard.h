/**
 * @file invguard.h
 * @brief Inventory guards: the one place that decides whether an inventory folder may be
 *        deleted, moved or dragged. Protected folders (plus/protectedfolders) are one
 *        guard; others register their own (RLV's folder locks, rlv-wear).
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

// Every official inventory entry that can refuse a folder or an item (the tree, the gallery, the model)
// asks the free functions below; they ask every registered guard, and the first that says
// no wins. A guard is a Guard subclass with the questions it cares about overridden (the
// others allow). Research/248 section 1.3.
//
// The entries (official hooks, each a few lines; plus/plus-hooks.sh lists them):
//  - get_is_category_removable(), LLInventoryModel::consolidateForType(): blocksRemoval()
//  - get_is_item_removable(): blocksItemRemoval()
//  - get_is_category_renameable(): blocksFolderRename(); LLItemBridge::isItemRenameable(): blocksItemRename()
//  - LLInventoryModel::emptyFolderType(), callbackEmptyFolderType(): refusesRemoval()
//  - plus/ao aoengine.cpp refusesRemoval() x2 and blocksRemoval() x2, aostore.cpp refusesRemoval():
//    AO deletes through AISAPI and not get_is_category_removable()
//  - LLInventoryModel::changeCategoryParent(): refusesMove(); changeItemParent(): refusesItemMove()
//  - LLFolderBridge::dragCategoryIntoFolder(), LLInventoryGallery::dragCategoryIntoFolder():
//    blocksMove() with the tooltip
//  - LLFolderBridge::dragItemIntoFolder(), LLInventoryGallery::dragItemIntoFolder(): blocksItemMove()
//  - LLFolderBridge::perform_pasteFromClipboard(), LLInventoryGallery::paste(): refusesPaste()
//  - LLFolderViewModelInventory::startDrag(), LLInventoryGallery::startDrag(): blocksDrag()
// A new entry is added the same way: a question here, a default in Guard, and the hook in the official file.
//
// The questions without "refuses" are pure (no message, cheap: they run for every folder of
// a tree). The "refuses" ones tell the user why, once; they are for code that has to say no.
//
// With several guards the "refuses" move and removal questions are asked in two steps, because a
// "refuses" question can show a message or a confirmation: first the pure "blocks" question of the
// guards that do not ask the user (asksUser() false). The first of them that blocks answers (and tells
// why) and no confirmation of another guard opens. Only when none blocks is every guard asked its
// "refuses" question in turn. So a confirmation (protected folders: "unprotect and delete?") never
// starts something another guard (RLV's folder locks) refuses.
//
// Guards are not owned: the caller keeps the object alive until remove() (a static one
// removes itself in its destructor). The set is changed on the main thread only, and not while a question is
// being asked.

#ifndef PLUS_INVGUARD_INVGUARD_H
#define PLUS_INVGUARD_INVGUARD_H

#include <string>

#include "lluuid.h"

namespace plus::invguard
{
    class Guard
    {
    public:
        virtual ~Guard() = default;

        // The folder itself cannot be dragged now.
        virtual bool blocksDrag(const LLUUID& folder_id) { return false; }

        // The folder cannot move into new_parent_id; then sets tooltip to say why (shown while
        // dragging).
        virtual bool blocksMove(const LLUUID& folder_id, const LLUUID& new_parent_id, std::string& tooltip) { return false; }

        // The same as blocksMove(), and tells the user. For code that has to refuse a move.
        virtual bool refusesMove(const LLUUID& folder_id, const LLUUID& new_parent_id) { return false; }

        // The folder cannot be deleted (itself, or something inside it is kept).
        virtual bool blocksRemoval(const LLUUID& folder_id) { return false; }

        // The same as blocksRemoval(), and tells the user (or asks to go on). For code that
        // deletes or empties a folder without asking get_is_category_removable().
        virtual bool refusesRemoval(const LLUUID& folder_id) { return false; }

        // The folder cannot be renamed.
        virtual bool blocksFolderRename(const LLUUID& folder_id) { return false; }

        // The item cannot be deleted / moved into new_parent_id / renamed.
        virtual bool blocksItemRemoval(const LLUUID& item_id) { return false; }
        virtual bool blocksItemMove(const LLUUID& item_id, const LLUUID& new_parent_id) { return false; }
        virtual bool blocksItemRename(const LLUUID& item_id) { return false; }

        // The same as blocksItemMove() for code that has to refuse a move, and tells the user.
        virtual bool refusesItemMove(const LLUUID& item_id, const LLUUID& new_parent_id) { return false; }

        // The item or folder source_id cannot be pasted into dest_folder_id; tells the user.
        virtual bool refusesPaste(const LLUUID& source_id, bool source_is_folder, const LLUUID& dest_folder_id) { return false; }

        // True if refusesRemoval() / refusesMove() may open a confirmation window (it is asked last).
        virtual bool asksUser() const { return false; }
    };

    // Registers a guard (ignored if null or already registered); remove() takes it out again.
    void add(Guard* guard);
    void remove(Guard* guard);

    // Each is true if any guard says no (the first to say it answers; the rest are not asked).
    bool blocksDrag(const LLUUID& folder_id);
    bool blocksMove(const LLUUID& folder_id, const LLUUID& new_parent_id, std::string& tooltip);
    bool refusesMove(const LLUUID& folder_id, const LLUUID& new_parent_id);
    bool blocksRemoval(const LLUUID& folder_id);
    bool refusesRemoval(const LLUUID& folder_id);
    bool blocksFolderRename(const LLUUID& folder_id);
    bool blocksItemRemoval(const LLUUID& item_id);
    bool blocksItemMove(const LLUUID& item_id, const LLUUID& new_parent_id);
    bool blocksItemRename(const LLUUID& item_id);
    bool refusesItemMove(const LLUUID& item_id, const LLUUID& new_parent_id);
    bool refusesPaste(const LLUUID& source_id, bool source_is_folder, const LLUUID& dest_folder_id);
}

#endif // PLUS_INVGUARD_INVGUARD_H

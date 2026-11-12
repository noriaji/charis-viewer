/**
 * @file invcolumns.cpp
 * @brief Inventory window like a file explorer: three columns while the window is wide enough.
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

// research/318. While the inventory window is wide enough (PlusInvExplorerColumnsWidth):
// - left: the tab shown is the same panel as always, its folder view set to folders only
//   (LLFolderView::setFoldersOnly, a marked change in llui): its filter still decides which folders show
//   (a search leaves the folders holding a match), only the item rows are hidden;
// - center: an official single-folder list (panel_plus_inv_center.xml) opens the folder chosen on the left and
//   takes the tab's filter (LLInventoryFilter::plusCopyFrom, copied whenever the tab's filter generation moved)
//   and sort order; a double click on a folder there opens it, and the left selects it;
// - right: the detail pane (invexplorer.cpp, placed by invlayout.cpp right of the center while it shows) shows what
//   is selected in either column.
// Where the columns are and when they come is invlayout.cpp's (the rectangles of the tab's contents, the split,
// the lines): it makes the center column (panel_plus_inv_center.xml) and hands its list over (connectCenter), and
// calls enter(), follow() and leave(). This file has what the columns mean: the folders-only flag, the filter's
// copy, the two selections, the rename, the column the gear menu acts on.
// One way only for the filter: tab -> center (the center has no search box). The two selections move each
// other through `syncing`, which a FlagScope sets and puts back on every way out.
//
// State (research/149 (R), research/318 6.5), all on the UI thread:
//   active       set by enter(), cleared by leave() (invlayout decides when). The views carrying the flag die
//                with the window.
//   left / leftRoot  the tab shown and its folder view, set to folders only while active; moved to the new
//                tab when the tab changes (custom tabs come and go: handles), put back when leaving.
//   copiedGeneration the tab filter's generation last copied (-1: copy at the next tick).
//   centerChosen the last choice was made in the center: the gear menu and the detail pane act on its selection
//                (commandPanel, centerSelection). Set by the center's notification (onCenterSelection), by the
//                focus (chosenCenter) and by keepEnteredSelection, which stands in for a notification that does
//                not come; taken down when the left is chosen (onSelection), the center's root changes
//                (onCenterRootChanged), the received items are chosen (followTab) and with the left (detachLeft).
//   itemBefore   the item selected on the left when the columns came (attachLeft; null: a folder or none),
//                until leave(), which selects it on the left again when the center selects nothing.
//   echoAwaited  the left has yet to give itemBefore way to its folder and ask for it again (attachLeft with an
//                item). Once: taken down by the first notification of the left with a row that gave way
//                (onSelection, whether the center's selection was kept or not), when itemBefore is no longer the
//                left's current selection without that notification having come (followTab: the left was chosen
//                or lost its selection first, its tab built a new folder view) and with the left (detachLeft).
//   shownParent  the folder the center's folder is in, noted every frame while that folder exists: where the
//                center goes when its folder is removed from the inventory (leaveGoneFolder).
//   joining      the other objects of a selection of several that came with the columns (attachLeft): added to
//                the center's selection once their rows are built (joinSelection); let go when the center shows
//                another folder, something else is selected or asked for first, the left itself is chosen
//                (onSelection without a row that gave way), or the columns go.

#include "llviewerprecompiledheaders.h"

#include "invcolumns.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

#include "invexplorer.h"

#include "llfloatersidepanelcontainer.h"
#include "llfocusmgr.h"
#include "llfolderview.h"
#include "llfolderviewitem.h"
#include "llfolderviewmodelinventory.h"
#include "llinventoryfilter.h"
#include "llinventorymodel.h"
#include "llinventorypanel.h"
#include "llpanelmaininventory.h"
#include "llsidepanelinventory.h"
#include "llviewerinventory.h"

namespace
{
    // Sets a flag for the scope and puts back what it was before, on every way out (nested scopes too).
    class FlagScope
    {
    public:
        explicit FlagScope(bool& flag) : mFlag(flag), mBefore(flag) { mFlag = true; }
        ~FlagScope() { mFlag = mBefore; }
        FlagScope(const FlagScope&) = delete;
        FlagScope& operator=(const FlagScope&) = delete;

    private:
        bool& mFlag;
        bool  mBefore;
    };

    // The center's own list gives the window the keyboard focus and brings it to the front whenever its root changes
    // (LLInventorySingleFolderPanel::updateSingleFolderRoot). Not for a change the columns make: a selection made on
    // the left without the keyboard focus (an object's contents copied to the inventory, an object moved into a
    // folder open there, a new object) must not, through the center following it, take the keys from the world,
    // from a chat line (an IME composition there) or from the left tree, nor put the window over another one. (A
    // selection the official code makes with the focus, as for an object received or taken, takes it in one column
    // too.) Only with the focus in the center's list itself, whose rows the change destroys, does the window take
    // it, as the official list has it.
    class QuietRootChange
    {
    public:
        explicit QuietRootChange(LLInventorySingleFolderPanel* list)
            : mList(list), mBefore(list->plusSetRootChangeTakesFocus(gFocusMgr.childHasKeyboardFocus(list))) {}
        ~QuietRootChange() { mList->plusSetRootChangeTakesFocus(mBefore); }
        QuietRootChange(const QuietRootChange&) = delete;
        QuietRootChange& operator=(const QuietRootChange&) = delete;

    private:
        LLInventorySingleFolderPanel* mList;   // (the caller's: it outlives the scope)
        bool mBefore;
    };

    // The rest of a selection of several objects that the center is still to take (attachLeft, joinSelection).
    struct Joining
    {
        LLUUID current;                    // the one selected first, which stays the current selection (null: nothing waits)
        std::vector<LLUUID> others;        // the others (joinSelection drops those not in the center's folder)
        LLHandle<LLFolderView> root;       // the center's root they are rows of (a rebuilt list is another one)

        void clear()
        {
            current.setNull();
            others.clear();
            root = LLHandle<LLFolderView>();
        }
    };

    struct Columns
    {
        LLHandle<LLPanel> owner;           // the LLPanelMainInventory this entry is for
        LLHandle<LLPanel> center;          // the single-folder list of the center column (connectCenter)
        boost::signals2::scoped_connection rootChanged;

        bool active = false;               // three columns now
        LLHandle<LLPanel>      left;       // the tab shown as folders only
        LLHandle<LLFolderView> leftRoot;   // its folder view, which carries the flag
        S32  copiedGeneration = -1;        // the tab filter's generation last copied to the center (-1: none)
        bool syncing      = false;         // one column is moving the other
        bool centerChosen = false;         // the last choice was made in the center (onCenterSelection, chosenCenter by the focus)
        bool leftHadSelection = false;     // the left had a selection at the last frame (followTab sees it lost)
        LLUUID renaming;                   // the new object renameInCenter() is to name (null: none)
        LLHandle<LLFolderView> renamingRoot; // the center's root it was set up on (a rebuilt list is another one)
        LLUUID itemBefore;                 // the item selected on the left when the columns came (null: a folder or none)
        bool echoAwaited = false;          // the left has yet to give itemBefore way and ask for it again (once: onSelection)
        LLUUID shownParent;                // the folder the center's folder is in (null: none, or not noted yet)
        Joining joining;                   // the objects selected with itemBefore, until the center has selected them
    };

    std::unordered_map<const LLPanelMainInventory*, Columns> sColumns;

    using plus::invexplorer::idOf;

    LLInventorySingleFolderPanel* centerOf(const Columns& c)
    {
        return dynamic_cast<LLInventorySingleFolderPanel*>(c.center.get());
    }

    LLInventoryPanel* leftOf(const Columns& c)
    {
        return dynamic_cast<LLInventoryPanel*>(c.left.get());
    }

    Columns* find(const LLPanelMainInventory* owner)
    {
        auto it = sColumns.find(owner);
        return it == sColumns.end() ? nullptr : &it->second;
    }

    Columns* find(const LLHandle<LLPanel>& owner_handle)
    {
        const LLPanelMainInventory* owner = dynamic_cast<const LLPanelMainInventory*>(owner_handle.get());
        return owner ? find(owner) : nullptr;
    }

    // The window's entry: made when its center column is connected (connectCenter).
    Columns& entryFor(const LLPanelMainInventory* owner)
    {
        auto [it, made] = sColumns.try_emplace(owner);
        if (made)
        {
            it->second.owner = owner->getHandle();
        }
        return it->second;
    }

    // The received items (the inbox under the inventory) have a selection. The official views clear the tab's
    // selection when the inbox takes the focus (LLPanelMarketplaceInbox::onFocusReceived), and the inbox's selection
    // when the inventory takes it (LLPanelMainInventory::onFocusReceived): sharing then acts on the inbox's selection
    // (LLAvatarActions::getInventorySelectedUUIDs falls back to it when the tab has none), and the gear menu's "Share"
    // is off. Both clear through the main inventory window's side panel (LLFloaterSidePanelContainer "inventory"),
    // whichever window was clicked, so that one is looked at too, besides this window's own. Called only when the
    // left lost its selection and when the gear menu asks (chosenCenter): it copies the selection set.
    bool inboxHasSelection(const LLPanelMainInventory* owner)
    {
        LLSidepanelInventory* own = owner ? owner->getParentByType<LLSidepanelInventory>() : nullptr;
        if (own && !own->getInboxSelectionList().empty())
        {
            return true;
        }
        LLSidepanelInventory* main = LLFloaterSidePanelContainer::findPanel<LLSidepanelInventory>("inventory");
        return main && main != own && !main->getInboxSelectionList().empty();
    }

    // Something was chosen in this window's columns (the center by the user, the left while the inventory has the
    // keyboard focus): this window's received items let go of their selection, so that a selection left there
    // always means they were chosen later (inboxHasSelection). The official focus handler does it for the main
    // window only (LLPanelMainInventory::onFocusReceived clears the "inventory" side panel's), so another window
    // (Ctrl+Shift+I) would keep an old one.
    // Only when there is a selection to let go of (chosenCenter may ask often: an avatar picker's OK button).
    void clearOwnInbox(const LLPanelMainInventory* owner)
    {
        LLSidepanelInventory* own = owner ? owner->getParentByType<LLSidepanelInventory>() : nullptr;
        LLInventoryPanel* inbox   = own ? own->getInboxPanel() : nullptr;
        LLFolderView* inbox_root  = inbox ? inbox->getRootFolder() : nullptr;
        if (inbox_root && !inbox_root->getSelectedItems().empty())
        {
            own->clearSelections(false, true);
        }
    }

    // The folder an inventory id opens in the center column: a folder itself, an item (or a link) the folder it
    // is in, with the item to select there. Null when the id is not in the inventory.
    void folderToOpen(const LLUUID& id, LLUUID& folder, LLUUID& item)
    {
        folder.setNull();
        item.setNull();
        if (gInventory.getCategory(id))
        {
            folder = id;
        }
        else if (const LLViewerInventoryItem* inv_item = gInventory.getItem(id))
        {
            folder = inv_item->getParentUUID();
            item   = folder.notNull() ? id : LLUUID::null;
        }
    }

    // What a view selects besides its current selection (the last of its list): the others of a selection of several.
    std::vector<LLUUID> othersSelected(LLFolderView* root)
    {
        std::vector<LLUUID> others;
        const LLFolderViewItem* current = root->getCurSelectedItem();
        for (const LLFolderViewItem* row : root->getSelectedItems())
        {
            if (row != current)
            {
                others.push_back(idOf(row));
            }
        }
        return others;
    }

    // Adds the built rows of `ids` to a view's selection, when `current` is its current selection: that one stays
    // the current one (the last in the view's list). Nothing in a view that selects one row at a time.
    void addToSelection(LLInventoryPanel* view, const LLUUID& current, const std::vector<LLUUID>& ids)
    {
        LLFolderView* root = view->getRootFolder();
        LLFolderViewItem* current_row = root ? root->getCurSelectedItem() : nullptr;
        if (!current_row || idOf(current_row) != current || !root->getAllowMultiSelect())
        {
            return;
        }
        bool added = false;
        for (const LLUUID& id : ids)
        {
            LLFolderViewItem* row = view->getItemByID(id);
            if (row && !row->isSelected())   // (changeSelection toggles an item: never one selected already)
            {
                root->changeSelection(row, true);
                added = true;
            }
        }
        if (added)
        {
            root->addToSelectionList(current_row);   // moves it back to the end of the list
        }
    }

    // Opens a folder in the center column, without the window taking the keyboard focus (QuietRootChange), and
    // selects an object there, alone (what was selected there before is not kept; onCenterSelection is told at the
    // list's next update).
    void showFolder(Columns& c, LLInventorySingleFolderPanel* center, const LLUUID& folder, const LLUUID& item)
    {
        if (folder.isNull())
        {
            return;
        }
        FlagScope scope(c.syncing);
        {
            QuietRootChange quiet(center);
            center->changeFolderRoot(folder);   // (the root was built by attachLeft)
        }
        center->clearNavigationHistory(); // no back or forward in the columns: the lists would only grow
        if (item.isNull())
        {
            return;
        }
        if (item != c.joining.current)
        {
            c.joining.clear();   // another object is asked for: the selection that came with the columns is not waited for
        }
        center->setSelectionByID(item, false);
    }

    // The left gave the item the columns came with way to its folder (onSelection, one update after attachLeft; only
    // that first request is brought here, echoAwaited: the same item shown again later is selected alone): the
    // center has selected that item already, maybe with the others that were selected with it (joinSelection).
    // Selecting it again would leave it alone, so the center's selection stays as it is and what its notification
    // would do is done here: the last choice is the center's (the caller shows it in the detail pane,
    // centerSelection), a selection the list still waits to make is let go (LLInventoryPanel::clearSelection only
    // forgets that; setSelectionByID would have), and the item is scrolled to.
    bool keepEnteredSelection(Columns& c, LLInventorySingleFolderPanel* center, const LLUUID& item)
    {
        LLFolderView* root = center->getRootFolder();
        if (item.isNull() || item != c.itemBefore || !root || idOf(root->getCurSelectedItem()) != item)
        {
            return false;
        }
        center->clearSelection();
        root->scrollToShowSelection();
        c.centerChosen = c.active;
        return true;
    }

    // The folder the center shows was removed from the inventory (purged from the trash, the trash emptied, here or
    // from another window). The official list keeps its root view for this (plusSetRootOutlivesFolder, connectCenter:
    // a list that lost its root would be taken apart for good), showing nothing; the center moves to what the left
    // selects, else to the folder the gone one was in, else to My Inventory. Every frame in three columns: one lookup.
    void leaveGoneFolder(Columns& c, LLInventoryPanel* tab, LLInventorySingleFolderPanel* center)
    {
        if (const LLViewerInventoryCategory* shown = gInventory.getCategory(center->getSingleFolderRoot()))
        {
            c.shownParent = shown->getParentUUID();
            return;
        }
        LLUUID folder;
        LLUUID item;
        LLFolderView* root = tab->getRootFolder();
        folderToOpen(idOf(root ? root->getCurSelectedItem() : nullptr), folder, item);
        if (folder.isNull())
        {
            folder = gInventory.getCategory(c.shownParent) ? c.shownParent : gInventory.getRootFolderID();
        }
        if (gInventory.getCategory(folder))   // (none at all, the inventory not usable: looked for again at the next frame)
        {
            showFolder(c, center, folder, item);
        }
    }

    // The objects selected with the one the columns came with (attachLeft) join it in the center once all their
    // rows are built (the official list builds a folder's rows in its idle, a large folder's over several frames,
    // and selects the one asked for at its next draw: LLInventoryPanel::updateSelection). Every frame in three
    // columns; the rows are looked up only while a selection waits. Not waited for any longer when the center shows
    // another folder or something else was selected first; objects no longer in the folder (moved, deleted) get no
    // row there and are not waited for.
    void joinSelection(Columns& c, LLInventorySingleFolderPanel* center)
    {
        Joining& joining = c.joining;
        if (joining.current.isNull())
        {
            return;
        }
        const LLUUID folder = center->getSingleFolderRoot();
        const auto in_folder = [&folder](const LLUUID& id)
        {
            const LLInventoryObject* object = gInventory.getObject(id);
            return object && object->getParentUUID() == folder;
        };
        LLFolderView* root = center->getRootFolder();
        LLFolderViewItem* selected = root ? root->getCurSelectedItem() : nullptr;
        if (!root || root != joining.root.get() || (selected && idOf(selected) != joining.current)
            || !in_folder(joining.current))
        {
            joining.clear();
            return;
        }
        std::erase_if(joining.others, [&in_folder](const LLUUID& id) { return !in_folder(id); });
        const auto built = [center](const LLUUID& id) { return center->getItemByID(id) != nullptr; };
        if (!built(joining.current) || !std::all_of(joining.others.begin(), joining.others.end(), built))
        {
            return;
        }
        if (!selected)
        {
            center->setSelectionByID(joining.current, false);   // (its row is built: selected at once)
        }
        addToSelection(center, joining.current, joining.others);
        joining.clear();
    }

    // The center's own root changed (a double click on a folder there, "open", a new folder): the left selects it.
    void onCenterRootChanged(const LLHandle<LLPanel>& owner_handle)
    {
        Columns* c = find(owner_handle);
        if (!c || !c->active || c->syncing)
        {
            return;
        }
        LLInventorySingleFolderPanel* center = centerOf(*c);
        LLInventoryPanel* left = leftOf(*c);
        if (!center || !left)
        {
            return;
        }
        FlagScope scope(c->syncing);
        c->centerChosen = false;   // the list was rebuilt: nothing is selected in it
        center->clearNavigationHistory();
        left->setSelectionByID(center->getSingleFolderRoot(), false);
    }

    // The center's selection changed (LLFolderView signals only a selection that is not empty): the detail pane
    // shows it, and the gear menu acts on it (commandPanel) until the left is chosen again.
    void onCenterSelection(const LLHandle<LLPanel>& owner_handle, const std::deque<LLFolderViewItem*>& items, bool user_action)
    {
        LLPanelMainInventory* owner = dynamic_cast<LLPanelMainInventory*>(owner_handle.get());
        Columns* c = owner ? find(owner) : nullptr;
        LLInventorySingleFolderPanel* center = c ? centerOf(*c) : nullptr;
        if (!center)
        {
            return;
        }
        // A rename that renameInCenter() set up waits for its object: when something else is selected first (the new
        // object is hidden by the center's filter, or another row was clicked), it must not open on that. Only on the
        // root it was set up on: a list rebuilt since then may wait for a rename of its own (a new object made there).
        LLFolderView* root = center->getRootFolder();
        if (c->renaming.notNull())
        {
            if (root && root == c->renamingRoot.get() && root->needsAutoRename()
                && (items.empty() || idOf(items.back()) != c->renaming))
            {
                root->setNeedsAutoRename(false);
            }
            c->renaming.setNull();
            c->renamingRoot = LLHandle<LLFolderView>();
        }
        center->onSelectionChange(items, user_action);   // as LLPanelMainInventory::onSelectionChange does for its views
        c->centerChosen = c->active && !items.empty();
        if (c->centerChosen && user_action)
        {
            clearOwnInbox(owner);
        }
        plus::invexplorer::showInDetail(owner, items);
    }

    void setLeftRoot(Columns& c, LLFolderView* root)
    {
        if (root == c.leftRoot.get())
        {
            return;
        }
        if (LLFolderView* old_root = c.leftRoot.get())
        {
            old_root->setFoldersOnly(false);
        }
        c.leftRoot = root ? root->getHandle() : LLHandle<LLFolderView>();
        if (root)
        {
            root->setFoldersOnly(true);
        }
    }

    // A rename renameInCenter() set up that can no longer happen is let go as soon as that is known, so it does not
    // wait on and take away the rename of the next new object made there: the list was rebuilt (another root), or
    // the new object's row was filtered out by the center's filter (not before: a row the filter has not looked at
    // yet is potentially visible), or the object is gone. Every frame in three columns; the row is looked up only
    // while a rename is set up.
    void letGoOfRename(Columns& c, LLInventorySingleFolderPanel* center)
    {
        if (c.renaming.isNull())
        {
            return;
        }
        LLFolderView* root = center->getRootFolder();
        bool let_go = root != c.renamingRoot.get();
        if (!let_go)
        {
            LLFolderViewItem* row = center->getItemByID(c.renaming);
            if (row)
            {
                let_go = row->getViewModelItem() && !row->getViewModelItem()->potentiallyVisible();
            }
            else
            {
                let_go = !gInventory.getItem(c.renaming);
            }
            if (let_go && root)
            {
                root->setNeedsAutoRename(false);
            }
        }
        if (let_go)
        {
            c.renaming.setNull();
            c.renamingRoot = LLHandle<LLFolderView>();
        }
    }

    // While in three columns, every frame: the tab's folder view (in case the tab built a new one), the center's
    // folder (in case it is gone), the tab's filter (only when its generation moved; plusCopyFrom writes only what
    // differs) and its sort order.
    void followTab(Columns& c, LLInventoryPanel* tab, LLInventorySingleFolderPanel* center)
    {
        setLeftRoot(c, tab->getRootFolder());
        // The left asking again for the item the columns came with (echoAwaited) is waited for only while that item
        // is still the left's current selection: once the left selects something else or nothing (it was chosen
        // itself, the received items took its selection, its tab built a new folder view), that request does not
        // come any more, and a later one for the same item is an ordinary one. One comparison, only while waiting.
        // (attachLeft calls this before the left's next update: the item is still its current selection there.)
        if (c.echoAwaited)
        {
            LLFolderView* left_root = tab->getRootFolder();
            c.echoAwaited = left_root && idOf(left_root->getCurSelectedItem()) == c.itemBefore;
        }
        leaveGoneFolder(c, tab, center);
        const S32 generation = tab->getFilter().getCurrentGeneration();
        if (generation != c.copiedGeneration)
        {
            c.copiedGeneration = generation;
            center->getFilter().plusCopyFrom(tab->getFilter());
        }
        const U32 order = tab->getSortOrder();
        if (center->getRootFolder() && order != center->getSortOrder())   // (setSortOrder needs the root)
        {
            center->setSortOrder(order);
        }
        letGoOfRename(c, center);
        joinSelection(c, center);
        // The left lost its selection without choosing anything (a drop into a folder in the center deselects all
        // there, LLFolderBridge::dragItemIntoFolder): it is given the center's folder. Only on the loss itself (a
        // selection at the last frame, none now): a left that had none, as when the columns come, may be waiting
        // for a selection asked for before its row was built (LLInventoryPanel::mSelectThisID), which this would
        // overwrite. Only a folder the left can show: one its filter hides would be dropped again at once.
        // Not when the received items were chosen: the official views clear the tab's selection on purpose then, so
        // that sharing acts on the received items (inboxHasSelection); nothing is chosen in the center either.
        LLFolderView* root = tab->getRootFolder();
        const bool has_selection = root && !root->getSelectedItems().empty();
        const bool lost = c.leftHadSelection && !has_selection;
        c.leftHadSelection = has_selection;
        if (!lost)
        {
            return;
        }
        if (inboxHasSelection(dynamic_cast<const LLPanelMainInventory*>(c.owner.get())))
        {
            c.centerChosen = false;
            return;
        }
        const LLUUID shown = center->getSingleFolderRoot();
        LLFolderViewItem* view = tab->getItemByID(shown);
        if (view && view != root && view->getViewModelItem() && view->getViewModelItem()->potentiallyVisible())
        {
            FlagScope scope(c.syncing);
            tab->setSelectionByID(shown, false);
        }
    }

    // The tab stops being the left column (leaving, another tab): its item rows back. (Its width and its follows
    // are invlayout's: it narrows the tab shown and gives the one before back.)
    void detachLeft(Columns& c)
    {
        setLeftRoot(c, nullptr);
        c.left             = LLHandle<LLPanel>();
        c.copiedGeneration = -1;
        c.centerChosen     = false;
        c.leftHadSelection = false;
        c.echoAwaited      = false;   // (the tab that was to give the item way is not the left any more)
        // (not the rename set up in the center: switchTab keeps the center and its rename)
    }

    // A tab becomes the left column: folders only, its filter and order on the center, and the center opens the
    // folder selected there (an item selected there: its folder, with the item selected in the center; the item
    // itself gives way to the folder on the left, LLFolderView::sanitizeSelection), else My Inventory. The item is
    // always selected anew, alone (what the center had selected when it was last shown goes); then the other objects
    // selected with it in its folder join it (joinSelection); those selected in other folders do not (the center
    // shows one folder).
    void attachLeft(Columns& c, LLInventoryPanel* tab, LLInventorySingleFolderPanel* center)
    {
        LLUUID folder;
        LLUUID item;
        LLFolderView* root = tab->getRootFolder();
        folderToOpen(idOf(root ? root->getCurSelectedItem() : nullptr), folder, item);
        c.itemBefore  = item;
        c.echoAwaited = item.notNull();   // (the left gives it way at its next update)
        c.joining.clear();
        std::vector<LLUUID> others;
        if (item.notNull())
        {
            others = othersSelected(root);   // (joinSelection keeps those in the item's folder)
        }
        if (folder.isNull())
        {
            folder = gInventory.getRootFolderID();
        }
        // The first time, the official list builds its root (null: the root it was made with). Before the copy in
        // followTab: building the root sets the list's own filter and sort order.
        center->initFolderRoot(folder);
        c.left = tab->getHandle();
        showFolder(c, center, folder, item);
        LLFolderView* center_root = center->getRootFolder();
        if (!others.empty() && center_root)
        {
            c.joining.current = item;
            c.joining.others  = std::move(others);
            c.joining.root    = center_root->getHandle();
        }
        followTab(c, tab, center);
    }


    // Another tab is shown while in three columns: as when the columns come, the center follows the new tab's
    // selection ("Show in inventory" from another tab switches to My Inventory and selects the object there, maybe
    // later, once its row is built: it comes through onSelection). Only while a new object made in the center waits
    // to be named (its rename is set up before the next tick, menu_create_inventory_item) does the center stay in
    // its folder: in a custom tab, reset_inventory_filter() switches to My Inventory first, and the new object must
    // stay in sight. Then the new tab selects that folder.
    void switchTab(Columns& c, LLInventoryPanel* tab, LLInventorySingleFolderPanel* center)
    {
        const LLFolderView* center_root = center->getRootFolder();
        const bool naming_new = center_root && center_root->needsAutoRename();
        detachLeft(c);
        const LLUUID shown = center->getSingleFolderRoot();
        if (!naming_new || shown.isNull() || !gInventory.getCategory(shown))
        {
            attachLeft(c, tab, center);
            return;
        }
        c.left = tab->getHandle();
        followTab(c, tab, center);
        FlagScope scope(c.syncing);
        tab->setSelectionByID(shown, false);
    }

    // A new object was made on the left (its rename was waiting) and gave way to its folder there: it is selected
    // and named in the center instead, as the official views do for a new object
    // (LLPanelMainInventory::updateCombinationVisibility).
    void renameInCenter(Columns& c, LLInventorySingleFolderPanel* center, const LLUUID& item)
    {
        center->setSelectionByID(item, true);   // the renamer needs the keyboard focus
        if (LLFolderView* root = center->getRootFolder())
        {
            c.renaming     = item;              // (onCenterSelection lets the rename go when another row comes first)
            c.renamingRoot = root->getHandle();
            root->scrollToShowSelection();
            root->setNeedsAutoRename(true);
        }
    }
}

namespace plus
{
namespace invcolumns
{
    void connectCenter(LLPanelMainInventory* owner, LLInventorySingleFolderPanel* center)
    {
        Columns& c = entryFor(owner);
        c.center = center->getHandle();
        // A folder removed from the inventory while the center shows it does not take the list's root along (the
        // official list would be taken apart for good then): leaveGoneFolder moves the center away.
        center->plusSetRootOutlivesFolder(true);
        const LLHandle<LLPanel> owner_handle = owner->getHandle();
        center->setSelectCallback([owner_handle](const std::deque<LLFolderViewItem*>& items, bool user_action)
        {
            onCenterSelection(owner_handle, items, user_action);
        });
        c.rootChanged = center->setRootChangedCallback([owner_handle]() { onCenterRootChanged(owner_handle); });
    }

    bool isActive(const LLPanelMainInventory* owner)
    {
        const Columns* c = find(owner);
        return c && c->active;
    }

    bool enter(LLPanelMainInventory* owner, LLInventoryPanel* tab)
    {
        Columns* c = find(owner);
        LLInventorySingleFolderPanel* center = c ? centerOf(*c) : nullptr;
        if (!center)
        {
            return false;
        }
        c->active = true;
        attachLeft(*c, tab, center);
        return true;
    }

    bool follow(LLPanelMainInventory* owner, LLInventoryPanel* tab)
    {
        Columns* c = find(owner);
        LLInventorySingleFolderPanel* center = c && c->active ? centerOf(*c) : nullptr;
        if (!center)
        {
            return false;   // (not expected: the center is the window's own child and goes with it)
        }
        if (tab != c->left.get())
        {
            switchTab(*c, tab, center);
        }
        else
        {
            followTab(*c, tab, center);
        }
        return true;
    }

    void leave(LLPanelMainInventory* owner)
    {
        Columns* c = find(owner);
        if (!c || !c->active)
        {
            return;
        }
        // Back to one column: the left shows its items again and selects what was selected in the center, all of it
        // (or the item selected before the columns came, while the center is still in that item's folder, with the
        // objects that were selected with it and had not joined it in the center yet), so one column goes on from
        // there. The others are selected with the current one only when its row on the left is built (it is selected
        // at once then; a row built later is selected alone by the official list, LLInventoryPanel::updateSelection).
        LLInventoryPanel* left = leftOf(*c);
        LLUUID reselect;
        std::vector<LLUUID> others;
        if (LLInventorySingleFolderPanel* center = centerOf(*c))
        {
            LLFolderView* center_root = center->getRootFolder();
            if (center_root && !center_root->getSelectedItems().empty())
            {
                reselect = idOf(center_root->getSelectedItems().back());
                others   = othersSelected(center_root);
            }
            const LLViewerInventoryItem* before = c->itemBefore.notNull() ? gInventory.getItem(c->itemBefore) : nullptr;
            if (reselect.isNull() && before && before->getParentUUID() == center->getSingleFolderRoot())
            {
                reselect = c->itemBefore;
                if (c->joining.current == reselect)
                {
                    others.swap(c->joining.others);
                }
            }
        }
        detachLeft(*c);
        c->active = false;   // first: the center does not follow the selection below
        c->itemBefore.setNull();
        c->joining.clear();
        c->renaming.setNull();
        c->renamingRoot = LLHandle<LLFolderView>();
        if (left && reselect.notNull())
        {
            left->setSelectionByID(reselect, false);
            addToSelection(left, reselect, others);
        }
    }

    void onSelection(LLPanelMainInventory* owner, LLInventoryPanel* panel, const std::deque<LLFolderViewItem*>& items)
    {
        Columns* c = find(owner);
        if (!c || !c->active || !panel || panel != c->left.get())
        {
            return;
        }
        // An item selected on the left ("Show in inventory", "Find original", a new object) gave way to its folder
        // in this same update (LLFolderView::sanitizeSelection): the center opens the item's folder and selects it.
        bool wanted_rename = false;
        LLFolderView* root = panel->getRootFolder();
        LLFolderViewItem* gave_way = root ? root->takeFoldersOnlyGaveWay(&wanted_rename) : nullptr;
        // The first row that gives way after the columns came is the item they came with (attachLeft): only that
        // request may keep the center's selection (keepEnteredSelection). Once, whatever becomes of the request.
        const bool entered_item_again = gave_way && c->echoAwaited;
        if (gave_way)
        {
            c->echoAwaited = false;
        }
        LLInventorySingleFolderPanel* center = centerOf(*c);
        if (c->syncing || items.empty() || !center)
        {
            return;
        }
        c->centerChosen = false;
        if (!gave_way)
        {
            c->joining.clear();   // chosen on the left itself: the selection that came with the columns is not waited for
        }
        // Chosen in the left tree ("Show in inventory" moves the focus there too): the window has the focus, not in
        // its detail pane (a child of the window too, research/318 7.3: the text or a button there chooses nothing).
        if (gFocusMgr.childHasKeyboardFocus(owner) && !plus::invexplorer::paneHasFocus(owner))
        {
            clearOwnInbox(owner);
        }
        LLUUID folder;
        LLUUID item;
        folderToOpen(idOf(gave_way ? gave_way : items.back()), folder, item);   // (back: the current selection)
        if (!entered_item_again || !keepEnteredSelection(*c, center, item))   // (after centerChosen = false above: it sets it back)
        {
            showFolder(*c, center, folder, item);
        }
        if (wanted_rename && item.notNull())
        {
            renameInCenter(*c, center, item);
        }
        else if (gave_way && item.notNull() && gFocusMgr.childHasKeyboardFocus(panel))
        {
            // The object asked for ("Show in inventory", "Find original") is selected in the center, not on the left:
            // the keyboard focus the left was given for it goes there too, so that the gear menu and the context
            // menu act on it (chosenCenter looks at the focus first) and the arrow keys move from it.
            center->setFocus(true);
        }
    }

    LLInventoryPanel* chosenCenter(LLPanelMainInventory* owner)
    {
        Columns* c = find(owner);
        LLInventorySingleFolderPanel* center = c && c->active ? centerOf(*c) : nullptr;
        LLFolderView* root = center ? center->getRootFolder() : nullptr;
        if (!root)
        {
            return nullptr;
        }
        const bool selected = !root->getSelectedItems().empty();
        // What is known at once comes first: a click or a right click on a column selects there and gives it the
        // keyboard focus on the spot (LLFolderViewItem::handleMouseDown / handleRightMouseDown), and the context menu
        // is built right after, while the selection's notification (onCenterSelection, centerChosen) comes only at
        // the next update, and not at all for a row selected already. A column with the focus was chosen after
        // anything else, the received items too (they take the focus when chosen).
        // What the focus decided is also kept as the last choice (centerChosen): a right click on a row selected
        // already brings no notification, and the gear button, which takes the focus, must act on the same column
        // after it (the context menu asks here when it is built, LLInvFVBridge::getClipboardEntries).
        if (gFocusMgr.childHasKeyboardFocus(center))
        {
            c->centerChosen = selected;
            if (selected)
            {
                clearOwnInbox(owner);   // chosen after the received items: as a click there would (onCenterSelection)
            }
            return selected ? center : nullptr;
        }
        if (LLInventoryPanel* left = leftOf(*c); left && gFocusMgr.childHasKeyboardFocus(left))
        {
            c->centerChosen = false;
            return nullptr;
        }
        // Neither has it (the gear button, a profile's "Share"): the last choice. Received items chosen after the
        // center (clicking the center clears them: onCenterSelection) are what the official gear menu and sharing
        // look at: as in one column, "Share" is off then.
        if (!c->centerChosen || inboxHasSelection(owner))
        {
            return nullptr;
        }
        return selected ? center : nullptr;
    }

    const std::deque<LLFolderViewItem*>* centerSelection(const LLPanelMainInventory* owner)
    {
        const Columns* c = find(owner);
        LLInventorySingleFolderPanel* center = c && c->active && c->centerChosen ? centerOf(*c) : nullptr;
        LLFolderView* root = center ? center->getRootFolder() : nullptr;
        return root && !root->getSelectedItems().empty() ? &root->getSelectedItems() : nullptr;
    }

    LLInventoryPanel* commandPanel(LLPanelMainInventory* owner)
    {
        LLInventoryPanel* center = chosenCenter(owner);
        return center ? center : owner->getActivePanel();
    }

    void forget(const LLPanelMainInventory* owner)
    {
        sColumns.erase(owner);   // the flagged folder view and the column go with the window
    }
}
}

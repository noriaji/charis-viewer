/**
 * @file rlvplus.cpp
 * @brief The bridges between RLVa (plus/rlv) and other plus features, research/248 3.4: rlv-wear's part is the
 *        inventory guard that answers RLV's folder locks for plus/invguard.
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

#include "rlvhooks.h"

#include "llinventorymodel.h"
#include "llviewerinventory.h"

#include "plus/invguard/invguard.h"

#include "rlvactions.h"
#include "rlvcommon.h"
#include "rlvhandler.h"
#include "rlvlocks.h"

namespace plus::rlv
{

namespace
{
    // Several items moved at once are refused one by one: tell once
    void notifyInvFolderBlocked()
    {
        static F64 sLastNotify = -100.0;
        const F64 now = LLTimer::getTotalSeconds();
        if (now - sLastNotify > 1.0)
        {
            sLastNotify = now;
            RlvUtil::notifyBlocked(RlvStringKeys::Blocked::InvFolder);
        }
    }

    // RLV's folder locks (@attachthis, @detachthis, @sharedwear ...) as an inventory guard (plus/invguard): a locked folder
    // and what is in it cannot be deleted, renamed or moved out; moving between folders under different locks is refused.
    // It does not ask the user anything (asksUser() false), so its refusal comes before a confirmation of another guard.
    // Added when RLVa is switched on and removed again when the program ends (it is never freed: static destruction
    // order must not matter); it only answers while RLVa is on.
    class RlvInvGuard final : public plus::invguard::Guard
    {
    public:
        // FS' hooks check hasLockedFolder() first: without a lock the folder questions are always "yes" and the item
        // questions would refuse items whose folder is unknown (library items)
        static bool locksActive()
        {
            return (RlvHandler::isEnabled()) && (RlvFolderLocks::instance().hasLockedFolder(RLV_LOCK_ANY));
        }

        bool blocksMove(const LLUUID& idFolder, const LLUUID& idNewParent, std::string& strTooltip) override
        {
            if ( (!locksActive()) || (RlvFolderLocks::instance().canMoveFolder(idFolder, idNewParent)) )
                return false;
            if (strTooltip.empty())
                strTooltip = RlvStrings::getString(RlvStringKeys::Blocked::InvFolder);
            return true;
        }

        bool refusesMove(const LLUUID& idFolder, const LLUUID& idNewParent) override
        {
            if ( (!locksActive()) || (RlvFolderLocks::instance().canMoveFolder(idFolder, idNewParent)) )
                return false;
            notifyInvFolderBlocked();
            return true;
        }

        bool blocksRemoval(const LLUUID& idFolder) override
        {
            return (locksActive()) && (!RlvFolderLocks::instance().canRemoveFolder(idFolder));
        }

        bool refusesRemoval(const LLUUID& idFolder) override
        {
            if (!blocksRemoval(idFolder))
                return false;
            notifyInvFolderBlocked();
            return true;
        }

        bool blocksFolderRename(const LLUUID& idFolder) override
        {
            return (locksActive()) && (!RlvFolderLocks::instance().canRenameFolder(idFolder));
        }

        bool blocksItemRemoval(const LLUUID& idItem) override
        {
            return (locksActive()) && (!RlvFolderLocks::instance().canRemoveItem(idItem));
        }

        bool blocksItemMove(const LLUUID& idItem, const LLUUID& idNewParent) override
        {
            return (locksActive()) && (!RlvFolderLocks::instance().canMoveItem(idItem, idNewParent));
        }

        bool blocksItemRename(const LLUUID& idItem) override
        {
            return (locksActive()) && (!RlvFolderLocks::instance().canRenameItem(idItem));
        }

        bool refusesItemMove(const LLUUID& idItem, const LLUUID& idNewParent) override
        {
            if (!blocksItemMove(idItem, idNewParent))
                return false;
            notifyInvFolderBlocked();
            return true;
        }

        bool refusesPaste(const LLUUID& idSource, bool fSourceIsFolder, const LLUUID& idDestFolder) override
        {
            if (!locksActive())
                return false;
            const LLInventoryCategory* pDestCat = gInventory.getCategory(idDestFolder);
            const bool fCanPaste = (fSourceIsFolder)
                ? RlvActions::canPasteInventory(gInventory.getCategory(idSource), pDestCat)
                : RlvActions::canPasteInventory(gInventory.getItem(idSource), pDestCat);
            if (fCanPaste)
                return false;
            notifyInvFolderBlocked();
            return true;
        }
    };

    RlvInvGuard* sInvGuard = nullptr;
}

// Called when RLVa is switched on (STATE_LOGIN_CLEANUP, before the inventory is read): from then on the folder locks guard the inventory.
void enableInvGuard()
{
    if (!sInvGuard)
        sInvGuard = new RlvInvGuard();
    plus::invguard::add(sInvGuard);
}

// Called from plus::rlv::cleanup() (LLAppViewer::cleanup()).
void disableInvGuard()
{
    if (sInvGuard)
        plus::invguard::remove(sInvGuard);
}

} // namespace plus::rlv

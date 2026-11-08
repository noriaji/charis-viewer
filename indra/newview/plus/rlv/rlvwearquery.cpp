/**
 *
 * Copyright (c) 2009-2016, Kitty Barnett
 *
 * The source code in this file is provided to you under the terms of the
 * GNU Lesser General Public License, version 2.1, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 * PARTICULAR PURPOSE. Terms of the LGPL can be found in doc/LGPL-licence.txt
 * in this distribution, or online at http://www.gnu.org/licenses/lgpl-2.1.txt
 *
 * By copying, modifying or distributing this software, you acknowledge that
 * you have read and understood your obligations described above, and agree to
 * abide by those obligations.
 *
 */


#include "llviewerprecompiledheaders.h"

#include "llagentwearables.h"
#include "llgesturemgr.h"
#include "llvoavatarself.h"

#include "rlvcommon.h"      // Charis: rlvforcewear.h uses RlvPredIsEqualOrLinkedItem without including rlvcommon.h
#include "rlvforcewear.h"
#include "rlvinventory.h"

// Charis: the RlvForceWear functions rlv-core needs (@getinvworn, rlvinventory): isWearingItem() and isStrippable().
//         The rest is in rlvforcewear.cpp.
// Checked: 2010-04-05 (RLVa-1.2.0d) | Modified: RLVa-1.2.0d
bool RlvForceWear::isWearingItem(const LLInventoryItem* pItem)
{
    if (pItem)
    {
        switch (pItem->getActualType())
        {
            case LLAssetType::AT_BODYPART:
            case LLAssetType::AT_CLOTHING:
                return gAgentWearables.isWearingItem(pItem->getUUID());
            case LLAssetType::AT_OBJECT:
                return (isAgentAvatarValid()) && (gAgentAvatarp->isWearingAttachment(pItem->getUUID()));
            case LLAssetType::AT_GESTURE:
                return LLGestureMgr::instance().isGestureActive(pItem->getUUID());
            case LLAssetType::AT_LINK:
                return isWearingItem(gInventory.getItem(pItem->getLinkedUUID()));
            default:
                break;
        }
    }
    return false;
}

// Checked: 2010-03-19 (RLVa-1.2.0c) | Modified: RLVa-1.2.0a
bool RlvForceWear::isStrippable(const LLInventoryItem* pItem)
{
    // An item is exempt from @detach or @remoutfit if:
    //   - its name contains "nostrip" (anywhere in the name)
    //   - its parent folder contains "nostrip" (anywhere in the name)
    if (pItem)
    {
        // If the item is an inventory link then we first examine its target before examining the link itself (and never its name)
        if (LLAssetType::AT_LINK == pItem->getActualType())
        {
            if (!isStrippable(pItem->getLinkedUUID()))
                return false;
        }
        else
        {
            if (std::string::npos != pItem->getName().find(RLV_FOLDER_FLAG_NOSTRIP))
                return false;
        }

        LLViewerInventoryCategory* pFolder = gInventory.getCategory(pItem->getParentUUID());
        // Charis: bounded walk up the folded parents (a cyclic parent chain in a damaged model would otherwise never end)
        for (S32 nDepth = 0; (pFolder) && (nDepth < RlvInventory::cMaxFolderDepth); nDepth++)
        {
            if (std::string::npos != pFolder->getName().find(RLV_FOLDER_FLAG_NOSTRIP))
                return false;
            // If the item's parent is a folded folder then we need to check its parent as well
            if ( (gInventory.getRootFolderID() != pFolder->getParentUUID()) && (RlvInventory::isFoldedFolder(pFolder, true)) )
                pFolder = gInventory.getCategory(pFolder->getParentUUID());
            else
                pFolder = NULL;
        }
    }
    return true;
}

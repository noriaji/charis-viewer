/**
 * @file rlvhooks_wear.cpp
 * @brief rlv-wear's part of the calls the official viewer files make into RLVa (plus/rlv), research/248
 *        (W5-W22, W25, W26, E49): attachment / outfit / inventory locks and the wear menus.
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

#include <algorithm>
#include <set>

#include "llagentwearables.h"
#include "llappearancemgr.h"
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "llselectmgr.h"
#include "llviewerinventory.h"
#include "llviewerjointattachment.h"
#include "llviewerobject.h"
#include "llvoavatarself.h"

#include "rlvactions.h"
#include "rlvattachpt.h"
#include "rlvcommon.h"
#include "rlvforcewear.h"
#include "rlvhandler.h"
#include "rlvhelper.h"
#include "rlvinventory.h"
#include "rlvlocks.h"

namespace plus::rlv
{

// Every function here runs only with RLVa on (the official callers check enabled() first) and on the main thread.
// They are the FS blocks ([RLVa:KB]) of the official files, moved here as they are; where Charis' official function is
// shaped differently the difference is in research/248 section 12 and plus/rlv/ORIGIN.md.

namespace
{
    // FS: LLAgentWearables::areInitialAttachmentsRequested(), set at the end of userAttachMultipleAttachments()
    bool sInitialAttachmentsRequested = false;

    const LLViewerInventoryItem* asViewerItem(const LLInventoryItem* pItem)
    {
        return dynamic_cast<const LLViewerInventoryItem*>(pItem);
    }

    // Some lock of the kind applies to this asset type: an attachment point for attachments, a layer for wearables.
    bool hasLocksFor(int eAssetType)
    {
        return (LLAssetType::AT_OBJECT == eAssetType)
            ? (gRlvAttachmentLocks.hasLockedAttachmentPoint(RLV_LOCK_ANY))
            : (gRlvWearableLocks.hasLockedWearableType(RLV_LOCK_ANY));
    }

    // What the wear locks leave of "wear" (replace) and "add" for the item (FS: gRlvWearableLocks / gRlvAttachmentLocks).
    ERlvWearMask wearMaskOf(const LLInventoryItem* pItem)
    {
        if (pItem)
        {
            switch (pItem->getType())
            {
                case LLAssetType::AT_BODYPART:
                case LLAssetType::AT_CLOTHING:
                    return gRlvWearableLocks.canWear(asViewerItem(pItem));
                case LLAssetType::AT_OBJECT:
                    return gRlvAttachmentLocks.canAttach(pItem);
                default:
                    break;
            }
        }
        return RLV_WEAR_LOCKED;
    }
}

// ================================================================ attachments

bool allowAttachmentRequest(const LLUUID& idItem, bool fAdd, unsigned char& nAttachPt, bool& fAddOut)
{
    // The lock watchdog's own requests skip the locks (FS: fRlvForce); so does everything before the initial attachments
    if ( (isForcedAttachRequest()) || (!sInitialAttachmentsRequested) || (!gRlvAttachmentLocks.hasLockedAttachmentPoint(RLV_LOCK_ANY)) )
        return true;

    const LLInventoryItem* pItem = gInventory.getItem(idItem);
    if (!pItem)
        return false;

    LLViewerJointAttachment* pAttachPt = NULL;
    ERlvWearMask eWearMask = gRlvAttachmentLocks.canAttach(pItem, &pAttachPt);
    if ( ((fAdd) && ((RLV_WEAR_ADD & eWearMask) == 0)) || ((!fAdd) && ((RLV_WEAR_REPLACE & eWearMask) == 0)) )
        return false;

    if ( (0 == nAttachPt) && (NULL != pAttachPt) )
    {
        const S32 idxAttachPt = RlvAttachPtLookup::getAttachPointIndex(pAttachPt);
        if ( (idxAttachPt > 0) && (idxAttachPt <= 255) )
            nAttachPt = (unsigned char)idxAttachPt;
    }
    RlvAttachmentLockWatchdog::instance().onWearAttachment(pItem, (fAdd) ? RLV_WEAR_ADD : RLV_WEAR_REPLACE);
    fAddOut = true;
    return true;
}

void onInitialAttachmentsRequested()
{
    sInitialAttachmentsRequested = true;
}

void removeLockedAttachments(std::vector<LLViewerObject*>& objects)
{
    if (!gRlvAttachmentLocks.hasLockedAttachmentPoint(RLV_LOCK_REMOVE))
        return;

    std::vector<LLViewerObject*>::iterator itObj = objects.begin();
    while (objects.end() != itObj)
    {
        const LLViewerObject* pAttachObj = *itObj;
        if ( (pAttachObj) && (gRlvAttachmentLocks.isLockedAttachment(pAttachObj)) )
        {
            itObj = objects.erase(itObj);

            // Fall-back code: link the attachment into the COF again if it got removed from there somehow (compensates for possible bugs elsewhere)
            const LLUUID idItem = pAttachObj->getAttachmentItemID();
            const bool fInCOF = LLAppearanceMgr::instance().isLinkedInCOF(idItem);
            RLV_ASSERT(fInCOF);
            if (!fInCOF)
            {
                LLAppearanceMgr::instance().registerAttachment(idItem);
            }
        }
        else
        {
            ++itObj;
        }
    }
}

// ================================================================ selections and objects

bool selectionHasLockedAttachment(bool fRootsOnly)
{
    if (!gRlvAttachmentLocks.hasLockedAttachmentPoint(RLV_LOCK_REMOVE))
        return false;

    LLObjectSelectionHandle hSelect = LLSelectMgr::getInstance()->getSelection();
    RlvSelectHasLockedAttach f;
    if (!hSelect->isAttachment())
        return false;
    return ((fRootsOnly) ? hSelect->getFirstRootNode(&f, false) : hSelect->getFirstNode(&f)) != NULL;
}

bool isLockedAttachment(const LLViewerObject* pObj)
{
    return (pObj) && (gRlvAttachmentLocks.isLockedAttachment(pObj));
}

bool isInLockedAttachment(const LLViewerObject* pObj)
{
    return (pObj) && (gRlvAttachmentLocks.isLockedAttachment(pObj->getRootEdit()));
}

bool refusesViewLockedAttachment(const LLViewerObject* pObj, int eAssetType)
{
    if (!isInLockedAttachment(pObj))
        return false;
    RlvUtil::notifyBlockedViewXXX((LLAssetType::EType)eAssetType);
    return true;
}

bool refusesChangeLockedAttachment(const LLViewerObject* pObj)
{
    if (!isInLockedAttachment(pObj))
        return false;
    RlvUtil::notifyBlockedGeneric();
    return true;
}

// ================================================================ the appearance manager

bool canWearItem(const LLInventoryItem* pItem, bool fReplace)
{
    return rlvPredCanWearItem(asViewerItem(pItem), (fReplace) ? RLV_WEAR_REPLACE : RLV_WEAR_ADD);
}

bool canRemoveItem(const LLUUID& idItem)
{
    return rlvPredCanRemoveItem(idItem);
}

bool isFolderLocked(const LLUUID& idFolder, bool fRemove)
{
    return RlvFolderLocks::instance().isLockedFolder(idFolder, (fRemove) ? RLV_LOCK_REMOVE : RLV_LOCK_ADD);
}

void dropUnwearableItems(std::vector<LLPointer<LLViewerInventoryItem> >& items, size_t nFirstNew, int eAssetType)
{
    // Filter out any new body parts / wearables / attachments that can't be worn before adding them (body parts replace)
    if ( ((!hasLocksFor(eAssetType)) && (!RlvFolderLocks::instance().hasLockedFolder(RLV_LOCK_ADD))) || (nFirstNew >= items.size()) )
        return;

    const ERlvWearMask eWearMask = (LLAssetType::AT_BODYPART == eAssetType) ? RLV_WEAR_REPLACE : RLV_WEAR;
    items.erase(std::remove_if(items.begin() + nFirstNew, items.end(), RlvPredCanNotWearItem(eWearMask)), items.end());
}

void keepLockedCofItems(std::vector<LLPointer<LLViewerInventoryItem> >& items, int eAssetType)
{
    // Make sure that all currently locked clothing layers / attachments remain in COF when replacing
    if ( (!hasLocksFor(eAssetType)) && (!RlvFolderLocks::instance().hasLockedFolder(RLV_LOCK_REMOVE)) )
        return;

    LLInventoryModel::cat_array_t cats;
    LLIsType isOfType((LLAssetType::EType)eAssetType);
    gInventory.collectDescendentsIf(LLAppearanceMgr::instance().getCOF(), cats, items, LLInventoryModel::EXCLUDE_TRASH, isOfType);
    items.erase(std::remove_if(items.begin(), items.end(), RlvPredCanRemoveItem()), items.end());
}

void notifyWearableChanges(const std::vector<LLPointer<LLInventoryItem> >& items)
{
    // The initial wearables are not reported (FS: gAgentWearables.areInitalWearablesLoaded())
    if (!gAgentWearables.areWearablesLoaded())
        return;

    // The item ids of all currently worn wearables
    uuid_vec_t idsCurrent;
    for (S32 idxType = 0; idxType < LLWearableType::WT_COUNT; idxType++)
    {
        const LLWearableType::EType eType = (LLWearableType::EType)idxType;
        const U32 cntWearable = gAgentWearables.getWearableCount(eType);
        for (U32 idxWearable = 0; idxWearable < cntWearable; idxWearable++)
        {
            const LLUUID idItem = gAgentWearables.getWearableItemID(eType, idxWearable);
            if (idItem.notNull())
                idsCurrent.push_back(idItem);
        }
    }

    // Remove the wearable from the current item ids if currently worn and requested, otherwise mark it as a new item
    std::vector<const LLInventoryItem*> itemsNew;
    for (const LLPointer<LLInventoryItem>& pItem : items)
    {
        if (pItem.isNull())
            continue;
        uuid_vec_t::iterator itItemID = std::find(idsCurrent.begin(), idsCurrent.end(), pItem->getUUID());
        if (idsCurrent.end() != itItemID)
            idsCurrent.erase(itItemID);
        else
            itemsNew.push_back(pItem.get());
    }

    // We need to report removals before additions or scripts will get confused
    for (const LLUUID& idItem : idsCurrent)
    {
        const LLWearable* pWearable = gAgentWearables.getWearableFromItemID(idItem);
        if (pWearable)
            RlvBehaviourNotifyHandler::onTakeOff(pWearable->getType(), true);
    }
    for (const LLInventoryItem* pItem : itemsNew)
    {
        const LLViewerInventoryItem* pViewerItem = asViewerItem(pItem);
        if (pViewerItem)
            RlvBehaviourNotifyHandler::onWear(pViewerItem->getWearableType(), true);
    }
}

void dropLockedFromDeletion(std::vector<LLUUID>& worn, std::vector<LLUUID>& itemsToDelete, std::vector<LLUUID>& foldersToDelete)
{
    std::vector<LLUUID> idsLocked;
    for (const LLUUID& idItem : worn)
    {
        if (!rlvPredCanRemoveItem(idItem))
            idsLocked.push_back(idItem);
    }
    if (idsLocked.empty())
        return;

    const auto isLocked = [&idsLocked](const LLUUID& idItem) { return std::find(idsLocked.begin(), idsLocked.end(), idItem) != idsLocked.end(); };
    worn.erase(std::remove_if(worn.begin(), worn.end(), isLocked), worn.end());
    itemsToDelete.erase(std::remove_if(itemsToDelete.begin(), itemsToDelete.end(), isLocked), itemsToDelete.end());
    foldersToDelete.erase(std::remove_if(foldersToDelete.begin(), foldersToDelete.end(),
        [&idsLocked](const LLUUID& idFolder)
        {
            for (const LLUUID& idItem : idsLocked)
            {
                if (gInventory.isObjectDescendentOf(idItem, idFolder))
                    return true;
            }
            return false;
        }), foldersToDelete.end());
    RlvUtil::notifyBlockedGeneric();
}

bool refusesUngroup(const LLUUID& idFolder, const LLUUID& idDest)
{
    if (!RlvFolderLocks::instance().hasLockedFolder(RLV_LOCK_ANY))
        return false;
    LLInventoryModel::cat_array_t* pCats = nullptr;
    LLInventoryModel::item_array_t* pItems = nullptr;
    gInventory.getDirectDescendentsOf(idFolder, pCats, pItems);
    bool fRefused = false;
    if (pCats)
    {
        for (const LLPointer<LLViewerInventoryCategory>& pCat : *pCats)
            fRefused = fRefused || ((pCat) && (!RlvFolderLocks::instance().canMoveFolder(pCat->getUUID(), idDest)));
    }
    if (pItems)
    {
        for (const LLPointer<LLViewerInventoryItem>& pItem : *pItems)
            fRefused = fRefused || ((pItem) && (!RlvFolderLocks::instance().canMoveItem(pItem->getUUID(), idDest)));
    }
    if (fRefused)
        RlvUtil::notifyBlocked(RlvStringKeys::Blocked::InvFolder);
    return fRefused;
}

void filterOutfitRestore(LLSD& sdContents, int& nKept, int& nDropped)
{
    nKept = nDropped = 0;
    if ( (!hasWearLocks()) && (!RlvFolderLocks::instance().hasLockedFolder(RLV_LOCK_ANY)) )
        return;

    // What the COF holds now
    LLInventoryModel::cat_array_t cats;
    LLInventoryModel::item_array_t itemsCOF;
    gInventory.collectDescendents(LLAppearanceMgr::instance().getCOF(), cats, itemsCOF, LLInventoryModel::EXCLUDE_TRASH);
    std::set<LLUUID> idsCurrent;
    for (const LLPointer<LLViewerInventoryItem>& pLink : itemsCOF)
    {
        if ( (pLink) && (pLink->getIsLinkType()) )
            idsCurrent.insert(pLink->getLinkedUUID());
    }

    // Items that are not worn yet need to be wearable (updateCOF(): new body parts replace, the rest wears)
    LLSD sdFiltered = LLSD::emptyArray();
    std::set<LLUUID> idsKept;
    for (LLSD::array_const_iterator itEntry = sdContents.beginArray(); itEntry != sdContents.endArray(); ++itEntry)
    {
        const LLUUID idLinked = (*itEntry)["linked_id"].asUUID();
        if ( (LLAssetType::AT_LINK == (*itEntry)["type"].asInteger()) && (idsCurrent.end() == idsCurrent.find(idLinked)) )
        {
            const LLViewerInventoryItem* pItem = gInventory.getItem(idLinked);
            if ( (pItem) && (!rlvPredCanWearItem(pItem, (LLAssetType::AT_BODYPART == pItem->getType()) ? RLV_WEAR_REPLACE : RLV_WEAR)) )
            {
                nDropped++;
                continue;
            }
        }
        idsKept.insert(idLinked);
        sdFiltered.append(*itEntry);
    }

    // What is locked on stays, whatever the copy says
    for (const LLPointer<LLViewerInventoryItem>& pLink : itemsCOF)
    {
        if ( (!pLink) || (!pLink->getIsLinkType()) || (idsKept.end() != idsKept.find(pLink->getLinkedUUID())) )
            continue;
        const LLAssetType::EType eType = pLink->getType();       // the type of what the link points to
        if ( ((LLAssetType::AT_CLOTHING == eType) || (LLAssetType::AT_OBJECT == eType)) && (!rlvPredCanRemoveItem(pLink)) )
        {
            LLSD sdEntry;
            sdEntry["name"]      = pLink->getName();
            sdEntry["desc"]      = pLink->getActualDescription();
            sdEntry["linked_id"] = pLink->getLinkedUUID();
            sdEntry["type"]      = (S32)LLAssetType::AT_LINK;
            sdFiltered.append(sdEntry);
            nKept++;
        }
    }
    // A body part type the filtering left without one stays as it is now (a shape, skin, hair and eyes are always worn:
    // the outfit backup added the current ones the copy lacked, but a copy's part dropped above must not leave a gap)
    std::set<S32> typesBody;
    for (LLSD::array_const_iterator itEntry = sdFiltered.beginArray(); itEntry != sdFiltered.endArray(); ++itEntry)
    {
        const LLViewerInventoryItem* pItem = gInventory.getItem((*itEntry)["linked_id"].asUUID());
        if ( (pItem) && (LLAssetType::AT_BODYPART == pItem->getType()) )
            typesBody.insert((S32)pItem->getWearableType());
    }
    for (const LLPointer<LLViewerInventoryItem>& pLink : itemsCOF)
    {
        if ( (!pLink) || (!pLink->getIsLinkType()) || (LLAssetType::AT_BODYPART != pLink->getType()) )
            continue;
        if (typesBody.insert((S32)pLink->getWearableType()).second)
        {
            LLSD sdEntry;
            sdEntry["name"]      = pLink->getName();
            sdEntry["desc"]      = pLink->getActualDescription();
            sdEntry["linked_id"] = pLink->getLinkedUUID();
            sdEntry["type"]      = (S32)LLAssetType::AT_LINK;
            sdFiltered.append(sdEntry);
            nKept++;
        }
    }
    sdContents = sdFiltered;
}

// ================================================================ menus and buttons

bool canViewWireframe()
{
    if (gRlvAttachmentLocks.hasLockedHUD())
    {
        RlvUtil::notifyBlocked(RlvStringKeys::Blocked::Wireframe);
        return false;
    }
    return true;
}

bool hasLockedHUD()
{
    return gRlvAttachmentLocks.hasLockedHUD();
}

float hudZoomMin()
{
    return (!gRlvAttachmentLocks.hasLockedHUD()) ? 0.1f : 0.85f;
}

bool hasRemoveLockedAttachmentPoint()
{
    return gRlvAttachmentLocks.hasLockedAttachmentPoint(RLV_LOCK_REMOVE);
}

bool hasWearLocks()
{
    return (gRlvAttachmentLocks.hasLockedAttachmentPoint(RLV_LOCK_ANY)) || (gRlvWearableLocks.hasLockedWearableType(RLV_LOCK_ANY));
}

bool canDetachFromPoint(const LLViewerJointAttachment* pAttachPt)
{
    return gRlvAttachmentLocks.canDetach(pAttachPt);
}

bool canAddToAttachmentPoint(const LLViewerJointAttachment* pAttachPt)
{
    return !gRlvAttachmentLocks.isLockedAttachmentPoint(pAttachPt, RLV_LOCK_ADD);
}

bool isAttachmentPointAddLocked(int idxAttachPt)
{
    return gRlvAttachmentLocks.isLockedAttachmentPoint((S32)idxAttachPt, RLV_LOCK_ADD);
}

bool canReplaceAttachmentOn(const LLViewerJointAttachment* pAttachPt)
{
    return (gRlvAttachmentLocks.canAttach(pAttachPt) & RLV_WEAR_REPLACE) != 0;
}

bool canAttachSelectionTo(int nIndex, const LLViewerJointAttachment* pAttachPt)
{
    // If 'nIndex != 0' then the object will be "add attached" [see LLSelectMgr::sendAttach()]
    if (!isAgentAvatarValid())
        return false;
    return !( ((!nIndex) && (gRlvAttachmentLocks.hasLockedAttachmentPoint(RLV_LOCK_ANY))) ||     // Can't wear on default attach point
              ((nIndex) && ((RLV_WEAR_ADD & gRlvAttachmentLocks.canAttach(pAttachPt)) == 0)) );  // or non-attachable attach point
}

bool allowRezAttachment(const LLViewerInventoryItem* pItem, LLViewerJointAttachment*& pAttachPt, bool fReplace)
{
    // If no attachment point was specified, try looking it up from the item name
    if ( (!RlvSettings::getDebugDeprecateExplicitPoint()) && (!pAttachPt) && (gRlvAttachmentLocks.hasLockedAttachmentPoint(RLV_LOCK_ANY)) )
        pAttachPt = RlvAttachPtLookup::getAttachPoint(pItem);
    return rlvPredCanWearItem(pItem, (fReplace) ? RLV_WEAR_REPLACE : RLV_WEAR_ADD);
}

void wearMenuState(const LLInventoryItem* pItem, bool& fCanReplace, bool& fCanAdd)
{
    const ERlvWearMask eWearMask = wearMaskOf(pItem);
    fCanReplace = ((eWearMask & RLV_WEAR_REPLACE) == RLV_WEAR_REPLACE);
    fCanAdd = ((eWearMask & RLV_WEAR_ADD) == RLV_WEAR_ADD);
}

void accumulateItemMenuState(const LLInventoryItem* pItem, bool fIsWorn, bool& fCanReplace, bool& fCanAdd, bool& fCanRemove)
{
    const ERlvWearMask eWearMask = wearMaskOf(pItem);
    switch (pItem->getType())
    {
        case LLAssetType::AT_BODYPART:
        case LLAssetType::AT_CLOTHING:
            fCanRemove |= (fIsWorn) ? gRlvWearableLocks.canRemove(pItem) : false;
            break;
        case LLAssetType::AT_OBJECT:
            fCanRemove |= (fIsWorn) ? gRlvAttachmentLocks.canDetach(pItem) : false;
            break;
        default:
            break;
    }
    fCanReplace |= ((eWearMask & RLV_WEAR_REPLACE) == RLV_WEAR_REPLACE);
    fCanAdd |= ((eWearMask & RLV_WEAR_ADD) == RLV_WEAR_ADD);
}

bool canRemoveWearable(const LLInventoryItem* pItem)
{
    return gRlvWearableLocks.canRemove(pItem);
}

bool canRemoveWearableType(int eWearableType)
{
    // Enabled if there is at least one wearable of this type that can be removed
    return gRlvWearableLocks.canRemove((LLWearableType::EType)eWearableType);
}

bool topUnlockedWearable(int eWearableType, unsigned int& nIndex)
{
    const LLWearableType::EType eType = (LLWearableType::EType)eWearableType;
    if (!gRlvWearableLocks.hasLockedWearable(eType))
        return true;

    // We'll use the first wearable we come across that can be removed (moving from top to bottom)
    S32 idxWearable = (S32)nIndex;
    for (; idxWearable >= 0; idxWearable--)
    {
        const LLViewerWearable* pWearable = gAgentWearables.getViewerWearable(eType, (U32)idxWearable);
        if (!gRlvWearableLocks.isLockedWearable(pWearable))
            break;
    }
    if (idxWearable < 0)
        return false;       // No wearable found that can be removed
    nIndex = (unsigned int)idxWearable;
    return true;
}

// ================================================================ inventory

bool isSharedRootFolder(const std::string& strName)
{
    return RLV_ROOT_FOLDER == strName;
}

void keepGivenFolderName(LLViewerInventoryCategory* pNewFolder, const LLViewerInventoryCategory* pOldFolder)
{
    // If we rename the folder on the first BulkUpdateInventory message, the later ones still carry the old name and
    // updateCategory() would undo the rename viewer-side only; so only #RLV/~ folders keep the name they have here
    if ( (pNewFolder) && (pOldFolder) && (!pOldFolder->getName().empty()) && (pNewFolder->getName() != pOldFolder->getName()) &&
         (pNewFolder->getName().find(RLV_PUTINV_PREFIX) == 0) )
    {
        pNewFolder->rename(pOldFolder->getName());
    }
}

// ================================================================ names and places in the world menu (E49)

bool canShowMenuName(const LLUUID& idAgent)
{
    return RlvActions::canShowName(RlvActions::SNC_DEFAULT, idAgent);
}

bool canShowMenuLocation()
{
    return RlvActions::canShowLocation();
}

bool canTeleportHomeMenu()
{
    return (!RlvActions::hasBehaviour(RLV_BHVR_TPLM)) && (!RlvActions::hasBehaviour(RLV_BHVR_TPLOC));
}

} // namespace plus::rlv

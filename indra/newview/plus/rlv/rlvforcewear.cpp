/**
 *
 * Copyright (c) 2009-2020, Kitty Barnett
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
#include "llagent.h"
#include "llappearancemgr.h"
#include "llattachmentsmgr.h"
#include "llgesturemgr.h"
#include "llnotificationsutil.h"
#include "llviewerobjectlist.h"

#include "rlvcommon.h"
#include "rlvforcewear.h"
#include "rlvhandler.h"
#include "rlvhelper.h"
#include "rlvinventory.h"
#include "rlvlocks.h"

// Charis: RlvForceWear of FS rlvhelper.cpp. isWearingItem() and isStrippable() are in rlvwearquery.cpp (rlv-core, they
//         don't depend on the locks). The LSL bridge exemption (FS FIRE-4453) is dropped: Charis has no LSL bridge.
//         done() is rewritten for the Charis LLAppearanceMgr (see the note there).

// ============================================================================
// RlvForceWear
//
// Checked: 2010-03-21 (RLVa-1.2.0a) | Modified: RLVa-1.2.0a
void RlvForceWear::forceFolder(const LLViewerInventoryCategory* pFolder, EWearAction eAction, EWearFlags eFlags)
{
    // [See LLWearableBridge::wearOnAvatar(): don't wear anything until initial wearables are loaded, can destroy clothing items]
    if (!gAgentWearables.areWearablesLoaded())
    {
        // Charis: a script can send this every few milliseconds - the modal alert is shown at most once a minute
        static F64 tsLastAlert = -60.0;
        if (LLFrameTimer::getElapsedSeconds() - tsLastAlert >= 60.0)
        {
            tsLastAlert = LLFrameTimer::getElapsedSeconds();
            LLNotificationsUtil::add("CanNotChangeAppearanceUntilLoaded");
        }
        return;
    }
    if (!isAgentAvatarValid())
        return;

    // Grab a list of all the items we'll be wearing/attaching
    LLInventoryModel::cat_array_t folders; LLInventoryModel::item_array_t items;
    RlvWearableItemCollector f(pFolder, eAction, eFlags);
    RlvInventory::collectDescendentsIf(pFolder->getUUID(), folders, items, false, f, true);  // Charis: the official one has no follow_folder_links (depth-limited, rlvinventory.cpp)

    // true if we've already encountered this LLWearableType::EType (used only on wear actions and only for AT_CLOTHING)
    bool fSeenWType[LLWearableType::WT_COUNT] = { false };

    EWearAction eCurAction = eAction;
    for (size_t idxItem = 0, cntItem = items.size(); idxItem < cntItem; idxItem++)
    {
        LLViewerInventoryItem* pRlvItem = items.at(idxItem);
        LLViewerInventoryItem* pItem = (LLAssetType::AT_LINK == pRlvItem->getActualType()) ? pRlvItem->getLinkedItem() : pRlvItem;
        if (!pItem)
            continue;       // Charis: a link whose target isn't in the inventory (yet)

        // If it's wearable it should be worn on detach
//      if ( (ACTION_DETACH == eAction) && (isWearableItem(pItem)) && (!isWearingItem(pItem)) )
//          continue;

        // Each folder can specify its own EWearAction override
        if (isWearAction(eAction))
            eCurAction = f.getWearAction(pRlvItem->getParentUUID());
        else
            eCurAction = eAction;

        //  NOTES: * if there are composite items then RlvWearableItemCollector made sure they can be worn (or taken off depending)
        //         * some scripts issue @remattach=force,attach:worn-items=force so we need to attach items even if they're currently worn
        switch (pItem->getType())
        {
            case LLAssetType::AT_BODYPART:
                RLV_ASSERT(isWearAction(eAction));  // RlvWearableItemCollector shouldn't be supplying us with body parts on detach
            case LLAssetType::AT_CLOTHING:
                // Charis: the layer is 0-255 from the item's flags; fSeenWType and the wearable lookups only take known layers
                if (static_cast<S32>(pItem->getWearableType()) >= LLWearableType::WT_COUNT)
                    break;
                if (isWearAction(eAction))
                {
                    // The first time we encounter any given clothing type we use 'eCurAction' (replace or add)
                    // The second time we encounter a given clothing type we'll always add (rather than replace the previous iteration)
                    eCurAction = (!fSeenWType[pItem->getWearableType()]) ? eCurAction : ACTION_WEAR_ADD;

                    ERlvWearMask eWearMask = gRlvWearableLocks.canWear(pRlvItem);
                    if ( ((ACTION_WEAR_REPLACE == eCurAction) && (eWearMask & RLV_WEAR_REPLACE)) ||
                         ((ACTION_WEAR_ADD == eCurAction) && (eWearMask & RLV_WEAR_ADD)) )
                    {
                        // The check for whether we're replacing a currently worn composite item happens in onWearableArrived()
                        if (!isAddWearable(pItem))
                            addWearable(pRlvItem, eCurAction);
                        fSeenWType[pItem->getWearableType()] = true;
                    }
                }
                else
                {
                    const LLViewerWearable* pWearable = gAgentWearables.getWearableFromItemID(pItem->getUUID());
                    if ( (pWearable) && (isForceRemovable(pWearable, false)) )
                        remWearable(pWearable);
                }
                break;

            case LLAssetType::AT_OBJECT:
                if (isWearAction(eAction))
                {
                    ERlvWearMask eWearMask = gRlvAttachmentLocks.canAttach(pRlvItem);
                    if ( ((ACTION_WEAR_REPLACE == eCurAction) && (eWearMask & RLV_WEAR_REPLACE)) ||
                         ((ACTION_WEAR_ADD == eCurAction) && (eWearMask & RLV_WEAR_ADD)) )
                    {
                        if (!isAddAttachment(pRlvItem))
                        {
                            #ifdef RLV_EXPERIMENTAL_COMPOSITEFOLDERS
                            // We still need to check whether we're about to replace a currently worn composite item
                            // (which we're not if we're just reattaching an attachment we're already wearing)
                            LLViewerInventoryCategory* pCompositeFolder = NULL;
                            if ( (pAttachPt->getObject()) && (RlvSettings::getEnableComposites()) &&
                                 (pAttachPt->getItemID() != pItem->getUUID()) &&
                                 (gRlvHandler.getCompositeInfo(pAttachPt->getItemID(), NULL, &pCompositeFolder)) )
                            {
                                // If we can't take off the composite folder this item would replace then don't allow it to get attached
                                if (gRlvHandler.canTakeOffComposite(pCompositeFolder))
                                {
                                    forceFolder(pCompositeFolder, ACTION_DETACH, FLAG_DEFAULT);
                                    addAttachment(pRlvItem);
                                }
                            }
                            else
                            #endif // RLV_EXPERIMENTAL_COMPOSITEFOLDERS
                            {
                                addAttachment(pRlvItem, eCurAction);
                            }
                        }
                    }
                }
                else
                {
                    const LLViewerObject* pAttachObj = gAgentAvatarp->getWornAttachment(pItem->getUUID());
                    if ( (pAttachObj) && (isForceDetachable(pAttachObj, false)) )
                        remAttachment(pAttachObj);
                }
                break;

            case LLAssetType::AT_GESTURE:
                if (isWearAction(eAction))
                {
                    if (std::find_if(m_addGestures.begin(), m_addGestures.end(), RlvPredIsEqualOrLinkedItem(pRlvItem)) == m_addGestures.end())
                        m_addGestures.push_back(pRlvItem);
                }
                else
                {
                    if (std::find_if(m_remGestures.begin(), m_remGestures.end(), RlvPredIsEqualOrLinkedItem(pRlvItem)) == m_remGestures.end())
                        m_remGestures.push_back(pRlvItem);
                }
                break;

            default:
                break;
        }
    }
}

// Checked: 2010-03-19 (RLVa-1.2.0a) | Modified: RLVa-1.2.0a
bool RlvForceWear::isForceDetachable(const LLViewerObject* pAttachObj, bool fCheckComposite /*=true*/, const LLUUID& idExcept /*=LLUUID::null*/)
{
    // Attachment can be detached by an RLV command if:
    //   - it's not "remove locked" by anything (or anything except the object specified by pExceptObj)
    //   - it's strippable
    //   - composite folders are disabled *or* it isn't part of a composite folder that has at least one item locked
    #ifdef RLV_EXPERIMENTAL_COMPOSITEFOLDERS
    LLViewerInventoryCategory* pFolder = NULL;
    #endif // RLV_EXPERIMENTAL_COMPOSITEFOLDERS
    return
      (
        (pAttachObj) && (pAttachObj->isAttachment())
        && ( (idExcept.isNull()) ? (!gRlvAttachmentLocks.isLockedAttachment(pAttachObj))
                                 : (!gRlvAttachmentLocks.isLockedAttachmentExcept(pAttachObj, idExcept)) )
        && (isStrippable(pAttachObj->getAttachmentItemID()))
        #ifdef RLV_EXPERIMENTAL_COMPOSITEFOLDERS
        && ( (!fCheckComposite) || (!RlvSettings::getEnableComposites()) ||
             (!gRlvHandler.getCompositeInfo(pAttachPt->getItemID(), NULL, &pFolder)) || (gRlvHandler.canTakeOffComposite(pFolder)) )
        #endif // RLV_EXPERIMENTAL_COMPOSITEFOLDERS
      );
}

// Checked: 2010-03-19 (RLVa-1.2.0a) | Added: RLVa-1.2.0a
bool RlvForceWear::isForceDetachable(const LLViewerJointAttachment* pAttachPt, bool fCheckComposite /*=true*/, const LLUUID& idExcept /*=LLUUID::null*/)
{
    // Attachment point can be detached by an RLV command if there's at least one attachment that can be removed
    for (LLViewerJointAttachment::attachedobjs_vec_t::const_iterator itAttachObj = pAttachPt->mAttachedObjects.begin();
            itAttachObj != pAttachPt->mAttachedObjects.end(); ++itAttachObj)
    {
        if (isForceDetachable(*itAttachObj, fCheckComposite, idExcept))
            return true;
    }
    return false;
}

// Checked: 2010-03-19 (RLVa-1.2.0a) | Added: RLVa-1.1.0i
void RlvForceWear::forceDetach(const LLViewerObject* pAttachObj)
{
    // Sanity check - no need to process duplicate removes
    if ( (!pAttachObj) || (isRemAttachment(pAttachObj)) )
        return;

    if (isForceDetachable(pAttachObj))
    {
        #ifdef RLV_EXPERIMENTAL_COMPOSITEFOLDERS
        LLViewerInventoryCategory* pFolder = NULL;
        if ( (RlvSettings::getEnableComposites()) &&
             (gRlvHandler.getCompositeInfo(pAttachPt->getItemID(), NULL, &pFolder)) )
        {
            // Attachment belongs to a composite folder so detach the entire folder (if we can take it off)
            if (gRlvHandler.canTakeOffComposite(pFolder))
                forceFolder(pFolder, ACTION_DETACH, FLAG_DEFAULT);
        }
        else
        #endif // RLV_EXPERIMENTAL_COMPOSITEFOLDERS
        {
            remAttachment(pAttachObj);
        }
    }
}

// Checked: 2010-03-19 (RLVa-1.2.0a) | Added: RLVa-1.2.0a
void RlvForceWear::forceDetach(const LLViewerJointAttachment* pAttachPt)
{
    for (LLViewerJointAttachment::attachedobjs_vec_t::const_iterator itAttachObj = pAttachPt->mAttachedObjects.begin();
            itAttachObj != pAttachPt->mAttachedObjects.end(); ++itAttachObj)
    {
        forceDetach(*itAttachObj);
    }
}

// Checked: 2010-03-19 (RLVa-1.2.0c) | Modified: RLVa-1.2.0a
bool RlvForceWear::isForceRemovable(const LLViewerWearable* pWearable, bool fCheckComposite /*=true*/, const LLUUID& idExcept /*=LLUUID::null*/)
{
    // Wearable can be removed by an RLV command if:
    //   - its asset type is AT_CLOTHING
    //   - it's not "remove locked" by anything (or anything except the object specified by idExcept)
    //   - it's strippable
    //   - composite folders are disabled *or* it isn't part of a composite folder that has at least one item locked
    #ifdef RLV_EXPERIMENTAL_COMPOSITEFOLDERS
    LLViewerInventoryCategory* pFolder = NULL;
    #endif // RLV_EXPERIMENTAL_COMPOSITEFOLDERS
    return
      (
        (pWearable) && (LLAssetType::AT_CLOTHING == pWearable->getAssetType())
        && ( (idExcept.isNull()) ? !gRlvWearableLocks.isLockedWearable(pWearable)
                                 : !gRlvWearableLocks.isLockedWearableExcept(pWearable, idExcept) )
        && (isStrippable(pWearable->getItemID()))
        #ifdef RLV_EXPERIMENTAL_COMPOSITEFOLDERS
        && ( (!fCheckComposite) || (!RlvSettings::getEnableComposites()) ||
             (!gRlvHandler.getCompositeInfo(pWearable->getItemID(), NULL, &pFolder)) || (gRlvHandler.canTakeOffComposite(pFolder)) )
        #endif // RLV_EXPERIMENTAL_COMPOSITEFOLDERS
      );
}

// Checked: 2010-03-19 (RLVa-1.2.0a) | Added: RLVa-1.2.0a
bool RlvForceWear::isForceRemovable(LLWearableType::EType wtType, bool fCheckComposite /*=true*/, const LLUUID& idExcept /*=LLUUID::null*/)
{
    // Wearable type can be removed by an RLV command if there's at least one currently worn wearable that can be removed
    for (U32 idxWearable = 0, cntWearable = gAgentWearables.getWearableCount(wtType); idxWearable < cntWearable; idxWearable++)
        if (isForceRemovable(gAgentWearables.getViewerWearable(wtType, idxWearable), fCheckComposite, idExcept))
            return true;
    return false;
}

// Checked: 2010-03-19 (RLVa-1.2.0a) | Modified: RLVa-1.2.0a
void RlvForceWear::forceRemove(const LLViewerWearable* pWearable)
{
    // Sanity check - no need to process duplicate removes
    if ( (!pWearable) || (isRemWearable(pWearable)) )
        return;

    if (isForceRemovable(pWearable))
    {
        #ifdef RLV_EXPERIMENTAL_COMPOSITEFOLDERS
        LLViewerInventoryCategory* pFolder = NULL;
        if ( (RlvSettings::getEnableComposites()) &&
             (gRlvHandler.getCompositeInfo(gAgent.getWearableItem(wtType), NULL, &pFolder)) )
        {
            // Wearable belongs to a composite folder so detach the entire folder (if we can take it off)
            if (gRlvHandler.canTakeOffComposite(pFolder))
                forceFolder(pFolder, ACTION_DETACH, FLAG_DEFAULT);
        }
        else
        #endif // RLV_EXPERIMENTAL_COMPOSITEFOLDERS
        {
            remWearable(pWearable);
        }
    }
}

// Checked: 2010-03-19 (RLVa-1.2.0a) | Added: RLVa-1.2.0a
void RlvForceWear::forceRemove(LLWearableType::EType wtType)
{
    for (U32 idxWearable = 0, cntWearable = gAgentWearables.getWearableCount(wtType); idxWearable < cntWearable; idxWearable++)
        forceRemove(gAgentWearables.getViewerWearable(wtType, idxWearable));
}


// Checked: 2010-08-30 (RLVa-1.2.1c) | Modified: RLVa-1.2.1c
void RlvForceWear::addAttachment(const LLViewerInventoryItem* pItem, EWearAction eAction)
{
    // Remove it from 'm_remAttachments' if it's queued for detaching
    const LLViewerObject* pAttachObj = (isAgentAvatarValid()) ? gAgentAvatarp->getWornAttachment(pItem->getLinkedUUID()) : NULL;
    if ( (pAttachObj) && (isRemAttachment(pAttachObj)) )
        m_remAttachments.erase(std::remove_if(m_remAttachments.begin(), m_remAttachments.end(), [pAttachObj](const LLPointer<LLViewerObject>& p) { return p.get() == pAttachObj; }), m_remAttachments.end());

    S32 idxAttachPt = RlvAttachPtLookup::getAttachPointIndex(pItem, true);
    if (ACTION_WEAR_ADD == eAction)
    {
        // Insert it at the back if it's not already there
        idxAttachPt |= ATTACHMENT_ADD;
        if (!isAddAttachment(pItem))
        {
            addattachments_map_t::iterator itAddAttachments = m_addAttachments.find(idxAttachPt);
            if (itAddAttachments == m_addAttachments.end())
            {
                m_addAttachments.insert(addattachment_pair_t(idxAttachPt, LLInventoryModel::item_array_t()));
                itAddAttachments = m_addAttachments.find(idxAttachPt);
            }
            itAddAttachments->second.push_back((LLViewerInventoryItem*)pItem);
        }
    }
    else if (ACTION_WEAR_REPLACE == eAction)
    {
        // Replace all pending attachments on this attachment point with the specified item (don't clear if it's the default attach point)
        addattachments_map_t::iterator itAddAttachments = m_addAttachments.find(idxAttachPt | ATTACHMENT_ADD);
        if ( (0 != idxAttachPt) && (itAddAttachments != m_addAttachments.end()) )
            itAddAttachments->second.clear();

        itAddAttachments = m_addAttachments.find(idxAttachPt);
        if (itAddAttachments == m_addAttachments.end())
        {
            m_addAttachments.insert(addattachment_pair_t(idxAttachPt, LLInventoryModel::item_array_t()));
            itAddAttachments = m_addAttachments.find(idxAttachPt);
        }

        if (0 != idxAttachPt)
            itAddAttachments->second.clear();
        itAddAttachments->second.push_back((LLViewerInventoryItem*)pItem);
    }
}

// Checked: 2010-08-30 (RLVa-1.2.1c) | Modified: RLVa-1.2.1c
void RlvForceWear::remAttachment(const LLViewerObject* pAttachObj)
{
    // Remove it from 'm_addAttachments' if it's queued for attaching
    const LLViewerInventoryItem* pItem = (pAttachObj->isAttachment()) ? gInventory.getItem(pAttachObj->getAttachmentItemID()) : NULL;
    if (pItem)
    {
        addattachments_map_t::iterator itAddAttachments = m_addAttachments.begin();
        while (itAddAttachments != m_addAttachments.end())
        {
            LLInventoryModel::item_array_t& wearItems = itAddAttachments->second;
            if (std::find_if(wearItems.begin(), wearItems.end(), RlvPredIsEqualOrLinkedItem(pItem)) != wearItems.end())
                wearItems.erase(std::remove_if(wearItems.begin(), wearItems.end(), RlvPredIsEqualOrLinkedItem(pItem)), wearItems.end());

            if (wearItems.empty())
                m_addAttachments.erase(itAddAttachments++);
            else
                ++itAddAttachments;
        }
    }

    // Add it to 'm_remAttachments' if it's not already there
    if (!isRemAttachment(pAttachObj))
    {
        m_remAttachments.push_back(LLPointer<LLViewerObject>(const_cast<LLViewerObject*>(pAttachObj)));
    }
}

// Checked: 2010-08-30 (RLVa-1.2.1c) | Modified: RLVa-1.2.1c
void RlvForceWear::addWearable(const LLViewerInventoryItem* pItem, EWearAction eAction)
{
    const LLViewerWearable* pWearable = gAgentWearables.getWearableFromItemID(pItem->getLinkedUUID());
    // When replacing remove all currently worn wearables of this type *unless* the item is currently worn
    if ( (ACTION_WEAR_REPLACE == eAction) && (!pWearable) )
        forceRemove(pItem->getWearableType());
    // Remove it from 'm_remWearables' if it's pending removal
    if ( (pWearable) && (isRemWearable(pWearable)) )
        m_remWearables.erase(std::remove(m_remWearables.begin(), m_remWearables.end(), pWearable), m_remWearables.end());

    addwearables_map_t::iterator itAddWearables = m_addWearables.find(pItem->getWearableType());
    if (itAddWearables == m_addWearables.end())
    {
        m_addWearables.insert(addwearable_pair_t(pItem->getWearableType(), LLInventoryModel::item_array_t()));
        itAddWearables = m_addWearables.find(pItem->getWearableType());
    }

    if (ACTION_WEAR_ADD == eAction)             // Add it at the back if it's not already there
    {
        if (!isAddWearable(pItem))
            itAddWearables->second.push_back((LLViewerInventoryItem*)pItem);
    }
    else if (ACTION_WEAR_REPLACE == eAction)    // Replace all pending wearables of this type with the specified item
    {
        itAddWearables->second.clear();
        itAddWearables->second.push_back((LLViewerInventoryItem*)pItem);
    }
}

// Checked: 2010-08-30 (RLVa-1.2.1c) | Modified: RLVa-1.2.1c
void RlvForceWear::remWearable(const LLViewerWearable* pWearable)
{
    // Remove it from 'm_addWearables' if it's queued for wearing
    const LLViewerInventoryItem* pItem = gInventory.getItem(pWearable->getItemID());
    if ( (pItem) && (isAddWearable(pItem)) )
    {
        addwearables_map_t::iterator itAddWearables = m_addWearables.find(pItem->getWearableType());

        LLInventoryModel::item_array_t& wearItems = itAddWearables->second;
        wearItems.erase(std::remove_if(wearItems.begin(), wearItems.end(), RlvPredIsEqualOrLinkedItem(pItem)), wearItems.end());
        if (wearItems.empty())
            m_addWearables.erase(itAddWearables);
    }

    // Add it to 'm_remWearables' if it's not already there
    if (!isRemWearable(pWearable))
        m_remWearables.push_back(pWearable);
}

// Checked: 2015-05-05 (RLVa-1.4.12)
void RlvForceWear::updatePendingAttachments()
{
    if (RlvForceWear::instanceExists())
    {
        RlvForceWear* pThis = RlvForceWear::getInstance();
        for (const pendingattachments_map_t::value_type& itAttach : pThis->m_pendingAttachments)
            LLAttachmentsMgr::instance().addAttachmentRequest(itAttach.first, itAttach.second & ~ATTACHMENT_ADD, itAttach.second & ATTACHMENT_ADD);
        pThis->m_pendingAttachments.clear();
    }
}

// Checked: 2015-05-05 (RLVa-1.4.12)
void RlvForceWear::addPendingAttachment(const LLUUID& idItem, U8 idxPoint)
{
    auto itAttach = m_pendingAttachments.find(idItem);
    if (m_pendingAttachments.end() == itAttach)
        m_pendingAttachments.insert(std::make_pair(idItem, idxPoint));
    else
        itAttach->second = idxPoint;
}

// Checked: 2015-05-05 (RLVa-1.4.12)
void RlvForceWear::remPendingAttachment(const LLUUID& idItem)
{
    m_pendingAttachments.erase(idItem);
}


// Checked: 2015-05-05 (RLVa-1.4.12) | Charis: rewritten. The Charis LLAppearanceMgr has no updateCOF(body, clothing, attachments,
//   gestures, ...) and removeItemsFromAvatar() takes (ids, post_update_func): wearItemsOnAvatar() handles body parts and
//   clothing, gestures are linked into the COF and activated by hand, removals go through removeItemsFromAvatar().
void RlvForceWear::done()
{
    // Sanity check - don't go through all the motions below only to find out there's nothing to actually do
    if ( (m_remWearables.empty()) && (m_remAttachments.empty()) && (m_remGestures.empty()) &&
         (m_addWearables.empty()) && (m_addAttachments.empty()) && (m_addGestures.empty()) )
    {
        return;
    }

    //
    // Process removals
    //

    uuid_vec_t remItems;

    // Wearables
    if (m_remWearables.size())
    {
        for (const LLViewerWearable* pWearable : m_remWearables)
            remItems.push_back(pWearable->getItemID());
        m_remWearables.clear();
    }

    // Gestures
    if (m_remGestures.size())
    {
        // NOTE: LLGestureMgr::deactivateGesture() will call LLAppearanceMgr::removeCOFItemLinks() for us and supply its own callback
        for (const LLViewerInventoryItem* pItem : m_remGestures)
            LLGestureMgr::instance().deactivateGesture(pItem->getUUID());
        m_remGestures.clear();
    }

    // Attachments
    if (m_remAttachments.size())
    {
        // Charis: an object may have died or been detached since it was queued (a line that waited for the next batch)
        std::vector<LLViewerObject*> remAttachments;
        for (const LLPointer<LLViewerObject>& pAttachObj : m_remAttachments)
        {
            if ( (pAttachObj.notNull()) && (!pAttachObj->isDead()) && (pAttachObj->isAttachment()) )
            {
                remAttachments.push_back(pAttachObj.get());
                remItems.push_back(pAttachObj->getAttachmentItemID());
            }
        }
        if (!remAttachments.empty())
            LLAgentWearables::userRemoveMultipleAttachments(remAttachments);
        m_remAttachments.clear();
    }

    //
    // Process additions
    //

    // Body parts and clothing go to LLAppearanceMgr::wearItemsOnAvatar() (it handles both itself and drops the worn body part
    // of the same type; the replace/add decision was already made in addWearable())
    uuid_vec_t addWearables;
    for (addwearables_map_t::const_iterator itAddWearables = m_addWearables.cbegin(); itAddWearables != m_addWearables.cend(); ++itAddWearables)
    {
        // NOTE: LLAppearanceMgr will filter our duplicates so no need for us to check here
        for (LLViewerInventoryItem* pItem : itAddWearables->second)
            addWearables.push_back(pItem->getUUID());
    }
    m_addWearables.clear();

    // Until LL provides a way for the COF update to selectively attach add/replace we have to deal with attachments ourselves
    for (addattachments_map_t::const_iterator itAddAttachments = m_addAttachments.cbegin(); itAddAttachments != m_addAttachments.cend(); ++itAddAttachments)
    {
        for (const LLViewerInventoryItem* pItem : itAddAttachments->second)
            addPendingAttachment(pItem->getLinkedUUID(), itAddAttachments->first);
    }
    m_addAttachments.clear();

    //
    // Tie it all together
    //

    //          |    Wearables     |   Attachments    |   Gestures                |
    //          |=================================================================|
    // Add    : | LLAppearanceMgr  | <custom>         | LLGestureMgr + COF link   |
    // Remove : | LLAppearanceMgr  | LLAppearanceMgr  | LLGestureMgr              |
    // One callback for the removals and the additions (as in FS): the appearance is updated from the COF once, after the last
    // link was made or removed, and then the pending attachments are requested. With nothing to do at all it is what
    // requests the pending attachments (it updates the appearance when the last reference goes away).
    {
        LLPointer<LLInventoryCallback> cb = new LLUpdateAppearanceOnDestroy(false, false, boost::bind(RlvForceWear::updatePendingAttachments));

        // The COF links go away at once, like LLAppearanceMgr::removeItemsFromAvatar() does (which can't share a callback; its
        // addDoomedTempAttachment() is private - the attachments themselves were already detached by userRemoveMultipleAttachments())
        for (const LLUUID& idRemove : remItems)
        {
            const LLUUID idLinked = gInventory.getLinkedItemID(idRemove);
            const LLViewerInventoryItem* pRemove = gInventory.getItem(idLinked);
            if ( (pRemove) && (LLAssetType::AT_BODYPART == pRemove->getType()) )
                continue;       // Body parts are never taken off
            LLAppearanceMgr::instance().removeCOFItemLinks(idLinked, cb);
            LLAttachmentsMgr::instance().clearPendingAttachmentLink(idLinked);
        }

        if (!addWearables.empty())
            LLAppearanceMgr::instance().wearItemsOnAvatar(addWearables, false, false, cb);

        if (!m_addGestures.empty())
        {
            LLViewerInventoryItem::item_array_t gestures;
            for (const LLViewerInventoryItem* pItem : m_addGestures)
            {
                LLViewerInventoryItem* pGesture = gInventory.getItem(pItem->getLinkedUUID());
                if (!pGesture)
                    continue;
                LLAppearanceMgr::instance().addCOFItemLink(pGesture->getUUID(), cb);
                gestures.push_back(pGesture);
            }
            LLGestureMgr::instance().activateGestures(gestures);
            m_addGestures.clear();
        }
    }

    // Make sure there are no leftovers for the next cycle
    RLV_ASSERT( (m_remWearables.empty()) && (m_remAttachments.empty()) && (m_remGestures.empty()) );
    RLV_ASSERT( (m_addWearables.empty()) && (m_addAttachments.empty()) && (m_addGestures.empty()) );
}

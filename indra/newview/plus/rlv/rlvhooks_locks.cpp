/**
 * @file rlvhooks_locks.cpp
 * @brief rlv-locks' part of the calls the official viewer files make into RLVa (plus/rlv), research/248 (W1-W3).
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

#include "llinventory.h"
#include "llnamevalue.h"
#include "llviewerjointattachment.h"
#include "llviewerobject.h"
#include "llvoavatarself.h"

#include "rlvforcewear.h"
#include "rlvhandler.h"
#include "rlvlocks.h"

namespace plus::rlv
{

// FS runs these from LLVOAvatarSelf::attachObject()/detachObject() ([RLVa:KB] blocks). Everything is main thread and
// only runs with RLVa on (the callers check enabled() first).

void onAttachObject(LLViewerObject* pAttachObj, const LLViewerJointAttachment* pAttachPt)
{
    if ( (!pAttachObj) || (!pAttachPt) )
        return;

    // FS: LLVOAvatarSelf::setAttachmentCallback() subscribers are told before the RLVa handlers (see gRlvAttachmentChanged)
    gRlvAttachmentChanged();

    RlvAttachmentLockWatchdog::instance().onAttach(pAttachObj, pAttachPt);
    gRlvHandler.onAttach(pAttachObj, pAttachPt);

    if ( (pAttachPt->getIsHUDAttachment()) && (!gRlvAttachmentLocks.hasLockedHUD()) )
        gRlvAttachmentLocks.updateLockedHUD();
}

void onRezzedAsAttachment(LLViewerObject* pObj)
{
    if (!pObj)
        return;
    // Rezzed object that's being worn as an attachment (we're assuming this will be due to llAttachToAvatar())
    S32 idxAttachPt = ATTACHMENT_ID_FROM_STATE(pObj->getAttachmentState());
    if (gRlvAttachmentLocks.isLockedAttachmentPoint(idxAttachPt, RLV_LOCK_ADD))
    {
        // If this will end up on an "add locked" attachment point then treat the attach as a user action
        LLNameValue* nvItem = pObj->getNVPair("AttachItemID");
        // Charis: the pair comes from the simulator - only a string that is a UUID is used (anything else is ignored)
        if ( (nvItem) && (NVT_STRING == nvItem->getTypeEnum()) )
        {
            LLUUID idItem;
            idItem.set(nvItem->getString(), false);
            // FS: at the moment llAttachToAvatar always seems to *add*
            if (idItem.notNull())
                RlvAttachmentLockWatchdog::instance().onWearAttachment(idItem, RLV_WEAR_ADD);
        }
    }
}

void onDetachObject(LLViewerObject* pAttachObj)
{
    if ( (!pAttachObj) || (!isAgentAvatarValid()) )
        return;

    for (const auto& entryAttachPt : gAgentAvatarp->mAttachmentPoints)
    {
        const LLViewerJointAttachment* pAttachPt = entryAttachPt.second;
        if ( (pAttachPt) && (pAttachPt->isObjectAttached(pAttachObj)) )
        {
            // NOTE: the RLVa handlers have to run *before* LLVOAvatar::detachObject() removes the object from its point
            RlvAttachmentLockWatchdog::instance().onDetach(pAttachObj, pAttachPt);
            gRlvHandler.onDetach(pAttachObj, pAttachPt);
            gRlvAttachmentChanged();
            break;
        }
    }
}

void onObjectDetached(LLViewerObject* pAttachObj)
{
    if ( (pAttachObj) && (pAttachObj->isHUDAttachment()) && (gRlvAttachmentLocks.hasLockedHUD()) )
        gRlvAttachmentLocks.updateLockedHUD();
}

bool canDetachAttachment(const LLInventoryItem* pItem)
{
    return (!pItem) || (gRlvAttachmentLocks.canDetach(pItem));
}

bool isForcedAttachRequest()
{
    return RlvForcedAttachScope::isActive();
}

void onSavedAssetIntoInventory(const LLUUID& idItem)
{
    if (RlvAttachmentLockWatchdog::instanceExists())
        RlvAttachmentLockWatchdog::instance().onSavedAssetIntoInventory(idItem);
}

// RlvForceWear::done() runs when a line's commands were all processed (the batch end of RlvHandler)
static void forceWearDone()
{
    if (RlvForceWear::instanceExists())
        RlvForceWear::instance().done();
}

namespace
{
    // Registered at program start (the pointer lives in RlvHandler as a constant-initialised static)
    struct ForceWearDoneRegistration
    {
        ForceWearDoneRegistration() { setCommandBatchDone(&forceWearDone); }
    } sForceWearDoneRegistration;
}

} // namespace plus::rlv

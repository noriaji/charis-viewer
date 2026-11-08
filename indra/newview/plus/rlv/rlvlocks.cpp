/**
 *
 * Copyright (c) 2009-2011, Kitty Barnett
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
#include "lloutfitobserver.h"
#include "llviewerobjectlist.h"
#include "llviewermenu.h"
#include "pipeline.h"

#include "rlvlocks.h"
#include "rlvhelper.h"
#include "rlvinventory.h"


// ============================================================================
// RlvAttachmentLocks member functions
//

RlvAttachmentLocks gRlvAttachmentLocks;

S32 RlvForcedAttachScope::s_nDepth = 0;

// Charis: FS has LLVOAvatarSelf::setAttachmentCallback(); here the attach/detach hooks (rlvhooks_locks.cpp) fire this signal
boost::signals2::signal<void()> gRlvAttachmentChanged;

// Checked: 2010-02-28 (RLVa-1.2.0a) | Modified: RLVa-1.2.0a
void RlvAttachmentLocks::addAttachmentLock(const LLUUID& idAttachObj, const LLUUID& idRlvObj)
{
/*
    // Sanity check - make sure it's an object we know about
    if ( (m_Objects.find(idRlvObj) == m_Objects.end()) || (!idxAttachPt) )
        return; // If (idxAttachPt) == 0 then: (pObj == NULL) || (pObj->isAttachment() == false)
*/

#ifndef RLV_RELEASE
    LLViewerObject* pDbgObj = gObjectList.findObject(idAttachObj);
    // Assertion: the object specified by idAttachObj exists/is rezzed, is an attachment and always specifies the root
    RLV_VERIFY( (pDbgObj) && (pDbgObj->isAttachment()) && (pDbgObj == pDbgObj->getRootEdit()) );
#endif // RLV_RELEASE

    m_AttachObjRem.insert(std::pair<LLUUID, LLUUID>(idAttachObj, idRlvObj));
    updateLockedHUD();
}

// Checked: 2010-02-28 (RLVa-1.2.0a) | Modified: RLVa-1.2.0a
void RlvAttachmentLocks::addAttachmentPointLock(S32 idxAttachPt, const LLUUID& idRlvObj, ERlvLockMask eLock)
{
/*
    // Sanity check - make sure it's an object we know about
    if ( (m_Objects.find(idRlvObj) == m_Objects.end()) || (!idxAttachPt) )
        return; // If (idxAttachPt) == 0 then: (pObj == NULL) || (pObj->isAttachment() == false)
*/

    // NOTE: m_AttachPtXXX can contain duplicate <idxAttachPt, idRlvObj> pairs (ie @detach:spine=n,detach=n from an attachment on spine)
    if (eLock & RLV_LOCK_REMOVE)
    {
        m_AttachPtRem.insert(std::pair<S32, LLUUID>(idxAttachPt, idRlvObj));
        updateLockedHUD();
    }
    if (eLock & RLV_LOCK_ADD)
        m_AttachPtAdd.insert(std::pair<S32, LLUUID>(idxAttachPt, idRlvObj));
}

// Checked: 2011-05-22 (RLVa-1.3.1b) | Added: RLVa-1.3.1b
bool RlvAttachmentLocks::canAttach() const
{
    if (isAgentAvatarValid())
    {
        for (LLVOAvatar::attachment_map_t::const_iterator itAttachPt = gAgentAvatarp->mAttachmentPoints.begin();
                itAttachPt != gAgentAvatarp->mAttachmentPoints.end(); ++itAttachPt)
        {
            if (!isLockedAttachmentPoint(itAttachPt->first, RLV_LOCK_ADD))
                return true;
        }
    }
    return false;
}

// Checked: 2010-08-07 (RLVa-1.2.0i) | Modified: RLVa-1.2.0i
bool RlvAttachmentLocks::canDetach(const LLViewerJointAttachment* pAttachPt, bool fDetachAll /*=false*/) const
{
    //   (fDetachAll) | (isLockedAttachment)
    //   ===================================
    //        F       |         F             => unlocked attachment => return true
    //        F       |         T             => locked attachment   => keep going
    //        T       |         F             => unlocked attachment => keep going
    //        T       |         T             => locked attachment   => return false
    //  -> inside the loop : (A xor (not B)) condition and return !fDetachAll
    //  -> outside the loop: return fDetachAll
    //       fDetachAll == false : return false => all attachments are locked
    //       fDetachAll == true  : return true  => all attachments are unlocked
    if (pAttachPt)
    {
        for (LLViewerJointAttachment::attachedobjs_vec_t::const_iterator itAttachObj = pAttachPt->mAttachedObjects.begin();
                itAttachObj != pAttachPt->mAttachedObjects.end(); ++itAttachObj)
        {
            if ( (fDetachAll) ^ (!isLockedAttachment(*itAttachObj)) )
                return !fDetachAll;
        }
    }
    return (pAttachPt) && (fDetachAll);
}

// Checked: 2010-02-28 (RLVa-1.2.0a) | Modified: RLVa-1.2.0a
bool RlvAttachmentLocks::hasLockedAttachment(const LLViewerJointAttachment* pAttachPt) const
{
    if (pAttachPt)
    {
        for (LLViewerJointAttachment::attachedobjs_vec_t::const_iterator itAttachObj = pAttachPt->mAttachedObjects.begin();
                itAttachObj != pAttachPt->mAttachedObjects.end(); ++itAttachObj)
        {
            if (isLockedAttachment(*itAttachObj))
                return true;
        }
    }
    return false;
}

// Checked: 2010-03-19 (RLVa-1.2.0a) | Modified: RLVa-1.2.0a
bool RlvAttachmentLocks::isLockedAttachmentExcept(const LLViewerObject* pObj, const LLUUID& idRlvObj) const
{
    if (idRlvObj.isNull())
        return isLockedAttachment(pObj);

    // Charis: we store root UUIDs in m_AttachObjRem, so a child prim is looked up through its root (FS asserts the caller did that)
    if (!pObj)
        return false;
    pObj = pObj->getRootEdit();

    // Loop over every object that has the specified attachment locked (but ignore any locks owned by idRlvObj)
    for (rlv_attachobjlock_map_t::const_iterator itAttachObj = m_AttachObjRem.lower_bound(pObj->getID()),
            endAttachObj = m_AttachObjRem.upper_bound(pObj->getID()); itAttachObj != endAttachObj; ++itAttachObj)
    {
        if (itAttachObj->second != idRlvObj)
            return true;
    }
    return isLockedAttachmentPointExcept(RlvAttachPtLookup::getAttachPointIndex(pObj), RLV_LOCK_REMOVE, idRlvObj);
}

// Checked: 2010-03-19 (RLVa-1.2.0a) | Added: RLVa-1.0.5b
bool RlvAttachmentLocks::isLockedAttachmentPointExcept(S32 idxAttachPt, ERlvLockMask eLock, const LLUUID& idRlvObj) const
{
    if (idRlvObj.isNull())
        return isLockedAttachmentPoint(idxAttachPt, eLock);

    // Loop over every object that has the specified attachment point locked (but ignore any locks owned by idRlvObj)
    if (eLock & RLV_LOCK_REMOVE)
    {
        for (rlv_attachptlock_map_t::const_iterator itAttachPt = m_AttachPtRem.lower_bound(idxAttachPt),
                endAttachPt = m_AttachPtRem.upper_bound(idxAttachPt); itAttachPt != endAttachPt; ++itAttachPt)
        {
            if (itAttachPt->second != idRlvObj)
                return true;
        }
    }
    if (eLock & RLV_LOCK_ADD)
    {
        for (rlv_attachptlock_map_t::const_iterator itAttachPt = m_AttachPtAdd.lower_bound(idxAttachPt),
                endAttachPt = m_AttachPtAdd.upper_bound(idxAttachPt); itAttachPt != endAttachPt; ++itAttachPt)
        {
            if (itAttachPt->second != idRlvObj)
                return true;
        }
    }
    return false;
}

// Checked: 2010-02-28 (RLVa-1.2.0a) | Modified: RLVa-1.2.0a
void RlvAttachmentLocks::removeAttachmentLock(const LLUUID& idAttachObj, const LLUUID& idRlvObj)
{
/*
    // Sanity check - make sure it's an object we know about
    if ( (m_Objects.find(idRlvObj) == m_Objects.end()) || (!idxAttachPt) )
        return; // If (idxAttachPt) == 0 then: (pObj == NULL) || (pObj->isAttachment() == false)
*/

#ifndef RLV_RELEASE
    // NOTE: pObj *can* be NULL [see comments for @detach=n in RlvHandler::onAddRemDetach()]
    const LLViewerObject* pDbgObj = gObjectList.findObject(idAttachObj);
    // Assertion: if the object exists then it's an attachment and always specifies the root
    RLV_VERIFY( (!pDbgObj) || ((pDbgObj->isAttachment()) && (pDbgObj == pDbgObj->getRootEdit())) );
#endif // RLV_RELEASE

    // NOTE: try to remove the lock even if pObj isn't an attachment (ie in case the user was able to "Drop" it)
    RLV_ASSERT( m_AttachObjRem.lower_bound(idAttachObj) != m_AttachObjRem.upper_bound(idAttachObj) ); // The lock should always exist
    for (rlv_attachobjlock_map_t::iterator itAttachObj = m_AttachObjRem.lower_bound(idAttachObj),
            endAttachObj = m_AttachObjRem.upper_bound(idAttachObj); itAttachObj != endAttachObj; ++itAttachObj)
    {
        if (idRlvObj == itAttachObj->second)
        {
            m_AttachObjRem.erase(itAttachObj);
            updateLockedHUD();
            break;
        }
    }
}

// Checked: 2010-02-28 (RLVa-1.2.0a) | Modified: RLVa-1.2.0a
void RlvAttachmentLocks::removeAttachmentPointLock(S32 idxAttachPt, const LLUUID& idRlvObj, ERlvLockMask eLock)
{
/*
    // Sanity check - make sure it's an object we know about
    if ( (m_Objects.find(idRlvObj) == m_Objects.end()) || (!idxAttachPt) )
        return; // If (idxAttachPt) == 0 then: (pObj == NULL) || (pObj->isAttachment() == false)
*/

    if (eLock & RLV_LOCK_REMOVE)
    {
        RLV_ASSERT( m_AttachPtRem.lower_bound(idxAttachPt) != m_AttachPtRem.upper_bound(idxAttachPt) ); // The lock should always exist
        for (rlv_attachptlock_map_t::iterator itAttachPt = m_AttachPtRem.lower_bound(idxAttachPt),
                endAttachPt = m_AttachPtRem.upper_bound(idxAttachPt); itAttachPt != endAttachPt; ++itAttachPt)
        {
            if (idRlvObj == itAttachPt->second)
            {
                m_AttachPtRem.erase(itAttachPt);
                updateLockedHUD();
                break;
            }
        }
    }
    if (eLock & RLV_LOCK_ADD)
    {
        RLV_ASSERT( m_AttachPtAdd.lower_bound(idxAttachPt) != m_AttachPtAdd.upper_bound(idxAttachPt) ); // The lock should always exist
        for (rlv_attachptlock_map_t::iterator itAttachPt = m_AttachPtAdd.lower_bound(idxAttachPt),
                endAttachPt = m_AttachPtAdd.upper_bound(idxAttachPt); itAttachPt != endAttachPt; ++itAttachPt)
        {
            if (idRlvObj == itAttachPt->second)
            {
                m_AttachPtAdd.erase(itAttachPt);
                break;
            }
        }
    }
}

// Checked: 2010-08-22 (RLVa-1.2.1a) | Modified: RLVa-1.2.1a
void RlvAttachmentLocks::updateLockedHUD()
{
    if (!isAgentAvatarValid())
        return;

    m_fHasLockedHUD = false;
    for (LLVOAvatar::attachment_map_t::const_iterator itAttachPt = gAgentAvatarp->mAttachmentPoints.begin();
            itAttachPt != gAgentAvatarp->mAttachmentPoints.end(); ++itAttachPt)
    {
        const LLViewerJointAttachment* pAttachPt = itAttachPt->second;
        if ( (pAttachPt) && (pAttachPt->getIsHUDAttachment()) && (hasLockedAttachment(pAttachPt)) )
        {
            m_fHasLockedHUD = true;
            break;
        }
    }

    // Reset HUD visibility and wireframe options if at least one HUD attachment is locked
    if (m_fHasLockedHUD)
    {
        gUseWireframe = false;     // Charis: FS calls set_use_wireframe(false) (llviewermenu.cpp), which only sets this flag
    }
}

// Charis: safety net, see the header
void RlvAttachmentLocks::removeAllLocksOf(const LLUUID& idRlvObj)
{
    bool fChanged = false;
    for (rlv_attachobjlock_map_t::iterator it = m_AttachObjRem.begin(); it != m_AttachObjRem.end(); )
    {
        if (it->second == idRlvObj) { it = m_AttachObjRem.erase(it); fChanged = true; } else ++it;
    }
    for (rlv_attachptlock_map_t* pMap : { &m_AttachPtRem, &m_AttachPtAdd })
    {
        for (rlv_attachptlock_map_t::iterator it = pMap->begin(); it != pMap->end(); )
        {
            if (it->second == idRlvObj) { it = pMap->erase(it); fChanged = true; } else ++it;
        }
    }
    if (fChanged)
        updateLockedHUD();
}

// Checked: 2010-03-11 (RLVa-1.2.0a) | Added: RLVa-1.2.0a
bool RlvAttachmentLocks::verifyAttachmentLocks()
{
    bool fSuccess = true;

    // Verify attachment locks
    rlv_attachobjlock_map_t::iterator itAttachObj = m_AttachObjRem.begin(), itCurrentObj;
    while (itAttachObj != m_AttachObjRem.end())
    {
        itCurrentObj = itAttachObj++;

        // If the attachment no longer exists (or is no longer attached) then we shouldn't be holding a lock to it
        const LLViewerObject* pAttachObj = gObjectList.findObject(itCurrentObj->first);
        if ( (!pAttachObj) || (!pAttachObj->isAttachment()) )
        {
            m_AttachObjRem.erase(itCurrentObj);
            fSuccess = false;
        }
    }

    return fSuccess;
}

// ============================================================================
// RlvAttachmentLockWatchdog member functions
//

// Checked: 2010-09-23 (RLVa-1.2.1d) | Added: RLVa-1.2.1d
bool RlvAttachmentLockWatchdog::RlvWearInfo::isAddLockedAttachPt(S32 idxAttachPt) const
{
    // If idxAttachPt has no entry in attachPts then the attachment point wasn't RLV_LOCK_ADD restricted at the time we were instantiated
    // [See RlvAttachmentLockWatchdog::onWearAttachment()]
    return (attachPts.find(idxAttachPt) != attachPts.end());
}

// Checked: 2010-09-23 (RLVa-1.2.1d) | Added: RLVa-1.2.1d
// Charis: logs with LL_DEBUGS("RLV") only (item names must not reach the log of a normal run)
void RlvAttachmentLockWatchdog::RlvWearInfo::dumpInstance() const
{
    const LLViewerInventoryItem* pItem = gInventory.getItem(idItem);
    std::string strItemId = idItem.asString();

    std::string strTemp = llformat("Wear %s '%s' (%s)",
        (RLV_WEAR_ADD == eWearAction) ? "add" : "replace", (pItem) ? pItem->getName().c_str() : "missing", strItemId.c_str());
    RLV_DEBUGS << strTemp.c_str() << RLV_ENDL;

    if (!attachPts.empty())
    {
        std::string strEmptyAttachPt;
        for (std::map<S32, uuid_vec_t>::const_iterator itAttachPt = attachPts.begin(); itAttachPt != attachPts.end(); ++itAttachPt)
        {
            const LLViewerJointAttachment* pAttachPt =
                get_if_there(gAgentAvatarp->mAttachmentPoints, itAttachPt->first, (LLViewerJointAttachment*)NULL);
            if (!itAttachPt->second.empty())
            {
                for (uuid_vec_t::const_iterator itAttach = itAttachPt->second.begin(); itAttach != itAttachPt->second.end(); ++itAttach)
                {
                    pItem = gInventory.getItem(*itAttach);
                    strItemId = (*itAttach).asString();

                    strTemp = llformat("  -> %s : %s (%s)",
                        pAttachPt->getName().c_str(), (pItem) ? pItem->getName().c_str() : "missing", strItemId.c_str());
                    RLV_DEBUGS << strTemp.c_str() << RLV_ENDL;
                }
            }
            else
            {
                if (!strEmptyAttachPt.empty())
                    strEmptyAttachPt += ", ";
                strEmptyAttachPt += pAttachPt->getName();
            }
        }
        if (!strEmptyAttachPt.empty())
            RLV_DEBUGS << "  -> " << strEmptyAttachPt << " : empty" << RLV_ENDL;
    }
    else
    {
        RLV_DEBUGS << "  -> no attachment point information" << RLV_ENDL;
    }
}

// Checked: 2010-07-28 (RLVa-1.2.0i) | Modified: RLVa-1.2.0i
void RlvAttachmentLockWatchdog::detach(const LLViewerObject* pAttachObj)
{
    if (pAttachObj)
    {
        gMessageSystem->newMessage("ObjectDetach");
        gMessageSystem->nextBlockFast(_PREHASH_AgentData);
        gMessageSystem->addUUIDFast(_PREHASH_AgentID, gAgent.getID() );
        gMessageSystem->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());

        gMessageSystem->nextBlockFast(_PREHASH_ObjectData);
        gMessageSystem->addU32Fast(_PREHASH_ObjectLocalID, pAttachObj->getLocalID());
        if (std::find(m_PendingDetach.begin(), m_PendingDetach.end(), pAttachObj->getAttachmentItemID()) == m_PendingDetach.end())
            m_PendingDetach.push_back(pAttachObj->getAttachmentItemID());
        m_tsPendingDetach = LLFrameTimer::getElapsedSeconds();      // Charis: a pending detach is forgotten after 60 seconds (onTimer())
        startTimer();

        gMessageSystem->sendReliable(gAgent.getRegionHost() );
    }
}

// Checked: 2011-06-13 (RLVa-1.3.1b) | Modified: RLVa-1.3.1b
void RlvAttachmentLockWatchdog::detach(S32 idxAttachPt, const uuid_vec_t& idsAttachObjExcept)
{
    const LLViewerJointAttachment* pAttachPt = RlvAttachPtLookup::getAttachPoint(idxAttachPt);
    if (!pAttachPt)
        return;

    std::vector<const LLViewerObject*> attachObjs;
    for (LLViewerJointAttachment::attachedobjs_vec_t::const_iterator itAttachObj = pAttachPt->mAttachedObjects.begin();
            itAttachObj != pAttachPt->mAttachedObjects.end(); ++itAttachObj)
    {
        const LLViewerObject* pAttachObj = *itAttachObj;
        if (idsAttachObjExcept.end() == std::find(idsAttachObjExcept.begin(), idsAttachObjExcept.end(), pAttachObj->getID()))
            attachObjs.push_back(pAttachObj);
    }

    if (!attachObjs.empty())
    {
        gMessageSystem->newMessage("ObjectDetach");
        gMessageSystem->nextBlockFast(_PREHASH_AgentData);
        gMessageSystem->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        gMessageSystem->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());

        for (std::vector<const LLViewerObject*>::const_iterator itAttachObj = attachObjs.begin(); itAttachObj != attachObjs.end(); ++itAttachObj)
        {
            const LLViewerObject* pAttachObj = *itAttachObj;
            gMessageSystem->nextBlockFast(_PREHASH_ObjectData);
            gMessageSystem->addU32Fast(_PREHASH_ObjectLocalID, pAttachObj->getLocalID());
            if (m_PendingDetach.end() == std::find(m_PendingDetach.begin(), m_PendingDetach.end(), pAttachObj->getAttachmentItemID()))
                m_PendingDetach.push_back(pAttachObj->getAttachmentItemID());
        }
        m_tsPendingDetach = LLFrameTimer::getElapsedSeconds();      // Charis: a pending detach is forgotten after 60 seconds (onTimer())
        startTimer();

        gMessageSystem->sendReliable(gAgent.getRegionHost());
    }
}

// Checked: 2010-09-23 (RLVa-1.2.1d) | Modified: RLVa-1.2.1d
void RlvAttachmentLockWatchdog::onAttach(const LLViewerObject* pAttachObj, const LLViewerJointAttachment* pAttachPt)
{
    S32 idxAttachPt = RlvAttachPtLookup::getAttachPointIndex(pAttachObj);
    const LLUUID& idAttachItem = (pAttachObj) ? pAttachObj->getAttachmentItemID() : LLUUID::null;
    RLV_ASSERT( (!isAgentAvatarValid()) || ((idxAttachPt) && (idAttachItem.notNull())) );
    if ( (!idxAttachPt) || (idAttachItem.isNull()) )
        return;

    // Check if the attachment point has a pending "reattach"
    rlv_attach_map_t::iterator itAttach = m_PendingAttach.lower_bound(idxAttachPt), itAttachEnd = m_PendingAttach.upper_bound(idxAttachPt);
    if (itAttach != itAttachEnd)
    {
        bool fPendingReattach = false;
        for (; itAttach != itAttachEnd; ++itAttach)
        {
            if (idAttachItem == itAttach->second.idItem)
            {
                fPendingReattach = true;
                RlvBehaviourNotifyHandler::onReattach(pAttachPt, true);
                m_PendingAttach.erase(itAttach);
                break;
            }
        }
        if (!fPendingReattach)
        {
            detach(pAttachObj);
            RlvBehaviourNotifyHandler::onAttach(pAttachPt, false);
        }
        return;
    }

    // Check if the attach was allowed at the time it was requested
    rlv_wear_map_t::iterator itWear = m_PendingWear.find(idAttachItem); bool fAttachAllowed = true;
    if (itWear != m_PendingWear.end())
    {
        // We'll need to return the attachment point to its previous state if it was non-attachable
        if (itWear->second.isAddLockedAttachPt(idxAttachPt))
        {
            // Get the saved state of the attachment point (but do nothing if the item itself was already worn then)
            std::map<S32, uuid_vec_t>::iterator itAttachPrev = itWear->second.attachPts.find(idxAttachPt);
            RLV_ASSERT(itAttachPrev != itWear->second.attachPts.end());
            if (std::find(itAttachPrev->second.begin(), itAttachPrev->second.end(), idAttachItem) == itAttachPrev->second.end())
            {
                // If it was empty we need to detach everything on the attachment point; if it wasn't we need to restore it to what it was
                if (itAttachPrev->second.empty())
                {
                    detach(idxAttachPt);
                }
                else
                {
                    // Iterate over all the current attachments and force detach any that shouldn't be there
                    for (LLViewerJointAttachment::attachedobjs_vec_t::const_iterator itAttachObj = pAttachPt->mAttachedObjects.begin();
                            itAttachObj != pAttachPt->mAttachedObjects.end(); ++itAttachObj)
                    {
                        const LLViewerObject* pAttachObj = *itAttachObj;

                        uuid_vec_t::iterator itAttach =
                            std::find(itAttachPrev->second.begin(), itAttachPrev->second.end(), pAttachObj->getAttachmentItemID());
                        if (itAttach == itAttachPrev->second.end())
                            detach(pAttachObj);
                        else
                            itAttachPrev->second.erase(itAttach);
                    }

                    // Whatever is left is something that needs to be reattached
                    for (uuid_vec_t::const_iterator itAttach = itAttachPrev->second.begin();
                            itAttach != itAttachPrev->second.end(); ++itAttach)
                    {
                        m_PendingAttach.insert(std::pair<S32, RlvReattachInfo>(idxAttachPt, RlvReattachInfo(*itAttach)));
                    }
                }
                fAttachAllowed = false;
            }
        }
        else if (RLV_WEAR_REPLACE == itWear->second.eWearAction)
        {
            // Now that we know where this attaches to, check if we can actually perform a "replace"
            uuid_vec_t idsAttachObjExcept;
            for (LLViewerJointAttachment::attachedobjs_vec_t::const_iterator itAttachObj = pAttachPt->mAttachedObjects.begin();
                    ((itAttachObj != pAttachPt->mAttachedObjects.end()) && (fAttachAllowed)); ++itAttachObj)
            {
                if ( (pAttachObj != *itAttachObj) && (gRlvAttachmentLocks.isLockedAttachment(*itAttachObj)) )
                {
                    // Fail if we encounter a non-detachable attachment (unless we're only replacing detachable attachments)
                    if (gSavedSettings.get<bool>(RlvSettingNames::WearReplaceUnlocked))
                        idsAttachObjExcept.push_back((*itAttachObj)->getID());
                    else
                        fAttachAllowed = false;
                }
            }

            if (fAttachAllowed)
            {
                idsAttachObjExcept.push_back(pAttachObj->getID());  // Replace == allowed: detach everything except the new attachment
                detach(idxAttachPt, idsAttachObjExcept);            // or detach all *unlocked* attachments except the new attachment
            }
            else
            {
                detach(pAttachObj);                                 // Replace != allowed: detach the new attachment
            }
        }
        m_PendingWear.erase(itWear); // No need to start the timer since it should be running already if '!m_PendingWear.empty()'
    }
    RlvBehaviourNotifyHandler::onAttach(pAttachPt, fAttachAllowed);
}

// Checked: 2010-07-28 (RLVa-1.2.0i) | Modified: RLVa-1.2.0i
void RlvAttachmentLockWatchdog::onDetach(const LLViewerObject* pAttachObj, const LLViewerJointAttachment* pAttachPt)
{
    S32 idxAttachPt = RlvAttachPtLookup::getAttachPointIndex(pAttachPt);
    const LLUUID& idAttachItem = (pAttachObj) ? pAttachObj->getAttachmentItemID() : LLUUID::null;
    RLV_ASSERT( (!isAgentAvatarValid()) || ((idxAttachPt) && (idAttachItem.notNull())) );
    if ( (!idxAttachPt) || (idAttachItem.isNull()) )
        return;

    // If it's an attachment that's pending force-detach then we don't want to do anything (even if it's currently "remove locked")
    rlv_detach_map_t::iterator itDetach = std::find(m_PendingDetach.begin(), m_PendingDetach.end(), idAttachItem);
    if (itDetach != m_PendingDetach.end())
    {
        m_PendingDetach.erase(itDetach);
        RlvBehaviourNotifyHandler::onDetach(pAttachPt, true);
        return;
    }

    // If the attachment is currently "remove locked" then we should reattach it (unless it's already pending reattach)
    bool fDetachAllowed = true;
    if (gRlvAttachmentLocks.isLockedAttachment(pAttachObj))
    {
        bool fPendingAttach = false;
        for (rlv_attach_map_t::const_iterator itReattach = m_PendingAttach.lower_bound(idxAttachPt),
                itReattachEnd = m_PendingAttach.upper_bound(idxAttachPt); itReattach != itReattachEnd; ++itReattach)
        {
            if (itReattach->second.idItem == idAttachItem)
            {
                fPendingAttach = true;
                break;
            }
        }

        // TODO-RLVa: [RLVa-1.2.1] we should re-add the item to COF as well to make sure it'll reattach when the user relogs
        //      -> check the call order in LLVOAvatarSelf::detachObject() since COF removal happens *after* we're called
        if (!fPendingAttach)
        {
            m_PendingAttach.insert(std::pair<S32, RlvReattachInfo>(idxAttachPt, RlvReattachInfo(idAttachItem)));
            startTimer();
        }
        fDetachAllowed = false;
    }
    RlvBehaviourNotifyHandler::onDetach(pAttachPt, fDetachAllowed);
}

// Checked: 2010-03-05 (RLVa-1.2.0a) | Modified: RLVa-1.0.5b
void RlvAttachmentLockWatchdog::onSavedAssetIntoInventory(const LLUUID& idItem)
{
    for (rlv_attach_map_t::iterator itAttach = m_PendingAttach.begin(); itAttach != m_PendingAttach.end(); ++itAttach)
    {
        if ( (!itAttach->second.fAssetSaved) && (idItem == itAttach->second.idItem) )
        {
            RlvForcedAttachScope fForced;   // Charis: FS passes fRlvForce=true as the fourth argument
            LLAttachmentsMgr::instance().addAttachmentRequest(itAttach->second.idItem, itAttach->first, true);
            itAttach->second.tsAttach = LLFrameTimer::getElapsedSeconds();
        }
    }
}

// Checked: 2010-03-05 (RLVa-1.2.0a) | Modified: RLVa-1.0.5b
bool RlvAttachmentLockWatchdog::onTimer()
{
    // RELEASE-RLVa: [SL-2.0.0] This will need rewriting for "ENABLE_MULTIATTACHMENTS"
    F64 tsCurrent = LLFrameTimer::getElapsedSeconds();

    // Garbage collect (failed) wear requests older than 60 seconds
    rlv_wear_map_t::iterator itWear = m_PendingWear.begin();
    while (itWear != m_PendingWear.end())
    {
        if (itWear->second.tsWear + 60 < tsCurrent)
            m_PendingWear.erase(itWear++);
        else
            ++itWear;
    }

    // Charis: a detach the simulator never confirmed is forgotten after 60 seconds (it would keep the timer and the exemption alive)
    if ( (!m_PendingDetach.empty()) && (m_tsPendingDetach + 60 < tsCurrent) )
        m_PendingDetach.clear();

    // Charis: no attachment point lock of any kind is left => nothing left to put back (an "add locked" point is put back too)
    if (!gRlvAttachmentLocks.hasLockedAttachmentPoint(RLV_LOCK_ANY))
        m_PendingAttach.clear();

    // Walk over the pending reattach list
    rlv_attach_map_t::iterator itAttach = m_PendingAttach.begin();
    while (itAttach != m_PendingAttach.end())
    {
        // Sanity check - make sure the item is still in the user's inventory
        if (gInventory.getItem(itAttach->second.idItem) == NULL)
        {
            m_PendingAttach.erase(itAttach++);
            continue;
        }

        // Force an attach if we haven't gotten a SavedAssetIntoInventory message after 15 seconds
        // (or if it's been 30 seconds since we last tried to reattach the item)
        bool fAttach = false;
        if ( (!itAttach->second.fAssetSaved) && (itAttach->second.tsDetach + 15 < tsCurrent) )
        {
            itAttach->second.fAssetSaved = true;
            fAttach = true;
        }
        else if ( (itAttach->second.fAssetSaved) && (itAttach->second.tsAttach + 30 < tsCurrent) )
        {
            fAttach = true;
        }

        // Charis: give up after RLV_MAX_REATTACH_TRIES requests (a lost item or a refused request must not loop forever)
        if ( (fAttach) && (++itAttach->second.nTries > RLV_MAX_REATTACH_TRIES) )
        {
            m_PendingAttach.erase(itAttach++);
            continue;
        }

        if (fAttach)
        {
            RlvForcedAttachScope fForced;   // Charis: FS passes fRlvForce=true as the fourth argument
            LLAttachmentsMgr::instance().addAttachmentRequest(itAttach->second.idItem, itAttach->first, true);
            itAttach->second.tsAttach = tsCurrent;
        }

        ++itAttach;
    }

    return ( (m_PendingAttach.empty()) && (m_PendingDetach.empty()) && (m_PendingWear.empty()) );
}

// Checked: 2010-07-28 (RLVa-1.2.0i) | Modified: RLVa-1.2.0i
void RlvAttachmentLockWatchdog::onWearAttachment(const LLUUID& idItem, ERlvWearMask eWearAction)
{
    // We only need to keep track of user wears if there's actually anything locked
    RLV_ASSERT(idItem.notNull());
    if ( (idItem.isNull()) || (!isAgentAvatarValid()) || (!gRlvAttachmentLocks.hasLockedAttachmentPoint(RLV_LOCK_ANY)) )
        return;

    // If the attachment point this will end up being attached to is:
    //   - unlocked    : nothing should happen (from RLVa's point of view)
    //   - RLV_LOCK_ADD: the new attachment should get detached and the current one(s) reattached (unless it's currently empty)
    //   - RLV_LOCK_REM:
    //       o eWearAction == RLV_WEAR_ADD     : nothing should happen (from RLVa's point of view)
    //       o eWearAction == RLV_WEAR_REPLACE : examine whether the new attachment can indeed replace/detach the old one
    RlvWearInfo infoWear(idItem, eWearAction);
    RLV_ASSERT( (RLV_WEAR_ADD == eWearAction) || (RLV_WEAR_REPLACE == eWearAction) ); // One of the two, but never both
    for (LLVOAvatar::attachment_map_t::const_iterator itAttachPt = gAgentAvatarp->mAttachmentPoints.begin();
            itAttachPt != gAgentAvatarp->mAttachmentPoints.end(); ++itAttachPt)
    {
        const LLViewerJointAttachment* pAttachPt = itAttachPt->second;
        // We only need to know which attachments were present for RLV_LOCK_ADD locked attachment points (and not RLV_LOCK_REM locked ones)
        if (gRlvAttachmentLocks.isLockedAttachmentPoint(pAttachPt, RLV_LOCK_ADD))
        {
            uuid_vec_t attachObjs;
            for (LLViewerJointAttachment::attachedobjs_vec_t::const_iterator itAttachObj = pAttachPt->mAttachedObjects.begin();
                    itAttachObj != pAttachPt->mAttachedObjects.end(); ++itAttachObj)
            {
                const LLViewerObject* pAttachObj = *itAttachObj;
                if (std::find(m_PendingDetach.begin(), m_PendingDetach.end(), pAttachObj->getAttachmentItemID()) != m_PendingDetach.end())
                    continue;   // Exclude attachments that are pending a force-detach
                attachObjs.push_back(pAttachObj->getAttachmentItemID());
            }
            infoWear.attachPts.insert(std::pair<S32, uuid_vec_t>(itAttachPt->first, attachObjs));
        }
    }

    m_PendingWear.insert(std::pair<LLUUID, RlvWearInfo>(idItem, infoWear));
#ifdef RLV_DEBUG
    infoWear.dumpInstance();
#endif // RLV_RELEASE
    startTimer();
}

// ============================================================================
// RlvWearableLocks member functions
//

RlvWearableLocks gRlvWearableLocks;

// Checked: 2010-03-18 (RLVa-1.2.0c) | Added: RLVa-1.2.0a
void RlvWearableLocks::addWearableTypeLock(LLWearableType::EType eType, const LLUUID& idRlvObj, ERlvLockMask eLock)
{
/*
    // Sanity check - make sure it's an object we know about
    if ( (m_Objects.find(idRlvObj) == m_Objects.end()) || (!idxAttachPt) )
        return; // If (idxAttachPt) == 0 then: (pObj == NULL) || (pObj->isAttachment() == false)
*/

    // NOTE: m_WearableTypeXXX can contain duplicate <eType, idRlvObj> pairs (ie @remoutfit:shirt=n,remoutfit=n from the same object)
    if (eLock & RLV_LOCK_REMOVE)
        m_WearableTypeRem.insert(std::pair<LLWearableType::EType, LLUUID>(eType, idRlvObj));
    if (eLock & RLV_LOCK_ADD)
        m_WearableTypeAdd.insert(std::pair<LLWearableType::EType, LLUUID>(eType, idRlvObj));
}

// Charis: safety net, see the header
void RlvWearableLocks::removeAllLocksOf(const LLUUID& idRlvObj)
{
    for (rlv_wearabletypelock_map_t* pMap : { &m_WearableTypeRem, &m_WearableTypeAdd })
    {
        for (rlv_wearabletypelock_map_t::iterator it = pMap->begin(); it != pMap->end(); )
        {
            if (it->second == idRlvObj) it = pMap->erase(it); else ++it;
        }
    }
}

// Checked: 2010-03-19 (RLVa-1.2.0c) | Added: RLVa-1.2.0a
bool RlvWearableLocks::canRemove(LLWearableType::EType eType) const
{
    // NOTE: we return true if the wearable type has at least one wearable that can be removed by the user
    for (U32 idxWearable = 0, cntWearable = gAgentWearables.getWearableCount(eType); idxWearable < cntWearable; idxWearable++)
        if (!isLockedWearable(gAgentWearables.getViewerWearable(eType, idxWearable)))
            return true;
    return false;
}

// Checked: 2010-03-19 (RLVa-1.2.0c) | Added: RLVa-1.2.0a
bool RlvWearableLocks::hasLockedWearable(LLWearableType::EType eType) const
{
    // NOTE: we return true if there is at least 1 non-removable wearable currently worn on this wearable type
    for (U32 idxWearable = 0, cntWearable = gAgentWearables.getWearableCount(eType); idxWearable < cntWearable; idxWearable++)
        if (isLockedWearable(gAgentWearables.getViewerWearable(eType, idxWearable)))
            return true;
    return false;
}

// Checked: 2010-03-19 (RLVa-1.2.0a) | Added: RLVa-1.2.0a
bool RlvWearableLocks::isLockedWearableExcept(const LLViewerWearable* pWearable, const LLUUID& idRlvObj) const
{
    if (idRlvObj.isNull())
        return isLockedWearable(pWearable);

    // TODO-RLVa: [RLVa-1.2.1] We don't have the ability to lock a specific wearable yet so rewrite this when we do
    return (pWearable) && (isLockedWearableTypeExcept(pWearable->getType(), RLV_LOCK_REMOVE, idRlvObj));
}

// Checked: 2010-03-19 (RLVa-1.2.0a) | Added: RLVa-1.2.0a
bool RlvWearableLocks::isLockedWearableTypeExcept(LLWearableType::EType eType, ERlvLockMask eLock, const LLUUID& idRlvObj) const
{
    if (idRlvObj.isNull())
        return isLockedWearableType(eType, eLock);

    // Loop over every object that marked the specified wearable type eLock type locked and skip over anything owned by idRlvObj
    if (eLock & RLV_LOCK_REMOVE)
    {
        for (rlv_wearabletypelock_map_t::const_iterator itWearableType = m_WearableTypeRem.lower_bound(eType),
                endWearableType = m_WearableTypeRem.upper_bound(eType); itWearableType != endWearableType; ++itWearableType)
        {
            if (itWearableType->second != idRlvObj)
                return true;
        }
    }
    if (eLock & RLV_LOCK_ADD)
    {
        for (rlv_wearabletypelock_map_t::const_iterator itWearableType = m_WearableTypeAdd.lower_bound(eType),
                endWearableType = m_WearableTypeAdd.upper_bound(eType); itWearableType != endWearableType; ++itWearableType)
        {
            if (itWearableType->second != idRlvObj)
                return true;
        }
    }
    return false;
}

// Checked: 2010-03-18 (RLVa-1.2.0c) | Added: RLVa-1.2.0a
void RlvWearableLocks::removeWearableTypeLock(LLWearableType::EType eType, const LLUUID& idRlvObj, ERlvLockMask eLock)
{
/*
    // Sanity check - make sure it's an object we know about
    if ( (m_Objects.find(idRlvObj) == m_Objects.end()) || (!idxAttachPt) )
        return; // If (idxAttachPt) == 0 then: (pObj == NULL) || (pObj->isAttachment() == false)
*/

    if (eLock & RLV_LOCK_REMOVE)
    {
        RLV_ASSERT( m_WearableTypeRem.lower_bound(eType) != m_WearableTypeRem.upper_bound(eType) ); // The lock should always exist
        for (rlv_wearabletypelock_map_t::iterator itWearableType = m_WearableTypeRem.lower_bound(eType),
                endWearableType = m_WearableTypeRem.upper_bound(eType); itWearableType != endWearableType; ++itWearableType)
        {
            if (idRlvObj == itWearableType->second)
            {
                m_WearableTypeRem.erase(itWearableType);
                break;
            }
        }
    }
    if (eLock & RLV_LOCK_ADD)
    {
        RLV_ASSERT( m_WearableTypeAdd.lower_bound(eType) != m_WearableTypeAdd.upper_bound(eType) ); // The lock should always exist
        for (rlv_wearabletypelock_map_t::iterator itWearableType = m_WearableTypeAdd.lower_bound(eType),
                endWearableType = m_WearableTypeAdd.upper_bound(eType); itWearableType != endWearableType; ++itWearableType)
        {
            if (idRlvObj == itWearableType->second)
            {
                m_WearableTypeAdd.erase(itWearableType);
                break;
            }
        }
    }
}

// ============================================================================
// RlvFolderLocks member functions
//

class RlvLockedDescendentsCollector : public LLInventoryCollectFunctor
{
public:
    RlvLockedDescendentsCollector(int eSourceTypeMask, RlvFolderLocks::ELockPermission ePermMask, ERlvLockMask eLockTypeMask)
        : m_eSourceTypeMask(eSourceTypeMask), m_ePermMask(ePermMask), m_eLockTypeMask(eLockTypeMask) {}
    /*virtual*/ ~RlvLockedDescendentsCollector() {}
    /*virtual*/ bool operator()(LLInventoryCategory* pFolder, LLInventoryItem* pItem)
    {
        return (pFolder) && (RlvFolderLocks::instance().isLockedFolderEntry(pFolder->getUUID(), m_eSourceTypeMask, m_ePermMask, m_eLockTypeMask));
    }
protected:
    RlvFolderLocks::ELockPermission m_ePermMask;
    int             m_eSourceTypeMask;
    ERlvLockMask    m_eLockTypeMask;
};

// Checked: 2011-03-28 (RLVa-1.3.0g) | Modified: RLVa-1.3.0g
RlvFolderLocks::RlvFolderLocks()
    : m_fLookupDirty(false), m_RootLockType(RLV_LOCK_NONE), m_cntLockAdd(0), m_cntLockRem(0)
{
    LLOutfitObserver::instance().addCOFChangedCallback(boost::bind(&RlvFolderLocks::onNeedsLookupRefresh, this));
    RlvInventory::instance().addSharedRootIDChangedCallback(boost::bind(&RlvFolderLocks::onNeedsLookupRefresh, this));
}

// Checked: 2011-03-27 (RLVa-1.3.0g) | Modified: RLVa-1.3.0g
void RlvFolderLocks::addFolderLock(const folderlock_source_t& lockSource, ELockPermission ePerm, ELockScope eScope,
                                   const LLUUID& idRlvObj, ERlvLockMask eLockType)
{
    // Sanity check - eLockType can be RLV_LOCK_ADD or RLV_LOCK_REMOVE but not both
    RLV_ASSERT( (RLV_LOCK_ADD == eLockType) || (RLV_LOCK_REMOVE == eLockType) );

    // NOTE: m_FolderXXX can contain duplicate folderlock_descr_t
    m_FolderLocks.push_back(new folderlock_descr_t(idRlvObj, eLockType, lockSource, ePerm, eScope));

    if (PERM_DENY == ePerm)
    {
        if (RLV_LOCK_REMOVE == eLockType)
            m_cntLockRem++;
        else if (RLV_LOCK_ADD == eLockType)
            m_cntLockAdd++;
    }

    if (!m_AttachmentChangeConnection.connected())
        m_AttachmentChangeConnection = gRlvAttachmentChanged.connect(boost::bind(&RlvFolderLocks::onNeedsLookupRefresh, this));
    m_fLookupDirty = true;
}

// Checked: 2011-03-28 (RLVa-1.3.0g) | Modified: RLVa-1.3.0g
bool RlvFolderLocks::getLockedFolders(const folderlock_source_t& lockSource, LLInventoryModel::cat_array_t& lockFolders) const
{
    auto cntFolders = lockFolders.size();
    switch (lockSource.first)
    {
        case ST_ATTACHMENT:
            {
                RLV_ASSERT(typeid(LLUUID) == lockSource.second.type())
                const LLViewerObject* pObj = gObjectList.findObject(boost::get<LLUUID>(lockSource.second));
                if ( (pObj) && (pObj->isAttachment()) )
                {
                    const LLViewerInventoryItem* pItem = gInventory.getItem(pObj->getAttachmentItemID());
                    if ( (pItem) && (RlvInventory::instance().isSharedFolder(pItem->getParentUUID())) )
                    {
                        LLViewerInventoryCategory* pItemFolder = gInventory.getCategory(pItem->getParentUUID());
                        if (pItemFolder)
                            lockFolders.push_back(pItemFolder);
                    }
                }
            }
            break;
        case ST_FOLDER:
            {
                RLV_ASSERT(typeid(LLUUID) == lockSource.second.type())
                LLViewerInventoryCategory* pFolder = gInventory.getCategory(boost::get<LLUUID>(lockSource.second));
                if (pFolder)
                    lockFolders.push_back(pFolder);
            }
            break;
        case ST_ROOTFOLDER:
            {
                LLViewerInventoryCategory* pFolder = gInventory.getCategory(gInventory.getRootFolderID());
                if (pFolder)
                    lockFolders.push_back(pFolder);
            }
            break;
        case ST_SHAREDPATH:
            {
                RLV_ASSERT(typeid(std::string) == lockSource.second.type())
                LLViewerInventoryCategory* pSharedFolder = RlvInventory::instance().getSharedFolder(boost::get<std::string>(lockSource.second));
                if (pSharedFolder)
                    lockFolders.push_back(pSharedFolder);
            }
            break;
        case ST_ATTACHMENTPOINT:
        case ST_WEARABLETYPE:
            {
                RLV_ASSERT( ((ST_ATTACHMENTPOINT == lockSource.first) && (typeid(S32) == lockSource.second.type())) ||
                            ((ST_WEARABLETYPE == lockSource.first) && (typeid(LLWearableType::EType) == lockSource.second.type())) );

                uuid_vec_t idItems;
                if (ST_ATTACHMENTPOINT == lockSource.first)
                    RlvCommandOptionGetPath::getItemIDs(RlvAttachPtLookup::getAttachPoint(boost::get<S32>(lockSource.second)), idItems);
                else if (ST_WEARABLETYPE == lockSource.first)
                    RlvCommandOptionGetPath::getItemIDs(boost::get<LLWearableType::EType>(lockSource.second), idItems);

                LLInventoryModel::cat_array_t itemFolders;
                if (RlvInventory::instance().getPath(idItems, itemFolders))
                    lockFolders.insert(lockFolders.end(), itemFolders.begin(), itemFolders.end());
            }
            break;
        default:
            return false;
    };
    return cntFolders != lockFolders.size();
}

// Checked: 2011-11-26 (RLVa-1.5.4a) | Modified: RLVa-1.5.4a
bool RlvFolderLocks::getLockedItems(const LLUUID& idFolder, LLInventoryModel::item_array_t& lockItems) const
{
    auto cntItems = lockItems.size();

    LLInventoryModel::cat_array_t folders; LLInventoryModel::item_array_t items;
    LLFindWearablesEx f(true, true);    // Collect all worn items
    RlvInventory::collectDescendentsIf(idFolder, folders, items, false, f, false);     // Charis: depth-limited

    // Generally several of the worn items will belong to the same folder so we'll cache the results of each lookup
    std::map<LLUUID, bool> folderLookups; std::map<LLUUID, bool>::const_iterator itLookup;

    bool fItemLocked = false;
    for (size_t idxItem = 0, cntItem = items.size(); idxItem < cntItem; idxItem++)
    {
        LLViewerInventoryItem* pItem = items.at(idxItem);
        if (LLAssetType::AT_LINK == pItem->getActualType())
            pItem = pItem->getLinkedItem();
        if (!pItem)
            continue;

        // Check the actual item's parent folder
        const LLUUID& idItemParent = RlvInventory::getFoldedParent(pItem->getParentUUID(), true);
        if ((itLookup = folderLookups.find(idItemParent)) != folderLookups.end())
        {
            fItemLocked = itLookup->second;
        }
        else
        {
            fItemLocked = isLockedFolder(idItemParent, RLV_LOCK_REMOVE);
            folderLookups.insert(std::pair<LLUUID, bool>(idItemParent, fItemLocked));
        }

        // Check the parent folders of any links to this item that exist under #RLV
        if (!fItemLocked)
        {
            LLInventoryModel::item_array_t itemLinks;
            LLInventoryModel::cat_array_t cats;
            LLLinkedItemIDMatches f(pItem->getUUID());
            RlvInventory::collectDescendentsIf(RlvInventory::instance().getSharedRootID(), cats, itemLinks, LLInventoryModel::EXCLUDE_TRASH, f, false);     // Charis: depth-limited

            for (LLInventoryModel::item_array_t::iterator itItemLink = itemLinks.begin();
                    (itItemLink < itemLinks.end()) && (!fItemLocked); ++itItemLink)
            {
                LLViewerInventoryItem* pItemLink = *itItemLink;

                const LLUUID& idItemLinkParent = (pItemLink) ? RlvInventory::getFoldedParent(pItemLink->getParentUUID(), true) : LLUUID::null;
                if ((itLookup = folderLookups.find(idItemLinkParent)) != folderLookups.end())
                {
                    fItemLocked = itLookup->second;
                }
                else
                {
                    fItemLocked = isLockedFolder(idItemLinkParent, RLV_LOCK_REMOVE);
                    folderLookups.insert(std::pair<LLUUID, bool>(idItemLinkParent, fItemLocked));
                }
            }
        }

        if (fItemLocked)
            lockItems.push_back(pItem);
    }

    return cntItems != lockItems.size();
}

// Checked: 2011-03-29 (RLVa-1.3.0g) | Added: RLVa-1.3.0g
bool RlvFolderLocks::hasLockedFolderDescendent(const LLUUID& idFolder, int eSourceTypeMask, ELockPermission ePermMask,
                                               ERlvLockMask eLockTypeMask, bool fCheckSelf) const
{
    if (!hasLockedFolder(eLockTypeMask))
        return false;
    if (m_fLookupDirty)
        refreshLockedLookups();
    if ( (fCheckSelf) && (isLockedFolderEntry(idFolder, eSourceTypeMask, ePermMask, RLV_LOCK_ANY)) )
        return true;

    LLInventoryModel::cat_array_t folders; LLInventoryModel::item_array_t items;
    RlvLockedDescendentsCollector f(eSourceTypeMask, ePermMask, eLockTypeMask);
    RlvInventory::collectDescendentsIf(idFolder, folders, items, false, f, false);  // Charis: depth-limited, see rlvinventory.cpp
    return !folders.empty();
}

// Checked: 2011-03-29 (RLVa-1.3.0g) | Added: RLVa-1.3.0g
bool RlvFolderLocks::isLockedFolderEntry(const LLUUID& idFolder, int eSourceTypeMask, ELockPermission ePermMask, ERlvLockMask eLockTypeMask) const
{
    for (folderlock_map_t::const_iterator itFolderLock = m_LockedFolderMap.lower_bound(idFolder),
            endFolderLock = m_LockedFolderMap.upper_bound(idFolder); itFolderLock != endFolderLock; ++itFolderLock)
    {
        const folderlock_descr_t* pLockDescr = itFolderLock->second;
        if ( (pLockDescr->lockSource.first & eSourceTypeMask) && (pLockDescr->eLockPermission & ePermMask) &&
             (pLockDescr->eLockType & eLockTypeMask) )
        {
            return true;
        }
    }
    return false;
}

// Checked: 2011-03-27 (RLVa-1.3.0g) | Modified: RLVa-1.3.0g
bool RlvFolderLocks::isLockedFolder(LLUUID idFolder, ERlvLockMask eLockTypeMask, int eSourceTypeMask, std::list<folderlock_source_t>* pLockSourceList) const
{
    // Sanity check - if there are no folder locks then we don't have to actually do anything
    if (!hasLockedFolder(eLockTypeMask))
        return false;

    // Folded folders will be locked if their "parent" is locked
    idFolder = RlvInventory::getFoldedParent(idFolder, true);
    if (idFolder.isNull())
        return false;

    if (m_fLookupDirty)
        refreshLockedLookups();

    // Walk up the folder tree and check if anything has 'idFolder' locked
    std::list<LLUUID> idsRlvObjRem, idsRlvObjAdd; const LLUUID& idFolderRoot = gInventory.getRootFolderID(); LLUUID idFolderCur = idFolder;
    for (S32 nDepth = 0; (idFolderRoot != idFolderCur) && (nDepth < RlvInventory::cMaxFolderDepth); nDepth++)     // Charis: depth limit (a parent cycle can't hang)
    {
        // Iterate over any folder locks for 'idFolderCur'
        for (folderlock_map_t::const_iterator itFolderLock = m_LockedFolderMap.lower_bound(idFolderCur),
                endFolderLock = m_LockedFolderMap.upper_bound(idFolderCur); itFolderLock != endFolderLock; ++itFolderLock)
        {
            const folderlock_descr_t* pLockDescr = itFolderLock->second;

            // We can skip over the current lock if:
            //   - the current lock type doesn't match eLockTypeMask
            //   - it's a node lock and the current folder doesn't match
            //   - we encountered a PERM_ALLOW lock from the current lock owner before which supercedes any subsequent locks
            //   - the lock source type doesn't match the mask passed in eSourceTypeMask
            ERlvLockMask eCurLockType = (ERlvLockMask)(pLockDescr->eLockType & eLockTypeMask);
            std::list<LLUUID>* pidRlvObjList = (RLV_LOCK_REMOVE == eCurLockType) ? &idsRlvObjRem : &idsRlvObjAdd;
            if ( (0 == eCurLockType) || ((SCOPE_NODE == pLockDescr->eLockScope) && (idFolder != idFolderCur)) ||
                 (pidRlvObjList->end() != std::find(pidRlvObjList->begin(), pidRlvObjList->end(), pLockDescr->idRlvObj)) ||
                 (0 == (pLockDescr->eLockType & eSourceTypeMask)) )
            {
                continue;
            }

            if (PERM_DENY == pLockDescr->eLockPermission)
            {
                if (pLockSourceList)
                    pLockSourceList->push_back(pLockDescr->lockSource);
                else
                    return true;                                // Folder is explicitly denied, indicate locked folder to our caller (unless it wants a list of all lock sources)
            }
            else if (PERM_ALLOW == pLockDescr->eLockPermission)
            {
                pidRlvObjList->push_back(pLockDescr->idRlvObj); // Folder is explicitly allowed, save the owner so we can skip it from now on
            }
        }

        // Move up to the folder tree
        const LLViewerInventoryCategory* pParent = gInventory.getCategory(idFolderCur);
        idFolderCur = (pParent) ? pParent->getParentUUID() : idFolderRoot;
    }

    if ( (pLockSourceList) && (!pLockSourceList->empty()) )
    {
        // If we're asked to return a list, make sure it's sorted so we can compare them
        pLockSourceList->sort([](const folderlock_source_t& lhs, const folderlock_source_t& rhs) { return lhs.first < rhs.first && lhs.second < rhs.second; });
        return true;
    }

    // If we didn't encounter an explicit deny lock with no exception then the folder is locked if the entire inventory is locked down
    return (m_RootLockType & eLockTypeMask) && (eSourceTypeMask & ST_ROOTFOLDER) && (idsRlvObjRem.empty()) && (idsRlvObjAdd.empty());
}

// Checked: 2010-11-30 (RLVa-1.3.0b) | Added: RLVa-1.3.0b
void RlvFolderLocks::onNeedsLookupRefresh()
{
    // NOTE: when removeFolderLock() removes the last folder lock we still want to refresh everything so mind the conditional OR assignment
    m_fLookupDirty |= !m_FolderLocks.empty();
}

// Checked: 2011-03-27 (RLVa-1.3.0g) | Added: RLVa-1.3.0g
void RlvFolderLocks::refreshLockedLookups() const
{
    //
    // Refresh locked folders
    //
    m_RootLockType = RLV_LOCK_NONE;
    m_LockedFolderMap.clear();
    for (folderlock_list_t::const_iterator itFolderLock = m_FolderLocks.begin(); itFolderLock != m_FolderLocks.end(); ++itFolderLock)
    {
        const folderlock_descr_t* pLockDescr = *itFolderLock;

        LLInventoryModel::cat_array_t lockedFolders; const LLUUID& idFolderRoot = gInventory.getRootFolderID();
        if (getLockedFolders(pLockDescr->lockSource, lockedFolders))
        {
            for (size_t idxFolder = 0, cntFolder = lockedFolders.size(); idxFolder < cntFolder; idxFolder++)
            {
                const LLViewerInventoryCategory* pFolder = lockedFolders.at(idxFolder);
                if (idFolderRoot != pFolder->getUUID())
                    m_LockedFolderMap.insert(std::pair<LLUUID, const folderlock_descr_t*>(pFolder->getUUID(), pLockDescr));
                else if (SCOPE_SUBTREE == pLockDescr->eLockScope)
                    m_RootLockType |= (U32)pLockDescr->eLockType;
            }
        }
    }
    m_fLookupDirty = false;

    //
    // Refresh locked items (iterate over COF and filter out any items residing in a RLV_LOCK_REMOVE locked PERM_DENY folder)
    //
    m_LockedAttachmentRem.clear();
    m_LockedWearableRem.clear();

    LLInventoryModel::item_array_t lockedItems;
    if (getLockedItems(LLAppearanceMgr::instance().getCOF(), lockedItems))
    {
        for (size_t idxItem = 0, cntItem = lockedItems.size(); idxItem < cntItem; idxItem++)
        {
            const LLViewerInventoryItem* pItem = lockedItems.at(idxItem);
            switch (pItem->getType())
            {
                case LLAssetType::AT_BODYPART:
                case LLAssetType::AT_CLOTHING:
                    m_LockedWearableRem.push_back(pItem->getLinkedUUID());
                    break;
                case LLAssetType::AT_OBJECT:
                    m_LockedAttachmentRem.push_back(pItem->getLinkedUUID());
                    break;
                default:
                    RLV_ASSERT(true);
                    break;
            }
        }
    }

    // Remove any duplicate items we may have picked up
    std::sort(m_LockedAttachmentRem.begin(), m_LockedAttachmentRem.end());
    m_LockedAttachmentRem.erase(std::unique(m_LockedAttachmentRem.begin(), m_LockedAttachmentRem.end()), m_LockedAttachmentRem.end());
    std::sort(m_LockedWearableRem.begin(), m_LockedWearableRem.end());
    m_LockedWearableRem.erase(std::unique(m_LockedWearableRem.begin(), m_LockedWearableRem.end()), m_LockedWearableRem.end());
}

// Charis: safety net, see the header
void RlvFolderLocks::removeAllLocksOf(const LLUUID& idRlvObj)
{
    for (folderlock_list_t::iterator it = m_FolderLocks.begin(); it != m_FolderLocks.end(); )
    {
        const folderlock_descr_t* pLockDescr = *it;
        if (pLockDescr->idRlvObj == idRlvObj)
        {
            if (RlvFolderLocks::PERM_DENY == pLockDescr->eLockPermission)
            {
                if (RLV_LOCK_REMOVE == pLockDescr->eLockType)
                    m_cntLockRem--;
                else if (RLV_LOCK_ADD == pLockDescr->eLockType)
                    m_cntLockAdd--;
            }
            delete pLockDescr;
            it = m_FolderLocks.erase(it);
            m_fLookupDirty = true;
        }
        else
            ++it;
    }
}

// Checked: 2011-03-27 (RLVa-1.3.0g) | Modified: RLVa-1.3.0g
void RlvFolderLocks::removeFolderLock(const folderlock_source_t& lockSource, ELockPermission ePerm, ELockScope eScope,
                                      const LLUUID& idRlvObj, ERlvLockMask eLockType)
{
    // Sanity check - eLockType can be RLV_LOCK_ADD or RLV_LOCK_REMOVE but not both
    RLV_ASSERT( (RLV_LOCK_ADD == eLockType) || (RLV_LOCK_REMOVE == eLockType) );

    folderlock_descr_t lockDescr(idRlvObj, eLockType, lockSource, ePerm, eScope); RlvPredValuesEqual<folderlock_descr_t> f = { &lockDescr };
    folderlock_list_t::iterator itFolderLock = std::find_if(m_FolderLocks.begin(), m_FolderLocks.end(), f);
    RLV_ASSERT( m_FolderLocks.end() != itFolderLock  ); // The lock should always exist
    if (m_FolderLocks.end() != itFolderLock)
    {
        delete *itFolderLock;
        m_FolderLocks.erase(itFolderLock);

        if (PERM_DENY == ePerm)
        {
            if (RLV_LOCK_REMOVE == eLockType)
                m_cntLockRem--;
            else if (RLV_LOCK_ADD == eLockType)
                m_cntLockAdd--;
        }
        m_fLookupDirty = true;
    }
}

// ============================================================================

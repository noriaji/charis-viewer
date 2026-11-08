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

// Charis: RlvHandler::onExperienceEvent(), onExperienceAttach() and getAttachmentResourcesCoro() from FS rlvhandler.cpp.
//         Added: the "AttachmentResources" response is checked by rlvValidateAttachmentResources() before use, and the
//         asynchronous callbacks do nothing once RLVa is off or the viewer is logging out or exiting.

#include "llviewerprecompiledheaders.h"

#include "llagent.h"
#include "llappviewer.h"
#include "llcorehttputil.h"
#include "llcoros.h"
#include "llexperiencecache.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"

#include "rlvexperience.h"
#include "rlvhandler.h"

#include <algorithm>

// ============================================================================
// Response check (no viewer state; the selftest builds this function on its own)
//

std::vector<RlvAttachResObject> rlvValidateAttachmentResources(const LLSD& sdResult)
{
    std::vector<RlvAttachResObject> objects;
    if ( (!sdResult.isMap()) || (!sdResult.has("attachments")) )
        return objects;

    const LLSD& sdAttachments = sdResult["attachments"];
    if (!sdAttachments.isArray())
        return objects;

    const size_t cntAttach = std::min(sdAttachments.size(), RLV_ATTACHRES_MAX_ATTACHMENTS);
    for (size_t idxAttach = 0; idxAttach < cntAttach; idxAttach++)
    {
        const LLSD& sdAttach = sdAttachments[idxAttach];
        if ( (!sdAttach.isMap()) || (!sdAttach.has("objects")) )
            continue;

        const LLSD& sdAttachObjects = sdAttach["objects"];
        if (!sdAttachObjects.isArray())
            continue;

        const size_t cntObj = std::min(sdAttachObjects.size(), RLV_ATTACHRES_MAX_OBJECTS);
        for (size_t idxObj = 0; idxObj < cntObj; idxObj++)
        {
            const LLSD& sdObj = sdAttachObjects[idxObj];
            if ( (!sdObj.isMap()) || (!sdObj.has("id")) || (!sdObj.has("name")) )
                continue;

            const LLSD& sdId = sdObj["id"];
            LLUUID idObj;
            if (sdId.isUUID())
                idObj = sdId.asUUID();
            else if ( (!sdId.isString()) || (!idObj.set(sdId.asStringRef(), false)) )
                continue;
            if (idObj.isNull())
                continue;

            const LLSD& sdName = sdObj["name"];
            if ( (!sdName.isString()) || (sdName.asStringRef().size() > RLV_ATTACHRES_MAX_NAME) )
                continue;

            objects.push_back(RlvAttachResObject{ idObj, sdName.asStringRef() });
        }
    }
    return objects;
}

// ============================================================================
// RlvHandler - experience (temporary attachment) handling
//

// Charis: true while an asynchronous experience callback may still act (RLVa on, not logging out or exiting)
static bool rlvCanHandleExperienceCallback()
{
    return (RlvHandler::isEnabled()) && (!LLApp::isExiting()) && (!LLAppViewer::instance()->logoutRequestSent());
}

void RlvHandler::getAttachmentResourcesCoro(const std::string& strUrl)
{
    LLCore::HttpRequest::policy_t httpPolicy(LLCore::HttpRequest::DEFAULT_POLICY_ID);
    LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t httpAdapter = std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("RlvHandler::getAttachmentResourcesCoro", httpPolicy);
    LLCore::HttpRequest::ptr_t httpRequest = std::make_shared<LLCore::HttpRequest>();
    const LLSD sdResult = httpAdapter->getAndSuspend(httpRequest, strUrl);

    // Charis: the viewer may have logged out or quit while the request was out
    if (!rlvCanHandleExperienceCallback())
        return;

    // An HTTP failure leaves the blocked entry unresolved (it expires in onGC()); restrictions are never removed on a guess
    const LLCore::HttpStatus httpStatus = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(sdResult[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);
    if (!httpStatus)
        return;

    const std::vector<RlvAttachResObject> attachObjects = rlvValidateAttachmentResources(sdResult);
    for (const RlvAttachResObject& attachObj : attachObjects)
    {
        const LLUUID& idObj = attachObj.idObject;

        // Charis: an object that is already blocked never resolves a second entry (FS checked this only for objects it could see)
        if (isBlockedObject(idObj))
            continue;

        // If it's an attachment, it should be a temporary one (NOTE: we might catch it before it's had a chance to attach)
        // Charis: only an object known to be a regular attachment is passed over; one that hasn't attached yet (or whose
        //         update hasn't arrived) is still matched by name, so the result doesn't depend on what arrived first (review S-M1)
        const LLViewerObject* pObj = gObjectList.findObject(idObj);
        if ( (pObj) && (pObj->isAttachment()) && (!pObj->isTempAttachment()) )
            continue;

        // Find it by object name (exact match)
        auto itBlockedObj = std::find_if(m_BlockedObjects.begin(), m_BlockedObjects.end(),
            [&attachObj](const rlv_blocked_object_t& entry) {
                return (std::get<0>(entry).isNull()) && (std::get<1>(entry) == attachObj.strName);
            });
        if (m_BlockedObjects.end() != itBlockedObj)
        {
            std::get<0>(*itBlockedObj) = idObj;

            // Charis: the whole linkset (a child prim may hold restrictions too), see RlvHandler::clearLinkset()
            RLV_DEBUGS << "Clearing restrictions from blocked object " << idObj.asString() << RLV_ENDL;
            clearLinkset(idObj, idObj);
            return;
        }
    }
}

void RlvHandler::onExperienceAttach(const LLSD& sdExperience, const std::string& strObjName)
{
    // Charis: the experience lookup may answer after logout
    if (!rlvCanHandleExperienceCallback())
        return;

    // Charis: the maturity is an LLSD integer from the experience service; keep it in U8 range instead of letting it wrap
    const LLSD::Integer nMaturity = llclamp<LLSD::Integer>(sdExperience[LLExperienceCache::MATURITY].asInteger(), 0, 255);
    if (!RlvSettings::isAllowedExperience(sdExperience[LLExperienceCache::EXPERIENCE_ID].asUUID(), static_cast<U8>(nMaturity)))
    {
        addBlockedObject(LLUUID::null, strObjName);

        const std::string strUrl = gAgent.getRegionCapability("AttachmentResources");
        if (!strUrl.empty())
        {
            LLCoros::instance().launch("RlvHandler::getAttachmentResourcesCoro", boost::bind(&RlvHandler::getAttachmentResourcesCoro, this, strUrl));
        }
    }
}

void RlvHandler::onExperienceEvent(const LLSD& sdEvent)
{
    if (!isEnabled())
        return;

    const int nPermission = sdEvent["Permission"].asInteger();
    switch (nPermission)
    {
        case 4: // Attach
            {
                const LLUUID idExperience = sdEvent["public_id"].asUUID();
                const std::string strObjName = sdEvent["ObjectName"].asString();
                LLExperienceCache::instance().get(idExperience, boost::bind(&RlvHandler::onExperienceAttach, this, _1, strObjName));
            }
            break;
        default:
            break;
    }
}

// ============================================================================

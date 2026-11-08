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

#ifndef RLV_ATTACHPT_H
#define RLV_ATTACHPT_H

// Charis: RlvAttachPtLookup moved out of FS rlvlocks.h/.cpp unchanged (it holds no lock state), so rlv-core can name attachment
//         points without the lock types. rlvlocks.h (rlv-locks) includes this header instead of declaring the class.

#include "llstl.h"
#include "llvoavatarself.h"

#include "rlvdefines.h"
#include "rlvcommon.h"

#include <map>
#include <string>

class LLInventoryCategory;
class LLInventoryItem;
class LLViewerJointAttachment;
class LLViewerObject;

// ============================================================================
// RlvAttachPtLookup class declaration
//

class RlvAttachPtLookup
{
public:
    static LLViewerJointAttachment* getAttachPoint(S32 idxAttachPt);
    static LLViewerJointAttachment* getAttachPoint(const std::string& strText);
    static LLViewerJointAttachment* getAttachPoint(const LLInventoryItem* pItem);

    static S32 getAttachPointIndex(std::string strText);
    static S32 getAttachPointIndex(const LLViewerObject* pAttachObj);
    static S32 getAttachPointIndex(const LLViewerJointAttachment* pAttachPt);
    static S32 getAttachPointIndex(const LLInventoryCategory* pFolder);
    static S32 getAttachPointIndex(const LLInventoryItem* pItem, bool fFollowLinks = true);

    static bool hasAttachPointName(const LLInventoryItem* pItem) { return (0 != getAttachPointIndex(pItem)); }
private:
    static S32 getAttachPointIndexLegacy(const LLInventoryCategory* pFolder);

public:
    static void initLookupTable();
private:
    static std::map<std::string, S32> m_AttachPtLookupMap;
};

// ============================================================================
// RlvAttachPtLookup inlined member functions
//

// Checked: 2010-11-30 (RLVa-1.4.0b) | Added: RLVa-1.4.0b
inline LLViewerJointAttachment* RlvAttachPtLookup::getAttachPoint(S32 idxAttachPt)
{
    return (isAgentAvatarValid()) ? get_if_there(gAgentAvatarp->mAttachmentPoints, idxAttachPt, (LLViewerJointAttachment*)NULL) : NULL;
}

// Checked: 2010-03-03 (RLVa-1.2.0a) | Modified: RLVa-0.2.0d
inline LLViewerJointAttachment* RlvAttachPtLookup::getAttachPoint(const std::string& strText)
{
    return (isAgentAvatarValid()) ? get_if_there(gAgentAvatarp->mAttachmentPoints, getAttachPointIndex(strText), (LLViewerJointAttachment*)NULL) : NULL;
}

// Checked: 2010-03-03 (RLVa-1.2.0a) | Modified: RLVa-1.0.1b
inline LLViewerJointAttachment* RlvAttachPtLookup::getAttachPoint(const LLInventoryItem* pItem)
{
    return (isAgentAvatarValid()) ? get_if_there(gAgentAvatarp->mAttachmentPoints, getAttachPointIndex(pItem), (LLViewerJointAttachment*)NULL) : NULL;
}

// Checked: 2010-03-03 (RLVa-1.2.0a) | Modified: RLVa-1.2.0a
inline S32 RlvAttachPtLookup::getAttachPointIndex(std::string strText)
{
    // Charis: FS fills the table from LLVOAvatarSelf::initInstance(); here it is filled on first use (no official hook)
    if (m_AttachPtLookupMap.empty())
        initLookupTable();

    LLStringUtil::toLower(strText);
    RLV_ASSERT(m_AttachPtLookupMap.size() > 0);
    std::map<std::string, S32>::const_iterator itAttachPt = m_AttachPtLookupMap.find(strText);
    return (itAttachPt != m_AttachPtLookupMap.end()) ? itAttachPt->second : 0;
}

// Checked: 2010-03-03 (RLVa-1.2.0a) | Modified: RLVa-0.2.0d
inline S32 RlvAttachPtLookup::getAttachPointIndex(const LLViewerObject* pObj)
{
    // Charis: the official LLViewerObject::getAttachmentState() is not const (FS made it const); it only reads the state
    return (pObj) ? ATTACHMENT_ID_FROM_STATE(const_cast<LLViewerObject*>(pObj)->getAttachmentState()) : 0;
}

// ============================================================================

#endif // RLV_ATTACHPT_H

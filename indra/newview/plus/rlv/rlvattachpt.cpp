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

// Charis: the RlvAttachPtLookup members of FS rlvlocks.cpp, unchanged

#include "llviewerprecompiledheaders.h"
#include "llagent.h"
#include "llinventorymodel.h"
#include "llviewerinventory.h"

#include "rlvattachpt.h"
#include "rlvhelper.h"

// ============================================================================
// RlvAttachPtLookup member functions
//

std::map<std::string, S32> RlvAttachPtLookup::m_AttachPtLookupMap;

// Checked: 2010-03-02 (RLVa-1.2.0a) | Modified: RLVa-1.2.0a
void RlvAttachPtLookup::initLookupTable()
{
    static bool fInitialized = false;
    if (!fInitialized)
    {
        if ( (gAgentAvatarp) && (gAgentAvatarp->mAttachmentPoints.size() > 0) )
        {
            std::string strAttachPtName;
            for (LLVOAvatar::attachment_map_t::const_iterator itAttach = gAgentAvatarp->mAttachmentPoints.begin();
                     itAttach != gAgentAvatarp->mAttachmentPoints.end(); ++itAttach)
            {
                const LLViewerJointAttachment* pAttachPt = itAttach->second;
                if (pAttachPt)
                {
                    strAttachPtName = pAttachPt->getName();
                    LLStringUtil::toLower(strAttachPtName);
                    m_AttachPtLookupMap.insert(std::pair<std::string, S32>(strAttachPtName, itAttach->first));

                    // HACK: the RLV API randomly renames "Avatar Center" to "Root" so make sure we add it (but keep the official name)
                    if ("avatar center" == strAttachPtName)
                    {
                        m_AttachPtLookupMap.insert(std::pair<std::string, S32>("root", itAttach->first));
                    }
                }
            }
            fInitialized = true;
        }
    }
}

// Checked: 2010-03-03 (RLVa-1.2.0a) | Added: RLVa-0.2.2a
S32 RlvAttachPtLookup::getAttachPointIndex(const LLViewerJointAttachment* pAttachPt)
{
    if (isAgentAvatarValid())
    {
        for (LLVOAvatar::attachment_map_t::const_iterator itAttach = gAgentAvatarp->mAttachmentPoints.begin();
                itAttach != gAgentAvatarp->mAttachmentPoints.end(); ++itAttach)
        {
            if (itAttach->second == pAttachPt)
                return itAttach->first;
        }
    }
    return 0;
}

// Checked: 2010-03-03 (RLVa-1.2.0a) | Modified: RLVa-1.0.1b
S32 RlvAttachPtLookup::getAttachPointIndex(const LLInventoryCategory* pFolder)
{
    if (!pFolder)
        return 0;

    // RLVa-1.0.1 added support for legacy matching (See http://rlva.catznip.com/blog/2009/07/attachment-point-naming-convention/)
    if (RlvSettings::getEnableLegacyNaming())
        return getAttachPointIndexLegacy(pFolder);

    // Otherwise the only valid way to specify an attachment point in a folder name is: ^\.\(\s+attachpt\s+\)
    std::string::size_type idxMatch;
    std::string strAttachPt = rlvGetFirstParenthesisedText(pFolder->getName(), &idxMatch);
    LLStringUtil::trim(strAttachPt);

    return ( (1 == idxMatch) && (RLV_FOLDER_PREFIX_HIDDEN == pFolder->getName().at(0)) ) ? getAttachPointIndex(strAttachPt) : 0;
}

// Checked: 2010-03-03 (RLVa-1.2.0a) | Modified: RLVa-1.2.0a
S32 RlvAttachPtLookup::getAttachPointIndex(const LLInventoryItem* pItem, bool fFollowLinks /*=true*/)
{
    // Sanity check - if it's not an object then it can't have an attachment point
    if ( (!pItem) || (LLAssetType::AT_OBJECT != pItem->getType()) )
        return 0;

    // If the item is an inventory link then we first examine its target before examining the link itself
    if ( (LLAssetType::AT_LINK == pItem->getActualType()) && (fFollowLinks) )
    {
        S32 idxAttachPt = getAttachPointIndex(gInventory.getItem(pItem->getLinkedUUID()), false);
        if (idxAttachPt)
            return idxAttachPt;
    }

    // The attachment point should be placed at the end of the item's name, surrounded by parenthesis
    // (if there is no such text then strAttachPt will be an empty string which is fine since it means we'll look at the item's parent)
    // (if the item is an inventory link then we only look at its containing folder and never examine its name)
    std::string strAttachPt =
        (LLAssetType::AT_LINK != pItem->getActualType()) ? rlvGetLastParenthesisedText(pItem->getName()) : LLStringUtil::null;
    LLStringUtil::trim(strAttachPt);

    // If the item is modify   : we look at the item's name first and only then at the containing folder
    // If the item is no modify: we look at the containing folder's name first and only then at the item itself
    S32 idxAttachPt = 0;
    if (pItem->getPermissions().allowModifyBy(gAgent.getID()))
    {
        idxAttachPt = (!strAttachPt.empty()) ? getAttachPointIndex(strAttachPt) : 0;
        if (!idxAttachPt)
            idxAttachPt = getAttachPointIndex(gInventory.getCategory(pItem->getParentUUID()));
    }
    else
    {
        idxAttachPt = getAttachPointIndex(gInventory.getCategory(pItem->getParentUUID()));
        if ( (!idxAttachPt) && (!strAttachPt.empty()) )
            idxAttachPt = getAttachPointIndex(strAttachPt);
    }
    return idxAttachPt;
}

// Checked: 2010-03-03 (RLVa-1.2.0a) | Added: RLVa-1.0.1b
S32 RlvAttachPtLookup::getAttachPointIndexLegacy(const LLInventoryCategory* pFolder)
{
    // Hopefully some day this can just be deprecated (see http://rlva.catznip.com/blog/2009/07/attachment-point-naming-convention/)
    if ( (!pFolder) || (pFolder->getName().empty()) )
        return 0;

    // Check for a (...) block *somewhere* in the name
    std::string::size_type idxMatch;
    std::string strAttachPt = rlvGetFirstParenthesisedText(pFolder->getName(), &idxMatch);
    if (!strAttachPt.empty())
    {
        // Could be "(attachpt)", ".(attachpt)" or "Folder name (attachpt)"
        if ( (0 != idxMatch) && ((1 != idxMatch) || (RLV_FOLDER_PREFIX_HIDDEN == pFolder->getName().at(0)) ) && // No '(' or '.(' start
             (idxMatch + strAttachPt.length() + 1 != pFolder->getName().length()) )                             // or there's extra text
        {
            // It's definitely not one of the first two so assume it's the last form (in which case we need the last paranthesised block)
            strAttachPt = rlvGetLastParenthesisedText(pFolder->getName());
        }
    }
    else
    {
        // There's no paranthesised block, but it could still be "attachpt" or ".attachpt" (just strip away the '.' from the last one)
        strAttachPt = pFolder->getName();
        if (RLV_FOLDER_PREFIX_HIDDEN == strAttachPt[0])
            strAttachPt.erase(0, 1);
    }
    return getAttachPointIndex(strAttachPt);
}

// ============================================================================

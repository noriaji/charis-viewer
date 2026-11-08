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


#ifndef RLV_FORCEWEAR_H
#define RLV_FORCEWEAR_H

#include "llsingleton.h"
#include "llinventorymodel.h"
#include "llviewerinventory.h"
#include "llwearabletype.h"

#include "rlvdefines.h"
#include "rlvcommon.h"

// Charis: moved out of rlvhelper.h. isWearingItem() and isStrippable() are defined in rlvwearquery.cpp (they don't need the
// locks), the rest in rlvforcewear.cpp (from FS rlvhelper.cpp "RlvForceWear").

class LLViewerJointAttachment;
class LLViewerObject;
class LLViewerWearable;
class LLWearable;

// ============================================================================
// RlvForceWear
//

class RlvForceWear : public LLSingleton<RlvForceWear>
{
    LLSINGLETON_EMPTY_CTOR(RlvForceWear);

public:
    // Folders
    enum EWearAction { ACTION_WEAR_REPLACE, ACTION_WEAR_ADD, ACTION_REMOVE };
    enum EWearFlags { FLAG_NONE = 0x00, FLAG_MATCHALL = 0x01, FLAG_DEFAULT = FLAG_NONE };
    void forceFolder(const LLViewerInventoryCategory* pFolder, EWearAction eAction, EWearFlags eFlags);

    // Generic
    static bool isWearAction(EWearAction eAction) { return (ACTION_WEAR_REPLACE == eAction) || (ACTION_WEAR_ADD == eAction); }
    static bool isWearableItem(const LLInventoryItem* pItem);
    static bool isWearingItem(const LLInventoryItem* pItem);

    // Nostrip
    static bool isStrippable(const LLUUID& idItem) { return isStrippable(gInventory.getItem(idItem)); }
    static bool isStrippable(const LLInventoryItem* pItem);

    // Attachments
    static bool isForceDetachable(const LLViewerObject* pAttachObj, bool fCheckComposite = true, const LLUUID& idExcept = LLUUID::null);
    static bool isForceDetachable(const LLViewerJointAttachment* pAttachPt, bool fCheckComposite = true, const LLUUID& idExcept = LLUUID::null);
    void forceDetach(const LLViewerObject* pAttachObj);
    void forceDetach(const LLViewerJointAttachment* ptAttachPt);

    // Wearables
    static bool isForceRemovable(const LLViewerWearable* pWearable, bool fCheckComposite = true, const LLUUID& idExcept = LLUUID::null);
    static bool isForceRemovable(LLWearableType::EType wtType, bool fCheckComposite = true, const LLUUID& idExcept = LLUUID::null);
    void forceRemove(const LLViewerWearable* pWearable);
    void forceRemove(LLWearableType::EType wtType);

public:
    void done();
protected:
    void addAttachment(const LLViewerInventoryItem* pItem, EWearAction eAction);
    void remAttachment(const LLViewerObject* pAttachObj);
    void addWearable(const LLViewerInventoryItem* pItem, EWearAction eAction);
    void remWearable(const LLViewerWearable* pWearable);

    // Convenience (prevents long lines that run off the screen elsewhere)
    bool isAddAttachment(const LLViewerInventoryItem* pItem) const
    {
        bool fFound = false;
        for (addattachments_map_t::const_iterator itAddAttachments = m_addAttachments.begin();
                (!fFound) && (itAddAttachments != m_addAttachments.end()); ++itAddAttachments)
        {
            const LLInventoryModel::item_array_t& wearItems = itAddAttachments->second;
            fFound = (std::find_if(wearItems.begin(), wearItems.end(), RlvPredIsEqualOrLinkedItem(pItem)) != wearItems.end());
        }
        return fFound;
    }
    bool isRemAttachment(const LLViewerObject* pAttachObj) const
    {
        return std::any_of(m_remAttachments.begin(), m_remAttachments.end(), [pAttachObj](const LLPointer<LLViewerObject>& p) { return p.get() == pAttachObj; });
    }
    bool isAddWearable(const LLViewerInventoryItem* pItem) const
    {
        bool fFound = false;
        for (addwearables_map_t::const_iterator itAddWearables = m_addWearables.begin();
                (!fFound) && (itAddWearables != m_addWearables.end()); ++itAddWearables)
        {
            const LLInventoryModel::item_array_t& wearItems = itAddWearables->second;
            fFound = (std::find_if(wearItems.begin(), wearItems.end(), RlvPredIsEqualOrLinkedItem(pItem)) != wearItems.end());
        }
        return fFound;
    }
    bool isRemWearable(const LLViewerWearable* pWearable) const
    {
        return std::find(m_remWearables.begin(), m_remWearables.end(), pWearable) != m_remWearables.end();
    }

    /*
     * Pending attachments
     */
public:
    static void updatePendingAttachments();
protected:
    void addPendingAttachment(const LLUUID& idItem, U8 idxPoint);
    void remPendingAttachment(const LLUUID& idItem);

protected:
    typedef std::pair<LLWearableType::EType, LLInventoryModel::item_array_t> addwearable_pair_t;
    typedef std::map<LLWearableType::EType, LLInventoryModel::item_array_t> addwearables_map_t;
    addwearables_map_t               m_addWearables;
    typedef std::pair<S32, LLInventoryModel::item_array_t> addattachment_pair_t;
    typedef std::map<S32, LLInventoryModel::item_array_t> addattachments_map_t;
    addattachments_map_t             m_addAttachments;
    LLInventoryModel::item_array_t   m_addGestures;
    std::vector<LLPointer<LLViewerObject>> m_remAttachments;  // Charis: kept alive (and checked for isDead() in done()) until the queue runs
    std::list<const LLViewerWearable*> m_remWearables;
    LLInventoryModel::item_array_t   m_remGestures;

    typedef std::map<LLUUID, U8> pendingattachments_map_t;
    pendingattachments_map_t         m_pendingAttachments;
};


// Checked: 2010-04-05 (RLVa-1.2.0d) | Modified: RLVa-1.2.0d
inline bool RlvForceWear::isWearableItem(const LLInventoryItem* pItem)
{
    LLAssetType::EType assetType = (pItem) ? pItem->getType() : LLAssetType::AT_NONE;
    return
        (LLAssetType::AT_BODYPART == assetType) || (LLAssetType::AT_CLOTHING == assetType) ||
        (LLAssetType::AT_OBJECT == assetType) || (LLAssetType::AT_GESTURE == assetType);
}

#endif // RLV_FORCEWEAR_H

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

#ifndef RLV_INVENTORY_H
#define RLV_INVENTORY_H

#include "llinventoryfunctions.h"
#include "llinventoryobserver.h"
#include "llsingleton.h"

#include "rlvhelper.h"
#include "rlvattachpt.h"    // Charis: was rlvlocks.h (rlv-core has no lock types; RlvAttachPtLookup and RlvForceWear are on their own)
#include "rlvforcewear.h"

// ============================================================================
// Forward declarations
//

class LLOfferInfo;

// ============================================================================
// RlvInventory class declaration
//

class RlvInventory : public LLSingleton<RlvInventory>, public LLInventoryObserver
{
    LLSINGLETON(RlvInventory);
    ~RlvInventory();

public:
    // LLInventoryObserver override
    /*virtual*/ void changed(U32 mask) override;

    /*
     * #RLV Shared inventory
     */
public:
    typedef boost::signals2::signal<void (void)> callback_signal_t;
    void                        addSharedRootIDChangedCallback(const callback_signal_t::slot_type& cb) { m_OnSharedRootIDChanged.connect(cb); }
    // Find all folders that match a supplied criteria (clears the output array)
    bool                        findSharedFolders(const std::string& strCriteria, LLInventoryModel::cat_array_t& folders) const;
    // Gets the shared path for any shared items present in idItems (clears the output array)
    bool                        getPath(const uuid_vec_t& idItems, LLInventoryModel::cat_array_t& folders) const;
    // Returns a pointer to the shared root folder (if there is one)
    LLViewerInventoryCategory*  getSharedRoot() const;
    const LLUUID&               getSharedRootID() const;
    // Returns a subfolder of idParent that starts with strFolderName (exact match > partial match)
    LLViewerInventoryCategory*  getSharedFolder(const LLUUID& idParent, const std::string& strFolderName, bool fMatchPartial = true) const;
    // Looks up a folder from a path (relative to the shared root)
    LLViewerInventoryCategory*  getSharedFolder(const std::string& strPath, bool fMatchPartial = true) const;
    // Returns the path of the supplied folder (relative to the shared root)
    std::string                 getSharedPath(const LLViewerInventoryCategory* pFolder) const;
    std::string                 getSharedPath(const LLUUID& idFolder) const;
    // Returns true if the supplied folder is a descendent of the #RLV folder
    bool                        isSharedFolder(const LLUUID& idFolder);
    // Returns true if the inventory offer is a "give to #RLV" offer
    bool                        isGiveToRLVOffer(const LLOfferInfo& offerInfo);

    /*
     * Inventory fetching
     */
public:
    void fetchSharedInventory();
    void fetchWornItems();
protected:
    void fetchSharedLinks();

    /*
     * General purpose helper functions
     */
public:
    // Returns the number of sub-folders of the specified folder
    static S32 getDirectDescendentsFolderCount(const LLInventoryCategory* pFolder);
    // Returns the number of direct descendents of the specified folder that have the specified type asset type
    static S32 getDirectDescendentsItemCount(const LLInventoryCategory* pFolder, LLAssetType::EType filterType);
    // Returns the folder the items of the specified folder should folded into (can be the folder itself)
    static const LLUUID& getFoldedParent(const LLUUID& idFolder, bool fCheckComposite);
    // A "folded folder" is a folder whose items logically belong to the grandparent rather than the parent
    static bool isFoldedFolder(const LLInventoryCategory* pFolder, bool fCheckComposite);
    // Charis: FS patches LLInventoryModel::collectDescendentsIf() with a 'follow_folder_links' argument; the official one has none.
    //         This is the FS version (same visiting order) with a depth limit so a malformed or cyclic tree can't run away.
    //         Use it where FS passes follow_folder_links (@getinvworn, force wear) and for searches below #RLV.
    static void collectDescendentsIf(const LLUUID& idFolder, LLInventoryModel::cat_array_t& folders, LLInventoryModel::item_array_t& items,
                                     bool fIncludeTrash, LLInventoryCollectFunctor& f, bool fFollowFolderLinks = false);
    // Charis: deepest folder level below the starting folder that searches and path walks visit
    static constexpr S32 cMaxFolderDepth = 64;
protected:
    static void collectDescendentsIfDepth(const LLUUID& idFolder, const LLUUID& idTrash, LLInventoryModel::cat_array_t& folders,
                                          LLInventoryModel::item_array_t& items, LLInventoryCollectFunctor& f, bool fFollowFolderLinks, S32 nDepth);

    /*
     * Member variables
     */
protected:
    bool                m_fFetchStarted;            // true if we fired off an inventory fetch
    bool                m_fFetchComplete;           // true if everything was fetched
    mutable LLUUID      m_idRlvRoot;
    callback_signal_t   m_OnSharedRootIDChanged;

private:
    static const std::string cstrSharedRoot;
    friend class RlvSharedInventoryFetcher;
};

// ============================================================================
// RlvRenameOnWearObserver - Handles "auto-rename-on-wear" for (linked) items living under #RLV
//

class RlvRenameOnWearObserver : public LLInventoryFetchItemsObserver
{
public:
    RlvRenameOnWearObserver(const LLUUID& idItem) : LLInventoryFetchItemsObserver(idItem) {}
    virtual ~RlvRenameOnWearObserver() {}
    virtual void done();
protected:
    void doneIdle();
    static void onCategoryCreate(const LLUUID& idFolder, const LLUUID idItem);
};

// ============================================================================
// "Give to #RLV" helper classes
//

class RlvGiveToRLVOffer
{
protected:
    RlvGiveToRLVOffer() {}
    virtual ~RlvGiveToRLVOffer() {}
protected:
    bool         createDestinationFolder(const std::string& strPath);
    virtual void onDestinationCreated(const LLUUID& idDestFolder, const std::string& strName) = 0;
    // Charis: fRenameTried stops a second rename when the inventory keeps a different name than requested (FS can loop there)
    static void  moveAndRename(const LLUUID& idFolder, const LLUUID& idDestination, const std::string& strName, LLPointer<LLInventoryCallback> cb, bool fRenameTried = false);
    // Charis: checks (and corrects) the folder names of a split "#RLV/~..." path; false if any is unusable
    static bool  isValidDestPath(std::list<std::string>& destPath);
private:
    static void  onCategoryCreateCallback(LLUUID idFolder, RlvGiveToRLVOffer* pInstance);

private:
    std::list<std::string> m_DestPath;
};

// [See LLInventoryTransactionObserver which says it's not entirely complete?]
// NOTE: the offer may span mulitple BulkUpdateInventory messages so if we're no longer around then (ie due to "delete this") then
//       we'll miss those; in this specific case we only care about the *folder* though and that will be in the very first message
// Charis: the official LLInventoryModel has no getTransactionId() (an FS patch), so the offered folder is recognised by name among
//         the folders added while the offer waits (at most cTimeout seconds after it was accepted): the exact name that was offered
//         (strOfferName, see getOfferedFolderName()), or - when the caller has no name - any "#RLV/~..." name. A folder is claimed
//         by one offer only.
class RlvGiveToRLVTaskOffer : public LLInventoryObserver, RlvGiveToRLVOffer
{
public:
    RlvGiveToRLVTaskOffer(const LLUUID& idTransaction, const std::string& strOfferName = LLStringUtil::null);
    ~RlvGiveToRLVTaskOffer() override;
    void changed(U32 mask) override;

    // Returns the folder name of an object's inventory offer from its LLOfferInfo::mDesc ("'#RLV/~name'  ( http://... )"), or an
    // empty string if mDesc doesn't have that form
    static std::string getOfferedFolderName(const std::string& strOfferDesc);
protected:
    void done();
    void doneIdle();
    void onDestinationCreated(const LLUUID& idDestFolder, const std::string& strName) override;
    void onOfferCompleted(const LLUUID& idOfferedFolder);

protected:
    typedef std::vector<LLUUID> folder_ref_t;
    folder_ref_t m_Folders;
    LLUUID       m_idTransaction;   // Kept from FS (unused until the inventory model reports transaction IDs)
    std::string  m_strOfferName;    // Name of the offered folder (empty: unknown)
    F64          m_tsCreated;

    static constexpr F64 cTimeout = 120.0;
    static uuid_set_t    s_ClaimedFolders;
};

class RlvGiveToRLVAgentOffer : public LLInventoryFetchDescendentsObserver, RlvGiveToRLVOffer
{
public:
    RlvGiveToRLVAgentOffer(const LLUUID& idFolder) : RlvGiveToRLVOffer(), LLInventoryFetchDescendentsObserver(idFolder) {}
    /*virtual*/ ~RlvGiveToRLVAgentOffer() {}
public:
    /*virtual*/ void done();
protected:
                void doneIdle();
    /*virtual*/ void onDestinationCreated(const LLUUID& idFolder, const std::string& strName);
};

// ============================================================================
// RlvCriteriaCategoryCollector - Criteria based folder matching filter used by @findfolder and @findfolders
//

class RlvCriteriaCategoryCollector : public LLInventoryCollectFunctor
{
public:
    RlvCriteriaCategoryCollector(const std::string& strCriteria)
    {
        std::string::size_type idxIt, idxLast = 0;
        while (idxLast < strCriteria.length())
        {
            idxIt = strCriteria.find("&&", idxLast);
            if (std::string::npos == idxIt)
                idxIt = strCriteria.length();
            if (idxIt != idxLast)
                m_Criteria.push_back(strCriteria.substr(idxLast, idxIt - idxLast));
            idxLast = idxIt + 2;
        }
    }
    virtual ~RlvCriteriaCategoryCollector() {}

    virtual bool operator()(LLInventoryCategory* pFolder, LLInventoryItem* pItem)
    {
        if ( (!pFolder) || (m_Criteria.empty()) )   // We're only interested in matching folders, we don't care about items
            return false;                           // (if there are no criteria then we don't want to return a match)

        std::string strFolderName = pFolder->getName();
        LLStringUtil::toLower(strFolderName);

        // NOTE: hidden or "give to #RLV" folders can never be a match
        if ( (strFolderName.empty()) ||
             (RLV_FOLDER_PREFIX_HIDDEN == strFolderName[0]) || (RLV_FOLDER_PREFIX_PUTINV == strFolderName[0]) ||
             (std::string::npos != strFolderName.find_first_of(RLV_FOLDER_INVALID_CHARS)) )
        {
            return false;
        }

        for (std::list<std::string>::const_iterator itCrit = m_Criteria.begin(); itCrit != m_Criteria.end(); ++itCrit)
            if (std::string::npos == strFolderName.find(*itCrit))
                return false;
        return true;
    }

protected:
    std::list<std::string> m_Criteria;
};

// ============================================================================
// RlvWearableItemCollector - Inventory item filter used by attach/detach/attachall/detachall/getinvworn
//

class RlvWearableItemCollector : public LLInventoryCollectFunctor
{
public:
    RlvWearableItemCollector(const LLInventoryCategory* pFolder, RlvForceWear::EWearAction eAction, RlvForceWear::EWearFlags eFlags);
    virtual ~RlvWearableItemCollector() {}

    virtual bool operator()(LLInventoryCategory* pFolder, LLInventoryItem* pItem);

    const LLUUID&             getFoldedParent(const LLUUID& idFolder) const;
    RlvForceWear::EWearAction getWearAction(const LLUUID& idFolder) const;
    RlvForceWear::EWearAction getWearActionNormal(const LLInventoryCategory* pFolder);
    RlvForceWear::EWearAction getWearActionFolded(const LLInventoryCategory* pFolder);
    bool                      isLinkedFolder(const LLUUID& idFolder);
protected:
    const LLUUID              m_idFolder;
    RlvForceWear::EWearAction m_eWearAction;
    RlvForceWear::EWearFlags  m_eWearFlags;

    bool onCollectFolder(const LLInventoryCategory* pFolder);
    bool onCollectItem(const LLInventoryItem* pItem);

    std::list<LLUUID> m_Folded;
    std::list<LLUUID> m_Linked;
    std::list<LLUUID> m_Wearable;
    std::map<LLUUID, LLUUID>                    m_FoldingMap;
    std::map<LLUUID, RlvForceWear::EWearAction> m_WearActionMap;

    std::string m_strWearAddPrefix;
    std::string m_strWearReplacePrefix;
};

// ============================================================================
// General purpose inventory helper classes
//

class RlvIsLinkType : public LLInventoryCollectFunctor
{
public:
    RlvIsLinkType() {}
    /*virtual*/ ~RlvIsLinkType() {}
    virtual bool operator()(LLInventoryCategory* pFolder, LLInventoryItem* pItem) { return (pItem) && (pItem->getIsLinkType()); }
};

// If the attachment item is linked in COF but isn't worn (or just detached) the function will return inconsistent information
class RlvFindAttachmentsOnPoint : public LLInventoryCollectFunctor
{
public:
    RlvFindAttachmentsOnPoint(const LLViewerJointAttachment* pAttachPt) : m_pAttachPt(pAttachPt) {}
    /*virtual*/ ~RlvFindAttachmentsOnPoint() {}
    virtual bool operator()(LLInventoryCategory* pFolder, LLInventoryItem* pItem);
protected:
    const LLViewerJointAttachment* m_pAttachPt;
};

// ============================================================================
// RlvInventory inlined member functions
//

// Checked: 2011-03-28 (RLVa-1.3.0g) | Modified: RLVa-1.3.0g
inline LLViewerInventoryCategory* RlvInventory::getSharedRoot() const
{
    const LLUUID& idRlvRoot = getSharedRootID();
    return (idRlvRoot.notNull()) ? gInventory.getCategory(idRlvRoot) : NULL;
}

// Checked: 2011-11-26 (RLVa-1.5.4a) | Added: RLVa-1.5.4a
inline const LLUUID& RlvInventory::getFoldedParent(const LLUUID& idFolder, bool fCheckComposite)
{
    LLViewerInventoryCategory* pFolder = gInventory.getCategory(idFolder);
    // Charis: bounded walk (a cyclic parent chain in a damaged model would otherwise never end)
    for (S32 nDepth = 0; (pFolder) && (isFoldedFolder(pFolder, fCheckComposite)); nDepth++)
    {
        if (nDepth >= cMaxFolderDepth)
            return LLUUID::null;
        pFolder = gInventory.getCategory(pFolder->getParentUUID());
    }
    return (pFolder) ? pFolder->getUUID() : LLUUID::null;
}

// Checked: 2011-11-26 (RLVa-1.5.4a) | Added: RLVa-1.5.4a
inline std::string RlvInventory::getSharedPath(const LLUUID& idFolder) const
{
    return getSharedPath(gInventory.getCategory(idFolder));
}

// Checked: 2010-03-19 (RLVa-1.2.0a) | Modified: RLVa-1.2.0a
inline bool RlvInventory::isFoldedFolder(const LLInventoryCategory* pFolder, bool fCheckComposite)
{
    // Charis: an empty folder name made FS's getName().at(0) throw; check the first character without at()
    return
      // If legacy naming isn't enabled we can return early if the folder name doesn't start with a '.' (= the most common case)
      (pFolder) && (!pFolder->getName().empty()) &&
      ( (RlvSettings::getEnableLegacyNaming()) || (RLV_FOLDER_PREFIX_HIDDEN == pFolder->getName()[0]) ) &&
      (
        // .(<attachpt>) type folder
        (0 != RlvAttachPtLookup::getAttachPointIndex(pFolder))
        // .(nostrip) folder
        || ( (pFolder) && (".(" RLV_FOLDER_FLAG_NOSTRIP ")" == pFolder->getName()) )
        // Composite folder (if composite folders are enabled and we're asked to look for them)
        #ifdef RLV_EXPERIMENTAL_COMPOSITEFOLDERS
        || ( (fCheckComposite) && (RlvSettings::getEnableComposites()) &&
             (pFolder) && (RLV_FOLDER_PREFIX_HIDDEN == pFolder->getName().at(0)) && (isCompositeFolder(pFolder)) )
        #endif // RLV_EXPERIMENTAL_COMPOSITEFOLDERS
      );
}

// Checked: 2010-08-29 (RLVa-1.2.0c) | Added: RLVa-1.2.0c
inline bool RlvInventory::isSharedFolder(const LLUUID& idFolder)
{
    const LLViewerInventoryCategory* pRlvRoot = getSharedRoot();
    return (pRlvRoot) ? (pRlvRoot->getUUID() != idFolder) && (gInventory.isObjectDescendentOf(idFolder, pRlvRoot->getUUID())) : false;
}

// ============================================================================
// RlvWearableItemCollector inlined member functions
//

// Checked: 2010-09-30 (RLVa-1.2.1d) | Added: RLVa-1.2.1d
inline bool RlvWearableItemCollector::isLinkedFolder(const LLUUID& idFolder)
{
    return (!m_Linked.empty()) && (m_Linked.end() != std::find(m_Linked.begin(), m_Linked.end(), idFolder));
}

// ============================================================================

#endif // RLV_INVENTORY_H

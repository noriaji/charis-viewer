/**
 * @file outfitbackup.cpp
 * @brief A local copy of the agent's outfit and attachment placements,
 *        offered back at login when the server has an older one.
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

// Why: the viewer writes outfit changes to the server at once (links in the
// Current Outfit Folder), yet after a crash or a forced logout (a failed
// teleport) the next login can come back with the outfit of long before, and
// edited attachments back where they were. A successful teleport or a normal
// logout keeps them. So the server seems to make a session's changes stick at
// those points only; the viewer cannot change that, but it can keep its own
// copy.
//
// What it does:
// - While logged in, every POLL_SECONDS: the links of the Current Outfit
//   Folder (target, description - the layer order - and type) and, for each
//   worn attachment the agent may modify, the position, rotation and size of
//   its root and of each child prim (in link order). Written to the account's
//   folder (one file per grid) when something changed, marked "unconfirmed":
//   a change of the folder at once, placements at most every WRITE_GAP_SECONDS.
// - Marked "confirmed" on arrival in a region (a teleport or a crossing) and
//   when the agent logs out normally; left unconfirmed on a crash or a forced
//   disconnect.
// - At login, once the outfit is loaded and the attachments have settled: if
//   the outfit differs from the copy and the copy is newer (a higher folder
//   version than the server sent at login, before the viewer's own changes) or
//   unconfirmed with the same version, or attachment placements differ and the
//   copy was unconfirmed, asks whether to put the copy back. A server folder
//   newer than the copy's means the copy missed the last change (a crash before
//   it was written): the outfit is not offered then. Yes: the folder is
//   replaced with the copy's links (items no longer in inventory left out; body
//   parts the copy lacks kept from the current outfit), then the placements are
//   sent once the attachments are worn. No: the current state becomes the copy.
// - If the copy's links are not in the folder by RESTORE_WAIT_SECONDS (judged
//   by the links, not the version alone), putting it back failed: that is told,
//   and the copy is kept - offered again at the next login, its version kept
//   abreast of the folder's - until the outfit changes. Should the links arrive
//   later, the restore goes on from there.
// - Nothing is recorded from login until that is settled, so the old state the
//   server sent cannot overwrite the copy.
//
// Placements are only restored on objects the agent may modify. Children are
// matched by link order and checked by shape (pcode, mesh or sculpt id, face
// count); if they do not match, only the root's position and rotation are put
// back (its size would move the children).

#include "llviewerprecompiledheaders.h"

#include "plus.h"

#include "outfitbackup.h"

#include "llagent.h"
#include "llappearancemgr.h"
#include "llappviewer.h"
#include "llcallbacklist.h"
#include "lldate.h"
#include "llinventorymodel.h"
#include "llinventoryobserver.h"
#include "llnotificationsutil.h"
#include "llsdserialize.h"
#include "llselectmgr.h"            // UPD_* flags of MultipleObjectUpdate
#include "llstartup.h"
#include "lltrans.h"
#include "llviewercontrol.h"
#include "llviewerinventory.h"
#include "llviewerjointattachment.h"
#include "llviewernetwork.h"        // LLGridManager
#include "llviewerobject.h"
#include "llviewerregion.h"
#include "llvoavatarself.h"
#include "llvolume.h"
#include "message.h"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <map>
#include <set>
#include <vector>

#include "plus/rlv/rlvhooks.h"

namespace
{
    constexpr char FILE_NAME[]          = "plus_outfit_backup";   // in the account's folder; ".<grid>" off the main grid, ".xml"
    constexpr S32  FILE_VERSION         = 1;

    constexpr F32  POLL_SECONDS         = 2.f;     // recording
    constexpr F32  WAIT_POLL_SECONDS    = 1.f;     // waiting for attachments
    constexpr F32  WRITE_GAP_SECONDS    = 5.f;     // at most one write in this time (scripts that move prims)
    constexpr F64  SETTLE_SECONDS       = 10.0;    // an attachment the same object with the same prims this long
    constexpr F32  LOGIN_WAIT_SECONDS   = 60.f;    // for the attachments, before comparing anyway
    constexpr F32  FETCH_WAIT_SECONDS   = 90.f;    // for the copy's items (the fetch itself gives up at 60)
    constexpr F32  RESTORE_WAIT_SECONDS = 90.f;    // for the restored attachments
    constexpr F32  RESTORE_START_SECONDS = 3.f;    // before the appearance update can have started
    constexpr F32  HOLD_SECONDS         = 10.f;    // after placements are sent, for the simulator's updates

    constexpr F32  POS_EPSILON          = 0.0005f; // m
    constexpr F32  ROT_MIN_DOT          = 0.99999f;

    struct Prim
    {
        S32         pcode = 0;
        LLUUID      sculpt;          // mesh or sculpt asset, null for plain prims
        S32         faces = 0;
        LLVector3   pos;             // root: offset from the attachment point; child: from the root
        LLQuaternion rot;
        LLVector3   scale;
    };

    struct Placement
    {
        Prim                root;
        std::vector<Prim>   children;   // link order (2, 3, ...)
    };

    struct Link
    {
        LLUUID              id;         // the linked item (or folder, for the base outfit)
        std::string         name;
        std::string         desc;       // "@N": the layer order of clothing
        S32                 type = LLAssetType::AT_LINK;

        bool operator<(const Link& other) const
        {
            return id != other.id ? id < other.id : desc < other.desc;
        }
        bool operator==(const Link& other) const
        {
            return id == other.id && desc == other.desc;
        }
    };

    struct Record
    {
        bool                            confirmed = true;
        S32                             cof_version = LLViewerInventoryCategory::VERSION_UNKNOWN;
        std::vector<Link>               outfit;     // sorted
        std::map<LLUUID, Placement>     attachments; // by the attachment's inventory item
    };

    enum class Stage
    {
        WaitLogin,      // until the outfit is loaded
        WaitSettle,     // until the attachments have arrived and settled
        Asking,         // the question is up
        Fetching,       // the copy's items being fetched
        WaitRestored,   // the outfit replaced, the attachments being worn
        Recording,
        Stopped,
    };

    struct Seen
    {
        U32     local_id = 0;
        size_t  children = 0;
        F64     since = 0.0;
    };

    Stage                   sStage = Stage::WaitLogin;
    Record                  sSaved;         // the copy found at login
    Record                  sLast;          // what was last recorded
    bool                    sWriteDue = false;
    LLFrameTimer            sPollTimer;
    LLFrameTimer            sStageTimer;
    LLFrameTimer            sWriteTimer;
    std::map<LLUUID, Seen>  sSeen;          // worn attachments: when they last changed object or prim count
    U32                     sFetchSerial = 0;
    S32                     sMissing = 0;   // the copy's items no longer in inventory
    S32                     sLoginCofVersion = LLViewerInventoryCategory::VERSION_UNKNOWN; // the folder's as the server sent it at login
    S32                     sSlamVersion = LLViewerInventoryCategory::VERSION_UNKNOWN; // the folder's, when the copy was sent
    std::vector<Link>       sSlamSent;      // the links sent for the folder (sorted)
    std::set<LLUUID>        sSlamRemoved;   // what the folder had then and the copy has not
    std::set<LLUUID>        sRestoreKept;   // WaitRestored: the copy's items seen in the folder (or there before and sent again)
    bool                    sHoldRecording = false;  // after putting placements back (sStageTimer)
    bool                    sKeepCopy = false;       // putting the copy back failed: the file is left as it is until the outfit changes
    LLFrameTimer            sKeepCopyTimer;          // since failRestore() (attachments may still arrive)
    boost::signals2::connection sRegionConnection;

    // ------------------------------------------------------------------
    // The file

    std::string gridId()
    {
        std::string grid = LLGridManager::getInstance()->getGridId();
        LLStringUtil::toLower(grid);
        return grid;
    }

    // The account's folder is the same on every grid, and an account has the same id on the main
    // grid and the beta grid (Aditi): a copy from one must not be offered on the other. As the
    // inventory cache does, the main grid's file has no grid name - the name it had before there
    // were others, so a copy written then is still found - and the others' have theirs.
    std::string filePath()
    {
        const std::string& dir = gDirUtilp->getLindenUserDir();
        if (dir.empty())
        {
            return std::string();
        }
        std::string name = FILE_NAME;
        if (!LLGridManager::getInstance()->isInProductionGrid())
        {
            std::string grid = gridId();
            for (char& c : grid)
            {
                if (!isalnum((unsigned char)c) && c != '.' && c != '-' && c != '_')
                {
                    c = '_';    // a grid's name from a grid list: nothing that leaves the folder
                }
            }
            name += "." + (grid.empty() ? std::string("other") : grid);
        }
        return gDirUtilp->add(dir, name + ".xml");
    }

    LLSD toLLSD(const Prim& prim)
    {
        LLSD sd;
        sd["pcode"]  = prim.pcode;
        sd["sculpt"] = prim.sculpt;
        sd["faces"]  = prim.faces;
        sd["pos"]    = prim.pos.getValue();
        sd["rot"]    = prim.rot.getValue();
        sd["scale"]  = prim.scale.getValue();
        return sd;
    }

    Prim primFromLLSD(const LLSD& sd)
    {
        Prim prim;
        prim.pcode  = sd["pcode"].asInteger();
        prim.sculpt = sd["sculpt"].asUUID();
        prim.faces  = sd["faces"].asInteger();
        prim.pos    = LLVector3(sd["pos"]);
        prim.rot    = LLQuaternion(sd["rot"]);
        prim.scale  = LLVector3(sd["scale"]);
        return prim;
    }

    LLSD toLLSD(const Record& record)
    {
        LLSD sd;
        sd["version"]     = FILE_VERSION;
        sd["agent"]       = gAgent.getID();
        sd["grid"]        = gridId();
        sd["saved"]       = LLDate::now();
        sd["confirmed"]   = record.confirmed;
        sd["cof_version"] = record.cof_version;

        LLSD& outfit = sd["outfit"] = LLSD::emptyArray();
        for (const Link& link : record.outfit)
        {
            LLSD entry;
            entry["id"]   = link.id;
            entry["name"] = link.name;
            entry["desc"] = link.desc;
            entry["type"] = link.type;
            outfit.append(entry);
        }

        LLSD& attachments = sd["attachments"] = LLSD::emptyMap();
        for (const auto& [item_id, placement] : record.attachments)
        {
            LLSD entry;
            entry["root"] = toLLSD(placement.root);
            LLSD& children = entry["children"] = LLSD::emptyArray();
            for (const Prim& child : placement.children)
            {
                children.append(toLLSD(child));
            }
            attachments[item_id.asString()] = entry;
        }
        return sd;
    }

    bool load(Record& record)
    {
        const std::string path = filePath();
        if (path.empty() || !gDirUtilp->fileExists(path))
        {
            return false;
        }
        llifstream in(path.c_str());
        LLSD sd;
        if (!in.is_open() || LLSDParser::PARSE_FAILURE == LLSDSerialize::fromXML(sd, in))
        {
            LL_WARNS("OutfitBackup") << "Cannot read " << path << LL_ENDL;
            return false;
        }
        if (sd["version"].asInteger() != FILE_VERSION || sd["agent"].asUUID() != gAgent.getID())
        {
            LL_WARNS("OutfitBackup") << path << " is of another version or account; ignored" << LL_ENDL;
            return false;
        }
        // Copies written before the grid was in the file name have no grid either (the main grid's
        // name; one written on another grid then cannot be told apart).
        if (sd.has("grid") && sd["grid"].asString() != gridId())
        {
            LL_WARNS("OutfitBackup") << path << " is of the grid " << sd["grid"].asString() << "; ignored" << LL_ENDL;
            return false;
        }

        record = Record();
        record.confirmed   = sd["confirmed"].asBoolean();
        record.cof_version = sd["cof_version"].asInteger();
        for (const LLSD& entry : llsd::inArray(sd["outfit"]))
        {
            Link link;
            link.id   = entry["id"].asUUID();
            link.name = entry["name"].asString();
            link.desc = entry["desc"].asString();
            link.type = entry["type"].asInteger();
            if (link.id.notNull())
            {
                record.outfit.push_back(link);
            }
        }
        std::sort(record.outfit.begin(), record.outfit.end());

        for (const auto& [key, entry] : llsd::inMap(sd["attachments"]))
        {
            Placement placement;
            placement.root = primFromLLSD(entry["root"]);
            for (const LLSD& child : llsd::inArray(entry["children"]))
            {
                placement.children.push_back(primFromLLSD(child));
            }
            record.attachments[LLUUID(key)] = placement;
        }
        LL_INFOS("OutfitBackup") << "Copy found: " << record.outfit.size() << " links (folder version " << record.cof_version
                                 << "), " << record.attachments.size() << " attachment placements, "
                                 << (record.confirmed ? "confirmed" : "unconfirmed") << ", saved " << sd["saved"].asString() << LL_ENDL;
        return true;
    }

    void writeFile(const Record& record)
    {
        const std::string path = filePath();
        if (path.empty())
        {
            return;
        }
        // Written next to the file, then moved over it: a crash while saving leaves the old copy, and a
        // cut-off file (a full disk) never replaces the good one.
        const plus::FileWrite wrote = plus::writeFileReplacing(path, [&](std::ostream& out) { LLSDSerialize::toPrettyXML(toLLSD(record), out); });
        if (wrote == plus::FileWrite::cannot_replace)
        {
            LL_WARNS("OutfitBackup") << "Cannot replace " << path << LL_ENDL;
            return;
        }
        if (wrote != plus::FileWrite::written)
        {
            LL_WARNS("OutfitBackup") << "Cannot write " << plus::tempPathFor(path) << LL_ENDL;
            return;
        }
        sWriteDue = false;
        sWriteTimer.reset();
        LL_DEBUGS("OutfitBackup") << "Written: " << record.outfit.size() << " links, " << record.attachments.size()
                                  << " placements, " << (record.confirmed ? "confirmed" : "unconfirmed") << LL_ENDL;
    }

    // The current state into the file, unless the copy is being kept after a failed restore.
    void write(const Record& record)
    {
        if (sKeepCopy)
        {
            LL_DEBUGS("OutfitBackup") << "Not written: the copy is kept after the failed restore" << LL_ENDL;
            return;
        }
        writeFile(record);
    }

    // ------------------------------------------------------------------
    // What the agent wears now

    std::vector<Link> currentOutfit()
    {
        std::vector<Link> links;
        LLInventoryModel::cat_array_t cats;
        LLInventoryModel::item_array_t items;
        gInventory.collectDescendents(LLAppearanceMgr::instance().getCOF(), cats, items, LLInventoryModel::EXCLUDE_TRASH);
        for (const LLPointer<LLViewerInventoryItem>& item : items)
        {
            if (item && item->getIsLinkType())
            {
                links.push_back({ item->getLinkedUUID(), item->getName(), item->getActualDescription(), (S32)item->getActualType() });
            }
        }
        std::sort(links.begin(), links.end());
        return links;
    }

    std::set<LLUUID> linkedIds(const std::vector<Link>& outfit)
    {
        std::set<LLUUID> ids;
        for (const Link& link : outfit)
        {
            ids.insert(link.id);
        }
        return ids;
    }

    // The object links of the current outfit (the attachments it should have).
    std::set<LLUUID> outfitObjects()
    {
        std::set<LLUUID> ids;
        for (const Link& link : currentOutfit())
        {
            const LLViewerInventoryItem* item = gInventory.getItem(link.id);
            if (item && item->getType() == LLAssetType::AT_OBJECT)
            {
                ids.insert(link.id);
            }
        }
        return ids;
    }

    // Worn attachments by their inventory item (temporary ones left out).
    std::map<LLUUID, LLViewerObject*> wornAttachments()
    {
        std::map<LLUUID, LLViewerObject*> worn;
        if (!isAgentAvatarValid())
        {
            return worn;
        }
        for (const auto& [point_id, attachment] : gAgentAvatarp->mAttachmentPoints)
        {
            if (!attachment)
            {
                continue;
            }
            for (const LLPointer<LLViewerObject>& object : attachment->mAttachedObjects)
            {
                if (object && !object->isDead() && !object->isTempAttachment() && object->getAttachmentItemID().notNull())
                {
                    worn[object->getAttachmentItemID()] = object.get();
                }
            }
        }
        return worn;
    }

    void updateSeen(const std::map<LLUUID, LLViewerObject*>& worn)
    {
        const F64 now = LLTimer::getTotalSeconds();
        std::map<LLUUID, Seen> seen;
        for (const auto& [item_id, object] : worn)
        {
            const auto it = sSeen.find(item_id);
            if (it != sSeen.end() && it->second.local_id == object->getLocalID() && it->second.children == object->getChildren().size())
            {
                seen[item_id] = it->second;
            }
            else
            {
                seen[item_id] = { object->getLocalID(), object->getChildren().size(), now };
            }
        }
        sSeen.swap(seen);
    }

    bool settled(const LLUUID& item_id)
    {
        const auto it = sSeen.find(item_id);
        return it != sSeen.end() && LLTimer::getTotalSeconds() - it->second.since >= SETTLE_SECONDS;
    }

    Prim primOf(const LLViewerObject* object)
    {
        Prim prim;
        prim.pcode = object->getPCode();
        if (const LLVolume* volume = object->getVolume())
        {
            prim.sculpt = volume->getParams().getSculptID();
        }
        prim.faces = object->getNumTEs();
        prim.pos   = object->getPosition();
        prim.rot   = object->getRotation();
        prim.scale = object->getScale();
        return prim;
    }

    Placement placementOf(const LLViewerObject* object)
    {
        Placement placement;
        placement.root = primOf(object);
        for (const LLPointer<LLViewerObject>& child : object->getChildren())
        {
            placement.children.push_back(primOf(child.get()));
        }
        return placement;
    }

    bool sameShape(const Prim& a, const Prim& b)
    {
        return a.pcode == b.pcode && a.sculpt == b.sculpt && a.faces == b.faces;
    }

    bool samePosRot(const Prim& a, const Prim& b)
    {
        const F32 dot = a.rot.mQ[VX] * b.rot.mQ[VX] + a.rot.mQ[VY] * b.rot.mQ[VY]
                      + a.rot.mQ[VZ] * b.rot.mQ[VZ] + a.rot.mQ[VW] * b.rot.mQ[VW];
        return dist_vec(a.pos, b.pos) <= POS_EPSILON && fabsf(dot) >= ROT_MIN_DOT;
    }

    bool sameScale(const Prim& a, const Prim& b)
    {
        return dist_vec(a.scale, b.scale) <= POS_EPSILON;
    }

    bool samePrim(const Prim& a, const Prim& b)
    {
        return sameShape(a, b) && samePosRot(a, b) && sameScale(a, b);
    }

    bool samePlacement(const Placement& a, const Placement& b)
    {
        if (!samePrim(a.root, b.root) || a.children.size() != b.children.size())
        {
            return false;
        }
        for (size_t i = 0; i < a.children.size(); ++i)
        {
            if (!samePrim(a.children[i], b.children[i]))
            {
                return false;
            }
        }
        return true;
    }

    bool sameRecord(const Record& a, const Record& b)
    {
        if (a.outfit != b.outfit || a.attachments.size() != b.attachments.size())
        {
            return false;
        }
        for (const auto& [item_id, placement] : a.attachments)
        {
            const auto it = b.attachments.find(item_id);
            if (it == b.attachments.end() || !samePlacement(placement, it->second))
            {
                return false;
            }
        }
        return true;
    }

    // The children of a worn object, in link order, when they are the copy's.
    bool childrenMatch(const Placement& saved, const LLViewerObject* object)
    {
        const LLViewerObject::child_list_t& children = object->getChildren();
        if (children.size() != saved.children.size())
        {
            return false;
        }
        size_t i = 0;
        for (const LLPointer<LLViewerObject>& child : children)
        {
            if (!sameShape(saved.children[i++], primOf(child.get())))
            {
                return false;
            }
        }
        return true;
    }

    // What would be sent for this object: a different root, or (when the children match) a different child.
    bool placementDiffers(const Placement& saved, const LLViewerObject* object)
    {
        const Prim root = primOf(object);
        if (!samePosRot(saved.root, root))
        {
            return true;
        }
        if (!childrenMatch(saved, object))
        {
            return false;   // only the position and rotation would be sent
        }
        if (!sameScale(saved.root, root))
        {
            return true;
        }
        size_t i = 0;
        for (const LLPointer<LLViewerObject>& child : object->getChildren())
        {
            if (!samePrim(saved.children[i++], primOf(child.get())))
            {
                return true;
            }
        }
        return false;
    }

    // The placements of the worn, modifiable attachments of the outfit. An attachment that has not
    // settled yet (just attached, prims still arriving) keeps what was recorded for it before, and
    // so does one still in the outfit that is not here yet (a teleport, a login).
    std::map<LLUUID, Placement> snapshotPlacements(const std::map<LLUUID, Placement>& previous,
                                                   const std::set<LLUUID>& outfit_ids,
                                                   const std::map<LLUUID, LLViewerObject*>& worn)
    {
        std::map<LLUUID, Placement> placements;
        for (const auto& [item_id, object] : worn)
        {
            if (!outfit_ids.count(item_id) || !object->permModify())
            {
                continue;
            }
            if (settled(item_id))
            {
                placements[item_id] = placementOf(object);
            }
            else if (const auto it = previous.find(item_id); it != previous.end())
            {
                placements[item_id] = it->second;
            }
        }
        for (const auto& [item_id, placement] : previous)
        {
            if (outfit_ids.count(item_id) && !worn.count(item_id))
            {
                placements[item_id] = placement;
            }
        }
        return placements;
    }

    Record snapshot(const std::map<LLUUID, Placement>& previous)
    {
        Record record;
        record.cof_version = LLAppearanceMgr::instance().getCOFVersion();
        record.outfit      = currentOutfit();
        const std::map<LLUUID, LLViewerObject*> worn = wornAttachments();
        updateSeen(worn);
        record.attachments = snapshotPlacements(previous, linkedIds(record.outfit), worn);
        return record;
    }

    // ------------------------------------------------------------------
    // Putting placements back

    void packPrim(LLMessageSystem* msg, U32 local_id, U8 type, const Prim& prim)
    {
        U8  data[36];
        S32 offset = 0;
        // The order the simulator reads them in (LLSelectMgr::packMultipleUpdate).
        if (type & UPD_POSITION)
        {
            htolememcpy(&data[offset], prim.pos.mV, MVT_LLVector3, 12);
            offset += 12;
        }
        if (type & UPD_ROTATION)
        {
            const LLVector3 packed = prim.rot.packToVector3();
            htolememcpy(&data[offset], packed.mV, MVT_LLQuaternion, 12);
            offset += 12;
        }
        if (type & UPD_SCALE)
        {
            htolememcpy(&data[offset], prim.scale.mV, MVT_LLVector3, 12);
            offset += 12;
        }
        msg->nextBlockFast(_PREHASH_ObjectData);
        msg->addU32Fast(_PREHASH_ObjectLocalID, local_id);
        msg->addU8Fast(_PREHASH_Type, type);
        msg->addBinaryDataFast(_PREHASH_Data, data, offset);
    }

    class UpdateSender
    {
    public:
        explicit UpdateSender(const LLHost& host) : mHost(host) {}
        ~UpdateSender()
        {
            if (mBlocks)
            {
                gMessageSystem->sendReliable(mHost);
            }
        }

        void add(U32 local_id, U8 type, const Prim& prim)
        {
            LLMessageSystem* msg = gMessageSystem;
            if (!mBlocks)
            {
                msg->newMessageFast(_PREHASH_MultipleObjectUpdate);
                msg->nextBlockFast(_PREHASH_AgentData);
                msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
                msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
            }
            packPrim(msg, local_id, type, prim);
            ++mBlocks;
            if (msg->isSendFull(NULL) || mBlocks >= MAX_BLOCKS)
            {
                msg->sendReliable(mHost);
                mBlocks = 0;
            }
        }

    private:
        static constexpr S32 MAX_BLOCKS = 254;  // as LLSelectMgr's MAX_OBJECTS_PER_PACKET

        LLHost  mHost;
        S32     mBlocks = 0;
    };

    // Sends the copy's placements of the outfit's attachments that differ. Returns the attachments
    // of the outfit that could not be put back (not worn, or not modifiable).
    S32 applyPlacements(const std::map<LLUUID, Placement>& placements, const std::set<LLUUID>& outfit_objects)
    {
        S32 sent = 0, failed = 0;
        for (const auto& [item_id, saved] : placements)
        {
            if (!outfit_objects.count(item_id))
            {
                continue;   // not in the outfit (left out, or the outfit was not put back)
            }
            LLViewerObject* object = isAgentAvatarValid() ? gAgentAvatarp->getWornAttachment(item_id) : nullptr;
            if (!object || object->isDead() || !object->permModify() || !object->getRegion())
            {
                ++failed;
                continue;
            }
            if (!placementDiffers(saved, object))
            {
                continue;
            }

            UpdateSender sender(object->getRegion()->getHost());
            const bool match = childrenMatch(saved, object);
            // The whole object first: moved and turned with its children.
            sender.add(object->getLocalID(), UPD_POSITION | UPD_ROTATION | UPD_LINKED_SETS, saved.root);
            if (match)
            {
                // Then the root's size alone and every child where it was (local to the root), so the
                // result does not depend on how the root's size moved them.
                sender.add(object->getLocalID(), UPD_SCALE, saved.root);
                size_t i = 0;
                for (const LLPointer<LLViewerObject>& child : object->getChildren())
                {
                    sender.add(child->getLocalID(), UPD_POSITION | UPD_ROTATION | UPD_SCALE, saved.children[i++]);
                }
            }
            else
            {
                LL_INFOS("OutfitBackup") << "Attachment " << item_id << ": its prims are not the copy's (" << object->getChildren().size()
                                         << " children, the copy has " << saved.children.size() << "); only its position and rotation put back" << LL_ENDL;
            }
            ++sent;
        }
        LL_INFOS("OutfitBackup") << "Placements put back: " << sent << ", not possible: " << failed << LL_ENDL;
        return failed;
    }

    // ------------------------------------------------------------------
    // Stages

    void startRecording(const Record& record)
    {
        sLast = record;
        write(sLast);
        sStage = Stage::Recording;
        sPollTimer.reset();
        LL_INFOS("OutfitBackup") << "Recording: " << sLast.outfit.size() << " links, " << sLast.attachments.size()
                                 << " placements, " << (sLast.confirmed ? "confirmed" : "unconfirmed") << LL_ENDL;
    }

    void stop(const char* why)
    {
        if (sWriteDue)
        {
            write(sLast);
        }
        sStage = Stage::Stopped;
        LL_INFOS("OutfitBackup") << "Stopped: " << why << " (" << (sLast.confirmed ? "confirmed" : "unconfirmed") << ")" << LL_ENDL;
    }

    void finishRestore()
    {
        const S32 failed = applyPlacements(sSaved.attachments, outfitObjects());

        std::string text = LLTrans::getString("PlusOutfitBackupDone");
        if (sMissing > 0)
        {
            LLStringUtil::format_map_t args;
            args["[COUNT]"] = llformat("%d", sMissing);
            text += "\n" + LLTrans::getString("PlusOutfitBackupMissing", args);
        }
        if (failed > 0)
        {
            LLStringUtil::format_map_t args;
            args["[COUNT]"] = llformat("%d", failed);
            text += "\n" + LLTrans::getString("PlusOutfitBackupNotPlaced", args);
        }
        LLNotificationsUtil::add("SystemMessageTip", LLSD().with("MESSAGE", text));

        // Not on the server for good until the next teleport or logout: still unconfirmed. The
        // placements sent come back from the simulator in a moment: the copy's are kept meanwhile,
        // and recording waits HOLD_SECONDS.
        Record record = snapshot(sSaved.attachments);
        for (auto& [item_id, placement] : record.attachments)
        {
            if (const auto it = sSaved.attachments.find(item_id); it != sSaved.attachments.end())
            {
                placement = it->second;
            }
        }
        record.confirmed = false;
        sKeepCopy = false;      // the copy's links arrived after all (late, after failRestore())
        startRecording(record);
        sStageTimer.reset();
        sHoldRecording = true;
    }

    // The copy's links are in the folder: every link sent is there with its description (the layer
    // order, so a change of the order alone counts too), and what it was to take out is gone. Links
    // the viewer adds or removes on its own (attachments arriving), or another change of the
    // folder's version, do not make it look done.
    bool slamArrived()
    {
        const std::vector<Link> current = currentOutfit();
        if (!std::includes(current.begin(), current.end(), sSlamSent.begin(), sSlamSent.end()))
        {
            return false;
        }
        const std::set<LLUUID> ids = linkedIds(current);
        for (const LLUUID& id : sSlamRemoved)
        {
            if (ids.count(id))
            {
                return false;
            }
        }
        return LLAppearanceMgr::instance().getCOFVersion() != sSlamVersion;
    }

    // After failRestore(): the outfit went from before to now by what is not the agent's change, so
    // the kept copy stays:
    //  - part of the copy's links arriving late (a link the copy sent, or one it took out going);
    //  - object links added, within LOGIN_WAIT_SECONDS of failRestore(): attachments arriving (a heavy
    //    login can bring them after it). Later, an object link added is the agent wearing it; the
    //    links alone cannot tell the two apart.
    // Anything else (clothing or a body part added or taken out, another layer order, an item not
    // known here) is the agent's change. Duplicate links count once (the viewer removes those itself).
    bool notAgentsChange(const std::vector<Link>& before, const std::vector<Link>& now)
    {
        const std::set<Link> before_set(before.begin(), before.end()), now_set(now.begin(), now.end());
        // the copy's links that are there now: one of the same item with another layer order went
        std::set<LLUUID> arrived_ids;
        for (const Link& link : sSlamSent)
        {
            if (now_set.count(link))
            {
                arrived_ids.insert(link.id);
            }
        }
        for (const Link& link : before_set)
        {
            if (!now_set.count(link) && !sSlamRemoved.count(link.id) && !arrived_ids.count(link.id))
            {
                return false;
            }
        }
        const bool attachments_arriving = sKeepCopyTimer.getElapsedTimeF32() < LOGIN_WAIT_SECONDS;
        for (const Link& link : now_set)
        {
            if (before_set.count(link) || std::binary_search(sSlamSent.begin(), sSlamSent.end(), link))
            {
                continue;
            }
            const LLViewerInventoryItem* item = link.type == LLAssetType::AT_LINK ? gInventory.getItem(link.id) : nullptr;
            if (!attachments_arriving || !item || item->getType() != LLAssetType::AT_OBJECT)
            {
                return false;
            }
        }
        return true;
    }

    // While the copy is put back (WaitRestored), the agent's own change of the outfit is told from
    // the folder: an item of the copy that was in it (or was on before and sent again, so it never
    // has to arrive) is gone - taken off, or replaced by wearing something else. Items put on
    // besides do not keep the copy's links from arriving (slamArrived()), so they need nothing.
    // (A wearable the viewer itself takes out, one whose asset never came, looks the same.)
    void startWatchingAgent(const std::vector<Link>& before)
    {
        const std::set<LLUUID> before_ids = linkedIds(before);     // the folder's items when the copy was sent
        sRestoreKept.clear();
        for (const LLUUID& id : linkedIds(sSlamSent))
        {
            if (before_ids.count(id))
            {
                sRestoreKept.insert(id);
            }
        }
    }

    bool agentChangedOutfit()
    {
        const std::set<LLUUID> now_ids = linkedIds(currentOutfit());
        for (const LLUUID& id : sRestoreKept)
        {
            if (!now_ids.count(id))
            {
                return true;
            }
        }
        for (const LLUUID& id : linkedIds(sSlamSent))
        {
            if (now_ids.count(id))
            {
                sRestoreKept.insert(id);
            }
        }
        return false;
    }

    // The agent changed the outfit while the copy was put back: what they wear now is what they
    // want. Not "putting it back failed": no notice, and the copy becomes the current state.
    void dropRestoreForAgent()
    {
        LL_INFOS("OutfitBackup") << "The outfit changed by the agent while the copy was put back; the restore stops, the current state recorded" << LL_ENDL;
        sKeepCopy = false;
        Record record = snapshot(std::map<LLUUID, Placement>());
        record.confirmed = false;   // the session's change: the server keeps it at the next teleport or logout
        startRecording(record);
    }

    // The folder did not get the copy's links: they never reached the server (or not yet - the
    // inventory requests queue up), and the current state is not the copy's. The copy stays in the
    // file, to be offered again at the next login. Recording goes on, but writes only once the agent
    // changes the outfit (newer than the copy); placements alone do not count, as scripts move prims
    // and attachments may still be arriving. If the copy's links come late, the restore goes on.
    void failRestore()
    {
        LL_WARNS("OutfitBackup") << "The copy's links not in the folder after " << RESTORE_WAIT_SECONDS << " s; putting the copy back failed, the copy kept" << LL_ENDL;
        LLNotificationsUtil::add("SystemMessageTip", LLSD().with("MESSAGE", LLTrans::getString("PlusOutfitBackupFailed")));
        sKeepCopy = true;
        sKeepCopyTimer.reset();
        startRecording(snapshot(std::map<LLUUID, Placement>()));
    }

    void restoreOutfit()
    {
        ++sFetchSerial;     // a fetch still running is no longer waited for

        const LLUUID trash_id = gInventory.findCategoryUUIDForType(LLFolderType::FT_TRASH);
        LLSD contents = LLSD::emptyArray();
        std::set<S32> body_types;
        sMissing = 0;
        for (const Link& link : sSaved.outfit)
        {
            std::string name;
            if (link.type == LLAssetType::AT_LINK_FOLDER)
            {
                const LLViewerInventoryCategory* cat = gInventory.getCategory(link.id);
                if (!cat || gInventory.isObjectDescendentOf(link.id, trash_id))
                {
                    continue;   // the base outfit's folder: only the outfit's name
                }
                name = cat->getName();
            }
            else
            {
                const LLViewerInventoryItem* item = gInventory.getItem(link.id);
                if (!item || gInventory.isObjectDescendentOf(link.id, trash_id))
                {
                    ++sMissing;
                    continue;
                }
                name = item->getName();
                if (item->getType() == LLAssetType::AT_BODYPART)
                {
                    body_types.insert(item->getWearableType());
                }
            }
            LLSD entry;
            entry["name"]      = name;
            entry["desc"]      = link.desc;
            entry["linked_id"] = link.id;
            entry["type"]      = link.type;
            contents.append(entry);
        }

        // A shape, skin, hair and eyes are always worn: keep the current ones the copy has none of.
        for (const Link& link : currentOutfit())
        {
            const LLViewerInventoryItem* item = gInventory.getItem(link.id);
            if (item && item->getType() == LLAssetType::AT_BODYPART && body_types.insert(item->getWearableType()).second)
            {
                LLSD entry;
                entry["name"]      = item->getName();
                entry["desc"]      = link.desc;
                entry["linked_id"] = link.id;
                entry["type"]      = link.type;
                contents.append(entry);
            }
        }

        // RLVa: the locks still apply to what is put back (the folder is replaced as it is, not through updateCOF()):
        // what may not be worn is left out, what is locked on stays (research/248 3.4, W8)
        if (plus::rlv::enabled())
        {
            int rlv_kept = 0, rlv_dropped = 0;
            plus::rlv::filterOutfitRestore(contents, rlv_kept, rlv_dropped);
            if (rlv_kept > 0 || rlv_dropped > 0)
            {
                LL_INFOS("OutfitBackup") << "Outfit: RLV locks kept " << rlv_kept << " worn items on, " << rlv_dropped << " items not worn" << LL_ENDL;
                LLNotificationsUtil::add("SystemMessageTip", LLSD().with("MESSAGE",
                    LLTrans::getString("PlusOutfitBackupRlv", LLSD().with("KEPT", rlv_kept).with("DROPPED", rlv_dropped))));
            }
        }

        std::vector<Link> sent;
        for (const LLSD& entry : llsd::inArray(contents))
        {
            sent.push_back({ entry["linked_id"].asUUID(), entry["name"].asString(), entry["desc"].asString(), entry["type"].asInteger() });
        }
        std::sort(sent.begin(), sent.end());
        const std::vector<Link> current = currentOutfit();
        if (sent == current)
        {
            // What differed is no longer in inventory: the folder would not change (nor its version).
            LL_INFOS("OutfitBackup") << "Outfit: nothing to put back (" << sMissing << " items no longer in inventory)" << LL_ENDL;
            finishRestore();
            return;
        }
        const std::set<LLUUID> sent_ids = linkedIds(sent), current_ids = linkedIds(current);
        sSlamSent = sent;
        sSlamRemoved.clear();
        std::set_difference(current_ids.begin(), current_ids.end(), sent_ids.begin(), sent_ids.end(),
                            std::inserter(sSlamRemoved, sSlamRemoved.begin()));
        startWatchingAgent(current);

        LL_INFOS("OutfitBackup") << "Outfit put back: " << contents.size() << " links (" << sSlamRemoved.size() << " taken out), "
                                 << sMissing << " items no longer in inventory" << LL_ENDL;
        // As LLAppearanceMgr::updateCOF() does: the folder's contents replaced, then the appearance updated
        // from it (the copy's layer order as it was, so nothing to enforce).
        sSlamVersion = LLAppearanceMgr::instance().getCOFVersion();
        slam_inventory_folder(LLAppearanceMgr::instance().getCOF(), contents, new LLUpdateAppearanceOnDestroy(false, false));
        sStage = Stage::WaitRestored;
        sStageTimer.reset();
    }

    class FetchThenRestore : public LLInventoryFetchItemsObserver
    {
    public:
        FetchThenRestore(const uuid_vec_t& ids, U32 serial)
        :   LLInventoryFetchItemsObserver(ids),
            mSerial(serial)
        {
        }

        void done() override
        {
            gInventory.removeObserver(this);
            const U32 serial = mSerial;
            doOnIdleOneTime([serial]()
                            {
                                if (serial == sFetchSerial && sStage == Stage::Fetching)
                                {
                                    restoreOutfit();
                                }
                            });
            delete this;
        }

    private:
        U32 mSerial;
    };

    void restore(bool outfit)
    {
        if (!outfit)
        {
            sMissing = 0;
            finishRestore();
            return;
        }

        // The copy's items may not be in the viewer's inventory yet (the background fetch).
        uuid_vec_t ids;
        for (const Link& link : sSaved.outfit)
        {
            if (link.type != LLAssetType::AT_LINK_FOLDER)
            {
                ids.push_back(link.id);
            }
        }
        sStage = Stage::Fetching;
        sStageTimer.reset();
        FetchThenRestore* fetch = new FetchThenRestore(ids, ++sFetchSerial);
        fetch->startFetch();
        if (fetch->isFinished())
        {
            fetch->done();
        }
        else
        {
            gInventory.addObserver(fetch);
        }
    }

    void compareAndAsk()
    {
        const std::vector<Link> outfit = currentOutfit();
        const S32 cof_version = LLAppearanceMgr::instance().getCOFVersion();
        // The version the server sent, before the viewer changed the folder itself at login (links of
        // arriving attachments, layer order, duplicates), each change a version up.
        const S32 server_version = sLoginCofVersion != LLViewerInventoryCategory::VERSION_UNKNOWN ? sLoginCofVersion : cof_version;
        const bool outfit_differs = !sSaved.outfit.empty() && outfit != sSaved.outfit;   // an outfit always has body parts
        // A server folder newer than the copy's has a change the copy missed (made just before a
        // crash, before it was written): putting the copy back would undo it.
        const bool ask_outfit = outfit_differs
                             && (sSaved.cof_version > server_version || (!sSaved.confirmed && sSaved.cof_version == server_version));

        S32 placements = 0;
        for (const auto& [item_id, saved] : sSaved.attachments)
        {
            if (!isAgentAvatarValid())
            {
                break;      // no avatar (logging out): no placements to compare
            }
            const LLViewerObject* object = gAgentAvatarp->getWornAttachment(item_id);
            if (object && object->permModify() && placementDiffers(saved, object))
            {
                ++placements;
            }
        }
        const bool ask_placements = placements > 0 && !sSaved.confirmed;

        LL_INFOS("OutfitBackup") << "Login: outfit " << (outfit_differs ? "differs" : "same") << " (folder version " << server_version
                                 << " at login, " << cof_version << " now, the copy's " << sSaved.cof_version << "), " << placements << " placements differ, the copy "
                                 << (sSaved.confirmed ? "confirmed" : "unconfirmed") << LL_ENDL;

        if (!ask_outfit && !ask_placements)
        {
            startRecording(snapshot(std::map<LLUUID, Placement>()));
            return;
        }

        std::string changes;
        if (ask_outfit)
        {
            std::set<LLUUID> now_ids = linkedIds(outfit), saved_ids = linkedIds(sSaved.outfit), differ;
            std::set_symmetric_difference(now_ids.begin(), now_ids.end(), saved_ids.begin(), saved_ids.end(),
                                          std::inserter(differ, differ.begin()));
            LLStringUtil::format_map_t args;
            args["[COUNT]"] = llformat("%d", (S32)differ.size());
            changes += "\n" + LLTrans::getString(differ.empty() ? "PlusOutfitBackupOrder" : "PlusOutfitBackupItems", args);
        }
        if (placements > 0)
        {
            LLStringUtil::format_map_t args;
            args["[COUNT]"] = llformat("%d", placements);
            changes += "\n" + LLTrans::getString("PlusOutfitBackupPlaces", args);
        }
        const std::string message = LLTrans::getString("PlusOutfitBackupAsk") + changes + "\n\n" + LLTrans::getString("PlusOutfitBackupAskQuestion");

        sStage = Stage::Asking;
        LLNotificationsUtil::add("GenericAlertYesCancel", LLSD().with("MESSAGE", message), LLSD(),
                                 [ask_outfit](const LLSD& notification, const LLSD& response)
                                 {
                                     if (sStage != Stage::Asking)
                                     {
                                         return;
                                     }
                                     if (LLNotificationsUtil::getSelectedOption(notification, response) == 0)
                                     {
                                         LL_INFOS("OutfitBackup") << "Putting the copy back" << LL_ENDL;
                                         restore(ask_outfit);
                                     }
                                     else
                                     {
                                         LL_INFOS("OutfitBackup") << "The copy declined; the current outfit is kept" << LL_ENDL;
                                         startRecording(snapshot(std::map<LLUUID, Placement>()));
                                     }
                                 });
    }

    // The worn attachments of the outfit have all arrived and settled.
    bool attachmentsSettled(const std::set<LLUUID>& wanted)
    {
        const std::map<LLUUID, LLViewerObject*> worn = wornAttachments();
        updateSeen(worn);
        for (const LLUUID& item_id : wanted)
        {
            if (!settled(item_id))
            {
                return false;
            }
        }
        return true;
    }

    void record()
    {
        // Mid-change: the folder and the worn objects do not agree yet.
        if (gAgent.getTeleportState() != LLAgent::TELEPORT_NONE
            || !gAgent.getRegion()
            || LLAppearanceMgr::instance().isInUpdateAppearanceFromCOF()
            || LLAppearanceMgr::instance().getCOFVersion() == LLViewerInventoryCategory::VERSION_UNKNOWN)
        {
            return;
        }
        Record now = snapshot(sLast.attachments);
        const bool outfit_changed = now.outfit != sLast.outfit;
        const bool folder_changed = outfit_changed || now.cof_version != sLast.cof_version;
        if (sKeepCopy && outfit_changed)
        {
            if (slamArrived())
            {
                // The copy's links came after failRestore() gave up: the placements go back once worn.
                LL_INFOS("OutfitBackup") << "The copy's links arrived late; the restore goes on" << LL_ENDL;
                startWatchingAgent(now.outfit);     // they are all there: any of them gone is the agent's
                sStage = Stage::WaitRestored;
                sStageTimer.reset();
                sPollTimer.reset();
                return;
            }
            if (notAgentsChange(sLast.outfit, now.outfit))
            {
                // Attachments or the copy's links arriving late: the copy is still offered at the
                // next login, as the failed restore said.
                LL_INFOS("OutfitBackup") << "Links arrived late, not the agent's change; the copy kept since the failed restore stays" << LL_ENDL;
            }
            else
            {
                sKeepCopy = false;
                LL_INFOS("OutfitBackup") << "The outfit changed: the copy kept since the failed restore is replaced" << LL_ENDL;
            }
        }
        if (sKeepCopy && now.cof_version > sSaved.cof_version)
        {
            // The folder's version passed the copy's with the outfit unchanged (the viewer's own
            // changes): the copy takes that version, unconfirmed, so that it is still offered at the
            // next login.
            sSaved.cof_version = now.cof_version;
            sSaved.confirmed   = false;
            writeFile(sSaved);
        }
        if (!sameRecord(now, sLast))
        {
            now.confirmed = false;
            sLast = now;
            if (!sKeepCopy)
            {
                sWriteDue = true;
            }
        }
        else if (folder_changed)
        {
            // The folder's version moved with the same links: the file keeps the version up to date
            // (compared with the server's at login).
            sLast.cof_version = now.cof_version;
            if (!sKeepCopy)
            {
                sWriteDue = true;
            }
        }
        // A change of the folder is written at once: a crash just after it must not leave the
        // previous outfit in the file. Placements wait for the gap (scripts that move prims).
        if (sWriteDue && (folder_changed || sWriteTimer.getElapsedTimeF32() >= WRITE_GAP_SECONDS))
        {
            write(sLast);
        }
    }

    // A teleport or a crossing arrived: the server keeps what the session changed up to here.
    void onRegionChanged()
    {
        if (sStage == Stage::Recording && !sKeepCopy && (!sLast.confirmed || sWriteDue))
        {
            sLast.confirmed = true;
            write(sLast);
            LL_INFOS("OutfitBackup") << "Region changed: the copy confirmed" << LL_ENDL;
        }
    }

    void onIdle(void*)
    {
        if (sStage == Stage::Stopped)
        {
            return;
        }
        if (LLApp::isExiting() || gDoDisconnect)
        {
            stop(gDoDisconnect ? "disconnected" : "exiting");
            return;
        }
        static LLCachedControl<bool> backup_on(gSavedSettings, "PlusOutfitBackup", false);  // read every frame while recording (fix-pipeleak)

        switch (sStage)
        {
        case Stage::WaitLogin:
            if (sLoginCofVersion == LLViewerInventoryCategory::VERSION_UNKNOWN)
            {
                // The first version known is the server's (the inventory skeleton or the folder's
                // fetch), before the viewer's own changes of the folder at login.
                sLoginCofVersion = LLAppearanceMgr::instance().getCOFVersion();
            }
            if (LLStartUp::getStartupState() != STATE_STARTED || !isAgentAvatarValid() || !gAgent.getRegion()
                || LLAppearanceMgr::instance().getCOFVersion() == LLViewerInventoryCategory::VERSION_UNKNOWN
                || LLAppearanceMgr::instance().isInUpdateAppearanceFromCOF())
            {
                return;
            }
            if (!backup_on)
            {
                stop("off");
                return;
            }
            sRegionConnection = gAgent.addRegionChangedCallback(&onRegionChanged);
            if (load(sSaved))
            {
                sStage = Stage::WaitSettle;
                sStageTimer.reset();
            }
            else
            {
                startRecording(snapshot(std::map<LLUUID, Placement>()));
            }
            return;

        case Stage::WaitSettle:
            if (sPollTimer.getElapsedTimeF32() < WAIT_POLL_SECONDS)
            {
                return;
            }
            sPollTimer.reset();
            if (!LLAppearanceMgr::instance().isInUpdateAppearanceFromCOF()
                && (attachmentsSettled(outfitObjects()) || sStageTimer.getElapsedTimeF32() >= LOGIN_WAIT_SECONDS))
            {
                compareAndAsk();
            }
            return;

        case Stage::Asking:
            return;

        case Stage::Fetching:
            if (sStageTimer.getElapsedTimeF32() >= FETCH_WAIT_SECONDS)
            {
                LL_WARNS("OutfitBackup") << "The copy's items did not all arrive; putting back what is here" << LL_ENDL;
                restoreOutfit();
            }
            return;

        case Stage::WaitRestored:
            if (sPollTimer.getElapsedTimeF32() < WAIT_POLL_SECONDS || sStageTimer.getElapsedTimeF32() < RESTORE_START_SECONDS)
            {
                return;
            }
            sPollTimer.reset();
            if (agentChangedOutfit())
            {
                dropRestoreForAgent();
                return;
            }
            {
                const bool slammed = slamArrived();
                if (sStageTimer.getElapsedTimeF32() >= RESTORE_WAIT_SECONDS)
                {
                    if (slammed)
                    {
                        finishRestore();    // some attachments did not settle: what is here is placed
                    }
                    else
                    {
                        failRestore();
                    }
                }
                else if (slammed && !LLAppearanceMgr::instance().isInUpdateAppearanceFromCOF() && attachmentsSettled(outfitObjects()))
                {
                    finishRestore();
                }
            }
            return;

        case Stage::Recording:
            if (LLAppViewer::instance()->logoutRequestSent())
            {
                // A normal logout: the server keeps the session's changes. The world is still here.
                record();
                sLast.confirmed = true;
                sWriteDue = true;
                stop("logout");
                return;
            }
            if (!backup_on)
            {
                stop("turned off");
                return;
            }
            if (sHoldRecording)
            {
                if (sStageTimer.getElapsedTimeF32() < HOLD_SECONDS)
                {
                    return;
                }
                sHoldRecording = false;
            }
            if (sPollTimer.getElapsedTimeF32() >= POLL_SECONDS || (sWriteDue && sWriteTimer.getElapsedTimeF32() >= WRITE_GAP_SECONDS))
            {
                sPollTimer.reset();
                record();
            }
            return;

        case Stage::Stopped:
            return;
        }
    }

    // The folder's version as the server sent it: the first one known (from the inventory skeleton,
    // or the folder's fetch when the cached one was out of date). Idle callbacks only start once
    // the world is up, when the viewer may have changed the folder already (updateAppearanceFromCOF
    // at login, links of arriving attachments); inventory observers hear of it from the start.
    class LoginVersionObserver : public LLInventoryObserver
    {
    public:
        void changed(U32) override
        {
            if (!LLAppearanceMgr::instanceExists())
            {
                return;
            }
            const S32 version = LLAppearanceMgr::instance().getCOFVersion();
            if (version == LLViewerInventoryCategory::VERSION_UNKNOWN)
            {
                return;
            }
            if (sLoginCofVersion == LLViewerInventoryCategory::VERSION_UNKNOWN)   // not taken by onIdle() first
            {
                sLoginCofVersion = version;
                LL_INFOS("OutfitBackup") << "Folder version at login: " << version << LL_ENDL;
            }
            gInventory.removeObserver(this);    // notifyObservers() allows it (as FetchThenRestore::done())
            delete this;
        }
    };

    void init()
    {
        gIdleCallbacks.addFunction(&onIdle, nullptr);
        gInventory.addObserver(new LoginVersionObserver());
    }

    plus::FeatureRegistration sRegistration({ "outfitbackup", &init });
}

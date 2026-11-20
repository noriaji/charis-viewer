/**
 * @file attachdiag.cpp
 * @brief A census of the attachments of the avatars in view (research/355 stage 0 (5.1)).
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

#include "attachdiag.h"

#include "llcharacter.h"
#include "llrhi.h"
#include "llviewerjointattachment.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llvocache.h"
#include "llvoavatar.h"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace plus::attachdiag
{
    const bool sOn = LLRHI::envSet("LLRHI_ATTACH_DIAG");
}

namespace
{
    constexpr F64 REPORT_SECONDS = 10.0;
    constexpr size_t WORST_LINES = 5;       // the avatars in view missing the most, one line each

    // What a report keeps of an avatar from one report to the next: the same count as GrayWire's (llgraywire.cpp,
    // noteListedAttachments, research/277 A; its AvatarEntry is in an unnamed namespace and is written by its own
    // looks, so the formula is copied here). The list is not sent again when an attachment is only taken off, so
    // a list longer than the most of it seen worn at once since the list last changed is what is missing; one that
    // was all there once and then fewer is not. Sampled every REPORT_SECONDS, so a short peak between two reports
    // is not seen. Unlike GrayWire's (keyed by the avatar's address, so that an avatar made again starts again) it is
    // keyed by the avatar's ID, with the object and its local ID kept to tell an avatar made again (which starts again:
    // one that was all there, dropped from the interest list and made again between two reports, is missing what the
    // new one does not have yet).
    struct Seen
    {
        const LLVOAvatar* object = nullptr;     // the avatar object and local ID this was counted for
        U32 local_id = 0;
        U32 listed = 0;
        U32 digest = 0;         // the XOR of the listed IDs' LLUUID::getCRC32: which are listed, not only how many
        U32 seen = 0;           // the most worn at once since the list changed (found in the list by ID, once any were)
        bool by_id = false;     // one was found in the list by its item or object ID once (so the IDs compare)
        U32 report = 0;         // the report that saw it last (0: new; an avatar a report does not find is dropped)
        U32 first = 0;          // the report that saw the avatar first
        U32 since = 0;          // the report from which this list (digest) has been the avatar's
    };

    // One other avatar, counted. The pointers are valid within one report only.
    struct Record
    {
        LLVOAvatar* avatar = nullptr;
        LLViewerRegion* region = nullptr;
        U32 local_id = 0;
        S32 rez = -1;               // LLVOAvatar::mLastRezzedStatus: what the REZTIME lines count
        bool in_view = false;       // its extents are in the main camera's frustum
        bool has_list = false;      // the region has sent its list (mSimAttachments is not empty)
        U32 listed = 0;             // L: listed on a point it has that is not a HUD's
        U32 listed_other = 0;       // listed on a HUD's point, or on a point it does not have
        U32 worn = 0;               // W: worn, not a HUD's
        U32 matched = 0;            // of those worn, found in the list by item or object ID on a listed point (by_item + by_object)
        U32 by_item = 0;            // ... of those, found by the object's item ID (getAttachmentItemID)
        U32 by_object = 0;          // ... and found by its object ID (getID), when the item ID was not in the list
        U32 pending = 0;            // objects waiting for their drawable (mPendingAttachment, HUD's too)
        U32 seen = 0;               // the most worn at once since the list changed (Seen::seen)
        U32 missing = 0;            // L - seen when seen < L, else 0: what is counted as missing
        U32 reports = 0;            // reports (10 s each) this list has been the avatar's, this one included
        U32 age = 0;                // reports the avatar has been seen, this one included
        // The object cache's entries whose parent is the avatar (scan_region), by what they are. The entry of an object
        // that is alive (worn or waiting for its drawable) is not one of these: it is only counted (of_live_*), since an
        // update of a live object makes its entry not valid or asleep while the object stays.
        U32 table = 0;              // valid, INACTIVE, waiting in the region's orphan map for the avatar (research/355 S1)
        U32 dead1 = 0;              // valid, INACTIVE, with an octree entry, not in the orphan map (dead end 1, S4)
        U32 dead3 = 0;              // valid, INACTIVE, no octree entry, not in the orphan map (dead end 3, S5)
        U32 in_queue = 0;           // valid, IN_QUEUE
        U32 waiting = 0;            // valid, WAITING
        U32 invalid = 0;            // not valid (isValid() false), any state (S3)
        U32 active = 0;             // valid, ACTIVE, and the object is neither worn nor waiting for its drawable: made, not attached
        U32 of_live_active = 0;     // entries of objects that are alive (worn or waiting) and ACTIVE (usually about W), left out of the counts above
        U32 of_live_other = 0;      // ... and not ACTIVE (not valid, or INACTIVE): a live attachment the cache has let go of (a dead end's forerunner)
        U32 orphan_ids = 0;         // local IDs in the region's orphan map for the avatar, each once
        U32 orphan_bare = 0;        // ... of those with no entry in the cache map at all
        // D: entries whose parent is the avatar only by the parent read from their packed data (the cache's own parent ID is 0: the
        // disk cache's, or one cleared by a kill), of objects that are not alive. Kept apart from the counts above (not in invalid
        // or dead3), so that those stay what they were.
        U32 disk_invalid = 0;       // D, not valid
        U32 disk_valid = 0;         // D, valid (a same-CRC resend only made it valid: INACTIVE, not in the octree; S5's form)
        // The gate for research/355 5.1 (every avatar in view, missing some or not): the entries whose parent is the avatar
        // and whose object is worn on a point that is not a HUD's, by ACTIVE or not (not valid, or asleep).
        U32 gate_active = 0;
        U32 gate_other = 0;
        // Where the missing ones are, in this order and no more than are missing (place_missing).
        U32 p_dead3 = 0;
        U32 p_dead1 = 0;
        U32 p_table = 0;
        U32 p_coming = 0;           // IN_QUEUE, WAITING, or an object waiting for its drawable
        U32 p_invalid = 0;
        U32 p_orphan_bare = 0;
        U32 p_disk_valid = 0;       // D (disk_valid)
        U32 p_disk_invalid = 0;     // D (disk_invalid)
        U32 p_made = 0;             // ACTIVE in the cache, but not worn and not waiting: made, not attached
        U32 p_nowhere = 0;
        U32 p_unsplit = 0;          // missing, but the cache could not be asked (culling off, no region)
        bool shown = false;         // already in a "worst" line
    };

    struct Totals
    {
        U32 avatars = 0;
        U32 no_list = 0;
        U32 rez3 = 0;               // status 3 "loading attachments"
        U32 rez3_not_missing = 0;   // ... of those, nothing missing by the count above
        U32 listed = 0;
        U32 listed_other = 0;
        U32 worn = 0;
        U32 matched = 0;
        U32 by_item = 0;
        U32 by_object = 0;
        U32 pending = 0;
        U32 below_list = 0;         // avatars with W < L (the plain count; includes one that took some off)
        U32 missing_avatars = 0;    // avatars with something missing by the count above
        U32 missing = 0;
        U32 dead3 = 0;
        U32 dead1 = 0;
        U32 table = 0;
        U32 coming = 0;
        U32 invalid = 0;
        U32 orphan_bare = 0;
        U32 made = 0;
        U32 disk_invalid = 0;
        U32 disk_valid = 0;
        U32 nowhere = 0;
        U32 unsplit = 0;
        U32 of_live_active = 0;     // entries of live objects left out (of the avatars missing some): ACTIVE ones
        U32 of_live_other = 0;      // ... and the others

        void add(const Record& r)
        {
            ++avatars;
            no_list += r.has_list ? 0 : 1;
            if (r.rez == 3)
            {
                ++rez3;
                rez3_not_missing += r.missing == 0 ? 1 : 0;
            }
            listed += r.listed;
            listed_other += r.listed_other;
            worn += r.worn;
            matched += r.matched;
            by_item += r.by_item;
            by_object += r.by_object;
            pending += r.pending;
            below_list += r.worn < r.listed ? 1 : 0;
            if (r.missing > 0)
            {
                ++missing_avatars;
                missing += r.missing;
                dead3 += r.p_dead3;
                dead1 += r.p_dead1;
                table += r.p_table;
                of_live_active += r.of_live_active;
                of_live_other += r.of_live_other;
                made += r.p_made;
                disk_invalid += r.p_disk_invalid;
                disk_valid += r.p_disk_valid;
                coming += r.p_coming;
                invalid += r.p_invalid;
                orphan_bare += r.p_orphan_bare;
                nowhere += r.p_nowhere;
                unsplit += r.p_unsplit;
            }
        }
    };

    // Reused between reports (a report is every 10 s; nothing is allocated in a frame between them).
    std::vector<Record> sRecords;
    std::vector<LLViewerRegion*> sRegions;
    std::unordered_map<U32, U32> sIndex;    // local ID of an avatar in view -> its place in sRecords (missing some or not: the gate counts all)
    // The object used for each avatar ID this report. An avatar made again in another region leaves two objects for a while: the one in
    // the camera's frustum is used, else the newer (LLCharacter::sInstances is in the order they were made). The totals, the gate, the
    // worst lines and the list count are of avatars; the other object would also reset what was seen (Seen) every report.
    struct Chosen
    {
        LLVOAvatar* avatar = nullptr;
        bool in_view = false;
    };
    std::unordered_map<LLUUID, Chosen> sChosen;
    std::unordered_set<U32> sLive;          // local IDs of the objects alive on those avatars (worn, or waiting for a drawable)
    std::unordered_set<U32> sWornPlain;     // local IDs of the objects worn on those avatars at a point that is not a HUD's (the gate)
    std::unordered_set<U64> sOrphanKeys;    // parent local ID << 32 | child local ID, for each orphan map entry of those avatars (once)
    std::unordered_map<LLUUID, Seen> sSeen; // by avatar ID
    U32 sReport = 0;
    U32 sBareRoots = 0;                     // this report: valid INACTIVE cache entries with no parent, not in the octree (regions walked)
    U32 sShortData = 0;                     // this report: entries whose packed data was too short for parent_from_data to read (regions walked)
    std::chrono::steady_clock::time_point sLastReport;     // the epoch: no report yet, the first frame starts the period

    // The same test as GrayWire's listedPoint (llgraywire.cpp, research/277 A; that one is in an unnamed
    // namespace): a point the avatar has, not a HUD's; the point without the add flag.
    bool is_listed_point(const LLVOAvatar* avatar, S32 point)
    {
        const auto it = avatar->mAttachmentPoints.find(point & ~(S32)ATTACHMENT_ADD);
        return it != avatar->mAttachmentPoints.end() && it->second && !it->second->getIsHUDAttachment();
    }

    void count_attachments(const LLVOAvatar* avatar, Record& r)
    {
        const std::map<LLUUID, S32>& list = avatar->mSimAttachments;
        r.has_list = !list.empty();
        U32 digest = 0;
        for (const auto& [id, point] : list)
        {
            if (is_listed_point(avatar, point))
            {
                ++r.listed;
                digest ^= id.getCRC32();
            }
            else
            {
                ++r.listed_other;
            }
        }
        for (const auto& [index, attachment] : avatar->mAttachmentPoints)
        {
            if (!attachment || attachment->getIsHUDAttachment())
            {
                continue;
            }
            for (const LLPointer<LLViewerObject>& object : attachment->mAttachedObjects)
            {
                if (!object)
                {
                    continue;
                }
                ++r.worn;
                bool by_item = true;        // which ID found it: the item ID first, the object ID when the item ID is not in the list
                auto it = list.find(object->getAttachmentItemID());
                if (it == list.end())
                {
                    by_item = false;
                    it = list.find(object->getID());
                }
                if (it != list.end() && is_listed_point(avatar, it->second))
                {
                    ++r.matched;
                    ++(by_item ? r.by_item : r.by_object);
                }
            }
        }
        r.pending = (U32)avatar->mPendingAttachment.size();

        Seen& seen = sSeen[avatar->getID()];
        const bool fresh = seen.report == 0 || seen.object != avatar || seen.local_id != r.local_id;
        if (fresh)
        {
            seen.object = avatar;
            seen.local_id = r.local_id;
            seen.by_id = false;
            seen.first = sReport;
        }
        if (fresh || r.listed != seen.listed || digest != seen.digest)
        {
            seen.listed = r.listed;
            seen.digest = digest;
            seen.seen = 0;
            seen.since = sReport;
        }
        seen.by_id = seen.by_id || r.matched > 0;
        seen.seen = llmax(seen.seen, seen.by_id ? r.matched : r.worn);
        seen.report = sReport;
        r.seen = seen.seen;
        r.reports = sReport - seen.since + 1;
        r.age = sReport - seen.first + 1;
        r.missing = r.listed > r.seen ? r.listed - r.seen : 0;
    }

    // Where the missing attachments of an avatar are, in this order and no more than are missing (research/355 5.3):
    // a valid INACTIVE entry in the cache whose parent is the avatar that nothing will make: with no octree entry and
    // not in the orphan map (dead end 3, S5), with an octree entry and not in the orphan map (dead end 1, S4), a valid
    // one that is the avatar's only by the parent in its packed data (D: parent ID 0 in the cache, valid after a same-CRC
    // resend, not in the octree: S5's form too), then one waiting in the orphan map (S1: the avatar's next update makes it);
    // on the way (IN_QUEUE or WAITING in the cache, or an object waiting for its drawable); an entry that is not valid
    // (cleared by a kill, waiting to be sent again: S3), a D that is not valid; a local ID in the orphan map that has no
    // entry in the cache at all; an ACTIVE entry whose object is neither worn nor waiting (made, not attached); the rest
    // nowhere. The entries of live objects are left out (scan_region), so a worn attachment is not counted as missing.
    // A count, not a match: an entry's packed data starts with its object's UUID (processObjectUpdateFromCache reads it), but comparing needs the
    // data read (its read position moves), so this only counts.
    void place_missing(Record& r, bool cache_known)
    {
        if (r.missing == 0)
        {
            return;
        }
        if (!cache_known)
        {
            r.p_unsplit = r.missing;
            return;
        }
        U32 left = r.missing;
        const auto take = [&left](U32 have)
        {
            const U32 n = llmin(left, have);
            left -= n;
            return n;
        };
        r.p_dead3 = take(r.dead3);
        r.p_dead1 = take(r.dead1);
        r.p_disk_valid = take(r.disk_valid);
        r.p_table = take(r.table);
        r.p_coming = take(r.in_queue + r.waiting + r.pending);
        r.p_invalid = take(r.invalid);
        r.p_disk_invalid = take(r.disk_invalid);
        r.p_orphan_bare = take(r.orphan_bare);
        r.p_made = take(r.active);
        r.p_nowhere = left;
    }

    U64 orphan_key(U32 parent_id, U32 child_id)
    {
        return ((U64)parent_id << 32) | child_id;
    }

    // Before the walk, for the avatars of this region in view (sIndex): the local IDs of the objects that are alive on
    // them (worn, at any point, or waiting for a drawable: sLive; worn at a point that is not a HUD's: sWornPlain, for the
    // gate), and, for those missing some, the orphan map's children of each, each ID once (the map's vector is not kept
    // free of repeats: sOrphanKeys, orphan_ids, orphan_bare).
    void prepare_region(LLViewerRegion* region, const std::map<U32, LLPointer<LLVOCacheEntry> >& cache)
    {
        sLive.clear();
        sWornPlain.clear();
        sOrphanKeys.clear();
        for (const auto& [parent_id, place] : sIndex)
        {
            Record& r = sRecords[place];
            for (const auto& [index, attachment] : r.avatar->mAttachmentPoints)
            {
                if (!attachment)
                {
                    continue;
                }
                for (const LLPointer<LLViewerObject>& object : attachment->mAttachedObjects)
                {
                    if (object)
                    {
                        sLive.insert(object->getLocalID());
                        if (!attachment->getIsHUDAttachment())
                        {
                            sWornPlain.insert(object->getLocalID());
                        }
                    }
                }
            }
            if (r.missing == 0)
            {
                continue;       // the gate needs only the worn ones
            }
            for (const LLPointer<LLViewerObject>& object : r.avatar->mPendingAttachment)
            {
                if (object)
                {
                    sLive.insert(object->getLocalID());
                }
            }
            const std::vector<U32>* children = region->getOrphanMapChildren(parent_id);
            if (!children)
            {
                continue;
            }
            for (const U32 child_id : *children)
            {
                if (sOrphanKeys.insert(orphan_key(parent_id, child_id)).second)
                {
                    ++r.orphan_ids;
                    r.orphan_bare += cache.find(child_id) == cache.end() ? 1 : 0;
                }
            }
        }
    }

    // The bytes of a packed entry that unpackParentID reads at most: the offset of ParentID after Omega (see
    // LLViewerObject::initObjectDataMap: ID 16, LocalID 4, PCode 1, State 1, CRC 4, Material 1, ClickAction 1, Scale, Pos, Rot
    // 12 each, SpecialCode 4, Owner 16, Omega 12) and its 4 bytes. Real compressed data is longer (path, profile, texture
    // entry follow). A shorter buffer is not read: the read would log a warning for each field (verifyLength) and leave its
    // value unset.
    constexpr S32 MIN_DATA_FOR_PARENT = 16 + 4 + 1 + 1 + 4 + 1 + 1 + 12 * 3 + 4 + 16 + 12 + 4;

    // The parent ID of an entry whose own parent ID is 0 and that is not valid, or is INACTIVE and not in the octree (the same
    // entries as sBareRoots counts, and the not valid ones: the entry made from the disk cache has 0, and a same-CRC resend only
    // makes it valid; llvocache.cpp ParentID(0), llviewerregion.cpp cacheFullUpdate), read from its packed data as
    // processObjectUpdateFromCache does (llviewerregion.cpp). unpackParentID moves the read position and puts it back to the
    // start (reset), as that does. 0 when there is no data, too little of it, or no parent in it. Not for an entry in the octree
    // that is valid, nor an ACTIVE or WAITING one: its parent ID is already right (and the read looks two std::string keys up
    // in a map).
    U32 parent_from_data(LLVOCacheEntry* entry)
    {
        LLDataPackerBinaryBuffer* dp = entry->getDP();      // NULL if nothing is cached
        U32 parent_id = 0;
        if (dp && dp->getBufferSize() >= MIN_DATA_FOR_PARENT)
        {
            LLViewerObject::unpackParentID(dp, parent_id);
        }
        else if (dp)
        {
            ++sShortData;       // not read (the same length check the fix B's entrance will make: how many it would leave as the official does)
        }
        return parent_id;
    }

    // One walk of a region's object cache for the avatars of sRecords in view there: the entries that have the avatar as
    // their parent, counted for the gate (the entries of its worn objects, ACTIVE or not: every avatar in view) and, for
    // the avatars missing some, by what they are (research/355 5.3's S1, S3, S4, S5), leaving out those of live objects;
    // and, from the same walk, the entries with no parent at all that are valid, INACTIVE and not in the octree
    // (sBareRoots: dead end 3 for a thing that is not a child, research/355 8). An entry with parent ID 0 that is not
    // valid or not in the octree is given the parent in its data (parent_from_data), so that the disk's entries are not
    // taken for ones with no parent. Returns the entries walked.
    size_t scan_region(LLViewerRegion* region)
    {
        sIndex.clear();
        for (size_t i = 0; i < sRecords.size(); ++i)
        {
            if (sRecords[i].region == region && sRecords[i].in_view)
            {
                sIndex[sRecords[i].local_id] = (U32)i;
            }
        }
        const std::map<U32, LLPointer<LLVOCacheEntry> >& cache = region->getVOCacheMap();
        prepare_region(region, cache);
        for (const auto& [local_id, entry_ptr] : cache)
        {
            LLVOCacheEntry* entry = entry_ptr.get();    // not const: hasState and getEntry are not
            if (!entry)
            {
                continue;
            }
            U32 parent_id = entry->getParentID();
            bool from_disk = false;
            if (parent_id == 0 && (!entry->isValid()
                || (entry->getState() == LLVOCacheEntry::INACTIVE && !entry->hasState(LLVOCacheEntry::IN_VO_TREE))))
            {
                parent_id = parent_from_data(entry);
                from_disk = parent_id != 0;
            }
            if (parent_id == 0)
            {
                sBareRoots += entry->isValid() && entry->getState() == LLVOCacheEntry::INACTIVE && !entry->hasState(LLVOCacheEntry::IN_VO_TREE) ? 1 : 0;
                continue;
            }
            const auto it = sIndex.find(parent_id);
            if (it == sIndex.end())
            {
                continue;
            }
            Record& r = sRecords[it->second];
            if (sWornPlain.count(local_id))
            {
                ++(entry->isValid() && entry->getState() == LLVOCacheEntry::ACTIVE ? r.gate_active : r.gate_other);
            }
            if (r.missing == 0)
            {
                continue;       // the classification below is for the avatars missing some
            }
            if (sLive.count(local_id))
            {
                // an update of a live object invalidates its entry, a same-CRC resend then wakes it asleep: not a missing one
                ++(entry->isValid() && entry->getState() == LLVOCacheEntry::ACTIVE ? r.of_live_active : r.of_live_other);
                continue;
            }
            if (from_disk)
            {
                ++(entry->isValid() ? r.disk_valid : r.disk_invalid);
                continue;
            }
            if (!entry->isValid())
            {
                ++r.invalid;
                continue;
            }
            switch (entry->getState())
            {
                case LLVOCacheEntry::INACTIVE:
                    if (sOrphanKeys.count(orphan_key(parent_id, local_id)))
                    {
                        ++r.table;
                    }
                    else
                    {
                        ++(entry->getEntry() ? r.dead1 : r.dead3);
                    }
                    break;
                case LLVOCacheEntry::IN_QUEUE: ++r.in_queue; break;
                case LLVOCacheEntry::WAITING:  ++r.waiting;  break;
                case LLVOCacheEntry::ACTIVE:   ++r.active;   break;
                default: break;
            }
        }
        return cache.size();
    }

    void format_totals(std::ostream& out, const Totals& t)
    {
        out << "avatars " << t.avatars << " (no list yet " << t.no_list << "), L listed " << t.listed << " (on a HUD or a point it lacks "
            << t.listed_other << "), W worn " << t.worn << " (found in the list by item ID "
            << t.by_item << ", by object ID " << t.by_object << "), waiting for a drawable "
            << t.pending << ", rez 3 " << t.rez3 << " (of them nothing missing " << t.rez3_not_missing << "); W<L avatars "
            << t.below_list << "; missing " << t.missing_avatars << " avatars, " << t.missing << " attachments: dead end 3 (valid INACTIVE, no octree entry, not in the orphan map) "
            << t.dead3 << ", dead end 1 (valid INACTIVE, octree entry, not in the orphan map) " << t.dead1 << ", waiting in the orphan map "
            << t.table << ", on the way " << t.coming << ", entry not valid " << t.invalid << ", orphan-map ID with no entry " << t.orphan_bare
            << ", made but not attached " << t.made << ", nowhere " << t.nowhere << ", not split (cache culling off, or no region) " << t.unsplit
            << "; of the missing, from disk (parent read from the data): valid " << t.disk_valid << ", not valid " << t.disk_invalid
            << "; entries of live objects left out: ACTIVE " << t.of_live_active << ", other " << t.of_live_other;
    }

    void log_worst(const Record& r)
    {
        std::ostringstream line;
        line << "worst " << r.avatar->getID().asString().substr(0, 8) << " rez " << r.rez << ", missing for " << r.reports << " reports (avatar seen for " << r.age
             << "): L " << r.listed << " W " << r.worn << " (found in the list by item ID " << r.by_item
             << ", by object ID " << r.by_object << ") most worn at once " << r.seen
             << ", waiting for a drawable " << r.pending
             << "; cache entries with it as parent: valid INACTIVE in the orphan map " << r.table << ", not in it with an octree entry "
             << r.dead1 << ", not in it with none " << r.dead3 << ", IN_QUEUE " << r.in_queue << ", WAITING " << r.waiting << ", not valid "
             << r.invalid << ", ACTIVE of an object not worn " << r.active << ", of live objects (left out): ACTIVE " << r.of_live_active
             << " other " << r.of_live_other << "; orphan map IDs " << r.orphan_ids << " (no entry "
             << r.orphan_bare << "), from disk (parent read from the data): valid " << r.disk_valid << ", not valid " << r.disk_invalid
             << "; missing " << r.missing << ": dead end 3 " << r.p_dead3 << ", dead end 1 " << r.p_dead1 << ", disk valid " << r.p_disk_valid
             << ", in the orphan map " << r.p_table << ", on the way " << r.p_coming << ", not valid " << r.p_invalid
             << ", disk not valid " << r.p_disk_invalid << ", orphan ID alone "
             << r.p_orphan_bare << ", made not attached " << r.p_made << ", nowhere " << r.p_nowhere << ", not split " << r.p_unsplit;
        LL_INFOS("VKPerf") << "attach diag " << line.str() << LL_ENDL;
    }

    void report()
    {
        const auto start = std::chrono::steady_clock::now();
        ++sReport;
        sBareRoots = 0;
        sShortData = 0;
        sRecords.clear();
        sChosen.clear();
        U32 visible = 0, rez3_all = 0, animesh = 0, self_seen = 0, duplicates = 0;
        for (LLCharacter* character : LLCharacter::sInstances)
        {
            LLVOAvatar* avatar = (LLVOAvatar*)character;
            if (avatar->isDead() || avatar->isUIAvatar())
            {
                continue;
            }
            if (avatar->isControlAvatar())
            {
                ++animesh;
                continue;
            }
            if (avatar->isSelf())
            {
                ++self_seen;        // its list is not another's: not counted
                continue;
            }
            const bool in_frustum = avatar->extentsInCameraFrustum();   // not isVisible(): that is also true of one only a shadow, probe or water pass sees
            Chosen& chosen = sChosen[avatar->getID()];
            if (!chosen.avatar || in_frustum || !chosen.in_view)    // a later object is the newer: it wins unless the one kept is in view and it is not
            {
                chosen = {avatar, in_frustum};
            }
        }
        for (LLCharacter* character : LLCharacter::sInstances)
        {
            LLVOAvatar* avatar = (LLVOAvatar*)character;
            if (avatar->isDead() || avatar->isUIAvatar() || avatar->isControlAvatar() || avatar->isSelf())
            {
                continue;
            }
            const Chosen& chosen = sChosen[avatar->getID()];
            if (chosen.avatar != avatar)
            {
                ++duplicates;       // the other object of an ID: not counted
                continue;
            }
            Record& r = sRecords.emplace_back();
            r.avatar = avatar;
            r.region = avatar->getRegion();
            r.local_id = avatar->getLocalID();
            r.rez = avatar->mLastRezzedStatus;
            r.in_view = chosen.in_view;
            visible += avatar->isVisible() ? 1 : 0;
            rez3_all += r.rez == 3 ? 1 : 0;
            count_attachments(avatar, r);   // the one object of every ID, so that the most worn at once is kept while it is out of view
        }
        for (auto it = sSeen.begin(); it != sSeen.end();)
        {
            it = it->second.report == sReport ? std::next(it) : sSeen.erase(it);
        }

        // The object cache: once per region that has an avatar in view, missing some or not (at most one walk each: the gate
        // counts every avatar in view).
        const bool cache_known = LLViewerRegion::sVOCacheCullingEnabled;
        size_t scanned = 0;
        U32 regions_scanned = 0;
        if (cache_known)
        {
            sRegions.clear();
            for (const Record& r : sRecords)
            {
                if (r.in_view && r.region && std::find(sRegions.begin(), sRegions.end(), r.region) == sRegions.end())
                {
                    sRegions.push_back(r.region);
                }
            }
            for (LLViewerRegion* region : sRegions)
            {
                scanned += scan_region(region);
                ++regions_scanned;
            }
        }
        Totals in_view;
        U32 gate_avatars = 0, gate_worn = 0, gate_active = 0, gate_other = 0;   // the gate: the avatars in view whose region's cache was walked
        for (Record& r : sRecords)
        {
            if (r.in_view)
            {
                place_missing(r, cache_known && r.region);
                in_view.add(r);
                if (cache_known && r.region)
                {
                    ++gate_avatars;
                    gate_worn += r.worn;
                    gate_active += r.gate_active;
                    gate_other += r.gate_other;
                }
            }
        }
        const F64 ms = std::chrono::duration<F64, std::milli>(std::chrono::steady_clock::now() - start).count();

        std::ostringstream line;
        line << std::fixed << std::setprecision(2);
        line << "others " << sRecords.size() << " (isVisible " << visible << ", rez 3 anywhere " << rez3_all << "; animesh " << animesh
             << " and self " << self_seen << " not counted; second objects of an ID " << duplicates << " not counted); in the camera's frustum: ";
        format_totals(line, in_view);
        line << "; cache: culling " << (cache_known ? "on" : "off") << ", " << regions_scanned << " regions, " << scanned
             << " entries walked (in those: valid INACTIVE entries with no parent and not in the octree " << sBareRoots
             << "; short data not read " << sShortData << "); object list orphans "<< gObjectList.getOrphanCount() << "; this count took " << ms << " ms";
        LL_INFOS("VKPerf") << "attach diag " << line.str() << LL_ENDL;

        // The gate for research/355 stage 1 (5.1): are the worn objects' entries in the cache (the cache's way) or not (uncompressed)?
        // Compare v + o with W: the same avatars, missing some or not. With culling off the cache is not read: a line that says so.
        if (cache_known)
        {
            LL_INFOS("VKPerf") << "attach diag gate: in view W " << gate_worn << ", entries of worn objects ACTIVE " << gate_active
                               << " other " << gate_other << " (of avatars " << gate_avatars << ")" << LL_ENDL;
        }
        else
        {
            LL_INFOS("VKPerf") << "attach diag gate: not read (cache culling off)" << LL_ENDL;
        }

        // The avatars in view missing the most (up to WORST_LINES, by repeated selection: at most a few hundred records, 5 passes).
        for (size_t n = 0; n < WORST_LINES; ++n)
        {
            Record* worst = nullptr;
            for (Record& r : sRecords)
            {
                if (r.in_view && r.missing > 0 && !r.shown && (!worst || r.missing > worst->missing))
                {
                    worst = &r;
                }
            }
            if (!worst)
            {
                break;
            }
            worst->shown = true;
            log_worst(*worst);
        }
    }
}

namespace plus::attachdiag
{
    void frame()
    {
        const auto now = std::chrono::steady_clock::now();
        if (sLastReport == std::chrono::steady_clock::time_point())
        {
            sLastReport = now;
            return;
        }
        if (std::chrono::duration<F64>(now - sLastReport).count() < REPORT_SECONDS)
        {
            return;
        }
        sLastReport = now;
        report();
    }
}

/**
 * @file areasearch.h
 * @brief Area search: lists the objects the viewer knows about, found by name, owner and more.
 *
 * Ported from Firestorm's fsareasearch.h (see ORIGIN.md).
 *
 * $LicenseInfo:firstyear=2012&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (c) 2012 Techwolf Lupindo
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
 *
 * The Phoenix Firestorm Project, Inc., 1831 Oakwood Drive, Fairmont, Minnesota 56031-3225 USA
 * http://www.firestormviewer.org
 * $/LicenseInfo$
 */

// Ported from Firestorm's FSAreaSearch (Techwolf Lupindo, 2012; later changes by Cron, Beq,
// minerjr and Neremyn), split in two: this file and areasearch.cpp are the search itself and
// know nothing of the window; areasearchfloater.cpp is the window (research/140).
//
// How it works
// ------------
// The simulator tells the viewer an object's name, owner and so on only for an object the
// agent has selected. So the search asks for the properties of every object it can list:
// an ObjectSelect and, at once, an ObjectDeselect for the same ids (nothing is shown as
// selected, the properties come back all the same). The answers (ObjectProperties) are
// kept for the session of the window; the objects themselves are the viewer's own
// (gObjectList). A pass over gObjectList once a second lists the objects that match the
// criteria, removes the rows of objects that are gone and keeps distance and land impact
// current.
//
// While the engine runs, the agent's interest list is the "360" one (the simulator sends
// everything of the region, not what is in view) and objects out of view are not culled
// from memory (keepObjects(), one hook in LLViewerRegion::killInvisibleObjects()), or the
// list would lose them as fast as the simulator sent them.
//
// Main thread only. One engine at a time (the window is a single instance).

#ifndef PLUS_AREASEARCH_AREASEARCH_H
#define PLUS_AREASEARCH_AREASEARCH_H

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <boost/regex.hpp>
#include <boost/signals2/connection.hpp>

#include "llcategory.h"
#include "llframetimer.h"
#include "llpermissions.h"
#include "llsaleinfo.h"
#include "lluuid.h"
#include "v3dmath.h"

class LLMessageSystem;
class LLViewerObject;
class LLViewerRegion;
class LLVOCacheEntry;
class LLAvatarName;

namespace plus::areasearch
{
    // What to list. The text fields match anywhere in the text without regard to case
    // (the whole text, when regex is on); empty: no condition.
    struct Criteria
    {
        std::string name;
        std::string description;
        std::string owner;
        std::string group;
        std::string creator;
        std::string lastOwner;
        bool regex = false;
        // Only what this avatar owns itself (not through a group), by id; null: no condition.
        // The "mine only" of the list's menu (names on the rows may carry a display name).
        LLUUID ownerId;
        // The list's quick filter: in any of the texts above (name ... last owner), in part and
        // without regard to case, never a regular expression.
        std::string anyText;

        // Only these objects ...
        bool onlyLocked = false;
        bool onlyPhysical = false;
        bool onlyTemporary = false;
        bool onlyPhantom = false;
        bool onlyAttachment = false;
        bool onlyMedia = false;
        bool onlyProbe = false;
        bool onlyCopy = false;      // the owner may copy / modify / transfer it
        bool onlyModify = false;
        bool onlyTransfer = false;
        bool onlyParcel = false;    // the parcel the agent stands on
        bool forSale = false;
        S32  priceMin = 0;
        S32  priceMax = 999999;
        bool distance = false;
        S32  distanceMin = 0;
        S32  distanceMax = 0;
        // 0 none, 1 any, 2 touch, 3 + the viewer's click action (sit, buy, pay, open, play,
        // open media, zoom), the order of the window's combo
        S32  clickAction = 0;

        // ... and not these (they are not even asked for)
        bool exceptAttachment = false;
        bool exceptPhysical = false;
        bool exceptTemporary = false;
        bool exceptProbe = false;
        bool exceptChild = true;      // linked prims other than the root
        bool exceptNeighbor = true;   // objects of the neighbouring regions
        // What any avatar wears, the agent's too (attachments and HUDs, every prim of them; not
        // what is rezzed in world, animesh included). PlusAreaSearchIncludeAttached off.
        bool exceptAttached = true;
    };

    // What the simulator told about one object, and what the list shows.
    struct Entry
    {
        // Failed: asked MAX_TRIES times without an answer; not asked again while the object
        // exists (an entry goes when its object does).
        enum class Request { Need, Sent, Finished, Failed };

        LLUUID id;
        U32 localId = 0;
        U64 regionHandle = 0;
        Request request = Request::Need;
        F64 sentAt = 0.0;             // viewer seconds of the last request; 0: never asked
        U8 tries = 0;                 // requests that went unanswered
        bool listed = false;
        bool namesPending = false;    // listing waits for a name of the cache
        // Counted in mPending by the last scan(): its answer takes that count back, whether or not
        // the conditions (the range, as the agent moves) still want it when it comes.
        bool pendingCounted = false;

        std::string name;
        std::string description;
        LLUUID creatorId;
        LLUUID ownerId;               // as sent: the group's id for a group's object
        LLUUID lastOwnerId;
        LLUUID groupId;
        LLUUID ownershipId;           // who owns it: the group when groupOwned
        bool groupOwned = false;
        U32 ownerMask = 0;
        LLSaleInfo saleInfo;
        LLCategory category;
        LLPermissions permissions;
        LLAggregatePermissions aggregatePerms;

        S32 shownDistance = -1;       // what the row shows now, so only changes reach the window
        F32 shownCost = -1.f;

        // Known from the object cache record alone: the viewer has made no object of it (hidden
        // behind something, too small at that distance, out of view), and there is none to ask.
        // position, updateFlags, clickAction, prims are what the record said last (cacheOnly is
        // set by scan(): only while no object exists). A record is read again when its CRC changes.
        bool cached = false;          // a record was seen (cacheCycle: in which pass over the records)
        bool cacheOnly = false;
        bool child = false;           // a linked prim other than the root
        LLVector3d position;          // global: the record's, or where scan() last saw the object
        U32 updateFlags = 0;
        U8 clickAction = 0;
        S32 prims = 1;
        U32 cacheCrc = 0;
        U32 cacheCycle = 0;
    };

    // One row of the list.
    struct Row
    {
        LLUUID id;
        S32 distance = 0;             // metres
        std::string name;
        std::string description;
        S32 price = -1;               // -1: not for sale
        F32 landImpact = 0.f;         // 0: not known yet
        S32 prims = 1;
        std::string owner;
        std::string group;
        std::string creator;
        std::string lastOwner;
        bool temporary = false;       // the window shows it in italics
        bool physical = false;        // ... and in bold
    };

    // What the engine tells the window (all optional).
    struct Callbacks
    {
        std::function<void(const Row&)>     rowAdded;
        std::function<void(const Row&)>     rowChanged;
        std::function<void(const std::vector<LLUUID>&)> rowsRemoved;   // all of one pass at once
        std::function<void()>               cleared;
        std::function<void(S32 listed, S32 pending, S32 total)> counts;
    };

    class Engine
    {
    public:
        explicit Engine(Callbacks callbacks);
        ~Engine();
        Engine(const Engine&) = delete;
        Engine& operator=(const Engine&) = delete;

        // Starts and stops the search (interest list, message handler, idle callback). A
        // started engine lists nothing until search() is called.
        void start();
        void stop();
        bool running() const { return mRunning; }

        // Lists again with these criteria. cacheClear: forget what the simulator told too
        // (asks again). Returns an empty string, or what is wrong with a regular expression.
        std::string search(const Criteria& criteria, bool cacheClear);
        // The rows again with the criteria of the last search (the window's columns changed and
        // its rows went): every object that matches is listed anew. Nothing is asked again.
        void relist();
        // Leaves what avatars wear out of the list (or takes it in) with the other conditions of
        // the last search; lists again only when it changes.
        void setExceptAttached(bool except);

        // The properties of an object that was listed (nullptr: unknown).
        const Entry* find(const LLUUID& id) const;
        // The objects the properties of which the simulator has not sent yet.
        S32 pending() const { return mPending; }

        // Called by the message handler for ObjectProperties; true when every object of the
        // message was asked for by the search (the official handler has nothing to do then).
        bool ownsProperties(LLMessageSystem* msg) const;
        void onObjectProperties(LLMessageSystem* msg);

    private:
        static void idle(void* self);
        void tick();
        void checkRegion();
        void scan();
        // A slice of the object cache records of the regions searched (a few thousand a frame, a
        // whole pass takes some seconds); the ones without an object become entries (research/218).
        void scanCache(LLViewerRegion* agentRegion);
        void visitRecord(LLViewerRegion* region, LLVOCacheEntry& record);
        bool cacheBacked(const Entry& entry) const { return entry.cached && entry.cacheCycle + 1 >= mCacheCycle; }
        bool searchableCached(const Entry& entry, LLViewerRegion* agentRegion) const;
        // The object when the viewer has one, else the cache record (object nullptr).
        bool wantedNow(const Entry& entry, LLViewerObject* object, LLViewerRegion* agentRegion) const;
        void sendRequests();
        void requestProperties(const std::vector<U32>& localIds, bool select, LLViewerRegion* region);
        void releaseInFlight(const Entry& entry);
        void rememberLate(const Entry& entry, F64 now);
        void forgetEntries();
        void disconnectNames();
        bool searchable(LLViewerObject* object, LLViewerRegion* agentRegion) const;
        // Within the search range of the agent (mRange, from mAgentPosition; always when it is 0).
        // The object, or without one (nullptr) where the entry was last seen or its cache record said it is.
        bool withinRange(LLViewerObject* object, const Entry* entry) const;
        // object may be nullptr (entry.cacheOnly): then the entry's own copy of what the record said.
        void tryList(Entry& entry, LLViewerObject* object);
        Row makeRow(Entry& entry, LLViewerObject* object);
        void refreshRows();
        std::string nameOf(const LLUUID& id, bool group, bool& pending, bool owner = false);
        void onNameArrived(const LLUUID& id);
        void applyArrivedNames();
        void clearRows();
        void publishCounts();

        // Does the text satisfy condition i (0 name ... 5 last owner)?
        bool textMatches(const std::string& text, S32 i) const;
        bool anyTextMatches(const Row& row) const;

        Callbacks mCallbacks;
        bool mRunning = false;
        bool mOwns360 = false;  // the "360" interest list in use is the one start() or tick() put in
        Criteria mCriteria;
        // lower-cased needles / compiled patterns of the text conditions (name, description,
        // owner, group, creator, last owner)
        std::string mNeedle[6];
        boost::regex mPattern[6];
        std::string mAnyNeedle;

        // The objects that exist (an entry goes with its object, scan()).
        std::map<LLUUID, Entry> mEntries;
        std::map<U64, S32> mRequestsInFlight;      // by region
        // The entries to ask about (Need), by region; made again by each scan(), taken from by
        // sendRequests() while the region has room in flight. Stale ids are skipped there.
        std::map<U64, std::deque<LLUUID>> mQueue;
        // Objects asked about whose entry went before the answer came (a new region, a search
        // that forgets what it knew, the object gone): their answers are still the search's, not
        // the official handler's (which would log a warning for each). Until when (viewer seconds).
        std::map<LLUUID, F64> mLateAnswers;
        LLViewerRegion* mLastRegion = nullptr;
        S32 mPending = 0;
        S32 mListed = 0;
        S32 mSearchable = 0;
        bool mRescan = false;
        // PlusAreaSearchRange as the search uses it, metres: only what is this near the agent is
        // asked about and listed; 0: no limit. Read by start() and, when it changes, by tick().
        F32 mRange = 0.f;
        // Where the agent is, for withinRange(): taken once at each way in (tick(), scan(), an
        // answer), not again for every object a pass looks at.
        LLVector3d mAgentPosition;

        // Where the pass over the cache records is: the region (by handle, ascending), the next
        // local id in it, and the number of the pass (an entry is cache-backed when seen in this
        // or the last pass).
        std::vector<U64> mCacheRegions;
        U64 mCacheRegion = 0;
        U32 mCacheCursor = 0;
        U32 mCacheCycle = 0;

        LLFrameTimer mScanTimer;
        std::set<LLUUID> mNamesAsked;
        // The names still on their way (one request a name; the connections are cut by stop()).
        struct NameRequest
        {
            boost::signals2::connection connection;
            F64 askedAt = 0.0;      // viewer seconds
        };
        std::map<LLUUID, NameRequest> mNameConnections;
        std::set<LLUUID> mArrivedNames;   // came since the last tick (onNameArrived(), applyArrivedNames())
        class ParcelObserver;
        std::unique_ptr<ParcelObserver> mParcelObserver;
    };

    // True while a search runs: objects out of view are kept (the hook in LLViewerRegion::
    // killInvisibleObjects()).
    bool keepObjects();

    // While a search runs: the distance in metres the simulator is asked to send objects from
    // (PlusAreaSearchRange, at most MAX_RANGE), apart from the draw distance; 0: no wish (the
    // hook in the AgentUpdate message, LLAgent's Far, asks for the farther of the two). What is
    // drawn does not change. The same setting limits the list too (Engine::withinRange()).
    F32 farRange();

    // The rows of the open window again, from what the search knows (RLVa's @shownames switched on or off:
    // plus/rlv). Nothing when the window is closed or has not searched yet. areasearchfloater.cpp.
    void refreshNames();
}

#endif // PLUS_AREASEARCH_AREASEARCH_H

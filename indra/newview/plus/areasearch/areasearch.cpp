/**
 * @file areasearch.cpp
 * @brief Area search: the search itself (see areasearch.h).
 *
 * Ported from Firestorm's fsareasearch.cpp (see ORIGIN.md).
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

#include "llviewerprecompiledheaders.h"

#include "areasearch.h"
#include "plus.h"
#include "plus/rlv/rlvhooks.h"

#include <algorithm>
#include <iterator>

#include "llagent.h"
#include "llagentcamera.h"
#include "llapp.h"
#include "llavatarnamecache.h"
#include "llcachename.h"
#include "llcallbacklist.h"
#include "llfloaterreg.h"
#include "message.h"
#include "llselectmgr.h"
#include "lltrans.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerparcelmgr.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llvoavatar.h"
#include "llvocache.h"
#include "object_flags.h"
#include "llprimitive.h"
#include "llworld.h"

namespace plus::areasearch
{
    namespace
    {
        // Objects in one ObjectSelect / ObjectDeselect message.
        constexpr S32 MAX_OBJECTS_PER_PACKET = 255;
        // Requests for properties out at once, per region; more wait (the simulator limits an
        // agent's selection).
        constexpr S32 MAX_IN_FLIGHT = 3 * MAX_OBJECTS_PER_PACKET - 3;
        // Seconds between passes over the object list; a changed search does not wait so long
        // (but not less than the minimum, so typing does not make a pass a frame).
        constexpr F32 SCAN_INTERVAL     = 1.f;
        constexpr F32 MIN_SCAN_INTERVAL = 0.25f;
        // Properties asked for and not answered for this long are asked for again (their place in
        // flight is given back), up to MAX_TRIES requests; then the object is left out (Failed).
        // A simulator that never answers for some objects would otherwise keep the region at its
        // limit in flight for good.
        constexpr F64 REQUEST_TIMEOUT = 30.0;
        constexpr U8  MAX_TRIES       = 3;
        // A name asked for and not come for this long may be asked again, once a search (nameOf():
        // mNamesAsked is emptied by search(), and by stop()): when a row needs it after the next
        // search. Within one search it is asked once (relist() asks nothing again).
        constexpr F64 NAME_RETRY      = 30.0;
        // The most the search asks the simulator for (PlusAreaSearchRange), metres.
        constexpr F32 MAX_RANGE       = 512.f;
        // Object cache records looked at in one frame (scanCache()).
        constexpr S32 CACHE_SLICE     = 2000;

        constexpr S32 TEXT_NAME = 0, TEXT_DESCRIPTION = 1, TEXT_OWNER = 2, TEXT_GROUP = 3,
                      TEXT_CREATOR = 4, TEXT_LAST_OWNER = 5, TEXT_COUNT = 6;

        // The engine the message handler and the culling hook talk to.
        Engine* sEngine = nullptr;

        // The 360 snapshot window (LLFloater360Capture) also wants the "360" interest list while it
        // is open; it puts back, when it closes, the mode it found when it opened.
        bool snapshot360Open()
        {
            return !LLFloaterReg::getFloaterList("360capture").empty();
        }

        // A search that stopped while the 360 snapshot window was open leaves the "360" interest
        // list to it; this puts the keyhole one back once that window has closed too (it found
        // "360", the search's, when it opened, so it leaves "360" on).
        bool sRevertPending = false;
        S32  sClosedFrames  = 0; // frames in a row the window has been seen closed

        void revertWhenSnapshotCloses(void*);

        void cancelRevert()
        {
            gIdleCallbacks.deleteFunction(&revertWhenSnapshotCloses, nullptr);
            sRevertPending = false;
            sClosedFrames  = 0;
        }

        void revertWhenSnapshotCloses(void*)
        {
            // Somebody else changed the list meanwhile (the debug menu, turned off and on again):
            // the "360" in use, if any, is no longer the search's to take back.
            if (LLApp::isExiting() || gAgent.getInterestListMode() != IL_MODE_360)
            {
                cancelRevert();
                return;
            }
            if (snapshot360Open())
            {
                sClosedFrames = 0;
                return;
            }
            // Closed twice in a row: a window closed other than from its own button leaves the
            // registry before its destructor puts back the mode it found (here "360").
            if (++sClosedFrames < 2)
            {
                return;
            }
            cancelRevert();
            gAgent.changeInterestListMode(IL_MODE_DEFAULT);
        }

        std::string lowerAscii(std::string text)
        {
            for (char& c : text)
            {
                if (c >= 'A' && c <= 'Z')
                {
                    c = static_cast<char>(c - 'A' + 'a');
                }
            }
            return text;
        }

        // The words of the quick filter: split at spaces (half width, ideographic U+3000 and tabs; any
        // run of them is one separator, none at the ends makes no empty word), lower-cased like the
        // other needles. Split as wide characters, so that a byte of a multi-byte character is never
        // taken for a separator.
        std::vector<std::string> quickFilterWords(const std::string& text)
        {
            static const LLWString separators = { llwchar(0x20), llwchar(0x09), llwchar(0x3000) };
            std::vector<LLWString> tokens;
            LLWStringUtil::getTokens(utf8str_to_wstring(text), tokens, separators, LLWString());
            std::vector<std::string> words;
            words.reserve(tokens.size());
            for (const LLWString& token : tokens)
            {
                words.push_back(lowerAscii(wstring_to_utf8str(token)));
            }
            return words;
        }

        // What a name shows until the name cache has it (text conditions see it; the quick filter does not).
        const std::string& waitingName()
        {
            static const std::string waiting = LLTrans::getString("AvatarNameWaiting");
            return waiting;
        }

        // Worn by an avatar (the agent's or anyone's): an attachment's root hangs from the avatar,
        // its other prims from the root (HUDs too). An attached animesh is told by its parents as
        // well (its getAvatar() is its control avatar, which is no parent); an animesh rezzed in
        // world, or an object an avatar sits on (the avatar is its child), has no avatar above it.
        // fix-round83 3: just after a login or a teleport an attachment's root can be an orphan
        // (its avatar not known yet: no parent) for a while; isAttachment() reads the attachment
        // state the server sends with the object, which an orphan has too. It is never set on an
        // object in world (one sat on, an animesh rezzed in world: state 0), and trees and grass,
        // whose state is their species, are not volumes (isAttachment() false).
        bool isAttached(LLViewerObject* object)
        {
            return object->isAttachment() || (object->getParent() && object->getAvatarAncestor());
        }

        F32 distanceTo(const LLVector3d& from, LLViewerObject* object)
        {
            return static_cast<F32>(dist_vec(from, positionOf(object)));
        }

        // Without an object (nullptr): where the cache record said it is.
        F32 distanceTo(const LLVector3d& from, const Entry& entry, LLViewerObject* object)
        {
            return object ? distanceTo(from, object) : static_cast<F32>(dist_vec(from, entry.position));
        }

        // PlusAreaSearchRange, metres. 0 is the draw distance: the cache records of the regions around
        // reach far past it (the whole region, and the regions the grid keeps), and a list of all of them
        // is not what "0" is taken to mean. Held to MAX_RANGE either way: a draw distance beyond it
        // lists (and asks the simulator for) no more than MAX_RANGE.
        F32 searchRange()
        {
            static LLCachedControl<F32> range(gSavedSettings, "PlusAreaSearchRange");
            const F32 metres = static_cast<F32>(range);
            return llclamp(metres > 0.f ? metres : gAgentCamera.mDrawDistance, 0.f, MAX_RANGE);
        }

        // ObjectProperties: the official handler (a selection of the agent's) and the engine's.
        void propertiesHandler(LLMessageSystem* msg, void** user_data)
        {
            // The properties of objects only the search asked for are not for the official
            // handler: it would log "Couldn't find object ... selected" for each of them.
            if (!sEngine || !sEngine->ownsProperties(msg))
            {
                LLSelectMgr::processObjectProperties(msg, user_data);
            }
            if (sEngine)
            {
                sEngine->onObjectProperties(msg);
            }
        }
    }

    bool keepObjects()
    {
        return sEngine && sEngine->running();
    }

    LLVector3d positionOf(LLViewerObject* object)
    {
        if (object->isHUDAttachment())
        {
            return gAgent.getPositionGlobal();
        }
        if (object->isAttachment() && object->mDrawable.isNull())
        {
            if (LLVOAvatar* wearer = object->getAvatarAncestor())
            {
                return wearer->getPositionGlobal();
            }
        }
        return object->getPositionGlobal();
    }

    F32 farRange()
    {
        return keepObjects() ? searchRange() : 0.f;
    }

    class Engine::ParcelObserver : public LLParcelObserver
    {
    public:
        explicit ParcelObserver(Engine& engine) : mEngine(engine) {}

    private:
        void changed() override { mEngine.checkRegion(); }
        Engine& mEngine;
    };

    Engine::Engine(Callbacks callbacks) : mCallbacks(std::move(callbacks)) {}

    Engine::~Engine()
    {
        stop();
        disconnectNames();  // also when it never ran: no name callback may reach a deleted engine
    }

    void Engine::start()
    {
        if (mRunning)
        {
            return;
        }
        mRunning = true;
        sEngine  = this;

        // The simulator sends everything of the region, not only what is in view. A keyhole list
        // still to be put back for an earlier search (revertWhenSnapshotCloses()) is this one's to
        // put back now.
        mOwns360 = gAgent.getInterestListMode() != IL_MODE_360;
        if (sRevertPending)
        {
            cancelRevert();
            mOwns360 = true;
        }
        gAgent.changeInterestListMode(IL_MODE_360);

        if (gMessageSystem)
        {
            gMessageSystem->setHandlerFuncFast(_PREHASH_ObjectProperties, propertiesHandler);
        }
        gIdleCallbacks.addFunction(&Engine::idle, this);

        mParcelObserver = std::make_unique<ParcelObserver>(*this);
        LLViewerParcelMgr::getInstance()->addObserver(mParcelObserver.get());

        mLastRegion = gAgent.getRegion();
        mRange      = searchRange();
        mScanTimer.start();
    }

    void Engine::disconnectNames()
    {
        for (auto& request : mNameConnections)
        {
            request.second.connection.disconnect();
        }
        mNameConnections.clear();
        mNamesAsked.clear();
        mArrivedNames.clear();
    }

    void Engine::stop()
    {
        if (!mRunning)
        {
            return;
        }
        mRunning = false;
        if (sEngine == this)
        {
            sEngine = nullptr;
        }

        gIdleCallbacks.deleteFunction(&Engine::idle, this);
        if (gMessageSystem)
        {
            gMessageSystem->setHandlerFuncFast(_PREHASH_ObjectProperties, LLSelectMgr::processObjectProperties);
        }
        if (mParcelObserver)
        {
            if (LLViewerParcelMgr::instanceExists())
            {
                LLViewerParcelMgr::getInstance()->removeObserver(mParcelObserver.get());
            }
            mParcelObserver.reset();
        }
        disconnectNames();

        // Back to the keyhole interest list, only when the "360" one in use is the one the search
        // put in (not when it was on already before the search began: the debug menu's, or the
        // 360 snapshot window's). While the snapshot window is open, it still needs "360": the
        // keyhole one comes back once it has closed.
        if (!LLApp::isExiting() && mOwns360 && gAgent.getInterestListMode() == IL_MODE_360)
        {
            if (snapshot360Open())
            {
                if (!sRevertPending)
                {
                    sRevertPending = true;
                    sClosedFrames  = 0;
                    gIdleCallbacks.addFunction(&revertWhenSnapshotCloses, nullptr);
                }
            }
            else
            {
                gAgent.changeInterestListMode(IL_MODE_DEFAULT);
            }
        }
        mOwns360 = false;
        mEntries.clear();
        mRequestsInFlight.clear();
        mQueue.clear();
        mLateAnswers.clear();   // the official handler is back: answers still on their way go to it
    }

    std::string Engine::search(const Criteria& criteria, bool cacheClear)
    {
        const std::string* texts[TEXT_COUNT] = { &criteria.name, &criteria.description, &criteria.owner,
                                                 &criteria.group, &criteria.creator, &criteria.lastOwner };
        std::string needle[TEXT_COUNT];
        boost::regex pattern[TEXT_COUNT];
        for (S32 i = 0; i < TEXT_COUNT; ++i)
        {
            if (texts[i]->empty())
            {
                continue;
            }
            if (criteria.regex)
            {
                try
                {
                    pattern[i] = boost::regex(*texts[i], boost::regex::perl | boost::regex::icase);
                }
                catch (const std::exception& e)
                {
                    // What is wrong with the pattern; the previous search stays.
                    return e.what();
                }
            }
            needle[i] = lowerAscii(*texts[i]);
        }

        mCriteria = criteria;
        for (S32 i = 0; i < TEXT_COUNT; ++i)
        {
            mNeedle[i]  = std::move(needle[i]);
            mPattern[i] = std::move(pattern[i]);
        }
        mAnyNeedles = quickFilterWords(criteria.anyText);

        checkRegion();
        if (cacheClear)
        {
            forgetEntries();
        }
        else
        {
            for (auto& entry : mEntries)
            {
                entry.second.listed = false;
            }
        }
        clearRows();
        // Names the cache did not have may be asked for again (a name still on its way is not:
        // its connection stays in mNameConnections, see nameOf()).
        mNamesAsked.clear();
        mRescan = true;
        scan();
        return std::string();
    }

    void Engine::relist()
    {
        // Not search(): a search refused (a wrong regular expression) would return before this,
        // leaving the entries marked as listed with their rows gone; tryList() passes over a
        // listed one, so they would never come back (fix-round11 3).
        for (auto& entry : mEntries)
        {
            entry.second.listed = false;
        }
        clearRows();
        mRescan = true;
        scan();
    }

    void Engine::setExceptAttached(bool except)
    {
        if (mCriteria.exceptAttached == except)
        {
            return;
        }
        mCriteria.exceptAttached = except;
        if (except)
        {
            // Nothing would be left to list "only" of (the window unticks that box too).
            mCriteria.onlyAttachment = false;
        }
        relist();
    }

    const Entry* Engine::find(const LLUUID& id) const
    {
        const auto it = mEntries.find(id);
        return it == mEntries.end() ? nullptr : &it->second;
    }

    // static
    void Engine::idle(void* self)
    {
        static_cast<Engine*>(self)->tick();
    }

    void Engine::tick()
    {
        // Someone else put the keyhole list back while the search runs (the 360 snapshot window,
        // closing, puts back the mode it found when it opened before the search): ask for "360"
        // again, as the search's own now.
        if (gAgent.getInterestListMode() != IL_MODE_360)
        {
            mOwns360 = true;
            gAgent.changeInterestListMode(IL_MODE_360);
        }
        // The range changed (the List tab's input): the next pass lists what came within it and
        // removes the rows of what is beyond it now. Nothing is asked again.
        const F32 range = searchRange();
        if (range != mRange)
        {
            mRange  = range;
            mRescan = true;
        }
        mAgentPosition = gAgent.getPositionGlobal();
        const F32 elapsed = mScanTimer.getElapsedTimeF32();
        if ((mRescan && elapsed > MIN_SCAN_INTERVAL) || elapsed > SCAN_INTERVAL)
        {
            scan();
        }
        if (LLViewerRegion* agentRegion = gAgent.getRegion())
        {
            scanCache(agentRegion);
        }
        if (!mQueue.empty())
        {
            sendRequests();
        }
        if (!mArrivedNames.empty())
        {
            applyArrivedNames();
        }
    }

    // A new region (a teleport; or a border crossing when the neighbours are searched too)
    // empties the list.
    void Engine::checkRegion()
    {
        LLViewerRegion* region = gAgent.getRegion();
        if (!region || region == mLastRegion)
        {
            return;
        }
        if (!mCriteria.exceptNeighbor)
        {
            std::vector<LLViewerRegion*> neighbors;
            region->getNeighboringRegions(neighbors);
            if (std::find(neighbors.begin(), neighbors.end(), mLastRegion) != neighbors.end())
            {
                // Crossed into a neighbouring region: its objects are known already.
                mLastRegion = region;
                return;
            }
        }
        mLastRegion = region;
        forgetEntries();
        clearRows();
        mRescan = true;
    }

    // An entry asked about goes before its answer came: the answer is taken quietly when it comes.
    void Engine::rememberLate(const Entry& entry, F64 now)
    {
        // Asked at least once and not answered (also one that timed out: its answer may still come).
        if (entry.sentAt > 0.0 && entry.request != Entry::Request::Finished)
        {
            mLateAnswers[entry.id] = now + REQUEST_TIMEOUT;
        }
    }

    // Forgets every entry (a new region, or a search that asks everything again).
    void Engine::forgetEntries()
    {
        const F64 now = LLFrameTimer::getElapsedSeconds();
        for (const auto& entry : mEntries)
        {
            rememberLate(entry.second, now);
        }
        mPending = 0;
        mEntries.clear();
        mRequestsInFlight.clear();
        mQueue.clear();
    }

    bool Engine::searchable(LLViewerObject* object, LLViewerRegion* agentRegion) const
    {
        // Land has no properties; avatars have none of the usual ones; what cannot be selected
        // cannot be asked about.
        if (!object->getRegion() || object->getPCode() == LLViewerObject::LL_VO_SURFACE_PATCH
            || !object->mbCanSelect || object->isAvatar())
        {
            return false;
        }
        if (mCriteria.exceptChild && !(object->isRoot() || (object->isAttachment() && object->isRootEdit())))
        {
            return false;
        }
        // With the other regions searched too, only those around: after a teleport far away the
        // old region stays in the viewer's world for a while (until its simulator lets it go),
        // with its objects, and they are not around any more. By distance, so that regions a
        // grid connects beyond the eight neighbours stay (fix-round9b 2, fix-round10 2).
        if (object->getRegion() != agentRegion
            && (mCriteria.exceptNeighbor || !plus::isRegionAround(object->getRegion())))
        {
            return false;
        }
        return !((mCriteria.exceptAttachment && object->isAttachment())
              || (mCriteria.exceptPhysical && object->flagUsePhysics())
              || (mCriteria.exceptTemporary && object->flagTemporaryOnRez())
              || (mCriteria.exceptProbe && object->mReflectionProbe.notNull())
              || (mCriteria.exceptAttached && isAttached(object)))
            && withinRange(object, nullptr);
    }

    // Unlike the distance filter (checked once, as the row is made), the range follows the agent:
    // it is part of what is searched at all (wantedNow()), so a row whose object is now beyond it
    // goes in the next pass (refreshRows()), and what comes within it is asked about and listed.
    bool Engine::withinRange(LLViewerObject* object, const Entry* entry) const
    {
        if (mRange <= 0.f)
        {
            return true;
        }
        const F32 distance = object ? distanceTo(mAgentPosition, object) : distanceTo(mAgentPosition, *entry, nullptr);
        return distance <= mRange;
    }

    // One pass over the viewer's objects: new ones are asked about, the ones with properties
    // are listed when they match.
    void Engine::scan()
    {
        LLViewerRegion* agentRegion = gAgent.getRegion();
        if (!agentRegion)
        {
            // Disconnected, or between regions.
            return;
        }
        checkRegion();
        mRescan        = false;
        mSearchable    = 0;
        mAgentPosition = gAgent.getPositionGlobal();

        const S32 count = gObjectList.getNumObjects();
        for (S32 i = 0; i < count; ++i)
        {
            LLViewerObject* object = gObjectList.getObject(i);
            if (!object || !searchable(object, agentRegion))
            {
                continue;
            }
            const LLUUID& id = object->getID();
            if (id.isNull())
            {
                continue;
            }
            ++mSearchable;

            auto it = mEntries.find(id);
            if (it == mEntries.end())
            {
                Entry& entry       = mEntries[id];
                entry.id           = id;
                entry.localId      = object->getLocalID();
                entry.regionHandle = object->getRegion()->getHandle();
                entry.position     = positionOf(object);
                continue;
            }
            Entry& entry = it->second;
            entry.cacheOnly = false;    // the viewer has an object of it now
            // Where it is, for when the object goes and the record speaks for it: a record seen only
            // while ACTIVE (visitRecord()) gives no position, and the range would measure from the
            // origin until the record is read again, taking the row out (research/275 L078).
            entry.position = positionOf(object);
            if (entry.request == Entry::Request::Finished)
            {
                tryList(entry, object);
            }
            else if (entry.request == Entry::Request::Need)
            {
                // Not asked yet: where it is now (a region crossing gives it a new local id).
                entry.localId      = object->getLocalID();
                entry.regionHandle = object->getRegion()->getHandle();
            }
        }

        refreshRows();

        // Entries of objects that are gone (killed, or their region left) are forgotten, so the
        // table follows the objects that exist, not every object ever seen. Requests that went
        // unanswered too long give their place in flight back. What is left to ask goes in the
        // queue of its region.
        const F64 now = LLFrameTimer::getElapsedSeconds();
        for (auto late = mLateAnswers.begin(); late != mLateAnswers.end();)
        {
            late = late->second < now ? mLateAnswers.erase(late) : std::next(late);
        }
        mPending = 0;
        mQueue.clear();
        for (auto it = mEntries.begin(); it != mEntries.end();)
        {
            Entry& entry           = it->second;
            LLViewerObject* object = gObjectList.findObject(entry.id);
            if (object && !object->getRegion())
            {
                object = nullptr;
            }
            if (!object && !cacheBacked(entry))
            {
                // Its row went in refreshRows(). No answer will give its place in flight back.
                if (entry.request == Entry::Request::Sent)
                {
                    releaseInFlight(entry);
                }
                rememberLate(entry, now);
                it = mEntries.erase(it);
                continue;
            }
            // No object, but a cache record (looked at in scanCache()): known from that.
            entry.cacheOnly = !object;
            // Left out by the conditions now (they changed): not asked about, not counted as
            // waiting. The entry stays, so that it is asked about if the conditions take it in again.
            // (One asked already keeps its place in flight until it is answered or times out.)
            const bool wanted = wantedNow(entry, object, agentRegion);
            if (wanted && entry.cacheOnly)
            {
                ++mSearchable;
            }
            entry.pendingCounted = false;
            if (entry.request == Entry::Request::Sent && now - entry.sentAt > REQUEST_TIMEOUT)
            {
                releaseInFlight(entry);
                rememberLate(entry, now);   // the first request's answer may still come after the second one's
                entry.request = ++entry.tries < MAX_TRIES ? Entry::Request::Need : Entry::Request::Failed;
                // Asked again where it is now (it may have crossed into another region meanwhile).
                if (object)
                {
                    entry.localId      = object->getLocalID();
                    entry.regionHandle = object->getRegion()->getHandle();
                }
            }
            if (wanted && entry.request == Entry::Request::Need)
            {
                mQueue[entry.regionHandle].push_back(entry.id);
            }
            if (wanted && (entry.request == Entry::Request::Need || entry.request == Entry::Request::Sent))
            {
                ++mPending;
                entry.pendingCounted = true;
            }
            ++it;
        }
        publishCounts();
        mScanTimer.start();
    }

    bool Engine::searchableCached(const Entry& entry, LLViewerRegion* agentRegion) const
    {
        LLViewerRegion* region = LLWorld::getInstance()->getRegionFromHandle(entry.regionHandle);
        if (!region)
        {
            return false;
        }
        if (region != agentRegion && (mCriteria.exceptNeighbor || !plus::isRegionAround(region)))
        {
            return false;
        }
        return !((mCriteria.exceptChild && entry.child)
              || (mCriteria.exceptPhysical && (entry.updateFlags & FLAGS_USE_PHYSICS)))
            && withinRange(nullptr, &entry);
    }

    bool Engine::wantedNow(const Entry& entry, LLViewerObject* object, LLViewerRegion* agentRegion) const
    {
        if (object)
        {
            return searchable(object, agentRegion);
        }
        return cacheBacked(entry) && searchableCached(entry, agentRegion);
    }

    // The objects of the regions searched, from their cache records: the simulator sends every
    // cacheable object as a record, and the viewer makes the object only for what is in view, large
    // enough and not hidden (LLViewerRegion::updateVisibleEntries()). A slice a frame; the place in
    // the pass is kept as a region handle and a local id (the records change meanwhile).
    void Engine::scanCache(LLViewerRegion* agentRegion)
    {
        mCacheRegions.clear();
        for (LLViewerRegion* region : LLWorld::getInstance()->getRegionList())
        {
            if (region == agentRegion || (!mCriteria.exceptNeighbor && plus::isRegionAround(region)))
            {
                mCacheRegions.push_back(region->getHandle());
            }
        }
        std::sort(mCacheRegions.begin(), mCacheRegions.end());

        S32 budget = CACHE_SLICE;
        while (budget > 0)
        {
            const auto at = std::lower_bound(mCacheRegions.begin(), mCacheRegions.end(), mCacheRegion);
            if (at == mCacheRegions.end())
            {
                // A pass is done.
                mCacheRegion = 0;
                mCacheCursor = 0;
                ++mCacheCycle;
                return;
            }
            if (*at != mCacheRegion)
            {
                mCacheRegion = *at;
                mCacheCursor = 0;
            }
            LLViewerRegion* region = LLWorld::getInstance()->getRegionFromHandle(mCacheRegion);
            if (!region)
            {
                ++mCacheRegion;
                mCacheCursor = 0;
                continue;
            }
            const auto& records = region->getVOCacheMap();
            auto record = records.lower_bound(mCacheCursor);
            for (; record != records.end() && budget > 0; ++record, --budget)
            {
                if (LLVOCacheEntry* cached = record->second.get())
                {
                    visitRecord(region, *cached);
                }
            }
            if (record == records.end())
            {
                ++mCacheRegion;     // the next region
                mCacheCursor = 0;
            }
            else
            {
                mCacheCursor = record->first;
            }
        }
    }

    void Engine::visitRecord(LLViewerRegion* region, LLVOCacheEntry& record)
    {
        LLDataPackerBinaryBuffer* dp = record.getDP();
        if (!record.isValid() || !dp)
        {
            return;
        }
        LLUUID id;
        dp->reset();
        dp->unpackUUID(id, "ID");
        dp->reset();
        if (id.isNull())
        {
            return;
        }
        auto it = mEntries.find(id);
        if (record.isState(LLVOCacheEntry::ACTIVE))
        {
            // The viewer has the object; scan() lists that one. Seen, so the entry stays (an
            // object that goes later leaves its record to speak for it).
            if (it != mEntries.end())
            {
                it->second.cached     = true;
                it->second.cacheCycle = mCacheCycle;
            }
            return;
        }
        // A linked prim other than the root: when they are wanted, placed by the root's record
        // (one without a root in the cache, a prim of what an avatar wears, is left out).
        const U32 parent_id    = record.getParentID();
        LLVOCacheEntry* parent = nullptr;
        if (parent_id > 0)
        {
            parent = mCriteria.exceptChild ? nullptr : region->getCacheEntry(parent_id, false);
            if (!parent || !parent->getDP())
            {
                return;
            }
        }
        const U32 flags = record.getUpdateFlags();
        if (mCriteria.exceptPhysical && (flags & FLAGS_USE_PHYSICS))
        {
            return;
        }

        const bool fresh   = it == mEntries.end();
        Entry& entry       = fresh ? mEntries[id] : it->second;
        const bool changed = fresh || !entry.cached || entry.cacheCrc != record.getCRC();
        if (fresh)
        {
            entry.id = id;
        }
        if (fresh || entry.request == Entry::Request::Need)
        {
            entry.localId      = record.getLocalID();
            entry.regionHandle = region->getHandle();
        }
        entry.cached      = true;
        entry.cacheCycle  = mCacheCycle;
        entry.updateFlags = flags;
        entry.child       = parent_id > 0;
        entry.prims       = record.getNumOfChildren() + 1;
        if (changed)
        {
            U8 pcode = 0;
            LLViewerObject::unpackU8(dp, pcode, "PCode");
            LLViewerObject::unpackU8(dp, entry.clickAction, "ClickAction");
            LLVector3 pos, scale;
            LLQuaternion rot;
            LLViewerObject::extractSpatialExtents(parent ? parent->getDP() : dp, pos, scale, rot);
            entry.position = region->getPosGlobalFromRegion(pos);
            entry.cacheCrc = record.getCRC();
            // Only what the viewer would make a selectable object of.
            if (pcode != LL_PCODE_VOLUME && pcode != LL_PCODE_LEGACY_GRASS && pcode != LL_PCODE_LEGACY_TREE)
            {
                entry.cached = false;   // not backed by the record: scan() drops it (when there is no object)
            }
        }
        if (entry.cached && entry.request == Entry::Request::Finished && !entry.listed
            && !gObjectList.findObject(id) && searchableCached(entry, gAgent.getRegion()))
        {
            tryList(entry, nullptr);
        }
    }

    void Engine::releaseInFlight(const Entry& entry)
    {
        const auto it = mRequestsInFlight.find(entry.regionHandle);
        if (it != mRequestsInFlight.end())
        {
            it->second = std::max(it->second - 1, 0);
        }
    }

    // Every frame while something waits: as many requests as each region has room for. A region
    // at its limit is passed over at once (it waits for answers, or for scan() to time them out).
    void Engine::sendRequests()
    {
        const F64 now = LLFrameTimer::getElapsedSeconds();
        for (auto queue = mQueue.begin(); queue != mQueue.end();)
        {
            const U64 handle = queue->first;
            S32& in_flight   = mRequestsInFlight[handle];
            if (in_flight >= MAX_IN_FLIGHT)
            {
                ++queue;
                continue;
            }
            LLViewerRegion* region = LLWorld::getInstance()->getRegionFromHandle(handle);
            if (!region)
            {
                // The region is gone, and its objects with it.
                queue = mQueue.erase(queue);
                continue;
            }
            std::deque<LLUUID>& ids = queue->second;
            std::vector<U32> local_ids;
            while (!ids.empty() && in_flight < MAX_IN_FLIGHT)
            {
                const auto it = mEntries.find(ids.front());
                ids.pop_front();
                // Gone, answered or moved to another region since the queue was made: skipped.
                if (it == mEntries.end() || it->second.request != Entry::Request::Need || it->second.regionHandle != handle)
                {
                    continue;
                }
                Entry& entry = it->second;
                local_ids.push_back(entry.localId);
                entry.request = Entry::Request::Sent;
                entry.sentAt  = now;
                ++in_flight;
            }
            if (!local_ids.empty())
            {
                requestProperties(local_ids, true, region);
                requestProperties(local_ids, false, region);
            }
            queue = ids.empty() ? mQueue.erase(queue) : std::next(queue);
        }
    }

    // ObjectSelect (select) or ObjectDeselect for these objects, in as many messages as it takes.
    void Engine::requestProperties(const std::vector<U32>& localIds, bool select, LLViewerRegion* region)
    {
        LLMessageSystem* msg = gMessageSystem;
        S32 in_message = 0;
        for (U32 local_id : localIds)
        {
            if (in_message == 0)
            {
                msg->newMessageFast(select ? _PREHASH_ObjectSelect : _PREHASH_ObjectDeselect);
                msg->nextBlockFast(_PREHASH_AgentData);
                msg->addUUIDFast(_PREHASH_AgentID, gAgentID);
                msg->addUUIDFast(_PREHASH_SessionID, gAgentSessionID);
            }
            msg->nextBlockFast(_PREHASH_ObjectData);
            msg->addU32Fast(_PREHASH_ObjectLocalID, local_id);
            ++in_message;

            if (msg->isSendFull(nullptr) || in_message >= MAX_OBJECTS_PER_PACKET)
            {
                msg->sendReliable(region->getHost());
                in_message = 0;
            }
        }
        if (in_message > 0)
        {
            msg->sendReliable(region->getHost());
        }
    }

    bool Engine::ownsProperties(LLMessageSystem* msg) const
    {
        const S32 count = msg->getNumberOfBlocksFast(_PREHASH_ObjectData);
        for (S32 i = 0; i < count; ++i)
        {
            LLUUID id;
            msg->getUUIDFast(_PREHASH_ObjectData, _PREHASH_ObjectID, id, i);
            const auto it = mEntries.find(id);
            // Asked for by the search (also a late answer to a request that timed out, or of an
            // entry forgotten meanwhile).
            const bool asked = it != mEntries.end() && it->second.request != Entry::Request::Finished && it->second.sentAt > 0.0;
            if (!asked && !mLateAnswers.count(id))
            {
                return false;
            }
            // Something the agent selected itself needs the official handler.
            LLViewerObject* object = gObjectList.findObject(id);
            if (object && object->isSelected())
            {
                return false;
            }
        }
        return count > 0;
    }

    void Engine::onObjectProperties(LLMessageSystem* msg)
    {
        LLViewerRegion* agentRegion = gAgent.getRegion();
        bool asked_arrived = false;
        mAgentPosition = gAgent.getPositionGlobal();

        const S32 count = msg->getNumberOfBlocksFast(_PREHASH_ObjectData);
        for (S32 i = 0; i < count; ++i)
        {
            LLUUID id;
            msg->getUUIDFast(_PREHASH_ObjectData, _PREHASH_ObjectID, id, i);
            if (id.isNull())
            {
                continue;
            }
            // (A late answer's mark stays until it runs out, scan(): the same object may have been
            // asked about twice, and the second answer is the search's too.)
            LLViewerObject* object = gObjectList.findObject(id);
            if (!object)
            {
                // Not made by the viewer: still ours when the cache record is what we know it by.
                const auto known = mEntries.find(id);
                if (known == mEntries.end() || !cacheBacked(known->second))
                {
                    continue;
                }
            }

            // Properties of objects nobody asked for (the agent selected them, or an answer
            // that came after its entry was forgotten) are kept as well.
            Entry& entry = mEntries[id];
            if (entry.request == Entry::Request::Finished)
            {
                continue;
            }
            const bool wanted = wantedNow(entry, object, agentRegion);
            if (entry.id.isNull())
            {
                entry.id = id;
            }
            else if (entry.request == Entry::Request::Need || entry.request == Entry::Request::Sent)
            {
                // Counted as waiting by scan() when the conditions wanted it then (not by what
                // they say now: the agent may have moved across the range since); only one that
                // was asked for takes a place in flight.
                if (entry.pendingCounted)
                {
                    entry.pendingCounted = false;
                    mPending = std::max(mPending - 1, 0);
                }
                if (entry.request == Entry::Request::Sent)
                {
                    releaseInFlight(entry);
                }
                asked_arrived = true;
            }
            entry.request = Entry::Request::Finished;

            U32 base_mask = 0, owner_mask = 0, group_mask = 0, everyone_mask = 0, next_owner_mask = 0;
            msg->getUUIDFast(_PREHASH_ObjectData, _PREHASH_CreatorID, entry.creatorId, i);
            msg->getUUIDFast(_PREHASH_ObjectData, _PREHASH_OwnerID, entry.ownerId, i);
            msg->getUUIDFast(_PREHASH_ObjectData, _PREHASH_GroupID, entry.groupId, i);
            msg->getUUIDFast(_PREHASH_ObjectData, _PREHASH_LastOwnerID, entry.lastOwnerId, i);
            msg->getU32Fast(_PREHASH_ObjectData, _PREHASH_BaseMask, base_mask, i);
            msg->getU32Fast(_PREHASH_ObjectData, _PREHASH_OwnerMask, owner_mask, i);
            msg->getU32Fast(_PREHASH_ObjectData, _PREHASH_GroupMask, group_mask, i);
            msg->getU32Fast(_PREHASH_ObjectData, _PREHASH_EveryoneMask, everyone_mask, i);
            msg->getU32Fast(_PREHASH_ObjectData, _PREHASH_NextOwnerMask, next_owner_mask, i);
            entry.saleInfo.unpackMultiMessage(msg, _PREHASH_ObjectData, i);
            entry.aggregatePerms.unpackMessage(msg, _PREHASH_ObjectData, _PREHASH_AggregatePerms, i);
            entry.category.unpackMultiMessage(msg, _PREHASH_ObjectData, i);
            msg->getStringFast(_PREHASH_ObjectData, _PREHASH_Name, entry.name, i);
            msg->getStringFast(_PREHASH_ObjectData, _PREHASH_Description, entry.description, i);

            entry.ownerMask = owner_mask;
            entry.permissions.init(entry.creatorId, entry.ownerId, entry.lastOwnerId, entry.groupId);
            entry.permissions.initMasks(base_mask, owner_mask, everyone_mask, group_mask, next_owner_mask);
            // Who owns it, and whether that is the group.
            entry.permissions.getOwnership(entry.ownershipId, entry.groupOwned);

            if (wanted)
            {
                tryList(entry, object);
            }
        }
        if (asked_arrived)
        {
            publishCounts();
        }
    }

    // Lists the object when it passes every condition and is not listed already.
    void Engine::tryList(Entry& entry, LLViewerObject* object)
    {
        if (entry.listed)
        {
            return;
        }
        const Criteria& c = mCriteria;

        if (c.forSale && !(entry.saleInfo.isForSale()
                           && entry.saleInfo.getSalePrice() >= c.priceMin && entry.saleInfo.getSalePrice() <= c.priceMax))
        {
            return;
        }
        if (c.distance)
        {
            const S32 metres = static_cast<S32>(distanceTo(gAgent.getPositionGlobal(), entry, object));
            if (metres < c.distanceMin || metres > c.distanceMax)
            {
                return;
            }
        }
        // What the object says, or the cache record (no object): never temporary, worn or a probe,
        // since the cache holds none of those.
        const bool touch    = object ? object->flagHandleTouch() : (entry.updateFlags & FLAGS_HANDLE_TOUCH) != 0;
        const S32 click     = object ? object->getClickAction() : entry.clickAction;
        const bool physical = object ? object->flagUsePhysics() : (entry.updateFlags & FLAGS_USE_PHYSICS) != 0;
        const bool phantom  = object ? object->flagPhantom() : (entry.updateFlags & FLAGS_PHANTOM) != 0;
        if (c.clickAction > 0)
        {
            switch (c.clickAction)
            {
            case 1: // any
                if (!(touch || click != 0))
                {
                    return;
                }
                break;
            case 2: // touch is a flag of its own
                if (!touch)
                {
                    return;
                }
                break;
            default: // the viewer's click actions follow touch (sit, buy, ...)
                if (c.clickAction - 2 != click)
                {
                    return;
                }
                break;
            }
        }
        if ((c.onlyPhysical && !physical)
            || (c.onlyTemporary && !(object && object->flagTemporaryOnRez()))
            || (c.onlyLocked && (entry.ownerMask & PERM_MOVE))
            || (c.onlyPhantom && !phantom)
            || (c.onlyAttachment && !(object && object->isAttachment()))
            || (c.onlyProbe && !(object && object->mReflectionProbe.notNull()))
            || (c.onlyCopy && !(entry.ownerMask & PERM_COPY))
            || (c.onlyModify && !(entry.ownerMask & PERM_MODIFY))
            || (c.onlyTransfer && !(entry.ownerMask & PERM_TRANSFER)))
        {
            return;
        }
        if (c.onlyMedia)
        {
            if (!object)
            {
                return;     // the faces are not in the record
            }
            bool media = false;
            for (U8 te = 0; te < object->getNumTEs() && !media; ++te)
            {
                const LLTextureEntry* texture_entry = object->getTE(te);
                media = texture_entry && texture_entry->hasMedia();
            }
            if (!media)
            {
                return;
            }
        }
        if (c.onlyParcel && !LLViewerParcelMgr::instance().inAgentParcel(object ? positionOf(object) : entry.position))
        {
            return;
        }

        if (c.ownerId.notNull() && (entry.groupOwned || entry.ownershipId != c.ownerId))
        {
            return;
        }

        // The texts the simulator sent first: no row is made for what fails on those.
        if (!textMatches(entry.name, TEXT_NAME) || !textMatches(entry.description, TEXT_DESCRIPTION))
        {
            return;
        }
        // Names, which the name cache may not have yet (they match "(waiting)" until it has).
        const Row row = makeRow(entry, object);
        if (!textMatches(row.owner, TEXT_OWNER) || !textMatches(row.group, TEXT_GROUP)
            || !textMatches(row.creator, TEXT_CREATOR) || !textMatches(row.lastOwner, TEXT_LAST_OWNER)
            || !anyTextMatches(row))
        {
            return;
        }

        entry.listed        = true;
        entry.shownDistance = row.distance;
        entry.shownCost     = row.landImpact;
        ++mListed;
        if (mCallbacks.rowAdded)
        {
            mCallbacks.rowAdded(row);
        }
    }

    Row Engine::makeRow(Entry& entry, LLViewerObject* object)
    {
        Row row;
        row.id          = entry.id;
        row.distance    = static_cast<S32>(distanceTo(gAgent.getPositionGlobal(), entry, object) + 0.5f);
        row.name        = entry.name;
        row.description = entry.description;
        row.price       = entry.saleInfo.isForSale() ? entry.saleInfo.getSalePrice() : -1;
        // Without an object the land impact is not known (the cost service answers for objects).
        const F32 cost  = object ? object->getLinksetCost() : 0.f;
        row.landImpact  = cost > F_ALMOST_ZERO ? cost : 0.f;
        row.prims       = object ? object->numChildren() + 1 : entry.prims;
        row.temporary   = object && object->flagTemporaryOnRez();
        row.physical    = object ? object->flagUsePhysics() : (entry.updateFlags & FLAGS_USE_PHYSICS) != 0;

        bool pending = false;
        row.owner     = nameOf(entry.ownershipId, entry.groupOwned, pending, true);
        row.group     = nameOf(entry.groupId, true, pending);
        // A creator who is the owner names the owner too.
        row.creator   = nameOf(entry.creatorId, false, pending, !entry.groupOwned && entry.creatorId == entry.ownershipId);
        row.lastOwner = nameOf(entry.lastOwnerId, false, pending, true);
        entry.namesPending = pending;
        return row;
    }

    // Rows of objects that are gone are removed; distance and land impact (which the
    // simulator's cost service answers after the row was made) follow.
    // The conditions are not checked again here: a row stays when the agent walks out of the
    // distance range or off the parcel ("only this parcel"), or the price changes, until the
    // next search. Firestorm's area search does the same (FSAreaSearch::matchObject() passes
    // over a listed object; FSPanelAreaSearchList::updateScrollList() removes only the rows of
    // objects that are gone, and its parcel observer only checks for a new region).
    // The search range (PlusAreaSearchRange) is not one of those conditions: it is part of what
    // is searched (wantedNow()), so a row beyond it goes here.
    void Engine::refreshRows()
    {
        LLViewerRegion* agentRegion = gAgent.getRegion();
        const LLVector3d agent_position = gAgent.getPositionGlobal();
        std::vector<LLUUID> removed;
        for (auto& pair : mEntries)
        {
            Entry& entry = pair.second;
            if (!entry.listed)
            {
                continue;
            }
            LLViewerObject* object = gObjectList.findObject(entry.id);
            if (!wantedNow(entry, object, agentRegion))
            {
                entry.listed = false;
                --mListed;
                removed.push_back(entry.id);
                continue;
            }
            const S32 distance = static_cast<S32>(distanceTo(agent_position, entry, object) + 0.5f);
            const F32 cost     = object ? object->getLinksetCost() : 0.f;
            if (distance != entry.shownDistance || cost != entry.shownCost)
            {
                entry.shownDistance = distance;
                entry.shownCost     = cost;
                if (mCallbacks.rowChanged)
                {
                    mCallbacks.rowChanged(makeRow(entry, object));
                }
            }
        }
        if (!removed.empty() && mCallbacks.rowsRemoved)
        {
            mCallbacks.rowsRemoved(removed);
        }
    }

    // The name of an avatar or group; "(waiting)" and an asked-for name until the cache has it.
    // owner: the name of the owner or the last owner (or of a creator who is the owner), which
    // RLVa's @shownames hides (Firestorm's RLVa_hideNameIfRestricted() of fsareasearch.cpp, for
    // the owner and the last owner; a group's name is not an avatar's and is shown).
    std::string Engine::nameOf(const LLUUID& id, bool group, bool& pending, bool owner)
    {
        if (id.isNull())
        {
            return std::string();
        }

        std::string name;
        if (group)
        {
            if (gCacheName->getGroupName(id, name))
            {
                return name;
            }
        }
        else
        {
            LLAvatarName avatar_name;
            if (LLAvatarNameCache::get(id, &avatar_name))
            {
                // The text conditions, the quick filter and the clipboard see the anonym too.
                return (owner && plus::rlv::enabled()) ? plus::rlv::avatarNameForPlus(id, avatar_name.getCompleteName())
                                                       : avatar_name.getCompleteName();
            }
        }

        pending = true;
        // Once a search. One connection a name: a request still on its way is cut before another
        // is made (search() forgets what was asked, not the connections; one replaced in the map
        // could no longer be cut, and would call a deleted engine after the window closed), and
        // it is not replaced at all before NAME_RETRY (a name that never came is asked again).
        const F64 now  = LLFrameTimer::getElapsedSeconds();
        const auto old = mNameConnections.find(id);
        const bool may_ask = old == mNameConnections.end() || now - old->second.askedAt >= NAME_RETRY;
        if (may_ask && mNamesAsked.insert(id).second)
        {
            if (old != mNameConnections.end())
            {
                old->second.connection.disconnect();
                mNameConnections.erase(old);
            }
            NameRequest request;
            request.askedAt = now;
            if (group)
            {
                request.connection = gCacheName->get(id, true,
                    [this](const LLUUID& named, const std::string&, bool) { onNameArrived(named); });
            }
            else
            {
                request.connection = LLAvatarNameCache::get(id,
                    [this](const LLUUID& named, const LLAvatarName&) { onNameArrived(named); });
            }
            mNameConnections[id] = request;
        }
        return waitingName();
    }

    // A name came: noted, and the rows are looked at once for all the names of the frame on the
    // next tick (applyArrivedNames()); one pass over every entry a name would make a list of
    // many objects (and many owners) slow while their names come in.
    void Engine::onNameArrived(const LLUUID& id)
    {
        const auto request = mNameConnections.find(id);
        if (request != mNameConnections.end())
        {
            request->second.connection.disconnect();
            mNameConnections.erase(request);
        }
        mArrivedNames.insert(id);
    }

    void Engine::applyArrivedNames()
    {
        std::set<LLUUID> arrived;
        arrived.swap(mArrivedNames);
        const auto named = [&arrived](const Entry& entry)
        {
            return arrived.count(entry.ownershipId) || arrived.count(entry.groupId) || arrived.count(entry.creatorId)
                   || arrived.count(entry.lastOwnerId);
        };

        LLViewerRegion* agentRegion = gAgent.getRegion();
        for (auto& pair : mEntries)
        {
            Entry& entry = pair.second;
            if (entry.listed)
            {
                if (named(entry))
                {
                    LLViewerObject* object = gObjectList.findObject(entry.id);
                    if ((object || cacheBacked(entry)) && mCallbacks.rowChanged)
                    {
                        mCallbacks.rowChanged(makeRow(entry, object));
                    }
                }
            }
            else if (entry.namesPending && named(entry))
            {
                // It did not match, or not yet, with the name still missing.
                LLViewerObject* object = gObjectList.findObject(entry.id);
                if (wantedNow(entry, object, agentRegion))
                {
                    tryList(entry, object);
                }
            }
        }
    }

    void Engine::clearRows()
    {
        mListed = 0;
        if (mCallbacks.cleared)
        {
            mCallbacks.cleared();
        }
        publishCounts();
    }

    void Engine::publishCounts()
    {
        if (mCallbacks.counts)
        {
            mCallbacks.counts(mListed, mPending, mSearchable);
        }
    }

    bool Engine::textMatches(const std::string& text, S32 i) const
    {
        if (mNeedle[i].empty())
        {
            return true;
        }
        if (mCriteria.regex)
        {
            try
            {
                return boost::regex_match(text, mPattern[i]);
            }
            catch (const std::exception&)
            {
                // A pattern too complex for the matcher matches nothing.
                return false;
            }
        }
        return lowerAscii(text).find(mNeedle[i]) != std::string::npos;
    }

    bool Engine::anyTextMatches(const Row& row) const
    {
        if (mAnyNeedles.empty())
        {
            return true;
        }
        // Each text lower-cased once for all the words. A name the cache does not have yet counts as
        // empty (the placeholder is no word to search for); applyArrivedNames() judges the row again
        // when it comes.
        const std::string& waiting = waitingName();
        const auto lowered = [&waiting](const std::string& text) { return text == waiting ? std::string() : lowerAscii(text); };
        const std::string texts[] = { lowerAscii(row.name), lowerAscii(row.description), lowered(row.owner),
                                      lowered(row.group),   lowered(row.creator),        lowered(row.lastOwner) };
        // Every word is in one of the texts (not all in the same one): AND.
        for (const std::string& needle : mAnyNeedles)
        {
            bool found = false;
            for (const std::string& text : texts)
            {
                if (text.find(needle) != std::string::npos)
                {
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                return false;
            }
        }
        return true;
    }
}

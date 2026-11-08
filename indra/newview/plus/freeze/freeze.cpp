/**
 * @file freeze.cpp
 * @brief World freeze: stop the world's time, keep the camera, the UI and
 *        the settings live.
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

// How it works
// ------------
// The official clock (gFrameTimeSeconds, gFrameIntervalSeconds) is not touched:
// the camera's smoothing, the UI and a lot of bookkeeping (object and face update
// times, the texture and mesh queues) run on it. Instead, what moves in the
// world is stopped one by one:
//
//   avatars' animations     the time factor of every motion controller is set to
//                           0 (what Develop > Animation speed does). The avatars
//                           still go through LLVOAvatar::updateCharacter(), so the
//                           pose is worked out again each frame at the same
//                           animation time: the camera gaze keeps following.
//   avatars' movement       their velocity is zeroed every frame (the dead reckoning that
//                           fills the time between two updates would carry them on)
//   moving objects, flexi   LLViewerObjectList::update() already has a branch for
//   prims, texture anims    the snapshot's FreezeTime setting that runs only the
//                           avatars' idleUpdate(); the <plus> hook adds our flag
//                           to that condition (FreezeTime itself is not set: it
//                           also stops the camera's follow code).
//   what the simulator      terse updates (movement) and, with PlusWorldFreezeAppearance,
//   sends                   full updates of the objects already shown (their looks:
//                           colour, full bright, glow, light, textures, shape) are
//                           held here (the latest per object), applied when the
//                           world resumes; see below
//   particles               LLWorld::updateParticles() returns while frozen (the
//                           particles that exist stay where they are)
//   clouds                  LLEnvironment::pauseCloudScroll() (if it was not paused)
//   waves                   the water shader clocks use worldSeconds()
//
// Held updates. The network keeps being read. Two kinds of update are copied into
// a table keyed by the simulator and the object's local ID, newer ones replacing
// older ones, so the table grows with the number of objects that changed and never
// with the time (a script that flashes a light ten times a second keeps one entry):
//
//   terse update   position, rotation, velocity: the only message that moves an
//                  object (ImprovedTerseObjectUpdate). Held whole, from the hook at
//                  the start of LLViewerObjectList::processObjectUpdate().
//   full update    everything about an object (ObjectUpdate, ObjectUpdateCompressed).
//                  A script's change of colour, alpha, full bright, glow, point light,
//                  texture or shape comes this way, so a "lightning" that switches
//                  full bright on and off kept flashing while the rest stood still.
//                  Held per object block, from the hook at the top of the message's
//                  loop, only for an object that is already shown and only when
//                  nothing else depends on it (see holdable()): an object that first
//                  comes while frozen, an avatar, an attachment, an object the user
//                  has selected, a change of parent or of region always go through,
//                  and so do kills, properties, chat, the avatars' appearance.
//
// Resuming applies the table through the code the message would have used, a few
// ms per frame: a terse update through LLViewerObject::processUpdateMessage() and the
// rest of processUpdateCore(); a compressed full update through
// LLViewerRegion::cacheFullUpdate(), which is what processObjectUpdate() does with it
// (the object cache is updated and the object is brought up to date from it). The
// updates that cannot be kept as bytes (an ObjectUpdate without compression reads
// its fields from the message; a temporary object's compressed update is applied
// from the message too; a terse update that carries a texture entry) keep only the
// ID: such objects are asked for again with the viewer's own cache-miss request
// (RequestMultipleObjects) and the simulator sends them as they are now. A live
// full update that arrives after the world resumes, for an object in the table, removes
// its entry (the live one is newer and whole); a live terse update replaces only the
// held terse one: the rest of the entry (a held full update, a request to ask again)
// is applied first, then the live update goes through. A kill removes the entry of the killed object
// (the <plus> hook in process_kill_object(), onObjectKilled()), so temporary objects
// that come and go while frozen do not pile up.
//
// A long freeze. After PlusWorldFreezeMaxSeconds, with more than
// PlusWorldFreezeMaxPending objects held, or with more than MAX_HELD_BYTES of held
// data, the held data is dropped and only the IDs are kept; those objects are asked
// for again when the world resumes. The table stays within PlusWorldFreezeMaxPending
// after that too: an update of an object not in it marks its region instead, and
// every object of a marked region that is in the scene is asked for again when the
// world resumes (one only in the object cache then, whose update did not fit, comes
// back from the cache as it was until the simulator sends it again).

#include "llviewerprecompiledheaders.h"

#include "freeze.h"

#include "plus.h"
#include "plus/localmesh/localmesh.h"

#include "llagent.h"
#include "llcallbacklist.h"
#include "llcharacter.h"
#include "lldatapacker.h"
#include "llenvironment.h"
#include "llframetimer.h"
#include "llmotioncontroller.h"
#include "llrhi.h"
#include "lltimer.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llvoavatar.h"
#include "llvocache.h"
#include "llworld.h"
#include "message.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    constexpr char SETTING_FROZEN[]      = "PlusWorldFreeze";
    constexpr char SETTING_APPEARANCE[]  = "PlusWorldFreezeAppearance";
    constexpr char SETTING_MAX_SECONDS[] = "PlusWorldFreezeMaxSeconds";
    constexpr char SETTING_MAX_PENDING[] = "PlusWorldFreezeMaxPending";

    constexpr S32    MAX_UPDATE_BYTES   = 2048;        // the official buffer (processObjectUpdate)
    constexpr size_t MAX_HELD_BYTES     = 64u << 20;   // all the held payloads together
    constexpr F64    DRAIN_BUDGET_SECS  = 0.002;       // per frame, once the world resumes
    constexpr S32    DRAIN_MIN_PER_TICK = 50;

    // One simulator's object: the simulator's address and the object's local ID.
    struct Key
    {
        U64 host;
        U32 local_id;
        bool operator==(const Key& o) const { return host == o.host && local_id == o.local_id; }
    };
    struct KeyHash
    {
        size_t operator()(const Key& k) const
        {
            return static_cast<size_t>(k.host * 0x9E3779B97F4A7C15ull) ^ (static_cast<size_t>(k.local_id) * 0x85EBCA6Bu);
        }
    };

    // What is held for one object: the latest of each kind of update.
    struct Held
    {
        U64 region_handle = 0;
        std::vector<U8> terse;      // the terse block's Data (starts with the local ID); newer than `full`
        std::vector<U8> full;       // a compressed full update's Data (starts with the full ID)
        LLUUID full_id;             // the object `full` was for
        U32 full_flags = 0;         // its UpdateFlags
        bool ask_again = false;     // something could not be kept as bytes: the object is asked for again
    };

    bool sFrozen = false;
    bool sDropped = false;                     // payloads dropped: IDs only, asked for again on resume
    F64  sFrozenSince = 0.0;                   // viewer seconds when the freeze began
    F64  sFrozenTotal = 0.0;                   // viewer seconds spent in earlier freezes
    std::unordered_map<Key, Held, KeyHash> sHeld;
    size_t sHeldBytes = 0;                     // the bytes of all the held payloads
    std::unordered_set<Key, KeyHash> sBorn;    // objects that first came while frozen: their full updates go through
    std::unordered_set<U64> sOverflowRegions;  // regions with updates the full table could not take: asked for whole on resume

    // What the freeze changed, to be put back.
    F32  sSavedCurrentFactor = 1.f;
    std::unordered_map<LLCharacter*, F32> sSavedFactors;
    bool sPausedClouds = false;

    bool sTickRegistered = false;

    // research/218 vk-s0b: the measuring tools' cue (tools/dummy-sim/run-local.ps1 -Freeze). With
    // LLRHI_CUE_DIR, <dir>/worldfreeze.on freezes the world and <dir>/worldfreeze.off resumes it
    // (each file taken and removed; looked for once a second). A freeze from the cue also pins the
    // world's clock (worldSeconds(): the waves) to CUE_WORLD_SECONDS, so two starts frozen the
    // same way show the same water.
    constexpr char   CUE_FREEZE[]       = "worldfreeze.on";
    constexpr char   CUE_RESUME[]       = "worldfreeze.off";
    constexpr F64    CUE_POLL_SECS      = 1.0;
    constexpr F64    CUE_WORLD_SECONDS  = 600.0;
    bool sPinned = false;   // this freeze came from the cue: worldSeconds() is CUE_WORLD_SECONDS

    Key makeKey(LLMessageSystem* msg, U32 local_id)
    {
        return { (static_cast<U64>(msg->getSenderIP()) << 32) | static_cast<U64>(msg->getSenderPort()), local_id };
    }

    bool holdAppearance()
    {
        static LLCachedControl<bool> hold(gSavedSettings, SETTING_APPEARANCE, true);
        return hold;
    }

    // Asked every frame while frozen (tick()): a cached control, as maxPending().
    F32 maxSeconds()
    {
        static LLCachedControl<F32> max_seconds(gSavedSettings, SETTING_MAX_SECONDS, 300.f);
        return max_seconds;
    }

    // Asked for every held update (slotFor()): a cached control, not a lookup by name.
    S32 maxPending()
    {
        static LLCachedControl<S32> max_pending(gSavedSettings, SETTING_MAX_PENDING, 100000);
        return max_pending;
    }

    // The payloads go through these two, so sHeldBytes stays the sum of their sizes.
    void keepBytes(std::vector<U8>& bytes, const U8* data, S32 size)
    {
        sHeldBytes -= bytes.size();
        bytes.assign(data, data + size);
        sHeldBytes += bytes.size();
    }

    void freeBytes(std::vector<U8>& bytes)
    {
        sHeldBytes -= bytes.size();
        std::vector<U8>().swap(bytes);
    }

    void forget(const Key& key)
    {
        auto it = sHeld.find(key);
        if (it != sHeld.end())
        {
            freeBytes(it->second.terse);
            freeBytes(it->second.full);
            sHeld.erase(it);
        }
    }

    void dropPayloads()
    {
        for (auto& entry : sHeld)
        {
            freeBytes(entry.second.terse);
            freeBytes(entry.second.full);
            entry.second.ask_again = true;
        }
        sDropped = true;
        LL_INFOS("WorldFreeze") << "the freeze is long (or the table is big): " << sHeld.size()
                                << " objects will be asked for again when the world resumes" << LL_ENDL;
    }

    // The entry a new update of `key` goes into. When the table would grow past its
    // limits the payloads are dropped first (sDropped then says to keep the ID only).
    // nullptr when the table is full even as IDs: the object's region is then asked for
    // whole when the world resumes (the update is not kept).
    Held* slotFor(const Key& key, U64 region_handle, size_t incoming_bytes)
    {
        const bool full = static_cast<S32>(sHeld.size()) >= maxPending() && sHeld.find(key) == sHeld.end();
        if (!sDropped && (full || sHeldBytes + incoming_bytes > MAX_HELD_BYTES))
        {
            dropPayloads();
        }
        if (full)
        {
            sOverflowRegions.insert(region_handle);
            return nullptr;
        }
        Held& held = sHeld[key];
        held.region_handle = region_handle;
        return &held;
    }

    // Whether a full update of this (existing) object can wait for the world to
    // resume: only when it changes nothing else depends on. The rest goes through
    // as always (and replaces what is held for the object).
    bool holdable(LLViewerObject* objectp, LLViewerRegion* regionp, U32 local_id, U32 parent_id)
    {
        if (objectp->isDead() || objectp->getRegion() != regionp || objectp->getLocalID() != local_id)
        {
            return false;   // a region crossing or a new local ID: the official code moves it
        }
        if (objectp->isAvatar() || objectp->isAttachment() || objectp->getAvatarAncestor())
        {
            return false;   // avatars and what they wear: appearance and attachments are not held
        }
        if (objectp->isSelected())
        {
            return false;   // the user is editing it: the edit must show
        }
        const LLViewerObject* parentp = static_cast<const LLViewerObject*>(objectp->getParent());
        const U32 current_parent = parentp ? parentp->getLocalID() : 0;
        return current_parent == parent_id;     // linked, unlinked, or an orphan's parent came: go through
    }

    // Applies a held terse update the way processObjectUpdate() applies the
    // message: the same cache update and the same processUpdateCore(), from the
    // held bytes (no message, as for the updates that come from the cache).
    void applyTerse(const Key& key, const std::vector<U8>& data, LLViewerRegion* regionp)
    {
        if (data.size() < sizeof(U32) || data.size() > MAX_UPDATE_BYTES)
        {
            return;
        }

        LLUUID fullid;
        gObjectList.getUUIDFromLocal(fullid, key.local_id, static_cast<U32>(key.host >> 32), static_cast<U32>(key.host & 0xFFFFFFFFu));
        if (fullid.isNull())
        {
            return;
        }
        LLViewerObject* objectp = gObjectList.findObject(fullid);
        if (!objectp || objectp->isDead())
        {
            return;
        }
        objectp = regionp->updateCacheEntry(key.local_id, objectp);
        if (!objectp || objectp->isDead() || objectp->getRegion() != regionp || objectp->getLocalID() != key.local_id)
        {
            return;
        }

        U8 buffer[MAX_UPDATE_BYTES];
        const S32 size = static_cast<S32>(data.size());
        std::memcpy(buffer, data.data(), data.size());
        LLDataPackerBinaryBuffer dp(buffer, size);
        U32 local_id = 0;
        dp.unpackU32(local_id, "LocalID");

        // processUpdateCore() cannot be used: its virtual processUpdateMessage() has
        // subclass code (LLVOVolume reads the TextureEntry from the message for a
        // terse update too) that needs the message, and there is none any more.
        // The position part is the base class's, so the base class does it, then
        // the rest of processUpdateCore() is repeated. (A terse update that carried
        // a texture entry was marked to be asked for again instead.) Not to an object
        // showing a local mesh, as LLVOVolume's own hook does not: it keeps the place a
        // local scale moved it to, and asks for the simulator's state when the mesh is
        // taken off (plus/localmesh; fix-round9b 5).
        if (!plus::localmesh::holdsUpdates(objectp))
        {
            objectp->LLViewerObject::processUpdateMessage(nullptr, nullptr, 0, OUT_TERSE_IMPROVED, &dp);
        }
        if (objectp->isDead())
        {
            return;
        }
        gObjectList.updateActive(objectp);
        objectp->setPixelAreaAndAngle(gAgent);
        gObjectList.findOrphans(objectp, regionp->getHost().getAddress(), regionp->getHost().getPort());
        objectp->setLastUpdateType(OUT_TERSE_IMPROVED);
    }

    // Applies a held compressed full update the way processObjectUpdate() applies
    // one for an object the cache keeps: LLViewerRegion::cacheFullUpdate(), which
    // updates the cache entry and, for an object in the scene, the object from it
    // (processObjectUpdateFromCache()). Only while that object is still the one the
    // update was for: a killed object is not brought back. An object that left the
    // scene meanwhile (out of the camera's reach) keeps its cache entry: that is
    // brought up to date, so the object comes back as it is now.
    void applyFull(const Key& key, const Held& held, LLViewerRegion* regionp)
    {
        if (held.full.size() < sizeof(LLUUID) + sizeof(U32) + sizeof(U8) || held.full.size() > MAX_UPDATE_BYTES)
        {
            return;
        }
        if (LLViewerObject* objectp = gObjectList.findObject(held.full_id))
        {
            if (objectp->isDead() || objectp->getRegion() != regionp || objectp->getLocalID() != key.local_id)
            {
                return;
            }
        }
        else
        {
            LLVOCacheEntry* entry = regionp->getCacheEntry(key.local_id, false);
            LLDataPackerBinaryBuffer* cached = entry ? entry->getDP() : nullptr;
            if (!cached)
            {
                return;     // killed (the kill removes the entry too)
            }
            LLUUID cached_id;
            cached->reset();
            cached->unpackUUID(cached_id, "ID");
            cached->reset();
            if (cached_id != held.full_id)
            {
                return;     // the local ID went to another object
            }
        }

        U8 buffer[MAX_UPDATE_BYTES];
        const S32 size = static_cast<S32>(held.full.size());
        std::memcpy(buffer, held.full.data(), held.full.size());
        LLDataPackerBinaryBuffer dp(buffer, size);
        LLUUID fullid;
        U32 local_id = 0;
        U8 pcode = 0;
        dp.unpackUUID(fullid, "ID");
        dp.unpackU32(local_id, "LocalID");
        dp.unpackU8(pcode, "PCode");
        if (pcode == 0 || fullid != held.full_id)
        {
            return;
        }
        regionp->cacheFullUpdate(dp, held.full_flags);
    }

    // One held entry, in the order the updates came: the full update (if any) is
    // older than the terse one (a full update clears the terse one it replaces).
    void applyEntry(const Key& key, const Held& held, std::vector<LLViewerRegion*>& ask_regions)
    {
        LLViewerRegion* regionp = LLWorld::instanceExists() ? LLWorld::getInstance()->getRegionFromHandle(held.region_handle) : nullptr;
        if (!regionp)
        {
            return;
        }
        if (!held.full.empty())
        {
            applyFull(key, held, regionp);
        }
        if (!held.terse.empty())
        {
            applyTerse(key, held.terse, regionp);
        }
        if (held.ask_again)
        {
            regionp->addCacheMissFull(key.local_id);
            bool known = false;
            for (LLViewerRegion* r : ask_regions)
            {
                known = known || (r == regionp);
            }
            if (!known)
            {
                ask_regions.push_back(regionp);
            }
        }
    }

    // Asks the simulators for the objects whose held update was dropped, and for every
    // object of the regions whose updates did not fit in the table.
    void askAgain()
    {
        std::vector<LLViewerRegion*> regions;
        size_t asked = 0;
        auto ask = [&regions, &asked](LLViewerRegion* regionp, U32 local_id) {
            regionp->addCacheMissFull(local_id);
            ++asked;
            if (std::find(regions.begin(), regions.end(), regionp) == regions.end())
            {
                regions.push_back(regionp);
            }
        };
        if (LLWorld::instanceExists())
        {
            // Each held object, also of a marked region (it may be out of the scene, in the
            // object cache only, where the pass below does not reach).
            for (const auto& entry : sHeld)
            {
                if (LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(entry.second.region_handle))
                {
                    ask(regionp, entry.first.local_id);
                }
            }
            // Then the other objects of the marked regions that are in the scene.
            if (!sOverflowRegions.empty())
            {
                const S32 count = gObjectList.getNumObjects();
                for (S32 i = 0; i < count; ++i)
                {
                    LLViewerObject* objectp = gObjectList.getObject(i);
                    LLViewerRegion* regionp = objectp && !objectp->isDead() ? objectp->getRegion() : nullptr;
                    if (!regionp || !sOverflowRegions.count(regionp->getHandle()))
                    {
                        continue;
                    }
                    const LLHost& host = regionp->getHost();
                    const Key key{ (static_cast<U64>(host.getAddress()) << 32) | static_cast<U64>(host.getPort()), objectp->getLocalID() };
                    if (!sHeld.count(key))
                    {
                        ask(regionp, objectp->getLocalID());
                    }
                }
            }
        }
        for (LLViewerRegion* regionp : regions)
        {
            regionp->requestCacheMisses();
        }
        LL_INFOS("WorldFreeze") << "asked for " << asked << " objects again (" << sOverflowRegions.size()
                                << " regions whole)" << LL_ENDL;
        sHeld.clear();
        sHeldBytes = 0;
        sDropped = false;
        sOverflowRegions.clear();
    }

    // An avatar's idleUpdate() (it still runs while frozen, for the pose and the
    // gaze) moves it on by its last velocity for a few seconds, as it does between
    // two updates from the simulator. No velocity, no drift.
    void stopAvatars()
    {
        for (LLCharacter* character : LLCharacter::sInstances)
        {
            LLVOAvatar* avatar = dynamic_cast<LLVOAvatar*>(character);
            if (!avatar || avatar->isDead())
            {
                continue;
            }
            if (!avatar->getVelocity().isExactlyZero() || !avatar->getAcceleration().isExactlyZero())
            {
                avatar->setVelocity(LLVector3::zero);
                avatar->setAcceleration(LLVector3::zero);
            }
            if (!avatar->getAngularVelocity().isExactlyZero())
            {
                avatar->setAngularVelocity(LLVector3::zero);
            }
        }
    }

    // The worked-through part of resuming, a few ms per frame.
    void drain()
    {
        const F64 start = LLTimer::getTotalSeconds();
        std::vector<LLViewerRegion*> ask_regions;
        S32 done = 0;
        while (!sHeld.empty())
        {
            auto it = sHeld.begin();
            applyEntry(it->first, it->second, ask_regions);
            // applyEntry() does not touch the table (no update is held while the world runs)
            freeBytes(it->second.terse);
            freeBytes(it->second.full);
            sHeld.erase(it);
            if (++done >= DRAIN_MIN_PER_TICK && LLTimer::getTotalSeconds() - start > DRAIN_BUDGET_SECS)
            {
                break;
            }
        }
        for (LLViewerRegion* regionp : ask_regions)
        {
            regionp->requestCacheMisses();
        }
    }

    void applyFrozen(bool on)
    {
        if (on == sFrozen)
        {
            return;
        }

        if (on)
        {
            sFrozen = true;
            sDropped = false;
            sFrozenSince = LLFrameTimer::getElapsedSeconds();
            sBorn.clear();
            sOverflowRegions.clear();

            // Animations: every motion controller's time factor to 0 (the controllers
            // made while frozen take the default for new ones).
            sSavedCurrentFactor = LLMotionController::getCurrentTimeFactor();
            sSavedFactors.clear();
            for (LLCharacter* character : LLCharacter::sInstances)
            {
                sSavedFactors[character] = character->getMotionController().getTimeFactor();
                character->setAnimTimeFactor(0.f);
            }
            LLMotionController::setCurrentTimeFactor(0.f);
            stopAvatars();

            // Clouds.
            if (LLEnvironment::instanceExists() && !LLEnvironment::instance().isCloudScrollPaused())
            {
                LLEnvironment::instance().pauseCloudScroll();
                sPausedClouds = true;
            }
        }
        else
        {
            sFrozenTotal += LLFrameTimer::getElapsedSeconds() - sFrozenSince;
            sFrozen = false;
            sPinned = false;    // the clock goes on from where it was before the freeze
            sBorn.clear();

            for (LLCharacter* character : LLCharacter::sInstances)
            {
                auto it = sSavedFactors.find(character);
                character->setAnimTimeFactor(it != sSavedFactors.end() ? it->second : sSavedCurrentFactor);
            }
            sSavedFactors.clear();
            LLMotionController::setCurrentTimeFactor(sSavedCurrentFactor);

            if (sPausedClouds)
            {
                sPausedClouds = false;
                if (LLEnvironment::instanceExists())
                {
                    LLEnvironment::instance().resumeCloudScroll();
                }
            }

            if (sDropped)
            {
                askAgain();
            }
            // otherwise tick() applies the held updates, a few ms a frame
        }

        LL_INFOS("WorldFreeze") << (on ? "frozen" : "resumed") << " (" << sHeld.size() << " objects held, "
                                << sHeldBytes << " bytes)" << LL_ENDL;
    }

    // research/218 vk-s0b: the cue files (see CUE_FREEZE), once a second, only with LLRHI_CUE_DIR.
    void pollCue()
    {
        static LLRHI::CuePoll cue_poll(CUE_POLL_SECS);
        if (!cue_poll.due())
        {
            return;
        }
        for (const bool freeze : { true, false })
        {
            if (!LLRHI::takeCue(freeze ? CUE_FREEZE : CUE_RESUME))
            {
                continue;
            }
            plus::freeze::setFrozen(freeze);
            sPinned = freeze && sFrozen;
            LL_INFOS("WorldFreeze") << "cue " << (freeze ? CUE_FREEZE : CUE_RESUME) << " taken: the world "
                                    << (sFrozen ? "frozen" : "running") << (sPinned ? ", its clock pinned (research/218)" : "")
                                    << LL_ENDL;
        }
    }

    // Every frame at idle.
    bool tick()
    {
        pollCue();
        if (sFrozen)
        {
            // A teleport replaces the world: nothing left to keep still.
            if (gAgent.getTeleportState() != LLAgent::TELEPORT_NONE)
            {
                plus::freeze::setFrozen(false);
            }
            else
            {
                stopAvatars();
                if (!sDropped && LLFrameTimer::getElapsedSeconds() - sFrozenSince > maxSeconds())
                {
                    dropPayloads();
                }
            }
        }
        else if (!sHeld.empty() && !sDropped)
        {
            drain();
        }
        return false;   // keep going
    }

    void init()
    {
        if (LLControlVariable* control = gSavedSettings.getControl(SETTING_FROZEN))
        {
            control->getSignal()->connect([](LLControlVariable*, const LLSD& value, const LLSD&) { applyFrozen(value.asBoolean()); });
        }
        if (!sTickRegistered)
        {
            sTickRegistered = true;
            doOnIdleRepeating([]() { return tick(); });
        }
    }

    plus::FeatureRegistration sRegistration({ "freeze", &init });
}

namespace plus::freeze
{
    bool isFrozen()
    {
        return sFrozen;
    }

    void setFrozen(bool frozen)
    {
        if (gSavedSettings.controlExists(SETTING_FROZEN))
        {
            gSavedSettings.setBOOL(SETTING_FROZEN, frozen);
        }
        applyFrozen(frozen);    // idempotent: also when the setting's signal already did it
    }

    F64 worldSeconds()
    {
        if (sFrozen && sPinned)
        {
            return CUE_WORLD_SECONDS;  // research/218 vk-s0b: frozen by the measuring tools' cue
        }
        const F64 now = LLFrameTimer::getElapsedSeconds();
        return now - sFrozenTotal - (sFrozen ? now - sFrozenSince : 0.0);
    }

    F32 frozenSeconds()
    {
        return sFrozen ? static_cast<F32>(LLFrameTimer::getElapsedSeconds() - sFrozenSince) : 0.f;
    }

    bool interceptObjectUpdate(LLMessageSystem* msg, S32 update_type, bool compressed)
    {
        if (!sFrozen && sHeld.empty())
        {
            return false;   // the usual case: one test
        }

        const bool terse = compressed && update_type == OUT_TERSE_IMPROVED;
        const bool full  = (!compressed && update_type == OUT_FULL) || (compressed && update_type == OUT_FULL_COMPRESSED);
        if (!terse && !full)
        {
            return false;
        }
        if (full && sFrozen)
        {
            return false;   // decided per object by holdFullUpdate(), in the message's loop
        }

        U64 region_handle = 0;
        msg->getU64Fast(_PREHASH_RegionData, _PREHASH_RegionHandle, region_handle);
        const S32 blocks = msg->getNumberOfBlocksFast(_PREHASH_ObjectData);
        std::vector<LLViewerRegion*> ask_regions;   // for the held entries a live terse update applies

        for (S32 i = 0; i < blocks; ++i)
        {
            U32 local_id = 0;
            U8 buffer[MAX_UPDATE_BYTES];
            S32 size = 0;

            if (compressed)
            {
                size = msg->getSizeFast(_PREHASH_ObjectData, i, _PREHASH_Data);
                if (size < static_cast<S32>(sizeof(U32)) || size > MAX_UPDATE_BYTES)
                {
                    continue;
                }
                msg->getBinaryDataFast(_PREHASH_ObjectData, _PREHASH_Data, buffer, 0, i, MAX_UPDATE_BYTES);
                LLDataPackerBinaryBuffer dp(buffer, size);
                if (terse)
                {
                    dp.unpackU32(local_id, "LocalID");
                }
                else
                {
                    LLUUID id;
                    dp.unpackUUID(id, "ID");
                    dp.unpackU32(local_id, "LocalID");
                }
            }
            else
            {
                msg->getU32Fast(_PREHASH_ObjectData, _PREHASH_ID, local_id, i);
            }

            const Key key = makeKey(msg, local_id);
            if (terse && sFrozen)
            {
                Held* slot = slotFor(key, region_handle, static_cast<size_t>(size));
                if (!slot)
                {
                    continue;   // its region is asked for whole on resume
                }
                Held& held = *slot;
                if (sDropped)
                {
                    held.ask_again = true;
                    continue;
                }
                keepBytes(held.terse, buffer, size);

                // A texture entry in a terse update is read by LLVOVolume from the
                // message, which will be gone: the object is asked for again instead.
                // (Avatars do not read it.)
                if (msg->getSizeFast(_PREHASH_ObjectData, i, _PREHASH_TextureEntry) > 0)
                {
                    LLUUID fullid;
                    gObjectList.getUUIDFromLocal(fullid, local_id, msg->getSenderIP(), msg->getSenderPort());
                    const LLViewerObject* objectp = fullid.notNull() ? gObjectList.findObject(fullid) : nullptr;
                    if (objectp && !objectp->isAvatar())
                    {
                        held.ask_again = true;
                    }
                }
            }
            else if (full)
            {
                // A live full update after the world resumed is the whole object as it is
                // now: it wins over everything held for it.
                forget(key);
            }
            else
            {
                // A live terse update after the world resumed wins over the held terse one
                // only: it has the position, not the looks. A held full update (older than
                // this one) is applied first and an object marked to be asked for again is
                // asked for, then the live update goes through as always.
                auto it = sHeld.find(key);
                if (it != sHeld.end())
                {
                    freeBytes(it->second.terse);
                    applyEntry(it->first, it->second, ask_regions);
                    forget(key);    // applyEntry() does not touch the table (no update is held while the world runs)
                }
            }
        }
        for (LLViewerRegion* regionp : ask_regions)
        {
            regionp->requestCacheMisses();
        }

        return terse && sFrozen;
    }

    bool holdFullUpdate(LLMessageSystem* msg, S32 block, S32 update_type, bool compressed, LLViewerRegion* regionp)
    {
        if (!sFrozen)
        {
            return false;   // the usual case: one test
        }
        const bool full = (!compressed && update_type == OUT_FULL) || (compressed && update_type == OUT_FULL_COMPRESSED);
        if (!full || !regionp)
        {
            return false;
        }

        // The block's object: the same fields processObjectUpdate() reads next.
        LLUUID fullid;
        U32 local_id = 0;
        U32 parent_id = 0;
        U32 flags = 0;
        U8 buffer[MAX_UPDATE_BYTES];
        S32 size = 0;
        msg->getU32Fast(_PREHASH_ObjectData, _PREHASH_UpdateFlags, flags, block);
        if (compressed)
        {
            size = msg->getSizeFast(_PREHASH_ObjectData, block, _PREHASH_Data);
            if (size < static_cast<S32>(sizeof(LLUUID) + sizeof(U32) + sizeof(U8)) || size > MAX_UPDATE_BYTES)
            {
                return false;   // the official code deals with it
            }
            msg->getBinaryDataFast(_PREHASH_ObjectData, _PREHASH_Data, buffer, 0, block, MAX_UPDATE_BYTES);
            LLDataPackerBinaryBuffer dp(buffer, size);
            dp.unpackUUID(fullid, "ID");
            dp.unpackU32(local_id, "LocalID");
            LLViewerObject::unpackParentID(&dp, parent_id);   // reads at fixed offsets, then rewinds
        }
        else
        {
            msg->getUUIDFast(_PREHASH_ObjectData, _PREHASH_FullID, fullid, block);
            msg->getU32Fast(_PREHASH_ObjectData, _PREHASH_ID, local_id, block);
            msg->getU32Fast(_PREHASH_ObjectData, _PREHASH_ParentID, parent_id, block);
        }

        const Key key = makeKey(msg, local_id);
        LLViewerObject* objectp = fullid.notNull() ? gObjectList.findObject(fullid) : nullptr;
        if (!objectp)
        {
            // Not shown yet (new, or only in the object cache): it is made as always,
            // and its next updates go through too while this freeze lasts.
            if (static_cast<S32>(sBorn.size()) < maxPending())
            {
                sBorn.insert(key);
            }
            forget(key);
            return false;
        }
        if (!holdAppearance() || sBorn.count(key) || !holdable(objectp, regionp, local_id, parent_id))
        {
            forget(key);    // goes through: newer than anything held for the object
            return false;
        }

        // A compressed update of an object the cache keeps is kept as bytes; the
        // rest (no compression, or a temporary object, both read from the message)
        // keeps the ID and is asked for again.
        const bool as_bytes = compressed && !(flags & FLAGS_TEMPORARY_ON_REZ);
        Held* slot = slotFor(key, regionp->getHandle(), as_bytes ? static_cast<size_t>(size) : 0);
        if (!slot)
        {
            return true;    // held all the same: its region is asked for whole on resume
        }
        Held& held = *slot;
        freeBytes(held.terse);  // older than this full update, which has the position too
        if (sDropped || !as_bytes)
        {
            freeBytes(held.full);
            held.ask_again = true;
        }
        else
        {
            keepBytes(held.full, buffer, size);
            held.full_id = fullid;
            held.full_flags = flags;
            held.ask_again = false;     // this update is the whole object
        }
        return true;
    }

    void onObjectKilled(LLMessageSystem* msg, U32 local_id)
    {
        if (!sFrozen && sHeld.empty())
        {
            return;     // the usual case: one test
        }
        // Nothing held is applied or asked for again for an object that is gone; its local
        // ID may come back as another object, which is new then.
        const Key key = makeKey(msg, local_id);
        forget(key);
        sBorn.erase(key);
    }
}

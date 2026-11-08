/**
 * @file derender.cpp
 * @brief Derender: objects and avatars hidden from the right-click menu until the next teleport
 *        (from Firestorm's temporary derender).
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

#include "plus/derender/derender.h"
#include "plus/plus.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llapp.h"
#include "llcallbacklist.h"
#include "llhost.h"
#include "llselectmgr.h"
#include "lluictrl.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llvoavatar.h"
#include "llvoavatarself.h"
#include "llvocache.h"
#include "llworld.h"
#include "message.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

// Firestorm: llviewermenu.cpp derenderObject() and LLViewerObjectList::mDerendered /
// resetDerenderList() (Ansariel, Nicky Dasmijn; LGPL 2.1). Rewritten here: no blacklist file,
// the ids of the whole linkset or avatar, the cache entries refused later forgotten on the idle,
// dropped when a teleport lands in another region (not in LLAgent::teleportCore(), which the
// simulator's teleports do not pass, and before a teleport within the region has anything sent again).

namespace plus::derender
{
    namespace detail
    {
        std::size_t sHiddenCount = 0;
    }

    namespace
    {
        // Avatars and attachments move from region to region: they stay until the teleport. Not a
        // handle a region can have (0 is that of the region at grid (0, 0)).
        constexpr U64 ANY_REGION = ~0ULL;

        // At most this many ids are hidden (a crowd of avatars with all their attachments is a few
        // thousand); a target that would pass it is left shown.
        constexpr std::size_t MAX_HIDDEN_IDS = 10000;

        struct Hidden
        {
            U64    keep_region; // let go when this region is no longer connected (ANY_REGION: at the teleport)
            U64    prim_region; // the region the prim was in when it was hidden
            U32    local_id;    // its local id there
            LLUUID root;        // the target it was hidden with
        };

        // Hidden id -> where it was.
        std::unordered_map<LLUUID, Hidden> sHidden;

        // The agent's region when it last changed (a teleport to the same region is not a change).
        U64 sAgentRegion = 0;

        struct CacheForget
        {
            U64    region_handle;
            U32    local_id;
            LLUUID id;
        };
        std::vector<CacheForget> sCacheForget;
        bool                     sCacheForgetQueued = false;

        void countChanged()
        {
            detail::sHiddenCount = sHidden.size();
        }

        // The id a cache entry describes (the first field of its data, as
        // processObjectUpdateFromCache() reads it); null when it has none.
        LLUUID cacheEntryId(LLVOCacheEntry* entry)
        {
            LLUUID id;
            LLDataPackerBinaryBuffer* dp = entry ? entry->getDP() : nullptr;
            if (dp)
            {
                dp->reset();
                dp->unpackUUID(id, "ID");
                dp->reset();
            }
            return id;
        }

        void forgetQueuedCacheEntries()
        {
            sCacheForgetQueued = false;
            std::vector<CacheForget> queued;
            queued.swap(sCacheForget);
            if (LLApp::isExiting() || !LLWorld::instanceExists())
            {
                return;
            }
            for (const CacheForget& item : queued)
            {
                if (!detail::contains(item.id))
                {
                    continue; // a teleport came first: the object may be made again
                }
                LLViewerRegion* region = LLWorld::getInstance()->getRegionFromHandle(item.region_handle);
                if (!region)
                {
                    continue;
                }
                // The local id may belong to another object by now: only the same id is taken out.
                if (cacheEntryId(region->getCacheEntry(item.local_id)) == item.id)
                {
                    region->killCacheEntry(item.local_id);
                }
            }
        }

        // By the parent, not isSitting(): a sit the simulator has made but the viewer not yet
        // marked counts too.
        bool agentSitsOn(LLViewerObject* root)
        {
            if (!isAgentAvatarValid())
            {
                return false;
            }
            LLViewerObject* seat = static_cast<LLViewerObject*>(gAgentAvatarp->getParent());
            return seat && seat->getRootEdit() == root;
        }

        // Whether a connected region stays connected from the region arrived in: its square within the
        // draw distance of that region's square (the simulator's child agents reach that far, past the
        // eight neighbours with a long draw distance). A region left behind beyond it closes soon.
        bool staysAround(const LLViewerRegion* region, const LLViewerRegion* arrived)
        {
            if (region == arrived)
            {
                return true;
            }
            const LLVector3d a = arrived->getOriginGlobal();
            const LLVector3d r = region->getOriginGlobal();
            const F64 aw = arrived->getWidth(), rw = region->getWidth();
            const F64 dx = llmax(r.mdV[VX] - (a.mdV[VX] + aw), a.mdV[VX] - (r.mdV[VX] + rw), 0.0);
            const F64 dy = llmax(r.mdV[VY] - (a.mdV[VY] + aw), a.mdV[VY] - (r.mdV[VY] + rw), 0.0);
            return llmax(dx, dy) <= (F64)gAgentCamera.mDrawDistance;
        }

        // Every id goes. A region the viewer connects to anew sends what it has, but one that stays
        // connected (the region arrived in when it was connected already, those around it) does not
        // send again what it thinks the viewer has: its prims are asked for, as showAgain() does.
        // Not a region left behind beyond the draw distance by a far teleport: it closes soon, and
        // what it sent would go with it (research/275 L079, review lows).
        void dropAll(const char* why)
        {
            if (sHidden.empty())
            {
                return;
            }
            LLWorld*        world   = LLWorld::instanceExists() ? LLWorld::getInstance() : nullptr;
            LLViewerRegion* arrived = gAgent.getRegion();
            size_t          asked   = 0;
            for (const auto& [id, hidden] : sHidden)
            {
                LLViewerRegion* region = world ? world->getRegionFromHandle(hidden.prim_region) : nullptr;
                if (region && (!arrived || staysAround(region, arrived)))
                {
                    region->addCacheMissFull(hidden.local_id);
                    ++asked;
                }
            }
            LL_INFOS("Plus") << why << ": " << sHidden.size() << " derendered ids let go (" << asked
                             << " asked for again in the regions that stay connected)" << LL_ENDL;
            sHidden.clear();
            countChanged();
        }

        // Shows the target root_id again; request: its prims are asked for again (the simulator
        // does not send what it thinks the viewer has), as the official viewer asks for cache misses.
        void showAgain(const LLUUID& root_id, bool request)
        {
            LLWorld* world = request && LLWorld::instanceExists() ? LLWorld::getInstance() : nullptr;
            for (auto it = sHidden.begin(); it != sHidden.end();)
            {
                if (it->second.root != root_id)
                {
                    ++it;
                    continue;
                }
                LLViewerRegion* region = world ? world->getRegionFromHandle(it->second.prim_region) : nullptr;
                if (region)
                {
                    region->addCacheMissFull(it->second.local_id);
                }
                it = sHidden.erase(it);
            }
            countChanged();
        }

        // What a right-clicked part stands for: an avatar itself, else the root of its linkset
        // (an attachment's root, not its wearer).
        LLViewerObject* targetOf(LLViewerObject* object)
        {
            return object->isAvatar() ? object : object->getRootEdit();
        }

        bool canDerender(LLViewerObject* target)
        {
            if (!target || target->isDead() || !target->getRegion() || target->getID() == gAgentID)
            {
                return false;
            }
            if (target->isAvatar())
            {
                LLVOAvatar* avatar = target->asAvatar();
                return avatar && !avatar->isSelf() && !avatar->isControlAvatar();
            }
            if (target->isHUDAttachment())
            {
                return false;
            }
            if (target->isAttachment())
            {
                // The agent's own attachments are left: a killed one leaves the outfit
                // (LLVOAvatarSelf::detachObject() unregisters it from the Current Outfit folder).
                LLVOAvatar* wearer = target->getAvatarAncestor();
                return wearer && !wearer->isSelf();
            }
            return !agentSitsOn(target);
        }

        // The distinct targets of the selection that can be derendered.
        void collectTargets(std::vector<LLPointer<LLViewerObject>>& targets)
        {
            std::unordered_set<LLViewerObject*> seen;
            LLObjectSelectionHandle selection = LLSelectMgr::getInstance()->getSelection();
            for (LLObjectSelection::iterator it = selection->begin(); it != selection->end(); ++it)
            {
                LLViewerObject* object = (*it)->getObject();
                if (!object)
                {
                    continue;
                }
                LLViewerObject* target = targetOf(object);
                if (seen.insert(target).second && canDerender(target))
                {
                    targets.emplace_back(target);
                }
            }
        }

        // The menu item's enable check (asked while the menu is shown): stops at the first target,
        // makes nothing.
        bool selectionHasTarget()
        {
            LLObjectSelectionHandle selection = LLSelectMgr::getInstance()->getSelection();
            for (LLObjectSelection::iterator it = selection->begin(); it != selection->end(); ++it)
            {
                LLViewerObject* object = (*it)->getObject();
                if (object && canDerender(targetOf(object)))
                {
                    return true;
                }
            }
            return false;
        }

        // The object and every prim and attachment under it; avatars sitting on it are not its own.
        void collectPrims(LLViewerObject* object, std::vector<LLViewerObject*>& prims)
        {
            prims.push_back(object);
            for (const LLPointer<LLViewerObject>& child : object->getChildren())
            {
                if (child.notNull() && !child->isAvatar())
                {
                    collectPrims(child.get(), prims);
                }
            }
        }

        // As process_kill_object() does for a kill from the simulator, here only. False when the
        // limit leaves it shown.
        bool hide(LLViewerObject* target, std::vector<LLViewerObject*>& prims)
        {
            prims.clear();
            collectPrims(target, prims);
            if (sHidden.size() + prims.size() > MAX_HIDDEN_IDS)
            {
                return false;
            }

            LLViewerRegion* region = target->getRegion();
            const U32       local_id = target->getLocalID();
            const bool      moves = target->isAvatar() || target->isAttachment();
            const LLUUID    root_id = target->getID();
            std::vector<LLUUID> ids;
            ids.reserve(prims.size());
            for (LLViewerObject* prim : prims)
            {
                const LLViewerRegion* prim_region = prim->getRegion() ? prim->getRegion() : region;
                sHidden[prim->getID()] = { moves ? ANY_REGION : region->getHandle(), prim_region->getHandle(),
                                           prim->getLocalID(), root_id };
                ids.push_back(prim->getID());
            }
            countChanged();

            gObjectList.killObject(target);
            if (LLViewerRegion::sVOCacheCullingEnabled && !moves) // avatars and attachments are not cached
            {
                region->killCacheEntry(local_id);
            }
            // Releases a tool still holding a part (the grab tool, the edit tool).
            for (const LLUUID& id : ids)
            {
                LLSelectMgr::getInstance()->removeObjectFromSelections(id);
            }
            return true;
        }

        void derenderSelection()
        {
            std::vector<LLPointer<LLViewerObject>> targets;
            collectTargets(targets);
            if (targets.empty())
            {
                return;
            }
            // The usual deselect of the right-click's selection (a physical object stays held while selected).
            LLSelectMgr::getInstance()->deselectAll();

            std::vector<LLViewerObject*> prims;
            size_t                       hidden = 0;
            for (const LLPointer<LLViewerObject>& target : targets)
            {
                if (!target->isDead())
                {
                    if (!hide(target.get(), prims))
                    {
                        LL_WARNS("Plus") << "Derender: at most " << MAX_HIDDEN_IDS << " ids are hidden; the rest is left" << LL_ENDL;
                        break;
                    }
                    ++hidden;
                }
            }
            LL_INFOS("Plus") << "Derendered " << hidden << " object(s) until the next teleport to another region ("
                             << sHidden.size() << " ids hidden)" << LL_ENDL;
        }

        // LLWorld lets go of a region (the simulator closed it after a walk away; every region at
        // logout): the ids hidden in it go, by the region each was hidden in (going back fetches the
        // region again, with its objects). Avatars and attachments stay until the teleport.
        void onRegionRemoved(LLViewerRegion* region)
        {
            if (!region || sHidden.empty())
            {
                return;
            }
            const U64 handle = region->getHandle();
            for (auto it = sHidden.begin(); it != sHidden.end();)
            {
                it = it->second.keep_region == handle ? sHidden.erase(it) : std::next(it);
            }
            countChanged();
        }

        // Connected at the first region change (LLWorld is made for each session, after init()).
        boost::signals2::connection sRegionRemovedConnection;

        void watchRegionRemoval()
        {
            if (!sRegionRemovedConnection.connected() && LLWorld::instanceExists())
            {
                sRegionRemovedConnection = LLWorld::getInstance()->setRegionRemovedCallback(&onRegionRemoved);
            }
        }

        // A teleport that lands in another region (begun by the viewer or by the simulator:
        // llTeleportAgent, an experience, sent home, ...) drops every id, and the objects come back
        // (dropAll()). The agent's region is set before the new region sends its objects, so none of
        // them is refused on the way. A teleport within the region, a failed or a cancelled one keeps
        // them (the simulator would not send them again). A walk into the next region keeps them:
        // the ids of a region go when the viewer lets go of it (onRegionRemoved()).
        void onAgentRegionChanged()
        {
            LLViewerRegion* region = gAgent.getRegion();
            const U64       handle = region ? region->getHandle() : 0;
            const bool      changed = handle != sAgentRegion;
            sAgentRegion = handle;
            watchRegionRemoval();
            if (changed && gAgent.getTeleportState() != LLAgent::TELEPORT_NONE)
            {
                dropAll("Teleport to another region");
            }
        }

        void init()
        {
            LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Plus.Derender.Selection",
                                                                     [](LLUICtrl*, const LLSD&) { derenderSelection(); });
            LLUICtrl::EnableCallbackRegistry::defaultRegistrar().add("Plus.Derender.CanSelection",
                                                                     [](LLUICtrl*, const LLSD&) { return selectionHasTarget(); });
            gAgent.addRegionChangedCallback(&onAgentRegionChanged);
        }

        plus::FeatureRegistration sRegistration({ "derender", &init });
    }

    bool detail::contains(const LLUUID& id)
    {
        return sHidden.find(id) != sHidden.end();
    }

    void forgetCacheEntry(LLViewerRegion* region, U32 local_id, const LLUUID& id)
    {
        if (!region)
        {
            return;
        }
        sCacheForget.push_back({ region->getHandle(), local_id, id });
        if (!sCacheForgetQueued)
        {
            sCacheForgetQueued = true;
            doOnIdleOneTime(&forgetQueuedCacheEntries);
        }
    }

    namespace
    {
        // Only the agent: sat on a hidden object (a forced sit, llSitOnLink, a sit answered late) it
        // would stay invisible with its seat, or on its old seat. The seat is shown and asked for
        // again; true when parent_id of the region at ip:port was a hidden prim.
        bool showHiddenSeat(LLViewerObject* child, U32 parent_id, U32 ip, U32 port)
        {
            if (child != gAgentAvatarp || !LLWorld::instanceExists())
            {
                return false;
            }
            LLViewerRegion* region = LLWorld::getInstance()->getRegion(LLHost(ip, port));
            if (!region)
            {
                return false;
            }
            const U64 handle = region->getHandle();
            for (const auto& [id, hidden] : sHidden)
            {
                if (hidden.prim_region == handle && hidden.local_id == parent_id)
                {
                    const LLUUID root_id = hidden.root; // showAgain() erases the entry
                    LL_INFOS("Plus") << "Derender: the agent sits on a derendered object; it is shown again" << LL_ENDL;
                    showAgain(root_id, true);
                    return true;
                }
            }
            return false;
        }
    }

    void detail::onOrphaned(LLViewerObject* child, U32 parent_id, U32 ip, U32 port)
    {
        showHiddenSeat(child, parent_id, ip, port);
    }

    bool detail::waitForHiddenSeat(LLViewerObject* avatar, U32 parent_id, LLMessageSystem* msg)
    {
        LLViewerRegion* region = avatar ? avatar->getRegion() : nullptr;
        if (avatar != gAgentAvatarp || (!msg && !region))
        {
            return false;
        }
        // Where the update came from, as processUpdateMessage() takes it for orphanize().
        const U32 ip   = msg ? msg->getSenderIP() : region->getHost().getAddress();
        const U32 port = msg ? msg->getSenderPort() : region->getHost().getPort();
        if (!showHiddenSeat(avatar, parent_id, ip, port))
        {
            return false;
        }
        // Off the old seat, onto the new one when it comes (findOrphans()). orphanize() calls
        // onOrphaned() again, which finds nothing: the seat's ids are gone.
        gObjectList.orphanize(avatar, parent_id, ip, port);
        return true;
    }
}

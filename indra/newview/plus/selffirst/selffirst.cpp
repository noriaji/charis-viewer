/**
 * @file selffirst.cpp
 * @brief The agent's own avatar first after login: a log of what the cloud
 *        waits for, and (PlusSelfFirst) its textures and meshes ahead of the
 *        world's.
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

// research/122. After login the agent's avatar can stay a cloud for a long
// while. What it waits for, in order (LLVOAvatar::updateIsFullyLoaded()):
//   3. the outfit (COF) from the inventory, the bake request to the
//      appearance service and the AvatarAppearance message with the bake ids
//      (outside the viewer);
//   1. the bakes' first data (rez status 2, "gray" gone; fetched like any
//      texture, BOOST_AVATAR_BAKED_SELF);
//   4. the attachments' objects from the simulator (outside the viewer);
//   2. the attachments' meshes and skin info (mesh fetch);
// then 3 s with nothing missing. The attachments' textures do not hold the
// cloud, but until they come the outfit shows blurry or gray.
//
// The log (tag SelfFirst, PlusSelfFirstLog, off by default: diagnostics) times each of these
// from the login and from the moment the world shows, every 5 s says what is
// still missing and where the agent's textures and meshes are in the fetch
// queues, and when the cloud goes sums up which of 1 to 4 it waited for last.
// Reading only: nothing changes.
//
// PlusSelfFirst (off by default, research/122 3): from the login reply until
// the cloud is gone and the agent's textures and meshes have their first data
// (at most ACTIVE_SECONDS after the world shows), the agent's bakes and
// attachment textures are fetched above every world texture and its meshes
// are requested before the world's, and the world's texture and mesh requests
// in flight are held to WORLD_SHARE of the usual limit (slowed, not stopped:
// the world's own requests are what is held to it, so the agent's never take
// the world's share away; the agent's go on top of it, up to the usual limit).

#include "llviewerprecompiledheaders.h"

#include "plus.h"

#include "selffirst.h"

#include "llappearancemgr.h"
#include "llappviewer.h"
#include "llcallbacklist.h"
#include "lldrawable.h"
#include "llevents.h"
#include "llfetchedgltfmaterial.h"
#include "llimageworker.h"
#include "llinventorymodel.h"
#include "llmeshrepository.h"
#include "llstartup.h"
#include "lltexturefetch.h"
#include "llviewercontrol.h"
#include "llviewerinventory.h"
#include "llviewerjointattachment.h"
#include "llviewerobject.h"
#include "llviewertexture.h"
#include "llviewertexturelist.h"
#include "llvoavatarself.h"
#include "llvovolume.h"

#include <atomic>
// <plus> research/299: a texture waiting for a retry of a failed fetch is not "no data"
#include "plus/texprio/fetchretry.h"
// </plus>

#include <map>
#include <unordered_map>
#include <unordered_set>

namespace
{
    constexpr F64 SCAN_SECONDS = 0.25;
    constexpr F64 REPORT_SECONDS = 5.0;
    constexpr F64 CLOUD_SETTLE_SECONDS = 3.0;           // FIRST_APPEARANCE_CLOUD_MIN_DELAY (llvoavatar.cpp)
    constexpr F64 TEXTURES_AFTER_CLOUD_SECONDS = 60.0;  // the textures are followed this long after the cloud went
    constexpr F64 GIVE_UP_SECONDS = 300.0;              // after the world showed: the log ends
    constexpr F64 ACTIVE_SECONDS = 120.0;               // after the world showed: PlusSelfFirst ends
    constexpr F64 ACTIVE_AFTER_LOGIN_SECONDS = 240.0;   // after the login reply, if the world never shows
    constexpr F32 WORLD_SHARE = 0.4f;                   // of the requests in flight, while the avatar loads
    constexpr F64 WAITER_SCANS = 20.0;                  // a second, while the world's share is in flight
    // Above every world texture's priority (a virtual size, at most
    // LLViewerFetchedTexture::sMaxVirtualSize = 8192^2) and every world mesh's
    // score (a radius over a distance of at least 1 m).
    constexpr F32 SELF_TEXTURE_PRIORITY = 1.0e9f;
    constexpr F32 SELF_MESH_SCORE = 1.0e6f;

    std::atomic<bool> sActive{ false };                 // PlusSelfFirst is acting now
    std::atomic<int> sWorldTexturesInFlight{ 0 };       // the world's HTTP texture requests (counted always)
    std::atomic<int> sSelfMeshesWaiting{ 0 };
    std::unordered_set<LLUUID> sSelfTextures;           // main thread; used while sActive

    enum Milestone
    {
        M_LOGIN_REPLY,      // STATE_WORLD_INIT
        M_PRECACHE,         // STATE_PRECACHE
        M_WEARABLES_WAIT,   // STATE_WEARABLES_WAIT
        M_WORLD_SHOWN,      // STATE_STARTED
        M_AVATAR,           // gAgentAvatarp exists
        M_COF,              // the COF's version is known
        M_BAKE_REQUEST,     // bake request sent to the appearance service
        M_APPEARANCE,       // an AvatarAppearance message for the agent processed
        M_BAKE_IDS,         // every bake has its id (isFullyBaked)
        M_GRAY_OFF,         // every bake has data (rez status 2)
        M_BAKES_FULL,       // every bake at full resolution (rez status 3)
        M_ATTACHED,         // every object in the COF is attached
        M_MESHES,           // no attached mesh waits (after M_ATTACHED)
        M_CLOUD_OFF,        // isFullyLoaded
        M_TEXTURES_DATA,    // every attachment texture has data (after M_ATTACHED)
        M_TEXTURES_DONE,    // every attachment texture at its level (after M_ATTACHED)
        M_COUNT
    };
    const char* const MILESTONE_NAMES[M_COUNT] = {
        "login reply", "precache", "wearables wait", "world shown", "avatar created", "COF known",
        "bake request sent", "appearance message", "bake ids (cause 3)", "bakes' first data, gray gone (cause 1)",
        "bakes at full resolution", "all COF attachments attached (cause 4)", "attachment meshes ready (cause 2)",
        "cloud gone", "attachment textures' first data", "attachment textures at their level"
    };

    struct Scan
    {
        bool avatar = false;
        S32 rez = -1;
        bool fully_loaded = false;
        bool bake_ids = false;
        S32 bakes = 0, bakes_with_data = 0, bakes_full = 0;
        S32 cof_version = LLViewerInventoryCategory::VERSION_UNKNOWN;
        S32 bake_requested = -1, appearance_received = -1;
        S32 expected = -1, worn = 0, unresolved_links = 0;  // objects in the COF (-1: COF not known)
        S32 attached = 0, pending_attachments = 0;
        S32 mesh_volumes = 0, meshes_waiting = 0;
        S32 textures = 0, textures_no_data = 0, textures_not_done = 0;
        std::unordered_set<LLUUID> texture_ids;             // bakes and attachment textures
        std::vector<LLUUID> textures_waiting;               // not at their level (bakes first)
        std::unordered_set<LLUUID> meshes_waiting_ids;      // mesh asset ids
        std::unordered_set<LLUUID> objects;                 // attached objects (roots and children)
        std::unordered_map<LLUUID, bool> object_mesh_waiting;
    };

    struct QueueSums
    {
        U32 samples = 0;
        F64 tex_requests = 0.0, http_waiting = 0.0, decode = 0.0, create = 0.0, mesh_pending = 0.0, mesh_active = 0.0;
    };

    struct State
    {
        bool tracking = false;
        F64 login = -1.0;                       // LLTimer::getTotalSeconds() at STATE_LOGIN_AUTH_INIT
        F64 at[M_COUNT];
        F64 last_scan = 0.0;
        F64 last_report = 0.0;
        bool active_said = false;
        bool active_over = false;                   // PlusSelfFirst has ended for this login
        std::unordered_map<LLUUID, F64> object_seen;        // attached object -> first seen
        std::unordered_map<LLUUID, F64> mesh_wait_start;    // attached object -> first seen with its mesh waiting
        F64 longest_mesh_wait = 0.0;                        // seen to mesh ready, over the objects
        F64 meshes_ready = -1.0;                            // the attached meshes last went from waiting to none
        F64 last_object_seen = -1.0;
        std::unordered_map<LLUUID, F64> texture_seen;       // texture -> first seen without data
        F64 longest_texture_wait = 0.0;                     // seen to first data
        QueueSums waiting;                                  // while the cloud is there
        QueueSums after;                                    // from the cloud's end to the textures'
        Scan last;
    };
    State sState;

    F64 now_seconds()
    {
        return LLTimer::getTotalSeconds();
    }

    bool log_on()
    {
        static LLCachedControl<bool> on(gSavedSettings, "PlusSelfFirstLog", false);
        return on;
    }

    bool switch_on()
    {
        static LLCachedControl<bool> on(gSavedSettings, "PlusSelfFirst", false);
        return on;
    }

    std::string since(F64 t)
    {
        if (t < 0.0)
        {
            return "-";
        }
        std::string text = llformat("+%.1f s", t - sState.login);
        if (sState.at[M_WORLD_SHOWN] >= 0.0)
        {
            text += llformat(" (world %+.1f s)", t - sState.at[M_WORLD_SHOWN]);
        }
        return text;
    }

    void reset()
    {
        sState = State();
        for (F64& t : sState.at)
        {
            t = -1.0;
        }
        sActive = false;
        sSelfMeshesWaiting = 0;
        sSelfTextures.clear();
    }

    // As check_object_for_mesh_loading() in llvoavatar.cpp: the volume's mesh
    // or skin info still holds the cloud.
    bool mesh_waiting(LLViewerObject* objp, bool& is_mesh)
    {
        is_mesh = false;
        if (!objp || objp->isDead() || objp->getPCode() != LL_PCODE_VOLUME)
        {
            return false;
        }
        LLVolume* volp = objp->getVolume();
        if (!volp || volp->getParams().getSculptID().isNull() || volp->isMeshAssetUnavaliable() || !objp->mDrawable)
        {
            return false;
        }
        LLVOVolume* vobj = objp->mDrawable->getVOVolume();
        if (!vobj)
        {
            return true;
        }
        if (!vobj->isMesh())
        {
            return false;
        }
        is_mesh = true;
        if (!volp->isMeshAssetLoaded())
        {
            return true;
        }
        return !vobj->getSkinInfo() && !vobj->isSkinInfoUnavaliable();
    }

    void add_texture(LLViewerTexture* texture, Scan& scan, bool bake)
    {
        LLViewerFetchedTexture* imagep = LLViewerTextureManager::staticCastToFetchedTexture(texture, false);
        if (!imagep || !scan.texture_ids.insert(imagep->getID()).second)
        {
            return;
        }
        const S32 discard = imagep->getDiscardLevel();
        if (bake)
        {
            ++scan.bakes;
            scan.bakes_with_data += discard >= 0 ? 1 : 0;
            scan.bakes_full += discard == 0 ? 1 : 0;
            if (discard != 0 && !imagep->isMissingAsset())
            {
                scan.textures_waiting.push_back(imagep->getID());
            }
            return;
        }
        ++scan.textures;
        // <plus> research/299: a texture waiting for a retry of a failed fetch is as one flagged missing: done with
        if (imagep->isMissingAsset() || plus::texprio::fetchretry::waiting(imagep))
        {
            return;
        }
        // </plus>
        if (discard < 0)
        {
            ++scan.textures_no_data;
        }
        if (discard < 0 || discard > imagep->getDesiredDiscardLevel())
        {
            ++scan.textures_not_done;
            scan.textures_waiting.push_back(imagep->getID());
        }
    }

    void add_object(LLViewerObject* objp, Scan& scan)
    {
        if (!objp || objp->isDead())
        {
            return;
        }
        scan.objects.insert(objp->getID());
        bool is_mesh = false;
        const bool waiting = mesh_waiting(objp, is_mesh);
        scan.object_mesh_waiting[objp->getID()] = waiting;
        scan.mesh_volumes += is_mesh ? 1 : 0;
        if (waiting)
        {
            ++scan.meshes_waiting;
            if (LLVolume* volp = objp->getVolume())
            {
                scan.meshes_waiting_ids.insert(volp->getParams().getSculptID());
            }
        }
        for (U8 te = 0; te < objp->getNumTEs(); ++te)
        {
            add_texture(objp->getTEImage(te), scan, false);
            add_texture(objp->getTENormalMap(te), scan, false);
            add_texture(objp->getTESpecularMap(te), scan, false);
            const LLTextureEntry* entry = objp->getTE(te);
            LLFetchedGLTFMaterial* material = entry ? dynamic_cast<LLFetchedGLTFMaterial*>(entry->getGLTFRenderMaterial()) : nullptr;
            if (material)
            {
                add_texture(material->mBaseColorTexture, scan, false);
                add_texture(material->mNormalTexture, scan, false);
                add_texture(material->mMetallicRoughnessTexture, scan, false);
                add_texture(material->mEmissiveTexture, scan, false);
            }
        }
    }

    Scan take_scan()
    {
        Scan scan;
        LLAppearanceMgr& appearance = LLAppearanceMgr::instance();
        scan.cof_version = appearance.getCOFVersion();
        if (!isAgentAvatarValid())
        {
            return scan;
        }
        LLVOAvatarSelf* self = gAgentAvatarp;
        scan.avatar = true;
        scan.rez = self->getRezzedStatus();
        scan.fully_loaded = self->isFullyLoaded();
        scan.bake_ids = self->isFullyBaked();
        scan.bake_requested = self->mLastUpdateRequestCOFVersion;
        scan.appearance_received = self->mLastUpdateReceivedCOFVersion;
        scan.pending_attachments = (S32)self->mPendingAttachment.size();

        std::set<LLUUID> bakes;
        self->collectBakedTextureUUIDs(bakes);
        for (const LLUUID& id : bakes)
        {
            add_texture(gTextureList.findImage(id, TEX_LIST_STANDARD), scan, true);
        }

        for (const auto& point : self->mAttachmentPoints)
        {
            const LLViewerJointAttachment* attachment = point.second;
            if (!attachment)
            {
                continue;
            }
            for (const LLPointer<LLViewerObject>& objp : attachment->mAttachedObjects)
            {
                if (objp.isNull() || objp->isDead())
                {
                    continue;
                }
                ++scan.attached;
                add_object(objp, scan);
                for (const LLPointer<LLViewerObject>& child : objp->getChildren())
                {
                    add_object(child, scan);
                }
            }
        }

        const LLUUID cof = appearance.getCOF();
        if (cof.notNull() && scan.cof_version != LLViewerInventoryCategory::VERSION_UNKNOWN)
        {
            LLInventoryModel::cat_array_t cats;
            LLInventoryModel::item_array_t items;
            gInventory.collectDescendents(cof, cats, items, false);
            scan.expected = 0;
            for (const LLPointer<LLViewerInventoryItem>& item : items)
            {
                if (!item)
                {
                    continue;
                }
                const LLAssetType::EType type = item->getType();
                if (type == LLAssetType::AT_LINK)
                {
                    ++scan.unresolved_links;    // its item not fetched yet
                }
                else if (type == LLAssetType::AT_OBJECT)
                {
                    ++scan.expected;
                    scan.worn += self->isWearingAttachment(item->getLinkedUUID()) ? 1 : 0;
                }
            }
        }
        return scan;
    }

    void sample_queues(QueueSums& sums)
    {
        LLTextureFetch* fetch = LLAppViewer::getTextureFetch();
        LLImageDecodeThread* decode = LLAppViewer::getImageDecodeThread();
        ++sums.samples;
        if (fetch)
        {
            sums.tex_requests += fetch->getNumRequests();
            sums.http_waiting += fetch->getHttpWaitersCount();
        }
        if (decode)
        {
            sums.decode += (F64)decode->getPending();
        }
        sums.create += LLViewerTextureList::sCreatePending;
        {
            LLMutexLock lock(gMeshRepo.mMeshMutex);
            sums.mesh_pending += (F64)gMeshRepo.mPendingRequests.size();
        }
        sums.mesh_active += LLMeshRepoThread::sActiveHeaderRequests + LLMeshRepoThread::sActiveLODRequests +
                            LLMeshRepoThread::sActiveSkinRequests;
    }

    std::string describe(const QueueSums& sums)
    {
        if (!sums.samples)
        {
            return "no samples";
        }
        const F64 n = (F64)sums.samples;
        return llformat("textures fetching %.0f (waiting for an HTTP slot %.0f), decode queue %.0f, to create %.0f; "
                        "meshes queued in the viewer %.0f, in flight %.0f",
                        sums.tex_requests / n, sums.http_waiting / n, sums.decode / n, sums.create / n,
                        sums.mesh_pending / n, sums.mesh_active / n);
    }

    // Where the agent's textures still waiting are in the fetcher, by state.
    std::string texture_states(const Scan& scan)
    {
        LLTextureFetch* fetch = LLAppViewer::getTextureFetch();
        if (!fetch || scan.textures_waiting.empty())
        {
            return "none";
        }
        std::map<std::string, S32> states;
        for (const LLUUID& id : scan.textures_waiting)
        {
            const S32 state = fetch->getFetchState(id);
            ++states[state ? LLTextureFetch::getStateString(state) : std::string("not requested")];
        }
        std::string text;
        for (const auto& entry : states)
        {
            text += llformat("%s%s %d", text.empty() ? "" : ", ", entry.first.c_str(), entry.second);
        }
        return text;
    }

    // Where the agent's meshes still waiting are in the viewer's queue.
    std::string mesh_queue(const Scan& scan)
    {
        if (scan.meshes_waiting_ids.empty())
        {
            return "none";
        }
        S32 queued = 0, first = -1, last = -1;
        size_t total = 0;
        {
            LLMutexLock lock(gMeshRepo.mMeshMutex);
            total = gMeshRepo.mPendingRequests.size();
            for (size_t i = 0; i < total; ++i)
            {
                const std::shared_ptr<PendingRequestBase>& request = gMeshRepo.mPendingRequests[i];
                if (request && scan.meshes_waiting_ids.count(request->getId()))
                {
                    ++queued;
                    first = first < 0 ? (S32)i : first;
                    last = (S32)i;
                }
            }
        }
        if (!queued)
        {
            return llformat("none in the viewer's queue of %u (requested, or waiting for their header)", (U32)total);
        }
        return llformat("%d in the viewer's queue at %d..%d of %u", queued, first, last, (U32)total);
    }

    void reach(Milestone milestone, F64 now, bool reached)
    {
        if (!reached || sState.at[milestone] >= 0.0)
        {
            return;
        }
        sState.at[milestone] = now;
        if (log_on())
        {
            LL_INFOS("SelfFirst") << "self first (plus/selffirst): " << MILESTONE_NAMES[milestone] << " at " << since(now)
                                  << LL_ENDL;
        }
    }

    F64 span(Milestone from, Milestone to)
    {
        return (sState.at[from] >= 0.0 && sState.at[to] >= 0.0) ? sState.at[to] - sState.at[from] : -1.0;
    }

    // When the cloud goes: what it waited for last, and the longest waits.
    void summarize_cloud(F64 now)
    {
        if (!log_on())
        {
            return;
        }
        // The cloud goes CLOUD_SETTLE_SECONDS after the last thing it waited
        // for. The meshes: those of the attachments there at the time (one
        // that comes later does not hold the cloud).
        struct Gate { F64 at; const char* cause; };
        const Gate gates[] = {
            { sState.at[M_BAKE_IDS], "3: the outfit and bake ids from the servers (outside the viewer)" },
            { sState.at[M_GRAY_OFF], "1: the bakes' first data (texture fetch)" },
            { sState.meshes_ready,   "2: the attachments' meshes (mesh fetch)" },
        };
        const Gate* last = nullptr;
        for (const Gate& gate : gates)
        {
            if (gate.at >= 0.0 && (!last || gate.at > last->at))
            {
                last = &gate;
            }
        }
        // A mesh is ready soon after its object comes when the fetch is not the wait.
        std::string mesh_note;
        if (last == &gates[2] && sState.last_object_seen >= 0.0 &&
            sState.meshes_ready - sState.last_object_seen < 2.0)
        {
            mesh_note = " - but the last attachment came only just before, so the simulator's order (4) held it";
        }
        // From the moment the avatar was there (the first look after it was made).
        const F64 avatar = sState.at[M_AVATAR];
        const F64 server = (avatar >= 0.0 && sState.at[M_BAKE_IDS] >= 0.0) ? sState.at[M_BAKE_IDS] - avatar : -1.0;
        const F64 objects = (avatar >= 0.0 && sState.last_object_seen >= 0.0) ? sState.last_object_seen - avatar : -1.0;
        const F64 inside = llmax(span(M_BAKE_IDS, M_GRAY_OFF), sState.longest_mesh_wait);
        const F64 outside = llmax(server, objects);

        LL_INFOS("SelfFirst") << "self first (plus/selffirst): cloud gone at " << since(now)
                              << (switch_on() ? " with PlusSelfFirst on" : " (PlusSelfFirst off)") << LL_ENDL;
        LL_INFOS("SelfFirst") << "  waited for last: " << (last ? last->cause : "nothing seen") << mesh_note
                              << llformat(" (at %s, the cloud goes %.0f s after it)",
                                          last ? since(last->at).c_str() : "-", CLOUD_SETTLE_SECONDS)
                              << LL_ENDL;
        LL_INFOS("SelfFirst") << llformat("  outside the viewer (3, 4): bake ids %.1f s after the avatar was there "
                                          "(COF known %s, bake request %s, appearance message %s); "
                                          "last attachment object %.1f s after the avatar was there",
                                          server, since(sState.at[M_COF]).c_str(), since(sState.at[M_BAKE_REQUEST]).c_str(),
                                          since(sState.at[M_APPEARANCE]).c_str(), objects)
                              << LL_ENDL;
        LL_INFOS("SelfFirst") << llformat("  inside the viewer (1, 2): bakes' first data %.1f s after their ids "
                                          "(full resolution %.1f s); longest mesh wait after its object came %.1f s",
                                          span(M_BAKE_IDS, M_GRAY_OFF), span(M_BAKE_IDS, M_BAKES_FULL),
                                          sState.longest_mesh_wait)
                              << LL_ENDL;
        LL_INFOS("SelfFirst") << llformat("  longest: %s (inside %.1f s, outside %.1f s)",
                                          inside > outside ? "inside the viewer - 1 and 2, where PlusSelfFirst can help"
                                                           : "outside the viewer - 3 and 4, which PlusSelfFirst cannot speed up",
                                          inside, outside)
                              << LL_ENDL;
        LL_INFOS("SelfFirst") << "  the world's queues while the cloud was there (average): " << describe(sState.waiting)
                              << LL_ENDL;
    }

    void summarize_end(F64 now, const char* why)
    {
        if (!log_on())
        {
            return;
        }
        const Scan& scan = sState.last;
        LL_INFOS("SelfFirst") << "self first (plus/selffirst): done (" << why << ") at " << since(now) << LL_ENDL;
        std::string times;
        for (S32 m = 0; m < M_COUNT; ++m)
        {
            times += llformat("%s%s %s", m ? "; " : "", MILESTONE_NAMES[m], since(sState.at[m]).c_str());
        }
        LL_INFOS("SelfFirst") << "  " << times << LL_ENDL;
        LL_INFOS("SelfFirst") << llformat("  attachments: %d objects (%d COF objects, %d worn), %d mesh volumes; "
                                          "textures %d, without data %d, not at their level %d; "
                                          "longest texture wait for first data %.1f s",
                                          scan.attached, scan.expected, scan.worn, scan.mesh_volumes, scan.textures,
                                          scan.textures_no_data, scan.textures_not_done, sState.longest_texture_wait)
                              << LL_ENDL;
        LL_INFOS("SelfFirst") << "  the world's queues after the cloud (average): " << describe(sState.after) << LL_ENDL;
    }

    void report_waiting(F64 now, const Scan& scan)
    {
        if (!log_on())
        {
            return;
        }
        LL_INFOS("SelfFirst") << "self first (plus/selffirst): at " << since(now) << " still a cloud"
                              << (switch_on() ? " (PlusSelfFirst on)" : "") << ": "
                              << llformat("rez %d, COF %s, bake ids %s, bakes with data %d/%d (full %d), "
                                          "attachments %d attached (COF objects %d, worn %d, unresolved links %d, pending %d), "
                                          "meshes waiting %d of %d, textures without data %d of %d (not at their level %d)",
                                          scan.rez, scan.cof_version == LLViewerInventoryCategory::VERSION_UNKNOWN ? "unknown" : "known",
                                          scan.bake_ids ? "yes" : "no", scan.bakes_with_data, scan.bakes, scan.bakes_full,
                                          scan.attached, scan.expected, scan.worn, scan.unresolved_links, scan.pending_attachments,
                                          scan.meshes_waiting, scan.mesh_volumes, scan.textures_no_data, scan.textures,
                                          scan.textures_not_done)
                              << LL_ENDL;
        LL_INFOS("SelfFirst") << "  the agent's textures waiting, by fetch state: " << texture_states(scan)
                              << "; its meshes waiting: " << mesh_queue(scan) << LL_ENDL;
        QueueSums now_sample;
        sample_queues(now_sample);
        LL_INFOS("SelfFirst") << "  the world's queues now: " << describe(now_sample) << LL_ENDL;
    }

    // PlusSelfFirst: on from the login reply until the agent's avatar is done.
    void update_active(F64 now, const Scan* scan)
    {
        bool want = switch_on() && sState.tracking && !sState.active_over && sState.at[M_LOGIN_REPLY] >= 0.0;
        const char* why = sState.tracking ? "switched off" : "the log ended";
        if (want && ((sState.at[M_WORLD_SHOWN] >= 0.0 && now - sState.at[M_WORLD_SHOWN] > ACTIVE_SECONDS) ||
                     now - sState.at[M_LOGIN_REPLY] > ACTIVE_AFTER_LOGIN_SECONDS))
        {
            want = false;
            why = "time is up";
        }
        if (want && scan && scan->fully_loaded && scan->meshes_waiting == 0 && scan->textures_no_data == 0 &&
            scan->bakes_with_data == scan->bakes && sState.at[M_ATTACHED] >= 0.0)
        {
            want = false;
            why = "the avatar has everything";
        }
        if (want && scan)
        {
            sSelfTextures = scan->texture_ids;
            sSelfMeshesWaiting = scan->meshes_waiting;
        }
        if (want == sActive.load())
        {
            return;
        }
        sActive = want;
        if (!want)
        {
            // Once off, off for this login: an attachment worn later must not
            // slow the world again.
            sState.active_over = true;
            sSelfTextures.clear();
            sSelfMeshesWaiting = 0;
        }
        if (log_on() && (want || sState.active_said))
        {
            sState.active_said = true;
            LL_INFOS("SelfFirst") << "self first (plus/selffirst): PlusSelfFirst "
                                  << (want ? llformat("on: the agent's textures and meshes first, the world's requests "
                                                      "held to %.0f%% of the limit", WORLD_SHARE * 100.f)
                                           : std::string("off: the world back to full speed (") + why + ")")
                                  << " at " << since(now) << LL_ENDL;
        }
    }

    void track_objects(F64 now, const Scan& scan)
    {
        for (const LLUUID& id : scan.objects)
        {
            if (sState.object_seen.emplace(id, now).second)
            {
                sState.last_object_seen = now;
            }
        }
        for (const auto& entry : scan.object_mesh_waiting)
        {
            if (entry.second)
            {
                sState.mesh_wait_start.emplace(entry.first, now);
            }
            else
            {
                auto it = sState.mesh_wait_start.find(entry.first);
                if (it != sState.mesh_wait_start.end())
                {
                    sState.longest_mesh_wait = llmax(sState.longest_mesh_wait, now - it->second);
                    sState.mesh_wait_start.erase(it);
                }
            }
        }
        for (const LLUUID& id : scan.texture_ids)
        {
            LLViewerFetchedTexture* imagep = gTextureList.findImage(id, TEX_LIST_STANDARD);
            if (!imagep)
            {
                continue;
            }
            if (imagep->getDiscardLevel() < 0 && !imagep->isMissingAsset() && !plus::texprio::fetchretry::waiting(imagep))   // research/299
            {
                sState.texture_seen.emplace(id, now);
            }
            else
            {
                auto it = sState.texture_seen.find(id);
                if (it != sState.texture_seen.end())
                {
                    sState.longest_texture_wait = llmax(sState.longest_texture_wait, now - it->second);
                    sState.texture_seen.erase(it);
                }
            }
        }
    }

    bool tick()
    {
        if (!sState.tracking)
        {
            return false;
        }
        const F64 now = now_seconds();
        if (now - sState.last_scan < SCAN_SECONDS)
        {
            return false;
        }
        sState.last_scan = now;

        const Scan scan = take_scan();
        reach(M_AVATAR, now, scan.avatar);
        reach(M_COF, now, scan.cof_version != LLViewerInventoryCategory::VERSION_UNKNOWN);
        reach(M_BAKE_REQUEST, now, scan.bake_requested >= 0);
        reach(M_APPEARANCE, now, scan.appearance_received >= 0);
        reach(M_BAKE_IDS, now, scan.avatar && scan.bake_ids);
        reach(M_GRAY_OFF, now, scan.avatar && scan.rez >= 2);
        reach(M_BAKES_FULL, now, scan.avatar && scan.rez >= 3);
        const bool attached = scan.expected >= 0 && scan.unresolved_links == 0 && scan.worn >= scan.expected &&
                              scan.pending_attachments == 0;
        reach(M_ATTACHED, now, attached);
        reach(M_MESHES, now, attached && scan.meshes_waiting == 0);
        const bool cloud_was = sState.at[M_CLOUD_OFF] < 0.0;
        reach(M_CLOUD_OFF, now, scan.avatar && scan.fully_loaded);
        reach(M_TEXTURES_DATA, now, attached && scan.textures_no_data == 0);
        reach(M_TEXTURES_DONE, now, attached && scan.textures_not_done == 0);
        track_objects(now, scan);
        if (sState.last.meshes_waiting > 0 && scan.meshes_waiting == 0)
        {
            sState.meshes_ready = now;
        }

        if (scan.avatar && sState.at[M_CLOUD_OFF] < 0.0)
        {
            sample_queues(sState.waiting);
        }
        else if (sState.at[M_CLOUD_OFF] >= 0.0)
        {
            sample_queues(sState.after);
        }
        sState.last = scan;
        update_active(now, &scan);

        if (cloud_was && sState.at[M_CLOUD_OFF] >= 0.0)
        {
            summarize_cloud(now);
        }
        else if (scan.avatar && sState.at[M_CLOUD_OFF] < 0.0 && sState.at[M_WORLD_SHOWN] >= 0.0 &&
                 now - sState.last_report >= REPORT_SECONDS)
        {
            sState.last_report = now;
            report_waiting(now, scan);
        }

        const char* end = nullptr;
        if (sState.at[M_TEXTURES_DONE] >= 0.0 && sState.at[M_CLOUD_OFF] >= 0.0)
        {
            end = "the cloud and the attachment textures are done";
        }
        else if (sState.at[M_CLOUD_OFF] >= 0.0 && now - sState.at[M_CLOUD_OFF] > TEXTURES_AFTER_CLOUD_SECONDS)
        {
            end = "the attachment textures not all at their level 60 s after the cloud";
        }
        else if (sState.at[M_WORLD_SHOWN] >= 0.0 && now - sState.at[M_WORLD_SHOWN] > GIVE_UP_SECONDS)
        {
            end = sState.at[M_CLOUD_OFF] < 0.0 ? "still a cloud 300 s after the world showed" : "300 s after the world showed";
        }
        if (end)
        {
            if (sState.at[M_CLOUD_OFF] < 0.0)
            {
                report_waiting(now, scan);
            }
            summarize_end(now, end);
            sState.tracking = false;
            update_active(now, nullptr);
        }
        return false;   // stays registered: a later login (after a failed one) starts over
    }

    bool on_startup_state(const LLSD& info)
    {
        const S32 state = info["enum"].asInteger();
        const F64 now = now_seconds();
        if (state == STATE_LOGIN_AUTH_INIT)
        {
            reset();
            sState.tracking = log_on() || switch_on();
            sState.login = now;
            return false;
        }
        if (!sState.tracking)
        {
            return false;
        }
        switch (state)
        {
        case STATE_WORLD_INIT:     reach(M_LOGIN_REPLY, now, true); update_active(now, nullptr); break;
        case STATE_PRECACHE:       reach(M_PRECACHE, now, true); break;
        case STATE_WEARABLES_WAIT: reach(M_WEARABLES_WAIT, now, true); break;
        case STATE_STARTED:        reach(M_WORLD_SHOWN, now, true); break;
        default: break;
        }
        return false;
    }

    void init()
    {
        reset();
        LLEventPumps::instance().obtain("StartupState").listen("PlusSelfFirst", &on_startup_state);
        doOnIdleRepeating(&tick);
    }

    plus::FeatureRegistration sRegistration({ "selffirst", &init });
}

namespace plus::selffirst
{
    float fetchPriority(const LLUUID& id, float priority)
    {
        if (!sActive.load(std::memory_order_relaxed) || priority <= 0.f || !sSelfTextures.count(id))
        {
            return priority;
        }
        return SELF_TEXTURE_PRIORITY + priority;
    }

    bool isOwnPriority(float priority)
    {
        return priority >= SELF_TEXTURE_PRIORITY;
    }

    bool countWorldRequest(float priority)
    {
        if (isOwnPriority(priority))
        {
            return false;   // the agent's (fetchPriority)
        }
        sWorldTexturesInFlight.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    void uncountWorldRequest()
    {
        sWorldTexturesInFlight.fetch_sub(1, std::memory_order_relaxed);
    }

    bool holdTexture(float priority, int high_water)
    {
        return sActive.load(std::memory_order_relaxed) && priority < SELF_TEXTURE_PRIORITY &&
               sWorldTexturesInFlight.load(std::memory_order_relaxed) >= (int)((F32)high_water * WORLD_SHARE);
    }

    bool skipWaiterScan(int high_water)
    {
        if (!sActive.load(std::memory_order_relaxed) ||
            sWorldTexturesInFlight.load(std::memory_order_relaxed) < (int)((F32)high_water * WORLD_SHARE))
        {
            return false;   // as upstream: the world may go too
        }
        static std::atomic<U64> last_scan{ 0 };
        const U64 now = LLTimer::getCurrentClockCount();
        const U64 interval = (U64)(get_timer_info().mClockFrequency / WAITER_SCANS);
        U64 last = last_scan.load(std::memory_order_relaxed);
        if (now >= last && now - last < interval)
        {
            return true;
        }
        last_scan.store(now, std::memory_order_relaxed);
        return false;
    }

    float meshScore(const LLVOAvatar* avatar, float score)
    {
        if (!sActive.load(std::memory_order_relaxed) || !avatar || avatar != gAgentAvatarp.get())
        {
            return score;
        }
        return SELF_MESH_SCORE + score;
    }

    int meshPushCount(int push_count, int active_count, int high_water)
    {
        if (!sActive.load(std::memory_order_relaxed))
        {
            return push_count;
        }
        // The mesh queues do not tell whose a request is: the agent's waiting meshes (volumes, about one
        // request each) are taken as in flight, the rest of active_count as the world's. An estimate: the
        // world may get a little more or less than its share, but the agent's no longer use it up.
        const int self_waiting = sSelfMeshesWaiting.load(std::memory_order_relaxed);
        const int world_active = llmax(0, active_count - self_waiting);
        const int world_room   = llmax(0, (int)((F32)high_water * WORLD_SHARE) - world_active);
        return llmin(push_count, world_room + self_waiting);
    }
}

/**
 * @file llmissingobjects.cpp
 * @brief Diagnostics for objects in view that are not drawn.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Charis Viewer Source Code
 * Copyright (C) 2026, Charis Viewer contributors
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

#include "llmissingobjects.h"

#include "lldrawable.h"
#include "llface.h"
#include "llappviewer.h"
#include "llfetchedgltfmaterial.h"
#include "lltexturefetch.h"
#include "llviewertexture.h"
#include "llfile.h"
#include "llframetimer.h"
#include "llmeshrepository.h"
#include "llspatialpartition.h"
#include "llstartup.h"
#include "lltextureentry.h"
#include "llviewercamera.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llvovolume.h"
#include "pipeline.h"

#include <algorithm>
#include <map>
#include <cstdlib>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace
{
    enum State
    {
        MO_OK, MO_OCCLUDED, MO_TRANSPARENT, MO_QUEUED, MO_MESH_LOADING,                    // can be normal
        MO_ORPHANED, MO_NO_DRAWABLE, MO_FORCE_INVISIBLE, MO_NO_GROUP, MO_NO_VOLUME,        // should not last
        MO_MESH_UNAVAILABLE, MO_DIRTY_NOT_QUEUED, MO_NO_GEOMETRY,
        MO_STATE_COUNT
    };
    const char* const STATE_NAMES[MO_STATE_COUNT] = {
        "ok", "occluded", "fully transparent", "queued to build", "mesh loading",
        "orphaned", "no drawable", "force invisible", "no spatial group", "no volume",
        "mesh unavailable", "geometry dirty but not queued", "no geometry" };

    // How long a state may last before the object is reported (0: never reported).
    F64 report_after(State state)
    {
        switch (state)
        {
            case MO_OK:
            case MO_OCCLUDED:          // behind something, or stuck: the dump lists the nearest
            case MO_TRANSPARENT:
            case MO_QUEUED:            // the load radius keeps far groups queued for a while
                return 0.0;
            case MO_MESH_LOADING:
                return 60.0;
            default:
                return 15.0;
        }
    }

    struct Seen
    {
        State state = MO_OK;
        F64 since = 0.0;
        bool reported = false;
    };
    std::unordered_map<LLUUID, Seen> sSeen;

    struct Entry
    {
        LLViewerObject* object;
        State state;
        F32 distance;
    };

    State classify(LLViewerObject* object)
    {
        if (object->isOrphaned())
        {
            return MO_ORPHANED;
        }
        LLDrawable* drawable = object->mDrawable;
        if (!drawable)
        {
            return MO_NO_DRAWABLE;
        }
        if (drawable->isState(LLDrawable::FORCE_INVISIBLE))
        {
            return MO_FORCE_INVISIBLE;
        }
        LLSpatialGroup* group = drawable->getSpatialGroup();
        if (!group || group->isDead())
        {
            return MO_NO_GROUP;
        }
        LLVOVolume* volume = (LLVOVolume*)object;
        if (!volume->getVolume())
        {
            return MO_NO_VOLUME;
        }
        if (volume->isMesh())
        {
            if (volume->getVolume()->isMeshAssetUnavaliable())
            {
                return MO_MESH_UNAVAILABLE;
            }
            if (!volume->getVolume()->isMeshAssetLoaded())
            {
                return MO_MESH_LOADING;
            }
        }
        if (group->hasState(LLSpatialGroup::GEOM_DIRTY))
        {
            return group->hasState(LLSpatialGroup::IN_BUILD_Q1) ? MO_QUEUED : MO_DIRTY_NOT_QUEUED;
        }
        bool any_visible_te = false;
        for (U8 i = 0; i < object->getNumTEs() && !any_visible_te; ++i)
        {
            const LLTextureEntry* te = object->getTE(i);
            any_visible_te = te && te->getColor().mV[VALPHA] > 0.f;
        }
        if (object->getNumTEs() && !any_visible_te)
        {
            return MO_TRANSPARENT;
        }
        S32 built = 0;
        for (S32 i = 0; i < drawable->getNumFaces(); ++i)
        {
            const LLFace* face = drawable->getFace(i);
            built += face && face->getVertexBuffer() && face->getGeomCount() > 0 ? 1 : 0;
        }
        if (drawable->getNumFaces() && !built)
        {
            return MO_NO_GEOMETRY;
        }
        if (group->isOcclusionState(LLSpatialGroup::OCCLUDED))
        {
            return MO_OCCLUDED;
        }
        return MO_OK;
    }

    std::string describe(const Entry& entry)
    {
        LLViewerObject* object = entry.object;
        std::ostringstream out;
        out << STATE_NAMES[entry.state] << ": " << object->getID() << " local " << object->getLocalID();
        LLViewerRegion* region = object->getRegion();
        out << " region '" << (region ? region->getName() : std::string()) << "' pos " << object->getPositionRegion()
            << " distance " << entry.distance << " scale " << object->getScale();
        if (object->isOrphaned())
        {
            out << " orphaned";
        }
        if (LLVOVolume* volume = (LLVOVolume*)object; volume->getVolume())
        {
            if (volume->isMesh())
            {
                const LLUUID& mesh_id = volume->getMeshID();
                out << " mesh " << mesh_id << " lod " << volume->getLOD() << (gMeshRepo.hasHeader(mesh_id) ? " header" : " no-header")
                    << (volume->getVolume()->isMeshAssetLoaded() ? " loaded" : "")
                    << (volume->getVolume()->isMeshAssetUnavaliable() ? " unavailable" : "");
                if (volume->isRiggedMeshFast())
                {
                    out << (volume->getSkinInfo() ? " skin" : " no-skin");
                }
            }
            out << " volume faces " << volume->getVolume()->getNumVolumeFaces();
        }
        if (LLDrawable* drawable = object->mDrawable)
        {
            S32 built = 0;
            for (S32 i = 0; i < drawable->getNumFaces(); ++i)
            {
                const LLFace* face = drawable->getFace(i);
                built += face && face->getVertexBuffer() && face->getGeomCount() > 0 ? 1 : 0;
            }
            out << " faces " << drawable->getNumFaces() << " built " << built << " drawable state 0x" << std::hex << drawable->getState() << std::dec;
            if (LLSpatialGroup* group = drawable->getSpatialGroup())
            {
                out << " group state 0x" << std::hex << group->getState() << " occlusion 0x" << group->getOcclusionState() << std::dec
                    << " draw infos " << group->mDrawMap.size() << (group->isDead() ? " dead" : "");
            }
        }
        return out.str();
    }

    // Objects in the world camera's view and within the draw distance.
    void collect(std::vector<Entry>& entries)
    {
        const LLViewerCamera* camera = LLViewerCamera::getInstance();
        const LLVector3 eye = camera->getOrigin();
        for (S32 i = 0; i < gObjectList.getNumObjects(); ++i)
        {
            LLViewerObject* object = gObjectList.getObject(i);
            if (!object || object->getPCode() != LL_PCODE_VOLUME || !object->getRegion() || object->getAvatarAncestor())
            {
                continue;
            }
            const LLVector3 center = object->getPositionAgent();
            const F32 radius = object->getScale().length() * 0.5f;
            const F32 distance = llmax(0.f, (center - eye).length() - radius);
            if (distance > LLPipeline::RenderFarClip || !camera->sphereInFrustum(center, radius))
            {
                continue;
            }
            entries.push_back({ object, classify(object), distance });
        }
    }

    void log_counts(const std::vector<Entry>& entries, const char* what)
    {
        U32 counts[MO_STATE_COUNT] = {};
        for (const Entry& entry : entries)
        {
            ++counts[entry.state];
        }
        std::ostringstream out;
        out << what << ": " << entries.size() << " objects in view";
        for (U32 s = 0; s < MO_STATE_COUNT; ++s)
        {
            if (counts[s])
            {
                out << ", " << STATE_NAMES[s] << " " << counts[s];
            }
        }
        LL_INFOS("MissingObjects") << out.str() << LL_ENDL;
    }

    void watch(const std::vector<Entry>& entries)
    {
        const F64 now = LLFrameTimer::getElapsedSeconds();
        std::unordered_map<LLUUID, Seen> seen;
        for (const Entry& entry : entries)
        {
            const F64 limit = report_after(entry.state);
            if (limit <= 0.0)
            {
                continue;
            }
            Seen current;
            auto before = sSeen.find(entry.object->getID());
            if (before != sSeen.end() && before->second.state == entry.state)
            {
                current = before->second;
            }
            else
            {
                current.state = entry.state;
                current.since = now;
            }
            if (!current.reported && now - current.since >= limit)
            {
                current.reported = true;
                LL_WARNS("MissingObjects") << "for " << (S32)(now - current.since) << " s: " << describe(entry) << LL_ENDL;
            }
            seen[entry.object->getID()] = current;
        }
        sSeen.swap(seen);
    }

    void dump(std::vector<Entry>& entries)
    {
        log_counts(entries, "dump");
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.distance < b.distance; });
        constexpr U32 MAX_PER_STATE = 50;
        U32 listed[MO_STATE_COUNT] = {};
        for (const Entry& entry : entries)
        {
            if (entry.state == MO_OK || entry.state == MO_TRANSPARENT || listed[entry.state]++ >= MAX_PER_STATE)
            {
                continue;
            }
            LL_INFOS("MissingObjects") << describe(entry) << LL_ENDL;
        }
    }
}

namespace
{
    void dump_textures()
    {
        constexpr F32 RANGE = 32.f;
        const LLViewerCamera* camera = LLViewerCamera::getInstance();
        const LLVector3 eye = camera->getOrigin();
        // Each texture once, at the nearest object using it.
        std::map<LLViewerFetchedTexture*, std::pair<F32, LLViewerObject*>> textures;
        auto note = [&](LLViewerTexture* texture, F32 distance, LLViewerObject* object)
        {
            LLViewerFetchedTexture* fetched = texture ? LLViewerTextureManager::staticCastToFetchedTexture(texture) : nullptr;
            if (!fetched)
            {
                return;
            }
            auto found = textures.find(fetched);
            if (found == textures.end() || distance < found->second.first)
            {
                textures[fetched] = { distance, object };
            }
        };
        for (S32 i = 0; i < gObjectList.getNumObjects(); ++i)
        {
            LLViewerObject* object = gObjectList.getObject(i);
            if (!object || object->getPCode() != LL_PCODE_VOLUME || !object->getRegion())
            {
                continue;
            }
            const LLVector3 center = object->getPositionAgent();
            const F32 radius = object->getScale().length() * 0.5f;
            const F32 distance = llmax(0.f, (center - eye).length() - radius);
            if (distance > RANGE || !camera->sphereInFrustum(center, radius))
            {
                continue;
            }
            for (U8 te = 0; te < object->getNumTEs(); ++te)
            {
                note(object->getTEImage(te), distance, object);
                note(object->getTENormalMap(te), distance, object);
                note(object->getTESpecularMap(te), distance, object);
                const LLTextureEntry* entry = object->getTE(te);
                LLFetchedGLTFMaterial* material = entry ? (LLFetchedGLTFMaterial*)entry->getGLTFRenderMaterial() : nullptr;
                if (material)
                {
                    note(material->mBaseColorTexture, distance, object);
                    note(material->mNormalTexture, distance, object);
                    note(material->mMetallicRoughnessTexture, distance, object);
                    note(material->mEmissiveTexture, distance, object);
                }
            }
        }

        std::vector<std::pair<F32, LLViewerFetchedTexture*>> blurry;
        for (const auto& entry : textures)
        {
            LLViewerFetchedTexture* texture = entry.first;
            const S32 discard = texture->getDiscardLevel();
            if (discard < 0 || discard > texture->getDesiredDiscardLevel())
            {
                blurry.emplace_back(entry.second.first, texture);
            }
        }
        // Where the short ones wait.
        U32 create_pending = 0, fast_cache = 0, fetching = 0, idle_fetcher = 0, not_requested = 0, missing = 0;
        for (const auto& entry : blurry)
        {
            LLViewerFetchedTexture* texture = entry.second;
            if (texture->isMissingAsset()) ++missing;
            else if (texture->mCreatePending) ++create_pending;
            else if (texture->isInFastCacheList()) ++fast_cache;
            else if (texture->isFetching()) ++fetching;
            else if (texture->hasFetcher()) ++idle_fetcher;
            else ++not_requested;
        }
        std::sort(blurry.begin(), blurry.end(),
                  [](const std::pair<F32, LLViewerFetchedTexture*>& a, const std::pair<F32, LLViewerFetchedTexture*>& b) { return a.first < b.first; });
        LL_INFOS("MissingObjects") << "textures within " << RANGE << " m in view: " << textures.size() << ", short of their resolution: "
                                   << blurry.size() << " (waiting for GL creation " << create_pending << ", fast cache " << fast_cache
                                   << ", fetching " << fetching << ", finished fetcher not yet removed " << idle_fetcher
                                   << ", not requested " << not_requested << ", missing " << missing << ")" << LL_ENDL;
        LLTextureFetch* fetch = LLAppViewer::getTextureFetch();
        constexpr size_t MAX_LISTED = 80;
        for (size_t i = 0; i < blurry.size() && i < MAX_LISTED; ++i)
        {
            LLViewerFetchedTexture* texture = blurry[i].second;
            std::ostringstream out;
            out << "texture " << texture->getID() << " distance " << blurry[i].first << " size " << texture->getFullWidth() << "x"
                << texture->getFullHeight() << " discard " << texture->getDiscardLevel() << " wants " << texture->getDesiredDiscardLevel()
                << " boost " << texture->getBoostLevel() << " vsize " << (S32)texture->getMaxVirtualSize()
                << (texture->hasFetcher() ? " has-fetcher" : "") << (texture->isFetching() ? " fetching" : "")
                << (texture->isMissingAsset() ? " MISSING" : "") << (texture->mCreatePending ? " create-pending" : "")
                << (texture->isInFastCacheList() ? " fast-cache" : "") << (texture->mDownScalePending ? " downscale-pending" : "")
                << " fetch-priority " << texture->getFetchPriority();
            if (fetch)
            {
                F32 progress = 0.f, requested_priority = 0.f, fetch_dtime = 0.f, request_dtime = 0.f;
                U32 fetch_priority = 0;
                bool can_use_http = false;
                const S32 state = fetch->getFetchState(texture->getID(), progress, requested_priority, fetch_priority, fetch_dtime,
                                                       request_dtime, can_use_http);
                out << " worker " << LLTextureFetch::getStateString(state) << " progress " << progress << " requested-priority "
                    << requested_priority << " fetch-dt " << fetch_dtime << " request-dt " << request_dtime;
            }
            LL_INFOS("MissingObjects") << out.str() << LL_ENDL;
        }
    }
}

void LLMissingObjects::update()
{
    static const F32 interval = []()
    {
        const char* value = getenv("LLRHI_MISSING_WATCH");
        return value ? (F32)atof(value) : 0.f;
    }();
    static const std::string cue = []()
    {
        const std::string dir = LLStringUtil::getenv("LLRHI_CUE_DIR");     // UTF-8, as LLFile takes it
        return dir.empty() ? std::string() : dir + "/missing.dump";
    }();
    static const std::string texture_cue = []()
    {
        const std::string dir = LLStringUtil::getenv("LLRHI_CUE_DIR");
        return dir.empty() ? std::string() : dir + "/textures.dump";
    }();
    if (interval <= 0.f && cue.empty())
    {
        return;
    }
    if (LLStartUp::getStartupState() < STATE_STARTED || LLViewerCamera::sCurCameraID != LLViewerCamera::CAMERA_WORLD)
    {
        return;
    }

    static LLFrameTimer watch_timer;
    static LLFrameTimer cue_timer;
    const bool watch_now = interval > 0.f && watch_timer.getElapsedTimeF32() >= interval;
    bool dump_now = false;
    if (!cue.empty() && cue_timer.getElapsedTimeF32() >= 2.f)
    {
        cue_timer.reset();
        dump_now = LLFile::isfile(cue) && LLFile::remove(cue) == 0;
        if (LLFile::isfile(texture_cue) && LLFile::remove(texture_cue) == 0)
        {
            dump_textures();
        }
    }
    if (!watch_now && !dump_now)
    {
        return;
    }

    std::vector<Entry> entries;
    collect(entries);
    if (watch_now)
    {
        watch_timer.reset();
        log_counts(entries, "watch");
        watch(entries);
    }
    if (dump_now)
    {
        dump(entries);
    }
}

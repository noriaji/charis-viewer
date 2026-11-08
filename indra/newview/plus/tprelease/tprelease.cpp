/**
 * @file tprelease.cpp
 * @brief After a teleport, lets go of the old region's textures without
 *        waiting for the simulator to drop the region.
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

// On by default; LLRHI_TP_RELEASE=0 leaves the textures to upstream's ways.
//
// research/97. What happens without it, after a teleport to a far region:
// - The old region's objects live on until the simulator lets the region go
//   (Le'eaf -> sui: 50 s), far away in the agent's new frame and never drawn.
//   Their textures keep their images: at a discard bias of 1 an unboosted
//   texture's virtual size is never reset (upstream, against up and down
//   rezzing), and once it is reset, a size that small (10 or under) takes
//   the branch of LLViewerLODTexture::processTextureStats that never scales
//   down. Texture pressure stages 2 and 3 (research/79) live in the other
//   branch, so they do not reach them either.
// - When the region goes, its textures leave the list 30 s after their last
//   reference (the lazy flush): 80 s and more in all, while the new region's
//   textures arrive in the first 10 to 30 s and VRAM runs over.
//
// With it, for WINDOW_SECONDS after the arrival:
// - A texture whose faces are all far past the draw distance and none of
//   which has been on screen since the arrival gets its virtual size from
//   those faces alone (so nothing more is fetched for it) and, from its next
//   visit (about a second), is scaled down (to the smallest level, from that
//   far), SENT_PER_FRAME a frame, MIN_SIDE and up. Faces
//   that far cannot be drawn: nothing on screen changes. Boosted textures
//   (avatars, terrain, UI, sculpts...), light and sculpt maps, and textures
//   with a face in reach are left alone. A texture needed again (a teleport
//   back) comes back from the disk cache like any other.
// - A texture last on screen before the arrival that nothing refers to any
//   more leaves the list after FLUSH_SECONDS instead of 30 s.
// With PlusRegionPark (plus/regionpark, research/242), a texture seen on one of
// the last few visits is left alone while VRAM has room, and kept in the list
// longer once unused; under texture pressure the above applies to it again.
// Log (tag TPRelease): the arrival, then at +10, +30, +60 and +120 s the
// textures sent down and VRAM, the backend's texture bytes and the list size
// against the arrival.

#include "llviewerprecompiledheaders.h"

#include "plus.h"

#include "tprelease.h"

#include "plus/regionpark/regionpark.h"

#include "llagent.h"
#include "llappviewer.h"
#include "llcallbacklist.h"
#include "llrender.h"
#include "llrhi.h"
#include "llimageworker.h"
#include "lltexturefetch.h"
#include "llviewercamera.h"
#include "llviewertexture.h"
#include "llviewertexturelist.h"

#include <array>
#include <cstdlib>
#include <cstring>

namespace
{
    constexpr F32 WINDOW_SECONDS = 120.f;   // after the arrival
    constexpr F32 FAR_MARGIN = 512.f;       // past the draw distance (a prim is at most 256 m across)
    constexpr S32 MIN_SIDE = 128;           // smaller ones go with the region: little to gain for a scale-down each
    constexpr U32 SENT_PER_FRAME = 64;      // scale-downs queued a frame: each draws and copies a texture
    constexpr F32 FLUSH_SECONDS = 5.f;      // upstream: 30 s
    // Seen this long before the arrival still counts as seen since: the time a
    // texture was last seen moves only when the time-sliced scan visits it.
    constexpr F32 ARRIVAL_GRACE_SECONDS = 0.5f;
    constexpr std::array<F32, 4> REPORT_AT = { 10.f, 30.f, 60.f, WINDOW_SECONDS };

    constexpr F32 MB = 1024.f * 1024.f;

    struct Snapshot
    {
        bool real = false;      // VRAM figures from the backend (Vulkan with VK_EXT_memory_budget)
        F32 vram_mb = 0.f;
        F32 budget_mb = 0.f;
        F32 textures_mb = 0.f;
        S32 listed = 0;
    };

    struct State
    {
        F32 arrival = -1.f;     // gFrameTimeSeconds at the last arrival; below 0: none
        U32 generation = 0;     // arrivals so far (an older report stops)
        Snapshot at_arrival;
        U32 sent = 0;           // scale-downs queued since the arrival
        U64 sent_bytes = 0;     // what their images took
        U32 frame = 0;
        U32 sent_this_frame = 0;
    };
    State sState;

    bool window_open()
    {
        return sState.arrival >= 0.f && (F32)gFrameTimeSeconds - sState.arrival < WINDOW_SECONDS;
    }

    // On screen since the arrival (or just before it), or new.
    bool seen_since_arrival(const LLViewerFetchedTexture* imagep)
    {
        return imagep->mRhiSeenTime >= sState.arrival - ARRIVAL_GRACE_SECONDS;
    }

    Snapshot take_snapshot()
    {
        Snapshot snapshot;
        U64 texture_bytes = 0, usage = 0, budget = 0;
        snapshot.real = LLRHI::sVRAMBudget && LLRHI::sVRAMBudget(texture_bytes, usage, budget);
        snapshot.vram_mb = (F32)usage / MB;
        snapshot.budget_mb = (F32)budget / MB;
        snapshot.textures_mb = (F32)texture_bytes / MB;
        snapshot.listed = gTextureList.getNumImages();
        return snapshot;
    }

    std::string describe(const Snapshot& now, const Snapshot& before)
    {
        if (!now.real)
        {
            return llformat("%d textures in the list (%d at the arrival)", now.listed, before.listed);
        }
        return llformat("VRAM %.0f of %.0f MB (%.0f at the arrival), texture images %.0f MB (%.0f), "
                        "%d textures in the list (%d)",
                        now.vram_mb, now.budget_mb, before.vram_mb, now.textures_mb, before.textures_mb,
                        now.listed, before.listed);
    }

    // What the texture's image takes now (every level).
    U64 image_bytes(const LLViewerFetchedTexture* imagep)
    {
        const LLImageGL* gl = imagep->getGLTexture();
        if (!gl)
        {
            return 0;
        }
        if (LLRHI::sTextureImageBytes)
        {
            const U32 name = gl->getTexName();
            U64 bytes = 0;
            LLRHI::sTextureImageBytes(&name, 1, &bytes);
            return bytes;
        }
        return (U64)gl->getWidth() * (U64)gl->getHeight() * 4 * 4 / 3;    // RGBA, with its mips
    }

    // Where a slow load waits: requests open, waiting for a connection, and images waiting to be decoded.
    std::string fetch_backlog()
    {
        LLTextureFetch* fetch = LLAppViewer::getTextureFetch();
        LLImageDecodeThread* decode = LLAppViewer::getImageDecodeThread();
        if (!fetch || !decode)
        {
            return std::string();
        }
        // research/296: getNumHTTPRequests() counts the requests sent (in flight on HTTP), not those waiting for a slot
        return llformat("; fetch: %d requests, %d in flight on HTTP, %d waiting for a slot, %u in its queue, %u images waiting to decode",
                        fetch->getNumRequests(), fetch->getNumHTTPRequests(), fetch->getHttpWaitersCount(), (U32)fetch->getPending(),
                        (U32)decode->getPending());
    }

    void report(F32 since)
    {
        LL_INFOS("TPRelease") << llformat("+%.0f s after the teleport (plus/tprelease): sent down %u textures (%.0f MB of images), ",
                                          since, sState.sent, sState.sent_bytes / MB)
                              << describe(take_snapshot(), sState.at_arrival)
                              << llformat(", discard bias %.2f, texture pressure %d",
                                          LLViewerTexture::sDesiredDiscardBias, LLViewerTexture::sRhiTexPressure)
                              << fetch_backlog()
                              << LL_ENDL;
    }

    // Main thread, from LLAgent::setRegion(). A teleport moves the agent into
    // the new region while its state is TELEPORT_MOVING; a region crossing
    // (walking, flying, sitting on a vehicle) does it with no teleport going on.
    void on_region_changed()
    {
        if (gAgent.getTeleportState() == LLAgent::TELEPORT_NONE)
        {
            return;
        }
        sState.arrival = llmax((F32)gFrameTimeSeconds, 0.f);
        sState.at_arrival = take_snapshot();
        sState.sent = 0;
        sState.sent_bytes = 0;
        const U32 generation = ++sState.generation;

        LL_INFOS("TPRelease") << "teleport arrival (plus/tprelease): " << describe(sState.at_arrival, sState.at_arrival)
                              << llformat("; for %.0f s, textures whose faces are all past the draw distance + %.0f m go down, "
                                          "and unused ones leave the list after %.0f s",
                                          WINDOW_SECONDS, FAR_MARGIN, FLUSH_SECONDS)
                              << LL_ENDL;

        doOnIdleRepeating([generation, next = (size_t)0]() mutable
        {
            if (generation != sState.generation)
            {
                return true;    // a later arrival reports for itself
            }
            const F32 since = (F32)gFrameTimeSeconds - sState.arrival;
            if (since < REPORT_AT[next])
            {
                return false;
            }
            report(REPORT_AT[next]);
            return ++next >= REPORT_AT.size();
        });
    }

    void init()
    {
        const char* env = std::getenv("LLRHI_TP_RELEASE");
        if (env && *env && !strcmp(env, "0"))
        {
            LL_INFOS("TPRelease") << "off (LLRHI_TP_RELEASE=0)" << LL_ENDL;
            return;
        }
        gAgent.addRegionChangedCallback(&on_region_changed);
    }

    plus::FeatureRegistration sRegistration({ "tprelease", &init });
}

namespace plus::tprelease
{
    void onFaceScan(LLViewerFetchedTexture* imagep, F32 nearest, bool all_faces, bool seen)
    {
        if (!window_open() || seen || !all_faces
            || seen_since_arrival(imagep)
            || imagep->getType() != LLViewerTexture::LOD_TEXTURE
            || imagep->getBoostLevel() != LLGLTexture::BOOST_NONE
            || nearest < LLViewerCamera::getInstance()->getFar() + FAR_MARGIN
            || imagep->getNumVolumes(LLRender::LIGHT_TEX) > 0
            || imagep->getNumVolumes(LLRender::SCULPT_TEX) > 0
            || !imagep->getUseDiscard()
            || imagep->rhiForceToSaveRawImage()
            || plus::regionpark::skipScaleDown(imagep))    // last: parked while VRAM has room (research/242)
        {
            return;
        }

        // The size those faces need from where they are now (addTextureStats
        // follows): processTextureStats then wants no more for it.
        imagep->resetTextureStats();

        // Its desired level is from the last visit's processTextureStats (the
        // first visit here only resets it). For a size that small it does not
        // scale down by itself; for a larger one it has queued it already.
        if (imagep->mDownScalePending || !imagep->hasGLTexture())
        {
            return;
        }
        const S32 current = imagep->getDiscardLevel();
        if (current < 0 || imagep->getDesiredDiscardLevel() <= current || current >= imagep->getMaxDiscardLevel()
            || llmax(imagep->getWidth(), imagep->getHeight()) < MIN_SIDE)
        {
            return;
        }
        if (sState.frame != gFrameCount)
        {
            sState.frame = gFrameCount;
            sState.sent_this_frame = 0;
        }
        if (sState.sent_this_frame >= SENT_PER_FRAME)
        {
            return;     // its next visit (about a second)
        }
        const U64 bytes = image_bytes(imagep);
        if (imagep->scaleDown())
        {
            ++sState.sent_this_frame;
            ++sState.sent;
            sState.sent_bytes += bytes;
        }
    }

    bool windowOpen()
    {
        return window_open();
    }

    F32 lazyFlushTimeout(const LLViewerFetchedTexture* imagep, F32 timeout)
    {
        // Parked while VRAM has room (research/242): longer than upstream, also after the window.
        if (plus::regionpark::holdInList(imagep, timeout))
        {
            return timeout;
        }
        if (!window_open() || imagep->mRhiSeenTime <= 0.f || seen_since_arrival(imagep))
        {
            return timeout;
        }
        return llmin(timeout, FLUSH_SECONDS);
    }
}

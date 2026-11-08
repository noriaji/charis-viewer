/**
 * @file texprio.cpp
 * @brief Textures on screen first (research/285 tex-view-priority).
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

// Why (research/285 §1.2): at a discard bias of 3 and up every face's virtual
// size was divided by 16 (the bias applied on screen too: in_frustum is always
// true), so a small sign 35 m away and 30 degrees off the middle of the screen
// fell to 10 or under and its first level was never asked for: grey, on
// screen, for minutes. And the fetch queue was by size alone, so what had
// gone behind the camera took its turn with what was in front.
//
// What this file decides; the official files pass the values (llface.cpp: the
// view cone, llviewertexturelist.cpp: the face scan, the creation queue,
// llviewertexture.cpp: the fetch priority and the hold, and the coarsest level
// a texture on screen settles for: coarsestWanted, research/303).

#include "llviewerprecompiledheaders.h"

#include "plus.h"

#include "texprio.h"
#include "fetchflow.h"
#include "fetchretry.h"

#include "llappviewer.h"
#include "llface.h"
#include "llimage.h"
#include "llimagegl.h"
#include "llrender.h"
#include "llrhi.h"
#include "lldrawable.h"
#include "llfetchedgltfmaterial.h"
#include "llspatialpartition.h"
#include "llviewerobject.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewertexture.h"
#include "llviewertexturelist.h"
#include "pipeline.h"
#include "plus/presets/presets.h"
#include "plus/tplag/tplag.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include <string>
#include <unordered_set>

namespace plus::texprio
{
namespace
{
    // The fetch priority's bands (fetchPriority). A band's sizes stay under
    // BAND_SPAN so it never reaches the band above, and BAND_NEAR plus it
    // stays under plus/selffirst's 1e9 for the agent's own textures. T3 and
    // the textures of no tier keep their virtual size (at most 8192^2, under
    // BAND_VIEW). F32 has 24 bits: sizes in a band are told apart to about 32
    // pixels (BAND_NEAR) or 16 (BAND_VIEW), past the floor's 8x8.
    constexpr F32 BAND_NEAR = 4.0e8f;
    constexpr F32 BAND_VIEW = 2.0e8f;
    constexpr F32 BAND_SPAN = 1.9e8f;
    constexpr F32 NO_IMAGE_SCALE = 4.f;     // a texture with no image yet goes before one as large that is only blurry
    constexpr F32 OFF_FIRST_LEVEL = 1.f;    // T3's first level while VRAM is short: after everything else
    constexpr F32 HIDDEN_SHARE = 0.1f;      // FaceView::view_size

    // Grace (research/285 §2.6): seconds a face counts as in view after it
    // left the cone: with room, at pressure stage 1, at stage 2 and up.
    constexpr F32 GRACE_ROOM = 30.f;
    constexpr F32 GRACE_STAGE_1 = 10.f;
    constexpr F32 GRACE_STAGE_2 = 3.f;
    constexpr F32 OFF_HOLD_SECONDS = 1.f;   // holdSeconds (upstream 5 s)
    constexpr F32 SHORT_BIAS = 1.5f;        // with pressure stage 1 and up: "VRAM is short" (upstream's slam to 1.5)
    constexpr U32 FLOOR_PX_MAX = 64;
    // grayDrawn: with no image, "drawn now" (the stamp is written every frame it is drawn: a short window, so that what
    // left the screen, or lost its faces in a teleport, goes back to the face scan's word); with an image coarser
    // than the floor, the memory of having been drawn gray (the step to the floor's level after its first bytes)
    constexpr F32 GRAY_DRAWN_SECONDS = 2.f;
    constexpr F32 GRAY_MEMORY_SECONDS = 30.f;
    constexpr S32 GRAY_LISTS_REFS = 3;      // the lists' own references (mImageList, mUUIDMap, the fetch pass's entries)
    constexpr U32 FLOOR_LEVEL_MIN = 3;      // PlusTexOnScreenCoarsest: at least 3 (a 4096 side stays within 512)
    constexpr F32 OFF_STAGE_2_SCALE = 4.f;  // off_divisor at pressure stage 2 and up: a level more

    // The last resort (research/285 §2.3; §9 2 left to its default: fit the
    // budget, what is on screen one level down at most). The textures against
    // the limit (fill, 1: at it):
    // - Over it: once over for LAST_RESORT_WAIT on end, with what is off screen
    //   down to under LAST_RESORT_OFF_SHARE of what is on screen (the last
    //   round) and the sized pools' blocks holding under LAST_RESORT_SLACK more
    //   than their allocations (else lowering textures gives no block back:
    //   the repacking's business, research/285 §3), the divisor on screen goes
    //   up, from 1 to LAST_RESORT_MAX over LAST_RESORT_RAMP_UP.
    // - Under LAST_RESORT_DOWN_BELOW: it comes down again, from
    //   LAST_RESORT_MAX to 1 over LAST_RESORT_RAMP_DOWN, and keeps coming down
    //   until the textures pass LAST_RESORT_DOWN_UNTIL (one level is a quarter
    //   of a texture: a quick return took them over again at once).
    // - Otherwise it is held.
    // A new start waits LAST_RESORT_COOLDOWN after the divisor came back to
    // 1, twice as long each time it starts again within twice that.
    // LLRHI_TEX_VIEW_LAST_RESORT=0: never (the frame rate may fall instead).
    constexpr F32 LAST_RESORT_WAIT = 10.f;
    constexpr F32 LAST_RESORT_RAMP_UP = 10.f;
    constexpr F32 LAST_RESORT_RAMP_DOWN = 30.f;
    constexpr F32 LAST_RESORT_MAX = 4.f;
    constexpr F32 LAST_RESORT_DOWN_BELOW = 0.9f;
    constexpr F32 LAST_RESORT_DOWN_UNTIL = 0.95f;
    constexpr F32 LAST_RESORT_COOLDOWN = 60.f;
    constexpr F32 LAST_RESORT_COOLDOWN_MAX = 960.f;
    constexpr F64 LAST_RESORT_OFF_SHARE = 0.1;
    constexpr U64 LAST_RESORT_SLACK = 1024ull * 1024 * 1024;
    constexpr F32 SLACK_CHECK_SECONDS = 1.f;
    struct LastResort
    {
        F32 over_since = 0.f;       // frame time the textures went over the limit (0: not over)
        F32 slack_checked = 0.f;
        bool slack_small = true;
        bool slack_known = false;   // the sized pools answered (else taken as small)
        U64 slack = 0;
        bool coming_down = false;
        F32 ended_at = 0.f;         // when the divisor last came back to 1 (0: never)
        F32 cooldown = LAST_RESORT_COOLDOWN;
    };
    LastResort sLastResort;

    bool sCueOn = true;     // texview.toggle
    bool sFetch = false;
    bool sFree = false;
    bool sShort = false;    // VRAM is short
    F32 sFloorArea = 0.f;
    bool sGrayFloor = false;    // research/304: what is drawn as the gray stand-in is on screen, with faces or not
    F32 sFarClip = 0.f;     // the draw distance (floorSize)
    ScanValues sValues;
    S32 sOnScreenCoarsest = MAX_DISCARD_LEVEL;  // research/303: the coarsest level a texture on screen may settle for
    bool sSplitAvailable = false;   // research/309: the split by view is on, whatever bias_all says
    bool sFarDiag = false;  // research/296: the candidate pass and its counts run (a TPLag timeline is open)

    // Counters. A round: the fetch pass's visits until it went round every
    // texture (about a second; faster while the bias is up), kept as the
    // last full one. The rest: sums since the last log line.
    constexpr U32 WAIT_BUCKETS = 4;     // under 1 s, 1-3 s, 3-10 s, 10 s and over
    constexpr F32 INTO_VIEW_GIVE_UP = 10.f;     // in view this long and still short: counted as 10 s and over
    // research/294: a texture with no image for GRAY_STUCK_SECONDS (counted at its visit: once a round, in Round),
    // by why it still has none, the first that holds in this order. A few are named (UUID and numbers) per window,
    // each texture once while it stays without an image, only the reasons that point at the cause.
    constexpr F32 GRAY_STUCK_SECONDS = 10.f;
    constexpr U32 GRAY_NAMED_PER_WINDOW = 2;    // per reason (research/304: one reason does not take them all)
    constexpr U32 MISSING_NAMED_PER_WINDOW = 4;
    enum GrayReason : U32
    {
        GRAY_BOOSTED = 0,   // boost HIGH and up (UI, HUD...): no faces looked at
        GRAY_NO_FACES,      // no face uses it: no size, so no request (TIER_NONE)
        GRAY_OFF_SCREEN,    // faces, none in view lately
        GRAY_FETCHING,      // in view, a fetch in flight: the flow's business (research/294 section 3)
        GRAY_RETRY_WAIT,    // a failed fetch waits for its retry (research/299)
        GRAY_NO_SIZE,       // in view, no fetch, its size is under the request threshold
        GRAY_NOT_ASKED,     // in view with a size, and no fetch in flight
        GRAY_REASONS
    };
    const char* const GRAY_NAMES[GRAY_REASONS] = { "boosted", "no faces", "off screen", "fetching", "retry wait", "size too small",
                                                   "in view and not asked" };

    struct Round
    {
        U32 count[TIER_COUNT] = {};
        U32 no_image[TIER_COUNT] = {};
        U64 bytes[TIER_COUNT] = {};     // estimated from the GL sizes (RGB as 4 bytes, full chains, BC7 not known)
        U64 on_discard_sum = 0;         // T1 and T2 with an image
        U32 on_discard_count = 0;
        U64 over_need_sum = 0;          // T1 and T2 with an image and a known need: levels over it
        U32 over_need_count = 0;
        S32 over_need_max = 0;
        U64 off_discard_sum = 0;        // T3 with an image
        U32 off_discard_count = 0;
        U32 gray_stuck[GRAY_REASONS] = {};  // research/294: no image for GRAY_STUCK_SECONDS, by why
        // research/302: textures on screen (near, view) by the level they have (index 0..5, 6: none), and of those
        // at level 5, how many need no better (the level the faces need is 5 too: the first 600 bytes are the end)
        U32 on_level[7] = {};
        U32 on_five_needs_five = 0;
        U32 missing[TIER_COUNT] = {};       // research/294: flagged missing asset (no code clears it for a world texture), by tier
        // research/296 (diagnostics, counted only while sFarDiag): textures a grace cut by distance would put in T3
        // that are not there now (cut_*), candidates for "all faces far" (cand_*, of them fetching), ones held back
        // by a moving face (moving_*), ones with a face in the cone past far_cut (in_view_far); their images' bytes
        U32 cut_n = 0;
        U64 cut_bytes = 0;
        U32 cand_n = 0;
        U64 cand_bytes = 0;
        U32 cand_fetching = 0;
        U32 moving_n = 0;
        U64 moving_bytes = 0;
        U32 in_view_far = 0;
        bool far_incomplete = false;    // a visit of the round came while the pass was off: the counts are partial
    };
    Round sRound;
    Round sLastRound;
    bool sRoundDone = false;

    struct Sums
    {
        U32 gray_named[GRAY_REASONS] = {};     // research/304: per reason
        U32 missing_named = 0;
        U32 requests[TIER_COUNT] = {};
        U32 waits[TIER_COUNT][WAIT_BUCKETS] = {};   // asked for to data, by the tier when asked
        U32 into_view[WAIT_BUCKETS] = {};           // from T3 into view to the level needed
        U32 zeroed[TIER_COUNT] = {};    // fetches in flight set to priority 0 (aborted only before their request goes out)
        U32 scale_downs[TIER_COUNT] = {};
    };
    Sums sSums;

    const char* const TIER_NAMES[TIER_COUNT] = { "no faces", "near on screen", "on screen", "off screen" };

    F32 now()
    {
        return llmax((F32)gFrameTimeSeconds, 0.001f);   // 0 stands for "never" in the textures' times
    }

    U32 waitBucket(F32 seconds)
    {
        return seconds < 1.f ? 0 : seconds < 3.f ? 1 : seconds < 10.f ? 2 : 3;
    }

    U8 tierOf(LLViewerFetchedTexture* tex)
    {
        return tex->getBoostLevel() >= LLGLTexture::BOOST_HIGH ? (U8)TIER_NONE : llmin(tex->mRhiViewTier, (U8)(TIER_COUNT - 1));
    }

    bool onScreen(U8 tier)
    {
        return tier == TIER_NEAR || tier == TIER_VIEW;
    }

    // research/304: drawn as the gray stand-in (GrayWire's Scope stamps mGrayWireDrawnAt for the textures of a main
    // view batch that have no GL name). The face scan can miss it (a batch draws a texture no face lists: tier none,
    // a size of 0), and what is drawn is on screen. With no image: drawn in the last GRAY_DRAWN_SECONDS. With an image
    // (its first bytes made a GL name, so no stamp comes any more): within GRAY_MEMORY_SECONDS of the last stamp, and
    // not for a texture the scan puts off screen (it is believed then: only what has no face, or is on screen).
    bool grayDrawn(const LLViewerFetchedTexture* tex)
    {
        if (!sGrayFloor)
        {
            return false;
        }
        const F32 since = LLViewerTexture::sCurrentTime - tex->mGrayWireDrawnAt;
        // (with an image: still held by something besides the lists, as finishImageDecodePriority counts: min_refs 3;
        // what a teleport left behind, its object gone, is not asked for more)
        return tex->getDiscardLevel() < 0 ? since <= GRAY_DRAWN_SECONDS
                                          : since <= GRAY_MEMORY_SECONDS && tex->mRhiViewTier != TIER_OFF && tex->getNumRefs() > GRAY_LISTS_REFS;
    }

    // A size in a band: 1 to BAND_SPAN (1 for anything not above 0, NaN too:
    // the fetch and creation orders compare these).
    F32 inBand(F32 size)
    {
        return size > 0.f ? llmin(size, BAND_SPAN) : 1.f;
    }

    // Projector lights use it: lit faces in view may need it whatever its own
    // faces' tier.
    bool lightMap(LLViewerFetchedTexture* tex)
    {
        return tex->getNumVolumes(LLRender::LIGHT_TEX) > 0;
    }

    // T3 while VRAM is short (and no light map): its sharper levels wait.
    bool offWhileShort(LLViewerFetchedTexture* tex)
    {
        return sShort && tierOf(tex) == TIER_OFF && !lightMap(tex);
    }

    // A fetch only the floor asks for (research/303): the texture has an image and its faces need a level
    // coarser than the floor. It goes to the bottom of what is on screen (BAND_VIEW + 1), after the ones with no image.
    bool floorOnly(LLViewerFetchedTexture* tex)
    {
        return sOnScreenCoarsest < MAX_DISCARD_LEVEL && tex->getDiscardLevel() > sOnScreenCoarsest && tex->mRhiViewNeed > sOnScreenCoarsest;
    }

    // The priority of a texture's tier and size (vsize: its virtual size, for
    // T3 and no tier).
    F32 bandPriority(LLViewerFetchedTexture* tex, F32 vsize)
    {
        const F32 scale = tex->getDiscardLevel() < 0 ? NO_IMAGE_SCALE : 1.f;
        switch (tex->mRhiViewTier)
        {
        case TIER_NEAR:
        case TIER_VIEW:
            if (floorOnly(tex))
            {
                return BAND_VIEW + 1.f;     // the bottom of what is on screen (after the far ones with no image)
            }
            return (tex->mRhiViewTier == TIER_NEAR ? BAND_NEAR : BAND_VIEW) + inBand(tex->mRhiViewSize * scale);
        case TIER_OFF:
            return inBand(vsize);
        default:
            // No faces: light maps, boosted ones (terrain...) and those that
            // give their own size (not LOD textures: water maps...) with the
            // rest on screen; the others (unused, waiting to leave the list)
            // by size.
            if (grayDrawn(tex))
            {
                // drawn as gray: on screen, its size not known (as a face scan that gave up: the top of the band), the
                // first level of what has no image at all
                return BAND_VIEW + (tex->getDiscardLevel() < 0 ? BAND_SPAN : inBand(vsize));
            }
            if (tex->getBoostLevel() > LLGLTexture::BOOST_NONE || lightMap(tex) || tex->getType() != LLViewerTexture::LOD_TEXTURE)
            {
                return BAND_VIEW + inBand(vsize);
            }
            return inBand(vsize);
        }
    }

    // The level a texture's faces in view need for texels (as
    // LLViewerLODTexture::processTextureStats at a bias of 1), -1: not known.
    S8 needLevel(LLViewerFetchedTexture* tex, F32 texels)
    {
        const F32 full = (F32)tex->getFullWidth() * (F32)tex->getFullHeight();
        if (full <= 0.f || texels <= 0.f)
        {
            return -1;
        }
        const S32 max_level = tex->getGLTexture() ? tex->getGLTexture()->getMaxDiscardLevel() : MAX_DISCARD_LEVEL;
        const F32 level = floorf(logf(full / texels) / logf(4.f));
        return (S8)llclamp((S32)level, 0, llmax(max_level, 0));
    }

    // From T3 into view (before: its tier at the last scan) to the level its
    // faces need, or its first level when that is not known: counted when it
    // gets there, or as INTO_VIEW_GIVE_UP and over when it does not in time.
    void noteIntoView(LLViewerFetchedTexture* tex, U8 before, F32 time)
    {
        if (!onScreen(tex->mRhiViewTier) || tex->isMissingAsset())
        {
            tex->mRhiViewSince = 0.f;
            return;
        }
        if (before == TIER_OFF && tex->mRhiViewSince <= 0.f)
        {
            tex->mRhiViewSince = time;
        }
        const S32 discard = tex->getDiscardLevel();
        // The level it is waited for at: what its faces need, no coarser than the floor (research/303)
        const S32 wanted = llmin(tex->mRhiViewNeed < 0 ? (S32)MAX_DISCARD_LEVEL : (S32)tex->mRhiViewNeed, sOnScreenCoarsest);
        const bool reached = discard >= 0 && discard <= wanted;
        if (tex->mRhiViewSince > 0.f && (reached || time - tex->mRhiViewSince >= INTO_VIEW_GIVE_UP))
        {
            ++sSums.into_view[waitBucket(time - tex->mRhiViewSince)];
            tex->mRhiViewSince = 0.f;
        }
    }

    std::string waitText(const U32* buckets)
    {
        return llformat("%u/%u/%u/%u", buckets[0], buckets[1], buckets[2], buckets[3]);
    }

    const char* const WHY_NAMES[LLViewerFetchedTexture::PLUS_WHY_COUNT] = {
        "would ask", "decode disabled", "waits to become a GL texture", "flagged missing", "image waits for callbacks", "in the fast cache list",
        "no GL image", "priority 0", "level wanted above the finest level", "waits for a retry", "at the minimum level",
        "that level was asked for", "has the level wanted" };

    // research/302: the gray faces' lines of a window (noteGrayDrawn; reset by logStats)
    std::unordered_set<LLUUID> sGrayDrawnSeen;
    U32 sGrayDrawnLines = 0;
    // research/302: this window's gray-face textures (each counted once), by the image they have (none, level 5,
    // level 4 or finer) and the reason updateFetch asks for nothing
    U32 sGrayByImage[3][LLViewerFetchedTexture::PLUS_WHY_COUNT] = {};
    constexpr U32 GRAY_DRAWN_PER_WINDOW = 12;
    // research/304: the textures a selection finds without an image (D1), and updateFetch's "asks for nothing" for
    // textures with no image by tier and reason (D3), over a window
    U32 sSelectLines = 0;
    std::unordered_set<LLUUID> sSelectSeen;     // told this window (a selection comes every idle)
    std::unordered_set<LLUUID> sGrayWeakNamed;  // named this window with a reason that may change (off screen, retry wait)
    constexpr U32 SELECT_LINES_PER_WINDOW = 8;
    U32 sNoRequest[TIER_COUNT][LLViewerFetchedTexture::PLUS_WHY_COUNT] = {};
    constexpr size_t GRAY_DRAWN_SEEN_MAX = 512;

    void logStats()
    {
        const U32 interval = statsSeconds();
        static LLFrameTimer timer;
        if (!interval || timer.getElapsedTimeF32() < (F32)interval)
        {
            return;
        }
        const F32 seconds = timer.getElapsedTimeF32();
        timer.reset();
        constexpr F64 MB = 1024.0 * 1024.0;
        const Round& r = sLastRound;
        const Sums& s = sSums;
        std::ostringstream out;
        out << llformat("texture view (research/285) over %.0f s [fetch %s, bias split %s (on screen / %.2f, off / %.0f), "
                        "off screen scaled down first %s, floor %.0f px, grace %.0f s]: ",
                        seconds, sFetch ? "on" : "off", sValues.split ? "on" : "off", sValues.on_divisor, sValues.off_divisor,
                        sFree ? "on" : "off", sqrtf(sFloorArea), sValues.grace);
        if (!sRoundDone)
        {
            out << "no full round of the fetch pass yet";
        }
        else
        {
            out << "last round:";
            for (U32 t = TIER_NEAR; t < TIER_COUNT; ++t)
            {
                out << llformat(" %s %u (no image %u, %.0f MB),", TIER_NAMES[t], r.count[t], r.no_image[t], r.bytes[t] / MB);
            }
            out << llformat(" no faces or boosted %u (no image %u); on screen discard %.2f, over need %.2f (max %d); "
                            "off screen discard %.2f",
                            r.count[TIER_NONE], r.no_image[TIER_NONE],
                            r.on_discard_count ? (F64)r.on_discard_sum / r.on_discard_count : 0.0,
                            r.over_need_count ? (F64)r.over_need_sum / r.over_need_count : 0.0, r.over_need_max,
                            r.off_discard_count ? (F64)r.off_discard_sum / r.off_discard_count : 0.0);
            out << "; on screen by the level they have 5/4/3/2/1/0/none: " << r.on_level[5] << "/" << r.on_level[4] << "/" << r.on_level[3]
                << "/" << r.on_level[2] << "/" << r.on_level[1] << "/" << r.on_level[0] << "/" << r.on_level[6]
                << ", at level 5 and needing no better " << r.on_five_needs_five << " (research/302)";
            out << "; flagged as missing assets (a flag no code clears for world textures), near/view/off/other: " << r.missing[TIER_NEAR] << "/"
                << r.missing[TIER_VIEW] << "/" << r.missing[TIER_OFF] << "/" << r.missing[TIER_NONE];
            out << "; no image for " << (U32)GRAY_STUCK_SECONDS << " s or more, by why (research/294):";
            for (U32 reason = 0; reason < GRAY_REASONS; ++reason)
            {
                out << " " << GRAY_NAMES[reason] << " " << r.gray_stuck[reason] << (reason + 1 < GRAY_REASONS ? "," : "");
            }
        }
        out << "; fetches asked for, near/view/off: " << s.requests[TIER_NEAR] << "/" << s.requests[TIER_VIEW] << "/"
            << s.requests[TIER_OFF] << "; waits for data under 1/1-3/3-10/10+ s: near " << waitText(s.waits[TIER_NEAR])
            << ", view " << waitText(s.waits[TIER_VIEW]) << ", off " << waitText(s.waits[TIER_OFF]) << ", other "
            << waitText(s.waits[TIER_NONE]) << "; into view to the level needed " << waitText(s.into_view)
            << "; fetches set to priority 0 (aborted only before the request goes out), off/other: " << s.zeroed[TIER_OFF] << "/"
            << (s.zeroed[TIER_NONE] + s.zeroed[TIER_NEAR] + s.zeroed[TIER_VIEW]) << "; scaled down, near/view/off/other: "
            << s.scale_downs[TIER_NEAR] << "/" << s.scale_downs[TIER_VIEW] << "/" << s.scale_downs[TIER_OFF] << "/"
            << s.scale_downs[TIER_NONE];
        LL_INFOS("TextureMemory") << out.str() << LL_ENDL;
        sSums = Sums();
        flow::logWindow(seconds);   // research/294: where the textures wait, over the same window
        if (!sGrayDrawnSeen.empty())
        {
            // research/302: the window's gray-face textures by the image they have and why nothing is asked for
            std::ostringstream gray;
            gray << "gray face textures this window (research/302), " << sGrayDrawnSeen.size() << (sGrayDrawnSeen.size() >= GRAY_DRAWN_SEEN_MAX ? " or more (counted the first " + std::to_string(GRAY_DRAWN_SEEN_MAX) + ")" : std::string(" in all")) << ", by image:";
            const char* const image_names[3] = { "none", "level 5", "level 4 or finer" };
            for (U32 image = 0; image < 3; ++image)
            {
                gray << " " << image_names[image] << " {";
                bool any = false;
                for (U32 why = 0; why < LLViewerFetchedTexture::PLUS_WHY_COUNT; ++why)
                {
                    if (sGrayByImage[image][why])
                    {
                        gray << (any ? ", " : "") << WHY_NAMES[why] << " " << sGrayByImage[image][why];
                        any = true;
                    }
                }
                gray << (any ? "" : "-") << "},";
            }
            LL_INFOS("TextureMemory") << gray.str() << LL_ENDL;
        }
        {
            // research/304 (D3): updateFetch asked for nothing for textures with no image, by tier and reason (visits)
            std::ostringstream none;
            bool any_tier = false;
            for (U32 tier = 0; tier < TIER_COUNT; ++tier)
            {
                bool any = false;
                for (U32 why = 0; why < LLViewerFetchedTexture::PLUS_WHY_COUNT; ++why)
                {
                    if (sNoRequest[tier][why])
                    {
                        none << (any_tier && !any ? "; " : any ? ", " : "") << (any ? "" : std::string(tier == TIER_NONE ? "no faces or boosted" : TIER_NAMES[tier]) + " {") << WHY_NAMES[why] << " "
                             << sNoRequest[tier][why];
                        any = true;
                    }
                }
                none << (any ? "}" : "");
                any_tier = any_tier || any;
            }
            if (any_tier)
            {
                LL_INFOS("TextureMemory") << "no request for textures with no image this window, in visits (research/304): " << none.str() << LL_ENDL;
            }
            std::memset(sNoRequest, 0, sizeof(sNoRequest));
            sSelectLines = 0;
            sSelectSeen.clear();
            sGrayWeakNamed.clear();
        }
        sGrayDrawnSeen.clear();     // research/302: the gray faces' lines start again
        sGrayDrawnLines = 0;
        std::memset(sGrayByImage, 0, sizeof(sGrayByImage));
        fetchretry::logWindow(seconds);     // research/299: the retries of failed fetches, over the same window
    }

    // The sized pools' blocks less their allocations (LastResort::slack). Not
    // known without the backend's counts or with no sized pools
    // (LLRHI_VK_POOLS=0): then taken as small. The free room of VMA's default
    // blocks is not counted (research/285 X0 reports it as text only).
    void checkSlack(LastResort& l)
    {
        LLRHI::BufferCompactCounts counts;
        if (LLRHI::sBufferCompactCounts)
        {
            LLRHI::sBufferCompactCounts(counts);
        }
        l.slack_known = counts.gl_pool_blocks + counts.texture_pool_blocks > 0;
        l.slack = (counts.gl_pool_blocks - llmin(counts.gl_pool_used, counts.gl_pool_blocks))
                  + (counts.texture_pool_blocks - llmin(counts.texture_pool_used, counts.texture_pool_blocks));
        l.slack_small = !l.slack_known || l.slack < LAST_RESORT_SLACK;
    }

    // The divisor on screen (LastResort). fill: the textures against the
    // limit (0: not known).
    void updateLastResort(F32 fill)
    {
        static const bool allowed = LLRHI::envDefaultOn("LLRHI_TEX_VIEW_LAST_RESORT");
        LastResort& l = sLastResort;
        const F32 time = now();
        const F32 step = llclamp((F32)gFrameIntervalSeconds, 0.f, 0.25f) * (LAST_RESORT_MAX - 1.f);
        F32 divisor = sValues.on_divisor;
        if (!sValues.split || !allowed)
        {
            divisor = 1.f;
            l.over_since = 0.f;
            l.coming_down = false;
        }
        else if (fill > 1.f)
        {
            l.coming_down = false;
            if (l.over_since <= 0.f)
            {
                l.over_since = time;
            }
            if (time - l.slack_checked >= SLACK_CHECK_SECONDS)
            {
                l.slack_checked = time;
                checkSlack(l);
            }
            const Round& r = sLastRound;
            const U64 on_bytes = r.bytes[TIER_NEAR] + r.bytes[TIER_VIEW];
            const bool off_down = sRoundDone && on_bytes > 0 && (F64)r.bytes[TIER_OFF] < LAST_RESORT_OFF_SHARE * (F64)on_bytes;
            const bool may_start = divisor > 1.f || l.ended_at <= 0.f || time - l.ended_at >= l.cooldown;
            if (time - l.over_since >= LAST_RESORT_WAIT && off_down && l.slack_small && may_start)
            {
                if (divisor <= 1.f)     // a new start: the wait after it doubles when it came again soon
                {
                    l.cooldown = l.ended_at > 0.f && time - l.ended_at < 2.f * l.cooldown
                                     ? llmin(2.f * l.cooldown, LAST_RESORT_COOLDOWN_MAX) : LAST_RESORT_COOLDOWN;
                }
                divisor = llmin(divisor + step / LAST_RESORT_RAMP_UP, LAST_RESORT_MAX);
            }
        }
        else if (fill <= 0.f)
        {
            l.over_since = 0.f;     // not known (no driver budget, or held around a snapshot): held as it is
        }
        else
        {
            l.over_since = 0.f;
            if (fill < LAST_RESORT_DOWN_BELOW)
            {
                l.coming_down = true;
            }
            else if (fill > LAST_RESORT_DOWN_UNTIL)
            {
                l.coming_down = false;  // held from here
            }
            if (l.coming_down)
            {
                divisor = llmax(divisor - step / LAST_RESORT_RAMP_DOWN, 1.f);
            }
        }
        if (divisor > 1.f && sValues.on_divisor <= 1.f)
        {
            LL_INFOS("TextureMemory") << "textures on screen (research/285): the last resort begins, lowered up to one level to fit "
                                         "the budget; textures at " << llformat("%.0f%%", fill * 100.f) << " of the limit, sized pools' slack "
                                      << (l.slack_known ? llformat("%.0f MB", l.slack / (1024.0 * 1024.0)) : std::string("not known"))
                                      << ", discard bias " << LLViewerTexture::sDesiredDiscardBias << ", texture pressure "
                                      << LLViewerTexture::sRhiTexPressure << ", next start no sooner than "
                                      << llformat("%.0f s", l.cooldown) << " after it ends" << LL_ENDL;
        }
        else if (divisor <= 1.f && sValues.on_divisor > 1.f)
        {
            l.ended_at = time;
            l.coming_down = false;
            LL_INFOS("TextureMemory") << "textures on screen (research/285): the last resort ends, back to what the screen needs; "
                                         "textures at " << llformat("%.0f%%", fill * 100.f) << " of the limit" << LL_ENDL;
        }
        sValues.on_divisor = divisor;
    }
}

const ScanValues& scanValues()
{
    return sValues;
}

bool grayDrawnNow(const LLViewerFetchedTexture* tex)
{
    return grayDrawn(tex);
}

bool cacheMissSkipOn()
{
    static const bool on = LLRHI::envDefaultOn("LLRHI_TEX_CACHE_MISS_SKIP");
    return on;
}

bool fetchOn()
{
    return sFetch;
}

U32 statsSeconds()
{
    // LLRHI_TEX_VIEW_STATS: every so many seconds (60 when unset, 0: never; research/294's counters stop with it).
    static const U32 seconds = []()
    {
        U64 value = 60;
        LLRHI::envNumber("LLRHI_TEX_VIEW_STATS", 0, 3600, value);
        return (U32)value;
    }();
    return seconds;
}

U8 tierOfPriority(F32 priority)
{
    return priority >= BAND_NEAR ? TIER_NEAR : priority >= BAND_VIEW ? TIER_VIEW : TIER_OFF;
}

void addFace(FaceView& view, const FaceInput& face)
{
    // The face's value first: llmax keeps the second when the first is NaN (a broken bounding box).
    view.in_view_time = llmax(face.in_view_time, view.in_view_time);
    // research/296 (diagnostics): a NaN near side counts as 0 (never far)
    const F32 near_side = face.near_side >= 0.f ? face.near_side : 0.f;
    view.would_grace = view.would_grace || face.would_grace;
    view.has_moving = view.has_moving || face.moving;
    view.has_hud = view.has_hud || face.hud;
    view.all_far = view.all_far && near_side >= face.far_cut;
    view.in_view_far = view.in_view_far || (face.in_view && near_side >= face.far_cut);
    if (!face.moving && !face.hud)
    {
        view.min_near_static = llmin(near_side, view.min_near_static);
    }
    if (!face.in_grace)
    {
        return;
    }
    const bool shown = face.in_view && face.drawn;
    view.in_grace = true;
    view.close_by = view.close_by || (shown && face.near_side <= NEAR_METERS);
    view.need_size = llmax(face.unbiased, view.need_size);
    view.view_size = llmax(shown ? face.unbiased : face.unbiased * HIDDEN_SHARE, view.view_size);
}

// research/296 (diagnostics): whether a priority 0 for a texture of the old region could take it at all, apart from where
// its faces are: an unboosted LOD texture nothing else waits for. Cheap conditions first, the twin lookup last.
static bool farEligible(LLViewerFetchedTexture* tex)
{
    if (tex->getType() != LLViewerTexture::LOD_TEXTURE || tex->getBoostLevel() != LLGLTexture::BOOST_NONE
        || tex->getNumVolumes(LLRender::LIGHT_TEX) > 0 || tex->getNumVolumes(LLRender::SCULPT_TEX) > 0
        || tex->rhiForceToSaveRawImage() || !tex->getUseDiscard() || tex->needsCreateTexture() || tex->getRawImage()
        || tex->isInFastCacheList() || tex->isMissingAsset() || tex->hasCallbacks())
    {
        return false;
    }
    return gTextureList.findImage(tex->getID(), TEX_LIST_SCALE) == nullptr;     // a thumbnail twin shares the fetch
}

// ... and whether its faces are all static and far past the draw distance with the grace cut by distance (what a
// priority 0 for the old region's textures would take). Reads the last verdict (mRhiFarFlags) for the hysteresis.
static bool farCandidate(LLViewerFetchedTexture* tex, const FaceView& view, U32 face_count)
{
    const F32 cut = sValues.far_cut;
    if (cut >= F32_MAX || tex->mRhiWouldTier != TIER_OFF || !view.all_scanned || face_count == 0 || view.has_moving || view.has_hud)
    {
        return false;
    }
    const F32 threshold = (tex->mRhiFarFlags & FAR_CANDIDATE) ? cut - FAR_HYSTERESIS_METERS : cut;
    return view.min_near_static >= threshold && farEligible(tex);
}

void onFaceScan(LLViewerFetchedTexture* tex, const FaceView& view, U32 face_count, bool unknown_faces, F32 nearest,
                F32 need_cap)
{
    const F32 time = now();
    const U8 before = tex->mRhiViewTier;
    if (unknown_faces)
    {
        tex->mRhiViewTier = nearest <= NEAR_METERS ? TIER_NEAR : TIER_VIEW;
        tex->mRhiViewSize = BAND_SPAN;
        tex->mRhiInViewTime = time;
        tex->mRhiViewNeed = -1;
    }
    else
    {
        tex->mRhiViewTier = view.close_by ? TIER_NEAR : view.in_grace ? TIER_VIEW : face_count > 0 ? TIER_OFF : TIER_NONE;
        tex->mRhiViewSize = view.view_size;
        tex->mRhiInViewTime = view.in_view_time;
        tex->mRhiViewNeed = onScreen(tex->mRhiViewTier) && view.need_size > 0.f ? needLevel(tex, llmin(view.need_size, need_cap))
                                                                                  : (S8)-1;
    }
    noteIntoView(tex, before, time);

    // research/296 (diagnostics): the tier the grace cut by distance would give, and the far flags. The last verdict's
    // candidate bit is read by farCandidate (hysteresis) before the flags are written again.
    tex->mRhiWouldTier = unknown_faces      ? tex->mRhiViewTier
                         : view.would_grace ? (view.close_by ? (U8)TIER_NEAR : (U8)TIER_VIEW)
                         : face_count > 0   ? (U8)TIER_OFF
                                            : (U8)TIER_NONE;
    U8 flags = 0;
    if (sFarDiag)
    {
        if (farCandidate(tex, view, face_count))
        {
            flags |= FAR_CANDIDATE;
        }
        if (!unknown_faces && view.all_scanned && face_count > 0 && view.has_moving && !view.has_hud && view.all_far
            && sValues.far_cut < F32_MAX && farEligible(tex))
        {
            flags |= FAR_MOVING;
        }
        if (view.in_view_far && sValues.far_cut < F32_MAX)
        {
            flags |= FAR_IN_VIEW_FAR;
        }
    }
    tex->mRhiFarFlags = flags;
}

F32 floorSize(LLViewerFetchedTexture* tex, F32 vsize, F32 nearest)
{
    if (!sFetch || sFloorArea <= 0.f || tex->isMissingAsset())
    {
        return vsize;
    }
    // (research/304: drawn as gray lately: whatever the face scan says, it is on screen: it asks for its first level,
    // and for the floor's level when its first bytes came, coarser than that)
    const bool gray = grayDrawn(tex);
    const S32 level = tex->getDiscardLevel();
    if (level >= 0 && !(gray && level > sOnScreenCoarsest))
    {
        return vsize;
    }
    if (!gray && (tex->mRhiViewTier == TIER_NONE || nearest > sFarClip))
    {
        return vsize;
    }
    return llmax(vsize, sFloorArea);
}

F32 fetchPriority(LLViewerFetchedTexture* tex, F32 priority)
{
    if (!sFetch || priority <= 0.f)
    {
        return priority;
    }
    if (tex->getBoostLevel() >= LLGLTexture::BOOST_HIGH)
    {
        return BAND_NEAR + inBand(priority);    // UI, HUD, selected, sculpt maps: with the near ones
    }
    if (offWhileShort(tex))
    {
        return tex->getDiscardLevel() < 0 ? OFF_FIRST_LEVEL : 0.f;
    }
    return bandPriority(tex, priority);
}

F32 holdSeconds(LLViewerFetchedTexture* tex, F32 hold)
{
    return sFetch && offWhileShort(tex) ? OFF_HOLD_SECONDS : hold;
}

F32 createOrder(LLViewerFetchedTexture* tex)
{
    return sFetch ? bandPriority(tex, tex->getMaxVirtualSize()) : 0.f;
}

bool scaleDownFirst(LLViewerFetchedTexture* tex)
{
    return sFree && tierOf(tex) == TIER_OFF;
}

void updateClass(F32 fill, bool bias_all)
{
    static LLCachedControl<bool> setting(gSavedSettings, "PlusTexViewPriority", true);
    static LLCachedControl<U32> floor_px(gSavedSettings, "PlusTexFarFloorPx", 8);
    static const bool env_fetch = LLRHI::envDefaultOn("LLRHI_TEX_VIEW_FETCH");
    static const bool env_bias = LLRHI::envDefaultOn("LLRHI_TEX_VIEW_BIAS");
    static const bool env_free = LLRHI::envDefaultOn("LLRHI_TEX_VIEW_FREE");
    static LLRHI::CuePoll cue_poll(1.0);
    if (cue_poll.due() && LLRHI::takeCue("texview.toggle"))
    {
        sCueOn = !sCueOn;
        LL_INFOS("TextureMemory") << "texview.toggle: textures on screen first (research/285) " << (sCueOn ? "on" : "off") << LL_ENDL;
    }
    const bool on = setting() && sCueOn;
    sFetch = on && env_fetch;
    sFree = on && env_free;
    const U32 px = llmin((U32)floor_px(), FLOOR_PX_MAX);
    sFloorArea = (F32)(px * px);
    static const bool env_gray = LLRHI::envDefaultOn("LLRHI_TEX_GRAY_FLOOR");
    sGrayFloor = on && env_fetch && env_gray;
    sFarClip = LLPipeline::sUseFarClip ? LLViewerCamera::getInstance()->getFar() : F32_MAX;

    const S32 pressure = LLViewerTexture::sRhiTexPressure;
    const F32 bias = llclamp(LLViewerTexture::sDesiredDiscardBias, 1.f, 4.f);
    sShort = pressure >= 1 || bias >= SHORT_BIAS;
    sValues.grace = pressure >= 2 ? GRACE_STAGE_2 : pressure == 1 ? GRACE_STAGE_1 : GRACE_ROOM;
    // A bias up for another reason than VRAM (bias_all) lowers what is on screen too, as upstream.
    sValues.split = on && env_bias && !bias_all;
    sSplitAvailable = on && env_bias;
    sValues.off_divisor = (F32)llroundf(powf(4.f, bias - 1.f)) * (pressure >= 2 ? OFF_STAGE_2_SCALE : 1.f);
    updateLastResort(fill);

    // research/303: what is on screen does not settle for the first 600 bytes (level 5). It gives way only when
    // the VRAM budget really lowers what is on screen: the last resort from a divisor of 2 (research/285
    // section 2.3), or a bias that is up for another reason than VRAM (bias_all: system memory, memguard,
    // minimised, the user's own VRAM budget) at 2 and up (where upstream starts to lower it). (Upstream's isUpdateFrozen() is never true.)
    static LLCachedControl<U32> coarsest(gSavedSettings, "PlusTexOnScreenCoarsest", 4);
    static const bool env_coarsest = LLRHI::envDefaultOn("LLRHI_TEX_ONSCREEN_COARSEST");
    const U32 wanted = coarsest();
    // (upstream lowers what is on screen from a bias of 2: below it the faces in view keep their size)
    const bool memory_bias = bias_all && bias >= 2.f;
    sOnScreenCoarsest = (on && env_coarsest && wanted > 0 && wanted < (U32)MAX_DISCARD_LEVEL && sValues.on_divisor < 2.f && !memory_bias)
                            ? (S32)llmax(wanted, FLOOR_LEVEL_MIN)
                            : (S32)MAX_DISCARD_LEVEL;

    // research/296 (diagnostics): the far cut, only while a TPLag timeline is open
    sFarDiag = plus::tplag::timelineOpen();
    if (sFarDiag && LLPipeline::sUseFarClip && !plus::presets::textureBudgetHeld())
    {
        const LLFace::PixelAreaCamera camera = LLFace::PixelAreaCamera::current();
        sValues.far_cut = camera.far_clip / llmax(camera.view_cos, 0.2f) + FAR_MARGIN_METERS;
    }
    else
    {
        sValues.far_cut = F32_MAX;
    }

    logStats();
}

S32 coarsestWanted(LLViewerFetchedTexture* tex)
{
    if (sOnScreenCoarsest >= MAX_DISCARD_LEVEL)
    {
        return MAX_DISCARD_LEVEL;
    }
    // Drawn as gray lately: on screen whatever the face scan says (its nearest face may be past the draw distance)
    // (research/304)
    if (grayDrawn(tex))
    {
        return sOnScreenCoarsest;
    }
    // A texture with no face the scan knows (not boosted: tier none, nearest face at the far end) is not past the draw
    // distance (research/304: with the gray floor on; off, as before)
    const bool no_faces = sGrayFloor && tex->mRhiViewTier == TIER_NONE && tex->getBoostLevel() < LLGLTexture::BOOST_HIGH;
    if (!no_faces && tex->mNearestFaceDistance > sFarClip)
    {
        return MAX_DISCARD_LEVEL;   // past the draw distance (an old region after a teleport)
    }
    if (onScreen(tierOf(tex)))
    {
        return sOnScreenCoarsest;
    }
    // Off screen: what it has at the floor's level stays (no new request: a level it already has is not asked
    // for again), so turning the camera back does not fetch it again. Not while VRAM is short.
    return !sShort && tex->getDiscardLevel() == sOnScreenCoarsest ? sOnScreenCoarsest : (S32)MAX_DISCARD_LEVEL;
}

std::string farDiagLine()
{
    if (!sFarDiag || !sRoundDone)
    {
        return std::string();
    }
    const Round& r = sLastRound;
    if (sValues.far_cut >= F32_MAX)
    {
        return "far-grace diag: not counted (draw distance off or a snapshot holds the budget)";
    }
    if (r.far_incomplete)
    {
        return "far-grace diag: not counted yet (the last full round began before the timeline opened)";
    }
    return llformat("far-grace diag (cut at %.0f m, the last full round; MB estimated from GL sizes): the grace cut by distance would put %u textures (%.0f MB) in T3; "
                    "far candidates %u (%.0f MB, %u fetching); held back by a moving face %u (%.0f MB); %u textures with a face in the cone past the cut",
                    sValues.far_cut, r.cut_n, (F32)r.cut_bytes / (1024.f * 1024.f), r.cand_n, (F32)r.cand_bytes / (1024.f * 1024.f),
                    r.cand_fetching, r.moving_n, (F32)r.moving_bytes / (1024.f * 1024.f), r.in_view_far);
}

// research/294: a texture with no image for GRAY_STUCK_SECONDS, at its visit (after the visit's size and fetch)
static void noteGray(LLViewerFetchedTexture* tex)
{
    const F64 time = LLTimer::getTotalSeconds();
    if (tex->mRhiGraySince <= 0.0)
    {
        tex->mRhiGraySince = time;
        return;
    }
    if (time - tex->mRhiGraySince < GRAY_STUCK_SECONDS)
    {
        return;
    }
    const U8 view_tier = tex->mRhiViewTier;
    const S32 faces = tex->getTotalNumFaces();
    const bool fetching = tex->hasFetcher();
    const F32 size = tex->getMaxVirtualSize();
    const GrayReason reason = tex->getBoostLevel() >= LLGLTexture::BOOST_HIGH ? GRAY_BOOSTED
                              : faces == 0                                    ? GRAY_NO_FACES
                              : !onScreen(view_tier)                          ? GRAY_OFF_SCREEN
                              : fetching                                      ? GRAY_FETCHING
                              : fetchretry::waiting(tex)                      ? GRAY_RETRY_WAIT
                              : size <= 10.f                                  ? GRAY_NO_SIZE
                                                                              : GRAY_NOT_ASKED;
    ++sRound.gray_stuck[reason];
    // (research/304: off screen and a retry that waits are named too, a few of each reason)
    const bool names_cause = reason != GRAY_BOOSTED && reason != GRAY_FETCHING;
    const bool weak = reason == GRAY_OFF_SCREEN || reason == GRAY_RETRY_WAIT;   // may change into the reason that matters
    if (names_cause && !tex->mRhiGrayNamed && sSums.gray_named[reason] < GRAY_NAMED_PER_WINDOW &&
        (!weak || sGrayWeakNamed.insert(tex->getID()).second))
    {
        // (a weak reason names a texture once a window, and does not use up its flag: named again when the reason changes)
        tex->mRhiGrayNamed = !weak;
        ++sSums.gray_named[reason];
        LL_INFOS("TextureMemory") << "texture with no image for " << llformat("%.0f", time - tex->mRhiGraySince) << " s (research/294): "
                                  << tex->getID() << " " << GRAY_NAMES[reason] << ", tier " << tierName(view_tier)
                                  << ", faces " << faces << " (light maps " << tex->getNumVolumes(LLRender::LIGHT_TEX) << ", sculpts "
                                  << tex->getNumVolumes(LLRender::SCULPT_TEX) << "), nearest face " << llformat("%.1f", tex->mNearestFaceDistance)
                                  << " m, virtual size " << llformat("%.0f", size) << ", desired level " << tex->getDesiredDiscardLevel()
                                  << ", boost " << (S32)tex->getBoostLevel() << ", type " << (S32)tex->getType()
                                  << ", asked " << (tex->mRhiRequestTime > 0.f ? "yes" : "no") << ", missing "
                                  << (tex->isMissingAsset() ? "yes" : "no") << LL_ENDL;
    }
}


void noteGrayDrawn(LLViewerFetchedTexture* tex, const char* cause, const LLDrawInfo* params)
{
    if (statsSeconds() == 0 || sGrayDrawnSeen.size() >= GRAY_DRAWN_SEEN_MAX || !sGrayDrawnSeen.insert(tex->getID()).second)
    {
        return;
    }
    LLViewerFetchedTexture::PlusAskState state;
    tex->plusAskState(state);
    const S32 level = tex->getDiscardLevel();
    ++sGrayByImage[level < 0 ? 0 : level >= 5 ? 1 : 2][state.why];
    if (sGrayDrawnLines >= GRAY_DRAWN_PER_WINDOW)
    {
        return;
    }
    ++sGrayDrawnLines;
    LL_INFOS("TextureMemory") << "gray face texture (research/302): " << tex->getID() << ", " << cause << ", updateFetch asks for nothing because: "
                              << WHY_NAMES[state.why] << ", image " << (tex->getDiscardLevel() < 0 ? std::string("none") : "level " + std::to_string(tex->getDiscardLevel()))
                              << " " << tex->getWidth() << "x" << tex->getHeight() << " of " << tex->getFullWidth() << "x" << tex->getFullHeight()
                              << ", level wanted " << state.desired << ", asked " << state.requested << ", has " << state.current << ", minimum "
                              << state.min_level << ", finest " << state.max_level << ", priority " << llformat("%.0f", state.priority)
                              << ", virtual size " << llformat("%.0f", state.virtual_size) << ", boost " << (S32)tex->getBoostLevel() << ", tier "
                              << tierName(tex->mRhiViewTier) << ", faces " << tex->getTotalNumFaces() << ", nearest face "
                              << llformat("%.1f", tex->mNearestFaceDistance) << " m, fetcher " << (state.has_fetcher ? "yes" : "no")
                              << ", fetching " << (state.fetching ? "yes" : "no") << ", waits to create " << (state.needs_create ? "yes" : "no")
                              << ", " << fetchretry::describe(tex)
                              << ", faces listed by it (diffuse " << tex->getNumFaces(LLRender::DIFFUSE_MAP) << ", base colour "
                              << tex->getNumFaces(LLRender::BASECOLOR_MAP) << "), the batch draws it as "
                              << (!params ? "?" : params->mTexture.get() == tex ? "its texture"
                                 : params->mGLTFMaterial.notNull() && params->mGLTFMaterial->mBaseColorTexture.get() == tex ? "its base colour"
                                                                                                                            : "another of its textures")
                              << LL_ENDL;
}

void noteNoRequest(LLViewerFetchedTexture* tex, int why)
{
    if (statsSeconds() != 0 && why >= 0 && why < (int)LLViewerFetchedTexture::PLUS_WHY_COUNT)
    {
        ++sNoRequest[tierOf(tex)][why];
    }
}

namespace
{
    // A batch the group still holds (a face's mDrawInfo is a raw pointer: it can name a batch cleared with the
    // group's draw map when the rebuild skipped the face). Only compared, never followed, before this says yes.
    bool batchAlive(LLSpatialGroup* group, const LLDrawInfo* info)
    {
        if (!group || !info)
        {
            return false;
        }
        for (const auto& element : group->mDrawMap)
        {
            for (const LLPointer<LLDrawInfo>& batch : element.second)
            {
                if (batch.get() == info)
                {
                    return true;
                }
            }
        }
        return false;
    }

    // One texture's line for a selected object's face with no image (D1).
    void selectedLine(LLViewerObject* object, S32 te, LLFace* face, LLDrawable* drawable, LLViewerFetchedTexture* tex, U32 channel)
    {
        LLViewerFetchedTexture::PlusAskState state;
        tex->plusAskState(state);
        auto listed = [&](U32 ch)
        {
            if (!face)
            {
                return false;
            }
            // (only the first getNumFaces() places of a face list are valid: a removed face's pointer stays after them)
            const LLViewerFetchedTexture::ll_face_list_t* faces = tex->getFaceList(ch);
            const size_t valid = std::min<size_t>((size_t)llmax(tex->getNumFaces(ch), 0), faces->size());
            return std::find(faces->begin(), faces->begin() + valid, face) != faces->begin() + valid;
        };
        LLSpatialGroup* group = drawable ? drawable->getSpatialGroup() : nullptr;
        const LLDrawInfo* info = face ? face->mDrawInfo : nullptr;
        const bool alive = batchAlive(group, info);
        std::string batch = !info ? "none" : !alive ? "stale (not in its group's draw map)" : "yes";
        if (alive)
        {
            batch += std::string(" (draws it as texture ") + (info->mTexture.get() == tex ? "yes" : "no") + ", as base colour " +
                     (info->mGLTFMaterial.notNull() && info->mGLTFMaterial->mBaseColorTexture.get() == tex ? "yes" : "no") + ")";
        }
        LL_INFOS("TextureMemory") << "selected texture with no image (research/304): " << tex->getID() << ", as the "
                                  << (channel == LLRender::BASECOLOR_MAP ? "base colour" : "diffuse") << " texture, type " << (S32)tex->getType()
                                  << ", TE " << te << " of " << (S32)object->getNumTEs() << ", object faces " << (drawable ? drawable->getNumFaces() : -1)
                                  << ", " << (object->getRenderMaterialID((U8)te).notNull() ? "PBR" : "not PBR") << ", face " << (face ? "yes" : "no")
                                  << ", face texture is it (diffuse " << (face && face->getTexture(LLRender::DIFFUSE_MAP) == tex ? "yes" : "no")
                                  << ", base colour " << (face && face->getTexture(LLRender::BASECOLOR_MAP) == tex ? "yes" : "no")
                                  << "), face listed by it (diffuse " << (listed(LLRender::DIFFUSE_MAP) ? "yes" : "no") << ", base colour "
                                  << (listed(LLRender::BASECOLOR_MAP) ? "yes" : "no") << "), faces it knows " << tex->getTotalNumFaces()
                                  << ", batch " << batch << ", group geometry dirty "
                                  << (group && (group->getState() & LLSpatialGroup::GEOM_DIRTY) ? "yes" : "no")
                                  << ", updateFetch asks for nothing because: " << WHY_NAMES[state.why] << ", level wanted " << state.desired
                                  << ", asked " << state.requested << ", has " << state.current << ", finest " << state.max_level << ", priority "
                                  << llformat("%.0f", state.priority) << ", virtual size " << llformat("%.0f", state.virtual_size) << ", boost "
                                  << (S32)tex->getBoostLevel() << ", tier " << tierName(tex->mRhiViewTier) << ", fetcher "
                                  << (state.has_fetcher ? "yes" : "no") << ", " << fetchretry::describe(tex) << LL_ENDL;
    }
}

void noteSelectedNoImage(LLViewerObject* object, S32 te)
{
    if (statsSeconds() == 0 || !object || te < 0 || sSelectLines >= SELECT_LINES_PER_WINDOW)
    {
        return;
    }
    LLDrawable* drawable = object->mDrawable.get();
    LLFace* face = nullptr;
    if (drawable && te < drawable->getNumFaces())
    {
        face = drawable->getFaces()[te];    // (null: no face for this TE yet)
    }
    // the diffuse texture of the TE, and a PBR face's base colour (its diffuse is blank then)
    // (a selection comes every idle for every TE: only a texture with no image is looked at further, no cast before)
    LLViewerTexture* diffuse_any = object->getTEImage((U8)te);
    LLViewerTexture* base_any = face ? face->getTexture(LLRender::BASECOLOR_MAP) : nullptr;
    LLViewerFetchedTexture* diffuse = diffuse_any && diffuse_any->getDiscardLevel() < 0 ? dynamic_cast<LLViewerFetchedTexture*>(diffuse_any) : nullptr;
    LLViewerFetchedTexture* base_colour = base_any && base_any->getDiscardLevel() < 0 ? dynamic_cast<LLViewerFetchedTexture*>(base_any) : nullptr;
    LLViewerFetchedTexture* const candidates[2] = { diffuse, base_colour };
    for (U32 i = 0; i < 2; ++i)
    {
        LLViewerFetchedTexture* tex = candidates[i];
        if (!tex || tex->getDiscardLevel() >= 0 || (i == 1 && tex == diffuse) || sSelectLines >= SELECT_LINES_PER_WINDOW ||
            sSelectSeen.count(tex->getID()))
        {
            continue;   // (a selection calls this every idle: each texture is told once a window, as found before the boost)
        }
        sSelectSeen.insert(tex->getID());
        ++sSelectLines;
        selectedLine(object, te, face, drawable, tex, i == 0 ? LLRender::DIFFUSE_MAP : LLRender::BASECOLOR_MAP);
    }
}

bool splitAvailable()
{
    return sSplitAvailable;
}

void noteVisit(LLViewerFetchedTexture* tex)
{
    const U8 tier = tierOf(tex);
    Round& r = sRound;
    ++r.count[tier];
    const S32 discard = tex->getDiscardLevel();
    if (!sFarDiag)
    {
        r.far_incomplete = true;    // this round's counts miss the visits made while the pass was off
    }
    else
    {
        // research/296 (diagnostics): by the verdicts the last scan wrote
        const U8 flags = tex->getBoostLevel() >= LLGLTexture::BOOST_HIGH ? 0 : tex->mRhiFarFlags;   // (not scanned: the verdict is old)
        const bool cut = tier != TIER_NONE && tier != TIER_OFF && tex->mRhiWouldTier == TIER_OFF;
        if (cut || flags)
        {
            const U64 bytes = discard >= 0 ? estimatedImageBytes(tex->getGLTexture()) : 0;
            if (cut)
            {
                ++r.cut_n;
                r.cut_bytes += bytes;
            }
            if (flags & FAR_CANDIDATE)
            {
                ++r.cand_n;
                r.cand_bytes += bytes;
                r.cand_fetching += tex->hasFetcher() ? 1 : 0;
            }
            if (flags & FAR_MOVING)
            {
                ++r.moving_n;
                r.moving_bytes += bytes;
            }
            r.in_view_far += (flags & FAR_IN_VIEW_FAR) ? 1 : 0;
        }
    }
    if (discard < 0 && tex->isMissingAsset() && statsSeconds() > 0)
    {
        // research/294 section 11: a fetch that ended with no data flags the texture missing for good
        ++r.missing[tier];
        if (!tex->mRhiMissingNamed && sSums.missing_named < MISSING_NAMED_PER_WINDOW)
        {
            tex->mRhiMissingNamed = true;
            ++sSums.missing_named;
            LL_INFOS("TextureMemory") << "texture flagged missing (research/294): " << tex->getID() << ", tier " << tierName(tier)
                                      << ", faces " << tex->getTotalNumFaces() << ", last HTTP status " << tex->lastHttpGetStatus().toHex()
                                      << ", type " << (S32)tex->getType() << ", boost "
                                      << (S32)tex->getBoostLevel() << LL_ENDL;
        }
    }
    if (!tex->isMissingAsset())
    {
        tex->mRhiMissingNamed = false;  // (the flag is cleared for avatar bakes and model previews only)
    }
    if (discard >= 0 || statsSeconds() == 0)
    {
        tex->mRhiGraySince = 0.0;
        tex->mRhiGrayNamed = false;
        tex->mRhiMissingNamed = false;
    }
    else if (tex->isMissingAsset())
    {
        tex->mRhiGraySince = 0.0;   // (named once as missing above, by mRhiMissingNamed; the gray naming starts over if cleared)
    }
    else
    {
        noteGray(tex);
    }
    if (onScreen(tier))
    {
        ++r.on_level[discard < 0 ? 6 : llmin(discard, 5)];
        r.on_five_needs_five += (discard == 5 && tex->mRhiViewNeed == 5) ? 1 : 0;
    }
    if (discard < 0)
    {
        r.no_image[tier] += tex->isMissingAsset() ? 0 : 1;
        return;
    }
    r.bytes[tier] += estimatedImageBytes(tex->getGLTexture());
    if (onScreen(tier))
    {
        r.on_discard_sum += discard;
        ++r.on_discard_count;
        if (tex->mRhiViewNeed >= 0)
        {
            const S32 over = llmax(discard - (S32)tex->mRhiViewNeed, 0);
            r.over_need_sum += over;
            ++r.over_need_count;
            r.over_need_max = llmax(r.over_need_max, over);
        }
    }
    else if (tier == TIER_OFF)
    {
        r.off_discard_sum += discard;
        ++r.off_discard_count;
    }
}

void endRound()
{
    sLastRound = sRound;
    sRound = Round();
    sRoundDone = true;
}

void noteRequest(LLViewerFetchedTexture* tex)
{
    const U8 tier = tierOf(tex);
    ++sSums.requests[tier];
    tex->mRhiRequestTime = now();
    tex->mRhiRequestTier = tier;
    tex->mRhiFetchLetGo = false;
}

void noteFetchDone(LLViewerFetchedTexture* tex, bool got_data)
{
    if (tex->mRhiRequestTime <= 0.f)
    {
        return;
    }
    if (got_data)
    {
        ++sSums.waits[llmin(tex->mRhiRequestTier, (U8)(TIER_COUNT - 1))][waitBucket(now() - tex->mRhiRequestTime)];
    }
    tex->mRhiRequestTime = 0.f;
}

void noteZeroPriority(LLViewerFetchedTexture* tex)
{
    if (!tex->mRhiFetchLetGo)
    {
        tex->mRhiFetchLetGo = true;
        ++sSums.zeroed[tierOf(tex)];
    }
}

void noteScaleDown(LLViewerFetchedTexture* tex)
{
    ++sSums.scale_downs[tierOf(tex)];
}

const char* tierName(U8 tier)
{
    return TIER_NAMES[llmin(tier, (U8)(TIER_COUNT - 1))];
}

U64 estimatedImageBytes(const LLImageGL* gl)
{
    if (!gl || !gl->getHasGLTexture())
    {
        return 0;
    }
    const U64 texels = (U64)gl->getWidth() * (U64)gl->getHeight();
    const S32 components = gl->getComponents();
    const U64 bpp = components == 3 ? 4 : (U64)llclamp(components, 1, 4);
    return texels * bpp * 4 / 3;    // the backend gives every image a full chain
}
}

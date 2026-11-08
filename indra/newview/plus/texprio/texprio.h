/**
 * @file texprio.h
 * @brief Textures on screen first (research/285 tex-view-priority): the
 *        view tiers of the fetch pass's textures, their fetch and creation
 *        order, the floor for on-screen textures with no image yet, and the
 *        counters of the TextureMemory log.
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

#ifndef PLUS_TEXPRIO_H
#define PLUS_TEXPRIO_H

#include "lldefs.h"
#include "stdtypes.h"

#include <string>

class LLImageGL;
class LLViewerFetchedTexture;
class LLViewerObject;
class LLDrawInfo;

// research/285 in the sl-viewer repository (decided 2026-11-06: "what is on
// screen loads first: near on screen > the rest on screen >>>>> what went off
// screen"). A face is in view when its bounding sphere touches the view cone
// with a 10 degree margin (LLFace::inViewCone, worked out with its pixel
// area). Each visit of the fetch pass puts the texture in a tier:
//   T1 near: a face in view now, drawn, within NEAR_METERS;
//   T2 view: a face in view now (far, or hidden), or in view within the grace;
//   T3 off: no face in view for longer than the grace.
// Switches (each 0: as before): PlusTexViewPriority (setting) and
// texview.toggle (cue) for all of it; LLRHI_TEX_VIEW_FETCH the fetch and
// creation order and the floor (PlusTexFarFloorPx, 0: none);
// LLRHI_TEX_VIEW_BIAS the discard bias off screen only (ScanValues; not while
// the bias is up for another reason than VRAM: updateClass's bias_all), with
// LLRHI_TEX_VIEW_LAST_RESORT=0 never lowering what is on screen;
// LLRHI_TEX_VIEW_FREE off screen scaled down first (scaleDownFirst);
// LLRHI_TEX_ONSCREEN_COARSEST (PlusTexOnScreenCoarsest, 3-4; 0 or 5+: none) what is on screen is not left at
// level 5 (coarsestWanted; research/303).
// LLRHI_TEX_VIEW_STATS=<seconds> (60 when unset, 0: none): the counters as a
// TextureMemory line. The tiers and the counters are worked out with the
// switches off too (to compare).
namespace plus::texprio
{
    enum Tier : U8
    {
        TIER_NONE = 0,  // no faces (lights, terrain...), or not looked at yet
        TIER_NEAR = 1,
        TIER_VIEW = 2,
        TIER_OFF = 3,
        TIER_COUNT = 4
    };

    constexpr F32 NEAR_METERS = 20.f;
    constexpr F32 OFF_CLOSE_METERS = 8.f;   // an off-screen face this close goes down one level at most

    // What the face scans read, set once a frame (updateClass) and copied into
    // LLViewerTextureList's scan context on the main thread.
    struct ScanValues
    {
        F32 grace = 30.f;       // seconds a face counts as in view after it left the cone
        // LLRHI_TEX_VIEW_BIAS (research/285 §2.3): the discard bias split by
        // view. A face's size is divided by on_divisor in view or within the
        // grace (1; up to 4 as the last resort), by off_divisor past it
        // (4^(bias - 1), 4 times that at pressure stage 2 and up), and by at
        // most 4 (one level) within OFF_CLOSE_METERS. Off: upstream's bias.
        bool split = false;
        F32 on_divisor = 1.f;
        F32 off_divisor = 1.f;
        // research/296 (tp-release, diagnostics only: nothing here changes a tier): faces whose near
        // side is at least far_cut away cannot be drawn: the draw distance over the view cone's cos
        // (the cone and the sphere together reach past it at a wide view) plus FAR_MARGIN_METERS.
        // F32_MAX: not counted (RenderUseFarClip off, a snapshot holds the budget).
        F32 far_cut = F32_MAX;
    };
    const ScanValues& scanValues();

    // Whether a face counts as in view: in the cone at its last pixel area
    // update, or in it within the grace. Any thread.
    inline bool inGrace(const ScanValues& values, F32 frame_time, bool in_view, F32 in_view_time)
    {
        return in_view || (in_view_time > 0.f && frame_time - in_view_time <= values.grace);
    }

    // What inGrace would say if the grace were cut by distance (research/296 section 14, diagnostics:
    // not used for the tiers): a static face whose near side is past far_cut has none. A moving face
    // (its centre may be stale), a HUD one and a NaN near side keep it. Any thread.
    // (face: in_view, in_view_time, near_side and moving are read; FaceInput is below.)
    struct FaceInput;
    inline bool inGraceFar(const ScanValues& values, F32 frame_time, const FaceInput& face);

    // With values.split: what a face's size is divided by (its bias);
    // near_side: the distance to the near side of its bounding sphere. Any thread.
    inline F32 faceDivisor(const ScanValues& values, bool in_grace, F32 near_side)
    {
        if (in_grace)
        {
            return values.on_divisor;
        }
        return near_side <= OFF_CLOSE_METERS ? llmin(values.off_divisor, 4.f) : values.off_divisor;
    }

    // What a texture's face scan found (LLViewerTextureList::scanFaces).
    struct FaceView
    {
        F32 in_view_time = 0.f; // the latest frame time a face was in view (LLFace::noteInView; 0: never)
        // Its size in its band: the largest size before the bias of a face in
        // view and drawn; a hidden one, or one only within the grace, counts
        // for HIDDEN_SHARE of its size.
        F32 view_size = 0.f;
        F32 need_size = 0.f;    // the largest size before the bias of a face in view or within the grace
        bool close_by = false;  // a face in view now, drawn, within NEAR_METERS
        bool in_grace = false;  // a face in view now, or within the grace
        // research/296 (diagnostics): see inGraceFar and addFace
        bool would_grace = false;       // a face in view now, or within the grace cut by distance
        bool has_moving = false;        // a face of an attachment, an avatar or a moving drawable
        bool has_hud = false;           // a face of a HUD attachment
        bool all_far = true;            // every face's near side past far_cut (the HUD's too: it is 0)
        F32 min_near_static = F32_MAX;  // the smallest near side of the faces that are neither moving nor HUD
        bool in_view_far = false;       // a face in the cone with its near side past far_cut (a stale centre?)
        bool all_scanned = false;       // every face was looked at (faces without an object are skipped by the scan)
    };

    // One face of a texture's scan: in_view and in_view_time from its last
    // pixel area update (a HUD attachment's: in view now), in_grace
    // (inGrace), near_side: the distance to the near side of its bounding
    // sphere (0 for a HUD attachment), drawn (seen by the main or a shadow
    // camera lately, or an attachment), unbiased: its size before the bias.
    // Any thread (reads its arguments only).
    // One face's values for addFace (named: a run of bools reads badly). would_grace (inGraceFar), moving and hud
    // are research/296's diagnostics, far_cut is ScanValues'.
    struct FaceInput
    {
        bool in_view = false;
        F32 in_view_time = 0.f;
        bool in_grace = false;
        F32 near_side = 0.f;
        bool drawn = false;
        F32 unbiased = 0.f;
        bool would_grace = false;
        bool moving = false;
        bool hud = false;
        F32 far_cut = F32_MAX;
    };
    void addFace(FaceView& view, const FaceInput& face);

    inline bool inGraceFar(const ScanValues& values, F32 frame_time, const FaceInput& face)
    {
        const bool far_off = !face.moving && face.near_side >= values.far_cut;
        return face.in_view || (!far_off && face.in_view_time > 0.f && frame_time - face.in_view_time <= values.grace);
    }

    // The <plus> hooks in LLViewerTextureList::finishFaceScan (main thread).
    // onFaceScan: the texture's tier, size in its band, the level its faces
    // in view need (need_cap: the most texels they may take, the pressure
    // cap) and when a face was last in view. unknown_faces: not every face
    // could be looked at (over 1024, or the scan stopped at the largest size):
    // taken as in view.
    void onFaceScan(LLViewerFetchedTexture* tex, const FaceView& view, U32 face_count, bool unknown_faces, F32 nearest,
                    F32 need_cap);
    // The virtual size with the floor: a texture with faces and no image yet
    // (not missing), its nearest face within the draw distance (when
    // RenderUseFarClip is on), asks for at
    // least PlusTexFarFloorPx squared, so its first level is always fetched.
    F32 floorSize(LLViewerFetchedTexture* tex, F32 vsize, F32 nearest);

    // The <plus> hooks in LLViewerFetchedTexture::updateFetch. The fetch
    // priority (upstream: priority, the virtual size; 0 still asks for nothing):
    // T1 above T2 above the rest, by size in the band (no image yet: 4 times);
    // T3 by its virtual size, and while VRAM is short nothing sharper (0) and a
    // first level last of all (1), unless a projector light uses it. No faces:
    // with T2 when boosted, a light map or not a LOD texture (water maps...).
    // Below plus/selffirst's own textures.
    F32 fetchPriority(LLViewerFetchedTexture* tex, F32 priority);
    // How long a fetch at priority 0 waits before the fetch thread is told
    // (upstream: hold): 1 s for T3 while VRAM is short (as fetchPriority).
    F32 holdSeconds(LLViewerFetchedTexture* tex, F32 hold);

    // The <plus> hook in LLViewerTextureList::updateImagesCreateTextures: the
    // order of the textures waiting for their image after the boosted ones
    // (larger first; 0: the switch is off, order as before).
    F32 createOrder(LLViewerFetchedTexture* tex);

    // The <plus> hook in LLViewerLODTexture::scaleDown (LLRHI_TEX_VIEW_FREE):
    // true puts the texture in the scale-down queue that goes first
    // (LLViewerTextureList::mDownScaleQueueOff): T3. Scaled down to its desired
    // level as before, never past its smallest (the image is kept).
    bool scaleDownFirst(LLViewerFetchedTexture* tex);

    bool fetchOn();

    // research/306 (P1): what the cache index does not have is not read (LOAD_FROM_TEXTURE_CACHE); the switch
    // LLRHI_TEX_CACHE_MISS_SKIP (default on, read once).
    bool cacheMissSkipOn();

    // research/304: drawn as the gray stand-in lately (what the face scan may not list: tier none), so on screen. For
    // plus/texprio/fetchretry (a retry of a refused first step is for what is on screen). Main thread.
    bool grayDrawnNow(const LLViewerFetchedTexture* tex);

    // research/303: the coarsest level (discard level) a texture may settle for: for one on screen (near or in view
    // by the face scan, within the draw distance), PlusTexOnScreenCoarsest (4 by default: not the first 600 bytes,
    // level 5); off screen, the level it already has when that is the floor's (kept, nothing new asked for, unless
    // VRAM is short); else MAX_DISCARD_LEVEL (as upstream). MAX too while the last resort's divisor is 2 and up or a
    // bias for another reason than VRAM is 2 and up. LLViewerLODTexture::processTextureStats takes the level it wants
    // no coarser than this, before the pressure stages. Main thread; a cached value.
    S32 coarsestWanted(LLViewerFetchedTexture* tex);

    // LLRHI_TEX_VIEW_STATS: the seconds between the counters' log lines (60 when unset, 0: none; the counters
    // of plus/texprio/fetchflow stop then too). Read once.
    U32 statsSeconds();

    // The tier a fetch priority (from fetchPriority) is in: TIER_NEAR, TIER_VIEW or TIER_OFF by its band. Not
    // the agent's own textures' (plus/selffirst). Meaningless with the fetch switch off (fetchOn()).
    U8 tierOfPriority(F32 priority);

    // Once a frame from LLViewerTexture::updateClass (main thread): the
    // switches, the grace, the divisors, the log line. fill: the textures
    // against the driver's budget less the rest of VRAM (texture pressure's
    // limit; over 1: over it; 0: not known, the last resort held as it is).
    // bias_all: the discard bias may have risen for another reason than VRAM
    // (system memory short, plus/memguard, the window in the background, a
    // VRAM budget the user set): no split then, the bias lowers what is on
    // screen too (upstream).
    void updateClass(F32 fill, bool bias_all);
    // research/309: the bias split by view is available (the setting and the switches, not bias_all): what the memory
    // guard's Soft state relies on to keep what is on screen as it is. As of the last updateClass (main thread).
    bool splitAvailable();

    // research/296 (tp-release, diagnostics, no change in behaviour): the texture's FarFlags (mRhiFarFlags).
    enum FarFlags : U8
    {
        FAR_CANDIDATE = 1,      // every face static, far past the draw distance, would be T3 with the grace cut: what a
                                // priority 0 for the old region's textures would take (not used yet)
        FAR_MOVING = 2,         // would be a candidate but for a moving face (kept as it is)
        FAR_IN_VIEW_FAR = 4     // a face in the view cone with its near side past far_cut
    };
    constexpr F32 FAR_MARGIN_METERS = 512.f;    // as plus/tprelease's FAR_MARGIN
    constexpr F32 FAR_HYSTERESIS_METERS = 64.f; // a candidate stays one while its near side is within this of the cut
    // The diagnostic segment of the TPLag line from the last full round (empty: no round yet, or off):
    // textures by what the grace cut by distance would change, static and moving apart, with their images' MB
    // (estimated from the GL sizes). The pass runs while a TPLag timeline is open (and counts only a round
    // that began after it opened: a round before that says "not counted yet").
    std::string farDiagLine();

    // Counters (main thread).
    void noteVisit(LLViewerFetchedTexture* tex);    // a visit of the fetch pass (also: a texture with no image for 10 s, by why)
    // LLGrayWire (PlusMaterializeLog on), a face drawn as the gray stand-in for good (research/302): its texture's
    // state on one log line, once a window per texture, a few per window. Main thread.
    void noteGrayDrawn(LLViewerFetchedTexture* tex, const char* cause, const LLDrawInfo* params = nullptr);
    // research/304 (diagnostics): updateFetch asked for nothing for a texture with no image, for this reason (the
    // chain's own, a PlusWhy; counted by tier and reason per window); a selected object's TE with no image
    // (boostTexturePriority, which a selection calls every idle: each texture once a window): the state of its
    // face, face lists and batch as it was before the boost. Main thread; both read only.
    void noteNoRequest(LLViewerFetchedTexture* tex, int why);
    void noteSelectedNoImage(LLViewerObject* object, S32 te);
    void endRound();                                // the fetch pass went round every texture
    void noteRequest(LLViewerFetchedTexture* tex);  // a fetch asked for (none in flight before)
    void noteFetchDone(LLViewerFetchedTexture* tex, bool got_data);
    void noteZeroPriority(LLViewerFetchedTexture* tex);  // a fetch in flight set to priority 0 (once per fetch)
    void noteScaleDown(LLViewerFetchedTexture* tex);

    // The texture census's (LLRHI_TEX_CENSUS) names of the tiers.
    const char* tierName(U8 tier);
    // What an image takes from its GL size, RGB as 4 bytes and a full mip
    // chain (BC7 not known): the counters' and the census's estimate.
    U64 estimatedImageBytes(const LLImageGL* gl);
}

#endif // PLUS_TEXPRIO_H

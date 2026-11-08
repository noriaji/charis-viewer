/**
 * @file llsceneshadow.h
 * @brief The shadow passes' casters drawn from the retained scene's records (research/206, stage 5 of research/197).
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

#ifndef LL_LLSCENESHADOW_H
#define LL_LLSCENESHADOW_H

#include "llrhiscene.h"

#include <utility>
#include <vector>

class LLCamera;
class LLDrawInfo;
class LLSpatialGroup;
class LLViewerTexture;

// research/206 (stage 5): the shadow casters the retained scene (research/197
// stage 2, llscenerecords.h) can draw: the opaque shadow types' batches of
// groups whose records are all resident and placed (volume and spatial bridge
// partitions; research/218 vk-s4b2: and the attachments' and animesh's, their
// batches that are not rigged, as bridges; LLSceneRecords::shadowTakes).
// research/219: and those types' rigged maps (type + 1), skinned by their records' palette rows,
// while the native shadows draw the rigged maps (LLRHI::sNativeRiggedShadows), the backend draws
// skinned shadows (shadowSkins) and LLRHI_SCENE_RIGGED is on (a call that only chooses: not off);
// a group of them only while its skins are ready (LLSceneRecords::shadowSkinsReady). 5c (research/206
// section 8): and the alpha masks' (PASS_ALPHA_MASK, PASS_FULLBRIGHT_ALPHA_MASK, the legacy
// materials' four masks) once the backend can draw them (shadowMasks): each
// type drawn as its packets are (ShadowPacket's mode), the record's diffuse
// texture's alpha against its cutoff; a group only while every such texture
// can be sampled from its number (else the whole group stays on the usual
// path that frame), the layer's textures resolved for its pass.
//
// A layer is the set of such groups one native shadow pass takes, each by
// its visible bit in the pass's view (a group's batches of the cut types are
// all in the pass, or none):
//  - moving (5a, 5b H6): a sun cascade's casters drawn every frame. The
//    groups whose batches are all moving: a spatial bridge's (PARTITION_
//    BRIDGE; vk-s4b2: an attachment's, PARTITION_AVATAR, and an animesh's,
//    PARTITION_CONTROL_AV), and (5b) a static group that has not settled. The cascades
//    of each: those that saw it, less those the receiver cull leaves out
//    (research/64).
//  - cache (5b H1): a sun cascade's cached depth when it is drawn again
//    (renderCachedShadowCascade's redraw). The settled static groups whose
//    batches of the cut types are all static, in the cascades whose cached
//    depth takes them (not those still joining). plus/shadowshift's strips
//    and partial redraws stay on the usual path: they list the casters from
//    the sort's entries (shadow_sort_visit_static), which keep every batch.
//  - spot (5b H7): a spot light's shadow (generateSunShadow): every group the
//    pass's cull leaves visible, marked in its postSort.
//
// LLPipeline::renderSunShadowCascades sorts each cascade's batches into the
// cached, joining and moving lists and marks the scene's groups per cascade
// and layer; while the scene draws (LLRHI_SCENE_SHADOW=on) those batches of
// the cut types stay out of the lists (the cached ones out of the redraw's,
// shadow_sort_fill). A spot pass's postSort leaves them out of its render
// map. The pass (drawNativeShadowCasters) has the scene choose the layer's
// view and draw the chosen records depth only before its packets
// (LLRHI::Scene::drawShadow, the backend's shadow renderer). When the scene
// cannot, the cut batches go back: as packets of the same pass, or into the
// render map when the native pass fails (and the scene cuts no more).
// Nothing of the static cache's bookkeeping changes (fingerprints, shadowshift's
// strips and content, the GPU shadow list's serials and groups: the groups
// it holds stay out of the cull and so out of these layers).
//
// Verify (LLRHI_SCENE_SHADOW=verify, and on while it draws): in turn, the
// batches a layer takes (or would take) against the view's choice as the
// CPU works it out (LLRHI::Scene::collect), and each batch against its record.
// auto (research/218 vk-s2f): verify until those checks agreed 600 world
// frames, then on (from the next call or spot pass); a difference: off for the
// run (nothing chosen from the next call on).
//
// Main thread, but groupId, cutType and spotOpen: the sort's cull threads and
// postSort's read them while the main thread waits.
class LLSceneShadow
{
public:
    static constexpr U32 CASCADES = 4;
    static constexpr U32 SPOTS = 2;
    static constexpr U32 VERIFY_EVERY = 8;          // layers between verified layers (moving: calls)
    static constexpr F64 REPORT_SECONDS = 10.0;
    static constexpr U32 PARTITION_SLOTS = 16;      // the census's partition types: > LLViewerRegion::NUM_PARTITIONS (checked in the .cpp)

    enum Layer : U8
    {
        LAYER_NONE,
        LAYER_MOVING,
        LAYER_CACHE,
        LAYER_SPOT,
        LAYER_COUNT
    };

    // A call of renderSunShadowCascades (main view, sun, static cache, the
    // sort on the cull threads, native shadow passes). Its cascades: index
    // per sort position.
    // research/208 13.6 (vk-s4a4): probe, the probes' shared maps (shareProbeShadows; counted apart).
    void beginCascades(bool eligible, U32 count, const S32* indices, bool probe = false);
    void endCascades();
    bool inCall() const { return mInCall; }

    // Before the sort, on the main thread: the scene's id of each group of
    // the union (0: not one the scene may draw).
    void noteGroups(const std::vector<LLSpatialGroup*>& groups);
    U32 groupId(size_t g) const { return g < mGroupIds.size() ? mGroupIds[g] : 0; }
    bool cutType(U32 type) const { return type < LLRHI::SCENE_MAX_PASSES && mCutType[type]; }
    // The sort cuts the marked batches out of the moving lists, and the
    // redraw of a cached depth out of its list (on, not stopped).
    bool cutting() const { return mInCall && mCutting; }
    // The fold (main thread): the group's cascades (sort positions' bits),
    // of its moving casters and of the cached depths that take it; g: its
    // place in noteGroups' list (5c: its masks' textures).
    void markGroup(U32 id, U32 moving, U32 cached, size_t g);
    // research/206 5c: the ShadowPacket::Mode a cut type's batches are drawn
    // with (0: depth only; the masks' modes as drawNativeShadowCasters gives
    // their packets).
    static U8 casterMode(U32 type);
    void noteCut(U32 batches) { mBatchesCut += batches; }
    void noteCacheCut(U32 batches) { mCacheBatchesCut += batches; }

    // The cascade being drawn (its sort position), its moving pass, and the
    // redraw of its cached depth.
    struct CascadeScope
    {
        explicit CascadeScope(U32 position);
        ~CascadeScope();
    };
    struct MovingScope
    {
        MovingScope();
        ~MovingScope();
        const Layer mSaved;
    };
    struct CacheScope
    {
        CacheScope();
        ~CacheScope();
        const Layer mSaved;
    };
    // The cached depth drawn in the last CacheScope took records its verify
    // found different from their batches (on): the caller draws it again
    // next frame (by then the usual way), rather than keeping it.
    bool cacheDiffered() const { return mCacheDiffered; }
    // research/206 5b: a spot light's shadow pass (spot 0 or 1), from its
    // cull (postSort marks the groups) to its draw. eligible: the native
    // shadow passes draw it.
    struct SpotScope
    {
        SpotScope(U32 spot, bool eligible);
        ~SpotScope();
        const Layer mSaved;
    };
    // postSort of the spot pass (main thread): whether a spot pass is being
    // sorted; then for each group it leaves visible, whether the scene takes
    // its batches of the cut types (true: leave them out of the render map;
    // verifying: marked, false).
    bool spotOpen() const { return mSpotOpen; }
    bool markSpotGroup(LLSpatialGroup* group);
    const std::vector<LLSpatialGroup*>& spotGroups() const { return mSpotGroups; }

    // The native pass being built belongs to a layer with groups marked.
    bool layerActive() const;
    Layer layer() const { return mLayer; }
    U32 layerPosition() const { return mPosition; }
    // Whether the layer's batches were cut (out of the lists or render map).
    bool layerCut() const;
    // drawNativeShadowCasters: the layer's view chosen and (cut) its records
    // made ready for the pass. False with layerCut: the caller draws the cut
    // batches itself (as packets).
    bool drawLayer(const F32* view_proj, LLCamera& camera);
    // The verify of this layer is due (the caller lists the cut batches).
    bool verifyDue() const { return layerActive() && mVerifyDue; }
    void verifyLayer(const std::vector<std::pair<U32, LLDrawInfo*> >& batches);
    // The cut batches were drawn the usual way: as packets (drawLayer
    // false), or into the render map (the native pass failed: no more cuts).
    void rescued(U32 batches, bool native_failed);

    // research/218 vk-s2f: what the shadows do now: LLRHI_SCENE_SHADOW as
    // asked, or its auto's progress (verify, then on after 600 world frames
    // of the layers' checks without a difference; off at one, at a difference
    // of the backend's checks or at a native pass that failed with the
    // scene's casters in it).
    LLRHI::SceneMode mode() const { return LLRHI::scenePartNow(LLRHI::sceneShadowMode(), mAuto); }

private:
    void autoCheck(bool differ, const char* why);   // research/218 vk-s2f: auto's step at a check
    void openCall(LLRHI::SceneMode mode);
    void chooseTypes(bool cutting);    // research/219: cutting: the rigged maps only while the rigged passes are on
    void endSpot();
    U32 layerView() const;
    const std::vector<U32>* layerBits() const;
    const std::vector<LLViewerTexture*>* layerTextures() const;    // 5c
    bool listLayerTextures();                                       // 5c: false: one cannot be sampled now
    void stop(const char* why);
    void report();
    void reportAvatars();       // research/206 H2 (research/218 vk-s0b): the avatars' side of the census

    bool mInCall = false;
    bool mCutting = false;
    bool mStopped = false;
    bool mLoggedMode = false;
    bool mProblemsKnown = false;
    U32 mSeenProblems = 0;          // LLRHI::Scene::problems() last seen
    LLRHI::SceneAuto mAuto;         // research/218 vk-s2f
    U32 mCount = 0;
    S32 mIndex[CASCADES] = { -1, -1, -1, -1 };
    U32 mPosition = U32_MAX;
    Layer mLayer = LAYER_NONE;
    bool mVerifyDue = false;        // the layer drawLayer made ready is the one verified now
    bool mCacheDiffered = false;    // since the last CacheScope began
    U32 mCalls = 0;
    U32 mLayerSerial[LAYER_COUNT] = {};     // layers drawn (cache, spot: their verify turns)

    bool mCutType[LLRHI::SCENE_MAX_PASSES] = {};
    std::vector<U32> mPasses;
    std::vector<U8> mModes;                 // 5c: per pass its caster mode
    bool mMasks = false;                    // 5c: mask types among the passes
    bool mRigged = false;                   // research/219: rigged maps among the passes (skinned)
    U64 mMaskBits[(LLRHI::SCENE_MAX_PASSES + 63) / 64] = {};
    std::vector<U32> mGroupIds;
    // research/206 5c: the masks' textures of noteGroups' groups (a run per
    // group), of each cascade's layers (moving, cache) and of the spot pass
    // (the groups marked in them, repeats and all), and drawLayer's list of
    // the layer's (number, GL name) sorted by number.
    std::vector<LLViewerTexture*> mGroupTextures;
    std::vector<U32> mGroupTexFirst;
    std::vector<U32> mGroupTexCount;
    std::vector<LLViewerTexture*> mLayerTextures[2][CASCADES];
    std::vector<LLViewerTexture*> mSpotTextures;
    std::vector<LLViewerTexture*> mTexScratch;
    std::vector<std::pair<U32, U32> > mTexPairs;
    std::vector<U32> mTexIds;
    std::vector<U32> mTexNames;
    std::vector<U32> mBits[CASCADES];       // moving casters
    std::vector<U32> mCacheBits[CASCADES];  // cached depths (5b)
    U32 mMarked[CASCADES] = {};
    U32 mCacheMarked[CASCADES] = {};

    // research/206 5b: the spot pass open now.
    bool mSpotOpen = false;
    bool mSpotCutting = false;
    U32 mSpot = 0;
    std::vector<U32> mSpotBits;
    std::vector<LLSpatialGroup*> mSpotGroups;   // marked, this pass only

    // Verify scratch.
    std::vector<U32> mOld;
    std::vector<U32> mDrawn;
    std::vector<U32> mBounded;
    U32 mLogged = 0;

    // Since the last report.
    F64 mReportAt = 0.0;
    U32 mReportCalls = 0, mReportSpots = 0;
    U32 mReportProbeCalls = 0;      // research/208 13.6: of mReportCalls, the probes' shared maps'
    U64 mGroupsCut = 0, mBatchesCut = 0, mCacheGroups = 0, mCacheBatchesCut = 0, mSpotGroupsCut = 0;
    // noteGroups' census: the union's groups by kind, and those the scene may draw.
    U64 mCensusVolume = 0, mCensusVolumeScene = 0, mCensusBridge = 0, mCensusBridgeScene = 0, mCensusAttachment = 0, mCensusOther = 0;
    U64 mCensusAttachmentScene = 0;         // research/218 vk-s4b2: the attachments' and animesh's groups the scene may draw
    // research/206 5c: groups left out for their masks' textures; H2's census (sampled calls, every
    // VERIFY_EVERY): the attachments' and animesh's batches, rigged or not, and the animesh groups.
    U64 mCensusMaskWait = 0;
    U64 mCensusSkinWait = 0;                // research/219: groups left out for their rigged records' palettes
    U32 mCensusSamples = 0;
    U64 mCensusAttachRigged = 0, mCensusAttachPlain = 0, mCensusAnimesh = 0;
    // research/218 vk-s0b: the union's groups by the partition type they come in (the last slot:
    // a type past the region's), every call. Together with reportAvatars it tells where H2's
    // batches go when the union's attachments stay 0.
    U64 mCensusPartitions[PARTITION_SLOTS] = {};
    U32 mLayers[LAYER_COUNT] = {}, mLayersFailed[LAYER_COUNT] = {}, mVerified[LAYER_COUNT] = {};
    U64 mLayerTextureCount = 0;             // 5c: textures resolved for the layers drawn
    U32 mLayerTexturesWait = 0;             // 5c: layers whose textures could not all be sampled (the usual way drew them)
    U64 mRescuedPackets = 0, mRescuedMap = 0;
    U32 mDifferences = 0, mChosenNotDrawn = 0, mDrawnNotChosen = 0, mNoRecord = 0, mContent = 0, mTextureMissing = 0;
};

extern LLSceneShadow gSceneShadow;

#endif // LL_LLSCENESHADOW_H

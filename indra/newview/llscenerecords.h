/**
 * @file llscenerecords.h
 * @brief The viewer's side of the retained scene (research/197 stage 2): LLDrawInfo to records, resident groups, views.
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

#ifndef LL_LLSCENERECORDS_H
#define LL_LLSCENERECORDS_H

#include "llpointer.h"
#include "llrhiscene.h"

#include <string>
#include <unordered_map>
#include <vector>

class LLCamera;
class LLDrawInfo;
class LLFace;
class LLFacePool;
class LLGLSLShader;
class LLMatrix4;
class LLMeshSkinInfo;
class LLSpatialGroup;
class LLViewerTexture;
class LLVOAvatar;

// The words of a record's material row (LLRHI::SceneMaterialDesc) as the
// scene's draws read them (the shader permutation that reads records uses the
// same numbers; research/198).
enum SceneMaterialTexture : U32
{
    SCENE_TEX_DIFFUSE = 0,
    SCENE_TEX_NORMAL = 1,
    SCENE_TEX_SPECULAR = 2,
    SCENE_TEX_USED = 3,
};

enum SceneMaterialWord : U32
{
    SCENE_MAT_SPEC_R = 0,           // LLDrawInfo::mSpecColor
    SCENE_MAT_SPEC_G = 1,
    SCENE_MAT_SPEC_B = 2,
    SCENE_MAT_SPEC_EXP = 3,
    SCENE_MAT_ENV_INTENSITY = 4,    // mEnvIntensity
    SCENE_MAT_ALPHA_CUTOFF = 5,     // mAlphaMaskCutoff
    SCENE_MAT_ALPHA_MODE = 6,       // mDiffuseAlphaMode
    SCENE_MAT_BUMP = 7,             // mBump
    SCENE_MAT_SHINY = 8,            // mShiny
    SCENE_MAT_FULLBRIGHT = 9,       // mFullbright (0 or 1)
    SCENE_MAT_SHADER_MASK = 10,     // mShaderMask
    SCENE_MAT_ALPHA_PLACE = 11,     // research/205 3a-3: an alpha batch's place in its group's draw map (its OIT priority's low bits)
};

// The viewer's side of LLRHI::sScene (research/197 3.3, research/198).
//
// A batch (LLDrawInfo) of a target pass in a volume or bridge group gets a
// record: its vertex buffer's number (stage 1b) and its indices in it, its
// textures' numbers and samplers (stage 1a), its model matrix and texture
// matrix as transform rows (one row per matrix, shared by the batches that
// point at it), its material and its bounds. A group whose target batches all
// have records is resident (drawing below); a batch the scene cannot hold
// (refused) keeps its whole group on the usual path.
//
// Registration waits for the next frame's head (the group's rebuild is done
// by then) and is spread over frames (at most BUDGET a frame): a group with
// batches still waiting is not resident.
//
// The events come from the official code (research/198 3.3 has the places):
// LLVolumeGeometryManager::registerFace, ~LLDrawInfo, clearDrawMap,
// ~LLSpatialGroup, LLDrawable::updateXform/shiftPos/destroy,
// LLViewerRegion::updateRenderMatrix and its destructor, the faces' texture
// matrices deleted (LLVOVolume::processUpdateMessage, LLFace::destroy), and
// LLImageGL's sampler changes (through LLRHI::Textures::samplerChanged).
// With LLRHI_SCENE off (LLRHI::sScene null) every entry returns at once.
//
// Drawing (research/198 5.5; LLRHI_SCENE=on, or auto after its clean
// frames): the main view (research/209 stage 2e: and the mirrors' faces and
// the water mirror, and with LLRHI_SCENE_PROBE=on the reflection probes'
// faces, each face a view of its kind in turn), and only the passes of the drawing scope
// (scopePass: research/198 5.5 PASS_SIMPLE; 5.6 the masks, the legacy
// materials, fullbright and glow, with the uniforms their pools set per batch
// in each record's own material row) whose program the backend can draw from
// rows (canDrawPass). postSort cuts
// those passes' batches of each visible resident group whose records all have
// their buffer placed with the attributes the pass reads (groupDrawable) out
// of the render map, and marks only such groups visible to the scene; at the
// pass's entry the scene draws them. If it cannot (drawPass false), the
// caller draws the cut batches the usual way from the groups' draw maps (the
// list drawPass returns), so nothing is ever missing from a frame.
//
// research/209 (stage 2e): a probe face or a mirror is a view of its kind
// (LLRHI::SCENE_VIEW_PROBE, _MIRROR, _WATER; the avatar reflection none),
// begun again face after face. Its texture matrices are caught up before
// the first such face after the idle (the mirrors are drawn before
// beginFrame). Its share of the frame's rows and its refused passes are its
// kind's in the backend. The probe list (G5) leaves the groups it cuts, and
// a face whose static prefix the probes' cache takes keeps PASS_SIMPLE
// (keepPass). One face of a kind in turn (LLRHI::Scene::viewTurn) is
// compared: drawn the usual way without the face's batch cull, the probe
// list and the static prefix (comparesView), its render map against the
// choice as the main view's.
//
// Verify (every VERIFY_EVERY frames while the scene does not draw): at each
// target pass of the main view, the render map's batches of resident groups
// (what every way of the usual path draws: pushBatch, the draw lists, the
// material packets) against the scene's choice for that pass (the same
// records, apart from those it leaves out by their own bounds), and each such
// batch described again against its record (the description's hash, and the
// model and texture matrices against their rows' copies): a missed event
// shows up as a difference.
//
// research/205 3a-3 (LLRHI_SCENE_ALPHA verify or on): the main view's
// plain and fullbright alpha batches (not rigged, not GLTF, no legacy
// material, the usual blend, a texture list of one, in volumes and bridges)
// get records too, of two passes of their own past the render types
// (alphaPass), in alpha groups of their own (a group's opaque residency does
// not depend on its alpha). In an OIT "all" session (the order does not
// matter there: the resolve orders each pixel's nodes by their priority)
// the alpha pool's renderAlpha marks the groups it draws (its group rules)
// visible to the alpha view (the main view's planes), with each group's OIT
// priority this frame, and draws each of the two passes from the records
// with the pass's program bound; each record's row gets its group's priority
// with its place in the group's draw map, as the pool gives a batch. A batch
// the scene drew (alphaDrawnBy) is left out by the pool's loops, which
// still draw its glow and every other batch. A pass the scene cannot draw
// is drawn by the loops as before. verify: chosen and expanded only, the
// rows compared by the backend. Other alpha passes (under water, the depth
// passes, rigged) stay on the usual way.
//
// research/205 3a-4: a batch with a legacy material has a record too, of
// an alpha pass per shader mask (gDeferredMaterialProgram[mask], its
// normal and specular maps and the three uniforms the pool sets per such
// batch from the record's material row). A custom blend, rigged and GLTF
// batches still have none.
//
// research/205 H5 (vk-s3a6): a plain or fullbright alpha batch with a
// texture list of several (no material, no texture matrix: TexSetup binds
// entry u to unit u and loads no texture matrix) has its list in its record
// (LLRHI::SceneTextureList): the alpha passes' draws bind each unit of the
// program's indexed textures from the record's list, a record without one
// its diffuse texture on unit 0 as before.
//
// research/205 (vk-s3a6), the glow: an alpha batch with a record whose
// vertex buffer has glow (TYPE_EMISSIVE: the pool's glow draw of it) has a
// second record, of the glow pass (gDeferredEmissiveProgram, its diffuse
// texture or list and texture matrix as TexSetup gives the glow draw), in
// the same alpha group at the same place. In an OIT "all" session the pool
// draws the glow pass from the records once its alpha passes are drawn
// (with the glow blend; each row's priority is its group's with 0xfff in
// the low bits, as setOITPriority gives a glow draw) and leaves the glow of
// those batches out (glowDrawnBy). In the sorted way the glow pass is
// expanded with the others and, at a group's end, its glow spans (the
// places next to each other with glow records) are drawn in their places
// among the loop's glow draws (glow_walk). A batch no face of which glows
// (plus/alphapass's GLOW_NONE) has no glow record: its glow draw stays the
// loop's (or left out by plus/alphapass's skip), as before.
//
// research/205 3a-5 (the sorted way): in a hybrid OIT session's world half
// and without OIT the order of the draws shows. The pool still goes through
// the groups far to near as before; a visible group's records come in spans
// (alpha_spans: the places next to each other in its draw map whose batches
// have records of one alpha pass), made when its records change, never per
// frame. At the pass's entry each alpha pass is expanded once with every
// record's command at a fixed place (its group's base this frame plus its
// place); then, in the loop, at a span's first batch the pool binds the
// span's program and the scene draws the span (drawAlphaSpan: one indirect
// draw per pool chunk); its batches are then left out, their glow still the
// loop's. A span the scene cannot draw is drawn by the loop batch by batch
// in its place, so the order is always the pool's. While the scene's alpha
// takes a pass (any mode but off) the pool's own loop draws the rest of it,
// not plus/alphalist (research/205 H6: alphalist absorbed).
//
// research/218 vk-s4b2 (research/206 8.4 (a)): the worn objects' groups, an
// attachment's linkset (PARTITION_AVATAR, the avatar's bridge) and an
// animesh's (PARTITION_CONTROL_AV), have records of their opaque batches that
// are not rigged, as a spatial bridge's: their model matrix is the bridge's
// root's world matrix, written by LLDrawable::updateXform (onMatrixChanged)
// whenever the avatar moves the attachment, and swept at the frame's head.
// LLRHI_SCENE_ATTACHMENTS (wornMode): off (no records, as before), verify
// (never cut: chosen and compared by the views that only choose, the main
// view's verified frames, the probes' and mirrors' turns, the shadows'
// verify; a view that cuts leaves them out of its choice), on (default: cut
// as the bridges are). A group with batches worn by an avatar (mWireOwner) is not
// cut while GrayWire draws that avatar as a wireframe, nor in the main view
// while its G-buffer pass marks skin (plus/skinsss: an object worn on an eye
// point is marked per batch). The shadows draw them in the moving layer
// (bridges). Their alpha and HUD batches have no records.
//
// research/219 (vk-s4b3b, LLRHI_SCENE_RIGGED not off): the rigged batches
// (worn rigged meshes, animesh) of the scope passes' rigged maps (render
// type + 1, riggedBase) have records too, drawn by the pass's rigged program
// (its base program's mRiggedVariant), each with its avatar x skin's resident
// palette row (LLVOAvatar::rhiPaletteSlot) in its record (SceneRecordDesc::
// palette; loose, no texture list, no model matrix); the record pins the row
// (rhiPinPaletteSlot). A group with rigged records is drawn from them in a
// view only while every skin of them is ready (readySkins: the palette built
// this frame and in the row of the backend frame's table, the row still the
// record's, no hair sway): else the whole group stays on the usual way that
// time. A record whose row changed is described again at the next frame's
// head (mSkinRedescribe), never in the middle of a frame. In the main view's
// G-buffer pass while it marks skin (plus/skinsss), a group with rigged
// records is cut only when every record's mark is the one its pass draws
// with (the rigged passes: the plan's avatar mark, SceneMarkScope; the other
// passes: none); a pass whose plan turned out wrong is drawn the usual way.
// The rigged passes' own auto (riggedMode): a difference of a rigged pass
// stops the rigged passes only.
//
// Main thread only (but onSamplerChanged, which only queues).
class LLSceneRecords
{
public:
    static constexpr U32 BUDGET = 2000;         // records registered a frame
    static constexpr U32 VERIFY_EVERY = 8;      // frames between verified frames
    static constexpr U32 AUTO_FRAMES = 600;     // verified frames without a difference before auto draws
    static constexpr U32 RETRY_EVERY = 32;      // frames between the retries of softly refused batches
    static constexpr U32 RETRY_MAX_GAP = 32;    // in RETRY_EVERY rounds (about 1,000 frames)
    static constexpr F64 REPORT_SECONDS = 10.0; // between the census lines in the log

    // Why a target batch has no record (the census).
    enum Refusal : U32
    {
        REFUSED_RIGGED,         // mAvatar or mSkinInfo (research/219: with LLRHI_SCENE_RIGGED off, or not of a scope pass's rigged map), or an alpha batch worn by an avatar (GrayWire)
        REFUSED_HUD,
        REFUSED_PARTITION,      // not a volume, bridge or (research/218 vk-s4b2) worn object's partition; a worn object's alpha
        REFUSED_NO_BUFFER,      // no vertex buffer, or no indices
        REFUSED_NO_PLACEMENT,   // the vertex buffer has no number (stage 1b off, or not on yet)
        REFUSED_NO_TEXTURE,     // a texture without a number (stage 1a off, or no image yet)
        REFUSED_TEXTURE_LIST,   // several diffuse textures (mTextureList)
        REFUSED_TEXTURE_MATRICES,   // a normal or specular map matrix of its own
        REFUSED_GLTF,           // PBR materials have a table of their own (later)
        REFUSED_MATRIX,         // no transform row for its model or texture matrix
        REFUSED_TABLE,          // the scene refused it (full, or not ready)
        REFUSED_PROGRAM,        // its pass's program is not loaded (its attribute mask unknown)
        REFUSED_MISSING_MAP,    // a material pass's batch without the normal or specular map its program samples
        REFUSED_ALPHA_KIND,     // research/205 3a-3: an alpha batch with a custom blend (3a-4: or a shader mask past the programs)
        REFUSED_NO_PALETTE,     // research/219: a rigged batch whose skin has no resident palette row now (soft)
        REFUSED_COUNT,
    };

    // research/219: the scope pass a rigged map's batches are drawn as (its
    // render type - 1), U32_MAX: not one of the scope passes' rigged maps.
    static U32 riggedBase(U32 pass);
    // research/219: what the rigged passes do now: LLRHI_SCENE_RIGGED as
    // asked, or its auto's progress (verify, then on after 600 world frames
    // of checks of rigged batches without a difference; off at one).
    LLRHI::SceneMode riggedMode() const { return LLRHI::scenePartNow(LLRHI::sceneRiggedMode(), mRiggedAuto); }
    // A check of rigged batches elsewhere (the shadows' verify); differ: it found a difference.
    void riggedChecked(bool differ, const char* why);

    // Render types the scene holds in stage 2 (opaque G-buffer, fullbright
    // and glow batches; not rigged, not alpha blended, not bump or shiny).
    static bool targetPass(U32 pass);
    // research/198 5.5, 5.6: those it draws when it draws (the rest are
    // chosen and verified only), each drawn by one program its pool binds
    // (passProgram): PASS_SIMPLE (Deferred Diffuse), PASS_ALPHA_MASK and
    // PASS_FULLBRIGHT_ALPHA_MASK (a cutoff per batch), PASS_FULLBRIGHT and
    // PASS_GLOW (their programs read draw rows while LLRHI_SCENE is not off),
    // and the legacy materials' twelve passes (their uniforms and normal and
    // specular maps per batch). Every batch: its diffuse texture (a list of
    // one), its texture matrix or none, its model matrix or none.
    static bool scopePass(U32 pass);

    // The view being drawn now (SCENE_VIEW_COUNT: none of the scene's:
    // shadows, HUDs, impostors, other cameras, the avatar reflection).
    static U32 currentView();

    // research/209: the view a plus feature's own pass under a probe face's
    // rules (gCubeSnapshot) is drawn as, while the scope lives: the water
    // mirror SCENE_VIEW_WATER, the avatar reflection SCENE_VIEW_COUNT (none:
    // your avatar and its attachments, drawn the usual way from its own cull).
    class ViewScope
    {
    public:
        explicit ViewScope(U32 view) : mSaved(sViewOverride) { sViewOverride = view; }
        ~ViewScope() { sViewOverride = mSaved; }
        ViewScope(const ViewScope&) = delete;
        ViewScope& operator=(const ViewScope&) = delete;

    private:
        const U32 mSaved;
    };

    // research/205 3a-3: the alpha records' passes (plain: the deferred alpha
    // program, fullbright: the fullbright alpha program; 3a-4: a legacy
    // material's, gDeferredMaterialProgram[mask]) and the switch.
    static constexpr U32 ALPHA_PASSES = 19;     // plain, fullbright, the 16 material masks, the glow (vk-s3a6)
    static constexpr U32 ALPHA_GLOW_INDEX = ALPHA_PASSES - 1;
    static U32 alphaPass(bool fullbright);
    static U32 alphaMaterialPass(U32 shader_mask);
    static U32 alphaGlowPass();                 // research/205 (vk-s3a6): the glow pass
    static U32 alphaPassAt(U32 index);          // index < ALPHA_PASSES
    static U32 alphaPassIndex(U32 pass);        // ALPHA_PASSES: not an alpha pass
    static bool isAlphaPass(U32 pass);
    static bool alphaWanted();      // LLRHI_SCENE_ALPHA not off
    static LLGLSLShader* alphaPassProgram(U32 pass) { return passProgram(pass); }

    // research/218 vk-s4b2: LLRHI_SCENE_ATTACHMENTS (off, verify, on; unset:
    // on, auto read as on), read once; off whenever LLRHI_SCENE is.
    static LLRHI::SceneMode wornMode();
    // Whether a shadow pass may take a group of the partition (give it its
    // id): the volumes and bridges, and the worn objects' while they have
    // records and either cut (on) or the pass only chooses (cutting false).
    static bool shadowTakes(U32 partition, bool cutting);
    // research/218 vk-s2f: what the alpha view and the reflection probes'
    // faces do now: LLRHI_SCENE_ALPHA and _PROBE as asked, or their auto's
    // progress (verify, then on after 600 world frames of their checks
    // without a difference; off at one: the backend's checks, a difference of
    // the view, and for the alpha llscenealpha's checks, alphaChecked).
    LLRHI::SceneMode alphaMode() const { return LLRHI::scenePartNow(LLRHI::sceneAlphaMode(), mAlphaAuto); }
    LLRHI::SceneMode probeMode() const { return LLRHI::scenePartNow(LLRHI::sceneProbeMode(), mProbeAuto); }
    // A verified alpha pass of llscenealpha (differ: it found a difference).
    void alphaChecked(bool differ);

    bool active() const { return LLRHI::sScene != nullptr && !mDisabled; }

    // --- events -------------------------------------------------------------
    void onCreated(LLDrawInfo& info, LLSpatialGroup* group, U32 pass);
    void onDestroyed(const LLDrawInfo& info);
    void onGroupCleared(LLSpatialGroup* group);         // its draw map is being cleared
    void onGroupDestroyed(LLSpatialGroup* group);
    // A model matrix the batches may point at was written (a drawable's world
    // matrix, a region's render matrix). Texture matrices have many writers
    // (animateTextures, plus/texanim): their rows follow them by the sweep at
    // the frame's head (sweepRows), as does any model matrix an event missed.
    void onMatrixChanged(const LLMatrix4* matrix);
    // The matrix's memory is freed (a face's texture matrix deleted, a
    // drawable or region gone) before its batches are: its row stays with
    // them, but a new matrix at the same address gets a row of its own.
    void onMatrixFreed(const LLMatrix4* matrix);
    void onTextureMatrixFreed(const LLMatrix4* matrix);

    // --- per frame ----------------------------------------------------------
    void beginFrame();
    bool resident(const LLSpatialGroup* group) const;
    // The scene draws (auto after its clean frames, on): the main view's
    // postSort cuts the drawable groups' batches of the passes cutsPass names.
    bool drawing() const;

    // A view (LLPipeline::postSort): beginView with its camera returns the
    // view (SCENE_VIEW_COUNT: none, or the scene is off); then each group the
    // CPU cull leaves visible, before its batches go to the render map
    // (true: the scene draws its batches of the passes cutsPass names, leave
    // those out); endView hands both to the scene.
    U32 beginView(LLCamera& camera);
    // research/209 S7: the view (a probe face or mirror) is compared this
    // time: postSort leaves the face's batch cull, the probe list and the
    // static prefix out of it.
    bool comparesView(U32 view) const { return view < LLRHI::SCENE_VIEW_COUNT && mViewCompare[view]; }
    // research/209 S3: the view does not cut the pass after all (the probes'
    // static prefix takes PASS_SIMPLE of the face). Before markVisible.
    void keepPass(U32 view, U32 pass)
    {
        if (view < LLRHI::SCENE_VIEW_COUNT && pass < LLRHI::SCENE_MAX_PASSES)
        {
            mCutPasses[view][pass / 64] &= ~(1ull << (pass % 64));
        }
    }
    bool markVisible(U32 view, const LLSpatialGroup* group)
    {
        return view < LLRHI::SCENE_VIEW_COUNT && markVisibleInView(view, group);
    }
    // Whether a group markVisible answered true for leaves its batches of
    // the pass out of the view's render map. Read only (the cull threads ask).
    bool cutsPass(U32 view, U32 pass) const
    {
        return view < LLRHI::SCENE_VIEW_COUNT && pass < LLRHI::SCENE_MAX_PASSES && ((mCutPasses[view][pass / 64] >> (pass % 64)) & 1);
    }
    void endView(U32 view);

    // A target pass's entry (the draw pools, with the pass's program bound):
    // the scene draws the pass's records of the current view (or, verifying,
    // only chooses them and compares). Returns the batches the caller must
    // draw the usual way: postSort cut them and the scene could not draw
    // them (empty almost always).
    const std::vector<LLDrawInfo*>& drawPass(U32 pass)
    {
        if (active())
        {
            return drawPassInView(pass);
        }
        mRescue.clear();
        return mRescue;
    }

    // --- research/206 (stage 5): the sun shadow views (llsceneshadow.cpp) ---
    // The scene's id of a group the shadows may draw from the records now:
    // resident, every record placed with what its pass reads
    // (LLRHI::Scene::groupDrawable). 0: none.
    U32 shadowGroup(const LLSpatialGroup* group) const;
    // The largest group id given out (the size of a view's visible bits).
    U32 groupSpan() const { return mGroupSpan; }
    // Verify: the batch's record (0: none) and whether the record still is
    // what the batch is now (its description, its model matrix's row).
    U32 shadowRecord(const LLDrawInfo& info, U32 pass, bool& same) const;
    // research/206 5c: the diffuse textures the group's records of the
    // passes `passes` names (bits by render type) sample (the shadows'
    // masks), appended once each; false when one cannot be sampled from its
    // number now (no image or GL name, its options waiting for a bind: the
    // usual way binds it this frame). Main thread.
    bool shadowTextures(const LLSpatialGroup* group, const U64* passes, std::vector<LLViewerTexture*>& out);
    // research/219: whether a shadow pass that draws the rigged passes may
    // draw the group from its records now (readySkins of its rigged records'
    // skins; true without any). Main thread.
    bool shadowSkinsReady(const LLSpatialGroup* group);

    // --- research/205 3a-3: an OIT "all" session's alpha (lldrawpoolalpha.cpp) ---
    // beginAlphaView: the alpha view starts (false: not this time, nothing
    // else of it is to be called; sorted: 3a-5's way); markAlphaVisible for
    // each group the pool's loop draws, with its OIT priority; endAlphaView
    // hands them to the scene; alphaPassesVisible: the alpha passes (bits by
    // index) the visible groups' records have; drawAlphaPass for each of
    // those with its program bound (true: the scene drew its records, or,
    // sorted, expanded them for their spans); alphaDrawnBy for each batch of
    // the loop (the program the scene drew it with, null: the loop draws it;
    // never sorted: the spans say); endAlphaPass when the pool's pass ends
    // (AlphaPassScope).
    bool beginAlphaView(bool sorted = false);
    void markAlphaVisible(LLSpatialGroup* group, U32 priority);
    void endAlphaView();
    U32 alphaPassesVisible() const { return mAlphaOpen ? mAlphaPassMask : 0; }
    bool drawAlphaPass(U32 pass);
    const LLGLSLShader* alphaDrawnBy(const LLDrawInfo& info) const
    {
        return mAlphaOpen && !mAlphaSorted && mAlphaAnyDrawn ? alphaDrawnByScene(info) : nullptr;
    }
    // research/205 (vk-s3a6): the scene drew the batch's glow this pass (an
    // OIT "all" session's glow pass; never sorted: the glow spans say).
    bool glowDrawnBy(const LLDrawInfo& info) const
    {
        return mAlphaOpen && !mAlphaSorted && mAlphaPassDrawn[ALPHA_GLOW_INDEX] && glowDrawnByScene(info);
    }
    void endAlphaPass()
    {
        mAlphaOpen = false;
        mAlphaSorted = false;
    }

    // --- research/205 3a-5: the sorted way (lldrawpoolalpha.cpp) ---
    // A span: places [first, first + count) of a group's PASS_ALPHA draw
    // map, every batch with a record of the alpha pass at pass_index.
    struct AlphaSpan
    {
        U16 first = 0;
        U16 count = 0;
        U8 pass_index = 0;
    };
    // The spans of a group in the sorted pass open now (count 0: none, the
    // loop draws every batch of it): the spans in draw map order, the record
    // at each place (0: none) and the group's base in the passes' arrays.
    // research/205 (vk-s3a6): and its glow spans (of the glow pass), with
    // the glow record at each place.
    struct AlphaSpans
    {
        const AlphaSpan* spans = nullptr;
        U32 count = 0;
        const U32* records = nullptr;
        U32 base = 0;
        const AlphaSpan* glow_spans = nullptr;
        U32 glow_count = 0;
        const U32* glow_records = nullptr;
    };
    AlphaSpans alphaSpans(const LLSpatialGroup* group) const;
    // Whether the span's pass was expanded for its spans this time, and its program.
    bool alphaSpanReady(const AlphaSpan& span) const { return span.pass_index < ALPHA_PASSES && mAlphaPassDrawn[span.pass_index]; }
    static LLGLSLShader* alphaSpanProgram(const AlphaSpan& span) { return passProgram(alphaPassAt(span.pass_index)); }
    // With the span's program bound and its batches' state set: the scene
    // draws the span (true: leave its batches out). A glow span (its pass
    // the glow's) with the glow program bound and the glow blend set.
    bool drawAlphaSpan(const AlphaSpans& spans, const AlphaSpan& span);
    struct AlphaPassScope
    {
        LLSceneRecords& records;
        ~AlphaPassScope() { records.endAlphaPass(); }
    };

    // --- research/204 10 (vk-s4c2): the world pools (terrain, trees, water) ---
    // Their faces come in the pools' face lists (stateSort), not render maps.
    // A world pool gets a scene pass of its own (a slot, WORLD_SLOTS of them
    // past the alpha passes): its records hold no textures or materials (the
    // pool binds them, every unit keeps the pass's), only the face's buffer
    // number and indices and the region's render matrix (the water none).
    // No events: each main view, postSort hands each world pool's face list
    // (markWorldPool) and endView compares every face of a group with its
    // record; a group whose every record is seen once and still holds is
    // shown to the scene (visible bit, its rows), any other is described
    // again from the faces seen (never shown that view). At the pool's pass
    // entry drawWorldPool draws the pool's pass from the records (on, while
    // the scene draws): its loop then leaves out the faces of the shown groups
    // (worldFaceDrawn); a pass the scene could not draw is the loop's whole.
    // LLRHI_SCENE_WORLD (worldMode): off, verify (shown and compared with the
    // pools' lists every VERIFY_EVERY frames, never left out), auto, on.
    static constexpr U32 WORLD_SLOTS = 20;
    LLRHI::SceneMode worldMode() const { return LLRHI::scenePartNow(LLRHI::sceneWorldMode(), mWorldAuto); }
    // postSort: whether the view takes the world pools (the main view, the switch not off).
    bool worldWanted(U32 view) const;
    // postSort, before endView: a world pool's faces this view.
    void markWorldPool(U32 view, LLFacePool* pool);
    // A world pool's loop, as drawWorldPool answered: whether the scene drew
    // its pass this view, and the view's serial its shown groups carry.
    struct WorldDraw
    {
        bool drew = false;
        U32 serial = 0;
        // worldFaceDrawn's cache (the faces of a group come together)
        const void* group = nullptr;
        bool group_drawn = false;
    };
    // The pool's pass entry (its program bound, its units and uniforms set):
    // the scene draws the pass's records, or only chooses them (verify).
    WorldDraw drawWorldPool(LLFacePool* pool);
    // Whether the face's group was drawn by the scene (leave the face out).
    bool worldFaceDrawn(WorldDraw& draw, const LLFace* face) const
    {
        return draw.drew && worldGroupDrawn(draw, face);
    }
    // A face pool goes (~LLFacePool): its slot is free.
    void onWorldPoolGone(const LLFacePool* pool);

private:
    struct InfoState
    {
        U32 record = 0;
        LLSpatialGroup* group = nullptr;
        U32 pass = 0;                       // its render type (an alpha record's: its alpha pass once registered)
        bool alpha = false;                 // research/205 3a-3: an alpha record (its group is the group's alpha group)
        bool list = false;                  // research/205 H5: its record has the batch's texture list
        U32 glow_record = 0;                // research/205 (vk-s3a6): its glow pass's record (0: none)
        bool pending = false;
        U8 refused = REFUSED_COUNT;     // why it has no record (REFUSED_COUNT: it has one, or waits)
        const LLMatrix4* model = nullptr;
        const LLMatrix4* texture = nullptr;
        U32 model_slot = 0;
        U32 texture_slot = 0;
        U32 textures[SCENE_TEX_USED] = {};  // texture numbers (the sampler index)
        U64 desc_hash = 0;                  // of what the record was written from (describe)
        U16 retry_gap = 0;                  // a soft refusal's next wait, in RETRY_EVERY rounds
        U32 retry_frame = 0;                // tried again from this frame on
        // research/219: a rigged record's avatar and skin (its pin on the
        // skin's row: by the hash, the palette cache's key) and its row.
        LLPointer<LLVOAvatar> avatar;
        const LLMeshSkinInfo* skin = nullptr;
        U64 skin_hash = 0;
        U32 palette = LLRHI::SCENE_NO_PALETTE;
    };

    struct GroupState
    {
        U32 id = 0;
        U32 records = 0;
        U32 pending = 0;
        U32 refused = 0;
        std::vector<const LLDrawInfo*> infos;
        // research/198 5.5: the textures of its scope batches (their draw
        // infos hold them; 5.6: the normal and specular maps of the passes
        // that bind them), each with its pass, made when it is first cut,
        // again after its batches changed: marked active each frame it is
        // cut, as the usual way's draws mark them (LLTexUnit::directTexture).
        struct PassTexture
        {
            U32 pass = 0;
            LLViewerTexture* texture = nullptr;
            bool diffuse = false;           // research/206 5c: a batch's diffuse texture (listTextures)
        };
        std::vector<PassTexture> textures;
        bool textures_known = false;
        // research/218 vk-s4b2: a worn object's group (an attachment's or
        // animesh's partition), and the avatars wearing its scope batches
        // (mWireOwner, each once), listed with the textures.
        bool worn = false;
        std::vector<const LLVOAvatar*> wearers;
        // research/219: its rigged records' skins (avatar, skin, row; each
        // once), listed with the textures. The records hold the avatars.
        struct Skin
        {
            LLVOAvatar* avatar = nullptr;
            const LLMeshSkinInfo* skin = nullptr;
            U32 base = 0;
        };
        std::vector<Skin> skins;
        U32 cut_serial = 0;                 // research/205 3a-3: the alpha pass it was marked visible in
        U32 alpha_passes = 0;               // research/205 3a-4: its records' alpha passes (bits by index), with the textures' list
        // research/205 3a-5: an alpha group's spans, the record at each place
        // of its draw map (0: none) and the places (0: no spans, the loop
        // draws it), made again when its records change (spans_known).
        std::vector<AlphaSpan> spans;
        std::vector<U32> place_records;
        std::vector<AlphaSpan> glow_spans;      // research/205 (vk-s3a6)
        std::vector<U32> place_glow_records;
        U32 places = 0;
        bool spans_known = false;
        // research/209 S5: the transform and texture transform rows its
        // records read (not 0), made with the textures' list.
        std::vector<U32> model_slots;
        std::vector<U32> uv_slots;
        bool slots_known = false;
    };

    struct MatrixRow
    {
        U32 slot = 0;
        U32 users = 0;
        F32 words[16] = {};     // what the row holds (verify compares the matrix with it)
    };

    // Rows by matrix address, and rows whose matrix was freed (by slot).
    struct MatrixRows
    {
        explicit MatrixRows(bool is_uv) : uv(is_uv) {}
        const bool uv;
        std::unordered_map<const LLMatrix4*, MatrixRow> live;
        std::unordered_map<U32, U32> orphans;   // slot -> users
    };

    struct ViewInput
    {
        LLRHI::SceneViewDesc desc;
        std::vector<U32> bits;
    };

    bool markVisibleInView(U32 view, const LLSpatialGroup* group);
    // The textures of the group's scope batches (GroupState::textures), made
    // again when they are not known.
    void listTextures(GroupState& g);
    // research/219: whether every skin of the group's rigged records is ready
    // to be drawn from its row now (LLVOAvatar::rhiScenePalette; a row that
    // changed queues its records to be described again); counted in reasons
    // (by LLVOAvatar::RHIScenePalette).
    bool readySkins(GroupState& g, U64* reasons);
    // research/219 (8.2): the main view's G-buffer pass marks skin: whether
    // each record of the group a cut pass of the view draws has the mark its
    // pass draws with (rigged: the plan's avatar mark; else none).
    bool marksFit(const GroupState& g, U32 view) const;
    // research/209: whether the probe faces or mirrors of the view's kind are
    // the scene's at all (wanted: the probes with LLRHI_SCENE_PROBE not off),
    // and cut while the scene draws (the probes: LLRHI_SCENE_PROBE=on;
    // research/218 vk-s2f: or its auto, probeMode).
    bool viewWanted(U32 view) const;
    bool viewCuts(U32 view) const;
    // research/209 S4: the texture matrices' rows (and the samplers) caught
    // up with the idle, once a frame: at beginFrame, or at the first probe
    // face or mirror before it.
    void catchUp();
    void listSlots(U32 view, GroupState& g);
    const LLGLSLShader* alphaDrawnByScene(const LLDrawInfo& info) const;
    bool glowDrawnByScene(const LLDrawInfo& info) const;
    // research/205 (vk-s3a6): the glow record's description from its batch's record's.
    static bool describeGlow(const LLRHI::SceneRecordDesc& desc, LLRHI::SceneRecordDesc& glow);
    void dropGlow(InfoState& state);
    // research/205 3a-5: the group's spans as its draw map and records are
    // now (into the vectors given), and the cached ones checked against them.
    // vk-s3a6: and its glow spans and glow records (when asked for).
    void makeAlphaSpans(const LLSpatialGroup* group, std::vector<AlphaSpan>& spans, std::vector<U32>& records, U32& places,
                        std::vector<AlphaSpan>* glow_spans = nullptr, std::vector<U32>* glow_records = nullptr) const;
    void checkAlphaSpans(const LLSpatialGroup* group, GroupState& g);
    const std::vector<LLDrawInfo*>& drawPassInView(U32 pass);
    // research/198 5.5: the locations the scope pass's program reads (0: none, or not loaded).
    static U32 passAttributes(U32 pass);
    // research/198 5.6: the program the scope pass's pool binds for it (null: not a scope pass).
    static LLGLSLShader* passProgram(U32 pass);
    // The material textures (SceneMaterialTexture bits) its draws bind.
    static U32 passTextures(U32 pass);
    // The material uniforms its pool sets per batch.
    static const LLRHI::SceneMaterialUniform* passMaterials(U32 pass, U32& count);
    GroupState* groupState(LLSpatialGroup* group, bool create, bool alpha = false);
    GroupState* groupOf(const InfoState& state) { return groupState(state.group, false, state.alpha); }
    void clearGroup(GroupState& g);
    bool residentState(const GroupState& g) const { return g.records && !g.pending && !g.refused; }
    // The record's description without its rows (transform slots 0, group 0)
    // and its hash; false and why when the batch cannot have one.
    static bool describe(const LLDrawInfo& info, LLSpatialGroup* group, U32 pass, LLRHI::SceneRecordDesc& desc, Refusal& why);
    static U64 descHash(const LLRHI::SceneRecordDesc& desc);
    bool acquireMatrix(MatrixRows& rows, const LLMatrix4* matrix, U32& slot);
    void releaseMatrix(MatrixRows& rows, const LLMatrix4* matrix, U32 slot);
    void freeMatrix(MatrixRows& rows, const LLMatrix4* matrix);
    void matrixWritten(MatrixRows& rows, const LLMatrix4* matrix);
    U32 sweepRows(MatrixRows& rows);
    bool matrixStale(const MatrixRows& rows, const LLMatrix4* matrix, U32 slot) const;
    void registerInfo(const LLDrawInfo* info);
    void dropInfo(const LLDrawInfo* info, InfoState& state, bool keep_in_group);
    void demote(const LLDrawInfo* info, InfoState& state, Refusal why);
    void indexTextures(const LLDrawInfo* info, const InfoState& state, bool add);
    void samplersChanged();
    void refuse(Refusal why, GroupState* group, InfoState* state, const LLDrawInfo* info = nullptr);
    static bool softRefusal(Refusal why);
    void retryRefused();
    void disable(const char* why);
    void noteDrawn(U32 view, U32 pass, const LLDrawInfo& info);
    void endPassVerify(U32 view, U32 pass);
    void noteDifference(const char* what, U32 view, U32 pass, U32 count);
    // research/218 vk-s2f: a check of a part with an auto of its own (the
    // alpha view, the probe faces) in this frame; logs a change.
    void partChecked(LLRHI::SceneAuto& progress, LLRHI::SceneMode asked, const char* part, bool differ, const char* why);
    std::string takeReport(U32 frames);

    std::unordered_map<const LLDrawInfo*, InfoState> mInfos;
    std::unordered_map<const LLSpatialGroup*, GroupState> mGroups;
    std::unordered_map<const LLSpatialGroup*, GroupState> mAlphaGroups;    // research/205 3a-3
    std::unordered_multimap<U32, const LLDrawInfo*> mByTexture;    // texture number -> batches with records
    MatrixRows mTransforms{ false };
    MatrixRows mUVTransforms{ true };
    std::vector<const LLDrawInfo*> mPending;
    std::vector<const LLDrawInfo*> mRetry;     // refused softly since the last retry (softRefusal)
    std::vector<U32> mSamplerScratch;
    U32 mGroupSpan = 0;                     // the largest group id given out
    ViewInput mViews[LLRHI::SCENE_VIEW_COUNT];
    bool mDisabled = false;
    bool mListening = false;
    static U32 sViewOverride;               // research/209: ViewScope's (U32_MAX: none)
    U32 mCaughtUp = U32_MAX;                // research/209 S4: LLFrameTimer's frame count caught up in
    // research/209: per view (the probe faces and mirrors, face after face).
    bool mViewTurn[LLRHI::SCENE_VIEW_COUNT] = {};       // the backend's turn (SceneDrawContext::verify_view)
    bool mViewCompare[LLRHI::SCENE_VIEW_COUNT] = {};    // ... and the viewer's compare (nothing cut)
    U64 mViewPassVerified[LLRHI::SCENE_VIEW_COUNT][(LLRHI::SCENE_MAX_PASSES + 63) / 64] = {};
    // research/209 S5: the rows the view's visible groups' records read (0 first), each once.
    std::vector<U32> mViewTransforms[LLRHI::SCENE_VIEW_COUNT];
    std::vector<U32> mViewUVs[LLRHI::SCENE_VIEW_COUNT];
    std::vector<U32> mTransformListed;      // by slot: the view serial that listed it
    std::vector<U32> mUVListed;
    U32 mViewSerial = 0;

    // research/198 5.5: drawing. Per view this frame: the passes postSort
    // cuts, and the groups it cut them of (for the caller's draw when the
    // scene cannot).
    U64 mCutPasses[LLRHI::SCENE_VIEW_COUNT][(LLRHI::SCENE_MAX_PASSES + 63) / 64] = {};
    std::vector<const LLSpatialGroup*> mCutGroups[LLRHI::SCENE_VIEW_COUNT];
    // The texture numbers those groups' scope batches sample, by pass (pass << 32 | number).
    std::vector<U64> mViewTextures[LLRHI::SCENE_VIEW_COUNT];
    bool mViewTexturesSorted[LLRHI::SCENE_VIEW_COUNT] = {};
    std::vector<U32> mPassTextures;         // drawPass's: the numbers of its pass
    std::vector<LLDrawInfo*> mRescue;
    LLRHI::DirectDraw::Attribute mLayout[LLRHI::DirectDraw::MAX_ATTRIBUTES];
    U32 mSeenProblems = 0;                  // the backend's problems() last seen
    bool mProblemLogged = false;

    // Verify.
    U32 mFrame = 0;
    bool mVerifyFrame = false;
    bool mFrameDiffered = false;
    U32 mCleanFrames = 0;
    U64 mPassVerified[(LLRHI::SCENE_MAX_PASSES + 63) / 64] = {};   // this verified frame's main view passes
    std::vector<U32> mOldDrawn;
    std::vector<U32> mDrawn;
    std::vector<U32> mBounded;
    U32 mLogged = 0;
    bool mDrawFailedLogged = false;
    // research/218 vk-s2f: the alpha view's and the probe faces' autos.
    LLRHI::SceneAuto mAlphaAuto;
    LLRHI::SceneAuto mProbeAuto;

    // Since the last report.
    F64 mReportAt = 0.0;
    U32 mReportFrames = 0;
    U32 mRefusals[REFUSED_COUNT] = {};
    U32 mCreated = 0, mRegistered = 0, mDropped = 0, mDeferred = 0, mRedescribed = 0, mRetried = 0;
    U32 mSweptTransforms = 0, mSweptUV = 0;     // rows the frame's sweep found changed (model: without an event)
    U32 mVerifiedPasses = 0, mDifferences = 0, mContentDiffers = 0, mStaleMatrices = 0;
    U64 mMapBatches = 0, mMapScene = 0;     // verified passes: render map batches, those of resident groups' records
    U32 mDrawFailed = 0;
    U64 mCutGroupCount = 0, mUncutResident = 0, mRescued = 0, mScenePasses = 0;   // research/198 5.5 (main view)
    // research/218 vk-s4b2: the worn objects' groups cut (every view), and the groups the scene could
    // draw that a wearer's GrayWire, the skin mark or LLRHI_SCENE_ATTACHMENTS=verify kept whole.
    U64 mWornCut = 0, mWornKept = 0;
    bool mSkinMarking = false;              // the main view's G-buffer pass marks skin this frame (beginView)

    // research/219: the rigged records.
    static constexpr U32 PALETTE_REASONS = 6;   // LLVOAvatar::RHI_PALETTE_REASONS (checked in the .cpp)
    LLRHI::SceneAuto mRiggedAuto;
    std::vector<const LLDrawInfo*> mSkinRedescribe;     // records whose row changed: described again at the frame's head
    bool mRiggedCuts[LLRHI::SCENE_VIEW_COUNT] = {};     // the view cuts a rigged pass (beginView)
    // The main view's plan of the skin mark (plus::skinsss::MarkPlan, beginView).
    bool mMarkPlanMarking = false, mMarkPlanAvatarBit = false;
    U32 mMarkPlanAvatarMark = 0;
    U64 mRiggedGroupsCut = 0, mRiggedKeptForMark = 0, mMarkPredictedWrong = 0, mSkinRedescribed = 0;
    U64 mSkinsNotReady[PALETTE_REASONS] = {};           // the views', by reason (shadows' apart)
    U64 mShadowSkinsNotReady[PALETTE_REASONS] = {};
    U32 mRiggedVerifiedPasses = 0, mRiggedDifferences = 0;
    U64 mRiggedBatchesCompared = 0;
    // research/219 (9.3): the verified frames' palette rows against the palettes.
    static constexpr U32 PALETTE_CHECKS = 32;   // a verified frame
    U32 mPaletteChecks = 0, mPaletteLogged = 0;
    U64 mPaletteRowsCompared = 0, mPaletteRowsDiffer = 0;
    // research/209: the probe faces and mirrors (by view).
    U32 mViewFaces[LLRHI::SCENE_VIEW_COUNT] = {}, mViewPasses[LLRHI::SCENE_VIEW_COUNT] = {}, mViewCompared[LLRHI::SCENE_VIEW_COUNT] = {};
    U64 mViewCutGroups[LLRHI::SCENE_VIEW_COUNT] = {}, mViewUncut[LLRHI::SCENE_VIEW_COUNT] = {};

    // research/205 3a-3: the alpha view of the pass open now.
    U32 mMainViewFrame = U32_MAX;           // mFrame of the main view's last beginView
    bool mAlphaOpen = false;
    bool mAlphaSorted = false;              // research/205 3a-5: the sorted way
    bool mAlphaAnyDrawn = false;            // some pass of it drawn (or, sorted, expanded)
    U32 mAlphaSerial = 0;
    U32 mAlphaPassMask = 0;                 // research/205 3a-4: the visible groups' records' alpha passes (bits by index)
    bool mAlphaPassDrawn[ALPHA_PASSES] = {};
    ViewInput mAlphaView;
    std::vector<U32> mAlphaPriority;        // by group slot: the OIT priority this frame (visible groups)
    std::vector<U32> mAlphaBase;            // research/205 3a-5: by group slot, its first command in the sorted arrays
    U32 mAlphaCommands = 0;                 // ... the arrays' commands (the visible groups' places)
    std::vector<U64> mAlphaTextures;        // pass << 32 | texture number, of the visible groups' records
    std::vector<AlphaSpan> mSpanScratch;    // checkAlphaSpans'
    std::vector<U32> mSpanRecordScratch;
    std::vector<AlphaSpan> mGlowSpanScratch;
    std::vector<U32> mGlowRecordScratch;
    U64 mAlphaPasses = 0, mAlphaGroupsCut = 0, mAlphaUncut = 0, mAlphaDrawnPasses = 0, mAlphaFailed = 0;
    U64 mSortedPasses = 0, mSpansDrawn = 0, mSpanBatches = 0, mSpansLoop = 0, mSpanChecks = 0, mSpanDiffers = 0;   // research/205 3a-5
    U64 mGlowSpansDrawn = 0, mGlowSpansLoop = 0;   // research/205 (vk-s3a6): the sorted way's glow spans

    // research/204 10 (vk-s4c2): the world pools.
    struct WorldSlot
    {
        const LLFacePool* pool = nullptr;   // null: free (compared only, never read)
        U32 users = 0;                      // records of its pass
        const LLGLSLShader* program = nullptr;  // what its pool bound at its last pass entry
        U32 attributes = 0;                 // the locations that program reads (the records' range.attributes)
        U32 serial = 1;                     // bumped when the attributes or the pool change (records of another: described again)
        U32 marked_frame = U32_MAX;         // the main view's face list as markWorldPool saw it
        U32 marked_view = 0;
        LLFace* const* faces = nullptr;
        size_t face_count = 0;
        U32 shown_faces = 0;                // of that list, the faces of shown groups
        U32 verified_frame = U32_MAX;
    };
    struct WorldFace
    {
        const LLFace* face = nullptr;
        U32 record = 0;                     // 0: draws nothing (no buffer, no indices)
        U8 slot = 0;
        U32 slot_serial = 0;
        U32 buffer = 0;
        U32 first = 0;
        U32 count = 0;
        const LLMatrix4* model = nullptr;
        U32 model_slot = 0;
        U32 seen = 0;                       // the view serial it was seen in
    };
    struct WorldGroup
    {
        U32 id = 0;
        std::vector<WorldFace> faces;
        U32 records = 0;
        U32 cursor = 0;                     // where the next face is looked for first
        U32 touched = 0;                    // the view serial its faces were walked in
        U32 seen = 0;                       // faces matched this view
        bool broken = false;                // a face this view not as its record (or not one of them, or twice)
        U32 shown = 0;                      // the view serial it was shown to the scene in
        std::vector<const LLFace*> walked;  // this view's faces of it, in the lists' order (to describe it again from)
        U32 retry_frame = 0;
        U16 retry_gap = 0;
        U32 described_frame = 0;
        std::vector<std::pair<const LLMatrix4*, U32>> models;   // its rows (each once)
    };
    static U32 worldPass(U32 slot);
    WorldSlot* worldSlotOf(const LLFacePool* pool);
    S32 worldTakeSlot(const LLFacePool* pool);
    // The face as a world record would draw it (false: its pool is no world pool or has no slot).
    bool worldFaceNow(const LLFace* face, WorldFace& out);
    void describeWorld(WorldGroup& g);
    void dropWorld(WorldGroup& g);
    void decideWorld(U32 view);
    bool worldGroupDrawn(WorldDraw& draw, const LLFace* face) const;
    void worldVerify(U32 slot, LLFacePool* pool);
    void worldChecked(bool differ, const char* why);
    std::unordered_map<const LLSpatialGroup*, WorldGroup> mWorldGroups;
    std::vector<const LLSpatialGroup*> mWorldTouched;   // this view's (endView)
    WorldSlot mWorldSlots[WORLD_SLOTS];
    U32 mWorldSerial = 0;                   // the main view's serial (markWorldPool, endView, drawWorldPool)
    U32 mWorldBudget = 0;                   // records described this frame
    LLRHI::SceneAuto mWorldAuto;
    std::vector<U32> mWorldExpected;        // worldVerify's
    U64 mWorldShown = 0, mWorldBroken = 0, mWorldDescribed = 0, mWorldRefused = 0, mWorldWaiting = 0, mWorldUnplaced = 0;
    U64 mWorldPasses = 0, mWorldFacesDrawn = 0, mWorldFailed = 0, mWorldNoSlot = 0, mWorldProgramChanged = 0;
    U32 mWorldVerified = 0, mWorldDiffer = 0, mWorldLogged = 0;
};

extern LLSceneRecords gSceneRecords;

// ==== scene glow span walk: begin ====
// research/205 (vk-s3a6): a group's glow at its end in the sorted way, in
// the loop's order. places: the place in the group's draw map of each glow
// draw the loop has (ascending, as the loop pushed them); spans: the group's
// glow spans (ascending, apart). A span is tried at the first glow draw
// inside it: drawn (draw_span true), every glow draw inside it is left out;
// else they are drawn one by one (draw_one) in their places, as is every
// glow draw outside the spans. Each glow draw ends up drawn once, by the
// span or by itself, in the loop's order.
template <class Span, class DrawSpan, class DrawOne>
void glow_walk(const U32* places, U32 count, const Span* spans, U32 span_count, DrawSpan draw_span, DrawOne draw_one)
{
    U32 next = 0;
    U32 covered = 0;    // the places below this were drawn with their span
    for (U32 i = 0; i < count; ++i)
    {
        const U32 place = places[i];
        if (place < covered)
        {
            continue;
        }
        while (next < span_count && (U32)spans[next].first + spans[next].count <= place)
        {
            ++next;
        }
        if (next < span_count && spans[next].first <= place)
        {
            const Span& span = spans[next++];
            if (draw_span(span))
            {
                covered = (U32)span.first + span.count;
                continue;
            }
        }
        draw_one(i);
    }
}
// ==== scene glow span walk: end ====

#endif // LL_LLSCENERECORDS_H

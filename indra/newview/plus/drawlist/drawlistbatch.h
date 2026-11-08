/**
 * @file drawlistbatch.h
 * @brief Draw lists for the draw pools (research/137 N2): a batch
 *        (LLDrawInfo) made into a draw list's Draw, the values
 *        LLRenderPass::pushDirectBatch hands over one by one. Shared by the
 *        opaque loops (plus/drawlist), the alpha pool (N3) and the legacy
 *        material and bump pools (N4).
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

#ifndef PLUS_DRAWLIST_DRAWLISTBATCH_H
#define PLUS_DRAWLIST_DRAWLISTBATCH_H

#include "stdtypes.h"
#include "llrhidrawlist.h"

#include <vector>

class LLDrawInfo;
class LLGLSLShader;
class LLGLTexture;
class LLStaticHashedString;
class LLVOAvatar;
class LLMeshSkinInfo;
class LLMatrix4;

// Two layers (research/140 section 7: the second can later run on the cull
// threads, one chunk per thread, research/137 N11):
//
// 1. BatchList (main thread): the loop's side. Opens the list, keeps the
//    uniforms the loop sets (patched into the next draw when they changed,
//    the viewer's cache brought to the last value after each submission:
//    promise 4), prepares the skinning palettes before the loop, sends a
//    batch the list cannot take the old way (drawOld), and applies the
//    texture marks (setActive) the second layer noted.
// 2. makeDraw (pure): one batch to one Draw from a BatchContext captured when
//    the list opened, into any DrawSink (the Builder's arrays, or a chunk's
//    LLRHI::DrawList::Arrays). It reads the batch, its buffer, its textures
//    and its material only; no global state, no LLRHI hook, no write but the
//    sink, the caches it is given and the list of textures to mark.
//
// How a pool loop uses it (drawlist.cpp has the examples):
//
//     BatchList& list = BatchList::get();
//     if (!list.open("opaque.pushBatches", LLRHI::DrawList::KIND_OPAQUE, needs))
//         return false;                               // the official loop runs
//     (rigged loops: list.preparePalette(p) for every batch first, in order)
//     for (each batch p)
//     {
//         list.uniform(...);                          // what the loop sets per batch (setMinimumAlpha ...)
//         const bool in_list = list.takes(p);
//         if (rigged && !list.palette(p, in_list)) continue;   // uploadMatrixPalette said no
//         if (!in_list || !list.add(p, ...))
//             list.drawOld([&] { the official per-batch call, e.g. pass.pushBatch(p, true, batch_textures); });
//     }
//     list.close();
//
// What goes the old way (the main thread, as today): a mapped or missing
// vertex buffer, a batch plus/skinsss marks (it sets the stencil reference
// through GL), a texture without a GL name yet or with sampler options to
// apply (directTexture would bind it through GL), and what the backend
// cannot take in a list (Builder::addDraw false).
namespace plus::drawlist
{
    // Textures a draw names (pushDirectBatch's texture / unit_textures).
    enum class Textures
    {
        NONE,   // pushUntexturedBatch, drawGeometry: the units as bound, no texture matrix
        BATCH,  // pushBatch: the batch's texture on unit 0 (with its texture matrix), or with
                // batch_textures its texture list on units 0.. (KEEP_UNIT for a null entry)
        GLTF,   // pushGLTFBatch's direct draw: the material's textures on the units the
                // program samples them on (LLFetchedGLTFMaterial::bindDirect), the texture matrix
        UNITS,  // the caller's names per unit (readTextureName), with the batch's texture matrix
    };

    // Uniforms a loop needs to patch: open() checks the program has a native
    // slot for each of them it has (else the official loop runs).
    struct Needs
    {
        static constexpr U32 MAX_UNIFORMS = 12;
        U32 uniforms[MAX_UNIFORMS] = {};    // LLShaderMgr uniform indices
        U32 uniform_count = 0;
        bool rigged = false;                // skinning palettes (the palette uniform, plusHairSway)

        Needs& uniform(U32 index);
        Needs& skinned() { rigged = true; return *this; }
    };

    // Per draw, besides what the batch gives (State's blend and cull, the OIT priority).
    struct Extras
    {
        U32 blend[4] = { LLRHI::DrawList::State::INHERIT, LLRHI::DrawList::State::INHERIT,
                         LLRHI::DrawList::State::INHERIT, LLRHI::DrawList::State::INHERIT };
        U32 cull = LLRHI::DrawList::State::INHERIT;    // 0: off (LLGLDisable cull_face), GL_BACK, GL_FRONT
        U32 oit_priority = 0;
    };

    // The viewer side's counts since the last takeStats (main thread).
    struct Stats
    {
        U32 loops = 0;              // loops drawn with lists
        U32 draws = 0;              // batches added to lists
        U32 old_mapped = 0;         // batches drawn the old way: no or a mapped vertex buffer
        U32 old_marked = 0;         //   plus/skinsss marks the batch (stencil)
        U32 old_texture = 0;        //   a texture without a GL name yet, or sampler options to apply
        U32 old_refused = 0;        //   the list did not take the draw (Builder::addDraw)
        U32 palette_rows = 0;       // palettes named by row
        U32 palette_patches = 0;    // palettes as a uniform patch
        U32 uniform_patches = 0;    // other uniform patches
        U32 marked_active = 0;      // DELETED textures marked active after their draw
    };
    void takeStats(Stats& out);

    // ------------------------------------------------------------------
    // The pure part

    // What a list's draws are made from, captured on the main thread when the
    // list opens (research/140 2.4); makeDraw reads nothing else global.
    struct BatchContext
    {
        const LLGLSLShader* program = nullptr;  // identity only (the matrix cache key)
        U32 attribute_mask = 0;                 // LLGLSLShader::mAttributeMask
        // The matrix uniforms the program has (as LLRender::fillDirectMatrices asks).
        bool has_normal = false;
        bool has_inverse = false;
        bool has_modelview_projection = false;
        F32 view[16] = {};                      // gGLModelView
        F32 projection[16] = {};                // gGL's projection matrix
        U32 frame = 0;                          // gFrameCount
        S32 palette_slot = -1;                  // the palette uniform's native slot (-1: none)
        // The units the program samples a GLTF material's base color, normal,
        // metallic-roughness and emissive maps on (-1: not sampled).
        S32 gltf_units[4] = { -1, -1, -1, -1 };

        // Fills it for the bound program and the current matrices (main thread).
        void capture(const LLGLSLShader& shader);
    };

    // The matrices pushDirectBatch hands over for a batch with this model
    // matrix (null: none), from the captured context: LLRender::
    // fillDirectMatrices' arithmetic for the matrix uniforms the program has
    // (the others stay 0). Pure (any thread); out.id is left as it is.
    void makeMatrices(const BatchContext& context, const LLMatrix4* model, LLRHI::DrawList::Matrices& out);

    // What makeDraw reuses between draws of one list (or chunk): its entries'
    // indices in the sink. Reset when the sink's arrays are cleared.
    struct BatchCaches
    {
        void reset() { *this = BatchCaches(); }

        // pushDirectBatch's MatrixCache.
        const LLMatrix4* matrix_model = nullptr;
        U32 matrix_entry = LLRHI::DrawList::NONE;
        // The last vertex buffer's attributes.
        const void* attribute_buffer = nullptr;
        U32 attribute_name = 0;
        U32 attribute_first = 0;
        U32 attribute_count = 0;
        // The last texture matrix.
        const LLMatrix4* texture_matrix = nullptr;
        U32 texture_matrix_first = LLRHI::DrawList::NONE;
    };

    // Where makeDraw puts the entries a draw names (the Builder's add*, or an
    // LLRHI::DrawList::Arrays for a chunk). The draw itself it hands back.
    class DrawSink
    {
    public:
        virtual ~DrawSink() = default;
        virtual U32 addAttributes(const LLRHI::DirectDraw::Attribute* attributes, U32 count) = 0;
        virtual U32 addTextures(const U32* names, U32 count) = 0;
        virtual U32 addMatrices(const LLRHI::DrawList::Matrices& matrices) = 0;
        virtual U32 addFloats(const F32* values, U32 count) = 0;
        virtual U32 addPatch(S32 slot, U32 count, U32 components, U32 rows, const F32* values) = 0;
        virtual U32 addState(const LLRHI::DrawList::State& state) = 0;
    };

    // A chunk's arrays as a sink (research/137 N11).
    class ArraysSink : public DrawSink
    {
    public:
        explicit ArraysSink(LLRHI::DrawList::Arrays& arrays) : mArrays(arrays) {}
        U32 addAttributes(const LLRHI::DirectDraw::Attribute* attributes, U32 count) override { return mArrays.addAttributes(attributes, count); }
        U32 addTextures(const U32* names, U32 count) override { return mArrays.addTextures(names, count); }
        U32 addMatrices(const LLRHI::DrawList::Matrices& matrices) override { return mArrays.addMatrices(matrices); }
        U32 addFloats(const F32* values, U32 count) override { return mArrays.addFloats(values, count); }
        U32 addPatch(S32 slot, U32 count, U32 components, U32 rows, const F32* values) override
        {
            return mArrays.addPatch(slot, count, components, rows, values);
        }
        U32 addState(const LLRHI::DrawList::State& state) override { return mArrays.addState(state); }

    private:
        LLRHI::DrawList::Arrays& mArrays;
    };

    // A palette the draw sets before it draws (uploadMatrixPalette's upload):
    // a row of this frame's table (sPlacePalette's) or the joints.
    struct BatchPalette
    {
        U32 row_base = LLRHI::DrawList::NONE;   // Draw::palette_base
        const F32* joints = nullptr;            // else mat3x4 joints for the palette uniform (BatchContext::palette_slot)
        U32 joint_count = 0;
    };

    // A uniform the draw sets before it draws (UniformPatch's values).
    struct BatchPatch
    {
        S32 slot = -1;
        U32 count = 1;
        U32 components = 1;
        U32 rows = 0;
        const F32* values = nullptr;
    };

    // The GL name a list draw samples a texture by, as LLTexUnit::directName
    // reads it. False: none yet, or sampler options to apply (directTexture
    // would bind it through GL: the batch goes the old way). A DELETED
    // texture goes into set_active: its draw marks it active (setActive)
    // once the draw is taken, on the main thread (research/140 2.3; setActive
    // changes nothing on an ACTIVE or NO_DELETE texture).
    bool readTextureName(LLGLTexture* texture, U32& name, std::vector<LLGLTexture*>& set_active);

    enum class Made
    {
        DRAW,           // draw is filled, its entries are in the sink
        OLD_TEXTURE,    // a texture readTextureName refused: the old way
    };
    // The batch's Draw, as pushDirectBatch would hand it over: buffers and
    // range, attributes, textures and texture matrix, the matrices (ids from
    // newMatrixId), palette, State (extras, the palette patch and patches in
    // one run). Pure as the top of this file says, with three exceptions to
    // settle before a worker thread calls it (research/137 N11):
    // newMatrixId (an atomic counter, any thread), and for Textures::GLTF
    // LLFetchedGLTFMaterial::getDirectTextures, which reads
    // LLPipeline::sShadowRender (lists never open in a shadow pass), the
    // white and flat normal stand-ins and the normal map's discard level.
    Made makeDraw(const BatchContext& context, BatchCaches& caches, DrawSink& sink, const LLDrawInfo& params,
                  Textures textures, bool batch_textures, const Extras* extras, const U32* units, U32 unit_count,
                  const BatchPalette* palette, const BatchPatch* patches, U32 patch_count,
                  std::vector<LLGLTexture*>& set_active, LLRHI::DrawList::Draw& draw);

    // ------------------------------------------------------------------
    // The loop's side (main thread)

    class BatchList
    {
    public:
        static BatchList& get();

        // Opens a list for one loop with the bound program (syncing gGL's
        // matrices first, as pushDirectBatch does). False: draw the loop the
        // official way (switch off, mirror mode, no program, a shadow pass, a
        // needed uniform without a native slot, the backend not ready).
        bool open(const char* name, LLRHI::DrawList::Kind kind, const Needs& needs);
        // Submits the draws left (and syncs the viewer's caches); closes.
        void close();
        bool isOpen() const { return mOpen; }

        // Whether the batch can be a list draw (else drawOld): its vertex
        // buffer is there and not mapped, and plus/skinsss does not mark it.
        bool takes(const LLDrawInfo& params);

        // A uniform the loop sets now, by LLShaderMgr index (as uniform1fv ..
        // uniform4fv would, components 1..4, `count` elements) or by name (as
        // uniform1f). Patched into the next draw when it changed.
        void uniform(U32 index, U32 count, U32 components, const F32* values);
        void uniform(U32 index, F32 value) { uniform(index, 1, 1, &value); }
        void uniform(const LLStaticHashedString& name, F32 value);

        // Rigged loops, before the loop: every batch in the loop's order
        // (nulls skipped as the loop skips them). Builds each new palette and
        // places its row now (research/140 7-3), in the order the old loop
        // would, so the rows are the same.
        void preparePalette(const LLDrawInfo& params);
        // In the loop, for the same batches in the same order:
        // uploadMatrixPalette(avatar, skin, lastAvatar, lastMeshId, skipLastSkin).
        // False when it would return false (draw nothing). A new palette goes
        // with the next add(), or with drawOld (the old way, then; pass
        // in_list false when the batch will go the old way anyway).
        bool palette(const LLDrawInfo& params, bool in_list);

        // Adds the batch's draw (a batch with no indices to draw adds none).
        // False: not taken; draw it with drawOld.
        bool add(LLDrawInfo& params, Textures textures, bool batch_textures = false, const Extras* extras = nullptr,
                 const U32* units = nullptr, U32 unit_count = 0);

        // Draws one batch the official way inside the loop: cuts the list,
        // sets the uniforms and the palette set for it the usual way, calls
        // draw(), and lets the list go on after it.
        template <typename F> void drawOld(F&& draw)
        {
            beginOldPath();
            draw();
            endOldPath();
        }
        void beginOldPath();
        void endOldPath();

    private:
        BatchList() = default;

        // A uniform the loop set: its value as the loop set it last (wanted),
        // what the backend has after the draws so far (known), and whether
        // the viewer's cache has to learn it.
        struct Tracked
        {
            static constexpr U32 MAX_FLOATS = 16;
            U32 index = 0;                              // LLShaderMgr index (name null)
            const LLStaticHashedString* name = nullptr; // or the uniform's name (by location)
            S32 location = -1;
            S32 slot = -1;
            U32 count = 1;
            U32 components = 1;
            F32 wanted[MAX_FLOATS] = {};
            F32 known[MAX_FLOATS] = {};
            bool pending = false;       // wanted is not in the backend yet
            bool known_valid = false;   // known is the backend's value
            bool viewer_dirty = false;  // the backend has it, the viewer's cache does not
        };
        Tracked* tracked(U32 index, const LLStaticHashedString* name);
        void setTracked(Tracked& t, U32 count, U32 components, const F32* values);
        void syncTracked();             // the viewer's cache (and the backend) to the wanted values
        static void syncThunk(void* user);

        bool ensureOpen();
        void applyPendingPalette();     // the old way (uploadMatrixPalette)

        bool mOpen = false;
        bool mFailed = false;           // the rest of the loop goes the old way
        const char* mName = "";
        LLRHI::DrawList::Kind mKind = LLRHI::DrawList::KIND_OPAQUE;
        U32 mSerial = 0;                // the builder's list this one is
        LLGLSLShader* mShader = nullptr;
        BatchContext mContext;
        BatchCaches mCaches;
        std::vector<Tracked> mTracked;
        std::vector<LLGLTexture*> mSetActive;

        // preparePalette's answer per batch, palette() takes them in order.
        struct PalettePlan
        {
            const LLDrawInfo* batch = nullptr;
            bool drawn = false;         // uploadMatrixPalette's answer
            bool change = false;        // a new palette is set with this batch
            LLVOAvatar* avatar = nullptr;
            LLMeshSkinInfo* skin = nullptr;
            BatchPalette palette;
            F32 sway = 0.f;             // plus::hairsway::swayIndex of the palette
        };
        std::vector<PalettePlan> mPlans;
        size_t mNextPlan = 0;
        const LLVOAvatar* mPrepAvatar = nullptr;    // preparePalette's lastAvatar, lastMeshId, skipLastSkin
        U64 mPrepMesh = 0;
        bool mPrepSkip = false;

        // The palette set for the batch being added.
        struct PendingPalette
        {
            bool active = false;
            LLVOAvatar* avatar = nullptr;
            LLMeshSkinInfo* skin = nullptr;
            BatchPalette palette;
        } mPending;
    };
}

#endif // PLUS_DRAWLIST_DRAWLISTBATCH_H

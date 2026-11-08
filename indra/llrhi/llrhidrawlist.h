/**
 * @file llrhidrawlist.h
 * @brief Draw lists (research/137 N1, API v0): one draw pool loop's draws
 *        handed to the backend as values in one call.
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

#ifndef LL_LLRHIDRAWLIST_H
#define LL_LLRHIDRAWLIST_H

#include "stdtypes.h"
#include "llrhi.h"

#include <string>
#include <vector>

// Research/137 section 2. A draw pool loop (LLRenderPass::pushBatches and
// friends, the alpha pool, the legacy material and bump pools) builds its
// draws as a list of values (what pushDirectBatch hands sDirectDraw one by
// one) and hands the list over in one call. The backend resolves the state
// (program, draw target, blend, depth, units, uniforms) once at the head of
// the list and records each draw from its values only.
//
// The promises (research/137 section 2.3; N1, N2 to N4 and N7 keep them):
//  1. Values only. Once sDrawList returns, the viewer may overwrite every
//     array of the list. No LLPointer or LLDrawInfo pointers in it.
//  2. Order. The list's draws are recorded in the order written. While a list
//     is open, a call of any other LLRHI hook (state, uniforms, textures,
//     draw target, another draw, GPU marks, buffer and texture contents ...)
//     first submits the list (the guard in llrhidrawlist.cpp: "cut"). Only
//     lookups that change no state do not cut (the guard's KEEP table:
//     sPlacePalette, sTableRows, sUniformSlot, sShaderProgram, reports).
//     The guard watches the calling thread's list only: hooks called from
//     other threads (texture threads) never cut, as with direct draws today.
//     Opening a list submits the one still open first.
//  3. State. A list's draws draw with the state at the head of the list
//     (the state as the viewer set it: at the open, or after a hook changed
//     it mid-list, which cut). Per draw only the Draw fields may change:
//     buffers, attributes, textures, matrices, texture matrix, palette row,
//     State (blend, cull, uniform patches) and the OIT priority.
//  4. State afterwards. After each submission the backend's program uniforms
//     and palette are as the old path leaves them after the last draw; blend
//     factors and culling go back to the head's (what the viewer's caches
//     still say: LLRender's blend, LLGLState). The owner brings the uniform
//     caches (LLGLSLShader::mValue) to the last values once the usual way
//     after each submission (Builder's Sync callback; the backend already has
//     the same values, so nothing changes there), and sets the blend it wants
//     after the list the usual way. Sync must not set blend or culling (a cut
//     may run inside LLRender::blendFunc, between its cache and its hook).
//     Direct draws change neither unit bindings nor gGL's matrices, so those
//     need nothing. Before a draw goes the old way (addDraw false) the list
//     is cut first, so that draw meets the caches and the backend agreeing.
//  5. Refusal. sDrawList false: nothing was recorded. True: every draw was
//     handled, in order: never handed back, which would break the order. (A
//     draw the backend cannot record after all, an owner's bug such as an
//     index out of its array, is skipped and counted, as a direct draw the
//     backend skips.) The Builder asks sDrawListReady when it opens a list
//     (and after each cut), and keeps out the draws sDirectDraw would refuse
//     (Arrays::addDraw), so a list it submits is not refused.
//  6. Not in v0 (the Builder does not open, or the backend is not ready):
//     probe faces drawn by the GPU list (rhiProbeDraw took them), ProbeList
//     captures, depth-only passes (shadows are native packets), inside an
//     occlusion query, the alpha census and the alpha GPU timing segments
//     (sAlphaCensus, sGPUTimingAlpha), and batches whose plus hooks change
//     state inside the loop (plus/skinsss's BatchScope: each owner checks).
//
// While a list is open these plain flags must not change (they are not hooks,
// so they do not cut): sDepthOnlyPass, sOcclusionScope, sCensusTag,
// sSuppressTranslation, sOITPriority (per draw: Draw::oit_priority instead),
// sGPUScope, sShadowDepthBias. The GL entry points (LLGLSLShader without a
// native program, gGL.flush() of immediate mode, glStencil*) cannot be
// wrapped by the guard: they cut the calling thread's list themselves
// (LLVKGL_REDIRECT, research/137 N7). The backend keeps a stamp of the state
// it said ready for (Context::stamp, List::stamp) and counts and logs a list
// whose state changed anyway. ProbeList and ShadowList methods are not hooks
// either: owners do not call them with a list open.
namespace LLRHI
{
namespace DrawList
{
    constexpr U32 NONE = 0xffffffffu;

    // The switches (research/137 section 4.1). Read at startup from
    // LLRHI_DRAW_LISTS (0 off, 1 the light path, 2 the reference path: each
    // draw made into a DirectDraw and recorded by the backend's direct_draw)
    // and LLRHI_DRAW_LISTS_OPAQUE / _ALPHA / _MATERIAL (0 / 1). Off by default.
    // Main thread, between frames; the cue toggles (LLRHI::pollCueToggles)
    // drawlists, drawlistsref (1 <-> 2), drawlistsopaque / alpha / material.
    extern U32 sMode;
    enum Kind
    {
        KIND_OPAQUE = 0,    // LLRenderPass's batch loops (N2)
        KIND_ALPHA,         // LLDrawPoolAlpha (N3)
        KIND_MATERIAL,      // LLDrawPoolMaterials, LLDrawPoolBump (N4)
        KIND_COUNT
    };
    extern bool sKind[KIND_COUNT];
    // sMode != 0 and the kind's switch is on (a cheap test before any work).
    inline bool wanted(Kind kind)
    {
        return sMode != 0 && kind < KIND_COUNT && sKind[kind];
    }

    // research/137 N11: the cull threads (LLRHI::sPostWork) take work off the
    // thread that builds and records the lists. Read at startup from
    // LLRHI_DRAW_LISTS_THREADS (bits; 0, off, by default: everything as
    // before); the cue drawliststhreads.toggle turns it off and back on.
    //  THREADS_BUILD: a long legacy material loop (plus/materiallist) is
    //    made into draws on a cull thread, a chunk at a time, while the main
    //    thread submits the chunks made before (the same draws in the same
    //    order: one chunk = one list, Builder::addChunk).
    //  THREADS_RECORD: the backend records a long list's draws in segments:
    //    it resolves each draw on its own thread as before and a cull thread
    //    records the segment's commands into a spliced stream
    //    (LLVKRecorder), with the same decisions (llvkdrawlist.cpp).
    // Main thread (the cue: between frames). LLRHI_DRAW_LISTS=1 only (the
    // reference path stays as it is).
    enum : U32
    {
        THREADS_BUILD = 1,
        THREADS_RECORD = 2,
    };
    extern U32 sThreads;

    // State a draw may change. List::states holds them and Draw::state picks
    // one. Lists other than alpha have one: everything INHERIT (the Builder
    // puts that one at index 0 of every list, so Draw::state 0 means it).
    struct State
    {
        static constexpr U32 INHERIT = 0xffffffffu;
        // GL blend factors in glBlendFuncSeparate's order (src_rgb, dst_rgb,
        // src_alpha, dst_alpha); INHERIT: as at the head of the list (promise
        // 3). Blending itself stays as at the head (enabled or not).
        U32 blend[4] = { INHERIT, INHERIT, INHERIT, INHERIT };
        // 0: no culling, GL_BACK or GL_FRONT: culling on with that face;
        // INHERIT: as at the head of the list.
        U32 cull = INHERIT;
        // Uniform patches applied before the draw: List::patches
        // [first_patch, first_patch + patch_count).
        U32 first_patch = 0;
        U32 patch_count = 0;
        // research/137 N7: the stencil reference value of both faces (what
        // glStencilFunc's ref sets; the test, its function and masks stay as
        // at the head). INHERIT: as at the head of the list. For the batches
        // plus/skinsss marks (its BatchScope sets the ref around the batch).
        U32 stencil_ref = INHERIT;
    };

    // Before the draw, the current program's uniform is set (as LLGLSLShader::
    // uniform* would, through sSetUniform / sSetUniformMatrix). It stays set
    // after the draw (promise 4). Never a sampler (a texture unit: the draw's
    // textures go in Draw::textures) and never slot -1 (the old way then
    // sets the uniform through GL): the backend skips and counts those.
    struct UniformPatch
    {
        S32 slot = -1;          // the program's LLRHI slot (sUniformSlot, LLGLSLShader's native slot of the uniform)
        U16 count = 1;          // elements (array length)
        U8 components = 1;      // 1..4; for a matrix its columns
        U8 rows = 0;            // 0: a vector; 2..4: a matrix with that many rows (column-major, not transposed)
        U32 first_float = 0;    // List::floats index of the values: 4-byte components as sSetUniform takes them (ints bit-copied)
    };

    // What LLRender::fillDirectMatrices makes (the DirectDraw matrices),
    // column-major.
    struct Matrices
    {
        F32 modelview[16];
        F32 modelview_projection[16];
        F32 normal[9];
        F32 inverse_modelview[16];
        // Non-zero: the same id as an earlier entry means the same values
        // (DirectDraw::matrix_id). The backend keeps rows by id across the
        // frame's direct draws and lists: take ids from newMatrixId(), which
        // pushDirectBatch's matrix cache must use too, never a counter of
        // your own.
        U32 id;
    };
    // A matrix id no other matrices of this session have (any thread).
    U32 newMatrixId();

    // One draw: values only (a DirectDraw with its pointers made indices).
    struct Draw
    {
        U32 vertex_buffer = 0;      // GL buffer names
        U32 index_buffer = 0;       // 0: not indexed
        U32 index_type = 0;         // GL_UNSIGNED_SHORT / GL_UNSIGNED_INT
        U32 first = 0;              // first index (not indexed: first vertex)
        U32 count = 0;              // indices (not indexed: vertices)
        U32 mode = 0x0004;          // GL primitive (GL_TRIANGLES)
        U32 buffer_vertices = 0;    // LLVertexBuffer::getNumVerts (0: unknown)
        U32 buffer_indices = 0;     // LLVertexBuffer::getNumIndices
        // List::attributes [first_attribute, first_attribute + attribute_count)
        // (DirectDraw::Attribute). Consecutive draws of one buffer share a range.
        U32 first_attribute = 0;
        U32 attribute_count = 0;
        // List::textures [first_texture, first_texture + texture_count): GL
        // texture names for units 0..texture_count-1, DirectDraw::KEEP_UNIT
        // keeps a unit's binding. NONE / 0: no textures (units as bound).
        U32 first_texture = NONE;
        U32 texture_count = 0;
        // List::matrices index. NONE: the previous draw's (the Builder fills in
        // the index; a list's first draw with NONE has none: the program's
        // matrix uniforms as set, as DirectDraw's null matrix pointers).
        U32 matrices = NONE;
        // List::floats index of 16 floats (DirectDraw::texture_matrix0). NONE: none.
        U32 texture_matrix = NONE;
        // This frame's PaletteRow::base (sPlacePalette) the program reads its
        // skinning palette from from this draw on, as sUsePaletteRow would set
        // it. NONE: the palette stays as it is. Only a row sPlacePalette
        // returned true for, and only when Builder::context() says the program
        // takes rows (Context::palette_rows).
        U32 palette_base = NONE;
        U32 state = 0;              // List::states index
        U32 oit_priority = 0;       // inside an OIT session: as LLRHI::sOITPriority for this draw
        U32 table_base = NONE;      // DirectDraw::table_base (NONE: DirectDraw::NO_TABLE)
        U32 instance_count = 1;
    };

    struct List
    {
        const char* name = "";      // for statistics and GPU segments (a string literal)
        const Draw* draws = nullptr;            U32 draw_count = 0;
        const State* states = nullptr;          U32 state_count = 0;
        const LLRHI::DirectDraw::Attribute* attributes = nullptr; U32 attribute_count = 0;
        const U32* textures = nullptr;          U32 texture_count = 0;
        const Matrices* matrices = nullptr;     U32 matrix_count = 0;
        const UniformPatch* patches = nullptr;  U32 patch_count = 0;
        const F32* floats = nullptr;            U32 float_count = 0;
        U64 stamp = 0;              // Context::stamp of the readiness the draws were added under
    };

    // What the backend tells about the current state when asked whether it
    // takes a list now.
    struct Context
    {
        // The current program reads its skinning palette from this frame's
        // rows (sUsePaletteRow would take a row sPlacePalette placed).
        bool palette_rows = false;
        // Units (bit per unit 0..31) the current program samples through
        // descriptor images: a draw naming its own texture for one of them
        // cannot go into a list (sDirectDraw refuses it too); Builder::addDraw
        // returns false for it.
        U32 descriptor_units = 0;
        // The backend's own: what the state was when it said ready. The
        // Builder hands it back in List::stamp; a different state at
        // sDrawList means something changed it past the hooks.
        U64 stamp = 0;
    };

    // Set by the backend. Records the list's draws in order with the current
    // state (program, draw target, blend, depth, units, uniforms: what
    // sDirectDraw uses). True: every draw handled. False: nothing recorded
    // (the caller draws them the old way). The viewer may change the arrays
    // after it returns.
    extern bool (*sDrawList)(const List& list);
    // Set by the backend. Whether sDrawList would take a list in the current
    // state (not a depth-only pass, not in a ProbeList capture or an
    // occlusion query, a program it records, ...); fills context. False: draw
    // the old way. Changes nothing.
    extern bool (*sDrawListReady)(Context& context);

    // fix-threads4: wraps the hooks of the guard's CUT table (llrhidrawlist.cpp)
    // once every hook is set: by the backend at the end of its install, before
    // other threads call the hooks. Only while lists can open (sDrawList and
    // sDrawListReady set, LLRHI_DRAW_LISTS and a kind on); otherwise the first
    // open wraps them, as before (a list turned on later by a cue). A hook
    // set after that is not wrapped (none is: each is set at install).
    void installGuard();

    // The viewer side's counts since the last takeStats (main thread).
    struct Stats
    {
        U32 lists = 0;              // opened
        U32 submissions = 0;        // sDrawList calls (lists, cuts, chunks) that recorded
        U32 draws = 0;              // draws submitted
        U32 cuts = 0;               // submissions made by the guard
        U32 not_ready = 0;          // open, addDraw or addChunk found the backend not ready
        U32 old_path_draws = 0;     // addDraw returned false (the caller drew them the old way)
        U32 refused = 0;            // sDrawList returned false after sDrawListReady said yes (a bug; the draws are lost)
        // The hooks that cut most, most first (at most 5).
        const char* cut_names[5] = {};
        U32 cut_counts[5] = {};
    };
    void takeStats(Stats& out);

    // research/137 N7: the VKPerf line. An owner (a pool loop's module:
    // plus/drawlist, plus/alphalist ...) may hand in a report of its own
    // counts: in words, per frame over `frames`, resetting them; read on
    // the thread that writes the VKPerf line, inside the present (the
    // owner's thread). Null: none. Set once, at startup.
    typedef std::string (*OwnerReport)(U32 frames);
    void setOwnerReport(Kind kind, OwnerReport report);
    // The viewer side's counts (takeStats: lists, submissions, draws, cuts and
    // the hooks that cut most, not ready, drawn the old way, refused) and the
    // owners' reports, in words, per frame over `frames`; resets them. Empty
    // while LLRHI_DRAW_LISTS is 0 and nothing was counted.
    std::string report(U32 frames);

    // A list's arrays and the add* functions over them, without the guard or
    // a submission: the Builder's own, and what a worker thread fills for a
    // chunk of a long list (research/140 7-5: one chunk = one list, handed to
    // Builder::addChunk on the list's thread). Touches nothing but itself.
    // The add* functions return indices into its arrays; they stay valid
    // until clear().
    class Arrays
    {
    public:
        Arrays() { clear(); }
        // Empties every array (keeping their storage) and puts the INHERIT
        // state at index 0.
        void clear();
        // Empties the draws only (a cut: later draws may still use the other entries).
        void clearDraws() { mDraws.clear(); }

        U32 addAttributes(const LLRHI::DirectDraw::Attribute* attributes, U32 count);  // the last range again when equal
        U32 addTextures(const U32* names, U32 count);
        U32 addMatrices(const Matrices& matrices);      // the last entry again when its id is the same (non-zero)
        U32 addFloats(const F32* values, U32 count);
        // A patch of `count` elements of `components` x max(rows, 1) floats from values.
        U32 addPatch(S32 slot, U32 count, U32 components, U32 rows, const F32* values);
        U32 addState(const State& state);               // the last entry again when equal

        // Appends the draw when a list can take it in `context` (what
        // sDirectDraw would refuse: no vertices or attributes, another index
        // type, its own texture on a descriptor image unit, a palette row the
        // program does not take). False: draw it the old way. Draw::matrices
        // NONE becomes the previous draw's index (the refused ones' too), so
        // a chunk's first draw names its matrices.
        bool addDraw(const Draw& draw, const Context& context);

        bool empty() const { return mDraws.empty(); }
        U32 drawCount() const { return (U32)mDraws.size(); }
        // The arrays as a list (valid until the arrays change).
        List list(const char* name, U64 stamp) const;

    private:
        U32 mLastMatrices = NONE;
        U32 mLastAttributes = NONE, mLastAttributeCount = 0;
        U32 mLastState = 0;
        std::vector<Draw> mDraws;
        std::vector<State> mStates;
        std::vector<LLRHI::DirectDraw::Attribute> mAttributes;
        std::vector<U32> mTextures;
        std::vector<Matrices> mMatrices;
        std::vector<UniformPatch> mPatches;
        std::vector<F32> mFloats;
    };

    // Builds the calling thread's list: arrays kept and reused across frames.
    // Typical use (a pool loop):
    //     DrawList::Builder& list = DrawList::Builder::get();
    //     if (list.open("simple", DrawList::KIND_OPAQUE, &sync, &owner))
    //     {
    //         for (each batch)
    //         {
    //             DrawList::Draw draw;  ... fill it, with addAttributes / addTextures /
    //                                       addMatrices / addFloats / addPatch / addState ...
    //             if (!list.addDraw(draw)) { draw the batch the old way; }
    //         }
    //         list.submit();
    //     }
    //     else { the old loop }
    // The add* functions return indices into the list's arrays. They stay
    // valid until submit() or the next open(), also across cuts (a cut submits
    // the draws so far and keeps the other arrays: later draws may still use
    // their entries).
    class Builder
    {
    public:
        // Called after each submission that recorded draws (the end of the
        // list, a cut, a chunk) so the owner sets its last uniform values the
        // usual way (promise 4; never blend or culling). It may call hooks: no
        // list is open while it runs. The owner notes those values when
        // addDraw takes a draw (not when it adds the patch: a cut may come
        // between addPatch and addDraw).
        typedef void (*Sync)(void* user);

        static Builder& get();

        // Opens a list (submitting one still open). True when the switches
        // allow it, sDrawList is there and the backend is ready now. False:
        // use the old path; nothing is open.
        bool open(const char* name, Kind kind, Sync sync = nullptr, void* user = nullptr);
        bool isOpen() const { return mOpen; }
        // Changes with every open(): lists do not nest (opening one submits
        // the one before), so an owner can tell its list is still the open one.
        U32 serial() const { return mSerial; }
        // What the backend said about the state when the list (or the segment
        // after the last cut) began. A worker filling a chunk takes a copy.
        const Context& context() const { return mContext; }

        U32 addAttributes(const LLRHI::DirectDraw::Attribute* attributes, U32 count) { return mArrays.addAttributes(attributes, count); }
        U32 addTextures(const U32* names, U32 count) { return mArrays.addTextures(names, count); }
        U32 addMatrices(const Matrices& matrices) { return mArrays.addMatrices(matrices); }
        U32 addFloats(const F32* values, U32 count) { return mArrays.addFloats(values, count); }
        U32 addPatch(S32 slot, U32 count, U32 components, U32 rows, const F32* values)
        {
            return mArrays.addPatch(slot, count, components, rows, values);
        }
        U32 addState(const State& state) { return mArrays.addState(state); }

        // Appends the draw. False: not taken (Arrays::addDraw's reasons, or
        // the backend not ready after a cut): draw this batch the old way;
        // the draws before it have been submitted already (promise 4). Later
        // draws may be taken again.
        bool addDraw(const Draw& draw);

        // A chunk a worker filled (with this list's context()): submits the
        // list's draws so far, then the chunk as a list of its own, and calls
        // Sync. False: the backend is not ready (draw the chunk's batches the
        // old way). The list stays open.
        bool addChunk(const Arrays& chunk);

        // Submits the draws not submitted yet, calls Sync, and closes the list.
        void submit();

        // The guard: submits the open list of the calling thread first, if
        // any (`why`: the hook's name, a string literal). Called by the guard's
        // wrappers; owners need not call it.
        static void cut(const char* why);

    private:
        Builder() = default;
        bool send(const List& list, const char* why);   // one sDrawList call and its counts
        void cutHere(const char* why);  // submits the draws so far, keeps the other arrays, calls Sync
        bool askReady();

        bool mOpen = false;
        U32 mSerial = 0;
        bool mNeedReady = false;        // a hook ran: ask the backend again before the next draw
        bool mReady = false;
        Kind mKind = KIND_OPAQUE;
        const char* mName = "";
        Sync mSync = nullptr;
        void* mUser = nullptr;
        Context mContext;
        Arrays mArrays;
    };

    // Opens a list for the scope and submits it at the end of the scope.
    struct Scope
    {
        Scope(const char* name, Kind kind, Builder::Sync sync = nullptr, void* user = nullptr)
            : mList(Builder::get()), mOpen(mList.open(name, kind, sync, user)), mSerial(mList.serial()) {}
        // Submits only its own list (one opened later already submitted it).
        ~Scope() { if (mOpen && mList.isOpen() && mList.serial() == mSerial) mList.submit(); }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        explicit operator bool() const { return mOpen; }
        Builder& mList;
        bool mOpen;
        U32 mSerial;
    };
}
}

#endif // LL_LLRHIDRAWLIST_H

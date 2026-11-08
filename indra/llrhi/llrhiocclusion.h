/**
 * @file llrhiocclusion.h
 * @brief Occlusion boxes in one batch (research/137 N8): the boxes of
 *        LLPipeline::doOcclusion handed to the backend as values in one call,
 *        each begun, drawn and ended as its query there.
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

#ifndef LL_LLRHIOCCLUSION_H
#define LL_LLRHIOCCLUSION_H

#include "stdtypes.h"

#include <atomic>
#include <vector>

// Research/137 N8. LLPipeline::doOcclusion draws a box per spatial group and
// reflection probe between glBeginQuery and glEndQuery (about 700 a frame at
// a heavy place): per box a query begin, two uniforms, a draw and a query end,
// five hook calls, and the backend resolves the program, the target and the
// fan's indices again for each. Here the viewer writes the boxes as values
// (query, center, size, which fans of the cube) into a batch and hands the
// batch over once; the backend resolves the state once and per box begins
// the query, pushes the box and draws it, and ends the query. The results
// read as before (glGetQueryObjectuiv, the same marks and frames, research/37
// and research/118 TT4): one to two frames later.
//
// The promises:
//  1. Values only: once sOcclusionBoxes returns, the viewer may overwrite the
//     arrays.
//  2. Order: the boxes are begun, drawn and ended in the order added. A box
//     drawn the old way while a batch is open is preceded by a cut (Batch::
//     cut): the boxes before it go first. The boxes write neither color nor
//     depth, so the only order that matters is that of the begins (a query
//     name begun twice reads the later box, as with GL).
//  3. State: the boxes draw with the state when the batch is submitted (the
//     viewer submits inside the block that set it up: the box program, color
//     writes off, depth test without writes, no culling). Per box only the
//     Box fields change (query, center, size, fans, depth clamp). Before each
//     submission the viewer brings the program's matrices up to date the
//     usual way (Viewer::prepare: what a box's drawRange would do first).
//  4. State afterwards: no query is open, the box program's box_center and
//     box_size uniforms are as they were (never set by a batch), nothing else
//     changed.
//  5. Refusal: sOcclusionBoxes false means nothing was recorded; the Batch
//     then draws every box the old way, in order (Viewer::old_path).
//
// The results: sOcclusionResult answers glGetQueryObjectuiv's two questions
// (available, result) in one call, the way the two calls would. The batch
// switch turns it on (LLRHI::Occlusion::getQueryObjectuiv).
namespace LLRHI
{
namespace Occlusion
{
    constexpr U32 NONE = 0xffffffffu;

    // LLRHI_OCCLUSION_BATCH: 1 on (default since research/179 s6.5: -0.49 ms
    // a frame at heavy-leeaf-2), 0 off. Read at startup; main thread
    // (occlusionbatch.toggle).
    extern U32 sMode;

    // Where the boxes' marks are stored (research/179 s6.3: the marks into
    // host memory took 2.0 ms of a 5.0 ms GPU frame at light).
    // LLRHI_OCCLUSION_STORE: 1 device-local memory, copied for the CPU at the
    // frame's end (default); 0 host memory, as before; 2 both (the device
    // marks decide, and the report counts the queries whose answer differs
    // from the host marks': it must stay 0). The backend takes it at a frame
    // slot's first box (occlusionstore.toggle: 0 <-> 1, between frames).
    enum : U32 { STORE_HOST = 0, STORE_DEVICE = 1, STORE_VERIFY = 2 };
    extern std::atomic<U32> sStoreMode;

    // One box: a query begun, the cube drawn scaled and moved, the query ended.
    struct Box
    {
        enum : U32
        {
            DEPTH_CLAMP = 1,    // drawn with depth clamping (water: LLGLEnable(GL_DEPTH_CLAMP) around the old draw)
        };
        U32 query = 0;          // GL query name
        U32 target = 0;         // GL_ANY_SAMPLES_PASSED / GL_SAMPLES_PASSED (glBeginQuery's)
        // The cube's fans drawn (index of each fan's first index in Cube::
        // indices, as the old drawRange's offset); [1] NONE: one fan.
        U32 first_index[2] = { NONE, NONE };
        U32 flags = 0;
        F32 center[3] = { 0.f, 0.f, 0.f };  // box_center
        F32 size[3] = { 1.f, 1.f, 1.f };    // box_size
    };

    // The cube the old draws take from LLPipeline::mCubeVB: its corners and
    // its index table of triangle fans (fan_size indices each).
    struct Cube
    {
        const F32* positions = nullptr;     // vertex_count x 3
        U32 vertex_count = 0;
        const U16* indices = nullptr;
        U32 index_count = 0;
        U32 fan_size = 0;
    };

    struct List
    {
        const char* name = "";      // statistics (a string literal)
        const Box* boxes = nullptr;
        U32 box_count = 0;
        Cube cube;
        U32 scope = 0;              // LLRHI::sOcclusionScope of the boxes
        U64 stamp = 0;              // Context::stamp the boxes were added under
    };

    // What the backend tells when asked whether it takes boxes now.
    struct Context
    {
        // The backend's own: what the state was when it said ready. A
        // different state at sOcclusionBoxes means something changed it past
        // the batch (counted and logged; the boxes draw with the state then).
        U64 stamp = 0;
    };

    // The viewer's side, given once (Batch::setViewer).
    struct Viewer
    {
        // Before each submission: the program's matrices the usual way
        // (gGL.syncMatrices). It may call hooks.
        void (*prepare)() = nullptr;
        // Draws one box the old way (begin, uniforms, drawRange, end) when the
        // backend refused a batch. It may call hooks.
        void (*old_path)(const Box& box) = nullptr;
        Cube cube;
    };

    // The viewer side's counts since the last takeStats (main thread).
    struct Stats
    {
        U32 batches = 0;        // opened
        U32 submissions = 0;    // sOcclusionBoxes calls that recorded
        U32 boxes = 0;          // boxes submitted
        U32 cuts = 0;           // submissions before a box drawn the old way
        U32 not_ready = 0;      // open or add found the backend not ready
        U32 refused = 0;        // boxes drawn the old way after sOcclusionBoxes refused them
        U32 results = 0;        // sOcclusionResult calls
        U32 results_kept = 0;   // GL_QUERY_RESULT answered from the call before (getQueryObjectuiv)
    };
    void takeStats(Stats& out);

    // Builds the main thread's batch (arrays kept across frames). Use:
    //     LLRHI::Occlusion::Batch& boxes = LLRHI::Occlusion::Batch::get();
    //     boxes.open("groups");               // false: the old way for every box
    //     ... per box: if (!boxes.add(box)) { boxes.cut(); draw it the old way; } ...
    //     boxes.submit();                     // inside the block, before it restores its state
    class Batch
    {
    public:
        static Batch& get();
        // The viewer's callbacks and cube (once, before the first open).
        static void setViewer(const Viewer& viewer);

        // Opens a batch (submitting one still open). True when the switch
        // is on, the hooks and the viewer's side are there and the backend
        // takes boxes now. Main thread only.
        bool open(const char* name);
        bool isOpen() const { return mOpen; }
        // Appends the box. False: not taken (no batch open, or the backend
        // not ready after a cut): the caller draws it the old way.
        bool add(const Box& box);
        // Before a box drawn the old way while a batch is open: submits the
        // boxes so far (promise 2). The batch stays open; the next add asks
        // the backend again (the old way changes state and comes back).
        void cut();
        // Submits the boxes not submitted yet and closes the batch.
        void submit();

    private:
        Batch() = default;
        void send(bool cut);    // the boxes so far (prepare, one sOcclusionBoxes call, the old way if refused)
        bool askReady();

        bool mOpen = false;
        bool mNeedReady = false;    // something drew the old way: ask the backend again before the next box
        bool mReady = false;
        const char* mName = "";
        U32 mScope = 0;
        Context mContext;
        std::vector<Box> mBoxes;
    };

    // glGetQueryObjectuiv for the occlusion queries (llrhigl.h's), one word
    // different at the call site. With the batch switch on and the backend's
    // sOcclusionResult there, GL_QUERY_RESULT_AVAILABLE reads both answers in
    // one call and keeps the result for a GL_QUERY_RESULT of the same name
    // right after it (LLOcclusionCullingGroup::checkOcclusion and
    // LLReflectionMap::doOcclusion ask in that order, nothing in between).
    void getQueryObjectuiv(U32 name, U32 pname, U32* params);
}

    // Set by the Vulkan backend (research/137 N8); null otherwise.
    // Whether sOcclusionBoxes would take boxes in the current state (the
    // box program bound, color writes off, not in a query, a capture or a
    // depth-only pass, the full occlusion mode ...); fills context. Changes
    // nothing.
    extern bool (*sOcclusionBoxesReady)(Occlusion::Context& context);
    // Records the list's boxes in order, each as its query (begun, drawn,
    // ended), with the current state. True: every box handled (a box that
    // gets no mark reads visible, as an unmarked query). False: nothing
    // recorded.
    extern bool (*sOcclusionBoxes)(const Occlusion::List& list);
    // glGetQueryObjectuiv(name, GL_QUERY_RESULT_AVAILABLE) into *available
    // and glGetQueryObjectuiv(name, GL_QUERY_RESULT) into *result, as the two
    // calls one after the other would answer.
    extern void (*sOcclusionResult)(U32 name, U32* available, U32* result);
}

#endif // LL_LLRHIOCCLUSION_H

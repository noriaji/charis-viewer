/**
 * @file llrhipassstate.h
 * @brief The world passes' declared states (research/204, research/197 stages 3d and 4c).
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

#ifndef LL_LLRHIPASSSTATE_H
#define LL_LLRHIPASSSTATE_H

#include "stdtypes.h"

#include <string>

// A world pass (the terrain, the trees, the water, the sky's parts) says once,
// at its head, the state its draws are drawn with: blending, colour mask,
// depth, culling, stencil, polygon offset, depth clamp, the program and the
// texture units the program reads. That is the pass state: the data a native
// pass would pick its pipeline and descriptor set from, instead of reading
// what the gGL calls before each draw left behind.
//
// The pools still make their own gGL calls in every mode (research/207 4.3:
// the calls are not dropped). LLRHI_PASS_STATE (startup):
//   off     the scope returns at once; the pools build no declaration.
//   verify  before the pass's draws the backend compares its actual state (what
//           the draw bakes into its pipeline: llvkgl's tracked state, the bound
//           program, the units the program reads) with the declaration and
//           counts what differs; the pass's calls into the backend (state, unit
//           bindings, program changes) are counted per pass and view (the
//           census). research/207 T4: the viewer's copy of the state (llrender)
//           is compared with the backend too (the copy watch: one writer, the
//           copy, keeps them equal), before the draws and after them, and the
//           fields the write-through would write are counted (none expected).
//           One "pass state:" line per VKPerf period, the first 8 differences
//           one by one.
//   auto    (default since research/218 vk-s2f) verify until 600 world
//           frames (frames with a check) had no difference (declaration or
//           copy watch), then on; one difference: off for the run.
//   on      nothing is compared or counted; the open pass's declaration is
//           published (openPassState) for the retained scene's passes, and
//           written through the copy for the pass's draws (PassStateWrite:
//           copy and backend together, put back after the draws). With
//           declarations verify found right, nothing differs and nothing is
//           written: the draws are the same as off's.
// Render thread only. Without a backend (not Vulkan) verify does nothing.
namespace LLRHI
{
    // ==== pass state types: begin ====
    enum class PassStateMode : U8
    {
        Off,
        Verify,
        Auto,
        On,
    };
    // As LLRHI_PASS_STATE asked (read once).
    PassStateMode passStateMode();
    const char* passStateModeName(PassStateMode mode);

    // The view a world pass is drawn for (the counts are kept apart: the state
    // around a pool may differ between them).
    enum PassView : U8
    {
        PASS_VIEW_MAIN = 0,
        PASS_VIEW_PROBE = 1,        // a reflection probe face (gCubeSnapshot)
        PASS_VIEW_REFLECTION = 2,   // the water reflection (LLPipeline::sReflectionRender)
        PASS_VIEW_COUNT = 3,
    };

    constexpr U32 PASS_BLEND_EQ_ADD = 0x8006;  // GL_FUNC_ADD
    constexpr U32 PASS_STATE_UNITS = 32;       // texture units (llvkgl's)

    // GL enums, as the viewer's state calls pass them. Only the groups named in
    // `declared` are compared; within a group, what the draw does not use is not:
    // the blend factors and equations while blending is off, the depth write and
    // function while the depth test is off, the face while culling is off.
    struct PassState
    {
        enum Group : U32
        {
            BLEND = 1 << 0,
            COLOR_MASK = 1 << 1,
            DEPTH = 1 << 2,
            CULL = 1 << 3,
            STENCIL = 1 << 4,           // the test on or off (its functions are the pass's surroundings')
            POLYGON_OFFSET = 1 << 5,    // on or off (the values are dynamic state)
            DEPTH_CLAMP = 1 << 6,
            PROGRAM = 1 << 7,
            SAMPLERS = 1 << 8,          // the units the program reads, all of them
            SAMPLERS_PART = 1 << 9,     // the units named, among others the pass's surroundings bind
        };
        static constexpr U32 GROUP_COUNT = 10;

        U32 declared = 0;
        bool blend = false;
        U32 blend_src_rgb = 0, blend_dst_rgb = 0, blend_src_a = 0, blend_dst_a = 0;
        U32 blend_eq_rgb = PASS_BLEND_EQ_ADD, blend_eq_a = PASS_BLEND_EQ_ADD;
        bool color_mask[4] = { true, true, true, true };
        bool depth_test = false;
        bool depth_write = false;
        U32 depth_func = 0;
        bool cull = false;
        U32 cull_face = 0;
        bool stencil = false;
        bool polygon_offset = false;
        bool depth_clamp = false;
        U32 program = 0;            // the GL program name
        U32 sampler_units = 0;      // one bit a texture unit

        PassState& noBlend() { declared |= BLEND; blend = false; return *this; }
        PassState& blendFunc(U32 src, U32 dst)
        {
            declared |= BLEND;
            blend = true;
            blend_src_rgb = blend_src_a = src;
            blend_dst_rgb = blend_dst_a = dst;
            blend_eq_rgb = blend_eq_a = PASS_BLEND_EQ_ADD;
            return *this;
        }
        PassState& colorMask(bool r, bool g, bool b, bool a)
        {
            declared |= COLOR_MASK;
            color_mask[0] = r; color_mask[1] = g; color_mask[2] = b; color_mask[3] = a;
            return *this;
        }
        PassState& depth(bool test, bool write, U32 func)
        {
            declared |= DEPTH;
            depth_test = test; depth_write = write; depth_func = func;
            return *this;
        }
        PassState& noCull() { declared |= CULL; cull = false; return *this; }
        PassState& cullFace(U32 face) { declared |= CULL; cull = true; cull_face = face; return *this; }
        PassState& noStencil() { declared |= STENCIL; stencil = false; return *this; }
        PassState& noPolygonOffset() { declared |= POLYGON_OFFSET; polygon_offset = false; return *this; }
        PassState& noDepthClamp() { declared |= DEPTH_CLAMP; depth_clamp = false; return *this; }
        PassState& useProgram(U32 name) { declared |= PROGRAM; program = name; return *this; }
        // A unit the program reads (a channel's unit; negative: the program has no such channel, nothing named).
        PassState& sampler(S32 unit)
        {
            declared |= (declared & SAMPLERS_PART) ? 0u : (U32)SAMPLERS;
            if (unit >= 0 && unit < (S32)PASS_STATE_UNITS)
            {
                sampler_units |= 1u << unit;
            }
            return *this;
        }
        // The units named are some of those the program reads (before sampler()).
        PassState& samplersPart() { declared = (declared & ~(U32)SAMPLERS) | SAMPLERS_PART; return *this; }
    };

    // What the draw would use now, as the backend has it (every group filled).
    struct PassStateActual
    {
        PassState state;            // program: the bound program's name; sampler_units: unused
        bool program_known = false; // the backend knows the bound program (else no sampler test)
        U32 sampled_units = 0;      // the units the bound program reads
        U32 bound_units = 0;        // of those, the ones with a texture bound where the program reads it
    };

    // The comparison's result.
    struct PassStateCheck
    {
        U32 differ = 0;                 // PassState::Group bits whose declared values the draw would not use
        U32 sampled_undeclared = 0;     // read by the program, not declared (SAMPLERS only)
        U32 declared_unsampled = 0;     // declared, not read by the program
        U32 declared_unbound = 0;       // declared and read, nothing bound
    };

    // Compares a declaration with the actual state. Pure: the backend fills
    // `actual`, this decides (selftest sim-passstate takes it from here).
    void comparePassState(const PassState& declared, const PassStateActual& actual, PassStateCheck& out);

    // auto's progress (pure; sim-passstate takes it from here too).
    struct PassStateAuto
    {
        static constexpr U32 CLEAN_FRAMES = 600;
        U32 clean_frames = 0;       // world frames with checks and no difference so far
        U32 last_frame = 0;
        bool counted_any = false;
        // One check in `frame`; `differ`: it found a difference. Returns the
        // mode to go on in: Verify while counting, On once enough frames were
        // clean, Off at the first difference.
        PassStateMode step(U32 frame, bool differ);
    };

    // research/207 T4: one bit a value of a pass state, for the write-through
    // (which values it writes and puts back) and the copy watch (which values
    // the viewer's copy and the backend disagree on).
    enum PassStateField : U32
    {
        FIELD_BLEND = 1u << 0,              // blending on or off
        FIELD_BLEND_FUNC = 1u << 1,         // the four factors (and, compared only, the equations)
        FIELD_COLOR_MASK = 1u << 2,
        FIELD_DEPTH_TEST = 1u << 3,
        FIELD_DEPTH_WRITE = 1u << 4,
        FIELD_DEPTH_FUNC = 1u << 5,
        FIELD_CULL = 1u << 6,
        FIELD_CULL_FACE = 1u << 7,
        FIELD_STENCIL = 1u << 8,            // the test on or off
        FIELD_POLYGON_OFFSET = 1u << 9,     // on or off (fill)
        FIELD_DEPTH_CLAMP = 1u << 10,
        FIELD_PROGRAM = 1u << 11,           // compared only: the pool binds its program itself
        FIELD_COUNT = 12,
        FIELDS_WRITTEN = (1u << 11) - 1u,   // all but the program
        FIELDS_ALL = (1u << 12) - 1u,
    };
    // ==== pass state types: end ====

    // ==== pass state write: begin ====
    // The write-through (research/207 4.2 (2)) as pure functions; the copy
    // (llrender) does the writes. Selftest sim-passstate takes them from here.
    //
    // The fields to write so that a draw made in `now` uses what `want`
    // declares: the declared groups only, and within a group only what a draw
    // uses (no factors while blending is off, no depth write or function while
    // the test is off, no face while culling is off; as comparePassState).
    U32 passStateWrites(const PassState& want, const PassState& now);
    // The fields among `fields` where a and b differ, every value compared
    // (used by a draw or not: a copy that is wrong while blending is off is
    // wrong once it is on).
    U32 passStateDiffers(const PassState& a, const PassState& b, U32 fields);
    // The fields to put back after the draws: of those written, the ones that
    // still hold what was written (one written again in between was the
    // pass's own: kept).
    U32 passStateRestores(const PassState& written, U32 fields, const PassState& now);
    // ==== pass state write: end ====

    // The backend's running totals the census takes differences of.
    struct PassStateCounters
    {
        U32 state_calls = 0;        // the native state API's calls (sSetCap, sBlendFunc, ...)
        U32 unit_bindings = 0;      // texture unit binding changes, all units
        U32 program_changes = 0;    // bound program changes
    };

    // Set by the Vulkan backend (llvkpassstate.cpp); null otherwise. Render
    // thread. Both take the backend's lock.
    class PassStateBackend
    {
    public:
        virtual ~PassStateBackend() = default;
        virtual bool actual(PassStateActual& out) = 0;
        virtual bool counters(PassStateCounters& out) = 0;
        // research/204 10 (vk-s4c2), LLRHI_PASS_UNITS verify: the units a
        // pass's cleanup just left unbound (bits); false: not watched now
        // (another thread holds the GL state). reads: the draws that read a
        // unit left so before it was bound again, since the start.
        virtual bool unitsLeft(U32 units) = 0;
        virtual U32 unitsRead() = 0;
    };
    extern PassStateBackend* sPassStateBackend;

    // research/204 10 (vk-s4c2): the units a world pass binds are its own
    // (204 6.3). LLRHI_PASS_UNITS (startup):
    //   off     (default) the pools' cleanups as before.
    //   verify  the cleanups as before; after each, the units it left unbound
    //           are told to the backend (passUnitsLeft), which counts the
    //           draws that read one of them before it is bound again (none
    //           expected: then dropping the cleanup draws the same).
    //   auto    verify until 600 world frames without such a read, then on;
    //           one: off for the run.
    //   on      the cleanups are left out (passUnitsKeep): the units keep the
    //           pass's textures (LLTexUnit's copy with them).
    enum class PassUnitsMode : U8
    {
        Off,
        Verify,
        Auto,
        On,
    };
    PassUnitsMode passUnitsMode();
    // Whether the pool leaves its cleanup out now (on, or auto once on).
    bool passUnitsKeep();
    // After a pool's cleanup (verify, or auto still verifying): the units it
    // left unbound (bits), in the world frame `frame`.
    void passUnitsLeft(U32 units, U32 frame);

    // research/207 T4: the viewer's copy of the state (llrender: LLGLState's
    // caps, LLGLDepthTest, gGL's blend factors, colour mask and cull face, the
    // bound LLGLSLShader), the only writer of the backend's state. Set by
    // LLRender::init; null before. Render thread.
    class PassStateCopy
    {
    public:
        virtual ~PassStateCopy() = default;
        // The copy's values, every field (program: the bound shader's GL name;
        // declared, sampler_units: 0).
        virtual void read(PassState& out) = 0;
        // The fields named (within FIELDS_WRITTEN) from `values`, to the copy
        // and the backend together, through the copy's own calls (one call a
        // field that changes, none for one that does not).
        virtual void write(const PassState& values, U32 fields) = 0;
    };
    extern PassStateCopy* sPassStateCopy;

    // Opened at a world pass's head, closed at its end (RAII). name: a string
    // literal (counts are kept by it); null: the scope does nothing (a pass
    // that is not declared, such as a pool's draws for a shadow map).
    class PassStateScope
    {
    public:
        PassStateScope(const char* name, PassView view, U32 frame);
        ~PassStateScope();
        PassStateScope(const PassStateScope&) = delete;
        PassStateScope& operator=(const PassStateScope&) = delete;

        // Whether to build the declaration (verify and on; never while off).
        bool declaring() const { return mDeclaring; }
        void declare(const PassState& state);
        // Right before the pass's draws: `draws` draws follow with no state
        // call between them (a loop of draws only), or 1 at each draw.
        void check(U32 draws);
        // The declaration once declare() ran, else null.
        const PassState* declared() const { return mDeclared ? &mState : nullptr; }

    private:
        friend class PassStateWrite;
        PassStateScope* mOuter = nullptr;   // the scope open around this one (restored at the end)
        const char* mName = nullptr;
        PassState mState;
        PassStateCounters mStart;
        U32 mFrame = 0;
        U8 mView = 0;
        U8 mEntry = 0xff;           // verify: the counts' entry
        bool mDeclaring = false;
        bool mDeclared = false;
        bool mCounting = false;     // verify, with the backend: the census runs
    };

    // research/207 T4: brackets a declared pass's draws (and only its draws:
    // no state call of the pool's between its construction and its end).
    //   on      writes the declaration through the copy (copy and backend
    //           together, only the fields that differ) and puts those fields
    //           back at the end, the ones the draws left as written. With a
    //           declaration that verify found right (no difference) nothing
    //           differs: nothing is written, the draws are drawn as before.
    //   verify  writes nothing: counts the fields the write-through would
    //           write (none expected) and, at the end, compares the copy with
    //           the backend again (the copy watch; check() does it before).
    //   off     nothing (the scope is not open).
    class PassStateWrite
    {
    public:
        explicit PassStateWrite(const PassStateScope& scope);
        ~PassStateWrite();
        PassStateWrite(const PassStateWrite&) = delete;
        PassStateWrite& operator=(const PassStateWrite&) = delete;

    private:
        const PassStateScope* mScope = nullptr;
        PassState mBefore;          // on: the copy before the write
        U32 mWritten = 0;           // on: the fields written
        bool mWatch = false;        // verify: compare the copy at the end
        bool mPublishing = false;   // on: openPassState answered with this scope while it lived (research/204 10)
        const PassStateScope* mSavedWriting = nullptr;
    };

    // On: the declaration of the pass open now, inside its write-through (its
    // draws) and only when the copy holds all of it (research/204 10: the
    // retained scene's pass heads pick their pipeline from it). Else null.
    const PassState* openPassState();

    // The VKPerf line for `frames` frames (empty while off and nothing was
    // counted); starts the counts over.
    std::string passStateReport(U32 frames);
}

#endif // LL_LLRHIPASSSTATE_H

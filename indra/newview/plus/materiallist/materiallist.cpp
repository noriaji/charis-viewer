/**
 * @file materiallist.cpp
 * @brief Material lists (research/137 N4): the legacy material pool's and the
 *        bump pool's batch loops as one draw list each (LLRHI::DrawList).
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

// Why the pixels cannot change (research/137 section 2.3): the old loops set
// the uniforms and bind the textures a batch needs, then draw it as a direct
// draw (drawGeometry -> pushDirectBatch without textures: the draw takes the
// program's uniforms and the units' bindings as they are). A list draw carries
// exactly those values instead: each uniform the batch changes as a patch
// (set before the draw, kept after it, as the uniform call would), the names
// the units would hold, the texture animation the loop would load into the
// texture matrix, and the model matrix pushDirectBatch would compute. Nothing
// else changes between the batches of these loops, so the backend may resolve
// the rest once at the head of the list.
//
// Batches the list cannot take go the old way, in their place: the list so far
// is submitted first (Builder::cut), then the official body for one batch runs.
// Such a batch has a texture without a GL name yet or with sampler options to
// apply (bindFast / bind deal with those through the usual calls), a mapped
// vertex buffer (setBuffer flushes it), or an avatar (plus/skinsss marks its
// batches through glStencilFunc, which the guard does not see).
//
// research/137 N11 (LLRHI_DRAW_LISTS_THREADS, THREADS_BUILD): a long
// material loop's draws are made on a cull thread, chunk after chunk, by the
// same per batch steps (LoopState), while the main thread submits the chunks
// made before (one chunk = one list, Builder::addChunk) and draws the old way
// batches in their place. See "The threaded build" below.

#include "llviewerprecompiledheaders.h"

#include "materiallist.h"

#include "lldrawpool.h"
#include "lldrawpoolbump.h"
#include "llglslshader.h"
#include "llgraywire.h"
#include "llrender.h"
#include "llrhi.h"
#include "llrhidrawlist.h"
#include "llshadermgr.h"
#include "llspatialpartition.h"
#include "llvertexbuffer.h"
#include "llviewertexture.h"
#include "pipeline.h"
#include "plus/drawlist/drawlistbatch.h"
#include "plus/skinsss/skinsss.h"

#include "glm/gtc/type_ptr.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

namespace
{
    namespace DL = LLRHI::DrawList;

    constexpr U32 MAX_UNITS = 16;           // texture units a list draw names (the loops' channels are below it)
    constexpr U32 MAX_LOOP_UNIFORMS = 4;
    constexpr U32 MAX_LOOP_UNITS = 3;

    plus::materiallist::BuildStats sBuildStats;     // main thread

    // The matrices LLRenderPass::pushDirectBatch computes for a batch with this
    // model matrix (the pass's modelview times it; LLRender::fillDirectMatrices
    // fills those the bound program has, the others stay zero).
    // research/137 N2: its shared "batch -> Draw" header replaces this when it lands.
    void fill_matrices(const LLMatrix4* model, DL::Matrices& out)
    {
        glm::mat4 modelview = glm::make_mat4(gGLModelView);
        if (model)
        {
            modelview *= glm::make_mat4((const F32*)model->mMatrix);
        }
        LLRender::DirectMatrices storage;
        LLRHI::DirectDraw pointers;
        gGL.fillDirectMatrices(modelview, storage, pointers);
        auto copy = [](F32* dst, const F32* src, size_t floats)
        {
            if (src)
            {
                memcpy(dst, src, floats * sizeof(F32));
            }
            else
            {
                memset(dst, 0, floats * sizeof(F32));
            }
        };
        copy(out.modelview, pointers.modelview, 16);
        copy(out.modelview_projection, pointers.modelview_projection, 16);
        copy(out.normal, pointers.normal, 9);
        copy(out.inverse_modelview, pointers.inverse_modelview, 16);
        // No id (0): the backend writes these values into a matrix row of the
        // draw's own and shares it with no other draw (sharing rows by id would
        // take ids from LLRHI::DrawList::newMatrixId(), research/137 N1/N2; not done
        // here). Consecutive batches with the same model matrix name the
        // previous draw's entry instead (Draw::matrices NONE).
        out.id = 0;
    }

    // LLDrawPoolBump::bindBumpMap(bump_code, texture, channel) without the
    // binding (lldrawpoolbump.cpp, private there): the bump map the batch's
    // bump code names, or null. A brightness or darkness map is made (again)
    // here when it is new or its source changed size: a draw into a render
    // target, whose LLRHI calls cut the list (the draws so far go first), and
    // whose state (the color mask it leaves on, unit 0 unbound) the batches
    // after it see, as in the official loop. So it is looked up in the
    // batch's place, before the batch is staged, never ahead.
    LLViewerTexture* bump_map(const LLDrawInfo& params)
    {
        LLViewerFetchedTexture* tex = LLViewerTextureManager::staticCastToFetchedTexture(params.mTexture.get());
        if (!tex)
        {
            return nullptr;
        }
        const U8 bump_code = params.mBump;
        switch (bump_code)
        {
        case BE_NO_BUMP:
            return nullptr;
        case BE_BRIGHTNESS:
        case BE_DARKNESS:
            return gBumpImageList.getBrightnessDarknessImage(tex, bump_code);
        default:
            if (bump_code < LLStandardBumpmap::sStandardBumpmapCount)
            {
                gBumpImageList.addTextureStats(bump_code, tex->getID(), tex->getMaxVirtualSize());
                return gStandardBumpmapList[bump_code].mImage;
            }
            return nullptr;
        }
    }

    // Where a loop's draw goes: the Builder's open list, or a chunk's arrays
    // (the threaded build). Also how its matrices are made (fill_matrices
    // reads gGL and the bound program: main thread only).
    class LoopSink : public plus::drawlist::DrawSink
    {
    public:
        // Builder::addDraw / Arrays::addDraw: false, not taken (the old way).
        virtual bool addDraw(const DL::Draw& draw) = 0;
        virtual void matrices(const LLMatrix4* model, DL::Matrices& out) = 0;
    };

    // The Builder's open list (the loops on the main thread).
    class BuilderSink : public LoopSink
    {
    public:
        U32 addAttributes(const LLRHI::DirectDraw::Attribute* attributes, U32 count) override { return mList.addAttributes(attributes, count); }
        U32 addTextures(const U32* names, U32 count) override { return mList.addTextures(names, count); }
        U32 addMatrices(const DL::Matrices& matrices) override { return mList.addMatrices(matrices); }
        U32 addFloats(const F32* values, U32 count) override { return mList.addFloats(values, count); }
        U32 addPatch(S32 slot, U32 count, U32 components, U32 rows, const F32* values) override
        {
            return mList.addPatch(slot, count, components, rows, values);
        }
        U32 addState(const DL::State& state) override { return mList.addState(state); }
        bool addDraw(const DL::Draw& draw) override { return mList.addDraw(draw); }
        void matrices(const LLMatrix4* model, DL::Matrices& out) override { fill_matrices(model, out); }

    private:
        DL::Builder& mList = DL::Builder::get();
    };

    // One loop's per batch state: what the old loop would have set last (each
    // uniform it sets per batch, each unit it binds per batch), so a draw
    // carries only what changed, and Sync leaves the viewer's caches
    // (LLGLSLShader::cachedUniform, LLTexUnit) as after the old loop (research/137
    // section 2.3, promise 4). Per batch: set* stage the batch's values, then
    // stage() writes the draw into a sink (or says no: the batch goes the old
    // way), then commit(). Reads the batch, its buffer, its textures' GL names
    // and its matrices only, writes itself and the sink: the threaded build
    // runs it on a cull thread (commit's texture marks then go to a list the
    // main thread applies).
    struct LoopState
    {
        struct Uniform
        {
            U32 index = 0;          // LLShaderMgr's uniform index
            U32 components = 1;
            S32 slot = -1;          // the program's native slot
            bool used = false;      // the program has it (the loop sets it)
            bool known = false;     // value is what the program has (else the next draw sets it)
            bool pending = false;   // staged for the batch being built
            F32 value[4] = {};
            F32 next[4] = {};
        };
        struct Unit
        {
            S32 channel = -1;
            bool full = false;
            bool diffuse = false;   // research/276: the batch's diffuse texture (GrayWire's wireframe and band go by it)
            bool known = false;     // texture/name are what the unit has (else as bound before: KEEP_UNIT)
            bool pending = false;
            LLViewerTexture* texture = nullptr;     // null: unbound (white)
            U32 name = 0;
            LLViewerTexture* next = nullptr;
            U32 next_name = 0;
        };

        Uniform uniforms[MAX_LOOP_UNIFORMS];
        U32 uniform_count = 0;
        Unit units[MAX_LOOP_UNITS];
        U32 unit_count = 0;
        U32 attribute_mask = 0;         // the program's LLGLSLShader::mAttributeMask
        bool gray_wire = false;         // research/276: GrayWire::enabled() as the loop opened (the threaded build reads this); research/289: in a wireframe's pass
        bool old_way = false;           // the staged batch needs the old way (a texture without a name)
        bool have_matrices = false;     // the list's last draw has last_model's matrices
        const LLMatrix4* last_model = nullptr;

        void setUniform(U32 u, const F32* value);
        void setTexture(U32 u, LLViewerTexture* texture);
        // The staged batch may be a list draw as far as it goes (stage() may still say no).
        bool takes(const LLDrawInfo& params) const;
        // The staged batch as a list draw into sink; its answer (false: the old way).
        bool stage(const LLDrawInfo& params, LoopSink& sink) const;
        // The staged values are the program's and the units' now: by the list
        // draw (drawn), or by the old body's calls. marks: null, mark the
        // drawn textures used here (main thread); else they go into it.
        void commit(bool drawn, std::vector<LLViewerTexture*>* marks);
        // After commit(true): the list's last draw has this batch's matrices.
        void taken(const LLDrawInfo& params)
        {
            have_matrices = true;
            last_model = params.mModelMatrix;
        }
    };

    void LoopState::setUniform(U32 u, const F32* value)
    {
        Uniform& uniform = uniforms[u];
        if (!uniform.used)
        {
            return;
        }
        // Compared as the loop and LLGLSLShader compare (float !=).
        bool same = uniform.known;
        for (U32 c = 0; c < uniform.components; ++c)
        {
            uniform.next[c] = value[c];
            same = same && uniform.value[c] == value[c];
        }
        uniform.pending = !same;
    }

    void LoopState::setTexture(U32 u, LLViewerTexture* texture)
    {
        Unit& unit = units[u];
        if (unit.channel < 0)
        {
            return;
        }
        unit.pending = true;
        unit.next = texture;
        unit.next_name = texture ? LLTexUnit::directName(texture) : 0;
        if (unit.next_name != LLRHI::DirectDraw::KEEP_UNIT)
        {
            return;
        }
        // No GL name yet (or sampler options to apply): bindFast / bind make or
        // set it up through the usual calls, so the batch goes the old way.
        // Unless bindFast did that for this very texture on this unit already
        // and the unit still holds what it left (the old way leaves such a unit
        // unknown, and nothing binds it since): the draw keeps the unit's
        // binding, as the official loop, which binds only a texture that
        // changed, leaves it. bind() is not skipped (the official loop calls it
        // for every batch).
        // research/276 (research/275 3-b): never while the loading wireframe is on
        // for a diffuse texture still loading (no GL name, or held on the GL path
        // by GrayWire): each such batch goes the old way, through GrayWire's draw
        // (a wireframe, or its band). A list draw would keep the unit's binding
        // and show it filled: the gray stand-in, or the look an old way batch
        // left on unit 0. Not a missing asset (review low 2), nor one without
        // its name whose wait was given up (review low 4, GrayWire::isGray's
        // note): never a wireframe, the unit's binding is what the old way
        // leaves. A held one (with its name) goes by its hold only: an
        // avatar's wireframe or a band, whatever its own last wait was.
        const LLImageGL* image = texture ? texture->getGLTexture() : nullptr;
        const bool named = image && image->getTexName();
        const bool loading = unit.diffuse && gray_wire && texture && !texture->isMissingAsset() &&
                             (named ? image->heldOnGL() : !texture->mGrayWireGivenUp);
        if (loading || !(texture && !unit.full && !unit.known && unit.texture == texture))
        {
            old_way = true;
        }
    }

    bool LoopState::takes(const LLDrawInfo& params) const
    {
        const LLVertexBuffer* vb = params.mVertexBuffer;
        // Nor a rigid object worn on an eye point while skin is marked: plus/skinsss marks it too (fix-round41).
        return !old_way && vb && !vb->isMapped() && params.mAvatar.isNull() &&
               !(plus::skinsss::detail::sMarking && plus::skinsss::detail::isEyeAttachment(params));
    }

    bool LoopState::stage(const LLDrawInfo& params, LoopSink& sink) const
    {
        const LLVertexBuffer* vb = params.mVertexBuffer;
        DL::Draw draw;
        draw.vertex_buffer = vb->getGLBuffer();
        draw.index_buffer = vb->getGLIndices();
        draw.index_type = vb->getIndicesType();
        draw.first = params.mOffset;
        draw.count = params.mCount;
        draw.buffer_vertices = vb->getNumVerts();
        draw.buffer_indices = vb->getNumIndices();

        // What each unit the loop binds would hold for this batch.
        U32 names[MAX_UNITS];
        U32 name_count = 0;
        for (U32 u = 0; u < unit_count; ++u)
        {
            const Unit& unit = units[u];
            if (unit.channel < 0)
            {
                continue;
            }
            while (name_count <= (U32)unit.channel)
            {
                names[name_count++] = LLRHI::DirectDraw::KEEP_UNIT;
            }
            names[unit.channel] = unit.pending ? unit.next_name : unit.known ? unit.name : LLRHI::DirectDraw::KEEP_UNIT;
        }
        if (name_count)
        {
            draw.first_texture = sink.addTextures(names, name_count);
            draw.texture_count = name_count;
        }

        if (!have_matrices || params.mModelMatrix != last_model)
        {
            DL::Matrices matrices;
            sink.matrices(params.mModelMatrix, matrices);
            draw.matrices = sink.addMatrices(matrices);
        }
        if (params.mTextureMatrix)
        {
            // The loop loads it into unit 0's texture matrix for the batch
            // and loads the identity after it.
            draw.texture_matrix = sink.addFloats((const F32*)params.mTextureMatrix->mMatrix, 16);
        }

        LLRHI::DirectDraw::Attribute attributes[LLRHI::DirectDraw::MAX_ATTRIBUTES];
        draw.attribute_count = vb->getAttributes(attribute_mask, attributes);
        draw.first_attribute = sink.addAttributes(attributes, draw.attribute_count);

        DL::State state;
        for (U32 u = 0; u < uniform_count; ++u)
        {
            const Uniform& uniform = uniforms[u];
            if (uniform.pending)
            {
                const U32 patch = sink.addPatch(uniform.slot, 1, uniform.components, 0, uniform.next);
                if (!state.patch_count)
                {
                    state.first_patch = patch;
                }
                llassert(patch == state.first_patch + state.patch_count);   // addPatch appends
                ++state.patch_count;
            }
        }
        draw.state = sink.addState(state);

        return sink.addDraw(draw);
    }

    void LoopState::commit(bool drawn, std::vector<LLViewerTexture*>* marks)
    {
        for (U32 u = 0; u < uniform_count; ++u)
        {
            Uniform& uniform = uniforms[u];
            if (uniform.pending)
            {
                memcpy(uniform.value, uniform.next, sizeof(uniform.value));
                uniform.known = true;
                uniform.pending = false;
            }
        }
        for (U32 u = 0; u < unit_count; ++u)
        {
            Unit& unit = units[u];
            if (!unit.pending)
            {
                continue;
            }
            unit.pending = false;
            if (drawn && unit.next)
            {
                // The bookkeeping the loop's binding does: bindFast marks the
                // texture used; bind() also counts the bind once a frame when
                // the unit held another texture.
                if (unit.full)
                {
                    llassert(!marks);   // the threaded build takes no loop with such a unit
                    LLTexUnit* tex_unit = gGL.getTexUnit(unit.channel);
                    const U32 held = unit.known ? unit.name : tex_unit ? tex_unit->getCurrTexture() : 0;
                    const LLImageGL* gl_tex = unit.next->getGLTexture();
                    if (held != unit.next_name && gl_tex && gl_tex->updateBindStats())
                    {
                        unit.next->setActive();
                        unit.next->updateBindStatsForTester();
                    }
                }
                else if (marks)
                {
                    marks->push_back(unit.next);
                }
                else
                {
                    unit.next->setActive();
                }
            }
            // A texture without a name went through bindFast / bind: the unit
            // holds what they left (the default image, or the texture).
            unit.known = unit.next_name != LLRHI::DirectDraw::KEEP_UNIT;
            unit.texture = unit.next;
            unit.name = unit.next_name;
        }
        old_way = false;
    }

    // One loop's list on the main thread: its LoopState, the Builder's list,
    // and the Sync that brings the viewer's caches to the state's values
    // after each submission. Per batch: set* stage the batch's values, then
    // add() writes the draw (or says no: the caller draws the batch the old
    // way and calls drewOldWay()).
    class Loop
    {
    public:
        explicit Loop(LLGLSLShader* shader) : mShader(shader) {}
        ~Loop() { submit(); }
        Loop(const Loop&) = delete;
        Loop& operator=(const Loop&) = delete;

        // A uniform the loop sets per batch (components: 1 for uniform1f, 4 for
        // uniform4fv); the number to stage it with. One the program does not
        // have is ignored, as the loop skips it.
        U32 addUniform(U32 index, U32 components);
        // A unit the loop binds per batch (channel -1: none, staging it does
        // nothing); full: the loop binds with bind() (bind statistics), else
        // with bindFast(). Either unbinds to white. diffuse: the batch's
        // diffuse texture's (research/276).
        U32 addUnit(S32 channel, bool full, bool diffuse = false);

        // Opens the list. False: the official loop draws (nothing changed).
        bool open(const char* name);

        void setUniform(U32 u, const F32* value) { mState.setUniform(u, value); }
        void setTexture(U32 u, LLViewerTexture* texture) { mState.setTexture(u, texture); }
        // The batch as a list draw with the staged values. False: not taken
        // (the list so far is submitted): draw it the old way, then drewOldWay().
        bool add(LLDrawInfo& params);
        // The old body drew the batch: its staged values are what the program
        // and the units have now.
        void drewOldWay(const LLDrawInfo& params);

        void submit();

        // The threaded build (research/137 N11): the state it starts from,
        // and the state Sync reads (a chunk's, while that chunk is submitted;
        // null: this loop's own).
        LoopState& state() { return mState; }
        void syncFrom(const LoopState* state) { mSyncState = state ? state : &mState; }
        bool hasFullUnit() const;

    private:
        bool isOpen() const;
        static void sync(void* user);

        LLGLSLShader* mShader;
        LoopState mState;
        const LoopState* mSyncState = &mState;
        bool mUsable = true;        // every uniform has a native slot, every channel fits
        bool mOpen = false;
        U32 mSerial = 0;
    };

    U32 Loop::addUniform(U32 index, U32 components)
    {
        llassert(mState.uniform_count < MAX_LOOP_UNIFORMS && components >= 1 && components <= 4);
        const U32 u = mState.uniform_count++;
        LoopState::Uniform& uniform = mState.uniforms[u];
        uniform.index = index;
        uniform.components = components;
        uniform.used = mShader->getUniformLocation(index) > -1;
        if (uniform.used)
        {
            uniform.slot = index < mShader->mNativeSlot.size() ? mShader->mNativeSlot[index] : -1;
            if (uniform.slot < 0)
            {
                mUsable = false;    // the old loop sets it through GL
            }
        }
        return u;
    }

    U32 Loop::addUnit(S32 channel, bool full, bool diffuse)
    {
        llassert(mState.unit_count < MAX_LOOP_UNITS);
        const U32 u = mState.unit_count++;
        LoopState::Unit& unit = mState.units[u];
        unit.channel = channel;
        unit.full = full;
        unit.diffuse = diffuse;
        if (channel >= (S32)MAX_UNITS)
        {
            mUsable = false;
        }
        return u;
    }

    bool Loop::hasFullUnit() const
    {
        for (U32 u = 0; u < mState.unit_count; ++u)
        {
            if (mState.units[u].channel >= 0 && mState.units[u].full)
            {
                return true;
            }
        }
        return false;
    }

    bool Loop::open(const char* name)
    {
        if (!mUsable || !mShader || mShader != LLGLSLShader::sCurBoundShaderPtr || !mShader->mNativeProgram ||
            !LLRHI::sDirect || !LLRHI::sDirectDraw || !LLRHI::sGLDrawsDisabled)
        {
            return false;
        }
        // Projection, lights and the pass's matrices reach the program as
        // through GL, as pushDirectBatch has them before its first batch.
        gGL.syncMatrices();
        DL::Builder& list = DL::Builder::get();
        if (!list.open(name, DL::KIND_MATERIAL, &Loop::sync, this))
        {
            return false;
        }
        mOpen = true;
        mSerial = list.serial();
        mState.attribute_mask = mShader->mAttributeMask;
        // research/289 (pre-merge review L3): only in a pass that draws the wireframe (GrayWire::wirePassNow). In
        // the others (the probes' and the water mirror's: cube snapshots) GrayWire's Scope does nothing and asks no
        // isGray, so a texture drawn only there never has its wait given up (mGrayWireGivenUp) and its batches
        // would take the old way for good; there the old way binds the gray stand-in, which a list draw keeps.
        mState.gray_wire = GrayWire::enabled() && GrayWire::wirePassNow();
        return true;
    }

    bool Loop::isOpen() const
    {
        const DL::Builder& list = DL::Builder::get();
        return mOpen && list.isOpen() && list.serial() == mSerial;
    }

    bool Loop::add(LLDrawInfo& params)
    {
        BuilderSink sink;
        if (!(mState.takes(params) && isOpen() && mState.stage(params, sink)))
        {
            // The draws so far go first; the old body's calls follow them.
            DL::Builder::cut("plus::materiallist old path");
            return false;
        }
        mState.commit(true, nullptr);
        mState.taken(params);
        return true;
    }

    void Loop::drewOldWay(const LLDrawInfo& params)
    {
        mState.commit(false, nullptr);
        // The old body's direct draw synced unit 0's texture matrix with the
        // batch's animation and left the identity in gGL: the program has it
        // back before the next list draw (which goes by the program's).
        if (params.mTextureMatrix)
        {
            gGL.syncMatrices();
        }
        mState.have_matrices = false;   // the next list draw names its matrices
    }

    void Loop::submit()
    {
        if (isOpen())
        {
            DL::Builder::get().submit();    // calls sync
        }
        mOpen = false;
    }

    // After each submission (the end of the list, a cut or a chunk): the last
    // values the usual way, so LLGLSLShader's value cache and LLTexUnit's
    // bindings are as the old loop leaves them. The backend has the same
    // values already.
    void Loop::sync(void* user)
    {
        const Loop& loop = *(const Loop*)user;
        const LoopState& state = *loop.mSyncState;
        LLGLSLShader* shader = loop.mShader;
        if (shader != LLGLSLShader::sCurBoundShaderPtr)
        {
            return;
        }
        for (U32 u = 0; u < state.uniform_count; ++u)
        {
            const LoopState::Uniform& uniform = state.uniforms[u];
            if (!uniform.known)
            {
                continue;
            }
            if (uniform.components == 1)
            {
                shader->uniform1f(uniform.index, uniform.value[0]);
            }
            else
            {
                shader->uniform4fv(uniform.index, 1, uniform.value);
            }
        }
        for (U32 u = 0; u < state.unit_count; ++u)
        {
            const LoopState::Unit& unit = state.units[u];
            LLTexUnit* tex_unit = unit.known ? gGL.getTexUnit(unit.channel) : nullptr;
            if (!tex_unit || tex_unit->getCurrTexture() == unit.name)
            {
                continue;
            }
            if (!unit.texture)
            {
                tex_unit->unbindFast(LLTexUnit::TT_TEXTURE);
            }
            else if (unit.full)
            {
                tex_unit->bind(unit.texture);
            }
            else
            {
                tex_unit->bindFast(unit.texture);
            }
        }
    }

    // ------------------------------------------------------------------
    // The official bodies for one batch (the old way)

    // LLDrawPoolMaterials::renderDeferred's body, setting every value (the
    // official loop sets those that changed; the values end up the same).
    struct MaterialPass
    {
        LLGLSLShader* shader = nullptr;
        bool specular = false, intensity = false, min_alpha = false, brightness = false;
        S32 diffuse_channel = -1, spec_channel = -1, norm_channel = -1;
    };

    void draw_material_batch(const MaterialPass& pass, LLDrawInfo& params)
    {
        LLGLSLShader* shader = pass.shader;
        if (pass.specular)
        {
            shader->uniform4fv(LLShaderMgr::SPECULAR_COLOR, 1, params.mSpecColor.mV);
        }
        if (pass.intensity)
        {
            shader->uniform1f(LLShaderMgr::ENVIRONMENT_INTENSITY, params.mEnvIntensity);
        }
        if (pass.min_alpha)
        {
            shader->uniform1f(LLShaderMgr::MINIMUM_ALPHA, params.mAlphaMaskCutoff);
        }
        if (pass.brightness)
        {
            shader->uniform1f(LLShaderMgr::EMISSIVE_BRIGHTNESS, params.mFullbright ? 1.f : 0.f);
        }
        // research/276 (research/275 3-b): the binds inside GrayWire's draw, as
        // pushBatch's: a batch whose texture is not loaded yet binds the
        // wireframe's look on unit 0 (its lines in the wireframe's colour, not
        // the gray stand-in's), and so does a band's line pass. drawGeometry's
        // own drawBatch inside draws once.
        GrayWire::drawBatch(params, [&]()
        {
            if (pass.norm_channel > -1 && params.mNormalMap.notNull())
            {
                gGL.getTexUnit(pass.norm_channel)->bindFast(params.mNormalMap);
            }
            if (pass.spec_channel > -1 && params.mSpecularMap.notNull())
            {
                gGL.getTexUnit(pass.spec_channel)->bindFast(params.mSpecularMap);
            }
            if (params.mTexture.notNull())
            {
                gGL.getTexUnit(pass.diffuse_channel)->bindFast(params.mTexture);
            }
            else
            {
                gGL.getTexUnit(pass.diffuse_channel)->unbindFast(LLTexUnit::TT_TEXTURE);
            }
            const bool tex_setup = params.mTextureMatrix != nullptr;
            if (tex_setup)
            {
                gGL.getTexUnit(0)->activate();
                gGL.matrixMode(LLRender::MM_TEXTURE);
                gGL.loadMatrix((GLfloat*)params.mTextureMatrix->mMatrix);
                gPipeline.mTextureMatrixOps++;
            }
            LLRenderPass::drawGeometry(params);
            if (tex_setup)
            {
                gGL.getTexUnit(0)->activate();
                gGL.loadIdentity();
                gGL.matrixMode(LLRender::MM_MODELVIEW);
            }
        });
    }

    // drawMaterials' uniforms and units (LoopState's numbers for them).
    struct MaterialSlots
    {
        U32 specular = 0, intensity = 0, min_alpha = 0, brightness = 0;
        U32 norm_unit = 0, spec_unit = 0, diffuse_unit = 0;
    };

    // What drawMaterials' loop stages for a batch (on the main thread, or on
    // the threaded build's cull thread: the same steps).
    void stage_material_batch(LoopState& state, const MaterialSlots& s, const LLDrawInfo& params)
    {
        state.setUniform(s.specular, params.mSpecColor.mV);
        state.setUniform(s.intensity, &params.mEnvIntensity);
        state.setUniform(s.min_alpha, &params.mAlphaMaskCutoff);
        const F32 fullbright = params.mFullbright ? 1.f : 0.f;
        state.setUniform(s.brightness, &fullbright);
        // The official loop binds the normal and specular maps the program
        // samples when they change (never to none: such batches have them).
        if (params.mNormalMap.notNull())
        {
            state.setTexture(s.norm_unit, params.mNormalMap.get());
        }
        if (params.mSpecularMap.notNull())
        {
            state.setTexture(s.spec_unit, params.mSpecularMap.get());
        }
        state.setTexture(s.diffuse_unit, params.mTexture.get());
    }

    // ------------------------------------------------------------------
    // The threaded build (research/137 N11, LLRHI_DRAW_LISTS_THREADS with
    // THREADS_BUILD)
    //
    // A cull thread (LLRHI::sPostWork) runs drawMaterials' per batch steps
    // (stage_material_batch, LoopState::takes / stage / commit) over the
    // loop's batches, from the state the loop has when it opened, into
    // chunks: each chunk is the arrays of a list (a chunk's first draw names
    // its matrices), the state after its draws (for Sync) and the textures
    // its draws mark used. A chunk ends after its batches (32, 128, then
    // 256), or at a batch that goes the old way (the chunk holds it, after
    // its draws). The main thread takes
    // the chunks in order: Builder::addChunk (the backend records it, Sync
    // brings the viewer's caches to the chunk's state), the marks
    // (setActive), and the old way batch (the official body).
    //
    // What may run at the same time, and why nothing conflicts:
    //  - The cull thread reads: the batches (LLDrawInfo: the uniform values,
    //    the texture pointers, mVertexBuffer, mAvatar, mModelMatrix,
    //    mTextureMatrix, mOffset, mCount), their vertex buffers (isMapped,
    //    names, sizes, getAttributes), their textures' GL names,
    //    mTexOptionsDirty and GrayWire's holds (LLTexUnit::directName,
    //    LLImageGL::heldOnGL: mWireHold, mBandHold, mCreateHold, and
    //    sFrameCount / sLastFrameTime / research/278 sBandTime, which
    //    LLViewerTexture::updateClass moves), and research/276 in setTexture
    //    mIsMissingAsset (isMissingAsset) and mGrayWireGivenUp, and the
    //    matrices. It takes no reference (LLPointer::get only), calls no hook
    //    and no GL.
    //  - Those are written by the geometry rebuild, the object and texture
    //    updates (idle, updateImages): never inside the render pass. Inside
    //    it, by the main thread only: an old way batch's official body
    //    (bindFast makes a missing name or applies sampler options; setBuffer
    //    flushes a mapped buffer; research/276: GrayWire::isGray, in its
    //    Scope, clears mCreateHold and sets mGrayWireGivenUp). So the cull
    //    thread stops after a chunk that ends with an old way batch, until
    //    the main thread has drawn it (release), and is stopped before the
    //    main thread draws batches the old way for any other reason (stop).
    //  - Meanwhile the main thread writes: the backend (the chunk's list),
    //    LLGLSLShader::cachedUniform and LLTexUnit (Sync), texture states
    //    (setActive: the marks, and Sync's bindFast of a texture a chunk
    //    named, which has its GL name and no sampler options to apply, so
    //    bindFast writes nothing directName reads). The cull thread reads
    //    none of those.
    //  - The chunks: the cull thread writes a chunk and hands it over under
    //    the mutex; it never touches it again in that build. The main thread
    //    reads it after taking it under the mutex.
    constexpr U32 BUILD_MIN_BATCHES = 256;  // shorter loops are built on the main thread (not worth a hand-over)
    // The first chunk small (the main thread waits for it), then larger (each
    // chunk is a list: a readiness check, a Sync and a segment chain of its
    // own in the backend), x4 a chunk up to CHUNK_BATCHES.
    constexpr U32 FIRST_CHUNK_BATCHES = 32;
    constexpr U32 CHUNK_BATCHES = 256;

    struct Chunk
    {
        DL::Arrays arrays;
        LLDrawInfo** first = nullptr;   // its first batch
        LLDrawInfo** old = nullptr;     // the batch after its draws that goes the old way (null: none)
        LoopState state;                // after its draws (before the old batch's commit): what Sync sets
        std::vector<LLViewerTexture*> marks;    // textures its draws mark used (setActive, main thread)
        U32 draws = 0;
    };

    // A chunk's arrays as the sink (the matrices from the captured context).
    class ChunkSink : public LoopSink
    {
    public:
        ChunkSink(DL::Arrays& arrays, const DL::Context& list, const plus::drawlist::BatchContext& context)
            : mArrays(arrays), mList(list), mContext(context) {}
        U32 addAttributes(const LLRHI::DirectDraw::Attribute* attributes, U32 count) override { return mArrays.addAttributes(attributes, count); }
        U32 addTextures(const U32* names, U32 count) override { return mArrays.addTextures(names, count); }
        U32 addMatrices(const DL::Matrices& matrices) override { return mArrays.addMatrices(matrices); }
        U32 addFloats(const F32* values, U32 count) override { return mArrays.addFloats(values, count); }
        U32 addPatch(S32 slot, U32 count, U32 components, U32 rows, const F32* values) override
        {
            return mArrays.addPatch(slot, count, components, rows, values);
        }
        U32 addState(const DL::State& state) override { return mArrays.addState(state); }
        bool addDraw(const DL::Draw& draw) override { return mArrays.addDraw(draw, mList); }
        void matrices(const LLMatrix4* model, DL::Matrices& out) override
        {
            out = DL::Matrices();
            plus::drawlist::makeMatrices(mContext, model, out);     // fill_matrices' values, from the context
            out.id = 0;                                             // as fill_matrices: a row of the draw's own
        }

    private:
        DL::Arrays& mArrays;
        const DL::Context& mList;
        const plus::drawlist::BatchContext& mContext;
    };

    class MaterialBuild
    {
    public:
        static MaterialBuild& get()
        {
            static MaterialBuild sBuild;    // the loops run on the main thread one at a time
            return sBuild;
        }

        // Builds [begin, end) from `state` on a cull thread. False: none
        // (nothing changed; the caller builds on the main thread).
        bool start(LLDrawInfo** begin, LLDrawInfo** end, const LoopState& state, const MaterialSlots& slots, const DL::Context& list,
                   const plus::drawlist::BatchContext& context)
        {
            llassert(mPhase.load(std::memory_order_relaxed) == IDLE);
            mBegin = begin;
            mEnd = end;
            mStart = state;
            mSlots = slots;
            mList = list;
            mContext = context;
            mReady.clear();
            mNext = 0;
            mReleased = 0;
            mFinished = false;
            mFailedFrom = nullptr;
            mStop = false;
            mBuildNs = 0;
            mPhase.store(POSTED, std::memory_order_release);
            if (!LLRHI::sPostWork || !LLRHI::sPostWork(&MaterialBuild::task, this))
            {
                mPhase.store(IDLE, std::memory_order_relaxed);
                return false;
            }
            return true;
        }

        // The next chunk in order, once it is made; null: no more (the build
        // ended). waited_ns: how long it waited for it. Main thread.
        Chunk* next(U64& waited_ns)
        {
            const auto start = std::chrono::steady_clock::now();
            std::unique_lock<std::mutex> lock(mMutex);
            mReadyCv.wait(lock, [this] { return mReady.size() > mNext || mFinished; });
            waited_ns = (U64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
            return mReady.size() > mNext ? mReady[mNext++] : nullptr;
        }

        // The main thread drew the old way batch of the chunk next() gave
        // last: the build may read the batches after it.
        void release()
        {
            {
                std::lock_guard<std::mutex> lock(mMutex);
                mReleased = mNext;
            }
            mGoCv.notify_one();
        }

        // Ends the build (also one no thread has started) and returns once no
        // thread is in it. Main thread.
        void stop()
        {
            int expected = POSTED;
            if (mPhase.compare_exchange_strong(expected, IDLE, std::memory_order_acq_rel))
            {
                return;     // never started: the task finds it IDLE and does nothing
            }
            {
                std::lock_guard<std::mutex> lock(mMutex);
                mStop = true;
            }
            mGoCv.notify_one();
            std::unique_lock<std::mutex> lock(mMutex);
            mReadyCv.wait(lock, [this] { return mFinished; });
            mPhase.store(IDLE, std::memory_order_relaxed);
        }

        // After the build ended (next() gave null, or stop()): the cull thread's time.
        U64 buildNs() const { return mBuildNs; }
        // fix-threads4: after the build ended: the first batch of those no
        // chunk holds when the build failed (null: it did not).
        LLDrawInfo** failedFrom() const { return mFailedFrom; }

    private:
        enum : int { IDLE, POSTED, RUNNING };

        static void task(void* self)
        {
            MaterialBuild& build = *static_cast<MaterialBuild*>(self);
            int expected = POSTED;
            if (build.mPhase.compare_exchange_strong(expected, RUNNING, std::memory_order_acq_rel))
            {
                build.run();
            }
        }

        Chunk& chunk(size_t i)
        {
            while (mPool.size() <= i)
            {
                mPool.push_back(std::make_unique<Chunk>());
            }
            return *mPool[i];
        }

        // fix-threads4: the build on the cull thread. One that throws
        // (bad_alloc, ...) is logged (the first few) and ends there: the
        // batches no chunk handed over are drawn the old way by the main
        // thread (failedFrom), which never waits for a chunk that will not come.
        void run()
        {
            const auto start = std::chrono::steady_clock::now();
            LLDrawInfo** done = mBegin;     // the batches before it are in chunks handed over
            bool failed = false;
            try
            {
                build(done);
            }
            catch (...)
            {
                failed = true;
                static std::atomic<U32> sFailures{ 0 };
                if (sFailures.fetch_add(1, std::memory_order_relaxed) < 5)
                {
                    try
                    {
                        LL_WARNS("MaterialList") << "the threaded material build failed: the rest of the loop is drawn the old way" << LL_ENDL;
                    }
                    catch (...)
                    {
                    }
                }
            }
            mBuildNs = (U64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
            {
                std::lock_guard<std::mutex> lock(mMutex);
                mFailedFrom = failed && done != mEnd ? done : nullptr;
                mFinished = true;
            }
            mReadyCv.notify_one();
        }

        void build(LLDrawInfo**& done)
        {
            LoopState state = mStart;
            LLDrawInfo** i = mBegin;
            U32 chunk_batches = FIRST_CHUNK_BATCHES;
            for (size_t used = 0; i != mEnd; ++used)
            {
                {
                    std::lock_guard<std::mutex> lock(mMutex);
                    if (mStop)
                    {
                        break;
                    }
                }
                Chunk& c = chunk(used);
                c.arrays.clear();
                c.marks.clear();
                c.first = i;
                c.old = nullptr;
                c.draws = 0;
                ChunkSink sink(c.arrays, mList, mContext);
                state.have_matrices = false;    // a chunk is a list of its own: its first draw names its matrices
                for (U32 n = 0; i != mEnd && n < chunk_batches; ++n)
                {
                    const LLDrawInfo& params = **i;
                    stage_material_batch(state, mSlots, params);
                    if (!(state.takes(params) && state.stage(params, sink)))
                    {
                        c.old = i++;
                        break;
                    }
                    state.commit(true, &c.marks);
                    state.taken(params);
                    ++c.draws;
                    ++i;
                }
                c.state = state;
                {
                    std::lock_guard<std::mutex> lock(mMutex);
                    mReady.push_back(&c);
                    done = i;
                }
                mReadyCv.notify_one();
                if (c.old)
                {
                    // Loop::drewOldWay's part that is the state's; then wait
                    // for the main thread to draw the batch through GL.
                    state.commit(false, nullptr);
                    state.have_matrices = false;
                    std::unique_lock<std::mutex> lock(mMutex);
                    const size_t mine = mReady.size();
                    mGoCv.wait(lock, [this, mine] { return mReleased >= mine || mStop; });
                }
                chunk_batches = llmin(chunk_batches * 4, CHUNK_BATCHES);
            }
        }

        std::atomic<int> mPhase{ IDLE };
        std::mutex mMutex;
        std::condition_variable mReadyCv;   // a chunk is ready, or the build ended
        std::condition_variable mGoCv;      // an old way batch was drawn, or stop
        // Set by start(), read by the cull thread (handed over by the post).
        LLDrawInfo** mBegin = nullptr;
        LLDrawInfo** mEnd = nullptr;
        LoopState mStart;
        MaterialSlots mSlots;
        DL::Context mList;
        plus::drawlist::BatchContext mContext;
        // The cull thread's while it builds (the main thread's between builds).
        std::vector<std::unique_ptr<Chunk>> mPool;
        U64 mBuildNs = 0;
        // Under mMutex.
        std::vector<Chunk*> mReady;
        size_t mReleased = 0;
        bool mFinished = false;
        LLDrawInfo** mFailedFrom = nullptr;  // fix-threads4: set with mFinished
        bool mStop = false;
        size_t mNext = 0;                   // main thread: the next chunk next() hands out
    };

    // drawMaterials' loop with the threaded build. False: it did not start
    // (nothing changed: the loop runs on the main thread). True: every batch
    // was drawn, in order.
    bool draw_materials_threaded(Loop& loop, const MaterialPass& pass, const MaterialSlots& slots, LLDrawInfo** begin, LLDrawInfo** end)
    {
        // The light path only (LLRHI_DRAW_LISTS=1): the reference path stays the plain loop to compare with.
        // Not while the cull threads are off (LLRHI::sPostWork would refuse every build).
        if (!(DL::sThreads & DL::THREADS_BUILD) || DL::sMode != 1 || !LLRHI::sCullThreads || end - begin < (ptrdiff_t)BUILD_MIN_BATCHES || loop.hasFullUnit())
        {
            return false;
        }
        DL::Builder& list = DL::Builder::get();
        plus::drawlist::BatchContext context;
        context.capture(*pass.shader);      // gGL's matrices as Loop::open synced them
        MaterialBuild& build = MaterialBuild::get();
        if (!build.start(begin, end, loop.state(), slots, list.context(), context))
        {
            ++sBuildStats.no_thread;
            return false;
        }
        // Stops the build however this ends (it reads the batches).
        struct Stop
        {
            MaterialBuild& build;
            Loop& loop;
            ~Stop()
            {
                build.stop();
                loop.syncFrom(nullptr);
            }
        } stop{ build, loop };
        ++sBuildStats.loops;
        LLDrawInfo** old_from = nullptr;    // the backend refused a chunk: the old way from there on
        U64 waited_ns = 0;
        while (Chunk* chunk = build.next(waited_ns))
        {
            sBuildStats.wait_ns += waited_ns;
            ++sBuildStats.chunks;
            // The Builder's own arrays are empty here (nothing in this loop
            // adds to them), so addChunk's Sync is the chunk's only.
            loop.syncFrom(&chunk->state);
            if (!list.addChunk(chunk->arrays))
            {
                old_from = chunk->first;
                break;
            }
            sBuildStats.draws += chunk->draws;
            for (LLViewerTexture* texture : chunk->marks)
            {
                texture->setActive();   // commit's bindFast bookkeeping (main thread)
            }
            if (chunk->old)
            {
                LLDrawInfo& params = **chunk->old;
                draw_material_batch(pass, params);
                if (params.mTextureMatrix)
                {
                    gGL.syncMatrices();     // Loop::drewOldWay's part that is the main thread's
                }
                ++sBuildStats.old_path;
                build.release();
            }
        }
        build.stop();
        sBuildStats.build_ns += build.buildNs();
        loop.syncFrom(nullptr);
        if (!old_from)
        {
            old_from = build.failedFrom();  // fix-threads4: the build failed: the rest the old way
        }
        if (old_from)
        {
            // Not ready any more (as the Builder answers each batch then):
            // the rest the old way, as the loop on the main thread would.
            ++sBuildStats.refused;
            for (LLDrawInfo** i = old_from; i != end; ++i)
            {
                LLDrawInfo& params = **i;
                draw_material_batch(pass, params);
                if (params.mTextureMatrix)
                {
                    gGL.syncMatrices();
                }
                ++sBuildStats.old_path;
            }
        }
        return true;
    }
}

namespace plus
{
namespace materiallist
{
    void takeBuildStats(BuildStats& out)
    {
        out = sBuildStats;
        sBuildStats = BuildStats();
    }

    bool drawMaterials(LLGLSLShader* shader, LLDrawInfo** begin, LLDrawInfo** end, S32 diffuse_channel, S32 spec_channel,
                       S32 norm_channel)
    {
        if (!DL::wanted(DL::KIND_MATERIAL) || begin == end || !shader)
        {
            return false;
        }
        LL_PROFILE_ZONE_SCOPED_CATEGORY_MATERIAL;

        Loop loop(shader);
        MaterialSlots slots;
        slots.specular = loop.addUniform(LLShaderMgr::SPECULAR_COLOR, 4);
        slots.intensity = loop.addUniform(LLShaderMgr::ENVIRONMENT_INTENSITY, 1);
        slots.min_alpha = loop.addUniform(LLShaderMgr::MINIMUM_ALPHA, 1);
        slots.brightness = loop.addUniform(LLShaderMgr::EMISSIVE_BRIGHTNESS, 1);
        slots.norm_unit = loop.addUnit(norm_channel, false);
        slots.spec_unit = loop.addUnit(spec_channel, false);
        slots.diffuse_unit = loop.addUnit(diffuse_channel, false, true);
        if (!loop.open("materials"))
        {
            return false;
        }

        MaterialPass pass;
        pass.shader = shader;
        pass.specular = shader->getUniformLocation(LLShaderMgr::SPECULAR_COLOR) > -1;
        pass.intensity = shader->getUniformLocation(LLShaderMgr::ENVIRONMENT_INTENSITY) > -1;
        pass.min_alpha = shader->getUniformLocation(LLShaderMgr::MINIMUM_ALPHA) > -1;
        pass.brightness = shader->getUniformLocation(LLShaderMgr::EMISSIVE_BRIGHTNESS) > -1;
        pass.diffuse_channel = diffuse_channel;
        pass.spec_channel = spec_channel;
        pass.norm_channel = norm_channel;

        if (draw_materials_threaded(loop, pass, slots, begin, end))
        {
            loop.submit();
            return true;
        }
        for (LLDrawInfo** i = begin; i != end; )
        {
            LLDrawInfo& params = **i;
            LLCullResult::increment_iterator(i, end);

            stage_material_batch(loop.state(), slots, params);
            if (!loop.add(params))
            {
                draw_material_batch(pass, params);
                loop.drewOldWay(params);
            }
        }
        loop.submit();
        return true;
    }

    bool drawBumpDeferred(LLRenderPass& pool, LLDrawInfo** begin, LLDrawInfo** end, S32 diffuse_channel, S32 bump_channel)
    {
        LLGLSLShader* shader = LLGLSLShader::sCurBoundShaderPtr;
        if (!DL::wanted(DL::KIND_MATERIAL) || begin == end || !shader)
        {
            return false;
        }
        LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;

        Loop loop(shader);
        const U32 min_alpha = loop.addUniform(LLShaderMgr::MINIMUM_ALPHA, 1);
        // bindBumpMap binds with bind(); pushBumpBatch (shiny, as renderDeferred
        // sets it) binds the diffuse texture only above shader level 1.
        const U32 bump_unit = loop.addUnit(bump_channel, true);
        const U32 diffuse_unit = loop.addUnit(pool.getShaderLevel() > 1 ? diffuse_channel : -1, false, true);
        if (!loop.open("bump"))
        {
            return false;
        }

        for (LLDrawInfo** i = begin; i != end; )
        {
            LLDrawInfo& params = **i;
            LLCullResult::increment_iterator(i, end);

            loop.setUniform(min_alpha, &params.mAlphaMaskCutoff);
            if (LLViewerTexture* bump = bump_map(params))
            {
                loop.setTexture(bump_unit, bump);   // without one, the unit keeps its binding
            }
            loop.setTexture(diffuse_unit, params.mTexture);
            if (!loop.add(params))
            {
                shader->setMinimumAlpha(params.mAlphaMaskCutoff);
                LLDrawPoolBump::bindBumpMap(params, bump_channel);
                pool.pushBumpBatch(params, true, false);
                loop.drewOldWay(params);
            }
        }
        loop.submit();
        return true;
    }

    bool drawBumpPost(LLRenderPass& pool, U32 type)
    {
        LLGLSLShader* shader = LLGLSLShader::sCurBoundShaderPtr;
        if (!DL::wanted(DL::KIND_MATERIAL) || !shader)
        {
            return false;
        }
        LLDrawInfo** begin = gPipeline.beginRenderMap(type);
        LLDrawInfo** end = gPipeline.endRenderMap(type);
        if (begin == end)
        {
            return false;
        }
        LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;

        // bindBumpMap(params) binds the bump map to units 1 and 0 with bindFast.
        Loop loop(shader);
        const U32 unit1 = loop.addUnit(1, false);
        const U32 unit0 = loop.addUnit(0, false);
        if (!loop.open("bump post"))
        {
            return false;
        }

        for (LLDrawInfo** i = begin; i != end; ++i)
        {
            LLDrawInfo& params = **i;
            LLViewerTexture* bump = bump_map(params);
            if (!bump)
            {
                continue;   // the official loop draws only batches with a bump map
            }
            loop.setTexture(unit1, bump);
            loop.setTexture(unit0, bump);
            if (!loop.add(params))
            {
                if (LLDrawPoolBump::bindBumpMap(params))
                {
                    pool.pushBumpBatch(params, false);
                }
                loop.drewOldWay(params);
            }
        }
        loop.submit();
        return true;
    }
}
}

// ----------------------------------------------------------------------------
// research/180 stage 2 (9 2): the material loop's textures by name.

namespace
{
    constexpr U32 KEEP_UNIT = LLRHI::DirectDraw::KEEP_UNIT;
    constexpr U32 MAX_DIRECT_UNITS = 32;    // LLTexUnit's units (a direct draw's table)

    // DirectUnits' counts since the last line (main thread).
    struct NameCounts
    {
        U64 frames = 0;
        U32 last_frame = 0;
        U64 by_name = 0;        // batches drawn with their textures by name
        U64 refused = 0;        // bound as the official loop and drawn the GL way
        U64 kept = 0;           // textures bound to their unit instead (no GL name yet, sampler options to set)
    };
    NameCounts sNames;
    F32 sNamesLoggedAt = 0.f;
}

namespace plus
{
namespace materiallist
{
    DirectUnits::DirectUnits(S32 diffuse_channel, S32 spec_channel, S32 norm_channel)
    {
        mDiffuse.unit = diffuse_channel;
        mSpecular.unit = spec_channel;
        mNormal.unit = norm_channel;
        const S32 top = llmax(diffuse_channel, spec_channel, norm_channel);
        const bool direct = top >= 0 && top < (S32)MAX_DIRECT_UNITS && LLRHI::sGLDrawsDisabled && LLRHI::sDirect && LLRHI::sDirectDraw;
        mByName = direct;   // research/218 vk-s7g: no switch, no verify (research/207 7.1 T8)
        mCount = direct ? (U32)top + 1 : 0;
    }

    DirectUnits::~DirectUnits()
    {
        if (!mByName)
        {
            return;
        }
        NameCounts& n = sNames;
        if (n.last_frame != gFrameCount)
        {
            n.last_frame = gFrameCount;
            ++n.frames;
        }
        if (gFrameTimeSeconds - sNamesLoggedAt < 30.f)
        {
            return;
        }
        if (sNamesLoggedAt > 0.f)
        {
            const F64 f = (F64)llmax(n.frames, (U64)1);
            LL_INFOS("MaterialNames") << llformat("material loop textures by name [on] over %llu frames, per frame: by name %.0f, "
                                                  "refused %.1f, bound to the unit %.1f",
                                                  (unsigned long long)n.frames, n.by_name / f, n.refused / f, n.kept / f)
                                      << LL_ENDL;
        }
        sNamesLoggedAt = gFrameTimeSeconds;
        n = NameCounts();
        n.last_frame = gFrameCount;
    }

    // The name the draw takes for the unit: worked out when the batch's
    // texture changes, as the official loop binds. A texture not ready to go
    // by name is bound to its unit (directTexture), as bindFast would.
    U32 DirectUnits::nameOf(Channel& c, LLTexture* texture, bool diffuse)
    {
        // research/243: known by the texture the unit draws (GrayWire's look
        // on unit 0 while its Scope is on), so a batch outside the Scope with
        // the same texture does not take the look's name.
        texture = LLTexUnit::unitTexture(texture, c.unit);
        if (c.have && c.named == texture && (c.name != KEEP_UNIT || c.bound == texture))
        {
            return c.name;
        }
        U32 name = KEEP_UNIT;
        if (!texture)
        {
            // No diffuse map: unbindFast binds the white texture. No normal or
            // specular map: the official loop leaves the unit as it is.
            name = diffuse ? LLTexUnit::sWhiteTexture : KEEP_UNIT;
        }
        else
        {
            name = gGL.getTexUnit(c.unit)->directTexture(texture);
            if (name == KEEP_UNIT)
            {
                c.bound = texture;      // bindFast put it on the unit
                ++sNames.kept;
            }
        }
        c.named = texture;
        c.name = name;
        c.have = true;
        return name;
    }

    // What the official loop's binds would leave on the unit (it binds when
    // the batch's texture differs from its last one).
    void DirectUnits::bindAsOfficial(Channel& c, LLTexture* texture, bool diffuse)
    {
        texture = LLTexUnit::unitTexture(texture, c.unit);  // research/243: as nameOf (bindFast binds the look the same)
        if (c.unit < 0 || texture == c.bound || (!texture && !diffuse))
        {
            return;
        }
        c.bound = texture;
        if (texture)
        {
            gGL.getTexUnit(c.unit)->bindFast(texture);
        }
        else
        {
            gGL.getTexUnit(c.unit)->unbindFast(LLTexUnit::TT_TEXTURE);
        }
    }

    bool DirectUnits::draw(LLDrawInfo& params)
    {
        if (!mByName)
        {
            return false;
        }
        U32 textures[MAX_DIRECT_UNITS];
        std::fill(textures, textures + mCount, KEEP_UNIT);
        // In the official loop's order: normal, specular, diffuse.
        if (mNormal.unit >= 0)
        {
            textures[mNormal.unit] = nameOf(mNormal, params.mNormalMap.get(), false);
        }
        if (mSpecular.unit >= 0)
        {
            textures[mSpecular.unit] = nameOf(mSpecular, params.mSpecularMap.get(), false);
        }
        if (mDiffuse.unit >= 0)
        {
            textures[mDiffuse.unit] = nameOf(mDiffuse, params.mTexture.get(), true);
        }
        // texture_matrix false: the loop loaded the batch's texture matrix through GL already (as drawGeometry).
        if (LLRenderPass::pushDirectBatch(params, false, false, textures, mCount, false))
        {
            ++sNames.by_name;
            // research/218 vk-s7f: a texture still given by its GL name (the old path, research/207 7.1).
            for (U32 u = 0; u < mCount; ++u)
            {
                const U32 t = textures[u];
                if (t && t != KEEP_UNIT && t != LLTexUnit::sWhiteTexture && !LLRHI::isTexHandle(t))
                {
                    LLRHI::sOldMaterialNames.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
            }
            return true;
        }
        // Refused: the units as the official loop would have them, then the GL way.
        bindAsOfficial(mNormal, params.mNormalMap.get(), false);
        bindAsOfficial(mSpecular, params.mSpecularMap.get(), false);
        bindAsOfficial(mDiffuse, params.mTexture.get(), true);
        if (params.mTextureMatrix)
        {
            gGL.getTexUnit(0)->activate();  // the loop activates unit 0 for the texture matrix after its binds
        }
        ++sNames.refused;
        return false;
    }
}
}

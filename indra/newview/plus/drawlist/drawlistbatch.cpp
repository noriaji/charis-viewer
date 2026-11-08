/**
 * @file drawlistbatch.cpp
 * @brief Draw lists for the draw pools (research/137 N2): a batch
 *        (LLDrawInfo) made into a draw list's Draw.
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

// The values are the ones LLRenderPass::pushDirectBatch (lldrawpool.cpp)
// makes, its matrices LLRender::fillDirectMatrices' arithmetic (llrender.cpp),
// its texture names LLTexUnit::directName's read; the palette is what
// upload_palette and LLRenderPass::rhiPaletteRow there decide, the hair sway
// uniform what plus::hairsway::bindShader sets, the GLTF units what
// LLFetchedGLTFMaterial::bindDirect picks. When those change, this has to
// follow (tools/shadowed-functions.py).

#include "llviewerprecompiledheaders.h"

#include "drawlistbatch.h"

#include "llappviewer.h"        // gFrameCount
#include "lldrawpool.h"
#include "llfetchedgltfmaterial.h"
#include "llgltexture.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llshadermgr.h"
#include "llspatialpartition.h" // LLDrawInfo
#include "llvertexbuffer.h"
#include "llviewertexture.h"
#include "llvoavatar.h"
#include "pipeline.h"
#include "plus/hairsway/hairsway.h"
#include "plus/skinsss/skinsss.h"

#include "glm/gtc/type_ptr.hpp"

#include <chrono>
#include <cstring>

namespace plus::drawlist
{
namespace
{
    using LLRHI::DrawList::Builder;
    using LLRHI::DrawList::NONE;

    constexpr U32 MAX_UNITS = 32;       // pushDirectBatch's MAX_UNITS
    static_assert(MAX_UNITS >= LL_NUM_TEXTURE_LAYERS, "bindDirect's units beyond the draw's (bound through GL) cannot happen");

    Stats sStats;

    // plus::hairsway::bindShader's uniform.
    const LLStaticHashedString& hair_sway_uniform()
    {
        static const LLStaticHashedString sUniform("plusHairSway");
        return sUniform;
    }

    S32 location_of_index(const LLGLSLShader* shader, U32 index)
    {
        return index < shader->mUniform.size() ? shader->mUniform[index] : -1;
    }

    // LLGLSLShader::nativeSlotOfIndex / nativeSlotOfLocation (private there),
    // from the same public tables.
    S32 native_slot_of_index(const LLGLSLShader* shader, U32 index)
    {
        return index < shader->mNativeSlot.size() ? shader->mNativeSlot[index] : -1;
    }

    S32 native_slot_of_location(const LLGLSLShader* shader, S32 location)
    {
        if (shader->mNativeSlotByLocation.empty())
        {
            return -1;
        }
        const auto it = shader->mNativeSlotByLocation.find(location);
        return it != shader->mNativeSlotByLocation.end() ? it->second : -1;
    }

    // A uniform the native shader API can set (LLGLSLShader's native_uniform):
    // the program does not have it, or it has a native slot.
    bool slot_usable(const LLGLSLShader* shader, S32 location, S32 slot)
    {
        return location < 0 || (shader->mNativeProgram && slot >= 0);
    }

    // As rhiPaletteRow, without plus/skinonce's shortcut: sPlacePalette keeps a
    // row that is this frame's and places it otherwise, so the row named by the
    // draw is always this frame's (a kept row may be an older frame's, which
    // sUsePaletteRow would refuse; a list draw cannot fall back by itself).
    bool place_row(const LLVOAvatar::MatrixPaletteCache& mpc)
    {
        if (!LLRHI::sPlacePalette || mpc.mGLMp.empty())
        {
            return false;
        }
        LLRHI::PaletteRow& row = mpc.mRHIRow;
        const U32 vec4s = (U32)(mpc.mGLMp.size() / 4);
        if (!LLRHI::sAvatarStats)
        {
            return LLRHI::sPlacePalette(mpc.mGLMp.data(), vec4s, row);
        }
        const LLRHI::PaletteRow before = row;
        const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        const bool ok = LLRHI::sPlacePalette(mpc.mGLMp.data(), vec4s, row);
        if (ok && (row.frame != before.frame || row.base != before.base))
        {
            LLRHI::AvatarCounters& c = LLRHI::sAvatarCounters;
            ++c.palette_places;
            c.palette_place_ns += (U64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
        }
        return ok;
    }

    // The Builder's open list as a sink.
    class BuilderSink : public DrawSink
    {
    public:
        U32 addAttributes(const LLRHI::DirectDraw::Attribute* attributes, U32 count) override { return mList.addAttributes(attributes, count); }
        U32 addTextures(const U32* names, U32 count) override { return mList.addTextures(names, count); }
        U32 addMatrices(const LLRHI::DrawList::Matrices& matrices) override { return mList.addMatrices(matrices); }
        U32 addFloats(const F32* values, U32 count) override { return mList.addFloats(values, count); }
        U32 addPatch(S32 slot, U32 count, U32 components, U32 rows, const F32* values) override
        {
            return mList.addPatch(slot, count, components, rows, values);
        }
        U32 addState(const LLRHI::DrawList::State& state) override { return mList.addState(state); }

    private:
        Builder& mList = Builder::get();
    };
}

// fillDirectMatrices' arithmetic from the captured context, for the
// matrix uniforms the program has (the others stay 0: no uniform to set).
void makeMatrices(const BatchContext& context, const LLMatrix4* model, LLRHI::DrawList::Matrices& out)
{
    glm::mat4 modelview = glm::make_mat4(context.view);
    if (model)
    {
        modelview *= glm::make_mat4((const F32*)model->mMatrix);
    }
    memcpy(out.modelview, glm::value_ptr(modelview), sizeof(out.modelview));
    if (context.has_normal || context.has_inverse)
    {
        const glm::mat4 inverse = glm::inverse(modelview);
        if (context.has_inverse)
        {
            memcpy(out.inverse_modelview, glm::value_ptr(inverse), sizeof(out.inverse_modelview));
        }
        if (context.has_normal)
        {
            const glm::mat4 normal_matrix = glm::transpose(inverse);
            const F32* norm = glm::value_ptr(normal_matrix);
            const F32 norm_mat[] =
            {
                norm[0], norm[1], norm[2],
                norm[4], norm[5], norm[6],
                norm[8], norm[9], norm[10]
            };
            memcpy(out.normal, norm_mat, sizeof(norm_mat));
        }
    }
    if (context.has_modelview_projection)
    {
        const glm::mat4 modelview_projection = glm::make_mat4(context.projection) * modelview;
        memcpy(out.modelview_projection, glm::value_ptr(modelview_projection), sizeof(out.modelview_projection));
    }
}

Needs& Needs::uniform(U32 index)
{
    llassert(uniform_count < MAX_UNIFORMS);
    if (uniform_count < MAX_UNIFORMS)
    {
        uniforms[uniform_count++] = index;
    }
    return *this;
}

void takeStats(Stats& out)
{
    out = sStats;
    sStats = Stats();
}

// ----------------------------------------------------------------------
// The pure part

void BatchContext::capture(const LLGLSLShader& shader)
{
    program = &shader;
    attribute_mask = shader.mAttributeMask;
    // As fillDirectMatrices asks the program.
    has_normal = location_of_index(&shader, LLShaderMgr::NORMAL_MATRIX) > -1;
    has_inverse = location_of_index(&shader, LLShaderMgr::INVERSE_MODELVIEW_MATRIX) >= 0;
    has_modelview_projection = location_of_index(&shader, LLShaderMgr::MODELVIEW_PROJECTION_MATRIX) > -1;
    memcpy(view, gGLModelView, sizeof(view));
    memcpy(projection, glm::value_ptr(gGL.getProjectionMatrix()), sizeof(projection));
    frame = gFrameCount;
    palette_slot = location_of_index(&shader, LLShaderMgr::AVATAR_MATRIX) >= 0 && shader.mNativeProgram
                       ? native_slot_of_index(&shader, LLShaderMgr::AVATAR_MATRIX)
                       : -1;
    // LLFetchedGLTFMaterial::getBindTextures' uniforms, in its order.
    static const S32 gltf_uniforms[4] = { LLShaderMgr::DIFFUSE_MAP, LLShaderMgr::BUMP_MAP, LLShaderMgr::SPECULAR_MAP, LLShaderMgr::EMISSIVE_MAP };
    for (U32 i = 0; i < 4; ++i)
    {
        const S32 uniform = gltf_uniforms[i];
        const S32 unit = uniform >= 0 && uniform < (S32)shader.mTexture.size() ? shader.getTextureChannel(uniform) : -1;
        gltf_units[i] = unit >= 0 && (U32)unit < LL_NUM_TEXTURE_LAYERS ? unit : -1;
    }
}

bool readTextureName(LLGLTexture* texture, U32& name, std::vector<LLGLTexture*>& set_active)
{
    // directName: the name, or KEEP_UNIT when directTexture would bind it through GL.
    const U32 read = texture ? LLTexUnit::directName(texture) : LLRHI::DirectDraw::KEEP_UNIT;
    if (read == LLRHI::DirectDraw::KEEP_UNIT)
    {
        return false;
    }
    if (texture->getTextureState() == LLGLTexture::DELETED)
    {
        set_active.push_back(texture);
    }
    name = read;
    return true;
}

Made makeDraw(const BatchContext& context, BatchCaches& caches, DrawSink& sink, const LLDrawInfo& params,
              Textures textures, bool batch_textures, const Extras* extras, const U32* units, U32 unit_count,
              const BatchPalette* palette, const BatchPatch* patches, U32 patch_count,
              std::vector<LLGLTexture*>& set_active, LLRHI::DrawList::Draw& draw)
{
    draw = LLRHI::DrawList::Draw();
    const size_t marks = set_active.size();

    // Textures as pushBatch binds them (units without one keep their binding).
    U32 names[MAX_UNITS];
    U32 texture_count = 0;
    const LLMatrix4* texture_matrix = nullptr;
    bool named = true;
    switch (textures)
    {
    case Textures::NONE:
        break;
    case Textures::UNITS:
        texture_count = llmin(unit_count, MAX_UNITS);
        if (texture_count)
        {
            memcpy(names, units, texture_count * sizeof(U32));
        }
        texture_matrix = params.mTextureMatrix;
        break;
    case Textures::GLTF:
    {
        const LLFetchedGLTFMaterial* mat = params.mGLTFMaterial.get();
        LLViewerTexture* gltf[LLFetchedGLTFMaterial::MAX_DIRECT_TEXTURES] = {};
        const U32 count = mat ? mat->getDirectTextures(params.mTexture.get(), gltf) : 0;
        for (U32 i = 0; i < count && i < 4 && named; ++i)
        {
            const S32 unit = context.gltf_units[i];
            if (unit < 0)
            {
                continue;   // the program does not sample it
            }
            while (texture_count <= (U32)unit)
            {
                names[texture_count++] = LLRHI::DirectDraw::KEEP_UNIT;
            }
            named = readTextureName(gltf[i], names[unit], set_active);
        }
        texture_matrix = params.mTextureMatrix;
        break;
    }
    case Textures::BATCH:
        if (batch_textures && params.mTextureList.size() > 1)
        {
            texture_count = (U32)llmin(params.mTextureList.size(), (size_t)MAX_UNITS);
            for (U32 i = 0; i < texture_count && named; ++i)
            {
                const LLPointer<LLViewerTexture>& texture = params.mTextureList[i];
                if (texture.notNull())
                {
                    named = readTextureName(texture.get(), names[i], set_active);
                }
                else
                {
                    names[i] = LLRHI::DirectDraw::KEEP_UNIT;
                }
            }
        }
        else
        {
            texture_count = 1;
            if (params.mTexture.notNull())
            {
                named = readTextureName(params.mTexture.get(), names[0], set_active);
                texture_matrix = params.mTextureMatrix;
            }
            else
            {
                names[0] = 0;   // unbound, as unbindFast leaves it
            }
        }
        break;
    }
    if (!named)
    {
        set_active.resize(marks);   // the old way marks them itself
        return Made::OLD_TEXTURE;
    }
    if (texture_count)
    {
        draw.first_texture = sink.addTextures(names, texture_count);
        draw.texture_count = texture_count;
    }
    if (texture_matrix)
    {
        if (caches.texture_matrix != texture_matrix || caches.texture_matrix_first == NONE)
        {
            caches.texture_matrix = texture_matrix;
            caches.texture_matrix_first = sink.addFloats((const F32*)texture_matrix->mMatrix, 16);
        }
        draw.texture_matrix = caches.texture_matrix_first;
    }

    // Buffers and range.
    const LLVertexBuffer* vb = params.mVertexBuffer;
    draw.vertex_buffer = vb->getGLBuffer();
    draw.index_buffer = vb->getGLIndices();
    draw.index_type = vb->getIndicesType();
    draw.first = params.mOffset;
    draw.count = params.mCount;
    draw.buffer_vertices = vb->getNumVerts();
    draw.buffer_indices = vb->getNumIndices();

    // Vertex data as setBuffer() lays it out for the program.
    if (caches.attribute_buffer != vb || caches.attribute_name != vb->getGLBuffer())
    {
        LLRHI::DirectDraw::Attribute attributes[LLRHI::DirectDraw::MAX_ATTRIBUTES];
        caches.attribute_count = vb->getAttributes(context.attribute_mask, attributes);
        caches.attribute_first = sink.addAttributes(attributes, caches.attribute_count);
        caches.attribute_buffer = vb;
        caches.attribute_name = vb->getGLBuffer();
    }
    draw.first_attribute = caches.attribute_first;
    draw.attribute_count = caches.attribute_count;

    // The matrices applyModelMatrix and syncMatrices would set; most batches
    // share the pass's (the view, projection, frame and program are the
    // context's, so the model matrix is the key).
    if (caches.matrix_entry == NONE || caches.matrix_model != params.mModelMatrix)
    {
        LLRHI::DrawList::Matrices matrices = {};
        makeMatrices(context, params.mModelMatrix, matrices);
        matrices.id = LLRHI::DrawList::newMatrixId();
        caches.matrix_model = params.mModelMatrix;
        caches.matrix_entry = sink.addMatrices(matrices);
    }
    draw.matrices = caches.matrix_entry;

    // State: the extras, then the palette's patch and the uniforms in one run.
    LLRHI::DrawList::State state;
    if (extras)
    {
        memcpy(state.blend, extras->blend, sizeof(state.blend));
        state.cull = extras->cull;
        draw.oit_priority = extras->oit_priority;
    }
    U32 first_patch = NONE;
    U32 count = 0;
    auto note = [&](U32 patch)
    {
        if (first_patch == NONE)
        {
            first_patch = patch;
        }
        llassert(patch == first_patch + count);
        ++count;
    };
    if (palette)
    {
        if (palette->row_base != NONE)
        {
            draw.palette_base = palette->row_base;
        }
        else if (palette->joints && context.palette_slot >= 0)
        {
            note(sink.addPatch(context.palette_slot, palette->joint_count, 3, 4, palette->joints));
        }
    }
    for (U32 i = 0; i < patch_count; ++i)
    {
        const BatchPatch& p = patches[i];
        note(sink.addPatch(p.slot, p.count, p.components, p.rows, p.values));
    }
    if (count)
    {
        state.first_patch = first_patch;
        state.patch_count = count;
    }
    draw.state = sink.addState(state);
    return Made::DRAW;
}

// ----------------------------------------------------------------------
// The loop's side

// static
BatchList& BatchList::get()
{
    static BatchList sList;
    return sList;
}

bool BatchList::open(const char* name, LLRHI::DrawList::Kind kind, const Needs& needs)
{
    if (mOpen)
    {
        close();
    }
    // Mirror mode draws through GL too, and lists replace direct draws.
    if (!LLRHI::DrawList::wanted(kind) || !LLRHI::sGLDrawsDisabled || !LLRHI::sDirect || !LLRHI::sDirectDraw ||
        LLPipeline::sShadowRender)
    {
        return false;
    }
    LLGLSLShader* shader = LLGLSLShader::sCurBoundShaderPtr;
    if (!shader)
    {
        return false;
    }
    for (U32 i = 0; i < needs.uniform_count; ++i)
    {
        const U32 index = needs.uniforms[i];
        if (!slot_usable(shader, location_of_index(shader, index), native_slot_of_index(shader, index)))
        {
            return false;
        }
    }
    if (needs.rigged)
    {
        const S32 sway = shader->getUniformLocation(hair_sway_uniform());
        if (!slot_usable(shader, location_of_index(shader, LLShaderMgr::AVATAR_MATRIX), native_slot_of_index(shader, LLShaderMgr::AVATAR_MATRIX)) ||
            !slot_usable(shader, sway, native_slot_of_location(shader, sway)))
        {
            return false;
        }
    }

    mShader = shader;
    mTracked.clear();
    if (needs.rigged)
    {
        tracked(0, &hair_sway_uniform());   // its location now (with gDebugGL a GL query), not inside the list
    }

    // Projection, lights and the pass's modelview reach the program as
    // pushDirectBatch has them sent (once: gGL does not change in the loop).
    gGL.syncMatrices();
    Builder& builder = Builder::get();
    if (!builder.open(name, kind, &BatchList::syncThunk, this))
    {
        mTracked.clear();
        mShader = nullptr;
        return false;
    }
    mOpen = true;
    mFailed = false;
    mName = name;
    mKind = kind;
    mSerial = builder.serial();
    mContext.capture(*shader);
    mCaches.reset();
    mSetActive.clear();
    mPending = PendingPalette();
    mPlans.clear();
    mNextPlan = 0;
    mPrepAvatar = nullptr;
    mPrepMesh = 0;
    mPrepSkip = false;
    ++sStats.loops;
    return true;
}

void BatchList::close()
{
    if (!mOpen)
    {
        return;
    }
    if (mPending.active)
    {
        // Never left by the loops (add or drawOld takes it); as the old way if it were.
        beginOldPath();
        endOldPath();
    }
    mOpen = false;
    Builder& builder = Builder::get();
    if (builder.isOpen() && builder.serial() == mSerial)
    {
        builder.submit();   // calls syncThunk
    }
    syncTracked();          // values set after the last draw, or the list was submitted by another open
    mTracked.clear();
    mPlans.clear();
    mShader = nullptr;
}

bool BatchList::takes(const LLDrawInfo& params)
{
    if (mFailed)
    {
        return false;
    }
    const LLVertexBuffer* vb = params.mVertexBuffer;
    if (!vb || vb->isMapped())
    {
        ++sStats.old_mapped;
        return false;
    }
    // plus/skinsss's BatchScope sets the stencil reference through GL around
    // such a batch (research/137 section 2.3, promise 6): an avatar's, or a
    // rigid object worn on an eye attachment point (fix-round41).
    if (plus::skinsss::detail::sMarking &&
        (params.mAvatar.notNull() || plus::skinsss::detail::isEyeAttachment(params)))
    {
        ++sStats.old_marked;
        return false;
    }
    return true;
}

bool BatchList::ensureOpen()
{
    if (!mOpen || mFailed)
    {
        return false;
    }
    Builder& builder = Builder::get();
    if (LLGLSLShader::sCurBoundShaderPtr != mShader)
    {
        // Something bound another program inside the loop: the tracked slots
        // and the context are the old program's. The rest of the loop goes
        // the old way.
        if (builder.isOpen() && builder.serial() == mSerial)
        {
            builder.submit();
        }
        mFailed = true;
        return false;
    }
    if (builder.isOpen() && builder.serial() == mSerial)
    {
        return true;
    }
    // Another list was opened (it submitted this one): open this one again.
    gGL.syncMatrices();
    if (!builder.open(mName, mKind, &BatchList::syncThunk, this))
    {
        mFailed = true;
        return false;
    }
    mSerial = builder.serial();
    mContext.capture(*mShader);
    mCaches.reset();            // the arrays were cleared
    for (Tracked& t : mTracked)
    {
        t.known_valid = false;  // the other list may have set them
    }
    return true;
}

BatchList::Tracked* BatchList::tracked(U32 index, const LLStaticHashedString* name)
{
    for (Tracked& t : mTracked)
    {
        if (t.name == name && (name || t.index == index))
        {
            return t.location >= 0 ? &t : nullptr;
        }
    }
    Tracked t;
    t.index = index;
    t.name = name;
    t.location = name ? mShader->getUniformLocation(*name) : location_of_index(mShader, index);
    t.slot = name ? native_slot_of_location(mShader, t.location) : native_slot_of_index(mShader, index);
    if (!slot_usable(mShader, t.location, t.slot))
    {
        t.slot = -1;        // set the usual way (after a cut) each time
    }
    mTracked.push_back(t);
    return mTracked.back().location >= 0 ? &mTracked.back() : nullptr;
}

void BatchList::setTracked(Tracked& t, U32 count, U32 components, const F32* values)
{
    const U32 floats = count * components;
    if (t.slot < 0 || floats > Tracked::MAX_FLOATS || !components || components > 4)
    {
        // No patch for it: through the usual setter, with no draw pending.
        Builder::cut("plus::drawlist uniform without a slot");
        if (t.name)
        {
            mShader->uniform1f(*t.name, values[0]);
        }
        else
        {
            switch (components)
            {
            case 1: mShader->uniform1fv(t.index, count, values); break;
            case 2: mShader->uniform2fv(t.index, count, values); break;
            case 3: mShader->uniform3fv(t.index, count, values); break;
            default: mShader->uniform4fv(t.index, count, values); break;
            }
        }
        t.pending = false;
        t.known_valid = false;
        t.viewer_dirty = false;
        return;
    }
    if (t.count != count || t.components != components)
    {
        t.count = count;
        t.components = components;
        t.known_valid = false;
    }
    memcpy(t.wanted, values, floats * sizeof(F32));
    t.pending = !(t.known_valid && memcmp(t.known, t.wanted, floats * sizeof(F32)) == 0);
}

void BatchList::uniform(U32 index, U32 count, U32 components, const F32* values)
{
    if (!mOpen || mFailed)
    {
        // The rest of the loop goes the old way: set it as the loop would.
        if (LLGLSLShader* shader = LLGLSLShader::sCurBoundShaderPtr)
        {
            switch (components)
            {
            case 1: shader->uniform1fv(index, count, values); break;
            case 2: shader->uniform2fv(index, count, values); break;
            case 3: shader->uniform3fv(index, count, values); break;
            default: shader->uniform4fv(index, count, values); break;
            }
        }
        return;
    }
    if (Tracked* t = tracked(index, nullptr))
    {
        setTracked(*t, count, components, values);
    }
}

void BatchList::uniform(const LLStaticHashedString& name, F32 value)
{
    if (!mOpen || mFailed)
    {
        if (LLGLSLShader* shader = LLGLSLShader::sCurBoundShaderPtr)
        {
            shader->uniform1f(name, value);
        }
        return;
    }
    if (Tracked* t = tracked(0, &name))
    {
        setTracked(*t, 1, 1, &value);
    }
}

void BatchList::syncTracked()
{
    LLGLSLShader* shader = LLGLSLShader::sCurBoundShaderPtr;
    const bool same = shader && shader == mShader;
    for (Tracked& t : mTracked)
    {
        if (t.location < 0 || (!t.pending && !t.viewer_dirty))
        {
            continue;
        }
        if (same)
        {
            // The backend may hold another value than the cache says (a patch):
            // set it whatever the cache has, so both end as the loop left them.
            shader->forgetUniform(t.location);
            if (t.name)
            {
                shader->uniform1f(*t.name, t.wanted[0]);
            }
            else
            {
                switch (t.components)
                {
                case 1: shader->uniform1fv(t.index, t.count, t.wanted); break;
                case 2: shader->uniform2fv(t.index, t.count, t.wanted); break;
                case 3: shader->uniform3fv(t.index, t.count, t.wanted); break;
                default: shader->uniform4fv(t.index, t.count, t.wanted); break;
                }
            }
            memcpy(t.known, t.wanted, sizeof(t.known));
            t.known_valid = true;
        }
        else
        {
            // Not the program any more: its cache must not claim a value the
            // backend may not have (the next setter sends it).
            if (mShader)
            {
                mShader->forgetUniform(t.location);
            }
            // A value set since the program changed (binding one cuts, and that
            // sync ran with the old program bound) is the bound program's, as
            // the old loop's setter would have set it.
            if (t.pending && shader)
            {
                if (t.name)
                {
                    shader->uniform1f(*t.name, t.wanted[0]);
                }
                else
                {
                    switch (t.components)
                    {
                    case 1: shader->uniform1fv(t.index, t.count, t.wanted); break;
                    case 2: shader->uniform2fv(t.index, t.count, t.wanted); break;
                    case 3: shader->uniform3fv(t.index, t.count, t.wanted); break;
                    default: shader->uniform4fv(t.index, t.count, t.wanted); break;
                    }
                }
            }
            t.known_valid = false;
        }
        t.pending = false;
        t.viewer_dirty = false;
    }
}

// static
void BatchList::syncThunk(void* user)
{
    static_cast<BatchList*>(user)->syncTracked();
}

void BatchList::preparePalette(const LLDrawInfo& params)
{
    // uploadMatrixPalette(avatar, skinInfo, lastAvatar, lastMeshId, skipLastSkin)'s
    // decisions, made before the loop.
    PalettePlan plan;
    plan.batch = &params;
    LLVOAvatar* avatar = params.mAvatar.get();
    LLMeshSkinInfo* info = params.mSkinInfo;
    llassert(info);
    if (avatar && info)
    {
        if (avatar == mPrepAvatar && info->mHash == mPrepMesh)
        {
            plan.drawn = !mPrepSkip;
        }
        else
        {
            const LLVOAvatar::MatrixPaletteCache& mpc = avatar->updateSkinInfoMatrixPalette(info);
            mPrepSkip = mpc.mMatrixPalette.empty();     // skin info not loaded yet: not drawn
            mPrepAvatar = avatar;
            mPrepMesh = info->mHash;
            plan.drawn = !mPrepSkip;
            if (plan.drawn)
            {
                plan.change = true;
                plan.avatar = avatar;
                plan.skin = info;
                plan.sway = (F32)plus::hairsway::swayIndex(mpc.mMatrixPalette);
                // upload_palette: this frame's row when the program reads rows, else the joints.
                // The row is placed on the same condition as rhiPaletteRow there (so the
                // frame's table fills as with the old loop), and named when the program
                // takes rows (sUsePaletteRow's answer).
                if (LLRHI::sPalettePrebuild && LLRHI::sUsePaletteRow && mShader->mNativeProgram && place_row(mpc) &&
                    Builder::get().context().palette_rows)
                {
                    plan.palette.row_base = mpc.mRHIRow.base;
                }
                else if (mContext.palette_slot >= 0 && mpc.mGLMp.size() >= mpc.mMatrixPalette.size() * 12)
                {
                    plan.palette.joints = mpc.mGLMp.data();
                    plan.palette.joint_count = (U32)mpc.mMatrixPalette.size();
                }
            }
        }
    }
    mPlans.push_back(plan);
}

bool BatchList::palette(const LLDrawInfo& params, bool in_list)
{
    llassert(mNextPlan < mPlans.size() && mPlans[mNextPlan].batch == &params);
    if (mNextPlan >= mPlans.size() || mPlans[mNextPlan].batch != &params)
    {
        // Not prepared (a loop's bug): the official upload, as the old loop would.
        const LLVOAvatar* last = nullptr;
        U64 mesh = 0;
        bool skip = false;
        beginOldPath();
        const bool drawn = LLRenderPass::uploadMatrixPalette(params.mAvatar.get(), params.mSkinInfo, last, mesh, skip);
        endOldPath();
        return drawn;
    }
    const PalettePlan& plan = mPlans[mNextPlan++];
    if (!plan.change)
    {
        return plan.drawn;
    }
    mPending.active = true;
    mPending.avatar = plan.avatar;
    mPending.skin = plan.skin;
    mPending.palette = plan.palette;
    if (in_list && mOpen && !mFailed)
    {
        // plus::hairsway::bindShader: the index of the sway entries (0: none),
        // set once the feature has been on (a value of 0 is set only over
        // another value, which is the same as never setting it).
        if (Tracked* t = tracked(0, &hair_sway_uniform()))
        {
            F32 current = 0.f;
            if (t->pending)
            {
                current = t->wanted[0];
            }
            else if (t->known_valid)
            {
                current = t->known[0];
            }
            else
            {
                const LLVector4* cached = mShader->cachedUniform(t->location);
                current = cached ? cached->mV[0] : 0.f;
            }
            if (plan.sway != 0.f || current != 0.f)
            {
                setTracked(*t, 1, 1, &plan.sway);
            }
        }
    }
    return true;
}

void BatchList::applyPendingPalette()
{
    if (!mPending.active)
    {
        return;
    }
    mPending.active = false;
    // The official upload of this frame's palette (built already: a lookup).
    LLRenderPass::uploadMatrixPalette(mPending.avatar, mPending.skin);
}

bool BatchList::add(LLDrawInfo& params, Textures textures, bool batch_textures, const Extras* extras, const U32* units, U32 unit_count)
{
    if (!params.mCount)
    {
        // pushBatch draws nothing; a palette set for the batch is set all the same.
        if (mPending.active)
        {
            beginOldPath();
            endOldPath();
        }
        return true;
    }
    if (!ensureOpen())
    {
        return false;
    }

    // The uniforms set since the last draw.
    BatchPatch patches[16];
    Tracked* patched[16];
    U32 patch_count = 0;
    for (Tracked& t : mTracked)
    {
        if (t.pending && t.location >= 0 && patch_count < 16)
        {
            patches[patch_count].slot = t.slot;
            patches[patch_count].count = t.count;
            patches[patch_count].components = t.components;
            patches[patch_count].values = t.wanted;
            patched[patch_count++] = &t;
        }
    }

    BuilderSink sink;
    LLRHI::DrawList::Draw draw;
    const size_t marks = mSetActive.size();
    const Made made = makeDraw(mContext, mCaches, sink, params, textures, batch_textures, extras, units, unit_count,
                               mPending.active ? &mPending.palette : nullptr, patches, patch_count, mSetActive, draw);
    if (made != Made::DRAW)
    {
        ++sStats.old_texture;
        return false;
    }
    if (!Builder::get().addDraw(draw))
    {
        mSetActive.resize(marks);
        ++sStats.old_refused;
        return false;   // drawOld sets the palette and the uniforms the usual way
    }

    // Taken: the textures' marks, what the backend has now, the counts.
    for (size_t i = marks; i < mSetActive.size(); ++i)
    {
        mSetActive[i]->setActive();
        ++sStats.marked_active;
    }
    mSetActive.resize(marks);
    for (U32 i = 0; i < patch_count; ++i)
    {
        Tracked& t = *patched[i];
        memcpy(t.known, t.wanted, sizeof(t.known));
        t.known_valid = true;
        t.pending = false;
        t.viewer_dirty = true;
        ++sStats.uniform_patches;
    }
    if (mPending.active)
    {
        mPending.active = false;
        const bool row = mPending.palette.row_base != NONE;
        if (row)
        {
            ++sStats.palette_rows;
        }
        else if (mPending.palette.joints)
        {
            ++sStats.palette_patches;
        }
        if (LLRHI::sAvatarStats)
        {
            ++(row ? LLRHI::sAvatarCounters.palette_row_uses : LLRHI::sAvatarCounters.palette_uploads);
        }
    }
    if (LLRHI::sAvatarStats && params.mAvatar.notNull())
    {
        LLRenderPass::rhiCountRigged(params);   // as pushDirectBatch counts every rigged batch
    }
    ++sStats.draws;
    return true;
}

void BatchList::beginOldPath()
{
    // Nothing of the old way may happen with list draws pending: some of it
    // changes GL state through GL itself (plus/skinsss's stencil reference).
    Builder::cut("plus::drawlist old path");
    syncTracked();
    applyPendingPalette();
}

void BatchList::endOldPath()
{
    // The old way set what it set the usual way: the backend's values are the
    // viewer's again, and none of the tracked ones is known any more.
    for (Tracked& t : mTracked)
    {
        t.pending = false;
        t.known_valid = false;
        t.viewer_dirty = false;
    }
    if (mOpen && !mFailed)
    {
        gGL.syncMatrices();     // as the next batch's pushDirectBatch would
    }
}
}

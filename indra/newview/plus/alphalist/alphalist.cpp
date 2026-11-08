/**
 * @file alphalist.cpp
 * @brief Alpha draw lists: the alpha pool's batch loop as draw lists
 *        (research/137 N3).
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

// How the old path draws an alpha batch (LLDrawPoolAlpha::renderAlpha, official
// code with the LLRHI lines; research/137 N3):
//  - per group: the particles' culling off (LLGLDisable for the group), the
//    OIT priority of its batches;
//  - per batch: the program (bindDeferredShaderFast when it changes; it binds
//    the deferred textures and uniforms too), the specular colour,
//    environment intensity and brightness uniforms, the hybrid OIT session's
//    half (attachments' batches by whether their program makes nodes), the
//    skinning palette (uploadMatrixPalette), the textures and texture matrix
//    (TexSetup), the blend function, a minimum alpha of 0 for custom blend
//    functions (back to 1/255 after the draw), then LLRenderPass::drawGeometry
//    (one direct draw: the translation layer resolves program, units,
//    uniforms and blend again for each);
//  - after the group's batches: the glow pass of its emissive batches, with
//    their own programs and an additive blend into the alpha channel.
//
// renderAlphaList below is that loop again (shadowed function: keep it in
// step with renderAlpha). While the program stays the same its batches go
// into one draw list (llrhi/llrhidrawlist.h); what a batch sets on the way is
// written as the old path's calls would leave it (Recorder: uniforms only
// where the shader's value cache would send them, the units as bindFast leaves
// them) and goes with the draw. Whatever cannot go with a draw submits the
// list first and runs the original code (the same calls in the same order).
// After each submission Recorder::sync sets the viewer's caches the usual way
// to what the old path would have left (promise 4).

#include "llviewerprecompiledheaders.h"

#include "plus.h"

#include "alphalist.h"

#include "lldrawpoolalpha.h"

#include "llglheaders.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llrhi.h"
#include "llrhidrawlist.h"
#include "llshadermgr.h"
#include "llspatialpartition.h"
#include "llstaticstringtable.h"
#include "llvertexbuffer.h"
#include "llviewerregion.h"
#include "llviewershadermgr.h"
#include "llviewertexture.h"
#include "llvoavatar.h"
#include "llenvironment.h"
#include "llfetchedgltfmaterial.h"
#include "pipeline.h"
#include "llscenealpha.h"

#include "plus/alphapass/alphapass.h"
#include "plus/dofalpha/dofalpha.h"
#include "plus/hairsway/hairsway.h"
#include "plus/skinonce/skinonce.h"
#include "plus/skinsss/skinsss.h"

#include <glm/gtc/type_ptr.hpp>

#include <vector>

// lldrawpoolalpha.cpp (research/41): the OIT priority of an alpha group's
// batches, and whether a non-rigged alpha group is an attachment's.
U32 oit_group_priority(LLSpatialGroup* group, bool rigged);
bool oit_avatar_group(LLSpatialGroup* group);

#define current_shader (LLGLSLShader::sCurBoundShaderPtr)

namespace plus::alphalist
{
namespace
{
    using LLRHI::DrawList::Builder;
    using LLRHI::DrawList::NONE;

    // lldrawpoolalpha.cpp's MINIMUM_ALPHA: the minimum alpha before discarding a fragment (~1/255).
    constexpr F32 MINIMUM_ALPHA = 0.004f;
    // lldrawpoolalpha.cpp's GLTF_DIRECT_UNITS (LLFetchedGLTFMaterial::bindDirect).
    constexpr U32 GLTF_DIRECT_UNITS = 32;
    // Units a list draw names (LLRHI::DirectDraw's textures, as pushDirectBatch's MAX_UNITS).
    constexpr U32 MAX_UNITS = 32;
    // The hair sway uniform plus/hairsway's bindShader sets (hairsway.cpp, sUniform).
    const LLStaticHashedString sHairSwayUniform("plusHairSway");

    // LLRender::eBlendFactor to GL, as llrender.cpp's sGLBlendFactor.
    constexpr U32 GL_BLEND_FACTOR[] =
    {
        GL_ONE,
        GL_ZERO,
        GL_DST_COLOR,
        GL_SRC_COLOR,
        GL_ONE_MINUS_DST_COLOR,
        GL_ONE_MINUS_SRC_COLOR,
        GL_DST_ALPHA,
        GL_SRC_ALPHA,
        GL_ONE_MINUS_DST_ALPHA,
        GL_ONE_MINUS_SRC_ALPHA,
        GL_ZERO     // BF_UNDEF
    };

    const char* const CUT_NAMES[CUT_COUNT] =
    {
        "program", "gltf", "palette", "texture", "buffer", "uniforms", "not taken", "glow", "oit variant", "end"
    };

    Stats sStats;

    // A texture TexSetup binds: on `unit`, as bindFast; null: unbindFast(TT_TEXTURE).
    struct UnitTexture
    {
        S32 unit = 0;
        LLTexture* texture = nullptr;
    };
    // TexSetup binds at most two maps by channel and a batch's textures.
    constexpr U32 MAX_UNIT_TEXTURES = MAX_UNITS + 2;

    // What TexSetup(draw, use_material) binds, in its order, and the texture
    // matrix it loads (null: none). Too many textures for a list draw end
    // with one on unit MAX_UNITS, which no list draw takes.
    U32 collect_textures(const LLDrawInfo& draw, bool use_material, LLGLSLShader* shader, LLGLSLShader* simple_shader,
                         UnitTexture* out, const LLMatrix4*& texture_matrix)
    {
        U32 count = 0;
        texture_matrix = nullptr;
        if (draw.mGLTFMaterial)
        {
            texture_matrix = draw.mTextureMatrix;   // a GLTF material (not blended): the texture matrix only
            return 0;
        }
        // LLGLSLShader::bindTexture's unit for a sampler uniform (-1: none).
        auto channel = [shader](S32 uniform)
        {
            return uniform >= 0 && uniform < (S32)shader->mTexture.size() ? shader->mTexture[uniform] : -1;
        };
        auto bind = [&](S32 unit, LLTexture* texture)
        {
            if (unit < 0)
            {
                return;
            }
            if (count == MAX_UNIT_TEXTURES)
            {
                out[count - 1].unit = (S32)MAX_UNITS;
                return;
            }
            out[count].unit = unit;
            out[count].texture = texture;
            ++count;
        };

        if (!LLPipeline::sRenderingHUDs && use_material && shader)
        {
            if (draw.mNormalMap)
            {
                bind(channel(LLShaderMgr::BUMP_MAP), draw.mNormalMap.get());
            }
            if (draw.mSpecularMap)
            {
                bind(channel(LLShaderMgr::SPECULAR_MAP), draw.mSpecularMap.get());
            }
        }
        else if (shader == simple_shader || shader == simple_shader->mRiggedVariant)
        {
            bind(channel(LLShaderMgr::BUMP_MAP), LLViewerFetchedTexture::sFlatNormalImagep.get());
            bind(channel(LLShaderMgr::SPECULAR_MAP), LLViewerFetchedTexture::sWhiteImagep.get());
        }
        if (draw.mTextureList.size() > 1)
        {
            for (U32 i = 0; i < draw.mTextureList.size(); ++i)
            {
                if (draw.mTextureList[i].notNull())
                {
                    bind((S32)i, draw.mTextureList[i].get());
                }
            }
        }
        else if (draw.mTexture.notNull())
        {
            bind(use_material ? channel(LLShaderMgr::DIFFUSE_MAP) : 0, draw.mTexture.get());
            texture_matrix = draw.mTextureMatrix;
        }
        else
        {
            bind(0, nullptr);
        }
        return count;
    }

    // The program's native slot of a uniform, as LLGLSLShader::nativeSlotOfIndex
    // and nativeSlotOfLocation (private) answer it from its public tables.
    S32 native_slot_of_index(const LLGLSLShader* shader, U32 index)
    {
        return index < shader->mNativeSlot.size() ? shader->mNativeSlot[index] : -1;
    }

    S32 native_slot_of_location(const LLGLSLShader* shader, GLint location)
    {
        auto found = shader->mNativeSlotByLocation.find(location);
        return found != shader->mNativeSlotByLocation.end() ? found->second : -1;
    }

    enum class Added { LISTED, SKIPPED, OLD_PATH };

    // The open list of one renderAlphaList call and what its draws changed
    // that the viewer's caches do not know yet. Main thread.
    class Recorder
    {
    public:
        static Recorder& get()
        {
            static Recorder recorder;
            return recorder;
        }

        void beginPass()
        {
            llassert(!mOpen && mTracked.empty() && !mBlendWant && !mOITWant && !mUnits);
            mPrograms.clear();
            mRefusedShader = nullptr;
            ++sStats.passes;
        }

        // A list of ours is open (no other open() submitted it).
        bool isOpen() const
        {
            return mOpen && mList.isOpen() && mList.serial() == mSerial;
        }

        // Submits the list, if one is open, and sets what the loop wrote
        // while it was open the usual way: the uniforms (sync(), which the
        // Builder calls only after submissions that recorded draws), the
        // blend and the units (the backend's blend went back to the head's,
        // which LLRender's cache still says; its units were never changed).
        void end(Cut why)
        {
            if (isOpen())
            {
                mList.submit();
                ++sStats.cuts[why];
            }
            mOpen = false;
            sync();
            if (mBlendWant)
            {
                gGL.blendFunc(mBlend[0], mBlend[1], mBlend[2], mBlend[3]);
                mBlendWant = false;
            }
            for (U32 unit = 0; unit < mUnits; ++unit)
            {
                if (mUnitNames[unit] == LLRHI::DirectDraw::KEEP_UNIT)
                {
                    continue;
                }
                if (mUnitTextures[unit])
                {
                    gGL.getTexUnit(unit)->bindFast(mUnitTextures[unit]);
                }
                else
                {
                    gGL.getTexUnit(unit)->unbindFast(LLTexUnit::TT_TEXTURE);
                }
            }
            resetUnits();
        }

        // The loop's uniform writes: kept for the next draw while a list is
        // open, else set the usual way.
        void setMaterialUniforms(LLGLSLShader* shader, const LLVector4& spec_color, F32 env_intensity, F32 brightness)
        {
            if (!isOpen())
            {
                shader->uniform4f(LLShaderMgr::SPECULAR_COLOR, spec_color.mV[VRED], spec_color.mV[VGREEN], spec_color.mV[VBLUE], spec_color.mV[VALPHA]);
                shader->uniform1f(LLShaderMgr::ENVIRONMENT_INTENSITY, env_intensity);
                shader->uniform1f(LLShaderMgr::EMISSIVE_BRIGHTNESS, brightness);
                return;
            }
            track(shader, LLShaderMgr::SPECULAR_COLOR, nullptr, spec_color.mV, 4);
            track(shader, LLShaderMgr::ENVIRONMENT_INTENSITY, nullptr, &env_intensity, 1);
            track(shader, LLShaderMgr::EMISSIVE_BRIGHTNESS, nullptr, &brightness, 1);
        }

        void setBlend(LLRender::eBlendFactor color_s, LLRender::eBlendFactor color_d, LLRender::eBlendFactor alpha_s, LLRender::eBlendFactor alpha_d)
        {
            if (!isOpen())
            {
                gGL.blendFunc(color_s, color_d, alpha_s, alpha_d);
                return;
            }
            mBlendWant = true;
            mBlend[0] = color_s;
            mBlend[1] = color_d;
            mBlend[2] = alpha_s;
            mBlend[3] = alpha_d;
        }

        // LLRHI::sOITPriority must not change while a list is open (llrhidrawlist.h).
        void setOITPriority(U32 priority)
        {
            if (!isOpen())
            {
                LLRHI::sOITPriority = priority;
                return;
            }
            mOITWant = true;
            mOITPriority = priority;
        }

        U32 oitPriority() const
        {
            return mOITWant ? mOITPriority : LLRHI::sOITPriority;
        }

        // Whether the program's draws make OIT nodes (LLRHI::sOITVariant, a
        // hook: asked once per program and pass, with no list open).
        bool oitNodes(LLGLSLShader* shader)
        {
            Program& program = programInfo(shader);
            if (!program.nodes_known)
            {
                end(CUT_OIT_VARIANT);
                program.nodes = shader && LLRHI::sOITVariant && LLRHI::sOITVariant(shader->mProgramObject);
                program.nodes_known = true;
            }
            return program.nodes;
        }

        // A batch without a GLTF material, after its program is bound and its
        // uniforms written (setMaterialUniforms): into the list (LISTED), not
        // drawn (SKIPPED: its palette is not loaded yet), or OLD_PATH: the
        // caller submits the list (end(why)) and draws it the original way.
        // The palette trackers are the loop's (uploadMatrixPalette's).
        Added addBatch(LLDrawInfo& params, const UnitTexture* textures, U32 texture_count, const LLMatrix4* texture_matrix,
                       bool cull_off, LLRender::eBlendFactor blend_s, LLRender::eBlendFactor blend_d,
                       LLRender::eBlendFactor alpha_s, LLRender::eBlendFactor alpha_d, bool custom_blend,
                       const LLVOAvatar*& last_avatar, U64& last_mesh, const LLGLSLShader*& last_avatar_shader, bool& skip_last_skin,
                       Cut& why);

    private:
        Recorder() : mList(Builder::get())
        {
            resetUnits();
        }

        // A uniform the list's draws set (or the loop wrote while it was open).
        struct Tracked
        {
            LLGLSLShader* shader = nullptr;
            U32 index = 0;                              // LLShaderMgr's uniform index (name null)
            const LLStaticHashedString* name = nullptr; // or by name
            S32 slot = -1;                              // the program's native slot
            U32 components = 1;                         // 1 or 4
            bool sent_known = false;                    // sent: the backend's value (the shader's cache had it, or a patch set it)
            bool patching = false;                      // in the draw being added
            F32 sent[4] = {};
            F32 want[4] = {};                           // what the old path would have set by now
        };

        struct Program
        {
            LLGLSLShader* shader = nullptr;
            bool lists = false;         // every uniform the loop sets has a native slot
            bool nodes_known = false;
            bool nodes = false;
        };

        static void sync(void* user)
        {
            static_cast<Recorder*>(user)->sync();
        }
        void sync();
        bool ensureOpen(LLGLSLShader* shader);
        Program& programInfo(LLGLSLShader* shader);
        void track(LLGLSLShader* shader, U32 index, const LLStaticHashedString* name, const F32* values, U32 components);
        bool hasValue(LLGLSLShader* shader, const LLStaticHashedString& name) const;
        void setUsual(const Tracked& tracked, const F32* values);
        U32 matrices(const LLMatrix4* model);
        void resetUnits();

        Builder& mList;
        bool mOpen = false;
        U32 mSerial = 0;
        LLGLSLShader* mShader = nullptr;            // the open list's program
        LLGLSLShader* mRefusedShader = nullptr;     // open() said no with this program bound (this pass)
        std::vector<Program> mPrograms;             // this pass's

        std::vector<Tracked> mTracked;
        bool mBlendWant = false;                    // mBlend: the old path's blend now (set by end())
        LLRender::eBlendFactor mBlend[4] = {};
        bool mOITWant = false;
        U32 mOITPriority = 0;

        // The units the list's draws changed, as bindFast / unbindFast left them.
        U32 mUnitNames[MAX_UNITS];                  // KEEP_UNIT: not changed
        LLTexture* mUnitTextures[MAX_UNITS] = {};
        U32 mUnits = 0;                             // units 0..mUnits-1 named by the draws
        bool mUnitsDirty = true;                    // not handed to the list since the last change
        U32 mFirstTexture = NONE;

        bool mMatricesValid = false;                // the list has the matrices of mLastModel
        const LLMatrix4* mLastModel = nullptr;
    };

    void Recorder::resetUnits()
    {
        for (U32 i = 0; i < MAX_UNITS; ++i)
        {
            mUnitNames[i] = LLRHI::DirectDraw::KEEP_UNIT;
            mUnitTextures[i] = nullptr;
        }
        mUnits = 0;
        mUnitsDirty = true;
        mFirstTexture = NONE;
    }

    Recorder::Program& Recorder::programInfo(LLGLSLShader* shader)
    {
        for (Program& program : mPrograms)
        {
            if (program.shader == shader)
            {
                return program;
            }
        }
        Program program;
        program.shader = shader;
        program.lists = shader && shader->mNativeProgram;
        // The uniforms the loop sets on the batch's program (setMaterialUniforms,
        // setMinimumAlpha, hair sway): present ones need a native slot, as
        // LLGLSLShader sets them through GL otherwise.
        const U32 indices[] = { LLShaderMgr::SPECULAR_COLOR, LLShaderMgr::ENVIRONMENT_INTENSITY, LLShaderMgr::EMISSIVE_BRIGHTNESS,
                                LLShaderMgr::MINIMUM_ALPHA };
        for (U32 index : indices)
        {
            if (program.lists && index < shader->mUniform.size() && shader->mUniform[index] >= 0 && native_slot_of_index(shader, index) < 0)
            {
                program.lists = false;
            }
        }
        if (program.lists)
        {
            const GLint location = shader->getUniformLocation(sHairSwayUniform);
            program.lists = location < 0 || native_slot_of_location(shader, location) >= 0;
        }
        mPrograms.push_back(program);
        return mPrograms.back();
    }

    bool Recorder::ensureOpen(LLGLSLShader* shader)
    {
        if (isOpen())
        {
            llassert(mShader == shader);    // a new program ends the list first
            return true;
        }
        mOpen = false;
        if (shader == mRefusedShader)
        {
            return false;
        }
        // What pushDirectBatch does before each direct draw: the projection,
        // lights and the pass's modelview reach the program as through GL.
        // gGL's matrices do not change while the list is open.
        gGL.syncMatrices();
        if (!mList.open("alpha", LLRHI::DrawList::KIND_ALPHA, &Recorder::sync, this))
        {
            mRefusedShader = shader;
            return false;
        }
        mOpen = true;
        mSerial = mList.serial();
        mShader = shader;
        mMatricesValid = false;
        mLastModel = nullptr;
        resetUnits();
        return true;
    }

    void Recorder::track(LLGLSLShader* shader, U32 index, const LLStaticHashedString* name, const F32* values, U32 components)
    {
        Tracked* tracked = nullptr;
        for (Tracked& t : mTracked)
        {
            if (t.shader == shader && t.index == index && t.name == name)
            {
                tracked = &t;
                break;
            }
        }
        if (!tracked)
        {
            const GLint location = name ? shader->getUniformLocation(*name)
                                        : (index < shader->mUniform.size() ? shader->mUniform[index] : -1);
            if (location < 0)
            {
                return;     // not in the program: LLGLSLShader::uniform* sets nothing either
            }
            Tracked t;
            t.shader = shader;
            t.index = index;
            t.name = name;
            t.slot = name ? native_slot_of_location(shader, location) : native_slot_of_index(shader, index);
            t.components = components;
            // The shader's value cache holds what the backend has (the usual
            // way keeps them equal; only open lists differ, and sync() makes
            // them equal again).
            if (const LLVector4* found = shader->cachedUniform(location))
            {
                t.sent_known = true;
                for (U32 i = 0; i < 4; ++i)
                {
                    t.sent[i] = found->mV[i];
                }
            }
            mTracked.push_back(t);
            tracked = &mTracked.back();
        }
        for (U32 i = 0; i < 4; ++i)
        {
            tracked->want[i] = i < components ? values[i] : 0.f;
        }
    }

    bool Recorder::hasValue(LLGLSLShader* shader, const LLStaticHashedString& name) const
    {
        const GLint location = shader->getUniformLocation(name);
        return location >= 0 && shader->cachedUniform(location) != nullptr;
    }

    void Recorder::setUsual(const Tracked& tracked, const F32* v)
    {
        if (tracked.name)
        {
            tracked.shader->uniform1f(*tracked.name, v[0]);
        }
        else if (tracked.components == 4)
        {
            tracked.shader->uniform4f(tracked.index, v[0], v[1], v[2], v[3]);
        }
        else
        {
            tracked.shader->uniform1f(tracked.index, v[0]);
        }
    }

    // After each submission that recorded draws (the Builder's Sync: the end
    // of the list, or a cut by the guard, after which the list goes on) and
    // at end(): the uniforms the loop wrote, and the OIT priority, are set
    // the usual way to what the old path leaves. Never the blend or the units
    // (a cut may come from inside LLRender or LLTexUnit, between a cache and
    // its hook): end() sets those. The list's program is still the bound one
    // (a cut by bind() comes before it changes sCurBoundShaderPtr). No list
    // is open here.
    void Recorder::sync()
    {
        for (const Tracked& t : mTracked)
        {
            const bool differs = !t.sent_known || memcmp(t.sent, t.want, sizeof(F32) * t.components) != 0;
            if (t.sent_known && differs)
            {
                setUsual(t, t.sent);    // the cache first takes the backend's value (sends nothing new)
            }
            setUsual(t, t.want);
        }
        mTracked.clear();

        if (mOITWant)
        {
            LLRHI::sOITPriority = mOITPriority;
            mOITWant = false;
        }
    }

    // pushDirectBatch's matrices for a batch (LLRender::fillDirectMatrices'
    // arithmetic, all four): NONE while the model matrix stays the same.
    U32 Recorder::matrices(const LLMatrix4* model)
    {
        if (mMatricesValid && model == mLastModel)
        {
            return NONE;
        }
        glm::mat4 modelview = glm::make_mat4(gGLModelView);
        if (model)
        {
            modelview *= glm::make_mat4((const F32*)model->mMatrix);
        }
        const glm::mat4 projection = gGL.getProjectionMatrix() * modelview;
        const glm::mat4 inverse = glm::inverse(modelview);
        const glm::mat4 normal = glm::transpose(inverse);
        const F32* n = glm::value_ptr(normal);

        LLRHI::DrawList::Matrices m;
        memcpy(m.modelview, glm::value_ptr(modelview), sizeof(m.modelview));
        memcpy(m.modelview_projection, glm::value_ptr(projection), sizeof(m.modelview_projection));
        memcpy(m.inverse_modelview, glm::value_ptr(inverse), sizeof(m.inverse_modelview));
        const F32 normal3[9] = { n[0], n[1], n[2], n[4], n[5], n[6], n[8], n[9], n[10] };
        memcpy(m.normal, normal3, sizeof(m.normal));
        m.id = LLRHI::DrawList::newMatrixId();

        mMatricesValid = true;
        mLastModel = model;
        return mList.addMatrices(m);
    }

    Added Recorder::addBatch(LLDrawInfo& params, const UnitTexture* textures, U32 texture_count, const LLMatrix4* texture_matrix,
                             bool cull_off, LLRender::eBlendFactor blend_s, LLRender::eBlendFactor blend_d,
                             LLRender::eBlendFactor alpha_s, LLRender::eBlendFactor alpha_d, bool custom_blend,
                             const LLVOAvatar*& last_avatar, U64& last_mesh, const LLGLSLShader*& last_avatar_shader, bool& skip_last_skin,
                             Cut& why)
    {
        LLGLSLShader* shader = current_shader;
        LLVertexBuffer* vb = params.mVertexBuffer;
        if (!shader || !programInfo(shader).lists)
        {
            why = CUT_UNIFORMS;
            return Added::OLD_PATH;
        }
        if (!vb || vb->isMapped())
        {
            why = CUT_BUFFER;   // pushDirectBatch would refuse it: drawGeometry draws it through GL
            return Added::OLD_PATH;
        }
        for (U32 i = 0; i < texture_count; ++i)
        {
            // directTexture would bind it through GL (bindFast) and keep the unit.
            if (textures[i].unit >= (S32)MAX_UNITS ||
                (textures[i].texture && LLTexUnit::directName(textures[i].texture) == LLRHI::DirectDraw::KEEP_UNIT))
            {
                why = CUT_TEXTURE;
                return Added::OLD_PATH;
            }
        }
        if (!ensureOpen(shader))
        {
            why = CUT_NOT_TAKEN;
            return Added::OLD_PATH;
        }
        for (U32 i = 0; i < texture_count; ++i)
        {
            // A unit the program samples through a descriptor image takes no
            // texture of the draw's own (Arrays::addDraw would refuse the draw).
            if (mList.context().descriptor_units & (1u << textures[i].unit))
            {
                why = CUT_TEXTURE;
                return Added::OLD_PATH;
            }
        }

        // The palette, as uploadMatrixPalette decides with the loop's trackers.
        // The trackers change only once the draw is in the list (or skipped).
        U32 palette_base = NONE;
        bool new_skin = false;
        U32 sway = 0;
        bool set_sway = false;
        if (params.mAvatar.notNull())
        {
            LLVOAvatar* avatar = params.mAvatar;
            const LLMeshSkinInfo* skin = params.mSkinInfo;
            llassert(skin);
            if (avatar == last_avatar && skin->mHash == last_mesh && last_avatar_shader == shader)
            {
                if (skip_last_skin)
                {
                    return Added::SKIPPED;
                }
            }
            else
            {
                const LLVOAvatar::MatrixPaletteCache& mpc = avatar->updateSkinInfoMatrixPalette(skin);
                if (mpc.mMatrixPalette.empty())
                {
                    // Skin info not loaded yet, don't render (as uploadMatrixPalette).
                    skip_last_skin = true;
                    last_avatar = avatar;
                    last_mesh = skin->mHash;
                    last_avatar_shader = shader;
                    return Added::SKIPPED;
                }
                // plus/skinonce keeps a row without asking: one from before a
                // present within this gFrameCount would be an older frame's,
                // which sUsePaletteRow refuses but a list draw (a base only)
                // cannot tell. A list draw asks sPlacePalette itself, which
                // keeps a row of this frame and places a stale one again.
                const bool rows = mList.context().palette_rows && LLRHI::sPalettePrebuild && LLRHI::sUsePaletteRow && shader->mNativeProgram;
                const bool placed = rows &&
                                    (plus::skinonce::enabled() && mpc.mRHIRow.frame != U32_MAX
                                         ? LLRHI::sPlacePalette && LLRHI::sPlacePalette(mpc.mGLMp.data(), (U32)(mpc.mGLMp.size() / 4), mpc.mRHIRow)
                                         : LLRenderPass::rhiPaletteRow(mpc.mGLMp, mpc.mRHIRow));
                if (!placed)
                {
                    why = CUT_PALETTE;  // the joints go up as a uniform
                    return Added::OLD_PATH;
                }
                palette_base = mpc.mRHIRow.base;
                new_skin = true;
                // plus::hairsway::bindShader: the palette's sway entries (0: none),
                // set once the feature has been on (the program then has a value).
                sway = plus::hairsway::swayIndex(mpc.mMatrixPalette);
                set_sway = sway != 0 || hasValue(shader, sHairSwayUniform);
            }
        }

        LLRHI::DrawList::Draw draw;
        draw.vertex_buffer = vb->getGLBuffer();
        draw.index_buffer = vb->getGLIndices();
        draw.index_type = vb->getIndicesType();
        draw.first = params.mOffset;
        draw.count = params.mCount;
        draw.buffer_vertices = vb->getNumVerts();
        draw.buffer_indices = vb->getNumIndices();
        // Vertex data as setBuffer() lays it out for the bound program.
        LLRHI::DirectDraw::Attribute attributes[LLRHI::DirectDraw::MAX_ATTRIBUTES];
        draw.attribute_count = vb->getAttributes(shader->mAttributeMask, attributes);
        draw.first_attribute = mList.addAttributes(attributes, draw.attribute_count);
        draw.matrices = matrices(params.mModelMatrix);

        // The units as TexSetup leaves them (they stay bound for the next
        // batches, so a list draw names every unit an earlier one changed).
        for (U32 i = 0; i < texture_count; ++i)
        {
            const U32 unit = (U32)textures[i].unit;
            LLTexture* texture = textures[i].texture;
            U32 name = 0;   // unbound, as unbindFast leaves it (pushDirectBatch's 0)
            if (texture)
            {
                texture->setActive();   // bindFast's bookkeeping
                name = LLTexUnit::directName(texture);
            }
            if (mUnitNames[unit] != name || mUnitTextures[unit] != texture)
            {
                mUnitNames[unit] = name;
                mUnitTextures[unit] = texture;
                mUnitsDirty = true;
            }
            mUnits = llmax(mUnits, unit + 1);
        }
        if (mUnits)
        {
            if (mUnitsDirty)
            {
                mFirstTexture = mList.addTextures(mUnitNames, mUnits);
                mUnitsDirty = false;
            }
            draw.first_texture = mFirstTexture;
            draw.texture_count = mUnits;
        }
        if (texture_matrix)
        {
            draw.texture_matrix = mList.addFloats((const F32*)texture_matrix->mMatrix, 16);
        }
        draw.palette_base = palette_base;
        draw.oit_priority = oitPriority();

        // The uniforms the draw sees: the loop's writes so far, a minimum alpha
        // of 0 for a custom blend function, the hair sway.
        if (custom_blend)
        {
            const F32 zero = 0.f;
            track(shader, LLShaderMgr::MINIMUM_ALPHA, nullptr, &zero, 1);
        }
        if (set_sway)
        {
            const F32 value = (F32)sway;
            track(shader, 0, &sHairSwayUniform, &value, 1);
        }
        LLRHI::DrawList::State state;
        for (U32 i = 0; i < 4; ++i)
        {
            const LLRender::eBlendFactor factor = i == 0 ? blend_s : i == 1 ? blend_d : i == 2 ? alpha_s : alpha_d;
            state.blend[i] = GL_BLEND_FACTOR[llmin((U32)factor, (U32)LLRender::BF_UNDEF)];
        }
        state.cull = cull_off ? 0 : LLRHI::DrawList::State::INHERIT;
        for (Tracked& t : mTracked)
        {
            llassert(t.shader == shader);
            t.patching = !t.sent_known || memcmp(t.sent, t.want, sizeof(F32) * t.components) != 0;
            if (t.patching)
            {
                const U32 patch = mList.addPatch(t.slot, 1, t.components, 0, t.want);
                if (!state.patch_count)
                {
                    state.first_patch = patch;
                }
                ++state.patch_count;
            }
        }
        draw.state = mList.addState(state);

        if (!mList.addDraw(draw))
        {
            // Not taken (the backend was not ready after a cut): the caller
            // submits and draws it the old way. What the loop wrote stays for
            // sync() (the old path writes the same again).
            for (Tracked& t : mTracked)
            {
                t.patching = false;
            }
            why = CUT_NOT_TAKEN;
            return Added::OLD_PATH;
        }

        // In the list: the backend has the patched values and the blend.
        for (Tracked& t : mTracked)
        {
            if (t.patching)
            {
                memcpy(t.sent, t.want, sizeof(t.sent));
                t.sent_known = true;
                t.patching = false;
            }
        }
        setBlend(blend_s, blend_d, alpha_s, alpha_d);
        if (custom_blend)
        {
            // The old path sets the minimum alpha back after the draw.
            const F32 minimum = MINIMUM_ALPHA;
            track(shader, LLShaderMgr::MINIMUM_ALPHA, nullptr, &minimum, 1);
        }
        if (new_skin)
        {
            skip_last_skin = false;
            last_avatar = params.mAvatar;
            last_mesh = params.mSkinInfo->mHash;
            last_avatar_shader = shader;
            if (LLRHI::sAvatarStats)
            {
                ++LLRHI::sAvatarCounters.palette_row_uses;
            }
        }
        if (LLRHI::sAvatarStats && params.mAvatar.notNull())
        {
            LLRenderPass::rhiCountRigged(params);   // as pushDirectBatch counts every rigged batch
        }
        if (texture_matrix)
        {
            gPipeline.mTextureMatrixOps++;
        }
        return Added::LISTED;
    }
}

const char* cutName(U32 cut)
{
    return cut < CUT_COUNT ? CUT_NAMES[cut] : "?";
}

void takeStats(Stats& out)
{
    out = sStats;
    sStats = Stats();
}

// Whether renderAlphaList draws this pass (else renderAlpha as before).
static bool wanted(bool depth_only)
{
    return LLRHI::DrawList::wanted(LLRHI::DrawList::KIND_ALPHA) && LLRHI::DrawList::sDrawList &&
           !depth_only && !LLPipeline::sRenderingHUDs &&
           // the diagnostics name or time each draw (research/54)
           !LLRHI::sGPUTiming && !LLRHI::sAlphaCensus &&
           // drawGeometry's direct draws, GL drawing nothing (not the mirror mode)
           LLRHI::sGLDrawsDisabled && LLRHI::sDirect && LLRHI::sDirectDraw &&
           // pushDirectBatch's skin mark changes the stencil around a draw (G-buffer pass only)
           !plus::skinsss::detail::sMarking;
}
}

// LLRHI native (research/137 N3): LLDrawPoolAlpha::renderAlpha with its
// batches in draw lists. A copy of renderAlpha's loop (shadowed function):
// the same groups, batches, programs and calls; see the file's head.
bool LLDrawPoolAlpha::renderAlphaList(U32 mask, bool depth_only, bool rigged, EAlphaGroups groups)
{
    using namespace plus::alphalist;
    if (!wanted(depth_only))
    {
        return false;
    }
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    plus::alphapass::PassScope pass_scope(depth_only);  // as renderAlpha: the pass's CPU counters
    Recorder& rec = Recorder::get();
    rec.beginPass();

    bool initialized_lighting = false;
    bool light_enabled = true;

    const LLVOAvatar* lastAvatar = nullptr;
    U64 lastMeshId = 0;
    const LLGLSLShader* lastAvatarShader = nullptr;
    bool skipLastSkin = false;

    LLCullResult::sg_iterator begin;
    LLCullResult::sg_iterator end;

    if (rigged)
    {
        begin = gPipeline.beginRiggedAlphaGroups();
        end = gPipeline.endRiggedAlphaGroups();
    }
    else
    {
        begin = gPipeline.beginAlphaGroups();
        end = gPipeline.endAlphaGroups();
    }

    LLEnvironment& env = LLEnvironment::instance();
    F32 water_height = env.getWaterHeight();

    bool above_water = getType() == LLDrawPool::POOL_ALPHA_POST_WATER;
    if (LLPipeline::sUnderWaterRender)
    {
        above_water = !above_water;
    }

    for (LLCullResult::sg_iterator i = begin; i != end; ++i)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DRAWPOOL("renderAlpha - group");
        LLSpatialGroup* group = *i;
        llassert(group);
        llassert(group->getSpatialPartition());

        if (group->getSpatialPartition()->mRenderByGroup &&
            !group->isDead())
        {
            const bool avatar_group = !rigged && groups != GROUPS_ALL && oit_avatar_group(group);
            if (!rigged && groups == GROUPS_AVATAR && !avatar_group)
            {
                continue;
            }

            LLSpatialBridge* bridge = group->getSpatialPartition()->asBridge();
            const LLVector4a* ext = bridge ? bridge->getSpatialExtents() : group->getExtents();

            if (!LLPipeline::sRenderingHUDs) // ignore above/below water for HUD render
            {
                if (above_water)
                { // reject any spatial groups that have no part above water
                    if (ext[1].getF32ptr()[2] < water_height)
                    {
                        continue;
                    }
                }
                else
                { // reject any spatial groups that he no part below water
                    if (ext[0].getF32ptr()[2] > water_height)
                    {
                        continue;
                    }
                }
            }

            static std::vector<LLDrawInfo*> emissives;
            static std::vector<LLDrawInfo*> rigged_emissives;
            static std::vector<LLDrawInfo*> pbr_emissives;
            static std::vector<LLDrawInfo*> pbr_rigged_emissives;

            emissives.resize(0);
            rigged_emissives.resize(0);
            pbr_emissives.resize(0);
            pbr_rigged_emissives.resize(0);

            bool is_particle_or_hud_particle = group->getSpatialPartition()->mPartitionType == LLViewerRegion::PARTITION_PARTICLE
                                                      || group->getSpatialPartition()->mPartitionType == LLViewerRegion::PARTITION_HUD_PARTICLE;

            // Was: LLGLDisable cull(...) for the group. The list draws take it
            // (Draw::state), the old path's draws and the glow pass set it around
            // themselves.
            const bool disable_cull = is_particle_or_hud_particle;

            LLSpatialGroup::drawmap_elem_t& draw_info = rigged ? group->mDrawMap[LLRenderPass::PASS_ALPHA_RIGGED] : group->mDrawMap[LLRenderPass::PASS_ALPHA];

            if (LLSceneAlpha::sActive)
            {
                gSceneAlpha.noteGroup(group);   // research/205: the alpha records' census and verify (as renderAlpha)
            }

            const U32 oit_priority = mOIT && !depth_only ? oit_group_priority(group, rigged) : 0;
            mOITPriorities.clear();

            for (LLSpatialGroup::drawmap_elem_t::iterator k = draw_info.begin(); k != draw_info.end(); ++k)
            {
                LLDrawInfo& params = **k;
                if ((bool)params.mAvatar != rigged)
                {
                    continue;
                }

                if (LLPipeline::sProbeCullSerial && params.mProbeCullStamp == LLPipeline::sProbeCullSerial)
                {
                    continue;
                }

                LL_PROFILE_ZONE_NAMED_CATEGORY_DRAWPOOL("ra - push batch");

                if (mOIT && !depth_only)
                {
                    const U32 priority = oit_priority | (U32)llmin(k - draw_info.begin(), (std::ptrdiff_t)4094);
                    mOITPriorities.emplace_back(&params, priority);
                    rec.setOITPriority(priority);
                }

                LLMaterial* mat = NULL;
                LLGLTFMaterial *gltf_mat = params.mGLTFMaterial;
                bool tex_setup = false;

                if (gltf_mat && gltf_mat->mAlphaMode == LLGLTFMaterial::ALPHA_MODE_BLEND)
                {
                    // The original code: LLFetchedGLTFMaterial's bind sets uniforms and textures through GL.
                    rec.end(CUT_GLTF);
                    LLGLDisable cull(disable_cull ? GL_CULL_FACE : 0);
                    LLGLDisable cull_face(gltf_mat->mDoubleSided ? GL_CULL_FACE : 0);
                    U32 gltf_textures[GLTF_DIRECT_UNITS];
                    U32 gltf_texture_count = 0;

                    target_shader = pbr_shader;
                    if (params.mAvatar != nullptr)
                    {
                        target_shader = target_shader->mRiggedVariant;
                    }

                    // shader must be bound before LLGLTFMaterial::bind
                    if (current_shader != target_shader)
                    {
                        plus::alphapass::noteBind(*target_shader);
                        gPipeline.bindDeferredShaderFast(*target_shader);
                    }

                    // direct draws: wanted() asked for them (gltf_direct())
                    gltf_texture_count = params.mGLTFMaterial->bindDirect(params.mTexture, gltf_textures, GLTF_DIRECT_UNITS);

                    if (avatar_group)
                    {
                        const bool nodes = current_shader && LLRHI::sOITVariant && LLRHI::sOITVariant(current_shader->mProgramObject);
                        if (nodes != (groups == GROUPS_AVATAR))
                        {
                            continue;
                        }
                    }

                    if (params.mAvatar && !uploadMatrixPalette(params.mAvatar, params.mSkinInfo, lastAvatar, lastMeshId, lastAvatarShader, skipLastSkin))
                    {
                        continue;
                    }

                    if (LLSceneAlpha::sActive)
                    {
                        // research/205: the batch drawn now (as renderAlpha)
                        gSceneAlpha.noteBatch(group, params, (size_t)(k - draw_info.begin()), current_shader, LLRHI::sOITPriority);
                    }

                    gGL.blendFunc((LLRender::eBlendFactor) params.mBlendFuncSrc, (LLRender::eBlendFactor) params.mBlendFuncDst, mAlphaSFactor, mAlphaDFactor);

                    bool reset_minimum_alpha = false;
                    if (!LLPipeline::sImpostorRender &&
                        params.mBlendFuncDst != LLRender::BF_SOURCE_ALPHA &&
                        params.mBlendFuncSrc != LLRender::BF_SOURCE_ALPHA &&
                        !(depth_only && plus::dofalpha::depthPassActive()))
                    { // this draw call has a custom blend function that may require rendering of "invisible" fragments
                        current_shader->setMinimumAlpha(0.f);
                        reset_minimum_alpha = true;
                    }

                    if (!LLRenderPass::pushDirectBatch(params, false, false, gltf_textures, gltf_texture_count))
                    {
                        // refused: the textures and texture matrix through GL as bind() and TexSetup would
                        params.mGLTFMaterial->bindTextures(params.mTexture);
                        tex_setup = TexSetup(&params, false);
                        LLRenderPass::drawGeometry(params);
                    }
                    stop_glerror();

                    if (reset_minimum_alpha)
                    {
                        current_shader->setMinimumAlpha(MINIMUM_ALPHA);
                    }
                }
                else
                {
                    mat = LLPipeline::sRenderingHUDs ? nullptr : params.mMaterial;

                    if (params.mFullbright)
                    {
                        // Turn off lighting if it hasn't already been so.
                        if (light_enabled || !initialized_lighting)
                        {
                            initialized_lighting = true;
                            target_shader = fullbright_shader;

                            light_enabled = false;
                        }
                    }
                    // Turn on lighting if it isn't already.
                    else if (!light_enabled || !initialized_lighting)
                    {
                        initialized_lighting = true;
                        target_shader = simple_shader;
                        light_enabled = true;
                    }

                    if (LLPipeline::sRenderingHUDs)
                    {
                        target_shader = fullbright_shader;
                    }
                    else if (mat)
                    {
                        U32 mask = params.mShaderMask;

                        llassert(mask < LLMaterial::SHADER_COUNT);
                        target_shader = &(gDeferredMaterialProgram[mask]);
                    }
                    else if (!params.mFullbright)
                    {
                        target_shader = simple_shader;
                    }
                    else
                    {
                        target_shader = fullbright_shader;
                    }

                    if (params.mAvatar != nullptr)
                    {
                        llassert(target_shader->mRiggedVariant != nullptr);
                        target_shader = target_shader->mRiggedVariant;
                    }

                    if (current_shader != target_shader)
                    {// If we need shaders, and we're not ALREADY using the proper shader, then bind it
                    // (this way we won't rebind shaders unnecessarily).
                        rec.end(CUT_PROGRAM);
                        plus::alphapass::noteBind(*target_shader);
                        gPipeline.bindDeferredShaderFast(*target_shader);

                        if (params.mFullbright)
                        { // make sure the bind the exposure map for fullbright shaders so they can cancel out exposure
                            S32 channel = target_shader->enableTexture(LLShaderMgr::EXPOSURE_MAP);
                            if (channel > -1)
                            {
                                gGL.getTexUnit(channel)->bind(&gPipeline.mExposureMap);
                            }
                        }
                    }

                    LLVector4 spec_color(1, 1, 1, 1);
                    F32 env_intensity = 0.0f;
                    F32 brightness = 1.0f;

                    // We have a material.  Supply the appropriate data here.
                    if (mat)
                    {
                        spec_color = params.mSpecColor;
                        env_intensity = params.mEnvIntensity;
                        brightness = params.mFullbright ? 1.f : 0.f;
                    }

                    if (current_shader)
                    {
                        rec.setMaterialUniforms(current_shader, spec_color, env_intensity, brightness);
                    }

                    if (avatar_group)
                    {
                        // An attachment's batch whose program makes no nodes (no OIT
                        // variant) would land in the target before all world alpha: it
                        // goes with the world's, in its group's place.
                        const bool nodes = rec.oitNodes(current_shader);
                        if (nodes != (groups == GROUPS_AVATAR))
                        {
                            continue;
                        }
                    }

                    const LLRender::eBlendFactor blend_s = (LLRender::eBlendFactor)params.mBlendFuncSrc;
                    const LLRender::eBlendFactor blend_d = (LLRender::eBlendFactor)params.mBlendFuncDst;
                    const bool custom_blend = !LLPipeline::sImpostorRender &&
                        params.mBlendFuncDst != LLRender::BF_SOURCE_ALPHA &&
                        params.mBlendFuncSrc != LLRender::BF_SOURCE_ALPHA &&
                        !(depth_only && plus::dofalpha::depthPassActive());

                    // The original's culling around the batch: the group's, and a
                    // double-sided GLTF material's (one that is not blended comes here).
                    const bool cull_off = disable_cull || (gltf_mat && gltf_mat->mDoubleSided);

                    UnitTexture textures[MAX_UNIT_TEXTURES];
                    const LLMatrix4* texture_matrix = nullptr;
                    const U32 texture_count = collect_textures(params, mat != nullptr, current_shader, simple_shader, textures, texture_matrix);

                    Cut why = CUT_END;
                    const Added added = rec.addBatch(params, textures, texture_count, texture_matrix, cull_off,
                                                     blend_s, blend_d, mAlphaSFactor, mAlphaDFactor, custom_blend,
                                                     lastAvatar, lastMeshId, lastAvatarShader, skipLastSkin, why);
                    if (added == Added::SKIPPED)
                    {
                        continue;
                    }
                    if (LLSceneAlpha::sActive)
                    {
                        // research/205: the batch drawn now (as renderAlpha; the priority the list's draw takes)
                        gSceneAlpha.noteBatch(group, params, (size_t)(k - draw_info.begin()), current_shader, rec.oitPriority());
                    }
                    if (added == Added::LISTED)
                    {
                        ++sStats.listed;
                    }
                    else
                    {
                        // The original code.
                        rec.end(why);
                        ++sStats.old_path;
                        LLGLDisable cull(cull_off ? GL_CULL_FACE : 0);

                        if (params.mAvatar && !uploadMatrixPalette(params.mAvatar, params.mSkinInfo, lastAvatar, lastMeshId, lastAvatarShader, skipLastSkin))
                        {
                            continue;
                        }

                        tex_setup = TexSetup(&params, (mat != nullptr));

                        gGL.blendFunc(blend_s, blend_d, mAlphaSFactor, mAlphaDFactor);

                        if (custom_blend)
                        { // this draw call has a custom blend function that may require rendering of "invisible" fragments
                            current_shader->setMinimumAlpha(0.f);
                        }

                        LLRenderPass::drawGeometry(params);
                        stop_glerror();

                        if (custom_blend)
                        {
                            current_shader->setMinimumAlpha(MINIMUM_ALPHA);
                        }
                    }
                }

                // If this alpha mesh has glow, then draw it a second time to add the destination-alpha (=glow).  Interleaving these state-changing calls is expensive, but glow must be drawn Z-sorted with alpha.
                // As renderAlpha: not a batch no face of which glows (plus/alphapass).
                if (getType() != LLDrawPool::POOL_ALPHA_PRE_WATER &&
                    plus::alphapass::glowPass(group, params, mOIT, depth_only))
                {
                    if (params.mAvatar != nullptr)
                    {
                        if (params.mGLTFMaterial.isNull())
                        {
                            rigged_emissives.push_back(&params);
                        }
                        else
                        {
                            pbr_rigged_emissives.push_back(&params);
                        }
                    }
                    else
                    {
                        if (params.mGLTFMaterial.isNull())
                        {
                            emissives.push_back(&params);
                        }
                        else
                        {
                            pbr_emissives.push_back(&params);
                        }
                    }
                }

                if (tex_setup)
                {
                    gGL.getTexUnit(0)->activate();
                    gGL.matrixMode(LLRender::MM_TEXTURE);
                    gGL.loadIdentity();
                    gGL.matrixMode(LLRender::MM_MODELVIEW);
                }
            }

            // render emissive faces into alpha channel for bloom effects
            if (!depth_only)
            {
                const bool no_glow = emissives.empty() && pbr_emissives.empty() && rigged_emissives.empty() && pbr_rigged_emissives.empty();
                plus::alphapass::GlowScope glow_scope(!no_glow);    // as renderAlpha
                if (no_glow)
                {
                    // Nothing to glow: the original sets the glow blend and its own
                    // back with nothing drawn between. gGL's light state only: no
                    // hook, the list stays open.
                    gPipeline.enableLightsDynamic();
                    rec.setBlend(mColorSFactor, mColorDFactor, mAlphaSFactor, mAlphaDFactor);
                }
                else
                {
                    // The original code, with the group's culling.
                    rec.end(CUT_GLOW);
                    LLGLDisable cull(disable_cull ? GL_CULL_FACE : 0);

                    gPipeline.enableLightsDynamic();

                    // install glow-accumulating blend mode
                    // don't touch color, add to alpha (glow)
                    gGL.blendFunc(LLRender::BF_ZERO, LLRender::BF_ONE, LLRender::BF_ONE, LLRender::BF_ONE);

                    bool rebind = false;
                    LLGLSLShader* lastShader = current_shader;
                    if (!emissives.empty())
                    {
                        light_enabled = true;
                        renderEmissives(emissives);
                        rebind = true;
                    }

                    if (!pbr_emissives.empty())
                    {
                        light_enabled = true;
                        renderPbrEmissives(pbr_emissives);
                        rebind = true;
                    }

                    if (!rigged_emissives.empty())
                    {
                        light_enabled = true;
                        renderRiggedEmissives(rigged_emissives);
                        rebind = true;
                    }

                    if (!pbr_rigged_emissives.empty())
                    {
                        light_enabled = true;
                        renderRiggedPbrEmissives(pbr_rigged_emissives);
                        rebind = true;
                    }

                    // restore our alpha blend mode
                    gGL.blendFunc(mColorSFactor, mColorDFactor, mAlphaSFactor, mAlphaDFactor);

                    if (lastShader && rebind)
                    {
                        lastShader->bind();
                    }
                }
            }
        }
    }

    rec.end(CUT_END);

    gGL.setSceneBlendType(LLRender::BT_ALPHA);

    LLVertexBuffer::unbind();

    if (!light_enabled)
    {
        gPipeline.enableLightsDynamic();
    }
    return true;
}

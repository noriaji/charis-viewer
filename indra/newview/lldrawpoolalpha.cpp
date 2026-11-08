/**
 * @file lldrawpoolalpha.cpp
 * @brief LLDrawPoolAlpha class implementation
 *
 * $LicenseInfo:firstyear=2002&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "lldrawpoolalpha.h"

#include "llglheaders.h"
#include "llviewercontrol.h"
#include "llcriticaldamp.h"
#include "llfasttimer.h"
#include "llrender.h"

#include "llcubemap.h"
#include "llsky.h"
#include "lldrawable.h"
#include "llface.h"
#include "llviewercamera.h"
#include "llviewertexturelist.h"    // For debugging
#include "llviewerobjectlist.h" // For debugging
#include "llviewerwindow.h"
#include "pipeline.h"
#include "llgraywire.h" // <plus> research/243 </plus>
#include "llviewershadermgr.h"
#include "llviewerregion.h"
#include "lldrawpoolwater.h"
#include "llspatialpartition.h"
#include "llglcommonfunc.h"
#include "llvoavatar.h"
#include "gltfscenemanager.h"
#include "llfetchedgltfmaterial.h"
#include "llrhi.h"
#include "llviewerjointattachment.h"
#include "llvopartgroup.h"

#include "llenvironment.h"
// <plus>
#include "plus/atmosphere/atmosphere.h"
#include "plus/volumetric/volumetric.h"
#include "plus/dofalpha/dofalpha.h"
#include "plus/alphapass/alphapass.h"
#include "llscenealpha.h" // research/205: census and verify of the alpha records
#include "llscenerecords.h" // research/205 3a-3: the alpha records drawn by the scene
// </plus>

#include <algorithm>
#include <iomanip>
#include <map>
#include <sstream>
#include <unordered_map>

bool LLDrawPoolAlpha::sShowDebugAlpha = false;

#define current_shader (LLGLSLShader::sCurBoundShaderPtr)

LLVector4 LLDrawPoolAlpha::sWaterPlane;

// minimum alpha before discarding a fragment
static const F32 MINIMUM_ALPHA = 0.004f; // ~ 1/255

// minimum alpha before discarding a fragment when rendering impostors
static const F32 MINIMUM_IMPOSTOR_ALPHA = 0.1f;

LLDrawPoolAlpha::LLDrawPoolAlpha(U32 type) :
        LLRenderPass(type), target_shader(NULL),
        mColorSFactor(LLRender::BF_UNDEF), mColorDFactor(LLRender::BF_UNDEF),
        mAlphaSFactor(LLRender::BF_UNDEF), mAlphaDFactor(LLRender::BF_UNDEF)
{

}

LLDrawPoolAlpha::~LLDrawPoolAlpha()
{
}


void LLDrawPoolAlpha::prerender()
{
    mShaderLevel = LLViewerShaderMgr::instance()->getShaderLevel(LLViewerShaderMgr::SHADER_OBJECT);
}

S32 LLDrawPoolAlpha::getNumPostDeferredPasses()
{
    return 1;
}

// set some common parameters on the given shader to prepare for alpha rendering
static void prepare_alpha_shader(LLGLSLShader* shader, bool deferredEnvironment, F32 water_sign)
{
    static LLCachedControl<F32> displayGamma(gSavedSettings, "RenderDeferredDisplayGamma");
    F32 gamma = displayGamma;

    static LLStaticHashedString waterSign("waterSign");

    // Does this deferred shader need environment uniforms set such as sun_dir, etc. ?
    // NOTE: We don't actually need a gbuffer since we are doing forward rendering (for transparency) post deferred rendering
    // TODO: bindDeferredShader() probably should have the updating of the environment uniforms factored out into updateShaderEnvironmentUniforms()
    // i.e. shaders\class1\deferred\alphaF.glsl
    if (deferredEnvironment)
    {
        shader->mCanBindFast = false;
    }

    shader->bind();
    shader->uniform1f(LLShaderMgr::DISPLAY_GAMMA, (gamma > 0.1f) ? 1.0f / gamma : (1.0f / 2.2f));

    if (LLPipeline::sRenderingHUDs)
    { // for HUD attachments, only the pre-water pass is executed and we never want to clip anything
        LLVector4 near_clip(0, 0, -1, 0);
        shader->uniform1f(waterSign, 1.f);
        shader->uniform4fv(LLShaderMgr::WATER_WATERPLANE, 1, near_clip.mV);
    }
    else
    {
        shader->uniform1f(waterSign, water_sign);
        shader->uniform4fv(LLShaderMgr::WATER_WATERPLANE, 1, LLDrawPoolAlpha::sWaterPlane.mV);
    }

    if (LLPipeline::sImpostorRender)
    {
        shader->setMinimumAlpha(MINIMUM_IMPOSTOR_ALPHA);
    }
    else
    {
        shader->setMinimumAlpha(MINIMUM_ALPHA);
    }

    //also prepare rigged variant
    if (shader->mRiggedVariant && shader->mRiggedVariant != shader)
    {
        prepare_alpha_shader(shader->mRiggedVariant, deferredEnvironment, water_sign);
    }
}

extern bool gCubeSnapshot;

// --- Alpha pass diagnostics (research/54) -----------------------------------
// GPU timing by kind of draw (LLRHI::sGPUTiming and sGPUTimingAlpha) and the
// alpha census (LLRHI::sAlphaCensus). Nothing here runs while both are off.
namespace
{
    enum AlphaKind : U8
    {
        KIND_ALPHA,         // blended, not rigged, none of the kinds below
        KIND_RIGGED,
        KIND_PARTICLES,     // LLVOPartGroup
        KIND_GRASS,
        KIND_GLTF,          // blended GLTF material, not rigged
        KIND_GLOW,          // each group's emissive pass (into the alpha channel)
        KIND_GLTF_SCENE,    // LL::GLTFSceneManager before the rigged pass
        KIND_DOF,           // the depth pass for depth of field
        KIND_COUNT
    };
    const char* const KIND_NAMES[KIND_COUNT] = { "alpha", "rigged", "particles", "grass", "gltf", "glow", "gltf scene", "dof depth" };
    // GPU timing segments (gpuMark keeps names by pointer): the post-water pool, the pre-water pool.
    const char* const KIND_MARKS[2][KIND_COUNT] =
    {
        { "alpha", "alpha rigged", "alpha particles", "alpha grass", "alpha gltf", "alpha glow", "alpha gltf scene", "alpha dof depth" },
        { "prewater alpha", "prewater rigged", "prewater particles", "prewater grass", "prewater gltf", "prewater glow",
          "prewater gltf scene", "prewater dof depth" },
    };

    bool sPrewater = false;     // the pool drawing is POOL_ALPHA_PRE_WATER (renderPostDeferred)

    // The draws after this go to the kind's GPU timing segment (no timestamp while it is open).
    void gpu_mark_kind(AlphaKind kind)
    {
        if (LLRHI::sGPUTiming && LLRHI::sGPUTimingAlpha && !LLPipeline::sRenderingHUDs)
        {
            LLRHI::gpuMark(KIND_MARKS[sPrewater ? 1 : 0][kind]);
        }
    }

    AlphaKind draw_kind(LLSpatialGroup* group, const LLDrawInfo& params, bool depth_only)
    {
        if (depth_only)
        {
            return KIND_DOF;
        }
        if (params.mAvatar.notNull())
        {
            return KIND_RIGGED;
        }
        const U32 partition = group->getSpatialPartition()->mPartitionType;
        if (partition == LLViewerRegion::PARTITION_PARTICLE || partition == LLViewerRegion::PARTITION_HUD_PARTICLE)
        {
            return KIND_PARTICLES;
        }
        if (partition == LLViewerRegion::PARTITION_GRASS)
        {
            return KIND_GRASS;
        }
        if (params.mGLTFMaterial.notNull() && params.mGLTFMaterial->mAlphaMode == LLGLTFMaterial::ALPHA_MODE_BLEND)
        {
            return KIND_GLTF;
        }
        return KIND_ALPHA;
    }

    // LLRHI_ALPHA_CENSUS=1: the main view's alpha draws, grouped by kind,
    // program, texture and object. Each draw is named to the backend
    // (LLRHI::sCensusTag), which counts its fragment shader invocations and
    // primitives; every 10 s the 20 groups with the most fragments per frame
    // are logged ("AlphaCensus"), with the alpha fragments per screen pixel.
    // Without the counts (no pipeline statistics) the projected bounding box
    // area stands in for the fragments.
    class AlphaCensus
    {
    public:
        // renderPostDeferred: counts this pool's draws if it draws the main view.
        void beginPool();
        void endPool()
        {
            mActive = false;
            clear();
        }
        // renderAlpha, per group: which faces (objects) the group's batches draw.
        void setGroup(LLSpatialGroup* group, const LLVector4a* agent_extents);
        // Before a draw: names it to the backend (call clear() after it).
        void tag(AlphaKind kind, const LLDrawInfo* draw);
        void tagScene();
        void clear() { LLRHI::sCensusTag = 0; }

    private:
        struct Key
        {
            U8 kind;
            const LLGLSLShader* shader;
            LLUUID texture;
            LLUUID object;
            bool operator<(const Key& other) const
            {
                if (kind != other.kind) return kind < other.kind;
                if (shader != other.shader) return shader < other.shader;
                if (texture != other.texture) return texture < other.texture;
                return object < other.object;
            }
        };
        struct Entry
        {
            U8 kind = KIND_ALPHA;
            std::string shader;
            std::string texture;    // UUID, + how many more a batch binds
            std::string object;     // what drew it, where
            bool animated = false;  // texture animation (a texture matrix)
            bool blend = false;     // not the usual alpha blend
            U64 draws = 0;          // CPU side, since the report
            U64 triangles = 0;
            U64 faces = 0;
            F64 box_pixels = 0.0;   // projected bounding box area, summed per draw
            U64 queries = 0;        // GPU side
            U64 primitives = 0;
            U64 clipped = 0;
            U64 fragments = 0;
        };
        struct FaceRef
        {
            LLDrawable* drawable;
            S32 face;
            U32 count;
        };

        U32 entryFor(const Key& key, AlphaKind kind, const LLDrawInfo* draw, const FaceRef* face);
        F64 boxPixels(const LLVector4a& lo, const LLVector4a& hi) const;
        void report();
        void reset();

        bool mActive = false;
        U32 mFrame = U32_MAX;
        U32 mFrames = 0;            // frames counted since the report (CPU side)
        U32 mWidth = 0;
        U32 mHeight = 0;
        F32 mViewProjection[16] = {};
        U8 mGeneration = 1;         // tags of earlier reports' frames still in flight are ignored
        LLFrameTimer mTimer;
        std::map<Key, U32> mIndex;
        std::vector<Entry> mEntries;
        const LLVector4a* mGroupExtents = nullptr;
        std::unordered_map<const LLDrawInfo*, FaceRef> mFaces;
        static constexpr U32 MAX_ENTRIES = 1u << 20;
        static constexpr F32 REPORT_SECONDS = 10.f;
    };
    AlphaCensus sCensus;

    void AlphaCensus::reset()
    {
        mIndex.clear();
        mEntries.clear();
        mFrames = 0;
        mGeneration = (U8)(mGeneration == 255 ? 1 : mGeneration + 1);
        mTimer.reset();
    }

    void AlphaCensus::beginPool()
    {
        mActive = false;
        clear();
        if (!LLRHI::sAlphaCensus)
        {
            if (!mEntries.empty() || mFrames)
            {
                reset();    // switched off: start over when it is switched on again
            }
            return;
        }
        if (LLPipeline::sRenderingHUDs || LLPipeline::sImpostorRender || LLPipeline::sReflectionRender ||
            LLPipeline::sShadowRender || gCubeSnapshot || !gPipeline.mRT)
        {
            return;     // the main view only
        }
        const U32 frame = LLFrameTimer::getFrameCount();
        if (frame != mFrame)
        {
            if (mFrame == U32_MAX)
            {
                mTimer.reset();
            }
            else if (mTimer.getElapsedTimeF32() >= REPORT_SECONDS)
            {
                report();
                reset();
            }
            mFrame = frame;
            ++mFrames;
        }
        mWidth = gPipeline.mRT->screen.getWidth();
        mHeight = gPipeline.mRT->screen.getHeight();
        // Agent space to clip space (column-major), for the bounding box estimate.
        for (U32 c = 0; c < 4; ++c)
        {
            for (U32 r = 0; r < 4; ++r)
            {
                F32 sum = 0.f;
                for (U32 k = 0; k < 4; ++k)
                {
                    sum += gGLProjection[k * 4 + r] * gGLModelView[c * 4 + k];
                }
                mViewProjection[c * 4 + r] = sum;
            }
        }
        mActive = true;
    }

    void AlphaCensus::setGroup(LLSpatialGroup* group, const LLVector4a* agent_extents)
    {
        mGroupExtents = agent_extents;
        mFaces.clear();
        if (!mActive)
        {
            return;
        }
        for (LLSpatialGroup::element_iter i = group->getDataBegin(); i != group->getDataEnd(); ++i)
        {
            LLDrawable* drawable = (LLDrawable*)(*i)->getDrawable();
            if (!drawable || drawable->isDead())
            {
                continue;
            }
            for (S32 f = 0; f < drawable->getNumFaces(); ++f)
            {
                LLFace* face = drawable->getFace(f);
                if (face && face->mDrawInfo)
                {
                    ++mFaces.try_emplace(face->mDrawInfo, FaceRef{ drawable, f, 0 }).first->second.count;
                }
            }
        }
    }

    // The screen pixels the bounding rectangle of the box's projection covers;
    // the whole screen when the box reaches behind the camera.
    F64 AlphaCensus::boxPixels(const LLVector4a& lo, const LLVector4a& hi) const
    {
        const F64 screen = (F64)mWidth * mHeight;
        F32 x0 = 1.f, y0 = 1.f, x1 = -1.f, y1 = -1.f;
        for (U32 i = 0; i < 8; ++i)
        {
            const F32 p[4] = { (i & 1 ? hi : lo).getF32ptr()[0], (i & 2 ? hi : lo).getF32ptr()[1], (i & 4 ? hi : lo).getF32ptr()[2], 1.f };
            F32 clip[4] = {};
            for (U32 r = 0; r < 4; ++r)
            {
                for (U32 c = 0; c < 4; ++c)
                {
                    clip[r] += mViewProjection[c * 4 + r] * p[c];
                }
            }
            if (clip[3] <= 0.01f)
            {
                return screen;
            }
            x0 = llmin(x0, clip[0] / clip[3]);
            x1 = llmax(x1, clip[0] / clip[3]);
            y0 = llmin(y0, clip[1] / clip[3]);
            y1 = llmax(y1, clip[1] / clip[3]);
        }
        x0 = llclamp(x0, -1.f, 1.f);
        x1 = llclamp(x1, -1.f, 1.f);
        y0 = llclamp(y0, -1.f, 1.f);
        y1 = llclamp(y1, -1.f, 1.f);
        if (x1 <= x0 || y1 <= y0)
        {
            return 0.0;
        }
        return (F64)(x1 - x0) * 0.5 * mWidth * (F64)(y1 - y0) * 0.5 * mHeight;
    }

    U32 AlphaCensus::entryFor(const Key& key, AlphaKind kind, const LLDrawInfo* draw, const FaceRef* face)
    {
        auto found = mIndex.find(key);
        if (found != mIndex.end())
        {
            return found->second;
        }
        if (mEntries.size() >= MAX_ENTRIES)
        {
            return U32_MAX;
        }
        Entry entry;
        entry.kind = kind;
        entry.shader = key.shader ? key.shader->mName : std::string(kind == KIND_GLTF_SCENE ? "(GLTF scene programs)" : "?");
        if (draw)
        {
            entry.texture = key.texture.notNull() ? key.texture.asString() : std::string("none");
            if (draw->mTextureList.size() > 1)
            {
                entry.texture += llformat(" (+%d batched)", (S32)draw->mTextureList.size() - 1);
            }
            entry.animated = draw->mTextureMatrix != nullptr;
            entry.blend = draw->mBlendFuncSrc != LLRender::BF_SOURCE_ALPHA || draw->mBlendFuncDst != LLRender::BF_ONE_MINUS_SOURCE_ALPHA;
        }
        std::ostringstream out;
        LLViewerObject* vobj = face ? face->drawable->getVObj().get() : nullptr;
        if (vobj && vobj->getPCode() == LLViewerObject::LL_VO_PART_GROUP)
        {
            out << "particles of " << key.object;
            if (LLViewerObject* source = gObjectList.findObject(key.object))
            {
                const LLVector3 pos = source->getPositionRegion();
                out << " at " << llformat("%.1f,%.1f,%.1f", pos.mV[0], pos.mV[1], pos.mV[2]);
            }
        }
        else if (vobj)
        {
            const LLVector3 pos = vobj->getPositionRegion();
            const LLVector3 scale = vobj->getScale();
            out << "object " << vobj->getID() << " (local " << vobj->getLocalID() << ") at "
                << llformat("%.1f,%.1f,%.1f", pos.mV[0], pos.mV[1], pos.mV[2]) << " scale "
                << llformat("%.2fx%.2fx%.2f", scale.mV[0], scale.mV[1], scale.mV[2]);
            LLViewerObject* root = vobj->getRootEdit();
            if (root && root->isAttachment())
            {
                const std::string& item = root->getAttachmentItemName();
                if (!item.empty())
                {
                    out << " \"" << item << "\"";
                }
            }
        }
        else if (kind == KIND_GLTF_SCENE)
        {
            out << "GLTF scene";
        }
        else if (mGroupExtents)
        {
            LLVector4a center;
            center.setAdd(mGroupExtents[0], mGroupExtents[1]);
            center.mul(0.5f);
            out << "group at agent " << llformat("%.1f,%.1f,%.1f", center.getF32ptr()[0], center.getF32ptr()[1], center.getF32ptr()[2]);
        }
        if (draw && draw->mAvatar.notNull())
        {
            out << " worn by " << draw->mAvatar->getID(); // <plus> log privacy: the id, not the name </plus>
        }
        else if (vobj && vobj->getAvatar())
        {
            out << " worn by " << vobj->getAvatar()->getID(); // <plus> log privacy: the id, not the name </plus>
        }
        entry.object = out.str();
        const U32 index = (U32)mEntries.size();
        mEntries.push_back(std::move(entry));
        mIndex.emplace(key, index);
        return index;
    }

    void AlphaCensus::tag(AlphaKind kind, const LLDrawInfo* draw)
    {
        if (!mActive || !draw)
        {
            return;
        }
        auto found = mFaces.find(draw);
        const FaceRef* face = found != mFaces.end() ? &found->second : nullptr;

        Key key;
        key.kind = (U8)kind;
        key.shader = LLGLSLShader::sCurBoundShaderPtr;
        if (draw->mTexture.notNull())
        {
            key.texture = draw->mTexture->getID();
        }
        else if (draw->mGLTFMaterial.notNull() && draw->mGLTFMaterial->mBaseColorTexture.notNull())
        {
            key.texture = draw->mGLTFMaterial->mBaseColorTexture->getID();
        }
        if (face)
        {
            LLViewerObject* vobj = face->drawable->getVObj().get();
            if (vobj && vobj->getPCode() == LLViewerObject::LL_VO_PART_GROUP)
            {
                key.object = ((LLVOPartGroup*)vobj)->getPartSource(face->face);
            }
            else if (vobj)
            {
                key.object = vobj->getID();
            }
        }
        const U32 index = entryFor(key, kind, draw, face);
        if (index == U32_MAX)
        {
            return;
        }
        Entry& entry = mEntries[index];
        ++entry.draws;
        entry.triangles += draw->mCount / 3;
        entry.faces += face ? face->count : 0;
        if (draw->mHasExtents && draw->mModelMatrix && draw->mAvatar.isNull())
        {
            // Bounds in region space (LLDrawInfo::mExtents).
            LLVector4a origin, lo, hi;
            origin.load3(draw->mModelMatrix->mMatrix[3]);
            lo.setAdd(draw->mExtents[0], origin);
            hi.setAdd(draw->mExtents[1], origin);
            entry.box_pixels += boxPixels(lo, hi);
        }
        else if (mGroupExtents)
        {
            entry.box_pixels += boxPixels(mGroupExtents[0], mGroupExtents[1]);
        }
        LLRHI::sCensusTag = ((U32)mGeneration << 24) | (index + 1);
    }

    void AlphaCensus::tagScene()
    {
        if (!mActive)
        {
            return;
        }
        Key key = { (U8)KIND_GLTF_SCENE, nullptr, LLUUID::null, LLUUID::null };
        mGroupExtents = nullptr;
        const U32 index = entryFor(key, KIND_GLTF_SCENE, nullptr, nullptr);
        if (index != U32_MAX)
        {
            ++mEntries[index].draws;
            LLRHI::sCensusTag = ((U32)mGeneration << 24) | (index + 1);
        }
    }

    void AlphaCensus::report()
    {
        std::vector<LLRHI::CensusCount> counts;
        U32 gpu_frames = 0;
        U32 dropped = 0;
        const bool gpu = LLRHI::sCensusTake && LLRHI::sCensusTake(counts, gpu_frames, dropped) && gpu_frames;
        for (const LLRHI::CensusCount& count : counts)
        {
            const U32 index = (count.tag & 0xffffffu) - 1;
            if ((count.tag >> 24) != mGeneration || index >= mEntries.size())
            {
                continue;   // drawn before the last report
            }
            Entry& entry = mEntries[index];
            entry.queries += count.queries;
            entry.primitives += count.primitives;
            entry.clipped += count.clipped;
            entry.fragments += count.fragments;
        }
        if (!mFrames || mEntries.empty())
        {
            return;
        }
        const F64 pixels = llmax((F64)mWidth * mHeight, 1.0);
        const F64 cpu_frames = mFrames;
        const F64 frames = gpu ? (F64)gpu_frames : cpu_frames;
        // Per kind, and in all (per frame).
        F64 kind_amount[KIND_COUNT] = {};
        F64 kind_draws[KIND_COUNT] = {};
        F64 total = 0.0, total_box = 0.0, draws = 0.0, queried = 0.0;
        std::vector<U32> order(mEntries.size());
        for (U32 i = 0; i < mEntries.size(); ++i)
        {
            const Entry& entry = mEntries[i];
            order[i] = i;
            const F64 amount = gpu ? entry.fragments / frames : entry.box_pixels / cpu_frames;
            kind_amount[entry.kind] += amount;
            kind_draws[entry.kind] += entry.draws / cpu_frames;
            total += amount;
            total_box += entry.box_pixels / cpu_frames;
            draws += entry.draws / cpu_frames;
            queried += entry.queries / frames;
        }
        auto amount_of = [&](const Entry& entry) { return gpu ? entry.fragments / frames : entry.box_pixels / cpu_frames; };
        std::sort(order.begin(), order.end(), [&](U32 a, U32 b) { return amount_of(mEntries[a]) > amount_of(mEntries[b]); });

        std::ostringstream head;
        head << std::fixed << std::setprecision(2);
        head << "Alpha census, " << mFrames << " frames, " << mWidth << "x" << mHeight << ": "
             << (gpu ? "fragment shader invocations" : "projected bounding boxes (no fragment counts)") << " per frame "
             << total / 1e6 << "M = " << total / pixels << " per screen pixel (";
        for (U32 k = 0; k < KIND_COUNT; ++k)
        {
            head << (k ? ", " : "") << KIND_NAMES[k] << ' ' << kind_amount[k] / pixels;
        }
        head << "); draws per frame " << draws << " (";
        for (U32 k = 0; k < KIND_COUNT; ++k)
        {
            head << (k ? ", " : "") << KIND_NAMES[k] << ' ' << kind_draws[k];
        }
        head << ")";
        if (gpu)
        {
            head << ", counted on the GPU " << queried << " (" << gpu_frames << " frames, " << dropped << " not counted)"
                 << "; bounding boxes " << total_box / pixels << " per screen pixel";
        }
        LL_INFOS("AlphaCensus") << head.str() << LL_ENDL;

        const U32 top = llmin((U32)order.size(), 20u);
        for (U32 rank = 0; rank < top; ++rank)
        {
            const Entry& entry = mEntries[order[rank]];
            const F64 amount = amount_of(entry);
            std::ostringstream line;
            line << std::fixed << std::setprecision(2);
            line << "#" << rank + 1 << ' ' << KIND_NAMES[entry.kind] << ": " << amount / 1e6 << "M px/frame ("
                 << (total > 0.0 ? 100.0 * amount / total : 0.0) << "%, " << amount / pixels << " of the screen)"
                 << ", draws " << entry.draws / cpu_frames << ", triangles " << entry.triangles / cpu_frames;
            if (gpu)
            {
                line << " (GPU: " << entry.primitives / frames << " in, " << entry.clipped / frames << " past clipping)";
            }
            line << ", bounding box " << entry.box_pixels / cpu_frames / 1e6 << "M px";
            if (entry.faces)
            {
                line << ", faces " << entry.faces / cpu_frames;
            }
            line << "; texture " << entry.texture << (entry.animated ? " animated" : "") << (entry.blend ? ", custom blend" : "")
                 << "; program \"" << entry.shader << "\"; " << entry.object;
            LL_INFOS("AlphaCensus") << line.str() << LL_ENDL;
        }
    }
}

// LLRHI (research/41): the OIT priority of an alpha group's batches (the
// batch's place in the group goes in bits 0-11; the higher on top). Inside a
// surface (nodes within the backend's depth tolerance of each other) nodes
// are blended by it instead of by depth, so it follows the order in which
// upstream's alpha pass blends the same draws, and is frame-stable where
// that order is:
// - rigged (bit 31 clear): upstream draws all rigged alpha first, groups in
//   descending mRenderOrder (LLSpatialGroup::CompareRenderOrder), which
//   LLVOAvatar::idleUpdateMisc numbers by attachment point id and, within a
//   point, attach order (LLViewerJointAttachment::mAttachedObjects): the
//   lower point id and, within a point, the earlier attached are drawn last,
//   on top. Rigged meshes that are not attachments (animesh) by local id.
//   Upstream orders different avatars by pointer: not here (no rule).
// - not rigged (bit 31 set: drawn after every rigged group upstream): groups
//   far to near by their distance (CompareDepthGreater on mDepth), so the
//   nearer group on top; to 1/2048 of the distance.
// Within a group the batches come in the group's draw order (rigged: the
// faces' own order, else far to near faces), the later on top, and the glow
// pass after all of the group's batches (setOITPriority).
U32 oit_group_priority(LLSpatialGroup* group, bool rigged) // LLRHI native (research/137 N3): plus/alphalist uses it too
{
    if (!rigged)
    {
        // Positive floats compare as their bits: the nearer, the higher.
        U32 bits;
        const F32 depth = llmax(group->mDepth, 0.f);
        memcpy(&bits, &depth, 4);
        return 0x80000000u | ((0x7ffffu - (bits >> 12)) << 12);
    }
    LLSpatialBridge* bridge = group->getSpatialPartition()->asBridge();
    LLDrawable* drawable = bridge ? bridge->mDrawable : nullptr;
    LLViewerObject* vobj = drawable ? drawable->getVObj().get() : nullptr;
    LLViewerObject* root = vobj ? vobj->getRootEdit() : nullptr;
    LLVOAvatar* avatar = root && root->isAttachment() ? root->getAvatar() : nullptr;
    if (avatar)
    {
        for (const auto& [point, attachment] : avatar->mAttachmentPoints)
        {
            const LLViewerJointAttachment::attachedobjs_vec_t& objects = attachment->mAttachedObjects;
            for (size_t i = 0; i < objects.size(); ++i)
            {
                if (objects[i].get() == root)
                {
                    return ((255u - (U32)llclamp(point, 0, 255)) << 23) | ((2047u - (U32)llmin(i, (size_t)2047)) << 12);
                }
            }
        }
    }
    return root ? (root->getLocalID() & 0x7ffffu) << 12 : 0u;
}

// LLRHI (research/41): a non-rigged alpha group of an avatar: an attachment
// (its linkset's spatial bridge). The hybrid OIT session draws these with the
// rigged alpha, before the rest of the alpha.
bool oit_avatar_group(LLSpatialGroup* group) // LLRHI native (research/137 N3): plus/alphalist uses it too
{
    LLSpatialBridge* bridge = group->getSpatialPartition()->asBridge();
    LLDrawable* drawable = bridge ? bridge->mDrawable : nullptr;
    LLViewerObject* vobj = drawable ? drawable->getVObj().get() : nullptr;
    LLViewerObject* root = vobj ? vobj->getRootEdit() : nullptr;
    return root && root->isAttachment();
}

void LLDrawPoolAlpha::setOITPriority(const LLDrawInfo* draw)
{
    if (!mOIT)
    {
        return;
    }
    for (const auto& entry : mOITPriorities)
    {
        if (entry.first == draw)
        {
            // Upstream's glow pass comes after all of the group's batches.
            LLRHI::sOITPriority = entry.second | 0xfffu;
            return;
        }
    }
}

void LLDrawPoolAlpha::renderPostDeferred(S32 pass)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;

    if (LLPipeline::isWaterClip() && getType() == LLDrawPool::POOL_ALPHA_PRE_WATER)
    { // don't render alpha objects on the other side of the water plane if water is opaque
        return;
    }

    F32 water_sign = 1.f;

    if (getType() == LLDrawPool::POOL_ALPHA_PRE_WATER)
    {
        water_sign = -1.f;
    }

    // <plus> volumetric light (plus/volumetric): by default (PlusVolumetricAlpha 3) its
    // shafts go into the scene here, before the main view's above-water alpha, as the haze
    // (doAtmospherics) does; the alpha then blends over them. Added after the
    // alpha, a ray through a leaf or a hair, which write no depth, ran on to
    // the sky and its air's light covered them (alpha-sky).
    const bool main_view = getType() == LLDrawPool::POOL_ALPHA_POST_WATER && !LLPipeline::sRenderingHUDs && !LLPipeline::sImpostorRender &&
                           !LLPipeline::sUnderWaterRender && !LLPipeline::sReflectionRender && !LLPipeline::sShadowRender && !gCubeSnapshot;
    if (main_view)
    {
        plus::atmosphere::renderFogBeforeAlpha();
        plus::volumetric::renderBeforeAlpha();
    }
    // </plus>

    // Diagnostics (research/54): GPU timing by kind of draw, the alpha census.
    sPrewater = getType() == LLDrawPool::POOL_ALPHA_PRE_WATER;
    sCensus.beginPool();

    if (LLPipeline::sUnderWaterRender)
    {
        water_sign *= -1.f;
    }

    // prepare shaders
    llassert(LLPipeline::sRenderDeferred);

    emissive_shader = &gDeferredEmissiveProgram;
    prepare_alpha_shader(emissive_shader, false, water_sign);

    pbr_emissive_shader = &gPBRGlowProgram;
    prepare_alpha_shader(pbr_emissive_shader, false, water_sign);


    fullbright_shader   =
        (LLPipeline::sImpostorRender) ? &gDeferredFullbrightAlphaMaskProgram :
        (LLPipeline::sRenderingHUDs) ? &gHUDFullbrightAlphaMaskAlphaProgram :
        &gDeferredFullbrightAlphaMaskAlphaProgram;
    prepare_alpha_shader(fullbright_shader, true, water_sign);

    simple_shader   =
        (LLPipeline::sImpostorRender) ? &gDeferredAlphaImpostorProgram :
        (LLPipeline::sRenderingHUDs) ? &gHUDAlphaProgram :
        &gDeferredAlphaProgram;

    prepare_alpha_shader(simple_shader, true, water_sign); //prime simple shader (loads shadow relevant uniforms)

    LLGLSLShader* materialShader = gDeferredMaterialProgram;
    for (int i = 0; i < LLMaterial::SHADER_COUNT*2; ++i)
    {
        prepare_alpha_shader(&materialShader[i], true, water_sign);
    }

    pbr_shader =
        (LLPipeline::sRenderingHUDs) ? &gHUDPBRAlphaProgram :
        &gDeferredPBRAlphaProgram;

    prepare_alpha_shader(pbr_shader, true, water_sign);

    // explicitly unbind here so render loop doesn't make assumptions about the last shader
    // already being setup for rendering
    LLGLSLShader::unbind();

    // LLRHI (research/41): the main view's above-water alpha as per-pixel
    // lists. Inside the session every blended fragment (rigged or not, the
    // glow pass's alpha included) becomes a node with its depth and blend
    // state, the backend writes no depth for them, and the resolve blends
    // each pixel's nodes back to front: rigged and other alpha in one session,
    // whatever order they come in. HUDs, impostors, probe faces, under water
    // and GL stay on the usual path; so does the depth of field pass below
    // (which then adds the rigged alpha's depth, as the session wrote none).
    // Hybrid (LLRHI::sOITHybrid, RenderOITMode 1): the avatars' alpha (rigged and
    // attachments) first, as nodes that mark their pixels; then the rest far
    // to near, which joins the lists only at marked pixels and elsewhere
    // blends into the target as without OIT (LLRHI::sOITWorld).
    bool oit = LLRHI::sOIT && LLRHI::sOITBegin && LLRHI::sOITResolve &&
               getType() == LLDrawPool::POOL_ALPHA_POST_WATER && !LLPipeline::sRenderingHUDs &&
               !LLPipeline::sImpostorRender && !LLPipeline::sUnderWaterRender && !LLPipeline::sReflectionRender &&
               !LLPipeline::sShadowRender && !gCubeSnapshot;
    bool hybrid = false;
    if (oit)
    {
        gGL.flush();    // nothing drawn before goes into the session
        const glm::mat4& projection = gGL.getProjectionMatrix();
        hybrid = LLRHI::sOITHybrid && LLRHI::sOITWorld;     // latched for the session
        oit = LLRHI::sOITBegin(projection[2][2], projection[3][2], hybrid);
        hybrid = hybrid && oit;
        LLRHI::sOITPriority = 0;    // the GLTF scene and anything else outside the groups
    }
    mOIT = oit;

    if (!LLPipeline::sRenderingHUDs)
    {
        // first pass, render rigged objects only and render to depth buffer
        // (hybrid: without the GLTF scene, which is no avatar's)
        forwardRender(true, hybrid ? GROUPS_AVATAR : GROUPS_ALL);
    }

    if (hybrid)
    {
        // The attachments' alpha, then (once every avatar node is in the lists)
        // the rest: the GLTF scene first, as upstream draws it before all other
        // alpha, then the groups far to near, with the attachments' batches that
        // make no nodes (no OIT variant) in their place.
        forwardRender(false, GROUPS_AVATAR);
        gGL.flush();
        LLRHI::sOITWorld();
        forwardRender(false, GROUPS_WORLD);
    }
    else
    {
        // second pass, regular forward alpha rendering
        forwardRender();
    }

    if (oit)
    {
        gGL.flush();    // everything the session drew, before its resolve
        if (LLRHI::sGPUTimingAlpha || LLRHI::sFrameCost)    // frame cost (research/83): the resolve on its own
        {
            LLRHI::gpuMark("alpha oit resolve");
        }
        LLRHI::sOITResolve();
        mOIT = false;
        mOITPriorities.clear();
        LLRHI::sOITPriority = 0;
    }

    // final pass, render to depth for depth of field effects
    // <plus> volumetric light (plus/volumetric): its march and composite in
    // renderFinalize stop each view ray at the scene depth. The OIT session
    // writes no depth for the rigged alpha (upstream's rigged pass does), so
    // behind hair the ray ran on through the sky and the light of the whole
    // air column was added over the hair (alpha-sky). The rigged alpha's depth
    // is written for it too, as for depth of field (PlusVolumetricAlpha 1; 2:
    // all the alpha's, exactly as depth of field writes it; both only to
    // compare). By default (PlusVolumetricAlpha 3) the shafts are drawn before
    // this pool instead (at its start) and want no alpha depth.
    const bool dof_depth = !LLPipeline::sImpostorRender && LLPipeline::RenderDepthOfField && !gCubeSnapshot && !LLPipeline::sRenderingHUDs && getType() == LLDrawPool::POOL_ALPHA_POST_WATER;
    // (the main view's: main_view at the start)
    const U32 volumetric_depth = main_view ? plus::volumetric::alphaDepth() : 0;
    const bool all_depth = dof_depth || volumetric_depth >= 2;
    const bool rigged_depth = oit && (all_depth || volumetric_depth >= 1);
    if (all_depth || rigged_depth)  // was: the condition in dof_depth
    // </plus>
    {
        //update depth buffer sampler
        simple_shader = fullbright_shader = &gDeferredFullbrightAlphaMaskProgram;

        // <plus> depth of field behind alpha (plus/dofalpha): the legacy material and PBR
        // programs discard below the cutoff too (they wrote depth over their whole triangles)
        plus::dofalpha::beginDepthPass(dof_depth);  // research/218: dof_depth for focus through glass
        const F32 depth_cutoff = plus::dofalpha::depthCutoff();     // upstream's 0.33 with PlusDoFAlpha off
        // </plus>

        simple_shader->bind();
        // <plus> was: simple_shader->setMinimumAlpha(0.33f);
        simple_shader->setMinimumAlpha(depth_cutoff);
        // </plus>

        // mask off color buffer writes as we're only writing to depth buffer
        gGL.setColorMask(false, false);

        // If the face is more than 90% transparent, then don't update the Depth buffer for Dof
        // We don't want the nearly invisible objects to cause of DoF effects
        // <plus> not when volumetric light wants the rigged alpha's alone
        if (all_depth)
        // </plus>
        renderAlpha(getVertexDataMask() | LLVertexBuffer::MAP_TEXTURE_INDEX | LLVertexBuffer::MAP_TANGENT | LLVertexBuffer::MAP_TEXCOORD1 | LLVertexBuffer::MAP_TEXCOORD2,
            true); // <--- discard mostly transparent faces

        // <plus> was: if (oit)
        if (rigged_depth)
        // </plus>
        {
            // LLRHI (research/41): the session wrote no depth, where upstream's
            // rigged pass does: the rigged alpha's depth for depth of field and
            // volumetric light too.
            LLGLDepthTest depth(GL_TRUE, GL_TRUE);
            if (LLGLSLShader* rigged = simple_shader->mRiggedVariant)
            {
                rigged->bind();
                // <plus> was: rigged->setMinimumAlpha(0.33f);
                rigged->setMinimumAlpha(depth_cutoff);
                // </plus>
            }
            renderAlpha(getVertexDataMask() | LLVertexBuffer::MAP_TEXTURE_INDEX | LLVertexBuffer::MAP_TANGENT | LLVertexBuffer::MAP_TEXCOORD1 | LLVertexBuffer::MAP_TEXCOORD2,
                true, true);
        }

        gGL.setColorMask(true, false);

        // <plus> depth of field behind alpha (plus/dofalpha): the colour passes discard nothing
        plus::dofalpha::endDepthPass();
        // </plus>
    }

    sCensus.endPool();
}

void LLDrawPoolAlpha::forwardRender(bool rigged, EAlphaGroups groups)
{
    gPipeline.enableLightsDynamic();

    LLGLSPipelineAlpha gls_pipeline_alpha;

    //enable writing to alpha for emissive effects
    gGL.setColorMask(true, true);

    bool write_depth = rigged ||
        LLDrawPoolWater::sSkipScreenCopy
        // we want depth written so that rendered alpha will
        // contribute to the alpha mask used for impostors
        || LLPipeline::sImpostorRenderAlphaDepthPass
        || getType() == LLDrawPoolAlpha::POOL_ALPHA_PRE_WATER; // needed for accurate water fog


    LLGLDepthTest depth(GL_TRUE, write_depth ? GL_TRUE : GL_FALSE);

    mColorSFactor = LLRender::BF_SOURCE_ALPHA;           // } regular alpha blend
    mColorDFactor = LLRender::BF_ONE_MINUS_SOURCE_ALPHA; // }
    mAlphaSFactor = LLRender::BF_ZERO;                         // } glow suppression
    mAlphaDFactor = LLRender::BF_ONE_MINUS_SOURCE_ALPHA;       // }
    gGL.blendFunc(mColorSFactor, mColorDFactor, mAlphaSFactor, mAlphaDFactor);

    // LLRHI (research/41): a hybrid OIT session draws the GLTF scene with
    // the world's alpha (writing depth as the rigged pass would).
    if (((rigged && groups == GROUPS_ALL) || groups == GROUPS_WORLD) && mType == LLDrawPool::POOL_ALPHA_POST_WATER)
    { // draw GLTF scene to depth buffer before rigged alpha
        gpu_mark_kind(KIND_GLTF_SCENE);
        sCensus.tagScene();
        LLGLDepthTest gltf_depth(GL_TRUE, GL_TRUE);
        LL::GLTFSceneManager::instance().render(false, false);
        LL::GLTFSceneManager::instance().render(false, true);
        LL::GLTFSceneManager::instance().render(false, false, true);
        LL::GLTFSceneManager::instance().render(false, true, true);
        sCensus.clear();
    }

    // If the face is more than 90% transparent, then don't update the Depth buffer for Dof
    // We don't want the nearly invisible objects to cause of DoF effects
    renderAlpha(getVertexDataMask() | LLVertexBuffer::MAP_TEXTURE_INDEX | LLVertexBuffer::MAP_TANGENT | LLVertexBuffer::MAP_TEXCOORD1 | LLVertexBuffer::MAP_TEXCOORD2, false, rigged, groups);

    gGL.setColorMask(true, false);

    if (!rigged && groups != GROUPS_AVATAR && (LLPipeline::sRenderingHUDs || getType() == LLDrawPoolAlpha::POOL_ALPHA_POST_WATER))
    { //render "highlight alpha" on final non-rigged pass for non-HUDs (HUDs only run pre-water alpha pass)
        // NOTE -- hacky call here protected by !rigged instead of alongside "forwardRender"
        // so renderDebugAlpha is executed while gls_pipeline_alpha and depth GL state
        // variables above are still in scope
        renderDebugAlpha();
    }
}

void LLDrawPoolAlpha::renderDebugAlpha()
{
    if (sShowDebugAlpha && !gCubeSnapshot && !LLPipeline::sReflectionRender)
    {
        gHighlightProgram.bind();
        gGL.diffuseColor4f(1, 0, 0, 1);
        gGL.getTexUnit(0)->bindFast(LLViewerFetchedTexture::getSmokeImage());


        renderAlphaHighlight();

        pushUntexturedBatches(LLRenderPass::PASS_ALPHA_MASK);
        pushUntexturedBatches(LLRenderPass::PASS_ALPHA_INVISIBLE);

        // Material alpha mask
        gGL.diffuseColor4f(0, 0, 1, 1);
        pushUntexturedBatches(LLRenderPass::PASS_MATERIAL_ALPHA_MASK);
        pushUntexturedBatches(LLRenderPass::PASS_NORMMAP_MASK);
        pushUntexturedBatches(LLRenderPass::PASS_SPECMAP_MASK);
        pushUntexturedBatches(LLRenderPass::PASS_NORMSPEC_MASK);
        pushUntexturedBatches(LLRenderPass::PASS_FULLBRIGHT_ALPHA_MASK);
        pushUntexturedBatches(LLRenderPass::PASS_GLTF_PBR_ALPHA_MASK);

        gGL.diffuseColor4f(0, 1, 0, 1);
        pushUntexturedBatches(LLRenderPass::PASS_INVISIBLE);

        gHighlightProgram.mRiggedVariant->bind();
        gGL.diffuseColor4f(1, 0, 0, 1);

        pushRiggedBatches(LLRenderPass::PASS_ALPHA_MASK_RIGGED, false);
        pushRiggedBatches(LLRenderPass::PASS_ALPHA_INVISIBLE_RIGGED, false);

        // Material alpha mask
        gGL.diffuseColor4f(0, 0, 1, 1);
        pushRiggedBatches(LLRenderPass::PASS_MATERIAL_ALPHA_MASK_RIGGED, false);
        pushRiggedBatches(LLRenderPass::PASS_NORMMAP_MASK_RIGGED, false);
        pushRiggedBatches(LLRenderPass::PASS_SPECMAP_MASK_RIGGED, false);
        pushRiggedBatches(LLRenderPass::PASS_NORMSPEC_MASK_RIGGED, false);
        pushRiggedBatches(LLRenderPass::PASS_FULLBRIGHT_ALPHA_MASK_RIGGED, false);
        pushRiggedBatches(LLRenderPass::PASS_GLTF_PBR_ALPHA_MASK_RIGGED, false);

        gGL.diffuseColor4f(0, 1, 0, 1);
        pushRiggedBatches(LLRenderPass::PASS_INVISIBLE_RIGGED, false);
        LLGLSLShader::sCurBoundShaderPtr->unbind();
    }
}

void LLDrawPoolAlpha::renderAlphaHighlight()
{
    for (int pass = 0; pass < 2; ++pass)
    { //two passes, one rigged and one not
        const LLVOAvatar* lastAvatar = nullptr;
        U64 lastMeshId = 0;
        bool skipLastSkin = false;

        LLCullResult::sg_iterator begin = pass == 0 ? gPipeline.beginAlphaGroups() : gPipeline.beginRiggedAlphaGroups();
        LLCullResult::sg_iterator end = pass == 0 ? gPipeline.endAlphaGroups() : gPipeline.endRiggedAlphaGroups();

        for (LLCullResult::sg_iterator i = begin; i != end; ++i)
        {
            LLSpatialGroup* group = *i;
            if (group->getSpatialPartition()->mRenderByGroup &&
                !group->isDead())
            {
                LLSpatialGroup::drawmap_elem_t& draw_info = group->mDrawMap[LLRenderPass::PASS_ALPHA+pass]; // <-- hacky + pass to use PASS_ALPHA_RIGGED on second pass

                for (LLSpatialGroup::drawmap_elem_t::iterator k = draw_info.begin(); k != draw_info.end(); ++k)
                {
                    LLDrawInfo& params = **k;

                    bool rigged = (params.mAvatar != nullptr);
                    gHighlightProgram.bind(rigged);

                    if (rigged)
                    {
                        if (!uploadMatrixPalette(params.mAvatar, params.mSkinInfo, lastAvatar, lastMeshId, skipLastSkin))
                        { // failed to upload matrix palette, skip rendering
                            continue;
                        }
                    }

                    gGL.diffuseColor4f(1, 0, 0, 1);
                    LLRenderPass::applyModelMatrix(params);
                    params.mVertexBuffer->setBuffer();
                    params.mVertexBuffer->drawRange(LLRender::TRIANGLES, params.mStart, params.mEnd, params.mCount, params.mOffset);
                }
            }
        }
    }

    // make sure static version of highlight shader is bound before returning
    gHighlightProgram.bind();
}

inline bool IsFullbright(LLDrawInfo& params)
{
    return params.mFullbright;
}

inline bool IsMaterial(LLDrawInfo& params)
{
    return params.mMaterial != nullptr;
}

inline bool IsEmissive(LLDrawInfo& params)
{
    return params.mVertexBuffer->hasDataType(LLVertexBuffer::TYPE_EMISSIVE);
}

inline void Draw(LLDrawInfo* draw, U32 mask)
{
    LLRenderPass::drawGeometry(*draw);
}

bool LLDrawPoolAlpha::TexSetup(LLDrawInfo* draw, bool use_material)
{
    bool tex_setup = false;

    if (draw->mGLTFMaterial)
    {
        if (draw->mTextureMatrix)
        {
            tex_setup = true;
            gGL.getTexUnit(0)->activate();
            gGL.matrixMode(LLRender::MM_TEXTURE);
            gGL.loadMatrix((GLfloat*)draw->mTextureMatrix->mMatrix);
            gPipeline.mTextureMatrixOps++;
        }
    }
    else
    {
        if (!LLPipeline::sRenderingHUDs && use_material && current_shader)
        {
            if (draw->mNormalMap)
            {
                current_shader->bindTexture(LLShaderMgr::BUMP_MAP, draw->mNormalMap);
            }

            if (draw->mSpecularMap)
            {
                current_shader->bindTexture(LLShaderMgr::SPECULAR_MAP, draw->mSpecularMap);
            }
        }
        else if (current_shader == simple_shader || current_shader == simple_shader->mRiggedVariant)
        {
            current_shader->bindTexture(LLShaderMgr::BUMP_MAP, LLViewerFetchedTexture::sFlatNormalImagep);
            current_shader->bindTexture(LLShaderMgr::SPECULAR_MAP, LLViewerFetchedTexture::sWhiteImagep);
        }
        if (draw->mTextureList.size() > 1)
        {
            for (U32 i = 0; i < draw->mTextureList.size(); ++i)
            {
                if (draw->mTextureList[i].notNull())
                {
                    gGL.getTexUnit(i)->bindFast(draw->mTextureList[i]);
                }
            }
        }
        else
        { //not batching textures or batch has only 1 texture -- might need a texture matrix
            if (draw->mTexture.notNull())
            {
                if (use_material)
                {
                    current_shader->bindTexture(LLShaderMgr::DIFFUSE_MAP, draw->mTexture);
                }
                else
                {
                    gGL.getTexUnit(0)->bindFast(draw->mTexture);
                }

                if (draw->mTextureMatrix)
                {
                    tex_setup = true;
                    gGL.getTexUnit(0)->activate();
                    gGL.matrixMode(LLRender::MM_TEXTURE);
                    gGL.loadMatrix((GLfloat*)draw->mTextureMatrix->mMatrix);
                    gPipeline.mTextureMatrixOps++;
                }
            }
            else
            {
                gGL.getTexUnit(0)->unbindFast(LLTexUnit::TT_TEXTURE);
            }
        }
    }

    return tex_setup;
}

void LLDrawPoolAlpha::RestoreTexSetup(bool tex_setup)
{
    if (tex_setup)
    {
        gGL.getTexUnit(0)->activate();
        gGL.matrixMode(LLRender::MM_TEXTURE);
        gGL.loadIdentity();
        gGL.matrixMode(LLRender::MM_MODELVIEW);
    }
}

void LLDrawPoolAlpha::drawEmissive(LLDrawInfo* draw)
{
    LLGLSLShader::sCurBoundShaderPtr->uniform1f(LLShaderMgr::EMISSIVE_BRIGHTNESS, 1.f);
    LLRenderPass::drawGeometry(*draw);
}


void LLDrawPoolAlpha::renderEmissives(std::vector<LLDrawInfo*>& emissives)
{
    emissive_shader->bind();
    emissive_shader->uniform1f(LLShaderMgr::EMISSIVE_BRIGHTNESS, 1.f);
    gpu_mark_kind(KIND_GLOW);

    for (LLDrawInfo* draw : emissives)
    {
        setOITPriority(draw);
        bool tex_setup = TexSetup(draw, false);
        sCensus.tag(KIND_GLOW, draw);
        drawEmissive(draw);
        sCensus.clear();
        RestoreTexSetup(tex_setup);
    }
}

// LLRHI: direct draws of GLTF batches take the material's textures in the
// packet (LLFetchedGLTFMaterial::bindDirect) instead of binding them.
static constexpr U32 GLTF_DIRECT_UNITS = 32;

static bool gltf_direct()
{
    return LLRHI::sGLDrawsDisabled && LLRHI::sDirect && LLRHI::sDirectDraw;
}

// The material and the geometry of a GLTF emissive batch: bind() + drawGeometry,
// or a direct draw with the material's textures. Like bind() + drawGeometry it
// does not load the batch's texture matrix.
static void draw_gltf_emissive(LLDrawInfo& draw)
{
    LLFetchedGLTFMaterial* mat = draw.mGLTFMaterial;
    GrayWire::Scope wire(draw, true);   // <plus> research/243: as its draw in the alpha pass; research/259: a wireframe for its wearer's band </plus>
    if (gltf_direct())
    {
        U32 textures[GLTF_DIRECT_UNITS];
        const U32 texture_count = mat->bindDirect(draw.mTexture, textures, GLTF_DIRECT_UNITS);
        if (LLRenderPass::pushDirectBatch(draw, false, false, textures, texture_count, false))
        {
            return;
        }
        mat->bindTextures(draw.mTexture);   // refused: through GL as bind() would
    }
    else
    {
        mat->bind(draw.mTexture);
    }
    LLRenderPass::drawGeometry(draw);
}

void LLDrawPoolAlpha::renderPbrEmissives(std::vector<LLDrawInfo*>& emissives)
{
    pbr_emissive_shader->bind();
    gpu_mark_kind(KIND_GLOW);

    for (LLDrawInfo* draw : emissives)
    {
        llassert(draw->mGLTFMaterial);
        setOITPriority(draw);
        LLGLDisable cull_face(draw->mGLTFMaterial->mDoubleSided ? GL_CULL_FACE : 0);
        sCensus.tag(KIND_GLOW, draw);
        draw_gltf_emissive(*draw);
        sCensus.clear();
    }
}

void LLDrawPoolAlpha::renderRiggedEmissives(std::vector<LLDrawInfo*>& emissives)
{
    LLGLDepthTest depth(GL_TRUE, GL_FALSE); //disable depth writes since "emissive" is additive so sorting doesn't matter
    LLGLSLShader* shader = emissive_shader->mRiggedVariant;
    shader->bind();
    shader->uniform1f(LLShaderMgr::EMISSIVE_BRIGHTNESS, 1.f);
    gpu_mark_kind(KIND_GLOW);

    const LLVOAvatar* lastAvatar = nullptr;
    U64 lastMeshId = 0;
    bool skipLastSkin = false;

    for (LLDrawInfo* draw : emissives)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DRAWPOOL("Emissives");

        if (uploadMatrixPalette(draw->mAvatar, draw->mSkinInfo, lastAvatar, lastMeshId, skipLastSkin))
        {
            setOITPriority(draw);
            bool tex_setup = TexSetup(draw, false);
            sCensus.tag(KIND_GLOW, draw);
            drawEmissive(draw);
            sCensus.clear();
            RestoreTexSetup(tex_setup);
        }
    }
}

void LLDrawPoolAlpha::renderRiggedPbrEmissives(std::vector<LLDrawInfo*>& emissives)
{
    LLGLDepthTest depth(GL_TRUE, GL_FALSE); //disable depth writes since "emissive" is additive so sorting doesn't matter
    pbr_emissive_shader->bind(true);
    gpu_mark_kind(KIND_GLOW);

    const LLVOAvatar* lastAvatar = nullptr;
    U64 lastMeshId = 0;
    bool skipLastSkin = false;

    for (LLDrawInfo* draw : emissives)
    {
        if (!uploadMatrixPalette(draw->mAvatar, draw->mSkinInfo, lastAvatar, lastMeshId, skipLastSkin))
        { // failed to upload matrix palette, skip rendering
            continue;
        }

        setOITPriority(draw);
        LLGLDisable cull_face(draw->mGLTFMaterial->mDoubleSided ? GL_CULL_FACE : 0);
        sCensus.tag(KIND_GLOW, draw);
        draw_gltf_emissive(*draw);
        sCensus.clear();
    }
}

// <plus> research/205 3a-3, 3a-5: the alpha passes' state, for the scene's draws of them.
// The alpha pass's program bound (force: again even when it is bound) with
// the state renderAlpha's loop gives a batch of it: the exposure map for
// the fullbright program (at its bind), TexSetup's flat normal and white
// specular for the plain one, the uniforms of a batch without a material
// (a legacy material's are each record's, research/205 3a-4).
static LLGLSLShader* bind_scene_alpha_program(U32 pass, bool force)
{
    LLGLSLShader* shader = LLSceneRecords::alphaPassProgram(pass);
    const bool fullbright = pass == LLSceneRecords::alphaPass(true);
    const bool plain = pass == LLSceneRecords::alphaPass(false);
    if (force || current_shader != shader)
    {
        plus::alphapass::noteBind(*shader);     // plus/alphapass
        gPipeline.bindDeferredShaderFast(*shader);
        if (fullbright)
        {
            const S32 channel = shader->enableTexture(LLShaderMgr::EXPOSURE_MAP);
            if (channel > -1)
            {
                gGL.getTexUnit(channel)->bind(&gPipeline.mExposureMap);
            }
        }
    }
    if (plain)
    {
        shader->bindTexture(LLShaderMgr::BUMP_MAP, LLViewerFetchedTexture::sFlatNormalImagep);
        shader->bindTexture(LLShaderMgr::SPECULAR_MAP, LLViewerFetchedTexture::sWhiteImagep);
    }
    if (plain || fullbright)
    {
        shader->uniform4f(LLShaderMgr::SPECULAR_COLOR, 1.f, 1.f, 1.f, 1.f);
        shader->uniform1f(LLShaderMgr::ENVIRONMENT_INTENSITY, 0.f);
        shader->uniform1f(LLShaderMgr::EMISSIVE_BRIGHTNESS, 1.f);
    }
    return shader;
}

// research/205 3a-5: a span at its first batch, in the loop's place: its
// program bound as drawSceneAlpha bound it for the pass's expand and the
// usual blend; the scene draws its batches (false: the loop draws them).
// The loop's lighting flags as its choice of that program would leave them.
static bool draw_scene_alpha_span(const LLSceneRecords::AlphaSpans& spans, const LLSceneRecords::AlphaSpan& span,
                                  LLRender::eBlendFactor alpha_sfactor, LLRender::eBlendFactor alpha_dfactor, bool& light_enabled,
                                  bool& initialized_lighting)
{
    if (!gSceneRecords.alphaSpanReady(span))
    {
        return false;
    }
    const U32 pass = LLSceneRecords::alphaPassAt(span.pass_index);
    bind_scene_alpha_program(pass, false);
    initialized_lighting = true;
    light_enabled = pass != LLSceneRecords::alphaPass(true);
    gGL.blendFunc(LLRender::BF_SOURCE_ALPHA, LLRender::BF_ONE_MINUS_SOURCE_ALPHA, alpha_sfactor, alpha_dfactor);
    return gSceneRecords.drawAlphaSpan(spans, span);
}
// </plus>

// <plus> research/205 3a-3: the main view's above-water alpha in an OIT "all"
// session (renderPostDeferred, forwardRender's state set): the groups the
// loop draws (its rules: by group, alive, some part above the water) are
// visible to the scene's alpha view with their OIT priority, then each alpha
// pass their records have is drawn from the records with its program bound
// and the state the loop gives such a batch (bind_scene_alpha_program). Only
// non-rigged batches of a full session: the order of the nodes' draws does
// not matter there (the resolve orders each pixel's nodes by priority).
// research/205 3a-5: in a hybrid session's world half and without OIT (the
// sorted way, where the order shows) the same groups but the hybrid
// session's avatar groups (the loop splits their batches between the
// halves) are visible, each with its spans, and each pass is only expanded
// here: the loop has the scene draw a span at its first batch
// (draw_scene_alpha_span), in the group's place far to near.
// research/205 (vk-s3a6): then the glow pass with the state the loop's glow
// draws have (renderEmissives' program, the glow blend): drawn here in an
// OIT "all" session (the loop leaves those batches' glow out), expanded for
// the groups' glow spans in the sorted way (drawn at the groups' ends).
bool LLDrawPoolAlpha::drawSceneAlpha(bool depth_only, bool rigged, EAlphaGroups groups)
{
    const bool all = groups == GROUPS_ALL && mOIT;
    const bool sorted = (groups == GROUPS_WORLD && mOIT) || (groups == GROUPS_ALL && !mOIT);
    if (!LLSceneRecords::alphaWanted() || rigged || depth_only || !(all || sorted) ||
        getType() != LLDrawPool::POOL_ALPHA_POST_WATER || LLPipeline::sUnderWaterRender || LLPipeline::sRenderingHUDs ||
        LLPipeline::sProbeCullSerial || !gSceneRecords.beginAlphaView(sorted))
    {
        return false;
    }
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    const F32 water_height = LLEnvironment::instance().getWaterHeight();
    for (LLCullResult::sg_iterator i = gPipeline.beginAlphaGroups(); i != gPipeline.endAlphaGroups(); ++i)
    {
        LLSpatialGroup* group = *i;
        if (!group->getSpatialPartition()->mRenderByGroup || group->isDead())
        {
            continue;
        }
        LLSpatialBridge* bridge = group->getSpatialPartition()->asBridge();
        const LLVector4a* ext = bridge ? bridge->getSpatialExtents() : group->getExtents();
        if (ext[1].getF32ptr()[2] < water_height)
        {
            continue;   // no part above the water: the loop skips it
        }
        if (groups == GROUPS_WORLD && oit_avatar_group(group))
        {
            continue;   // an attachment's: the loop draws some of its batches in each half
        }
        gSceneRecords.markAlphaVisible(group, mOIT ? oit_group_priority(group, false) : 0);
    }
    gSceneRecords.endAlphaView();

    gGL.blendFunc(LLRender::BF_SOURCE_ALPHA, LLRender::BF_ONE_MINUS_SOURCE_ALPHA, mAlphaSFactor, mAlphaDFactor);
    const U32 passes = gSceneRecords.alphaPassesVisible();
    for (U32 index = 0; index < LLSceneRecords::ALPHA_PASSES; ++index)
    {
        if ((passes & (1u << index)) && index != LLSceneRecords::ALPHA_GLOW_INDEX)
        {
            const U32 pass = LLSceneRecords::alphaPassAt(index);
            bind_scene_alpha_program(pass, true);
            gSceneRecords.drawAlphaPass(pass);
        }
    }
    if (passes & (1u << LLSceneRecords::ALPHA_GLOW_INDEX))
    {
        // As the loop's glow at a group's end (renderAlpha, renderEmissives).
        gPipeline.enableLightsDynamic();
        gGL.blendFunc(LLRender::BF_ZERO, LLRender::BF_ONE, LLRender::BF_ONE, LLRender::BF_ONE);
        emissive_shader->bind();
        emissive_shader->uniform1f(LLShaderMgr::EMISSIVE_BRIGHTNESS, 1.f);
        gSceneRecords.drawAlphaPass(LLSceneRecords::alphaGlowPass());
        gGL.blendFunc(mColorSFactor, mColorDFactor, mAlphaSFactor, mAlphaDFactor);
    }
    // The loop binds what its batches need (as after renderPostDeferred's preparation).
    LLGLSLShader::unbind();
    return true;
}
// </plus>

void LLDrawPoolAlpha::renderAlpha(U32 mask, bool depth_only, bool rigged, EAlphaGroups groups)
{
    // <plus> research/205: the pass, for the alpha records' census and verify (renderAlphaList's too)
    LLSceneAlpha::PassScope scene_alpha_pass(getType() == LLDrawPool::POOL_ALPHA_POST_WATER, depth_only, rigged, groups, mOIT && !depth_only);
    // research/205 3a-3: an OIT "all" session's alpha from the scene's records first (the session
    // orders the nodes by priority, not by when they come); the loop leaves out what it drew
    // (gSceneRecords.alphaDrawnBy). 3a-5: the sorted way's passes expanded, their spans drawn by
    // the loop's turn (gSceneRecords.alphaSpans). H6: while the scene's alpha takes the pass (any
    // mode but off), the loop below draws the rest of it, not plus/alphalist's copy of it.
    LLSceneRecords::AlphaPassScope scene_alpha_records{ gSceneRecords };
    const bool scene_alpha = drawSceneAlpha(depth_only, rigged, groups);
    // </plus>
    if (!scene_alpha && renderAlphaList(mask, depth_only, rigged, groups)) { return; } // LLRHI native (research/137 N3)
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    plus::alphapass::PassScope plus_pass_scope(depth_only); // <plus> plus/alphapass: the pass's CPU counters </plus>
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
            // LLRHI (research/41): the hybrid OIT session's half (rigged groups
            // are all avatars'). The avatar half draws the attachments' batches
            // that make nodes; the world half every other group and the
            // attachments' other batches, far to near (per batch below).
            const bool avatar_group = !rigged && groups != GROUPS_ALL && oit_avatar_group(group);
            if (!rigged && groups == GROUPS_AVATAR && !avatar_group)
            {
                continue;
            }
            // <plus> research/218 focus through glass (plus/dofalpha): in depth of field's depth pass the
            // world's see-through alpha writes no depth; an avatar's and its attachments' keep theirs
            const bool glass_world = depth_only && plus::dofalpha::glassActive() && !rigged && !oit_avatar_group(group);
            // </plus>

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
            static std::vector<U32> emissive_places;    // <plus> research/205 (vk-s3a6): each of emissives' place in the draw map </plus>

            emissives.resize(0);
            emissive_places.resize(0);  // <plus> vk-s3a6 </plus>
            rigged_emissives.resize(0);
            pbr_emissives.resize(0);
            pbr_rigged_emissives.resize(0);

            bool is_particle_or_hud_particle = group->getSpatialPartition()->mPartitionType == LLViewerRegion::PARTITION_PARTICLE
                                                      || group->getSpatialPartition()->mPartitionType == LLViewerRegion::PARTITION_HUD_PARTICLE;

            bool disable_cull = is_particle_or_hud_particle;
            LLGLDisable cull(disable_cull ? GL_CULL_FACE : 0);

            LLSpatialGroup::drawmap_elem_t& draw_info = rigged ? group->mDrawMap[LLRenderPass::PASS_ALPHA_RIGGED] : group->mDrawMap[LLRenderPass::PASS_ALPHA];

            if (LLRHI::sAlphaCensus)
            {
                sCensus.setGroup(group, ext);   // research/54: which objects the batches draw
            }
            if (LLSceneAlpha::sActive) { gSceneAlpha.noteGroup(group); } // <plus> research/205 </plus>

            // LLRHI (research/41): the group's OIT priority, per batch below.
            const U32 oit_priority = mOIT && !depth_only ? oit_group_priority(group, rigged) : 0;
            mOITPriorities.clear();

            // <plus> research/205 3a-5: the group's spans the scene draws (the sorted way), each at its first batch
            const LLSceneRecords::AlphaSpans scene_spans = gSceneRecords.alphaSpans(group);
            U32 next_span = 0;
            U32 span_end = 0;           // the place past the span gone through last
            bool span_drawn = false;
            // </plus>

            for (LLSpatialGroup::drawmap_elem_t::iterator k = draw_info.begin(); k != draw_info.end(); ++k)
            {
                LLDrawInfo& params = **k;
                if ((bool)params.mAvatar != rigged)
                {
                    continue;
                }

                if (LLPipeline::sProbeCullSerial && params.mProbeCullStamp == LLPipeline::sProbeCullSerial)
                {
                    // LLRHI: entirely outside this reflection probe face (LLPipeline::postSort)
                    continue;
                }

                LL_PROFILE_ZONE_NAMED_CATEGORY_DRAWPOOL("ra - push batch");

                if (mOIT && !depth_only)
                {
                    // The batch's place in the group: later batches on top, as upstream blends them.
                    const U32 priority = oit_priority | (U32)llmin(k - draw_info.begin(), (std::ptrdiff_t)4094);
                    mOITPriorities.emplace_back(&params, priority);
                    LLRHI::sOITPriority = priority;
                }

                // <plus> research/205 3a-5: a span the scene draws, from its first batch on (its
                // batches' glow below, as before); one it cannot draw the loop draws batch by batch
                const U32 place = (U32)(k - draw_info.begin());
                while (next_span < scene_spans.count && scene_spans.spans[next_span].first <= place)
                {
                    const LLSceneRecords::AlphaSpan& span = scene_spans.spans[next_span++];
                    span_end = (U32)span.first + span.count;
                    span_drawn = span.first == place && draw_scene_alpha_span(scene_spans, span, mAlphaSFactor, mAlphaDFactor, light_enabled,
                                                                                 initialized_lighting);
                }
                if (span_drawn && place < span_end)
                {
                    if (LLSceneAlpha::sActive)
                    {
                        gSceneAlpha.noteBatch(group, params, place, current_shader, LLRHI::sOITPriority);
                    }
                    if (getType() != LLDrawPool::POOL_ALPHA_PRE_WATER && plus::alphapass::glowPass(group, params, mOIT, depth_only))
                    {
                        emissives.push_back(&params);   // a record's batch: not rigged, no GLTF material
                        emissive_places.push_back(place);
                    }
                    continue;
                }
                // research/205 3a-3: drawn from its record already (its glow below, as before; vk-s3a6: unless
                // the glow pass drew it too)
                if (const LLGLSLShader* scene_program = gSceneRecords.alphaDrawnBy(params))
                {
                    initialized_lighting = true;    // as its program's choice below would leave them
                    light_enabled = !params.mFullbright;
                    if (LLSceneAlpha::sActive)
                    {
                        gSceneAlpha.noteBatch(group, params, (size_t)(k - draw_info.begin()), scene_program, LLRHI::sOITPriority);
                    }
                    if (getType() != LLDrawPool::POOL_ALPHA_PRE_WATER && plus::alphapass::glowPass(group, params, mOIT, depth_only) &&
                        !gSceneRecords.glowDrawnBy(params))
                    {
                        emissives.push_back(&params);   // a record's batch: not rigged, no GLTF material
                        emissive_places.push_back(place);
                    }
                    continue;
                }
                // </plus>

                LLMaterial* mat = NULL;
                LLGLTFMaterial *gltf_mat = params.mGLTFMaterial;
                // LLRHI: a blended GLTF batch's textures (and texture matrix) go with
                // its direct draw instead of being bound through GL.
                bool direct_gltf = false;
                U32 gltf_textures[GLTF_DIRECT_UNITS];
                U32 gltf_texture_count = 0;

                LLGLDisable cull_face(gltf_mat && gltf_mat->mDoubleSided ? GL_CULL_FACE : 0);
                // <plus> research/243: a texture not loaded yet is a wireframe for the binds and the draw
                // below (the direct GLTF draw, and the look on unit 0 for TexSetup's binds)
                GrayWire::Scope wire(params, true);  // research/259: and for its wearer's band (alpha takes none)
                // </plus>

                if (gltf_mat && gltf_mat->mAlphaMode == LLGLTFMaterial::ALPHA_MODE_BLEND)
                {
                    target_shader = pbr_shader;
                    if (params.mAvatar != nullptr)
                    {
                        target_shader = target_shader->mRiggedVariant;
                    }

                    // shader must be bound before LLGLTFMaterial::bind
                    if (current_shader != target_shader)
                    {
                        plus::alphapass::noteBind(*target_shader); // <plus> plus/alphapass </plus>
                        gPipeline.bindDeferredShaderFast(*target_shader);
                    }

                    direct_gltf = gltf_direct();
                    if (direct_gltf)
                    {
                        gltf_texture_count = params.mGLTFMaterial->bindDirect(params.mTexture, gltf_textures, GLTF_DIRECT_UNITS);
                    }
                    else
                    {
                        params.mGLTFMaterial->bind(params.mTexture);
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
                        plus::alphapass::noteBind(*target_shader); // <plus> plus/alphapass </plus>
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
                        current_shader->uniform4f(LLShaderMgr::SPECULAR_COLOR, spec_color.mV[VRED], spec_color.mV[VGREEN], spec_color.mV[VBLUE], spec_color.mV[VALPHA]);
                        current_shader->uniform1f(LLShaderMgr::ENVIRONMENT_INTENSITY, env_intensity);
                        current_shader->uniform1f(LLShaderMgr::EMISSIVE_BRIGHTNESS, brightness);
                    }
                }

                if (avatar_group)
                {
                    // An attachment's batch whose program makes no nodes (no OIT
                    // variant) would land in the target before all world alpha: it
                    // goes with the world's, in its group's place.
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

                // <plus> research/205: the batch drawn now, its place and OIT priority
                if (LLSceneAlpha::sActive)
                {
                    gSceneAlpha.noteBatch(group, params, (size_t)(k - draw_info.begin()), current_shader, LLRHI::sOITPriority);
                }
                // </plus>

                // A direct GLTF draw takes the texture matrix with its textures.
                bool tex_setup = direct_gltf ? false : TexSetup(&params, (mat != nullptr));

                {
                    if (LLRHI::sGPUTiming || LLRHI::sAlphaCensus)
                    {
                        // Diagnostics (research/54): the draw's GPU timing segment and census name.
                        const AlphaKind kind = draw_kind(group, params, depth_only);
                        gpu_mark_kind(kind);
                        sCensus.tag(kind, &params);
                    }
                    if (depth_only) { plus::dofalpha::batchCutoff(current_shader, glass_world); } // <plus> research/218 focus through glass </plus>
                    gGL.blendFunc((LLRender::eBlendFactor) params.mBlendFuncSrc, (LLRender::eBlendFactor) params.mBlendFuncDst, mAlphaSFactor, mAlphaDFactor);

                    bool reset_minimum_alpha = false;
                    if (!LLPipeline::sImpostorRender &&
                        params.mBlendFuncDst != LLRender::BF_SOURCE_ALPHA &&
                        params.mBlendFuncSrc != LLRender::BF_SOURCE_ALPHA &&
                        // <plus> depth of field behind alpha (plus/dofalpha): its depth pass keeps the cutoff for every draw
                        !(depth_only && plus::dofalpha::depthPassActive()))
                        // </plus>
                    { // this draw call has a custom blend function that may require rendering of "invisible" fragments
                        current_shader->setMinimumAlpha(0.f);
                        reset_minimum_alpha = true;
                    }

                    if (!direct_gltf ||
                        !LLRenderPass::pushDirectBatch(params, false, false, gltf_textures, gltf_texture_count))
                    {
                        if (direct_gltf)
                        {   // refused: the textures and texture matrix through GL as bind() and TexSetup would
                            params.mGLTFMaterial->bindTextures(params.mTexture);
                            tex_setup = TexSetup(&params, false);
                        }
                        LLRenderPass::drawGeometry(params);
                    }
                    stop_glerror();
                    sCensus.clear();

                    if (reset_minimum_alpha)
                    {
                        current_shader->setMinimumAlpha(MINIMUM_ALPHA);
                    }
                }

                // If this alpha mesh has glow, then draw it a second time to add the destination-alpha (=glow).  Interleaving these state-changing calls is expensive, but glow must be drawn Z-sorted with alpha.
                if (getType() != LLDrawPool::POOL_ALPHA_PRE_WATER &&
                    // <plus> plus/alphapass: was params.mVertexBuffer->hasDataType(LLVertexBuffer::TYPE_EMISSIVE);
                    // not a batch no face of which glows (its glow draw adds 0); research/205 (vk-s3a6): nor one
                    // whose glow the scene's glow pass drew
                    plus::alphapass::glowPass(group, params, mOIT, depth_only) && !gSceneRecords.glowDrawnBy(params))
                    // </plus>
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
                            emissive_places.push_back(place);   // <plus> vk-s3a6 </plus>
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
                // <plus> plus/alphapass: the glow pass's CPU counters
                plus::alphapass::GlowScope plus_glow_scope(!emissives.empty() || !pbr_emissives.empty() || !rigged_emissives.empty() || !pbr_rigged_emissives.empty());
                // </plus>
                gPipeline.enableLightsDynamic();

                // install glow-accumulating blend mode
                // don't touch color, add to alpha (glow)
                gGL.blendFunc(LLRender::BF_ZERO, LLRender::BF_ONE, LLRender::BF_ONE, LLRender::BF_ONE);

                bool rebind = false;
                LLGLSLShader* lastShader = current_shader;
                if (!emissives.empty())
                {
                    light_enabled = true;
                    // <plus> research/205 (vk-s3a6): the sorted way's glow spans of the group, drawn by the
                    // scene in their places among the loop's glow draws (renderEmissives' state)
                    if (scene_spans.glow_count && emissive_places.size() == emissives.size())
                    {
                        emissive_shader->bind();
                        emissive_shader->uniform1f(LLShaderMgr::EMISSIVE_BRIGHTNESS, 1.f);
                        gpu_mark_kind(KIND_GLOW);
                        glow_walk(emissive_places.data(), (U32)emissive_places.size(), scene_spans.glow_spans, scene_spans.glow_count,
                                  [&](const LLSceneRecords::AlphaSpan& span) { return gSceneRecords.drawAlphaSpan(scene_spans, span); },
                                  [&](U32 i)
                                  {
                                      LLDrawInfo* draw = emissives[i];
                                      setOITPriority(draw);
                                      bool tex_setup = TexSetup(draw, false);
                                      sCensus.tag(KIND_GLOW, draw);
                                      drawEmissive(draw);
                                      sCensus.clear();
                                      RestoreTexSetup(tex_setup);
                                  });
                    }
                    else
                    // </plus>
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

    gGL.setSceneBlendType(LLRender::BT_ALPHA);

    LLVertexBuffer::unbind();

    if (!light_enabled)
    {
        gPipeline.enableLightsDynamic();
    }
}

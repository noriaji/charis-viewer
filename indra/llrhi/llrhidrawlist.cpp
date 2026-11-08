/**
 * @file llrhidrawlist.cpp
 * @brief Draw lists (research/137 N1): the switches, the Builder and the guard
 *        that submits an open list before any other hook call.
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

#include "linden_common.h"

#include "llrhidrawlist.h"

#include "llrhi.h"
#include "llrhiatmosphere.h"
#include "llrhiglare.h"
#include "llrhigtao.h"
#include "llrhiimmediate.h"
#include "llrhilook.h"
#include "llrhiupscale.h"
#include "llrhitaa.h"
#include "llrhiocclusion.h"
#include "llrhipass.h"
#include "llrhiresource.h"
#include "llrhiskinfill.h"
#include "llrhiskinsss.h"
#include "llrhissgi.h"
#include "llrhissr.h"
#include "llrhivolumetric.h"
#include "llrhiwetlook.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <thread>
#include <type_traits>

namespace
{
    U32 env_u32(const char* name, U32 max)
    {
        U64 value = 0;
        LLRHI::envNumber(name, 0, ~(U64)0, value);
        return (U32)llmin((U64)max, value);
    }
}

U32 LLRHI::DrawList::sMode = env_u32("LLRHI_DRAW_LISTS", 2);
bool LLRHI::DrawList::sKind[LLRHI::DrawList::KIND_COUNT] =
{
    env_u32("LLRHI_DRAW_LISTS_OPAQUE", 1) != 0,
    env_u32("LLRHI_DRAW_LISTS_ALPHA", 1) != 0,
    env_u32("LLRHI_DRAW_LISTS_MATERIAL", 1) != 0,
};
U32 LLRHI::DrawList::sThreads = env_u32("LLRHI_DRAW_LISTS_THREADS", THREADS_BUILD | THREADS_RECORD);
bool (*LLRHI::DrawList::sDrawList)(const List&) = nullptr;
bool (*LLRHI::DrawList::sDrawListReady)(Context&) = nullptr;

using namespace LLRHI::DrawList;

namespace
{
    // The calling thread's open list (the guard looks only at its own thread's).
    thread_local Builder* tOpen = nullptr;
    // Lists are built on one thread (the first that opens one: the render
    // thread); another thread's open() is refused.
    std::thread::id sOwner;
    bool sOwnerSet = false;

    // The viewer side's counts (owner thread only).
    Stats sStats;
    struct CutCount
    {
        const char* name;
        U32 count;
    };
    CutCount sCutCounts[64];
    U32 sCutKinds = 0;

    void count_cut(const char* why)
    {
        ++sStats.cuts;
        for (U32 i = 0; i < sCutKinds; ++i)
        {
            if (sCutCounts[i].name == why)
            {
                ++sCutCounts[i].count;
                return;
            }
        }
        if (sCutKinds < sizeof(sCutCounts) / sizeof(sCutCounts[0]))
        {
            sCutCounts[sCutKinds++] = { why, 1 };
        }
    }

    // ---------------------------------------------------------------------
    // The guard (research/137 section 2.3, promise 2). Each hook of the CUT
    // table is wrapped: a call first submits the calling thread's open list,
    // then calls what the hook pointed at, with the same arguments and
    // result. fix-threads4: the wrapping is done once, at the backend's
    // install (installGuard), while no other thread calls the hooks yet
    // (writing a hook while a texture or cull thread reads it is a data
    // race); when lists could not open then, at the first open instead.

    template <auto Hook, typename F = std::remove_reference_t<decltype(*Hook)>>
    struct Guard;

    template <auto Hook, typename R, typename... A>
    struct Guard<Hook, R (*)(A...)>
    {
        // Atomic: texture and cull threads call hooks too, and must never see
        // the wrapper before what it calls (written first, with a full fence).
        static inline std::atomic<R (*)(A...)> sOriginal{ nullptr };
        static inline const char* sName = "";

        static R call(A... args)
        {
            if (tOpen)
            {
                Builder::cut(sName);
            }
            return sOriginal.load(std::memory_order_acquire)(static_cast<A&&>(args)...);
        }

        static void wrap(const char* name)
        {
            R (*const current)(A...) = *Hook;
            if (current && current != &call)
            {
                sName = name;
                sOriginal.store(current, std::memory_order_seq_cst);
                *Hook = &call;
            }
        }
    };

#define LLRHI_DRAWLIST_CUT(hook) Guard<&LLRHI::hook>::wrap(#hook)
    // Never cuts; the name must exist (a renamed hook fails to compile here).
#define LLRHI_DRAWLIST_KEEP(hook) (void)sizeof(&LLRHI::hook)

    // The guard's tables. tools/native-hooks.py (research/137 N5) checks that
    // every hook declared in llrhi/*.h is named here once, in a line of the
    // form CUT(sName); or KEEP(sName); (KEEP: why, in the line's comment).
    // A new hook goes into CUT unless it changes no state and records nothing.
    void guard_hooks()
    {
#define CUT LLRHI_DRAWLIST_CUT
#define KEEP LLRHI_DRAWLIST_KEEP
        // --- Draws and what draws read (llrhi.h)
        CUT(sDirectDraw);
        CUT(sDrawTemplate);
        CUT(sDrawTemplateReleased);
        CUT(sPacketDraw);                   // research/180 stage 4: draws (the list so far goes first)
        CUT(sDrawShadowPackets);
        CUT(sImmediateDraw);                // llrhiimmediate.h (research/137 N9)
        CUT(sJoinShadowRecording);          // splices recorded passes into the frame
        // --- Programs, uniforms, palettes
        CUT(sUseProgram);
        CUT(sSetUniform);
        CUT(sSetUniformMatrix);
        CUT(sUsePaletteRow);
        KEEP(sShaderProgram);               // a lookup
        KEEP(sUniformSlot);                 // a lookup (hands out a slot)
        KEEP(sPlacePalette);                // fills this frame's palette row; no state, nothing recorded
        KEEP(sTableRows);                   // room in this frame's table; no state, nothing recorded
        KEEP(sLightTableProgram);           // a lookup
        // --- Texture binding and contents
        CUT(sActiveTexture);
        CUT(sBindTexture);
        CUT(sTexParameters);
        CUT(sTexImage2D);
        CUT(sCompressedTexImage2D);
        CUT(sTexSubImage2D);
        CUT(sGenerateMipmap);
        CUT(sCopyTexSubImage3D);
        CUT(sTexStorage2D);
        CUT(sCopyTexSubImage2D);
        CUT(sTexScaleDown);                 // texture-leak (2026-10-27): as sTexStorage2D + sCopyTexSubImage2D
        CUT(sPixelStorei);
        CUT(sTexConvertJoin);
        // --- Buffer contents, queries, uniform buffers
        CUT(sBufferData);
        CUT(sBufferSubData);
        CUT(sBufferWriteRanges);            // llrhiresource.h (research/137 N14): as sBufferSubData
        KEEP(sBufferHandle);                // a lookup
        CUT(sBeginQuery);
        CUT(sEndQuery);
        CUT(sGetQueryObjectuiv);
        CUT(sOcclusionBoxes);               // research/137 N8: occlusion boxes, each a query (llrhiocclusion.h)
        KEEP(sOcclusionBoxesReady);         // asks only
        CUT(sOcclusionResult);              // as sGetQueryObjectuiv
        CUT(sBindBufferBase);
        // --- Render state and targets
        CUT(sSetCap);
        CUT(sBlendFunc);
        CUT(sColorMask);
        CUT(sDepthFunc);
        CUT(sDepthMask);
        CUT(sLineWidth);
        CUT(sCullFace);
        CUT(sViewport);
        CUT(sScissor);
        CUT(sPolygonOffset);
        CUT(sPolygonMode);
        CUT(sClearColor);
        CUT(sBindFramebuffer);
        CUT(sBindTarget);                   // llrhipass.h (research/156 N12c): binds a target, as sBindFramebuffer
        CUT(sDrawBuffers);
        CUT(sDrawBuffer);
        CUT(sReadBuffer);
        CUT(sClear);
        // --- OIT
        CUT(sOITBegin);
        CUT(sOITWorld);
        KEEP(sOITVariant);                  // a lookup: does the program make OIT nodes (may make its variant; records nothing)
        CUT(sOITResolve);
        CUT(sOITSnapshotCheck);
        KEEP(sOITSupported);                // a lookup
        // --- GPU marks, windows, shaders, misc backend work
        CUT(sGPUMark);                      // a timestamp in the frame
        KEEP(sCPUMark);                     // a CPU segment only
        CUT(sPresentWindow);
        CUT(sForgetWindow);
        CUT(sParallelShaderReload);
        CUT(sPrewarmShaders);
        CUT(sBakeShaders);                  // research/159 3.1: --bake-shaders, as sParallelShaderReload
        CUT(sGPUBenchmark);
        // --- Full-screen passes (llrhiglare.h ... llrhiwetlook.h)
        CUT(sGlareDownsample);
        CUT(sGlareUpsample);
        CUT(sGlareSunVisibility);
        CUT(sGlareStreakExtract);
        CUT(sGlareStreak);
        CUT(sGlareComposite);
        CUT(sGtaoDepth);
        CUT(sGtaoOcclusion);
        CUT(sGtaoDenoise);
        CUT(sGtaoApply);
        CUT(sContactShadows);
        CUT(sGtaoApplyContact);
        CUT(sLookGrade);
        CUT(sUpscaleDraw);
        CUT(sTaaDraw);
        CUT(sSkinFillMask);
        CUT(sSkinFillShrink);
        CUT(sSkinFillSoften);
        CUT(sSkinFillOcclusion);
        CUT(sGroundFog);
        CUT(sFireflies);
        CUT(sSkinFillLight);
        CUT(sSkinSssDebug);
        CUT(sSsrHiZ);
        CUT(sSsrTrace);
        CUT(sSsrAvatarMask);
        CUT(sSsrResolve);
        CUT(sSsrFilter);
        CUT(sSsgiCapture);
        CUT(sSsgiReproject);
        CUT(sSsgiOcclusion);
        CUT(sSsgiDenoise);
        CUT(sSsgiDebugView);
        KEEP(sSsgiCompactLight);            // a lookup (the format's features, asked once)
        CUT(sVolumetricMarch);
        CUT(sVolumetricComposite);
        CUT(sVolumetricBlur);
        CUT(sWetLookCompute);
        CUT(sWetLookWriteBack);
        CUT(sRainStreaks);
        // --- Full-screen passes as units (llrhipass.h, research/137 N12)
        CUT(sPassBegin);                    // records the pass's barrier plan
        KEEP(sPassEnd);                     // counts only; records nothing
        // --- Reports and lookups: no state, nothing recorded
        KEEP(sObjectVersions);
        KEEP(sBackendCounters);
        KEEP(sMemoryReport);
        KEEP(sNoteEvent);
        KEEP(sResourceCounts);
        KEEP(sDumpAllocations);
        KEEP(sVRAMBudget);
        KEEP(sTextureImageBytes);
        KEEP(sPoolDetail);
        KEEP(sBufferCompactCounts);         // research/245: counts only
        KEEP(sTextureCompressed);
        KEEP(sTextureMoveHold);             // research/285: a flag on the texture; nothing recorded
        KEEP(sTexBCNReport);
        KEEP(sDrawRunPrograms);
        KEEP(sAvatarBackendReport);
        KEEP(sCensusTake);
        KEEP(sDescribeDrawTarget);
        // --- Set by the viewer for the backend, or the retired translation thread's (always null)
        KEEP(sPostWork);                    // runs a job on a cull thread
        KEEP(sVulkanUnavailableExit);       // ends the viewer
        KEEP(sRayTracingSource);            // asks the viewer once a frame what ray tracing should do (research/230 R2)
        KEEP(sTranslateFlush);              // research/207 T1: never set
        // --- The draw list's own (llrhidrawlist.h)
        KEEP(DrawList::sDrawList);          // the submission itself
        KEEP(DrawList::sDrawListReady);     // asks only
#undef CUT
#undef KEEP
    }
#undef LLRHI_DRAWLIST_CUT
#undef LLRHI_DRAWLIST_KEEP

    // fix-threads4: guard_hooks ran (installGuard, or the first open).
    std::atomic<bool> sGuarded{ false };

    bool same_attribute(const LLRHI::DirectDraw::Attribute& a, const LLRHI::DirectDraw::Attribute& b)
    {
        return a.location == b.location && a.size == b.size && a.type == b.type && a.normalized == b.normalized &&
               a.integer == b.integer && a.stride == b.stride && a.offset == b.offset && a.source == b.source &&
               a.source_offset == b.source_offset;
    }

    bool same_state(const State& a, const State& b)
    {
        return a.blend[0] == b.blend[0] && a.blend[1] == b.blend[1] && a.blend[2] == b.blend[2] && a.blend[3] == b.blend[3] &&
               a.cull == b.cull && a.first_patch == b.first_patch && a.patch_count == b.patch_count && a.stencil_ref == b.stencil_ref;
    }
}

// static
Builder& Builder::get()
{
    static Builder builder;
    return builder;
}

U32 LLRHI::DrawList::newMatrixId()
{
    static std::atomic<U32> sNext{ 0 };
    U32 id = sNext.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!id)
    {
        id = sNext.fetch_add(1, std::memory_order_relaxed) + 1;     // 0 means "no id"; after 4 billion, once
    }
    return id;
}

// ---------------------------------------------------------------------------
// Arrays

void Arrays::clear()
{
    mDraws.clear();
    mStates.clear();
    mAttributes.clear();
    mTextures.clear();
    mMatrices.clear();
    mPatches.clear();
    mFloats.clear();
    mStates.push_back(State());     // index 0: everything INHERIT
    mLastMatrices = NONE;
    mLastAttributes = NONE;
    mLastAttributeCount = 0;
    mLastState = 0;
}

U32 Arrays::addAttributes(const LLRHI::DirectDraw::Attribute* attributes, U32 count)
{
    if (mLastAttributes != NONE && count == mLastAttributeCount)
    {
        bool same = true;
        for (U32 i = 0; same && i < count; ++i)
        {
            same = same_attribute(mAttributes[mLastAttributes + i], attributes[i]);
        }
        if (same)
        {
            return mLastAttributes;
        }
    }
    mLastAttributes = (U32)mAttributes.size();
    mLastAttributeCount = count;
    mAttributes.insert(mAttributes.end(), attributes, attributes + count);
    return mLastAttributes;
}

U32 Arrays::addTextures(const U32* names, U32 count)
{
    const U32 first = (U32)mTextures.size();
    mTextures.insert(mTextures.end(), names, names + count);
    return first;
}

U32 Arrays::addMatrices(const Matrices& matrices)
{
    if (matrices.id && !mMatrices.empty() && mMatrices.back().id == matrices.id)
    {
        return (U32)mMatrices.size() - 1;
    }
    mMatrices.push_back(matrices);
    return (U32)mMatrices.size() - 1;
}

U32 Arrays::addFloats(const F32* values, U32 count)
{
    const U32 first = (U32)mFloats.size();
    mFloats.insert(mFloats.end(), values, values + count);
    return first;
}

U32 Arrays::addPatch(S32 slot, U32 count, U32 components, U32 rows, const F32* values)
{
    llassert(slot >= 0 && count && count <= 0xffff && components >= 1 && components <= 4 && rows <= 4);
    UniformPatch patch;
    patch.slot = slot;
    patch.count = (U16)llmin(count, 0xffffu);
    patch.components = (U8)llclamp(components, 1u, 4u);
    patch.rows = (U8)llmin(rows, 4u);
    patch.first_float = addFloats(values, (U32)patch.count * patch.components * llmax((U32)patch.rows, 1u));
    mPatches.push_back(patch);
    return (U32)mPatches.size() - 1;
}

U32 Arrays::addState(const State& state)
{
    if (mLastState < mStates.size() && same_state(mStates[mLastState], state))
    {
        return mLastState;
    }
    if (same_state(mStates[0], state))
    {
        return 0;
    }
    mLastState = (U32)mStates.size();
    mStates.push_back(state);
    return mLastState;
}

bool Arrays::addDraw(const Draw& draw, const Context& context)
{
    // Draw::matrices NONE: the previous draw's, the refused ones' included.
    const U32 matrices = draw.matrices == NONE ? mLastMatrices : draw.matrices;
    mLastMatrices = matrices;
    // What sDirectDraw would refuse goes the old way.
    bool takes = draw.count && draw.attribute_count &&
                 (!draw.index_buffer || draw.index_type == 0x1403 /* GL_UNSIGNED_SHORT */ || draw.index_type == 0x1405 /* GL_UNSIGNED_INT */);
    if (takes && draw.texture_count &&
        (draw.first_texture == NONE || (size_t)draw.first_texture + draw.texture_count > mTextures.size()))
    {
        llassert(false);    // the owner's indices are wrong
        takes = false;
    }
    if (takes && context.descriptor_units && draw.texture_count)
    {
        for (U32 unit = 0; takes && unit < draw.texture_count && unit < 32; ++unit)
        {
            takes = !(context.descriptor_units & (1u << unit)) ||
                    mTextures[draw.first_texture + unit] == LLRHI::DirectDraw::KEEP_UNIT;
        }
    }
    if (takes && draw.palette_base != NONE && !context.palette_rows)
    {
        takes = false;
    }
    if (!takes)
    {
        return false;
    }
    mDraws.push_back(draw);
    mDraws.back().matrices = matrices;      // resolved here, so it holds across a cut too
    return true;
}

List Arrays::list(const char* name, U64 stamp) const
{
    List list;
    list.name = name ? name : "";
    list.draws = mDraws.data();
    list.draw_count = (U32)mDraws.size();
    list.states = mStates.data();
    list.state_count = (U32)mStates.size();
    list.attributes = mAttributes.data();
    list.attribute_count = (U32)mAttributes.size();
    list.textures = mTextures.data();
    list.texture_count = (U32)mTextures.size();
    list.matrices = mMatrices.data();
    list.matrix_count = (U32)mMatrices.size();
    list.patches = mPatches.data();
    list.patch_count = (U32)mPatches.size();
    list.floats = mFloats.data();
    list.float_count = (U32)mFloats.size();
    list.stamp = stamp;
    return list;
}

void LLRHI::DrawList::installGuard()
{
    const bool any_kind = sKind[KIND_OPAQUE] || sKind[KIND_ALPHA] || sKind[KIND_MATERIAL];
    if (!sDrawList || !sDrawListReady || !sMode || !any_kind)
    {
        return;     // the first open wraps them, if lists are turned on later
    }
    guard_hooks();
    sGuarded.store(true, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Builder

bool Builder::open(const char* name, Kind kind, Sync sync, void* user)
{
    if (mOpen)
    {
        submit();   // lists do not nest (promise 2)
    }
    if (!wanted(kind) || !sDrawList || !sDrawListReady)
    {
        return false;
    }
    const std::thread::id self = std::this_thread::get_id();
    if (!sOwnerSet)
    {
        sOwner = self;
        sOwnerSet = true;
    }
    else if (sOwner != self)
    {
        return false;
    }
    if (!sGuarded.load(std::memory_order_acquire))
    {
        guard_hooks();      // fix-threads4: lists could not open at install (turned on later)
        sGuarded.store(true, std::memory_order_release);
    }
    Context context;
    if (!sDrawListReady(context))
    {
        ++sStats.not_ready;
        return false;
    }
    mArrays.clear();
    mOpen = true;
    ++mSerial;
    mNeedReady = false;
    mReady = true;
    mKind = kind;
    mName = name ? name : "";
    mSync = sync;
    mUser = user;
    mContext = context;
    tOpen = this;
    ++sStats.lists;
    return true;
}

bool Builder::askReady()
{
    if (mNeedReady)
    {
        // A hook ran since the readiness was given: the state may have changed.
        mNeedReady = false;
        mReady = sDrawListReady && sDrawListReady(mContext);
        if (!mReady)
        {
            ++sStats.not_ready;
        }
    }
    return mReady;
}

bool Builder::addDraw(const Draw& draw)
{
    if (!mOpen)
    {
        return false;
    }
    if (askReady() && mArrays.addDraw(draw, mContext))
    {
        return true;
    }
    // The old way: the draws so far go first, and the caches the old way
    // reads agree with the backend again (promise 4).
    ++sStats.old_path_draws;
    if (!mArrays.empty())
    {
        cutHere("old path");
    }
    return false;
}

bool Builder::addChunk(const Arrays& chunk)
{
    if (!mOpen)
    {
        return false;
    }
    if (!mArrays.empty())
    {
        cutHere("chunk");
    }
    if (!askReady())
    {
        return false;
    }
    if (chunk.empty())
    {
        return true;
    }
    Builder* const open = tOpen;
    tOpen = nullptr;    // hooks the backend or Sync calls do not cut
    struct Restore
    {
        Builder* list;
        ~Restore() { tOpen = list; }
    } restore{ open };
    const bool recorded = send(chunk.list(mName, mContext.stamp), "chunk");
    mNeedReady = true;  // Sync may change state
    if (recorded && mSync)
    {
        mSync(mUser);
    }
    return recorded;
}

bool Builder::send(const List& list, const char* why)
{
    const bool recorded = sDrawList && sDrawList(list);
    if (recorded)
    {
        ++sStats.submissions;
        sStats.draws += list.draw_count;
    }
    else
    {
        // sDrawListReady said yes and no hook ran since: a backend bug.
        sStats.refused += list.draw_count;
        LL_WARNS_ONCE("RHIDrawList") << "draw list '" << list.name << "' refused after the backend said it was ready ("
                                     << list.draw_count << " draws lost" << (why ? ", " : "") << (why ? why : "") << ")" << LL_ENDL;
    }
    return recorded;
}

void Builder::cutHere(const char* why)
{
    Builder* const open = tOpen;
    tOpen = nullptr;    // the hooks the backend or Sync calls do not cut again
    struct Restore
    {
        Builder* list;
        Arrays& arrays;
        ~Restore()
        {
            arrays.clearDraws();    // also when something throws: never submitted twice
            tOpen = list;
        }
    } restore{ open, mArrays };
    bool recorded = false;
    if (!mArrays.empty())
    {
        count_cut(why);
        recorded = send(mArrays.list(mName, mContext.stamp), why);
    }
    mNeedReady = true;
    if (recorded && mSync)
    {
        mSync(mUser);
    }
}

void Builder::submit()
{
    if (!mOpen)
    {
        return;
    }
    tOpen = nullptr;    // hooks the backend or Sync calls do not cut
    mOpen = false;
    const Sync sync = mSync;
    void* const user = mUser;
    mSync = nullptr;
    mUser = nullptr;
    bool recorded = false;
    if (!mArrays.empty())
    {
        struct Clear
        {
            Arrays& arrays;
            ~Clear() { arrays.clearDraws(); }
        } clear{ mArrays };
        recorded = send(mArrays.list(mName, mContext.stamp), nullptr);
    }
    if (recorded && sync)
    {
        sync(user);
    }
}

// static
void Builder::cut(const char* why)
{
    Builder* list = tOpen;
    if (list)
    {
        list->cutHere(why);
    }
}

void LLRHI::DrawList::takeStats(Stats& out)
{
    out = sStats;
    // The five hooks that cut most, most first.
    for (U32 k = 0; k < 5; ++k)
    {
        S32 best = -1;
        for (U32 i = 0; i < sCutKinds; ++i)
        {
            bool taken = false;
            for (U32 j = 0; j < k; ++j)
            {
                taken |= out.cut_names[j] == sCutCounts[i].name;
            }
            if (!taken && sCutCounts[i].count && (best < 0 || sCutCounts[i].count > sCutCounts[best].count))
            {
                best = (S32)i;
            }
        }
        if (best >= 0)
        {
            out.cut_names[k] = sCutCounts[best].name;
            out.cut_counts[k] = sCutCounts[best].count;
        }
    }
    sStats = Stats();
    sCutKinds = 0;
}

namespace
{
    OwnerReport sOwnerReports[KIND_COUNT] = {};
}

void LLRHI::DrawList::setOwnerReport(Kind kind, OwnerReport report)
{
    if (kind < KIND_COUNT)
    {
        sOwnerReports[kind] = report;
    }
}

std::string LLRHI::DrawList::report(U32 frames)
{
    frames = llmax(frames, 1u);
    Stats s;
    takeStats(s);
    std::string owners[KIND_COUNT];
    bool any_owner = false;
    for (U32 k = 0; k < KIND_COUNT; ++k)
    {
        if (sOwnerReports[k])
        {
            owners[k] = sOwnerReports[k](frames);
            any_owner |= !owners[k].empty();
        }
    }
    if (!sMode && !s.lists && !s.submissions && !any_owner)
    {
        return std::string();
    }
    const F64 per = 1.0 / frames;
    std::ostringstream out;
    out << std::fixed << std::setprecision(2);
    out << "viewer side (LLRHI_DRAW_LISTS=" << sMode << ", opaque " << sKind[KIND_OPAQUE] << " alpha " << sKind[KIND_ALPHA]
        << " material " << sKind[KIND_MATERIAL] << ", LLRHI_DRAW_LISTS_THREADS=" << sThreads << "): lists " << s.lists * per
        << ", submissions " << s.submissions * per << ", draws " << s.draws * per << ", old_path_draws " << s.old_path_draws * per << ", not ready " << s.not_ready * per
        << ", refused (lost) " << s.refused << ", cuts " << s.cuts * per << " [";
    for (U32 i = 0; i < 5 && s.cut_names[i]; ++i)
    {
        out << (i ? ", " : "") << s.cut_names[i] << " " << s.cut_counts[i] * per;
    }
    out << "]";
    static const char* const kind_names[KIND_COUNT] = { "opaque", "alpha", "material" };
    for (U32 k = 0; k < KIND_COUNT; ++k)
    {
        if (!owners[k].empty())
        {
            out << "; " << kind_names[k] << ": " << owners[k];
        }
    }
    return out.str();
}

/**
 * @file alphapass.cpp
 * @brief The alpha pool's glow pass without the batches whose faces have no
 *        glow, and the alpha pass's CPU counters (research/178 F0).
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

// What upstream does (LLDrawPoolAlpha::renderAlpha): after each alpha group's
// batches, every batch whose vertex buffer has an emissive attribute is drawn a
// second time with the emissive program, adding its glow to the alpha channel.
// A group's buffers all get the attribute when any one face of the group glows
// (LLVolumeGeometryManager::rebuildGeom, "include emissive byte to preserve
// batching"), so in a group with one glowing sign every alpha batch of the
// group is drawn twice, and the group's glow pass binds the emissive program
// and the batch program again. The second draw of a batch without glow adds 0
// (emissiveF.glsl: alpha = texture alpha * emissive alpha, the emissive alpha
// of its faces 0; blend ZERO, ONE for colour and ONE, ONE for alpha; no depth
// writes). With LLRHI::sAlphaGlowSkip (off unless LLRHI_ALPHA_GLOW_SKIP=1, cue
// alphaglowskip.toggle) those draws are left out, and a group's glow pass
// with nothing left is left out whole.
//
// Which batches have glow: registerFace notes each face's glow on its batch
// (noteFace); rebuildMesh, which rewrites faces in place and keeps the
// batches, marks a group with a glowing rewritten face unknown (noteMeshFace).
// Batches made elsewhere (particles, grass, other partitions) stay unknown and
// are drawn as upstream.
//
// LLRHI_ALPHA_PASS_STATS=1 (cue alphapassstats.toggle): an AlphaPass line every
// 5 s with the colour passes' batches, program binds, glow draws and CPU time,
// and the check (VERIFY) that no batch left out has a face with glow: for each
// one, the group's faces in the batch's vertex range are looked at
// ("glowing faces in left out batches", must be 0).

#include "llviewerprecompiledheaders.h"

#include "plus.h"

#include "alphapass.h"

#include "llappviewer.h"
#include "lldrawable.h"
#include "llface.h"
#include "llglslshader.h"
#include "llrhi.h"
#include "llspatialpartition.h"
#include "lltextureentry.h"
#include "llvertexbuffer.h"
#include "pipeline.h"

#include <iomanip>

namespace plus::alphapass
{
namespace
{
    constexpr F64 REPORT_SECONDS = 5.0;

    struct Stats
    {
        U64 frames = 0;
        U64 passes = 0;             // renderAlpha colour passes
        U64 pass_ns = 0;
        U64 batches = 0;            // the post-water pool's batches looked at for the glow pass
        U64 binds = 0;              // program changes in the loop
        U64 binds_slow = 0;         // of those, the full bindDeferredShader (first in the pool)
        U64 glow_wanted = 0;        // batches upstream draws again (emissive attribute)
        U64 glow_skipped = 0;       // of those, left out (no face glows)
        U64 glow_some = 0;          // drawn: a face glows
        U64 glow_unknown = 0;       // drawn: not known
        U64 glow_kept_oit = 0;      // drawn: OIT session
        U64 glow_kept_gltf = 0;     // drawn: GLTF material
        U64 glow_kept_off = 0;      // drawn: the switch off
        U64 glow_groups = 0;        // glow passes drawn
        U64 glow_groups_skipped = 0;// glow passes left out whole
        U64 glow_ns = 0;
        U64 verify_checked = 0;     // left out batches checked
        U64 verify_glowing = 0;     // glowing faces found in them (must be 0)
    };
    Stats sStats;
    bool sStatsWere = false;
    U32 sLastFrame = 0;
    F64 sLastReport = 0.0;
    bool sGroupWanted = false;      // the current group had a batch for the glow pass (upstream)

    bool glows(const LLTextureEntry* te)
    {
        return te && te->getGlow() > 0.f;
    }

    // VERIFY: the glowing faces of the group inside the batch's vertex range.
    U32 glowing_faces(LLSpatialGroup* group, const LLDrawInfo& draw)
    {
        U32 found = 0;
        for (LLSpatialGroup::element_iter it = group->getDataBegin(); it != group->getDataEnd(); ++it)
        {
            LLDrawable* drawable = (LLDrawable*)(*it)->getDrawable();
            if (!drawable || drawable->isDead())
            {
                continue;
            }
            for (S32 i = 0; i < drawable->getNumFaces(); ++i)
            {
                LLFace* face = drawable->getFace(i);
                if (!face || face->getVertexBuffer() != draw.mVertexBuffer.get() || !face->getGeomCount())
                {
                    continue;
                }
                const U32 first = face->getGeomIndex();
                const U32 last = first + face->getGeomCount() - 1;
                if (last < draw.mStart || first > draw.mEnd)
                {
                    continue;
                }
                if (glows(face->getTextureEntry()))
                {
                    ++found;
                }
            }
        }
        return found;
    }

    void report()
    {
        const F64 per = 1.0 / (F64)llmax(sStats.frames, (U64)1);
        LL_INFOS("AlphaPass") << std::fixed << std::setprecision(2)
            << "per frame over " << sStats.frames << " frames: colour passes " << sStats.passes * per
            << " in " << sStats.pass_ns * 1e-6 * per << " ms, post-water batches " << sStats.batches * per
            << ", program binds " << sStats.binds * per << " (full " << sStats.binds_slow * per << ")"
            << "; glow pass (LLRHI_ALPHA_GLOW_SKIP " << (LLRHI::sAlphaGlowSkip ? "on" : "off") << "): batches upstream draws again "
            << sStats.glow_wanted * per << ", left out " << sStats.glow_skipped * per << ", drawn [glows "
            << sStats.glow_some * per << ", unknown " << sStats.glow_unknown * per << ", oit " << sStats.glow_kept_oit * per
            << ", gltf " << sStats.glow_kept_gltf * per << ", switch off " << sStats.glow_kept_off * per << "], groups drawn "
            << sStats.glow_groups * per << " left out whole " << sStats.glow_groups_skipped * per << ", glow passes "
            << sStats.glow_ns * 1e-6 * per << " ms; verify: left out batches checked " << sStats.verify_checked * per
            << ", glowing faces in them " << sStats.verify_glowing << " (must be 0)" << LL_ENDL;
    }

    // Once a frame (the first colour pass): frames, and the line every 5 s.
    void frame_tick()
    {
        if (!LLRHI::sAlphaPassStats)
        {
            sStatsWere = false;
            return;
        }
        const F64 now = LLTimer::getTotalSeconds();
        if (!sStatsWere)
        {
            sStats = Stats();
            sStatsWere = true;
            sLastReport = now;
            sLastFrame = gFrameCount;
        }
        if (gFrameCount == sLastFrame)
        {
            return;
        }
        sLastFrame = gFrameCount;
        ++sStats.frames;
        if (now - sLastReport >= REPORT_SECONDS)
        {
            report();
            sStats = Stats();
            sLastReport = now;
        }
    }

    U64 elapsed_ns(std::chrono::steady_clock::time_point start)
    {
        return (U64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
    }
}

void noteFace(LLDrawInfo& info, const LLTextureEntry* te, bool new_info)
{
    if (new_info)
    {
        info.mRHIGlow = glows(te) ? GLOW_SOME : GLOW_NONE;
    }
    else if (info.mRHIGlow == GLOW_NONE && glows(te))
    {
        info.mRHIGlow = GLOW_SOME;
    }
}

void noteMeshFace(LLSpatialGroup* group, const LLTextureEntry* te, bool& marked)
{
    if (marked || !group || !glows(te))
    {
        return;
    }
    marked = true;
    for (U32 pass : { (U32)LLRenderPass::PASS_ALPHA, (U32)LLRenderPass::PASS_ALPHA_RIGGED })
    {
        auto found = group->mDrawMap.find(pass);
        if (found == group->mDrawMap.end())
        {
            continue;
        }
        for (auto& info : found->second)
        {
            info->mRHIGlow = GLOW_UNKNOWN;
        }
    }
}

bool glowPass(LLSpatialGroup* group, const LLDrawInfo& draw, bool oit, bool depth_only)
{
    const bool emissive = draw.mVertexBuffer->hasDataType(LLVertexBuffer::TYPE_EMISSIVE);
    if (depth_only)
    {
        return emissive;    // upstream's condition (a depth pass draws no glow)
    }
    const bool stats = LLRHI::sAlphaPassStats && sStatsWere;
    if (stats)
    {
        ++sStats.batches;
    }
    if (!emissive)
    {
        return false;
    }
    sGroupWanted = true;
    if (stats)
    {
        ++sStats.glow_wanted;
    }

    // Not while making avatar impostors: their alpha depth pass draws the
    // (non-rigged) glow pass with depth writes on and no alpha discard
    // (forwardRender's write_depth, renderEmissives), so there the draw is
    // not a no-op for the depth the impostor's mask is made from.
    const bool skip = LLRHI::sAlphaGlowSkip && !oit && !LLPipeline::sImpostorRender && draw.mGLTFMaterial.isNull() &&
                      draw.mRHIGlow == GLOW_NONE;
    if (stats)
    {
        if (skip)
        {
            ++sStats.glow_skipped;
            ++sStats.verify_checked;
            sStats.verify_glowing += glowing_faces(group, draw);
        }
        else if (!LLRHI::sAlphaGlowSkip)
        {
            ++sStats.glow_kept_off;
        }
        else if (oit)
        {
            ++sStats.glow_kept_oit;
        }
        else if (draw.mGLTFMaterial.notNull())
        {
            ++sStats.glow_kept_gltf;
        }
        else if (draw.mRHIGlow == GLOW_SOME)
        {
            ++sStats.glow_some;
        }
        else
        {
            ++sStats.glow_unknown;
        }
    }
    return !skip;
}

void noteBind(const LLGLSLShader& shader)
{
    if (LLRHI::sAlphaPassStats && sStatsWere)
    {
        ++sStats.binds;
        if (!shader.mCanBindFast)
        {
            ++sStats.binds_slow;
        }
    }
}

PassScope::PassScope(bool depth_only)
{
    if (depth_only)
    {
        return;
    }
    frame_tick();
    sGroupWanted = false;
    if (LLRHI::sAlphaPassStats && sStatsWere)
    {
        mTiming = true;
        ++sStats.passes;
        mStart = std::chrono::steady_clock::now();
    }
}

PassScope::~PassScope()
{
    if (mTiming)
    {
        sStats.pass_ns += elapsed_ns(mStart);
    }
}

GlowScope::GlowScope(bool drawing)
{
    const bool wanted = sGroupWanted;
    sGroupWanted = false;
    if (!LLRHI::sAlphaPassStats || !sStatsWere)
    {
        return;
    }
    if (drawing)
    {
        ++sStats.glow_groups;
        mTiming = true;
        mStart = std::chrono::steady_clock::now();
    }
    else if (wanted)
    {
        ++sStats.glow_groups_skipped;
    }
}

GlowScope::~GlowScope()
{
    if (mTiming)
    {
        sStats.glow_ns += elapsed_ns(mStart);
    }
}
}

/**
 * @file tecolor.cpp
 * @brief "TE color without a group rebuild" (see tecolor.h).
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

#include "llviewerprecompiledheaders.h"

#include "tecolor.h"

#include "lldrawable.h"
#include "llface.h"
#include "llframetimer.h"
#include "llrhi.h"
#include "llspatialpartition.h"
#include "lltextureentry.h"
#include "llvertexbuffer.h"
#include "llvovolume.h"
#include "pipeline.h"

#include <chrono>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>

namespace
{
    // Read once; the toggle files flip them while the viewer runs.
    bool sColorOn = LLRHI::envDefaultOn("LLRHI_COLOR_NOREBUILD");
    bool sAlphaOn = LLRHI::envDefaultOn("LLRHI_ALPHA_NOREBUILD");
    bool sStats = LLRHI::envSet("LLRHI_COLOR_STATS");

    // False only in the GPU benchmark process (plain GL).
    bool vulkan()
    {
        return LLRHI::sVulkanGL;
    }

    U64 now_ns()
    {
        return (U64)std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    enum Kind
    {
        KIND_RGB = 0,       // the alpha did not change
        KIND_ALPHA_SAME,    // the alpha changed within (0, 0.999): same transparency class
        KIND_ALPHA_CLASS,   // the alpha changed class (opaque, translucent, invisible)
        KIND_COUNT
    };

    // Strictly translucent: in the alpha pool (getPoolTypeFromTE: < 0.999),
    // never an alpha mask (canRenderAsMask needs 1.0), and drawn (rebuildGeom
    // drops alpha 0 faces without glow).
    bool translucent(F32 alpha)
    {
        return alpha > 0.f && alpha < 0.999f;
    }

    struct Counters
    {
        U32 changes[KIND_COUNT] = {};
        U32 fast[KIND_COUNT] = {};
        U32 not_eligible = 0;
        const char* last_not_eligible = "";

        U32 geom_color = 0;         // group rebuilds (GEOM_DIRTY) of groups holding a changed object
        U64 geom_color_ns = 0;
        U64 geom_color_bytes = 0;   // their vertex buffers' bytes (group->mGeometryBytes)
        U32 geom_all = 0;
        U64 geom_all_ns = 0;

        U32 mesh_color = 0;         // in-place rebuilds (MESH_DIRTY) that rewrote a changed object
        U32 mesh_color_only = 0;    //   of which only colors (every changed object in it)
        U64 mesh_color_ns = 0;
        U64 mesh_color_bytes = 0;   // bytes the changed objects' faces rewrote (estimate)
        U32 mesh_all = 0;
        U64 mesh_all_ns = 0;
    };

    std::mutex sMutex;              // the stats below (main thread in practice)
    Counters sCounters;
    // Objects whose color changed and whose group has not rebuilt them yet,
    // with the frame of the change. Never dereferenced: only compared with
    // the drawables of a group being rebuilt.
    std::unordered_map<const LLDrawable*, U32> sPending;
    F64 sReportStart = -1.0;
    U32 sReportFrame = 0;

    void report_locked(F64 now)
    {
        const U32 frame = LLFrameTimer::getFrameCount();
        if (sReportStart < 0.0)
        {
            sReportStart = now;
            sReportFrame = frame;
            return;
        }
        const F64 seconds = now - sReportStart;
        if (seconds < 10.0)
        {
            return;
        }
        const U32 frames = llmax(frame - sReportFrame, 1u);
        const Counters& c = sCounters;
        const U32 changes = c.changes[KIND_RGB] + c.changes[KIND_ALPHA_SAME] + c.changes[KIND_ALPHA_CLASS];
        const F64 color_ms = (F64)(c.geom_color_ns + c.mesh_color_ns) * 1e-6;
        LL_INFOS("RHIColor") << llformat(
            "TE color changes in %.1f s / %u frames: %u (RGB %u, done in place %u; alpha within its class %u, in place %u; "
            "alpha class change %u; not eligible %u, last: %s). "
            "Group rebuilds holding a changed object: %u, %.2f ms, %.1f KB of vertices; all group rebuilds %u, %.2f ms. "
            "In-place rebuilds of changed objects: %u (colors only %u), %.2f ms, %.1f KB; all in-place rebuilds %u, %.2f ms. "
            "Color changes cost %.3f ms a frame.",
            seconds, frames, changes,
            c.changes[KIND_RGB], c.fast[KIND_RGB], c.changes[KIND_ALPHA_SAME], c.fast[KIND_ALPHA_SAME],
            c.changes[KIND_ALPHA_CLASS], c.not_eligible, c.last_not_eligible,
            c.geom_color, (F64)c.geom_color_ns * 1e-6, (F64)c.geom_color_bytes / 1024.0,
            c.geom_all, (F64)c.geom_all_ns * 1e-6,
            c.mesh_color, c.mesh_color_only, (F64)c.mesh_color_ns * 1e-6, (F64)c.mesh_color_bytes / 1024.0,
            c.mesh_all, (F64)c.mesh_all_ns * 1e-6,
            color_ms / frames) << LL_ENDL;

        // Forget changes no rebuild picked up for a while (the group is out of
        // sight, or the object went away and its address may come back).
        for (auto it = sPending.begin(); it != sPending.end();)
        {
            it = frame - it->second > 3600 ? sPending.erase(it) : std::next(it);
        }
        sCounters = Counters();
        sReportStart = now;
        sReportFrame = frame;
    }

    // At most once a second (frame time): the toggle files and the report.
    void poll()
    {
        static F64 next_poll = 0.0;
        const F64 now = LLFrameTimer::getElapsedSeconds();
        if (now < next_poll)
        {
            return;
        }
        next_poll = now + 1.0;

        if (!LLRHI::cueDir().empty())
        {
            struct Toggle
            {
                const char* file;
                bool* flag;
                const char* label;
            };
            static const Toggle toggles[] =
            {
                { "colornorebuild.toggle", &sColorOn, "TE color without a group rebuild (research/81)" },
                { "alphanorebuild.toggle", &sAlphaOn, "TE alpha within its class without a group rebuild (research/81)" },
                { "colorstats.toggle",     &sStats,   "TE color change counters (research/81)" },
            };
            for (const Toggle& toggle : toggles)
            {
                if (!LLRHI::takeCue(toggle.file))
                {
                    continue;
                }
                *toggle.flag = !*toggle.flag;
                LL_INFOS("RHIProfile") << toggle.label << " " << (*toggle.flag ? "on" : "off") << LL_ENDL;
                if (toggle.flag == &sStats)
                {
                    std::lock_guard<std::mutex> lock(sMutex);
                    sCounters = Counters();
                    sPending.clear();
                    sReportStart = -1.0;
                }
            }
        }

        if (sStats)
        {
            std::lock_guard<std::mutex> lock(sMutex);
            report_locked(now);
        }
    }

    // Null when the in-place path can take this object's color change,
    // otherwise why not (for the counters).
    const char* not_eligible(LLVOVolume* vobj, U8 te)
    {
        LLDrawable* drawable = vobj->mDrawable;
        if (!drawable || drawable->isDead() || vobj->isDead())
        {
            return "no drawable";
        }
        if (vobj->isNoLOD())
        {
            return "no LOD yet";            // rebuildMesh skips it
        }
        if (vobj->isFlexible())
        {
            return "flexible";              // its own geometry update
        }
        if (vobj->mGLTFAsset)
        {
            return "glTF asset";            // not drawn from faces
        }
        if (drawable->getNumFaces() != vobj->getNumTEs() || te >= drawable->getNumFaces())
        {
            return "faces changing";
        }
        LLSpatialGroup* group = drawable->getSpatialGroup();
        if (!group || group->isDead())
        {
            return "no group";
        }
        if (!dynamic_cast<LLVolumeGeometryManager*>(group->getSpatialPartition()))
        {
            return "not a volume partition";
        }
        LLFace* face = drawable->getFace(te);
        LLVertexBuffer* buffer = face ? face->getVertexBuffer() : nullptr;
        if (!buffer || !buffer->hasDataType(LLVertexBuffer::TYPE_COLOR))
        {
            return "not built yet";         // the group's first build writes it
        }
        return nullptr;
    }
}

bool plus::tecolor::setTEColor(LLVOVolume* vobj, U8 te, const LLColor4& color, S32& retval)
{
    poll();

    const LLTextureEntry* tep = vobj ? vobj->getTE(te) : nullptr;
    if (!tep || (!sStats && !vulkan()))
    {
        return false;
    }

    const F32 old_alpha = tep->getColor().mV[VALPHA];
    const F32 new_alpha = color.mV[VALPHA];
    const Kind kind = new_alpha == old_alpha ? KIND_RGB :
                      translucent(old_alpha) && translucent(new_alpha) ? KIND_ALPHA_SAME : KIND_ALPHA_CLASS;
    // Without Vulkan (the GPU benchmark process): upstream's rebuild.
    const bool wanted = vulkan() && ((kind == KIND_RGB && sColorOn) || (kind == KIND_ALPHA_SAME && sAlphaOn));
    const char* why_not = wanted ? not_eligible(vobj, te) : nullptr;
    const bool in_place = wanted && !why_not;

    if (sStats)
    {
        std::lock_guard<std::mutex> lock(sMutex);
        ++sCounters.changes[kind];
        if (in_place)
        {
            ++sCounters.fast[kind];
        }
        else if (why_not)
        {
            ++sCounters.not_eligible;
            sCounters.last_not_eligible = why_not;
        }
        if (vobj->mDrawable.notNull())
        {
            sPending[vobj->mDrawable.get()] = LLFrameTimer::getFrameCount();
        }
    }

    if (!in_place)
    {
        return false;   // upstream: REBUILD_ALL, mColorChanged (and the alpha's LOD change)
    }

    // Unlike upstream, mColorChanged stays unset: an object whose color a
    // script keeps changing is not made active (taken out of the octree,
    // LLVOVolume::onDrawableUpdateFromServer), since rewriting the colors in
    // place costs little.
    retval = vobj->LLPrimitive::setTEColor(te, color);
    if (retval)
    {
        LLDrawable* drawable = vobj->mDrawable;
        // Upstream isolates objects whose TEs change on the fly in the octree;
        // keep it so the group (and the vertex buffer it rewrites) stays small.
        vobj->shrinkWrap();
        // Only the colors: rebuildMesh -> LLFace::getGeometryVolume writes the
        // color and glow bytes of the object's faces in place. The group goes
        // on the build queue (not only rebuilt once it is seen by the main
        // camera, as an in-place update would) so reflections and shadows of a
        // group out of sight get the color as soon as upstream's rebuild would.
        // (rebuildPriorityGroups rebuilds such a group at once: it is not
        // GEOM_DIRTY, so neither of its waits applies. If rebuildMesh finds
        // the buffers do not fit, it marks the group GEOM_DIRTY itself, as for
        // any in-place update; markRebuild skips a group already in the queue.)
        drawable->setState(LLDrawable::REBUILD_COLOR);
        LLSpatialGroup* group = drawable->getSpatialGroup();
        if (group)
        {
            group->dirtyMesh();
            gPipeline.markRebuild(group);
        }
    }
    return true;
}

plus::tecolor::BuildTimer::BuildTimer(LLSpatialGroup* group, bool mesh)
    : mMesh(mesh)
{
    poll();
    if (!sStats || !group)
    {
        return;
    }
    mGroup = group;

    std::lock_guard<std::mutex> lock(sMutex);
    if (!sPending.empty())
    {
        for (LLSpatialGroup::element_iter iter = group->getDataBegin(); iter != group->getDataEnd(); ++iter)
        {
            LLDrawable* drawable = (LLDrawable*)(*iter)->getDrawable();
            if (!drawable || sPending.find(drawable) == sPending.end())
            {
                continue;
            }
            if (mesh && !drawable->isState(LLDrawable::REBUILD_ALL))
            {
                continue;   // rebuildMesh leaves it alone
            }
            mColorDrawables.push_back(drawable);
            if (!mesh)
            {
                continue;   // rebuildGeom: the group's bytes, read afterwards
            }
            // The bytes getGeometryVolume rewrites: colors (and glow) only, or
            // whole vertices when something else is pending as well.
            const bool color_only = !drawable->isState(LLDrawable::REBUILD_VOLUME | LLDrawable::REBUILD_POSITION |
                                                       LLDrawable::REBUILD_TCOORD);
            mColorOnly = mColorOnly && color_only;
            for (S32 i = 0; i < drawable->getNumFaces(); ++i)
            {
                LLFace* face = drawable->getFace(i);
                LLVertexBuffer* buffer = face ? face->getVertexBuffer() : nullptr;
                if (!buffer)
                {
                    continue;
                }
                const U32 vertex_bytes = color_only ?
                    (buffer->hasDataType(LLVertexBuffer::TYPE_EMISSIVE) ? 8 : 4) :
                    LLVertexBuffer::calcVertexSize(buffer->getTypeMask());
                mBytes += (U64)face->getGeomCount() * vertex_bytes;
            }
        }
    }
    mStart = now_ns();
}

plus::tecolor::BuildTimer::~BuildTimer()
{
    if (!mGroup)
    {
        return;
    }
    const U64 ns = now_ns() - mStart;

    std::lock_guard<std::mutex> lock(sMutex);
    bool by_color = false;
    for (LLDrawable* drawable : mColorDrawables)
    {
        if (!drawable->isState(LLDrawable::REBUILD_ALL))    // rebuilt
        {
            sPending.erase(drawable);
            by_color = true;
        }
    }

    Counters& c = sCounters;
    if (mMesh)
    {
        ++c.mesh_all;
        c.mesh_all_ns += ns;
        if (by_color)
        {
            ++c.mesh_color;
            c.mesh_color_only += mColorOnly ? 1 : 0;
            c.mesh_color_ns += ns;
            c.mesh_color_bytes += mBytes;
        }
    }
    else
    {
        ++c.geom_all;
        c.geom_all_ns += ns;
        if (by_color)
        {
            ++c.geom_color;
            c.geom_color_ns += ns;
            c.geom_color_bytes += mGroup->mGeometryBytes;
        }
    }
}

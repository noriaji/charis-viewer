/**
 * @file shadowshift.cpp
 * @brief Moving sun shadows: a cached cascade moved by whole texels instead of
 *        drawn again whole (PlusShadowShift, research/151-shadow-shift-reuse.md
 *        in the sl-viewer repository).
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

#include "plus.h"

#include "shadowshift.h"

#include "llappviewer.h"
#include "llcamera.h"
#include "llgl.h"
#include "llglstates.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llrhi.h"
#include "llrhigl.h"
#include "llspatialpartition.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewershadermgr.h"
#include "pipeline.h"
#include "plus/shadowdist/shadowdist.h"

#include "glm/gtc/type_ptr.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <sstream>

extern bool gCubeSnapshot;  // llviewerdisplay.cpp: a reflection probe's or mirror's render

namespace plus::shadowshift
{
namespace
{
    // Settings (app_settings/settings_plus.xml). Not in the catalog until the
    // switch is on by default (research/151 S5).
    constexpr char SETTING_SHIFT[] = "PlusShadowShift";         // the move (and the depth range in steps, plus/shadowdist)
    constexpr char SETTING_VERIFY[] = "PlusShadowShiftVerify";  // each move checked against a whole redraw (heavy, not saved)
    constexpr char SETTING_WHOLE[] = "PlusShadowShiftWhole";    // the strips are the whole map: only the copies are tested (not saved)
    constexpr char SETTING_PARTIAL[] = "PlusShadowShiftPartial"; // S4(a): what changed where both windows look, redrawn there only
    constexpr char SETTING_FIRST_SEEN[] = "PlusShadowShiftFirstSeen"; // S4(b): a group first seen, built long ago, is settled at once
    constexpr char CUE_TOGGLE[] = "shadowshift.toggle";         // LLRHI_CUE_DIR: PlusShadowShift flipped, not saved (A/B in one start)
    constexpr F64 CUE_POLL_SECONDS = 1.0;

    constexpr F32 SAME_TURN = 0.9999f;          // the views' axes at least this close: the same turn step (steps are 22.5 degrees)
    constexpr F32 GRID_SLACK = 0.05f;           // texels: a cached centre this close to a whole texel is on the grid
    constexpr F32 SAME_SIZE = 1e-4f;            // relative: the same size step (steps are 9%; glm::ortho's r - l shakes the last bits with the centre)
    constexpr F32 LARGE_SHARE = 0.5f;           // strips over this share of the map: drawn whole instead
    constexpr F32 CLAMP_TOLERANCE = 1e-3f;      // relative: samplingProjection's finite differences shake the low bits
    constexpr F32 EDGE_TEXELS = 2.f;            // a differing texel this close to the old window's edge is "at the edge"
    constexpr U32 VERIFY_WAIT_FRAMES = 8;       // a moved cascade waits this long for its turn to be checked
    constexpr F64 REPORT_SECONDS = 5.0;
    constexpr F64 DEPTH_STEPS = 16777215.0;     // the shadow maps' 24-bit depth (llvkgl: X8_D24)
    constexpr U32 MAX_DIFFER = 128;             // partial redraw: more casters come or gone than this: drawn whole
    constexpr size_t MAX_DIRTY_RECTS = 8;       // partial redraw: the rects cleared and redrawn, at most

    const char* const WHY_NAMES[WHY_COUNT] = { "first", "sun",      "water", "turn",  "size",     "depth",
                                               "large", "content",  "sort",  "stable", "mismatch", "list",
                                               "clamp", "age",      "contents", "cull", "target",  "dirty",
                                               "projection" };

    bool shiftSetting()
    {
        static LLCachedControl<bool> shift(gSavedSettings, SETTING_SHIFT, false);
        return shift;
    }

    bool verifySetting()
    {
        static LLCachedControl<bool> verify(gSavedSettings, SETTING_VERIFY, false);
        return verify;
    }

    bool partialSetting()
    {
        static LLCachedControl<bool> partial(gSavedSettings, SETTING_PARTIAL, false);
        return partial;
    }

    bool firstSeenSetting()
    {
        static LLCachedControl<bool> first_seen(gSavedSettings, SETTING_FIRST_SEEN, false);
        return first_seen;
    }

    bool wholeSetting()
    {
        static LLCachedControl<bool> whole(gSavedSettings, SETTING_WHOLE, false);
        return whole;
    }

    // ------------------------------------------------------------------
    // Places in a cascade's grid

    // A rectangle in texels of a cascade's grid: the light view's x and y over
    // the texel's size, so whole numbers are texel corners.
    struct Rect
    {
        F32 x0 = 1.f, y0 = 1.f, x1 = -1.f, y1 = -1.f;     // empty

        bool empty() const { return x0 > x1 || y0 > y1; }
        bool overlaps(const Rect& r) const
        {
            return !empty() && !r.empty() && x0 <= r.x1 && r.x0 <= x1 && y0 <= r.y1 && r.y0 <= y1;
        }
        Rect grown(F32 d) const { return empty() ? *this : Rect{ x0 - d, y0 - d, x1 + d, y1 + d }; }
        Rect intersection(const Rect& r) const
        {
            return { llmax(x0, r.x0), llmax(y0, r.y0), llmin(x1, r.x1), llmin(y1, r.y1) };
        }
    };
    const Rect EVERYWHERE = { -FLT_MAX, -FLT_MAX, FLT_MAX, FLT_MAX };

    // A cascade's window on its grid: the centre (a texel corner: the maps'
    // sides are even) and the sides, in texels.
    struct Window
    {
        S32 cx = 0, cy = 0;
        S32 w = 0, h = 0;

        Rect rect() const
        {
            return { (F32)(cx - w / 2), (F32)(cy - h / 2), (F32)(cx + w / 2), (F32)(cy + h / 2) };
        }
    };

    // Texels [x0, x1) x [y0, y1) of a map (GL window coordinates: llvkgl
    // passes viewports and scissors as they are, so they are the image's too).
    struct PixelRect
    {
        S32 x0 = 0, y0 = 0, x1 = 0, y1 = 0;

        U64 area() const { return (U64)(x1 - x0) * (U64)(y1 - y0); }
        bool touches(const PixelRect& r) const { return x0 <= r.x1 && r.x0 <= x1 && y0 <= r.y1 && r.y0 <= y1; }
        PixelRect joined(const PixelRect& r) const
        {
            return { llmin(x0, r.x0), llmin(y0, r.y0), llmax(x1, r.x1), llmax(y1, r.y1) };
        }
    };

    // A static caster as the cached depth holds it: its hash (the sort's) and
    // where its batch's box lies on the grid.
    struct Item
    {
        U64 hash;
        Rect rect;
    };

    // Agent (or a batch's model) space to the grid's texels along x and y.
    class GridRows
    {
    public:
        void set(const glm::mat4& view, const glm::mat4& proj, U32 width, U32 height)
        {
            // An orthographic cascade: NDC x = proj[0][0] x_view + proj[3][0], a texel 2 / width of it.
            const F32 sx = proj[0][0] * 0.5f * (F32)width;
            const F32 sy = proj[1][1] * 0.5f * (F32)height;
            mX = glm::vec4(view[0][0], view[1][0], view[2][0], view[3][0]) * sx;
            mY = glm::vec4(view[0][1], view[1][1], view[2][1], view[3][1]) * sy;
            mModelSet = false;
        }

        // The batch's box on the grid; everywhere when it has none.
        Rect rect(const LLDrawInfo* info)
        {
            if (!info->mHasExtents)
            {
                return EVERYWHERE;
            }
            if (!mModelSet || info->mModelMatrix != mModel)
            {
                // Batches come in runs of the same region's matrix.
                mModel = info->mModelMatrix;
                mModelSet = true;
                if (mModel)
                {
                    const glm::mat4 model = glm::make_mat4((const F32*)mModel->mMatrix);
                    mModelX = mX * model;
                    mModelY = mY * model;
                }
                else
                {
                    mModelX = mX;
                    mModelY = mY;
                }
            }
            const F32* lo = info->mExtents[0].getF32ptr();
            const F32* hi = info->mExtents[1].getF32ptr();
            F32 c[3], e[3];
            for (U32 i = 0; i < 3; ++i)
            {
                c[i] = 0.5f * (lo[i] + hi[i]);
                e[i] = 0.5f * (hi[i] - lo[i]);
            }
            const F32 x = mModelX.x * c[0] + mModelX.y * c[1] + mModelX.z * c[2] + mModelX.w;
            const F32 y = mModelY.x * c[0] + mModelY.y * c[1] + mModelY.z * c[2] + mModelY.w;
            const F32 ex = std::fabs(mModelX.x) * e[0] + std::fabs(mModelX.y) * e[1] + std::fabs(mModelX.z) * e[2];
            const F32 ey = std::fabs(mModelY.x) * e[0] + std::fabs(mModelY.y) * e[1] + std::fabs(mModelY.z) * e[2];
            const Rect r = { x - ex, y - ey, x + ex, y + ey };
            return std::isfinite(r.x0) && std::isfinite(r.y0) && std::isfinite(r.x1) && std::isfinite(r.y1) ? r : EVERYWHERE;
        }

    private:
        glm::vec4 mX{ 0.f }, mY{ 0.f };
        const LLMatrix4* mModel = nullptr;
        bool mModelSet = false;
        glm::vec4 mModelX{ 0.f }, mModelY{ 0.f };
    };

    // ------------------------------------------------------------------
    // State

    struct Cascade
    {
        // This frame's move (accept), from the cached window to the new one.
        U32 moved_frame = U32_MAX;
        Window from, to;
        // Why it was not moved this frame: refused (accept, noteReject), or
        // moved and then drawn whole (drawShift).
        U32 why_frame = U32_MAX;
        Why why = WHY_FIRST;
        U32 failed_frame = U32_MAX;
        Why failed = WHY_FIRST;
        // What the cached depth holds: its static casters on its grid, as of
        // its last whole redraw or move (content), and the caster's bias clamp
        // it was drawn with. next: being listed.
        bool content_valid = false;
        glm::mat4 content_view{ 1.f };
        U32 content_width = 0, content_height = 0;
        F32 clamp = 0.f;
        std::vector<Item> content, next;
        // next's batches, this frame only (a move): content's are never followed.
        struct Draw
        {
            U32 type;
            LLDrawInfo* info;
        };
        std::vector<Draw> next_draws;
        std::unique_ptr<LLCullResult> strips;      // the strips' casters (kept from frame to frame)
    };
    Cascade sCascades[4];

    Cascade* cascadeAt(S32 index)
    {
        return index >= 0 && index < 4 ? &sCascades[index] : nullptr;
    }

    // The cascade whose casters noteCached or noteContent take now.
    struct Noting
    {
        Cascade* cascade = nullptr;
        S32 index = -1;
        GridRows rows;
        // A move: what both windows show (grown by a texel: casters on its
        // edge count), and the strips that came into view (grown likewise).
        Rect shared;
        Rect strip[2];
        bool whole = false;
        U64 shared_now = 0;
        U32 cached_infos = 0, strip_infos = 0;
        U32 dirty_infos = 0;                // drawn for a partial redraw's rects only
        LLCullResult* verify = nullptr;     // every cached caster too, for the check
    };
    Noting sNoting;

    // A partial redraw's scratch (PlusShadowShiftPartial; kept from frame to frame).
    struct Partial
    {
        std::vector<std::pair<U64, U32>> before, now;   // (hash, item) where both windows look: content's, next's
        std::vector<PixelRect> rects;                   // cleared and redrawn (the new map's texels)
        std::vector<Rect> touch;                        // the rects on the grid grown by a texel: a caster touching one is drawn again
    };
    Partial sPartial;

    // PlusShadowShiftVerify: one moved cascade a frame drawn whole as well, and
    // read back with the cached depth at the next frame's start.
    struct Verify
    {
        LLRenderTarget* target = nullptr;   // not static: let go of in release(), with GL alive
        std::unique_ptr<LLCullResult> casters;
        bool pending = false;
        U32 frame = 0;
        S32 index = -1;
        Window from, to;
        U32 strip_infos = 0, cached_infos = 0;
        std::vector<PixelRect> dirty;       // a partial redraw's rects
        S32 turn = 0;
        U32 waited = 0;
        std::vector<F32> cached, whole;
    };
    Verify sVerify;

    struct Stats
    {
        U32 frames = 0;
        U32 cascades = 0;
        U32 moved = 0;
        U64 moved_texels = 0;
        U64 strip_infos = 0;
        U64 cached_infos = 0;
        U32 whole[WHY_COUNT] = {};
        U64 first_seen = 0;     // groups the sort counted as settled at first sight (S4(b))
        U32 partial = 0;        // moves with a partial redraw (S4(a))
        U64 partial_rects = 0;
        F64 partial_share = 0.0;
        U64 partial_infos = 0;
    };
    Stats sStats;
    std::chrono::steady_clock::time_point sReportStart;
    U32 sLastFrame = U32_MAX;
    bool sHolding = false;      // something to let go of once PlusShadowShift is off
    // PlusShadowShift as the frame's first cascade found it: every hook of a
    // frame agrees, even when the cue (beginSort) or the settings change it
    // half way (a cascade moved by accept is drawn moved, not reused as it was).
    bool sActive = false;

    // ------------------------------------------------------------------
    // Drawing

    // Held while a copy draws through a moved viewport; back to the target's.
    class ViewportScope
    {
    public:
        ViewportScope(S32 x, S32 y, S32 width, S32 height) { LLRHI::viewport(x, y, width, height); }
        ~ViewportScope() { LLRHI::viewport(gGLViewport[0], gGLViewport[1], gGLViewport[2], gGLViewport[3]); }

        ViewportScope(const ViewportScope&) = delete;
        ViewportScope& operator=(const ViewportScope&) = delete;
    };

    // Held while the GPU shadow list's resident groups for a pass are set.
    class ListPassScope
    {
    public:
        ListPassScope(bool volume, U32 since_max) : mSaved(gPipeline.mRHIListPass)
        {
            LLPipeline::RHIListPass pass;
            pass.volume = volume;
            pass.since_max = since_max;
            gPipeline.mRHIListPass = pass;
        }
        ~ListPassScope() { gPipeline.mRHIListPass = mSaved; }

        ListPassScope(const ListPassScope&) = delete;
        ListPassScope& operator=(const ListPassScope&) = delete;

    private:
        LLPipeline::RHIListPass mSaved;
    };

    // Held while accept() makes a camera's cull planes from its matrices.
    class MatrixScope
    {
    public:
        MatrixScope() : mView(get_current_modelview()), mProj(get_current_projection()) {}
        ~MatrixScope()
        {
            set_current_modelview(mView);
            set_current_projection(mProj);
        }

        MatrixScope(const MatrixScope&) = delete;
        MatrixScope& operator=(const MatrixScope&) = delete;

    private:
        glm::mat4 mView, mProj;
    };

    // Held while a texture's filter is put back: the anisotropy level the
    // shadow maps were made with (LLPipeline::allocateShadowBuffer, which the
    // receiver cull assumes: research/64), not a level changed since.
    class AnisotropyLevelScope
    {
    public:
        explicit AnisotropyLevelScope(F32 level) : mSaved(LLRender::sAnisotropicFilteringLevel)
        {
            LLRender::sAnisotropicFilteringLevel = level;
        }
        ~AnisotropyLevelScope() { LLRender::sAnisotropicFilteringLevel = mSaved; }

        AnisotropyLevelScope(const AnisotropyLevelScope&) = delete;
        AnisotropyLevelScope& operator=(const AnisotropyLevelScope&) = delete;

    private:
        F32 mSaved;
    };

    // The bound target's depth = source's depth texel (x + dx, y + dy), where
    // the source has one (the rest stays as it is). A copy through a viewport
    // whose origin is (-dx, -dy): copyV.glsl's texture coordinate follows the
    // viewport, so every texel moves by whole texels and keeps its bits.
    // source_filter: the source's filter. copyDepthOnlyF.glsl reads with texture(), so a
    // source with a linear filter (a sun shadow map: TFO_ANISOTROPIC) is read
    // with TFO_POINT for the copy and put back after: no neighbour's depth mixes
    // in where the texture coordinate's rounding misses the texel's centre.
    void copyDepth(LLRenderTarget& source, S32 dx, S32 dy, U32 width, U32 height, LLTexUnit::eTextureFilterOptions source_filter)
    {
        LLGLDepthTest depth(GL_TRUE, GL_TRUE, GL_ALWAYS);
        LLGLDisable cull(GL_CULL_FACE);
        ViewportScope viewport(-dx, -dy, (S32)width, (S32)height);
        gShadowCopyDepthProgram.bind();    // depth only: the shadow maps have no colour attachment (fragcolor-depth)
        const S32 channel = gShadowCopyDepthProgram.getTextureChannel(LLShaderMgr::DEFERRED_DEPTH);
        LLTexUnit* unit = gGL.getTexUnit(channel);
        unit->bind(&source, true);
        const bool point = source_filter != LLTexUnit::TFO_POINT;
        if (point)
        {
            unit->setTextureFilteringOption(LLTexUnit::TFO_POINT);
        }
        gPipeline.mScreenTriangleVB->setBuffer();
        gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
        if (point)
        {
            unit->activate();
            AnisotropyLevelScope level(gPipeline.mRHISunShadowAniso);
            unit->setTextureFilteringOption(source_filter);
        }
        unit->unbind(LLTexUnit::TT_TEXTURE);
        gShadowCopyDepthProgram.unbind();
    }

    // Static casters into the bound target, as a whole redraw of the cached
    // depth draws them (renderCachedShadowCascade): the GPU list's resident
    // groups up to list_serial with them.
    void drawCasters(LLCullResult& casters, const glm::mat4& view, const glm::mat4& proj, LLCamera& camera, U32 list_serial)
    {
        gPipeline.grabReferences(casters);
        ListPassScope list((gPipeline.mRHIListKinds & 1) != 0, list_serial);
        gPipeline.renderShadowCasters(view, proj, camera, true, false);
    }

    // ------------------------------------------------------------------
    // The partial redraw (PlusShadowShiftPartial, research/158 section 2)

    bool touchesAny(const std::vector<Rect>& rects, const Rect& r)
    {
        for (const Rect& t : rects)
        {
            if (t.overlaps(r))
            {
                return true;
            }
        }
        return false;
    }

    bool isEverywhere(const Rect& r)
    {
        return r.x0 == -FLT_MAX || r.y0 == -FLT_MAX || r.x1 == FLT_MAX || r.y1 == FLT_MAX;
    }

    // A move whose shared casters differ from the cached depth's: the rects to
    // clear and draw again (sPartial.rects; sPartial.touch picks the casters to
    // draw), from the casters gone and come where both windows look.
    // WHY_COUNT: done; otherwise why it is drawn whole instead.
    Why findDirty(const Cascade& c, const Noting& n, F32 strip_texels)
    {
        Partial& p = sPartial;
        p.before.clear();
        p.now.clear();
        p.rects.clear();
        p.touch.clear();
        for (U32 i = 0; i < (U32)c.content.size(); ++i)
        {
            if (c.content[i].rect.overlaps(n.shared))
            {
                p.before.emplace_back(c.content[i].hash, i);
            }
        }
        for (U32 i = 0; i < (U32)c.next.size(); ++i)
        {
            if (c.next[i].rect.overlaps(n.shared))
            {
                p.now.emplace_back(c.next[i].hash, i);
            }
        }
        std::sort(p.before.begin(), p.before.end());
        std::sort(p.now.begin(), p.now.end());

        // A caster gone or come: its box (grown by a texel for the raster's edge
        // rule) in the new window, out to whole texels.
        const Window& to = c.to;
        const F32 left = (F32)(to.cx - to.w / 2), bottom = (F32)(to.cy - to.h / 2);
        const Rect window = to.rect();
        U32 differ = 0;
        auto add = [&](const Rect& r)
        {
            if (++differ > MAX_DIFFER || isEverywhere(r))
            {
                return false;
            }
            const Rect g = r.grown(1.f).intersection(window);
            if (g.empty())
            {
                return true;
            }
            PixelRect px;
            px.x0 = llclamp((S32)std::floor(g.x0 - left), 0, to.w);
            px.y0 = llclamp((S32)std::floor(g.y0 - bottom), 0, to.h);
            px.x1 = llclamp((S32)std::ceil(g.x1 - left), 0, to.w);
            px.y1 = llclamp((S32)std::ceil(g.y1 - bottom), 0, to.h);
            if (px.x1 > px.x0 && px.y1 > px.y0)
            {
                p.rects.push_back(px);
            }
            return true;
        };
        size_t a = 0, b = 0;
        while (a < p.before.size() || b < p.now.size())
        {
            if (b == p.now.size() || (a < p.before.size() && p.before[a].first < p.now[b].first))
            {
                if (!add(c.content[p.before[a].second].rect))     // gone
                {
                    return WHY_MISMATCH;
                }
                ++a;
            }
            else if (a == p.before.size() || p.now[b].first < p.before[a].first)
            {
                if (!add(c.next[p.now[b].second].rect))           // come
                {
                    return WHY_MISMATCH;
                }
                ++b;
            }
            else
            {
                ++a;
                ++b;
            }
        }

        // Rects that touch become one; then the two whose union grows the least,
        // down to MAX_DIRTY_RECTS.
        std::vector<PixelRect>& rects = p.rects;
        for (bool merged = true; merged;)
        {
            merged = false;
            for (size_t i = 0; i < rects.size() && !merged; ++i)
            {
                for (size_t j = i + 1; j < rects.size(); ++j)
                {
                    if (rects[i].touches(rects[j]))
                    {
                        rects[i] = rects[i].joined(rects[j]);
                        rects[j] = rects.back();
                        rects.pop_back();
                        merged = true;
                        break;
                    }
                }
            }
        }
        while (rects.size() > MAX_DIRTY_RECTS)
        {
            size_t best_i = 0, best_j = 1;
            S64 best = std::numeric_limits<S64>::max();
            for (size_t i = 0; i < rects.size(); ++i)
            {
                for (size_t j = i + 1; j < rects.size(); ++j)
                {
                    // Below 0 when the two overlap (an earlier join here can make them).
                    const S64 grow = (S64)rects[i].joined(rects[j]).area() - (S64)rects[i].area() - (S64)rects[j].area();
                    if (grow < best)
                    {
                        best = grow;
                        best_i = i;
                        best_j = j;
                    }
                }
            }
            rects[best_i] = rects[best_i].joined(rects[best_j]);
            rects[best_j] = rects.back();
            rects.pop_back();
        }
        U64 area = 0;
        for (const PixelRect& r : rects)
        {
            area += r.area();
        }
        if ((F32)area + strip_texels > LARGE_SHARE * (F32)to.w * (F32)to.h)
        {
            return WHY_DIRTY;
        }
        // The casters to draw again: any touching a final rect (grown by a texel) on the grid.
        for (const PixelRect& r : rects)
        {
            p.touch.push_back(Rect{ (F32)r.x0 + left, (F32)r.y0 + bottom, (F32)r.x1 + left, (F32)r.y1 + bottom }.grown(1.f));
        }
        return WHY_COUNT;
    }

    // The bound target's depth back to far (the clear depth) in each rect. The
    // scissor is on for these clears only, and left as the whole target.
    void clearRects(LLRenderTarget& target, const std::vector<PixelRect>& rects, U32 width, U32 height)
    {
        if (rects.empty())
        {
            return;
        }
        LLGLDepthTest depth(GL_TRUE, GL_TRUE, GL_ALWAYS);   // a clear writes depth only while depth writes are on
        LLGLEnable scissor(GL_SCISSOR_TEST);
        for (const PixelRect& r : rects)
        {
            LLRHI::scissor(r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0);
            target.clear(GL_DEPTH_BUFFER_BIT);
        }
        LLRHI::scissor(0, 0, (S32)width, (S32)height);
    }

    // ------------------------------------------------------------------
    // The check (PlusShadowShiftVerify)

    void readDepth(LLRenderTarget& target, std::vector<F32>& out)
    {
        out.resize((size_t)target.getWidth() * target.getHeight());
        target.bindTarget();
        glReadPixels(0, 0, (GLsizei)target.getWidth(), (GLsizei)target.getHeight(), GL_DEPTH_COMPONENT, GL_FLOAT, out.data());
        target.flush();
    }

    void releaseVerify()
    {
        Verify& v = sVerify;
        if (v.target)
        {
            v.target->release();
            delete v.target;
            v.target = nullptr;
        }
        v.casters.reset();
        v.pending = false;
        std::vector<F32>().swap(v.cached);
        std::vector<F32>().swap(v.whole);
        std::vector<PixelRect>().swap(v.dirty);
    }

    // The last frame's checked cascade: its cached depth (moved) against the
    // same projection drawn whole. Every texel should be the same, but for the
    // odd one where a caster's edge falls on a texel's centre (the moved
    // projection's last bits) and the depth's last step (research/151 3.2).
    void checkVerified()
    {
        Verify& v = sVerify;
        if (!v.pending || v.frame == gFrameCount)
        {
            return;
        }
        v.pending = false;
        const U32 width = (U32)v.to.w, height = (U32)v.to.h;
        LLRenderTarget& cache = gPipeline.mShadowStaticCache[v.index];
        if (!v.target || cache.getWidth() != width || cache.getHeight() != height || v.target->getWidth() != width ||
            v.target->getHeight() != height)
        {
            LL_INFOS("RHIShadow") << "shift verify cascade " << v.index << ": not checked (the maps were made again)" << LL_ENDL;
            return;
        }
        readDepth(cache, v.cached);
        readDepth(*v.target, v.whole);

        // The old window's edges in the new map's texels (x: columns, y: rows).
        const S32 dx = v.to.cx - v.from.cx, dy = v.to.cy - v.from.cy;
        const F32 edge_x = dx > 0 ? (F32)((S32)width - dx) : (F32)(-dx);
        const F32 edge_y = dy > 0 ? (F32)((S32)height - dy) : (F32)(-dy);
        U32 differ = 0, over_step = 0, at_edge = 0, by_rect = 0;
        F64 largest = 0.0;
        for (U32 y = 0; y < height; ++y)
        {
            const size_t row = (size_t)y * width;
            for (U32 x = 0; x < width; ++x)
            {
                const F32 a = v.cached[row + x], b = v.whole[row + x];
                if (a == b)
                {
                    continue;
                }
                ++differ;
                const F64 steps = std::fabs((F64)a - (F64)b) * DEPTH_STEPS;
                largest = llmax(largest, steps);
                over_step += steps > 1.5 ? 1 : 0;
                const bool near_x = dx != 0 && std::fabs((F32)x + 0.5f - edge_x) <= EDGE_TEXELS;
                const bool near_y = dy != 0 && std::fabs((F32)y + 0.5f - edge_y) <= EDGE_TEXELS;
                at_edge += near_x || near_y ? 1 : 0;
                for (const PixelRect& r : v.dirty)
                {
                    const F32 cx = (F32)x + 0.5f, cy = (F32)y + 0.5f;
                    if (cx >= (F32)r.x0 - EDGE_TEXELS && cx <= (F32)r.x1 + EDGE_TEXELS && cy >= (F32)r.y0 - EDGE_TEXELS &&
                        cy <= (F32)r.y1 + EDGE_TEXELS)
                    {
                        ++by_rect;
                        break;
                    }
                }
            }
        }
        LL_INFOS("RHIShadow") << "shift verify cascade " << v.index << ": " << differ << " of " << (size_t)width * height
                              << " texels differ (max " << largest << " depth steps; over one step " << over_step
                              << ", at the old window's edge " << at_edge << "); moved " << dx << ", " << dy << " texels, strip batches "
                              << v.strip_infos << " of " << v.cached_infos << (wholeSetting() ? " (strips: the whole map)" : "")
                              << "; redrawn rects " << v.dirty.size() << " (texels differing in or by one " << by_rect << ")"
                              << LL_ENDL;
    }

    // Draws cascade index's static casters whole (sVerify.casters) with the
    // moved projection, for checkVerified() at the next frame's start.
    void drawVerify(S32 index, const Cascade& c, const glm::mat4& view, const glm::mat4& proj, LLCamera& camera, U32 list_serial)
    {
        Verify& v = sVerify;
        const U32 width = (U32)c.to.w, height = (U32)c.to.h;
        if (!v.target)
        {
            v.target = new LLRenderTarget();
        }
        if (v.target->getWidth() != width || v.target->getHeight() != height)
        {
            v.target->release();
            if (!v.target->allocate(width, height, 0, true))
            {
                return;
            }
        }
        v.target->bindTarget();
        v.target->getViewport(gGLViewport);
        v.target->clear();
        drawCasters(*v.casters, view, proj, camera, list_serial);
        v.target->flush();

        v.pending = true;
        v.frame = gFrameCount;
        v.index = index;
        v.from = c.from;
        v.to = c.to;
        v.strip_infos = sNoting.strip_infos;
        v.cached_infos = sNoting.cached_infos;
        v.dirty = sPartial.rects;
        v.turn = (index + 1) & 3;
        v.waited = 0;
    }

    // ------------------------------------------------------------------
    // The VKPerf line and letting go

    void report()
    {
        using Clock = std::chrono::steady_clock;
        const Clock::time_point now = Clock::now();
        if (sReportStart == Clock::time_point())
        {
            sReportStart = now;
            return;
        }
        if (std::chrono::duration<F64>(now - sReportStart).count() < REPORT_SECONDS)
        {
            return;
        }
        // Written in development starts (LLRHI::sPeriodicLogs) or while checking.
        if ((LLRHI::sPeriodicLogs || verifySetting()) && sStats.cascades)
        {
            U32 whole = 0;
            for (U32 why : sStats.whole)
            {
                whole += why;
            }
            const U32 moved = sStats.moved;
            const F64 per_move = moved ? 1.0 / moved : 0.0;
            std::ostringstream line;
            line << "Shadow shift: " << sStats.frames << " frames, " << sStats.cascades << " cascades: moved " << moved
                 << " (" << sStats.moved_texels * per_move << " texels, strip batches " << sStats.strip_infos * per_move << " of "
                 << sStats.cached_infos * per_move << " cached a move), drawn whole " << whole << " (";
            for (U32 why = 0; why < WHY_PROJECTION; ++why)
            {
                line << (why ? ", " : "") << WHY_NAMES[why] << " " << sStats.whole[why];
            }
            line << "), reused " << (sStats.cascades > moved + whole ? sStats.cascades - moved - whole : 0);
            // research/158: added at the end (J8).
            const U32 partial = sStats.partial;
            const F64 per_partial = partial ? 1.0 / partial : 0.0;
            line << ", partial " << partial << " (" << sStats.partial_rects * per_partial << " rects, "
                 << 100.0 * sStats.partial_share * per_partial << "% of the map, " << sStats.partial_infos * per_partial
                 << " batches a partial), first seen " << sStats.first_seen << " groups";
            LL_INFOS("VKPerf") << line.str() << LL_ENDL;
        }
        sStats = Stats();
        sReportStart = now;
    }

    void letGo()
    {
        for (Cascade& c : sCascades)
        {
            c.moved_frame = U32_MAX;
            c.content_valid = false;
            std::vector<Item>().swap(c.content);
            std::vector<Item>().swap(c.next);
            std::vector<Cascade::Draw>().swap(c.next_draws);
            c.strips.reset();
        }
        sNoting = Noting();
        sPartial = Partial();
        releaseVerify();
        sStats = Stats();
        sReportStart = std::chrono::steady_clock::time_point();
        sHolding = false;
    }

    void newFrame()
    {
        const bool was_active = sActive;
        sActive = shiftSetting();
        if (sActive != was_active)
        {
            // The switch changed the alpha casters' dither (ditherWidth: width or
            // width + 1): cached depth drawn with the other one would mix the two
            // until each cascade's 2 s refresh. Every main view cascade is drawn
            // whole this frame (reuseShadowCascade refuses an invalid entry). The
            // probes' caches keep theirs: their dither never changes (gCubeSnapshot).
            for (LLPipeline::ShadowCacheEntry& entry : gPipeline.mShadowCacheEntry)
            {
                entry.valid = false;
            }
        }
        if (!sActive)
        {
            if (sHolding)
            {
                letGo();
            }
            return;
        }
        sHolding = true;
        if (verifySetting())
        {
            checkVerified();
            ++sVerify.waited;
        }
        else if (sVerify.target || sVerify.casters)
        {
            releaseVerify();
        }
        ++sStats.frames;
        report();
    }

    // The cue's flip; its own function so the log line reads "...::poll : ..."
    // (what tools/native-ab.ps1 takes as the toggle's answer).
    void poll()
    {
        LLControlVariable* control = gSavedSettings.getControl(SETTING_SHIFT);
        if (!control)
        {
            LL_INFOS("RHIProfile") << "shadow shift (research/151): no " << SETTING_SHIFT << " setting, unchanged" << LL_ENDL;
            return;
        }
        const bool on = !control->getValue().asBoolean();
        control->setValue(LLSD(on), false);
        LL_INFOS("RHIProfile") << "shadow shift (research/151) " << (on ? "on" : "off") << LL_ENDL;
    }

    // At most once a second, with LLRHI_CUE_DIR only: shadowshift.toggle flips
    // PlusShadowShift for an A/B in one start (tools/native-ab.ps1). Not saved,
    // as camerapath.run's "set" (the saved value stays). The depth range in
    // steps (plus/shadowdist) follows the same setting.
    void pollCue()
    {
        static LLRHI::CuePoll cue_poll(CUE_POLL_SECONDS);
        if (cue_poll.due() && LLRHI::takeCue(CUE_TOGGLE))
        {
            poll();
        }
    }

    // The frame's first hook, whichever it is: beginReuse (only with the sun
    // shadow cache, LLRHI::sCacheSunShadows) or beginSort (every sun shadow
    // pass of the main view). So sActive follows PlusShadowShift, and what it
    // kept is let go of, with the cache off too.
    void touchFrame()
    {
        if (gFrameCount != sLastFrame)
        {
            sLastFrame = gFrameCount;
            newFrame();
        }
    }

    void refuse(Cascade& c, Why why)
    {
        c.why = why;
        c.why_frame = gFrameCount;
    }
}

    void beginReuse(S32 j)
    {
        touchFrame();
        if (Cascade* c = cascadeAt(j))
        {
            // Nothing of an earlier pass (a snapshot draws several in a frame) carries over.
            c->moved_frame = U32_MAX;
            c->why_frame = U32_MAX;
            c->failed_frame = U32_MAX;
        }
    }

    void noteReject(S32 j, Why why)
    {
        Cascade* c = cascadeAt(j);
        if (c && sActive)
        {
            refuse(*c, why);
        }
    }

    bool accept(S32 j, const glm::mat4& cached_view, const glm::mat4& cached_proj, glm::mat4& view, glm::mat4& proj,
                LLCamera& shadow_cam, const std::vector<LLVector3>& points, U32 map_width, U32 map_height)
    {
        Cascade* c = cascadeAt(j);
        if (!c || !sActive || gCubeSnapshot || points.empty())
        {
            return false;
        }
        Cascade& cascade = *c;
        // What the cached depth holds is listed from the sort's chunks (research/57).
        if (!LLRHI::sShadowSortThreads || !LLRHI::sCullThreads)
        {
            refuse(cascade, WHY_SORT);
            return false;
        }
        // Both stable cascades (orthographic, plus/shadowdist), of even sides.
        const S32 width = (S32)map_width, height = (S32)map_height;
        if (cached_proj[3][3] != 1.f || proj[3][3] != 1.f || cached_proj[2][3] != 0.f || width < 16 || height < 16 ||
            (width & 1) || (height & 1))
        {
            refuse(cascade, WHY_STABLE);
            return false;
        }
        // This frame's size step (G2): as the cached one.
        if (std::fabs(proj[0][0] - cached_proj[0][0]) > SAME_SIZE * std::fabs(cached_proj[0][0]) ||
            std::fabs(proj[1][1] - cached_proj[1][1]) > SAME_SIZE * std::fabs(cached_proj[1][1]))
        {
            refuse(cascade, WHY_SIZE);
            return false;
        }
        // This frame's turn step (G1): the views differ by the sun's drift at
        // most (reuseShadowCascade allows 0.05 degrees), a step is 22.5.
        for (U32 axis = 0; axis < 2; ++axis)
        {
            const glm::vec3 a(cached_view[0][axis], cached_view[1][axis], cached_view[2][axis]);
            const glm::vec3 b(view[0][axis], view[1][axis], view[2][axis]);
            if (glm::dot(a, b) < SAME_TURN)
            {
                refuse(cascade, WHY_TURN);
                return false;
            }
        }
        // What the cached depth holds, on this grid.
        if (!cascade.content_valid || cascade.content_view != cached_view || cascade.content_width != map_width ||
            cascade.content_height != map_height)
        {
            refuse(cascade, WHY_CONTENT);
            return false;
        }

        // The cached window: its centre on the grid (stabilizeCascade put it on a whole texel).
        const F32 to_texels_x = cached_proj[0][0] * 0.5f * (F32)width;
        const F32 to_texels_y = cached_proj[1][1] * 0.5f * (F32)height;
        const F32 from_x = -cached_proj[3][0] * 0.5f * (F32)width;
        const F32 from_y = -cached_proj[3][1] * 0.5f * (F32)height;
        Window from;
        from.cx = (S32)std::lround(from_x);
        from.cy = (S32)std::lround(from_y);
        from.w = width;
        from.h = height;
        if (std::fabs(from_x - (F32)from.cx) > GRID_SLACK || std::fabs(from_y - (F32)from.cy) > GRID_SLACK)
        {
            refuse(cascade, WHY_STABLE);
            return false;
        }

        // The receivers on the grid, and in the cached depth range (G3).
        F32 lo_x = FLT_MAX, lo_y = FLT_MAX, hi_x = -FLT_MAX, hi_y = -FLT_MAX;
        for (const LLVector3& p : points)
        {
            const glm::vec4 q = cached_view * glm::vec4(p.mV[0], p.mV[1], p.mV[2], 1.f);
            const F32 z = cached_proj[2][2] * q.z + cached_proj[3][2];
            if (!(z >= -1.f && z <= 1.f))
            {
                refuse(cascade, WHY_DEPTH);
                return false;
            }
            const F32 x = q.x * to_texels_x, y = q.y * to_texels_y;
            lo_x = llmin(lo_x, x);
            hi_x = llmax(hi_x, x);
            lo_y = llmin(lo_y, y);
            hi_y = llmax(hi_y, y);
        }
        // The new window around them, the grid's whole texels apart from the cached one;
        // an even number along x, where the alpha casters' dither takes the column
        // counted from the window's centre (ditherWidth): it stays where it was.
        Window to = from;
        const F32 mid_x = 0.5f * (lo_x + hi_x);
        to.cx = (S32)std::lround(mid_x);
        if ((to.cx - from.cx) & 1)
        {
            to.cx += mid_x > (F32)to.cx ? 1 : -1;
        }
        to.cy = (S32)std::lround(0.5f * (lo_y + hi_y));
        const Rect window = to.rect();
        if (!(lo_x >= window.x0 && hi_x <= window.x1 && lo_y >= window.y0 && hi_y <= window.y1) ||
            (to.cx == from.cx && to.cy == from.cy))
        {
            refuse(cascade, WHY_SIZE);
            return false;
        }
        // The strips that come into view: drawn whole when they are most of it.
        const F32 dx = std::fabs((F32)(to.cx - from.cx)), dy = std::fabs((F32)(to.cy - from.cy));
        const F32 strips = dx * (F32)height + dy * (F32)width - dx * dy;
        if (dx >= (F32)width || dy >= (F32)height || strips > LARGE_SHARE * (F32)width * (F32)height)
        {
            refuse(cascade, WHY_LARGE);
            return false;
        }

        // The cached view and projection, the centre moved: x_ndc = proj[0][0] x + proj[3][0],
        // the centre at -proj[3][0] * width / 2 texels. Made from the whole numbers each time,
        // so moves never add up rounding.
        glm::mat4 moved = cached_proj;
        moved[3][0] = -2.f * (F32)to.cx / (F32)width;
        moved[3][1] = -2.f * (F32)to.cy / (F32)height;
        view = cached_view;
        proj = moved;
        {
            // The cull planes for it, as generateSunShadow makes them; its near
            // plane (the light's side, at the camera) stays this frame's.
            MatrixScope matrices;
            const LLPlane near_plane = shadow_cam.getAgentPlane(LLCamera::AGENT_PLANE_NEAR);
            set_current_modelview(view);
            set_current_projection(proj);
            LLViewerCamera::updateFrustumPlanes(shadow_cam, false, false, true);
            shadow_cam.getAgentPlane(LLCamera::AGENT_PLANE_NEAR).set(near_plane);
        }
        cascade.moved_frame = gFrameCount;
        cascade.from = from;
        cascade.to = to;
        return true;
    }

    bool beginDraw(S32 index, S32 position)
    {
        Cascade* c = cascadeAt(index);
        if (!c || !sActive)
        {
            return false;
        }
        ++sStats.cascades;
        return c->moved_frame == gFrameCount && position >= 0;
    }

    void beginStrips(S32 index, const glm::mat4& view, const glm::mat4& proj)
    {
        sNoting = Noting();
        Cascade* c = cascadeAt(index);
        if (!c || c->moved_frame != gFrameCount)
        {
            return;
        }
        Noting& n = sNoting;
        n.cascade = c;
        n.index = index;
        n.rows.set(view, proj, (U32)c->to.w, (U32)c->to.h);
        c->next.clear();
        c->next_draws.clear();
        if (!c->strips)
        {
            c->strips = std::make_unique<LLCullResult>();
        }
        c->strips->clear();

        const Rect from = c->from.rect(), to = c->to.rect();
        n.shared = from.intersection(to).grown(1.f);
        const S32 dx = c->to.cx - c->from.cx, dy = c->to.cy - c->from.cy;
        if (dx)
        {
            n.strip[0] = (dx > 0 ? Rect{ from.x1, to.y0, to.x1, to.y1 } : Rect{ to.x0, to.y0, from.x0, to.y1 }).grown(1.f);
        }
        if (dy)
        {
            n.strip[1] = (dy > 0 ? Rect{ to.x0, from.y1, to.x1, to.y1 } : Rect{ to.x0, to.y0, to.x1, from.y0 }).grown(1.f);
        }
        n.whole = wholeSetting();

        Verify& v = sVerify;
        if (verifySetting() && !v.pending && (index == v.turn || v.waited >= VERIFY_WAIT_FRAMES))
        {
            if (!v.casters)
            {
                v.casters = std::make_unique<LLCullResult>();
            }
            v.casters->clear();
            n.verify = v.casters.get();
        }
    }

    void noteCached(U32 type, LLDrawInfo* info, U64 hash)
    {
        Noting& n = sNoting;
        if (!n.cascade || !info)
        {
            return;
        }
        // Only listed here: which ones the strips and a partial redraw take is
        // known once all are (drawShift).
        const Rect r = n.rows.rect(info);
        n.cascade->next.push_back({ hash, r });
        n.cascade->next_draws.push_back({ type, info });
        ++n.cached_infos;
        if (r.overlaps(n.shared))
        {
            n.shared_now += hash;
        }
    }

    bool drawShift(S32 index, LLRenderTarget& shadow, LLRenderTarget& cache, const glm::mat4& view, const glm::mat4& proj,
                   LLCamera& camera, U32 list_serial)
    {
        Cascade* c = cascadeAt(index);
        Noting& n = sNoting;
        sPartial.rects.clear();
        sPartial.touch.clear();
        if (!c || n.cascade != c)
        {
            if (c)
            {
                c->failed = WHY_CONTENT;
                c->failed_frame = gFrameCount;
            }
            return false;
        }
        Cascade& cascade = *c;
        n.cascade = nullptr;    // done listing
        auto fail = [&cascade](Why why)
        {
            cascade.failed = why;
            cascade.failed_frame = gFrameCount;
            cascade.next_draws.clear();
            sPartial.rects.clear();
            sPartial.touch.clear();
            return false;
        };

        // The casters' bias clamp the cached depth was drawn with (G6).
        const F32 clamp = plus::shadowdist::casterClamp(index);
        if (std::fabs(clamp - cascade.clamp) > CLAMP_TOLERANCE * llmax(clamp, cascade.clamp))
        {
            return fail(WHY_CLAMP);
        }
        const U32 width = shadow.getWidth(), height = shadow.getHeight();
        if (cache.getWidth() != width || cache.getHeight() != height || width != (U32)cascade.to.w || height != (U32)cascade.to.h)
        {
            return fail(WHY_TARGET);
        }
        const S32 dx = cascade.to.cx - cascade.from.cx, dy = cascade.to.cy - cascade.from.cy;
        // Where both windows look, the cached depth holds exactly the casters the
        // cull found now (none gone, none come: research/151 2.8); or, with
        // PlusShadowShiftPartial, what differs is cleared and redrawn (research/158 2).
        U64 shared_cached = 0;
        for (const Item& item : cascade.content)
        {
            if (item.rect.overlaps(n.shared))
            {
                shared_cached += item.hash;
            }
        }
        if (shared_cached != n.shared_now)
        {
            if (!partialSetting())
            {
                return fail(WHY_MISMATCH);
            }
            const F32 strip_texels = (F32)std::abs(dx) * (F32)height + (F32)std::abs(dy) * (F32)width - (F32)std::abs(dx * dy);
            const Why why = findDirty(cascade, n, strip_texels);
            if (why != WHY_COUNT)
            {
                return fail(why);
            }
        }

        // The casters drawn over the moved depth: the strips' and the cleared rects'.
        // Drawn whole even where the windows overlap or outside a rect: the same caster
        // with the same projection writes the same depth there (depth test LESS).
        for (size_t i = 0; i < cascade.next.size(); ++i)
        {
            const Rect& r = cascade.next[i].rect;
            const Cascade::Draw& draw = cascade.next_draws[i];
            if (n.whole || r.overlaps(n.strip[0]) || r.overlaps(n.strip[1]))
            {
                cascade.strips->pushDrawInfo(draw.type, draw.info);
                ++n.strip_infos;
            }
            else if (touchesAny(sPartial.touch, r))
            {
                cascade.strips->pushDrawInfo(draw.type, draw.info);
                ++n.dirty_infos;
            }
            if (n.verify)
            {
                n.verify->pushDrawInfo(draw.type, draw.info);
            }
        }
        cascade.next_draws.clear();

        // 1. The cached depth moved into the shadow map, far (cleared) in the strips;
        // 2. far in the partial redraw's rects; 3. the casters above over it.
        shadow.bindTarget();
        shadow.getViewport(gGLViewport);
        shadow.clear();
        copyDepth(cache, dx, dy, width, height, LLTexUnit::TFO_POINT);     // the cache: allocateDepth's TFO_POINT
        clearRects(shadow, sPartial.rects, width, height);
        if (n.strip_infos || n.dirty_infos || (gPipeline.mRHIListKinds & 1))
        {
            drawCasters(*cascade.strips, view, proj, camera, list_serial);
        }
        shadow.flush();
        // 4. Back into the cache: the moved window's static depth.
        cache.bindTarget();
        cache.getViewport(gGLViewport);
        copyDepth(shadow, 0, 0, width, height, LLTexUnit::TFO_ANISOTROPIC);    // allocateShadowBuffer's filter
        cache.flush();

        cascade.content.swap(cascade.next);
        ++sStats.moved;
        sStats.moved_texels += (U64)(std::abs(dx) + std::abs(dy));
        sStats.strip_infos += n.strip_infos;
        sStats.cached_infos += n.cached_infos;
        if (!sPartial.rects.empty())
        {
            U64 area = 0;
            for (const PixelRect& r : sPartial.rects)
            {
                area += r.area();
            }
            ++sStats.partial;
            sStats.partial_rects += sPartial.rects.size();
            sStats.partial_share += (F64)area / ((F64)width * (F64)height);
            sStats.partial_infos += n.dirty_infos;
        }
        if (n.verify)
        {
            drawVerify(index, cascade, view, proj, camera, list_serial);
        }
        return true;
    }

    bool recording(S32 index, S32 position)
    {
        Cascade* c = cascadeAt(index);
        if (!c || !sActive)
        {
            return false;
        }
        if (position < 0)
        {
            c->content_valid = false;   // nothing to list it from
            ++sStats.whole[WHY_SORT];
            return false;
        }
        return true;
    }

    void beginContent(S32 index, const glm::mat4& view, const glm::mat4& proj, U32 width, U32 height, Why why)
    {
        sNoting = Noting();
        Cascade* c = cascadeAt(index);
        if (!c)
        {
            return;
        }
        if (c->failed_frame == gFrameCount)
        {
            why = c->failed;
        }
        else if (why == WHY_PROJECTION)
        {
            why = c->why_frame == gFrameCount ? c->why : WHY_FIRST;
        }
        ++sStats.whole[why < WHY_PROJECTION ? why : WHY_FIRST];

        Noting& n = sNoting;
        n.cascade = c;
        n.index = index;
        n.rows.set(view, proj, width, height);
        c->content_valid = false;
        c->content_view = view;
        c->content_width = width;
        c->content_height = height;
        c->next.clear();
        c->next_draws.clear();
    }

    void noteContent(LLDrawInfo* info, U64 hash)
    {
        Noting& n = sNoting;
        if (n.cascade && info)
        {
            n.cascade->next.push_back({ hash, n.rows.rect(info) });
        }
    }

    void endContent()
    {
        Noting& n = sNoting;
        if (!n.cascade)
        {
            return;
        }
        Cascade& c = *n.cascade;
        c.content.swap(c.next);
        c.content_valid = true;
        c.clamp = plus::shadowdist::casterClamp(n.index);
        sNoting = Noting();
    }

    void release()
    {
        letGo();
    }

    U32 ditherWidth(U32 width)
    {
        const S32 cascade = (S32)LLViewerCamera::sCurCameraID - (S32)LLViewerCamera::CAMERA_SUN_SHADOW0;
        // sActive only as this frame's hooks set it (none ran: no sun shadow pass of the main view).
        if (cascade < 0 || cascade > 3 || gCubeSnapshot || !sActive || sLastFrame != gFrameCount)
        {
            return width;
        }
        return width + 1;
    }

    void beginFrame()
    {
        touchFrame();
    }

    bool beginSort()
    {
        touchFrame();
        pollCue();
        if (!sActive || !firstSeenSetting())
        {
            return false;
        }
        // Only while the camera moves (a cascade moved, or left its cached
        // projection): standing still, a group seen for the first time would
        // force a whole redraw of the cascades whose cached depth it joined at
        // once (research/158 3.4), where it waited for the 2 s refresh before.
        for (const Cascade& c : sCascades)
        {
            if (c.moved_frame == gFrameCount || c.why_frame == gFrameCount)
            {
                return true;
            }
        }
        return false;
    }

    void noteFirstSeen(U32 groups)
    {
        sStats.first_seen += groups;
    }
}

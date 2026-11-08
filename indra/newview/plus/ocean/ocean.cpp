/**
 * @file ocean.cpp
 * @brief The ocean: which water is sea, the distance to the shore and its
 *        direction, as a small texture for the water shader
 *        (research/112-ocean-shader-plan.md in the sl-viewer repository).
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

// SL has no "sea": there is only each region's water height, the same for a
// pool and for the ocean. The shore effects (white edges, waves coming in)
// only look right on open water, so this decides what is sea:
//   - a body of water (4-connected cells lower than the water) is sea when it
//     covers at least PlusOceanMinArea m2 (default 4 ha), or a quarter of that
//     while it reaches the edge of the loaded terrain (it goes on past it);
//   - smaller ones are ponds: a thin edge of foam, no waves.
// The terrain around the camera (1024 m square, 4 m cells, snapped to 64 m of
// the world) is sorted once, and again when the camera nears its edge or the
// loaded terrain changes. The result is a 256 x 256 RGBA8 texture (see
// ocean.h). The water shader (class3/environment/waterF.glsl, the <plus>
// parts) adds the foam from it and from the depth behind the water.
//
// Settings (settings_plus.xml): PlusOcean (on / off), PlusOceanMinArea (m2).
// Retired 2026-10-31 (ocean-retire): PlusOcean is off, not saved (Persist 0), and in neither the
// photo tools nor the quality levels; Debug Settings can turn it on for one session.
// Main thread only.

#include "llviewerprecompiledheaders.h"

#include "plus.h"

#include "ocean.h"
#include "plus/freeze/freeze.h"

#include "llagent.h"
#include "llcallbacklist.h"
#include "llenvironment.h"   // PLUS_WATER_DIAG (fix-round30)
#include "llframetimer.h"
#include "llglslshader.h"
#include "llimage.h"
#include "llrender.h"
#include "llrhi.h"           // LLRHI::sPeriodicLogs
#include "llsurface.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llviewertexture.h"
#include "llworld.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern bool gCubeSnapshot;      // llviewerdisplay.cpp: a reflection probe face or mirror is being drawn

namespace plus::ocean::field
{
    namespace
    {
        constexpr S32 CELLS = GRID * GRID;
    }

    void classify(const U8* kind, F32 min_area, U8* body)
    {
        std::memset(body, BODY_NONE, CELLS);
        const F32 cell_area = CELL_METERS * CELL_METERS;
        std::vector<U8> seen(CELLS, 0);
        std::vector<S32> stack;
        std::vector<S32> members;

        for (S32 start = 0; start < CELLS; ++start)
        {
            if (kind[start] != KIND_WATER || seen[start])
            {
                continue;
            }
            bool open_edge = false;
            stack.clear();
            members.clear();
            stack.push_back(start);
            seen[start] = 1;
            while (!stack.empty())
            {
                const S32 c = stack.back();
                stack.pop_back();
                members.push_back(c);
                const S32 x = c % GRID;
                const S32 y = c / GRID;
                const S32 nx[4] = { x - 1, x + 1, x, x };
                const S32 ny[4] = { y, y, y - 1, y + 1 };
                for (S32 k = 0; k < 4; ++k)
                {
                    if (nx[k] < 0 || nx[k] >= GRID || ny[k] < 0 || ny[k] >= GRID)
                    {
                        open_edge = true;
                        continue;
                    }
                    const S32 n = ny[k] * GRID + nx[k];
                    if (kind[n] == KIND_UNKNOWN)
                    {
                        open_edge = true;
                    }
                    else if (kind[n] == KIND_WATER && !seen[n])
                    {
                        seen[n] = 1;
                        stack.push_back(n);
                    }
                }
            }
            const F32 area = (F32)members.size() * cell_area;
            const bool sea = area >= min_area || (open_edge && area >= min_area * 0.25f);
            const U8 value = sea ? BODY_SEA : BODY_POND;
            for (S32 c : members)
            {
                body[c] = value;
            }
        }
    }

    namespace
    {
        // One line of the exact distance transform (Felzenszwalb and Huttenlocher): d[q] = min over p of
        // ((q - p)^2 + f[p]). v and z are scratch for n and n + 1 entries.
        void distanceLine(const F32* f, S32 n, F32* d, S32* v, F32* z)
        {
            constexpr F32 BIG = 1.0e12f;
            S32 k = 0;
            v[0] = 0;
            z[0] = -BIG;
            z[1] = BIG;
            for (S32 q = 1; q < n; ++q)
            {
                F32 s = 0.f;
                while (true)
                {
                    s = ((f[q] + (F32)q * q) - (f[v[k]] + (F32)v[k] * v[k])) / (2.f * q - 2.f * v[k]);
                    if (s <= z[k] && k > 0)
                    {
                        --k;
                    }
                    else
                    {
                        break;
                    }
                }
                if (s <= z[k])
                {
                    // k == 0 and the new point beats it everywhere
                    v[0] = q;
                    z[0] = -BIG;
                    z[1] = BIG;
                    continue;
                }
                ++k;
                v[k] = q;
                z[k] = s;
                z[k + 1] = BIG;
            }
            k = 0;
            for (S32 q = 0; q < n; ++q)
            {
                while (z[k + 1] < q)
                {
                    ++k;
                }
                d[q] = ((F32)q - v[k]) * ((F32)q - v[k]) + f[v[k]];
            }
        }
    }

    void shoreDistance(const U8* kind, F32* dist)
    {
        // The exact distance (not a chamfer, whose lines of equal distance are octagons) from each cell to
        // the nearest land cell, then smoothed a little so the 4 m cells do not show in the waves.
        constexpr F32 NOT_LAND = 1.0e10f;
        std::vector<F32> f(GRID), d(GRID), tmp(CELLS);
        std::vector<S32> v(GRID);
        std::vector<F32> z(GRID + 1);
        for (S32 x = 0; x < GRID; ++x)
        {
            for (S32 y = 0; y < GRID; ++y)
            {
                f[y] = (kind[y * GRID + x] == KIND_LAND) ? 0.f : NOT_LAND;
            }
            distanceLine(f.data(), GRID, d.data(), v.data(), z.data());
            for (S32 y = 0; y < GRID; ++y)
            {
                tmp[y * GRID + x] = d[y];
            }
        }
        for (S32 y = 0; y < GRID; ++y)
        {
            distanceLine(tmp.data() + y * GRID, GRID, d.data(), v.data(), z.data());
            for (S32 x = 0; x < GRID; ++x)
            {
                dist[y * GRID + x] = std::min(std::sqrt(d[x]) * CELL_METERS, MAX_SHORE_DIST);
            }
        }

        // [1 2 1] / 4 along x then y, twice.
        for (S32 pass = 0; pass < 2; ++pass)
        {
            for (S32 y = 0; y < GRID; ++y)
            {
                for (S32 x = 0; x < GRID; ++x)
                {
                    const F32 a = dist[y * GRID + std::max(x - 1, 0)];
                    const F32 b = dist[y * GRID + x];
                    const F32 c = dist[y * GRID + std::min(x + 1, GRID - 1)];
                    tmp[y * GRID + x] = 0.25f * a + 0.5f * b + 0.25f * c;
                }
            }
            for (S32 y = 0; y < GRID; ++y)
            {
                for (S32 x = 0; x < GRID; ++x)
                {
                    const F32 a = tmp[std::max(y - 1, 0) * GRID + x];
                    const F32 b = tmp[y * GRID + x];
                    const F32 c = tmp[std::min(y + 1, GRID - 1) * GRID + x];
                    dist[y * GRID + x] = 0.25f * a + 0.5f * b + 0.25f * c;
                }
            }
        }
    }

    void pack(const U8* kind, const U8* body, const F32* dist, U8* rgba)
    {
        auto distAt = [&](S32 x, S32 y)
        {
            x = std::clamp(x, 0, GRID - 1);
            y = std::clamp(y, 0, GRID - 1);
            return dist[y * GRID + x];
        };
        auto toByte = [](F32 v) { return (U8)std::clamp((S32)std::lround(v * 255.f), 0, 255); };

        for (S32 y = 0; y < GRID; ++y)
        {
            for (S32 x = 0; x < GRID; ++x)
            {
                const S32 c = y * GRID + x;
                U8* px = rgba + c * 4;
                if (body[c] == BODY_NONE)
                {
                    px[0] = 0;
                    px[1] = 128;
                    px[2] = 128;
                    px[3] = 0;
                    continue;
                }
                // The shore lies where the distance falls: the direction is minus its gradient.
                F32 gx = -(distAt(x + 1, y) - distAt(x - 1, y));
                F32 gy = -(distAt(x, y + 1) - distAt(x, y - 1));
                const F32 len = std::sqrt(gx * gx + gy * gy);
                if (len > 1.0e-3f)
                {
                    gx /= len;
                    gy /= len;
                }
                else
                {
                    gx = 0.f;
                    gy = 0.f;
                }
                px[0] = toByte(dist[c] / MAX_SHORE_DIST);
                px[1] = toByte(gx * 0.5f + 0.5f);
                px[2] = toByte(gy * 0.5f + 0.5f);
                px[3] = (body[c] == BODY_SEA) ? 255 : 128;
            }
        }

        // Land next to water takes its neighbour's values (two cells deep).
        std::vector<U8> before;
        for (S32 pass = 0; pass < 2; ++pass)
        {
            before.assign(rgba, rgba + CELLS * 4);
            for (S32 y = 0; y < GRID; ++y)
            {
                for (S32 x = 0; x < GRID; ++x)
                {
                    const S32 c = y * GRID + x;
                    if (kind[c] != KIND_LAND || before[c * 4 + 3] != 0)
                    {
                        continue;
                    }
                    S32 best = -1;
                    for (S32 dy = -1; dy <= 1; ++dy)
                    {
                        for (S32 dx = -1; dx <= 1; ++dx)
                        {
                            const S32 nx = x + dx;
                            const S32 ny = y + dy;
                            if ((dx == 0 && dy == 0) || nx < 0 || nx >= GRID || ny < 0 || ny >= GRID)
                            {
                                continue;
                            }
                            const S32 n = ny * GRID + nx;
                            if (before[n * 4 + 3] != 0 && (best < 0 || before[n * 4] < before[best * 4]))
                            {
                                best = n;
                            }
                        }
                    }
                    if (best >= 0)
                    {
                        U8* px = rgba + c * 4;
                        px[0] = 0;
                        px[1] = before[best * 4 + 1];
                        px[2] = before[best * 4 + 2];
                        px[3] = before[best * 4 + 3];
                    }
                }
            }
        }
    }
}

namespace
{
    constexpr char SETTING_ON[]       = "PlusOcean";
    constexpr char SETTING_MIN_AREA[] = "PlusOceanMinArea";

    constexpr F64 SNAP_METERS         = 64.0;       // the field's corner is a multiple of this (of the world's coordinates)
    constexpr F64 REBUILD_NEAR_EDGE   = 192.0;      // rebuilt when the camera is this far from the middle of the field (m)
    constexpr F64 MIN_REBUILD_SECONDS = 2.0;        // between two rebuilds because the terrain changed
    constexpr F64 CHECK_SECONDS       = 1.0;        // how often the terrain is looked at for changes
    constexpr F64 TIME_WRAP_SECONDS   = 1000.0;     // the shader's clock restarts here (its wave rates make it seamless)

    struct State
    {
        LLPointer<LLImageRaw>      raw;
        LLPointer<LLViewerTexture> texture;
        LLVector3d                 origin_global;   // the field's corner (x, y), in the world's coordinates
        bool                       valid = false;
        bool                       dry = false;     // valid, but no water in it: no field was made (the shader draws no foam)
        U64                        signature = 0;
        F64                        last_build = -1.0e9;
        F64                        last_check = -1.0e9;
    };

    State& state()
    {
        static State s;
        return s;
    }

    bool isOn()
    {
        static LLCachedControl<bool> on(gSavedSettings, SETTING_ON, false);
        return on();
    }

    // A fingerprint of the loaded terrain: the regions and a few heights of each.
    U64 terrainSignature()
    {
        U64 hash = 1469598103934665603ULL;
        auto mix = [&hash](U64 v) { hash = (hash ^ v) * 1099511628211ULL; };
        for (LLViewerRegion* region : LLWorld::getInstance()->getRegionList())
        {
            if (!region || !region->getLand().hasZData())
            {
                continue;
            }
            mix((U64)(uintptr_t)region);
            const LLSurface& land = region->getLand();
            for (S32 j = 0; j < 4; ++j)
            {
                for (S32 i = 0; i < 4; ++i)
                {
                    mix((U64)(S64)std::lround(land.resolveHeightRegion(16.f + 64.f * i, 16.f + 64.f * j) * 10.f));
                }
            }
        }
        return hash;
    }

    bool anyTerrain()
    {
        for (LLViewerRegion* region : LLWorld::getInstance()->getRegionList())
        {
            if (region && region->getLand().hasZData())
            {
                return true;
            }
        }
        return false;
    }

    // fix-logcache: the field is made again as the camera moves, so its lines
    // go to the log every time only in development starts
    // (LLRHI::sPeriodicLogs); otherwise at most one a minute.
    bool logDue()
    {
        static F64 sLogged = -1.0;
        const F64 now = LLFrameTimer::getElapsedSeconds();
        if (LLRHI::sPeriodicLogs || sLogged < 0.0 || now - sLogged >= 60.0)
        {
            sLogged = now;
            return true;
        }
        return false;
    }

    void rebuild(const LLVector3d& origin_global)
    {
        using namespace plus::ocean::field;
        State& s = state();

        const LLVector3 origin = gAgent.getPosAgentFromGlobal(origin_global);
        std::vector<U8> kind(GRID * GRID, KIND_UNKNOWN);
        for (LLViewerRegion* region : LLWorld::getInstance()->getRegionList())
        {
            if (!region)
            {
                continue;
            }
            const LLSurface& land = region->getLand();
            if (!land.hasZData())
            {
                continue;
            }
            const LLVector3 region_origin = region->getOriginAgent();
            const F32 width = region->getWidth();
            const F32 water = region->getWaterHeight();
            const S32 x0 = std::max(0, (S32)std::floor((region_origin.mV[VX] - origin.mV[VX]) / CELL_METERS));
            const S32 x1 = std::min(GRID - 1, (S32)std::ceil((region_origin.mV[VX] + width - origin.mV[VX]) / CELL_METERS));
            const S32 y0 = std::max(0, (S32)std::floor((region_origin.mV[VY] - origin.mV[VY]) / CELL_METERS));
            const S32 y1 = std::min(GRID - 1, (S32)std::ceil((region_origin.mV[VY] + width - origin.mV[VY]) / CELL_METERS));
            for (S32 y = y0; y <= y1; ++y)
            {
                const F32 ly = origin.mV[VY] + (y + 0.5f) * CELL_METERS - region_origin.mV[VY];
                if (ly < 0.f || ly >= width)
                {
                    continue;
                }
                for (S32 x = x0; x <= x1; ++x)
                {
                    const F32 lx = origin.mV[VX] + (x + 0.5f) * CELL_METERS - region_origin.mV[VX];
                    if (lx < 0.f || lx >= width)
                    {
                        continue;
                    }
                    kind[y * GRID + x] = (land.resolveHeightRegion(lx, ly) >= water) ? KIND_LAND : KIND_WATER;
                }
            }
        }

        // No cell below the water (inland, or high above the land): the field would be all
        // "neither sea nor pond" (alpha 0), where waterF.glsl draws no foam, the same as no field
        // at all. So it is not made (research/125 #12; PlusOceanSkipDryField); the triggers for
        // the next one stay as they are.
        static LLCachedControl<bool> skip_dry(gSavedSettings, "PlusOceanSkipDryField", true);
        if (skip_dry && std::find(kind.begin(), kind.end(), (U8)KIND_WATER) == kind.end())
        {
            if (logDue())
            {
                LL_INFOS("Plus") << "Ocean: no water in the shore field's square, none made" << LL_ENDL;
            }
            s.dry = true;
            s.origin_global = origin_global;
            s.valid = true;
            s.signature = terrainSignature();
            s.last_build = LLFrameTimer::getElapsedSeconds();
            return;
        }
        s.dry = false;

        static LLCachedControl<F32> min_area(gSavedSettings, SETTING_MIN_AREA, 40000.f);
        std::vector<U8> body(GRID * GRID);
        std::vector<F32> dist(GRID * GRID);
        classify(kind.data(), std::max(0.f, min_area()), body.data());
        shoreDistance(kind.data(), dist.data());

        if (s.raw.isNull())
        {
            s.raw = new LLImageRaw(GRID, GRID, 4);
        }
        pack(kind.data(), body.data(), dist.data(), s.raw->getData());

        if (s.texture.isNull())
        {
            s.texture = LLViewerTextureManager::getLocalTexture(s.raw.get(), false);
            s.texture->setAddressMode(LLTexUnit::TAM_CLAMP);
            s.texture->setFilteringOption(LLTexUnit::TFO_BILINEAR);
        }
        if (!s.texture->hasGLTexture())
        {
            s.texture->createGLTexture(0, s.raw);
        }
        s.texture->setSubImage(s.raw, 0, 0, GRID, GRID);

        S32 sea_cells = 0;
        S32 pond_cells = 0;
        for (S32 c = 0; c < GRID * GRID; ++c)
        {
            sea_cells += (body[c] == BODY_SEA) ? 1 : 0;
            pond_cells += (body[c] == BODY_POND) ? 1 : 0;
        }
        if (logDue())
        {
            LL_INFOS("Plus") << "Ocean: shore field made, " << sea_cells << " sea cells, " << pond_cells << " pond cells of "
                             << GRID * GRID << LL_ENDL;
        }

        s.origin_global = origin_global;
        s.valid = true;
        s.signature = terrainSignature();
        s.last_build = LLFrameTimer::getElapsedSeconds();
    }

    // Every frame at idle (never while a frame is drawn): keeps the field
    // around the camera and up to date.
    bool onIdle()
    {
        if (!isOn() || !LLWorld::instanceExists() || !LLViewerCamera::instanceExists())
        {
            return false;
        }
        State& s = state();
        const F64 now = LLFrameTimer::getElapsedSeconds();
        const LLVector3d camera = gAgent.getPosGlobalFromAgent(LLViewerCamera::getInstance()->getOrigin());
        const F64 half = plus::ocean::field::SPAN_METERS * 0.5;

        bool need = false;
        if (!s.valid)
        {
            need = (now - s.last_build > 0.5) && anyTerrain();
        }
        else
        {
            const F64 off_x = camera.mdV[VX] - (s.origin_global.mdV[VX] + half);
            const F64 off_y = camera.mdV[VY] - (s.origin_global.mdV[VY] + half);
            if (std::fabs(off_x) > REBUILD_NEAR_EDGE || std::fabs(off_y) > REBUILD_NEAR_EDGE)
            {
                need = now - s.last_build > 0.5;
            }
            else if (now - s.last_check > CHECK_SECONDS)
            {
                s.last_check = now;
                need = now - s.last_build > MIN_REBUILD_SECONDS && terrainSignature() != s.signature;
            }
        }
        if (need)
        {
            LLVector3d origin;
            origin.mdV[VX] = std::floor((camera.mdV[VX] - half) / SNAP_METERS) * SNAP_METERS;
            origin.mdV[VY] = std::floor((camera.mdV[VY] - half) / SNAP_METERS) * SNAP_METERS;
            origin.mdV[VZ] = 0.0;
            rebuild(origin);
        }
        return false;
    }

    void onUIReady()
    {
        // Settings: app_settings/settings_plus.xml (PlusOcean*).
        // Used by the <plus> hook in LLDrawPoolWater::renderPostDeferred() and
        // the <plus> parts of class3/environment/waterF.glsl.
        doOnIdleRepeating(&onIdle);
    }

    plus::FeatureRegistration sRegistration({ "ocean", nullptr, &onUIReady });
}

namespace
{
    // PLUS_WATER_DIAG=1 (fix-round30, the sea blinking under a personal sky): once a frame, for
    // the main view's water, what the water is drawn with: the selected environment, the water
    // and sky objects and the values the water shader takes from them, and the ocean field's
    // state. A line is logged when any of it differs from the frame before (at most
    // DIAG_MAX_LINES lines), and every 10 seconds how many frames changed. A blink every frame
    // or every few frames shows as lines with frame numbers close together, and the field that
    // flips tells which part of the environment does it. Off: one cached check.
    constexpr U32 DIAG_MAX_LINES = 400;

    void waterDiag(bool ocean_valid, bool ocean_dry)
    {
        static const bool on = []()
        {
            const char* env = std::getenv("PLUS_WATER_DIAG");
            return env && *env == '1';
        }();
        if (!on || gCubeSnapshot)
        {
            return;
        }
        static U32 last_frame = U32_MAX;
        const U32 frame = LLFrameTimer::getFrameCount();
        if (frame == last_frame)
        {
            return;     // once a frame (the first water draw of the main view)
        }
        last_frame = frame;

        LLEnvironment& environment = LLEnvironment::instance();
        const LLSettingsWater::ptr_t water = environment.getCurrentWater();
        const LLSettingsSky::ptr_t sky = environment.getCurrentSky();
        const LLVector3 light = environment.getLightDirection();
        std::string sig = llformat("env %d water %p sky %p sun_up %d light %.3f %.3f %.3f ocean %d%d",
                                   (S32)environment.getSelectedEnvironment(), (void*)water.get(), (void*)sky.get(),
                                   environment.getIsSunUp() ? 1 : 0, light.mV[VX], light.mV[VY], light.mV[VZ],
                                   ocean_valid ? 1 : 0, ocean_dry ? 1 : 0);
        if (water)
        {
            const LLColor3 fog = water->getWaterFogColor();
            sig += llformat(" | normal %s next %s blend %.3f fog %.3f %.3f %.3f density %.3f scale %.3f",
                            water->getNormalMapID().asString().c_str(), water->getNextNormalMapID().asString().c_str(),
                            (F32)water->getBlendFactor(), fog.mV[VRED], fog.mV[VGREEN], fog.mV[VBLUE],
                            water->getWaterFogDensity(), water->getScaleAbove());
        }
        if (sky)
        {
            sig += llformat(" | classic %d probe_ambiance %.3f gamma %.3f", sky->canAutoAdjust() ? 1 : 0,
                            sky->getReflectionProbeAmbiance(), sky->getGamma());
        }

        static std::string last_sig;
        static U32 lines = 0;
        static U32 changed_frames = 0;
        static U32 frames = 0;
        static LLFrameTimer window;     // started at its first use
        ++frames;
        if (sig != last_sig)
        {
            ++changed_frames;
            if (lines < DIAG_MAX_LINES)
            {
                ++lines;
                LL_INFOS("PlusWaterDiag") << "frame " << frame << ": " << sig << LL_ENDL;
            }
            last_sig = sig;
        }
        if (window.getElapsedTimeF32() >= 10.f)
        {
            LL_INFOS("PlusWaterDiag") << "last 10 s: " << changed_frames << " of " << frames << " water frames changed" << LL_ENDL;
            window.reset();
            changed_frames = 0;
            frames = 0;
        }
    }
}

namespace plus::ocean
{
    void bind(LLGLSLShader* shader)
    {
        static LLStaticHashedString s_field("plusOceanField");
        static LLStaticHashedString s_area("plusOceanArea");
        static LLStaticHashedString s_time("plusOceanTime");
        static LLStaticHashedString s_camera("plusOceanCam");
        static LLStaticHashedString s_right("plusOceanRight");
        static LLStaticHashedString s_up("plusOceanUp");
        static LLStaticHashedString s_at("plusOceanAt");

        if (!shader)
        {
            return;
        }
        const State& s = state();
        waterDiag(s.valid, s.dry);     // PLUS_WATER_DIAG=1 only (fix-round30)
        if (!isOn() || !s.valid || s.dry || s.texture.isNull() || !s.texture->hasGLTexture())
        {
            shader->uniform4f(s_area, 0.f, 0.f, 1.f, 0.f);
            return;
        }

        // plusOceanField is not one of the reserved uniforms, so linking gave it no texture
        // unit, and bindTexture takes a reserved uniform's index, not a location: the field was
        // never bound and the shader read unit 0 (research/131). It takes the first unit after
        // the program's own (plus/avatarrefl keeps the last two).
        const S32 unit = shader->mActiveTextureChannels;
        if (unit < 0 || unit >= (S32)LL_NUM_TEXTURE_LAYERS - 2)
        {
            shader->uniform4f(s_area, 0.f, 0.f, 1.f, 0.f);     // no unit to spare: no foam
            return;
        }
        shader->uniform1i(s_field, unit);
        gGL.getTexUnit(unit)->bind(s.texture.get());
        gGL.getTexUnit(0)->activate();

        const LLVector3 origin = gAgent.getPosAgentFromGlobal(s.origin_global);
        shader->uniform4f(s_area, origin.mV[VX], origin.mV[VY], 1.f / field::SPAN_METERS, 1.f);
        // waterF.glsl skips the foam where it would be exactly 0 (research/125 #20): the same pixels.
        static LLStaticHashedString s_foam_skip("plusOceanFoamSkip");
        static LLCachedControl<bool> foam_skip(gSavedSettings, "PlusOceanFoamSkip", true);
        shader->uniform1f(s_foam_skip, foam_skip ? 1.f : 0.f);
        shader->uniform1f(s_time, (F32)std::fmod(plus::freeze::worldSeconds(), TIME_WRAP_SECONDS));   // stops with the world (plus/freeze)

        // The camera's axes (agent space), to bring the water's eye space position back to the world.
        LLViewerCamera* camera = LLViewerCamera::getInstance();
        const LLVector3 right = -camera->getLeftAxis();
        shader->uniform3fv(s_camera, 1, camera->getOrigin().mV);
        shader->uniform3fv(s_right, 1, right.mV);
        shader->uniform3fv(s_up, 1, camera->getUpAxis().mV);
        shader->uniform3fv(s_at, 1, camera->getAtAxis().mV);
    }
}

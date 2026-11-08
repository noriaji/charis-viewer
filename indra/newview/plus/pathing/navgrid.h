/**
 * @file navgrid.h
 * @brief A navigation grid built from triangles, with no Havok and no data from the
 *        server: where a character can stand, how far from an edge, a path between two
 *        points, a ray pick and the drawing rectangles.
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

// Header only, no viewer headers: navgrid_test.cpp compiles it on its own.
//
// The method is the heightfield one used by Recast (Mikko Mononen's navigation mesh
// builder, zlib licence; the method, not the code): every triangle is rasterised into
// columns of the grid as a solid span, the spans are merged, and a span whose top
// faces up, has headroom and is not a ledge is a floor a character can stand on.
//
//   column      a cell of the grid (cell x cell metres), holds spans from low to high
//   span        solid from lo to hi (height units); the free space above it runs up to
//               the next span's lo (ceil). Its top surface, hi, is the floor
//   walk        the top of the span faces up (slope), has the headroom, is not a ledge
//   link        a floor to the floor next to it (4 directions) that a character of at
//               least the smallest size can step to (climb, headroom between the two)
//   Analysis    the grid seen by one size of character: the floors it fits on, the links
//               it can use, and how far from the nearest edge each floor is (clearance)
//
// Coordinates are region-local metres: x, y in [0, width), z up. Heights are kept in
// units of Params::unitZ from Params::zBase so that comparing them is integer work.

#ifndef PLUS_PATHING_NAVGRID_H
#define PLUS_PATHING_NAVGRID_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <queue>
#include <utility>
#include <vector>

namespace plus
{
namespace pathing
{
namespace navgrid
{

struct V3
{
    float x = 0.f, y = 0.f, z = 0.f;
};

inline V3 operator+(const V3& a, const V3& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline V3 operator-(const V3& a, const V3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline V3 operator*(const V3& a, float s) { return { a.x * s, a.y * s, a.z * s }; }
inline V3 cross(const V3& a, const V3& b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
inline float dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float length(const V3& a) { return std::sqrt(dot(a, a)); }

struct Params
{
    float width = 256.f;      // the grid covers [0, width) in x and y
    float cell = 0.5f;        // column size
    float unitZ = 0.0625f;    // height unit
    float zBase = -64.f;      // z of height unit 0 (anything lower is clamped to it)
    float climb = 0.5f;       // tallest step between two floors that are linked
    float slopeCos = 0.70f;   // cosine of the steepest slope that is walkable (about 45 degrees)
    float headroom = 1.0f;    // free height the smallest character needs over a floor
    float ledgeHeadroom = 0.f; // free height over a neighbour that counts it in the ledge test; 0: headroom
};

// A character's size as the grid sees it.
struct Shape
{
    float radius = 0.35f;
    float height = 1.8f;
    int typeBit = 0;          // 0: any floor; 1..4: only floors marked for that character type (bit n of Span::walk)
};

constexpr int32_t kOpenAbove = 1 << 24;   // ceil of a span with nothing over it
// Highest height unit: 8128 m with the default zBase and unitZ (1 << 16 stopped at 4032 m,
// under the 4096 m things can be built up to).
constexpr int32_t kMaxUnits = 1 << 17;
constexpr int kNoLink = -1;

// Link directions: 0 = -x, 1 = +y, 2 = +x, 3 = -y.
constexpr int kDx[4] = { -1, 0, 1, 0 };
constexpr int kDy[4] = { 0, 1, 0, -1 };

struct Span
{
    int32_t lo = 0;
    int32_t hi = 0;       // top surface: the floor
    int32_t ceil = 0;     // lo of the next span up
    uint8_t walk = 0;     // 0: not a floor; else bit 0 = a floor, bits 1..4 = a floor for character type 1..4
};

constexpr uint8_t kFloorAll = 0x1F;

// What one Shape can use of the grid.
struct Analysis
{
    Shape shape;
    std::vector<float> clear;       // per span: metres from the floor's centre to the nearest edge; < 0 = does not fit
    std::vector<int32_t> link;      // per span, 4 entries: the floor next to it that this shape can step to
    bool valid() const { return !clear.empty(); }
};

// A block of floor cells [x0..x1] x [y0..y1] at one height, for drawing.
struct Rect
{
    int x0, y0, x1, y1;
    int32_t hi;
    uint32_t color;
};

// One side of a cell with no floor to step to.
struct Seg
{
    float x0, y0, x1, y1;
    int32_t hi;
};

class Grid
{
public:
    Params p;
    int nx = 0, ny = 0;
    std::vector<int32_t> colStart;     // nx * ny + 1: the spans of column i are colStart[i] .. colStart[i + 1]
    std::vector<Span> spans;
    std::vector<int32_t> conn;         // 4 per span: the floor next to it (best match), kNoLink for none
    std::vector<int32_t> spanCol;      // per span: its column
    int headUnits = 1;
    int ledgeUnits = 1;
    int climbUnits = 0;

    // ---- building ----------------------------------------------------------------

    void begin(const Params& params)
    {
        p = params;
        nx = ny = std::max(1, static_cast<int>(std::ceil(p.width / p.cell)));
        mCols.assign(static_cast<size_t>(nx) * ny, std::vector<BSpan>());
        colStart.clear();
        spans.clear();
        conn.clear();
        spanCol.clear();
    }

    int toUnits(float z) const
    {
        const float u = std::floor((z - p.zBase) / p.unitZ);
        return static_cast<int>(std::min(std::max(u, 0.f), static_cast<float>(kMaxUnits)));
    }

    float unitsToZ(int32_t units) const { return p.zBase + static_cast<float>(units) * p.unitZ; }

    // A solid triangle. Its top counts as a floor where the slope is walkable.
    // `floorMask` is what its top is as a floor where the slope allows (0: solid but never a floor).
    void addTriangle(const V3& a, const V3& b, const V3& c, uint8_t floorMask = kFloorAll)
    {
        V3 n = cross(b - a, c - a);
        const float len = length(n);
        if (!(len > 1e-9f))
        {
            return;
        }
        n = n * (1.f / len);
        const uint8_t walk = n.z >= p.slopeCos ? floorMask : 0;

        const float minx = std::min(a.x, std::min(b.x, c.x)), maxx = std::max(a.x, std::max(b.x, c.x));
        const float miny = std::min(a.y, std::min(b.y, c.y)), maxy = std::max(a.y, std::max(b.y, c.y));
        if (maxx < 0.f || minx >= p.width || maxy < 0.f || miny >= p.width)
        {
            return;
        }
        const int x0 = std::max(0, static_cast<int>(std::floor(minx / p.cell)));
        const int x1 = std::min(nx - 1, static_cast<int>(std::floor(maxx / p.cell)));
        const int y0 = std::max(0, static_cast<int>(std::floor(miny / p.cell)));
        const int y1 = std::min(ny - 1, static_cast<int>(std::floor(maxy / p.cell)));

        // Cut the triangle into rows, then each row into cells (the polygon of the
        // triangle inside one cell gives that cell's lowest and highest z).
        float buf[7 * 3 * 4];
        float* in = buf;
        float* inrow = buf + 7 * 3;
        float* p1 = inrow + 7 * 3;
        float* p2 = p1 + 7 * 3;
        set(in, 0, a);
        set(in, 1, b);
        set(in, 2, c);
        int nvIn = 3;
        // Only the cut at the top of each row (column) is made below, so what lies under
        // y = 0 (x = 0) would stay in the first row (column): cut off first. What lies past
        // the far edge is left over after the last row (column) and dropped.
        if (miny < 0.f)
        {
            int nvOut = 0;
            dividePoly(in, nvIn, inrow, nvOut, p1, nvIn, 0.f, 1);
            std::swap(in, p1);
        }
        for (int y = y0; y <= y1 && nvIn >= 3; ++y)
        {
            int nvrow = 0;
            dividePoly(in, nvIn, inrow, nvrow, p1, nvIn, p.cell * static_cast<float>(y + 1), 1);
            std::swap(in, p1);
            if (nvrow < 3)
            {
                continue;
            }
            int nv2 = nvrow;
            if (minx < 0.f)
            {
                int nvOut = 0;
                dividePoly(inrow, nv2, p1, nvOut, p2, nv2, 0.f, 0);
                std::swap(inrow, p2);
                if (nv2 < 3)
                {
                    continue;
                }
            }
            for (int x = x0; x <= x1; ++x)
            {
                int nv = 0;
                dividePoly(inrow, nv2, p1, nv, p2, nv2, p.cell * static_cast<float>(x + 1), 0);
                std::swap(inrow, p2);
                if (nv < 3)
                {
                    continue;
                }
                float zmin = p1[2], zmax = p1[2];
                for (int k = 1; k < nv; ++k)
                {
                    zmin = std::min(zmin, p1[k * 3 + 2]);
                    zmax = std::max(zmax, p1[k * 3 + 2]);
                }
                addSpan(x, y, zmin, zmax, walk);
            }
        }
    }

    // Terrain: (n x n) heights at `spacing` metres, row-major with x running fastest.
    void addHeightfield(const std::vector<float>& h, int n, float spacing)
    {
        for (int j = 0; j + 1 < n; ++j)
        {
            for (int i = 0; i + 1 < n; ++i)
            {
                const V3 a{ i * spacing, j * spacing, h[static_cast<size_t>(j) * n + i] };
                const V3 b{ (i + 1) * spacing, j * spacing, h[static_cast<size_t>(j) * n + i + 1] };
                const V3 c{ i * spacing, (j + 1) * spacing, h[static_cast<size_t>(j + 1) * n + i] };
                const V3 d{ (i + 1) * spacing, (j + 1) * spacing, h[static_cast<size_t>(j + 1) * n + i + 1] };
                addTriangle(a, b, d);
                addTriangle(a, d, c);
            }
        }
    }

    // Closes the build: flat arrays, headroom, ledges, links. `exclude` (x, y, z in region-local
    // metres) takes floors out of use, for exclusion volumes.
    void finish(const std::function<bool(float, float, float)>* exclude = nullptr)
    {
        headUnits = std::max(1, static_cast<int>(std::ceil(p.headroom / p.unitZ)));
        ledgeUnits = p.ledgeHeadroom > 0.f ? std::max(1, static_cast<int>(std::ceil(p.ledgeHeadroom / p.unitZ))) : headUnits;
        climbUnits = static_cast<int>(std::floor(p.climb / p.unitZ));

        const size_t ncol = mCols.size();
        colStart.assign(ncol + 1, 0);
        size_t total = 0;
        for (const auto& c : mCols)
        {
            total += c.size();
        }
        spans.clear();
        spans.reserve(total);
        spanCol.clear();
        spanCol.reserve(total);
        for (size_t i = 0; i < ncol; ++i)
        {
            colStart[i] = static_cast<int32_t>(spans.size());
            const auto& c = mCols[i];
            for (size_t k = 0; k < c.size(); ++k)
            {
                Span s;
                s.lo = c[k].lo;
                s.hi = c[k].hi;
                s.ceil = k + 1 < c.size() ? c[k + 1].lo : kOpenAbove;
                s.walk = c[k].walk;
                spans.push_back(s);
                spanCol.push_back(static_cast<int32_t>(i));
            }
        }
        colStart[ncol] = static_cast<int32_t>(spans.size());
        mCols.clear();
        mCols.shrink_to_fit();

        for (Span& s : spans)
        {
            if (s.ceil - s.hi < headUnits)
            {
                s.walk = 0;
            }
        }
        if (exclude && *exclude)
        {
            for (size_t i = 0; i < spans.size(); ++i)
            {
                if (spans[i].walk && (*exclude)(center(static_cast<int>(i)).x, center(static_cast<int>(i)).y, floorZ(static_cast<int>(i)) + 0.1f))
                {
                    spans[i].walk = 0;
                }
            }
        }
        filterLedges();
        buildLinks();
    }

    // ---- reading -----------------------------------------------------------------

    int colIndex(int x, int y) const { return y * nx + x; }
    int spanBegin(int col) const { return colStart[static_cast<size_t>(col)]; }
    int spanEnd(int col) const { return colStart[static_cast<size_t>(col) + 1]; }
    int cellX(int span) const { return spanCol[static_cast<size_t>(span)] % nx; }
    int cellY(int span) const { return spanCol[static_cast<size_t>(span)] / nx; }
    float floorZ(int span) const { return unitsToZ(spans[static_cast<size_t>(span)].hi); }

    V3 center(int span) const
    {
        return { (static_cast<float>(cellX(span)) + 0.5f) * p.cell, (static_cast<float>(cellY(span)) + 0.5f) * p.cell, floorZ(span) };
    }

    size_t walkableCount() const
    {
        size_t n = 0;
        for (const Span& s : spans)
        {
            n += s.walk ? 1u : 0u;
        }
        return n;
    }

    // ---- one size of character ---------------------------------------------------

    void analyse(const Shape& shape, Analysis& a) const
    {
        const size_t n = spans.size();
        a.shape = shape;
        a.clear.assign(n, -1.f);
        a.link.assign(n * 4, kNoLink);
        if (n == 0)
        {
            return;
        }
        const int hU = std::max(1, static_cast<int>(std::ceil(shape.height / p.unitZ)));

        std::vector<uint8_t> use(n, 0);
        const uint8_t bit = static_cast<uint8_t>(1u << std::min(std::max(shape.typeBit, 0), 4));
        for (size_t s = 0; s < n; ++s)
        {
            use[s] = ((spans[s].walk & bit) && spans[s].ceil - spans[s].hi >= hU) ? 1 : 0;
        }
        for (size_t s = 0; s < n; ++s)
        {
            if (!use[s])
            {
                continue;
            }
            for (int d = 0; d < 4; ++d)
            {
                const int32_t t = conn[s * 4 + d];
                if (t >= 0 && use[static_cast<size_t>(t)] && gap(static_cast<int>(s), t) >= hU)
                {
                    a.link[s * 4 + d] = t;
                }
            }
        }

        // Distance to the nearest edge: two passes of the 2/3 chamfer (half-cell units).
        constexpr int32_t kFar = 1 << 20;
        std::vector<int32_t> dist(n, kFar);
        for (size_t s = 0; s < n; ++s)
        {
            if (!use[s])
            {
                continue;
            }
            for (int d = 0; d < 4; ++d)
            {
                if (a.link[s * 4 + d] == kNoLink)
                {
                    dist[s] = 0;
                    break;
                }
            }
        }
        const auto& l = a.link;
        for (int y = 0; y < ny; ++y)
        {
            for (int x = 0; x < nx; ++x)
            {
                const int col = colIndex(x, y);
                for (int s = spanBegin(col); s < spanEnd(col); ++s)
                {
                    if (!use[static_cast<size_t>(s)])
                    {
                        continue;
                    }
                    int32_t& ds = dist[static_cast<size_t>(s)];
                    const int w = l[static_cast<size_t>(s) * 4 + 0];
                    if (w >= 0)
                    {
                        ds = std::min(ds, dist[static_cast<size_t>(w)] + 2);
                        const int ww = l[static_cast<size_t>(w) * 4 + 3];
                        if (ww >= 0)
                        {
                            ds = std::min(ds, dist[static_cast<size_t>(ww)] + 3);
                        }
                    }
                    const int so = l[static_cast<size_t>(s) * 4 + 3];
                    if (so >= 0)
                    {
                        ds = std::min(ds, dist[static_cast<size_t>(so)] + 2);
                        const int se = l[static_cast<size_t>(so) * 4 + 2];
                        if (se >= 0)
                        {
                            ds = std::min(ds, dist[static_cast<size_t>(se)] + 3);
                        }
                    }
                }
            }
        }
        for (int y = ny - 1; y >= 0; --y)
        {
            for (int x = nx - 1; x >= 0; --x)
            {
                const int col = colIndex(x, y);
                for (int s = spanBegin(col); s < spanEnd(col); ++s)
                {
                    if (!use[static_cast<size_t>(s)])
                    {
                        continue;
                    }
                    int32_t& ds = dist[static_cast<size_t>(s)];
                    const int e = l[static_cast<size_t>(s) * 4 + 2];
                    if (e >= 0)
                    {
                        ds = std::min(ds, dist[static_cast<size_t>(e)] + 2);
                        const int ne = l[static_cast<size_t>(e) * 4 + 1];
                        if (ne >= 0)
                        {
                            ds = std::min(ds, dist[static_cast<size_t>(ne)] + 3);
                        }
                    }
                    const int no = l[static_cast<size_t>(s) * 4 + 1];
                    if (no >= 0)
                    {
                        ds = std::min(ds, dist[static_cast<size_t>(no)] + 2);
                        const int nw = l[static_cast<size_t>(no) * 4 + 0];
                        if (nw >= 0)
                        {
                            ds = std::min(ds, dist[static_cast<size_t>(nw)] + 3);
                        }
                    }
                }
            }
        }
        for (size_t s = 0; s < n; ++s)
        {
            if (use[s])
            {
                // An edge floor's centre is half a cell from the edge itself.
                a.clear[s] = static_cast<float>(std::min(dist[s], kFar)) * p.cell * 0.5f + p.cell * 0.5f;
            }
        }
    }

    bool allowed(const Analysis& a, int span) const
    {
        // A floor's clearance is measured from its centre; a character can stand anywhere in the
        // cell, so a quarter of a cell is allowed on top (a one metre gap takes a 0.4 m radius).
        return span >= 0 && a.clear[static_cast<size_t>(span)] + p.cell * 0.25f >= a.shape.radius;
    }

    // The floor nearest to (x, y, z) that the shape fits on, looking out a few cells.
    int nearestSpan(const Analysis& a, float x, float y, float z, int maxRing = 4) const
    {
        const int cx = static_cast<int>(std::floor(x / p.cell));
        const int cy = static_cast<int>(std::floor(y / p.cell));
        int best = -1;
        float bestScore = 1e30f;
        for (int ring = 0; ring <= maxRing; ++ring)
        {
            for (int dy = -ring; dy <= ring; ++dy)
            {
                for (int dx = -ring; dx <= ring; ++dx)
                {
                    if (std::max(std::abs(dx), std::abs(dy)) != ring)
                    {
                        continue;
                    }
                    const int xx = cx + dx, yy = cy + dy;
                    if (xx < 0 || yy < 0 || xx >= nx || yy >= ny)
                    {
                        continue;
                    }
                    const int col = colIndex(xx, yy);
                    for (int s = spanBegin(col); s < spanEnd(col); ++s)
                    {
                        if (!allowed(a, s))
                        {
                            continue;
                        }
                        const float ex = (static_cast<float>(xx) + 0.5f) * p.cell - x;
                        const float ey = (static_cast<float>(yy) + 0.5f) * p.cell - y;
                        const float ez = floorZ(s) - z;
                        // Height counts most: the floor under a point, not the one over it.
                        const float score = ex * ex + ey * ey + 4.f * ez * ez;
                        if (score < bestScore)
                        {
                            bestScore = score;
                            best = s;
                        }
                    }
                }
            }
            if (best >= 0)
            {
                break;
            }
        }
        return best;
    }

    // A path from `from` to `to` for the shape; false when there is none. The points
    // run from `from` through floor centres to `to`, pulled straight where they can be.
    bool findPath(const Analysis& a, const V3& from, const V3& to, std::vector<V3>& out) const
    {
        out.clear();
        const int s0 = nearestSpan(a, from.x, from.y, from.z);
        const int s1 = nearestSpan(a, to.x, to.y, to.z);
        if (s0 < 0 || s1 < 0)
        {
            return false;
        }
        std::vector<int> cells;
        if (!astar(a, s0, s1, cells))
        {
            return false;
        }

        // Pull the route straight: from each point, the farthest one still in line of sight.
        std::vector<int> kept;
        kept.push_back(cells.front());
        size_t i = 0;
        while (i + 1 < cells.size())
        {
            size_t j = cells.size() - 1;
            while (j > i + 1 && !lineClear(a, cells[i], cells[j]))
            {
                --j;
            }
            kept.push_back(cells[j]);
            i = j;
        }

        out.push_back(from);
        for (size_t k = 1; k + 1 < kept.size(); ++k)
        {
            out.push_back(center(kept[k]));
        }
        out.push_back(to);
        return true;
    }

    // The first floor a segment from `o` to `e` comes down on, from above.
    bool pick(const V3& o, const V3& e, V3& hit, int* spanOut = nullptr) const
    {
        const V3 d = e - o;
        if (!(d.z < -1e-9f) || spans.empty())
        {
            return false;
        }
        // Clip the segment to the grid's square (slab method).
        float t0 = 0.f, t1 = 1.f;
        const float lo = 0.f, hi = p.cell * static_cast<float>(nx);
        const float oo[2] = { o.x, o.y };
        const float dd[2] = { d.x, d.y };
        for (int k = 0; k < 2; ++k)
        {
            if (std::fabs(dd[k]) < 1e-12f)
            {
                if (oo[k] < lo || oo[k] >= hi)
                {
                    return false;
                }
            }
            else
            {
                float ta = (lo - oo[k]) / dd[k], tb = (hi - oo[k]) / dd[k];
                if (ta > tb)
                {
                    std::swap(ta, tb);
                }
                t0 = std::max(t0, ta);
                t1 = std::min(t1, tb);
                if (t0 > t1)
                {
                    return false;
                }
            }
        }

        // Walk the columns the segment crosses, in order (Amanatides and Woo).
        const float px = o.x + d.x * t0, py = o.y + d.y * t0;
        int cx = std::min(nx - 1, std::max(0, static_cast<int>(std::floor(px / p.cell))));
        int cy = std::min(ny - 1, std::max(0, static_cast<int>(std::floor(py / p.cell))));
        const int stepX = d.x > 0.f ? 1 : -1, stepY = d.y > 0.f ? 1 : -1;
        constexpr float kInf = 1e30f;
        const float tDeltaX = std::fabs(d.x) > 1e-12f ? p.cell / std::fabs(d.x) : kInf;
        const float tDeltaY = std::fabs(d.y) > 1e-12f ? p.cell / std::fabs(d.y) : kInf;
        float tMaxX = kInf, tMaxY = kInf;
        if (std::fabs(d.x) > 1e-12f)
        {
            const float edge = (static_cast<float>(cx) + (stepX > 0 ? 1.f : 0.f)) * p.cell;
            tMaxX = (edge - o.x) / d.x;
        }
        if (std::fabs(d.y) > 1e-12f)
        {
            const float edge = (static_cast<float>(cy) + (stepY > 0 ? 1.f : 0.f)) * p.cell;
            tMaxY = (edge - o.y) / d.y;
        }

        float tCur = t0;
        for (;;)
        {
            const float tNext = std::min(std::min(tMaxX, tMaxY), t1);
            const int col = colIndex(cx, cy);
            float bestT = kInf;
            int bestSpan = -1;
            for (int s = spanBegin(col); s < spanEnd(col); ++s)
            {
                if (!spans[static_cast<size_t>(s)].walk)
                {
                    continue;
                }
                const float t = (floorZ(s) - o.z) / d.z;
                if (t >= tCur - 1e-5f && t <= tNext + 1e-5f && t >= 0.f && t <= 1.f && t < bestT)
                {
                    bestT = t;
                    bestSpan = s;
                }
            }
            if (bestSpan >= 0)
            {
                hit = o + d * bestT;
                if (spanOut)
                {
                    *spanOut = bestSpan;
                }
                return true;
            }
            if (tNext >= t1)
            {
                return false;
            }
            tCur = tNext;
            if (tMaxX < tMaxY)
            {
                cx += stepX;
                tMaxX += tDeltaX;
            }
            else
            {
                cy += stepY;
                tMaxY += tDeltaY;
            }
            if (cx < 0 || cy < 0 || cx >= nx || cy >= ny)
            {
                return false;
            }
        }
    }

    // ---- for drawing -------------------------------------------------------------

    // Floors with a colour (non-zero) joined into rectangles of one height and colour.
    std::vector<Rect> greedyRects(const std::vector<uint32_t>& color) const
    {
        std::vector<Rect> out;
        std::vector<uint8_t> done(spans.size(), 0);
        for (int y = 0; y < ny; ++y)
        {
            for (int x = 0; x < nx; ++x)
            {
                const int col = colIndex(x, y);
                for (int s = spanBegin(col); s < spanEnd(col); ++s)
                {
                    const size_t si = static_cast<size_t>(s);
                    if (color[si] == 0 || done[si])
                    {
                        continue;
                    }
                    const int32_t hi = spans[si].hi;
                    const uint32_t c = color[si];
                    int x1 = x;
                    while (x1 + 1 < nx)
                    {
                        const int t = findSpan(x1 + 1, y, hi, c, color, done);
                        if (t < 0)
                        {
                            break;
                        }
                        ++x1;
                    }
                    int y1 = y;
                    while (y1 + 1 < ny)
                    {
                        bool all = true;
                        for (int xx = x; xx <= x1 && all; ++xx)
                        {
                            all = findSpan(xx, y1 + 1, hi, c, color, done) >= 0;
                        }
                        if (!all)
                        {
                            break;
                        }
                        ++y1;
                    }
                    for (int yy = y; yy <= y1; ++yy)
                    {
                        for (int xx = x; xx <= x1; ++xx)
                        {
                            done[static_cast<size_t>(findSpan(xx, yy, hi, c, color, done))] = 1;
                        }
                    }
                    out.push_back({ x, y, x1, y1, hi, c });
                }
            }
        }
        return out;
    }

    // The sides of coloured floors that have no coloured floor to step to.
    std::vector<Seg> boundarySegs(const std::vector<uint32_t>& color) const
    {
        std::vector<Seg> out;
        for (size_t s = 0; s < spans.size(); ++s)
        {
            if (color[s] == 0)
            {
                continue;
            }
            const int x = cellX(static_cast<int>(s)), y = cellY(static_cast<int>(s));
            const float x0 = static_cast<float>(x) * p.cell, x1 = x0 + p.cell;
            const float y0 = static_cast<float>(y) * p.cell, y1 = y0 + p.cell;
            for (int d = 0; d < 4; ++d)
            {
                const int32_t t = conn[s * 4 + d];
                if (t >= 0 && color[static_cast<size_t>(t)] != 0)
                {
                    continue;
                }
                Seg g{};
                g.hi = spans[s].hi;
                switch (d)
                {
                case 0: g = { x0, y0, x0, y1, g.hi }; break;
                case 1: g = { x0, y1, x1, y1, g.hi }; break;
                case 2: g = { x1, y0, x1, y1, g.hi }; break;
                default: g = { x0, y0, x1, y0, g.hi }; break;
                }
                out.push_back(g);
            }
        }
        return out;
    }

private:
    struct BSpan
    {
        int32_t lo, hi;
        uint8_t walk;
    };
    std::vector<std::vector<BSpan>> mCols;

    static void set(float* v, int i, const V3& a)
    {
        v[i * 3 + 0] = a.x;
        v[i * 3 + 1] = a.y;
        v[i * 3 + 2] = a.z;
    }

    // Cuts a polygon at coordinate `x` of `axis`: out1 gets the part at or below, out2 the rest.
    static void dividePoly(const float* in, int nin, float* out1, int& nout1, float* out2, int& nout2, float x, int axis)
    {
        float d[12];
        for (int i = 0; i < nin; ++i)
        {
            d[i] = x - in[i * 3 + axis];
        }
        int m = 0, n = 0;
        for (int i = 0, j = nin - 1; i < nin; j = i, ++i)
        {
            const bool ina = d[j] >= 0.f, inb = d[i] >= 0.f;
            if (ina != inb)
            {
                const float s = d[j] / (d[j] - d[i]);
                for (int k = 0; k < 3; ++k)
                {
                    const float v = in[j * 3 + k] + (in[i * 3 + k] - in[j * 3 + k]) * s;
                    out1[m * 3 + k] = v;
                    out2[n * 3 + k] = v;
                }
                ++m;
                ++n;
                // The i'th point goes to the side it is on (a point on the line was added above).
                if (d[i] > 0.f)
                {
                    for (int k = 0; k < 3; ++k)
                    {
                        out1[m * 3 + k] = in[i * 3 + k];
                    }
                    ++m;
                }
                else if (d[i] < 0.f)
                {
                    for (int k = 0; k < 3; ++k)
                    {
                        out2[n * 3 + k] = in[i * 3 + k];
                    }
                    ++n;
                }
            }
            else
            {
                if (d[i] >= 0.f)
                {
                    for (int k = 0; k < 3; ++k)
                    {
                        out1[m * 3 + k] = in[i * 3 + k];
                    }
                    ++m;
                    if (d[i] != 0.f)
                    {
                        continue;
                    }
                }
                for (int k = 0; k < 3; ++k)
                {
                    out2[n * 3 + k] = in[i * 3 + k];
                }
                ++n;
            }
        }
        nout1 = m;
        nout2 = n;
    }

    void addSpan(int x, int y, float zmin, float zmax, uint8_t walk)
    {
        int lo = toUnits(zmin);
        int hi = static_cast<int>(std::ceil((zmax - p.zBase) / p.unitZ));
        hi = std::min(std::max(hi, 0), kMaxUnits);
        if (hi <= lo)
        {
            hi = lo + 1;
        }
        BSpan ns{ lo, hi, walk };
        std::vector<BSpan>& c = mCols[static_cast<size_t>(colIndex(x, y))];

        // Spans that touch the new one join it; the higher top decides whether it is a floor.
        const int mergeThr = std::max(climbUnitsFor(), 1);
        size_t w = 0;
        for (size_t r = 0; r < c.size(); ++r)
        {
            const BSpan cur = c[r];
            if (cur.hi < ns.lo || cur.lo > ns.hi)
            {
                c[w++] = cur;
                continue;
            }
            if (cur.hi > ns.hi + mergeThr)
            {
                ns.walk = cur.walk;
            }
            else if (!(ns.hi > cur.hi + mergeThr))
            {
                ns.walk = static_cast<uint8_t>(ns.walk | cur.walk);
            }
            ns.lo = std::min(ns.lo, cur.lo);
            ns.hi = std::max(ns.hi, cur.hi);
        }
        c.resize(w);
        const auto it = std::lower_bound(c.begin(), c.end(), ns.lo, [](const BSpan& s, int v) { return s.lo < v; });
        c.insert(it, ns);
    }

    int climbUnitsFor() const { return static_cast<int>(std::floor(p.climb / p.unitZ)); }

    // free height between two floors that stand next to each other
    int gap(int a, int b) const
    {
        const Span& sa = spans[static_cast<size_t>(a)];
        const Span& sb = spans[static_cast<size_t>(b)];
        return std::min(sa.ceil, sb.ceil) - std::max(sa.hi, sb.hi);
    }

    // A floor whose neighbour is far below (a ledge) or whose neighbours differ more than
    // a step (too steep) is not walkable (Recast's filterLedgeSpans).
    void filterLedges()
    {
        std::vector<uint8_t> keep(spans.size(), 0);
        for (int y = 0; y < ny; ++y)
        {
            for (int x = 0; x < nx; ++x)
            {
                const int col = colIndex(x, y);
                for (int s = spanBegin(col); s < spanEnd(col); ++s)
                {
                    const Span& sp = spans[static_cast<size_t>(s)];
                    if (!sp.walk)
                    {
                        continue;
                    }
                    const int bot = sp.hi, top = sp.ceil;
                    int minh = kOpenAbove;
                    int asmin = bot, asmax = bot;
                    for (int d = 0; d < 4; ++d)
                    {
                        const int xx = x + kDx[d], yy = y + kDy[d];
                        if (xx < 0 || yy < 0 || xx >= nx || yy >= ny)
                        {
                            minh = std::min(minh, -climbUnits - 1);
                            continue;
                        }
                        const int ncol = colIndex(xx, yy);
                        const int b = spanBegin(ncol), e = spanEnd(ncol);
                        {
                            const int nbot = -climbUnits;
                            const int ntop = b < e ? spans[static_cast<size_t>(b)].lo : kOpenAbove;
                            if (std::min(top, ntop) - std::max(bot, nbot) > ledgeUnits)
                            {
                                minh = std::min(minh, nbot - bot);
                            }
                        }
                        for (int k = b; k < e; ++k)
                        {
                            const int nbot = spans[static_cast<size_t>(k)].hi;
                            const int ntop = spans[static_cast<size_t>(k)].ceil;
                            if (std::min(top, ntop) - std::max(bot, nbot) > ledgeUnits)
                            {
                                minh = std::min(minh, nbot - bot);
                                if (std::abs(nbot - bot) <= climbUnits)
                                {
                                    asmin = std::min(asmin, nbot);
                                    asmax = std::max(asmax, nbot);
                                }
                            }
                        }
                    }
                    if (minh >= -climbUnits && asmax - asmin <= climbUnits)
                    {
                        keep[static_cast<size_t>(s)] = sp.walk;
                    }
                }
            }
        }
        for (size_t s = 0; s < spans.size(); ++s)
        {
            spans[s].walk = keep[s];
        }
    }

    // For each floor, the floor next to it in each direction whose height is the closest
    // within a step, with the free height between them.
    void buildLinks()
    {
        conn.assign(spans.size() * 4, kNoLink);
        for (int y = 0; y < ny; ++y)
        {
            for (int x = 0; x < nx; ++x)
            {
                const int col = colIndex(x, y);
                for (int s = spanBegin(col); s < spanEnd(col); ++s)
                {
                    if (!spans[static_cast<size_t>(s)].walk)
                    {
                        continue;
                    }
                    for (int d = 0; d < 4; ++d)
                    {
                        const int xx = x + kDx[d], yy = y + kDy[d];
                        if (xx < 0 || yy < 0 || xx >= nx || yy >= ny)
                        {
                            continue;
                        }
                        const int ncol = colIndex(xx, yy);
                        int best = kNoLink, bestDiff = 1 << 30;
                        for (int k = spanBegin(ncol); k < spanEnd(ncol); ++k)
                        {
                            if (!spans[static_cast<size_t>(k)].walk)
                            {
                                continue;
                            }
                            const int diff = std::abs(spans[static_cast<size_t>(k)].hi - spans[static_cast<size_t>(s)].hi);
                            if (diff <= climbUnits && gap(s, k) >= headUnits && diff < bestDiff)
                            {
                                best = k;
                                bestDiff = diff;
                            }
                        }
                        conn[static_cast<size_t>(s) * 4 + d] = best;
                    }
                }
            }
        }
    }

    int findSpan(int x, int y, int32_t hi, uint32_t c, const std::vector<uint32_t>& color, const std::vector<uint8_t>& done) const
    {
        const int col = colIndex(x, y);
        for (int s = spanBegin(col); s < spanEnd(col); ++s)
        {
            if (spans[static_cast<size_t>(s)].hi == hi && color[static_cast<size_t>(s)] == c && !done[static_cast<size_t>(s)])
            {
                return s;
            }
        }
        return -1;
    }

    // The floor reached from `s` by a diagonal step (dx, dy) = dirs d1 then d2 or d2 then d1,
    // only when both ways agree, so a corner is never cut.
    int diagonal(const Analysis& a, int s, int d1, int d2) const
    {
        const int n1 = a.link[static_cast<size_t>(s) * 4 + d1];
        const int n2 = a.link[static_cast<size_t>(s) * 4 + d2];
        if (n1 < 0 || n2 < 0 || !allowed(a, n1) || !allowed(a, n2))
        {
            return -1;
        }
        const int t1 = a.link[static_cast<size_t>(n1) * 4 + d2];
        const int t2 = a.link[static_cast<size_t>(n2) * 4 + d1];
        return (t1 >= 0 && t1 == t2 && allowed(a, t1)) ? t1 : -1;
    }

    bool astar(const Analysis& a, int start, int goal, std::vector<int>& out) const
    {
        const size_t n = spans.size();
        std::vector<float> g(n, 1e30f);
        std::vector<int32_t> parent(n, -1);
        std::vector<uint8_t> closed(n, 0);
        using Item = std::pair<float, int>;
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;

        const int gx = cellX(goal), gy = cellY(goal);
        const auto heuristic = [&](int s)
        {
            const float dx = static_cast<float>(std::abs(cellX(s) - gx));
            const float dy = static_cast<float>(std::abs(cellY(s) - gy));
            return (std::max(dx, dy) + 0.41421356f * std::min(dx, dy)) * p.cell;
        };
        // Moving close to an edge costs more, so a route keeps to the middle where it can.
        const auto step = [&](int from, int to, float base)
        {
            const float room = std::max(0.f, a.clear[static_cast<size_t>(to)] - a.shape.radius);
            const float edge = room < 1.5f ? 0.6f * (1.f - room / 1.5f) : 0.f;
            const float rise = 0.5f * std::fabs(floorZ(to) - floorZ(from));
            return base * (1.f + edge) + rise;
        };

        g[static_cast<size_t>(start)] = 0.f;
        open.push({ heuristic(start), start });
        while (!open.empty())
        {
            const int s = open.top().second;
            open.pop();
            if (closed[static_cast<size_t>(s)])
            {
                continue;
            }
            closed[static_cast<size_t>(s)] = 1;
            if (s == goal)
            {
                std::vector<int> rev;
                for (int c = goal; c >= 0; c = parent[static_cast<size_t>(c)])
                {
                    rev.push_back(c);
                }
                out.assign(rev.rbegin(), rev.rend());
                return true;
            }
            const auto relax = [&](int t, float base)
            {
                if (t < 0 || closed[static_cast<size_t>(t)] || !allowed(a, t))
                {
                    return;
                }
                const float ng = g[static_cast<size_t>(s)] + step(s, t, base);
                if (ng < g[static_cast<size_t>(t)])
                {
                    g[static_cast<size_t>(t)] = ng;
                    parent[static_cast<size_t>(t)] = s;
                    open.push({ ng + heuristic(t), t });
                }
            };
            for (int d = 0; d < 4; ++d)
            {
                relax(a.link[static_cast<size_t>(s) * 4 + d], p.cell);
            }
            for (int d = 0; d < 4; ++d)
            {
                relax(diagonal(a, s, d, (d + 1) & 3), p.cell * 1.41421356f);
            }
        }
        return false;
    }

    // Whether a straight walk from floor `sa` to floor `sb` stays on floors the shape can use:
    // sampled finely, and each change of cell has to be a link (a diagonal needs both sides).
    bool lineClear(const Analysis& a, int sa, int sb) const
    {
        const V3 pa = center(sa), pb = center(sb);
        const V3 d = pb - pa;
        const float len = std::sqrt(d.x * d.x + d.y * d.y);
        const int steps = std::max(1, static_cast<int>(std::ceil(len / (p.cell * 0.25f))));
        int cur = sa;
        int cx = cellX(sa), cy = cellY(sa);
        for (int i = 1; i <= steps; ++i)
        {
            const float t = static_cast<float>(i) / static_cast<float>(steps);
            const int nxc = static_cast<int>(std::floor((pa.x + d.x * t) / p.cell));
            const int nyc = static_cast<int>(std::floor((pa.y + d.y * t) / p.cell));
            if (nxc == cx && nyc == cy)
            {
                continue;
            }
            const int ddx = nxc - cx, ddy = nyc - cy;
            if (std::abs(ddx) > 1 || std::abs(ddy) > 1)
            {
                return false;
            }
            int next = -1;
            if (ddx != 0 && ddy != 0)
            {
                const int dirX = ddx < 0 ? 0 : 2, dirY = ddy < 0 ? 3 : 1;
                next = diagonal(a, cur, dirX, dirY);
            }
            else
            {
                const int dir = ddx < 0 ? 0 : ddx > 0 ? 2 : ddy < 0 ? 3 : 1;
                next = a.link[static_cast<size_t>(cur) * 4 + dir];
                if (!allowed(a, next))
                {
                    next = -1;
                }
            }
            if (next < 0)
            {
                return false;
            }
            cur = next;
            cx = nxc;
            cy = nyc;
        }
        return cur == sb;
    }
};

}   // namespace navgrid
}   // namespace pathing
}   // namespace plus

#endif // PLUS_PATHING_NAVGRID_H

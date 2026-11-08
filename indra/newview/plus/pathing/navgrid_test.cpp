// Stand-alone test of navgrid.h (not part of the viewer build):
//   cl /nologo /EHsc /std:c++17 /W4 /WX navgrid_test.cpp && navgrid_test.exe
#include "navgrid.h"

#include <chrono>
#include <cstdio>

using namespace plus::pathing::navgrid;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

// An axis-aligned solid box, outward faces.
static void addBox(Grid& g, float x0, float y0, float z0, float x1, float y1, float z1)
{
    const V3 c[8] = { { x0, y0, z0 }, { x1, y0, z0 }, { x1, y1, z0 }, { x0, y1, z0 },
                      { x0, y0, z1 }, { x1, y0, z1 }, { x1, y1, z1 }, { x0, y1, z1 } };
    const int q[6][4] = { { 4, 5, 6, 7 }, { 3, 2, 1, 0 }, { 0, 1, 5, 4 }, { 1, 2, 6, 5 }, { 2, 3, 7, 6 }, { 3, 0, 4, 7 } };
    for (const auto& f : q)
    {
        g.addTriangle(c[f[0]], c[f[1]], c[f[2]]);
        g.addTriangle(c[f[0]], c[f[2]], c[f[3]]);
    }
}

static void flatGround(Grid& g, float width, float z)
{
    const int n = static_cast<int>(width) + 1;
    std::vector<float> h(static_cast<size_t>(n) * n, z);
    g.addHeightfield(h, n, 1.f);
}

static Grid make(float width)
{
    Grid g;
    Params p;
    p.width = width;
    p.zBase = 0.f;
    g.begin(p);
    return g;
}

static float pathLength(const std::vector<V3>& pts)
{
    float len = 0.f;
    for (size_t i = 1; i < pts.size(); ++i)
    {
        len += length(pts[i] - pts[i - 1]);
    }
    return len;
}

int main()
{
    // 1. flat ground: the middle is a floor, the rim of the grid is a ledge
    {
        Grid g = make(64.f);
        flatGround(g, 64.f, 20.f);
        g.finish();
        const int mid = g.spanBegin(g.colIndex(64, 64));
        CHECK(g.spans[static_cast<size_t>(mid)].walk != 0);
        CHECK(std::fabs(g.floorZ(mid) - 20.f) < 0.15f);
        CHECK(g.spans[static_cast<size_t>(g.spanBegin(g.colIndex(0, 0)))].walk == 0);
        Analysis a;
        g.analyse(Shape{ 0.35f, 1.8f }, a);
        // the middle of a 64 m square is about 32 m from the nearest edge
        CHECK(a.clear[static_cast<size_t>(mid)] > 30.f && a.clear[static_cast<size_t>(mid)] < 33.f);
        // a ray down onto it
        V3 hit;
        CHECK(g.pick({ 10.f, 10.f, 50.f }, { 10.f, 10.f, 0.f }, hit));
        CHECK(std::fabs(hit.z - 20.f) < 0.15f && std::fabs(hit.x - 10.f) < 0.01f);
        CHECK(g.pick({ 5.f, 5.f, 40.f }, { 25.f, 25.f, 10.f }, hit));
        CHECK(!g.pick({ 5.f, 5.f, 10.f }, { 25.f, 25.f, 40.f }, hit));      // goes up: no hit
        CHECK(!g.pick({ 100.f, 5.f, 40.f }, { 120.f, 5.f, 10.f }, hit));    // outside
    }

    // 2. a wall with a gap at one end: the path goes round it
    {
        Grid g = make(64.f);
        flatGround(g, 64.f, 20.f);
        addBox(g, 30.f, 0.f, 20.f, 31.f, 48.f, 24.f);
        g.finish();
        Analysis a;
        g.analyse(Shape{ 0.35f, 1.8f }, a);
        std::vector<V3> path;
        CHECK(g.findPath(a, { 10.f, 32.f, 20.f }, { 50.f, 32.f, 20.f }, path));
        CHECK(path.size() >= 3);
        float maxY = 0.f;
        for (const V3& v : path)
        {
            maxY = std::max(maxY, v.y);
        }
        CHECK(maxY > 47.f);                          // it went through the gap
        CHECK(pathLength(path) > 40.f + 10.f);       // and is longer than the straight way
        CHECK(pathLength(path) < 130.f);

        // the same without the wall is straight
        Grid h = make(64.f);
        flatGround(h, 64.f, 20.f);
        h.finish();
        Analysis b;
        h.analyse(Shape{ 0.35f, 1.8f }, b);
        CHECK(h.findPath(b, { 10.f, 32.f, 20.f }, { 50.f, 32.f, 20.f }, path));
        CHECK(path.size() == 2);
        CHECK(std::fabs(pathLength(path) - 40.f) < 0.01f);
    }

    // 3. a gap one metre wide: a small character goes through, a wide one does not
    {
        Grid g = make(64.f);
        flatGround(g, 64.f, 20.f);
        addBox(g, 30.f, 0.f, 20.f, 31.f, 31.5f, 24.f);
        addBox(g, 30.f, 32.5f, 20.f, 31.f, 64.f, 24.f);
        g.finish();
        Analysis small, wide;
        g.analyse(Shape{ 0.3f, 1.8f }, small);   // 1 m gap, 0.3 m radius
        g.analyse(Shape{ 0.9f, 1.8f }, wide);
        std::vector<V3> path;
        CHECK(g.findPath(small, { 10.f, 32.f, 20.f }, { 50.f, 32.f, 20.f }, path));
        CHECK(!g.findPath(wide, { 10.f, 32.f, 20.f }, { 50.f, 32.f, 20.f }, path));
    }

    // 4. steps: a low one is climbed, a tall one is a wall; its top is an island
    {
        Grid g = make(64.f);
        flatGround(g, 64.f, 20.f);
        addBox(g, 20.f, 20.f, 20.f, 24.f, 24.f, 20.3f);      // step
        addBox(g, 40.f, 20.f, 20.f, 44.f, 24.f, 21.8f);      // block
        g.finish();
        Analysis a;
        g.analyse(Shape{ 0.35f, 1.8f }, a);
        std::vector<V3> path;
        CHECK(g.findPath(a, { 10.f, 22.f, 20.f }, { 22.f, 22.f, 20.3f }, path));    // onto the step
        CHECK(g.findPath(a, { 10.f, 22.f, 20.f }, { 30.f, 22.f, 20.f }, path));
        const int top = g.nearestSpan(a, 42.f, 22.f, 21.8f);
        CHECK(top >= 0 && g.floorZ(top) > 21.5f);
        CHECK(!g.findPath(a, { 42.f, 22.f, 21.8f }, { 10.f, 22.f, 20.f }, path));   // cannot get down
    }

    // 5. a low ceiling: fine for a small character, closed to a tall one
    {
        Grid g = make(64.f);
        flatGround(g, 64.f, 20.f);
        addBox(g, 20.f, 20.f, 21.5f, 28.f, 28.f, 22.f);       // 1.5 m under the slab
        g.finish();
        const int under = g.spanBegin(g.colIndex(48, 48));
        CHECK(g.spans[static_cast<size_t>(under)].walk != 0);            // 1 m headroom is enough for the grid
        Analysis tall, low;
        g.analyse(Shape{ 0.35f, 1.8f }, tall);
        g.analyse(Shape{ 0.35f, 1.2f }, low);
        CHECK(tall.clear[static_cast<size_t>(under)] < 0.f);
        CHECK(low.clear[static_cast<size_t>(under)] > 0.f);
    }

    // 6. the drawing rectangles cover every coloured floor once
    {
        Grid g = make(64.f);
        flatGround(g, 64.f, 20.f);
        addBox(g, 30.f, 0.f, 20.f, 31.f, 48.f, 24.f);
        addBox(g, 10.f, 10.f, 20.f, 14.f, 14.f, 20.3f);
        g.finish();
        Analysis a;
        g.analyse(Shape{ 0.35f, 1.8f }, a);
        std::vector<uint32_t> color(g.spans.size(), 0);
        size_t n = 0;
        for (size_t s = 0; s < color.size(); ++s)
        {
            if (a.clear[s] >= 0.35f)
            {
                color[s] = (g.spans[s].hi % 2) ? 0xff00ff00u : 0xff0000ffu;
                ++n;
            }
        }
        const std::vector<Rect> rects = g.greedyRects(color);
        size_t covered = 0;
        for (const Rect& r : rects)
        {
            covered += static_cast<size_t>(r.x1 - r.x0 + 1) * static_cast<size_t>(r.y1 - r.y0 + 1);
        }
        CHECK(covered == n);
        CHECK(rects.size() < n / 20);          // big flat areas join up
        const std::vector<Seg> segs = g.boundarySegs(color);
        CHECK(!segs.empty());
    }


    // 8. floors per character type, and an exclusion volume
    {
        Grid g = make(64.f);
        flatGround(g, 64.f, 20.f);
        // a platform that is a floor for type 1 only (bits 0 and 1)
        const V3 c[4] = { { 20.f, 20.f, 20.f }, { 28.f, 20.f, 20.f }, { 28.f, 28.f, 20.f }, { 20.f, 28.f, 20.f } };
        (void)c;
        addBox(g, 40.f, 40.f, 20.f, 48.f, 48.f, 20.3f);            // a floor for everything
        g.finish();
        Analysis any, typed;
        g.analyse(Shape{ 0.35f, 1.8f, 0 }, any);
        g.analyse(Shape{ 0.35f, 1.8f, 2 }, typed);
        const int s = g.nearestSpan(any, 44.f, 44.f, 20.3f);
        CHECK(s >= 0 && typed.clear[static_cast<size_t>(s)] >= 0.f);

        Grid h = make(64.f);
        flatGround(h, 64.f, 20.f);
        // solid, but its top is not a floor, and a second one only for type 1
        const V3 a{ 10.f, 10.f, 20.f };
        (void)a;
        h.addTriangle({ 30.f, 30.f, 20.5f }, { 40.f, 30.f, 20.5f }, { 40.f, 40.f, 20.5f }, 0x03);
        h.addTriangle({ 30.f, 30.f, 20.5f }, { 40.f, 40.f, 20.5f }, { 30.f, 40.f, 20.5f }, 0x03);
        const std::function<bool(float, float, float)> ex = [](float x, float, float) { return x > 55.f; };
        h.finish(&ex);
        Analysis one, two;
        h.analyse(Shape{ 0.2f, 1.0f, 1 }, one);
        h.analyse(Shape{ 0.2f, 1.0f, 2 }, two);
        const int t1 = h.spanBegin(h.colIndex(70, 70)) + 1;     // the sheet is the second span (ground, then sheet)
        std::printf("col spans %d, t1 walk %d hi %d\n", h.spanEnd(h.colIndex(70, 70)) - h.spanBegin(h.colIndex(70, 70)), h.spans[static_cast<size_t>(t1)].walk, h.spans[static_cast<size_t>(t1)].hi);
        CHECK(h.spans[static_cast<size_t>(t1)].walk == 0x03);
        CHECK(one.clear[static_cast<size_t>(t1)] >= 0.f);
        CHECK(two.clear[static_cast<size_t>(t1)] < 0.f);
        const int ex1 = h.spanBegin(h.colIndex(120, 60));
        CHECK(h.spans[static_cast<size_t>(ex1)].walk == 0);               // excluded
    }

    // 7. time for a full region: 256 m, with a few thousand boxes
    {
        Grid g = make(256.f);
        const auto t0 = std::chrono::steady_clock::now();
        flatGround(g, 256.f, 20.f);
        for (int i = 0; i < 3000; ++i)
        {
            const float x = static_cast<float>((i * 37) % 240) + 4.f;
            const float y = static_cast<float>((i * 91) % 240) + 4.f;
            addBox(g, x, y, 20.f, x + 2.f, y + 3.f, 23.f);
        }
        g.finish();
        const auto t1 = std::chrono::steady_clock::now();
        Analysis a;
        g.analyse(Shape{ 0.35f, 1.8f }, a);
        const auto t2 = std::chrono::steady_clock::now();
        std::vector<V3> path;
        const int s0 = g.nearestSpan(a, 2.2f, 2.2f, 20.f), s1 = g.nearestSpan(a, 253.f, 253.f, 20.f);
        std::printf("start span %d goal span %d\n", s0, s1);
        const bool ok = g.findPath(a, { 2.2f, 2.2f, 20.f }, { 253.f, 253.f, 20.f }, path);
        const auto t3 = std::chrono::steady_clock::now();
        const auto ms = [](auto a0, auto b0) { return std::chrono::duration<double, std::milli>(b0 - a0).count(); };
        std::printf("256 m region, 3000 boxes: build %.0f ms, analyse %.0f ms, path %.0f ms (%s, %zu points), %zu spans, %zu floors\n",
                    ms(t0, t1), ms(t1, t2), ms(t2, t3), ok ? "found" : "none", path.size(), g.spans.size(), g.walkableCount());
        CHECK(ok);
    }

    if (failures == 0)
    {
        std::printf("navgrid: all passed\n");
    }
    return failures ? 1 : 0;
}

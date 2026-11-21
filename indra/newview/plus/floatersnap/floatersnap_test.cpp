// Stand-alone test of floatersnapmath.h (not part of the viewer build):
//   cl /nologo /EHsc /std:c++17 /W4 /WX floatersnap_test.cpp && floatersnap_test.exe
#include "floatersnapmath.h"

#include <cstdio>
#include <vector>

using namespace plus::floatersnap;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

static Box makeBox(int left, int bottom, int right, int top)
{
    Box b;
    b.left = left;
    b.bottom = bottom;
    b.right = right;
    b.top = top;
    return b;
}

int main()
{
    const Box a = makeBox(0, 0, 100, 100);

    // touching: side by side, stacked, with the gap, a corner alone, overlap, far
    CHECK(touching(a, makeBox(100, 0, 200, 100), 0));           // right of a, exactly together
    CHECK(touching(makeBox(100, 0, 200, 100), a, 0));           // and the other way round
    CHECK(touching(a, makeBox(101, 10, 200, 90), 0));           // 1 apart: the tolerance
    CHECK(!touching(a, makeBox(102, 10, 200, 90), 0));          // 2 apart: not touching without a gap
    CHECK(touching(a, makeBox(104, 10, 200, 90), 3));           // gap 3: up to 4 apart
    CHECK(!touching(a, makeBox(105, 10, 200, 90), 3));
    CHECK(touching(a, makeBox(0, 100, 100, 200), 0));           // above
    CHECK(touching(a, makeBox(50, -100, 150, 0), 0));           // below, shifted
    CHECK(!touching(a, makeBox(100, 100, 200, 200), 0));        // a corner alone
    CHECK(!touching(a, makeBox(100, 85, 200, 185), 0));         // shares 15 < MIN_SHARED
    CHECK(touching(a, makeBox(100, 84, 200, 184), 0));          // shares 16 = MIN_SHARED
    CHECK(!touching(a, makeBox(50, 50, 150, 150), 0));          // overlapping: not next to each other
    CHECK(!touching(a, makeBox(300, 0, 400, 100), 0));          // far
    CHECK(touching(a, makeBox(100, 0, 200, 100), 99));          // a gap setting out of range is clamped (8)
    CHECK(!touching(a, makeBox(110, 0, 200, 100), 99));         // 10 apart: beyond 8 + 1
    CHECK(clampGap(-3) == 0 && clampGap(5) == 5 && clampGap(99) == MAX_GAP);

    // collectGroup: a chain, bridges, the cap, a start out of range, skipped boxes
    {
        std::vector<Box> row;
        for (int i = 0; i < 5; ++i)
        {
            row.push_back(makeBox(i * 100, 0, i * 100 + 100, 100));     // 0..4 touching in a row
        }
        row.push_back(makeBox(0, 500, 100, 600));                       // 5: far away
        int out[MAX_GROUP];
        int n = collectGroup(row.data(), nullptr, (int)row.size(), 2, 0, MAX_GROUP, out);
        CHECK(n == 5 && out[0] == 2);                                   // the start first, the far one not in
        bool seen[6] = {};
        for (int i = 0; i < n; ++i) { seen[out[i]] = true; }
        CHECK(seen[0] && seen[1] && seen[2] && seen[3] && seen[4] && !seen[5]);
        CHECK(out[1] == 1 || out[1] == 3);                              // breadth first: the nearest next
        n = collectGroup(row.data(), nullptr, (int)row.size(), 5, 0, MAX_GROUP, out);
        CHECK(n == 1 && out[0] == 5);                                   // alone
        n = collectGroup(row.data(), nullptr, (int)row.size(), 0, 0, 3, out);
        CHECK(n == 3 && out[0] == 0 && out[1] == 1 && out[2] == 2);     // the cap
        CHECK(collectGroup(row.data(), nullptr, (int)row.size(), -1, 0, MAX_GROUP, out) == 0);
        CHECK(collectGroup(row.data(), nullptr, (int)row.size(), 6, 0, MAX_GROUP, out) == 0);
        CHECK(collectGroup(row.data(), nullptr, (int)row.size(), 0, 0, 0, out) == 0);
        CHECK(collectGroup(nullptr, nullptr, 0, 0, 0, MAX_GROUP, out) == 0);
        bool skip[6] = {};
        skip[2] = true;                                                 // the bridge in the middle is skipped
        n = collectGroup(row.data(), skip, (int)row.size(), 0, 0, MAX_GROUP, out);
        CHECK(n == 2);                                                  // 0 and 1 only
        CHECK(collectGroup(row.data(), skip, (int)row.size(), 2, 0, MAX_GROUP, out) == 0);   // a skipped start
    }
    {
        // many windows in a row: never more than the cap, none twice
        std::vector<Box> row;
        for (int i = 0; i < 40; ++i)
        {
            row.push_back(makeBox(i * 100, 0, i * 100 + 100, 100));
        }
        int out[MAX_GROUP];
        const int n = collectGroup(row.data(), nullptr, (int)row.size(), 0, 0, MAX_GROUP, out);
        CHECK(n == MAX_GROUP);
        for (int i = 0; i < n; ++i)
        {
            for (int k = i + 1; k < n; ++k)
            {
                CHECK(out[i] != out[k]);
            }
        }
    }

    // nearerSnap: not caught (distance 0) is not the same as in line (caught, distance 0)
    {
        const AxisSnap none;
        AxisSnap inline_edge;               // an edge exactly in line with the screen's edge
        inline_edge.hit = true;
        AxisSnap far_edge;                  // another window's edge 7 away
        far_edge.hit = true;
        far_edge.distance = 7;
        AxisSnap near_edge;
        near_edge.hit = true;
        near_edge.distance = -2;
        CHECK(!nearerSnap(none, none).hit);
        CHECK(nearerSnap(none, far_edge).hit && nearerSnap(none, far_edge).distance == 7);
        CHECK(nearerSnap(far_edge, none).distance == 7);
        // the scene of the review: A in line with the screen's edge, B a far snap: A wins, the group does not move
        CHECK(nearerSnap(inline_edge, far_edge).hit && nearerSnap(inline_edge, far_edge).distance == 0);
        CHECK(nearerSnap(far_edge, inline_edge).hit && nearerSnap(far_edge, inline_edge).distance == 0);
        CHECK(nearerSnap(far_edge, near_edge).distance == -2 && nearerSnap(near_edge, far_edge).distance == -2);
        AxisSnap equal_edge;
        equal_edge.hit = true;
        equal_edge.distance = 2;
        CHECK(nearerSnap(near_edge, equal_edge).distance == -2);        // equal: the first
    }

    // nearEnough: reaches out by the snap distance on both axes
    CHECK(nearEnough(a, makeBox(105, 20, 200, 80), 10));                // right of a, 5 apart
    CHECK(nearEnough(a, makeBox(110, 20, 200, 80), 10));                // exactly the reach
    CHECK(!nearEnough(a, makeBox(111, 20, 200, 80), 10));
    CHECK(nearEnough(a, makeBox(-200, 20, -10, 80), 10));               // left of a, exactly the reach
    CHECK(!nearEnough(a, makeBox(-200, 20, -11, 80), 10));
    CHECK(nearEnough(a, makeBox(20, -200, 80, -10), 10));               // below, exactly the reach
    CHECK(!nearEnough(a, makeBox(20, -200, 80, -11), 10));
    CHECK(nearEnough(a, makeBox(20, 110, 80, 200), 10));                // above, exactly the reach
    CHECK(!nearEnough(a, makeBox(20, 111, 80, 200), 10));
    CHECK(!nearEnough(a, makeBox(105, 300, 200, 400), 10));             // far above: not a place to snap to
    CHECK(nearEnough(a, makeBox(105, 105, 200, 200), 10));              // near the corner
    CHECK(nearEnough(a, makeBox(20, 20, 40, 40), 0));                   // inside
    CHECK(!nearEnough(a, makeBox(101, 0, 200, 100), 0));                // reach 0: touching or overlapping only

    // strongestPush: the largest move, the sign kept, the first of equals
    {
        const int none[3] = { 0, 0, 0 };
        CHECK(strongestPush(none, 3) == 0);
        const int mixed[4] = { 2, -7, 5, 0 };
        CHECK(strongestPush(mixed, 4) == -7);
        const int equal[2] = { 4, -4 };
        CHECK(strongestPush(equal, 2) == 4);
        CHECK(strongestPush(mixed, 0) == 0);
    }

    if (failures == 0)
    {
        std::printf("floatersnap_test: all passed\n");
    }
    return failures == 0 ? 0 : 1;
}

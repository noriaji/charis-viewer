// Stand-alone test of fpscornermath.h (not part of the viewer build):
//   cl /nologo /EHsc /std:c++17 /W4 /WX fpscorner_test.cpp && fpscorner_test.exe
#include "fpscornermath.h"

#include <cmath>
#include <cstdio>
#include <limits>

using namespace plus::fpscorner;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

static Box makeBox(int left, int top, int right, int bottom)
{
    Box b;
    b.left = left;
    b.top = top;
    b.right = right;
    b.bottom = bottom;
    return b;
}

static bool inside(const Box& inner, const Box& outer)
{
    return inner.left >= outer.left && inner.right <= outer.right && inner.bottom >= outer.bottom && inner.top <= outer.top;
}

int main()
{
    // the setting: the saved corners keep their values, the status bar is 4 and the default
    CHECK(positionFrom(0) == Position::TopRight);
    CHECK(positionFrom(1) == Position::TopLeft);
    CHECK(positionFrom(2) == Position::BottomLeft);
    CHECK(positionFrom(3) == Position::BottomRight);
    CHECK(positionFrom(4) == Position::StatusBar);
    CHECK(positionFrom(5) == Position::StatusBar);
    CHECK(positionFrom(-1) == Position::StatusBar);
    static_assert(DEFAULT_POSITION == Position::StatusBar, "the status bar is the default");
    CHECK(!isCorner(Position::StatusBar) && !isTop(Position::StatusBar) && !isRight(Position::StatusBar));

    // each corner: the box's size, the margin to the two edges of its corner, inside the area
    const Box area = makeBox(30, 700, 1250, 40); // like the snap region between the toolbars
    const int w = 60, h = 18, m = 6;
    for (int i = 0; i < (int)Position::StatusBar; ++i)
    {
        const Position c = (Position)i;
        CHECK(isCorner(c));
        Box b;
        CHECK(place(area, w, h, c, m, b));
        CHECK(b.right - b.left == w && b.top - b.bottom == h);
        CHECK(inside(b, area));
        CHECK(isRight(c) ? b.right == area.right - m : b.left == area.left + m);
        CHECK(isTop(c) ? b.top == area.top - m : b.bottom == area.bottom + m);
    }
    Box tr;
    CHECK(place(area, w, h, Position::TopRight, m, tr) && tr.left == 1184 && tr.top == 694 && tr.bottom == 676);

    // too small an area, nothing to show, or not a corner: not placed, out untouched
    Box keep = makeBox(1, 2, 3, 4);
    CHECK(!place(makeBox(0, 20, 50, 0), w, h, Position::TopRight, m, keep));
    CHECK(!place(makeBox(0, 25, 500, 0), w, h, Position::BottomLeft, m, keep));
    CHECK(!place(area, 0, h, Position::TopRight, m, keep));
    CHECK(!place(makeBox(100, 50, 50, 100), w, h, Position::TopRight, m, keep)); // an empty intersection
    CHECK(!place(area, w, h, Position::StatusBar, m, keep));
    CHECK(keep.left == 1 && keep.top == 2 && keep.right == 3 && keep.bottom == 4);
    // just big enough
    Box exact;
    CHECK(place(makeBox(0, h + 2 * m, w + 2 * m, 0), w, h, Position::BottomRight, m, exact) && exact.left == m && exact.bottom == m);

    // room in the status bar: the group's official left edge, moved by the shift, pad away from the menus
    CHECK(statusBarHasRoom(700, 400, 50, 8));   // plenty
    CHECK(statusBarHasRoom(458, 400, 50, 8));   // just enough: 458 - 50 = 408 = 400 + 8
    CHECK(!statusBarHasRoom(457, 400, 50, 8));  // one pixel short
    CHECK(!statusBarHasRoom(300, 400, 50, 8));  // the official layout already overlaps the menus
    CHECK(statusBarHasRoom(60, 0, 50, 8));      // no menus (mouselook: the menu bar is hidden)

    // the number
    CHECK(shownFps(0.0) == 0);
    CHECK(shownFps(-5.0) == 0);
    CHECK(shownFps(std::numeric_limits<double>::quiet_NaN()) == 0);
    CHECK(shownFps(59.4) == 59);
    CHECK(shownFps(59.5) == 60);
    CHECK(shownFps(143.9) == 144);
    CHECK(shownFps(1e9) == 9999);
    CHECK(shownFps(std::numeric_limits<double>::infinity()) == 9999);

    if (failures == 0)
    {
        std::printf("fpscorner_test: all passed\n");
    }
    return failures == 0 ? 0 : 1;
}

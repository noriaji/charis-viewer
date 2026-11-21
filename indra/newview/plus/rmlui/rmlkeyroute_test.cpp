// Stand-alone test of rmlkeyroute.h (not part of the viewer build):
//   cl /nologo /EHsc /std:c++17 /W4 /WX rmlkeyroute_test.cpp && rmlkeyroute_test.exe
// research/387 3.1: what a key does in a read-only field. Esc and the function keys are the viewer's (the floater
// closes, F12 is the photo screen): the document must not take them.
#define LL_COMMON_API       // (indra_constants.h declares its LLUUID constants with it; nothing of them is used here)
#include "../../../llcommon/indra_constants.h"
#include "rmlkeyroute.h"

#include <cstdio>

using namespace plus::rmlui;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

int main()
{
    // the viewer's keys
    CHECK(readOnlyKeyRoute(KEY_ESCAPE, MASK_NONE) == KeyRoute::Skip);
    CHECK(readOnlyKeyRoute(KEY_ESCAPE, MASK_SHIFT) == KeyRoute::Skip);
    for (KEY key = KEY_F1; key <= KEY_F12; ++key)
    {
        CHECK(readOnlyKeyRoute(key, MASK_NONE) == KeyRoute::Skip);
        CHECK(readOnlyKeyRoute(key, MASK_CONTROL) == KeyRoute::Skip);
        CHECK(readOnlyKeyRoute(key, MASK_SHIFT) == KeyRoute::Skip);
    }

    // the modifiers alone and Tab go to RmlUi (the focus ring, the selection's growing)
    CHECK(readOnlyKeyRoute(KEY_SHIFT, MASK_SHIFT) == KeyRoute::Send);
    CHECK(readOnlyKeyRoute(KEY_CONTROL, MASK_CONTROL) == KeyRoute::Send);
    CHECK(readOnlyKeyRoute(KEY_ALT, MASK_ALT) == KeyRoute::Send);
    CHECK(readOnlyKeyRoute(KEY_TAB, MASK_NONE) == KeyRoute::Send);
    CHECK(readOnlyKeyRoute(KEY_TAB, MASK_SHIFT) == KeyRoute::Send);

    // select and copy
    CHECK(readOnlyKeyRoute('A', MASK_CONTROL) == KeyRoute::Send);
    CHECK(readOnlyKeyRoute('C', MASK_CONTROL) == KeyRoute::Send);
    CHECK(readOnlyKeyRoute(KEY_LEFT, MASK_SHIFT) == KeyRoute::Send);
    CHECK(readOnlyKeyRoute(KEY_RIGHT, MASK_SHIFT | MASK_CONTROL) == KeyRoute::Send);
    CHECK(readOnlyKeyRoute(KEY_END, MASK_SHIFT) == KeyRoute::Send);
    CHECK(readOnlyKeyRoute(KEY_PAGE_DOWN, MASK_SHIFT) == KeyRoute::Send);
    CHECK(readOnlyKeyRoute(KEY_PAD_UP, MASK_SHIFT) == KeyRoute::Send);

    // scrolling
    CHECK(readOnlyKeyRoute(KEY_UP, MASK_NONE) == KeyRoute::Scroll);
    CHECK(readOnlyKeyRoute(KEY_DOWN, MASK_NONE) == KeyRoute::Scroll);
    CHECK(readOnlyKeyRoute(KEY_PAGE_UP, MASK_NONE) == KeyRoute::Scroll);
    CHECK(readOnlyKeyRoute(KEY_PAGE_DOWN, MASK_NONE) == KeyRoute::Scroll);
    CHECK(readOnlyKeyRoute(KEY_HOME, MASK_NONE) == KeyRoute::Scroll);
    CHECK(readOnlyKeyRoute(KEY_END, MASK_NONE) == KeyRoute::Scroll);
    CHECK(readOnlyKeyRoute(KEY_PAD_PGDN, MASK_NONE) == KeyRoute::Scroll);
    CHECK(readOnlyKeyRoute(KEY_HOME, MASK_CONTROL) == KeyRoute::Scroll);

    // not a read-only field's: the viewer gets them (W walks)
    CHECK(readOnlyKeyRoute('W', MASK_NONE) == KeyRoute::Skip);
    CHECK(readOnlyKeyRoute(KEY_RETURN, MASK_NONE) == KeyRoute::Skip);
    CHECK(readOnlyKeyRoute(KEY_BACKSPACE, MASK_NONE) == KeyRoute::Skip);
    CHECK(readOnlyKeyRoute(KEY_DELETE, MASK_NONE) == KeyRoute::Skip);
    CHECK(readOnlyKeyRoute('X', MASK_CONTROL) == KeyRoute::Skip);
    CHECK(readOnlyKeyRoute('V', MASK_CONTROL) == KeyRoute::Skip);
    CHECK(readOnlyKeyRoute('Z', MASK_CONTROL) == KeyRoute::Skip);
    CHECK(readOnlyKeyRoute(KEY_LEFT, MASK_NONE) == KeyRoute::Skip);
    CHECK(readOnlyKeyRoute(KEY_RIGHT, MASK_NONE) == KeyRoute::Skip);
    CHECK(readOnlyKeyRoute('A', MASK_ALT) == KeyRoute::Skip);
    CHECK(readOnlyKeyRoute(KEY_UP, MASK_ALT) == KeyRoute::Skip);
    CHECK(readOnlyKeyRoute('A', MASK_SHIFT) == KeyRoute::Skip);      // Shift with a letter is typing

    if (failures == 0)
    {
        std::printf("rmlkeyroute_test: OK\n");
    }
    return failures == 0 ? 0 : 1;
}

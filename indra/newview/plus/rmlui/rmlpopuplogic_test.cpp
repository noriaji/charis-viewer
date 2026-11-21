// Stand-alone test of rmlpopuplogic.h (not part of the viewer build):
//   cl /nologo /EHsc /std:c++17 /W4 /WX rmlpopuplogic_test.cpp && rmlpopuplogic_test.exe
#include "rmlpopuplogic.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace plus::rmlui::popuplogic;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

struct Row
{
    int option;
    bool selectable;
};

static std::vector<Row> rows(const char* pattern)     // 'x' can be chosen, '-' cannot; option = index
{
    std::vector<Row> out;
    for (const char* c = pattern; *c; ++c)
    {
        out.push_back(Row{(int)out.size(), *c == 'x'});
    }
    return out;
}

static Box box(int left, int bottom, int right, int top)
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
    // ---- stepRows: the ends and the places with nothing to choose (the review's freeze: all disabled) ----
    const std::vector<Row> none;
    CHECK(stepRows(none, -1, 1) == -1 && stepRows(none, -1, -1) == -1 && stepRows(none, 3, 1) == -1);
    const std::vector<Row> off = rows("---");
    CHECK(stepRows(off, -1, 1) == -1);
    CHECK(stepRows(off, -1, -1) == -1);          // was 3 (= count): typeAhead looped for ever, handleWheel read rows[3]
    CHECK(stepRows(off, 3, -1) == -1);           // End
    CHECK(stepRows(off, 1, 1) == 1);             // a cursor on a row that cannot be chosen stays
    const std::vector<Row> one = rows("x");
    CHECK(stepRows(one, -1, 1) == 0 && stepRows(one, -1, -1) == 0);
    CHECK(stepRows(one, 0, 1) == 0 && stepRows(one, 0, -1) == 0);
    const std::vector<Row> mix = rows("-x-xx-");
    CHECK(stepRows(mix, -1, 1) == 1);            // the first row that can be chosen
    CHECK(stepRows(mix, -1, -1) == 4);           // the last
    CHECK(stepRows(mix, 1, 1) == 3 && stepRows(mix, 3, 1) == 4);
    CHECK(stepRows(mix, 4, 1) == 4);             // the end stops it
    CHECK(stepRows(mix, 1, -1) == 1);
    CHECK(stepRows(mix, 4, -2) == 1);            // two rows up, skipping the one that cannot be chosen
    CHECK(stepRows(mix, 4, -9) == 1);            // a long step stops at the end
    CHECK(stepRows(mix, 3, 0) == 3);
    CHECK(stepRows(mix, 17, 1) == 1 && stepRows(mix, 17, -1) == 4);     // a place outside the list: down from the top, up from the end
    CHECK(stepRows(mix, (int)mix.size(), -1) == 4);                       // End
    CHECK(stepRows(mix, -1, 1) >= 0 && stepRows(mix, -1, 1) < (int)mix.size());

    // ---- rowOfOption ----
    CHECK(rowOfOption(mix, 3) == 3 && rowOfOption(mix, 9) == -1 && rowOfOption(none, 0) == -1);
    {
        std::vector<Row> more = rows("xx");
        more.push_back(Row{-1, false});             // the row that says options were left out has no option
        CHECK(rowOfOption(more, -1) == -1);         // a select with nothing selected is on no row, not on that one
        CHECK(rowOfOption(more, 1) == 1);
    }

    // ---- type-ahead ----
    const std::vector<std::string> names = {"apple", "avocado", "banana", "blueberry", "cherry"};
    const std::vector<Row> all = rows("xxxxx");
    CHECK(findByPrefix(all, names, std::string("b")) == 2);
    CHECK(findByPrefix(all, names, std::string("bl")) == 3);
    CHECK(findByPrefix(all, names, std::string("z")) == -1);
    CHECK(findByPrefix(rows("x-xxx"), names, std::string("a")) == 0);
    CHECK(findByPrefix(rows("-xxxx"), names, std::string("ap")) == -1);   // the row that matches cannot be chosen
    CHECK(findByPrefix(none, std::vector<std::string>(), std::string("a")) == -1);
    CHECK(nextByFirstChar(all, names, 0, 'a') == 1);                      // the same letter again: the next row with it
    CHECK(nextByFirstChar(all, names, 1, 'a') == 0);                      // round the list once: back to the first
    CHECK(nextByFirstChar(all, names, 2, 'b') == 3 && nextByFirstChar(all, names, 3, 'b') == 2);
    CHECK(nextByFirstChar(all, names, 4, 'c') == -1);                     // only the cursor's own row starts with c
    CHECK(nextByFirstChar(all, names, -1, 'c') == 4);                     // no cursor: from the first row
    CHECK(nextByFirstChar(all, names, 5, 'c') == 4);                      // a cursor past the end (the old freeze)
    CHECK(nextByFirstChar(off, std::vector<std::string>{"a", "a", "a"}, 3, 'a') == -1);   // all disabled: it ends
    CHECK(nextByFirstChar(off, std::vector<std::string>{"a", "a", "a"}, -1, 'a') == -1);
    CHECK(nextByFirstChar(none, std::vector<std::string>(), 0, 'a') == -1);
    CHECK(nextByFirstChar(one, std::vector<std::string>{"a"}, 0, 'a') == -1);             // one row: the cursor's own

    // ---- placeList: the area is 0..1000 high, the anchor a select 30 high ----
    PlaceLimits limits;
    limits.scrollbar_width = 6;
    const Box area = box(0, 0, 1920, 1000);
    // it fits below: the top of the list at the anchor's bottom
    Box r = placeList(area, box(100, 500, 300, 530), 150, 200, 20, limits);
    CHECK(r.top == 500 && r.bottom == 300 && r.left == 100 && r.width() == 200);        // never narrower than the select
    // exactly the room below (content == space): below; one more: the side with more room
    r = placeList(area, box(100, 200, 300, 230), 150, 200, 20, limits);
    CHECK(r.top == 200 && r.bottom == 0);
    r = placeList(area, box(100, 199, 300, 229), 150, 200, 20, limits);                 // room below 199, above 771: above
    CHECK(r.bottom == 229 && r.top == 429);
    // the same room below and above (485 each): above, as the XUI showList (its test is a strict >)
    r = placeList(area, box(100, 485, 300, 515), 150, 900, 20, limits);
    CHECK(r.bottom == 515 && r.height() == 485);
    // little room both sides: it shrinks to the larger one and scrolls (the scroll bar widens it)
    r = placeList(box(0, 0, 1920, 300), box(100, 100, 120, 130), 150, 400, 20, limits);   // below 100, above 170: above
    CHECK(r.bottom == 130 && r.height() == 170 && r.width() == 156);
    // taller than the area less the margin: cut at area - 50
    r = placeList(area, box(100, 500, 300, 530), 150, 5000, 20, limits);
    CHECK(r.height() <= 950 && r.bottom >= 0 && r.top <= 1000);
    // the area is smaller than one row: no inverted clamp, the list still lies in the area
    r = placeList(box(0, 0, 500, 10), box(100, 4, 200, 8), 50, 100, 22, limits);
    CHECK(r.height() == 22 && r.bottom >= 0);       // one row is shown whatever the area (the clamp's range is never inverted)
    r = placeList(box(0, 0, 500, 10), box(100, -100, 200, -70), 50, 100, 22, limits);   // room enough, the area is not
    CHECK(r.height() == 22);
    // at the right edge: moved left into the area; at the left: not out of it; wider than the area: starts at its left
    r = placeList(area, box(1900, 500, 1920, 530), 150, 100, 20, limits);
    CHECK(r.right == 1920 && r.left == 1920 - 150);
    r = placeList(box(0, 0, 100, 1000), box(0, 500, 20, 530), 400, 100, 20, limits);
    CHECK(r.left == 0);
    // the width: the text's, at least the select's, at most 500 (more only when the select is wider)
    r = placeList(area, box(0, 500, 100, 530), 900, 100, 20, limits);
    CHECK(r.width() == 500);
    r = placeList(area, box(0, 500, 700, 530), 900, 100, 20, limits);
    CHECK(r.width() == 700);
    // a list whose bottom would go under the area's bottom is lifted
    r = placeList(box(0, 100, 1920, 1000), box(100, 120, 300, 150), 150, 600, 20, limits);   // below 20, above 850: above
    CHECK(r.bottom >= 100);

    if (failures == 0)
    {
        std::printf("rmlpopuplogic: all passed\n");
    }
    return failures == 0 ? 0 : 1;
}

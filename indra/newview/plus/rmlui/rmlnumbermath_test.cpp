// Stand-alone test of rmlnumbermath.h (not part of the viewer build):
//   cl /nologo /EHsc /std:c++17 /W4 /WX rmlnumbermath_test.cpp && rmlnumbermath_test.exe
// research/387 3.5: what the number box reads and writes.
#include "rmlnumbermath.h"

#include <cstdio>

using namespace plus::rmlui;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

static bool parses(const char* text, double expected)
{
    double value = 12345.0;
    return parsePlainNumber(text, value) && value == expected;
}

static bool refuses(const char* text)
{
    double value = 777.0;
    return !parsePlainNumber(text, value) && value == 777.0;     // the out value is left alone
}

int main()
{
    // the plain numbers the box reads
    CHECK(parses("12", 12.0));
    CHECK(parses("-0.5", -0.5));
    CHECK(parses(".5", 0.5));
    CHECK(parses("0", 0.0));
    CHECK(parses("100000", 100000.0));
    CHECK(parses("007", 7.0));

    // what it does not read as a plain number (an expression goes to the calculator instead)
    CHECK(refuses(""));
    CHECK(refuses(" "));
    CHECK(refuses("+5"));
    CHECK(refuses("1e3"));
    CHECK(refuses("inf"));
    CHECK(refuses("nan"));
    CHECK(refuses("--1"));
    CHECK(refuses("1.2.3"));
    CHECK(refuses("12abc"));
    CHECK(refuses("10*3"));
    CHECK(refuses("0x10"));
    CHECK(refuses("1,5"));
    CHECK(refuses("12 "));     // the whole text: spaces are cut off before (normalizeNumberText)

    // formatNumber: decimals digits, a small negative number is a plain zero
    CHECK(formatNumber(12.0, 0) == "12");
    CHECK(formatNumber(12.5, 1) == "12.5");
    CHECK(formatNumber(12.0, 2) == "12.00");
    CHECK(formatNumber(-3.25, 2) == "-3.25");
    CHECK(formatNumber(0.0, 0) == "0");
    CHECK(formatNumber(-0.0, 0) == "0");
    CHECK(formatNumber(-0.001, 0) == "0");
    CHECK(formatNumber(-0.001, 2) == "0.00");
    CHECK(formatNumber(-0.4, 0) == "0");
    CHECK(formatNumber(-0.6, 0) == "-1");
    CHECK(formatNumber(-10.0, 0) == "-10");     // a minus before nonzero digits stays
    CHECK(formatNumber(-0.05, 1) == "-0.1");

    // roundTo
    CHECK(roundTo(1.26, 1) == 1.3);
    CHECK(roundTo(1.24, 1) == 1.2);
    CHECK(roundTo(12.5, 0) == 13.0);
    CHECK(roundTo(-12.5, 0) == -13.0);
    CHECK(roundTo(3.0, 3) == 3.0);

    // the sum of steps stays exact when shown
    CHECK(formatNumber(roundTo(0.1 + 0.2, 1), 1) == "0.3");
    CHECK(formatNumber(roundTo(1.0 / 3.0, 2), 2) == "0.33");

    // foldNumberChar: full-width ASCII, the ideographic space, the minus sign
    CHECK(foldNumberChar(0xFF11) == U'1');
    CHECK(foldNumberChar(0xFF10) == U'0');
    CHECK(foldNumberChar(0xFF19) == U'9');
    CHECK(foldNumberChar(0xFF0E) == U'.');
    CHECK(foldNumberChar(0xFF0D) == U'-');
    CHECK(foldNumberChar(0xFF0A) == U'*');
    CHECK(foldNumberChar(0xFF01) == U'!');
    CHECK(foldNumberChar(0xFF5E) == U'~');
    CHECK(foldNumberChar(0x3000) == U' ');
    CHECK(foldNumberChar(0x2212) == U'-');
    CHECK(foldNumberChar(U'7') == U'7');
    CHECK(foldNumberChar(0xFF00) == 0xFF00);     // just outside the full-width ASCII
    CHECK(foldNumberChar(0xFF5F) == 0xFF5F);
    CHECK(foldNumberChar(0x3042) == 0x3042);     // an ordinary Japanese letter is left (it makes the text no number)

    if (failures == 0)
    {
        std::printf("rmlnumbermath_test: OK\n");
    }
    return failures == 0 ? 0 : 1;
}

// Stand-alone test of rmltextmath.h (not part of the viewer build):
//   cl /nologo /EHsc /std:c++17 /W4 /WX rmltextmath_test.cpp && rmltextmath_test.exe
// research/387 3.1: the text a read-only log field keeps. RmlUi erases CR and TAB from a multi-line field's value
// (WidgetTextInputMultiLine::SanitizeValue) and sends a change event; a text that still has them never equals the
// field's value, so the log's "compare, then set" would set (and send a change) at every call.
#include "rmltextmath.h"

#include <algorithm>
#include <cstdio>
#include <string>

using namespace plus::rmlui;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

// the field's sanitize, as RmlUi does it: both gone
static std::string sanitized(const std::string& value)
{
    std::string out;
    for (char c : value)
    {
        if (c != '\r' && c != '\t')
        {
            out += c;
        }
    }
    return out;
}

// true when the sanitize leaves the text as it is: the "equal" the log's compare needs
static bool settles(const std::string& value)
{
    return sanitized(value) == value;
}

static bool wellFormedUtf8(const std::string& text)
{
    size_t i = 0;
    while (i < text.size())
    {
        const unsigned char c = (unsigned char)text[i];
        const size_t length = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (length == 0 || i + length > text.size())
        {
            return false;
        }
        for (size_t k = 1; k < length; ++k)
        {
            if (((unsigned char)text[i + k] & 0xC0) != 0x80)
            {
                return false;
            }
        }
        i += length;
    }
    return true;
}

int main()
{
    // plain text: nothing to do (the caller uses the text as it is, with no copy)
    CHECK(isPlainLogText("", 10));
    CHECK(isPlainLogText("line 1\nline 2", 100));
    CHECK(isPlainLogText("0123456789", 10));          // exactly the limit
    CHECK(!isPlainLogText("0123456789A", 10));         // one over
    CHECK(!isPlainLogText("a\r\nb", 100));
    CHECK(!isPlainLogText("a\tb", 100));
    CHECK(!isPlainLogText("\t", 100));

    // CR is dropped and TAB is a space: what is put in is what RmlUi keeps
    CHECK(logFieldText("a\r\nb\r\n", 100, false) == "a\nb\n");
    CHECK(logFieldText("a\tb", 100, false) == "a b");
    CHECK(logFieldText("\t\t", 100, true) == "  ");
    CHECK(logFieldText("\r\r\r", 100, true).empty());
    CHECK(logFieldText("plain", 100, false) == "plain");
    // the result settles: RmlUi's sanitize changes nothing in it, so the field's value equals it
    CHECK(settles(logFieldText("x\r\ny\tz\r\n\t", 100, false)));
    CHECK(settles(logFieldText("x\r\ny\tz\r\n\t", 100, true)));
    CHECK(isPlainLogText(logFieldText("x\r\ny\tz\r\n\t", 100, false), 100));

    // the limit: the start, or the end for a log that follows its end
    CHECK(logFieldText("0123456789ABCDEF", 10, false) == "0123456789");
    CHECK(logFieldText("0123456789ABCDEF", 10, true) == "6789ABCDEF");
    CHECK(logFieldText("0123456789", 10, true) == "0123456789");
    CHECK(logFieldText("abc", 0, false).empty());
    CHECK(logFieldText("abc", 0, true).empty());

    // never half of a UTF-8 character at the cut (a Japanese letter is 3 bytes)
    const std::string jp = "\xE3\x81\x82\xE3\x81\x84\xE3\x81\x86\xE3\x81\x88";     // 4 letters, 12 bytes
    for (size_t limit = 0; limit <= 14; ++limit)
    {
        for (int end : { 0, 1 })
        {
            const std::string cut = logFieldText(jp, limit, end != 0);
            CHECK(wellFormedUtf8(cut));
            CHECK(cut.size() <= limit);
            CHECK(cut.size() % 3 == 0);
            CHECK(cut.size() + 3 > std::min(limit, jp.size()));      // loses less than one letter
        }
    }
    CHECK(logFieldText(jp, 7, false) == "\xE3\x81\x82\xE3\x81\x84");
    CHECK(logFieldText(jp, 7, true) == "\xE3\x81\x86\xE3\x81\x88");
    // the limit counts the bytes before the cleaning (it is the memory limit of what is handed in)
    CHECK(logFieldText("a\r\nb\r\nc\r\n", 5, false) == "a\nb");
    CHECK(logFieldText("a\r\nb\r\nc\r\n", 5, true) == "\nc\n");

    if (failures == 0)
    {
        std::printf("rmltextmath_test: OK\n");
    }
    return failures == 0 ? 0 : 1;
}

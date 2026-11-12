/**
 * @file updateversion_test.cpp
 * @brief Stand-alone test of updateversion.cpp and updateoffer.cpp (not part of
 *        the viewer build). boost::json comes from the build's packages (header
 *        only: this file includes its sources); {packages} is that folder
 *        (viewer\build-vc170-64\packages; tools/selftest puts it in):
 *
 *   cl /nologo /std:c++20 /EHsc /W4 /WX /external:W0 /external:I{packages}\include updateversion_test.cpp updateversion.cpp updateoffer.cpp && updateversion_test
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

#include "updateoffer.h"
#include "updateversion.h"

#include <boost/json/src.hpp>   // boost::json's definitions (the viewer has them in llcommon)

#include <cstdio>

using namespace plus::updatecheck;

namespace
{
    int sFailures = 0;

    void check(bool ok, const char* what)
    {
        if (!ok)
        {
            std::printf("FAIL: %s\n", what);
            ++sFailures;
        }
    }

    bool sameVersion(const Version& a, const Version& b)
    {
        return a.major == b.major && a.minor == b.minor && a.patch == b.patch && a.stage_rank == b.stage_rank
               && a.stage_number == b.stage_number && a.build == b.build;
    }

    bool sameOffer(const OfferFile& a, const OfferFile& b)
    {
        return sameVersion(a.version, b.version) && a.has_min_version == b.has_min_version
               && a.min_major == b.min_major && a.min_minor == b.min_minor && a.min_patch == b.min_patch
               && a.download_url == b.download_url && a.notes_url == b.notes_url
               && a.message_ja == b.message_ja && a.message_en == b.message_en;
    }

    // readOfferFile() with the notes counted.
    bool read(std::string_view body, std::string_view hosts, OfferFile& out, size_t* notes_count = nullptr)
    {
        std::string why;
        std::vector<std::string> notes;
        const bool ok = readOfferFile(body, hosts, out, why, notes);
        if (notes_count)
        {
            *notes_count = notes.size();
        }
        return ok;
    }

    Version make(int a, int b, int c, int rank, int number, uint64_t build)
    {
        Version v;
        v.major = a;
        v.minor = b;
        v.patch = c;
        v.stage_rank = rank;
        v.stage_number = number;
        v.build = build;
        return v;
    }
}

int main()
{
    // The installer's version (research/326 section 4.2)
    check(packageVersion(make(0, 1, 0, STAGE_RELEASE, 0, 3300)) == "0.1.0", "package: release, its build left out");
    check(packageVersion(make(0, 1, 0, STAGE_ALPHA, 2, 0)) == "0.1.0-alpha.2", "package: alpha 2 without a build");
    check(packageVersion(make(0, 1, 0, STAGE_ALPHA, 2, 3300)) == "0.1.0-alpha.2.3300", "package: alpha 2 build 3300");
    check(packageVersion(make(0, 1, 0, STAGE_BETA, 0, 0)) == "0.1.0-beta.0", "package: beta without a number");
    check(packageVersion(make(0, 1, 0, STAGE_UNKNOWN, 1, 5)).empty(), "package: unknown stage");
    struct Match
    {
        const char* feed;
        const char* agreed;
        bool        matches;
    };
    const Match matches[] = {
        { "0.1.0-alpha.2",            "0.1.0-alpha.2",      true  },
        { "0.1.0-alpha.2.3300",       "0.1.0-alpha.2",      true  },
        { "0.1.0-beta.0.3400",        "0.1.0-beta.0",       true  },
        { "0.1.0",                    "0.1.0",              true  },
        { "0.1.0-alpha.2.3300",       "0.1.0-alpha.2.3300", true  },
        { "0.1.0-alpha.2.0",          "0.1.0-alpha.2",      true  },   // build 0: not checked here
        { "0.1.0-alpha.2.0033",       "0.1.0-alpha.2",      true  },   // leading zeros: not checked here
        { "0.1.0-alpha.20",           "0.1.0-alpha.2",      false },
        { "0.1.0-alpha.20.1",         "0.1.0-alpha.2",      false },
        { "0.1.0-alpha.2.",           "0.1.0-alpha.2",      false },
        { "0.1.0-alpha.2.3300.1",     "0.1.0-alpha.2",      false },
        { "0.1.0-alpha.2.-1",         "0.1.0-alpha.2",      false },
        { "0.1.0-alpha.2.3300+m",     "0.1.0-alpha.2",      false },
        { "0.1.0-ALPHA.2.3300",       "0.1.0-alpha.2",      false },
        { "0.1.0-alpha.2.\xEF\xBC\x93", "0.1.0-alpha.2",      false },   // a full width digit 3 (UTF-8)
        { "0.1.0",                    "0.1.0-alpha.2",      false },
        { "0.1.0-alpha.2",            "0.1.0",              false },
        { "0.1.0.5",                  "0.1.0",              false },
        { "0.1.0-alpha.2",            "0.1.0-alpha.2.3300", false },
        { "0.1.0-alpha.2.33001",      "0.1.0-alpha.2.3300", false },
        { "",                         "0.1.0-alpha.2",      false },
        { "0.1.0-alpha.2",            "",                   false },
        { "",                         "",                   false },
    };
    for (const Match& m : matches)
    {
        if (packageVersionMatches(m.feed, m.agreed) != m.matches)
        {
            std::printf("FAIL: packageVersionMatches(\"%s\", \"%s\") is not %s\n", m.feed, m.agreed, m.matches ? "true" : "false");
            ++sFailures;
        }
    }

    // Numbers
    int a = -1, b = -1, c = -1;
    check(parseNumbers("0.1.0", a, b, c) && a == 0 && b == 1 && c == 0, "0.1.0");
    check(parseNumbers("12.34.56789", a, b, c) && a == 12 && c == 56789, "12.34.56789");
    check(!parseNumbers("0.1", a, b, c), "two numbers");
    check(!parseNumbers("0.1.0.5", a, b, c), "four numbers");
    check(!parseNumbers("0.1.x", a, b, c), "letter");
    check(!parseNumbers("0..1", a, b, c), "empty number");
    check(!parseNumbers("-1.0.0", a, b, c), "negative");
    check(!parseNumbers(" 0.1.0", a, b, c), "space");
    check(!parseNumbers("", a, b, c), "empty");
    check(!parseNumbers("123456.0.0", a, b, c), "too long");

    // Stages
    check(stageRank("") == STAGE_RELEASE, "release");
    check(stageRank("alpha") == STAGE_ALPHA && stageRank("Alpha") == STAGE_ALPHA, "alpha");
    check(stageRank("beta") == STAGE_BETA, "beta");
    check(stageRank("RC") == STAGE_RC, "rc");
    check(stageRank("gamma") == STAGE_UNKNOWN, "unknown stage");

    // Order: numbers, stage, stage number, build
    const Version alpha1 = make(0, 1, 0, STAGE_ALPHA, 1, 262910911);
    check(compare(make(0, 1, 0, STAGE_ALPHA, 2, 100), alpha1) > 0, "alpha 2 > alpha 1 (build lower)");
    check(compare(make(0, 1, 0, STAGE_BETA, 0, 0), alpha1) > 0, "beta > alpha 1");
    check(compare(make(0, 1, 0, STAGE_RC, 1, 0), make(0, 1, 0, STAGE_BETA, 9, 0)) > 0, "rc > beta");
    check(compare(make(0, 1, 0, STAGE_RELEASE, 0, 0), make(0, 1, 0, STAGE_RC, 3, 0)) > 0, "release > rc");
    check(compare(make(0, 1, 1, STAGE_ALPHA, 1, 0), make(0, 1, 0, STAGE_RELEASE, 0, 0)) > 0, "0.1.1 alpha > 0.1.0");
    check(compare(make(1, 0, 0, STAGE_ALPHA, 1, 0), make(0, 99, 99, STAGE_RELEASE, 0, 0)) > 0, "major first");
    check(compare(make(0, 1, 0, STAGE_ALPHA, 1, 262910912), alpha1) > 0, "same stage, higher build");
    check(compare(make(0, 1, 0, STAGE_ALPHA, 1, 262910910), alpha1) < 0, "same stage, lower build");
    check(compare(make(0, 1, 0, STAGE_ALPHA, 1, 0), alpha1) == 0, "no build: same");
    check(compare(alpha1, alpha1) == 0, "same");
    check(compare(make(0, 0, 9, STAGE_RELEASE, 0, 0), alpha1) < 0, "older numbers");

    // min_version: numbers only
    check(!numbersBelow(alpha1, 0, 1, 0), "0.1.0 alpha is not below 0.1.0");
    check(numbersBelow(alpha1, 0, 1, 1), "0.1.0 below 0.1.1");
    check(numbersBelow(alpha1, 1, 0, 0), "0.1.0 below 1.0.0");
    check(!numbersBelow(alpha1, 0, 0, 9), "0.1.0 not below 0.0.9");

    // Key and text
    check(key(make(0, 1, 0, STAGE_ALPHA, 2, 262911132)) == "0.1.0-alpha.2+262911132", "key alpha");
    check(key(make(1, 2, 3, STAGE_RELEASE, 0, 0)) == "1.2.3", "key release");
    check(displayText(make(0, 1, 0, STAGE_ALPHA, 2, 262911132)) == "0.1.0 alpha 2 (rev 262911132)", "text alpha");
    check(displayText(make(0, 2, 0, STAGE_BETA, 0, 0)) == "0.2.0 beta", "text beta");

    // URLs
    check(isWebURL("https://updates.example.invalid/charis/latest.json"), "https");
    check(isWebURL("http://127.0.0.1:8000/latest.json"), "http 127.0.0.1");
    check(isWebURL("http://localhost:8765/latest.json"), "http localhost");
    check(isWebURL("http://[::1]:8765/latest.json"), "http [::1]");
    check(isWebURL("https://example.com"), "no path");
    check(isWebURL("https://example.com:443/x?a=1&b=%E3%81%82#top"), "port, query, escaped, fragment");
    check(isWebURL("https://example.com?x=1"), "query right after the host");
    check(!isWebURL("http://updates.example.invalid/charis/"), "plain http to another computer");
    check(!isWebURL("http://127.0.0.1.evil.example/"), "http to a host that starts like 127.0.0.1");
    check(!isWebURL("http://localhost@evil.example/"), "http with a user name");
    check(!isWebURL("file:///C:/latest.json"), "file");
    check(!isWebURL("javascript:alert(1)"), "javascript");
    check(!isWebURL("secondlife:///app/teleport"), "secondlife");
    check(!isWebURL("ftp://example.com/"), "ftp");
    check(!isWebURL("https://"), "no host");
    check(!isWebURL("https:///x"), "empty host");
    check(!isWebURL("https://:443/"), "port only");
    check(!isWebURL("https://exa mple.com"), "space");
    check(!isWebURL("https://example.com/\n"), "newline");
    check(!isWebURL("https://example.com/\x7f"), "delete");
    check(!isWebURL(""), "empty url");
    check(!isWebURL("https://updates.example.invalid@evil.example/charis/"), "user name before the host");
    check(!isWebURL("https://user:pass@example.com/"), "user name and password");
    check(!isWebURL("https://evil.example\\@updates.example.invalid/"), "backslash");
    check(!isWebURL("https://example.com/a\\b"), "backslash in the path");
    check(!isWebURL("https://example.com/\xE2\x80\xAE" "exe.fdp"), "U+202E in the path");
    check(!isWebURL("https://ex\xC3\xA4mple.com/"), "non-ASCII host");
    check(!isWebURL("https://example.com/\xE3\x81\x82"), "non-ASCII path");
    check(!isWebURL("https://example.com/\"onclick"), "double quote");
    check(!isWebURL("https://example.com/<x>"), "angle brackets");
    check(!isWebURL("https://exa_mple.com/"), "underscore in the host");
    check(!isWebURL("https://exa%6dple.com/"), "escaped host");
    check(!isWebURL("https://.example.com/"), "host starting with a dot");
    check(!isWebURL("https://example.com:/"), "empty port");
    check(!isWebURL("https://example.com:123456/"), "port too long");
    check(!isWebURL("https://example.com:44a/"), "port with a letter");
    check(!isWebURL("https://[2001:db8::1]/"), "IPv6 other than [::1]");
    check(!isWebURL("https://[::1]x/"), "text after [::1]");
    check(!isWebURL(std::string("https://example.com/") + std::string(2100, 'a')), "too long");

    // Scheme and host in lower case, the rest as it is
    check(webURL("HTTPS://Updates.Example.INVALID/Charis/?A=B") == "https://updates.example.invalid/Charis/?A=B", "lower case scheme and host");
    check(webURL("Http://LocalHost:8765/X") == "http://localhost:8765/X", "lower case http localhost");
    check(webURL("https://updates.example.invalid@evil.example/").empty(), "rejected: empty");

    // Hosts
    check(urlHost("https://Updates.Example.invalid:443/charis/") == "updates.example.invalid", "host without port");
    check(urlHost("http://[::1]:8765/x") == "[::1]", "host [::1]");
    check(urlHost("https://example.com?x") == "example.com", "host before a query");
    check(urlHost("https://a@b.example/").empty(), "no host from a rejected address");
    check(hostInList("updates.example.invalid", "updates.example.invalid"), "host in a list of one");
    check(hostInList("example.com", " updates.example.invalid, Example.COM ;x.example"), "host in a list, any case");
    check(!hostInList("evil.example", "updates.example.invalid, example.com"), "host not in the list");
    check(!hostInList("example.invalid", "updates.example.invalid"), "parent domain not in the list");
    check(!hostInList("updates.example.invalid.evil.example", "updates.example.invalid"), "longer host not in the list");
    check(!hostInList("", "updates.example.invalid"), "empty host");
    check(hostListEmpty("") && hostListEmpty(" ,;\t ") && !hostListEmpty(" github.com "), "empty host list");
    check(numbersText(12, 0, 345) == "12.0.345", "numbers text");
    check(!hostInList("updates.example.invalid", ""), "empty list");
    check(!hostInList("updates.example.invalid", " , ; "), "list of separators");

    // Once a day
    const uint32_t day = 24 * 60 * 60;
    check(dueForCheck(1000000, 0, day), "never checked");
    check(!dueForCheck(1000000 + day - 1, 1000000, day), "within a day");
    check(dueForCheck(1000000 + day, 1000000, day), "a day later");
    check(dueForCheck(999999, 1000000, day), "clock went back");

    // Once an hour after a try that got no answer
    const uint32_t hour = 60 * 60;
    check(!dueForCheck(1000000 + hour - 1, 1000000, hour), "tried within the hour");
    check(dueForCheck(1000000 + hour, 1000000, hour), "an hour after a try");

    // The update file and the copy kept of it (research/325 4.2)
    {
        const char* file =
            "\xEF\xBB\xBF{\"version\":\"0.1.0\",\"stage\":\"Alpha\",\"stage_number\":\"2\",\"build\":262911132,"
            "\"download_url\":\"https://GitHub.com/noriaji/charis-viewer/releases/latest\","
            "\"notes_url\":\"https://raw.githubusercontent.com/x\","
            "\"message_ja\":\"\\u5f71\\u3068\\u6c34\\u9762\",\"message_en\":\"Fixes \\\"quoted\\\"\\n\",\"min_version\":\"0.1.0\","
            "\"unknown\":[1,2,3]}";
        OfferFile offer;
        size_t notes = 0;
        check(read(file, "github.com", offer, &notes), "file read");
        check(sameVersion(offer.version, make(0, 1, 0, STAGE_ALPHA, 2, 262911132)), "file version");
        check(offer.has_min_version && offer.min_major == 0 && offer.min_minor == 1 && offer.min_patch == 0, "file min_version");
        check(offer.download_url == "https://github.com/noriaji/charis-viewer/releases/latest", "download_url kept, host in lower case");
        check(offer.notes_url.empty() && notes == 1, "notes_url on a host not allowed: left out, one note");
        check(offer.message_ja == "\xE5\xBD\xB1\xE3\x81\xA8\xE6\xB0\xB4\xE9\x9D\xA2", "message_ja");
        check(offer.message_en == "Fixes \"quoted\"\n", "message_en");

        // Written out and read back: the same fields, and the same text again.
        const std::string copy = writeOfferFile(offer);
        OfferFile back;
        check(read(copy, "github.com", back) && sameOffer(offer, back), "copy read back the same");
        check(writeOfferFile(back) == copy, "copy written again the same");

        // Read back with other allowed hosts: the address is checked again.
        OfferFile other;
        check(read(copy, "example.com", other, &notes) && other.download_url.empty() && notes == 1, "copy: host checked again");

        // A release, and no optional fields
        OfferFile release;
        release.version = make(1, 2, 3, STAGE_RELEASE, 0, 0);
        OfferFile release_back;
        check(read(writeOfferFile(release), "", release_back) && sameOffer(release, release_back), "release copy");
        check(!release_back.has_min_version && release_back.download_url.empty(), "release copy: nothing extra");

        // Long messages are cut at a whole character; the largest usual copy stays within the limit.
        std::string japanese;
        for (int i = 0; i < 600; ++i)
        {
            japanese += "\xE3\x81\x82";   // 3 bytes each: 1800 bytes
        }
        const std::string long_file = "{\"version\":\"0.2.0\",\"stage\":\"beta\",\"message_ja\":\"" + japanese + "\"}";
        OfferFile cut;
        check(read(long_file, "", cut) && cut.message_ja.size() == 999, "message cut to 999 bytes (333 characters)");
        OfferFile big;
        big.version = make(0, 2, 0, STAGE_BETA, 1, 5);
        big.download_url = "https://github.com/" + std::string(2000, 'a');
        big.notes_url = big.download_url;
        big.message_ja = cut.message_ja;
        big.message_en = std::string(MAX_MESSAGE_BYTES, 'x');
        const std::string big_copy = writeOfferFile(big);
        OfferFile big_back;
        check(big_copy.size() <= MAX_OFFER_BYTES && read(big_copy, "github.com", big_back) && sameOffer(big, big_back),
              "the largest usual copy fits and reads back");

        // What is not an update file
        OfferFile bad;
        check(!read("", "", bad), "empty");
        check(!read("hello", "", bad), "not JSON");
        check(!read("[1]", "", bad), "not an object");
        check(!read("{\"version\":1}", "", bad), "version not a string");
        check(!read("{\"version\":\"0.1\"}", "", bad), "version of two numbers");
        check(!read("{\"version\":\"0.1.0\",\"stage\":\"gamma\"}", "", bad), "unknown stage");
        check(!read("{\"version\":\"0.1.0\",\"stage_number\":3}", "", bad), "release with a stage number");
        check(!read("{\"version\":\"0.1.0\",\"stage\":\"rc\",\"stage_number\":10000}", "", bad), "stage number too big");
        check(!read("{\"version\":\"0.1.0\",\"build\":-1}", "", bad), "negative build");
        check(!read("{\"version\":\"0.1.0\",\"build\":1.5}", "", bad), "build with a fraction");
        check(!read("{\"version\":\"0.1.0\",\"build\":\"12a\"}", "", bad), "build of letters");
        check(!read("{\"version\":\"0.1.0\",\"message_en\":\"\xFF\"}", "", bad), "not UTF-8");
        check(!read("{\"version\":\"0.1.0\" " + std::string(MAX_OFFER_BYTES, ' ') + "}", "", bad), "more than 16 KiB");
        check(read("{\"version\":\"0.1.0\",\"min_version\":\"x\"}", "", bad, &notes) && !bad.has_min_version && notes == 1,
              "min_version of another form: ignored with a note");
        check(read("{\"version\":\"0.1.0\",\"download_url\":\"file:///C:/\"}", "", bad, &notes) && bad.download_url.empty() && notes == 1,
              "download_url not a web URL: left out with a note");
    }

    // UTF-8 cut
    check(truncateUtf8("abc", 3) == "abc", "cut: fits");
    check(truncateUtf8("abcd", 3) == "abc", "cut: ASCII");
    check(truncateUtf8("a\xE3\x81\x82", 3) == "a", "cut: before a character that does not fit");
    check(truncateUtf8("a\xE3\x81\x82", 4) == "a\xE3\x81\x82", "cut: the whole character fits");
    check(truncateUtf8("\xE3\x81\x82", 2).empty(), "cut: nothing left");

    if (sFailures == 0)
    {
        std::printf("all passed\n");
    }
    return sFailures == 0 ? 0 : 1;
}

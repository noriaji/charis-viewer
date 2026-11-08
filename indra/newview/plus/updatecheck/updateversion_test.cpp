/**
 * @file updateversion_test.cpp
 * @brief Stand-alone test of updateversion.cpp (not part of the viewer build).
 *
 *   cl /nologo /std:c++20 /EHsc /W4 updateversion_test.cpp updateversion.cpp && updateversion_test
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

#include "updateversion.h"

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

    if (sFailures == 0)
    {
        std::printf("all passed\n");
    }
    return sFailures == 0 ? 0 : 1;
}

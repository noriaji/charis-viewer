/**
 * @file updateversion.cpp
 * @brief Update check: version numbers (see updateversion.h).
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

#include <algorithm>

namespace plus::updatecheck
{
    namespace
    {
        bool equalsNoCase(std::string_view a, std::string_view b)
        {
            if (a.size() != b.size())
            {
                return false;
            }
            for (size_t i = 0; i < a.size(); ++i)
            {
                char c = a[i];
                if (c >= 'A' && c <= 'Z')
                {
                    c = static_cast<char>(c - 'A' + 'a');
                }
                if (c != b[i])
                {
                    return false;
                }
            }
            return true;
        }

        char lowerChar(char c)
        {
            return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        }

        bool startsWithNoCase(std::string_view text, std::string_view prefix)
        {
            return text.size() >= prefix.size() && equalsNoCase(text.substr(0, prefix.size()), prefix);
        }

        // One number of "0.1.0": 1 to 5 digits.
        bool parseNumber(std::string_view text, int& value)
        {
            if (text.empty() || text.size() > 5)
            {
                return false;
            }
            int n = 0;
            for (char c : text)
            {
                if (c < '0' || c > '9')
                {
                    return false;
                }
                n = n * 10 + (c - '0');
            }
            value = n;
            return true;
        }

        // Between the hosts of a list (hostInList(), hostListEmpty()).
        constexpr std::string_view HOST_SEPARATORS = ", ;\t";

        template <typename T>
        int order(T a, T b)
        {
            return (a < b) ? -1 : ((b < a) ? 1 : 0);
        }

        // webURL() and urlHost(): false when url is not such an address.
        bool parseWebURL(std::string_view url, std::string& normal, std::string& host)
        {
            if (url.empty() || url.size() > 2048)
            {
                return false;
            }
            for (char c : url)
            {
                const unsigned char u = static_cast<unsigned char>(c);
                if (u <= 0x20 || u >= 0x7f || std::string_view("\\\"<>^`{}|").find(c) != std::string_view::npos)
                {
                    return false;
                }
            }

            size_t start = 0;
            bool https = false;
            if (startsWithNoCase(url, "https://"))
            {
                https = true;
                start = 8;
            }
            else if (startsWithNoCase(url, "http://"))
            {
                start = 7;
            }
            else
            {
                return false;
            }

            // The authority ends at the path, the query or the fragment.
            const size_t end = std::min(url.find_first_of("/?#", start), url.size());
            const std::string_view authority = url.substr(start, end - start);
            if (authority.empty() || authority.find('@') != std::string_view::npos)
            {
                return false;   // no host, or a user name before it
            }

            host.clear();
            size_t after_host = 0;
            if (authority[0] == '[')
            {
                // Only the IPv6 address of this computer, for a test server.
                if (!startsWithNoCase(authority, "[::1]"))
                {
                    return false;
                }
                host = "[::1]";
                after_host = 5;
            }
            else
            {
                after_host = std::min(authority.find(':'), authority.size());
                if (after_host == 0)
                {
                    return false;
                }
                for (size_t i = 0; i < after_host; ++i)
                {
                    const char c = authority[i];
                    const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
                    const bool digit = c >= '0' && c <= '9';
                    if (!letter && !digit && c != '.' && c != '-')
                    {
                        return false;
                    }
                    host += lowerChar(c);
                }
                if (host[0] == '.' || host[0] == '-')
                {
                    return false;
                }
            }

            const std::string_view port = authority.substr(after_host);
            if (!port.empty())
            {
                int number = 0;
                if (port[0] != ':' || !parseNumber(port.substr(1), number))
                {
                    return false;
                }
            }

            if (!https && host != "localhost" && host != "127.0.0.1" && host != "[::1]")
            {
                return false;   // plain http only to this computer
            }

            normal = https ? "https://" : "http://";
            normal += host;
            normal.append(port.data(), port.size());
            normal.append(url.data() + end, url.size() - end);
            return true;
        }
    }

    int stageRank(std::string_view stage)
    {
        if (stage.empty())
        {
            return STAGE_RELEASE;
        }
        if (equalsNoCase(stage, "alpha"))
        {
            return STAGE_ALPHA;
        }
        if (equalsNoCase(stage, "beta"))
        {
            return STAGE_BETA;
        }
        if (equalsNoCase(stage, "rc"))
        {
            return STAGE_RC;
        }
        return STAGE_UNKNOWN;
    }

    const char* stageName(int rank)
    {
        switch (rank)
        {
        case STAGE_ALPHA: return "alpha";
        case STAGE_BETA:  return "beta";
        case STAGE_RC:    return "rc";
        default:          return "";
        }
    }

    bool parseNumbers(std::string_view text, int& major, int& minor, int& patch)
    {
        const size_t dot1 = text.find('.');
        if (dot1 == std::string_view::npos)
        {
            return false;
        }
        const size_t dot2 = text.find('.', dot1 + 1);
        if (dot2 == std::string_view::npos)
        {
            return false;
        }
        int a = 0, b = 0, c = 0;
        if (!parseNumber(text.substr(0, dot1), a)
            || !parseNumber(text.substr(dot1 + 1, dot2 - dot1 - 1), b)
            || !parseNumber(text.substr(dot2 + 1), c))   // a third dot fails here
        {
            return false;
        }
        major = a;
        minor = b;
        patch = c;
        return true;
    }

    int compare(const Version& a, const Version& b)
    {
        if (int r = order(a.major, b.major))               return r;
        if (int r = order(a.minor, b.minor))               return r;
        if (int r = order(a.patch, b.patch))               return r;
        if (int r = order(a.stage_rank, b.stage_rank))     return r;
        if (int r = order(a.stage_number, b.stage_number)) return r;
        if (a.build != 0 && b.build != 0)
        {
            return order(a.build, b.build);
        }
        return 0;
    }

    bool numbersBelow(const Version& v, int major, int minor, int patch)
    {
        if (v.major != major)
        {
            return v.major < major;
        }
        if (v.minor != minor)
        {
            return v.minor < minor;
        }
        return v.patch < patch;
    }

    std::string numbersText(int major, int minor, int patch)
    {
        return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
    }

    std::string packageVersion(const Version& v)
    {
        const std::string numbers = numbersText(v.major, v.minor, v.patch);
        if (v.stage_rank == STAGE_RELEASE)
        {
            return numbers;
        }
        const std::string stage = stageName(v.stage_rank);
        if (stage.empty())
        {
            return std::string();
        }
        std::string version = numbers + "-" + stage + "." + std::to_string(v.stage_number);
        if (v.build)
        {
            version += "." + std::to_string(v.build);
        }
        return version;
    }

    bool packageVersionMatches(std::string_view feed, std::string_view agreed)
    {
        if (feed.empty() || agreed.empty())
        {
            return false;
        }
        if (feed == agreed)
        {
            return true;
        }
        const size_t dash = agreed.find('-');
        const bool without_build = dash != std::string_view::npos
            && std::count(agreed.begin() + dash, agreed.end(), '.') == 1;   // "-stage.n": no ".build"
        if (!without_build || feed.size() <= agreed.size() + 1
            || feed.substr(0, agreed.size()) != agreed || feed[agreed.size()] != '.')
        {
            return false;
        }
        return feed.find_first_not_of("0123456789", agreed.size() + 1) == std::string_view::npos;
    }

    std::string key(const Version& v)
    {
        std::string text = numbersText(v.major, v.minor, v.patch);
        if (v.stage_rank != STAGE_RELEASE)
        {
            text += "-";
            text += stageName(v.stage_rank);
            text += "." + std::to_string(v.stage_number);
        }
        if (v.build != 0)
        {
            text += "+" + std::to_string(v.build);
        }
        return text;
    }

    std::string displayText(const Version& v)
    {
        std::string text = numbersText(v.major, v.minor, v.patch);
        if (v.stage_rank != STAGE_RELEASE)
        {
            text += " ";
            text += stageName(v.stage_rank);
            if (v.stage_number > 0)
            {
                text += " " + std::to_string(v.stage_number);
            }
        }
        if (v.build != 0)
        {
            text += " (rev " + std::to_string(v.build) + ")";
        }
        return text;
    }

    std::string webURL(std::string_view url)
    {
        std::string normal, host;
        return parseWebURL(url, normal, host) ? normal : std::string();
    }

    bool isWebURL(std::string_view url)
    {
        return !webURL(url).empty();
    }

    std::string urlHost(std::string_view url)
    {
        std::string normal, host;
        return parseWebURL(url, normal, host) ? host : std::string();
    }

    bool hostInList(std::string_view host, std::string_view list)
    {
        if (host.empty())
        {
            return false;
        }
        size_t pos = 0;
        while (pos < list.size())
        {
            const size_t start = list.find_first_not_of(HOST_SEPARATORS, pos);
            if (start == std::string_view::npos)
            {
                break;
            }
            const size_t end = std::min(list.find_first_of(HOST_SEPARATORS, start), list.size());
            const std::string_view entry = list.substr(start, end - start);
            if (entry.size() == host.size())
            {
                bool same = true;
                for (size_t i = 0; i < entry.size() && same; ++i)
                {
                    same = lowerChar(entry[i]) == lowerChar(host[i]);
                }
                if (same)
                {
                    return true;
                }
            }
            pos = end;
        }
        return false;
    }

    bool hostListEmpty(std::string_view list)
    {
        return list.find_first_not_of(HOST_SEPARATORS) == std::string_view::npos;
    }

    bool dueForCheck(uint32_t now, uint32_t last, uint32_t interval)
    {
        return last == 0 || now < last || now - last >= interval;
    }
}

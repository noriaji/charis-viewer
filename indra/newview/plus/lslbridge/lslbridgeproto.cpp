/**
 * @file lslbridgeproto.cpp
 * @brief The texts of the pose bridge (plus/lslbridge): parsing and checking the bridge's lines, the bodies
 *        posted to it. See lslbridgeproto.h.
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

#include "lslbridgeproto.h"

namespace plus::lslbridge::proto
{
    namespace
    {
        constexpr std::string_view ACTION_NAMES[ACTION_COUNT] = { "request", "accept", "refuse", "cancel", "withdraw" };

        constexpr std::string_view POSE_OPEN      = "<auroraPose>";
        constexpr std::string_view POSE_CLOSE     = "</auroraPose>";
        constexpr std::string_view SHARE_OPEN     = "<charisShare>";
        constexpr std::string_view SHARE_CLOSE    = "</charisShare>";
        constexpr std::string_view REFLECT_OPEN   = "<charisReflect>";
        constexpr std::string_view REFLECT_CLOSE  = "</charisReflect>";
        constexpr std::string_view PUSH_OPEN      = "<charisPush>";
        constexpr std::string_view PUSH_CLOSE     = "</charisPush>";
        constexpr std::string_view ANNOUNCE_OPEN  = "<charisBridge>";
        constexpr std::string_view ANNOUNCE_CLOSE = "</charisBridge>";
        constexpr std::string_view NO_URL_OPEN    = "<charisBridgeNoUrl>";
        constexpr std::string_view NO_URL_CLOSE   = "</charisBridgeNoUrl>";
        constexpr std::string_view BRIDGE_TAG     = "<charisBridge"; // every tag of our script's own lines

        constexpr std::string_view URL_SCHEME     = "https://";
        constexpr std::string_view URL_CAP_PATH   = "/cap/";
        constexpr std::string_view URL_DOMAINS[]  = { ".secondlife.io", ".lindenlab.com" };
        constexpr std::size_t      PORT_DIGITS_MAX = 5;
        constexpr std::size_t      UUID_LENGTH     = 36;

        bool startsWith(std::string_view text, std::string_view head)
        {
            return text.size() >= head.size() && text.compare(0, head.size(), head) == 0;
        }

        bool endsWith(std::string_view text, std::string_view tail)
        {
            return text.size() >= tail.size() && text.compare(text.size() - tail.size(), tail.size(), tail) == 0;
        }

        bool isHex(char c)
        {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        }

        bool isDigit(char c)
        {
            return c >= '0' && c <= '9';
        }

        std::string lowerCase(std::string_view text)
        {
            std::string out(text);
            for (char& c : out)
            {
                if (c >= 'A' && c <= 'Z')
                {
                    c = static_cast<char>(c - 'A' + 'a');
                }
            }
            return out;
        }

        // The inside of "<open>...</close>" when line is exactly that and at most max_bytes; false otherwise.
        bool inside(std::string_view line, std::string_view open, std::string_view close, std::size_t max_bytes,
                    std::string_view& body)
        {
            if (line.size() > max_bytes || line.size() < open.size() + close.size() || !startsWith(line, open) ||
                !endsWith(line, close))
            {
                return false;
            }
            body = line.substr(open.size(), line.size() - open.size() - close.size());
            return true;
        }

        // Splits text at '|' into exactly `count` fields; false for more or fewer.
        template <std::size_t N>
        bool splitFields(std::string_view text, std::string_view (&fields)[N])
        {
            std::size_t start = 0;
            for (std::size_t n = 0; n < N; ++n)
            {
                const std::size_t bar = text.find('|', start);
                const bool        last = (n + 1 == N);
                if (last != (bar == std::string_view::npos))
                {
                    return false; // a field missing, or one too many
                }
                fields[n] = text.substr(start, last ? std::string_view::npos : bar - start);
                start     = bar + 1;
            }
            return true;
        }

        bool isVersion(std::string_view text)
        {
            if (text.empty() || text.size() > VERSION_MAX)
            {
                return false;
            }
            for (char c : text)
            {
                if (!isDigit(c) && c != '.')
                {
                    return false;
                }
            }
            return true;
        }

        bool isHostChar(char c)
        {
            return (c >= 'a' && c <= 'z') || isDigit(c) || c == '-' || c == '.';
        }

        // A label of a host as the log shows it: a word of a domain (letters and hyphens, LABEL_SHOWN_MAX at most) as it
        // is; any other (a UUID, a number, a long one: it could be the token, or name the simulator) by its length.
        constexpr std::size_t LABEL_SHOWN_MAX = 16;

        std::string shownLabel(std::string_view label)
        {
            bool word = !label.empty() && label.size() <= LABEL_SHOWN_MAX;
            for (std::size_t n = 0; word && n < label.size(); ++n)
            {
                const char c = label[n];
                word         = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-';
            }
            return word ? std::string(label) : "[" + std::to_string(label.size()) + " bytes]";
        }
    }

    const char* actionName(int action)
    {
        return (action >= 0 && action < ACTION_COUNT) ? ACTION_NAMES[action].data() : nullptr;
    }

    int actionFromName(std::string_view name)
    {
        for (int n = 0; n < ACTION_COUNT; ++n)
        {
            if (name == ACTION_NAMES[n])
            {
                return n;
            }
        }
        return -1;
    }

    LineKind kindOf(std::string_view line)
    {
        if (startsWith(line, POSE_OPEN))
        {
            return LineKind::Pose;
        }
        if (startsWith(line, ANNOUNCE_OPEN))
        {
            return LineKind::Announcement;
        }
        if (startsWith(line, NO_URL_OPEN))
        {
            return LineKind::NoUrl;
        }
        if (startsWith(line, SHARE_OPEN))
        {
            return LineKind::Share;
        }
        if (startsWith(line, REFLECT_OPEN))
        {
            return LineKind::Reflect;
        }
        if (startsWith(line, PUSH_OPEN))
        {
            return LineKind::Push;
        }
        return startsWith(line, BRIDGE_TAG) ? LineKind::OtherBridge : LineKind::None;
    }

    bool isUuid(std::string_view text)
    {
        if (text.size() != UUID_LENGTH)
        {
            return false;
        }
        bool non_zero = false;
        for (std::size_t n = 0; n < UUID_LENGTH; ++n)
        {
            const char c = text[n];
            if (n == 8 || n == 13 || n == 18 || n == 23)
            {
                if (c != '-')
                {
                    return false;
                }
                continue;
            }
            if (!isHex(c))
            {
                return false;
            }
            non_zero = non_zero || c != '0';
        }
        return non_zero;
    }

    namespace
    {
        // The fields of a pose line (nonce, action, peer) of the tags and the limit given.
        bool parseNonceLine(std::string_view line, std::string_view open, std::string_view close, std::size_t max_bytes,
                            PoseLine& out)
        {
            std::string_view body;
            std::string_view fields[3];
            if (!inside(line, open, close, max_bytes, body) || !splitFields(body, fields))
            {
                return false;
            }
            const int action = actionFromName(fields[1]);
            if (!isUuid(fields[0]) || action < 0 || !isUuid(fields[2]))
            {
                return false;
            }
            out.nonce  = lowerCase(fields[0]);
            out.action = action;
            out.peer   = lowerCase(fields[2]);
            return true;
        }
    }

    bool parseReflectLine(std::string_view line, PoseLine& out)
    {
        return parseNonceLine(line, REFLECT_OPEN, REFLECT_CLOSE, REFLECT_LINE_MAX, out);
    }

    bool parsePoseLine(std::string_view line, PoseLine& out)
    {
        return parseNonceLine(line, POSE_OPEN, POSE_CLOSE, POSE_LINE_MAX, out);
    }


    bool isBase64(std::string_view text)
    {
        if (text.empty() || text.size() % 4 != 0)
        {
            return false;
        }
        std::size_t pads = 0;
        for (std::size_t n = 0; n < text.size(); ++n)
        {
            const char c = text[n];
            if (c == '=')
            {
                if (n + 2 < text.size())
                {
                    return false; // '=' only as the last one or two
                }
                ++pads;
                continue;
            }
            if (pads > 0 || !(isDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '+' || c == '/'))
            {
                return false;
            }
        }
        return true;
    }

    namespace
    {
        // A decimal number of 1 to maxDigits digits, no leading zero but "0": false for anything else.
        bool parseSmallNumber(std::string_view text, std::size_t maxDigits, int& out)
        {
            if (text.empty() || text.size() > maxDigits || (text.size() > 1 && text[0] == '0'))
            {
                return false;
            }
            int value = 0;
            for (const char c : text)
            {
                if (!isDigit(c))
                {
                    return false;
                }
                value = value * 10 + (c - '0');
            }
            out = value;
            return true;
        }

        // A payload: base64 up to SHARE_PAYLOAD_MAX, or empty (the end of a share or a push; only with aud 0).
        bool isPayloadOrEnd(std::string_view payload, int aud)
        {
            if (payload.empty())
            {
                return aud == 0;
            }
            return payload.size() <= SHARE_PAYLOAD_MAX && isBase64(payload);
        }
    }

    bool parseShareLine(std::string_view line, ShareLine& out)
    {
        std::string_view body;
        std::string_view fields[5];
        if (!inside(line, SHARE_OPEN, SHARE_CLOSE, SHARE_LINE_MAX, body) || !splitFields(body, fields))
        {
            return false;
        }
        int aud = 0;
        int seq = 0;
        if (fields[0] != "1" || !isUuid(fields[1]) || !parseSmallNumber(fields[2], 1, aud) || aud > 1 ||
            !parseSmallNumber(fields[3], SHARE_SEQ_DIGITS_MAX, seq) || !isPayloadOrEnd(fields[4], aud))
        {
            return false;
        }
        out.peer    = lowerCase(fields[1]);
        out.aud     = aud;
        out.seq     = seq;
        out.payload = std::string(fields[4]);
        return true;
    }

    bool parsePushLine(std::string_view line, PushLine& out)
    {
        std::string_view body;
        std::string_view fields[4];
        if (!inside(line, PUSH_OPEN, PUSH_CLOSE, PUSH_LINE_MAX, body) || !splitFields(body, fields))
        {
            return false;
        }
        int seq = 0;
        if (fields[0] != "1" || !isUuid(fields[1]) || !parseSmallNumber(fields[2], SHARE_SEQ_DIGITS_MAX, seq) ||
            !isPayloadOrEnd(fields[3], 0))
        {
            return false;
        }
        out.peer    = lowerCase(fields[1]);
        out.seq     = seq;
        out.payload = std::string(fields[3]);
        return true;
    }

    AnnouncementCheck parseAnnouncement(std::string_view line, Announcement& out)
    {
        std::string_view body;
        std::string_view fields[3];
        if (!inside(line, ANNOUNCE_OPEN, ANNOUNCE_CLOSE, ANNOUNCE_LINE_MAX, body) || !splitFields(body, fields))
        {
            return AnnouncementCheck::Form;
        }
        if (!isVersion(fields[0]))
        {
            return AnnouncementCheck::Version;
        }
        if (!isBridgeUrl(fields[1]))
        {
            return AnnouncementCheck::Url;
        }
        if (!isUuid(fields[2]))
        {
            return AnnouncementCheck::Token;
        }
        out.version = std::string(fields[0]);
        out.url     = std::string(fields[1]);
        out.token   = lowerCase(fields[2]);
        return AnnouncementCheck::Ok;
    }

    bool parseNoUrl(std::string_view line, std::string& version)
    {
        std::string_view body;
        if (!inside(line, NO_URL_OPEN, NO_URL_CLOSE, NO_URL_LINE_MAX, body) || !isVersion(body))
        {
            return false;
        }
        version = std::string(body);
        return true;
    }

    std::string describeAnnouncement(std::string_view line)
    {
        std::string out = "bytes " + std::to_string(line.size());
        std::string_view body = line;
        if (startsWith(body, ANNOUNCE_OPEN))
        {
            body.remove_prefix(ANNOUNCE_OPEN.size());
        }
        if (endsWith(body, ANNOUNCE_CLOSE))
        {
            body.remove_suffix(ANNOUNCE_CLOSE.size());
        }
        std::size_t      count = 1;
        std::string_view fields[3];
        std::size_t      start = 0;
        for (std::size_t n = 0;; ++n)
        {
            const std::size_t bar = body.find('|', start);
            if (n < 3)
            {
                fields[n] = body.substr(start, bar == std::string_view::npos ? std::string_view::npos : bar - start);
            }
            if (bar == std::string_view::npos)
            {
                break;
            }
            ++count;
            start = bar + 1;
        }
        out += ", fields " + std::to_string(count) + ", version " + (isVersion(fields[0]) ? "ok" : "not one");

        // The URL: scheme, the end of the host, port, path. A field without "<letters>://" is not looked into at all
        // (it may be the token itself, as in a line of two fields).
        std::string_view  url    = fields[1];
        const std::size_t scheme = url.find("://");
        bool              letters = scheme != std::string_view::npos && scheme > 0 && scheme <= 8;
        for (std::size_t n = 0; letters && n < scheme; ++n)
        {
            const char c = url[n];
            letters      = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        }
        if (!letters)
        {
            out += ", address not a URL (" + std::to_string(url.size()) + " bytes)";
            out += std::string(", token ") + (isUuid(fields[2]) ? "a UUID" : "not a UUID");
            return out;
        }
        out += ", scheme " + lowerCase(url.substr(0, scheme));
        url.remove_prefix(scheme + 3);
        std::size_t host_end = 0;
        while (host_end < url.size() && url[host_end] != ':' && url[host_end] != '/')
        {
            ++host_end;
        }
        const std::string_view host = url.substr(0, host_end);
        std::size_t labels = host.empty() ? 0 : 1;
        std::size_t cut    = std::string_view::npos; // where the last two labels start
        std::size_t dots   = 0;
        for (std::size_t n = host.size(); n-- > 0;)
        {
            if (host[n] == '.')
            {
                ++labels;
                if (++dots == 2)
                {
                    cut = n + 1;
                }
            }
        }
        if (labels >= 3 && cut != std::string_view::npos)
        {
            // Only the domain (the last two labels): what comes before it names the simulator. Each of the two is shown
            // only when it is a word (shownLabel()).
            const std::string_view domain = host.substr(cut);
            const std::size_t      dot    = domain.find('.'); // the one dot between the two
            out += ", host *." + shownLabel(domain.substr(0, dot)) + "." + shownLabel(domain.substr(dot + 1)) + " (" +
                   std::to_string(labels) + " labels)";
        }
        else
        {
            out += ", host of " + std::to_string(labels) + " label(s), " + std::to_string(host.size()) + " bytes";
        }
        url.remove_prefix(host_end);
        std::string port = "none";
        if (!url.empty() && url.front() == ':')
        {
            std::size_t digits = 1;
            while (digits < url.size() && isDigit(url[digits]))
            {
                ++digits;
            }
            port = (digits > 1 && digits - 1 <= PORT_DIGITS_MAX) ? std::string(url.substr(1, digits - 1)) : std::string("?");
            url.remove_prefix(digits);
        }
        out += ", port " + port;
        if (startsWith(url, URL_CAP_PATH))
        {
            out += ", path /cap/ + " + std::to_string(url.size() - URL_CAP_PATH.size()) + " bytes";
        }
        else
        {
            out += ", path not /cap/ (" + std::to_string(url.size()) + " bytes)";
        }
        out += std::string(", token ") + (isUuid(fields[2]) ? "a UUID" : "not a UUID");
        return out;
    }

    bool isBridgeUrl(std::string_view url)
    {
        if (url.size() > URL_MAX || !startsWith(url, URL_SCHEME))
        {
            return false;
        }
        std::string_view rest = url.substr(URL_SCHEME.size());

        // The host: up to the port or the path, lower-case name characters, no empty label.
        std::size_t host_end = 0;
        while (host_end < rest.size() && rest[host_end] != ':' && rest[host_end] != '/')
        {
            if (!isHostChar(rest[host_end]))
            {
                return false;
            }
            ++host_end;
        }
        const std::string_view host = rest.substr(0, host_end);
        if (host.empty() || host.front() == '.' || host.front() == '-' || host.find("..") != std::string_view::npos)
        {
            return false;
        }
        bool known_domain = false;
        for (std::string_view domain : URL_DOMAINS)
        {
            // At least one label before the domain ("x.secondlife.io", not ".secondlife.io" alone).
            known_domain = known_domain || (host.size() > domain.size() && endsWith(host, domain));
        }
        if (!known_domain)
        {
            return false;
        }
        rest = rest.substr(host_end);

        // The port, when there is one.
        if (!rest.empty() && rest.front() == ':')
        {
            std::size_t digits = 1;
            while (digits < rest.size() && isDigit(rest[digits]))
            {
                ++digits;
            }
            if (digits == 1 || digits - 1 > PORT_DIGITS_MAX)
            {
                return false;
            }
            rest = rest.substr(digits);
        }

        // "/cap/<uuid>" and nothing after it.
        return startsWith(rest, URL_CAP_PATH) && isUuid(rest.substr(URL_CAP_PATH.size()));
    }

    bool isOldVersion(std::string_view version)
    {
        return version == "1.0" || version == "1.1";
    }

    std::string probeText(std::string_view objectKey)
    {
        if (!isUuid(objectKey))
        {
            return std::string();
        }
        std::string text(PROBE_TEXT);
        text.push_back('|');
        for (const char c : objectKey)
        {
            text.push_back((c >= 'A' && c <= 'F') ? static_cast<char>(c - 'A' + 'a') : c);
        }
        return text;
    }

    std::string helloBody(std::string_view token)
    {
        std::string body("hello|");
        body.append(token);
        return body;
    }

    std::string poseBody(std::string_view token, int action, std::string_view nonce, std::string_view target)
    {
        const char* name = actionName(action);
        if (!name)
        {
            return std::string(); // the bridge refuses an empty body
        }
        std::string body("pose|");
        body.append(token).append("|").append(name).append("|").append(nonce).append("|").append(target);
        return body;
    }

    std::string reflectBody(std::string_view token, int action, std::string_view nonce, std::string_view target)
    {
        const char* name = actionName(action);
        if (!name)
        {
            return std::string();
        }
        std::string body("reflect|");
        body.append(token).append("|").append(name).append("|").append(nonce).append("|").append(target);
        return body;
    }

    std::string shareBody(std::string_view token, int aud, int seq, std::string_view payload)
    {
        if ((aud != 0 && aud != 1) || seq < 0 || seq > SEQ_MAX || payload.size() > SHARE_PAYLOAD_MAX || !isBase64(payload))
        {
            return std::string();
        }
        std::string body("share|");
        body.append(token).append("|").append(std::to_string(aud)).append("|").append(std::to_string(seq)).append("|")
            .append(payload);
        return body;
    }

    std::string shareEndBody(std::string_view token, int seq)
    {
        if (seq < 0 || seq > SEQ_MAX)
        {
            return std::string();
        }
        std::string body("shareend|");
        body.append(token).append("|").append(std::to_string(seq));
        return body;
    }

    std::string pushBody(std::string_view token, std::string_view target, int seq, std::string_view payload)
    {
        if (seq < 0 || seq > SEQ_MAX || payload.size() > SHARE_PAYLOAD_MAX || !isBase64(payload))
        {
            return std::string();
        }
        std::string body("push|");
        body.append(token).append("|").append(target).append("|").append(std::to_string(seq)).append("|").append(payload);
        return body;
    }

    std::string pushEndBody(std::string_view token, std::string_view target, int seq)
    {
        if (seq < 0 || seq > SEQ_MAX)
        {
            return std::string();
        }
        std::string body("pushend|");
        body.append(token).append("|").append(target).append("|").append(std::to_string(seq));
        return body;
    }
}

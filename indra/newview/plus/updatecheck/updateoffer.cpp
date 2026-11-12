/**
 * @file updateoffer.cpp
 * @brief Update check: the update file and the copy kept of it (see updateoffer.h).
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

#include <boost/json.hpp>

#include <cmath>

namespace plus::updatecheck
{
    namespace
    {
        // A string member; "" when it is missing or not a string.
        std::string stringField(const boost::json::object& obj, const char* name)
        {
            const boost::json::value* value = obj.if_contains(name);
            if (value && value->is_string())
            {
                const boost::json::string& text = value->get_string();
                return std::string(text.data(), text.size());
            }
            return std::string();
        }

        // An optional whole number, not negative: a JSON number or a string of
        // digits. A missing member (or null) leaves out as it is; false when
        // the member is there but not such a number.
        bool numberField(const boost::json::object& obj, const char* name, uint64_t& out)
        {
            const boost::json::value* value = obj.if_contains(name);
            if (!value || value->is_null())
            {
                return true;
            }
            if (value->is_int64())
            {
                if (value->get_int64() < 0)
                {
                    return false;
                }
                out = static_cast<uint64_t>(value->get_int64());
                return true;
            }
            if (value->is_uint64())
            {
                out = value->get_uint64();
                return true;
            }
            if (value->is_double())
            {
                const double d = value->get_double();
                if (!std::isfinite(d) || d < 0.0 || d > 1e15 || std::floor(d) != d)
                {
                    return false;
                }
                out = static_cast<uint64_t>(d);
                return true;
            }
            if (value->is_string())
            {
                const boost::json::string& text = value->get_string();
                if (text.empty() || text.size() > 15)
                {
                    return false;
                }
                uint64_t n = 0;
                for (char c : text)
                {
                    if (c < '0' || c > '9')
                    {
                        return false;
                    }
                    n = n * 10 + static_cast<uint64_t>(c - '0');
                }
                out = n;
                return true;
            }
            return false;
        }

        // A URL member that may be opened later: kept only when it is a web
        // URL (webURL(): https, no user name, ASCII only) on an allowed host.
        std::string urlField(const boost::json::object& obj, const char* name, std::string_view allowed_hosts,
                             std::vector<std::string>& notes)
        {
            const std::string text = stringField(obj, name);
            if (text.empty())
            {
                return text;
            }
            const std::string url = webURL(text);
            if (url.empty())
            {
                notes.push_back("\"" + std::string(name) + "\" is not an https URL that may be opened; left out");
                return url;
            }
            const std::string host = urlHost(url);
            if (!hostInList(host, allowed_hosts))
            {
                notes.push_back("\"" + std::string(name) + "\" is on " + host + ", not on a host of PlusUpdateCheckAllowedHosts ("
                                + std::string(allowed_hosts) + "); left out");
                return std::string();
            }
            return url;
        }
    }

    bool readOfferFile(std::string_view body, std::string_view allowed_hosts, OfferFile& out, std::string& why,
                       std::vector<std::string>& notes)
    {
        if (body.size() > MAX_OFFER_BYTES)
        {
            why = "more than " + std::to_string(MAX_OFFER_BYTES) + " bytes";
            return false;
        }
        // A file saved with a UTF-8 byte order mark (Windows editors, PowerShell) is accepted.
        if (body.size() >= 3 && body.compare(0, 3, "\xEF\xBB\xBF") == 0)
        {
            body.remove_prefix(3);
        }

        boost::system::error_code ec;
        const boost::json::value root = boost::json::parse(body, ec);
        if (ec || !root.is_object())
        {
            why = "not a JSON object";
            return false;
        }
        const boost::json::object& obj = root.get_object();

        OfferFile offer;
        Version& v = offer.version;
        if (!parseNumbers(stringField(obj, "version"), v.major, v.minor, v.patch))
        {
            why = "no \"version\" of the form 0.1.0";
            return false;
        }
        v.stage_rank = stageRank(stringField(obj, "stage"));
        if (v.stage_rank == STAGE_UNKNOWN)
        {
            why = "\"stage\" is not alpha, beta, rc or empty";
            return false;
        }
        uint64_t stage_number = 0;
        if (!numberField(obj, "stage_number", stage_number) || stage_number > 9999)
        {
            why = "\"stage_number\" is not a small whole number";
            return false;
        }
        if (v.stage_rank == STAGE_RELEASE && stage_number != 0)
        {
            // key() leaves a release's stage number out: "skip 0.2.0 (1)" would also skip 0.2.0 (2).
            why = "a release (\"stage\": \"\") has a \"stage_number\"";
            return false;
        }
        v.stage_number = static_cast<int>(stage_number);
        if (!numberField(obj, "build", v.build))
        {
            why = "\"build\" is not a whole number";
            return false;
        }

        const std::string min_version = stringField(obj, "min_version");
        if (!min_version.empty())
        {
            offer.has_min_version = parseNumbers(min_version, offer.min_major, offer.min_minor, offer.min_patch);
            if (!offer.has_min_version)
            {
                notes.push_back("\"min_version\" is not of the form 0.1.0; ignored");
            }
        }

        offer.download_url = urlField(obj, "download_url", allowed_hosts, notes);
        offer.notes_url    = urlField(obj, "notes_url", allowed_hosts, notes);
        offer.message_ja   = truncateUtf8(stringField(obj, "message_ja"), MAX_MESSAGE_BYTES);
        offer.message_en   = truncateUtf8(stringField(obj, "message_en"), MAX_MESSAGE_BYTES);

        out = std::move(offer);
        return true;
    }

    std::string writeOfferFile(const OfferFile& offer)
    {
        const Version& v = offer.version;
        boost::json::object obj;
        obj["version"]      = numbersText(v.major, v.minor, v.patch);
        obj["stage"]        = stageName(v.stage_rank);
        obj["stage_number"] = v.stage_number;
        obj["build"]        = v.build;
        if (offer.has_min_version)
        {
            obj["min_version"] = numbersText(offer.min_major, offer.min_minor, offer.min_patch);
        }
        if (!offer.download_url.empty())
        {
            obj["download_url"] = offer.download_url;
        }
        if (!offer.notes_url.empty())
        {
            obj["notes_url"] = offer.notes_url;
        }
        if (!offer.message_ja.empty())
        {
            obj["message_ja"] = offer.message_ja;
        }
        if (!offer.message_en.empty())
        {
            obj["message_en"] = offer.message_en;
        }
        return boost::json::serialize(obj);
    }

    std::string truncateUtf8(std::string_view text, size_t max_bytes)
    {
        if (text.size() <= max_bytes)
        {
            return std::string(text);
        }
        size_t cut = max_bytes;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
        {
            --cut;   // text[cut] continues a character: cut before that character
        }
        return std::string(text.substr(0, cut));
    }
}

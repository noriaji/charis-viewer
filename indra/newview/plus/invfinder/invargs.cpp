/**
 * @file invargs.cpp
 * @brief JSON arguments of the inventory finder tools to a Query, and a Result back to JSON.
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

#include "invargs.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace json = boost::json;

namespace plus::invfinder
{
    namespace
    {
        constexpr std::int64_t kMaxCount = 1000000;

        const json::value* find(const json::object& o, const char* key)
        {
            const json::value* v = o.if_contains(key);
            return (v && !v->is_null()) ? v : nullptr;
        }

        bool readString(const json::value& v, const char* name, std::string& out, std::string& error)
        {
            if (!v.is_string())
            {
                error = std::string("\"") + name + "\" must be a string.";
                return false;
            }
            out = std::string(v.get_string());
            return true;
        }

        // A string or an array of strings.
        bool readStringList(const json::value& v, const char* name, std::vector<std::string>& out, std::string& error)
        {
            if (v.is_string())
            {
                out.emplace_back(v.get_string());
                return true;
            }
            if (v.is_array())
            {
                for (const json::value& e : v.get_array())
                {
                    if (!e.is_string())
                    {
                        error = std::string("\"") + name + "\" must be a list of strings.";
                        return false;
                    }
                    out.emplace_back(e.get_string());
                }
                return true;
            }
            error = std::string("\"") + name + "\" must be a string or a list of strings.";
            return false;
        }

        bool readInt(const json::value& v, const char* name, std::int64_t& out, std::string& error)
        {
            if (v.is_int64())
                out = v.get_int64();
            else if (v.is_uint64())
                out = static_cast<std::int64_t>(std::min<std::uint64_t>(v.get_uint64(), kMaxCount));
            else if (v.is_double() && std::isfinite(v.get_double()))
                // Clamped as a double first: 1e30 does not fit an int64 (the cast would be undefined).
                out = static_cast<std::int64_t>(std::clamp(v.get_double(), -static_cast<double>(kMaxCount), static_cast<double>(kMaxCount)));
            else
            {
                error = std::string("\"") + name + "\" must be a number.";
                return false;
            }
            out = std::clamp<std::int64_t>(out, -kMaxCount, kMaxCount);
            return true;
        }

        std::string lower(std::string s)
        {
            for (char& c : s)
            {
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c + 32);
            }
            return s;
        }

        // Words models tend to use, to the names of the inventory's asset types.
        void addType(const std::string& word, std::vector<std::string>& out)
        {
            const std::string w = lower(word);
            if (w == "wearable" || w == "wearables")
            {
                out.push_back("clothing");
                out.push_back("bodypart");
            }
            else if (w == "script" || w == "scripts")
                out.push_back("lsltext");
            else if (w == "animation" || w == "animations")
                out.push_back("animatn");
            else if (w == "callingcard" || w == "calling card")
                out.push_back("callcard");
            else if (w == "attachment" || w == "attachments" || w == "objects")
                out.push_back("object");
            else if (w == "body part" || w == "bodyparts")
                out.push_back("bodypart");
            else if (w == "textures" || w == "image")
                out.push_back("texture");
            else if (w == "notecards")
                out.push_back("notecard");
            else if (w == "sounds")
                out.push_back("sound");
            else
                out.push_back(w);
        }

        bool looksLikeUuid(const std::string& s)
        {
            if (s.size() != 36)
                return false;
            for (std::size_t i = 0; i < s.size(); ++i)
            {
                const char c = s[i];
                const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
                if (dash ? c != '-' : !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
                    return false;
            }
            return true;
        }

        // Two decimals. Formatted and read back: with /fp:fast a division by 100
        // becomes a multiplication by 0.01 and 3.8 comes out as 3.8000000000000003.
        double round2(double v)
        {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.2f", v);
            return std::strtod(buf, nullptr);
        }

        json::array groupIndexes(std::uint64_t mask)
        {
            json::array out;
            for (int i = 0; i < 64; ++i)
            {
                if (mask & (std::uint64_t(1) << i))
                    out.emplace_back(i);
            }
            return out;
        }

        json::array countsToJson(const std::vector<Count>& counts, const NameOf* names)
        {
            json::array out;
            for (const Count& c : counts)
            {
                json::object o;
                if (names && *names)
                {
                    o["id"] = c.key;
                    const std::string name = (*names)(c.key);
                    o["name"] = name.empty() ? c.key : name;
                }
                else if (names)
                {
                    o["id"] = c.key;
                }
                else
                {
                    o["key"] = c.key.empty() ? std::string("(none)") : c.key;
                }
                o["count"] = c.count;
                out.emplace_back(std::move(o));
            }
            return out;
        }
    }

    bool parseDate(const std::string& text, std::int64_t& unixTime)
    {
        // exactly YYYY-MM-DD
        if (text.size() != 10 || text[4] != '-' || text[7] != '-')
            return false;
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            if (i != 4 && i != 7 && (text[i] < '0' || text[i] > '9'))
                return false;
        }
        const int y = std::stoi(text.substr(0, 4));
        const int m = std::stoi(text.substr(5, 2));
        const int d = std::stoi(text.substr(8, 2));
        if (y < 1970 || y > 9999 || m < 1 || m > 12 || d < 1)
            return false;
        static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
        if (d > kDays[m - 1] + ((m == 2 && leap) ? 1 : 0))
            return false;
        // days from civil (Howard Hinnant)
        const int yy = m <= 2 ? y - 1 : y;
        const int era = (yy >= 0 ? yy : yy - 399) / 400;
        const int yoe = yy - era * 400;
        const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        const std::int64_t days = static_cast<std::int64_t>(era) * 146097 + doe - 719468;
        unixTime = days * 86400;
        return true;
    }

    std::string formatDate(std::int64_t unixTime)
    {
        std::int64_t z = unixTime / 86400;
        if (unixTime % 86400 < 0)
            --z;
        z += 719468;
        const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
        const std::int64_t doe = z - era * 146097;
        const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        std::int64_t y = yoe + era * 400;
        const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        const std::int64_t mp = (5 * doy + 2) / 153;
        const int d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
        const int m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
        if (m <= 2)
            ++y;
        char buf[32];
        std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", static_cast<int>(y), m, d);
        return buf;
    }

    bool parseSearchArgs(const json::object& args, SearchRequest& out, std::string& error)
    {
        static const char* const kKnown[] = {"groups", "min_groups", "exclude", "target", "where", "types", "subtypes",
                                             "under", "creator", "acquired_after", "acquired_before", "permissions",
                                             "scope", "include_links", "group_by", "limit", "offset", "show"};
        for (const auto& kv : args)
        {
            const std::string key(kv.key());
            if (std::find(std::begin(kKnown), std::end(kKnown), key) == std::end(kKnown))
            {
                error = "Unknown argument \"" + key + "\". Known: groups, min_groups, exclude, target, where, types, "
                        "subtypes, under, creator, acquired_after, acquired_before, permissions, scope, include_links, "
                        "group_by, limit, offset, show.";
                return false;
            }
        }

        Query& q = out.query;
        if (const json::value* v = find(args, "groups"))
        {
            if (v->is_string())
            {
                q.groups.push_back({std::string(v->get_string())});
            }
            else if (v->is_array())
            {
                for (const json::value& e : v->get_array())
                {
                    std::vector<std::string> words;
                    if (e.is_string() || e.is_array())
                    {
                        if (!readStringList(e, "groups", words, error))
                            return false;
                    }
                    else
                    {
                        error = "\"groups\" must be a list of word lists, e.g. [[\"christmas\",\"xmas\"],[\"dress\"]].";
                        return false;
                    }
                    q.groups.push_back(std::move(words));
                }
            }
            else
            {
                error = "\"groups\" must be a list of word lists, e.g. [[\"christmas\",\"xmas\"],[\"dress\"]].";
                return false;
            }
            if (q.groups.size() > 64)
            {
                error = "\"groups\" can have at most 64 word lists.";
                return false;
            }
        }

        std::int64_t n = 0;
        if (const json::value* v = find(args, "min_groups"))
        {
            if (!readInt(*v, "min_groups", n, error))
                return false;
            q.min_groups = static_cast<int>(std::clamp<std::int64_t>(n, 1, 64));
        }
        if (const json::value* v = find(args, "exclude"))
        {
            if (!readStringList(*v, "exclude", q.exclude, error))
                return false;
        }

        if (const json::value* v = find(args, "target"))
        {
            std::string s;
            if (!readString(*v, "target", s, error))
                return false;
            s = lower(s);
            if (s == "folders")
                q.target = Target::Folders;
            else if (s == "items")
                q.target = Target::Items;
            else if (s == "both")
                q.target = Target::Both;
            else
            {
                error = "\"target\" must be \"folders\", \"items\" or \"both\".";
                return false;
            }
        }

        if (const json::value* v = find(args, "where"))
        {
            std::vector<std::string> list;
            if (!readStringList(*v, "where", list, error))
                return false;
            std::uint8_t mask = 0;
            for (const std::string& w : list)
            {
                const std::string s = lower(w);
                if (s == "name")
                    mask |= WHERE_NAME;
                else if (s == "path")
                    mask |= WHERE_PATH;
                else if (s == "description" || s == "desc")
                    mask |= WHERE_DESC;
                else
                {
                    error = "\"where\" entries must be \"name\", \"path\" or \"description\".";
                    return false;
                }
            }
            q.where = mask;
        }

        if (const json::value* v = find(args, "types"))
        {
            std::vector<std::string> list;
            if (!readStringList(*v, "types", list, error))
                return false;
            for (const std::string& t : list)
                addType(t, q.types);
        }
        if (const json::value* v = find(args, "subtypes"))
        {
            if (!readStringList(*v, "subtypes", q.subtypes, error))
                return false;
        }
        if (const json::value* v = find(args, "under"))
        {
            if (!readString(*v, "under", q.under, error))
                return false;
        }
        if (const json::value* v = find(args, "creator"))
        {
            std::string s;
            if (!readString(*v, "creator", s, error))
                return false;
            if (looksLikeUuid(s))
                q.creator_ids.push_back(lower(s));
            else
                out.creator_name = s;
        }

        if (const json::value* v = find(args, "acquired_after"))
        {
            std::string s;
            if (!readString(*v, "acquired_after", s, error))
                return false;
            if (!parseDate(s, q.acquired_after))
            {
                error = "\"acquired_after\" must be a date like 2026-10-17.";
                return false;
            }
        }
        if (const json::value* v = find(args, "acquired_before"))
        {
            std::string s;
            if (!readString(*v, "acquired_before", s, error))
                return false;
            std::int64_t t = 0;
            if (!parseDate(s, t))
            {
                error = "\"acquired_before\" must be a date like 2026-10-17.";
                return false;
            }
            q.acquired_before = t + 86399; // the whole day
        }

        if (const json::value* v = find(args, "permissions"))
        {
            std::vector<std::string> list;
            if (!readStringList(*v, "permissions", list, error))
                return false;
            for (const std::string& p : list)
            {
                const std::string s = lower(p);
                if (s == "copy")
                    q.perms |= PERM_COPY;
                else if (s == "modify" || s == "mod")
                    q.perms |= PERM_MODIFY;
                else if (s == "transfer" || s == "trans")
                    q.perms |= PERM_TRANSFER;
                else
                {
                    error = "\"permissions\" entries must be \"copy\", \"modify\" or \"transfer\".";
                    return false;
                }
            }
        }

        if (const json::value* v = find(args, "scope"))
        {
            std::vector<std::string> list;
            if (!readStringList(*v, "scope", list, error))
                return false;
            std::uint8_t mask = 0;
            for (const std::string& a : list)
            {
                const std::string s = lower(a);
                if (s == "mine")
                    mask |= AREA_MINE;
                else if (s == "library")
                    mask |= AREA_LIBRARY;
                else if (s == "trash")
                    mask |= AREA_TRASH;
                else if (s == "outfit" || s == "current_outfit")
                    mask |= AREA_OUTFIT;
                else if (s == "all")
                    mask |= AREA_MINE | AREA_LIBRARY | AREA_TRASH | AREA_OUTFIT;
                else
                {
                    error = "\"scope\" entries must be \"mine\", \"library\", \"trash\", \"outfit\" or \"all\".";
                    return false;
                }
            }
            if (mask)
                q.areas = mask;
        }

        if (const json::value* v = find(args, "include_links"))
        {
            if (!v->is_bool())
            {
                error = "\"include_links\" must be true or false.";
                return false;
            }
            q.include_links = v->get_bool();
        }
        if (const json::value* v = find(args, "group_by"))
        {
            std::string s;
            if (!readString(*v, "group_by", s, error))
                return false;
            s = lower(s);
            if (s == "folder")
                q.group_by = GroupBy::Folder;
            else if (s == "none")
                q.group_by = GroupBy::None;
            else
            {
                error = "\"group_by\" must be \"folder\" or \"none\".";
                return false;
            }
        }
        if (const json::value* v = find(args, "limit"))
        {
            if (!readInt(*v, "limit", n, error))
                return false;
            q.limit = static_cast<std::size_t>(std::clamp<std::int64_t>(n, 1, 100));
        }
        if (const json::value* v = find(args, "offset"))
        {
            if (!readInt(*v, "offset", n, error))
                return false;
            q.offset = static_cast<std::size_t>(std::max<std::int64_t>(n, 0));
        }
        if (const json::value* v = find(args, "show"))
        {
            if (!v->is_bool())
            {
                error = "\"show\" must be true or false.";
                return false;
            }
            out.show = v->get_bool();
            q.collect_ids = out.show;
        }
        return true;
    }

    bool parseShowArgs(const json::object& args, ShowRequest& out, std::string& error)
    {
        constexpr std::size_t kMaxIds = 20000;
        for (const auto& kv : args)
        {
            const std::string key(kv.key());
            if (key != "ids" && key != "clear")
            {
                error = "Unknown argument \"" + key + "\". Known: ids, clear.";
                return false;
            }
        }
        if (const json::value* v = find(args, "clear"))
        {
            if (!v->is_bool())
            {
                error = "\"clear\" must be true or false.";
                return false;
            }
            out.clear = v->get_bool();
        }
        if (const json::value* v = find(args, "ids"))
        {
            std::vector<std::string> list;
            if (!readStringList(*v, "ids", list, error))
                return false;
            if (list.size() > kMaxIds)
            {
                error = "\"ids\" can have at most 20000 entries.";
                return false;
            }
            for (const std::string& id : list)
            {
                if (!looksLikeUuid(id))
                {
                    error = "\"ids\" entries must be inventory ids like 3f2504e0-4f89-11d3-9a0c-0305e82c3301, got \"" + id + "\".";
                    return false;
                }
                out.ids.push_back(lower(id));
            }
        }
        if (!out.clear && out.ids.empty())
        {
            error = "Give \"ids\" (the ids from inventory_search) or \"clear\": true.";
            return false;
        }
        return true;
    }

    json::object hitToJson(const Hit& hit, const NameOf& creatorName)
    {
        json::object o;
        o["id"] = hit.id;
        o["name"] = hit.name;
        o["kind"] = hit.is_folder ? "folder" : "item";
        if (!hit.type.empty())
            o["type"] = hit.type;
        if (!hit.subtype.empty())
            o["subtype"] = hit.subtype;
        if (!hit.path.empty())
            o["path"] = hit.path;
        if (!hit.creator.empty())
        {
            const std::string name = creatorName ? creatorName(hit.creator) : std::string();
            o["creator"] = name.empty() ? hit.creator : name;
        }
        if (hit.acquired > 0)
            o["acquired"] = formatDate(hit.acquired);
        if (!hit.is_folder)
        {
            std::string perms = "---";
            if (hit.perms & PERM_COPY)
                perms[0] = 'c';
            if (hit.perms & PERM_MODIFY)
                perms[1] = 'm';
            if (hit.perms & PERM_TRANSFER)
                perms[2] = 't';
            o["perms"] = perms;
        }
        if (hit.is_link)
            o["link"] = true;
        o["score"] = round2(hit.score);
        if (hit.matched_groups)
            o["matched_groups"] = groupIndexes(hit.matched_groups);
        if (hit.matched_where)
        {
            json::array where;
            if (hit.matched_where & WHERE_NAME)
                where.emplace_back("name");
            if (hit.matched_where & WHERE_PATH)
                where.emplace_back("path");
            if (hit.matched_where & WHERE_DESC)
                where.emplace_back("description");
            o["matched_in"] = std::move(where);
        }
        return o;
    }

    json::object searchResultToJson(const Result& result, const NameOf& creatorName)
    {
        json::object o;
        o["matches"] = result.total_matches;
        if (result.grouped)
        {
            o["folders_total"] = result.total_groups;
            json::array groups;
            for (const Group& g : result.groups)
            {
                json::object go;
                if (!g.folder_id.empty())
                {
                    json::object folder;
                    folder["id"] = g.folder_id;
                    folder["name"] = g.folder_name;
                    folder["path"] = g.folder_path;
                    go["folder"] = std::move(folder);
                }
                go["folder_name_matched"] = g.folder_matched;
                go["items"] = g.item_count;
                go["score"] = round2(g.score);
                go["matched_groups"] = groupIndexes(g.matched_groups);
                json::array samples;
                for (const Hit& h : g.samples)
                    samples.emplace_back(hitToJson(h, creatorName));
                go["samples"] = std::move(samples);
                groups.emplace_back(std::move(go));
            }
            o["groups"] = std::move(groups);
        }
        else
        {
            json::array hits;
            for (const Hit& h : result.hits)
                hits.emplace_back(hitToJson(h, creatorName));
            o["hits"] = std::move(hits);
        }
        if (result.next_offset >= 0)
            o["next_offset"] = result.next_offset;

        json::object facets;
        facets["types"] = countsToJson(result.types, nullptr);
        facets["creators"] = countsToJson(result.creators, &creatorName);
        o["facets"] = std::move(facets);
        return o;
    }
}

/**
 * @file invindex.cpp
 * @brief In-memory search index over inventory items and folders (no viewer state).
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

#include "invindex.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <string_view>

namespace plus::invfinder
{
    namespace
    {
        constexpr std::size_t kNpos = std::string::npos;
        constexpr std::size_t kNoSlot = static_cast<std::size_t>(-1);
        constexpr std::size_t kMaxDepth = 64;
        constexpr std::size_t kMaxLimit = 100;
        constexpr std::size_t kSamples = 3;
        constexpr std::size_t kFacetTop = 8;

        // ---- UTF-8 ----------------------------------------------------------

        // Reads one code point. On bad input returns false and consumes one byte.
        bool nextCodePoint(std::string_view s, std::size_t& i, char32_t& cp)
        {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            if (c < 0x80)
            {
                cp = c;
                ++i;
                return true;
            }
            std::size_t extra;
            char32_t v;
            char32_t least;
            if ((c & 0xE0) == 0xC0)
            {
                extra = 1;
                v = c & 0x1F;
                least = 0x80;
            }
            else if ((c & 0xF0) == 0xE0)
            {
                extra = 2;
                v = c & 0x0F;
                least = 0x800;
            }
            else if ((c & 0xF8) == 0xF0)
            {
                extra = 3;
                v = c & 0x07;
                least = 0x10000;
            }
            else
            {
                ++i;
                return false;
            }
            if (i + extra >= s.size())
            {
                ++i;
                return false;
            }
            for (std::size_t k = 1; k <= extra; ++k)
            {
                const unsigned char cc = static_cast<unsigned char>(s[i + k]);
                if ((cc & 0xC0) != 0x80)
                {
                    ++i;
                    return false;
                }
                v = (v << 6) | (cc & 0x3F);
            }
            if (v < least || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF))
            {
                ++i;
                return false;
            }
            cp = v;
            i += extra + 1;
            return true;
        }

        void appendUtf8(std::string& out, char32_t c)
        {
            if (c < 0x80)
            {
                out += static_cast<char>(c);
            }
            else if (c < 0x800)
            {
                out += static_cast<char>(0xC0 | (c >> 6));
                out += static_cast<char>(0x80 | (c & 0x3F));
            }
            else if (c < 0x10000)
            {
                out += static_cast<char>(0xE0 | (c >> 12));
                out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (c & 0x3F));
            }
            else
            {
                out += static_cast<char>(0xF0 | (c >> 18));
                out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
                out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (c & 0x3F));
            }
        }

        // ---- folding --------------------------------------------------------

        // Half-width katakana U+FF66..U+FF9D as full-width katakana.
        constexpr char32_t kHalfKana[] = {
            0x30F2, 0x30A1, 0x30A3, 0x30A5, 0x30A7, 0x30A9, 0x30E3, 0x30E5, 0x30E7, 0x30C3, // ヲ ァ ィ ゥ ェ ォ ャ ュ ョ ッ
            0x30FC, 0x30A2, 0x30A4, 0x30A6, 0x30A8, 0x30AA, 0x30AB, 0x30AD, 0x30AF, 0x30B1, // ー ア イ ウ エ オ カ キ ク ケ
            0x30B3, 0x30B5, 0x30B7, 0x30B9, 0x30BB, 0x30BD, 0x30BF, 0x30C1, 0x30C4, 0x30C6, // コ サ シ ス セ ソ タ チ ツ テ
            0x30C8, 0x30CA, 0x30CB, 0x30CC, 0x30CD, 0x30CE, 0x30CF, 0x30D2, 0x30D5, 0x30D8, // ト ナ ニ ヌ ネ ノ ハ ヒ フ ヘ
            0x30DB, 0x30DE, 0x30DF, 0x30E0, 0x30E1, 0x30E2, 0x30E4, 0x30E6, 0x30E8, 0x30E9, // ホ マ ミ ム メ モ ヤ ユ ヨ ラ
            0x30EA, 0x30EB, 0x30EC, 0x30ED, 0x30EF, 0x30F3,                                 // リ ル レ ロ ワ ン
        };

        // A katakana that takes a voiced mark (゛) by moving one code point up.
        bool takesDakuten(char32_t c)
        {
            return (c >= 0x30AB && c <= 0x30C1 && (c & 1)) // カ..チ
                || c == 0x30C4 || c == 0x30C6 || c == 0x30C8 // ツ テ ト
                || c == 0x30CF || c == 0x30D2 || c == 0x30D5 || c == 0x30D8 || c == 0x30DB; // ハ行
        }

        bool takesHandakuten(char32_t c)
        {
            return c == 0x30CF || c == 0x30D2 || c == 0x30D5 || c == 0x30D8 || c == 0x30DB;
        }

        // Latin-1 letters U+00C0..U+00FF without accents; '?' = not a letter.
        constexpr char kLatin1[] =
            "AAAAAAACEEEEIIII"
            "DNOOOOO?OUUUUYTs"
            "aaaaaaaceeeeiiii"
            "dnooooo?ouuuuyty";

        // Folds one code point: 0 = drop it, ' ' = separator, otherwise the folded letter.
        char32_t foldCodePoint(char32_t c)
        {
            if (c < 0x80)
            {
                if (c >= 'A' && c <= 'Z')
                    return c + 32;
                if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
                    return c;
                if (c == '\'')
                    return 0;
                return ' ';
            }
            if (c <= 0xBF)
            {
                if (c == 0xAA)
                    return 'a';
                if (c == 0xBA)
                    return 'o';
                return ' ';
            }
            if (c <= 0xFF)
            {
                const char l = kLatin1[c - 0xC0];
                if (l == '?')
                    return ' ';
                return (l >= 'A' && l <= 'Z') ? static_cast<char32_t>(l + 32) : static_cast<char32_t>(l);
            }
            if (c >= 0x0410 && c <= 0x042F)
                return c + 0x20; // Cyrillic
            if (c >= 0x0400 && c <= 0x040F)
                return c + 0x50;
            if (c == 0x2018 || c == 0x2019)
                return 0; // curly apostrophes: "men’s" = "mens"
            if (c >= 0x2000 && c <= 0x206F)
                return ' '; // general punctuation, spaces, ZWJ
            if (c >= 0x2190 && c <= 0x2BFF)
                return ' '; // arrows, shapes, dingbats, stars
            if ((c >= 0x3000 && c <= 0x3004) || (c >= 0x3008 && c <= 0x3020) || c == 0x3030 || c == 0x30FB)
                return ' ';
            if (c == 0x3099 || c == 0x309A || c == 0x309B || c == 0x309C)
                return 0; // stray voiced marks
            if ((c >= 0x30A1 && c <= 0x30F6) || c == 0x30FD || c == 0x30FE)
                return c - 0x60; // katakana to hiragana
            if (c == 0xFF9E || c == 0xFF9F)
                return 0;
            if (c >= 0xFF5F && c <= 0xFF65)
                return ' ';
            if (c >= 0xFE00 && c <= 0xFE0F)
                return 0; // variation selectors
            if (c >= 0x1F000 && c <= 0x1FAFF)
                return ' '; // emoji
            return c;
        }

        // ---- misc -----------------------------------------------------------

        bool isBoundary(const std::string& text, std::size_t pos)
        {
            return pos == 0 || text[pos - 1] == ' ';
        }

        // Folder names that only say what is inside a product folder.
        bool isGenericFolderName(const std::string& n)
        {
            static const char* const kNames[] = {
                "texture", "textures", "unpack", "unpacked", "boxed", "box", "contents", "content",
                "script", "scripts", "animation", "animations", "anim", "anims", "mesh", "meshes",
                "image", "images", "notecard", "notecards", "sound", "sounds", "resize", "resizer",
                "huds", "hud", "applier", "appliers", "extras", "extra", "demo", "demos",
            };
            if (n.compare(0, 6, "unpack") == 0)
                return true;
            for (const char* k : kNames)
            {
                if (n == k)
                    return true;
            }
            return false;
        }

        std::vector<std::string> normalizeAll(const std::vector<std::string>& in)
        {
            std::vector<std::string> out;
            for (const std::string& s : in)
            {
                std::string n = normalize(s);
                if (!n.empty())
                    out.push_back(std::move(n));
            }
            return out;
        }

        bool oneOf(const std::vector<std::string>& set, const std::string& v)
        {
            return std::find(set.begin(), set.end(), v) != set.end();
        }

        std::string joinSegments(const std::vector<std::string>& segs)
        {
            std::string out;
            for (const std::string& s : segs)
            {
                if (!out.empty())
                    out += '/';
                out += s;
            }
            return out;
        }

        std::vector<std::string> splitPath(const std::string& p)
        {
            std::vector<std::string> out;
            std::size_t start = 0;
            while (start <= p.size())
            {
                std::size_t end = p.find('/', start);
                if (end == kNpos)
                    end = p.size();
                std::string seg = normalize(std::string_view(p).substr(start, end - start));
                if (!seg.empty())
                    out.push_back(std::move(seg));
                start = end + 1;
            }
            return out;
        }
    }

    std::string normalize(std::string_view s)
    {
        std::u32string cps;
        cps.reserve(s.size());
        for (std::size_t i = 0; i < s.size();)
        {
            char32_t cp = 0;
            if (nextCodePoint(s, i, cp))
                cps += cp;
        }

        std::string out;
        out.reserve(s.size());
        bool pendingSpace = false;
        auto emit = [&](char32_t c)
        {
            if (c == 0)
                return;
            if (c == U' ')
            {
                pendingSpace = !out.empty();
                return;
            }
            if (pendingSpace)
            {
                out += ' ';
                pendingSpace = false;
            }
            appendUtf8(out, c);
        };

        for (std::size_t i = 0; i < cps.size(); ++i)
        {
            char32_t c = cps[i];
            if (c >= 0xFF01 && c <= 0xFF5E)
            {
                c -= 0xFEE0; // full-width ASCII
            }
            else if (c >= 0xFF66 && c <= 0xFF9D)
            {
                c = kHalfKana[c - 0xFF66];
                if (i + 1 < cps.size())
                {
                    const char32_t next = cps[i + 1];
                    if (next == 0xFF9E && c == 0x30A6)
                    {
                        c = 0x30F4; // ヴ
                        ++i;
                    }
                    else if (next == 0xFF9E && takesDakuten(c))
                    {
                        c += 1;
                        ++i;
                    }
                    else if (next == 0xFF9F && takesHandakuten(c))
                    {
                        c += 2;
                        ++i;
                    }
                }
            }
            emit(foldCodePoint(c));
        }
        return out;
    }

    // ---- Index ----------------------------------------------------------------

    namespace
    {
        struct Rec
        {
            Entry e;
            std::string nname;
            std::string ndesc;
            std::string ntype;
            std::string nsubtype;
            std::string ncreator;
            bool generic = false; // a folder named like Texture or Unpack

            // The parent folder's slot in Impl::recs; valid while parentGen == Impl::folderGen.
            mutable std::uint64_t parentGen = 0;
            mutable std::size_t parentIdx = 0;

            // Folders only; valid while pathGen == Impl::folderGen.
            mutable std::uint64_t pathGen = 0;
            mutable std::string path;     // "A/B/C" including its own name, root omitted
            mutable std::string npath;    // names normalized, joined with ' ' (for matching)
            mutable std::string npathSep; // names normalized, joined with '/' (for `under`)
        };

        struct Cand
        {
            std::size_t idx = 0;
            double score = 0;
            std::uint64_t gmask = 0;
            std::uint8_t where = 0;
        };
    }

    struct Index::Impl
    {
        std::vector<Rec> recs;
        std::unordered_map<std::string, std::size_t> byId;
        std::uint64_t gen = 1;
        std::uint64_t folderGen = 1;
        std::size_t folders = 0;

        // Bumped whenever a folder is added, changed or removed, or any slot moves.
        const Rec* parentFolder(const Rec& r) const
        {
            if (r.e.parent.empty())
                return nullptr;
            if (r.parentGen != folderGen)
            {
                const auto it = byId.find(r.e.parent);
                r.parentIdx = (it == byId.end() || !recs[it->second].e.is_folder) ? kNoSlot : it->second;
                r.parentGen = folderGen;
            }
            return r.parentIdx == kNoSlot ? nullptr : &recs[r.parentIdx];
        }

        // Fills the path caches of folder `idx` (and of the folders above it).
        void ensurePath(std::size_t idx) const
        {
            std::size_t chain[kMaxDepth];
            std::size_t n = 0;
            std::size_t cur = idx;
            while (n < kMaxDepth)
            {
                const Rec& r = recs[cur];
                if (r.pathGen == folderGen)
                    break;
                chain[n++] = cur;
                const Rec* up = parentFolder(r);
                if (!up)
                    break;
                cur = static_cast<std::size_t>(up - recs.data());
            }
            for (std::size_t k = n; k-- > 0;)
            {
                const Rec& r = recs[chain[k]];
                const Rec* p = parentFolder(r);
                const std::string& nname = r.nname;
                if (r.e.parent.empty())
                {
                    // a root folder: its own name is left out of every path
                    r.path.clear();
                    r.npath.clear();
                    r.npathSep.clear();
                }
                else if (p && p->pathGen == folderGen)
                {
                    r.path = p->path.empty() ? r.e.name : p->path + "/" + r.e.name;
                    r.npath = p->npath.empty() ? nname : p->npath + " " + nname;
                    r.npathSep = p->npathSep.empty() ? nname : p->npathSep + "/" + nname;
                }
                else
                {
                    r.path = r.e.name;
                    r.npath = nname;
                    r.npathSep = nname;
                }
                r.pathGen = folderGen;
            }
        }

        // The folder an item or folder is shown under: its folder, climbed past
        // generic names (but never into or past a root folder).
        std::size_t productFolder(std::size_t startFolder) const
        {
            std::size_t cur = startFolder;
            for (std::size_t depth = 0; depth < 16; ++depth)
            {
                const Rec& f = recs[cur];
                if (!f.generic)
                    break;
                const Rec* up = parentFolder(f);
                if (!up || up->e.parent.empty())
                    break;
                cur = static_cast<std::size_t>(up - recs.data());
            }
            return cur;
        }

        Hit makeHit(const Cand& c) const
        {
            const Rec& r = recs[c.idx];
            Hit h;
            h.id = r.e.id;
            h.name = r.e.name;
            h.type = r.e.type;
            h.subtype = r.e.subtype;
            if (const Rec* p = parentFolder(r))
            {
                ensurePath(static_cast<std::size_t>(p - recs.data()));
                h.path = p->path;
            }
            h.creator = r.e.creator;
            h.acquired = r.e.acquired;
            h.perms = r.e.perms;
            h.is_folder = r.e.is_folder;
            h.is_link = r.e.is_link;
            h.score = c.score;
            h.matched_groups = c.gmask;
            h.matched_where = c.where;
            return h;
        }

        bool better(const Cand& a, const Cand& b) const
        {
            if (a.score != b.score)
                return a.score > b.score;
            const Rec& ra = recs[a.idx];
            const Rec& rb = recs[b.idx];
            if (ra.e.acquired != rb.e.acquired)
                return ra.e.acquired > rb.e.acquired;
            if (ra.nname != rb.nname)
                return ra.nname < rb.nname;
            return ra.e.id < rb.e.id;
        }
    };

    Index::Index() : m(std::make_unique<Impl>()) {}
    Index::~Index() = default;

    void Index::upsert(Entry entry)
    {
        Rec rec;
        rec.nname = normalize(entry.name);
        rec.ndesc = normalize(entry.desc);
        rec.ntype = normalize(entry.type);
        rec.nsubtype = normalize(entry.subtype);
        rec.ncreator = normalize(entry.creator);
        rec.generic = entry.is_folder && isGenericFolderName(rec.nname);
        rec.e = std::move(entry);

        ++m->gen;
        const auto it = m->byId.find(rec.e.id);
        if (it != m->byId.end())
        {
            Rec& old = m->recs[it->second];
            if (old.e.is_folder || rec.e.is_folder)
                ++m->folderGen;
            if (!old.e.is_folder && rec.e.is_folder)
                ++m->folders;
            else if (old.e.is_folder && !rec.e.is_folder)
                --m->folders;
            old = std::move(rec);
            return;
        }
        if (rec.e.is_folder)
        {
            ++m->folderGen;
            ++m->folders;
        }
        m->byId.emplace(rec.e.id, m->recs.size());
        m->recs.push_back(std::move(rec));
    }

    bool Index::remove(const std::string& id)
    {
        const auto it = m->byId.find(id);
        if (it == m->byId.end())
            return false;
        const std::size_t idx = it->second;
        ++m->gen;
        ++m->folderGen; // a slot moves
        if (m->recs[idx].e.is_folder)
            --m->folders;
        m->byId.erase(it);
        const std::size_t last = m->recs.size() - 1;
        if (idx != last)
        {
            m->recs[idx] = std::move(m->recs[last]);
            m->byId[m->recs[idx].e.id] = idx;
        }
        m->recs.pop_back();
        return true;
    }

    void Index::clear()
    {
        m->recs.clear();
        m->byId.clear();
        m->folders = 0;
        ++m->gen;
        ++m->folderGen;
    }

    std::size_t Index::size() const { return m->recs.size(); }
    std::size_t Index::folderCount() const { return m->folders; }
    std::uint64_t Index::generation() const { return m->gen; }

    const Entry* Index::find(const std::string& id) const
    {
        const auto it = m->byId.find(id);
        return it == m->byId.end() ? nullptr : &m->recs[it->second].e;
    }

    std::vector<std::string> Index::distinctCreators() const
    {
        std::unordered_set<std::string_view> seen;
        std::vector<std::string> out;
        for (const Rec& r : m->recs)
        {
            if (!r.e.is_folder && !r.e.creator.empty() && seen.insert(r.e.creator).second)
                out.push_back(r.e.creator);
        }
        return out;
    }

    std::string Index::folderPath(const std::string& id) const
    {
        const auto it = m->byId.find(id);
        if (it == m->byId.end() || !m->recs[it->second].e.is_folder)
            return std::string();
        m->ensurePath(it->second);
        return m->recs[it->second].path;
    }

    Result Index::search(const Query& q) const
    {
        const Impl& d = *m;
        Result res;
        res.generation = d.gen;
        res.grouped = q.group_by == GroupBy::Folder;

        // ---- compile the query ----
        std::vector<std::vector<std::string>> groups;
        for (const auto& g : q.groups)
        {
            std::vector<std::string> terms = normalizeAll(g);
            if (!terms.empty())
                groups.push_back(std::move(terms));
        }
        if (groups.size() > 64)
            groups.resize(64);
        std::size_t minGroups = 0;
        if (!groups.empty())
            minGroups = std::clamp<std::size_t>(q.min_groups < 1 ? 1 : static_cast<std::size_t>(q.min_groups), 1, groups.size());

        const std::uint8_t where = q.where ? q.where : static_cast<std::uint8_t>(WHERE_NAME | WHERE_PATH);
        const std::vector<std::string> exclude = normalizeAll(q.exclude);
        const std::vector<std::string> types = normalizeAll(q.types);
        const std::vector<std::string> subtypes = normalizeAll(q.subtypes);
        const std::string creator = normalize(q.creator);
        const bool itemOnly = !types.empty() || !subtypes.empty() || !creator.empty() || !q.creator_ids.empty() || q.perms != 0
            || q.acquired_after != 0 || q.acquired_before != 0;

        std::unordered_set<std::string> underIds;
        if (!q.under.empty())
        {
            if (d.byId.count(q.under))
            {
                underIds.insert(q.under);
            }
            else
            {
                std::vector<std::string> segs = splitPath(q.under);
                std::vector<std::string> noRoot;
                if (segs.size() > 1)
                {
                    // "My Inventory/Clothing": the root's own name is not part of a path
                    for (const Rec& r : d.recs)
                    {
                        if (r.e.is_folder && r.e.parent.empty() && r.nname == segs[0])
                        {
                            noRoot.assign(segs.begin() + 1, segs.end());
                            break;
                        }
                    }
                }
                const std::string full = joinSegments(segs);
                const std::string trimmed = joinSegments(noRoot);
                for (std::size_t i = 0; i < d.recs.size(); ++i)
                {
                    const Rec& r = d.recs[i];
                    if (!r.e.is_folder || r.e.parent.empty())
                        continue;
                    d.ensurePath(i);
                    if (r.npathSep == full || (!noRoot.empty() && r.npathSep == trimmed))
                        underIds.insert(r.e.id);
                }
                if (underIds.empty() && segs.size() == 1)
                {
                    for (const Rec& r : d.recs)
                    {
                        if (r.e.is_folder && !r.e.parent.empty() && r.nname == segs[0])
                            underIds.insert(r.e.id);
                    }
                }
            }
            if (underIds.empty())
            {
                res.error = "under: no folder matches \"" + q.under + "\"";
                return res;
            }
        }

        // range of acquisition times, for a small recency bonus
        std::int64_t tmin = 0;
        std::int64_t tmax = 0;
        for (const Rec& r : d.recs)
        {
            if (r.e.is_folder || r.e.acquired <= 0)
                continue;
            tmin = tmin == 0 ? r.e.acquired : std::min(tmin, r.e.acquired);
            tmax = std::max(tmax, r.e.acquired);
        }

        // ---- scan ----
        // Items in one folder share the folder-path part of the match: work it out once per folder.
        struct CtxInfo
        {
            std::vector<double> pathScore; // per group
            bool excluded = false;
        };
        std::vector<std::int32_t> slotOf(d.recs.size(), -1);
        std::vector<CtxInfo> ctxInfo;
        auto ctxFor = [&](std::size_t folderIdx) -> const CtxInfo&
        {
            std::int32_t& slot = slotOf[folderIdx];
            if (slot < 0)
            {
                slot = static_cast<std::int32_t>(ctxInfo.size());
                ctxInfo.emplace_back();
                CtxInfo& ci = ctxInfo.back();
                d.ensurePath(folderIdx);
                const std::string& npath = d.recs[folderIdx].npath;
                ci.pathScore.assign(groups.size(), 0.0);
                if (where & WHERE_PATH)
                {
                    for (std::size_t g = 0; g < groups.size(); ++g)
                    {
                        for (const std::string& term : groups[g])
                        {
                            const std::size_t pos = npath.find(term);
                            if (pos != kNpos)
                                ci.pathScore[g] = std::max(ci.pathScore[g], isBoundary(npath, pos) ? 2.5 : 2.0);
                        }
                    }
                }
                for (const std::string& term : exclude)
                {
                    if (npath.find(term) != kNpos)
                        ci.excluded = true;
                }
            }
            return ctxInfo[static_cast<std::size_t>(slot)];
        };

        std::vector<Cand> cands;
        const std::string empty;
        for (std::size_t i = 0; i < d.recs.size(); ++i)
        {
            const Rec& r = d.recs[i];
            if (!(q.areas & r.e.area))
                continue;
            if (r.e.is_folder)
            {
                if (q.target == Target::Items || itemOnly || r.e.parent.empty())
                    continue;
            }
            else
            {
                if (q.target == Target::Folders)
                    continue;
                if (r.e.is_link && !q.include_links)
                    continue;
                if (!types.empty() && !oneOf(types, r.ntype))
                    continue;
                if (!subtypes.empty() && !oneOf(subtypes, r.nsubtype))
                    continue;
                if (!creator.empty() && r.ncreator.find(creator) == kNpos)
                    continue;
                if (!q.creator_ids.empty() && std::find(q.creator_ids.begin(), q.creator_ids.end(), r.e.creator) == q.creator_ids.end())
                    continue;
                if ((r.e.perms & q.perms) != q.perms)
                    continue;
                if (q.acquired_after != 0 && r.e.acquired < q.acquired_after)
                    continue;
                if (q.acquired_before != 0 && r.e.acquired > q.acquired_before)
                    continue;
            }

            const Rec* parent = d.parentFolder(r);
            const CtxInfo* ctx = parent ? &ctxFor(static_cast<std::size_t>(parent - d.recs.data())) : nullptr;
            if (ctx && ctx->excluded)
                continue;

            Cand c;
            c.idx = i;
            if (!groups.empty())
            {
                std::size_t matched = 0;
                for (std::size_t g = 0; g < groups.size(); ++g)
                {
                    double best = 0;
                    std::uint8_t gw = 0;
                    for (const std::string& term : groups[g])
                    {
                        if (where & WHERE_NAME)
                        {
                            const std::size_t pos = r.nname.find(term);
                            if (pos != kNpos)
                            {
                                double w = 3.0;
                                if (isBoundary(r.nname, pos))
                                    w += 0.5;
                                if (r.nname.size() == term.size())
                                    w += 1.0;
                                best = std::max(best, w);
                                gw |= WHERE_NAME;
                            }
                        }
                        if ((where & WHERE_DESC) && !r.ndesc.empty())
                        {
                            const std::size_t pos = r.ndesc.find(term);
                            if (pos != kNpos)
                            {
                                best = std::max(best, isBoundary(r.ndesc, pos) ? 1.5 : 1.0);
                                gw |= WHERE_DESC;
                            }
                        }
                    }
                    if (ctx && ctx->pathScore[g] > 0)
                    {
                        best = std::max(best, ctx->pathScore[g]);
                        gw |= WHERE_PATH;
                    }
                    if (best > 0)
                    {
                        ++matched;
                        c.score += best;
                        c.gmask |= std::uint64_t(1) << g;
                        c.where |= gw;
                    }
                }
                if (matched < minGroups)
                    continue;
            }

            bool excluded = false;
            for (const std::string& term : exclude)
            {
                if (r.nname.find(term) != kNpos)
                {
                    excluded = true;
                    break;
                }
            }
            if (excluded)
                continue;

            if (!underIds.empty())
            {
                bool inside = false;
                const Rec* p = parent;
                for (std::size_t depth = 0; p && depth < kMaxDepth; ++depth)
                {
                    if (underIds.count(p->e.id))
                    {
                        inside = true;
                        break;
                    }
                    p = d.parentFolder(*p);
                }
                if (!inside)
                    continue;
            }

            if (!r.e.is_folder && r.e.acquired > 0 && tmax > tmin)
                c.score += 0.3 * static_cast<double>(r.e.acquired - tmin) / static_cast<double>(tmax - tmin);
            cands.push_back(c);
        }
        res.total_matches = cands.size();
        if (q.collect_ids)
        {
            res.ids.reserve(cands.size());
            for (const Cand& c : cands)
                res.ids.push_back(d.recs[c.idx].e.id);
        }

        // ---- facets over the item hits ----
        {
            std::unordered_map<std::string_view, std::size_t> typeCount;
            std::unordered_map<std::string_view, std::size_t> creatorCount;
            for (const Cand& c : cands)
            {
                const Rec& r = d.recs[c.idx];
                if (r.e.is_folder)
                    continue;
                ++typeCount[r.e.type];
                if (!r.e.creator.empty())
                    ++creatorCount[r.e.creator];
            }
            auto top = [](const std::unordered_map<std::string_view, std::size_t>& in)
            {
                std::vector<Count> v;
                for (const auto& kv : in)
                    v.push_back({std::string(kv.first), kv.second});
                std::sort(v.begin(), v.end(), [](const Count& a, const Count& b)
                          { return a.count != b.count ? a.count > b.count : a.key < b.key; });
                if (v.size() > kFacetTop)
                    v.resize(kFacetTop);
                return v;
            };
            res.types = top(typeCount);
            res.creators = top(creatorCount);
        }

        const std::size_t limit = std::clamp<std::size_t>(q.limit, 1, kMaxLimit);
        const std::size_t offset = q.offset;
        auto order = [&d](const Cand& a, const Cand& b) { return d.better(a, b); };

        if (q.group_by == GroupBy::None)
        {
            std::sort(cands.begin(), cands.end(), order);
            for (std::size_t i = offset; i < cands.size() && i < offset + limit; ++i)
                res.hits.push_back(d.makeHit(cands[i]));
            if (offset + limit < cands.size())
                res.next_offset = static_cast<std::int64_t>(offset + limit);
            return res;
        }

        // ---- collapse into product folders ----
        struct Acc
        {
            bool folderMatched = false;
            double bestScore = 0;
            std::uint64_t gmask = 0;
            std::vector<Cand> items;
        };
        constexpr std::size_t kNoFolder = static_cast<std::size_t>(-1);
        std::unordered_map<std::size_t, Acc> acc;
        for (const Cand& c : cands)
        {
            const Rec& r = d.recs[c.idx];
            std::size_t key = kNoFolder;
            if (r.e.is_folder)
            {
                key = d.productFolder(c.idx);
            }
            else if (const Rec* p = d.parentFolder(r))
            {
                key = d.productFolder(static_cast<std::size_t>(p - d.recs.data()));
            }
            Acc& a = acc[key];
            a.bestScore = std::max(a.bestScore, c.score);
            a.gmask |= c.gmask;
            if (r.e.is_folder)
            {
                if (key == c.idx)
                    a.folderMatched = true;
            }
            else
            {
                a.items.push_back(c);
            }
        }

        struct Ranked
        {
            std::size_t key;
            double score;
        };
        std::vector<Ranked> ranked;
        ranked.reserve(acc.size());
        for (const auto& kv : acc)
        {
            ranked.push_back({kv.first, kv.second.bestScore + 0.1 * std::log2(1.0 + static_cast<double>(kv.second.items.size()))});
        }
        auto rankedLess = [&d, &empty](const Ranked& a, const Ranked& b)
        {
            if (a.score != b.score)
                return a.score > b.score;
            const std::string& na = a.key == kNoFolder ? empty : d.recs[a.key].nname;
            const std::string& nb = b.key == kNoFolder ? empty : d.recs[b.key].nname;
            if (na != nb)
                return na < nb;
            return a.key < b.key;
        };
        std::sort(ranked.begin(), ranked.end(), rankedLess);
        res.total_groups = ranked.size();

        for (std::size_t i = offset; i < ranked.size() && i < offset + limit; ++i)
        {
            Acc& a = acc[ranked[i].key];
            Group g;
            g.score = ranked[i].score;
            g.folder_matched = a.folderMatched;
            g.matched_groups = a.gmask;
            g.item_count = a.items.size();
            if (ranked[i].key != kNoFolder)
            {
                const Rec& f = d.recs[ranked[i].key];
                d.ensurePath(ranked[i].key);
                g.folder_id = f.e.id;
                g.folder_name = f.e.name;
                g.folder_path = f.path;
            }
            std::sort(a.items.begin(), a.items.end(), order);
            for (std::size_t k = 0; k < a.items.size() && k < kSamples; ++k)
                g.samples.push_back(d.makeHit(a.items[k]));
            res.groups.push_back(std::move(g));
        }
        if (offset + limit < ranked.size())
            res.next_offset = static_cast<std::int64_t>(offset + limit);
        return res;
    }
}

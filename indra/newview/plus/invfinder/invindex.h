/**
 * @file invindex.h
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

// Standard library only, so it is built and tested on its own
// (tools/invfinder in the sl-viewer repository). Design: research/inventory-finder-mcp.md.
//
// Not thread-safe: the glue (invsource.cpp feeds it, invtools.cpp queries it
// through InvSource) uses it from the main thread only.

#ifndef PLUS_INVFINDER_INVINDEX_H
#define PLUS_INVFINDER_INVINDEX_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace plus::invfinder
{
    // Folds a string for matching: lower case, full-width ASCII and half-width
    // katakana to their normal forms, katakana to hiragana, accents removed
    // from Latin-1 letters, apostrophes dropped, every other punctuation or
    // symbol run turned into one space. Invalid UTF-8 bytes are dropped.
    std::string normalize(std::string_view utf8);

    enum Area : std::uint8_t
    {
        AREA_MINE = 1,
        AREA_LIBRARY = 2,
        AREA_TRASH = 4,
        AREA_OUTFIT = 8, // the current outfit's links
    };

    enum Perm : std::uint8_t
    {
        PERM_COPY = 1,
        PERM_MODIFY = 2,
        PERM_TRANSFER = 4,
    };

    enum Where : std::uint8_t
    {
        WHERE_NAME = 1,
        WHERE_PATH = 2, // names of the folders above the entry
        WHERE_DESC = 4,
    };

    enum class Target
    {
        Folders,
        Items,
        Both,
    };

    enum class GroupBy
    {
        None,
        Folder, // collapse into "product folders"
    };

    struct Entry
    {
        std::string id;     // opaque (UUID text)
        std::string parent; // "" for a root folder
        bool is_folder = false;
        bool is_link = false;
        std::string name;
        std::string desc;
        std::string type;    // items: "object", "wearable", "notecard", ...
        std::string subtype; // items: "skirt", "jacket", ...
        std::string creator; // name or id
        std::int64_t acquired = 0; // unix time, 0 = unknown
        std::uint8_t perms = 0;    // Perm bits the owner has
        std::uint8_t area = AREA_MINE;
    };

    // Filters other than the text ones (types, subtypes, creator, perms,
    // dates) describe items, so a query that sets any of them returns no folders.
    struct Query
    {
        // Each inner list is a set of interchangeable words (OR). An entry
        // must match at least min_groups of the lists. No lists = no text
        // condition (filters only).
        std::vector<std::vector<std::string>> groups;
        int min_groups = 1;
        std::vector<std::string> exclude; // dropped when found in name or path

        Target target = Target::Both;
        std::uint8_t where = WHERE_NAME | WHERE_PATH; // 0 = the default
        std::vector<std::string> types;
        std::vector<std::string> subtypes;
        // Folder id, or a path ("Clothing/Brands", the root's name may lead),
        // or a single folder name when no path of that name exists.
        std::string under;
        std::string creator; // part of the stored creator text
        std::vector<std::string> creator_ids; // exact creator texts (any of them); with `creator`, both must hold
        std::int64_t acquired_after = 0;  // 0 = no limit
        std::int64_t acquired_before = 0; // 0 = no limit
        std::uint8_t perms = 0;           // required Perm bits
        std::uint8_t areas = AREA_MINE;
        bool include_links = false;

        GroupBy group_by = GroupBy::Folder;
        std::size_t limit = 30; // 1..100
        std::size_t offset = 0;
        bool collect_ids = false; // Result::ids gets the id of every match (not only the page)
    };

    struct Hit
    {
        std::string id;
        std::string name;
        std::string type;
        std::string subtype;
        std::string path; // folders above it, "A/B/C"
        std::string creator;
        std::int64_t acquired = 0;
        std::uint8_t perms = 0;
        bool is_folder = false;
        bool is_link = false;
        double score = 0;
        std::uint64_t matched_groups = 0; // bit n = Query::groups[n] (after empty ones are dropped)
        std::uint8_t matched_where = 0;   // Where bits
    };

    // A "product folder": the folder that holds a hit, climbed past generic
    // names (Texture, Unpack, Boxed, ...), with everything found inside it.
    struct Group
    {
        std::string folder_id; // "" when the hits had no known parent
        std::string folder_name;
        std::string folder_path; // path of the folder itself
        bool folder_matched = false; // the folder's own name matched
        std::size_t item_count = 0;
        double score = 0;
        std::uint64_t matched_groups = 0;
        std::vector<Hit> samples; // best items, up to 3
    };

    struct Count
    {
        std::string key;
        std::size_t count = 0;
    };

    struct Result
    {
        std::string error; // non-empty = nothing was searched
        bool grouped = false; // the query's group_by was Folder: look in `groups`, not `hits`
        std::size_t total_matches = 0; // folders + items, before grouping
        std::size_t total_groups = 0;  // 0 when group_by is None
        std::vector<Group> groups;     // group_by Folder
        std::vector<Hit> hits;         // group_by None
        std::vector<Count> types;      // item hits by type, top 8
        std::vector<Count> creators;   // item hits by creator, top 8
        std::vector<std::string> ids;  // every match (folders and items), when Query::collect_ids
        std::int64_t next_offset = -1; // -1 = no more
        std::uint64_t generation = 0;  // changes whenever the index changes
    };

    class Index
    {
    public:
        Index();
        ~Index();
        Index(const Index&) = delete;
        Index& operator=(const Index&) = delete;

        void upsert(Entry entry);
        bool remove(const std::string& id);
        void clear();

        std::size_t size() const;
        std::size_t folderCount() const;
        std::uint64_t generation() const;
        const Entry* find(const std::string& id) const;
        // The distinct creator texts of all items, in no particular order.
        std::vector<std::string> distinctCreators() const;
        // "A/B/C" of the folder itself ("" for a root or an unknown id)
        std::string folderPath(const std::string& id) const;

        Result search(const Query& query) const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m;
    };
}

#endif // PLUS_INVFINDER_INVINDEX_H

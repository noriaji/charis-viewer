/**
 * @file invargs.h
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

// boost::json and invindex only, so it is tested on its own
// (tools/invfinder in the sl-viewer repository).

#ifndef PLUS_INVFINDER_INVARGS_H
#define PLUS_INVFINDER_INVARGS_H

#include "invindex.h"

#include <boost/json.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace plus::invfinder
{
    struct SearchRequest
    {
        Query query;
        // A creator given by name: the caller turns it into query.creator_ids
        // (the index stores creators as ids).
        std::string creator_name;
        // Also show every match in the inventory's AI search tab.
        bool show = false;
    };

    struct ShowRequest
    {
        std::vector<std::string> ids; // item and folder ids (UUID text, lower case)
        bool clear = false;           // empty the tab instead
    };

    // Reads the arguments of inventory_search. On failure returns false and
    // says what is wrong (for the model, in English).
    bool parseSearchArgs(const boost::json::object& args, SearchRequest& out, std::string& error);

    // Reads the arguments of inventory_show.
    bool parseShowArgs(const boost::json::object& args, ShowRequest& out, std::string& error);

    // Name for a creator id (the viewer's name cache); empty = unknown.
    using NameOf = std::function<std::string(const std::string& id)>;

    boost::json::object hitToJson(const Hit& hit, const NameOf& creatorName);
    boost::json::object searchResultToJson(const Result& result, const NameOf& creatorName);

    // "2026-10-17" to unix time (UTC, midnight); false if it is not a valid date.
    bool parseDate(const std::string& text, std::int64_t& unixTime);
    std::string formatDate(std::int64_t unixTime);
}

#endif // PLUS_INVFINDER_INVARGS_H

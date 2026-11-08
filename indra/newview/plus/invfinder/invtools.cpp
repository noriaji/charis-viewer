/**
 * @file invtools.cpp
 * @brief The MCP tools of the inventory finder: status and search over the
 *        whole inventory.
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

#include "llviewerprecompiledheaders.h"

#include "invtools.h"

#include "invargs.h"
#include "invsource.h"
#include "invview.h"

#include "llinventorymodelbackgroundfetch.h"

namespace json = boost::json;
using plus::mcp::Tool;
using plus::mcp::ToolResult;

namespace plus::invfinder
{
    namespace
    {
        constexpr double kStepTimeoutSeconds = 60.0;

        const char kSearchDescription[] =
            "Searches the user's whole Second Life inventory (tens of thousands of items) and returns candidates, "
            "best first. You do the thinking, this tool does the looking: the user's request is usually vague "
            "(\"something for Christmas\"), so turn it into several lists of interchangeable words in English AND "
            "Japanese and let the tool find them. Folder names carry most of the meaning (item names inside a "
            "boxed product are often just \"Dress\" or \"Texture\"), so words are matched against item names AND "
            "the names of the folders above them.\n"
            "\n"
            "How to query: put synonyms of ONE idea in one list inside `groups`, e.g. christmas = "
            "[\"christmas\",\"xmas\",\"x-mas\",\"santa\",\"noel\",\"reindeer\",\"snow\",\"winter\",\"クリスマス\","
            "\"サンタ\",\"トナカイ\",\"スノー\"]; a second idea in a second list (\"dress\",\"gown\","
            "\"ワンピース\",\"ドレス\"). An entry must match at least `min_groups` lists (1 = any, "
            "2 = both ideas). Case, width (Ａ/A, ｶﾅ/カナ), hiragana/katakana and punctuation do not matter.\n"
            "\n"
            "Default result is grouped by \"product folder\": each group is a folder with the number of matching "
            "items in it and up to 3 sample items, so you can tell products apart. Start with the default; if "
            "`matches` is huge, narrow with more groups/min_groups, `types`, `under`, `creator` or dates "
            "(look at `facets` for what is in there); if it is zero, widen the word lists or set `where` to "
            "also include \"description\". Use group_by=\"none\" to list individual entries. Do not give up "
            "after one query; refine, and say how sure you are when you answer.\n"
            "\n"
            "Not included by default: Trash, Library, Current Outfit and link items (`scope`, `include_links`). "
            "If the result carries a warning that the inventory is not fully loaded, tell the user: matches may be missing. "
            "With show=true every match (not only this page) is also shown in the \"AI Search\" tab of the user's "
            "inventory window, like a normal search result, so the user can look at it, wear it, drag it. Use it once "
            "the query is good, not on every try. This tool never changes the inventory; it cannot wear, move or delete anything.";

        const char kShowDescription[] =
            "Shows the given inventory ids (items and folders, e.g. picked from inventory_search results) in the "
            "\"AI Search\" tab of the user's inventory window and opens that tab, like a normal search result: the "
            "entries in their folders. Use it to show a hand-picked list; to show everything a search found, use "
            "inventory_search with show=true instead. clear=true empties the tab. Nothing in the inventory changes.";

        const char kStatusDescription[] =
            "Reports whether the inventory is loaded and indexed: number of entries and folders, whether the viewer "
            "has fetched the whole inventory from the server, and which folders have not been fetched yet. "
            "Call it before the first search of a session when results might be incomplete.";

        json::object parseObject(const char* text)
        {
            return json::parse(text).as_object();
        }

        json::object searchSchema()
        {
            return parseObject(R"({
  "type": "object",
  "additionalProperties": false,
  "properties": {
    "groups": {
      "type": "array",
      "description": "Lists of interchangeable words (OR inside a list). Each list is one idea; an entry must match at least min_groups lists. Omit for a filter-only query (types, under, creator, dates). Max 64 lists.",
      "items": { "type": "array", "items": { "type": "string" } }
    },
    "min_groups": { "type": "integer", "minimum": 1, "description": "How many of the lists must match. Default 1." },
    "exclude": { "type": "array", "items": { "type": "string" }, "description": "Drop entries whose name or folder path contains any of these words (e.g. [\"men\",\"male\"])." },
    "target": { "type": "string", "enum": ["folders", "items", "both"], "description": "What can be found. Default both. Product folders are often the best hits." },
    "where": { "type": "array", "items": { "type": "string", "enum": ["name", "path", "description"] }, "description": "Where words are looked for. Default [\"name\",\"path\"]; path = names of the folders above the entry." },
    "types": { "type": "array", "items": { "type": "string" }, "description": "Item types: clothing (system layers), bodypart, object (rigged mesh clothes and attachments are objects!), texture, notecard, sound, animatn, gesture, landmark, lsltext (scripts), mesh, settings, material. \"wearable\" = clothing + bodypart. Folders are left out when any item filter is used." },
    "subtypes": { "type": "array", "items": { "type": "string" }, "description": "System clothing layer: shirt, pants, shoes, socks, jacket, gloves, undershirt, underpants, skirt, alpha, tattoo, universal, physics; body parts: shape, skin, hair, eyes." },
    "under": { "type": "string", "description": "Only inside this folder (and its sub folders): a folder id, a path like \"Clothing/Brands\", or a single folder name." },
    "creator": { "type": "string", "description": "Creator name (part of it) or id. Names are known only for creators the viewer has looked up; a warning says if some were not ready." },
    "acquired_after": { "type": "string", "description": "YYYY-MM-DD, items added to the inventory on or after this day." },
    "acquired_before": { "type": "string", "description": "YYYY-MM-DD, items added on or before this day." },
    "permissions": { "type": "array", "items": { "type": "string", "enum": ["copy", "modify", "transfer"] }, "description": "Permissions the user has on the item (all listed)." },
    "scope": { "type": "array", "items": { "type": "string", "enum": ["mine", "library", "trash", "outfit", "all"] }, "description": "Where to look. Default [\"mine\"] (the user's inventory without Trash and Current Outfit)." },
    "include_links": { "type": "boolean", "description": "Also return link items (outfit folders are full of them). Default false." },
    "group_by": { "type": "string", "enum": ["folder", "none"], "description": "folder (default): one result per product folder with sample items. none: individual entries." },
    "limit": { "type": "integer", "minimum": 1, "maximum": 100, "description": "Results per page (groups or entries). Default 30." },
    "show": { "type": "boolean", "description": "Also show ALL matches (not just this page) in the AI Search tab of the user's inventory window. Default false." },
    "offset": { "type": "integer", "minimum": 0, "description": "Skip this many results (use next_offset from the previous page)." }
  }
})");
        }

        json::object showSchema()
        {
            return parseObject(R"({
  "type": "object",
  "additionalProperties": false,
  "properties": {
    "ids": { "type": "array", "items": { "type": "string" }, "description": "Inventory item and folder ids (UUID text), e.g. the ids from inventory_search results. At most 20000." },
    "clear": { "type": "boolean", "description": "Empty the AI Search tab instead." }
  }
})");
        }

        json::object statusSchema()
        {
            return parseObject(R"({ "type": "object", "additionalProperties": false, "properties": {} })");
        }

        // ---- main-thread work ----

        ToolResult doStatus()
        {
            InvSource& source = InvSource::instance();
            const InvSource::Status s = source.status();
            if (!s.usable)
                return ToolResult::error("The inventory is not loaded yet (the viewer may still be logging in).");

            json::object o;
            o["entries"] = s.entries;
            o["folders"] = s.folders;
            o["everything_fetched"] = s.everythingFetched;
            o["fetch_in_progress"] = s.fetchBusy;
            o["unfetched_folders"] = s.unfetchedFolders;
            if (!s.unfetchedExamples.empty())
            {
                json::array examples;
                for (const std::string& e : s.unfetchedExamples)
                    examples.emplace_back(e);
                o["unfetched_examples"] = std::move(examples);
            }
            o["index_generation"] = s.generation;
            o["last_rebuild_ms"] = s.lastRebuildMs;
            o["last_sync_ms"] = s.lastSyncMs;
            if (!s.everythingFetched)
                o["note"] = "The viewer has not fetched the whole inventory yet; searches may miss items in unfetched folders.";
            return ToolResult::json(o);
        }

        ToolResult doSearch(SearchRequest request)
        {
            InvSource& source = InvSource::instance();
            if (!source.sync())
                return ToolResult::error("The inventory is not loaded yet (the viewer may still be logging in). Try again in a moment.");

            json::array warnings;
            if (!request.creator_name.empty())
            {
                // Creators are stored as ids: find the ones whose name contains the text.
                const std::string wanted = normalize(request.creator_name);
                std::size_t notReady = 0;
                for (const std::string& id : source.index().distinctCreators())
                {
                    bool known = false;
                    const std::string name = source.creatorName(id, &known);
                    if (!known)
                        ++notReady;
                    else if (normalize(name).find(wanted) != std::string::npos)
                        request.query.creator_ids.push_back(id);
                }
                if (notReady)
                {
                    warnings.emplace_back(std::to_string(notReady) + " creator names have not been looked up yet; "
                                          "the creator filter may miss some. Try again in a few seconds.");
                }
                if (request.query.creator_ids.empty())
                    request.query.creator_ids.push_back("-"); // nobody matches: no result, not "any creator"
            }

            const Result result = source.index().search(request.query);
            if (!result.error.empty())
                return ToolResult::error(result.error);

            const NameOf nameOf = [&source](const std::string& id)
            {
                bool known = false;
                std::string name = source.creatorName(id, &known);
                return known ? name : std::string();
            };
            json::object o = searchResultToJson(result, nameOf);

            const bool fetched = LLInventoryModelBackgroundFetch::instance().isEverythingFetched();
            if (!fetched)
            {
                warnings.emplace_back("The viewer has not finished loading the whole inventory; matches may be missing "
                                      "(inventory_status lists the folders not fetched yet).");
            }
            if (request.show)
            {
                const ShowResult shown = showInInventory(result.ids);
                if (shown.error.empty())
                    o["shown_in_inventory"] = shown.shown;
                else
                    warnings.emplace_back("Not shown in the inventory window: " + shown.error);
            }
            if (!warnings.empty())
                o["warnings"] = std::move(warnings);
            return ToolResult::json(o);
        }

        ToolResult doShow(ShowRequest request)
        {
            InvSource& source = InvSource::instance();
            if (!source.sync())
                return ToolResult::error("The inventory is not loaded yet (the viewer may still be logging in). Try again in a moment.");
            const ShowResult shown = request.clear ? clearInventoryTab() : showInInventory(request.ids);
            if (!shown.error.empty())
                return ToolResult::error(shown.error);
            json::object o;
            o["shown_in_inventory"] = shown.shown;
            if (request.clear)
                o["cleared"] = true;
            return ToolResult::json(o);
        }
    }

    std::vector<Tool> makeInvTools(const MainThreadRunner& run)
    {
        std::vector<Tool> tools;

        {
            Tool t;
            t.name = "inventory_status";
            t.title = "Inventory index status";
            t.description = kStatusDescription;
            t.inputSchema = statusSchema();
            t.readOnly = true;
            t.handler = [run](const json::object&) { return run([]() { return doStatus(); }, kStepTimeoutSeconds); };
            tools.push_back(std::move(t));
        }
        {
            Tool t;
            t.name = "inventory_search";
            t.title = "Search the inventory";
            t.description = kSearchDescription;
            t.inputSchema = searchSchema();
            t.readOnly = true;
            t.handler = [run](const json::object& arguments)
            {
                SearchRequest request;
                std::string error;
                if (!parseSearchArgs(arguments, request, error))
                    return ToolResult::error(error);
                return run([request]() { return doSearch(request); }, kStepTimeoutSeconds);
            };
            tools.push_back(std::move(t));
        }
        {
            Tool t;
            t.name = "inventory_show";
            t.title = "Show ids in the inventory window";
            t.description = kShowDescription;
            t.inputSchema = showSchema();
            t.readOnly = true; // it changes what the window shows, not the inventory
            t.handler = [run](const json::object& arguments)
            {
                ShowRequest request;
                std::string error;
                if (!parseShowArgs(arguments, request, error))
                    return ToolResult::error(error);
                return run([request]() { return doShow(request); }, kStepTimeoutSeconds);
            };
            tools.push_back(std::move(t));
        }
        return tools;
    }

    std::string serverInstructions()
    {
        return "Finds things in the user's Second Life inventory, which can hold tens of thousands of items. "
               "Use inventory_search with several lists of synonyms (English and Japanese) for a vague request such as "
               "\"clothes for Christmas\"; folder names matter as much as item names. Refine until the candidates "
               "look right, then tell the user the best few with their folder paths; to let the user browse the matches, "
               "search again with show=true (the \"AI Search\" tab of the inventory window). The tools never change the inventory.";
    }
}

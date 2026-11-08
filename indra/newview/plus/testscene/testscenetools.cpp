/**
 * @file testscenetools.cpp
 * @brief Test scene: the MCP tools testscene_build, testscene_clear, testscene_stats.
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

#include "testscenetools.h"

#include "testscene.h"
#include "testscenedef.h"
#include "posermcp/mcpschema.h"

#include "llsdjson.h"

namespace json = boost::json;
using plus::mcp::Tool;
using plus::mcp::ToolResult;
using plus::posermcp::JobProgress;

namespace
{
    constexpr double BUILD_TIMEOUT = 120.0;   // seconds: thousands of objects and textures on the main thread
    constexpr double QUICK_TIMEOUT = 20.0;
    constexpr size_t MAX_OVERRIDES = 64;

    using plus::mcp::ToolError;      // mcpschema.h (research/275 L110)
    using plus::mcp::prop;
    using plus::mcp::objectSchema;

    std::optional<ToolResult> refusedWhenOff()
    {
        if (!plus::testscene::enabled())
            return ToolResult::error("PlusTestScene is off: the test scene is for development (Debug Settings, or start the "
                                     "viewer with --set PlusTestScene 1). It is not saved.");
        return std::nullopt;
    }

    ToolResult statsResult()
    {
        LLSD stats = plus::testscene::stats();
        if (stats.isUndefined())
        {
            json::object none;
            none["built"]  = false;
            json::array names;
            for (const std::string& name : plus::testscene::sceneNames())
                names.push_back(json::string(name));
            none["scenes"] = std::move(names);
            return ToolResult::json(none);
        }
        stats["built"] = true;
        return ToolResult::json(LlsdToJson(stats));
    }
}

namespace plus::testscene
{
    void appendTools(std::vector<Tool>& tools, const plus::posermcp::MainThreadRunner& run)
    {
        {
            Tool t;
            t.name        = "testscene_build";
            t.title       = "Build a local test scene";
            t.description = "Builds a made-up load of objects that only this viewer has, above the avatar (development, for "
                            "repeatable frame-time measurements; PlusTestScene must be on). scene: a name of "
                            "app_settings/plus/testscene (heavy-leeaf, light, rain-alpha, prims-only) or a full .xml file name. "
                            "overrides: keys of the scene file over its values (seed, prims, meshes, alpha_sheets, alpha, "
                            "textures, texture_size, radius, vertical, height, clusters, texanim, ...). A scene already built "
                            "is removed first. Nothing is sent to the simulator. Returns the counts and the hash; look=true "
                            "also puts the camera where it sees the scene.";
            json::object p;
            p["scene"]     = prop("string", "Scene name (without .xml) or a full .xml file name.");
            p["overrides"] = prop("object", "Scene keys -> values, over the file's (e.g. {\"seed\": 2, \"prims\": 1000}).");
            p["look"]      = prop("boolean", "Put the camera where it sees the whole scene (default false).");
            t.inputSchema  = objectSchema(std::move(p), { "scene" });
            t.handler      = [run](const json::object& args) -> ToolResult
            {
                const json::value* scene = args.if_contains("scene");
                if (!scene || !scene->is_string() || scene->get_string().empty() || scene->get_string().size() > 1024)
                    return ToolResult::error("'scene' must be a scene name or an .xml file name");
                LLSD overrides = LLSD::emptyMap();
                if (const json::value* o = args.if_contains("overrides"); o && !o->is_null())
                {
                    if (!o->is_object() || o->get_object().size() > MAX_OVERRIDES)
                        return ToolResult::error("'overrides' must be an object of at most 64 scene keys");
                    overrides = LlsdFromJson(*o);
                }
                bool look = false;
                if (const json::value* l = args.if_contains("look"); l && !l->is_null())
                {
                    if (!l->is_bool())
                        return ToolResult::error("'look' must be true or false");
                    look = l->get_bool();
                }
                const std::string name(scene->get_string());
                return run([name, overrides, look](const JobProgress&) -> std::optional<ToolResult>
                           {
                               if (auto off = refusedWhenOff())
                                   return off;
                               std::string error;
                               if (!build(name, overrides, error))
                                   return ToolResult::error(error);
                               if (look && !lookAt(error))
                                   return ToolResult::error("built, but the camera: " + error);
                               return statsResult();
                           },
                           BUILD_TIMEOUT);
            };
            tools.push_back(std::move(t));
        }
        {
            Tool t;
            t.name        = "testscene_clear";
            t.title       = "Remove the local test scene";
            t.description = "Removes the test scene's objects (nothing when none is built). Its textures leave the texture "
                            "list by themselves about 30 seconds later.";
            t.inputSchema = objectSchema(json::object());
            t.handler     = [run](const json::object&) -> ToolResult
            {
                return run([](const JobProgress&) -> std::optional<ToolResult>
                           {
                               if (auto off = refusedWhenOff())
                                   return off;
                               const bool was = isBuilt();
                               clear("mcp");
                               json::object out;
                               out["cleared"] = was;
                               return ToolResult::json(out);
                           },
                           QUICK_TIMEOUT);
            };
            tools.push_back(std::move(t));
        }
        {
            Tool t;
            t.name        = "testscene_stats";
            t.title       = "Count the local test scene";
            t.description = "What the built test scene has now: objects alive and dead, drawables, how many are static in "
                            "the region's volume octree (should be all, once the geometry is built) and how many active or "
                            "on the active list (should be 0), spatial groups, faces, alpha faces, texture animations, "
                            "textures, the hash and the place. Without a scene: the scene names.";
            t.readOnly    = true;
            t.inputSchema = objectSchema(json::object());
            t.handler     = [run](const json::object&) -> ToolResult
            {
                return run([](const JobProgress&) -> std::optional<ToolResult>
                           {
                               if (auto off = refusedWhenOff())
                                   return off;
                               return statsResult();
                           },
                           QUICK_TIMEOUT);
            };
            tools.push_back(std::move(t));
        }
    }
}

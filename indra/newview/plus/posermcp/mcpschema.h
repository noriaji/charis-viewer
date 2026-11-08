/**
 * @file mcpschema.h
 * @brief The MCP tools' shared pieces: the error of a wrong argument and the JSON schema of the arguments.
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

#ifndef PLUS_POSERMCP_MCPSCHEMA_H
#define PLUS_POSERMCP_MCPSCHEMA_H

#include <boost/json.hpp>

#include <initializer_list>
#include <stdexcept>
#include <string>

// One copy for the tool files of plus/posermcp and plus/testscene (research/275 L110). settingstools.cpp keeps its
// own ToolError: selftest sim-mcpsettings takes the file's text out to build its rules on their own.
namespace plus::mcp
{
    // A wrong argument (or a refusal): its message goes back to the model as the tool's error.
    struct ToolError : public std::runtime_error
    {
        using std::runtime_error::runtime_error;
    };

    // One property of a tool's arguments.
    inline boost::json::object prop(const char* type, const std::string& description)
    {
        return boost::json::object{ { "type", type }, { "description", description } };
    }

    // The schema of a tool's arguments: these properties and no others, the required ones named.
    inline boost::json::object objectSchema(boost::json::object properties, std::initializer_list<const char*> required = {})
    {
        boost::json::object schema;
        schema["type"]                 = "object";
        schema["properties"]           = std::move(properties);
        schema["additionalProperties"] = false;
        if (required.size())
        {
            boost::json::array list;
            for (const char* r : required)
            {
                list.push_back(boost::json::string(r));
            }
            schema["required"] = std::move(list);
        }
        return schema;
    }
}

#endif // PLUS_POSERMCP_MCPSCHEMA_H

/**
 * @file posesolverjson.h
 * @brief The solve_pose JSON (goals.json of tools/poser-mcp) <-> posesolver.h.
 *        boost::json only, no viewer state.
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

#ifndef PLUS_POSERMCP_POSESOLVERJSON_H
#define PLUS_POSERMCP_POSESOLVERJSON_H

#include "posesolver.h"

#include <boost/json.hpp>

namespace plus::posermcp::solver
{
    // The spec object (start, free, lock, floor_z, goals, options, camera, sites_override)
    // as posesolver.py reads it. Keys it does not know are ignored (the tool's schema checks
    // them). Throws SolveError with a message for the model.
    Spec parseSpec(const boost::json::object& spec);

    // A point: "mWristLeft" or {"joint", "site"?, "offset"?, "normal"?} or {"mid": [...]}.
    PointSpec parsePoint(const boost::json::value& v, const std::string& what);

    // [x, y, z] (finite numbers), else SolveError.
    Vec3 parseVec3(const boost::json::value& v, const std::string& what);

    // The report as posesolver.report() (rotations as set_joints' Euler degrees),
    // predicted positions only when asked.
    boost::json::object resultToJson(const Result& result, bool withPredicted);

    boost::json::array vecJson(const Vec3& v, int decimals);
}

#endif // PLUS_POSERMCP_POSESOLVERJSON_H

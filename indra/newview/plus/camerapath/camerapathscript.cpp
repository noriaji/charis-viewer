/**
 * @file camerapathscript.cpp
 * @brief Camera path: reading a camera script (a JSON file made by hand or by a program, for example from a
 *        piece of music, research/music-video-camera.md) into a path.
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

// The script is one JSON object. Every key but "points" is optional:
//
//   {
//     "loop": false,
//     "countdown": 3,                         seconds of "3, 2, 1" before it starts
//     "coords": "region",                     "region" (default): places are in metres from the corner of the
//                                             region the avatar is in; "global": metres in the grid
//     "shake": { "kind": "handheld",          "none", "handheld", "walking", "running" or "quake"
//                "amount": 0.5, "speed": 0.5, "seed": 1 },   seed 0 to 999999
//     "points": [
//       { "time": 0.0,                        when the camera is at this point, in seconds from the start
//         "pos": [128, 128, 25],
//         "look": [140, 128, 22],             where it looks, or:
//         "subject": "self",                  an avatar ("self" or its id) and "part": "face"|"chest"|"body", or:
//         "yaw_deg": 0, "pitch_deg": 0,       a direction (0 = along +X, counter-clockwise from above)
//         "fov_deg": 50,                      or "fov" in radians; without one the camera's own angle is kept
//         "roll": 0,                          tilt in degrees, + to the right (-180 to 180)
//         "spin": 0,                          extra turns of the picture while it moves into this point (1 = one
//                                             full turn, + the way + tilt goes; -8 to 8)
//         "motion": "smooth",                 how it gets here: "smooth", "linear", "ease", "orbit" or "cut". An orbit goes
//                                             round what the point looks at ("look" or "subject") in an arc, facing it
//         "orbit": 0,                         laps on top of the short way round (-4 to 4); with "orbit" motion only
//         "hold": 0.0,                        seconds it stays at this point
//         "name": "wide" } ]
//   }
//
// The times are when the camera is at each point; the path's own numbers (the time each move takes,
// the time it stays) are made from them: a move takes the time between the previous point's leaving
// (its time plus its hold) and this point's time, and a cut makes the previous point stay until the cut.

#include "llviewerprecompiledheaders.h"

#include "camerapath.h"

#include "plus/photomode/photomode.h"

#include <algorithm>
#include <cmath>

#include <boost/json.hpp>

#include "plus/camerashot/shotmath.h"
#include "plus/camroll/camroll.h"

#include "llagent.h"
#include "llfile.h"
#include "llviewercamera.h"
#include "llviewerregion.h"

namespace plus::camerapath
{
    namespace
    {
        namespace json = boost::json;

        constexpr S64    MAX_SCRIPT_BYTES = 1024 * 1024;
        constexpr double MAX_SECONDS      = 600.0;
        constexpr double DEFAULT_LOOK_DISTANCE = 10.0;
        constexpr double DEFAULT_MOVE_SECONDS  = 3.0;
        constexpr double RADIANS = 0.017453292519943295;

        double numberOf(const json::object& object, const char* key, double fallback)
        {
            const auto it = object.find(key);
            if (it == object.end())
            {
                return fallback;
            }
            const json::value& value = it->value();
            double number = fallback;
            if (value.is_double())
            {
                number = value.get_double();
            }
            else if (value.is_int64())
            {
                number = (double)value.get_int64();
            }
            else if (value.is_uint64())
            {
                number = (double)value.get_uint64();
            }
            return std::isfinite(number) ? number : fallback;   // 1e999 reads as infinity
        }

        std::string textOf(const json::object& object, const char* key)
        {
            const auto it = object.find(key);
            if (it == object.end() || !it->value().is_string())
            {
                return std::string();
            }
            return std::string(it->value().get_string().c_str());
        }

        bool boolOf(const json::object& object, const char* key, bool fallback)
        {
            const auto it = object.find(key);
            return it != object.end() && it->value().is_bool() ? it->value().get_bool() : fallback;
        }

        // [x, y, z]
        bool vectorOf(const json::object& object, const char* key, double out[3])
        {
            const auto it = object.find(key);
            if (it == object.end() || !it->value().is_array() || it->value().get_array().size() != 3)
            {
                return false;
            }
            const json::array& array = it->value().get_array();
            for (size_t i = 0; i < 3; ++i)
            {
                const json::value& value = array[i];
                if (value.is_double())
                {
                    out[i] = value.get_double();
                }
                else if (value.is_int64())
                {
                    out[i] = (double)value.get_int64();
                }
                else
                {
                    return false;
                }
                if (!std::isfinite(out[i]))
                {
                    return false;
                }
            }
            return true;
        }

        struct Frame
        {
            bool global = false;
            LLViewerRegion* region = nullptr;
            plus::camerashot::FovLimits fov;   // what the camera can show in this window (shotmath.h)

            // False for a place no camera can be put at (camerapath.h saneRegionPlace / saneGlobalPlace).
            bool place(const double v[3], LLVector3d& out) const
            {
                if (global)
                {
                    out = LLVector3d(v[0], v[1], v[2]);
                }
                else
                {
                    if (!saneRegionPlace(v))
                    {
                        return false;   // checked first: (F32) of a number too large is infinite
                    }
                    out = region->getPosGlobalFromRegion(LLVector3((F32)v[0], (F32)v[1], (F32)v[2]));
                }
                return saneGlobalPlace(out);
            }
        };

        ShakeKind shakeKindOf(const std::string& name)
        {
            if (name == "handheld") return ShakeKind::Handheld;
            if (name == "walking")  return ShakeKind::Walking;
            if (name == "running")  return ShakeKind::Running;
            if (name == "quake")    return ShakeKind::Quake;
            return ShakeKind::None;
        }

        plus::camerashot::Part partOf(const std::string& name)
        {
            if (name == "chest") return plus::camerashot::Part::Chest;
            if (name == "body")  return plus::camerashot::Part::Body;
            return plus::camerashot::Part::Face;
        }

        // The points of the script into 'result'; false when one is not usable.
        bool readPoints(const json::array& points, const Frame& frame, Path& result)
        {
            double previous_time = 0.0;
            for (const json::value& item : points)
            {
                if (result.points.size() >= MAX_POINTS)
                {
                    break;   // the rest is left out
                }
                if (!item.is_object())
                {
                    return false;
                }
                const json::object& object = item.get_object();
                double position[3];
                if (!vectorOf(object, "pos", position))
                {
                    return false;
                }
                camerapath::Point point;
                point.name = textOf(object, "name");
                if (!frame.place(position, point.position))
                {
                    return false;   // a place out of any region
                }

                // where the camera looks
                double look[3];
                const std::string subject = textOf(object, "subject");
                if (!subject.empty())
                {
                    point.subject = subject == "self" ? gAgentID : LLUUID(subject);
                    if (point.subject.isNull())
                    {
                        return false;   // not an id
                    }
                    point.part = partOf(textOf(object, "part"));
                }
                if (vectorOf(object, "look", look))
                {
                    if (!frame.place(look, point.focus))
                    {
                        return false;
                    }
                }
                else if (point.subject.notNull() && plus::camerashot::subjectPlace(point.subject, point.part, point.focus))
                {
                    // the avatar's place now, for when it cannot be seen later
                }
                else
                {
                    const double yaw   = numberOf(object, "yaw_deg", 0.0) * RADIANS;
                    const double pitch = numberOf(object, "pitch_deg", 0.0) * RADIANS;
                    point.focus = point.position + LLVector3d(std::cos(yaw) * std::cos(pitch), std::sin(yaw) * std::cos(pitch), std::sin(pitch))
                                                       * DEFAULT_LOOK_DISTANCE;
                }

                if (object.contains("fov_deg"))
                {
                    point.fov = (float)(numberOf(object, "fov_deg", 0.0) * RADIANS);
                }
                else
                {
                    point.fov = (float)numberOf(object, "fov", 0.0);
                }
                // Only sensible values are kept; playing limits the angle to what the window takes (runFov).
                point.fov  = point.fov > 0.f ? llclamp(point.fov, MIN_POINT_FOV, MAX_POINT_FOV) : 0.f;
                point.roll = (float)std::remainder(numberOf(object, "roll", 0.0), 360.0);   // a tilt of 400 is 40
                point.spin = (float)std::round(std::clamp(numberOf(object, "spin", 0.0), -(double)MAX_SPIN, (double)MAX_SPIN));   // whole turns
                point.hold = std::clamp(numberOf(object, "hold", 0.0), 0.0, MAX_SECONDS);
                point.orbit = (float)std::round(std::clamp(numberOf(object, "orbit", 0.0), -(double)MAX_ORBIT, (double)MAX_ORBIT));   // whole laps

                // the time: the move into this point takes what is left of the gap after the previous point's stay
                // A point without a time comes DEFAULT_MOVE_SECONDS after the previous one has left.
                if (result.points.empty())
                {
                    previous_time = std::max(numberOf(object, "time", 0.0), 0.0);
                    point.seconds = 0.0;
                }
                else
                {
                    camerapath::Point& before = result.points.back();
                    const double time = std::max(numberOf(object, "time", previous_time + before.hold + DEFAULT_MOVE_SECONDS), 0.0);
                    const double gap = std::clamp(time - previous_time, 0.0, MAX_SECONDS);
                    const std::string motion = textOf(object, "motion");
                    if (motion == "cut")
                    {
                        point.seconds = 0.0;
                        before.hold   = std::max(before.hold, gap);   // it stays until the cut
                    }
                    else
                    {
                        // The stay before it is cut short when it would leave no time for the move: a move the
                        // script asks for does not turn into a cut. Two points at the same time are a cut.
                        if (before.hold >= gap)
                        {
                            before.hold = gap * 0.5;
                        }
                        point.seconds = std::max(gap - before.hold, 0.0);
                        point.motion  = motion == "linear" ? Motion::Linear : motion == "ease" ? Motion::Ease
                                      : motion == "orbit" ? Motion::Orbit : Motion::Smooth;
                    }
                    previous_time = time;
                }
                result.points.push_back(point);
            }
            return !result.points.empty();
        }
    }

    bool importScript(const std::string& file, Path& result, std::string& error)
    {
        error.clear();
        const S64 bytes = LLFile::size(file);
        if (bytes <= 0 || bytes > MAX_SCRIPT_BYTES)
        {
            error = "path_err_read";
            return false;
        }
        std::string text((size_t)bytes, '\0');
        {
            llifstream stream(file.c_str(), std::ios::binary);
            if (!stream.is_open() || !stream.read(text.data(), (std::streamsize)bytes))
            {
                error = "path_err_read";
                return false;
            }
        }
        // A UTF-8 BOM in front (Notepad, PowerShell 5's Out-File) is not JSON: left out.
        if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0)
        {
            text.erase(0, 3);
        }
        boost::system::error_code ec;
        const json::value root = json::parse(text, ec);
        if (ec || !root.is_object())
        {
            error = "path_err_script";
            return false;
        }
        const json::object& object = root.get_object();

        Frame frame;
        frame.global = textOf(object, "coords") == "global";
        frame.region = gAgent.getRegion();
        const LLViewerCamera* camera = LLViewerCamera::getInstance();
        frame.fov    = plus::camerashot::fovLimits(camera->getMinView(), camera->getMaxView()); // the window's aspect
        if (!frame.global && !frame.region)
        {
            error = "path_err_no_camera";   // places in the region need the region
            return false;
        }
        const auto points = object.find("points");
        Path read;
        if (points == object.end() || !points->value().is_array() || !readPoints(points->value().get_array(), frame, read))
        {
            error = "path_err_script";
            return false;
        }
        read.loop      = boolOf(object, "loop", false);
        read.countdown = (int)std::clamp(numberOf(object, "countdown", 0.0), 0.0, (double)MAX_COUNTDOWN);
        read.region    = frame.global ? std::string() : plus::photomode::regionName();
        const auto shake = object.find("shake");
        if (shake != object.end() && shake->value().is_object())
        {
            const json::object& s = shake->value().get_object();
            read.shake.kind   = shakeKindOf(textOf(s, "kind"));
            read.shake.amount = std::clamp(numberOf(s, "amount", 0.5), 0.0, 1.0);
            read.shake.speed  = std::clamp(numberOf(s, "speed", 0.5), 0.0, 1.0);
            read.shake.seed   = (unsigned)std::clamp(numberOf(s, "seed", 1.0), 0.0, (double)MAX_SEED);
        }
        result = std::move(read);
        return true;
    }
}

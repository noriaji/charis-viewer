/**
 * @file posesolverjson.cpp
 * @brief The solve_pose JSON <-> posesolver.h. See posesolverjson.h.
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

#include "linden_common.h"

#include "posesolverjson.h"

#include <cmath>
#include <limits>

namespace json = boost::json;

namespace plus::posermcp::solver
{
    namespace
    {
        constexpr size_t MAX_GOALS = 200;

        bool number(const json::value& v, double& out)
        {
            if (v.is_double())
                out = v.get_double();
            else if (v.is_int64())
                out = (double)v.get_int64();
            else if (v.is_uint64())
                out = (double)v.get_uint64();
            else
                return false;
            // Finite and within F32: the solver works in F32, where 1e39 would become inf.
            return std::isfinite(out) && std::fabs(out) <= std::numeric_limits<F32>::max();
        }

        std::optional<double> optNumber(const json::object& o, const char* key, const std::string& what)
        {
            const json::value* v = o.if_contains(key);
            if (!v || v->is_null())
                return std::nullopt;
            double d = 0.0;
            if (!number(*v, d))
                throw SolveError(what + ": '" + key + "' must be a finite number");
            return d;
        }

        double numberOr(const json::object& o, const char* key, double fallback, const std::string& what)
        {
            return optNumber(o, key, what).value_or(fallback);
        }

        // A tolerance: finite and above zero (0 or less would never be met, and
        // the solver divides by some of them).
        std::optional<double> optPositive(const json::object& o, const char* key, const std::string& what)
        {
            const std::optional<double> d = optNumber(o, key, what);
            if (d && !(*d > 0.0))
                throw SolveError(what + ": '" + key + "' must be a positive number");
            return d;
        }

        double positiveOr(const json::object& o, const char* key, double fallback, const std::string& what)
        {
            return optPositive(o, key, what).value_or(fallback);
        }

        std::string stringOr(const json::object& o, const char* key, const std::string& fallback, const std::string& what)
        {
            const json::value* v = o.if_contains(key);
            if (!v || v->is_null())
                return fallback;
            if (!v->is_string())
                throw SolveError(what + ": '" + key + "' must be a string");
            return std::string(v->get_string());
        }

        bool boolOr(const json::object& o, const char* key, bool fallback, const std::string& what)
        {
            const json::value* v = o.if_contains(key);
            if (!v || v->is_null())
                return fallback;
            if (!v->is_bool())
                throw SolveError(what + ": '" + key + "' must be true or false");
            return v->get_bool();
        }

        std::optional<Vec3> optVec3(const json::object& o, const char* key, const std::string& what)
        {
            const json::value* v = o.if_contains(key);
            if (!v || v->is_null())
                return std::nullopt;
            return parseVec3(*v, what + " " + key);
        }

        // A direction that is normalized later: not (nearly) zero.
        std::optional<Vec3> optNormal(const json::object& o, const char* key, const std::string& what)
        {
            const std::optional<Vec3> n = optVec3(o, key, what);
            if (n && !(norm(*n) > 1e-9))
                throw SolveError(what + ": '" + key + "' must not be a zero vector");
            return n;
        }

        void pair2(const json::value& v, double out[2], const std::string& what)
        {
            if (!v.is_array() || v.get_array().size() != 2 || !number(v.get_array()[0], out[0]) || !number(v.get_array()[1], out[1]))
                throw SolveError(what + " must be [a, b] with two finite numbers");
        }

        std::vector<std::string> stringList(const json::value& v, const std::string& what)
        {
            std::vector<std::string> out;
            if (v.is_string())
            {
                out.emplace_back(v.get_string());
                return out;
            }
            if (!v.is_array())
                throw SolveError(what + " must be an array of joint names");
            for (const json::value& s : v.get_array())
            {
                if (!s.is_string())
                    throw SolveError(what + " must be an array of joint names");
                out.emplace_back(s.get_string());
            }
            return out;
        }

        CameraSpec parseCamera(const json::value& v, const std::string& what)
        {
            if (!v.is_object())
                throw SolveError(what + " must be an object {azimuth, elevation, roll?, fov?, distance?, focus?, size?, center?}");
            const json::object& o = v.get_object();
            CameraSpec          c;
            c.azimuth   = numberOr(o, "azimuth", 0.0, what);
            c.elevation = numberOr(o, "elevation", 0.0, what);
            c.roll      = numberOr(o, "roll", 0.0, what);
            c.fov       = optNumber(o, "fov", what);
            c.distance  = optNumber(o, "distance", what);
            if (std::optional<Vec3> f = optVec3(o, "focus", what))
                c.focus = *f;
            if (const json::value* s = o.if_contains("size"); s && !s->is_null())
            {
                double wh[2];
                pair2(*s, wh, what + " size");
                if (!(wh[0] > 0.0) || !(wh[1] > 0.0))
                    throw SolveError(what + ": size must be [width, height] above zero");
                c.width  = wh[0];
                c.height = wh[1];
            }
            if (const json::value* s = o.if_contains("center"); s && !s->is_null())
            {
                double xy[2];
                pair2(*s, xy, what + " center");
                c.center = Vec3(xy[0], xy[1], 0.0);
            }
            if (c.fov && !(*c.fov > 1.0 && *c.fov < 179.0))
                throw SolveError(what + ": fov must be between 1 and 179 degrees");
            if (c.distance && !(*c.distance > 0.0))
                throw SolveError(what + ": distance must be positive");
            return c;
        }

        GoalType goalType(const std::string& t)
        {
            static const std::map<std::string, GoalType> types{
                { "bone_dir", GoalType::BoneDir },         { "contact", GoalType::Contact },
                { "on_floor", GoalType::OnFloor },         { "above_floor", GoalType::AboveFloor },
                { "anchor", GoalType::Anchor },            { "joint_range", GoalType::JointRange },
                { "bone_elevation", GoalType::BoneElevation }, { "torso_lean", GoalType::TorsoLean },
                { "balance", GoalType::Balance },          { "look", GoalType::Look },
                { "image_dir", GoalType::ImageDir },       { "image_point", GoalType::ImagePoint },
                { "smooth_spine", GoalType::SmoothSpine },
            };
            auto it = types.find(t);
            if (it == types.end())
                throw SolveError("unknown goal type '" + t + "' (bone_dir, contact, on_floor, above_floor, anchor, joint_range, "
                                 "bone_elevation, torso_lean, balance, look, image_dir, image_point)");
            return it->second;
        }

        PointSpec requirePoint(const json::object& o, const char* key, const std::string& what)
        {
            const json::value* v = o.if_contains(key);
            if (!v || v->is_null())
                throw SolveError(what + ": '" + key + "' is required");
            return parsePoint(*v, what + " " + key);
        }

        Goal parseGoal(const json::object& o, size_t index)
        {
            const std::string type = stringOr(o, "type", "", "goal " + std::to_string(index));
            Goal              g;
            g.type                 = goalType(type);
            g.name                 = stringOr(o, "name", "", type);
            const std::string what = g.name.empty() ? type + " (goal " + std::to_string(index) + ")" : "goal '" + g.name + "'";
            g.weight               = optNumber(o, "weight", what);
            g.tolerance            = optPositive(o, "tolerance", what);
            g.toleranceDeg         = optPositive(o, "tolerance_deg", what);
            switch (g.type)
            {
                case GoalType::BoneDir:
                {
                    g.joint = stringOr(o, "joint", "", what);
                    if (g.joint.empty())
                        throw SolveError(what + ": 'joint' is required");
                    const json::value* d = o.if_contains("direction");
                    if (!d)
                        throw SolveError(what + ": 'direction' is required");
                    g.direction  = parseVec3(*d, what + " direction");
                    g.toward     = stringOr(o, "toward", "", what);
                    g.rollToward = optVec3(o, "roll_toward", what);
                    g.rollWeight = optNumber(o, "roll_weight", what);
                    g.withinDeg  = optNumber(o, "within_deg", what);
                    break;
                }
                case GoalType::Contact:
                    g.a            = requirePoint(o, "a", what);
                    g.b            = requirePoint(o, "b", what);
                    g.distance     = numberOr(o, "distance", 0.0, what);
                    g.skinMargin   = numberOr(o, "skin_margin", 0.0, what);
                    g.hard         = boolOr(o, "hard", true, what);
                    g.facing       = boolOr(o, "facing", false, what);
                    g.facingWeight = numberOr(o, "facing_weight", 0.2, what);
                    break;
                case GoalType::OnFloor:
                    g.at               = requirePoint(o, "at", what);
                    g.flat             = boolOr(o, "flat", false, what);
                    g.flatToleranceDeg = positiveOr(o, "flat_tolerance_deg", 2.0, what);
                    g.floorZ           = optNumber(o, "floor_z", what);
                    g.height           = numberOr(o, "height", 0.0, what);
                    break;
                case GoalType::AboveFloor:
                {
                    const json::value* at = o.if_contains("at");
                    if (!at || !at->is_array() || at->get_array().empty())
                        throw SolveError(what + ": 'at' must be a non-empty array of points");
                    for (const json::value& p : at->get_array())
                        g.atList.push_back(parsePoint(p, what + " at"));
                    break;
                }
                case GoalType::Anchor:
                {
                    g.at = requirePoint(o, "at", what);
                    if (const json::value* p = o.if_contains("position"); p && !p->is_null())
                    {
                        if (p->is_string())
                        {
                            if (p->get_string() != "current")
                                throw SolveError(what + ": position must be \"current\" or [x, y, z]");
                        }
                        else
                        {
                            g.position = parseVec3(*p, what + " position");
                        }
                    }
                    g.orient             = boolOr(o, "orient", false, what);
                    g.orientToleranceDeg = positiveOr(o, "orient_tolerance_deg", 1.0, what);
                    if (const json::value* t = o.if_contains("orient_target"); t && !t->is_null())
                    {
                        if (!t->is_array() || t->get_array().size() != 3)
                            throw SolveError(what + ": orient_target must be a 3x3 matrix (rows)");
                        Mat3 m;
                        for (int i = 0; i < 3; ++i)
                        {
                            const Vec3 r = parseVec3(t->get_array()[i], what + " orient_target");
                            for (int j = 0; j < 3; ++j)
                                m.m[i][j] = r[j];
                        }
                        g.orientTarget = m;
                    }
                    break;
                }
                case GoalType::JointRange:
                    g.joint  = stringOr(o, "joint", "", what);
                    g.motion = stringOr(o, "motion", "", what);
                    g.min    = optNumber(o, "min", what);
                    g.max    = optNumber(o, "max", what);
                    if (g.joint.empty() || g.motion.empty())
                        throw SolveError(what + ": 'joint' and 'motion' are required");
                    break;
                case GoalType::BoneElevation:
                    g.joint  = stringOr(o, "joint", "", what);
                    g.toward = stringOr(o, "toward", "", what);
                    g.min    = optNumber(o, "min", what);
                    g.max    = optNumber(o, "max", what);
                    if (g.joint.empty())
                        throw SolveError(what + ": 'joint' is required");
                    break;
                case GoalType::TorsoLean:
                    g.min        = optNumber(o, "min", what);
                    g.max        = optNumber(o, "max", what);
                    g.forwardDeg = optNumber(o, "forward_deg", what);
                    g.sideDeg    = optNumber(o, "side_deg", what);
                    break;
                case GoalType::Balance:
                {
                    g.supportSite = stringOr(o, "site", "sole", what);
                    if (const json::value* s = o.if_contains("support"); s && !s->is_null())
                    {
                        if (!s->is_array())
                            throw SolveError(what + ": 'support' must be an array");
                        for (const json::value& p : s->get_array())
                        {
                            if (p.is_string())
                            {
                                PointSpec ps;
                                ps.joint = std::string(p.get_string());
                                ps.site  = g.supportSite;
                                g.support.push_back(ps);
                            }
                            else
                            {
                                g.support.push_back(parsePoint(p, what + " support"));
                            }
                        }
                    }
                    break;
                }
                case GoalType::Look:
                {
                    const json::value* t = o.if_contains("toward");
                    if (!t)
                        throw SolveError(what + ": 'toward' is required");
                    g.lookToward = parseVec3(*t, what + " toward");
                    break;
                }
                case GoalType::ImageDir:
                {
                    g.label = stringOr(o, "label", "", what);
                    if (const json::value* a = o.if_contains("axis"); a && !a->is_null())
                    {
                        if (!a->is_object())
                            throw SolveError(what + ": axis must be {\"joint\", \"local\": [x, y, z]}");
                        g.axisJoint = stringOr(a->get_object(), "joint", "", what + " axis");
                        const json::value* l = a->get_object().if_contains("local");
                        if (g.axisJoint.empty() || !l)
                            throw SolveError(what + ": axis needs joint and local");
                        g.axisLocal = parseVec3(*l, what + " axis local");
                    }
                    else
                    {
                        g.from = requirePoint(o, "from", what);
                        g.to   = requirePoint(o, "to", what);
                    }
                    const json::value* pv = o.if_contains("photo_vec");
                    if (!pv || pv->is_null())
                        throw SolveError(what + ": give photo_vec [dx, dy] (pixels in the photo, y down). The viewer has no "
                                         "photo: tools/poser-mcp/solve_pose_cli.py --evidence turns photo keypoints into photo_vec");
                    pair2(*pv, g.photoVec, what + " photo_vec");
                    if (const json::value* c = o.if_contains("camera"); c && !c->is_null())
                        g.camera = parseCamera(*c, what + " camera");
                    break;
                }
                case GoalType::ImagePoint:
                {
                    if (o.if_contains("at"))
                    {
                        g.at = requirePoint(o, "at", what);
                    }
                    else
                    {
                        g.joint    = stringOr(o, "joint", "", what);
                        g.at.joint = g.joint;
                        g.at.site  = stringOr(o, "site", "", what);
                        if (g.joint.empty())
                            throw SolveError(what + ": 'at' or 'joint' is required");
                    }
                    const json::value* px = o.if_contains("px");
                    if (!px)
                        throw SolveError(what + ": 'px' is required");
                    pair2(*px, g.px, what + " px");
                    g.mPerPx = optNumber(o, "m_per_px", what);
                    if (const json::value* c = o.if_contains("camera"); c && !c->is_null())
                        g.camera = parseCamera(*c, what + " camera");
                    break;
                }
                case GoalType::SmoothSpine:
                    break;
            }
            return g;
        }

        SiteOverrides parseOverrides(const json::value& v)
        {
            if (!v.is_object())
                throw SolveError("sites_override must be an object {site: {side: {anchor?, frame?, offset?, normal?, along?}}}");
            const json::object* o = &v.get_object();
            if (const json::value* inner = o->if_contains("sites"); inner && inner->is_object())
                o = &inner->get_object();
            SiteOverrides out;
            for (const auto& kv : *o)
            {
                const std::string name(kv.key());
                if (name.empty() || name[0] == '_' || !kv.value().is_object())
                    continue;
                for (const auto& side : kv.value().get_object())
                {
                    if (!side.value().is_object())
                        continue;
                    const json::object& f    = side.value().get_object();
                    const std::string   what = "sites_override " + name + " " + std::string(side.key());
                    SiteOverride        so;
                    if (const json::value* a = f.if_contains("anchor"); a && a->is_string())
                        so.anchor = std::string(a->get_string());
                    if (const json::value* a = f.if_contains("frame"); a && a->is_string())
                        so.frame = std::string(a->get_string());
                    so.offset = optVec3(f, "offset", what);
                    so.normal = optNormal(f, "normal", what);
                    so.along  = optVec3(f, "along", what);
                    out[name][std::string(side.key())] = so;
                }
            }
            return out;
        }

        Options parseOptions(const json::value& v)
        {
            Options o;
            if (v.is_null())
                return o;
            if (!v.is_object())
                throw SolveError("options must be an object");
            const json::object& j = v.get_object();
            const std::string   what = "options";
            o.flexibility = stringOr(j, "flexibility", "normal", what);
            if (o.flexibility != "normal" && o.flexibility != "athletic" && o.flexibility != "dancer")
                throw SolveError("options.flexibility must be normal, athletic or dancer");
            auto intIn = [&](const char* key, int fallback, int lo, int hi)
            {
                const double d = numberOr(j, key, fallback, what);
                if (d < lo || d > hi || d != std::floor(d))
                    throw SolveError(std::string("options.") + key + " must be a whole number from " + std::to_string(lo) + " to " +
                                     std::to_string(hi));
                return (int)d;
            };
            o.starts   = intIn("starts", 6, 1, 12);
            o.seed     = (unsigned)intIn("seed", 1, 0, 1000000);
            o.maxOuter = intIn("max_outer", 6, 1, 10);
            o.maxIter  = intIn("max_iter", 60, 1, 500);
            o.stay           = numberOr(j, "stay", 1.0, what);
            o.comfort        = numberOr(j, "comfort", 0.05, what);
            o.spineSmooth    = numberOr(j, "spine_smooth", 0.3, what);
            o.maxRotationDeg = numberOr(j, "max_rotation_deg", 120.0, what);
            o.ranges         = boolOr(j, "ranges", true, what);
            o.floorGuard     = boolOr(j, "floor_guard", true, what);
            if (o.stay < 0.0 || o.comfort < 0.0 || o.spineSmooth < 0.0 || !(o.maxRotationDeg > 0.0))
                throw SolveError("options: stay, comfort and spine_smooth must not be negative, max_rotation_deg positive");
            if (const json::value* sw = j.if_contains("stay_weight"); sw && !sw->is_null())
            {
                if (!sw->is_object())
                    throw SolveError("options.stay_weight must be an object {joint or part: weight}");
                for (const auto& kv : sw->get_object())
                {
                    double d = 0.0;
                    if (!number(kv.value(), d) || d < 0.0)
                        throw SolveError("options.stay_weight values must be numbers >= 0");
                    o.stayWeight[std::string(kv.key())] = d;
                }
            }
            return o;
        }

        double rounded(double v, int decimals)
        {
            const double f = std::pow(10.0, decimals);
            const double r = std::round(v * f) / f;
            return r == 0.0 ? 0.0 : r;
        }

        json::value numberOrNull(double v, int decimals)
        {
            if (std::isnan(v))
                return nullptr;
            return rounded(v, decimals);
        }

        json::value optJson(const std::optional<double>& v)
        {
            if (!v)
                return nullptr;
            return *v;
        }

        json::array eulerJson(const Mat3& m) { return vecJson(eulerDegrees(m), 3); }
    }

    Vec3 parseVec3(const json::value& v, const std::string& what)
    {
        double x = 0.0, y = 0.0, z = 0.0;
        if (v.is_array() && v.get_array().size() == 3)
        {
            const json::array& a = v.get_array();
            if (number(a[0], x) && number(a[1], y) && number(a[2], z))
                return Vec3(x, y, z);
        }
        throw SolveError(what + " must be [x, y, z] with three finite numbers");
    }

    PointSpec parsePoint(const json::value& v, const std::string& what)
    {
        PointSpec p;
        if (v.is_string())
        {
            p.joint = std::string(v.get_string());
            return p;
        }
        if (!v.is_object())
            throw SolveError(what + " must be a joint name or {\"joint\", \"site\"?, \"offset\"?} or {\"mid\": [...]}");
        const json::object& o = v.get_object();
        if (const json::value* mid = o.if_contains("mid"); mid && !mid->is_null())
        {
            if (!mid->is_array() || mid->get_array().empty() || mid->get_array().size() > 8)
                throw SolveError(what + ": mid must be an array of 1-8 points");
            for (const json::value& m : mid->get_array())
                p.mid.push_back(parsePoint(m, what + " mid"));
            return p;
        }
        p.joint  = stringOr(o, "joint", "", what);
        p.site   = stringOr(o, "site", "", what);
        p.offset = optVec3(o, "offset", what);
        p.normal = optNormal(o, "normal", what);
        if (p.joint.empty())
            throw SolveError(what + ": 'joint' is required");
        // contact's words (as solve_pose_cli.py turns contacts into goals): joint = the joint centre,
        // surface = the palm of a hand, the top of a foot, else the joint centre
        if (p.site == "joint")
            p.site.clear();
        else if (p.site == "surface")
            p.site = p.joint.rfind("mWrist", 0) == 0 ? "palm"
                   : (p.joint.rfind("mAnkle", 0) == 0 || p.joint.rfind("mFoot", 0) == 0) ? "instep"
                                                                                           : "";
        return p;
    }

    Spec parseSpec(const json::object& o)
    {
        Spec s;
        s.start = stringOr(o, "start", "current", "spec");
        if (s.start != "current" && s.start != "tpose")
            throw SolveError("start must be \"current\" or \"tpose\"");
        if (const json::value* f = o.if_contains("free"); f && !f->is_null())
        {
            if (f->is_string() && f->get_string() == "auto")
            {
                s.freeAuto = true;
            }
            else
            {
                s.freeAuto = false;
                s.free     = stringList(*f, "free");
                if (s.free.empty())
                    throw SolveError("free must be \"auto\" or a non-empty array of joint names");
            }
        }
        if (const json::value* l = o.if_contains("lock"); l && !l->is_null())
            s.lock = stringList(*l, "lock");
        s.lockPosition = boolOr(o, "lock_position", false, "spec");
        if (const json::value* f = o.if_contains("floor_z"); f && !f->is_null())
        {
            double z = 0.0;
            if (f->is_string() && (f->get_string() == "rest" || f->get_string() == "lowest"))
            {
                s.floorMode = std::string(f->get_string());
            }
            else if (number(*f, z))
            {
                s.floorMode = "value";
                s.floorZ    = z;
            }
            else
            {
                throw SolveError("floor_z must be \"rest\", \"lowest\" or a height (m, body frame)");
            }
        }
        if (const json::value* g = o.if_contains("goals"); g && !g->is_null())
        {
            if (!g->is_array())
                throw SolveError("goals must be an array");
            if (g->get_array().size() > MAX_GOALS)
                throw SolveError("too many goals (at most " + std::to_string(MAX_GOALS) + ")");
            size_t i = 0;
            for (const json::value& item : g->get_array())
            {
                if (!item.is_object())
                    throw SolveError("each goal must be an object with a type");
                s.goals.push_back(parseGoal(item.get_object(), i++));
            }
        }
        if (const json::value* op = o.if_contains("options"))
            s.options = parseOptions(*op);
        if (const json::value* c = o.if_contains("camera"); c && !c->is_null())
            s.camera = parseCamera(*c, "camera");
        if (const json::value* so = o.if_contains("sites_override"); so && !so->is_null())
        {
            if (so->is_string())
                throw SolveError("sites_override must be the object itself (the viewer does not read files)");
            s.sitesOverride = parseOverrides(*so);
        }
        return s;
    }

    json::array vecJson(const Vec3& v, int decimals)
    {
        return json::array{ rounded(v.x, decimals), rounded(v.y, decimals), rounded(v.z, decimals) };
    }

    json::object resultToJson(const Result& r, bool withPredicted)
    {
        json::object out;
        out["status"]         = r.status;
        out["start"]          = r.start;
        out["iterations"]     = r.iterations;
        out["soft_cost"]      = r.softCost;
        out["hard_violation"] = r.hardViolation;
        json::array goals;
        for (const GoalReport& g : r.goals)
        {
            json::object e;
            e["name"]  = g.name;
            e["kind"]  = g.kind;
            e["unit"]  = g.unit;
            e["value"] = g.value;
            for (const GoalReport::Extra& x : g.extras)
            {
                if (x.list)
                {
                    json::array a;
                    for (double d : x.values)
                        a.push_back(numberOrNull(d, 4));
                    e[x.key] = std::move(a);
                }
                else if (!x.values.empty())
                {
                    e[x.key] = numberOrNull(x.values[0], 4);
                }
            }
            if (g.tolerance)
                e["tolerance"] = *g.tolerance;
            if (g.met)
                e["met"] = *g.met;
            goals.push_back(std::move(e));
        }
        out["goals"] = std::move(goals);
        json::array unmet;
        for (const Result::Unmet& u : r.unmet)
        {
            json::object e;
            e["name"] = u.name;
            if (u.range.empty())
            {
                e["value"]     = u.value;
                e["unit"]      = u.unit;
                e["tolerance"] = u.tolerance;
            }
            else
            {
                json::object values, limits;
                for (const Anatomy& a : u.range)
                {
                    values[a.motion] = a.value;
                    if (a.lo || a.hi)
                        limits[a.motion] = json::array{ optJson(a.lo), optJson(a.hi) };
                }
                e["values"] = std::move(values);
                e["limits"] = std::move(limits);
            }
            unmet.push_back(std::move(e));
        }
        out["unmet"] = std::move(unmet);
        json::array warnings;
        for (const std::string& w : r.warnings)
            warnings.push_back(json::string(w));
        out["warnings"] = std::move(warnings);
        json::array hits;
        for (const Result::RangeHit& h : r.rangeHits)
        {
            json::object motions;
            for (const Anatomy& a : h.motions)
                motions[a.motion] = json::array{ a.value, optJson(a.lo), optJson(a.hi) };
            hits.push_back(json::object{ { "joint", h.joint }, { "motions", std::move(motions) } });
        }
        out["range_hits"] = std::move(hits);
        json::object anatomy;
        for (const auto& [joint, values] : r.anatomy)
        {
            json::object v;
            for (const Anatomy& a : values)
                v[a.motion] = a.value;
            anatomy[joint] = std::move(v);
        }
        out["anatomy"] = std::move(anatomy);
        json::object joints;
        for (const Result::Joint& j : r.joints)
        {
            json::object e;
            e["rotation"]   = eulerJson(j.local);
            e["change_deg"] = j.changeDeg;
            e["angle_deg"]  = j.angleDeg;
            if (j.twistDeg)
                e["twist_deg"] = *j.twistDeg;
            joints[j.name] = std::move(e);
        }
        out["joints"] = std::move(joints);
        if (r.pelvisFree)
        {
            out["pelvis_position"] = vecJson(r.pelvisPosition, 4);
            out["pelvis_moved_cm"] = vecJson(r.pelvisMovedCm, 2);
        }
        out["floor_z"] = r.floorZ ? json::value(rounded(*r.floorZ, 4)) : json::value(nullptr);
        json::array starts;
        for (const Result::StartSummary& s : r.starts)
            starts.push_back(json::object{ { "start", s.name },
                                           { "status", s.status },
                                           { "soft", rounded(s.soft, 2) },
                                           { "violation", rounded(s.violation, 2) },
                                           { "iterations", s.iterations } });
        out["starts"] = std::move(starts);
        json::array alts;
        for (const Result::Alternative& a : r.alternatives)
        {
            json::object j;
            for (const auto& [name, m] : a.joints)
                j[name] = eulerJson(m);
            alts.push_back(json::object{
                { "start", a.start }, { "soft", rounded(a.soft, 2) }, { "differs_deg", a.differsDeg }, { "joints", std::move(j) } });
        }
        out["alternatives"] = std::move(alts);
        if (withPredicted)
        {
            json::object p;
            for (const auto& [name, v] : r.predicted)
                p[name] = vecJson(v, 4);
            out["predicted_positions"] = std::move(p);
        }
        return out;
    }
}

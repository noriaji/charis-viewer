/**
 * @file posenudge.cpp
 * @brief The nudge tool's maths. See posenudge.h.
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

#include "posenudge.h"

#include "posesolvertables.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>

namespace plus::posermcp::nudge
{
    namespace
    {
        constexpr double PI  = 3.14159265358979323846;
        constexpr double DEG = 180.0 / PI;
        constexpr double RAD = PI / 180.0;

        // a turn whose bone points within this of the opposite way uses the fallback axis
        constexpr double OPPOSITE_DEG = 175.0;

        bool startsWith(const std::string& s, const std::string& p) { return s.size() >= p.size() && s.compare(0, p.size(), p) == 0; }

        struct Table
        {
            std::vector<Motion>                                        motions;
            std::vector<std::pair<std::string, std::pair<std::string, std::string>>> aliases; // family -> (alias, name)
            std::vector<std::string>                                   families;
        };

        const Table& table()
        {
            static const Table t = []
            {
                Table out;
                for (const tables::FamilyRow& f : tables::FAMILIES)
                    out.families.push_back(f.family);
                for (const tables::MotionRow& r : tables::MOTIONS)
                {
                    Motion m;
                    m.family  = r.family;
                    m.name    = r.name;
                    m.kind    = r.kind;
                    m.axis    = Vec3(r.axis[0], r.axis[1], r.axis[2]);
                    m.toward  = Vec3(r.toward[0], r.toward[1], r.toward[2]);
                    m.sign    = r.sign;
                    m.segment = r.segment;
                    m.check   = r.check;
                    for (const tables::FamilyRow& f : tables::FAMILIES)
                    {
                        if (m.family == f.family)
                        {
                            m.sided = f.sided;
                            m.ref   = f.ref;
                            m.what  = f.what;
                        }
                    }
                    out.motions.push_back(m);
                }
                for (const tables::AliasRow& a : tables::ALIASES)
                    out.aliases.push_back({ a.family, { a.alias, a.motion } });
                return out;
            }();
            return t;
        }

        std::string lower(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            return s;
        }

        Vec3 mirrorAxis(const Vec3& a) { return Vec3(-a.x, a.y, -a.z); }
        Vec3 mirrorDir(const Vec3& d) { return Vec3(d.x, -d.y, d.z); }
    }

    std::string familyOf(const std::string& joint)
    {
        std::string best;
        for (const std::string& f : table().families)
        {
            if (joint == f)
                return f;
            if (!startsWith(joint, f))
                continue;
            const std::string rest = joint.substr(f.size());
            const bool ok = rest == "Left" || rest == "Right" ||
                            (rest.size() > 1 && rest[0] >= '1' && rest[0] <= '3' && (rest.substr(1) == "Left" || rest.substr(1) == "Right"));
            if (ok && f.size() > best.size())
                best = f;
        }
        return best;
    }

    int segmentOf(const std::string& joint)
    {
        const std::string f = familyOf(joint);
        if (!startsWith(f, "mHand") || joint.size() <= f.size())
            return 0;
        const char c = joint[f.size()];
        return (c >= '1' && c <= '3') ? c - '0' : 0;
    }

    std::string wordsOf(const std::string& joint)
    {
        const std::string f = familyOf(joint);
        std::string       names, aliases;
        for (const Motion& m : table().motions)
        {
            if (m.family == f)
                names += (names.empty() ? "" : ", ") + m.name;
        }
        for (const auto& [fam, a] : table().aliases)
        {
            if (fam == f)
                aliases += (aliases.empty() ? "" : ", ") + a.first;
        }
        if (names.empty())
            return "(none)";
        return names + (aliases.empty() ? "" : " (also " + aliases + ")");
    }

    const Motion& findMotion(const std::string& joint, const std::string& word)
    {
        const std::string f = familyOf(joint);
        if (f.empty())
            throw NudgeError("nudge has no words for " + joint + " (thumbs, face, tail ...: use set_joints)");
        std::string name = lower(word);
        for (const auto& [fam, a] : table().aliases)
        {
            if (fam == f && a.first == name)
                name = a.second;
        }
        for (const Motion& m : table().motions)
        {
            if (m.family != f || m.name != name)
                continue;
            const int seg = segmentOf(joint);
            if (m.segment && seg && seg != m.segment)
                throw NudgeError(word + " only works on segment " + std::to_string(m.segment) + " of a finger (" + joint + " is " +
                                 std::to_string(seg) + ")");
            return m;
        }
        throw NudgeError(joint + " takes " + wordsOf(joint) + "; not '" + word + "'");
    }

    std::vector<std::string> expandJoints(const std::string& joint, const std::string& word)
    {
        std::vector<std::string> fingers;
        std::string              side;
        const std::string        j = lower(joint);
        if (j == "fingers_left" || j == "fingers_right")
        {
            side = j == "fingers_left" ? "Left" : "Right";
            for (const char* f : { "mHandIndex", "mHandMiddle", "mHandRing", "mHandPinky" })
                fingers.push_back(f);
        }
        else
        {
            for (const char* f : { "mHandIndex", "mHandMiddle", "mHandRing", "mHandPinky" })
            {
                for (const char* s : { "Left", "Right" })
                {
                    if (lower(std::string(f) + s) == j)
                    {
                        fingers.push_back(f);
                        side = s;
                    }
                }
            }
        }
        if (fingers.empty())
        {
            findMotion(joint, word); // throws with the words when the joint has none
            return { joint };
        }
        std::vector<std::string> out;
        for (const std::string& f : fingers)
        {
            for (int seg = 1; seg <= 3; ++seg)
            {
                const std::string name = f + std::to_string(seg) + side;
                try
                {
                    findMotion(name, word);
                    out.push_back(name);
                }
                catch (const NudgeError&)
                {
                    // a segment (or a finger) the word is not for
                }
            }
        }
        if (out.empty())
            throw NudgeError(joint + ": no finger segment takes '" + word + "' (" + wordsOf(fingers.front() + "1" + side) + ")");
        return out;
    }

    namespace
    {
        struct Violation
        {
            double                worst = 0.0;
            std::string           motion;
            double                value = 0.0;
            std::optional<double> limit;
        };

        Violation violation(const std::string& name, const Mat3& local, const std::optional<Vec3>& rest, const std::string& flexibility)
        {
            Violation out;
            Vec3      hingeAxis;
            double    natural = 1.0;
            const bool hinge  = solver::hingeOf(name, hingeAxis, natural);
            std::vector<solver::Anatomy> values;
            if (hinge)
                values.push_back(solver::Anatomy{ "flex", solver::hingeAngle(local, hingeAxis) * natural * DEG, std::nullopt, std::nullopt });
            else
                values = solver::anatomicalValues(name, local, rest.value_or(Vec3(0, 0, 1)));
            for (const solver::Anatomy& l : solver::defaultLimits(name, hinge, flexibility))
            {
                for (const solver::Anatomy& v : values)
                {
                    if (v.motion != l.motion)
                        continue;
                    if (l.lo && v.value < *l.lo && *l.lo - v.value > out.worst)
                        out = Violation{ *l.lo - v.value, v.motion, v.value, l.lo };
                    if (l.hi && v.value > *l.hi && v.value - *l.hi > out.worst)
                        out = Violation{ v.value - *l.hi, v.motion, v.value, l.hi };
                }
            }
            return out;
        }
    }

    Turn turnJoint(const solver::Skeleton& sk, const solver::State& st, int j, const Motion& m, double deg, bool limits,
                   const std::string& flexibility)
    {
        if (j < 0 || j >= (int)sk.names.size())
            throw NudgeError("nudge: no such joint in the skeleton");
        if (!(deg > 0.0) || deg > MAX_DEG + 1e-9)
            throw NudgeError("deg must be more than 0 and at most " + std::to_string((int)MAX_DEG));
        const std::string& name  = sk.names[j];
        const bool         right = m.sided && solver::sideOf(name) == "Right";
        const Mat3         L     = st.local[j];
        std::vector<Mat3>  W;
        std::vector<Vec3>  P;
        solver::forward(sk, st, W, P);
        const int  p  = sk.parent[j];
        const Mat3 Wp = p >= 0 ? W[p] : Mat3::identity();

        Turn out;
        out.askedDeg = deg;
        double maxRad = deg * RAD;
        std::function<Mat3(double)> turned;
        if (m.kind == "own")
        {
            const Vec3 axis = right ? mirrorAxis(m.axis) : m.axis;
            turned          = [=](double th) { return solver::rotAxis(m.sign * th, axis) * L; };
        }
        else if (m.kind == "twist")
        {
            const std::optional<Vec3> rest = solver::restBone(sk, j);
            if (!rest)
                throw NudgeError(name + " has no bone to twist about");
            const Vec3   axis = solver::unit(*rest);
            const double s    = m.sign * (right ? -1.0 : 1.0);
            turned            = [=](double th) { return solver::rotAxis(s * th, axis) * L; };
        }
        else if (m.kind == "hinge")
        {
            Vec3   axis;
            double natural = 1.0;
            if (!solver::hingeOf(name, axis, natural))
                throw NudgeError(name + " is not a hinge");
            const double now = solver::hingeAngle(L, axis);
            turned           = [=](double th) { return solver::rotAxis(now + natural * m.sign * th, axis); };
        }
        else if (m.kind == "toward")
        {
            const int ref = m.ref.empty() ? p : sk.find(m.ref);
            const int end = sk.find(solver::boneEnd(name));
            if (ref < 0 || end < 0 || p < 0)
                throw NudgeError(name + ": the skeleton lacks the joints this motion needs");
            const Vec3 D    = solver::unit((right ? mirrorDir(m.toward) : m.toward) * W[ref]);
            const Vec3 A    = (right ? mirrorAxis(m.axis) : m.axis) * W[ref];
            const Vec3 b    = solver::unit(P[end] - P[j]);
            const double ang = solver::angleBetween(b, D);
            Vec3       axisWorld;
            if (ang > OPPOSITE_DEG * RAD)
            {
                const Vec3 a = A - b * solver::dot(A, b);
                if (solver::norm(a) < 1e-6)
                {
                    out.local    = L;
                    out.note     = "the bone points the opposite way and along the turning axis: nothing moved";
                    return out;
                }
                axisWorld = solver::unit(a);
            }
            else
            {
                const Vec3 c = solver::cross(b, D);
                if (solver::norm(c) < 1e-9)
                {
                    out.local   = L;
                    out.reached = true;
                    out.note    = "the bone already points that way";
                    return out;
                }
                axisWorld = solver::unit(c);
                if (maxRad > ang)
                {
                    maxRad      = ang;
                    out.reached = true;
                }
            }
            const Vec3 axisParent = axisWorld * solver::transpose(Wp);
            turned                = [=](double th) { return L * solver::rotAxis(th, axisParent); };
        }
        else
        {
            throw NudgeError("unknown motion kind " + m.kind);
        }

        Mat3 result = turned(maxRad);
        const bool body = std::find(solver::bodyJoints().begin(), solver::bodyJoints().end(), name) != solver::bodyJoints().end();
        if (limits && body)
        {
            const std::optional<Vec3> rest   = solver::restBone(sk, j);
            const Violation           before = violation(name, L, rest, flexibility);
            const Violation           full   = violation(name, result, rest, flexibility);
            const double              allow  = before.worst + 0.01;
            if (full.worst > allow)
            {
                double lo = 0.0, hi = 1.0;
                for (int i = 0; i < 40; ++i)
                {
                    const double mid = 0.5 * (lo + hi);
                    if (violation(name, turned(maxRad * mid), rest, flexibility).worst <= allow)
                        lo = mid;
                    else
                        hi = mid;
                }
                result          = turned(maxRad * lo);
                out.limited     = true;
                out.reached     = false;
                out.limitMotion = full.motion;
                out.limitValue  = full.value;
                out.limit       = full.limit;
            }
        }
        out.local    = result;
        out.movedDeg = solver::rotationAngle(solver::transpose(L) * result) * DEG;
        return out;
    }

    bool directionOf(const std::string& word, const std::string& side, Vec3& out)
    {
        const std::string w = lower(word);
        const double      s = side == "Right" ? -1.0 : 1.0;
        if (w == "forward" || w == "front")
            out = Vec3(1, 0, 0);
        else if (w == "back" || w == "backward")
            out = Vec3(-1, 0, 0);
        else if (w == "up")
            out = Vec3(0, 0, 1);
        else if (w == "down")
            out = Vec3(0, 0, -1);
        else if (w == "left")
            out = Vec3(0, 1, 0);
        else if (w == "right")
            out = Vec3(0, -1, 0);
        else if (w == "out" || w == "outward")
            out = Vec3(0, s, 0);
        else if (w == "in" || w == "inward")
            out = Vec3(0, -s, 0);
        else
            return false;
        return true;
    }

    solver::Spec moveSpec(const solver::PointSpec& at, const Vec3& target, const std::string& root, const std::string& middle,
                          const std::string& flexibility)
    {
        solver::Spec spec;
        spec.start    = "current";
        spec.freeAuto = false;
        spec.free     = { root, middle };
        solver::Goal g;
        g.type      = solver::GoalType::Anchor;
        g.name      = "point at its new place";
        g.at        = at;
        g.position  = target;
        g.tolerance = 0.002;
        spec.goals.push_back(g);
        spec.options.flexibility = flexibility;
        spec.options.starts      = 3;
        spec.options.comfort     = 0.0;
        return spec;
    }
}

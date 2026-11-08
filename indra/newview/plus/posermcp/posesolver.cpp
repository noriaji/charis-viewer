/**
 * @file posesolver.cpp
 * @brief solve_pose: every body joint at once from goals. See posesolver.h.
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

// A line-by-line port of tools/poser-mcp/posesolver.py (same terms in the same
// order, same residuals, Jacobians, LM and augmented Lagrangian), so that the
// cases it dumps are reproduced here (ik/posesolver_test.cpp). Comments name the
// Python function a part comes from.

#include "linden_common.h"

#include "posesolver.h"

#include "aimsolver.h"
#include "contactsolver.h"
#include "posesolvertables.h"
#include "reachsolver.h"

#include "llmath.h"
#include "m3math.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <random>
#include <set>

namespace plus::posermcp::solver
{
    namespace
    {
        constexpr double PI  = 3.14159265358979323846;
        constexpr double DEG = 180.0 / PI;
        constexpr double RAD = PI / 180.0;
        constexpr double CM  = 100.0;

        constexpr double MAX_ROTATION_DEG = 120.0; // the report's warning (the range uses the option)

        bool startsWith(const std::string& s, const char* prefix)
        {
            const size_t n = std::char_traits<char>::length(prefix);
            return s.size() >= n && s.compare(0, n, prefix) == 0;
        }

        bool endsWith(const std::string& s, const char* suffix)
        {
            const size_t n = std::char_traits<char>::length(suffix);
            return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
        }

        bool iequals(const std::string& a, const std::string& b)
        {
            if (a.size() != b.size())
                return false;
            for (size_t i = 0; i < a.size(); ++i)
            {
                if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                    return false;
            }
            return true;
        }

        double clamp1(double v) { return v < -1.0 ? -1.0 : (v > 1.0 ? 1.0 : v); }

        double roundTo(double v, int decimals)
        {
            const double f = std::pow(10.0, decimals);
            const double r = std::round(v * f) / f;
            return r == 0.0 ? 0.0 : r;
        }

        // Python's "%s" of a float that was rounded (repr: shortest form, ".0" for whole numbers).
        std::string pyNum(double v)
        {
            char buf[64];
            for (int p = 1; p <= 17; ++p)
            {
                std::snprintf(buf, sizeof(buf), "%.*g", p, v);
                if (std::strtod(buf, nullptr) == v)
                    break;
            }
            std::string s(buf);
            if (s.find_first_of(".eEn") == std::string::npos)
                s += ".0";
            return s;
        }

        std::string fmt(const char* format, double v)
        {
            char buf[64];
            std::snprintf(buf, sizeof(buf), format, v);
            return buf;
        }
    }

    // ------------------------------------------------------------------ vectors and rotations

    double norm(const Vec3& a) { return std::sqrt(dot(a, a)); }

    Vec3 unit(const Vec3& a)
    {
        const double n = norm(a);
        return n > 1e-12 ? a / n : a;
    }

    Vec3 operator*(const Vec3& v, const Mat3& r)
    {
        return Vec3(v.x * r.m[0][0] + v.y * r.m[1][0] + v.z * r.m[2][0], v.x * r.m[0][1] + v.y * r.m[1][1] + v.z * r.m[2][1],
                    v.x * r.m[0][2] + v.y * r.m[1][2] + v.z * r.m[2][2]);
    }

    Mat3 operator*(const Mat3& a, const Mat3& b)
    {
        Mat3 c;
        for (int i = 0; i < 3; ++i)
        {
            for (int j = 0; j < 3; ++j)
                c.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
        }
        return c;
    }

    Mat3 transpose(const Mat3& r)
    {
        Mat3 t;
        for (int i = 0; i < 3; ++i)
        {
            for (int j = 0; j < 3; ++j)
                t.m[i][j] = r.m[j][i];
        }
        return t;
    }

    // ik_check.rot_axis: the right-handed column form, transposed for row vectors.
    Mat3 rotAxis(double angle, const Vec3& axis)
    {
        const double n = norm(axis);
        if (!(n > 0.0))
            return Mat3::identity();
        const Vec3   a = axis / n;
        const double c = std::cos(angle), s = std::sin(angle), t = 1.0 - c;
        const double x = a.x, y = a.y, z = a.z;
        const double col[3][3] = { { t * x * x + c, t * x * y - s * z, t * x * z + s * y },
                                   { t * x * y + s * z, t * y * y + c, t * y * z - s * x },
                                   { t * x * z - s * y, t * y * z + s * x, t * z * z + c } };
        Mat3 r;
        for (int i = 0; i < 3; ++i)
        {
            for (int j = 0; j < 3; ++j)
                r.m[i][j] = col[j][i];
        }
        return r;
    }

    Mat3 expRot(const Vec3& w)
    {
        const double a = norm(w);
        if (a < 1e-15)
            return Mat3::identity();
        return rotAxis(a, w / a);
    }

    // posemodel.log_rot
    Vec3 logRot(const Mat3& r)
    {
        const double cosine = clamp1((r.m[0][0] + r.m[1][1] + r.m[2][2] - 1.0) / 2.0);
        // the column form c = r^T: v = (c21 - c12, c02 - c20, c10 - c01)
        const Vec3   v(r.m[1][2] - r.m[2][1], r.m[2][0] - r.m[0][2], r.m[0][1] - r.m[1][0]);
        const double s = norm(v) / 2.0;
        const double a = std::atan2(s, cosine);
        if (cosine > 0.0)
            return s < 1e-15 ? v / 2.0 : v / (2.0 * s) * a;
        if (s > 1e-6)
            return v / (2.0 * s) * a;
        // near pi
        double d[3];
        for (int i = 0; i < 3; ++i)
            d[i] = std::sqrt(std::max(0.0, (r.m[i][i] + 1.0) / 2.0));
        int k = 0;
        for (int i = 1; i < 3; ++i)
        {
            if (d[i] > d[k])
                k = i;
        }
        Vec3 axis(r.m[k][0], r.m[k][1], r.m[k][2]); // column k of c = row k of r
        axis[k] += 1.0;
        return solver::unit(axis) * a;
    }

    // ik_check.rotation_angle
    double rotationAngle(const Mat3& r)
    {
        const Vec3 v(r.m[1][2] - r.m[2][1], r.m[2][0] - r.m[0][2], r.m[0][1] - r.m[1][0]);
        return std::atan2(norm(v) / 2.0, (r.m[0][0] + r.m[1][1] + r.m[2][2] - 1.0) / 2.0);
    }

    double angleBetween(const Vec3& a, const Vec3& b) { return std::atan2(norm(cross(a, b)), dot(a, b)); }

    // ik_check.swing_rotation
    Mat3 swingRotation(const Vec3& a, const Vec3& b)
    {
        const Vec3   c      = cross(a, b);
        const double s      = norm(c);
        const double cosine = dot(a, b);
        if (s > 1e-6)
            return rotAxis(std::atan2(s, cosine), c);
        if (cosine > 0.0)
            return Mat3::identity();
        int k = 0;
        for (int i = 1; i < 3; ++i)
        {
            if (std::fabs(a[i]) < std::fabs(a[k]))
                k = i;
        }
        Vec3 e;
        e[k] = 1.0;
        return rotAxis(PI, solver::unit(e - a * dot(e, a)));
    }

    // posemodel.quat_of
    Quat quatOf(const Mat3& r)
    {
        // column form c[i][j] = r[j][i]
        auto   c = [&](int i, int j) { return r.m[j][i]; };
        double q[4] = { 1.0, 0.0, 0.0, 0.0 };
        const double t = c(0, 0) + c(1, 1) + c(2, 2);
        if (t > 0.0)
        {
            const double s = std::sqrt(t + 1.0) * 2.0;
            q[0] = 0.25 * s;
            q[1] = (c(2, 1) - c(1, 2)) / s;
            q[2] = (c(0, 2) - c(2, 0)) / s;
            q[3] = (c(1, 0) - c(0, 1)) / s;
        }
        else
        {
            int i = 0;
            for (int k = 1; k < 3; ++k)
            {
                if (c(k, k) > c(i, i))
                    i = k;
            }
            const int    j = (i + 1) % 3, k = (i + 2) % 3;
            const double s = std::sqrt(std::max(1e-30, 1.0 + c(i, i) - c(j, j) - c(k, k))) * 2.0;
            q[0]           = (c(k, j) - c(j, k)) / s;
            q[1 + i]       = 0.25 * s;
            q[1 + j]       = (c(j, i) + c(i, j)) / s;
            q[1 + k]       = (c(k, i) + c(i, k)) / s;
        }
        const double n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        double       f = 1.0 / n;
        if (q[0] < 0.0)
            f = -f;
        return Quat{ q[0] * f, q[1] * f, q[2] * f, q[3] * f };
    }

    double hingeAngle(const Mat3& r, const Vec3& axis)
    {
        const Quat q = quatOf(r);
        return 2.0 * std::atan2(q.x * axis.x + q.y * axis.y + q.z * axis.z, q.w);
    }

    // ik_check.rot_to_euler
    Vec3 eulerDegrees(const Mat3& r)
    {
        auto         c  = [&](int i, int j) { return r.m[j][i]; };
        const double sy = c(0, 2);
        const double y  = std::asin(clamp1(sy));
        double       x = 0.0, z = 0.0;
        if (std::fabs(sy) < 1.0 - 1e-12)
        {
            x = std::atan2(-c(1, 2), c(2, 2));
            z = std::atan2(-c(0, 1), c(0, 0));
        }
        else
        {
            x = std::atan2(c(2, 1), c(1, 1));
        }
        return Vec3(x * DEG, y * DEG, z * DEG);
    }

    // ik_check.euler_to_rot: v' = v Rz Ry Rx
    Mat3 eulerToRot(const Vec3& degrees)
    {
        return rotAxis(degrees.z * RAD, Vec3(0, 0, 1)) * rotAxis(degrees.y * RAD, Vec3(0, 1, 0)) *
               rotAxis(degrees.x * RAD, Vec3(1, 0, 0));
    }

    Mat3 fromLL(const LLQuaternion& q)
    {
        LLQuaternion n = q;
        n.normalize();
        const LLMatrix3 m = n.getMatrix3();
        Mat3            r;
        for (int i = 0; i < 3; ++i)
        {
            for (int j = 0; j < 3; ++j)
                r.m[i][j] = m.mMatrix[i][j];
        }
        // re-orthonormalise in double (F32 rounding)
        const Vec3 x = solver::unit(r.row(0));
        const Vec3 z = solver::unit(cross(x, r.row(1)));
        const Vec3 y = cross(z, x);
        for (int j = 0; j < 3; ++j)
        {
            r.m[0][j] = x[j];
            r.m[1][j] = y[j];
            r.m[2][j] = z[j];
        }
        return r;
    }

    Vec3 fromLL(const LLVector3& v) { return Vec3(v.mV[VX], v.mV[VY], v.mV[VZ]); }

    LLQuaternion toLLQuat(const Mat3& r)
    {
        const Quat   q = quatOf(r);
        LLQuaternion out((F32)q.x, (F32)q.y, (F32)q.z, (F32)q.w);
        out.normalize();
        return out;
    }

    LLVector3 toLL(const Vec3& v) { return LLVector3((F32)v.x, (F32)v.y, (F32)v.z); }

    // ------------------------------------------------------------------ the skeleton

    int Skeleton::find(const std::string& name) const
    {
        for (size_t i = 0; i < names.size(); ++i)
        {
            if (names[i] == name)
                return (int)i;
        }
        for (size_t i = 0; i < names.size(); ++i)
        {
            if (iequals(names[i], name))
                return (int)i;
        }
        return -1;
    }

    bool Skeleton::valid(std::string& why) const
    {
        const size_t n = names.size();
        if (n == 0)
        {
            why = "the skeleton is empty";
            return false;
        }
        if (parent.size() != n || offset.size() != n || local.size() != n)
        {
            why = "the skeleton's lists differ in length";
            return false;
        }
        if (parent[0] != -1)
        {
            why = "the first joint must be the root";
            return false;
        }
        for (size_t i = 1; i < n; ++i)
        {
            if (parent[i] < 0 || parent[i] >= (int)i)
            {
                why = "joint " + names[i] + " comes before its parent";
                return false;
            }
        }
        return true;
    }

    namespace
    {
        const char* const SIDES[2] = { "Left", "Right" };

        const std::vector<std::string>& bodyList()
        {
            static const std::vector<std::string> list = []
            {
                std::vector<std::string> v{ "mPelvis", "mTorso", "mChest", "mNeck", "mHead" };
                for (const char* s : SIDES)
                {
                    for (const char* j : { "mCollar", "mShoulder", "mElbow", "mWrist", "mHip", "mKnee", "mAnkle" })
                        v.push_back(std::string(j) + s);
                }
                return v;
            }();
            return list;
        }

        const std::map<std::string, std::string>& boneEnds()
        {
            static const std::map<std::string, std::string> ends = []
            {
                std::map<std::string, std::string> m{
                    { "mPelvis", "mTorso" }, { "mTorso", "mChest" }, { "mChest", "mNeck" }, { "mNeck", "mHead" }, { "mHead", "mSkull" }
                };
                for (const char* s : SIDES)
                {
                    const std::string S(s);
                    m["mCollar" + S]   = "mShoulder" + S;
                    m["mShoulder" + S] = "mElbow" + S;
                    m["mElbow" + S]    = "mWrist" + S;
                    m["mWrist" + S]    = "mHandMiddle1" + S;
                    m["mHip" + S]      = "mKnee" + S;
                    m["mKnee" + S]     = "mAnkle" + S;
                    m["mAnkle" + S]    = "mFoot" + S;
                    m["mFoot" + S]     = "mToe" + S;
                }
                return m;
            }();
            return ends;
        }

        // posesolver.RANGES (degrees, flexibility normal), in the Python dict's order.
        struct RangeRow
        {
            const char* motion;
            double      lo, hi;
        };
        struct RangeFamily
        {
            const char*           family;
            std::vector<RangeRow> rows;
        };
        const std::vector<RangeFamily>& rangeTable()
        {
            static const std::vector<RangeFamily> table{
                { "mShoulder", { { "back", -50, 90 }, { "across", -60, 90 }, { "twist", -80, 80 } } },
                { "mElbow", { { "flex", -5, 150 } } },
                { "mWrist", { { "flex", -70, 80 }, { "deviate", -30, 20 }, { "twist", -10, 10 } } },
                { "mHip", { { "flex", -20, 125 }, { "abduct", -30, 60 }, { "twist", -45, 45 } } },
                { "mKnee", { { "flex", -5, 150 } } },
                { "mAnkle", { { "flex", -20, 50 }, { "invert", -20, 30 }, { "twist", -10, 10 } } },
                { "mFoot", { { "flex", -60, 30 } } },
                { "mTorso", { { "forward", -15, 40 }, { "side", -20, 20 }, { "twist", -25, 25 } } },
                { "mChest", { { "forward", -15, 40 }, { "side", -20, 20 }, { "twist", -25, 25 } } },
                { "mNeck", { { "forward", -30, 40 }, { "side", -25, 25 }, { "twist", -40, 40 } } },
                { "mHead", { { "forward", -30, 40 }, { "side", -25, 25 }, { "twist", -40, 40 } } },
                { "mCollar", { { "up", -10, 25 }, { "forward", -15, 15 }, { "twist", -10, 10 } } },
            };
            return table;
        }

        const RangeFamily* jointFamily(const std::string& name)
        {
            for (const RangeFamily& f : rangeTable())
            {
                if (startsWith(name, f.family))
                    return &f;
            }
            return nullptr;
        }

        double flexScale(const std::string& flexibility, const std::string& family)
        {
            if (flexibility == "athletic")
                return 1.15;
            if (flexibility == "dancer")
                return (family == "mHip" || family == "mTorso" || family == "mChest" || family == "mShoulder") ? 1.3 : 1.0;
            return 1.0;
        }

        std::string bodyPart(const std::string& name)
        {
            if (name == "mPelvis")
                return "pelvis";
            if (name == "mTorso" || name == "mChest")
                return "trunk";
            if (name == "mNeck" || name == "mHead")
                return "head";
            if (startsWith(name, "mHip") || startsWith(name, "mKnee") || startsWith(name, "mAnkle") || startsWith(name, "mFoot"))
                return "leg";
            return "arm";
        }

        double stayDefault(const std::string& part)
        {
            if (part == "arm")
                return 0.1;
            if (part == "leg")
                return 0.2;
            if (part == "head")
                return 0.15;
            return 0.3; // trunk, pelvis
        }

        double sideSign(const std::string& name)
        {
            const std::string s = sideOf(name);
            return s == "Left" ? 1.0 : (s == "Right" ? -1.0 : 0.0);
        }

        struct Segment
        {
            std::string a, b;
            double      t, mass;
        };
        const std::vector<Segment>& segments()
        {
            static const std::vector<Segment> list = []
            {
                std::vector<Segment> v{ { "mPelvis", "mTorso", 0.5, 0.142 }, { "mTorso", "mChest", 0.5, 0.139 },
                                        { "mChest", "mNeck", 0.5, 0.216 }, { "mHead", "mSkull", 1.0, 0.081 } };
                for (const char* s : SIDES)
                {
                    const std::string S(s);
                    v.push_back({ "mShoulder" + S, "mElbow" + S, 0.44, 0.028 });
                    v.push_back({ "mElbow" + S, "mWrist" + S, 0.43, 0.016 });
                    v.push_back({ "mWrist" + S, "mHandMiddle1" + S, 0.5, 0.006 });
                    v.push_back({ "mHip" + S, "mKnee" + S, 0.43, 0.100 });
                    v.push_back({ "mKnee" + S, "mAnkle" + S, 0.43, 0.0465 });
                    v.push_back({ "mAnkle" + S, "mFoot" + S, 0.5, 0.0145 });
                }
                return v;
            }();
            return list;
        }

        struct LimbDef
        {
            const char* name;
            const char* root;
            const char* middle;
            const char* end;
            Vec3        hinge;
            double      natural;
            Vec3        pole;
            bool        arm;
        };
        const std::vector<LimbDef>& limbDefs()
        {
            static const std::vector<LimbDef> list{
                { "left_arm", "mShoulderLeft", "mElbowLeft", "mWristLeft", Vec3(0, 0, 1), -1.0, Vec3(-0.3, 0.4, -1.0), true },
                { "right_arm", "mShoulderRight", "mElbowRight", "mWristRight", Vec3(0, 0, 1), 1.0, Vec3(-0.3, -0.4, -1.0), true },
                { "left_leg", "mHipLeft", "mKneeLeft", "mAnkleLeft", Vec3(0, 1, 0), 1.0, Vec3(1.0, 0.15, 0.0), false },
                { "right_leg", "mHipRight", "mKneeRight", "mAnkleRight", Vec3(0, 1, 0), 1.0, Vec3(1.0, -0.15, 0.0), false },
            };
            return list;
        }
    }

    const std::vector<std::string>& bodyJoints() { return bodyList(); }

    std::string boneEnd(const std::string& joint)
    {
        auto it = boneEnds().find(joint);
        return it != boneEnds().end() ? it->second : std::string();
    }

    Vec3 sideAxis(const std::string& name)
    {
        if (name == "mPelvis" || name == "mTorso" || name == "mChest" || name == "mNeck" || name == "mHead" || startsWith(name, "mSpine"))
            return Vec3(1, 0, 0);
        if (startsWith(name, "mCollar"))
            return Vec3(0, 0, 1);
        if (startsWith(name, "mShoulder"))
            return Vec3(1, 0, 0);
        if (startsWith(name, "mElbow") || startsWith(name, "mWrist") || startsWith(name, "mHand"))
            return Vec3(0, 0, -1);
        if (startsWith(name, "mHip"))
            return Vec3(-1, 0, 0);
        if (startsWith(name, "mKnee"))
            return Vec3(1, 0, 0);
        return Vec3(0, 0, 1);
    }

    std::string sideOf(const std::string& name)
    {
        if (endsWith(name, "Left") || startsWith(name, "L_"))
            return "Left";
        if (endsWith(name, "Right") || startsWith(name, "R_"))
            return "Right";
        return "";
    }

    bool hingeOf(const std::string& joint, Vec3& axis, double& natural)
    {
        if (joint == "mElbowLeft" || joint == "mElbowRight")
        {
            axis    = Vec3(0, 0, 1);
            natural = joint == "mElbowLeft" ? -1.0 : 1.0;
            return true;
        }
        if (joint == "mKneeLeft" || joint == "mKneeRight" || joint == "mFootLeft" || joint == "mFootRight")
        {
            axis    = Vec3(0, 1, 0);
            natural = 1.0;
            return true;
        }
        return false;
    }

    void forward(const Skeleton& sk, const State& st, std::vector<Mat3>& W, std::vector<Vec3>& P)
    {
        const size_t n = sk.names.size();
        W.resize(n);
        P.resize(n);
        for (size_t i = 0; i < n; ++i)
        {
            const int p = sk.parent[i];
            if (p < 0)
            {
                W[i] = st.local[i];
                P[i] = st.root;
            }
            else
            {
                W[i] = st.local[i] * W[p];
                P[i] = P[p] + sk.offset[i] * W[p];
            }
        }
    }

    // posemodel.Avatar.rest_bone
    std::optional<Vec3> restBone(const Skeleton& sk, int i)
    {
        const std::string end = boneEnd(sk.names[i]);
        const int         e   = end.empty() ? -1 : sk.find(end);
        if (e >= 0 && sk.parent[e] == i)
            return sk.offset[e];
        if (e >= 0)
        {
            State rest;
            rest.local.assign(sk.names.size(), Mat3::identity());
            rest.root = sk.rootPos;
            std::vector<Mat3> W;
            std::vector<Vec3> P;
            forward(sk, rest, W, P);
            return P[e] - P[i];
        }
        int    best   = -1;
        double length = 0.001;
        for (size_t c = 0; c < sk.names.size(); ++c)
        {
            if (sk.parent[c] != i || sk.names[c].empty() || sk.names[c][0] != 'm')
                continue;
            const double l = norm(sk.offset[c]);
            if (l > length)
            {
                best   = (int)c;
                length = l;
            }
        }
        if (best < 0)
            return std::nullopt;
        return sk.offset[best];
    }

    // ------------------------------------------------------------------ sites

    const SiteTable& defaultSites()
    {
        static const SiteTable table = []
        {
            SiteTable t;
            for (const tables::SiteRow& row : tables::SITES)
            {
                SiteEntry e;
                e.anchor = row.anchor;
                e.frame  = row.frame;
                e.offset = Vec3(row.offset[0], row.offset[1], row.offset[2]);
                if (row.hasNormal)
                    e.normal = Vec3(row.normal[0], row.normal[1], row.normal[2]);
                if (row.hasAlong)
                    e.along = Vec3(row.along[0], row.along[1], row.along[2]);
                t[row.site][row.side] = e;
            }
            return t;
        }();
        return table;
    }

    namespace
    {
        SiteTable mergeSites(const SiteTable& base, const SiteOverrides& over)
        {
            SiteTable out = base;
            for (const auto& [name, sides] : over)
            {
                if (name.empty() || name[0] == '_')
                    continue;
                for (const auto& [side, f] : sides)
                {
                    SiteEntry& e = out[name][side];
                    if (f.anchor)
                        e.anchor = *f.anchor;
                    if (f.frame)
                        e.frame = *f.frame;
                    if (f.offset)
                        e.offset = *f.offset;
                    if (f.normal)
                        e.normal = *f.normal;
                    if (f.along)
                        e.along = *f.along;
                }
            }
            return out;
        }

        // posesolver.site_entry
        const SiteEntry& siteEntry(const SiteTable& sites, std::string name, const std::string& joint)
        {
            std::string side = sideOf(joint);
            if (side.empty())
                side = "*";
            for (const auto& [suffix, sd] : { std::pair<const char*, const char*>{ "_left", "Left" }, { "_right", "Right" } })
            {
                if (endsWith(name, suffix))
                {
                    const std::string stem = name.substr(0, name.size() - std::char_traits<char>::length(suffix));
                    if (sites.count(stem))
                    {
                        name = stem;
                        side = sd;
                    }
                }
            }
            auto it = sites.find(name);
            if (it == sites.end())
            {
                std::string known;
                for (const auto& kv : sites)
                    known += (known.empty() ? "" : ", ") + kv.first;
                throw SolveError("unknown site " + name + " (sites: " + known + ")");
            }
            auto s = it->second.find(side);
            if (s == it->second.end())
                s = it->second.find("*");
            if (s == it->second.end())
                throw SolveError("site " + name + " has no " + side + " side");
            return s->second;
        }
    }

    // ------------------------------------------------------------------ the problem

    namespace
    {
        struct Jac
        {
            int                 rows = 0, cols = 0;
            std::vector<double> a;

            void reset(int r, int c)
            {
                rows = r;
                cols = c;
                a.assign((size_t)r * (size_t)c, 0.0);
            }
            double*       row(int r) { return a.data() + (size_t)r * (size_t)cols; }
            const double* row(int r) const { return a.data() + (size_t)r * (size_t)cols; }
        };

        struct Lever
        {
            int  var = -1;
            bool inA = false, inF = false;
        };

        struct Part
        {
            double             w = 1.0;
            int                anchor = -1, frame = -1;
            Vec3               offset;
            std::vector<Lever> levers;
        };
        using Point = std::vector<Part>;

        enum class Kind
        {
            Soft,
            Eq,
            Ineq
        };

        const char* kindName(Kind k) { return k == Kind::Soft ? "soft" : (k == Kind::Eq ? "eq" : "ineq"); }

        // A camera (posesolver.Camera)
        struct Camera
        {
            CameraSpec spec;
            Vec3       forward, right, up, down;

            explicit Camera(const CameraSpec& s) : spec(s)
            {
                const double a = s.azimuth * RAD, e = s.elevation * RAD;
                const Vec3   toward(std::cos(e) * std::cos(a), std::cos(e) * std::sin(a), std::sin(e));
                forward = -toward;
                Vec3 r  = cross(forward, Vec3(0, 0, 1));
                if (norm(r) < 1e-6)
                    r = Vec3(-std::sin(a), std::cos(a), 0.0);
                r            = solver::unit(r);
                const Vec3 u = cross(r, forward);
                const double ro = s.roll * RAD;
                right = std::cos(ro) * r + std::sin(ro) * u;
                up    = -std::sin(ro) * r + std::cos(ro) * u;
                down  = -up;
            }

            bool   perspective() const { return spec.fov.has_value() && spec.distance.has_value(); }
            Vec3   position() const { return spec.focus - forward * spec.distance.value_or(0.0); }
            double focalPx() const { return (spec.height / 2.0) / std::tan(spec.fov.value_or(60.0) * RAD / 2.0); }
            double centerX() const { return spec.center ? spec.center->x : spec.width / 2.0; }
            double centerY() const { return spec.center ? spec.center->y : spec.height / 2.0; }
        };

        struct Ctx;

        struct Term
        {
            std::string name, unit, type;
            Kind        kind  = Kind::Soft;
            double      tol   = 1.0;
            bool        group = false;

            virtual ~Term() = default;
            // vals and, with jac, J (vals.size() x n)
            virtual void eval(const Ctx& ctx, bool jac, std::vector<double>& vals, Jac& J) const = 0;
            virtual void describe(const Ctx& ctx, GoalReport& out) const;
        };
    }

    struct Problem::Impl
    {
        Skeleton                       sk;
        Spec                           spec;
        SiteTable                      sites;
        CameraSpec                     camera;
        std::vector<std::vector<char>> inChain; // [i][j]: j is i or one of its ancestors
        State                          measured, start, ref;
        std::map<int, Vec3>            restDir;
        std::vector<Variable>          vars;
        std::vector<int>               varOf; // joint -> index into vars (rotations / hinges only), -1
        int                            trans = -1;
        int                            n     = 0;
        std::optional<double>          floor;
        std::vector<std::unique_ptr<Term>> terms;

        // build
        int   joint(const std::string& name) const;
        Point resolve(const PointSpec& spec) const;
        bool  siteNormal(const PointSpec& spec, int& frame, Vec3& normal) const;
        void  levers(Part& part) const;
        double floorZ() const;
        void  buildTerms();

        // evaluate
        State step(const State& st, const double* dx) const;
        struct AL
        {
            double                                          mu = 1.0;
            std::vector<std::optional<std::vector<double>>> lam;
        };
        int    evaluate(const State& st, const AL& al, bool jac, std::vector<double>& r, Jac& J) const;
        double hardStatus(const State& st, std::vector<std::pair<int, std::vector<double>>>* cs) const;
        double softCost(const State& st) const;
        std::vector<Anatomy> rangeValues(int j, const Mat3& L, const Variable& v, std::vector<Vec3>* grads) const;
    };

    namespace
    {
        // posesolver.Ctx
        struct Ctx
        {
            const Problem::Impl& pb;
            const State&         st;
            std::vector<Mat3>    W;
            std::vector<Vec3>    P;

            Ctx(const Problem::Impl& p, const State& s) : pb(p), st(s) { forward(pb.sk, st, W, P); }

            int worldAxes(const Variable& v, Vec3 axes[3]) const
            {
                if (v.kind == Variable::Kind::Trans)
                {
                    axes[0] = Vec3(1, 0, 0);
                    axes[1] = Vec3(0, 1, 0);
                    axes[2] = Vec3(0, 0, 1);
                    return 3;
                }
                const int  p  = pb.sk.parent[v.joint];
                const Mat3 Wp = p >= 0 ? W[p] : Mat3::identity();
                if (v.kind == Variable::Kind::Rot3)
                {
                    for (int k = 0; k < 3; ++k)
                        axes[k] = Wp.row(k);
                    return 3;
                }
                axes[0] = v.axis * Wp;
                return 1;
            }

            // p and, with J (3 x n), its Jacobian
            Vec3 point(const Point& parts, Jac* J) const
            {
                Vec3 p;
                if (J)
                    J->reset(3, pb.n);
                for (const Part& part : parts)
                {
                    const Vec3 ow = part.offset * W[part.frame];
                    const Vec3 q  = P[part.anchor] + ow;
                    p += part.w * q;
                    if (!J)
                        continue;
                    for (const Lever& l : part.levers)
                    {
                        const Variable& v = pb.vars[l.var];
                        const int       j = v.joint;
                        Vec3            lever;
                        if (l.inA && l.inF)
                            lever = P[part.anchor] - P[j] + ow;
                        else if (l.inA)
                            lever = P[part.anchor] - P[j];
                        else
                            lever = ow;
                        Vec3      axes[3];
                        const int k = worldAxes(v, axes);
                        for (int c = 0; c < k; ++c)
                        {
                            const Vec3 d = cross(axes[c], lever) * part.w;
                            for (int r = 0; r < 3; ++r)
                                J->row(r)[v.col + c] += d[r];
                        }
                    }
                    if (pb.trans >= 0)
                    {
                        const int col = pb.vars[pb.trans].col;
                        for (int r = 0; r < 3; ++r)
                            J->row(r)[col + r] += part.w;
                    }
                }
                return p;
            }

            Vec3 vector(int frame, const Vec3& local, Jac* J) const
            {
                const Vec3 u = local * W[frame];
                if (!J)
                    return u;
                J->reset(3, pb.n);
                for (int j = frame; j >= 0; j = pb.sk.parent[j])
                {
                    const int vi = pb.varOf[j];
                    if (vi < 0)
                        continue;
                    const Variable& v = pb.vars[vi];
                    Vec3            axes[3];
                    const int       k = worldAxes(v, axes);
                    for (int c = 0; c < k; ++c)
                    {
                        const Vec3 d = cross(axes[c], u);
                        for (int r = 0; r < 3; ++r)
                            J->row(r)[v.col + c] += d[r];
                    }
                }
                return u;
            }

            void orientJac(int frame, Jac& J) const
            {
                J.reset(3, pb.n);
                for (int j = frame; j >= 0; j = pb.sk.parent[j])
                {
                    const int vi = pb.varOf[j];
                    if (vi < 0)
                        continue;
                    const Variable& v = pb.vars[vi];
                    Vec3            axes[3];
                    const int       k = worldAxes(v, axes);
                    for (int c = 0; c < k; ++c)
                    {
                        for (int r = 0; r < 3; ++r)
                            J.row(r)[v.col + c] += axes[c][r];
                    }
                }
            }
        };

        // ---- helpers for the terms

        // (I - u u^T) Jv / |v| for the rows of a 3-row Jacobian
        Vec3 normJac(const Vec3& v, const Jac& Jv, Jac& Ju)
        {
            const double n = norm(v);
            const Vec3   u = v / n;
            Ju.reset(3, Jv.cols);
            for (int c = 0; c < Jv.cols; ++c)
            {
                const Vec3   col(Jv.row(0)[c], Jv.row(1)[c], Jv.row(2)[c]);
                const Vec3   d = (col - u * dot(u, col)) / n;
                for (int r = 0; r < 3; ++r)
                    Ju.row(r)[c] = d[r];
            }
            return u;
        }

        Vec3 jacCol(const Jac& J, int c) { return Vec3(J.row(0)[c], J.row(1)[c], J.row(2)[c]); }

        // row = sum_k w_k * J.row(k)
        void combine(const Jac& J, const Vec3& w, double* out, double scale)
        {
            for (int c = 0; c < J.cols; ++c)
                out[c] = (w.x * J.row(0)[c] + w.y * J.row(1)[c] + w.z * J.row(2)[c]) * scale;
        }

        void subtract(Jac& a, const Jac& b)
        {
            for (size_t i = 0; i < a.a.size(); ++i)
                a.a[i] -= b.a[i];
        }

        void addExtra(GoalReport& out, const char* key, double v)
        {
            out.extras.push_back(GoalReport::Extra{ key, { v }, false });
        }

        void addList(GoalReport& out, const char* key, std::vector<double> v)
        {
            out.extras.push_back(GoalReport::Extra{ key, std::move(v), true });
        }

        const double NaN = std::numeric_limits<double>::quiet_NaN();

        // posesolver.Term.describe: the norm of the residuals
        void Term::describe(const Ctx& ctx, GoalReport& out) const
        {
            std::vector<double> v;
            Jac                 J;
            eval(ctx, false, v, J);
            double s = 0.0;
            for (double x : v)
                s += x * x;
            out.value = std::sqrt(s);
        }

        // ---------------------------------------------------------------- terms (posesolver.py classes)

        struct BoneDirTerm : Term
        {
            int                 j = -1, end = -1;
            Point               a, b;
            Vec3                d;
            double              w = 1.0;
            std::optional<Vec3> roll;
            Vec3                side;
            double              rw = 0.5;

            Vec3 u(const Ctx& ctx, Jac* Ju) const
            {
                Jac        Ja, Jb;
                const Vec3 pa = ctx.point(a, Ju ? &Ja : nullptr);
                const Vec3 pbv = ctx.point(b, Ju ? &Jb : nullptr);
                const Vec3 v   = pbv - pa;
                if (!Ju)
                    return v / norm(v);
                subtract(Jb, Ja);
                return normJac(v, Jb, *Ju);
            }

            void eval(const Ctx& ctx, bool jac, std::vector<double>& vals, Jac& J) const override
            {
                Jac        Ju;
                const Vec3 uu = u(ctx, jac ? &Ju : nullptr);
                const int  n  = ctx.pb.n;
                int        rows = 3;
                // roll part
                bool       withRoll = false;
                Vec3       e, t, s;
                double     len = 0.0;
                Jac        Js;
                if (roll)
                {
                    s = ctx.vector(j, side, jac ? &Js : nullptr);
                    t = *roll - d * dot(*roll, d);
                    if (norm(t) > 1e-3)
                    {
                        t             = solver::unit(t);
                        const Vec3 sp = s - uu * dot(s, uu);
                        len           = norm(sp);
                        if (len > 1e-6)
                        {
                            e        = sp / len;
                            withRoll = true;
                            rows     = 6;
                        }
                    }
                }
                vals.assign((size_t)rows, 0.0);
                if (jac)
                    J.reset(rows, n);
                for (int r = 0; r < 3; ++r)
                    vals[r] = (uu[r] - d[r]) * DEG * w;
                if (jac)
                {
                    for (int r = 0; r < 3; ++r)
                    {
                        for (int c = 0; c < n; ++c)
                            J.row(r)[c] = Ju.row(r)[c] * DEG * w;
                    }
                }
                if (!withRoll)
                    return;
                for (int r = 0; r < 3; ++r)
                    vals[3 + r] = (e[r] - t[r]) * DEG * rw;
                if (!jac)
                    return;
                // dsp = Js - u (u . Js) - u (s . Ju) - (s . u) Ju
                const double su = dot(s, uu);
                for (int c = 0; c < n; ++c)
                {
                    const Vec3 js = jacCol(Js, c), ju = jacCol(Ju, c);
                    const Vec3 dsp = js - uu * dot(uu, js) - uu * dot(s, ju) - ju * su;
                    const Vec3 de  = (dsp - e * dot(e, dsp)) / len;
                    for (int r = 0; r < 3; ++r)
                        J.row(3 + r)[c] = de[r] * DEG * rw;
                }
            }

            void describe(const Ctx& ctx, GoalReport& out) const override
            {
                const Vec3 uu = u(ctx, nullptr);
                out.value     = roundTo(angleBetween(uu, d) * DEG, 2);
                addList(out, "achieved", { roundTo(uu.x, 4), roundTo(uu.y, 4), roundTo(uu.z, 4) });
                if (roll)
                {
                    const Vec3 s  = ctx.vector(j, side, nullptr);
                    const Vec3 sp = s - uu * dot(s, uu);
                    const Vec3 t  = *roll - d * dot(*roll, d);
                    if (norm(sp) > 1e-6 && norm(t) > 1e-6)
                        addExtra(out, "roll_deg", roundTo(angleBetween(solver::unit(sp), solver::unit(t)) * DEG, 2));
                }
            }
        };

        struct BoneConeTerm : Term
        {
            BoneDirTerm bd;
            double      within = 0.0;

            void eval(const Ctx& ctx, bool jac, std::vector<double>& vals, Jac& J) const override
            {
                Jac          Ju;
                const Vec3   uu  = bd.u(ctx, jac ? &Ju : nullptr);
                const double c   = clamp1(dot(uu, bd.d));
                const double ang = std::acos(c);
                vals.assign(1, ang * DEG - within);
                if (!jac)
                    return;
                J.reset(1, ctx.pb.n);
                const double s = std::max(std::sin(ang), 1e-6);
                combine(Ju, bd.d, J.row(0), -DEG / s);
            }

            void describe(const Ctx& ctx, GoalReport& out) const override
            {
                const Vec3 uu = bd.u(ctx, nullptr);
                out.value     = roundTo(angleBetween(uu, bd.d) * DEG, 2);
                addExtra(out, "within_deg", within);
            }
        };

        struct BoneElevationTerm : Term
        {
            BoneDirTerm           bd;
            std::optional<double> lo, hi;

            void eval(const Ctx& ctx, bool jac, std::vector<double>& vals, Jac& J) const override
            {
                Jac          Ju;
                const Vec3   uu = bd.u(ctx, jac ? &Ju : nullptr);
                const double z  = clamp1(uu.z);
                const double e  = std::asin(z) * DEG;
                const double k  = DEG / std::max(std::sqrt(1.0 - z * z), 1e-6);
                vals.clear();
                const int rows = (lo ? 1 : 0) + (hi ? 1 : 0);
                if (jac)
                    J.reset(rows, ctx.pb.n);
                int r = 0;
                if (lo)
                {
                    vals.push_back(*lo - e);
                    if (jac)
                        combine(Ju, Vec3(0, 0, 1), J.row(r), -k);
                    ++r;
                }
                if (hi)
                {
                    vals.push_back(e - *hi);
                    if (jac)
                        combine(Ju, Vec3(0, 0, 1), J.row(r), k);
                }
            }

            void describe(const Ctx& ctx, GoalReport& out) const override
            {
                const Vec3 uu = bd.u(ctx, nullptr);
                out.value     = roundTo(std::asin(clamp1(uu.z)) * DEG, 2);
                addList(out, "range", { lo.value_or(NaN), hi.value_or(NaN) });
            }
        };

        struct ImageDirTerm : Term
        {
            bool   useAxis = false;
            int    axisJoint = -1;
            Vec3   axisLocal;
            Point  a, b;
            double p[2] = { 0.0, 0.0 };
            Camera cam{ CameraSpec() };
            double w = 1.0;

            double angle(const Ctx& ctx, std::vector<double>* dang) const
            {
                Vec3 v;
                Jac  Jv;
                if (useAxis)
                {
                    v = ctx.vector(axisJoint, axisLocal, dang ? &Jv : nullptr);
                }
                else
                {
                    Jac        Ja, Jb;
                    const Vec3 pa  = ctx.point(a, dang ? &Ja : nullptr);
                    const Vec3 pbv = ctx.point(b, dang ? &Jb : nullptr);
                    v              = pbv - pa;
                    if (dang)
                    {
                        subtract(Jb, Ja);
                        Jv = std::move(Jb);
                    }
                }
                const double m0  = dot(v, cam.right), m1 = dot(v, cam.down);
                const double ang = std::atan2(p[0] * m1 - p[1] * m0, p[0] * m0 + p[1] * m1);
                if (dang)
                {
                    const double mm = std::max(m0 * m0 + m1 * m1, 1e-12);
                    dang->assign((size_t)ctx.pb.n, 0.0);
                    for (int c = 0; c < ctx.pb.n; ++c)
                    {
                        const Vec3   col = jacCol(Jv, c);
                        const double j0 = dot(cam.right, col), j1 = dot(cam.down, col);
                        (*dang)[c]       = (m0 * j1 - m1 * j0) / mm;
                    }
                }
                return ang;
            }

            void eval(const Ctx& ctx, bool jac, std::vector<double>& vals, Jac& J) const override
            {
                std::vector<double> dang;
                const double        ang = angle(ctx, jac ? &dang : nullptr);
                vals.assign(1, ang * DEG * w);
                if (!jac)
                    return;
                J.reset(1, ctx.pb.n);
                for (int c = 0; c < ctx.pb.n; ++c)
                    J.row(0)[c] = dang[c] * DEG * w;
            }

            void describe(const Ctx& ctx, GoalReport& out) const override
            {
                const double ang = angle(ctx, nullptr);
                out.value        = roundTo(ang * DEG, 2);
                addExtra(out, "signed_deg", roundTo(ang * DEG, 2));
            }
        };

        struct ImagePointTerm : Term
        {
            Point  pt;
            double px[2] = { 0.0, 0.0 };
            Camera cam{ CameraSpec() };
            double mpp = 0.0;
            double w   = 0.5;

            // pixel and d(px)/d(p) rows
            void project(const Vec3& p, double q[2], Vec3* J0, Vec3* J1) const
            {
                const Vec3   c = p - cam.position();
                const double z = dot(c, cam.forward);
                const double f = cam.focalPx();
                const double x = dot(c, cam.right), y = dot(c, cam.down);
                q[0]           = cam.centerX() + f * x / z;
                q[1]           = cam.centerY() + f * y / z;
                if (J0)
                {
                    *J0 = f * (cam.right * z - cam.forward * x) / (z * z);
                    *J1 = f * (cam.down * z - cam.forward * y) / (z * z);
                }
            }

            void eval(const Ctx& ctx, bool jac, std::vector<double>& vals, Jac& J) const override
            {
                Jac        Jp;
                const Vec3 p = ctx.point(pt, jac ? &Jp : nullptr);
                double     q[2];
                Vec3       J0, J1;
                project(p, q, jac ? &J0 : nullptr, &J1);
                const double k = mpp * CM * w;
                vals           = { (q[0] - px[0]) * k, (q[1] - px[1]) * k };
                if (!jac)
                    return;
                J.reset(2, ctx.pb.n);
                combine(Jp, J0, J.row(0), k);
                combine(Jp, J1, J.row(1), k);
            }

            void describe(const Ctx& ctx, GoalReport& out) const override
            {
                const Vec3 p = ctx.point(pt, nullptr);
                double     q[2];
                project(p, q, nullptr, nullptr);
                out.value = roundTo(std::hypot(q[0] - px[0], q[1] - px[1]), 1);
            }
        };

        struct ContactTerm : Term
        {
            Point  a, b;
            double dist = 0.0;
            double w    = 3.0;

            // v (cm) and its Jacobian: 3 rows, or 1 with a distance
            void gap(const Ctx& ctx, bool jac, std::vector<double>& v, Jac& J) const
            {
                Jac        Ja, Jb;
                const Vec3 pa  = ctx.point(a, jac ? &Ja : nullptr);
                const Vec3 pbv = ctx.point(b, jac ? &Jb : nullptr);
                const Vec3 d   = (pa - pbv) * CM;
                if (dist <= 0.0)
                {
                    v = { d.x, d.y, d.z };
                    if (jac)
                    {
                        subtract(Ja, Jb);
                        for (double& x : Ja.a)
                            x *= CM;
                        J = std::move(Ja);
                    }
                    return;
                }
                const double n = norm(d);
                const Vec3   u = d / std::max(n, 1e-9);
                v              = { n - dist * CM };
                if (jac)
                {
                    subtract(Ja, Jb);
                    J.reset(1, ctx.pb.n);
                    combine(Ja, u, J.row(0), CM);
                }
            }

            void eval(const Ctx& ctx, bool jac, std::vector<double>& vals, Jac& J) const override
            {
                gap(ctx, jac, vals, J);
                if (kind == Kind::Soft)
                {
                    const double k = w / std::max(tol, 1e-6) * 0.5;
                    for (double& x : vals)
                        x *= k;
                    if (jac)
                    {
                        for (double& x : J.a)
                            x *= k;
                    }
                }
            }

            void describe(const Ctx& ctx, GoalReport& out) const override
            {
                std::vector<double> v;
                Jac                 J;
                gap(ctx, false, v, J);
                double s = 0.0;
                for (double x : v)
                    s += x * x;
                out.value      = roundTo(std::sqrt(s) * 10.0, 1);
                const Vec3 pa  = ctx.point(a, nullptr);
                const Vec3 pbv = ctx.point(b, nullptr);
                addExtra(out, "distance_mm", roundTo(norm(pa - pbv) * 1000.0, 1));
            }
        };

        struct FacingTerm : Term
        {
            int    fa = -1, fb = -1;
            Vec3   na, nb;
            double w = 0.2;

            void eval(const Ctx& ctx, bool jac, std::vector<double>& vals, Jac& J) const override
            {
                Jac        Ja, Jb;
                const Vec3 ua = ctx.vector(fa, na, jac ? &Ja : nullptr);
                const Vec3 ub = ctx.vector(fb, nb, jac ? &Jb : nullptr);
                const Vec3 s  = (ua + ub) * DEG * w;
                vals          = { s.x, s.y, s.z };
                if (!jac)
                    return;
                J.reset(3, ctx.pb.n);
                for (size_t i = 0; i < J.a.size(); ++i)
                    J.a[i] = (Ja.a[i] + Jb.a[i]) * DEG * w;
            }

            void describe(const Ctx& ctx, GoalReport& out) const override
            {
                const Vec3 ua = ctx.vector(fa, na, nullptr);
                const Vec3 ub = ctx.vector(fb, nb, nullptr);
                out.value     = roundTo(angleBetween(ua, -ub) * DEG, 1);
            }
        };

        struct OnFloorTerm : Term
        {
            Point  pt;
            double z = 0.0;

            void eval(const Ctx& ctx, bool jac, std::vector<double>& vals, Jac& J) const override
            {
                Jac        Jp;
                const Vec3 p = ctx.point(pt, jac ? &Jp : nullptr);
                vals.assign(1, (p.z - z) * CM);
                if (!jac)
                    return;
                J.reset(1, ctx.pb.n);
                for (int c = 0; c < ctx.pb.n; ++c)
                    J.row(0)[c] = Jp.row(2)[c] * CM;
            }

            void describe(const Ctx& ctx, GoalReport& out) const override
            {
                const Vec3 p = ctx.point(pt, nullptr);
                out.value    = roundTo((p.z - z) * 1000.0, 1);
                addExtra(out, "z", roundTo(p.z, 4));
                addExtra(out, "floor_z", roundTo(z, 4));
            }
        };

        struct FlatTerm : Term
        {
            int  f = -1;
            Vec3 n, target, e1, e2;

            void eval(const Ctx& ctx, bool jac, std::vector<double>& vals, Jac& J) const override
            {
                Jac        Ju;
                const Vec3 u = ctx.vector(f, n, jac ? &Ju : nullptr);
                vals         = { dot(u, e1) * DEG, dot(u, e2) * DEG };
                if (!jac)
                    return;
                J.reset(2, ctx.pb.n);
                combine(Ju, e1, J.row(0), DEG);
                combine(Ju, e2, J.row(1), DEG);
            }

            void describe(const Ctx& ctx, GoalReport& out) const override
            {
                const Vec3 u = ctx.vector(f, n, nullptr);
                out.value    = roundTo(angleBetween(u, target) * DEG, 2);
            }
        };

        struct AboveFloorTerm : Term
        {
            Point  pt;
            double z = 0.0;

            void eval(const Ctx& ctx, bool jac, std::vector<double>& vals, Jac& J) const override
            {
                Jac        Jp;
                const Vec3 p = ctx.point(pt, jac ? &Jp : nullptr);
                vals.assign(1, (z - p.z) * CM);
                if (!jac)
                    return;
                J.reset(1, ctx.pb.n);
                for (int c = 0; c < ctx.pb.n; ++c)
                    J.row(0)[c] = -Jp.row(2)[c] * CM;
            }

            void describe(const Ctx& ctx, GoalReport& out) const override
            {
                const Vec3 p = ctx.point(pt, nullptr);
                out.value    = roundTo(std::max(0.0, z - p.z) * 1000.0, 1);
            }
        };

        struct AnchorTerm : Term
        {
            Point  pt;
            Vec3   target;
            double w = 1.0; // a weak anchor (hard=false) pulls with this weight per cm

            void eval(const Ctx& ctx, bool jac, std::vector<double>& vals, Jac& J) const override
            {
                Jac        Jp;
                const Vec3 p = ctx.point(pt, jac ? &Jp : nullptr);
                const Vec3 d = (p - target) * (CM * w);
                vals         = { d.x, d.y, d.z };
                if (!jac)
                    return;
                for (double& x : Jp.a)
                    x *= CM * w;
                J = std::move(Jp);
            }

            void describe(const Ctx& ctx, GoalReport& out) const override
            {
                const Vec3 p = ctx.point(pt, nullptr);
                out.value    = roundTo(norm(p - target) * 1000.0, 1);
            }
        };

        struct OrientTerm : Term
        {
            int  j = -1;
            Mat3 target;

            void eval(const Ctx& ctx, bool jac, std::vector<double>& vals, Jac& J) const override
            {
                const Vec3 r = logRot(transpose(target) * ctx.W[j]) * DEG;
                vals         = { r.x, r.y, r.z };
                if (!jac)
                    return;
                ctx.orientJac(j, J);
                for (double& x : J.a)
                    x *= DEG;
            }

            void describe(const Ctx& ctx, GoalReport& out) const override
            {
                out.value = roundTo(norm(logRot(transpose(target) * ctx.W[j])) * DEG, 2);
            }
        };

        struct RangeTerm : Term
        {
            int                                                                       j = -1, var = -1;
            std::vector<std::pair<std::string, std::pair<std::optional<double>, std::optional<double>>>> limits;
            struct Row
            {
                std::string motion;
                double      sign, limit;
            };
            std::vector<Row> rows;

            void eval(const Ctx& ctx, bool jac, std::vector<double>& vals, Jac& J) const override
            {
                const Variable&      v = ctx.pb.vars[var];
                std::vector<Vec3>    grads;
                std::vector<Anatomy> av = ctx.pb.rangeValues(j, ctx.st.local[j], v, &grads);
                vals.assign(rows.size(), 0.0);
                if (jac)
                    J.reset((int)rows.size(), ctx.pb.n);
                for (size_t i = 0; i < rows.size(); ++i)
                {
                    for (size_t k = 0; k < av.size(); ++k)
                    {
                        if (av[k].motion != rows[i].motion)
                            continue;
                        vals[i] = rows[i].sign * (av[k].value - rows[i].limit);
                        if (jac)
                        {
                            for (int c = 0; c < v.size(); ++c)
                                J.row((int)i)[v.col + c] = rows[i].sign * grads[k][c];
                        }
                        break;
                    }
                }
            }

            std::vector<Anatomy> values(const Ctx& ctx) const
            {
                std::vector<Anatomy> av = ctx.pb.rangeValues(j, ctx.st.local[j], ctx.pb.vars[var], nullptr);
                for (Anatomy& a : av)
                {
                    a.value = roundTo(a.value, 1);
                    a.lo.reset();
                    a.hi.reset();
                    for (const auto& [m, lim] : limits)
                    {
                        if (m == a.motion)
                        {
                            a.lo = lim.first;
                            a.hi = lim.second;
                        }
                    }
                }
                return av;
            }

            void describe(const Ctx& ctx, GoalReport& out) const override
            {
                std::vector<double> v;
                Jac                 J;
                eval(ctx, false, v, J);
                double worst = 0.0;
                for (double x : v)
                    worst = std::max(worst, x);
                out.value = roundTo(worst, 2);
            }
        };

        struct TorsoLeanTerm : Term
        {
            Point                 a, b;
            std::optional<double> lo, hi, fwd, side;
            double                w = 1.0;

            struct Angles
            {
                double lean = 0.0, fwd = 0.0, side = 0.0;
            };

            Angles angles(const Ctx& ctx, std::vector<double>* dl, std::vector<double>* df, std::vector<double>* ds) const
            {
                const bool   jac = dl != nullptr;
                Jac          Ja, Jb;
                const Vec3   pa  = ctx.point(a, jac ? &Ja : nullptr);
                const Vec3   pbv = ctx.point(b, jac ? &Jb : nullptr);
                const Vec3   v   = pbv - pa;
                const Vec3   u   = v / norm(v);
                Angles       out;
                out.lean = std::acos(clamp1(u.z));
                out.fwd  = std::atan2(u.x, u.z);
                out.side = std::atan2(u.y, u.z);
                if (!jac)
                    return out;
                subtract(Jb, Ja);
                Jac Ju;
                normJac(v, Jb, Ju);
                const double s  = std::max(std::sin(out.lean), 1e-6);
                const int    n  = ctx.pb.n;
                dl->assign((size_t)n, 0.0);
                df->assign((size_t)n, 0.0);
                ds->assign((size_t)n, 0.0);
                const double qf = std::max(u.x * u.x + u.z * u.z, 1e-12);
                const double qs = std::max(u.y * u.y + u.z * u.z, 1e-12);
                for (int c = 0; c < n; ++c)
                {
                    const Vec3 ju = jacCol(Ju, c);
                    (*dl)[c]      = -ju.z / s;
                    (*df)[c]      = (u.z * ju.x - u.x * ju.z) / qf;
                    (*ds)[c]      = (u.z * ju.y - u.y * ju.z) / qs;
                }
                return out;
            }

            void eval(const Ctx& ctx, bool jac, std::vector<double>& vals, Jac& J) const override
            {
                std::vector<double> dl, df, ds;
                const Angles        an = angles(ctx, jac ? &dl : nullptr, &df, &ds);
                vals.clear();
                std::vector<std::vector<double>> rows;
                if (kind == Kind::Ineq)
                {
                    if (lo)
                    {
                        vals.push_back(*lo - an.lean * DEG);
                        if (jac)
                        {
                            rows.push_back(dl);
                            for (double& x : rows.back())
                                x *= -DEG;
                        }
                    }
                    if (hi)
                    {
                        vals.push_back(an.lean * DEG - *hi);
                        if (jac)
                        {
                            rows.push_back(dl);
                            for (double& x : rows.back())
                                x *= DEG;
                        }
                    }
                }
                else
                {
                    if (fwd)
                    {
                        vals.push_back((an.fwd * DEG - *fwd) * w);
                        if (jac)
                        {
                            rows.push_back(df);
                            for (double& x : rows.back())
                                x *= DEG * w;
                        }
                    }
                    if (side)
                    {
                        vals.push_back((an.side * DEG - *side) * w);
                        if (jac)
                        {
                            rows.push_back(ds);
                            for (double& x : rows.back())
                                x *= DEG * w;
                        }
                    }
                }
                if (!jac)
                    return;
                J.reset((int)rows.size(), ctx.pb.n);
                for (size_t r = 0; r < rows.size(); ++r)
                    std::copy(rows[r].begin(), rows[r].end(), J.row((int)r));
            }

            void describe(const Ctx& ctx, GoalReport& out) const override
            {
                const Angles an = angles(ctx, nullptr, nullptr, nullptr);
                out.value       = roundTo(an.lean * DEG, 2);
                addExtra(out, "forward_deg", roundTo(an.fwd * DEG, 1));
                addExtra(out, "side_deg", roundTo(an.side * DEG, 1));
            }
        };

        struct BalanceTerm : Term
        {
            Point              com;
            std::vector<Point> support;
            double             w = 0.5;

            void offset(const Ctx& ctx, bool jac, double v[2], Jac& J) const
            {
                Jac        Jc;
                const Vec3 c = ctx.point(com, jac ? &Jc : nullptr);
                Vec3       base;
                Jac        Jb;
                if (jac)
                    Jb.reset(3, ctx.pb.n);
                for (const Point& s : support)
                {
                    Jac        Js;
                    const Vec3 p = ctx.point(s, jac ? &Js : nullptr);
                    base += p / (double)support.size();
                    if (jac)
                    {
                        for (size_t i = 0; i < Jb.a.size(); ++i)
                            Jb.a[i] += Js.a[i] / (double)support.size();
                    }
                }
                v[0] = (c.x - base.x) * CM;
                v[1] = (c.y - base.y) * CM;
                if (!jac)
                    return;
                J.reset(2, ctx.pb.n);
                for (int r = 0; r < 2; ++r)
                {
                    for (int col = 0; col < ctx.pb.n; ++col)
                        J.row(r)[col] = (Jc.row(r)[col] - Jb.row(r)[col]) * CM;
                }
            }

            void eval(const Ctx& ctx, bool jac, std::vector<double>& vals, Jac& J) const override
            {
                double v[2];
                offset(ctx, jac, v, J);
                vals = { v[0] * w, v[1] * w };
                if (jac)
                {
                    for (double& x : J.a)
                        x *= w;
                }
            }

            void describe(const Ctx& ctx, GoalReport& out) const override
            {
                double v[2];
                Jac    J;
                offset(ctx, false, v, J);
                out.value = roundTo(std::hypot(v[0], v[1]), 1);
                addList(out, "offset_cm", { roundTo(v[0], 1), roundTo(v[1], 1) });
            }
        };

        struct LookTerm : Term
        {
            int    j = -1;
            Vec3   d;
            double w = 1.0;

            void eval(const Ctx& ctx, bool jac, std::vector<double>& vals, Jac& J) const override
            {
                Jac        Ju;
                const Vec3 u = ctx.vector(j, Vec3(1, 0, 0), jac ? &Ju : nullptr);
                const Vec3 r = (u - d) * DEG * w;
                vals         = { r.x, r.y, r.z };
                if (!jac)
                    return;
                for (double& x : Ju.a)
                    x *= DEG * w;
                J = std::move(Ju);
            }

            void describe(const Ctx& ctx, GoalReport& out) const override
            {
                const Vec3 u = ctx.vector(j, Vec3(1, 0, 0), nullptr);
                out.value    = roundTo(angleBetween(u, d) * DEG, 1);
            }
        };

        struct StayTerm : Term
        {
            std::vector<double> w; // per variable
            const State*        ref = nullptr;

            void eval(const Ctx& ctx, bool jac, std::vector<double>& vals, Jac& J) const override
            {
                const Problem::Impl& pb = ctx.pb;
                vals.assign((size_t)pb.n, 0.0);
                if (jac)
                    J.reset(pb.n, pb.n);
                for (size_t i = 0; i < pb.vars.size(); ++i)
                {
                    const Variable& v = pb.vars[i];
                    double          r[3] = { 0.0, 0.0, 0.0 };
                    if (v.kind == Variable::Kind::Rot3)
                    {
                        const Vec3 l = logRot(transpose(ref->local[v.joint]) * ctx.st.local[v.joint]) * DEG;
                        r[0] = l.x;
                        r[1] = l.y;
                        r[2] = l.z;
                    }
                    else if (v.kind == Variable::Kind::Hinge)
                    {
                        r[0] = (hingeAngle(ctx.st.local[v.joint], v.axis) - hingeAngle(ref->local[v.joint], v.axis)) * DEG;
                    }
                    else
                    {
                        const Vec3 d = (ctx.st.root - ref->root) * CM;
                        r[0] = d.x;
                        r[1] = d.y;
                        r[2] = d.z;
                    }
                    for (int k = 0; k < v.size(); ++k)
                    {
                        vals[v.col + k] = r[k] * w[i];
                        if (jac)
                            J.row(v.col + k)[v.col + k] = w[i] * (v.kind == Variable::Kind::Trans ? CM : DEG);
                    }
                }
            }
        };

        // ik inverse right Jacobian of SO(3) (posesolver.jr_inv)
        Mat3 jrInv(const Vec3& r)
        {
            const double a    = norm(r);
            const double K[3][3] = { { 0, -r.z, r.y }, { r.z, 0, -r.x }, { -r.y, r.x, 0 } };
            Mat3         out;
            double       k2 = 0.0;
            if (a >= 1e-6)
            {
                double s = std::sin(a);
                if (std::fabs(s) < 1e-12)
                    s = s < 0 ? -1e-12 : 1e-12;
                k2 = 1.0 / (a * a) - (1.0 + std::cos(a)) / (2.0 * a * s);
            }
            for (int i = 0; i < 3; ++i)
            {
                for (int j = 0; j < 3; ++j)
                {
                    double kk = 0.0;
                    for (int t = 0; t < 3; ++t)
                        kk += K[i][t] * K[t][j];
                    out.m[i][j] = (i == j ? 1.0 : 0.0) - 0.5 * K[i][j] + k2 * kk;
                }
            }
            return out;
        }

        struct ComfortTerm : Term
        {
            std::vector<int> js;
            double           w = 0.05;

            void eval(const Ctx& ctx, bool jac, std::vector<double>& vals, Jac& J) const override
            {
                vals.assign(3 * js.size(), 0.0);
                if (jac)
                    J.reset((int)(3 * js.size()), ctx.pb.n);
                for (size_t k = 0; k < js.size(); ++k)
                {
                    const Vec3 r = logRot(ctx.st.local[js[k]]);
                    for (int i = 0; i < 3; ++i)
                        vals[3 * k + i] = r[i] * DEG * w;
                    if (!jac)
                        continue;
                    const Variable& v  = ctx.pb.vars[ctx.pb.varOf[js[k]]];
                    const Mat3      ji = jrInv(r);
                    for (int i = 0; i < 3; ++i)
                    {
                        for (int c = 0; c < 3; ++c)
                            J.row((int)(3 * k) + i)[v.col + c] = ji.m[i][c] * DEG * w;
                    }
                }
            }
        };

        struct SmoothTerm : Term
        {
            int    a = -1, b = -1;
            double w = 0.3;

            void eval(const Ctx& ctx, bool jac, std::vector<double>& vals, Jac& J) const override
            {
                const Vec3 r = (logRot(ctx.st.local[a]) - logRot(ctx.st.local[b])) * DEG * w;
                vals         = { r.x, r.y, r.z };
                if (!jac)
                    return;
                J.reset(3, ctx.pb.n);
                const Variable& va = ctx.pb.vars[ctx.pb.varOf[a]];
                const Variable& vb = ctx.pb.vars[ctx.pb.varOf[b]];
                for (int i = 0; i < 3; ++i)
                {
                    J.row(i)[va.col + i] = DEG * w;
                    J.row(i)[vb.col + i] = -DEG * w;
                }
            }
        };
    }

    // ------------------------------------------------------------------ anatomy (posesolver.range_values)

    namespace
    {
        struct Grad3
        {
            double value;
            Vec3   grad; // over the joint's own variable (rot3: 3 components)
        };

        // values and gradients of the joint's local rotation L (L <- L exp(d))
        std::vector<std::pair<std::string, Grad3>> anatomyGrad(const std::string& name, const Mat3& L, const Vec3* rest)
        {
            std::vector<std::pair<std::string, Grad3>> out;
            const RangeFamily* fam = jointFamily(name);
            double             s   = sideSign(name);
            if (s == 0.0)
                s = 1.0;
            const Quat q = quatOf(L);
            const Vec3 qv(q.x, q.y, q.z);
            // _qderiv: dw = -0.5 v; dv rows k = 0.5 (w e_k + e_k x v)
            const Vec3 dw = qv * -0.5;
            Vec3       dv[3];
            for (int k = 0; k < 3; ++k)
            {
                Vec3 e;
                e[k]  = 1.0;
                dv[k] = (e * q.w + cross(e, qv)) * 0.5;
            }
            auto vec = [&](const Vec3& c, Vec3 df[3])
            {
                const Vec3 f = c * L;
                for (int k = 0; k < 3; ++k)
                {
                    Vec3 e;
                    e[k]  = 1.0;
                    df[k] = cross(e, f);
                }
                return f;
            };
            // d/d delta_k of a component: gather a vector over k
            auto comp = [](const Vec3 df[3], int i) { return Vec3(df[0][i], df[1][i], df[2][i]); };
            auto atan2g = [](double a, double b, const Vec3& da, const Vec3& db)
            {
                const double qq = std::max(a * a + b * b, 1e-12);
                return Grad3{ std::atan2(a, b) * DEG, (db * -a + da * b) / qq * DEG };
            };
            auto asing = [](double c, const Vec3& dc)
            {
                c = clamp1(c);
                return Grad3{ std::asin(c) * DEG, dc / std::max(std::sqrt(1.0 - c * c), 1e-6) * DEG };
            };
            auto twist = [&](const Vec3& axis)
            {
                const Vec3   r  = solver::unit(axis);
                const double sv = dot(qv, r);
                const Vec3   ds(dot(dv[0], r), dot(dv[1], r), dot(dv[2], r));
                const double val = 2.0 * std::atan2(sv, q.w);
                const Vec3   dt  = (ds * q.w - dw * sv) * 2.0 / std::max(sv * sv + q.w * q.w, 1e-12);
                return Grad3{ val * DEG, dt * DEG };
            };
            const std::string family = fam ? fam->family : "";
            if (family == "mShoulder" && rest)
            {
                Vec3       dd[3];
                const Vec3 d = vec(*rest, dd);
                out.push_back({ "back", asing(d.x, comp(dd, 0)) });
                out.push_back({ "across", asing(s * d.y, comp(dd, 1) * s) });
                out.push_back({ "twist", twist(*rest) });
            }
            else if (family == "mWrist" && rest)
            {
                Vec3       dd[3];
                const Vec3 d = vec(*rest, dd);
                out.push_back({ "flex", atan2g(-d.z, s * d.y, comp(dd, 2) * -1.0, comp(dd, 1) * s) });
                out.push_back({ "deviate", atan2g(d.x, s * d.y, comp(dd, 0), comp(dd, 1) * s) });
                out.push_back({ "twist", twist(*rest) });
            }
            else if (family == "mHip" && rest)
            {
                Vec3       dd[3];
                const Vec3 d = vec(*rest, dd);
                out.push_back({ "flex", atan2g(d.x, -d.z, comp(dd, 0), comp(dd, 2) * -1.0) });
                out.push_back({ "abduct", asing(s * d.y, comp(dd, 1) * s) });
                out.push_back({ "twist", twist(*rest) });
            }
            else if (family == "mAnkle")
            {
                Vec3       df[3], du[3];
                const Vec3 f = vec(Vec3(1, 0, 0), df);
                const Vec3 u = vec(Vec3(0, 0, 1), du);
                out.push_back({ "flex", atan2g(-f.z, f.x, comp(df, 2) * -1.0, comp(df, 0)) });
                out.push_back({ "invert", atan2g(s * u.y, u.z, comp(du, 1) * s, comp(du, 2)) });
                out.push_back({ "twist", twist(Vec3(0, 0, 1)) });
            }
            else if (family == "mTorso" || family == "mChest" || family == "mNeck" || family == "mHead")
            {
                Vec3       du[3];
                const Vec3 u = vec(Vec3(0, 0, 1), du);
                out.push_back({ "forward", atan2g(u.x, u.z, comp(du, 0), comp(du, 2)) });
                out.push_back({ "side", asing(-u.y, comp(du, 1) * -1.0) });
                out.push_back({ "twist", twist(Vec3(0, 0, 1)) });
            }
            else if (family == "mCollar")
            {
                Vec3       db[3];
                const Vec3 b = vec(Vec3(0, s, 0), db);
                out.push_back({ "up", asing(b.z, comp(db, 2)) });
                out.push_back({ "forward", asing(b.x, comp(db, 0)) });
                out.push_back({ "twist", twist(Vec3(0, s, 0)) });
            }
            if (name != "mPelvis")
            {
                const double nv  = norm(qv);
                const double ang = 2.0 * std::atan2(nv, q.w);
                out.push_back({ "rotation", Grad3{ ang * DEG, nv > 1e-9 ? qv / nv * DEG : Vec3() } });
            }
            return out;
        }
    }

    std::vector<Anatomy> anatomicalValues(const std::string& joint, const Mat3& local, const Vec3& restDir)
    {
        std::vector<Anatomy> out;
        const Vec3           rest = solver::unit(restDir);
        for (const auto& [m, g] : anatomyGrad(joint, local, &rest))
            out.push_back(Anatomy{ m, g.value, std::nullopt, std::nullopt });
        return out;
    }

    std::vector<Anatomy> defaultLimits(const std::string& joint, bool hinge, const std::string& flexibility, double maxRotationDeg)
    {
        std::vector<Anatomy> out;
        const RangeFamily*   fam = jointFamily(joint);
        if (fam)
        {
            for (const RangeRow& r : fam->rows)
            {
                const std::string m = r.motion;
                const double      k = (m == "twist" || hinge) ? 1.0 : flexScale(flexibility, fam->family);
                out.push_back(Anatomy{ m, 0.0, r.lo * k, r.hi * k });
            }
        }
        if (joint != "mPelvis" && !hinge)
            out.push_back(Anatomy{ "rotation", 0.0, std::nullopt, maxRotationDeg });
        return out;
    }

    std::vector<Anatomy> Problem::Impl::rangeValues(int j, const Mat3& L, const Variable& v, std::vector<Vec3>* grads) const
    {
        std::vector<Anatomy> out;
        if (grads)
            grads->clear();
        if (v.kind == Variable::Kind::Hinge)
        {
            const double th = hingeAngle(L, v.axis) * v.natural;
            out.push_back(Anatomy{ "flex", th * DEG, std::nullopt, std::nullopt });
            if (grads)
                grads->push_back(Vec3(v.natural * DEG, 0, 0));
            return out;
        }
        auto        it   = restDir.find(j);
        const Vec3* rest = it != restDir.end() ? &it->second : nullptr;
        for (const auto& [m, g] : anatomyGrad(sk.names[j], L, rest))
        {
            out.push_back(Anatomy{ m, g.value, std::nullopt, std::nullopt });
            if (grads)
                grads->push_back(g.grad);
        }
        return out;
    }

    // ------------------------------------------------------------------ building the problem

    int Problem::Impl::joint(const std::string& name) const
    {
        const int i = sk.find(name);
        if (i < 0)
            throw SolveError("unknown joint " + (name.empty() ? std::string("(none)") : name));
        return i;
    }

    void Problem::Impl::levers(Part& part) const
    {
        part.levers.clear();
        const size_t count = sk.names.size();
        for (size_t j = 0; j < count; ++j)
        {
            const bool inA = inChain[part.anchor][j] != 0;
            const bool inF = inChain[part.frame][j] != 0;
            if ((inA || inF) && varOf[j] >= 0)
                part.levers.push_back(Lever{ varOf[j], inA, inF });
        }
    }

    namespace
    {
        // posesolver.resolve_point
        void resolveInto(const Skeleton& sk, const SiteTable& sites, const PointSpec& spec, double w, std::vector<ResolvedPart>& out)
        {
            if (!spec.mid.empty())
            {
                const double k = 1.0 / (double)spec.mid.size();
                for (const PointSpec& s : spec.mid)
                    resolveInto(sk, sites, s, w * k, out);
                return;
            }
            const int j = sk.find(spec.joint);
            if (j < 0)
                throw SolveError("unknown joint " + (spec.joint.empty() ? std::string("(none)") : spec.joint));
            ResolvedPart p;
            p.w = w;
            if (spec.offset)
            {
                p.anchor = p.frame = j;
                p.offset           = *spec.offset;
            }
            else if (!spec.site.empty())
            {
                const SiteEntry& s = siteEntry(sites, spec.site, sk.names[j]);
                p.anchor           = sk.find(s.anchor);
                p.frame            = sk.find(s.frame);
                if (p.anchor < 0 || p.frame < 0)
                    throw SolveError("site " + spec.site + " hangs from " + s.anchor + " / " + s.frame + ", which this skeleton lacks");
                p.offset = s.offset;
            }
            else
            {
                p.anchor = p.frame = j;
            }
            out.push_back(p);
        }

        bool ancestorOrSelf(const Skeleton& sk, int ancestor, int joint)
        {
            for (int k = joint; k >= 0; k = sk.parent[k])
            {
                if (k == ancestor)
                    return true;
            }
            return false;
        }
    }

    std::vector<ResolvedPart> resolvePoint(const Skeleton& skeleton, const PointSpec& point, const SiteOverrides& overrides)
    {
        std::vector<ResolvedPart> out;
        if (overrides.empty())
            resolveInto(skeleton, defaultSites(), point, 1.0, out);
        else
            resolveInto(skeleton, mergeSites(defaultSites(), overrides), point, 1.0, out);
        return out;
    }

    Vec3 pointPosition(const std::vector<Mat3>& W, const std::vector<Vec3>& P, const std::vector<ResolvedPart>& parts)
    {
        Vec3 p;
        for (const ResolvedPart& r : parts)
            p += r.w * (P[r.anchor] + r.offset * W[r.frame]);
        return p;
    }

    bool limbOfPoint(const Skeleton& sk, const std::vector<ResolvedPart>& parts, std::string& root, std::string& middle, std::string& limb)
    {
        for (const LimbDef& l : limbDefs())
        {
            const int e = sk.find(l.end);
            if (e < 0 || sk.names[e] != l.end || parts.empty())
                continue;
            bool all = true;
            for (const ResolvedPart& p : parts)
            {
                if (!ancestorOrSelf(sk, e, p.anchor) && !ancestorOrSelf(sk, e, p.frame))
                    all = false;
            }
            if (all)
            {
                root   = l.root;
                middle = l.middle;
                limb   = l.name;
                return true;
            }
        }
        return false;
    }

    Point Problem::Impl::resolve(const PointSpec& point) const
    {
        std::vector<ResolvedPart> parts;
        resolveInto(sk, sites, point, 1.0, parts);
        Point out;
        for (const ResolvedPart& r : parts)
        {
            Part p;
            p.w      = r.w;
            p.anchor = r.anchor;
            p.frame  = r.frame;
            p.offset = r.offset;
            levers(p);
            out.push_back(std::move(p));
        }
        return out;
    }

    bool Problem::Impl::siteNormal(const PointSpec& point, int& frame, Vec3& normal) const
    {
        if (!point.site.empty() && !point.joint.empty())
        {
            const SiteEntry& s = siteEntry(sites, point.site, sk.names[joint(point.joint)]);
            if (s.normal)
            {
                frame = sk.find(s.frame);
                if (frame < 0)
                    return false;
                normal = solver::unit(*s.normal);
                return true;
            }
        }
        if (point.normal && !point.joint.empty())
        {
            frame  = joint(point.joint);
            normal = solver::unit(*point.normal);
            return true;
        }
        return false;
    }

    double Problem::Impl::floorZ() const
    {
        if (spec.floorMode == "value")
            return spec.floorZ;
        State st;
        if (spec.floorMode == "lowest")
        {
            st = measured;
        }
        else
        {
            st.local.assign(sk.names.size(), Mat3::identity());
            st.root = sk.rootPos - sk.pelvisOffset;
        }
        const Ctx ctx(*this, st);
        double    z = std::numeric_limits<double>::infinity();
        for (const char* s : SIDES)
        {
            PointSpec ps;
            ps.joint = std::string("mAnkle") + s;
            ps.site  = "sole";
            z        = std::min(z, ctx.point(resolve(ps), nullptr).z);
        }
        return z;
    }

    namespace
    {
        std::string pointJoint(const PointSpec& p) { return p.joint.empty() ? std::string("None") : p.joint; }
    }

    void Problem::Impl::buildTerms()
    {
        const Ctx ctx0(*this, start);
        const Ctx ctxm(*this, measured);
        terms.clear();
        floor.reset();
        std::vector<std::pair<std::string, std::string>> onFloorSites;
        std::map<std::string, std::vector<std::pair<std::string, std::pair<std::optional<double>, std::optional<double>>>>> userRanges;
        const Options& o = spec.options;

        auto boneDir = [&](const Goal& g, BoneDirTerm& t, bool withRoll)
        {
            t.j                    = joint(g.joint);
            const std::string end  = !g.toward.empty() ? g.toward : boneEnd(sk.names[t.j]);
            const int         e    = end.empty() ? -1 : sk.find(end);
            if (e < 0)
                throw SolveError("bone_dir: " + g.joint + " has no bone end");
            t.end = e;
            Part pa;
            pa.anchor = pa.frame = t.j;
            levers(pa);
            Part pe;
            pe.anchor = pe.frame = e;
            levers(pe);
            t.a = { pa };
            t.b = { pe };
            if (norm(g.direction) < 1e-9)
                throw SolveError("bone_dir " + g.joint + ": direction must not be zero");
            t.d = solver::unit(g.direction);
            t.w = g.weight.value_or(1.0);
            if (withRoll && g.rollToward)
            {
                t.roll = solver::unit(*g.rollToward);
                t.side = sideAxis(sk.names[t.j]);
                t.rw   = g.rollWeight.value_or(0.5 * t.w);
            }
            t.unit = "deg";
        };

        for (const Goal& g : spec.goals)
        {
            switch (g.type)
            {
                case GoalType::BoneDir:
                {
                    if (g.withinDeg)
                    {
                        auto t = std::make_unique<BoneConeTerm>();
                        boneDir(g, t->bd, false);
                        t->type   = "BoneCone";
                        t->kind   = Kind::Ineq;
                        t->within = *g.withinDeg;
                        t->tol    = g.toleranceDeg.value_or(0.5);
                        t->name   = !g.name.empty() ? g.name : "bone_dir " + g.joint + " within " + fmt("%g", t->within) + " deg";
                        t->unit   = "deg";
                        terms.push_back(std::move(t));
                    }
                    else
                    {
                        auto t = std::make_unique<BoneDirTerm>();
                        boneDir(g, *t, true);
                        t->type = "BoneDir";
                        t->name = !g.name.empty() ? g.name : "bone_dir " + g.joint;
                        terms.push_back(std::move(t));
                    }
                    break;
                }
                case GoalType::ImageDir:
                {
                    auto t = std::make_unique<ImageDirTerm>();
                    t->type = "ImageDir";
                    if (!g.axisJoint.empty())
                    {
                        t->useAxis   = true;
                        t->axisJoint = joint(g.axisJoint);
                        t->axisLocal = g.axisLocal;
                    }
                    else
                    {
                        t->a = resolve(g.from);
                        t->b = resolve(g.to);
                    }
                    const double pn = std::hypot(g.photoVec[0], g.photoVec[1]);
                    if (!(pn > 0.0))
                        throw SolveError("image_dir: photo_vec must not be zero");
                    t->p[0] = g.photoVec[0] / pn;
                    t->p[1] = g.photoVec[1] / pn;
                    t->cam  = Camera(g.camera ? *g.camera : camera);
                    t->w    = g.weight.value_or(1.0);
                    t->name = !g.name.empty() ? g.name : "image_dir " + g.label;
                    t->unit = "deg";
                    terms.push_back(std::move(t));
                    break;
                }
                case GoalType::ImagePoint:
                {
                    auto t   = std::make_unique<ImagePointTerm>();
                    t->type  = "ImagePoint";
                    t->pt    = resolve(g.at);
                    t->px[0] = g.px[0];
                    t->px[1] = g.px[1];
                    t->cam   = Camera(g.camera ? *g.camera : camera);
                    if (g.mPerPx)
                        t->mpp = *g.mPerPx;
                    else if (t->cam.perspective())
                        t->mpp = *t->cam.spec.distance / t->cam.focalPx();
                    else
                        throw SolveError("image_point needs m_per_px or a camera with fov and distance");
                    if (!t->cam.perspective())
                        throw SolveError("image_point needs a camera with fov and distance");
                    t->w    = g.weight.value_or(0.5);
                    t->name = !g.name.empty() ? g.name : "image_point " + g.joint;
                    t->unit = "px";
                    terms.push_back(std::move(t));
                    break;
                }
                case GoalType::Contact:
                {
                    auto t   = std::make_unique<ContactTerm>();
                    t->type  = "Contact";
                    t->a     = resolve(g.a);
                    t->b     = resolve(g.b);
                    t->dist  = g.distance + g.skinMargin;
                    t->tol   = g.tolerance.value_or(0.005) * CM;
                    t->kind  = g.hard ? Kind::Eq : Kind::Soft;
                    t->w     = g.weight.value_or(3.0);
                    t->name  = !g.name.empty() ? g.name
                                               : "contact " + pointJoint(g.a) + "." + g.a.site + " - " + pointJoint(g.b) + "." + g.b.site;
                    t->unit  = "mm";
                    t->group = true;
                    const std::string cname = t->name;
                    terms.push_back(std::move(t));
                    if (g.facing)
                    {
                        int  fa = -1, fb = -1;
                        Vec3 na, nb;
                        if (siteNormal(g.a, fa, na) && siteNormal(g.b, fb, nb))
                        {
                            auto f  = std::make_unique<FacingTerm>();
                            f->type = "Facing";
                            f->fa   = fa;
                            f->fb   = fb;
                            f->na   = na;
                            f->nb   = nb;
                            f->w    = g.facingWeight;
                            f->name = cname + " (facing)";
                            f->unit = "deg";
                            terms.push_back(std::move(f));
                        }
                    }
                    break;
                }
                case GoalType::OnFloor:
                {
                    if (!floor)
                        floor = floorZ();
                    auto t  = std::make_unique<OnFloorTerm>();
                    t->type = "OnFloor";
                    t->kind = Kind::Eq;
                    t->pt   = resolve(g.at);
                    t->z    = (g.floorZ ? *g.floorZ : *floor) + g.height;
                    t->tol  = g.tolerance.value_or(0.003) * CM;
                    t->name = !g.name.empty() ? g.name : "on_floor " + pointJoint(g.at) + "." + g.at.site;
                    t->unit = "mm";
                    terms.push_back(std::move(t));
                    onFloorSites.push_back({ g.at.joint, g.at.site });
                    if (g.flat)
                    {
                        int  f = -1;
                        Vec3 nrm;
                        if (siteNormal(g.at, f, nrm))
                        {
                            auto ft    = std::make_unique<FlatTerm>();
                            ft->type   = "Flat";
                            ft->kind   = Kind::Eq;
                            ft->f      = f;
                            ft->n      = nrm;
                            ft->target = Vec3(0, 0, -1);
                            ft->tol    = g.flatToleranceDeg;
                            ft->name   = "flat " + pointJoint(g.at);
                            ft->unit   = "deg";
                            ft->group  = true;
                            ft->e1     = solver::unit(cross(ft->target, std::fabs(ft->target.x) < 0.9 ? Vec3(1, 0, 0) : Vec3(0, 1, 0)));
                            ft->e2     = cross(ft->target, ft->e1);
                            terms.push_back(std::move(ft));
                        }
                    }
                    break;
                }
                case GoalType::AboveFloor:
                {
                    if (!floor)
                        floor = floorZ();
                    for (const PointSpec& s : g.atList)
                    {
                        auto t  = std::make_unique<AboveFloorTerm>();
                        t->type = "AboveFloor";
                        t->kind = Kind::Ineq;
                        t->pt   = resolve(s);
                        t->z    = *floor;
                        t->tol  = 0.3;
                        t->name = "above_floor " + pointJoint(s) + "." + s.site;
                        t->unit = "mm";
                        terms.push_back(std::move(t));
                    }
                    break;
                }
                case GoalType::Anchor:
                {
                    auto t  = std::make_unique<AnchorTerm>();
                    t->type = "Anchor";
                    t->kind = g.hard ? Kind::Eq : Kind::Soft;
                    t->w    = g.hard ? 1.0 : g.weight.value_or(1.0);
                    t->pt   = resolve(g.at);
                    if (g.position)
                        t->target = *g.position;
                    else
                        t->target = ctxm.point(t->pt, nullptr);
                    t->tol   = g.tolerance.value_or(0.003) * CM;
                    t->name  = !g.name.empty() ? g.name : "anchor " + pointJoint(g.at);
                    t->unit  = "mm";
                    t->group = true;
                    terms.push_back(std::move(t));
                    if (g.orient)
                    {
                        auto ot    = std::make_unique<OrientTerm>();
                        ot->type   = "Orient";
                        ot->kind   = Kind::Eq;
                        ot->j      = joint(g.at.joint);
                        ot->target = g.orientTarget ? *g.orientTarget : ctxm.W[ot->j];
                        ot->tol    = g.orientToleranceDeg;
                        ot->name   = "orient " + sk.names[ot->j];
                        ot->unit   = "deg";
                        ot->group  = true;
                        terms.push_back(std::move(ot));
                    }
                    break;
                }
                case GoalType::JointRange:
                {
                    const std::string name = sk.names[joint(g.joint)];
                    auto&             list = userRanges[name];
                    bool              found = false;
                    for (auto& e : list)
                    {
                        if (e.first == g.motion)
                        {
                            e.second = { g.min, g.max };
                            found    = true;
                        }
                    }
                    if (!found)
                        list.push_back({ g.motion, { g.min, g.max } });
                    break;
                }
                case GoalType::BoneElevation:
                {
                    auto t = std::make_unique<BoneElevationTerm>();
                    Goal gg = g;
                    gg.direction = Vec3(0, 0, 1);
                    boneDir(gg, t->bd, false);
                    t->type = "BoneElevation";
                    t->kind = Kind::Ineq;
                    t->lo   = g.min;
                    t->hi   = g.max;
                    t->tol  = g.toleranceDeg.value_or(0.5);
                    t->name = !g.name.empty() ? g.name : "bone_elevation " + g.joint;
                    t->unit = "deg";
                    terms.push_back(std::move(t));
                    break;
                }
                case GoalType::TorsoLean:
                {
                    auto t  = std::make_unique<TorsoLeanTerm>();
                    t->type = "TorsoLean";
                    PointSpec hips, shoulders;
                    for (const char* s : SIDES)
                    {
                        PointSpec h;
                        h.joint = std::string("mHip") + s;
                        hips.mid.push_back(h);
                        PointSpec sh;
                        sh.joint = std::string("mShoulder") + s;
                        shoulders.mid.push_back(sh);
                    }
                    t->a    = resolve(hips);
                    t->b    = resolve(shoulders);
                    t->lo   = g.min;
                    t->hi   = g.max;
                    t->fwd  = g.forwardDeg;
                    t->side = g.sideDeg;
                    t->w    = g.weight.value_or(1.0);
                    t->kind = (g.min || g.max) ? Kind::Ineq : Kind::Soft;
                    t->tol  = g.toleranceDeg.value_or(0.5);
                    t->name = !g.name.empty() ? g.name : "torso_lean";
                    t->unit = "deg";
                    terms.push_back(std::move(t));
                    break;
                }
                case GoalType::Balance:
                {
                    auto t  = std::make_unique<BalanceTerm>();
                    t->type = "Balance";
                    std::vector<std::pair<int, std::pair<int, std::pair<double, double>>>> segs;
                    double total = 0.0;
                    for (const Segment& s : segments())
                    {
                        const int ia = sk.find(s.a), ib = sk.find(s.b);
                        if (ia >= 0 && ib >= 0 && sk.names[ia] == s.a && sk.names[ib] == s.b)
                        {
                            segs.push_back({ ia, { ib, { s.t, s.mass } } });
                            total += s.mass;
                        }
                    }
                    for (const auto& s : segs)
                    {
                        const double k = s.second.second.second / total;
                        const double tt = s.second.second.first;
                        Part pa;
                        pa.w      = k * (1.0 - tt);
                        pa.anchor = pa.frame = s.first;
                        levers(pa);
                        Part pbp;
                        pbp.w      = k * tt;
                        pbp.anchor = pbp.frame = s.second.first;
                        levers(pbp);
                        t->com.push_back(pa);
                        t->com.push_back(pbp);
                    }
                    for (const PointSpec& s : g.support)
                        t->support.push_back(resolve(s));
                    t->w    = g.weight.value_or(0.5);
                    t->name = !g.name.empty() ? g.name : "balance";
                    t->unit = "cm";
                    terms.push_back(std::move(t));
                    break;
                }
                case GoalType::Look:
                {
                    auto t  = std::make_unique<LookTerm>();
                    t->type = "Look";
                    t->j    = joint("mHead");
                    if (norm(g.lookToward) < 1e-9)
                        throw SolveError("look: toward must not be zero");
                    t->d    = solver::unit(g.lookToward);
                    t->w    = g.weight.value_or(1.0);
                    t->name = !g.name.empty() ? g.name : "look";
                    t->unit = "deg";
                    terms.push_back(std::move(t));
                    break;
                }
                case GoalType::SmoothSpine:
                    break;
            }
        }

        // the floor: nothing else goes through it
        if (floor && o.floorGuard)
        {
            for (const char* s : SIDES)
            {
                for (const char* site : { "sole", "heel", "toe_tip", "knee_front" })
                {
                    const std::string jn = std::string(std::string(site) == "knee_front" ? "mKnee" : "mAnkle") + s;
                    const std::string foot = std::string("mFoot") + s;
                    bool held = false;
                    for (const auto& h : onFloorSites)
                    {
                        if ((h.first == jn || h.first == foot) && h.second == site)
                            held = true;
                    }
                    if (held)
                        continue;
                    PointSpec ps;
                    ps.joint = jn;
                    ps.site  = site;
                    auto t   = std::make_unique<AboveFloorTerm>();
                    t->type  = "AboveFloor";
                    t->kind  = Kind::Ineq;
                    t->pt    = resolve(ps);
                    t->z     = *floor;
                    t->tol   = 0.3;
                    t->name  = "above_floor " + jn + "." + site;
                    t->unit  = "mm";
                    terms.push_back(std::move(t));
                }
            }
        }

        // ranges
        std::set<int> oriented;
        for (const auto& t : terms)
        {
            if (const OrientTerm* ot = dynamic_cast<const OrientTerm*>(t.get()))
                oriented.insert(ot->j);
        }
        if (o.ranges)
        {
            for (size_t vi = 0; vi < vars.size(); ++vi)
            {
                const Variable& v = vars[vi];
                if (v.kind == Variable::Kind::Trans)
                    continue;
                const std::string& name = sk.names[v.joint];
                const RangeFamily* fam  = jointFamily(name);
                std::vector<std::pair<std::string, std::pair<std::optional<double>, std::optional<double>>>> lim;
                if (fam)
                {
                    for (const RangeRow& r : fam->rows)
                    {
                        const std::string m = r.motion;
                        const double      k = (m == "twist" || v.kind == Variable::Kind::Hinge) ? 1.0 : flexScale(o.flexibility, fam->family);
                        lim.push_back({ m, { r.lo * k, r.hi * k } });
                    }
                }
                if (name != "mPelvis" && v.kind == Variable::Kind::Rot3)
                    lim.push_back({ "rotation", { std::nullopt, o.maxRotationDeg } });
                if (fam && std::string(fam->family) == "mAnkle")
                {
                    for (auto& e : lim)
                    {
                        if (e.first != "flex")
                            continue;
                        // a calibrated sole (sites_override): the flat foot is this mesh body's neutral
                        auto s = sites.find("sole");
                        if (s == sites.end())
                            break;
                        auto side = s->second.find(sideOf(name));
                        if (side == s->second.end() || !side->second.normal)
                            break;
                        const Vec3   nn    = *side->second.normal;
                        const double delta = std::atan2(nn.x, -nn.z) * DEG;
                        if (std::fabs(delta) > 0.5)
                            e.second = { *e.second.first + delta, *e.second.second + delta };
                        break;
                    }
                }
                if (oriented.count(v.joint))
                {
                    std::vector<Anatomy> now = rangeValues(v.joint, measured.local[v.joint], v, nullptr);
                    for (auto& e : lim)
                    {
                        double nv = 0.0;
                        bool   has = false;
                        for (const Anatomy& a : now)
                        {
                            if (a.motion == e.first)
                            {
                                nv  = a.value;
                                has = true;
                            }
                        }
                        if (!has)
                            continue;
                        if (e.second.first)
                            e.second.first = std::min(*e.second.first, nv);
                        if (e.second.second)
                            e.second.second = std::max(*e.second.second, nv);
                    }
                }
                auto ur = userRanges.find(name);
                if (ur != userRanges.end())
                {
                    for (const auto& u : ur->second)
                    {
                        bool found = false;
                        for (auto& e : lim)
                        {
                            if (e.first == u.first)
                            {
                                e.second = u.second;
                                found    = true;
                            }
                        }
                        if (!found)
                            lim.push_back(u);
                    }
                }
                if (lim.empty())
                    continue;
                auto t    = std::make_unique<RangeTerm>();
                t->type   = "Range";
                t->kind   = Kind::Ineq;
                t->j      = v.joint;
                t->var    = (int)vi;
                t->limits = lim;
                t->tol    = 0.5;
                t->name   = "range " + name;
                t->unit   = "deg";
                for (const auto& [m, lh] : lim)
                {
                    if (lh.first)
                        t->rows.push_back({ m, -1.0, *lh.first });
                    if (lh.second)
                        t->rows.push_back({ m, 1.0, *lh.second });
                }
                terms.push_back(std::move(t));
            }
        }

        // stay
        {
            auto t  = std::make_unique<StayTerm>();
            t->type = "Stay";
            t->name = "stay";
            t->unit = "deg";
            t->ref  = &ref;
            for (const Variable& v : vars)
            {
                if (v.kind == Variable::Kind::Trans)
                {
                    auto it = o.stayWeight.find("trans");
                    t->w.push_back(it != o.stayWeight.end() ? it->second : 0.3 * o.stay);
                    continue;
                }
                const std::string& name = sk.names[v.joint];
                const std::string  part = bodyPart(name);
                auto               byName = o.stayWeight.find(name);
                auto               byPart = o.stayWeight.find(part);
                t->w.push_back(byName != o.stayWeight.end() ? byName->second
                                                            : (byPart != o.stayWeight.end() ? byPart->second : stayDefault(part) * o.stay));
            }
            terms.push_back(std::move(t));
        }
        if (o.comfort > 0.0)
        {
            auto t = std::make_unique<ComfortTerm>();
            for (const Variable& v : vars)
            {
                if (v.kind != Variable::Kind::Rot3 || oriented.count(v.joint))
                    continue;
                const std::string& jn = sk.names[v.joint];
                for (const char* p : { "mTorso", "mChest", "mNeck", "mHead", "mCollar", "mWrist", "mAnkle" })
                {
                    if (startsWith(jn, p))
                    {
                        t->js.push_back(v.joint);
                        break;
                    }
                }
            }
            if (!t->js.empty())
            {
                t->type = "Comfort";
                t->name = "comfort";
                t->unit = "deg";
                t->w    = o.comfort;
                terms.push_back(std::move(t));
            }
        }
        {
            const int a = sk.find("mTorso"), b = sk.find("mChest");
            if (a >= 0 && b >= 0 && varOf[a] >= 0 && varOf[b] >= 0 && vars[varOf[a]].kind == Variable::Kind::Rot3 &&
                vars[varOf[b]].kind == Variable::Kind::Rot3)
            {
                auto t  = std::make_unique<SmoothTerm>();
                t->type = "Smooth";
                t->name = "spine smooth";
                t->unit = "deg";
                t->a    = a;
                t->b    = b;
                t->w    = o.spineSmooth;
                terms.push_back(std::move(t));
            }
        }
    }

    Problem::Problem(const Skeleton& skeleton, const Spec& spec) : mImpl(std::make_unique<Impl>())
    {
        Impl& pb = *mImpl;
        std::string why;
        if (!skeleton.valid(why))
            throw SolveError(why);
        pb.sk     = skeleton;
        pb.spec   = spec;
        pb.sites  = mergeSites(defaultSites(), spec.sitesOverride);
        pb.camera = spec.camera;
        const size_t count = skeleton.names.size();
        pb.inChain.assign(count, std::vector<char>(count, 0));
        for (size_t i = 0; i < count; ++i)
        {
            for (int k = (int)i; k >= 0; k = skeleton.parent[k])
                pb.inChain[i][k] = 1;
        }
        pb.measured.local = skeleton.local;
        pb.measured.root  = skeleton.rootPos;
        if (spec.start == "tpose")
        {
            pb.start.local.assign(count, Mat3::identity());
            pb.start.root = skeleton.rootPos - skeleton.pelvisOffset;
        }
        else if (spec.start == "current" || spec.start.empty())
        {
            pb.start = pb.measured;
        }
        else
        {
            throw SolveError("start must be \"current\" or \"tpose\"");
        }
        for (const std::string& n : bodyList())
        {
            const int i = skeleton.find(n);
            if (i < 0 || skeleton.names[i] != n)
                continue;
            if (std::optional<Vec3> b = restBone(skeleton, i))
                pb.restDir[i] = solver::unit(*b);
        }
        // variables
        std::set<std::string> lock;
        bool                  lockPosition = spec.lockPosition;
        for (const std::string& l : spec.lock)
        {
            if (iequals(l, "mPelvis.position"))
                lockPosition = true;
            else
                lock.insert(pb.sk.names[pb.joint(l)]);
        }
        std::vector<std::string> names;
        if (spec.freeAuto)
            names = bodyList();
        else
        {
            for (const std::string& f : spec.free)
                names.push_back(pb.sk.names[pb.joint(f)]);
        }
        std::vector<std::string> kept;
        for (const std::string& nm : names)
        {
            const int i = skeleton.find(nm);
            if (i >= 0 && skeleton.names[i] == nm && !lock.count(nm) &&
                std::find(kept.begin(), kept.end(), nm) == kept.end())
                kept.push_back(nm);
        }
        pb.varOf.assign(count, -1);
        int col = 0;
        if (std::find(kept.begin(), kept.end(), "mPelvis") != kept.end() && !lockPosition)
        {
            Variable v;
            v.joint = skeleton.find("mPelvis");
            v.kind  = Variable::Kind::Trans;
            v.col   = col;
            pb.trans = (int)pb.vars.size();
            pb.vars.push_back(v);
            col += 3;
        }
        for (const std::string& nm : kept)
        {
            const int i = skeleton.find(nm);
            Variable  v;
            v.joint = i;
            v.col   = col;
            Vec3   axis;
            double natural = 1.0;
            if (hingeOf(nm, axis, natural))
            {
                v.kind    = Variable::Kind::Hinge;
                v.axis    = axis;
                v.natural = natural;
                // a hinge holds only its bend: drop any other part of the start rotation
                pb.start.local[i] = rotAxis(hingeAngle(pb.start.local[i], axis), axis);
            }
            else
            {
                v.kind = Variable::Kind::Rot3;
            }
            pb.varOf[i] = (int)pb.vars.size();
            pb.vars.push_back(v);
            col += v.size();
        }
        pb.n   = col;
        pb.ref = pb.start;
        pb.buildTerms();
    }

    Problem::~Problem() = default;

    int Problem::findTerm(const std::string& name) const
    {
        for (size_t i = 0; i < mImpl->terms.size(); ++i)
        {
            if (mImpl->terms[i]->type == "Anchor" && mImpl->terms[i]->name == name)
                return (int)i;
        }
        return -1;
    }

    bool Problem::setAnchorTarget(size_t term, const Vec3& position)
    {
        if (term >= mImpl->terms.size() || mImpl->terms[term]->type != "Anchor")
            return false;
        static_cast<AnchorTerm&>(*mImpl->terms[term]).target = position;
        return true;
    }

    int                          Problem::size() const { return mImpl->n; }
    const std::vector<Variable>& Problem::variables() const { return mImpl->vars; }
    const State&                 Problem::startState() const { return mImpl->start; }
    const State&                 Problem::measuredState() const { return mImpl->measured; }
    std::optional<double>        Problem::floor() const { return mImpl->floor; }
    size_t                       Problem::termCount() const { return mImpl->terms.size(); }
    std::string                  Problem::termName(size_t i) const { return mImpl->terms.at(i)->name; }
    std::string                  Problem::termType(size_t i) const { return mImpl->terms.at(i)->type; }
    std::string                  Problem::termKind(size_t i) const { return kindName(mImpl->terms.at(i)->kind); }
    double                       Problem::termTolerance(size_t i) const { return mImpl->terms.at(i)->tol; }

    void Problem::termEval(size_t i, const State& state, std::vector<double>& values, std::vector<double>* jac) const
    {
        const Ctx ctx(*mImpl, state);
        Jac       J;
        mImpl->terms.at(i)->eval(ctx, jac != nullptr, values, J);
        if (jac)
        {
            if (J.rows != (int)values.size())
                J.reset((int)values.size(), mImpl->n);
            *jac = J.a;
        }
    }

    std::vector<Anatomy> Problem::anatomyOf(int joint, const Mat3& local) const
    {
        const int vi = mImpl->varOf.at(joint);
        if (vi < 0)
            return {};
        return mImpl->rangeValues(joint, local, mImpl->vars[vi], nullptr);
    }

    // ------------------------------------------------------------------ evaluation and LM

    State Problem::Impl::step(const State& st, const double* dx) const
    {
        State s = st;
        for (const Variable& v : vars)
        {
            const double* d = dx + v.col;
            if (v.kind == Variable::Kind::Trans)
                s.root = s.root + Vec3(d[0], d[1], d[2]);
            else if (v.kind == Variable::Kind::Rot3)
                s.local[v.joint] = s.local[v.joint] * expRot(Vec3(d[0], d[1], d[2]));
            else
                s.local[v.joint] = s.local[v.joint] * rotAxis(d[0], v.axis);
        }
        return s;
    }

    State Problem::step(const State& state, const std::vector<double>& dx) const
    {
        if ((int)dx.size() != mImpl->n)
            throw SolveError("step: wrong size");
        return mImpl->step(state, dx.data());
    }

    // posesolver.Problem.evaluate: returns the number of residual rows
    int Problem::Impl::evaluate(const State& st, const AL& al, bool jac, std::vector<double>& r, Jac& J) const
    {
        const Ctx           ctx(*this, st);
        const double        mu = al.mu;
        const double        sq = std::sqrt(mu);
        r.clear();
        std::vector<double> rowsJ;
        std::vector<double> v;
        Jac                 Jt;
        for (size_t i = 0; i < terms.size(); ++i)
        {
            const Term& t = *terms[i];
            t.eval(ctx, jac, v, Jt);
            if (v.empty())
                continue;
            if (t.kind == Kind::Soft)
            {
                r.insert(r.end(), v.begin(), v.end());
                if (jac)
                    rowsJ.insert(rowsJ.end(), Jt.a.begin(), Jt.a.end());
                continue;
            }
            const std::vector<double>* lam = (i < al.lam.size() && al.lam[i] && al.lam[i]->size() == v.size()) ? &*al.lam[i] : nullptr;
            for (size_t k = 0; k < v.size(); ++k)
            {
                const double c = v[k] / t.tol;
                const double l = lam ? (*lam)[k] : 0.0;
                if (t.kind == Kind::Eq)
                {
                    r.push_back(sq * (c + l / mu));
                    if (jac)
                    {
                        const double* src = Jt.row((int)k);
                        for (int col = 0; col < n; ++col)
                            rowsJ.push_back(sq * src[col] / t.tol);
                    }
                }
                else
                {
                    const double a   = c + l / mu;
                    const bool   act = a > 0.0;
                    r.push_back(act ? sq * a : 0.0);
                    if (jac)
                    {
                        const double* src = Jt.row((int)k);
                        for (int col = 0; col < n; ++col)
                            rowsJ.push_back(act ? sq * src[col] / t.tol : 0.0);
                    }
                }
            }
        }
        if (jac)
        {
            J.rows = (int)r.size();
            J.cols = n;
            J.a    = std::move(rowsJ);
        }
        return (int)r.size();
    }

    double Problem::Impl::hardStatus(const State& st, std::vector<std::pair<int, std::vector<double>>>* cs) const
    {
        const Ctx           ctx(*this, st);
        double              worst = 0.0;
        std::vector<double> v;
        Jac                 J;
        if (cs)
            cs->clear();
        for (size_t i = 0; i < terms.size(); ++i)
        {
            const Term& t = *terms[i];
            if (t.kind == Kind::Soft)
                continue;
            t.eval(ctx, false, v, J);
            std::vector<double> c(v.size());
            for (size_t k = 0; k < v.size(); ++k)
                c[k] = v[k] / t.tol;
            double viol = 0.0;
            if (t.kind == Kind::Eq)
            {
                if (t.group)
                {
                    double s = 0.0;
                    for (double x : c)
                        s += x * x;
                    viol = std::sqrt(s);
                }
                else
                {
                    for (double x : c)
                        viol = std::max(viol, std::fabs(x));
                }
            }
            else
            {
                for (double x : c)
                    viol = std::max(viol, x);
            }
            // std::max passes a NaN over: a NaN residual is the worst violation there is.
            if (std::any_of(c.begin(), c.end(), [](double x) { return std::isnan(x); }))
                viol = std::numeric_limits<double>::infinity();
            worst = std::max(worst, viol);
            if (cs)
                cs->push_back({ (int)i, std::move(c) });
        }
        return worst;
    }

    double Problem::Impl::softCost(const State& st) const
    {
        const Ctx           ctx(*this, st);
        double              total = 0.0;
        std::vector<double> v;
        Jac                 J;
        for (const auto& t : terms)
        {
            if (t->kind != Kind::Soft)
                continue;
            t->eval(ctx, false, v, J);
            for (double x : v)
                total += x * x;
        }
        return total;
    }

    double Problem::hardViolation(const State& state) const { return mImpl->hardStatus(state, nullptr); }
    double Problem::softCost(const State& state) const { return mImpl->softCost(state); }

    namespace
    {
        // Solves M x = b for a symmetric positive definite M (n x n, row major). False when not positive definite.
        bool cholesky(std::vector<double>& M, int n, std::vector<double>& b)
        {
            for (int j = 0; j < n; ++j)
            {
                double d = M[(size_t)j * n + j];
                for (int k = 0; k < j; ++k)
                    d -= M[(size_t)j * n + k] * M[(size_t)j * n + k];
                if (!(d > 0.0) || !std::isfinite(d))
                    return false;
                d                     = std::sqrt(d);
                M[(size_t)j * n + j] = d;
                for (int i = j + 1; i < n; ++i)
                {
                    double s = M[(size_t)i * n + j];
                    for (int k = 0; k < j; ++k)
                        s -= M[(size_t)i * n + k] * M[(size_t)j * n + k];
                    M[(size_t)i * n + j] = s / d;
                }
            }
            for (int i = 0; i < n; ++i)
            {
                double s = b[i];
                for (int k = 0; k < i; ++k)
                    s -= M[(size_t)i * n + k] * b[k];
                b[i] = s / M[(size_t)i * n + i];
            }
            for (int i = n - 1; i >= 0; --i)
            {
                double s = b[i];
                for (int k = i + 1; k < n; ++k)
                    s -= M[(size_t)k * n + i] * b[k];
                b[i] = s / M[(size_t)i * n + i];
            }
            return true;
        }

        void checkCancel(const std::atomic<bool>* cancel)
        {
            if (cancel && cancel->load(std::memory_order_relaxed))
                throw SolveCancelled();
        }

        struct LMOut
        {
            State  state;
            double cost       = 0.0;
            int    iterations = 0;
        };

        // posesolver.lm
        LMOut levenbergMarquardt(const Problem::Impl& pb, State state, const Problem::Impl::AL& al, int maxIter,
                                 const std::atomic<bool>* cancel)
        {
            const int           n = pb.n;
            std::vector<double> r, rn;
            Jac                 J, Jn;
            int                 m = pb.evaluate(state, al, true, r, J);
            LMOut               out;
            double              cost = 0.0;
            for (double x : r)
                cost += x * x;
            if (m == 0 || n == 0)
            {
                out.state = state;
                out.cost  = cost;
                return out;
            }
            double              lam = 1e-3;
            int                 it  = 0;
            std::vector<double> A((size_t)n * n), g((size_t)n), D((size_t)n), M, dx;
            for (it = 1; it <= maxIter; ++it)
            {
                checkCancel(cancel);
                std::fill(A.begin(), A.end(), 0.0);
                std::fill(g.begin(), g.end(), 0.0);
                for (int row = 0; row < m; ++row)
                {
                    const double* jr = J.row(row);
                    for (int i = 0; i < n; ++i)
                    {
                        const double ji = jr[i];
                        if (ji == 0.0)
                            continue;
                        g[i] += ji * r[row];
                        double* Ai = A.data() + (size_t)i * n;
                        for (int k = 0; k <= i; ++k)
                            Ai[k] += ji * jr[k];
                    }
                }
                for (int i = 0; i < n; ++i)
                {
                    for (int k = 0; k < i; ++k)
                        A[(size_t)k * n + i] = A[(size_t)i * n + k];
                    D[i] = A[(size_t)i * n + i] + 1e-6;
                }
                bool   accepted = false;
                double rel      = 0.0;
                while (lam < 1e8)
                {
                    M = A;
                    for (int i = 0; i < n; ++i)
                        M[(size_t)i * n + i] += lam * D[i];
                    dx.resize((size_t)n);
                    for (int i = 0; i < n; ++i)
                        dx[i] = -g[i];
                    if (!cholesky(M, n, dx))
                    {
                        lam *= 10.0;
                        continue;
                    }
                    double big = 0.0;
                    for (double x : dx)
                        big = std::max(big, std::fabs(x));
                    if (big > 25.0 * RAD)
                    {
                        const double k = 25.0 * RAD / big;
                        for (double& x : dx)
                            x *= k;
                    }
                    const State cand = pb.step(state, dx.data());
                    pb.evaluate(cand, al, false, rn, Jn);
                    double cn = 0.0;
                    for (double x : rn)
                        cn += x * x;
                    if (cn < cost)
                    {
                        rel   = (cost - cn) / std::max(cost, 1e-12);
                        state = cand;
                        cost  = cn;
                        m     = pb.evaluate(state, al, true, r, J);
                        lam   = std::max(lam / 4.0, 1e-9);
                        accepted = true;
                        break;
                    }
                    lam *= 6.0;
                }
                if (!accepted)
                    break;
                double big = 0.0;
                for (double x : dx)
                    big = std::max(big, std::fabs(x));
                if (rel < 1e-7 || big < 0.01 * RAD)
                    break;
            }
            out.state      = state;
            out.cost       = cost;
            out.iterations = std::min(it, maxIter);
            return out;
        }
    }

    // posesolver.solve_from
    Run Problem::solveFrom(const State& start, const std::string& name, const std::atomic<bool>* cancel) const
    {
        const Impl& pb = *mImpl;
        Impl::AL    al;
        al.mu = 1.0;
        al.lam.assign(pb.terms.size(), std::nullopt);
        int    total = 0;
        double prev  = -1.0;
        Run    run;
        run.name   = name;
        run.status = "unmet";
        State state = start;
        bool  met   = false;
        std::vector<std::pair<int, std::vector<double>>> cs;
        for (int outer = 0; outer < pb.spec.options.maxOuter; ++outer)
        {
            LMOut o = levenbergMarquardt(pb, state, al, pb.spec.options.maxIter, cancel);
            state   = o.state;
            total += o.iterations;
            const double worst = pb.hardStatus(state, &cs);
            if (worst <= 1.0)
            {
                met = true;
                break;
            }
            for (auto& [i, c] : cs)
            {
                const Term&          t   = *pb.terms[i];
                std::vector<double>  lam = al.lam[i] && al.lam[i]->size() == c.size() ? *al.lam[i] : std::vector<double>(c.size(), 0.0);
                for (size_t k = 0; k < c.size(); ++k)
                {
                    lam[k] += al.mu * c[k];
                    if (t.kind == Kind::Ineq)
                        lam[k] = std::max(0.0, lam[k]);
                }
                al.lam[i] = std::move(lam);
            }
            if (prev >= 0.0 && worst > 0.25 * prev)
                al.mu = std::min(al.mu * 10.0, 1e5);
            prev = worst;
        }
        run.state      = state;
        run.iterations = total;
        run.violation  = pb.hardStatus(state, nullptr);
        run.status     = (met && run.violation <= 1.0) ? "converged" : "unmet";
        run.soft       = pb.softCost(state);
        return run;
    }

    // ------------------------------------------------------------------ starts

    namespace
    {
        const LimbDef* limbOfPoint(const Problem::Impl& pb, const Point& parts)
        {
            for (const LimbDef& l : limbDefs())
            {
                const int e = pb.sk.find(l.end);
                if (e < 0)
                    continue;
                bool all = !parts.empty();
                for (const Part& p : parts)
                {
                    if (!pb.inChain[p.anchor][e] && !pb.inChain[p.frame][e])
                        all = false;
                }
                if (all)
                    return &l;
            }
            return nullptr;
        }

        TwoBoneInput limbInput(const Problem::Impl& pb, const std::vector<Mat3>& W, const std::vector<Vec3>& P, const LimbDef& l)
        {
            const int    r = pb.sk.find(l.root), m = pb.sk.find(l.middle), e = pb.sk.find(l.end);
            TwoBoneInput in;
            in.root        = toLL(P[r]);
            in.middle      = toLL(P[m]);
            in.end         = toLL(P[e]);
            in.rootWorld   = toLLQuat(W[r]);
            in.middleWorld = toLLQuat(W[m]);
            const int p    = pb.sk.parent[r];
            in.parentWorld = toLLQuat(p >= 0 ? W[p] : Mat3::identity());
            in.hinge       = toLL(l.hinge);
            in.natural     = (F32)l.natural;
            return in;
        }

        // posesolver.aim_start
        std::optional<State> aimStart(const Problem::Impl& pb, const State& state)
        {
            std::vector<const BoneDirTerm*> goals;
            for (const auto& t : pb.terms)
            {
                if (t->type == "BoneDir")
                    goals.push_back(static_cast<const BoneDirTerm*>(t.get()));
            }
            if (goals.empty())
                return std::nullopt;
            std::vector<Mat3> W;
            std::vector<Vec3> P;
            forward(pb.sk, state, W, P);
            std::vector<int>    names;
            std::map<int, int>  index;
            for (const BoneDirTerm* t : goals)
            {
                std::vector<int> chain;
                for (int k = t->j; k >= 0 && !index.count(k); k = pb.sk.parent[k])
                    chain.push_back(k);
                for (auto it = chain.rbegin(); it != chain.rend(); ++it)
                {
                    index[*it] = (int)names.size();
                    names.push_back(*it);
                }
            }
            std::map<std::string, const BoneDirTerm*> byJoint;
            for (const BoneDirTerm* t : goals)
                byJoint[pb.sk.names[t->j]] = t;
            std::vector<AimJoint> joints;
            for (int k : names)
            {
                const int p = pb.sk.parent[k];
                AimJoint  aj;
                aj.parent      = p >= 0 ? index.at(p) : -1;
                aj.local       = toLLQuat(state.local[k]);
                aj.parentWorld = toLLQuat(p >= 0 ? W[p] : Mat3::identity());
                joints.push_back(aj);
            }
            std::vector<AimBone> bones;
            for (const BoneDirTerm* t : goals)
            {
                const std::string& jn = pb.sk.names[t->j];
                AimBone            b;
                b.joint     = index.at(t->j);
                b.bone      = toLL((P[t->end] - P[t->j]) * transpose(W[t->j]));
                b.direction = toLL(t->d);
                b.twist     = AimTwist::Auto;
                b.roll      = t->roll.has_value();
                b.rollToward = t->roll ? toLL(*t->roll) : LLVector3::zero;
                b.side      = toLL(sideAxis(jn));
                for (const LimbDef& l : limbDefs())
                {
                    if (jn == l.root && pb.sk.names[t->end] == l.middle)
                    {
                        const int m = pb.sk.find(l.middle), e = pb.sk.find(l.end);
                        if (m >= 0 && e >= 0)
                        {
                            b.limb         = true;
                            b.childBone    = toLL((P[e] - P[m]) * transpose(W[m]));
                            b.childHinge   = toLL(l.hinge);
                            b.childNatural = (F32)l.natural;
                            auto child     = byJoint.find(l.middle);
                            if (!b.roll && child != byJoint.end())
                            {
                                b.roll       = true;
                                b.rollToward = toLL(child->second->d);
                            }
                        }
                    }
                    if (jn == l.middle && pb.sk.names[t->end] == l.end)
                    {
                        b.hinge     = true;
                        b.hingeAxis = toLL(l.hinge);
                        b.natural   = (F32)l.natural;
                    }
                }
                bones.push_back(b);
            }
            std::vector<AimResult> results;
            std::string            why;
            int                    bad = -1;
            if (!solveAim(joints, bones, results, why, bad))
                return std::nullopt;
            State s = state;
            for (size_t i = 0; i < goals.size(); ++i)
            {
                const int j = goals[i]->j;
                s.local[j]  = fromLL(results[i].local);
                const int vi = pb.varOf[j];
                if (vi >= 0 && pb.vars[vi].kind == Variable::Kind::Hinge)
                    s.local[j] = rotAxis(hingeAngle(s.local[j], pb.vars[vi].axis), pb.vars[vi].axis);
            }
            return s;
        }

        // posesolver.held_limbs
        std::set<const LimbDef*> heldLimbs(const Problem::Impl& pb)
        {
            std::set<const LimbDef*> out;
            for (const auto& t : pb.terms)
            {
                const Point* pt = nullptr;
                if (t->type == "Anchor")
                    pt = &static_cast<const AnchorTerm*>(t.get())->pt;
                else if (t->type == "OnFloor")
                    pt = &static_cast<const OnFloorTerm*>(t.get())->pt;
                if (pt)
                {
                    if (const LimbDef* l = limbOfPoint(pb, *pt))
                        out.insert(l);
                }
            }
            return out;
        }

        // posesolver.contact_start
        std::optional<State> contactStart(const Problem::Impl& pb, const State& state, bool flip)
        {
            std::vector<const ContactTerm*> contacts;
            for (const auto& t : pb.terms)
            {
                if (t->type == "Contact")
                    contacts.push_back(static_cast<const ContactTerm*>(t.get()));
            }
            if (contacts.empty())
                return std::nullopt;
            State                    s     = state;
            std::set<const LimbDef*> held  = heldLimbs(pb);
            std::set<const LimbDef*> moved;
            for (const ContactTerm* t : contacts)
            {
                std::vector<Mat3> W;
                std::vector<Vec3> P;
                forward(pb.sk, s, W, P);
                const Ctx ctx(pb, s);
                struct Side
                {
                    Vec3           point;
                    const LimbDef* limb = nullptr;
                    ContactSide    cs;
                    TwoBoneInput   input;
                    Vec3           offsetWorld;
                };
                Side sides[2];
                const Point* parts[2] = { &t->a, &t->b };
                for (int k = 0; k < 2; ++k)
                {
                    Side& sd   = sides[k];
                    sd.point   = ctx.point(*parts[k], nullptr);
                    sd.limb    = limbOfPoint(pb, *parts[k]);
                    sd.cs.point = toLL(sd.point);
                    if (!sd.limb)
                        continue;
                    const int r = pb.sk.find(sd.limb->root), m = pb.sk.find(sd.limb->middle), e = pb.sk.find(sd.limb->end);
                    sd.cs.movable = !moved.count(sd.limb) && r >= 0 && m >= 0 && pb.varOf[r] >= 0 && pb.varOf[m] >= 0;
                    sd.cs.arm     = sd.limb->arm;
                    sd.cs.support = held.count(sd.limb) > 0;
                    sd.input      = limbInput(pb, W, P, *sd.limb);
                    sd.offsetWorld = sd.point - P[e];
                    F32         lo = 0.f, hi = 0.f;
                    const char* why = "";
                    if (!reachRange(sd.input, lo, hi, why))
                    {
                        sd.cs.movable = false;
                    }
                    else
                    {
                        sd.cs.shell.centre   = sd.input.root + toLL(sd.offsetWorld);
                        sd.cs.shell.minReach = lo;
                        sd.cs.shell.maxReach = hi;
                    }
                }
                const char*       note = "";
                const ContactMode mode = resolveAuto(sides[0].cs, sides[1].cs, 0.92f, 0.005f, note);
                if (mode == ContactMode::None)
                    continue;
                std::vector<int> movers;
                LLVector3        goals[2];
                if (mode == ContactMode::Both)
                {
                    movers                = { 0, 1 };
                    const Meeting meeting = chooseMeeting(ContactMode::Both, sides[0].cs.point, sides[1].cs.point, sides[0].cs.shell,
                                                          sides[1].cs.shell, 0.92f);
                    goals[0] = meeting.goalA;
                    goals[1] = meeting.goalB;
                }
                else
                {
                    const int k = mode == ContactMode::A ? 0 : 1;
                    movers      = { k };
                    goals[k]    = sides[1 - k].cs.point;
                }
                for (int k : movers)
                {
                    Side&        sd = sides[k];
                    TwoBoneInput in = sd.input;
                    LLVector3    pole = autoPole(in, toLL(sd.limb->pole));
                    if (flip)
                        pole = -pole;
                    in.pole = pole;
                    LimbGoal    best;
                    const char* why = "";
                    if (!solveLimbToGoal(in, toLL(sd.offsetWorld), goals[k], best, why))
                        continue;
                    const int r = pb.sk.find(sd.limb->root), m = pb.sk.find(sd.limb->middle);
                    s.local[r]  = fromLL(best.solved.rootLocal);
                    s.local[m]  = fromLL(best.solved.middleLocal);
                    moved.insert(sd.limb);
                }
            }
            return s;
        }

        State jitter(const Problem::Impl& pb, const State& state, std::mt19937& rng, double deg = 8.0)
        {
            State                            s = state;
            std::normal_distribution<double> rot(0.0, deg * RAD), hinge(0.0, 1.5 * deg * RAD);
            for (const Variable& v : pb.vars)
            {
                if (v.kind == Variable::Kind::Rot3)
                {
                    const double a = rot(rng), b = rot(rng), c = rot(rng);
                    s.local[v.joint] = s.local[v.joint] * expRot(Vec3(a, b, c));
                }
                else if (v.kind == Variable::Kind::Hinge)
                {
                    double th = hingeAngle(s.local[v.joint], v.axis) + hinge(rng);
                    if (th * v.natural < 0.0)
                        th = 0.0;
                    s.local[v.joint] = rotAxis(th, v.axis);
                }
            }
            return s;
        }
    }

    std::vector<std::pair<std::string, State>> Problem::starts(int n, unsigned seed) const
    {
        const Impl&                                pb = *mImpl;
        std::vector<std::pair<std::string, State>> out{ { "start", pb.start } };
        std::optional<State>                       a = aimStart(pb, pb.start);
        if (a)
            out.push_back({ "aim", *a });
        const State& base = a ? *a : pb.start;
        if (std::optional<State> c = contactStart(pb, base, false))
        {
            out.push_back({ "contact", *c });
            if (std::optional<State> c2 = contactStart(pb, base, true))
                out.push_back({ "contact_flipped", *c2 });
        }
        std::mt19937 rng(seed);
        for (int k = 0; (int)out.size() < n; ++k)
            out.push_back({ "jitter" + std::to_string(k), jitter(pb, (k % 2) ? base : pb.start, rng) });
        out.resize((size_t)std::max(1, std::min(n, (int)out.size())));
        return out;
    }

    // ------------------------------------------------------------------ the report

    namespace
    {
        double jointChangeDeg(const Mat3& a, const Mat3& b) { return norm(logRot(transpose(a) * b)) * DEG; }
    }

    Result Problem::report(const Run& best, const std::vector<Run>& runs, const std::vector<std::pair<const Run*, double>>& alternatives) const
    {
        const Impl& pb = *mImpl;
        Result      out;
        const State& st = best.state;
        std::vector<std::pair<int, std::vector<double>>> cs;
        pb.hardStatus(st, &cs);
        std::map<int, double> viol;
        for (const auto& [i, c] : cs)
        {
            const Term& t = *pb.terms[i];
            double      v = 0.0;
            if (t.kind == Kind::Eq)
            {
                if (t.group)
                {
                    double s = 0.0;
                    for (double x : c)
                        s += x * x;
                    v = std::sqrt(s);
                }
                else
                {
                    for (double x : c)
                        v = std::max(v, std::fabs(x));
                }
            }
            else
            {
                for (double x : c)
                    v = std::max(v, x);
            }
            // As hardStatus: a NaN residual counts as an infinite violation (std::max passes it over).
            if (std::any_of(c.begin(), c.end(), [](double x) { return std::isnan(x); }))
                v = std::numeric_limits<double>::infinity();
            viol[i] = v;
        }
        const Ctx ctx(pb, st);
        for (size_t i = 0; i < pb.terms.size(); ++i)
        {
            const Term& t = *pb.terms[i];
            if (t.type == "Stay" || t.type == "Smooth" || t.type == "Comfort")
                continue;
            GoalReport g;
            g.name = t.name;
            g.kind = kindName(t.kind);
            g.unit = t.unit;
            if (t.type != "Range")
                t.describe(ctx, g);
            if (t.kind != Kind::Soft)
            {
                g.tolerance = roundTo(t.tol * (t.unit == "mm" ? 10.0 : 1.0), 3);
                g.met       = viol[(int)i] <= 1.0;
                if (t.type == "Range")
                {
                    const RangeTerm&     rt   = static_cast<const RangeTerm&>(t);
                    std::vector<Anatomy> vals = rt.values(ctx);
                    std::vector<Anatomy> edge;
                    for (const auto& [m, lim] : rt.limits)
                    {
                        for (const Anatomy& a : vals)
                        {
                            if (a.motion != m)
                                continue;
                            if ((lim.first && a.value <= *lim.first + 0.6) || (lim.second && a.value >= *lim.second - 0.6))
                                edge.push_back(a);
                        }
                    }
                    if (!edge.empty())
                        out.rangeHits.push_back(Result::RangeHit{ pb.sk.names[rt.j], edge });
                    out.anatomy.push_back({ pb.sk.names[rt.j], vals });
                    if (!*g.met)
                        out.unmet.push_back(Result::Unmet{ t.name, 0.0, t.unit, 0.0, vals });
                    continue;
                }
                if (!*g.met)
                {
                    out.unmet.push_back(Result::Unmet{ t.name, g.value, t.unit, *g.tolerance, {} });
                    out.warnings.push_back("RED: " + t.name + " is " + pyNum(g.value) + " " + t.unit + " (tolerance " +
                                           pyNum(*g.tolerance) + "): kept as a goal, not met");
                }
            }
            out.goals.push_back(std::move(g));
        }
        for (const Variable& v : pb.vars)
        {
            if (v.kind == Variable::Kind::Trans)
                continue;
            Result::Joint j;
            j.name      = pb.sk.names[v.joint];
            j.local     = st.local[v.joint];
            j.hinge     = v.kind == Variable::Kind::Hinge;
            j.changeDeg = roundTo(jointChangeDeg(pb.measured.local[v.joint], j.local), 2);
            j.angleDeg  = roundTo(norm(logRot(j.local)) * DEG, 1);
            auto                  rest = pb.restDir.find(v.joint);
            std::optional<double> tw;
            if (v.kind == Variable::Kind::Rot3 && rest != pb.restDir.end())
            {
                const Quat q = quatOf(j.local);
                tw = std::fabs(2.0 * std::atan2(q.x * rest->second.x + q.y * rest->second.y + q.z * rest->second.z, q.w)) * DEG;
                j.twistDeg = roundTo(*tw, 1);
            }
            if (j.name != "mPelvis" && j.angleDeg > MAX_ROTATION_DEG + 0.5 && v.kind == Variable::Kind::Rot3)
                out.warnings.push_back(j.name + " turns " + fmt("%.0f", j.angleDeg) + " deg (> " + fmt("%.0f", MAX_ROTATION_DEG) + ")");
            if (tw && *tw > 90.0)
                out.warnings.push_back(j.name + " twists " + fmt("%.0f", *tw) + " deg about its bone");
            out.joints.push_back(j);
        }
        out.status        = best.status;
        out.start         = best.name;
        out.iterations    = best.iterations;
        out.softCost      = roundTo(best.soft, 3);
        out.hardViolation = roundTo(best.violation, 3);
        out.pelvisFree    = pb.trans >= 0;
        out.pelvisPosition = pb.sk.pelvisOffset + (st.root - pb.measured.root);
        out.pelvisMovedCm  = (st.root - pb.measured.root) * CM;
        out.floorZ         = pb.floor;
        for (const Run& r : runs)
            out.starts.push_back(Result::StartSummary{ r.name, r.status, r.soft, r.violation, r.iterations });
        for (const auto& [run, differs] : alternatives)
        {
            Result::Alternative a;
            a.start      = run->name;
            a.soft       = run->soft;
            a.differsDeg = differs;
            for (const Variable& v : pb.vars)
            {
                if (v.kind != Variable::Kind::Trans)
                    a.joints.push_back({ pb.sk.names[v.joint], run->state.local[v.joint] });
            }
            out.alternatives.push_back(std::move(a));
        }
        std::vector<Mat3> W;
        std::vector<Vec3> P;
        forward(pb.sk, st, W, P);
        std::set<std::string> wanted(bodyList().begin(), bodyList().end());
        for (const auto& kv : boneEnds())
            wanted.insert(kv.second);
        for (const char* v : { "L_HAND", "R_HAND", "L_FOOT", "R_FOOT" })
            wanted.insert(v);
        for (size_t i = 0; i < pb.sk.names.size(); ++i)
        {
            if (wanted.count(pb.sk.names[i]))
                out.predicted.push_back({ pb.sk.names[i], P[i] });
        }
        out.state  = st;
        out.frozen = frozen();
        return out;
    }

    Spec Problem::frozen() const
    {
        const Impl& pb   = *mImpl;
        Spec        spec = pb.spec;
        std::vector<const AnchorTerm*> anchors;
        std::vector<const OrientTerm*> orients;
        for (const auto& t : pb.terms)
        {
            if (t->type == "Anchor")
                anchors.push_back(static_cast<const AnchorTerm*>(t.get()));
            else if (t->type == "Orient")
                orients.push_back(static_cast<const OrientTerm*>(t.get()));
        }
        size_t ai = 0, oi = 0;
        for (Goal& g : spec.goals)
        {
            if (g.type != GoalType::Anchor)
                continue;
            if (ai < anchors.size())
                g.position = anchors[ai++]->target;
            if (g.orient && oi < orients.size())
                g.orientTarget = orients[oi++]->target;
        }
        if (pb.floor)
        {
            spec.floorMode = "value";
            spec.floorZ    = *pb.floor;
        }
        return spec;
    }

    // ------------------------------------------------------------------ solve

    Result solve(const Skeleton& skeleton, const Spec& spec, const std::atomic<bool>* cancel)
    {
        const Problem    pb(skeleton, spec);
        std::vector<Run> runs;
        for (const auto& [name, s0] : pb.starts(spec.options.starts, spec.options.seed))
        {
            checkCancel(cancel);
            runs.push_back(pb.solveFrom(s0, name, cancel));
        }
        if (runs.empty())
            throw SolveError("no start to solve from");
        std::vector<const Run*> met, all;
        for (const Run& r : runs)
        {
            all.push_back(&r);
            if (r.violation <= 1.0)
                met.push_back(&r);
        }
        // the hard goals met, then the weak cost; else the least violation
        const std::vector<const Run*>& pool  = met.empty() ? all : met;
        auto                           score = [&](const Run* r) { return met.empty() ? r->violation * 1e6 + r->soft : r->soft; };
        const Run*                     best  = pool.front();
        for (const Run* r : pool)
        {
            if (score(r) < score(best))
                best = r;
        }
        // clearly different poses of similar cost
        auto distance = [&](const State& a, const State& b)
        {
            double d = 0.0;
            for (const Variable& v : pb.variables())
            {
                if (v.kind != Variable::Kind::Trans)
                    d = std::max(d, jointChangeDeg(a.local[v.joint], b.local[v.joint]));
            }
            return d;
        };
        std::vector<const Run*> sorted = met;
        std::stable_sort(sorted.begin(), sorted.end(), [](const Run* a, const Run* b) { return a->soft < b->soft; });
        std::vector<std::pair<const Run*, double>> alts;
        for (const Run* r : sorted)
        {
            if (r == best || r->soft > best->soft * 1.25 + 2.0)
                continue;
            const double d  = distance(best->state, r->state);
            bool         ok = d > 25.0;
            for (const auto& a : alts)
            {
                if (distance(a.first->state, r->state) <= 25.0)
                    ok = false;
            }
            if (ok)
                alts.push_back({ r, roundTo(d, 1) });
            if (alts.size() >= 2)
                break;
        }
        return pb.report(*best, runs, alts);
    }

    std::vector<std::string> neededJoints(const Spec& spec)
    {
        std::set<std::string> keep(bodyList().begin(), bodyList().end());
        for (const auto& kv : boneEnds())
            keep.insert(kv.second);
        keep.insert("mHandMiddle3Left");
        keep.insert("mHandMiddle3Right");
        const SiteTable sites = mergeSites(defaultSites(), spec.sitesOverride);
        for (const auto& [name, sides] : sites)
        {
            for (const auto& [side, e] : sides)
            {
                keep.insert(e.anchor);
                keep.insert(e.frame);
            }
        }
        std::function<void(const PointSpec&)> point = [&](const PointSpec& p)
        {
            if (!p.joint.empty())
                keep.insert(p.joint);
            for (const PointSpec& m : p.mid)
                point(m);
        };
        for (const Goal& g : spec.goals)
        {
            for (const std::string* s : { &g.joint, &g.toward, &g.axisJoint })
            {
                if (!s->empty())
                    keep.insert(*s);
            }
            for (const PointSpec* p : { &g.a, &g.b, &g.at, &g.from, &g.to })
                point(*p);
            for (const PointSpec& p : g.atList)
                point(p);
            for (const PointSpec& p : g.support)
                point(p);
        }
        for (const std::string& f : spec.free)
            keep.insert(f);
        keep.erase(std::string());
        return std::vector<std::string>(keep.begin(), keep.end());
    }
}

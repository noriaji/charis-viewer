/**
 * @file posesolver.h
 * @brief solve_pose for the poser MCP: every body joint at once from goals
 *        (bone directions, contacts, the floor, ranges ...). The maths only, no
 *        viewer state.
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

// The C++ twin of tools/poser-mcp/posesolver.py in the sl-viewer repository
// (research/poser-solver-design.md sections 2-3). Only needs llmath and the other
// solvers here (aimsolver, contactsolver, reachsolver), so it is built and tested
// on its own (tools/poser-mcp/ik/posesolver_test.cpp) against cases the Python
// version writes (posesolver_test.py --dump).
//
// Variables: a local rotation vector per 3-dof joint (each step multiplies the
// current rotation by a small one: L <- L * exp(d), d in the parent's axes, so no
// Euler angles and no gimbal lock), one angle per hinge (elbows about Z, knees
// about Y: they cannot twist), and the pelvis position. The Jacobian is analytic:
// turning joint j about the world axis a moves a point p by a x (p - p_j) and a
// direction u by a x u. Levenberg-Marquardt on the weighted residuals; hard goals
// (contacts, the floor, anchors, ranges) through an augmented Lagrangian down to
// their tolerance. When a hard goal cannot be met the result says "unmet" and
// keeps the change (the caller applies it and warns). Several starts: the start
// pose, aim's closed form (aimsolver), the contact limbs placed by contactsolver
// (and with the elbow / knee flipped), jittered copies.
//
// Units of the residuals: 1 cm = 1 degree = 1, times the weight. Solved in double;
// the viewer's F32 types only at the edges (fromLL / toLL).
//
// Conventions of the viewer: row vectors (v' = v * M), world = local * parent, the
// body frame +X forward, +Y the avatar's left, +Z up, meters.

#ifndef PLUS_POSERMCP_POSESOLVER_H
#define PLUS_POSERMCP_POSESOLVER_H

#include "llmath.h" // must come first (llquaternion.h insists)
#include "llquaternion.h"
#include "v3math.h"

#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace plus::posermcp::solver
{
    // ---------------------------------------------------------------- vectors and rotations (double)

    struct Vec3
    {
        double x = 0.0, y = 0.0, z = 0.0;

        Vec3() = default;
        constexpr Vec3(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}

        double  operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }
        double& operator[](int i) { return i == 0 ? x : (i == 1 ? y : z); }
    };

    inline Vec3   operator+(const Vec3& a, const Vec3& b) { return Vec3(a.x + b.x, a.y + b.y, a.z + b.z); }
    inline Vec3   operator-(const Vec3& a, const Vec3& b) { return Vec3(a.x - b.x, a.y - b.y, a.z - b.z); }
    inline Vec3   operator-(const Vec3& a) { return Vec3(-a.x, -a.y, -a.z); }
    inline Vec3   operator*(const Vec3& a, double s) { return Vec3(a.x * s, a.y * s, a.z * s); }
    inline Vec3   operator*(double s, const Vec3& a) { return Vec3(a.x * s, a.y * s, a.z * s); }
    inline Vec3   operator/(const Vec3& a, double s) { return Vec3(a.x / s, a.y / s, a.z / s); }
    inline Vec3&  operator+=(Vec3& a, const Vec3& b) { a.x += b.x; a.y += b.y; a.z += b.z; return a; }
    inline double dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    inline Vec3   cross(const Vec3& a, const Vec3& b)
    {
        return Vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
    }
    double norm(const Vec3& a);
    Vec3   unit(const Vec3& a); // a / |a|; a itself when |a| <= 1e-12 (as ik_check.unit)

    // A rotation for row vectors: v' = v * M (the transpose of the usual column form).
    struct Mat3
    {
        double m[3][3] = { { 1.0, 0.0, 0.0 }, { 0.0, 1.0, 0.0 }, { 0.0, 0.0, 1.0 } };

        static Mat3 identity() { return Mat3(); }
        Vec3        row(int i) const { return Vec3(m[i][0], m[i][1], m[i][2]); }
    };

    Vec3   operator*(const Vec3& v, const Mat3& r);
    Mat3   operator*(const Mat3& a, const Mat3& b);
    Mat3   transpose(const Mat3& r);
    Mat3   rotAxis(double angle, const Vec3& axis);  // turns by angle (rad) about axis, right-handed
    Mat3   expRot(const Vec3& w);                    // rotation vector -> rotation
    Vec3   logRot(const Mat3& r);                    // rotation -> rotation vector (precise near 0 and pi)
    double rotationAngle(const Mat3& r);             // 0..pi
    double angleBetween(const Vec3& a, const Vec3& b);
    Mat3   swingRotation(const Vec3& a, const Vec3& b); // smallest turn of unit a onto unit b (as aimsolver)

    struct Quat
    {
        double w = 1.0, x = 0.0, y = 0.0, z = 0.0;
    };
    Quat   quatOf(const Mat3& r);                    // w >= 0
    double hingeAngle(const Mat3& r, const Vec3& axis); // signed angle of the part of r about a coordinate axis

    // set_joints' Euler angles (degrees; the poser's LLMatrix3(roll, pitch, yaw)).
    Vec3 eulerDegrees(const Mat3& r);
    Mat3 eulerToRot(const Vec3& degrees);

    // The viewer's types.
    Mat3         fromLL(const LLQuaternion& q);
    Vec3         fromLL(const LLVector3& v);
    LLQuaternion toLLQuat(const Mat3& r);
    LLVector3    toLL(const Vec3& v);

    // ---------------------------------------------------------------- the skeleton as drawn

    // Every joint the goals use and their ancestors, parents before children, as
    // drawn now: offsets from the parent in the parent's frame (shape, mesh joint
    // position overrides and scale included), local rotations, the root (mPelvis)
    // position in the body frame. The viewer reads it from LLJoint; posemodel.py
    // works it out from joint_positions + get_pose.
    struct Skeleton
    {
        std::vector<std::string> names;
        std::vector<int>         parent;       // -1 for the root (its parent frame is the body frame)
        std::vector<Vec3>        offset;       // meters, in the parent's frame (the root's is ignored)
        Vec3                     rootPos;      // the root joint in the body frame
        std::vector<Mat3>        local;        // local rotations as drawn
        Vec3                     pelvisOffset; // the poser's mPelvis position offset (get_pose "position")

        int  find(const std::string& name) const; // exact, then case-insensitive; -1 when missing
        bool valid(std::string& why) const;       // sizes agree, parents before children
    };

    // The joints a solve may turn (3 = rotation vector; elbows / knees are hinges).
    const std::vector<std::string>& bodyJoints();
    // The joint a bone runs to (aim / joint_positions), "" for others.
    std::string boneEnd(const std::string& joint);
    // The reference side axis of a joint (aimSideAxis in posertools_aim.cpp).
    Vec3 sideAxis(const std::string& joint);
    // "Left", "Right" or "".
    std::string sideOf(const std::string& joint);

    // ---------------------------------------------------------------- goals (the solve_pose JSON)

    // A point on the body: a joint centre, a site (sites.json) on a joint's side, an
    // offset (m, the joint's axes) from a joint, or the mean of several points.
    struct PointSpec
    {
        std::string            joint;
        std::string            site;
        std::optional<Vec3>    offset;
        std::optional<Vec3>    normal; // a skin normal for facing / flat when there is no site
        std::vector<PointSpec> mid;
    };

    // A pinhole camera looking at focus from azimuth / elevation (deg; azimuth from
    // the body front, + toward the avatar's left), as snapshot views and pose_error.
    struct CameraSpec
    {
        double                azimuth   = 0.0;
        double                elevation = 0.0;
        double                roll      = 0.0;
        std::optional<double> fov;      // vertical, degrees
        std::optional<double> distance; // meters
        Vec3                  focus;
        double                width  = 512.0;
        double                height = 512.0;
        std::optional<Vec3>   center; // principal point (x, y px; z unused)
    };

    enum class GoalType
    {
        BoneDir,       // a bone's direction (weak), or within within_deg of it (hard)
        Contact,       // two points touch (or keep distance), hard unless hard=false
        OnFloor,       // a point on the floor (+ height); flat: its site's normal straight down
        AboveFloor,    // points that must not go below the floor
        Anchor,        // a point kept where it is (or at position); orient: also its joint's turn
        JointRange,    // an anatomical range overriding the default one
        BoneElevation, // a bone's angle above the horizontal within [min, max]
        TorsoLean,     // the hips -> shoulders line: [min, max] from vertical (hard) or forward / side targets
        Balance,       // the centre of mass over the support soles
        Look,          // where the head's front points
        ImageDir,      // a direction seen in a photo (2D, orthographic camera)
        ImagePoint,    // a point on a photo pixel (perspective camera)
        SmoothSpine    // accepted and ignored (the spine smoothing is always on)
    };

    struct Goal
    {
        GoalType    type = GoalType::BoneDir;
        std::string name;   // "" = a name made from the goal
        std::string label;  // image_dir

        // bone_dir, joint_range, bone_elevation
        std::string           joint;
        std::string           toward;    // bone_dir: another child joint as the bone's end
        Vec3                  direction;
        std::optional<Vec3>   rollToward;
        std::optional<double> rollWeight;
        std::optional<double> withinDeg;
        std::optional<double> toleranceDeg;
        std::optional<double> weight;

        // contact (a, b), on_floor / anchor (at), above_floor (atList), balance (support)
        PointSpec              a, b, at;
        std::vector<PointSpec> atList;
        std::vector<PointSpec> support;
        std::string            supportSite = "sole";
        double                 distance    = 0.0;
        double                 skinMargin  = 0.0;
        std::optional<double>  tolerance; // meters for points
        bool                   hard         = true;
        bool                   facing       = false;
        double                 facingWeight = 0.2;
        bool                   flat             = false;
        double                 flatToleranceDeg = 2.0;
        std::optional<double>  floorZ;  // on_floor: this floor instead of the spec's
        double                 height = 0.0;
        std::optional<Vec3>    position; // anchor: body frame; none = where it is now
        bool                   orient = false;
        std::optional<Mat3>    orientTarget;
        double                 orientToleranceDeg = 1.0;

        // joint_range (motion), bone_elevation and torso_lean (min / max), torso_lean targets
        std::string           motion;
        std::optional<double> min;
        std::optional<double> max;
        std::optional<double> forwardDeg;
        std::optional<double> sideDeg;

        // look
        Vec3 lookToward;

        // image_dir: from / to points, or a joint's own axis
        PointSpec                 from, to;
        std::string               axisJoint;
        Vec3                      axisLocal;
        double                    photoVec[2] = { 0.0, 0.0 };
        std::optional<CameraSpec> camera;

        // image_point
        double                px[2] = { 0.0, 0.0 };
        std::optional<double> mPerPx;
    };

    struct SiteEntry
    {
        std::string         anchor; // a joint or collision volume the point hangs from
        std::string         frame;  // whose axes the offset is in
        Vec3                offset;
        std::optional<Vec3> normal;
        std::optional<Vec3> along;
    };
    // site name -> side ("Left", "Right" or "*") -> entry
    using SiteTable = std::map<std::string, std::map<std::string, SiteEntry>>;
    const SiteTable& defaultSites(); // tools/poser-mcp/sites.json (posesolvertables.h)

    // sites_override: site -> side -> the fields to replace (a mesh body's feet ...).
    struct SiteOverride
    {
        std::optional<std::string> anchor, frame;
        std::optional<Vec3>        offset, normal, along;
    };
    using SiteOverrides = std::map<std::string, std::map<std::string, SiteOverride>>;

    struct Options
    {
        std::string                   flexibility = "normal"; // normal | athletic (x1.15) | dancer (hips, spine, shoulders x1.3)
        int                           starts      = 6;
        unsigned                      seed        = 1;
        int                           maxOuter    = 6;   // augmented Lagrangian rounds
        int                           maxIter     = 60;  // LM iterations per round
        double                        stay        = 1.0; // scales the default stay weights
        std::map<std::string, double> stayWeight;        // joint name or body part (arm, leg, trunk, pelvis, head, trans)
        double                        comfort        = 0.05;
        double                        spineSmooth    = 0.3;
        double                        maxRotationDeg = 120.0;
        bool                          ranges         = true;
        bool                          floorGuard     = true;
    };

    struct Spec
    {
        std::string              start = "current"; // current | tpose
        bool                     freeAuto = true;   // the body joints, else `free`
        std::vector<std::string> free;
        std::vector<std::string> lock;              // "mPelvis.position" keeps the pelvis in place
        bool                     lockPosition = false;
        std::string              floorMode = "rest"; // rest (T-pose soles) | lowest (sole now) | value
        double                   floorZ    = 0.0;    // floorMode "value"
        std::vector<Goal>        goals;
        Options                  options;
        CameraSpec               camera;
        SiteOverrides            sitesOverride;
    };

    // ---------------------------------------------------------------- solving

    struct State
    {
        std::vector<Mat3> local;
        Vec3              root;
    };

    struct Variable
    {
        enum class Kind
        {
            Rot3,
            Hinge,
            Trans
        };
        int    joint = -1;
        Kind   kind  = Kind::Rot3;
        int    col   = 0;
        Vec3   axis; // hinge
        double natural = 1.0;
        int    size() const { return kind == Kind::Hinge ? 1 : 3; }
    };

    // A spec that cannot be solved (unknown joint or site, bad values).
    struct SolveError : public std::runtime_error
    {
        using std::runtime_error::runtime_error;
    };
    // The caller's cancel flag went up.
    struct SolveCancelled : public std::runtime_error
    {
        SolveCancelled() : std::runtime_error("cancelled") {}
    };

    struct Run
    {
        std::string name;
        State       state;
        std::string status; // converged | unmet
        int         iterations = 0;
        double      violation  = 0.0; // the worst hard goal / its tolerance (met when <= 1)
        double      soft       = 0.0; // the weak goals' cost
    };

    // An anatomical value of a joint (degrees) and its limits.
    struct Anatomy
    {
        std::string           motion;
        double                value = 0.0;
        std::optional<double> lo, hi;
    };

    struct GoalReport
    {
        struct Extra
        {
            std::string         key;
            std::vector<double> values; // NaN = null
            bool                list = false;
        };
        std::string           name, kind, unit; // kind: soft | eq | ineq
        double                value = 0.0;      // as posesolver.py's describe() (rounded likewise)
        std::optional<double> tolerance;        // hard goals, in the unit
        std::optional<bool>   met;
        std::vector<Extra>    extras;
    };

    struct Result
    {
        struct Unmet
        {
            std::string          name;
            double               value = 0.0;
            std::string          unit;
            double               tolerance = 0.0;
            std::vector<Anatomy> range; // a range goal: the values and limits
        };
        struct RangeHit
        {
            std::string          joint;
            std::vector<Anatomy> motions; // at (within 0.6 degrees of) a limit
        };
        struct Joint
        {
            std::string           name;
            Mat3                  local;
            bool                  hinge     = false;
            double                changeDeg = 0.0; // from the pose before the solve
            double                angleDeg  = 0.0; // from the rest rotation
            std::optional<double> twistDeg;        // about its own bone
        };
        struct StartSummary
        {
            std::string name, status;
            double      soft = 0.0, violation = 0.0;
            int         iterations = 0;
        };
        struct Alternative
        {
            std::string                               start;
            double                                    soft = 0.0, differsDeg = 0.0;
            std::vector<std::pair<std::string, Mat3>> joints;
        };

        std::string                                          status; // converged | unmet
        std::string                                          start;
        int                                                  iterations    = 0;
        double                                               softCost      = 0.0;
        double                                               hardViolation = 0.0;
        std::vector<GoalReport>                              goals;
        std::vector<Unmet>                                   unmet;
        std::vector<std::string>                             warnings;
        std::vector<RangeHit>                                rangeHits;
        std::vector<std::pair<std::string, std::vector<Anatomy>>> anatomy;
        std::vector<Joint>                                   joints;
        bool                                                 pelvisFree = false;
        Vec3                                                 pelvisPosition; // the poser's new mPelvis position offset
        Vec3                                                 pelvisMovedCm;
        std::optional<double>                                floorZ;
        std::vector<StartSummary>                            starts;
        std::vector<Alternative>                             alternatives;
        std::vector<std::pair<std::string, Vec3>>            predicted; // body frame, the solved pose
        State                                                state;
        Spec                                                 frozen; // the spec with anchors and the floor made absolute
    };

    class Problem
    {
    public:
        // Throws SolveError for a spec that does not fit the skeleton.
        Problem(const Skeleton& skeleton, const Spec& spec);
        ~Problem();
        Problem(const Problem&)            = delete;
        Problem& operator=(const Problem&) = delete;

        int                          size() const; // columns of the Jacobian
        const std::vector<Variable>& variables() const;
        const State&                 startState() const;
        const State&                 measuredState() const;
        std::optional<double>        floor() const;

        size_t      termCount() const;
        std::string termName(size_t i) const;
        std::string termType(size_t i) const; // the Python class name (BoneDir, Contact, ...)
        std::string termKind(size_t i) const; // soft | eq | ineq
        double      termTolerance(size_t i) const;
        // Residuals of term i at a state and, with jac, its Jacobian (rows x size(), row major).
        void termEval(size_t i, const State& state, std::vector<double>& values, std::vector<double>* jac) const;

        // Live use (the poser's IK drag): the term of the anchor goal with this name (-1 when none), a new
        // target for it (body frame; false when term is not an anchor).
        int  findTerm(const std::string& name) const;
        bool setAnchorTarget(size_t term, const Vec3& position);

        State  step(const State& state, const std::vector<double>& dx) const;
        double hardViolation(const State& state) const;
        double softCost(const State& state) const;

        // The starts: the start pose, aim, contact, contact_flipped, jitter0 ... (n of them).
        std::vector<std::pair<std::string, State>> starts(int n, unsigned seed) const;
        // LM inside the augmented Lagrangian rounds from one start.
        Run solveFrom(const State& start, const std::string& name, const std::atomic<bool>* cancel = nullptr) const;
        // The best run (the hard goals met, then the weak cost) with clearly different alternatives.
        Result report(const Run& best, const std::vector<Run>& runs, const std::vector<std::pair<const Run*, double>>& alternatives) const;
        // The spec with every "current" anchor / orient and the floor turned into this solve's absolute values.
        Spec frozen() const;

        // Anatomical values of one variable joint at a local rotation (as the range goals see them).
        std::vector<Anatomy> anatomyOf(int joint, const Mat3& local) const;

        struct Impl;

    private:
        std::unique_ptr<Impl> mImpl;
    };

    // The whole solve: starts, a solveFrom each, the best and alternatives, the report.
    // Throws SolveError (bad spec) or SolveCancelled.
    Result solve(const Skeleton& skeleton, const Spec& spec, const std::atomic<bool>* cancel = nullptr);

    // The joints a spec can use (body, bone ends, the sites' anchors and frames, every
    // joint a goal names). The caller reads these and their ancestors into the Skeleton.
    std::vector<std::string> neededJoints(const Spec& spec);

    // Default anatomical limits of a joint ("rotation" included), for nudge.
    std::vector<Anatomy> defaultLimits(const std::string& joint, bool hinge, const std::string& flexibility,
                                       double maxRotationDeg = 120.0);
    // Anatomical values of a joint's local rotation (restDir: its bone in its own frame).
    std::vector<Anatomy> anatomicalValues(const std::string& joint, const Mat3& local, const Vec3& restDir);
    // The joint's bone in its own frame (to its bone end, else its longest child), or nullopt.
    std::optional<Vec3> restBone(const Skeleton& skeleton, int joint);
    // Forward kinematics: world (body frame) rotations and positions.
    void forward(const Skeleton& skeleton, const State& state, std::vector<Mat3>& world, std::vector<Vec3>& pos);
    // The hinge of an elbow / knee / foot (local axis, natural bending sign); false for other joints.
    bool hingeOf(const std::string& joint, Vec3& axis, double& natural);

    // A point resolved on a skeleton: skin points (anchor joint, frame joint, offset in the
    // frame's axes) with weights. Throws SolveError for an unknown joint or site.
    struct ResolvedPart
    {
        double w = 1.0;
        int    anchor = -1, frame = -1;
        Vec3   offset;
    };
    std::vector<ResolvedPart> resolvePoint(const Skeleton& skeleton, const PointSpec& point, const SiteOverrides& overrides = {});
    Vec3 pointPosition(const std::vector<Mat3>& world, const std::vector<Vec3>& pos, const std::vector<ResolvedPart>& parts);

    // The arm or leg that carries a point (the point hangs from its wrist / ankle or below):
    // its root (shoulder / hip) and middle (elbow / knee) joint names. False when none does.
    bool limbOfPoint(const Skeleton& skeleton, const std::vector<ResolvedPart>& parts, std::string& root, std::string& middle,
                     std::string& limb);
}

#endif // PLUS_POSERMCP_POSESOLVER_H

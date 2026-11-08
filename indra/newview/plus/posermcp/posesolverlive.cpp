/**
 * @file posesolverlive.cpp
 * @brief The poser's IK drag (see posesolverlive.h).
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

#include "posesolverlive.h"

#include <algorithm>
#include <cmath>

namespace plus::posermcp
{
    using namespace solver;

    namespace
    {
        // Pulls per cm (a degree of stay is about 0.1-0.3): the pointer, a pinned joint, the wrist / ankle kept in place.
        constexpr double DRAG_WEIGHT = 25.0;
        constexpr double PIN_WEIGHT  = 60.0;
        constexpr double HOLD_WEIGHT = 12.0;

        bool startsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

        bool contains(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }

        bool isLimbJoint(const std::string& n)
        {
            for (const char* p : { "mCollar", "mShoulder", "mElbow", "mWrist", "mHip", "mKnee", "mAnkle" })
            {
                if (startsWith(n, p))
                    return true;
            }
            return false;
        }

        // The body joints that turn when `drag` is pulled (parents first), and the end kept in place.
        void chainOf(const std::string& drag, bool body, bool carryEnd, std::vector<std::string>& free, std::string& held)
        {
            const std::string side = sideOf(drag);
            auto limb = [&](std::initializer_list<const char*> parts)
            {
                // the joints above the dragged one; an elbow / knee also turns itself to keep its end in place
                std::vector<std::string> names;
                for (const char* p : parts)
                    names.push_back(std::string(p) + side);
                const auto it = std::find(names.begin(), names.end(), drag);
                free.assign(names.begin(), it);
                if (it + 1 != names.end() && !carryEnd && (startsWith(drag, "mElbow") || startsWith(drag, "mKnee")))
                {
                    free.push_back(drag);
                    held = *(it + 1);
                }
            };
            if (startsWith(drag, "mCollar") || startsWith(drag, "mShoulder") || startsWith(drag, "mElbow") || startsWith(drag, "mWrist"))
            {
                limb({ "mCollar", "mShoulder", "mElbow", "mWrist" });
                if (startsWith(drag, "mCollar") || body)
                {
                    std::vector<std::string> trunk;
                    if (body)
                        trunk = { "mPelvis", "mTorso" };
                    trunk.push_back("mChest");
                    free.insert(free.begin(), trunk.begin(), trunk.end());
                }
            }
            else if (startsWith(drag, "mHip") || startsWith(drag, "mKnee") || startsWith(drag, "mAnkle"))
            {
                limb({ "mHip", "mKnee", "mAnkle" });
                if (startsWith(drag, "mHip") || body)
                    free.insert(free.begin(), "mPelvis");
            }
            else if (drag == "mPelvis")
            {
                free = { "mPelvis" };
            }
            else
            {
                // mTorso, mChest, mNeck, mHead: the trunk below it
                for (const char* t : { "mTorso", "mChest", "mNeck", "mHead" })
                {
                    if (drag == t)
                        break;
                    free.push_back(t);
                }
                if (body || free.empty())
                    free.insert(free.begin(), "mPelvis");
            }
        }
    }

    struct LiveSolve::Impl
    {
        Skeleton                          sk;
        std::unique_ptr<Problem>          problem;
        int                               dragIndex = -1;
        size_t                            dragTerm  = 0;
        int                               top       = -1;  // the topmost turning joint; the reach sphere's centre
        double                            reach     = 0.0; // m, 0 = no cut
        State                             start, cur;
        std::vector<int>                  turning;         // skeleton indices of the rotation variables
        std::vector<std::pair<int, Mat3>> changed;
        std::vector<Mat3>                 W;
        std::vector<Vec3>                 P;
        bool                              limit = false;
    };

    LiveSolve::LiveSolve(const Skeleton& skeleton, const State& state, const std::string& dragJoint, const std::vector<Pin>& pins,
                         const solver::Options& options, Mods mods)
        : mImpl(std::make_unique<Impl>())
    {
        Impl& m = *mImpl;
        m.sk    = skeleton;
        if (state.local.size() != skeleton.local.size())
            throw SolveError("LiveSolve: the state does not fit the skeleton");
        m.sk.local   = state.local;
        m.sk.rootPos = state.root;
        m.start = m.cur = state;
        m.dragIndex     = m.sk.find(dragJoint);
        if (m.dragIndex < 0)
            throw SolveError("LiveSolve: unknown joint " + dragJoint);
        const std::string drag = m.sk.names[m.dragIndex];

        std::vector<std::string> free;
        std::string              held;
        chainOf(drag, mods.trunk, mods.carry, free, held);

        // pins (never the dragged joint itself)
        std::vector<int> pinJoints;
        for (const Pin& p : pins)
        {
            const int i = m.sk.find(p.joint);
            if (i < 0)
                throw SolveError("LiveSolve: unknown pin joint " + p.joint);
            pinJoints.push_back(i == m.dragIndex ? -1 : i);
        }
        const bool pelvisMoves = drag == "mPelvis";
        // a pinned limb joins when something that turns carries it, so the pin can hold while the body moves
        for (int pi : pinJoints)
        {
            if (pi < 0)
                continue;
            bool carried = pelvisMoves;
            for (int k = pi; k >= 0 && !carried; k = m.sk.parent[k])
                carried = contains(free, m.sk.names[k]);
            if (!carried)
                continue;
            std::vector<std::string> up;
            for (int k = m.sk.parent[pi]; k >= 0; k = m.sk.parent[k])
            {
                if (isLimbJoint(m.sk.names[k]) && k != m.dragIndex)
                    up.push_back(m.sk.names[k]);
            }
            for (auto it = up.rbegin(); it != up.rend(); ++it)
            {
                if (!contains(free, *it) && *it != held)
                    free.push_back(*it);
            }
        }
        std::vector<std::string> keep;
        for (const std::string& n : free)
        {
            const int i = m.sk.find(n);
            if (i >= 0 && m.sk.names[i] == n && !contains(keep, n))
                keep.push_back(n);
        }

        Spec spec;
        spec.options            = options;
        spec.start                = "current";
        spec.freeAuto             = false;
        spec.free                 = keep;
        spec.lockPosition         = !pelvisMoves;
        spec.options.floorGuard   = false;
        spec.options.starts       = 1;
        spec.options.maxOuter     = std::clamp(options.maxOuter, 1, 2);
        spec.options.maxIter      = std::clamp(options.maxIter, 10, 20);

        forward(m.sk, m.start, m.W, m.P);
        auto anchor = [&](const std::string& joint, const Vec3& at, double weight, const std::string& name)
        {
            Goal g;
            g.type     = GoalType::Anchor;
            g.name     = name;
            g.at.joint = joint;
            g.position = at;
            g.hard     = false;
            g.weight   = weight;
            spec.goals.push_back(g);
        };
        anchor(drag, m.P[m.dragIndex], DRAG_WEIGHT, "drag");
        bool heldPinned = false;
        for (size_t k = 0; k < pins.size(); ++k)
        {
            if (pinJoints[k] < 0)
                continue;
            const std::string& n = m.sk.names[pinJoints[k]];
            heldPinned |= n == held;
            anchor(n, pins[k].world, PIN_WEIGHT, "pin " + n);
        }
        const int heldIndex = held.empty() ? -1 : m.sk.find(held);
        if (heldIndex >= 0 && !heldPinned)
            anchor(held, m.P[heldIndex], HOLD_WEIGHT, "hold " + held);

        m.problem = std::make_unique<Problem>(m.sk, spec);
        const int term = m.problem->findTerm("drag");
        if (term < 0)
            throw SolveError("LiveSolve: no drag term");
        m.dragTerm = (size_t)term;
        for (const Variable& v : m.problem->variables())
        {
            if (v.kind != Variable::Kind::Trans)
                m.turning.push_back(v.joint);
        }

        // the reach sphere: the topmost turning joint's centre, the bones down to the dragged joint
        if (!pelvisMoves && !mods.trunk)
        {
            int topJoint = -1;
            for (int k = m.sk.parent[m.dragIndex]; k >= 0; k = m.sk.parent[k])
            {
                if (std::find(m.turning.begin(), m.turning.end(), k) != m.turning.end())
                    topJoint = k;
            }
            if (topJoint >= 0)
            {
                m.top      = topJoint;
                double len = 0.0;
                for (int k = m.dragIndex; k != topJoint; k = m.sk.parent[k])
                    len += norm(m.sk.offset[k]);
                m.reach = len * 0.999;
            }
        }
        m.changed.reserve(m.turning.size());
    }

    LiveSolve::~LiveSolve() = default;

    const std::vector<std::pair<int, Mat3>>& LiveSolve::moveTarget(const Vec3& bodyFrameTarget)
    {
        Impl& m = *mImpl;
        m.changed.clear();
        Vec3 t  = bodyFrameTarget;
        m.limit = false;
        if (m.reach > 0.0)
        {
            forward(m.sk, m.cur, m.W, m.P);
            const Vec3   c = m.P[m.top];
            const Vec3   d = t - c;
            const double l = norm(d);
            if (l > m.reach)
            {
                t       = c + d * (m.reach / l);
                m.limit = true;
            }
        }
        m.problem->setAnchorTarget(m.dragTerm, t);
        const Run run = m.problem->solveFrom(m.cur, "live");
        for (int j : m.turning)
        {
            if (rotationAngle(transpose(m.cur.local[j]) * run.state.local[j]) > 1e-6)
                m.changed.emplace_back(j, run.state.local[j]);
        }
        m.cur = run.state;
        forward(m.sk, m.cur, m.W, m.P);
        if (norm(m.P[m.dragIndex] - t) > 0.01)
            m.limit = true;
        return m.changed;
    }

    Vec3 LiveSolve::pelvisPosition() const
    {
        const Impl& m = *mImpl;
        return m.sk.pelvisOffset + (m.cur.root - m.start.root);
    }

    bool         LiveSolve::atLimit() const { return mImpl->limit; }
    Vec3         LiveSolve::dragPosition() const { return mImpl->P[mImpl->dragIndex]; }
    const State& LiveSolve::state() const { return mImpl->cur; }
}

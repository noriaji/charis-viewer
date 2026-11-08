/**
 * @file contactsolver.cpp
 * @brief The maths of the poser MCP "contact" tool (see contactsolver.h).
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

#include "contactsolver.h"

#include "llmath.h"

#include <cmath>

namespace plus::posermcp
{
    LLVector3 projectToShell(const LLVector3& p, const ContactShell& shell)
    {
        LLVector3 v = p - shell.centre;
        F32       d = v.magVec();
        if (d < 1e-9f)
        {
            v = LLVector3::x_axis;
            d = 0.f;
        }
        else
        {
            v /= d;
        }
        return shell.centre + v * llclamp(d, shell.minReach, llmax(shell.minReach, shell.maxReach));
    }

    bool reachRange(const TwoBoneInput& in, F32& minReach, F32& maxReach, const char*& why)
    {
        TwoBoneInput probe = in;
        probe.target       = (in.end - in.root).magVec() > 0.01f ? in.end : in.root - LLVector3::z_axis;
        probe.pole         = autoPole(in, -LLVector3::z_axis);
        TwoBoneResult result;
        if (!solveTwoBone(probe, result, why))
            return false;
        minReach = result.minReach;
        maxReach = result.maxReach;
        return true;
    }

    LLVector3 autoPole(const TwoBoneInput& in, const LLVector3& fallback, F32 straight)
    {
        LLVector3 u = in.end - in.root;
        u.normalize();
        const LLVector3 upper = in.middle - in.root;
        LLVector3       v     = upper - u * (upper * u);
        if (v.magVec() < straight * upper.magVec())
        {
            LLVector3 f = fallback;
            f.normalize();
            return f;
        }
        v.normalize();
        return v;
    }

    LimbPrediction predictLimb(const TwoBoneInput& in, const TwoBoneResult& solved, const LLVector3& offset)
    {
        const LLQuaternion root_world   = solved.rootLocal * in.parentWorld;
        const LLQuaternion middle_world = solved.middleLocal * root_world;
        const LLQuaternion turn         = ~in.middleWorld * middle_world;
        LimbPrediction     out;
        out.middle      = in.root + (in.middle - in.root) * ~in.rootWorld * root_world;
        out.end         = out.middle + (in.end - in.middle) * turn;
        out.point       = out.end + offset * turn;
        out.middleWorld = middle_world;
        return out;
    }

    bool solveLimbToGoal(const TwoBoneInput& in, const LLVector3& offset, const LLVector3& goal, LimbGoal& out,
                         const char*& why, int iterations, F32 tolerance)
    {
        TwoBoneInput probe = in;
        probe.target       = goal - offset;
        bool have          = false;
        for (int i = 0; i < llmax(1, iterations); ++i)
        {
            TwoBoneResult solved;
            if (!solveTwoBone(probe, solved, why))
            {
                if (!have)
                    return false;
                break;
            }
            const LimbPrediction predicted = predictLimb(in, solved, offset);
            const LLVector3      error     = goal - predicted.point;
            const F32            miss      = error.magVec();
            if (!have || miss < out.miss - 1e-7f)
            {
                out.solved    = solved;
                out.target    = probe.target;
                out.predicted = predicted;
                out.miss      = miss;
                have          = true;
            }
            if (miss <= tolerance)
                break;
            probe.target += error;
        }

        // Out of reach (or the offset turns too much): a pattern search over the wrist /
        // ankle target for the contact point nearest the goal. Beyond the reach the solve
        // clamps the distance, so this searches the direction.
        static const LLVector3 axes[6] = { LLVector3::x_axis, -LLVector3::x_axis, LLVector3::y_axis,
                                           -LLVector3::y_axis, LLVector3::z_axis, -LLVector3::z_axis };
        F32       step  = llmax(0.02f, out.miss);
        LLVector3 t     = out.target;
        int       evals = 0;
        while (out.miss > tolerance && step > 1e-4f && evals < 600)
        {
            bool improved = false;
            for (const LLVector3& axis : axes)
            {
                probe.target = t + axis * step;
                ++evals;
                TwoBoneResult solved;
                const char*   ignored = "";
                if (!solveTwoBone(probe, solved, ignored))
                    continue;
                const LimbPrediction predicted = predictLimb(in, solved, offset);
                const F32            miss      = (goal - predicted.point).magVec();
                if (miss < out.miss - 1e-7f)
                {
                    out.solved    = solved;
                    out.target    = probe.target;
                    out.predicted = predicted;
                    out.miss      = miss;
                    t             = probe.target;
                    improved      = true;
                    break;
                }
            }
            if (!improved)
                step *= 0.5f;
        }
        return true;
    }

    Meeting chooseMeeting(ContactMode mode, const LLVector3& pointA, const LLVector3& pointB, const ContactShell& shellA,
                          const ContactShell& shellB, F32 comfort)
    {
        Meeting out;
        out.goalA = pointA;
        out.goalB = pointB;
        switch (mode)
        {
            case ContactMode::A:
                out.goalA = projectToShell(pointB, shellA);
                break;
            case ContactMode::B:
                out.goalB = projectToShell(pointA, shellB);
                break;
            case ContactMode::Both:
            {
                ContactShell comfortable = shellA;
                comfortable.maxReach     = llmax(shellA.minReach, shellA.maxReach * comfort);
                LLVector3 p              = projectToShell(pointB, comfortable);
                for (int i = 0; i < 1000; ++i)
                {
                    const LLVector3 next = projectToShell(projectToShell(p, shellB), shellA);
                    const bool      done = (next - p).magVec() < 1e-7f;
                    p                    = next;
                    if (done)
                        break;
                }
                out.goalA = p;
                out.goalB = projectToShell(p, shellB);
                break;
            }
            case ContactMode::None:
                break;
        }
        out.gap = (out.goalA - out.goalB).magVec();
        return out;
    }

    ContactMode resolveAuto(const ContactSide& a, const ContactSide& b, F32 comfort, F32 tolerance, const char*& note)
    {
        note             = "";
        auto free_side   = [](const ContactSide& s) { return s.movable && !s.support && !s.busy; };
        auto usable_side = [](const ContactSide& s) { return s.movable && !s.support; };
        bool use_a = free_side(a), use_b = free_side(b);
        if (!use_a && !use_b)
        {
            use_a = usable_side(a);
            use_b = usable_side(b);
        }
        if (!use_a && !use_b)
        {
            note = (a.movable || b.movable) ? "only a supporting leg could move; give mover explicitly to move it"
                                            : "neither point is on an arm or a leg below the elbow / knee";
            return ContactMode::None;
        }
        if (use_a != use_b)
            return use_a ? ContactMode::A : ContactMode::B;

        const bool         prefer_a = a.arm || !b.arm;
        const ContactSide& pref     = prefer_a ? a : b;
        const ContactSide& other    = prefer_a ? b : a;
        ContactShell       comfortable = pref.shell;
        comfortable.maxReach           = llmax(pref.shell.minReach, pref.shell.maxReach * comfort);
        if ((projectToShell(other.point, comfortable) - other.point).magVec() <= tolerance)
            return prefer_a ? ContactMode::A : ContactMode::B;
        return ContactMode::Both;
    }

    TrunkBend trunkBend(const LLVector3& torso, const LLVector3& chest, const LLVector3& centre, const LLVector3& goal,
                        F32 reach, F32 maxAngle)
    {
        TrunkBend out;
        out.centre = centre;
        LLVector3 axis = (centre - chest) % (goal - chest);
        if (axis.magVec() < 1e-6f)
        {
            out.axis    = LLVector3::z_axis;
            out.shortBy = llmax(0.f, (centre - goal).magVec() - reach);
            return out;
        }
        axis.normalize();
        out.axis = axis;

        auto moved = [&](F32 theta)
        {
            const LLQuaternion half(theta * 0.5f, axis);
            return torso + (chest - torso) * half + (centre - chest) * (half * half);
        };
        auto short_of = [&](F32 theta) { return (moved(theta) - goal).magVec() - reach; };

        F32 best_angle = 0.f, best_short = short_of(0.f);
        if (best_short <= 0.f)
        {
            out.shortBy = 0.f;
            return out;
        }
        const int steps = 48;
        F32       prev  = 0.f;
        for (int i = 1; i <= steps; ++i)
        {
            const F32 t = maxAngle * (F32)i / (F32)steps;
            const F32 s = short_of(t);
            if (s < best_short)
            {
                best_angle = t;
                best_short = s;
            }
            if (s <= 0.f)
            {
                F32 lo = prev, hi = t;
                for (int k = 0; k < 40; ++k)
                {
                    const F32 mid = 0.5f * (lo + hi);
                    if (short_of(mid) <= 0.f)
                        hi = mid;
                    else
                        lo = mid;
                }
                out.angle   = hi;
                out.centre  = moved(hi);
                out.shortBy = 0.f;
                return out;
            }
            prev = t;
        }
        out.angle   = best_angle;
        out.centre  = moved(best_angle);
        out.shortBy = llmax(0.f, best_short);
        return out;
    }

    void trunkLocals(const LLQuaternion& torsoLocal, const LLQuaternion& torsoParentWorld, const LLQuaternion& chestLocal,
                     const LLQuaternion& torsoWorld, const LLVector3& axis, F32 angle, LLQuaternion& newTorso,
                     LLQuaternion& newChest)
    {
        // The torso's world rotation gains the turn (pivot: the torso joint), the chest
        // gains it again on top (pivot: the chest joint): world = local * parent.
        const LLQuaternion half(angle * 0.5f, axis);
        newTorso = torsoLocal * torsoParentWorld * half * ~torsoParentWorld;
        newChest = chestLocal * torsoWorld * half * ~torsoWorld;
        newTorso.normalize();
        newChest.normalize();
    }
}

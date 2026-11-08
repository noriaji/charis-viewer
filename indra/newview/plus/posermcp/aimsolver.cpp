/**
 * @file aimsolver.cpp
 * @brief Bone aiming for the poser MCP "aim" tool. See aimsolver.h.
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

#include "aimsolver.h"
#include "reachsolver.h" // basisRotation

#include "llmath.h"

#include <algorithm>
#include <cmath>

namespace plus::posermcp
{
    namespace
    {
        F32 angleBetween(const LLVector3& a, const LLVector3& b)
        {
            return atan2f((a % b).magVec(), a * b);
        }

        // The part of v across the unit vector axis.
        LLVector3 across(const LLVector3& v, const LLVector3& axis)
        {
            return v - axis * (v * axis);
        }

        F32 wrapAngle(F32 angle)
        {
            while (angle > F_PI)
                angle -= F_TWO_PI;
            while (angle <= -F_PI)
                angle += F_TWO_PI;
            return angle;
        }

        // Upper arm / thigh pointing along d (unit), twisted so that the child bone r2
        // (in the child's frame), bent about the child's hinge only, points along t (unit).
        // As in reachsolver.cpp: the bend angle comes from the angle the two bones must
        // make (cos = c1 c2 + n1 n2 cos(phi - base); the bones need not be straight on at
        // rest, the legs are not), then both bones are aligned at once. Of the two ways
        // round the natural one, then the closer, then the smaller bend.
        bool limbRoll(const LLVector3& r1, const LLVector3& d, const LLVector3& child_bone, const LLVector3& child_hinge,
                      F32 natural, const LLVector3& t, LLQuaternion& world, F32& miss, F32& bend)
        {
            LLVector3 k = child_hinge;
            LLVector3 r2 = child_bone;
            if (k.normalize() < 0.5f || r2.normalize() < 1e-6f)
                return false;
            const F32       c1 = r1 * k;
            const F32       c2 = r2 * k;
            const LLVector3 p1 = r1 - k * c1;
            const LLVector3 p2 = r2 - k * c2;
            const F32       n1 = p1.magVec();
            const F32       n2 = p2.magVec();
            if (n1 < 0.01f || n2 < 0.01f)
                return false;
            LLVector3 e1 = p1;
            e1.normalize();
            const LLVector3 e2     = k % e1;
            const F32       base   = -atan2f(p2 * e2, p2 * e1);
            const F32       cosine = llclamp(d * t, -1.f, 1.f);
            const F32       spread = acosf(llclamp((cosine - c1 * c2) / (n1 * n2), -1.f, 1.f));

            LLVector3 B = across(t, d);
            if (B.normalize() < 1e-3f)
                return false;

            bool found = false, best_natural = false;
            F32  best_error = F32_MAX, best_angle = 0.f;
            for (F32 side : { 1.f, -1.f })
            {
                const F32          angle = wrapAngle(base + side * spread);
                const LLQuaternion bent(angle, k);
                const LLVector3    r2_bent = r2 * bent;
                LLVector3          b       = across(r2_bent, r1);
                if (b.normalize() < 1e-3f)
                    continue; // the child would lie along the bone: no twist to read
                const LLQuaternion candidate = basisRotation(r1, b, d, B);
                const F32          error     = (r2_bent * candidate - t).magVec();
                const bool         is_natural = angle * natural >= -0.02f;
                const bool better = !found || (is_natural && !best_natural) ||
                                    (is_natural == best_natural &&
                                     (error < best_error - 1e-4f || (fabsf(error - best_error) <= 1e-4f && fabsf(angle) < fabsf(best_angle))));
                if (better)
                {
                    found        = true;
                    world        = candidate;
                    best_error   = error;
                    best_angle   = angle;
                    best_natural = is_natural;
                }
            }
            if (!found)
                return false;
            miss = 2.f * asinf(llclamp(best_error * 0.5f, 0.f, 1.f)); // chord -> angle
            bend = best_angle;
            return true;
        }
    }

    LLQuaternion swingRotation(const LLVector3& a, const LLVector3& b)
    {
        const LLVector3 c = a % b;
        const F32       s = c.magVec();
        const F32       cosine = a * b;
        if (s > 1e-6f)
        {
            LLQuaternion q(atan2f(s, cosine), c); // normalizes the axis
            q.normalize();
            return q;
        }
        if (cosine > 0.f)
            return LLQuaternion::DEFAULT; // already there

        // Opposite: half a turn about the axis across a closest to the coordinate
        // axis a is least along.
        int k = 0;
        for (int i = 1; i < 3; ++i)
        {
            if (fabsf(a.mV[i]) < fabsf(a.mV[k]))
                k = i;
        }
        LLVector3 e;
        e.mV[k]        = 1.f;
        LLVector3 axis = across(e, a);
        axis.normalize();
        LLQuaternion q(F_PI, axis);
        q.normalize();
        return q;
    }

    bool solveAim(const std::vector<AimJoint>& joints, const std::vector<AimBone>& bones, std::vector<AimResult>& results,
                  std::string& why, int& bad_bone)
    {
        results.assign(bones.size(), AimResult());
        bad_bone    = -1;
        const int n = (int)joints.size();

        // Depth of every joint in the list (parents first), and no loops.
        std::vector<int> depth(n, 0);
        for (int i = 0; i < n; ++i)
        {
            int p     = joints[i].parent;
            int steps = 0;
            while (p != -1)
            {
                if (p < 0 || p >= n || ++steps > n)
                {
                    why = "the joint list has a bad parent link";
                    return false;
                }
                p = joints[p].parent;
            }
            depth[i] = steps;
        }

        std::vector<int> bone_of(n, -1);
        for (int b = 0; b < (int)bones.size(); ++b)
        {
            const AimBone& bone = bones[b];
            bad_bone            = b;
            if (bone.joint < 0 || bone.joint >= n)
            {
                why = "bad joint index";
                return false;
            }
            if (bone_of[bone.joint] != -1)
            {
                why = "the joint is listed twice";
                return false;
            }
            bone_of[bone.joint] = b;
            if (bone.bone.magVec() < 1e-6f)
            {
                why = "the bone has no length";
                return false;
            }
            if (bone.direction.magVec() < 1e-6f)
            {
                why = "the direction is zero";
                return false;
            }
            if (bone.roll && (bone.rollToward.magVec() < 1e-6f || bone.side.magVec() < 1e-6f))
            {
                why = "roll_toward is zero";
                return false;
            }
        }
        bad_bone = -1;

        std::vector<int> order(bones.size());
        for (int b = 0; b < (int)bones.size(); ++b)
            order[b] = b;
        std::stable_sort(order.begin(), order.end(),
                         [&](int a, int b) { return depth[bones[a].joint] < depth[bones[b].joint]; });

        std::vector<LLQuaternion> local(n);
        for (int i = 0; i < n; ++i)
            local[i] = joints[i].local;

        // World rotation from the locals solved so far (world = local * parent).
        auto world_of = [&](int i)
        {
            LLQuaternion q = local[i];
            while (joints[i].parent != -1)
            {
                i = joints[i].parent;
                q = q * local[i];
            }
            return q * joints[i].parentWorld;
        };

        for (int b : order)
        {
            const AimBone& bone = bones[b];
            AimResult&     out  = results[b];
            const int      j    = bone.joint;

            const LLQuaternion parent_world = joints[j].parent != -1 ? world_of(joints[j].parent) : joints[j].parentWorld;

            LLVector3 r = bone.bone;
            r.normalize();
            LLVector3 d = bone.direction;
            d.normalize();

            LLQuaternion new_world;
            LLVector3    toward_across;
            bool         solved    = false;
            bool         from_rest = bone.twist == AimTwist::Rest;
            // Elbows and knees only bend: a twist would fold the mesh (SL has no forearm /
            // shin twist joint). The upper arm's / thigh's limb roll sets their twist.
            const bool roll = bone.roll && !bone.hinge;
            out.rollIgnored = bone.roll && bone.hinge;
            if (roll)
            {
                LLVector3 toward = bone.rollToward;
                toward.normalize();
                toward_across = across(toward, d);
                if (toward_across.normalize() < 0.05f)
                {
                    out.note = "roll_toward is (almost) along the bone, so it was ignored";
                }
                else if (bone.limb && angleBetween(d, toward) < AIM_STRAIGHT_LIMB_DEG * DEG_TO_RAD)
                {
                    out.straight = true;
                    from_rest    = true;
                    out.note     = "the arm / leg is almost straight (its two bones less than 15 degrees apart), so "
                                   "limb_roll was not used and the twist is the rest pose's (twist rest); set the hand "
                                   "with mWrist, the foot with mAnkle (+ mFoot)";
                }
                else if (bone.limb)
                {
                    F32 miss = 0.f, bend = 0.f;
                    if (limbRoll(r, d, bone.childBone, bone.childHinge, bone.childNatural, toward, new_world, miss, bend))
                    {
                        out.method    = AimMethod::Limb;
                        out.rollError = miss;
                        out.bend      = bend;
                        solved        = true;
                    }
                    else
                    {
                        out.note = "the limb's bones lie along the hinge, so roll_toward was ignored";
                    }
                }
                else
                {
                    LLVector3 side = bone.side;
                    side.normalize();
                    LLVector3 side_across = across(side, r);
                    if (side_across.normalize() < 0.05f)
                    {
                        out.note = "the bone runs along its reference side axis, so roll_toward was ignored";
                    }
                    else
                    {
                        new_world  = basisRotation(r, side_across, d, toward_across);
                        out.method = AimMethod::Side;
                        solved     = true;
                    }
                }
            }
            if (!solved && bone.hinge && bone.twist == AimTwist::Auto)
            {
                // Elbow / knee: bend about the hinge only (from the rest pose). The bend
                // puts the bone as close to the direction as the hinge can (the direction
                // projected onto the cone the hinge sweeps the bone around); within 0.25
                // degrees a tiny swing takes up the rest, otherwise the miss is reported
                // (never a free turn: it would fold the mesh). At most 5 degrees the
                // wrong way.
                LLVector3  k    = bone.hingeAxis;
                const bool k_ok = k.normalize() > 0.5f;
                LLVector3  p    = across(r, k);
                if (k_ok && p.normalize() > 0.05f)
                {
                    const LLVector3 target = d * ~parent_world; // in the parent's frame
                    LLVector3       q      = across(target, k);
                    F32             angle  = 0.f;               // (the direction along the hinge axis: any bend misses alike)
                    if (q.normalize() > 0.05f)
                        angle = atan2f((p % q) * k, p * q);
                    const F32 back = -AIM_HINGE_BACK_DEG * DEG_TO_RAD;
                    if (angle * bone.natural < back)
                    {
                        angle            = back * bone.natural;
                        out.hingeClamped = true;
                    }
                    const LLQuaternion bent(angle, k);
                    const LLQuaternion world = bent * parent_world;
                    LLVector3          now   = r * world;
                    now.normalize();
                    const F32 miss = angleBetween(now, d);
                    if (miss <= 0.25f * DEG_TO_RAD)
                        new_world = world * swingRotation(now, d);
                    else
                    {
                        new_world     = world;
                        out.hingeMiss = miss;
                    }
                    out.method = AimMethod::Hinge;
                    out.bend   = angle;
                    solved     = true;
                }
            }
            if (!solved)
            {
                // Smallest turn from where the bone points now (Keep, Auto) or would
                // point with no local rotation (Rest), so the twist about the bone stays.
                const LLQuaternion current = from_rest ? parent_world : local[j] * parent_world;
                LLVector3          now     = r * current;
                now.normalize();
                new_world = current * swingRotation(now, d);
            }
            new_world.normalize();

            LLQuaternion new_local = new_world * ~parent_world;
            new_local.normalize();
            local[j] = new_local;

            out.local = new_local;
            out.world = new_local * parent_world;
            out.world.normalize();
            out.achieved = r * out.world;
            out.achieved.normalize();
            out.error = angleBetween(out.achieved, d);
            if (out.method == AimMethod::Side)
            {
                LLVector3 side = bone.side;
                side.normalize();
                LLVector3 side_now = across(side * out.world, out.achieved);
                side_now.normalize();
                out.rollError = angleBetween(side_now, toward_across);
            }

            // How far the joint turned and twisted from its rest rotation (folding meshes).
            const F32 sine = sqrtf(new_local.mQ[VX] * new_local.mQ[VX] + new_local.mQ[VY] * new_local.mQ[VY] +
                                   new_local.mQ[VZ] * new_local.mQ[VZ]);
            out.angle     = 2.f * atan2f(sine, fabsf(new_local.mQ[VW]));
            out.twist     = twistAbout(new_local, r);
            // (hinge bends and limb rolls are the pose itself: an upper arm's / thigh's twist is what lets
            // the elbow / knee bend onto the forearm / shin)
            const bool posture = out.method == AimMethod::Hinge || out.method == AimMethod::Limb;
            out.warnAngle      = !posture && out.angle > AIM_WARN_ANGLE_DEG * DEG_TO_RAD;
            out.warnTwist      = out.method != AimMethod::Limb && out.twist > AIM_WARN_TWIST_DEG * DEG_TO_RAD;
        }
        return true;
    }

    F32 twistAbout(const LLQuaternion& q, const LLVector3& axis)
    {
        LLVector3 a = axis;
        if (a.normalize() < 1e-6f)
            return 0.f;
        // a vector across the axis: from the coordinate axis it is least along
        int k = 0;
        for (int i = 1; i < 3; ++i)
        {
            if (fabsf(a.mV[i]) < fabsf(a.mV[k]))
                k = i;
        }
        LLVector3 e;
        e.mV[k] = 1.f;
        e       = across(e, a);
        e.normalize();
        LLVector3 turned = a * q;
        turned.normalize();
        const LLQuaternion swing = swingRotation(a, turned);
        return angleBetween(e * q, e * swing);
    }
}

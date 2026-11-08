/**
 * @file reachsolver.cpp
 * @brief Two-bone IK for the poser MCP "reach" tool. See reachsolver.h.
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

#include "reachsolver.h"

#include "llmath.h"
#include "m3math.h"

#include <cmath>

namespace plus::posermcp
{
    LLQuaternion basisRotation(const LLVector3& a, const LLVector3& b, const LLVector3& A, const LLVector3& B)
    {
        const LLVector3 c = a % b;
        const LLVector3 C = A % B;
        // v * M with M = L^T * W, rows of L = (a, b, c), rows of W = (A, B, C).
        LLMatrix3 m;
        for (int i = 0; i < 3; ++i)
        {
            for (int j = 0; j < 3; ++j)
                m.mMatrix[i][j] = a.mV[i] * A.mV[j] + b.mV[i] * B.mV[j] + c.mV[i] * C.mV[j];
        }
        LLQuaternion q = m.quaternion();
        q.normalize();
        return q;
    }

    bool solveTwoBone(const TwoBoneInput& in, TwoBoneResult& out, const char*& why)
    {
        const LLVector3& S  = in.root;
        const LLVector3& E  = in.middle;
        const LLVector3& W  = in.end;
        const F32        L1 = (E - S).magVec();
        const F32        L2 = (W - E).magVec();
        if (L1 < 0.01f || L2 < 0.01f)
        {
            why = "bone lengths could not be read";
            return false;
        }

        LLVector3 u = in.target - S;
        F32       d = u.normalize();
        if (d < 0.001f)
        {
            why = "the target is at the root joint";
            return false;
        }

        // Bone directions in the joints' own frames, from the drawn skeleton.
        LLVector3 r1 = (E - S) * ~in.rootWorld;
        r1.normalize();
        LLVector3 r2 = (W - E) * ~in.middleWorld;
        r2.normalize();

        // The middle joint only bends about its hinge, but the bones are not exactly
        // straight on at rest: the legs have a knee bend of about 6.5 degrees at rest
        // (3.4 of it about the hinge) and the thigh and shin lean sideways a little
        // (the arms are exact). So the bend angle is solved from the angle the triangle
        // needs between the two bones, not by projecting along the hinge; then the root
        // is turned so that both bones point where they should (exact whenever that
        // angle is possible). With a bend of phi about the hinge, the cosine of the
        // angle between the bones is c1 c2 + n1 n2 cos(phi - base).
        LLVector3 hinge = in.hinge;
        if (hinge.normalize() < 0.5f)
        {
            why = "no hinge axis";
            return false;
        }
        const F32       c1 = r1 * hinge;
        const F32       c2 = r2 * hinge;
        const LLVector3 p1 = r1 - hinge * c1;
        const LLVector3 p2 = r2 - hinge * c2;
        const F32       n1 = p1.magVec();
        const F32       n2 = p2.magVec();
        if (n1 < 0.01f || n2 < 0.01f)
        {
            why = "the bones lie along the hinge axis";
            return false;
        }
        LLVector3 e1 = p1;
        e1.normalize();
        const LLVector3 e2   = hinge % e1;
        const F32       base = -atan2f(p2 * e2, p2 * e1);

        // The distances the limb can span. A leg cannot be made fully straight: the
        // sideways lean stays, and taking out the rest knee bend would bend the knee
        // the wrong way, so its longest natural reach is at phi = 0 (arms: L1 + L2).
        // A little short of the longest, so that the bend plane stays defined.
        const F32 phi_straight = base * in.natural >= 0.f ? base : 0.f;
        const F32 cos_max      = c1 * c2 + n1 * n2 * cosf(phi_straight - base);
        const F32 cos_min      = c1 * c2 - n1 * n2;
        const F32 d_max        = sqrtf(llmax(0.f, L1 * L1 + L2 * L2 + 2.f * L1 * L2 * cos_max)) * 0.999f;
        const F32 d_min        = sqrtf(llmax(0.f, L1 * L1 + L2 * L2 + 2.f * L1 * L2 * cos_min)) + 0.001f;
        if (d_min >= d_max)
        {
            why = "the bone lengths leave no room to bend";
            return false;
        }

        // Where the elbow/knee goes: on the circle around the root->target line,
        // on the side of the pole.
        const F32 reach_d = llclamp(d, d_min, d_max);
        const F32 a       = (L1 * L1 - L2 * L2 + reach_d * reach_d) / (2.f * reach_d);
        const F32 h       = sqrtf(llmax(0.f, L1 * L1 - a * a));

        LLVector3 pole = in.pole;
        pole.normVec();
        LLVector3 v = pole - u * (pole * u);
        if (v.normalize() < 0.01f)
        {
            // pole is along the reach line: keep the current bend side.
            v = (E - S) - u * ((E - S) * u);
            if (v.normalize() < 0.01f)
            {
                v = LLVector3::z_axis % u;
                if (v.normalize() < 0.01f)
                {
                    // fix-round53 3: reaching straight up or down: another axis gives the plane
                    v = LLVector3::x_axis % u;
                    v.normalize();
                }
            }
        }
        const LLVector3 elbow   = S + u * a + v * h;
        const LLVector3 reached = S + u * reach_d;
        LLVector3       dir1    = elbow - S;
        dir1.normalize();
        LLVector3 dir2 = reached - elbow;
        dir2.normalize();
        LLVector3 normal = dir1 % dir2;
        if (normal.normalize() < 0.001f)
        {
            normal = dir1 % v; // straight limb: keep the bend plane of the pole
            normal.normalize();
        }

        const F32  cos_theta = llclamp(dir1 * dir2, -1.f, 1.f);
        const F32  spread    = acosf(llclamp((cos_theta - c1 * c2) / (n1 * n2), -1.f, 1.f));
        const bool bent      = (dir1 % dir2).magVec() > 1e-3f;

        // Of the two ways round, take the one that bends the natural way. Both can be
        // exact (folded nearly double): then the smaller bend.
        LLQuaternion best_root, best_middle;
        F32          best_angle = 0.f, best_error = F32_MAX;
        bool         best_natural = false;
        for (F32 side : { 1.f, -1.f })
        {
            F32 angle = base + side * spread;
            while (angle > F_PI)
                angle -= F_TWO_PI;
            while (angle <= -F_PI)
                angle += F_TWO_PI;
            const LLQuaternion bend(angle, hinge);
            const LLVector3    r2_bent = r2 * bend;

            // Root: map the (upper bone, forearm/shin) pair onto the wanted directions.
            // A straight limb has no bend plane: put the hinge across the pole's plane.
            LLVector3 b, B;
            if (bent && (r1 % r2_bent).magVec() > 1e-3f)
            {
                b = r2_bent - r1 * (r2_bent * r1);
                B = dir2 - dir1 * (dir2 * dir1);
            }
            else
            {
                b = hinge - r1 * (hinge * r1);
                B = normal * side;
            }
            b.normalize();
            B.normalize();
            const LLQuaternion new_root_world = basisRotation(r1, b, dir1, B);
            const F32          error          = (r2_bent * new_root_world - dir2).magVec();
            const bool         natural        = angle * in.natural >= -0.02f;
            const bool better = (natural && !best_natural) ||
                                (natural == best_natural &&
                                 (error < best_error - 1e-4f || (fabsf(error - best_error) <= 1e-4f && fabsf(angle) < fabsf(best_angle))));
            if (better)
            {
                best_root    = new_root_world * ~in.parentWorld; // world = local * parent
                best_middle  = bend;
                best_angle   = angle;
                best_error   = error;
                best_natural = natural;
            }
        }

        out.rootLocal    = best_root;
        out.middleLocal  = best_middle;
        out.bendPoint    = elbow;
        out.reachedPoint = reached;
        out.bendAngle    = best_angle;
        out.error        = best_error;
        out.distance     = d;
        out.maxReach     = d_max;
        out.minReach     = d_min;
        out.reachable    = d <= d_max && d >= d_min;
        return true;
    }
}

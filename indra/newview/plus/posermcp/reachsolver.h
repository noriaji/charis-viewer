/**
 * @file reachsolver.h
 * @brief Two-bone IK for the poser MCP "reach" tool: the maths only, no viewer state.
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

// Only needs llmath (LLVector3, LLQuaternion), so it can be built and tested
// on its own (tools/poser-mcp/ik in the sl-viewer repository), against numbers
// produced from the real skeleton file by tools/poser-mcp/ik_check.py.

#ifndef PLUS_POSERMCP_REACHSOLVER_H
#define PLUS_POSERMCP_REACHSOLVER_H

#include "llmath.h" // must come first (llquaternion.h insists)
#include "llquaternion.h"
#include "v3math.h"

namespace plus::posermcp
{
    // Everything is in world space except `hinge`, which is in the middle
    // joint's own frame. Rotations follow the viewer: row vectors (v * q) and
    // world = local * parent.
    struct TwoBoneInput
    {
        LLVector3    root;          // shoulder / hip position
        LLVector3    middle;        // elbow / knee position
        LLVector3    end;           // wrist / ankle position
        LLQuaternion rootWorld;     // world rotations of the joints as drawn now
        LLQuaternion middleWorld;
        LLQuaternion parentWorld;   // world rotation of the root joint's parent
        LLVector3    target;        // where the wrist / ankle should go
        LLVector3    pole;          // direction the elbow / knee should point to (need not be unit)
        LLVector3    hinge;         // the middle joint's bending axis (unit, middle joint's frame)
        float        natural = 1.f; // sign of the bend angle that bends the natural way
    };

    struct TwoBoneResult
    {
        LLQuaternion rootLocal;       // new local rotation of the root joint
        LLQuaternion middleLocal;     // new local rotation of the middle joint (a rotation about the hinge)
        LLVector3    bendPoint;       // where the elbow / knee ends up
        LLVector3    reachedPoint;    // where the wrist / ankle ends up (the target, or the nearest reachable point)
        float        bendAngle = 0.f; // radians, about the hinge
        float        error     = 0.f; // how far the forearm / shin direction is from the wanted one (0 = exact)
        float        distance  = 0.f; // root to target
        float        maxReach  = 0.f; // longest root-to-end distance with a natural bend (a hair short of it)
        float        minReach  = 0.f; // shortest
        bool         reachable = false; // minReach <= distance <= maxReach
    };

    // Returns false (and says why) when the input cannot be solved: bone length
    // zero, target on the root, no hinge, bones along the hinge.
    bool solveTwoBone(const TwoBoneInput& in, TwoBoneResult& out, const char*& why);

    // Rotation (row-vector convention, v * q) that maps the orthonormal basis
    // (a, b, a x b) onto (A, B, A x B).
    LLQuaternion basisRotation(const LLVector3& a, const LLVector3& b, const LLVector3& A, const LLVector3& B);
}

#endif // PLUS_POSERMCP_REACHSOLVER_H

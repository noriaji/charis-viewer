/**
 * @file somaretarget.h
 * @brief Poses between Second Life's skeleton and the SOMA skeleton of the
 * motion model ("Motion Between Poses", research/324 section 2.4): the maths
 * only, no viewer state.
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

// Only needs llmath (LLVector3, LLQuaternion), so it is built and tested on its
// own (somaretarget_test.cpp) against the Python twin's answers
// (tools/motionbricks-eval/sl_to_soma.py and golden/ in the sl-viewer repository).
//
// The method: both rest skeletons stand in a T pose. SL's rest skeleton has no
// rotation in any joint; SOMA's has a rest rotation per joint (somatables.h).
// For each of the 21 pairs (SOMA joint J, SL joint j) the turn away from the
// rest pose is carried over as it is (written in the column-vector order, a * b =
// first b, then a; the code writes them the viewer's way, see Frames below):
//   SL to SOMA:  Q(J)    = C * W_SL(j) * C^-1 * Qrest(J)
//   SOMA to SL:  W_SL(j) = C^-1 * Q(J) * Qrest(J)^-1 * C
// (world rotations; C turns SL's body frame into SOMA's aligned frame). The two
// are exact inverses: SL to SOMA to SL comes back up to rounding.
//
// Frames:
//   SL: the avatar's body frame, +X forward, +Y left, +Z up, metres. Rotations as
//     the viewer keeps them: row vectors (v * q), world = local * parent_world.
//   SOMA: the aligned frame, +X forward, Y up, +Z right, metres. Kept in
//     LLQuaternion too; the four numbers are the same unit quaternion as SOMA's
//     {w, x, y, z} (LLQuaternion stores x, y, z, w), only the order of products
//     is written the viewer's way (a * b = first a, then b).
// The hips height is not taken from the poses: SL to SOMA puts the feet on the
// floor, SOMA to SL puts SL's soles at the height of SOMA's soles (times k).

#ifndef PLUS_POSERMOTION_SOMARETARGET_H
#define PLUS_POSERMOTION_SOMARETARGET_H

#include "llmath.h" // must come first (llquaternion.h insists)
#include "llquaternion.h"
#include "v3math.h"

#include <array>
#include <string>
#include <vector>

namespace plus::posermotion
{
    inline constexpr S32 SOMA_JOINT_COUNT = 30;
    inline constexpr S32 MAX_REST_JOINTS  = 512;    // the SL skeleton has 133 bones
    inline constexpr F32 MAX_POSITION_M   = 10.f;   // pelvis, hips and rest offsets beyond this are refused
    inline constexpr F32 QUAT_LENGTH_MIN  = 0.5f;   // a rotation of another length is refused (degenerate),
    inline constexpr F32 QUAT_LENGTH_MAX  = 2.f;    // one inside is normalized

    /// One bone of SL's rest skeleton (avatar_skeleton.xml, no shape sliders).
    struct RestJoint
    {
        std::string name;
        S32         parent = -1;            // index in RestSkeleton::joints, -1 for the root (mPelvis)
        LLVector3   pos;                    // rest position in the parent's frame (m): LLJoint::getDefaultPosition()
        LLVector3   scale{ 1.f, 1.f, 1.f }; // LLJoint::getDefaultScale(); a child's pos is scaled by it
    };

    /// SL's rest skeleton as values (restskeleton.h makes it from the avatar), parents before children.
    struct RestSkeleton
    {
        std::vector<RestJoint> joints;
    };

    /// An SL pose: a local rotation for every joint of the RestSkeleton (same order; identity where the pose
    /// does not turn the joint) and the position of mPelvis in the body frame (its rest position when the pose
    /// does not move it).
    struct SlPose
    {
        std::vector<LLQuaternion> local;
        LLVector3                 pelvis;
    };

    /// A SOMA pose: world positions and world rotations of the 30 joints (aligned frame), the model's input.
    struct SomaPose
    {
        std::array<LLVector3, SOMA_JOINT_COUNT>    pos;
        std::array<LLQuaternion, SOMA_JOINT_COUNT> rot;
    };

    /// One frame of the model's answer: the hips position and each joint's rotation to its parent (Hips: world).
    struct SomaFrame
    {
        LLVector3                                  hips;
        std::array<LLQuaternion, SOMA_JOINT_COUNT> local;
    };

    enum class RetargetResult
    {
        Ok,
        BadSkeleton, // the rest skeleton is not one this works on (names, order, sizes)
        BadInput,    // a pose or a frame has the wrong size, a degenerate rotation or a position out of range
        NotFinite,   // NaN or infinity in the input or the result
    };

    const char* describe(RetargetResult result);

    /// The angle (radians, 0..pi) between two rotations, precise near 0 (not acos of a dot product).
    F32 rotationAngle(const LLQuaternion& a, const LLQuaternion& b);

    /// v is finite and no component is beyond MAX_POSITION_M.
    bool validPosition(const LLVector3& v);

    /// A copy of q at unit length when its length is in [QUAT_LENGTH_MIN, QUAT_LENGTH_MAX] and finite; false if not.
    bool normalizedRotation(const LLQuaternion& q, LLQuaternion& out);

    /// Scratch arrays of the conversions, sized on first use and kept by the caller (no allocation afterwards).
    struct RetargetWork
    {
        std::vector<LLQuaternion> world;
        std::vector<LLVector3>    pos;
    };

    /// Both rest skeletons and what follows from them (the joint pairs, the hips scale k, the rest heights of
    /// the soles). Made once per rest skeleton; after init() the conversions only read it (they keep no state,
    /// so one object may be used from any one thread at a time with its own RetargetWork).
    class SomaRetarget
    {
    public:
        /// Builds everything from rest and somatables.h. On failure the object is left empty and not ready
        /// (jointCount() 0), never half made.
        RetargetResult init(const RestSkeleton& rest);
        bool           ready() const { return mReady; }

        S32              jointCount() const { return (S32)mRest.size(); }
        S32              find(const std::string& name) const; // index in the rest skeleton, -1 if none
        /// The rest joint at index (asserts the range; an empty joint outside it).
        const RestJoint& joint(S32 index) const;
        /// True for the 21 SL joints the model moves; the others (fingers, face, spine between, tail, wings...)
        /// keep the rotations the caller gives somaToSl().
        bool             isDriven(S32 index) const
        {
            return index >= 0 && index < (S32)mDrivenSoma.size() && mDrivenSoma[index] >= 0;
        }
        /// k: SL's hips are this many times as high as SOMA's (rest poses, the hip joints' middle).
        F32              hipScale() const { return mK; }
        const LLVector3& pelvisRest() const { return mPelvisRest; }

        /// SL to SOMA. pose.local must have jointCount() rotations; out is changed only when Ok.
        RetargetResult slToSoma(const SlPose& pose, SomaPose& out, RetargetWork& work) const;

        /// SOMA to SL. In: io.local of the joints that are not driven (sized jointCount()); out: io.local of the
        /// driven joints (to their SL parents, so a turned spine or shoulder in between is taken into account) and
        /// io.pelvis. On failure io is left as it was.
        RetargetResult somaToSl(const SomaFrame& frame, SlPose& io, RetargetWork& work) const;

        /// The model's local rotations of a SOMA pose (the inverse of what somaToSl reads), for tests and callers
        /// that turn a SomaPose into the answer's form.
        static void localsOf(const SomaPose& pose, SomaFrame& out);

    private:
        bool initSl(const RestSkeleton& rest); // the SL side: order, names, sizes, rest positions
        bool initSoma();                       // somatables.h as viewer types
        bool initPairs();                      // the 21 pairs, the soles, k

        template <typename T> using PerSoma = std::array<T, SOMA_JOINT_COUNT>;
        static constexpr S32 SOLE_POINTS = 4;

        bool                   mReady = false;
        // SL side
        std::vector<RestJoint> mRest;
        std::vector<S32>       mDrivenSoma;       // per SL joint: the SOMA joint it takes, -1 if none
        std::vector<LLVector3> mRestPos;          // rest positions in the body frame
        LLVector3              mPelvisRest;       // mPelvis' rest position (the root's pos)
        std::array<S32, SOLE_POINTS> mSoleSl{};   // mAnkleLeft, mFootLeft, mAnkleRight, mFootRight
        // SOMA side (somatables.h, as viewer types)
        PerSoma<S32>           mSlOfSoma{};       // per SOMA joint: the SL joint it gives, -1 if none
        PerSoma<S32>           mSomaParent{};
        PerSoma<LLVector3>     mSomaOffset;
        PerSoma<LLQuaternion>  mSomaRestLocal;
        PerSoma<LLQuaternion>  mSomaRestWorld;
        std::array<S32, SOLE_POINTS> mSoleSoma{}; // LeftFoot, LeftToeBase, RightFoot, RightToeBase
        std::array<F32, SOLE_POINTS> mSoleSomaRestY{};
        F32                    mK = 1.f;
    };
} // namespace plus::posermotion

#endif // PLUS_POSERMOTION_SOMARETARGET_H

/**
 * @file somaretarget.cpp
 * @brief Poses between Second Life's skeleton and the SOMA skeleton of the
 * motion model: the maths only, no viewer state (see somaretarget.h).
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

#include "somaretarget.h"
#include "somatables.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_set>
#include <utility>

namespace plus::posermotion
{
    static_assert(soma::JOINT_COUNT == SOMA_JOINT_COUNT, "somatables.h has the model's 30 joints");

    namespace
    {
        // The 21 pairs (research/324 section 2.4). Spine1, Neck2, the jaw, the eyes and the finger ends of SOMA
        // have no SL partner: SL to SOMA keeps them at their rest rotation to the parent.
        struct Pair
        {
            const char* soma;
            const char* sl;
        };
        constexpr Pair PAIRS[] = {
            { "Hips", "mPelvis" },
            { "Spine2", "mTorso" },
            { "Chest", "mChest" },
            { "Neck1", "mNeck" },
            { "Head", "mHead" },
            { "LeftShoulder", "mCollarLeft" },
            { "LeftArm", "mShoulderLeft" },
            { "LeftForeArm", "mElbowLeft" },
            { "LeftHand", "mWristLeft" },
            { "LeftLeg", "mHipLeft" },
            { "LeftShin", "mKneeLeft" },
            { "LeftFoot", "mAnkleLeft" },
            { "LeftToeBase", "mFootLeft" },
            { "RightShoulder", "mCollarRight" },
            { "RightArm", "mShoulderRight" },
            { "RightForeArm", "mElbowRight" },
            { "RightHand", "mWristRight" },
            { "RightLeg", "mHipRight" },
            { "RightShin", "mKneeRight" },
            { "RightFoot", "mAnkleRight" },
            { "RightToeBase", "mFootRight" },
        };

        // The points that put the feet on the floor: their height above their rest height, the lowest one.
        constexpr const char* SOLE_SOMA[] = { "LeftFoot", "LeftToeBase", "RightFoot", "RightToeBase" };
        constexpr const char* SOLE_SL[]   = { "mAnkleLeft", "mFootLeft", "mAnkleRight", "mFootRight" };

        // k is the ratio of the rest heights of the hip joints' middle; outside this the skeleton is not a body.
        constexpr F32 HIP_SCALE_MIN = 0.1f;
        constexpr F32 HIP_SCALE_MAX = 10.f;

        /// The column-vector product a b of SOMA's formulas (first b, then a), in the viewer's order.
        inline LLQuaternion col(const LLQuaternion& a, const LLQuaternion& b)
        {
            return b * a;
        }

        // C turns SL's body frame (x, y, z) into SOMA's aligned frame (x, z, -y); on a rotation (C q C^-1) it
        // turns the axis the same way.
        inline LLVector3 vecToSoma(const LLVector3& v)
        {
            return LLVector3(v.mV[VX], v.mV[VZ], -v.mV[VY]);
        }

        inline LLQuaternion rotToSoma(const LLQuaternion& q)
        {
            return LLQuaternion(q.mQ[VX], q.mQ[VZ], -q.mQ[VY], q.mQ[VW]);
        }

        inline LLQuaternion rotToSl(const LLQuaternion& q)
        {
            return LLQuaternion(q.mQ[VX], -q.mQ[VZ], q.mQ[VY], q.mQ[VW]);
        }

        inline LLQuaternion fromWxyz(const float (&q)[4])
        {
            return LLQuaternion(q[1], q[2], q[3], q[0]);
        }

        inline LLVector3 fromXyz(const float (&v)[3])
        {
            return LLVector3(v[0], v[1], v[2]);
        }

        S32 somaIndex(const char* name)
        {
            for (S32 i = 0; i < SOMA_JOINT_COUNT; ++i)
            {
                if (std::strcmp(soma::REST[i].name, name) == 0)
                {
                    return i;
                }
            }
            return -1;
        }

        /// A rotation of the input: Ok and the unit rotation, or why not.
        inline RetargetResult readRotation(const LLQuaternion& q, LLQuaternion& out)
        {
            if (!q.isFinite())
            {
                return RetargetResult::NotFinite;
            }
            return normalizedRotation(q, out) ? RetargetResult::Ok : RetargetResult::BadInput;
        }

        inline RetargetResult readPosition(const LLVector3& v)
        {
            if (!v.isFinite())
            {
                return RetargetResult::NotFinite;
            }
            return validPosition(v) ? RetargetResult::Ok : RetargetResult::BadInput;
        }
    } // namespace

    const char* describe(RetargetResult result)
    {
        switch (result)
        {
            case RetargetResult::Ok:          return "ok";
            case RetargetResult::BadSkeleton: return "bad rest skeleton";
            case RetargetResult::BadInput:    return "bad input";
            case RetargetResult::NotFinite:   return "not finite";
        }
        return "unknown";
    }

    F32 rotationAngle(const LLQuaternion& a, const LLQuaternion& b)
    {
        const LLQuaternion d = ~a * b;
        const F64 v = std::sqrt((F64)d.mQ[VX] * d.mQ[VX] + (F64)d.mQ[VY] * d.mQ[VY] + (F64)d.mQ[VZ] * d.mQ[VZ]);
        return (F32)(2.0 * std::atan2(v, std::fabs((F64)d.mQ[VW])));
    }

    bool validPosition(const LLVector3& v)
    {
        return v.isFinite() && std::fabs(v.mV[VX]) <= MAX_POSITION_M && std::fabs(v.mV[VY]) <= MAX_POSITION_M &&
               std::fabs(v.mV[VZ]) <= MAX_POSITION_M;
    }

    bool normalizedRotation(const LLQuaternion& q, LLQuaternion& out)
    {
        if (!q.isFinite())
        {
            return false;
        }
        const F64 len = std::sqrt((F64)q.mQ[VX] * q.mQ[VX] + (F64)q.mQ[VY] * q.mQ[VY] + (F64)q.mQ[VZ] * q.mQ[VZ] +
                                  (F64)q.mQ[VW] * q.mQ[VW]);
        if (!(len >= QUAT_LENGTH_MIN && len <= QUAT_LENGTH_MAX))
        {
            return false;
        }
        out = LLQuaternion((F32)(q.mQ[VX] / len), (F32)(q.mQ[VY] / len), (F32)(q.mQ[VZ] / len), (F32)(q.mQ[VW] / len));
        return true;
    }

    RetargetResult SomaRetarget::init(const RestSkeleton& rest)
    {
        // made in a new object and moved in only when every step passed: a failure leaves no half state
        SomaRetarget next;
        if (!(next.initSl(rest) && next.initSoma() && next.initPairs()))
        {
            *this = SomaRetarget();
            return RetargetResult::BadSkeleton;
        }
        next.mReady = true;
        *this       = std::move(next);
        return RetargetResult::Ok;
    }

    const RestJoint& SomaRetarget::joint(S32 index) const
    {
        static const RestJoint NONE;
        llassert(index >= 0 && index < (S32)mRest.size());
        return index >= 0 && index < (S32)mRest.size() ? mRest[index] : NONE;
    }

    bool SomaRetarget::initSl(const RestSkeleton& rest)
    {
        // order, names, sizes, and the rest positions in the body frame
        const S32 n = (S32)rest.joints.size();
        if (n < 1 || n > MAX_REST_JOINTS)
        {
            return false;
        }
        std::unordered_set<std::string> names;
        std::vector<LLVector3> rest_pos(n);
        S32 roots = 0;
        for (S32 i = 0; i < n; ++i)
        {
            const RestJoint& j = rest.joints[i];
            if (j.parent < -1 || j.parent >= i || j.name.empty() || !names.insert(j.name).second)
            {
                return false;
            }
            if (!validPosition(j.pos) || !validPosition(j.scale) ||
                !(j.scale.mV[VX] > 0.f && j.scale.mV[VY] > 0.f && j.scale.mV[VZ] > 0.f))
            {
                return false;
            }
            if (j.parent < 0)
            {
                ++roots;
                rest_pos[i] = j.pos;
            }
            else
            {
                rest_pos[i] = rest_pos[j.parent] + j.pos.scaledVec(rest.joints[j.parent].scale);
            }
        }
        mRest    = rest.joints;
        mRestPos = std::move(rest_pos);
        const S32 pelvis = find("mPelvis");
        if (roots != 1 || pelvis < 0 || mRest[pelvis].parent != -1)
        {
            return false;
        }
        mPelvisRest = mRestPos[pelvis];
        return true;
    }

    bool SomaRetarget::initSoma()
    {
        // the table as viewer types
        for (S32 i = 0; i < SOMA_JOINT_COUNT; ++i)
        {
            const soma::JointRest& r = soma::REST[i];
            if (r.parent < -1 || r.parent >= i || (r.parent < 0) != (i == 0))
            {
                return false;
            }
            mSomaParent[i]    = r.parent;
            mSomaOffset[i]    = fromXyz(r.offset);
            mSomaRestLocal[i] = fromWxyz(r.rest_local);
            mSomaRestWorld[i] = fromWxyz(r.rest_world);
        }
        return true;
    }

    bool SomaRetarget::initPairs()
    {
        // the pairs; the model's root is the pelvis
        mSlOfSoma.fill(-1);
        mDrivenSoma.assign(mRest.size(), -1);
        for (const Pair& p : PAIRS)
        {
            const S32 s = somaIndex(p.soma);
            const S32 l = find(p.sl);
            if (s < 0 || l < 0)
            {
                return false;
            }
            mSlOfSoma[s]   = l;
            mDrivenSoma[l] = s;
        }
        if (mSlOfSoma[0] != find("mPelvis"))
        {
            return false;
        }

        // the soles, and k from the hip joints' middle
        for (S32 k = 0; k < SOLE_POINTS; ++k)
        {
            mSoleSl[k]   = find(SOLE_SL[k]);
            mSoleSoma[k] = somaIndex(SOLE_SOMA[k]);
            if (mSoleSl[k] < 0 || mSoleSoma[k] < 0)
            {
                return false;
            }
            mSoleSomaRestY[k] = soma::REST[mSoleSoma[k]].rest_pos[1];
        }
        const S32 hip_l = find("mHipLeft");
        const S32 hip_r = find("mHipRight");
        const S32 leg_l = somaIndex("LeftLeg");
        const S32 leg_r = somaIndex("RightLeg");
        if (hip_l < 0 || hip_r < 0 || leg_l < 0 || leg_r < 0)
        {
            return false;
        }
        const F32 sl_mid   = 0.5f * (mRestPos[hip_l].mV[VZ] + mRestPos[hip_r].mV[VZ]);
        const F32 soma_mid = 0.5f * (soma::REST[leg_l].rest_pos[1] + soma::REST[leg_r].rest_pos[1]);
        if (!(soma_mid > 0.f))
        {
            return false;
        }
        mK = sl_mid / soma_mid;
        return std::isfinite(mK) && mK >= HIP_SCALE_MIN && mK <= HIP_SCALE_MAX;
    }

    S32 SomaRetarget::find(const std::string& name) const
    {
        for (S32 i = 0; i < (S32)mRest.size(); ++i)
        {
            if (mRest[i].name == name)
            {
                return i;
            }
        }
        return -1;
    }

    RetargetResult SomaRetarget::slToSoma(const SlPose& pose, SomaPose& out, RetargetWork& work) const
    {
        const S32 n = jointCount();
        if (!mReady || (S32)pose.local.size() != n)
        {
            return mReady ? RetargetResult::BadInput : RetargetResult::BadSkeleton;
        }
        RetargetResult r = readPosition(pose.pelvis);
        if (r != RetargetResult::Ok)
        {
            return r;
        }

        // SL world rotations, parents first
        work.world.resize(n);
        for (S32 i = 0; i < n; ++i)
        {
            LLQuaternion q;
            if ((r = readRotation(pose.local[i], q)) != RetargetResult::Ok)
            {
                return r;
            }
            const S32 p = mRest[i].parent;
            work.world[i] = p < 0 ? q : q * work.world[p];
        }

        // SOMA world rotations: the paired joints carry SL's turn away from the rest pose, the rest keep their
        // rest rotation to the parent
        SomaPose res;
        for (S32 s = 0; s < SOMA_JOINT_COUNT; ++s)
        {
            const S32 l = mSlOfSoma[s];
            res.rot[s] = l >= 0 ? col(rotToSoma(work.world[l]), mSomaRestWorld[s]) : col(res.rot[mSomaParent[s]], mSomaRestLocal[s]);
        }

        // positions with the hips at 0, then the feet on the floor and the hips across at SL's pelvis / k
        res.pos[0].setZero();
        for (S32 s = 1; s < SOMA_JOINT_COUNT; ++s)
        {
            const S32 p = mSomaParent[s];
            res.pos[s] = res.pos[p] + mSomaOffset[s] * res.rot[p];
        }
        F32 sole = F32_MAX;
        for (S32 k = 0; k < SOLE_POINTS; ++k)
        {
            sole = llmin(sole, res.pos[mSoleSoma[k]].mV[VY] - mSoleSomaRestY[k]);
        }
        const LLVector3 across = vecToSoma(pose.pelvis) / mK;
        const LLVector3 hips(across.mV[VX], -sole, across.mV[VZ]);
        for (S32 s = 0; s < SOMA_JOINT_COUNT; ++s)
        {
            res.pos[s] += hips;
            if (!res.pos[s].isFinite() || !res.rot[s].isFinite())
            {
                return RetargetResult::NotFinite;
            }
        }
        out = res;
        return RetargetResult::Ok;
    }

    RetargetResult SomaRetarget::somaToSl(const SomaFrame& frame, SlPose& io, RetargetWork& work) const
    {
        const S32 n = jointCount();
        if (!mReady || (S32)io.local.size() != n)
        {
            return mReady ? RetargetResult::BadInput : RetargetResult::BadSkeleton;
        }
        RetargetResult r = readPosition(frame.hips);
        if (r != RetargetResult::Ok)
        {
            return r;
        }

        // SOMA world rotations and positions
        PerSoma<LLQuaternion> world;
        PerSoma<LLVector3>    pos;
        for (S32 s = 0; s < SOMA_JOINT_COUNT; ++s)
        {
            LLQuaternion q;
            if ((r = readRotation(frame.local[s], q)) != RetargetResult::Ok)
            {
                return r;
            }
            const S32 p = mSomaParent[s];
            world[s] = p < 0 ? q : col(world[p], q);
            pos[s]   = p < 0 ? frame.hips : pos[p] + mSomaOffset[s] * world[p];
        }

        // SL, parents first: a paired joint gets the world rotation SOMA asks for, as a local rotation to the
        // parent as it stands (the joints in between keep the caller's rotations); positions with the pelvis
        // at its rest, for the soles
        work.world.resize(n);
        work.pos.resize(n);
        PerSoma<LLQuaternion> driven_local; // by SOMA joint
        for (S32 i = 0; i < n; ++i)
        {
            const S32 p = mRest[i].parent;
            const S32 s = mDrivenSoma[i];
            if (s >= 0)
            {
                const LLQuaternion target = rotToSl(col(world[s], ~mSomaRestWorld[s]));
                driven_local[s] = p < 0 ? target : target * ~work.world[p];
                work.world[i]   = target;
            }
            else
            {
                LLQuaternion q;
                if ((r = readRotation(io.local[i], q)) != RetargetResult::Ok)
                {
                    return r;
                }
                work.world[i] = p < 0 ? q : q * work.world[p];
            }
            work.pos[i] = p < 0 ? mPelvisRest : work.pos[p] + mRest[i].pos.scaledVec(mRest[p].scale) * work.world[p];
        }

        // the pelvis: across at SOMA's hips * k; up so that SL's soles are as high as SOMA's (* k)
        F32 soma_sole = F32_MAX;
        F32 sl_sole   = F32_MAX;
        for (S32 k = 0; k < SOLE_POINTS; ++k)
        {
            soma_sole = llmin(soma_sole, pos[mSoleSoma[k]].mV[VY] - mSoleSomaRestY[k]);
            const S32 l = mSoleSl[k];
            sl_sole = llmin(sl_sole, work.pos[l].mV[VZ] - mRestPos[l].mV[VZ]);
        }
        const LLVector3 pelvis(frame.hips.mV[VX] * mK, -frame.hips.mV[VZ] * mK, mPelvisRest.mV[VZ] + soma_sole * mK - sl_sole);
        if (!pelvis.isFinite())
        {
            return RetargetResult::NotFinite;
        }
        for (S32 s = 0; s < SOMA_JOINT_COUNT; ++s)
        {
            if (mSlOfSoma[s] >= 0 && !driven_local[s].isFinite())
            {
                return RetargetResult::NotFinite;
            }
        }

        for (S32 s = 0; s < SOMA_JOINT_COUNT; ++s)
        {
            if (mSlOfSoma[s] >= 0)
            {
                io.local[mSlOfSoma[s]] = driven_local[s];
            }
        }
        io.pelvis = pelvis;
        return RetargetResult::Ok;
    }

    void SomaRetarget::localsOf(const SomaPose& pose, SomaFrame& out)
    {
        out.hips     = pose.pos[0];
        out.local[0] = pose.rot[0];
        for (S32 s = 1; s < SOMA_JOINT_COUNT; ++s)
        {
            out.local[s] = pose.rot[s] * ~pose.rot[soma::REST[s].parent]; // parent^-1 * world, column vectors
        }
    }
} // namespace plus::posermotion

/**
 * @file gaze.cpp
 * @brief Camera gaze: make avatars look at the camera in this viewer only.
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

// How it works
// ------------
// LLVOAvatar::updateCharacter() blends the animations into the joints
// (LLMotionController::updateMotions -> LLPoseBlender::blendAndApply). Right
// after that, apply() writes the neck, head and eye rotations: whatever the
// animations, the AO or the official look-at (LLHeadRotMotion / LLEyeMotion,
// MEDIUM_PRIORITY) left there is replaced, so nothing can win over it, and the
// eyes' own jitter and look-away (which only add to the eye rotation LLEyeMotion
// writes) are gone with it. Blinks are morphs and stay.
//
// The split between eyes and head (all angles measured from where the head
// points, in the head's own frame):
//   1. the eyes turn up to PlusGazeEyeLimit degrees sideways and only up to
//      PlusGazePitchLimit up or down (more shows the white of the eye);
//   2. what is left, per axis, is added to the head, at most PlusGazeHeadMax
//      sideways and PlusGazeHeadPitchMax up or down,
//      shared between the neck (PlusGazeNeckShare) and the head joint. The torso
//      is never touched;
//   3. the eyes then aim at the camera from the head's new direction (both
//      along the direction from between them: parallel, never cross-eyed), limited again.
// Past the combined reach the gaze fades back to the pose over 30 degrees
// (camera behind the avatar: the eyes look forward as usual).
// So a camera inside the eyes' range moves only the eyes, and one further out
// moves the head by the difference and no more; behind or above the limits the
// eyes and head just stop at the edge. For heads whose eyes do not move (mesh
// heads), set the eye limit low and the head limit high.
//
// The camera point is followed with a lag (PlusGazeLag seconds, exponential),
// in the world, so a moving camera is not tracked instantly. The animations
// rewrite a joint every frame only when some animation drives it; for the others
// we remember what we wrote and the pose underneath it, and undo ours before the
// next frame (or when the mode goes off), so that nothing piles up.

#include "llviewerprecompiledheaders.h"

#include "gaze.h"

#include "llagentcamera.h"
#include "llcharacter.h"
#include "llframetimer.h"
#include "lljoint.h"
#include "llquaternion.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llvoavatar.h"

#include <cmath>
#include <unordered_map>

namespace plus::gaze
{
    namespace
    {
        constexpr char SETTING_MODE[]       = "PlusGazeMode";
        constexpr char SETTING_EYE_LIMIT[]  = "PlusGazeEyeLimit";
        constexpr char SETTING_HEAD_MAX[]   = "PlusGazeHeadMax";
        constexpr char SETTING_PITCH_LIMIT[] = "PlusGazePitchLimit";
        constexpr char SETTING_HEAD_PITCH_MAX[] = "PlusGazeHeadPitchMax";
        constexpr char SETTING_NECK_SHARE[] = "PlusGazeNeckShare";
        constexpr char SETTING_LAG[]        = "PlusGazeLag";
        constexpr char SETTING_DIAG[]       = "PlusGazeDiag";
        constexpr char SETTING_ALLOW_EVERYONE[] = "PlusGazeAllowEveryone";
        constexpr char SETTING_FOV_ADAPTIVE[] = "PlusGazeFovAdaptive";
        constexpr char SETTING_HEAD_MAX_ABS[] = "PlusGazeHeadMaxAbs";
        constexpr F32  FOV_MARGIN = 5.f * DEG_TO_RAD;   // reach beyond the picture's edge
        constexpr char SETTING_CONVERGENCE[] = "PlusGazeConvergence";
        constexpr char SETTING_BIAS_L[]     = "PlusGazeEyeBiasL";
        constexpr char SETTING_BIAS_R[]     = "PlusGazeEyeBiasR";
        constexpr F64  DIAG_INTERVAL        = 3.0;   // s between diagnostic lines

        // Past the reach of eyes and head the gaze fades back to the pose over this
        // many radians, instead of pinning the eyes at their limit (a camera behind
        // the avatar would otherwise swing the eyes hard to one side).
        constexpr F32 FADE_WIDTH = 30.f * DEG_TO_RAD;

        constexpr F32 MIN_CAMERA_DISTANCE = 0.25f;   // m from the eyes: the camera is in the face, leave it
        constexpr F32 MAX_STEP_TIME       = 0.5f;    // s: a longer gap (hidden, lagging) just snaps
        constexpr U32 SWEEP_INTERVAL      = 600;     // frames between sweeps of the state map
        constexpr U32 STALE_FRAMES        = 300;     // frames an avatar may go unseen before its state is dropped

        enum Slot : int { NECK, HEAD, EYE_L, EYE_R, ALT_L, ALT_R, SLOT_COUNT };

        // What we wrote into one joint last time and the pose that was there.
        struct Written
        {
            LLQuaternion base;      // the rotation before we changed it
            LLQuaternion written;   // the rotation we set
            bool         valid = false;
        };

        struct State
        {
            Written    slot[SLOT_COUNT];
            LLVector3  target;          // the smoothed camera point, in render space
            F64        last_time = 0.0;
            bool       have_target = false;
            U32        last_frame = 0;
        };

        std::unordered_map<const LLVOAvatar*, State>& states()
        {
            static std::unordered_map<const LLVOAvatar*, State> sStates;
            return sStates;
        }

        // Puts the joint back to the pose when the animations did not rewrite
        // it since we changed it; either way records what is there now as the base.
        void takeBase(Written& w, LLJoint* joint)
        {
            if (!joint)
            {
                w.valid = false;
                return;
            }
            if (w.valid && joint->getRotation() == w.written)
            {
                joint->setRotation(w.base);
            }
            else
            {
                w.base = joint->getRotation();
            }
            w.valid = false;
        }

        void commit(Written& w, LLJoint* joint, const LLQuaternion& rot)
        {
            if (!joint)
            {
                return;
            }
            joint->setRotation(rot);
            w.written = joint->getRotation();
            w.valid = true;
        }

        // Yaw and pitch (no roll) that turn the +X axis, where an eye or the
        // head looks, to dir_local: the way LLEyeMotion::adjustEyeTarget() does.
        void yawPitch(const LLVector3& dir_local, F32& pitch, F32& yaw)
        {
            const LLVector3 up(0.f, 0.f, 1.f);
            LLVector3 left = up % dir_local;
            if (left.normVec() < 1.0e-4f)
            {
                left.set(0.f, 1.f, 0.f);  // straight up or down
            }
            LLVector3 new_up = dir_local % left;
            LLQuaternion q(dir_local, left, new_up);
            F32 roll;
            q.getEulerAngles(&roll, &pitch, &yaw);
        }

        bool applies(LLVOAvatar* avatar, S32 mode)
        {
            if (mode == MODE_OFF)
            {
                return false;
            }
            if (mode == MODE_SELF && !avatar->isSelf())
            {
                return false;
            }
            if (avatar->isSelf() && gAgentCamera.cameraMouselook())
            {
                return false;
            }
            return avatar->mNeckp && avatar->mHeadp && avatar->mEyeLeftp && avatar->mEyeRightp
                && avatar->mHeadp->getParent() == avatar->mNeckp && avatar->mNeckp->getParent();
        }

        void sweep(U32 frame)
        {
            static U32 sLastSweep = 0;
            if (frame - sLastSweep < SWEEP_INTERVAL)
            {
                return;
            }
            sLastSweep = frame;
            auto& map = states();
            for (auto it = map.begin(); it != map.end();)
            {
                it = (frame - it->second.last_frame > STALE_FRAMES) ? map.erase(it) : std::next(it);
            }
        }

        // True once per DIAG_INTERVAL for your own avatar while PlusGazeDiag is on.
        bool diagDue(LLVOAvatar* avatar)
        {
            static LLCachedControl<bool> diag(gSavedSettings, SETTING_DIAG, false);
            static F64 sLast = 0.0;
            if (!diag || !avatar->isSelf())
            {
                return false;
            }
            const F64 now = LLFrameTimer::getElapsedSeconds();
            if (now - sLast < DIAG_INTERVAL)
            {
                return false;
            }
            sLast = now;
            return true;
        }

        void eulerDeg(const LLQuaternion& q, F32& pitch, F32& yaw)
        {
            F32 roll;
            q.getEulerAngles(&roll, &pitch, &yaw);
            pitch *= RAD_TO_DEG;
            yaw   *= RAD_TO_DEG;
        }

        // Angle in degrees between a joint's +X axis (world) and a world direction.
        F32 forwardError(LLJoint* joint, LLVector3 to_target)
        {
            to_target.normVec();
            LLVector3 fwd = LLVector3(1.f, 0.f, 0.f) * joint->getWorldRotation();
            return acosf(llclamp(fwd * to_target, -1.f, 1.f)) * RAD_TO_DEG;
        }

        // The mode as it counts: "everyone" is only honoured while PlusGazeAllowEveryone
        // is on (off in public builds: changing how other people's avatars look to
        // you is not what a third-party viewer's policy allows); else it is "self only".
        S32 effectiveMode(S32 mode)
        {
            static LLCachedControl<bool> allow_everyone(gSavedSettings, SETTING_ALLOW_EVERYONE, false);
            mode = llclamp(mode, (S32)MODE_OFF, (S32)MODE_ALL);
            return (mode == MODE_ALL && !allow_everyone) ? (S32)MODE_SELF : mode;
        }

        // Undoes our change on the avatar and forgets it.
        void release(LLVOAvatar* avatar)
        {
            auto& map = states();
            auto it = map.find(avatar);
            if (it == map.end())
            {
                return;
            }
            State& st = it->second;
            LLJoint* joints[SLOT_COUNT] = { avatar->mNeckp, avatar->mHeadp, avatar->mEyeLeftp, avatar->mEyeRightp,
                                            avatar->getJoint("mFaceEyeAltLeft"), avatar->getJoint("mFaceEyeAltRight") };
            for (int i = 0; i < SLOT_COUNT; ++i)
            {
                takeBase(st.slot[i], joints[i]);
            }
            map.erase(it);
        }
    }

    Mode getMode()
    {
        return (Mode)effectiveMode(gSavedSettings.getS32(SETTING_MODE));
    }

    void setMode(Mode mode)
    {
        gSavedSettings.setS32(SETTING_MODE, effectiveMode((S32)mode));
    }

    void apply(LLVOAvatar* avatar)
    {
        static LLCachedControl<S32> mode_setting(gSavedSettings, SETTING_MODE, MODE_OFF);
        auto& map = states();
        const S32 mode = effectiveMode(mode_setting);
        if (mode == MODE_OFF && map.empty())
        {
            return;
        }

        const U32 frame = LLFrameTimer::getFrameCount();
        sweep(frame);

        if (!avatar || !applies(avatar, mode))
        {
            if (avatar)
            {
                release(avatar);
            }
            return;
        }

        static LLCachedControl<F32> eye_limit_setting(gSavedSettings, SETTING_EYE_LIMIT, 14.f);
        static LLCachedControl<F32> head_max_setting(gSavedSettings, SETTING_HEAD_MAX, 25.f);
        static LLCachedControl<F32> pitch_limit_setting(gSavedSettings, SETTING_PITCH_LIMIT, 11.f);
        static LLCachedControl<F32> head_pitch_max_setting(gSavedSettings, SETTING_HEAD_PITCH_MAX, 10.f);
        static LLCachedControl<F32> convergence_setting(gSavedSettings, SETTING_CONVERGENCE, 0.6f);
        static LLCachedControl<F32> bias_l_setting(gSavedSettings, SETTING_BIAS_L, 0.f);
        static LLCachedControl<F32> bias_r_setting(gSavedSettings, SETTING_BIAS_R, 0.f);
        static LLCachedControl<bool> fov_adaptive_setting(gSavedSettings, SETTING_FOV_ADAPTIVE, true);
        static LLCachedControl<F32> head_max_abs_setting(gSavedSettings, SETTING_HEAD_MAX_ABS, 40.f);
        static LLCachedControl<F32> neck_share_setting(gSavedSettings, SETTING_NECK_SHARE, 0.4f);
        static LLCachedControl<F32> lag_setting(gSavedSettings, SETTING_LAG, 0.15f);
        const F32 eye_limit  = llclamp((F32)eye_limit_setting, 0.f, 60.f) * DEG_TO_RAD;
        F32 head_max         = llclamp((F32)head_max_setting, 0.f, 60.f) * DEG_TO_RAD;
        const F32 pitch_limit    = llclamp((F32)pitch_limit_setting, 0.f, 40.f) * DEG_TO_RAD;
        F32 head_pitch_max   = llclamp((F32)head_pitch_max_setting, 0.f, 60.f) * DEG_TO_RAD;
        if (fov_adaptive_setting)
        {
            // Someone standing straight at the edge of the picture sees the lens
            // half the field of view off to the side: reach at least that far (the
            // eyes' own limits stay; the head makes up the difference, up to
            // PlusGazeHeadMaxAbs; up and down only half of that).
            const LLViewerCamera* cam = LLViewerCamera::getInstance();
            const F32 half_v = cam->getView() * 0.5f;
            const F32 half_h = atanf(tanf(half_v) * cam->getAspect());
            const F32 abs_max = llclamp((F32)head_max_abs_setting, 0.f, 60.f) * DEG_TO_RAD;
            head_max       = llmax(head_max,       llmin(half_h + FOV_MARGIN - eye_limit,   abs_max));
            head_pitch_max = llmax(head_pitch_max, llmin(half_v + FOV_MARGIN - pitch_limit, abs_max * 0.5f));
        }
        const F32 convergence = llclamp((F32)convergence_setting, 0.f, 1.f);
        const F32 bias_l = llclamp((F32)bias_l_setting, -20.f, 20.f) * DEG_TO_RAD;
        const F32 bias_r = llclamp((F32)bias_r_setting, -20.f, 20.f) * DEG_TO_RAD;
        const F32 neck_share = llclamp((F32)neck_share_setting, 0.f, 1.f);
        const F32 lag        = llclamp((F32)lag_setting, 0.f, 1.f);

        LLJoint* neck  = avatar->mNeckp;
        LLJoint* head  = avatar->mHeadp;
        LLJoint* eye_l = avatar->mEyeLeftp;
        LLJoint* eye_r = avatar->mEyeRightp;
        LLJoint* joints[SLOT_COUNT] = { neck, head, eye_l, eye_r,
                                        avatar->getJoint("mFaceEyeAltLeft"), avatar->getJoint("mFaceEyeAltRight") };

        State& st = map[avatar];
        st.last_frame = frame;
        const bool diag = diagDue(avatar);

        // Back to the pose first: the head's frame below is the animated one.
        for (int i = 0; i < SLOT_COUNT; ++i)
        {
            takeBase(st.slot[i], joints[i]);
        }

        // The camera point, followed with a lag.
        const LLVector3 camera = LLViewerCamera::getInstance()->getOrigin();
        const F64 now = LLFrameTimer::getElapsedSeconds();
        const F32 dt = (F32)(now - st.last_time);
        st.last_time = now;
        if (!st.have_target || dt > MAX_STEP_TIME || lag < 0.001f)
        {
            st.target = camera;
            st.have_target = true;
        }
        else if (dt > 0.f)
        {
            st.target += (camera - st.target) * (1.f - expf(-dt / lag));
        }

        const LLVector3 pos_l = eye_l->getWorldPosition();
        const LLVector3 pos_r = eye_r->getWorldPosition();
        const LLVector3 mid = (pos_l + pos_r) * 0.5f;
        LLVector3 to_mid = st.target - mid;
        if (to_mid.magVec() < MIN_CAMERA_DISTANCE)
        {
            if (diag)
            {
                LL_INFOS("Gaze") << "skipped: camera inside the face (" << to_mid.magVec() << " m)" << LL_ENDL;
            }
            return;     // rotations stay as the pose has them
        }

        // Everything below is in the head's frame (x forward, y left, z up).
        const LLQuaternion head_world     = head->getWorldRotation();
        const LLQuaternion head_world_inv = ~head_world;
        const F32 camera_distance = to_mid.normalize();    // the length before (for the diagnostic log)
        const LLVector3 v_mid = to_mid * head_world_inv;

        // The head takes what the eyes cannot reach.
        F32 pitch, yaw;
        yawPitch(v_mid, pitch, yaw);
        // Yaw and pitch each have their own eye limit and head share: what the eyes
        // cannot reach on one axis goes to the head on that axis only.
        const F32 over_yaw   = fabsf(yaw)   - (eye_limit   + head_max);
        const F32 over_pitch = fabsf(pitch) - (pitch_limit + head_pitch_max);
        const F32 t = llclamp(1.f - llmax(over_yaw, over_pitch) / FADE_WIDTH, 0.f, 1.f);
        const F32 weight = t * t * (3.f - 2.f * t);     // 1 within reach, 0 well beyond it
        if (weight <= 0.f)
        {
            if (diag)
            {
                LL_INFOS("Gaze") << "skipped: out of reach, yaw " << yaw * RAD_TO_DEG << " pitch " << pitch * RAD_TO_DEG
                                 << " deg from the head's forward" << LL_ENDL;
            }
            return;     // out of reach: the pose as it is
        }
        const F32 head_yaw   = llclamp(fabsf(yaw)   - eye_limit,   0.f, head_max)       * weight * (yaw   < 0.f ? -1.f : 1.f);
        const F32 head_pitch = llclamp(fabsf(pitch) - pitch_limit, 0.f, head_pitch_max) * weight * (pitch < 0.f ? -1.f : 1.f);
        LLQuaternion head_turn;     // in the head's frame; identity unless needed
        if (fabsf(head_yaw) > 1.0e-4f || fabsf(head_pitch) > 1.0e-4f)
        {
            head_turn.setQuat(0.f, head_pitch, head_yaw);

            // As a rotation of the world, appended to the joints' world rotations:
            // the neck first by its share, the head joint by the rest (same axis).
            const LLQuaternion turn_world = head_world_inv * head_turn * head_world;
            F32 turn_angle;
            LLVector3 axis;
            turn_world.getAngleAxis(&turn_angle, axis);
            const LLQuaternion turn_neck(turn_angle * neck_share, axis);
            const LLQuaternion turn_head(turn_angle * (1.f - neck_share), axis);

            const LLQuaternion neck_world   = neck->getWorldRotation();
            const LLQuaternion parent_world = neck->getParent()->getWorldRotation();
            commit(st.slot[NECK], neck, neck->getRotation() * (parent_world * turn_neck * ~parent_world));
            commit(st.slot[HEAD], head, head->getRotation() * (neck_world * turn_head * ~neck_world));
        }

        // Each eye aims at the camera from its own place, in the frame of its own
        // parent, after the head turn (the neck and head were just rewritten, so the
        // world positions and rotations asked for here are the new ones). Aiming them
        // in parallel would look wall-eyed close up: at 0.6 m the eyes' 6 cm apart is
        // 3 degrees each.
        struct Aim { F32 want_yaw, want_pitch; LLQuaternion rot; };
        const LLVector3 mid_now = (eye_l->getWorldPosition() + eye_r->getWorldPosition()) * 0.5f;
        auto aim = [&](Slot slot, LLJoint* joint, F32 bias) -> Aim
        {
            Aim a;
            const LLQuaternion parent_inv = ~joint->getParent()->getWorldRotation();
            LLVector3 local = st.target - joint->getWorldPosition();
            local.normVec();
            local = local * parent_inv;
            F32 p, y;
            yawPitch(local, p, y);                  // from this eye's own place
            LLVector3 par = st.target - mid_now;
            par.normVec();
            par = par * parent_inv;
            F32 pp, py;
            yawPitch(par, pp, py);                  // parallel with the other eye (from between them)
            y = py + (y - py) * convergence;
            p = pp + (p - pp) * convergence;
            y += bias;
            a.want_yaw = y;
            a.want_pitch = p;
            LLQuaternion q;
            q.setQuat(0.f, llclamp(p, -pitch_limit, pitch_limit), llclamp(y, -eye_limit, eye_limit));
            a.rot = nlerp(weight, st.slot[slot].base, q);
            return a;
        };
        const Aim aim_l = aim(EYE_L, eye_l, bias_l);
        const Aim aim_r = aim(EYE_R, eye_r, bias_r);
        commit(st.slot[EYE_L], eye_l, aim_l.rot);
        commit(st.slot[EYE_R], eye_r, aim_r.rot);
        Aim alt_l = aim_l, alt_r = aim_r;
        if (joints[ALT_L] && joints[ALT_L]->getParent())
        {
            alt_l = aim(ALT_L, joints[ALT_L], bias_l);
            commit(st.slot[ALT_L], joints[ALT_L], alt_l.rot);
        }
        if (joints[ALT_R] && joints[ALT_R]->getParent())
        {
            alt_r = aim(ALT_R, joints[ALT_R], bias_r);
            commit(st.slot[ALT_R], joints[ALT_R], alt_r.rot);
        }

        if (diag)
        {
            // Per eye: where it should point (yaw/pitch in its parent's frame; + yaw is
            // toward the avatar's left), what was written, and the angle left between the
            // eye bone's +X and the camera. Then the head and the alt (mesh head) bones.
            auto line = [&](const char* name, LLJoint* joint, const Aim& a, const Written& w)
            {
                F32 wp, wy, bp, by;
                eulerDeg(joint->getRotation(), wp, wy);
                eulerDeg(w.base, bp, by);
                const bool hit = fabsf(a.want_yaw) >= eye_limit - 1.0e-4f || fabsf(a.want_pitch) >= pitch_limit - 1.0e-4f;
                LL_INFOS("Gaze") << name << ": wanted yaw " << a.want_yaw * RAD_TO_DEG << " pitch " << a.want_pitch * RAD_TO_DEG
                    << " | written yaw " << wy << " pitch " << wp << (hit ? " (HIT limit)" : "")
                    << " | pose before yaw " << by << " pitch " << bp
                    << " | bone +X to camera " << forwardError(joint, st.target - joint->getWorldPosition()) << " deg"
                    << " | parent " << joint->getParent()->getName() << LL_ENDL;
            };
            LL_INFOS("Gaze") << "everyone allowed: " << (gSavedSettings.getBOOL(SETTING_ALLOW_EVERYONE) ? "yes" : "no (mode 2 counts as 1)")
                << ", dist " << camera_distance << " m, weight " << weight << ", convergence " << convergence << ", bias L " << bias_l * RAD_TO_DEG << " R " << bias_r * RAD_TO_DEG << ", camera on the head's "
                << (yaw >= 0.f ? "LEFT" : "RIGHT")
                << " | target vs head fwd: yaw " << yaw * RAD_TO_DEG << " pitch " << pitch * RAD_TO_DEG
                << " | head turn: yaw " << head_yaw * RAD_TO_DEG << " pitch " << head_pitch * RAD_TO_DEG
                << " | limits: eye yaw " << eye_limit * RAD_TO_DEG << " pitch " << pitch_limit * RAD_TO_DEG << ", head yaw " << head_max * RAD_TO_DEG << " pitch " << head_pitch_max * RAD_TO_DEG
                << " | head +X to camera " << forwardError(head, st.target - mid) << " deg" << LL_ENDL;
            line("eye L", eye_l, aim_l, st.slot[EYE_L]);
            line("eye R", eye_r, aim_r, st.slot[EYE_R]);
            if (joints[ALT_L] && joints[ALT_R] && joints[ALT_L]->getParent() && joints[ALT_R]->getParent())
            {
                line("alt eye L (mesh head)", joints[ALT_L], alt_l, st.slot[ALT_L]);
                line("alt eye R (mesh head)", joints[ALT_R], alt_r, st.slot[ALT_R]);
            }
        }
    }
}

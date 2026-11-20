/**
 * @file motionclip.cpp
 * @brief "Motion Between Poses": the clip and the time of the playback (see motionclip.h).
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

#include "motionclip.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace plus::posermotion
{
    namespace
    {
        /// More extra tracks than this are refused (the poser has about 30 collision volumes and attachment points).
        constexpr S32 MAX_EXTRA_TRACKS = 1024;
        constexpr F64 PI               = 3.14159265358979323846;

        /// q to the power w, the short way round (sl_roundtrip.rotvec_pow): the same axis, w times the angle.
        LLQuaternion rotPow(const LLQuaternion& q, F64 w)
        {
            const F64 sign = q.mQ[VW] < 0.f ? -1.0 : 1.0;
            const F64 x = sign * q.mQ[VX], y = sign * q.mQ[VY], z = sign * q.mQ[VZ], qw = sign * q.mQ[VW];
            const F64 v = std::sqrt(x * x + y * y + z * z);
            if (v < 1e-12)
            {
                return LLQuaternion::DEFAULT;
            }
            const F64 half = std::atan2(v, qw) * w; // half the new angle
            const F64 s    = std::sin(half) / v;
            return LLQuaternion((F32)(x * s), (F32)(y * s), (F32)(z * s), (F32)std::cos(half));
        }

        /// The fitting's weight edge_frames from an end: 1 at the end, a cosine down to 0 at edge_frames.
        F64 edgeWeight(S32 from_end, S32 edge_frames)
        {
            return from_end < edge_frames ? 0.5 * (1.0 + std::cos(PI * (F64)from_end / (F64)edge_frames)) : 0.0;
        }

        /// The ease-in-out curve from the start (0) to the end (1) over the clip.
        F64 easeAt(S32 frame, S32 frame_count)
        {
            return 0.5 - 0.5 * std::cos(PI * (F64)frame / (F64)std::max(1, frame_count - 1));
        }

        /// From a to b, t of the way (a and b unit): a * (a^-1 * b)^t.
        LLQuaternion easeRotation(const LLQuaternion& a, const LLQuaternion& b, F64 t)
        {
            LLQuaternion q = a * rotPow(~a * b, t);
            q.normalize();
            return q;
        }

        /// Unit copies of a key pose's rotations; false if one is degenerate or a vector is out of range.
        bool readKey(const KeyPose& key, std::vector<LLQuaternion>& local, std::vector<LLQuaternion>& extra)
        {
            local.resize(key.sl.local.size());
            for (size_t i = 0; i < local.size(); ++i)
            {
                if (!normalizedRotation(key.sl.local[i], local[i]))
                {
                    return false;
                }
            }
            extra.resize(key.extra_rot.size());
            for (size_t i = 0; i < extra.size(); ++i)
            {
                if (!normalizedRotation(key.extra_rot[i], extra[i]))
                {
                    return false;
                }
            }
            if (!validPosition(key.sl.pelvis))
            {
                return false;
            }
            for (const LLVector3& v : key.extra_vec)
            {
                if (!validPosition(v))
                {
                    return false;
                }
            }
            return true;
        }

        /// Keeps each track's rotations on one side of the sphere from frame to frame (q and -q are the same turn;
        /// the playback's slerp would otherwise go the long way between two frames).
        void keepSigns(std::vector<LLQuaternion>& rot, S32 frame_count, S32 tracks)
        {
            for (S32 f = 1; f < frame_count; ++f)
            {
                for (S32 j = 0; j < tracks; ++j)
                {
                    const LLQuaternion& prev = rot[(size_t)(f - 1) * tracks + j];
                    LLQuaternion&       q    = rot[(size_t)f * tracks + j];
                    if (dot(prev, q) < 0.f)
                    {
                        q = LLQuaternion(-q.mQ[VX], -q.mQ[VY], -q.mQ[VZ], -q.mQ[VW]);
                    }
                }
            }
        }
    } // namespace

    RetargetResult buildClip(const SomaRetarget& retarget, const KeyPose& start, const KeyPose& end,
                             const std::vector<SomaFrame>& answer, MotionClip& out, ClipStats* stats)
    {
        if (!retarget.ready())
        {
            return RetargetResult::BadSkeleton;
        }
        const S32 n      = (S32)answer.size();
        const S32 joints = retarget.jointCount();
        const S32 erot   = (S32)start.extra_rot.size();
        const S32 evec   = (S32)start.extra_vec.size();
        if (n < CLIP_MIN_FRAMES || n > CLIP_MAX_FRAMES || (S32)start.sl.local.size() != joints ||
            (S32)end.sl.local.size() != joints || (S32)end.extra_rot.size() != erot || (S32)end.extra_vec.size() != evec ||
            erot > MAX_EXTRA_TRACKS || evec > MAX_EXTRA_TRACKS)
        {
            return RetargetResult::BadInput;
        }
        std::vector<LLQuaternion> a_local, b_local, a_extra, b_extra;
        if (!readKey(start, a_local, a_extra) || !readKey(end, b_local, b_extra))
        {
            return RetargetResult::BadInput;
        }
        std::vector<S32> driven;
        for (S32 j = 0; j < joints; ++j)
        {
            if (retarget.isDriven(j))
            {
                driven.push_back(j);
            }
        }

        MotionClip clip;
        clip.frame_count     = n;
        clip.joint_count     = joints;
        clip.extra_rot_count = erot;
        clip.extra_vec_count = evec;
        clip.local.resize((size_t)n * joints);
        clip.pelvis.resize(n);
        clip.extra_rot.resize((size_t)n * erot);
        clip.extra_vec.resize((size_t)n * evec);

        // 1. SOMA to SL, with the undriven joints on the ease curve from the start to the end
        SlPose       pose{ a_local, start.sl.pelvis };
        RetargetWork work;
        for (S32 f = 0; f < n; ++f)
        {
            const F64 t = easeAt(f, n);
            for (S32 j = 0; j < joints; ++j)
            {
                if (!retarget.isDriven(j))
                {
                    pose.local[j] = easeRotation(a_local[j], b_local[j], t);
                }
            }
            const RetargetResult r = retarget.somaToSl(answer[f], pose, work);
            if (r != RetargetResult::Ok)
            {
                return r;
            }
            std::copy(pose.local.begin(), pose.local.end(), clip.local.begin() + (size_t)f * joints);
            clip.pelvis[f] = pose.pelvis;
            for (S32 k = 0; k < erot; ++k)
            {
                clip.extra_rot[(size_t)f * erot + k] = easeRotation(a_extra[k], b_extra[k], t);
            }
            for (S32 k = 0; k < evec; ++k)
            {
                const LLVector3& va = start.extra_vec[k];
                clip.extra_vec[(size_t)f * evec + k] = va + (end.extra_vec[k] - va) * (F32)t;
            }
        }

        // 2. the driven joints and the pelvis fitted to the two poses at the ends, on SL's local rotations
        const S32 edge = std::min(EDGE_FRAMES, n / 2);
        const LLQuaternion* first = &clip.local[0];
        const LLQuaternion* last  = &clip.local[(size_t)(n - 1) * joints];
        std::vector<LLQuaternion> to_start(joints), to_end(joints);
        ClipStats st;
        st.edge_frames = edge;
        for (S32 j : driven)
        {
            to_start[j]    = ~first[j] * a_local[j];
            to_end[j]      = ~last[j] * b_local[j];
            st.start_max_deg = std::max(st.start_max_deg, rotationAngle(first[j], a_local[j]) * RAD_TO_DEG);
            st.end_max_deg   = std::max(st.end_max_deg, rotationAngle(last[j], b_local[j]) * RAD_TO_DEG);
        }
        const LLVector3 pelvis_to_start = start.sl.pelvis - clip.pelvis[0];
        const LLVector3 pelvis_to_end   = end.sl.pelvis - clip.pelvis[n - 1];
        st.start_pelvis_m = pelvis_to_start.length();
        st.end_pelvis_m   = pelvis_to_end.length();
        for (S32 f = 0; f < n; ++f)
        {
            const F64 ws = edgeWeight(f, edge);
            const F64 we = edgeWeight(n - 1 - f, edge);
            if (ws == 0.0 && we == 0.0)
            {
                continue;
            }
            LLQuaternion* row = &clip.local[(size_t)f * joints];
            for (S32 j : driven)
            {
                row[j] = row[j] * rotPow(to_start[j], ws) * rotPow(to_end[j], we);
                row[j].normalize();
            }
            clip.pelvis[f] += pelvis_to_start * (F32)ws + pelvis_to_end * (F32)we;
        }

        // 3. one side of the sphere per track, and nothing that is not a number
        keepSigns(clip.local, n, joints);
        keepSigns(clip.extra_rot, n, erot);
        for (const LLQuaternion& q : clip.local)
        {
            if (!q.isFinite())
            {
                return RetargetResult::NotFinite;
            }
        }
        for (const LLVector3& p : clip.pelvis)
        {
            if (!p.isFinite())
            {
                return RetargetResult::NotFinite;
            }
        }

        out = std::move(clip);
        if (stats)
        {
            *stats = st;
        }
        return RetargetResult::Ok;
    }

    namespace
    {
        /// The pose t of the way from a to b, written into the arrays the caller sized (rotations spherical, the short
        /// way round; the pelvis, the extra vectors linear). a and b are the unit rotations readKey made of ka and kb.
        /// Used for every frame of an interpolated segment and for the single pose of blendKeyPose.
        void blendInto(const std::vector<LLQuaternion>& a_local, const std::vector<LLQuaternion>& b_local,
                       const std::vector<LLQuaternion>& a_extra, const std::vector<LLQuaternion>& b_extra, const KeyPose& ka,
                       const KeyPose& kb, F64 t, LLQuaternion* local, LLQuaternion* extra_rot, LLVector3& pelvis,
                       LLVector3* extra_vec)
        {
            for (size_t j = 0; j < a_local.size(); ++j)
            {
                local[j] = easeRotation(a_local[j], b_local[j], t);
            }
            for (size_t k = 0; k < a_extra.size(); ++k)
            {
                extra_rot[k] = easeRotation(a_extra[k], b_extra[k], t);
            }
            pelvis = ka.sl.pelvis + (kb.sl.pelvis - ka.sl.pelvis) * (F32)t;
            for (size_t k = 0; k < ka.extra_vec.size(); ++k)
            {
                extra_vec[k] = ka.extra_vec[k] + (kb.extra_vec[k] - ka.extra_vec[k]) * (F32)t;
            }
        }

        /// The two key poses have the same joint and extra track counts (and not too many extras).
        bool sizedAlike(const KeyPose& a, const KeyPose& b)
        {
            return !a.sl.local.empty() && a.sl.local.size() == b.sl.local.size() && a.extra_rot.size() == b.extra_rot.size() &&
                   a.extra_vec.size() == b.extra_vec.size() && (S32)a.extra_rot.size() <= MAX_EXTRA_TRACKS &&
                   (S32)a.extra_vec.size() <= MAX_EXTRA_TRACKS;
        }
    } // namespace

    RetargetResult makeInterpClip(const KeyPose& start, const KeyPose& end, S32 frames, InterpCurve curve, MotionClip& out)
    {
        if (frames < TIMELINE_MIN_FRAMES || frames > TIMELINE_MAX_FRAMES || !sizedAlike(start, end))
        {
            return RetargetResult::BadInput;
        }
        const S32 joints = (S32)start.sl.local.size();
        const S32 erot   = (S32)start.extra_rot.size();
        const S32 evec   = (S32)start.extra_vec.size();
        std::vector<LLQuaternion> a_local, b_local, a_extra, b_extra;
        if (!readKey(start, a_local, a_extra) || !readKey(end, b_local, b_extra))
        {
            return RetargetResult::BadInput;
        }
        MotionClip clip;
        clip.frame_count     = frames;
        clip.joint_count     = joints;
        clip.extra_rot_count = erot;
        clip.extra_vec_count = evec;
        clip.local.resize((size_t)frames * joints);
        clip.pelvis.resize(frames);
        clip.extra_rot.resize((size_t)frames * erot);
        clip.extra_vec.resize((size_t)frames * evec);
        for (S32 f = 0; f < frames; ++f)
        {
            const F64 t = curve == InterpCurve::EaseInOut ? easeAt(f, frames) : (F64)f / (F64)(frames - 1);
            blendInto(a_local, b_local, a_extra, b_extra, start, end, t, &clip.local[(size_t)f * joints],
                      erot > 0 ? &clip.extra_rot[(size_t)f * erot] : nullptr, clip.pelvis[(size_t)f],
                      evec > 0 ? &clip.extra_vec[(size_t)f * evec] : nullptr);
        }
        keepSigns(clip.local, frames, joints);
        keepSigns(clip.extra_rot, frames, erot);
        out = std::move(clip);
        return RetargetResult::Ok;
    }

    RetargetResult blendKeyPose(const KeyPose& a, const KeyPose& b, F64 t, KeyPose& out)
    {
        if (!sizedAlike(a, b) || !std::isfinite(t))
        {
            return RetargetResult::BadInput;
        }
        std::vector<LLQuaternion> a_local, b_local, a_extra, b_extra;
        if (!readKey(a, a_local, a_extra) || !readKey(b, b_local, b_extra))
        {
            return RetargetResult::BadInput;
        }
        KeyPose made;
        made.sl.local.resize(a_local.size());
        made.extra_rot.resize(a_extra.size());
        made.extra_vec.resize(a.extra_vec.size());
        blendInto(a_local, b_local, a_extra, b_extra, a, b, t, made.sl.local.data(), made.extra_rot.data(), made.sl.pelvis,
                  made.extra_vec.data());
        // one side of the sphere as a, so that the poses of a row (t, 2t, 3t) read the same way
        for (size_t j = 0; j < made.sl.local.size(); ++j)
        {
            if (dot(a_local[j], made.sl.local[j]) < 0.f)
            {
                made.sl.local[j] = LLQuaternion(-made.sl.local[j].mQ[VX], -made.sl.local[j].mQ[VY], -made.sl.local[j].mQ[VZ],
                                                -made.sl.local[j].mQ[VW]);
            }
        }
        for (size_t k = 0; k < made.extra_rot.size(); ++k)
        {
            if (dot(a_extra[k], made.extra_rot[k]) < 0.f)
            {
                made.extra_rot[k] = LLQuaternion(-made.extra_rot[k].mQ[VX], -made.extra_rot[k].mQ[VY],
                                                 -made.extra_rot[k].mQ[VZ], -made.extra_rot[k].mQ[VW]);
            }
        }
        out = std::move(made);
        return RetargetResult::Ok;
    }

    namespace
    {
        bool consistent(const MotionClip& c)
        {
            const size_t n = c.frame_count > 0 ? (size_t)c.frame_count : 0;
            return n > 0 && c.joint_count > 0 && c.extra_rot_count >= 0 && c.extra_vec_count >= 0 &&
                   c.local.size() == n * c.joint_count && c.pelvis.size() == n &&
                   c.extra_rot.size() == n * c.extra_rot_count && c.extra_vec.size() == n * c.extra_vec_count;
        }
    } // namespace

    bool tailPoses(const MotionClip& clip, S32 count, std::vector<SlPose>& out)
    {
        if (count < 1 || !consistent(clip) || clip.frame_count < count)
        {
            return false;
        }
        std::vector<SlPose> poses((size_t)count);
        for (S32 k = 0; k < count; ++k)
        {
            const S32 f = clip.frame_count - count + k;
            const LLQuaternion* row = &clip.local[(size_t)f * clip.joint_count];
            poses[(size_t)k].local.assign(row, row + clip.joint_count);
            poses[(size_t)k].pelvis = clip.pelvis[(size_t)f];
        }
        out = std::move(poses);
        return true;
    }

    bool tailStepDiffers(const std::vector<SlPose>& asked, const MotionClip& clip, F32 max_deg, F32 max_pelvis_m)
    {
        if (asked.size() != 2 || clip.frame_count < 2 || !consistent(clip) || (S32)asked[0].local.size() != clip.joint_count ||
            (S32)asked[1].local.size() != clip.joint_count)
        {
            return true;
        }
        const LLQuaternion* prev = &clip.local[(size_t)(clip.frame_count - 2) * clip.joint_count];
        const LLQuaternion* last = &clip.local[(size_t)(clip.frame_count - 1) * clip.joint_count];
        const LLVector3     pelvis_asked = asked[1].pelvis - asked[0].pelvis;
        const LLVector3     pelvis_now   = clip.pelvis[(size_t)clip.frame_count - 1] - clip.pelvis[(size_t)clip.frame_count - 2];
        if ((pelvis_asked - pelvis_now).length() > max_pelvis_m)
        {
            return true;
        }
        for (S32 j = 0; j < clip.joint_count; ++j)
        {
            const LLQuaternion step_asked = ~asked[0].local[(size_t)j] * asked[1].local[(size_t)j];
            const LLQuaternion step_now   = ~prev[j] * last[j];
            if (rotationAngle(step_asked, step_now) * RAD_TO_DEG > max_deg)
            {
                return true;
            }
        }
        return false;
    }

    ConcatResult concatClips(const std::vector<const MotionClip*>& clips, const std::vector<S32>& holds, MotionClip& out)
    {
        if (clips.empty())
        {
            return ConcatResult::NoClips;
        }
        if (holds.size() != clips.size() + 1)
        {
            return ConcatResult::BadHold;
        }
        S64 total = 1;
        for (S32 h : holds)
        {
            if (h < 0)
            {
                return ConcatResult::BadHold;
            }
            total += h;
        }
        for (const MotionClip* c : clips)
        {
            if (!c || c->frame_count < 2 || !consistent(*c) || c->joint_count != clips[0]->joint_count ||
                c->extra_rot_count != clips[0]->extra_rot_count || c->extra_vec_count != clips[0]->extra_vec_count)
            {
                return ConcatResult::Mismatch;
            }
            total += c->frame_count - 1;
            if (total > TIMELINE_MAX_FRAMES)
            {
                return ConcatResult::TooLong;
            }
        }
        const S32 joints = clips[0]->joint_count, erot = clips[0]->extra_rot_count, evec = clips[0]->extra_vec_count;
        MotionClip all;
        all.frame_count     = (S32)total;
        all.joint_count     = joints;
        all.extra_rot_count = erot;
        all.extra_vec_count = evec;
        all.local.reserve((size_t)total * joints);
        all.pelvis.reserve((size_t)total);
        all.extra_rot.reserve((size_t)total * erot);
        all.extra_vec.reserve((size_t)total * evec);
        auto push = [&](const MotionClip& c, S32 f, S32 times)
        {
            for (S32 i = 0; i < times; ++i)
            {
                all.local.insert(all.local.end(), c.local.begin() + (size_t)f * joints, c.local.begin() + (size_t)(f + 1) * joints);
                all.pelvis.push_back(c.pelvis[f]);
                all.extra_rot.insert(all.extra_rot.end(), c.extra_rot.begin() + (size_t)f * erot,
                                     c.extra_rot.begin() + (size_t)(f + 1) * erot);
                all.extra_vec.insert(all.extra_vec.end(), c.extra_vec.begin() + (size_t)f * evec,
                                     c.extra_vec.begin() + (size_t)(f + 1) * evec);
            }
        };
        push(*clips[0], 0, 1 + holds[0]);
        for (size_t i = 0; i < clips.size(); ++i)
        {
            const MotionClip& c = *clips[i];
            for (S32 f = 1; f < c.frame_count - 1; ++f)
            {
                push(c, f, 1);
            }
            push(c, c.frame_count - 1, 1 + holds[i + 1]); // key i + 1's frame, then its hold
        }
        keepSigns(all.local, all.frame_count, joints);
        keepSigns(all.extra_rot, all.frame_count, erot);
        out = std::move(all);
        return ConcatResult::Ok;
    }

    PlayCursor cursorAt(S32 frame_count, F64 elapsed_sec, LoopMode mode, F64 loop_hold_sec)
    {
        PlayCursor c;
        if (frame_count <= 1)
        {
            c.finished = mode == LoopMode::Once;
            return c;
        }
        const S32  last     = frame_count - 1;
        const F64  duration = (F64)last / CLIP_FPS;
        const bool forever  = std::isinf(elapsed_sec) && elapsed_sec > 0.0; // +infinity: the end, in both modes
        F64 t = std::isfinite(elapsed_sec) && elapsed_sec > 0.0 ? elapsed_sec : 0.0;
        if (mode == LoopMode::Loop && !forever)
        {
            t = std::fmod(t, duration + (std::isfinite(loop_hold_sec) && loop_hold_sec > 0.0 ? loop_hold_sec : 0.0));
        }
        if (forever || t >= duration)
        {
            c.frame = c.next = last;
            c.finished       = mode == LoopMode::Once;
            return c;
        }
        const F64 ft = t * CLIP_FPS;
        c.frame      = std::clamp((S32)std::floor(ft), 0, last);
        c.next       = std::min(c.frame + 1, last);
        c.blend      = (F32)std::clamp(ft - (F64)c.frame, 0.0, 1.0);
        return c;
    }

    PlayCursor cursorAtFrame(S32 frame_count, S32 frame)
    {
        PlayCursor c;
        if (frame_count > 0)
        {
            c.frame = c.next = std::clamp(frame, 0, frame_count - 1);
        }
        return c;
    }

    void sizePoseFor(const MotionClip& clip, KeyPose& out)
    {
        out.sl.local.resize(clip.joint_count);
        out.extra_rot.resize(clip.extra_rot_count);
        out.extra_vec.resize(clip.extra_vec_count);
    }

    bool samplePose(const MotionClip& clip, const PlayCursor& cursor, KeyPose& out)
    {
        const size_t frames = clip.frame_count > 0 ? (size_t)clip.frame_count : 0;
        if (frames == 0 || clip.joint_count <= 0 || clip.local.size() != frames * clip.joint_count ||
            clip.pelvis.size() != frames || clip.extra_rot.size() != frames * std::max(clip.extra_rot_count, 0) ||
            clip.extra_vec.size() != frames * std::max(clip.extra_vec_count, 0) ||
            (S32)out.sl.local.size() != clip.joint_count || (S32)out.extra_rot.size() != clip.extra_rot_count ||
            (S32)out.extra_vec.size() != clip.extra_vec_count)
        {
            return false; // empty, not made by buildClip, or out not sized by sizePoseFor
        }
        const S32 last  = clip.frame_count - 1;
        const S32 fa    = std::clamp(cursor.frame, 0, last);
        const S32 fb    = std::clamp(cursor.next, 0, last);
        const F32 blend = std::isfinite(cursor.blend) ? std::clamp(cursor.blend, 0.f, 1.f) : 0.f;

        auto rotations = [blend](const LLQuaternion* a, const LLQuaternion* b, S32 count, LLQuaternion* dst)
        {
            for (S32 j = 0; j < count; ++j)
            {
                if (blend <= 0.f)
                {
                    dst[j] = a[j];
                }
                else
                {
                    dst[j] = slerp(blend, a[j], b[j]);
                    dst[j].normalize();
                }
            }
        };
        rotations(&clip.local[(size_t)fa * clip.joint_count], &clip.local[(size_t)fb * clip.joint_count], clip.joint_count,
                  out.sl.local.data());
        if (clip.extra_rot_count > 0)
        {
            rotations(&clip.extra_rot[(size_t)fa * clip.extra_rot_count], &clip.extra_rot[(size_t)fb * clip.extra_rot_count],
                      clip.extra_rot_count, out.extra_rot.data());
        }
        const LLVector3& pa = clip.pelvis[fa];
        out.sl.pelvis       = pa + (clip.pelvis[fb] - pa) * blend;
        for (S32 k = 0; k < clip.extra_vec_count; ++k)
        {
            const LLVector3& va = clip.extra_vec[(size_t)fa * clip.extra_vec_count + k];
            out.extra_vec[k]    = va + (clip.extra_vec[(size_t)fb * clip.extra_vec_count + k] - va) * blend;
        }
        return true;
    }
} // namespace plus::posermotion

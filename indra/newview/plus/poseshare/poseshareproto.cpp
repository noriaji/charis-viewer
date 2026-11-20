/**
 * @file poseshareproto.cpp
 * @brief See poseshareproto.h.
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

#include "poseshareproto.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace plus::poseshare::proto
{
    const char* const JOINT_NAMES[JOINT_COUNT] = {
        "mHead", "mNeck", "mPelvis", "mChest", "mTorso", "mCollarLeft",
        "mShoulderLeft", "mElbowLeft", "mWristLeft", "mCollarRight", "mShoulderRight", "mElbowRight",
        "mWristRight", "mHipLeft", "mKneeLeft", "mAnkleLeft", "mFootLeft", "mToeLeft",
        "mHipRight", "mKneeRight", "mAnkleRight", "mFootRight", "mToeRight", "mFaceRoot",
        "mFaceForeheadLeft", "mFaceForeheadCenter", "mFaceForeheadRight", "mFaceEyebrowOuterLeft", "mFaceEyebrowCenterLeft", "mFaceEyebrowInnerLeft",
        "mFaceEyebrowOuterRight", "mFaceEyebrowCenterRight", "mFaceEyebrowInnerRight", "mEyeLeft", "mEyeRight", "mFaceEyeLidUpperLeft",
        "mFaceEyecornerInnerLeft", "mFaceEyeLidLowerLeft", "mFaceEyeLidUpperRight", "mFaceEyecornerInnerRight", "mFaceEyeLidLowerRight", "mFaceEar1Left",
        "mFaceEar2Left", "mFaceEar1Right", "mFaceEar2Right", "mFaceNoseBase", "mFaceNoseBridge", "mFaceNoseLeft",
        "mFaceNoseCenter", "mFaceNoseRight", "mFaceCheekUpperLeft", "mFaceCheekLowerLeft", "mFaceCheekUpperRight", "mFaceCheekLowerRight",
        "mFaceLipUpperLeft", "mFaceLipUpperCenter", "mFaceLipUpperRight", "mFaceLipCornerLeft", "mFaceLipCornerRight", "mFaceTeethUpper",
        "mFaceTeethLower", "mFaceTongueBase", "mFaceTongueTip", "mFaceLipLowerLeft", "mFaceLipLowerCenter", "mFaceLipLowerRight",
        "mFaceJaw", "mFaceChin", "mHandThumb1Left", "mHandThumb2Left", "mHandThumb3Left", "mHandIndex1Left",
        "mHandIndex2Left", "mHandIndex3Left", "mHandMiddle1Left", "mHandMiddle2Left", "mHandMiddle3Left", "mHandRing1Left",
        "mHandRing2Left", "mHandRing3Left", "mHandPinky1Left", "mHandPinky2Left", "mHandPinky3Left", "mHandThumb1Right",
        "mHandThumb2Right", "mHandThumb3Right", "mHandIndex1Right", "mHandIndex2Right", "mHandIndex3Right", "mHandMiddle1Right",
        "mHandMiddle2Right", "mHandMiddle3Right", "mHandRing1Right", "mHandRing2Right", "mHandRing3Right", "mHandPinky1Right",
        "mHandPinky2Right", "mHandPinky3Right", "mTail1", "mTail2", "mTail3", "mTail4",
        "mTail5", "mTail6", "mGroin", "mHindLimbsRoot", "mHindLimb1Left", "mHindLimb2Left",
        "mHindLimb3Left", "mHindLimb4Left", "mHindLimb1Right", "mHindLimb2Right", "mHindLimb3Right", "mHindLimb4Right",
        "mWingsRoot", "mWing1Left", "mWing2Left", "mWing3Left", "mWing4Left", "mWing4FanLeft",
        "mWing1Right", "mWing2Right", "mWing3Right", "mWing4Right", "mWing4FanRight", "mSkull",
        "mSpine1", "mSpine2", "mSpine3", "mSpine4",
    };

    namespace
    {
        constexpr float         SQRT2          = 1.41421356237f;
        constexpr int           ROT_BITS_MAX   = 1023;
        constexpr int           POSITION_STEPS = 32767; // -32768 is not a value
        constexpr int           SCALE_STEPS    = 65535;
        constexpr unsigned char FLAG_POSITION  = 1;
        constexpr unsigned char FLAG_SCALE     = 2;

        bool finite3(const float v[3]) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }

        // The rotation as the four bytes of the format: the largest component's number, the other three in 10 bits
        std::uint32_t packRotation(const float in[4])
        {
            float q[4]     = { in[0], in[1], in[2], in[3] };
            float length2  = 0.f;
            bool  usable   = true;
            for (float c : q)
            {
                if (!std::isfinite(c))
                    usable = false;
                length2 += c * c;
            }
            if (!usable || !(length2 > 1e-12f) || !std::isfinite(length2))
            {
                q[0] = q[1] = q[2] = 0.f;
                q[3]    = 1.f;
                length2 = 1.f;
            }
            const float inv = 1.f / std::sqrt(length2);
            for (float& c : q)
                c *= inv;

            int largest = 0;
            for (int i = 1; i < 4; ++i)
            {
                if (std::fabs(q[i]) > std::fabs(q[largest]))
                    largest = i;
            }
            const float   sign   = q[largest] < 0.f ? -1.f : 1.f; // q and -q are one rotation: the largest one is positive
            std::uint32_t packed = static_cast<std::uint32_t>(largest) << 30;
            int           shift  = 20;
            for (int i = 0; i < 4; ++i)
            {
                if (i == largest)
                    continue;
                const float v = std::clamp(sign * q[i] * SQRT2, -1.f, 1.f); // the others are within +-1/sqrt2
                const int   u = std::clamp(static_cast<int>(std::lround((v * 0.5f + 0.5f) * ROT_BITS_MAX)), 0, ROT_BITS_MAX);
                packed |= static_cast<std::uint32_t>(u) << shift;
                shift -= 10;
            }
            return packed;
        }

        void unpackRotation(std::uint32_t packed, float out[4])
        {
            const int largest = static_cast<int>(packed >> 30);
            float     sum2    = 0.f;
            int       shift   = 20;
            for (int i = 0; i < 4; ++i)
            {
                if (i == largest)
                    continue;
                const int   u = static_cast<int>((packed >> shift) & 0x3FFu);
                const float v = (static_cast<float>(u) / ROT_BITS_MAX - 0.5f) * 2.f / SQRT2;
                out[i]        = v;
                sum2 += v * v;
                shift -= 10;
            }
            out[largest] = std::sqrt(std::max(0.f, 1.f - sum2));
        }

        int quantisePosition(float v)
        {
            return static_cast<int>(std::lround(std::clamp(v, -POSITION_LIMIT, POSITION_LIMIT) / POSITION_LIMIT * POSITION_STEPS));
        }
        float dequantisePosition(int q) { return static_cast<float>(q) / POSITION_STEPS * POSITION_LIMIT; }

        int quantiseScale(float v)
        {
            const float t = (std::clamp(v, SCALE_DELTA_MIN, SCALE_DELTA_MAX) - SCALE_DELTA_MIN) / (SCALE_DELTA_MAX - SCALE_DELTA_MIN);
            return static_cast<int>(std::lround(t * SCALE_STEPS));
        }
        float dequantiseScale(int q)
        {
            return SCALE_DELTA_MIN + static_cast<float>(q) / SCALE_STEPS * (SCALE_DELTA_MAX - SCALE_DELTA_MIN);
        }

        void put16(std::vector<unsigned char>& out, unsigned value)
        {
            out.push_back(static_cast<unsigned char>(value & 0xFFu));
            out.push_back(static_cast<unsigned char>((value >> 8) & 0xFFu));
        }

        unsigned get16(const unsigned char* p) { return static_cast<unsigned>(p[0]) | (static_cast<unsigned>(p[1]) << 8); }

        // The values of an extra as they are sent: the position as a signed number (held in 16 bits), the scale as steps
        struct Extra
        {
            unsigned char index{ 0 };
            unsigned char flags{ 0 };
            int           position[3]{ 0, 0, 0 };
            int           scale[3]{ 0, 0, 0 };
            std::size_t   bytes() const
            {
                return 2 + ((flags & FLAG_POSITION) ? VECTOR_BYTES : 0) + ((flags & FLAG_SCALE) ? VECTOR_BYTES : 0);
            }
        };
    }

    std::string base64Encode(const unsigned char* data, std::size_t size)
    {
        static const char LETTERS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string       out;
        out.reserve((size + 2) / 3 * 4);
        for (std::size_t i = 0; i < size; i += 3)
        {
            const unsigned a = data[i];
            const unsigned b = i + 1 < size ? data[i + 1] : 0u;
            const unsigned c = i + 2 < size ? data[i + 2] : 0u;
            const unsigned n = (a << 16) | (b << 8) | c;
            out.push_back(LETTERS[(n >> 18) & 63u]);
            out.push_back(LETTERS[(n >> 12) & 63u]);
            out.push_back(i + 1 < size ? LETTERS[(n >> 6) & 63u] : '=');
            out.push_back(i + 2 < size ? LETTERS[n & 63u] : '=');
        }
        return out;
    }

    bool base64Decode(std::string_view text, std::vector<unsigned char>& out)
    {
        if (text.empty() || text.size() % 4 != 0)
            return false;
        std::vector<unsigned char> bytes;
        bytes.reserve(text.size() / 4 * 3);
        for (std::size_t i = 0; i < text.size(); i += 4)
        {
            unsigned n    = 0;
            int      have = 0;
            for (std::size_t k = 0; k < 4; ++k)
            {
                const char c = text[i + k];
                unsigned   v = 0;
                if (c >= 'A' && c <= 'Z')
                    v = static_cast<unsigned>(c - 'A');
                else if (c >= 'a' && c <= 'z')
                    v = static_cast<unsigned>(c - 'a') + 26u;
                else if (c >= '0' && c <= '9')
                    v = static_cast<unsigned>(c - '0') + 52u;
                else if (c == '+')
                    v = 62u;
                else if (c == '/')
                    v = 63u;
                else if (c == '=')
                {
                    if (i + k + 2 < text.size())
                        return false; // '=' only as the last one or two of the whole text
                    n <<= 6;
                    continue;
                }
                else
                    return false;
                if (have < static_cast<int>(k))
                    return false; // a letter after a '='
                n = (n << 6) | v;
                ++have;
            }
            if (have < 2)
                return false;
            bytes.push_back(static_cast<unsigned char>((n >> 16) & 0xFFu));
            if (have > 2)
                bytes.push_back(static_cast<unsigned char>((n >> 8) & 0xFFu));
            if (have > 3)
                bytes.push_back(static_cast<unsigned char>(n & 0xFFu));
        }
        out = std::move(bytes);
        return true;
    }

    bool encodePose(const Pose& pose, std::string& out, std::size_t* extrasDropped)
    {
        out.clear();
        if (extrasDropped)
            *extrasDropped = 0;

        unsigned char              mask[MASK_BYTES] = {};
        std::vector<std::uint32_t> rotations;
        std::vector<Extra>         extras;
        const int                  zeroScaleStep = quantiseScale(0.f); // a scale of "no change" is not an extra
        for (std::size_t i = 0; i < JOINT_COUNT; ++i)
        {
            const JointValue& j = pose.joints[i];
            if (!j.present)
                continue;
            mask[i / 8] = static_cast<unsigned char>(mask[i / 8] | (1u << (i % 8)));
            rotations.push_back(packRotation(j.rotation));

            Extra e;
            e.index = static_cast<unsigned char>(i);
            if (j.hasPosition && finite3(j.position))
            {
                for (int k = 0; k < 3; ++k)
                    e.position[k] = quantisePosition(j.position[k]);
                if (e.position[0] != 0 || e.position[1] != 0 || e.position[2] != 0)
                    e.flags = static_cast<unsigned char>(e.flags | FLAG_POSITION);
            }
            if (j.hasScale && finite3(j.scale))
            {
                for (int k = 0; k < 3; ++k)
                    e.scale[k] = quantiseScale(j.scale[k]);
                if (e.scale[0] != zeroScaleStep || e.scale[1] != zeroScaleStep || e.scale[2] != zeroScaleStep)
                    e.flags = static_cast<unsigned char>(e.flags | FLAG_SCALE);
            }
            if (e.flags != 0)
                extras.push_back(e);
        }
        if (rotations.empty())
            return false;

        std::vector<unsigned char> body;
        body.reserve(HEADER_BYTES + MASK_BYTES + rotations.size() * ROTATION_BYTES + 1 + 64);
        body.push_back(static_cast<unsigned char>(FORMAT_VERSION));
        body.push_back(static_cast<unsigned char>(KIND_POSE));
        body.push_back(static_cast<unsigned char>(rotations.size()));
        body.insert(body.end(), mask, mask + MASK_BYTES);
        for (std::uint32_t r : rotations)
        {
            body.push_back(static_cast<unsigned char>(r & 0xFFu));
            body.push_back(static_cast<unsigned char>((r >> 8) & 0xFFu));
            body.push_back(static_cast<unsigned char>((r >> 16) & 0xFFu));
            body.push_back(static_cast<unsigned char>((r >> 24) & 0xFFu));
        }

        // The extras that fit, in the order of the joints: PAYLOAD_MAX characters of base64 are PAYLOAD_MAX / 4 * 3
        // bytes, and one of them is the number of the extras
        const std::size_t maxBytes = PAYLOAD_MAX / 4 * 3;
        std::size_t       used     = body.size() + 1;
        std::size_t       kept     = 0;
        while (kept < extras.size() && used + extras[kept].bytes() <= maxBytes)
        {
            used += extras[kept].bytes();
            ++kept;
        }
        if (extrasDropped)
            *extrasDropped = extras.size() - kept;
        body.push_back(static_cast<unsigned char>(kept));
        for (std::size_t n = 0; n < kept; ++n)
        {
            const Extra& e = extras[n];
            body.push_back(e.index);
            body.push_back(e.flags);
            if (e.flags & FLAG_POSITION)
            {
                for (int k = 0; k < 3; ++k)
                    put16(body, static_cast<unsigned>(e.position[k]) & 0xFFFFu);
            }
            if (e.flags & FLAG_SCALE)
            {
                for (int k = 0; k < 3; ++k)
                    put16(body, static_cast<unsigned>(e.scale[k]));
            }
        }

        out = base64Encode(body.data(), body.size());
        return true;
    }

    bool decodePose(std::string_view text, Pose& out)
    {
        if (text.size() > PAYLOAD_MAX)
            return false;
        std::vector<unsigned char> bytes;
        if (!base64Decode(text, bytes))
            return false;
        if (bytes.size() < HEADER_BYTES + MASK_BYTES + 1)
            return false;
        if (bytes[0] != FORMAT_VERSION || bytes[1] != KIND_POSE)
            return false;
        const std::size_t count = bytes[2];
        if (count == 0 || count > JOINT_COUNT)
            return false;

        const unsigned char* mask    = bytes.data() + HEADER_BYTES;
        std::size_t          present = 0;
        for (std::size_t i = 0; i < MASK_BYTES * 8; ++i)
        {
            if (!(mask[i / 8] & (1u << (i % 8))))
                continue;
            if (i >= JOINT_COUNT)
                return false; // a joint the table has not
            ++present;
        }
        if (present != count)
            return false;

        const std::size_t rotationsEnd = HEADER_BYTES + MASK_BYTES + count * ROTATION_BYTES;
        if (bytes.size() < rotationsEnd + 1)
            return false;

        Pose                 pose; // built whole and given only when everything is right
        const unsigned char* r = bytes.data() + HEADER_BYTES + MASK_BYTES;
        for (std::size_t i = 0; i < JOINT_COUNT; ++i)
        {
            if (!(mask[i / 8] & (1u << (i % 8))))
                continue;
            const std::uint32_t packed = static_cast<std::uint32_t>(r[0]) | (static_cast<std::uint32_t>(r[1]) << 8) |
                                         (static_cast<std::uint32_t>(r[2]) << 16) | (static_cast<std::uint32_t>(r[3]) << 24);
            r += ROTATION_BYTES;
            JointValue& j = pose.joints[i];
            j.present     = true;
            unpackRotation(packed, j.rotation);
        }

        std::size_t       at          = rotationsEnd;
        const std::size_t extrasCount = bytes[at++];
        if (extrasCount > count)
            return false;
        int previous = -1;
        for (std::size_t n = 0; n < extrasCount; ++n)
        {
            if (bytes.size() < at + 2)
                return false;
            const unsigned char index = bytes[at];
            const unsigned char flags = bytes[at + 1];
            at += 2;
            if (index >= JOINT_COUNT || static_cast<int>(index) <= previous || !pose.joints[index].present || flags < 1 || flags > 3)
                return false;
            previous      = index;
            JointValue& j = pose.joints[index];
            if (flags & FLAG_POSITION)
            {
                if (bytes.size() < at + VECTOR_BYTES)
                    return false;
                for (std::size_t k = 0; k < 3; ++k)
                {
                    const int q = static_cast<std::int16_t>(get16(&bytes[at + 2 * k]));
                    if (q < -POSITION_STEPS)
                        return false; // -32768: no encoder makes it
                    j.position[k] = dequantisePosition(q);
                }
                j.hasPosition = true;
                at += VECTOR_BYTES;
            }
            if (flags & FLAG_SCALE)
            {
                if (bytes.size() < at + VECTOR_BYTES)
                    return false;
                for (std::size_t k = 0; k < 3; ++k)
                    j.scale[k] = dequantiseScale(static_cast<int>(get16(&bytes[at + 2 * k])));
                j.hasScale = true;
                at += VECTOR_BYTES;
            }
        }
        if (at != bytes.size())
            return false; // something left over

        out = pose;
        return true;
    }

    bool seqIsAccepted(int last, int seq, bool isEnd)
    {
        if (seq > last || (isEnd && seq == last))
            return true;
        return last - seq >= SEQ_RESTART_GAP; // a new run of shares, started from another number
    }

    bool RateLimit::take(double nowSeconds)
    {
        if (nowSeconds < mWindowStart || nowSeconds - mWindowStart >= 1.0)
        {
            mWindowStart = nowSeconds;
            mCount       = 0;
        }
        if (mCount >= mLimit)
            return false;
        ++mCount;
        return true;
    }
}

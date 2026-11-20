/**
 * @file poseshareproto.h
 * @brief What a shared pose looks like on the wire (plus/poseshare, research/374 section 3): the joint table, the
 *        packing of a pose into one base64 text, the strict reading of one, the rule for the sequence numbers and the
 *        counters of messages a second. Plain C++ without viewer types, so that the stand-alone test
 *        (poseshare_test.cpp) runs it as the viewer does.
 *
 * A pose is the target of each posed joint of the poser (what the sender's screen is made of): bytes
 *   version (1) | kind (1 = a pose) | joint count | mask of the joints present (MASK_BYTES, bit i = JOINT_NAMES[i])
 *   | rotations (4 bytes each, in the order of the mask) | extras count | extras
 * A rotation is "smallest three": the number of the largest component (2 bits) and the other three in 10 bits each
 * (the error is 0.1 degree on average and at most about 0.25). An extra is a joint number, a flags byte (1 position, 2 scale, 3 both) and 3 x 16 bits for each
 * of them: the position as a change of the base within +-POSITION_LIMIT m, the scale as a change of the base from
 * SCALE_DELTA_MIN to SCALE_DELTA_MAX. All little endian. The bytes are base64 text (standard letters, '=' padding):
 * the bridge's script never reads them. At most PAYLOAD_MAX characters (the bridge's limit, lslbridge::proto::
 * SHARE_PAYLOAD_MAX): a pose of all joints takes 724; extras that do not fit are left out (the later joints first).
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

#ifndef PLUS_POSESHARE_POSESHAREPROTO_H
#define PLUS_POSESHARE_POSESHAREPROTO_H

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace plus::poseshare::proto
{
    constexpr int         FORMAT_VERSION = 1;
    constexpr int         KIND_POSE      = 1;
    constexpr std::size_t JOINT_COUNT    = 130; // the joints of the poser without its collision volumes
    constexpr std::size_t MASK_BYTES     = (JOINT_COUNT + 7) / 8;
    constexpr std::size_t HEADER_BYTES   = 3;
    constexpr std::size_t PAYLOAD_MAX    = 900; // characters; the same as lslbridge::proto::SHARE_PAYLOAD_MAX
    constexpr std::size_t ROTATION_BYTES = 4;
    constexpr std::size_t VECTOR_BYTES   = 6;

    constexpr float POSITION_LIMIT  = 10.f;   // m, as the pose loading (plus::poser::POSER_VALUE_POSITION_LIMIT)
    constexpr float SCALE_DELTA_MIN = -0.95f; // as plus::poser::POSER_VALUE_SCALE_DELTA_MIN
    constexpr float SCALE_DELTA_MAX = 19.f;

    // The joints of version 1, in the order of the poser's list (FSPoserAnimator::PoserJoints) without its collision
    // volumes. The number in a pose is the place in this table: it never changes within a version (a new table is a
    // new FORMAT_VERSION). The names are those of the avatar's skeleton.
    extern const char* const JOINT_NAMES[JOINT_COUNT];

    struct JointValue
    {
        bool  present{ false };
        float rotation[4]{ 0.f, 0.f, 0.f, 1.f }; // x y z w, the target rotation of the joint
        bool  hasPosition{ false };
        float position[3]{ 0.f, 0.f, 0.f };      // as a change of the base
        bool  hasScale{ false };
        float scale[3]{ 0.f, 0.f, 0.f };         // as a change of the base
    };
    struct Pose
    {
        std::array<JointValue, JOINT_COUNT> joints;
    };

    // The base64 text of `pose` in `out`. false (out empty) when no joint is present. Values that are not finite
    // count as the identity and as zero; a rotation is normalised. `extrasDropped`: the number of positions and
    // scales left out for want of room (the rotations always fit).
    bool encodePose(const Pose& pose, std::string& out, std::size_t* extrasDropped = nullptr);

    // Reads a text of encodePose(): every check, and `out` is left as it was when false. false for: not base64, a size
    // that is not what the header and the mask say (nothing left over), another version or kind, no joint, a joint
    // count that is not the mask's, a joint beyond JOINT_COUNT, extras that are not in ascending order, for a joint
    // that is not present, with a flags byte that is not 1 to 3, or with a position value that cannot be one.
    bool decodePose(std::string_view text, Pose& out);

    // Standard base64 (what lslbridge::proto::isBase64() accepts).
    std::string base64Encode(const unsigned char* data, std::size_t size);
    bool        base64Decode(std::string_view text, std::vector<unsigned char>& out);

    // The sequence numbers of one sender. Within one run of shares they grow by one; a run starts from a new random
    // number, so a number lower than the last by SEQ_RESTART_GAP or more is a new run, and a lower one by less is a
    // message that came late (dropped). The end of a share may repeat the number of the one before it.
    constexpr int SEQ_RESTART_GAP = 1000000;
    bool seqIsAccepted(int last, int seq, bool isEnd);

    // At most `perSecond` takes in one second (a fixed window that opens at the first take after the last one's second).
    // `nowSeconds` is a clock that only goes on; one that goes back opens a new window.
    class RateLimit
    {
      public:
        explicit RateLimit(int perSecond = 1) : mLimit(perSecond) {}
        bool take(double nowSeconds);
        // When the window the last take is in opened (a very old time before any take): a count older than a second is of no use
        double windowStart() const { return mWindowStart; }

      private:
        int    mLimit;
        int    mCount{ 0 };
        double mWindowStart{ -1e18 };
    };
}

#endif // PLUS_POSESHARE_POSESHAREPROTO_H

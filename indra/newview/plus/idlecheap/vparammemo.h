/**
 * @file vparammemo.h
 * @brief LLVOAvatar::visualParamWeightsAreDefault from a kept list of the
 *        avatar's tweakable parameters (research/193 B).
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

#ifndef PLUS_IDLECHEAP_VPARAMMEMO_H
#define PLUS_IDLECHEAP_VPARAMMEMO_H

#include "stdtypes.h"
#include "vparamscan.h"

#include <vector>

class LLVisualParam;
class LLVOAvatar;

// Upstream's visualParamWeightsAreDefault walks all of an avatar's visual
// parameters (a std::map, about 250 nodes) and dynamic_casts each tweakable
// one, every frame for every avatar not yet fully loaded (the loading
// effect) and every name tag (getHasMissingParts). Its answer: no tweakable
// parameter is off its default weight (skirt parameters count only while a
// skirt is worn).
//
// Here the tweakable parameters, with whether each is a skirt one, are
// listed once per avatar (again when the number of parameters changes), and
// the weights are read through the same getWeight / getDefaultWeight. The
// parameter found off its default last time (the witness) is read first:
// still off, the answer is false at once. Otherwise the whole list is read,
// so the answer is upstream's on every call: nothing is remembered but which
// parameter to read first, and no change of a weight has to be caught.
//
// LLRHI::sVParamMemo / sVParamMemoVerify / sVParamMemoAuto (llrhi.h).
namespace plus::vparammemo
{
    using Entry = BasicEntry<LLVisualParam>;

    // A member of LLVOAvatar (main thread).
    struct Memo
    {
        std::vector<Entry> tweakable;   // in the avatar's parameter order
        S32 listed_of = -1;             // getVisualParamCount() when listed
        int witness = -1;               // the entry found off its default last time (allDefault)
    };

    enum Mode
    {
        UPSTREAM,   // off: upstream's walk answers
        ANSWERED,   // answered from the memo (answer())
        VERIFY      // answered from the memo, and upstream's walk runs to compare (compare())
    };

    // At the top of visualParamWeightsAreDefault.
    Mode answer(LLVOAvatar& avatar, Memo& memo, bool& out);
    // At its end, after upstream's walk, in VERIFY mode.
    void compare(const LLVOAvatar& avatar, bool memo_answer, bool upstream_answer);

    // The counters, for the census line (idlecensus.cpp); taken and cleared.
    struct Counts
    {
        U64 calls = 0;          // answer() with the memo on or verifying
        U64 witness_hits = 0;   // answered false by the witness alone
        U64 walks = 0;          // the whole list read
        U64 default_answers = 0;    // answered true (all at the default: a cloud)
        U64 listed = 0;         // lists made
        U64 checked = 0;        // verify: compared
        U64 mismatches = 0;     // verify: different from upstream (must stay 0)
    };
    Counts takeCounts();
}

#endif // PLUS_IDLECHEAP_VPARAMMEMO_H

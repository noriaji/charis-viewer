/**
 * @file vparammemo.cpp
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

#include "llviewerprecompiledheaders.h"

#include "vparammemo.h"

#include "llrhi.h"
#include "llviewervisualparam.h"
#include "llvoavatar.h"
#include "llwearabletype.h"

namespace
{
    // auto: this many answers compared with no mismatch, then the memo alone.
    constexpr U64 AUTO_CHECKS = 20000;
    // verify: the first mismatches logged one by one.
    constexpr U64 LOGGED_MISMATCHES = 5;

    plus::vparammemo::Counts sCounts;
    U64 sAutoChecked = 0;   // auto: compared so far (not cleared by takeCounts)

    void list_params(LLVOAvatar& avatar, plus::vparammemo::Memo& memo)
    {
        memo.tweakable.clear();
        memo.witness = -1;
        // Upstream's walk (it moves the same iterator, mCurIterator).
        for (LLVisualParam* param = avatar.getFirstVisualParam(); param; param = avatar.getNextVisualParam())
        {
            if (param->isTweakable())
            {
                const LLViewerVisualParam* vparam = dynamic_cast<const LLViewerVisualParam*>(param);
                memo.tweakable.push_back({ param, vparam && LLWearableType::WT_SKIRT == vparam->getWearableType() });
            }
        }
        memo.listed_of = avatar.getVisualParamCount();
        ++sCounts.listed;
    }
}

namespace plus::vparammemo
{
    Mode answer(LLVOAvatar& avatar, Memo& memo, bool& out)
    {
        if (!LLRHI::sVParamMemo && !LLRHI::sVParamMemoVerify)
        {
            return UPSTREAM;
        }
        ++sCounts.calls;
        if (memo.listed_of != avatar.getVisualParamCount())
        {
            list_params(avatar, memo);
        }
        const bool wearing_skirt = avatar.isWearingWearableType(LLWearableType::WT_SKIRT);
        bool by_witness = false;
        out = allDefault(memo.tweakable, memo.witness, wearing_skirt, by_witness);
        if (by_witness)
        {
            ++sCounts.witness_hits;
        }
        else
        {
            ++sCounts.walks;
        }
        if (out)
        {
            ++sCounts.default_answers;
        }
        return LLRHI::sVParamMemoVerify ? VERIFY : ANSWERED;
    }

    void compare(const LLVOAvatar& avatar, bool memo_answer, bool upstream_answer)
    {
        ++sCounts.checked;
        if (memo_answer != upstream_answer)
        {
            ++sCounts.mismatches;
            if (sCounts.mismatches <= LOGGED_MISMATCHES)
            {
                LL_WARNS("VKPerf") << "vparam memo mismatch: avatar " << avatar.getID() << " memo "
                                   << (memo_answer ? "default" : "not default") << ", upstream "
                                   << (upstream_answer ? "default" : "not default") << LL_ENDL;
            }
            if (LLRHI::sVParamMemoAuto)
            {
                // auto: off for the run at the first mismatch (upstream's walk answers).
                LLRHI::sVParamMemoAuto = false;
                LLRHI::sVParamMemo = false;
                LLRHI::sVParamMemoVerify = false;
                LL_WARNS("VKPerf") << "vparam memo [auto]: a mismatch after " << sAutoChecked
                                   << " compared answers; off for this run" << LL_ENDL;
            }
            return;
        }
        if (LLRHI::sVParamMemoAuto && ++sAutoChecked >= AUTO_CHECKS)
        {
            LLRHI::sVParamMemoAuto = false;
            LLRHI::sVParamMemoVerify = false;
            LL_INFOS("VKPerf") << "vparam memo [auto]: " << sAutoChecked
                               << " answers compared with no mismatch; on (verify off)" << LL_ENDL;
        }
    }

    Counts takeCounts()
    {
        const Counts counts = sCounts;
        sCounts = Counts();
        return counts;
    }
}

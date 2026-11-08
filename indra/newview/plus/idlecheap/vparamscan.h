/**
 * @file vparamscan.h
 * @brief The read of research/193 B's parameter list (no viewer headers, so
 *        that vparamscan_test.cpp can build it alone).
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

#ifndef PLUS_IDLECHEAP_VPARAMSCAN_H
#define PLUS_IDLECHEAP_VPARAMSCAN_H

#include <vector>

namespace plus::vparammemo
{
    // One tweakable parameter of an avatar (Param: LLVisualParam in the
    // viewer, anything with getWeight() and getDefaultWeight() in the test).
    template <class Param>
    struct BasicEntry
    {
        const Param* param;
        bool skirt;     // an LLViewerVisualParam of the skirt wearable
    };

    // Upstream's test for one parameter (visualParamWeightsAreDefault): off
    // its default weight, and counted (a skirt parameter only while a skirt
    // is worn).
    template <class Param>
    inline bool offDefault(const BasicEntry<Param>& entry, bool wearing_skirt)
    {
        return entry.param->getWeight() != entry.param->getDefaultWeight() && (wearing_skirt || !entry.skirt);
    }

    // Upstream's answer: true when no entry is off its default. The entry
    // found off last time (witness, an index or -1) is read first; still
    // off, the answer is false without reading the rest (by_witness).
    // Otherwise every entry is read, in order, and the first one off becomes
    // the witness. Nothing else is kept, so the answer is upstream's
    // whatever changed since the last call.
    template <class Param>
    bool allDefault(const std::vector<BasicEntry<Param>>& entries, int& witness, bool wearing_skirt, bool& by_witness)
    {
        const int count = (int)entries.size();
        if (witness >= 0 && witness < count && offDefault(entries[witness], wearing_skirt))
        {
            by_witness = true;
            return false;
        }
        by_witness = false;
        witness = -1;
        for (int i = 0; i < count; ++i)
        {
            if (offDefault(entries[i], wearing_skirt))
            {
                witness = i;
                return false;
            }
        }
        return true;
    }
}

#endif // PLUS_IDLECHEAP_VPARAMSCAN_H

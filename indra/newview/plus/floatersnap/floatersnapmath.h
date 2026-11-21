/**
 * @file floatersnapmath.h
 * @brief Windows that touch move together (plus/floatersnap, research/387): the pure parts, with no viewer type in
 *        them so floatersnap_test.cpp builds them alone.
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

#ifndef PLUS_FLOATERSNAP_MATH_H
#define PLUS_FLOATERSNAP_MATH_H

#include <algorithm>
#include <cstdlib>

namespace plus::floatersnap
{
    // A window's rectangle in the floater view's coordinates (y up: bottom < top), as LLRect.
    struct Box
    {
        int left   = 0;
        int bottom = 0;
        int right  = 0;
        int top    = 0;
    };

    // The most windows one drag moves together (the one grabbed counts).
    constexpr int MAX_GROUP = 32;
    // Two windows next to each other touch only when they share this much of the other axis (a corner alone does not).
    constexpr int MIN_SHARED = 16;
    // The gap between touching windows the setting allows (PlusFloaterSnapGap).
    constexpr int MAX_GAP = 8;

    inline int clampGap(int gap) { return std::min(std::max(gap, 0), MAX_GAP); }

    // How much of [a0, a1] and [b0, b1] they have in common (negative: none).
    inline int shared(int a0, int a1, int b0, int b1) { return std::min(a1, b1) - std::max(a0, b0); }

    // Whether a and b are next to each other: facing edges at most gap + 1 apart (the snap puts them gap apart, or
    // exactly together), sharing MIN_SHARED of the other axis. Windows that overlap are not next to each other.
    inline bool touching(const Box& a, const Box& b, int gap)
    {
        const int reach = clampGap(gap) + 1;
        const bool side_by_side = shared(a.bottom, a.top, b.bottom, b.top) >= MIN_SHARED
                                  && (std::abs(a.right - b.left) <= reach || std::abs(b.right - a.left) <= reach);
        const bool stacked = shared(a.left, a.right, b.left, b.right) >= MIN_SHARED
                             && (std::abs(a.top - b.bottom) <= reach || std::abs(b.top - a.bottom) <= reach);
        return side_by_side || stacked;
    }

    // The windows that move with boxes[start]: the ones touching it, and the ones touching those (breadth first, so the
    // nearest come first), at most cap of them. Writes their indexes into out (cap entries, start first) and returns
    // how many. start out of range: 0. A box in skip (n flags, may be null) is neither a member nor a bridge.
    inline int collectGroup(const Box* boxes, const bool* skip, int n, int start, int gap, int cap, int* out)
    {
        if (!boxes || !out || cap <= 0 || start < 0 || start >= n || (skip && skip[start]))
        {
            return 0;
        }
        int count = 0;
        out[count++] = start;
        for (int head = 0; head < count; ++head)
        {
            const Box& from = boxes[out[head]];
            for (int i = 0; i < n && count < cap; ++i)
            {
                if (skip && skip[i])
                {
                    continue;
                }
                bool known = false;
                for (int k = 0; k < count && !known; ++k)
                {
                    known = out[k] == i;
                }
                if (!known && touching(from, boxes[i], gap))
                {
                    out[count++] = i;
                }
            }
        }
        return count;
    }

    // The snap along one axis: whether an edge was caught (an edge that is exactly in line counts: distance 0) and how far to
    // move. "Not caught" and "in line" are both distance 0, so the distance alone cannot tell them apart.
    struct AxisSnap
    {
        bool  hit = false;
        int   distance = 0;
        void* tag = nullptr;    // what was caught (opaque here; carried along with the snap)
    };

    // Of two snaps along one axis, the nearer one that caught something (an edge in line, distance 0, is the nearest); the
    // first of equals; none caught: not caught.
    inline AxisSnap nearerSnap(const AxisSnap& a, const AxisSnap& b)
    {
        if (!a.hit)
        {
            return b;
        }
        if (!b.hit)
        {
            return a;
        }
        return std::abs(b.distance) < std::abs(a.distance) ? b : a;
    }

    // Whether a window at self can snap to other: self, reached out by reach on every side, meets other. A window far off
    // on the other axis (above and below each other, say) is not a place to snap to.
    inline bool nearEnough(const Box& self, const Box& other, int reach)
    {
        return self.left - reach <= other.right && other.left <= self.right + reach
               && self.bottom - reach <= other.top && other.bottom <= self.top + reach;
    }

    // When the screen pushes a window back, the others go back with it: of the pushes along one axis, the one that
    // moved a window the most (the sign kept; the first of equals).
    inline int strongestPush(const int* pushes, int count)
    {
        int best = 0;
        for (int i = 0; i < count; ++i)
        {
            if (std::abs(pushes[i]) > std::abs(best))
            {
                best = pushes[i];
            }
        }
        return best;
    }
}

#endif // PLUS_FLOATERSNAP_MATH_H

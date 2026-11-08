/**
 * @file minimapchat.h
 * @brief Draws the ranges text chat carries (whisper, say, shout) on the minimap.
 *
 *        waiting for the simulator to drop the region.
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

#ifndef PLUS_MINIMAPCHAT_H
#define PLUS_MINIMAPCHAT_H

namespace plus::minimapchat
{
    // The <plus> hook in LLNetMap::draw(), after the frustum: three circles
    // (whisper, say, shout) around the agent's dot (x, y in the map's own
    // pixels), when MiniMapShowChatRange is on. pixels_per_meter: the map's
    // current scale. Main thread.
    void drawRanges(float x, float y, float pixels_per_meter);
}

#endif // PLUS_MINIMAPCHAT_H

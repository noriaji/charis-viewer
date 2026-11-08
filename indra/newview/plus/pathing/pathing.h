/**
 * @file pathing.h
 * @brief Pathfinding view without Havok: the navmesh, the test path and the shapes,
 *        worked out on the client from the region's own geometry.
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

// The official viewer reaches the pathfinding view through LLPathingLib, which Linden Lab
// implements with Havok in a package outside the open source (the open source build has a
// stub that returns nothing, so the pathfinding view stays off). This feature gives
// LLPathingLib an implementation of its own (LLPathingLib::setReplacement, called from the
// feature's init); there is nothing else to call from outside. See research/128.

#ifndef PLUS_PATHING_PATHING_H
#define PLUS_PATHING_PATHING_H

namespace plus::pathing
{
    // How big a character is, by its type in the pathfinding console (A humanoid, B creature,
    // C mechanical, D other). The server's real sizes are not published: these are chosen
    // to look right and are the one place to change them.
    struct CharacterSize
    {
        float radius;
        float height;
    };
    constexpr CharacterSize CHARACTER_TYPE_A = { 0.30f, 1.80f };
    constexpr CharacterSize CHARACTER_TYPE_B = { 0.50f, 1.00f };
    constexpr CharacterSize CHARACTER_TYPE_C = { 1.00f, 2.50f };
    constexpr CharacterSize CHARACTER_TYPE_D = { 0.25f, 0.60f };
}

#endif // PLUS_PATHING_PATHING_H

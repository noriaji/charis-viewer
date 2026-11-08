/**
 * @file ao.h
 * @brief Animation Overrider (ported from Firestorm, plus/ao): the calls the
 *        official viewer makes, from its <plus> hooks.
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

#ifndef PLUS_AO_AO_H
#define PLUS_AO_AO_H

class LLUUID;

namespace plus::ao
{
    // LLVOAvatar::startMotion() / stopMotion() of the agent's own avatar. True
    // when the AO replaced the motion: the replacement has been requested from
    // the region and the Linden motion is neither started nor stopped here
    // (Firestorm's <FS:Zi> Animation Overrider, FIRE-29020).
    bool overrideStart(const LLUUID& motion);
    bool overrideStop(const LLUUID& motion);
}

#endif // PLUS_AO_AO_H

/**
 * @file motionfloater.h
 * @brief "Motion Between Poses" (research/324 section 1): the window. Two poses
 * of the own avatar taken from the poser, the motion between them made by the
 * helper, played on the avatar, saved as BVH, or discarded.
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

// The window (motionfloater.cpp and motionfloater_*.cpp, class in motionfloater_impl.h) holds machine C of research/324 section 5 (the clip, its playback and the pose
// from before the first "generate"); machines M and H are motionModel() and motionClient() (posermotion.h), which
// live on when it closes (a download goes on). Opened from the poser window's button, the Charis menu
// (menu_plus.xml) and Floater.ToggleOrBringToFront with this name.

#ifndef PLUS_POSERMOTION_MOTIONFLOATER_H
#define PLUS_POSERMOTION_MOTIONFLOATER_H

namespace plus::posermotion
{
    inline constexpr const char* MOTION_FLOATER = "plus_poser_motion";
} // namespace plus::posermotion

#endif // PLUS_POSERMOTION_MOTIONFLOATER_H

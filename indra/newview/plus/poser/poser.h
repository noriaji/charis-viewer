/**
 * @file poser.h
 * @brief The poser's small door for official files (plus/poser).
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

#ifndef PLUS_POSER_POSER_H
#define PLUS_POSER_POSER_H

class LLUUID;

namespace plus::poser
{
    // True while the avatar of `avatar_id` is another person posed on this screen with their consent (research/336,
    // FSPoserAnimator::isPosingOther) or one whose shared pose is held (research/374, FSPoserAnimator::isSharedOther).
    // LLVOAvatar::exemptFromFullDistance() asks it for every avatar each frame (the
    // full distance and the silhouette distance): nobody posed is one test of an empty list. Main thread.
    bool isPosingOther(const LLUUID& avatar_id);
}

#endif // PLUS_POSER_POSER_H

/**
 * @file restskeleton.h
 * @brief "Motion Between Poses": SL's rest skeleton (somaretarget.h's
 * RestSkeleton) read from an avatar.
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

#ifndef PLUS_POSERMOTION_RESTSKELETON_H
#define PLUS_POSERMOTION_RESTSKELETON_H

#include "somaretarget.h"

class LLAvatarAppearance;

namespace plus::posermotion
{
    /// The avatar's bones (not the collision volumes) in the skeleton's order (parents first), each with its
    /// parent and LLJoint::getDefaultPosition() / getDefaultScale(): the values of avatar_skeleton.xml, which
    /// only the skeleton's loading writes, so no shape slider or attachment's joint override changes them
    /// (research/324 F20). The live rotations are not read. Main thread (the avatar's joints are main-thread
    /// objects); called once when a clip is generated. False (out cleared) when the avatar has no skeleton yet.
    bool buildRestSkeleton(LLAvatarAppearance& avatar, RestSkeleton& out);
} // namespace plus::posermotion

#endif // PLUS_POSERMOTION_RESTSKELETON_H

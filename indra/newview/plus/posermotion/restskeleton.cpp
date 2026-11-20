/**
 * @file restskeleton.cpp
 * @brief "Motion Between Poses": SL's rest skeleton read from an avatar (see restskeleton.h).
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

#include "restskeleton.h"

#include "llavatarappearance.h"
#include "llavatarjoint.h"
#include "llthread.h"

#include <unordered_map>

namespace plus::posermotion
{
    bool buildRestSkeleton(LLAvatarAppearance& avatar, RestSkeleton& out)
    {
        llassert(on_main_thread());
        out.joints.clear();
        const LLAvatarAppearance::avatar_joint_list_t& bones = avatar.getSkeleton();
        if (bones.empty() || bones.size() > (size_t)MAX_REST_JOINTS)
        {
            return false;
        }

        // the skeleton is in the order setupBone() numbered it (a joint before its children), so each parent is
        // found before it is asked for; the root's parent (the avatar's mRoot) is not a bone
        std::unordered_map<const LLJoint*, S32> index;
        index.reserve(bones.size());
        out.joints.reserve(bones.size());
        for (LLAvatarJoint* bone : bones)
        {
            if (!bone)
            {
                out.joints.clear();
                return false;
            }
            RestJoint j;
            j.name  = bone->getName();
            j.pos   = bone->getDefaultPosition();
            j.scale = bone->getDefaultScale();
            const auto parent = index.find(bone->getParent());
            j.parent = parent != index.end() ? parent->second : -1;
            index.emplace(bone, (S32)out.joints.size());
            out.joints.push_back(std::move(j));
        }
        return true;
    }
} // namespace plus::posermotion

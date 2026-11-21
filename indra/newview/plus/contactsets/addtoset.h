/**
 * @file addtoset.h
 * @brief What the "add to a contact set" windows share: the target and the adding (research/333, research/386).
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

// The key of the window (plus::contactsets::showAddToSet / showMoveToSet): an avatar's id, an array of ids, or
// { "operation": "move", "source_set", "id" or "ids" }. Both windows (the XUI one, fsfloateraddtocontactset.cpp, and the
// RmlUi one, addtosetrml.cpp) read it and do the adding here, so that they cannot differ.

#ifndef PLUS_CONTACTSETS_ADDTOSET_H
#define PLUS_CONTACTSETS_ADDTOSET_H

#include "llsd.h"
#include "lluuid.h"

#include <string>
#include <vector>

namespace plus::contactsets
{
    class AddToSetTarget
    {
    public:
        explicit AddToSetTarget(const LLSD& target);

        bool multiple() const { return mHasMultipleAgents; }
        S32 count() const { return (S32)mAgentIDs.size(); }
        // The one avatar of a single target: its inspect link ("secondlife:///app/agent/<id>/inspect").
        std::string agentLink() const;
        // The sets to choose from: all of them, the one moved from left out when moving.
        std::vector<std::string> setNames() const;
        // Adds (or moves) the avatars to the set and tells the user; true when done (the window closes). False: nothing
        // was done (not a set, or no avatar).
        bool add(const std::string& set);

    private:
        bool mHasMultipleAgents = false;
        bool mIsMoveOperation = false;
        LLUUID mAgentID;
        uuid_vec_t mAgentIDs;
        std::string mSourceSet;
    };
}

#endif // PLUS_CONTACTSETS_ADDTOSET_H

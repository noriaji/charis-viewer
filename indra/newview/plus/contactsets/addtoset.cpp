/**
 * @file addtoset.cpp
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

// Moved out of Firestorm's FSFloaterAddToContactSet (fsfloateraddtocontactset.cpp: its constructor's reading of the key and
// onClickAdd, populateContactSets' list of names) without a change of what they do; plus/contactsets/ORIGIN.md.

#include "llviewerprecompiledheaders.h"

#include "addtoset.h"

#include "lggcontactsets.h"
#include "plus.h"

#include "llnotificationsutil.h"
#include "llsdutil.h"
#include "llslurl.h"

namespace plus::contactsets
{
    AddToSetTarget::AddToSetTarget(const LLSD& target)
    {
        auto addIds = [this](const LLSD& ids)
        {
            for (const LLSD& id : llsd::inArray(ids))
            {
                if (id.asUUID().notNull())
                {
                    mAgentIDs.push_back(id.asUUID());
                }
            }
        };
        if (target.isMap())
        {
            mIsMoveOperation = (target["operation"].asString() == "move");
            mSourceSet = target["source_set"].asString();
            const LLSD ids = target["ids"];
            if (ids.isArray())
            {
                mHasMultipleAgents = true;
                addIds(ids);
            }
            else if (target.has("id"))
            {
                mHasMultipleAgents = false;
                mAgentID = target["id"].asUUID();
            }
        }
        else if (target.isArray())
        {
            mHasMultipleAgents = true;
            addIds(target);
        }
        else
        {
            mHasMultipleAgents = false;
            mAgentID = target.asUUID();
        }
    }

    std::string AddToSetTarget::agentLink() const
    {
        return LLSLURL("agent", mAgentID, "inspect").getSLURLString();
    }

    std::vector<std::string> AddToSetTarget::setNames() const
    {
        std::vector<std::string> names;
        for (auto const& set_name : LGGContactSets::getInstance()->getAllContactSets())
        {
            if (mIsMoveOperation && set_name == mSourceSet)
            {
                continue;
            }
            names.push_back(set_name);
        }
        return names;
    }

    bool AddToSetTarget::add(const std::string& set)
    {
        LGGContactSets& contact_sets = LGGContactSets::instance();
        if (!contact_sets.isValidSet(set))
        {
            return false;
        }

        if (!mHasMultipleAgents)
        {
            if (mAgentID.isNull())
            {
                return false;
            }
            mAgentIDs.assign(1, mAgentID);
        }
        else if (mAgentIDs.empty())
        {
            return false;
        }

        contact_sets.addToSet(mAgentIDs, set);
        if (mIsMoveOperation && set != mSourceSet)
        {
            for (const LLUUID& id : mAgentIDs)
            {
                contact_sets.removeFriendFromSet(id, mSourceSet);
            }
        }

        if (!mIsMoveOperation && mHasMultipleAgents)
        {
            LLSD args;
            args["COUNT"] = llformat("%d", (S32)mAgentIDs.size());
            args["SET"] = plus::safeText(set);
            LLNotificationsUtil::add("AddToContactSetMultipleSuccess", args);
        }
        else if (!mIsMoveOperation)
        {
            LLSD args;
            args["NAME"] = agentLink();
            args["SET"] = plus::safeText(set);
            LLNotificationsUtil::add("AddToContactSetSingleSuccess", args);
        }
        return true;
    }
}

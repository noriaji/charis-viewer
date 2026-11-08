/**
 * @file lostattach.cpp
 * @brief Attachments kept through teleports and region crossings (from Firestorm FIRE-12004).
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

#include "plus/lostattach/lostattach.h"

#include "llagent.h"
#include "llappearancemgr.h"
#include "llattachmentsmgr.h"
#include "llcallbacklist.h"
#include "llchat.h"
#include "llnotificationmanager.h"
#include "llviewercontrol.h"
#include "llviewerjointattachment.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llselectmgr.h"
#include "llvoavatarself.h"

#include "plus/rlv/rlvhooks.h"

namespace plus::lostattach
{
namespace
{
    // How long a region crossing counts if the avatar never reports its new region.
    constexpr F64 CROSSING_MAX_SECONDS = 30.0;
    // The pause between an ignored late kill and asking for the attachments again.
    constexpr F32 REFRESH_DELAY_SECONDS = 5.f;

    F64  sArrivalGraceEnd  = 0.0;   // LLTimer::getTotalSeconds() when the grace period for late kills ends
    F64  sCrossingEnd      = 0.0;   // the crossing state ends at the latest
    bool sRefreshScheduled = false;
    // The objects whose kill was ignored for the reason "timer" since the last refresh. At the
    // refresh each is checked: one no longer worn (its outfit link is gone) gets the kill now.
    constexpr size_t MAX_TIMER_IGNORED = 64;
    std::vector<LLUUID> sTimerIgnored;

    void report(const std::string& message)
    {
        LL_WARNS("Messaging") << message << LL_ENDL;
        static LLCachedControl<bool> report_to_chat(gSavedSettings, "FSExperimentalLostAttachmentsFixReport");
        if (report_to_chat)
        {
            LLChat chat;
            chat.mText = message;
            // <plus> rlv: @showloc / @shownames - the line names the region and the avatar's attachments (research/248 M1)
            if (plus::rlv::enabled()) plus::rlv::filterTextForPlus(chat.mText);
            // </plus>
            chat.mSourceType = CHAT_SOURCE_SYSTEM;
            LLNotificationsUI::LLNotificationManager::instance().onChat(chat, LLSD());
        }
    }

    void killNoLongerWorn();

    void scheduleRefresh()
    {
        if (sRefreshScheduled)
        {
            return;
        }
        sRefreshScheduled = true;
        doAfterInterval([]()
        {
            sRefreshScheduled = false;
            killNoLongerWorn();
            report("Refreshing attachments...");
            refreshAttachments();
        }, REFRESH_DELAY_SECONDS);
    }

    // An ignored kill whose item has since left the outfit (taken off by the agent while the
    // kill was on its way) was a real one: kill the object now, so it does not stay as a ghost
    // and is not attached again by the refresh.
    void killNoLongerWorn()
    {
        std::vector<LLUUID> ids;
        ids.swap(sTimerIgnored);
        if (!isAgentAvatarValid())
        {
            return;
        }
        for (const LLUUID& id : ids)
        {
            LLViewerObject* objectp = gObjectList.findObject(id);
            if (!objectp || objectp->isDead() || !objectp->isAttachment())
            {
                continue;
            }
            if (LLAppearanceMgr::instance().isLinkedInCOF(objectp->getAttachmentItemID()))
            {
                continue;
            }
            report("Attachment no longer worn, removed: " + objectp->getAttachmentItemName());
            gObjectList.killObject(objectp);
            LLSelectMgr::getInstance()->removeObjectFromSelections(id);
        }
    }
}

void onTeleportArrival()
{
    static LLCachedControl<F32> kill_delay(gSavedSettings, "FSExperimentalLostAttachmentsFixKillDelay");
    sArrivalGraceEnd = LLTimer::getTotalSeconds() + llclamp((F32)kill_delay, 0.f, 30.f);
}

void onCrossedRegion()
{
    sCrossingEnd = LLTimer::getTotalSeconds() + CROSSING_MAX_SECONDS;
}

void onRegionChanged()
{
    sCrossingEnd = 0.0;
}

bool shouldIgnoreKill(LLViewerObject* objectp, const LLViewerRegion* regionp)
{
    static LLCachedControl<bool> fix_enabled(gSavedSettings, "FSExperimentalLostAttachmentsFix");
    if (!fix_enabled || !objectp || !isAgentAvatarValid())
    {
        return false;
    }
    // Temporary attachments (llAttachToAvatarTemp) are left out: the simulator takes them off
    // when the agent leaves the experience's land, and nothing would put them back.
    if (!objectp->isAttachment() || objectp->isTempAttachment() || !objectp->permYouOwner())
    {
        return false;
    }

    const F64 now = LLTimer::getTotalSeconds();
    const char* reason = nullptr;
    bool refresh = false;
    if (gAgent.getTeleportState() != LLAgent::TELEPORT_NONE)
    {
        reason = "tp";
    }
    else if (now <= sCrossingEnd)
    {
        reason = "crossing";
    }
    else if (now <= sArrivalGraceEnd)
    {
        reason = "timer";
        refresh = true;
    }
    if (!reason)
    {
        return false;
    }
    // An item no longer in the outfit was taken off on purpose (the outfit link goes first): the
    // kill is a real one, in a teleport and a crossing too.
    if (!LLAppearanceMgr::instance().isLinkedInCOF(objectp->getAttachmentItemID()))
    {
        return false;
    }

    const LLViewerRegion* agent_region = gAgent.getRegion();
    std::string kill_region       = regionp ? regionp->getName() : std::string("?");
    std::string agent_region_name = agent_region ? agent_region->getName() : std::string("?");
    // @showloc: the region left behind may no longer be among the regions filterTextForPlus() knows (research/248 M1)
    if (plus::rlv::enabled())
    {
        kill_region       = plus::rlv::regionNameForPlus(kill_region);
        agent_region_name = plus::rlv::regionNameForPlus(agent_region_name);
    }
    report("Region \"" + kill_region + "\" tried to kill attachment: "
           + objectp->getAttachmentItemName() + " (" + reason + ") - Agent region: \""
           + agent_region_name + "\"");
    if (refresh)
    {
        if (sTimerIgnored.size() < MAX_TIMER_IGNORED)
        {
            sTimerIgnored.push_back(objectp->getID());
        }
        scheduleRefresh();
    }
    return true;
}

void refreshAttachments()
{
    if (!isAgentAvatarValid())
    {
        return;
    }
    LLAttachmentsMgr& mgr = LLAttachmentsMgr::instance();
    for (const auto& point : gAgentAvatarp->mAttachmentPoints)
    {
        for (const LLViewerObject* attached : point.second->mAttachedObjects)
        {
            if (!attached || attached->isDead() || attached->isTempAttachment())
            {
                continue;
            }
            // Items asked for lately are left out by the manager itself.
            mgr.addAttachmentRequest(attached->getAttachmentItemID(), (U8)point.first, true);
        }
    }
}

} // namespace plus::lostattach

/**
 * @file groupwhitelist.cpp
 * @brief Group chat black and white lists (see groupwhitelist.h).
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

// How a group's chat is refused
// -----------------------------
// The decision is made from the session id alone (a group's chat session has the
// group's id) and our own saved lists, never from the agent's group data, which
// arrives some time after login. In this order:
//   1. blacklisted: refused
//   2. "only whitelisted groups" off, or whitelisted: received
//   3. a group (one of the agent's groups now, or seen in an earlier session): refused
//   4. the agent's groups have arrived: received (not a group: an ad-hoc conference)
//   5. otherwise (just after login: a group not seen before, or a conference): held,
//      dropped like a refused one and left once the groups show it is a group. A
//      conference the agent started here is received. When the groups show it is not
//      a refused group, its text invitations held back (its lines until it is joined)
//      go through the official handler then; lines that came another way (an offline
//      line through LLIMMgr::addMessage()) stay dropped.
// Refused chat is dropped before the official viewer keeps anything (no session, no
// history file, no notice); the session is left on the server and, if it is open here,
// closed. Pending invitations and agent list updates of a refused session are cleared
// for a while after (the server may still send a few).
//
// Every way a group chat comes in goes through one of the two hooks (llimview.cpp):
// ChatterBoxInvitation (all three kinds) and LLIMMgr::addMessage(). A session that is
// opened some other way (the agent starts the group's chat) is closed again by the
// session observer below, and a change of the lists or the switch closes the sessions
// it refuses.

#include "llviewerprecompiledheaders.h"

#include "groupwhitelist.h"

#include "plus.h"

#include "llagent.h"
#include "llapp.h"
#include "llcallbacklist.h"
#include "llfloater.h"
#include "llfloaterreg.h"
#include "llimview.h"
#include "llnotificationsutil.h"
#include "llsdserialize.h"
#include "llstartup.h"
#include "lltimer.h"
#include "lltrans.h"
#include "llviewercontrol.h"
#include "llsdutil_math.h"
#include "llviewermessage.h" // formatted_time
#include "message.h"

#include <algorithm>
#include <deque>

namespace
{
    using plus::groupwhitelist::Mode;

    constexpr char WHITELIST_ONLY_SETTING[] = "PlusGroupChatWhitelistOnly"; // per account
    constexpr char LISTS_FILE[]             = "plus_group_chat_lists.xml";  // in the account's folder
    constexpr char REFUSED_TIP[]            = "PlusGroupChatRefusedTip";    // strings_plus.xml

    constexpr F64 GROUP_LIST_WAIT   = 60.0; // s after login: the agent's groups count as arrived (none)
    constexpr F64 GROUP_LIST_SETTLE = 2.0;  // s after the first of them arrived
    constexpr F64 LEAVE_INTERVAL    = 10.0; // s: a refused session is left on the server at most this often
    constexpr F64 CLEANUP_TIME      = 30.0; // s after a refusal its pending invitation and updates are cleared
    constexpr F64 TICK_INTERVAL     = 1.0;

    constexpr S32 P2P_CHAT_SESSION = 2;     // a voice invitation's "invitation_type" (EMultiAgentChatSessionType, llimview.cpp)

    enum class Verdict
    {
        Receive,
        Refuse,
        Hold,   // not known yet whether the session is a group's; dropped for now
    };

    // The lists of the logged-in account.
    struct Lists
    {
        std::string dir;            // the account's folder they were read from; empty until login
        uuid_set_t  blacklist;
        uuid_set_t  whitelist;
        uuid_set_t  known_groups;   // the agent's groups, seen now or in an earlier session
    } sLists;

    U32 sRevision     = 0;
    F64 sStartedAt    = -1.0;   // when STATE_STARTED was first seen for these lists (the wait for the groups starts there;
                                // the lists are read at STATE_LOGIN_CLEANUP, before the login is even accepted)
    F64 sGroupsSeenAt = -1.0;   // when the agent's groups (gAgent.mGroups) were first not empty
    bool sAutoSaveBlocked = false; // the lists file could not be read nor moved aside: only a user change saves

    std::map<LLUUID, F64> sRefused; // refused sessions -> when they were last left on the server
    uuid_set_t            sHeld;    // sessions dropped as Hold, decided once the groups arrived

    // The text invitations (ChatterBoxInvitation bodies) of the held sessions, oldest first:
    // a conference's lines come as invitations until it is joined, so they are sent through
    // the official handler again when the session turns out not to be a refused group's.
    constexpr size_t MAX_HELD_LINES = 50; // a session; later ones are dropped as before
    // Sessions whose lines are kept (security audit A L4): many conferences in the short wait
    // for the groups do not grow the table without end. The oldest session's lines go first
    // (the session stays held and is still decided; only its early lines are not let in).
    constexpr size_t MAX_HELD_SESSIONS = 256;
    std::map<LLUUID, std::vector<LLSD>> sHeldInvitations;
    std::deque<LLUUID>                  sHeldInvitationOrder; // the sessions of sHeldInvitations, oldest first

    F64 now()
    {
        return LLTimer::getTotalSeconds();
    }

    std::string listsPath()
    {
        return gDirUtilp->add(sLists.dir, LISTS_FILE);
    }

    void readIds(const LLSD& array, uuid_set_t& ids)
    {
        for (LLSD::array_const_iterator it = array.beginArray(); it != array.endArray(); ++it)
        {
            const LLUUID id = it->asUUID();
            if (id.notNull())
            {
                ids.insert(id);
            }
        }
    }

    LLSD writeIds(const uuid_set_t& ids)
    {
        LLSD array = LLSD::emptyArray();
        for (const LLUUID& id : ids)
        {
            array.append(id);
        }
        return array;
    }

    void save()
    {
        if (sLists.dir.empty())
        {
            return;
        }
        LLSD data = LLSD::emptyMap();
        data["blacklist"]    = writeIds(sLists.blacklist);
        data["whitelist"]    = writeIds(sLists.whitelist);
        data["known_groups"] = writeIds(sLists.known_groups);

        // Written next to the file, then moved over it: a crash while saving leaves the old lists, and a
        // cut-off file (a full disk) never replaces them (they would read back empty).
        const std::string path = listsPath();
        const plus::FileWrite wrote = plus::writeFileReplacing(path, [&](std::ostream& out) { LLSDSerialize::toPrettyXML(data, out); });
        if (wrote == plus::FileWrite::cannot_replace)
        {
            LL_WARNS("GroupWhitelist") << "Cannot replace " << path << LL_ENDL;
        }
        else if (wrote != plus::FileWrite::written)
        {
            LL_WARNS("GroupWhitelist") << "Cannot write " << plus::tempPathFor(path) << LL_ENDL;
        }
    }

    // Reads the lists of the logged-in account the first time they are needed.
    // False before login (no account folder yet).
    bool ensureLoaded()
    {
        const std::string& dir = gDirUtilp->getLindenUserDir();
        if (dir.empty())
        {
            return false;
        }
        if (dir == sLists.dir)
        {
            return true;
        }

        // Another account: what was held back was the other one's. (Before the first account
        // there is no folder: what came then waits for this one's groups.)
        if (!sLists.dir.empty())
        {
            sHeld.clear();
            sHeldInvitations.clear();
            sHeldInvitationOrder.clear();
        }
        sLists = Lists();
        sLists.dir = dir;
        sAutoSaveBlocked = false;
        sStartedAt = -1.0;
        sGroupsSeenAt = -1.0;
        sRefused.clear();
        ++sRevision;

        const std::string path = listsPath();
        bool unreadable = false;
        {
            llifstream in(path.c_str());
            if (in.is_open())
            {
                LLSD data;
                if (LLSDParser::PARSE_FAILURE == LLSDSerialize::fromXML(data, in))
                {
                    unreadable = true;
                }
                else
                {
                    readIds(data["blacklist"], sLists.blacklist);
                    readIds(data["whitelist"], sLists.whitelist);
                    readIds(data["known_groups"], sLists.known_groups);
                }
            }
        } // closed: Windows cannot rename an open file
        if (unreadable)
        {
            // The next save (the agent's groups arriving) would replace it with empty lists:
            // keep the broken file aside so the lists can still be recovered by hand.
            // (".bad", ".bad1" ... ".bad99")
            const std::string bad = plus::putAside(path, 100, 1);
            if (!bad.empty())
            {
                LL_WARNS("GroupWhitelist") << "Cannot parse " << path << "; moved to " << bad
                                           << ", the group chat lists are empty" << LL_ENDL;
            }
            else
            {
                // Could not keep it aside: leave it alone until the user changes the lists.
                LL_WARNS("GroupWhitelist") << "Cannot parse " << path << " nor move it aside; the group chat lists are empty"
                                           << " and are not saved until they are changed" << LL_ENDL;
                sAutoSaveBlocked = true;
            }
        }
        LL_INFOS("GroupWhitelist") << sLists.blacklist.size() << " blacklisted, " << sLists.whitelist.size()
                                   << " whitelisted group(s), " << sLists.known_groups.size() << " known" << LL_ENDL;
        return true;
    }

    // Adds the agent's groups to the known ones, once they have arrived.
    void noteGroups()
    {
        if (gAgent.mGroups.empty())
        {
            return;
        }
        if (sGroupsSeenAt < 0.0)
        {
            sGroupsSeenAt = now();
        }
        bool added = false;
        for (const LLGroupData& group : gAgent.mGroups)
        {
            added |= sLists.known_groups.insert(group.mID).second;
        }
        if (added && !sAutoSaveBlocked)
        {
            save();
        }
    }

    // False until the login has completed (STATE_STARTED): a slow login, or a wrong password
    // and a second try, must not use up the wait before the groups could come.
    bool loginCompleted()
    {
        if (LLStartUp::getStartupState() < STATE_STARTED)
        {
            return false;
        }
        if (sStartedAt < 0.0)
        {
            sStartedAt = now();
        }
        return true;
    }

    // The agent's groups came and have stopped changing.
    bool groupsSettled()
    {
        return loginCompleted() && sGroupsSeenAt >= 0.0 && now() - sGroupsSeenAt >= GROUP_LIST_SETTLE;
    }

    bool groupsArrived()
    {
        if (!loginCompleted())
        {
            return false;
        }
        return groupsSettled() || now() - sStartedAt >= GROUP_LIST_WAIT;
    }

    bool isKnownGroup(const LLUUID& id)
    {
        return gAgent.isInGroup(id, true) || sLists.known_groups.count(id);
    }

    Verdict judge(const LLUUID& session_id)
    {
        if (!ensureLoaded())
        {
            return Verdict::Hold;
        }
        noteGroups();
        if (sLists.blacklist.count(session_id))
        {
            return Verdict::Refuse;
        }
        if (!plus::groupwhitelist::whitelistOnly() || sLists.whitelist.count(session_id))
        {
            return Verdict::Receive;
        }
        if (isKnownGroup(session_id))
        {
            return Verdict::Refuse;
        }
        return groupsArrived() ? Verdict::Receive : Verdict::Hold;
    }

    void clearPending(const LLUUID& session_id)
    {
        gIMMgr->clearPendingInvitation(session_id);
        gIMMgr->clearPendingAgentListUpdates(session_id);
    }

    // A voice invitation of the session still asked in its window (shown before the
    // group was refused) is declined, as its Reject button does.
    void rejectIncomingCall(const LLUUID& session_id)
    {
        std::vector<LLIncomingCallDialog*> calls;
        for (LLFloater* floater : LLFloaterReg::getFloaterList("incoming_call"))
        {
            LLIncomingCallDialog* call = dynamic_cast<LLIncomingCallDialog*>(floater);
            if (call && call->getKey()["session_id"].asUUID() == session_id)
            {
                calls.push_back(call);
            }
        }
        for (LLIncomingCallDialog* call : calls)
        {
            LLIncomingCallDialog::onReject(call);
        }
    }

    // Leaves a refused session on the server and removes what is kept of it here.
    void closeSession(const LLUUID& session_id, const LLUUID& other_participant_id)
    {
        if (!gIMMgr)
        {
            return;
        }
        rejectIncomingCall(session_id);
        clearPending(session_id);

        const F64 time = now();
        auto refused = sRefused.find(session_id);
        if (refused == sRefused.end())
        {
            LL_INFOS("GroupWhitelist") << "Refusing the chat of group " << session_id << LL_ENDL;
        }
        if (gIMMgr->hasSession(session_id))
        {
            // sends the leave and removes the session; its window closes
            gIMMgr->leaveSession(session_id);
            sRefused[session_id] = time;
        }
        else if (refused == sRefused.end() || time - refused->second >= LEAVE_INTERVAL)
        {
            LLIMModel::sendLeaveSession(session_id, other_participant_id.notNull() ? other_participant_id : session_id);
            sRefused[session_id] = time;
        }
    }

    // An ad-hoc conference the agent started here: never a group's chat, whether
    // the groups have arrived or not.
    bool startedAsConference(const LLUUID& session_id)
    {
        const LLIMModel::LLIMSession* session = LLIMModel::getInstance()->findIMSession(session_id);
        return session && session->mType == IM_SESSION_CONFERENCE_START;
    }

    void resolveHeld();

    // held: set when the chat is dropped as Hold (not refused for good).
    bool refuse(const LLUUID& session_id, const LLUUID& from_id, bool* held = nullptr)
    {
        if (session_id.isNull() || from_id == gAgentID)
        {
            return false; // the agent's own lines are never stopped (Firestorm FIRE-14222)
        }
        // Lines held back go first, should one come before the next tick lets them in.
        resolveHeld();
        switch (judge(session_id))
        {
        case Verdict::Receive:
            return false;
        case Verdict::Hold:
            if (startedAsConference(session_id))
            {
                return false;
            }
            if (sHeld.insert(session_id).second)
            {
                LL_INFOS("GroupWhitelist") << "Holding back the chat of " << session_id
                                           << " until the agent's groups have arrived" << LL_ENDL;
            }
            if (held)
            {
                *held = true;
            }
            return true;
        case Verdict::Refuse:
            closeSession(session_id, from_id);
            return true;
        }
        return false;
    }

    // Closes the open sessions (and voice invitations) that the lists now refuse.
    void applyToOpenSessions()
    {
        if (!gIMMgr || !ensureLoaded())
        {
            return;
        }
        std::vector<std::pair<LLUUID, LLUUID>> refused;
        for (const auto& [session_id, session] : LLIMModel::getInstance()->mId2SessionMap)
        {
            if (session && judge(session_id) == Verdict::Refuse)
            {
                refused.emplace_back(session_id, session->mOtherParticipantID);
            }
        }
        for (const auto& [session_id, other_participant_id] : refused)
        {
            closeSession(session_id, other_participant_id);
        }

        std::vector<LLUUID> calls;
        for (LLFloater* floater : LLFloaterReg::getFloaterList("incoming_call"))
        {
            const LLUUID session_id = floater ? floater->getKey()["session_id"].asUUID() : LLUUID::null;
            if (session_id.notNull() && judge(session_id) == Verdict::Refuse)
            {
                calls.push_back(session_id);
            }
        }
        for (const LLUUID& session_id : calls)
        {
            closeSession(session_id, LLUUID::null);
        }
    }

    // A held text invitation after the first, added as LLViewerChatterBoxInvitation::post()
    // (llimview.cpp) adds it, without its request to join (the first one made it).
    void addHeldLine(const LLSD& body)
    {
        const LLSD&  params  = body["instantmessage"]["message_params"];
        const LLUUID from_id = params["from_id"].asUUID();
        if (!gIMMgr || gAgent.isDoNotDisturb() || from_id == gAgentID)
        {
            return;
        }
        const U8     offline   = (U8)params["offline"].asInteger();
        const time_t timestamp = (time_t)params["timestamp"].asInteger();
        std::string  saved;
        if (offline == IM_OFFLINE)
        {
            LLStringUtil::format_map_t args;
            args["[LONG_TIMESTAMP]"] = formatted_time(timestamp);
            saved = LLTrans::getString("Saved_message", args);
        }
        // The session's name, up to its terminating zero.
        const std::vector<U8> bucket = params["data"]["binary_bucket"].asBinary();
        const auto            end    = std::find(bucket.begin(), bucket.end(), U8(0));
        const std::string     session_name(bucket.begin(), end);
        gIMMgr->addMessage(params["id"].asUUID(), from_id, params["from_name"].asString(), saved + params["message"].asString(),
                           IM_OFFLINE == offline, session_name, IM_SESSION_INVITE, params["parent_estate_id"].asInteger(),
                           params["region_id"].asUUID(), ll_vector3_from_sd(params["position"]), false, (U32)timestamp);
    }

    // Decides the held sessions once the agent's groups are known.
    void resolveHeld()
    {
        // Before the login has completed the groups have not "arrived" (groupsArrived()).
        if (sHeld.empty() || !ensureLoaded() || !groupsArrived())
        {
            return;
        }
        uuid_set_t held;
        held.swap(sHeld);
        std::map<LLUUID, std::vector<LLSD>> invitations;
        invitations.swap(sHeldInvitations);
        sHeldInvitationOrder.clear();
        for (const LLUUID& session_id : held)
        {
            if (judge(session_id) == Verdict::Refuse)
            {
                closeSession(session_id, LLUUID::null);
                continue;
            }
            // An ad-hoc conference: the lines held back go through the official handler
            // now (they keep their own time), and the next ones come as usual.
            const auto it = invitations.find(session_id);
            if (it == invitations.end())
            {
                continue;
            }
            // The first as it came (it opens the session and asks once to join), the rest
            // added as lines: each invitation through the handler would ask to join again,
            // and every one of those fails with a notice when the conference has ended.
            LL_INFOS("GroupWhitelist") << "Letting in " << it->second.size() << " line(s) held back of " << session_id << LL_ENDL;
            LLSD message;
            message["body"] = it->second.front();
            LLMessageSystem::dispatch("ChatterBoxInvitation", message);
            for (size_t i = 1; i < it->second.size(); ++i)
            {
                addHeldLine(it->second[i]);
            }
        }
    }

    // A session of a refused group can still be opened here: the agent starts the
    // group's chat. It is closed again at once, with a tip saying why.
    class SessionObserver : public LLIMSessionObserver
    {
    public:
        void sessionAdded(const LLUUID& session_id, const std::string&, const LLUUID&, bool) override
        {
            if (judge(session_id) != Verdict::Refuse)
            {
                return;
            }
            // not from inside LLIMModel::newSession(), which is still using the session
            doOnIdleOneTime([session_id]()
            {
                if (gIMMgr && gIMMgr->hasSession(session_id) && judge(session_id) == Verdict::Refuse)
                {
                    closeSession(session_id, LLUUID::null);
                    LLSD args;
                    args["MESSAGE"] = LLTrans::getString(REFUSED_TIP);
                    LLNotificationsUtil::add("SystemMessageTip", args);
                }
            });
        }
        void sessionActivated(const LLUUID&, const std::string&, const LLUUID&) override {}
        void sessionVoiceOrIMStarted(const LLUUID&) override {}
        void sessionRemoved(const LLUUID&) override {}
        void sessionIDUpdated(const LLUUID&, const LLUUID&) override {}
    } sObserver;

    bool tick()
    {
        static F64 next_tick = 0.0;
        if (LLApp::isExiting())
        {
            return true;
        }
        const F64 time = now();
        if (time < next_tick || !gIMMgr || !ensureLoaded())
        {
            return false;
        }
        next_tick = time + TICK_INTERVAL;

        static bool observing = false;
        if (!observing)
        {
            gIMMgr->addSessionObserver(&sObserver);
            observing = true;
        }

        noteGroups();
        resolveHeld();

        // Once the groups are known, a session that was let in just before (it was
        // open already when the switch was turned on) is judged again. If the wait ran
        // out before they came, it is judged once more when they have come and settled
        // (a group's chat let in by the wait is closed then).
        static std::string groups_applied_for;
        static bool        groups_settled_applied = false;
        if (groups_applied_for != sLists.dir)
        {
            if (groupsArrived())
            {
                groups_applied_for     = sLists.dir;
                groups_settled_applied = groupsSettled();
                applyToOpenSessions();
            }
        }
        else if (!groups_settled_applied && groupsSettled())
        {
            groups_settled_applied = true;
            applyToOpenSessions();
        }

        for (auto it = sRefused.begin(); it != sRefused.end();)
        {
            if (time - it->second > CLEANUP_TIME)
            {
                it = sRefused.erase(it);
            }
            else
            {
                clearPending(it->first);
                ++it;
            }
        }
        return false;
    }

    void init()
    {
        if (LLControlVariable* control = gSavedPerAccountSettings.getControl(WHITELIST_ONLY_SETTING))
        {
            control->getSignal()->connect([](LLControlVariable*, const LLSD&, const LLSD&)
            {
                ++sRevision;
                applyToOpenSessions();
            });
        }
        else
        {
            LL_WARNS("GroupWhitelist") << WHITELIST_ONLY_SETTING << " is not declared (settings_plus.xml)" << LL_ENDL;
        }
        doOnIdleRepeating(&tick);
    }

    plus::FeatureRegistration sRegistration({ "groupwhitelist", &init });
}

namespace plus::groupwhitelist
{
    Mode mode(const LLUUID& group_id)
    {
        if (!ensureLoaded())
        {
            return Mode::Normal;
        }
        if (sLists.blacklist.count(group_id))
        {
            return Mode::Blacklist;
        }
        if (sLists.whitelist.count(group_id))
        {
            return Mode::Whitelist;
        }
        return Mode::Normal;
    }

    void setMode(const LLUUID& group_id, Mode mode)
    {
        if (group_id.isNull() || !ensureLoaded())
        {
            return;
        }
        sLists.blacklist.erase(group_id);
        sLists.whitelist.erase(group_id);
        if (mode == Mode::Blacklist)
        {
            sLists.blacklist.insert(group_id);
        }
        else if (mode == Mode::Whitelist)
        {
            sLists.whitelist.insert(group_id);
        }
        sLists.known_groups.insert(group_id);
        ++sRevision;
        sAutoSaveBlocked = false; // the user changed the lists: they are what is wanted now
        save();
        applyToOpenSessions();
    }

    bool whitelistOnly()
    {
        return gSavedPerAccountSettings.controlExists(WHITELIST_ONLY_SETTING)
            && gSavedPerAccountSettings.getBOOL(WHITELIST_ONLY_SETTING);
    }

    void setWhitelistOnly(bool on)
    {
        if (!gSavedPerAccountSettings.controlExists(WHITELIST_ONLY_SETTING) || whitelistOnly() == on)
        {
            return;
        }
        gSavedPerAccountSettings.setBOOL(WHITELIST_ONLY_SETTING, on); // the signal closes what it refuses
        // Saved now, not only at logout: a crash must not bring the old setting back.
        if (LLStartUp::getStartupState() == STATE_STARTED)
        {
            plus::saveSettingsFile(gSavedPerAccountSettings, gSavedSettings.getString("PerAccountSettingsFile"), true);
        }
    }

    U32 revision()
    {
        return sRevision;
    }

    bool refuseInvitation(const LLSD& body)
    {
        if (body.has("instantmessage"))
        {
            const LLSD& params = body["instantmessage"]["message_params"];
            const LLUUID session_id = params["id"].asUUID();
            bool held = false;
            const bool refused = refuse(session_id, params["from_id"].asUUID(), &held);
            if (held)
            {
                // Kept to be let in once the groups show it is no refused group (resolveHeld).
                if (!sHeldInvitations.count(session_id))
                {
                    while (sHeldInvitationOrder.size() >= MAX_HELD_SESSIONS)
                    {
                        sHeldInvitations.erase(sHeldInvitationOrder.front());
                        sHeldInvitationOrder.pop_front();
                    }
                    sHeldInvitationOrder.push_back(session_id);
                }
                std::vector<LLSD>& lines = sHeldInvitations[session_id];
                if (lines.size() < MAX_HELD_LINES)
                {
                    lines.push_back(body);
                }
            }
            return refused;
        }
        if (body.has("voice") && body["voice"]["invitation_type"].asInteger() == P2P_CHAT_SESSION)
        {
            return false; // a person-to-person call
        }
        if (body.has("voice") || body.has("immediate"))
        {
            return refuse(body["session_id"].asUUID(), body["from_id"].asUUID());
        }
        return false;
    }

    bool refuseMessage(const LLUUID& session_id, const LLUUID& from_id, EInstantMessage dialog)
    {
        // Group and ad-hoc chat come with IM_SESSION_INVITE (the invitation and
        // IM_SESSION_SEND both pass it). Person-to-person IMs and the rest are never
        // judged: offline IMs come at login, before the groups.
        switch (dialog)
        {
        case IM_SESSION_INVITE:
        case IM_SESSION_GROUP_START:
        case IM_SESSION_CONFERENCE_START:
        case IM_SESSION_SEND:
            return refuse(session_id, from_id);
        default:
            return false;
        }
    }
}

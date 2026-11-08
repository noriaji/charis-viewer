/**
 * @file photoconv.cpp
 * @brief The photo screen's conversations: the model side (research/247, first stage, branch ps-im).
 *
 * What is here: while the photo screen is on, the official IM model's signals (a new message, a conversation
 * read) and the session observer (a conversation added, removed, its id changed when a group or conference
 * started) mark the list stale; an idle callback makes it again at most four times a second and tells the
 * conversations panels and the tabs' badges (onChanged()). Everything is read from LLIMModel's memory: the
 * names, the unread counts (the people's messages: mParticipantUnreadMessageCount, as the official chiclets),
 * the last messages. Nothing is read from files, nothing is sent but what you type (send(), with the official
 * window's function, so the packets are the official window's).
 *
 * The badge counts what the official notice settings tell of (Preferences > Chat > Notifications: friends,
 * others, conferences, groups): a kind set to "no action" and a muted sender are shown in the list in grey and
 * not on the badge (research/247 question 4 (a)).
 *
 * The notice: notify_of_message() (llimview.cpp) plays the official sound and then asks takesNotice(); while
 * the photo screen is shown it returns, so the toast at the top right, the flash and the conversations window
 * opening by itself are left out; the badge says it instead (research/247 question 3). Do Not Disturb, the
 * nearby chat, mouselook and a conversations window left open keep the official way.
 *
 * Speed: nothing runs while the photo screen is off. While on: one timer test an idle; the list (a handful of
 * conversations) is made only after a signal.
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

#include "photoconv.h"

#include "photoscreen.h"

#include <algorithm>
#include <map>

#include "llagent.h"
#include "llapp.h"
#include "llcallbacklist.h"
#include "llcallingcard.h"      // LLAvatarTracker
#include "llcommandmanager.h"
#include "lldbstrings.h"
#include "llfloaterimsession.h"
#include "llfloaterreg.h"
#include "llframetimer.h"
#include "llimview.h"
#include "llmutelist.h"
#include "lltoolbarview.h"
#include "llviewercontrol.h"

namespace plus::photoscreen::conv
{
    namespace
    {
        constexpr F32  TICK_SECONDS = 0.25f;
        constexpr char NO_ACTION[]  = "noaction";   // the official notice settings' "no action"

        // ---- what is kept -------------------------------------------------------------------------

        bool sStarted        = false;
        bool sTickRegistered = false;
        bool sDirty          = true;
        U32  sVersion        = 0;
        S32  sBadge          = 0;
        LLFrameTimer sTimer;

        std::vector<Entry> sEntries;
        LLUUID sChosen;
        std::map<LLUUID, std::string> sDrafts;      // typed and not sent, by conversation
        std::map<LLUUID, U32> sActivity;            // the order of the last message, by conversation
        std::map<LLUUID, LLUUID> sRenamed;          // a session's first id -> the one the server gave it
        U32 sActivityClock = 0;

        boost::signals2::scoped_connection sNewMsg, sNoUnread;
        changed_signal_t sChanged;

        void touch(const LLUUID& id)
        {
            if (id.notNull())
            {
                sActivity[id] = ++sActivityClock;
            }
        }

        // The official model's conversations change: a session added, removed, its id changed.
        class Observer : public LLIMSessionObserver
        {
        public:
            void sessionAdded(const LLUUID&, const std::string&, const LLUUID&, bool) override { sDirty = true; }
            void sessionActivated(const LLUUID&, const std::string&, const LLUUID&) override { sDirty = true; }
            void sessionVoiceOrIMStarted(const LLUUID&) override { sDirty = true; }

            void sessionRemoved(const LLUUID& id) override
            {
                sDrafts.erase(id);
                sActivity.erase(id);
                for (auto it = sRenamed.begin(); it != sRenamed.end();)
                {
                    it = it->first == id || it->second == id ? sRenamed.erase(it) : std::next(it);
                }
                sDirty = true;
            }

            // A group or conference got its id from the server: what was kept under the first id moves.
            void sessionIDUpdated(const LLUUID& old_id, const LLUUID& new_id) override
            {
                sRenamed[old_id] = new_id;
                if (sChosen == old_id)
                {
                    sChosen = new_id;
                }
                const auto draft = sDrafts.find(old_id);
                if (draft != sDrafts.end())
                {
                    sDrafts[new_id] = draft->second;
                    sDrafts.erase(old_id);
                }
                const auto activity = sActivity.find(old_id);
                if (activity != sActivity.end())
                {
                    sActivity[new_id] = activity->second;
                    sActivity.erase(old_id);
                }
                sDirty = true;
            }
        };
        Observer sObserver;

        void onNewMessage(const LLSD& data)
        {
            touch(data["session_id"].asUUID());
            sDirty = true;
        }

        void onNoUnread(const LLSD&)
        {
            sDirty = true;
        }

        // ---- the list -----------------------------------------------------------------------------

        Kind kindOf(const LLIMModel::LLIMSession& session)
        {
            if (session.isGroupSessionType())
            {
                return Kind::Group;
            }
            if (session.isAdHocSessionType())
            {
                return Kind::Conference;
            }
            return Kind::IM;
        }

        // The official notice setting of the session's kind (notify_of_message() reads the same ones).
        const std::string& noticeOption(const LLIMModel::LLIMSession& session)
        {
            static LLCachedControl<std::string> friend_im(gSavedSettings, "NotificationFriendIMOptions", "");
            static LLCachedControl<std::string> other_im(gSavedSettings, "NotificationNonFriendIMOptions", "");
            static LLCachedControl<std::string> conference(gSavedSettings, "NotificationConferenceIMOptions", "");
            static LLCachedControl<std::string> group(gSavedSettings, "NotificationGroupChatOptions", "");
            if (session.isGroupSessionType())
            {
                return group;
            }
            if (session.isAdHocSessionType())
            {
                return conference;
            }
            return LLAvatarTracker::instance().isBuddy(session.mOtherParticipantID) ? friend_im : other_im;
        }

        bool counted(const LLIMModel::LLIMSession& session)
        {
            return noticeOption(session) != NO_ACTION && !LLMuteList::getInstance()->isMuted(session.mOtherParticipantID);
        }

        S32 newestStamp(const LLIMModel::LLIMSession& session)
        {
            return session.mMsgs.empty() ? 0 : session.mMsgs.front()["timestamp"].asInteger();
        }

        void rebuild()
        {
            struct Keyed
            {
                Entry entry;
                U32   activity;
                S32   stamp;
            };
            std::vector<Keyed> keyed;
            sBadge = 0;
            for (const auto& [id, s] : LLIMModel::instance().mId2SessionMap)
            {
                if (!s || id.isNull())
                {
                    continue;
                }
                Keyed k;
                k.entry.id         = id;
                k.entry.name       = s->mName;
                k.entry.kind       = kindOf(*s);
                k.entry.unread     = llmax(0, s->mParticipantUnreadMessageCount);
                k.entry.counted    = counted(*s);
                k.entry.connecting = !s->mSessionInitialized;
                const auto activity = sActivity.find(id);
                k.activity = activity == sActivity.end() ? 0 : activity->second;
                k.stamp    = newestStamp(*s);
                if (k.entry.counted)
                {
                    sBadge += k.entry.unread;
                }
                keyed.push_back(std::move(k));
            }
            // unread that the badge counts first, then the latest talked in here, then the newest message, then by name
            std::stable_sort(keyed.begin(), keyed.end(), [](const Keyed& a, const Keyed& b) {
                const bool a_new = a.entry.counted && a.entry.unread > 0;
                const bool b_new = b.entry.counted && b.entry.unread > 0;
                if (a_new != b_new)
                {
                    return a_new;
                }
                if (a.activity != b.activity)
                {
                    return a.activity > b.activity;
                }
                if (a.stamp != b.stamp)
                {
                    return a.stamp > b.stamp;
                }
                return a.entry.name < b.entry.name;
            });
            sEntries.clear();
            sEntries.reserve(keyed.size());
            for (Keyed& k : keyed)
            {
                sEntries.push_back(std::move(k.entry));
            }
            if (sChosen.notNull() && !find(sChosen))
            {
                sChosen.setNull();      // gone: the panel chooses again when shown
            }
        }

        void refreshNow()
        {
            sDirty = false;
            rebuild();
            ++sVersion;
            sChanged();
        }

        // Four times a second while the photo screen is on (an idle callback; true removes it).
        bool tick()
        {
            if (!sStarted)
            {
                sTickRegistered = false;
                return true;
            }
            if (!sDirty || sTimer.getElapsedTimeF32() < TICK_SECONDS)
            {
                return false;
            }
            sTimer.reset();
            refreshNow();
            return false;
        }

        LLIMModel::LLIMSession* session(const LLUUID& id)
        {
            return id.isNull() ? nullptr : LLIMModel::instance().findIMSession(id);
        }
    }

    void start()
    {
        if (sStarted || !gIMMgr)
        {
            return;
        }
        sStarted = true;
        LLIMModel& model = LLIMModel::instance();
        sNewMsg   = model.addNewMsgCallback(&onNewMessage);
        sNoUnread = model.addNoUnreadMsgsCallback(&onNoUnread);
        gIMMgr->addSessionObserver(&sObserver);
        refreshNow();       // the badge is right at the first frame
        if (!sTickRegistered)
        {
            sTickRegistered = true;
            sTimer.reset();
            doOnIdleRepeating(&tick);
        }
    }

    void stop()
    {
        if (!sStarted)
        {
            return;
        }
        sStarted = false;
        sNewMsg.disconnect();
        sNoUnread.disconnect();
        if (gIMMgr)
        {
            gIMMgr->removeSessionObserver(&sObserver);
        }
        if (LLApp::isExiting() || !LLIMModel::instanceExists())
        {
            return;
        }
        rebuild();
        // The official flash was left out while the photo screen had the notice: once now for what is still unread.
        if (sBadge > 0 && gToolBarView && !gAgent.isDoNotDisturb())
        {
            gToolBarView->flashCommand(LLCommandId("chat"), true);
        }
    }

    bool started()
    {
        return sStarted;
    }

    const std::vector<Entry>& entries()
    {
        return sEntries;
    }

    const Entry* find(const LLUUID& id)
    {
        for (const Entry& entry : sEntries)
        {
            if (entry.id == id)
            {
                return &entry;
            }
        }
        return nullptr;
    }

    S32 badgeCount()
    {
        return sBadge;
    }

    U32 version()
    {
        return sVersion;
    }

    boost::signals2::connection onChanged(const changed_signal_t::slot_type& slot)
    {
        return sChanged.connect(slot);
    }

    const LLUUID& chosen()
    {
        return sChosen;
    }

    void choose(const LLUUID& id)
    {
        sChosen = find(id) ? id : LLUUID::null;
    }

    LLUUID currentId(const LLUUID& id)
    {
        const auto it = sRenamed.find(id);
        return it == sRenamed.end() ? id : it->second;
    }

    void chooseForOpening()
    {
        const Entry* newest = nullptr;
        for (const Entry& entry : sEntries)
        {
            if (entry.counted && entry.unread > 0)
            {
                newest = &entry;    // the list has them first, the latest first
                break;
            }
        }
        if (newest)
        {
            sChosen = newest->id;
        }
        else if (!find(sChosen))
        {
            sChosen = sEntries.empty() ? LLUUID::null : sEntries.front().id;
        }
    }

    void lines(const LLUUID& id, size_t count, std::vector<Line>& out)
    {
        out.clear();
        const LLIMModel::LLIMSession* s = session(id);
        if (!s)
        {
            return;
        }
        // mMsgs has the newest first
        for (const LLSD& message : s->mMsgs)
        {
            if (out.size() >= count)
            {
                break;
            }
            Line line;
            const LLUUID from_id = message["from_id"].asUUID();
            line.from   = message["from"].asString();
            line.text   = message["message"].asString();
            line.own    = from_id == gAgentID;
            line.system = from_id.isNull();
            out.push_back(std::move(line));
        }
        std::reverse(out.begin(), out.end());
    }

    std::string speakerLine(const std::string& who, const std::string& text)
    {
        if (who.empty())
        {
            return text;
        }
        if (text.compare(0, 3, "/me") == 0 && (text.size() == 3 || text[3] == ' ' || text[3] == '\''))
        {
            const bool space = text.size() > 3 && text[3] == ' ';
            return who + (space ? " " : "") + text.substr(space ? 4 : 3);
        }
        return who + ": " + text;
    }

    void markRead(const LLUUID& id)
    {
        const LLIMModel::LLIMSession* s = session(id);
        if (s && (s->mNumUnread > 0 || s->mParticipantUnreadMessageCount > 0))
        {
            LLIMModel::instance().sendNoUnreadMessages(id);     // the official counters and windows follow
        }
    }

    std::string draft(const LLUUID& id)
    {
        const auto it = sDrafts.find(id);
        return it == sDrafts.end() ? std::string() : it->second;
    }

    void setDraft(const LLUUID& id, const std::string& text)
    {
        if (id.isNull())
        {
            return;
        }
        if (text.empty())
        {
            sDrafts.erase(id);
        }
        else
        {
            sDrafts[id] = text;
        }
    }

    SendResult send(const LLUUID& sent_to, const std::string& text)
    {
        const LLUUID id = currentId(sent_to);   // a group or conference that got its id from the server meanwhile
        std::string utf8_text = text;
        LLStringUtil::trim(utf8_text);
        if (utf8_text.empty())
        {
            return SendResult::Empty;
        }
        LLIMModel::LLIMSession* s = session(id);
        if (!s || !s->mTextIMPossible)
        {
            return SendResult::Refused;
        }
        // as the official window's sendMsgFromInputEditor(): no IM to everyone
        if (!gAgent.isGodlike() && s->mType == IM_NOTHING_SPECIAL && s->mOtherParticipantID.isNull())
        {
            return SendResult::Refused;
        }
        if (LLFloaterIMSession* floater = LLFloaterIMSession::findInstance(id))
        {
            floater->sendMsg(utf8_text);    // queued by it until a group or conference has started
        }
        else if (s->mSessionInitialized)
        {
            LLIMModel::sendMessage(utf8str_truncate(utf8_text, MAX_MSG_BUF_SIZE - 1), id, s->mOtherParticipantID, s->mType);
        }
        else
        {
            return SendResult::Waiting;
        }
        sDrafts.erase(id);
        touch(id);
        sDirty = true;
        return SendResult::Sent;
    }

    bool takesNotice(const LLUUID& session_id)
    {
        if (!sStarted || session_id.isNull() || !plus::photoscreen::isActive() || gAgent.isDoNotDisturb())
        {
            return false;
        }
        LLRect world;
        if (!plus::photoscreen::shownWorldRect(world))
        {
            return false;   // mouselook: the frame and its badge are not shown
        }
        // a conversations window left open is the user's: the official notice goes on there
        LLFloater* container = LLFloaterReg::findInstance("im_container");
        if (LLFloater::isVisible(container) && !container->isMinimized())
        {
            return false;
        }
        LLFloaterIMSession* floater = LLFloaterIMSession::findInstance(session_id);
        if (floater && floater->isTornOff() && LLFloater::isVisible(floater) && !floater->isMinimized())
        {
            return false;
        }
        return true;
    }
}

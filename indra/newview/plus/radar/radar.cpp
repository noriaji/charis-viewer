/**
 * @file radar.cpp
 * @brief Radar: the sweep over the avatars around, the entries and the reports.
 *
 * Ported from Firestorm's fsradar.cpp and fsradarentry.cpp (see ORIGIN.md).
 *
 * $LicenseInfo:firstyear=2013&license=viewerlgpl$
 * Copyright (c) 2013 Ansariel Hiller @ Second Life
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
 *
 * The Phoenix Firestorm Project, Inc., 1831 Oakwood Drive, Fairmont, Minnesota 56031-3225 USA
 * http://www.firestormviewer.org
 * $/LicenseInfo$
 */

// Settings (gSavedSettings, shared by every account):
//   PlusRadarNameFormat        NameFormat of the names
//   PlusRadarLimitRange        list only the avatars within PlusRadarRange (m)
//   PlusRadarShowMuted         list the muted avatars too
//   PlusRadarReport<Chat|Draw|Region><Enter|Leave>   a line in the nearby chat when one comes
//                              into or goes out of chat range, the draw distance, the region
//
// Ported from Firestorm's FSRadar::updateRadarList(); the comparison of one sweep with the
// one before is the same, with these differences: the entries keep what the comparison needs
// (Firestorm has a second map for it), nothing is reported in the first sweep after the
// radar starts or after the region you are in changes (all the avatars would "come in" at
// once), and the lines name the avatar with a link (the nearby chat shows its name).
// The range limit and hiding the muted only take rows out of the list: those avatars are
// still followed (Entry::hidden), so crossing the limit, or the window's switches, report
// nothing (Firestorm drops them from its list and reports them as gone and back).

#include "llviewerprecompiledheaders.h"

#include "radar.h"
#include "plus.h"
#include "plus/rlv/rlvhooks.h"

#include <algorithm>

#include "llagent.h"
#include "llanimationstates.h"
#include "llavatarname.h"
#include "llavatarnamecache.h"
#include "llcallingcard.h"
#include "llchat.h"
#include "lleventtimer.h"
#include "llframetimer.h"
#include "llmutelist.h"
#include "llnotificationmanager.h"
#include "lltracker.h"
#include "lltrans.h"
#include "llviewercontrol.h"
#include "llviewerobjectlist.h"
#include "llviewerparcelmgr.h"
#include "llviewerregion.h"
#include "llvoavatar.h"
#include "llworld.h"

namespace plus::radar
{
    namespace
    {
        constexpr F32 SWEEP_INTERVAL = 1.f;

        // A profile's answer is used again for this long (the age changes once a day, the
        // payment flags hardly ever); one asked for and not answered is asked again after
        // PROFILE_RETRY. The table is pruned every PROFILE_PRUNE_SWEEPS sweeps.
        constexpr F64 PROFILE_KEEP         = 30.0 * 60.0;
        constexpr F64 PROFILE_RETRY        = 60.0;
        constexpr U32 PROFILE_PRUNE_SWEEPS = 60;

        constexpr const char* REPORT_SETTINGS[] = {
            "PlusRadarReportChatEnter", "PlusRadarReportChatLeave",
            "PlusRadarReportDrawEnter", "PlusRadarReportDrawLeave",
            "PlusRadarReportRegionEnter", "PlusRadarReportRegionLeave",
        };

        bool inChat(F32 range)
        {
            return range > UNKNOWN_RANGE && range <= CHAT_RANGE_SAY;
        }

        bool inDraw(F32 range, F32 draw_distance)
        {
            return range > UNKNOWN_RANGE && range <= draw_distance;
        }
    }

    class Engine::Timer : public LLEventTimer
    {
    public:
        explicit Timer(Engine& engine) : LLEventTimer(SWEEP_INTERVAL), mEngine(engine) {}

        // True: the engine has nothing to do and let go of this timer (it is deleted).
        bool tick() override { return mEngine.sweep(); }

    private:
        Engine& mEngine;
    };

    Engine::Engine()
    {
        // The settings the run state and the names depend on.
        for (const char* name : REPORT_SETTINGS)
        {
            mConnections.push_back(gSavedSettings.getControl(name)->getSignal()->connect(
                [this](LLControlVariable*, const LLSD&, const LLSD&) { applyRunState(); }));
        }
        mConnections.push_back(gSavedSettings.getControl("PlusRadarNameFormat")->getSignal()->connect(
            [this](LLControlVariable*, const LLSD&, const LLSD&) { refreshNames(); }));
        mConnections.push_back(gSavedSettings.getControl("UseDisplayNames")->getSignal()->connect(
            [this](LLControlVariable*, const LLSD&, const LLSD&) { refreshNames(); }));
    }

    Engine::~Engine()
    {
        for (boost::signals2::connection& connection : mConnections)
        {
            connection.disconnect();
        }
        delete mTimer;
        mTimer = nullptr;
    }

    void Engine::cleanupSingleton()
    {
        mListener = nullptr;
        clearEntries();
    }

    // static
    bool Engine::reportsOn()
    {
        for (const char* name : REPORT_SETTINGS)
        {
            if (gSavedSettings.getBOOL(name))
            {
                return true;
            }
        }
        return false;
    }

    void Engine::setListener(std::function<void()> listener)
    {
        mListener = std::move(listener);
    }

    void Engine::setWindowOpen(bool open)
    {
        mWindowOpen = open;
        applyRunState();
    }

    void Engine::applyRunState()
    {
        if (mWindowOpen || reportsOn())
        {
            start();
        }
        else
        {
            stop();
        }
    }

    void Engine::start()
    {
        if (mTimer)
        {
            return;
        }
        // What is around when the radar starts is not "coming in".
        mQuietSweeps = 1;
        mTimer       = new Timer(*this);
        sweep();
    }

    void Engine::stop()
    {
        if (!mTimer)
        {
            return;
        }
        delete mTimer;
        mTimer = nullptr;
        clearEntries();
        mTrackedId.setNull();
        if (mListener)
        {
            mListener();
        }
    }

    void Engine::clearEntries()
    {
        // The profiles go too: nothing is left observed when the radar stops (or the engine goes).
        if (LLAvatarPropertiesProcessor::instanceExists())
        {
            LLAvatarPropertiesProcessor* processor = LLAvatarPropertiesProcessor::getInstance();
            for (const auto& [id, profile] : mProfiles)
            {
                if (profile.observing)
                {
                    processor->removeObserver(id, this);
                }
            }
        }
        mProfiles.clear();
        mEntries.clear();
        mRows.clear();
        mStats = Stats();
    }

    const Entry* Engine::find(const LLUUID& id) const
    {
        const auto found = mEntries.find(id);
        return found == mEntries.end() ? nullptr : &found->second;
    }

    // static
    std::string Engine::formatName(const LLAvatarName& av_name)
    {
        // Without display names there is one name to show.
        if (!gSavedSettings.getBOOL("UseDisplayNames"))
        {
            return av_name.getUserName();
        }
        const std::string display = av_name.getDisplayName();
        const std::string user    = av_name.getUserName();
        switch (gSavedSettings.getU32("PlusRadarNameFormat"))
        {
            case NAME_USER:
                return user;
            case NAME_DISPLAY_USER:
                return av_name.isDisplayNameDefault() ? user : display + " (" + user + ")";
            case NAME_USER_DISPLAY:
                return av_name.isDisplayNameDefault() ? user : user + " (" + display + ")";
            default:
                return display;
        }
    }

    void Engine::requestName(const LLUUID& id)
    {
        LLAvatarName av_name;
        if (LLAvatarNameCache::get(id, &av_name))
        {
            onName(id, av_name);
            return;
        }
        LLAvatarNameCache::get(id, [](const LLUUID& av_id, const LLAvatarName& name) {
            if (Engine::instanceExists())
            {
                Engine::getInstance()->onName(av_id, name);
            }
        });
    }

    void Engine::onName(const LLUUID& id, const LLAvatarName& av_name)
    {
        const auto found = mEntries.find(id);
        if (found != mEntries.end())
        {
            found->second.name = formatName(av_name);
            // RLVa @shownames: the anonym (FSRadarEntry::getRadarName()). The tracking beacon, the quick filter
            // and the clipboard take the name from here too.
            if (plus::rlv::enabled())
            {
                found->second.name = plus::rlv::avatarNameForPlus(id, found->second.name);
            }
        }
    }

    void Engine::refreshNames()
    {
        for (const auto& [id, entry] : mEntries)
        {
            requestName(id);
        }
        if (mListener)
        {
            mListener();
        }
    }

    // A new entry's age and flags: from an answer of the last PROFILE_KEEP, or from the answer
    // to come (processProperties() fills the entry then), or asked for now.
    void Engine::requestProperties(Entry& entry)
    {
        const F64 now    = LLFrameTimer::getElapsedSeconds();
        Profile& profile = mProfiles[entry.id];
        if (profile.known && now - profile.knownAt < PROFILE_KEEP)
        {
            entry.age   = profile.age;
            entry.flags = profile.flags;
            return;
        }
        if (profile.observing && now - profile.askedAt < PROFILE_RETRY)
        {
            return;
        }
        LLAvatarPropertiesProcessor* processor = LLAvatarPropertiesProcessor::getInstance();
        processor->addObserver(entry.id, this);   // once per avatar (the processor checks)
        processor->sendAvatarPropertiesRequest(entry.id);
        profile.observing = true;
        profile.askedAt   = now;
    }

    // Answers older than PROFILE_KEEP and requests unanswered for PROFILE_RETRY go, so the table
    // holds the avatars of the last half hour, not of the session.
    void Engine::pruneProfiles(F64 now)
    {
        LLAvatarPropertiesProcessor* processor = LLAvatarPropertiesProcessor::getInstance();
        for (auto it = mProfiles.begin(); it != mProfiles.end();)
        {
            Profile& profile = it->second;
            if (profile.observing && now - profile.askedAt >= PROFILE_RETRY)
            {
                processor->removeObserver(it->first, this);
                profile.observing = false;
            }
            if (!profile.observing && (!profile.known || now - profile.knownAt >= PROFILE_KEEP))
            {
                it = mProfiles.erase(it);
            }
            else
            {
                ++it;
            }
        }
        // Those still in the list whose answer never came are asked again.
        for (auto& [id, entry] : mEntries)
        {
            if (entry.asked && entry.age == -1)
            {
                requestProperties(entry);
            }
        }
    }

    void Engine::processProperties(void* data, EAvatarProcessorType type)
    {
        if (!data || (type != APT_PROPERTIES && type != APT_PROPERTIES_LEGACY))
        {
            return;
        }
        // The two carry the same fields we use: the flags and the date of birth.
        LLUUID avatar_id;
        U32 flags = 0;
        LLDate born_on;
        bool hide_age = false;
        if (type == APT_PROPERTIES)
        {
            const LLAvatarData* avatar_data = static_cast<const LLAvatarData*>(data);
            avatar_id = avatar_data->avatar_id;
            flags     = avatar_data->flags;
            born_on   = avatar_data->born_on;
            hide_age  = avatar_data->hide_age;
        }
        else
        {
            const LLAvatarLegacyData* avatar_data = static_cast<const LLAvatarLegacyData*>(data);
            avatar_id = avatar_data->avatar_id;
            flags     = avatar_data->flags;
            born_on   = avatar_data->born_on;
        }
        S32 age = -1;
        if (hide_age)
        {
            age = -2;
        }
        else if (born_on.secondsSinceEpoch() <= 0.0)
        {
            age = -3; // no date sent: not 1970, which would give some 20,000 days
        }
        else
        {
            // never below 0: a clock behind the server would give -1..-3, the marks above
            age = llmax(0, (S32)((LLDate::now().secondsSinceEpoch() - born_on.secondsSinceEpoch()) / 86400));
        }

        // Kept for the next time the avatar comes into the list; the answer is in, so this engine
        // stops observing it (the processor notifies from a copy of its observers: safe here).
        const auto profile = mProfiles.find(avatar_id);
        if (profile != mProfiles.end())
        {
            profile->second.age     = age;
            profile->second.flags   = flags;
            profile->second.known   = true;
            profile->second.knownAt = LLFrameTimer::getElapsedSeconds();
            if (profile->second.observing)
            {
                profile->second.observing = false;
                LLAvatarPropertiesProcessor::getInstance()->removeObserver(avatar_id, this);
            }
        }

        const auto found = mEntries.find(avatar_id);
        if (found == mEntries.end())
        {
            return;
        }
        found->second.flags = flags;
        found->second.age   = age;
    }

    void Engine::report(const LLUUID& id, const char* string_id, F32 distance)
    {
        LLStringUtil::format_map_t args;
        args["DISTANCE"] = llformat("%3.2f", distance);
        LLChat chat;
        // The nearby chat turns the link into the name.
        // <plus> rlv: @shownames - the link of a hidden name is the anonym's (FS: radarAlertMsg) (research/248 H1) </plus>
        const char* verb = (plus::rlv::enabled() && !plus::rlv::canShowMenuName(id)) ? "/rlvanonym " : "/about ";
        chat.mText       = "secondlife:///app/agent/" + id.asString() + verb + LLTrans::getString(string_id, args);
        chat.mSourceType = CHAT_SOURCE_SYSTEM;
        LLNotificationsUI::LLNotificationManager::instance().onChat(chat, LLSD());
    }

    void Engine::startTracking(const LLUUID& id)
    {
        const Entry* entry = find(id);
        if (!entry || entry->range <= UNKNOWN_RANGE)
        {
            return;
        }
        mTrackedId = id;
        LLTracker::trackLocation(entry->position, entry->name, "");
    }

    void Engine::updateTracking()
    {
        if (mTrackedId.isNull())
        {
            return;
        }
        const Entry* entry = find(mTrackedId);
        if (LLTracker::getTrackingStatus() != LLTracker::TRACKING_LOCATION)
        {
            // Something else is tracked now.
            mTrackedId.setNull();
        }
        else if (!entry || entry->hidden || entry->range <= UNKNOWN_RANGE)
        {
            // The avatar is gone (or out of the list: beyond its range limit, or blocked).
            LLTracker::stopTracking(false);
            mTrackedId.setNull();
        }
        else if (LLTracker::getTrackedPositionGlobal() != entry->position)
        {
            LLTracker::trackLocation(entry->position, entry->name, "");
        }
    }

    void Engine::sweepQuietly()
    {
        if (!mTimer)
        {
            return;     // not running: nothing to sweep (start() sweeps quietly anyway)
        }
        mQuietSweeps = llmax(mQuietSweeps, 1);
        sweep();
    }

    bool Engine::sweep()
    {
        if (!mWindowOpen && !reportsOn())
        {
            // Nothing needs the radar: the timer goes (the caller deletes it) and what it knew.
            mTimer = nullptr;
            clearEntries();
            mTrackedId.setNull();
            if (mListener)
            {
                mListener();
            }
            return true;
        }

        LLWorld* world = LLWorld::getInstance();
        const LLVector3d self_position = gAgent.getPositionGlobal();
        LLUUID self_region;
        if (const LLViewerRegion* region = gAgent.getRegion())
        {
            self_region = region->getRegionID();
        }
        if (self_region != mSelfRegion)
        {
            // Everyone around is new, and the ones of the old region go (below): no reports of
            // either. Nobody tells the radar that the old region has gone.
            mSelfRegion  = self_region;
            mQuietSweeps = llmax(mQuietSweeps, 2);
        }
        // After a teleport far away the old region stays in the viewer's world for a while (until
        // its simulator lets it go), and getAvatars() still gives the avatars in it: they are not
        // around (plus::isRegionAround(), by distance: regions a grid connects beyond the eight
        // neighbours stay; fix-round9b 2, fix-round10 2). While plus/tprescue keeps the old
        // region for a slow arrival the agent is still in it, so it is around until the arrival.
        const bool quiet = mQuietSweeps > 0;
        if (quiet)
        {
            --mQuietSweeps;
        }

        const bool limit        = gSavedSettings.getBOOL("PlusRadarLimitRange");
        const F32 limit_range   = gSavedSettings.getF32("PlusRadarRange");
        const bool show_muted   = gSavedSettings.getBOOL("PlusRadarShowMuted");
        const F32 draw_distance = gSavedSettings.getF32("RenderFarClip");
        const bool report_chat_enter   = gSavedSettings.getBOOL("PlusRadarReportChatEnter");
        const bool report_chat_leave   = gSavedSettings.getBOOL("PlusRadarReportChatLeave");
        const bool report_draw_enter   = gSavedSettings.getBOOL("PlusRadarReportDrawEnter");
        const bool report_draw_leave   = gSavedSettings.getBOOL("PlusRadarReportDrawLeave");
        const bool report_region_enter = gSavedSettings.getBOOL("PlusRadarReportRegionEnter");
        const bool report_region_leave = gSavedSettings.getBOOL("PlusRadarReportRegionLeave");

        // Everyone around, also beyond the list's range limit: those are followed (hidden) so
        // that the reports tell when they really come and go, not when they cross the limit.
        uuid_vec_t ids;
        std::vector<LLVector3d> positions;
        world->getAvatars(&ids, &positions);

        const time_t now = time(nullptr);
        ++mSweepCount;
        for (size_t i = 0; i < ids.size() && i < positions.size(); ++i)
        {
            const LLUUID& id = ids[i];
            if (id == gAgentID)
            {
                continue;
            }
            const LLVector3d& position = positions[i];
            LLViewerRegion* avatar_region = world->getRegionFromPosGlobal(position);
            if (!plus::isRegionAround(avatar_region))
            {
                continue;   // in a region that is not around (one left behind by a teleport): gone
            }
            const bool muted = LLMuteList::getInstance()->isMuted(id);
            const bool muted_hidden = muted && !show_muted;     // no reports of them either

            LLVOAvatar* avatar         = dynamic_cast<LLVOAvatar*>(gObjectList.findObject(id));
            // Above the minimap's reach the height is not known until the avatar is drawn.
            const F32 range = (position.mdV[VZ] >= UNKNOWN_Z && !avatar)
                                  ? UNKNOWN_RANGE
                                  : (F32)dist_vec(position, self_position);
            // The limit as the world's search by radius had it: by the position as it is known (one
            // above the minimap's reach has the reach's height until it is drawn).
            const bool hidden = muted_hidden || (limit && (F32)dist_vec(position, self_position) > limit_range);

            const auto [it, is_new] = mEntries.try_emplace(id);
            Entry& entry            = it->second;
            if (is_new)
            {
                entry.id        = id;
                entry.firstSeen = now;
                entry.name      = LLTrans::getString("AvatarNameWaiting");
            }
            if (!hidden && !entry.asked)
            {
                // Asked once it is listed: not for everyone of the region beyond the limit.
                // (A profile asked for stays observed: its answer is kept for later.)
                entry.asked = true;
                requestName(id);
                requestProperties(entry);
            }

            LLUUID region_id;
            if (avatar_region)
            {
                region_id = avatar_region->getRegionID();
            }
            const bool in_region = region_id.notNull() && region_id == self_region;

            if (!quiet && !muted_hidden)
            {
                // A new entry has nothing before it: every range it is in, it comes into.
                const bool was_chat = !is_new && inChat(entry.lastRange);
                const bool was_draw = !is_new && inDraw(entry.lastRange, draw_distance);
                if (report_chat_enter && inChat(range) && !was_chat)
                {
                    report(id, "PlusRadarChatEnter", range);
                }
                else if (report_chat_leave && !inChat(range) && was_chat)
                {
                    report(id, "PlusRadarChatLeave", range);
                }
                if (report_draw_enter && inDraw(range, draw_distance) && !was_draw)
                {
                    report(id, "PlusRadarDrawEnter", range);
                }
                else if (report_draw_leave && !inDraw(range, draw_distance) && was_draw)
                {
                    report(id, "PlusRadarDrawLeave", range);
                }
                if (report_region_enter && in_region && (is_new || (region_id != entry.lastRegion && entry.lastRegion.notNull())))
                {
                    // The distance is not told when it is not known.
                    report(id, range > UNKNOWN_RANGE ? "PlusRadarRegionEnterDistance" : "PlusRadarRegionEnter", range);
                }
                else if (report_region_leave && !is_new && self_region.notNull() && entry.lastRegion == self_region
                         && !in_region && region_id.notNull())
                {
                    report(id, "PlusRadarRegionLeave", range);
                }
            }

            entry.range      = range;
            entry.position   = position;
            entry.region     = region_id;
            entry.lastRange  = range;
            entry.lastRegion = region_id;
            entry.sweep      = mSweepCount;
            entry.inRegion   = in_region;
            entry.onParcel   = LLViewerParcelMgr::getInstance()->inAgentParcel(position);
            entry.muted      = muted;
            entry.hidden     = hidden;
            entry.friendship = LLAvatarTracker::instance().getBuddyInfo(id) != nullptr;
            entry.inDraw     = avatar && inDraw(range, draw_distance);
            entry.sitting    = avatar && (avatar->getParent() || avatar->isSitting()
                                          || avatar->isMotionActive(ANIM_AGENT_SIT_GROUND)
                                          || avatar->isMotionActive(ANIM_AGENT_SIT_GROUND_CONSTRAINED));
        }

        // Gone since the last sweep (left the regions around, logged off, teleported away). What
        // their profiles told stays in mProfiles (pruned here now and then).
        if (mSweepCount % PROFILE_PRUNE_SWEEPS == 0)
        {
            pruneProfiles(LLFrameTimer::getElapsedSeconds());
        }
        for (auto it = mEntries.begin(); it != mEntries.end();)
        {
            const Entry& entry = it->second;
            if (entry.sweep == mSweepCount)
            {
                ++it;
                continue;
            }
            if (!quiet && !(entry.muted && !show_muted))
            {
                if (report_chat_leave && inChat(entry.lastRange))
                {
                    report(entry.id, "PlusRadarChatLeave", entry.lastRange);
                }
                if (report_draw_leave && inDraw(entry.lastRange, draw_distance))
                {
                    report(entry.id, "PlusRadarDrawLeave", entry.lastRange);
                }
                // Only when the region is known: one whose region was never known may not have been in ours.
                if (report_region_leave && entry.lastRegion.notNull() && entry.lastRegion == self_region)
                {
                    report(entry.id, "PlusRadarRegionLeave", entry.lastRange);
                }
            }
            it = mEntries.erase(it);
        }

        // The rows of the window: nearest first, those whose range is not known last.
        mRows.clear();
        mStats = Stats();
        for (const auto& [id, entry] : mEntries)
        {
            if (entry.hidden)
            {
                continue;
            }
            mRows.push_back(&entry);
            ++mStats.total;
            mStats.region    += entry.inRegion ? 1 : 0;
            mStats.chatRange += inChat(entry.range) ? 1 : 0;
        }
        std::sort(mRows.begin(), mRows.end(), [](const Entry* a, const Entry* b) {
            const bool a_known = a->range > UNKNOWN_RANGE;
            const bool b_known = b->range > UNKNOWN_RANGE;
            if (a_known != b_known)
            {
                return a_known;
            }
            return a_known ? a->range < b->range : a->id < b->id;
        });

        updateTracking();
        if (mListener)
        {
            mListener();
        }
        return false;
    }

    void refreshNames()
    {
        if (Engine::instanceExists())
        {
            Engine::getInstance()->refreshNames();
        }
    }
}

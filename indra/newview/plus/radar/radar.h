/**
 * @file radar.h
 * @brief Radar: the avatars around you (distance, region, parcel, sitting, age, time seen) and
 *        the reports when one comes into or goes out of chat range, draw distance or the region.
 *
 * Ported from Firestorm's fsradar.h and fsradarentry.h (see ORIGIN.md).
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

// From Firestorm's FSRadar (fsradar.*, fsradarentry.*). The engine knows nothing of the
// window: radarfloater.cpp shows what rows() holds after each sweep. It sweeps once a second
// while the window is open or any report is switched on, and not at all otherwise.
//
// Left out of Firestorm's: the LSL bridge (heights above 1020 m and the alerts to scripts),
// contact sets, the asset blacklist, voice levels, notes, the age alert and the alert sounds
// (none of them exists here), and RLVa's @shownearby (not in Charis' RLVa yet). RLVa's
// @shownames is Firestorm's: the names anonymized, the age and the friends' bold hidden, no
// menu on the list (plus/rlv, research/248 3.4).

#ifndef PLUS_RADAR_H
#define PLUS_RADAR_H

#include <ctime>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "llavatarpropertiesprocessor.h"
#include "llsingleton.h"
#include "lluuid.h"
#include "v3dmath.h"

class LLAvatarName;

namespace plus::radar
{
    // Heights of the minimap's avatar dots stop at this; higher is "not known" (no object yet).
    constexpr F64 UNKNOWN_Z     = 1020.0;
    constexpr F32 UNKNOWN_RANGE = -1.f;

    // Where one is heard (the simulator's say and shout ranges).
    constexpr F32 CHAT_RANGE_SAY   = 20.f;
    constexpr F32 CHAT_RANGE_SHOUT = 100.f;

    // PlusRadarNameFormat
    enum NameFormat : U32
    {
        NAME_DISPLAY,
        NAME_USER,
        NAME_DISPLAY_USER,
        NAME_USER_DISPLAY,
    };

    struct Entry
    {
        LLUUID      id;
        std::string name;               // as the name format says
        F32         range = UNKNOWN_RANGE;
        LLVector3d  position;
        LLUUID      region;
        time_t      firstSeen = 0;
        S32         age = -1;           // days; -1 not known (yet), -2 hidden by the resident, -3 no birth date in the reply
        U32         flags = 0;          // AVATAR_* of the profile
        bool        inRegion = false;   // in the region you are in
        bool        onParcel = false;   // on the parcel you are on
        bool        sitting = false;
        bool        muted = false;
        bool        friendship = false;
        bool        inDraw = false;     // within the draw distance and the object exists

        // What the reports compare with.
        F32         lastRange = UNKNOWN_RANGE;
        LLUUID      lastRegion;
        U32         sweep = 0;          // the sweep that saw it last

        // Not in the list, but still followed for the reports: beyond the list's range limit,
        // or blocked while blocked avatars are not listed (they make no reports then). Coming
        // into the list again is not "coming in", nor is going out of it "leaving".
        bool        hidden = false;
        bool        asked = false;      // its name and profile asked for (once it was listed)
    };

    struct Stats
    {
        S32 total = 0;
        S32 region = 0;
        S32 chatRange = 0;
    };

    class Engine : public LLSingleton<Engine>, public LLAvatarPropertiesObserver
    {
        LLSINGLETON(Engine);
        ~Engine() override;

    public:
        // The window: it is told after each sweep, and keeps the engine running while it is open.
        void setListener(std::function<void()> listener);
        void setWindowOpen(bool open);

        // Runs when the window is open or a report is on (settings change, log in).
        void applyRunState();

        // The avatars of the last sweep, nearest first (not the hidden ones).
        const std::vector<const Entry*>& rows() const { return mRows; }
        const Stats& stats() const { return mStats; }
        const Entry* find(const LLUUID& id) const;      // hidden ones too (Entry::hidden)

        // Names again (the name format or the display name setting changed).
        void refreshNames();

        // The map's marker (and its beacon) follow the avatar.
        void startTracking(const LLUUID& id);

        // The text of a name in the format the setting says.
        static std::string formatName(const LLAvatarName& av_name);

        void processProperties(void* data, EAvatarProcessorType type) override;

        // One sweep over the avatars around: the timer's. True when nothing needs the radar any
        // more (the timer is then deleted by its caller).
        bool sweep();

        // A sweep now, without reports: the window's, after a change of who is listed.
        void sweepQuietly();

    protected:
        void cleanupSingleton() override;

    private:
        class Timer;

        static bool reportsOn();
        void start();
        void stop();
        void clearEntries();
        void requestProperties(Entry& entry);
        void pruneProfiles(F64 now);
        void requestName(const LLUUID& id);
        void onName(const LLUUID& id, const LLAvatarName& av_name);
        void report(const LLUUID& id, const char* string_id, F32 distance);
        void updateTracking();

        // What a profile told (the age and the payment flags), kept a while for those who come
        // and go (at the edge of the range or of a region), so they are not asked about each time.
        struct Profile
        {
            S32  age = -1;
            U32  flags = 0;
            bool known = false;         // an answer came
            bool observing = false;     // asked, this engine is the processor's observer for it
            F64  knownAt = 0.0;         // viewer seconds
            F64  askedAt = 0.0;
        };
        std::unordered_map<LLUUID, Profile> mProfiles;

        std::unordered_map<LLUUID, Entry> mEntries;
        std::vector<const Entry*> mRows;
        Stats mStats;
        std::function<void()> mListener;
        Timer* mTimer = nullptr;
        bool mWindowOpen = false;
        U32 mSweepCount = 0;
        LLUUID mSelfRegion;
        S32 mQuietSweeps = 0;       // sweeps left without reports (the first, a region change)
        LLUUID mTrackedId;
        std::vector<boost::signals2::connection> mConnections;
    };

    // The names again when the radar runs (RLVa's @shownames switched on or off: plus/rlv). Nothing when it does not.
    void refreshNames();
}

#endif // PLUS_RADAR_H

/**
 * @file lslbridge.cpp
 * @brief The pose bridge (plus/lslbridge, research/336): makes, wears and repairs the attachment that carries
 *        the pose-consent messages, talks to its script over HTTP-in, and takes its llOwnerSay lines.
 *
 * Ported from Firestorm's LSL bridge (fslslbridge.cpp, LGPL 2.1 only; see ORIGIN.md): the library object
 * copied into our own folder, worn on the HUD, made small and clear, given its script, and the script's
 * HTTP-in URL learned from an llOwnerSay line. What Firestorm's bridge does besides (radar altitudes, script
 * info, movelock, flight assist, the AO integrations) is not here. Charis checks more: the bridge object is
 * only the one it made and recorded for the account (never one found by its name), of one prim; the line must
 * come from it; the URL must be a Second Life simulator's and its certificate name its host; the script takes
 * a token.
 * The steps and states are the table "lslbridge" (an appendix) of research/336-poser-aurora-compat.md.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * The parts taken from Firestorm's fslslbridge.cpp (making and wearing the bridge, makeSmallAndClear()):
 * Copyright (C) 2011-2017, The Phoenix Firestorm Project, Inc.
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

#include "lslbridge.h"
#include "lslbridgehooks.h"
#include "lslbridgeproto.h"

#include "plus.h"
#include "plus/invguard/invguard.h"
#include "plus/safemode/safemode.h"

#include "llagent.h"
#include "llagentwearables.h"
#include "llapp.h"
#include "llappearancemgr.h"
#include "llattachmentsmgr.h"
#include "llcallbacklist.h"
#include "llchat.h"
#include "llcorehttputil.h"
#include "llcoros.h"
#include "lldir.h"
#include "llfile.h"
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "llinventorymodelbackgroundfetch.h"
#include "llmanip.h"
#include "llnotificationsutil.h"
#include "llparcel.h"
#include "llregionflags.h"
#include "llselectmgr.h" // UPD_POSITION ...
#include "llstartup.h"
#include "llviewerassetupload.h"
#include "llviewercontrol.h"
#include "llviewerinventory.h"
#include "llviewerjointattachment.h"
#include "llviewernetwork.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerparcelmgr.h"
#include "llviewerregion.h"
#include "llviewertexture.h"
#include "llvoavatarself.h"
#include "llvoinventorylistener.h"
#include "llvolume.h"
#include "message.h"

#include <algorithm>
#include <set>
#include <iterator>

namespace
{
    using plus::lslbridge::PoseAction;
    using plus::lslbridge::State;
    namespace proto = plus::lslbridge::proto;

    static_assert(static_cast<int>(PoseAction::Request) == 0 && static_cast<int>(PoseAction::Accept) == 1 &&
                  static_cast<int>(PoseAction::Refuse) == 2 && static_cast<int>(PoseAction::Cancel) == 3 &&
                  static_cast<int>(PoseAction::Withdraw) == 4 && proto::ACTION_COUNT == 5,
                  "proto's action numbers are PoseAction's");

    // Both per account (gSavedPerAccountSettings, read at login). The switch is on by default (research/353): before
    // the account's file is read the bridge waits in WaitLogin, and a "false" saved there stops it before it made
    // anything. The item is the bridge object this viewer made for the account.
    constexpr char SETTING_ENABLE[]     = "PlusPoseBridgeEnable";
    constexpr char SETTING_ITEM[]       = "PlusPoseBridgeItem";
    constexpr char BRIDGE_NAME_PREFIX[] = "#Charis Bridge v"; // + proto::BRIDGE_VERSION: the object and its script
    constexpr char SCRIPT_FILE[]        = "charis_bridge.lsl"; // app_settings/plus/lslbridge/
    constexpr char COMPILE_TARGET[]     = "mono";
    constexpr char SCRIPT_CAPABILITY[]  = "UpdateScriptAgent";
    constexpr std::size_t SCRIPT_FILE_MAX_BYTES = 64 * 1024;
    constexpr U8   ATTACH_POINT         = 31;   // HUD "Center 2", as Firestorm's bridge (FS_BRIDGE_POINT)
    constexpr F32  SMALL_SCALE          = 0.02f; // a worn bridge larger than this has not been made small yet

    // The library object the bridge is made from, tried in this order: its item id (null until it is read from
    // the Second Life library: research/336, not confirmed yet), Firestorm's name (LIB_ROCK_NAME) in the rock
    // folder and then anywhere in the library, another rock of that folder.
    const LLUUID   LIB_ROCK_ITEM_ID;
    constexpr char LIB_ROCK_NAME[]      = "Rock - medium, round";
    constexpr char LIB_OBJECTS_FOLDER[] = "Objects";
    constexpr char LIB_ROCK_FOLDER[]    = "Landscaping";  // Firestorm's FS_BRIDGE_CONTAINER_FOLDER
    constexpr char LIB_SPARE_PREFIX[]   = "Rock";

    // Times: seconds of LLTimer::getTotalSeconds() (real time).
    constexpr F32 TICK_SECONDS          = 1.f;
    constexpr F64 STEP_SECONDS          = 30.0;  // an inventory step, the copy, the contents
    constexpr F64 WEAR_SECONDS          = 60.0;  // the worn object arriving (a large outfit at login)
    constexpr F64 SCRIPT_SECONDS        = 120.0; // the script made, uploaded (the upload queue) and put in
    constexpr F64 START_ANSWER_SECONDS  = 30.0;  // the script's announcement after it was worn, put in or teleported
    // ... after it had been answering (a region crossing, a failed post). The script's ANNOUNCE_GAP (15 s) and the
    // time a busy region takes to grant the new URL (10 s are allowed): an announcement missed at the crossing is
    // caught by the next one (lslbridge_lsl_check.py checks ANSWER_SECONDS >= ANNOUNCE_GAP + 10).
    constexpr F64 ANSWER_SECONDS        = 30.0;
    constexpr int MAX_REFUSED_ANNOUNCEMENTS = 2; // in a row for the same reason: it would not get better (Failed)
    constexpr F64 DETACH_SETTLE_SECONDS = 3.0;   // off the avatar and out of the COF this long: taken off
    constexpr F64 FORGIVE_SECONDS       = 600.0; // since the last repair (or retry), the bridge running again: forgiven
    constexpr int MAX_REPAIRS           = 3;     // per attempt
    constexpr int MAX_RETRIES           = 4;     // new attempts after Failed, then none (counted again as Tries are)
    constexpr F64 RETRY_FIRST_SECONDS   = 60.0;  // the first one; each next waits twice as long
    constexpr int MAX_REWEARS           = 3;     // put on again after a take-off (an outfit change), at most this many
    constexpr F64 REWEAR_WINDOW_SECONDS = 600.0; // ... in this long
    constexpr int MAX_SCRIPT_TRIES      = 2;     // the script item made again when it went before it was put in, since
                                                 // the attempt began or the hello was answered (a take-off while the
                                                 // script is made is not counted here)
    constexpr int MAX_SCRIPT_MAKES      = 5;     // script items made at all, since the attempt began or the hello was
                                                 // answered (those cut short by a take-off too): then Failed (Make),
                                                 // and the retries go on from there
    constexpr U32 POST_TIMEOUT_SECONDS  = 10;
    constexpr U32 MAX_POSTS_IN_FLIGHT   = 16;
    constexpr std::size_t MAX_LANE_POSTS = 8;     // sharing posts on their way (and as many waiting): one lane's own count
    constexpr F64 SHARE_WINDOW_SECONDS  = 1.0;    // the sliding window of the sharing rate (both lanes together) ...
    constexpr int MAX_SHARES_PER_WINDOW = 4;      // ... this many shares and pushes (not ends) launched in any such window
                                                  // (374 section 4); the script allows 6 things a second: these 4 plus the ends
    constexpr F64 SHARE_SPACING_SECONDS = 0.35;   // from the end of one post of a key to the launch of the next: the bridge
                                                  // says a post when it arrives, before its answer comes back, so the
                                                  // bridge's own gap is at least this; the receiving script takes one
                                                  // avatar's share or push every 0.3 s (SHARE_GAP), the ones nearer are lost
    constexpr F64 LANE_FAIL_SECONDS     = 15.0;   // sharing posts failing (busy, timed out) this long, with no answer between:
                                                  // the URL is taken as lost
    constexpr F64 LANE_FAIL_GAP_SECONDS = 12.0;   // a failure this long after the last one (a post times out in 10 s, the next
                                                  // goes 0.35 s after) starts a new run: they were not failing in between
    constexpr int MAX_ENDS_PER_WINDOW   = 2;      // ends launched in any SHARE_WINDOW_SECONDS (their own window): with the 4
                                                  // shares and pushes, at most the 6 things a second the script says
    constexpr U32 MAX_TRANSIENT_FAILURES = 3;    // posts in a row that timed out / met a busy simulator: the URL is lost
    constexpr int MAX_STEPS_PER_TICK    = 8;
    constexpr F64 PROBE_GAP_SECONDS     = 3.0;   // between the calls to the script (research/366): it says nothing before one
    constexpr int MAX_PROBES            = 10;    // calls in one wait for its URL (a step's 30 s)

    // What Starting is doing (the table's 1'.)
    enum class Step
    {
        None, WaitLogin, FindFolder, MakeFolder, FindItem, FindLibrary, CopyItem, Wear, CheckContents, AddScript,
        AwaitAnswer, Detach
    };
    // Why the bridge is being taken off by us (so that it is not "taken off" by someone else).
    enum class OwnDetach { None, Rewear, Recreate, Off };
    // How a repair goes: put it on again, make a new one, or the first then the second (Silent).
    enum class Repair { Rewear, Recreate, Auto };
    // Why it could not be made; each has its notification. NoLibrary and NotHere are not tried again; NoSlot is tried
    // again when a change of the attachments leaves a place (waitingForPlace), not by the clock.
    enum class Failure { NoLibrary, Make, GaveUp, NotHere, NoSlot };
    enum class PostKind { Hello, Pose, Reflect, Share, Push };
    enum class PostResult { Ok, Transient, Gone, Refused, Certificate, Rate }; // Rate: the script's 429 (sharing only)
    constexpr short CURL_PEER_NOT_VERIFIED_OLD = 51; // CURLE_PEER_FAILED_VERIFICATION before curl 7.62
    constexpr short CURL_PEER_NOT_VERIFIED     = 60; // CURLE_PEER_FAILED_VERIFICATION / CURLE_SSL_CACERT
    // A look at the worn bridge's contents: the first, or the one more after an answer to an older request.
    enum class ContentsAsk { First, Again };

    const char* stateName(State state)
    {
        switch (state)
        {
            case State::Off:      return "off";
            case State::Starting: return "starting";
            case State::Running:  return "running";
            case State::Detached: return "detached";
            case State::Failed:   return "failed";
            case State::Silent:   return "silent";
        }
        return "?";
    }

    const char* stepName(Step step)
    {
        switch (step)
        {
            case Step::None:          return "none";
            case Step::WaitLogin:     return "wait for login";
            case Step::FindFolder:    return "find folder";
            case Step::MakeFolder:    return "make folder";
            case Step::FindItem:      return "find bridge";
            case Step::FindLibrary:   return "find library object";
            case Step::CopyItem:      return "copy library object";
            case Step::Wear:          return "wear";
            case Step::CheckContents: return "check contents";
            case Step::AddScript:     return "add script";
            case Step::AwaitAnswer:   return "await answer";
            case Step::Detach:        return "take off";
        }
        return "?";
    }

    // The field an announcement was refused for (the log names the field, never its value).
    const char* checkName(proto::AnnouncementCheck check)
    {
        switch (check)
        {
            case proto::AnnouncementCheck::Ok:      return "ok";
            case proto::AnnouncementCheck::Form:    return "form";
            case proto::AnnouncementCheck::Version: return "version";
            case proto::AnnouncementCheck::Url:     return "address";
            case proto::AnnouncementCheck::Token:   return "token";
        }
        return "?";
    }

    F64 now()
    {
        return LLTimer::getTotalSeconds();
    }

    // A number of tries (repairs of an attempt, retries after Failed): forgiven once the bridge has run again
    // (Running) and FORGIVE_SECONDS passed since the last try, so a long healthy session (region crossings and all)
    // has its count back, and a bridge that keeps breaking does not.
    struct Tries
    {
        int  used     = 0;
        F64  lastAt   = 0.0;
        bool ranSince = false; // Running reached since the last try

        // False when none is left; else the try is counted.
        bool take(int most)
        {
            if (used > 0 && ranSince && now() - lastAt >= FORGIVE_SECONDS)
            {
                used = 0;
            }
            if (used >= most)
            {
                return false;
            }
            ++used;
            lastAt   = now();
            ranSince = false;
            return true;
        }

        void reset()
        {
            used     = 0;
            lastAt   = 0.0;
            ranSince = false;
        }
    };

    // The HTTP lane of the sharing messages (research/374, 374b): the latest body only. A post goes at once unless one
    // of its key is on the way, the spacing since the end of the key's last post has not passed or the window of
    // launches has no room; then it waits in that key's single place (a newer body replaces it, whatever the rate,
    // except that an end is not replaced by a body that is not an end) and flushKey sends it. A key is "share", or a
    // push's target. Main thread only.
    struct LaneWait
    {
        std::string key;
        std::string body;
        bool        end       = false;
        bool        scheduled = false; // a flush (flushKey) is on its way for this key
    };
    struct SharingLane
    {
        std::vector<std::string> inFlight;     // the keys of the posts on the way
        std::vector<LaneWait>    waiting;      // at most one per key
        std::vector<std::pair<std::string, F64>> lastPostAt; // when a key's last post ended (launched, while it is on the
                                                             // way), while within the spacing
        void clear()
        {
            inFlight.clear();
            waiting.clear();
            lastPostAt.clear();
        }
    };

    // Everything the bridge keeps (the table's 2.). Main thread only.
    struct Bridge
    {
        State       state        = State::Off;
        Step        step         = Step::None;
        F64         stepDeadline = 0.0;   // 0: no deadline
        bool        wearAsked    = false; // Wear: the attach request was sent in this step
        bool        waitForCof   = false; // Wear right after FindItem: an item linked in the COF is left to the outfit
        bool        itemFetchAsked = false; // FindItem: the recorded item, not in the inventory read yet, asked for
        bool        answerWaited = true;  // Silent: the script had its time to answer where scripts run (false: not
                                          // yet, it was where scripts do not run: it gets AwaitAnswer before a repair)
        LLUUID      cleanOf;              // the worn object seen holding no other script for the URL held (cleared
                                          // at another URL and a new attempt); the hello and the pose lines wait for it
        int         refusedInRow = 0;     // announcements refused in a row for the same reason
        proto::AnnouncementCheck lastRefusal = proto::AnnouncementCheck::Ok;
        LLUUID      folder;               // #Charis; null: not found (yet)
        LLUUID      item;                 // the bridge object; null: none (yet)
        std::string url;                  // the script's HTTP-in URL; never logged by us (a failed post's line of the
                                          // official HTTP code names it, its capability id shown as "(capability)"
                                          // by llerror's scrubCapabilities, its host as it is: see post())
        LLUUID      token;                // the script's token; never logged (it goes in the body only)
        U32         epoch        = 0;     // one per URL (and per attempt / stop): late results of another are dropped
        bool        helloSent    = false;
        bool        helloOk      = false;
        bool        urlRefused   = false; // the script said the region gives it no URL: no repair here
        U32         postsInFlight     = 0;
        U32         transientFailures = 0;
        Tries       repairs;              // of this attempt
        Tries       retries;              // new attempts after Failed (from 0 when the setting goes on)
        F64         retryAt      = 0.0;   // when Failed starts a new attempt; 0: none
        F64         rewearAt[MAX_REWEARS] = {}; // when it was put on again after a take-off; 0: not used
        int         scriptTries  = 0;     // the script item made again after it went (since the attempt or the hello)
        int         scriptMakes  = 0;     // script items made (since the attempt or the hello; MAX_SCRIPT_MAKES)
        LLUUID      wearAskedOf;          // the item asked to be worn and not seen worn yet; null: none
        F64         wearAskedAt  = 0.0;   // when it was asked
        OwnDetach   ownDetach    = OwnDetach::None;
        F64         notWornSince = 0.0;   // 0: worn (or not watched)
        LLUUID      contentsAskedOf;      // the object whose contents were asked; null: none asked
        ContentsAsk contentsAsk  = ContentsAsk::First; // of the ask in contentsAskedOf
        bool        contentsOlder = false; // its answer is the one of a request on the way before it (a second one is
                                           // not sent): no proof of clean contents, asked once more
        U32         contentsAskNumber = 0; // one per ask: an answer waiting for the idle after a newer ask is dropped
        bool        ticking      = false;
        bool        oldWaiting   = false; // an old version's bridge is waited for to arrive (setStep clears it)
        struct GoneOld
        {
            LLUUID id;
            F64    until;
        };
        std::vector<GoneOld> goneOld;         // old versions' items made anew: taken off if they still come (watchGoneOld)
        int         probeCount   = 0;     // calls sent in this step (setStep and leaveSteps set it to 0)
        F64         nextProbeAt  = 0.0;   // the next call's time; 0: at once
        U32         notified     = 0;     // bit per Failure: told (or logged as not told) once a session
        bool        turnedOnByUser = false; // the person turned the setting on in this session, once logged in: only
                                            // then are the failures told (research/353 §3); a bridge that started by
                                            // itself at the login (on by default) logs them
        bool        certificateTold = false; // the certificate's failure is logged once a session
        bool        waitingForPlace = false; // Failed (NoSlot): no attachment place to spare; tried again when a change of
                                             // the attachments leaves one (roomForBridge()), not by the clock
        bool        certificateFailed = false; // Failed by a certificate in this region: its announcements wait for the
                                               // retry (beginAttempt) or a region change
        boost::signals2::connection attachConnection;
        boost::signals2::connection regionConnection;
        plus::lslbridge::PoseHandler       poseHandler;
        plus::lslbridge::StateHandler      stateHandler;
        plus::lslbridge::SendResultHandler sendResultHandler;
        plus::lslbridge::ShareHandler      shareHandler;
        plus::lslbridge::PoseHandler       reflectHandler;
        plus::lslbridge::SendResultHandler reflectResultHandler;
        plus::lslbridge::PushHandler       pushHandler;
        SharingLane                        shareLane;
        SharingLane                        pushLane;
        std::vector<F64>                   shareLaunches;        // when the last shares and pushes (not ends) were
                                                                 // launched, both lanes together, within the window
        std::vector<F64>                   endLaunches;          // the same for the ends (their own window)
        F64                                laneFailSince = 0.0;  // the first of the run of sharing posts failing with no
                                                                 // answer between; 0: none
        F64                                laneFailLast  = 0.0;  // the last of them
        bool                               laneRefusalTold = false; // a sharing post refused (not the rate): warned once
    };

    Bridge& bridge()
    {
        static Bridge b;
        return b;
    }

    // Whether the bridge is to run: the setting is on, and this is not a safe start (plus/safemode). A safe start leaves
    // it off without changing the setting (nothing is saved): it makes and wears nothing, and takes off a bridge worn
    // from the outfit as with the setting off (research/353 D5; safemode_table.h cannot reach a per-account setting).
    bool settingOn()
    {
        static LLCachedControl<bool> enabled(gSavedPerAccountSettings, SETTING_ENABLE, true); // as settings_plus.xml
        return enabled && !plus::safemode::active();
    }

    // The bridge object this viewer made for the account (written when the library object is copied): the only
    // object worn or taken as the bridge. An object of the bridge's name and place given by someone else (it could
    // hold scripts in other prims) is never worn, removed or listened to. Null: none made yet (or not readable).
    LLUUID recordedItem()
    {
        static LLCachedControl<std::string> recorded(gSavedPerAccountSettings, SETTING_ITEM, "");
        const std::string& text = recorded;
        LLUUID id;
        if (text.size() != static_cast<std::size_t>(UUID_STR_LENGTH - 1) || !LLUUID::validate(text) || !id.set(text, false))
        {
            return LLUUID::null;
        }
        return id;
    }

    void recordItem(const LLUUID& id)
    {
        gSavedPerAccountSettings.setString(SETTING_ITEM, id.isNull() ? std::string() : id.asString());
        // Saved now, not only at logout: after a crash the bridge worn from the COF at the next login would not be
        // the recorded one (a second one made, the first left as if someone gave it).
        if (LLStartUp::getStartupState() == STATE_STARTED)
        {
            plus::saveSettingsFile(gSavedPerAccountSettings, gSavedSettings.getString("PerAccountSettingsFile"), true);
        }
    }

    const std::string& bridgeName()
    {
        static const std::string name = std::string(BRIDGE_NAME_PREFIX) + proto::BRIDGE_VERSION;
        return name;
    }

    // The names we give the object and its script. Only our own script items under it are tidied (made by us); an
    // object under it is ours only by recordedItem().
    // The names of the old versions count too: their objects and scripts are tidied away (1.0 says its URL unasked and
    // 1.1 has no pose sharing, so an old bridge is never worn: stepFindItem()).
    bool isOldBridgeName(const std::string& name)
    {
        const std::string prefix(BRIDGE_NAME_PREFIX);
        return name.size() > prefix.size() && name.compare(0, prefix.size(), prefix) == 0 &&
               proto::isOldVersion(std::string_view(name).substr(prefix.size()));
    }

    bool isOurName(const std::string& name)
    {
        return name == bridgeName() || isOldBridgeName(name);
    }

    bool inTrash(const LLUUID& id)
    {
        const LLUUID trash = gInventory.findCategoryUUIDForType(LLFolderType::FT_TRASH);
        return trash.notNull() && gInventory.isObjectDescendentOf(id, trash);
    }

    LLViewerObject* wornObject()
    {
        const Bridge& b = bridge();
        return (b.item.notNull() && isAgentAvatarValid()) ? gAgentAvatarp->getWornAttachment(b.item) : nullptr;
    }

    // Whether our attachment's scripts run where we stand: the region and the estate let scripts run, and the
    // parcel lets others' scripts run, or it is our land, or it lets its group's run and we wear that group.
    // (LLViewerParcelMgr::allowAgentScripts() looks at the others' scripts only.)
    bool scriptsRunHere()
    {
        const LLViewerRegion* region = gAgent.getRegion();
        const LLParcel*       parcel = LLViewerParcelMgr::getInstance()->getAgentParcel();
        if (!region || !parcel || region->getRegionFlag(REGION_FLAGS_SKIP_SCRIPTS) ||
            region->getRegionFlag(REGION_FLAGS_ESTATE_SKIP_SCRIPTS))
        {
            return false;
        }
        if (parcel->getAllowOtherScripts() || parcel->getOwnerID() == gAgentID)
        {
            return true;
        }
        return parcel->getAllowGroupScripts() && parcel->getGroupID().notNull() && parcel->getGroupID() == gAgent.getGroupID();
    }

    // Moves an item of ours (the bridge object or its script, by its own name) out of our folder to the trash, unless
    // a guard (a protected folder, an RLVa lock) keeps it.
    void removeOwnItem(const LLUUID& id)
    {
        if (plus::invguard::blocksItemRemoval(id))
        {
            LL_INFOS("PlusLSLBridge") << "Bridge item kept: the inventory guards it" << LL_ENDL;
            return;
        }
        gInventory.removeItem(id);
    }

    // ------------------------------------------------------------------ state

    bool tick();
    void connectSignals();

    void wake()
    {
        Bridge& b = bridge();
        if (!b.ticking)
        {
            b.ticking = true;
            doPeriodically(&tick, TICK_SECONDS);
        }
    }

    void setState(State state)
    {
        Bridge& b = bridge();
        if (b.state == state)
        {
            return;
        }
        if (state == State::Running)
        {
            b.repairs.ranSince = true;
            b.retries.ranSince = true;
        }
        LL_INFOS("PlusLSLBridge") << "Bridge " << stateName(b.state) << " -> " << stateName(state) << LL_ENDL;
        b.state = state;
        if (b.stateHandler)
        {
            b.stateHandler(state); // last: the handler may call back in (sendPose, ensure)
        }
    }

    void setStep(Step step, F64 wait_seconds)
    {
        Bridge& b = bridge();
        if (b.step != step)
        {
            LL_INFOS("PlusLSLBridge") << "Bridge step: " << stepName(step) << LL_ENDL;
        }
        b.step         = step;
        b.stepDeadline = (wait_seconds > 0.0) ? now() + wait_seconds : 0.0;
        b.probeCount   = 0; // every step is a new wait: its calls begin again
        b.oldWaiting   = false;
        b.nextProbeAt  = 0.0;
        b.wearAsked    = false;
        b.waitForCof   = false;
        b.itemFetchAsked = false;
        if (step != Step::None)
        {
            setState(State::Starting);
        }
        wake();
    }

    bool stepExpired()
    {
        const Bridge& b = bridge();
        return b.stepDeadline > 0.0 && now() >= b.stepDeadline;
    }

    // The URL and token are no longer good: nothing can be sent until the script announces a new one.
    void dropUrl()
    {
        Bridge& b = bridge();
        b.url.clear();
        b.token.setNull();
        b.helloSent         = false;
        b.helloOk           = false;
        b.transientFailures = 0;
        b.shareLane.clear(); // what was on the way or waiting was for the URL dropped
        b.pushLane.clear();
        b.shareLaunches.clear();
        b.endLaunches.clear();
        b.laneFailSince = 0.0;
        b.laneFailLast  = 0.0;
        ++b.epoch;
    }

    struct ContentsListener : public LLVOInventoryListener
    {
        void inventoryChanged(LLViewerObject* object, LLInventoryObject::object_list_t* inventory, S32 serial_num,
                              void* user_data) override;
    };

    ContentsListener& contentsListener()
    {
        static ContentsListener listener;
        return listener;
    }

    void cancelContents()
    {
        Bridge& b = bridge();
        if (b.contentsAskedOf.notNull())
        {
            if (LLViewerObject* object = gObjectList.findObject(b.contentsAskedOf))
            {
                object->removeInventoryListener(&contentsListener());
            }
            b.contentsAskedOf.setNull();
        }
    }

    // Out of Starting (to Failed, Silent, Detached, Off): no step, no URL, no contents asked for.
    void leaveSteps()
    {
        Bridge& b = bridge();
        dropUrl();
        cancelContents();
        b.step         = Step::None;
        b.stepDeadline = 0.0;
        b.probeCount   = 0;
        b.nextProbeAt  = 0.0;
    }

    // Once a session for each kind. Shown only when the person turned the bridge on in this session (turnedOnByUser):
    // on by default, a login that is slow, a grid that is not Second Life or an RLVa lock would show it to everyone at
    // every login. Else the log only (when someone is chosen, the poser says that the bridge is not connected, or that it
    // could not be made and how to see why: failedForGood()).
    void notifyFailure(Failure failure)
    {
        Bridge&   b   = bridge();
        const U32 bit = 1u << static_cast<U32>(failure);
        if (b.notified & bit)
        {
            return;
        }
        b.notified |= bit;
        if (!b.turnedOnByUser)
        {
            LL_INFOS("PlusLSLBridge") << "Bridge failure not shown (it started by itself at the login)" << LL_ENDL;
            return;
        }
        switch (failure)
        {
            case Failure::NoLibrary: LLNotificationsUtil::add("PlusLSLBridgeNoLibrary"); break;
            case Failure::Make:      LLNotificationsUtil::add("PlusLSLBridgeMakeFailed"); break;
            case Failure::GaveUp:    LLNotificationsUtil::add("PlusLSLBridgeGaveUp"); break;
            case Failure::NotHere:   LLNotificationsUtil::add("PlusLSLBridgeNotHere"); break;
            case Failure::NoSlot:    LLNotificationsUtil::add("PlusLSLBridgeNoSlot"); break;
        }
    }

    // After a Failed that may go better later (Make, GaveUp): a new attempt after RETRY_FIRST_SECONDS, twice as long
    // each time, MAX_RETRIES of them (forgiven as Tries are: the count, and the wait, start again once the bridge ran
    // again and FORGIVE_SECONDS passed since the last).
    void scheduleRetry()
    {
        Bridge& b = bridge();
        if (!b.retries.take(MAX_RETRIES))
        {
            LL_INFOS("PlusLSLBridge") << "Bridge not tried again (" << MAX_RETRIES << " tries)" << LL_ENDL;
            return;
        }
        const F64 wait = RETRY_FIRST_SECONDS * static_cast<F64>(1 << (b.retries.used - 1));
        b.retryAt      = now() + wait;
        LL_INFOS("PlusLSLBridge") << "Bridge tried again in " << llformat("%.0f", wait) << " s" << LL_ENDL;
        wake();
    }

    void fail(Failure failure, const char* why)
    {
        LL_WARNS("PlusLSLBridge") << "Bridge failed: " << why << LL_ENDL;
        bridge().waitingForPlace = failure == Failure::NoSlot; // before the state handler reads failedForGood()
        leaveSteps();
        bridge().ownDetach = OwnDetach::None;
        setState(State::Failed);
        notifyFailure(failure);
        if (failure == Failure::Make || failure == Failure::GaveUp)
        {
            scheduleRetry();
        }
    }

    // Worn but not answering. The tick repairs it where scripts run (not while the region gives it no URL); where
    // they do not, it waits, and gets AwaitAnswer again before a repair once they do (answerWaited).
    void enterSilent()
    {
        leaveSteps();
        bridge().answerWaited = true;
        setState(State::Silent);
        wake();
    }

    // Removes the bridge item (ours, made by us: a new one is made in its place).
    void forgetItem()
    {
        Bridge& b = bridge();
        if (b.item.notNull() && gInventory.getItem(b.item) && !get_is_item_worn(b.item))
        {
            removeOwnItem(b.item);
        }
        if (b.item.notNull() && b.item == recordedItem())
        {
            recordItem(LLUUID::null); // a new one is made and recorded in its place
        }
        b.item.setNull();
    }

    void takeOffOurselves(OwnDetach why);
    void addScript();

    void repair(Repair kind, const char* why)
    {
        Bridge& b = bridge();
        if (!b.repairs.take(MAX_REPAIRS))
        {
            fail(Failure::GaveUp, why);
            return;
        }
        LL_INFOS("PlusLSLBridge") << "Bridge repair " << b.repairs.used << " of " << MAX_REPAIRS << ": " << why << LL_ENDL;
        dropUrl();
        cancelContents();
        // Auto: put on again, then made anew. A bridge that started by itself (on by default) is only put on again
        // (research/353, review L1): where it never answers, no rock and script go to the trash at every repair. Made
        // anew all the same: another script version (a new viewer's script comes only that way) and linked parts.
        const bool recreate = (kind == Repair::Recreate) ||
                              (kind == Repair::Auto && b.repairs.used > 1 && b.turnedOnByUser);
        if (wornObject())
        {
            takeOffOurselves(recreate ? OwnDetach::Recreate : OwnDetach::Rewear);
            return;
        }
        if (recreate)
        {
            forgetItem();
            setStep(Step::FindLibrary, STEP_SECONDS);
            return;
        }
        setStep(Step::Wear, WEAR_SECONDS);
    }

    void takeOffOurselves(OwnDetach why)
    {
        Bridge& b = bridge();
        b.ownDetach = why;
        if (!LLVOAvatarSelf::detachAttachmentIntoInventory(b.item))
        {
            // Kept on (an RLVa @detach lock): this repair is spent, the next tick tries the next one.
            LL_INFOS("PlusLSLBridge") << "Bridge could not be taken off" << LL_ENDL;
            b.ownDetach = OwnDetach::None;
            enterSilent();
            return;
        }
        setStep(Step::Detach, STEP_SECONDS);
    }

    // Our bridge is one prim (the library object): another prim linked to it could hold scripts the viewer never looks
    // at, running as our attachment. Such a bridge is not used: taken off and made anew (a repair, so it ends).
    bool refuseLinkedParts(LLViewerObject* object)
    {
        if (object->numChildren() <= 0)
        {
            return false;
        }
        LL_WARNS("PlusLSLBridge") << "Bridge object has " << object->numChildren() << " linked part(s): not used, made anew"
                                  << LL_ENDL;
        repair(Repair::Recreate, "linked parts");
        if (bridge().state == State::Failed && wornObject())
        {
            LLVOAvatarSelf::detachAttachmentIntoInventory(bridge().item); // no repair left: still not kept on
        }
        return true;
    }

    // Put on again after a take-off: at most MAX_REWEARS in REWEAR_WINDOW_SECONDS (outfit changes take a HUD off;
    // something that takes it off at once every time is not fought for long). Not counted as a repair.
    bool takeRewear()
    {
        Bridge&   b    = bridge();
        const F64 t    = now();
        F64*      slot = nullptr;
        for (F64& at : b.rewearAt)
        {
            if (at <= 0.0 || t - at >= REWEAR_WINDOW_SECONDS)
            {
                slot = &at;
                break;
            }
        }
        if (!slot)
        {
            return false;
        }
        *slot = t;
        return true;
    }

    // The table's "taken off": by the user, an outfit change, a script, RLVa... not by us. Off while its hello was
    // waited for (its contents being looked at, or a URL held and no hello answered): the bridge's own script takes
    // itself off when no hello comes, which is what happens when its contents never come to the viewer (the hello
    // waits for them). That is a repair, so that it ends (GaveUp, told) instead of being put on again and again.
    void onTakenOff()
    {
        Bridge&    b            = bridge();
        const bool hello_waited = b.step == Step::CheckContents ||
                                  (b.step == Step::AwaitAnswer && !b.url.empty() && !b.helloOk);
        b.notWornSince = 0.0;
        if (hello_waited)
        {
            repair(Repair::Auto, "off while its hello was waited for");
            return;
        }
        leaveSteps();
        setState(State::Detached);
        if (!takeRewear())
        {
            LL_INFOS("PlusLSLBridge") << "Bridge left off: taken off " << MAX_REWEARS << " times lately (put on again "
                                      << "when the oldest of them is " << llformat("%.0f", REWEAR_WINDOW_SECONDS)
                                      << " s old)" << LL_ENDL;
            wake(); // the tick puts it on again when a turn is free
            return;
        }
        LL_INFOS("PlusLSLBridge") << "Bridge taken off: put on again" << LL_ENDL;
        setStep(Step::Wear, WEAR_SECONDS);
    }

    // At every hello and before every script item is made: our folder's script items under our own name that we made
    // go to the trash (the script's copy left in the inventory, a script item of a making cut short). Objects stay
    // (the bridge object made anew goes through forgetItem(); one of its name we did not record is not ours), links
    // stay, a script someone else made stays, a guarded item stays.
    void removeOtherBridgeItems()
    {
        Bridge& b = bridge();
        if (b.folder.isNull() || !gInventory.isCategoryComplete(b.folder))
        {
            return;
        }
        LLInventoryModel::cat_array_t*  cats  = nullptr;
        LLInventoryModel::item_array_t* items = nullptr;
        gInventory.getDirectDescendentsOf(b.folder, cats, items);
        if (!items)
        {
            return;
        }
        uuid_vec_t gone;
        for (const LLPointer<LLViewerInventoryItem>& it : *items)
        {
            if (!it->getIsLinkType() && it->getType() == LLAssetType::AT_LSL_TEXT && it->getUUID() != b.item &&
                isOurName(it->getName()) && it->getCreatorUUID() == gAgentID)
            {
                gone.push_back(it->getUUID());
            }
        }
        for (const LLUUID& id : gone)
        {
            removeOwnItem(id);
        }
        if (!gone.empty())
        {
            LL_INFOS("PlusLSLBridge") << "Bridge folder: " << gone.size() << " old item(s) to the trash" << LL_ENDL;
        }
    }

    // ------------------------------------------------------------------ HTTP

    PostResult classify(const LLSD& result)
    {
        const LLSD               http   = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
        const LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(http);
        if (status)
        {
            return (result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_CONTENT].asString() == proto::REPLY_OK)
                       ? PostResult::Ok
                       : PostResult::Refused;
        }
        if (!status.isHttpStatus())
        {
            // curl's CURLE_PEER_FAILED_VERIFICATION (51 in older curl: the certificate does not name the host) and
            // CURLE_SSL_CACERT (60, its alias since curl 7.62: not trusted or not naming the host). Not transient: the
            // same at every try.
            if (status.getType() == LLCore::HttpStatus::EXT_CURL_EASY &&
                (status.getStatus() == CURL_PEER_NOT_VERIFIED_OLD || status.getStatus() == CURL_PEER_NOT_VERIFIED))
            {
                return PostResult::Certificate;
            }
            return PostResult::Transient; // timed out, the connection failed
        }
        const S32 code = status.getType();
        if (code >= 500)
        {
            return PostResult::Transient; // the simulator is busy
        }
        if (code == 429)
        {
            return PostResult::Rate; // the script's sharing rate (it says 429 for nothing else)
        }
        if (code == 403 || code == 404 || code == 410)
        {
            return PostResult::Gone;      // the URL is no more, or its script took another token (a new URL)
        }
        return PostResult::Refused;
    }

    // A pose or reflect message posted, given back to the send result handler with its result; empty for the hello;
    // key is the lane key of a share or a push post (else empty).
    struct PoseSent
    {
        PoseAction  action = PoseAction::Request;
        LLUUID      nonce;
        LLUUID      target;
        std::string key;
    };

    void onPostResult(U32 epoch, PostKind kind, const PoseSent& sent, PostResult result);

    // Posts body (one LLSD string) to the script's URL. Not through HttpCoroutineAdapter::callbackHttpPost(), which
    // logs the URL at every post. A post that fails is still logged by the official HttpCoroHandler::onCompleted()
    // ("Possible failure ... cannot POST url", INFO): llerror's scrubCapabilities() writes the URL's capability id as
    // "(capability)", the simulator's host name shows. The token is in the body only (never logged), so what the log
    // shows does not let anyone post to the bridge. Every post ends in onPostResult() (the timeout bounds it).
    void post(const std::string& body, PostKind kind, const PoseSent& sent = PoseSent())
    {
        Bridge&           b     = bridge();
        const std::string url   = b.url;
        const U32         epoch = b.epoch;
        if (kind != PostKind::Share && kind != PostKind::Push)
        {
            ++b.postsInFlight; // the sharing lanes have their own count (SharingLane::inFlight)
        }
        LLCoros::instance().launch("PlusLSLBridgePost", [url, body, epoch, kind, sent]()
        {
            auto adapter = std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("PlusLSLBridgePost",
                                                                                   LLCore::HttpRequest::DEFAULT_POLICY_ID);
            auto request = std::make_shared<LLCore::HttpRequest>();
            auto options = std::make_shared<LLCore::HttpOptions>();
            options->setTimeout(POST_TIMEOUT_SECONDS);
            options->setTransferTimeout(POST_TIMEOUT_SECONDS);
            options->setRetries(0);
            options->setFollowRedirects(false); // only the simulator's own address
            // The certificate and that it names the URL's host (as updatecheck, Velopack and the translation do): a
            // man in the middle would read the token. Not seen yet on an HTTP-in URL: if its certificate does not name
            // its host, curl says so (PostResult::Certificate) and the bridge is Failed (Make) at once, not made anew
            // (the first check in Second Life looks for "Bridge starting -> running"; "Bridge post refused: the
            // certificate does not name the host" means it does not match).
            options->setSSLVerifyPeer(true);
            options->setSSLVerifyHost(true);
            const LLSD result = adapter->postAndSuspend(request, url, LLSD(body), options);
            onPostResult(epoch, kind, sent, classify(result));
        });
    }

    SharingLane& laneOf(PostKind kind)
    {
        return (kind == PostKind::Share) ? bridge().shareLane : bridge().pushLane;
    }

    // A sharing message can go only while Running, after the hello, with a URL.
    bool laneReady()
    {
        const Bridge& b = bridge();
        return b.state == State::Running && b.helloOk && !b.url.empty();
    }

    // The time of key's last post, as its end (or, while it is on the way, its launch): the next one goes
    // SHARE_SPACING_SECONDS after it. Only the keys still within the spacing are kept.
    void setKeyTime(SharingLane& lane, const std::string& key, F64 t)
    {
        lane.lastPostAt.erase(std::remove_if(lane.lastPostAt.begin(), lane.lastPostAt.end(),
                                             [&key, t](const std::pair<std::string, F64>& e)
                                             { return e.first == key || t - e.second >= SHARE_SPACING_SECONDS; }),
                              lane.lastPostAt.end());
        lane.lastPostAt.emplace_back(key, t);
    }

    // The time left before key's next sharing post may go (SHARE_SPACING_SECONDS after the end of its last one); <= 0: now.
    F64 spacingLeft(const SharingLane& lane, const std::string& key)
    {
        for (const auto& entry : lane.lastPostAt)
        {
            if (entry.first == key)
            {
                return entry.second + SHARE_SPACING_SECONDS - now();
            }
        }
        return 0.0;
    }

    // The time left before another share or push may be launched: at most MAX_SHARES_PER_WINDOW in any
    // SHARE_WINDOW_SECONDS, both lanes together (a sliding window of the launches); the ends have their own window of
    // MAX_ENDS_PER_WINDOW (so the 4 and the 2 together stay within the script's 6 things a second); <= 0: now.
    F64 shareRateLeft(bool end)
    {
        Bridge&           b      = bridge();
        const F64         t      = now();
        std::vector<F64>& window = end ? b.endLaunches : b.shareLaunches;
        const std::size_t most   = static_cast<std::size_t>(end ? MAX_ENDS_PER_WINDOW : MAX_SHARES_PER_WINDOW);
        window.erase(std::remove_if(window.begin(), window.end(), [t](F64 at) { return t - at >= SHARE_WINDOW_SECONDS || at > t; }),
                     window.end());
        if (window.size() < most)
        {
            return 0.0;
        }
        return window[window.size() - most] + SHARE_WINDOW_SECONDS - t;
    }

    // When body for key may go: after the spacing and when the window of its kind has room.
    F64 launchDelay(const SharingLane& lane, const std::string& key, bool end)
    {
        return std::max(spacingLeft(lane, key), shareRateLeft(end));
    }

    void launchLane(PostKind kind, const std::string& key, const std::string& body, bool end)
    {
        SharingLane& lane = laneOf(kind);
        const F64    t    = now();
        setKeyTime(lane, key, t);
        (end ? bridge().endLaunches : bridge().shareLaunches).push_back(t);
        lane.inFlight.push_back(key);
        PoseSent sent;
        sent.key = key;
        post(body, kind, sent);
    }

    void flushKey(PostKind kind, const std::string& key, U32 epoch);

    // What waits for key goes after delay seconds (once: a flush already on its way is enough).
    void scheduleFlush(PostKind kind, const std::string& key, F64 delay)
    {
        bool found = false;
        for (LaneWait& wait : laneOf(kind).waiting)
        {
            if (wait.key == key)
            {
                if (wait.scheduled)
                {
                    return;
                }
                wait.scheduled = true;
                found          = true;
            }
        }
        if (!found)
        {
            return;
        }
        const U32 epoch = bridge().epoch;
        doAfterInterval([kind, key, epoch]() { flushKey(kind, key, epoch); }, static_cast<F32>(std::max(delay, 0.0)));
    }

    // The time for what waits for key has come: it goes when no post of the key is on the way (the end of that post
    // does it then), the spacing has passed and the window has room (else once more later), for the URL it was held for.
    void flushKey(PostKind kind, const std::string& key, U32 epoch)
    {
        Bridge& b = bridge();
        if (!LLApp::isRunning() || epoch != b.epoch)
        {
            return;
        }
        SharingLane& lane = laneOf(kind);
        const auto   wait = std::find_if(lane.waiting.begin(), lane.waiting.end(), [&key](const LaneWait& w) { return w.key == key; });
        if (wait == lane.waiting.end())
        {
            return;
        }
        wait->scheduled = false;
        if (std::find(lane.inFlight.begin(), lane.inFlight.end(), key) != lane.inFlight.end())
        {
            return;
        }
        if (lane.inFlight.size() >= MAX_LANE_POSTS)
        {
            scheduleFlush(kind, key, SHARE_SPACING_SECONDS); // 8 on their way: it goes when one has ended
            return;
        }
        const F64 left = launchDelay(lane, key, wait->end);
        if (left > 0.0)
        {
            scheduleFlush(kind, key, left);
            return;
        }
        const std::string next = wait->body;
        const bool        end  = wait->end;
        lane.waiting.erase(wait);
        if (laneReady())
        {
            launchLane(kind, key, next, end);
        }
    }

    // body (empty: refused) for key: posted at once, or held in the single place of key's waiting body (a newer body
    // replaces it, whatever the rate: only what is launched is counted) while a post of that key is on the way, its
    // spacing has not passed or the window has no room (then flushKey sends it). False: not taken (not ready, 8 posts on
    // their way, 8 waiting, or an end that a body of another kind would replace).
    bool queueLane(PostKind kind, const std::string& key, const std::string& body, bool end)
    {
        if (body.empty() || !laneReady())
        {
            return false;
        }
        SharingLane& lane  = laneOf(kind);
        const bool   busy  = std::find(lane.inFlight.begin(), lane.inFlight.end(), key) != lane.inFlight.end();
        const F64    delay = launchDelay(lane, key, end);
        const bool   waits = busy || delay > 0.0;
        for (LaneWait& wait : lane.waiting)
        {
            if (wait.key == key)
            {
                if (wait.end && !end)
                {
                    return false;
                }
                wait.body = body;
                wait.end  = end;
                return true;
            }
        }
        if (waits)
        {
            if (lane.waiting.size() >= MAX_LANE_POSTS)
            {
                return false;
            }
            lane.waiting.push_back(LaneWait{key, body, end, false});
            if (!busy)
            {
                scheduleFlush(kind, key, delay);
            }
            return true;
        }
        if (lane.inFlight.size() >= MAX_LANE_POSTS)
        {
            return false;
        }
        launchLane(kind, key, body, end);
        return true;
    }

    // A sharing post of the URL still held ended: its key is free, the spacing is counted from now (the bridge said it
    // when it arrived, before this answer), and what waited for it goes when the spacing and the window allow.
    void onLanePostDone(PostKind kind, const std::string& key, U32 epoch)
    {
        Bridge& b = bridge();
        if (epoch != b.epoch)
        {
            return; // the lane was cleared with the URL it was for (dropUrl)
        }
        SharingLane& lane = laneOf(kind);
        const auto   in   = std::find(lane.inFlight.begin(), lane.inFlight.end(), key);
        if (in != lane.inFlight.end())
        {
            lane.inFlight.erase(in);
        }
        setKeyTime(lane, key, now());
        scheduleFlush(kind, key, spacingLeft(lane, key));
    }

    void sendHello()
    {
        Bridge& b  = bridge();
        b.helloSent = true;
        post(proto::helloBody(b.token.asString()), PostKind::Hello);
    }

    // The call (research/366): the script says nothing, its URL included, until the owner's viewer whispers this on a
    // positive channel (heard by scripts only: no other avatar's chat shows it). The script listens to the owner avatar
    // alone, but the owner's avatar also speaks when they press the button of someone's dialog or click a
    // secondlife:///app/chat/ link, with any text: so the call holds the key of the worn bridge object, which is on no
    // list while it is worn on a HUD point (a bridge on the body has a public key: it is not called). The text is no
    // secret; the key is only said here and in the relay of pose requests, while the Charis Viewer hides the bridge's
    // lines. Sent straight as ChatFromViewer, not through send_chat_from_viewer: the RLV filter there
    // (@sendchannel...) is for what the person types, and would leave the bridge deaf.
    void sendProbe(const LLUUID& bridge_object)
    {
        Bridge& b = bridge();
        ++b.probeCount;
        b.nextProbeAt = now() + PROBE_GAP_SECONDS;
        gMessageSystem->newMessageFast(_PREHASH_ChatFromViewer);
        gMessageSystem->nextBlockFast(_PREHASH_AgentData);
        gMessageSystem->addUUIDFast(_PREHASH_AgentID, gAgentID);
        gMessageSystem->addUUIDFast(_PREHASH_SessionID, gAgentSessionID);
        gMessageSystem->nextBlockFast(_PREHASH_ChatData);
        gMessageSystem->addStringFast(_PREHASH_Message, proto::probeText(bridge_object.asString()));
        gMessageSystem->addU8Fast(_PREHASH_Type, CHAT_TYPE_WHISPER);
        gMessageSystem->addS32("Channel", proto::PROBE_CHANNEL);
        gAgent.sendReliableMessage();
        LL_DEBUGS("PlusLSLBridge") << "Bridge called (" << b.probeCount << " of " << MAX_PROBES << ")" << LL_ENDL;
    }

    // The bridge is worn on the HUD (the item of this step, or the recorded one before the steps found it: at the login
    // the outfit wears it long before the steps reach it, and its script gives up after PAIR_WAIT seconds without a
    // call). Worn on the body it is not called: its key is on every list (llGetAttachedList) and the script takes no call
    // and says nothing there. One that is on the body when it is worn (the login's outfit) is put on the HUD again by
    // stepWear; one moved there by hand while it runs stays Running until its next URL, which its script does not say
    // (the wait for it ends Silent, a repair puts it on the HUD).
    LLViewerObject* bridgeWornForCall()
    {
        const Bridge& b    = bridge();
        const LLUUID  item = b.item.notNull() ? b.item : recordedItem();
        LLViewerObject* object = (item.notNull() && isAgentAvatarValid()) ? gAgentAvatarp->getWornAttachment(item) : nullptr;
        return (object && object->isHUDAttachment()) ? object : nullptr;
    }

    // While a step of the bridge waits (not the take-off) and its URL is not held: the call, at once and then every
    // PROBE_GAP_SECONDS, MAX_PROBES times in a step, once logged in. Once its URL is held nothing more is sent.
    void probeIfDue()
    {
        Bridge& b = bridge();
        if (b.step == Step::None || b.step == Step::Detach || !b.url.empty() || b.probeCount >= MAX_PROBES ||
            now() < b.nextProbeAt || LLStartUp::getStartupState() < STATE_STARTED)
        {
            return;
        }
        if (const LLViewerObject* object = bridgeWornForCall()) // its key is in the call: a new one at every wear
        {
            sendProbe(object->getID());
        }
    }

    // The URL is lost: wait for the script to announce one (it does after a region change and while no hello came; else
    // the wait ends in Silent and a repair).
    void urlLost()
    {
        LL_INFOS("PlusLSLBridge") << "Bridge URL lost" << LL_ENDL;
        dropUrl();
        setStep(Step::AwaitAnswer, ANSWER_SECONDS);
    }

    // A sharing post failed (busy simulator, timeout): told when a run starts, the URL lost when the run has lasted
    // LANE_FAIL_SECONDS, every failure within LANE_FAIL_GAP_SECONDS of the last (a failure after a longer quiet starts a
    // new run), an answer in between (laneAnswered()) ending it.
    void laneFailed()
    {
        Bridge&   b = bridge();
        const F64 t = now();
        if (b.laneFailSince <= 0.0 || t - b.laneFailLast > LANE_FAIL_GAP_SECONDS)
        {
            b.laneFailSince = t;
            b.laneFailLast  = t;
            LL_INFOS("PlusLSLBridge") << "Sharing posts are failing (busy or timed out)" << LL_ENDL;
            return;
        }
        b.laneFailLast = t;
        if (t - b.laneFailSince >= LANE_FAIL_SECONDS)
        {
            LL_INFOS("PlusLSLBridge") << "Sharing posts failed for " << LANE_FAIL_SECONDS << " s" << LL_ENDL;
            urlLost();
        }
    }

    // The bridge answered (ok, or the rate refusal, or any other refusal: the URL is alive): a run of failures ends.
    void laneAnswered()
    {
        Bridge& b = bridge();
        if (b.laneFailSince > 0.0)
        {
            LL_INFOS("PlusLSLBridge") << "Sharing posts are answered again" << LL_ENDL;
            b.laneFailSince = 0.0;
        }
    }

    const char* postName(PostKind kind)
    {
        switch (kind)
        {
            case PostKind::Hello:   return "hello";
            case PostKind::Pose:    return "pose message";
            case PostKind::Reflect: return "reflect message";
            case PostKind::Share:   return "share";
            case PostKind::Push:    return "push";
        }
        return "?";
    }

    // What a post's result does to the bridge (the URL it was for is still the one held).
    void applyPostResult(PostKind kind, PostResult result)
    {
        Bridge&    b    = bridge();
        const bool lane = (kind == PostKind::Share || kind == PostKind::Push);
        if (result == PostResult::Rate)
        {
            if (lane)
            {
                laneAnswered();
                LL_DEBUGS("PlusLSLBridge") << "A " << postName(kind) << " was refused for its rate (429)" << LL_ENDL;
                return;
            }
            result = PostResult::Refused; // only the sharing can be told that
        }
        if (lane && result == PostResult::Transient)
        {
            // A busy simulator or a timeout on a post of the sharing (2 or more a second) is not counted toward
            // MAX_TRANSIENT_FAILURES (only the hello and the pose messages, which come seldom, are): the URL is taken as
            // lost only when they have all failed for LANE_FAIL_SECONDS with no answer between. The next share is the
            // latest pose anyway.
            laneFailed();
            return;
        }
        if (result == PostResult::Ok)
        {
            b.transientFailures = 0;
            laneAnswered();
            if (kind == PostKind::Hello && !b.helloOk)
            {
                b.laneRefusalTold = false; // a new URL: a refusal of a sharing post is warned once again
                b.helloOk      = true;
                b.step         = Step::None;
                b.stepDeadline = 0.0;
                b.notWornSince = 0.0;
                b.retryAt      = 0.0;
                b.scriptMakes  = 0; // made and running: a long session's remakes (repairs) count from 0 again
                b.scriptTries  = 0; // ... and its script items taken away from outside (made again in addScript(),
                                    // so scriptMakes still bounds them)
                b.wearAskedOf.setNull(); // it is worn and answering
                connectSignals(); // also when the announcement came before the steps were through the login
                setState(State::Running);
                removeOtherBridgeItems();
            }
            return;
        }
        if (result == PostResult::Certificate)
        {
            // The same at every try: no repair (a new bridge gets a URL of the same kind), Failed and its few retries.
            if (!b.certificateTold)
            {
                b.certificateTold = true;
                LL_WARNS("PlusLSLBridge") << "Bridge post refused: the certificate does not name the host" << LL_ENDL;
            }
            fail(Failure::Make, "certificate");
            b.certificateFailed = true; // the script says the same URL again: not taken before the retry's time
            return;
        }
        if (result == PostResult::Refused)
        {
            if (lane)
            {
                laneAnswered();
            }
            if (lane && b.laneRefusalTold)
            {
                // A body the script does not take (another form or version): warned once for a URL, then debug
                LL_DEBUGS("PlusLSLBridge") << "Bridge refused a " << postName(kind) << LL_ENDL;
            }
            else
            {
                b.laneRefusalTold = b.laneRefusalTold || lane;
                LL_WARNS("PlusLSLBridge") << "Bridge refused a " << postName(kind) << LL_ENDL;
            }
            if (kind == PostKind::Hello)
            {
                b.helloSent = false; // the next announcement is answered again
            }
            return;
        }
        if (result == PostResult::Transient && ++b.transientFailures < MAX_TRANSIENT_FAILURES)
        {
            LL_INFOS("PlusLSLBridge") << "Bridge post failed (" << b.transientFailures << " in a row)" << LL_ENDL;
            if (kind == PostKind::Hello)
            {
                b.helloSent = false;
            }
            return;
        }
        urlLost();
    }

    // Every post ends here once (on the main thread). What it does to the bridge only for the URL still held. A pose
    // message's HTTP result, whatever became of the URL or the bridge since (a script that answered "ok" did relay it,
    // also when the URL was dropped or the setting went off meanwhile), goes to the send result handler in the next
    // idle: never from inside sendPose() (the coroutine may end at once, a post that cannot be queued fails at
    // once). Not while the viewer quits.
    void onPostResult(U32 epoch, PostKind kind, const PoseSent& sent, PostResult result)
    {
        Bridge& b = bridge();
        const bool laneKind = (kind == PostKind::Share || kind == PostKind::Push);
        if (!laneKind && b.postsInFlight > 0)
        {
            --b.postsInFlight;
        }
        if (!LLApp::isRunning())
        {
            return;
        }
        if (epoch == b.epoch && b.state != State::Off) // else: for a URL dropped since
        {
            applyPostResult(kind, result);
        }
        if (laneKind)
        {
            onLanePostDone(kind, sent.key, epoch); // after applyPostResult: a URL it dropped cleared the lane
        }
        if (kind == PostKind::Pose || kind == PostKind::Reflect)
        {
            const bool ok = result == PostResult::Ok;
            const bool reflect = (kind == PostKind::Reflect);
            doOnIdleOneTime([sent, ok, reflect]()
            {
                // A copy: the handler may set another.
                const plus::lslbridge::SendResultHandler handler =
                    reflect ? bridge().reflectResultHandler : bridge().sendResultHandler;
                if (LLApp::isRunning() && handler)
                {
                    handler(sent.action, sent.nonce, sent.target, ok);
                }
            });
        }
    }

    // ------------------------------------------------------------------ making the bridge

    // Firestorm's FSLSLBridge::setupBridgePrim(): small, clear and out of sight on the HUD.
    void makeSmallAndClear(LLViewerObject* object)
    {
        if (!object->getRegion())
        {
            return;
        }
        LLProfileParams profile(LL_PCODE_PROFILE_CIRCLE, 0.230f, 0.250f, 0.95f);
        LLPathParams    path(LL_PCODE_PATH_CIRCLE, 0.2f, 0.22f,
                             0.f, 350.f,    // scale
                             0.f, 0.f,      // shear
                             0.f, 0.f,      // twist
                             0.f,           // offset
                             0.f, 0.f,      // taper
                             0.05f, 0.05f); // revolutions, skew
        path.setRevolutions(1.f);
        object->setVolume(LLVolumeParams(profile, path), 0);
        object->sendShapeUpdate();

        object->setScale(LLVector3(0.01f, 0.01f, 0.01f));
        object->setPosition(LLVector3(2.f, 2.f, 2.f));
        LLManip::rebuild(object);

        U8 data[24];
        htolememcpy(&data[0], &(object->getPosition().mV), MVT_LLVector3, 12);
        htolememcpy(&data[12], &(object->getScale().mV), MVT_LLVector3, 12);
        gMessageSystem->newMessage("MultipleObjectUpdate");
        gMessageSystem->nextBlockFast(_PREHASH_AgentData);
        gMessageSystem->addUUIDFast(_PREHASH_AgentID, gAgentID);
        gMessageSystem->addUUIDFast(_PREHASH_SessionID, gAgentSessionID);
        gMessageSystem->nextBlockFast(_PREHASH_ObjectData);
        gMessageSystem->addU32Fast(_PREHASH_ObjectLocalID, object->getLocalID());
        gMessageSystem->addU8Fast(_PREHASH_Type, UPD_POSITION | UPD_SCALE | UPD_LINKED_SETS);
        gMessageSystem->addBinaryDataFast(_PREHASH_Data, data, 24);
        gMessageSystem->sendReliable(object->getRegion()->getHost());

        for (U8 te = 0; te < object->getNumTEs(); ++te)
        {
            object->setTEImage(te, LLViewerTextureManager::getFetchedTexture(IMG_INVISIBLE));
        }
        object->sendTEUpdate();
        object->setChanged(LLXform::MOVED | LLXform::SILHOUETTE | LLXform::TEXTURE);
        object->markForUpdate();
    }

    void askContents(LLViewerObject* object, ContentsAsk ask = ContentsAsk::First)
    {
        Bridge& b = bridge();
        cancelContents();
        if (object->getScale().mV[VX] > SMALL_SCALE)
        {
            makeSmallAndClear(object);
        }
        b.contentsAskedOf = object->getID();
        b.contentsAsk     = ask;
        ++b.contentsAskNumber;
        object->registerInventoryListener(&contentsListener(), nullptr);
        // The simulator's contents now, not the copy kept from an earlier look (a script removed since, one put in):
        // the copy is let go only with a listener on the object (so it is registered first).
        object->dirtyInventory();
        // A request already on the way is not sent again (fetchInventoryFromServer() drops it): its answer may be from
        // before a removal, so onContents() takes it as no proof of clean contents and asks once more.
        b.contentsOlder = object->isInventoryPending();
        object->requestInventory();
    }

    // What the worn bridge holds. Only scripts matter: another one could speak for the bridge (anything else stays).
    struct ContentsSeen
    {
        bool       known = false;  // false: the contents did not come
        bool       ours  = false;  // our script is there (the first of them is kept)
        uuid_vec_t extraScripts;   // every other script, and more copies of ours: removed from the object
    };

    ContentsSeen judgeContents(LLViewerObject* object, LLInventoryObject::object_list_t* inventory)
    {
        ContentsSeen seen;
        if (object->flagInventoryEmpty())
        {
            seen.known = true;
            return seen;
        }
        if (!inventory)
        {
            return seen;
        }
        seen.known = true;
        for (const LLPointer<LLInventoryObject>& entry : *inventory)
        {
            const LLInventoryItem* it = dynamic_cast<const LLInventoryItem*>(entry.get());
            if (!it || it->getType() != LLAssetType::AT_LSL_TEXT)
            {
                continue; // the contents' own folder entry, and anything that is not a script
            }
            if (!seen.ours && it->getName() == bridgeName() && it->getCreatorUUID() == gAgentID)
            {
                seen.ours = true;
            }
            else
            {
                seen.extraScripts.push_back(it->getUUID());
            }
        }
        return seen;
    }

    // The hello for the URL held goes only to a bridge whose contents were seen holding no other script for that URL
    // (cleanOf): another script in it could have said that URL itself, and its pose lines would pass as the bridge's.
    // Not seen yet: the contents are asked for, and onContents() says hello once they are clean.
    void helloWhenClean()
    {
        Bridge&         b      = bridge();
        LLViewerObject* object = wornObject();
        if (!object || b.url.empty() || b.helloSent || b.helloOk)
        {
            return;
        }
        if (refuseLinkedParts(object))
        {
            return; // prims linked to it after it was worn: no hello
        }
        if (object->getID() == b.cleanOf)
        {
            sendHello();
            return;
        }
        askContents(object);
    }

    void onContents(const LLUUID& object_id, U32 ask_number, const ContentsSeen& seen)
    {
        Bridge& b = bridge();
        if (!LLApp::isRunning() || b.contentsAskedOf != object_id || ask_number != b.contentsAskNumber)
        {
            return; // cancelled since, or asked again before this answer's idle (it may be from before the new ask)
        }
        b.contentsAskedOf.setNull();
        LLViewerObject* object = wornObject();
        const bool      worn   = object && object->getID() == object_id;
        const bool      older  = b.contentsOlder; // the answer of a request on the way before ours
        if (worn && refuseLinkedParts(object))
        {
            return; // its contents are only its root prim's: not used at all
        }
        if (!seen.extraScripts.empty() && worn)
        {
            // Our own object: a script that is not the bridge's goes (the bridge's script also stops them). It may
            // have said the URL held: that is dropped, and the hello waits for our script's next announcement, after
            // the contents were seen clean.
            LL_WARNS("PlusLSLBridge") << "Bridge held " << seen.extraScripts.size() << " other script(s): removed" << LL_ENDL;
            for (const LLUUID& script : seen.extraScripts)
            {
                object->removeInventory(script);
            }
            b.cleanOf.setNull();
            if (!b.url.empty() && !b.helloOk)
            {
                dropUrl();
            }
        }
        else if (seen.known && worn && !older)
        {
            b.cleanOf = object_id;
        }
        if (worn && older && b.contentsAsk == ContentsAsk::First)
        {
            if (b.step == Step::CheckContents)
            {
                b.stepDeadline = now() + STEP_SECONDS; // its own time for the one more look (once: Again is the last)
            }
            askContents(object, ContentsAsk::Again); // once more, now that the older request is answered
            return;
        }
        if (b.step == Step::CheckContents)
        {
            if (!worn)
            {
                // Not the object worn now: the one worn now (worn again as another) is looked at; none worn: watchWorn
                // sees to it (taken off while its hello is waited for: a repair). No script is added for it.
                if (object)
                {
                    askContents(object);
                }
                return;
            }
            if (seen.ours || !seen.known)
            {
                setStep(Step::AwaitAnswer, START_ANSWER_SECONDS); // not known: perhaps the script is there
                return;
            }
            addScript();
            return;
        }
        if (b.step == Step::AwaitAnswer && b.cleanOf == object_id)
        {
            helloWhenClean(); // the announcement came first: its hello now (not clean: after the next announcement)
        }
    }

    void ContentsListener::inventoryChanged(LLViewerObject* object, LLInventoryObject::object_list_t* inventory,
                                            S32 /*serial_num*/, void* /*user_data*/)
    {
        object->removeInventoryListener(this);
        // Only what was seen is taken here, inside the object's callback loop; it is acted on in the next idle, for the
        // ask it answers (a newer ask before that idle has its own answer to wait for).
        const LLUUID       object_id  = object->getID();
        const U32          ask_number = bridge().contentsAskNumber;
        const ContentsSeen seen       = judgeContents(object, inventory);
        doOnIdleOneTime([object_id, ask_number, seen]() { onContents(object_id, ask_number, seen); });
    }

    bool readScript(std::string& text)
    {
        const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "plus", "lslbridge", SCRIPT_FILE);
        llifstream file(path.c_str(), std::ios::in | std::ios::binary);
        if (!file.is_open())
        {
            return false;
        }
        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (content.empty() || content.size() > SCRIPT_FILE_MAX_BYTES)
        {
            return false;
        }
        text.swap(content);
        return true;
    }

    void onScriptUploaded(U32 epoch, const LLUUID& script_item, const LLSD& response)
    {
        Bridge& b = bridge();
        if (!LLApp::isRunning() || epoch != b.epoch || b.step != Step::AddScript)
        {
            return;
        }
        if (!response["compiled"].asBoolean())
        {
            LL_WARNS("PlusLSLBridge") << "Bridge script did not compile (" << response["errors"].size() << " error(s))" << LL_ENDL;
            fail(Failure::Make, "script did not compile");
            return;
        }
        LLViewerObject* object = wornObject();
        if (!object)
        {
            onTakenOff(); // taken off meanwhile: as any take-off (put on again in its turns), then made anew
            return;
        }
        LLViewerInventoryItem* script = gInventory.getItem(script_item);
        if (!script)
        {
            // The item went before it was put in: made again, MAX_SCRIPT_TRIES times an attempt (only this is
            // counted: a bridge taken off while its script is made is put on again and its script made anew freely).
            if (++bridge().scriptTries > MAX_SCRIPT_TRIES)
            {
                fail(Failure::Make, "script item gone again");
                return;
            }
            addScript();
            return;
        }
        object->saveScript(script, true, true, LLUUID::null);
        setStep(Step::AwaitAnswer, START_ANSWER_SECONDS);
    }

    void onScriptItemMade(U32 epoch, const LLUUID& script_item)
    {
        Bridge& b = bridge();
        if (!LLApp::isRunning() || epoch != b.epoch || b.step != Step::AddScript)
        {
            return;
        }
        if (script_item.isNull())
        {
            fail(Failure::Make, "script item not made");
            return;
        }
        std::string text;
        if (!readScript(text))
        {
            fail(Failure::Make, "script file missing");
            return;
        }
        const std::string capability = gAgent.getRegionCapability(SCRIPT_CAPABILITY);
        if (capability.empty())
        {
            fail(Failure::Make, "no script upload here");
            return;
        }
        LLResourceUploadInfo::ptr_t info = std::make_shared<LLScriptAssetUpload>(
            script_item, COMPILE_TARGET, text,
            [epoch](LLUUID item_id, LLUUID, LLUUID, LLSD response) { onScriptUploaded(epoch, item_id, response); },
            [epoch](LLUUID, LLUUID, LLSD, std::string) -> bool
            {
                if (LLApp::isRunning() && epoch == bridge().epoch && bridge().step == Step::AddScript)
                {
                    fail(Failure::Make, "script upload refused");
                }
                return true; // told here
            });
        LLViewerAssetUpload::EnqueueInventoryUpload(capability, info);
    }

    void addScript()
    {
        Bridge& b = bridge();
        if (b.folder.isNull())
        {
            setStep(Step::FindFolder, STEP_SECONDS); // the script item goes into our folder; back here through Wear
            return;
        }
        // Every making counts here, those cut short by a take-off too: a bridge taken off at every wear ends in Failed
        // (and the retries) instead of making scripts all session.
        if (++b.scriptMakes > MAX_SCRIPT_MAKES)
        {
            fail(Failure::Make, "script made too often");
            return;
        }
        // The script items of earlier makings cut short go first (the trash keeps them).
        removeOtherBridgeItems();
        setStep(Step::AddScript, SCRIPT_SECONDS);
        const U32 epoch = b.epoch;
        create_inventory_item(gAgentID, gAgentSessionID, b.folder, LLTransactionID::tnull, bridgeName(), bridgeName(),
                              LLAssetType::AT_LSL_TEXT, LLInventoryType::IT_LSL, NO_INV_SUBTYPE, PERM_ALL,
                              new LLBoostFuncInventoryCallback([epoch](const LLUUID& id) { onScriptItemMade(epoch, id); }));
    }

    // How findLibraryRock() takes a library folder still being fetched.
    enum class LibraryLook
    {
        WaitForFolders, // nothing is chosen until the rock folder is loaded (without one there, all of the library)
        WhatIsLoaded,   // the step's time is up: what is loaded is all there is
    };

    struct LibraryRock
    {
        LLViewerInventoryItem* item    = nullptr; // null: none (yet)
        bool                   waiting = false;   // a library folder is still being fetched: "none" is not sure yet
    };

    // The viewer's own fetch of all of the library (as the inventory window's Library starts it), when none started.
    void fetchLibrary(const LLUUID& library)
    {
        LLInventoryModelBackgroundFetch& fetch = LLInventoryModelBackgroundFetch::instance();
        if (!fetch.libraryFetchStarted())
        {
            fetch.start(library, true);
        }
    }

    // Whether every folder of the library is loaded (the root and each folder under it complete): only then does
    // "not found" mean that the library has none.
    bool libraryAllLoaded(const LLUUID& library, const LLInventoryModel::cat_array_t& folders)
    {
        if (!gInventory.isCategoryComplete(library))
        {
            return false;
        }
        for (const LLPointer<LLViewerInventoryCategory>& folder : folders)
        {
            if (!gInventory.isCategoryComplete(folder->getUUID()))
            {
                return false;
            }
        }
        return true;
    }

    // A library object to copy (research/215: the item id, the name in the rock folder, the name anywhere in the
    // library, another rock of the rock folder).
    LibraryRock findLibraryRock(LibraryLook look)
    {
        LibraryRock  found;
        const LLUUID library = gInventory.getLibraryRootFolderID();
        if (library.isNull())
        {
            return found;
        }
        auto usable = [&library](LLViewerInventoryItem* it)
        {
            return it && !it->getIsLinkType() && it->getType() == LLAssetType::AT_OBJECT &&
                   gInventory.isObjectDescendentOf(it->getUUID(), library) && it->getPermissions().allowCopyBy(gAgentID) &&
                   (it->getPermissions().getMaskNextOwner() & PERM_MODIFY);
        };
        if (LIB_ROCK_ITEM_ID.notNull())
        {
            LLViewerInventoryItem* it = gInventory.getItem(LIB_ROCK_ITEM_ID);
            if (usable(it))
            {
                found.item = it;
                return found;
            }
        }

        // The rock folder (Library/Objects/Landscaping), when the library has it.
        LLViewerInventoryItem* spare = nullptr;
        LLUUID                 objects, rocks;
        if (!plus::findChildFolder(library, LIB_OBJECTS_FOLDER, objects) ||
            (objects.notNull() && !plus::findChildFolder(objects, LIB_ROCK_FOLDER, rocks)))
        {
            found.waiting = true;
        }
        else if (rocks.notNull())
        {
            if (!gInventory.isCategoryComplete(rocks))
            {
                gInventory.fetchDescendentsOf(rocks);
                found.waiting = true;
            }
            LLInventoryModel::cat_array_t*  cats  = nullptr;
            LLInventoryModel::item_array_t* items = nullptr;
            gInventory.getDirectDescendentsOf(rocks, cats, items);
            if (items)
            {
                for (const LLPointer<LLViewerInventoryItem>& it : *items)
                {
                    if (!usable(it))
                    {
                        continue;
                    }
                    if (it->getName() == LIB_ROCK_NAME)
                    {
                        found.item = it;
                        return found;
                    }
                    if (LLStringUtil::startsWith(it->getName(), LIB_SPARE_PREFIX) &&
                        (!spare || it->getName() < spare->getName()))
                    {
                        spare = it;
                    }
                }
            }
        }
        if (found.waiting && look == LibraryLook::WaitForFolders)
        {
            return found;
        }
        // Without the name in the rock folder (or without that folder: not loaded yet, or another library) the rest of
        // the library is waited for, once its fetch is through (the background fetch; checked folder by folder below).
        if (look == LibraryLook::WaitForFolders && !LLInventoryModelBackgroundFetch::instance().libraryFetchCompleted())
        {
            fetchLibrary(library);
            found.waiting = true;
            return found;
        }

        // The name anywhere in what is loaded of the library (as Firestorm looks for it).
        LLInventoryModel::cat_array_t  cats;
        LLInventoryModel::item_array_t items;
        gInventory.collectDescendents(library, cats, items, LLInventoryModel::EXCLUDE_TRASH);
        for (const LLPointer<LLViewerInventoryItem>& it : items)
        {
            if (it->getName() == LIB_ROCK_NAME && usable(it))
            {
                found.item = it;
                return found;
            }
        }
        found.item = spare;
        if (!found.item && !libraryAllLoaded(library, cats))
        {
            fetchLibrary(library);
            found.waiting = true; // not all of the library is here: perhaps it has one further in
        }
        return found;
    }

    void onItemCopied(U32 epoch, const LLUUID& new_item)
    {
        Bridge& b = bridge();
        if (!LLApp::isRunning() || epoch != b.epoch || b.step != Step::CopyItem || new_item.isNull())
        {
            return;
        }
        b.item = new_item;
        recordItem(new_item); // the only object taken as the bridge from now on (this account)
        setStep(Step::Wear, WEAR_SECONDS);
    }

    void onFolderMade(U32 epoch, const LLUUID& folder)
    {
        Bridge& b = bridge();
        if (!LLApp::isRunning() || epoch != b.epoch || b.step != Step::MakeFolder)
        {
            return;
        }
        if (folder.isNull())
        {
            fail(Failure::Make, "folder not made");
            return;
        }
        b.folder = folder;
        setStep(Step::FindItem, STEP_SECONDS);
    }

    // Objects of the bridge's name in our folder that are not the bridge we made (recordedItem()): someone may have
    // given them (a folder and an object of our names, with scripts in other prims). Never worn, removed or taken off
    // by us: only told in the log.
    void noteForeignBridges(const LLInventoryModel::item_array_t& items, const LLUUID& ours)
    {
        int foreign = 0;
        int worn    = 0;
        for (const LLPointer<LLViewerInventoryItem>& it : items)
        {
            if (it->getUUID() != ours && !it->getIsLinkType() && it->getType() == LLAssetType::AT_OBJECT &&
                isOurName(it->getName()))
            {
                ++foreign;
                worn += get_is_item_worn(it->getUUID()) ? 1 : 0;
            }
        }
        if (foreign > 0)
        {
            LL_WARNS("PlusLSLBridge") << "Bridge folder: " << foreign << " object(s) of the bridge's name that are not the "
                                      << "recorded bridge (" << worn << " worn): not used, left alone" << LL_ENDL;
        }
    }

    // ---- the steps of Starting (the table's 1'.). Each returns after starting what it waits for.

    // An outfit being put on (the login's own, or one the person wears). A precaution (research/353 D4): the official
    // LLAppearanceMgr::updateAppearanceFromCOF() takes off, once the avatar is fully loaded, the attachments its COF does
    // not list, but keeps one just worn whose COF link is not made yet (LLAttachmentsMgr::getPendingAttachments()), so a
    // bridge worn meanwhile would mostly stay; waiting also keeps the bridge's requests out of the busiest part of the
    // login. Never for long: the official checkStuckAppearance() ends an outfit change that hangs (600 s at most).
    bool outfitBeingPutOn()
    {
        return gAgentWearables.changeInProgress();
    }

    // The temporary attachments worn (an outfit change keeps them, and they take places as the others do). The one
    // count of them for roomForBridge() and bridgeFitsWith() (research/353, the third review).
    U32 countTempAttachments()
    {
        U32 temporary = 0;
        if (!isAgentAvatarValid())
        {
            return temporary;
        }
        for (const auto& [point, attachment] : gAgentAvatarp->mAttachmentPoints)
        {
            if (!attachment)
            {
                continue;
            }
            for (const LLPointer<LLViewerObject>& object : attachment->mAttachedObjects)
            {
                temporary += (object.notNull() && !object->isDead() && object->isTempAttachment()) ? 1 : 0;
            }
        }
        return temporary;
    }

    // Whether the bridge has a place beside other_objects attachments of the person (the bridge not counted), theirs
    // first (research/353, the second and third reviews): those, the temporary attachments worn, the bridge and one
    // place left for the person. Else the bridge is not asked for, or left out of a new COF (it comes off), and is put
    // on again once a change of the attachments leaves a place (Failed NoSlot until then). A limit not known (-1): it
    // fits, as the official canAttachMoreObjects() takes it.
    bool bridgeFitsWith(U32 other_objects)
    {
        if (!isAgentAvatarValid())
        {
            return false;
        }
        const S32 most = gAgentAvatarp->getMaxAttachments();
        if (most < 0)
        {
            return true;
        }
        return other_objects + countTempAttachments() + 2 <= static_cast<U32>(most);
    }

    // Whether the avatar, not wearing the bridge, has a place for it with one left for the person (research/353, review
    // M1): by the attachments worn (canAttachMoreObjects counts the temporary ones too) and by those the COF lists (the
    // outfit may still be bringing some) with the temporary ones (bridgeFitsWith), the bridge not counted. Without one
    // the bridge is not asked for (it would be refused, or take the last place).
    bool roomForBridge()
    {
        if (!isAgentAvatarValid() || !gAgentAvatarp->canAttachMoreObjects(2))
        {
            return false;
        }
        const LLUUID& item = bridge().item;
        LLInventoryModel::cat_array_t  cats;
        LLInventoryModel::item_array_t links;
        LLIsType                       is_object(LLAssetType::AT_OBJECT);
        gInventory.collectDescendentsIf(LLAppearanceMgr::instance().getCOF(), cats, links, LLInventoryModel::EXCLUDE_TRASH,
                                        is_object);
        U32 listed = 0;
        for (const LLPointer<LLViewerInventoryItem>& link : links)
        {
            listed += (link->getLinkedUUID() != item) ? 1 : 0;
        }
        return bridgeFitsWith(listed);
    }

    // Logged in, the avatar and the inventory there, and no outfit being put on (the login's own first). No deadline.
    void stepWaitLogin()
    {
        if (LLStartUp::getStartupState() < STATE_STARTED || !isAgentAvatarValid() || !gInventory.isInventoryUsable() ||
            gInventory.getRootFolderID().isNull() || outfitBeingPutOn())
        {
            return;
        }
        if (!LLGridManager::getInstance()->isSystemGrid())
        {
            fail(Failure::NotHere, "not a Second Life grid");
            return;
        }
        connectSignals();
        setStep(Step::FindFolder, STEP_SECONDS);
    }

    void stepFindFolder()
    {
        Bridge& b = bridge();
        LLUUID  found;
        if (!plus::findChildFolder(gInventory.getRootFolderID(), plus::CHARIS_FOLDER_NAME, found))
        {
            if (stepExpired())
            {
                fail(Failure::Make, "inventory root not loaded");
            }
            return;
        }
        if (found.notNull())
        {
            b.folder = found;
            setStep(Step::FindItem, STEP_SECONDS);
            return;
        }
        setStep(Step::MakeFolder, STEP_SECONDS);
        const U32 epoch = b.epoch;
        plus::makeCharisFolder([epoch](const LLUUID& id) { onFolderMade(epoch, id); }); // shared with plus/ao
    }

    void stepFindItem()
    {
        Bridge& b = bridge();
        if (b.folder.isNull() || !gInventory.getCategory(b.folder) || inTrash(b.folder))
        {
            b.folder.setNull();
            setStep(Step::FindFolder, STEP_SECONDS);
            return;
        }
        if (!gInventory.isCategoryComplete(b.folder))
        {
            gInventory.fetchDescendentsOf(b.folder);
            if (stepExpired())
            {
                fail(Failure::Make, "folder not loaded");
            }
            return;
        }
        // The bridge is the object recorded when this viewer made it (this account), nothing else of its name. Gone
        // (removed, in the trash) or none made yet: a new one is made from the library and recorded.
        const LLUUID                 recorded = recordedItem();
        const LLViewerInventoryItem* it       = recorded.notNull() ? gInventory.getItem(recorded) : nullptr;
        if (recorded.notNull() && !it && !stepExpired())
        {
            // Recorded but not in the inventory read so far (moved to a folder not loaded yet, perhaps): asked for by
            // its id, and waited for the step's time before another is made (a second bridge, the first left over).
            if (!b.itemFetchAsked)
            {
                b.itemFetchAsked = true;
                LLInventoryModelBackgroundFetch::instance().scheduleItemFetch(recorded, true);
            }
            return;
        }
        const LLUUID found = (it && !it->getIsLinkType() && it->getType() == LLAssetType::AT_OBJECT && !inTrash(recorded))
                                 ? recorded
                                 : LLUUID::null;
        if (found.notNull() && isOldBridgeName(it->getName()))
        {
            // The bridge of an old version of the script (1.0 says its URL unasked, which another viewer would show in
            // the owner's chat, research/366; 1.1 has no pose sharing, research/374): never worn, made anew. One worn
            // already (the login's outfit) comes off.
            b.item = recorded;
            if (wornObject())
            {
                LL_INFOS("PlusLSLBridge") << "Bridge of an old version worn: taken off, made anew" << LL_ENDL;
                takeOffOurselves(OwnDetach::Recreate);
                return;
            }
            if (LLAppearanceMgr::instance().isLinkedInCOF(recorded))
            {
                // Its wear is on the way (the login's outfit): waited for as long as a normal wear (WEAR_SECONDS from
                // here, not what the folder's fetch left of this step), then taken off.
                if (!b.oldWaiting)
                {
                    b.oldWaiting   = true;
                    b.stepDeadline = now() + WEAR_SECONDS;
                }
                if (!stepExpired())
                {
                    return;
                }
            }
            LL_INFOS("PlusLSLBridge") << "Bridge of an old version: made anew" << LL_ENDL;
            LLAppearanceMgr::instance().removeCOFItemLinks(recorded); // else the next login would wear it again
            // One that still comes after this (the outfit's wear was slow) is taken off by watchGoneOld(): the official
            // attachments manager links what arrives into the COF again, whatever the trash holds.
            b.goneOld.push_back({ recorded, now() + 2 * WEAR_SECONDS });
            forgetItem();
            setStep(Step::FindLibrary, STEP_SECONDS);
            return;
        }
        LLInventoryModel::cat_array_t*  cats  = nullptr;
        LLInventoryModel::item_array_t* items = nullptr;
        gInventory.getDirectDescendentsOf(b.folder, cats, items);
        if (items)
        {
            noteForeignBridges(*items, found);
        }
        b.item = found;
        if (found.notNull())
        {
            setStep(Step::Wear, WEAR_SECONDS);
            b.waitForCof = true; // at login the outfit brings what is linked in the COF
        }
        else
        {
            setStep(Step::FindLibrary, STEP_SECONDS);
        }
    }

    void stepFindLibrary()
    {
        Bridge&           b       = bridge();
        const bool        expired = stepExpired();
        const LibraryRock found   = findLibraryRock(expired ? LibraryLook::WhatIsLoaded : LibraryLook::WaitForFolders);
        if (!found.item)
        {
            if (!found.waiting)
            {
                fail(Failure::NoLibrary, "no library object"); // all of the library is loaded and has none: not tried again
            }
            else if (expired)
            {
                fail(Failure::Make, "library folder not loaded in time"); // it may load later: tried again
            }
            return;
        }
        if (b.folder.isNull())
        {
            setStep(Step::FindFolder, STEP_SECONDS);
            return;
        }
        setStep(Step::CopyItem, STEP_SECONDS);
        const U32 epoch = b.epoch;
        copy_inventory_item(gAgentID, found.item->getPermissions().getOwner(), found.item->getUUID(), b.folder, bridgeName(),
                            new LLBoostFuncInventoryCallback([epoch](const LLUUID& id) { onItemCopied(epoch, id); }));
    }

    void stepWear()
    {
        Bridge& b = bridge();
        if (b.item.isNull() || !gInventory.getItem(b.item) || inTrash(b.item))
        {
            b.item.setNull();
            setStep(Step::FindItem, STEP_SECONDS);
            return;
        }
        if (LLViewerObject* object = wornObject())
        {
            b.wearAskedOf.setNull(); // it came
            if (!object->isHUDAttachment())
            {
                // Worn on the body (the login's outfit wears an item where it was saved): its key is on every list and its
                // script takes no call there. Put on again, on the HUD (ATTACH_POINT); a repair, so it ends.
                repair(Repair::Rewear, "worn on the body");
                return;
            }
            if (refuseLinkedParts(object))
            {
                return;
            }
            setStep(Step::CheckContents, STEP_SECONDS);
            askContents(object);
            return;
        }
        if (!b.wearAsked)
        {
            if (outfitBeingPutOn())
            {
                b.stepDeadline = now() + WEAR_SECONDS; // asked once the outfit is on, with the step's whole time
                return;
            }
            // Right after FindItem an item linked in the COF is left to the outfit (the login). Asked for in every
            // other case: not linked, a repair, a take-off (whose link may stay in the COF for a while).
            const bool ask = !b.waitForCof || !LLAppearanceMgr::instance().isLinkedInCOF(b.item);
            if (ask && !roomForBridge())
            {
                // Not asked: looked at again at every tick until the step's time is up, then Failed (NoSlot) with no
                // try by the clock: a new attempt once a change of the attachments leaves a place (tick()).
                if (stepExpired())
                {
                    fail(Failure::NoSlot, "no attachment place to spare");
                }
                return;
            }
            b.wearAsked = true;
            if (ask)
            {
                LLAttachmentsMgr::instance().addAttachmentRequest(b.item, ATTACH_POINT, true);
                b.wearAskedOf = b.item; // taken off again if it comes after the bridge stopped (watchLateWear)
                b.wearAskedAt = now();
            }
        }
        if (stepExpired())
        {
            repair(Repair::Rewear, "not worn in time");
        }
    }

    void stepAwaitAnswer()
    {
        if (!stepExpired())
        {
            return;
        }
        if (wornObject())
        {
            enterSilent();
        }
        else
        {
            setStep(Step::Wear, WEAR_SECONDS);
        }
    }

    void stepDetach()
    {
        Bridge& b = bridge();
        if (!wornObject())
        {
            const OwnDetach why = b.ownDetach;
            b.ownDetach         = OwnDetach::None;
            if (why == OwnDetach::Recreate)
            {
                forgetItem();
                setStep(Step::FindLibrary, STEP_SECONDS);
            }
            else
            {
                setStep(Step::Wear, WEAR_SECONDS);
            }
            return;
        }
        if (stepExpired())
        {
            b.ownDetach = OwnDetach::None;
            enterSilent(); // still on: this repair is spent
        }
    }

    void runStep()
    {
        switch (bridge().step)
        {
            case Step::None:        return;
            case Step::WaitLogin:   stepWaitLogin(); return;
            case Step::FindFolder:  stepFindFolder(); return;
            case Step::FindItem:    stepFindItem(); return;
            case Step::FindLibrary: stepFindLibrary(); return;
            case Step::Wear:        stepWear(); return;
            case Step::AwaitAnswer: stepAwaitAnswer(); return;
            case Step::Detach:      stepDetach(); return;
            case Step::CheckContents:
                if (stepExpired())
                {
                    cancelContents();
                    setStep(Step::AwaitAnswer, START_ANSWER_SECONDS); // perhaps the script is there
                }
                return;
            // Waiting for a callback: only the deadline here.
            case Step::MakeFolder:
                if (stepExpired()) fail(Failure::Make, "folder not made in time");
                return;
            case Step::CopyItem:
                if (stepExpired()) fail(Failure::Make, "copy not made in time");
                return;
            case Step::AddScript:
                if (stepExpired()) fail(Failure::Make, "script not added in time");
                return;
        }
    }

    // Watches the worn bridge while it should be on (the table's "taken off": off the avatar, and not linked in
    // the COF, or still not back after WEAR_SECONDS; a teleport keeps the link in the COF).
    void watchWorn()
    {
        Bridge& b = bridge();
        const bool watched = (b.state == State::Running || b.state == State::Silent ||
                              b.step == Step::CheckContents || b.step == Step::AddScript || b.step == Step::AwaitAnswer);
        if (!watched || b.item.isNull() || b.ownDetach != OwnDetach::None)
        {
            b.notWornSince = 0.0;
            return;
        }
        if (wornObject())
        {
            b.notWornSince = 0.0;
            b.wearAskedOf.setNull(); // worn: no wear is on its way any more
            return;
        }
        const F64 t = now();
        if (b.notWornSince <= 0.0)
        {
            b.notWornSince = t;
            return;
        }
        const F64 off_for = t - b.notWornSince;
        if (off_for >= WEAR_SECONDS ||
            (off_for >= DETACH_SETTLE_SECONDS && !LLAppearanceMgr::instance().isLinkedInCOF(b.item)))
        {
            onTakenOff();
        }
    }

    // A wear asked for before the bridge stopped (Off, or Failed with no new try to come): the object that comes after
    // that is taken off again (else it stays on the HUD, its script never put in, and comes back at the next login
    // through the COF; no line of it comes to take it off by). Watched WEAR_SECONDS after the ask; true while it is.
    bool watchLateWear()
    {
        Bridge& b = bridge();
        if (b.wearAskedOf.isNull())
        {
            return false;
        }
        if (now() - b.wearAskedAt >= WEAR_SECONDS)
        {
            b.wearAskedOf.setNull(); // its time is over (first: a bridge worn long since is not "late")
            return false;
        }
        const bool stopped = b.state == State::Off || (b.state == State::Failed && b.retryAt <= 0.0);
        if (!stopped)
        {
            return false; // the steps (or the new try) see to it
        }
        if (isAgentAvatarValid() && gAgentAvatarp->getWornAttachment(b.wearAskedOf))
        {
            // ownDetach is left as it is: nothing watches the take-off while Off or Failed.
            LL_INFOS("PlusLSLBridge") << "Bridge worn after it stopped: taken off" << LL_ENDL;
            LLVOAvatarSelf::detachAttachmentIntoInventory(b.wearAskedOf);
            b.wearAskedOf.setNull();
            return false;
        }
        return true;
    }

    void beginAttempt();

    // Items of old versions that were made anew (stepFindItem) and still come onto the avatar: taken off, for 2 *
    // WEAR_SECONDS after; true while any is watched.
    bool watchGoneOld()
    {
        Bridge& b = bridge();
        for (auto it = b.goneOld.begin(); it != b.goneOld.end();)
        {
            if (now() >= it->until)
            {
                it = b.goneOld.erase(it);
            }
            else if (isAgentAvatarValid() && gAgentAvatarp->getWornAttachment(it->id))
            {
                LL_INFOS("PlusLSLBridge") << "An old version's bridge came late: taken off" << LL_ENDL;
                LLAppearanceMgr::instance().removeCOFItemLinks(it->id);
                LLVOAvatarSelf::detachAttachmentIntoInventory(it->id);
                it = b.goneOld.erase(it);
            }
            else
            {
                ++it;
            }
        }
        return !b.goneOld.empty();
    }

    // Every TICK_SECONDS while something waits; returns true (stop) when nothing does.
    bool tick()
    {
        Bridge& b = bridge();
        if (!LLApp::isRunning())
        {
            b.ticking = false;
            return true;
        }
        const bool gone_old = watchGoneOld();
        if (b.state == State::Off)
        {
            const bool late_wear = watchLateWear() || gone_old; // Off: only a late wear (and an old item's) is watched
            b.ticking            = late_wear;
            return !late_wear;
        }
        for (int n = 0; n < MAX_STEPS_PER_TICK; ++n)
        {
            const Step before = b.step;
            runStep();
            if (b.step == before)
            {
                break;
            }
        }
        if (b.state == State::Silent && b.step == Step::None && !b.urlRefused)
        {
            if (!scriptsRunHere())
            {
                b.answerWaited = false; // its script is stopped here: the wait it had did not count
            }
            else if (!b.answerWaited)
            {
                // Scripts run here again (another parcel): the script may answer by itself; a repair only after that.
                LL_INFOS("PlusLSLBridge") << "Bridge where scripts run again: waiting for its answer" << LL_ENDL;
                setStep(Step::AwaitAnswer, START_ANSWER_SECONDS);
            }
            else
            {
                repair(Repair::Auto, "not answering");
            }
        }
        if (b.state == State::Detached && b.step == Step::None && takeRewear())
        {
            LL_INFOS("PlusLSLBridge") << "Bridge put on again (a turn came free)" << LL_ENDL;
            setStep(Step::Wear, WEAR_SECONDS);
        }
        if (b.state == State::Failed && b.retryAt > 0.0 && now() >= b.retryAt)
        {
            b.retryAt = 0.0;
            LL_INFOS("PlusLSLBridge") << "Bridge tried again (" << b.retries.used << " of " << MAX_RETRIES << ")" << LL_ENDL;
            beginAttempt();
        }
        if (b.state == State::Failed && b.waitingForPlace && roomForBridge())
        {
            // Woken by a change of the attachments (onAttachmentsChanged): the person took something off. Only then,
            // never by the clock, so no wear is asked for while there is no place.
            LL_INFOS("PlusLSLBridge") << "Bridge has an attachment place to spare again: tried again" << LL_ENDL;
            beginAttempt();
        }
        probeIfDue();
        watchWorn();
        const bool late_wear = watchLateWear();

        const bool waiting = b.step != Step::None || b.state == State::Silent || b.notWornSince > 0.0 ||
                             (b.state == State::Detached) || (b.state == State::Failed && b.retryAt > 0.0) || late_wear ||
                             gone_old;
        if (!waiting)
        {
            b.ticking = false;
            return true;
        }
        return false;
    }

    void onAttachmentsChanged()
    {
        if (bridge().state != State::Off)
        {
            wake(); // looked at in the tick, not inside the attach
        }
    }

    void onRegionChanged();

    // Once logged in (the appearance manager exists then); again after a stop. Idempotent.
    void connectSignals()
    {
        Bridge& b = bridge();
        if (!b.attachConnection.connected())
        {
            b.attachConnection = LLAppearanceMgr::instance().setAttachmentsChangedCallback(&onAttachmentsChanged);
        }
        if (!b.regionConnection.connected())
        {
            b.regionConnection = gAgent.addRegionChangedCallback(&onRegionChanged);
        }
    }

    void onRegionChanged()
    {
        Bridge& b    = bridge();
        b.urlRefused = false; // another region may have a URL for it (the script asks there)
        b.certificateFailed = false; // another simulator, another certificate
        // Running, waiting for an answer, or Silent: the script announces a new URL there by itself, so it is waited
        // for (no repair before that wait is over). Silent also when the worn bridge is not seen yet (right after a
        // teleport): when the wait is over, it is put on if it is not worn, and taken as taken off (watchWorn) if it
        // came off.
        const bool silent = b.state == State::Silent && b.step == Step::None;
        if (b.state != State::Running && b.step != Step::AwaitAnswer && !silent)
        {
            if (b.state != State::Off)
            {
                wake();
            }
            return;
        }
        // The script's URL was the old region's; it announces a new one (and again, until the hello).
        const bool teleport = gAgent.getTeleportState() != LLAgent::TELEPORT_NONE;
        dropUrl();
        setStep(Step::AwaitAnswer, teleport ? START_ANSWER_SECONDS : ANSWER_SECONDS);
    }

    // A new attempt (after the setting went on, or a Failed's retry): the steps from the login wait, the repairs and
    // script tries of the attempt from 0. The session's retries and take-off turns are kept.
    void beginAttempt()
    {
        Bridge& b = bridge();
        b.repairs.reset();
        b.waitingForPlace = false;
        b.scriptTries  = 0;
        b.scriptMakes  = 0;
        b.retryAt      = 0.0;
        b.urlRefused   = false;
        b.certificateFailed = false;
        b.refusedInRow = 0;
        b.lastRefusal  = proto::AnnouncementCheck::Ok;
        b.answerWaited = true;
        b.cleanOf.setNull();
        b.wearAskedOf.setNull(); // a wear still on its way is the steps' again (FindItem, Wear)
        b.ownDetach    = OwnDetach::None;
        b.notWornSince = 0.0;
        leaveSteps();
        setStep(Step::WaitLogin, 0.0);
    }

    // The setting went on (or the login with it on): the session's counts from 0 too.
    void start()
    {
        Bridge& b = bridge();
        if (b.state == State::Starting || b.state == State::Running)
        {
            return;
        }
        b.retries.reset();
        std::fill(std::begin(b.rewearAt), std::end(b.rewearAt), 0.0);
        beginAttempt();
    }

    void stop()
    {
        Bridge& b = bridge();
        if (b.state == State::Off)
        {
            return;
        }
        if (wornObject())
        {
            b.ownDetach = OwnDetach::Off;
            LLVOAvatarSelf::detachAttachmentIntoInventory(b.item);
            if (b.wearAskedOf == b.item)
            {
                b.wearAskedOf.setNull(); // it came, and goes now
            }
        }
        leaveSteps();
        b.attachConnection.disconnect();
        b.regionConnection.disconnect();
        b.notWornSince = 0.0;
        b.retryAt      = 0.0;
        b.waitingForPlace = false;
        setState(State::Off); // the tick stops at its next call (after a late wear, if one was asked for: watchLateWear)
        if (b.wearAskedOf.notNull())
        {
            wake();
        }
    }

    // The setting's signal: the person's change once logged in (Preferences, which enables it only then; the Debug
    // Settings), or the account's file read at login (a "false" saved there) and a login tried again (the values back to
    // their defaults, plus.cpp onStartupState). Its value, not settingOn(): the cached control may not have it yet.
    void onSettingChanged(bool on)
    {
        if (!on)
        {
            stop();
            return;
        }
        Bridge& b = bridge();
        if (LLStartUp::getStartupState() == STATE_STARTED)
        {
            b.turnedOnByUser = true; // the person turned it on: its failures are told
        }
        b.notified = 0; // turned on again: a failure is told again
        if (plus::safemode::active())
        {
            LL_INFOS("PlusLSLBridge") << "Bridge not started: a safe start" << LL_ENDL;
            return;
        }
        start();
    }

    // ------------------------------------------------------------------ the bridge's lines

    // The chatter is the worn bridge: the root of its link set, with no other prim linked to it, is our attachment of
    // the bridge item. Before the steps found the item (the bridge's announcement can come right after its call at login), the
    // item is taken as the bridge only when it is the one recorded when this viewer made it (recordedItem()): an
    // object of the bridge's name and place that someone gave is never the bridge.
    bool isOurBridge(LLViewerObject* chatter)
    {
        LLViewerObject* root = chatter->getRootEdit();
        // The bridge's script is in its root prim: a script of a child prim is not the bridge (and the bridge has none).
        if (!root || root != chatter || !root->isAttachment() || root->numChildren() > 0 || !isAgentAvatarValid())
        {
            return false;
        }
        const LLUUID worn_item = root->getAttachmentItemID();
        if (worn_item.isNull() || gAgentAvatarp->getWornAttachment(worn_item) != root)
        {
            return false;
        }
        Bridge& b = bridge();
        if (b.item.notNull())
        {
            return worn_item == b.item;
        }
        if (worn_item != recordedItem())
        {
            return false;
        }
        b.item = worn_item;
        return true;
    }

    // Why a pose line of the worn bridge (the chatter) is not taken, as a word for the log (never a value); null: it
    // is taken. Taken once its contents were seen holding no other script for the URL held (its script relays only
    // after the hello, which waits for that), in the form, and not naming ourselves.
    const char* poseLineRefusal(const LLViewerObject* chatter, const std::string& text, proto::PoseLine& line)
    {
        if (bridge().cleanOf != chatter->getID())
        {
            return "contents";
        }
        if (!proto::parsePoseLine(text, line))
        {
            return "form";
        }
        if (LLUUID(line.peer) == gAgentID)
        {
            return "self";
        }
        return nullptr;
    }

    // A worn bridge while the setting is off (worn by hand, or from an earlier session): taken off again.
    void takeOffWhileOff()
    {
        Bridge& b = bridge();
        if (wornObject())
        {
            b.ownDetach = OwnDetach::Off;
            LLVOAvatarSelf::detachAttachmentIntoInventory(b.item);
        }
    }

    void onAnnouncement(const proto::Announcement& announcement)
    {
        Bridge& b = bridge();
        if (!settingOn())
        {
            takeOffWhileOff();
            return;
        }
        if (b.state == State::Off)
        {
            start();
        }
        if (b.step == Step::Detach)
        {
            return; // being taken off by us
        }
        b.wearAskedOf.setNull(); // the worn bridge speaks: no wear is on its way any more
        if (b.state == State::Failed && b.certificateFailed)
        {
            // Its certificate did not pass here: the same URL said again would fail again (and spend a retry every
            // 15 s). Dropped until the retry's time (beginAttempt) or a region change (another simulator).
            return;
        }
        if (announcement.version != proto::BRIDGE_VERSION)
        {
            repair(Repair::Recreate, "other script version");
            return;
        }
        const LLUUID token(announcement.token);
        if (announcement.url == b.url && token == b.token && (b.helloSent || b.helloOk))
        {
            return; // this one is answered already
        }
        if (announcement.url != b.url)
        {
            b.cleanOf.setNull(); // a new URL (a region crossed, worn again): its contents are looked at again, once
        }
        dropUrl();
        b.url          = announcement.url;
        b.token        = token;
        b.urlRefused   = false;
        b.retryAt      = 0.0;
        b.refusedInRow = 0;
        setStep(Step::AwaitAnswer, START_ANSWER_SECONDS);
        helloWhenClean(); // at once only for the same URL said again once its contents were clean; else after a look
    }

    // An announcement of the worn bridge that did not pass (its form, address or token): the same reason twice in a
    // row would not get better by making a new bridge (the same script and region), so it is Failed (Make: tried
    // again later), not repaired.
    void onRefusedAnnouncement(proto::AnnouncementCheck check)
    {
        Bridge& b = bridge();
        b.refusedInRow = (check == b.lastRefusal) ? b.refusedInRow + 1 : 1;
        b.lastRefusal  = check;
        if (b.refusedInRow >= MAX_REFUSED_ANNOUNCEMENTS && settingOn() && b.state != State::Off && b.state != State::Failed)
        {
            b.refusedInRow = 0;
            fail(Failure::Make, "announcement refused twice for the same reason");
        }
    }

    // The script says the region gives it no URL (all the attachment URLs taken...): it keeps asking there; making a
    // new bridge would not help, so it is not repaired here (until a region change or its announcement).
    void onNoUrl()
    {
        Bridge& b = bridge();
        if (!settingOn())
        {
            takeOffWhileOff();
            return;
        }
        LL_INFOS("PlusLSLBridge") << "Bridge has no URL in this region: waiting, not repaired here" << LL_ENDL;
        b.urlRefused = true;
        if (b.state == State::Running || b.step == Step::AwaitAnswer || b.step == Step::CheckContents)
        {
            enterSilent();
        }
    }

    // ------------------------------------------------------------------ registration

    void init()
    {
        // Settings: app_settings/settings_plus.xml (PlusPoseBridgeEnable). Notifications: notifications.xml
        // (PlusLSLBridge*). The script: app_settings/plus/lslbridge/charis_bridge.lsl. The chat hook:
        // llviewermessage.cpp (lslbridgehooks.h).
        if (LLControlVariable* control = gSavedPerAccountSettings.getControl(SETTING_ENABLE))
        {
            control->getSignal()->connect([](LLControlVariable*, const LLSD& value, const LLSD&)
            {
                onSettingChanged(value.asBoolean());
            });
        }
    }

    void onUIReady()
    {
        // During the login (the menus exist, the world does not), with the default (on) until the account's file is
        // read: WaitLogin waits for the rest, and a "false" read there stops it (onSettingChanged).
        if (plus::safemode::active())
        {
            LL_INFOS("PlusLSLBridge") << "Bridge not started: a safe start" << LL_ENDL;
        }
        if (settingOn())
        {
            start();
        }
    }

    plus::FeatureRegistration sRegistration({ "lslbridge", &init, &onUIReady });
}

namespace plus::lslbridge
{
    State state()
    {
        return bridge().state;
    }

    bool failedForGood()
    {
        const Bridge& b = bridge();
        return b.state == State::Failed && b.retryAt <= 0.0 && !b.waitingForPlace;
    }

    bool isBridgeItem(const LLUUID& item_id)
    {
        return item_id.notNull() && item_id == recordedItem();
    }

    void dropBridgeLinks(std::vector<LLPointer<LLViewerInventoryItem>>& items)
    {
        const LLUUID ours = recordedItem();
        if (ours.isNull())
        {
            return;
        }
        items.erase(std::remove_if(items.begin(), items.end(),
                                   [&ours](const LLPointer<LLViewerInventoryItem>& item)
                                   { return item.notNull() && item->getLinkedUUID() == ours; }),
                    items.end());
    }

    LLViewerInventoryItem* bridgeForOutfit(U32 other_objects)
    {
        const LLUUID ours = recordedItem();
        if (ours.isNull() || !settingOn() || !isAgentAvatarValid())
        {
            return nullptr;
        }
        if (!gAgentAvatarp->getWornAttachment(ours) && !LLAppearanceMgr::instance().isLinkedInCOF(ours))
        {
            return nullptr; // neither worn nor on its way: the steps put it on (where there is a place)
        }
        LLViewerInventoryItem* item = gInventory.getItem(ours);
        if (!item || item->getIsLinkType() || item->getType() != LLAssetType::AT_OBJECT || inTrash(ours))
        {
            return nullptr;
        }
        if (!bridgeFitsWith(other_objects))
        {
            LL_INFOS("PlusLSLBridge") << "Bridge left out of the new outfit: its attachments come first (put on again "
                                      << "once a place is free)" << LL_ENDL;
            return nullptr;
        }
        return item;
    }

    void placeBridgeInNewCof(std::vector<LLPointer<LLViewerInventoryItem>>& obj_items, bool append)
    {
        const LLUUID ours = recordedItem();
        if (ours.isNull() || !settingOn())
        {
            return; // no bridge made, the setting off, a safe start: the list as the official viewer makes it
        }
        // The person's attachments of the new COF, each once (removeDuplicateItems() comes after), the bridge not
        // counted; then the bridge has exactly one place in it, or none (a link of it the new outfit holds, saved by an
        // older viewer, too).
        std::set<LLUUID> others;
        for (const LLPointer<LLViewerInventoryItem>& item : obj_items)
        {
            if (item.notNull() && item->getLinkedUUID() != ours)
            {
                others.insert(item->getLinkedUUID());
            }
        }
        dropBridgeLinks(obj_items);
        if (LLViewerInventoryItem* item = bridgeForOutfit(static_cast<U32>(others.size())))
        {
            if (!append)
            {
                LL_INFOS("PlusLSLBridge") << "Bridge kept on through the outfit's replacing" << LL_ENDL;
            }
            obj_items.push_back(item);
        }
    }

    void ensure()
    {
        // Calling it often is fine: only Off starts (which resets the counts); the other states go on by themselves
        // (repairs, take-off turns, the Failed retries), so this only wakes the tick.
        if (!LLApp::isRunning() || !settingOn())
        {
            return;
        }
        if (bridge().state == State::Off)
        {
            start();
            return;
        }
        wake();
    }

    // A pose1 message (Pose) or a reflect message (Reflect): the same checks and the same 16 posts on their way.
    bool sendRequest(PostKind kind, PoseAction action, const LLUUID& nonce, const LLUUID& target)
    {
        Bridge& b = bridge();
        if (b.state != State::Running || !b.helloOk || b.url.empty() || nonce.isNull() || target.isNull() ||
            target == gAgentID || b.postsInFlight >= MAX_POSTS_IN_FLIGHT)
        {
            return false;
        }
        const std::string body =
            (kind == PostKind::Reflect)
                ? proto::reflectBody(b.token.asString(), static_cast<int>(action), nonce.asString(), target.asString())
                : proto::poseBody(b.token.asString(), static_cast<int>(action), nonce.asString(), target.asString());
        if (body.empty())
        {
            return false;
        }
        PoseSent sent;
        sent.action = action;
        sent.nonce  = nonce;
        sent.target = target;
        post(body, kind, sent); // its result goes to the send result handler, once
        return true;
    }

    bool sendPose(PoseAction action, const LLUUID& nonce, const LLUUID& target)
    {
        return sendRequest(PostKind::Pose, action, nonce, target);
    }

    bool sendReflect(PoseAction action, const LLUUID& nonce, const LLUUID& target)
    {
        return sendRequest(PostKind::Reflect, action, nonce, target);
    }

    // The key of the one lane of shares (a push's key is its target's).
    constexpr char SHARE_KEY[] = "share";

    bool sendShare(int aud, int seq, const std::string& payload)
    {
        if (!laneReady())
        {
            return false;
        }
        return queueLane(PostKind::Share, SHARE_KEY, proto::shareBody(bridge().token.asString(), aud, seq, payload), false);
    }

    bool sendShareEnd(int seq)
    {
        if (!laneReady())
        {
            return false;
        }
        return queueLane(PostKind::Share, SHARE_KEY, proto::shareEndBody(bridge().token.asString(), seq), true);
    }

    bool sendPush(const LLUUID& target, int seq, const std::string& payload)
    {
        if (target.isNull() || target == gAgentID || !laneReady())
        {
            return false;
        }
        return queueLane(PostKind::Push, target.asString(),
                         proto::pushBody(bridge().token.asString(), target.asString(), seq, payload), false);
    }

    bool sendPushEnd(const LLUUID& target, int seq)
    {
        if (target.isNull() || target == gAgentID || !laneReady())
        {
            return false;
        }
        return queueLane(PostKind::Push, target.asString(),
                         proto::pushEndBody(bridge().token.asString(), target.asString(), seq), true);
    }

    void setPoseHandler(PoseHandler handler)
    {
        bridge().poseHandler = std::move(handler);
    }

    void setSendResultHandler(SendResultHandler handler)
    {
        bridge().sendResultHandler = std::move(handler);
    }

    void setShareHandler(ShareHandler handler)
    {
        bridge().shareHandler = std::move(handler);
    }

    void setReflectHandler(PoseHandler handler)
    {
        bridge().reflectHandler = std::move(handler);
    }

    void setReflectResultHandler(SendResultHandler handler)
    {
        bridge().reflectResultHandler = std::move(handler);
    }

    void setPushHandler(PushHandler handler)
    {
        bridge().pushHandler = std::move(handler);
    }

    void setStateHandler(StateHandler handler)
    {
        Bridge& b      = bridge();
        b.stateHandler = std::move(handler);
        if (b.stateHandler)
        {
            b.stateHandler(b.state);
        }
    }

    namespace
    {
        // A line of the sharing kinds (research/374, 374b) said by an object of ours: the speaker is the worn bridge, its
        // contents seen clean (cleanOf), the line in its form and not naming ourselves; else it is dropped unread (it
        // names a peer and carries a pose: never shown, logged or translated). Passed on only while the bridge is not
        // Off. Always true: a line of this form is never a line to show.
        bool handleSharingLine(proto::LineKind kind, const std::string& text, LLViewerObject* chatter, bool from_bridge)
        {
            Bridge& b = bridge();
            if (!from_bridge)
            {
                // Always dropped, whoever it was and whatever the cause (the speaker's object not seen, or not
                // confirmed as the worn bridge for a moment): no script of the person's has a reason to say this form,
                // and these lines come 2 or more a second from every sharer near.
                LL_DEBUGS("PlusLSLBridge") << "A sharing line of an object not confirmed as the worn bridge dropped" << LL_ENDL;
                return true;
            }
            if (b.cleanOf != chatter->getID())
            {
                LL_DEBUGS("PlusLSLBridge") << "A sharing line of the bridge dropped: contents" << LL_ENDL;
                return true;
            }
            proto::ShareLine share;
            proto::PushLine  push;
            proto::PoseLine  reflect;
            LLUUID           peer;
            bool             ok = false;
            if (kind == proto::LineKind::Share)
            {
                ok = proto::parseShareLine(text, share);
                peer.set(share.peer);
            }
            else if (kind == proto::LineKind::Push)
            {
                ok = proto::parsePushLine(text, push);
                peer.set(push.peer);
            }
            else
            {
                ok = proto::parseReflectLine(text, reflect);
                peer.set(reflect.peer);
            }
            if (!ok)
            {
                LL_DEBUGS("PlusLSLBridge") << "A sharing line of the bridge dropped: form" << LL_ENDL;
                return true;
            }
            if (peer == gAgentID)
            {
                LL_DEBUGS("PlusLSLBridge") << "A sharing line of the bridge dropped: self" << LL_ENDL;
                return true;
            }
            if (b.state == State::Off)
            {
                return true;
            }
            // Copies: a handler may set another or call back in.
            if (kind == proto::LineKind::Share)
            {
                const ShareHandler handler = b.shareHandler;
                if (handler)
                {
                    handler(peer, share.aud, share.seq, share.payload);
                }
            }
            else if (kind == proto::LineKind::Push)
            {
                const PushHandler handler = b.pushHandler;
                if (handler)
                {
                    handler(peer, push.seq, push.payload);
                }
            }
            else
            {
                const PoseHandler handler = b.reflectHandler;
                if (handler)
                {
                    handler(LLUUID(reflect.nonce), static_cast<PoseAction>(reflect.action), peer);
                }
            }
            return true;
        }
    }

    bool handleOwnerSay(const LLChat& chat, const std::string& text, const LLUUID& from_id, const LLUUID& owner_id,
                        LLViewerObject* chatter)
    {
        if (CHAT_TYPE_OWNER != chat.mChatType || CHAT_SOURCE_OBJECT != chat.mSourceType || owner_id != gAgentID)
        {
            return false;
        }
        const proto::LineKind kind = proto::kindOf(text);
        if (kind == proto::LineKind::None)
        {
            return false;
        }
        const bool from_bridge = chatter && CHAT_AUDIBLE_FULLY == chat.mAudible && isOurBridge(chatter);
        if (kind == proto::LineKind::Pose)
        {
            // A pose line of the worn bridge that is not taken is dropped unread: it names a peer and a request (never
            // shown, logged or translated).
            if (!from_bridge)
            {
                if (from_id.notNull() && from_id == bridge().cleanOf)
                {
                    // The bridge by its id (the one whose contents were seen clean), its object not seen here or not
                    // confirmed as the worn bridge (not fully audible, not worn, another item).
                    LL_INFOS("PlusLSLBridge") << "A pose line of the bridge dropped: speaker" << LL_ENDL;
                    return true;
                }
                return false; // another object's line in this form: shown as any line of it
            }
            proto::PoseLine line;
            if (const char* refusal = poseLineRefusal(chatter, text, line))
            {
                LL_INFOS("PlusLSLBridge") << "A pose line of the bridge dropped: " << refusal << LL_ENDL;
                return true;
            }
            const LLUUID nonce(line.nonce);
            const LLUUID peer(line.peer);
            Bridge& b = bridge();
            if (b.state != State::Off && b.poseHandler)
            {
                b.poseHandler(nonce, static_cast<PoseAction>(line.action), peer);
            }
            return true;
        }

        if (kind == proto::LineKind::Share || kind == proto::LineKind::Push || kind == proto::LineKind::Reflect)
        {
            return handleSharingLine(kind, text, chatter, from_bridge);
        }

        // Our script's own lines carry the URL and the token: never shown, logged or translated, whoever said them.
        if (!from_bridge)
        {
            LL_INFOS("PlusLSLBridge") << "A bridge line of an object that is not the worn bridge: dropped unread" << LL_ENDL;
            return true;
        }
        if (kind == proto::LineKind::NoUrl)
        {
            std::string version;
            if (proto::parseNoUrl(text, version))
            {
                onNoUrl();
            }
            return true;
        }
        if (kind == proto::LineKind::OtherBridge)
        {
            LL_INFOS("PlusLSLBridge") << "Bridge said a line of no known kind: dropped" << LL_ENDL;
            return true;
        }
        proto::Announcement           announcement;
        const proto::AnnouncementCheck check = proto::parseAnnouncement(text, announcement);
        if (check != proto::AnnouncementCheck::Ok)
        {
            // Which field and the line's shape, never a value: the URL's id and the token are secrets.
            LL_WARNS("PlusLSLBridge") << "Bridge announcement not taken: its " << checkName(check) << " ("
                                      << proto::describeAnnouncement(text) << ")" << LL_ENDL;
            onRefusedAnnouncement(check);
            return true;
        }
        onAnnouncement(announcement);
        return true;
    }
}

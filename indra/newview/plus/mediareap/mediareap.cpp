/**
 * @file mediareap.cpp
 * @brief Media plugins nobody uses are shut down early (plus/mediareap)
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

// Why: the official media code keeps a plugin process (SLPlugin.exe, and for web pages its
// dullahan_host helpers: about 0.6 GB of commit each) for every media impl whose priority is
// PRIORITY_HIDDEN. Only PRIORITY_UNLOADED shuts one down. LLViewerMedia::updateMedia() gives
// PRIORITY_UNLOADED to in-world media only past PluginInstancesTotal (8), counting in-world media
// only; the browsers of the UI are not counted at all, and the login preloads four of them
// (destinations, avatar_welcome_pack, search, marketplace: LLViewerWindow::initWorldUI()) that stay
// loaded, hidden, for the whole session. A session of teleports ended with 13 plugins (8 in-world
// kept while hidden + the UI ones) and ran out of commit (media-reap, 2026-11-01).
//
// What: once a second, after the official priorities are set, this gives PRIORITY_UNLOADED to:
//  - in-world media that is not drawn (official priority HIDDEN: no face of it recently visible)
//    and out of earshot: farther than max(PlusMediaReapDistance, MediaRollOffMax) (80 m by
//    default, where media sound fades out) from both the avatar and the ear (the camera, or the
//    avatar: MediaSoundsEarLocation), for PlusMediaReapSeconds (30 s); after a teleport or a
//    region change PlusMediaReapTeleportSeconds (10 s) is enough;
//  - a UI browser whose floater is closed (or hidden) for PlusMediaReapUISeconds (60 s), once the
//    login is done. Not judged while floaters are hidden as a group (mouselook, View > Hide UI,
//    the photo screen): an open floater looks closed then;
//  - while more plugins run than PluginInstancesTotal, any of the above after 3 s, lowest
//    priority first.
// Never: media with focus or its controls shown, the parcel media, HUD media, media whose impl has
// no plugin, media loaded again less than 10 s ago, anything while the window is minimized or a
// teleport is in progress. PlusMediaReap switches it (on by default).
//
// Coming back: an unloaded impl is the official "over the limit" state, and the hook in
// updateMedia() (priorityFor()) keeps it there while the official priority is HIDDEN, so a navigate
// (scripted media changing its URL every few seconds) only records the URL ("NOT LOADING"). When
// it is drawn again (in-world), its floater is shown again (UI) or it gets focus, it gets the
// official priority and LLViewerMediaImpl::update() creates the plugin and navigates to its URL
// again (the page reloads; time-based media resumes from the time setPriority() saved). When the
// look finds it is no candidate any more (back in earshot, its floater open), it is let go too:
// the official rules apply to it again.

#include "llviewerprecompiledheaders.h"

#include "plus.h"
#include "plus/mediareap/mediareap.h"
#include "plus/photoscreen/photoscreen.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llapp.h"
#include "llcallbacklist.h"
#include "llfloater.h"
#include "llframetimer.h"
#include "llmediactrl.h"
#include "llmultifloater.h"
#include "llstartup.h"
#include "llviewercontrol.h"
#include "llviewermedia.h"
#include "llviewermediafocus.h"
#include "llviewerregion.h"
#include "llviewerwindow.h"
#include "llvovolume.h"

#include <cmath>
#include <iterator>
#include <sstream>
#include <unordered_map>

namespace plus::mediareap
{
    namespace
    {
        constexpr F64 LOOK_SECONDS = 1.0;           // how often the media list is looked at
        constexpr F64 OVER_LIMIT_SECONDS = 3.0;     // the wait while more plugins run than the limit
        constexpr F64 RELOADED_HOLD_SECONDS = 10.0; // loaded again this recently: not shut down yet
        constexpr F64 JUMP_METERS = 64.0;           // the avatar moved this far between looks: a teleport

        // What the rules say about an impl, whether its plugin runs or not.
        enum class Rule
        {
            KEEP,       // wanted (focus, parcel, HUD, in earshot, floater open): official rules
            UNKNOWN,    // cannot tell now (UI while floaters are hidden as a group): leave as it is
            IN_WORLD,   // not drawn and out of earshot: may be shut down
            UI,         // closed UI browser: may be shut down
        };

        struct Entry
        {
            F64 since = 0.0;    // when it became a candidate (continuously since)
            U32 look = 0;       // the look that last saw it as a candidate
        };

        struct State
        {
            std::unordered_map<LLUUID, Entry> entries;  // candidates, by media texture id
            std::unordered_map<LLUUID, U32> reaped;     // shut down, kept unloaded: the look that last saw it
            std::unordered_map<LLUUID, F64> released;   // let go (it may load again): when
            U32 look = 0;
            F64 movedAt = -1.0;                         // the last teleport / region change
            U64 regionHandle = 0;
            LLVector3d lastPos;
            bool havePos = false;
        };

        // The waits and the distance, from the settings (clamped).
        struct Limits
        {
            F64 waitInWorld;    // in-world media not drawn and out of earshot
            F64 waitMoved;      // ... left behind by a teleport or a region change
            F64 waitUI;         // a closed UI browser
            F64 farM2;          // out of earshot beyond this distance, squared
            // Plugins of any kind (UI and in-world) running at once before the 3 s wait applies.
            // PluginInstancesTotal as it is: the official limit (LLViewerMedia::setMaxInstances())
            // counts in-world media only and is 2 less with under 8 GB of memory; this one counts
            // every running plugin, since each costs the same memory.
            S32 maxRunning;
        };

        // What a look needs to judge each impl, read once per look.
        struct Context
        {
            LLUUID controlsId;      // media whose controls are shown (hover or focus)
            bool uiUnknown;         // floaters hidden as a group: an open one looks closed
            LLVector3d agentPos;
            LLVector3d earPos;      // where media sound is heard from (MediaSoundsEarLocation)
            F64 farM2;
        };

        State& state()
        {
            static State s;
            return s;
        }

        bool switchedOn()
        {
            static LLCachedControl<bool> on(gSavedSettings, "PlusMediaReap", true);
            return on;
        }

        Limits readLimits()
        {
            static LLCachedControl<F32> reap_seconds(gSavedSettings, "PlusMediaReapSeconds", 30.f);
            static LLCachedControl<F32> reap_distance(gSavedSettings, "PlusMediaReapDistance", 80.f);
            static LLCachedControl<F32> rolloff_max(gSavedSettings, "MediaRollOffMax", 80.f);
            static LLCachedControl<F32> teleport_seconds(gSavedSettings, "PlusMediaReapTeleportSeconds", 10.f);
            static LLCachedControl<F32> ui_seconds(gSavedSettings, "PlusMediaReapUISeconds", 60.f);
            static LLCachedControl<U32> max_total(gSavedSettings, "PluginInstancesTotal", 8);

            Limits limits;
            limits.waitInWorld = llclamp((F64)reap_seconds(), 2.0, 3600.0);
            limits.waitMoved = llclamp((F64)teleport_seconds(), 2.0, limits.waitInWorld);
            limits.waitUI = llclamp((F64)ui_seconds(), 2.0, 3600.0);
            // Never closer than where media sound fades out, whatever the setting says.
            const F64 far_m = llclamp(llmax((F64)reap_distance(), (F64)rolloff_max()), 0.0, 1024.0);
            limits.farM2 = far_m * far_m;
            limits.maxRunning = (S32)llclamp(max_total(), 1U, 1000U);
            return limits;
        }

        Context readContext(const Limits& limits)
        {
            static LLCachedControl<S32> ear_location(gSavedSettings, "MediaSoundsEarLocation", 0);

            Context ctx;
            ctx.controlsId = LLViewerMediaFocus::instanceExists()
                ? LLViewerMediaFocus::getInstance()->getControlsMediaID() : LLUUID::null;
            ctx.uiUnknown = gAgentCamera.cameraMouselook()
                || gAgentCamera.getCameraMode() == CAMERA_MODE_MOUSELOOK
                || !gViewerWindow->getUIVisibility()
                || plus::photoscreen::isActive();
            ctx.agentPos = gAgent.getPositionGlobal();
            // As LLViewerMediaImpl::calculateInterest(): 1 is the avatar, anything else the camera.
            ctx.earPos = (ear_location() == 1) ? ctx.agentPos : gAgentCamera.getCameraPositionGlobal();
            ctx.farM2 = limits.farM2;
            return ctx;
        }

        // Drops the entries the look just made did not see.
        template <typename Map, typename LookOf>
        void eraseUnseen(Map& map, U32 look, LookOf look_of)
        {
            for (auto it = map.begin(); it != map.end();)
            {
                it = (look_of(it->second) == look) ? std::next(it) : map.erase(it);
            }
        }

        void forget()
        {
            State& s = state();
            s.entries.clear();
            s.havePos = false;
        }

        // A teleport, a region change, or a jump of the avatar since the last look.
        void notePlace(F64 now)
        {
            State& s = state();
            const LLViewerRegion* region = gAgent.getRegion();
            const U64 handle = region ? region->getHandle() : 0;
            const LLVector3d pos = gAgent.getPositionGlobal();
            if (s.havePos && (handle != s.regionHandle || (pos - s.lastPos).magVec() > JUMP_METERS))
            {
                s.movedAt = now;
            }
            s.regionHandle = handle;
            s.lastPos = pos;
            s.havePos = true;
        }

        // A shut-down impl may load again: the official rules apply to it from now on.
        void release(State& s, std::unordered_map<LLUUID, U32>::iterator found, F64 now)
        {
            s.released[found->first] = now;
            s.reaped.erase(found);
        }

        // The UI browser's floater (or the control itself when it has none) is hidden.
        bool uiClosed(LLViewerMediaImpl* impl)
        {
            auto ctrl = LLMediaCtrl::getInstance(impl->getMediaTextureID());
            if (!ctrl)
            {
                return false;   // a UI impl without a control: not ours to judge
            }
            if (LLFloater* floater = ctrl->getParentByType<LLFloater>())
            {
                // The floater's own flag: a minimized floater stays visible (its page may play
                // sound). Floaters hidden as a group (mouselook, Hide UI, the photo screen) lose
                // it too, so the caller does not ask then (Context::uiUnknown). A floater in a tab of
                // a host (conversations, the profiles' window) is hidden while another tab is chosen:
                // it is open while its host is (else every change of tab loads the page again).
                if (LLFloater* host = floater->getHost())
                {
                    return !host->getVisible();
                }
                return !floater->getVisible();
            }
            return !ctrl->isInVisibleChain();
        }

        // The nearer of the avatar and the ear to the media's object, squared. -1: no object.
        F64 nearestM2(LLViewerMediaImpl* impl, const Context& ctx)
        {
            LLVOVolume* object = impl->getSomeObject();
            if (!object)
            {
                return -1.0;
            }
            const LLVector3d pos = object->getPositionGlobal();
            return llmin((ctx.agentPos - pos).magVecSquared(), (ctx.earPos - pos).magVecSquared());
        }

        // What the rules say about the impl, whether its plugin runs or not.
        Rule ruleFor(LLViewerMediaImpl* impl, const Context& ctx)
        {
            if (impl->hasFocus() || impl->isParcelMedia())
            {
                return Rule::KEEP;
            }
            const LLUUID id = impl->getMediaTextureID();
            if (id.isNull() || id == ctx.controlsId)
            {
                return Rule::KEEP;
            }
            if (impl->getUsedInUI())
            {
                if (ctx.uiUnknown)
                {
                    return Rule::UNKNOWN;
                }
                return uiClosed(impl) ? Rule::UI : Rule::KEEP;
            }
            if (impl->isAttachedToHUD())
            {
                return Rule::KEEP;
            }
            // No object any more: nothing can show it.
            const F64 near_m2 = nearestM2(impl, ctx);
            return (near_m2 >= 0.0 && near_m2 < ctx.farM2) ? Rule::KEEP : Rule::IN_WORLD;
        }

        void logUnload(LLViewerMediaImpl* impl, Rule rule, const Context& ctx, F64 unused_seconds, bool over_limit)
        {
            std::ostringstream line;
            line << "unloaded the plugin of " << (rule == Rule::UI ? "a closed UI browser " : "in-world media ")
                 << impl->getMediaTextureID() << " (unused for " << S32(unused_seconds) << " s";
            if (rule == Rule::IN_WORLD)
            {
                const F64 near_m2 = nearestM2(impl, ctx);
                if (near_m2 >= 0.0)
                {
                    line << ", " << S32(std::sqrt(near_m2)) << " m away";
                }
                else
                {
                    line << ", its object is gone";
                }
            }
            if (over_limit)
            {
                line << ", over PluginInstancesTotal";
            }
            line << ")";
            LL_INFOS("MediaReap") << line.str() << LL_ENDL;
        }

        // When a candidate's plugin is shut down.
        F64 deadlineFor(Rule rule, const LLUUID& id, const Entry& entry, const Limits& limits, bool over_limit)
        {
            const State& s = state();
            F64 deadline = entry.since + (rule == Rule::UI ? limits.waitUI : limits.waitInWorld);
            if (rule == Rule::IN_WORLD && s.movedAt >= 0.0 && entry.since <= s.movedAt + limits.waitMoved)
            {
                // Left behind by a teleport or a region change.
                deadline = llmin(deadline, llmax(entry.since, s.movedAt) + limits.waitMoved);
            }
            if (over_limit)
            {
                deadline = llmin(deadline, entry.since + OVER_LIMIT_SECONDS);
            }
            // Loaded again a moment ago: no shutting down and starting over every few seconds.
            auto released = s.released.find(id);
            if (released != s.released.end())
            {
                deadline = llmax(deadline, released->second + RELOADED_HOLD_SECONDS);
            }
            return deadline;
        }

        S32 countRunning(const LLViewerMedia::impl_list& list)
        {
            S32 running = 0;
            for (LLViewerMediaImpl* impl : list)
            {
                running += impl->hasMedia() ? 1 : 0;
            }
            return running;
        }

        void look(F64 now)
        {
            const Limits limits = readLimits();
            const Context ctx = readContext(limits);

            State& s = state();
            notePlace(now);
            ++s.look;

            for (auto it = s.released.begin(); it != s.released.end();)
            {
                it = (now - it->second >= RELOADED_HOLD_SECONDS) ? s.released.erase(it) : std::next(it);
            }

            LLViewerMedia::impl_list& list = LLViewerMedia::getInstance()->getPriorityList();
            const S32 running = countRunning(list);
            S32 excess = running - limits.maxRunning;

            // The list is sorted by priority (LLViewerMedia::priorityComparitor): from the back, the
            // least wanted first, so the excess goes from there.
            S32 unloaded_in_world = 0;
            S32 unloaded_ui = 0;
            for (auto it = list.rbegin(); it != list.rend(); ++it)
            {
                LLViewerMediaImpl* impl = *it;
                const LLUUID id = impl->getMediaTextureID();
                const Rule rule = ruleFor(impl, ctx);

                auto reaped = s.reaped.find(id);
                if (reaped != s.reaped.end())
                {
                    if (rule == Rule::KEEP)
                    {
                        release(s, reaped, now);    // back in earshot, its floater open, focused
                    }
                    else
                    {
                        reaped->second = s.look;
                    }
                    continue;
                }

                if (rule != Rule::IN_WORLD && rule != Rule::UI)
                {
                    continue;
                }
                if (!impl->hasMedia() || impl->getPriority() != LLPluginClassMedia::PRIORITY_HIDDEN)
                {
                    continue;   // no plugin to shut down, or drawn / shown
                }

                // Entries left from the last look were candidates then (the others are erased below).
                auto [found, is_new] = s.entries.try_emplace(id);
                Entry& entry = found->second;
                if (is_new)
                {
                    entry.since = now;
                }
                entry.look = s.look;

                if (now < deadlineFor(rule, id, entry, limits, excess > 0))
                {
                    continue;
                }

                logUnload(impl, rule, ctx, now - entry.since, excess > 0);
                s.reaped[id] = s.look;
                impl->setPriority(LLPluginClassMedia::PRIORITY_UNLOADED);
                s.entries.erase(found);
                --excess;
                if (rule == Rule::UI)
                {
                    ++unloaded_ui;
                }
                else
                {
                    ++unloaded_in_world;
                }
            }

            // Entries not seen at this look are no candidates any more, and shut-down impls not seen
            // are gone (their object died, their floater was destroyed).
            eraseUnseen(s.entries, s.look, [](const Entry& entry) { return entry.look; });
            eraseUnseen(s.reaped, s.look, [](U32 look) { return look; });

            if (unloaded_in_world + unloaded_ui > 0)
            {
                LL_INFOS("MediaReap") << "unloaded " << (unloaded_in_world + unloaded_ui) << " media plugins ("
                                      << unloaded_in_world << " in-world, " << unloaded_ui << " UI); "
                                      << (running - unloaded_in_world - unloaded_ui) << " still running" << LL_ENDL;
            }
        }

        // The idle loop, every frame: a setting and a clock compare until the next look is due.
        void idle(void*)
        {
            static F64 next_look = 0.0;
            if (!switchedOn())
            {
                if (!state().entries.empty())
                {
                    forget();
                }
                return;
            }
            const F64 now = LLFrameTimer::getElapsedSeconds();
            if (now < next_look)
            {
                return;
            }
            next_look = now + LOOK_SECONDS;

            if (LLApp::isExiting() || LLStartUp::getStartupState() < STATE_STARTED || !LLViewerMedia::instanceExists())
            {
                return;
            }
            if (!gViewerWindow || !gViewerWindow->getActive())
            {
                // Minimized: nothing is drawn, so everything looks unused. Start over after.
                forget();
                return;
            }
            if (gAgent.getTeleportState() != LLAgent::TELEPORT_NONE)
            {
                return; // the place is changing: wait (the waits keep their start)
            }
            look(now);
        }

        void init()
        {
            // Settings: app_settings/settings_plus.xml (PlusMediaReap*). The idle callback chooses from
            // the priorities LLViewerMedia::updateMedia() set; the hook there (priorityFor(),
            // llviewermedia.cpp) keeps what it shut down unloaded.
            gIdleCallbacks.addFunction(&idle);
        }

        plus::FeatureRegistration sRegistration({ "mediareap", &init });
    }

    LLPluginClassMedia::EPriority priorityFor(LLViewerMediaImpl* impl, LLPluginClassMedia::EPriority priority)
    {
        State& s = state();
        if (s.reaped.empty() || priority == LLPluginClassMedia::PRIORITY_UNLOADED)
        {
            return priority;
        }
        if (!switchedOn())
        {
            s.reaped.clear();   // switched off: the official priorities from now on
            return priority;
        }
        auto found = s.reaped.find(impl->getMediaTextureID());
        if (found == s.reaped.end())
        {
            return priority;
        }
        if (priority > LLPluginClassMedia::PRIORITY_HIDDEN || impl->hasFocus())
        {
            // Drawn, shown or focused again: LLViewerMediaImpl::update() loads it.
            release(s, found, LLFrameTimer::getElapsedSeconds());
            return priority;
        }
        return LLPluginClassMedia::PRIORITY_UNLOADED;
    }
}

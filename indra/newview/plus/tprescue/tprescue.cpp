/**
 * @file tprescue.cpp
 * @brief Teleport rescue: no disconnect while the old region goes before the
 *        new one has answered, and the new one asked again.
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

// On by default; LLRHI_TP_RESCUE=0 leaves teleports to upstream's ways,
// LLRHI_TP_RESCUE=test also plays a slow new region (see below).
//
// research/43 (R1). By TeleportFinish the simulators have already handed the
// agent over: the old region cannot take it back. Until the new region's
// AgentMovementComplete, though, the viewer's agent is still in the old
// region (gAgent.getRegion()), and three checks watch that region:
// - LLWorld::removeRegion(): the old region's DisableSimulator removes the
//   agent's region -> "You have been disconnected"
// - LLAppViewer::idleNetwork(): the old region's CloseCircuit kills the
//   agent's region's circuit -> "lost connection"
// - the event poll: the old region's poll fails on -> "lost connection"
// So a new region that is only slow loses the session when the old one lets
// go first. And the viewer asks the new region once: UseCircuitCode with the
// default three retries (at login it waits far longer), CompleteAgentMovement
// never again.
//
// With it, from TeleportFinish until the agent is in the new region:
// - UseCircuitCode goes as patiently as at login.
// - The old region going (disabled, or its circuit or event poll lost) is no
//   reason to disconnect: the region is kept (the agent points at it) and
//   removed once the agent has arrived, unless it was added again since on
//   its host (EnableSimulator from the new region: it is a neighbour now).
//   Added again on another host: the wait ends (upstream's disconnect).
// - TeleportFinish again from the same old region keeps what is known of it.
// - No arrival for RESEND_SECONDS: UseCircuitCode and CompleteAgentMovement go
//   to the new region again, up to MAX_RESENDS times.
// - Once the old region has gone, the viewer disconnects (as upstream would
//   have, with the same message) when nothing has come from the new region
//   for DEST_GONE_SECONDS, or at GIVE_UP_SECONDS with no arrival.
// - A logout (or a disconnect) ends the wait: no request again, no hold-off.
// Log (tag TPRescue): what was held off, each request again, the arrival or
// the end.
//
// On the viewer's side the old region's circuit does not tell whether it has
// gone or come back: LLMessageSystem::disableCircuit() (DisableSimulator,
// CloseCircuit) only removes circuits with a circuit code, which the viewer
// never has, so the circuit stays until its ping timeout (100 s). So whether
// the old region stays is decided by what was asked last (disabled, or added
// again), and whether the new region is alive by when it last sent anything.
//
// LLRHI_TP_RESCUE=test: at TeleportFinish the old region lets go first, on the
// viewer's side: it is removed as its DisableSimulator would (kept by the
// rescue) and its circuit dropped as its ping timeout would; the first
// UseCircuitCode is not sent (a lost packet), so the agent arrives through the
// first request again, about RESEND_SECONDS later, and the old region is then
// removed. Teleport far (not to a neighbour of the old region).

#include "llviewerprecompiledheaders.h"

#include "plus.h"

#include "tprescue.h"

#include "llagent.h"
#include "llappviewer.h"
#include "llcallbacklist.h"
#include "llcircuit.h"
#include "llhost.h"
#include "lltimer.h"
#include "lltrans.h"
#include "llviewercontrol.h"
#include "llviewermessage.h"
#include "llviewerregion.h"
#include "llworld.h"
#include "message.h"

#include <cstdlib>
#include <cstring>

namespace
{
    constexpr F64 RESEND_SECONDS = 10.0;     // no arrival this long: ask the new region again
    constexpr S32 MAX_RESENDS = 5;
    constexpr F64 DEST_GONE_SECONDS = 30.0;  // the old region gone, nothing from the new one this long: give up
    constexpr F64 GIVE_UP_SECONDS = 90.0;    // the old region gone, no arrival: give up

    struct State
    {
        bool waiting = false;       // between TeleportFinish and the arrival (or the end)
        U32 generation = 0;         // teleports so far (an older idle loop stops)
        F64 finish_time = 0.0;
        F64 finish_msg_time = 0.0;  // the same, in the message system's time (LLCircuitData's)
        LLHost source_host;
        LLHost dest_host;
        U64 dest_handle = 0;
        bool source_lost = false;   // a check was held off: the old region has gone
        bool remove_source = false; // the old region was disabled or lost since TeleportFinish: to remove after the arrival
        bool source_added = false;  // ... and added again since (a neighbour of the new one): it stays
        S32 resends = 0;
    };
    State sState;

    bool sEnabled = true;
    bool sTest = false;

    F64 now()
    {
        return LLTimer::getTotalSeconds();
    }

    bool disconnecting()
    {
        return gDisconnected || gDoDisconnect || LLApp::isExiting()
               || LLAppViewer::instance()->logoutRequestSent();
    }

    bool circuit_alive(const LLHost& host)
    {
        LLCircuitData* cdp = gMessageSystem ? gMessageSystem->mCircuitInfo.findCircuit(host) : nullptr;
        return cdp && cdp->isAlive();
    }

    // Seconds since anything came from host, counted from the TeleportFinish at
    // the most (a new region sends nothing before it); -1: no living circuit.
    F64 silent_seconds(const LLHost& host)
    {
        LLCircuitData* cdp = gMessageSystem ? gMessageSystem->mCircuitInfo.findCircuit(host) : nullptr;
        if (!cdp || !cdp->isAlive())
        {
            return -1.0;
        }
        const F64 last_in = llmax(cdp->getLastPacketInTime().value(), sState.finish_msg_time);
        return LLMessageSystem::getMessageTimeSeconds().value() - last_in;
    }

    // The wait is over (arrival, give-up, disconnect or logout): nothing kept any more.
    void stop_waiting()
    {
        sState.waiting = false;
        sState.remove_source = false;
        sState.source_added = false;
    }

    // The UseCircuitCode built in msg, as patiently as at login.
    void send_use_circuit_code(LLMessageSystem* msg, const LLHost& host)
    {
        void (*callback)(void**, S32) = [](void**, S32 result)
        {
            if (!LLApp::isExiting() && result != LL_ERR_NOERR)
            {
                LL_WARNS("TPRescue") << "UseCircuitCode to the new region not acknowledged (err=" << result << ")" << LL_ENDL;
            }
        };
        msg->sendReliable(host,
                          gSavedSettings.getS32("UseCircuitCodeMaxRetries"),
                          false,
                          (F32Seconds)gSavedSettings.getF32("UseCircuitCodeTimeout"),
                          callback,
                          nullptr);
    }

    void ask_again()
    {
        ++sState.resends;
        LL_WARNS("TPRescue") << llformat("no arrival %.0f s after TeleportFinish: UseCircuitCode and CompleteAgentMovement "
                                         "to the new region again (%d of %d)",
                                         now() - sState.finish_time, sState.resends, MAX_RESENDS)
                             << " dest=" << sState.dest_host << LL_ENDL;

        LLMessageSystem* msg = gMessageSystem;
        msg->newMessageFast(_PREHASH_UseCircuitCode);
        msg->nextBlockFast(_PREHASH_CircuitCode);
        msg->addU32Fast(_PREHASH_Code, msg->getOurCircuitCode());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        msg->addUUIDFast(_PREHASH_ID, gAgent.getID());
        send_use_circuit_code(msg, sState.dest_host);

        send_complete_agent_movement(sState.dest_host);
    }

    // After the arrival: the old region kept by keepAgentRegion() goes now
    // that the agent is elsewhere, unless it was added again since (a
    // neighbour of the new region). Not by its circuit: see the top.
    void settle_kept_region()
    {
        LLViewerRegion* regionp = LLWorld::getInstance()->getRegion(sState.source_host);
        if (!regionp || regionp == gAgent.getRegion())
        {
            return;
        }
        if (!sState.remove_source)
        {
            if (sState.source_added)
            {
                LL_INFOS("TPRescue") << "the old region " << regionp->getName() << " was added again (a neighbour): kept" << LL_ENDL;
                // It was lost or disabled before (source_added follows that only): its event poll
                // may have stopped (sourceMayGo() only held off the disconnect), and a seed the
                // same as the one it has (the simulator may send it again) makes no new poll
                // (LLViewerRegion::setSeedCapability()). Asked for anew now that it is a
                // neighbour, not the agent's region: its capabilities and a new event poll.
                const std::string seed = regionp->getCapability("Seed");
                if (!seed.empty())
                {
                    regionp->setCapability("Seed", std::string());
                    regionp->setSeedCapability(seed);
                    LL_INFOS("TPRescue") << "its capabilities and event poll asked for anew" << LL_ENDL;
                }
            }
            return;
        }
        const std::string name = regionp->getName();
        LLWorld::getInstance()->removeRegion(sState.source_host);
        LL_INFOS("TPRescue") << "removed the old region " << name << " now that the agent has arrived ("
                             << (LLWorld::getInstance()->getRegion(sState.source_host) ? "STILL THERE" : "gone") << ", "
                             << LLWorld::getInstance()->getRegionList().size() << " region(s) left)" << LL_ENDL;
    }

    // Idle, while waiting. True: done.
    bool tick(U32 generation)
    {
        if (generation != sState.generation || !sState.waiting)
        {
            return true;    // a later teleport has its own loop
        }
        if (disconnecting())
        {
            if (sState.source_lost || sState.resends > 0)
            {
                LL_INFOS("TPRescue") << llformat("logout or disconnect %.1f s after TeleportFinish: stopped waiting", now() - sState.finish_time)
                                     << LL_ENDL;
            }
            stop_waiting();
            return true;
        }

        const F64 since = now() - sState.finish_time;
        LLViewerRegion* agent_region = gAgent.getRegion();
        if (agent_region && agent_region->getHost() != sState.source_host)
        {
            if (sState.source_lost || sState.resends > 0)
            {
                LL_INFOS("TPRescue") << llformat("arrived %.1f s after TeleportFinish in ", since) << agent_region->getName()
                                     << (agent_region->getHandle() == sState.dest_handle ? "" : " (not the region the teleport named)")
                                     << ", asked again " << sState.resends << " time(s), old region gone: "
                                     << (sState.source_lost ? "yes" : "no") << LL_ENDL;
            }
            settle_kept_region();
            stop_waiting();
            return true;
        }

        if (circuit_alive(sState.dest_host) && sState.resends < MAX_RESENDS && since >= RESEND_SECONDS * (sState.resends + 1))
        {
            ask_again();
        }

        if (!sState.source_lost)
        {
            if (since > GIVE_UP_SECONDS)
            {
                // Still in a living old region: whatever happens next is upstream's.
                LL_WARNS("TPRescue") << llformat("no arrival %.0f s after TeleportFinish, the old region still there: stopped", since) << LL_ENDL;
                stop_waiting();
                return true;
            }
            return false;
        }

        const F64 dest_silent = silent_seconds(sState.dest_host);
        const bool dest_gone = dest_silent < 0.0 || dest_silent > DEST_GONE_SECONDS;
        if (dest_gone || since > GIVE_UP_SECONDS)
        {
            // The checks held off do not fire again for this region: disconnect
            // the way upstream would have.
            LL_WARNS("TPRescue") << llformat("giving up %.0f s after TeleportFinish: ", since)
                                 << (!dest_gone ? std::string("no arrival")
                                     : dest_silent < 0.0 ? std::string("the new region's circuit is gone too")
                                     : llformat("nothing from the new region for %.0f s", dest_silent))
                                 << ", asked again " << sState.resends << " time(s)" << LL_ENDL;
            stop_waiting();
            LLAppViewer::instance()->forceDisconnect(LLTrans::getString("AgentLostConnection"));
            return true;
        }
        return false;
    }

    void init()
    {
        const char* env = std::getenv("LLRHI_TP_RESCUE");
        if (env && !strcmp(env, "0"))
        {
            sEnabled = false;
            LL_INFOS("TPRescue") << "off (LLRHI_TP_RESCUE=0)" << LL_ENDL;
            return;
        }
        sTest = env && !strcmp(env, "test");
        if (sTest)
        {
            LL_WARNS("TPRescue") << "test mode (LLRHI_TP_RESCUE=test): every teleport closes the old region's circuit "
                                    "and loses the first UseCircuitCode" << LL_ENDL;
        }
    }

    plus::FeatureRegistration sRegistration({ "tprescue", &init });
}

namespace plus::tprescue
{
    void onTeleportFinish(LLMessageSystem* msg, const LLHost& dest_host, U64 dest_handle)
    {
        LLViewerRegion* source = gAgent.getRegion();
        if (!sEnabled || !source || source->getHost() == dest_host)
        {
            if (sState.waiting)
            {
                // A teleport back into the region still waited out of: the arrival
                // waited for will not come.
                LL_INFOS("TPRescue") << "a teleport into the old region while waiting: stopped waiting" << LL_ENDL;
                ++sState.generation;
                stop_waiting();
            }
            msg->sendReliable(dest_host);   // upstream
            return;
        }

        // Another TeleportFinish out of the region still waited out of: what is
        // known of the old region stands (a lost region's checks do not fire
        // again). The same teleport once more (the same new region): the wait
        // goes on as it is, only the UseCircuitCode goes (as upstream's would).
        const bool same_source = sState.waiting && sState.source_host == source->getHost();
        if (same_source && sState.dest_host == dest_host)
        {
            LL_INFOS("TPRescue") << llformat("TeleportFinish again %.1f s after the first: still waiting", now() - sState.finish_time)
                                 << LL_ENDL;
            send_use_circuit_code(msg, dest_host);
            return;
        }

        sState.waiting = true;
        const U32 generation = ++sState.generation;
        sState.finish_time = now();
        sState.finish_msg_time = LLMessageSystem::getMessageTimeSeconds().value();
        sState.source_host = source->getHost();
        sState.dest_host = dest_host;
        sState.dest_handle = dest_handle;
        if (!same_source)
        {
            sState.source_lost = false;
            sState.remove_source = false;
            sState.source_added = false;
        }
        sState.resends = 0;

        if (sTest)
        {
            msg->clearMessage();
            LL_WARNS("TPRescue") << "test: the first UseCircuitCode not sent; the old region disabled and its circuit dropped "
                                    "on the viewer's side" << LL_ENDL;
            // What the old region's DisableSimulator does (process_disable_simulator(); keepAgentRegion() keeps it) ...
            LLWorld::getInstance()->removeRegion(sState.source_host);
            // ... and its circuit's ping timeout when it no longer answers (the agent region's circuit check: sourceMayGo())
            gMessageSystem->mCircuitInfo.removeCircuitData(sState.source_host);
        }
        else
        {
            send_use_circuit_code(msg, dest_host);
        }

        doOnIdleRepeating([generation]() { return tick(generation); });
    }

    bool keepAgentRegion(LLViewerRegion* regionp)
    {
        if (!sState.waiting || disconnecting()
            || !regionp || regionp->getHost() != sState.source_host)
        {
            return false;
        }
        // Again after it was added again (disabled, added, disabled): the last word counts.
        const bool again = sState.remove_source || sState.source_added;
        sState.remove_source = true;
        sState.source_added = false;
        sState.source_lost = true;
        LL_WARNS("TPRescue") << llformat("the old region %s was disabled%s %.1f s after TeleportFinish, before the arrival: "
                                         "kept until the agent is in the new region, no disconnect",
                                         regionp->getName().c_str(), again ? " again" : "", now() - sState.finish_time)
                             << LL_ENDL;
        return true;
    }

    bool regionAddedAgain(LLViewerRegion* regionp, const LLHost& host)
    {
        if (!sState.waiting || disconnecting() || !regionp || regionp->getHost() != sState.source_host)
        {
            return false;
        }
        // Not the agent's any more: arrived, the idle loop still to come
        // (settle_kept_region()). Upstream's way, but no longer to remove.
        const bool agents = regionp == gAgent.getRegion();
        if (host != sState.source_host)
        {
            if (!agents)
            {
                return false;
            }
            // The old region's place on another host: addRegion() removes the
            // agent's region, which upstream answers with a disconnect. Kept, it
            // would stay beside the new one (two regions with one handle).
            LL_WARNS("TPRescue") << llformat("the old region %s added again on another host %.1f s after TeleportFinish: "
                                             "stopped waiting (upstream's disconnect)",
                                             regionp->getName().c_str(), now() - sState.finish_time)
                                 << " host=" << host << LL_ENDL;
            ++sState.generation;
            stop_waiting();
            return false;
        }
        // The same host: returned as it is, alive or not yet (removing it would
        // only keep it, and addRegion() would put a second one beside it).
        if (!sState.source_lost)
        {
            return agents;  // never gone: as upstream answers for a living region
        }
        if (sState.remove_source)
        {
            LL_WARNS("TPRescue") << llformat("the old region %s was added again %.1f s after TeleportFinish "
                                             "(a neighbour of the new one): it stays after the arrival",
                                             regionp->getName().c_str(), now() - sState.finish_time)
                                 << LL_ENDL;
        }
        sState.remove_source = false;
        sState.source_added = true;
        return agents;
    }

    bool sourceMayGo(const LLHost& host)
    {
        if (!sState.waiting || disconnecting() || host != sState.source_host)
        {
            return false;
        }
        if (sState.source_added)
        {
            // Added again since: a check of the region from before (its old
            // event poll giving up) says nothing of it now.
            sState.source_lost = true;
        }
        else if (!sState.remove_source)
        {
            // Lost (its circuit or its event poll): no longer of use, removed
            // after the arrival unless added again.
            sState.source_lost = true;
            sState.remove_source = true;
            sState.source_added = false;
            LL_WARNS("TPRescue") << llformat("the old region's connection lost %.1f s after TeleportFinish, before the arrival: "
                                             "no disconnect while the new region is waited for",
                                             now() - sState.finish_time)
                                 << " source=" << host << LL_ENDL;
        }
        return true;
    }
}

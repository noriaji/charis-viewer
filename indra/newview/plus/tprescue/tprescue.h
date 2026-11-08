/**
 * @file tprescue.h
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

#ifndef PLUS_TPRESCUE_H
#define PLUS_TPRESCUE_H

#include "stdtypes.h"

class LLHost;
class LLMessageSystem;
class LLViewerRegion;

namespace plus::tprescue
{
    // The <plus> hook in process_teleport_finish(), in place of sending the
    // UseCircuitCode built there: notes the old and the new region (the agent
    // is still in the old one) and sends it as patiently as at login
    // (UseCircuitCodeMaxRetries, UseCircuitCodeTimeout). Main thread.
    void onTeleportFinish(LLMessageSystem* msg, const LLHost& dest_host, U64 dest_handle);

    // The <plus> hook in LLWorld::removeRegion(), where the region to remove is
    // the agent's: true when it is the old region of a teleport still waiting
    // for the new one. The region is then kept (the agent still points at it)
    // and removed once the agent has arrived; no disconnect. Main thread.
    bool keepAgentRegion(LLViewerRegion* regionp);

    // The <plus> hook in LLWorld::addRegion(), where a region with the handle to
    // add is there already (regionp; host: the host to add). While a teleport
    // waits and regionp is its old region (the agent's): on the same host
    // (EnableSimulator again: a neighbour of the new region) true, and
    // addRegion() returns it as it is (alive or not: removing it would only
    // keep it beside a second one); it is then not removed after the arrival.
    // On another host the wait ends and false: upstream's removal, a
    // disconnect. Just after the arrival (no longer the agent's region, the
    // wait not yet settled) false, but on its host it is no longer removed.
    // Otherwise false. Main thread.
    bool regionAddedAgain(LLViewerRegion* regionp, const LLHost& host);

    // The <plus> hooks in LLAppViewer::idleNetwork() (the agent's region's
    // circuit has died) and in the event poll (the agent's region's poll has
    // given up): true when host is the old region of a teleport still waiting
    // for the new one, and the loss is no reason to disconnect. The region is
    // then removed after the arrival, as a disabled one. Main thread.
    bool sourceMayGo(const LLHost& host);
}

#endif // PLUS_TPRESCUE_H

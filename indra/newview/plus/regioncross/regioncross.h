/**
 * @file regioncross.h
 * @brief Region crossing timeline: when each step of a crossing happens (log only).
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

#ifndef PLUS_REGIONCROSS_H
#define PLUS_REGIONCROSS_H

#include "stdtypes.h"

class LLHost;

// Region crossing timeline (research/145, research/218). Every step from the
// CrossedRegion message to the agent's arrival (messages, circuits, seed and
// capabilities, handshake), with its time since the crossing began, and the
// stalled frames after it, is written as one "crossing timeline" line to the
// log (LL_INFOS("RegionCross")) when the crossing has settled. It tells how much
// of a slow crossing is the simulator's hand-over and how much is the viewer's
// own work. Log only: nothing in the viewer's behaviour changes.
//
// The neighbour regions' own links (EnableSimulator, EstablishAgentCommunication,
// DisableSimulator, capabilities) that came shortly before are listed with a
// negative time, so a crossing into a neighbour that was not ready shows.
// Between crossings the hooks only keep the last few of those; the per-frame
// callback runs only while a crossing is being recorded. Main thread.
namespace plus::regioncross
{
    // CrossedRegion received (process_crossed_region()): a timeline begins.
    void begin(U64 region_handle);

    // A step of the crossing (or a neighbour's link before it). region_handle 0: none.
    void step(const char* what, U64 region_handle = 0);

    // The same for a region known by its simulator's address (DisableSimulator).
    void stepHost(const char* what, const LLHost& host);

    // AgentMovementComplete received: the agent moves again. The timeline is
    // written a few seconds later, after the frames that follow have been
    // counted. Ignored when no crossing is recorded (a teleport, the login).
    void arrived(U64 region_handle);
}

#endif // PLUS_REGIONCROSS_H

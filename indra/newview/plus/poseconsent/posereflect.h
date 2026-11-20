/**
 * @file posereflect.h
 * @brief Stage 2 of the consent to pose another avatar (research/374b): the person posed (T) allows the person who
 *        poses them (P) to have the pose P sets shown on T's own screen, and, by T's own sharing setting, on the
 *        screens of the Charis Viewers near T. Stage 1 (poseconsent.h, Aurora's: P's own screen only) comes first: P may
 *        ask for stage 2 only of someone who allowed stage 1, and T takes a request only from someone it allowed. Stage 2 is
 *        for friends only and ends when the two stop being friends. T chooses in the confirmation whether the pose may
 *        also be shown to those near T (mayShowAround()) or stays on T's own screen.
 *
 * The ledger is the one of poseconsent (poseconsentcore.h), a second instance on its own wire: "reflect1|<nonce>|
 * <action>|<target>" through the bridge's REFLECT channel (lslbridge::sendReflect), which another viewer's or Aurora's
 * bridge never hears. Same rules as stage 1: an answer within 60 s, 30 s of cooldown after a refusal, 64 sessions, an
 * end at a teleport, a region change, a logout, a peer who leaves or is muted. T always sees a confirmation (there is
 * no automatic yes).
 *
 * Main thread only. The viewer's side of the poses themselves (the pushes P sends, the pose T takes) is
 * plus/poseshare (poseshare_send.cpp sends, poseshare_pushed.cpp takes).
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

#ifndef PLUS_POSEREFLECT_H
#define PLUS_POSEREFLECT_H

#include "poseconsent.h"

#include "lluuid.h"

#include <vector>

namespace plus::poseconsent::reflect
{
    // ---- the person who poses (P)
    Status status(const LLUUID& peer);
    // Stage 1 allowed (and usable), a friend, the bridge runs, the person near and not muted, nothing asked or cooling
    // down.
    bool canAsk(const LLUUID& peer);
    bool ask(const LLUUID& peer);
    // Pending: takes the request back. Accepted: ends it (the person is told).
    void cancel(const LLUUID& peer);
    // Accepted and near, with stage 1 usable: the pose set on `peer` may be pushed to them.
    bool canPush(const LLUUID& peer);
    // Everyone canPush() holds for, in `out` (cleared first).
    void pushTargets(std::vector<LLUUID>& out);

    // ---- the person posed (T)
    // We allowed `peer` to have their poses of us shown on our screen, stage 1 is still allowed, `peer` is still a
    // friend, and nothing is withdrawn: the push of a pose is taken only while this holds.
    bool mayReflect(const LLUUID& peer);
    // And we chose, in the confirmation, that the pose they set may also be shown to the Charis Viewers near us when
    // we share our pose (the other choice: on our own screen only).
    bool mayShowAround(const LLUUID& peer);
    // Withdraws what we allowed `peer` (their requests in too): their poses stop showing on our screen; what we allowed
    // in stage 1 stays (the reminder window's "Stop Reflect"; withdrawing stage 1 ends both).
    void withdraw(const LLUUID& peer);
    // Bumped whenever the people we allowed change (the reminder window redraws then).
    unsigned grantedGeneration();
    // A tip: the pose `peer` sets now shows on our avatar (called when the first push of a run is taken, with the way
    // to stop it).
    void tellPushStarted(const LLUUID& peer);
}

#endif // PLUS_POSEREFLECT_H

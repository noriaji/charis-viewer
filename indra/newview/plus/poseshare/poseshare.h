/**
 * @file poseshare.h
 * @brief Showing the pose of your own avatar to the Charis Viewers near you, and showing theirs on your screen
 *        (plus/poseshare, research/374). The pose goes through the pose bridge (plus/lslbridge): the worn bridge of the
 *        sender shouts it, the worn bridge of each viewer near says it to its owner, and the viewer poses the sender's
 *        avatar on its own screen only. The pose is the sender's own choice to show; nothing here lets anybody pose
 *        another person's avatar.
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

#ifndef PLUS_POSESHARE_POSESHARE_H
#define PLUS_POSESHARE_POSESHARE_H

#include "lluuid.h"

namespace plus::poseshare
{
    // PlusPoseShareMode (per account): who gets the pose of your own avatar while you pose it. Off is the default.
    // "Friends only" is kept by the receiving viewer (the pose reaches everyone near; the other viewer drops it when you
    // are not its friend): it is an agreement between Charis Viewers, not a lock.
    enum class SendMode { Off = 0, Friends = 1, Everyone = 2 };
    // PlusPoseShareReceive (per account): whose shared pose is shown on your screen. Friends is the default.
    enum class ReceiveMode { Nobody = 0, Friends = 1, Everyone = 2 };

    // Main thread only for every function below.
    SendMode    sendMode();
    ReceiveMode receiveMode();
    // Writes PlusPoseShareMode (a value out of range is not written).
    void setSendMode(SendMode mode);
    // Writes PlusPoseShareReceive (a value out of range is not written).
    void setReceiveMode(ReceiveMode mode);

    // True while the pose shared by `avatar_id` is held for the screen (shown, or waiting because the poser window poses
    // that person): for the mark in the poser's list of people. One look-up of a small map.
    bool isReceiving(const LLUUID& avatar_id);
    // True while the pose `peer` sets on us (research/374b, with our consent) is on our own avatar; and a number that
    // changes whenever that starts or ends (for the reminder window).
    bool isPushedBy(const LLUUID& peer);
    unsigned pushedGeneration();
    // How many people's shared pose is held now (at most FSPoserAnimator::MAX_SHARED_OTHERS).
    int receivingCount();
}

#endif // PLUS_POSESHARE_POSESHARE_H

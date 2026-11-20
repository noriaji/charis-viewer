/**
 * @file poseconsentui.h
 * @brief Inside plus/poseconsent only: what poseconsent.cpp and the reminder window (poseconsentfloater.cpp) share.
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

#ifndef PLUS_POSECONSENTUI_H
#define PLUS_POSECONSENTUI_H

#include <string>
#include <vector>

class LLUUID;

namespace plus::poseconsent
{
    // The name of an avatar as the simulator's name service has it (never what a message says): the display
    // name and the user name, "Display (user.name)", so that a display name cannot pass for someone else;
    // hidden under RLVa's @shownames. False when the name is not known yet: it is asked for, and
    // on_arrival runs (main thread) once it comes.
    bool avatarName(const LLUUID& agent_id, std::string& name, void (*on_arrival)());

    // The reminder window of who may pose us (floater_plus_pose_consent.xml).
    constexpr char FLOATER_NAME[] = "plus_pose_consent";
    void registerFloater();
    // Opens it; without taking the keyboard focus when opened by itself (someone was just allowed).
    void showGrantedFloater(bool take_focus);
    // Bumped whenever the list of who may pose us changes (the window redraws its list then).
    unsigned grantedGeneration();
    // withdraw() for several people at once. Those who could not be told (no bridge, or a bridge there, Running or
    // Starting, that could not send right now) are told in one notice, the first time only; idle() sends their
    // withdraws again once the bridge runs, until they go, and the reminder shows them as waiting meanwhile.
    void withdrawPeers(const std::vector<LLUUID>& peers);
}

#endif // PLUS_POSECONSENTUI_H

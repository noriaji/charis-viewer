/**
 * @file lostattach.h
 * @brief Attachments kept through teleports and region crossings (from Firestorm FIRE-12004).
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

#ifndef PLUS_LOSTATTACH_H
#define PLUS_LOSTATTACH_H

// Now and then an attachment falls off in a teleport or a region crossing: the old region's
// "kill object" for it reaches the viewer after the agent is in the new region, and the new
// region's hand-over of it is late or lost (research/146). Two measures, both from Firestorm
// (Ansariel, FIRE-12004; refreshAttachments from Catznip's Appearance-PhantomAttach):
// - a kill for an attachment the agent owns is ignored while a teleport is under way, for
//   FSExperimentalLostAttachmentsFixKillDelay seconds after it has arrived (the simulator's
//   late kills), and while the agent is crossing a region; temporary attachments are not kept
//   (the simulator takes them off on leaving an experience), nor an item no longer in the
//   outfit (taken off by the agent);
// - a kill ignored after the arrival also has the attachments asked for again, five seconds
//   later, as "Advanced > Refresh Attachments" does (items already asked for lately are left);
//   before that, an ignored one whose item has left the outfit meanwhile gets its kill.
// Settings (settings_plus.xml, the names Firestorm uses): FSExperimentalLostAttachmentsFix
// (on), FSExperimentalLostAttachmentsFixKillDelay (3 s), FSExperimentalLostAttachmentsFixReport
// (off: says it in the nearby chat when a kill is ignored). Main thread.

class LLViewerObject;
class LLViewerRegion;

namespace plus::lostattach
{
    // The <plus> hook where the teleport state turns to arriving (llviewerdisplay.cpp): the
    // grace period for late kills starts.
    void onTeleportArrival();

    // The <plus> hook in process_crossed_region(): the simulator says the agent crosses a
    // region. The state ends when the avatar changes its region (onRegionChanged), by itself
    // after a while if that never comes.
    void onCrossedRegion();

    // The <plus> hook in LLVOAvatarSelf::updateRegion(): the crossing is done.
    void onRegionChanged();

    // The <plus> hook in process_kill_object(), for an object the viewer knows: true when the
    // kill is to be ignored.
    bool shouldIgnoreKill(LLViewerObject* objectp, const LLViewerRegion* regionp);

    // Asks for all attachments of the agent again (not the temporary ones). The menu
    // Advanced > Refresh Attachments.
    void refreshAttachments();
}

#endif // PLUS_LOSTATTACH_H

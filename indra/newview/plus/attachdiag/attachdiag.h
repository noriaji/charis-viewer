/**
 * @file attachdiag.h
 * @brief A census of the attachments of the avatars in view (research/355 stage 0 (5.1)).
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

#ifndef PLUS_ATTACHDIAG_ATTACHDIAG_H
#define PLUS_ATTACHDIAG_ATTACHDIAG_H

#include "linden_common.h"

// LLRHI_ATTACH_DIAG=1: a VKPerf "attach diag" line every 10 s (and up to 5 "attach diag worst" lines) about
// the other avatars in the main camera's frustum, to tell an attachment that is not there from one that is
// there and not made (research/355 section 2.3, 2.4): for each of them
//   L  the attachments the region lists (LLVOAvatar::mSimAttachments) on a point the avatar has that is not
//      a HUD's (the same count as GrayWire's listedPoint),
//   W  those worn (the objects in the attachment points that are not a HUD's),
//   missing: L above the most worn at once since the list last changed (GrayWire's listed_seen and digest: one
//      that was all there once and then fewer is not missing), and for those, where they are (research/355 5.3):
//      valid INACTIVE entries in the region's object cache whose parent is the avatar that nothing will make (no
//      octree entry and not in the orphan map: dead end 3; an octree entry and not in the map: dead end 1), those
//      waiting in the orphan map, those on their way (IN_QUEUE, WAITING, or an object waiting for its drawable),
//      entries not valid, local IDs in the orphan map with no entry at all, an ACTIVE entry whose object is not worn
//      (made, not attached), or nowhere. The entries of objects that are alive are left out and only counted, by
//      ACTIVE or not (an update of a live object leaves its entry not valid or asleep). Also the number of
//      valid INACTIVE cache entries with no parent that are not in the octree (research/355 8), in the regions walked.
//   An entry whose own parent ID is 0 and that is not valid, or is INACTIVE and not in the octree (the disk cache's), is given the
//   parent in its packed data (when the data is long enough); only one whose data has none is counted as having no parent. Such an
//   entry of an avatar's is counted apart ("from disk": not valid, valid with no octree entry), not as dead end 3 or not valid.
//   Each avatar ID is counted once. After the summary, a "gate" line: for every
//   avatar in view (missing some or not) the W worn (not a HUD's) and the cache entries of those worn objects, ACTIVE and other,
//   to tell whether attachments come the cache's way (v + o near W) or uncompressed (v + o near 0).
// Reading only: nothing it looks at is changed (a scan of the region's cache map, the
// avatar's attachment tables and the orphan map; the read of a parent ID from an entry's data moves its read position and puts it back to the start,
// as the viewer's own reads do). Off: the hook is one flag test. Main thread only (the idle
// loop). The avatars it takes: not self (its list is another's), not animesh, not an avatar of a UI view, not
// dead; "in view" is LLVOAvatar::extentsInCameraFrustum() (isVisible() is only counted, as a number to compare:
// it is also true of an avatar only a shadow, probe or water pass sees).
namespace plus::attachdiag
{
    // Set once from the environment.
    extern const bool sOn;

    // Counts once a frame the time to the next report; the report (a scan of the region's cache) is every 10 s.
    void frame();
    inline void endFrame()
    {
        if (sOn)
        {
            frame();
        }
    }
}

#endif // PLUS_ATTACHDIAG_ATTACHDIAG_H

/**
 * @file tprelease.h
 * @brief After a teleport, lets go of the old region's textures without
 *        waiting for the simulator to drop the region.
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

#ifndef PLUS_TPRELEASE_H
#define PLUS_TPRELEASE_H

class LLViewerFetchedTexture;

namespace plus::tprelease
{
    // The <plus> hook in LLViewerTextureList::finishFaceScan(), before the
    // texture's stats are added. For a while after a teleport, a texture whose
    // faces are all far past the draw distance and none of which has been on
    // screen since the arrival (the old region's, whose objects live on until
    // the simulator lets the region go) has its stats reset and is sent down
    // to the level its faces need from there (the smallest one). nearest: the
    // nearest face's distance; all_faces: it has faces and every one was
    // looked at; seen: one of them is on screen. Main thread.
    void onFaceScan(LLViewerFetchedTexture* imagep, float nearest, bool all_faces, bool seen);

    // The <plus> hook in LLViewerTextureList::finishImageDecodePriority(): how
    // long a texture nothing refers to stays in the list (upstream: timeout).
    // For a while after a teleport, shorter for the textures last on screen
    // before the arrival. Main thread.
    float lazyFlushTimeout(const LLViewerFetchedTexture* imagep, float timeout);

    // Whether the window after an arrival is open (the textures' visits do
    // more then: the fetch pass does not pace them, research/178 G1).
    bool windowOpen();
}

#endif // PLUS_TPRELEASE_H

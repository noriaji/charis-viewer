/**
 * @file hairdetect.h
 * @brief "Hair sway": which of the agent's attachments are hair, judged by
 *        their shape and textures (not their names), or set by hand.
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

#ifndef PLUS_HAIRDETECT_H
#define PLUS_HAIRDETECT_H

class LLViewerJointAttachment;
class LLViewerObject;
class LLVOAvatar;
class LLVOAvatarSelf;

namespace plus::hairsway::detect
{
    // Whether one of the agent's attachments (its root object, worn on
    // attachment) sways as hair: set by hand (the attachment's context menu,
    // PlusHairSwayManual), else judged (PlusHairSwayAutoDetect) once its mesh
    // and textures are in (textures: at most 30 s after the meshes; a
    // judgement made without some is made again when they come in), the
    // judgement kept per attachment. False while it is still being judged
    // (undecided as long as its meshes are not in), and for everything with
    // PlusHairSwayDetectOnly. Main thread, from the hair sway's scan.
    bool sways(LLViewerObject* root, const LLViewerJointAttachment* attachment, LLVOAvatarSelf* self);

    // Main thread, after a scan: forgets the attachments it did not see that
    // were still being judged (judgements made are kept for the session,
    // until the attachment's meshes or textures change); one that was judged
    // and was not seen lets go of the highest LODs it holds (one judged on a
    // shown LOD is judged again when it is next seen).
    void endScan();

    // Main thread: lets go of the highest LODs every judgement holds (the
    // hair sway is off); the judgements are kept.
    void releaseAll();

    // Main thread, at quitting: lets go of the meshes' highest LODs the
    // judgements hold (the volume manager is gone by the static destructors).
    void shutdown();

    // The attachment point is on the head: mHead, a joint under it, or mNeck.
    bool onHead(const LLViewerJointAttachment* attachment, LLVOAvatar* avatar);

    // From the feature's init (menu callbacks) and onUIReady (adds the
    // hair sway items to the agent's attachment context menu).
    void init();
    void onUIReady();
}

#endif // PLUS_HAIRDETECT_H

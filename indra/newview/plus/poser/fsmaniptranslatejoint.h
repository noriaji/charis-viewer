/**
 * @file fsmaniptranslatejoint.h
 * @brief custom manipulator for moving joints
 *
 * $LicenseInfo:firstyear=2024&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (c) 2025 Angeldark Raymaker @ Second Life
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
 *
 * The Phoenix Firestorm Project, Inc., 1831 Oakwood Drive, Fairmont, Minnesota 56031-3225 USA
 * http://www.firestormviewer.org
 * $/LicenseInfo$
 */

#ifndef FS_MANIP_TRANSLATE_JOINT_H
#define FS_MANIP_TRANSLATE_JOINT_H

#include "fsmaniprotatejoint.h"
#include "llmanip.h"

class LLJoint;
class LLVOAvatar; // for LLVOAvatarSelf, etc.

// Port note: derives from LLManip instead of LLManipTranslate. Firestorm made
// LLManipTranslate's private members protected to reach them; the members this
// class uses are declared below instead, so llmaniptranslate.h stays official.
class FSManipTranslateJoint : public LLManip
{
public:
    class ManipulatorHandle
    {
    public:
        LLVector3  mStartPosition;
        LLVector3  mEndPosition;
        EManipPart mManipID;
        F32        mHotSpotRadius;

        ManipulatorHandle(LLVector3 start_pos, LLVector3 end_pos, EManipPart id, F32 radius)
            : mStartPosition(start_pos), mEndPosition(end_pos), mManipID(id), mHotSpotRadius(radius) {}
    };

    FSManipTranslateJoint(LLToolComposite* composite);
    virtual ~FSManipTranslateJoint() = default;

    /// <summary>
    /// Sets the joint we are going to manipulate.
    /// </summary>
    /// <param name="joint">The joint to interact with.</param>
    void setJoint(LLJoint* joint);

    /// <summary>
    /// Sets the avatar the manip should interact with.
    /// </summary>
    /// <param name="avatar">The avatar to interact with.</param>
    void setAvatar(LLVOAvatar* avatar);

    /// <summary>
    /// Sets the avatar the manip should interact with.
    /// </summary>
    /// <param name="avatar">The avatar to interact with.</param>
    void setReferenceFrame(const E_PoserReferenceFrame frame) { mReferenceFrame = frame; };

    bool        handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool        handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool        handleHover(S32 x, S32 y, MASK mask) override;
    void        render() override;
    void        handleSelect() override;

    void highlightManipulators(S32 x, S32 y) override;
    bool handleMouseDownOnPart(S32 x, S32 y, MASK mask) override;
    bool canAffectSelection() override;

protected:
    void renderArrow(S32 which_arrow, S32 selected_arrow, F32 box_size, F32 arrow_size, F32 handle_size, bool reverse_direction);
    void renderTranslationHandles();
    void renderText();

private:
    bool isAvatarJointSafeToUse();

    /// <summary>
    /// Determines if the currently selected joint is one to move with manip.
    /// </summary>
    /// <returns>True if we can move the joint with manip, otherwise false.</returns>
    bool isMoveableJoint();
    void getManipJointNormal(LLJoint* joint, EManipPart manip, LLVector3& normal) { getManipNormal(nullptr, manip, normal); };
    bool getManipJointAxis(LLJoint* joint, EManipPart manip, LLVector3& axis) { return getManipAxis(nullptr, manip, axis); };

    LLVector3 getChangeInPosition(const LLVector3& newPosition) const;

    LLVector3 mDragCursorLastGlobal;

    LLJoint*              mJoint          = nullptr;
    LLVOAvatar*           mAvatar         = nullptr;
    // <plus> As in FSManipRotateJoint: the avatar is not owned here (an animesh taken back to
    // the inventory, the avatars of a region left by a teleport, go while the poser keeps one
    // selected), so mAvatar and mJoint are used only while avatarStillThere() finds it among
    // the living characters (LLCharacter::sInstances, which its destructor leaves) with the same id.
    LLCharacter*          mAvatarCharacter = nullptr;   // mAvatar as the character it was given as
    LLUUID                mAvatarID;
    bool avatarStillThere();
    // </plus>
    E_PoserReferenceFrame mReferenceFrame = POSER_FRAME_BONE;

    // From LLManipTranslate (private there), with its initial values.
    S32        mLastHoverMouseX = -1;
    S32        mLastHoverMouseY = -1;
    bool       mMouseOutsideSlop = false;
    S32        mMouseDownX = -1;
    S32        mMouseDownY = -1;
    F32        mAxisArrowLength = 50.f; // pixels
    F32        mConeSize = 0.f;         // meters, world space
    F32        mArrowLengthMeters = 0.f;
    F32        mPlaneManipOffsetMeters = 0.f;
    LLVector3  mManipNormal;
    LLVector3d mDragCursorStartGlobal;
    LLVector3d mDragSelectionStartGlobal;
    LLVector4  mManipulatorVertices[18];
    LLQuaternion mGridRotation;
    LLVector3  mGridOrigin;
    LLVector3  mGridScale;
    LLVector3  mArrowScales{ 1.f, 1.f, 1.f };
    LLVector3  mPlaneScales{ 1.f, 1.f, 1.f };
    LLVector4  mPlaneManipPositions{ 1.f, 1.f, 1.f, 1.f };
};

#endif

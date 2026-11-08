/**
 * @file fsmanipikjoint.h
 * @brief Manipulator for the poser's IK drag: grab a joint in the 3D view and the chain follows.
 *
 * research/226-poser-ik-drag.md. The maths is plus::posermcp::LiveSolve (the solver the MCP
 * solve_pose tool uses); this class picks the joint, makes the target point from the mouse,
 * draws the handles and writes the solved rotations back to the poser.
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

#ifndef FS_MANIP_IK_JOINT_H
#define FS_MANIP_IK_JOINT_H

#include "llmanip.h"

#include "../posermcp/posesolverlive.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

class LLCharacter;
class LLJoint;
class LLVOAvatar;

class FSManipIKJoint : public LLManip
{
public:
    explicit FSManipIKJoint(LLToolComposite* composite);
    virtual ~FSManipIKJoint() = default;

    /// The avatar whose joints are handles (null: none). Pins belong to one avatar and are dropped on a change.
    void setAvatar(LLVOAvatar* avatar);

    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    void render() override;
    void onMouseCaptureLost() override;
    void handleDeselect() override;

    void highlightManipulators(S32 x, S32 y) override;
    bool handleMouseDownOnPart(S32 x, S32 y, MASK mask) override;
    bool canAffectSelection() override;

    /// Joints pinned now (for the poser window's list), and letting them all go.
    std::vector<std::string> pinnedJoints() const;
    void                     clearPins();
    /// Bumped when the pins change, so the window can refresh its list only then.
    U32 pinsVersion() const { return mPinsVersion; }

private:
    struct Handle
    {
        const char* name;
        LLVector3   world;      // agent space
        LLCoordGL   screen;
        bool        onScreen = false;
    };
    static constexpr S32 HANDLE_COUNT = 19;

    struct Pin
    {
        std::string joint;
        LLVector3   body; // body frame, as when it was pinned
    };

    bool avatarStillThere();
    bool avatarUsable();
    void updateHandles();
    S32  nearestHandle(S32 x, S32 y) const;
    void togglePin(S32 handle);
    bool beginDrag(S32 handle, S32 x, S32 y, MASK mask);
    void endDrag();
    void applySolve();
    void drawHandle(const Handle& h, bool hot, bool pinned, bool atLimit, bool dragged);

    LLVOAvatar*  mAvatar = nullptr;
    // As in FSManipTranslateJoint: the avatar is not owned here; it is used only while
    // avatarStillThere() finds it among the living characters with the same id.
    LLCharacter* mAvatarCharacter = nullptr;
    LLUUID       mAvatarID;

    std::array<Handle, HANDLE_COUNT> mHandles;
    S32                              mHoverHandle = -1;
    std::vector<Pin>                 mPins;
    U32                              mPinsVersion = 0;

    // the drag
    S32                                     mDragHandle = -1;
    S32                                     mPinClickHandle = -1;
    std::unique_ptr<plus::posermcp::LiveSolve> mSolve;
    plus::posermcp::solver::Skeleton        mSkeleton;
    std::string                             mAvatarArg; // "self" or the avatar's id, as the MCP helpers take it
    LLVector3                               mFrameOrigin;
    LLQuaternion                            mFrameRotation; // body -> agent
    LLVector3                               mJointStartAgent;
    LLVector3d                              mCursorStartGlobal;
    LLVector3                               mPlaneNormal;
    LLVector3                               mPlaneDelta; // agent space, along the drag plane
    F32                                     mDepth = 0.f; // meters along the view direction (Alt)
    S32                                     mLastY = 0;
    S32                                     mMouseDownX = 0;
    S32                                     mMouseDownY = 0;
    bool                                    mMouseOutsideSlop = false;
    bool                                    mRecording = false;
    bool                                    mAtLimit = false;
    LLVector3                               mLastPelvis;
    bool                                    mHavePelvis = false;
    std::vector<std::pair<std::string, LLQuaternion>> mRotations; // reused every move
};

#endif

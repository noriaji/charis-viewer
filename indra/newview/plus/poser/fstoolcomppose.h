/**
 * @file fstoolcomppose.h
 * @brief Tool composites for the poser's in-world joint manipulators.
 *
 * Moved out of Firestorm's lltoolcomp.h/.cpp so that the official tool files
 * stay untouched.
 *
 * $LicenseInfo:firstyear=2024&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (c) 2025 Beq Janus @ Second Life
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

#ifndef FS_TOOLCOMPPOSE_H
#define FS_TOOLCOMPPOSE_H

#include "lltoolcomp.h"
#include "llsingleton.h"
#include "fsmaniprotatejoint.h"
#include "fsmaniptranslatejoint.h"
#include "fsmanipikjoint.h"

class LLJoint;
class LLPickInfo;
class LLToolset;
class LLVOAvatar;

/// <summary>
/// The toolset the poser switches to while its visual manipulators are on
/// (Firestorm keeps it as gPoserToolset in lltoolmgr.cpp).
/// </summary>
LLToolset* getPoserToolset();

class FSToolCompPose : public LLToolComposite, public LLSingleton<FSToolCompPose>
{
    LLSINGLETON(FSToolCompPose);
    virtual ~FSToolCompPose();

public:
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleDoubleClick(S32 x, S32 y, MASK mask) override;
    void render() override;

    // Render the manipulator even with nothing selected; this is how the
    // official LLViewerWindow::renderSelections() calls our render().
    bool isAlwaysRendered() override { return true; }

    // CTRL switches to the translate manipulator.
    LLTool* getOverrideTool(MASK mask) override;

    void setAvatar(LLVOAvatar* avatar) { mJointManip->setAvatar(avatar); }
    void setJoint(LLJoint* joint) { mJointManip->setJoint(joint); }
    void setReferenceFrame(E_PoserReferenceFrame frame) { mJointManip->setReferenceFrame(frame); }

    static void pickCallback(const LLPickInfo& pick_info);

protected:
    FSManipRotateJoint* mJointManip = nullptr;
};

class FSToolCompPoseTranslate : public LLToolComposite, public LLSingleton<FSToolCompPoseTranslate>
{
    LLSINGLETON(FSToolCompPoseTranslate);
    virtual ~FSToolCompPoseTranslate();

public:
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleDoubleClick(S32 x, S32 y, MASK mask) override;
    void render() override;
    bool isAlwaysRendered() override { return true; }

    LLTool* getOverrideTool(MASK mask) override;

    void setAvatar(LLVOAvatar* avatar) { mJointManip->setAvatar(avatar); }
    void setJoint(LLJoint* joint) { mJointManip->setJoint(joint); }
    void setReferenceFrame(E_PoserReferenceFrame frame) { mJointManip->setReferenceFrame(frame); }

    static void pickCallback(const LLPickInfo& pick_info);

protected:
    FSManipTranslateJoint* mJointManip = nullptr;
};

/// <summary>
/// The IK drag tool: grab a joint in the 3D view and the chain follows (research/226).
/// </summary>
class FSToolCompPoseIK : public LLToolComposite, public LLSingleton<FSToolCompPoseIK>
{
    LLSINGLETON(FSToolCompPoseIK);
    virtual ~FSToolCompPoseIK();

public:
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleDoubleClick(S32 x, S32 y, MASK mask) override;
    void render() override;
    bool isAlwaysRendered() override { return true; }

    LLTool* getOverrideTool(MASK mask) override;

    void setAvatar(LLVOAvatar* avatar) { mJointManip->setAvatar(avatar); }
    FSManipIKJoint* getManip() { return mJointManip; }

protected:
    FSManipIKJoint* mJointManip = nullptr;
};

#endif // FS_TOOLCOMPPOSE_H

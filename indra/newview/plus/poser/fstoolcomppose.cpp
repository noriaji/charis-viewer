/**
 * @file fstoolcomppose.cpp
 * @brief Tool composites for the poser's in-world joint manipulators.
 *
 * Moved out of Firestorm's lltoolcomp.cpp so that the official tool files
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

#include "llviewerprecompiledheaders.h"

#include "fstoolcomppose.h"

#include "llfloaterreg.h"
#include "llselectmgr.h"
#include "lltoolmgr.h"
#include "llviewerwindow.h"

LLToolset* getPoserToolset()
{
    static LLToolset* sToolset = nullptr;
    if (!sToolset)
    {
        sToolset = new LLToolset();
        sToolset->setShowFloaterTools(false);
        sToolset->addTool(FSToolCompPose::getInstance());
        sToolset->addTool(FSToolCompPoseTranslate::getInstance());
        sToolset->addTool(FSToolCompPoseIK::getInstance());
    }
    return sToolset;
}

namespace
{
    // The manipulators draw in agent space; a HUD selection puts the
    // renderSelections() pass in HUD coordinates, so stay out of it.
    bool selectionIsHud()
    {
        return LLSelectMgr::getInstance()->getSelection()->getSelectType() == SELECT_TYPE_HUD;
    }
}

//-----------------------------------------------------------------------
// FSToolCompPose

FSToolCompPose::FSToolCompPose()
:   LLToolComposite(std::string("Pose"))
{
    mJointManip = new FSManipRotateJoint(this);
    mManip      = mJointManip;
    mCur        = mJointManip;
    mDefault    = mJointManip;
}

FSToolCompPose::~FSToolCompPose()
{
    mManip = nullptr;
    delete mJointManip;
    mJointManip = nullptr;
}

bool FSToolCompPose::handleHover(S32 x, S32 y, MASK mask)
{
    // If the current subtool hasn't captured the mouse, switch to the manip subtool.
    if (!mCur->hasMouseCapture())
    {
        setCurrentTool(mJointManip);
    }
    return mCur->handleHover(x, y, mask);
}

bool FSToolCompPose::handleMouseDown(S32 x, S32 y, MASK mask)
{
    mMouseDown = true;

    // Kick off an async pick; pickCallback decides whether a manipulator ring was hit.
    gViewerWindow->pickAsync(x, y, mask, pickCallback);
    return true;
}

//static
void FSToolCompPose::pickCallback(const LLPickInfo& pick_info)
{
    FSToolCompPose*     self  = FSToolCompPose::getInstance();
    FSManipRotateJoint* manip = self->mJointManip;
    if (!manip)
        return;

    manip->highlightManipulators(pick_info.mMousePt.mX, pick_info.mMousePt.mY);

    if (!self->mMouseDown)
        return; // mouse is up again; the highlight is all there is to do

    if (manip->getHighlightedPart() != LLManip::LL_NO_PART)
    {
        self->setCurrentTool(manip);
        manip->handleMouseDownOnPart(pick_info.mMousePt.mX, pick_info.mMousePt.mY, pick_info.mKeyMask);
    }
    else
    {
        LL_DEBUGS("FSToolCompPose") << "No manipulator ring selected" << LL_ENDL;
    }
}

bool FSToolCompPose::handleMouseUp(S32 x, S32 y, MASK mask)
{
    mMouseDown = false;
    return LLToolComposite::handleMouseUp(x, y, mask);
}

LLTool* FSToolCompPose::getOverrideTool(MASK mask)
{
    if (mask == MASK_CONTROL)
    {
        return FSToolCompPoseTranslate::getInstance();
    }

    return LLToolComposite::getOverrideTool(mask);
}

bool FSToolCompPose::handleDoubleClick(S32 x, S32 y, MASK mask)
{
    if (!mJointManip->getSelection()->isEmpty() && mJointManip->getHighlightedPart() == LLManip::LL_NO_PART)
    {
        LLFloaterReg::showInstance("fs_poser");
        return true;
    }

    return handleMouseDown(x, y, mask);
}

void FSToolCompPose::render()
{
    if (selectionIsHud())
        return;

    mCur->render();
}

//-----------------------------------------------------------------------
// FSToolCompPoseTranslate

FSToolCompPoseTranslate::FSToolCompPoseTranslate()
:   LLToolComposite(std::string("PoseTranslate"))
{
    mJointManip = new FSManipTranslateJoint(this);
    mManip      = mJointManip;
    mCur        = mJointManip;
    mDefault    = mJointManip;
}

FSToolCompPoseTranslate::~FSToolCompPoseTranslate()
{
    mManip = nullptr;
    delete mJointManip;
    mJointManip = nullptr;
}

bool FSToolCompPoseTranslate::handleHover(S32 x, S32 y, MASK mask)
{
    if (!mCur->hasMouseCapture())
    {
        setCurrentTool(mJointManip);
    }
    return mCur->handleHover(x, y, mask);
}

bool FSToolCompPoseTranslate::handleMouseDown(S32 x, S32 y, MASK mask)
{
    mMouseDown = true;
    gViewerWindow->pickAsync(x, y, mask, pickCallback);
    return true;
}

//static
void FSToolCompPoseTranslate::pickCallback(const LLPickInfo& pick_info)
{
    FSToolCompPoseTranslate* self  = FSToolCompPoseTranslate::getInstance();
    FSManipTranslateJoint*   manip = self->mJointManip;
    if (!manip)
        return;

    manip->highlightManipulators(pick_info.mMousePt.mX, pick_info.mMousePt.mY);

    if (!self->mMouseDown)
        return;

    if (manip->getHighlightedPart() != LLManip::LL_NO_PART)
    {
        self->setCurrentTool(manip);
        manip->handleMouseDownOnPart(pick_info.mMousePt.mX, pick_info.mMousePt.mY, pick_info.mKeyMask);
    }
    else
    {
        LL_DEBUGS("FSToolCompPose") << "No manipulator handle selected" << LL_ENDL;
    }
}

bool FSToolCompPoseTranslate::handleMouseUp(S32 x, S32 y, MASK mask)
{
    mMouseDown = false;
    return LLToolComposite::handleMouseUp(x, y, mask);
}

LLTool* FSToolCompPoseTranslate::getOverrideTool(MASK mask)
{
    // Firestorm hands CTRL+SHIFT to the object scale tool here, which has
    // nothing to do with posing; keep the default behaviour instead.
    return LLToolComposite::getOverrideTool(mask);
}

bool FSToolCompPoseTranslate::handleDoubleClick(S32 x, S32 y, MASK mask)
{
    if (!mJointManip->getSelection()->isEmpty() && mJointManip->getHighlightedPart() == LLManip::LL_NO_PART)
    {
        LLFloaterReg::showInstance("fs_poser");
        return true;
    }

    return handleMouseDown(x, y, mask);
}

void FSToolCompPoseTranslate::render()
{
    if (selectionIsHud())
        return;

    mCur->render();
}

//-----------------------------------------------------------------------
// FSToolCompPoseIK

FSToolCompPoseIK::FSToolCompPoseIK()
:   LLToolComposite(std::string("PoseIK"))
{
    mJointManip = new FSManipIKJoint(this);
    mManip      = mJointManip;
    mCur        = mJointManip;
    mDefault    = mJointManip;
}

FSToolCompPoseIK::~FSToolCompPoseIK()
{
    mManip = nullptr;
    delete mJointManip;
    mJointManip = nullptr;
}

bool FSToolCompPoseIK::handleHover(S32 x, S32 y, MASK mask)
{
    if (!mCur->hasMouseCapture())
    {
        setCurrentTool(mJointManip);
    }
    return mCur->handleHover(x, y, mask);
}

bool FSToolCompPoseIK::handleMouseDown(S32 x, S32 y, MASK mask)
{
    // The handles are picked on the screen (no GPU pick needed).
    setCurrentTool(mJointManip);
    return mJointManip->handleMouseDown(x, y, mask);
}

bool FSToolCompPoseIK::handleMouseUp(S32 x, S32 y, MASK mask)
{
    return LLToolComposite::handleMouseUp(x, y, mask);
}

LLTool* FSToolCompPoseIK::getOverrideTool(MASK mask)
{
    // Ctrl and Shift are the trunk / pin modifiers here, not tool switches.
    return LLToolComposite::getOverrideTool(mask);
}

bool FSToolCompPoseIK::handleDoubleClick(S32 x, S32 y, MASK mask)
{
    return handleMouseDown(x, y, mask);
}

void FSToolCompPoseIK::render()
{
    if (selectionIsHud())
        return;

    mCur->render();
}

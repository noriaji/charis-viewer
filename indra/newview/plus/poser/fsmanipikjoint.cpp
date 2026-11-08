/**
 * @file fsmanipikjoint.cpp
 * @brief Manipulator for the poser's IK drag: grab a joint in the 3D view and the chain follows.
 *
 * research/226-poser-ik-drag.md
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

#include "llviewerprecompiledheaders.h"

#include "fsmanipikjoint.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llfloaterreg.h"
#include "llcharacter.h"
#include "lljoint.h"
#include "llrender.h"
#include "llselectmgr.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewershadermgr.h"
#include "llviewertexture.h"
#include "llviewerwindow.h"
#include "llvoavatar.h"

#include "fsfloaterposer.h"
#include "fsjointpose.h"

#include "../posermcp/posehistorytools.h"
#include "../posermcp/posesolvetools.h"

#include <boost/json.hpp>

#include <algorithm>

namespace solver = plus::posermcp::solver;
namespace json   = boost::json;

namespace
{
    constexpr F32 HOT_RADIUS_PX = 14.f;  // the mouse picks the nearest handle within this many pixels
    constexpr F32 DOT_RADIUS_PX = 6.f;
    constexpr F32 MAX_DEPTH_M   = 3.f;   // how far Alt can push a joint along the view direction
    constexpr S32 MOUSE_DRAG_SLOP = 2;   // pixels

    // The joints that are handles and the handle each hangs from (for the chain lines).
    struct HandleDef
    {
        const char* name;
        S32         parent;
    };
    constexpr HandleDef HANDLE_DEFS[] = {
        { "mPelvis", -1 },        { "mTorso", 0 },          { "mChest", 1 },         { "mNeck", 2 },
        { "mHead", 3 },           { "mCollarLeft", 2 },     { "mCollarRight", 2 },   { "mShoulderLeft", 5 },
        { "mShoulderRight", 6 },  { "mElbowLeft", 7 },      { "mElbowRight", 8 },    { "mWristLeft", 9 },
        { "mWristRight", 10 },    { "mHipLeft", 0 },        { "mHipRight", 0 },      { "mKneeLeft", 13 },
        { "mKneeRight", 14 },     { "mAnkleLeft", 15 },     { "mAnkleRight", 16 },
    };

    // World size (m) of a screen pixel at a point.
    F32 metersPerPixel(const LLVector3& point)
    {
        LLViewerCamera* cam = LLViewerCamera::getInstance();
        const F32 range = dist_vec(cam->getOrigin(), point);
        return range * 2.f * tanf(cam->getView() * 0.5f) / llmax(1.f, (F32)cam->getViewHeightInPixels());
    }

    // A filled disc facing the camera.
    void fillDisc(const LLVector3& c, const LLVector3& left, const LLVector3& up, F32 r)
    {
        constexpr S32 N = 16;
        gGL.begin(LLRender::TRIANGLES);
        for (S32 i = 0; i < N; ++i)
        {
            const F32 a0 = F_TWO_PI * i / N, a1 = F_TWO_PI * (i + 1) / N;
            gGL.vertex3fv(c.mV);
            gGL.vertex3fv((c + (left * cosf(a0) + up * sinf(a0)) * r).mV);
            gGL.vertex3fv((c + (left * cosf(a1) + up * sinf(a1)) * r).mV);
        }
        gGL.end();
    }

    void ringLoop(const LLVector3& c, const LLVector3& left, const LLVector3& up, F32 r)
    {
        constexpr S32 N = 20;
        gGL.begin(LLRender::LINE_LOOP);
        for (S32 i = 0; i < N; ++i)
        {
            const F32 a = F_TWO_PI * i / N;
            gGL.vertex3fv((c + (left * cosf(a) + up * sinf(a)) * r).mV);
        }
        gGL.end();
    }
}

FSManipIKJoint::FSManipIKJoint(LLToolComposite* composite) :
    LLManip(std::string("IK"), composite)
{
    for (S32 i = 0; i < HANDLE_COUNT; ++i)
        mHandles[i].name = HANDLE_DEFS[i].name;
}

void FSManipIKJoint::setAvatar(LLVOAvatar* avatar)
{
    if (avatar != mAvatar)
    {
        if (mDragHandle >= 0)
            endDrag();
        mPins.clear();
        ++mPinsVersion;
    }
    mAvatar          = avatar;
    mAvatarCharacter = avatar;
    mAvatarID        = avatar ? avatar->getID() : LLUUID::null;
    mHoverHandle     = -1;
}

bool FSManipIKJoint::avatarStillThere()
{
    if (!mAvatar)
        return false;
    const auto& living = LLCharacter::sInstances;
    if (std::find(living.begin(), living.end(), mAvatarCharacter) != living.end() && mAvatar->getID() == mAvatarID)
        return true;
    // Gone: forget it without reading it (endDrag does not read the avatar).
    endDrag();
    mAvatar          = nullptr;
    mAvatarCharacter = nullptr;
    mAvatarID.setNull();
    mHoverHandle     = -1;
    mHighlightedPart = LL_NO_PART;
    mManipPart       = LL_NO_PART;
    return false;
}

bool FSManipIKJoint::avatarUsable()
{
    if (!avatarStillThere())
        return false;
    if (mAvatar->isDead() || !mAvatar->isFullyLoaded())
        return false;
    return mAvatar->getRootJoint() != nullptr;
}

bool FSManipIKJoint::canAffectSelection()
{
    return avatarUsable();
}

void FSManipIKJoint::updateHandles()
{
    LLViewerCamera* cam = LLViewerCamera::getInstance();
    for (Handle& h : mHandles)
    {
        h.onScreen = false;
        LLJoint* joint = mAvatar->getJoint(h.name);
        if (!joint)
            continue;
        h.world    = joint->getWorldPosition();
        h.onScreen = cam->projectPosAgentToScreen(h.world, h.screen, false);
    }
}

S32 FSManipIKJoint::nearestHandle(S32 x, S32 y) const
{
    S32 best = -1;
    F32 bestDist = HOT_RADIUS_PX * HOT_RADIUS_PX;
    for (S32 i = 0; i < HANDLE_COUNT; ++i)
    {
        const Handle& h = mHandles[i];
        if (!h.onScreen)
            continue;
        const F32 dx = (F32)(h.screen.mX - x), dy = (F32)(h.screen.mY - y);
        const F32 d  = dx * dx + dy * dy;
        if (d <= bestDist)
        {
            bestDist = d;
            best     = i;
        }
    }
    return best;
}

void FSManipIKJoint::highlightManipulators(S32 x, S32 y)
{
    mHighlightedPart = LL_NO_PART;
    mHoverHandle     = -1;
    if (!avatarUsable())
        return;
    updateHandles();
    mHoverHandle = nearestHandle(x, y);
    if (mHoverHandle >= 0)
        mHighlightedPart = LL_ROT_GENERAL;
}

bool FSManipIKJoint::handleMouseDown(S32 x, S32 y, MASK mask)
{
    return handleMouseDownOnPart(x, y, mask);
}

bool FSManipIKJoint::handleMouseDownOnPart(S32 x, S32 y, MASK mask)
{
    if (!avatarUsable())
        return false;
    highlightManipulators(x, y);
    if (mHoverHandle < 0)
        return false; // nothing under the mouse: let the click go to the camera / selection

    // Shift+click pins; Shift+drag is a drag that takes the end of the limb along (decided on the mouse up)
    mPinClickHandle = (mask & MASK_SHIFT) ? mHoverHandle : -1;
    return beginDrag(mHoverHandle, x, y, mask);
}

void FSManipIKJoint::togglePin(S32 handle)
{
    const std::string name = mHandles[handle].name;
    auto it = std::find_if(mPins.begin(), mPins.end(), [&](const Pin& p) { return p.joint == name; });
    if (it != mPins.end())
    {
        mPins.erase(it);
    }
    else
    {
        LLJoint* root = mAvatar->getRootJoint();
        mPins.push_back({ name, (mHandles[handle].world - root->getWorldPosition()) * ~root->getWorldRotation() });
    }
    ++mPinsVersion;
}

std::vector<std::string> FSManipIKJoint::pinnedJoints() const
{
    std::vector<std::string> out;
    out.reserve(mPins.size());
    for (const Pin& p : mPins)
        out.push_back(p.joint);
    return out;
}

void FSManipIKJoint::clearPins()
{
    if (mPins.empty())
        return;
    mPins.clear();
    ++mPinsVersion;
}

bool FSManipIKJoint::beginDrag(S32 handle, S32 x, S32 y, MASK mask)
{
    using namespace plus::posermcp;

    mAvatarArg = mAvatar->isSelf() ? "self" : mAvatar->getID().asString();
    json::object args{ { "avatar", mAvatarArg } };

    try
    {
        // The skeleton as drawn, read the same way the MCP solver does (the joints of the handles and their ancestors).
        std::vector<std::string> wanted;
        wanted.reserve(HANDLE_COUNT);
        for (const Handle& h : mHandles)
            wanted.push_back(h.name);
        SolveCapture cap;
        captureSolveSkeleton(args, wanted, cap);
        mSkeleton = std::move(cap.skeleton);

        solver::State state{ mSkeleton.local, mSkeleton.rootPos };

        std::vector<LiveSolve::Pin> pins;
        pins.reserve(mPins.size());
        const std::string dragName = mHandles[handle].name;
        for (const Pin& p : mPins)
        {
            if (p.joint != dragName)
                pins.push_back({ p.joint, solver::fromLL(p.body) });
        }

        static LLCachedControl<bool> keep_ranges(gSavedSettings, "PlusPoserIKRanges", true);
        static LLCachedControl<bool> keep_floor(gSavedSettings, "PlusPoserIKKeepFloor", false);
        solver::Options options;
        options.ranges     = keep_ranges;
        options.floorGuard = keep_floor;

        LiveSolve::Mods mods;
        mods.trunk = (mask & MASK_CONTROL) != 0;
        mods.carry = (mask & MASK_SHIFT) != 0;
        mSolve     = std::make_unique<LiveSolve>(mSkeleton, state, dragName, pins, options, mods);
    }
    catch (const std::exception& e)
    {
        LL_WARNS("PoserIK") << "IK drag not started: " << e.what() << LL_ENDL;
        mSolve.reset();
        return true; // the click is used up; the poser is not posing this avatar, or the skeleton is not ready
    }

    LLJoint* root   = mAvatar->getRootJoint();
    mFrameOrigin    = root->getWorldPosition();
    mFrameRotation  = root->getWorldRotation();
    mJointStartAgent = mHandles[handle].world;
    mPlaneNormal    = LLViewerCamera::getInstance()->getAtAxis();
    mPlaneNormal.normalize();
    getMousePointOnPlaneGlobal(mCursorStartGlobal, x, y, gAgent.getPosGlobalFromAgent(mJointStartAgent), mPlaneNormal);
    mPlaneDelta.setZero();
    mDepth      = 0.f;
    mLastY      = y;
    mAtLimit    = false;
    mLastPelvis = solver::toLL(mSkeleton.pelvisOffset);
    mHavePelvis = true;
    mDragHandle = handle;
    mMouseDownX = x;
    mMouseDownY = y;
    mMouseOutsideSlop = false;
    mRecording  = beginUserEdit();
    FSJointPose::beginUndoBatch(); // the whole drag is one step for the poser window's undo

    setMouseCapture(true);
    return true;
}

bool FSManipIKJoint::handleHover(S32 x, S32 y, MASK mask)
{
    gViewerWindow->setCursor(UI_CURSOR_TOOLTRANSLATE);

    if (!hasMouseCapture() || mDragHandle < 0)
    {
        highlightManipulators(x, y);
        return true;
    }
    if (!avatarUsable() || !mSolve)
    {
        endDrag();
        return true;
    }

    if (!mMouseOutsideSlop)
    {
        if (abs(mMouseDownX - x) < MOUSE_DRAG_SLOP && abs(mMouseDownY - y) < MOUSE_DRAG_SLOP)
            return true;
        mMouseOutsideSlop = true;
    }

    if (mask & MASK_ALT)
    {
        // Alt: the mouse moves the joint toward / away from the camera instead of across the view.
        const F32 mpp = metersPerPixel(mJointStartAgent + mPlaneDelta);
        mDepth = llclamp(mDepth + (F32)(y - mLastY) * mpp, -MAX_DEPTH_M, MAX_DEPTH_M);
    }
    else
    {
        LLVector3d cursor;
        getMousePointOnPlaneGlobal(cursor, x, y, gAgent.getPosGlobalFromAgent(mJointStartAgent), mPlaneNormal);
        mPlaneDelta.setVec(cursor - mCursorStartGlobal);
    }
    mLastY = y;

    applySolve();
    plus::posermcp::holdUserEdit();
    return true;
}

void FSManipIKJoint::applySolve()
{
    using namespace plus::posermcp;

    const LLVector3 targetAgent = mJointStartAgent + mPlaneDelta + LLViewerCamera::getInstance()->getAtAxis() * mDepth;
    const LLVector3 targetBody  = (targetAgent - mFrameOrigin) * ~mFrameRotation;

    try
    {
        const auto& changed = mSolve->moveTarget(solver::fromLL(targetBody));
        mRotations.clear();
        for (const auto& [index, rot] : changed)
            mRotations.emplace_back(mSkeleton.names[index], solver::toLLQuat(rot));

        std::optional<LLVector3> pelvis;
        const LLVector3          p = solver::toLL(mSolve->pelvisPosition());
        if (dist_vec_squared(p, mLastPelvis) > 1e-8f)
        {
            pelvis      = p;
            mLastPelvis = p;
        }
        mAtLimit = mSolve->atLimit();
        if (!mRotations.empty() || pelvis)
            applySolvedPose(json::object{ { "avatar", mAvatarArg } }, mRotations, pelvis);
    }
    catch (const std::exception& e)
    {
        LL_WARNS("PoserIK") << "IK drag stopped: " << e.what() << LL_ENDL;
        endDrag();
    }
}

void FSManipIKJoint::endDrag()
{
    // The state is cleared before the capture is let go: that calls onMouseCaptureLost, which finds nothing to end.
    const bool        dragging  = mDragHandle >= 0 || mSolve;
    const bool        recording = mRecording;
    const std::string joint     = mDragHandle >= 0 ? mHandles[mDragHandle].name : "";
    mRecording  = false;
    mDragHandle = -1;
    mSolve.reset();
    mManipPart = LL_NO_PART;
    if (hasMouseCapture())
        setMouseCapture(false);

    if (recording)
        plus::posermcp::endUserEdit("ik", "IK drag: " + joint);
    if (dragging)
    {
        FSJointPose::endUndoBatch(mAvatarID);
        if (auto* poser = LLFloaterReg::findTypedInstance<FSFloaterPoser>("fs_poser"))
            poser->onIkDragDone();
    }
}

void FSManipIKJoint::onMouseCaptureLost()
{
    // Alt+Tab or another window took the mouse in the middle of a drag: the drag ends where it is.
    if (mDragHandle >= 0 || mSolve || mRecording)
        endDrag();
    mPinClickHandle = -1;
}

void FSManipIKJoint::handleDeselect()
{
    if (mDragHandle >= 0 || mSolve || mRecording)
        endDrag();
    mPinClickHandle = -1;
    LLManip::handleDeselect();
}

bool FSManipIKJoint::handleMouseUp(S32 x, S32 y, MASK mask)
{
    if (hasMouseCapture() || mDragHandle >= 0)
    {
        const S32 pinHandle = (mPinClickHandle >= 0 && !mMouseOutsideSlop) ? mPinClickHandle : -1;
        endDrag();
        if (pinHandle >= 0 && avatarUsable())
            togglePin(pinHandle);
    }
    mPinClickHandle = -1;
    return LLManip::handleMouseUp(x, y, mask);
}

void FSManipIKJoint::drawHandle(const Handle& h, bool hot, bool pinned, bool atLimit, bool dragged)
{
    LLViewerCamera* cam  = LLViewerCamera::getInstance();
    const LLVector3 left = cam->getLeftAxis();
    const LLVector3 up   = cam->getUpAxis();
    const F32       mpp  = metersPerPixel(h.world);
    const F32       r    = (hot || dragged ? DOT_RADIUS_PX * 1.4f : DOT_RADIUS_PX) * mpp;

    LLColor4 fill(1.f, 1.f, 1.f, 0.55f);
    if (pinned)
        fill = LLColor4(1.f, 0.55f, 0.1f, 0.9f);
    if (hot || dragged)
        fill = LLColor4(1.f, 0.9f, 0.2f, 0.95f);
    if (dragged && atLimit)
        fill = LLColor4(1.f, 0.15f, 0.1f, 0.95f);

    gGL.diffuseColor4fv(fill.mV);
    fillDisc(h.world, left, up, r);
    gGL.diffuseColor4f(0.f, 0.f, 0.f, 0.8f);
    ringLoop(h.world, left, up, r);

    if (pinned)
    {
        // a small lock: a ring (the shackle) over a filled box
        const LLVector3 top = h.world + up * (r * 1.9f);
        gGL.diffuseColor4f(1.f, 0.55f, 0.1f, 1.f);
        ringLoop(top, left, up, r * 0.55f);
        gGL.begin(LLRender::TRIANGLES);
        const LLVector3 a = h.world + up * (r * 2.9f) - left * (r * 0.9f), b = h.world + up * (r * 2.9f) + left * (r * 0.9f);
        const LLVector3 c = h.world + up * (r * 1.3f) + left * (r * 0.9f), d = h.world + up * (r * 1.3f) - left * (r * 0.9f);
        gGL.vertex3fv(a.mV); gGL.vertex3fv(b.mV); gGL.vertex3fv(c.mV);
        gGL.vertex3fv(a.mV); gGL.vertex3fv(c.mV); gGL.vertex3fv(d.mV);
        gGL.end();
    }
}

void FSManipIKJoint::render()
{
    if (!avatarUsable())
        return;
    updateHandles();

    gUIProgram.bind();
    LLGLSUIDefault gls_ui;
    gGL.getTexUnit(0)->bind(LLViewerFetchedTexture::sWhiteImagep);
    LLGLDepthTest gls_depth(GL_FALSE);
    LLGLEnable blend(GL_BLEND);

    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.pushMatrix();

    // the chain of the handle under the mouse (or being dragged), as lines down to the pelvis
    const S32 focus = mDragHandle >= 0 ? mDragHandle : mHoverHandle;
    if (focus >= 0)
    {
        gGL.setLineWidth(3.f);
        gGL.diffuseColor4f(1.f, 0.9f, 0.2f, 0.8f);
        gGL.begin(LLRender::LINES);
        for (S32 i = focus; i >= 0 && HANDLE_DEFS[i].parent >= 0; i = HANDLE_DEFS[i].parent)
        {
            gGL.vertex3fv(mHandles[i].world.mV);
            gGL.vertex3fv(mHandles[HANDLE_DEFS[i].parent].world.mV);
        }
        gGL.end();
        gGL.setLineWidth(1.f);
    }

    for (S32 i = 0; i < HANDLE_COUNT; ++i)
    {
        const Handle& h = mHandles[i];
        if (!h.onScreen)
            continue;
        const bool pinned = std::any_of(mPins.begin(), mPins.end(), [&](const Pin& p) { return p.joint == h.name; });
        drawHandle(h, i == mHoverHandle, pinned, mAtLimit, i == mDragHandle);
    }

    gGL.popMatrix();
}

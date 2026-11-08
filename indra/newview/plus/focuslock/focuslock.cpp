/**
 * @file focuslock.cpp
 * @brief Focus lock: the depth of field focuses on a face, or on the point under the cursor.
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

// How it works
// ------------
// LLPipeline::renderDoF() decides a focus point (agent coordinates) and turns its
// distance along the camera's axis into the circle of confusion. focusPoint() is
// called there before the official sources of the point (selected media comes
// first, so the hook sits after it); when it answers true the official ones are
// skipped. It reads the target's joints every frame, so a moving avatar or camera
// is followed, and the pipeline's own transition (CameraFocusTransitionTime) makes
// the focus glide when it jumps to someone else.
//
// Whenever the target cannot be trusted - no avatar, a joint the skeleton lacks, an
// impostor or an avatar that is not drawn (their joints are not updated), a cursor
// outside the world view, a point at the camera itself - the answer is false and the official focus is used, so the
// picture never goes blurry for lack of a point.
//
// Glass between the camera and the target (PlusFocusThroughGlass): the focus distance of a face
// is the joint's place, so no surface in between moves it; but each pixel's blur comes from the
// depth buffer, and the alpha pool's depth pass for depth of field wrote a pane's depth over the
// face seen through it (fullbrightF.glsl tests only the texture's alpha there, and a pane's
// transparency is its colour's alpha), so the face was blurred as if it were at the glass.
// plus/dofalpha leaves those pixels out of that pass; the cursor's ray here goes through panes.
//
// The place clicked (mode 4, research/264): three <plus> hooks in LLViewerWindow take, while picking,
// the left clicks on the world that the UI did not take (worldClick(), before the tool), show the cross
// cursor (worldHover()) and end the picking with Esc (worldKey()). No tool is pushed and no official
// state is changed: the hooks look at the flag on each event, so ending the picking (another mode, Esc,
// leaving the photo screen) is that flag alone. The ray is cast once, at the click (rigged meshes as
// drawn); the point is kept global and turned into agent coordinates each frame.

#include "llviewerprecompiledheaders.h"

#include "focuslock.h"
#include "focushint.h"

#include "plus/radar/radar.h"
#include "plus/rlv/rlvhooks.h"

#include <algorithm>

#include "llagent.h"
#include "llagentcamera.h"
#include "llappviewer.h"
#include "llavatarname.h"
#include "llavatarnamecache.h"
#include "llcontrol.h"
#include "llfocusmgr.h"
#include "llgltfmaterial.h"
#include "lljoint.h"
#include "llstartup.h"
#include "lltextureentry.h"
#include "lluictrl.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerobjectlist.h"
#include "llviewerparcelmgr.h"
#include "llviewerwindow.h"
#include "llvoavatar.h"
#include "llvoavatarself.h"
#include "llworld.h"
#include "pipeline.h"

namespace plus::focuslock
{
    namespace
    {
        constexpr char SETTING_MODE[]   = "PlusFocusMode";
        constexpr char SETTING_PART[]   = "PlusFocusPart";
        constexpr char SETTING_AVATAR[] = "PlusFocusAvatar";
        constexpr char SETTING_DOF[]    = "RenderDepthOfField";

        // Right at the camera (first person: your own eyes) there is no distance to focus on.
        constexpr F32 MIN_DISTANCE = 0.3f;

        Mode clampMode(S32 mode)
        {
            return static_cast<Mode>(llclamp<S32>(mode, MODE_OFF, MODE_COUNT - 1));
        }

        Mode cachedMode()
        {
            static LLCachedControl<S32> mode(gSavedSettings, SETTING_MODE, 0);
            // The place clicked (mode 4) and its picking are not saved: a session that starts in mode 4 has neither,
            // so the menus would say "the place clicked" while the focus is the official one and a click does
            // nothing. It starts in mode 0 instead (modes 2 and 3 keep their target in the settings). Once, at the
            // first look; the flag is set before the setting so a listener that reads the mode does not come back.
            static bool started = false;
            if (!started)
            {
                started = true;
                if (clampMode(mode) == MODE_CLICK)
                {
                    gSavedSettings.setS32(SETTING_MODE, MODE_OFF);
                }
            }
            return clampMode(mode);
        }

        Part cachedPart()
        {
            static LLCachedControl<S32> part(gSavedSettings, SETTING_PART, 0);
            return static_cast<Part>(llclamp<S32>(part, PART_EYES, PART_BODY));
        }

        // The target's id, parsed again only when the setting's text changed.
        LLUUID cachedAvatar()
        {
            static LLCachedControl<std::string> text(gSavedSettings, SETTING_AVATAR, std::string());
            static std::string seen;
            static LLUUID      id;
            const std::string& now = text;
            if (now != seen)
            {
                seen = now;
                id.set(seen);
            }
            return id;
        }

        bool dofOn()
        {
            static LLCachedControl<bool> on(gSavedSettings, SETTING_DOF, false);
            return on;
        }

        LLVOAvatar* findAvatar(const LLUUID& id)
        {
            if (id.isNull())
            {
                return nullptr;
            }
            if (id == gAgentID)
            {
                return isAgentAvatarValid() ? gAgentAvatarp.get() : nullptr;
            }
            LLViewerObject* object = gObjectList.findObject(id);
            if (!object || object->isDead() || !object->isAvatar())
            {
                return nullptr;
            }
            return static_cast<LLVOAvatar*>(object);
        }

        // The place of the part in agent coordinates, the space plus/gaze works in. False when the
        // avatar's joints cannot be trusted this frame.
        bool partPosition(LLVOAvatar* avatar, Part part, LLVector3& position)
        {
            if (!avatar || avatar->isDead() || !avatar->mDrawable || avatar->isCulled() || avatar->isImpostor() ||
                avatar->isVisuallyMuted())
            {
                return false;
            }
            switch (part)
            {
                case PART_EYES:
                    if (!avatar->mEyeLeftp || !avatar->mEyeRightp)
                    {
                        return false;
                    }
                    position = (avatar->mEyeLeftp->getWorldPosition() + avatar->mEyeRightp->getWorldPosition()) * 0.5f;
                    break;
                case PART_CHEST:
                    if (!avatar->mChestp)
                    {
                        return false;
                    }
                    position = avatar->mChestp->getWorldPosition();
                    break;
                default:
                    if (!avatar->mPelvisp)
                    {
                        return false;
                    }
                    position = avatar->mPelvisp->getWorldPosition();
                    break;
            }
            return !position.isExactlyZero();
        }

        // Modes 2 and 3.
        bool avatarPoint(Mode mode, LLVector3& position)
        {
            LLVOAvatar* avatar = findAvatar(mode == MODE_SELF ? gAgentID : cachedAvatar());
            if (!partPosition(avatar, cachedPart(), position))
            {
                return false;
            }
            return (position - LLViewerCamera::getInstance()->getOrigin()).magVecSquared() > MIN_DISTANCE * MIN_DISTANCE;
        }

        // The cursor, when it is over the world view and not over a panel beside it.
        bool cursorInWorld()
        {
            return gViewerWindow->getWorldViewRectScaled().pointInRect(
                gViewerWindow->getCurrentMouseX(), gViewerWindow->getCurrentMouseY());
        }

        // A face of the world a ray should go through for the focus: a reflection probe's
        // volume, or a face less opaque than SEE_THROUGH_OPACITY (a PBR face by its base
        // colour when it is blended; a legacy face by its colour's alpha, the "transparency"
        // of the build floater). The texture's own alpha is already looked at by the pick
        // (LLVOVolume::lineSegmentIntersect skips the clear texels of a masked texture).
        // (An attachment is told by its root: only the root carries the attachment state, so the lens of a pair of
        // glasses, a child prim, would pass for the world's and let the focus through to the eye. research/275 L089)
        bool seeThrough(const LLViewerObject* object, S32 face)
        {
            if (!object || object->isAvatar() || object->getRootEdit()->isAttachment())
            {
                return false;
            }
            if (object->isReflectionProbe())
            {
                return true;
            }
            if (face < 0 || face >= (S32)object->getNumTEs())
            {
                return false;
            }
            const LLTextureEntry* te = object->getTE((U8)face);
            if (!te)
            {
                return false;
            }
            if (const LLGLTFMaterial* material = te->getGLTFRenderMaterial())
            {
                return material->mAlphaMode == LLGLTFMaterial::ALPHA_MODE_BLEND &&
                       material->mBaseColor.mV[VALPHA] < SEE_THROUGH_OPACITY;
            }
            return te->getColor().mV[VALPHA] < SEE_THROUGH_OPACITY;
        }

        constexpr F32 RAY_LENGTH = 512.f;

        // The ray under (x, y) of the window (scaled coordinates; -1, -1: the mouse now). Returns the first
        // object it meets: a HUD object means the point is not in the world (point is left zero then).
        // point: where the ray stops in the world (agent coordinates), zero when it meets nothing (the sky);
        // ray_end: the ray's end, RAY_LENGTH meters out. rigged: rigged meshes (mesh bodies) are hit as
        // drawn, not by the avatar's collision volumes (heavier: for a click, not for every frame).
        // With throughGlass() the ray goes on past see-through faces; when nothing is behind
        // them (the sky) the last of them is the point, as before.
        LLViewerObject* castRay(S32 x, S32 y, bool rigged, LLVector3& point, LLVector3& ray_end)
        {
            constexpr S32 MAX_PANES  = 4;      // layers of glass looked through, at most
            constexpr F32 PAST_FACE  = 0.01f;  // meters the ray starts beyond a pane it goes through

            S32        face = -1;
            LLVector4a hit;
            LLVector4a start;
            LLVector4a end;
            hit.clear();
            start.clear();
            end.clear();
            point.setZero();
            // as before: the official mouselook focus's ray (llpipeline renderDoF)
            LLViewerObject* const first = gViewerWindow->cursorIntersect(x, y, RAY_LENGTH, nullptr, -1, false, rigged, true, true, &face,
                                                                         nullptr, nullptr, &hit, nullptr, nullptr, nullptr, &start, &end);
            ray_end.set(end.getF32ptr());
            if (first && first->getRootEdit()->isHUDAttachment())
            {
                return first;   // a HUD's point is not in the world (any prim of it: the root says)
            }
            LLViewerObject* object = first;
            point.set(hit.getF32ptr());
            if (object && throughGlass())
            {
                LLVector4a step;
                step.setSub(end, start);
                step.normalize3fast();
                step.mul(PAST_FACE);
                for (S32 pane = 0; pane < MAX_PANES && object && seeThrough(object, face); ++pane)
                {
                    start.setAdd(hit, step);
                    face = -1;
                    object = gPipeline.lineSegmentIntersectInWorld(start, end, false, rigged, true, true, &face, nullptr, nullptr, &hit);
                    if (object)
                    {
                        point.set(hit.getF32ptr());
                    }
                }
            }
            return first;
        }

        // Mode 1: the point under the cursor (agent coordinates), or false (the sky, a HUD).
        bool cursorPoint(LLVector3& agent_point)
        {
            LLVector3 point;
            LLVector3 ray_end;
            const LLViewerObject* first = castRay(-1, -1, false, point, ray_end);
            if ((first && first->getRootEdit()->isHUDAttachment()) || point.isExactlyZero())
            {
                return false;   // a HUD, the sky, or nothing under the cursor
            }
            agent_point = point;
            return true;
        }

        // ---- mode 4: the place clicked (research/264) ----

        bool       sPicking  = false;   // a left click on the world puts the point (only counts in MODE_CLICK)
        bool       sHasClick = false;
        LLVector3d sClickGlobal;        // the point, global: it stays on the same place when the camera moves
        bool       sPressHeld = false;  // a left press was the picking's: its release is too

        // A point farther than this is not believed (a teleport far away): it is forgotten.
        constexpr F64 CLICK_FORGET_DISTANCE = 2048.0;

        // The clicked point in agent coordinates, when it is in front of the camera.
        bool clickPoint(LLVector3& agent_point)
        {
            if (!sHasClick)
            {
                return false;
            }
            const LLVector3d camera = gAgentCamera.getCameraPositionGlobal();
            if ((sClickGlobal - camera).magVecSquared() > CLICK_FORGET_DISTANCE * CLICK_FORGET_DISTANCE)
            {
                sHasClick = false;
                return false;
            }
            const LLVector3 point = gAgent.getPosAgentFromGlobal(sClickGlobal);
            const LLViewerCamera* view = LLViewerCamera::getInstance();
            if (view->getAtAxis() * (point - view->getOrigin()) <= MIN_DISTANCE)
            {
                return false;   // behind the camera (or at it): the pipeline's distance would be negative
            }
            agent_point = point;
            return true;
        }

        // The picking takes the world's clicks now: picking in mode 4, in the world, not in mouselook
        // (the gun tool's clicks are its own).
        bool pickingNow()
        {
            return isPicking() && !gDisconnected && LLStartUp::getStartupState() >= STATE_STARTED &&
                   !gAgentCamera.cameraMouselook();
        }

        // Whether (x, y) is on a HUD, its clear parts too: pose and AO HUDs have click areas of alpha 0, and
        // LLToolPie::handleMouseDown() picks them (transparent faces included). The world's ray stays as it is
        // (castRay(): a clear face of the world lets the focus through).
        bool onHud(S32 x, S32 y)
        {
            if (!LLPipeline::sShowHUDAttachments)
            {
                return false;   // no HUD drawn: no ray (cursorIntersect() would look in the world too)
            }
            const LLViewerObject* hit = gViewerWindow->cursorIntersect(x, y, RAY_LENGTH, nullptr, -1, true, false);
            return hit && hit->getRootEdit()->isHUDAttachment();
        }

        // A teleport (within the region too) leaves the place clicked behind: it is forgotten. Connected once, at
        // the first click, and kept for the session (no static that disconnects after the parcel manager is gone).
        void watchTeleports()
        {
            static bool connected = false;
            if (!connected)
            {
                connected = true;
                LLViewerParcelMgr::getInstance()->setTeleportFinishedCallback(
                    [](const LLVector3d&, const bool&) { sHasClick = false; });
            }
        }

        // The click at (x, y): the point it meets, the sky's far point when it meets nothing.
        // False for a HUD (its click is the HUD's).
        bool pickAt(S32 x, S32 y)
        {
            if (onHud(x, y))
            {
                return false;
            }
            LLVector3 point;
            LLVector3 ray_end;
            castRay(x, y, true, point, ray_end);    // not a HUD: onHud() saw its clear parts too
            if (point.isExactlyZero())
            {
                point = ray_end;    // the sky: far along the ray, so the distance is sharp
            }
            watchTeleports();
            sClickGlobal = gAgent.getPosGlobalFromAgent(point);
            sHasClick    = true;
            return true;
        }

        // The point of a mode that holds one (2, 3: the avatar's part; 4: the place clicked). False for the
        // other modes, and while the point cannot be used.
        bool heldPoint(Mode mode, LLVector3& position)
        {
            switch (mode)
            {
                case MODE_SELF:
                case MODE_OTHER: return avatarPoint(mode, position);
                case MODE_CLICK: return clickPoint(position);
                default:         return false;
            }
        }
    }

    bool throughGlass()
    {
        static LLCachedControl<bool> on(gSavedSettings, "PlusFocusThroughGlass", true);
        return on;
    }

    Mode getMode()
    {
        return cachedMode();
    }

    void setMode(Mode mode)
    {
        mode = clampMode(mode);
        if (mode == MODE_CLICK && cachedMode() != MODE_CLICK)
        {
            sHasClick = false;      // a new choice of the mode starts from no point
        }
        gSavedSettings.setS32(SETTING_MODE, mode);
        setPicking(mode == MODE_CLICK);
    }

    bool isPicking()
    {
        return sPicking && cachedMode() == MODE_CLICK;
    }

    void setPicking(bool on)
    {
        sPicking = on && cachedMode() == MODE_CLICK;
        if (sPicking)
        {
            // the control chosen from (the drop-down, the button) lets go of the keyboard, or the first Esc would only
            // take the focus off it (LLPanel::handleKeyHere()) before worldKey(); a text field being typed in keeps it
            const LLUICtrl* focus = dynamic_cast<const LLUICtrl*>(gFocusMgr.getKeyboardFocus());
            if (focus && !focus->acceptsTextInput())
            {
                gFocusMgr.setKeyboardFocus(nullptr);
            }
        }
        hint::setShown(sPicking);   // "click the place · Esc to stop" over the world
    }

    bool hasClickPoint()
    {
        return sHasClick;
    }

    bool worldClick(EMouseClickType clicktype, bool down, MASK mask, S32 x, S32 y)
    {
        if (clicktype != CLICK_LEFT && clicktype != CLICK_DOUBLELEFT)
        {
            return false;
        }
        if (!down)
        {
            // the release of a press the picking took, even when the picking ended meanwhile (Esc while held):
            // the tool must not get a release without its press
            const bool ours = sPressHeld;
            sPressHeld = false;
            return ours;
        }
        sPressHeld = false;
        // A double click's second press comes as CLICK_DOUBLELEFT: quick clicks one after another move the
        // point too. Alt is the Alt zoom (the camera tool), left alone.
        if (!pickingNow() || (mask & MASK_ALT) || !pickAt(x, y))
        {
            return false;
        }
        sPressHeld = true;
        // as LLTool::handleAnyMouseClick() does for a click on the world: a text field lets go of the keyboard
        // (it commits), and Esc comes to worldKey()
        gFocusMgr.setKeyboardFocus(nullptr);
        return true;
    }

    void worldHover(MASK mask)
    {
        if (!(mask & MASK_ALT) && pickingNow())
        {
            gViewerWindow->setCursor(UI_CURSOR_CROSS);
        }
    }

    bool worldKey(KEY key, MASK mask)
    {
        if (key != KEY_ESCAPE || mask != MASK_NONE || !pickingNow())
        {
            return false;
        }
        setPicking(false);
        return true;
    }

    Part getPart()
    {
        return cachedPart();
    }

    void setPart(Part part)
    {
        gSavedSettings.setS32(SETTING_PART, llclamp<S32>(part, PART_EYES, PART_BODY));
    }

    LLUUID getAvatar()
    {
        return cachedAvatar();
    }

    void setAvatar(const LLUUID& id)
    {
        gSavedSettings.setString(SETTING_AVATAR, id.isNull() ? std::string() : id.asString());
    }

    bool focusPoint(LLVector3& agent_point)
    {
        const Mode mode = cachedMode();
        if (mode == MODE_OFF)
        {
            return false;
        }
        if (mode == MODE_CURSOR)
        {
            return cursorInWorld() && cursorPoint(agent_point);
        }
        // through a local: the pipeline takes any point left in agent_point, even with false
        LLVector3 position;
        if (!heldPoint(mode, position))
        {
            return false;
        }
        agent_point = position;
        return true;
    }

    State state()
    {
        const Mode mode = cachedMode();
        if (mode == MODE_OFF)
        {
            return State::Off;
        }
        if (!dofOn())
        {
            return State::BlurOff;
        }
        if (mode == MODE_CURSOR)
        {
            return cursorInWorld() ? State::Locked : State::Lost;
        }
        LLVector3 position;
        return heldPoint(mode, position) ? State::Locked : State::Lost;
    }

    bool focusScreenPos(LLVector2& pixels)
    {
        const Mode mode = cachedMode();
        if (mode == MODE_OFF || !dofOn())
        {
            return false;
        }
        if (mode == MODE_CURSOR)
        {
            if (!cursorInWorld())
            {
                return false;
            }
            pixels.set((F32)gViewerWindow->getCurrentMouseX(), (F32)gViewerWindow->getCurrentMouseY());
            return true;
        }
        LLVector3 position;
        if (!heldPoint(mode, position))
        {
            return false;
        }
        LLCoordGL point;
        if (!LLViewerCamera::getInstance()->projectPosAgentToScreen(position, point, true))
        {
            return false;   // behind the camera, or outside the world view
        }
        pixels.set((F32)point.mX, (F32)point.mY);
        return true;
    }

    bool focusDistance(F32& meters)
    {
        LLVector3 position;
        if (!dofOn() || !heldPoint(cachedMode(), position))
        {
            return false;
        }
        meters = (position - LLViewerCamera::getInstance()->getOrigin()).magVec();
        return true;
    }

    std::string focusName()
    {
        const Mode mode = cachedMode();
        if (mode != MODE_SELF && mode != MODE_OTHER)
        {
            return std::string();
        }
        const LLUUID id = mode == MODE_SELF ? gAgentID : cachedAvatar();
        if (id.isNull())
        {
            return std::string();
        }
        LLAvatarName name;
        std::string text;
        if (LLAvatarNameCache::get(id, &name))
        {
            text = plus::radar::Engine::formatName(name);
        }
        else if (LLVOAvatar* avatar = findAvatar(id))
        {
            text = avatar->getFullname();
        }
        // RLVa @shownames: the anonym (plus/rlv, research/248 3.4)
        return (plus::rlv::enabled() && !text.empty()) ? plus::rlv::avatarNameForPlus(id, text) : text;
    }

    const char* partKey(Part part)
    {
        switch (part)
        {
            case PART_EYES:  return "eyes";
            case PART_CHEST: return "chest";
            default:         return "body";
        }
    }

    size_t listNearby(std::vector<Nearby>& out, F32 radius)
    {
        out.clear();
        uuid_vec_t              ids;
        std::vector<LLVector3d> positions;
        const LLVector3d        origin = gAgentCamera.getCameraPositionGlobal();
        LLWorld::getInstance()->getAvatars(&ids, &positions, origin, radius);
        for (size_t i = 0; i < ids.size(); ++i)
        {
            if (ids[i] == gAgentID)
            {
                continue;
            }
            LLVOAvatar* avatar = findAvatar(ids[i]);
            if (!avatar || avatar->isControlAvatar())
            {
                continue;
            }
            Nearby entry;
            entry.id       = ids[i];
            entry.distance = (F32)(positions[i] - origin).magVec();
            LLAvatarName name;
            entry.name     = LLAvatarNameCache::get(ids[i], &name) ? plus::radar::Engine::formatName(name) : avatar->getFullname();
            if (plus::rlv::enabled())
            {
                entry.name = plus::rlv::avatarNameForPlus(ids[i], entry.name);   // RLVa @shownames (research/248 3.4)
            }
            out.push_back(std::move(entry));
        }
        std::sort(out.begin(), out.end(), [](const Nearby& a, const Nearby& b) { return a.distance < b.distance; });
        return out.size();
    }
}

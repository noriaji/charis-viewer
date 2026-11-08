/**
 * @file photocamera.cpp
 * @brief Photo mode: the camera memory (a temporary position and named ones).
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

// The views are kept in gSavedSettings (PlusPhotoCameraSlots, PlusPhotoCameraTemp),
// not in gSavedPerAccountSettings: the camera is wanted again with whichever
// account is logged in. A view is a position and a focus point in global
// coordinates (the direction is the line between them), the tilt of the camera
// (plus/camroll) and the field of view. The list shows the views of the region the
// avatar is in; a view carries that region's name.
//
// Going back is what the official camera tools do for a free camera
// (LLAgentCamera::setCameraPosAndFocusGlobal, as plus/posermcp does): the
// camera leaves the avatar and stays where it is put, until the user moves it
// (Esc or the camera controls bring it back behind the avatar).

#include "llviewerprecompiledheaders.h"

#include "photocamera.h"
#include "photomode.h"
#include "phototoolsfloater.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llcallbacklist.h"
#include "llframetimer.h"
#include "llsdutil.h"
#include "llviewercamera.h"
#include "llviewerjoystick.h"
#include "llviewermenu.h"
#include "plus/camerapath/camerapath.h"
#include "plus/camerashot/camerashot.h"
#include "plus/camroll/camroll.h"
#include "plus/rlv/rlvhooks.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llworld.h"

namespace plus::photomode
{
    namespace
    {
        constexpr char SETTING_SLOTS[]  = "PlusPhotoCameraSlots";
        constexpr char SETTING_TEMP[]   = "PlusPhotoCameraTemp";
        // With the flycam the focus point is this far ahead of its camera when the old one is too near.
        constexpr double FLYCAM_FOCUS_MIN = 1.0;
        constexpr double FLYCAM_FOCUS_DEFAULT = 10.0;

        LLSD slots()
        {
            LLSD data = gSavedSettings.getLLSD(SETTING_SLOTS);
            return data.isArray() ? data : LLSD::emptyArray();
        }

        // The places of slots() that are in the region the avatar is in and can be read: the rows of
        // namedViews(), in its order (a broken row is in neither, so the n-th of both is the same row).
        // None while namedViewsHidden(): the list is empty, and going to, renaming and deleting find nothing.
        std::vector<LLSD::Integer> regionIndices(const LLSD& data)
        {
            std::vector<LLSD::Integer> indices;
            if (namedViewsHidden())
            {
                return indices;
            }
            const std::string          region = regionName();
            for (LLSD::Integer i = 0; i < data.size(); ++i)
            {
                CameraView view;
                if (data[i]["region"].asString() == region && CameraView::fromLLSD(data[i], view))
                {
                    indices.push_back(i);
                }
            }
            return indices;
        }
    }

    LLSD CameraView::toLLSD() const
    {
        LLSD data;
        data["name"]     = name;
        data["region"]   = region;
        data["position"] = position.getValue();
        data["focus"]    = focus.getValue();
        data["fov"]      = (LLSD::Real)fov;
        data["roll"]     = (LLSD::Real)roll;
        return data;
    }

    bool CameraView::fromLLSD(const LLSD& data, CameraView& view)
    {
        if (!data.isMap() || !data.has("position") || !data.has("focus"))
        {
            return false;
        }
        view.position = LLVector3d(data["position"]);
        view.focus    = LLVector3d(data["focus"]);
        view.fov      = (float)data["fov"].asReal();
        view.roll     = (float)data["roll"].asReal(); // missing in an old saved position: level
        // NaN or infinity (a broken file) would pass llclamp(): no angle (keep the camera's), level
        if (!llfinite(view.fov))
        {
            view.fov = 0.f;
        }
        if (!llfinite(view.roll))
        {
            view.roll = 0.f;
        }
        view.name     = data["name"].asString();
        view.region   = data["region"].asString();
        return true;
    }

    std::string regionName()
    {
        const LLViewerRegion* region = gAgent.getRegion();
        return region ? region->getName() : std::string();
    }

    bool namedViewsHidden()
    {
        return plus::rlv::enabled() && !plus::rlv::canShowMenuLocation();
    }

    bool currentView(CameraView& view)
    {
        if (!gAgent.getRegion())
        {
            return false;
        }
        view.position = gAgentCamera.getCameraPositionGlobal(); // the camera's own place (the flycam's too)
        view.focus    = gAgentCamera.getFocusGlobal();
        // The angle on the camera: a camera shot's (plus/camerashot puts it there without
        // writing CameraAngle), else CameraAngle's.
        view.fov      = LLViewerCamera::getInstance()->getDefaultFOV();
        if (LLViewerJoystick::getInstance()->getOverrideCamera())
        {
            // The flycam moves the camera itself: the focus point is still where it was before the flycam
            // and its zoom is the camera's angle, not CameraAngle. A point straight ahead (as far as the old
            // one, so a glide back is about as long) and the angle on the screen.
            const LLViewerCamera* camera   = LLViewerCamera::getInstance();
            double                distance = (view.focus - view.position).magVec();
            if (distance < FLYCAM_FOCUS_MIN)
            {
                distance = FLYCAM_FOCUS_DEFAULT;
            }
            view.focus = view.position + LLVector3d(camera->getAtAxis()) * distance;
            view.fov   = camera->getView();
        }
        view.roll     = plus::camroll::currentDegrees();
        view.region   = regionName();
        view.name.clear();
        return !view.position.isExactlyZero();
    }

    void goToView(const CameraView& view, bool smooth)
    {
        if (!gAgent.getRegion())
        {
            return;
        }
        // A camera shot running would move the camera on from the view the next frame.
        plus::camerashot::stop();
        plus::camerapath::stop();
        if (gAgentCamera.cameraMouselook())
        {
            gAgentCamera.changeCameraToDefault();
        }
        // The flycam (3D mouse) would keep the camera where its own attitude puts it: hand the camera back,
        // as Firestorm does when it loads a camera position.
        if (LLViewerJoystick::getInstance()->getOverrideCamera())
        {
            handle_toggle_flycam();
        }
        // Made again from the view now, also just after the flycam was turned off (it would stay where the
        // flycam put it until the avatar moves, and camroll would hold it there).
        LLViewerJoystick::getInstance()->setCameraNeedsUpdate(true);
        gAgentCamera.setFocusOnAvatar(false, false);
        if (smooth)
        {
            // From where the camera is now (setCameraPosAndFocusGlobal starts one only
            // when the focus point moves).
            gAgentCamera.startCameraAnimation();
        }
        gAgentCamera.setCameraPosAndFocusGlobal(view.position, view.focus, LLUUID::null);
        if (!smooth)
        {
            gAgentCamera.stopCameraAnimation(); // jump
        }
        if (view.fov > 0.f)
        {
            // On the camera only, as a camera shot does: CameraAngle (saved) is not written, so a
            // narrow angle saved during or just after a shot does not stay into the next session.
            // The user's CameraAngle comes back when the camera goes back to the avatar (Esc).
            plus::camerashot::holdFov(view.fov);
        }
        plus::camroll::setDegrees(view.roll); // after the resets of the camera above (they level it)
    }

    void saveTempView()
    {
        CameraView view;
        if (currentView(view))
        {
            gSavedSettings.setLLSD(SETTING_TEMP, view.toLLSD());
        }
    }

    namespace
    {
        // The temporary view, when it is of the region the avatar is in (in another one its place would
        // be somewhere else entirely: the named views are kept by region for the same reason).
        bool tempView(CameraView& view)
        {
            return CameraView::fromLLSD(gSavedSettings.getLLSD(SETTING_TEMP), view) && !view.region.empty() &&
                   view.region == regionName();
        }
    }

    bool hasTempView()
    {
        CameraView view;
        return tempView(view);
    }

    bool goToTempView(bool smooth)
    {
        CameraView view;
        if (!tempView(view))
        {
            return false;
        }
        goToView(view, smooth);
        return true;
    }

    std::vector<CameraView> namedViews()
    {
        std::vector<CameraView> views;
        const LLSD              data = slots();
        for (LLSD::Integer index : regionIndices(data))
        {
            CameraView view;
            if (CameraView::fromLLSD(data[index], view))
            {
                views.push_back(view);
            }
        }
        return views;
    }

    bool saveNamedView(const std::string& name)
    {
        CameraView view;
        if (namedViewsHidden() || !currentView(view))
        {
            return false;
        }
        // A row that cannot be read (edited by hand, an old format) is shown nowhere and cannot be deleted
        // from the list: it is dropped here, so it does not keep a place of MAX_VIEWS_ALL.
        LLSD data = LLSD::emptyArray();
        for (const LLSD& row : llsd::inArray(slots()))
        {
            CameraView readable;
            if (CameraView::fromLLSD(row, readable))
            {
                data.append(row);
            }
        }
        if (regionIndices(data).size() >= (size_t)MAX_NAMED_VIEWS || data.size() >= (LLSD::Integer)MAX_VIEWS_ALL)
        {
            return false;
        }
        view.name = name;
        if (view.name.empty())
        {
            view.name = "Camera " + std::to_string(regionIndices(data).size() + 1);
        }
        data.append(view.toLLSD());
        gSavedSettings.setLLSD(SETTING_SLOTS, data);
        return true;
    }

    bool allNamedViewsFull()
    {
        LLSD::Integer readable = 0;
        for (const LLSD& row : llsd::inArray(slots()))
        {
            CameraView view;
            readable += CameraView::fromLLSD(row, view) ? 1 : 0;
        }
        return readable >= (LLSD::Integer)MAX_VIEWS_ALL;
    }

    bool goToNamedView(size_t index, bool smooth)
    {
        const std::vector<CameraView> views = namedViews();
        if (index >= views.size())
        {
            return false;
        }
        goToView(views[index], smooth);
        return true;
    }

    bool renameNamedView(size_t index, const std::string& name)
    {
        LLSD                             data    = slots();
        const std::vector<LLSD::Integer> indices = regionIndices(data);
        if (index >= indices.size() || name.empty())
        {
            return false;
        }
        data[indices[index]]["name"] = name;
        gSavedSettings.setLLSD(SETTING_SLOTS, data);
        return true;
    }

    bool deleteNamedView(size_t index)
    {
        const LLSD                       data    = slots();
        const std::vector<LLSD::Integer> indices = regionIndices(data);
        if (index >= indices.size())
        {
            return false;
        }
        LLSD kept = LLSD::emptyArray();
        for (LLSD::Integer i = 0; i < data.size(); ++i)
        {
            if (i != indices[index])
            {
                kept.append(data[i]);
            }
        }
        gSavedSettings.setLLSD(SETTING_SLOTS, kept);
        return true;
    }

    // --- The region's push-back of the camera (photocamera.h) ----------------------

    namespace
    {
        constexpr char SETTING_IGNORE_SIM_PLANE[] = "PlusIgnoreSimCameraConstraint";

        // The last plane the region sent (before the filter) and the region it came from: put back
        // when the setting is turned off, so the push-back works at once instead of after the
        // region's next message.
        LLVector4 sSimPlane(0.f, 0.f, 0.f, 0.f);
        U64       sSimPlaneRegion = 0;

        // The setting works in the photo mode only (research/275 L166, 2026-11-06): while the photo screen is
        // on or the photo tools window is open and not minimized. Out of it the region's plane stands, the
        // setting on or not.
        // How often the photo mode is looked at while the setting is on (going in or out of it moves the
        // plane this much later at most). Not looked at while the setting is off.
        constexpr F32 PHOTO_MODE_POLL_SECONDS = 0.25f;

        bool        sIgnoring = false;  // LLAgentCamera has "no plane" from us now
        bool        sWatching = false;  // the idle check of the photo mode is registered
        LLFrameTimer sPollTimer;

        U64 currentRegionHandle()
        {
            const LLViewerRegion* region = gAgent.getRegion();
            return region ? region->getHandle() : 0;
        }

        bool settingOn()
        {
            static LLCachedControl<bool> ignore(gSavedSettings, SETTING_IGNORE_SIM_PLANE, false);
            return ignore;
        }

        bool wantIgnore()
        {
            return settingOn() && inPhotoMode();
        }

        // Gives LLAgentCamera the plane for the state now, when that changes: none while ignoring; back out
        // of it, the region's plane from before (another region's would be wrong: that one waits for its own
        // message).
        void applyIgnore(bool ignore)
        {
            if (ignore == sIgnoring)
            {
                return;
            }
            sIgnoring = ignore;
            if (ignore)
            {
                gAgentCamera.setCameraCollidePlane(LLVector4(0.f, 0.f, 0.f, 0.f));
            }
            else if (sSimPlaneRegion != 0 && sSimPlaneRegion == currentRegionHandle())
            {
                gAgentCamera.setCameraCollidePlane(sSimPlane);
            }
        }

        // While the setting is on: follows the photo mode going in and out. Stops itself when it goes off.
        void watchPhotoMode()
        {
            if (sWatching)
            {
                return;
            }
            sWatching = true;
            sPollTimer.reset();
            doOnIdleRepeating([]()
            {
                if (!settingOn())
                {
                    applyIgnore(false);
                    sWatching = false;
                    return true;    // done
                }
                if (sPollTimer.getElapsedTimeF32() >= PHOTO_MODE_POLL_SECONDS)
                {
                    sPollTimer.reset();
                    applyIgnore(inPhotoMode());
                }
                return false;
            });
        }
    }

    void filterCameraCollidePlane(LLVector4& plane, const LLHost& sender)
    {
        // Kept under the region that sent it (a neighbour's message can come in while the agent is
        // on another region, or just as it crosses); an unknown sender is not put back later.
        const LLViewerRegion* from = LLWorld::getInstance()->getRegion(sender);
        sSimPlane       = plane;
        sSimPlaneRegion = from ? from->getHandle() : 0;
        sIgnoring       = wantIgnore();
        if (sIgnoring)
        {
            plane.set(0.f, 0.f, 0.f, 0.f);
        }
    }

    void watchCameraConstraintSetting()
    {
        if (LLControlVariable* control = gSavedSettings.getControl(SETTING_IGNORE_SIM_PLANE))
        {
            control->getSignal()->connect([](LLControlVariable*, const LLSD& value, const LLSD&)
            {
                if (value.asBoolean())
                {
                    applyIgnore(inPhotoMode());
                    watchPhotoMode();
                }
                else
                {
                    applyIgnore(false);
                }
            });
        }
        if (settingOn())
        {
            watchPhotoMode();   // on from the last session: it works once the photo mode is opened
        }
    }
}

/**
 * @file composition.cpp
 * @brief Composition presets of the simple photo screen (research/218).
 *
 * How a shot is worked out
 * ------------------------
 * The person's size comes from the skeleton, not from a fixed number, so a tall or a small avatar is
 * framed alike: H is the body height (the avatar's own measure, mBodySize; when it is not usable, the
 * eye-to-pelvis distance scaled up), and the sole of the foot is taken to be 0.93 H below the eyes (the
 * proportion of a standing figure). Every shot is a band of the body, in fractions of H over the sole:
 *
 *   full  -0.03 .. 1.03     the whole figure with a little air at both ends
 *   knee   0.30 .. 1.04     from just above the knees
 *   bust   0.62 .. 1.04     from the chest
 *   face   0.80 .. 1.03     the neck and the head
 *   low    -0.03 .. 1.03    as full, the camera 18 degrees under the middle looking up
 *   high   -0.03 .. 1.03    as full, the camera 30 degrees over the middle looking down
 *
 * The camera looks at the middle of the band and stands in front of the person (the way the avatar faces,
 * level) at the distance where the band, with 15 % of margin, fills the picture's height:
 *     d = (band * H * 1.15 / 2) / tan(vertical angle of view / 2),
 * kept between 0.4 m and 20 m. Raised or lowered by the shot's angle on a sphere round the middle, so the
 * distance to the middle stays d. The angle of view is not changed (the picture's shape and the zoom stay
 * the user's); the tilt (roll) stays too.
 *
 * Where the person is "the person in focus": plus/focuslock's target (yourself in mode 2, the chosen
 * avatar in mode 3), yourself when the focus lock is off or on the cursor.
 *
 * Moving the camera is photomode's goToView() (LLAgentCamera::setCameraPosAndFocusGlobal, as the camera
 * memory does): a glide, any camera shot or flycam handed back first. A wall between the camera and the
 * person is not looked for: the region's push-back plane is the one the camera memory meets too, and with
 * PlusIgnoreSimCameraConstraint on it is gone. The camera before the move is kept (one step) for
 * restorePrevious().
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

#include "composition.h"

#include "plus/focuslock/focuslock.h"
#include "plus/photomode/photomode.h"

#include <cmath>

#include "llagent.h"
#include "lljoint.h"
#include "llviewercamera.h"
#include "llviewerobjectlist.h"
#include "llvoavatar.h"
#include "llvoavatarself.h"

namespace plus::photoscreen::composition
{
    namespace
    {
        struct Band
        {
            const char* key;
            F32         low;        // fractions of the body height over the sole
            F32         high;
            F32         elevation;  // degrees, + the camera is over the middle
        };
        constexpr Band BANDS[(size_t)Shot::Count] = {
            { "full", -0.03f, 1.03f,   0.f },
            { "knee",  0.30f, 1.04f,   0.f },
            { "bust",  0.62f, 1.04f,   0.f },
            { "face",  0.80f, 1.03f,   0.f },
            { "low",  -0.03f, 1.03f, -18.f },
            { "high", -0.03f, 1.03f,  30.f },
        };

        constexpr F32 EYE_HEIGHT_FRACTION   = 0.93f;    // of the body height, the eyes over the sole
        constexpr F32 PELVIS_HEIGHT_FRACTION = 0.50f;
        constexpr F32 MARGIN                = 1.15f;
        constexpr F32 DISTANCE_MIN          = 0.4f;
        constexpr F32 DISTANCE_MAX          = 20.f;
        constexpr F32 BODY_MIN              = 0.3f;     // a body height outside of these is not believed
        constexpr F32 BODY_MAX              = 20.f;

        plus::photomode::CameraView sPrevious;
        bool                        sHasPrevious = false;

        LLVOAvatar* targetAvatar()
        {
            namespace fl = plus::focuslock;
            LLUUID id = gAgentID;
            if (fl::getMode() == fl::MODE_OTHER)
            {
                id = fl::getAvatar();
            }
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

        // Joints are only kept up to date for an avatar that is drawn in full (the same test as plus/focuslock).
        bool usable(LLVOAvatar* avatar)
        {
            return avatar && !avatar->isDead() && avatar->mDrawable && !avatar->isCulled() && !avatar->isImpostor() &&
                   !avatar->isVisuallyMuted() && avatar->mPelvisp && avatar->mEyeLeftp && avatar->mEyeRightp;
        }
    }

    const char* key(Shot shot)
    {
        return (size_t)shot < (size_t)Shot::Count ? BANDS[(size_t)shot].key : "";
    }

    bool canApply()
    {
        return usable(targetAvatar());
    }

    Result apply(Shot shot)
    {
        if ((size_t)shot >= (size_t)Shot::Count)
        {
            return Result::NoTarget;
        }
        LLVOAvatar* avatar = targetAvatar();
        if (!usable(avatar))
        {
            return Result::NoTarget;
        }
        plus::photomode::CameraView now;
        if (!plus::photomode::currentView(now))
        {
            return Result::NoCamera;
        }

        // The figure, in agent coordinates.
        const LLVector3 eyes   = (avatar->mEyeLeftp->getWorldPosition() + avatar->mEyeRightp->getWorldPosition()) * 0.5f;
        const LLVector3 pelvis = avatar->mPelvisp->getWorldPosition();
        F32             height = avatar->mBodySize.mV[VZ];
        if (!(height >= BODY_MIN && height <= BODY_MAX))
        {
            height = (eyes.mV[VZ] - pelvis.mV[VZ]) / (EYE_HEIGHT_FRACTION - PELVIS_HEIGHT_FRACTION);
        }
        if (!(height >= BODY_MIN && height <= BODY_MAX))
        {
            return Result::NoTarget;    // a skeleton that cannot be measured
        }
        const F32 sole = eyes.mV[VZ] - EYE_HEIGHT_FRACTION * height;

        const Band& band   = BANDS[(size_t)shot];
        const F32   span   = (band.high - band.low) * height;
        LLVector3   middle = pelvis;
        middle.mV[VZ]      = sole + (band.low + band.high) * 0.5f * height;

        // The way the person faces, level.
        LLVector3 facing = LLVector3::x_axis * avatar->getRenderRotation();
        facing.mV[VZ]    = 0.f;
        if (facing.magVecSquared() < 1e-6f)
        {
            facing = LLVector3::x_axis;
        }
        facing.normalize();

        const F32 half_angle = LLViewerCamera::getInstance()->getView() * 0.5f;
        if (!(half_angle > 0.01f && half_angle < 1.5f))
        {
            return Result::NoCamera;
        }
        const F32 distance = llclamp(span * MARGIN * 0.5f / std::tan(half_angle), DISTANCE_MIN, DISTANCE_MAX);
        const F32 elevation = band.elevation * DEG_TO_RAD;
        const LLVector3 offset = (facing * std::cos(elevation) + LLVector3::z_axis * std::sin(elevation)) * distance;

        plus::photomode::CameraView view = now;
        view.position = gAgent.getPosGlobalFromAgent(middle + offset);
        view.focus    = gAgent.getPosGlobalFromAgent(middle);
        view.fov      = 0.f;    // the angle of view stays as it is
        view.name.clear();

        sPrevious    = now;
        sHasPrevious = true;
        plus::photomode::goToView(view, true);
        return Result::Ok;
    }

    bool hasPrevious()
    {
        return sHasPrevious;
    }

    bool restorePrevious()
    {
        if (!sHasPrevious)
        {
            return false;
        }
        plus::photomode::CameraView back = sPrevious;
        back.fov                         = 0.f;     // the angle of view was not changed by the shot, nor is it now
        sHasPrevious                     = false;
        plus::photomode::goToView(back, true);
        return true;
    }
}

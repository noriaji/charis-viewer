/**
 * @file poser.cpp
 * @brief Registers the Poser (ported from Firestorm) with the plus foundation.
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

#include "plus.h"

#include "poser.h"
#include "fsfloaterposer.h"
#include "fsposeranimator.h"
#include "poseconsent/poseconsent.h"
#include "llapp.h"
#include "llcallbacklist.h"
#include "llfloaterreg.h"
#include "llframetimer.h"

namespace
{
    // <plus> other people's avatars (research/336). The pose of another person lasts only while plus::poseconsent
    // says canPose(): these stop it at every end of the consent, whether the poser window is open or not.
    constexpr F64 OTHERS_CHECK_SECONDS = 1.0; // how often the safety net looks (the same pace as poseconsent::idle)

    FSFloaterPoser* openPoser()
    {
        return LLFloaterReg::findTypedInstance<FSFloaterPoser>("fs_poser");
    }

    // poseconsent: an accepted person may no longer be posed (any end of the consent, or out of reach)
    void onConsentLost(const LLUUID& peer)
    {
        // research/374: a pose the person shares of themselves is not the consent's (the window's pose of them is)
        if (!FSPoserAnimator::isHeldByShare(peer))
            FSPoserAnimator::forceStopPosingAvatar(peer);
        if (FSFloaterPoser* poser = openPoser())
            poser->onOthersChanged(); // only a flag: poseconsent is not called back from here
    }

    void onConsentNotice(const std::string& stringId, const LLUUID& peer)
    {
        if (FSFloaterPoser* poser = openPoser())
            poser->showConsentNotice(stringId, peer);
    }

    // The safety net: a pose of someone who may not be posed any more (a missed end, a region change), or that no
    // longer runs on a live avatar (FSPoserAnimator::isOtherPoseAlive: the avatar is gone, or a new object of the same
    // person has not the motion; the pose went with the old object and the tables are cleared here), is stopped
    // within a second. Nothing posed (the usual case): one test of an empty vector.
    void idleOthers(void*)
    {
        const std::vector<LLUUID>& posed = FSPoserAnimator::posedOthers();
        if (posed.empty())
            return;

        static F64 next_check_s = 0.0;
        const F64 now_s = LLFrameTimer::getElapsedSeconds();
        if (now_s < next_check_s)
            return;
        next_check_s = now_s + OTHERS_CHECK_SECONDS;
        if (LLApp::isExiting())
            return;

        // forceStopPosingAvatar() takes the stopped one out of `posed` (the ones before it keep their place): walk it
        // from the back
        bool stopped = false;
        for (size_t i = posed.size(); i-- > 0;)
        {
            if (!plus::poseconsent::canPose(posed[i]) || !FSPoserAnimator::isOtherPoseAlive(posed[i]))
            {
                FSPoserAnimator::forceStopPosingAvatar(posed[i]);
                stopped = true;
            }
        }
        if (stopped)
        {
            if (FSFloaterPoser* poser = openPoser())
                poser->onOthersChanged();
        }
    }
    // </plus>

    void init()
    {
        // Firestorm: llviewerfloaterreg.cpp
        LLFloaterReg::add("fs_poser", "floater_fs_poser.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<FSFloaterPoser>);

        // Firestorm: textures.xml. Images the official textures.xml lacks;
        // the same names as in Firestorm so its XUI works unchanged.
        plus::declareUIImage("Plus_Poser_Visual_On",        "plus/visual_pose_enabled.png");
        plus::declareUIImage("Plus_Icon_Undock_Foreground", "plus/Icon_Undock_Foreground.png");
        plus::declareUIImage("Plus_Edit_Flip_X",            "plus/Edit_Flip_X.png");
        plus::declareUIImage("Plus_Script_Undo",            "plus/script_undo.png");
        plus::declareUIImage("Plus_Script_Redo",            "plus/script_redo.png");
        plus::declareUIImage("Plus_Script_Running",         "plus/green_square.png");
        plus::declareUIImage("Plus_Script_NotRunning",      "plus/yellow_square.png");
        plus::declareUIImage("Plus_Script_Error",           "plus/red_square.png");

        // Settings: app_settings/settings_plus.xml (FSPoser*, FSManip*).
        // Menu item: skins/default/xui/<lang>/menu_plus.xml ("Poser").

        // <plus> other people's avatars with their consent (research/336)
        plus::poseconsent::setConsentLostHandler(&onConsentLost);
        plus::poseconsent::setNoticeHandler(&onConsentNotice);
        gIdleCallbacks.addFunction(&idleOthers);
        // </plus>
    }

    plus::FeatureRegistration sRegistration({ "poser", &init, {} });
}

// <plus> research/336
bool plus::poser::isPosingOther(const LLUUID& avatar_id)
{
    // research/374: the people whose shared pose is held count too (not turned into an impostor or a silhouette)
    const std::vector<LLUUID>& posed  = FSPoserAnimator::posedOthers();
    const std::vector<LLUUID>& shared = FSPoserAnimator::sharedOthers();
    return (!posed.empty() && FSPoserAnimator::isPosingOther(avatar_id)) ||
           (!shared.empty() && FSPoserAnimator::isSharedOther(avatar_id));
}
// </plus>

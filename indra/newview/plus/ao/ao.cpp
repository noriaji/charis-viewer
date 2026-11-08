/**
 * @file ao.cpp
 * @brief Animation Overrider (ported from Firestorm, plus/ao): registration
 *        with the plus foundation, the motion hooks and the Plus menu items.
 *
 * Where Firestorm calls the AO from official files, this feature looks for
 * itself instead, so that only LLVOAvatar::startMotion()/stopMotion() carry
 * a hook:
 *   - login complete (FS: LLVOAvatar::idleUpdateLoadingEffect()): the engine
 *     is made on the first idle in-world and starts when the agent's avatar
 *     is fully loaded (AOEngine::onIdle());
 *   - mouselook (FS: LLAgentCamera) and water (FS: LLVOAvatar::
 *     idleUpdateBelowWater()): AOEngine::onIdle() reads both every frame;
 *   - quitting (FS: deleting the singleton in LLAppViewer::cleanup() before
 *     the settings are saved): the "LLApp" event pump's "quitting";
 *   - the floater (FS: llviewerfloaterreg.cpp): LLFloaterReg::add() below.
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

#include "ao.h"

#include "aoengine.h"
#include "aofloater.h"
#include "plus.h"

#include "llagent.h"
#include "llapp.h"
#include "llcallbacklist.h"
#include "llevents.h"
#include "llfloaterreg.h"
#include "llstartup.h"
#include "lluictrl.h"
#include "llviewercontrol.h"

namespace
{
    constexpr char SETTING_ENABLED[] = "PlusAOEnabled"; // per account
    constexpr char SETTING_STANDS[]  = "PlusAOStands";  // per account

    bool onIdle()
    {
        if (LLStartUp::getStartupState() >= STATE_STARTED && !LLApp::isExiting())
        {
            // Made here on the first idle in-world.
            plus::ao::AOEngine::instance().onIdle();
        }
        return false; // keep running
    }

    bool getAccountBool(const char* name)
    {
        return gSavedPerAccountSettings.controlExists(name) && gSavedPerAccountSettings.getBOOL(name);
    }

    void toggleAccountBool(const char* name)
    {
        if (gSavedPerAccountSettings.controlExists(name))
        {
            gSavedPerAccountSettings.setBOOL(name, !gSavedPerAccountSettings.getBOOL(name));
        }
    }

    void init()
    {
        // Firestorm: llviewerfloaterreg.cpp
        plus::declareUIImage("Plus_AO_Icon", "plus/ao_18.png"); // toolbar button (app_settings/commands_plus.xml)
        LLFloaterReg::add(plus::ao::AOFloater::NAME, "floater_plus_ao.xml",
                          (LLFloaterBuildFunc)&LLFloaterReg::build<plus::ao::AOFloater>);

        // Menu item: skins/default/xui/<lang>/menu_plus.xml ("AnimationOverrider", the window); the switches:
        // panel_preferences_charis.xml (c_ao_enabled, c_ao_stands; plus/prefscharis reads Plus.AO.Is*).
        // Settings: app_settings/settings_plus.xml (PlusAO*; most of them per account).
        LLUICtrl::EnableCallbackRegistry::defaultRegistrar().add("Plus.AO.IsEnabled",
                                                                 [](LLUICtrl*, const LLSD&) { return getAccountBool(SETTING_ENABLED); });
        LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Plus.AO.ToggleEnabled",
                                                                 [](LLUICtrl*, const LLSD&) { toggleAccountBool(SETTING_ENABLED); });
        LLUICtrl::EnableCallbackRegistry::defaultRegistrar().add("Plus.AO.IsStands",
                                                                 [](LLUICtrl*, const LLSD&) { return getAccountBool(SETTING_STANDS); });
        LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Plus.AO.ToggleStands",
                                                                 [](LLUICtrl*, const LLSD&) { toggleAccountBool(SETTING_STANDS); });

        doOnIdleRepeating(&onIdle);

        // Remember the current set and animations before the settings are saved.
        LLEventPumps::instance().obtain("LLApp").listen("PlusAO", [](const LLSD& event)
        {
            if (event["status"].asString() == "quitting" && plus::ao::AOEngine::instanceExists())
            {
                plus::ao::AOEngine::instance().onQuitting();
            }
            return false;
        });
    }

    plus::FeatureRegistration sRegistration({ "ao", &init, {} });
}

namespace plus::ao
{
    bool overrideStart(const LLUUID& motion)
    {
        if (!AOEngine::instanceExists())
        {
            return false;
        }
        const LLUUID animation = AOEngine::instance().override(motion, true);
        if (animation.isNull())
        {
            return false;
        }
        gAgent.sendAnimationRequest(animation, ANIM_REQUEST_START);
        return true;
    }

    bool overrideStop(const LLUUID& motion)
    {
        if (!AOEngine::instanceExists())
        {
            return false;
        }
        const LLUUID animation = AOEngine::instance().override(motion, false);
        if (animation.isNull())
        {
            return false;
        }
        gAgent.sendAnimationRequest(animation, ANIM_REQUEST_STOP);
        return true;
    }
}

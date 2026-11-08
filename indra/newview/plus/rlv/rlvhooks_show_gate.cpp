/**
 * @file rlvhooks_show_gate.cpp
 * @brief rlv-show hooks: E1-E3,E5 llui floater gate, URL entries, commands.xml, rlvui_show.cpp (toggle handlers), F1, F5 (research/248). The code of every hook is FS' [RLVa:KB] block of the same function.
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

#include "rlvhooks.h"

#include "plus/plus.h"

#include "llavatarname.h"
#include "llfloaterreg.h"   // gPlusFloaterGate (E1)
#include "llfloatersidepanelcontainer.h"    // gPlusPanelGate (E2)
#include "lluictrl.h"
#include "llurlentry.h"     // gPlusUrlAnonymName (E5)

#include "rlvcommon.h"
#include "rlvhandler.h"
#include "rlvui.h"

namespace plus::rlv
{

namespace
{
    // ---- E1 / E2: the gates. They sit in the official code only while RlvUIEnabler has a filter (its gate hooks below).

    bool floaterGate(std::string_view strFloater, const LLSD& sdKey)
    {
        return (!RlvUIEnabler::instanceExists()) || (RlvUIEnabler::instance().canShowFloater(strFloater, sdKey));
    }

    bool panelGate(std::string_view strFloater, std::string_view strPanel, const LLSD& sdKey)
    {
        return (!RlvUIEnabler::instanceExists()) || (RlvUIEnabler::instance().canShowPanel(strFloater, strPanel, sdKey));
    }

    // ---- E3: "Plus.Rlv.CanShowFloater" of commands.xml (FS: "Floater.CanShow" -> LLFloaterReg::canShowInstance()).
    // A toolbar button asks it every frame: one bool with RLVa off, nothing more while no filter is in.
    bool canShowFloaterCommand(const LLSD& sdParam)
    {
        if ( (!RlvHandler::isEnabled()) || (!gPlusFloaterGate) )
            return true;
        return gPlusFloaterGate(sdParam.asStringRef(), LLSD());
    }

    // ---- E5: the name of a "rlvanonym" agent Url (FS: rlvGetAnonym()). With RLVa off it is the complete name.
    std::string urlAnonymName(const LLAvatarName& avName)
    {
        return (RlvHandler::isEnabled()) ? RlvStrings::getAnonym(avName) : avName.getCompleteName(true, true);
    }

    // Always registered (RLVa on or off), before the toolbars and text widgets are made
    void init()
    {
        gPlusUrlAnonymName = &urlAnonymName;
        LLUICtrl::EnableCallbackRegistry::defaultRegistrar().add("Plus.Rlv.CanShowFloater",
            [](LLUICtrl*, const LLSD& sdParam) { return canShowFloaterCommand(sdParam); });
    }

    plus::FeatureRegistration sRegistration({ "rlvshow", &init });
}

void registerShowToggles()
{
    static bool s_fRegistered = false;
    if ( (s_fRegistered) || (!RlvHandler::isEnabled()) )
        return;
    s_fRegistered = true;

    RlvUIEnabler& rlvUIEnabler = RlvUIEnabler::instance();
    rlvUIEnabler.setFloaterGateHook([](bool fInstall) { gPlusFloaterGate = (fInstall) ? &floaterGate : nullptr; });
    rlvUIEnabler.setPanelGateHook([](bool fInstall) { gPlusPanelGate = (fInstall) ? &panelGate : nullptr; });
    registerShowUIHandlers();
}

} // namespace plus::rlv

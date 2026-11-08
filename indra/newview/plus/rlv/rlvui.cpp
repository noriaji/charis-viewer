/**
 *
 * Copyright (c) 2009-2011, Kitty Barnett
 *
 * The source code in this file is provided to you under the terms of the
 * GNU Lesser General Public License, version 2.1, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 * PARTICULAR PURPOSE. Terms of the LGPL can be found in doc/LGPL-licence.txt
 * in this distribution, or online at http://www.gnu.org/licenses/lgpl-2.1.txt
 *
 * By copying, modifying or distributing this software, you acknowledge that
 * you have read and understood your obligations described above, and agree to
 * abide by those obligations.
 *
 */

#include "llviewerprecompiledheaders.h"
#include "llagent.h"
#include "llavataractions.h"            // LLAvatarActions::profileVisible()
#include "llfloaterreg.h"
#include "llimview.h"                   // LLIMMgr::computeSessionID()
#include "llparcel.h"
#include "llparcelselection.h"
#include "llviewerparcelmgr.h"
#include "llviewerregion.h"
#include "roles_constants.h"            // Group "powers"

#include "rlvui.h"
#include "rlvcommon.h"
#include "rlvhandler.h"
#include "rlvhelper.h"

// ============================================================================

// Checked: 2010-02-28 (RLVa-1.4.0a) | Added: RLVa-1.2.0a
RlvUIEnabler::RlvUIEnabler()
{
    // Connect us to the behaviour toggle signal
    m_ConnBehaviourToggle = gRlvHandler.setBehaviourToggleCallback(boost::bind(&RlvUIEnabler::onBehaviourToggle, this, _1, _2));
    // ... and to the exception signal (FS refreshes the UI from the individual behaviour handlers instead)
    m_ConnException = gRlvHandler.setExceptionCallback([this](ERlvBehaviour eBhvr, const LLUUID& idException, ERlvParamType) { onExceptionChange(eBhvr, idException); });

    // NOTE: FS registers every UI handler here. In Charis the owning branch registers them from its rlvui_<branch>.cpp:
    //         rlv-show: onRefreshHoverText (showloc, shownames), onToggleShowLoc, onToggleShowNames, onToggleShowMinimap,
    //                   onToggleShowWorldMap, onToggleTp (tploc, tplm)
    //         rlv-sit : onToggleUnsit
    //       (onToggleMovement, onToggleViewXXX and the @showhovertext* refresh belong to stage 3.)

    // The nearby name table (rlvnames.cpp) caches who is hidden; start over whenever that may have changed
    registerToggle(RLV_BHVR_SHOWNAMES, [](bool) { RlvNearbyNames::invalidate(); });
    registerExceptionChange(RLV_BHVR_SHOWNAMES, [](const LLUUID&) { RlvNearbyNames::invalidate(); });

    // onUpdateLoginLastLocation (tploc and unsit)
    const toggle_handler_t fnUpdateLoginLastLocation = [](bool fQuitting) { if (!fQuitting) gRlvHandler.scheduleLoginLastLocationUpdate(); };
    registerToggle(RLV_BHVR_TPLOC, fnUpdateLoginLastLocation);
    registerToggle(RLV_BHVR_UNSIT, fnUpdateLoginLastLocation);
}

RlvUIEnabler::~RlvUIEnabler()
{
    m_ConnBehaviourToggle.disconnect();
    m_ConnException.disconnect();
    for (auto& itConn : m_ConnModifiers)
        itConn.second.disconnect();
    m_ConnModifiers.clear();

    // Take the gates out of the official code before the filters they call go away
    if ( (m_fFloaterGateInstalled) && (m_FloaterGateHook) )
        m_FloaterGateHook(false);
    if ( (m_fPanelGateInstalled) && (m_PanelGateHook) )
        m_PanelGateHook(false);
    m_fFloaterGateInstalled = m_fPanelGateInstalled = false;
}

// ============================================================================
// Registration
//

void RlvUIEnabler::registerToggle(ERlvBehaviour eBhvr, const toggle_handler_t& fn)
{
    if (fn)
        m_Handlers.insert(std::make_pair(eBhvr, fn));
}

void RlvUIEnabler::registerExceptionChange(ERlvBehaviour eBhvr, const exception_handler_t& fn)
{
    if (fn)
        m_ExceptionHandlers.insert(std::make_pair(eBhvr, fn));
}

void RlvUIEnabler::registerModifierChange(ERlvBehaviourModifier eModifier, const modifier_handler_t& fn)
{
    if (!fn)
        return;
    m_ModifierHandlers.insert(std::make_pair(eModifier, fn));

    // Listen to the modifier itself once
    if (m_ConnModifiers.end() == m_ConnModifiers.find(eModifier))
    {
        if (RlvBehaviourModifier* pModifier = RlvBehaviourDictionary::instance().getModifier(eModifier))
            m_ConnModifiers[eModifier] = pModifier->getSignal().connect([this, eModifier](const RlvBehaviourModifierValue&) { onModifierChange(eModifier); });
    }
}

// ============================================================================
// Notifications
//

// Checked: 2010-02-28 (RLVa-1.4.0a) | Added: RLVa-1.2.0a
void RlvUIEnabler::onBehaviourToggle(ERlvBehaviour eBhvr, ERlvParamType eType)
{
    bool fQuitting = LLApp::isExiting();
    for (auto itHandler = m_Handlers.lower_bound(eBhvr), endHandler = m_Handlers.upper_bound(eBhvr); itHandler != endHandler; ++itHandler)
    {
        itHandler->second(fQuitting);
    }
}

void RlvUIEnabler::onExceptionChange(ERlvBehaviour eBhvr, const LLUUID& idException)
{
    if (LLApp::isExiting())
        return;
    for (auto itHandler = m_ExceptionHandlers.lower_bound(eBhvr), endHandler = m_ExceptionHandlers.upper_bound(eBhvr); itHandler != endHandler; ++itHandler)
    {
        itHandler->second(idException);
    }
}

void RlvUIEnabler::onModifierChange(ERlvBehaviourModifier eModifier)
{
    if (LLApp::isExiting())
        return;
    for (auto itHandler = m_ModifierHandlers.lower_bound(eModifier), endHandler = m_ModifierHandlers.upper_bound(eModifier); itHandler != endHandler; ++itHandler)
    {
        itHandler->second();
    }
}

// ============================================================================
// Floater and sidebar validation
//

#ifdef CATZNIP_STRINGVIEW
bool RlvUIEnabler::addGenericFloaterFilter(const std::string& strFloaterName, const std::string_view& strRlvNotification)
#else
bool RlvUIEnabler::addGenericFloaterFilter(const std::string& strFloaterName, const std::string& strRlvNotification)
#endif // CATZNIP_STRINGVIEW
{
    return addGenericFloaterFilter(strFloaterName, [strRlvNotification]() { RlvUtil::notifyBlocked(strRlvNotification); });
}

bool RlvUIEnabler::addGenericFloaterFilter(const std::string& strFloaterName, const std::function<void()>& fn)
{
    // NOTE: we don't currently support multiple filters for the same floater (due to the need to remove the correct one at the end of it all)
    if (m_FilteredFloaterMap.end() != m_FilteredFloaterMap.find(strFloaterName))
        return false;

    m_FilteredFloaterMap.insert(std::make_pair(strFloaterName, fn));
    updateFloaterGate();
    return true;
}

bool RlvUIEnabler::removeGenericFloaterFilter(const std::string& strFloaterName)
{
    auto itFloater = m_FilteredFloaterMap.find(strFloaterName);
    if (m_FilteredFloaterMap.end() == itFloater)
        return false;

    m_FilteredFloaterMap.erase(itFloater);
    updateFloaterGate();
    return true;
}

S32 RlvUIEnabler::addFloaterFilter(const floater_filter_t& fn)
{
    if (!fn)
        return 0;
    const S32 idFilter = m_nNextFilterId++;
    m_FloaterFilters[idFilter] = fn;
    updateFloaterGate();
    return idFilter;
}

void RlvUIEnabler::removeFloaterFilter(S32 idFilter)
{
    if (m_FloaterFilters.erase(idFilter))
        updateFloaterGate();
}

S32 RlvUIEnabler::addPanelFilter(const panel_filter_t& fn)
{
    if (!fn)
        return 0;
    const S32 idFilter = m_nNextFilterId++;
    m_PanelFilters[idFilter] = fn;
    updatePanelGate();
    return idFilter;
}

void RlvUIEnabler::removePanelFilter(S32 idFilter)
{
    if (m_PanelFilters.erase(idFilter))
        updatePanelGate();
}

bool RlvUIEnabler::canShowFloater(std::string_view strFloater, const LLSD& sdKey) const
{
    if (!RlvHandler::isEnabled())
        return true;

    // Checked: 2010-04-22 (RLVa-1.4.5) | Added: RLVa-1.2.0 (FS: filterFloaterGeneric)
    auto itFloater = m_FilteredFloaterMap.find(strFloater);
    if (m_FilteredFloaterMap.end() != itFloater)
    {
        if (itFloater->second)
            itFloater->second();
        return false;
    }

    for (const auto& itFilter : m_FloaterFilters)
    {
        if (!itFilter.second(strFloater, sdKey))
            return false;
    }
    return true;
}

bool RlvUIEnabler::canShowPanel(std::string_view strFloater, std::string_view strPanel, const LLSD& sdKey) const
{
    if (!RlvHandler::isEnabled())
        return true;

    for (const auto& itFilter : m_PanelFilters)
    {
        if (!itFilter.second(strFloater, strPanel, sdKey))
            return false;
    }
    return true;
}

void RlvUIEnabler::setFloaterGateHook(const gate_hook_t& fn)
{
    if ( (m_fFloaterGateInstalled) && (m_FloaterGateHook) )
        m_FloaterGateHook(false);
    m_fFloaterGateInstalled = false;
    m_FloaterGateHook = fn;
    updateFloaterGate();
}

void RlvUIEnabler::setPanelGateHook(const gate_hook_t& fn)
{
    if ( (m_fPanelGateInstalled) && (m_PanelGateHook) )
        m_PanelGateHook(false);
    m_fPanelGateInstalled = false;
    m_PanelGateHook = fn;
    updatePanelGate();
}

// The gate only sits in the official code while a filter needs it (no cost otherwise)
void RlvUIEnabler::updateFloaterGate()
{
    const bool fNeeded = (!m_FilteredFloaterMap.empty()) || (!m_FloaterFilters.empty());
    if ( (fNeeded != m_fFloaterGateInstalled) && (m_FloaterGateHook) )
    {
        m_FloaterGateHook(fNeeded);
        m_fFloaterGateInstalled = fNeeded;
    }
}

void RlvUIEnabler::updatePanelGate()
{
    const bool fNeeded = !m_PanelFilters.empty();
    if ( (fNeeded != m_fPanelGateInstalled) && (m_PanelGateHook) )
    {
        m_PanelGateHook(fNeeded);
        m_fPanelGateInstalled = fNeeded;
    }
}

// ============================================================================
// Helper functions
//

// Checked: 2012-02-09 (RLVa-1.4.5) | Modified: RLVa-1.4.5
bool RlvUIEnabler::canViewParcelProperties()
{
    // We'll allow "About Land" as long as the user has the ability to return prims (through ownership or through group powers)
    bool fShow = !gRlvHandler.hasBehaviour(RLV_BHVR_SHOWLOC);
    if (gRlvHandler.hasBehaviour(RLV_BHVR_SHOWLOC))
    {
        // RELEASE-RLVa: [SL-3.2] Check that opening the "About Land" floater still sets focus to the current parcel is none is selected
        const LLParcel* pParcel = NULL;
        if (LLViewerParcelMgr::getInstance()->selectionEmpty())
        {
            pParcel = LLViewerParcelMgr::getInstance()->getAgentParcel();
        }
        else
        {
            LLParcelSelection* pParcelSel = LLViewerParcelMgr::getInstance()->getFloatingParcelSelection();
            if ( (!pParcelSel) || (pParcelSel->hasOthersSelected()) )
                return false;
            pParcel = pParcelSel->getParcel();
        }

        // Ideally we could just use LLViewerParcelMgr::isParcelOwnedByAgent(), but that has that sneaky exemption for "god-like"
        if (pParcel)
        {
            const LLUUID& idOwner = pParcel->getOwnerID();
            if ( (idOwner != gAgent.getID()) )
            {
                auto count = gAgent.mGroups.size();
                for (size_t i = 0; i < count; ++i)
                {
                    if (gAgent.mGroups.at(i).mID == idOwner)
                    {
                        fShow = ((gAgent.mGroups.at(i).mPowers & GP_LAND_RETURN) > 0);
                        break;
                    }
                }
            }
            else
            {
                fShow = true;
            }
        }
    }
    return fShow;
}

// Checked: 2010-04-20 (RLVa-1.4.5) | Modified: RLVa-1.2.0
bool RlvUIEnabler::canViewRegionProperties()
{
    // We'll allow "Region / Estate" if the user is either the region owner or an estate manager
    bool fShow = !gRlvHandler.hasBehaviour(RLV_BHVR_SHOWLOC);
    if (gRlvHandler.hasBehaviour(RLV_BHVR_SHOWLOC))
    {
        // [See LLRegion::canManageEstate() but without the "god-like" exception]
        const LLViewerRegion* pRegion = gAgent.getRegion();
        if (pRegion)
            fShow = (pRegion->isEstateManager()) || (pRegion->getOwner() == gAgent.getID());
    }
    return fShow;
}

// Checked: 2010-04-20 (RLVa-1.2.0f) | Added: RLVa-1.2.0f
bool RlvUIEnabler::hasOpenIM(const LLUUID& idAgent)
{
    LLUUID idSession = LLIMMgr::computeSessionID(IM_NOTHING_SPECIAL, idAgent);
    // FS checks its own "fs_impanel"; Charis uses the official IM floater
    return (NULL != LLFloaterReg::findInstance("impanel", idSession));
}

// Checked: 2011-11-04 (RLVa-1.4.4a) | Modified: RLVa-1.4.4a
bool RlvUIEnabler::hasOpenProfile(const LLUUID& idAgent)
{
    //   -> SL-2.1.0 added the ability to "share" inventory by dropping it on the avatar picker floater so we should check for that
    //   -> we can drag/drop inventory onto calling cards but probably noone knows about it and it would be too involved to check for that
    // TODO-RLVa: [RLVa-1.2.1] Check the avatar picker as well

    // Check if the user has the specified agent's profile open
    return LLAvatarActions::profileVisible(idAgent);
}

// ============================================================================

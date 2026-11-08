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

#ifndef RLV_UI_H
#define RLV_UI_H

#include "llsd.h"
#include "llsingleton.h"
#include "lluuid.h"

#include "rlvdefines.h"

#include <boost/signals2.hpp>
#include <functional>
#include <map>
#include <string_view>

// ============================================================================
// RlvUIEnabler - self-contained class that handles disabling or reenabling certain aspects of the viewer's UI
//
// Charis keeps only the frame here (research/248 3.1): what used to be hard-wired in FS's constructor
// (m_Handlers.insert of every onToggle...) is registered by the branch that owns that part of the UI,
// from its own rlvui_<branch>.cpp:
//   - registerToggle()          : called whenever the behaviour is set for the first time or cleared for the last time
//   - registerExceptionChange() : called when an exception (e.g. @shownames:<uuid>=add/rem) changes
//   - registerModifierChange()  : called when a modifier's value changes (e.g. @shownametags:<distance>)
//   - floater / panel filters   : decide whether a floater (or side panel) may open. The official
//                                 LLFloaterReg gate is installed through setFloaterGateHook() /
//                                 setPanelGateHook() only while at least one filter exists.
// Toggles and exception changes arrive through RlvHandler's behaviour toggle and exception signals,
// modifier changes through the modifier's own change signal (connected on the first registration).
//

class RlvUIEnabler : public LLSingleton<RlvUIEnabler>
{
    LLSINGLETON(RlvUIEnabler);
    ~RlvUIEnabler() override;

protected:
    friend class RlvHandler;

public:
    typedef std::function<void(bool fQuitting)> toggle_handler_t;
    typedef std::function<void(const LLUUID& idException)> exception_handler_t;
    typedef std::function<void()> modifier_handler_t;

    // Returns false (and checks nothing) when the floater / panel may not open
    typedef std::function<bool(std::string_view strFloater, const LLSD& sdKey)> floater_filter_t;
    typedef std::function<bool(std::string_view strFloater, std::string_view strPanel, const LLSD& sdKey)> panel_filter_t;
    // Called with true when the first filter is added and with false when the last one goes
    typedef std::function<void(bool fInstall)> gate_hook_t;

    /*
     * Registration (rlvui_<branch>.cpp, once at startup)
     */
public:
    void registerToggle(ERlvBehaviour eBhvr, const toggle_handler_t& fn);
    void registerExceptionChange(ERlvBehaviour eBhvr, const exception_handler_t& fn);
    void registerModifierChange(ERlvBehaviourModifier eModifier, const modifier_handler_t& fn);

    /*
     * Notifications (rlvhandler.cpp)
     */
public:
    void onBehaviourToggle(ERlvBehaviour eBhvr, ERlvParamType eType);   // RlvHandler::rlv_behaviour_signal_t
    void onExceptionChange(ERlvBehaviour eBhvr, const LLUUID& idException);   // RlvHandler::rlv_exception_signal_t
    void onModifierChange(ERlvBehaviourModifier eModifier);

    /*
     * Floater and sidebar validation
     */
public:
#ifdef CATZNIP_STRINGVIEW
    bool addGenericFloaterFilter(const std::string& strFloaterName, const std::string_view& strRlvNotification);
#else
    bool addGenericFloaterFilter(const std::string& strFloaterName, const std::string& strRlvNotification);
#endif // CATZNIP_STRINGVIEW
    bool addGenericFloaterFilter(const std::string& strFloaterName, const std::function<void()>& fn = nullptr);
    bool removeGenericFloaterFilter(const std::string& strFloaterName);

    // Filters with their own logic (e.g. @showloc's "About Land"); the returned id removes it again
    S32  addFloaterFilter(const floater_filter_t& fn);
    void removeFloaterFilter(S32 idFilter);
    S32  addPanelFilter(const panel_filter_t& fn);
    void removePanelFilter(S32 idFilter);

    // The gate in the official floater / side panel code calls these
    bool canShowFloater(std::string_view strFloater, const LLSD& sdKey) const;
    bool canShowPanel(std::string_view strFloater, std::string_view strPanel, const LLSD& sdKey) const;

    // How the gate gets in and out of the official code (set by rlv-show)
    void setFloaterGateHook(const gate_hook_t& fn);
    void setPanelGateHook(const gate_hook_t& fn);

protected:
    void updateFloaterGate();
    void updatePanelGate();

    /*
     * Helper functions
     */
public:
    static bool canViewParcelProperties();                              // showloc
    static bool canViewRegionProperties();                              // showloc
    static bool hasOpenIM(const LLUUID& idAgent);                       // shownames
    static bool hasOpenProfile(const LLUUID& idAgent);                  // shownames

    /*
     * Member variables
     */
protected:
    std::multimap<ERlvBehaviour, toggle_handler_t>             m_Handlers;
    std::multimap<ERlvBehaviour, exception_handler_t>          m_ExceptionHandlers;
    std::multimap<ERlvBehaviourModifier, modifier_handler_t>   m_ModifierHandlers;
    boost::signals2::connection                                m_ConnBehaviourToggle;
    boost::signals2::connection                                m_ConnException;
    std::map<ERlvBehaviourModifier, boost::signals2::connection> m_ConnModifiers;

    std::map<std::string, std::function<void()>, std::less<>>  m_FilteredFloaterMap;
    std::map<S32, floater_filter_t>                            m_FloaterFilters;
    std::map<S32, panel_filter_t>                              m_PanelFilters;
    S32                                                        m_nNextFilterId = 1;

    gate_hook_t m_FloaterGateHook;
    gate_hook_t m_PanelGateHook;
    bool        m_fFloaterGateInstalled = false;
    bool        m_fPanelGateInstalled = false;
};

// ============================================================================

#endif // RLV_UI_H

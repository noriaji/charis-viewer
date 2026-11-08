/**
 * @file rlvui_show.cpp
 * @brief rlv-show's part of RlvUIEnabler: what @shownames, @shownametags, @showloc, @showminimap and @showworldmap change
 *        in the UI when they are switched on and off (research/248 3.1). FS: rlvui.cpp.
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

// FS' RlvUIEnabler::onRefreshHoverText(), onToggleShowLoc(), onToggleShowNames(), onToggleShowMinimap(),
// onToggleShowWorldMap(), onToggleTp(), filterFloaterShowLoc(), filterPanelShowLoc(), and the UI part of rlvhandler.cpp's
// @shownames / @shownametags handlers (RlvBehaviourToggleHandler<RLV_BHVR_SHOWNAMES>::onCommandToggle(),
// RlvBehaviourHandler<RLV_BHVR_SHOWNAMES>::onCommand(), RlvBehaviourModifierHandler<RLV_MODIFIER_SHOWNAMETAGSDIST>::onValueChange(),
// RlvBehaviourHandler<RLV_BHVR_SHOWNAMETAGS>::onCommand()). Registered with RlvUIEnabler by registerShowUIHandlers().
// Everything here runs when a behaviour is set for the first time or cleared for the last time (or an exception / the
// name tag distance changes): never per frame. The heavy refreshes (name lists, hover text, name tags) are only noted
// there and done once on the next idle, however many switches came in meanwhile (a relay may send hundreds a second).

#include "llviewerprecompiledheaders.h"

#include "rlvhooks.h"

#include "plus/radar/radar.h"

#include "llagent.h"
#include "llappviewer.h"                // gDoDisconnect
#include "llavatarlist.h"
#include "llavatarnamecache.h"
#include "llbutton.h"
#include "llcallbacklist.h"             // doOnIdleOneTime()
#include "llchatmentionhelper.h"
#include "llfloaterreg.h"
#include "llfloatersidepanelcontainer.h"
#include "llfloaterworldmap.h"
#include "lllocationinputctrl.h"
#include "llnavigationbar.h"
#include "llpanelplaces.h"
#include "llpaneltopinfobar.h"
#include "llteleporthistory.h"
#include "llteleporthistorystorage.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llvoavatar.h"

#include "rlvactions.h"
#include "rlvcommon.h"
#include "rlvhandler.h"
#include "rlvui.h"

#include <algorithm>

// The friend of LLTeleportHistory and LLTeleportHistoryStorage (<plus> marks in their headers): what FS' RlvUIEnabler does
// to the history when @showloc is switched on
struct PlusRlvTeleportHistory
{
    static void update(LLTeleportHistory* pHistory, const LLVector3d& posGlobal) { pHistory->updateCurrentLocation(posGlobal); }
    static void notifyRemoved(LLTeleportHistoryStorage* pStorage) { pStorage->mHistoryChangedSignal(-1); }
    // FS: pTpHistoryStg->compareByTitleAndGlobalPos() (FS makes RlvUIEnabler a friend)
    static bool isSamePlace(LLTeleportHistoryStorage* pStorage, const LLTeleportHistoryPersistentItem& itemStg, const LLTeleportHistoryItem& item)
    {
        return pStorage->compareByTitleAndGlobalPos(itemStg, LLTeleportHistoryPersistentItem(item.mTitle, item.mGlobalPos));
    }
};

namespace plus::rlv
{

namespace
{
    // The UI is left alone while the viewer quits or disconnects (FS: the @shownames toggle)
    bool isLeaving(bool fQuitting)
    {
        return (fQuitting) || (LLApp::isExiting()) || (gDoDisconnect);
    }

    // A floater / panel filter of RlvUIEnabler that is in while fOn (idFilter: 0 = not in)
    void setFloaterFilter(S32& idFilter, bool fOn, const RlvUIEnabler::floater_filter_t& fn)
    {
        RlvUIEnabler& rlvUIEnabler = RlvUIEnabler::instance();
        if ( (fOn) && (0 == idFilter) )
        {
            idFilter = rlvUIEnabler.addFloaterFilter(fn);
        }
        else if ( (!fOn) && (0 != idFilter) )
        {
            rlvUIEnabler.removeFloaterFilter(idFilter);
            idFilter = 0;
        }
    }

    void setPanelFilter(S32& idFilter, bool fOn, const RlvUIEnabler::panel_filter_t& fn)
    {
        RlvUIEnabler& rlvUIEnabler = RlvUIEnabler::instance();
        if ( (fOn) && (0 == idFilter) )
        {
            idFilter = rlvUIEnabler.addPanelFilter(fn);
        }
        else if ( (!fOn) && (0 != idFilter) )
        {
            rlvUIEnabler.removePanelFilter(idFilter);
            idFilter = 0;
        }
    }

    S32 s_idShowLocFloaterFilter = 0;
    S32 s_idShowLocPanelFilter = 0;

    // ========================================================================
    // Hover text (F5)
    //

    // FS: LLHUDText::refreshAllObjectText(). The original text of an object's hover text is the official
    // LLViewerObject::mHudText; setting it again sends it through the @shownames / @showloc filter of
    // LLHUDText::setString() (E20). Only the objects that have hover text are touched (the official LLHUDText keeps
    // its list of texts private, so the object list is walked; at most once per idle, see doPendingRefresh()).
    void refreshHoverText()
    {
        for (S32 idxObj = 0, cntObj = gObjectList.getNumObjects(); idxObj < cntObj; ++idxObj)
        {
            LLViewerObject* pObj = gObjectList.getObject(idxObj);
            if ( (pObj) && (!pObj->isDead()) && (!pObj->mHudText.empty()) )
                pObj->restoreHudText();
        }
    }

    // ========================================================================
    // @shownames
    //

    // The radar (plus/radar; FS: FSRadar::updateNames()) and the People panel's nearby list (RLVa:
    // LLAvatarList::updateAvatarNames(); handleDisplayNamesOptionChanged() is its public way, the names are made again
    // on the list's next draw). The voice speaker list FS also refreshes (fs_voice_controls) has no counterpart in the
    // official viewer.
    void refreshNameLists()
    {
        refreshPlusNames();                 // the radar and the area search (rlvhooks_show_plusfx.cpp)

        if (LLPanel* pPeoplePanel = LLFloaterSidePanelContainer::findPanel("people", "panel_people"))
        {
            LLPanel* pNearbyTab = pPeoplePanel->findChild<LLPanel>("nearby_panel");
            LLAvatarList* pNearbyList = (pNearbyTab) ? pNearbyTab->findChild<LLAvatarList>("avatar_list") : nullptr;
            if (pNearbyList)
                pNearbyList->handleDisplayNamesOptionChanged();
        }
    }

    // F1 (FS: LLAvatarNameCache::setForceDisplayNames()): while @shownames is in, the name cache uses display names so both
    // the display and the legacy names can be filtered. When the user turns UseDisplayNames off meanwhile, the preferences'
    // own handler (connected whenever the preferences floater is built, maybe after us) sets the cache: it is put back
    // after the change is done. Off again: the user's setting.
    boost::signals2::connection s_ConnUseDisplayNames;

    void forceDisplayNames(bool fForce)
    {
        LLControlVariable* pControl = gSavedSettings.getControl("UseDisplayNames");
        if (fForce)
        {
            if ( (pControl) && (!s_ConnUseDisplayNames.connected()) )
            {
                s_ConnUseDisplayNames = pControl->getSignal()->connect([](LLControlVariable*, const LLSD& sdNewValue, const LLSD&)
                {
                    if (sdNewValue.asBoolean())
                        return;
                    doOnIdleOneTime([]()
                    {
                        if ( (!LLApp::isExiting()) && (RlvHandler::isEnabled()) && (gRlvHandler.hasBehaviour(RLV_BHVR_SHOWNAMES)) )
                            LLAvatarNameCache::getInstance()->setUseDisplayNames(true);
                    });
                });
            }
            LLAvatarNameCache::getInstance()->setUseDisplayNames(true);
        }
        else
        {
            s_ConnUseDisplayNames.disconnect();
            LLAvatarNameCache::getInstance()->setUseDisplayNames((pControl) ? pControl->getValue().asBoolean() : true);
        }
    }

    // ========================================================================
    // The refreshes, once per idle
    //
    // FS refreshes everything from each toggle and each exception. A relay that switches @shownames:<uuid>=add/rem back
    // and forth would make that hundreds of walks over the objects a second: the handlers only note what is to be
    // refreshed, and the next idle does it once.

    struct PendingRefresh
    {
        bool       fScheduled = false;
        bool       fDisplayNames = false;   // the name cache goes back to UseDisplayNames (if @shownames is still off by then)
        bool       fNameLists = false;      // refreshNameLists()
        bool       fHoverText = false;      // refreshHoverText()
        bool       fAllNameTags = false;    // every name tag (idNameTags is then empty)
        uuid_set_t idNameTags;              // the agents whose name tag is refreshed
    };
    PendingRefresh s_Pending;

    // More agents than this and every name tag is refreshed instead (also keeps the set small whatever a relay sends)
    constexpr size_t MAX_PENDING_NAME_TAGS = 16;

    void doPendingRefresh()
    {
        // Take the notes first: whatever is asked for while these run goes to the next idle
        PendingRefresh pending;
        std::swap(pending, s_Pending);
        if (isLeaving(false))
            return;

        if ( (pending.fDisplayNames) && (!gRlvHandler.hasBehaviour(RLV_BHVR_SHOWNAMES)) )
            forceDisplayNames(false);
        if (pending.fNameLists)
        {
            refreshNameLists();
            refreshConversationNames();     // Conversations' nearby participants (rlvhooks_show_list.cpp)
        }
        if (pending.fAllNameTags)
        {
            LLVOAvatar::invalidateNameTags();
        }
        else
        {
            for (const LLUUID& idAgent : pending.idNameTags)
                LLVOAvatar::invalidateNameTag(idAgent);
        }
        if (pending.fHoverText)
            refreshHoverText();
    }

    void scheduleRefresh()
    {
        if (!s_Pending.fScheduled)
        {
            s_Pending.fScheduled = true;
            doOnIdleOneTime(&doPendingRefresh);
        }
    }

    void refreshLater(bool fNameLists, bool fHoverText)
    {
        s_Pending.fNameLists |= fNameLists;
        s_Pending.fHoverText |= fHoverText;
        scheduleRefresh();
    }

    void refreshAllNameTagsLater()
    {
        s_Pending.fAllNameTags = true;
        s_Pending.idNameTags.clear();
        scheduleRefresh();
    }

    void refreshNameTagLater(const LLUUID& idAgent)
    {
        if ( (idAgent.isNull()) || (s_Pending.fAllNameTags) )
            return;
        if ( (s_Pending.idNameTags.size() >= MAX_PENDING_NAME_TAGS) && (!s_Pending.idNameTags.count(idAgent)) )
        {
            refreshAllNameTagsLater();
            return;
        }
        s_Pending.idNameTags.insert(idAgent);
        scheduleRefresh();
    }

    // Checked: 2010-03-02 (RLVa-1.4.0a) | Added: RLVa-1.2.0a (FS: RlvUIEnabler::onRefreshHoverText())
    void onRefreshHoverText(bool fQuitting)
    {
        if (!isLeaving(fQuitting))
            refreshLater(false, true);
    }

    // Checked: 2026-02-12 (RLVa-2.4.2) (FS: RlvUIEnabler::onToggleShowNames() and the rest of
    // RlvBehaviourToggleHandler<RLV_BHVR_SHOWNAMES>::onCommandToggle(); RlvActions::setShowName() stays in rlvhandler.cpp)
    void onToggleShowNames(bool fQuitting)
    {
        if (isLeaving(fQuitting))
            return;

        if (gRlvHandler.hasBehaviour(RLV_BHVR_SHOWNAMES))
        {
            if (LLChatMentionHelper::instanceExists())
                LLChatMentionHelper::instance().hideHelper();
            // Force the use of the "display name" cache at once so we can filter both display and legacy names
            forceDisplayNames(true);
        }
        else
        {
            // ... and return back to the user's preference on the next idle
            s_Pending.fDisplayNames = true;
        }

        // Refresh the nearby people lists, all name tags and HUD text
        refreshLater(true, true);
        refreshAllNameTagsLater();
    }

    // FS: RlvBehaviourHandler<RLV_BHVR_SHOWNAMES>::onCommand() with an exception (@shownames:<uuid>=n|y)
    void onShowNamesException(const LLUUID& idAgent)
    {
        if (isLeaving(false))
            return;
        refreshLater(true, true);
        refreshNameTagLater(idAgent);
    }

    // ========================================================================
    // @shownametags
    //

    // FS: RlvBehaviourModifierHandler<RLV_MODIFIER_SHOWNAMETAGSDIST>::onValueChange(). The plain toggle refreshes the name
    // tags as well (FS leaves that to the name tags' own updates).
    void onToggleShowNameTags(bool fQuitting)
    {
        if (!isLeaving(fQuitting))
            refreshAllNameTagsLater();
    }

    // FS: RlvBehaviourHandler<RLV_BHVR_SHOWNAMETAGS>::onCommand() with an exception (@shownametags:<uuid>=n|y)
    void onShowNameTagsException(const LLUUID& idAgent)
    {
        if (!isLeaving(false))
            refreshNameTagLater(idAgent);
    }

    // ========================================================================
    // @showloc
    //

    // Checked: 2010-04-22 (RLVa-1.4.5) | Added: RLVa-1.2.0 (FS: RlvUIEnabler::filterFloaterShowLoc())
    bool filterFloaterShowLoc(std::string_view strName, const LLSD&)
    {
        if ("about_land" == strName)
            return RlvUIEnabler::canViewParcelProperties();
        else if ("region_info" == strName)
            return RlvUIEnabler::canViewRegionProperties();
        else if ("god_tools" == strName)
            return false;
        else if ("add_landmark" == strName)
            return false;   // Charis: the landmark it makes shows the current place (the panel filter's "create_landmark" as a floater)
        return true;
    }

    // Checked: 2012-02-07 (RLVa-1.4.5) | Added: RLVa-1.4.5 (FS: RlvUIEnabler::filterPanelShowLoc())
    bool filterPanelShowLoc(std::string_view strFloater, std::string_view, const LLSD& sdKey)
    {
        if ( ("places" == strFloater) && (sdKey.isMap()) )
        {
            const std::string strType = sdKey["type"].asString();
            if ("create_landmark" == strType)
                return false;
            else if ("agent" == strType)
                return RlvUIEnabler::canViewParcelProperties();
        }
        return true;
    }

    // Checked: 2010-04-22 (RLVa-1.2.0f) | Modified: RLVa-1.2.0f (FS: RlvUIEnabler::onToggleShowLoc())
    void onToggleShowLoc(bool fQuitting)
    {
        if (isLeaving(fQuitting))
            return;

        const bool fEnable = !gRlvHandler.hasBehaviour(RLV_BHVR_SHOWLOC);

        // The location of the navigation bar and of the top info bar (FS: LLNavigationBar::refreshLocationCtrl(); RLVa:
        // LLPanelTopInfoBar::update()). handleLoginComplete() is the public way to their refresh() / update().
        if (LLNavigationBar::instanceExists())
        {
            if (LLLocationInputCtrl* pLocationCtrl = LLNavigationBar::instance().findChild<LLLocationInputCtrl>("location_combo"))
                pLocationCtrl->handleLoginComplete();
        }
        if (LLPanelTopInfoBar::instanceExists())
            LLPanelTopInfoBar::instance().handleLoginComplete();
        // Charis: the open world map's coordinates and agent's region name (set when it opened) follow at once (research/275 L104)
        if (LLFloaterWorldMap* pWorldMap = LLFloaterReg::findTypedInstance<LLFloaterWorldMap>("world_map"))
            pWorldMap->plusRefreshShowLoc();

        if (!fEnable)
        {
            // Hide the "About Land" floater if it's currently visible
            if (LLFloaterReg::instanceVisible("about_land"))
                LLFloaterReg::hideInstance("about_land");
            // Hide the "Region / Estate" floater if it's currently visible
            if (LLFloaterReg::instanceVisible("region_info"))
                LLFloaterReg::hideInstance("region_info");
            // Hide the "God Tools" floater if it's currently visible
            if (LLFloaterReg::instanceVisible("god_tools"))
                LLFloaterReg::hideInstance("god_tools");
            // Charis: the "Add Landmark" floater as well
            if (LLFloaterReg::instanceVisible("add_landmark"))
                LLFloaterReg::hideInstance("add_landmark");
            // Charis: the Places panel left at the agent's place goes back to its list (open or not: an open with no key would
            // resume it, and while open it follows the agent's parcel). LLPanelPlaces::onOpen() does it (E31).
            LLPanelPlaces* pPlacesPanel = LLFloaterSidePanelContainer::findPanel<LLPanelPlaces>("places");
            if ( (pPlacesPanel) && ("agent" == pPlacesPanel->getPlaceInfoType()) && (!RlvUIEnabler::canViewParcelProperties()) )
                pPlacesPanel->onOpen(LLSD());

            //
            // Manipulate the teleport history
            //

            // If the last entry in the persistent teleport history matches the current teleport history entry then we should remove it
            LLTeleportHistory* pTpHistory = LLTeleportHistory::getInstance();
            LLTeleportHistoryStorage* pTpHistoryStg = LLTeleportHistoryStorage::getInstance();
            if ( (pTpHistory) && (pTpHistory->getItems().size() > 0) && (pTpHistory->getCurrentItemIndex() >= 0) &&
                 (pTpHistoryStg) && (pTpHistoryStg->getItems().size() > 0) )
            {
                const LLTeleportHistoryItem& tpItem = pTpHistory->getItems().back();
                const LLTeleportHistoryPersistentItem& tpItemStg = pTpHistoryStg->getItems().back();
                if (PlusRlvTeleportHistory::isSamePlace(pTpHistoryStg, tpItemStg, tpItem))
                {
                    pTpHistoryStg->removeItem(static_cast<S32>(pTpHistoryStg->getItems().size()) - 1);
                    PlusRlvTeleportHistory::notifyRemoved(pTpHistoryStg);
                }
            }

            // Clear the current location in the teleport history
            if (pTpHistory)
                PlusRlvTeleportHistory::update(pTpHistory, gAgent.getPositionGlobal());
        }
        else
        {
            // Reset the current location in the teleport history (also takes care of adding it to the persistent teleport history)
            LLTeleportHistory* pTpHistory = LLTeleportHistory::getInstance();
            if ( (pTpHistory) && (NULL != gAgent.getRegion()) )
                PlusRlvTeleportHistory::update(pTpHistory, gAgent.getPositionGlobal());
        }

        // Start or stop filtering the "About Land" and "Region / Estate" floaters
        setFloaterFilter(s_idShowLocFloaterFilter, !fEnable, &filterFloaterShowLoc);
        setPanelFilter(s_idShowLocPanelFilter, !fEnable, &filterPanelShowLoc);

        // The place buttons of the open profiles' picks and classifieds (E33, rlvhooks_show_panel2.cpp)
        updateProfilePlaceButtons();
    }

    // ========================================================================
    // @showminimap / @showworldmap
    //

    // Checked: 2010-02-28 (RLVa-1.4.0a) | Added: RLVa-1.2.0a (FS: RlvUIEnabler::onToggleShowMinimap())
    void onToggleShowMinimap(bool fQuitting)
    {
        if (isLeaving(fQuitting))
            return;

        const bool fEnable = !gRlvHandler.hasBehaviour(RLV_BHVR_SHOWMINIMAP);

        // Start or stop filtering showing the mini-map floater
        RlvUIEnabler& rlvUIEnabler = RlvUIEnabler::instance();
        if (!fEnable)
        {
            RLV_VERIFY(rlvUIEnabler.addGenericFloaterFilter("mini_map"));
        }
        else
        {
            RLV_VERIFY(rlvUIEnabler.removeGenericFloaterFilter("mini_map"));
        }

        // Hide the mini-map floater if it's currently visible (or restore it if it was previously visible)
        static bool s_fPrevVisible = false;
        if ( (!fEnable) && ((s_fPrevVisible = LLFloaterReg::instanceVisible("mini_map"))) )
            LLFloaterReg::hideInstance("mini_map");
        else if ( (fEnable) && (s_fPrevVisible) )
            LLFloaterReg::showInstance("mini_map");

        // Break/reestablish the visibility connection for the nearby people panel embedded minimap instance
        // (FS: the "minimaplayout" panel and ShowRadarMinimap; the official layout panel has no name: the minimap's parent,
        // shown by NearbyListShowMap). Plus/radar's window has no minimap.
        LLPanel* pPeoplePanel = LLFloaterSidePanelContainer::getPanel("people", "panel_people");
        LLView* pNetMap = (pPeoplePanel) ? pPeoplePanel->findChild<LLView>("Net Map") : nullptr;
        LLUICtrl* pNetMapPanel = (pNetMap) ? dynamic_cast<LLUICtrl*>(pNetMap->getParent()) : nullptr;
        RLV_ASSERT( (pPeoplePanel) && (pNetMapPanel) );
        if (pNetMapPanel)
        {
            pNetMapPanel->setMakeVisibleControlVariable( (fEnable) ? gSavedSettings.getControl("NearbyListShowMap").get() : nullptr );
            // Reestablishing the visiblity connection will show the panel if needed so we only need to take care of hiding it when needed
            if ( (!fEnable) && (pNetMapPanel->getVisible()) )
                pNetMapPanel->setVisible(false);
        }
    }

    // Checked: 2010-02-28 (RLVa-1.4.0a) | Added: RLVa-1.2.0a (FS: RlvUIEnabler::onToggleShowWorldMap())
    void onToggleShowWorldMap(bool fQuitting)
    {
        if (isLeaving(fQuitting))
            return;

        const bool fEnable = !gRlvHandler.hasBehaviour(RLV_BHVR_SHOWWORLDMAP);

        // Hide the world map if it's currently visible
        if ( (!fEnable) && (LLFloaterReg::instanceVisible("world_map")) )
            LLFloaterReg::hideInstance("world_map");

        // Start or stop filtering opening the world map
        RlvUIEnabler& rlvUIEnabler = RlvUIEnabler::instance();
        if (!fEnable)
        {
            RLV_VERIFY(rlvUIEnabler.addGenericFloaterFilter("world_map"));
        }
        else
        {
            RLV_VERIFY(rlvUIEnabler.removeGenericFloaterFilter("world_map"));
        }
    }

    // ========================================================================
    // @tplm / @tploc
    //

    // Checked: 2010-08-22 (RLVa-1.2.1a) | Added: RLVa-1.2.1a (FS: RlvUIEnabler::onToggleTp())
    void onToggleTp(bool fQuitting)
    {
        if ( (isLeaving(fQuitting)) || (!LLNavigationBar::instanceExists()) )
            return;

        // Disable the navigation bar "Home" button if both @tplm=n *and* @tploc=n restricted
        LLButton* pNavBarHomeBtn = LLNavigationBar::instance().findChild<LLButton>("home_btn");
        RLV_ASSERT(pNavBarHomeBtn);
        if (pNavBarHomeBtn)
            pNavBarHomeBtn->setEnabled(!(gRlvHandler.hasBehaviour(RLV_BHVR_TPLM) && gRlvHandler.hasBehaviour(RLV_BHVR_TPLOC)));
    }
}

void registerShowUIHandlers()
{
    RlvUIEnabler& rlvUIEnabler = RlvUIEnabler::instance();

    // onRefreshHoverText() (@showhovertext* is stage 3)
    rlvUIEnabler.registerToggle(RLV_BHVR_SHOWLOC, &onRefreshHoverText);

    // onToggleXXX
    rlvUIEnabler.registerToggle(RLV_BHVR_SHOWLOC, &onToggleShowLoc);
    rlvUIEnabler.registerToggle(RLV_BHVR_SHOWNAMES, &onToggleShowNames);
    rlvUIEnabler.registerToggle(RLV_BHVR_SHOWMINIMAP, &onToggleShowMinimap);
    rlvUIEnabler.registerToggle(RLV_BHVR_SHOWWORLDMAP, &onToggleShowWorldMap);

    // onToggleTp
    rlvUIEnabler.registerToggle(RLV_BHVR_TPLOC, &onToggleTp);
    rlvUIEnabler.registerToggle(RLV_BHVR_TPLM, &onToggleTp);

    // The UI part of rlvhandler.cpp's @shownames / @shownametags handlers
    rlvUIEnabler.registerExceptionChange(RLV_BHVR_SHOWNAMES, &onShowNamesException);
    rlvUIEnabler.registerToggle(RLV_BHVR_SHOWNAMETAGS, &onToggleShowNameTags);
    rlvUIEnabler.registerExceptionChange(RLV_BHVR_SHOWNAMETAGS, &onShowNameTagsException);
    rlvUIEnabler.registerModifierChange(RLV_MODIFIER_SHOWNAMETAGSDIST, []() { if (!isLeaving(false)) refreshAllNameTagsLater(); });
}

} // namespace plus::rlv

/**
 * @file uiscope.cpp
 * @brief Hiding the viewer's UI for a while and putting back exactly what was hidden
 *        (research/365 in the sl-viewer repository, "the common part").
 *
 * What it keeps, one by one: every view it hid (by handle: one that is gone by the time it lets go is skipped) and
 * the floaters (also by handle, and dropped when the floater is closed meanwhile). What it counts, over all
 * the Scopes that are on: the toolbars' flag (one flag in the official code), the HUD's value (one flag) and the
 * name tags' and hover text's values (one flag each).
 *
 * Speed: made and let go once; a tick is a few bool reads and getVisible() calls, no allocation (the list grows
 * only when something is hidden, one entry per kind of bar).
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

#include "uiscope.h"

#include "plus/fastexit/exittime.h"
#include "plus/rlv/rlvhooks.h"

#include <algorithm>
#include <utility>

#include "llagentcamera.h"
#include "llfloater.h"
#include "llmoveview.h"
#include "llnavigationbar.h"
#include "llpanel.h"
#include "llpaneltopinfobar.h"
#include "llstatusbar.h"
#include "lltoolbarview.h"
#include "llviewercontrol.h"
#include "llhudnametag.h"
#include "llhudtext.h"
#include "llviewermenu.h"
#include "llviewerwindow.h"
#include "pipeline.h"

extern LLFloaterView* gPlusDetachedFloaterView;

namespace plus::uiscope
{
    namespace
    {
        constexpr char SETTING_HIDE_UI[]    = "HideUIControls";
        constexpr char STAND_STOP_NAME[]    = "state_management_buttons_container";

        std::vector<Scope*> sActive;            // the Scopes that are on, the oldest first
        S32                 sToolbarHolders = 0; // Scopes that hold the toolbars' flag hidden
        S32                 sHudHolders     = 0; // Hud::Hide Scopes that hide the HUD now
        bool                sHudOriginal    = true;  // what the HUD was before the first of them
        S32                 sTagHolders        = 0;     // Tags::Hide Scopes that hide the name tags and the hover text now
        bool                sNameTagOriginal   = true;  // LLHUDNameTag::sDisplayText before the first of them
        bool                sHoverTextOriginal = true;  // LLHUDText::sDisplayText before the first of them
        bool                sQuitHooked     = false;
        U32                 sSerial         = 0;     // the next Scope's mSerial

        // What View > Hide UI does back (LLViewToggleUI::confirm()), without its question.
        void undoHideUiControls()
        {
            if (gSavedSettings.getBOOL(SETTING_HIDE_UI))
            {
                gViewerWindow->setUIVisibility(true);
                LLPanelStandStopFlying::getInstance()->setVisible(true);
                gSavedSettings.setBOOL(SETTING_HIDE_UI, false);
            }
        }

        bool rlvLocksHud()
        {
            return plus::rlv::enabled() && plus::rlv::hasLockedHUD();
        }

        // The container of the stand / fly buttons (a child of the toolbar view); looked up once.
        LLView* standStopContainer()
        {
            static LLHandle<LLView> sContainer;
            if (!sContainer.get() && gToolBarView)
            {
                if (LLPanel* panel = gToolBarView->findChild<LLPanel>(STAND_STOP_NAME))
                {
                    sContainer = panel->getHandle();
                }
            }
            return sContainer.get();
        }
    }

    Scope::Scope(const Options& options) : mOptions(options), mHudSaved(sHudHolders > 0 ? sHudOriginal : LLPipeline::sShowHUDAttachments), mSerial(++sSerial)
    {
        if (!sQuitHooked)
        {
            sQuitHooked = true;
            plus::exittime::addQuitObserver(&releaseAll);
        }
        sActive.push_back(this);
        if (mOptions.ownsHideUi)
        {
            undoHideUiControls();
        }
        mUiVisible = !gViewerWindow || gViewerWindow->getUIVisibility();
        holdSingles();
        holdToolbars();
        hideFloaters(gFloaterView, Kind::Floater);
        hideFloaters(gPlusDetachedFloaterView, Kind::Detached);
        holdHud();
        holdTags();
    }

    Scope::~Scope()
    {
        release();
    }

    LLView* Scope::viewFor(Kind kind) const
    {
        switch (kind)
        {
        case Kind::Menu:      return gMenuBarView;
        case Kind::Nav:       return LLNavigationBar::getInstance();
        case Kind::TopInfo:   return LLPanelTopInfoBar::getInstance();
        case Kind::Status:    return gStatusBar ? gStatusBar->getParent() : nullptr;  // the container, as setUIVisibility()
        case Kind::Chiclet:   return gViewerWindow ? gViewerWindow->getChicletContainer() : nullptr;
        case Kind::StandStop: return standStopContainer();
        default:              return nullptr;
        }
    }

    bool Scope::hidesKind(Kind kind) const
    {
        return kind != Kind::StandStop || mOptions.standStop;
    }

    // Whether this, staying on when the Scope made at 'leaverSerial' lets go, takes over what that hid. Windows: only
    // when this was made later (it hides every window shown now, the leaver's among them), or hides windows
    // again by itself (follows View > Hide UI). One that hid windows only when it was made (the photo screen) is
    // the older: what is opened in it since is the user's, and shown when the path in it ends.
    bool Scope::wants(const Held& held, U32 leaverSerial) const
    {
        if (held.kind == Kind::Floater || held.kind == Kind::Detached)
        {
            if (mSerial < leaverSerial && mOptions.windowsOnlyAtEntry)
            {
                return false;
            }
            const LLFloater* floater = dynamic_cast<const LLFloater*>(held.view.get());
            return floater && !(mOptions.staysShown && mOptions.staysShown(floater));
        }
        return hidesKind(held.kind);
    }

    void Scope::hookClose(Held& held)
    {
        if (held.kind != Kind::Floater && held.kind != Kind::Detached)
        {
            return;
        }
        if (LLFloater* floater = dynamic_cast<LLFloater*>(held.view.get()))
        {
            const LLView* raw = held.raw;
            // Closed while hidden (Ctrl+I twice, hideInstance): not shown again at the end, as
            // LLFloaterView::hideAllFloaters() does for its list.
            held.closed = floater->setCloseCallback([this, raw](LLUICtrl*, const LLSD&) { forgetClosed(raw); });
        }
    }

    // An entry for 'view' whose window is still the one (a gone window's address may be used again).
    bool Scope::isEntryOf(const Held& held, const LLView* view)
    {
        return held.raw == view && held.view.get() == view;
    }

    void Scope::forgetClosed(const LLView* raw)
    {
        auto it = std::find_if(mHeld.begin(), mHeld.end(), [raw](const Held& held) { return isEntryOf(held, raw); });
        if (it != mHeld.end())
        {
            it->closed.disconnect();
            mHeld.erase(it);
        }
    }

    void Scope::adopt(Held&& held)
    {
        if (isHeld(held.raw))
        {
            return;     // already held here: it stays hidden by this one
        }
        mHeld.push_back(std::move(held));
        hookClose(mHeld.back());
    }

    bool Scope::isHeld(const LLView* raw) const
    {
        return std::any_of(mHeld.begin(), mHeld.end(), [raw](const Held& held) { return isEntryOf(held, raw); });
    }

    bool Scope::holds(const LLView* view) const
    {
        return view && isHeld(view);
    }

    // One kind of bar: hidden if it is shown; kept once (a bar the official code shows again is hidden again, not listed again).
    void Scope::holdSingle(Kind kind)
    {
        LLView* view = viewFor(kind);
        if (!view || !view->getVisible())
        {
            return;
        }
        if (!isHeld(view))
        {
            Held held;
            held.view = view->getHandle();
            held.raw  = view;
            held.kind = kind;
            mHeld.push_back(std::move(held));
        }
        view->setVisible(false);
    }

    // The bars and the menu: every frame, as the official code may show them again (View > Hide UI turned off,
    // a setting that shows the navigation bar, the avatar sitting down). What is not shown is left alone.
    void Scope::holdSingles()
    {
        for (Kind kind : { Kind::Menu, Kind::Nav, Kind::TopInfo, Kind::Status, Kind::Chiclet, Kind::StandStop })
        {
            if (hidesKind(kind))
            {
                holdSingle(kind);
            }
        }
    }

    // The toolbars have one flag (LLToolBarView::setToolBarsVisible()) and no getter: held by count over the Scopes, and
    // never held while View > Hide UI has them hidden (it shows them itself when it is turned off).
    void Scope::holdToolbars()
    {
        if (!gToolBarView || !mUiVisible)
        {
            return;
        }
        if (!mToolbars)
        {
            mToolbars = true;
            ++sToolbarHolders;
        }
        gToolBarView->setToolBarsVisible(false);    // the same every frame (leaving mouselook sets it true)
    }

    void Scope::releaseToolbars()
    {
        if (!mToolbars)
        {
            return;
        }
        mToolbars = false;
        if (--sToolbarHolders > 0)
        {
            return;
        }
        sToolbarHolders = 0;
        // Not while View > Hide UI has them hidden (it shows them when it is turned off), nor in mouselook (the
        // official code shows them when it is left).
        if (gToolBarView && gViewerWindow && gViewerWindow->getUIVisibility() && !gAgentCamera.cameraMouselook())
        {
            gToolBarView->setToolBarsVisible(true);
        }
    }

    void Scope::holdHud()
    {
        if (mOptions.hud == Hud::Hide)
        {
            if (mHudHidden || rlvLocksHud())     // RLVa: a locked HUD cannot be hidden (research/248 W10)
            {
                return;
            }
            mHudHidden = true;
            if (sHudHolders++ == 0)
            {
                sHudOriginal = LLPipeline::sShowHUDAttachments;
            }
            LLPipeline::sShowHUDAttachments = false;
        }
    }

    void Scope::releaseHud()
    {
        if (mOptions.hud == Hud::Hide)
        {
            if (!mHudHidden)
            {
                return;
            }
            mHudHidden = false;
            if (--sHudHolders <= 0)
            {
                sHudHolders = 0;
                // RLVa: a HUD locked on while it was hidden is not left hidden
                LLPipeline::sShowHUDAttachments = sHudOriginal || rlvLocksHud();
            }
        }
        else if (mOptions.hud == Hud::Restore)
        {
            if (sHudHolders > 0)
            {
                // another Scope hides it now: it shows it again with the value from before it, which is this one's
                // (made while it was hidden, this took the original: see the constructor)
                sHudOriginal = mHudSaved;
            }
            else
            {
                LLPipeline::sShowHUDAttachments = mHudSaved || rlvLocksHud();
            }
        }
    }

    // The name tags and the hover text: two flags in the official code (nothing else in the viewer writes them), held by
    // count over the Scopes like the HUD's: the first holder takes what they were, the last one puts that back. Not done
    // again every frame (nothing turns them on by itself).
    void Scope::holdTags()
    {
        if (mOptions.tags != Tags::Hide || mTagsHidden)
        {
            return;
        }
        mTagsHidden = true;
        if (sTagHolders++ == 0)
        {
            sNameTagOriginal   = LLHUDNameTag::getDisplayText();
            sHoverTextOriginal = LLHUDText::getDisplayText();
        }
        LLHUDNameTag::setDisplayText(false);
        LLHUDText::setDisplayText(false);
    }

    void Scope::releaseTags()
    {
        if (!mTagsHidden)
        {
            return;
        }
        mTagsHidden = false;
        if (--sTagHolders <= 0)
        {
            sTagHolders = 0;
            LLHUDNameTag::setDisplayText(sNameTagOriginal);
            LLHUDText::setDisplayText(sHoverTextOriginal);
        }
    }

    // Once, at the entry (and when View > Hide UI is turned off while this follows it): a floater that opens
    // in here is the user's to see.
    void Scope::hideFloaters(LLView* parent, Kind kind)
    {
        if (!parent)
        {
            return;
        }
        const LLView::child_list_t children = *parent->getChildList();    // setVisible() may call back
        for (LLView* child : children)
        {
            LLFloater* floater = dynamic_cast<LLFloater*>(child);
            if (!floater || !floater->getVisible() || (mOptions.staysShown && mOptions.staysShown(floater)))
            {
                continue;
            }
            if (!isHeld(floater))
            {
                Held held;
                held.view = static_cast<LLView*>(floater)->getHandle();
                held.raw  = floater;
                held.kind = kind;
                mHeld.push_back(std::move(held));
                hookClose(mHeld.back());
            }
            floater->setVisible(false);
        }
    }

    // View > Hide UI pressed while this is on (a Scope that does not own it): the official code has hidden the bars and
    // the toolbars and put the floaters it found on its own list, so what this holds of those is left to it: it
    // shows them again when it is turned off, and it must not be shown here. The floaters are shown and handed to
    // its list (hideAllFloaters()); the menu bar, the chiclet bar and the detached windows are not its business: kept.
    void Scope::handOverToHideUi()
    {
        std::vector<Held> all;
        all.swap(mHeld);    // setVisible() below may close another window: nothing is walked that can change
        std::vector<Held> kept;
        bool              floaters = false;
        for (Held& held : all)
        {
            LLView* view = held.view.get();
            if (held.kind == Kind::Floater)
            {
                held.closed.disconnect();
                if (view)
                {
                    view->setVisible(true);
                    floaters = true;
                }
            }
            else if (held.kind == Kind::Nav || held.kind == Kind::TopInfo || held.kind == Kind::Status || held.kind == Kind::StandStop)
            {
                if (view && view->getVisible())
                {
                    kept.push_back(std::move(held));    // not hidden by it after all
                }
            }
            else
            {
                kept.push_back(std::move(held));
            }
        }
        mHeld.swap(kept);
        if (floaters && gFloaterView)
        {
            gFloaterView->hideAllFloaters();
        }
        if (mToolbars)
        {
            mToolbars = false;
            sToolbarHolders = std::max(sToolbarHolders - 1, 0);
        }
    }

    void Scope::syncHideUi(bool releasing)
    {
        if (!gViewerWindow)
        {
            return;
        }
        if (mOptions.ownsHideUi)
        {
            static LLCachedControl<bool> hide_ui(gSavedSettings, SETTING_HIDE_UI, false);
            if (hide_ui && !releasing)  // on the way out the user's press is theirs
            {
                undoHideUiControls();   // View > Hide UI is not for in here
            }
            mUiVisible = gViewerWindow->getUIVisibility();
            return;
        }
        if (ownerActive())
        {
            // The one that undoes View > Hide UI is on: it shows what Hide UI hid, and this must not take that over
            // (nor hide it again). Only "shown" is taken in, so that this holds the toolbars too, as when the UI was
            // shown from the start; "hidden" is not (it is undone at once).
            if (gViewerWindow->getUIVisibility())
            {
                mUiVisible = true;
            }
            return;
        }
        const bool visible = gViewerWindow->getUIVisibility();
        if (visible == mUiVisible)
        {
            return;
        }
        mUiVisible = visible;
        if (!visible)
        {
            handOverToHideUi();
        }
        else if (!releasing)
        {
            hideFloaters(gFloaterView, Kind::Floater);  // what it showed again; the bars and toolbars follow in this tick
        }
    }

    void Scope::tick()
    {
        if (mReleased)
        {
            return;
        }
        syncHideUi(false);
        holdSingles();
        holdToolbars();
        if (mHudHidden && rlvLocksHud())
        {
            // RLVa locked the HUD on while it is hidden here: shown at once, and not this one's to put back
            mHudHidden = false;
            sHudHolders = std::max(sHudHolders - 1, 0);
            LLPipeline::sShowHUDAttachments = true;
        }
    }

    void Scope::release()
    {
        if (mReleased)
        {
            return;
        }
        mReleased = true;
        sActive.erase(std::remove(sActive.begin(), sActive.end(), this), sActive.end());
        syncHideUi(true);   // View > Hide UI first: what it has taken over is not shown here

        Scope* heir = sActive.empty() ? nullptr : sActive.back();
        const bool mouselook = gAgentCamera.cameraMouselook();
        std::vector<Held> held;
        held.swap(mHeld);   // setVisible() may call back: nothing is walked that can change
        for (Held& entry : held)
        {
            entry.closed.disconnect();
            LLView* view = entry.view.get();
            if (!view)
            {
                continue;
            }
            if (heir && heir->wants(entry, mSerial))
            {
                heir->adopt(std::move(entry));  // the one that stays hides it: not shown for a frame in between
                continue;
            }
            // what the official mouselook hides one by one it shows again when it is left
            if (mouselook && (entry.kind == Kind::Menu || entry.kind == Kind::Nav || entry.kind == Kind::TopInfo))
            {
                continue;
            }
            view->setVisible(true);
        }
        releaseToolbars();
        releaseHud();
        releaseTags();
    }

    bool Scope::ownerActive()
    {
        return std::any_of(sActive.begin(), sActive.end(), [](const Scope* scope) { return scope->mOptions.ownsHideUi; });
    }

    void PlaybackHide::begin(bool enabled)
    {
        if (!enabled || mScope)
        {
            return;
        }
        Options options;
        options.hud       = Hud::Hide;
        options.tags      = Tags::Hide;
        options.standStop = true;
        mScope            = new Scope(options);
    }

    void PlaybackHide::end()
    {
        Scope* scope = mScope;
        mScope       = nullptr;   // first: release() calls back into views
        delete scope;
    }

    bool PlaybackHide::tick()
    {
        if (!mScope)
        {
            return false;
        }
        if (mScope->released())
        {
            end();
            return true;
        }
        mScope->tick();
        return false;
    }

    bool PlaybackHide::hiding() const
    {
        return mScope && !mScope->released();
    }

    // The condition is "a Scope that follows View > Hide UI is on" (a released Scope is not in sActive); only a
    // PlaybackHide makes one that does (see the header).
    bool hidingForPlayback()
    {
        return std::any_of(sActive.begin(), sActive.end(), [](const Scope* scope) { return scope->followsHideUi(); });
    }

    bool holds(const LLView* view)
    {
        for (const Scope* scope : sActive)
        {
            if (scope->holds(view))
            {
                return true;
            }
        }
        return false;
    }

    void releaseAll()
    {
        const std::vector<Scope*> scopes = sActive;
        for (Scope* scope : scopes)
        {
            scope->release();
        }
    }
}

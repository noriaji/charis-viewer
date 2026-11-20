/**
 * @file uiscope.h
 * @brief Hiding the viewer's UI for a while and putting back exactly what was hidden. One part for the
 *        photo screen (plus/photoscreen) and for what hides it while something plays: the camera path
 *        (plus/camerapath) and the camera shots (plus/camerashot) (research/365, 367 in the sl-viewer repository).
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

#ifndef PLUS_UISCOPE_UISCOPE_H
#define PLUS_UISCOPE_UISCOPE_H

#include <vector>

#include "llhandle.h"
#include "llview.h"
#include "stdtypes.h"

#include <boost/signals2/connection.hpp>

class LLFloater;

// A Scope hides, when it is made, what is shown of: the menu bar, the toolbars, the navigation bar, the mini
// location bar, the status bar's container, the chiclet bar's container, (the stand / fly buttons' container),
// the floaters (and the detached windows of plus/detach) and, as asked, the HUD attachments and the name tags / hover text; and puts back
// exactly those, nothing else, when it is let go. It is the only code that does this hiding: the photo screen,
// the camera path and the camera shots all use it, so two of them on at once (the camera path played in the photo
// screen, the photo screen entered while a path plays, a shot while the last picture of a path is awaited) hand
// what they hid over to each other instead of showing it (the
// exception: windows are not handed to a photo screen that was there first, see Options::windowsOnlyAtEntry).
//
// It does not use LLFloaterView::hideAllFloaters() / showHiddenFloaters() (that list is View > Hide UI's) except
// to hand floaters to View > Hide UI when it is pressed while a Scope that follows it is on. Main thread only.
namespace plus::uiscope
{
    enum class Hud
    {
        Keep,       // not touched
        Restore,    // not hidden, put back to what it was when the Scope was made (the photo screen)
        Hide,       // hidden (unless RLV has it locked), put back afterwards (the camera path, the camera shots)
    };

    // The name tags above the avatars (LLHUDNameTag) and the hover text of objects (LLHUDText): the official code draws
    // them in the world, not in the UI, so hiding the UI does not hide them.
    enum class Tags
    {
        Keep,       // not touched
        Hide,       // not drawn, put back to what they were afterwards (the camera path, the camera shots)
    };

    struct Options
    {
        // The floaters that stay shown (the photo screen's chat). Null: none.
        bool (*staysShown)(const LLFloater*) = nullptr;
        Hud  hud                             = Hud::Keep;
        Tags tags                            = Tags::Keep;
        // The container of the "stand up / stop flying" and "hide beacon" buttons is hidden too.
        bool standStop = false;
        // View > Hide UI is undone when the Scope is made and every tick (the photo screen). False: it is the
        // user's, and the Scope follows it (it is pressed while the Scope is on: what it hides is left to it; not
        // while a Scope that owns it is on: that one shows what Hide UI hid, this takes nothing over).
        bool ownsHideUi = false;
        // The windows are hidden only when the Scope is made (the photo screen): a window opened in it is the user's.
        // When a Scope made later lets go, this one does not take over the windows that one hid, they are shown.
        bool windowsOnlyAtEntry = false;
    };

    class Scope
    {
    public:
        explicit Scope(const Options& options);
        ~Scope();

        Scope(const Scope&)            = delete;
        Scope& operator=(const Scope&) = delete;

        // Every frame while it is on: follows View > Hide UI, hides again what the official code showed
        // (a bar by its setting, the stand / fly buttons when the avatar sits down), gives a HUD back that RLV locked.
        void tick();

        // Puts back (or hands over) what it hid. Done by the destructor too; safe to call any number of times.
        // After it the Scope holds nothing and tick() does nothing.
        void release();

        // True once it has let go (the quit observer, release()): its owner is to end what it was for too.
        bool released() const { return mReleased; }

        // False for a Scope that undoes View > Hide UI (Options::ownsHideUi, the photo screen's), true for one that follows it.
        bool followsHideUi() const { return !mOptions.ownsHideUi; }

        // Whether 'view' is one of the floaters this Scope hid and holds hidden now.
        bool holds(const LLView* view) const;

    private:
        enum class Kind : U8
        {
            Menu,
            Nav,
            TopInfo,
            Status,
            Chiclet,
            StandStop,
            Floater,    // of gFloaterView
            Detached,   // of the detached windows' view (plus/detach)
        };

        struct Held
        {
            LLHandle<LLView>            view;
            const LLView*               raw = nullptr;   // identity only, never used as a pointer
            Kind                        kind = Kind::Floater;
            boost::signals2::connection closed;          // a floater: shuts when it is closed while hidden
        };

        LLView* viewFor(Kind kind) const;
        bool    hidesKind(Kind kind) const;
        bool    wants(const Held& held, U32 leaverSerial) const;
        void    adopt(Held&& held);
        void    hookClose(Held& held);
        void    forgetClosed(const LLView* raw);
        void    holdSingles();
        void    holdSingle(Kind kind);
        void    holdToolbars();
        void    releaseToolbars();
        void    holdHud();
        void    releaseHud();
        void    holdTags();
        void    releaseTags();
        void    hideFloaters(LLView* parent, Kind kind);
        void    syncHideUi(bool releasing);
        void    handOverToHideUi();
        bool    isHeld(const LLView* raw) const;
        static bool ownerActive();
        static bool isEntryOf(const Held& held, const LLView* view);

        Options           mOptions;
        std::vector<Held> mHeld;
        bool              mToolbars           = false;   // this holds the toolbars hidden (counted in the .cpp)
        bool              mUiVisible          = true;    // View > Hide UI is off, as last seen
        bool              mHudHidden          = false;   // Hud::Hide: this hid it (and is counted)
        bool              mTagsHidden         = false;   // Tags::Hide: this hid the name tags and the hover text (and is counted)
        bool              mHudSaved           = true;    // LLPipeline::sShowHUDAttachments when made; while another Scope hides it, the value from before that one (sHudOriginal)
        U32               mSerial             = 0;       // the order the Scopes were made in (what is handed over depends on it)
        bool              mReleased           = false;
    };

    // The hiding that goes with something playing (the camera path's run, a camera shot): a Scope that hides the HUD,
    // the name tags and the hover text too and follows View > Hide UI, made when the run starts and let go of in every way it ends. The Options are
    // here, not in each user (they cannot differ). Never deleted at the end of the process on purpose (the Scope's
    // handles): keep it in a static and call end() from every way out.
    class PlaybackHide
    {
    public:
        // Hides, unless 'enabled' is false (the window's check box) or it already has a Scope (active(): also one that let go by itself and tick() has not dropped yet). Safe to call again.
        void begin(bool enabled);
        // Puts back (or hands over) what it hid. Safe to call any number of times.
        void end();
        // Every frame while something plays (and while the last picture of it is awaited): the Scope's tick(). When
        // the Scope let go on its own (a quit was asked for and cancelled) it is dropped, the run goes on with the
        // UI. True if it let go in this call.
        bool tick();
        // Whether it has a Scope (hiding, or let go of by itself and not dropped yet by tick()).
        bool active() const { return mScope != nullptr; }
        // Whether it is hiding the UI now.
        bool hiding() const;

    private:
        Scope* mScope = nullptr;
    };

    // True while a Scope that follows View > Hide UI (not Options::ownsHideUi) is on: what plus/shortcutbar,
    // plus/update, plus/mediareap and plus/chatconsole stay out of the way for, and plus/camerapath does not draw its
    // route in. The only Scope that follows it now is a PlaybackHide's: make one directly with ownsHideUi set, or use
    // a PlaybackHide, or these react to it too. Not the photo screen (its Scope owns Hide UI; it has isActive()).
    // Constant time.
    bool hidingForPlayback();

    // Whether a Scope that is on holds 'view' hidden: the floater is "shown" for what asks (the photo tools'
    // window hidden by a Scope is still the photo mode). Constant time while no Scope is on.
    bool holds(const LLView* view);

    // The viewer is quitting (plus/fastexit's begin(), before the official code closes the floaters): every
    // Scope lets go, so the floaters' visibility that is saved is "open" and not the hidden state.
    void releaseAll();
}

#endif

/**
 * @file rmlpopup.h
 * @brief RmlPopupView and SelectPopup: the list of a <select> of an RmlUi window, shown outside the window (research/388).
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

// RmlUi draws a <select>'s list inside the select's document, so the window's own clip and the context's size cut it
// (research/388 Q18, Q19). Here the list is another host (RmlPopupView, a child view of the window's host with its
// own context and the small document popup.rml) that goes in the viewer's popup layer (LLUI::addPopup: above every
// floater, below the menus and the tool tips), as the list of an XUI combo box does. The window's document keeps its
// plain <select>: its value, its "change" event and its data binding are RmlUi's own; only the list is ours.
//
// What a window has to do: nothing. RmlFloater (rmlfloater.h) makes a SelectPopup for its host, so every <select> of
// every RmlFloater window gets the list. The window can read the selection back as always.
//
// What it does (the XUI combo box's, research/388 2.4): a press on the select opens the list (a press held on and
// released over a row chooses it, a click and then a click on a row too); the arrows, Page Up / Down, Home / End and
// the first letters of a closed select open the list with the cursor moved (the combo box's list takes the key, and
// opens when the selection changed); in the open list the arrows, Page Up / Down (a page less one row), Home / End,
// Enter, Esc and the first letters work as in the combo box; a press outside closes it (the same select pressed
// again only closes); the wheel scrolls it; the list goes below the select, or above when there is more room, and
// stays inside the screen (or the detached window the select is in).
//
// Opening and closing happen only in the viewer's idle (SelectPopup::idleStep) and in the list's own overrides after
// RmlUi's work for it ended, never inside an RmlUi event: the listeners only note what was asked (research/388 3).

#ifndef PLUS_RMLPOPUP_H
#define PLUS_RMLPOPUP_H

#include "rmlcommon.h"
#include "rmlhost.h"

#include "llframetimer.h"
#include "llrect.h"
#include "lltimer.h"
#include "v2math.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace plus::rmlui
{
    // The list: an LLRmlView with its own context (popup.rml). It is a child of the window's host (so it goes with the
    // window and finds the window's detached space), hidden until the SelectPopup shows it, and drawn and given the
    // mouse by the popup layer, not by its parent (rmlhost.cpp draw / handleMouse do not reach children).
    class RmlPopupView : public LLRmlView
    {
    public:
        // What the list reports to its SelectPopup. Set by it, and cleared (setClient(nullptr)) before it goes.
        class Client
        {
        public:
            virtual ~Client() = default;
            virtual void popupEvent(Rml::Event& event) = 0;     // an event of popup.rml's document (inside RmlUi's work)
            virtual void popupUpdated() = 0;                    // after the list's Context::Update (inside RmlUi's work)
            virtual bool popupKey(KEY key, MASK mask) = 0;      // true: used
            virtual bool popupChar(llwchar uni_char) = 0;       // true: used
            virtual void popupMouseUp() = 0;                    // after RmlUi saw the release (not inside RmlUi's work)
            virtual void popupLost() = 0;                       // keyboard focus lost, hidden: the list must close
            virtual void popupTopLost() = 0;                    // a press outside it (or the app lost the focus): it must close
            virtual void popupCaptureLost() = 0;                // the mouse capture it was handed went (Alt-Tab, a modal window)
        };

        void setClient(Client* client) { mClient = client; }
        // Sets the hooks (the document's mouse listeners) and loads popup.rml; false when it could not be loaded.
        bool start();
        LLHandle<RmlPopupView> getPopupHandle() const { return getDerivedHandle<RmlPopupView>(); }

        // ---- LLView ----
        bool handleKeyHere(KEY key, MASK mask) override;
        bool handleUnicodeCharHere(llwchar uni_char) override;
        bool handleMouseUp(S32 x, S32 y, MASK mask) override;
        bool handleScrollWheel(S32 x, S32 y, S32 clicks) override;
        void onFocusLost() override;
        void onMouseCaptureLost() override;
        void onVisibilityChange(bool new_visibility) override;
        void onTopLost() override;

    protected:
        friend class LLUICtrlFactory;
        explicit RmlPopupView(const Params& p);

    private:
        Client* mClient = nullptr;
    };

    // The list of the <select>s of one window. Made by RmlFloater, which calls attach() when the document is loaded,
    // ownerUpdated() after each of the host's updates and detach() before the document goes.
    class SelectPopup final : private RmlPopupView::Client
    {
    public:
        SelectPopup();
        ~SelectPopup();
        SelectPopup(const SelectPopup&) = delete;
        SelectPopup& operator=(const SelectPopup&) = delete;

        // Starts listening in the document (the selects of it, and the ones made later by data-for, are covered).
        void attach(LLRmlView* owner, Rml::ElementDocument& document);
        // Closes the list and lets everything go: the listeners, the list's context. Idempotent. Call before the
        // window's document is unloaded (RmlFloater::closeDocument).
        void detach();
        // After the host's Context::Update (inside RmlUi's work): a select that went or changed closes the list.
        void ownerUpdated();
        // The owner window moved (dragging, snapping, being pushed back): the list does not follow, it closes. For
        // the window-moved notification of research/387, when it exists; until then idleStep() sees the move itself.
        void ownerMoved();
        // A typed character that the window's host did not use: the first letters of a closed select.
        bool handleChar(llwchar uni_char);
        // The wheel over a closed select, when PlusComboWheelSelect is on: the selection moves (the XUI combo box's: it takes
        // the wheel before anything that scrolls under it). x, y: the mouse in the owner's own coordinates.
        bool handleWheel(S32 x, S32 y, S32 clicks);

    private:
        enum class State
        {
            Closed,
            Requested,      // asked in an RmlUi event (or a typed character): the idle opens it
            Open,
            ClosePending,   // a reason to close came inside RmlUi's work: the idle closes it
        };
        enum class Trigger
        {
            None,
            Press,          // a left press on the select
            Key,            // an arrow (step), Enter or Space (step 0), or typed characters
        };
        struct Request
        {
            Trigger trigger = Trigger::None;
            int step = 0;               // the rows an arrow moves the cursor
            int pages = 0;              // Page Up (-1) / Page Down (+1): as many pages (a page is known when the list is placed)
            int edge = 0;               // Home (-1) / End (+1): the cursor goes to the first / last row
            LLWString chars;            // typed characters, in the order they came
        };
        struct Row
        {
            int option = 0;             // the option's index in the select (what SetSelection takes)
            std::string value;
            std::string text;
            bool selectable = true;
        };
        // One listener on the owner's document: the selects' press, click, keys, blur and scroll.
        class OwnerListener : public Rml::EventListener
        {
        public:
            void setController(SelectPopup* controller) { mController = controller; }
            void ProcessEvent(Rml::Event& event) override;

        private:
            SelectPopup* mController = nullptr;
        };
        // The idle callback's link: it holds this for as long as the SelectPopup is there (detach and the destructor
        // clear it, so a callback that comes after sees nothing).
        struct Link
        {
            SelectPopup* self = nullptr;
        };
        friend class OwnerListener;

        // ---- RmlPopupView::Client ----
        void popupEvent(Rml::Event& event) override;
        void popupUpdated() override;
        bool popupKey(KEY key, MASK mask) override;
        bool popupChar(llwchar uni_char) override;
        void popupMouseUp() override;
        void popupLost() override;
        void popupTopLost() override;
        void popupCaptureLost() override;

        // What the owner's listener saw (inside RmlUi's work: only a note is made).
        void ownerEvent(Rml::Event& event);
        struct Move
        {
            int step = 0;
            int pages = 0;
            int edge = 0;
        };
        void request(Rml::ElementFormControlSelect* select, Trigger trigger, const Move& move, llwchar character);
        void queueIdle();
        bool idleStep();

        // Opening. tryOpen reads the select again (it may have changed since the request), places the list and shows it.
        void tryOpen();
        bool ensureView(LLRmlView* owner);
        bool readRows(Rml::ElementFormControlSelect& select, std::vector<Row>& rows) const;
        // The sizes popup.rcss gives the list and the widest row text, measured in the list's own document (its fonts and
        // its dp ratio), for the box placeView makes. False: the list's document is not usable.
        bool measure(RmlPopupView& view, const LLVector2& scale);
        // The open look of the select: its class and, whatever its own style sheet says, the accent border and the arrow
        // turned up (inline properties: a window's own rules for its selects cannot be beaten by a rule of charis.rcss).
        static void setOpenLook(Rml::ElementFormControlSelect& select, bool open);
        static U64 rowsMark(const std::vector<Row>& rows);
        bool placeView(LLRmlView* owner, Rml::ElementFormControlSelect& select, RmlPopupView& view);
        void buildRows(RmlPopupView& view);

        // Closing: requestClose from anywhere (the list's lost, the listeners); closeNow does the work, in the order of
        // research/388 3 (the state first, so removePopup's onTopLost is not heard twice).
        void requestClose();
        void closeNow();

        // The cursor row and choosing.
        void setCursor(int row, bool scroll);
        void moveCursor(int step);
        void moveCursorToEnd(int direction);    // Home (-1) / End (1)
        int pageRows() const { return std::max(1, mRowsPerPage - 1); }      // a page less one row (the XUI list's)
        void choose(int row);
        bool typeAhead(llwchar uni_char);
        void releaseHandedCapture();

        LLHandle<LLRmlView> mOwner;
        LLHandle<RmlPopupView> mView;
        Rml::ObserverPtr<Rml::Element> mDocument;       // the owner's document (where the listeners are)
        Rml::ObserverPtr<Rml::Element> mSelect;         // the select the list is for (set while Requested and Open)
        OwnerListener mListener;
        std::shared_ptr<Link> mLink;

        State mState = State::Closed;
        Request mRequest;
        bool mIdleQueued = false;
        // The list could not be made (popup.rml did not load): the selects keep RmlUi's own list for the rest of the
        // window's life (like a failed RmlUi start for the session; nothing to put back).
        bool mUnavailable = false;

        std::vector<Row> mRows;         // the rows of the open list (reused)
        std::vector<Row> mCheckRows;    // ownerUpdated's scratch (reused)
        std::vector<LLWString> mLabels; // the rows' texts, lower case and trimmed, for the type-ahead (made at the first letter)
        bool mLabelsReady = false;
        std::string mOpenValue;         // the select's value when the list opened
        U64 mOptionsMark = 0;
        int mCursor = -1;               // a row of mRows; -1: none
        int mChoose = -1;               // a row a mouse release or Enter asked for
        bool mPressOnRow = false;       // the press that is going on began on a row
        bool mHandedPress = false;      // the owner's press was handed over to the list (its release ends it)
        bool mScrollToCursor = false;
        bool mScrollCenter = false;
        S32 mRowHeight = 0;             // UI units
        S32 mRowsPerPage = 1;
        // What popup.rcss gives the list (UI units), measured when it opens.
        S32 mSidePadding = 0;
        S32 mBorder = 0;
        S32 mScrollbarWidth = 0;
        S32 mVerticalPadding = 0;       // above the first row and below the last (the corners' radius)
        S32 mWidestTextPx = 0;          // context pixels of the popup's context

        // What the list was placed for (idleStep closes it when any of them changes).
        Rml::Vector2f mAnchorPx;
        Rml::Vector2f mAnchorSizePx;
        LLVector2 mScale;
        S32 mOwnerScreenX = 0;
        S32 mOwnerScreenY = 0;

        // After a press outside closed the list: a press on the same select in the same frame (the one that closed it, now
        // coming to the window) does not open it again.
        U32 mClosedFrame = 0;
        Rml::ObserverPtr<Rml::Element> mClosedSelect;

        LLWString mSearch;
        LLTimer mSearchTimer;
    };
}

#endif // PLUS_RMLPOPUP_H

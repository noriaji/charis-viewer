/**
 * @file rmlhost.h
 * @brief LLRmlView (XUI tag plus_rml_view): an RmlUi document as a floater's content (research/364 2.2, 2.4).
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

// The floater stays the official one (title, moving, resizing, closing, Z order, detaching, saved place):
// a plus_rml_view fills its content and is one Rml::Context with one document (research/364 2.4). It is put
// in a floater as its whole content, not inside a scroll container (the document's scissors stay inside the
// host's own clip, research/364 2.1.2).
//
//   <plus_rml_view name="body" document="demo.rml" layout="topleft" left="0" top="18" width="420" height="542"
//                  follows="all" />
//
// document: a file under app_settings/plus/rmlui/ (rmlsystem.h isDocumentPath). Its words are "[[name]]" in
// the RML: the <string name="name"> of the floater's XML (en and ja), put in when the text is made (a value
// holding < & { } is not used: it would be read as RML or data). A title="[[name]]" attribute is the tool tip.
//
// Per frame (research/364 3.1): Context::Update and Render run only when something changed (input, a
// dirtied data model or property: markDirty(), the size, the UI scale) or when RmlUi asked for an update by
// then (GetNextUpdateDelay: animations, the caret's blink). Any other frame draws the last recording again
// (rmlrender.h). A hidden or closed floater does not draw: nothing runs.
//
// Input (research/364 2.2.2): the mouse in context pixels (UI units times the UI scale, y down); a press takes
// the keyboard focus and the mouse capture, a lost capture ends the press without a click; the wheel goes to
// the parent when the document does not scroll; keys belong to the document while a text field has its focus
// (acceptsTextInput: the key bindings do not get them), else only the keys it uses go to it; typed characters
// go to the focused text field; the IME through rmlime.h. What the fields do beyond RmlUi's own (read-only text, select
// on focus, number boxes, the edit menu on a right click, the cut name's tool tip) is rmlparts.h's, the official menus
// a window opens are rmlmenu.h's (research/387). A field that is read-only (the attribute readonly) takes no
// characters, so it is not "a text field" for acceptsTextInput, but it selects, copies and scrolls.
//
// The owner (the floater) sets its hooks and calls load() from its postBuild, and unload() from its destructor
// (its data model and callbacks go before it does).

#ifndef PLUS_RMLHOST_H
#define PLUS_RMLHOST_H

#include "rmlcommon.h"
#include "rmlmenu.h"
#include "rmlparts.h"
#include "rmlrender.h"

#include "llcursortypes.h"
#include "lleditmenuhandler.h"
#include "lluictrl.h"

#include <functional>
#include <string>

// LLEditMenuHandler: while a text field of the document has the focus, the view is the edit menu's handler
// (LLEditMenuHandler::gEditMenuHandler), as LLLineEditor is: the edit menu's keys (Del, Ctrl+X/C/V/A, Ctrl+Z/Y)
// run before the keyboard focus gets a key (LLViewerWindow::handleKey), and would otherwise act on the last
// handler (a selection in the world: Del deleted the selected objects).
class LLRmlView : public LLUICtrl, public LLEditMenuHandler
{
public:
    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
        Optional<std::string> document;     // the RML file under app_settings/plus/rmlui/
        Params();
    };

    // What the owner adds to the document. Called on the main thread inside RmlUi's work for this host.
    struct Hooks
    {
        std::function<void(Rml::Context&)> before_load;         // data models (the document binds them as it loads)
        std::function<void(Rml::ElementDocument&)> after_load;  // listeners (listen()), first contents
        std::function<void()> before_update;                    // changes kept out of RmlUi's callbacks (later())
        std::function<void()> after_update;                     // reads the new layout (sizes) after Context::Update
        std::function<void(Rml::Event&)> on_event;              // an event of an element passed to listen()
    };

    ~LLRmlView() override;

    void setHooks(Hooks hooks) { mHooks = std::move(hooks); }
    // Loads the document once (later calls do nothing). draw() calls it when the owner did not.
    void load();
    // Takes the context and its document away; the view then shows nothing. Idempotent. Called while RmlUi works
    // for this view (from a hook or an event: a "close" button), it waits until that work ends (mBusy).
    void unload();
    // Calls hooks.on_event for element's event (the listener is the view's own: it goes with the document).
    void listen(Rml::Element* element, const char* event);
    // The same, as a capture-phase listener. On the element itself (the event's target) the order is: the
    // listeners that are not capture ones first, then the capture ones in the order they were added (RmlUi's own
    // widget added its own when the element was made: a text field's Blur, which ends an IME composition, runs
    // before a listenCapture() of the window on that field). A capture listener on an ancestor runs before all
    // the target's.
    void listenCapture(Rml::Element* element, const char* event);
    // Something changed outside RmlUi's input (a data model dirtied, a property set): update on the next frame.
    void markDirty();

    // The layout is made again at once, in the window's after_update hook: a class that was set from the new layout
    // (RmlList's has-bar) takes effect in this frame's drawing. Counted like the update, and what RmlUi asks for in
    // it (the next update's time) replaces the first one's.
    void updateAgain();

    Rml::Context* getContext() const { return mContext; }
    Rml::ElementDocument* getDocument() const { return mDocument; }
    // The host's field parts (rmlparts.h: read-only text, select on focus, number boxes ...). Valid from load() to unload().
    plus::rmlui::Parts& parts() { return mParts; }
    // A point of the view (UI units, y up) in the context's pixels: the conversion the host's own mouse input uses.
    Rml::Vector2i pointToContext(S32 x, S32 y) const { return toContext(x, y); }

    // ---- for rmlsystem.cpp and rmlime.cpp ----
    // The host RmlUi is working for now (its input, update, render, load), or null.
    static LLRmlView* current() { return sCurrent; }
    // A host's RmlUi work: sets current() for RmlUi's global interfaces (cursor, words, caret) and counts the
    // host busy; the last one out does an unload() asked for meanwhile. Nests.
    class Scope
    {
    public:
        explicit Scope(LLRmlView* host);
        ~Scope();
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        LLRmlView* mHost;
        LLRmlView* mPrevious;
    };
    // The floater's <string name="key">, when it is safe in a text node; false: leave "[[key]]".
    bool translate(const std::string& key, std::string& out) const;
    // RmlUi's cursor name for the element under the mouse (Context::EnableMouseCursor).
    void setCursorName(const std::string& name);
    // A context pixel point and rectangle in the window's GL coordinates (pixels from the bottom-left), for the IME.
    LLCoordGL contextToGL(Rml::Vector2f point) const;
    LLRect contextRectToGL(const Rml::Rectanglef& rect) const;
    // The same point in this view's own units (x right, y up from the bottom), the reverse of toContext (rmlmenu.cpp).
    void contextToLocal(Rml::Vector2f point, S32& x, S32& y) const;
    // One character typed into the focused text field (a key, or one of an IME's result).
    bool typeCharacter(llwchar uni_char);
    LLHandle<LLRmlView> getRmlHandle() const { return getDerivedHandle<LLRmlView>(); }
    // The focused element is a password field (no copy or cut out of it).
    bool passwordFocused() const;
    // The edit menu's handler while a text field has the focus (rmlime.cpp: OnActivate takes it, and so does the
    // view's onFocusReceived; the field's blur, destruction and the view's unload / focus loss give it back).
    // releaseEditMenu does nothing when another handler has it.
    void takeEditMenu();
    void releaseEditMenu();

    // ---- LLEditMenuHandler (only while a text field has the focus) ----
    // Undo / redo: RmlUi's text fields have none (canUndo false: the menu does not take Ctrl+Z, the field does).
    void cut() override;
    bool canCut() const override;
    void copy() override;
    bool canCopy() const override;
    void paste() override;
    bool canPaste() const override;
    void doDelete() override;
    bool canDoDelete() const override;
    void selectAll() override;
    bool canSelectAll() const override;

    // ---- LLView ----
    void draw() override;
    void reshape(S32 width, S32 height, bool called_from_parent = true) override;
    void onVisibilityChange(bool new_visibility) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleRightMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleRightMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleMiddleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMiddleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleDoubleClick(S32 x, S32 y, MASK mask) override;
    bool handleScrollWheel(S32 x, S32 y, S32 clicks) override;
    bool handleToolTip(S32 x, S32 y, MASK mask) override;
    bool handleKeyHere(KEY key, MASK mask) override;
    bool handleKeyUpHere(KEY key, MASK mask) override;
    bool handleUnicodeCharHere(llwchar uni_char) override;
    void onMouseCaptureLost() override;
    void onMouseLeave(S32 x, S32 y, MASK mask) override;
    void onFocusReceived() override;
    void onFocusLost() override;
    bool acceptsTextInput() const override;

protected:
    friend class LLUICtrlFactory;
    explicit LLRmlView(const Params& p);

private:
    enum class Status
    {
        NotLoaded,  // load() not called yet
        Loaded,     // the document shows
        SafeMode,   // a safe start: the message instead
        Failed,     // RmlUi or the document could not be loaded: the message instead
        Unloaded,   // unload(): nothing
    };
    // What makes the next draw update (research/364 6, mDirty).
    enum Dirty : U32
    {
        DIRTY_INPUT = 1 << 0,
        DIRTY_DATA  = 1 << 1,
        DIRTY_SIZE  = 1 << 2,
        DIRTY_ALL   = DIRTY_INPUT | DIRTY_DATA | DIRTY_SIZE,
    };

    // The listener of every listen(): a member, so it lives as long as the context it is attached in.
    class Listener : public Rml::EventListener
    {
    public:
        void setHost(LLRmlView* host) { mHost = host; }
        void ProcessEvent(Rml::Event& event) override;

    private:
        LLRmlView* mHost = nullptr;
    };

    bool ready() const { return mStatus == Status::Loaded && mContext; }
    void fail(Status status, const char* message_name);
    void drawMessage() const;
    void runUpdate(F64 now);
    // The context's size and dp ratio for this frame's UI scale (marks the size dirty when it changed).
    void syncSize(const LLVector2& scale);
    LLVector2 inputScale() const;
    Rml::Vector2i toContext(S32 x, S32 y) const;
    // ProcessMouseMove when the mouse is somewhere else than last time (hover comes every frame).
    void moveMouse(S32 x, S32 y, MASK mask);
    // A tool tip with text over the element's box (the tool tip manager's sticky rectangle).
    void showToolTip(Rml::Element* element, const std::string& text);
    // The right press landed on a field of the document that no listener of the window took: the official text editor
    // menu there (research/387 3.2). Inside a Scope.
    void showEditMenu(const Rml::ObserverPtr<Rml::Element>& field);
    bool pressMouse(S32 x, S32 y, MASK mask, int button, bool capture);
    bool releaseMouse(S32 x, S32 y, MASK mask, int button);
    void releaseCapture();
    // The mouse left with a press held (capture lost, hidden): hover gone, the press ends without a click.
    void leaveMouse();
    // What the focused element is to the keyboard (rmlparts.h fieldKind): typable = takes characters (the IME, typed
    // text, cut / paste / delete); selectable = also a read-only field (the edit menu: copy, select all).
    plus::rmlui::FieldKind focusedFieldKind() const;
    bool typableFocused() const { return plus::rmlui::isTypable(focusedFieldKind()); }
    bool selectableFocused() const { return plus::rmlui::isSelectable(focusedFieldKind()); }
    enum class KeyMove { Down, Up };
    bool processKey(KEY key, MASK mask, KeyMove move);
    // Whether a key RmlUi had stays the document's (true) or goes on to the viewer (research/364 2.2.2).
    bool keyForDocument(KEY key, MASK mask, bool not_consumed) const;
    // An edit menu command as the field's own key (Ctrl+X / C / V / A, Delete), when a text field has the focus.
    void sendEditKey(Rml::Input::KeyIdentifier key, int modifiers);
    // unload()'s work (the destructor runs it even while busy: nothing can wait then).
    void unloadNow();
    void logOpened(F64 now);

    static LLRmlView* sCurrent;

    std::string mDocumentPath;
    Hooks mHooks;
    Listener mListener;
    Status mStatus = Status::NotLoaded;
    bool mHolding = false;                  // acquire() gave Ready; release() is owed
    S32 mBusy = 0;                          // Scopes open for this view (RmlUi works for it)
    bool mUnloadPending = false;            // unload() asked while busy: the last Scope out does it
    std::string mContextName;
    Rml::Context* mContext = nullptr;       // RmlUi owns it; RemoveContext(mContextName) in unload()
    Rml::ElementDocument* mDocument = nullptr;
    plus::rmlui::DrawList mDrawList;
    plus::rmlui::Parts mParts;
    plus::rmlui::ContextMenu mEditMenu;     // the text editor menu (a window's own menus are its own ContextMenu)
    U32 mDirty = DIRTY_ALL;
    F64 mNextUpdateAt = 0.0;                // LLTimer::getTotalSeconds(); NO_DEADLINE: none
    LLVector2 mPixelScale;                  // context pixels per UI unit (this frame's UI scale), 0: not yet
    S32 mWidthPx = 0;
    S32 mHeightPx = 0;
    // The text field the view's focus loss blurred; the focus coming back puts the keyboard focus on it again (as
    // LLFloater::setFocus does for an XUI field), unless it comes from a press (the pressed element decides).
    Rml::ObserverPtr<Rml::Element> mBlurredField;
    Rml::ObserverPtr<Rml::Element> mBlurPlace;  // where the Blur put the focus (the field's parent)
    bool mPressing = false;                 // pressMouse is giving the view the focus
    bool mMouseCaptured = false;            // this view took the capture for a press
    bool mMouseInside = false;              // the last mouse position below is the mouse's
    Rml::Vector2i mMousePx;
    ECursorType mCursor = UI_CURSOR_ARROW;
    LLWString mMessage;                     // shown instead of the document (SafeMode, Failed)
    // The "opened" log line, written after the first frame drawn (its layout and glyphs included).
    bool mOpenLogPending = false;
    F64 mOpenStart = 0.0;
    F64 mLoadMs = 0.0;
    F64 mInitMs = 0.0;
    F64 mFontsMs = 0.0;
    S32 mFontFaces = 0;
};

#endif // PLUS_RMLHOST_H

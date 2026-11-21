/**
 * @file rmlpopup.cpp
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

#include "llviewerprecompiledheaders.h"

#include "rmlpopup.h"

#include "rmlpopuplogic.h"

#include "llcallbacklist.h"
#include "llfocusmgr.h"
#include "llfontgl.h"
#include "llstring.h"
#include "llui.h"
#include "lltrans.h"
#include "lluictrlfactory.h"

#include <algorithm>

// plus/detach (research/87): llui.cpp. Where a popup opened at a point may go: the screen, or the detached window the
// point is on. The same declaration as llcombobox.cpp's.
LLRect plus_popup_area(const LLView* area_view, const LLView* at_view, S32 at_x, S32 at_y, const LLRect& area);

using namespace plus::rmlui;

namespace
{
    constexpr char POPUP_DOCUMENT[] = "popup.rml";
    constexpr char LIST_ID[] = "list";

    // As the XUI combo box (llcombobox.cpp, llscrolllistctrl.cpp): the widest the list goes, how far from the area's
    // edge it stays, and the padding between the cells' text and the row's edge.
    constexpr S32 MAX_POPUP_WIDTH = 500;        // MAX_COMBO_WIDTH
    constexpr S32 AREA_MARGIN = 50;
    constexpr S32 ROW_PADDING = 2;              // the XUI list's row_padding

    // The list's box is the sizes popup.rcss gives it (1 dp of popup.rcss is 1 UI unit: the context's dp ratio is the UI
    // scale). They are read from #probe of popup.rml when the list opens (SelectPopup::measure); these are used only when
    // that fails, and equal the tokens as they are now (--combo-pad, the border of body.popup, --scroll-width,
    // --combo-radius of charis_tokens.rcss).
    constexpr S32 FALLBACK_SIDE_PADDING = 10;
    constexpr S32 FALLBACK_BORDER = 1;
    constexpr S32 FALLBACK_SCROLLBAR = 6;
    constexpr S32 FALLBACK_VERTICAL_PADDING = 6;
    // One UI unit more than the rows need: the context is a whole number of pixels (a UI scale that is not a whole number
    // makes the rows a little taller than the box), and a list that is a hair too short for its rows shows a scroll bar.
    constexpr S32 HEIGHT_SLACK = 1;

    constexpr size_t MAX_ROWS = 500;            // a list this long is cut (a select with so many options is not a drop-down)
    constexpr size_t WARN_ROWS = 300;           // the rows are all elements: a longer list is slow to open
    constexpr S32 MAX_ROW_TEXT_BYTES = 1024;
    constexpr F32 ANCHOR_TOLERANCE_PX = 0.5f;
    constexpr char MORE_STRING[] = "PlusRmlSelectMore";     // strings_plus.xml: "...and [COUNT] more"
    constexpr char CHEVRON_OPEN[] = "image(ui-image/Plus_PS_Icon_chevron_up)";

    // The select an element is (or is inside of), if any.
    Rml::ElementFormControlSelect* selectOf(Rml::Element* element)
    {
        for (; element; element = element->GetParentNode())
        {
            if (auto* select = rmlui_dynamic_cast<Rml::ElementFormControlSelect*>(element))
            {
                return select;
            }
        }
        return nullptr;
    }

    // The text of an option: its text nodes in order (a text node is never read as RML again: the text of a window can
    // come from outside, research/358 P30).
    void collectText(Rml::Element* element, std::string& out)
    {
        const int count = element->GetNumChildren(true);
        for (int i = 0; i < count; ++i)
        {
            Rml::Element* child = element->GetChild(i);
            if (auto* text = rmlui_dynamic_cast<Rml::ElementText*>(child))
            {
                out += text->GetText();
            }
            else
            {
                collectText(child, out);
            }
        }
    }

    // The list element of the popup's document (null while it is not loaded).
    Rml::Element* listOf(RmlPopupView* view)
    {
        Rml::ElementDocument* document = view ? view->getDocument() : nullptr;
        return document ? document->GetElementById(LIST_ID) : nullptr;
    }

    U64 fnv(U64 hash, const std::string& text)
    {
        for (const char c : text)
        {
            hash = (hash ^ (U8)c) * 1099511628211ULL;
        }
        return (hash ^ 0xff) * 1099511628211ULL;
    }
}

// ---------------------------------------------------------------- RmlPopupView

RmlPopupView::RmlPopupView(const Params& p)
:   LLRmlView(p)
{
}

bool RmlPopupView::start()
{
    Hooks hooks;
    hooks.after_load = [this](Rml::ElementDocument& document)
    {
        listen(&document, "mousedown");
        listen(&document, "mouseup");
    };
    hooks.after_update = [this]()
    {
        if (mClient)
        {
            mClient->popupUpdated();
        }
    };
    hooks.on_event = [this](Rml::Event& event)
    {
        if (mClient)
        {
            mClient->popupEvent(event);
        }
    };
    setHooks(std::move(hooks));
    load();
    return getDocument() != nullptr;
}

bool RmlPopupView::handleKeyHere(KEY key, MASK mask)
{
    return mClient && mClient->popupKey(key, mask);
}

bool RmlPopupView::handleUnicodeCharHere(llwchar uni_char)
{
    return mClient && mClient->popupChar(uni_char);
}

bool RmlPopupView::handleMouseUp(S32 x, S32 y, MASK mask)
{
    const bool handled = LLRmlView::handleMouseUp(x, y, mask);     // RmlUi sees the release (the rows' "mouseup")
    if (mClient)
    {
        mClient->popupMouseUp();    // and now, outside RmlUi's work: choose, give the capture back
    }
    return handled;
}

bool RmlPopupView::handleScrollWheel(S32 x, S32 y, S32 clicks)
{
    LLRmlView::handleScrollWheel(x, y, clicks);
    return true;    // over the list the wheel is the list's, at its ends too
}

void RmlPopupView::onFocusLost()
{
    LLRmlView::onFocusLost();
    if (mClient)
    {
        mClient->popupLost();
    }
}

void RmlPopupView::onMouseCaptureLost()
{
    LLRmlView::onMouseCaptureLost();
    if (mClient)
    {
        mClient->popupCaptureLost();    // (the capture it was handed: the press that opened the list ends)
    }
}

void RmlPopupView::onVisibilityChange(bool new_visibility)
{
    LLRmlView::onVisibilityChange(new_visibility);
    if (!new_visibility && mClient)
    {
        mClient->popupLost();   // the window above it was hidden (the list itself hidden: the state is Closed already)
    }
}

void RmlPopupView::onTopLost()
{
    if (mClient)
    {
        mClient->popupTopLost();
    }
    LLRmlView::onTopLost();
}

// ---------------------------------------------------------------- SelectPopup

void SelectPopup::OwnerListener::ProcessEvent(Rml::Event& event)
{
    if (mController)
    {
        mController->ownerEvent(event);
    }
}

SelectPopup::SelectPopup()
:   mLink(std::make_shared<Link>())
{
    mLink->self = this;
    mListener.setController(this);
}

SelectPopup::~SelectPopup()
{
    detach();
}

void SelectPopup::attach(LLRmlView* owner, Rml::ElementDocument& document)
{
    detach();
    mOwner = owner ? owner->getRmlHandle() : LLHandle<LLRmlView>();
    mDocument = document.GetObserverPtr();
    mListener.setController(this);
    if (!mLink)
    {
        mLink = std::make_shared<Link>();
    }
    mLink->self = this;
    // Capture phase on the document: before the select's own widget (WidgetDropDown listens to the click, the keys
    // and the blur on the select), so a click and an arrow can be stopped before RmlUi opens its own list.
    document.AddEventListener("mousedown", &mListener, true);
    document.AddEventListener("click", &mListener, true);
    document.AddEventListener("keydown", &mListener, true);
    document.AddEventListener("blur", &mListener, true);
    document.AddEventListener("scroll", &mListener, true);
}

void SelectPopup::detach()
{
    if (LLRmlView::current() && mState != State::Closed)
    {
        // The window was closed from inside an RmlUi event (rmlfloater.h: closeFloater is for outside them): the list
        // closes here all the same, but its focus and capture are put back in the middle of RmlUi's work.
        LL_WARNS_ONCE("RmlUi") << "RmlUi: a window with an open select list was closed inside an RmlUi event" << LL_ENDL;
    }
    closeNow();     // (an open list first: its focus and capture go back while the window is still whole)
    if (mLink)
    {
        mLink->self = nullptr;      // an idle callback still queued finds nothing and ends
        mLink.reset();
    }
    mIdleQueued = false;
    mState = State::Closed;
    mRequest = Request();
    LLRmlView* owner = mOwner.get();
    if (Rml::Element* document = mDocument.get())
    {
        LLRmlView::Scope scope(owner);
        document->RemoveEventListener("mousedown", &mListener, true);
        document->RemoveEventListener("click", &mListener, true);
        document->RemoveEventListener("keydown", &mListener, true);
        document->RemoveEventListener("blur", &mListener, true);
        document->RemoveEventListener("scroll", &mListener, true);
    }
    mDocument = Rml::ObserverPtr<Rml::Element>();
    mSelect = Rml::ObserverPtr<Rml::Element>();
    mClosedSelect = Rml::ObserverPtr<Rml::Element>();
    if (RmlPopupView* view = mView.get())
    {
        view->setClient(nullptr);
        view->unload();     // its context goes now (or when RmlUi's work for it ends)
    }
    mView = LLHandle<RmlPopupView>();
    mOwner = LLHandle<LLRmlView>();
    mListener.setController(nullptr);
}

void SelectPopup::ownerUpdated()
{
    if (mState != State::Open)
    {
        return;
    }
    auto* box = rmlui_dynamic_cast<Rml::ElementFormControlSelect*>(mSelect.get());
    if (!box || box->IsDisabled() || !readRows(*box, mCheckRows) || rowsMark(mCheckRows) != mOptionsMark ||
        box->GetValue() != mOpenValue)
    {
        requestClose();     // the select went, was disabled, or its options or value changed from outside
    }
}

void SelectPopup::ownerMoved()
{
    requestClose();
}

bool SelectPopup::handleChar(llwchar uni_char)
{
    if (mUnavailable || uni_char < 0x20 || uni_char == 0x7f || (mState != State::Closed && mState != State::Requested))
    {
        return false;
    }
    LLRmlView* owner = mOwner.get();
    Rml::Context* context = owner ? owner->getContext() : nullptr;
    auto* box = context ? selectOf(context->GetFocusElement()) : nullptr;
    if (!box || box->IsDisabled())
    {
        return false;
    }
    // A space opens the list (as the XUI combo box's button does); the other letters also look for a row. How the space
    // comes here: its key down reaches the document, where RmlUi's default action makes a click of it (only for a select
    // with tab-index: auto) and ownerEvent stops that click, so RmlUi's own list does not open; the character the space
    // types comes after it and is the one that opens ours. That relies on the click being stopped.
    request(box, Trigger::Key, Move(), uni_char == ' ' ? 0 : uni_char);
    return true;
}

bool SelectPopup::handleWheel(S32 x, S32 y, S32 clicks)
{
    static LLUICachedControl<bool> wheel_select("PlusComboWheelSelect", false);
    if (!wheel_select || clicks == 0 || mState != State::Closed)
    {
        return false;
    }
    LLRmlView* owner = mOwner.get();
    Rml::Context* context = owner ? owner->getContext() : nullptr;
    if (!context || !owner->isInVisibleChain())
    {
        return false;
    }
    // The select under the mouse (the host moves its hover only when it gets the wheel, which it does after this), found
    // with the host's own conversion of a point.
    const Rml::Vector2i point = owner->pointToContext(x, y);
    auto* box = selectOf(context->GetElementAtPoint(Rml::Vector2f((F32)point.x, (F32)point.y)));
    if (!box || box->IsDisabled())
    {
        return false;
    }
    LLRmlView::Scope scope(owner);
    if (!readRows(*box, mCheckRows))
    {
        return false;
    }
    // A row at a time, the ones that cannot be chosen skipped; the ends stop it (the XUI combo box's selectNextItem).
    const int current = popuplogic::rowOfOption(mCheckRows, box->GetSelection());
    if (current < 0 && box->GetSelection() >= 0)
    {
        return false;   // the chosen option is one the list does not have (cut at MAX_ROWS): the wheel leaves it alone
    }
    const int moved = popuplogic::stepRows(mCheckRows, current, (int)clicks);
    if (moved == current || moved < 0 || moved >= (int)mCheckRows.size() || mCheckRows[moved].option < 0)
    {
        return false;
    }
    box->SetSelection(mCheckRows[moved].option);    // "change" (the window's listeners run here)
    if (LLRmlView* again = mOwner.get())
    {
        again->markDirty();
    }
    return true;
}

// ---- what the owner's document tells (inside RmlUi's work: a note only) ----

void SelectPopup::ownerEvent(Rml::Event& event)
{
    if (mUnavailable)
    {
        return;     // no list could be made: the selects keep RmlUi's own
    }
    const Rml::EventId id = event.GetId();
    // (the select only for the events of a press, a click and the keys: a scroll or a blur needs none)
    Rml::ElementFormControlSelect* select =
        (id == Rml::EventId::Blur || id == Rml::EventId::Scroll) ? nullptr : selectOf(event.GetTargetElement());
    switch (id)
    {
    case Rml::EventId::Mousedown:
        if (select && !select->IsDisabled() && event.GetParameter<int>("button", -1) == 0 &&
            // the press that closed this select's list from outside comes here after it, in the same frame: it only closes
            !(mClosedFrame == LLFrameTimer::getFrameCount() && mClosedSelect.get() == select))
        {
            request(select, Trigger::Press, Move(), 0);
        }
        break;
    case Rml::EventId::Click:
        if (select && !select->IsDisabled())
        {
            event.StopPropagation();    // RmlUi's own list opens on the click: not this one
        }
        break;
    case Rml::EventId::Keydown:
        if (select && !select->IsDisabled() && mState != State::Open && mState != State::ClosePending &&
            !event.GetParameter<bool>("ctrl_key", false) && !event.GetParameter<bool>("alt_key", false) &&
            !event.GetParameter<bool>("meta_key", false))
        {
            // What the XUI combo box's list does with a key when it is closed: it takes it, and opens when the selection moved
            // (the arrows, Page Up / Down, Home / End); Enter and a space open it (its button's click).
            Move move;
            bool used = true;
            switch (event.GetParameter<int>("key_identifier", 0))
            {
            case Rml::Input::KI_UP:     move.step = -1; break;
            case Rml::Input::KI_DOWN:   move.step = 1; break;
            case Rml::Input::KI_PRIOR:  move.pages = -1; break;
            case Rml::Input::KI_NEXT:   move.pages = 1; break;
            case Rml::Input::KI_HOME:   move.edge = -1; break;
            case Rml::Input::KI_END:    move.edge = 1; break;
            case Rml::Input::KI_RETURN:
            case Rml::Input::KI_NUMPADENTER:
                break;      // (a space comes as a typed character, handleChar)
            default:
                used = false;
                break;
            }
            if (used)
            {
                request(select, Trigger::Key, move, 0);
                event.StopPropagation();    // (and not RmlUi's own: the click it makes of Enter, its list's arrows)
            }
        }
        break;
    case Rml::EventId::Blur:
        // (the focus can be on a part of the select: its value or its arrow)
        if (mState == State::Open && selectOf(event.GetTargetElement()) == mSelect.get())
        {
            requestClose();
        }
        break;
    case Rml::EventId::Scroll:
        if (mState == State::Open)
        {
            requestClose();     // the document scrolled under the list (RmlUi's own list closes then too)
        }
        break;
    default:
        break;
    }
}

void SelectPopup::request(Rml::ElementFormControlSelect* select, Trigger trigger, const Move& move, llwchar character)
{
    if (mState == State::Open || mState == State::ClosePending || !select || !mLink)
    {
        return;
    }
    if (mState == State::Closed || mSelect.get() != select)
    {
        mRequest = Request();       // a new one (a request for another select replaces the old)
        mSelect = select->GetObserverPtr();
    }
    mState = State::Requested;
    if (trigger == Trigger::Press || mRequest.trigger == Trigger::None)
    {
        mRequest.trigger = trigger;
    }
    mRequest.step += move.step;
    mRequest.pages += move.pages;
    if (move.edge != 0)
    {
        // Home / End goes to an end of the list, wherever the earlier keys of this request would have taken the cursor:
        // they are dropped (Page Up and then End is End, not a page up from the end).
        mRequest.edge = move.edge;
        mRequest.step = 0;
        mRequest.pages = 0;
    }
    if (character)
    {
        mRequest.chars.push_back(character);
    }
    queueIdle();
}

void SelectPopup::queueIdle()
{
    if (mIdleQueued || !mLink)
    {
        return;
    }
    mIdleQueued = true;
    std::shared_ptr<Link> link = mLink;
    doOnIdleRepeating([link]() { return link->self ? link->self->idleStep() : true; });
}

// The idle's one step (research/388 3): an asked open or close, and while the list is open the checks of what it was
// placed for. Not inside RmlUi's work.
bool SelectPopup::idleStep()
{
    if (LLRmlView::current())
    {
        return false;   // (not expected: try again next frame)
    }
    switch (mState)
    {
    case State::Requested:
        tryOpen();
        break;
    case State::ClosePending:
        closeNow();
        break;
    case State::Open:
        {
            LLRmlView* owner = mOwner.get();
            RmlPopupView* view = mView.get();
            Rml::Element* select = mSelect.get();
            bool still = owner && view && select && view->getVisible() && owner->getContext() && owner->isInVisibleChain() &&
                         select->IsVisible(true) && LLUI::getScaleFactor() == mScale;
            if (still)
            {
                S32 x = 0;
                S32 y = 0;
                owner->localPointToScreen(0, 0, &x, &y);
                const Rml::Vector2f offset = select->GetAbsoluteOffset(Rml::BoxArea::Border);
                const Rml::Vector2f size = select->GetBox().GetSize(Rml::BoxArea::Border);
                still = x == mOwnerScreenX && y == mOwnerScreenY &&
                        std::abs(offset.x - mAnchorPx.x) <= ANCHOR_TOLERANCE_PX &&
                        std::abs(offset.y - mAnchorPx.y) <= ANCHOR_TOLERANCE_PX &&
                        std::abs(size.x - mAnchorSizePx.x) <= ANCHOR_TOLERANCE_PX &&
                        std::abs(size.y - mAnchorSizePx.y) <= ANCHOR_TOLERANCE_PX;
            }
            if (!still)
            {
                closeNow();     // the window moved or was hidden, the UI scale changed, the select moved or went
            }
        }
        break;
    case State::Closed:
        break;
    }
    if (mState == State::Open)
    {
        return false;   // stays in the idle while open
    }
    mIdleQueued = false;
    return true;
}

// ---- opening ----

bool SelectPopup::readRows(Rml::ElementFormControlSelect& select, std::vector<Row>& rows) const
{
    rows.clear();
    size_t left_out = 0;
    const int count = select.GetNumOptions();
    for (int i = 0; i < count; ++i)
    {
        Rml::Element* option = select.GetOption(i);
        if (!option || option->GetComputedValues().display() == Rml::Style::Display::None)
        {
            continue;   // (a data-for template)
        }
        if (rows.size() >= MAX_ROWS)
        {
            ++left_out;
            continue;
        }
        Row row;
        row.option = i;
        row.value = option->GetAttribute<Rml::String>("value", Rml::String());
        collectText(option, row.text);
        LLStringUtil::trim(row.text);
        row.text = utf8str_truncate(row.text, MAX_ROW_TEXT_BYTES);
        row.selectable = !option->HasAttribute("disabled");
        rows.push_back(std::move(row));
    }
    const bool any = !rows.empty();
    if (left_out > 0)
    {
        // A select with so many options is not a drop-down: the list ends with a row that says how many are not shown.
        LL_WARNS_ONCE("RmlUi") << "RmlUi: a select with more than " << MAX_ROWS << " options: the list is cut" << LL_ENDL;
        LLStringUtil::format_map_t args;
        args["[COUNT]"] = std::to_string(left_out);
        Row more;
        more.option = -1;
        more.text = LLTrans::getString(MORE_STRING, args);
        more.selectable = false;
        rows.push_back(std::move(more));
    }
    return any;
}

U64 SelectPopup::rowsMark(const std::vector<Row>& rows)
{
    U64 hash = 14695981039346656037ULL;
    for (const Row& row : rows)
    {
        hash = fnv(hash, row.value);
        hash = fnv(hash, row.text);
        hash = (hash ^ (U64)(row.option * 2 + (row.selectable ? 1 : 0))) * 1099511628211ULL;
    }
    return hash;
}

bool SelectPopup::ensureView(LLRmlView* owner)
{
    if (mView.get())
    {
        return true;
    }
    LLRmlView::Params params;
    params.name("select_popup");
    params.document(POPUP_DOCUMENT);
    params.rect(LLRect(0, 20, 20, 0));
    params.visible(false);
    params.tab_stop(false);
    params.follows.flags(FOLLOWS_LEFT | FOLLOWS_TOP);
    RmlPopupView* view = LLUICtrlFactory::create<RmlPopupView>(params, owner);
    if (!view)
    {
        return false;
    }
    view->setClient(this);
    if (!view->start())
    {
        LL_WARNS("RmlUi") << "RmlUi: the list of the selects (" << POPUP_DOCUMENT << ") could not be made: the selects keep "
                          << "RmlUi's own list" << LL_ENDL;
        view->setClient(nullptr);
        owner->removeChild(view);
        delete view;
        return false;
    }
    mView = view->getPopupHandle();
    return true;
}

bool SelectPopup::measure(RmlPopupView& view, const LLVector2& scale)
{
    LLRmlView::Scope scope(&view);
    Rml::Context* context = view.getContext();
    Rml::Element* list = listOf(&view);
    Rml::ElementDocument* document = view.getDocument();
    if (!context || !list || !document)
    {
        return false;
    }
    while (Rml::Element* last = list->GetLastChild())
    {
        list->RemoveChild(last);    // (the rows of the last time: the document is small while it is measured)
    }
    context->SetDensityIndependentPixelRatio(scale.mV[VY]);     // (the host sets it again at its next frame)
    context->Update();
    mSidePadding = FALLBACK_SIDE_PADDING;
    mBorder = FALLBACK_BORDER;
    mScrollbarWidth = FALLBACK_SCROLLBAR;
    mVerticalPadding = FALLBACK_VERTICAL_PADDING;
    if (Rml::Element* probe = document->GetElementById("probe"))
    {
        // #probe has the width of --combo-pad, the height of --scroll-width and the top padding of --combo-radius, and
        // the body is the list's frame (popup.rcss): the list's own sizes, so this code writes none of them.
        const Rml::Box& box = probe->GetBox();
        const Rml::Vector2f content = box.GetSize(Rml::BoxArea::Content);
        if (content.x > 0.f && content.y > 0.f)
        {
            mSidePadding = ll_round(content.x / scale.mV[VX]);
            mScrollbarWidth = ll_round(content.y / scale.mV[VY]);
            mVerticalPadding = ll_round(box.GetEdge(Rml::BoxArea::Padding, Rml::BoxEdge::Top) / scale.mV[VY]);
            mBorder = ll_round(document->GetBox().GetEdge(Rml::BoxArea::Border, Rml::BoxEdge::Left) / scale.mV[VX]);
        }
    }
    // The text is measured in the list's own document (its font size and the dp ratio it draws with), not the select's.
    mWidestTextPx = 0;
    for (const Row& row : mRows)
    {
        mWidestTextPx = llmax(mWidestTextPx, Rml::ElementUtilities::GetStringWidth(list, row.text));
    }
    return true;
}

bool SelectPopup::placeView(LLRmlView* owner, Rml::ElementFormControlSelect& select, RmlPopupView& view)
{
    Rml::Context* context = owner->getContext();
    const LLVector2 scale = LLUI::getScaleFactor();
    if (!context || scale.mV[VX] <= 0.f || scale.mV[VY] <= 0.f || !measure(view, scale))
    {
        return false;
    }
    // The select's box in the owner's UI units (y up), then on the screen.
    const Rml::Vector2f offset = select.GetAbsoluteOffset(Rml::BoxArea::Border);
    const Rml::Vector2f size = select.GetBox().GetSize(Rml::BoxArea::Border);
    const F32 context_height = (F32)context->GetDimensions().y;
    const LLRect anchor_local(llfloor(offset.x / scale.mV[VX]), llceil((context_height - offset.y) / scale.mV[VY]),
                              llceil((offset.x + size.x) / scale.mV[VX]),
                              llfloor((context_height - offset.y - size.y) / scale.mV[VY]));
    LLRect anchor;
    owner->localRectToScreen(anchor_local, &anchor);
    LLView* root = owner->getRootView();
    if (!root)
    {
        return false;
    }
    const LLRect area = plus_popup_area(root, owner, anchor_local.mLeft, anchor_local.mBottom, root->getLocalRect());

    const S32 frame = 2 * mBorder + 2 * mVerticalPadding;
    const S32 text_width = llceil((F32)mWidestTextPx / scale.mV[VX]) + 2 * mSidePadding + 2 * mBorder + 1;
    mRowHeight = LLFontGL::getFontSansSerifSmall()->getLineHeight() + ROW_PADDING;
    const S32 content_height = (S32)mRows.size() * mRowHeight + frame + HEIGHT_SLACK;
    popuplogic::PlaceLimits limits;
    limits.max_width = MAX_POPUP_WIDTH;
    limits.area_margin = AREA_MARGIN;
    limits.scrollbar_width = mScrollbarWidth;
    auto box_of = [](const LLRect& r)
    {
        popuplogic::Box box;
        box.left = r.mLeft;
        box.top = r.mTop;
        box.right = r.mRight;
        box.bottom = r.mBottom;
        return box;
    };
    const popuplogic::Box placed = popuplogic::placeList(box_of(area), box_of(anchor), text_width, content_height,
                                                         mRowHeight + frame, limits);
    LLRect local;
    owner->screenRectToLocal(LLRect(placed.left, placed.top, placed.right, placed.bottom), &local);
    view.setOrigin(local.mLeft, local.mBottom);
    view.reshape(local.getWidth(), local.getHeight());
    mRowsPerPage = llmax(1, (local.getHeight() - frame) / mRowHeight);

    mAnchorPx = offset;
    mAnchorSizePx = size;
    mScale = scale;
    owner->localPointToScreen(0, 0, &mOwnerScreenX, &mOwnerScreenY);
    return true;
}

void SelectPopup::setOpenLook(Rml::ElementFormControlSelect& select, bool open)
{
    select.SetClass("popup-open", open);
    // Inline: the select's own style sheet (a window's .tczone, charis.rcss's .ps-select) has its rules for the border
    // and the arrow, which a rule of ours could not beat (RmlUi has no !important); the accent is what has the focus.
    if (open)
    {
        select.SetProperty("border-color", "var(--accent)");
    }
    else
    {
        select.RemoveProperty("border-color");
    }
    const int count = select.GetNumChildren(true);
    for (int i = 0; i < count; ++i)
    {
        Rml::Element* part = select.GetChild(i);
        if (part && part->GetTagName() == "selectarrow")
        {
            if (open)
            {
                part->SetProperty("decorator", CHEVRON_OPEN);
            }
            else
            {
                part->RemoveProperty("decorator");
            }
        }
    }
}

void SelectPopup::buildRows(RmlPopupView& view)
{
    LLRmlView::Scope scope(&view);
    Rml::Element* list = listOf(&view);
    if (!list)
    {
        return;
    }
    Rml::ElementDocument* document = view.getDocument();
    document->SetProperty("--popup-row-height", std::to_string(mRowHeight) + "dp");
    while (Rml::Element* last = list->GetLastChild())
    {
        list->RemoveChild(last);
    }
    if (mRows.size() > WARN_ROWS)
    {
        LL_WARNS_ONCE("RmlUi") << "RmlUi: a select with " << mRows.size() << " options: the list is slow to open" << LL_ENDL;
    }
    for (size_t i = 0; i < mRows.size(); ++i)
    {
        Rml::ElementPtr row = document->CreateElement("div");
        std::string classes = "popup-row";
        if (!mRows[i].selectable)
        {
            classes += " disabled";
        }
        if ((int)i == mCursor)
        {
            classes += " cursor";
        }
        row->SetClassNames(classes);
        if (Rml::ElementPtr text = document->CreateTextNode(mRows[i].text))
        {
            row->AppendChild(std::move(text));
        }
        list->AppendChild(std::move(row));
    }
    list->SetScrollTop(0.f);
}

void SelectPopup::tryOpen()
{
    const Request wanted = std::move(mRequest);
    mRequest = Request();
    mState = State::Closed;     // until it opens
    LLRmlView* owner = mOwner.get();
    auto* box = rmlui_dynamic_cast<Rml::ElementFormControlSelect*>(mSelect.get());
    mSelect = Rml::ObserverPtr<Rml::Element>();
    if (!owner || !box || !owner->getContext() || !owner->isInVisibleChain() || box->IsDisabled() || !box->IsVisible(true))
    {
        return;     // the window or the select went or was hidden since the request
    }
    {
        // The select (or a part of it) still has the focus: a blur since the request (the window lost the focus, a click
        // elsewhere) cancels it. A press has focused what it hit before this: Context::ProcessMouseButtonDown focuses the
        // pressed element first and only then sends "mousedown", the event this request came from.
        Rml::Element* focus = owner->getContext()->GetFocusElement();
        if (!focus || selectOf(focus) != box)
        {
            return;
        }
    }
    if (!ensureView(owner))
    {
        mUnavailable = true;    // the list could not be made: the selects keep RmlUi's own from now on
        return;
    }
    RmlPopupView* view = mView.get();
    {
        LLRmlView::Scope scope(owner);
        if (!readRows(*box, mRows))
        {
            return;
        }
        mSearch.clear();    // (a search of an earlier list does not go on; typed letters of this request are replayed below)
        mLabelsReady = false;
        mOpenValue = box->GetValue();
        mOptionsMark = rowsMark(mRows);
        mCursor = popuplogic::rowOfOption(mRows, box->GetSelection());
        if (!placeView(owner, *box, *view))
        {
            return;
        }
        // The key's move, from the value the select has now (the page is known now that the list is placed). The XUI combo
        // box opens for an arrow whether or not the cursor moved, for Page Up / Down and Home / End only when it did.
        const int before = mCursor;
        if (wanted.edge != 0)
        {
            // (no row that can be chosen: -1, and the cursor stays where it is)
            const int end = wanted.edge < 0 ? popuplogic::stepRows(mRows, -1, 1)
                                            : popuplogic::stepRows(mRows, (int)mRows.size(), -1);
            if (end >= 0)
            {
                mCursor = end;
            }
        }
        const int total = wanted.step + wanted.pages * pageRows();
        if (total != 0)
        {
            mCursor = popuplogic::stepRows(mRows, mCursor, total);
        }
        if (wanted.step == 0 && (wanted.edge != 0 || wanted.pages != 0) && mCursor == before)
        {
            return;     // Page Up / Down, Home / End that move nothing do not open it
        }
        if (!wanted.chars.empty())
        {
            bool matched = false;
            for (const llwchar c : wanted.chars)
            {
                matched = typeAhead(c) || matched;
            }
            if (!matched && wanted.step == 0 && wanted.pages == 0 && wanted.edge == 0)
            {
                return;     // letters that match no row: the list does not open
            }
        }
        mSelect = box->GetObserverPtr();
        setOpenLook(*box, true);
    }
    buildRows(*view);
    mChoose = -1;
    mPressOnRow = false;
    mScrollToCursor = mCursor >= 0;
    mScrollCenter = true;
    view->markDirty();
    owner->markDirty();

    // XUI's order (llcombobox.cpp showList / onButtonMouseDown): the keyboard focus, then show, add to the popup layer.
    view->setFocus(true);
    view->setVisible(true);
    LLUI::getInstance()->addPopup(view);
    mState = State::Open;
    if (wanted.trigger == Trigger::Press && owner->hasMouseCapture())
    {
        // The press is still held: its release chooses the row it is over (the owner's press ends without a click:
        // its onMouseCaptureLost, rmlhost.cpp).
        mHandedPress = true;
        gFocusMgr.setMouseCapture(view);
    }
}

// ---- closing ----

void SelectPopup::requestClose()
{
    switch (mState)
    {
    case State::Closed:
    case State::ClosePending:
        break;
    case State::Requested:
        mState = State::Closed;
        mSelect = Rml::ObserverPtr<Rml::Element>();
        mRequest = Request();
        break;
    case State::Open:
        if (LLRmlView::current())
        {
            mState = State::ClosePending;   // inside RmlUi's work: the idle closes it
            queueIdle();
        }
        else
        {
            closeNow();
        }
        break;
    }
}

void SelectPopup::closeNow()
{
    if (mState == State::Closed)
    {
        return;
    }
    if (mState == State::Requested)
    {
        mState = State::Closed;
        mSelect = Rml::ObserverPtr<Rml::Element>();
        mRequest = Request();
        return;
    }
    mState = State::Closed;         // 1. first: removePopup tells the list it lost the top, which asks to close again
    Rml::ObserverPtr<Rml::Element> select = mSelect;
    mSelect = Rml::ObserverPtr<Rml::Element>();
    mChoose = -1;
    mPressOnRow = false;
    mHandedPress = false;
    LLRmlView* owner = mOwner.get();
    if (RmlPopupView* view = mView.get())
    {
        if (LLUI::instanceExists())
        {
            LLUI::getInstance()->removePopup(view);     // 2. (the viewer's shutdown may have ended LLUI before a window)
        }
        if (view->hasMouseCapture())
        {
            gFocusMgr.removeMouseCaptureWithoutCallback(view);  // 3.
        }
        // 4. The keyboard goes back to the window, not by setFocus (the list is the owner's child, so the owner "has" it
        // already). The XUI combo box's hideList leaves it on its hidden list; here that list would be the keyboard's
        // (acceptsTextInput of a hidden view: false), so the window's text fields would not hear the IME or the keys.
        if (view->hasFocus())
        {
            if (owner && owner->isInVisibleChain() && owner->getEnabled())
            {
                gFocusMgr.setKeyboardFocus(owner);
            }
            else
            {
                view->setFocus(false);
            }
        }
        view->setVisible(false);                        // 5.
    }
    if (owner)
    {
        if (auto* box = rmlui_dynamic_cast<Rml::ElementFormControlSelect*>(select.get()))      // 6.
        {
            LLRmlView::Scope scope(owner);
            setOpenLook(*box, false);
        }
        owner->markDirty();
    }
}

// ---- the list's own events ----

void SelectPopup::popupEvent(Rml::Event& event)
{
    if (mState != State::Open)
    {
        return;
    }
    Rml::Element* list = listOf(mView.get());
    if (!list || (event.GetId() != Rml::EventId::Mousedown && event.GetId() != Rml::EventId::Mouseup) ||
        event.GetParameter<int>("button", -1) != 0)
    {
        return;
    }
    // The row the event's target is in (an index of mRows), -1 for the scroll bar or the border.
    int row = -1;
    for (Rml::Element* element = event.GetTargetElement(); element && row < 0; element = element->GetParentNode())
    {
        if (element->GetParentNode() == list)
        {
            for (int i = 0; i < list->GetNumChildren(); ++i)
            {
                if (list->GetChild(i) == element)
                {
                    row = i;
                }
            }
            break;
        }
    }
    if (event.GetId() == Rml::EventId::Mousedown)
    {
        mPressOnRow = row >= 0;     // a press that began on the scroll bar does not choose where it ends
    }
    else if ((mPressOnRow || mHandedPress) && row >= 0 && row < (int)mRows.size() && mRows[row].selectable)
    {
        mChoose = row;
    }
}

void SelectPopup::popupUpdated()
{
    if (!mScrollToCursor || mState != State::Open)
    {
        return;
    }
    mScrollToCursor = false;
    Rml::Element* list = listOf(mView.get());
    if (list && mCursor >= 0 && mCursor < list->GetNumChildren())
    {
        list->GetChild(mCursor)->ScrollIntoView(
            Rml::ScrollIntoViewOptions(mScrollCenter ? Rml::ScrollAlignment::Center : Rml::ScrollAlignment::Nearest,
                                       Rml::ScrollAlignment::Nearest, Rml::ScrollBehavior::Instant,
                                       Rml::ScrollParentage::Closest));
    }
}

void SelectPopup::popupMouseUp()
{
    if (mState != State::Open)
    {
        mChoose = -1;
        mPressOnRow = false;
        return;
    }
    releaseHandedCapture();
    const int row = mChoose;
    mChoose = -1;
    mPressOnRow = false;
    if (row >= 0)
    {
        choose(row);
    }
}

void SelectPopup::releaseHandedCapture()
{
    if (!mHandedPress)
    {
        return;
    }
    mHandedPress = false;       // (the list's own press, if it was a press on the list, is returned by RmlUi's host)
    RmlPopupView* view = mView.get();
    if (view && view->hasMouseCapture())
    {
        gFocusMgr.setMouseCapture(nullptr);
    }
}

void SelectPopup::popupLost()
{
    requestClose();
}

void SelectPopup::popupTopLost()
{
    if (mState == State::Open)
    {
        // A press outside (or the app lost the focus) took the top: the press goes on to the window, where it must not
        // open the list again if it is on the select this list was for (the same select pressed again only closes).
        mClosedFrame = LLFrameTimer::getFrameCount();
        mClosedSelect = mSelect;
    }
    requestClose();
}

void SelectPopup::popupCaptureLost()
{
    mHandedPress = false;   // (not mChoose or mPressOnRow: the list's own press releases its capture on the way to popupMouseUp)
}

bool SelectPopup::popupKey(KEY key, MASK mask)
{
    if (mState != State::Open)
    {
        return false;
    }
    if (key == KEY_ESCAPE && mask == MASK_NONE)
    {
        requestClose();
        return true;
    }
    if (mask & (MASK_CONTROL | MASK_ALT))
    {
        return false;
    }
    switch (key)
    {
    case KEY_UP:
    case KEY_PAD_UP:
        moveCursor(-1);
        return true;
    case KEY_DOWN:
    case KEY_PAD_DOWN:
        moveCursor(1);
        return true;
    case KEY_PAGE_UP:
    case KEY_PAD_PGUP:
        moveCursor(-pageRows());
        return true;
    case KEY_PAGE_DOWN:
    case KEY_PAD_PGDN:
        moveCursor(pageRows());
        return true;
    case KEY_HOME:
    case KEY_PAD_HOME:
        moveCursorToEnd(-1);
        return true;
    case KEY_END:
    case KEY_PAD_END:
        moveCursorToEnd(1);
        return true;
    case KEY_RETURN:
    case KEY_PAD_RETURN:
        if (mCursor >= 0 && mCursor < (int)mRows.size() && mRows[mCursor].selectable)
        {
            choose(mCursor);
        }
        else
        {
            requestClose();     // (no row, or one that cannot be chosen: it closes without choosing)
        }
        return true;
    case KEY_TAB:
        requestClose();
        return false;   // the key goes on to the window (the focus has gone back to it): the next field
    default:
        return false;
    }
}

bool SelectPopup::popupChar(llwchar uni_char)
{
    if (mState != State::Open || uni_char < 0x20 || uni_char == 0x7f)
    {
        return false;
    }
    if (uni_char != ' ')
    {
        typeAhead(uni_char);
    }
    return true;
}

// ---- the cursor and choosing ----

void SelectPopup::moveCursor(int step)
{
    setCursor(popuplogic::stepRows(mRows, mCursor, step), true);
}

void SelectPopup::moveCursorToEnd(int direction)
{
    const int end = direction < 0 ? popuplogic::stepRows(mRows, -1, 1) : popuplogic::stepRows(mRows, (int)mRows.size(), -1);
    if (end >= 0)
    {
        setCursor(end, true);   // (no row that can be chosen: the cursor stays, and keeps its highlight)
    }
}

void SelectPopup::setCursor(int row, bool scroll)
{
    if (row == mCursor)
    {
        return;
    }
    RmlPopupView* view = mView.get();
    if (Rml::Element* list = listOf(view))
    {
        if (mCursor >= 0 && mCursor < list->GetNumChildren())
        {
            list->GetChild(mCursor)->SetClass("cursor", false);
        }
        if (row >= 0 && row < list->GetNumChildren())
        {
            list->GetChild(row)->SetClass("cursor", true);
        }
    }
    mCursor = row;
    if (scroll && row >= 0)
    {
        mScrollToCursor = true;
        mScrollCenter = false;
    }
    if (view)
    {
        view->markDirty();
    }
}

// The XUI list's type-ahead (llscrolllistctrl.cpp handleUnicodeCharHere, selectItemByPrefix): the letters typed in a
// row (the setting TypeAheadTimeout) are the prefix of the first row that can be chosen; the same letter again goes
// to the next row that starts with it. True when the cursor was put on a row.
bool SelectPopup::typeAhead(llwchar uni_char)
{
    static LLUICachedControl<F32> type_ahead_timeout("TypeAheadTimeout", 0);
    if (mRows.empty())
    {
        return false;
    }
    if (!mLabelsReady)
    {
        // The rows' texts, lower case and trimmed (the rows cannot change while the list is open: ownerUpdated closes it).
        mLabels.clear();
        mLabels.reserve(mRows.size());
        for (const Row& row : mRows)
        {
            LLWString text = utf8str_to_wstring(row.text);
            LLWStringUtil::toLower(text);
            LLWStringUtil::trim(text);
            mLabels.push_back(std::move(text));
        }
        mLabelsReady = true;
    }
    if (mSearchTimer.getElapsedTimeF32() > type_ahead_timeout)
    {
        mSearch.clear();
    }
    uni_char = LLStringOps::toLower(uni_char);
    const LLWString target = mSearch + uni_char;
    int found = popuplogic::findByPrefix(mRows, mLabels, target);
    if (found >= 0)
    {
        mSearch = target;
    }
    else if (std::all_of(target.begin(), target.end(), [uni_char](llwchar c) { return c == uni_char; }))
    {
        // the same letter again: the next row after the cursor that starts with it, round the list once
        found = popuplogic::nextByFirstChar(mRows, mLabels, mCursor, uni_char);
    }
    if (found < 0)
    {
        return false;
    }
    mSearchTimer.reset();
    setCursor(found, true);
    return true;
}

void SelectPopup::choose(int row)
{
    if (mState != State::Open || row < 0 || row >= (int)mRows.size() || !mRows[row].selectable || mRows[row].option < 0)
    {
        return;
    }
    const int option = mRows[row].option;
    const Rml::ObserverPtr<Rml::Element> select = mSelect;    // (closeNow forgets it)
    const LLHandle<LLRmlView> owner = mOwner;
    closeNow();
    // Last: the window's "change" listeners run in it and may close the window, or this (nothing is used after it).
    LLRmlView* host = owner.get();
    auto* box = rmlui_dynamic_cast<Rml::ElementFormControlSelect*>(select.get());
    if (!host || !box || box->IsDisabled() || !host->getContext())
    {
        return;
    }
    {
        LLRmlView::Scope scope(host);
        box->SetSelection(option);      // "change" when the value is another (the data binding and the window's listener)
    }
    if (LLRmlView* again = owner.get())
    {
        again->markDirty();
    }
}

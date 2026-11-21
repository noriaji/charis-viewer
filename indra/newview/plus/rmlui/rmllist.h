/**
 * @file rmllist.h
 * @brief RmlList: a list of one-line rows with selection, for RmlUi windows (research/386).
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

// What an XUI scroll_list does (one column, or several with a heading: setColumns()), for a few hundred rows at most
// (every row is an element; a long list wants the windowed rows of rmldemo.cpp): click, Ctrl+click and Shift+click
// select (multi), the arrows, PageUp/PageDown and Ctrl+A move and select when the list has the focus, a double click
// activates, a right press selects the row (when it was not selected) and reports ContextMenu, a rebuild keeps the
// selection (by the row's key) and the scroll position. A change of the rows or of their order writes into the elements
// that are there (the i-th element shows the i-th row; the ones at the end are added or removed): only the changed
// cells are written, the listeners, the hover and a press in progress stay (a list is built again only for the first
// rows, the columns and an empty list). Typing the first letters of a row (letters and digits of the keys, the search
// column, TypeAheadTimeout) selects it; Backspace shortens the typed text; Ctrl+C copies the selected rows (the cells
// of the shown columns joined by ", ", a line a row; the XUI list gives the hidden columns' too) unless the window took
// Ctrl+C (setCopyHandled(): the event is Result::Copy). Striped rows, the selection and the hover come from charis.rcss
// (.ps-list).
//
// In the document (the heading is optional and a plain element):
//     <div class="ps-list-box">
//         <div class="ps-list-head">[[folder]]</div>
//         <div id="list" class="ps-list" tabindex="0"></div>
//     </div>
//
// In the window (the contract with the host: RmlFloater's hooks):
//     afterLoad:     mList.attach(document, "list", true); host->listen(el, "click"); "dblclick"; "keydown" on the list;
//                    "mousedown" on the list (the left press selects the row at once, as the XUI list; the right press
//                    reports ContextMenu). A window that does not listen to it selects at the click (a list of one
//                    mouse button only: no ContextMenu, and a row may be missed when the order changes under the
//                    press)
//     beforeUpdate:  mList.apply();            // the rows are changed here only (never inside an event)
//     afterUpdate:   mList.afterUpdate();      // also moves the heading to the list's scroll bar (syncBar): call it for a
//                                              // list with columns always (without it the heading is a bar's width off when
//                                              // the bar appears); a scroll position kept over a rebuild is put back here
//     onEvent:       if (id == "list") switch (mList.handleEvent(event)) { ... }
//     anywhere:      mList.setRows(rows);      // a list that did not change is not rebuilt (cheap to call often)
//     unbind:        mList.detach();
// A window enables its buttons from the selection (selectedKeys()) in beforeUpdate, not in the event.
//
// Also here: fillSelect(), for a <select> (a drop-down) whose options come from C++, and the escape for a text
// put in as RML.

#ifndef PLUS_RMLLIST_H
#define PLUS_RMLLIST_H

#include "rmlcommon.h"

#include <string>

#include "llstring.h"
#include <vector>

namespace plus::rmlui
{
    // A text as RML text (& < >, the braces of a data expression and "[", so that "[[name]]" is not taken for a word of the
    // window: rmlhost.h translate runs before the entities are read). For a text that comes from outside.
    std::string escapeRml(const std::string& text);

    // A button enabled or disabled: the "disabled" attribute, which charis.rcss shows (button[disabled]: the disabled step).
    // RmlUi 6.3 has no :disabled for a button and the attribute stops no click: the window ignores the clicks of a disabled
    // button itself (isElementDisabled at the head of its onEvent).
    void setElementDisabled(Rml::Element* element, bool disabled);
    bool isElementDisabled(const Rml::Element* element);

    // The options of a <select>: "value" is what the window reads back. selected_value: the one shown (the first when it
    // is not among them). Removes the old options. Not for use inside RmlUi's event of that select.
    struct SelectOption
    {
        std::string value;
        std::string text;
    };
    void fillSelect(Rml::ElementFormControlSelect& box, const std::vector<SelectOption>& options,
                    const std::string& selected_value);

    class RmlList
    {
    public:
        // One cell of a row of a list with columns (setColumns()).
        struct Cell
        {
            std::string text;
            bool bold = false;
            bool italic = false;
            bool has_colour = false;
            unsigned colour = 0;        // 0xRRGGBB, when has_colour
            double sort_value = 0;      // what a SortKind::Number column sorts by
            bool operator==(const Cell& other) const
            {
                return text == other.text && bold == other.bold && italic == other.italic &&
                       has_colour == other.has_colour && colour == other.colour && sort_value == other.sort_value;
            }
        };
        struct Row
        {
            std::string key;        // what the window knows the row by (an id); unique
            std::string text;       // a list without columns: the row's text
            bool italic = false;
            std::vector<Cell> cells;    // a list with columns: one per column of setColumns() (hidden ones too, in order)
            bool pinned_first = false;  // stays above the other rows whatever the sort column and direction (the XUI's hidden "0" key)
            bool operator==(const Row& other) const
            {
                return key == other.key && text == other.text && italic == other.italic && cells == other.cells &&
                       pinned_first == other.pinned_first;
            }
        };
        enum class SortKind
        {
            None,       // the heading does not sort
            Text,       // the cell's text, upper and lower case alike (LLStringUtil::compareDict)
            Number,     // the cell's sort_value
        };
        struct Column
        {
            std::string id;
            std::string title;          // the heading (plain text)
            std::string tip;            // its tool tip (plain text; empty: none)
            int width_dp = 0;           // 0: the columns of this kind share what the others leave, equally
            bool visible = true;
            SortKind sort = SortKind::Text;
            bool operator==(const Column& other) const
            {
                return id == other.id && title == other.title && tip == other.tip && width_dp == other.width_dp &&
                       visible == other.visible && sort == other.sort;
            }
        };
        enum class Result
        {
            None,
            Selection,      // the selection changed
            Activate,       // a double click on a row (activatedKey())
            Sort,           // a click on a heading changed the order (sortColumn(), sortAscending())
            ContextMenu,    // a right press on a row (contextKey()): the row is selected first when it was not selected
            Copy,           // Ctrl+C with setCopyHandled(true): the window copies (the key is taken)
        };

        ~RmlList() { detach(); }

        // The element of the list (a div, its id) in the document. multi: Ctrl / Shift select more than one row.
        void attach(Rml::ElementDocument& document, const char* id, bool multi);
        // The element is gone with the document (or the window closes): forgets it. Idempotent.
        void detach();

        // The new rows. Nothing is rebuilt when they equal the old ones; a rebuild keeps the selected keys and the scroll.
        void setRows(std::vector<Row> rows);
        // Columns: a heading row and cells (the rows' cells follow the order of the columns, hidden ones included).
        // Without them the list is of one-line rows. The heading is a plain element of the document, outside the list,
        // and its click is listened to like the list's (host->listen(headElement(), "click")). A column's visible flag
        // shows or hides it; equal columns set again change nothing.
        //     <div class="ps-list-box">
        //         <div id="list_head" class="ps-list-head"></div>
        //         <div id="list" class="ps-list" tabindex="0"></div>
        //     </div>
        void attachHead(Rml::ElementDocument& document, const char* head_id);
        Rml::Element* headElement() const { return mHead.get(); }
        void setColumns(std::vector<Column> columns);
        // The order of the rows (a click on a heading does it too: ascending the first time, then the other way
        // round). An id that is not a visible, sortable column: the order of setRows.
        void sortBy(const std::string& column_id, bool ascending);
        std::string sortColumn() const { return mSortId; }
        bool sortAscending() const { return mSortAscending; }

        // What an empty list shows in place of rows (a dim line; not a row: it cannot be selected). Default: nothing.
        void setEmptyText(const std::string& text);
        // The rows into the document when they changed. Not inside an event of the list.
        void apply();
        // After Context::Update (the host's after_update hook): the heading follows the list's scroll bar (the bar is there
        // only while the rows overflow; a list with columns: the heading gets a spacer of the bar's width, and the layout
        // is made again at once when that changed), and a scroll position kept over a rebuild is put back here, when the
        // new rows have their layout.
        void afterUpdate();

        // The list's click, dblclick and keydown. Inside the event: only classes and attributes change.
        Result handleEvent(Rml::Event& event);

        // The row with this key is selected (alone) when the rows have been given to the document (apply()); a key
        // that is not among them selects nothing. For a window that chooses the first row.
        void selectKey(const std::string& key);
        // Every row selected (a multi-select list; Ctrl+A of a window that is not the list's own key).
        void selectEverything();
        // The column a typed prefix is looked for in (the XUI list's search_column; default: the first shown one).
        void setSearchColumn(const std::string& column_id);
        // Ctrl+C in the list: the window copies it (Result::Copy) instead of the list's selectedCsv(). Default: the list.
        void setCopyHandled(bool handled) { mCopyHandled = handled; }
        // The selected rows as Ctrl+C gives them (the texts of the shown columns joined by ", ", a line a row; the
        // XUI list's has the hidden columns too).
        std::string selectedCsv() const;
        std::vector<std::string> selectedKeys() const;
        size_t selectedCount() const;
        std::string activatedKey() const { return mActivated; }
        std::string contextKey() const { return mContext; }
        // Selects these rows (the keys that are among the rows) in place of the selection now; applied at the next apply()
        // (after the rows are in the document). The selection version changes when the selection does.
        void setSelectedKeys(const std::vector<std::string>& keys);
        // Scrolls the row with this key into view (the least scroll), at the next afterUpdate() (when the layout is there).
        void ensureVisible(const std::string& key);
        // Bumped at each change of the selection (a window compares it, to update a button).
        unsigned selectionVersion() const { return mSelectionVersion; }

    private:
        int rowOf(Rml::Element* target) const;
        enum class Pick { Replace, Toggle, Range };
        void select(int index, Pick pick);
        void moveSelection(int step, bool shift);
        void selectAll();
        void paintSelection();
        void applySelection();
        bool updateInPlace(const std::vector<Row>& rows);
        void writeRow(Rml::Element* element, const Row& now_row, const Row& was_row) const;
        std::string rowInnerRml(const Row& row) const;
        std::string searchText(const Row& row) const;
        int findPrefix(const LLWString& prefix, int from) const;
        bool typeAhead(llwchar character);
        bool searchBackspace();
        void pageSelection(int direction, bool shift);

        void renderHead();
        void syncBar();
        std::string rowsRml() const;
        void sortRows(std::vector<Row>& rows) const;

        Rml::ObserverPtr<Rml::Element> mElement;
        Rml::ObserverPtr<Rml::Element> mHead;
        std::vector<Column> mColumns;
        std::string mSortId;
        bool mSortAscending = true;
        bool mHeadChanged = false;
        bool mFullRebuild = true;           // the next apply() builds every row (the columns, the order or the document changed)
        std::vector<Row> mApplied;          // what setRows had when the document last got its rows (not sorted)
        bool mMulti = false;
        std::vector<Row> mRows;             // what the document shows (after apply)
        std::vector<Row> mWanted;           // what setRows was given last
        std::string mEmptyText;
        std::vector<char> mSelected;        // per row of mRows
        bool mWantedChanged = false;
        bool mSelectPending = false;
        std::string mSelectKey;
        int mAnchor = -1;                       // the row a Shift range starts from (kept by its key over an apply)
        int mCursor = -1;                       // the row that was selected last (kept by its key over an apply)
        bool mCopyHandled = false;
        std::string mActivated;
        std::string mContext;
        std::vector<std::string> mSelectKeys;   // setSelectedKeys, until apply()
        bool mSelectKeysPending = false;
        bool mPressHandled = false;             // the left press selected the row: the click after it does not
        std::string mSearchColumn;              // the column the typed prefix is looked for in ("": the first shown)
        LLWString mSearchString;
        F64 mSearchAt = 0.0;
        std::string mEnsureKey;                 // ensureVisible, until afterUpdate()
        unsigned mSelectionVersion = 0;
        float mPendingScroll = -1.f;        // the scroll top to put back after the next update (-1: none)
    };
}

#endif // PLUS_RMLLIST_H

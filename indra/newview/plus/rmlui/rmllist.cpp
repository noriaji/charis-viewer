/**
 * @file rmllist.cpp
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

#include "llviewerprecompiledheaders.h"

#include "rmllist.h"

#include "rmlhost.h"

#include <algorithm>
#include <cstdio>

#include "llclipboard.h"
#include "llstring.h"
#include "lltimer.h"
#include "llui.h"

namespace plus::rmlui
{
    std::string escapeRml(const std::string& text)
    {
        std::string out;
        out.reserve(text.size());
        for (const char c : text)
        {
            switch (c)
            {
                case '&': out += "&amp;"; break;
                case '<': out += "&lt;"; break;
                case '>': out += "&gt;"; break;
                case '[': out += "&#91;"; break;
                case '{': out += "&#123;"; break;
                case '}': out += "&#125;"; break;
                default: out += c; break;
            }
        }
        return out;
    }

    void setElementDisabled(Rml::Element* element, bool disabled)
    {
        if (!element)
        {
            return;
        }
        if (disabled)
        {
            element->SetAttribute("disabled", Rml::String("disabled"));
        }
        else
        {
            element->RemoveAttribute("disabled");
        }
    }

    bool isElementDisabled(const Rml::Element* element)
    {
        return element && element->HasAttribute("disabled");
    }

    void fillSelect(Rml::ElementFormControlSelect& box, const std::vector<SelectOption>& options,
                    const std::string& selected_value)
    {
        box.RemoveAll();
        int selected = options.empty() ? -1 : 0;
        for (size_t i = 0; i < options.size(); ++i)
        {
            box.Add(escapeRml(options[i].text), options[i].value);
            if (options[i].value == selected_value)
            {
                selected = (int)i;
            }
        }
        box.SetSelection(selected);
    }

    void RmlList::attach(Rml::ElementDocument& document, const char* id, bool multi)
    {
        Rml::Element* element = document.GetElementById(id);
        mElement = element ? element->GetObserverPtr() : Rml::ObserverPtr<Rml::Element>();
        mMulti = multi;
        mRows.clear();
        mApplied.clear();
        mSelected.clear();
        mAnchor = mCursor = -1;
        mPressHandled = false;
        mWantedChanged = mWantedChanged || !mWanted.empty() || !mEmptyText.empty();     // rows given before the document was there
        mFullRebuild = true;
    }

    void RmlList::detach()
    {
        mElement = Rml::ObserverPtr<Rml::Element>();
        mHead = Rml::ObserverPtr<Rml::Element>();
        mPendingScroll = -1.f;
    }

    void RmlList::setEmptyText(const std::string& text)
    {
        mEmptyText = text;
        mWantedChanged = true;
    }

    void RmlList::setRows(std::vector<Row> rows)
    {
        mWanted = std::move(rows);
        mWantedChanged = mWantedChanged || !(mWanted == mApplied);    // (a change not applied yet stays asked for)
    }

    void RmlList::apply()
    {
        Rml::Element* list = mElement.get();
        if (!list)
        {
            return;
        }
        if (mHeadChanged)
        {
            renderHead();
        }
        if (!mWantedChanged)
        {
            applySelection();
            return;
        }
        mWantedChanged = false;
        std::vector<Row> sorted = mWanted;
        sortRows(sorted);
        const std::vector<std::string> keep = selectedKeys();
        const std::string cursor_key = (mCursor >= 0 && mCursor < (int)mRows.size()) ? mRows[mCursor].key : std::string();
        const std::string anchor_key = (mAnchor >= 0 && mAnchor < (int)mRows.size()) ? mRows[mAnchor].key : std::string();
        // The elements stay (their listeners, the hover and a press in progress with them): the i-th element takes the
        // i-th row of the new order, whatever the order was, and the elements at the end are added or removed.
        const bool in_place = updateInPlace(sorted);
        if (!in_place)
        {
            mFullRebuild = false;
            mPendingScroll = list->GetScrollTop();
            std::string rml;
            if (sorted.empty() && !mEmptyText.empty())
            {
                rml = "<div class=\"ps-list-empty\">" + escapeRml(mEmptyText) + "</div>";
            }
            mRows = sorted;
            rml += rowsRml();
            list->SetInnerRML(rml);
        }
        mApplied = mWanted;
        mRows = std::move(sorted);
        mSelected.assign(mRows.size(), 0);
        mCursor = mAnchor = -1;
        for (size_t i = 0; i < mRows.size(); ++i)
        {
            if (std::find(keep.begin(), keep.end(), mRows[i].key) != keep.end())
            {
                mSelected[i] = 1;
            }
            // (the cursor and the start of a Shift range are kept apart: a sweep must not shrink the range)
            if (!cursor_key.empty() && mRows[i].key == cursor_key)
            {
                mCursor = (int)i;
            }
            if (!anchor_key.empty() && mRows[i].key == anchor_key)
            {
                mAnchor = (int)i;
            }
        }
        if (mAnchor < 0)
        {
            mAnchor = mCursor;      // the start row went: the range starts at the cursor
        }
        if (keep.size() != selectedCount())
        {
            ++mSelectionVersion;    // a selected row went
        }
        paintSelection();
        applySelection();
    }

    // The rows' texts and cells written into the elements that are there: the i-th element shows rows[i] (what the i-th
    // showed before is mRows[i]; only what differs is written), and elements are added or removed at the end. False:
    // nothing was written and the list is built again (the first build, the columns changed, an empty list or an empty
    // result, whose line is not a row).
    bool RmlList::updateInPlace(const std::vector<Row>& rows)
    {
        Rml::Element* list = mElement.get();
        if (!list || mFullRebuild || mRows.empty() || rows.empty() || list->GetNumChildren() != (int)mRows.size())
        {
            return false;
        }
        const size_t common = std::min(rows.size(), mRows.size());
        for (size_t i = 0; i < common; ++i)
        {
            writeRow(list->GetChild((int)i), rows[i], mRows[i]);
        }
        for (size_t i = common; i < rows.size(); ++i)
        {
            Rml::ElementPtr element = list->GetOwnerDocument()->CreateElement("div");
            element->SetClass("ps-list-row", true);
            element->SetClass("grid", !mColumns.empty());
            element->SetClass("italic", rows[i].italic);
            element->SetInnerRML(rowInnerRml(rows[i]));
            list->AppendChild(std::move(element));
        }
        while ((size_t)list->GetNumChildren() > rows.size())
        {
            list->RemoveChild(list->GetChild(list->GetNumChildren() - 1));
        }
        return true;
    }

    void RmlList::writeRow(Rml::Element* element, const Row& now_row, const Row& was_row) const
    {
        if (now_row == was_row)
        {
            return;
        }
        element->SetClass("italic", now_row.italic);
        if (mColumns.empty())
        {
            if (now_row.text != was_row.text)
            {
                element->SetInnerRML(escapeRml(now_row.text));
            }
            return;
        }
        static const Cell empty;
        int child = 0;
        for (size_t c = 0; c < mColumns.size(); ++c)
        {
            if (!mColumns[c].visible)
            {
                continue;
            }
            const Cell& now = c < now_row.cells.size() ? now_row.cells[c] : empty;
            const Cell& was = c < was_row.cells.size() ? was_row.cells[c] : empty;
            Rml::Element* cell = element->GetChild(child++);
            if (!cell || now == was)
            {
                continue;
            }
            cell->SetClass("bold", now.bold);
            cell->SetClass("italic", now.italic);
            if (now.text != was.text)
            {
                cell->SetInnerRML(escapeRml(now.text));
            }
            if (now.has_colour)
            {
                char colour[16];
                snprintf(colour, sizeof(colour), "#%06x", now.colour & 0xffffffu);
                cell->SetProperty("color", colour);
            }
            else
            {
                cell->RemoveProperty("color");
            }
        }
    }

    void RmlList::setSelectedKeys(const std::vector<std::string>& keys)
    {
        mSelectKeys = keys;
        mSelectKeysPending = true;
        mSelectPending = false;
    }

    void RmlList::ensureVisible(const std::string& key)
    {
        mEnsureKey = key;
    }

    void RmlList::setSearchColumn(const std::string& column_id)
    {
        mSearchColumn = column_id;
    }

    // The text a typed prefix is looked for in: the row's text, or the search column's cell (the first shown column when
    // none was named).
    std::string RmlList::searchText(const Row& row) const
    {
        if (mColumns.empty())
        {
            return row.text;
        }
        for (size_t i = 0; i < mColumns.size() && i < row.cells.size(); ++i)
        {
            if (mColumns[i].visible && (mSearchColumn.empty() || mColumns[i].id == mSearchColumn))
            {
                return row.cells[i].text;
            }
        }
        return std::string();
    }

    // The row after `from` (wrapping) whose text begins with the prefix (case insensitive), or -1.
    int RmlList::findPrefix(const LLWString& prefix, int from) const
    {
        const int count = (int)mRows.size();
        for (int k = 1; k <= count; ++k)
        {
            const int index = ((from < 0 ? -1 : from) + k) % count;
            LLWString text = utf8str_to_wstring(searchText(mRows[index]));
            LLWStringUtil::toLower(text);
            if (text.compare(0, prefix.size(), prefix) == 0)
            {
                return index;
            }
        }
        return -1;
    }

    // A typed character (the XUI list's type-ahead: LLScrollListCtrl::handleUnicodeCharHere, TypeAheadTimeout): the
    // first row that begins with what was typed is selected; the same character again goes on to the next row with it.
    bool RmlList::typeAhead(llwchar character)
    {
        if (mRows.empty())
        {
            return false;
        }
        static LLUICachedControl<F32> type_ahead_timeout("TypeAheadTimeout", 0);
        const F64 now = LLTimer::getTotalSeconds();
        if (now - mSearchAt > type_ahead_timeout)
        {
            mSearchString.clear();
        }
        character = LLStringOps::toLower(character);
        const LLWString target = mSearchString + character;
        int index = findPrefix(target, -1);
        if (index >= 0)
        {
            mSearchString = target;
        }
        else if (target.find_first_not_of(character) == LLWString::npos)
        {
            // a repeated character: from the row after the cursor row
            index = findPrefix(LLWString(1, character), mCursor);
        }
        if (index < 0)
        {
            return false;
        }
        mSearchAt = now;
        select(index, Pick::Replace);
        return true;
    }

    // Backspace in the typed text: the shorter text is looked for. False when there was no typed text.
    bool RmlList::searchBackspace()
    {
        if (mSearchString.empty())
        {
            return false;
        }
        mSearchString.erase(mSearchString.size() - 1, 1);
        mSearchAt = LLTimer::getTotalSeconds();
        if (!mSearchString.empty())
        {
            const int index = findPrefix(mSearchString, -1);
            if (index >= 0)
            {
                select(index, Pick::Replace);
            }
        }
        return true;
    }

    // The selected rows as the XUI list's Ctrl+C gives them: the texts of the shown columns joined by ", ", a line a row
    // (the XUI list gives the hidden columns' too).
    std::string RmlList::selectedCsv() const
    {
        std::string text;
        for (size_t i = 0; i < mRows.size() && i < mSelected.size(); ++i)
        {
            if (!mSelected[i])
            {
                continue;
            }
            if (mColumns.empty())
            {
                text += mRows[i].text;
            }
            else
            {
                bool first = true;
                for (size_t c = 0; c < mColumns.size() && c < mRows[i].cells.size(); ++c)
                {
                    if (mColumns[c].visible)
                    {
                        text += (first ? "" : ", ") + mRows[i].cells[c].text;
                        first = false;
                    }
                }
            }
            text += "\n";
        }
        return text;
    }

    void RmlList::selectKey(const std::string& key)
    {
        mSelectKey = key;
        mSelectPending = true;
    }

    void RmlList::selectEverything()
    {
        const std::vector<char> before = mSelected;
        selectAll();
        if (before != mSelected)
        {
            ++mSelectionVersion;
        }
    }

    void RmlList::applySelection()
    {
        if (mSelectKeysPending)
        {
            mSelectKeysPending = false;
            const std::vector<char> before = mSelected;
            std::fill(mSelected.begin(), mSelected.end(), 0);
            bool hit = false;
            for (size_t i = 0; i < mRows.size(); ++i)
            {
                if (std::find(mSelectKeys.begin(), mSelectKeys.end(), mRows[i].key) != mSelectKeys.end())
                {
                    mSelected[i] = 1;
                    mCursor = mAnchor = (int)i;
                    hit = true;
                }
            }
            if (!hit)
            {
                mCursor = mAnchor = -1;
            }
            paintSelection();
            if (before != mSelected)
            {
                ++mSelectionVersion;
            }
        }
        if (!mSelectPending)
        {
            return;
        }
        mSelectPending = false;
        for (size_t i = 0; i < mRows.size(); ++i)
        {
            if (mRows[i].key == mSelectKey)
            {
                const std::vector<char> before = mSelected;
                select((int)i, Pick::Replace);
                if (before != mSelected)
                {
                    ++mSelectionVersion;
                }
                return;
            }
        }
    }

    namespace
    {
        // A text as the value of an attribute written in RML (double quotes).
        std::string escapeAttribute(const std::string& text)
        {
            std::string out = escapeRml(text);
            for (size_t at = out.find('"'); at != std::string::npos; at = out.find('"', at))
            {
                out.replace(at, 1, "&quot;");
            }
            return out;
        }

        std::string widthStyle(const RmlList::Column& column)
        {
            return column.width_dp > 0 ? "width: " + std::to_string(column.width_dp) + "dp;" : std::string();
        }
    }

    // The inside of a row's element: its text, or its cells.
    std::string RmlList::rowInnerRml(const Row& row) const
    {
        if (mColumns.empty())
        {
            return escapeRml(row.text);
        }
        std::string rml;
        for (size_t i = 0; i < mColumns.size(); ++i)
        {
            if (!mColumns[i].visible)
            {
                continue;
            }
            static const Cell empty;
            const Cell& cell = i < row.cells.size() ? row.cells[i] : empty;
            rml += "<div class=\"ps-cell";
            rml += cell.bold ? " bold" : "";
            rml += cell.italic ? " italic" : "";
            rml += mColumns[i].width_dp > 0 ? "" : " flex";
            rml += "\" style=\"" + widthStyle(mColumns[i]);
            if (cell.has_colour)
            {
                char colour[24];
                snprintf(colour, sizeof(colour), "color: #%06x;", cell.colour & 0xffffffu);
                rml += colour;
            }
            rml += "\">" + escapeRml(cell.text) + "</div>";
        }
        return rml;
    }

    std::string RmlList::rowsRml() const
    {
        std::string rml;
        for (const Row& row : mRows)
        {
            rml += mColumns.empty() ? "<div class=\"ps-list-row" : "<div class=\"ps-list-row grid";
            rml += row.italic ? " italic\">" : "\">";
            rml += rowInnerRml(row);
            rml += "</div>";
        }
        return rml;
    }

    void RmlList::sortRows(std::vector<Row>& rows) const
    {
        size_t index = mColumns.size();
        for (size_t i = 0; i < mColumns.size(); ++i)
        {
            if (mColumns[i].id == mSortId && mColumns[i].visible && mColumns[i].sort != SortKind::None)
            {
                index = i;
            }
        }
        if (index == mColumns.size())
        {
            return;
        }
        const bool text = mColumns[index].sort == SortKind::Text;
        const bool ascending = mSortAscending;
        static const Cell empty;
        std::stable_sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) {
            if (a.pinned_first != b.pinned_first)
            {
                return a.pinned_first;      // before the others, ascending or descending
            }
            const Cell& cell_a = index < a.cells.size() ? a.cells[index] : empty;
            const Cell& cell_b = index < b.cells.size() ? b.cells[index] : empty;
            const double order = text ? (double)LLStringUtil::compareDict(cell_a.text, cell_b.text)
                                      : cell_a.sort_value - cell_b.sort_value;
            return ascending ? order < 0 : order > 0;
        });
    }

    void RmlList::attachHead(Rml::ElementDocument& document, const char* head_id)
    {
        Rml::Element* element = document.GetElementById(head_id);
        mHead = element ? element->GetObserverPtr() : Rml::ObserverPtr<Rml::Element>();
        mHeadChanged = true;
    }

    // The heading's right edge follows the list's vertical scroll bar: the bar is there only while the rows overflow
    // (charis.rcss .ps-list: overflow-y auto), and the heading carries a spacer of the bar's width (.ps-head-bar, shown
    // by the class has-bar) so that its columns stay above the cells. Read from the layout of the update that just ran
    // (the bar's size is what the list's padding box has more than its client width); written only when the bar
    // appeared or went, and then laid out again at once (the host's updateAgain()), so that no frame shows the heading
    // one bar off. The same rows give the same state: nothing is written then.
    void RmlList::syncBar()
    {
        Rml::Element* list = mElement.get();
        Rml::Element* head = mHead.get();
        if (!list || !head)
        {
            return;
        }
        static const Rml::String bar_class("has-bar");
        const bool want = !mColumns.empty() &&
                          list->GetBox().GetSize(Rml::BoxArea::Padding).x - list->GetClientWidth() > 0.5f;
        if (head->IsClassSet(bar_class) == want)
        {
            return;
        }
        head->SetClass(bar_class, want);
        // (the host counts it and takes what RmlUi asks for in it; current() is the host whose draw this is)
        Rml::Context* context = head->GetContext();
        LLRmlView* host = LLRmlView::current();
        if (host && context && host->getContext() == context)
        {
            host->updateAgain();
        }
        else if (context)
        {
            context->Update();
        }
    }

    void RmlList::setColumns(std::vector<Column> columns)
    {
        if (columns == mColumns)
        {
            return;
        }
        mColumns = std::move(columns);
        mHeadChanged = true;
        mWantedChanged = true;      // the rows' cells follow the columns
        mApplied.clear();
        mFullRebuild = true;
    }

    void RmlList::sortBy(const std::string& column_id, bool ascending)
    {
        if (column_id == mSortId && ascending == mSortAscending)
        {
            return;
        }
        mSortId = column_id;
        mSortAscending = ascending;
        mHeadChanged = true;
        mWantedChanged = true;
        mApplied.clear();       // (the elements stay: apply() writes the new order into them)
    }

    void RmlList::renderHead()
    {
        mHeadChanged = false;
        Rml::Element* head = mHead.get();
        if (!head || mColumns.empty())
        {
            return;     // (the heading of a list without columns is the window's own text)
        }
        head->SetClass("grid", true);
        std::string rml;
        for (const Column& column : mColumns)
        {
            if (!column.visible)
            {
                continue;
            }
            rml += "<div class=\"ps-head-cell";
            rml += column.sort != SortKind::None ? " sortable" : "";
            rml += column.width_dp > 0 ? "" : " flex";
            rml += "\" col=\"" + escapeAttribute(column.id) + "\" style=\"" + widthStyle(column) + "\"";
            if (!column.tip.empty())
            {
                rml += " title=\"" + escapeAttribute(column.tip) + "\"";
            }
            rml += ">" + escapeRml(column.title);
            if (column.sort != SortKind::None && column.id == mSortId)
            {
                rml += mSortAscending ? " &#9650;" : " &#9660;";
            }
            rml += "</div>";
        }
        rml += "<div class=\"ps-head-bar\"></div>";      // (the scroll bar's width, shown while the list has its bar: syncBar)
        head->SetInnerRML(rml);
    }

    void RmlList::afterUpdate()
    {
        syncBar();      // first: the layout the scroll position is put back on is the final one
        Rml::Element* list = mElement.get();
        if (list && mPendingScroll >= 0.f)
        {
            list->SetScrollTop(mPendingScroll);
        }
        mPendingScroll = -1.f;
        if (list && !mEnsureKey.empty())
        {
            for (size_t i = 0; i < mRows.size() && (int)i < list->GetNumChildren(); ++i)
            {
                if (mRows[i].key == mEnsureKey)
                {
                    list->GetChild((int)i)->ScrollIntoView(
                        Rml::ScrollIntoViewOptions(Rml::ScrollAlignment::Nearest, Rml::ScrollAlignment::Nearest));
                    break;
                }
            }
        }
        mEnsureKey.clear();
    }

    void RmlList::paintSelection()
    {
        Rml::Element* list = mElement.get();
        if (!list)
        {
            return;
        }
        const int count = list->GetNumChildren();
        for (int i = 0; i < count && i < (int)mSelected.size(); ++i)
        {
            list->GetChild(i)->SetClass("selected", mSelected[i] != 0);
        }
    }

    int RmlList::rowOf(Rml::Element* target) const
    {
        Rml::Element* list = mElement.get();
        while (target && list && target->GetParentNode() != list)
        {
            target = target->GetParentNode();
        }
        if (!target || !list)
        {
            return -1;
        }
        const int count = list->GetNumChildren();
        for (int i = 0; i < count; ++i)
        {
            if (list->GetChild(i) == target)
            {
                return i < (int)mRows.size() ? i : -1;
            }
        }
        return -1;
    }

    void RmlList::select(int index, Pick pick)
    {
        if (index < 0 || index >= (int)mRows.size())
        {
            return;
        }
        if (mMulti && pick == Pick::Range && mAnchor >= 0 && mAnchor < (int)mRows.size())
        {
            std::fill(mSelected.begin(), mSelected.end(), 0);
            for (int i = std::min(mAnchor, index); i <= std::max(mAnchor, index); ++i)
            {
                mSelected[i] = 1;
            }
        }
        else if (mMulti && pick == Pick::Toggle)
        {
            mSelected[index] = !mSelected[index];
            mAnchor = index;
        }
        else
        {
            std::fill(mSelected.begin(), mSelected.end(), 0);
            mSelected[index] = 1;
            mAnchor = index;
        }
        mCursor = index;
        paintSelection();
        if (Rml::Element* row = mElement ? mElement->GetChild(index) : nullptr)
        {
            row->ScrollIntoView(Rml::ScrollIntoViewOptions(Rml::ScrollAlignment::Nearest, Rml::ScrollAlignment::Nearest));
        }
    }

    void RmlList::moveSelection(int step, bool shift)
    {
        if (mRows.empty())
        {
            return;
        }
        const int from = mCursor >= 0 ? mCursor : (step > 0 ? -1 : (int)mRows.size());
        select(std::clamp(from + step, 0, (int)mRows.size() - 1), shift ? Pick::Range : Pick::Replace);
    }

    // PageUp / PageDown: a page of rows (what the list shows, less one), as the XUI scroll list.
    void RmlList::pageSelection(int direction, bool shift)
    {
        Rml::Element* list = mElement.get();
        Rml::Element* first = list && list->GetNumChildren() > 0 ? list->GetChild(0) : nullptr;
        const float row_height = first ? first->GetOffsetHeight() : 0.f;
        const int page = row_height > 0.f ? std::max(1, (int)(list->GetClientHeight() / row_height) - 1) : 1;
        moveSelection(direction * page, shift);
    }

    void RmlList::selectAll()
    {
        if (mMulti)
        {
            std::fill(mSelected.begin(), mSelected.end(), 1);
            paintSelection();
        }
    }

    RmlList::Result RmlList::handleEvent(Rml::Event& event)
    {
        if (Rml::Element* head = mHead.get())
        {
            // A click on a heading: its column sorts (ascending first, then the other way round).
            Rml::Element* target = event.GetTargetElement();
            while (target && target != head && !target->HasAttribute("col"))
            {
                target = target->GetParentNode();
            }
            if (target && target != head)
            {
                if (event.GetId() == Rml::EventId::Click)
                {
                    const std::string id = target->GetAttribute<Rml::String>("col", "");
                    for (const Column& column : mColumns)
                    {
                        if (column.id == id && column.sort != SortKind::None)
                        {
                            sortBy(id, id == mSortId ? !mSortAscending : true);
                            return Result::Sort;
                        }
                    }
                }
                return Result::None;
            }
        }
        if (!mElement || mRows.empty())
        {
            return Result::None;
        }
        const std::vector<char> before = mSelected;
        const Rml::EventId type = event.GetId();
        if (type == Rml::EventId::Mousedown && event.GetParameter<int>("button", 0) == 0)
        {
            // A left press selects the row under it at once (as the XUI list), not at the click: the rows may change
            // order between the press and the release, and the click then lands on another row's element.
            mPressHandled = false;      // (a press beside a row: the click after it selects nothing either)
            const int index = rowOf(event.GetTargetElement());
            if (index < 0)
            {
                return Result::None;
            }
            mPressHandled = true;
            select(index, event.GetParameter<bool>("shift_key", false) ? Pick::Range
                          : (event.GetParameter<bool>("ctrl_key", false) ? Pick::Toggle : Pick::Replace));
        }
        else if (type == Rml::EventId::Mousedown && event.GetParameter<int>("button", 0) == 1)
        {
            // A right press: the row is selected first when it is not (as the XUI list); the window opens its menu.
            const int index = rowOf(event.GetTargetElement());
            if (index < 0)
            {
                return Result::None;
            }
            if (!mSelected[index])
            {
                select(index, Pick::Replace);
            }
            mContext = mRows[index].key;
            if (before != mSelected)
            {
                ++mSelectionVersion;
            }
            return Result::ContextMenu;
        }
        if (type == Rml::EventId::Click || type == Rml::EventId::Dblclick)
        {
            const int index = rowOf(event.GetTargetElement());
            if (index < 0)
            {
                return Result::None;
            }
            if (type == Rml::EventId::Dblclick)
            {
                // The row the second press selected: the double click is sent in the same handling of that press, to the
                // same element (a window that listens to mousedown). The key of the row, not its place.
                mActivated = mRows[index].key;
                return Result::Activate;
            }
            if (mPressHandled)
            {
                mPressHandled = false;      // the press selected it already
            }
            else
            {
                const bool shift = event.GetParameter<bool>("shift_key", false);
                const bool ctrl = event.GetParameter<bool>("ctrl_key", false);
                select(index, shift ? Pick::Range : (ctrl ? Pick::Toggle : Pick::Replace));
            }
        }
        else if (type == Rml::EventId::Keydown)
        {
            const int key = event.GetParameter<int>("key_identifier", 0);
            const bool shift = event.GetParameter<bool>("shift_key", false);
            if (key == Rml::Input::KI_UP || key == Rml::Input::KI_DOWN)
            {
                moveSelection(key == Rml::Input::KI_UP ? -1 : 1, shift);
            }
            else if (key == Rml::Input::KI_PRIOR || key == Rml::Input::KI_NEXT)
            {
                pageSelection(key == Rml::Input::KI_PRIOR ? -1 : 1, shift);
            }
            else if (key == Rml::Input::KI_HOME || key == Rml::Input::KI_END)
            {
                mCursor = key == Rml::Input::KI_HOME ? -1 : (int)mRows.size();
                moveSelection(key == Rml::Input::KI_HOME ? 1 : -1, shift);
            }
            else if (key == Rml::Input::KI_A && event.GetParameter<bool>("ctrl_key", false))
            {
                selectAll();
            }
            else if (key == Rml::Input::KI_C && event.GetParameter<bool>("ctrl_key", false))
            {
                if (mCopyHandled)
                {
                    event.StopPropagation();    // the window copies what it wants (Result::Copy)
                    return Result::Copy;
                }
                const std::string text = selectedCsv();
                if (text.empty())
                {
                    return Result::None;
                }
                const LLWString wide = utf8str_to_wstring(text);
                LLClipboard::instance().copyToClipboard(wide, 0, static_cast<S32>(wide.length()));
            }
            else if (key == Rml::Input::KI_BACK && !event.GetParameter<bool>("ctrl_key", false))
            {
                if (!searchBackspace())
                {
                    return Result::None;
                }
            }
            else if (!event.GetParameter<bool>("ctrl_key", false) && !event.GetParameter<bool>("alt_key", false) &&
                     ((key >= Rml::Input::KI_A && key <= Rml::Input::KI_Z) || (key >= Rml::Input::KI_0 && key <= Rml::Input::KI_9)))
            {
                // (typed characters do not come to a list through the host yet: the letters and digits of the keys do)
                const llwchar character = key >= Rml::Input::KI_A ? (llwchar)('a' + (key - Rml::Input::KI_A))
                                                                 : (llwchar)('0' + (key - Rml::Input::KI_0));
                if (!typeAhead(character))
                {
                    return Result::None;
                }
            }
            else
            {
                return Result::None;
            }
            event.StopPropagation();    // the document took the key (rmlhost.h keyForDocument): it does not go on to the viewer
        }
        if (before != mSelected)
        {
            ++mSelectionVersion;
            return Result::Selection;
        }
        return Result::None;
    }

    std::vector<std::string> RmlList::selectedKeys() const
    {
        std::vector<std::string> keys;
        for (size_t i = 0; i < mRows.size() && i < mSelected.size(); ++i)
        {
            if (mSelected[i])
            {
                keys.push_back(mRows[i].key);
            }
        }
        return keys;
    }

    size_t RmlList::selectedCount() const
    {
        return (size_t)std::count(mSelected.begin(), mSelected.end(), 1);
    }
}

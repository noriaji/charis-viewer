/**
 * @file photoconv_panel.cpp
 * @brief The photo screen's conversations tab (research/247, first stage, branch ps-im).
 *
 * One panel (panel_plus_photoscreen_conv.xml and the views made here) is put in each mode: the detailed mode's
 * left panel (the rail's last tab) and the simple mode's "Talk" page. From the top: the heading, the list of
 * conversations (a row each: the name and the unread count in a badge, grey for one the official notice
 * settings keep quiet), a line, the name and kind of the one chosen, its last messages (wrapped lines, the
 * newest at the bottom, the wheel goes back), and the line to type in with the send button. The input's
 * placeholder says who it goes to ("To Aiko"), and a conversation chosen brings back what was typed in it, so
 * nothing goes to the wrong person.
 *
 * Other users' text (names, messages) is only drawn as text: never put in a format string, never made a link
 * here (the second stage decides on the official chat history's links). A message is at most the official
 * 1024 bytes.
 *
 * Lifetimes: the list's rows are made again only when what they show changed, never from inside one of their
 * own callbacks (a click is handled on the next idle). The panel listens to photoconv's change signal by a
 * scoped connection, so a panel that is gone is not called.
 *
 * Speed: nothing per frame but drawing; the lines are wrapped when they or the width change. A hidden panel
 * only marks itself stale on a change and is brought up to date when shown.
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

#include "photoconv_panel.h"

#include "photoconv.h"
#include "psui/psstack.h"
#include "psui/psstyle.h"
#include "psui/pswidgets.h"

#include <algorithm>
#include <vector>

#include "llcallbacklist.h"
#include "lldbstrings.h"    // MAX_MSG_BUF_SIZE
#include "lllineeditor.h"
#include "llpanel.h"
#include "llscrollcontainer.h"
#include "lltextbox.h"
#include "lluictrlfactory.h"
#include "llview.h"

namespace plus::photoscreen::conv
{
    namespace
    {
        namespace ps    = plus::psui;
        namespace style = plus::psui::style;

        // Sizes at a window 1080 high (research/247 §3 案 A): the heading, the list's rows, the peer's two lines,
        // the history (at least), the input line; the sides as the left panel's rows.
        constexpr S32 HEAD_H        = 52;
        constexpr S32 ROW_H         = style::LIST_ROW_HEIGHT;
        constexpr S32 LIST_ROWS_MAX = 5;        // more rows scroll
        constexpr S32 LIST_X        = 6;        // the rows' inset at the sides
        constexpr S32 EMPTY_H       = 44;       // "no conversations yet", two lines
        constexpr S32 DIVIDER_H     = 9;
        constexpr S32 PEER_H        = 24;
        constexpr S32 PEER_SUB_H    = 18;
        constexpr S32 HISTORY_MIN   = 72;
        constexpr S32 HISTORY_TOP_GAP = 4;
        constexpr S32 GAP           = 8;
        constexpr S32 INPUT_H       = 30;
        constexpr S32 INPUT_EDIT_H  = 22;
        constexpr S32 INPUT_INSET   = 4;
        constexpr S32 SEND_W        = 32;
        constexpr S32 BOTTOM_PAD    = 8;
        constexpr S32 PAD_X         = style::HEAD_PAD;
        constexpr S32 LINE_GAP      = 2;        // between the history's lines
        constexpr size_t HISTORY_LINES = 30;    // the last messages shown
        constexpr S32 WRAP_MAX      = 40;       // wrapped lines of one message at most
        constexpr S32 WHEEL_LINES   = 3;
        constexpr S32 BADGE_RIGHT   = 14;       // the simple tab's badge: its middle from the button's right

        // The viewer's notes have no speaker; your own lines are "You: ...".
        std::string speakerText(const Line& line, const std::string& me)
        {
            return speakerLine(line.system ? std::string() : line.own ? me : line.from, line.text);
        }

        // ---- the last messages ----------------------------------------------------------------------

        class ConvHistory : public LLView
        {
        public:
            struct Params : public LLInitParam::Block<Params, LLView::Params>
            {
                Params() {}
            };

            // The messages to show; wrapped again only when they (or the name for your own) changed.
            void setLines(const std::vector<Line>& lines, const std::string& me)
            {
                if (lines == mSource && me == mMe)
                {
                    return;
                }
                mSource = lines;
                mMe = me;
                mWrappedWidth = -1;
            }

            // A conversation chosen: from its newest line.
            void resetScroll() { mScroll = 0; }

            void draw() override
            {
                const S32 width = getRect().getWidth();
                if (width != mWrappedWidth)
                {
                    wrap(width);
                }
                const S32 total = (S32)mWrapped.size();
                const S32 line_h = lineHeight();
                const S32 visible = getRect().getHeight() / line_h;
                if (total == 0 || visible <= 0)
                {
                    return;
                }
                mScroll = llclamp(mScroll, 0, llmax(0, total - visible));
                const F32 alpha = getDrawContext().mAlpha;
                S32 y = 0;
                for (S32 i = total - 1 - mScroll; i >= 0 && y + line_h <= getRect().getHeight(); --i)
                {
                    ps::drawText(mFont, mWrapped[i], 0.f, (F32)y, ps::faded(colorOf(mKinds[i]), alpha), LLFontGL::LEFT,
                                 LLFontGL::BOTTOM, width, true);
                    y += line_h;
                }
            }

            // The wheel goes back through the lines and never reaches the world (the camera) behind.
            bool handleScrollWheel(S32 x, S32 y, S32 clicks) override
            {
                mScroll = llmax(0, mScroll - clicks * WHEEL_LINES);    // up (negative): older lines
                return true;
            }

        protected:
            friend class LLUICtrlFactory;
            explicit ConvHistory(const Params& p) : LLView(p), mFont(ps::font(style::FONT_BODY)) {}

        private:
            enum : U8 { OTHER, OWN, SYSTEM };

            S32 lineHeight() const
            {
                return (mFont ? (S32)mFont->getLineHeight() : 14) + LINE_GAP;
            }

            static const LLColor4& colorOf(U8 kind)
            {
                return kind == OWN ? style::CONV_OWN_TEXT : kind == SYSTEM ? style::CONV_SYSTEM_TEXT : style::TEXT_WHITE;
            }

            void wrap(S32 width)
            {
                mWrapped.clear();
                mKinds.clear();
                mWrappedWidth = width;
                if (!mFont || width < 20)
                {
                    return;
                }
                for (const Line& line : mSource)
                {
                    const U8 kind = line.system ? SYSTEM : line.own ? OWN : OTHER;
                    const LLWString text = utf8str_to_wstring(speakerText(line, mMe));
                    // each paragraph of the message on its own lines, WRAP_MAX lines for the whole message
                    S32 room = WRAP_MAX;
                    size_t start = 0;
                    while (start <= text.length() && room > 0)
                    {
                        size_t end = text.find(L'\n', start);
                        if (end == LLWString::npos)
                        {
                            end = text.length();
                        }
                        LLWString paragraph = text.substr(start, end - start);
                        // no control characters (a tab, a carriage return) in a drawn line
                        paragraph.erase(std::remove_if(paragraph.begin(), paragraph.end(), [](llwchar c) { return c < 0x20; }),
                                        paragraph.end());
                        const S32 added = ps::wrapText(mFont, paragraph.empty() ? LLWString(1, L' ') : paragraph,
                                                       (F32)width, mWrapped, room);
                        mKinds.insert(mKinds.end(), added, kind);
                        room -= llmax(1, added);
                        start = end + 1;
                    }
                }
            }

            const LLFontGL*        mFont;
            std::vector<Line>      mSource;
            std::string            mMe;
            std::vector<LLWString> mWrapped;
            std::vector<U8>        mKinds;
            S32                    mWrappedWidth = -1;
            S32                    mScroll = 0;    // lines back from the newest
        };

        // ---- the panel ----------------------------------------------------------------------------------

        class ConvPanel : public LLPanel
        {
        public:
            struct Params : public LLInitParam::Block<Params, LLPanel::Params>
            {
                Params() {}
            };

            bool postBuild() override;
            void reshape(S32 width, S32 height, bool called_from_parent = true) override;
            void onVisibilityChange(bool new_visibility) override;
            // The panel's area never zooms the camera behind it.
            bool handleScrollWheel(S32 x, S32 y, S32 clicks) override
            {
                LLPanel::handleScrollWheel(x, y, clicks);
                return true;
            }

            void open();

        protected:
            friend class LLUICtrlFactory;
            explicit ConvPanel(const Params& p) : LLPanel(p) {}

        private:
            // What a row shows (made again only when this changed).
            struct RowShown
            {
                LLUUID      id;
                std::string name;
                S32         unread = 0;
                bool        quiet = false;
                bool        chosen = false;
                Kind        kind = Kind::IM;
                bool operator==(const RowShown&) const = default;
            };

            void onModelChanged();
            void refresh();
            void refreshList(bool shown);
            void makeRows();
            void refreshPeer(const Entry* entry);
            void refreshHistory();
            void layout();
            void showChosen(const LLUUID& id);
            bool chooseAgain();
            LLLineEditor* makeInput();
            void setUsable(bool usable);
            void focusInput();
            void chooseRow(const LLUUID& id);
            void sendTyped();
            std::string kindText(Kind kind, bool connecting) const;

            bool               mBuilt = false;
            bool               mStale = true;
            ps::PsHeader*      mHeader = nullptr;
            LLScrollContainer* mListScroll = nullptr;
            LLPanel*           mList = nullptr;
            LLTextBox*         mEmpty = nullptr;
            LLView*            mDivider = nullptr;
            ps::PsLabel*       mPeer = nullptr;
            ps::PsLabel*       mPeerSub = nullptr;
            ConvHistory*       mHistory = nullptr;
            LLView*            mInputBox = nullptr;
            LLLineEditor*      mInput = nullptr;
            LLUICtrl*          mSend = nullptr;

            std::vector<RowShown> mRowsShown, mRowsWanted;
            std::vector<LLView*>  mRows;            // children of mList, made with mRowsShown
            LLUUID                mShownId;
            std::string           mStatus;          // a send that failed, until the next conversation or send
            bool                  mEnded = false;   // the conversation shown is gone; what was typed for it is kept
            // The line and the send button can be used (kept here: LLLineEditor::setEnabled() only makes it read-only,
            // its getEnabled() stays true). As made by the XML: usable.
            bool                  mUsable = true;
            std::string           mInputTip;        // the line's own tool tip (another one while the conversation has ended)
            bool                  mEndedTip = false;
            std::string           mInputLabel;
            U32                   mHistoryVersion = ~0u;
            std::vector<Line>     mLinesScratch;
            boost::signals2::scoped_connection mChanged;
        };

        // Work on the panel on the next idle, if it is still there (not from inside a row's or a button's callback).
        void later(const LLHandle<ConvPanel>& handle, std::function<void(ConvPanel*)> work)
        {
            doOnIdleOneTime([handle, work = std::move(work)]() {
                if (ConvPanel* panel = handle.get())
                {
                    work(panel);
                }
            });
        }

        // The line to type in, in the glass box: see-through, its colours from the style table (made here, not in
        // the XML, so no colour is written there). The official limit of one message.
        LLLineEditor* ConvPanel::makeInput()
        {
            LLLineEditor::Params p(LLUICtrlFactory::getDefaultParams<LLLineEditor>());
            p.name                 = "conv_input";
            p.rect                 = LLRect(INPUT_INSET, INPUT_INSET + INPUT_EDIT_H, 100, INPUT_INSET);
            p.follows.flags        = FOLLOWS_LEFT | FOLLOWS_RIGHT | FOLLOWS_TOP;
            p.max_length.bytes     = MAX_MSG_BUF_SIZE - 1;
            p.commit_on_focus_lost = false;     // a click on the world does not send what is half typed
            p.use_bg_color         = true;
            p.bg_color             = LLColor4::transparent;
            p.text_color           = style::TEXT_WHITE;
            p.cursor_color         = style::TEXT_WHITE;
            p.text_readonly_color  = style::TEXT_DIM;
            p.text_tentative_color = style::TEXT_DIM;
            p.draw_focus_border    = false;
            p.text_pad_left        = 6;
            p.border.visible       = false;
            p.tool_tip             = getString("InputTip");
            LLLineEditor* input = LLUICtrlFactory::create<LLLineEditor>(p);
            if (input)
            {
                mInputBox->addChildInBack(input);    // behind the send button: Tab goes from the line to the button, as they stand
            }
            return input;
        }

        bool ConvPanel::postBuild()
        {
            mHeader     = findChild<ps::PsHeader>("conv_head");
            mListScroll = findChild<LLScrollContainer>("conv_list_scroll");
            mList       = findChild<LLPanel>("conv_list");
            mDivider    = findChildView("conv_divider");
            mPeer       = findChild<ps::PsLabel>("conv_peer");
            mPeerSub    = findChild<ps::PsLabel>("conv_peer_sub");
            mInputBox   = findChildView("conv_input_box");
            mSend       = findChild<LLUICtrl>("conv_send");
            if (!mHeader || !mListScroll || !mList || !mDivider || !mPeer || !mPeerSub || !mInputBox || !mSend)
            {
                LL_WARNS("PlusPhotoScreen") << "The conversations panel's XML lacks a widget" << LL_ENDL;
                return true;
            }

            mEmpty = ps::makeNote("conv_empty", LLRect(0, EMPTY_H, 100, 0), getString("Empty"), ps::font(style::FONT_SMALL),
                                  style::TEXT_SUB);
            if (mEmpty)
            {
                addChild(mEmpty);
            }

            mInput = makeInput();
            if (!mInput)
            {
                return true;
            }

            ConvHistory::Params history;
            history.name         = "conv_history";
            history.rect         = LLRect(0, HISTORY_MIN, 100, 0);
            history.mouse_opaque = true;    // takes the wheel
            mHistory = LLUICtrlFactory::create<ConvHistory>(history);
            if (mHistory)
            {
                addChild(mHistory);
            }

            // Enter in the line and the send button
            const LLHandle<ConvPanel> handle = getDerivedHandle<ConvPanel>();
            const auto send = [handle](LLUICtrl*, const LLSD&) {
                if (ConvPanel* panel = handle.get())
                {
                    panel->sendTyped();
                }
            };
            mInput->setCommitCallback(send);
            mInputTip = mInput->getToolTip();
            mSend->setCommitCallback(send);
            mChanged = conv::onChanged([this]() { onModelChanged(); });
            mBuilt = mEmpty && mHistory;
            return true;
        }

        void ConvPanel::reshape(S32 width, S32 height, bool called_from_parent)
        {
            LLPanel::reshape(width, height, called_from_parent);
            layout();
        }

        void ConvPanel::onVisibilityChange(bool new_visibility)
        {
            LLPanel::onVisibilityChange(new_visibility);
            if (new_visibility && mStale)
            {
                // not while the views are being told of the change: the rows may be made again
                later(getDerivedHandle<ConvPanel>(), [](ConvPanel* panel) {
                    if (panel->isInVisibleChain())
                    {
                        panel->refresh();
                    }
                });
            }
        }

        void ConvPanel::open()
        {
            if (!mBuilt)
            {
                return;
            }
            conv::chooseForOpening();
            refresh();
            focusInput();
        }

        void ConvPanel::onModelChanged()
        {
            if (!mBuilt)
            {
                return;
            }
            if (!isInVisibleChain())
            {
                mStale = true;      // brought up to date when shown
                return;
            }
            refresh();
        }

        void ConvPanel::refresh()
        {
            if (!mBuilt)
            {
                return;
            }
            mStale = false;
            const bool shown = isInVisibleChain();
            const bool was_ended = mEnded;
            mEnded = !conv::find(conv::chosen()) && !chooseAgain();
            if (was_ended && !mEnded)
            {
                mStatus.clear();    // nothing of before stays under the name
            }
            if (mEnded)
            {
                refreshList(shown);
                refreshPeer(nullptr);
                return;
            }
            showChosen(conv::chosen());
            const Entry* entry = conv::find(mShownId);
            if (shown && entry && entry->unread > 0)
            {
                conv::markRead(mShownId);   // read as it is shown; the counts follow on the next change
            }
            refreshList(shown);
            refreshPeer(entry);
            refreshHistory();
        }

        // No conversation is chosen (the one chosen is gone): the one shown if it is back (the same person wrote
        // again), else the newest unread or the first. False, and nothing chosen, when the one shown is gone with
        // words typed for it: another one put in its place would get them. The line then stays, unusable, until a
        // conversation is chosen.
        bool ConvPanel::chooseAgain()
        {
            const LLUUID shown = conv::currentId(mShownId);
            if (mShownId.notNull() && conv::find(shown))
            {
                conv::choose(shown);
                return true;
            }
            if (mShownId.notNull() && !mInput->getText().empty())
            {
                return false;
            }
            conv::chooseForOpening();
            return true;
        }

        // The conversation shown: what was typed in the one before is kept for it, and this one's comes back.
        void ConvPanel::showChosen(const LLUUID& id)
        {
            if (id == mShownId)
            {
                return;
            }
            if (mShownId.notNull() && conv::currentId(mShownId) == id)
            {
                mShownId = id;      // the same conversation under the id the server gave it: the line is kept
                mHistoryVersion = ~0u;
                return;
            }
            if (mShownId.notNull() && conv::find(mShownId))
            {
                conv::setDraft(mShownId, mInput->getText());
            }
            mInput->setText(conv::draft(id));
            mShownId = id;
            mStatus.clear();
            mHistory->resetScroll();
            mHistoryVersion = ~0u;
        }

        void ConvPanel::chooseRow(const LLUUID& id)
        {
            conv::choose(id);
            refresh();
            focusInput();
        }

        std::string ConvPanel::kindText(Kind kind, bool connecting) const
        {
            const char* name = kind == Kind::Group ? "KindGroup" : kind == Kind::Conference ? "KindConference" : "KindIM";
            if (!connecting)
            {
                return getString(name);
            }
            LLStringUtil::format_map_t args;
            args["[KIND]"] = getString(name);
            return getString("Connecting", args);
        }

        void ConvPanel::refreshList(bool shown)
        {
            mRowsWanted.clear();
            for (const Entry& entry : conv::entries())
            {
                RowShown row;
                row.id     = entry.id;
                row.name   = entry.name;
                row.unread = shown && entry.id == mShownId ? 0 : entry.unread;     // being read
                row.quiet  = !entry.counted;
                row.chosen = entry.id == mShownId;
                row.kind   = entry.kind;
                mRowsWanted.push_back(std::move(row));
            }
            if (mRowsWanted == mRowsShown)
            {
                return;
            }
            mRowsShown.swap(mRowsWanted);
            const S32 doc_pos = mListScroll->getDocPosVertical();
            makeRows();
            layout();
            // where the list was scrolled to stays (in the new rows' coordinates)
            const S32 content_h = mList->getRect().getHeight();
            if (doc_pos > 0 && content_h > 0)
            {
                const S32 top = llmax(0, content_h - doc_pos);
                mListScroll->scrollToShowRect(LLRect(0, top, mList->getRect().getWidth(),
                                                     llmax(0, top - mListScroll->getRect().getHeight())));
            }
            else
            {
                mListScroll->goToTop();
            }
        }

        void ConvPanel::makeRows()
        {
            mList->deleteAllChildren();
            mRows.clear();
            const LLHandle<ConvPanel> handle = getDerivedHandle<ConvPanel>();
            for (const RowShown& shown : mRowsShown)
            {
                ps::PsListRow::Params p;
                p.name          = "conv_row";
                p.label         = shown.name;
                p.starrable     = false;
                p.rect          = LLRect(0, ROW_H, 100, 0);
                p.follows.flags = FOLLOWS_LEFT | FOLLOWS_TOP;
                p.tool_tip = shown.quiet ? getString("QuietTip") : kindText(shown.kind, false);
                ps::PsListRow* row = LLUICtrlFactory::create<ps::PsListRow>(p);
                if (!row)
                {
                    continue;
                }
                row->setValue(shown.chosen);
                row->setBadge(shown.unread, shown.quiet);
                const LLUUID id = shown.id;
                row->setCommitCallback([handle, id](LLUICtrl*, const LLSD&) {
                    later(handle, [id](ConvPanel* panel) { panel->chooseRow(id); });   // the row is made again
                });
                mList->addChild(row);
                mRows.push_back(row);
            }
        }

        void ConvPanel::refreshPeer(const Entry* entry)
        {
            if (mEnded)
            {
                mPeerSub->setLabelText(getString("Ended"));     // the name of the one that ended stays
            }
            else
            {
                mPeer->setLabelText(entry ? entry->name : getString("NoneChosen"));
                mPeerSub->setLabelText(!mStatus.empty() ? mStatus : entry ? kindText(entry->kind, entry->connecting) : std::string());
            }
            std::string label;
            if (entry)
            {
                LLStringUtil::format_map_t args;
                args["[NAME]"] = entry->name;      // a value, not the format: the name is shown as it is
                label = getString("To", args);
            }
            if (label != mInputLabel && !mEnded)
            {
                mInputLabel = label;
                mInput->setLabel(label);
            }
            setUsable(entry != nullptr);
            // while the conversation has ended, the line says what becomes of the words left in it
            if (mEnded != mEndedTip)
            {
                mEndedTip = mEnded;
                mInput->setToolTip(mEnded ? getString("EndedTip") : mInputTip);
            }
        }

        // The keyboard to the line, when it can be used and is on the screen.
        void ConvPanel::focusInput()
        {
            if (mUsable && mInput->isInVisibleChain())
            {
                mInput->setFocus(true);
            }
        }

        void ConvPanel::setUsable(bool usable)
        {
            if (usable != mUsable)
            {
                mUsable = usable;
                mInput->setEnabled(usable);     // read-only or not
                mSend->setEnabled(usable);
            }
        }

        void ConvPanel::refreshHistory()
        {
            if (conv::version() == mHistoryVersion)
            {
                return;
            }
            mHistoryVersion = conv::version();
            conv::lines(mShownId, HISTORY_LINES, mLinesScratch);
            mHistory->setLines(mLinesScratch, getString("Me"));
        }

        void ConvPanel::sendTyped()
        {
            if (!mBuilt || !mUsable)
            {
                return;     // Return in a read-only line still commits: nothing is sent from it
            }
            switch (conv::send(mShownId, mInput->getText()))
            {
                case SendResult::Sent:
                    mInput->setText(LLStringUtil::null);
                    mStatus.clear();
                    break;
                case SendResult::Empty:
                    return;
                case SendResult::Waiting:
                    mStatus = getString("NotSent");     // the text stays in the line
                    break;
                case SendResult::Refused:
                    mStatus = getString("Refused");
                    break;
            }
            refreshPeer(conv::find(mShownId));
        }

        // The heading, the list (as many rows as fit, up to LIST_ROWS_MAX, the rest scrolled), the peer, the
        // history and the input at the bottom. Whole pixels; the panel's height is at least 250 (the simple
        // mode's page is 288 and up).
        void ConvPanel::layout()
        {
            if (!mBuilt)
            {
                return;
            }
            const S32 w = getRect().getWidth();
            const S32 h = getRect().getHeight();
            S32 top = h;
            mHeader->setShape(LLRect(0, top, w, top - HEAD_H));
            top -= HEAD_H;

            // the input at the bottom
            const LLRect box(PAD_X, BOTTOM_PAD + INPUT_H, llmax(PAD_X + SEND_W + 1, w - PAD_X), BOTTOM_PAD);
            mInputBox->setShape(box);
            const S32 box_w = box.getWidth();
            mInput->setShape(LLRect(INPUT_INSET, INPUT_INSET + INPUT_EDIT_H, llmax(INPUT_INSET + 1, box_w - SEND_W), INPUT_INSET));
            mSend->setShape(LLRect(box_w - SEND_W, INPUT_H, box_w, 0));

            // the list: as many rows as leave the history its least height
            const S32 rows = (S32)mRows.size();
            const S32 under = DIVIDER_H + PEER_H + PEER_SUB_H + HISTORY_TOP_GAP + HISTORY_MIN + GAP + INPUT_H + BOTTOM_PAD;
            const S32 room_rows = llmax(1, (top - under) / ROW_H);
            const S32 shown_rows = llclamp(rows, 1, llmin(LIST_ROWS_MAX, room_rows));
            mEmpty->setVisible(rows == 0);
            mListScroll->setVisible(rows > 0);
            if (rows == 0)
            {
                mEmpty->setShape(LLRect(PAD_X, top, w - PAD_X, top - EMPTY_H));
                top -= EMPTY_H;
            }
            else
            {
                static LLUICachedControl<S32> scrollbar_size("UIScrollbarSize", 0);
                const S32 list_h = shown_rows * ROW_H;
                mListScroll->setShape(LLRect(LIST_X, top, w - LIST_X, top - list_h));
                const S32 inner = llmax(1, w - 2 * LIST_X - (rows > shown_rows ? (S32)scrollbar_size : 0));
                const S32 content_h = rows * ROW_H;
                mList->reshape(inner, content_h);
                for (S32 i = 0; i < rows; ++i)
                {
                    const S32 row_top = content_h - i * ROW_H;
                    mRows[i]->setShape(LLRect(0, row_top, inner, row_top - ROW_H));
                }
                top -= list_h;
            }

            mDivider->setShape(LLRect(0, top, w, top - DIVIDER_H));
            top -= DIVIDER_H;
            mPeer->setShape(LLRect(PAD_X, top, w - PAD_X, top - PEER_H));
            top -= PEER_H;
            mPeerSub->setShape(LLRect(PAD_X, top, w - PAD_X, top - PEER_SUB_H));
            top -= PEER_SUB_H + HISTORY_TOP_GAP;
            const S32 history_bottom = BOTTOM_PAD + INPUT_H + GAP;
            mHistory->setShape(LLRect(PAD_X, top, w - PAD_X, llmin(top, history_bottom)));
        }

        // ---- the simple mode's tab badge ------------------------------------------------------------------

        class ConvBadge : public LLView
        {
        public:
            struct Params : public LLInitParam::Block<Params, LLView::Params>
            {
                Params() {}
            };

            void setCount(S32 count)
            {
                if (count != mCount)
                {
                    mCount = count;
                    mText = ps::badgeText(count);
                }
            }

            void draw() override
            {
                if (mCount > 0)
                {
                    ps::drawBadge(mText, (F32)(getRect().getWidth() - BADGE_RIGHT), getRect().getHeight() * 0.5f, false,
                                  getDrawContext().mAlpha);
                }
            }

        protected:
            friend class LLUICtrlFactory;
            explicit ConvBadge(const Params& p) : LLView(p) {}

        private:
            S32       mCount = 0;
            LLWString mText;
        };
    }

    LLPanel* createPanel(LLView* parent)
    {
        if (!parent)
        {
            return nullptr;
        }
        ConvPanel::Params params;
        params.name          = "plus_photoscreen_conv";
        params.rect          = parent->getLocalRect();
        params.follows.flags = FOLLOWS_ALL;
        params.mouse_opaque  = false;
        ConvPanel* panel = LLUICtrlFactory::create<ConvPanel>(params);
        if (!panel)
        {
            return nullptr;
        }
        if (!panel->buildFromFile("panel_plus_photoscreen_conv.xml"))
        {
            delete panel;
            return nullptr;
        }
        parent->addChild(panel);
        panel->setShape(parent->getLocalRect());
        panel->setVisible(false);
        return panel;
    }

    void openPanel(LLPanel* panel)
    {
        if (ConvPanel* conv = dynamic_cast<ConvPanel*>(panel))
        {
            conv->open();
        }
    }

    LLView* addBadge(LLView* view)
    {
        if (!view)
        {
            return nullptr;
        }
        ConvBadge::Params params;
        params.name          = "conv_badge";
        params.rect          = view->getLocalRect();
        params.follows.flags = FOLLOWS_ALL;
        params.mouse_opaque  = false;
        ConvBadge* badge = LLUICtrlFactory::create<ConvBadge>(params);
        if (badge)
        {
            view->addChild(badge);
        }
        return badge;
    }

    void setBadge(LLView* badge, S32 count)
    {
        if (ConvBadge* view = dynamic_cast<ConvBadge*>(badge))
        {
            view->setCount(count);
        }
    }
}

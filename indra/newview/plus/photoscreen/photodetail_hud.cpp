/**
 * @file photodetail_hud.cpp
 * @brief The drawing view over the world of the detailed photo screen (research/218 in the sl-viewer
 *        repository): the focus frame and the nearby chat lines.
 *
 * The focus frame is four corners, a small cross and four marks at the middle of the sides, around the
 * point the focus lock (plus/focuslock) keeps sharp, with a line of text under it (who, which part, how
 * far); thin white lines as research/221 10. It is drawn only while the lock holds
 * a target (the cursor in the mode "under the mouse"); focusScreenPos() answers in window coordinates,
 * which the view turns into its own. Per frame: one state test, and while locked one projection.
 *
 * The chat lines come from the "LLChat" event pump, as plus/chatconsole's do, and are drawn bottom
 * up over a see-through rounded box at the left of the world (the chips' glass, research/221 8.). The lines are cut to the width again only when a line
 * arrives or the width changes.
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

#include "photodetail_hud.h"

#include "photoconv.h"
#include "photodetail_layout.h"
#include "psui/psstyle.h"
#include "plus/chatconsole/chatconsole.h"
#include "plus/focuslock/focuslock.h"

#include <deque>
#include <vector>

#include "llevents.h"
#include "llfontgl.h"
#include "llframetimer.h"
#include "llrender2dutils.h"
#include "lluictrlfactory.h"
#include "llview.h"
#include "llviewercontrol.h"

namespace plus::photoscreen::detail::hud
{
    namespace
    {
        namespace fl = plus::focuslock;

        constexpr char SETTING_FRAME[] = "PlusPhotoScreenFocusFrame";

        // FRAME_W, FRAME_H, CORNER_SHARE, CROSS, FRAME_TICK, CHAT_PAD: the table of sizes, photodetail_layout.h
        using namespace plus::photoscreen::sizes;
        namespace ps = plus::psui;
        constexpr S32 MAX_LINES    = 60;    // chat lines kept

        // research/221 10.: the frame's lines 1.5 px white 85 %, the marks white 50 %.
        const LLColor4 FRAME_COLOR(1.f, 1.f, 1.f, 0.85f);
        const LLColor4 FRAME_HALF_PX(1.f, 1.f, 1.f, 0.85f * 0.5f);   // the half pixel of the 1.5 px lines
        const LLColor4 FRAME_MARK(1.f, 1.f, 1.f, 0.5f);

        std::string sFrameLabel;

        // One chat line: its own word-wrapped sub-lines, and when it arrived, for the same fade-out as
        // plus/chatconsole's nearby chat (PlusChatConsolePersistTime, the last FADE_SECONDS of it fading).
        struct ChatLine
        {
            std::string             text;
            F64                     added = 0.0;
            std::vector<LLWString>  wrapped;
        };
        std::deque<ChatLine> sLines;
        bool sChatDirty = true;
        S32  sWrappedWidth = -1;

        LLTempBoundListener sListener;

        // "Name: text", "Name does something" for /me, the bare text for a line with no speaker.
        bool onChatEvent(const LLSD& event)
        {
            const std::string text = event["message"].asString();
            const std::string from = event["from"].asString();
            if (text.empty())
            {
                return false;
            }
            ChatLine line;
            line.text  = plus::photoscreen::conv::speakerLine(from, text);
            line.added = LLFrameTimer::getElapsedSeconds();
            sLines.push_back(std::move(line));
            while ((S32)sLines.size() > MAX_LINES)
            {
                sLines.pop_front();
            }
            sChatDirty = true;
            return false;   // the other listeners get it too
        }

        class HudDrawView : public LLView
        {
        public:
            struct Params : public LLInitParam::Block<Params, LLView::Params>
            {
                Params() {}
            };

        protected:
            explicit HudDrawView(const Params& p) : LLView(p) {}
            friend class LLUICtrlFactory;

        public:
            void setChatRect(const LLRect& rect) { mChat = rect; sChatDirty = true; }

            void draw() override
            {
                drawChat();
                drawFrame();
            }

        private:
            // A rectangle in whole pixels, any two opposite corners.
            static void box(S32 x1, S32 y1, S32 x2, S32 y2, const LLColor4& color)
            {
                gl_rect_2d(llmin(x1, x2), llmax(y1, y2), llmax(x1, x2), llmin(y1, y2), color);
            }

            // One corner of the frame: (x, y) the outer corner point, dx and dy point inwards (+1 / -1) along the
            // arms. A line of 1.5 px: one pixel at full colour on the outside and one at half colour inside it.
            static void corner(S32 x, S32 y, S32 dx, S32 dy, S32 arm_x, S32 arm_y)
            {
                box(x, y, x + dx * arm_x, y + dy, FRAME_COLOR);                    // across, the outer pixel
                box(x + dx, y + dy, x + dx * arm_x, y + dy * 2, FRAME_HALF_PX);    // (not over the outer column)
                box(x, y + dy, x + dx, y + dy * arm_y, FRAME_COLOR);               // up or down, the outer pixel
                box(x + dx, y + dy * 2, x + dx * 2, y + dy * arm_y, FRAME_HALF_PX);
            }

            void drawFrame()
            {
                static LLCachedControl<bool> show(gSavedSettings, SETTING_FRAME, true);
                if (!show || fl::state() != fl::State::Locked)
                {
                    return;
                }
                LLVector2 pos;
                if (!fl::focusScreenPos(pos))
                {
                    return;
                }
                S32 x = 0, y = 0;
                screenPointToLocal(ll_round(pos.mV[0]), ll_round(pos.mV[1]), &x, &y);
                const LLRect local = getLocalRect();
                if (!local.pointInRect(x, y))
                {
                    return;
                }
                const S32 left   = x - FRAME_W / 2;
                const S32 right  = left + FRAME_W;
                const S32 bottom = y - FRAME_H / 2;
                const S32 top    = bottom + FRAME_H;
                const S32 arm_x  = ll_round(FRAME_W * CORNER_SHARE);
                const S32 arm_y  = ll_round(FRAME_H * CORNER_SHARE);
                // The four corners (the box() rectangles are [left, right) x [bottom, top)).
                corner(left, top, 1, -1, arm_x, arm_y);
                corner(right, top, -1, -1, arm_x, arm_y);
                corner(left, bottom, 1, 1, arm_x, arm_y);
                corner(right, bottom, -1, 1, arm_x, arm_y);
                // The marks at the middle of the sides, pointing in.
                box(x, top, x + 1, top - FRAME_TICK, FRAME_MARK);
                box(x, bottom, x + 1, bottom + FRAME_TICK, FRAME_MARK);
                box(left, y, left + FRAME_TICK, y + 1, FRAME_MARK);
                box(right, y, right - FRAME_TICK, y + 1, FRAME_MARK);
                // The cross in the middle, 1 px.
                box(x - CROSS, y, x + CROSS + 1, y + 1, FRAME_COLOR);
                box(x, y - CROSS, x + 1, y + CROSS + 1, FRAME_COLOR);
                if (!sFrameLabel.empty())
                {
                    LLFontGL::getFontSansSerifSmall()->renderUTF8(sFrameLabel, 0, (F32)x, (F32)(bottom - FRAME_LABEL_GAP),
                                                                  LLColor4::white, LLFontGL::HCENTER, LLFontGL::TOP,
                                                                  LLFontGL::NORMAL, LLFontGL::DROP_SHADOW);
                }
            }

            // Each line keeps its own wrapped sub-lines (not one flat list any more), so a line's fade does
            // not need the others rewrapped too.
            void wrapLines(const LLFontGL* font, S32 width)
            {
                sWrappedWidth = width;
                sChatDirty    = false;
                const F32 limit = (F32)(width - CHAT_PAD * 2);
                for (ChatLine& line : sLines)
                {
                    line.wrapped.clear();
                    if (limit >= 20.f)
                    {
                        ps::wrapText(font, utf8str_to_wstring(line.text), limit, line.wrapped);
                    }
                }
            }

            void drawChat()
            {
                // old lines go even when nothing is drawn, with the same persist time as plus/chatconsole's
                // nearby chat (the left-bottom one, not shown while the detailed photo screen is on)
                const F64 now     = LLFrameTimer::getElapsedSeconds();
                const F64 persist = plus::chatconsole::persistTime();
                while (!sLines.empty() && now - sLines.front().added >= persist)
                {
                    sLines.pop_front();
                    sChatDirty = true;    // the box may need to shrink
                }
                if (sLines.empty() || mChat.getWidth() < 40 || mChat.getHeight() < 20)
                {
                    return;
                }
                const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
                if (sChatDirty || sWrappedWidth != mChat.getWidth())
                {
                    wrapLines(font, mChat.getWidth());
                }
                S32 total = 0;
                for (const ChatLine& line : sLines)
                {
                    total += (S32)line.wrapped.size();
                }
                const S32 line_h = font->getLineHeight() + 1;
                const S32 count  = llmin(total, (mChat.getHeight() - CHAT_PAD) / line_h);
                if (count <= 0)
                {
                    return;
                }
                const S32 box_h = count * line_h + CHAT_PAD;
                const S32 top   = mChat.mBottom + box_h;
                const F32 box_alpha = getDrawContext().mAlpha * plus::chatconsole::fadeAlpha(sLines.back().added, now);
                ps::drawRound(LLRectf((F32)mChat.mLeft, (F32)top, (F32)mChat.mRight, (F32)mChat.mBottom), ps::style::CHIP_RADIUS,
                              ps::Fill::solid(ps::faded(ps::style::CHIP_FILL, box_alpha)));
                // newest message at the bottom, its own newest sub-line first, as the flat list did
                S32 y = mChat.mBottom + CHAT_PAD / 2;
                S32 drawn = 0;
                for (auto msg = sLines.rbegin(); msg != sLines.rend() && drawn < count; ++msg)
                {
                    const F32 alpha = getDrawContext().mAlpha * plus::chatconsole::fadeAlpha(msg->added, now);
                    for (auto sub = msg->wrapped.rbegin(); sub != msg->wrapped.rend() && drawn < count; ++sub, ++drawn)
                    {
                        font->render(*sub, 0, (F32)(mChat.mLeft + CHAT_PAD), (F32)y, ps::faded(ps::style::TEXT_WHITE, alpha),
                                     LLFontGL::LEFT, LLFontGL::BOTTOM, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW);
                        y += line_h;
                    }
                }
            }

            LLRect mChat;
        };
    }

    LLView* create(LLView* parent)
    {
        if (!parent)
        {
            return nullptr;
        }
        HudDrawView::Params params;
        params.name          = "hud_draw";
        params.rect          = parent->getLocalRect();
        params.follows.flags = FOLLOWS_ALL;
        params.mouse_opaque  = false;
        HudDrawView* view = LLUICtrlFactory::create<HudDrawView>(params);
        if (!view)
        {
            return nullptr;
        }
        parent->addChild(view);
        parent->sendChildToBack(view);
        if (!sListener.connected())
        {
            sListener = LLEventPumps::instance().obtain("LLChat").listen("plus_photoscreen_detail", &onChatEvent);
        }
        return view;
    }

    void setChatRect(LLView* view, const LLRect& rect)
    {
        if (HudDrawView* draw = dynamic_cast<HudDrawView*>(view))
        {
            draw->setChatRect(rect);
        }
    }

    void setFrameLabel(const std::string& label)
    {
        sFrameLabel = label;
    }
}

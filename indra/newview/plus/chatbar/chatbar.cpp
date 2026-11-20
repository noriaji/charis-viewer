/**
 * @file chatbar.cpp
 * @brief Chat bar (research/334): Firestorm's chat bar at the bottom left, a "Nearby Chat" button and a one-line
 *        nearby chat field beside the bottom toolbar.
 *
 * The layout is in panel_toolbar_view.xml: the bottom panel (bottom_toolbar_panel) holds a horizontal stack with
 * the chat bar's panel (chat_bar_layout_panel, shown by PlusShowChatBar through its visibility_control, its width
 * set by the user with the stack's resize bar) and the bottom toolbar's own panel (toolbar_panel), as Firestorm's
 * chat_bar_layout_panel. The floaters keep out of the bar on their own: their snap region (floater_snap_region) is
 * above the bottom panel.
 *
 * What is ours: the field (ChatBar, the XUI tag plus_chat_bar), the button's function (Plus.ChatBar.NearbyChat: what
 * Communicate > Nearby Chat does), the bottom panel shown and sized for the bar too (syncBottomPanel(), from
 * LLToolBarView::draw()) and the toolbar's centre the buttons over it are placed by (bottomToolbarCenterX()).
 * Typing and sending are the nearby chat floater's own functions (LLFloaterIMNearbyChat::onChatEntryKeystroke /
 * sendChatFromEntry): the typing animation, the gesture completion, "/number", gestures, "/whisper" "/shout", RLVa's
 * chat filters, CloseChatOnReturn. The bar says on channel 0 and Return alone says normal, as Firestorm's chat bar: the
 * nearby chat floater's channel selector and chat type button (chat-channel-select) are that floater's only.
 * Left out (Firestorm has them, research/334): its own nearby chat hub and window, the voice monitor, hiding the bar
 * by itself (AutohideChatBar), letter keys moving the focus to the bar (FSLetterKeysFocusNearbyChatBar), the bar's
 * width kept between sessions.
 *
 * Cost: each frame only syncBottomPanel()'s few compares (a cached setting, two visibilities, two heights, the bar's
 * width against the one wanted after a dynamic_cast), writing only what differs; the field works on events only.
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

#include "chatbar.h"

#include "plus.h"

#include "llagent.h"
#include "llappviewer.h"                    // gDisconnected
#include "llautoreplace.h"
#include "llchatmentionhelper.h"
#include "llemojihelper.h"
#include "llfontgl.h"
#include "llfloaterchatmentionpicker.h"
#include "llfloaterimnearbychat.h"
#include "llgestureautocompletehelper.h"
#include "lllayoutstack.h"
#include "llpanel.h"
#include "lltoolbarview.h"
#include "lluictrlfactory.h"
#include "llviewerchat.h"
#include "llviewercontrol.h"

namespace plus::chatbar
{
    namespace
    {
        // The chat bar's height in the bottom panel (panel_toolbar_view.xml: plus_ps_box "chat_bar_box"). The bottom
        // toolbar is as high (its buttons' desired_height 24 and the pads, widgets/toolbar.xml) or higher (two rows);
        // an empty toolbar is lower (its min_girth 24), and the panel keeps the bar's height then.
        constexpr S32 CHAT_BAR_HEIGHT = 26;

        // The toolbar's panel the bar leaves at least while the toolbar has buttons (its toolbar 400 between the 40 on
        // each side, several buttons a row; panel_toolbar_view.xml). The bar's narrowest is its panel's min_dim.
        constexpr S32 TOOLBAR_PANEL_MIN_WIDTH = 480;

        // The channel the bar says on without "/number": always 0, as Firestorm's chat bar, whatever the nearby chat
        // floater's channel selector (chat-channel-select) shows (it is that floater's own).
        constexpr S32 CHAT_BAR_CHANNEL = 0;

        LLDefaultChildRegistry::Register<ChatBar> sChatBarRegistration("plus_chat_bar");

        // The chat bar's button: what Communicate > Nearby Chat (Ctrl+H) does, the same function. The menu's
        // functions are registered by init_menus() during login, after the toolbar view was built (a button there
        // cannot name them): looked up when clicked.
        void toggleNearbyChat(LLUICtrl* ctrl, const LLSD&)
        {
            LLUICtrl::commit_callback_t* menu_function =
                LLUICtrl::CommitCallbackRegistry::getValue("Communicate.NearbyChat");
            if (menu_function)
            {
                (*menu_function)(ctrl, LLSD());
            }
        }

        void init()
        {
            LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Plus.ChatBar.NearbyChat", &toggleNearbyChat);
        }

        plus::FeatureRegistration sRegistration({ "chatbar", &init });

        // The chat bar (one in panel_toolbar_view.xml), for hasFocus() and startChat(). Set when it is built; the
        // handle is empty once the toolbar view is gone.
        LLHandle<ChatBar> sBar;

        // The chat font (ChatFontSize), at most SansSerif: the field (20) and its box (CHAT_BAR_HEIGHT) keep their
        // height, so the bottom panel stays as high as the toolbar; a larger font is cut off in a one-line field.
        LLFontGL* barFont(LLFontGL* chat_font)
        {
            LLFontGL* largest = LLFontGL::getFontSansSerif();
            if (!chat_font || (largest && chat_font->getLineHeight() > largest->getLineHeight()))
            {
                return largest;
            }
            return chat_font;
        }
    }

    ChatBar::Params::Params()
    {
    }

    ChatBar::ChatBar(const Params& p)
    :   LLChatEntry(p)
    {
        // As the nearby chat floater's field (LLFloaterIMSessionTab::postBuild(), LLFloaterIMNearbyChat::postBuild()).
        // LLAutoReplace is looked up when a word is typed, not while the toolbar view is built at start.
        setAutoreplaceCallback([](S32& start, S32& length, LLWString& replacement, S32& cursor, const LLWString& text)
        {
            LLAutoReplace::getInstance()->autoreplaceCallback(start, length, replacement, cursor, text);
        });
        setKeystrokeCallback([this](LLTextEditor*)
        {
            LLFloaterIMNearbyChat::onChatEntryKeystroke(this, CHAT_BAR_CHANNEL);
        });
        setCommitOnFocusLost(false);
        setPassDelete(true);
        setFont(barFont(LLViewerChat::getChatFont()));
        mFontChanged = LLViewerChat::setFontChangedCallback([this](LLFontGL* font) { setFont(barFont(font)); });
        enableSingleLineMode(true);
        sBar = getDerivedHandle<ChatBar>();
    }

    bool ChatBar::postBuild()
    {
        // The words while the field is empty (its box's <string>, en and ja): built in the box, so it is there now.
        LLPanel* box = dynamic_cast<LLPanel*>(getParent());
        if (box && box->hasString("chat_bar_label"))
        {
            setLabel(box->getString("chat_bar_label"));
        }
        return LLChatEntry::postBuild();
    }

    namespace
    {
        // The chat type of Return with mask (Firestorm's chat bar): Return alone says (normal), whatever the nearby
        // chat floater's chat type button (chat-channel-select); false for the other masks (Alt, Ctrl+Shift), which
        // go on to the field.
        bool chatTypeOfReturn(MASK mask, EChatType& type)
        {
            switch (mask)
            {
            case MASK_NONE:     type = CHAT_TYPE_NORMAL;  return true;
            case MASK_SHIFT:    type = CHAT_TYPE_WHISPER; return true;
            case MASK_CONTROL:  type = CHAT_TYPE_SHOUT;   return true;
            default:            return false;
            }
        }

        // A helper's list (gesture, emoji, mention) is open on the field: Return picks from it, as in the floater's
        // field.
        bool helperOpen(const LLUICtrl* field)
        {
            return LLGestureAutocompleteHelper::instance().isActive(field)
                || LLEmojiHelper::instance().isActive(field)
                || LLChatMentionHelper::instance().isActive(field);
        }
    }

    bool ChatBar::handleKeyHere(KEY key, MASK mask)
    {
        EChatType type = CHAT_TYPE_NORMAL;
        if (KEY_RETURN == key && !getReadOnly() && !helperOpen(this) && chatTypeOfReturn(mask, type))
        {
            LLChatEntry::onCommit();    // keeps the line for Ctrl+Up/Down (LLChatEntry::updateHistory())
            LLFloaterIMNearbyChat::sendChatFromEntry(this, type, CHAT_BAR_CHANNEL);
            return true;
        }
        return LLChatEntry::handleKeyHere(key, mask);
    }

    void ChatBar::onFocusReceived()
    {
        setEnabled(!gDisconnected);         // as LLFloaterIMNearbyChat::onChatBoxFocusReceived()
        // The "@" mention helper lists the nearby chat's people (the null session), as LLFloaterIMSessionTab::
        // onFocusReceived() sets it for the floater's field; RLVa's @shownames follows the same session (plus/rlv).
        LLFloaterChatMentionPicker::updateSessionID(LLUUID::null);
        LLChatEntry::onFocusReceived();
    }

    void ChatBar::onFocusLost()
    {
        gAgent.stopTyping();                // as LLFloaterIMNearbyChat::onChatBoxFocusLost()
        LLChatEntry::onFocusLost();
    }

    namespace
    {
        // The bar's width the user wants: its layout panel's at start (the XML's 320), then the one the user drags it
        // to (the stack's resize bar). Kept with the panel it is of (a new toolbar view starts from its own).
        struct BarWidth
        {
            LLHandle<LLPanel> panel;    // the bar's layout panel (chat_bar_layout_panel)
            S32 wanted = 0;             // the width the user wants
            S32 last = 0;               // the panel's target width when fitBarWidth() last left it
        };
        BarWidth sBarWidth;

        // While the bar and the toolbar's panel are both shown: the bar as wide as the user wants, but no wider than
        // leaves the toolbar's panel TOOLBAR_PANEL_MIN_WIDTH (a narrow window, a pull to the right edge) and never
        // narrower than its panel's min_dim; as wide as wanted again when the window is widened. A width this did not
        // leave (the user dragged the resize bar) is the new wanted one. Reshapes only when the width differs.
        void fitBarWidth(const LLView* toolbar_panel)
        {
            const ChatBar* bar = sBar.get();
            LLLayoutPanel* bar_panel = (bar && bar->getParent())
                ? dynamic_cast<LLLayoutPanel*>(bar->getParent()->getParent())   // the box's parent
                : nullptr;
            const LLView* stack = toolbar_panel->getParent();
            if (!bar_panel || !stack)
            {
                return;
            }
            // The target width (the stack lays the panel out to it), not the rect: a reshape and a drag write it
            // at once, the rect follows at the stack's next layout.
            const S32 current = bar_panel->getTargetDim();
            if (sBarWidth.panel.get() != bar_panel)
            {
                sBarWidth.panel = bar_panel->getHandle();
                sBarWidth.wanted = current;
            }
            else if (current != sBarWidth.last)
            {
                sBarWidth.wanted = current;
            }
            const S32 narrowest = bar_panel->getMinDim();
            const S32 widest = llmax(narrowest, stack->getRect().getWidth() - TOOLBAR_PANEL_MIN_WIDTH);
            const S32 goal = llclamp(sBarWidth.wanted, narrowest, widest);
            if (goal != current)
            {
                bar_panel->reshape(goal, bar_panel->getRect().getHeight());
            }
            sBarWidth.last = bar_panel->getTargetDim();
        }
    }

    void syncBottomPanel(LLView* bottom_panel, const LLView* bottom_toolbar, bool show_toolbars)
    {
        const LLView* toolbar_panel = bottom_toolbar ? bottom_toolbar->getParent() : nullptr;
        if (!bottom_panel || !toolbar_panel || toolbar_panel == bottom_panel)
        {
            return;     // a toolbar view without the chat bar's stack: LLToolBarView::draw() did it all
        }
        static LLCachedControl<bool> show_chat_bar(gSavedSettings, "PlusShowChatBar", true);

        const bool visible = show_toolbars && (show_chat_bar || toolbar_panel->getVisible());
        if (bottom_panel->getVisible() != visible)
        {
            bottom_panel->setVisible(visible);
        }

        if (show_chat_bar && toolbar_panel->getVisible())
        {
            fitBarWidth(toolbar_panel);
        }

        S32 height = bottom_toolbar->getRect().getHeight();
        if (show_chat_bar)
        {
            height = llmax(height, CHAT_BAR_HEIGHT);
        }
        if (bottom_panel->getRect().getHeight() != height)
        {
            bottom_panel->reshape(bottom_panel->getRect().getWidth(), height);
        }
    }

    S32 bottomToolbarCenterX(const LLView* bottom_toolbar)
    {
        if (!bottom_toolbar)
        {
            return 0;
        }
        const LLView* bottom_panel = gToolBarView ? gToolBarView->getBottomToolbar() : nullptr;
        const LLView* toolbar_panel = bottom_toolbar->getParent();
        if (!bottom_panel || !toolbar_panel || toolbar_panel == bottom_panel)
        {
            return bottom_toolbar->getRect().getCenterX();
        }
        if (!toolbar_panel->getVisible())
        {
            // No buttons: the toolbar's panel is hidden (LLToolBarView::draw()) and its rect is not laid out.
            return bottom_panel->getRect().getWidth() / 2;
        }
        S32 x = 0;
        S32 y = 0;
        bottom_toolbar->localPointToOtherView(bottom_toolbar->getRect().getWidth() / 2, 0, &x, &y, bottom_panel);
        return x;
    }

    bool hasFocus()
    {
        const ChatBar* bar = sBar.get();
        return bar && bar->hasFocus();
    }

    bool startChat(const char* line)
    {
        ChatBar* bar = sBar.get();
        if (!bar || !bar->isInVisibleChain())
        {
            return false;
        }
        bar->setFocus(true);
        if (line && bar->getLength() == 0)
        {
            bar->setText(LLStringExplicit(line));   // "/" starts a gesture in an empty bar; a line half typed is kept
        }
        bar->endOfDoc();
        return true;
    }
}

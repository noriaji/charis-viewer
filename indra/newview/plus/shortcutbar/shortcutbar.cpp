/**
 * @file shortcutbar.cpp
 * @brief Menu shortcut bar (plus/shortcutbar, research/225): a row under the navigation bar with
 *        text-only buttons, each running one item of the menu bar. Items are added with a right
 *        click on the menu item itself.
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

#include "plus.h"
#include "shortcutbar.h"
#include "plus/photoscreen/photoscreen.h"
#include "plus/photoscreen/psui/psstyle.h"

#include "llcallbacklist.h"
#include "llcontrol.h"
#include "lldir.h"
#include "llfile.h"
#include "llfontgl.h"
#include "llkeyboard.h"
#include "lllayoutstack.h"
#include "llmenugl.h"
#include "llnotificationsutil.h"
#include "llpaneltopinfobar.h"
#include "llrender2dutils.h"
#include "llsdserialize.h"
#include "llstartup.h"
#include "lltrans.h"
#include "lluicolortable.h"
#include "lluictrlfactory.h"
#include "llviewercontrol.h"
#include "llviewermenu.h"
#include "llviewerwindow.h"
#include "llrootview.h"
#include "llwindow.h"
#include "llfloaterreg.h"

#include <string>
#include <vector>

namespace plus::shortcutbar
{
    namespace
    {
        constexpr char SETTING_SHOW[]     = "PlusShortcutBarShow";
        constexpr char SETTING_BACKGROUND[] = "PlusShortcutBarBackground";
        constexpr char FILE_NAME[]        = "plus_shortcut_bar.xml";
        constexpr char PANEL_NAME[]       = "shortcut_bar_container";
        constexpr char MENU_BAR_FILE[]    = "menu_plus_shortcut_bar.xml";
        constexpr char MENU_ITEM_FILE[]   = "menu_plus_shortcut_item.xml";
        constexpr S32  MAX_ENTRIES        = 40;
        constexpr S32  MAX_DEPTH          = 8;
        constexpr S32  MAX_ALIAS_BYTES    = 40;
        namespace style = plus::psui::style;
        constexpr S32  BTN_PAD            = style::SHORTCUT_BTN_PAD;     // a button is this much wider than its text on each side
        constexpr S32  MARGIN_LEFT        = style::SHORTCUT_EDGE + BTN_PAD; // the first text (the first button starts SHORTCUT_EDGE from the edge)
        constexpr S32  GAP                = 2 * BTN_PAD + style::SHORTCUT_BTN_GAP; // between the texts of two buttons
        constexpr S32  SEPARATOR_EXTRA    = 2 * style::SHORTCUT_BTN_GAP; // more room between two buttons for a separator line
        constexpr S32  SEPARATOR_INSET    = 4;    // the separator line stops this far from the row's top and bottom
        constexpr F32  REFRESH_SECONDS    = 0.5f; // enabled / checked state of the items
        constexpr LLFontGL::ShadowType TEXT_SHADOW = LLFontGL::DROP_SHADOW_SOFT; // the hint of an empty bar (it has no box), as LLButton's label_shadow

        using Path = std::vector<std::string>;

        struct Entry
        {
            Path        path;               // names of the menus down to the item, the item last
            std::string alias;              // empty: the menu item's own label
            bool        separator = false;  // a "|" is drawn before it
        };

        std::vector<Entry> sEntries;
        bool               sLoaded = false;
        bool               sDirty  = true;  // the layout has to be made again
        bool               sKeepFile = false; // the file could not be read nor put aside: it is not written over
        bool               sKeepFileTold = false; // the user was told once that changes are not saved

        //---------------------------------------------------------------------------------------------
        // The menu bar

        std::string filePath()
        {
            return gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, FILE_NAME);
        }

        std::string trimmed(const std::string& text)
        {
            std::string out = text;
            LLStringUtil::trim(out);
            return out;
        }

        // The name on the bar: a menu label like "Radar (a list of the people around)" is shown as
        // "Radar" (the explanation stays in the menu). A label that is only a parenthesis is kept.
        std::string shortName(const std::string& text)
        {
            std::string out = trimmed(text);
            static const char* const CLOSERS[] = { "\xEF\xBC\x89", ")" }; // fullwidth closing paren and )
            static const char* const OPENERS[] = { "\xEF\xBC\x88", "(" }; // fullwidth opening paren and (
            for (const char* closer : CLOSERS)
            {
                const size_t len = strlen(closer);
                if (out.size() <= len || out.compare(out.size() - len, len, closer) != 0)
                {
                    continue;
                }
                size_t cut = std::string::npos;
                for (const char* opener : OPENERS)
                {
                    const size_t at = out.find(opener);
                    if (at != std::string::npos && at < cut)
                    {
                        cut = at;
                    }
                }
                if (cut != std::string::npos && cut > 0)
                {
                    std::string head = out.substr(0, cut);
                    LLStringUtil::trim(head);
                    if (!head.empty())
                    {
                        return head;
                    }
                }
            }
            return out;
        }

        // The item the names lead to, or null (the menu is not there: a skin without it, another version).
        LLMenuItemGL* resolve(const Path& path)
        {
            if (!gMenuBarView || path.empty())
            {
                return nullptr;
            }
            LLMenuGL* menu = gMenuBarView;
            LLMenuItemGL* item = nullptr;
            for (size_t i = 0; i < path.size(); ++i)
            {
                item = menu->getItem(path[i]);
                if (!item)
                {
                    return nullptr;
                }
                if (i + 1 < path.size())
                {
                    LLMenuItemBranchGL* branch = dynamic_cast<LLMenuItemBranchGL*>(item);
                    if (!branch || !branch->getBranch())
                    {
                        return nullptr;
                    }
                    menu = branch->getBranch();
                }
            }
            return item;
        }

        // Fills path with the names from the menu bar down to target. Depth-limited (a menu that
        // contains itself would not end).
        bool findPath(LLMenuGL* menu, const LLMenuItemGL* target, Path& path)
        {
            if (!menu || (S32)path.size() >= MAX_DEPTH)
            {
                return false;
            }
            const S32 count = (S32)menu->getItemCount();
            for (S32 i = 0; i < count; ++i)
            {
                LLMenuItemGL* item = menu->getItem(i);
                if (!item)
                {
                    continue;
                }
                path.push_back(item->getName());
                if (item == target)
                {
                    return true;
                }
                if (LLMenuItemBranchGL* branch = dynamic_cast<LLMenuItemBranchGL*>(item))
                {
                    if (findPath(branch->getBranch(), target, path))
                    {
                        return true;
                    }
                }
                path.pop_back();
            }
            return false;
        }

        S32 indexOf(const Path& path)
        {
            for (S32 i = 0; i < (S32)sEntries.size(); ++i)
            {
                if (sEntries[i].path == path)
                {
                    return i;
                }
            }
            return -1;
        }

        //---------------------------------------------------------------------------------------------
        // The list's file

        LLSD toSD()
        {
            LLSD list = LLSD::emptyArray();
            for (const Entry& e : sEntries)
            {
                LLSD item;
                LLSD path = LLSD::emptyArray();
                for (const std::string& name : e.path)
                {
                    path.append(name);
                }
                item["path"] = path;
                if (!e.alias.empty())
                {
                    item["alias"] = e.alias;
                }
                if (e.separator)
                {
                    item["separator"] = true;
                }
                list.append(item);
            }
            LLSD root;
            root["version"] = 1;
            root["items"]   = list;
            return root;
        }

        void save()
        {
            if (sKeepFile)
            {
                LL_WARNS("Plus") << "Shortcut bar: the list file could not be read; it is left as it is" << LL_ENDL;
                if (!sKeepFileTold)
                {
                    sKeepFileTold = true;
                    LLSD args;
                    args["MESSAGE"] = LLTrans::getString("PlusShortcutBarNotSaved");
                    LLNotificationsUtil::add("SystemMessageTip", args);
                }
                return;
            }
            const std::string path = filePath();
            const plus::FileWrite wrote = plus::writeFileReplacing(path, [](std::ostream& out) { LLSDSerialize::toPrettyXML(toSD(), out); });
            if (wrote == plus::FileWrite::cannot_replace)
            {
                LL_WARNS("Plus") << "Shortcut bar: cannot replace " << path << LL_ENDL;
            }
            else if (wrote != plus::FileWrite::written)
            {
                LL_WARNS("Plus") << "Shortcut bar: cannot write " << plus::tempPathFor(path) << LL_ENDL;
            }
        }

        void changed()
        {
            sDirty = true;
            save();
        }

        // What a new user starts with, to show that the bar is there: items of the menus that the
        // toolbar has no button for.
        void addDefaults()
        {
            sEntries = {
                { { "Avatar", "Stop Animating My Avatar" }, "", false },
                { { "World",  "Radar" },                    "", false },
                { { "World",  "AreaSearch" },               "", false },
            };
        }

        // A file that cannot be read is moved aside before the list starts empty, so the next change does
        // not write over what the user had. Up to three are kept (".bad", ".bad2", ".bad3"); the oldest, the
        // user's list as it first broke, is never the one replaced. If it cannot be moved, it is never
        // written over.
        void putAside(const std::string& path)
        {
            const std::string bad = plus::putAside(path, 3); // all three there: the newest (".bad3") gives way
            if (!bad.empty())
            {
                LL_WARNS("Plus") << "Shortcut bar: the unreadable list was kept as " << bad << LL_ENDL;
            }
            else
            {
                sKeepFile = true;
            }
        }

        void load()
        {
            if (sLoaded)
            {
                return;
            }
            sLoaded = true;
            sEntries.clear();

            const std::string path = filePath();
            llifstream in(path.c_str());
            if (!in.is_open())
            {
                addDefaults();   // no file yet: the first start
                return;
            }
            in.close();
            if (!plus::xmlNestingOk(path))
            {
                LL_WARNS("Plus") << "Shortcut bar: " << path << " is nested too deep, left out" << LL_ENDL;
                putAside(path);
                return;
            }
            LLSD root;
            {
                llifstream file(path.c_str());
                if (LLSDSerialize::fromXML(root, file) == LLSDParser::PARSE_FAILURE || !root.isMap())
                {
                    root = LLSD();
                }
            } // closed before it may be moved aside
            if (!root.isMap() || !root["items"].isArray())
            {
                LL_WARNS("Plus") << "Shortcut bar: cannot read " << path << LL_ENDL;
                putAside(path);
                return;
            }
            const LLSD& items = root["items"];
            for (LLSD::array_const_iterator it = items.beginArray(); it != items.endArray(); ++it)
            {
                if ((S32)sEntries.size() >= MAX_ENTRIES)
                {
                    break;
                }
                const LLSD& item = *it;
                if (!item.isMap() || !item["path"].isArray() || item["path"].size() == 0
                    || item["path"].size() > MAX_DEPTH)
                {
                    continue;
                }
                Entry e;
                for (LLSD::array_const_iterator p = item["path"].beginArray(); p != item["path"].endArray(); ++p)
                {
                    e.path.push_back(p->asString());
                }
                e.alias     = utf8str_truncate(item["alias"].asString(), MAX_ALIAS_BYTES);
                e.separator = item["separator"].asBoolean();
                sEntries.push_back(e);
            }
        }

        //---------------------------------------------------------------------------------------------
        // The bar

        class ShortcutBarView : public LLView
        {
        public:
            struct Params : public LLInitParam::Block<Params, LLView::Params>
            {
                Params() {}
            };

        protected:
            explicit ShortcutBarView(const Params& p) : LLView(p) {}
            friend class LLUICtrlFactory;

        public:
            void draw() override
            {
                const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
                if (!font)
                {
                    return;
                }
                refresh(font);

                const LLRect rect = getLocalRect();
                const S32 text_y = rect.getCenterY() - (S32)(font->getLineHeight() / 2.f) + 1;
                const LLColor4 normal = LLUIColorTable::instance().getColor("LtGray");   // the hint of an empty bar

                // The bar lies over the world, as the Firestorm favorites bar does: no background unless the
                // user asks for one (0 = clear, 1 = black). Each button is a translucent box (drawButton()).
                static LLCachedControl<F32> background(gSavedSettings, SETTING_BACKGROUND, 0.f);
                const F32 back_alpha = llclamp((F32)background, 0.f, 1.f);
                if (back_alpha > 0.f)
                {
                    gl_rect_2d(rect, LLColor4(0.f, 0.f, 0.f, back_alpha), true);
                }

                if (mSlots.empty())
                {
                    if (sEntries.empty())
                    {
                        font->renderUTF8(mEmptyLabel, 0, MARGIN_LEFT, text_y,
                                         LLColor4(normal.mV[0], normal.mV[1], normal.mV[2], 0.7f),
                                         LLFontGL::LEFT, LLFontGL::BOTTOM, LLFontGL::NORMAL, TEXT_SHADOW);
                    }
                    return;
                }

                const F32 alpha = getDrawContext().mAlpha;
                const F32 box_bottom = F32((rect.getHeight() - style::SHORTCUT_BTN_HEIGHT) / 2);
                const F32 box_top    = box_bottom + F32(style::SHORTCUT_BTN_HEIGHT);
                for (S32 i = 0; i < (S32)mSlots.size(); ++i)
                {
                    const Slot& slot = mSlots[i];
                    const bool hover = (i == mHover) && slot.enabled;
                    if (slot.separatorX >= 0)
                    {
                        plus::psui::fillRect(LLRectf(F32(slot.separatorX), box_top - F32(SEPARATOR_INSET), F32(slot.separatorX + 1),
                                                     box_bottom + F32(SEPARATOR_INSET)),
                                             plus::psui::Fill::solid(plus::psui::faded(style::DIVIDER_RIGHT, alpha)));
                    }
                    drawButton(LLRectf(F32(slot.x - BTN_PAD), box_top, F32(slot.x + slot.width + BTN_PAD), box_bottom),
                               slot.label, text_y, { hover, hover && mDownOn == i, slot.checked, slot.enabled }, alpha);
                }
                if (mMoreX >= 0)
                {
                    const bool hover = (mHover == MORE);
                    drawButton(LLRectf(F32(mMoreX - BTN_PAD), box_top, F32(mMoreX + mMoreWidth + BTN_PAD), box_bottom),
                               mMoreLabel, text_y, { hover, hover && mDownOn == MORE, false, true }, alpha);
                }
            }

            bool handleHover(S32 x, S32 y, MASK mask) override
            {
                const S32 hit = hitTest(x);
                if (hit != mHover)
                {
                    mHover = hit;
                }
                if (hit == NONE)
                {
                    return false; // between the buttons the bar is clear: the world under it gets the mouse
                }
                getWindow()->setCursor(UI_CURSOR_HAND);
                return true;
            }

            void onMouseLeave(S32 x, S32 y, MASK mask) override
            {
                mHover  = NONE;
                mDownOn = NONE; // a press on a button let go outside the bar is dropped (no click on a later release)
                LLView::onMouseLeave(x, y, mask);
            }

            // The world's hover tip and double click do not come through a button (the clear part keeps them).
            bool handleToolTip(S32 x, S32 y, MASK mask) override
            {
                return hitTest(x) != NONE;
            }

            // The second release of a double click on a button is the bar's too (DOUBLE): let through, it
            // reached the world's tool, and the avatar glanced at what was clicked before (research/275 L068).
            bool handleDoubleClick(S32 x, S32 y, MASK mask) override
            {
                if (hitTest(x) == NONE)
                {
                    return false;
                }
                mDownOn = DOUBLE;
                return true;
            }

            bool handleMouseDown(S32 x, S32 y, MASK mask) override
            {
                mDownOn = hitTest(x);
                return mDownOn != NONE;
            }

            bool handleMouseUp(S32 x, S32 y, MASK mask) override
            {
                const S32 hit = hitTest(x);
                const S32 down = mDownOn;
                mDownOn = NONE;
                if (down == NONE)
                {
                    return false; // the press was not on a button (the world's, or nobody's)
                }
                if (down == DOUBLE || hit != down)
                {
                    return true;
                }
                if (hit == MORE)
                {
                    showMoreMenu(x, y);
                }
                else
                {
                    run(mSlots[hit].entry);
                }
                return true;
            }

            bool handleRightMouseDown(S32 x, S32 y, MASK mask) override
            {
                const S32 hit = hitTest(x);
                // On the clear part the world's own right click (the pie menu) is kept; an empty bar's hint
                // text still opens the bar's menu, so the bar can be hidden from there.
                const bool on_hint = mSlots.empty() && sEntries.empty()
                                     && x >= MARGIN_LEFT - BTN_PAD && x < MARGIN_LEFT + mEmptyWidth + BTN_PAD;
                if (hit == NONE && !on_hint)
                {
                    return false;
                }
                showBarMenu(hit >= 0 ? mSlots[hit].entry : -1, x, y);
                return true;
            }

            // Runs an entry as a click on its menu item would.
            static void run(S32 entry)
            {
                if (entry < 0 || entry >= (S32)sEntries.size())
                {
                    return;
                }
                LLMenuItemGL* item = resolve(sEntries[entry].path);
                if (!item)
                {
                    return;
                }
                item->buildDrawLabel(); // the enabled state, as the menu would show it
                if (!item->getEnabled())
                {
                    return;
                }
                item->onCommit();
            }

        private:
            static constexpr S32 NONE = -1;
            static constexpr S32 MORE = -2;
            static constexpr S32 DOUBLE = -3;   // mDownOn only: the second press of a double click on a button

            struct Slot
            {
                S32         entry = 0;
                std::string label;
                S32         x = 0;
                S32         width = 0;
                S32         separatorX = -1;
                bool        enabled = true;
                bool        checked = false;
            };

            // One button: a rounded translucent box (the world buttons' navy; hovered: lighter with a lighter
            // line; pressed: darker, the text one pixel down; on = a menu item with a check mark that is
            // checked: blue as a chosen segment; disabled: all of it at 40 %) and its text in the middle.
            struct ButtonState
            {
                bool hover   = false;   // under the mouse (an enabled button only)
                bool pressed = false;   // the mouse button is down on it
                bool on      = false;   // a check mark item that is checked
                bool enabled = true;
            };

            void drawButton(const LLRectf& box, const std::string& label, S32 text_y, const ButtonState& state,
                            F32 alpha) const
            {
                const bool hover = state.hover, pressed = state.pressed, enabled = state.enabled;
                const bool on = state.on && enabled;
                const F32 a = enabled ? alpha : alpha * style::STATE_DISABLED;
                plus::psui::Fill fill = on
                    ? plus::psui::Fill::horizontal(style::SHORTCUT_ON_LEFT, style::SHORTCUT_ON_RIGHT)
                    : plus::psui::Fill::solid(style::SHORTCUT_FILL);
                LLColor4 border = on ? style::BUTTON_ON_BORDER : style::BUTTON_BORDER;
                if (pressed)
                {
                    fill = fill.pressed();
                }
                else if (hover)
                {
                    fill = fill.hovered();
                }
                if (hover)
                {
                    border = style::BUTTON_BORDER_HOVER;
                }
                plus::psui::drawRoundBordered(box, style::SHORTCUT_BTN_RADIUS, fill.faded(a), plus::psui::faded(border, a));
                const LLColor4 text = on ? style::TAB_TEXT_ON : style::TEXT_WHITE;
                LLFontGL::getFontSansSerifSmall()->renderUTF8(label, 0, S32(box.mLeft) + BTN_PAD, text_y - (pressed ? 1 : 0),
                                                              plus::psui::faded(text, a), LLFontGL::LEFT, LLFontGL::BOTTOM,
                                                              LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
            }

            S32 hitTest(S32 x) const
            {
                for (S32 i = 0; i < (S32)mSlots.size(); ++i)
                {
                    if (x >= mSlots[i].x - BTN_PAD && x < mSlots[i].x + mSlots[i].width + BTN_PAD)
                    {
                        return i;
                    }
                }
                if (mMoreX >= 0 && x >= mMoreX - BTN_PAD && x < mMoreX + mMoreWidth + BTN_PAD)
                {
                    return MORE;
                }
                return NONE;
            }

            // Makes the slots again when the list or the width changed, and reads the items' states
            // every half second (the menu's own update runs only while it is open).
            void refresh(const LLFontGL* font)
            {
                const S32 width = getRect().getWidth();
                const bool state_due = mRefresh.getElapsedTimeF32() > REFRESH_SECONDS;
                if (!sDirty && width == mLayoutWidth && !state_due)
                {
                    return;
                }
                if (state_due)
                {
                    mRefresh.reset();
                }
                sDirty = false;
                mLayoutWidth = width;

                // Labels are read once per call; a text that did not change keeps its width.
                std::vector<Slot> all;
                all.reserve(sEntries.size());
                for (S32 i = 0; i < (S32)sEntries.size(); ++i)
                {
                    LLMenuItemGL* item = resolve(sEntries[i].path);
                    if (!item)
                    {
                        continue; // not in this menu: left out of the bar, kept in the file
                    }
                    item->buildDrawLabel();
                    Slot slot;
                    slot.entry   = i;
                    slot.label   = sEntries[i].alias.empty() ? shortName(item->getLabel()) : trimmed(sEntries[i].alias);
                    slot.width   = font->getWidth(slot.label);
                    slot.enabled = item->getEnabled();
                    slot.checked = dynamic_cast<LLMenuItemCheckGL*>(item) && item->getValue().asBoolean();
                    all.push_back(slot);
                }

                mMoreLabel = LLTrans::getString("PlusShortcutBarMore");
                mMoreWidth = font->getWidth(mMoreLabel);
                mEmptyLabel = LLTrans::getString("PlusShortcutBarEmpty");
                mEmptyWidth = font->getWidth(mEmptyLabel);
                mSlots.clear();
                mMoreX = -1;

                const S32 right = width - MARGIN_LEFT;
                S32 x = MARGIN_LEFT;
                bool overflow = false;
                for (size_t n = 0; n < all.size(); ++n)
                {
                    Slot& slot = all[n];
                    const bool sep = sEntries[slot.entry].separator && !mSlots.empty();
                    const S32 sep_w = sep ? SEPARATOR_EXTRA : 0;
                    // Room for this one and, if more follow, the "more" button.
                    const S32 need_more = (n + 1 < all.size()) ? GAP + mMoreWidth : 0;
                    if (x + sep_w + slot.width + need_more > right)
                    {
                        overflow = true;
                        // The last that fit is kept if the "more" button still has room next to it.
                        break;
                    }
                    if (sep)
                    {
                        // the line is midway between the previous button's right edge and this one's left edge
                        slot.separatorX = ((x - BTN_PAD - style::SHORTCUT_BTN_GAP) + (x + sep_w - BTN_PAD)) / 2;
                        x += sep_w;
                    }
                    slot.x = x;
                    x += slot.width + GAP;
                    mSlots.push_back(slot);
                }
                mOverflowFrom = overflow ? (S32)mSlots.size() : -1;
                if (overflow)
                {
                    mMoreX = x;
                    mAll   = all;
                }
                else
                {
                    mAll.clear();
                }
            }

            void showMoreMenu(S32 x, S32 y)
            {
                if (mOverflowFrom < 0)
                {
                    return;
                }
                if (LLContextMenu* old_menu = mMoreMenuHandle.get())
                {
                    old_menu->die();
                    mMoreMenuHandle.markDead();
                }
                LLContextMenu::Params params;
                params.name = "plus_shortcut_more";
                LLContextMenu* menu = LLUICtrlFactory::create<LLContextMenu>(params);
                if (!menu)
                {
                    return;
                }
                LLMenuGL::sMenuContainer->addChild(menu);
                mMoreMenuHandle = menu->getHandle();
                for (size_t n = (size_t)mOverflowFrom; n < mAll.size(); ++n)
                {
                    LLMenuItemCallGL::Params item_params;
                    item_params.name  = "more_" + std::to_string(n);
                    item_params.label = mAll[n].label;
                    LLMenuItemCallGL* item = LLUICtrlFactory::create<LLMenuItemCallGL>(item_params);
                    if (!item)
                    {
                        continue;
                    }
                    const S32 entry = mAll[n].entry;
                    item->setCommitCallback([entry](LLUICtrl*, const LLSD&) { run(entry); });
                    item->setEnabled(mAll[n].enabled);
                    menu->addChild(item);
                }
                LLMenuGL::showPopup(this, menu, x, y);
            }

            void showBarMenu(S32 entry, S32 x, S32 y)
            {
                if (LLContextMenu* old_menu = mMenuHandle.get())
                {
                    old_menu->die();
                    mMenuHandle.markDead();
                }
                LLUICtrl::CommitCallbackRegistry::ScopedRegistrar registrar;
                LLUICtrl::EnableCallbackRegistry::ScopedRegistrar enable_registrar;
                registrar.add("Plus.Shortcut.Bar", [entry](LLUICtrl*, const LLSD& param) { barAction(entry, param.asString()); });
                enable_registrar.add("Plus.Shortcut.Bar.Enable", [entry](LLUICtrl*, const LLSD& param)
                {
                    return barEnabled(entry, param.asString());
                });

                LLContextMenu* menu = LLUICtrlFactory::getInstance()->createFromFile<LLContextMenu>(
                    MENU_BAR_FILE, LLMenuGL::sMenuContainer, LLViewerMenuHolderGL::child_registry_t::instance());
                if (!menu)
                {
                    return;
                }
                mMenuHandle = menu->getHandle();
                menu->show(x, y);
                LLMenuGL::showPopup(this, menu, x, y);
            }

            static bool barEnabled(S32 entry, const std::string& what)
            {
                if (what == "slot")
                {
                    return entry >= 0 && entry < (S32)sEntries.size();
                }
                if (what == "left")
                {
                    return entry > 0 && entry < (S32)sEntries.size();
                }
                if (what == "right")
                {
                    return entry >= 0 && entry + 1 < (S32)sEntries.size();
                }
                if (what == "any")
                {
                    return !sEntries.empty();
                }
                return true;
            }

            static void barAction(S32 entry, const std::string& what);

            std::vector<Slot>  mSlots;
            std::vector<Slot>  mAll;          // all that resolved, for the "more" menu
            S32                mOverflowFrom = -1;
            S32                mMoreX = -1;
            S32                mMoreWidth = 0;
            std::string        mMoreLabel;
            std::string        mEmptyLabel;   // the hint of an empty bar
            S32                mEmptyWidth = 0;
            S32                mHover = NONE;
            S32                mDownOn = NONE;
            S32                mLayoutWidth = -1;
            LLFrameTimer       mRefresh;
            LLHandle<LLContextMenu> mMenuHandle;
            LLHandle<LLContextMenu> mMoreMenuHandle;
        };

        LLHandle<LLView> sBar;

        ShortcutBarView* bar()
        {
            return dynamic_cast<ShortcutBarView*>(sBar.get());
        }

        void swapEntries(S32 a, S32 b)
        {
            std::swap(sEntries[a], sEntries[b]);
        }

        void onRenameAnswer(const LLSD& notification, const LLSD& response)
        {
            if (LLNotificationsUtil::getSelectedOption(notification, response) != 0)
            {
                return;
            }
            // The entry is found by its menu path: the list may have been changed while the question was open.
            const LLSD& names = notification["payload"]["path"];
            if (!names.isArray())
            {
                return;
            }
            Path path;
            for (LLSD::array_const_iterator it = names.beginArray(); it != names.endArray(); ++it)
            {
                path.push_back(it->asString());
            }
            const S32 entry = indexOf(path);
            if (entry < 0)
            {
                return;
            }
            sEntries[entry].alias = utf8str_truncate(trimmed(response["alias"].asString()), MAX_ALIAS_BYTES);
            changed();
        }

    }

    // Defined here: it uses the helpers above.
    namespace
    {
        void ShortcutBarView::barAction(S32 entry, const std::string& what)
        {
            if (what == "clear")
            {
                sEntries.clear();
                changed();
                return;
            }
            if (what == "hide")
            {
                gSavedSettings.setBOOL(SETTING_SHOW, false);
                return;
            }
            if (entry < 0 || entry >= (S32)sEntries.size())
            {
                return;
            }
            if (what == "remove")
            {
                sEntries.erase(sEntries.begin() + entry);
                changed();
            }
            else if (what == "left" && entry > 0)
            {
                swapEntries(entry, entry - 1);
                changed();
            }
            else if (what == "right" && entry + 1 < (S32)sEntries.size())
            {
                swapEntries(entry, entry + 1);
                changed();
            }
            else if (what == "separator")
            {
                sEntries[entry].separator = !sEntries[entry].separator;
                changed();
            }
            else if (what == "rename")
            {
                LLSD path = LLSD::emptyArray();
                for (const std::string& name : sEntries[entry].path)
                {
                    path.append(name);
                }
                LLSD payload;
                payload["path"] = path;
                LLNotificationsUtil::add("PlusShortcutRename", LLSD(), payload, &onRenameAnswer);
            }
        }
    }

    namespace
    {
        LLHandle<LLView>        sPanel;
        S32                     sTopOffset = 0; // the panel is this far below the top of the world view
        LLHandle<LLContextMenu> sItemMenu; // the last menu of a right click on a menu bar item
        bool                    sSuppressed = false; // setSuppressed(): another mode (the photo screen) hides the bar

        // The menu of a right click on an item of the menu bar: add it to the bar, or take it off.
        void showItemMenu(LLMenuItemGL* item, const Path& path, S32 x, S32 y)
        {
            LLUICtrl::CommitCallbackRegistry::ScopedRegistrar registrar;
            LLUICtrl::EnableCallbackRegistry::ScopedRegistrar enable_registrar;
            registrar.add("Plus.Shortcut.Item", [path](LLUICtrl*, const LLSD& param)
            {
                const std::string what = param.asString();
                const S32 at = indexOf(path);
                if (what == "add" && at < 0 && (S32)sEntries.size() < MAX_ENTRIES)
                {
                    Entry e;
                    e.path = path;
                    sEntries.push_back(e);
                    changed();
                }
                else if (what == "remove" && at >= 0)
                {
                    sEntries.erase(sEntries.begin() + at);
                    changed();
                }
            });
            enable_registrar.add("Plus.Shortcut.Item.Enable", [path](LLUICtrl*, const LLSD& param)
            {
                const bool added = indexOf(path) >= 0;
                return param.asString() == "added" ? added : !added;
            });

            if (LLContextMenu* old_menu = sItemMenu.get())
            {
                old_menu->die();
                sItemMenu.markDead();
            }
            // The pull-down the item is in is closed first: a right button let go over another of its items
            // (on the way to this menu) would run that item, as a right drag in a menu does.
            LLMenuGL::sMenuContainer->hideMenus();

            LLContextMenu* menu = LLUICtrlFactory::getInstance()->createFromFile<LLContextMenu>(
                MENU_ITEM_FILE, LLMenuGL::sMenuContainer, LLViewerMenuHolderGL::child_registry_t::instance());
            if (!menu)
            {
                return;
            }
            sItemMenu = menu->getHandle();
            menu->show(x, y);
            LLMenuGL::showPopup(item, menu, x, y);
        }

        // sRightClickHook: only the items that run something and are in the menu bar (the popup menus
        // of the bar itself are not, so they keep their right click).
        bool onItemRightClick(LLMenuItemGL* item, S32 x, S32 y)
        {
            if (!gMenuBarView || !dynamic_cast<LLMenuItemCallGL*>(item))
            {
                return false;
            }
            Path path;
            if (!findPath(gMenuBarView, item, path))
            {
                return false;
            }
            showItemMenu(item, path, x, y);
            return true;
        }

        // The panel shows when the setting is on, the UI is shown, the avatar is in the world and no
        // other mode hides it (setSuppressed(); the photo screen also by isActive(), every idle).
        void updateVisibility(void*)
        {
            static LLCachedControl<bool> show(gSavedSettings, SETTING_SHOW, true);
            LLView* panel = sPanel.get();
            if (!panel)
            {
                return;
            }
            const bool want = show && !sSuppressed && !plus::photoscreen::isActive()
                              && gViewerWindow && gViewerWindow->getUIVisibility()
                              && LLStartUp::getStartupState() == STATE_STARTED;
            if (panel->getVisible() != want)
            {
                panel->setVisible(want);
            }
            if (!want)
            {
                return;
            }
            // The bar lies at the top of the world view; the mini location bar (topinfo_bar_container) is
            // there too, so the bar goes under it while it is shown. Moved only when that changes.
            S32 offset = 0;
            if (LLPanelTopInfoBar::instanceExists())
            {
                const LLPanelTopInfoBar* topinfo = LLPanelTopInfoBar::getInstance();
                if (topinfo->getVisible())
                {
                    offset = topinfo->getRect().getHeight();
                }
            }
            if (offset != sTopOffset)
            {
                panel->translate(0, sTopOffset - offset);
                sTopOffset = offset;
            }
        }

        void init()
        {
            // Settings: app_settings/settings_plus.xml (PlusShortcutBarShow, PlusShortcutBarBackground). The
            // panel is in main_view.xml, at the top of world_panel (over the world, as the Firestorm favorites bar).
        }

        void onUIReady()
        {
            if (sPanel.get() || !gViewerWindow || !gViewerWindow->getRootView())
            {
                return;
            }
            LLView* panel = gViewerWindow->getRootView()->findChildView(PANEL_NAME, true);
            if (!panel)
            {
                LL_WARNS("Plus") << "Shortcut bar: " << PANEL_NAME << " is not in main_view.xml" << LL_ENDL;
                return;
            }
            load();
            ShortcutBarView::Params params;
            params.name          = "plus_shortcut_bar";
            params.rect          = panel->getLocalRect();
            params.follows.flags = FOLLOWS_ALL;
            params.mouse_opaque  = false; // the clear part between the buttons is the world's
            ShortcutBarView* view = LLUICtrlFactory::create<ShortcutBarView>(params);
            if (!view)
            {
                return;
            }
            panel->addChild(view);
            panel->setVisible(false);
            sPanel = panel->getHandle();
            sBar   = view->getHandle();
            LLMenuItemGL::sRightClickHook = &onItemRightClick;
            gIdleCallbacks.addFunction(&updateVisibility);
        }

        plus::FeatureRegistration sRegistration({ "shortcutbar", &init, &onUIReady });
    }

    void setSuppressed(bool on)
    {
        if (sSuppressed == on)
        {
            return;
        }
        sSuppressed = on;
        updateVisibility(nullptr);  // at once, not at the next idle (no frame of the bar over the photo screen)
    }
}

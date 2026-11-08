/**
 * @file chatconsole.cpp
 * @brief Chat console: nearby chat lines at the bottom left of the screen while the nearby chat window
 *        is not open, in the chat's colors, fading out after a while.
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

// The idea is Firestorm's (FSConsoleUtils::ProcessChatMessage feeding gConsole, FSUseNearbyChatConsole);
// the code is Charis's own; the one official hook is replacesToasts() in
// LLFloaterIMNearbyChatHandler::processChat().
//
// The official viewer shows new nearby chat as toasts, or as bubbles over the speaker's head: someone
// speaking from outside the screen is seen in neither. The console lists every nearby chat line, wherever
// the speaker is, at the bottom left, as old viewers did.
//
// Lines come from the "LLChat" event pump, which LLFloaterIMNearbyChatHandler::processChat() posts to for
// each chat line it accepts (muted lines and empty ones are gone by then). Their color is the chat
// window's (LLViewerChat::getChatColor()). The text is "Name: message", "Name shouts: ..." for shouts and
// whispers, "Name does something" for /me, and the bare text for system lines with no sender. As in the
// chat window, an avatar's name is the display name and the viewer's links (secondlife:///app/agent/...,
// group, place, item) read as their labels; a line is made again when a name it waits for arrives.
//
// The view is a plain LLView at the back of the root view (like plus/fpscorner), so floaters cover it. It
// draws into the rectangle the official console (gConsole) keeps up for itself every frame, so the left
// edge, the width (ChatFullWidth) and the room over the chat bar are the official ones; the official
// console is not used (it takes white lines only).
//
// The official toasts of new chat lines are left out while the console is on (replacesToasts(), one hook
// in the chat handler), or every line would be shown twice.
//
// Not shown: while the nearby chat window's transcript is open (the window has the chat), in Do Not
// Disturb, before the agent is in the world, in a snapshot, and in the photo screen's detailed mode
// (plus/photoscreen, which shows the nearby chat in its own box beside the world). Its simple mode has no
// lines of its own, so the console is drawn there, its lines starting over the mode's chat line
// (chatConsoleFloor()). Nothing is saved or changed for that: the console is drawn as before as soon as
// the photo screen is gone, however it went.

#include "llviewerprecompiledheaders.h"

#include "chatconsole.h"

#include "plus.h"
#include "plus/presets/presets.h"
#include "plus/photoscreen/photoscreen.h"
#include "plus/photoscreen/psui/psstyle.h"

#include "llagent.h"
#include "llchat.h"
#include "llconsole.h"
#include "llevents.h"
#include "llfloaterimnearbychat.h"
#include "llmultifloater.h"
#include "llfloaterreg.h"
#include "llfontgl.h"
#include "llframetimer.h"
#include "llrender2dutils.h"
#include "llstartup.h"
#include "lltrans.h"
#include "lluicolortable.h"
#include "lluictrlfactory.h"
#include "llview.h"
#include "llviewerchat.h"
#include "llviewercontrol.h"

#include "llavatarname.h"
#include "llavatarnamecache.h"
#include "llurlmatch.h"
#include "llurlregistry.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"

#include <deque>
#include <set>
#include <string>
#include <vector>

namespace plus::chatconsole
{
    bool replacesToasts()
    {
        static LLCachedControl<bool> console_on(gSavedSettings, "PlusChatConsoleOn", true);
        return console_on;
    }

    F32 persistTime()
    {
        static LLCachedControl<F32> persist_time(gSavedSettings, "PlusChatConsolePersistTime", 20.f);
        return llmax(FADE_SECONDS + 1.f, (F32)persist_time);
    }

    F32 fadeAlpha(F64 added, F64 now)
    {
        return llclamp((F32)((persistTime() - (now - added)) / FADE_SECONDS), 0.f, 1.f);
    }

    namespace
    {
        constexpr char SETTING_ON[]      = "PlusChatConsoleOn";
        constexpr char SETTING_LINES[]   = "PlusChatConsoleMaxLines";

        constexpr S32 BUFFER_SIZE  = 60;   // chat lines kept (older ones are gone whatever their age)
        constexpr F32 PAD_X        = 10.f; // box to the text, as the official console
        constexpr F32 PAD_Y        = 3.f;
        constexpr S32 WRAP_MARGIN  = 30;   // narrower than the console, as the official console

        // One chat line, wrapped to the console's width when drawn.
        struct Entry
        {
            LLChat                 source;           // the line as it came: the text is made again from it when a name arrives
            LLWString              text;
            LLColor4               color;
            F64                    added = 0.0;      // LLFrameTimer::getElapsedSeconds()
            std::vector<LLWString> lines;            // text wrapped to wrappedWidth in wrappedFont
            S32                    wrappedWidth = -1;
            const LLFontGL*        wrappedFont  = nullptr;
            F32                    boxWidth     = 0.f;
        };

        std::deque<Entry> sEntries;

        bool inWorld()
        {
            return LLStartUp::getStartupState() >= STATE_STARTED;
        }

        const LLFontGL* chatFont()
        {
            // the numbers of the official LLConsole::setFontSize() (ChatFontSize)
            static LLCachedControl<S32> font_size(gSavedSettings, "ChatFontSize", 1);
            const S32 size = font_size;
            const LLFontGL* font = nullptr;
            if (size < 0)
            {
                font = LLFontGL::getFontMonospace();
            }
            else if (size == 0)
            {
                font = LLFontGL::getFontSansSerif();
            }
            else if (size == 1)
            {
                font = LLFontGL::getFontSansSerifBig();
            }
            else
            {
                font = LLFontGL::getFontSansSerifHuge();
            }
            return font ? font : LLFontGL::getFontDefault();
        }

        // "/me " and "/me'" start an emote.
        bool isMePrefix(const std::string& text)
        {
            return text.size() > 3 && text.compare(0, 3, "/me") == 0 && (text[3] == ' ' || text[3] == '\'');
        }

        void rebuild(Entry& entry);

        // A name or a label has arrived: the lines that wait for it are made again (and wrapped again).
        void onLinkLabel(const std::string& url, const std::string& /*label*/, const std::string& /*icon*/)
        {
            for (Entry& entry : sEntries)
            {
                if (entry.source.mText.find(url) != std::string::npos)
                {
                    rebuild(entry);
                }
            }
        }

        std::set<LLUUID> sAwaitedNames;     // speakers whose name has been asked for
        bool sRebuildingForName = false;    // onSpeakerName() is remaking lines

        void onSpeakerName(const LLUUID& id, const LLAvatarName& /*name*/)
        {
            sAwaitedNames.erase(id);
            // A callback fired from inside the remaking (a name already in the cache answers at once)
            // does not remake again: the lines being remade read the name from the cache.
            if (sRebuildingForName)
            {
                return;
            }
            struct Guard
            {
                Guard() { sRebuildingForName = true; }
                ~Guard() { sRebuildingForName = false; }
            } guard;
            for (Entry& entry : sEntries)
            {
                if (entry.source.mSourceType == CHAT_SOURCE_AGENT && entry.source.mFromID == id)
                {
                    rebuild(entry);
                }
            }
        }

        // The links of the viewer in the text as the chat window shows them (their labels: an avatar's or a
        // group's name, a place, an item), not the raw "secondlife:///app/..." (the console draws plain
        // text). A label still loading reads "Loading..." until onLinkLabel() puts it right.
        std::string resolveLinks(const std::string& text)
        {
            std::string out;
            std::string rest = text;
            LLUrlMatch match;
            while (!rest.empty() && LLUrlRegistry::instance().findUrl(rest, match, &onLinkLabel))
            {
                const size_t start = match.getStart();
                const size_t end   = static_cast<size_t>(match.getEnd()) + 1;
                if (end <= start || end > rest.size())
                {
                    break;
                }
                out.append(rest, 0, start);
                out += match.getLabel();
                out += match.getQuery();
                rest.erase(0, end);
            }
            return out + rest;
        }

        // The speaker's name as the chat window shows it: an avatar's display name (the name the simulator
        // sent, the old name, until the display name is known; the line is made again then).
        std::string speakerName(const LLChat& chat)
        {
            if (chat.mSourceType != CHAT_SOURCE_AGENT || chat.mFromID.isNull())
            {
                return chat.mFromName;
            }
            if (chat.mRlvNamesFiltered)
            {
                return chat.mFromName;  // RLVa @shownames: the anonym the line came with (plus/rlv, research/248 A1)
            }
            LLAvatarName name;
            if (LLAvatarNameCache::get(chat.mFromID, &name))
            {
                // In the cache: asking again would answer at once (and remake this line, without end).
                const std::string& display = name.getDisplayName();
                return !display.empty() ? display : chat.mFromName;
            }
            if (sAwaitedNames.insert(chat.mFromID).second)
            {
                LLAvatarNameCache::get(chat.mFromID, &onSpeakerName);
            }
            return chat.mFromName;
        }

        std::string consoleText(const LLChat& chat)
        {
            if (chat.mFromName.empty())
            {
                return resolveLinks(chat.mText); // a system line (or a line with no sender)
            }
            const std::string name = speakerName(chat);
            if (isMePrefix(chat.mText))
            {
                // "/me waves" -> "Name waves", "/me's hat" -> "Name's hat"
                const std::string body = resolveLinks(chat.mText.substr(chat.mText[3] == ' ' ? 4 : 3));
                return name + (chat.mText[3] == ' ' ? std::string(" ") : std::string()) + body;
            }
            const std::string text = resolveLinks(chat.mText);
            if (chat.mChatType == CHAT_TYPE_SHOUT || chat.mChatType == CHAT_TYPE_WHISPER)
            {
                // process_chat_from_simulator() already put "shouts: " / "whispers: " in front of the text
                return name + " " + text;
            }
            return name + ": " + text;
        }

        void rebuild(Entry& entry)
        {
            entry.text         = utf8str_to_wstring(consoleText(entry.source));
            entry.wrappedWidth = -1;
        }

        // The place of the speaker when it is in view: the chat window's color is fainter for a speaker
        // beyond the chat range (LLViewerChat::getChatColor()), and the chat event carries no place.
        LLVector3 speakerPosition(const LLChat& chat)
        {
            if ((chat.mSourceType == CHAT_SOURCE_AGENT || chat.mSourceType == CHAT_SOURCE_OBJECT) && chat.mFromID.notNull())
            {
                if (LLViewerObject* object = gObjectList.findObject(chat.mFromID))
                {
                    if (!object->isDead())
                    {
                        return object->getPositionAgent();
                    }
                }
            }
            return LLVector3::zero;
        }

        void addEntry(const LLChat& chat)
        {
            if (!gSavedSettings.getBOOL(SETTING_ON) || gAgent.isDoNotDisturb())
            {
                return;
            }
            if (chat.mChatType == CHAT_TYPE_START || chat.mChatType == CHAT_TYPE_STOP) // typing signs
            {
                return;
            }
            if (chat.mChatType == CHAT_TYPE_DIRECT && !chat.mText.empty() && chat.mText[0] == '@') // a script's command (RLV)
            {
                return;
            }
            const std::string text = consoleText(chat);
            if (text.empty())
            {
                return;
            }

            LLChat placed = chat;
            placed.mPosAgent = speakerPosition(chat);
            LLUIColor color;
            F32 alpha = 1.f;
            LLViewerChat::getChatColor(placed, color, alpha);

            Entry entry;
            entry.source = chat;
            entry.text  = utf8str_to_wstring(text);
            entry.color = color.get();
            entry.color.mV[VALPHA] *= alpha;
            entry.added = LLFrameTimer::getElapsedSeconds();
            sEntries.push_back(std::move(entry));
            while ((S32)sEntries.size() > BUFFER_SIZE)
            {
                sEntries.pop_front();
            }
        }

        bool onChatEvent(const LLSD& event)
        {
            LLChat chat;
            chat.mText       = event["message"].asString();
            chat.mFromName   = event["from"].asString();
            chat.mFromID     = event["from_id"].asUUID();
            chat.mSourceType = (EChatSourceType)event["source"].asInteger();
            chat.mChatType   = (EChatType)event["chat_type"].asInteger();
            chat.mChatStyle  = (EChatStyle)event["chat_style"].asInteger();
            addEntry(chat);
            return false; // other listeners get it too
        }

        // Cuts the text into lines that fit width (the official console's word wrap).
        void wrap(Entry& entry, const LLFontGL* font, S32 width)
        {
            entry.lines.clear();
            entry.boxWidth     = 0.f;
            entry.wrappedWidth = width;
            entry.wrappedFont  = font;
            const F32 limit = (F32)(width - WRAP_MARGIN);
            const S32 length = (S32)entry.text.length();
            S32 offset = 0;
            while (offset < length)
            {
                const size_t newline = entry.text.find(llwchar('\n'), offset);
                const S32 line_end = newline == LLWString::npos ? length : (S32)newline;
                const S32 skip     = newline == LLWString::npos ? 0 : 1;
                const S32 drawable = (S32)font->maxDrawableChars(entry.text.c_str() + offset, limit, line_end - offset,
                                                                 LLFontGL::WORD_BOUNDARY_IF_POSSIBLE);
                if (drawable > 0)
                {
                    LLWString line = entry.text.substr(offset, drawable);
                    entry.boxWidth = llmax(entry.boxWidth, (F32)font->getWidth(line.c_str()));
                    entry.lines.push_back(std::move(line));
                }
                else if (skip == 0)
                {
                    break; // not even one letter fits: nothing to draw
                }
                offset += drawable + skip;
            }
        }

        // The chat window's transcript is open: the chat is there to read.
        bool chatWindowOpen()
        {
            LLFloaterIMNearbyChat* nearby = LLFloaterReg::findTypedInstance<LLFloaterIMNearbyChat>("nearby_chat");
            if (!nearby || !nearby->isInVisibleChain() || !nearby->isMessagePaneExpanded())
            {
                return false;
            }
            // a minimized floater keeps its children's visible flags but is not drawn (the conversations
            // window, when the chat is its tab, is the one minimized)
            const LLFloater* top = nearby;
            if (const LLFloater* host = nearby->getHost())
            {
                top = host;
            }
            return !top->isMinimized();
        }

        class ChatConsoleView : public LLView
        {
        public:
            struct Params : public LLInitParam::Block<Params, LLView::Params>
            {
                Params() {}
            };

        protected:
            explicit ChatConsoleView(const Params& p) : LLView(p) {}
            friend class LLUICtrlFactory;

        public:
            void draw() override
            {
                static LLCachedControl<bool> console_on(gSavedSettings, SETTING_ON, true);
                static LLCachedControl<S32> max_lines_setting(gSavedSettings, SETTING_LINES, 12);
                // off: nothing to do (lines are only added while on, and the old ones are dropped below
                // before anything is drawn when it is turned on again)
                if (!console_on)
                {
                    return;
                }
                // old lines go even when nothing is drawn
                const F64 now     = LLFrameTimer::getElapsedSeconds();
                const F64 persist = persistTime();
                while (!sEntries.empty() && now - sEntries.front().added >= persist)
                {
                    sEntries.pop_front();
                }
                if (sEntries.empty() || !gConsole || !inWorld()
                    || plus::presets::snapshotInProgress()
                    || (plus::photoscreen::isActive() && !plus::photoscreen::isSimple())
                    || gAgent.isDoNotDisturb() || chatWindowOpen())
                {
                    return;
                }
                const LLFontGL* font = chatFont();
                if (!font)
                {
                    return;
                }

                // the official console's place (kept up by LLViewerWindow::updateUI()), in screen pixels; in the
                // photo screen's simple mode between its chat line and its rail / open panel
                LLRect area = gConsole->getRect();
                if (const S32 ceiling = plus::photoscreen::chatConsoleCeiling(); ceiling > 0)
                {
                    area.mTop = llmin(area.mTop, ceiling);
                }
                const S32 width   = area.getWidth();
                const S32 max_lines = llmax(1, (S32)max_lines_setting);
                const F32 line_height = (F32)font->getLineHeight();

                static LLCachedControl<F32> background_opacity(gSavedSettings, "ConsoleBackgroundOpacity", 0.7f);
                static const LLUIColor background = LLUIColorTable::instance().getColor("ConsoleBackground", LLColor4(0.f, 0.f, 0.f, 0.5f));
                LLColor4 box_color = background.get();
                box_color.mV[VALPHA] *= llclamp((F32)background_opacity, 0.f, 1.f);

                // newest at the bottom (over the photo screen's chat line in its simple mode); stack upwards until
                // the lines or the room run out
                F32 y = (F32)llmax(area.mBottom, plus::photoscreen::chatConsoleFloor());
                if (y >= area.mTop)
                {
                    return;     // no room between them
                }
                S32 lines_drawn = 0;
                for (auto it = sEntries.rbegin(); it != sEntries.rend(); ++it)
                {
                    Entry& entry = *it;
                    if (entry.wrappedFont != font || entry.wrappedWidth != width)
                    {
                        wrap(entry, font, width);
                    }
                    S32 count = (S32)entry.lines.size();
                    if (count == 0)
                    {
                        continue;
                    }
                    // the box of count lines reaches up to y + (count + 1) line heights (its top is top_y +
                    // line_height below): that must stay under the console's top
                    const S32 room = llmin(max_lines - lines_drawn, (S32)((area.mTop - y) / line_height) - 1);
                    if (count > room)
                    {
                        // a line too long for the room (a script's many-line message) is cut to the room, but
                        // only the newest one; an older one that does not fit whole ends the stack
                        if (lines_drawn > 0 || room <= 0)
                        {
                            break;
                        }
                        count = room;
                    }
                    lines_drawn += count;

                    const F32 alpha = fadeAlpha(entry.added, now);

                    // placed as the official console does: the bottom line's baseline one line height over
                    // the paragraph's bottom, the box from just under that line's descent
                    const F32 top_y      = y + count * line_height;
                    const S32 box_height = llfloor(count * line_height + PAD_Y);
                    const S32 box_width  = llfloor(entry.boxWidth + PAD_X);
                    const S32 box_left   = area.mLeft - 14;
                    const S32 box_bottom = llfloor(top_y + line_height) - box_height;
                    LLColor4 box = box_color;
                    box.mV[VALPHA] *= alpha;
                    gl_rect_2d(box_left, box_bottom + box_height, box_left + box_width, box_bottom, box);

                    LLColor4 color = entry.color;
                    color.mV[VALPHA] *= alpha;
                    for (S32 i = 0; i < count; ++i)
                    {
                        font->render(entry.lines[i], 0, (F32)(area.mLeft - 8), top_y - i * line_height, color, LLFontGL::LEFT,
                                     LLFontGL::BASELINE, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW, S32_MAX, box_width);
                    }
                    y = top_y + PAD_Y;
                }
            }
        };

        LLHandle<LLView> sView;
        LLTempBoundListener sListener;

        void attachView()
        {
            psui::attachWorldOverlay<ChatConsoleView>(sView, "plus_chat_console");
        }

        void onUIReady()
        {
            attachView();
            sListener = LLEventPumps::instance().obtain("LLChat").listen("plus_chatconsole", &onChatEvent);
        }

        plus::FeatureRegistration sRegistration({ "chatconsole", [] {}, &onUIReady });
    }
}

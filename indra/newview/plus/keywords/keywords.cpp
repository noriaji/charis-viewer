/**
 * @file keywords.cpp
 * @brief Chat keyword alerts: the matching, the sound and notice, the color (plus/keywords).
 *
 * Ported from Firestorm's fskeywords.cpp (see ORIGIN.md).
 *
 * $LicenseInfo:firstyear=2011&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2011, The Phoenix Firestorm Project, Inc.
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
 *
 * The Phoenix Firestorm Project, Inc., 1831 Oakwood Drive, Fairmont, Minnesota 56031-3225 USA
 * http://www.firestormviewer.org
 * $/LicenseInfo$
 */

// Ported from Firestorm's FSKeywords (fskeywords.cpp). What is changed:
//  - The matching is done on characters, not on bytes, and Japanese works. Firestorm cuts the
//    UTF-8 text with boost::regex "\b", which has no word boundary inside Japanese text (so whole-
//    word matching never finds a Japanese word), and lowercases bytes only (ASCII).
//    Here: the text and the words are widened to UTF-32 and "folded" the same way before they are
//    compared: capitals to small (Latin, Greek, Cyrillic), full-width letters, digits and signs to
//    normal width, half-width katakana to full width, katakana to hiragana ("ありす" finds "アリス"
//    and "ｱﾘｽ"). The folding of widths and kana is a setting (PlusKeywordsFoldWidth, on).
//  - "Whole words": a word counts as whole when the characters next to it are not letters or digits
//    of a script written with spaces (Latin, Greek, Cyrillic, ...) or "_". Japanese (and Chinese)
//    characters never count as a word, since they have no spaces: a Japanese keyword is found
//    wherever it stands, whatever the setting. Only the edge of the keyword is checked, and only
//    when that edge is a letter or digit ("ab" does not match inside "cab", "ありす" matches in "abcありす").
//  - The words are separated by "," "，" "、" or a new line; empty ones are skipped (Firestorm's
//    empty word after a trailing comma matches every line).
//  - Your own name (display name, user name, first name) can be added to the list for you.
//  - The sound is not played in "Do not disturb" and not more than twice a second; a notice on the
//    screen is an option.
//  - Not taken: Growl, the alert from the friends' online notice and from the radar (llcallingcard,
//    fsradar) and the object IM (llimprocessing) of Firestorm.

#include "llviewerprecompiledheaders.h"

#include "keywords.h"
#include "plus.h"
#include "plus/rlv/rlvhooks.h"

#include <algorithm>
#include <map>
#include <vector>

#include "llagent.h"
#include "llavatarname.h"
#include "llavatarnamecache.h"
#include "llchat.h"
#include "llinstantmessage.h"
#include "llmutelist.h"
#include "llnotificationsutil.h"
#include "lltimer.h"
#include "lltrans.h"
#include "lluicolortable.h"
#include "llui.h"
#include "llviewercontrol.h"

namespace plus::keywords
{
    namespace
    {
        // Settings (settings_plus.xml, per account)
        const char* const SET_ON        = "PlusKeywordsOn";
        const char* const SET_IN_CHAT   = "PlusKeywordsInChat";
        const char* const SET_IN_IM     = "PlusKeywordsInIM";
        const char* const SET_LIST      = "PlusKeywordsList";
        const char* const SET_OWN_NAME  = "PlusKeywordsOwnName";
        const char* const SET_SPEAKER   = "PlusKeywordsSpeakerName";
        const char* const SET_CASE      = "PlusKeywordsCaseSensitive";
        const char* const SET_WHOLE     = "PlusKeywordsWholeWords";
        const char* const SET_FOLD      = "PlusKeywordsFoldWidth";
        const char* const SET_COLOR_ON  = "PlusKeywordsChangeColor";
        const char* const SET_COLOR     = "PlusKeywordsColor";
        const char* const SET_SOUND_ON  = "PlusKeywordsPlaySound";
        const char* const SET_SOUND     = "PlusKeywordsSound";
        const char* const SET_TOAST     = "PlusKeywordsToast";

        const char* const COLOR_NAME    = "PlusKeywordColor";

        // Half-width katakana U+FF66..U+FF9D as hiragana (the long vowel mark stays as it is).
        const llwchar HALF_KANA[] =
        {
            0x3092, 0x3041, 0x3043, 0x3045, 0x3047, 0x3049, 0x3083, 0x3085, 0x3087, 0x3063, 0x30FC,
            0x3042, 0x3044, 0x3046, 0x3048, 0x304A,
            0x304B, 0x304D, 0x304F, 0x3051, 0x3053,
            0x3055, 0x3057, 0x3059, 0x305B, 0x305D,
            0x305F, 0x3061, 0x3064, 0x3066, 0x3068,
            0x306A, 0x306B, 0x306C, 0x306D, 0x306E,
            0x306F, 0x3072, 0x3075, 0x3078, 0x307B,
            0x307E, 0x307F, 0x3080, 0x3081, 0x3082,
            0x3084, 0x3086, 0x3088,
            0x3089, 0x308A, 0x308B, 0x308C, 0x308D,
            0x308F, 0x3093
        };
        const llwchar HALF_KANA_FIRST = 0xFF66;
        const llwchar HALF_KANA_LAST  = 0xFF9D;
        static_assert(sizeof(HALF_KANA) / sizeof(HALF_KANA[0]) == HALF_KANA_LAST - HALF_KANA_FIRST + 1,
                      "one hiragana for each half-width katakana");

        // は ひ ふ へ ほ
        bool isHaGyou(llwchar k)
        {
            return k == 0x306F || k == 0x3072 || k == 0x3075 || k == 0x3078 || k == 0x307B;
        }

        // か..ち, つ, て, と, は行 (the kana that take the voiced mark, written as the one before it)
        bool takesVoicedMark(llwchar k)
        {
            return (k >= 0x304B && k <= 0x3061 && ((k - 0x304B) % 2) == 0)
                || k == 0x3064 || k == 0x3066 || k == 0x3068 || isHaGyou(k);
        }

        llwchar lowerOf(llwchar c)
        {
            if (c >= 'A' && c <= 'Z')                    return c + 0x20;
            if (c < 0x80)                                return c;
            if (c >= 0xC0 && c <= 0xDE && c != 0xD7)     return c + 0x20;   // Latin-1
            if (c >= 0x391 && c <= 0x3A9)                return c + 0x20;   // Greek
            if (c >= 0x410 && c <= 0x42F)                return c + 0x20;   // Cyrillic
            if (c >= 0x400 && c <= 0x40F)                return c + 0x50;
            return c;
        }

        // Letters and digits of the scripts written with spaces, and "_".
        bool isWordChar(llwchar c)
        {
            if (c < 0x80)
            {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
            }
            return c >= 0xC0 && c < 0x2000 && c != 0xD7 && c != 0xF7;
        }

        bool isSpace(llwchar c)
        {
            return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == 0x3000;
        }

        LLWString fold(const LLWString& in, bool case_sensitive, bool fold_width)
        {
            LLWString out;
            out.reserve(in.size());
            for (size_t i = 0; i < in.size(); ++i)
            {
                llwchar c = in[i];
                if (fold_width)
                {
                    if (c == 0x3000)
                    {
                        c = ' ';
                    }
                    else if (c >= 0xFF01 && c <= 0xFF5E)
                    {
                        c -= 0xFEE0;
                    }
                    else if (c >= HALF_KANA_FIRST && c <= HALF_KANA_LAST)
                    {
                        llwchar kana = HALF_KANA[c - HALF_KANA_FIRST];
                        if (i + 1 < in.size())
                        {
                            const llwchar next = in[i + 1];
                            if (next == 0xFF9E && kana == 0x3046)
                            {
                                kana = 0x3094;      // ｳﾞ
                                ++i;
                            }
                            else if (next == 0xFF9E && takesVoicedMark(kana))
                            {
                                kana += 1;
                                ++i;
                            }
                            else if (next == 0xFF9F && isHaGyou(kana))
                            {
                                kana += 2;
                                ++i;
                            }
                        }
                        c = kana;
                    }
                    else if ((c >= 0x30A1 && c <= 0x30F6) || c == 0x30FD || c == 0x30FE)
                    {
                        c -= 0x60;                  // katakana -> hiragana
                    }
                }
                if (!case_sensitive)
                {
                    c = lowerOf(c);
                }
                out.push_back(c);
            }
            return out;
        }

        struct Word
        {
            LLWString   folded;
            std::string shown;      // as typed
            bool        leadWord;   // the first character is a letter or digit: a whole word starts after a non-letter
            bool        trailWord;
        };

        struct Matcher
        {
            std::vector<Word> words;
            bool caseSensitive = false;
            bool wholeWords    = false;
            bool foldWidth     = true;
        };

        void addWord(Matcher& m, const LLWString& raw)
        {
            size_t b = 0;
            size_t e = raw.size();
            while (b < e && isSpace(raw[b])) ++b;
            while (e > b && isSpace(raw[e - 1])) --e;
            if (b == e)
            {
                return;
            }
            const LLWString word = raw.substr(b, e - b);
            Word w;
            w.folded    = fold(word, m.caseSensitive, m.foldWidth);
            w.shown     = wstring_to_utf8str(word);
            w.leadWord  = isWordChar(w.folded.front());
            w.trailWord = isWordChar(w.folded.back());
            m.words.push_back(std::move(w));
        }

        // The list: words separated by , ， 、 or a new line.
        void addList(Matcher& m, const std::string& list)
        {
            const LLWString all = utf8str_to_wstring(list);
            LLWString cur;
            for (llwchar c : all)
            {
                if (c == ',' || c == 0xFF0C || c == 0x3001 || c == '\n')
                {
                    addWord(m, cur);
                    cur.clear();
                }
                else
                {
                    cur.push_back(c);
                }
            }
            addWord(m, cur);
        }

        bool containsWord(const Matcher& m, const LLWString& text, const Word& w)
        {
            size_t pos = 0;
            while ((pos = text.find(w.folded, pos)) != LLWString::npos)
            {
                if (!m.wholeWords)
                {
                    return true;
                }
                const size_t end = pos + w.folded.size();
                const bool startOk = !w.leadWord || pos == 0 || !isWordChar(text[pos - 1]);
                const bool endOk   = !w.trailWord || end >= text.size() || !isWordChar(text[end]);
                if (startOk && endOk)
                {
                    return true;
                }
                ++pos;
            }
            return false;
        }

        // The first word of the matcher the text holds (the text is not folded yet).
        const Word* findWord(const Matcher& m, const std::string& text)
        {
            if (m.words.empty())
            {
                return nullptr;
            }
            const LLWString folded = fold(utf8str_to_wstring(text), m.caseSensitive, m.foldWidth);
            for (const Word& w : m.words)
            {
                if (containsWord(m, folded, w))
                {
                    return &w;
                }
            }
            return nullptr;
        }

        // ---- the live matcher, from the settings ----

        Matcher     sMatcher;
        bool        sDirty = true;
        bool        sSignalsConnected = false;
        bool        sOwnNameKnown = false;
        std::vector<std::string> sOwnNames;
        unsigned    sGeneration = 0; // of sMatcher (listGeneration())

        void markDirty()
        {
            sDirty = true;
        }

        void connectSignals()
        {
            if (sSignalsConnected)
            {
                return;
            }
            sSignalsConnected = true;
            for (const char* name : { SET_LIST, SET_OWN_NAME, SET_CASE, SET_WHOLE, SET_FOLD })
            {
                if (LLControlVariable* control = gSavedPerAccountSettings.getControl(name))
                {
                    control->getSignal()->connect([](LLControlVariable*, const LLSD&, const LLSD&) { markDirty(); });
                }
                else
                {
                    LL_WARNS("Keywords") << name << " is not declared (settings_plus.xml)" << LL_ENDL;
                }
            }
        }

        // Your names, once the name cache has them (asked for each time until it does).
        void learnOwnNames()
        {
            if (sOwnNameKnown)
            {
                return;
            }
            LLAvatarName av_name;
            if (!LLAvatarNameCache::get(gAgentID, &av_name))
            {
                return;
            }
            sOwnNameKnown = true;
            sOwnNames.clear();
            const std::string display = av_name.getDisplayName();
            std::string user = av_name.getUserName();                   // "first.last" or "First Last"
            std::string first = av_name.getLegacyName().substr(0, av_name.getLegacyName().find(' '));
            for (std::string name : { display, user, first })
            {
                // a one-letter name would be in every line
                if (utf8str_to_wstring(name).size() >= 2 && std::find(sOwnNames.begin(), sOwnNames.end(), name) == sOwnNames.end())
                {
                    sOwnNames.push_back(name);
                }
            }
            markDirty();
        }

        void refresh()
        {
            connectSignals();
            if (!sDirty && sOwnNameKnown)
            {
                return; // the settings did not change (their signals mark it) and your names are learnt
            }
            const bool own = gSavedPerAccountSettings.getBOOL(SET_OWN_NAME);
            if (own)
            {
                learnOwnNames();
            }
            if (!sDirty)
            {
                return;
            }
            sDirty = false;
            Matcher m;
            m.caseSensitive = gSavedPerAccountSettings.getBOOL(SET_CASE);
            m.wholeWords    = gSavedPerAccountSettings.getBOOL(SET_WHOLE);
            m.foldWidth     = gSavedPerAccountSettings.getBOOL(SET_FOLD);
            addList(m, gSavedPerAccountSettings.getString(SET_LIST));
            if (own)
            {
                for (const std::string& name : sOwnNames)
                {
                    addWord(m, utf8str_to_wstring(name));
                }
            }
            sMatcher = std::move(m);
            ++sGeneration;
        }

        // Does the line hold a keyword (and is it one we look at)?
        const Word* keywordOf(const LLChat& chat, bool is_local)
        {
            if (!gSavedPerAccountSettings.getBOOL(SET_ON)
                || (is_local && !gSavedPerAccountSettings.getBOOL(SET_IN_CHAT))
                || (!is_local && !gSavedPerAccountSettings.getBOOL(SET_IN_IM)))
            {
                return nullptr;
            }
            // not what we said ourselves, nor what our own objects say, nor the viewer's notices
            if (chat.mFromID == gAgentID || (chat.mOwnerID.notNull() && chat.mOwnerID == gAgentID)
                || chat.mFromName == SYSTEM_FROM || chat.mSourceType == CHAT_SOURCE_SYSTEM)
            {
                return nullptr;
            }
            refresh();
            if (sMatcher.words.empty())
            {
                return nullptr;
            }
            if (gSavedPerAccountSettings.getBOOL(SET_SPEAKER))
            {
                return findWord(sMatcher, chat.mFromName + " " + chat.mText);
            }
            return findWord(sMatcher, chat.mText);
        }

        void notify(const LLChat& chat, const Word& word)
        {
            // Blocked for text (one blocked for voice only still writes to us); and nothing while
            // Do Not Disturb is on, neither the sound nor the notice (the line itself is kept).
            if (chat.mMuted || LLMuteList::getInstance()->isMuted(chat.mFromID, LLMute::flagTextChat)
                || gAgent.isDoNotDisturb())
            {
                return;
            }
            if (gSavedPerAccountSettings.getBOOL(SET_SOUND_ON))
            {
                static F64 sLastSound = -1.0;
                const F64 now = LLTimer::getElapsedSeconds();
                const LLUUID sound(gSavedPerAccountSettings.getString(SET_SOUND));
                if (sound.notNull() && (sLastSound < 0.0 || now - sLastSound > 0.5) && LLUI::getInstance()->mAudioCallback)
                {
                    sLastSound = now;
                    LLUI::getInstance()->mAudioCallback(sound);
                }
            }
            if (gSavedPerAccountSettings.getBOOL(SET_TOAST))
            {
                // The same word from the same speaker again within a few seconds: no new notice
                // (someone repeating it, or a script saying it in a loop, would fill the screen).
                constexpr F64 TOAST_REPEAT_SECONDS = 10.0;
                static std::map<std::string, F64> sLastToast; // word + speaker -> when
                const F64 now = LLTimer::getElapsedSeconds();
                const std::string key = word.shown + "\n" + (chat.mFromID.notNull() ? chat.mFromID.asString() : chat.mFromName);
                const auto last = sLastToast.find(key);
                if (last != sLastToast.end() && now - last->second < TOAST_REPEAT_SECONDS)
                {
                    return;
                }
                if (sLastToast.size() >= 64)
                {
                    for (auto it = sLastToast.begin(); it != sLastToast.end();)
                    {
                        it = now - it->second >= TOAST_REPEAT_SECONDS ? sLastToast.erase(it) : std::next(it);
                    }
                }
                sLastToast[key] = now;

                // The speaker's name and words are someone else's: no links of theirs in the
                // tip (its text box trusts its text, the chat history does not).
                // RLVa: the local chat is seen here before its text is filtered for the chat window
                // (LLNearbyChatHandler), so the copies in the tip hide names and places (@shownames, @showloc) themselves.
                std::string from_name = chat.mFromName;
                std::string text = chat.mText;
                if (plus::rlv::enabled())
                {
                    // the whole text first: a name cut by the truncation would not be found
                    plus::rlv::filterTextForPlus(from_name);
                    plus::rlv::filterTextForPlus(text);
                }
                LLStringUtil::format_map_t args;
                args["[NAME]"] = plus::safeText(from_name);
                args["[TEXT]"] = plus::safeText(utf8str_truncate(text, 160));
                args["[WORD]"] = plus::safeText(word.shown);
                std::string message = LLTrans::getString("PlusKeywordsAlert");
                LLStringUtil::format(message, args);
                LLNotificationsUtil::add("SystemMessageTip", LLSD().with("MESSAGE", message));
            }
        }
    }

    void onLocalChat(const LLChat& chat)
    {
        if (const Word* word = keywordOf(chat, true))
        {
            notify(chat, *word);
        }
    }

    void onInstantMessage(const LLUUID& session_id, const LLUUID& from_id, const std::string& from_name,
                          const std::string& text)
    {
        LLChat chat(text);
        chat.mFromID    = from_id;
        chat.mFromName  = from_name;
        chat.mSessionID = session_id;
        if (const Word* word = keywordOf(chat, false))
        {
            notify(chat, *word);
        }
    }

    bool colorFor(const LLChat& chat, LLUIColor& color)
    {
        if (!gSavedPerAccountSettings.getBOOL(SET_COLOR_ON) || chat.mMuted)
        {
            return false;
        }
        if (!keywordOf(chat, chat.mSessionID.isNull()))
        {
            return false;
        }
        color = LLUIColor(gSavedPerAccountSettings.getColor4(SET_COLOR));
        return true;
    }

    bool colorNameFor(const LLChat& chat, std::string& color_name)
    {
        LLUIColor color;
        if (!colorFor(chat, color))
        {
            return false;
        }
        static LLColor4 sLast(-1.f, -1.f, -1.f, -1.f);
        const LLColor4 now = color.get();
        if (now != sLast)
        {
            sLast = now;
            LLUIColorTable::instance().setColor(COLOR_NAME, now);
        }
        color_name = COLOR_NAME;
        return true;
    }

    bool testText(const std::string& text, std::string& word)
    {
        refresh();
        if (const Word* w = findWord(sMatcher, text))
        {
            word = w->shown;
            return true;
        }
        return false;
    }

    unsigned listGeneration()
    {
        refresh();
        return sGeneration;
    }
}

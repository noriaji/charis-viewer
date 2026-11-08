/**
 * @file langdetect.h
 * @brief Translation (plus/translate, research/228): which language is a chat line written in?
 *
 * The official chat translation hands its text to a service that finds the language out itself. The local models
 * translate one fixed direction each, so the language has to be known first, and a wrong guess is worse than none (a
 * Spanish line given to the English-to-Japanese model comes out as nonsense). So this answers only when it is fairly
 * sure, and answers "" (unknown: do not translate) otherwise.
 *
 *   by script:  kana (and the kanji around it) -> "ja"; hangul -> "ko"; kanji alone -> "zh"; Cyrillic "ru"; Arabic "ar";
 *               Thai "th". Only "ja", "ko" and "en" are languages the local models can read.
 *   Latin text: "en" when enough of its words are common English words and nothing points to another language
 *               (accented letters, common words of Spanish, French, German, Portuguese and Italian); otherwise "".
 *
 * Standard C++ only (tested on its own in translate_test.cpp).
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

#ifndef PLUS_TRANSLATE_LANGDETECT_H
#define PLUS_TRANSLATE_LANGDETECT_H

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace plus
{
    namespace translate
    {
        namespace langdetect_detail
        {
            // The next code point of a UTF-8 text (a bad byte counts as one code point of its own).
            inline uint32_t nextCodePoint(const std::string& s, size_t& i)
            {
                const unsigned char c = static_cast<unsigned char>(s[i]);
                size_t n = 1;
                uint32_t cp = c;
                if (c >= 0xF0 && c < 0xF8)
                {
                    n = 4;
                    cp = c & 0x07;
                }
                else if (c >= 0xE0)
                {
                    n = 3;
                    cp = c & 0x0F;
                }
                else if (c >= 0xC0)
                {
                    n = 2;
                    cp = c & 0x1F;
                }
                if (i + n > s.size())
                {
                    ++i;
                    return c;
                }
                for (size_t k = 1; k < n; ++k)
                {
                    cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
                }
                i += n;
                return cp;
            }

            inline bool isKana(uint32_t c)
            {
                return (c >= 0x3041 && c <= 0x30FF) || (c >= 0x31F0 && c <= 0x31FF) || (c >= 0xFF66 && c <= 0xFF9F);
            }
            inline bool isHan(uint32_t c)
            {
                return (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF) || (c >= 0xF900 && c <= 0xFAFF);
            }
            inline bool isHangul(uint32_t c)
            {
                return (c >= 0xAC00 && c <= 0xD7AF) || (c >= 0x1100 && c <= 0x11FF) || (c >= 0x3130 && c <= 0x318F);
            }
            inline bool isCyrillic(uint32_t c)
            {
                return c >= 0x0400 && c <= 0x04FF;
            }
            inline bool isArabic(uint32_t c)
            {
                return c >= 0x0600 && c <= 0x06FF;
            }
            inline bool isThai(uint32_t c)
            {
                return c >= 0x0E00 && c <= 0x0E7F;
            }
            inline bool isAsciiLetter(uint32_t c)
            {
                return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
            }
            // Latin letters beyond ASCII: the accented letters of the European languages.
            inline bool isLatinAccented(uint32_t c)
            {
                return (c >= 0x00C0 && c <= 0x024F && c != 0x00D7 && c != 0x00F7);
            }

            inline const std::set<std::string>& englishWords()
            {
                static const std::set<std::string> sWords = {
                    "the", "and", "you", "your", "are", "is", "to", "of", "in", "it", "that", "for", "my", "me", "we", "this", "with",
                    "have", "has", "was", "be", "not", "do", "don't", "can", "can't", "will", "what", "who", "where", "when", "why",
                    "how", "i", "i'm", "i'll", "i've", "i'd", "a", "an", "on", "at", "so", "but", "or", "if", "they", "he", "she",
                    "his", "her", "our", "their", "just", "like", "love", "good", "nice", "thanks", "thank", "please", "sorry", "hello",
                    "hi", "hey", "yes", "no", "yeah", "ok", "okay", "lol", "wow", "oh", "here", "there", "now", "then", "got", "get",
                    "go", "going", "come", "know", "see", "let's", "let", "want", "need", "think", "time", "today", "tonight", "again",
                    "really", "very", "too", "all", "one", "up", "out", "from", "about", "been", "were", "would", "could", "should",
                    "its", "it's", "that's", "there's", "what's", "you're", "we're", "they're", "he's", "she's", "isn't", "aren't",
                    "wasn't", "didn't", "doesn't", "won't", "wanna", "gonna", "thx", "pls", "plz", "brb", "omg", "afk", "np", "gl",
                    "bye", "welcome", "friend", "friends", "club", "sim", "land", "event", "free", "new", "help", "help?", "much",
                    "some", "any", "more", "when's", "also", "after", "before", "because", "name", "home", "back", "over", "off",
                    "problem", "store", "sleep", "lot", "fun", "cute", "um", "uh", "hmm", "yep", "nope", "sure", "cool", "great",
                    "awesome", "dress", "outfit", "hair", "skin", "shape", "avatar", "lag", "laggy", "tp", "teleport", "group",
                    "chat", "voice", "mic", "ty", "gtg", "irl", "ready", "late", "soon", "wait", "look", "make", "take", "give",
                    "tell", "ask", "try", "work", "play", "live", "talk", "meet", "stay", "leave", "buy", "sell", "find", "show",
                    // forms of verbs and more common words (2026-11-02). Words other languages write too are left out ("am" and "still"
                    // are German, "even" is Dutch)
                    "did", "does", "doing", "done", "wants", "needs", "likes", "said", "says", "gave", "went", "made", "told", "feel",
                    "him", "them", "than", "these", "those", "which", "something", "anything", "nothing", "everything", "someone",
                    "everyone", "anyone", "people", "thing", "things", "big", "little", "right", "well", "only", "never", "always", "maybe"
                };
                return sWords;
            }

            // English words that other languages write too ("a", "no", "me", "on", ...): they say nothing about the language.
            inline const std::set<std::string>& ambiguousWords()
            {
                static const std::set<std::string> sWords = { "a", "no", "me", "on", "so", "in", "an", "or", "as", "do", "he", "go" };
                return sWords;
            }

            // Common words of the other languages written in Latin letters (a few each; the accents say much more).
            inline const std::set<std::string>& foreignWords()
            {
                static const std::set<std::string> sWords = {
                    // Spanish, Portuguese, Italian, French, German, Dutch, Indonesian
                    "el", "los", "las", "una", "por", "para", "con", "es", "se", "te", "mi", "tu", "que", "como", "pero", "muy", "hola",
                    "de", "la", "le", "en", "y", "un", "lo", "del", "al", "du", "da", "em", "ver", "hay", "va", "gusta", "bien", "si", "sin",
                    "gracias", "buenos", "dias", "est", "pas", "je", "tu", "nous", "vous", "les", "des", "une", "et", "bonjour", "merci",
                    "der", "die", "das", "und", "ist", "nicht", "ich", "du", "ein", "eine", "mit", "auf", "ja", "nein", "danke", "hallo",
                    "voce", "uma", "com", "os", "obrigado", "ola", "il", "di", "che", "per", "sono", "ciao", "grazie", "non", "bene",
                    "ik", "je", "niet", "het", "een", "dat", "dank", "aku", "kamu", "tidak", "terima", "kasih", "selamat", "yang", "di"
                };
                return sWords;
            }

            inline std::string lower(std::string s)
            {
                for (char& c : s)
                {
                    if (c >= 'A' && c <= 'Z')
                    {
                        c = char(c - 'A' + 'a');
                    }
                }
                return s;
            }

            // Words of a Latin text: runs of ASCII letters, with the apostrophes of English contractions kept inside.
            inline std::vector<std::string> latinWords(const std::string& text)
            {
                std::vector<std::string> words;
                std::string cur;
                auto flush = [&] {
                    while (!cur.empty() && cur.back() == '\'')
                    {
                        cur.pop_back();
                    }
                    if (!cur.empty())
                    {
                        words.push_back(lower(cur));
                    }
                    cur.clear();
                };
                for (size_t i = 0; i < text.size();)
                {
                    const uint32_t c = nextCodePoint(text, i);
                    if (isAsciiLetter(c) || (c == '\'' && !cur.empty()) || (c == 0x2019 && !cur.empty()))
                    {
                        cur += (c == 0x2019) ? '\'' : static_cast<char>(c);
                    }
                    else if (isLatinAccented(c))
                    {
                        // an accented letter inside a word: the word is not English; keep it as a marker (see detectLanguage)
                        cur += '~';
                    }
                    else
                    {
                        flush();
                    }
                }
                flush();
                return words;
            }
        }

        // The language of a text: "ja", "ko", "en", "zh", "ru", "ar", "th", or "" when it is not known or there is nothing to
        // translate (digits, symbols, emoji only).
        inline std::string detectLanguage(const std::string& text)
        {
            using namespace langdetect_detail;
            size_t kana = 0, han = 0, hangul = 0, cyrillic = 0, arabic = 0, thai = 0, ascii = 0, accented = 0;
            for (size_t i = 0; i < text.size();)
            {
                const uint32_t c = nextCodePoint(text, i);
                if (isKana(c)) ++kana;
                else if (isHan(c)) ++han;
                else if (isHangul(c)) ++hangul;
                else if (isCyrillic(c)) ++cyrillic;
                else if (isArabic(c)) ++arabic;
                else if (isThai(c)) ++thai;
                else if (isAsciiLetter(c)) ++ascii;
                else if (isLatinAccented(c)) ++accented;
            }
            const size_t letters = kana + han + hangul + cyrillic + arabic + thai + ascii + accented;
            if (letters == 0)
            {
                return std::string();
            }
            auto share = [&](size_t n) { return n * 2 >= letters; };   // at least half of the letters
            if (kana > 0 && share(kana + han))
            {
                return "ja";
            }
            if (share(hangul))
            {
                return "ko";
            }
            if (share(han))
            {
                return "zh";   // kanji alone: Chinese, or a Japanese word too short to tell; not translated either way
            }
            if (share(cyrillic))
            {
                return "ru";
            }
            if (share(arabic))
            {
                return "ar";
            }
            if (share(thai))
            {
                return "th";
            }
            if (!share(ascii + accented))
            {
                return std::string();   // a mix of scripts with no clear lead
            }

            // Latin letters: English only when sure.
            const std::vector<std::string> words = latinWords(text);
            if (words.empty())
            {
                return std::string();
            }
            // strong: words that are English; weak: words English shares with other languages ("a", "no", "me"...); foreign: words of other
            // languages and words with an accent
            size_t strong = 0, weak = 0, foreign = 0, accentedWords = 0;
            for (const std::string& w : words)
            {
                if (w.find('~') != std::string::npos)
                {
                    ++accentedWords;
                }
                else if (ambiguousWords().count(w))
                {
                    ++weak;
                }
                else if (englishWords().count(w))
                {
                    ++strong;
                }
                else if (foreignWords().count(w))
                {
                    ++foreign;
                }
            }
            // accents and foreign words weigh against English (an English line seldom has them; a Spanish line almost always does)
            const size_t against = foreign + accentedWords * 3;
            if (against > 0 && against >= strong)
            {
                return std::string();
            }
            if (strong == 0)
            {
                return std::string();   // no word we know as English: a name, a string of letters, another language, a lone "no"
            }
            // The shared words are English too when nothing points to another language ("go to the store"); when something does, they
            // say nothing: they are left out of the count and of the words it is divided by.
            const size_t english = strong + (against == 0 ? weak : 0);
            const size_t counted = words.size() - (against == 0 ? 0 : weak);
            // enough of the words are English: a third (or, for a very short line, any)
            if (counted <= 3 || english * 3 >= counted)
            {
                return "en";
            }
            return std::string();
        }
    }
}

#endif // PLUS_TRANSLATE_LANGDETECT_H

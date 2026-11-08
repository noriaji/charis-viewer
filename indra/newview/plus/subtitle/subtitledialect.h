/**
 * @file subtitledialect.h
 * @brief Subtitles (plus/subtitle, research/222): Japanese translations written in a dialect.
 *
 * A translation into Japanese can be rewritten in a dialect (Kansai, Hakata, Tohoku), as a game: a table of replacements,
 * mostly of word endings and a few common words, applied to the translated text only (never to what was spoken). No
 * morphological analysis: at each place the longest rule that fits is taken. A rule marked "atEnd" applies only where a
 * sentence or clause ends (the end of the text, a stop, a comma, a space, a question or exclamation mark), so that
 * "desu" is turned into "ya" in "...desu." but not in "...desuga". A rule whose answer is its own text protects a word
 * from a shorter rule (the "dou" of "douzo"). Standard C++ only (tested on its own in subtitle_test.cpp).
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

#ifndef PLUS_SUBTITLE_DIALECT_H
#define PLUS_SUBTITLE_DIALECT_H

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace plus
{
    namespace subtitle
    {
        struct DialectRule
        {
            const char* from;
            const char* to;
            bool atEnd;   // only where a sentence or clause ends
            // The rule is not taken when the character just before "from" is one of these (UTF-8, whole
            // characters), or the character just after it: the word "from" is only the end or the start of.
            const char* notAfter  = nullptr;
            const char* notBefore = nullptr;
        };

        namespace dialect_detail
        {
            // What may follow a word that ends a clause.
            inline bool endsHere(const std::string& s, size_t i)
            {
                if (i >= s.size())
                {
                    return true;
                }
                static const char* const sStops[] = { " ", "\n", ".", ",", "!", "?", "~", "。", "、", "！", "？", "…", "♪", "」", "）", "　" };
                for (const char* stop : sStops)
                {
                    if (s.compare(i, std::strlen(stop), stop) == 0)
                    {
                        return true;
                    }
                }
                return false;
            }

            // The character (its UTF-8 bytes) that ends just before i, or that starts at i; "" at the ends.
            inline std::string charBefore(const std::string& s, size_t i)
            {
                if (i == 0 || i > s.size())
                {
                    return std::string();
                }
                size_t start = i - 1;
                while (start > 0 && (static_cast<unsigned char>(s[start]) & 0xC0) == 0x80)
                {
                    --start;
                }
                return s.substr(start, i - start);
            }

            inline std::string charAt(const std::string& s, size_t i)
            {
                if (i >= s.size())
                {
                    return std::string();
                }
                size_t end = i + 1;
                while (end < s.size() && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80)
                {
                    ++end;
                }
                return s.substr(i, end - i);
            }

            // Whether c (one whole character) is in the list of characters. UTF-8 keeps a whole character
            // from matching in the middle of another, so a plain search is enough.
            inline bool oneOf(const char* list, const std::string& c)
            {
                return list && !c.empty() && std::strstr(list, c.c_str()) != nullptr;
            }

            // The context the rule asks for, around text[i, i + len).
            inline bool fits(const DialectRule& r, const std::string& text, size_t i, size_t len)
            {
                if (r.atEnd && !endsHere(text, i + len))
                {
                    return false;
                }
                if (oneOf(r.notAfter, charBefore(text, i)))
                {
                    return false;
                }
                if (oneOf(r.notBefore, charAt(text, i + len)))
                {
                    return false;
                }
                return true;
            }

            // "だ" that ends a past tense or another word ("読んだ", "急いだ"), not the copula.
            constexpr const char* NOT_COPULA_AFTER = "んい";

            // The "…んだ" that explains ("行くんだ", "言ったんだ") against the past of a verb in む / ぶ / ぬ written in
            // kana, which ends the same: its "…んだ" rule is not taken after these (the "だ" after "ん" is then left as it
            // is). A particle or a stop before it: the kana is the verb itself ("腕をくんだ", "子をうんだ", "つんだ").
            // Kana alone cannot always tell the two apart; where both are words the rule is left out, the safe side (a
            // sentence left in the standard form, never one turned into a wrong dialect): "すくんだ" (竦んだ / 空くんだ),
            // "ふくんだ" (含んだ / 吹くんだ), "つぐんだ" (噤んだ / 注ぐんだ), "くるんだ" (包んだ / 来るんだ), "ぬるんだ"
            // (温んだ / 塗るんだ). Kanji before the kana tells them apart ("来るんだ", "塗るんだ" still change).
            constexpr const char* WORD_START       = "をがはもにでとへやの、。！？「（ 　";
            // Some characters and WORD_START (made once; the rule tables hold the pointer)
            inline const char* NOT_KUNDA_AFTER()  { static const std::string s = std::string("りぐすふ") + WORD_START; return s.c_str(); } // 取りくんだ, はぐくんだ, すくんだ, ふくんだ; くむ: 組んだ
            constexpr const char* NOT_GUNDA_AFTER  = "涙めつ";                                // 涙ぐんだ, めぐんだ, つぐんだ
            constexpr const char* NOT_RUNDA_AFTER  = "くゆぬ";                                // くるんだ, ゆるんだ, ぬるんだ
            constexpr const char* NOT_UNDA_AFTER   = WORD_START;                              // うむ: 産んだ
            inline const char* NOT_TSUNDA_AFTER() { static const std::string s = std::string("つ") + WORD_START; return s.c_str(); } // つつんだ; つむ: 積んだ
            constexpr const char* NOT_TANDA_AFTER  = "た";                                    // たたんだ
            // "まだ" (not yet) is left as it is, but not the end of "そのままだ", "ひまだ", "じゃまだ", "いまだ", "さまだ".
            constexpr const char* NOT_MADA_AFTER = "まひゃいさ";

            // "いい" of "言います" written in kana ("そういいます"); "いいわけ" has a rule of its own.
            constexpr const char* NOT_GOOD_BEFORE = "ま";

            // The rules, longest "from" first: at one place the first that fits is taken.
            inline std::vector<DialectRule> sorted(std::vector<DialectRule> rules)
            {
                std::stable_sort(rules.begin(), rules.end(), [](const DialectRule& a, const DialectRule& b) {
                    return std::strlen(a.from) > std::strlen(b.from);
                });
                return rules;
            }

            inline const std::vector<DialectRule>& kansai()
            {
                static const std::vector<DialectRule> sRules = sorted({
                    { "ありがとうございます", "おおきに", false }, { "ありがとうございました", "おおきに", false }, { "ありがとう", "おおきに", false },
                    { "ごめんなさい", "かんにんな", false }, { "すみません", "すんません", false },
                    { "さようなら", "ほなな", false }, { "おはよう", "おはようさん", false },
                    { "本当に", "ほんまに", false }, { "本当", "ほんま", false }, { "とても", "めっちゃ", false },
                    { "非常に", "めっちゃ", false }, { "すごく", "めっちゃ", false }, { "たくさん", "ぎょうさん", false },
                    { "だから", "せやから", false }, { "それでは", "ほな", false },
                    { "そうですね", "そうやな", false }, { "そうですよ", "そうやで", false }, { "そうですか", "そうなん", false },
                    { "そうです", "そうや", false },
                    { "そうだね", "そうやな", false }, { "そうだ", "そうや", true },
                    { "ではありません", "ちゃいます", false }, { "じゃありません", "ちゃいます", false },
                    { "ではない", "やない", false }, { "じゃない", "やない", false },
                    { "違います", "ちゃいます", false }, { "違う", "ちゃう", false }, { "いいえ", "ちゃうちゃう", false },
                    { "いいですね", "ええなぁ", false }, { "いいね", "ええなぁ", false },
                    { "いいわけ", "いいわけ", false },
                    { "いい", "ええ", false, nullptr, NOT_GOOD_BEFORE },
                    { "良い", "ええ", false }, { "よい", "ええ", false },
                    { "面白い", "おもろい", false }, { "おもしろい", "おもろい", false },
                    { "どうして", "なんで", false }, { "なぜ", "なんで", false }, { "どうですか", "どないですか", false },
                    { "どうぞ", "どうぞ", false }, { "どうも", "どうも", false },
                    { "あなた", "あんた", false },
                    { "わからない", "わからへん", true }, { "分からない", "分からへん", true }, { "知らない", "知らん", true },
                    { "できない", "できひん", true }, { "行かない", "行かへん", true }, { "来ない", "けえへん", true },
                    { "しない", "せえへん", true },
                    { "ですよね", "やんな", false }, { "ですよ", "やで", false }, { "ですね", "やな", false },
                    { "ですか", "ですか", false }, { "でしょう", "やろ", false }, { "でした", "やった", false }, { "です", "や", true },
                    { "だろう", "やろ", false }, { "だよね", "やんな", false }, { "だよ", "やで", false }, { "だね", "やな", false },
                    { "だった", "やった", false }, { "なんだ", "なんや", true }, { "まだ", "まだ", false, NOT_MADA_AFTER },
                    // the "だ" after "い" / "ん" that is the copula after all (a na-adjective, "…んだ" explaining)
                    { "みたいだ", "みたいや", true }, { "嫌いだ", "嫌いや", true }, { "きらいだ", "きらいや", true }, { "きれいだ", "きれいや", true },
                    { "くんだ", "くんや", true, NOT_KUNDA_AFTER() }, { "ぐんだ", "ぐんや", true, NOT_GUNDA_AFTER },
                    { "るんだ", "るんや", true, NOT_RUNDA_AFTER }, { "うんだ", "うんや", true, NOT_UNDA_AFTER },
                    { "つんだ", "つんや", true, NOT_TSUNDA_AFTER() }, { "たんだ", "たんや", true, NOT_TANDA_AFTER },
                    { "だ", "や", true, NOT_COPULA_AFTER },
                    { "しています", "してます", false }, { "している", "してる", false }, { "していた", "してた", false },
                });
                return sRules;
            }

            inline const std::vector<DialectRule>& hakata()
            {
                static const std::vector<DialectRule> sRules = sorted({
                    { "本当に", "ほんなこつ", false }, { "とても", "ばり", false }, { "すごく", "ばり", false },
                    { "すごい", "すごか", false }, { "かわいい", "かわいか", false }, { "おいしい", "うまか", false },
                    { "いいですね", "よかね", false }, { "いいえ", "いいえ", false },
                    { "いいわけ", "いいわけ", false },
                    { "いい", "よか", false, nullptr, NOT_GOOD_BEFORE }, { "良い", "よか", false },
                    { "だから", "やけん", false }, { "ですから", "やけん", false },
                    { "どうして", "なして", false }, { "なぜ", "なして", false }, { "どうですか", "どげんね", false },
                    { "どうぞ", "どうぞ", false }, { "どうも", "どうも", false },
                    { "あなた", "あんた", false },
                    { "わからない", "わからん", true }, { "分からない", "分からん", true }, { "知らない", "知らん", true },
                    { "できない", "できん", true }, { "行かない", "行かん", true }, { "いない", "おらん", true }, { "しない", "せん", true },
                    { "好きです", "好いとうよ", false }, { "好き", "好いとう", true },
                    { "そうですね", "そうたいね", false }, { "そうだね", "そうたいね", false }, { "そうですか", "そうと", false },
                    { "そうです", "そうたい", false },
                    { "そうだ", "そうたい", true },
                    { "ですよね", "ばいね", false }, { "ですよ", "ばい", false }, { "ですね", "ばいね", false }, { "ですか", "と", false },
                    { "でしょう", "やろ", false }, { "でした", "やった", false }, { "です", "ばい", true },
                    { "だろう", "やろ", false }, { "だよ", "たい", false }, { "だね", "やね", false }, { "だった", "やった", false },
                    { "まだ", "まだ", false, NOT_MADA_AFTER },
                    { "みたいだ", "みたいたい", true }, { "嫌いだ", "嫌いたい", true }, { "きらいだ", "きらいたい", true }, { "きれいだ", "きれいたい", true },
                    { "くんだ", "くんたい", true, NOT_KUNDA_AFTER() }, { "ぐんだ", "ぐんたい", true, NOT_GUNDA_AFTER },
                    { "るんだ", "るんたい", true, NOT_RUNDA_AFTER }, { "うんだ", "うんたい", true, NOT_UNDA_AFTER },
                    { "つんだ", "つんたい", true, NOT_TSUNDA_AFTER() }, { "たんだ", "たんたい", true, NOT_TANDA_AFTER },
                    { "だ", "たい", true, NOT_COPULA_AFTER },
                });
                return sRules;
            }

            inline const std::vector<DialectRule>& tohoku()
            {
                static const std::vector<DialectRule> sRules = sorted({
                    { "ありがとうございます", "ありがとなっす", false }, { "ありがとうございました", "ありがとなっした", false },
                    { "ありがとう", "ありがとなぁ", false },
                    { "とても", "すんげえ", false }, { "すごく", "すんげえ", false }, { "非常に", "すんげえ", false },
                    { "すごい", "すんげえ", false }, { "かわいい", "めんこい", false }, { "可愛い", "めんこい", false },
                    { "たくさん", "いっぺぇ", false },
                    // not the "私" of "私立", "私的", … nor the "僕" of "公僕", "下僕"
                    { "私", "おら", false, nullptr, "立的用服物有設語" }, { "わたし", "おら", false },
                    { "僕", "おら", false, "公下", nullptr }, { "俺", "おら", false },
                    { "あなた", "おめえ", false },
                    { "どうして", "なして", false }, { "なぜ", "なして", false }, { "どうぞ", "どうぞ", false }, { "どうも", "どうも", false },
                    { "だから", "んだがら", false }, { "それでは", "んだば", false }, { "さようなら", "んだば", false },
                    { "わからない", "わがんねえ", true }, { "分からない", "分がんねえ", true }, { "知らない", "知らねえ", true },
                    { "できない", "でぎねえ", true }, { "行かない", "行がねえ", true }, { "いない", "いねえ", true }, { "しない", "しねえ", true },
                    { "そうですね", "んだなぁ", false }, { "そうですよ", "んだべ", false }, { "そうですか", "んだが", false },
                    { "そうです", "んだ", false },
                    { "そうだね", "んだんだ", false }, { "そうだ", "んだ", true },
                    { "ですよね", "だべな", false }, { "ですよ", "だべさ", false }, { "ですね", "だなぁ", false }, { "ですか", "だべか", false },
                    { "でしょう", "だべ", false }, { "でした", "だった", false }, { "です", "だべ", true },
                    { "だろう", "だべ", false }, { "だよ", "だべさ", false }, { "だね", "だなぁ", false },
                    { "まだ", "まだ", false, NOT_MADA_AFTER },
                    { "みたいだ", "みたいだべ", true }, { "嫌いだ", "嫌いだべ", true }, { "きらいだ", "きらいだべ", true }, { "きれいだ", "きれいだべ", true },
                    { "くんだ", "くんだべ", true, NOT_KUNDA_AFTER() }, { "ぐんだ", "ぐんだべ", true, NOT_GUNDA_AFTER },
                    { "るんだ", "るんだべ", true, NOT_RUNDA_AFTER }, { "うんだ", "うんだべ", true, NOT_UNDA_AFTER },
                    { "つんだ", "つんだべ", true, NOT_TSUNDA_AFTER() }, { "たんだ", "たんだべ", true, NOT_TANDA_AFTER },
                    { "だ", "だべ", true, NOT_COPULA_AFTER },
                });
                return sRules;
            }

            inline const std::vector<DialectRule>* rulesFor(const std::string& id)
            {
                if (id == "kansai")
                {
                    return &kansai();
                }
                if (id == "hakata")
                {
                    return &hakata();
                }
                if (id == "tohoku")
                {
                    return &tohoku();
                }
                return nullptr;
            }
        }

        // The dialects that can be asked for ("kansai", "hakata", "tohoku"); "" is the standard speech.
        inline bool knownDialect(const std::string& id)
        {
            return dialect_detail::rulesFor(id) != nullptr;
        }

        // The text in the dialect; the text as it is for "" or a dialect that is not known.
        inline std::string applyDialect(const std::string& text, const std::string& id)
        {
            const std::vector<DialectRule>* rules = dialect_detail::rulesFor(id);
            if (!rules || text.empty())
            {
                return text;
            }
            std::string out;
            out.reserve(text.size() + 16);
            size_t i = 0;
            while (i < text.size())
            {
                const DialectRule* hit = nullptr;
                size_t hit_len = 0;
                // only the byte that starts a character can start a rule (a rule starts with a whole character)
                if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80)
                {
                    for (const DialectRule& r : *rules)
                    {
                        const size_t len = std::strlen(r.from);
                        if (text.compare(i, len, r.from) == 0 && dialect_detail::fits(r, text, i, len))
                        {
                            hit = &r;
                            hit_len = len;
                            break;
                        }
                    }
                }
                if (hit)
                {
                    out += hit->to;
                    i += hit_len;
                }
                else
                {
                    out += text[i++];
                }
            }
            return out;
        }
    }
}

#endif // PLUS_SUBTITLE_DIALECT_H

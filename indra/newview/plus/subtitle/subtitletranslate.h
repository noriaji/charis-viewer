/**
 * @file subtitletranslate.h
 * @brief Subtitles (plus/subtitle, research/137 section 7): when a sentence needs translating.
 *
 * No viewer dependencies: subtitle_test.cpp builds this on its own.
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

#ifndef PLUS_SUBTITLE_TRANSLATE_H
#define PLUS_SUBTITLE_TRANSLATE_H

#include <cctype>
#include <string>

namespace plus
{
    namespace subtitle
    {
        // "zh-CN", "zh_TW", "ZH" -> "zh"
        inline std::string primaryLanguage(const std::string& code)
        {
            std::string out;
            for (char c : code)
            {
                if (c == '-' || c == '_')
                {
                    break;
                }
                out += char(std::tolower((unsigned char)c));
            }
            return out;
        }

        // Is it worth sending a sentence that was spoken in `spoken` to a translation service that
        // writes `target`? Not when the language is unknown (the recognizer could not tell), when
        // either is empty, or when they are the same language (the regions of Chinese and so on do not
        // count as different here: the service decides what to do with them).
        inline bool needsTranslation(const std::string& spoken, const std::string& target)
        {
            const std::string a = primaryLanguage(spoken);
            const std::string b = primaryLanguage(target);
            return !a.empty() && !b.empty() && a != b;
        }

        // The directions the local models cover (research/222, 228): English to Japanese, Chinese, Spanish, French, German, Russian,
        // Italian and Portuguese; Japanese to English; and, with the one multilingual model, Japanese and Korean both ways and
        // English with Korean both ways. The codes are primaryLanguage() of what was spoken or written and of the target.
        // (The models are listed in translatefetch.cpp; the two lists go together.)
        inline bool localPairSupported(const std::string& spoken, const std::string& target)
        {
            const std::string a = primaryLanguage(spoken);
            const std::string b = primaryLanguage(target);
            if (a == "ko")
            {
                return b == "ja" || b == "en";
            }
            if (a == "ja")
            {
                return b == "en" || b == "ko";
            }
            if (a != "en")
            {
                return false;
            }
            static const char* const sTargets[] = { "ja", "zh", "es", "fr", "de", "ru", "it", "pt", "ko" };
            for (const char* t : sTargets)
            {
                if (b == t)
                {
                    return true;
                }
            }
            return false;
        }
    }
}

#endif // PLUS_SUBTITLE_TRANSLATE_H

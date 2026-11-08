/**
 * @file textsplit.h
 * @brief Translation (plus/translate, research/228): cut a chat line into what is translated and what is left as it is.
 *
 * A chat line holds things that must not be translated: addresses (http://, https://, secondlife://, www.), the "/me"
 * of an action, and the spaces between pieces. The official Azure service wraps links in "no-translate" marks; the local
 * models have no such marks, so the line is cut instead: the protected parts are kept as they are, the rest is translated
 * piece by piece, and the answers are put back in order. (Avatar names are inside secondlife:// links, so they are
 * protected too.)
 *
 * An address ends at the first white space or the first non-ASCII byte (Japanese text can follow one directly), and
 * stops before a closing punctuation mark.
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

#ifndef PLUS_TRANSLATE_TEXTSPLIT_H
#define PLUS_TRANSLATE_TEXTSPLIT_H

#include <cstring>
#include <string>
#include <vector>

namespace plus
{
    namespace translate
    {
        struct TextPiece
        {
            std::string text;
            bool translate = false;   // false: put back as it is
        };

        namespace textsplit_detail
        {
            inline bool isSpace(char c)
            {
                return c == ' ' || c == '\t' || c == '\n' || c == '\r';
            }

            // Has a letter, a digit or any non-ASCII character: something worth translating (not only marks and spaces).
            inline bool hasContent(const std::string& s)
            {
                for (const unsigned char c : s)
                {
                    if (c >= 0x80 || (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
                    {
                        return true;
                    }
                }
                return false;
            }

            // Does an address start here? (at a word start: the start of the text, or after a space or an opening mark)
            inline size_t addressPrefixLength(const std::string& s, size_t i)
            {
                static const char* const sPrefixes[] = { "https://", "http://", "secondlife://", "www." };
                if (i > 0)
                {
                    const char before = s[i - 1];
                    // a start of a word; text in Japanese (a byte of a multi-byte character) may run straight into an address
                    if (!isSpace(before) && before != '(' && before != '[' && before != '<' && before != '"' && before != '\'' &&
                        static_cast<unsigned char>(before) < 0x80)
                    {
                        return 0;
                    }
                }
                for (const char* p : sPrefixes)
                {
                    const size_t n = std::strlen(p);
                    // compared without regard to case ("HTTP://" is an address too)
                    bool same = s.size() >= i + n;
                    for (size_t k = 0; same && k < n; ++k)
                    {
                        char c = s[i + k];
                        if (c >= 'A' && c <= 'Z')
                        {
                            c = char(c - 'A' + 'a');
                        }
                        same = (c == p[k]);
                    }
                    if (same)
                    {
                        return n;
                    }
                }
                return 0;
            }

            // The end of an address that starts at `i`: before the first white space or non-ASCII byte, and before
            // closing marks that belong to the sentence, not the address.
            inline size_t addressEnd(const std::string& s, size_t i)
            {
                size_t e = i;
                while (e < s.size() && !isSpace(s[e]) && static_cast<unsigned char>(s[e]) < 0x80)
                {
                    ++e;
                }
                while (e > i)
                {
                    const char c = s[e - 1];
                    if (c == '.' || c == ',' || c == ';' || c == ':' || c == '!' || c == '?' || c == ')' || c == ']' || c == '>' || c == '"' || c == '\'')
                    {
                        --e;
                    }
                    else
                    {
                        break;
                    }
                }
                return e;
            }

            inline void push(std::vector<TextPiece>& out, const std::string& text, bool translate)
            {
                if (text.empty())
                {
                    return;
                }
                if (!out.empty() && out.back().translate == translate)
                {
                    out.back().text += text;   // neighbours of one kind are one piece
                    return;
                }
                out.push_back({ text, translate });
            }
        }

        // The line as a list of pieces; joining the pieces' texts gives the line back. A piece to translate has no white space
        // at its ends (the spaces are pieces of their own, put back as they are).
        inline std::vector<TextPiece> splitForTranslation(const std::string& line)
        {
            using namespace textsplit_detail;
            std::vector<TextPiece> raw;
            std::string cur;   // text waiting to be translated
            size_t i = 0;
            // an action: "/me" at the start stays as it is
            if (line.compare(0, 3, "/me") == 0 && (line.size() == 3 || isSpace(line[3])))
            {
                push(raw, "/me", false);
                i = 3;
            }
            while (i < line.size())
            {
                const size_t prefix = addressPrefixLength(line, i);
                if (prefix > 0)
                {
                    const size_t end = addressEnd(line, i);
                    if (end > i + prefix)
                    {
                        push(raw, cur, true);
                        cur.clear();
                        push(raw, line.substr(i, end - i), false);
                        i = end;
                        continue;
                    }
                    // "www." with nothing after it is no address: it stays in the text
                }
                cur += line[i++];
            }
            push(raw, cur, true);

            // move white space off the ends of the pieces to translate, and keep what has no content as it is
            std::vector<TextPiece> out;
            for (const TextPiece& p : raw)
            {
                if (!p.translate)
                {
                    push(out, p.text, false);
                    continue;
                }
                size_t a = 0, b = p.text.size();
                while (a < b && isSpace(p.text[a]))
                {
                    ++a;
                }
                while (b > a && isSpace(p.text[b - 1]))
                {
                    --b;
                }
                push(out, p.text.substr(0, a), false);
                const std::string core = p.text.substr(a, b - a);
                push(out, core, hasContent(core));
                push(out, p.text.substr(b), false);
            }
            return out;
        }
    }
}

#endif // PLUS_TRANSLATE_TEXTSPLIT_H
